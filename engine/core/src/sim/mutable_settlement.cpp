// A `Mutable` settlement made into a race's. See the header for what the
// original does and where this reads it one step short.

#include "imperivm/core/sim/mutable_settlement.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/world/editor.hpp"

namespace imperivm::core::sim {
namespace {

/// The two placeholder classes, by exact name: 0x00552c1b looks both up in
/// the class registry and compares class handles, so a class that merely
/// descends from one is not a placeholder.
constexpr std::string_view kMutableStronghold = "MutableStronghold";
constexpr std::string_view kMutableVillage = "MutableVillage";

/// 0x0058dfc0 collects at most this many matches.
constexpr std::size_t kMatchCap = 64;

/// The first pass converts placeholders owned by players below this; the
/// second everything else (0x00552d4b, 0x00552ee4: `[owner+8] < 8`).
constexpr PlayerId kFirstPassPlayers = 8;

/// `(min + max) / 2` as the original computes it: `cdq; sub eax, edx; sar
/// eax, 1`, which is division truncated towards zero.
[[nodiscard]] std::int32_t midpoint(std::int32_t low, std::int32_t high) noexcept {
  return (low + high) / 2;
}

/// `v` rounded towards zero to a multiple of 128 -- `cdq; and edx, 0x7f; add
/// eax, edx; sar eax, 7; shl eax, 7` (0x0058e4d2).
[[nodiscard]] std::int32_t trunc128(std::int32_t v) noexcept { return (v / 128) * 128; }

/// `v % 128` with the sign of `v` -- the `and 0x8000007f` idiom at 0x0058e4bb.
[[nodiscard]] std::int32_t rem128(std::int32_t v) noexcept { return v % 128; }

/// A placeholder settlement: its record, whether it is a village, and its
/// anchor's position.
struct Placeholder {
  SettlementId id = kNoSettlement;
  bool village = false;
};

/// Every settlement whose anchor is one of the two placeholder classes, in
/// `ID` order. The original walks the object list (0x00552cb8's visitor); by
/// settlement is the same set, since every placeholder is a settlement's
/// first and only building, and `ID` order is the order the nodes over them
/// will be numbered in.
[[nodiscard]] std::vector<Placeholder> find_placeholders(const World& world,
                                                        const EconomySystem& economy) {
  std::vector<Placeholder> out;
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return out;
  const ClassIndex stronghold = graph->lookup(kMutableStronghold);
  const ClassIndex village = graph->lookup(kMutableVillage);
  for (const Settlement& s : economy.settlements().all()) {
    const WorldObject* anchor = s.anchor == kNoObject ? nullptr : world.find(s.anchor);
    if (anchor == nullptr) continue;
    if (anchor->class_index != kNoClass && anchor->class_index == stronghold) {
      out.push_back(Placeholder{s.id, false});
    } else if (anchor->class_index != kNoClass && anchor->class_index == village) {
      out.push_back(Placeholder{s.id, true});
    }
  }
  return out;
}

/// A settlement the first pass converted: where, and to what.
struct Converted {
  Point at;
  std::int32_t race = 0;
};

/// The race of the converted settlement nearest to `at` -- by `isqrt` of the
/// squared distance and strictly-less-replaces, as 0x00552f20 compares -- or
/// `Gaul` when nothing was converted.
[[nodiscard]] std::int32_t nearest_race(std::span<const Converted> converted, Point at) {
  std::int32_t best = 0;
  std::int64_t best_distance = std::numeric_limits<std::int64_t>::max();
  for (const Converted& c : converted) {
    const std::int64_t dx = static_cast<std::int64_t>(c.at.x) - at.x;
    const std::int64_t dy = static_cast<std::int64_t>(c.at.y) - at.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    if (distance < best_distance) {
      best_distance = distance;
      best = c.race;
    }
  }
  return best;
}

/// The owner's race for the first pass: the match's resolved race, else the
/// map's own `race` attribute when it names one. 0x00551970 reads the setup
/// record first and the player table's race second; a player whose race is
/// still unresolved is left for the second pass, which is an inference --
/// the original's second read is of a field this engine has no unresolved
/// value for.
[[nodiscard]] std::int32_t owner_race(const World& world, const MatchSystem& match,
                                      PlayerId owner) {
  if (!PlayerTable::is_valid(owner)) return kNoRace;
  const std::int32_t resolved = match.player(owner).race;
  if (resolved >= 0 && resolved < kRaceCount) return resolved;
  const std::int32_t declared = race_from_name(world.players().setup(owner).race);
  if (declared >= 0 && declared < kRaceCount) return declared;
  return kNoRace;
}

/// Convert one placeholder. False when it was left alone.
bool convert(World& world, EconomySystem& economy, const SettlementTemplateLibrary& library,
             const EntityResolver* entities, std::int32_t map_size, const Placeholder& target,
             std::int32_t race, std::vector<Converted>& converted,
             std::vector<TemplateStamp>& stamps) {
  const ClassGraph& graph = *world.class_graph();
  const std::vector<std::size_t> matches =
      library.matching(settlement_template_prefix(race, target.village));
  if (matches.empty()) return false;
  // The draw happens before anything else is looked at, as 0x0058dfc0's
  // does, so that a template refused below still moved the stream the way
  // the original's would have.
  const std::size_t pick = matches[static_cast<std::size_t>(
      world.rng().below(static_cast<std::int32_t>(matches.size())))];
  const SettlementTemplate& tpl = library.templates()[pick];
  if (graph.lookup(tpl.class_of_first_building) == kNoClass) return false;

  Settlement* old = economy.settlements().find(target.id);
  if (old == nullptr) return false;
  const WorldObject* anchor = world.find(old->anchor);
  if (anchor == nullptr) return false;
  const Point at = anchor->state.position;
  const PlayerId owner = old->owner;
  const ObjectId settlement_object = old->object;

  // What the old record contributes to the new one (0x00551cd5-0x00551d14,
  // read before the members go).
  const std::string name = old->name;
  const std::int32_t population = old->population;
  const std::int32_t max_population = old->max_population;
  const std::int32_t gold = old->warehouse.gold;
  const std::int32_t food = old->warehouse.food;
  old = nullptr;  // `recreate` replaces the row

  // The members, and the aliases that name them. Collected before the
  // despawn because a despawn is a vector erase.
  std::vector<ObjectId> members;
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.settlement != settlement_object) continue;
    members.push_back(slot.id);
  }
  std::vector<std::string> aliases;
  const NamedObjectTable& names = world.named_objects();
  for (std::size_t i = 0; i < names.size(); ++i) {
    const ObjectId bound = names.object(static_cast<std::int32_t>(i));
    if (std::find(members.begin(), members.end(), bound) != members.end()) {
      aliases.emplace_back(names.name(static_cast<std::int32_t>(i)));
    }
  }
  for (const ObjectId id : members) (void)world.despawn(id);

  // The template, around the placeholder's position.
  const Point origin = settlement_template_anchor(tpl, at, map_size);
  ObjectId central = kNoObject;
  std::vector<ObjectId> spawned;
  for (const MapObject& member : tpl.members) {
    const Point where{origin.x + member.x, origin.y + member.y};
    const ObjectId id = world.spawn_map_object(member, where, settlement_object, 0, entities);
    if (id == kNoObject) continue;
    spawned.push_back(id);
    const WorldObject* slot = world.find(id);
    // Buildings take the settlement's owner (0x0058e9a5: only what casts to a
    // building is handed the player); the rest stays unowned, whatever the
    // template's own `player` attribute says.
    if (slot != nullptr && slot->state.flags.is_building) (void)world.set_owner(id, owner);
    if (central == kNoObject && member.class_name == tpl.class_of_first_building) central = id;
  }
  if (central == kNoObject) {
    // The class was known a moment ago; a template whose central building
    // did not spawn is a template with no settlement, and the record is left
    // pointing at nothing rather than at a house.
    return false;
  }

  SettlementInit init;
  init.anchor = central;
  init.owner = owner;
  init.name = name;
  fill_settlement_class_defaults(graph, world.find(central)->class_index, init);
  const auto take = [](std::int32_t authored, std::int32_t& into) {
    if (authored >= 0) into = authored;
  };
  take(tpl.max_gold, init.max_gold);
  take(tpl.max_food, init.max_food);
  take(tpl.extra_sentries, init.extra_sentries);
  // The old record's population pair, verbatim (0x00551e9a), and its gold and
  // food each no higher than the new ceiling (0x00551e3e-0x00551e5e). The
  // clamp is the original's own; `Warehouse::set` would clamp the same way,
  // and the sweep labels the two minima as equivalences for that reason.
  init.population = population;
  init.max_population = max_population;
  init.gold = std::min(gold, init.max_gold);
  init.food = std::min(food, init.max_food);
  if (!economy.recreate(world, target.id, init)) return false;
  // And the rest of the template's buildings into the record's list, in
  // spawn order -- the deque the original's `BestBarrack` walks.
  for (const ObjectId id : spawned) {
    if (id == central) continue;
    const WorldObject* slot = world.find(id);
    if (slot == nullptr || !slot->state.flags.is_building) continue;
    std::int32_t max_health = 0;
    if (slot->class_index != kNoClass) {
      (void)parse_int(graph.property(slot->class_index, "maxhealth"), max_health);
    }
    (void)economy.add_building(world, target.id, id, max_health);
  }

  // The placeholder's alias goes onto the new central building
  // (0x00551e6d-0x00551e95), so `NO_MyTown.obj` keeps resolving.
  for (const std::string& alias : aliases) (void)world.named_objects().rebind(alias, central);

  converted.push_back(Converted{at, race});
  stamps.push_back(TemplateStamp{pick, origin});
  return true;
}

}  // namespace

// --------------------------------------------------------------------------
// the library
// --------------------------------------------------------------------------

Result<SettlementTemplateLibrary> SettlementTemplateLibrary::parse(
    std::span<const std::byte> map_obj_xml) {
  Result<MapObjectList> map = MapObjectList::parse(map_obj_xml);
  if (!map.ok()) return map.error();
  return from_map(map.value());
}

SettlementTemplateLibrary SettlementTemplateLibrary::from_map(const MapObjectList& map) {
  SettlementTemplateLibrary out;
  out.templates_.reserve(map.settlements().size());
  for (std::size_t i = 0; i < map.settlements().size(); ++i) {
    const MapSettlement& declared = map.settlements()[i];
    SettlementTemplate tpl;
    tpl.name = declared.name;
    tpl.class_of_first_building = declared.class_of_first_building;
    tpl.population = declared.population;
    tpl.max_population = declared.max_population;
    tpl.gold = declared.gold;
    tpl.food = declared.food;
    tpl.max_gold = declared.max_gold;
    tpl.max_food = declared.max_food;
    tpl.extra_sentries = declared.extra_sentries;
    // The box over the members' positions; see the header for what the
    // original's box is over. `+1` on the extents as 0x0059253d has it.
    std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
    std::int32_t min_y = min_x;
    std::int32_t max_x = std::numeric_limits<std::int32_t>::min();
    std::int32_t max_y = max_x;
    for (const MapObject& object : map.objects()) {
      if (object.settlement != static_cast<std::int32_t>(i)) continue;
      tpl.members.push_back(object);
      min_x = std::min(min_x, object.x);
      max_x = std::max(max_x, object.x);
      min_y = std::min(min_y, object.y);
      max_y = std::max(max_y, object.y);
    }
    if (!tpl.members.empty()) {
      tpl.centre = Point{midpoint(min_x, max_x), midpoint(min_y, max_y)};
      tpl.width = max_x - min_x + 1;
      tpl.height = max_y - min_y + 1;
      for (MapObject& member : tpl.members) {
        member.x -= tpl.centre.x;
        member.y -= tpl.centre.y;
      }
    }
    out.templates_.push_back(std::move(tpl));
  }
  return out;
}

std::vector<std::size_t> SettlementTemplateLibrary::matching(std::string_view prefix) const {
  std::vector<std::size_t> out;
  if (prefix.empty()) return out;
  for (std::size_t i = 0; i < templates_.size() && out.size() < kMatchCap; ++i) {
    const SettlementTemplate& tpl = templates_[i];
    if (tpl.members.empty()) continue;
    if (std::string_view(tpl.name).starts_with(prefix)) out.push_back(i);
  }
  return out;
}

std::string_view settlement_template_prefix(std::int32_t race, bool village) {
  static constexpr std::string_view kTowns[] = {
      "Gaul Town",       "Republican Roman Town", "Carthaginian Town", "Iberian Town",
      "Imperial Roman Town", "Briton Town",       "Egyptian Town",     "German Town",
  };
  static constexpr std::string_view kVillages[] = {
      "Gaul Village",           "Republican Roman Village", "Carthaginian Village",
      "Iberian Village",        "Imperial Roman Village",   "Briton Village",
      "Egyptian Village",       "German Village",
  };
  if (race < 0 || race >= kRaceCount) return {};
  return village ? kVillages[race] : kTowns[race];
}

Point settlement_template_anchor(const SettlementTemplate& tpl, Point at, std::int32_t map_size) {
  // The template keeps its own offset inside the 128-unit cell, and the
  // placeholder's position is taken to the cell (0x0058e4b8-0x0058e507).
  Point out{trunc128(at.x) + rem128(tpl.centre.x), trunc128(at.y) + rem128(tpl.centre.y)};
  // Then clamped so the whole extent is inside the map (0x0058e509-0x0058e561).
  const std::int32_t half_w = tpl.width / 2;
  const std::int32_t half_h = tpl.height / 2;
  if (out.x < half_w + 1) {
    out.x = half_w + 1;
  } else if (out.x + half_w + 1 >= map_size) {
    out.x = map_size - half_w - 2;
  }
  if (out.y < half_h + 1) {
    out.y = half_h + 1;
  } else if (out.y + half_h + 1 >= map_size) {
    out.y = map_size - half_h - 2;
  }
  return out;
}

// --------------------------------------------------------------------------
// the template's ground
// --------------------------------------------------------------------------

namespace {

constexpr std::int32_t kGroundCell = 64;
constexpr std::int32_t kHeightCell = 32;
constexpr std::uint32_t kUnpainted = 15;

[[nodiscard]] std::int32_t floor_div(std::int32_t a, std::int32_t b) noexcept {
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}

[[nodiscard]] bool in_grid(const Grid& grid, std::int32_t x, std::int32_t y) noexcept {
  return grid.cell_size() != 0 && x >= 0 && y >= 0 && x < static_cast<std::int32_t>(grid.width()) &&
         y < static_cast<std::int32_t>(grid.height());
}

[[nodiscard]] std::uint32_t cell_at(const Grid& grid, std::int32_t x, std::int32_t y) noexcept {
  return grid.cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
}

/// The ceiling of the distance in cells, the way the tools' 21 x 21 table
/// answers it (0x0097d34c): `i^2 + j^2 <= r^2` is within `r`.
[[nodiscard]] bool within(std::int32_t i, std::int32_t j, std::int32_t r) noexcept {
  return i * i + j * j <= r * r;
}

/// 0x005468d0: a deep cell that may no longer be deep, or a cell within
/// four of one that is.
[[nodiscard]] bool shore_here(const Grid& terrain, std::int32_t x, std::int32_t y) noexcept {
  if (cell_at(terrain, x, y) == static_cast<std::uint32_t>(kDeepWaterLayer)) {
    return !edit::deep_water_eligible(terrain, x, y);
  }
  for (std::int32_t j = -4; j <= 4; ++j) {
    for (std::int32_t i = -4; i <= 4; ++i) {
      if (i == 0 && j == 0) continue;
      if (!within(i, j, 4) || !in_grid(terrain, x + i, y + j)) continue;
      if (cell_at(terrain, x + i, y + j) == static_cast<std::uint32_t>(kDeepWaterLayer)) return true;
    }
  }
  return false;
}

/// The height under terrain cell `(cx, cy)` to sea level: its four 32-unit
/// cells, when the cell lies 32 units inside the map (0x00590602), each
/// marked as levelled for the limiter.
std::size_t level_under(const GroundLayers& map, const Grid& terrain, std::int32_t cx, std::int32_t cy) {
  Grid* height = map.height;
  if (height == nullptr || !height->writable() || height->cell_size() == 0) return 0;
  const std::int32_t x = cx * kGroundCell;
  const std::int32_t y = cy * kGroundCell;
  const auto extent_x = static_cast<std::int32_t>(terrain.extent_x());
  const auto extent_y = static_cast<std::int32_t>(terrain.extent_y());
  if (x <= 32 || y <= 32 || x >= extent_x - 33 || y >= extent_y - 33) return 0;
  std::size_t n = 0;
  for (std::int32_t hy = y / kHeightCell; hy < (y + kGroundCell) / kHeightCell; ++hy) {
    for (std::int32_t hx = x / kHeightCell; hx < (x + kGroundCell) / kHeightCell; ++hx) {
      if (!in_grid(*height, hx, hy)) continue;
      (void)height->set_cell(static_cast<std::uint32_t>(hx), static_cast<std::uint32_t>(hy), 0);
      if (map.marks != nullptr) {
        if (map.marks->size() != static_cast<std::size_t>(height->width()) * height->height()) {
          map.marks->assign(static_cast<std::size_t>(height->width()) * height->height(), 0);
        }
        (*map.marks)[static_cast<std::size_t>(hy) * height->width() + static_cast<std::size_t>(hx)] = 1;
      }
      ++n;
    }
  }
  return n;
}

/// The slope limiter over `rect` (world units): 0x00548830 and 0x00548070.
std::size_t limit_slopes(const GroundLayers& map, const edit::WorldRect& rect) {
  Grid* height = map.height;
  if (height == nullptr || !height->writable() || height->cell_size() == 0 || map.marks == nullptr) return 0;
  const auto width = static_cast<std::int32_t>(height->width());
  const auto rows = static_cast<std::int32_t>(height->height());
  if (map.marks->size() != static_cast<std::size_t>(width) * rows) {
    map.marks->assign(static_cast<std::size_t>(width) * rows, 0);
  }
  std::vector<std::uint8_t>& marks = *map.marks;
  const auto mark_at = [&](std::int32_t x, std::int32_t y) -> std::uint8_t& {
    return marks[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
  };
  const auto h = [&](std::int32_t x, std::int32_t y) {
    return static_cast<std::int32_t>(cell_at(*height, x, y));
  };
  constexpr std::int32_t kStep = 20;
  // The queue: every cell of the rectangle grown by 160, in raster order.
  const std::int32_t x0 = std::max(0, floor_div(rect.x0 - 160, kHeightCell));
  const std::int32_t y0 = std::max(0, floor_div(rect.y0 - 160, kHeightCell));
  const std::int32_t x1 = std::min(width - 1, floor_div(rect.x1 + 160, kHeightCell));
  const std::int32_t y1 = std::min(rows - 1, floor_div(rect.y1 + 160, kHeightCell));
  std::vector<std::pair<std::int32_t, std::int32_t>> queue;
  for (std::int32_t y = y0; y <= y1; ++y) {
    for (std::int32_t x = x0; x <= x1; ++x) queue.emplace_back(x, y);
  }
  std::size_t moved = 0;
  static constexpr std::int32_t kAround[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const auto [x, y] = queue[head];
    bool changed = false;
    for (const auto& d : kAround) {
      const std::int32_t nx = x + d[0];
      const std::int32_t ny = y + d[1];
      if (nx < 0 || ny < 0 || nx >= width || ny >= rows) continue;
      const std::int32_t here = h(x, y);
      const std::int32_t there = h(nx, ny);
      if (here - there <= kStep && there - here <= kStep) continue;
      if (mark_at(nx, ny) == 1 && mark_at(x, y) != 1) continue;
      const std::int32_t pulled = here > there ? here - kStep : here + kStep;
      (void)height->set_cell(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny),
                             static_cast<std::uint32_t>(std::clamp(pulled, 0, 255)));
      mark_at(nx, ny) = 0xff;
      queue.emplace_back(nx, ny);
      ++moved;
      changed = true;
    }
    if (changed) queue.emplace_back(x, y);
    // A runaway is impossible -- every move shrinks a difference -- but a
    // bound keeps a corrupt layer from spinning.
    if (queue.size() > static_cast<std::size_t>(width) * rows * 64) break;
  }
  return moved;
}

}  // namespace

TemplateGround stamp_template_ground(const GroundLayers& map, const Grid& template_terrain,
                                     const SettlementTemplate& tpl, Point origin) {
  TemplateGround out;
  if (map.terrain == nullptr || !map.terrain->writable() || map.terrain->cell_size() == 0) return out;
  if (template_terrain.cell_size() == 0 || tpl.members.empty()) return out;
  const Grid& source = template_terrain;

  // The box, in template-map cells: the members' box grown by 256, and
  // every painted cell 4-connected to a painted cell inside it.
  std::int32_t sx0 = std::numeric_limits<std::int32_t>::max();
  std::int32_t sy0 = sx0;
  std::int32_t sx1 = std::numeric_limits<std::int32_t>::min();
  std::int32_t sy1 = sx1;
  for (const MapObject& member : tpl.members) {
    sx0 = std::min(sx0, tpl.centre.x + member.x);
    sy0 = std::min(sy0, tpl.centre.y + member.y);
    sx1 = std::max(sx1, tpl.centre.x + member.x);
    sy1 = std::max(sy1, tpl.centre.y + member.y);
  }
  edit::CellRect box{floor_div(sx0 - 256, kGroundCell), floor_div(sy0 - 256, kGroundCell),
                     floor_div(sx1 + 256, kGroundCell), floor_div(sy1 + 256, kGroundCell)};
  {
    std::vector<std::pair<std::int32_t, std::int32_t>> stack;
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(source.width()) * source.height(), 0);
    const auto visit = [&](std::int32_t x, std::int32_t y) {
      if (!in_grid(source, x, y)) return;
      std::uint8_t& mark = seen[static_cast<std::size_t>(y) * source.width() + static_cast<std::size_t>(x)];
      if (mark != 0 || cell_at(source, x, y) == kUnpainted) return;
      mark = 1;
      stack.emplace_back(x, y);
    };
    for (std::int32_t y = box.y0; y <= box.y1; ++y) {
      for (std::int32_t x = box.x0; x <= box.x1; ++x) visit(x, y);
    }
    while (!stack.empty()) {
      const auto [x, y] = stack.back();
      stack.pop_back();
      box.add(x, y);
      visit(x + 1, y);
      visit(x - 1, y);
      visit(x, y + 1);
      visit(x, y - 1);
    }
  }
  out.box = edit::WorldRect{box.x0 * kGroundCell, box.y0 * kGroundCell, (box.x1 + 1) * kGroundCell - 1,
                            (box.y1 + 1) * kGroundCell - 1};

  // Template cell (tx, ty) lands on map cell (tx + dx, ty + dy).
  const std::int32_t shift_x = origin.x - tpl.centre.x;
  const std::int32_t shift_y = origin.y - tpl.centre.y;
  const auto map_cell_x = [&](std::int32_t tx) { return floor_div(tx * kGroundCell + shift_x, kGroundCell); };
  const auto map_cell_y = [&](std::int32_t ty) { return floor_div(ty * kGroundCell + shift_y, kGroundCell); };

  // The bulldozer: every decoration within 0.6 of the box's diagonal of its
  // centre, the box in map units (0x005904a3-0x005904ef).
  if (map.decor != nullptr && map.decor->writable() && map.decor->cell_size() != 0) {
    const std::int64_t w = out.box.x1 - out.box.x0 + 1;
    const std::int64_t h = out.box.y1 - out.box.y0 + 1;
    std::int64_t diagonal = 0;
    while ((diagonal + 1) * (diagonal + 1) <= w * w + h * h) ++diagonal;
    const std::int32_t radius = static_cast<std::int32_t>(diagonal / 2 + diagonal / 10);
    const std::int32_t centre_x = map_cell_x(box.x0) * kGroundCell + static_cast<std::int32_t>(w / 2);
    const std::int32_t centre_y = map_cell_y(box.y0) * kGroundCell + static_cast<std::int32_t>(h / 2);
    const std::int32_t cx = floor_div(centre_x, kGroundCell);
    const std::int32_t cy = floor_div(centre_y, kGroundCell);
    const std::int32_t r = (radius + kGroundCell - 1) / kGroundCell;
    for (std::int32_t j = -r; j <= r; ++j) {
      for (std::int32_t i = -r; i <= r; ++i) {
        if (i * i + j * j >= r * r) continue;
        if (!in_grid(*map.decor, cx + i, cy + j) || cell_at(*map.decor, cx + i, cy + j) == 0) continue;
        (void)map.decor->set_cell(static_cast<std::uint32_t>(cx + i), static_cast<std::uint32_t>(cy + j), 0);
        ++out.decorations_bulldozed;
      }
    }
  }

  // The copy, and the ground under it to sea level.
  Grid& terrain = *map.terrain;
  for (std::int32_t ty = box.y0; ty <= box.y1; ++ty) {
    for (std::int32_t tx = box.x0; tx <= box.x1; ++tx) {
      if (!in_grid(source, tx, ty)) continue;
      const std::uint32_t value = cell_at(source, tx, ty);
      if (value == kUnpainted) continue;
      const std::int32_t mx = map_cell_x(tx);
      const std::int32_t my = map_cell_y(ty);
      if (!in_grid(terrain, mx, my) || value > terrain.max_cell_value()) continue;
      (void)terrain.set_cell(static_cast<std::uint32_t>(mx), static_cast<std::uint32_t>(my), value);
      ++out.cells_copied;
      out.cells_levelled += level_under(map, terrain, mx, my);
    }
  }

  // The shore in the margin: 512 around the box, where the template paints
  // nothing.
  const std::int32_t margin = 512 / kGroundCell;
  for (std::int32_t ty = box.y0 - margin; ty <= box.y1 + margin; ++ty) {
    for (std::int32_t tx = box.x0 - margin; tx <= box.x1 + margin; ++tx) {
      if (in_grid(source, tx, ty) && cell_at(source, tx, ty) != kUnpainted) continue;
      const std::int32_t mx = map_cell_x(tx);
      const std::int32_t my = map_cell_y(ty);
      if (!in_grid(terrain, mx, my) || !shore_here(terrain, mx, my)) continue;
      (void)terrain.set_cell(static_cast<std::uint32_t>(mx), static_cast<std::uint32_t>(my),
                             static_cast<std::uint32_t>(kShallowWaterLayer));
      ++out.shore_cells;
      out.cells_levelled += level_under(map, terrain, mx, my);
    }
  }
  // The slope limiter over the box grown by 128 (0x005909ee), in map units.
  out.cells_sloped = limit_slopes(
      map, edit::WorldRect{out.box.x0 + shift_x - 128, out.box.y0 + shift_y - 128, out.box.x1 + shift_x + 128,
                           out.box.y1 + shift_y + 128});
  return out;
}

// --------------------------------------------------------------------------
// the materialisation
// --------------------------------------------------------------------------

MaterialiseReport materialise_mutable_settlements(World& world, EconomySystem& economy,
                                                  const MatchSystem& match,
                                                  const SettlementTemplateLibrary& library,
                                                  const EntityResolver* entities) {
  MaterialiseReport report;
  if (library.empty() || world.class_graph() == nullptr) return report;
  const std::vector<Placeholder> placeholders = find_placeholders(world, economy);
  if (placeholders.empty()) return report;
  const std::int32_t map_size = match.rules().map_size;

  std::vector<Converted> converted;
  const auto record = [&](const Placeholder& p, bool done) {
    if (!done) {
      ++report.left;
    } else if (p.village) {
      ++report.villages;
    } else {
      ++report.strongholds;
    }
  };

  // First pass: the placeholders of players 1..8 whose race is known.
  std::vector<Placeholder> later;
  for (const Placeholder& p : placeholders) {
    const Settlement* s = economy.settlements().find(p.id);
    if (s == nullptr) continue;
    const PlayerId owner = s->owner;
    const std::int32_t race =
        owner < kFirstPassPlayers ? owner_race(world, match, owner) : kNoRace;
    if (race == kNoRace) {
      later.push_back(p);
      continue;
    }
    record(p, convert(world, economy, library, entities, map_size, p, race, converted, report.stamps));
  }
  // Second pass: the rest. 0x00552f03 asks the owner's setup record first,
  // as the first pass did, and only a player with no race there takes the
  // nearest of what the *first* pass converted -- the list 0x00552f20 walks
  // is the one 0x00552490 filled there, and nothing the second pass converts
  // is added to it.
  const std::vector<Converted> first_pass = converted;
  for (const Placeholder& p : later) {
    const Settlement* s = economy.settlements().find(p.id);
    if (s == nullptr) continue;
    const WorldObject* anchor = world.find(s->anchor);
    if (anchor == nullptr) continue;
    std::int32_t race = owner_race(world, match, s->owner);
    if (race == kNoRace) race = nearest_race(first_pass, anchor->state.position);
    record(p, convert(world, economy, library, entities, map_size, p, race, converted, report.stamps));
  }
  return report;
}

}  // namespace imperivm::core::sim
