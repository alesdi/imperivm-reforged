// The tick loop. See include/imperivm/core/sim/tick.hpp.

#include "imperivm/core/sim/tick.hpp"

namespace imperivm::core::sim {

std::int32_t turn_length_from_real_ms(std::int32_t real_ms, std::int32_t game_speed) noexcept {
  if (real_ms <= 0 || game_speed <= 0) return 1;
  const std::int64_t units =
      (static_cast<std::int64_t>(real_ms) * game_speed) / kGameTimeUnitsPerSecond;
  if (units <= 0) return 1;
  if (units > 0x7FFFFFFF) return 0x7FFFFFFF;
  return static_cast<std::int32_t>(units);
}

const Turn& Clock::advance(std::int32_t length) noexcept {
  if (length > 0) config_.turn_length = length;
  const std::int32_t used = config_.turn_length;

  ++turn_.index;
  turn_.length = used;
  // Measured, and all three exactly, in all nine dumps: the turn about to run
  // starts one unit past the boundary and its window is `length` wide.
  turn_.start = time_ + 1;
  turn_.end = turn_.start + used;
  time_ += used;
  turn_.time = time_;
  return turn_;
}

}  // namespace imperivm::core::sim
