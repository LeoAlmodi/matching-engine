// T-02: types, config and events (definitions only).
// Covers R-CMD-02, R-EVT-01, R-EVT-02.
#include "me/config.hpp"
#include "me/events.hpp"
#include "me/types.hpp"
#include "support/recording_sink.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <type_traits>
#include <vector>

namespace {

using me::test::RecordingSink;

// R-CMD-02: order_id is unsigned 64-bit; price and quantity are signed 64-bit.
static_assert(std::is_same_v<me::OrderId, std::uint64_t>);
static_assert(std::is_same_v<me::Price, std::int64_t>);
static_assert(std::is_same_v<me::Qty, std::int64_t>);

static_assert(me::EventSink<RecordingSink>);

// A sink missing one callback does not satisfy the concept.
struct NoModifiedSink {
  void on_accepted(me::OrderId) {}
  void on_rejected(me::OrderId, me::Command, me::RejectReason) {}
  void on_trade(me::OrderId, me::OrderId, me::Price, me::Qty, me::Qty, me::Qty) {}
  void on_canceled(me::OrderId, me::Qty, me::CancelReason) {}
};
static_assert(!me::EventSink<NoModifiedSink>);

TEST(Types, R_EVT_02_RejectReasonsAreDistinct) {
  const std::vector<me::RejectReason> reasons = {
      me::RejectReason::InvalidQuantity,  me::RejectReason::InvalidPrice,
      me::RejectReason::DuplicateOrderId, me::RejectReason::CapacityExceeded,
      me::RejectReason::NoLiquidity,      me::RejectReason::UnknownOrder,
  };
  EXPECT_EQ(std::set<me::RejectReason>(reasons.begin(), reasons.end()).size(), 6u);
}

TEST(Types, R_EVT_01_CancelReasonsAndCommandsAreDistinct) {
  EXPECT_NE(me::CancelReason::User, me::CancelReason::MarketRemainder);
  const std::vector<me::Command> commands = {me::Command::New, me::Command::Cancel,
                                             me::Command::Modify};
  EXPECT_EQ(std::set<me::Command>(commands.begin(), commands.end()).size(), 3u);
}

TEST(Types, R_CMD_02_ExtremeValuesAreRepresentable) {
  const me::OrderId max_id = UINT64_MAX;
  const me::Price negative_price = -1;
  const me::Qty negative_qty = INT64_MIN;
  RecordingSink sink;
  sink.on_rejected(max_id, me::Command::New, me::RejectReason::InvalidPrice);
  sink.on_modified(0, negative_price, negative_qty);
  const std::vector<me::test::Event> expected = {
      me::Rejected{max_id, me::Command::New, me::RejectReason::InvalidPrice},
      me::Modified{0, -1, INT64_MIN},
  };
  EXPECT_EQ(sink.events, expected);
}

TEST(RecordingSink, R_EVT_01_RecordsEachEventTypeWithAllFields) {
  RecordingSink sink;
  sink.on_accepted(1);
  sink.on_rejected(2, me::Command::Cancel, me::RejectReason::UnknownOrder);
  sink.on_trade(3, 4, 100, 5, 6, 7);
  sink.on_canceled(8, 9, me::CancelReason::MarketRemainder);
  sink.on_modified(10, 101, 11);

  const std::vector<me::test::Event> expected = {
      me::Accepted{1},
      me::Rejected{2, me::Command::Cancel, me::RejectReason::UnknownOrder},
      me::Trade{3, 4, 100, 5, 6, 7},
      me::Canceled{8, 9, me::CancelReason::MarketRemainder},
      me::Modified{10, 101, 11},
  };
  EXPECT_EQ(sink.events, expected);
}

TEST(RecordingSink, R_EVT_01_EqualityComparesEveryField) {
  const me::Trade base{3, 4, 100, 5, 6, 7};
  EXPECT_EQ(base, (me::Trade{3, 4, 100, 5, 6, 7}));
  EXPECT_NE(base, (me::Trade{9, 4, 100, 5, 6, 7}));
  EXPECT_NE(base, (me::Trade{3, 9, 100, 5, 6, 7}));
  EXPECT_NE(base, (me::Trade{3, 4, 999, 5, 6, 7}));
  EXPECT_NE(base, (me::Trade{3, 4, 100, 9, 6, 7}));
  EXPECT_NE(base, (me::Trade{3, 4, 100, 5, 9, 7}));
  EXPECT_NE(base, (me::Trade{3, 4, 100, 5, 6, 9}));

  EXPECT_NE((me::Rejected{1, me::Command::New, me::RejectReason::InvalidPrice}),
            (me::Rejected{1, me::Command::Modify, me::RejectReason::InvalidPrice}));
  EXPECT_NE((me::Canceled{1, 2, me::CancelReason::User}),
            (me::Canceled{1, 2, me::CancelReason::MarketRemainder}));
  EXPECT_NE((me::Modified{1, 2, 3}), (me::Modified{1, 2, 4}));

  // Different event types never compare equal.
  const me::test::Event accepted = me::Accepted{1};
  const me::test::Event canceled = me::Canceled{1, 0, me::CancelReason::User};
  EXPECT_NE(accepted, canceled);
}

// Plan §9: tests compare the exact event sequence, so order must be kept.
TEST(RecordingSink, PreservesEmissionOrder) {
  RecordingSink sink;
  sink.on_accepted(1);
  sink.on_accepted(2);
  const std::vector<me::test::Event> reversed = {me::Accepted{2}, me::Accepted{1}};
  EXPECT_NE(sink.events, reversed);
  ASSERT_EQ(sink.events.size(), 2u);
  EXPECT_EQ(sink.events[0], me::test::Event{me::Accepted{1}});
}

// Plan §4: Config only holds the limits; R-CFG-01 to R-CFG-03 validation is tested in T-06.
TEST(Config, HoldsLimits) {
  const me::Config cfg{100'000, 1'000'000, 1'000};
  EXPECT_EQ(cfg.max_price, 100'000);
  EXPECT_EQ(cfg.max_qty, 1'000'000);
  EXPECT_EQ(cfg.max_orders, 1'000u);
}

} // namespace
