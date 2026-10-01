#include "imperivm/core/ui/markup.hpp"

#include <algorithm>

#include "imperivm/core/ui/interface.hpp"

namespace imperivm::core::ui {
namespace {

bool is_blank(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

/// `r g b` after `color`: three decimal numbers, each clamped to a byte.
/// Anything short of three leaves `ink` alone.
bool read_color(std::string_view body, Color& ink) {
  std::int32_t channel[3] = {0, 0, 0};
  std::size_t i = 0;
  for (std::int32_t& value : channel) {
    while (i < body.size() && is_blank(body[i])) ++i;
    if (i >= body.size() || body[i] < '0' || body[i] > '9') return false;
    std::int32_t number = 0;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
      number = std::min(number * 10 + (body[i] - '0'), 255);
      ++i;
    }
    value = number;
  }
  ink.red = static_cast<std::uint8_t>(channel[0]);
  ink.green = static_cast<std::uint8_t>(channel[1]);
  ink.blue = static_cast<std::uint8_t>(channel[2]);
  return true;
}

std::string_view trim_blank(std::string_view text) {
  while (!text.empty() && is_blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_blank(text.back())) text.remove_suffix(1);
  return text;
}

struct Builder {
  std::vector<MarkedLine> lines;
  std::string text;
  Color ink;

  void flush_text() {
    if (text.empty()) return;
    if (lines.empty()) lines.emplace_back();
    // A colour tag naming the colour already in force starts no new span.
    if (!lines.back().spans.empty() && lines.back().spans.back().image.empty() &&
        lines.back().spans.back().ink == ink) {
      lines.back().spans.back().text += text;
      text.clear();
      return;
    }
    TextSpan span;
    span.text = std::move(text);
    span.ink = ink;
    lines.back().spans.push_back(std::move(span));
    text.clear();
  }
  void image(std::string_view path, bool keyed) {
    flush_text();
    if (lines.empty()) lines.emplace_back();
    TextSpan span;
    span.image = resolve_alias(path);
    span.keyed = keyed;
    span.ink = ink;
    lines.back().spans.push_back(std::move(span));
  }
  void line_break() {
    flush_text();
    if (lines.empty()) lines.emplace_back();
    lines.emplace_back();
  }
};

const Image* span_image(ResourceCache& cache, const TextSpan& span) {
  if (span.image.empty()) return nullptr;
  ImageRef ref;
  ref.path = span.image;
  ref.has_key = span.keyed;
  ref.key_x = 0;
  ref.key_y = 0;
  return cache.image(ref);
}

}  // namespace

std::vector<MarkedLine> parse_markup(std::string_view text, Color ink) {
  Builder out;
  out.ink = ink;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\n') {
      out.line_break();
      continue;
    }
    if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
      out.line_break();
      ++i;
      continue;
    }
    if (c == '<') {
      const std::size_t close = text.find('>', i);
      // No `>` ahead, or another `<` before it: this one is text.
      if (close == std::string_view::npos || text.find('<', i + 1) < close) {
        out.text.push_back(c);
        continue;
      }
      const std::string_view body = trim_blank(text.substr(i + 1, close - i - 1));
      const std::size_t space = body.find_first_of(" \t");
      const std::string_view name = body.substr(0, space);
      const std::string_view rest =
          space == std::string_view::npos ? std::string_view{} : trim_blank(body.substr(space + 1));
      if (name == "color") {
        out.flush_text();
        (void)read_color(rest, out.ink);
      } else if (name == "imagetransp") {
        out.image(rest, true);
      } else if (name == "image") {
        out.image(rest, false);
      } else {
        // Anything else is text: the exe's tag vocabulary is exactly these,
        // and the table's `<Nessuno>` (`<None>`) and `<Prossima etichetta>`
        // are item texts that show their brackets.
        out.text.append(text.substr(i, close - i + 1));
      }
      i = close;
      continue;
    }
    out.text.push_back(c);
  }
  out.flush_text();
  // Trailing spaces before a break are the composer's separators (`250 `),
  // not content; a wholly blank tail line is the trailing `\n` and is kept,
  // because a break is a break.
  for (MarkedLine& line : out.lines) {
    if (line.spans.empty() || !line.spans.back().image.empty()) continue;
    std::string& tail = line.spans.back().text;
    while (!tail.empty() && is_blank(tail.back())) tail.pop_back();
    if (tail.empty()) line.spans.pop_back();
  }
  return out.lines;
}

std::string strip_markup(std::string_view text) {
  std::string out;
  const std::vector<MarkedLine> lines = parse_markup(text, Color{});
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) out.push_back('\n');
    for (const TextSpan& span : lines[i].spans) out += span.text;
  }
  return out;
}

std::int32_t marked_line_width(ResourceCache& cache, const Font& font, const MarkedLine& line) {
  std::int32_t width = 0;
  for (const TextSpan& span : line.spans) {
    if (const Image* image = span_image(cache, span); image != nullptr) {
      width += static_cast<std::int32_t>(image->width);
    } else if (span.image.empty()) {
      width += font.measure(span.text);
    }
  }
  return width;
}

std::int32_t marked_line_height(ResourceCache& cache, const Font& font, const MarkedLine& line) {
  std::int32_t height = font.height();
  for (const TextSpan& span : line.spans) {
    if (const Image* image = span_image(cache, span); image != nullptr) {
      height = std::max(height, static_cast<std::int32_t>(image->height));
    }
  }
  return height;
}

std::int32_t draw_marked_line(Canvas& canvas, ResourceCache& cache, const Font& font,
                              const MarkedLine& line, std::int32_t x, std::int32_t y,
                              const Rect& clip) {
  std::int32_t pen = 0;
  for (const TextSpan& span : line.spans) {
    if (const Image* image = span_image(cache, span); image != nullptr) {
      canvas.blit(*image, x + pen, y, clip);
      pen += static_cast<std::int32_t>(image->width);
    } else if (span.image.empty()) {
      pen += canvas.draw_text(font, span.text, x + pen, y, span.ink, clip);
    }
  }
  return pen;
}

}  // namespace imperivm::core::ui
