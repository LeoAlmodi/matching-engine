// T-17, plan §10 (throughput). Commands per second for the planned mix and for each command type
// alone. Each iteration builds a fresh engine and replays the pre-fill with the timer paused, then
// times the measured stream only.

#include "counting_sink.hpp"
#include "generator.hpp"
#include "me/engine.hpp"

#include <benchmark/benchmark.h>

#include <memory>

namespace {

using me::Engine;
using me::bench::CountingSink;
using me::bench::Preset;
using me::bench::Workload;

void BM_Throughput(benchmark::State &state, const Preset &preset) {
  const Workload w = me::bench::generate(preset); // before any timing
  for (auto _ : state) {
    state.PauseTiming();
    CountingSink sink;
    auto engine = std::make_unique<Engine<CountingSink>>(w.cfg, sink);
    for (const auto &c : w.prefill) {
      me::bench::submit(*engine, c);
    }
    state.ResumeTiming();

    for (const auto &c : w.stream) {
      me::bench::submit(*engine, c);
    }
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();

    state.PauseTiming();
    if (sink.rejected != 0) {
      state.SkipWithError("unexpected rejections");
    }
    state.counters["trades/cmd"] =
        static_cast<double>(sink.trades) / static_cast<double>(w.stream.size());
    engine.reset(); // free the engine outside the timed region
    state.ResumeTiming();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(w.stream.size()));
}

[[maybe_unused]] const bool registered = [] {
  for (const Preset &p : me::bench::presets()) {
    benchmark::RegisterBenchmark("throughput/" + p.name, BM_Throughput, p)
        ->Unit(benchmark::kMillisecond);
  }
  return true;
}();

} // namespace

BENCHMARK_MAIN();
