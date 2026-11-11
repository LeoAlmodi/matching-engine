#pragma once

// Plan §6: benchmark sink. Only counts events, so the compiler cannot discard the engine's work,
// at the cost of one increment per event.

#include "me/types.hpp"

#include <cstdint>

namespace me::bench {

struct CountingSink {
  std::uint64_t accepted = 0, rejected = 0, trades = 0, canceled = 0, modified = 0;

  void on_accepted(OrderId) { ++accepted; }
  void on_rejected(OrderId, Command, RejectReason) { ++rejected; }
  void on_trade(OrderId, OrderId, Price, Qty, Qty, Qty) { ++trades; }
  void on_canceled(OrderId, Qty, CancelReason) { ++canceled; }
  void on_modified(OrderId, Price, Qty) { ++modified; }
};

} // namespace me::bench
