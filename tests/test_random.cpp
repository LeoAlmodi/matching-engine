// T-13: randomized invariant test. Covers R-INV-01 to R-INV-06 and R-CMD-04 (plus R-CMD-05 on
// every command). Millions of generated commands, valid and invalid, with the ledger and
// check_invariants after each one. On failure, the seed and command index are printed so the run
// can be reproduced.

#include "me/engine.hpp"
#include "support/command_gen.hpp"
#include "support/invariants.hpp"
#include "support/ledger.hpp"
#include "support/recording_sink.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <variant>

namespace {

using me::Config;
using me::Engine;
using me::RejectReason;
using me::test::Cmd;
using me::test::CommandGen;
using me::test::Event;
using me::test::Ledger;
using me::test::NewCmd;
using me::test::RecordingSink;

// Small price range: the invariant checker scans every level after every command.
constexpr Config kRandomConfig{.max_price = 64, .max_qty = 1000, .max_orders = 2'000'000};

// How often each kind of outcome happened, to show the generator reaches every path.
struct Stats {
  std::uint64_t accepted = 0, trades = 0, user_cancels = 0, market_remainders = 0, modified = 0;
  std::array<std::uint64_t, 6> rejects{}; // indexed by RejectReason

  void count(const Event &e) {
    if (std::holds_alternative<me::Accepted>(e)) {
      ++accepted;
    } else if (std::holds_alternative<me::Trade>(e)) {
      ++trades;
    } else if (const auto *c = std::get_if<me::Canceled>(&e)) {
      ++(c->reason == me::CancelReason::User ? user_cancels : market_remainders);
    } else if (std::holds_alternative<me::Modified>(e)) {
      ++modified;
    } else {
      ++rejects[static_cast<std::size_t>(std::get<me::Rejected>(e).reason)];
    }
  }
};

// Runs `n` commands from `seed` through two engines with the same configuration. Engine `a` is
// checked against the ledger and the invariants after every command (R-INV-01 to R-INV-06);
// engine `b` must emit exactly the same events (R-CMD-04). Stops at the first failure.
Stats run(std::uint64_t seed, const Config &cfg, std::uint64_t n) {
  RecordingSink sink_a, sink_b;
  Engine<RecordingSink> a(cfg, sink_a), b(cfg, sink_b);
  Ledger ledger;
  CommandGen gen(seed, cfg);
  Stats stats;

  for (std::uint64_t i = 0; i < n; ++i) {
    const Cmd cmd = gen.next();
    if (const auto *nc = std::get_if<NewCmd>(&cmd)) {
      ledger.new_command(nc->id, nc->side, nc->type, nc->price, nc->qty);
    }
    me::test::submit(a, cmd);
    me::test::submit(b, cmd);

    const auto where = [&] {
      return ::testing::Message() << "seed " << seed << ", command #" << i << ": " << cmd << "\n";
    };
    if (sink_a.events.empty()) {
      ADD_FAILURE() << where() << "R-CMD-05: no event";
      return stats;
    }
    if (sink_a.events != sink_b.events) {
      ADD_FAILURE() << where() << "R-CMD-04: two engines emitted different events";
      return stats;
    }
    for (const Event &e : sink_a.events) {
      ledger.apply(e);
      gen.observe(e);
      stats.count(e);
    }
    if (const auto ok = me::test::check_invariants(a, ledger); !ok) {
      ADD_FAILURE() << where() << ok.message();
      return stats;
    }
    ledger.clear_touched();
    sink_a.events.clear();
    sink_b.events.clear();
  }
  return stats;
}

void expect_every_path(const Stats &s) {
  EXPECT_GT(s.accepted, 0u);
  EXPECT_GT(s.trades, 0u);
  EXPECT_GT(s.user_cancels, 0u);
  EXPECT_GT(s.market_remainders, 0u);
  EXPECT_GT(s.modified, 0u);
  for (RejectReason r :
       {RejectReason::InvalidQuantity, RejectReason::InvalidPrice, RejectReason::DuplicateOrderId,
        RejectReason::NoLiquidity, RejectReason::UnknownOrder}) {
    EXPECT_GT(s.rejects[static_cast<std::size_t>(r)], 0u) << "reason " << static_cast<int>(r);
  }
}

// R-INV-01 to R-INV-06, R-CMD-04: one million commands per seed.
class RandomSeeds : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(RandomSeeds, R_INV_InvariantsHoldForOneMillionCommands) {
  expect_every_path(run(GetParam(), kRandomConfig, 1'000'000));
}

INSTANTIATE_TEST_SUITE_P(Random, RandomSeeds, ::testing::Values(1, 42, 20261110));

// R-VAL-07 under random load: a small MAX_ORDERS is reached early, after which NEW is rejected
// while cancels and modifies go on.
TEST(Random, R_INV_InvariantsHoldAtCapacity) {
  const Config cfg{.max_price = 64, .max_qty = 1000, .max_orders = 500};
  const Stats s = run(7, cfg, 200'000);
  EXPECT_GT(s.rejects[static_cast<std::size_t>(RejectReason::CapacityExceeded)], 0u);
  EXPECT_GT(s.user_cancels, 0u);
  EXPECT_GT(s.modified, 0u);
}

// Smallest valid book: one price, one unit, a handful of orders.
TEST(Random, R_INV_InvariantsHoldOnTinyConfig) {
  const Config cfg{.max_price = 1, .max_qty = 1, .max_orders = 50};
  run(11, cfg, 100'000);
}

} // namespace
