#pragma once

// Colour conversions shared by the pixel formats.
//
// Everything here is integer arithmetic, and not only because the core forbids
// floating point: bit replication is also the *correct* expansion. Scaling a
// 5-bit channel by 255/31 is the same thing done in a way that can round
// differently per compiler, and it maps 31 to 255 only if the rounding happens
// to go the right way. Replication maps 0 to 0 and 31 to 255 exactly, by
// construction.

#include <cstdint>

namespace imperivm::core {

struct Rgb888 {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;

  friend constexpr bool operator==(const Rgb888&, const Rgb888&) = default;
};

/// Expand a 5-bit channel to 8 bits: the top three bits are repeated in the
/// bottom three.
constexpr std::uint8_t expand5(std::uint32_t value) noexcept {
  return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

/// X1R5G5B5 (`xrrrrrgggggbbbbb`) to 8-bit RGB. Used by `.vq` codebook samples
/// and by RGB555 sprite frames, which share the packing. Bit 15 is ignored: it
/// is clear in every retail sample and carries no alpha.
constexpr Rgb888 expand_x1r5g5b5(std::uint16_t sample) noexcept {
  return Rgb888{expand5((sample >> 10) & 0x1F), expand5((sample >> 5) & 0x1F),
                expand5(sample & 0x1F)};
}

}  // namespace imperivm::core
