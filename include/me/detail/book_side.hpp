#pragma once

#include "me/detail/order_pool.hpp"
#include "me/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace me::detail {

// Plan §5.2: one price level, an intrusive doubly linked FIFO of pool nodes.
struct Level {
  Index head = NIL, tail = NIL; // FIFO: head is the oldest order
  Qty total = 0;                // sum of leaves, R-QRY-02, R-INV-06
};

// Plan §5.3: all levels of one side, indexed by price, with the best non-empty price cached.
// All memory is allocated in the constructor.
class BookSide {
public:
  // Valid prices are 1..max_price (R-CFG-01).
  BookSide(Side side, Price max_price, OrderPool &pool);

  // Links the node at the tail of its price level and adds its leaves to the level total. The
  // node's id, price, leaves and side must be set.
  void append(Index idx) {
    OrderNode &n = pool_[idx];
    assert(n.side == side_);
    assert(n.price >= 1 && n.price <= max_price_);
    Level &lvl = levels_[static_cast<std::size_t>(n.price)];
    n.prev = lvl.tail;
    n.next = NIL;
    if (lvl.tail == NIL) {
      lvl.head = idx;
      ++non_empty_;
    } else {
      pool_[lvl.tail].next = idx;
    }
    lvl.tail = idx;
    lvl.total += n.leaves;
    if (best_ == 0 || better(n.price, best_)) {
      best_ = n.price;
    }
  }

  // Removes the node from its level and subtracts its leaves. If that empties the best level, the
  // best price moves to the next non-empty level toward worse prices.
  void unlink(Index idx) {
    const OrderNode &n = pool_[idx];
    assert(n.side == side_);
    Level &lvl = levels_[static_cast<std::size_t>(n.price)];
    if (n.prev == NIL) {
      lvl.head = n.next;
    } else {
      pool_[n.prev].next = n.next;
    }
    if (n.next == NIL) {
      lvl.tail = n.prev;
    } else {
      pool_[n.next].prev = n.prev;
    }
    lvl.total -= n.leaves;
    if (lvl.head == NIL) {
      --non_empty_;
      if (n.price == best_) {
        best_ = non_empty_ == 0 ? 0 : scan_from(n.price);
      }
    }
  }

  // Reduces a resting order's leaves and its level total by `qty`, keeping its position (plan
  // §8.2, §8.4). Leaves may reach 0; the caller then unlinks the order.
  void reduce(Index idx, Qty qty) {
    OrderNode &n = pool_[idx];
    assert(n.side == side_);
    assert(qty >= 0 && qty <= n.leaves);
    n.leaves -= qty;
    levels_[static_cast<std::size_t>(n.price)].total -= qty;
  }

  // Oldest order at the best price, or NIL if the side is empty.
  Index front() const { return best_ == 0 ? NIL : levels_[static_cast<std::size_t>(best_)].head; }

  bool empty() const { return non_empty_ == 0; }

  // Best price with a non-empty level, or 0 if the side is empty (prices start at 1, D-17).
  Price best() const { return best_; }

  Side side() const { return side_; }

  // Precondition: 1 <= price <= max_price. An empty level has total 0 (R-QRY-02).
  const Level &level(Price price) const {
    assert(price >= 1 && price <= max_price_);
    return levels_[static_cast<std::size_t>(price)];
  }

private:
  // R-BOOK-02: higher is better for BUY, lower for SELL.
  bool better(Price a, Price b) const { return side_ == Side::Buy ? a > b : a < b; }

  // Next non-empty level strictly worse than `from`. Precondition: one exists.
  Price scan_from(Price from) const;

  OrderPool &pool_;
  std::vector<Level> levels_;
  Price max_price_;
  Price best_ = 0;
  std::uint32_t non_empty_ = 0;
  Side side_;
};

} // namespace me::detail
