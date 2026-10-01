#pragma once

/// The fog manager's light grid: what the local player's fog of war looks
/// like, as `gbr.exe` computes it. Presentation, never hashed, never saved.
///
/// `CVXFog` (constructed at 0x00606970, initialised at 0x00606670) keeps, for
/// the **local player only**, a grid of 16-unit cells over the fog rect, one
/// `uint16` a cell -- and it keeps two of them. The **displayed** grid
/// (`fog+0xc8`) is what the draw (0x00604df0) and the hide test (0x00605e10)
/// read; the **target** grid (`fog+0xcc`) is what a rebuild (0x00607190)
/// writes, and it is either copied into the displayed grid whole ("snap") or
/// turned into a per-tick delta so the displayed grid slides to it over four
/// ticks. The words the rebuild produces:
///
///   | word            | level | produced by |
///   |-----------------|-------|-------------|
///   | `0x3e00`        | 31    | inside a source's sight circle (0x006056d0) |
///   | `0x3c00`        | 30    | a 256-cell lit wholesale (0x00606de0); fog of war off |
///   | `0x2000..0x3e00`| 16..31| the 32-unit ramp at a sight circle's edge |
///   | `0x2000`        | 16    | the explored floor (the visitor, 0x00605cc0) |
///   | `0..0x3c00`     | 0..30 | the exploration edge's cap in a partially explored cell (0x00516ab0) |
///   | `0`             | 0     | a never-seen cell (0x005145a0) |
///
/// **Nothing writes `0x3a00`**; an earlier reading had it as "unexplored" and
/// it is the draw's threshold constant. Unexplored is word 0.
///
/// ## The rebuild, 0x00607190
///
/// The rect is grown by 2048 world units on each side and clipped to the
/// world. Every cell of it is set to the floor `0x2000`. Then every object on
/// the local player's side -- the owner's bit in the local record's side
/// mask, here *the local player, or one it shares its view with*
/// (`Relation::share_view`) -- that is spawned, alive and has a non-zero
/// `sight` lights a circle:
///
///   * **The coarse pass first** (0x00606de0). An object whose *class* sight
///     is above 362 marks its 256-unit cell; a marked cell whose eight
///     neighbours (those inside the rect) are all marked is *interior*: its
///     sixteen 16-cells are set to `0x3c00` and the objects standing in it
///     are dropped from the precise pass. 362 is `256 * sqrt 2` rounded down,
///     the farthest a cell's corner can be from a point inside it -- an
///     inference from the constant, and so is "class sight" for the field
///     the test reads (`[class+0x2d0]`).
///   * **Then the circle** (0x006056d0), at the object's own `sight` `s`,
///     evaluated at each 16-cell's **top-left corner**: `0x3e00` inside
///     `s - 32`, a ramp linear in the *squared* distance from `0x3e00` down
///     to `0x2000` across the outer 32 units, nothing beyond; `max`ed with
///     what is there, so overlapping sources never darken each other, and a
///     cell already at `0x3e00` is skipped. The original does the ramp in
///     floats; the integer quotient here can differ by one word unit.
///
/// With fog of war off the rect is filled with `0x3c00` and the objects are
/// skipped. Then, exploration on, the exploration map is read (0x00516f80,
/// `sim/fog.hpp`): a never-seen 1024-cell's 16-cells are set to **0**; a
/// partially explored one has its fine record's nibbles interpolated
/// bilinearly at 32-unit spacing -- the neighbouring 1024-cells providing the
/// far samples -- and the grid capped at `nibble * 1024` (0x00516ab0); a fully
/// explored cell is left alone. With both off the rebuild returns without
/// touching the grid.
///
/// ## The fade, 0x00607c90
///
/// A timer whose period is `min(100, 100000 / speed)` ms. Every fourth tick
/// the whole rect is rebuilt and the target turned into a delta in place,
/// `trunc((target - displayed) / 4)`; every tick the displayed grid has its
/// delta added. So a cell entering sight ramps up over four ticks, one leaving
/// it ramps down to the floor or the exploration cap, and one newly explored
/// lifts from black -- all symmetric, all linear. A snap (grid creation, and
/// the strips a scroll exposes) copies the target across and zeroes the delta,
/// so scrolling never fades in ground already explored.
///
/// ## The draw, 0x00604a40, and the darkening table, 0x00604610
///
/// A word above `0x3a00` draws nothing (and so does `0x3c00..0x3dff`, by the
/// `(w >> 9) + 2 == 32` test before it); a word below `0x400` is solid black;
/// anything else darkens the pixel through two tables indexed by
/// `L = word >> 10`, so the floor `0x2000` is `L = 8`. The tables scale each
/// 5-bit channel by `f / 32` with `f = floor(68 * L / 31)`: 0, 2, 4, 6, 8, 10,
/// 13, 15, **17**, 19, 21, 24, 26, 28, 30 for `L = 0..14` -- the floor is
/// 17/32, and the earlier "one half" was a guess that happened to be close. An
/// ordered dither of at most 480 is added to the word before the shift; it is
/// not reproduced, and neither is the per-pixel interpolation of 0x00604bb0,
/// which the renderer's linear sampling of the overlay stands in for.
///
/// ## The hide test, 0x00605e10, and who it is applied to, 0x00605ff0
///
/// Only **units**, **projectiles** (`CVXArrowShot`) and the "Watersteps"
/// class are ever tested; everything else -- **buildings included** -- is
/// drawn wherever it stands, darkened by the overlay, covered by the black.
/// There is no last-seen memory: a building destroyed out of sight vanishes
/// at once. For a unit: the owner shares its view with the local player
/// (`[owner + 0x24 + local * 4] & 0x10`, and the diagonal word has the bit)
/// → visible; else the hidden bit → hidden; else an `Animal` on explored
/// ground → visible; else the light test, which is also the projectile's:
/// hidden iff the displayed grid, sampled at the object, is below **0x3000**
/// (level 24) -- so a unit at the outer part of a sight edge is already
/// hidden, and one in a cell ramping up appears when the ramp passes it.
///
/// ## What is still unknown
///
///   * Whether a held object is in the spatial hash the collector walks; it
///     is skipped here, which is what its held position would do anyway.
///   * That `[class+0x2d0]` is the class's sight, inferred from 362 alone.
///   * The default and units of the speed global `[globals+0x1244]`; only the
///     period formula is established, and 100 ms is used.
///   * The sampler's off-corner arithmetic (0x006049a0); a plain bilinear
///     blend of the four corner words is used.
///   * What 0x006074e0, run after each fourth-tick rebuild, computes.
///   * The dither matrix, and the "Watersteps" disagreement between the two
///     functions -- 0x00605e10's exemption is the one taken.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;
struct WorldObject;

class FogLight {
 public:
  /// World units per cell.
  static constexpr std::int32_t kCell = 16;
  /// The explored floor, level 16.
  static constexpr std::uint16_t kFloor = 0x2000;
  /// A 256-cell lit wholesale, and the fill with fog of war off. Level 30.
  static constexpr std::uint16_t kLitCoarse = 0x3c00;
  /// Inside a sight circle. Level 31.
  static constexpr std::uint16_t kLitFull = 0x3e00;
  /// The draw: a word above this draws nothing.
  static constexpr std::uint16_t kLitAbove = 0x3a00;
  /// The draw: a word below this is solid black.
  static constexpr std::uint16_t kBlackBelow = 0x400;
  /// The hide test: a unit is hidden below this. Level 24.
  static constexpr std::uint16_t kHideBelow = 0x3000;
  /// The sight circle's edge ramp, world units.
  static constexpr std::int32_t kSightRamp = 32;
  /// How far the rebuild grows its rect on each side.
  static constexpr std::int32_t kGrow = 2048;
  /// The coarse pass: its cell, and the class sight above which an object
  /// marks its cell.
  static constexpr std::int32_t kCoarseCell = 256;
  static constexpr std::int32_t kCoarseSight = 362;
  /// Ticks between rebuilds, and the fade's divisor.
  static constexpr std::int32_t kTicksPerRebuild = 4;

  /// A rect in world units, both ends inclusive.
  struct Rect {
    std::int32_t x0 = 0;
    std::int32_t y0 = 0;
    std::int32_t x1 = -1;
    std::int32_t y1 = -1;
    [[nodiscard]] constexpr bool empty() const noexcept { return x1 < x0 || y1 < y0; }
    friend constexpr bool operator==(const Rect&, const Rect&) = default;
  };

  /// The two switches the rebuild reads: `fog+0xd4` and `fog+0xd8`.
  struct Setup {
    bool fog_of_war = true;
    bool exploration = true;
  };

  /// Size both grids for a square map `map_size` units on a side, zeroed --
  /// which is black until something is rebuilt. Zero empties them.
  void resize(std::int32_t map_size);
  [[nodiscard]] bool empty() const noexcept { return columns_ == 0; }
  [[nodiscard]] std::int32_t columns() const noexcept { return columns_; }
  [[nodiscard]] std::int32_t rows() const noexcept { return rows_; }

  /// The rebuild over `rect`, grown by `kGrow` and clipped to the world. With
  /// `snap` the displayed grid takes the target at once; otherwise the delta
  /// is set for the next `kTicksPerRebuild` ticks.
  void rebuild(const World& world, const ExplorationMap* map, PlayerId local, Setup setup,
               Rect rect, bool snap);

  /// The strips of `rect` (grown) that the last rebuild or show did not
  /// cover, snapped; a scroll never fades in explored ground. The first call
  /// after `resize` snaps the whole rect.
  void show(const World& world, const ExplorationMap* map, PlayerId local, Setup setup,
            Rect rect);

  /// One timer tick: every fourth, a rebuild of `rect` into the delta; every
  /// tick, the delta added. Returns whether this tick rebuilt.
  bool tick(const World& world, const ExplorationMap* map, PlayerId local, Setup setup,
            Rect rect);

  /// The timer's period: `min(100, 100000 / speed)` milliseconds.
  [[nodiscard]] static std::int32_t period_ms(std::int32_t speed) noexcept;

  /// The displayed word of the cell `at` falls in; 0 off the grid.
  [[nodiscard]] std::uint16_t word_at(Point at) const noexcept;
  /// The target word of the cell `at` falls in; 0 off the grid.
  [[nodiscard]] std::uint16_t target_at(Point at) const noexcept;
  /// The displayed grid sampled at `at`: the corner word on a corner, the
  /// bilinear blend of the four corners between them. 0 off the grid.
  [[nodiscard]] std::uint16_t sample(Point at) const noexcept;

  /// The darkening factor's numerator over 32 for a word: 32 when the draw
  /// leaves the pixel alone, 0 when it paints it black, `floor(68 L / 31)`
  /// for `L = word >> 10` between.
  [[nodiscard]] static std::int32_t factor_of(std::uint16_t word) noexcept;

  /// The hide test over the displayed grid, for `object` as `local` sees it.
  [[nodiscard]] bool hides(const World& world, const WorldObject& object, PlayerId local,
                           const ExplorationMap* map, Setup setup) const noexcept;

  /// Row-major, `rows() * columns()` words each.
  [[nodiscard]] std::span<const std::uint16_t> displayed() const noexcept { return displayed_; }
  [[nodiscard]] std::span<const std::uint16_t> target() const noexcept { return target_; }

 private:
  /// The rect in cells, clipped to the grid, both ends inclusive.
  struct Cells {
    std::int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    [[nodiscard]] bool empty() const noexcept { return x1 < x0 || y1 < y0; }
  };
  [[nodiscard]] Cells clip(Rect rect) const noexcept;
  [[nodiscard]] Rect grown(Rect rect) const noexcept;
  /// The rebuild over a rect already grown and clipped.
  void rebuild_cells(const World& world, const ExplorationMap* map, PlayerId local, Setup setup,
                     Cells cells, bool snap);
  void light_objects(const World& world, PlayerId local, Cells cells);
  void cap_exploration(const ExplorationMap& map, std::int32_t slot, Cells cells);
  [[nodiscard]] std::size_t index(std::int32_t cx, std::int32_t cy) const noexcept {
    return static_cast<std::size_t>(cy) * static_cast<std::size_t>(columns_) +
           static_cast<std::size_t>(cx);
  }

  std::int32_t columns_ = 0;
  std::int32_t rows_ = 0;
  std::vector<std::uint16_t> displayed_;
  std::vector<std::uint16_t> target_;
  std::vector<std::int16_t> delta_;
  std::int32_t counter_ = 0;
  /// What `show` last covered, in cells; empty after `resize`.
  Cells shown_;
};

}  // namespace imperivm::core::sim
