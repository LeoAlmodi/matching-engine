#pragma once

// T-16, plan §10 (generator). Builds a benchmark workload before measurement: a pre-fill phase
// that gives the book depth on both sides, then the stream to measure, both in pre-allocated
// arrays of plain commands. Everything here may allocate; none of it runs while measuring.
//
// To make cancels and modifies target orders that are resting at that point of the stream, the
// generator drives its own Engine while it generates and tracks the resting ids from the events.
// The engine is deterministic (R-CMD-04), so replaying the same commands on a fresh engine with
// the same Config reproduces the same book, and every cancel or modify hits a resting order.

#include "me/config.hpp"
#include "me/detail/id_map.hpp"
#include "me/engine.hpp"
#include "me/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace me::bench {

// One command, as a flat struct so replay is a switch over a contiguous array.
struct Cmd {
  Command kind;
  Side side;      // NEW only
  OrderType type; // NEW only
  OrderId id;
  Price price; // NEW (LIMIT) and MODIFY
  Qty qty;     // NEW and MODIFY
  friend bool operator==(const Cmd &, const Cmd &) = default;
};

// Sends one command to anything with the Engine command API.
template <class E> inline void submit(E &engine, const Cmd &c) {
  switch (c.kind) {
  case Command::New:
    engine.new_order(c.id, c.side, c.type, c.price, c.qty);
    break;
  case Command::Cancel:
    engine.cancel(c.id);
    break;
  case Command::Modify:
    engine.modify(c.id, c.price, c.qty);
    break;
  }
}

// Percentages of each command type in the measured stream. They add up to 100.
struct Mix {
  unsigned limit, market, cancel, modify;
};

struct Preset {
  std::string name;
  Mix mix;
  std::size_t prefill;  // resting LIMIT orders added before the measured stream
  std::size_t commands; // length of the measured stream
  std::uint64_t seed;
};

inline constexpr Price kMaxPrice = 100'000;
inline constexpr Price kMid = kMaxPrice / 2;
inline constexpr Price kDepth = 50; // resting prices span mid +- kDepth
inline constexpr Price kCross = 5;  // how far past mid an incoming LIMIT may reach
inline constexpr Qty kMaxQty = 1'000'000;
inline constexpr Qty kMaxOrderQty = 100;

// Plan §10: the initial mix, and each command type alone. The single-type streams share a larger
// pre-fill, so cancels and market orders never run out of resting orders.
inline std::vector<Preset> presets() {
  return {
      {"mix", {60, 5, 30, 5}, 10'000, 1'000'000, 1},
      {"limit", {100, 0, 0, 0}, 250'000, 200'000, 2},
      {"market", {0, 100, 0, 0}, 250'000, 200'000, 3},
      {"cancel", {0, 0, 100, 0}, 250'000, 200'000, 4},
      {"modify", {0, 0, 0, 100}, 250'000, 200'000, 5},
  };
}

struct Workload {
  Config cfg;
  std::vector<Cmd> prefill;
  std::vector<Cmd> stream;
};

namespace detail {

// Deterministic, platform-independent random numbers (std distributions differ between standard
// libraries).
class Rng {
public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ += 0x9e3779b97f4a7c15ULL;
    return me::detail::splitmix64(state_);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
  std::int64_t between(std::int64_t lo, std::int64_t hi) { // inclusive
    return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo) + 1));
  }

private:
  std::uint64_t state_;
};

// Tracks which ids are resting from the events of the generator's engine.
class LiveSet {
public:
  void on_accepted(OrderId id) { add(id); } // removed below if it does not rest
  void on_rejected(OrderId, Command, RejectReason) {}
  void on_trade(OrderId aggressor, OrderId passive, Price, Qty, Qty aggressor_leaves,
                Qty passive_leaves) {
    if (aggressor_leaves == 0) {
      remove(aggressor);
    }
    if (passive_leaves == 0) {
      remove(passive);
    }
  }
  void on_canceled(OrderId id, Qty, CancelReason) { remove(id); }
  void on_modified(OrderId, Price, Qty) {}

  bool empty() const { return ids_.empty(); }
  OrderId pick(Rng &rng) const { return ids_[rng.below(ids_.size())]; }

private:
  void add(OrderId id) {
    if (pos_.emplace(id, ids_.size()).second) {
      ids_.push_back(id);
    }
  }
  void remove(OrderId id) {
    const auto it = pos_.find(id);
    if (it == pos_.end()) {
      return;
    }
    const std::size_t p = it->second;
    pos_.erase(it);
    if (p + 1 != ids_.size()) {
      ids_[p] = ids_.back();
      pos_[ids_[p]] = p;
    }
    ids_.pop_back();
  }

  std::vector<OrderId> ids_;
  std::unordered_map<OrderId, std::size_t> pos_;
};

inline Side random_side(Rng &rng) { return rng.below(2) == 0 ? Side::Buy : Side::Sell; }

// Pre-fill prices never cross: bids below mid, asks above.
inline Price passive_price(Rng &rng, Side side) {
  const Price off = rng.between(1, kDepth);
  return side == Side::Buy ? kMid - off : kMid + off;
}

// Stream prices: mostly passive, with a small range that reaches past mid and may cross.
inline Price stream_price(Rng &rng, Side side) {
  return side == Side::Buy ? rng.between(kMid - kDepth, kMid + kCross)
                           : rng.between(kMid - kCross, kMid + kDepth);
}

inline Qty order_qty(Rng &rng) { return rng.between(1, kMaxOrderQty); }

} // namespace detail

// Number of NEW commands of a preset, which is the number of orders accepted in its session.
inline std::size_t new_orders(const Workload &w) {
  auto is_new = [](const Cmd &c) { return c.kind == Command::New; };
  return static_cast<std::size_t>(std::count_if(w.prefill.begin(), w.prefill.end(), is_new) +
                                  std::count_if(w.stream.begin(), w.stream.end(), is_new));
}

// Throws std::logic_error if the preset asks for a cancel, modify or market order when the book
// cannot serve it (no resting orders, or no liquidity on either side).
inline Workload generate(const Preset &p) {
  const Mix &m = p.mix;
  if (m.limit + m.market + m.cancel + m.modify != 100) {
    throw std::invalid_argument("mix must add up to 100");
  }

  // Every NEW is accepted (all fields are valid and ids are unique), so the session needs room
  // for at most prefill + commands orders (R-VAL-07).
  const auto max_orders = static_cast<std::uint32_t>(p.prefill + p.commands);
  Workload w{Config{kMaxPrice, kMaxQty, max_orders}, {}, {}};
  w.prefill.reserve(p.prefill);
  w.stream.reserve(p.commands);

  detail::Rng rng(p.seed);
  detail::LiveSet live;
  auto engine = std::make_unique<Engine<detail::LiveSet>>(w.cfg, live);
  OrderId next_id = 1;

  auto emit = [&](std::vector<Cmd> &out, const Cmd &c) {
    out.push_back(c);
    submit(*engine, c);
  };

  for (std::size_t i = 0; i < p.prefill; ++i) {
    const Side s = detail::random_side(rng);
    emit(w.prefill, {Command::New, s, OrderType::Limit, next_id++, detail::passive_price(rng, s),
                     detail::order_qty(rng)});
  }

  for (std::size_t i = 0; i < p.commands; ++i) {
    const std::uint64_t r = rng.below(100);
    if (r < m.limit) {
      const Side s = detail::random_side(rng);
      emit(w.stream, {Command::New, s, OrderType::Limit, next_id++, detail::stream_price(rng, s),
                      detail::order_qty(rng)});
    } else if (r < m.limit + m.market) {
      // Pick a side with liquidity on the opposite side, so the order is not rejected (R-MKT-02).
      Side s = detail::random_side(rng);
      auto has_liquidity = [&](Side side) {
        return side == Side::Buy ? engine->best_ask().has_value() : engine->best_bid().has_value();
      };
      if (!has_liquidity(s)) {
        s = s == Side::Buy ? Side::Sell : Side::Buy;
        if (!has_liquidity(s)) {
          throw std::logic_error(p.name + ": book empty, cannot generate a market order");
        }
      }
      // The price of a MARKET order is ignored (R-VAL-03).
      emit(w.stream, {Command::New, s, OrderType::Market, next_id++, 0, detail::order_qty(rng)});
    } else {
      if (live.empty()) {
        throw std::logic_error(p.name + ": no resting orders to cancel or modify");
      }
      const OrderId id = live.pick(rng);
      if (r < m.limit + m.market + m.cancel) {
        emit(w.stream, {Command::Cancel, Side::Buy, OrderType::Limit, id, 0, 0});
      } else {
        // Half keep the price and do not increase quantity, so they keep priority (R-MOD-05); the
        // other half move to a new price and may trade (R-MOD-07).
        const auto o = *engine->order_state(id);
        const bool in_place = rng.below(2) == 0;
        const Price price = in_place ? o.price : detail::stream_price(rng, o.side);
        const Qty qty = in_place ? rng.between(1, o.leaves) : detail::order_qty(rng);
        emit(w.stream, {Command::Modify, o.side, OrderType::Limit, id, price, qty});
      }
    }
  }
  return w;
}

} // namespace me::bench
