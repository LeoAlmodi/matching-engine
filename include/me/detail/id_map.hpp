#pragma once

#include "me/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace me::detail {

// splitmix64 finalizer (plan §5.4): spreads sequential or adversarial ids across the table.
constexpr std::uint64_t splitmix64(std::uint64_t x) {
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// Open-addressing map from OrderId to a 32-bit value, with linear probing and fixed capacity
// (plan §5.4). All memory is allocated in the constructor. Entries are never deleted, only
// updated, so there are no tombstones (D-15).
class IdMap {
public:
  // Slot unused. Equal to NIL, so it never collides with a pool index.
  static constexpr std::uint32_t EMPTY = NIL;
  // Id accepted in this session, but the order is filled or canceled (R-VAL-04, R-CXL-02,
  // R-MOD-01).
  static constexpr std::uint32_t NOT_RESTING = NIL - 1;

  // Capacity is the smallest power of two >= 2 * max_orders (load factor <= 0.5).
  explicit IdMap(std::uint32_t max_orders);

  // Returns the value stored for `id`, or EMPTY if the id was never inserted.
  std::uint32_t find(OrderId id) const { return entries_[probe(id)].value; }

  // Precondition: `id` is not present, `value != EMPTY`, and fewer than `max_orders` ids have been
  // inserted.
  void insert(OrderId id, std::uint32_t value) {
    assert(value != EMPTY);
    Entry &e = entries_[probe(id)];
    assert(e.value == EMPTY);
    e.key = id;
    e.value = value;
  }

  // Precondition: `id` is present and `value != EMPTY`.
  void update(OrderId id, std::uint32_t value) {
    assert(value != EMPTY);
    Entry &e = entries_[probe(id)];
    assert(e.value != EMPTY);
    e.value = value;
  }

  std::size_t capacity() const { return entries_.size(); }

private:
  struct Entry {
    OrderId key;
    std::uint32_t value;
  };

  // Slot holding `id`, or the first EMPTY slot on its probe sequence. Terminates because the load
  // factor is at most 0.5, so at least one slot is always EMPTY.
  std::size_t probe(OrderId id) const {
    std::size_t i = static_cast<std::size_t>(splitmix64(id)) & mask_;
    while (entries_[i].value != EMPTY && entries_[i].key != id) {
      i = (i + 1) & mask_;
    }
    return i;
  }

  std::vector<Entry> entries_;
  std::size_t mask_;
};

} // namespace me::detail
