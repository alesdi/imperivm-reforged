#include "imperivm/core/sim/lsa.hpp"

#include <vector>

namespace imperivm::core::sim {
namespace {

/// The terrain byte under a world point, with the `+ half` round-to-nearest the
/// original applies before it divides (`IsPointInWater`'s rounding, and the
/// baker's). Off the layer answers 0, which is not water.
[[nodiscard]] std::uint32_t terrain_at(const Grid& terrain, Point where) noexcept {
  if (terrain.cell_size() == 0) return 0;
  const auto half = static_cast<std::int32_t>(terrain.cell_size() / 2);
  const std::int32_t x = where.x + half;
  const std::int32_t y = where.y + half;
  if (x < 0 || y < 0) return 0;
  const auto cx = static_cast<std::uint32_t>(x) / terrain.cell_size();
  const auto cy = static_cast<std::uint32_t>(y) / terrain.cell_size();
  if (cx >= terrain.width() || cy >= terrain.height()) return 0;
  return terrain.cell(cx, cy);
}

}  // namespace

void LsaPartition::build(const Grid& terrain, const ObstructionGrid& passability) {
  width_ = passability.width();
  height_ = passability.height();
  labels_.clear();
  areas_.clear();
  slot_labels_.clear();
  neighbour_starts_.clear();
  neighbours_.clear();
  slot_columns_ = 0;
  slot_rows_ = 0;
  if (width_ <= 0 || height_ <= 0 || terrain.cell_size() == 0) {
    width_ = 0;
    height_ = 0;
    return;
  }
  labels_.assign(static_cast<std::size_t>(width_) * height_, kNoLsa);

  const auto index = [&](std::int32_t x, std::int32_t y) {
    return static_cast<std::size_t>(y) * width_ + x;
  };
  const auto centre = [](std::int32_t cell) { return ObstructionGrid::centre_of(cell); };

  // "Taken" is the original's private copy of the passability bitmap, which it
  // *writes into* as the fill consumes cells so that blocked and claimed are
  // one test. Kept as its own vector here rather than mutating the grid, which
  // this engine shares with the pathfinder.
  std::vector<bool> taken(labels_.size(), false);
  for (std::int32_t y = 0; y < height_; ++y) {
    for (std::int32_t x = 0; x < width_; ++x) {
      if (passability.blocked_cell(x, y)) taken[index(x, y)] = true;
    }
  }

  const std::int32_t slot_cells = kLsaSlotSize / kCollisionCellSize;
  slot_columns_ = (width_ + slot_cells - 1) / slot_cells;
  slot_rows_ = (height_ + slot_cells - 1) / slot_cells;
  const auto slot_stride = static_cast<std::size_t>(slot_columns_);
  const std::size_t slot_count = slot_stride * static_cast<std::size_t>(slot_rows_);
  slot_labels_.assign(slot_count, kNoLsa);
  // The claim marks, which outlive every fill: a slot is the first fill's
  // that touches it, and a fill that is then discarded keeps the claim with
  // no label behind it. See the header.
  std::vector<bool> slot_claimed(slot_count, false);
  std::vector<bool> touched;
  std::vector<std::size_t> frontier;
  std::vector<std::size_t> claimed;
  // The water pass first, then ground. Which pass may *seed* a cell is the only
  // thing terrain decides; the fill itself never looks at it again.
  //
  // **The order is the original's and it is deliberately not tested**, because
  // the only thing it can change is which of two areas gets the lower id --
  // and `sim/lsa.hpp` argues at length that the numbering is this engine's own
  // to choose, because no script ever writes an id down or compares one against
  // a literal. Fault injection agrees: swapping the two passes changes no test
  // in the suite, and a test that caught it would be asserting the one property
  // the header says is free. Written as the original writes it, for the reader
  // rather than for the behaviour.
  for (int pass = 0; pass < 2; ++pass) {
    const bool want_water = pass == 0;
    for (std::int32_t y = 0; y < height_; ++y) {
      for (std::int32_t x = 0; x < width_; ++x) {
        if (taken[index(x, y)]) continue;
        const bool here_water =
            terrain_at(terrain, Point{centre(x), centre(y)}) == kDeepWaterTerrain;
        if (here_water != want_water) continue;

        claimed.clear();
        frontier.clear();
        touched.assign(slot_count, false);
        std::int32_t coverage = 0;
        taken[index(x, y)] = true;
        frontier.push_back(index(x, y));
        while (!frontier.empty()) {
          const std::size_t cell = frontier.back();
          frontier.pop_back();
          claimed.push_back(cell);
          // Coverage, which is what the size threshold is measured against: the
          // counter advances on every cell *except* the first one in each
          // 128-unit slot, so it is `cells - slots touched`.
          {
            const auto sx = static_cast<std::int32_t>(cell % static_cast<std::size_t>(width_)) /
                            slot_cells;
            const auto sy = static_cast<std::int32_t>(cell / static_cast<std::size_t>(width_)) /
                            slot_cells;
            const std::size_t slot = static_cast<std::size_t>(sy) * slot_stride + sx;
            if (touched[slot]) {
              ++coverage;
            } else {
              touched[slot] = true;
            }
          }
          const auto cx = static_cast<std::int32_t>(cell % static_cast<std::size_t>(width_));
          const auto cy = static_cast<std::int32_t>(cell / static_cast<std::size_t>(width_));
          const std::int32_t nx[4] = {cx - 1, cx + 1, cx, cx};
          const std::int32_t ny[4] = {cy, cy, cy - 1, cy + 1};
          for (int n = 0; n < 4; ++n) {
            if (nx[n] < 0 || ny[n] < 0 || nx[n] >= width_ || ny[n] >= height_) continue;
            const std::size_t at = index(nx[n], ny[n]);
            if (taken[at]) continue;
            taken[at] = true;
            frontier.push_back(at);
          }
        }

        // Too small to be a place. The cells are released rather than labelled
        // and the id is not consumed, so the numbering stays dense.
        const bool kept = coverage >= kMinLsaCoverage;
        const LsaId id = kept ? static_cast<LsaId>(areas_.size() + 1) : kNoLsa;
        // Every slot this fill touched and nobody claimed before is claimed
        // now -- with the new id, or with no id at all when the area is
        // about to be discarded, which is the poisoning the header describes.
        for (std::size_t slot = 0; slot < slot_count; ++slot) {
          if (!touched[slot] || slot_claimed[slot]) continue;
          slot_claimed[slot] = true;
          slot_labels_[slot] = id;
        }
        if (!kept) {
          for (const std::size_t cell : claimed) taken[cell] = false;
          taken[index(x, y)] = true;  // ...except the seed, or the scan re-finds it
          continue;
        }

        areas_.push_back(LsaArea{});
        LsaArea& area = areas_.back();
        area.cells = static_cast<std::int32_t>(claimed.size());
        std::int64_t sum_x = 0;
        std::int64_t sum_y = 0;
        for (const std::size_t cell : claimed) {
          labels_[cell] = id;
          const auto cx = static_cast<std::int32_t>(cell % static_cast<std::size_t>(width_));
          const auto cy = static_cast<std::int32_t>(cell / static_cast<std::size_t>(width_));
          sum_x += centre(cx);
          sum_y += centre(cy);
        }
        area.centroid = Point{static_cast<std::int32_t>(sum_x / area.cells),
                              static_cast<std::int32_t>(sum_y / area.cells)};
      }
    }
  }

  // The type census: one vote per slot an area claimed, cast by the terrain
  // at the slot's centre, and the area is water when more than half are deep
  // water. The pass that built an area proposes; this disposes.
  std::vector<std::int32_t> slots(areas_.size(), 0);
  std::vector<std::int32_t> wet(areas_.size(), 0);
  for (std::int32_t sy = 0; sy < slot_rows_; ++sy) {
    for (std::int32_t sx = 0; sx < slot_columns_; ++sx) {
      const LsaId id = slot_labels_[static_cast<std::size_t>(sy) * slot_stride + sx];
      if (id == kNoLsa) continue;
      const Point middle{sx * kLsaSlotSize + kLsaSlotSize / 2,
                         sy * kLsaSlotSize + kLsaSlotSize / 2};
      const auto i = static_cast<std::size_t>(id) - 1;
      ++slots[i];
      if (terrain_at(terrain, middle) == kDeepWaterTerrain) ++wet[i];
    }
  }
  for (std::size_t i = 0; i < areas_.size(); ++i) {
    areas_[i].water = slots[i] / 2 < wet[i];
  }

  // Adjacency: two labelled slots that touch on any of eight sides make their
  // areas neighbours when the areas are of different types. Gathered in a
  // matrix first so that a pair touching along a whole coast is listed once,
  // then read out ascending, which is the order the original's lists carry.
  const std::size_t n = areas_.size();
  std::vector<std::uint8_t> adjacent(n * n, 0);
  constexpr std::int32_t kAround[8][2] = {{-1, 1}, {-1, 0}, {-1, -1}, {0, -1},
                                          {1, -1}, {1, 0},  {1, 1},   {0, 1}};
  for (std::int32_t sy = 0; sy < slot_rows_; ++sy) {
    for (std::int32_t sx = 0; sx < slot_columns_; ++sx) {
      const LsaId a = slot_labels_[static_cast<std::size_t>(sy) * slot_stride + sx];
      if (a == kNoLsa) continue;
      for (const auto& step : kAround) {
        const LsaId b = slot_label(sx + step[0], sy + step[1]);
        if (b == kNoLsa || b == a) continue;
        if (areas_[static_cast<std::size_t>(a) - 1].water ==
            areas_[static_cast<std::size_t>(b) - 1].water) {
          continue;
        }
        // Both halves, as the original writes them. The eight offsets are
        // symmetric, so the scan from `b`'s slot would set the mirror on its
        // own; a sweep cannot tell the two apart, and this is the reading.
        adjacent[(static_cast<std::size_t>(a) - 1) * n + (static_cast<std::size_t>(b) - 1)] = 1;
        adjacent[(static_cast<std::size_t>(b) - 1) * n + (static_cast<std::size_t>(a) - 1)] = 1;
      }
    }
  }
  neighbour_starts_.assign(n + 1, 0);
  for (std::size_t i = 0; i < n; ++i) {
    neighbour_starts_[i] = static_cast<std::uint32_t>(neighbours_.size());
    for (std::size_t j = 0; j < n; ++j) {
      if (adjacent[j * n + i] != 0) neighbours_.push_back(static_cast<LsaId>(j + 1));
    }
  }
  neighbour_starts_[n] = static_cast<std::uint32_t>(neighbours_.size());
}

std::span<const LsaId> LsaPartition::neighbours(LsaId id) const noexcept {
  if (id < 1 || static_cast<std::size_t>(id) > areas_.size() ||
      neighbour_starts_.size() != areas_.size() + 1) {
    return {};
  }
  const std::uint32_t from = neighbour_starts_[static_cast<std::size_t>(id) - 1];
  const std::uint32_t to = neighbour_starts_[static_cast<std::size_t>(id)];
  return std::span<const LsaId>(neighbours_).subspan(from, to - from);
}

LsaId LsaPartition::slot_label(std::int32_t sx, std::int32_t sy) const noexcept {
  if (sx < 0 || sy < 0 || sx >= slot_columns_ || sy >= slot_rows_) return kNoLsa;
  return slot_labels_[static_cast<std::size_t>(sy) * static_cast<std::size_t>(slot_columns_) + sx];
}

LsaId LsaPartition::at(Point where) const noexcept {
  if (width_ <= 0 || height_ <= 0) return kNoLsa;
  const std::int32_t cx = ObstructionGrid::cell_of(where.x);
  const std::int32_t cy = ObstructionGrid::cell_of(where.y);
  if (cx < 0 || cy < 0 || cx >= width_ || cy >= height_) return kNoLsa;
  return labels_[static_cast<std::size_t>(cy) * width_ + cx];
}

LsaId LsaPartition::at_or_near(Point where) const noexcept {
  const LsaId here = at(where);
  // An empty partition needs no test of its own: every cell of every ring
  // fails the bounds check below.
  if (here != kNoLsa) return here;
  const std::int32_t cx = ObstructionGrid::cell_of(where.x);
  const std::int32_t cy = ObstructionGrid::cell_of(where.y);
  const std::int32_t reach = kLsaNearReach / kCollisionCellSize;
  for (std::int32_t r = 1; r <= reach; ++r) {
    for (std::int32_t dy = -r; dy <= r; ++dy) {
      // Interior rows touch the ring at their two ends only. Walking their
      // inside as well would give the same answer more slowly: those cells
      // belong to the smaller rings, which were all unlabelled to get here.
      const std::int32_t step = (dy == -r || dy == r) ? 1 : 2 * r;
      for (std::int32_t dx = -r; dx <= r; dx += step) {
        const std::int32_t x = cx + dx;
        const std::int32_t y = cy + dy;
        if (x < 0 || y < 0 || x >= width_ || y >= height_) continue;
        const LsaId label = labels_[static_cast<std::size_t>(y) * width_ + x];
        if (label != kNoLsa) return label;
      }
    }
  }
  return kNoLsa;
}

}  // namespace imperivm::core::sim
