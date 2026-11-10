// T-05: BookSide and Level (plan §5.2, §5.3). Supports R-BOOK-02, R-QRY-01, R-QRY-02, R-INV-03 and
// R-INV-06.

#include "me/detail/book_side.hpp"
#include "me/detail/order_pool.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using me::Index;
using me::NIL;
using me::OrderId;
using me::Price;
using me::Qty;
using me::Side;
using me::detail::BookSide;
using me::detail::OrderPool;

constexpr Price kMaxPrice = 1000;

class BookSideTest : public ::testing::Test {
protected:
  OrderPool pool{64};
  BookSide bids{Side::Buy, kMaxPrice, pool};
  BookSide asks{Side::Sell, kMaxPrice, pool};

  Index add(BookSide &book, OrderId id, Price price, Qty leaves) {
    const Index idx = pool.alloc();
    auto &n = pool[idx];
    n.id = id;
    n.price = price;
    n.leaves = leaves;
    n.side = book.side();
    book.append(idx);
    return idx;
  }

  // Ids of the orders in one level, head to tail. Also checks that the backward links mirror the
  // forward ones.
  std::vector<OrderId> ids_at(const BookSide &book, Price price) {
    std::vector<OrderId> forward;
    Index prev = NIL;
    for (Index i = book.level(price).head; i != NIL; i = pool[i].next) {
      EXPECT_EQ(pool[i].prev, prev);
      forward.push_back(pool[i].id);
      prev = i;
    }
    EXPECT_EQ(book.level(price).tail, prev);
    return forward;
  }
};

using Ids = std::vector<OrderId>;

TEST_F(BookSideTest, EmptySide) {
  EXPECT_TRUE(bids.empty());
  EXPECT_EQ(bids.best(), 0);
  EXPECT_EQ(bids.front(), NIL);
  EXPECT_EQ(bids.level(100).total, 0); // R-QRY-02: 0 if no level exists
  EXPECT_EQ(bids.level(100).head, NIL);
  EXPECT_EQ(bids.level(100).tail, NIL);
  EXPECT_TRUE(asks.empty());
  EXPECT_EQ(asks.best(), 0);
  EXPECT_EQ(asks.front(), NIL);
}

// R-BOOK-02, R-INV-03: same price, earlier arrival first.
TEST_F(BookSideTest, R_BOOK_02_FifoWithinLevel) {
  const Index a = add(bids, 1, 100, 10);
  add(bids, 2, 100, 20);
  add(bids, 3, 100, 30);
  EXPECT_EQ(ids_at(bids, 100), (Ids{1, 2, 3}));
  EXPECT_EQ(bids.front(), a);
}

// R-QRY-02, R-INV-06: total equals the sum of leaves after every operation.
TEST_F(BookSideTest, R_INV_06_TotalTracksAppendAndUnlink) {
  const Index a = add(asks, 1, 200, 10);
  EXPECT_EQ(asks.level(200).total, 10);
  const Index b = add(asks, 2, 200, 25);
  EXPECT_EQ(asks.level(200).total, 35);
  const Index c = add(asks, 3, 200, 5);
  EXPECT_EQ(asks.level(200).total, 40);
  asks.unlink(b);
  EXPECT_EQ(asks.level(200).total, 15);
  asks.unlink(a);
  EXPECT_EQ(asks.level(200).total, 5);
  asks.unlink(c);
  EXPECT_EQ(asks.level(200).total, 0);
}

TEST_F(BookSideTest, UnlinkHead) {
  const Index a = add(bids, 1, 100, 10);
  const Index b = add(bids, 2, 100, 20);
  add(bids, 3, 100, 30);
  bids.unlink(a);
  EXPECT_EQ(ids_at(bids, 100), (Ids{2, 3}));
  EXPECT_EQ(bids.front(), b);
  EXPECT_EQ(bids.level(100).total, 50);
}

TEST_F(BookSideTest, UnlinkMiddle) {
  add(bids, 1, 100, 10);
  const Index b = add(bids, 2, 100, 20);
  add(bids, 3, 100, 30);
  bids.unlink(b);
  EXPECT_EQ(ids_at(bids, 100), (Ids{1, 3}));
  EXPECT_EQ(bids.level(100).total, 40);
}

TEST_F(BookSideTest, UnlinkTail) {
  add(bids, 1, 100, 10);
  add(bids, 2, 100, 20);
  const Index c = add(bids, 3, 100, 30);
  bids.unlink(c);
  EXPECT_EQ(ids_at(bids, 100), (Ids{1, 2}));
  EXPECT_EQ(bids.level(100).total, 30);
  add(bids, 4, 100, 40); // the new tail links after the old second order
  EXPECT_EQ(ids_at(bids, 100), (Ids{1, 2, 4}));
}

TEST_F(BookSideTest, UnlinkOnlyOrderEmptiesLevelAndSide) {
  const Index a = add(asks, 1, 300, 10);
  EXPECT_FALSE(asks.empty());
  asks.unlink(a);
  EXPECT_EQ(ids_at(asks, 300), Ids{});
  EXPECT_EQ(asks.level(300).head, NIL);
  EXPECT_EQ(asks.level(300).tail, NIL);
  EXPECT_TRUE(asks.empty());
  EXPECT_EQ(asks.best(), 0);
  EXPECT_EQ(asks.front(), NIL);
}

// A re-appended order goes to the tail, which is how a modification loses priority (R-MOD-05).
TEST_F(BookSideTest, ReappendGoesToTail) {
  const Index a = add(bids, 1, 100, 10);
  add(bids, 2, 100, 20);
  bids.unlink(a);
  bids.append(a);
  EXPECT_EQ(ids_at(bids, 100), (Ids{2, 1}));
  EXPECT_EQ(bids.level(100).total, 30);
}

// R-BOOK-02, R-QRY-01: higher price is better for BUY.
TEST_F(BookSideTest, R_BOOK_02_BuyBestIsHighestPrice) {
  add(bids, 1, 100, 10);
  EXPECT_EQ(bids.best(), 100);
  const Index b = add(bids, 2, 105, 10);
  EXPECT_EQ(bids.best(), 105);
  add(bids, 3, 95, 10);
  EXPECT_EQ(bids.best(), 105);
  EXPECT_EQ(bids.front(), b);
}

// R-BOOK-02, R-QRY-01: lower price is better for SELL.
TEST_F(BookSideTest, R_BOOK_02_SellBestIsLowestPrice) {
  add(asks, 1, 100, 10);
  EXPECT_EQ(asks.best(), 100);
  const Index b = add(asks, 2, 95, 10);
  EXPECT_EQ(asks.best(), 95);
  add(asks, 3, 105, 10);
  EXPECT_EQ(asks.best(), 95);
  EXPECT_EQ(asks.front(), b);
}

TEST_F(BookSideTest, BuyBestScansDownAcrossGapsWhenBestLevelEmpties) {
  const Index a = add(bids, 1, 500, 10);
  const Index b = add(bids, 2, 420, 10);
  const Index c = add(bids, 3, 7, 10);
  bids.unlink(a);
  EXPECT_EQ(bids.best(), 420);
  EXPECT_EQ(bids.front(), b);
  bids.unlink(b);
  EXPECT_EQ(bids.best(), 7);
  EXPECT_EQ(bids.front(), c);
  bids.unlink(c);
  EXPECT_TRUE(bids.empty());
  EXPECT_EQ(bids.best(), 0);
}

TEST_F(BookSideTest, SellBestScansUpAcrossGapsWhenBestLevelEmpties) {
  const Index a = add(asks, 1, 7, 10);
  const Index b = add(asks, 2, 420, 10);
  const Index c = add(asks, 3, 999, 10);
  asks.unlink(a);
  EXPECT_EQ(asks.best(), 420);
  EXPECT_EQ(asks.front(), b);
  asks.unlink(b);
  EXPECT_EQ(asks.best(), 999);
  EXPECT_EQ(asks.front(), c);
  asks.unlink(c);
  EXPECT_TRUE(asks.empty());
  EXPECT_EQ(asks.best(), 0);
}

TEST_F(BookSideTest, BestUnchangedWhileBestLevelStillHasOrders) {
  const Index a = add(bids, 1, 200, 10);
  const Index b = add(bids, 2, 200, 20);
  add(bids, 3, 150, 30);
  bids.unlink(a);
  EXPECT_EQ(bids.best(), 200);
  EXPECT_EQ(bids.front(), b);
}

TEST_F(BookSideTest, UnlinkingNonBestLevelKeepsBest) {
  add(asks, 1, 100, 10);
  const Index b = add(asks, 2, 300, 10);
  asks.unlink(b);
  EXPECT_EQ(asks.best(), 100);
  EXPECT_FALSE(asks.empty());
}

// Valid prices are 1..MAX_PRICE (R-CFG-01, D-17); both ends must work as levels.
TEST_F(BookSideTest, ExtremePrices) {
  const Index top = add(bids, 1, kMaxPrice, 10);
  add(bids, 2, 1, 20);
  EXPECT_EQ(bids.best(), kMaxPrice);
  bids.unlink(top);
  EXPECT_EQ(bids.best(), 1);
  EXPECT_EQ(bids.level(1).total, 20);

  const Index low = add(asks, 3, 1, 10);
  add(asks, 4, kMaxPrice, 20);
  EXPECT_EQ(asks.best(), 1);
  asks.unlink(low);
  EXPECT_EQ(asks.best(), kMaxPrice);
  EXPECT_EQ(asks.level(kMaxPrice).total, 20);
}

// Plan §8.2, §8.4: a partial fill or an in-place decrease reduces leaves and the level total
// together (R-INV-06), and the order keeps its position.
TEST_F(BookSideTest, R_INV_06_ReduceKeepsPositionAndTotal) {
  add(asks, 1, 100, 10);
  const Index b = add(asks, 2, 100, 20);
  add(asks, 3, 100, 30);
  asks.reduce(b, 15);
  EXPECT_EQ(pool[b].leaves, 5);
  EXPECT_EQ(asks.level(100).total, 45);
  EXPECT_EQ(ids_at(asks, 100), (Ids{1, 2, 3}));
  EXPECT_EQ(asks.best(), 100);
}

// A full fill reduces leaves to 0 first, then unlinks; the total must end at the sum of the rest.
TEST_F(BookSideTest, ReduceToZeroThenUnlink) {
  const Index a = add(bids, 1, 100, 10);
  const Index b = add(bids, 2, 100, 20);
  bids.reduce(a, 10);
  EXPECT_EQ(pool[a].leaves, 0);
  EXPECT_EQ(bids.level(100).total, 20);
  bids.unlink(a);
  EXPECT_EQ(bids.level(100).total, 20);
  EXPECT_EQ(bids.front(), b);
}

TEST_F(BookSideTest, SidesAreIndependent) {
  add(bids, 1, 100, 10);
  add(asks, 2, 100, 20);
  EXPECT_EQ(ids_at(bids, 100), (Ids{1}));
  EXPECT_EQ(ids_at(asks, 100), (Ids{2}));
  EXPECT_EQ(bids.level(100).total, 10);
  EXPECT_EQ(asks.level(100).total, 20);
}

} // namespace
