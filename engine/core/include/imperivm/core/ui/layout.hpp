#pragma once

/// Turning a screen's design rectangles into the rectangles it draws at.
///
/// The engine's resize rule is a three-spring split, one per axis. `HAlign` and
/// `VAlign` carry the weights `(before, self, after)`; the slack between the
/// screen's design size and its actual size is divided in that ratio between
/// the space in front of the widget, the widget itself, and the space behind
/// it.
///
/// **The evidence that the weights are ratios and not flags** is
/// `COMMON/SCRIPTEDIT.INI`, which is not one of the files this part owns and so
/// is an independent test. Its 320-wide dialog puts a list at `x=7, w=125` with
/// `HAlign = 0, 2, 8` and the scroll bar that must stay glued to the list's
/// right edge at `x=132, w=15` with `HAlign = 2, 0, 8`. Under the ratio rule the
/// list's right edge moves by `0.2 * slack` and the bar's left edge moves by
/// `0.2 * slack`, so they stay in contact at every width. Under any reading
/// where 2 and 8 are flags they come apart. The same file's `[ScriptBack]`
/// (`2, 8, 0`, right edge `298`) and `[Script.VScrollBack]` (`1, 0, 0`, left
/// edge `298`) make the same prediction at the other end of the dialog, and
/// `MENU/*.INI` uses `0,1,4 / 1,1,3 / 2,1,2 / 3,1,1 / 4,1,0` — five weights
/// summing to five — to spread a row of controls.
///
/// **The arithmetic is integer**, as everything in the core is, and it rounds
/// the same way at both ends of a widget: an edge's offset is computed as
/// `slack * weight / total` from the screen origin rather than accumulated, so
/// two widgets that share an edge in the design share it at every size. Rounding
/// down is what makes 0.2 of an odd slack land on the same pixel for both.

#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/ui/interface.hpp"

namespace imperivm::core::ui {

/// One widget's place on screen.
struct LaidOutWidget {
  const Widget* widget = nullptr;
  Rect rect;
};

/// A whole screen placed at a size.
struct Layout {
  Rect rect;  ///< the screen itself, after `place_screen`
  std::vector<LaidOutWidget> widgets;

  [[nodiscard]] const LaidOutWidget* find(std::string_view name) const noexcept;
};

/// Apply one axis. `design` is the widget's `(origin, extent)` in the screen's
/// design space, `design_extent` the screen's design extent, `extent` its
/// actual one.
///
/// Returns the placed `(origin, extent)`. With `Align{0,0,1}` — the default —
/// the result is the input, which is why a widget with no alignment key does
/// not move.
void place_axis(std::int32_t design_origin, std::int32_t design_extent,
                std::int32_t screen_design_extent, std::int32_t screen_extent,
                const Align& align, std::int32_t& origin, std::int32_t& extent) noexcept;

/// Lay `screen` out at `width` x `height`.
///
/// The screen's own rectangle is placed at the origin with the given size, which
/// is what the bars want: `gbr.exe` reads `UIBars` out of `CONST.INI` and gives
/// the info bar and command bar the full width of the display. The design size
/// the springs measure against is the screen's authored `RectWH` — 1024 x 80 for
/// every info bar, 1024 x 54 for every command bar — clamped up to `MinSize`
/// where one is declared.
[[nodiscard]] Layout layout_screen(const Screen& screen, std::int32_t width,
                                   std::int32_t height);

}  // namespace imperivm::core::ui
