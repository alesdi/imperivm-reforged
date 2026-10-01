#pragma once

/// Inline markup in interface text: `<color r g b>` and `<imagetransp path>`.
///
/// Two tags, and `gbr.exe` composes with no others. The command tooltip is
/// built from them (0x004f3590, 0x004ea790):
///
/// ```
/// <color 255 255 0>%s1<color 255 255 255>  <imagetransp gameres/infobar/common/hotkey.bmp> <color 255 255 255>%s2
/// <color 255 0 0><imagetransp gameres/infobar/common/gold_cost.bmp> 250
/// ```
///
/// -- a title in yellow, the hotkey icon and the key, and a cost line whose
/// colour is white while the player can pay and red when they cannot -- and
/// the translation table carries the same tags in `MPGAMEMENU.INI`'s bonus
/// `HelpText` and in a verifier's `reasonText = rollover(bld, true) +
/// "\n<color 255 0 0>"` (`CREATE_FOOD_MULE_VERIFY.VS:33`). The exe's own
/// strings name `<imagetransp ` (0x007cfeec) and nothing else tag-shaped that
/// is not XML, so `<image path>` without the key is accepted here by analogy
/// and labelled as such; no shipped text uses it.
///
/// A colour holds until the next `<color>`, across line breaks -- every
/// shipped composer resets to white before its own break, so the reading is
/// not tested by the data. A line break is a newline or the two characters
/// `\n`, which the INI files and the table both write. An image stands in the
/// line like a glyph, its top at the line's top; whether the original
/// centres it is not read, and the shipped icons are 18 and 20 pixels
/// against a 14-pixel font, so the line grows to hold them. `imagetransp`
/// keys the bitmap at its top-left pixel, the convention every keyed
/// interface bitmap follows (`interface-ini.md`).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/ui/paint.hpp"

namespace imperivm::core::ui {

/// One run of a marked line: text in one colour, or one image.
struct TextSpan {
  std::string text;   ///< cp1252; empty when this span is an image
  std::string image;  ///< alias-resolved bitmap path; empty when text
  bool keyed = false;  ///< `imagetransp`: colour-keyed at its top-left pixel
  Color ink;
};

struct MarkedLine {
  std::vector<TextSpan> spans;
  [[nodiscard]] bool empty() const noexcept { return spans.empty(); }
};

/// Parse `text` into lines of spans, starting in `ink`. Unknown tags are
/// dropped with their contents; a `<` that no `>` follows is text.
[[nodiscard]] std::vector<MarkedLine> parse_markup(std::string_view text, Color ink);

/// The text of `text` with every tag removed, breaks kept as newlines.
[[nodiscard]] std::string strip_markup(std::string_view text);

/// The width and height of `line` drawn in `font`: the text's advance plus
/// each image's width, and the tallest of the font and the images.
[[nodiscard]] std::int32_t marked_line_width(ResourceCache& cache, const Font& font,
                                             const MarkedLine& line);
[[nodiscard]] std::int32_t marked_line_height(ResourceCache& cache, const Font& font,
                                              const MarkedLine& line);

/// Draw `line` with its top-left at `(x, y)`; returns the pen advance.
std::int32_t draw_marked_line(Canvas& canvas, ResourceCache& cache, const Font& font,
                              const MarkedLine& line, std::int32_t x, std::int32_t y,
                              const Rect& clip);

}  // namespace imperivm::core::ui
