#pragma once

// The object grid: the original's own, cell for cell and list for list
// (`docs/formats/spatial-index.md` has the model with its addresses).
//
// **The grid.** 256-unit cells, 128 to a row, row-major: the cell of a point
// is `(y / 256) * 128 + x / 256`, the divisions truncating (0x0053d030). A
// point whose `x` is -1 is in no cell -- the marker a held object stands at
// (the holder entry 0x005d3e10 writes `(-1, -1)`, 0x005d3ef9) -- so a held
// object is filed nowhere and no area sweep finds it. Internal objects --
// settlements, holders, warehouses, queries -- are filed nowhere either.
//
// **A cell's list is in arrival order.** Each cell holds a doubly linked list
// of the objects standing in it. The position setter (0x0053f000) relinks an
// object only when the point it moves to is in another cell: out of the old
// list where it stood (0x0053d630, which closes the gap and keeps the rest in
// order) and onto the new list's **tail** (0x0053ce50). A move inside one
// cell does not touch the list. So a cell lists its objects in the order they
// last came into it, and a map's own objects, linked as they load
// (0x00540540), in the map's order.
//
// **The walk.** Every area sweep in the original (the query sweeps 0x004fd060,
// 0x004fd2d0, 0x004fd9c0, 0x004fdc20, 0x004fde60; the nearest-by-grid search
// 0x0053e290; some forty more that share the cell-circle test 0x00416810)
// clamps its box to the map, then visits the box's rows top to bottom, each
// row's cells left to right, and each cell's list head to tail. `gather`
// answers in that order.
//
// **State, and saved.** The order inside a cell is history -- who came in
// first -- so it is part of the game: each object carries the stamp of its
// last arrival (`SpatialSlot::arrival`), the world section saves it (and the
// next stamp), and a
// load files every cell's objects back in stamp order. (The original relinks
// its objects as they load, which on its own load order would reorder every
// cell; this engine keeps the order, so that a loaded game plays as the saved
// one would have.) The cell is derived from the position and is not saved.
// Neither is hashed: what the order changes, the hashed state shows.
//
// `World`'s queries answer either in this order or, for the callers not yet
// read against a grid sweep, put back into spawn (id) order; see
// `SweepOrder` in `sim/world.hpp`.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

struct WorldObject;

/// A closed box in world units, `[left, right] x [top, bottom]`. 64-bit so
/// that `centre +- radius` cannot overflow for any radius a script can pass.
struct SpatialBox {
  std::int64_t left = 0;
  std::int64_t top = 0;
  std::int64_t right = -1;
  std::int64_t bottom = -1;

  [[nodiscard]] constexpr bool contains(Point at) const noexcept {
    return at.x >= left && at.x <= right && at.y >= top && at.y <= bottom;
  }
};

/// Where the index has filed one object. Lives on `WorldObject`, so a copied
/// `World` carries a grid that still agrees with it.
struct SpatialSlot {
  std::int32_t cell = -1;  ///< grid cell, or -1 when not in the grid
  /// The stamp of the object's last arrival in a cell: its place in that
  /// cell's list, earlier first. Saved; 0 for an object never filed.
  std::uint64_t arrival = 0;
};

class SpatialIndex {
 public:
  /// 256-unit cells, 128 to a side: 32,768 units, the largest map there is
  /// (the `>> 8` and the `* 0x80` of 0x0053d030). Anything outside is filed in
  /// the nearest border cell, where the original's arithmetic would run off its
  /// array; no object stands there on a shipped map.
  static constexpr std::int32_t kCellShift = 8;
  static constexpr std::int32_t kCellSize = 1 << kCellShift;
  static constexpr std::int32_t kCellsPerSide = 128;
  static constexpr std::int32_t kCells = kCellsPerSide * kCellsPerSide;

  struct Entry {
    ObjectId id = kNoObject;
    Point at{};
    std::uint64_t arrival = 0;
  };

  void clear() noexcept;

  /// File `slot` where it is now: left where it is in its cell's list when it
  /// is still in that cell, else unlinked and appended to its new cell's tail
  /// with a fresh arrival stamp -- or, being internal or held, filed nowhere.
  void file(WorldObject& slot);

  /// Take `slot` out of the index altogether (despawn).
  void drop(WorldObject& slot) noexcept;

  /// File every object afresh, each cell's list in the order of the stamps the
  /// objects carry (a load), and stamp on from `next_arrival` -- or from past
  /// the latest stamp, should that be later.
  void rebuild(std::span<WorldObject> objects, std::uint64_t next_arrival);

  /// The stamp the next arrival takes. Saved with the world, so that a loaded
  /// game stamps exactly as the saved one would have.
  [[nodiscard]] std::uint64_t next_arrival() const noexcept { return next_arrival_; }

  /// Append to `out` the id of every grid entry whose position is in `box`, in
  /// the grid's walk order: rows top to bottom, cells left to right, each
  /// cell's list from its head.
  void gather(const SpatialBox& box, std::vector<ObjectId>& out) const;

  /// How many cells `box` touches, for the caller's choice between the grid
  /// and a straight scan.
  [[nodiscard]] static std::int64_t cells_touched(const SpatialBox& box) noexcept;

  /// The cell a point is filed in, the row times 128 plus the column.
  [[nodiscard]] static std::int32_t cell_of(Point at) noexcept;

  /// True when an object stands in the grid at all: not internal, not held.
  [[nodiscard]] static bool stands(const WorldObject& slot) noexcept;

  /// True when every object is filed exactly where `file` would file it now,
  /// every cell's list is in stamp order, and nothing else is filed. What the
  /// tests ask; the simulation never does.
  [[nodiscard]] bool consistent(std::span<const WorldObject> objects) const;

 private:
  [[nodiscard]] static std::int32_t axis_cell(std::int64_t v) noexcept;
  void unfile_cell(WorldObject& slot) noexcept;

  std::vector<std::vector<Entry>> cells_;
  std::uint64_t next_arrival_ = 1;
};

}  // namespace imperivm::core::sim
