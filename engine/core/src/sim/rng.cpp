// The simulation's one RNG. See include/imperivm/core/sim/rng.hpp for why the
// algorithm here is a documented placeholder rather than a recovered one.

#include "imperivm/core/sim/rng.hpp"

namespace imperivm::core::sim {
namespace {

/// Numerical Recipes' 32-bit LCG constants. Full period over the whole 32-bit
/// state, so no seed is a dead end -- including zero.
constexpr std::uint32_t kMultiplier = 1664525u;
constexpr std::uint32_t kIncrement = 1013904223u;

/// A bijective finaliser over the LCG state.
///
/// An LCG's low bits have short periods -- bit 0 alternates -- so `next() & 1`
/// on the raw state is not a coin flip. Mixing the state through a bijection
/// before handing it out fixes that without touching the state itself, which
/// keeps the serialised value and the stream position the same thing.
constexpr std::uint32_t scramble(std::uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

}  // namespace

std::uint32_t Rng::next() noexcept {
  state_ = state_ * kMultiplier + kIncrement;
  return scramble(state_);
}

std::int32_t Rng::below(std::int32_t bound) noexcept {
  // The draw happens whether or not it is used: a system that calls rand and
  // discards the result still moves the stream, and the peers must agree about
  // that. Returning early without drawing would make the bound part of the
  // stream position.
  const std::uint32_t draw = next();
  if (bound <= 1) return 0;
  // Lemire's multiply-shift: uniform to within one part in 2^32 and, unlike
  // `draw % bound`, free of the modulo bias that skews small bounds. Integer
  // only, no division, and the product cannot overflow 64 bits.
  const std::uint64_t product = static_cast<std::uint64_t>(draw) * static_cast<std::uint64_t>(
                                                                      static_cast<std::uint32_t>(bound));
  return static_cast<std::int32_t>(product >> 32);
}

std::int32_t Rng::between(std::int32_t low, std::int32_t high) noexcept {
  if (high <= low) {
    (void)next();  // draw anyway; see below()
    return low;
  }
  // `high - low + 1` as 64-bit: the span of INT32_MIN..INT32_MAX overflows a
  // 32-bit count, and an overflow here would be a desync rather than a wrong
  // number, because two builds need not overflow the same way.
  const std::int64_t span = static_cast<std::int64_t>(high) - static_cast<std::int64_t>(low) + 1;
  const std::uint32_t draw = next();
  const std::uint64_t product =
      static_cast<std::uint64_t>(draw) * static_cast<std::uint64_t>(span);
  return static_cast<std::int32_t>(static_cast<std::int64_t>(low) +
                                   static_cast<std::int64_t>(product >> 32));
}

}  // namespace imperivm::core::sim
