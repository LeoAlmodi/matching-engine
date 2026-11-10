#include "me/detail/id_map.hpp"

#include <bit>

namespace me::detail {

IdMap::IdMap(std::uint32_t max_orders)
    : entries_(std::bit_ceil(2 * static_cast<std::size_t>(max_orders)), Entry{0, EMPTY}),
      mask_(entries_.size() - 1) {}

} // namespace me::detail
