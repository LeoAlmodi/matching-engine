#include "me/detail/book_side.hpp"

namespace me::detail {

BookSide::BookSide(Side side, Price max_price, OrderPool &pool)
    : pool_(pool), levels_(static_cast<std::size_t>(max_price) + 1), max_price_(max_price),
      side_(side) {
  assert(max_price >= 1);
}

// O(distance to the next non-empty level), accepted in v1 (plan §5.3).
Price BookSide::scan_from(Price from) const {
  const Price step = side_ == Side::Buy ? -1 : 1;
  Price p = from + step;
  while (levels_[static_cast<std::size_t>(p)].head == NIL) {
    p += step;
    assert(p >= 1 && p <= max_price_);
  }
  return p;
}

} // namespace me::detail
