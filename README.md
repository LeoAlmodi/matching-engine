# Matching Engine

Limit order book matching engine for a single instrument, in C++20.

## Benchmark baseline

Release build (`-O3 -march=native`), pinned to one core with `taskset`. Intel Core i7-1165G7, Linux 7.0, GCC 13.3. Details and targets in [`specs/plan.md`](specs/plan.md) §10.

| Workload | Throughput |
|---|---|
| mix (60% limit, 5% market, 30% cancel, 5% modify) | 13.4 M cmd/s |
| limit only | 14.0 M cmd/s |
| market only | 6.3 M cmd/s |
| cancel only | 12.2 M cmd/s |
| modify only | 9.7 M cmd/s |

| Per-command latency, mix | p50 | p99 | p99.9 |
|---|---|---|---|
| ns | 220 | 510 | 730 |

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
taskset -c 2 ./build/release/bench
taskset -c 2 ./build/release/latency
```
