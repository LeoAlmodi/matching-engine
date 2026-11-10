// T-07: validation of NEW. Covers R-VAL-01 to R-VAL-07, R-EVT-02 and R-CMD-05; plan §8.1 step 1.
// Every command is followed by the R-CMD-05 check and check_invariants (see Harness). Rejections
// of filled and canceled ids are covered in test_match.cpp and test_cxl.cpp.

#include "support/harness.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace {

using me::Accepted;
using me::Command;
using me::Config;
using me::OrderId;
using me::OrderType;
using me::Price;
using me::Qty;
using me::Rejected;
using me::RejectReason;
using me::Side;
using me::test::Event;
using me::test::Harness;
using me::test::kDefaultConfig;

using Events = std::vector<Event>;
using LevelView = me::Engine<me::test::RecordingSink>::LevelView;
using OrderView = me::Engine<me::test::RecordingSink>::OrderView;

constexpr Price kMaxPrice = kDefaultConfig.max_price;
constexpr Qty kMaxQty = kDefaultConfig.max_qty;
constexpr Qty kQtyMin = std::numeric_limits<Qty>::min();
constexpr Qty kQtyMax = std::numeric_limits<Qty>::max();

Events rejected(OrderId id, RejectReason reason) {
  return Events{Rejected{id, Command::New, reason}};
}

// R-VAL-06: nothing rests and nothing is accepted.
void expect_untouched(const Harness &h, OrderId id) {
  EXPECT_FALSE(h.engine.order_state(id).has_value());
  EXPECT_TRUE(h.ledger.orders().count(id) == 0);
}

// ---- R-VAL-01: quantity -------------------------------------------------------------------------

TEST(Val, R_VAL_01_InvalidQuantityIsRejected) {
  for (Qty q : {Qty{0}, Qty{-1}, kMaxQty + 1, kQtyMin, kQtyMax}) {
    Harness h;
    EXPECT_EQ(h.buy(1, 100, q), rejected(1, RejectReason::InvalidQuantity)) << "qty " << q;
    expect_untouched(h, 1);
    EXPECT_EQ(h.sell(2, 100, q), rejected(2, RejectReason::InvalidQuantity)) << "qty " << q;
    expect_untouched(h, 2);
  }
}

TEST(Val, R_VAL_01_QuantityBoundsAreAccepted) {
  Harness h;
  EXPECT_EQ(h.buy(1, 100, 1), (Events{Accepted{1}}));
  EXPECT_EQ(h.buy(2, 100, kMaxQty), (Events{Accepted{2}}));
  EXPECT_EQ(h.engine.level_qty(Side::Buy, 100), 1 + kMaxQty);
}

// ---- R-VAL-02: price of a LIMIT order -----------------------------------------------------------

TEST(Val, R_VAL_02_InvalidPriceIsRejected) {
  for (Price p : {Price{0}, Price{-1}, kMaxPrice + 1, std::numeric_limits<Price>::min(),
                  std::numeric_limits<Price>::max()}) {
    Harness h;
    EXPECT_EQ(h.buy(1, p, 10), rejected(1, RejectReason::InvalidPrice)) << "price " << p;
    expect_untouched(h, 1);
    EXPECT_EQ(h.sell(2, p, 10), rejected(2, RejectReason::InvalidPrice)) << "price " << p;
    expect_untouched(h, 2);
  }
}

TEST(Val, R_VAL_02_PriceBoundsAreAccepted) {
  Harness h;
  EXPECT_EQ(h.buy(1, 1, 10), (Events{Accepted{1}}));
  EXPECT_EQ(h.sell(2, kMaxPrice, 10), (Events{Accepted{2}}));
}

// ---- R-VAL-04: duplicate id ---------------------------------------------------------------------

TEST(Val, R_VAL_04_DuplicateOfRestingOrderIsRejected) {
  Harness h;
  h.buy(1, 100, 10);
  EXPECT_EQ(h.buy(1, 100, 10), rejected(1, RejectReason::DuplicateOrderId));
  EXPECT_EQ(h.sell(1, 200, 5), rejected(1, RejectReason::DuplicateOrderId));
  // The resting order is unaffected.
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Buy, 100, 10}));
  EXPECT_EQ(h.engine.level_qty(Side::Buy, 100), 10);
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

TEST(Val, R_VAL_04_ExtremeIdsAreOrdinary) {
  Harness h;
  constexpr OrderId kMaxId = std::numeric_limits<OrderId>::max();
  EXPECT_EQ(h.buy(0, 100, 10), (Events{Accepted{0}}));
  EXPECT_EQ(h.buy(kMaxId, 100, 10), (Events{Accepted{kMaxId}}));
  EXPECT_EQ(h.buy(0, 100, 10), rejected(0, RejectReason::DuplicateOrderId));
  EXPECT_EQ(h.buy(kMaxId, 100, 10), rejected(kMaxId, RejectReason::DuplicateOrderId));
}

// ---- R-VAL-07: capacity -------------------------------------------------------------------------

TEST(Val, R_VAL_07_CapacityExceeded) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 3});
  h.buy(1, 100, 10);
  h.buy(2, 101, 10);
  h.sell(3, 200, 10);
  EXPECT_EQ(h.buy(4, 100, 10), rejected(4, RejectReason::CapacityExceeded));
  EXPECT_EQ(h.sell(5, 300, 10), rejected(5, RejectReason::CapacityExceeded));
  expect_untouched(h, 4);
  expect_untouched(h, 5);
}

// ---- R-VAL-05: precedence quantity, price, duplicate id, capacity -------------------------------

TEST(Val, R_VAL_05_QuantityBeforePrice) {
  Harness h;
  EXPECT_EQ(h.buy(1, 0, 0), rejected(1, RejectReason::InvalidQuantity));
}

TEST(Val, R_VAL_05_PriceBeforeDuplicate) {
  Harness h;
  h.buy(1, 100, 10);
  EXPECT_EQ(h.buy(1, kMaxPrice + 1, 10), rejected(1, RejectReason::InvalidPrice));
  EXPECT_EQ(h.buy(1, 100, kMaxQty + 1), rejected(1, RejectReason::InvalidQuantity));
}

TEST(Val, R_VAL_05_DuplicateBeforeCapacity) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 1});
  h.buy(1, 100, 10);
  EXPECT_EQ(h.buy(1, 100, 10), rejected(1, RejectReason::DuplicateOrderId));
  EXPECT_EQ(h.buy(2, 100, 10), rejected(2, RejectReason::CapacityExceeded));
}

TEST(Val, R_VAL_05_AllChecksFailingReportsQuantity) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 1});
  h.buy(1, 100, 10);
  EXPECT_EQ(h.buy(1, -1, -1), rejected(1, RejectReason::InvalidQuantity));
}

// ---- R-VAL-06: a rejected order changes no state ------------------------------------------------

TEST(Val, R_VAL_06_RejectionLeavesBookUnchanged) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 2});
  h.buy(1, 100, 10);
  h.sell(2, 200, 20);
  h.buy(3, 0, 10);   // invalid price
  h.sell(4, 150, 0); // invalid quantity
  h.buy(1, 150, 10); // duplicate
  h.buy(5, 150, 10); // capacity
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 10}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{200, 20}));
  EXPECT_EQ(h.engine.level_qty(Side::Buy, 150), 0);
  EXPECT_EQ(h.engine.level_qty(Side::Sell, 150), 0);
}

TEST(Val, R_VAL_06_RejectedIdCanBeReused) {
  Harness h;
  EXPECT_EQ(h.buy(7, 100, 0), rejected(7, RejectReason::InvalidQuantity));
  EXPECT_EQ(h.buy(7, 0, 10), rejected(7, RejectReason::InvalidPrice));
  EXPECT_EQ(h.buy(7, 100, 10), (Events{Accepted{7}}));
  EXPECT_EQ(h.engine.order_state(7), (OrderView{Side::Buy, 100, 10}));
}

// A rejected order does not count toward capacity (R-VAL-06, R-VAL-07).
TEST(Val, R_VAL_06_RejectionDoesNotConsumeCapacity) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 2});
  h.buy(1, 100, 0);
  h.buy(2, 0, 10);
  h.buy(3, 100, 10);
  h.buy(3, 100, 10); // duplicate
  EXPECT_EQ(h.buy(4, 100, 10), (Events{Accepted{4}}));
  EXPECT_EQ(h.buy(5, 100, 10), rejected(5, RejectReason::CapacityExceeded));
}

} // namespace
