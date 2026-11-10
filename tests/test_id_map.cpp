// T-04: IdMap (plan §5.4). Supports R-VAL-04, R-CXL-02, R-MOD-01 and D-15.

#include "me/detail/id_map.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

using me::NIL;
using me::OrderId;
using me::detail::IdMap;
using me::detail::splitmix64;

namespace {

constexpr OrderId kMaxId = std::numeric_limits<OrderId>::max();

// Returns `count` distinct keys whose home slot is `slot` in a table of `capacity` entries.
std::vector<OrderId> keys_with_home_slot(std::size_t capacity, std::size_t slot, int count) {
  std::vector<OrderId> keys;
  for (OrderId k = 1; static_cast<int>(keys.size()) < count; ++k) {
    if ((splitmix64(k) & (capacity - 1)) == slot) {
      keys.push_back(k);
    }
  }
  return keys;
}

} // namespace

TEST(IdMap, SentinelsAreOutsideThePoolIndexRange) {
  // Config requires max_orders < NIL - 1, so every pool index is below both sentinels.
  EXPECT_EQ(IdMap::EMPTY, NIL);
  EXPECT_EQ(IdMap::NOT_RESTING, NIL - 1);
}

TEST(IdMap, SplitMix64MatchesReferenceOutput) {
  // First output of the reference splitmix64 generator seeded with 0 is the finalizer applied to
  // the golden gamma.
  EXPECT_EQ(splitmix64(0x9e3779b97f4a7c15ULL), 0xe220a8397b1dcdafULL);
}

TEST(IdMap, CapacityIsSmallestPowerOfTwoAtLeastTwiceMaxOrders) {
  EXPECT_EQ(IdMap(1).capacity(), 2u);
  EXPECT_EQ(IdMap(4).capacity(), 8u);
  EXPECT_EQ(IdMap(5).capacity(), 16u);
  EXPECT_EQ(IdMap(1000).capacity(), 2048u);
  EXPECT_EQ(IdMap(1024).capacity(), 2048u);
}

TEST(IdMap, FindOnEmptyMapReturnsEmpty) {
  IdMap map(16);
  EXPECT_EQ(map.find(1), IdMap::EMPTY);
  EXPECT_EQ(map.find(0), IdMap::EMPTY);
  EXPECT_EQ(map.find(kMaxId), IdMap::EMPTY);
}

// R-VAL-04: an accepted id is remembered.
TEST(IdMap, InsertThenFindReturnsValue) {
  IdMap map(16);
  map.insert(42, 7);
  EXPECT_EQ(map.find(42), 7u);
  EXPECT_EQ(map.find(43), IdMap::EMPTY);
}

// R-CXL-02, R-MOD-01, D-15: a filled or canceled order stays known but is no longer resting.
TEST(IdMap, UpdateToNotRestingKeepsIdKnown) {
  IdMap map(16);
  map.insert(42, 7);
  map.update(42, IdMap::NOT_RESTING);
  EXPECT_EQ(map.find(42), IdMap::NOT_RESTING);
}

TEST(IdMap, UpdateChangesPoolIndex) {
  IdMap map(16);
  map.insert(42, IdMap::NOT_RESTING);
  map.update(42, 3);
  EXPECT_EQ(map.find(42), 3u);
}

TEST(IdMap, ZeroAndMaxIdsAreOrdinaryKeys) {
  IdMap map(16);
  map.insert(0, 1);
  map.insert(kMaxId, 2);
  EXPECT_EQ(map.find(0), 1u);
  EXPECT_EQ(map.find(kMaxId), 2u);
  map.update(0, IdMap::NOT_RESTING);
  EXPECT_EQ(map.find(0), IdMap::NOT_RESTING);
  EXPECT_EQ(map.find(kMaxId), 2u);
}

TEST(IdMap, CollidingKeysAreAllFoundAndUpdatedIndependently) {
  IdMap map(16);
  const auto keys = keys_with_home_slot(map.capacity(), 5, 6);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    map.insert(keys[i], static_cast<std::uint32_t>(i));
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    EXPECT_EQ(map.find(keys[i]), i);
  }
  map.update(keys[2], IdMap::NOT_RESTING);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    EXPECT_EQ(map.find(keys[i]), i == 2 ? IdMap::NOT_RESTING : i);
  }
}

TEST(IdMap, ProbingWrapsAroundTheEndOfTheTable) {
  IdMap map(16);
  const std::size_t last = map.capacity() - 1;
  const auto keys = keys_with_home_slot(map.capacity(), last, 4);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    map.insert(keys[i], static_cast<std::uint32_t>(i));
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    EXPECT_EQ(map.find(keys[i]), i);
  }
  // A key that collides with the wrapped run but was never inserted is still absent.
  const auto more = keys_with_home_slot(map.capacity(), last, 5);
  EXPECT_EQ(map.find(more[4]), IdMap::EMPTY);
}

TEST(IdMap, SequentialIds) {
  constexpr std::uint32_t kMaxOrders = 10'000;
  IdMap map(kMaxOrders);
  for (std::uint32_t i = 0; i < kMaxOrders; ++i) {
    map.insert(1'000'000 + i, i);
  }
  for (std::uint32_t i = 0; i < kMaxOrders; ++i) {
    ASSERT_EQ(map.find(1'000'000 + i), i);
  }
  EXPECT_EQ(map.find(1'000'000 + kMaxOrders), IdMap::EMPTY);
}

// R-VAL-07 caps accepted orders at max_orders, so the map must hold exactly that many ids without
// growing.
TEST(IdMap, FillsToMaxOrdersWithoutGrowing) {
  constexpr std::uint32_t kMaxOrders = 1000;
  IdMap map(kMaxOrders);
  const std::size_t capacity = map.capacity();
  for (std::uint32_t i = 0; i < kMaxOrders; ++i) {
    map.insert(splitmix64(i) ^ 0x5555, i); // arbitrary spread-out ids
  }
  EXPECT_EQ(map.capacity(), capacity);
  for (std::uint32_t i = 0; i < kMaxOrders; ++i) {
    map.update(splitmix64(i) ^ 0x5555, IdMap::NOT_RESTING);
  }
  for (std::uint32_t i = 0; i < kMaxOrders; ++i) {
    ASSERT_EQ(map.find(splitmix64(i) ^ 0x5555), IdMap::NOT_RESTING);
  }
}
