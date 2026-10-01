// The fog manager's light grid. See include/imperivm/core/sim/fog_light.hpp.

#include "imperivm/core/sim/fog_light.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

namespace {

[[nodiscard]] std::int32_t parse_sight(std::string_view text) noexcept {
  std::int32_t value = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  while (begin < end && *begin == ' ') ++begin;
  std::from_chars(begin, end, value);
  return value;
}

/// `vtbl[0x50]`, IsDead, as this engine reads it elsewhere: a unit or a
/// building at no health. A decor has no health to lose.
[[nodiscard]] bool alive(const WorldObject& object) noexcept {
  if (!object.state.flags.is_unit && !object.state.flags.is_building) return true;
  return object.state.health > 0;
}

}  // namespace

void FogLight::resize(std::int32_t map_size) {
  counter_ = 0;
  shown_ = Cells{};
  if (map_size <= 0) {
    columns_ = rows_ = 0;
    displayed_.clear();
    target_.clear();
    delta_.clear();
    return;
  }
  // `stride = trunc((x1 - x0 + 1) / 16) + 1` (0x00607bf3): one more than the
  // cells, so the far edge has a corner to sample.
  columns_ = rows_ = map_size / kCell + 1;
  const std::size_t count = static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_);
  displayed_.assign(count, 0);
  target_.assign(count, 0);
  delta_.assign(count, 0);
}

FogLight::Cells FogLight::clip(Rect rect) const noexcept {
  Cells out;
  if (empty() || rect.empty() || rect.x1 < 0 || rect.y1 < 0) return out;
  out.x0 = std::max<std::int32_t>(0, rect.x0) / kCell;
  out.y0 = std::max<std::int32_t>(0, rect.y0) / kCell;
  out.x1 = std::min<std::int32_t>(columns_ - 1, rect.x1 / kCell);
  out.y1 = std::min<std::int32_t>(rows_ - 1, rect.y1 / kCell);
  return out;
}

FogLight::Rect FogLight::grown(Rect rect) const noexcept {
  return Rect{rect.x0 - kGrow, rect.y0 - kGrow, rect.x1 + kGrow, rect.y1 + kGrow};
}

std::int32_t FogLight::period_ms(std::int32_t speed) noexcept {
  if (speed <= 0) return 100;
  return std::min<std::int32_t>(100, 100000 / speed);
}

// --------------------------------------------------------------------------
// the rebuild
// --------------------------------------------------------------------------

void FogLight::rebuild(const World& world, const ExplorationMap* map, PlayerId local, Setup setup,
                       Rect rect, bool snap) {
  const Cells cells = clip(grown(rect));
  if (cells.empty()) return;
  rebuild_cells(world, map, local, setup, cells, snap);
  shown_ = cells;
}

void FogLight::show(const World& world, const ExplorationMap* map, PlayerId local, Setup setup,
                    Rect rect) {
  const Cells cells = clip(grown(rect));
  if (cells.empty()) return;
  if (shown_.empty()) {
    rebuild_cells(world, map, local, setup, cells, /*snap=*/true);
    shown_ = cells;
    return;
  }
  // 0x00604700: the four strips the move exposes -- the full-height strips
  // on the left and right, and what is left above and below between them.
  const Cells& old = shown_;
  Cells strips[4];
  strips[0] = Cells{cells.x0, cells.y0, std::min(cells.x1, old.x0 - 1), cells.y1};
  strips[1] = Cells{std::max(cells.x0, old.x1 + 1), cells.y0, cells.x1, cells.y1};
  const std::int32_t mid_x0 = std::max(cells.x0, old.x0);
  const std::int32_t mid_x1 = std::min(cells.x1, old.x1);
  strips[2] = Cells{mid_x0, cells.y0, mid_x1, std::min(cells.y1, old.y0 - 1)};
  strips[3] = Cells{mid_x0, std::max(cells.y0, old.y1 + 1), mid_x1, cells.y1};
  for (const Cells& strip : strips) {
    if (strip.empty()) continue;
    rebuild_cells(world, map, local, setup, strip, /*snap=*/true);
  }
  shown_ = cells;
}

bool FogLight::tick(const World& world, const ExplorationMap* map, PlayerId local, Setup setup,
                    Rect rect) {
  if (empty()) return false;
  bool rebuilt = false;
  if (++counter_ >= kTicksPerRebuild) {
    counter_ = 0;
    rebuild(world, map, local, setup, rect, /*snap=*/false);
    rebuilt = true;
  }
  for (std::size_t i = 0; i < displayed_.size(); ++i) {
    displayed_[i] = static_cast<std::uint16_t>(displayed_[i] + delta_[i]);
  }
  return rebuilt;
}

void FogLight::rebuild_cells(const World& world, const ExplorationMap* map, PlayerId local,
                             Setup setup, Cells cells, bool snap) {
  // With both switches off the rebuild returns without touching the grid.
  if (!setup.fog_of_war && !setup.exploration) return;
  // The floor over the rect, or the wholesale light with fog of war off.
  const std::uint16_t fill = setup.fog_of_war ? kFloor : kLitCoarse;
  for (std::int32_t cy = cells.y0; cy <= cells.y1; ++cy) {
    std::fill_n(target_.begin() + static_cast<std::ptrdiff_t>(index(cells.x0, cy)),
                cells.x1 - cells.x0 + 1, fill);
  }
  if (setup.fog_of_war) light_objects(world, local, cells);
  // A player index above 8 does nothing at 0x00516f80 -- and `IsExplored`
  // answers true there, which is the same statement.
  if (setup.exploration && map != nullptr && local < ExplorationMap::kSlots) {
    cap_exploration(*map, static_cast<std::int32_t>(local), cells);
  }
  // The tail, 0x0060747f: snap, or the delta toward the target over four
  // ticks, truncated toward zero.
  for (std::int32_t cy = cells.y0; cy <= cells.y1; ++cy) {
    for (std::int32_t cx = cells.x0; cx <= cells.x1; ++cx) {
      const std::size_t i = index(cx, cy);
      if (snap) {
        displayed_[i] = target_[i];
        delta_[i] = 0;
      } else {
        const std::int32_t gap = static_cast<std::int32_t>(target_[i]) -
                                 static_cast<std::int32_t>(displayed_[i]);
        delta_[i] = static_cast<std::int16_t>(gap / kTicksPerRebuild);
      }
    }
  }
}

void FogLight::light_objects(const World& world, PlayerId local, Cells cells) {
  struct Source {
    Point at;
    std::int32_t sight = 0;
    std::int32_t class_sight = 0;
    std::int32_t coarse = -1;
  };
  const std::int32_t wx0 = cells.x0 * kCell;
  const std::int32_t wy0 = cells.y0 * kCell;
  const std::int32_t wx1 = (cells.x1 + 1) * kCell - 1;
  const std::int32_t wy1 = (cells.y1 + 1) * kCell - 1;
  const ClassGraph* graph = world.class_graph();

  // The visitor's tests (0x00605cc0, 0x00606a70), in its order: on the local
  // side, spawned, a world object, alive, a sight. A held object stands
  // nowhere the collector looks.
  std::vector<Source> sources;
  for (const WorldObject& object : world.objects()) {
    if (object.internal != InternalKind::none) continue;
    if (object.state.owner != local &&
        !world.players().has(local, object.state.owner, Relation::share_view)) {
      continue;
    }
    if (object.state.flags.unspawned || object.state.is_held()) continue;
    if (!alive(object)) continue;
    if (object.sight == 0) continue;
    const Point at = object.state.position;
    if (at.x < wx0 || at.x > wx1 || at.y < wy0 || at.y > wy1) continue;
    Source source;
    source.at = at;
    source.sight = object.sight;
    source.class_sight = graph != nullptr && object.class_index != kNoClass
                             ? parse_sight(graph->property(object.class_index, "sight"))
                             : 0;
    sources.push_back(source);
  }
  if (sources.empty()) return;

  // The coarse pass, 0x00606de0: a scratch grid of 256-cells over the rect.
  const std::int32_t kx0 = wx0 / kCoarseCell;
  const std::int32_t ky0 = wy0 / kCoarseCell;
  const std::int32_t kx1 = wx1 / kCoarseCell;
  const std::int32_t ky1 = wy1 / kCoarseCell;
  const std::int32_t kw = kx1 - kx0 + 1;
  const std::int32_t kh = ky1 - ky0 + 1;
  std::vector<std::uint8_t> marks(static_cast<std::size_t>(kw) * static_cast<std::size_t>(kh), 0);
  std::vector<std::int32_t> seeds;
  for (Source& source : sources) {
    const std::int32_t kx = source.at.x / kCoarseCell - kx0;
    const std::int32_t ky = source.at.y / kCoarseCell - ky0;
    source.coarse = ky * kw + kx;
    if (source.class_sight <= kCoarseSight) continue;
    std::uint8_t& mark = marks[static_cast<std::size_t>(source.coarse)];
    if (mark == 0) {
      mark = 1;
      seeds.push_back(source.coarse);
    }
  }
  for (const std::int32_t seed : seeds) {
    const std::int32_t kx = seed % kw;
    const std::int32_t ky = seed / kw;
    // 0x00605960: interior iff every neighbour inside the rect is marked.
    bool interior = true;
    for (std::int32_t dy = -1; dy <= 1 && interior; ++dy) {
      for (std::int32_t dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        const std::int32_t nx = kx + dx;
        const std::int32_t ny = ky + dy;
        if (nx < 0 || ny < 0 || nx >= kw || ny >= kh) continue;
        if (marks[static_cast<std::size_t>(ny * kw + nx)] == 0) {
          interior = false;
          break;
        }
      }
    }
    if (!interior) continue;
    marks[static_cast<std::size_t>(seed)] = 2;
    // The 256-aligned square, clipped to the rect, lit wholesale.
    const std::int32_t cx0 = std::max(cells.x0, (kx + kx0) * (kCoarseCell / kCell));
    const std::int32_t cy0 = std::max(cells.y0, (ky + ky0) * (kCoarseCell / kCell));
    const std::int32_t cx1 = std::min(cells.x1, (kx + kx0 + 1) * (kCoarseCell / kCell) - 1);
    const std::int32_t cy1 = std::min(cells.y1, (ky + ky0 + 1) * (kCoarseCell / kCell) - 1);
    for (std::int32_t cy = cy0; cy <= cy1; ++cy) {
      for (std::int32_t cx = cx0; cx <= cx1; ++cx) target_[index(cx, cy)] = kLitCoarse;
    }
  }

  // The precise pass, 0x006056d0, over the objects not standing in an
  // interior cell.
  for (const Source& source : sources) {
    if (marks[static_cast<std::size_t>(source.coarse)] == 2) continue;
    const std::int32_t s = source.sight;
    const std::int32_t r_cells = s / kCell + 1;
    const std::int32_t cx = source.at.x / kCell;
    const std::int32_t cy = source.at.y / kCell;
    const std::int32_t bx0 = std::max(cells.x0, cx - r_cells);
    const std::int32_t by0 = std::max(cells.y0, cy - r_cells);
    const std::int32_t bx1 = std::min(cells.x1, cx + r_cells);
    const std::int32_t by1 = std::min(cells.y1, cy + r_cells);
    // A sight under the ramp's width would put the inner radius past the
    // outer; the shipped sights start at 100 and the case is closed at zero.
    // The original multiplies by `7680 / (outer2 - inner2)` in floats; the
    // integer quotient here can differ from that by one word unit, on a
    // grid that is drawn and never hashed.
    const std::int64_t inner = std::max(s - kSightRamp, 0);
    const std::int64_t outer2 = static_cast<std::int64_t>(s) * s;
    const std::int64_t inner2 = inner * inner;
    const std::int64_t span = outer2 - inner2;
    for (std::int32_t y = by0; y <= by1; ++y) {
      for (std::int32_t x = bx0; x <= bx1; ++x) {
        std::uint16_t& cur = target_[index(x, y)];
        if (cur == kLitFull) continue;
        // The cell's top-left corner, not its centre.
        const std::int64_t dx = x * kCell - source.at.x;
        const std::int64_t dy = y * kCell - source.at.y;
        const std::int64_t d2 = dx * dx + dy * dy;
        std::uint16_t value = kFloor;
        if (d2 <= inner2) {
          value = kLitFull;
        } else if (d2 <= outer2 && span > 0) {
          value = static_cast<std::uint16_t>(kFloor + (outer2 - d2) * 7680 / span);
        }
        if (value > cur) cur = value;
      }
    }
  }
}

void FogLight::cap_exploration(const ExplorationMap& map, std::int32_t slot, Cells cells) {
  using State = ExplorationMap::State;
  constexpr std::int32_t kPerCell = ExplorationMap::kCellSize / kCell;   // 64
  constexpr std::int32_t kPerFine = ExplorationMap::kFineSpacing / kCell;  // 2
  constexpr std::int32_t kFine = ExplorationMap::kFineSide;                // 32
  const std::int32_t ex0 = cells.x0 / kPerCell;
  const std::int32_t ey0 = cells.y0 / kPerCell;
  const std::int32_t ex1 = cells.x1 / kPerCell;
  const std::int32_t ey1 = cells.y1 / kPerCell;
  for (std::int32_t ey = ey0; ey <= ey1; ++ey) {
    for (std::int32_t ex = ex0; ex <= ex1; ++ex) {
      const State state = map.state(ex, ey, slot);
      if (state == State::full || state == State::unwritten) continue;
      const std::int32_t cx0 = std::max(cells.x0, ex * kPerCell);
      const std::int32_t cy0 = std::max(cells.y0, ey * kPerCell);
      const std::int32_t cx1 = std::min(cells.x1, (ex + 1) * kPerCell - 1);
      const std::int32_t cy1 = std::min(cells.y1, (ey + 1) * kPerCell - 1);
      if (state == State::never) {
        // 0x005145a0: the black.
        for (std::int32_t cy = cy0; cy <= cy1; ++cy) {
          std::fill_n(target_.begin() + static_cast<std::ptrdiff_t>(index(cx0, cy)),
                      cx1 - cx0 + 1, static_cast<std::uint16_t>(0));
        }
        continue;
      }
      // 0x00516ab0: the record's nibbles, the right, lower and diagonal
      // neighbours providing the far samples -- a never-seen neighbour reads
      // 0, a full one 15, a partial one its own record -- blended at the
      // 16-cell's offset inside its 32-unit sub-cell, and the grid capped at
      // `nibble * 1024`. The four samples are fetched once per neighbour cell
      // rather than once per 16-cell, which is a cost and not a reading.
      const ExplorationMap::FineRecord* records[2][2];
      std::int32_t flats[2][2];
      for (std::int32_t j = 0; j < 2; ++j) {
        for (std::int32_t i = 0; i < 2; ++i) {
          const State neighbour = map.state(ex + i, ey + j, slot);
          records[j][i] = neighbour == State::partial ? map.fine(ex + i, ey + j, slot) : nullptr;
          flats[j][i] = neighbour == State::never ? 0 : ExplorationMap::kFineMax;
        }
      }
      const auto at = [&](std::int32_t fx, std::int32_t fy) -> std::int32_t {
        const std::int32_t i = fx >= kFine ? 1 : 0;
        const std::int32_t j = fy >= kFine ? 1 : 0;
        const ExplorationMap::FineRecord* record = records[j][i];
        if (record == nullptr) return flats[j][i];
        const std::size_t k = static_cast<std::size_t>(fy - j * kFine) * kFine +
                              static_cast<std::size_t>(fx - i * kFine);
        return record->values[k];
      };
      // The blend's fractions are `offset / 2` on both axes, so it is exact
      // in integers over `kPerFine * kPerFine`.
      for (std::int32_t cy = cy0; cy <= cy1; ++cy) {
        const std::int32_t ly = cy - ey * kPerCell;
        const std::int32_t fy = ly / kPerFine;
        const std::int32_t ay = ly % kPerFine;
        for (std::int32_t cx = cx0; cx <= cx1; ++cx) {
          const std::int32_t lx = cx - ex * kPerCell;
          const std::int32_t fx = lx / kPerFine;
          const std::int32_t ax = lx % kPerFine;
          const std::int32_t blend = at(fx, fy) * (kPerFine - ax) * (kPerFine - ay) +
                                     at(fx + 1, fy) * ax * (kPerFine - ay) +
                                     at(fx, fy + 1) * (kPerFine - ax) * ay +
                                     at(fx + 1, fy + 1) * ax * ay;
          const auto cap = static_cast<std::uint16_t>(blend * ExplorationMap::kCellSize /
                                                      (kPerFine * kPerFine));
          std::uint16_t& word = target_[index(cx, cy)];
          if (cap < word) word = cap;
        }
      }
    }
  }
}

// --------------------------------------------------------------------------
// the readers
// --------------------------------------------------------------------------

std::uint16_t FogLight::word_at(Point at) const noexcept {
  if (empty() || at.x < 0 || at.y < 0) return 0;
  const std::int32_t cx = at.x / kCell;
  const std::int32_t cy = at.y / kCell;
  if (cx >= columns_ || cy >= rows_) return 0;
  return displayed_[index(cx, cy)];
}

std::uint16_t FogLight::target_at(Point at) const noexcept {
  if (empty() || at.x < 0 || at.y < 0) return 0;
  const std::int32_t cx = at.x / kCell;
  const std::int32_t cy = at.y / kCell;
  if (cx >= columns_ || cy >= rows_) return 0;
  return target_[index(cx, cy)];
}

std::uint16_t FogLight::sample(Point at) const noexcept {
  if (empty() || at.x < 0 || at.y < 0) return 0;
  const std::int32_t cx = at.x / kCell;
  const std::int32_t cy = at.y / kCell;
  if (cx >= columns_ || cy >= rows_) return 0;
  const std::int32_t nx = std::min(cx + 1, columns_ - 1);
  const std::int32_t ny = std::min(cy + 1, rows_ - 1);
  const std::int64_t tx = at.x % kCell;
  const std::int64_t ty = at.y % kCell;
  const std::int64_t w00 = displayed_[index(cx, cy)];
  const std::int64_t w10 = displayed_[index(nx, cy)];
  const std::int64_t w01 = displayed_[index(cx, ny)];
  const std::int64_t w11 = displayed_[index(nx, ny)];
  const std::int64_t blend = w00 * (kCell - tx) * (kCell - ty) + w10 * tx * (kCell - ty) +
                             w01 * (kCell - tx) * ty + w11 * tx * ty;
  return static_cast<std::uint16_t>(blend / (kCell * kCell));
}

std::int32_t FogLight::factor_of(std::uint16_t word) noexcept {
  // 0x00604a40: `(w >> 9) + 2 == 32` and `w > 0x3a00` both leave the pixel;
  // `w < 0x400` paints it black; the rest go through the tables at
  // `L = w >> 10`, whose rows scale by `floor(68 L / 31) / 32`.
  if ((word >> 9) + 2 == 32 || word > kLitAbove) return 32;
  if (word < kBlackBelow) return 0;
  const std::int32_t level = word >> 10;
  return 68 * level / 31;
}

bool FogLight::hides(const World& world, const WorldObject& object, PlayerId local,
                     const ExplorationMap* map, Setup setup) const noexcept {
  if (!setup.fog_of_war) return false;
  // 0x00605ff0 tests units, projectiles and "Watersteps"; a building, a decor
  // or anything else is drawn wherever it stands. Projectiles are not world
  // objects here, and "Watersteps" takes 0x00605e10's exemption.
  if (!object.state.flags.is_unit) return false;
  const PlayerId owner = object.state.owner;
  if (owner == local) return false;
  if (PlayerTable::is_valid(owner) && PlayerTable::is_valid(local) &&
      world.players().has(owner, local, Relation::share_view)) {
    return false;
  }
  if (object.state.flags.hidden) return true;
  if (map != nullptr && world.class_graph() != nullptr) {
    const ClassIndex animal = world.class_graph()->find("Animal");
    if (animal != kNoClass && world.class_is_a(object.id, animal) &&
        map->explored(object.state.position, static_cast<std::int32_t>(local))) {
      return false;
    }
  }
  // The light test: off the grid is hidden, and so is anything under level 24.
  if (empty()) return true;
  return sample(object.state.position) < kHideBelow;
}

}  // namespace imperivm::core::sim
