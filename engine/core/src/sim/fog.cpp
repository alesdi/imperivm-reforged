// The explored map. See include/imperivm/core/sim/fog.hpp.

#include "imperivm/core/sim/fog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/spatial_index.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

// --------------------------------------------------------------------------
// the map
// --------------------------------------------------------------------------

void ExplorationMap::resize(std::int32_t extent) {
  // A fresh map, as the constructor (0x005179d0) makes one: the flag down.
  everywhere_ = false;
  for (auto& records : fine_) records.clear();
  if (extent <= 0) {
    side_ = 0;
    cells_.clear();
    return;
  }
  // Round up: a map whose extent is not a multiple of the cell size still has a
  // partial cell on each far edge, and `x >> 10` will index it.
  side_ = (extent + kCellSize - 1) / kCellSize;
  cells_.assign(static_cast<std::size_t>(side_) * static_cast<std::size_t>(side_), 0u);
}

std::int32_t ExplorationMap::index_of(Point at) const noexcept {
  if (side_ == 0) return -1;
  if (at.x < 0 || at.y < 0) return -1;
  const std::int32_t cx = at.x / kCellSize;
  const std::int32_t cy = at.y / kCellSize;
  if (cx >= side_ || cy >= side_) return -1;
  return cy * side_ + cx;
}

ExplorationMap::State ExplorationMap::state_at(std::int32_t index,
                                               std::int32_t slot) const noexcept {
  return static_cast<State>((cells_[static_cast<std::size_t>(index)] >> (2 * slot)) & 3u);
}

void ExplorationMap::set_state(std::int32_t index, std::int32_t slot, State state) noexcept {
  std::uint16_t& cell = cells_[static_cast<std::size_t>(index)];
  cell = static_cast<std::uint16_t>((cell & ~(3u << (2 * slot))) |
                                    (static_cast<unsigned>(state) << (2 * slot)));
}

ExplorationMap::FineRecord& ExplorationMap::record_for(std::int32_t index, std::int32_t slot) {
  return fine_[static_cast<std::size_t>(slot)][index];
}

void ExplorationMap::drop_record(std::int32_t index, std::int32_t slot) {
  fine_[static_cast<std::size_t>(slot)].erase(index);
}

void ExplorationMap::normalise(std::int32_t index, std::int32_t slot) {
  auto& records = fine_[static_cast<std::size_t>(slot)];
  const auto it = records.find(index);
  if (it == records.end()) return;
  for (const std::uint8_t value : it->second.values) {
    if (value != kFineMax) return;
  }
  records.erase(it);
  set_state(index, slot, State::full);
}

bool ExplorationMap::explored(Point at, std::int32_t slot) const noexcept {
  // The missing-manager path first: with no grid nothing is explored, for
  // anybody. 0x004c684f answers false before it ever reaches the slot check.
  if (side_ == 0) return false;
  // Then 0x00515950 itself. Its slot is **unsigned**: anything past the
  // eighth -- and so also the -1 that `IsExplored(pt, 0)` decrements to -- is
  // explored everywhere (0x00515958).
  if (static_cast<std::uint32_t>(slot) >= static_cast<std::uint32_t>(kSlots)) return true;
  const std::int32_t index = index_of(at);
  if (index < 0) return false;
  switch (state_at(index, slot)) {
    case State::never:
      return false;
    case State::full:
      return true;
    case State::partial:
    case State::unwritten:
      break;
  }
  // **A partial cell answers its fine record at the point, not "seen".** For
  // states 1 and 2 0x00515950 walks the slot's record list (`[expl + 0x50 +
  // slot * 12]`) for the record whose rectangle holds the point -- none is
  // false -- and reads the four-bit value of the 32-unit sub-cell the point
  // falls in (0x00514a60: row `dy >> 5`, column `dx >> 5` from the record's
  // origin); explored iff it is not zero. A record here is its cell's, at the
  // cell's origin, so that is `fine_at` at `(x >> 5, y >> 5)`. State 1 is
  // never written and has no record, so it answers false.
  const auto it = fine_[static_cast<std::size_t>(slot)].find(index);
  if (it == fine_[static_cast<std::size_t>(slot)].end()) return false;
  const std::int32_t i = (at.x % kCellSize) / kFineSpacing;
  const std::int32_t j = (at.y % kCellSize) / kFineSpacing;
  return it->second.values[static_cast<std::size_t>(j) * kFineSide + i] != 0;
}

ExplorationMap::State ExplorationMap::state(std::int32_t cx, std::int32_t cy,
                                            std::int32_t slot) const noexcept {
  if (slot < 0 || slot >= kSlots) return State::never;
  if (cx < 0 || cy < 0 || cx >= side_ || cy >= side_) return State::never;
  return state_at(cy * side_ + cx, slot);
}

const ExplorationMap::FineRecord* ExplorationMap::fine(std::int32_t cx, std::int32_t cy,
                                                       std::int32_t slot) const noexcept {
  if (state(cx, cy, slot) != State::partial) return nullptr;
  const auto& records = fine_[static_cast<std::size_t>(slot)];
  const auto it = records.find(cy * side_ + cx);
  return it == records.end() ? nullptr : &it->second;
}

std::int32_t ExplorationMap::fine_at(std::int32_t fx, std::int32_t fy,
                                     std::int32_t slot) const noexcept {
  if (fx < 0 || fy < 0) return 0;
  const std::int32_t cx = fx / kFineSide;
  const std::int32_t cy = fy / kFineSide;
  switch (state(cx, cy, slot)) {
    case State::never:
      return 0;
    case State::full:
    case State::unwritten:
      return kFineMax;
    case State::partial: {
      const FineRecord* record = fine(cx, cy, slot);
      if (record == nullptr) return 0;
      const std::int32_t i = fx - cx * kFineSide;
      const std::int32_t j = fy - cy * kFineSide;
      return record->values[static_cast<std::size_t>(j) * kFineSide + i];
    }
  }
  return 0;
}

void ExplorationMap::explore(Point at, std::int32_t slot) {
  if (slot < 0 || slot >= kSlots) return;
  const std::int32_t index = index_of(at);
  if (index < 0) return;
  drop_record(index, slot);
  set_state(index, slot, State::full);
}

namespace {

/// The integer square root the rim needs, exact and fast. `sim::isqrt` is
/// Newton's method from `value` down and costs thirty divisions; this is one
/// `sqrt` and a correction, and the correction is what makes it the same
/// function: the largest `r` with `r * r <= value`, on every machine. The
/// values here are squared distances under a few thousand units, far inside
/// the range where a double holds them exactly.
[[nodiscard]] std::int64_t rim_isqrt(std::int64_t value) noexcept {
  if (value <= 0) return 0;
  auto r = static_cast<std::int64_t>(std::sqrt(static_cast<double>(value)));  // imperivm: allow-float: a seed only; the two loops below make the result the exact integer root whatever the rounding
  while (r * r > value) --r;
  while ((r + 1) * (r + 1) <= value) ++r;
  return r;
}

}  // namespace

namespace {

// The original's stamps compute in 32-bit registers: sums, differences and
// squares wrap, and the comparisons are signed. These keep that without the
// undefined behaviour a signed overflow is in C++.
[[nodiscard]] constexpr std::int32_t add32(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr std::int32_t sub32(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr std::int32_t mul32(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
/// `cdq / xor / sub`: the absolute value, and `INT_MIN` stays itself.
[[nodiscard]] constexpr std::int32_t abs32(std::int32_t v) noexcept {
  const std::uint32_t sign = v < 0 ? 0xffffffffu : 0u;
  return static_cast<std::int32_t>((static_cast<std::uint32_t>(v) ^ sign) - sign);
}
/// Down to a multiple of 32 the way the stamps do it, toward zero.
[[nodiscard]] constexpr std::int32_t trunc32(std::int32_t v) noexcept { return v / 32 * 32; }

/// The floored root of an unsigned 32-bit value: 0x0067c3d0, which answers
/// `floor(sqrt(v))` for every input (0xffff above 0xfffe0000, which is the
/// same thing).
[[nodiscard]] std::int32_t isqrt32(std::uint32_t value) noexcept {
  std::uint32_t r = 0;
  std::uint32_t bit = 1u << 30;
  while (bit > value) bit >>= 2;
  while (bit != 0) {
    if (value >= r + bit) {
      value -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return static_cast<std::int32_t>(r);
}

/// 0x005146a0: where a cell rectangle (edges included) stands against the
/// circle of radius `rad` about `(px, py)`. **1** when all four corners are
/// within it, **-1** when the two do not meet, **0** when they meet otherwise.
/// The box test first, then the farthest corner, then the nearest corner, and
/// last the two arms of the rounded rectangle the circle's centre must lie in.
[[nodiscard]] std::int32_t classify_cell(std::int32_t x0, std::int32_t y0, std::int32_t x1,
                                         std::int32_t y1, std::int32_t px, std::int32_t py,
                                         std::int32_t rad) noexcept {
  if (sub32(px, rad) > x1 || add32(px, rad) < x0 || sub32(py, rad) > y1 ||
      add32(py, rad) < y0) {
    return -1;
  }
  const std::int32_t ax0 = abs32(sub32(px, x0));
  const std::int32_t ax1 = abs32(sub32(px, x1));
  const std::int32_t ay0 = abs32(sub32(py, y0));
  const std::int32_t ay1 = abs32(sub32(py, y1));
  const std::int32_t near_x = std::min(ax0, ax1);
  const std::int32_t far_x = std::max(ax0, ax1);
  const std::int32_t near_y = std::min(ay0, ay1);
  const std::int32_t far_y = std::max(ay0, ay1);
  const std::int32_t r2 = mul32(rad, rad);
  if (add32(mul32(far_y, far_y), mul32(far_x, far_x)) <= r2) return 1;
  if (add32(mul32(near_y, near_y), mul32(near_x, near_x)) <= r2) return 0;
  if (px >= sub32(x0, rad) && px <= add32(x1, rad) && py >= y0 && py <= y1) return 0;
  if (px >= x0 && px <= x1 && py >= sub32(y0, rad) && py <= add32(y1, rad)) return 0;
  return -1;
}

/// The rim of the large stamp: 0x00514b80, into one cell's record.
///
/// `R` is the radius plus 48. The ramp is evaluated at the four corners of
/// the 32-unit square each lattice point heads -- `0` past `R`, `15` short of
/// `R - 96`, `(R - d) * 15 / 96` between, `d` the floored root of the squared
/// distance -- and blended bilinearly with the weights `(x - o) % 16` and
/// `(y - o) % 16`, `o` the origin of the box `16 * (R / 16 + 2)` either side
/// of the centre that also bounds the lattice. Every division truncates; the
/// value is `max`ed with what is there.
void stamp_rim(ExplorationMap::FineRecord& record, std::int32_t x0, std::int32_t y0,
               Point centre, std::int32_t outer) {
  constexpr std::int32_t kMax = ExplorationMap::kFineMax;
  const std::int32_t x1 = x0 + ExplorationMap::kCellSize - 1;
  const std::int32_t y1 = y0 + ExplorationMap::kCellSize - 1;
  const std::int32_t span = mul32(outer / 16 + 2, 32);
  const std::int32_t ox = sub32(centre.x, span / 2);
  const std::int32_t oy = sub32(centre.y, span / 2);
  const std::int32_t xmin = add32(ox, 16);
  const std::int32_t ymin = add32(oy, 16);
  const std::int32_t xmax = sub32(add32(span, ox), 16);
  const std::int32_t ymax = sub32(add32(span, oy), 16);
  const std::int32_t xs = trunc32(std::max(x0, xmin));
  const std::int32_t ys = trunc32(std::max(y0, ymin));
  const std::int32_t xe = trunc32(std::min(x1, xmax));
  const std::int32_t ye = trunc32(std::min(y1, ymax));
  const auto ramp = [outer](std::int32_t d) -> std::int32_t {
    if (outer < d) return 0;
    if (d < sub32(outer, ExplorationMap::kRamp)) return kMax;
    return mul32(sub32(outer, d), kMax) / ExplorationMap::kRamp;
  };
  const auto dist = [&centre](std::int32_t x, std::int32_t y) -> std::int32_t {
    const std::int32_t dx = sub32(x, centre.x);
    const std::int32_t dy = sub32(y, centre.y);
    return isqrt32(static_cast<std::uint32_t>(add32(mul32(dx, dx), mul32(dy, dy))));
  };
  for (std::int32_t x = xs; x <= xe; x += ExplorationMap::kFineSpacing) {
    for (std::int32_t y = ys; y <= ye; y += ExplorationMap::kFineSpacing) {
      if (x < xmin || x > xmax || y < ymin || y > ymax) continue;
      const std::int32_t v00 = ramp(dist(x, y));
      const std::int32_t v10 = ramp(dist(x + 32, y));
      const std::int32_t v01 = ramp(dist(x, y + 32));
      const std::int32_t v11 = ramp(dist(x + 32, y + 32));
      const std::int32_t fx = sub32(x, ox) % 16;
      const std::int32_t fy = sub32(y, oy) % 16;
      const std::int32_t top = v00 + (v10 - v00) * fx / 16;
      const std::int32_t bottom = v01 + (v11 - v01) * fx / 16;
      const std::int32_t value = top + (bottom - top) * fy / 16;
      const auto i = static_cast<std::size_t>(static_cast<std::uint32_t>(x - x0) >> 5);
      const auto j = static_cast<std::size_t>(static_cast<std::uint32_t>(y - y0) >> 5);
      std::uint8_t& nibble = record.values[j * ExplorationMap::kFineSide + i];
      if (nibble < value) nibble = static_cast<std::uint8_t>(value);
    }
  }
}

}  // namespace

const std::vector<std::int32_t>& ExplorationMap::brush(std::int32_t n) const {
  // 0x005f8810 on the table object at `map + 0xe4` (0x005f8780(96): 130
  // slots, width 96). Built on first use for each `n` and kept: its contents
  // are a function of `n` alone, so a cache here writes what the original's
  // does. `(2n + 1)` rows of `(2n + 1)` samples 16 units apart, the centre at
  // `(n, n)`, each `value << 9` with `w = min(96 / 16 + 1, n)`:
  //
  //   * 0 for a sample farther than `n` from the centre,
  //   * 15 for one nearer than `n - w`,
  //   * `trunc((n - d) * 15 / w)` between, `d` its distance.
  //
  // The original takes `d` as a single-precision root and the ramp in the
  // FPU; on the integers it is handed, the comparisons are exact in squares
  // and the ramp is `(15n - ceil(sqrt(225 m))) / w`, `m = a² + b²`, which
  // agrees with single, double and exact evaluation at every sample of every
  // `n` from 2 to 127 (checked off-line, once).
  std::vector<std::int32_t>& table = brush_[static_cast<std::size_t>(n)];
  if (!table.empty()) return table;
  const std::int32_t side = 2 * n + 1;
  const std::int32_t w = std::min<std::int32_t>(kRamp / 16 + 1, n);
  table.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side), 0);
  for (std::int32_t a = -n; a <= n; ++a) {
    for (std::int32_t b = -n; b <= n; ++b) {
      const std::int64_t m = static_cast<std::int64_t>(a) * a + static_cast<std::int64_t>(b) * b;
      std::int32_t value = 0;
      if (m > static_cast<std::int64_t>(n) * n) {
        value = 0;
      } else if (m < static_cast<std::int64_t>(n - w) * (n - w)) {
        value = kFineMax;
      } else {
        const std::int64_t s = m == 0 ? 0 : rim_isqrt(225 * m - 1) + 1;
        value = static_cast<std::int32_t>((15 * static_cast<std::int64_t>(n) - s) / w);
      }
      table[static_cast<std::size_t>((n + a) * side + (n + b))] = value << 9;
    }
  }
  return table;
}

void ExplorationMap::stamp_brush(FineRecord& record, std::int32_t x0, std::int32_t y0,
                                 Point centre, std::int32_t outer) const {
  // 0x00514fc0: the small stamp's lattice writer. The box is the table's,
  // `16 * (R / 16 + 2)` either side of the centre, less 16 at each edge; the
  // lattice points are the cell's multiples of 32 inside it.
  const std::int32_t n = outer / 16 + 2;
  const std::vector<std::int32_t>& table = brush(n);
  const std::int32_t side = 2 * n + 1;
  const std::int32_t span = n * 32;
  const std::int32_t ox = centre.x - span / 2;
  const std::int32_t oy = centre.y - span / 2;
  const std::int32_t xmin = ox + 16;
  const std::int32_t ymin = oy + 16;
  const std::int32_t xmax = span + ox - 16;
  const std::int32_t ymax = span + oy - 16;
  std::int32_t xs = trunc32(std::max(x0, xmin));
  std::int32_t ys = trunc32(std::max(y0, ymin));
  if (xs < xmin) xs += 32;
  if (ys < ymin) ys += 32;
  const std::int32_t xe = trunc32(std::min(x0 + kCellSize - 1, xmax));
  const std::int32_t ye = trunc32(std::min(y0 + kCellSize - 1, ymax));
  for (std::int32_t x = xs; x <= xe; x += kFineSpacing) {
    const std::int32_t tx = x - ox;
    const std::int32_t col = tx / 16;
    const std::int32_t fx = tx % 16;
    for (std::int32_t y = ys; y <= ye; y += kFineSpacing) {
      const std::int32_t ty = y - oy;
      const std::int32_t row = ty / 16;
      const std::int32_t fy = ty % 16;
      const auto at = [&](std::int32_t r, std::int32_t c) {
        return table[static_cast<std::size_t>(r * side + c)];
      };
      const std::int32_t t00 = at(row, col);
      const std::int32_t t01 = at(row, col + 1);
      const std::int32_t t10 = at(row + 1, col);
      const std::int32_t t11 = at(row + 1, col + 1);
      const std::int32_t top = t00 + (t01 - t00) * fx / 16;
      const std::int32_t bottom = t10 + (t11 - t10) * fx / 16;
      const std::int32_t value = ((top + (bottom - top) * fy / 16) >> 9) & 0xffff;
      const auto i = static_cast<std::size_t>(static_cast<std::uint32_t>(x - x0) >> 5);
      const auto j = static_cast<std::size_t>(static_cast<std::uint32_t>(y - y0) >> 5);
      std::uint8_t& nibble = record.values[j * kFineSide + i];
      if (nibble < value) nibble = static_cast<std::uint8_t>(value);
    }
  }
}

void ExplorationMap::explore_circle(Point centre, std::int32_t radius, std::int32_t slot) {
  // 0x00516450. A radius of zero stamps nothing (its first test, beside the
  // empty mask); any other radius, a negative one included, runs.
  if (side_ == 0 || slot < 0 || slot >= kSlots || radius == 0) return;
  const std::int32_t outer = add32(radius, kInner);
  const std::int32_t inner = sub32(radius, kInner);
  // The box `(0, 0, 2R, 2R)` centred on the point (0x00423360), and the cells
  // from the one its low corner falls in (toward zero) up to the one its high
  // corner does, each kept to the map rectangle `(0, 0, w-1, h-1)`.
  const std::int32_t width = mul32(outer, 2);
  const std::int32_t bx0 = sub32(centre.x, width / 2);
  const std::int32_t by0 = sub32(centre.y, width / 2);
  const std::int32_t bx1 = add32(width, bx0);
  const std::int32_t by1 = add32(width, by0);
  const std::int64_t xs = static_cast<std::int64_t>(bx0 / kCellSize);
  const std::int64_t ys = static_cast<std::int64_t>(by0 / kCellSize);
  const std::int64_t xe = static_cast<std::int64_t>(bx1 / kCellSize) + 1;
  const std::int64_t ye = static_cast<std::int64_t>(by1 / kCellSize) + 1;
  const auto lo_x = static_cast<std::int32_t>(std::max<std::int64_t>(xs, 0));
  const auto lo_y = static_cast<std::int32_t>(std::max<std::int64_t>(ys, 0));
  const auto hi_x = static_cast<std::int32_t>(std::min<std::int64_t>(xe, side_));
  const auto hi_y = static_cast<std::int32_t>(std::min<std::int64_t>(ye, side_));
  for (std::int32_t cx = lo_x; cx < hi_x; ++cx) {
    for (std::int32_t cy = lo_y; cy < hi_y; ++cy) {
      const std::int32_t x0 = cx * kCellSize;
      const std::int32_t y0 = cy * kCellSize;
      const std::int32_t x1 = x0 + kCellSize - 1;
      const std::int32_t y1 = y0 + kCellSize - 1;
      if (classify_cell(x0, y0, x1, y1, centre.x, centre.y, outer) == -1) continue;
      const std::int32_t where = classify_cell(x0, y0, x1, y1, centre.x, centre.y, inner);
      const std::int32_t index = cy * side_ + cx;
      const State state = state_at(index, slot);
      if (where == 1) {
        // Inside `r - 48`: 0 and 2 become 3, a record erased; 1 and 3 stay.
        if (state == State::never || state == State::partial) {
          drop_record(index, slot);
          set_state(index, slot, State::full);
        }
      } else if (where == 0) {
        // Meets it: the rim, into a record a cell at 0 gets zeroed.
        if (state != State::never && state != State::partial) continue;
        FineRecord& record = record_for(index, slot);
        set_state(index, slot, State::partial);
        stamp_rim(record, x0, y0, centre, outer);
        normalise(index, slot);
      }
      // **Outside `r - 48` but inside `R`: nothing.** The second test answers
      // -1 for a cell the rim alone reaches, and the stamp writes only on 0
      // and 1, so such a cell keeps no trace of this circle.
    }
  }
}

void ExplorationMap::explore_sight(Point centre, std::int32_t radius, std::uint32_t mask) {
  // 0x00517ad0, the front end every unit stamp goes through. A radius past
  // 1024 is the large stamp's, whole.
  if (radius > kCellSize) {
    explore_circle_mask(centre, radius, mask);
    return;
  }
  mask &= 0xffu;
  if (side_ == 0 || radius == 0 || mask == 0) return;
  // **A negative radius stamps nothing here.** No caller passes one -- a
  // sight, or the notes' 300 -- and below -32 the original would index its
  // table cache before the array.
  if (radius < 0) return;
  const std::int32_t outer = radius + kInner;
  // The box `pt ± R`, and the three by three cells about the point's own
  // (its coordinates divided toward zero): a circle that reaches a fourth
  // cell -- a radius above 976 with the point near its cell's edge -- is cut
  // at the third.
  const std::int32_t base_x = centre.x / kCellSize;
  const std::int32_t base_y = centre.y / kCellSize;
  for (std::int32_t cx = base_x - 1; cx <= base_x + 1; ++cx) {
    for (std::int32_t cy = base_y - 1; cy <= base_y + 1; ++cy) {
      if (cx < 0 || cy < 0 || cx >= side_ || cy >= side_) continue;
      const std::int32_t x0 = cx * kCellSize;
      const std::int32_t y0 = cy * kCellSize;
      if (centre.x - outer > x0 + kCellSize - 1 || x0 > centre.x + outer ||
          centre.y - outer > y0 + kCellSize - 1 || y0 > centre.y + outer) {
        continue;
      }
      const std::int32_t index = cy * side_ + cx;
      for (std::int32_t slot = 0; slot < kSlots; ++slot) {
        if (((mask >> slot) & 1u) == 0) continue;
        const State state = state_at(index, slot);
        // 0 gets a zeroed record and becomes 2 -- even when the circle only
        // grazes the box and writes nothing into it; 2 is stamped; 1 and 3
        // are left alone. This stamp never writes 3.
        if (state != State::never && state != State::partial) continue;
        FineRecord& record = record_for(index, slot);
        set_state(index, slot, State::partial);
        stamp_brush(record, x0, y0, centre, outer);
        normalise(index, slot);
      }
    }
  }
}

void ExplorationMap::explore_all() {
  // `ExploreAll()` (0x005157c0) fills every half-word with 0xFFFF: state 3 for
  // every slot, and no record has anything left to say. It raises the
  // explored-everywhere flag first, whatever the grid holds.
  everywhere_ = true;
  std::fill(cells_.begin(), cells_.end(), static_cast<std::uint16_t>(0xFFFFu));
  for (auto& records : fine_) records.clear();
}

std::vector<ExplorationMap::FineEntry> ExplorationMap::fine_entries() const {
  std::vector<FineEntry> out;
  for (std::int32_t slot = 0; slot < kSlots; ++slot) {
    for (const auto& [index, record] : fine_[static_cast<std::size_t>(slot)]) {
      out.push_back(FineEntry{slot, index, &record});
    }
  }
  return out;
}

bool ExplorationMap::set_raw(std::int32_t side, std::span<const std::uint16_t> cells,
                             std::span<const FineEntry> records) {
  if (side < 0) return false;
  if (cells.size() != static_cast<std::size_t>(side) * static_cast<std::size_t>(side)) {
    return false;
  }
  std::array<std::map<std::int32_t, FineRecord>, kSlots> fine;
  for (const FineEntry& entry : records) {
    if (entry.record == nullptr || entry.slot < 0 || entry.slot >= kSlots) return false;
    if (entry.index < 0 || static_cast<std::size_t>(entry.index) >= cells.size()) return false;
    const auto state = static_cast<State>((cells[static_cast<std::size_t>(entry.index)] >>
                                           (2 * entry.slot)) & 3u);
    if (state != State::partial) return false;
    for (const std::uint8_t value : entry.record->values) {
      if (value > kFineMax) return false;
    }
    if (!fine[static_cast<std::size_t>(entry.slot)].emplace(entry.index, *entry.record).second) {
      return false;
    }
  }
  // And the other direction: a partial cell without its record is a grid this
  // writer could not have produced.
  for (std::size_t index = 0; index < cells.size(); ++index) {
    for (std::int32_t slot = 0; slot < kSlots; ++slot) {
      if (((cells[index] >> (2 * slot)) & 3u) != static_cast<unsigned>(State::partial)) continue;
      if (!fine[static_cast<std::size_t>(slot)].contains(static_cast<std::int32_t>(index))) {
        return false;
      }
    }
  }
  side_ = side;
  cells_.assign(cells.begin(), cells.end());
  fine_ = std::move(fine);
  return true;
}

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

FogSystem::Stamp FogSystem::stamp_of(const WorldObject& object) noexcept {
  // A held object stands at no point: `(-1, -1)`, the marker the original's
  // move stamp refuses (the point at 0x008212bc), so leaving a holder is a move even
  // onto the point it entered from.
  return Stamp{object.state.is_held() ? kHeldPosition : object.state.position,
               object.state.owner};
}

namespace {

/// The objects the original's move and owner stamps can reach: not internal
/// and not a spawn template (`[obj+0x2c] & 0x08000000`, tested by both
/// 0x005a8660 and 0x005aa0a0). 0x005a8660 tests 0x40000000 as well, a bit
/// whose meaning was not identified; nothing here carries it.
[[nodiscard]] bool stampable(const WorldObject& object) noexcept {
  return object.internal == InternalKind::none && !object.state.flags.unspawned;
}

}  // namespace

void FogSystem::sweep(World& world) {
  for (const WorldObject& slot : world.objects()) {
    if (!stampable(slot)) continue;
    // **A stamp is a move or an owner change.** 0x005a8660 stamps the new
    // point whenever the position changes, and 0x005aa0a0 stamps the point
    // whenever the owner is set; nothing else in the original stamps a unit's
    // own surroundings except a widening of its sight, which `SetSight` and
    // the scout skill stamp themselves. So an object is stamped when it has
    // moved or changed owner since it was last seen here, or was never seen
    // (a new object, or every object at a new game's start); a sight that
    // changed in place, or an owner's `viewflags` that changed, stamps
    // nothing. **Inference, labelled:** this runs once a turn, after
    // movement, where the original stamps at each position change -- the same
    // stamps for a unit that moves once a turn.
    const Stamp now = stamp_of(slot);
    const auto [it, fresh] = stamped_.try_emplace(slot.id, now);
    if (!fresh && it->second == now) continue;
    it->second = now;
    // No owner record, no stamp (`[obj+0x70] == 0`); a held object's point
    // is the refused marker.
    if (slot.state.owner == kNoPlayer || slot.state.is_held()) continue;
    // The owner record's `viewflags` (`[[obj+0x70] + 0x14]`): a unit explores
    // for every player its owner shares its view with.
    map_.explore_sight(slot.state.position, slot.sight, fog_view_mask(world, slot.state.owner));
  }
}

void FogSystem::prime(const World& world) {
  stamped_.clear();
  for (const WorldObject& slot : world.objects()) {
    if (stampable(slot)) stamped_.emplace(slot.id, stamp_of(slot));
  }
}

void FogSystem::stamp_now(World& world, ObjectId object, std::int32_t sight) {
  const WorldObject* slot = world.find(object);
  if (slot == nullptr || !stampable(*slot)) return;
  stamped_[slot->id] = stamp_of(*slot);
  if (slot->state.owner == kNoPlayer || slot->state.is_held()) return;
  map_.explore_sight(slot->state.position, sight, fog_view_mask(world, slot->state.owner));
}

void FogSystem::start(World& world) {
  const MatchSystem* match = match_system_of(world);
  map_.resize(match == nullptr ? 0 : match->rules().map_size);
  stamped_.clear();
  sweep(world);
}

void FogSystem::advance(World& world, const Turn& turn) {
  (void)turn;
  sweep(world);
}

FogSystem* fog_system_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "fog") return static_cast<FogSystem*>(system);
  }
  return nullptr;
}

std::int32_t fog_slot_from_script(std::int32_t player) noexcept {
  return player >= 1 && player <= 16 ? player - 1 : -1;
}

std::uint32_t fog_view_mask(const World& world, PlayerId player) noexcept {
  if (!PlayerTable::is_valid(player)) return 0;
  // The player's own bit. `SetRelation` (0x005652f0) never writes the
  // diagonal, so the own bit is the record initialiser's: 0x00564660, which
  // the game setup (0x00522314) runs once for each of the sixteen records,
  // writes `1 << id` into `playerflag`, `viewflags`, `seeflags`,
  // `supportflags` and `controlflags` alike, its complement into
  // `enemyflags`, and 0x35 into the diagonal of `relations`.
  std::uint32_t mask = 1u << player;
  // `SetRelation(A, B, word)`: share-view (0x10) in A's word for B sets B's
  // `playerflag` in A's `viewflags` (`record + 0x14`), and clears it without.
  // So A's view mask is A plus every B A's own row grants share-view.
  for (PlayerId other = 0; other < static_cast<PlayerId>(ExplorationMap::kSlots); ++other) {
    if (other == player) continue;
    if (world.players().has(player, other, Relation::share_view)) mask |= 1u << other;
  }
  // The stamp keeps the low byte (0x00516474): players 9..16 have no fog slot.
  return mask & 0xffu;
}

void merge_shared_view(World& world, PlayerId granter, PlayerId receiver) {
  FogSystem* fog = fog_system_of(world);
  if (fog == nullptr) return;
  // Both callers have range-checked the pair already. This one is for the
  // function's own sake: an unowned object's owner is `kNoPlayer`, so an
  // invalid number here would match every unowned object.
  if (!PlayerTable::is_valid(granter) || !PlayerTable::is_valid(receiver)) return;
  // The stamp keeps the mask's low byte (0x00517ad0's `param_3 & 0xff`, as
  // 0x00516474 does): a receiver past the eighth player has no slot, so the
  // walk writes nothing.
  ExplorationMap& map = fog->map();
  // 0x00564cd0 over the whole map rectangle, which is every object the grid
  // holds: a held object is in no cell, and an internal one is filed nowhere.
  // The stamps are `max`es and promotions, so the walk's own order -- cells
  // by rows, each cell's list from its head -- writes the same map as any
  // other, and spawn order is taken.
  for (const WorldObject& slot : world.objects()) {
    if (!SpatialIndex::stands(slot)) continue;
    // `test [obj+0x2c], 0x8000000` / `jne skip`: a spawn template is in its
    // cell and is passed over.
    if (slot.state.flags.unspawned) continue;
    // `[node+0x2c] & (granter | receiver)`: the one-hot owner is one of the
    // pair. An unowned object has no bit.
    const PlayerId owner = slot.state.owner;
    if (owner != granter && owner != receiver) continue;
    // The object's own sight (`[obj+0xd0]`) about its own point (`vtbl +
    // 0x3c`), for the receiver's slot alone, through the unit stamp
    // 0x00517ad0 as the sweep's stamps are. No test of life and none of the
    // sight; the stamp's own first test refuses a radius of zero.
    map.explore_sight(slot.state.position, slot.sight, 1u << receiver);
  }
}

void ExplorationMap::explore_circle_mask(Point centre, std::int32_t radius, std::uint32_t mask) {
  // 0x00516450 walks the mask's bits within each cell, two state bits a slot;
  // the slots are independent, so slot by slot is the same writes.
  for (std::int32_t slot = 0; slot < kSlots; ++slot) {
    if (((mask >> slot) & 1u) != 0) explore_circle(centre, radius, slot);
  }
}

// --------------------------------------------------------------------------
// the host entry points
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::HostOutcome;
using script::Value;

[[nodiscard]] World* host_world(CallContext& ctx) {
  return ctx.user == nullptr ? nullptr : static_cast<HostContext*>(ctx.user)->world;
}

[[nodiscard]] ExplorationMap* host_map(CallContext& ctx) {
  World* world = host_world(ctx);
  if (world == nullptr) return nullptr;
  FogSystem* fog = fog_system_of(*world);
  return fog == nullptr ? nullptr : &fog->map();
}

/// `IsExplored(pt, player)` -- 5 sites, all of them a tutorial hint deciding
/// whether the player has found the thing it is about to talk about.
///
/// `0x004c6820` pops the player, then the point. No exploration manager
/// (`[0x009a721c] == 0` or its `+0xb0` null) answers **false**. Otherwise it
/// hands the point and `player - 1` to 0x00515950 -- `ExplorationMap::explored`
/// -- whose slot is unsigned, so player 0 (slot -1) and every player past the
/// eighth answer **true**, and a partially explored cell answers its fine
/// record at the point. No argument is validated.
HostOutcome fn_is_explored(CallContext& ctx) {
  const ExplorationMap* map = host_map(ctx);
  if (map == nullptr || ctx.count() < 2) return HostOutcome::ok_with(Value::boolean(false));
  if (!is_point(ctx.arg(0)) || !ctx.arg(1).is_integer()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const std::int32_t slot = ctx.arg(1).as_integer() - 1;
  return HostOutcome::ok_with(Value::boolean(map->explored(unpack_point(ctx.arg(0)), slot)));
}

/// The `viewflags` a script's player number reaches in `ExploreCircle` and
/// `ExploreArea`: both bounds-check `0 <= player < 16` and read
/// `[globals + 0xfc0 + player * 0x320]`, which is `record[player - 1] +
/// 0x14` (see the header). Player 0 passes the check and reads the word
/// before the record array; that is a fault rather than a behaviour, so it
/// explores nothing here. Player 16 fails the check and explores nothing in
/// both.
[[nodiscard]] std::uint32_t script_view_mask(const World& world, std::int32_t player) noexcept {
  if (player < 1 || player >= 16) return 0;
  return fog_view_mask(world, static_cast<PlayerId>(player - 1));
}

/// `ExploreCircle(player, pt, radius)` -- 3 sites: a settlement's bought map, a
/// tower's own sight, and the research that widens it.
///
/// `0x004c6470` pops the radius, the point and the player; a player outside
/// `0..15` does nothing, and otherwise the circle stamp (0x00516450) runs with
/// that player's `viewflags` -- its own slot and every slot it shares its view
/// with. A radius of zero stamps nothing (the stamp's own first test).
HostOutcome fn_explore_circle(CallContext& ctx) {
  World* world = host_world(ctx);
  ExplorationMap* map = host_map(ctx);
  if (world == nullptr || map == nullptr || ctx.count() < 3) return HostOutcome::ok_void();
  if (!ctx.arg(0).is_integer() || !is_point(ctx.arg(1)) || !ctx.arg(2).is_integer()) {
    return HostOutcome::ok_void();
  }
  map->explore_circle_mask(unpack_point(ctx.arg(1)), ctx.arg(2).as_integer(),
                           script_view_mask(*world, ctx.arg(0).as_integer()));
  return HostOutcome::ok_void();
}

/// `ExploreArea(player, area)` -- 24 sites, every one inside a map container.
///
/// `0x004c9430` pops the name, then the player. It resolves the name in the
/// named-group dictionary (0x004c93c0) and takes the group's front member as
/// the area; a name that matches nothing, or a front member whose handle is
/// dead, prints *"Could not find area named '%s' in function 'ExploreArea'.
/// Check the spelling."* and explores nothing, which is a return and not a
/// refusal. A player outside `0..15` explores nothing either. Otherwise the
/// circle stamp runs at the area's centre (0x004d67a0: the circle's own
/// centre, or the rectangle's truncated midpoints) with radius **the bounding
/// box's bottom less the centre's y** (0x004c95c1: `[box + 0xc] - [centre +
/// 4]`, the box from 0x004d6800) -- a circle's own radius, a rectangle's half
/// *height*, not a radius that covers it -- and the player's `viewflags`.
///
/// The original takes a rectangle's corners as authored where `AreaShape`
/// sorts them; for a rectangle authored bottom-up the original's radius is
/// negative. No shipped area is.
HostOutcome fn_explore_area(CallContext& ctx) {
  World* world = host_world(ctx);
  ExplorationMap* map = host_map(ctx);
  if (world == nullptr || map == nullptr || ctx.count() < 2) return HostOutcome::ok_void();
  if (!ctx.arg(0).is_integer() || !ctx.arg(1).is_string()) return HostOutcome::ok_void();
  const AreaShape* shape = area_named(*world, ctx.arg(1).as_string());
  if (shape == nullptr) return HostOutcome::ok_void();
  const Point centre = shape->centre();
  const std::int32_t radius = shape->bounds().bottom - centre.y;
  map->explore_circle_mask(centre, radius, script_view_mask(*world, ctx.arg(0).as_integer()));
  return HostOutcome::ok_void();
}

/// `ExploreAll()` -- 9 sites, and every one of them is a mission ending or a
/// cutscene that wants the whole board visible. 0x006a42a0 calls 0x005157c0
/// and nothing else.
HostOutcome fn_explore_all(CallContext& ctx) {
  if (ExplorationMap* map = host_map(ctx); map != nullptr) map->explore_all();
  return HostOutcome::ok_void();
}

/// The terrain-type bytes `GetUnexploredPoint` treats as water (0x005d9b9e,
/// `cmp 0xc` and `cmp 0xd`). `IsPointInWater` names only 13.
constexpr std::uint32_t kExploreWaterLow = 12;
constexpr std::uint32_t kExploreWaterHigh = 13;
/// The sample grid's step and the most candidates kept.
constexpr std::int32_t kExploreStep = 300;
constexpr std::int32_t kExploreMaxCandidates = 100;

/// `Unit::GetUnexploredPoint(radius)` -- 1 site, `UNIT_EXPLORE.VS`, which
/// passes **-1** and tests the answer's `x` against -1.
///
/// What 0x005d99e0 does, in its order:
///
///   1. A receiver handle that names no object prints "...called for an
///      uninitialized or invalid object." and answers `(-1, -1)`. Nothing else
///      about the receiver is checked.
///   2. The radius: an argument below the unit's `sight` (`[obj+0xd0]`) or
///      above 800,000 becomes `sight * 4`, raised to 512; then anything above
///      2,999 becomes 3,000.
///   3. A grid of offsets from `-r/2` (C division) up to but not including
///      `r/2` in steps of 300, **x in the outer loop, y in the inner**, each
///      added to the unit's position (`vtbl+0x3c`) and clamped into the map
///      rectangle (`[0x009a721c]+0xb4..0xc0`).
///   4. A sample is a candidate when the owner's slot (`[[obj+0x70]+8]`) has
///      not explored it (0x00515950), its 16-unit obstruction cell
///      (`[0x009a721c]+0x60`, the grid the area sampler tests) is clear, and
///      its terrain-type byte -- the 64-unit layer, no half-cell bias -- is
///      12 or 13 for a `water_unit` class (`[class+0xb30]`) and neither for
///      any other. The first 100 candidates are kept and the scan stops at
///      the hundredth.
///   5. None: `(-1, -1)`. Otherwise **one draw**, `Range(0, n - 1)` on the
///      game's generator (`[0x00996ff4]+0x12a0`, `vtbl+0x14`), picks the
///      answer -- which, with one candidate, draws nothing.
///
/// This used to answer the nearest never-seen cell's centre within the
/// radius, with no draw: a different point, and the world's random stream one
/// draw short whenever more than one candidate stood.
HostOutcome fn_get_unexplored_point(CallContext& ctx) {
  World* world = host_world(ctx);
  const ExplorationMap* map = host_map(ctx);
  const auto nowhere = [] { return HostOutcome::ok_with(pack_point(Point{-1, -1})); };
  if (world == nullptr || map == nullptr) return nowhere();
  if (!ctx.arg(0).is_object()) return nowhere();
  const WorldObject* slot = world->find(ctx.arg(0).as_object().id);
  if (slot == nullptr) return nowhere();

  std::int32_t radius = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : -1;
  if (radius < slot->sight || radius > 800000) {
    radius = std::max<std::int32_t>(slot->sight * 4, 512);
  }
  radius = std::min<std::int32_t>(radius, 3000);

  // The map rectangle, `(0, 0, size - 1, size - 1)`.
  const MatchSystem* match = match_system_of(*world);
  const std::int32_t high = match != nullptr && match->rules().map_size > 0
                                ? match->rules().map_size - 1
                                : map->cells() * ExplorationMap::kCellSize - 1;
  const MovementSystem* movement = movement_system(*world);
  const ObstructionGrid* grid = movement != nullptr ? &movement->grid() : nullptr;
  if (grid != nullptr && grid->empty()) grid = nullptr;
  const Grid& terrain = world->terrain();
  const bool water_unit = class_flag(*world, *slot, "water_unit");
  // An unowned object's slot is past the eighth, which 0x00515950 answers
  // explored everywhere: no candidates.
  const std::int32_t fog_slot = slot->state.owner == kNoPlayer
                                    ? ExplorationMap::kSlots
                                    : static_cast<std::int32_t>(slot->state.owner);

  const Point from = world->resolve_position(slot->id);
  const std::int32_t half = radius / 2;
  std::vector<Point> candidates;
  for (std::int32_t ox = -half; ox < half; ox += kExploreStep) {
    for (std::int32_t oy = -half; oy < half; oy += kExploreStep) {
      const Point at{std::clamp(from.x + ox, 0, high), std::clamp(from.y + oy, 0, high)};
      if (map->explored(at, fog_slot)) continue;
      if (grid != nullptr && grid->blocked(at)) continue;
      std::uint32_t type = 0;
      if (terrain.cell_size() != 0) {
        const auto cx = static_cast<std::uint32_t>(at.x) / terrain.cell_size();
        const auto cy = static_cast<std::uint32_t>(at.y) / terrain.cell_size();
        if (cx < terrain.width() && cy < terrain.height()) type = terrain.cell(cx, cy);
      }
      const bool water = type == kExploreWaterLow || type == kExploreWaterHigh;
      if (water != water_unit) continue;
      candidates.push_back(at);
      if (static_cast<std::int32_t>(candidates.size()) == kExploreMaxCandidates) break;
    }
    if (static_cast<std::int32_t>(candidates.size()) == kExploreMaxCandidates) break;
  }
  if (candidates.empty()) return nowhere();
  const std::int32_t pick =
      world->rng().between(0, static_cast<std::int32_t>(candidates.size()) - 1);
  return HostOutcome::ok_with(pack_point(candidates[static_cast<std::size_t>(pick)]));
}

constexpr std::size_t kEntryCount = 5;

}  // namespace

std::size_t fog_host_entry_count() noexcept { return kEntryCount; }

std::size_t register_fog_host(script::HostRegistry& registry) {
  const std::size_t before = registry.implemented();
  constexpr script::CallKind kFree = script::CallKind::free_function;
  registry.define(kFree, "IsExplored", 2, &fn_is_explored);        //  5
  registry.define(kFree, "ExploreArea", 2, &fn_explore_area);      // 24
  registry.define(kFree, "ExploreCircle", 3, &fn_explore_circle);  //  3
  registry.define(kFree, "ExploreAll", 0, &fn_explore_all);        //  9
  registry.define(script::CallKind::member, "GetUnexploredPoint", 1,
                  &fn_get_unexplored_point);  //  1
  // `RecreateExploration/0` and `ToggleFog/0` are registered in `gbr.exe` and
  // are not bound here; the header says why for each.
  // `RevealHiddenEnemyUnits/3` is bound, in `sim/player_host.cpp`: it is a
  // diplomacy test and a hidden bit, and never looks at this grid.
  return registry.implemented() - before;
}

}  // namespace imperivm::core::sim
