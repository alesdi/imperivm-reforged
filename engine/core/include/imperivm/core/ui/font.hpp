#pragma once

/// The bitmap fonts the interface draws with (`.apf`).
///
/// `docs/formats/apf.md` has the format; this is the reader the interface needs
/// and nothing more. Six fonts ship in `Fonts.pak` and the in-game bars name
/// exactly one of them, `Fonts/Tahoma14b.apf`, through `%InterfaceFont%`.
///
/// A glyph is 3-bit coverage, not colour: the engine multiplies it against
/// whatever ink the caller asks for, which is why one font serves the white
/// unit name and the yellow highlight in the same bar. So this decodes to an
/// alpha mask and `paint.hpp` supplies the colour.
///
/// Decoding is lazy per glyph and cached, because a font is 1,633 glyphs across
/// ten Unicode ranges and a command bar draws perhaps forty of them.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::ui {

/// One glyph's ink box: `width * height` bytes of 0..255 coverage.
struct Glyph {
  std::int32_t advance = 0;  ///< abc_a + ink_width + abc_c, the pen step
  std::int32_t bearing = 0;  ///< abc_a, added to the pen before blitting
  std::int32_t top = 0;      ///< first ink row, from the top of the line box
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> coverage;

  [[nodiscard]] bool blank() const noexcept { return width == 0 || height == 0; }
};

/// A parsed `.apf`.
class Font {
 public:
  /// Parse `bytes`, which the font keeps a view of and must not outlive.
  [[nodiscard]] static Result<Font> parse(std::span<const std::byte> bytes);

  [[nodiscard]] std::int32_t height() const noexcept { return height_; }
  [[nodiscard]] std::int32_t ascent() const noexcept { return ascent_; }
  [[nodiscard]] std::int32_t descent() const noexcept { return descent_; }

  /// The glyph for a code point, or null if the font has no range covering it.
  [[nodiscard]] const Glyph* glyph(std::uint32_t code_point) const;

  /// The kerning between two code points, which is zero in five of the six
  /// shipped fonts and -1 for 184 pairs in `TAHOMA13`. Whether the engine
  /// applies it at all is unknown; this reports it and `measure`/`draw` use it.
  [[nodiscard]] std::int32_t kerning(std::uint32_t first, std::uint32_t second) const;

  /// The advance width of `text`, interpreted as cp1252 — which is what the
  /// files and the translation table are.
  [[nodiscard]] std::int32_t measure(std::string_view text) const;

 private:
  struct Range {
    std::size_t block_offset = 0;
    std::size_t block_size = 0;
    std::uint32_t first_char = 0;
    std::uint32_t char_count = 0;
    std::size_t kern_offset = 0;
    std::uint32_t kern_count = 0;
    std::size_t pixel_offset = 0;
    std::size_t pixel_size = 0;
  };

  [[nodiscard]] const Range* range_for(std::uint32_t code_point) const noexcept;

  std::span<const std::byte> bytes_;
  std::vector<Range> ranges_;
  std::int32_t height_ = 0;
  std::int32_t ascent_ = 0;
  std::int32_t descent_ = 0;
  /// Decoded on first use. `mutable` because `glyph` is logically const: the
  /// cache is not part of the font's value.
  mutable std::vector<Glyph> cache_;
  mutable std::vector<std::uint8_t> cached_;  ///< 0 = not yet decoded
  std::vector<std::uint32_t> cache_base_;     ///< index of each range's first glyph
};

/// cp1252 to Unicode for the 32 code points that differ from Latin-1.
///
/// The files, the translation table and the fonts are all cp1252, and the font
/// indexes glyphs by Unicode code point: `0x92` in a file is a right single
/// quote, U+2019, which lives in the font's General Punctuation range and not
/// at 0x92 in Latin-1 where nothing is. Without this a translated apostrophe
/// draws as a hole.
[[nodiscard]] std::uint32_t cp1252_to_unicode(std::uint8_t byte) noexcept;
/// The reverse, for text that arrives as UTF-8 -- `game.xml`'s descriptions
/// carry an ellipsis as three bytes -- into the cp1252 the fonts index. A
/// code point cp1252 has no slot for becomes `?`; bytes that are not
/// valid UTF-8 are taken as cp1252 already and kept.
[[nodiscard]] std::string cp1252_from_utf8(std::string_view text);

}  // namespace imperivm::core::ui
