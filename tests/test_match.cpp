// T-09: limit order matching. Covers R-MATCH-01 to R-MATCH-06, R-EVT-03, R-EVT-05, R-CMD-03,
// R-INV-01, R-INV-03, and R-VAL-04 / R-VAL-07 for filled orders; plan §8.2. Every command is
// followed by the ledger and check_invariants (see Harness).

#include "support/harness.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using me::Accepted;
using me::Command;
using me::Config;
using me::OrderId;
using me::Rejected;
using me::RejectReason;
using me::Side;
using me::Trade;
using me::test::Event;
using me::test::Harness;

using Events = std::vector<Event>;
using Ids = std::vector<OrderId>;
using LevelView = me::Engine<me::test::RecordingSink>::LevelView;
using OrderView = me::Engine<me::test::RecordingSink>::OrderView;

// ---- R-MATCH-01: matchable prices ---------------------------------------------------------------

// R-EVT-03: ACCEPTED, then the trade. R-EVT-05: leaves 0 means fully executed, so neither rests.
TEST(Match, R_MATCH_01_BuyCrossesAtExactPrice) {
  Harness h;
  h.sell(1, 100, 10);
  EXPECT_EQ(h.buy(2, 100, 10), (Events{Accepted{2}, Trade{2, 1, 100, 10, 0, 0}}));
  EXPECT_FALSE(h.engine.order_state(1).has_value());
  EXPECT_FALSE(h.engine.order_state(2).has_value());
  EXPECT_FALSE(h.engine.best_bid().has_value());
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

TEST(Match, R_MATCH_01_SellCrossesAtExactPrice) {
  Harness h;
  h.buy(1, 100, 10);
  EXPECT_EQ(h.sell(2, 100, 10), (Events{Accepted{2}, Trade{2, 1, 100, 10, 0, 0}}));
  EXPECT_FALSE(h.engine.best_bid().has_value());
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

TEST(Match, R_MATCH_01_NoTradeWhenNotMatchable) {
  Harness h;
  h.sell(1, 101, 10);
  EXPECT_EQ(h.buy(2, 100, 10), (Events{Accepted{2}}));
  h.buy(3, 99, 10);
  EXPECT_EQ(h.sell(4, 101, 10), (Events{Accepted{4}}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 10}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{101, 20}));
}

// ---- R-MATCH-03: trade at the passive price -----------------------------------------------------

TEST(Match, R_MATCH_03_BuyAboveBestAskTradesAtAskPrice) {
  Harness h;
  h.sell(1, 100, 5);
  EXPECT_EQ(h.buy(2, 105, 5), (Events{Accepted{2}, Trade{2, 1, 100, 5, 0, 0}}));
}

TEST(Match, R_MATCH_03_SellBelowBestBidTradesAtBidPrice) {
  Harness h;
  h.buy(1, 100, 5);
  EXPECT_EQ(h.sell(2, 95, 5), (Events{Accepted{2}, Trade{2, 1, 100, 5, 0, 0}}));
}

// ---- R-MATCH-04, R-MATCH-05: quantity and partial fills
// ------------------------------------------

// The aggressor is partially filled and its remainder rests at its own price (R-BOOK-01).
TEST(Match, R_MATCH_04_AggressorPartialFillRests) {
  Harness h;
  h.sell(1, 100, 4);
  EXPECT_EQ(h.buy(2, 102, 10), (Events{Accepted{2}, Trade{2, 1, 100, 4, 6, 0}}));
  EXPECT_EQ(h.engine.order_state(2), (OrderView{Side::Buy, 102, 6}));
  EXPECT_FALSE(h.engine.order_state(1).has_value());
  EXPECT_EQ(h.engine.best_bid(), (LevelView{102, 6}));
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

// The passive order is partially filled, keeps its leaves and its place at the head of the level.
TEST(Match, R_MATCH_05_PassivePartialFillKeepsPriority) {
  Harness h;
  h.sell(1, 100, 10);
  h.sell(2, 100, 10);
  EXPECT_EQ(h.buy(3, 100, 4), (Events{Accepted{3}, Trade{3, 1, 100, 4, 0, 6}}));
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Sell, 100, 6}));
  EXPECT_EQ(h.ids_at(Side::Sell, 100), (Ids{1, 2}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{100, 16}));
  EXPECT_FALSE(h.engine.order_state(3).has_value());
}

// ---- R-MATCH-02, R-MATCH-06: priority and one TRADE per match -----------------------------------

// R-INV-03: within a level, executions follow arrival order.
TEST(Match, R_MATCH_02_FifoWithinLevel) {
  Harness h;
  h.sell(1, 100, 3);
  h.sell(2, 100, 3);
  h.sell(3, 100, 3);
  EXPECT_EQ(h.buy(4, 100, 7), (Events{Accepted{4}, Trade{4, 1, 100, 3, 4, 0},
                                      Trade{4, 2, 100, 3, 1, 0}, Trade{4, 3, 100, 1, 0, 2}}));
  EXPECT_EQ(h.ids_at(Side::Sell, 100), (Ids{3}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{100, 2}));
}

TEST(Match, R_MATCH_02_BuySweepsSeveralLevels) {
  Harness h;
  h.sell(1, 100, 2);
  h.sell(2, 101, 3);
  h.sell(3, 103, 4);
  h.sell(4, 104, 5);
  EXPECT_EQ(h.buy(5, 103, 8), (Events{Accepted{5}, Trade{5, 1, 100, 2, 6, 0},
                                      Trade{5, 2, 101, 3, 3, 0}, Trade{5, 3, 103, 3, 0, 1}}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{103, 1}));
  EXPECT_EQ(h.engine.level_qty(Side::Sell, 104), 5);
  EXPECT_FALSE(h.engine.best_bid().has_value());
}

TEST(Match, R_MATCH_02_SellSweepsSeveralLevels) {
  Harness h;
  h.buy(1, 100, 2);
  h.buy(2, 99, 3);
  h.buy(3, 97, 4);
  EXPECT_EQ(h.sell(4, 97, 9), (Events{Accepted{4}, Trade{4, 1, 100, 2, 7, 0},
                                      Trade{4, 2, 99, 3, 4, 0}, Trade{4, 3, 97, 4, 0, 0}}));
  EXPECT_FALSE(h.engine.best_bid().has_value());
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

// The sweep stops at the aggressor's limit; the remainder rests and the book is not crossed
// (R-INV-01).
TEST(Match, R_MATCH_02_SweepStopsAtLimitAndRemainderRests) {
  Harness h;
  h.sell(1, 100, 2);
  h.sell(2, 101, 3);
  h.sell(3, 103, 4);
  EXPECT_EQ(h.buy(4, 101, 10),
            (Events{Accepted{4}, Trade{4, 1, 100, 2, 8, 0}, Trade{4, 2, 101, 3, 5, 0}}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{101, 5}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{103, 4}));
}

// The opposite side runs out before the aggressor is filled; a LIMIT remainder rests.
TEST(Match, R_MATCH_02_OppositeSideExhausted) {
  Harness h;
  h.buy(1, 100, 2);
  EXPECT_EQ(h.sell(2, 90, 5), (Events{Accepted{2}, Trade{2, 1, 100, 2, 3, 0}}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{90, 3}));
  EXPECT_FALSE(h.engine.best_bid().has_value());
}

// R-CMD-03: a later command sees the book left by the earlier one.
TEST(Match, R_CMD_03_CommandsApplyInOrder) {
  Harness h;
  h.sell(1, 100, 5);
  EXPECT_EQ(h.buy(2, 100, 3), (Events{Accepted{2}, Trade{2, 1, 100, 3, 0, 2}}));
  EXPECT_EQ(h.buy(3, 100, 3), (Events{Accepted{3}, Trade{3, 1, 100, 2, 1, 0}}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 1}));
}

// ---- R-VAL-04, R-VAL-07 for filled orders
// --------------------------------------------------------

TEST(Match, R_VAL_04_FilledIdIsDuplicate) {
  Harness h;
  h.sell(1, 100, 5);
  h.buy(2, 100, 5);
  EXPECT_EQ(h.buy(1, 100, 5), (Events{Rejected{1, Command::New, RejectReason::DuplicateOrderId}}));
  EXPECT_EQ(h.sell(2, 100, 5), (Events{Rejected{2, Command::New, RejectReason::DuplicateOrderId}}));
}

TEST(Match, R_VAL_07_FilledOrdersCountTowardCapacity) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 2});
  h.sell(1, 100, 5);
  h.buy(2, 100, 5); // both filled, book empty
  EXPECT_EQ(h.buy(3, 100, 5), (Events{Rejected{3, Command::New, RejectReason::CapacityExceeded}}));
}

} // namespace
