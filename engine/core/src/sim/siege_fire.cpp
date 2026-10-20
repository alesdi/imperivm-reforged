#include "imperivm/core/sim/siege_fire.hpp"

#include <algorithm>
#include <vector>

#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/entrance.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

namespace {

/// The type of entity point a fake tower stands on.
constexpr std::int32_t kTowerPoint = 7;

[[nodiscard]] std::int64_t distance_squared(Point a, Point b) noexcept {
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
  const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
  return dx * dx + dy * dy;
}

[[nodiscard]] bool heir_of(const World& world, ObjectId id, std::string_view name) noexcept {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return false;
  const ClassIndex base = graph->find(name);
  return base != kNoClass && world.class_is_a(id, base);
}

/// The settlement an object belongs to: through its back-link when it has
/// one, else by membership -- a catapult is filed among its own settlement's
/// buildings as the anchor.
[[nodiscard]] Settlement* settlement_of(World& world, ObjectId id) {
  EconomySystem* economy = economy_of(world);
  const WorldObject* slot = world.find(id);
  if (economy == nullptr || slot == nullptr) return nullptr;
  if (slot->settlement != kNoObject) {
    if (Settlement* s = economy->settlements().for_object(slot->settlement)) return s;
  }
  return economy->settlements().for_object(id);
}

/// The walk 0x004e3600, 0x004e3710 and 0x004e37f0 share: `target` into the
/// UI target of every `CatapultTower` heir on the engine's settlement roll,
/// only while `has_towers` is set.
void mirror_to_towers(World& world, ObjectId catapult, ObjectId target) {
  const WorldObject* slot = world.find(catapult);
  if (slot == nullptr || !slot->state.flags.has_towers) return;
  const Settlement* set = settlement_of(world, catapult);
  if (set == nullptr) return;
  // Copied first: the walk writes object state, not the roll, but the roll
  // is the economy's and nothing here should hold a reference into it.
  std::vector<ObjectId> roll;
  for (const SettlementBuilding& b : set->buildings) roll.push_back(b.object);
  for (const ObjectId id : roll) {
    if (!heir_of(world, id, "CatapultTower")) continue;
    if (ObjectState* state = world.mutable_state(id)) state->ui_target = target;
  }
}

[[nodiscard]] const EntityAnim* throw_anim(const World& world, ObjectId catapult) noexcept {
  const WorldObject* slot = world.find(catapult);
  if (slot == nullptr || slot->object == nullptr || slot->object->entity == nullptr) return nullptr;
  return slot->object->entity->anim(kAnimAttack);
}

/// The box sweep both entry points start from, in the grid's walk order --
/// rows of 256-unit cells top to bottom, cells left to right, each cell's list
/// in the order its objects came into it (0x004dc1e0 and 0x004e2760 walk the
/// world's grid; `docs/formats/spatial-index.md`) -- and without anything held
/// or not a combatant.
std::vector<ObjectId> sweep_box(const World& world, const CombatSystem& combat, Point centre,
                                std::int32_t half) {
  std::vector<ObjectId> ids;
  if (half < 0) return ids;
  world.objects_in_rect(centre.x - half, centre.y - half, centre.x + half, centre.y + half,
                        ClassFilter{}, ids, SweepOrder::grid);
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [&](ObjectId id) {
                             const ObjectState* s = world.state(id);
                             return s == nullptr || s->is_held() || combat.find(id) == nullptr;
                           }),
            ids.end());
  return ids;
}

/// 0x005a77b0: centre to centre, less both class radii.
[[nodiscard]] std::int64_t surface_distance(const CombatSystem& combat, const Combatant& a,
                                            Point a_at, const Combatant& b, Point b_at) {
  return isqrt(distance_squared(a_at, b_at)) - combat.profile(a.class_index).radius -
         combat.profile(b.class_index).radius;
}

}  // namespace

Settlement* siege_settlement_of(World& world, ObjectId building) {
  return settlement_of(world, building);
}

bool is_catapult(const World& world, ObjectId id) noexcept {
  const WorldObject* slot = world.find(id);
  return slot != nullptr && slot->object != nullptr && slot->object->is_a(NativeClass::catapult);
}

void catapult_set_target(World& world, ObjectId catapult, ObjectId target) {
  ObjectState* state = world.mutable_state(catapult);
  if (state == nullptr) return;
  state->catapult_target = world.find(target) == nullptr ? kNoObject : target;
  mirror_to_towers(world, catapult, state->catapult_target);
}

void catapult_clear_target(World& world, ObjectId catapult) {
  ObjectState* state = world.mutable_state(catapult);
  if (state == nullptr) return;
  state->catapult_target = kNoObject;
  mirror_to_towers(world, catapult, kNoObject);
}

void catapult_clear_tower_target(World& world, ObjectId catapult) {
  mirror_to_towers(world, catapult, kNoObject);
}

std::int32_t catapult_throw_ms(const World& world, ObjectId catapult) noexcept {
  const EntityAnim* anim = throw_anim(world, catapult);
  return anim == nullptr || anim->duration < 0 ? 0 : anim->duration;
}

std::int32_t catapult_release_ms(const World& world, ObjectId catapult) noexcept {
  const EntityAnim* anim = throw_anim(world, catapult);
  return anim == nullptr || anim->action_time < 0 ? 0 : anim->action_time;
}

bool catapult_point_in_range(const World& world, const CombatSystem& combat, ObjectId catapult,
                             Point point) noexcept {
  const Combatant* self = combat.find(catapult);
  if (self == nullptr || world.find(catapult) == nullptr) return false;
  const CombatProfile mine = combat.profile(self->class_index);
  const std::int64_t d =
      isqrt(distance_squared(world.resolve_position(catapult), point)) - mine.radius;
  return d >= mine.min_range && d <= mine.range;
}

ObjectId catapult_best_target(const World& world, const CombatSystem& combat, ObjectId catapult) {
  const WorldObject* slot = world.find(catapult);
  const Combatant* self = combat.find(catapult);
  if (slot == nullptr || self == nullptr) return kNoObject;
  const CombatProfile mine = combat.profile(self->class_index);
  const Point at = world.resolve_position(catapult);
  const std::int32_t reach = slot->sight + 512;
  // `World::class_graph` and `economy_of` are the world's; the sweep reads
  // the settlement roll only, through a const view.
  World& mutable_world = const_cast<World&>(world);

  ObjectId best_building = kNoObject;
  std::int32_t building_priority = 0;
  ObjectId best_unit = kNoObject;
  std::int32_t unit_priority = 0;
  for (const ObjectId id : sweep_box(world, combat, at, reach)) {
    const ObjectState* state = world.state(id);
    if (distance_squared(at, state->position) >
        static_cast<std::int64_t>(reach) * reach) {
      continue;
    }
    const Combatant* other = combat.find(id);
    if (!state->flags.is_building && !state->flags.is_unit) continue;
    if (!combat.valid_target(*self, *other)) continue;
    const std::int64_t d = surface_distance(combat, *self, at, *other, state->position);
    if (d > slot->sight || d < mine.min_range || d > mine.range) continue;
    const CombatProfile theirs = combat.profile(other->class_index);
    if (state->flags.is_building) {
      if (heir_of(world, id, "Wall")) continue;
      // Nearly destroyed: tier 3 and under a twentieth of its maximum, spared
      // only as a settlement's centre with somebody inside.
      const WorldObject* building = world.find(id);
      const std::int32_t maximum = object_max_health(mutable_world, *building);
      if (state->damage_state == 3 && state->health < maximum / 20) {
        const Settlement* set = settlement_of(mutable_world, id);
        if (set == nullptr || set->anchor != id || set->holder.count() < 1) continue;
      }
      if (best_building == kNoObject || building_priority < theirs.target_priority) {
        best_building = id;
        building_priority = theirs.target_priority;
      }
    } else if (best_unit == kNoObject || unit_priority < theirs.target_priority) {
      best_unit = id;
      unit_priority = theirs.target_priority;
    }
  }
  if (best_building != kNoObject) return best_building;
  return slot->state.flags.has_towers ? best_unit : kNoObject;
}

ObjectId building_best_target(const World& world, CombatSystem& combat, ObjectId building) {
  const WorldObject* slot = world.find(building);
  const Combatant* self = combat.find(building);
  if (slot == nullptr || self == nullptr) return kNoObject;
  const CombatProfile mine = combat.profile(self->class_index);
  const Point at = world.resolve_position(building);
  const GameTime now = world.time();

  ObjectId best = kNoObject;
  std::int64_t best_distance = 0;
  // Set by the first unit taken when its danger is under 4: from then on a
  // far unit must also be no farther than the one held.
  bool low_danger = false;
  for (const ObjectId id : sweep_box(world, combat, at, slot->sight)) {
    const ObjectState* state = world.state(id);
    if (!state->flags.is_unit || state->flags.hidden) continue;
    const Combatant* other = combat.find(id);
    if (!combat.valid_target(*self, *other)) continue;
    if (combat.profile(other->class_index).never_targeted()) continue;
    const std::int64_t d = surface_distance(combat, *self, at, *other, state->position);
    if (d > slot->sight) continue;
    if (best == kNoObject) {
      // The first unit's danger is read (0x005d2800), and decays as it is.
      low_danger = combat.danger(id, now) < 4;
      best = id;
      best_distance = d;
      continue;
    }
    if (d < mine.min_range) continue;
    if (d <= mine.range) {
      // Taken at once, without a read; the sweep ends here.
      best = id;
      break;
    }
    if (combat.danger(id, now) > 3) continue;
    if (low_danger && best_distance < d) continue;
    best = id;
    best_distance = d;
  }
  // 0x004dcd80: the answer is one more search's pick.
  if (best != kNoObject) combat.raise_danger(best, 1, now);
  return best;
}

std::int32_t settlement_fire_rate(const World& world, const Settlement& settlement,
                                  std::int32_t base_rate) {
  std::int64_t military = 0;
  std::int64_t others = 0;
  for (const ObjectId id : settlement.holder.units) {
    if (world.find(id) == nullptr) continue;
    if (heir_of(world, id, "Military")) {
      ++military;
    } else {
      ++others;
    }
  }
  if (others > 10) others = 10;
  return static_cast<std::int32_t>(isqrt(military + others) * base_rate / 100);
}

std::size_t place_fake_towers(World& world, ObjectId object) {
  WorldObject* slot = world.find(object);
  if (slot == nullptr || slot->state.flags.towers_posted) return 0;
  slot->state.flags.towers_posted = true;
  if (slot->object == nullptr) return 0;
  const bool outpost = slot->object->native_class() == NativeClass::outpost;
  const bool catapult = slot->object->native_class() == NativeClass::catapult;
  if (!outpost && !catapult) return 0;
  const Entity* entity = slot->object->entity;
  const ClassGraph* graph = world.class_graph();
  if (entity == nullptr || graph == nullptr) return 0;
  const ClassIndex tower = graph->find(outpost ? "FakeTower" : "CatapultTower");
  if (tower == kNoClass) return 0;

  std::vector<Point> sites;
  for (const EntityPoint& point : entity->points()) {
    if (point.type != kTowerPoint) continue;
    const Point landed = class_point_in_world(world, object, point.x, point.y);
    if (landed != kNoClassPoint) sites.push_back(landed);
  }
  const PlayerId owner = slot->state.owner;
  std::size_t placed = 0;
  for (const Point& site : sites) {
    const ObjectId id = world.spawn_of_class(tower);
    if (id == kNoObject) continue;
    world.set_owner(id, owner);
    world.set_position(id, site);
    if (EconomySystem* economy = economy_of(world); economy != nullptr) {
      if (const Settlement* set = settlement_of(world, object); set != nullptr) {
        const WorldObject* made = world.find(id);
        (void)economy->add_building(world, set->id, id,
                                    made == nullptr ? 0 : made->state.health);
      }
    }
    ++placed;
    if (catapult) {
      if (WorldObject* again = world.find(object)) again->state.flags.has_towers = true;
    }
  }
  return placed;
}

std::size_t post_fake_towers(World& world) {
  std::vector<ObjectId> owed;
  for (const WorldObject& slot : world.objects()) {
    if (slot.state.flags.towers_posted || slot.object == nullptr) continue;
    const NativeClass native = slot.object->native_class();
    if (native != NativeClass::outpost && native != NativeClass::catapult) continue;
    owed.push_back(slot.id);
  }
  std::size_t placed = 0;
  for (const ObjectId id : owed) placed += place_fake_towers(world, id);
  return placed;
}

}  // namespace imperivm::core::sim
