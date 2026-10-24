#pragma once

#include <cstdint>
#include <limits>

namespace me {

// R-CMD-02
using Price = std::int64_t; // ticks
using Qty = std::int64_t;
using OrderId = std::uint64_t;

// Position in the order pool. Links between orders are indices, not pointers.
using Index = std::uint32_t;
inline constexpr Index NIL = std::numeric_limits<Index>::max();

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

// R-EVT-01: the command a REJECTED event refers to.
enum class Command : std::uint8_t { New, Cancel, Modify };

// R-EVT-02
enum class RejectReason : std::uint8_t {
  InvalidQuantity,
  InvalidPrice,
  DuplicateOrderId,
  CapacityExceeded,
  NoLiquidity,
  UnknownOrder,
};

// R-EVT-01: reasons carried by CANCELED.
enum class CancelReason : std::uint8_t { User, MarketRemainder };

} // namespace me
