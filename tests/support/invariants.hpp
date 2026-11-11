#pragma once

#include "me/engine.hpp"
#include "support/ledger.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace me::test {

// Plan §9: verifies R-INV-01 to R-INV-06 on the engine's book, cross-checked against the ledger.
// Scans every price level, so it is meant for tests with a small MAX_PRICE.
template <class Sink>
::testing::AssertionResult check_invariants(const Engine<Sink> &engine, const Ledger &ledger) {
  std::vector<std::string> failures = ledger.errors();
  const auto fail = [&failures](const auto &...parts) {
    std::ostringstream s;
    (s << ... << parts);
    failures.push_back(s.str());
  };
  const Config &cfg = engine.config();
  std::size_t resting = 0;
  Price best_price[2] = {0, 0};

  for (Side side : {Side::Buy, Side::Sell}) {
    const char *name = side == Side::Buy ? "BUY" : "SELL";
    Price &best = best_price[side == Side::Buy ? 0 : 1];
    for (Price p = 1; p <= cfg.max_price; ++p) {
      Qty sum = 0;
      bool first = true;
      std::uint64_t prev_seq = 0;
      engine.visit_level(side, p, [&](OrderId id, Qty leaves) {
        ++resting;
        sum += leaves;
        // R-INV-04
        if (leaves < 1 || leaves > cfg.max_qty) {
          fail("R-INV-04: ", name, " order ", id, " at ", p, " has leaves ", leaves);
        }
        // R-QRY-03 must agree with the book.
        const auto state = engine.order_state(id);
        if (!state || state->side != side || state->price != p || state->leaves != leaves) {
          fail("order_state(", id, ") disagrees with the book at ", name, " ", p);
        }
        const auto it = ledger.orders().find(id);
        if (it == ledger.orders().end()) {
          fail("resting order ", id, " was never accepted");
          return;
        }
        const OrderRecord &r = it->second;
        // R-INV-05
        if (r.type != OrderType::Limit) {
          fail("R-INV-05: MARKET order ", id, " rests in the book");
        }
        if (r.side != side || r.price != p) {
          fail("order ", id, " rests at ", name, " ", p, " but the ledger has it elsewhere");
        }
        // R-INV-02
        if (r.leaves() != leaves) {
          fail("R-INV-02: order ", id, " has leaves ", leaves, ", ledger expects ", r.leaves());
        }
        // R-INV-03: FIFO order within the level is arrival-sequence order.
        if (!first && r.seq <= prev_seq) {
          fail("R-INV-03: order ", id, " at ", name, " ", p, " is out of arrival order");
        }
        first = false;
        prev_seq = r.seq;
      });
      // R-INV-06
      if (engine.level_qty(side, p) != sum) {
        fail("R-INV-06: ", name, " level ", p, " reports ", engine.level_qty(side, p),
             ", orders sum to ", sum);
      }
      if (sum > 0 && (best == 0 || (side == Side::Buy ? p > best : p < best))) {
        best = p;
      }
    }

    // R-QRY-01 must agree with the levels.
    const auto reported = side == Side::Buy ? engine.best_bid() : engine.best_ask();
    if (best == 0 && reported) {
      fail("best ", name, " reported at ", reported->price, " but the side is empty");
    }
    if (best != 0 &&
        (!reported || reported->price != best || reported->total != engine.level_qty(side, best))) {
      fail("best ", name, " should be ", best);
    }
  }

  // R-INV-01
  if (best_price[0] != 0 && best_price[1] != 0 && best_price[0] >= best_price[1]) {
    fail("R-INV-01: book is crossed, bid ", best_price[0], " ask ", best_price[1]);
  }

  // R-INV-02: every accepted order with leaves rests, and every other order does not. Each order
  // visited above was matched against a ledger order with the same leaves (>= 1), so the visited
  // orders are a subset of the ledger's orders with leaves; equal counts make the sets equal.
  // Negative leaves are reported by the ledger itself.
  // An order the last command filled or canceled must no longer be resting (R-QRY-03). Without
  // this, a stale id could point at a freed pool slot and corrupt the book later.
  for (OrderId id : ledger.touched()) {
    const auto it = ledger.orders().find(id);
    if (it != ledger.orders().end() && it->second.leaves() == 0 && engine.order_state(id)) {
      fail("R-INV-02: order ", id, " has no leaves but is still resting");
    }
  }
  if (resting != ledger.with_leaves()) {
    fail("R-INV-02: book holds ", resting, " orders, ledger expects ", ledger.with_leaves(),
         " with leaves");
  }

  if (failures.empty()) {
    return ::testing::AssertionSuccess();
  }
  auto result = ::testing::AssertionFailure();
  for (const auto &f : failures) {
    result << f << "\n";
  }
  return result;
}

} // namespace me::test
