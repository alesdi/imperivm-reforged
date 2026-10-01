#pragma once

// The zoom map's ground, as gbr.exe composes it.
//
// Specification: docs/engine/rendering.md, "The zoom map's ground".
//
// Read off the original's builder (0x00616f10, which calls 0x006186a0 on a
// surface twice the size of the picture and then halves it). It is the
// ground compositor again, at a small scale and from its own art:
//
// - **The art is `Minimap.pak`'s**, not the ground textures shrunk: each
//   `<layer>` of `DATA\TERRAINS.XML` names a `minimap` tile, loaded as
//   `minimap/zoom%d/terrain/<minimap>` (0x006183b0, the format string at
//   0x007d6d60, the attribute stored at 0x00622913), and the thirty masks
//   `minimap/zoom%d/terrain/transitions/c????.bmp` and `d????.bmp`.
// - **The tile over a cell is a dual-grid tile** (0x006180c0): the four
//   vertices at its corners, the far ones clamped at the map edge; the base
//   is the lowest layer present at any corner, drawn whole (0x00617990), and
//   every higher layer present follows in ascending order through the mask
//   named by the corners it does *not* hold, C on even cell rows and D on
//   odd (0x00617a50), blended as `(over * a + under * (256 - a)) >> 8`.
//   Unlike the ground's compositor, the waters are not lifted above the land
//   layers here: the order is the layer number and nothing else.
// - **Then the light** (0x00617bd0, the zoom map's warp): the baked level
//   bilinear across 32-unit quads, the gain `terrain_light_channel`'s.
// - **Then halved** (0x00617820): each picture pixel is the mean of a 2 x 2
//   block, each channel summed and shifted down by two.
//
// So a road is drawn wherever a road vertex is, through the same soft ramps
// the ground draws it with -- not only in a cell whose four corners are all
// road, which a road one vertex wide never is.

#include <array>
#include <cstdint>
#include <span>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/ui/image.hpp"
#include "imperivm/core/world/map.hpp"

namespace imperivm::core {

/// The zoom map's tile over cell `(cx, cy)` (0x006180c0): the base is the
/// lowest layer at any of the four corners, and every other layer present
/// follows ascending. No exception for the waters, which `terrain_tile`
/// makes for the ground.
[[nodiscard]] TerrainTile zoom_terrain_tile(const Grid& terrain, std::int32_t cx, std::int32_t cy);

/// What the zoom map is drawn from.
struct ZoomGroundArt {
  /// A layer's minimap tile, indexed by its `z`, as RGBA; a null, empty or
  /// missing entry is a layer with no art, which the original leaves
  /// undrawn (its base draw and its overlay draw both return early).
  std::span<const ui::Image* const> tiles;
  /// The masks, indexed `row * 16 + code`: `row` 0 is C (even cell rows)
  /// and 1 is D, `code` the four corners the overlay does not hold. Only
  /// the red channel is read: the masks are greyscale.
  std::array<const ui::Image*, 32> masks{};
};

/// A tally of what `compose_zoom_ground` drew, for a caller to report.
struct ZoomGroundStats {
  /// Pixels of the picture at least half of whose composed area is a layer
  /// `counted` says to count -- the roads, when the caller asks for them.
  std::uint32_t counted_pixels = 0;
};

/// The whole map's ground at `1 / divisor` of the screen scale into `out`
/// (RGBA8): `cells * (64 / divisor)` pixels wide and
/// `world_to_screen_y(cells * 64) / divisor` tall, the size the zoom map
/// places its objects and its frame on. Composed at twice that from `art`,
/// which must be the art for `divisor` (`MINIMAP\ZOOM<divisor>`, whose
/// masks are a cell wide at twice the scale), then lit by `light` and
/// halved. `counted`, when given, is a table indexed by layer `z` of the
/// layers to tally in the stats.
///
/// **Readings, labelled.** The rows a cell covers are the ones the
/// engine's own projection (46/64) puts in it, not the original's 181/256,
/// so the picture registers with the objects and the camera frame drawn on
/// it. The original's row loop draws terrain row `k` over the span of row
/// `k - 1` from the third row on and never draws the second (0x006182b0:
/// the end of a row is computed from the row before's top): read as an
/// off-by-one and not reproduced. The warp also lifts the picture by the
/// height layer; this picture stays flat, as the objects on it do.
ZoomGroundStats compose_zoom_ground(const Grid& terrain, const Grid& light,
                                    std::uint32_t divisor, const ZoomGroundArt& art,
                                    ui::Image& out,
                                    std::span<const bool> counted = {});

}  // namespace imperivm::core
