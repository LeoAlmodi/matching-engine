// T-18, plan §10 (latency). Per-command latency of the planned mix: each command is timestamped
// with rdtsc into a pre-allocated array, converted to nanoseconds with a calibrated TSC frequency,
// then sorted to report p50, p99, p99.9 and max. A full warm-up pass on a separate engine runs
// first. Pin the process to one core: taskset -c 2 ./build/release/latency

#include "counting_sink.hpp"
#include "generator.hpp"
#include "me/engine.hpp"

#if !defined(__x86_64__)
#error "latency uses rdtsc and requires x86-64"
#endif
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using me::Engine;
using me::bench::CountingSink;
using me::bench::Workload;

// lfence keeps the timed instructions from being reordered across the timestamp.
inline std::uint64_t tsc() {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

// TSC ticks per nanosecond, measured against the steady clock.
double calibrate_tsc_ghz() {
  using clock = std::chrono::steady_clock;
  const auto c0 = clock::now();
  const std::uint64_t t0 = tsc();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const auto c1 = clock::now();
  const std::uint64_t t1 = tsc();
  const double ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
  return static_cast<double>(t1 - t0) / ns;
}

std::string cpu_model() {
  std::ifstream in("/proc/cpuinfo");
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("model name", 0) == 0) {
      const auto colon = line.find(':');
      return colon == std::string::npos ? line : line.substr(colon + 2);
    }
  }
  return "unknown";
}

// Builds a fresh engine and replays the pre-fill. Returns the engine ready for the stream.
std::unique_ptr<Engine<CountingSink>> prepared(const Workload &w, CountingSink &sink) {
  auto engine = std::make_unique<Engine<CountingSink>>(w.cfg, sink);
  for (const auto &c : w.prefill) {
    me::bench::submit(*engine, c);
  }
  return engine;
}

// Nearest-rank percentile of sorted samples.
std::uint64_t percentile(const std::vector<std::uint64_t> &sorted, double p) {
  auto rank = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size()) + 0.999999);
  rank = std::clamp<std::size_t>(rank, 1, sorted.size());
  return sorted[rank - 1];
}

} // namespace

int main() {
  const auto preset = me::bench::presets().front(); // the planned mix
  const Workload w = me::bench::generate(preset);
  std::vector<std::uint64_t> cycles(w.stream.size());

  // Warm-up: the whole stream on a separate engine, timed the same way.
  {
    CountingSink sink;
    auto engine = prepared(w, sink);
    for (std::size_t i = 0; i < w.stream.size(); ++i) {
      const std::uint64_t t0 = tsc();
      me::bench::submit(*engine, w.stream[i]);
      cycles[i] = tsc() - t0;
    }
  }

  CountingSink sink;
  auto engine = prepared(w, sink);
  for (std::size_t i = 0; i < w.stream.size(); ++i) {
    const std::uint64_t t0 = tsc();
    me::bench::submit(*engine, w.stream[i]);
    cycles[i] = tsc() - t0;
  }
  if (sink.rejected != 0) {
    std::fprintf(stderr, "unexpected rejections: %llu\n",
                 static_cast<unsigned long long>(sink.rejected));
    return 1;
  }

  // Cost of the two timestamps alone, for reference. Not subtracted from the results.
  std::vector<std::uint64_t> empty(100'000);
  for (auto &e : empty) {
    const std::uint64_t t0 = tsc();
    e = tsc() - t0;
  }
  std::sort(empty.begin(), empty.end());

  const double ghz = calibrate_tsc_ghz();
  std::sort(cycles.begin(), cycles.end());
  auto ns = [&](std::uint64_t c) { return static_cast<double>(c) / ghz; };

  std::printf("cpu:       %s\n", cpu_model().c_str());
  std::printf("tsc:       %.3f GHz\n", ghz);
  std::printf("workload:  %s, %zu pre-filled orders, %zu commands, %llu trades\n",
              preset.name.c_str(), w.prefill.size(), w.stream.size(),
              static_cast<unsigned long long>(sink.trades));
  std::printf("timer:     %.1f ns (median cost of the timestamps, included below)\n",
              ns(percentile(empty, 50)));
  std::printf("p50:       %.1f ns\n", ns(percentile(cycles, 50)));
  std::printf("p99:       %.1f ns\n", ns(percentile(cycles, 99)));
  std::printf("p99.9:     %.1f ns\n", ns(percentile(cycles, 99.9)));
  std::printf("max:       %.1f ns\n", ns(cycles.back()));
  return 0;
}
