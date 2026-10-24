// T-03: OrderPool. Covers plan §5.1 (no spec requirement of its own).
#include "me/detail/order_pool.hpp"

#include <gtest/gtest.h>

#include <set>
#include <vector>

namespace {

using me::Index;
using me::NIL;
using me::detail::OrderPool;

TEST(OrderPool, AllocatesDistinctSlotsUpToCapacity) {
  OrderPool pool(4);
  EXPECT_EQ(pool.capacity(), 4u);
  std::set<Index> seen;
  for (int i = 0; i < 4; ++i) {
    const Index idx = pool.alloc();
    ASSERT_NE(idx, NIL);
    EXPECT_LT(idx, 4u);
    seen.insert(idx);
  }
  EXPECT_EQ(seen.size(), 4u);
}

TEST(OrderPool, AllocReturnsNilWhenExhausted) {
  OrderPool pool(2);
  pool.alloc();
  pool.alloc();
  EXPECT_EQ(pool.alloc(), NIL);
}

TEST(OrderPool, CapacityOfOne) {
  OrderPool pool(1);
  const Index idx = pool.alloc();
  EXPECT_EQ(idx, 0u);
  EXPECT_EQ(pool.alloc(), NIL);
  pool.free(idx);
  EXPECT_EQ(pool.alloc(), idx);
}

TEST(OrderPool, FreedSlotIsReused) {
  OrderPool pool(3);
  pool.alloc();
  const Index middle = pool.alloc();
  pool.alloc();
  pool.free(middle);
  EXPECT_EQ(pool.alloc(), middle);
  EXPECT_EQ(pool.alloc(), NIL);
}

TEST(OrderPool, SeveralFreedSlotsAreAllReused) {
  OrderPool pool(5);
  std::vector<Index> all;
  for (int i = 0; i < 5; ++i) {
    all.push_back(pool.alloc());
  }
  pool.free(all[0]);
  pool.free(all[2]);
  pool.free(all[4]);
  const std::set<Index> reused = {pool.alloc(), pool.alloc(), pool.alloc()};
  EXPECT_EQ(reused, (std::set<Index>{all[0], all[2], all[4]}));
  EXPECT_EQ(pool.alloc(), NIL);
}

TEST(OrderPool, NodeFieldsAreStoredByIndex) {
  OrderPool pool(2);
  const Index a = pool.alloc();
  const Index b = pool.alloc();
  pool[a] = {.id = 7, .price = 100, .leaves = 5, .prev = NIL, .next = b, .side = me::Side::Buy};
  pool[b] = {.id = 8, .price = 101, .leaves = 6, .prev = a, .next = NIL, .side = me::Side::Sell};

  const OrderPool &cpool = pool;
  EXPECT_EQ(cpool[a].id, 7u);
  EXPECT_EQ(cpool[a].price, 100);
  EXPECT_EQ(cpool[a].leaves, 5);
  EXPECT_EQ(cpool[a].next, b);
  EXPECT_EQ(cpool[a].side, me::Side::Buy);
  EXPECT_EQ(cpool[b].id, 8u);
  EXPECT_EQ(cpool[b].prev, a);
  EXPECT_EQ(cpool[b].side, me::Side::Sell);
}

// Plan §1, §5.1: everything is allocated in the constructor. Many alloc/free cycles must not
// change the capacity or move any node.
TEST(OrderPool, NoAllocationAfterConstruction) {
  constexpr Index kCap = 64;
  OrderPool pool(kCap);
  std::vector<const me::detail::OrderNode *> addresses;
  for (Index i = 0; i < kCap; ++i) {
    addresses.push_back(&pool[i]);
  }

  for (int round = 0; round < 1000; ++round) {
    std::vector<Index> taken;
    Index idx;
    while ((idx = pool.alloc()) != NIL) {
      taken.push_back(idx);
    }
    ASSERT_EQ(taken.size(), kCap);
    for (const Index t : taken) {
      pool.free(t);
    }
  }

  EXPECT_EQ(pool.capacity(), kCap);
  for (Index i = 0; i < kCap; ++i) {
    EXPECT_EQ(&pool[i], addresses[i]);
  }
}

} // namespace
