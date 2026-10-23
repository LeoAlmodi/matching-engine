#include <benchmark/benchmark.h>
static void BM_Noop(benchmark::State& s) { for (auto _ : s) benchmark::DoNotOptimize(0); }
BENCHMARK(BM_Noop);
