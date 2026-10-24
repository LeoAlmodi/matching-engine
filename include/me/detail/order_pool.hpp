#pragma once

#include "me/types.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace me::detail {

// Plan §5.1
struct OrderNode {
  OrderId id;
  Price price;
  Qty leaves;
  Index prev, next; // FIFO links within the level; `next` also threads the free list
  Side side;
};

// Fixed-size pool of order nodes. All memory is allocated in the constructor; alloc and free are
// O(1) and never allocate.
class OrderPool {
public:
  explicit OrderPool(std::uint32_t capacity);

  // Returns a free slot, or NIL if the pool is exhausted. The node's fields are not reset.
  Index alloc() {
    const Index idx = free_head_;
    if (idx != NIL) {
      free_head_ = nodes_[idx].next;
    }
    return idx;
  }

  void free(Index idx) {
    assert(idx < nodes_.size());
    nodes_[idx].next = free_head_;
    free_head_ = idx;
  }

  OrderNode &operator[](Index idx) {
    assert(idx < nodes_.size());
    return nodes_[idx];
  }
  const OrderNode &operator[](Index idx) const {
    assert(idx < nodes_.size());
    return nodes_[idx];
  }

  std::uint32_t capacity() const { return static_cast<std::uint32_t>(nodes_.size()); }

private:
  std::vector<OrderNode> nodes_;
  Index free_head_;
};

} // namespace me::detail
