#include "imperivm/core/ui/paint.hpp"

#include <algorithm>

#include "imperivm/core/formats/ini.hpp"

namespace imperivm::core::ui {
namespace {

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

/// `(src * a + dst * (255 - a) + 127) / 255`. Exact and target independent.
std::uint8_t over(std::uint8_t source, std::uint8_t destination, std::uint8_t alpha) noexcept {
  const std::uint32_t value = static_cast<std::uint32_t>(source) * alpha +
                              static_cast<std::uint32_t>(destination) * (255u - alpha) + 127u;
  return static_cast<std::uint8_t>(value / 255u);
}

Rect intersect(const Rect& a, const Rect& b) noexcept {
  const std::int32_t x = std::max(a.x, b.x);
  const std::int32_t y = std::max(a.y, b.y);
  const std::int32_t right = std::min(a.right(), b.right());
  const std::int32_t bottom = std::min(a.bottom(), b.bottom());
  return Rect{x, y, std::max(0, right - x), std::max(0, bottom - y)};
}

}  // namespace

Color parse_color(std::string_view text, Color fallback) {
  const std::vector<std::string_view> parts = split_list(text);
  if (parts.size() < 3) return fallback;
  std::int32_t channel[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (!parse_int(parts[static_cast<std::size_t>(i)], channel[i])) return fallback;
    if (channel[i] < 0 || channel[i] > 255) return fallback;
  }
  return Color{static_cast<std::uint8_t>(channel[0]), static_cast<std::uint8_t>(channel[1]),
               static_cast<std::uint8_t>(channel[2]), 255};
}

// -- Canvas -----------------------------------------------------------------

void Canvas::resize(std::uint32_t width, std::uint32_t height) {
  width_ = width;
  height_ = height;
  rgba_.assign(static_cast<std::size_t>(width) * height * 4u, 0);
}

void Canvas::clear(Color color) {
  for (std::size_t i = 0; i + 3 < rgba_.size(); i += 4) {
    rgba_[i] = color.red;
    rgba_[i + 1] = color.green;
    rgba_[i + 2] = color.blue;
    rgba_[i + 3] = color.alpha;
  }
}

Color Canvas::at(std::uint32_t x, std::uint32_t y) const noexcept {
  if (x >= width_ || y >= height_) return Color{0, 0, 0, 0};
  const std::size_t at = (static_cast<std::size_t>(y) * width_ + x) * 4u;
  return Color{rgba_[at], rgba_[at + 1], rgba_[at + 2], rgba_[at + 3]};
}

void Canvas::blit(const Image& image, std::int32_t x, std::int32_t y, const Rect& clip) {
  if (image.empty() || width_ == 0) return;
  const Rect target = intersect(
      intersect(Rect{x, y, static_cast<std::int32_t>(image.width),
                     static_cast<std::int32_t>(image.height)},
                clip),
      Rect{0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)});
  for (std::int32_t row = target.y; row < target.bottom(); ++row) {
    const std::uint8_t* source =
        image.pixel(static_cast<std::uint32_t>(target.x - x), static_cast<std::uint32_t>(row - y));
    std::uint8_t* destination =
        rgba_.data() + (static_cast<std::size_t>(row) * width_ + target.x) * 4u;
    for (std::int32_t column = 0; column < target.width; ++column) {
      const std::uint8_t alpha = source[3];
      if (alpha != 0) {
        destination[0] = over(source[0], destination[0], alpha);
        destination[1] = over(source[1], destination[1], alpha);
        destination[2] = over(source[2], destination[2], alpha);
        destination[3] = static_cast<std::uint8_t>(
            std::min(255, static_cast<int>(destination[3]) + alpha));
      }
      source += 4;
      destination += 4;
    }
  }
}

void Canvas::blit_region(const Image& image, const Rect& source, std::int32_t x,
                         std::int32_t y, const Rect& clip) {
  if (image.empty() || width_ == 0) return;
  const Rect bounds = intersect(
      source, Rect{0, 0, static_cast<std::int32_t>(image.width),
                   static_cast<std::int32_t>(image.height)});
  if (bounds.width <= 0 || bounds.height <= 0) return;
  const Rect target = intersect(
      intersect(Rect{x, y, bounds.width, bounds.height}, clip),
      Rect{0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)});
  for (std::int32_t row = target.y; row < target.bottom(); ++row) {
    const std::uint8_t* from = image.pixel(static_cast<std::uint32_t>(bounds.x + target.x - x),
                                           static_cast<std::uint32_t>(bounds.y + row - y));
    std::uint8_t* destination =
        rgba_.data() + (static_cast<std::size_t>(row) * width_ + target.x) * 4u;
    for (std::int32_t column = 0; column < target.width; ++column) {
      const std::uint8_t alpha = from[3];
      if (alpha != 0) {
        destination[0] = over(from[0], destination[0], alpha);
        destination[1] = over(from[1], destination[1], alpha);
        destination[2] = over(from[2], destination[2], alpha);
        destination[3] = static_cast<std::uint8_t>(
            std::min(255, static_cast<int>(destination[3]) + alpha));
      }
      from += 4;
      destination += 4;
    }
  }
}

void Canvas::tile_region(const Image& image, const Rect& source, const Rect& target,
                         const Rect& clip) {
  if (source.width <= 0 || source.height <= 0 || target.width <= 0 || target.height <= 0) return;
  const Rect bounds = intersect(target, clip);
  if (bounds.width <= 0 || bounds.height <= 0) return;
  for (std::int32_t y = target.y; y < target.bottom(); y += source.height) {
    for (std::int32_t x = target.x; x < target.right(); x += source.width) {
      blit_region(image, source, x, y, bounds);
    }
  }
}

void Canvas::blit_scaled(const Image& image, const Rect& target, const Rect& clip) {
  if (image.empty() || width_ == 0 || target.width <= 0 || target.height <= 0) return;
  const Rect bounds = intersect(
      intersect(target, clip),
      Rect{0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)});
  for (std::int32_t row = bounds.y; row < bounds.bottom(); ++row) {
    const std::uint32_t sy = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(row - target.y) * image.height / target.height);
    std::uint8_t* destination =
        rgba_.data() + (static_cast<std::size_t>(row) * width_ + bounds.x) * 4u;
    for (std::int32_t column = bounds.x; column < bounds.right(); ++column) {
      const std::uint32_t sx = static_cast<std::uint32_t>(
          static_cast<std::int64_t>(column - target.x) * image.width / target.width);
      const std::uint8_t* from = image.pixel(std::min(sx, image.width - 1), std::min(sy, image.height - 1));
      const std::uint8_t alpha = from[3];
      if (alpha != 0) {
        destination[0] = over(from[0], destination[0], alpha);
        destination[1] = over(from[1], destination[1], alpha);
        destination[2] = over(from[2], destination[2], alpha);
        destination[3] = static_cast<std::uint8_t>(
            std::min(255, static_cast<int>(destination[3]) + alpha));
      }
      destination += 4;
    }
  }
}

void Canvas::fill_rect(const Rect& rect, Color color) {
  const Rect target = intersect(
      rect, Rect{0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)});
  for (std::int32_t row = target.y; row < target.bottom(); ++row) {
    std::uint8_t* destination =
        rgba_.data() + (static_cast<std::size_t>(row) * width_ + target.x) * 4u;
    for (std::int32_t column = 0; column < target.width; ++column) {
      destination[0] = over(color.red, destination[0], color.alpha);
      destination[1] = over(color.green, destination[1], color.alpha);
      destination[2] = over(color.blue, destination[2], color.alpha);
      destination[3] = static_cast<std::uint8_t>(
          std::min(255, static_cast<int>(destination[3]) + color.alpha));
      destination += 4;
    }
  }
}

std::int32_t Canvas::draw_text(const Font& font, std::string_view text, std::int32_t x,
                               std::int32_t y, Color ink, const Rect& clip) {
  const Rect target = intersect(
      clip, Rect{0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)});
  std::int32_t pen = 0;
  std::uint32_t previous = 0;
  for (const char raw : text) {
    const std::uint32_t code = cp1252_to_unicode(static_cast<std::uint8_t>(raw));
    if (previous != 0) pen += font.kerning(previous, code);
    previous = code;
    const Glyph* glyph = font.glyph(code);
    if (glyph == nullptr) continue;
    if (!glyph->blank()) {
      const std::int32_t ox = x + pen + glyph->bearing;
      const std::int32_t oy = y + glyph->top;
      for (std::uint32_t row = 0; row < glyph->height; ++row) {
        const std::int32_t py = oy + static_cast<std::int32_t>(row);
        if (py < target.y || py >= target.bottom()) continue;
        for (std::uint32_t column = 0; column < glyph->width; ++column) {
          const std::int32_t px = ox + static_cast<std::int32_t>(column);
          if (px < target.x || px >= target.right()) continue;
          const std::uint8_t coverage = glyph->coverage[static_cast<std::size_t>(row) * glyph->width + column];
          if (coverage == 0) continue;
          const std::uint8_t alpha =
              static_cast<std::uint8_t>((static_cast<std::uint32_t>(coverage) * ink.alpha + 127u) / 255u);
          std::uint8_t* destination =
              rgba_.data() + (static_cast<std::size_t>(py) * width_ + px) * 4u;
          destination[0] = over(ink.red, destination[0], alpha);
          destination[1] = over(ink.green, destination[1], alpha);
          destination[2] = over(ink.blue, destination[2], alpha);
          destination[3] = static_cast<std::uint8_t>(
              std::min(255, static_cast<int>(destination[3]) + alpha));
        }
      }
    }
    pen += glyph->advance;
  }
  return pen;
}

// -- ResourceCache ----------------------------------------------------------

const Image* ResourceCache::image(const ImageRef& ref) {
  if (ref.empty()) return nullptr;
  std::string key = ref.path;
  if (ref.has_key) {
    key += '\n';
    key += std::to_string(ref.key_x);
    key += ',';
    key += std::to_string(ref.key_y);
  }
  for (const auto& entry : images_) {
    if (entry.key == key) return entry.ok ? &entry.image : nullptr;
  }
  ImageEntry entry;
  entry.key = key;
  const std::span<const std::byte> bytes = provider_ ? provider_(ref.path) : std::span<const std::byte>{};
  if (!bytes.empty()) {
    Result<Image> decoded = decode_bmp(bytes);
    if (decoded.ok()) {
      entry.image = std::move(decoded.value());
      if (ref.has_key) {
        apply_color_key(entry.image, static_cast<std::uint32_t>(ref.key_x),
                        static_cast<std::uint32_t>(ref.key_y));
      }
      entry.ok = true;
    }
  }
  if (!entry.ok) missing_.push_back(ref.path);
  images_.push_back(std::move(entry));
  return images_.back().ok ? &images_.back().image : nullptr;
}

const Image* ResourceCache::image_frame(const ImageRef& ref, std::uint32_t column,
                                        std::uint32_t columns, std::uint32_t row,
                                        std::uint32_t rows) {
  if (columns <= 1 && rows <= 1) return image(ref);
  std::string key = ref.path;
  key += '\n';
  key += std::to_string(ref.has_key ? ref.key_x : -1);
  key += ',';
  key += std::to_string(ref.has_key ? ref.key_y : -1);
  key += "\n#";
  key += std::to_string(column);
  key += '/';
  key += std::to_string(columns);
  key += ',';
  key += std::to_string(row);
  key += '/';
  key += std::to_string(rows);
  for (const auto& entry : images_) {
    if (entry.key == key) return entry.ok ? &entry.image : nullptr;
  }
  const Image* whole = image(ref);
  ImageEntry entry;
  entry.key = key;
  if (whole != nullptr) {
    entry.image = sub_image(*whole, column, columns, row, rows);
    entry.ok = !entry.image.empty();
  }
  images_.push_back(std::move(entry));
  return images_.back().ok ? &images_.back().image : nullptr;
}

const Font* ResourceCache::font(std::string_view path) {
  const std::string resolved = resolve_alias(path);
  for (const auto& entry : fonts_) {
    if (entry.path == resolved) return entry.ok ? &entry.font : nullptr;
  }
  FontEntry entry;
  entry.path = resolved;
  const std::span<const std::byte> bytes =
      provider_ ? provider_(resolved) : std::span<const std::byte>{};
  if (!bytes.empty()) {
    Result<Font> parsed = Font::parse(bytes);
    if (parsed.ok()) {
      entry.font = std::move(parsed.value());
      entry.ok = true;
    }
  }
  if (!entry.ok) missing_.push_back(resolved);
  fonts_.push_back(std::move(entry));
  return fonts_.back().ok ? &fonts_.back().font : nullptr;
}

// -- painting ---------------------------------------------------------------

namespace {

ControlState state_of(const Widget& widget, const BarContent& content) {
  if (has(widget.style, Style::kDisabled)) return ControlState::kDisabled;
  if (!content.pressed.empty() && iequal(content.pressed, widget.name)) {
    return ControlState::kPressed;
  }
  if (!content.hovered.empty() && iequal(content.hovered, widget.name)) {
    return ControlState::kHighlighted;
  }
  return ControlState::kNormal;
}

/// The image a widget draws in its current state, honouring `ImageType` (which
/// selects a column of the strip) and `Rows` (which selects a row).
const Image* current_image(const Widget& widget, ResourceCache& cache,
                           const BarContent& content) {
  if (widget.image.empty()) return nullptr;
  const std::uint32_t columns = std::max(1u, image_type_frames(widget.image_type));
  const std::uint32_t rows = static_cast<std::uint32_t>(std::max(1, widget.rows));
  const std::uint32_t column =
      widget.image_type.empty()
          ? 0u
          : std::min(columns - 1,
                     image_type_frame(widget.image_type,
                                      static_cast<std::uint32_t>(state_of(widget, content))));
  std::uint32_t row = std::min(rows - 1, static_cast<std::uint32_t>(std::max(0, widget.initial_row)));
  // A blinking `Switch` shows the row it is not on: `1 - row` (0x006c1032).
  std::int32_t blink_time = 0;
  if (widget.kind == WidgetType::kSwitch && content.blink &&
      widget.attribute_int("BlinkTime", blink_time) && blink_time > 0) {
    row = std::min(rows - 1, 1u - std::min(1u, row));
  }
  return cache.image_frame(widget.image, column, columns, row, rows);
}

/// A bitmap by virtual path: a class icon, an item image, a skill icon.
///
/// These are named without a colour-key probe -- the class XML's `icon=`
/// carries a path and nothing else -- and most of them are painted on the
/// pure green every keyed bitmap in the info bar art uses as its key
/// (`interface-ini.md`: probed across the art, the key is `0,255,0` every
/// time). So the top-left pixel is probed here: pure green, and the bitmap
/// is keyed there; anything else, and it is drawn as it is. The unit
/// specials' 18 x 18 icons are the ones that are not green at the corner,
/// and keying them would punch holes in their grey.
const Image* icon_image(ResourceCache& cache, const std::string& path) {
  if (path.empty()) return nullptr;
  ImageRef ref;
  ref.path = resolve_alias(path);
  const Image* raw = cache.image(ref);
  if (raw == nullptr || raw->empty()) return raw;
  const std::uint8_t* corner = raw->pixel(0, 0);
  if (corner[0] == 0 && corner[1] == 255 && corner[2] == 0) {
    ref.has_key = true;
    ref.key_x = 0;
    ref.key_y = 0;
    return cache.image(ref);
  }
  return raw;
}

/// The colour a ramp bitmap gives for `percent`, or a flat green without one.
///
/// The shipped ramps (`health gradient 0.bmp`, 9 x 3, and `1`, 9 x 5) run
/// **green at the left to red at the right**, so full health samples column
/// zero and empty health the last one. Sampled the other way round, a full
/// bar is red -- which is how it was drawn for as long as nothing put a
/// selection in the bar.
Color ramp_color(ResourceCache& cache, const ImageRef& gradient, std::int32_t percent) {
  Color bar{64, 200, 64, 255};
  const Image* ramp = cache.image(gradient);
  if (ramp != nullptr && !ramp->empty()) {
    const std::int32_t width = static_cast<std::int32_t>(ramp->width);
    const std::int32_t missing = 100 - std::clamp<std::int32_t>(percent, 0, 100);
    const std::uint32_t column =
        static_cast<std::uint32_t>(std::min<std::int32_t>(width - 1, (width - 1) * missing / 100));
    const std::uint8_t* sample = ramp->pixel(column, 0);
    bar = Color{sample[0], sample[1], sample[2], 255};
  }
  return bar;
}

/// Blit `image` so that it is centred across `width` at `x`.
void blit_centred(Canvas& canvas, const Image& image, std::int32_t x, std::int32_t width,
                  std::int32_t y, const Rect& clip) {
  canvas.blit(image, x + (width - static_cast<std::int32_t>(image.width)) / 2, y, clip);
}

/// Where a strip's cells stand: the cell and the gap, the gap being the
/// widest the row allows between `MinIconSpace` and `MaxIconSpace` over all
/// `total` cells that will stand in it. What `draw_strip` draws and what
/// `strip_cell_at` hits are the same rectangles because both come from here.
struct StripGeometry {
  std::int32_t icon_width = 69;
  std::int32_t icon_height = 76;
  std::int32_t space = 6;
  bool reverse = false;
};

StripGeometry strip_geometry(const Widget& widget, const Rect& rect, std::size_t cells) {
  StripGeometry out;
  std::int32_t min_space = 0;
  std::int32_t max_space = 6;
  (void)widget.attribute_int("IconWidth", out.icon_width);
  (void)widget.attribute_int("IconHeight", out.icon_height);
  (void)widget.attribute_int("MinIconSpace", min_space);
  (void)widget.attribute_int("MaxIconSpace", max_space);
  out.reverse = widget.reverse_draw;
  out.space = max_space;
  const std::int32_t total = static_cast<std::int32_t>(cells);
  if (total > 1 && out.icon_width > 0) {
    const std::int32_t fitting = (rect.width - total * out.icon_width) / (total - 1);
    out.space = std::clamp(fitting, min_space, max_space);
  }
  return out;
}

/// The rectangle of the `slot`-th cell of a row, or an empty one where the row
/// has run out of room and the cell is not drawn.
Rect strip_slot_rect(const StripGeometry& geometry, const Rect& rect, std::size_t slot) {
  if (geometry.icon_width <= 0) return Rect{};
  const std::int32_t offset = static_cast<std::int32_t>(slot) * (geometry.icon_width + geometry.space);
  const std::int32_t x = geometry.reverse ? rect.right() - geometry.icon_width - offset : rect.x + offset;
  if (x + geometry.icon_width > rect.right() + geometry.icon_width / 2 || x < rect.x - geometry.icon_width / 2) {
    return Rect{};
  }
  return Rect{x, rect.y, geometry.icon_width, geometry.icon_height};
}

/// The row of cells `BuildingQueue`, `UIHolder` and `UIInventory` draw, with
/// the keys those three sections share:
///
///     IconWidth / IconHeight        the cell
///     MinIconSpace / MaxIconSpace   the gap, as wide as the row allows
///     IconYPosition                 where the icon sits in the cell
///     NumberYPosition               where the number is drawn
///     HealthBarYPosition / HealthWidth / GradientImage   the bar
///     BackImage / BackGlowImage     under the icon (`UIInventory` has both)
///     NoSelFrameImage / SelFrameImage / TrainFrameImage / WaitFrameImage
///     Frame                         `UIInventory`'s one frame
///     ReverseDraw                   fill from the right edge
///
/// `first` is where the row starts, in cells -- `Combiner` draws its second
/// strip after its first. Returns how many cells were drawn, so that the
/// caller can continue.
std::size_t draw_strip(Canvas& canvas, ResourceCache& cache, const Widget& widget,
                       const Rect& rect, const Rect& clip, std::span<const StripCell> cells,
                       const Font* font, Color ink, std::size_t first) {
  if (cells.empty()) return 0;
  std::int32_t icon_y = 0;
  std::int32_t number_y = -1;
  std::int32_t bar_y = -1;
  std::int32_t bar_width = 0;
  (void)widget.attribute_int("IconYPosition", icon_y);
  (void)widget.attribute_int("NumberYPosition", number_y);
  (void)widget.attribute_int("HealthBarYPosition", bar_y);
  (void)widget.attribute_int("HealthWidth", bar_width);
  const StripGeometry geometry = strip_geometry(widget, rect, first + cells.size());
  if (geometry.icon_width <= 0) return 0;
  const std::int32_t icon_width = geometry.icon_width;
  const std::int32_t icon_height = geometry.icon_height;

  const ImageRef gradient = widget.attribute_image("GradientImage");
  const Image* back = cache.image(widget.attribute_image("BackImage"));
  const Image* glow = cache.image(widget.attribute_image("BackGlowImage"));
  const Image* frame_normal = cache.image(widget.attribute_image("NoSelFrameImage"));
  const Image* frame_selected = cache.image(widget.attribute_image("SelFrameImage"));
  const Image* frame_train = cache.image(widget.attribute_image("TrainFrameImage"));
  const Image* frame_wait = cache.image(widget.attribute_image("WaitFrameImage"));
  const Image* frame_only = cache.image(widget.attribute_image("Frame"));

  std::size_t drawn = 0;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    const Rect cell_rect = strip_slot_rect(geometry, rect, first + i);
    if (cell_rect.width <= 0) break;
    const std::int32_t x = cell_rect.x;
    const std::int32_t y = cell_rect.y;
    const StripCell& cell = cells[i];

    if (cell.glow && glow != nullptr) {
      canvas.blit(*glow, x, y, clip);
    } else if (back != nullptr) {
      canvas.blit(*back, x, y, clip);
    }
    if (const Image* icon = icon_image(cache, cell.icon)) {
      blit_centred(canvas, *icon, x, icon_width, y + icon_y, clip);
    }
    const Image* frame = frame_only;
    switch (cell.frame) {
      case StripCell::Frame::kNormal: if (frame_normal != nullptr) frame = frame_normal; break;
      case StripCell::Frame::kSelected: if (frame_selected != nullptr) frame = frame_selected; break;
      case StripCell::Frame::kTrain: if (frame_train != nullptr) frame = frame_train; break;
      case StripCell::Frame::kWait: if (frame_wait != nullptr) frame = frame_wait; break;
    }
    if (frame != nullptr) canvas.blit(*frame, x, y, clip);
    if (font != nullptr && !cell.number.empty() && number_y >= 0) {
      const std::int32_t width = font->measure(cell.number);
      canvas.draw_text(*font, cell.number, x + (icon_width - width) / 2, y + number_y, ink, clip);
    }
    if (cell.health >= 0 && bar_y >= 0 && bar_width > 0) {
      const std::int32_t height = std::max(1, icon_height - bar_y);
      const std::int32_t filled = bar_width * std::clamp<std::int32_t>(cell.health, 0, 100) / 100;
      canvas.fill_rect(intersect(Rect{x + (icon_width - bar_width) / 2, y + bar_y, filled, height}, clip),
                       ramp_color(cache, gradient, cell.health));
    }
    ++drawn;
  }
  return drawn;
}

/// `HeroSkills`: one framed icon per skill, `IconSpace` apart, with the
/// points at `IconTextOffset` from the cell's top-right corner and `PlusSign`
/// over a cell the hero can spend a point on.
void draw_skills(Canvas& canvas, ResourceCache& cache, const Widget& widget, const Rect& rect,
                 const Rect& clip, std::span<const StripCell> cells, const Font* font) {
  if (cells.empty()) return;
  const Image* frame = cache.image(widget.attribute_image("IconFrame"));
  const Image* plus = cache.image(widget.attribute_image("PlusSign"));
  std::int32_t space = 5;
  (void)widget.attribute_int("IconSpace", space);
  std::int32_t offset_x = 0;
  std::int32_t offset_y = 0;
  {
    const std::vector<std::string_view> parts = split_list(widget.attribute("IconTextOffset"));
    if (parts.size() >= 2) {
      (void)parse_int(parts[0], offset_x);
      (void)parse_int(parts[1], offset_y);
    }
  }
  const Color ink = parse_color(widget.attribute("IconTextColor"), Color{255, 255, 255, 255});
  const std::int32_t width = frame != nullptr ? static_cast<std::int32_t>(frame->width) : 40;
  const std::int32_t height = frame != nullptr ? static_cast<std::int32_t>(frame->height) : 40;
  const std::int32_t y = rect.y + (rect.height - height) / 2;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    const std::int32_t x = rect.x + static_cast<std::int32_t>(i) * (width + space);
    if (x + width > rect.right()) break;
    const StripCell& cell = cells[i];
    if (const Image* icon = icon_image(cache, cell.icon)) blit_centred(canvas, *icon, x, width, y, clip);
    if (frame != nullptr) canvas.blit(*frame, x, y, clip);
    if (font != nullptr && !cell.number.empty()) {
      const std::int32_t text_width = font->measure(cell.number);
      canvas.draw_text(*font, cell.number, x + width + offset_x - text_width, y + offset_y, ink, clip);
    }
    if (cell.plus && plus != nullptr) {
      canvas.blit(*plus, x + width - static_cast<std::int32_t>(plus->width), y, clip);
    }
  }
}

/// `UnitSpecials`: up to three lines, each an `Icon<i>RectWH` in `IconFrame`
/// and a `Text<i>RectWH` beside it, both relative to the control.
void draw_specials(Canvas& canvas, ResourceCache& cache, const Widget& widget, const Rect& rect,
                   const Rect& clip, std::span<const StripCell> cells, const Font* font,
                   Color ink) {
  const Image* frame = cache.image(widget.attribute_image("IconFrame"));
  for (std::size_t i = 0; i < cells.size() && i < 3; ++i) {
    const std::string index = std::to_string(i);
    const std::vector<std::string_view> icon_parts = split_list(widget.attribute("Icon" + index + "RectWH"));
    const std::vector<std::string_view> text_parts = split_list(widget.attribute("Text" + index + "RectWH"));
    if (icon_parts.size() < 4) continue;
    std::int32_t ix = 0, iy = 0, iw = 0, ih = 0;
    if (!parse_int(icon_parts[0], ix) || !parse_int(icon_parts[1], iy) ||
        !parse_int(icon_parts[2], iw) || !parse_int(icon_parts[3], ih)) {
      continue;
    }
    const StripCell& cell = cells[i];
    if (const Image* icon = icon_image(cache, cell.icon)) {
      canvas.blit(*icon, rect.x + ix, rect.y + iy, clip);
    }
    if (frame != nullptr) canvas.blit(*frame, rect.x + ix, rect.y + iy, clip);
    if (font != nullptr && !cell.text.empty() && text_parts.size() >= 4) {
      std::int32_t tx = 0, ty = 0, tw = 0, th = 0;
      if (parse_int(text_parts[0], tx) && parse_int(text_parts[1], ty) &&
          parse_int(text_parts[2], tw) && parse_int(text_parts[3], th)) {
        const std::int32_t baseline = rect.y + ty + (th - font->height()) / 2;
        canvas.draw_text(*font, cell.text, rect.x + tx, baseline, ink, clip);
      }
    }
  }
}

/// The strip a widget draws, by its class.
std::span<const StripCell> strip_of(const Widget& widget, const BarContent& content) {
  switch (widget.kind) {
    case WidgetType::kBuildingQueue: return content.queue;
    case WidgetType::kUIHolder: return content.holder;
    case WidgetType::kUIInventory: return content.items;
    default: return {};
  }
}

/// The slot a `Value = <n>` widget shows, or null.
const Slot* slot_of(const Widget& widget, const BarContent& content) {
  std::int32_t index = 0;
  if (!parse_int(widget.value, index) || index < 0) return nullptr;
  const std::size_t at = static_cast<std::size_t>(index);
  if (at >= content.values.size()) return nullptr;
  return content.values[at].present ? &content.values[at] : nullptr;
}

}  // namespace

namespace {

/// Visit every cell a visible strip draws in `layout`, as `paint_screen`
/// draws them: `visit(strip, index, rect)` with `strip` the strip whose cell
/// it is -- a `Combiner`'s cells are its two strips' -- until it returns true.
template <typename Visit>
bool for_each_strip_cell(const Layout& layout, const BarContent& content, Visit&& visit) {
  const auto walk = [&](const Widget& strip, const Rect& rect, std::size_t first) -> std::size_t {
    const std::span<const StripCell> cells = strip_of(strip, content);
    if (cells.empty()) return 0;
    const StripGeometry geometry = strip_geometry(strip, rect, first + cells.size());
    std::size_t drawn = 0;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      const Rect cell = strip_slot_rect(geometry, rect, first + i);
      if (cell.width <= 0) break;
      if (visit(strip, i, cell)) return ~std::size_t{0};
      ++drawn;
    }
    return drawn;
  };
  for (const LaidOutWidget& placed : layout.widgets) {
    const Widget& widget = *placed.widget;
    if (has(widget.style, Style::kHidden)) continue;
    if (!widget.visible_for(content.tags, content.tab)) continue;
    switch (widget.kind) {
      case WidgetType::kBuildingQueue:
      case WidgetType::kUIHolder:
      case WidgetType::kUIInventory:
        if (walk(widget, placed.rect, 0) == ~std::size_t{0}) return true;
        break;
      case WidgetType::kCombiner: {
        std::size_t drawn = 0;
        for (const char* key : {"Id1", "Id2"}) {
          const std::string_view name = widget.attribute(key);
          for (const LaidOutWidget& other : layout.widgets) {
            if (other.widget == nullptr || !iequal(other.widget->name, name)) continue;
            const std::size_t more = walk(*other.widget, placed.rect, drawn);
            if (more == ~std::size_t{0}) return true;
            drawn += more;
            break;
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return false;
}

}  // namespace

StripCellHit strip_cell_at(const Layout& layout, const BarContent& content, std::int32_t x,
                           std::int32_t y) {
  StripCellHit hit;
  (void)for_each_strip_cell(layout, content, [&](const Widget& strip, std::size_t index, const Rect& cell) {
    if (x < cell.x || x >= cell.right() || y < cell.y || y >= cell.bottom()) return false;
    hit.strip = &strip;
    hit.index = static_cast<std::int32_t>(index);
    return true;
  });
  return hit;
}

Rect strip_cell_rect(const Layout& layout, const BarContent& content, std::string_view strip,
                     std::size_t index) {
  Rect out;
  (void)for_each_strip_cell(layout, content, [&](const Widget& drawn, std::size_t i, const Rect& cell) {
    if (i != index || !iequal(drawn.name, strip)) return false;
    out = cell;
    return true;
  });
  return out;
}

void paint_screen(Canvas& canvas, const Layout& layout, ResourceCache& cache,
                  const BarContent& content) {
  const Rect screen{0, 0, static_cast<std::int32_t>(canvas.width()),
                    static_cast<std::int32_t>(canvas.height())};

  for (const LaidOutWidget& placed : layout.widgets) {
    const Widget& widget = *placed.widget;
    if (has(widget.style, Style::kHidden)) continue;
    if (!widget.visible_for(content.tags, content.tab)) continue;

    // A widget clips to its own rectangle. The bars rely on it: every skin's
    // `Background` is a 2048-wide bitmap in a rectangle the width of the screen.
    const Rect clip = intersect(placed.rect, screen);
    if (clip.width <= 0 || clip.height <= 0) continue;

    const Color ink = parse_color(widget.attribute("TextColor"), Color{255, 255, 255, 255});
    const Font* font = nullptr;
    const std::string_view font_path = widget.attribute("Font");
    if (!font_path.empty()) font = cache.font(font_path);

    switch (widget.kind) {
      case WidgetType::kThumbnail: {
        const Image* portrait = content.thumbnail;
        if (portrait == nullptr) portrait = icon_image(cache, content.thumbnail_path);
        if (portrait != nullptr) {
          blit_centred(canvas, *portrait, placed.rect.x, placed.rect.width, placed.rect.y, clip);
        }
        break;
      }
      case WidgetType::kSelectionName: {
        if (font != nullptr && !content.name.empty()) {
          const std::int32_t baseline =
              placed.rect.y + (placed.rect.height - font->height()) / 2;
          canvas.draw_text(*font, content.name, placed.rect.x, baseline, ink, clip);
        }
        break;
      }
      case WidgetType::kSelectionHealth: {
        if (content.health >= 0) {
          // The gradient bitmap is a ramp sampled by health; see `ramp_color`.
          // Without one the bar still reads, so a missing file degrades to a
          // flat fill rather than to nothing.
          const std::int32_t filled =
              placed.rect.width * std::min<std::int32_t>(content.health, 100) / 100;
          canvas.fill_rect(Rect{placed.rect.x, placed.rect.y, filled, placed.rect.height},
                           ramp_color(cache, widget.attribute_image("GradientImage"), content.health));
        }
        break;
      }
      case WidgetType::kInfobarIcon: {
        const Slot* slot = slot_of(widget, content);
        if (slot == nullptr) break;
        const Image* frame = cache.image(widget.frame);
        if (frame != nullptr) canvas.blit(*frame, placed.rect.x, placed.rect.y, clip);
        if (!slot->icon.empty()) {
          ImageRef icon;
          icon.path = resolve_alias(slot->icon);
          const Image* image = cache.image(icon);
          if (image != nullptr) canvas.blit(*image, placed.rect.x, placed.rect.y, clip);
        }
        break;
      }
      case WidgetType::kInfobarText: {
        const Slot* slot = slot_of(widget, content);
        if (slot == nullptr || font == nullptr || slot->text.empty()) break;
        const std::int32_t baseline = placed.rect.y + (placed.rect.height - font->height()) / 2;
        canvas.draw_text(*font, slot->text, placed.rect.x, baseline, ink, clip);
        break;
      }
      // The queue, the holder, the inventory, the hero's skills and a unit's
      // specials draw a list of icons that only the simulation can supply,
      // and `BarContent` carries them. An empty list draws nothing: the
      // original shows no empty furniture.
      case WidgetType::kBuildingQueue:
      case WidgetType::kUIHolder:
      case WidgetType::kUIInventory: {
        (void)draw_strip(canvas, cache, widget, placed.rect, clip, strip_of(widget, content), font,
                         ink, 0);
        break;
      }
      case WidgetType::kCombiner: {
        // `Id1` and `Id2` name two strips of this screen; the second continues
        // the row the first began, each drawn with its own keys.
        std::size_t drawn = 0;
        for (const char* key : {"Id1", "Id2"}) {
          const std::string_view name = widget.attribute(key);
          for (const LaidOutWidget& other : layout.widgets) {
            if (other.widget == nullptr || !iequal(other.widget->name, name)) continue;
            const Font* other_font = nullptr;
            const std::string_view other_font_path = other.widget->attribute("Font");
            if (!other_font_path.empty()) other_font = cache.font(other_font_path);
            const Color other_ink =
                parse_color(other.widget->attribute("TextColor"), Color{255, 255, 255, 255});
            drawn += draw_strip(canvas, cache, *other.widget, placed.rect, clip,
                                strip_of(*other.widget, content), other_font, other_ink, drawn);
            break;
          }
        }
        break;
      }
      case WidgetType::kHeroSkills:
        draw_skills(canvas, cache, widget, placed.rect, clip, content.skills, font);
        break;
      case WidgetType::kUnitSpecials:
        draw_specials(canvas, cache, widget, placed.rect, clip, content.specials, font, ink);
        break;
      case WidgetType::kButton:
      case WidgetType::kIcon:
      case WidgetType::kSwitch:
      case WidgetType::kUnknown:
      default: {
        const Image* image = current_image(widget, cache, content);
        if (image != nullptr) canvas.blit(*image, placed.rect.x, placed.rect.y, clip);
        if (font != nullptr && !widget.text.empty()) {
          const std::int32_t baseline = placed.rect.y + (placed.rect.height - font->height()) / 2;
          canvas.draw_text(*font, content.text_of(widget.text), placed.rect.x, baseline, ink, clip);
        }
        break;
      }
    }
  }
}

// -- the menus ---------------------------------------------------------------

const WidgetState* DialogContent::state_of(std::string_view name) const noexcept {
  for (const WidgetState& entry : states) {
    if (iequal(entry.name, name)) return &entry;
  }
  return nullptr;
}

WidgetState& DialogContent::state(std::string_view name) {
  for (WidgetState& entry : states) {
    if (iequal(entry.name, name)) return entry;
  }
  states.push_back(WidgetState{});
  states.back().name.assign(name);
  return states.back();
}

namespace {

/// `ID_CAPTION`, `ID_LBULLET`, `ID_RBULLET` from `MENU/TEMPLATE.INI`'s
/// `[Params]`: the caption a dialog's bullets flank.
constexpr std::int32_t kIdCaption = 0x1007f;
constexpr std::int32_t kIdLeftBullet = 0x75ee;
constexpr std::int32_t kIdRightBullet = 0x75ef;

ControlState dialog_state_of(const Widget& widget, const DialogContent& content,
                             const WidgetState* state) {
  if (has(widget.style, Style::kDisabled) || (state != nullptr && state->disabled)) {
    return ControlState::kDisabled;
  }
  if (!content.pressed.empty() && iequal(content.pressed, widget.name)) {
    return ControlState::kPressed;
  }
  if (!content.hovered.empty() && iequal(content.hovered, widget.name)) {
    return ControlState::kHighlighted;
  }
  return ControlState::kNormal;
}

/// The cell of a widget's bitmap for its state and row. An `ImageButton`
/// declares the grid as `XFrames` x `YFrames`; a `Button` implies the
/// columns from its `ImageType` and the rows from `Rows`.
const Image* dialog_image(const Widget& widget, ResourceCache& cache, ControlState state,
                          std::int32_t row_override) {
  if (widget.image.empty()) return nullptr;
  std::uint32_t columns = std::max(1u, image_type_frames(widget.image_type));
  if (widget.xframes > 0) columns = static_cast<std::uint32_t>(widget.xframes);
  std::uint32_t rows = static_cast<std::uint32_t>(std::max(1, widget.rows));
  if (widget.yframes > 0) rows = static_cast<std::uint32_t>(widget.yframes);
  const std::uint32_t column =
      widget.image_type.empty()
          ? 0u
          : std::min(columns - 1,
                     image_type_frame(widget.image_type, static_cast<std::uint32_t>(state)));
  const std::int32_t wanted = row_override >= 0 ? row_override : widget.initial_row;
  const std::uint32_t row = std::min(rows - 1, static_cast<std::uint32_t>(std::max(0, wanted)));
  return cache.image_frame(widget.image, column, columns, row, rows);
}

/// Break `text` into lines no wider than `width`, at spaces; a word wider
/// than the line stands alone. A line break in the text breaks too, and so
/// does the two-character `\n` an `.ini` value carries -- `GAMEOPTIONS.INI`
/// writes `Turn off object animations\n(saves CPU time)`.
std::vector<std::string> wrap_lines(const Font& font, std::string_view source, std::int32_t width) {
  std::string text;
  text.reserve(source.size());
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\\' && i + 1 < source.size() && source[i + 1] == 'n') {
      text.push_back('\n');
      ++i;
    } else {
      text.push_back(source[i]);
    }
  }
  std::vector<std::string> lines;
  std::string line;
  std::size_t i = 0;
  while (i <= text.size()) {
    std::size_t end = i;
    while (end < text.size() && text[end] != ' ' && text[end] != '\n') ++end;
    const std::string_view word = std::string_view{text}.substr(i, end - i);
    std::string candidate = line;
    if (!candidate.empty()) candidate += ' ';
    candidate.append(word);
    if (!line.empty() && font.measure(candidate) > width) {
      lines.push_back(line);
      line.assign(word);
    } else {
      line = std::move(candidate);
    }
    if (end < text.size() && text[end] == '\n') {
      lines.push_back(line);
      line.clear();
    }
    i = end + 1;
  }
  lines.push_back(line);
  return lines;
}

/// Text in a rectangle: `ALIGN_LEFT`/`ALIGN_RIGHT`/`ALIGN_CENTER`, one line
/// vertically centred, or `MULTILINE` wrapped at `Width` (the rectangle's
/// width without one) and set from the top. **Reading, labelled:** where a
/// single line sits vertically is not in the file; centred is what the
/// 23-tall `StaticText` rectangles over a 14-pixel font look right with.
void draw_text_block(Canvas& canvas, const Font& font, std::string_view text, const Rect& rect,
                     Style style, Color ink, const Rect& clip, std::int32_t wrap_width) {
  if (text.empty()) return;
  const bool multiline = has(style, Style::kMultiline);
  std::vector<std::string> lines;
  if (multiline) {
    lines = wrap_lines(font, text, wrap_width > 0 ? wrap_width : rect.width);
  } else {
    lines.emplace_back(text);
  }
  const std::int32_t line_height = font.height();
  std::int32_t y = multiline ? rect.y : rect.y + (rect.height - line_height) / 2;
  for (const std::string& line : lines) {
    const std::int32_t width = font.measure(line);
    std::int32_t x = rect.x;
    if (has(style, Style::kAlignCenter)) {
      x = rect.x + (rect.width - width) / 2;
    } else if (has(style, Style::kAlignRight)) {
      x = rect.right() - width;
    }
    canvas.draw_text(font, line, x, y, ink, clip);
    y += line_height;
  }
}

/// A `Frame`: nine pieces of `Image` cut at `Dividers`, corners as they are,
/// edges and the middle tiled. Without dividers the bitmap is drawn once.
/// **Reading, labelled:** tiled rather than stretched. Every shipped frame's
/// edge bands are a constant colour across their run and its middle is the
/// colour key, so the two are indistinguishable on the retail art; a 16-bit
/// software rasteriser tiles.
void draw_frame(Canvas& canvas, const Image& image, const Widget& widget, const Rect& rect,
                const Rect& clip) {
  const std::int32_t iw = static_cast<std::int32_t>(image.width);
  const std::int32_t ih = static_cast<std::int32_t>(image.height);
  if (!widget.has_dividers) {
    canvas.blit(image, rect.x, rect.y, clip);
    return;
  }
  // The executable's clamp (0x00663570): a left wider than the bitmap is the
  // bitmap, a middle that would pass its edge is cut there.
  const std::int32_t left = std::clamp(widget.divider_left, 0, iw);
  const std::int32_t middle = std::clamp(widget.divider_middle, 0, iw - left);
  const std::int32_t right = iw - left - middle;
  const std::int32_t top = std::clamp(widget.divider_top, 0, ih);
  const std::int32_t middle_h = std::clamp(widget.divider_middle_height, 0, ih - top);
  const std::int32_t bottom = ih - top - middle_h;

  const std::int32_t inner_w = std::max(0, rect.width - left - right);
  const std::int32_t inner_h = std::max(0, rect.height - top - bottom);
  const std::int32_t x0 = rect.x;
  const std::int32_t x1 = rect.x + left;
  const std::int32_t x2 = rect.x + left + inner_w;
  const std::int32_t y0 = rect.y;
  const std::int32_t y1 = rect.y + top;
  const std::int32_t y2 = rect.y + top + inner_h;
  const Rect inner_clip = intersect(rect, clip);

  // Corners.
  canvas.blit_region(image, Rect{0, 0, left, top}, x0, y0, inner_clip);
  canvas.blit_region(image, Rect{left + middle, 0, right, top}, x2, y0, inner_clip);
  canvas.blit_region(image, Rect{0, top + middle_h, left, bottom}, x0, y2, inner_clip);
  canvas.blit_region(image, Rect{left + middle, top + middle_h, right, bottom}, x2, y2,
                     inner_clip);
  // Edges.
  if (middle > 0) {
    canvas.tile_region(image, Rect{left, 0, middle, top}, Rect{x1, y0, inner_w, top}, inner_clip);
    canvas.tile_region(image, Rect{left, top + middle_h, middle, bottom},
                       Rect{x1, y2, inner_w, bottom}, inner_clip);
  }
  if (middle_h > 0) {
    canvas.tile_region(image, Rect{0, top, left, middle_h}, Rect{x0, y1, left, inner_h}, inner_clip);
    canvas.tile_region(image, Rect{left + middle, top, right, middle_h},
                       Rect{x2, y1, right, inner_h}, inner_clip);
  }
  // The middle.
  if (middle > 0 && middle_h > 0) {
    canvas.tile_region(image, Rect{left, top, middle, middle_h}, Rect{x1, y1, inner_w, inner_h},
                       inner_clip);
  }
}

/// A `Background`: `BkColor` filled, then a two-pixel bevel. **Reading,
/// labelled:** which of `FrameColor1..4` goes where is not in the data;
/// this puts 1 and 2 on the outer top-left and bottom-right and 3 and 4 on
/// the inner ones, which draws `ShadowFrame`'s tan/white/brown/cream as a
/// raised panel.
void draw_background(Canvas& canvas, const Widget& widget, const Rect& rect, const Rect& clip,
                     const WidgetState* state) {
  const Rect area = intersect(rect, clip);
  if (area.width <= 0 || area.height <= 0) return;
  const std::string_view back = widget.attribute("BkColor");
  if (state != nullptr && state->has_color) {
    canvas.fill_rect(area, state->color);
  } else if (!back.empty()) {
    canvas.fill_rect(area, parse_color(back));
  }
  const Color none{0, 0, 0, 0};
  const Color outer_tl = parse_color(widget.attribute("FrameColor1"), none);
  const Color outer_br = parse_color(widget.attribute("FrameColor2"), none);
  const Color inner_tl = parse_color(widget.attribute("FrameColor3"), none);
  const Color inner_br = parse_color(widget.attribute("FrameColor4"), none);
  const auto edge = [&](std::int32_t inset, Color tl, Color br) {
    if (tl.alpha == 0 && br.alpha == 0) return;
    const Rect r{rect.x + inset, rect.y + inset, rect.width - 2 * inset, rect.height - 2 * inset};
    if (r.width <= 0 || r.height <= 0) return;
    if (tl.alpha != 0) {
      canvas.fill_rect(intersect(Rect{r.x, r.y, r.width, 1}, clip), tl);
      canvas.fill_rect(intersect(Rect{r.x, r.y, 1, r.height}, clip), tl);
    }
    if (br.alpha != 0) {
      canvas.fill_rect(intersect(Rect{r.x, r.bottom() - 1, r.width, 1}, clip), br);
      canvas.fill_rect(intersect(Rect{r.right() - 1, r.y, 1, r.height}, clip), br);
    }
  };
  edge(0, outer_tl, outer_br);
  edge(1, inner_tl, inner_br);
}

/// An item's lines: split at line breaks (the two-character `\n` included)
/// and, with a font and a width, wrapped at the width -- the help's
/// paragraphs are one entry each.
std::vector<std::string> item_lines(std::string_view item, const Font* font, std::int32_t width) {
  std::vector<std::string> lines(1);
  for (std::size_t i = 0; i < item.size(); ++i) {
    if (item[i] == '\n' || (item[i] == '\\' && i + 1 < item.size() && item[i + 1] == 'n')) {
      lines.emplace_back();
      if (item[i] == '\\') ++i;
    } else {
      lines.back().push_back(item[i]);
    }
  }
  if (font == nullptr || width <= 0) return lines;
  std::vector<std::string> wrapped;
  for (const std::string& line : lines) {
    if (line.empty()) {
      wrapped.emplace_back();
      continue;
    }
    for (std::string& piece : wrap_lines(*font, line, width)) wrapped.push_back(std::move(piece));
  }
  return wrapped;
}

/// The font an item is set in: the list's `BoldFont` for a large one.
const Font* item_font(ResourceCache& cache, const Widget& widget, const WidgetState& state,
                      std::size_t index, const Font* normal) {
  if (index < state.item_flags.size() && (state.item_flags[index] & WidgetState::kItemLarge) != 0) {
    if (const std::string_view path = widget.attribute("BoldFont"); !path.empty()) {
      if (const Font* bold = cache.font(path)) return bold;
    }
  }
  return normal;
}

/// Where an item's text starts: the list's `TextOffs` when it declares one
/// (the notes list), else beside the item's own icon, six pixels on (the
/// help's rows, whose list declares no offsets), else two pixels in.
std::int32_t item_text_offset(ResourceCache& cache, const Widget& widget, const WidgetState& state,
                              std::size_t index) {
  std::int32_t declared = 0;
  if (!state.icons.empty() && widget.attribute_int("TextOffs", declared)) return declared;
  if (index < state.icons.size() && !state.icons[index].empty()) {
    if (const Image* icon = icon_image(cache, state.icons[index])) {
      return static_cast<std::int32_t>(icon->width) + 6;
    }
  }
  return 2;
}

/// Rows of text. The line height is the font's (`AUTOCALC`); an item is as
/// tall as its lines, or its icon. A plain list fills the selected item
/// with `SelectedBkColor` and sets it in `SelectedFontColor`. A list with
/// icons -- the notes -- is read differently, from `NOTES.INI`'s own
/// comments: `SelectedFontColor` is the *title colour*, every item's first
/// line; `FocusFontColor` "defines the color of the selected items", so
/// the selected item's text is set in it; and nothing is filled, because
/// the template's `224,224,224` under a `228,220,140` title would be
/// unreadable and the file says its scrolls are "not setup to work"
/// without the C++ side. **Reading, labelled.** Icons at `IconOffs`, text
/// at `TextOffs`.
void draw_list(Canvas& canvas, ResourceCache& cache, const Widget& widget, const Rect& rect,
               const Rect& clip, const WidgetState* state, const DialogContent& content) {
  const Font* font = nullptr;
  if (const std::string_view path = widget.attribute("Font"); !path.empty()) font = cache.font(path);
  if (font == nullptr && !content.default_font.empty()) font = cache.font(content.default_font);
  if (font == nullptr || state == nullptr || state->items.empty()) return;
  const std::int32_t line_height = list_row_height(cache, widget);
  const Color ink = parse_color(widget.attribute("FontColor"), content.default_ink);
  const Color selected_ink = parse_color(widget.attribute("SelectedFontColor"), ink);
  const Color focus_ink = parse_color(widget.attribute("FocusFontColor"), selected_ink);
  Color selected_back = parse_color(widget.attribute("SelectedBkColor"), Color{0, 0, 0, 0});
  std::int32_t icon_offset = 0;
  const bool with_icons = !state->icons.empty();
  if (with_icons) {
    (void)widget.attribute_int("IconOffs", icon_offset);
    selected_back = Color{0, 0, 0, 0};
  }
  const Rect inner = intersect(rect, clip);
  std::int32_t y = rect.y;
  for (std::size_t i = static_cast<std::size_t>(std::max(0, state->scroll)); i < state->items.size(); ++i) {
    const std::int32_t height = list_item_height(cache, widget, *state, i, rect.width);
    if (y + height > rect.bottom()) break;
    const bool selected = static_cast<std::int32_t>(i) == state->selected;
    const Rect row{rect.x, y, rect.width, height};
    if (selected && selected_back.alpha != 0) {
      canvas.fill_rect(intersect(row, inner), selected_back);
    }
    const std::int32_t text_offset = item_text_offset(cache, widget, *state, i);
    const std::int32_t text_width = rect.width - text_offset - 2;
    if (with_icons && i < state->icons.size()) {
      if (const Image* icon = icon_image(cache, state->icons[i])) {
        // Vertically centred on its row when the row is taller.
        const std::int32_t icon_y = y + std::max(0, (height - static_cast<std::int32_t>(icon->height)) / 2);
        canvas.blit(*icon, rect.x + icon_offset, icon_y, inner);
      }
    }
    const Font* item_face = item_font(cache, widget, *state, i, font);
    const bool centred = i < state->item_flags.size() && (state->item_flags[i] & WidgetState::kItemCentred) != 0;
    const bool wrap = !has(widget.style, Style::kNoWordWrap);
    const std::vector<std::string> lines = item_lines(content.text_of(state->items[i]), wrap ? item_face : nullptr, text_width);
    const std::int32_t item_line = item_face->height() + 2;
    std::int32_t line_y = y + (lines.size() == 1 ? (height - item_face->height()) / 2 : 0);
    for (std::size_t line = 0; line < lines.size(); ++line) {
      Color colour = selected ? selected_ink : ink;
      if (with_icons) colour = line == 0 ? selected_ink : (selected ? focus_ink : ink);
      std::int32_t x = rect.x + text_offset;
      if (centred) x = rect.x + text_offset + (text_width - item_face->measure(lines[line])) / 2;
      canvas.draw_text(*item_face, lines[line], x, line_y, colour, inner);
      line_y += item_line;
    }
    y += height;
  }
}

/// A `Scroll`: the `Thumb` bitmap, at the fraction of its run that its
/// `TargetId` list has scrolled.
void draw_scroll(Canvas& canvas, ResourceCache& cache, const Widget& widget, const Rect& rect,
                 const Rect& clip, const Layout& layout, const DialogContent& content,
                 ControlState state) {
  const ImageRef thumb_ref = widget.attribute_image("Thumb");
  const std::uint32_t columns = std::max(1u, image_type_frames(widget.image_type));
  const std::uint32_t column = widget.image_type.empty()
                                   ? 0u
                                   : std::min(columns - 1, image_type_frame(widget.image_type,
                                                                            static_cast<std::uint32_t>(state)));
  const Image* thumb = cache.image_frame(thumb_ref, column, columns, 0, 1);
  if (thumb == nullptr) return;
  std::int32_t numerator = 0;
  std::int32_t denominator = 1;
  const std::string_view target = widget.attribute("TargetId");
  if (target.empty()) {
    // A slider: the thumb at the state's value.
    if (const WidgetState* state = content.state_of(widget.name)) {
      numerator = std::clamp(state->value, 0, 100);
      denominator = 100;
    }
  }
  for (const LaidOutWidget& placed : layout.widgets) {
    if (placed.widget == nullptr || !iequal(placed.widget->name, target)) continue;
    if (const WidgetState* list = content.state_of(target)) {
      const std::int32_t visible = list_visible_rows(cache, *placed.widget, placed.rect, list);
      const std::int32_t hidden = static_cast<std::int32_t>(list->items.size()) - visible;
      if (hidden > 0) {
        numerator = std::clamp(list->scroll, 0, hidden);
        denominator = hidden;
      }
    }
    break;
  }
  // `AUTOHIDE`: no thumb when the target has nothing to scroll.
  if (has(widget.style, Style::kAutoHide) && !target.empty() && denominator <= 1 && numerator == 0) {
    bool scrollable = false;
    for (const LaidOutWidget& placed : layout.widgets) {
      if (placed.widget == nullptr || !iequal(placed.widget->name, target)) continue;
      if (const WidgetState* list = content.state_of(target)) {
        scrollable = static_cast<std::int32_t>(list->items.size()) >
                     list_visible_rows(cache, *placed.widget, placed.rect, list);
      }
    }
    if (!scrollable) return;
  }
  const bool vertical = has(widget.style, Style::kVScroll);
  const std::int32_t run = vertical ? rect.height - static_cast<std::int32_t>(thumb->height)
                                    : rect.width - static_cast<std::int32_t>(thumb->width);
  const std::int32_t offset = run > 0 ? run * numerator / denominator : 0;
  canvas.blit(*thumb, rect.x + (vertical ? 0 : offset), rect.y + (vertical ? offset : 0), clip);
}

/// A `Combobox`: its closed box -- `BkColor` with a `FrameColor1` line, the
/// current text, and `ButtonImage` at the right edge -- and, when open, its
/// list below in the same colours.
void draw_combobox(Canvas& canvas, ResourceCache& cache, const Widget& widget, const Rect& rect,
                   const Rect& clip, const WidgetState* state, const DialogContent& content) {
  const Font* font = nullptr;
  if (const std::string_view path = widget.attribute("Font"); !path.empty()) font = cache.font(path);
  const std::int32_t closed = combobox_closed_height(cache, widget);
  const Rect box{rect.x, rect.y, rect.width, closed};
  const Color back = parse_color(widget.attribute("BkColor"), Color{32, 32, 32, 255});
  const Color line = parse_color(widget.attribute("FrameColor1"), Color{220, 189, 129, 255});
  const Color ink = parse_color(widget.attribute("TextColor"), content.default_ink);
  const auto outline = [&](const Rect& r) {
    canvas.fill_rect(intersect(r, clip), back);
    canvas.fill_rect(intersect(Rect{r.x, r.y, r.width, 1}, clip), line);
    canvas.fill_rect(intersect(Rect{r.x, r.bottom() - 1, r.width, 1}, clip), line);
    canvas.fill_rect(intersect(Rect{r.x, r.y, 1, r.height}, clip), line);
    canvas.fill_rect(intersect(Rect{r.right() - 1, r.y, 1, r.height}, clip), line);
  };
  outline(box);
  const Image* arrow = cache.image_frame(widget.attribute_image("ButtonImage"), 0, 3, 0, 1);
  std::int32_t arrow_width = 0;
  if (arrow != nullptr) {
    arrow_width = static_cast<std::int32_t>(arrow->width);
    canvas.blit(*arrow, box.right() - arrow_width - 1,
                box.y + (box.height - static_cast<std::int32_t>(arrow->height)) / 2, clip);
  }
  std::string_view text;
  if (state != nullptr) {
    if (state->has_text) {
      text = state->text;
    } else if (state->selected >= 0 &&
               static_cast<std::size_t>(state->selected) < state->items.size()) {
      text = content.text_of(state->items[static_cast<std::size_t>(state->selected)]);
    }
  }
  if (font != nullptr && !text.empty()) {
    const Rect text_clip = intersect(Rect{box.x + 1, box.y, box.width - arrow_width - 2, box.height}, clip);
    canvas.draw_text(*font, text, box.x + 4, box.y + (box.height - font->height()) / 2, ink, text_clip);
  }
  if (state != nullptr && state->open && font != nullptr && !state->items.empty()) {
    const std::int32_t row_height = font->height() + 2;
    // `AUTOSIZE`: the list is as tall as its items; otherwise as tall as the
    // rectangle below the box allows.
    const std::int32_t rows = has(widget.style, Style::kAutosize)
                                  ? static_cast<std::int32_t>(state->items.size())
                                  : std::max(1, (rect.height - closed) / row_height);
    const Rect list{rect.x, box.bottom(), rect.width, std::min(rows, static_cast<std::int32_t>(state->items.size())) * row_height + 2};
    outline(list);
    const Color selected_back = parse_color(widget.attribute("SelBgColor"), Color{128, 128, 128, 255});
    const Color selected_ink = parse_color(widget.attribute("SelColor"), ink);
    std::int32_t y = list.y + 1;
    for (std::size_t i = static_cast<std::size_t>(std::max(0, state->scroll)); i < state->items.size(); ++i) {
      if (y + row_height > list.bottom()) break;
      const bool selected = static_cast<std::int32_t>(i) == state->selected;
      if (selected) canvas.fill_rect(intersect(Rect{list.x + 1, y, list.width - 2, row_height}, clip), selected_back);
      canvas.draw_text(*font, content.text_of(state->items[i]), list.x + 4, y + 1,
                       selected ? selected_ink : ink, intersect(list, clip));
      y += row_height;
    }
  }
}

/// A `VXMenuBack`: a `Frame` grown by its four `...Offs`, a title at
/// `TextYPos` between `LeftImage` and `RightImage`, `BulletTextDist` from
/// the text and `BulletYPos` from its top.
void draw_menu_back(Canvas& canvas, ResourceCache& cache, const Widget& widget, const Rect& rect,
                    const Rect& clip, const WidgetState* state, const DialogContent& content) {
  std::int32_t top = 0, left = 0, bottom = 0, right = 0;
  (void)widget.attribute_int("topOffs", top);
  (void)widget.attribute_int("leftOffs", left);
  (void)widget.attribute_int("bottomOffs", bottom);
  (void)widget.attribute_int("rightOffs", right);
  const Rect grown{rect.x + left, rect.y + top, rect.width - left + right, rect.height - top + bottom};
  const Rect grown_clip = intersect(grown, Rect{0, 0, static_cast<std::int32_t>(canvas.width()),
                                                 static_cast<std::int32_t>(canvas.height())});
  if (const Image* frame = cache.image(widget.image)) draw_frame(canvas, *frame, widget, grown, grown_clip);
  std::string_view title = widget.text;
  const bool declared = state == nullptr || !state->has_text;
  if (!declared) title = state->text;
  if (title.empty()) return;
  const Font* font = nullptr;
  if (const std::string_view path = widget.attribute("Font"); !path.empty()) font = cache.font(path);
  if (font == nullptr) return;
  std::int32_t text_y = 7, bullet_y = -2, distance = 7;
  (void)widget.attribute_int("TextYPos", text_y);
  (void)widget.attribute_int("BulletYPos", bullet_y);
  (void)widget.attribute_int("BulletTextDist", distance);
  const std::string_view text = declared ? content.text_of(title, widget.name, "Text") : title;
  const std::int32_t width = font->measure(text);
  const std::int32_t x = rect.x + (rect.width - width) / 2;
  const std::int32_t y = rect.y + text_y;
  const Color ink = parse_color(widget.attribute("TextColor"), Color{255, 255, 255, 255});
  canvas.draw_text(*font, text, x, y, ink, grown_clip);
  if (const Image* bullet = cache.image(widget.attribute_image("LeftImage"))) {
    canvas.blit(*bullet, x - distance - static_cast<std::int32_t>(bullet->width), y + bullet_y, grown_clip);
  }
  if (const Image* bullet = cache.image(widget.attribute_image("RightImage"))) {
    canvas.blit(*bullet, x + width + distance, y + bullet_y, grown_clip);
  }
}

}  // namespace

std::int32_t text_block_height(const Font& font, std::string_view text, std::int32_t width) {
  return static_cast<std::int32_t>(wrap_lines(font, text, width).size()) * font.height();
}

std::int32_t list_row_height(ResourceCache& cache, const Widget& widget) {
  const Font* font = nullptr;
  if (const std::string_view path = widget.attribute("Font"); !path.empty()) font = cache.font(path);
  std::int32_t height = font != nullptr ? font->height() + 2 : 16;
  (void)widget.attribute_int("ItemHeight", height);
  return std::max(1, height);
}

std::int32_t list_item_height(ResourceCache& cache, const Widget& widget, const WidgetState& state,
                              std::size_t index, std::int32_t width) {
  const std::int32_t line = list_row_height(cache, widget);
  if (index >= state.items.size()) return line;
  const Font* font = nullptr;
  if (const std::string_view path = widget.attribute("Font"); !path.empty()) font = cache.font(path);
  const Font* face = item_font(cache, widget, state, index, font);
  const std::int32_t text_offset = item_text_offset(cache, widget, state, index);
  const bool wrap = !has(widget.style, Style::kNoWordWrap);
  const std::vector<std::string> lines = item_lines(state.items[index], wrap ? face : nullptr, width - text_offset - 2);
  const std::int32_t item_line = face != nullptr ? face->height() + 2 : line;
  std::int32_t height = static_cast<std::int32_t>(lines.size()) * item_line;
  if (index < state.icons.size()) {
    if (const Image* icon = icon_image(cache, state.icons[index])) {
      height = std::max(height, static_cast<std::int32_t>(icon->height));
    }
  }
  return std::max(1, height);
}

std::int32_t list_visible_rows(ResourceCache& cache, const Widget& widget, const Rect& rect,
                               const WidgetState* state) {
  if (state == nullptr || state->items.empty()) {
    return std::max(0, rect.height / list_row_height(cache, widget));
  }
  std::int32_t y = 0;
  std::int32_t rows = 0;
  for (std::size_t i = static_cast<std::size_t>(std::max(0, state->scroll)); i < state->items.size(); ++i) {
    y += list_item_height(cache, widget, *state, i, rect.width);
    if (y > rect.height) break;
    ++rows;
  }
  return rows;
}

std::int32_t list_item_at(ResourceCache& cache, const Widget& widget, const Rect& rect,
                          const WidgetState& state, std::int32_t y) {
  std::int32_t top = rect.y;
  for (std::size_t i = static_cast<std::size_t>(std::max(0, state.scroll)); i < state.items.size(); ++i) {
    const std::int32_t height = list_item_height(cache, widget, state, i, rect.width);
    if (top + height > rect.bottom()) break;
    if (y >= top && y < top + height) return static_cast<std::int32_t>(i);
    top += height;
  }
  return -1;
}

std::int32_t combobox_closed_height(ResourceCache& cache, const Widget& widget) {
  const Image* arrow = cache.image_frame(widget.attribute_image("ButtonImage"), 0, 3, 0, 1);
  return arrow != nullptr ? static_cast<std::int32_t>(arrow->height) + 2 : 20;
}

Rect bullet_rect(ResourceCache& cache, const Layout& layout, const DialogContent& content,
                 const Widget& bullet) {
  if (!bullet.has_id || (bullet.id != kIdLeftBullet && bullet.id != kIdRightBullet)) return Rect{};
  const Image* image = cache.image(bullet.image);
  if (image == nullptr) return Rect{};
  for (const LaidOutWidget& placed : layout.widgets) {
    const Widget* caption = placed.widget;
    if (caption == nullptr || !caption->has_id || caption->id != kIdCaption) continue;
    const Font* font = nullptr;
    if (const std::string_view path = caption->attribute("Font"); !path.empty()) font = cache.font(path);
    if (font == nullptr) return Rect{};
    std::string_view text = caption->text;
    bool declared = true;
    if (const WidgetState* state = content.state_of(caption->name); state != nullptr && state->has_text) {
      text = state->text;
      declared = false;
    }
    const std::int32_t width = font->measure(declared ? content.text_of(text, caption->name, "Text") : text);
    std::int32_t x = placed.rect.x;
    if (has(caption->style, Style::kAlignCenter)) {
      x = placed.rect.x + (placed.rect.width - width) / 2;
    } else if (has(caption->style, Style::kAlignRight)) {
      x = placed.rect.right() - width;
    }
    constexpr std::int32_t kDistance = 7;  // `BulletTextDist` of `MenuFrame`
    const std::int32_t w = static_cast<std::int32_t>(image->width);
    const std::int32_t h = static_cast<std::int32_t>(image->height);
    const std::int32_t y = placed.rect.y + (placed.rect.height - h) / 2;
    return bullet.id == kIdLeftBullet ? Rect{x - kDistance - w, y, w, h}
                                      : Rect{x + width + kDistance, y, w, h};
  }
  return Rect{};
}

bool acts_as_list(const Widget& widget, const WidgetState* state) noexcept {
  if (widget.kind == WidgetType::kList) return true;
  return widget.kind == WidgetType::kControl && state != nullptr && !state->items.empty();
}

std::vector<std::int32_t> brush_frames(const Widget& widget, std::uint32_t columns) {
  std::vector<std::int32_t> frames;
  for (const char c : widget.attribute("Frames")) {
    if (c >= '1' && c <= '9' && static_cast<std::uint32_t>(c - '0') <= columns) frames.push_back(c - '0');
  }
  if (frames.empty()) {
    for (std::uint32_t k = 1; k <= columns; ++k) frames.push_back(static_cast<std::int32_t>(k));
  }
  return frames;
}

std::int32_t brush_item_width(const Widget& widget) noexcept {
  std::int32_t width = 42;
  (void)widget.attribute_int("ItemWidth", width);
  return std::max(1, width);
}

std::int32_t brush_item_height(const Widget& widget) noexcept {
  std::int32_t height = 42;
  (void)widget.attribute_int("ItemHeight", height);
  return std::max(1, height);
}

/// The editor's brush picker: the offered columns of the strip side by side,
/// the chosen one from the second row.
void draw_brush_size(Canvas& canvas, ResourceCache& cache, const Widget& widget, const Rect& rect,
                     const Rect& clip, const WidgetState* state) {
  const Image* image = cache.image(widget.image);
  if (image == nullptr) return;
  const std::int32_t item_width = brush_item_width(widget);
  const std::int32_t item_height = brush_item_height(widget);
  const std::uint32_t columns = std::max(1u, image->width / static_cast<std::uint32_t>(item_width));
  const std::uint32_t rows = std::max(1u, image->height / static_cast<std::uint32_t>(item_height));
  const std::int32_t chosen = state != nullptr ? state->value : 0;
  std::int32_t x = rect.x;
  for (const std::int32_t frame : brush_frames(widget, columns)) {
    const std::int32_t row = frame == chosen && rows > 1 ? 1 : 0;
    const Rect source{(frame - 1) * item_width, row * item_height, item_width, item_height};
    canvas.blit_region(*image, source, x, rect.y, clip);
    x += item_width;
  }
}

/// The spin buttons beside a number edit: `Buttons` whole, or the pressed
/// half's own picture while the pointer holds it down.
void draw_spin(Canvas& canvas, ResourceCache& cache, const Widget& widget, const Rect& rect,
               const Rect& clip, const WidgetState* state, bool pressed) {
  const Image* image = nullptr;
  if (pressed && state != nullptr && state->value > 0) image = cache.image(widget.attribute_image("UpButtonPressed"));
  if (pressed && state != nullptr && state->value < 0) image = cache.image(widget.attribute_image("DownButtonPressed"));
  if (image == nullptr) image = cache.image(widget.attribute_image("Buttons"));
  if (image == nullptr) return;
  canvas.blit(*image, rect.x, rect.y, clip);
}

void paint_dialog(Canvas& canvas, const Layout& layout, ResourceCache& cache,
                  const DialogContent& content) {
  const Rect screen{0, 0, static_cast<std::int32_t>(canvas.width()),
                    static_cast<std::int32_t>(canvas.height())};
  if (content.backdrop != nullptr) canvas.blit(*content.backdrop, 0, 0, screen);
  // Two passes: an open combobox's list drops over whatever is declared
  // after it, so it is drawn after everything.
  for (int pass = 0; pass < 2; ++pass)
  for (const LaidOutWidget& placed : layout.widgets) {
    const Widget& widget = *placed.widget;
    const WidgetState* state = content.state_of(widget.name);
    const bool dropped = (widget.kind == WidgetType::kCombobox || widget.kind == WidgetType::kPlayerCombobox) &&
                         state != nullptr && state->open;
    if (dropped != (pass == 1)) continue;
    const bool hidden = state != nullptr && state->has_hidden ? state->hidden : has(widget.style, Style::kHidden);
    if (hidden) continue;
    Rect rect = placed.rect;
    // The bullets a dialog's template gives no rectangle stand beside its
    // caption; see `bullet_rect`.
    if (rect.width == 0 && rect.height == 0 && widget.has_id &&
        (widget.id == kIdLeftBullet || widget.id == kIdRightBullet)) {
      rect = bullet_rect(cache, layout, content, widget);
      if (rect.width == 0) continue;
    }
    // A `VXMenuBack` draws outside its rectangle by its offsets, and a
    // combobox's list drops below the closed box; both clip to the screen.
    const bool unclipped = widget.kind == WidgetType::kVXMenuBack ||
                           widget.kind == WidgetType::kCombobox ||
                           widget.kind == WidgetType::kPlayerCombobox;
    const Rect clip = unclipped ? screen : intersect(rect, screen);
    if (clip.width <= 0 || clip.height <= 0) continue;

    const ControlState control = dialog_state_of(widget, content, state);
    const Font* font = nullptr;
    if (const std::string_view path = widget.attribute("Font"); !path.empty()) font = cache.font(path);
    // A text the file declares is translated by where it stands; one put on
    // the widget from outside is already what it should say.
    std::string_view text = widget.text;
    const bool declared = state == nullptr || !state->has_text;
    if (!declared) text = state->text;
    const auto shown = [&](std::string_view key) -> std::string_view {
      return declared ? content.text_of(key, widget.name, "Text") : key;
    };

    switch (widget.kind) {
      case WidgetType::kControl:
        if (acts_as_list(widget, state)) draw_list(canvas, cache, widget, rect, clip, state, content);
        break;
      case WidgetType::kDialog:
      case WidgetType::kBmpScroll:
        break;
      case WidgetType::kSpin:
        draw_spin(canvas, cache, widget, rect, clip, state,
                  !content.pressed.empty() && iequal(content.pressed, widget.name));
        break;
      case WidgetType::kBrushSize:
        draw_brush_size(canvas, cache, widget, rect, clip, state);
        break;
      case WidgetType::kConquestMap:
        // What the code composed for it: the campaign map's viewport.
        if (state != nullptr && state->bitmap != nullptr) {
          canvas.blit(*state->bitmap, rect.x, rect.y, clip);
        }
        break;
      case WidgetType::kDarkFrame:
        // Half of what is under it: 0x00663c70 shifts every pixel right by
        // one and masks the carry. Black at half cover is the same thing
        // once the layer is blended over the world.
        canvas.fill_rect(clip, Color{0, 0, 0, 128});
        break;
      case WidgetType::kBackground:
        draw_background(canvas, widget, rect, clip, state);
        break;
      case WidgetType::kFrame: {
        if (const Image* image = cache.image(widget.image)) draw_frame(canvas, *image, widget, rect, clip);
        break;
      }
      case WidgetType::kVXMenuBack:
        draw_menu_back(canvas, cache, widget, rect, clip, state, content);
        break;
      case WidgetType::kTextW:
      case WidgetType::kTextEx: {
        if (font == nullptr) break;
        const Color ink = state != nullptr && state->has_ink
                              ? state->ink
                              : parse_color(widget.attribute("TextColor"), content.default_ink);
        std::int32_t wrap = 0;
        (void)widget.attribute_int("Width", wrap);
        // A text that names a scrollbar (`VScrollId`) is a log that scrolls:
        // set from the top and wrapped, whatever its template's style.
        // **Reading, labelled:** `MPCHAT.INI`'s `ChatLog` is a `StaticText`
        // with a `Width`, a `BufSize` and a scrollbar, and a log shown on one
        // centred line is no log.
        Style style = widget.style;
        if (!widget.attribute("VScrollId").empty()) style |= Style::kMultiline;
        if (state != nullptr && state->text_scroll != 0) {
          // Rolled: the block starts `text_scroll` above the rectangle, and
          // what stands outside the rectangle is clipped away.
          Rect rolled = rect;
          rolled.y -= state->text_scroll;
          rolled.height += state->text_scroll;
          draw_text_block(canvas, *font, shown(text), rolled, style, ink, intersect(rect, clip), wrap);
          break;
        }
        draw_text_block(canvas, *font, shown(text), rect, style, ink, clip, wrap);
        break;
      }
      case WidgetType::kEditW: {
        if (font == nullptr) break;
        const Color ink = parse_color(widget.attribute("TextColor"), content.default_ink);
        std::string shown(text);
        if (has(widget.style, Style::kSecure)) shown.assign(shown.size(), '*');
        // An edit's text is never translated: it is what was typed.
        std::int32_t wrap = 0;
        (void)widget.attribute_int("Width", wrap);
        const Rect inset{rect.x + 2, rect.y, rect.width - 4, rect.height};
        draw_text_block(canvas, *font, shown, inset, widget.style, ink, clip, wrap);
        const bool focused = !content.focused.empty() && iequal(content.focused, widget.name);
        if (focused && content.caret_on && !has(widget.style, Style::kMultiline)) {
          const std::size_t at = state != nullptr && state->caret >= 0
                                     ? std::min(shown.size(), static_cast<std::size_t>(state->caret))
                                     : shown.size();
          const std::int32_t pen = font->measure(std::string_view{shown}.substr(0, at));
          const std::int32_t y = rect.y + (rect.height - font->height()) / 2;
          canvas.fill_rect(intersect(Rect{inset.x + pen, y, 1, font->height()}, clip), ink);
        }
        break;
      }
      case WidgetType::kList:
        draw_list(canvas, cache, widget, rect, clip, state, content);
        break;
      case WidgetType::kScroll:
        draw_scroll(canvas, cache, widget, rect, clip, layout, content, control);
        break;
      case WidgetType::kCombobox:
      case WidgetType::kPlayerCombobox:
        draw_combobox(canvas, cache, widget, rect, clip, state, content);
        break;
      case WidgetType::kImageButton:
      case WidgetType::kActiveButton:
      case WidgetType::kButton:
      case WidgetType::kIcon:
      case WidgetType::kSwitch:
      default: {
        // The bitmap's cell for the state, centred in the rectangle. An
        // `ImgButton200` is a 237 x 38 cell in a 250 x 40 rectangle, and the
        // rectangles are themselves centred in their dialog; **reading,
        // labelled**, the cell is centred too. The label is centred in
        // `FontColor`, or `DisabledFontColor` when the button is.
        if (state != nullptr && state->bitmap != nullptr) {
          // A picture the code composed, at the widget's top-left: the
          // editor's *Place Label* screen hangs the minimap on an
          // `INACTIVE` button (`MapPlace.ini`'s `BMP`), as the campaign
          // map's `ConquestMap` carries its viewport.
          canvas.blit(*state->bitmap, rect.x, rect.y, clip);
          break;
        }
        if (state != nullptr && !state->image.empty()) {
          // A picture put on the widget from outside, fitted to it.
          if (const Image* picture = icon_image(cache, state->image)) {
            const std::int64_t pw = picture->width;
            const std::int64_t ph = picture->height;
            std::int32_t width = rect.width;
            std::int32_t height = static_cast<std::int32_t>(ph * rect.width / std::max<std::int64_t>(1, pw));
            if (height > rect.height) {
              height = rect.height;
              width = static_cast<std::int32_t>(pw * rect.height / std::max<std::int64_t>(1, ph));
            }
            const Rect fitted{rect.x + (rect.width - width) / 2, rect.y + (rect.height - height) / 2, width, height};
            canvas.blit_scaled(*picture, fitted, clip);
          }
          break;
        }
        const Image* image = dialog_image(widget, cache, control, state != nullptr ? state->row : -1);
        if (image != nullptr) {
          const std::int32_t x = rect.x + (rect.width - static_cast<std::int32_t>(image->width)) / 2;
          const std::int32_t y = rect.y + (rect.height - static_cast<std::int32_t>(image->height)) / 2;
          canvas.blit(*image, x, y, clip);
        }
        if (font != nullptr && !text.empty()) {
          Color ink = parse_color(widget.attribute("FontColor"),
                                  parse_color(widget.attribute("TextColor"), content.default_ink));
          if (control == ControlState::kDisabled) {
            ink = parse_color(widget.attribute("DisabledFontColor"), ink);
          }
          Style style = widget.style;
          if (!has(style, Style::kAlignLeft) && !has(style, Style::kAlignRight)) style |= Style::kAlignCenter;
          draw_text_block(canvas, *font, shown(text), rect, style, ink, clip, 0);
        }
        break;
      }
    }
  }
}

// -- the command buttons -----------------------------------------------------

Rect command_button_cell(ResourceCache& cache, const BarContent& content) {
  const Image* frame = cache.image(content.button_frame);
  if (frame == nullptr || frame->empty()) return Rect{0, 0, 51, 52};
  return Rect{0, 0, static_cast<std::int32_t>(frame->width / 3),
              static_cast<std::int32_t>(frame->height)};
}

Rect command_button_rect(const Rect& bar, const Rect& cell, std::size_t index,
                         std::size_t count) {
  const std::int32_t row_width = static_cast<std::int32_t>(count) * cell.width;
  const std::int32_t left = bar.x + (bar.width - row_width) / 2;
  const std::int32_t top = bar.y + (bar.height - cell.height) / 2;
  return Rect{left + static_cast<std::int32_t>(index) * cell.width, top, cell.width,
              cell.height};
}

void paint_command_buttons(Canvas& canvas, ResourceCache& cache, const BarContent& content,
                           const Rect& bar) {
  if (content.buttons.empty()) return;
  const Rect cell = command_button_cell(cache, content);
  const Rect clip = intersect(bar, Rect{0, 0, static_cast<std::int32_t>(canvas.width()),
                                        static_cast<std::int32_t>(canvas.height())});
  for (std::size_t i = 0; i < content.buttons.size(); ++i) {
    const CommandButtonView& button = content.buttons[i];
    const Rect at = command_button_rect(bar, cell, i, content.buttons.size());
    std::uint32_t state = 0;
    if (!button.enabled) {
      state = 2;
    } else if (!content.pressed.empty() && iequal(content.pressed, button.name)) {
      state = 2;
    } else if (!content.hovered.empty() && iequal(content.hovered, button.name)) {
      state = 1;
    }
    if (!button.icon.empty()) {
      ImageRef icon;
      icon.path = resolve_alias(button.icon);
      // The icons carry no probe of their own; green at the corner is the
      // key, as for every other icon the bars name by path.
      if (const Image* whole = cache.image(icon); whole != nullptr && !whole->empty()) {
        const std::uint8_t* corner = whole->pixel(0, 0);
        if (corner[0] == 0 && corner[1] == 255 && corner[2] == 0) {
          icon.has_key = true;
          icon.key_x = 0;
          icon.key_y = 0;
        }
      }
      if (const Image* image = cache.image_frame(icon, state, 3, 0, 1)) {
        canvas.blit(*image, at.x, at.y, clip);
      }
    }
    if (const Image* frame = cache.image_frame(content.button_frame, state, 3, 0, 1)) {
      canvas.blit(*frame, at.x, at.y, clip);
    }
  }
}

std::int32_t command_button_at(ResourceCache& cache, const BarContent& content, const Rect& bar,
                               std::int32_t x, std::int32_t y) {
  if (content.buttons.empty()) return -1;
  const Rect cell = command_button_cell(cache, content);
  for (std::size_t i = 0; i < content.buttons.size(); ++i) {
    const Rect at = command_button_rect(bar, cell, i, content.buttons.size());
    if (x >= at.x && x < at.right() && y >= at.y && y < at.bottom()) {
      return static_cast<std::int32_t>(i);
    }
  }
  return -1;
}

}  // namespace imperivm::core::ui
