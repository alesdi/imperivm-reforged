#pragma once

// Reader for the HMMSYS vector-quantised terrain texture (`.vq`).
//
// Specification: docs/formats/vq.md
//
// Despite the name this is not block VQ in the S3TC sense. The quantisation
// unit is a run of **four horizontally adjacent pixels**: the image is a grid
// of such runs and each one is an index into a shared codebook, which costs the
// rasteriser one lookup and one 8-byte copy per four pixels. Samples are
// X1R5G5B5, not RGB565.
//
// Decoding writes into a caller-supplied buffer. Terrain textures reach
// 1024x800, and a reader that returned a vector would allocate several
// megabytes per tile behind the caller's back.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "imperivm/core/formats/color.hpp"
#include "imperivm/core/formats/result.hpp"

namespace imperivm::core {

/// On disk the bytes read `mbqv`: the FourCC 'vqbm' stored as a little-endian
/// u32, which is how the engine compares it.
inline constexpr std::string_view kVqMagic = "mbqv";
inline constexpr std::size_t kVqHeaderSize = 0x2C;

/// The quantisation geometry, constant across all retail data. A decoder that
/// hardcodes it and merely checks the header words is safe; this one checks.
inline constexpr std::uint32_t kVqBlockWidth = 4;
inline constexpr std::uint32_t kVqBlockHeight = 1;
inline constexpr std::uint32_t kVqBytesPerPixel = 2;

struct VqHeader {
  std::uint32_t index_size_log2 = 0;
  std::uint32_t codebook_len = 0;
  std::uint32_t index_size = 0;
  std::uint32_t bytes_per_pixel = 0;
  std::uint32_t block_width = 0;
  std::uint32_t block_height = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t blocks_x = 0;
  std::uint32_t blocks_y = 0;

  constexpr std::uint64_t codebook_bytes() const noexcept {
    return static_cast<std::uint64_t>(codebook_len) * block_width * block_height * bytes_per_pixel;
  }
  constexpr std::uint64_t index_count() const noexcept {
    return static_cast<std::uint64_t>(blocks_x) * blocks_y;
  }
  constexpr std::uint64_t expected_size() const noexcept {
    return kVqHeaderSize + codebook_bytes() + index_count() * index_size;
  }
};

class VqImage {
 public:
  static Result<VqImage> parse(std::span<const std::byte> data);

  const VqHeader& header() const noexcept { return header_; }
  std::uint32_t width() const noexcept { return header_.width; }
  std::uint32_t height() const noexcept { return header_.height; }

  /// Raw X1R5G5B5 sample at a pixel, or 0 outside the image.
  std::uint16_t sample(std::uint32_t x, std::uint32_t y) const noexcept;

  /// Expand one row into `out`, which must hold `3 * width` bytes.
  Status decode_row_rgb888(std::uint32_t y, std::span<std::byte> out) const noexcept;

  /// Expand the whole image into `out`, which must hold `3 * width * height`
  /// bytes, row major, top row first.
  Status decode_rgb888(std::span<std::byte> out) const noexcept;

  /// The structural invariants from the specification, including that no index
  /// escapes the codebook and no sample sets bit 15. Walking the index array
  /// costs a pass over the file, so it is separate from parsing.
  Status validate() const noexcept;

 private:
  std::uint32_t index_at(std::uint64_t block) const noexcept;

  std::span<const std::byte> codebook_{};
  std::span<const std::byte> indices_{};
  VqHeader header_{};
};

}  // namespace imperivm::core
