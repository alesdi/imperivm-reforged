// The `Feeder`: see sim/feeder.hpp for where every number comes from.

#include "imperivm/core/sim/feeder.hpp"

#include <algorithm>

#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

namespace {

/// Decimal, with an optional sign; anything unparseable is the fallback. Local
/// rather than shared for the same reason `combat.cpp` keeps its own: a class
/// property is not an INI value and must not fail loudly when a class simply
/// does not declare it.
[[nodiscard]] std::int32_t to_int(std::string_view text, std::int32_t fallback) noexcept {
  if (text.empty()) return fallback;
  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '-' || text[0] == '+') {
    negative = text[0] == '-';
    i = 1;
  }
  if (i >= text.size()) return fallback;
  std::int64_t value = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return fallback;
    value = value * 10 + (c - '0');
    if (value > 0x7FFFFFFFll) return fallback;
  }
  return static_cast<std::int32_t>(negative ? -value : value);
}

[[nodiscard]] std::string_view class_property(const World& world, ObjectId id,
                                              std::string_view key) noexcept {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return {};
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->class_index == kNoClass) return {};
  return graph->property(slot->class_index, key);
}

}  // namespace

// --------------------------------------------------------------------------
// the chain
// --------------------------------------------------------------------------

FeedingUnit* FeederSystem::find(ObjectId unit) noexcept {
  const auto it = std::lower_bound(
      chain_.begin(), chain_.end(), unit,
      [](const FeedingUnit& link, ObjectId id) { return link.unit < id; });
  if (it == chain_.end() || it->unit != unit) return nullptr;
  return &*it;
}

const FeedingUnit* FeederSystem::find(ObjectId unit) const noexcept {
  return const_cast<FeederSystem*>(this)->find(unit);
}

FeedingUnit& FeederSystem::enrol(const FeedingUnit& link) {
  const auto it = std::lower_bound(
      chain_.begin(), chain_.end(), link.unit,
      [](const FeedingUnit& l, ObjectId id) { return l.unit < id; });
  if (it != chain_.end() && it->unit == link.unit) return *it;
  const auto inserted = chain_.insert(it, link);
  if (inserted->food <= 0) mark_hungry(inserted->unit);
  return *inserted;
}

FeedingUnit& FeederSystem::enrol(World& world, ObjectId unit) {
  if (FeedingUnit* existing = find(unit); existing != nullptr) return *existing;

  FeedingUnit link;
  link.unit = unit;
  link.max_food = to_int(class_property(world, unit, "max_food"), rules_.default_max_food);
  link.feeds = to_int(class_property(world, unit, "feeds"), rules_.default_feeds ? 1 : 0) != 0;
  // The starvation floor needs a ceiling to be a percentage of. `maxhealth` is
  // the class's; a world with no class graph falls back to the health the unit
  // currently has, which for a freshly spawned object is its maximum (see
  // `World::spawn`, which seeds `state.health` from `maxhealth`).
  link.max_health = to_int(class_property(world, unit, "maxhealth"), 0);
  if (link.max_health <= 0) {
    const ObjectState* state = world.state(unit);
    if (state != nullptr) link.max_health = state->health;
  }
  // A unit arrives fed. `OUTPOST_BEHAVIOR.VS` and `TTENT_FEEDING_BEHAVIOR.VS`
  // both open by setting a newly placed garrison to `maxfood`, and a unit that
  // spawned starving would take starvation damage before any script could
  // reach it.
  link.food = link.max_food;
  return enrol(link);
}

bool FeederSystem::remove(ObjectId unit) {
  const auto it = std::lower_bound(
      chain_.begin(), chain_.end(), unit,
      [](const FeedingUnit& link, ObjectId id) { return link.unit < id; });
  if (it == chain_.end() || it->unit != unit) return false;
  chain_.erase(it);
  clear_hungry(unit);
  return true;
}

void FeederSystem::mark_hungry(ObjectId unit) {
  const auto it = std::lower_bound(hungry_.begin(), hungry_.end(), unit);
  if (it != hungry_.end() && *it == unit) return;
  hungry_.insert(it, unit);
}

void FeederSystem::clear_hungry(ObjectId unit) {
  const auto it = std::lower_bound(hungry_.begin(), hungry_.end(), unit);
  if (it != hungry_.end() && *it == unit) hungry_.erase(it);
  if (FeedingUnit* link = find(unit); link != nullptr) link->hungry_since = 0;
}

// --------------------------------------------------------------------------
// per-unit food
// --------------------------------------------------------------------------

bool FeederSystem::has_food_record(ObjectId unit) const noexcept {
  return find(unit) != nullptr;
}

std::int32_t FeederSystem::food(ObjectId unit) const noexcept {
  const FeedingUnit* link = find(unit);
  return link == nullptr ? 0 : link->food;
}

std::int32_t FeederSystem::max_food(ObjectId unit) const noexcept {
  const FeedingUnit* link = find(unit);
  return link == nullptr ? 0 : link->max_food;
}

bool FeederSystem::set_food(ObjectId unit, std::int32_t value) {
  FeedingUnit* link = find(unit);
  if (link == nullptr) return false;
  link->food = std::clamp(value, 0, link->max_food);
  if (link->food > 0) {
    clear_hungry(unit);
  } else {
    mark_hungry(unit);
  }
  return true;
}

bool FeederSystem::write_food(ObjectId unit, std::int32_t value) {
  FeedingUnit* link = find(unit);
  if (link == nullptr) return false;
  link->food = value;
  if (link->food > 0) {
    clear_hungry(unit);
  } else {
    mark_hungry(unit);
  }
  return true;
}

bool FeederSystem::set_feeding(ObjectId unit, bool feeding) {
  FeedingUnit* link = find(unit);
  if (link == nullptr) return false;
  link->feeds = feeding;
  return true;
}

bool FeederSystem::feeding(ObjectId unit) const noexcept {
  const FeedingUnit* link = find(unit);
  return link != nullptr && link->feeds;
}

// --------------------------------------------------------------------------
// the turn
// --------------------------------------------------------------------------

bool FeederSystem::enrol_if_eligible(World& world, ObjectId unit) {
  const WorldObject* slot = world.find(unit);
  if (slot == nullptr) return false;
  if (slot->internal != InternalKind::none) return false;
  if (!slot->state.flags.is_unit) return false;
  if (slot->state.health <= 0) return false;
  if (find(unit) != nullptr) return false;
  enrol(world, unit);
  return true;
}

void FeederSystem::sync_with_world(World& world) {
  // Enrol first, in ascending id order, which is `World::objects()`'s order.
  // The chain is kept sorted by id, so an object enrolled early by
  // `enrol_if_eligible` lands where this loop would have put it.
  for (const WorldObject& slot : world.objects()) {
    enrol_if_eligible(world, slot.id);
  }
  // Then drop what is gone or dead. Two passes rather than one because
  // `enrol` inserts into the very vector the removal walks.
  std::size_t i = 0;
  while (i < chain_.size()) {
    const ObjectId id = chain_[i].unit;
    const WorldObject* slot = world.find(id);
    if (slot == nullptr || !slot->state.flags.is_unit || slot->state.health <= 0) {
      chain_.erase(chain_.begin() + static_cast<std::ptrdiff_t>(i));
      clear_hungry(id);
      continue;
    }
    ++i;
  }
}

void FeederSystem::start(World& world) {
  if (world_bound_) sync_with_world(world);
  if (next_tick_ <= 0) next_tick_ = rules_.feed_quant_interval;
}

void FeederSystem::advance(World& world, const Turn& turn) {
  if (turn.length <= 0) return;
  if (world_bound_) sync_with_world(world);
  if (rules_.feed_quant_interval <= 0) return;
  // A fresh system is one whole interval away from its first step, not zero
  // away: a step at t=0 would make a 10,000 ms span contain 101 steps rather
  // than 100 and put the drain 1 percent out.
  if (next_tick_ <= 0) next_tick_ = rules_.feed_quant_interval;

  // Accumulated, never counted in turns: 200, 400, 799 and 800 all occur as
  // turn lengths, so `next_tick_` carries the remainder across the boundary and
  // any partition of a span produces the same sequence of steps.
  //
  // Each step is given the game time **of that step**, not the time at the end
  // of the turn containing it. `World::time()` has already advanced by the whole
  // turn by the time a system runs, so passing it straight through would make a
  // unit's next search due later in one long turn than in the four short turns
  // covering the same span -- a divergence that only shows up in a partition
  // test, which is exactly the kind this project cannot afford to ship.
  const GameTime base = world.time() - turn.length;
  std::int32_t consumed = 0;
  std::int32_t left = turn.length;
  while (left > 0) {
    if (next_tick_ > left) {
      next_tick_ -= left;
      break;
    }
    left -= next_tick_;
    consumed += next_tick_;
    next_tick_ = rules_.feed_quant_interval;
    step(world, base + consumed);
  }
}

template <typename Apply>
void FeederSystem::walk(ObjectId& cursor, std::int64_t count, Apply apply) {
  if (chain_.empty() || count <= 0) return;
  // More than a full pass in one step means the chain is shorter than the
  // number of visits owed; clamping to one pass keeps a unit from losing two
  // food in one step, which the original's single-linked walk cannot do either.
  const std::int64_t total = static_cast<std::int64_t>(chain_.size());
  if (count > total) count = total;

  auto it = std::lower_bound(
      chain_.begin(), chain_.end(), cursor,
      [](const FeedingUnit& link, ObjectId id) { return link.unit < id; });
  for (std::int64_t n = 0; n < count; ++n) {
    if (it == chain_.end()) it = chain_.begin();
    apply(*it);
    ++it;
  }
  cursor = it == chain_.end() ? 0 : it->unit;
}

void FeederSystem::step(World& world, GameTime now) {
  const std::int64_t count = static_cast<std::int64_t>(chain_.size());

  if (count > 0) {
    // One pass per `DropFoodByOneIvl`, spread over the steps in it. The residue
    // is carried, so the rate is exact for any chain length.
    if (rules_.drop_food_by_one_interval > 0) {
      food_quant_ += count * rules_.feed_quant_interval;
      const std::int64_t due = food_quant_ / rules_.drop_food_by_one_interval;
      food_quant_ %= rules_.drop_food_by_one_interval;
      walk(food_cursor_, due, [&](FeedingUnit& link) { drop_food(link, now); });
    }
    // One pass per `HealthDropIvl`. Food before health at the same instant, for
    // the reason `SettlementTimer`'s order gives: a settlement's inputs land
    // before its outputs, and here the unit's store is the input to the
    // starvation test.
    if (rules_.health_drop_interval > 0) {
      health_quant_ += count * rules_.feed_quant_interval;
      const std::int64_t due = health_quant_ / rules_.health_drop_interval;
      health_quant_ %= rules_.health_drop_interval;
      walk(health_cursor_, due, [&](FeedingUnit& link) { drop_health(world, link); });
    }
  }

  run_searches(world, now);
}

void FeederSystem::drop_food(FeedingUnit& link, GameTime now) {
  if (!link.feeds) return;
  if (link.food <= 0) {
    mark_hungry(link.unit);
    // The tick that finds the unit at zero is the one the original posts
    // `army starving` from; the first such tick is the age the tutorial asks.
    if (link.hungry_since == 0) link.hungry_since = now;
    return;
  }
  link.food -= 1;
  if (link.food <= 0) {
    link.food = 0;
    mark_hungry(link.unit);
    if (link.hungry_since == 0) link.hungry_since = now;
    // The first search is due immediately: the unit has just run out, and
    // `schedule_search` is what puts the ± delay on the *next* one.
    link.search_due = now;
  }
}

void FeederSystem::drop_health(World& world, FeedingUnit& link) {
  if (!link.feeds || link.food > 0) return;
  if (link.max_health <= 0 || rules_.health_decrease_step <= 0) return;
  ObjectState* state = world.mutable_state(link.unit);
  if (state == nullptr) return;
  // `value * percent / 100`, in that order, truncating: the economy's rounding
  // rule, and the only one this project has.
  const std::int32_t floor = static_cast<std::int32_t>(
      static_cast<std::int64_t>(link.max_health) * rules_.health_drop_bound_percent / 100);
  if (state->health <= floor) return;
  state->health = std::max(floor, state->health - rules_.health_decrease_step);
}

void FeederSystem::schedule_search(World& world, FeedingUnit& link, GameTime now) {
  // `FoodSearchFreq = 1000` ± `FoodSearchFreqVar = 1000`.
  //
  // Read as `Freq + rand(Var)`, which is the idiom every shipped behaviour
  // spells out -- `Sleep(rand(nSleepTime) + 1000)` opens
  // `VILLAGE_BEHAVIOR_GIVEFOOD.VS`, and `TOWNHALL_BUILDINGSBEHAVIOR.VS` opens
  // `Sleep(rand(2000) + 500)`. The literal `Freq - Var + rand(2*Var)` reading
  // is rejected because it admits a zero delay, and a hungry unit searching
  // every frame is not a jitter, it is a busy loop. INFERRED either way.
  //
  // The draw is from `World::rng()`. Rule 2: one generator, owned by the world,
  // serialised as world state.
  const std::int32_t jitter =
      rules_.food_search_freq_var > 0 ? world.rng().below(rules_.food_search_freq_var) : 0;
  link.search_due = now + rules_.food_search_freq + jitter;
}

void FeederSystem::run_searches(World& world, GameTime now) {
  if (hungry_.empty()) return;
  EconomySystem* economy = draws_from_warehouse_ ? economy_of(world) : nullptr;

  // A copy, because a successful search clears the unit from `hungry_` and the
  // walk must not be reindexed under itself. Ascending id order is the order
  // food is offered in, and that is state.
  const std::vector<ObjectId> due = hungry_;
  for (const ObjectId id : due) {
    FeedingUnit* link = find(id);
    if (link == nullptr || !link->feeds || link->food > 0) continue;
    if (link->search_due > now) continue;

    if (economy != nullptr) {
      // The engine path: a unit inside a settlement's holder eats that
      // settlement's store. `Unit::GetHolderSett` is the same lookup. A unit in
      // the open is fed by `village_behavior_givefood.vs` through `SetFood`,
      // by a food wagon its squad was sent (below), or not at all.
      const ObjectState* state = world.state(id);
      const std::int32_t want = link->max_food - link->food;
      if (state != nullptr && state->holder != kNoObject) {
        Settlement* s = economy->settlements().for_object(state->holder);
        if (s != nullptr && want > 0) {
          const std::int32_t got = economy->withdraw(s->id, Resource::food, want);
          if (got > 0) {
            link->food += got;
            clear_hungry(id);
            continue;  // fed: no next search to schedule
          }
        }
      }
    }
    // The open field: a unit in no holder searches around itself (0x005dbba0).
    // `want` is the class's food capacity, the unit being at zero.
    if (draws_from_warehouse_) {
      const ObjectState* state = world.state(id);
      const std::int32_t want = link->max_food - link->food;
      if (state != nullptr && state->holder == kNoObject && want > 0) {
        const std::int32_t got = search_open_field(world, economy, id, want);
        // The unit can have been moved by the draw: `find` again.
        if (got > 0) {
          if (FeedingUnit* fed = find(id); fed != nullptr) {
            fed->food += got;
            if (fed->food > 0) clear_hungry(id);
          }
          continue;  // fed: no next search to schedule
        }
      }
    }
    if (FeedingUnit* again = find(id); again != nullptr) schedule_search(world, *again, now);
  }
}

/// The hungry unit's open-field search, 0x005dbba0, with its visitor
/// 0x005d58d0 and its draw 0x005d3700. Three candidates are kept -- a store, a
/// food wagon, a fed unit -- and the draw takes from the first of them that
/// has anything:
///
///   1. **The settlements' buildings.** Every settlement in the store's order
///      whose owner grants the unit's player share-support (bit 2 of the
///      owner's relation word to the unit's player), and which is not a
///      Teuton tent's (0x00440100), offers each of its buildings in its deque's
///      order (`settlement_buildings`) that stands within the radius --
///      `isqrt` of the centre distance less the building's class radius, at
///      most the unit's sight plus 512 -- to the visitor.
///   2. **The grid.** The world's grid over the circle of that radius about
///      the unit, in its walk order (`SweepOrder::grid`), offers every object
///      carrying food (`0x20000000`: a unit with food, a wagon carrying food;
///      a building never carries it) to the visitor.
///
/// The visitor takes an object whose owner the unit's player grants
/// share-support (the mask at `[player+0x1c]`, kept by the relation setter
/// 0x005652f0 from bit 2) and that the unit can see (0x005a7870: a non-zero
/// sight at least the centre distance less both class radii). Then:
///
///   * a **building**: its settlement's store. One holding at least `want`
///     is taken (and ends the grid walk); a smaller one only when it holds
///     strictly more than the store kept so far. In the walk over the
///     settlements nothing ends, so the *last* store holding enough wins;
///   * a **wagon**: when it carries food and strictly more than the wagon kept
///     so far;
///   * a **unit**, until one with enough has been found: alive, holding more
///     than 1. One holding at least `want` is kept and no later unit replaces
///     it; otherwise one holding strictly more than the unit kept so far.
///
/// The draw: the store, when it holds anything (`min(want, food)` out of the
/// warehouse); else the wagon, `min(want, cargo)` through the cargo setter, so
/// a wagon that gives its last food dies; else **half the unit's food** at
/// most, which leaves it at least 1.
///
/// **Not modelled:** the catapult crew's arm (a unit inside a manned
/// catapult's holder searches from the catapult), and 0x00440100's exception
/// for a tent whose holder names an object at `+0xe`: every Teuton tent's
/// settlement is passed over here.
std::int32_t FeederSystem::search_open_field(World& world, EconomySystem* economy, ObjectId unit,
                                             std::int32_t want) {
  const WorldObject* self = world.find(unit);
  if (self == nullptr || want <= 0) return 0;
  const Point at = self->state.position;
  const PlayerId owner = self->state.owner;
  const std::int32_t sight = self->sight;
  const std::int32_t reach = sight + 512;
  const PlayerTable& players = world.players();
  const std::int64_t own_radius = to_int(class_property(world, unit, "radius"), 0);

  const auto distance = [&](Point p) {
    const std::int64_t dx = std::int64_t{p.x} - at.x;
    const std::int64_t dy = std::int64_t{p.y} - at.y;
    return isqrt(dx * dx + dy * dy);
  };
  // The visitor's two gates: the share-support mask and 0x005a7870.
  const auto offered = [&](const WorldObject& other) {
    if (!PlayerTable::is_valid(owner) || !PlayerTable::is_valid(other.state.owner)) return false;
    if (!players.has(owner, other.state.owner, Relation::share_support)) return false;
    if (sight == 0) return false;
    const std::int64_t edge = distance(other.state.position) -
                              to_int(class_property(world, other.id, "radius"), 0) - own_radius;
    return edge <= sight;
  };

  Settlement* store = nullptr;
  ObjectId wagon = kNoObject;
  ObjectId donor = kNoObject;
  bool donor_enough = false;
  const auto cargo_of = [&](ObjectId id) {
    const WorldObject* slot = world.find(id);
    return slot == nullptr ? 0 : slot->state.cargo;
  };
  // 0x005d58d0. Answers false when the walk ends.
  const auto visit = [&](const WorldObject& other) {
    if (!offered(other)) return true;
    if (other.state.flags.is_building) {
      Settlement* s = economy == nullptr ? nullptr : economy->settlements().for_object(other.id);
      if (s == nullptr) return true;
      if (s->warehouse.food < want) {
        if (store == nullptr || s->warehouse.food > store->warehouse.food) store = s;
        return true;
      }
      store = s;
      return false;
    }
    if (other.object != nullptr && other.object->is_a(NativeClass::wagon)) {
      if (other.state.cargo_resource != static_cast<std::int32_t>(Resource::food)) return true;
      if (wagon != kNoObject && other.state.cargo <= cargo_of(wagon)) return true;
      wagon = other.id;
      return true;
    }
    if (!donor_enough && other.state.flags.is_unit && other.state.health > 0) {
      const std::int32_t has = food(other.id);
      if (has > 1) {
        if (has >= want) {
          donor = other.id;
          donor_enough = true;
        }
        if (donor == kNoObject || food(donor) < has) donor = other.id;
      }
    }
    return true;
  };

  // 1. The settlements' buildings.
  if (economy != nullptr) {
    const ClassGraph* graph = world.class_graph();
    const ClassFilter tent = ClassFilter::parse("TTent", graph);
    std::vector<ObjectId> buildings;
    for (Settlement& s : economy->settlements().all()) {
      if (!PlayerTable::is_valid(s.owner) || !PlayerTable::is_valid(owner)) continue;
      if (!players.has(s.owner, owner, Relation::share_support)) continue;
      if (const WorldObject* anchor = world.find(s.anchor);
          anchor != nullptr && !tent.match_all && world.matches_filter(*anchor, tent)) {
        continue;
      }
      settlement_buildings(world, s, buildings);
      for (const ObjectId id : buildings) {
        const WorldObject* building = world.find(id);
        if (building == nullptr) continue;
        const std::int64_t gap =
            distance(building->state.position) - to_int(class_property(world, id, "radius"), 0);
        if (gap > reach) continue;
        (void)visit(*building);
      }
    }
  }

  // 2. The grid, for whatever carries food.
  std::vector<ObjectId> around;
  world.objects_in_radius(at, reach, ClassFilter{}, around, SweepOrder::grid);
  for (const ObjectId id : around) {
    const WorldObject* other = world.find(id);
    if (other == nullptr || id == unit) continue;
    const bool carries =
        (other->object != nullptr && other->object->is_a(NativeClass::wagon))
            ? other->state.cargo > 0 &&
                  other->state.cargo_resource == static_cast<std::int32_t>(Resource::food)
            : (!other->state.flags.is_building && food(id) >= 1);
    if (!carries) continue;
    if (!visit(*other)) break;
  }

  // 0x005d3700.
  if (store != nullptr && store->warehouse.food != 0) {
    return economy->withdraw(store->id, Resource::food, want);
  }
  if (wagon != kNoObject && cargo_of(wagon) != 0) {
    const std::int32_t cargo = cargo_of(wagon);
    const std::int32_t taken = std::min(want, cargo);
    set_wagon_cargo(world, wagon, cargo - taken);
    return taken;
  }
  if (donor != kNoObject) {
    const std::int32_t has = food(donor);
    const std::int32_t taken = std::clamp(want, 0, has / 2);
    (void)write_food(donor, has - taken);
    return taken;
  }
  return 0;
}

// --------------------------------------------------------------------------
// hashing
// --------------------------------------------------------------------------

void FeederSystem::hash(std::uint64_t& accumulator) const {
  constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  const auto mix = [&accumulator](std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      accumulator ^= static_cast<std::uint8_t>(value >> (i * 8));
      accumulator *= kFnvPrime;
    }
  };
  // `unit_chain` and `total_unit_count`. The dumps hash per-unit `food`, so it
  // belongs here; `search_due` is folded in too because two peers that disagree
  // about when a unit next eats will disagree about a warehouse a second later.
  mix(chain_.size());
  for (const FeedingUnit& link : chain_) {
    mix(link.unit);
    mix(static_cast<std::uint32_t>(link.food));
    mix(static_cast<std::uint32_t>(link.max_food));
    mix(static_cast<std::uint32_t>(link.max_health));
    mix(link.feeds ? 1u : 0u);
    mix(static_cast<std::uint64_t>(link.search_due));
    mix(static_cast<std::uint64_t>(link.hungry_since));
  }
  mix(static_cast<std::uint32_t>(next_tick_));
  mix(static_cast<std::uint64_t>(food_quant_));
  mix(static_cast<std::uint64_t>(health_quant_));
  mix(food_cursor_);
  mix(health_cursor_);
}

// --------------------------------------------------------------------------
// host functions
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::CallKind;
using script::HostFn;
using script::HostOutcome;
using script::ObjectRef;
using script::Value;

struct FeederHostDef {
  CallKind kind;
  const char* name;
  std::uint16_t arity;
  HostFn fn;
};

/// The world's feeder, or null. A null `CallContext::user` gives a null world
/// here, which every entry point below turns into `HostOutcome::failed`: this
/// engine's missing wiring, not the script's.
[[nodiscard]] FeederSystem* host_feeder(CallContext& ctx, World*& world) noexcept {
  world = world_of(ctx);
  if (world == nullptr) return nullptr;
  return feeder_of(*world);
}

[[nodiscard]] HostOutcome no_feeder() {
  return HostOutcome::failed("no feeder on the world");
}

/// The unit's chain link, enrolling a unit the reconcile has not reached yet;
/// null for an object that is no living unit.
[[nodiscard]] FeedingUnit* link_of(World& world, FeederSystem& feeder, ObjectId id) {
  (void)feeder.enrol_if_eligible(world, id);
  return feeder.find(id);
}

/// `SetFeeding`'s argument is a **byte** (0x005d7ef0 and 0x00579db0 read the
/// top byte of the stack), so only the low eight bits decide.
[[nodiscard]] bool feeding_arg(CallContext& ctx) noexcept {
  if (ctx.count() < 2 || !ctx.arg(1).is_integer()) return true;
  return (ctx.arg(1).as_integer() & 0xff) != 0;
}

constexpr FeederHostDef kFeederHosts[] = {
    // 11 call sites, all `.AsUnit.maxfood`, and `UNIT.SC.XML` and `HERO.SC.XML`
    // both display `.AsUnit.food + '/' + .AsUnit.maxfood` from it.
    //
    // 0x005d8cb0: a handle naming nothing prints *"The function
    // 'scUnitMaxFood' called for an uninitialized or invalid object."* and
    // answers 0; otherwise it answers the class's `max_food` (`[[obj+0x3c] +
    // 0x2b4]`) -- a class field, so a unit the chain has dropped (a dead one)
    // still answers it. Here: the chain link's, which is the class's read at
    // enrolment, or the class's read now.
    {CallKind::member, "maxfood", 0,
     +[](CallContext& ctx) -> HostOutcome {
       World* world = nullptr;
       FeederSystem* feeder = host_feeder(ctx, world);
       if (feeder == nullptr) return no_feeder();
       if (ctx.count() == 0 || !ctx.arg(0).is_object()) return HostOutcome::ok_with(Value::integer(0));
       const ObjectId id = ctx.arg(0).as_object().id;
       if (ctx.arg(0).as_object().type != kTypeObj || world->find(id) == nullptr) {
         return HostOutcome::ok_with(Value::integer(0));
       }
       if (const FeedingUnit* link = link_of(*world, *feeder, id); link != nullptr) {
         return HostOutcome::ok_with(Value::integer(link->max_food));
       }
       return HostOutcome::ok_with(Value::integer(
           to_int(class_property(*world, id, "max_food"), feeder->rules().default_max_food)));
     }},
    // 14 call sites across five behaviours: `SETTLEMENT_BEHAVIOR_AMBIENT.VS`
    // switches the hens and the ambient peasants off, `TTENT_BEHAVIOR.VS`
    // switches its commanded Teutons back on, `OUTPOST_BEHAVIOR.VS` and
    // `FISH_IDLE.VS` switch off. Every call passes a literal boolean.
    //
    // **Two bodies under one name**, and the handle type picks:
    //
    //   * `Unit::SetFeeding` (0x005d7ef0): a handle naming nothing prints and
    //     does nothing; otherwise bit 0x20000 of the unit's flags word
    //     `[obj+0x194]` is set or cleared, and nothing else is checked -- a
    //     dead unit takes the write, which nothing then reads;
    //   * `Query::SetFeeding` (0x00579db0): a handle naming nothing prints and
    //     does nothing; an empty result prints *"...was called for an empty
    //     query"*; otherwise every member that is a unit (the `CVXUnit` cast)
    //     and not dead (`vtbl+0x50`) gets the same write, and a temporary
    //     handle is released (0x00571680). It walks the query's last result
    //     (`vtbl+0x20`, no refresh); this engine evaluates it now, which is
    //     the same set whenever the query was read on the same instant.
    //
    // The bit is `FeedingUnit::feeds` here. A receiver that is no living unit
    // in the chain was a trap; it is the original's silent no-op now.
    {CallKind::member, "SetFeeding", 1,
     +[](CallContext& ctx) -> HostOutcome {
       World* world = nullptr;
       FeederSystem* feeder = host_feeder(ctx, world);
       if (feeder == nullptr) return no_feeder();
       if (ctx.count() == 0 || !ctx.arg(0).is_object()) return HostOutcome::ok_void();
       const bool feeding = feeding_arg(ctx);
       if (ctx.arg(0).as_object().type == kTypeQuery) {
         for (const ObjectId id : receiver_objects(ctx, *world, ctx.arg(0))) {
           const WorldObject* slot = world->find(id);
           if (slot == nullptr || !slot->state.flags.is_unit || slot->state.health <= 0) continue;
           if (FeedingUnit* link = link_of(*world, *feeder, id); link != nullptr) {
             link->feeds = feeding;
           }
         }
         return HostOutcome::ok_void();
       }
       if (ctx.arg(0).as_object().type != kTypeObj) return HostOutcome::ok_void();
       if (FeedingUnit* link = link_of(*world, *feeder, ctx.arg(0).as_object().id);
           link != nullptr) {
         link->feeds = feeding;
       }
       return HostOutcome::ok_void();
     }},
};

}  // namespace

FeederSystem* feeder_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "feeder") {
      return static_cast<FeederSystem*>(system);
    }
  }
  return nullptr;
}

namespace {

struct NamedFeederConstant {
  std::string_view name;
  std::int32_t FeederRules::*field;
};

constexpr NamedFeederConstant kFeederConstants[] = {
    {"FeedinQuantIvl", &FeederRules::feed_quant_interval},
    {"DropFoodByOneIvl", &FeederRules::drop_food_by_one_interval},
    {"HealthDropIvl", &FeederRules::health_drop_interval},
    {"HealthDecreaseStep", &FeederRules::health_decrease_step},
    {"HealthDropBoundPercent", &FeederRules::health_drop_bound_percent},
    {"FoodSearchFreq", &FeederRules::food_search_freq},
    {"FoodSearchFreqVar", &FeederRules::food_search_freq_var},
    {"FastMetabolismSpeed", &FeederRules::fast_metabolism_speed},
};

}  // namespace

bool feeder_constant(const FeederRules& rules, std::string_view key,
                     std::int32_t& out) noexcept {
  for (const NamedFeederConstant& c : kFeederConstants) {
    if (c.name == key) {
      out = rules.*(c.field);
      return true;
    }
  }
  return false;
}

std::size_t register_feeder_hosts(script::HostRegistry& registry) {
  for (const FeederHostDef& def : kFeederHosts) {
    registry.define(def.kind, def.name, def.arity, def.fn);
  }
  return feeder_host_entry_count();
}

std::size_t feeder_host_entry_count() noexcept {
  return sizeof(kFeederHosts) / sizeof(kFeederHosts[0]);
}

}  // namespace imperivm::core::sim
