#pragma once

#include "me/engine.hpp"
#include "support/invariants.hpp"
#include "support/ledger.hpp"
#include "support/recording_sink.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace me::test {

inline constexpr Config kDefaultConfig{.max_price = 1000, .max_qty = 500, .max_orders = 64};

// Engine, recording sink and ledger wired together (plan §9). Each command returns the events it
// emitted, feeds them to the ledger, and checks R-CMD-05 and every invariant afterwards.
struct Harness {
  explicit Harness(const Config &cfg = kDefaultConfig) : engine(cfg, sink) {}

  std::vector<Event> new_order(OrderId id, Side side, OrderType type, Price price, Qty qty) {
    const std::size_t from = sink.events.size();
    ledger.new_command(id, side, type, price, qty);
    engine.new_order(id, side, type, price, qty);
    return finish(from);
  }

  std::vector<Event> buy(OrderId id, Price price, Qty qty) {
    return new_order(id, Side::Buy, OrderType::Limit, price, qty);
  }
  std::vector<Event> sell(OrderId id, Price price, Qty qty) {
    return new_order(id, Side::Sell, OrderType::Limit, price, qty);
  }

  std::vector<Event> cancel(OrderId id) {
    const std::size_t from = sink.events.size();
    engine.cancel(id);
    return finish(from);
  }

  std::vector<Event> modify(OrderId id, Price price, Qty qty) {
    const std::size_t from = sink.events.size();
    engine.modify(id, price, qty);
    return finish(from);
  }

  // FIFO order of the ids resting at one level.
  std::vector<OrderId> ids_at(Side side, Price price) const {
    std::vector<OrderId> ids;
    engine.visit_level(side, price, [&](OrderId id, Qty) { ids.push_back(id); });
    return ids;
  }

  RecordingSink sink;
  Ledger ledger;
  Engine<RecordingSink> engine;

private:
  std::vector<Event> finish(std::size_t from) {
    std::vector<Event> emitted(sink.events.begin() + static_cast<std::ptrdiff_t>(from),
                               sink.events.end());
    for (const Event &e : emitted) {
      ledger.apply(e);
    }
    EXPECT_FALSE(emitted.empty()) << "R-CMD-05: every command produces at least one event";
    EXPECT_TRUE(check_invariants(engine, ledger));
    ledger.clear_touched();
    return emitted;
  }
};

} // namespace me::test
