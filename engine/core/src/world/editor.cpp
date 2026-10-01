#include "imperivm/core/world/editor.hpp"

#include <algorithm>

#include "imperivm/core/sim/flying.hpp"
#include "imperivm/core/sim/projection.hpp"

namespace imperivm::core::edit {
namespace {

constexpr std::int32_t kTerrainCell = 64;
constexpr std::int32_t kHeightCell = 32;
constexpr std::int32_t kPassCell = 16;
constexpr std::int32_t kShallowWater = kShallowWaterLayer;
constexpr std::int32_t kDeepWater = kDeepWaterLayer;

[[nodiscard]] constexpr std::int32_t floor_div(std::int32_t a, std::int32_t b) noexcept {
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}

[[nodiscard]] bool in_grid(const Grid& grid, std::int32_t x, std::int32_t y) noexcept {
  return x >= 0 && y >= 0 && x < static_cast<std::int32_t>(grid.width()) &&
         y < static_cast<std::int32_t>(grid.height());
}

[[nodiscard]] std::uint32_t at(const Grid& grid, std::int32_t x, std::int32_t y) noexcept {
  return grid.cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
}

void put(Grid& grid, std::int32_t x, std::int32_t y, std::uint32_t value) noexcept {
  (void)grid.set_cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), value);
}

[[nodiscard]] bool is_water(std::uint32_t z) noexcept {
  return z == static_cast<std::uint32_t>(kShallowWater) || z == static_cast<std::uint32_t>(kDeepWater);
}

[[nodiscard]] bool deep_eligible(const Grid& terrain, std::int32_t x, std::int32_t y) noexcept {
  return deep_water_eligible(terrain, x, y);
}

[[nodiscard]] std::int32_t isqrt(std::int64_t v) noexcept {
  std::int64_t r = 0;
  while ((r + 1) * (r + 1) <= v) ++r;
  return static_cast<std::int32_t>(r);
}

}  // namespace

bool deep_water_eligible(const Grid& terrain, std::int32_t x, std::int32_t y) noexcept {
  for (std::int32_t j = -4; j <= 4; ++j) {
    for (std::int32_t i = -4; i <= 4; ++i) {
      if (!in_grid(terrain, x + i, y + j)) return false;
      if (!is_water(at(terrain, x + i, y + j))) return false;
    }
  }
  return true;
}

void CellRect::add(std::int32_t x, std::int32_t y) noexcept {
  if (empty()) {
    x0 = x1 = x;
    y0 = y1 = y;
    return;
  }
  x0 = std::min(x0, x);
  y0 = std::min(y0, y);
  x1 = std::max(x1, x);
  y1 = std::max(y1, y);
}

void CellRect::merge(const CellRect& other) noexcept {
  if (other.empty()) return;
  add(other.x0, other.y0);
  add(other.x1, other.y1);
}

std::int32_t brush_radius(std::int32_t slot, bool height_tool) noexcept {
  slot = std::clamp(slot, 0, 4);
  if (height_tool) return 2 * slot + 1;
  return slot == 4 ? 5 : slot;
}

std::vector<sim::Point> brush_disc(std::int32_t radius) {
  std::vector<sim::Point> cells;
  radius = std::max(0, radius);
  for (std::int32_t j = -radius; j <= radius; ++j) {
    for (std::int32_t i = -radius; i <= radius; ++i) {
      if (i * i + j * j <= radius * radius) cells.push_back(sim::Point{i, j});
    }
  }
  return cells;
}

// --------------------------------------------------------------------------
// terrain
// --------------------------------------------------------------------------

sim::Point terrain_brush_cell(sim::Point world) noexcept {
  const std::int32_t sx = floor_div(world.x + kTerrainCell / 2, kTerrainCell) * kTerrainCell;
  const std::int32_t sy = floor_div(world.y + kTerrainCell / 2, kTerrainCell) * kTerrainCell;
  return sim::Point{floor_div(sx + kTerrainCell / 2, kTerrainCell),
                    floor_div(sy + kTerrainCell / 2, kTerrainCell)};
}

bool terrain_stroke_moved(sim::Point last, sim::Point now) noexcept {
  const std::int64_t dx = now.x - last.x;
  const std::int64_t dy = now.y - last.y;
  return isqrt(dx * dx + dy * dy) > kTerrainCell / 2;
}

CellRect paint_terrain(Grid& terrain, std::int32_t cx, std::int32_t cy, std::int32_t radius,
                       std::span<const std::int32_t> layers, bool water_group, bool shift,
                       sim::Rng& rng) {
  CellRect changed;
  if (!terrain.writable() || layers.empty()) return changed;
  const std::vector<sim::Point> disc = brush_disc(radius);
  const auto pick = [&]() -> std::int32_t {
    if (water_group) return kDeepWater;
    if (layers.size() == 1) return layers[0];
    return layers[static_cast<std::size_t>(rng.between(0, static_cast<std::int32_t>(layers.size()) - 1))];
  };
  const auto write = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
    if (!in_grid(terrain, x, y)) return;
    if (at(terrain, x, y) == static_cast<std::uint32_t>(z)) return;
    if (static_cast<std::uint32_t>(z) > terrain.max_cell_value()) return;
    put(terrain, x, y, static_cast<std::uint32_t>(z));
    changed.add(x, y);
  };
  // Deep water is two passes: shallow everywhere first, then deep where the
  // neighbourhood allows it -- or everywhere, under Shift.
  const bool deep = water_group || (layers.size() == 1 && layers[0] == kDeepWater);
  if (deep) {
    for (const sim::Point d : disc) {
      const std::int32_t x = cx + d.x;
      const std::int32_t y = cy + d.y;
      if (in_grid(terrain, x, y) && at(terrain, x, y) != static_cast<std::uint32_t>(kDeepWater)) {
        write(x, y, kShallowWater);
      }
    }
    for (const sim::Point d : disc) {
      const std::int32_t x = cx + d.x;
      const std::int32_t y = cy + d.y;
      if (!in_grid(terrain, x, y)) continue;
      if (shift || deep_eligible(terrain, x, y)) write(x, y, kDeepWater);
    }
    return changed;
  }
  for (const sim::Point d : disc) {
    const std::int32_t x = cx + d.x;
    const std::int32_t y = cy + d.y;
    if (!in_grid(terrain, x, y)) continue;
    const std::int32_t z = pick();
    write(x, y, z);
    if (z == kShallowWater) continue;
    // Land under a deep cell's neighbourhood takes the depth away.
    for (std::int32_t j = -4; j <= 4; ++j) {
      for (std::int32_t i = -4; i <= 4; ++i) {
        const std::int32_t nx = x + i;
        const std::int32_t ny = y + j;
        if (!in_grid(terrain, nx, ny)) continue;
        if (at(terrain, nx, ny) != static_cast<std::uint32_t>(kDeepWater)) continue;
        if (!deep_eligible(terrain, nx, ny)) write(nx, ny, kShallowWater);
      }
    }
  }
  return changed;
}

// --------------------------------------------------------------------------
// passability
// --------------------------------------------------------------------------

namespace {

/// The mask's grid centre, in the mask's own units: a `.pass` is 2048 wide,
/// and 0x00546a80 offsets a set cell by `col * 16 - 1016` across and
/// `1032 - row * 16` down -- the centre less half a cell one way, plus half
/// a cell the other.
constexpr std::int32_t kMaskHalf = 1024;
constexpr std::int32_t kMaskCentreX = kMaskHalf - kPassCell / 2;  // 1016
constexpr std::int32_t kMaskCentreY = kMaskHalf + kPassCell / 2;  // 1032
/// How far a stamp can reach from its anchor, and so how far past a
/// rectangle the rebuild looks for the objects and decorations to put back.
constexpr std::int32_t kStampReach = 1024;

[[nodiscard]] bool pass_in(const Grid& pass, std::int32_t cx, std::int32_t cy) noexcept {
  return pass.bits_per_cell() == 1 && in_grid(pass, cx, cy);
}

[[nodiscard]] bool pass_get(const Grid& pass, std::int32_t cx, std::int32_t cy) noexcept {
  return pass_in(pass, cx, cy) && at(pass, cx, cy) != 0;
}

void pass_put(Grid& pass, std::int32_t cx, std::int32_t cy, bool blocked) noexcept {
  if (pass_in(pass, cx, cy)) put(pass, cx, cy, blocked ? 1u : 0u);
}

/// The terrain layer under world `(x, y)`, sampled the way every reader in
/// this family samples it: the cell holding `x >> 6`, or -1 off the grid.
[[nodiscard]] std::int32_t terrain_at(const Grid& terrain, std::int32_t x, std::int32_t y) noexcept {
  if (x < 0 || y < 0) return -1;
  const std::int32_t tx = x / kTerrainCell;
  const std::int32_t ty = y / kTerrainCell;
  if (!in_grid(terrain, tx, ty)) return -1;
  return static_cast<std::int32_t>(at(terrain, tx, ty));
}

/// The height under a world point as 0x0053eaa0 answers it: bilinear inside
/// the map, zero outside it, zero with no layer.
[[nodiscard]] std::int32_t height_under(const Grid& height, const WorldRect& map, sim::Point at) noexcept {
  if (height.cell_size() == 0 || !map.contains(at)) return 0;
  return sim::sample_height(height, at);
}

/// 0x00545100: whether the 16-unit cell at `(x, y)` -- on a `passable_water`
/// layer -- is blocked as the rim of its 64-unit cell. Only the four cells
/// along each edge of the terrain cell's sampling window can be; for those,
/// the neighbouring terrain cells across the rim (and the diagonal one at a
/// corner) must all be `deep`, or the cell blocks.
[[nodiscard]] bool water_rim_blocked(const Grid& terrain, const WorldRect& map, std::int32_t x,
                                     std::int32_t y, std::int32_t deep) noexcept {
  // The sample point of the cell, then its place in the 64-unit window.
  const std::int32_t ax = floor_div(x, kPassCell) * kPassCell + kTerrainCell / 2;
  const std::int32_t ay = floor_div(y, kPassCell) * kPassCell + kTerrainCell / 2;
  const std::int32_t rx = ax - floor_div(ax, kTerrainCell) * kTerrainCell;
  const std::int32_t ry = ay - floor_div(ay, kTerrainCell) * kTerrainCell;
  const bool left = rx == 0;
  const bool right = rx == kTerrainCell - kPassCell;
  const bool top = ry == 0;
  const bool bottom = ry == kTerrainCell - kPassCell;
  if (!left && !right && !top && !bottom) return false;
  const std::int32_t cx = floor_div(ax, kTerrainCell);
  const std::int32_t cy = floor_div(ay, kTerrainCell);
  const std::int32_t last_col = floor_div(map.x1, kTerrainCell);
  const std::int32_t last_row = floor_div(map.y1, kTerrainCell);
  const bool has_left = cx > 0;
  const bool has_top = cy > 0;
  const bool has_right = cx < last_col;
  const bool has_bottom = cy < last_row;
  const std::int32_t wx = cx * kTerrainCell;
  const std::int32_t wy = cy * kTerrainCell;
  const auto is_deep = [&](std::int32_t px, std::int32_t py) {
    return terrain_at(terrain, px, py) == deep;
  };
  bool diagonal = true;
  bool side_x = true;
  bool side_y = true;
  if (left) {
    if (top && has_left && has_top) diagonal = is_deep(wx - kTerrainCell, wy - kTerrainCell);
    if (bottom && has_left && has_bottom) diagonal = is_deep(wx - kTerrainCell, wy + kTerrainCell);
  }
  if (right) {
    if (top && has_right && has_top) diagonal = is_deep(wx + kTerrainCell, wy - kTerrainCell);
    if (bottom && has_right && has_bottom) diagonal = is_deep(wx + kTerrainCell, wy + kTerrainCell);
  }
  if (left && has_left) side_x = is_deep(wx - kTerrainCell, wy);
  if (top && has_top) side_y = is_deep(wx, wy - kTerrainCell);
  if (right && has_right) side_x = is_deep(wx + kTerrainCell, wy);
  if (bottom && has_bottom) side_y = is_deep(wx, wy + kTerrainCell);
  return !(side_x && side_y && diagonal);
}

}  // namespace

CellRect mask_bounds(const Grid& mask) {
  CellRect box;
  if (mask.bits_per_cell() != 1) return box;
  for (std::uint32_t row = 0; row < mask.height(); ++row) {
    for (std::uint32_t col = 0; col < mask.width(); ++col) {
      if (mask.blocked(col, row)) box.add(static_cast<std::int32_t>(col), static_cast<std::int32_t>(row));
    }
  }
  return box;
}

const PassMask* PassMaskLibrary::find(std::string_view path) const noexcept {
  for (const auto& entry : masks_) {
    if (entry->path == path) return entry->grid.cell_size() != 0 ? &entry->mask : nullptr;
  }
  return nullptr;
}

bool PassMaskLibrary::known(std::string_view path) const noexcept {
  for (const auto& entry : masks_) {
    if (entry->path == path) return true;
  }
  return false;
}

const PassMask* PassMaskLibrary::load(std::string_view path, std::span<const std::byte> bytes) {
  if (known(path)) return find(path);
  auto entry = std::make_unique<Entry>();
  entry->path = std::string(path);
  entry->bytes.assign(bytes.begin(), bytes.end());
  if (auto parsed = Grid::parse(entry->bytes); parsed.ok() && parsed->validate(entry->bytes.size()).ok() &&
      parsed->bits_per_cell() == 1 && parsed->cell_size() == static_cast<std::uint32_t>(kPassCell)) {
    entry->grid = parsed.value();
    entry->mask.grid = &entry->grid;
    entry->mask.bounds = mask_bounds(entry->grid);
  }
  masks_.push_back(std::move(entry));
  const Entry& kept = *masks_.back();
  return kept.grid.cell_size() != 0 ? &kept.mask : nullptr;
}

WorldRect WorldRect::clamped(const WorldRect& bounds) const noexcept {
  return WorldRect{std::max(x0, bounds.x0), std::max(y0, bounds.y0), std::min(x1, bounds.x1),
                   std::min(y1, bounds.y1)};
}

WorldRect WorldRect::of_map(const Grid& terrain) noexcept {
  if (terrain.cell_size() == 0) return WorldRect{};
  return WorldRect{0, 0, static_cast<std::int32_t>(terrain.extent_x()) - 1,
                   static_cast<std::int32_t>(terrain.extent_y()) - 1};
}

WorldRect terrain_stroke_rect(std::int32_t cx, std::int32_t cy, std::int32_t radius) noexcept {
  // The stroke's sample point is the cell's corner plus 32; the span is
  // `128 r + 255`, halved to `64 r + 127` before and the rest after.
  const std::int32_t px = cx * kTerrainCell - kTerrainCell / 2 + kTerrainCell / 2;
  const std::int32_t py = cy * kTerrainCell - kTerrainCell / 2 + kTerrainCell / 2;
  const std::int32_t span = radius * 2 * kTerrainCell + 255;
  const std::int32_t before = span / 2;
  return WorldRect{px - before, py - before, px - before + span, py - before + span};
}

WorldRect water_leaf_rect(std::int32_t cx, std::int32_t cy, std::int32_t radius) noexcept {
  const std::int32_t px = cx * kTerrainCell;
  const std::int32_t py = cy * kTerrainCell;
  const std::int32_t span = radius * 2 * kTerrainCell + 127;
  const std::int32_t before = span / 2;
  return WorldRect{px - before, py - before, px - before + span, py - before + span};
}

CellRect level_height(Grid& heights, const WorldRect& rect, std::int32_t level) {
  CellRect changed;
  if (rect.empty() || !heights.writable() || heights.cell_size() == 0) return changed;
  const std::int32_t x0 = floor_div(rect.x0, kHeightCell);
  const std::int32_t y0 = floor_div(rect.y0, kHeightCell);
  const std::int32_t x1 = floor_div(rect.x1, kHeightCell);
  const std::int32_t y1 = floor_div(rect.y1, kHeightCell);
  const auto value = static_cast<std::uint32_t>(std::clamp(level, 0, 255));
  for (std::int32_t y = y0; y <= y1; ++y) {
    for (std::int32_t x = x0; x <= x1; ++x) {
      if (!in_grid(heights, x, y)) continue;
      put(heights, x, y, value);
      changed.add(x, y);
    }
  }
  return changed;
}

CellRect clear_decor_in(Grid& decor, const WorldRect& rect) {
  CellRect cleared;
  if (rect.empty() || !decor.writable() || decor.cell_size() == 0) return cleared;
  const std::int32_t x0 = floor_div(rect.x0, kTerrainCell);
  const std::int32_t y0 = floor_div(rect.y0, kTerrainCell);
  const std::int32_t x1 = floor_div(rect.x1, kTerrainCell);
  const std::int32_t y1 = floor_div(rect.y1, kTerrainCell);
  for (std::int32_t y = y0; y <= y1; ++y) {
    for (std::int32_t x = x0; x <= x1; ++x) {
      if (!in_grid(decor, x, y) || at(decor, x, y) == 0) continue;
      put(decor, x, y, 0);
      cleared.add(x, y);
    }
  }
  return cleared;
}

// --------------------------------------------------------------------------
// undo
// --------------------------------------------------------------------------

namespace {

/// The cells of `grid` a world rectangle touches, inclusive.
[[nodiscard]] CellRect cells_under(const Grid& grid, const WorldRect& rect) noexcept {
  CellRect cells;
  if (grid.cell_size() == 0 || rect.empty()) return cells;
  const auto size = static_cast<std::int32_t>(grid.cell_size());
  const std::int32_t x0 = std::max(0, floor_div(rect.x0, size));
  const std::int32_t y0 = std::max(0, floor_div(rect.y0, size));
  const std::int32_t x1 = std::min(static_cast<std::int32_t>(grid.width()) - 1, floor_div(rect.x1, size));
  const std::int32_t y1 = std::min(static_cast<std::int32_t>(grid.height()) - 1, floor_div(rect.y1, size));
  if (x1 < x0 || y1 < y0) return cells;
  cells.x0 = x0;
  cells.y0 = y0;
  cells.x1 = x1;
  cells.y1 = y1;
  return cells;
}

void copy_out(const Grid* grid, const WorldRect& rect, std::vector<std::uint32_t>& out) {
  out.clear();
  if (grid == nullptr) return;
  const CellRect cells = cells_under(*grid, rect);
  if (cells.empty()) return;
  out.reserve(static_cast<std::size_t>(cells.x1 - cells.x0 + 1) * static_cast<std::size_t>(cells.y1 - cells.y0 + 1));
  for (std::int32_t y = cells.y0; y <= cells.y1; ++y) {
    for (std::int32_t x = cells.x0; x <= cells.x1; ++x) out.push_back(at(*grid, x, y));
  }
}

void copy_in(Grid* grid, const WorldRect& rect, const std::vector<std::uint32_t>& values) {
  if (grid == nullptr || !grid->writable() || values.empty()) return;
  const CellRect cells = cells_under(*grid, rect);
  if (cells.empty()) return;
  std::size_t i = 0;
  for (std::int32_t y = cells.y0; y <= cells.y1; ++y) {
    for (std::int32_t x = cells.x0; x <= cells.x1; ++x) {
      if (i >= values.size()) return;
      put(*grid, x, y, values[i++]);
    }
  }
}

}  // namespace

std::size_t UndoStack::Record::size() const noexcept {
  return (terrain.size() + height.size() + decor.size() + pass.size()) * sizeof(std::uint32_t);
}

UndoStack::Record UndoStack::take(const Layers& layers, const WorldRect& rect, std::uint32_t flags) {
  Record record;
  record.rect = rect;
  record.flags = flags;
  if ((flags & kTerrain) != 0) copy_out(layers.terrain, rect, record.terrain);
  if ((flags & kHeight) != 0) copy_out(layers.height, rect, record.height);
  if ((flags & kDecor) != 0) copy_out(layers.decor, rect, record.decor);
  if ((flags & kPass) != 0) copy_out(layers.pass, rect, record.pass);
  return record;
}

void UndoStack::put_back(const Layers& layers, const Record& record) {
  copy_in(layers.terrain, record.rect, record.terrain);
  copy_in(layers.height, record.rect, record.height);
  copy_in(layers.decor, record.rect, record.decor);
  copy_in(layers.pass, record.rect, record.pass);
}

void UndoStack::snapshot(const Layers& layers, const WorldRect& rect, std::uint32_t flags,
                         bool new_generation) {
  if (rect.empty()) return;
  // A record clamped to the map (0x00499630) -- the layers' own extents
  // stand in for the map's here, which is the same rectangle.
  WorldRect clamped = rect;
  clamped.x0 = std::max(0, clamped.x0);
  clamped.y0 = std::max(0, clamped.y0);
  redo_.clear();
  Record record = take(layers, clamped, flags);
  record.generation = undo_.empty() ? 0 : undo_.back().generation;
  if (new_generation) ++record.generation;
  bytes_ += record.size();
  undo_.push_back(std::move(record));
  // Oldest first past the cap, whatever generation they belong to.
  std::size_t drop = 0;
  while (bytes_ > kByteCap && drop < undo_.size() - 1) bytes_ -= undo_[drop++].size();
  if (drop > 0) undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(drop));
}

WorldRect UndoStack::swap_generation(const Layers& layers, std::vector<Record>& from,
                                     std::vector<Record>& to) {
  WorldRect touched;
  if (from.empty()) return touched;
  const std::uint32_t generation = from.back().generation;
  while (!from.empty() && from.back().generation == generation) {
    Record record = std::move(from.back());
    from.pop_back();
    Record now = take(layers, record.rect, record.flags);
    now.generation = record.generation;
    put_back(layers, record);
    if (touched.empty()) {
      touched = record.rect;
    } else {
      touched.x0 = std::min(touched.x0, record.rect.x0);
      touched.y0 = std::min(touched.y0, record.rect.y0);
      touched.x1 = std::max(touched.x1, record.rect.x1);
      touched.y1 = std::max(touched.y1, record.rect.y1);
    }
    to.push_back(std::move(now));
  }
  // The records changed sides, not size.
  return touched;
}

WorldRect UndoStack::undo(const Layers& layers) {
  // The redo list receives the generation in the order the undo list
  // gave it up, so a redo takes it back in reverse -- last written, first
  // restored -- which is what makes overlapping steps land right.
  return swap_generation(layers, undo_, redo_);
}

WorldRect UndoStack::redo(const Layers& layers) { return swap_generation(layers, redo_, undo_); }

void UndoStack::clear() {
  undo_.clear();
  redo_.clear();
  bytes_ = 0;
}

WorldRect undo_rect(sim::Point corner, std::int32_t half) noexcept {
  return WorldRect{corner.x - half, corner.y - half, corner.x + half, corner.y + half};
}

sim::Point decor_position(std::int32_t cx, std::int32_t cy, std::uint32_t cell) noexcept {
  const auto nx = static_cast<std::int32_t>((cell >> 8) & 0xf);
  const auto ny = static_cast<std::int32_t>((cell >> 12) & 0xf);
  return sim::Point{cx * kTerrainCell + nx * 4, cy * kTerrainCell + ny * 4};
}

WorldRect footprint_rect(const PassMask& mask, sim::Point at) {
  WorldRect box;
  if (mask.empty()) return box;
  box.x0 = at.x + mask.bounds.x0 * kPassCell - kMaskCentreX;
  box.x1 = at.x + mask.bounds.x1 * kPassCell - kMaskCentreX;
  box.y0 = at.y + kMaskCentreY - mask.bounds.y1 * kPassCell;
  box.y1 = at.y + kMaskCentreY - mask.bounds.y0 * kPassCell;
  return box;
}

void stamp_footprint(Grid& pass, const PassMask& footprint, sim::Point at, const Grid& height,
                     const WorldRect& map, const WorldRect& limit) {
  if (!pass.writable() || pass.bits_per_cell() != 1 || footprint.empty()) return;
  const Grid& mask = *footprint.grid;
  if (mask.bits_per_cell() != 1 || mask.cell_size() != static_cast<std::uint32_t>(kPassCell)) return;
  if (map.empty() || limit.empty()) return;
  const auto height_at = [&](sim::Point p) { return height_under(height, map, p); };
  // The anchor's screen row, and the screen rectangle a stamp is held to.
  const std::int32_t anchor_row = sim::project_scale(at.y) - height_at(at);
  const std::int32_t screen_x0 = map.x0;
  const std::int32_t screen_x1 = map.x1 - 32;
  const std::int32_t screen_y0 = sim::project_scale(map.y0);
  const std::int32_t screen_y1 = sim::project_scale(map.y1) - 255 - 32;
  // The blob's box only, as the original walks it (grown by a cell of
  // clear cells, which changes nothing).
  const auto col0 = static_cast<std::uint32_t>(footprint.bounds.x0);
  const auto col1 = static_cast<std::uint32_t>(footprint.bounds.x1);
  const auto row0 = static_cast<std::uint32_t>(footprint.bounds.y0);
  const auto row1 = static_cast<std::uint32_t>(footprint.bounds.y1);
  for (std::uint32_t col = col0; col <= col1 && col < mask.width(); ++col) {
    const std::int32_t mx = static_cast<std::int32_t>(col) * kPassCell;
    bool have_previous = false;
    std::int32_t previous_y = 0;
    for (std::uint32_t row = row0; row <= row1 && row < mask.height(); ++row) {
      if (!mask.blocked(col, row)) {
        have_previous = false;
        continue;
      }
      const std::int32_t my = static_cast<std::int32_t>(row) * kPassCell;
      sim::Point screen{at.x + mx - kMaskCentreX,
                        anchor_row + sim::project_scale(kMaskCentreY - my)};
      screen.x = std::clamp(screen.x, screen_x0, screen_x1);
      screen.y = std::clamp(screen.y, screen_y0, screen_y1);
      const sim::Point world = sim::screen_to_world_over(screen, height_at);
      if (!limit.contains(world)) continue;
      const std::int32_t cx = floor_div(world.x, kPassCell);
      const std::int32_t cy = floor_div(world.y, kPassCell);
      pass_put(pass, cx, cy, true);
      // The column between this cell and the last one stamped, where the
      // slope pulled them apart (0x00546cc0).
      if (have_previous && world.y <= previous_y) {
        for (std::int32_t y = world.y; y <= previous_y; y += kPassCell) {
          pass_put(pass, cx, floor_div(y, kPassCell), true);
        }
      }
      have_previous = true;
      previous_y = world.y;
    }
  }
}

void bake_terrain_passability(Grid& pass, const Grid& terrain, const TerrainTable& table,
                              const WorldRect& rect, const WorldRect& map) {
  if (!pass.writable() || pass.bits_per_cell() != 1 || terrain.cell_size() == 0) return;
  const WorldRect r = rect.clamped(map);
  if (r.empty()) return;
  const std::int32_t width = map.x1 - map.x0 + 1;
  const std::int32_t height = map.y1 - map.y0 + 1;
  // The eight neighbours, in the order 0x00547090 tries them.
  static constexpr sim::Point kAround[8] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                            {-1, 1}, {-1, -1}, {1, -1}, {1, 1}};
  for (std::int32_t x = r.x0; x <= r.x1; x += kPassCell) {
    if (x + 32 >= width) break;
    for (std::int32_t y = r.y0; y <= r.y1; y += kPassCell) {
      if (y + 32 >= height) break;
      const std::int32_t z = terrain_at(terrain, x + 32, y + 32);
      const TerrainLayerDef* layer = z >= 0 ? table.layer(z) : nullptr;
      const std::int32_t cx = floor_div(x, kPassCell);
      const std::int32_t cy = floor_div(y, kPassCell);
      const bool impassable = layer != nullptr && !layer->passable;
      pass_put(pass, cx, cy, impassable);
      if (layer != nullptr && layer->passable_water) {
        pass_put(pass, cx, cy, water_rim_blocked(terrain, map, x, y, kDeepWater));
        continue;
      }
      if (impassable) continue;
      bool shore = false;
      for (const sim::Point d : kAround) {
        const std::int32_t nx = x + d.x * kPassCell;
        const std::int32_t ny = y + d.y * kPassCell;
        if (nx < 0 || ny < 0 || nx >= map.x1 - 33 || ny >= map.y1 - 33) continue;
        if (terrain_at(terrain, nx + 32, ny + 32) == kDeepWater) {
          shore = true;
          break;
        }
      }
      pass_put(pass, cx, cy, shore);
    }
  }
}

void frame_passability(Grid& pass, const Grid& height, const WorldRect& map) {
  if (!pass.writable() || pass.bits_per_cell() != 1 || map.empty()) return;
  const auto extent_x = static_cast<std::int32_t>(pass.extent_x());
  const auto extent_y = static_cast<std::int32_t>(pass.extent_y());
  const auto width = static_cast<std::int32_t>(pass.width());
  const auto height_cells = static_cast<std::int32_t>(pass.height());
  // The two columns at either side, on every row.
  for (std::int32_t cy = 0; cy < height_cells; ++cy) {
    pass_put(pass, 0, cy, true);
    pass_put(pass, 1, cy, true);
    pass_put(pass, width - 1, cy, true);
    pass_put(pass, width - 2, cy, true);
  }
  const auto height_at = [&](sim::Point p) { return height_under(height, map, p); };
  const std::int32_t top_row = sim::project_scale(map.y0);
  const std::int32_t bottom_row = sim::project_scale(map.y1) - 255 - 32;
  for (std::int32_t x = 0; x < extent_x; x += kPassCell) {
    const std::int32_t cx = x / kPassCell;
    // The top band: to 48 past the edge's world row, rounded up to 16 mod 32.
    std::int32_t top = sim::screen_to_world_over(sim::Point{x, top_row}, height_at).y;
    top += (-16 - top) & 16;
    for (std::int32_t y = 0; y < top + 48; y += kPassCell) pass_put(pass, cx, y / kPassCell, true);
    // The bottom band: from 32 above the strip's world row, rounded down to 32.
    std::int32_t bottom = sim::screen_to_world_over(sim::Point{x, bottom_row}, height_at).y;
    bottom -= bottom & 16;
    for (std::int32_t y = bottom - 32; y < extent_y; y += kPassCell) {
      if (y >= 0) pass_put(pass, cx, y / kPassCell, true);
    }
  }
}

void rebuild_passability(Grid& pass, const WorldRect& rect, const Grid& terrain,
                         const TerrainTable& table, const Grid& height,
                         std::span<const Footprint> objects, const Grid& decor,
                         std::span<const PassMask* const> decor_masks) {
  if (!pass.writable() || pass.bits_per_cell() != 1 || terrain.cell_size() == 0) return;
  const WorldRect map = WorldRect::of_map(terrain);
  const WorldRect r = rect.clamped(map);
  if (r.empty()) return;
  // 1. Every cell in the rectangle cleared.
  for (std::int32_t x = r.x0; x <= r.x1; x += kPassCell) {
    for (std::int32_t y = r.y0; y <= r.y1; y += kPassCell) {
      pass_put(pass, floor_div(x, kPassCell), floor_div(y, kPassCell), false);
    }
  }
  // 2. The terrain's own bits.
  bake_terrain_passability(pass, terrain, table, r, map);
  // 3. The objects standing within reach, stamped into the rectangle.
  const WorldRect reach = r.grown(kStampReach).clamped(map);
  for (const Footprint& footprint : objects) {
    if (footprint.mask == nullptr || !reach.contains(footprint.at)) continue;
    stamp_footprint(pass, *footprint.mask, footprint.at, height, map, r);
  }
  // 4. The decorations whose cells lie within reach, stamped against the
  // whole map (0x00547700 hands the decoration stamp the map's rectangle,
  // not the rebuild's).
  if (decor.cell_size() == static_cast<std::uint32_t>(kTerrainCell)) {
    const std::int32_t x0 = floor_div(reach.x0, kTerrainCell);
    const std::int32_t y0 = floor_div(reach.y0, kTerrainCell);
    for (std::int32_t x = x0 * kTerrainCell; x <= reach.x1; x += kTerrainCell) {
      for (std::int32_t y = y0 * kTerrainCell; y <= reach.y1; y += kTerrainCell) {
        const std::int32_t cx = x / kTerrainCell;
        const std::int32_t cy = y / kTerrainCell;
        if (!in_grid(decor, cx, cy)) continue;
        const std::uint32_t cell = at(decor, cx, cy);
        if (cell == 0) continue;
        const std::size_t kind = cell & 0xff;
        if (kind >= decor_masks.size() || decor_masks[kind] == nullptr) continue;
        stamp_footprint(pass, *decor_masks[kind], decor_position(cx, cy, cell), height, map, map);
      }
    }
  }
  // 5. The frame, over the whole layer.
  frame_passability(pass, height, map);
}

// --------------------------------------------------------------------------
// height
// --------------------------------------------------------------------------

std::int32_t height_of_level(std::int32_t level) noexcept {
  return std::clamp(std::clamp(level, 0, 100) * 256 / 101, 0, 255);
}

std::int32_t level_of_height(std::int32_t height) noexcept {
  return std::clamp(height, 0, 255) * 101 / 256;
}

std::int32_t height_delta(std::int32_t amount) noexcept {
  amount = std::clamp(amount, -100, 100);
  return std::clamp(((amount + 100) * 256 / 200) / 2 - 64, -64, 64);
}

void HeightStroke::begin(const Grid& heights) {
  width = heights.width();
  height = heights.height();
  before.assign(static_cast<std::size_t>(width) * height, 0);
  written.assign(before.size(), false);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      before[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(heights.cell(x, y));
    }
  }
}

CellRect apply_height(Grid& heights, HeightStroke& stroke, HeightTool tool, std::int32_t cx,
                      std::int32_t cy, std::int32_t radius, std::int32_t value) {
  CellRect changed;
  if (!heights.writable()) return changed;
  if (stroke.width != heights.width() || stroke.height != heights.height()) stroke.begin(heights);
  const std::vector<sim::Point> disc = brush_disc(radius);
  for (const sim::Point d : disc) {
    const std::int32_t x = cx + d.x;
    const std::int32_t y = cy + d.y;
    if (!in_grid(heights, x, y)) continue;
    const std::size_t index = static_cast<std::size_t>(y) * stroke.width + static_cast<std::size_t>(x);
    const auto current = static_cast<std::int32_t>(at(heights, x, y));
    std::int32_t next = current;
    switch (tool) {
      case HeightTool::kSet:
        next = height_of_level(value);
        break;
      case HeightTool::kRaiseLower:
        if (stroke.written[index]) continue;
        stroke.written[index] = true;
        next = std::clamp(static_cast<std::int32_t>(stroke.before[index]) + height_delta(value), 0, 255);
        break;
      case HeightTool::kSmooth: {
        std::int32_t sum = 0;
        std::int32_t n = 0;
        const sim::Point around[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const sim::Point a : around) {
          if (!in_grid(heights, x + a.x, y + a.y)) continue;
          sum += static_cast<std::int32_t>(at(heights, x + a.x, y + a.y));
          ++n;
        }
        next = (64 * current + 26 * sum) / (64 + 26 * n);
        break;
      }
    }
    if (next == current) continue;
    put(heights, x, y, static_cast<std::uint32_t>(next));
    changed.add(x, y);
  }
  return changed;
}

void rebake_light(Grid& light, const Grid& heights, const CellRect& cells) {
  if (cells.empty() || !light.writable() || light.cell_size() != heights.cell_size()) return;
  const auto w = static_cast<std::int32_t>(heights.width());
  const auto h = static_cast<std::int32_t>(heights.height());
  // A cell's light reads its right and lower neighbours, so the cell to the
  // left and above a changed one changes too.
  const std::int32_t x0 = std::max(0, cells.x0 - 1);
  const std::int32_t y0 = std::max(0, cells.y0 - 1);
  const std::int32_t x1 = std::min(w - 1, cells.x1);
  const std::int32_t y1 = std::min(h - 1, cells.y1);
  for (std::int32_t y = y0; y <= y1; ++y) {
    for (std::int32_t x = x0; x <= x1; ++x) {
      std::int32_t value = 16;
      if (x + 1 < w && y + 1 < h) {
        const auto here = static_cast<std::int32_t>(at(heights, x, y));
        const auto right = static_cast<std::int32_t>(at(heights, x + 1, y));
        const auto below = static_cast<std::int32_t>(at(heights, x, y + 1));
        value = std::clamp(here - (right + below) / 2 + 16, 0, 21);
      }
      if (in_grid(light, x, y)) put(light, x, y, static_cast<std::uint32_t>(value));
    }
  }
}

// --------------------------------------------------------------------------
// decorations
// --------------------------------------------------------------------------

CellRect stamp_decor(Grid& decor, sim::Point world, std::int32_t kind) {
  CellRect changed;
  if (!decor.writable() || kind <= 0 || kind > 255) return changed;
  std::int32_t cx = floor_div(world.x, kTerrainCell);
  std::int32_t cy = floor_div(world.y, kTerrainCell);
  // The remainder in four-unit steps, rounded; sixteen carries.
  std::int32_t nx = (world.x - cx * kTerrainCell + kDecorOffsetStep / 2) / kDecorOffsetStep;
  std::int32_t ny = (world.y - cy * kTerrainCell + kDecorOffsetStep / 2) / kDecorOffsetStep;
  if (nx >= 16) {
    nx = 0;
    ++cx;
  }
  if (ny >= 16) {
    ny = 0;
    ++cy;
  }
  if (!in_grid(decor, cx, cy)) return changed;
  const std::uint32_t word = static_cast<std::uint32_t>(kind) | (static_cast<std::uint32_t>(nx) << 8) |
                             (static_cast<std::uint32_t>(ny) << 12);
  put(decor, cx, cy, word);
  changed.add(cx, cy);
  return changed;
}

CellRect scatter_decor(Grid& decor, std::int32_t cx, std::int32_t cy, std::int32_t radius,
                       std::span<const std::int32_t> kinds, std::int32_t density, sim::Rng& rng) {
  CellRect changed;
  if (!decor.writable() || kinds.empty()) return changed;
  density = std::clamp(density, 0, 100);
  for (const sim::Point d : brush_disc(radius)) {
    const std::int32_t x = cx + d.x;
    const std::int32_t y = cy + d.y;
    if (!in_grid(decor, x, y)) continue;
    if (rng.between(0, 100) > density) continue;
    if (at(decor, x, y) != 0) continue;
    const std::uint32_t offsets = static_cast<std::uint32_t>(rng.between(0, 255)) << 8;
    const std::int32_t kind = kinds[static_cast<std::size_t>(rng.between(0, static_cast<std::int32_t>(kinds.size()) - 1))];
    if (kind <= 0 || kind > 255) continue;
    put(decor, x, y, static_cast<std::uint32_t>(kind) | offsets);
    changed.add(x, y);
  }
  return changed;
}

CellRect clear_decor(Grid& decor, std::int32_t cx, std::int32_t cy, std::int32_t radius) {
  CellRect changed;
  if (!decor.writable()) return changed;
  for (const sim::Point d : brush_disc(radius)) {
    const std::int32_t x = cx + d.x;
    const std::int32_t y = cy + d.y;
    if (!in_grid(decor, x, y) || at(decor, x, y) == 0) continue;
    put(decor, x, y, 0);
    changed.add(x, y);
  }
  return changed;
}

}  // namespace imperivm::core::edit
