// T-15: differential test against the naive reference engine (plan §9). Both engines receive the
// same generated commands; after each command their events, best bid and ask, and number of
// resting orders must be identical. This checks event fields the invariants do not constrain,
// such as trade prices (R-MATCH-03) and the order of trades (R-MATCH-02). On a mismatch the seed,
// command index, command and both event streams are printed.

#include "me/engine.hpp"
#include "support/command_gen.hpp"
#include "support/recording_sink.hpp"
#include "support/reference_engine.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {

using me::Config;
using me::Engine;
using me::Side;
using me::test::Cmd;
using me::test::CommandGen;
using me::test::Event;
using me::test::RecordingSink;
using me::test::ReferenceEngine;

struct Totals {
  std::uint64_t events = 0, trades = 0;
};

Totals run(std::uint64_t seed, const Config &cfg, std::uint64_t n) {
  RecordingSink sink, ref_sink;
  Engine<RecordingSink> engine(cfg, sink);
  ReferenceEngine<RecordingSink> ref(cfg, ref_sink);
  CommandGen gen(seed, cfg);
  Totals totals;

  // The reference engine's view of the best level, in the engine's terms.
  const auto ref_best = [&](Side side) -> std::optional<Engine<RecordingSink>::LevelView> {
    const auto b = ref.best(side);
    if (!b) {
      return std::nullopt;
    }
    return Engine<RecordingSink>::LevelView{b->price, b->total};
  };

  for (std::uint64_t i = 0; i < n; ++i) {
    const Cmd cmd = gen.next();
    me::test::submit(engine, cmd);
    me::test::submit(ref, cmd);

    const auto where = [&] {
      return ::testing::Message() << "seed " << seed << ", command #" << i << ": " << cmd << "\n";
    };
    if (sink.events != ref_sink.events) {
      ADD_FAILURE() << where() << "engine:    " << ::testing::PrintToString(sink.events) << "\n"
                    << "reference: " << ::testing::PrintToString(ref_sink.events);
      return totals;
    }
    if (engine.best_bid() != ref_best(Side::Buy) || engine.best_ask() != ref_best(Side::Sell)) {
      ADD_FAILURE() << where() << "best bid or ask differs from the reference";
      return totals;
    }
    // Counting resting orders scans every level, so it is done only for small price ranges.
    std::size_t resting = ref.resting();
    if (cfg.max_price <= 1024) {
      resting = 0;
      for (Side side : {Side::Buy, Side::Sell}) {
        for (me::Price p = 1; p <= cfg.max_price; ++p) {
          engine.visit_level(side, p, [&](me::OrderId, me::Qty) { ++resting; });
        }
      }
    }
    if (resting != ref.resting()) {
      ADD_FAILURE() << where() << "engine rests " << resting << " orders, reference "
                    << ref.resting();
      return totals;
    }

    for (const Event &e : sink.events) {
      gen.observe(e);
      totals.trades += std::holds_alternative<me::Trade>(e) ? 1 : 0;
    }
    totals.events += sink.events.size();
    sink.events.clear();
    ref_sink.events.clear();
  }
  return totals;
}

constexpr Config kConfig{.max_price = 64, .max_qty = 1000, .max_orders = 2'000'000};

class DifferentialSeeds : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(DifferentialSeeds, MatchesReferenceForOneMillionCommands) {
  const Totals t = run(GetParam(), kConfig, 1'000'000);
  EXPECT_GE(t.events, 1'000'000u);
  EXPECT_GT(t.trades, 0u);
}

INSTANTIATE_TEST_SUITE_P(Differential, DifferentialSeeds, ::testing::Values(3, 99, 123456));

TEST(Differential, MatchesReferenceAtCapacity) {
  run(5, Config{.max_price = 64, .max_qty = 1000, .max_orders = 500}, 200'000);
}

TEST(Differential, MatchesReferenceOnTinyConfig) {
  run(13, Config{.max_price = 1, .max_qty = 1, .max_orders = 50}, 100'000);
}

TEST(Differential, MatchesReferenceWithWidePrices) {
  run(17, Config{.max_price = 100'000, .max_qty = 1'000'000, .max_orders = 200'000}, 100'000);
}

} // namespace
