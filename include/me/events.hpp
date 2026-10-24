#pragma once

#include "me/types.hpp"

namespace me {

// R-EVT-01: one struct per event type, with the fields listed in the spec.

struct Accepted {
  OrderId order_id;
  friend bool operator==(const Accepted &, const Accepted &) = default;
};

struct Rejected {
  OrderId order_id;
  Command command;
  RejectReason reason;
  friend bool operator==(const Rejected &, const Rejected &) = default;
};

struct Trade {
  OrderId aggressor_id;
  OrderId passive_id;
  Price price;
  Qty quantity;
  Qty aggressor_leaves;
  Qty passive_leaves;
  friend bool operator==(const Trade &, const Trade &) = default;
};

struct Canceled {
  OrderId order_id;
  Qty canceled_quantity;
  CancelReason reason;
  friend bool operator==(const Canceled &, const Canceled &) = default;
};

struct Modified {
  OrderId order_id;
  Price price;
  Qty leaves;
  friend bool operator==(const Modified &, const Modified &) = default;
};

// The engine reports events by calling the sink directly (static dispatch).
template <class S>
concept EventSink =
    requires(S &s, OrderId id, Command c, RejectReason r, CancelReason cr, Price p, Qty q) {
      s.on_accepted(id);
      s.on_rejected(id, c, r);
      // aggressor, passive, price, quantity, aggressor_leaves, passive_leaves
      s.on_trade(id, id, p, q, q, q);
      s.on_canceled(id, q, cr);
      s.on_modified(id, p, q);
    };

} // namespace me
