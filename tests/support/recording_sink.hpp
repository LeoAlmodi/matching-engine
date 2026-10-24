#pragma once

#include "me/events.hpp"

#include <ostream>
#include <variant>
#include <vector>

namespace me {

// gtest printers, so a failed comparison shows the events instead of raw bytes.
inline void PrintTo(const Accepted &e, std::ostream *os) {
  *os << "Accepted{id=" << e.order_id << "}";
}
inline void PrintTo(const Rejected &e, std::ostream *os) {
  *os << "Rejected{id=" << e.order_id << ", command=" << static_cast<int>(e.command)
      << ", reason=" << static_cast<int>(e.reason) << "}";
}
inline void PrintTo(const Trade &e, std::ostream *os) {
  *os << "Trade{aggressor=" << e.aggressor_id << ", passive=" << e.passive_id
      << ", price=" << e.price << ", qty=" << e.quantity
      << ", aggressor_leaves=" << e.aggressor_leaves << ", passive_leaves=" << e.passive_leaves
      << "}";
}
inline void PrintTo(const Canceled &e, std::ostream *os) {
  *os << "Canceled{id=" << e.order_id << ", qty=" << e.canceled_quantity
      << ", reason=" << static_cast<int>(e.reason) << "}";
}
inline void PrintTo(const Modified &e, std::ostream *os) {
  *os << "Modified{id=" << e.order_id << ", price=" << e.price << ", leaves=" << e.leaves << "}";
}

namespace test {

using Event = std::variant<Accepted, Rejected, Trade, Canceled, Modified>;

// Stores every event in emission order. Allocates, so it is for tests only.
struct RecordingSink {
  std::vector<Event> events;

  void on_accepted(OrderId id) { events.emplace_back(Accepted{id}); }
  void on_rejected(OrderId id, Command c, RejectReason r) {
    events.emplace_back(Rejected{id, c, r});
  }
  void on_trade(OrderId aggressor, OrderId passive, Price p, Qty q, Qty aggressor_leaves,
                Qty passive_leaves) {
    events.emplace_back(Trade{aggressor, passive, p, q, aggressor_leaves, passive_leaves});
  }
  void on_canceled(OrderId id, Qty q, CancelReason r) { events.emplace_back(Canceled{id, q, r}); }
  void on_modified(OrderId id, Price p, Qty leaves) {
    events.emplace_back(Modified{id, p, leaves});
  }
};

} // namespace test
} // namespace me
