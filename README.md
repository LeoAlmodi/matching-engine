# Matching Engine

A limit order book matching engine for a single instrument, written in C++20.

It accepts limit and market orders, cancels and modifies, matches them with price-time priority (FIFO within each price level), supports partial executions and reports every outcome as an event. It runs on a single thread, has no dynamic allocation while it processes orders, and depends only on the standard library.

On an Intel Core i7-1165G7 laptop it processes **13.4 million commands per second** on a realistic mix. Per-command latency is **220 ns at p50, 510 ns at p99 and 730 ns at p99.9**.

## Spec-driven development

The project is built with Spec-Driven Development: the specification is written first and the code follows it, never the other way around.

| Document | Role |
|---|---|
| [`specs/spec.md`](specs/spec.md) | **What** the engine does: 52 requirements with ids (`R-<AREA>-<NN>`), invariants, edge cases and a decision log (`D-01` to `D-22`) that resolves every ambiguity. No implementation details. |
| [`specs/plan.md`](specs/plan.md) | **How** it is built: types, data structures, algorithms, event output, test strategy and benchmark method. |
| [`specs/tasks.md`](specs/tasks.md) | The plan broken into small tasks (`T-01` to `T-20`), each with the requirements it covers and an acceptance criterion. |

Each task follows the same loop:

1. Read the requirements and plan sections it covers.
2. Write the tests first. Every test names the requirement ids it verifies, for example `TEST(Match, R_MATCH_02_SweepsSeveralLevels)`.
3. Implement until the tests pass under ASan and UBSan with no warnings.
4. Review the code against the spec, not only against the tests.

New behavior starts with a spec change, then plan, tasks, tests and code, in that order. A script ([`tools/spec_coverage.py`](tools/spec_coverage.py)) fails CI if any requirement is not referenced by a test.

## Build and test

Requirements: a C++20 compiler (tested with GCC 13), CMake 3.24+, Ninja and Python 3. GoogleTest and Google Benchmark are downloaded by CMake.

```bash
# Debug build with AddressSanitizer and UndefinedBehaviorSanitizer: use it for correctness
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DME_SANITIZE=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure --parallel 4

# Check that every requirement is covered by a test
python3 tools/spec_coverage.py

# Release build: use it only for benchmarks
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
taskset -c 2 ./build/release/bench     # throughput
taskset -c 2 ./build/release/latency   # per-command latency
```

`taskset` pins the process to one core. The Release build uses `-O3 -march=native`; pass `-DME_NATIVE=OFF` to build without `-march=native`.

## Usage

```cpp
#include "me/engine.hpp"

#include <cstdio>

struct Printer {  // any type with these five methods is an event sink
  void on_accepted(me::OrderId id) { std::printf("accepted %llu\n", (unsigned long long)id); }
  void on_rejected(me::OrderId, me::Command, me::RejectReason) {}
  void on_trade(me::OrderId, me::OrderId, me::Price price, me::Qty qty, me::Qty, me::Qty) {
    std::printf("trade %lld @ %lld\n", (long long)qty, (long long)price);
  }
  void on_canceled(me::OrderId, me::Qty, me::CancelReason) {}
  void on_modified(me::OrderId, me::Price, me::Qty) {}
};

Printer sink;
me::Engine<Printer> engine({.max_price = 100'000, .max_qty = 1'000'000, .max_orders = 1'000'000}, sink);

engine.new_order(1, me::Side::Sell, me::OrderType::Limit, 101, 10);
engine.new_order(2, me::Side::Buy, me::OrderType::Limit, 101, 4);  // trades 4 @ 101
engine.modify(1, 102, 6);
engine.cancel(1);
auto ask = engine.best_ask();  // std::optional<{price, total}>
```

Prices and quantities are integers in ticks; there is no floating point anywhere.

## Design

| Component | Structure | Cost |
|---|---|---|
| `OrderPool` | One array of order nodes allocated up front, with an intrusive free list. Orders link by 32-bit index, not pointer. | O(1) alloc and free |
| `Level` | Intrusive doubly linked FIFO of orders at one price, with the total quantity. | O(1) append, remove head, unlink from middle |
| `BookSide` | Array of levels indexed by price, plus the best price and a count of non-empty levels. | O(1) best price; scan to the next level only when the best level empties |
| `IdMap` | Open-addressing hash map, linear probing, splitmix64 hash, load factor at most 0.5. Remembers every id accepted in the session, so duplicates are always rejected. | O(1) expected |
| `Engine<Sink>` | Template on the event sink: events are direct calls, inlinable, with no virtual dispatch and no buffering. | |

All memory is allocated in the constructor. Commands never throw: every failure becomes a `REJECTED` event. The engine is deterministic: the same commands always produce the same events.

## Testing

- **Unit tests per spec area** (validation, book, matching, market orders, cancel, modify). Each compares the exact sequence of events and runs an invariant checker at the end.
- **Invariant checker.** Verifies the six invariants (`R-INV-01` to `R-INV-06`): the book is never crossed, requested quantity equals executed plus leaves plus canceled for every order, FIFO execution within a level, valid price and leaves for every resting order, no resting market order, and level totals equal the sum of their orders.
- **Randomized test.** Millions of generated commands, valid and invalid, with the invariant checker after every command. A failure prints the seed and command index.
- **Differential test.** A naive reference engine (vectors and linear search) runs the same random commands; its event stream must be identical to the engine's.
- **Sanitizers.** Everything runs under ASan and UBSan in CI.

## Benchmark results

Release build, `-O3 -march=native`, pinned to one core. Intel Core i7-1165G7 (TSC 2.8 GHz, turbo up to 4.7 GHz, `powersave` governor), Linux 7.0, GCC 13.3. Throughput is the median of 5 repetitions; latency the median of 5 runs.

The commands are generated before measurement from a fixed seed, with prices concentrated around a mid price, and the book is pre-filled. Cancels and modifies always target resting orders, so the measured stream has no rejections.

| Workload | Throughput |
|---|---|
| mix (60% limit, 5% market, 30% cancel, 5% modify), 1M commands | 13.4 M cmd/s |
| limit only | 14.0 M cmd/s |
| market only (about 2 trades per command) | 6.3 M cmd/s |
| cancel only | 12.2 M cmd/s |
| modify only | 9.7 M cmd/s |

| Per-command latency, mix | p50 | p99 | p99.9 |
|---|---|---|---|
| ns | 220 | 510 | 730 |

Latency is measured with `rdtsc` around each command, fenced with `lfence`, and includes about 13 ns of timestamp cost. The fences stop consecutive commands from overlapping, so p50 is above the 75 ns average per command implied by throughput. Targets and method are in [`specs/plan.md`](specs/plan.md) §10.

## Requirement coverage

Every requirement in the spec and the test files that reference it. Generated with `python3 tools/spec_coverage.py --markdown`.

<!-- coverage:begin -->
| Requirement | Tests |
|---|---|
| R-CFG-01 | `test_book_side.cpp`, `test_engine_basic.cpp`, `test_types.cpp` |
| R-CFG-02 | `test_engine_basic.cpp` |
| R-CFG-03 | `test_engine_basic.cpp`, `test_types.cpp` |
| R-CMD-01 | `test_engine_basic.cpp` |
| R-CMD-02 | `test_engine_basic.cpp`, `test_types.cpp` |
| R-CMD-03 | `test_match.cpp` |
| R-CMD-04 | `test_bench_generator.cpp`, `test_random.cpp` |
| R-CMD-05 | `harness.hpp`, `test_random.cpp`, `test_val.cpp` |
| R-VAL-01 | `ledger.hpp`, `test_val.cpp` |
| R-VAL-02 | `test_val.cpp` |
| R-VAL-03 | `command_gen.hpp`, `test_mkt.cpp` |
| R-VAL-04 | `command_gen.hpp`, `reference_engine.hpp`, `test_cxl.cpp`, `test_id_map.cpp`, `test_match.cpp`, `test_mkt.cpp`, `test_val.cpp` |
| R-VAL-05 | `reference_engine.hpp`, `test_mkt.cpp`, `test_val.cpp` |
| R-VAL-06 | `test_val.cpp` |
| R-VAL-07 | `test_cxl.cpp`, `test_id_map.cpp`, `test_match.cpp`, `test_random.cpp`, `test_val.cpp` |
| R-BOOK-01 | `reference_engine.hpp`, `test_book.cpp`, `test_match.cpp` |
| R-BOOK-02 | `reference_engine.hpp`, `test_book.cpp`, `test_book_side.cpp` |
| R-BOOK-03 | `ledger.hpp`, `reference_engine.hpp`, `test_book.cpp`, `test_engine_basic.cpp` |
| R-BOOK-04 | `test_mkt.cpp` |
| R-MATCH-01 | `reference_engine.hpp`, `test_match.cpp` |
| R-MATCH-02 | `test_differential.cpp`, `test_match.cpp` |
| R-MATCH-03 | `test_differential.cpp`, `test_match.cpp` |
| R-MATCH-04 | `test_match.cpp` |
| R-MATCH-05 | `test_match.cpp` |
| R-MATCH-06 | `reference_engine.hpp`, `test_match.cpp` |
| R-MKT-01 | `reference_engine.hpp`, `test_mkt.cpp` |
| R-MKT-02 | `reference_engine.hpp`, `test_mkt.cpp` |
| R-MKT-03 | `reference_engine.hpp`, `test_mkt.cpp` |
| R-CXL-01 | `reference_engine.hpp`, `test_cxl.cpp` |
| R-CXL-02 | `reference_engine.hpp`, `test_bench_generator.cpp`, `test_cxl.cpp`, `test_id_map.cpp` |
| R-MOD-01 | `test_bench_generator.cpp`, `test_id_map.cpp`, `test_mod.cpp` |
| R-MOD-02 | `test_mod.cpp` |
| R-MOD-03 | `test_mod.cpp` |
| R-MOD-04 | `reference_engine.hpp`, `test_mod.cpp` |
| R-MOD-05 | `ledger.hpp`, `reference_engine.hpp`, `test_book_side.cpp`, `test_engine_basic.cpp`, `test_mod.cpp` |
| R-MOD-06 | `ledger.hpp`, `test_engine_basic.cpp`, `test_mod.cpp` |
| R-MOD-07 | `reference_engine.hpp`, `test_mod.cpp` |
| R-MOD-08 | `test_engine_basic.cpp`, `test_mod.cpp` |
| R-EVT-01 | `test_types.cpp` |
| R-EVT-02 | `test_types.cpp`, `test_val.cpp` |
| R-EVT-03 | `test_book.cpp`, `test_match.cpp` |
| R-EVT-04 | `reference_engine.hpp`, `test_mod.cpp` |
| R-EVT-05 | `test_match.cpp` |
| R-QRY-01 | `invariants.hpp`, `reference_engine.hpp`, `test_book_side.cpp`, `test_engine_basic.cpp` |
| R-QRY-02 | `reference_engine.hpp`, `test_book_side.cpp`, `test_engine_basic.cpp` |
| R-QRY-03 | `invariants.hpp`, `ledger.hpp`, `test_book.cpp`, `test_engine_basic.cpp` |
| R-INV-01 | `invariants.hpp`, `test_engine_basic.cpp`, `test_match.cpp`, `test_random.cpp` |
| R-INV-02 | `invariants.hpp`, `ledger.hpp`, `test_engine_basic.cpp`, `test_mod.cpp` |
| R-INV-03 | `invariants.hpp`, `test_book_side.cpp`, `test_match.cpp`, `test_mod.cpp` |
| R-INV-04 | `invariants.hpp` |
| R-INV-05 | `invariants.hpp`, `test_mkt.cpp` |
| R-INV-06 | `invariants.hpp`, `test_book_side.cpp`, `test_engine_basic.cpp`, `test_random.cpp` |
<!-- coverage:end -->

## Layout

```
specs/        spec.md, plan.md, tasks.md
include/me/   public headers (engine.hpp, types.hpp, config.hpp, events.hpp)
src/          non-template parts of the data structures (library target `me`)
tests/        GoogleTest unit, randomized and differential tests, plus test support
bench/        workload generator, throughput benchmark, latency tool
tools/        spec coverage check
```

## Roadmap

Each version starts with a change to the spec.

- v2: IOC and FOK orders.
- v3: multiple instruments.
- v4: ITCH message parser.

## License

[MIT](LICENSE)
