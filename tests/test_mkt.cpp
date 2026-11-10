// T-10: market orders. Covers R-MKT-01 to R-MKT-03, R-VAL-03 and R-BOOK-04; plan §8.1 steps 2 and
// 5. Every command is followed by the ledger and check_invariants (see Harness), which also checks
// that no MARKET order rests (R-INV-05).

#include "support/harness.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace {

using me::Accepted;
using me::Canceled;
using me::CancelReason;
using me::Command;
using me::Config;
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
using LevelView = me::Engine<me::test::RecordingSink>::LevelView;

Events market(Harness &h, OrderId id, Side side, Qty qty, Price price = 0) {
  return h.new_order(id, side, OrderType::Market, price, qty);
}

Events rejected(OrderId id, RejectReason reason) {
  return Events{Rejected{id, Command::New, reason}};
}

// ---- R-MKT-01: matchable against any resting price ----------------------------------------------

TEST(Mkt, R_MKT_01_FullFill) {
  Harness h;
  h.sell(1, 100, 10);
  EXPECT_EQ(market(h, 2, Side::Buy, 10), (Events{Accepted{2}, Trade{2, 1, 100, 10, 0, 0}}));
  EXPECT_FALSE(h.engine.order_state(2).has_value());
  EXPECT_FALSE(h.engine.best_ask().has_value());
  EXPECT_FALSE(h.engine.best_bid().has_value());
}

TEST(Mkt, R_MKT_01_BuySweepsEveryLevelUpToMaxPrice) {
  Harness h;
  h.sell(1, 100, 2);
  h.sell(2, 150, 3);
  h.sell(3, kDefaultConfig.max_price, 4);
  EXPECT_EQ(market(h, 4, Side::Buy, 9),
            (Events{Accepted{4}, Trade{4, 1, 100, 2, 7, 0}, Trade{4, 2, 150, 3, 4, 0},
                    Trade{4, 3, kDefaultConfig.max_price, 4, 0, 0}}));
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

TEST(Mkt, R_MKT_01_SellSweepsEveryLevelDownToPriceOne) {
  Harness h;
  h.buy(1, 500, 2);
  h.buy(2, 1, 3);
  EXPECT_EQ(market(h, 3, Side::Sell, 4),
            (Events{Accepted{3}, Trade{3, 1, 500, 2, 2, 0}, Trade{3, 2, 1, 2, 0, 1}}));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{1, 1}));
}

// ---- R-MKT-03, R-BOOK-04: the remainder is canceled, never rests --------------------------------

TEST(Mkt, R_MKT_03_RemainderIsCanceled) {
  Harness h;
  h.sell(1, 100, 4);
  EXPECT_EQ(market(h, 2, Side::Buy, 10), (Events{Accepted{2}, Trade{2, 1, 100, 4, 6, 0},
                                                 Canceled{2, 6, CancelReason::MarketRemainder}}));
  EXPECT_FALSE(h.engine.order_state(2).has_value());
  EXPECT_FALSE(h.engine.best_bid().has_value());
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

TEST(Mkt, R_BOOK_04_SellRemainderAcrossLevelsIsCanceled) {
  Harness h;
  h.buy(1, 100, 1);
  h.buy(2, 90, 1);
  EXPECT_EQ(market(h, 3, Side::Sell, 5),
            (Events{Accepted{3}, Trade{3, 1, 100, 1, 4, 0}, Trade{3, 2, 90, 1, 3, 0},
                    Canceled{3, 3, CancelReason::MarketRemainder}}));
  EXPECT_FALSE(h.engine.best_ask().has_value());
}

// ---- R-MKT-02: no liquidity ---------------------------------------------------------------------

TEST(Mkt, R_MKT_02_EmptyBookIsNoLiquidity) {
  Harness h;
  EXPECT_EQ(market(h, 1, Side::Buy, 10), rejected(1, RejectReason::NoLiquidity));
  EXPECT_EQ(market(h, 2, Side::Sell, 10), rejected(2, RejectReason::NoLiquidity));
}

// Only the opposite side matters; the same side is left unchanged.
TEST(Mkt, R_MKT_02_EmptyOppositeSideIsNoLiquidity) {
  Harness h;
  h.buy(1, 100, 10);
  EXPECT_EQ(market(h, 2, Side::Buy, 10), rejected(2, RejectReason::NoLiquidity));
  EXPECT_EQ(h.engine.best_bid(), (LevelView{100, 10}));
  EXPECT_FALSE(h.engine.order_state(2).has_value());
}

// No state changes: the id is not used and no capacity is consumed.
TEST(Mkt, R_MKT_02_NoLiquidityChangesNoState) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 1});
  EXPECT_EQ(market(h, 1, Side::Buy, 10), rejected(1, RejectReason::NoLiquidity));
  EXPECT_EQ(h.sell(1, 100, 10), (Events{Accepted{1}}));
}

// R-MKT-02 runs after the R-VAL-05 checks.
TEST(Mkt, R_MKT_02_ValidationComesFirst) {
  Harness h(Config{.max_price = 1000, .max_qty = 500, .max_orders = 1});
  EXPECT_EQ(market(h, 1, Side::Buy, 0), rejected(1, RejectReason::InvalidQuantity));
  h.buy(2, 100, 10); // bids only; asks stay empty
  EXPECT_EQ(market(h, 2, Side::Buy, 10), rejected(2, RejectReason::DuplicateOrderId));
  EXPECT_EQ(market(h, 3, Side::Buy, 10), rejected(3, RejectReason::CapacityExceeded));
}

// ---- R-VAL-03: the price of a MARKET order is ignored -------------------------------------------

TEST(Mkt, R_VAL_03_PriceIsIgnored) {
  OrderId id = 100;
  for (Price p : {Price{0}, Price{-1}, Price{1}, Price{50}, kDefaultConfig.max_price + 1,
                  std::numeric_limits<Price>::min(), std::numeric_limits<Price>::max()}) {
    Harness h;
    h.sell(1, 200, 5);
    h.buy(2, 100, 5);
    EXPECT_EQ(market(h, id, Side::Buy, 5, p), (Events{Accepted{id}, Trade{id, 1, 200, 5, 0, 0}}))
        << "price " << p;
    EXPECT_EQ(market(h, id + 1, Side::Sell, 5, p),
              (Events{Accepted{id + 1}, Trade{id + 1, 2, 100, 5, 0, 0}}))
        << "price " << p;
    id += 2;
  }
}

// A market order's id stays used after it is filled or its remainder is canceled (R-VAL-04).
TEST(Mkt, R_VAL_04_MarketIdIsDuplicateAfterwards) {
  Harness h;
  h.sell(1, 100, 4);
  market(h, 2, Side::Buy, 10);
  EXPECT_EQ(h.sell(2, 100, 1), rejected(2, RejectReason::DuplicateOrderId));
}

} // namespace
