#pragma once

/// The map editor's brushes, as `gbr.exe`'s tool windows apply them to the
/// terrain layers -- the arithmetic and nothing else, so that a headless test
/// can paint a grid and read the cells back.
///
/// Read off the executable and reproduced here (addresses so a reader can
/// check): the brush disc (0x005445f0 builds the 21 x 21 table the tools
/// index; a cell is inside when the ceiling of its distance in cells is at
/// most the radius, which is `i^2 + j^2 <= r^2`), the terrain stroke
/// (0x004b3570), the passability rebuild it ends with (0x00547700, its own
/// section below: the terrain's rules, every mask stamped through the
/// projection, the frame -- proven against every shipped map layer), the
/// three height tools (set 0x0049d220, raise/lower 0x0049d3c0, smooth
/// 0x0049d680) and the light re-bake their commit runs (0x00544e10), the
/// decoration stamp (0x0048c370 exact, 0x0048db40 over a disc) and its
/// delete (0x0048c910). What each function does *not* reproduce is stated
/// on it.
///
/// **Sizes.** The brush picker's slots (`BrushSize`, `Frames = 23456` or
/// `13456`) are indices 0..4 to the tools, not the frame digits: the terrain
/// and decoration tools read radius 0, 1, 2, 3 and 5 cells of 64 units from
/// them (0x004b3020, 0x0048bc00), the height tools 1, 3, 5, 7 and 9 cells of
/// 32 (0x0049d1c0). The pictures' first two frames are both "one cell".
///
/// The random draws -- a group's layer, a decoration's kind and offsets, the
/// density test -- come from the caller's `Rng`. The original draws them from
/// a generator apart from the simulation's (`[0x996ff4] + 0x12b4`); where it
/// is seeded was not read, so the editor's is this engine's own.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/world/map.hpp"

namespace imperivm::core::edit {

/// A rectangle of cells, inclusive at both ends; empty when `x1 < x0`.
struct CellRect {
  std::int32_t x0 = 0;
  std::int32_t y0 = 0;
  std::int32_t x1 = -1;
  std::int32_t y1 = -1;

  [[nodiscard]] bool empty() const noexcept { return x1 < x0 || y1 < y0; }
  void add(std::int32_t x, std::int32_t y) noexcept;
  void merge(const CellRect& other) noexcept;
};

/// The radius, in the tool's own cells, of brush slot `slot` (0..4).
[[nodiscard]] std::int32_t brush_radius(std::int32_t slot, bool height_tool) noexcept;

/// The cell offsets a brush of radius `radius` covers: every `(i, j)` with
/// `i^2 + j^2 <= radius^2`, row by row. Radius 0 is the one cell.
[[nodiscard]] std::vector<sim::Point> brush_disc(std::int32_t radius);

// --------------------------------------------------------------------------
// terrain
// --------------------------------------------------------------------------

/// Whether every cell within four of `(x, y)` is water -- what deep water
/// needs to stay deep (0x005467e0; a cell off the grid counts as not water).
[[nodiscard]] bool deep_water_eligible(const Grid& terrain, std::int32_t x, std::int32_t y) noexcept;

/// The 64-unit cell a terrain stroke is centred on: the one holding the
/// grid corner nearest the pointer (0x004b3570 snaps `p` to
/// `floor((p + 32) / 64) * 64` and samples 32 further on).
[[nodiscard]] sim::Point terrain_brush_cell(sim::Point world) noexcept;

/// Whether a stroke has moved far enough from the last one to paint again:
/// the terrain tool repaints when the pointer is more than 32 world units
/// from where it last painted.
[[nodiscard]] bool terrain_stroke_moved(sim::Point last, sim::Point now) noexcept;

/// One terrain stroke at cell `(cx, cy)` with radius `radius`, writing one
/// of `layers` -- the leaf's own, or one drawn from the group's list per
/// cell (0x004b3570 draws `rand(0, count - 1)`). `water_group` is the
/// original's special case: painting the Water group writes deep water, 13
/// (0x004b316b), and deep water has its rules -- a first pass writes shallow
/// water, 12, into every cell of the disc not already deep, and a second
/// writes 13 only where every cell within four is water, or everywhere when
/// `shift` is held (0x005467e0). Painting anything else then demotes every
/// deep cell within four of a painted cell that no longer qualifies to 12.
/// Returns the cells changed, for the passability re-bake and the redraw.
[[nodiscard]] CellRect paint_terrain(Grid& terrain, std::int32_t cx, std::int32_t cy,
                                     std::int32_t radius, std::span<const std::int32_t> layers,
                                     bool water_group, bool shift, sim::Rng& rng);

// --------------------------------------------------------------------------
// passability
// --------------------------------------------------------------------------
//
// The original keeps the map's passability layer *incrementally*: nothing
// rebuilds it at save time (0x0054b260 writes the live grid), the brushes and
// the object tools call one rebuild over a rectangle after every change, and
// the shipped `Terrain.pass.grid` is what those calls left behind. This is
// that rebuild, in the three steps 0x00547700 takes and with the three
// bodies it calls, so that the layer an edited map is saved with is the one
// the original would have saved -- and so that the reading can be checked:
// `immap passability` rebuilds every shipped map's layer from nothing and
// diffs it against the stored one.
//
// **The mask is applied through the projection, and upside down.** A `.pass`
// file is a square grid of 16-unit cells (`docs/formats/pass.md`), but
// 0x00546a80 does not lay it on the ground as one. For each set cell it
// forms a *screen* point -- `x` the anchor's plus the cell's offset from the
// mask's centre column, `y` the anchor's projected row plus `(1032 - 16 *
// row) * 181 / 256` -- and asks the camera's inverse for the world point
// under it, height and all. Two things follow. The row offset is *negated*:
// row 64 lands on the anchor and row 63 one cell *below* it in world `y`,
// so a mask is stamped mirrored top to bottom against its file order,
// which is why every symmetric mask in the install measures as anchored at
// row 64.5 rather than 63.5 (`pass.md`, "Anchor" -- that discrepancy is this
// flip). And on a slope the cells spread out: the inverse (`sim/projection.
// hpp`) turns a screen row into a farther world row where the ground rises,
// and the stamp fills the column between consecutive cells' world rows so
// the footprint stays solid. `sim/path.hpp`'s `ObstructionGrid::stamp` lays
// the file on the ground unflipped and unprojected; it is the guess the
// spec labelled, this is the reading, and the simulation adopts the shipped
// layer verbatim so the two never met.

/// A rectangle in world units, inclusive at both ends -- the shape the
/// original's tools hand its rebuild (`{x0, y0, x1, y1}` at 0x00547700).
struct WorldRect {
  std::int32_t x0 = 0;
  std::int32_t y0 = 0;
  std::int32_t x1 = -1;
  std::int32_t y1 = -1;

  [[nodiscard]] bool empty() const noexcept { return x1 < x0 || y1 < y0; }
  [[nodiscard]] bool operator==(const WorldRect&) const noexcept = default;
  [[nodiscard]] bool contains(sim::Point p) const noexcept {
    return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
  }
  /// This rectangle cut to `bounds`.
  [[nodiscard]] WorldRect clamped(const WorldRect& bounds) const noexcept;
  /// This rectangle grown by `by` on every side.
  [[nodiscard]] WorldRect grown(std::int32_t by) const noexcept {
    return WorldRect{x0 - by, y0 - by, x1 + by, y1 + by};
  }
  /// The map's own rectangle: `0 .. extent - 1` on both axes, read off the
  /// terrain layer.
  [[nodiscard]] static WorldRect of_map(const Grid& terrain) noexcept;
};

/// The rectangle a terrain stroke at cell `(cx, cy)` with `radius` rebuilds
/// (0x004b3570): the stroke's sample point `+-(64 * radius + 127)`, which
/// is the disc's cells with a margin of two cells less one unit before and
/// two cells after.
[[nodiscard]] WorldRect terrain_stroke_rect(std::int32_t cx, std::int32_t cy,
                                            std::int32_t radius) noexcept;

/// The square a water *leaf* levels and clears (0x004b3dcd): the same
/// centre `+-(64 * radius + 63)`. Painting a leaf of type 4 -- shallow or
/// deep water, chosen as a leaf rather than through the Water group -- sets
/// the height under it to sea level (0x00549cc0), removes the decorations
/// standing in it (0x00547a90) and rebuilds the passability there, before
/// the stroke's own rebuild. The group does none of this.
[[nodiscard]] WorldRect water_leaf_rect(std::int32_t cx, std::int32_t cy,
                                        std::int32_t radius) noexcept;

/// Every 32-unit height cell whose corner lies in `rect` set to `level`,
/// as the water leaf sets sea level. Returns the height cells written, for
/// the light re-bake.
[[nodiscard]] CellRect level_height(Grid& heights, const WorldRect& rect,
                                    std::int32_t level);

/// Every 64-unit decoration cell whose corner lies in `rect` cleared
/// (0x00547a90's first half). Returns the cells that held one.
[[nodiscard]] CellRect clear_decor_in(Grid& decor, const WorldRect& rect);

/// The `.pass` masks an editor has read, by resource path, owning their
/// bytes: a `Grid` views the buffer it was parsed from, and the pack a mask
/// came out of is not obliged to keep it. `load` parses and keeps; a mask
/// that does not parse, or that is not a 16-unit one-bit grid, is refused
/// and the path remembered as having none, so it is not read twice.
/// A `.pass` mask with the bounding box of its set cells, in cells -- what
/// the original keeps beside the grid (`entity + 0x2a4`, computed once by
/// 0x005fdf70) so that a stamp walks the blob and not the 128 x 128 canvas.
/// A mask with no set cell has an empty box and stamps nothing.
struct PassMask {
  const Grid* grid = nullptr;
  CellRect bounds;

  [[nodiscard]] bool empty() const noexcept { return grid == nullptr || bounds.empty(); }
};

/// The bounding box of `mask`'s set cells, empty when there is none.
[[nodiscard]] CellRect mask_bounds(const Grid& mask);

/// The `.pass` masks an editor has read, by resource path, owning their
/// bytes: a `Grid` views the buffer it was parsed from, and the pack a mask
/// came out of is not obliged to keep it. `load` parses and keeps; a mask
/// that does not parse, or that is not a 16-unit one-bit grid, is refused
/// and the path remembered as having none, so it is not read twice.
class PassMaskLibrary {
 public:
  [[nodiscard]] const PassMask* find(std::string_view path) const noexcept;
  const PassMask* load(std::string_view path, std::span<const std::byte> bytes);
  /// Whether `path` has been asked for, with or without success.
  [[nodiscard]] bool known(std::string_view path) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return masks_.size(); }

  /// The mask `entity` names, read on the first ask through `read(path) ->
  /// span<const byte>` at each of `pass_path_candidates` in turn -- the
  /// bare name, then with `.PASS` -- and kept. Null for an entity that
  /// names none, or none that reads and parses.
  template <typename Read>
  const PassMask* resolve(const Entity* entity, Read&& read) {
    if (entity == nullptr || entity->pass_file().empty()) return nullptr;
    for (const std::string& candidate : pass_path_candidates(entity->path(), entity->pass_file())) {
      if (known(candidate)) {
        if (const PassMask* found = find(candidate)) return found;
        continue;
      }
      const std::span<const std::byte> bytes = read(candidate);
      if (bytes.empty()) continue;
      if (const PassMask* loaded = load(candidate, bytes)) return loaded;
    }
    return nullptr;
  }

 private:
  struct Entry {
    std::string path;
    std::vector<std::byte> bytes;
    Grid grid;  ///< empty when the bytes did not parse
    PassMask mask;
  };
  std::vector<std::unique_ptr<Entry>> masks_;
};

/// A footprint to stamp: an entity's `.pass` mask, and the world point its
/// owner stands at. A null mask stamps nothing.
struct Footprint {
  const PassMask* mask = nullptr;
  sim::Point at;
};

/// The mask's extent around `at`, flat -- what 0x005fe080 answers and the
/// rebuild after a removal covers: the set cells' bounding box, columns as
/// `16 * col - 1016` and rows as `1032 - 16 * row`, so the box is mirrored
/// the way the stamp is. Empty for a mask with no set cell.
[[nodiscard]] WorldRect footprint_rect(const PassMask& mask, sim::Point at);

/// One footprint stamped into `pass` (0x00546a80): every set cell of `mask`
/// projected from `at` and unprojected through `height`, as the comment at
/// the top of this section describes; only cells inside `limit` are
/// written, and the screen point is first clamped to the map's own screen
/// rectangle (`x` to `map.x1 - 32`, `y` to `project(map.y1) - 287`), which is
/// the original's guard against a stamp hanging off the edge. `height` is
/// read the way 0x0053eaa0 reads it -- zero outside the map -- and a map
/// with no height layer stamps as if flat.
void stamp_footprint(Grid& pass, const PassMask& mask, sim::Point at, const Grid& height,
                     const WorldRect& map, const WorldRect& limit);

/// The terrain's own passability over `rect` (0x00547090). For each 16-unit
/// cell whose corner `(x, y)` lies in the rectangle -- and stops 32 short of
/// the map's far edge, so the last two columns and rows are never written
/// -- the layer sampled at `(x + 32, y + 32)` decides: `passable="0"`
/// blocks; a `passable_water` layer blocks the rim of its 64-unit cell
/// wherever a neighbouring cell across that rim is not deep water
/// (0x00545100: the four sides on a side rim, the diagonal too at a corner);
/// any other layer blocks where one of the eight neighbouring 16-unit cells
/// samples deep water. So deep water carries a one-cell shore on both
/// sides of its edge. Cells outside `map` are left alone.
void bake_terrain_passability(Grid& pass, const Grid& terrain, const TerrainTable& table,
                              const WorldRect& rect, const WorldRect& map);

/// The frame every rebuild ends by re-imposing (0x00544bd0), which is why
/// every shipped layer is blocked along its edges: the first two and the
/// last two columns on every row; and, per 16-unit column, a top band from
/// row 0 to 48 units past the world row the map's top screen edge unprojects
/// to (rounded up to an odd multiple of 16 -- four rows on flat ground), and
/// a bottom band from 32 units above the world row that `project(map.y1) -
/// 287` unprojects to (rounded down to a multiple of 32) to the end -- 28
/// rows on flat ground, and lower where the ground at the edge rises, since
/// the unprojection reads the height. The 287 is 255 + 32: the tallest
/// ground plus one cell, the strip along the bottom where a screen row can
/// belong to something under the map's edge.
void frame_passability(Grid& pass, const Grid& height, const WorldRect& map);

/// The whole rebuild over `rect` (0x00547700): every 16-unit cell in the
/// rectangle cleared, the terrain baked back, then every object standing
/// within 1024 units of the rectangle re-stamped into it, then every
/// decoration whose 64-unit cell lies within 1024 units re-stamped at its
/// cell plus its four-unit offsets, and the frame put back over the whole
/// layer. `objects` is every footprint the caller has -- the function picks
/// the ones in range; `decor_masks` is indexed by decoration kind (`type`),
/// a null entry standing for a kind with no mask.
///
/// Not reproduced, and labelled: 0x00547c00, which the terrain stroke runs
/// first unless Shift is held, widens the rectangle to a shipyard's extent
/// when one stands near it -- on a layer that is already consistent the
/// wider rebuild lands on the same bits, which is why it can be left out.
void rebuild_passability(Grid& pass, const WorldRect& rect, const Grid& terrain,
                         const TerrainTable& table, const Grid& height,
                         std::span<const Footprint> objects, const Grid& decor,
                         std::span<const PassMask* const> decor_masks);

// --------------------------------------------------------------------------
// undo
// --------------------------------------------------------------------------

/// The editor's undo, as the original's manager keeps it (0x00499e50, one
/// per editor session at 0x0097b088): a record is a rectangle of up to
/// four layers copied *before* a brush touches them -- the terrain, the
/// height, the decorations, the passability, chosen by `Layer` bits --
/// tagged with a generation (0x0049a850). A stroke's press opens a new
/// generation and every step of its drag adds a record to the same one,
/// so one undo takes back one stroke: `undo` walks the newest generation
/// (0x0049a6a0), copies what the layers hold now into a redo record of the
/// same rectangle and flags, and writes the snapshot back; `redo` is the
/// mirror (0x00499ea0). A new snapshot drops the redo list (0x00499fa0).
/// Records are dropped oldest first once they hold more than four
/// megabytes (0x00494628). Only the brushes call it -- the terrain with all
/// four layers, the height with terrain, height and passability, the
/// decorations with decorations and passability -- and nothing the object
/// or area tools do is undoable, which is the original's choice, not this
/// engine's.
///
/// **Reading, labelled:** the original copies cells stepping 64 units from
/// the rectangle's corner, so a rectangle that is not cell-aligned can
/// leave its last partial cell out; here every cell the rectangle touches
/// is copied, a superset, and the rectangles the tools pass are wide enough
/// that the two never differ on what a brush changed. The light layer is
/// not in a record: it is a function of the height and is re-baked by the
/// caller over the rectangle an undo returns.
class UndoStack {
 public:
  enum Layer : std::uint32_t {
    kTerrain = 1,
    kHeight = 2,
    kDecor = 4,
    kPass = 8,
  };
  /// The live layers a record is taken from and written back to. A null
  /// layer is skipped whatever the flags say.
  struct Layers {
    Grid* terrain = nullptr;
    Grid* height = nullptr;
    Grid* decor = nullptr;
    Grid* pass = nullptr;
  };
  static constexpr std::size_t kByteCap = 4u << 20;

  /// Copy `rect` of the flagged layers as they are now, into a record of
  /// the current generation, or of a new one when `new_generation`.
  void snapshot(const Layers& layers, const WorldRect& rect, std::uint32_t flags,
                bool new_generation);
  /// Take back the newest generation. Returns the union of the rectangles
  /// restored, empty when there was nothing to take back.
  [[nodiscard]] WorldRect undo(const Layers& layers);
  [[nodiscard]] WorldRect redo(const Layers& layers);

  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  [[nodiscard]] std::size_t records() const noexcept { return undo_.size(); }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
  void clear();

 private:
  struct Record {
    WorldRect rect;
    std::uint32_t flags = 0;
    std::uint32_t generation = 0;
    std::vector<std::uint32_t> terrain;
    std::vector<std::uint32_t> height;
    std::vector<std::uint32_t> decor;
    std::vector<std::uint32_t> pass;
    [[nodiscard]] std::size_t size() const noexcept;
  };
  static Record take(const Layers& layers, const WorldRect& rect, std::uint32_t flags);
  static void put_back(const Layers& layers, const Record& record);
  WorldRect swap_generation(const Layers& layers, std::vector<Record>& from,
                            std::vector<Record>& to);

  std::vector<Record> undo_;
  std::vector<Record> redo_;
  std::size_t bytes_ = 0;
};

/// The rectangle a brush snapshots before it paints: the terrain tool
/// takes `(radius + 2)` cells around its cell's corner (0x004b4108), the
/// decoration tools 320 units, five cells, whatever the brush (0x0048d9c1),
/// and the height tools the cells their disc covers. `half` is in world
/// units; the rectangle is the corner `+- half`.
[[nodiscard]] WorldRect undo_rect(sim::Point corner, std::int32_t half) noexcept;

/// The decoration's world position: its 64-unit cell's corner plus the
/// four-unit offsets the cell's high byte carries (the table 0x005441a0
/// builds: `nx * 4, ny * 4`).
[[nodiscard]] sim::Point decor_position(std::int32_t cx, std::int32_t cy,
                                        std::uint32_t cell) noexcept;

// --------------------------------------------------------------------------
// height
// --------------------------------------------------------------------------

enum class HeightTool : std::uint8_t {
  kRaiseLower,  ///< HeightPaintSettings.ini: `Amount` added, once per stroke
  kSmooth,      ///< HeightBlurSettings.ini: a weighted mean of the four neighbours
  kSet,         ///< HeightSetSettings.ini: every cell to `Level`
};

/// The height a `Level` of 0..100 sets: `Level * 256 / 101` (0x0049d174),
/// so 100 is 253. And back, for the right button: `H * 101 / 256`.
[[nodiscard]] std::int32_t height_of_level(std::int32_t level) noexcept;
[[nodiscard]] std::int32_t level_of_height(std::int32_t height) noexcept;

/// What Raise/lower adds per cell for an `Amount` of -100..100:
/// `clamp(((Amount + 100) * 256 / 200) / 2 - 64, -64, 64)`, flat across
/// the disc with no falloff.
[[nodiscard]] std::int32_t height_delta(std::int32_t amount) noexcept;

/// A height stroke in progress. Raise/lower writes each cell once per
/// stroke, relative to the height it had when the button went down -- so a
/// drag back over painted ground does not pile up -- and the snapshot and
/// the written set live here from press to release.
struct HeightStroke {
  std::vector<std::uint8_t> before;  ///< the whole layer at the press
  std::vector<bool> written;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  void begin(const Grid& heights);
};

/// One application of a height tool at 32-unit cell `(cx, cy)`. `value`
/// is the `Amount` (raise/lower) or the `Level` (set); smooth ignores it.
/// Smooth is the original's in-place pass in raster order: `(64 c + 26 s) /
/// (64 + 26 n)` for the sum `s` of the `n` in-range four-neighbours, every
/// step, with no once-per-stroke rule. Returns the cells changed.
[[nodiscard]] CellRect apply_height(Grid& heights, HeightStroke& stroke, HeightTool tool,
                                    std::int32_t cx, std::int32_t cy, std::int32_t radius,
                                    std::int32_t value);

/// The baked light over `cells` of the height grid, as the height commit
/// recomputes it (0x00544e10): `clamp(H[x, y] - (H[x + 1, y] + H[x, y + 1])
/// / 2 + 16, 0, 21)`, and the neutral 16 on the far row and column, where
/// there is no neighbour. This is the shipped layer's whole meaning -- the
/// 0..21 around 16 `docs/formats/map.md` measured is a slope.
void rebake_light(Grid& light, const Grid& heights, const CellRect& cells);

// --------------------------------------------------------------------------
// decorations
// --------------------------------------------------------------------------

/// An exact stamp: kind `kind` at `world`, its sub-cell offsets the
/// position's remainder in the 64-unit cell rounded to four-unit steps
/// (0x0048c370 rounds, and a remainder that rounds to 16 carries into the
/// next cell). An occupied cell is overwritten. Returns the cell written,
/// or an empty rectangle off the grid.
[[nodiscard]] CellRect stamp_decor(Grid& decor, sim::Point world, std::int32_t kind);

/// A disc of decorations around cell `(cx, cy)`: each empty cell of the
/// disc gets a kind drawn from `kinds` with random offsets when
/// `rand(0, 100) <= density` (0x0048db40 skips a cell when the draw exceeds
/// the density, and skips an occupied one). Returns the cells written.
[[nodiscard]] CellRect scatter_decor(Grid& decor, std::int32_t cx, std::int32_t cy,
                                     std::int32_t radius, std::span<const std::int32_t> kinds,
                                     std::int32_t density, sim::Rng& rng);

/// Every cell of the disc cleared (0x0048c910). Returns the cells that held
/// something.
[[nodiscard]] CellRect clear_decor(Grid& decor, std::int32_t cx, std::int32_t cy,
                                   std::int32_t radius);

}  // namespace imperivm::core::edit
