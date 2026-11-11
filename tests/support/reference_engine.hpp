#pragma once

#include "me/config.hpp"
#include "me/events.hpp"
#include "me/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace me::test {

// T-15, plan §9: a deliberately naive implementation of the spec, written from spec.md alone and
// sharing no code with Engine. All resting orders live in one vector; the best order is found by a
// linear scan. Same command API and event sink as Engine, so both can be driven by the same
// commands and their event streams compared.
template <EventSink Sink> class ReferenceEngine {
public:
  ReferenceEngine(const Config &cfg, Sink &sink) : cfg_(cfg), sink_(sink) {}

  void new_order(OrderId id, Side side, OrderType type, Price price, Qty qty) {
    // R-VAL-05: quantity, price, duplicate id, capacity; then R-MKT-02.
    if (qty < 1 || qty > cfg_.max_qty) {
      return sink_.on_rejected(id, Command::New, RejectReason::InvalidQuantity);
    }
    if (type == OrderType::Limit && (price < 1 || price > cfg_.max_price)) {
      return sink_.on_rejected(id, Command::New, RejectReason::InvalidPrice);
    }
    if (used_ids_.count(id) != 0) {
      return sink_.on_rejected(id, Command::New, RejectReason::DuplicateOrderId);
    }
    if (accepted_ >= cfg_.max_orders) {
      return sink_.on_rejected(id, Command::New, RejectReason::CapacityExceeded);
    }
    const Side other = side == Side::Buy ? Side::Sell : Side::Buy;
    if (type == OrderType::Market && !best_index(other)) {
      return sink_.on_rejected(id, Command::New, RejectReason::NoLiquidity);
    }

    used_ids_.insert(id);
    ++accepted_;
    sink_.on_accepted(id);

    const std::optional<Price> limit =
        type == OrderType::Limit ? std::optional<Price>(price) : std::nullopt;
    const Qty leaves = trade(id, side, limit, qty);
    if (leaves == 0) {
      return;
    }
    if (type == OrderType::Market) {
      sink_.on_canceled(id, leaves, CancelReason::MarketRemainder); // R-MKT-03
      return;
    }
    book_.push_back(Order{id, side, price, leaves, next_seq_++}); // R-BOOK-01
  }

  void cancel(OrderId id) {
    const auto it = find(id);
    if (it == book_.end()) {
      return sink_.on_rejected(id, Command::Cancel, RejectReason::UnknownOrder); // R-CXL-02
    }
    const Qty leaves = it->leaves;
    book_.erase(it);
    sink_.on_canceled(id, leaves, CancelReason::User); // R-CXL-01
  }

  void modify(OrderId id, Price new_price, Qty new_qty) {
    // R-MOD-04: unknown order, quantity, price.
    const auto it = find(id);
    if (it == book_.end()) {
      return sink_.on_rejected(id, Command::Modify, RejectReason::UnknownOrder);
    }
    if (new_qty < 1 || new_qty > cfg_.max_qty) {
      return sink_.on_rejected(id, Command::Modify, RejectReason::InvalidQuantity);
    }
    if (new_price < 1 || new_price > cfg_.max_price) {
      return sink_.on_rejected(id, Command::Modify, RejectReason::InvalidPrice);
    }
    sink_.on_modified(id, new_price, new_qty); // R-EVT-04

    // R-MOD-05: same price and not larger keeps the arrival sequence.
    if (new_price == it->price && new_qty <= it->leaves) {
      it->leaves = new_qty;
      return;
    }
    // Otherwise the order is taken out, may trade (R-MOD-07), and rests as a new arrival.
    const Side side = it->side;
    book_.erase(it);
    const Qty leaves = trade(id, side, new_price, new_qty);
    if (leaves > 0) {
      book_.push_back(Order{id, side, new_price, leaves, next_seq_++});
    }
  }

  // R-QRY-01, as (price, total) or nothing.
  struct Best {
    Price price;
    Qty total;
    friend bool operator==(const Best &, const Best &) = default;
  };
  std::optional<Best> best(Side side) const {
    const auto i = best_index(side);
    if (!i) {
      return std::nullopt;
    }
    return Best{book_[*i].price, level_qty(side, book_[*i].price)};
  }

  // R-QRY-02
  Qty level_qty(Side side, Price price) const {
    Qty total = 0;
    for (const Order &o : book_) {
      if (o.side == side && o.price == price) {
        total += o.leaves;
      }
    }
    return total;
  }

  std::size_t resting() const { return book_.size(); }

private:
  struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty leaves;
    std::uint64_t seq; // arrival sequence, R-BOOK-03
  };

  // R-MATCH-01 to R-MATCH-06 (and R-MKT-01 when `limit` is empty). Returns the remaining leaves.
  Qty trade(OrderId aggressor, Side side, std::optional<Price> limit, Qty leaves) {
    const Side other = side == Side::Buy ? Side::Sell : Side::Buy;
    while (leaves > 0) {
      const auto i = best_index(other);
      if (!i) {
        break;
      }
      Order &passive = book_[*i];
      if (limit && (side == Side::Buy ? passive.price > *limit : passive.price < *limit)) {
        break;
      }
      const Qty q = std::min(leaves, passive.leaves);
      leaves -= q;
      passive.leaves -= q;
      sink_.on_trade(aggressor, passive.id, passive.price, q, leaves, passive.leaves);
      if (passive.leaves == 0) {
        book_.erase(book_.begin() + static_cast<std::ptrdiff_t>(*i));
      }
    }
    return leaves;
  }

  // R-BOOK-02: best price first (highest BUY, lowest SELL), then earliest arrival.
  std::optional<std::size_t> best_index(Side side) const {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < book_.size(); ++i) {
      const Order &o = book_[i];
      if (o.side != side) {
        continue;
      }
      if (!best) {
        best = i;
        continue;
      }
      const Order &b = book_[*best];
      const bool better_price = side == Side::Buy ? o.price > b.price : o.price < b.price;
      if (better_price || (o.price == b.price && o.seq < b.seq)) {
        best = i;
      }
    }
    return best;
  }

  typename std::vector<Order>::iterator find(OrderId id) {
    return std::find_if(book_.begin(), book_.end(), [id](const Order &o) { return o.id == id; });
  }

  Config cfg_;
  Sink &sink_;
  std::vector<Order> book_;
  std::unordered_set<OrderId> used_ids_; // every accepted id, R-VAL-04
  std::uint32_t accepted_ = 0;
  std::uint64_t next_seq_ = 0;
};

} // namespace me::test
