#pragma once

// Reader for the HMMSYS sprite format (`*.rle.mmp` frame tables and `rle.mmp`).
//
// Specification: docs/formats/rle.md
//
// A sprite is split across two files, and the split is the first thing that
// shapes this API:
//
//   * the **frame table**, `NAME.RLE.MMP`, lives inside a pack and holds the
//     grid shape, the per-frame geometry and the palette;
//   * the **pixel store**, the single 400 MB `rle.mmp` at the install root,
//     holds nothing but concatenated compressed payloads, addressed by the
//     absolute `data_offset` in each frame record.
//
// So decoding takes a parsed frame table plus a span over the store. The core
// never opens either; the platform maps the store once and hands the span down.
//
// Rows are runs of (gap, length) pairs, so decoding is naturally a stream of
// covered spans. `rle_for_each_span` is the primitive — it allocates nothing
// and hands the caller the raw payload — and `rle_decode_rgba` is the
// convenience built on it for callers that just want a texture.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/formats/color.hpp"
#include "imperivm/core/formats/result.hpp"

namespace imperivm::core {

inline constexpr std::string_view kRleMagic = "IMGRLE";
inline constexpr std::string_view kRleFrameMagic = "RLE2";
/// `mmap` with its bytes reversed, which is what named the pixel store.
inline constexpr std::string_view kRleStoreMagic = "pamm";

inline constexpr std::size_t kRleHeaderSize = 26;
inline constexpr std::size_t kRleFrameRecordSize = 54;
inline constexpr std::size_t kRleLutSize = 512;

/// Palette slots 0..63 are the remappable team-colour block; the shipped copy
/// of 64..127 is only a neutral default so the sprite reads correctly untinted.
inline constexpr std::size_t kRlePlayerColorSlots = 64;

/// Storage strategy of a whole image. Correlates with the `drawmode` attribute
/// of the entity XML, but the XML disagrees with the bytes a few hundred times
/// in the retail data — the file is authoritative.
enum class RleImageClass : std::uint32_t {
  truecolor = 0,     ///< RGB555, drawmode="normal"
  indexed = 1,       ///< 8-bit, 1..256 palette entries
  player_color = 2,  ///< 8-bit, 256 entries, first 64 remappable, plus a LUT
  shadow = 4,        ///< 1-bit coverage mask
  clouds = 5,        ///< RGB555, additive-looking
};

enum class RlePixelFormat : std::uint16_t {
  none = 0,      ///< an empty frame, which stores no pixels at all
  indexed8 = 1,  ///< one palette index per covered pixel
  rgb555 = 3,    ///< one little-endian u16 per covered pixel
  mask = 7,      ///< no payload: a run is simply "covered"
};

constexpr std::uint32_t bytes_per_pixel(RlePixelFormat format) noexcept {
  switch (format) {
    case RlePixelFormat::indexed8:
      return 1;
    case RlePixelFormat::rgb555:
      return 2;
    default:
      return 0;
  }
}

/// One frame. The bounds are inclusive and absolute in a canvas shared by every
/// frame of the image and by the matching shadow image, so compositing an
/// animation is just "blit each frame's pixels at (left, top)".
struct RleFrame {
  std::uint32_t index = 0;
  std::uint32_t left = 0;
  std::uint32_t top = 0;
  std::uint32_t right = 0;
  std::uint32_t bottom = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  RlePixelFormat pixel_format = RlePixelFormat::none;
  std::uint32_t data_size = 0;
  std::uint32_t data_offset = 0;  ///< absolute, into the pixel store
  std::uint32_t color_key = 0;
  /// Set on exactly the 439 frames whose payload exceeds 65,535 bytes, where a
  /// u16 row offset could no longer reach the end of the payload.
  bool wide_row_table = false;
  /// An empty frame carries only its 16-byte bounding box, with no RLE2 or
  /// pamm block following. Detected by `right < left || bottom < top`, never by
  /// a sentinel: the degenerate box varies per file.
  bool empty = true;

  constexpr std::uint32_t row_entry_size() const noexcept { return wide_row_table ? 4u : 2u; }
};

/// A parsed frame table. Views the caller's bytes for the palette and LUT; the
/// frame records themselves are decoded into a vector because the records are
/// variable length (empty frames are 16 bytes, populated ones 54), so random
/// access is impossible without one pass.
class RleImage {
 public:
  static Result<RleImage> parse(std::span<const std::byte> table);

  RleImageClass image_class() const noexcept { return static_cast<RleImageClass>(image_class_); }
  std::uint32_t raw_image_class() const noexcept { return image_class_; }
  std::uint32_t columns() const noexcept { return columns_; }
  std::uint32_t rows() const noexcept { return rows_; }

  const std::vector<RleFrame>& frames() const noexcept { return frames_; }

  /// Frames are row major. In unit and building sprites the column selects the
  /// facing direction and the row selects the animation step.
  Result<RleFrame> frame(std::uint32_t row, std::uint32_t column) const;

  std::size_t palette_size() const noexcept { return palette_.size() / 4; }
  /// Palette entries are Windows RGBQUAD: blue, green, red, and a padding byte
  /// that is not alpha.
  Rgb888 palette_color(std::size_t index) const noexcept;

  /// The 512-byte RGB555 lookup table appended to class 2 files. Redundant with
  /// the palette — it can be rebuilt from it — so nothing here needs it.
  bool has_lut() const noexcept { return !lut_.empty(); }
  std::uint16_t lut(std::size_t index) const noexcept;

  /// Smallest canvas containing every frame's bounding box.
  void canvas_size(std::uint32_t& width, std::uint32_t& height) const noexcept;

  /// The structural invariants from the specification: one pixel format per
  /// file, 8-bit frames have a palette holding their colour key, the wide-row
  /// flag agrees with the payload size, and class 2 mirrors slots 0..63 onto
  /// 64..127.
  Status validate() const;

 private:
  std::span<const std::byte> palette_{};
  std::span<const std::byte> lut_{};
  std::vector<RleFrame> frames_;
  std::uint32_t image_class_ = 0;
  std::uint32_t columns_ = 0;
  std::uint32_t rows_ = 0;
};

/// The compressed payload of one frame, as a subspan of the pixel store.
/// Empty frames yield an empty span.
Result<std::span<const std::byte>> rle_frame_payload(const RleFrame& frame,
                                                     std::span<const std::byte> store);

/// Walk every covered run of a frame, in row order, left to right.
///
/// `fn(y, x, length, payload)` receives the row, the starting column, the run
/// length in pixels and the run's bytes — empty for a mask frame, `length`
/// bytes for indexed, `2 * length` for RGB555. Runs of length zero are skipped:
/// they exist only so a gap wider than 255 can be expressed as repeated pairs.
///
/// Fails on any row whose pairs do not consume its byte range exactly or whose
/// gaps and runs do not sum to exactly the frame width, which is the check that
/// catches a payload addressed at the wrong offset in the store.
template <class F>
Status rle_for_each_span(const RleFrame& frame, std::span<const std::byte> blob, F&& fn) {
  if (frame.empty) return {};
  if (frame.height == 0 || frame.width == 0) return FormatError::malformed;

  const std::uint32_t entry_size = frame.row_entry_size();
  const std::uint64_t table_bytes = static_cast<std::uint64_t>(frame.height) * entry_size;
  if (table_bytes > blob.size()) return FormatError::truncated;

  const std::uint32_t stride = bytes_per_pixel(frame.pixel_format);
  const auto row_offset = [&](std::uint32_t row) -> std::uint64_t {
    const std::size_t at = static_cast<std::size_t>(row) * entry_size;
    return entry_size == 2 ? read_u16le(blob, at) : read_u32le(blob, at);
  };

  // The table abuts the row data, and offsets never go backwards.
  if (row_offset(0) != table_bytes) return FormatError::malformed;

  for (std::uint32_t y = 0; y < frame.height; ++y) {
    const std::uint64_t start = row_offset(y);
    const std::uint64_t end = y + 1 < frame.height ? row_offset(y + 1) : blob.size();
    if (start < table_bytes || end < start || end > blob.size()) return FormatError::malformed;

    std::uint64_t position = start;
    std::uint64_t x = 0;
    while (position + 1 < end) {
      const std::uint32_t gap = read_u8(blob, static_cast<std::size_t>(position));
      const std::uint32_t length = read_u8(blob, static_cast<std::size_t>(position + 1));
      position += 2;
      x += gap;
      if (length != 0) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(length) * stride;
        if (bytes > end - position) return FormatError::malformed;
        fn(y, static_cast<std::uint32_t>(x), length,
           blob.subspan(static_cast<std::size_t>(position), static_cast<std::size_t>(bytes)));
        position += bytes;
        x += length;
      }
      if (x > frame.width) return FormatError::malformed;
    }
    if (position != end) return FormatError::malformed;
    if (x != frame.width) return FormatError::malformed;
  }
  return {};
}

/// Expand one frame into a `width * height * 4` RGBA buffer. Uncovered pixels
/// are left fully transparent, which is the whole of the format's transparency:
/// there is no alpha channel and the colour key is never needed to decode.
///
/// `player_colors`, when supplied, must hold 64 entries and replaces palette
/// slots 0..63 — this is how the engine tints a unit for its owner. Pass an
/// empty span to keep the neutral colours shipped in the file.
Status rle_decode_rgba(const RleImage& image, const RleFrame& frame,
                       std::span<const std::byte> blob, std::span<std::byte> out,
                       std::span<const Rgb888> player_colors = {},
                       Rgb888 shadow_color = Rgb888{0, 0, 0});

}  // namespace imperivm::core
