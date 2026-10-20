// The object grid. Rationale: sim/spatial_index.hpp and
// docs/formats/spatial-index.md.

#include "imperivm/core/sim/spatial_index.hpp"

#include <algorithm>

#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

void SpatialIndex::clear() noexcept {
  cells_.clear();
  next_arrival_ = 1;
}

std::int32_t SpatialIndex::axis_cell(std::int64_t v) noexcept {
  // The original truncates toward zero; for the coordinates a map holds,
  // which are never below zero, that is the shift. A negative one clamps into
  // cell 0, which is where truncation puts everything above -256 too.
  const std::int64_t cell = v >> kCellShift;
  if (cell < 0) return 0;
  if (cell >= kCellsPerSide) return kCellsPerSide - 1;
  return static_cast<std::int32_t>(cell);
}

std::int32_t SpatialIndex::cell_of(Point at) noexcept {
  return axis_cell(at.y) * kCellsPerSide + axis_cell(at.x);
}

bool SpatialIndex::stands(const WorldObject& slot) noexcept {
  return slot.internal == InternalKind::none && !slot.state.is_held();
}

std::int64_t SpatialIndex::cells_touched(const SpatialBox& box) noexcept {
  if (box.right < box.left || box.bottom < box.top) return 0;
  const std::int64_t w = axis_cell(box.right) - axis_cell(box.left) + 1;
  const std::int64_t h = axis_cell(box.bottom) - axis_cell(box.top) + 1;
  return w * h;
}

void SpatialIndex::unfile_cell(WorldObject& slot) noexcept {
  if (slot.spatial.cell < 0) return;
  std::vector<Entry>& cell = cells_[static_cast<std::size_t>(slot.spatial.cell)];
  // Out of the list where it stands, the rest closing up in their order: the
  // list is the walk's order (0x0053d630 unlinks a node of a doubly linked
  // list).
  const auto at = std::find_if(cell.begin(), cell.end(),
                               [&slot](const Entry& entry) { return entry.id == slot.id; });
  if (at != cell.end()) cell.erase(at);
  slot.spatial.cell = -1;
}

void SpatialIndex::file(WorldObject& slot) {
  if (!stands(slot)) {
    drop(slot);
    return;
  }
  if (cells_.empty()) cells_.resize(kCells);
  const Point at = slot.state.position;
  const std::int32_t cell = cell_of(at);
  if (cell == slot.spatial.cell) {
    // Still in its cell: the list is not touched (0x0053f000 relinks only on a
    // change of cell), only the copy of the position the sweep tests.
    for (Entry& entry : cells_[static_cast<std::size_t>(cell)]) {
      if (entry.id == slot.id) {
        entry.at = at;
        return;
      }
    }
  }
  unfile_cell(slot);
  // Onto the new cell's tail (0x0053ce50), with the stamp that says so.
  slot.spatial.arrival = next_arrival_++;
  cells_[static_cast<std::size_t>(cell)].push_back(Entry{slot.id, at, slot.spatial.arrival});
  slot.spatial.cell = cell;
}

void SpatialIndex::drop(WorldObject& slot) noexcept { unfile_cell(slot); }

void SpatialIndex::rebuild(std::span<WorldObject> objects, std::uint64_t next_arrival) {
  cells_.clear();
  cells_.resize(kCells);
  next_arrival_ = std::max<std::uint64_t>(next_arrival, 1);
  for (WorldObject& slot : objects) {
    slot.spatial.cell = -1;
    // A held object's stamp is its place in its holder's roster: past it too.
    next_arrival_ = std::max(next_arrival_, slot.spatial.arrival + 1);
    if (!stands(slot)) continue;
    const std::int32_t cell = cell_of(slot.state.position);
    cells_[static_cast<std::size_t>(cell)].push_back(
        Entry{slot.id, slot.state.position, slot.spatial.arrival});
    slot.spatial.cell = cell;
  }
  // Each list back into the order its objects came in. The objects are taken
  // in id order above, so equal stamps -- none, unless a file was written by
  // hand -- fall back on spawn order.
  for (std::vector<Entry>& cell : cells_) {
    std::stable_sort(cell.begin(), cell.end(), [](const Entry& a, const Entry& b) {
      return a.arrival < b.arrival;
    });
  }
}

void SpatialIndex::gather(const SpatialBox& box, std::vector<ObjectId>& out) const {
  if (cells_.empty() || box.right < box.left || box.bottom < box.top) return;
  const std::int32_t x0 = axis_cell(box.left);
  const std::int32_t x1 = axis_cell(box.right);
  const std::int32_t y0 = axis_cell(box.top);
  const std::int32_t y1 = axis_cell(box.bottom);
  for (std::int32_t cy = y0; cy <= y1; ++cy) {
    for (std::int32_t cx = x0; cx <= x1; ++cx) {
      for (const Entry& entry : cells_[static_cast<std::size_t>(cy * kCellsPerSide + cx)]) {
        if (box.contains(entry.at)) out.push_back(entry.id);
      }
    }
  }
}

bool SpatialIndex::consistent(std::span<const WorldObject> objects) const {
  std::size_t in_grid = 0;
  for (const WorldObject& slot : objects) {
    if (!stands(slot)) {
      if (slot.spatial.cell >= 0) return false;
      continue;
    }
    if (cells_.empty()) return false;
    if (slot.spatial.cell != cell_of(slot.state.position)) return false;
    if (slot.spatial.arrival >= next_arrival_) return false;
    bool found = false;
    for (const Entry& entry : cells_[static_cast<std::size_t>(slot.spatial.cell)]) {
      if (entry.id != slot.id) continue;
      if (entry.at != slot.state.position || entry.arrival != slot.spatial.arrival) return false;
      found = true;
      break;
    }
    if (!found) return false;
    ++in_grid;
  }
  std::size_t filed = 0;
  for (const std::vector<Entry>& cell : cells_) {
    filed += cell.size();
    for (std::size_t i = 1; i < cell.size(); ++i) {
      if (cell[i - 1].arrival >= cell[i].arrival) return false;
    }
  }
  return filed == in_grid;
}

}  // namespace imperivm::core::sim
