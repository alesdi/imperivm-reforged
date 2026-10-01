#include "imperivm/core/sim/siege.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/gate.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::Value;

/// The player index the planner refuses to besiege: 0x004384be compares the
/// settlement owner's index against 0xf, which is the sixteenth slot, and
/// **not** the independents (14) that `AttackSetForTraining` tests for at
/// 0x004398d7. Nothing owned by the last player gets catapults.
constexpr PlayerId kLastPlayer = 15;

/// `ObjectState::damage_state` of a building that is broken -- the value
/// `Building::IsBroken` answers true on, and the one 0x0044870d skips a site's
/// building for.
constexpr std::int32_t kBrokenState = 3;

/// The distance the catapult sites keep from the thing they besiege:
/// `range - 128` (0x004484d0).
constexpr std::int32_t kSiteInset = 128;

/// Ten crews is where the planner stops sending the attackers in
/// (0x00438a51).
constexpr std::int32_t kEnoughCrews = 10;

// -- class questions ---------------------------------------------------------

[[nodiscard]] bool is_heir_of(const World& world, const WorldObject& slot, const char* name) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return false;
  const ClassFilter filter = ClassFilter::parse(name, graph);
  return !filter.match_all && world.matches_filter(slot, filter);
}

/// `Unit::HasSpecial(30)`: the `freedom` token of the class's `unit_specials`,
/// matched the way `sim/world_host.cpp`'s `class_has_special` matches --
/// lowercased, spaces to underscores -- so the two readers cannot drift on a
/// token they both accept.
[[nodiscard]] bool has_freedom(const World& world, const WorldObject& slot) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass) return false;
  const std::string_view list = graph->property(slot.class_index, "unit_specials");
  std::size_t at = 0;
  while (at <= list.size()) {
    const std::size_t comma = list.find(',', at);
    std::string_view token =
        list.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
      token.remove_prefix(1);
    }
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
      token.remove_suffix(1);
    }
    std::string normalised;
    normalised.reserve(token.size());
    for (const char c : token) {
      normalised.push_back(c == ' ' ? '_'
                                    : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (normalised == "freedom") return true;
    if (comma == std::string_view::npos) break;
    at = comma + 1;
  }
  return false;
}

/// `projectile_class` declared and not the loader's `**Invalid**` default:
/// what 0x0053f5b0 tests and what 0x0042a530 counts the complement of.
[[nodiscard]] bool is_ranged(const World& world, const WorldObject& slot) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass) return false;
  const std::string_view projectile = graph->property(slot.class_index, "projectile_class");
  return !projectile.empty() && projectile != "**Invalid**";
}

[[nodiscard]] DamageType damage_type_of(const World& world, const WorldObject& slot) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass) return DamageType::none;
  return damage_type_from_name(graph->property(slot.class_index, "damage_type"));
}

[[nodiscard]] std::int32_t race_of(const World& world, const WorldObject& slot) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass) return kNoRace;
  return race_from_name(graph->property(slot.class_index, "race"));
}

/// The settlement an object belongs to, through the object model's own link
/// (`[obj+0x148]` in the original), then the store.
[[nodiscard]] Settlement* settlement_of(World& world, EconomySystem* economy, ObjectId id) {
  if (economy == nullptr) return nullptr;
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->settlement == kNoObject) return nullptr;
  return economy->settlements().for_object(slot->settlement);
}

// -- Unit::CanAttack (vtbl+0xb4, 0x005b1da0) ----------------------------------

/// A hero's ceasefire covers the hero and everyone attached to it
/// (0x005d37f0 resolves a unit to its hero before asking), which is what
/// `HeroSystem::modifiers_for` already resolves for the damage formula.
[[nodiscard]] bool under_ceasefire(const HeroSystem* heroes, ObjectId id, GameTime now) {
  return heroes != nullptr && heroes->modifiers_for(id, now).ceasefire;
}

/// `Unit::CanAttack(target)`, the virtual at `vtbl+0xb4` that
/// `Obj::CanAttack` (0x005ab060) wraps and the planner's split turns on.
///
/// The order is the original's. What it does not model is listed in the
/// header: the per-player visibility mask behind bit 21, the two unit flag
/// bits, the class flag at `+0xb30`, and `[settlement+0x40]`.
[[nodiscard]] bool can_attack(World& world, const HeroSystem* heroes, EconomySystem* economy,
                              const WorldObject& self, const WorldObject& target, GameTime now) {
  if (target.state.health <= 0) return false;
  if (self.state.flags.is_unit) {
    if (!world.players().is_enemy(self.state.owner, target.state.owner)) {
      // Not an enemy: only a training partner or a standing building.
      if (!target.state.flags.training &&
          !(target.state.flags.is_building && target.state.health > 0)) {
        return false;
      }
    }
  }
  if (under_ceasefire(heroes, self.id, now)) return false;
  if (target.state.flags.is_unit) {
    if (under_ceasefire(heroes, target.id, now)) return false;
    if (is_heir_of(world, target, "Sentry") && !is_ranged(world, self)) return false;
    if (is_heir_of(world, self, "RamUnit")) return false;
  }
  {
    const ClassGraph* graph = world.class_graph();
    if (graph != nullptr && target.class_index != kNoClass &&
        graph->at(target.class_index).id == "Fish") {
      return false;
    }
  }
  if (target.state.flags.is_building) {
    const Settlement* set = settlement_of(world, economy, target.id);
    if (set != nullptr) {
      if (is_heir_of(world, target, "FakeTower")) return false;
      if (is_heir_of(world, self, "RamUnit")) {
        const WorldObject* anchor = world.find(set->anchor);
        const bool soft = anchor != nullptr && (is_heir_of(world, *anchor, "Outpost") ||
                                                is_heir_of(world, *anchor, "BaseShipyard"));
        return soft || is_heir_of(world, target, "Gate");
      }
    }
  }
  if (target.state.flags.is_building && is_heir_of(world, target, "Catapult")) {
    for (const char* beast : {"Wolf", "Bear", "Boar", "LionM", "LionF"}) {
      if (is_heir_of(world, self, beast)) return false;
    }
    return true;
  }
  switch (damage_type_of(world, self)) {
    case DamageType::slash:
    case DamageType::pierce:
      return target.state.flags.is_unit || is_ranged(world, self);
    case DamageType::siege:
      return true;
    case DamageType::none:
    default:
      return false;
  }
}

// -- the map and the sites ---------------------------------------------------

struct MapRect {
  bool known = false;
  std::int32_t high = 0;

  [[nodiscard]] bool contains(Point p) const noexcept {
    return !known || (p.x >= 0 && p.x <= high && p.y >= 0 && p.y <= high);
  }
  /// `point::IntoRect`: low edge then high edge, per axis.
  [[nodiscard]] Point clamp(Point p) const noexcept {
    if (!known) return p;
    if (p.x < 0) p.x = 0;
    if (p.x > high) p.x = high;
    if (p.y < 0) p.y = 0;
    if (p.y > high) p.y = high;
    return p;
  }
};

[[nodiscard]] MapRect map_rect_of(World& world) {
  MapRect rect;
  const MatchSystem* match = match_system_of(world);
  if (match != nullptr && match->rules().map_size > 0) {
    rect.known = true;
    rect.high = match->rules().map_size - 1;
  }
  return rect;
}

/// A class property as a non-negative integer; 0 when absent or not a number.
[[nodiscard]] std::int32_t class_property_int(const ClassGraph& graph, ClassIndex index,
                                              std::string_view key) noexcept {
  const std::string_view text = graph.property(index, key);
  std::int32_t value = 0;
  if (text.empty()) return 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return 0;
    value = value * 10 + (c - '0');
  }
  return value;
}

/// Whether a catapult of `catapult_class` can stand at `at`: the footprint
/// proxy the header describes. The cell under the point and its eight
/// neighbours must be free, and no building may stand within the class's
/// `radius` -- the stand-in for the object query the original runs over the
/// rectangle around the point, and what keeps a second engine off the site the
/// first one took, since nothing here stamps a placed building into the grid.
/// No grid, or an empty one, refuses nothing on passability.
[[nodiscard]] bool site_is_free(World& world, Point at, ClassIndex catapult_class) {
  const MovementSystem* movement = movement_system(world);
  if (movement != nullptr && !movement->grid().empty() && !movement->grid().passable_3x3(at)) {
    return false;
  }
  const ClassGraph* graph = world.class_graph();
  const std::int32_t radius =
      graph == nullptr || catapult_class == kNoClass
          ? 0
          : class_property_int(*graph, catapult_class, "radius");
  std::vector<ObjectId> near;
  world.objects_in_radius(at, radius, ClassFilter{}, near);
  for (const ObjectId id : near) {
    const WorldObject* slot = world.find(id);
    if (slot != nullptr && slot->state.flags.is_building) return false;
  }
  return true;
}

struct SiegeSite {
  Point at;
  ObjectId building = kNoObject;
};

/// `round(r * cos(k * 45 degrees))` in integers, for the eight-point circle.
[[nodiscard]] std::int32_t diagonal(std::int32_t r) noexcept {
  // 0.70711 to five places: the double cosine the original multiplies by,
  // rounded to nearest as its `fistp` rounds.
  const std::int64_t scaled = static_cast<std::int64_t>(r) * 70711;
  return static_cast<std::int32_t>((scaled + 50000) / 100000);
}

/// 0x00448220: eight sites on the circle of radius `r` around `centre`, at
/// multiples of 45 degrees from the x axis.
void circle_sites(const MapRect& rect, World& world, Point centre, std::int32_t r,
                  ObjectId building, ClassIndex catapult_class, std::vector<SiegeSite>& out) {
  const std::int32_t d = diagonal(r);
  const Point offsets[8] = {{r, 0}, {d, d}, {0, r}, {-d, d}, {-r, 0}, {-d, -d}, {0, -r}, {d, -d}};
  for (const Point& o : offsets) {
    const Point at{centre.x + o.x, centre.y + o.y};
    if (!rect.contains(at)) continue;
    if (!site_is_free(world, at, catapult_class)) continue;
    out.push_back(SiegeSite{at, building});
  }
}

/// 0x00448310: nine sites along the gate's axis, pushed `r` outward -- away
/// from `inside`, the central building.
void gate_sites(const MapRect& rect, World& world, const WorldObject& gate, Point inside,
                std::int32_t r, ClassIndex catapult_class, std::vector<SiegeSite>& out) {
  Point a{};
  Point b{};
  if (!gate_axis(gate, a, b)) return;
  const std::int64_t dx = static_cast<std::int64_t>(b.x) - a.x;
  const std::int64_t dy = static_cast<std::int64_t>(b.y) - a.y;
  const std::int64_t length = isqrt(dx * dx + dy * dy);
  if (length == 0) return;
  std::int64_t nx = -dy * r / length;
  std::int64_t ny = dx * r / length;
  if ((inside.x - a.x) * nx + (inside.y - a.y) * ny > 0) {
    nx = -nx;
    ny = -ny;
  }
  for (std::int32_t k = 0; k <= 8; ++k) {
    const Point at{static_cast<std::int32_t>(a.x + dx * k / 8 + nx),
                   static_cast<std::int32_t>(a.y + dy * k / 8 + ny)};
    if (!rect.contains(at)) continue;
    if (!site_is_free(world, at, catapult_class)) continue;
    out.push_back(SiegeSite{at, gate.id});
  }
}

/// 0x00448470 then 0x00448660: the settlement's sites for `catapult_class`,
/// and the nearest of those attached to `target` to `from`. `(-1, -1)` is no
/// site.
[[nodiscard]] Point find_site(World& world, const Settlement* set, Point from, ObjectId target,
                              ClassIndex catapult_class) {
  const Point none{-1, -1};
  const ClassGraph* graph = world.class_graph();
  if (set == nullptr || graph == nullptr || catapult_class == kNoClass) return none;
  const WorldObject* target_slot = world.find(target);
  if (target_slot == nullptr || target_slot->state.damage_state == kBrokenState) return none;

  const std::int32_t range = class_property_int(*graph, catapult_class, "range");
  const std::int32_t r = range - kSiteInset;
  const MapRect rect = map_rect_of(world);

  const WorldObject* anchor = world.find(set->anchor);
  const Point centre = anchor == nullptr ? Point{0, 0} : anchor->state.position;
  const bool outpost = anchor != nullptr && is_heir_of(world, *anchor, "Outpost");

  std::vector<ObjectId> roll;
  world.buildings_in_settlement(set->object, ClassFilter{}, roll);
  if (anchor != nullptr && std::find(roll.begin(), roll.end(), set->anchor) == roll.end()) {
    roll.insert(roll.begin(), set->anchor);
  }
  std::vector<SiegeSite> sites;
  for (const ObjectId id : roll) {
    const WorldObject* slot = world.find(id);
    if (slot == nullptr) continue;
    if (outpost && id == set->anchor) {
      circle_sites(rect, world, centre, r, id, catapult_class, sites);
    } else if (is_heir_of(world, *slot, "Gate")) {
      gate_sites(rect, world, *slot, centre, r, catapult_class, sites);
    }
  }

  Point best = none;
  std::int64_t best_distance = 0x7fffffff;
  for (const SiegeSite& site : sites) {
    if (site.building != target) continue;
    const std::int64_t dx = static_cast<std::int64_t>(from.x) - site.at.x;
    const std::int64_t dy = static_cast<std::int64_t>(from.y) - site.at.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    if (distance >= best_distance) continue;
    best_distance = distance;
    best = site.at;
  }
  return best;
}

// -- the plan ------------------------------------------------------------------

struct CrewEntry {
  ObjectId unit = kNoObject;
  ObjectId catapult = kNoObject;
  std::int64_t distance_sq = 0;
  std::size_t order = 0;  ///< list position, the tie-break the header names
};

/// 0x00435ca0: give `wanted` of the unassigned crews, nearest first, to
/// `catapult`. Answers whether any crew is still unassigned.
[[nodiscard]] bool assign_crews(World& world, std::vector<CrewEntry>& plan, ObjectId catapult,
                                std::int32_t wanted) {
  const Point at = world.resolve_position(catapult);
  std::vector<std::size_t> free;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    if (plan[i].catapult != kNoObject) continue;
    const Point here = world.resolve_position(plan[i].unit);
    const std::int64_t dx = static_cast<std::int64_t>(here.x) - at.x;
    const std::int64_t dy = static_cast<std::int64_t>(here.y) - at.y;
    plan[i].distance_sq = dx * dx + dy * dy;
    free.push_back(i);
  }
  std::stable_sort(free.begin(), free.end(), [&](std::size_t l, std::size_t r) {
    return plan[l].distance_sq < plan[r].distance_sq;
  });
  const std::size_t take =
      std::min(free.size(), static_cast<std::size_t>(std::max(wanted, 0)));
  for (std::size_t i = 0; i < take; ++i) plan[free[i]].catapult = catapult;
  return free.size() > take;
}

/// Drop the pending tail, queue `verb`, end the runner: `vtbl+0xc0(0)`, then
/// `vtbl+0xbc(cmd, 0)`, then 0x005b07d0(0). A verb the class does not bind
/// queues nothing, where the original prints and moves on.
[[nodiscard]] bool issue(World& world, CommandSystem& commands, ObjectId unit,
                         std::string_view verb, const Command& prototype) {
  if (commands.script_for(world, unit, verb).empty()) return false;
  (void)commands.add_command(world, unit, /*front=*/false, verb, prototype);
  return true;
}

}  // namespace

SiegeReport run_siege_plan(World& world, std::span<const ObjectId> members, ObjectId target,
                           std::int32_t max_catapults, std::int32_t siege_state, GameTime now) {
  SiegeReport report;
  const std::vector<ObjectId> units(members.begin(), members.end());
  if (units.empty()) return report;
  const WorldObject* first = world.find(units.front());
  const WorldObject* target_slot = world.find(target);
  if (first == nullptr || target_slot == nullptr) return report;
  const ClassGraph* graph = world.class_graph();
  CommandSystem* commands = command_system(world);
  HeroSystem* heroes = hero_system_of(world);
  EconomySystem* economy = economy_of(world);
  if (graph == nullptr || commands == nullptr || heroes == nullptr) return report;

  const PlayerId player = first->state.owner;
  const Point centre = world.resolve_position(first->id);

  // 1. Whether catapults are wanted.
  Settlement* set = target_slot->state.flags.is_building ? settlement_of(world, economy, target)
                                                         : nullptr;
  const bool is_gate = is_heir_of(world, *target_slot, "Gate");
  const bool is_central = set != nullptr && set->anchor == target;
  bool wanted = false;
  if (set != nullptr && set->owner != kLastPlayer) {
    if (is_gate || !is_central) {
      wanted = true;
    } else {
      wanted = set->holder.count() > 0;
    }
  }

  // 2. Crews and attackers.
  std::vector<CrewEntry> plan;
  std::vector<ObjectId> attackers;
  std::map<std::int32_t, std::int32_t> votes;
  std::int64_t damage_sum = 0;
  for (std::size_t i = 0; i < units.size(); ++i) {
    const WorldObject* slot = world.find(units[i]);
    if (slot == nullptr) continue;
    const bool hurts = can_attack(world, heroes, economy, *slot, *target_slot, now);
    const bool crew = wanted && !heroes->is_hero(slot->id) && !hurts &&
                      is_heir_of(world, *slot, "Military") && !has_freedom(world, *slot);
    if (crew) {
      votes[race_of(world, *slot)] += 1;
      plan.push_back(CrewEntry{slot->id, kNoObject, 0, i});
    } else {
      attackers.push_back(slot->id);
      if (hurts) damage_sum += class_int(world, *slot, "damage");
    }
  }

  // 4. The engine, from the majority race.
  std::int32_t race = kNoRace;
  std::int32_t best_votes = -1;
  for (const auto& [candidate, count] : votes) {
    if (count > best_votes) {
      best_votes = count;
      race = candidate;
    }
  }
  if (race == static_cast<std::int32_t>(Race::britain) ||
      race == static_cast<std::int32_t>(Race::germany)) {
    max_catapults = 1;
  }
  const std::string_view catapult_name = catapult_class_for_race(race);
  const ClassIndex catapult_class =
      catapult_name.empty() ? kNoClass : graph->lookup(catapult_name);

  // 3. Reconsidered from the attackers' damage.
  if (wanted) {
    if (is_central) {
      std::int64_t garrison = 0;
      for (const ObjectId member : set->holder.units) {
        const WorldObject* inside = world.find(member);
        if (inside != nullptr) garrison += inside->state.health;
      }
      if (garrison < damage_sum * 5) {
        std::int64_t building = target_slot->state.health;
        const std::int64_t maximum = class_int(world, *target_slot, "maxhealth");
        for (int round = 0; round < 5; ++round) {
          const std::int64_t at_building = maximum == 0 ? 0 : building * damage_sum / maximum;
          const std::int64_t at_garrison = damage_sum - at_building;
          garrison -= at_garrison;
          if (garrison <= 0) {
            wanted = false;
            break;
          }
          building -= std::max<std::int64_t>(at_building, 1);
          if (building < 0) building = 0;
        }
      }
    } else {
      wanted = damage_sum * 5 < target_slot->state.health;
    }
  }
  report.wanted_catapults = wanted;

  // 5 and 6. The engines, and their crews.
  Point rally{-1, -1};
  bool have_rally = false;
  std::int32_t placed = 0;
  std::int32_t inside = 0;
  CombatSystem* combat = combat_system_of(world);
  const auto aimed_at = [&](ObjectId catapult) {
    if (combat == nullptr) return false;
    const Combatant* record = combat->find(catapult);
    return record != nullptr && record->target == target;
  };
  if (wanted && !plan.empty() && economy != nullptr) {
    for (const Settlement& s : economy->settlements().all()) {
      const WorldObject* engine = world.find(s.anchor);
      if (engine == nullptr || !is_heir_of(world, *engine, "Catapult")) continue;
      if (engine->state.owner != player || !aimed_at(engine->id)) continue;
      rally = world.resolve_position(engine->id);
      have_rally = true;
      ++placed;
      const std::int32_t occupied = s.holder.count();
      inside += occupied;
      if (occupied < s.holder.max_units) {
        if (!assign_crews(world, plan, engine->id, s.holder.max_units - occupied)) break;
      }
    }
    if (placed < max_catapults) {
      while (std::any_of(plan.begin(), plan.end(),
                         [](const CrewEntry& e) { return e.catapult == kNoObject; })) {
        // Re-resolved on every pass: placing an engine creates a settlement,
        // and the store's vector can move every record it holds.
        const Point site = find_site(world, settlement_of(world, economy, target), centre,
                                     target, catapult_class);
        if (site.x < 0 || site.y < 0) break;
        const ObjectId engine = place_catapult(world, catapult_class, site, player + 1);
        if (engine == kNoObject) break;
        ++report.placed;
        if (combat != nullptr) {
          (void)combat->reconcile(world);
          (void)combat->order_attack(engine, target);
        }
        rally = site;
        have_rally = true;
        ++placed;
        const Settlement* own = settlement_of(world, economy, engine);
        if (own != nullptr) {
          if (!assign_crews(world, plan, engine, own->holder.max_units)) break;
        }
        if (placed >= max_catapults) break;
      }
    }
  }

  // 7. The orders to the crews (0x00436490).
  std::stable_sort(plan.begin(), plan.end(), [](const CrewEntry& l, const CrewEntry& r) {
    if (l.catapult != r.catapult) return l.catapult < r.catapult;  // kNoObject sorts first
    return l.distance_sq < r.distance_sq;
  });
  std::vector<ObjectId> crews;
  for (const CrewEntry& entry : plan) {
    if (entry.catapult == kNoObject) {
      attackers.push_back(entry.unit);
      continue;
    }
    (void)heroes->detach(entry.unit);
    (void)commands->clear_commands(world, entry.unit);
    Command build;
    build.arg_kind = CommandArgKind::object;
    build.object = entry.catapult;
    if (issue(world, *commands, entry.unit, "build_catapult", build)) ++report.crews;
    (void)commands->kill_command(world, entry.unit);
    crews.push_back(entry.unit);
  }
  if (!crews.empty()) regroup_into_fresh_squads(world, *heroes, crews, siege_state, now);
  report.crews_inside = inside;

  // 8. The attackers.
  bool attack = false;
  if (report.crews + inside < kEnoughCrews) {
    if (!wanted) {
      attack = true;
    } else {
      std::size_t melee = 0;
      for (const ObjectId id : attackers) {
        const WorldObject* slot = world.find(id);
        if (slot != nullptr && !is_ranged(world, *slot)) ++melee;
      }
      attack = melee > attackers.size() / 2;
    }
  }
  report.attacked = attack;

  if (attack) {
    const std::string_view verb = is_gate ? "ai_attack_gate" : "attack";
    for (const ObjectId id : attackers) {
      const WorldObject* slot = world.find(id);
      if (slot == nullptr) continue;
      (void)commands->clear_commands(world, id);
      Command order;
      order.arg_kind = CommandArgKind::object;
      order.object = target;
      if (issue(world, *commands, id, verb, order)) (void)commands->kill_command(world, id);
      if (heroes->is_hero(id)) break;
    }
    return report;
  }

  const MapRect rect = map_rect_of(world);
  const Point target_at = target_slot->state.position;
  const auto push_out = [&](Point from, std::int32_t by) {
    const std::int64_t dx = static_cast<std::int64_t>(from.x) - target_at.x;
    const std::int64_t dy = static_cast<std::int64_t>(from.y) - target_at.y;
    const std::int64_t length = isqrt(dx * dx + dy * dy);
    Point out = from;
    if (length > 0) {
      out.x = static_cast<std::int32_t>(from.x + dx * by / length);
      out.y = static_cast<std::int32_t>(from.y + dy * by / length);
    }
    return rect.clamp(out);
  };
  for (const ObjectId id : attackers) {
    const WorldObject* slot = world.find(id);
    if (slot == nullptr) continue;
    (void)commands->clear_commands(world, id);
    Point at = rally;
    if (!have_rally) {
      at = find_site(world, settlement_of(world, economy, target), world.resolve_position(id),
                     target, catapult_class);
      if (at.x == -1 && at.y == -1) continue;
    }
    const Point stand = push_out(push_out(at, 200), 400);
    Command move;
    move.arg_kind = CommandArgKind::point;
    move.point = stand;
    if (issue(world, *commands, id, "move", move)) {
      (void)issue(world, *commands, id, "idle", Command{});
      (void)commands->kill_command(world, id);
    }
    if (heroes->is_hero(id)) break;
  }
  return report;
}

namespace {

[[nodiscard]] std::int32_t int_arg(CallContext& ctx, std::size_t at) noexcept {
  return ctx.count() > at && ctx.arg(at).is_integer() ? ctx.arg(at).as_integer() : 0;
}

[[nodiscard]] GameTime now_of(CallContext& ctx, World& world) noexcept {
  return ctx.scheduler != nullptr ? ctx.scheduler->now() : world.time();
}

/// `ol.Siege(target, max_cats, nSiegeState)` -- 3 sites. 0x00438f20 unpacks
/// the four values and, for a target that resolves, runs the planner; a
/// target that does not prints `Parameter #1 in function 'ObjList::Siege' is
/// uninitialized or invalid object.` and runs nothing. Void either way.
HostOutcome objlist_siege_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Siege: no World behind CallContext::user");
  if (ctx.count() < 4 || !is_receiver(*world, ctx.arg(0), /*accept_query=*/false)) {
    return HostOutcome::failed("Siege: the receiver is not an object list");
  }
  if (!ctx.arg(1).is_object() || world->find(ctx.arg(1).as_object().id) == nullptr) {
    return HostOutcome::ok_void();
  }
  const std::vector<ObjectId> members = receiver_objects(*world, ctx.arg(0), false);
  (void)run_siege_plan(*world, members, ctx.arg(1).as_object().id, int_arg(ctx, 2),
                       int_arg(ctx, 3), now_of(ctx, *world));
  return HostOutcome::ok_void();
}

/// `squad.Siege(target, max_cats, nSiegeState, nAttackState)` -- 4 sites.
/// 0x00438e60 resolves the squad, refuses one carrying `SF_NOAI`, writes
/// `nAttackState` into its state with the time, and hands its member deque
/// to the planner. An unresolved target prints and stops there, after the
/// state was written.
HostOutcome squad_siege_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Siege: no World behind CallContext::user");
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed("Siege: no HeroSystem on the world");
  if (ctx.count() < 5 || !is_squad(ctx.arg(0))) {
    return HostOutcome::failed("Siege: the receiver is not a squad");
  }
  Squad* squad = heroes->squads().find(unpack_squad(ctx.arg(0)));
  if (squad == nullptr || (squad->flags & kSquadFlagNoAi) != 0) return HostOutcome::ok_void();
  const GameTime now = now_of(ctx, *world);
  squad->state = int_arg(ctx, 4);
  squad->state_time = now;
  if (!ctx.arg(1).is_object() || world->find(ctx.arg(1).as_object().id) == nullptr) {
    return HostOutcome::ok_void();
  }
  const std::vector<ObjectId> members = squad->members;
  (void)run_siege_plan(*world, members, ctx.arg(1).as_object().id, int_arg(ctx, 2),
                       int_arg(ctx, 3), now);
  return HostOutcome::ok_void();
}

}  // namespace

std::size_t register_siege_host(script::HostRegistry& registry) {
  const std::size_t before = registry.implemented();
  registry.define(CallKind::member, "Siege", 3, &objlist_siege_impl);  //  3, the two helpers
  registry.define(CallKind::member, "Siege", 4, &squad_siege_impl);    //  4, the two strategies
  return registry.implemented() - before;
}

}  // namespace imperivm::core::sim
