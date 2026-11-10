// T-08: resting limit orders. Covers R-BOOK-01, R-BOOK-02, R-BOOK-03 and R-EVT-03 (ACCEPTED);
// plan §8.1 steps 3 and 5. Every command is followed by check_invariants (see Harness).

#include "support/harness.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using me::Accepted;
using me::OrderId;
using me::Side;
using me::test::Event;
using me::test::Harness;
using me::test::kDefaultConfig;

using Events = std::vector<Event>;
using Ids = std::vector<OrderId>;
using LevelView = me::Engine<me::test::RecordingSink>::LevelView;
using OrderView = me::Engine<me::test::RecordingSink>::OrderView;

// R-EVT-03, R-BOOK-01: an order that does not trade emits only ACCEPTED and rests with its full
// quantity.
TEST(Book, R_BOOK_01_BuyRestsAndEmitsAccepted) {
  Harness h;
  EXPECT_EQ(h.buy(1, 100, 10), (Events{Accepted{1}}));
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Buy, 100, 10}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 10}));
  EXPECT_FALSE(h.engine.best_ask().has_value());
  EXPECT_EQ(h.engine.level_qty(Side::Buy, 100), 10);
  EXPECT_EQ(h.engine.level_qty(Side::Sell, 100), 0);
}

TEST(Book, R_BOOK_01_SellRestsAndEmitsAccepted) {
  Harness h;
  EXPECT_EQ(h.sell(1, 200, 7), (Events{Accepted{1}}));
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Sell, 200, 7}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{200, 7}));
  EXPECT_FALSE(h.engine.best_bid().has_value());
}

// R-BOOK-02: higher price first for BUY.
TEST(Book, R_BOOK_02_BuyPricePriority) {
  Harness h;
  h.buy(1, 100, 10);
  h.buy(2, 102, 20);
  h.buy(3, 99, 30);
  EXPECT_EQ(h.engine.best_bid(), (LevelView{102, 20}));
  EXPECT_EQ(h.engine.level_qty(Side::Buy, 100), 10);
  EXPECT_EQ(h.engine.level_qty(Side::Buy, 99), 30);
}

// R-BOOK-02: lower price first for SELL.
TEST(Book, R_BOOK_02_SellPricePriority) {
  Harness h;
  h.sell(1, 200, 10);
  h.sell(2, 198, 20);
  h.sell(3, 205, 30);
  EXPECT_EQ(h.engine.best_ask(), (LevelView{198, 20}));
  EXPECT_EQ(h.engine.level_qty(Side::Sell, 200), 10);
  EXPECT_EQ(h.engine.level_qty(Side::Sell, 205), 30);
}

// R-BOOK-02, R-BOOK-03: same price, earlier acceptance first; the level total is the sum.
TEST(Book, R_BOOK_03_ArrivalOrderWithinLevel) {
  Harness h;
  h.sell(5, 150, 1);
  h.sell(3, 150, 2);
  h.sell(9, 150, 3);
  EXPECT_EQ(h.ids_at(Side::Sell, 150), (Ids{5, 3, 9}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{150, 6}));
}

TEST(Book, BothSidesRestWithoutCrossing) {
  Harness h;
  EXPECT_EQ(h.buy(1, 100, 10), (Events{Accepted{1}}));
  EXPECT_EQ(h.sell(2, 101, 20), (Events{Accepted{2}}));
  EXPECT_EQ(h.buy(3, 100, 5), (Events{Accepted{3}}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 15}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{101, 20}));
  EXPECT_EQ(h.ids_at(Side::Buy, 100), (Ids{1, 3}));
}

// Valid extremes: price 1 and MAX_PRICE, quantity 1 and MAX_QTY.
TEST(Book, R_BOOK_01_ExtremeValuesRest) {
  Harness h;
  h.buy(1, 1, kDefaultConfig.max_qty);
  h.sell(2, kDefaultConfig.max_price, 1);
  EXPECT_EQ(h.engine.best_bid(), (LevelView{1, kDefaultConfig.max_qty}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{kDefaultConfig.max_price, 1}));
}

TEST(Book, R_QRY_03_OtherIdsAreNotResting) {
  Harness h;
  h.buy(1, 100, 10);
  EXPECT_FALSE(h.engine.order_state(2).has_value());
}

} // namespace
