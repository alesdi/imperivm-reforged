#include "imperivm/core/ui/font.hpp"

#include <array>

#include "imperivm/core/game/localization.hpp"

namespace imperivm::core::ui {
namespace {

constexpr std::size_t kMetricFields = 14;
constexpr std::size_t kGlyphRecord = 32;
constexpr std::size_t kKernRecord = 12;
constexpr std::size_t kRangeRecord = 16;

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + i]))
             << (8 * i);
  }
  return value;
}

std::int32_t read_i32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::int32_t>(read_u32(bytes, offset));
}

/// cp1252's 0x80..0x9F window. Every other byte is its own code point.
constexpr std::array<std::uint32_t, 32> kHighWindow{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

}  // namespace

std::uint32_t cp1252_to_unicode(std::uint8_t byte) noexcept {
  if (byte < 0x80 || byte > 0x9F) return byte;
  return kHighWindow[byte - 0x80];
}

std::string cp1252_from_utf8(std::string_view text) {
  return game::cp1252_from_utf8(text);
}

Result<Font> Font::parse(std::span<const std::byte> bytes) {
  if (bytes.size() < 0x20) return FormatError::truncated;
  if (static_cast<std::uint8_t>(bytes[0]) != 'A' || static_cast<std::uint8_t>(bytes[1]) != 'B' ||
      static_cast<std::uint8_t>(bytes[2]) != 'C' || static_cast<std::uint8_t>(bytes[3]) != 'F') {
    return FormatError::bad_magic;
  }

  Font font;
  font.bytes_ = bytes;

  const std::size_t metrics = read_u32(bytes, 0x04);
  if (metrics + kMetricFields * 4 + 4 > bytes.size()) return FormatError::out_of_range;
  font.height_ = read_i32(bytes, metrics + 0);
  font.ascent_ = read_i32(bytes, metrics + 5 * 4);
  font.descent_ = read_i32(bytes, metrics + 6 * 4);
  // The specification records that this holds for all six shipped fonts; a file
  // where it does not is one this reader has not seen and should not pretend to.
  if (font.height_ <= 0 || font.height_ != font.ascent_ + font.descent_) {
    return FormatError::malformed;
  }

  const std::size_t table = metrics + kMetricFields * 4;
  const std::uint32_t range_count = read_u32(bytes, table);
  if (range_count == 0 || range_count > 4096) return FormatError::malformed;
  if (table + 4 + kRangeRecord * range_count > bytes.size()) return FormatError::truncated;

  font.ranges_.reserve(range_count);
  std::uint32_t glyph_base = 0;
  for (std::uint32_t i = 0; i < range_count; ++i) {
    const std::size_t at = table + 4 + kRangeRecord * i;
    Range range;
    range.block_offset = read_u32(bytes, at + 0);
    range.block_size = read_u32(bytes, at + 4);
    range.first_char = read_u32(bytes, at + 8);
    range.char_count = read_u32(bytes, at + 12);
    if (range.block_offset + range.block_size > bytes.size()) return FormatError::out_of_range;
    if (range.block_size < 16) return FormatError::malformed;

    range.kern_offset = read_u32(bytes, range.block_offset + 0);
    range.kern_count = read_u32(bytes, range.block_offset + 4);
    range.pixel_offset = read_u32(bytes, range.block_offset + 8);
    range.pixel_size = read_u32(bytes, range.block_offset + 12);
    if (range.kern_offset != 16 + kGlyphRecord * range.char_count) return FormatError::malformed;
    if (range.pixel_offset != range.kern_offset + kKernRecord * range.kern_count) {
      return FormatError::malformed;
    }
    if (range.pixel_offset + range.pixel_size != range.block_size) return FormatError::malformed;

    font.cache_base_.push_back(glyph_base);
    glyph_base += range.char_count;
    font.ranges_.push_back(range);
  }

  font.cache_.resize(glyph_base);
  font.cached_.assign(glyph_base, 0);
  return font;
}

const Font::Range* Font::range_for(std::uint32_t code_point) const noexcept {
  for (const Range& range : ranges_) {
    if (code_point >= range.first_char && code_point - range.first_char < range.char_count) {
      return &range;
    }
  }
  return nullptr;
}

const Glyph* Font::glyph(std::uint32_t code_point) const {
  const Range* range = range_for(code_point);
  if (range == nullptr) return nullptr;
  const std::size_t which = static_cast<std::size_t>(range - ranges_.data());
  const std::uint32_t index = code_point - range->first_char;
  const std::size_t slot = cache_base_[which] + index;
  if (cached_[slot] != 0) return &cache_[slot];

  const std::size_t record = range->block_offset + 16 + kGlyphRecord * index;
  Glyph glyph;
  const std::int32_t abc_a = read_i32(bytes_, record + 0);
  const std::int32_t ink_width = read_i32(bytes_, record + 4);
  const std::int32_t abc_c = read_i32(bytes_, record + 8);
  const std::int32_t top = read_i32(bytes_, record + 16);
  const std::int32_t bottom = read_i32(bytes_, record + 24);
  const std::uint32_t pixel_start = read_u32(bytes_, record + 28);

  glyph.advance = abc_a + ink_width + abc_c;
  glyph.bearing = abc_a;
  glyph.top = top;

  // `bottom < top` marks a blank glyph, and the specification records that this
  // agrees with "stores zero pixel bytes" both ways across all 9,798 glyphs.
  if (bottom >= top && ink_width > 0) {
    glyph.width = static_cast<std::uint32_t>(ink_width);
    glyph.height = static_cast<std::uint32_t>(bottom - top + 1);
    const std::size_t pixels = static_cast<std::size_t>(glyph.width) * glyph.height;
    glyph.coverage.assign(pixels, 0);

    // A glyph's runs end where the next glyph's begin; the last runs to the end
    // of the pool. Runs may cross row boundaries, so this fills a flat array.
    std::size_t end = range->pixel_size;
    if (index + 1 < range->char_count) {
      end = read_u32(bytes_, record + kGlyphRecord + 28);
    }
    const std::size_t pool = range->block_offset + range->pixel_offset;
    std::size_t at = pool + pixel_start;
    const std::size_t stop = pool + end;
    std::size_t written = 0;
    while (at < stop && at < bytes_.size() && written < pixels) {
      const std::uint8_t byte = static_cast<std::uint8_t>(bytes_[at++]);
      const std::uint32_t alpha = static_cast<std::uint32_t>(byte >> 5);
      const std::uint32_t run = static_cast<std::uint32_t>(byte & 0x1F) + 1;
      const std::uint8_t coverage = static_cast<std::uint8_t>(alpha * 255u / 7u);
      for (std::uint32_t i = 0; i < run && written < pixels; ++i) {
        glyph.coverage[written++] = coverage;
      }
    }
  }

  cache_[slot] = std::move(glyph);
  cached_[slot] = 1;
  return &cache_[slot];
}

std::int32_t Font::kerning(std::uint32_t first, std::uint32_t second) const {
  // Pairs live in the block of the *first* character but may name a second from
  // any range.
  const Range* range = range_for(first);
  if (range == nullptr || range->kern_count == 0) return 0;
  const std::size_t base = range->block_offset + range->kern_offset;
  for (std::uint32_t i = 0; i < range->kern_count; ++i) {
    const std::size_t at = base + kKernRecord * i;
    if (read_u32(bytes_, at) == first && read_u32(bytes_, at + 4) == second) {
      return read_i32(bytes_, at + 8);
    }
  }
  return 0;
}

std::int32_t Font::measure(std::string_view text) const {
  std::int32_t pen = 0;
  std::uint32_t previous = 0;
  for (const char raw : text) {
    const std::uint32_t code = cp1252_to_unicode(static_cast<std::uint8_t>(raw));
    if (previous != 0) pen += kerning(previous, code);
    const Glyph* glyph = this->glyph(code);
    if (glyph != nullptr) pen += glyph->advance;
    previous = code;
  }
  return pen;
}

}  // namespace imperivm::core::ui
