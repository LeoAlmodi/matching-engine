# Matching Engine v1: Implementation Plan

This document defines how the engine is built. It implements `spec.md` and must not add behavior.
If anything here contradicts the spec, the spec wins and this document is fixed.

## 1. Goals and constraints

- Single-threaded engine, one instrument.
- No dynamic allocation while processing commands. Everything is allocated once, in the constructor.
- No exceptions on the processing path. Every failure becomes a `REJECTED` event (spec §11).
- Deterministic: no randomness, no clock reads, no iteration over unordered containers in the engine.
- The engine depends only on the C++20 standard library.

## 2. Files

```
include/me/types.hpp        Price, Qty, OrderId, Side, OrderType, RejectReason, CancelReason, Command
include/me/config.hpp       Config
include/me/events.hpp       event structs and the EventSink concept
include/me/engine.hpp       Engine<Sink> (template, header)
include/me/detail/order_pool.hpp   OrderNode, OrderPool
include/me/detail/id_map.hpp       IdMap
include/me/detail/book_side.hpp    Level, BookSide
src/order_pool.cpp, src/id_map.cpp, src/book_side.cpp   non-template parts
```

`Engine` is a template on its event sink, so it lives in a header. The data structures under `detail/` do not depend on the sink and are compiled in `src/`.

## 3. Types

```cpp
using Price   = std::int64_t;   // ticks
using Qty     = std::int64_t;
using OrderId = std::uint64_t;
using Index   = std::uint32_t;  // position in the order pool
inline constexpr Index NIL = std::numeric_limits<Index>::max();

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class Command : std::uint8_t { New, Cancel, Modify };
enum class RejectReason : std::uint8_t {
  InvalidQuantity, InvalidPrice, DuplicateOrderId, CapacityExceeded, NoLiquidity, UnknownOrder };
enum class CancelReason : std::uint8_t { User, MarketRemainder };
```

Plain aliases in v1, no strong types. Links between orders are 32-bit pool indices, not pointers: half the size and stable if the pool moves.

## 4. Configuration

```cpp
struct Config {
  Price max_price;          // R-CFG-01
  Qty max_qty;              // R-CFG-02
  std::uint32_t max_orders; // R-CFG-03, must be < NIL - 1
};
```

Runtime parameters, validated in the constructor. An invalid `Config` throws `std::invalid_argument` at construction (not on the processing path).

## 5. Data structures

### 5.1 OrderNode and OrderPool

```cpp
struct OrderNode {
  OrderId id;
  Price price;
  Qty leaves;
  Index prev, next;   // FIFO links within the level
  Side side;
};
```

`OrderPool` holds a `std::vector<OrderNode>` of size `max_orders`, allocated in the constructor, plus an intrusive free list threaded through `next`. `alloc()` and `free(Index)` are O(1). Since at most `max_orders` orders are ever accepted (R-VAL-07), the pool never runs out.

### 5.2 Level

```cpp
struct Level {
  Index head = NIL, tail = NIL;  // FIFO: head is the oldest order
  Qty total = 0;                 // sum of leaves, R-QRY-02, R-INV-06
};
```

Doubly linked intrusive list. Append at tail, remove from head, unlink from the middle: all O(1).

### 5.3 BookSide

One per side. Holds `std::vector<Level>` of size `max_price + 1`, indexed by price, plus:

- `best`: the best price with a non-empty level, or none.
- `non_empty`: number of non-empty levels, so an empty side is detected without scanning.

Operations:

- `append(Index)`: link at the tail of the order's level, add leaves to `total`, update `best` if the new price is better.
- `unlink(Index)`: remove from the level, subtract leaves. If the level becomes empty and it was `best`, scan toward worse prices to the next non-empty level (guaranteed to exist if `non_empty > 0`).
- `front()`: head order of the `best` level.

The scan after a level empties is O(distance to the next level). This is accepted in v1; if profiling shows it matters, a later performance task adds a bitmap of occupied levels (see §11).

Memory: `2 × (max_price + 1) × 16` bytes. For `max_price = 100'000`, about 3.2 MB.

### 5.4 IdMap

Open-addressing hash map from `OrderId` to a slot value, with fixed capacity: the smallest power of two ≥ `2 × max_orders` (load factor ≤ 0.5). Linear probing. Hash: the splitmix64 finalizer, so sequential or adversarial ids spread well.

Each entry stores the key and a 32-bit value:

- `EMPTY`: slot unused.
- `NOT_RESTING`: the id was accepted in this session but the order is filled or canceled. Covers R-VAL-04 (duplicate detection) and R-CXL-02 / R-MOD-01 (unknown order).
- any other value: the pool index of the resting order.

Entries are never deleted, only updated to `NOT_RESTING`, because the spec requires remembering every accepted id (D-15). As a result there are no tombstones and probing stays simple. The number of accepted orders is counted separately for R-VAL-07.

## 6. Event output

```cpp
template <class S>
concept EventSink = requires(S& s, OrderId id, Command c, RejectReason r, CancelReason cr,
                             Price p, Qty q) {
  s.on_accepted(id);
  s.on_rejected(id, c, r);
  s.on_trade(id, id, p, q, q, q);   // aggressor, passive, price, qty, aggressor_leaves, passive_leaves
  s.on_canceled(id, q, cr);
  s.on_modified(id, p, q);
};
```

`Engine<Sink>` holds a reference to the sink and calls it directly: static dispatch, inlinable, no virtual calls and no buffering. Sinks:

- Tests: `RecordingSink`, which appends event structs to a `std::vector` (allocation is fine in tests).
- Benchmark: `CountingSink`, which only increments counters, so the compiler cannot discard the work.

## 7. Public API

```cpp
template <EventSink Sink>
class Engine {
public:
  Engine(const Config& cfg, Sink& sink);

  void new_order(OrderId id, Side side, OrderType type, Price price, Qty qty);
  void cancel(OrderId id);
  void modify(OrderId id, Price new_price, Qty new_qty);

  struct LevelView { Price price; Qty total; };
  struct OrderView { Side side; Price price; Qty leaves; };

  std::optional<LevelView> best_bid() const;           // R-QRY-01
  std::optional<LevelView> best_ask() const;           // R-QRY-01
  Qty level_qty(Side side, Price price) const;         // R-QRY-02
  std::optional<OrderView> order_state(OrderId id) const;  // R-QRY-03

  // Test support only: visits resting orders of one level in FIFO order.
  template <class F> void visit_level(Side side, Price price, F&& f) const;
};
```

`visit_level` is not part of the spec's observable behavior; it exists so the invariant checker can verify R-INV-03 and R-INV-06. It is not used on the processing path.

## 8. Algorithms

### 8.1 NEW

1. Validate in spec order: quantity (R-VAL-01), price for LIMIT (R-VAL-02), duplicate id (R-VAL-04), capacity (R-VAL-07). On failure, `on_rejected` and return.
2. MARKET with empty opposite side: `on_rejected(NoLiquidity)` and return (R-MKT-02).
3. Insert id in `IdMap`, increment accepted count, `on_accepted` (R-EVT-03).
4. Match (§8.2) with limit price P for LIMIT, or no limit for MARKET.
5. Remaining leaves: LIMIT allocates a node, sets the IdMap value to its index and `append`s it (R-BOOK-01). MARKET emits `on_canceled(MarketRemainder)` (R-MKT-03) and sets the IdMap value to `NOT_RESTING`.

If the order is fully filled, its IdMap value is `NOT_RESTING`.

### 8.2 Match loop (shared by NEW and MODIFY)

```
while leaves > 0 and opposite side not empty and best opposite price is matchable:
    passive = opposite.front()
    q = min(leaves, passive.leaves)                       R-MATCH-04
    leaves -= q; passive.leaves -= q; level.total -= q    R-MATCH-05
    sink.on_trade(aggressor, passive.id, passive.price, q, leaves, passive.leaves)   R-MATCH-03, 06
    if passive.leaves == 0:
        opposite.unlink(passive); pool.free(passive); idmap[passive.id] = NOT_RESTING
```

"Matchable" is `ask ≤ P` for a buy and `bid ≥ P` for a sell (R-MATCH-01), or always for a market order (R-MKT-01).

### 8.3 CANCEL

Look up the id. If the value is not a pool index, `on_rejected(UnknownOrder)` (R-CXL-02). Otherwise `unlink`, emit `on_canceled(User, leaves)`, free the node, set `NOT_RESTING` (R-CXL-01).

### 8.4 MODIFY

1. Validate: unknown order (R-MOD-01), quantity (R-MOD-02), price (R-MOD-03), in that order (R-MOD-04).
2. Emit `on_modified(id, new_price, new_qty)` (R-EVT-04).
3. If `new_price == price` and `new_qty ≤ leaves`: update `leaves` and `level.total` in place; the order keeps its position (R-MOD-05, R-MOD-08).
4. Otherwise: `unlink` the order, set the new price and leaves, run the match loop with the order as aggressor (R-MOD-07), then `append` any remainder at the tail of its new level (loses priority, R-MOD-05). If fully filled, free the node and set `NOT_RESTING`.

Quantity accounting (R-MOD-06) needs no stored state in the engine: it is reconstructed by the invariant checker from commands and events (§9).

### 8.5 Complexity

| Operation | Cost |
|---|---|
| NEW that rests without trading | O(1) |
| Each trade | O(1), plus the level scan when a level empties |
| CANCEL | O(1), plus the level scan if it empties the best level |
| MODIFY | O(1) in place, or as CANCEL + NEW |
| Queries | O(1); `visit_level` O(orders in level) |
| Id lookup | O(1) expected |

## 9. Testing

- **Unit tests per requirement group.** One file per spec area: `test_val.cpp`, `test_book.cpp`, `test_match.cpp`, `test_mkt.cpp`, `test_cxl.cpp`, `test_mod.cpp`, `test_evt.cpp`, `test_qry.cpp`. Test names carry the requirement id, for example `TEST(Match, R_MATCH_02_SweepsSeveralLevels)`. Tests compare the exact sequence of recorded events.
- **Invariant checker.** `check_invariants(engine, ledger)` verifies R-INV-01 to R-INV-06 and is called at the end of every unit test. R-INV-02 uses a `Ledger` in the test code that tracks requested, executed and canceled quantity per order from commands and events.
- **Randomized tests.** Millions of commands from a fixed-seed generator, with the invariant checker after each command. On failure, print the seed and the command index.
- **Reference engine (later task).** A naive implementation in `tests/` (vectors, linear search) driven by the same commands; its event stream must equal the engine's.
- **Sanitizers.** All tests run in the debug build with ASan and UBSan.

## 10. Benchmark

- **Generator.** Commands are generated before measurement into a pre-allocated array, with a fixed seed. Prices are concentrated around a mid price. Initial mix: about 60% limit, 5% market, 30% cancel, 5% modify. Cancels and modifies target ids that are currently resting. The book is pre-filled before measuring.
- **Throughput.** Google Benchmark target `bench`, with `CountingSink`, reporting commands per second.
- **Latency.** A separate executable `latency` (custom `main`, no framework): timestamps each command with `rdtsc` (converted to ns with a calibrated frequency) into a pre-allocated array, then sorts and reports p50, p99, p99.9 and max. Warm-up pass before measuring.
- **Environment.** Release build with `-O3 -march=native`, process pinned to one core with `taskset`, CPU model and frequency recorded with every result.
- **Targets.** Not fixed yet. A baseline is measured first, and targets are set from it in a later revision of this plan.

## 11. Deferred optimizations

Only after a baseline exists and profiling justifies them, each as its own task with before and after numbers:

- Bitmap of occupied levels to find the next best price without scanning.
- Hot/cold split of `OrderNode` fields and cache-line alignment.
- Prefetching the next passive order in the match loop.

## 12. Traceability

| Spec area | Component |
|---|---|
| R-CFG-* | `Config`, `Engine` constructor |
| R-CMD-* | `Engine::new_order`, `cancel`, `modify` |
| R-VAL-* | §8.1 step 1, `IdMap` |
| R-BOOK-* | `BookSide`, `Level`, `OrderPool` |
| R-MATCH-* | match loop, §8.2 |
| R-MKT-* | §8.1 steps 2 and 5, §8.2 |
| R-CXL-* | §8.3 |
| R-MOD-* | §8.4 |
| R-EVT-* | `EventSink`, call order in §8 |
| R-QRY-* | query methods, §7 |
| R-INV-* | test invariant checker, §9 |
