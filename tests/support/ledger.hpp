#pragma once

#include "support/recording_sink.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace me::test {

// Accounting of one accepted order, rebuilt from commands and events only (plan §9).
struct OrderRecord {
  Side side;
  OrderType type;
  Price price; // current price; meaningless for MARKET
  Qty requested = 0;
  Qty executed = 0;
  Qty canceled = 0;
  std::uint64_t seq = 0; // arrival sequence, R-BOOK-03
  // R-INV-02: requested = executed + leaves + canceled.
  Qty leaves() const { return requested - executed - canceled; }
};

// Tracks requested, executed and canceled quantity per order (R-INV-02, R-MOD-06) and the arrival
// sequence (R-BOOK-03, R-MOD-05). Any event that contradicts the accounting is recorded in
// errors(), which check_invariants reports.
//
// Usage: call new_command() before submitting a NEW to the engine, then apply() each event the
// engine emitted, in order. CANCEL and MODIFY need no call: their events carry what is needed.
class Ledger {
public:
  void new_command(OrderId id, Side side, OrderType type, Price price, Qty qty) {
    pending_ = PendingNew{id, side, type, price, qty};
  }

  void apply(const Event &event) {
    std::visit([this](const auto &e) { on(e); }, event);
  }

  const std::map<OrderId, OrderRecord> &orders() const { return orders_; }
  const std::vector<std::string> &errors() const { return errors_; }

private:
  struct PendingNew {
    OrderId id;
    Side side;
    OrderType type;
    Price price;
    Qty qty;
  };

  void on(const Accepted &e) {
    if (!pending_ || pending_->id != e.order_id) {
      error() << "ACCEPTED for id " << e.order_id << " without a matching NEW command";
      return;
    }
    if (orders_.count(e.order_id) != 0) {
      error() << "ACCEPTED twice for id " << e.order_id;
      return;
    }
    const PendingNew &n = *pending_;
    orders_[n.id] = OrderRecord{n.side, n.type, n.price, n.qty, 0, 0, next_seq_++};
    pending_.reset();
  }

  void on(const Rejected &e) {
    if (e.command == Command::New && pending_ && pending_->id == e.order_id) {
      pending_.reset();
    }
  }

  void on(const Trade &e) {
    execute(e.aggressor_id, e.quantity, e.aggressor_leaves, "aggressor");
    execute(e.passive_id, e.quantity, e.passive_leaves, "passive");
  }

  void on(const Canceled &e) {
    OrderRecord *r = find(e.order_id, "CANCELED");
    if (r == nullptr) {
      return;
    }
    r->canceled += e.canceled_quantity;
    if (r->leaves() != 0) {
      error() << "CANCELED id " << e.order_id << " qty " << e.canceled_quantity << " leaves "
              << r->leaves() << " behind";
    }
  }

  void on(const Modified &e) {
    OrderRecord *r = find(e.order_id, "MODIFIED");
    if (r == nullptr) {
      return;
    }
    const Qty old = r->leaves();
    // R-MOD-06
    if (e.leaves > old) {
      r->requested += e.leaves - old;
    } else {
      r->canceled += old - e.leaves;
    }
    // R-MOD-05: a price change or an increase loses priority.
    if (e.price != r->price || e.leaves > old) {
      r->seq = next_seq_++;
    }
    r->price = e.price;
  }

  void execute(OrderId id, Qty qty, Qty reported_leaves, const char *role) {
    OrderRecord *r = find(id, "TRADE");
    if (r == nullptr) {
      return;
    }
    r->executed += qty;
    if (r->leaves() != reported_leaves) {
      error() << "TRADE " << role << " id " << id << " reports leaves " << reported_leaves
              << ", expected " << r->leaves();
    }
  }

  OrderRecord *find(OrderId id, const char *event) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
      error() << event << " for id " << id << " that was never accepted";
      return nullptr;
    }
    return &it->second;
  }

  // Appends a new error message and returns a stream to write it.
  struct ErrorStream {
    std::vector<std::string> &out;
    std::ostringstream s;
    ~ErrorStream() { out.push_back(s.str()); }
    template <class T> ErrorStream &operator<<(const T &v) {
      s << v;
      return *this;
    }
  };
  ErrorStream error() { return ErrorStream{errors_, {}}; }

  std::map<OrderId, OrderRecord> orders_;
  std::vector<std::string> errors_;
  std::optional<PendingNew> pending_;
  std::uint64_t next_seq_ = 0;
};

} // namespace me::test
