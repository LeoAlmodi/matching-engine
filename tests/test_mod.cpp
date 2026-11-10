// T-12: modification. Covers R-MOD-01 to R-MOD-08 and R-EVT-04; plan §8.4. Every command is
// followed by the ledger and check_invariants (see Harness), which verify the quantity accounting
// (R-MOD-06, R-INV-02) and the arrival order within levels (R-MOD-05, R-INV-03).

#include "support/harness.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace {

using me::Command;
using me::Modified;
using me::OrderId;
using me::OrderType;
using me::Price;
using me::Qty;
using me::Rejected;
using me::RejectReason;
using me::Side;
using me::Trade;
using me::test::Event;
using me::test::Harness;
using me::test::kDefaultConfig;

using Events = std::vector<Event>;
using Ids = std::vector<OrderId>;
using LevelView = me::Engine<me::test::RecordingSink>::LevelView;
using OrderView = me::Engine<me::test::RecordingSink>::OrderView;

Events rejected(OrderId id, RejectReason reason) {
  return Events{Rejected{id, Command::Modify, reason}};
}

// ---- R-MOD-01: unknown order --------------------------------------------------------------------

TEST(Mod, R_MOD_01_NeverAcceptedIsUnknown) {
  Harness h;
  EXPECT_EQ(h.modify(1, 100, 10), rejected(1, RejectReason::UnknownOrder));
}

TEST(Mod, R_MOD_01_FilledCanceledAndMarketAreUnknown) {
  Harness h;
  h.sell(1, 100, 5);
  h.buy(2, 100, 5); // both filled
  h.buy(3, 90, 5);
  h.cancel(3);
  h.sell(4, 100, 2);
  h.new_order(5, Side::Buy, OrderType::Market, 0, 4); // remainder canceled
  for (OrderId id : {1, 2, 3, 5}) {
    EXPECT_EQ(h.modify(id, 100, 10), rejected(id, RejectReason::UnknownOrder)) << "id " << id;
  }
  EXPECT_FALSE(h.engine.best_bid().has_value());
}

// ---- R-MOD-02, R-MOD-03: quantity and price -----------------------------------------------------

TEST(Mod, R_MOD_02_InvalidQuantityIsRejected) {
  Harness h;
  h.buy(1, 100, 10);
  for (Qty q : {Qty{0}, Qty{-1}, kDefaultConfig.max_qty + 1, std::numeric_limits<Qty>::min()}) {
    EXPECT_EQ(h.modify(1, 100, q), rejected(1, RejectReason::InvalidQuantity)) << "qty " << q;
  }
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Buy, 100, 10}));
}

TEST(Mod, R_MOD_03_InvalidPriceIsRejected) {
  Harness h;
  h.buy(1, 100, 10);
  for (Price p :
       {Price{0}, Price{-1}, kDefaultConfig.max_price + 1, std::numeric_limits<Price>::max()}) {
    EXPECT_EQ(h.modify(1, p, 10), rejected(1, RejectReason::InvalidPrice)) << "price " << p;
  }
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Buy, 100, 10}));
}

TEST(Mod, R_MOD_02_BoundsAreValid) {
  Harness h;
  h.buy(1, 100, 10);
  EXPECT_EQ(h.modify(1, 1, 1), (Events{Modified{1, 1, 1}}));
  EXPECT_EQ(h.modify(1, 1, kDefaultConfig.max_qty),
            (Events{Modified{1, 1, kDefaultConfig.max_qty}}));
  h.sell(2, kDefaultConfig.max_price, 1);
  EXPECT_EQ(h.modify(2, kDefaultConfig.max_price, 2),
            (Events{Modified{2, kDefaultConfig.max_price, 2}}));
}

// ---- R-MOD-04: precedence unknown, quantity, price ----------------------------------------------

TEST(Mod, R_MOD_04_Precedence) {
  Harness h;
  EXPECT_EQ(h.modify(1, 0, 0), rejected(1, RejectReason::UnknownOrder));
  h.buy(1, 100, 10);
  EXPECT_EQ(h.modify(1, 0, 0), rejected(1, RejectReason::InvalidQuantity));
  EXPECT_EQ(h.modify(1, 0, 5), rejected(1, RejectReason::InvalidPrice));
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Buy, 100, 10}));
}

// ---- R-MOD-05, R-MOD-08: priority ---------------------------------------------------------------

class ModLevel : public ::testing::Test {
protected:
  void SetUp() override {
    h.buy(1, 100, 10);
    h.buy(2, 100, 20);
    h.buy(3, 99, 30);
  }
  Harness h;
};

// R-EVT-04: a valid MODIFY emits MODIFIED with the new price and leaves.
TEST_F(ModLevel, R_MOD_05_DecreaseKeepsPriority) {
  EXPECT_EQ(h.modify(1, 100, 4), (Events{Modified{1, 100, 4}}));
  EXPECT_EQ(h.ids_at(Side::Buy, 100), (Ids{1, 2}));
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Buy, 100, 4}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 24}));
}

TEST_F(ModLevel, R_MOD_05_IncreaseLosesPriority) {
  EXPECT_EQ(h.modify(1, 100, 15), (Events{Modified{1, 100, 15}}));
  EXPECT_EQ(h.ids_at(Side::Buy, 100), (Ids{2, 1}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 35}));
}

TEST_F(ModLevel, R_MOD_05_PriceChangeLosesPriority) {
  EXPECT_EQ(h.modify(1, 99, 10), (Events{Modified{1, 99, 10}}));
  EXPECT_EQ(h.ids_at(Side::Buy, 99), (Ids{3, 1}));
  EXPECT_EQ(h.ids_at(Side::Buy, 100), (Ids{2}));
  EXPECT_EQ(h.engine.level_qty(Side::Buy, 99), 40);
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Buy, 99, 10}));
}

// Even a price change with a smaller quantity loses priority.
TEST_F(ModLevel, R_MOD_05_PriceChangeWithDecreaseLosesPriority) {
  EXPECT_EQ(h.modify(3, 100, 5), (Events{Modified{3, 100, 5}}));
  EXPECT_EQ(h.ids_at(Side::Buy, 100), (Ids{1, 2, 3}));
  EXPECT_EQ(h.engine.level_qty(Side::Buy, 99), 0);
}

// Moving the only order of the best level moves the best price.
TEST_F(ModLevel, R_MOD_05_MovingBestLevelUpdatesBest) {
  h.modify(1, 98, 10);
  h.modify(2, 98, 20);
  EXPECT_EQ(h.engine.best_bid(), (LevelView{99, 30}));
  h.modify(3, 101, 30);
  EXPECT_EQ(h.engine.best_bid(), (LevelView{101, 30}));
}

TEST_F(ModLevel, R_MOD_08_NoOpKeepsPriority) {
  EXPECT_EQ(h.modify(1, 100, 10), (Events{Modified{1, 100, 10}}));
  EXPECT_EQ(h.ids_at(Side::Buy, 100), (Ids{1, 2}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 30}));
}

// ---- R-MOD-07, R-EVT-04: a modify that crosses trades as an aggressor ---------------------------

TEST(Mod, R_MOD_07_CrossingModifyPartiallyFillsAndRests) {
  Harness h;
  h.sell(1, 100, 5);
  h.buy(2, 99, 8);
  EXPECT_EQ(h.modify(2, 100, 8), (Events{Modified{2, 100, 8}, Trade{2, 1, 100, 5, 3, 0}}));
  EXPECT_EQ(h.engine.order_state(2), (OrderView{Side::Buy, 100, 3}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 3}));
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

TEST(Mod, R_MOD_07_CrossingModifyFullyFills) {
  Harness h;
  h.sell(1, 100, 5);
  h.sell(2, 101, 5);
  h.buy(3, 99, 8);
  EXPECT_EQ(h.modify(3, 101, 8),
            (Events{Modified{3, 101, 8}, Trade{3, 1, 100, 5, 3, 0}, Trade{3, 2, 101, 3, 0, 2}}));
  EXPECT_FALSE(h.engine.order_state(3).has_value());
  EXPECT_FALSE(h.engine.best_bid().has_value());
  EXPECT_EQ(h.engine.best_ask(), (LevelView{101, 2}));
  // The fully filled order is gone: unknown to CANCEL and MODIFY, and its id stays used.
  EXPECT_EQ(h.cancel(3), (Events{Rejected{3, Command::Cancel, RejectReason::UnknownOrder}}));
  EXPECT_EQ(h.modify(3, 50, 1), rejected(3, RejectReason::UnknownOrder));
  EXPECT_EQ(h.buy(3, 50, 1), (Events{Rejected{3, Command::New, RejectReason::DuplicateOrderId}}));
}

TEST(Mod, R_MOD_07_SellCrossingModifyWithIncrease) {
  Harness h;
  h.buy(1, 100, 4);
  h.buy(2, 98, 4);
  h.sell(3, 101, 2);
  EXPECT_EQ(h.modify(3, 99, 6), (Events{Modified{3, 99, 6}, Trade{3, 1, 100, 4, 2, 0}}));
  EXPECT_EQ(h.engine.best_ask(), (LevelView{99, 2}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{98, 4}));
}

// ---- R-MOD-06: quantity accounting --------------------------------------------------------------

TEST(Mod, R_MOD_06_DecreaseAndIncreaseAccounting) {
  Harness h;
  h.buy(1, 100, 10);
  h.modify(1, 100, 4); // decrease by 6: canceled
  h.modify(1, 100, 9); // increase by 5: requested
  const auto &r = h.ledger.orders().at(1);
  EXPECT_EQ(r.requested, 15);
  EXPECT_EQ(r.canceled, 6);
  EXPECT_EQ(r.executed, 0);
  EXPECT_EQ(r.leaves(), 9);
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Buy, 100, 9}));
}

TEST(Mod, R_MOD_06_AccountingAfterPartialFill) {
  Harness h;
  h.sell(1, 100, 10);
  h.buy(2, 100, 4);    // order 1 executes 4, leaves 6
  h.modify(1, 100, 8); // leaves 6 -> 8: requested + 2
  h.modify(1, 101, 3); // leaves 8 -> 3: canceled + 5
  const auto &r = h.ledger.orders().at(1);
  EXPECT_EQ(r.requested, 12);
  EXPECT_EQ(r.executed, 4);
  EXPECT_EQ(r.canceled, 5);
  EXPECT_EQ(r.leaves(), 3);
  EXPECT_EQ(h.engine.order_state(1), (OrderView{Side::Sell, 101, 3}));
}

} // namespace
