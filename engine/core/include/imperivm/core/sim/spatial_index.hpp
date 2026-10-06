#pragma once

// A uniform grid over object positions, so that a spatial query visits the
// cells its box touches instead of every object in the world.
//
// **Derived state, and nothing else.** The grid is never hashed, never saved,
// and never the order of an answer: `World` rebuilds it on load and refiles an
// object in every mutator that moves it or changes its holder (`set_position`,
// `put_in_holder`, `remove_from_holder`, spawn, despawn, a template's copy, a
// class mutation, the map loader). A query takes its *candidates* from the grid,
// puts them back into spawn order, and applies exactly the test the linear scan
// applied, so the answer is the scan's answer, id for id and in the same order.
// Iteration order is state; the order the grid holds its entries in is not
// allowed to reach anything.
//
// Only an object that stands somewhere is filed: in the cell of its own
// `state.position`, with that position copied into the entry so the sweep can
// test it without a lookup.
//
// A **held** object is filed nowhere, and no spatial query finds it. That is
// the original's grid: the holder entry 0x005d3e10 sets the unit's position to
// `(-1, -1)` (0x005d3ef9), the relink in 0x0053f000 asks 0x0053d030 for the
// cell of that point, and 0x0053d030 answers none for `x == -1`, so the object
// leaves its cell's list; every area sweep (the circle at 0x004fde60,
// `EnemyInRange`'s square at 0x004dbea0, and the rest) walks those lists and
// nothing else. This index once kept held objects in a list of their own and
// resolved each to its holder at every query -- which, a garrison's holder
// record standing at (0, 0), found every garrison on the map in the corner.
//
// Internal objects -- settlements, holders, warehouses, queries -- are filed
// nowhere either, since no spatial query can answer with one.

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
};

class SpatialIndex {
 public:
  /// 512-unit cells, 64 to a side: 32,768 units, the largest map there is.
  /// Anything outside is filed in the nearest border cell, which is still
  /// exact because the clamp is monotonic -- a box's cell range clamped the
  /// same way always includes the border cells a far object was filed in.
  static constexpr std::int32_t kCellShift = 9;
  static constexpr std::int32_t kCellsPerSide = 64;
  static constexpr std::int32_t kCells = kCellsPerSide * kCellsPerSide;

  struct Entry {
    ObjectId id = kNoObject;
    Point at{};
  };

  void clear() noexcept;

  /// File `slot` where it is now -- in its cell, or (being internal or held)
  /// nowhere -- moving it from wherever it was filed before.
  void file(WorldObject& slot);

  /// Take `slot` out of the index altogether (despawn).
  void drop(WorldObject& slot) noexcept;

  /// Append to `out` the id of every grid entry whose position is in `box`.
  /// Unordered.
  void gather(const SpatialBox& box, std::vector<ObjectId>& out) const;

  /// How many cells `box` touches, for the caller's choice between the grid
  /// and a straight scan.
  [[nodiscard]] static std::int64_t cells_touched(const SpatialBox& box) noexcept;

  /// True when every object is filed exactly where `file` would file it now,
  /// and nothing else is filed. What the tests ask; the simulation never does.
  [[nodiscard]] bool consistent(std::span<const WorldObject> objects) const;

 private:
  [[nodiscard]] static std::int32_t axis_cell(std::int64_t v) noexcept;
  [[nodiscard]] static std::int32_t cell_of(Point at) noexcept;
  void unfile_cell(WorldObject& slot) noexcept;

  std::vector<std::vector<Entry>> cells_;
};

}  // namespace imperivm::core::sim
