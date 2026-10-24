#pragma once

#include "me/types.hpp"

#include <cstdint>

namespace me {

// Validated by the Engine constructor, not here.
struct Config {
  Price max_price;          // R-CFG-01
  Qty max_qty;              // R-CFG-02
  std::uint32_t max_orders; // R-CFG-03, must be < NIL - 1
};

} // namespace me
