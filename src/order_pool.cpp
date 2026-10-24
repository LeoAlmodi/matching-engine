#include "me/detail/order_pool.hpp"

namespace me::detail {

OrderPool::OrderPool(std::uint32_t capacity) : nodes_(capacity), free_head_(capacity ? 0 : NIL) {
  assert(capacity < NIL);
  // Thread the free list in index order, so the first allocations are 0, 1, 2, ...
  for (std::uint32_t i = 0; i < capacity; ++i) {
    nodes_[i].next = i + 1 < capacity ? i + 1 : NIL;
  }
}

} // namespace me::detail
