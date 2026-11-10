#pragma once

#include "me/config.hpp"
#include "me/detail/book_side.hpp"
#include "me/detail/id_map.hpp"
#include "me/detail/order_pool.hpp"
#include "me/events.hpp"
#include "me/types.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace me {

// Plan §7. Single-instrument limit order book. Reports events by calling `Sink` directly.
template <EventSink Sink> class Engine {
public:
  struct LevelView {
    Price price;
    Qty total;
    friend bool operator==(const LevelView &, const LevelView &) = default;
  };
  struct OrderView {
    Side side;
    Price price;
    Qty leaves;
    friend bool operator==(const OrderView &, const OrderView &) = default;
  };

  // Throws std::invalid_argument if `cfg` is invalid (R-CFG-01 to R-CFG-03, plan §4). All memory
  // is allocated here.
  Engine(const Config &cfg, Sink &sink)
      : cfg_(validated(cfg)), sink_(sink), pool_(cfg.max_orders), ids_(cfg.max_orders),
        bids_(Side::Buy, cfg.max_price, pool_), asks_(Side::Sell, cfg.max_price, pool_) {}

  // The books hold a reference to the pool, so the engine cannot be copied or moved.
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  // R-CMD-01. Plan §8.1.
  void new_order(OrderId id, Side side, OrderType type, Price price, Qty qty) {
    // Step 1: validate in the order of R-VAL-05. A rejection changes no state (R-VAL-06).
    if (!valid_qty(qty)) {
      return reject_new(id, RejectReason::InvalidQuantity); // R-VAL-01
    }
    if (type == OrderType::Limit && !valid_price(price)) {
      return reject_new(id, RejectReason::InvalidPrice); // R-VAL-02, R-VAL-03
    }
    if (ids_.find(id) != detail::IdMap::EMPTY) {
      return reject_new(id, RejectReason::DuplicateOrderId); // R-VAL-04
    }
    if (accepted_ >= cfg_.max_orders) {
      return reject_new(id, RejectReason::CapacityExceeded); // R-VAL-07
    }

    const bool is_market = type == OrderType::Market;
    // Step 2: a MARKET order needs liquidity on the opposite side (R-MKT-02).
    if (is_market && book(opposite(side)).empty()) {
      return reject_new(id, RejectReason::NoLiquidity);
    }

    // Step 3: accept (R-EVT-03).
    ++accepted_;
    sink_.on_accepted(id);

    // Step 4: match against the opposite side (R-MATCH-01, R-MATCH-02). A MARKET order uses the
    // worst possible limit, so every resting price is matchable (R-MKT-01) and its price field is
    // never read (R-VAL-03).
    const Price limit = !is_market ? price : side == Side::Buy ? cfg_.max_price : Price{1};
    const Qty leaves = match(id, side, limit, qty);
    if (leaves == 0) {
      ids_.insert(id, detail::IdMap::NOT_RESTING); // fully executed, id stays used (R-VAL-04)
      return;
    }

    // Step 5: a MARKET remainder is canceled and never rests (R-MKT-03, R-BOOK-04).
    if (is_market) {
      sink_.on_canceled(id, leaves, CancelReason::MarketRemainder);
      ids_.insert(id, detail::IdMap::NOT_RESTING);
      return;
    }

    // Step 5: the LIMIT remainder rests with its leaves (R-BOOK-01), at the tail of its level,
    // which gives it the latest arrival sequence (R-BOOK-03).
    rest(id, side, price, leaves);
  }

  // R-CMD-01. Plan §8.3.
  void cancel(OrderId id) {
    const std::uint32_t v = ids_.find(id);
    if (!is_resting(v)) {
      // R-CXL-02: never accepted, filled or already canceled. No state changes.
      return sink_.on_rejected(id, Command::Cancel, RejectReason::UnknownOrder);
    }
    // R-CXL-01
    const detail::OrderNode &n = pool_[v];
    book(n.side).unlink(v);
    sink_.on_canceled(id, n.leaves, CancelReason::User);
    ids_.update(id, detail::IdMap::NOT_RESTING); // the id stays used (R-VAL-04)
    pool_.free(v);
  }

  // R-CMD-01. Plan §8.4.
  void modify(OrderId id, Price new_price, Qty new_qty) {
    // Step 1: validate in the order of R-MOD-04. A rejection changes no state.
    const std::uint32_t v = ids_.find(id);
    if (!is_resting(v)) {
      return reject_modify(id, RejectReason::UnknownOrder); // R-MOD-01
    }
    if (!valid_qty(new_qty)) {
      return reject_modify(id, RejectReason::InvalidQuantity); // R-MOD-02
    }
    if (!valid_price(new_price)) {
      return reject_modify(id, RejectReason::InvalidPrice); // R-MOD-03
    }

    // Step 2: R-EVT-04, MODIFIED comes before any trade.
    sink_.on_modified(id, new_price, new_qty);

    detail::OrderNode &n = pool_[v];
    detail::BookSide &own = book(n.side);

    // Step 3: same price and not larger keeps priority (R-MOD-05, R-MOD-08). The decrease is
    // applied in place.
    if (new_price == n.price && new_qty <= n.leaves) {
      own.reduce(v, n.leaves - new_qty);
      return;
    }

    // Step 4: otherwise the order loses priority (R-MOD-05). It leaves its level, may trade as an
    // aggressor at the new price (R-MOD-07), and any remainder rests at the tail of its new level.
    own.unlink(v);
    const Qty leaves = match(id, n.side, new_price, new_qty);
    if (leaves == 0) {
      ids_.update(id, detail::IdMap::NOT_RESTING);
      pool_.free(v);
      return;
    }
    n.price = new_price;
    n.leaves = leaves;
    own.append(v);
  }

  // R-QRY-01
  std::optional<LevelView> best_bid() const { return best(bids_); }
  std::optional<LevelView> best_ask() const { return best(asks_); }

  // R-QRY-02: 0 if no level exists, including prices outside 1..MAX_PRICE.
  Qty level_qty(Side side, Price price) const {
    return valid_price(price) ? book(side).level(price).total : 0;
  }

  // R-QRY-03: nullopt if the order is not resting.
  std::optional<OrderView> order_state(OrderId id) const {
    const std::uint32_t v = ids_.find(id);
    if (!is_resting(v)) {
      return std::nullopt;
    }
    const detail::OrderNode &n = pool_[v];
    return OrderView{n.side, n.price, n.leaves};
  }

  // Test support only (plan §7): calls f(id, leaves) for each resting order of one level, in FIFO
  // order. Not used on the processing path.
  template <class F> void visit_level(Side side, Price price, F &&f) const {
    if (!valid_price(price)) {
      return;
    }
    for (Index i = book(side).level(price).head; i != NIL; i = pool_[i].next) {
      f(pool_[i].id, pool_[i].leaves);
    }
  }

  const Config &config() const { return cfg_; }

private:
  // Plan §8.2. Matches an aggressor with `leaves` against the opposite side while the best
  // opposite price is matchable against `limit` (R-MATCH-01). Returns the aggressor's remaining
  // leaves.
  Qty match(OrderId aggressor, Side side, Price limit, Qty leaves) {
    detail::BookSide &opp = book(opposite(side));
    while (leaves > 0 && !opp.empty()) {
      const Price best = opp.best();
      if (side == Side::Buy ? best > limit : best < limit) {
        break;
      }
      // R-MATCH-02: highest-priority passive order.
      const Index pi = opp.front();
      detail::OrderNode &passive = pool_[pi];
      const Qty q = leaves < passive.leaves ? leaves : passive.leaves; // R-MATCH-04
      leaves -= q;
      opp.reduce(pi, q); // R-MATCH-05
      // R-MATCH-03, R-MATCH-06
      sink_.on_trade(aggressor, passive.id, passive.price, q, leaves, passive.leaves);
      if (passive.leaves == 0) {
        // R-MATCH-05: a fully executed passive order leaves the book.
        opp.unlink(pi);
        ids_.update(passive.id, detail::IdMap::NOT_RESTING);
        pool_.free(pi);
      }
    }
    return leaves;
  }

  // Appends a new resting order and records its pool index under its id.
  void rest(OrderId id, Side side, Price price, Qty leaves) {
    const Index idx = pool_.alloc();
    assert(idx != NIL);
    detail::OrderNode &n = pool_[idx];
    n.id = id;
    n.side = side;
    n.price = price;
    n.leaves = leaves;
    ids_.insert(id, idx);
    book(side).append(idx);
  }

  static const Config &validated(const Config &cfg) {
    if (cfg.max_price < 1) {
      throw std::invalid_argument("max_price must be >= 1 (R-CFG-01)");
    }
    if (cfg.max_qty < 1) {
      throw std::invalid_argument("max_qty must be >= 1 (R-CFG-02)");
    }
    if (cfg.max_orders < 1) {
      throw std::invalid_argument("max_orders must be >= 1 (R-CFG-03)");
    }
    if (cfg.max_orders >= NIL - 1) {
      throw std::invalid_argument("max_orders must be < NIL - 1 (plan §4)");
    }
    return cfg;
  }

  bool valid_price(Price price) const { return price >= 1 && price <= cfg_.max_price; }
  bool valid_qty(Qty qty) const { return qty >= 1 && qty <= cfg_.max_qty; }

  static Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

  // An IdMap value is a pool index only for resting orders.
  static bool is_resting(std::uint32_t v) {
    return v != detail::IdMap::EMPTY && v != detail::IdMap::NOT_RESTING;
  }

  void reject_new(OrderId id, RejectReason reason) { sink_.on_rejected(id, Command::New, reason); }
  void reject_modify(OrderId id, RejectReason reason) {
    sink_.on_rejected(id, Command::Modify, reason);
  }

  const detail::BookSide &book(Side side) const { return side == Side::Buy ? bids_ : asks_; }
  detail::BookSide &book(Side side) { return side == Side::Buy ? bids_ : asks_; }

  static std::optional<LevelView> best(const detail::BookSide &side) {
    if (side.empty()) {
      return std::nullopt;
    }
    return LevelView{side.best(), side.level(side.best()).total};
  }

  Config cfg_;
  Sink &sink_;
  detail::OrderPool pool_;
  detail::IdMap ids_;
  detail::BookSide bids_;
  detail::BookSide asks_;
  std::uint32_t accepted_ = 0; // orders accepted in this session, R-VAL-07
};

} // namespace me
