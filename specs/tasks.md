# Matching Engine v1: Tasks

The spec (`spec.md`) and plan (`plan.md`) broken into small tasks. Work them in order, one at a time.

For every task:

1. Read the requirements and plan sections it lists.
2. Write the tests first, named or commented with the requirement ids. Confirm they fail.
3. Implement until the tests pass in the debug build (ASan + UBSan) with no warnings.
4. Review the code against the spec, not only the tests.
5. Suggest a commit message (Conventional Commits, with the task id). The user commits.
6. Mark the task as done here.

A task is done only when every item under "Done when" holds. If a task reveals a gap or ambiguity in the spec, stop and propose a spec change first.

## Phase 0: Infrastructure

### T-01. CI

- **Covers:** none (infrastructure).
- **Files:** `.github/workflows/ci.yml`.
- **Work:** GitHub Actions workflow on push and pull request: configure and build the debug build with sanitizers, run `ctest --output-on-failure`.
- **Done when:** the workflow passes on `main` with the current placeholder test.

## Phase 1: Building blocks

### T-02. Types, config and events

- **Covers:** R-CMD-02, R-EVT-01, R-EVT-02 (definitions only); plan §3, §4, §6.
- **Files:** `include/me/types.hpp`, `include/me/config.hpp`, `include/me/events.hpp`, `tests/support/recording_sink.hpp`, `tests/test_types.cpp`.
- **Work:** type aliases and enums, `Config`, event structs, the `EventSink` concept, and a `RecordingSink` for tests that stores events in order and supports equality comparison.
- **Done when:** `static_assert(EventSink<RecordingSink>)` compiles; a test shows `RecordingSink` records each event type with all fields.

### T-03. OrderPool

- **Covers:** plan §5.1.
- **Files:** `include/me/detail/order_pool.hpp`, `src/order_pool.cpp`, `tests/test_order_pool.cpp`.
- **Work:** fixed-size pool with an intrusive free list; `alloc()`, `free(Index)`, access by index.
- **Done when:** tests cover allocating up to capacity, freeing and reusing slots, and that no allocation happens after construction (checked by capacity and address stability).

### T-04. IdMap

- **Covers:** plan §5.4; supports R-VAL-04, R-CXL-02, R-MOD-01.
- **Files:** `include/me/detail/id_map.hpp`, `src/id_map.cpp`, `tests/test_id_map.cpp`.
- **Work:** open-addressing map with power-of-two capacity, linear probing, splitmix64 hash; values `EMPTY`, `NOT_RESTING` or a pool index; insert, find, update, no deletion.
- **Done when:** tests cover insert and find, update to `NOT_RESTING`, colliding keys, sequential ids, id `0` and `UINT64_MAX`, and filling to `max_orders` entries.

### T-05. BookSide and Level

- **Covers:** plan §5.2, §5.3; supports R-BOOK-02, R-QRY-01, R-QRY-02.
- **Files:** `include/me/detail/book_side.hpp`, `src/book_side.cpp`, `tests/test_book_side.cpp`.
- **Work:** level array indexed by price, FIFO intrusive list per level, `total` per level, `best`, `non_empty`; `append`, `unlink`, `front`.
- **Done when:** tests cover FIFO order within a level, unlink from head, middle and tail, `total` after each operation, `best` updates on append and after the best level empties (including scans across gaps), and both sides' price ordering (higher is better for BUY, lower for SELL).

## Phase 2: Engine

### T-06. Engine skeleton, queries and test support

- **Covers:** R-CFG-01, R-CFG-02, R-CFG-03, R-CMD-01, R-QRY-01, R-QRY-02, R-QRY-03; plan §7, §9.
- **Files:** `include/me/engine.hpp`, `tests/support/invariants.hpp`, `tests/support/ledger.hpp`, `tests/test_engine_basic.cpp`.
- **Work:** `Engine<Sink>` constructor with config validation, the three command methods as stubs, all queries, and `visit_level`. Test support: `check_invariants` (R-INV-01, R-INV-03 to R-INV-06) and `Ledger` (R-INV-02), both processing every event type.
- **Done when:** invalid configs throw at construction; queries on an empty book return empty or zero; `check_invariants` passes on an empty book.

### T-07. Validation of NEW

- **Covers:** R-VAL-01 to R-VAL-07, R-EVT-02, R-CMD-05; plan §8.1 step 1.
- **Files:** `include/me/engine.hpp`, `tests/test_val.cpp`.
- **Done when:** tests cover each rejection reason, the boundaries (0, 1, max, max + 1 for price and quantity, negatives), the precedence between failing checks, that a rejected order changes no state, that a rejected id can be reused, and that capacity counts filled and canceled orders.

### T-08. Resting limit orders

- **Covers:** R-BOOK-01, R-BOOK-02, R-BOOK-03, R-EVT-03 (ACCEPTED); plan §8.1.
- **Files:** `include/me/engine.hpp`, `tests/test_book.cpp`.
- **Done when:** tests show non-crossing limit orders on both sides emit `ACCEPTED` and appear in queries with the right price, leaves and level totals, ordered by price then arrival.

### T-09. Limit order matching

- **Covers:** R-MATCH-01 to R-MATCH-06, R-EVT-03, R-EVT-05, R-CMD-03, R-INV-01, R-INV-03; plan §8.2.
- **Files:** `include/me/engine.hpp`, `tests/test_match.cpp`.
- **Done when:** tests cover exact-price crossing, aggressor better than the best price (trade at passive price), partial fill of the aggressor (remainder rests), partial fill of the passive order, FIFO within a level, sweeping several levels, and the exact event sequence and fields in each case. `check_invariants` and the ledger pass after every command.

### T-10. Market orders

- **Covers:** R-MKT-01 to R-MKT-03, R-VAL-03, R-BOOK-04; plan §8.1 steps 2 and 5.
- **Files:** `include/me/engine.hpp`, `tests/test_mkt.cpp`.
- **Done when:** tests cover a full fill, a fill across levels, a partial fill with `CANCELED(MARKET_REMAINDER)`, rejection with `NO_LIQUIDITY` (no `ACCEPTED`, no state change), any price value being ignored, and that no market order ever rests.

### T-11. Cancellation

- **Covers:** R-CXL-01, R-CXL-02; plan §8.3.
- **Files:** `include/me/engine.hpp`, `tests/test_cxl.cpp`.
- **Done when:** tests cover canceling the head, middle and tail of a level, the last order of the best level (best price moves), a partially filled order (canceled quantity equals leaves), and rejection for never-accepted, filled and already-canceled ids.

### T-12. Modification

- **Covers:** R-MOD-01 to R-MOD-08, R-EVT-04; plan §8.4.
- **Files:** `include/me/engine.hpp`, `tests/test_mod.cpp`.
- **Done when:** tests cover each rejection and its precedence, decrease keeping priority, increase losing priority, price change losing priority, price change that crosses and trades (with `MODIFIED` before the trades), a crossing modify that fully fills, a no-op modify, and the ledger accounting for decreases and increases.

## Phase 3: Verification

### T-13. Randomized invariant test

- **Covers:** R-INV-01 to R-INV-06, R-CMD-04.
- **Files:** `tests/support/command_gen.hpp`, `tests/test_random.cpp`.
- **Work:** fixed-seed generator of valid and invalid commands of every type; run at least 1,000,000 commands checking invariants after each; run the same seed twice and compare event streams.
- **Done when:** the test passes for several seeds, and a failure prints the seed and command index.

### T-14. Spec coverage check

- **Covers:** project rule "every requirement has at least one test".
- **Files:** `tools/spec_coverage.py`, CI workflow.
- **Work:** script that extracts every `R-*` id from `spec.md` and checks that each appears in `tests/`; non-zero exit if any is missing. Add it to CI.
- **Done when:** the script passes and runs in CI.

### T-15. Reference engine

- **Covers:** differential check of the whole spec; plan §9.
- **Files:** `tests/support/reference_engine.hpp`, `tests/test_differential.cpp`.
- **Work:** naive implementation (vectors, linear search) of the same API; drive both engines with the generator from T-13 and compare event streams.
- **Done when:** both engines produce identical events for at least 1,000,000 commands over several seeds.

## Phase 4: Performance

### T-16. Benchmark command generator

- **Covers:** plan §10 (generator).
- **Files:** `bench/generator.hpp`.
- **Work:** pre-generated command array with a fixed seed, prices around a mid price, the planned mix, cancels and modifies targeting resting ids, and a pre-fill phase.
- **Done when:** a test or assertion checks the mix proportions and that the generated stream runs without `UNKNOWN_ORDER` rejections for cancels and modifies.

### T-17. Throughput benchmark

- **Covers:** plan §10 (throughput).
- **Files:** `bench/counting_sink.hpp`, `bench/bench_throughput.cpp`, `CMakeLists.txt`.
- **Done when:** `./build/release/bench` reports commands per second for the full mix and for each command type alone.

### T-18. Latency measurement

- **Covers:** plan §10 (latency).
- **Files:** `bench/latency.cpp`, `CMakeLists.txt` (new target `latency`).
- **Done when:** `./build/release/latency` prints p50, p99, p99.9 and max in nanoseconds, after a warm-up pass, with the CPU model.

### T-19. Baseline and targets

- **Covers:** plan §10 (targets).
- **Files:** `specs/plan.md`, `README.md`.
- **Work:** run T-17 and T-18 pinned to one core, record results and hardware, and set performance targets in the plan from the baseline.
- **Done when:** the plan has explicit targets and the README has the baseline table.

## Phase 5: Presentation

### T-20. README

- **Files:** `README.md`.
- **Work:** what the project is, the SDD workflow, how to build and test, design summary, benchmark results, and a requirement → test table (generated from T-14's script).
- **Done when:** a reader can build, test and benchmark from the README alone.

## Status

- [x] T-01 CI
- [x] T-02 Types, config and events
- [x] T-03 OrderPool
- [ ] T-04 IdMap
- [ ] T-05 BookSide and Level
- [ ] T-06 Engine skeleton, queries and test support
- [ ] T-07 Validation of NEW
- [ ] T-08 Resting limit orders
- [ ] T-09 Limit order matching
- [ ] T-10 Market orders
- [ ] T-11 Cancellation
- [ ] T-12 Modification
- [ ] T-13 Randomized invariant test
- [ ] T-14 Spec coverage check
- [ ] T-15 Reference engine
- [ ] T-16 Benchmark command generator
- [ ] T-17 Throughput benchmark
- [ ] T-18 Latency measurement
- [ ] T-19 Baseline and targets
- [ ] T-20 README
