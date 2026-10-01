#include "imperivm/core/ui/image.hpp"

#include <algorithm>

#include <cstring>
#include <string_view>

#include "imperivm/core/formats/color.hpp"

namespace imperivm::core::ui {
namespace {

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + i]))
             << (8 * i);
  }
  return value;
}

std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8));
}

}  // namespace

Result<Image> decode_bmp(std::span<const std::byte> bytes) {
  constexpr std::size_t kFileHeader = 14;
  constexpr std::size_t kInfoHeader = 40;
  if (bytes.size() < kFileHeader + kInfoHeader) return FormatError::truncated;
  if (static_cast<std::uint8_t>(bytes[0]) != 'B' || static_cast<std::uint8_t>(bytes[1]) != 'M') {
    return FormatError::bad_magic;
  }

  const std::uint32_t data_offset = read_u32(bytes, 10);
  const std::uint32_t header_size = read_u32(bytes, kFileHeader);
  if (header_size < kInfoHeader) return FormatError::unsupported;

  const std::int32_t width = static_cast<std::int32_t>(read_u32(bytes, kFileHeader + 4));
  const std::int32_t signed_height = static_cast<std::int32_t>(read_u32(bytes, kFileHeader + 8));
  const std::uint16_t bpp = read_u16(bytes, kFileHeader + 14);
  const std::uint32_t compression = read_u32(bytes, kFileHeader + 16);

  if (compression != 0) return FormatError::unsupported;
  if (width <= 0 || signed_height == 0) return FormatError::malformed;
  if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return FormatError::unsupported;

  const bool bottom_up = signed_height > 0;
  const std::uint32_t height =
      static_cast<std::uint32_t>(bottom_up ? signed_height : -signed_height);
  const std::uint32_t uwidth = static_cast<std::uint32_t>(width);

  // Rows are padded to a four-byte boundary. `(w * bpp + 31) / 32 * 4` is the
  // usual spelling and cannot overflow at these sizes.
  const std::size_t stride =
      ((static_cast<std::size_t>(uwidth) * bpp + 31u) / 32u) * 4u;
  const std::size_t needed = static_cast<std::size_t>(data_offset) + stride * height;
  if (needed > bytes.size()) return FormatError::truncated;

  std::uint32_t palette[256] = {};
  if (bpp == 8) {
    std::uint32_t colors = read_u32(bytes, kFileHeader + 32);
    if (colors == 0 || colors > 256) colors = 256;
    const std::size_t table = kFileHeader + header_size;
    if (table + 4u * colors > bytes.size()) return FormatError::truncated;
    for (std::uint32_t i = 0; i < colors; ++i) {
      const std::size_t at = table + 4u * i;
      const std::uint8_t b = static_cast<std::uint8_t>(bytes[at]);
      const std::uint8_t g = static_cast<std::uint8_t>(bytes[at + 1]);
      const std::uint8_t r = static_cast<std::uint8_t>(bytes[at + 2]);
      palette[i] = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
                   (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u;
    }
  }

  Image out;
  out.width = uwidth;
  out.height = height;
  out.rgba.assign(static_cast<std::size_t>(uwidth) * height * 4u, 0);

  // A 32-bit bitmap's fourth byte is its alpha when any pixel uses it --
  // `Minimap.pak`'s zoom pictures are cut out that way -- and padding when
  // none does, which is the older convention and reads as opaque.
  bool any_alpha = false;
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t source_row = bottom_up ? height - 1 - y : y;
    const std::size_t row = static_cast<std::size_t>(data_offset) + stride * source_row;
    std::uint8_t* destination = out.pixel(0, y);
    for (std::uint32_t x = 0; x < uwidth; ++x) {
      std::uint8_t r = 0;
      std::uint8_t g = 0;
      std::uint8_t b = 0;
      std::uint8_t a = 0xFF;
      switch (bpp) {
        case 8: {
          const std::uint32_t entry = palette[static_cast<std::uint8_t>(bytes[row + x])];
          r = static_cast<std::uint8_t>(entry);
          g = static_cast<std::uint8_t>(entry >> 8);
          b = static_cast<std::uint8_t>(entry >> 16);
          break;
        }
        case 16: {
          const Rgb888 colour = expand_x1r5g5b5(read_u16(bytes, row + 2u * x));
          r = colour.red;
          g = colour.green;
          b = colour.blue;
          break;
        }
        case 24:
        case 32: {
          const std::size_t at = row + static_cast<std::size_t>(bpp / 8) * x;
          b = static_cast<std::uint8_t>(bytes[at]);
          g = static_cast<std::uint8_t>(bytes[at + 1]);
          r = static_cast<std::uint8_t>(bytes[at + 2]);
          if (bpp == 32) {
            a = static_cast<std::uint8_t>(bytes[at + 3]);
            if (a != 0) any_alpha = true;
          }
          break;
        }
        default:
          break;
      }
      destination[4 * x + 0] = r;
      destination[4 * x + 1] = g;
      destination[4 * x + 2] = b;
      destination[4 * x + 3] = a;
    }
  }
  if (bpp == 32 && !any_alpha) {
    for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 0xFF;
  }
  return out;
}

Result<IndexImage> decode_bmp_indices(std::span<const std::byte> bytes) {
  constexpr std::size_t kFileHeader = 14;
  constexpr std::size_t kInfoHeader = 40;
  if (bytes.size() < kFileHeader + kInfoHeader) return FormatError::truncated;
  if (static_cast<std::uint8_t>(bytes[0]) != 'B' || static_cast<std::uint8_t>(bytes[1]) != 'M') {
    return FormatError::bad_magic;
  }
  const std::uint32_t data_offset = read_u32(bytes, 10);
  const std::int32_t width = static_cast<std::int32_t>(read_u32(bytes, kFileHeader + 4));
  const std::int32_t signed_height = static_cast<std::int32_t>(read_u32(bytes, kFileHeader + 8));
  const std::uint16_t bpp = read_u16(bytes, kFileHeader + 14);
  const std::uint32_t compression = read_u32(bytes, kFileHeader + 16);
  if (compression != 0 || bpp != 8) return FormatError::unsupported;
  if (width <= 0 || signed_height == 0) return FormatError::malformed;
  const bool bottom_up = signed_height > 0;
  const std::uint32_t height = static_cast<std::uint32_t>(bottom_up ? signed_height : -signed_height);
  const std::uint32_t uwidth = static_cast<std::uint32_t>(width);
  const std::size_t stride = ((static_cast<std::size_t>(uwidth) * 8u + 31u) / 32u) * 4u;
  if (static_cast<std::size_t>(data_offset) + stride * height > bytes.size()) return FormatError::truncated;
  IndexImage out;
  out.width = uwidth;
  out.height = height;
  out.indices.resize(static_cast<std::size_t>(uwidth) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t source_row = bottom_up ? height - 1 - y : y;
    const std::size_t row = static_cast<std::size_t>(data_offset) + stride * source_row;
    for (std::uint32_t x = 0; x < uwidth; ++x) {
      out.indices[static_cast<std::size_t>(y) * uwidth + x] = static_cast<std::uint8_t>(bytes[row + x]);
    }
  }
  return out;
}

void tint_by_index(Image& image, const IndexImage& mask, std::span<const IndexTint> tints) {
  if (image.empty() || mask.width != image.width || mask.height != image.height) return;
  const IndexTint* by_index[256] = {};
  for (const IndexTint& tint : tints) by_index[tint.index] = &tint;
  for (std::uint32_t y = 0; y < image.height; ++y) {
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const IndexTint* tint = by_index[mask.at(x, y)];
      if (tint == nullptr) continue;
      std::uint8_t* pixel = image.pixel(x, y);
      // RGB -> HSV in integers: value is the max channel, saturation the
      // spread over it (0..1024), and the hue is replaced outright.
      const std::int32_t r = pixel[0];
      const std::int32_t g = pixel[1];
      const std::int32_t b = pixel[2];
      const std::int32_t high = std::max(r, std::max(g, b));
      const std::int32_t low = std::min(r, std::min(g, b));
      std::int32_t saturation = high == 0 ? 0 : (high - low) * 1024 / high;
      saturation = std::clamp(saturation * tint->saturation / 1024, 0, 1024);
      // HSV -> RGB with the new hue: sextant `hue / 256`, fraction within.
      const std::int32_t hue = ((tint->hue % 1536) + 1536) % 1536;
      const std::int32_t sextant = hue / 256;
      const std::int32_t fraction = hue % 256;
      const std::int32_t p = high * (1024 - saturation) / 1024;
      const std::int32_t q = high * (1024 - saturation * fraction / 256) / 1024;
      const std::int32_t t = high * (1024 - saturation * (256 - fraction) / 256) / 1024;
      std::int32_t out[3] = {high, high, high};
      switch (sextant) {
        case 0: out[0] = high; out[1] = t; out[2] = p; break;
        case 1: out[0] = q; out[1] = high; out[2] = p; break;
        case 2: out[0] = p; out[1] = high; out[2] = t; break;
        case 3: out[0] = p; out[1] = q; out[2] = high; break;
        case 4: out[0] = t; out[1] = p; out[2] = high; break;
        default: out[0] = high; out[1] = p; out[2] = q; break;
      }
      pixel[0] = static_cast<std::uint8_t>(std::clamp(out[0], 0, 255));
      pixel[1] = static_cast<std::uint8_t>(std::clamp(out[1], 0, 255));
      pixel[2] = static_cast<std::uint8_t>(std::clamp(out[2], 0, 255));
    }
  }
}

Image resample_box(const Image& source, std::uint32_t width, std::uint32_t height) {
  Image out;
  if (source.empty() || width == 0 || height == 0) return out;
  out.width = width;
  out.height = height;
  out.rgba.assign(static_cast<std::size_t>(width) * height * 4u, 0);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * source.height / height);
    const std::uint32_t y1 = std::max(y0 + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1) * source.height / height));
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint32_t x0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * source.width / width);
      const std::uint32_t x1 = std::max(x0 + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1) * source.width / width));
      std::uint32_t sum[4] = {0, 0, 0, 0};
      std::uint32_t count = 0;
      for (std::uint32_t sy = y0; sy < y1 && sy < source.height; ++sy) {
        for (std::uint32_t sx = x0; sx < x1 && sx < source.width; ++sx) {
          const std::uint8_t* pixel = source.pixel(sx, sy);
          for (int c = 0; c < 4; ++c) sum[c] += pixel[c];
          ++count;
        }
      }
      std::uint8_t* destination = out.pixel(x, y);
      for (int c = 0; c < 4; ++c) {
        destination[c] = static_cast<std::uint8_t>(count == 0 ? 0 : (sum[c] + count / 2) / count);
      }
    }
  }
  return out;
}

void apply_color_key(Image& image, std::uint32_t key_x, std::uint32_t key_y) noexcept {
  if (image.empty() || key_x >= image.width || key_y >= image.height) return;
  const std::uint8_t* key = image.pixel(key_x, key_y);
  const std::uint8_t r = key[0];
  const std::uint8_t g = key[1];
  const std::uint8_t b = key[2];
  for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
    if (image.rgba[i] == r && image.rgba[i + 1] == g && image.rgba[i + 2] == b) {
      image.rgba[i + 3] = 0;
    }
  }
}

Image sub_image(const Image& source, std::uint32_t column, std::uint32_t columns,
                std::uint32_t row, std::uint32_t rows) {
  Image out;
  if (columns == 0 || rows == 0 || source.empty()) return out;
  if (column >= columns || row >= rows) return out;
  if (source.width % columns != 0 || source.height % rows != 0) return out;
  out.width = source.width / columns;
  out.height = source.height / rows;
  out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4u);
  for (std::uint32_t y = 0; y < out.height; ++y) {
    std::memcpy(out.pixel(0, y), source.pixel(column * out.width, row * out.height + y),
                static_cast<std::size_t>(out.width) * 4u);
  }
  return out;
}

std::uint32_t image_type_frames(std::string_view code) noexcept {
  std::uint32_t highest = 0;
  bool any = false;
  for (const char c : code) {
    if (c < 'A' || c > 'Z') return 0;
    const std::uint32_t index = static_cast<std::uint32_t>(c - 'A');
    if (index > highest) highest = index;
    any = true;
  }
  return any ? highest + 1 : 0;
}

std::uint32_t image_type_frame(std::string_view code, std::uint32_t state) noexcept {
  if (code.empty()) return 0;
  const std::size_t index = state < code.size() ? state : 0;
  const char c = code[index];
  if (c < 'A' || c > 'Z') return 0;
  return static_cast<std::uint32_t>(c - 'A');
}
}  // namespace imperivm::core::ui
