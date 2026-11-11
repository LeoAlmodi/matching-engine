#pragma once

#include "me/config.hpp"
#include "me/detail/id_map.hpp"
#include "me/events.hpp"
#include "me/types.hpp"
#include "support/recording_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <unordered_map>
#include <variant>
#include <vector>

namespace me::test {

struct NewCmd {
  OrderId id;
  Side side;
  OrderType type;
  Price price;
  Qty qty;
};
struct CancelCmd {
  OrderId id;
};
struct ModifyCmd {
  OrderId id;
  Price price;
  Qty qty;
};
using Cmd = std::variant<NewCmd, CancelCmd, ModifyCmd>;

inline std::ostream &operator<<(std::ostream &os, const Cmd &cmd) {
  if (const auto *n = std::get_if<NewCmd>(&cmd)) {
    return os << "NEW(id=" << n->id << ", " << (n->side == Side::Buy ? "BUY" : "SELL") << ", "
              << (n->type == OrderType::Limit ? "LIMIT" : "MARKET") << ", price=" << n->price
              << ", qty=" << n->qty << ")";
  }
  if (const auto *c = std::get_if<CancelCmd>(&cmd)) {
    return os << "CANCEL(id=" << c->id << ")";
  }
  const auto &m = std::get<ModifyCmd>(cmd);
  return os << "MODIFY(id=" << m.id << ", price=" << m.price << ", qty=" << m.qty << ")";
}

// Sends one command to anything with the Engine command API.
template <class E> void submit(E &engine, const Cmd &cmd) {
  if (const auto *n = std::get_if<NewCmd>(&cmd)) {
    engine.new_order(n->id, n->side, n->type, n->price, n->qty);
  } else if (const auto *c = std::get_if<CancelCmd>(&cmd)) {
    engine.cancel(c->id);
  } else {
    const auto &m = std::get<ModifyCmd>(cmd);
    engine.modify(m.id, m.price, m.qty);
  }
}

// Deterministic, platform-independent random numbers (std distributions differ between standard
// libraries, and a printed seed must reproduce the same run everywhere).
class Rng {
public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ += 0x9e3779b97f4a7c15ULL;
    return detail::splitmix64(state_);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
  bool percent(unsigned p) { return below(100) < p; }
  std::int64_t between(std::int64_t lo, std::int64_t hi) { // inclusive
    return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo) + 1));
  }

private:
  std::uint64_t state_;
};

// Plan §9: fixed-seed generator of valid and invalid commands of every type, for the randomized
// invariant test (T-13) and the differential test (T-15). Prices cluster around the middle of the
// price range so orders cross often. About 3% of fields are invalid, and some ids are duplicates
// or unknown.
//
// Feed every emitted event back through observe(): the generator then knows which ids rest, and
// most cancels and modifies target them, which keeps the book at a steady size. The sequence of
// commands depends only on the seed and on the events, so it is deterministic.
class CommandGen {
public:
  CommandGen(std::uint64_t seed, const Config &cfg) : rng_(seed), cfg_(cfg) {}

  void observe(const Event &event) {
    if (const auto *a = std::get_if<Accepted>(&event)) {
      add_live(a->order_id); // removed below if it does not rest
    } else if (const auto *t = std::get_if<Trade>(&event)) {
      if (t->aggressor_leaves == 0) {
        remove_live(t->aggressor_id);
      }
      if (t->passive_leaves == 0) {
        remove_live(t->passive_id);
      }
    } else if (const auto *c = std::get_if<Canceled>(&event)) {
      remove_live(c->order_id);
    }
  }

  Cmd next() {
    const std::uint64_t r = rng_.below(100);
    if (r < 50) {
      const Side s = side();
      return NewCmd{new_id(), s, OrderType::Limit, price(s == Side::Buy ? -4 : 4), qty()};
    }
    if (r < 55) {
      // R-VAL-03: any price value, valid or not.
      return NewCmd{new_id(), side(), OrderType::Market, static_cast<Price>(rng_.next()), qty()};
    }
    if (r < 78) {
      return CancelCmd{target_id()};
    }
    return ModifyCmd{target_id(), price(0), qty()};
  }

private:
  Side side() { return rng_.percent(50) ? Side::Buy : Side::Sell; }

  OrderId new_id() {
    if (!issued_.empty() && rng_.percent(3)) {
      return recent_id(); // probably a duplicate (R-VAL-04)
    }
    if (rng_.percent(1)) {
      return rng_.percent(50) ? OrderId{0} : ~OrderId{0} - rng_.below(4); // extreme ids
    }
    const OrderId id = next_id_++;
    issued_.push_back(id);
    return id;
  }

  OrderId target_id() {
    const std::uint64_t r = rng_.below(100);
    if (r < 40 && !live_.empty()) {
      return live_[rng_.below(live_.size())]; // resting
    }
    if (r < 95 && !issued_.empty()) {
      return recent_id(); // may have been filled or canceled already
    }
    return rng_.next(); // almost surely never accepted
  }

  void add_live(OrderId id) {
    if (live_pos_.emplace(id, live_.size()).second) {
      live_.push_back(id);
    }
  }

  void remove_live(OrderId id) {
    const auto it = live_pos_.find(id);
    if (it == live_pos_.end()) {
      return;
    }
    const std::size_t pos = it->second;
    live_pos_.erase(it);
    if (pos + 1 != live_.size()) {
      live_[pos] = live_.back();
      live_pos_[live_[pos]] = pos;
    }
    live_.pop_back();
  }

  OrderId recent_id() {
    const std::size_t window = std::min<std::size_t>(issued_.size(), 256);
    return issued_[issued_.size() - 1 - rng_.below(window)];
  }

  // A valid price around `mid + offset`, or occasionally an invalid one. Buys center a little
  // below the middle and sells a little above, so the ranges overlap (orders cross often) but the
  // book keeps some depth on both sides.
  Price price(Price offset) {
    if (rng_.percent(3)) {
      constexpr Price kInvalid[] = {0, -1, -1000};
      const std::uint64_t k = rng_.below(4);
      return k < 3 ? kInvalid[k] : cfg_.max_price + 1;
    }
    const Price mid = (cfg_.max_price + 1) / 2;
    const Price spread = std::min<Price>(10, mid - 1);
    const Price center = mid + offset;
    return std::clamp<Price>(rng_.between(center - spread, center + spread), 1, cfg_.max_price);
  }

  Qty qty() {
    if (rng_.percent(3)) {
      constexpr Qty kInvalid[] = {0, -1, -1000};
      const std::uint64_t k = rng_.below(4);
      return k < 3 ? kInvalid[k] : cfg_.max_qty + 1;
    }
    if (rng_.percent(1)) {
      return cfg_.max_qty;
    }
    return rng_.between(1, std::min<Qty>(cfg_.max_qty, 50));
  }

  Rng rng_;
  Config cfg_;
  OrderId next_id_ = 1;
  std::vector<OrderId> issued_;
  std::vector<OrderId> live_; // ids resting after the last observed command
  std::unordered_map<OrderId, std::size_t> live_pos_;
};

} // namespace me::test
