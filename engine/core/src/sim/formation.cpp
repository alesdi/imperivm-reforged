#include "imperivm/core/sim/formation.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/world.hpp"

// The model is docs/formats/formation.md; the addresses below are where each
// rule is read in `gbr.exe`.

namespace imperivm::core::sim {

bool Formation::holds(ObjectId unit) const noexcept {
  if (slot_of(unit) != nullptr) return true;
  return std::find(pending.begin(), pending.end(), unit) != pending.end();
}

const FormationSlot* Formation::slot_of(ObjectId unit) const noexcept {
  for (const FormationSlot& slot : slots) {
    if (slot.unit == unit) return &slot;
  }
  return nullptr;
}

void formation_add(Formation& form, ObjectId unit) {
  if (unit == kNoObject || form.holds(unit)) return;
  // 0x005f47e0: pushed on the pending vector (`+0x74`), the layout marked.
  form.pending.push_back(unit);
  form.rebuild = true;
}

bool formation_remove(Formation& form, ObjectId unit) {
  // 0x005f3290: a slot is unlinked from the list, one fewer alive and one
  // more died; a pending unit is erased from the vector. The radius stands
  // until the layout runs again.
  for (auto it = form.slots.begin(); it != form.slots.end(); ++it) {
    if (it->unit != unit) continue;
    form.slots.erase(it);
    --form.alive;
    ++form.died;
    form.rebuild = true;
    return true;
  }
  const auto it = std::find(form.pending.begin(), form.pending.end(), unit);
  if (it == form.pending.end()) return false;
  form.pending.erase(it);
  form.rebuild = true;
  return true;
}

// --------------------------------------------------------------------------
// the layout
// --------------------------------------------------------------------------

FormationClassTraits formation_class_traits(const ClassGraph& graph, ClassIndex klass) {
  FormationClassTraits out;
  if (klass == kNoClass || klass >= graph.size()) {
    out.priority = kUndeclaredPriority;
    return out;
  }
  // 0x005f0470: `[class+0x2e4]`, and `[class+0x2e0]` when that is negative.
  // The class loader (0x0059c310) has already copied an undeclared value
  // down from the parent, which `ClassGraph::property` does by walking up.
  std::int32_t value = 0;
  if (parse_int(graph.property(klass, "formation_radius"), value) && value >= 0) {
    out.spacing_radius = value;
  } else if (parse_int(graph.property(klass, "selection_radius"), value)) {
    out.spacing_radius = value;
  }
  out.priority = parse_int(graph.property(klass, "formation_priority"), value)
                     ? value
                     : kUndeclaredPriority;
  return out;
}

namespace {

enum Group : std::size_t { kCentral = 0, kFront = 1, kLeft = 2, kRight = 3, kGroups = 4 };

/// Whether `klass` is `base` or descends from it (0x0059a1f0, up the parent
/// chain at `[class+0xe4]`).
[[nodiscard]] bool is_a(const ClassGraph& graph, ClassIndex klass, ClassIndex base) {
  if (klass == kNoClass || base == kNoClass) return false;
  for (const ClassIndex step : graph.ancestry(klass)) {
    if (step == base) return true;
  }
  return false;
}

/// One `<Class>` entry, its class resolved.
struct Entry {
  ClassIndex klass = kNoClass;
  /// The chooser's weights, doubled so the halved wings stay integers:
  /// 0x005f1dbc..0x005f1dd4 take `CentralBlock`, `FrontLine`, and `Wings`
  /// times 0.5 for each wing.
  std::array<std::int64_t, kGroups> weight{};
};

/// The loader's order (0x005f6ca0): each entry goes before the first entry
/// already listed whose class it is or descends from, so a class is met
/// before its ancestors; otherwise at the end.
[[nodiscard]] std::vector<Entry> loader_order(const ClassGraph& graph,
                                              const FormationClassDef& shape) {
  std::vector<Entry> out;
  for (const FormationRole& role : shape.roles) {
    Entry entry;
    entry.klass = graph.lookup(role.class_name);
    if (entry.klass == kNoClass) continue;
    entry.weight = {2 * static_cast<std::int64_t>(role.central),
                    2 * static_cast<std::int64_t>(role.front), role.wings, role.wings};
    auto at = out.begin();
    for (; at != out.end(); ++at) {
      if (is_a(graph, entry.klass, at->klass)) break;
    }
    out.insert(at, entry);
  }
  return out;
}

/// 0x005f01f0: the group whose count over weight is lowest -- a weight of 0
/// never, the earliest on a tie -- and its count goes up by one. The
/// original divides in single precision; with the small integers involved
/// the quotients compare as the exact fractions do, which is what the cross
/// products here compare. A record no group will take (every weight 0) goes
/// to the central block (docs/formats/formation.md, "What is still unknown").
[[nodiscard]] std::size_t choose_group(const std::array<std::int64_t, kGroups>& weight,
                                       std::array<std::int64_t, kGroups>& count) {
  std::size_t best = kGroups;
  for (std::size_t g = 0; g < kGroups; ++g) {
    if (weight[g] == 0) continue;
    if (best == kGroups) {
      best = g;
      continue;
    }
    // count[g] / weight[g] < count[best] / weight[best]
    std::int64_t lhs = count[g] * weight[best];
    std::int64_t rhs = count[best] * weight[g];
    if ((weight[g] < 0) != (weight[best] < 0)) std::swap(lhs, rhs);
    if (lhs < rhs) best = g;
  }
  if (best == kGroups) best = kCentral;
  ++count[best];
  return best;
}

/// Where the layout writes, and the largest squared offset it has written
/// (the global at 0x009c0978). Every write goes through 0x005f03f0, whose
/// jitter is a draw in `[-shape_chaos, shape_chaos]`: with `shape_chaos` 0 the
/// range is empty and nothing is drawn.
struct Writer {
  std::vector<FormationRecord>& records;
  std::int64_t farthest = 0;

  void put(std::size_t index, std::int32_t x, std::int32_t y) {
    records[index].offset = Point{x, y};
    farthest = std::max(farthest, static_cast<std::int64_t>(x) * x +
                                      static_cast<std::int64_t>(y) * y);
  }
};

/// `j`-th of `n` along a side from `-h` to `h` (0x005f04b0): `2hj/(n+1) - h`.
[[nodiscard]] std::int32_t along(std::int32_t h, std::int32_t j, std::int32_t n) {
  return static_cast<std::int32_t>(static_cast<std::int64_t>(2) * h * j / (n + 1)) - h;
}

struct Extent {
  std::int32_t half_width = 0;  ///< `L`
  std::int32_t depth = 0;       ///< `Fr`, to the front
};

/// The central block (0x005f04b0): square rings round the hero.
Extent lay_block(Writer& out, std::span<const std::size_t> group, std::int32_t radius,
                 std::int32_t spacing, bool front_line) {
  Extent extent{radius, radius};
  std::size_t next = 0;
  std::int32_t h = radius;
  for (std::int32_t ring = 1; next < group.size(); ++ring) {
    h += spacing;
    const auto left = static_cast<std::int32_t>(group.size() - next);
    const std::int32_t k = std::min(left, 8 * ring);
    const auto put = [&](std::int32_t x, std::int32_t y) { out.put(group[next++], x, y); };
    if (k >= 4) {
      const std::int32_t q = (k - 4) / 4;
      const std::int32_t m = (k - 4) % 4;
      std::int32_t back = q;
      std::int32_t sides = q;
      std::int32_t front = q;
      if (m == 1) {
        front = q + 1;
      } else if (m == 2) {
        if (front_line) {
          sides = q + 1;
        } else {
          back = q + 1;
          front = q + 1;
        }
      } else if (m == 3) {
        front = q + 1;
        sides = q + 1;
      }
      for (std::int32_t j = 1; j <= back; ++j) put(along(h, j, back), h);
      for (std::int32_t j = 1; j <= sides; ++j) {
        const std::int32_t y = along(h, j, sides);
        put(-h, y);
        put(h, y);
      }
      for (std::int32_t j = 1; j <= front; ++j) put(along(h, j, front), -h);
      put(-h, h);
      put(h, h);
      put(-h, -h);
      put(h, -h);
      extent = {h, h};
    } else if (k == 1) {
      put(0, -h);
      extent = {h - spacing, h};
    } else if (k == 2) {
      if (front_line) {
        put(-h, 0);
        put(h, 0);
        extent = {h, h - spacing};
      } else {
        put(0, -h);
        put(0, h);
        extent = {h - spacing, h};
      }
    } else {
      put(-h, -h);
      put(h, -h);
      put(0, h);
      extent = {h, h};
    }
  }
  return extent;
}

/// The front line's grid (0x005f02e0).
void front_grid(std::int32_t n, std::int32_t width, std::int32_t height, std::int32_t& cols,
                std::int32_t& rows) {
  cols = width;
  rows = height;
  while (static_cast<std::int64_t>(cols) * rows < n) {
    cols += width;
    rows += height;
  }
  while (cols > 1 && static_cast<std::int64_t>(cols - 1) * rows >= n) --cols;
  while (rows > 1 && static_cast<std::int64_t>(rows - 1) * cols >= n) --rows;
}

/// The front line (0x005f0b60), ahead of the block.
Extent lay_front(Writer& out, std::span<const std::size_t> group, const FormationClassDef& shape,
                 std::int32_t spacing, Extent block) {
  const std::int32_t depth = block.depth + shape.offset_front_line_y;
  if (group.empty()) return {block.half_width, depth};
  const auto n = static_cast<std::int32_t>(group.size());
  std::int32_t cols = 1;
  std::int32_t rows = 1;
  front_grid(n, std::max(shape.width, 1), std::max(shape.height, 1), cols, rows);
  std::int32_t half = (cols / 2) * spacing;
  if (cols % 2 == 0) half -= spacing / 2;
  half = std::max(half, 0);
  half = std::max(half, block.half_width);
  const std::int32_t total = rows * spacing + depth;

  std::size_t next = 0;
  std::int32_t y = -total;
  while (next < group.size()) {
    std::int32_t p = std::min<std::int32_t>(cols, static_cast<std::int32_t>(group.size() - next));
    std::int32_t inner = 0;
    std::int32_t outer = 0;
    if (p % 2 == 1) {
      out.put(group[next++], 0, y);
      --p;
      inner = -spacing;
      outer = spacing;
    } else {
      inner = -(spacing / 2);
      outer = spacing - spacing / 2;
    }
    const std::int32_t pairs = p / 2;
    if (pairs == 1) {
      out.put(group[next++], -half, y);
      out.put(group[next++], half, y);
    } else {
      for (std::int32_t i = 0; i < pairs; ++i) {
        out.put(group[next++],
                inner - static_cast<std::int32_t>(static_cast<std::int64_t>(half + inner) * i /
                                                  (pairs - 1)),
                y);
        out.put(group[next++],
                static_cast<std::int32_t>(static_cast<std::int64_t>(half - outer) * i /
                                          (pairs - 1)) +
                    outer,
                y);
      }
    }
    y += spacing;
  }
  return {half, total};
}

/// One wing (0x005f0d90), standing off the front line's edge.
void lay_wing(Writer& out, std::span<const std::size_t> group, const FormationClassDef& shape,
              std::int32_t spacing, Extent front, bool left) {
  if (group.empty()) return;
  const auto n = static_cast<std::int64_t>(group.size());
  std::int64_t q = std::max<std::int64_t>(isqrt(n), 1);
  std::int64_t cols = q;
  if (q * q < n) cols = n <= q * (q + 1) ? q : q + 1;
  const std::int32_t start = left ? -front.half_width - shape.offset_wings_x
                                  : front.half_width + shape.offset_wings_x;
  const std::int32_t step = left ? -spacing : spacing;
  std::int32_t y = -(front.depth + shape.offset_wings_y);
  std::size_t next = 0;
  while (next < group.size()) {
    const std::size_t row = std::min<std::size_t>(static_cast<std::size_t>(cols),
                                                  group.size() - next);
    std::int32_t x = start;
    for (std::size_t i = 0; i < row; ++i) {
      x += step;
      out.put(group[next++], x, y);
    }
    y += spacing;
  }
}

}  // namespace

std::int32_t formation_lay_out(const ClassGraph& graph, const FormationClassDef& shape,
                               std::int32_t hero_radius, std::vector<FormationRecord>& records) {
  // The grouping (0x005f1d30). The four counts run on across every entry.
  struct Taken {
    std::size_t index;
    std::int32_t priority;
  };
  std::array<std::vector<Taken>, kGroups> taken;
  std::array<std::int64_t, kGroups> count{};
  std::vector<bool> used(records.size(), false);
  for (const Entry& entry : loader_order(graph, shape)) {
    while (true) {
      ClassIndex klass = kNoClass;
      for (std::size_t i = 0; i < records.size(); ++i) {
        if (!used[i] && is_a(graph, records[i].klass, entry.klass)) {
          klass = records[i].klass;
          break;
        }
      }
      if (klass == kNoClass) break;
      const std::int32_t priority = formation_class_traits(graph, klass).priority;
      for (std::size_t i = 0; i < records.size(); ++i) {
        if (used[i] || records[i].klass != klass) continue;
        used[i] = true;
        taken[choose_group(entry.weight, count)].push_back({i, priority});
      }
    }
  }

  // Within a group, by `formation_priority`, lowest first, then in the order
  // taken (0x005f1c20: a map keyed by priority, a list under each key).
  std::array<std::vector<std::size_t>, kGroups> groups;
  std::array<std::int32_t, kGroups> spacing{};
  for (std::size_t g = 0; g < kGroups; ++g) {
    std::stable_sort(taken[g].begin(), taken[g].end(),
                     [](const Taken& a, const Taken& b) { return a.priority < b.priority; });
    // 0x005f0470: twice the largest radius in the group, 0 for none.
    std::int32_t widest = 0;
    bool any = false;
    for (const Taken& t : taken[g]) {
      groups[g].push_back(t.index);
      const std::int32_t r = formation_class_traits(graph, records[t.index].klass).spacing_radius;
      widest = any ? std::max(widest, r) : r;
      any = true;
    }
    spacing[g] = 2 * widest;
  }

  // The layout (0x005f0eb0).
  Writer out{records};
  const Extent block = lay_block(out, groups[kCentral], hero_radius, spacing[kCentral],
                                 !groups[kFront].empty());
  const Extent front = lay_front(out, groups[kFront], shape, spacing[kFront], block);
  lay_wing(out, groups[kLeft], shape, spacing[kLeft], front, true);
  lay_wing(out, groups[kRight], shape, spacing[kRight], front, false);

  // The slot list, central, front, left, right; whatever no entry took is
  // dropped.
  std::vector<FormationRecord> ordered;
  for (const std::vector<std::size_t>& group : groups) {
    for (const std::size_t i : group) ordered.push_back(records[i]);
  }
  records = std::move(ordered);
  return static_cast<std::int32_t>(isqrt(out.farthest));
}

namespace {

[[nodiscard]] const FormationClassDef* shape_of(const Formation& form,
                                                const FormationTable& table) {
  if (!form.shape.empty()) {
    if (const FormationClassDef* chosen = table.find(form.shape)) return chosen;
  }
  return table.default_formation();
}

[[nodiscard]] ClassIndex class_of(const World& world, ObjectId id) {
  const WorldObject* slot = world.find(id);
  return slot == nullptr ? kNoClass : slot->class_index;
}

}  // namespace

bool formation_rebuild(World& world, Formation& form, const FormationTable& table,
                       std::vector<ObjectId>& joined) {
  if (!form.rebuild) return false;
  const std::vector<ObjectId> was_pending = form.pending;

  // 0x005f45f0 and 0x005f1ad0: the slots' units in slot order, then every
  // pending unit in the order it joined.
  std::vector<ObjectId> units;
  for (const FormationSlot& slot : form.slots) units.push_back(slot.unit);
  for (const ObjectId id : form.pending) units.push_back(id);
  form.pending.clear();
  form.slots.clear();

  const ClassGraph* graph = world.class_graph();
  const FormationClassDef* shape = shape_of(form, table);
  if (units.empty()) {
    form.radius = 0;
  } else if (graph != nullptr && shape != nullptr) {
    std::vector<FormationRecord> records;
    records.reserve(units.size());
    for (const ObjectId id : units) records.push_back({class_of(world, id), Point{}});
    std::int32_t hero_radius = 0;
    (void)parse_int(graph->property(class_of(world, form.hero), "radius"), hero_radius);
    form.radius = formation_lay_out(*graph, *shape, hero_radius, records);

    // 0x005f4b30: every sample zeroed, and each unit, in order, takes the
    // first free slot of exactly its class.
    form.slots.reserve(records.size());
    for (const FormationRecord& record : records) {
      form.slots.push_back({kNoObject, record.klass, record.offset, Point{0, 0}});
    }
    for (const ObjectId id : units) {
      const ClassIndex klass = class_of(world, id);
      for (FormationSlot& slot : form.slots) {
        if (slot.unit == kNoObject && slot.klass == klass) {
          slot.unit = id;
          break;
        }
      }
    }
  }
  form.alive = static_cast<std::int32_t>(form.slots.size());
  form.died = 0;
  form.rebuild = false;
  form.reassign = true;
  // 0x005f4390: every unit that was pending is handed `form_move`.
  joined.insert(joined.end(), was_pending.begin(), was_pending.end());
  return true;
}

// --------------------------------------------------------------------------
// the sample
// --------------------------------------------------------------------------

namespace {

/// `num / sqrt(len2)`, truncated toward zero, exactly: the largest `k` with
/// `k^2 * len2 <= num^2`.
[[nodiscard]] std::int32_t over_length(std::int64_t num, std::int64_t len2) {
  const std::int64_t magnitude = isqrt((num < 0 ? -num : num) * (num < 0 ? -num : num) / len2);
  return static_cast<std::int32_t>(num < 0 ? -magnitude : magnitude);
}

}  // namespace

Point formation_turn(Point centre, Point dir, Point offset) noexcept {
  if (dir.x == 0 && dir.y == 0) return Point{centre.x + offset.x, centre.y + offset.y};
  // 0x005f2900: with `a = -dy/|d|` and `b = dx/|d|`, the sample is
  // `(cx + trunc(ox*a - oy*b), cy + trunc(oy*a + ox*b))`. The original takes
  // the products from single-precision factors in the FPU; this takes the
  // exact quotient, which differs only where a product lands within rounding
  // of an integer (docs/formats/formation.md, "Still approximated").
  const std::int64_t dx = dir.x;
  const std::int64_t dy = dir.y;
  const std::int64_t len2 = dx * dx + dy * dy;
  const std::int64_t ox = offset.x;
  const std::int64_t oy = offset.y;
  return Point{centre.x + over_length(-ox * dy - oy * dx, len2),
               centre.y + over_length(-oy * dy + ox * dx, len2)};
}

// -- the claim on the stations ----------------------------------------------------

namespace {

/// A cell nobody may have, and one the flood has not reached.
constexpr std::int32_t kClaimNobody = std::numeric_limits<std::int32_t>::max();
/// A closed cell while the flood runs: the least integer.
constexpr std::int32_t kClaimClosed = std::numeric_limits<std::int32_t>::min();
/// A straight and a diagonal step, 16.16.
constexpr std::int32_t kClaimStraight = 0x10000;
constexpr std::int32_t kClaimDiagonal = 0x16a09;

/// A world coordinate's cell, divided towards zero.
[[nodiscard]] constexpr std::int32_t claim_cell(std::int32_t world) noexcept {
  return world / kCollisionCellSize;
}

/// Cells `x0..x1` by `y0..y1`, both ends included.
struct ClaimBox {
  std::int32_t x0 = 0;
  std::int32_t y0 = 0;
  std::int32_t x1 = -1;
  std::int32_t y1 = -1;
};

/// The claim's square of cells, each its distance from the centre or
/// nobody's.
class ClaimSquare {
 public:
  ClaimBox box;
  std::int32_t columns = 0;
  std::vector<std::int32_t> cells;

  [[nodiscard]] bool holds(std::int32_t x, std::int32_t y) const noexcept {
    return x >= box.x0 && x <= box.x1 && y >= box.y0 && y <= box.y1;
  }
  [[nodiscard]] std::int32_t& at(std::int32_t x, std::int32_t y) noexcept {
    return cells[static_cast<std::size_t>(y - box.y0) * static_cast<std::size_t>(columns) +
                 static_cast<std::size_t>(x - box.x0)];
  }

  /// The cheapest cell of `area` that is somebody's, for a station in cell
  /// `(sx, sy)`, written to `out` with its cost; the least integer, `out`
  /// untouched, when there is none.
  std::int32_t cheapest(ClaimBox area, std::int32_t sx, std::int32_t sy, Point& out) {
    area.x0 = std::max(area.x0, box.x0);
    area.x1 = std::min(area.x1, box.x1);
    area.y0 = std::max(area.y0, box.y0);
    area.y1 = std::min(area.y1, box.y1);
    std::int32_t best = kClaimNobody;
    for (std::int32_t y = area.y0; y <= area.y1; ++y) {
      for (std::int32_t x = area.x0; x <= area.x1; ++x) {
        const std::int32_t distance = at(x, y);
        if (distance == kClaimNobody) continue;
        const std::int32_t ox = x >= sx ? x - sx : sx - x;
        const std::int32_t oy = y >= sy ? y - sy : sy - y;
        const std::int32_t reach = oy < ox ? ox + oy / 2 : oy + ox / 2;
        const std::int32_t cost = (distance >> 17) + reach * reach;
        if (cost < best) {
          best = cost;
          out = Point{x, y};
        }
      }
    }
    return best == kClaimNobody ? kClaimClosed : best;
  }
};

}  // namespace

void formation_claim_gates(World& world, ObjectId hero, GameTime now, CellOverlay& out) {
  out.clear();
  const ObjectState* own = world.state(hero);
  if (own == nullptr) {
    out.seal();
    return;
  }
  const Point at = own->position;
  GateLines& lines = world.gate_lines();
  lines.refresh(world);
  for (const GateLines::Line& line : lines.lines()) {
    if (!line.has_axis) continue;
    const WorldObject* gate = world.find(line.gate);
    if (gate == nullptr) continue;
    const Point where = gate->state.position;
    if (where.x < at.x - kClaimGateReach || where.x > at.x + kClaimGateReach) continue;
    if (where.y < at.y - kClaimGateReach || where.y > at.y + kClaimGateReach) continue;
    const bool enemy = gate_bars(world, *gate, own->owner);
    const bool shut = !gate_lets_through(*gate, now) && !gate->state.flags.gate_open;
    if (!enemy && !shut) continue;
    for (const GateCell& cell : line.cells) out.add(cell.x, cell.y);
  }
  out.seal();
}

void formation_claim_stations(const ObstructionGrid& grid, const CellOverlay& closed,
                              Point centre, std::int32_t radius,
                              std::span<const Point> positions, std::span<Point> stations) {
  if (grid.empty() || stations.empty()) return;
  // The square: the radius rounded up to whole cells, either side of the
  // centre's cell, clipped to the map.
  std::int32_t span = radius;
  if (span % kCollisionCellSize != 0) span = span - span % kCollisionCellSize + kCollisionCellSize;
  span /= kCollisionCellSize;
  const std::int32_t cx = claim_cell(centre.x);
  const std::int32_t cy = claim_cell(centre.y);
  ClaimSquare square;
  square.box = ClaimBox{std::max(cx - span, 0), std::max(cy - span, 0),
                        std::min(cx + span, grid.width() - 1),
                        std::min(cy + span, grid.height() - 1)};
  if (square.box.x0 > square.box.x1 || square.box.y0 > square.box.y1) return;
  square.columns = square.box.x1 - square.box.x0 + 1;
  const std::int32_t rows = square.box.y1 - square.box.y0 + 1;
  square.cells.assign(static_cast<std::size_t>(square.columns) * static_cast<std::size_t>(rows),
                      kClaimNobody);
  for (std::int32_t y = square.box.y0; y <= square.box.y1; ++y) {
    for (std::int32_t x = square.box.x0; x <= square.box.x1; ++x) {
      if (grid.blocked_cell(x, y) || closed.contains(x, y)) square.at(x, y) = kClaimClosed;
    }
  }

  // The flood from the centre's cell, which takes 0 even when closed.
  if (square.holds(cx, cy)) {
    const bool centre_closed = square.at(cx, cy) == kClaimClosed;
    const std::int32_t limit = span << 16;
    std::vector<Point> queue;
    std::size_t head = 0;
    square.at(cx, cy) = 0;
    queue.push_back(Point{cx, cy});
    const auto relax = [&](std::int32_t x, std::int32_t y, std::int32_t d) {
      std::int32_t& cell = square.at(x, y);
      if (d < cell) {
        cell = d;
        queue.push_back(Point{x, y});
      }
    };
    while (head < queue.size()) {
      const Point c = queue[head++];
      const std::int32_t d = square.at(c.x, c.y);
      const std::int32_t straight = d + kClaimStraight;
      const std::int32_t diagonal = d + kClaimDiagonal;
      if (diagonal >= limit) continue;
      const bool above = c.y > square.box.y0;
      const bool below = c.y < square.box.y1;
      if (c.x > square.box.x0) {
        if (above) relax(c.x - 1, c.y - 1, diagonal);
        relax(c.x - 1, c.y, straight);
        if (below) relax(c.x - 1, c.y + 1, diagonal);
      }
      if (above) relax(c.x, c.y - 1, straight);
      if (below) relax(c.x, c.y + 1, straight);
      if (c.x < square.box.x1) {
        if (above) relax(c.x + 1, c.y - 1, diagonal);
        relax(c.x + 1, c.y, straight);
        if (below) relax(c.x + 1, c.y + 1, diagonal);
      }
    }
    if (centre_closed) square.at(cx, cy) = kClaimClosed;
  }
  for (std::int32_t& cell : square.cells) {
    if (cell == kClaimClosed) cell = kClaimNobody;
  }

  // Each member in turn.
  for (std::size_t i = 0; i < stations.size(); ++i) {
    const std::int32_t sx = claim_cell(stations[i].x);
    const std::int32_t sy = claim_cell(stations[i].y);
    if (square.holds(sx, sy) && square.at(sx, sy) != kClaimNobody) {
      square.at(sx, sy) = kClaimNobody;
      continue;
    }
    const ClaimBox between{std::min(cx, sx), std::min(cy, sy), std::max(cx, sx),
                           std::max(cy, sy)};
    Point cell;
    if (square.cheapest(between, sx, sy, cell) == kClaimClosed) {
      // The strips round the box. One with no cell answers the least
      // integer, cheaper than anything: the best becomes the cell the last
      // strip with one wrote, and nothing after beats it.
      const ClaimBox& g = square.box;
      std::int32_t best = kClaimNobody;
      Point written{-7, -7};
      Point chosen{-7, -7};
      const auto strip = [&](ClaimBox area, bool last) {
        const std::int32_t cost = square.cheapest(area, sx, sy, written);
        if (cost < best) {
          chosen = written;
          if (!last) best = cost;
        }
      };
      if (g.x0 < between.x0) strip(ClaimBox{g.x0, g.y0, between.x0 - 1, g.y1}, false);
      if (between.x1 < g.x1) strip(ClaimBox{between.x1 + 1, g.y0, g.x1, g.y1}, false);
      if (g.y0 < between.y0) strip(ClaimBox{between.x0, g.y0, between.x1, between.y0 - 1}, false);
      if (between.y1 < g.y1) strip(ClaimBox{between.x0, between.y1 + 1, between.x1, g.y1}, true);
      if (chosen.x != -7) {
        cell = chosen;
      } else {
        const Point here = i < positions.size() ? positions[i] : stations[i];
        cell = Point{claim_cell(here.x), claim_cell(here.y)};
      }
    }
    stations[i] = Point{cell.x * kCollisionCellSize + kCollisionCellSize / 2,
                        cell.y * kCollisionCellSize + kCollisionCellSize / 2};
  }
}

void formation_place_sample(World& world, Formation& form, const FormationTable& table,
                            Point centre, Point dir, std::vector<ObjectId>& joined) {
  // 0x005f5a20: the hero's own record and the formation's centre take the
  // point, the layout runs if it is marked, every slot is placed.
  form.centre = centre;
  form.dir = dir;
  (void)formation_rebuild(world, form, table, joined);
  for (FormationSlot& slot : form.slots) slot.sample = formation_turn(centre, dir, slot.offset);
  // Then the stations are claimed, with the gates near the hero laid shut
  // for the claim alone (0x005f5b62 .. 0x005f5b85).
  if (MovementSystem* movement = movement_system(world);
      movement != nullptr && !form.slots.empty()) {
    const std::int32_t radius = std::max(
        formation_reach(&form, movement->radius_of(world, form.hero)), kClaimRadiusFloor);
    CellOverlay gates;
    formation_claim_gates(world, form.hero, world.time(), gates);
    std::vector<Point> positions;
    std::vector<Point> stations;
    positions.reserve(form.slots.size());
    stations.reserve(form.slots.size());
    for (const FormationSlot& slot : form.slots) {
      positions.push_back(slot.unit != kNoObject ? world.resolve_position(slot.unit)
                                                 : slot.sample);
      stations.push_back(slot.sample);
    }
    formation_claim_stations(movement->grid(), gates, centre, radius, positions, stations);
    for (std::size_t i = 0; i < form.slots.size(); ++i) form.slots[i].sample = stations[i];
  }
  formation_reassign(world, form);
}

void formation_reassign(World& world, Formation& form) {
  if (!form.reassign || form.slots.empty()) return;
  // 0x005f2850: the members' mean, over the slots' units and the pending
  // ones, divided as integers.
  std::int64_t ux = 0;
  std::int64_t uy = 0;
  std::int64_t members = 0;
  const auto add = [&](ObjectId id) {
    const Point at = world.resolve_position(id);
    ux += at.x;
    uy += at.y;
    ++members;
  };
  for (const FormationSlot& slot : form.slots) add(slot.unit);
  for (const ObjectId id : form.pending) add(id);
  const Point unit_mean{static_cast<std::int32_t>(ux / members),
                        static_cast<std::int32_t>(uy / members)};
  std::int64_t sx = 0;
  std::int64_t sy = 0;
  for (const FormationSlot& slot : form.slots) {
    sx += slot.sample.x;
    sy += slot.sample.y;
  }
  const auto n = static_cast<std::int64_t>(form.slots.size());
  const Point slot_mean{static_cast<std::int32_t>(sx / n), static_cast<std::int32_t>(sy / n)};

  // 0x005f49a0 -> 0x005f3020: each unit, in slot order, takes the free slot
  // of its class nearest it relative to the two means; the first on a tie.
  std::vector<ObjectId> units;
  units.reserve(form.slots.size());
  for (FormationSlot& slot : form.slots) {
    units.push_back(slot.unit);
    slot.unit = kNoObject;
  }
  for (const ObjectId id : units) {
    const ClassIndex klass = class_of(world, id);
    const Point at = world.resolve_position(id);
    const std::int64_t rx = static_cast<std::int64_t>(at.x) - unit_mean.x;
    const std::int64_t ry = static_cast<std::int64_t>(at.y) - unit_mean.y;
    FormationSlot* best = nullptr;
    std::int64_t best_d = 0;
    for (FormationSlot& slot : form.slots) {
      if (slot.unit != kNoObject || slot.klass != klass) continue;
      const std::int64_t dx = rx - (static_cast<std::int64_t>(slot.sample.x) - slot_mean.x);
      const std::int64_t dy = ry - (static_cast<std::int64_t>(slot.sample.y) - slot_mean.y);
      const std::int64_t d = dx * dx + dy * dy;
      if (best == nullptr || d < best_d) {
        best = &slot;
        best_d = d;
      }
    }
    if (best != nullptr) best->unit = id;
  }
  form.reassign = false;
}

std::int32_t formation_reach(const Formation* form, std::int32_t class_radius) noexcept {
  if (form != nullptr && !form->slots.empty() && form->radius != 0) return form->radius;
  return class_radius;
}

// -- the formation's route ------------------------------------------------------

namespace {

/// A 32-bit word as the original's arithmetic leaves it.
[[nodiscard]] std::int32_t wrap32(std::int64_t v) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(v)));
}

/// A leg's length: the integer root of its square, the square an unsigned
/// 32-bit sum as the root is handed it.
[[nodiscard]] std::uint32_t leg_root(Point from, Point to) noexcept {
  const auto dx = static_cast<std::uint32_t>(to.x - from.x);
  const auto dy = static_cast<std::uint32_t>(to.y - from.y);
  return static_cast<std::uint32_t>(isqrt(static_cast<std::uint32_t>(dx * dx + dy * dy)));
}

[[nodiscard]] std::int64_t magnitude(std::int64_t v) noexcept { return v < 0 ? -v : v; }

}  // namespace

void formation_route_lay(FormationRoute& route, std::vector<Point> points, bool complete,
                         std::int32_t stride) {
  route.points = std::move(points);
  route.stride = stride;
  route.leg = 0;
  route.steps = 0;
  route.step = 0;
  route.delta_x = 0;
  route.delta_y = 0;
  route.step_length = wrap32(static_cast<std::int64_t>(route.stride) * 65536);
  std::uint32_t total = 0;
  for (std::size_t i = 0; i + 1 < route.points.size(); ++i) {
    total += leg_root(route.points[i], route.points[i + 1]) * 65536u;
  }
  route.remaining = static_cast<std::int32_t>(total);
  route.complete = complete;
  route.gate_crossings.clear();
}

bool formation_route_step(FormationRoute& route, Point& out) noexcept {
  if (route.remaining == 0) return false;
  const auto count = static_cast<std::int32_t>(route.points.size());
  if (route.step == route.steps) {
    // The leg is spent: the next one is cut into the whole number of strides
    // whose length comes nearest the stride, never fewer than one.
    ++route.leg;
    if (route.leg >= count) return false;
    const Point from = route.points[static_cast<std::size_t>(route.leg - 1)];
    const Point to = route.points[static_cast<std::size_t>(route.leg)];
    const std::uint32_t length = leg_root(from, to);
    const auto stride = static_cast<std::uint32_t>(route.stride);
    const std::uint32_t n = length / stride;
    const std::uint32_t scaled = length * 65536u;
    const std::int64_t target = static_cast<std::int64_t>(stride) * 65536;
    const std::int64_t with_n = n == 0 ? 0 : static_cast<std::int64_t>(scaled / n);
    const std::int64_t with_more = static_cast<std::int64_t>(scaled / (n + 1));
    const bool more = n == 0 || magnitude(wrap32(with_more - target)) <
                                    magnitude(wrap32(with_n - target));
    const auto steps = static_cast<std::int32_t>(more ? n + 1 : n);
    route.steps = steps;
    route.delta_x = wrap32(static_cast<std::int64_t>(to.x - from.x) * 65536) / steps;
    route.delta_y = wrap32(static_cast<std::int64_t>(to.y - from.y) * 65536) / steps;
    route.step_length = wrap32(more ? with_more : with_n);
    route.step = 0;
    // A leg of no length is no step, though the route may go on.
    if (route.delta_x == 0 && route.delta_y == 0) return false;
  }
  const std::int32_t next = route.step + 1;
  if (next == route.steps) {
    out = route.points[static_cast<std::size_t>(route.leg)];
  } else {
    // An arithmetic shift: the offset is floored, not truncated.
    const Point from = route.points[static_cast<std::size_t>(route.leg - 1)];
    const std::int32_t ox = wrap32(static_cast<std::int64_t>(route.delta_x) * next);
    const std::int32_t oy = wrap32(static_cast<std::int64_t>(route.delta_y) * next);
    out = Point{from.x + (ox >> 16), from.y + (oy >> 16)};
  }
  return true;
}

void formation_route_commit(FormationRoute& route) noexcept {
  route.remaining = wrap32(static_cast<std::int64_t>(route.remaining) - route.step_length);
  ++route.step;
  if (route.step == route.steps &&
      route.leg + 1 == static_cast<std::int32_t>(route.points.size())) {
    route.remaining = 0;
  }
}

std::int32_t formation_route_left(const FormationRoute& route) noexcept {
  return static_cast<std::int16_t>(static_cast<std::uint32_t>(route.remaining) / 65536u);
}

}  // namespace imperivm::core::sim
