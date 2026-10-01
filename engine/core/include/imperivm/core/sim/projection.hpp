#pragma once

/// The isometric projection, and the inverse the simulation needs.
///
/// `docs/engine/projection.md` describes the forward mapping the renderer uses.
/// This header is the *simulation's* half of it, and it exists for one reason:
/// a building's entrance and exit markers are stored as **screen-pixel offsets**
/// on its entity, and turning one into the world point a unit can stand on
/// needs the inverse.
///
/// ## The forward rule, in the form the inverse has to undo
///
/// `gbr.exe` 0x005c11a0 forms a screen point from a world point as
///
///     screen.x = world.x
///     screen.y = (world.y * 181) / 256 - terrain_height(world.x, world.y)
///
/// -- the horizontal scale is exactly 1, so only the vertical axis has anything
/// to invert, and the height term is what makes it interesting: the same screen
/// row is produced by a low point on high ground and a farther point on low
/// ground.
///
/// ## The inverse is a scan, not algebra, and that is the original's choice
///
/// 0x006243b0 does **not** solve for `world.y`. It brackets:
///
///   1. The first candidate is `(screen.y * 362) / 256`, floored to a multiple
///      of 32 -- 362/256 is the reciprocal of 181/256 and 32 is the height
///      layer's cell size. Terrain height is never negative and is *subtracted*
///      on the way out, so this candidate is always at or below the answer and
///      the scan only ever marches one way.
///   2. While the candidate's forward projection is at or below the target,
///      remember it and advance by 32.
///   3. The first candidate that overshoots closes the bracket. One truncating
///      linear interpolation inside that 32-unit window gives the answer.
///
/// No tolerance, no iteration cap, no floating point, no randomness. The step
/// count is bounded by how far the terrain rises across the bracket: about
/// `height * 256 / 181 / 32` steps, so at most eleven for the full 0..255 range
/// and zero or one on flat ground.
///
/// ## It is accurate to one screen unit, and never exact
///
/// The bracket is 32 **world** units wide and the function inside it is a
/// truncating scale, so closing with a *linear* interpolation lands one off on
/// about a third of all screen rows -- on dead flat ground, where there is no
/// height term at all. That is not slack in this transcription; it is what
/// 0x006243b0 computes. Solving the algebra would be strictly more accurate
/// than the original, and would answer differently on the folds above, so it
/// would be the wrong function.
///
/// **Reimplementing the scan reproduces a property solving the algebra would
/// not.** The projection is monotone in `world.y` only while the terrain rises
/// more slowly than about 0.707 units per unit; on a steeper slope the mapping
/// folds and several world rows share a screen row. The scan answers with the
/// *nearest* fold, which is the front-most surface -- exactly what a picking
/// routine wants, and what the original therefore returns.
///
/// **One guard is this engine's and is not the original's.** When the very
/// first candidate already overshoots, the two ends of the bracket can coincide
/// -- a terrain step that exactly cancels the scale across one 32-unit
/// increment -- and 0x006243b0 divides by their difference with no check. That
/// is a fault rather than a behaviour; here the interpolation is skipped and
/// the low end is returned.

#include <cstdint>

#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

/// The height layer's cell size, which is also the inverse scan's step.
inline constexpr std::int32_t kHeightCellSize = 32;

/// `world.y * 181 / 256`, the vertical scale, before the height term.
[[nodiscard]] constexpr std::int32_t project_scale(std::int32_t world_y) noexcept {
  return static_cast<std::int32_t>((static_cast<std::int64_t>(world_y) * 181) >> 8);
}

/// The screen point a world point projects to. `x` passes through.
[[nodiscard]] Point world_to_screen(const World& world, Point at) noexcept;

/// The world point a screen point picks out -- the scan described above.
///
/// `screen.x` is `world.x` unchanged, so only the vertical axis is searched.
/// The result is **not** clamped to the map: the caller decides whether a point
/// off the edge is a miss, which is what 0x005c11a0 does with its own bounds
/// check.
[[nodiscard]] Point screen_to_world(const World& world, Point screen) noexcept;

/// The same scan over any height function: `height_at(Point) -> int32_t`.
/// This is the body; the `World` overload hands it `terrain_height`, and the
/// map editor hands it the layer it is painting, sampled the way 0x0053eaa0
/// samples -- zero outside the map -- because the editor has no `World`.
template <typename HeightAt>
[[nodiscard]] Point screen_to_world_over(Point screen, HeightAt&& height_at) noexcept {
  const auto forward = [&](std::int32_t candidate) {
    return project_scale(candidate) - height_at(Point{screen.x, candidate});
  };

  // `(screen.y * 362) / 256`, floored to a multiple of the height cell. The
  // mask floors toward negative infinity, which is what the original's `and`
  // does and what keeps the candidate at or below the answer for a screen row
  // above the origin.
  //
  // **The 362 is an optimisation, not a behaviour.** Any starting candidate at
  // or below the answer finds the same bracket, because the scan marches
  // upward and stops at the *first* overshoot -- so halving it, or starting
  // from zero, would answer identically and only take more steps. It is
  // written as the original writes it; fault injection confirms that a wrong
  // reciprocal here changes nothing observable.
  std::int32_t candidate = static_cast<std::int32_t>(
      (static_cast<std::int64_t>(screen.y) * 362) >> 8);
  candidate &= ~(kHeightCellSize - 1);

  std::int32_t low = candidate;
  std::int32_t low_projection = 0;
  std::int32_t projection = forward(candidate);
  bool advanced = false;
  while (projection <= screen.y) {
    low = candidate;
    low_projection = projection;
    candidate += kHeightCellSize;
    projection = forward(candidate);
    advanced = true;
  }
  const std::int32_t high_projection = projection;
  if (!advanced) {
    // **Unreachable while the height layer is non-negative, and kept because
    // the original has it.** The first candidate is `floor32(screen.y * 362 /
    // 256)`, so `candidate <= screen.y * 256 / 181`; the forward projection is
    // `candidate * 181 / 256` minus a height that cannot be negative, so
    // `forward(candidate) <= screen.y` for either sign of `screen.y` and the
    // loop always runs at least once. Fault injection agrees: removing this
    // recomputation changes no test. It is transcribed rather than dropped
    // because a height layer that ever went below sea level would reach it, and
    // because the original's own division-by-zero fault lives inside it.
    low = candidate - kHeightCellSize;
    low_projection = forward(low);
  }

  // Likewise unreachable: `span` is zero only when the two ends of the bracket
  // project to the same row, which needs the branch above. 0x006243b0 divides
  // here with no check at all; this is a guard, not a transcription.
  const std::int32_t span = high_projection - low_projection;
  if (span == 0) return Point{screen.x, low};
  return Point{screen.x,
               low + static_cast<std::int32_t>(
                         (static_cast<std::int64_t>(kHeightCellSize) *
                          (screen.y - low_projection)) /
                         span)};
}

}  // namespace imperivm::core::sim
