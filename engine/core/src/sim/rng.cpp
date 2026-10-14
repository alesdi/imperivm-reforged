// The simulation's one RNG: gbr.exe's Range(low, high) (0x0063ff00) on its
// LCG. See include/imperivm/core/sim/rng.hpp.

#include "imperivm/core/sim/rng.hpp"

namespace imperivm::core::sim {
namespace {

/// Numerical Recipes' 32-bit LCG constants. Full period over the whole 32-bit
/// state, so no seed is a dead end -- including zero.
constexpr std::uint32_t kMultiplier = 1664525u;
constexpr std::uint32_t kIncrement = 1013904223u;

}  // namespace

std::uint32_t Rng::next() noexcept {
  state_ = state_ * kMultiplier + kIncrement;
  return state_;
}

std::int32_t Rng::below(std::int32_t bound) noexcept {
  // `rand` (0x004c63a0) is Range(0, n - 1), and so is this. `bound - 1` wraps
  // the way the original's 32-bit `dec` does.
  return between(0, static_cast<std::int32_t>(static_cast<std::uint32_t>(bound) - 1u));
}

std::int32_t Rng::between(std::int32_t low, std::int32_t high) noexcept {
  // Range's three arms: equal ends answer without touching the state; a
  // reversed range is swapped; then one draw.
  if (low == high) return low;
  std::uint32_t span = 0;
  if (low < high) {
    span = static_cast<std::uint32_t>(high) - static_cast<std::uint32_t>(low);
  } else {
    span = static_cast<std::uint32_t>(low) - static_cast<std::uint32_t>(high);
    low = high;
  }
  const std::uint32_t draw = next();
  // `span + 1` in 32 bits, sign-extended to 64, times the draw zero-extended;
  // the answer is the product's high dword. Unsigned arithmetic gives the same
  // low 64 bits as the original's signed multiply, without overflow.
  const auto count = static_cast<std::int32_t>(span + 1u);
  const std::uint64_t product =
      static_cast<std::uint64_t>(static_cast<std::int64_t>(count)) * static_cast<std::uint64_t>(draw);
  const auto high_dword = static_cast<std::uint32_t>(product >> 32);
  return static_cast<std::int32_t>(high_dword + static_cast<std::uint32_t>(low));
}

}  // namespace imperivm::core::sim
