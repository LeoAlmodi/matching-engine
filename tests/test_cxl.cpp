// T-11: cancellation. Covers R-CXL-01, R-CXL-02, and R-VAL-04 / R-VAL-07 for canceled orders;
// plan §8.3. Every command is followed by the ledger and check_invariants (see Harness).

#include "support/harness.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using me::Accepted;
using me::Canceled;
using me::CancelReason;
using me::Command;
using me::Config;
using me::OrderId;
using me::OrderType;
using me::Rejected;
using me::RejectReason;
using me::Side;
using me::test::Event;
using me::test::Harness;

using Events = std::vector<Event>;
using Ids = std::vector<OrderId>;
using LevelView = me::Engine<me::test::RecordingSink>::LevelView;

Events canceled(OrderId id, me::Qty qty) { return Events{Canceled{id, qty, CancelReason::User}}; }

Events unknown(OrderId id) {
  return Events{Rejected{id, Command::Cancel, RejectReason::UnknownOrder}};
}

// ---- R-CXL-01: canceling a resting order --------------------------------------------------------

class CxlLevel : public ::testing::Test {
protected:
  void SetUp() override {
    h.sell(1, 100, 10);
    h.sell(2, 100, 20);
    h.sell(3, 100, 30);
  }
  Harness h;
};

TEST_F(CxlLevel, R_CXL_01_CancelHead) {
  EXPECT_EQ(h.cancel(1), canceled(1, 10));
  EXPECT_EQ(h.ids_at(Side::Sell, 100), (Ids{2, 3}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{100, 50}));
  EXPECT_FALSE(h.engine.order_state(1).has_value());
}

TEST_F(CxlLevel, R_CXL_01_CancelMiddle) {
  EXPECT_EQ(h.cancel(2), canceled(2, 20));
  EXPECT_EQ(h.ids_at(Side::Sell, 100), (Ids{1, 3}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{100, 40}));
}

TEST_F(CxlLevel, R_CXL_01_CancelTail) {
  EXPECT_EQ(h.cancel(3), canceled(3, 30));
  EXPECT_EQ(h.ids_at(Side::Sell, 100), (Ids{1, 2}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{100, 30}));
  // A later order still joins at the tail.
  h.sell(4, 100, 1);
  EXPECT_EQ(h.ids_at(Side::Sell, 100), (Ids{1, 2, 4}));
}

// Canceling the last order of the best level moves the best price to the next level.
TEST(Cxl, R_CXL_01_LastOrderOfBestLevelMovesBest) {
  Harness h;
  h.buy(1, 100, 10);
  h.buy(2, 90, 20);
  h.buy(3, 80, 30);
  EXPECT_EQ(h.cancel(1), canceled(1, 10));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{90, 20}));
  EXPECT_EQ(h.cancel(2), canceled(2, 20));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{80, 30}));
  EXPECT_EQ(h.cancel(3), canceled(3, 30));
  EXPECT_FALSE(h.engine.best_bid().has_value());
}

TEST(Cxl, R_CXL_01_CancelNonBestLevelKeepsBest) {
  Harness h;
  h.sell(1, 100, 10);
  h.sell(2, 120, 20);
  EXPECT_EQ(h.cancel(2), canceled(2, 20));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{100, 10}));
  EXPECT_EQ(h.engine.level_qty(Side::Sell, 120), 0);
}

// The canceled quantity is the leaves, not the original quantity.
TEST(Cxl, R_CXL_01_PartiallyFilledOrderCancelsLeaves) {
  Harness h;
  h.sell(1, 100, 10);
  h.buy(2, 100, 4);
  EXPECT_EQ(h.cancel(1), canceled(1, 6));
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

// A partially filled aggressor that rested can be canceled too.
TEST(Cxl, R_CXL_01_RestingRemainderOfAggressor) {
  Harness h;
  h.sell(1, 100, 4);
  h.buy(2, 100, 10);
  EXPECT_EQ(h.cancel(2), canceled(2, 6));
  EXPECT_FALSE(h.engine.best_bid().has_value());
}

// ---- R-CXL-02: not resting ----------------------------------------------------------------------

TEST(Cxl, R_CXL_02_NeverAcceptedIdIsUnknown) {
  Harness h;
  h.buy(1, 100, 10);
  EXPECT_EQ(h.cancel(2), unknown(2));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 10}));
}

TEST(Cxl, R_CXL_02_RejectedNewIdIsUnknown) {
  Harness h;
  h.buy(1, 0, 10); // invalid price, never accepted
  EXPECT_EQ(h.cancel(1), unknown(1));
}

TEST(Cxl, R_CXL_02_FilledIdIsUnknown) {
  Harness h;
  h.sell(1, 100, 5);
  h.buy(2, 100, 5);
  EXPECT_EQ(h.cancel(1), unknown(1));
  EXPECT_EQ(h.cancel(2), unknown(2));
}

TEST(Cxl, R_CXL_02_MarketIdIsUnknown) {
  Harness h;
  h.sell(1, 100, 4);
  h.new_order(2, Side::Buy, OrderType::Market, 0, 10); // remainder canceled
  EXPECT_EQ(h.cancel(2), unknown(2));
}

TEST(Cxl, R_CXL_02_AlreadyCanceledIdIsUnknown) {
  Harness h;
  h.buy(1, 100, 10);
  h.buy(2, 100, 20);
  h.cancel(1);
  EXPECT_EQ(h.cancel(1), unknown(1));
  // No state changes: the other order is untouched.
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 20}));
  EXPECT_EQ(h.ids_at(Side::Buy, 100), (Ids{2}));
}

// ---- R-VAL-04, R-VAL-07 for canceled orders
// ------------------------------------------------------

TEST(Cxl, R_VAL_04_CanceledIdIsDuplicate) {
  Harness h;
  h.buy(1, 100, 10);
  h.cancel(1);
  EXPECT_EQ(h.buy(1, 100, 10), (Events{Rejected{1, Command::New, RejectReason::DuplicateOrderId}}));
}

TEST(Cxl, R_VAL_07_CanceledOrdersCountTowardCapacity) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 1});
  h.buy(1, 100, 10);
  h.cancel(1);
  EXPECT_EQ(h.buy(2, 100, 10), (Events{Rejected{2, Command::New, RejectReason::CapacityExceeded}}));
}

// A canceled slot is reused by a later order without disturbing the book.
TEST(Cxl, SlotReuseAfterCancel) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 3});
  h.buy(1, 100, 10);
  h.cancel(1);
  EXPECT_EQ(h.buy(2, 100, 20), (Events{Accepted{2}}));
  EXPECT_EQ(h.sell(3, 101, 30), (Events{Accepted{3}}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 20}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{101, 30}));
}

} // namespace
