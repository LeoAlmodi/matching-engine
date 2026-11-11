// T-16: benchmark command generator (plan §10). Checks, for every benchmark preset, that the
// measured stream has the planned mix and that replaying the workload on a fresh engine produces
// no rejections at all: in particular no UNKNOWN_ORDER for cancels (R-CXL-02) or modifies
// (R-MOD-01), so the benchmark measures the intended paths. Replay relies on determinism
// (R-CMD-04).

#include "generator.hpp"
#include "me/engine.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>

namespace {

using me::Command;
using me::Engine;
using me::OrderType;
using me::bench::Cmd;
using me::bench::Preset;
using me::bench::Workload;

struct RejectCounter {
  std::array<std::uint64_t, 6> by_reason{}; // indexed by RejectReason
  std::uint64_t total = 0;
  void on_accepted(me::OrderId) {}
  void on_rejected(me::OrderId, Command, me::RejectReason r) {
    ++by_reason[static_cast<std::size_t>(r)];
    ++total;
  }
  void on_trade(me::OrderId, me::OrderId, me::Price, me::Qty, me::Qty, me::Qty) {}
  void on_canceled(me::OrderId, me::Qty, me::CancelReason) {}
  void on_modified(me::OrderId, me::Price, me::Qty) {}
};

class BenchGenerator : public testing::TestWithParam<Preset> {};

TEST_P(BenchGenerator, PlannedMixAndNoRejectionsOnReplay) {
  const Preset &p = GetParam();
  const Workload w = me::bench::generate(p);

  ASSERT_EQ(w.prefill.size(), p.prefill);
  ASSERT_EQ(w.stream.size(), p.commands);
  for (const Cmd &c : w.prefill) {
    ASSERT_EQ(c.kind, Command::New);
    ASSERT_EQ(c.type, OrderType::Limit);
  }

  // Mix proportions in the measured stream, within one percentage point.
  std::array<std::uint64_t, 4> count{}; // limit, market, cancel, modify
  for (const Cmd &c : w.stream) {
    if (c.kind == Command::New) {
      ++count[c.type == OrderType::Limit ? 0 : 1];
    } else {
      ++count[c.kind == Command::Cancel ? 2 : 3];
    }
  }
  const std::array<unsigned, 4> planned{p.mix.limit, p.mix.market, p.mix.cancel, p.mix.modify};
  for (std::size_t k = 0; k < 4; ++k) {
    const double pct = 100.0 * static_cast<double>(count[k]) / static_cast<double>(p.commands);
    EXPECT_NEAR(pct, planned[k], 1.0) << "command type " << k;
  }

  // Replay on a fresh engine, as the benchmark does.
  RejectCounter sink;
  auto engine = std::make_unique<Engine<RejectCounter>>(w.cfg, sink);
  for (const Cmd &c : w.prefill) {
    me::bench::submit(*engine, c);
  }
  ASSERT_EQ(sink.total, 0u) << "pre-fill rejected";
  for (const Cmd &c : w.stream) {
    me::bench::submit(*engine, c);
  }
  EXPECT_EQ(sink.by_reason[static_cast<std::size_t>(me::RejectReason::UnknownOrder)], 0u);
  EXPECT_EQ(sink.total, 0u);
}

// Same preset, same commands.
TEST(BenchGenerator, Deterministic) {
  const Preset p = me::bench::presets().front();
  const Preset small{p.name, p.mix, 1000, 10'000, p.seed};
  const Workload a = me::bench::generate(small), b = me::bench::generate(small);
  ASSERT_EQ(a.stream.size(), b.stream.size());
  for (std::size_t i = 0; i < a.stream.size(); ++i) {
    ASSERT_EQ(a.stream[i], b.stream[i]) << "command " << i;
  }
}

INSTANTIATE_TEST_SUITE_P(Presets, BenchGenerator, testing::ValuesIn(me::bench::presets()),
                         [](const testing::TestParamInfo<Preset> &info) {
                           return info.param.name;
                         });

} // namespace
