// The object grid. Rationale: sim/spatial_index.hpp.

#include "imperivm/core/sim/spatial_index.hpp"

#include <algorithm>

#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

void SpatialIndex::clear() noexcept {
  cells_.clear();
}

std::int32_t SpatialIndex::axis_cell(std::int64_t v) noexcept {
  // Arithmetic shift is a floor, so a negative coordinate lands below zero and
  // is clamped into cell 0 rather than rounded into it.
  const std::int64_t cell = v >> kCellShift;
  if (cell < 0) return 0;
  if (cell >= kCellsPerSide) return kCellsPerSide - 1;
  return static_cast<std::int32_t>(cell);
}

std::int32_t SpatialIndex::cell_of(Point at) noexcept {
  return axis_cell(at.y) * kCellsPerSide + axis_cell(at.x);
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
  for (std::size_t i = 0; i < cell.size(); ++i) {
    if (cell[i].id != slot.id) continue;
    // Order inside a cell carries nothing -- every query sorts -- so the entry
    // is swapped out rather than erased.
    cell[i] = cell.back();
    cell.pop_back();
    break;
  }
  slot.spatial.cell = -1;
}

void SpatialIndex::file(WorldObject& slot) {
  // An internal object stands nowhere, and neither does a held one: its
  // position is `(-1, -1)`, which 0x0053d030 maps to no cell at all.
  if (slot.internal != InternalKind::none || slot.state.is_held()) {
    drop(slot);
    return;
  }
  if (cells_.empty()) cells_.resize(kCells);
  const Point at = slot.state.position;
  const std::int32_t cell = cell_of(at);
  if (cell == slot.spatial.cell) {
    for (Entry& entry : cells_[static_cast<std::size_t>(cell)]) {
      if (entry.id == slot.id) {
        entry.at = at;
        return;
      }
    }
  }
  unfile_cell(slot);
  cells_[static_cast<std::size_t>(cell)].push_back(Entry{slot.id, at});
  slot.spatial.cell = cell;
}

void SpatialIndex::drop(WorldObject& slot) noexcept { unfile_cell(slot); }

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
    if (slot.internal != InternalKind::none || slot.state.is_held()) {
      if (slot.spatial.cell >= 0) return false;
      continue;
    }
    if (cells_.empty()) return false;
    if (slot.spatial.cell != cell_of(slot.state.position)) return false;
    bool found = false;
    for (const Entry& entry : cells_[static_cast<std::size_t>(slot.spatial.cell)]) {
      if (entry.id != slot.id) continue;
      if (entry.at != slot.state.position) return false;
      found = true;
      break;
    }
    if (!found) return false;
    ++in_grid;
  }
  std::size_t filed = 0;
  for (const std::vector<Entry>& cell : cells_) filed += cell.size();
  return filed == in_grid;
}

}  // namespace imperivm::core::sim
