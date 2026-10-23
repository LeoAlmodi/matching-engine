# Matching Engine v1: Specification

This document defines what the engine does. It contains no implementation details; those belong in `plan.md`.
Every requirement has an identifier (`R-<AREA>-<NN>`). Tests must reference the identifiers they verify.

## 1. Scope

In scope:

- A single instrument.
- Limit and market orders, buy and sell.
- Cancellation and modification of resting orders.
- Price-time priority (FIFO within each price level).
- Partial executions.
- Event emission for every command.

Out of scope for v1:

- Multiple instruments.
- Network transport and real protocols (ITCH, FIX).
- Persistence and recovery.
- IOC, FOK and stop orders.
- Order ownership (accounts, participants) and therefore self-trade prevention.

## 2. Definitions

- **Tick:** the minimum price increment. All prices are integers expressed in ticks.
- **Side:** BUY or SELL.
- **Resting order:** an accepted limit order with open quantity that sits in the book.
- **Book:** the set of resting orders, split into the bid side (BUY) and the ask side (SELL).
- **Price level:** all resting orders on one side at the same price.
- **Best bid / best ask:** the highest resting BUY price / the lowest resting SELL price.
- **Crossed book:** both sides non-empty and best bid ≥ best ask.
- **Aggressor:** the incoming order (or modified order) that triggers matching.
- **Passive order:** the resting order it matches against.
- **Leaves:** the open (unexecuted, uncanceled) quantity of an order.
- **Requested quantity:** the initial quantity of an order plus every increase applied by modifications.
- **Arrival sequence:** the position of an order in time priority, assigned by the engine.

## 3. Configuration

- **R-CFG-01.** The engine is configured with `MAX_PRICE`, an integer ≥ 1. Valid prices are `1..MAX_PRICE` inclusive.
- **R-CFG-02.** The engine is configured with `MAX_QTY`, an integer ≥ 1. Valid quantities are `1..MAX_QTY` inclusive.
- **R-CFG-03.** The engine is configured with `MAX_ORDERS`, an integer ≥ 1: the maximum number of orders that can be accepted in a session, counting every accepted order whether it is still resting, filled or canceled.

## 4. Commands

- **R-CMD-01.** The engine accepts three commands:
  - `NEW(order_id, side, type, price, quantity)`, where `type` is LIMIT or MARKET.
  - `CANCEL(order_id)`.
  - `MODIFY(order_id, new_price, new_quantity)`.
- **R-CMD-02.** `order_id` is supplied by the client and is an unsigned 64-bit integer. `price` and `quantity` are signed 64-bit integers.
- **R-CMD-03.** Commands are processed one at a time, in the order received. A command is fully processed, with all of its events emitted, before the next one starts.
- **R-CMD-04.** Processing is deterministic: the same sequence of commands on the same configuration produces the same sequence of events.
- **R-CMD-05.** Every command produces at least one event.

## 5. Validation of NEW

- **R-VAL-01.** If `quantity < 1` or `quantity > MAX_QTY`, the order is rejected with reason `INVALID_QUANTITY`.
- **R-VAL-02.** For a LIMIT order, if `price < 1` or `price > MAX_PRICE`, the order is rejected with reason `INVALID_PRICE`.
- **R-VAL-03.** For a MARKET order, the `price` field is ignored and never causes a rejection.
- **R-VAL-04.** If `order_id` belongs to an order previously accepted in this session, the order is rejected with reason `DUPLICATE_ORDER_ID`. This holds even if that order has since been filled or canceled.
- **R-VAL-05.** Checks are applied in the order quantity, price, duplicate id, capacity. Only the first failing check is reported, as a single `REJECTED` event.
- **R-VAL-06.** A rejected order changes no state. Its `order_id` is not considered used.
- **R-VAL-07.** If `MAX_ORDERS` orders have already been accepted in this session, the order is rejected with reason `CAPACITY_EXCEEDED`.

## 6. Book

- **R-BOOK-01.** A LIMIT order with leaves > 0 after matching rests in the book at its price with those leaves.
- **R-BOOK-02.** Priority between resting orders on the same side is by price first (higher price first for BUY, lower price first for SELL), then by arrival sequence (earlier first).
- **R-BOOK-03.** An order's arrival sequence is assigned when it is accepted, and is reassigned only by a modification that loses priority (R-MOD-05).
- **R-BOOK-04.** A MARKET order never rests in the book.

## 7. Matching

- **R-MATCH-01.** A BUY LIMIT order with price P is matchable against resting SELL orders with price ≤ P. A SELL LIMIT order with price P is matchable against resting BUY orders with price ≥ P.
- **R-MATCH-02.** The aggressor matches against the passive order with the highest priority (R-BOOK-02), and continues to the next one, across price levels, while it has leaves > 0 and a matchable passive order exists.
- **R-MATCH-03.** The trade price is the passive order's price.
- **R-MATCH-04.** The trade quantity is the minimum of the aggressor's leaves and the passive order's leaves.
- **R-MATCH-05.** Each match reduces the leaves of both orders by the trade quantity. A passive order whose leaves reach 0 is removed from the book.
- **R-MATCH-06.** Each match emits exactly one `TRADE` event.

## 8. Market orders

- **R-MKT-01.** A MARKET BUY order is matchable against any resting SELL order; a MARKET SELL order against any resting BUY order. Matching otherwise follows R-MATCH-02 to R-MATCH-06.
- **R-MKT-02.** If the opposite side is empty when a valid MARKET order arrives, the order is rejected with reason `NO_LIQUIDITY`. No `ACCEPTED` event is emitted and no state changes. This check runs after R-VAL-05.
- **R-MKT-03.** If the opposite side is exhausted before a MARKET order is fully filled, the remaining leaves are canceled and a `CANCELED` event with reason `MARKET_REMAINDER` is emitted.

## 9. Cancellation

- **R-CXL-01.** `CANCEL` on a resting order removes it from the book and emits `CANCELED` with reason `USER` and `canceled_quantity` equal to its leaves.
- **R-CXL-02.** `CANCEL` on an `order_id` that is not resting (never accepted, fully filled or already canceled) is rejected with reason `UNKNOWN_ORDER`. No state changes.

## 10. Modification

- **R-MOD-01.** `MODIFY` on an `order_id` that is not resting is rejected with reason `UNKNOWN_ORDER`.
- **R-MOD-02.** `new_quantity` is the new leaves of the order (not its new total). If `new_quantity < 1` or `new_quantity > MAX_QTY`, the command is rejected with reason `INVALID_QUANTITY`. A `new_quantity` of 0 is therefore rejected; `CANCEL` is the way to remove an order.
- **R-MOD-03.** If `new_price < 1` or `new_price > MAX_PRICE`, the command is rejected with reason `INVALID_PRICE`.
- **R-MOD-04.** Checks are applied in the order unknown order, quantity, price. A rejected `MODIFY` changes no state.
- **R-MOD-05.** Priority after a valid modification:
  - Same price and `new_quantity ≤` current leaves: the order keeps its arrival sequence.
  - Different price, or `new_quantity >` current leaves: the order receives a new arrival sequence, as if it had just arrived.
- **R-MOD-06.** Quantity accounting:
  - A decrease of leaves by `d` adds `d` to the order's canceled quantity.
  - An increase of leaves by `d` adds `d` to the order's requested quantity.
- **R-MOD-07.** If the new price makes the order matchable (R-MATCH-01), it matches as an aggressor following R-MATCH-02 to R-MATCH-06. Remaining leaves rest at the new price.
- **R-MOD-08.** A modification with the same price and the same quantity is valid. It emits `MODIFIED` and the order keeps its arrival sequence.

## 11. Events

- **R-EVT-01.** Event types and fields:

  | Event | Fields |
  |---|---|
  | `ACCEPTED` | `order_id` |
  | `REJECTED` | `order_id`, `command` (NEW, CANCEL, MODIFY), `reason` |
  | `TRADE` | `aggressor_id`, `passive_id`, `price`, `quantity`, `aggressor_leaves`, `passive_leaves` |
  | `CANCELED` | `order_id`, `canceled_quantity`, `reason` (USER, MARKET_REMAINDER) |
  | `MODIFIED` | `order_id`, `price`, `leaves` |

- **R-EVT-02.** Rejection reasons: `INVALID_QUANTITY`, `INVALID_PRICE`, `DUPLICATE_ORDER_ID`, `CAPACITY_EXCEEDED`, `NO_LIQUIDITY`, `UNKNOWN_ORDER`.
- **R-EVT-03.** Event order for an accepted `NEW`: `ACCEPTED`, then one `TRADE` per match in matching order, then `CANCELED` (MARKET_REMAINDER) if applicable.
- **R-EVT-04.** Event order for a valid `MODIFY`: `MODIFIED`, then one `TRADE` per match in matching order.
- **R-EVT-05.** An order is fully executed when a `TRADE` reports its leaves as 0, and partially executed otherwise.

## 12. Queries

The engine exposes the following read-only state, used by tests:

- **R-QRY-01.** Best bid and best ask: price and total leaves at that level, or an indication that the side is empty.
- **R-QRY-02.** Total leaves at a given side and price (0 if no level exists).
- **R-QRY-03.** State of an order by `order_id`: if resting, its side, price and leaves; otherwise, an indication that it is not resting.

## 13. Invariants

These hold after every command is processed.

- **R-INV-01.** The book is never crossed.
- **R-INV-02.** For every accepted order: requested quantity = executed quantity + leaves + canceled quantity.
- **R-INV-03.** Within a price level, orders are executed in arrival-sequence order.
- **R-INV-04.** Every resting order has leaves in `1..MAX_QTY` and price in `1..MAX_PRICE`.
- **R-INV-05.** No MARKET order rests in the book.
- **R-INV-06.** The total leaves reported for a level equals the sum of the leaves of its resting orders.

## 14. Edge cases

| Case | Requirements |
|---|---|
| Quantity zero or negative | R-VAL-01, R-MOD-02 |
| Price invalid (≤ 0 or > MAX_PRICE) | R-VAL-02, R-MOD-03 |
| Cancel or modify a nonexistent, filled or canceled order | R-CXL-02, R-MOD-01 |
| Market order with no liquidity | R-MKT-02 |
| Market order with partial liquidity | R-MKT-03 |
| Order sweeping several price levels | R-MATCH-02 |
| Modify that crosses the book | R-MOD-07 |
| Duplicate order id | R-VAL-04 |
| Session order capacity reached | R-VAL-07 |

## 15. Decision log

| ID | Decision |
|---|---|
| D-01 | Market order remainder is canceled (R-MKT-03). |
| D-02 | Market order with no liquidity is rejected (R-MKT-02). |
| D-03 | Price field on market orders is ignored (R-VAL-03). |
| D-04 | `new_quantity` in MODIFY means new leaves (R-MOD-02). |
| D-05 | Decrease keeps priority; increase or price change loses it (R-MOD-05). |
| D-06 | A modify that crosses executes (R-MOD-07). |
| D-07 | Modify to quantity 0 is rejected (R-MOD-02). |
| D-08 | No-op modify is accepted and keeps priority (R-MOD-08). |
| D-09 | Conservation uses requested quantity; decreases count as canceled (R-MOD-06, R-INV-02). |
| D-10 | Modifications emit a dedicated `MODIFIED` event (R-EVT-01). |
| D-11 | One `TRADE` event per match, carrying both sides (R-EVT-01). |
| D-12 | Event order fixed per command (R-EVT-03, R-EVT-04). |
| D-13 | Cancel and modify rejections use `REJECTED` with a reason (R-EVT-01). |
| D-14 | Order ids are supplied by the client (R-CMD-02). |
| D-15 | Ids are unique among orders accepted in the session (R-VAL-04). |
| D-16 | A single `UNKNOWN_ORDER` reason covers nonexistent, filled and canceled orders (R-CXL-02, R-MOD-01). |
| D-17 | Prices range over `1..MAX_PRICE` (R-CFG-01). |
| D-18 | Quantities range over `1..MAX_QTY` (R-CFG-02). |
| D-19 | Validation precedence is fixed (R-VAL-05, R-MOD-04). |
| D-20 | Orders have no owner in v1; self-trade prevention is out of scope (§1). |
| D-21 | The engine exposes best bid/ask, level quantity and order state (§12). |
| D-22 | Accepted orders per session are capped by `MAX_ORDERS`; beyond it, NEW is rejected with `CAPACITY_EXCEEDED` (R-CFG-03, R-VAL-07). |
