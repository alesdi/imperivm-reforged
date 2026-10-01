#include "imperivm/core/formats/rle.hpp"

namespace imperivm::core {
namespace {

/// Parse one frame record at `position`, advancing it. Empty frames are 16
/// bytes and populated ones 54, which is why the table can only be read
/// sequentially.
Result<RleFrame> parse_frame(std::span<const std::byte> table, std::uint32_t index,
                             std::size_t& position) {
  ByteReader reader(table, position);

  RleFrame frame;
  frame.index = index;
  if (!reader.u32(frame.left) || !reader.u32(frame.top) || !reader.u32(frame.right) ||
      !reader.u32(frame.bottom)) {
    return FormatError::truncated;
  }

  if (frame.right < frame.left || frame.bottom < frame.top) {
    // The encoder grows the box inwards from a degenerate starting value and
    // never touched this one, so no RLE2 record follows. The starting value
    // varies per file, so testing for a sentinel would miss 913 frames.
    frame.empty = true;
    position = reader.position();
    return frame;
  }

  std::span<const std::byte> magic;
  if (!reader.bytes(4, magic)) return FormatError::truncated;
  if (!has_magic(magic, kRleFrameMagic)) return FormatError::malformed;

  std::uint16_t pixel_format = 0;
  std::uint32_t reserved = 0;
  std::uint32_t wide = 0;
  if (!reader.u32(frame.width) || !reader.u32(frame.height) || !reader.u16(pixel_format) ||
      !reader.u32(frame.data_size) || !reader.u32(reserved) || !reader.u32(wide) ||
      !reader.u32(frame.color_key)) {
    return FormatError::truncated;
  }
  if (!reader.bytes(4, magic)) return FormatError::truncated;
  if (!has_magic(magic, kRleStoreMagic)) return FormatError::malformed;
  if (!reader.u32(frame.data_offset)) return FormatError::truncated;

  if (reserved != 0) return FormatError::malformed;
  if (wide > 1) return FormatError::malformed;
  // The box and the size are redundant with each other in every retail frame,
  // so disagreement means the record was not understood.
  if (frame.left + frame.width - 1 != frame.right ||
      frame.top + frame.height - 1 != frame.bottom) {
    return FormatError::malformed;
  }
  switch (pixel_format) {
    case 1:
    case 3:
    case 7:
      break;
    default:
      return FormatError::unsupported;
  }

  frame.pixel_format = static_cast<RlePixelFormat>(pixel_format);
  frame.wide_row_table = wide != 0;
  frame.empty = false;
  position = reader.position();
  return frame;
}

}  // namespace

Result<RleImage> RleImage::parse(std::span<const std::byte> table) {
  if (!has_magic(table, kRleMagic)) return FormatError::bad_magic;
  if (table.size() < kRleHeaderSize) return FormatError::truncated;

  RleImage image;
  image.image_class_ = read_u32le(table, 6);
  const std::uint32_t reserved_a = read_u32le(table, 10);
  const std::uint32_t reserved_b = read_u32le(table, 14);
  image.columns_ = read_u32le(table, 18);
  image.rows_ = read_u32le(table, 22);
  if (reserved_a != 0 || reserved_b != 0) return FormatError::malformed;

  const std::uint64_t count = static_cast<std::uint64_t>(image.columns_) * image.rows_;
  // Every frame record is at least the 16-byte bounding box, so a count that
  // could not fit in the file is rejected before anything is reserved for it.
  if (count * 16 > table.size()) return FormatError::truncated;
  image.frames_.reserve(static_cast<std::size_t>(count));

  std::size_t position = kRleHeaderSize;
  for (std::uint64_t index = 0; index < count; ++index) {
    const Result<RleFrame> frame =
        parse_frame(table, static_cast<std::uint32_t>(index), position);
    if (!frame) return frame.error();
    image.frames_.push_back(*frame);
  }

  if (position + 4 > table.size()) return FormatError::truncated;
  const std::uint32_t palette_len = read_u32le(table, position);
  position += 4;
  if (palette_len > 256) return FormatError::malformed;
  if (!span_contains(table, position, 4ull * palette_len)) return FormatError::truncated;
  image.palette_ = table.subspan(position, 4 * palette_len);
  position += 4 * palette_len;

  // The file ends on the palette, or on the 512-byte lookup table that class 2
  // appends. Anything else is unexplained, and an unexplained tail is how a
  // misparsed frame table announces itself.
  const std::size_t tail = table.size() - position;
  if (tail == kRleLutSize) {
    image.lut_ = table.subspan(position, kRleLutSize);
  } else if (tail != 0) {
    return FormatError::malformed;
  }
  return image;
}

Result<RleFrame> RleImage::frame(std::uint32_t row, std::uint32_t column) const {
  if (row >= rows_ || column >= columns_) return FormatError::out_of_range;
  return frames_[static_cast<std::size_t>(row) * columns_ + column];
}

Rgb888 RleImage::palette_color(std::size_t index) const noexcept {
  if (index >= palette_size()) return Rgb888{};
  const std::size_t at = index * 4;
  return Rgb888{read_u8(palette_, at + 2), read_u8(palette_, at + 1), read_u8(palette_, at)};
}

std::uint16_t RleImage::lut(std::size_t index) const noexcept {
  if (2 * index + 1 >= lut_.size()) return 0;
  return read_u16le(lut_, 2 * index);
}

void RleImage::canvas_size(std::uint32_t& width, std::uint32_t& height) const noexcept {
  width = 0;
  height = 0;
  for (const RleFrame& frame : frames_) {
    if (frame.empty) continue;
    if (frame.right + 1 > width) width = frame.right + 1;
    if (frame.bottom + 1 > height) height = frame.bottom + 1;
  }
}

Status RleImage::validate() const {
  RlePixelFormat seen = RlePixelFormat::none;
  for (const RleFrame& frame : frames_) {
    if (frame.empty) continue;
    if (seen == RlePixelFormat::none) {
      seen = frame.pixel_format;
    } else if (seen != frame.pixel_format) {
      return FormatError::malformed;  // every frame of a file shares one format
    }
    if (frame.pixel_format == RlePixelFormat::indexed8) {
      if (palette_size() == 0) return FormatError::malformed;
      if (frame.color_key >= palette_size()) return FormatError::malformed;
    }
    // The wide flag exists solely because a u16 offset cannot reach past 65535.
    if (frame.wide_row_table != (frame.data_size > 0xFFFF)) return FormatError::malformed;
  }

  if (image_class() == RleImageClass::player_color) {
    if (palette_size() < 2 * kRlePlayerColorSlots) return FormatError::malformed;
    for (std::size_t i = 0; i < kRlePlayerColorSlots; ++i) {
      if (palette_color(i) != palette_color(i + kRlePlayerColorSlots)) {
        return FormatError::malformed;
      }
    }
  }
  return {};
}

Result<std::span<const std::byte>> rle_frame_payload(const RleFrame& frame,
                                                     std::span<const std::byte> store) {
  if (frame.empty) return std::span<const std::byte>{};
  if (!span_contains(store, frame.data_offset, frame.data_size)) return FormatError::out_of_range;
  return store.subspan(frame.data_offset, frame.data_size);
}

Status rle_decode_rgba(const RleImage& image, const RleFrame& frame,
                       std::span<const std::byte> blob, std::span<std::byte> out,
                       std::span<const Rgb888> player_colors, Rgb888 shadow_color) {
  const std::uint64_t needed = static_cast<std::uint64_t>(frame.width) * frame.height * 4;
  if (out.size() < needed) return FormatError::buffer_too_small;
  if (!player_colors.empty() && player_colors.size() != kRlePlayerColorSlots) {
    return FormatError::malformed;
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(needed); ++i) out[i] = std::byte{0};
  if (frame.empty) return {};

  const auto colour_of_index = [&](std::uint32_t index) -> Rgb888 {
    if (!player_colors.empty() && index < kRlePlayerColorSlots) return player_colors[index];
    return image.palette_color(index);
  };

  return rle_for_each_span(
      frame, blob,
      [&](std::uint32_t y, std::uint32_t x, std::uint32_t length,
          std::span<const std::byte> payload) {
        for (std::uint32_t i = 0; i < length; ++i) {
          const std::uint64_t column = static_cast<std::uint64_t>(x) + i;
          if (column >= frame.width) return;  // clipped; the row check will fail
          Rgb888 colour = shadow_color;
          if (frame.pixel_format == RlePixelFormat::indexed8) {
            colour = colour_of_index(read_u8(payload, i));
          } else if (frame.pixel_format == RlePixelFormat::rgb555) {
            colour = expand_x1r5g5b5(read_u16le(payload, 2 * i));
          }
          const std::size_t at =
              static_cast<std::size_t>((static_cast<std::uint64_t>(y) * frame.width + column) * 4);
          out[at + 0] = static_cast<std::byte>(colour.red);
          out[at + 1] = static_cast<std::byte>(colour.green);
          out[at + 2] = static_cast<std::byte>(colour.blue);
          out[at + 3] = std::byte{0xFF};
        }
      });
}

}  // namespace imperivm::core
