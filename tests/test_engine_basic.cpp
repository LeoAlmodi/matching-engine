// T-06: Engine skeleton, queries and test support. Covers R-CFG-01 to R-CFG-03, R-CMD-01,
// R-QRY-01 to R-QRY-03, and the test support for R-INV-01 to R-INV-06 (plan §7, §9).

#include "me/engine.hpp"
#include "support/invariants.hpp"
#include "support/ledger.hpp"
#include "support/recording_sink.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace {

using me::Accepted;
using me::Canceled;
using me::CancelReason;
using me::Command;
using me::Config;
using me::Modified;
using me::NIL;
using me::OrderId;
using me::OrderType;
using me::Price;
using me::Qty;
using me::Rejected;
using me::RejectReason;
using me::Side;
using me::Trade;
using me::test::check_invariants;
using me::test::Ledger;
using me::test::RecordingSink;

using TestEngine = me::Engine<RecordingSink>;

constexpr Config kConfig{.max_price = 1000, .max_qty = 500, .max_orders = 64};
constexpr OrderId kMaxId = std::numeric_limits<OrderId>::max();

// ---- Configuration -----------------------------------------------------------------------------

TEST(EngineConfig, AcceptsMinimalValidConfig) {
  RecordingSink sink;
  EXPECT_NO_THROW(TestEngine(Config{.max_price = 1, .max_qty = 1, .max_orders = 1}, sink));
}

TEST(EngineConfig, R_CFG_01_RejectsMaxPriceBelowOne) {
  RecordingSink sink;
  EXPECT_THROW(TestEngine(Config{.max_price = 0, .max_qty = 1, .max_orders = 1}, sink),
               std::invalid_argument);
  EXPECT_THROW(TestEngine(Config{.max_price = -5, .max_qty = 1, .max_orders = 1}, sink),
               std::invalid_argument);
}

TEST(EngineConfig, R_CFG_02_RejectsMaxQtyBelowOne) {
  RecordingSink sink;
  EXPECT_THROW(TestEngine(Config{.max_price = 1, .max_qty = 0, .max_orders = 1}, sink),
               std::invalid_argument);
  EXPECT_THROW(TestEngine(Config{.max_price = 1, .max_qty = -5, .max_orders = 1}, sink),
               std::invalid_argument);
}

TEST(EngineConfig, R_CFG_03_RejectsMaxOrdersBelowOne) {
  RecordingSink sink;
  EXPECT_THROW(TestEngine(Config{.max_price = 1, .max_qty = 1, .max_orders = 0}, sink),
               std::invalid_argument);
}

// Plan §4: max_orders < NIL - 1, so pool indices never collide with the IdMap sentinels.
TEST(EngineConfig, R_CFG_03_RejectsMaxOrdersThatCollideWithSentinels) {
  RecordingSink sink;
  EXPECT_THROW(TestEngine(Config{.max_price = 1, .max_qty = 1, .max_orders = NIL - 1}, sink),
               std::invalid_argument);
  EXPECT_THROW(TestEngine(Config{.max_price = 1, .max_qty = 1, .max_orders = NIL}, sink),
               std::invalid_argument);
}

TEST(EngineConfig, ExposesConfig) {
  RecordingSink sink;
  TestEngine engine(kConfig, sink);
  EXPECT_EQ(engine.config().max_price, kConfig.max_price);
  EXPECT_EQ(engine.config().max_qty, kConfig.max_qty);
  EXPECT_EQ(engine.config().max_orders, kConfig.max_orders);
}

// ---- Commands ----------------------------------------------------------------------------------

// R-CMD-01: the three commands with the parameters of R-CMD-02. Behavior is covered by T-07 to
// T-12.
static_assert(requires(TestEngine &e, OrderId id, Side s, OrderType t, Price p, Qty q) {
  e.new_order(id, s, t, p, q);
  e.cancel(id);
  e.modify(id, p, q);
});

// ---- Queries on an empty book ------------------------------------------------------------------

class EmptyEngine : public ::testing::Test {
protected:
  RecordingSink sink;
  TestEngine engine{kConfig, sink};
  Ledger ledger;
};

TEST_F(EmptyEngine, R_QRY_01_BestBidAndAskAreEmpty) {
  EXPECT_FALSE(engine.best_bid().has_value());
  EXPECT_FALSE(engine.best_ask().has_value());
}

TEST_F(EmptyEngine, R_QRY_02_LevelQtyIsZeroForEveryPrice) {
  for (Side side : {Side::Buy, Side::Sell}) {
    EXPECT_EQ(engine.level_qty(side, 1), 0);
    EXPECT_EQ(engine.level_qty(side, 500), 0);
    EXPECT_EQ(engine.level_qty(side, kConfig.max_price), 0);
    // Outside 1..MAX_PRICE no level can exist.
    EXPECT_EQ(engine.level_qty(side, 0), 0);
    EXPECT_EQ(engine.level_qty(side, -1), 0);
    EXPECT_EQ(engine.level_qty(side, kConfig.max_price + 1), 0);
    EXPECT_EQ(engine.level_qty(side, std::numeric_limits<Price>::max()), 0);
    EXPECT_EQ(engine.level_qty(side, std::numeric_limits<Price>::min()), 0);
  }
}

TEST_F(EmptyEngine, R_QRY_03_NoOrderIsResting) {
  EXPECT_FALSE(engine.order_state(0).has_value());
  EXPECT_FALSE(engine.order_state(1).has_value());
  EXPECT_FALSE(engine.order_state(kMaxId).has_value());
}

TEST_F(EmptyEngine, VisitLevelVisitsNothing) {
  int calls = 0;
  for (Side side : {Side::Buy, Side::Sell}) {
    for (Price p : {Price{0}, Price{1}, Price{500}, kConfig.max_price, kConfig.max_price + 1}) {
      engine.visit_level(side, p, [&](OrderId, Qty) { ++calls; });
    }
  }
  EXPECT_EQ(calls, 0);
}

TEST_F(EmptyEngine, InvariantsHold) { EXPECT_TRUE(check_invariants(engine, ledger)); }

// The checker must catch a book that disagrees with the ledger (R-INV-02): here the ledger has an
// accepted order with leaves that the engine does not have.
TEST_F(EmptyEngine, InvariantCheckerDetectsMissingRestingOrder) {
  ledger.new_command(7, Side::Buy, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{7});
  EXPECT_FALSE(check_invariants(engine, ledger));
}

// ---- Ledger (R-INV-02 accounting, R-BOOK-03 arrival sequence) ----------------------------------

TEST(Ledger, AcceptedNewCreatesRecord) {
  Ledger ledger;
  ledger.new_command(1, Side::Sell, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{1});
  ASSERT_EQ(ledger.orders().count(1), 1u);
  const auto &r = ledger.orders().at(1);
  EXPECT_EQ(r.side, Side::Sell);
  EXPECT_EQ(r.type, OrderType::Limit);
  EXPECT_EQ(r.price, 100);
  EXPECT_EQ(r.requested, 10);
  EXPECT_EQ(r.executed, 0);
  EXPECT_EQ(r.canceled, 0);
  EXPECT_EQ(r.leaves(), 10);
  EXPECT_TRUE(ledger.errors().empty());
}

TEST(Ledger, RejectedNewCreatesNoRecord) {
  Ledger ledger;
  ledger.new_command(1, Side::Buy, OrderType::Limit, 0, 10);
  ledger.apply(Rejected{1, Command::New, RejectReason::InvalidPrice});
  EXPECT_TRUE(ledger.orders().empty());
  ledger.apply(Rejected{9, Command::Cancel, RejectReason::UnknownOrder});
  EXPECT_TRUE(ledger.orders().empty());
  EXPECT_TRUE(ledger.errors().empty());
}

TEST(Ledger, TradeAddsExecutedToBothOrders) {
  Ledger ledger;
  ledger.new_command(1, Side::Sell, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{1});
  ledger.new_command(2, Side::Buy, OrderType::Limit, 100, 4);
  ledger.apply(Accepted{2});
  ledger.apply(Trade{2, 1, 100, 4, 0, 6});
  EXPECT_EQ(ledger.orders().at(1).executed, 4);
  EXPECT_EQ(ledger.orders().at(1).leaves(), 6);
  EXPECT_EQ(ledger.orders().at(2).executed, 4);
  EXPECT_EQ(ledger.orders().at(2).leaves(), 0);
  EXPECT_TRUE(ledger.errors().empty());
}

TEST(Ledger, TradeWithWrongReportedLeavesIsAnError) {
  Ledger ledger;
  ledger.new_command(1, Side::Sell, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{1});
  ledger.new_command(2, Side::Buy, OrderType::Limit, 100, 4);
  ledger.apply(Accepted{2});
  ledger.apply(Trade{2, 1, 100, 4, 0, 7}); // passive leaves should be 6
  EXPECT_FALSE(ledger.errors().empty());
}

TEST(Ledger, CanceledRemovesAllLeaves) {
  Ledger ledger;
  ledger.new_command(1, Side::Buy, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{1});
  ledger.apply(Canceled{1, 10, CancelReason::User});
  EXPECT_EQ(ledger.orders().at(1).canceled, 10);
  EXPECT_EQ(ledger.orders().at(1).leaves(), 0);
  EXPECT_TRUE(ledger.errors().empty());
}

TEST(Ledger, PartialCancelIsAnError) {
  Ledger ledger;
  ledger.new_command(1, Side::Buy, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{1});
  ledger.apply(Canceled{1, 3, CancelReason::User});
  EXPECT_FALSE(ledger.errors().empty());
}

TEST(Ledger, MarketRemainderCanceled) {
  Ledger ledger;
  ledger.new_command(1, Side::Sell, OrderType::Limit, 100, 4);
  ledger.apply(Accepted{1});
  ledger.new_command(2, Side::Buy, OrderType::Market, 0, 10);
  ledger.apply(Accepted{2});
  ledger.apply(Trade{2, 1, 100, 4, 6, 0});
  ledger.apply(Canceled{2, 6, CancelReason::MarketRemainder});
  const auto &r = ledger.orders().at(2);
  EXPECT_EQ(r.type, OrderType::Market);
  EXPECT_EQ(r.requested, 10);
  EXPECT_EQ(r.executed, 4);
  EXPECT_EQ(r.canceled, 6);
  EXPECT_EQ(r.leaves(), 0);
  EXPECT_TRUE(ledger.errors().empty());
}

// R-MOD-06: a decrease adds to canceled.
TEST(Ledger, R_MOD_06_DecreaseAddsToCanceled) {
  Ledger ledger;
  ledger.new_command(1, Side::Buy, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{1});
  ledger.apply(Modified{1, 100, 7});
  const auto &r = ledger.orders().at(1);
  EXPECT_EQ(r.requested, 10);
  EXPECT_EQ(r.canceled, 3);
  EXPECT_EQ(r.leaves(), 7);
}

// R-MOD-06: an increase adds to requested.
TEST(Ledger, R_MOD_06_IncreaseAddsToRequested) {
  Ledger ledger;
  ledger.new_command(1, Side::Buy, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{1});
  ledger.apply(Modified{1, 100, 15});
  const auto &r = ledger.orders().at(1);
  EXPECT_EQ(r.requested, 15);
  EXPECT_EQ(r.canceled, 0);
  EXPECT_EQ(r.leaves(), 15);
}

// R-BOOK-03, R-MOD-05: arrival sequence follows acceptance, and is reassigned only when a
// modification loses priority.
TEST(Ledger, R_MOD_05_ArrivalSequence) {
  Ledger ledger;
  ledger.new_command(1, Side::Buy, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{1});
  ledger.new_command(2, Side::Buy, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{2});
  const auto seq = [&](OrderId id) { return ledger.orders().at(id).seq; };
  EXPECT_LT(seq(1), seq(2));

  ledger.apply(Modified{1, 100, 10}); // same price, same quantity: keeps priority (R-MOD-08)
  EXPECT_LT(seq(1), seq(2));
  ledger.apply(Modified{1, 100, 5}); // decrease: keeps priority
  EXPECT_LT(seq(1), seq(2));
  ledger.apply(Modified{1, 100, 6}); // increase: loses priority
  EXPECT_GT(seq(1), seq(2));
  ledger.apply(Modified{2, 101, 10}); // price change: loses priority
  EXPECT_GT(seq(2), seq(1));
  EXPECT_EQ(ledger.orders().at(2).price, 101);
}

TEST(Ledger, EventForUnknownOrderIsAnError) {
  Ledger ledger;
  ledger.apply(Canceled{5, 1, CancelReason::User});
  EXPECT_FALSE(ledger.errors().empty());
}

TEST(Ledger, AcceptedWithoutMatchingNewIsAnError) {
  Ledger ledger;
  ledger.new_command(1, Side::Buy, OrderType::Limit, 100, 10);
  ledger.apply(Accepted{2});
  EXPECT_FALSE(ledger.errors().empty());
}

} // namespace
