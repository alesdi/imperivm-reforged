#include "imperivm/core/ui/layout.hpp"

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

/// `value * numerator / denominator`, rounding towards negative infinity so
/// that a negative slack — a screen narrower than its design — walks widgets in
/// step instead of splaying them by a pixel each.
std::int32_t share(std::int32_t value, std::int32_t numerator, std::int32_t denominator) noexcept {
  if (denominator == 0) return 0;
  const std::int64_t product = static_cast<std::int64_t>(value) * numerator;
  std::int64_t quotient = product / denominator;
  if ((product % denominator != 0) && ((product < 0) != (denominator < 0))) --quotient;
  return static_cast<std::int32_t>(quotient);
}

}  // namespace

const LaidOutWidget* Layout::find(std::string_view name) const noexcept {
  for (const auto& entry : widgets) {
    if (entry.widget != nullptr && iequal(entry.widget->name, name)) return &entry;
  }
  return nullptr;
}

void place_axis(std::int32_t design_origin, std::int32_t design_extent,
                std::int32_t screen_design_extent, std::int32_t screen_extent,
                const Align& align, std::int32_t& origin, std::int32_t& extent) noexcept {
  const std::int32_t total = align.total();
  const std::int32_t slack = screen_extent - screen_design_extent;
  if (total <= 0 || slack == 0) {
    origin = design_origin;
    extent = design_extent;
    return;
  }
  // Both edges are computed from the screen origin rather than one from the
  // other, so widgets that share an edge keep sharing it after rounding.
  const std::int32_t before = share(slack, align.before, total);
  const std::int32_t through = share(slack, align.before + align.self, total);
  origin = design_origin + before;
  extent = design_extent + (through - before);
}

Layout layout_screen(const Screen& screen, std::int32_t width, std::int32_t height) {
  Layout out;
  out.rect = Rect{0, 0, width, height};

  std::int32_t design_width = screen.design.width;
  std::int32_t design_height = screen.design.height;
  if (design_width <= 0) design_width = width;
  if (design_height <= 0) design_height = height;
  // **`MinSize` is not a floor on the design.** The slack a spring divides
  // is the actual extent against the *authored* `RectWH`, and `UpperDlg`
  // authors `0, 0, 1024, 80` under a `MinSize` of `320, 200` it inherits
  // from `StdDlg`. Flooring the design at 200 gave an 80-tall bar a slack of
  // -120, and every strip on it -- `VAlign = 0, 1, 0`, the stretch-to-fill
  // spring -- came out 45 pixels *tall in the negative* and was never drawn.
  // Nothing noticed while the strips were empty. `MinSize` is what a
  // resizable dialog will not shrink below, and the bars are not resized.

  out.widgets.reserve(screen.widgets.size());
  for (const Widget& widget : screen.widgets) {
    LaidOutWidget placed;
    placed.widget = &widget;
    place_axis(widget.design.x, widget.design.width, design_width, width, widget.halign,
               placed.rect.x, placed.rect.width);
    place_axis(widget.design.y, widget.design.height, design_height, height, widget.valign,
               placed.rect.y, placed.rect.height);
    out.widgets.push_back(placed);
  }
  return out;
}

}  // namespace imperivm::core::ui
