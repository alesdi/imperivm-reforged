#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/game/class_graph.hpp"

#include "imperivm/core/sim/boarding.hpp"

#include "imperivm/core/sim/entrance.hpp"
#include "imperivm/core/sim/gate.hpp"
#include "imperivm/core/sim/lsa.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/orders.hpp"

#include <algorithm>
#include <climits>
#include <string>
#include <string_view>

#include "imperivm/core/formats/ini.hpp"

#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/gaika_table.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/array.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/hooks.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

namespace {

/// The cargo of every selected wagon carrying `wanted`, summed. See
/// `SelectionGold`.
[[nodiscard]] std::int32_t selection_cargo(script::CallContext& ctx, Resource wanted) {
  World* world = world_of(ctx);
  HostContext* host = host_context_of(ctx);
  if (world == nullptr || host == nullptr || host->selections == nullptr ||
      host->local_player == kNoPlayer) {
    return 0;
  }
  std::int64_t total = 0;
  for (const ObjectId id : host->selections->player(host->local_player).ids()) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr) continue;
    if (slot->state.cargo_resource != static_cast<std::int32_t>(wanted)) continue;
    total += slot->state.cargo;
  }
  return static_cast<std::int32_t>(std::clamp<std::int64_t>(total, INT32_MIN, INT32_MAX));
}

/// `value * percent / 100`, in that order, truncating. Every percentage in the
/// economy goes through here so that there is exactly one rounding rule to
/// argue about.
[[nodiscard]] std::int32_t percent_of(std::int32_t value, std::int32_t percent) noexcept {
  const std::int64_t wide = static_cast<std::int64_t>(value) * percent / 100;
  return static_cast<std::int32_t>(wide);
}

}  // namespace

void settlement_production_rates(const EconomyRules& rules, bool produces_gold,
                                 bool produces_food, std::int32_t& gold_rate,
                                 std::int32_t& food_rate) noexcept {
  food_rate = produces_food ? rules.production_food_base : 0;
  if (!produces_gold) {
    gold_rate = 0;
    return;
  }
  // `1 1 24 0` on a town hall, `1 1 12 24` on the German one, which is the only
  // shipped class that sets both flags. See the header: fitted, not explained.
  gold_rate = produces_food ? rules.production_gold_base / 2 : rules.production_gold_base;
}

// --------------------------------------------------------------------------
// creation
// --------------------------------------------------------------------------

SettlementId EconomySystem::create(const SettlementInit& init) {
  const SettlementId id = store_.create(init);
  finish_creation(id, init);
  return id;
}

void EconomySystem::finish_creation(SettlementId id, const SettlementInit& init) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return;

  settlement_production_rates(rules_, init.produces_gold, init.produces_food, s->gold_rate,
                              s->food_rate);

  // The map's own `extrasentries`. `InitialStrongholdSentries` is not added
  // here: `TOWNHALL_SENTRIES_CONTROL.VS`, the behaviour of `BaseTownhall` and
  // its heirs, adds it with `AddMaxSentries` once it runs.
  s->max_sentries = init.extra_sentries;

  s->loyalty = init.can_be_captured ? rules_.loyalty_settlements_initial : 0;
  s->loyalty_decreased = 0;

  for (std::size_t i = 0; i < kSettlementTimerCount; ++i) {
    s->timers[i] = interval(static_cast<SettlementTimer>(i));
  }

  if (stats_.size() < store_.size()) stats_.resize(store_.size());
  stats_[id] = Statistics{};
}

bool EconomySystem::recreate(World& world, SettlementId id, SettlementInit init) {
  const Settlement* old = store_.find(id);
  if (old == nullptr) return false;
  init.settlement_object = old->object;
  init.holder_object = old->holder.object;
  init.warehouse_object = old->warehouse.object;
  if (!store_.replace(id, init)) return false;
  finish_creation(id, init);
  const Settlement* s = store_.find(id);
  for (ObjectId object : {s->object, s->holder.object, s->warehouse.object}) {
    if (WorldObject* wo = world.find(object)) wo->settlement = s->object;
  }
  for (const SettlementBuilding& building : s->buildings) {
    if (WorldObject* wo = world.find(building.object)) wo->settlement = s->object;
  }
  return true;
}

SettlementId EconomySystem::create(World& world, SettlementInit init) {
  if (init.settlement_object == kNoObject) {
    const World::SettlementIds ids = world.spawn_settlement(init.owner);
    init.settlement_object = ids.settlement;
    init.holder_object = ids.holder;
    init.warehouse_object = ids.warehouse;
  }
  const SettlementId id = create(init);
  const Settlement* s = store_.find(id);
  if (s == nullptr) return id;
  const ObjectId owner_object = s->object;
  for (ObjectId object : {s->object, s->holder.object, s->warehouse.object}) {
    if (WorldObject* wo = world.find(object)) wo->settlement = owner_object;
  }
  for (const SettlementBuilding& building : s->buildings) {
    if (WorldObject* wo = world.find(building.object)) wo->settlement = owner_object;
  }
  return id;
}

bool EconomySystem::add_building(World& world, SettlementId id, ObjectId building,
                                 std::int32_t max_health) {
  if (!store_.add_building(id, building, max_health)) return false;
  const Settlement* s = store_.find(id);
  if (s != nullptr) {
    if (WorldObject* wo = world.find(building)) wo->settlement = s->object;
  }
  return true;
}

namespace {

/// Where `Settlement::Units`'s aliased handle points.
///
/// `owner` is the settlement's holder object -- the handle `gbr.exe` reads at
/// `settlement + 0x5e` -- and the list is the vector at `holder + 0x28`, which
/// is `Holder::units` here. Resolved through the store on every access rather
/// than cached, because `SettlementStore::create` appends to a vector and can
/// move every row it holds.
///
/// Null for a settlement that has gone, which the pool reads as an empty list.
std::vector<ObjectId>* resolve_holder_roster(void* user, ObjectId owner) {
  World* world = static_cast<World*>(user);
  if (world == nullptr) return nullptr;
  EconomySystem* economy = economy_of(*world);
  if (economy == nullptr) return nullptr;
  Settlement* settlement = economy->settlements().for_object(owner);
  if (settlement == nullptr || settlement->holder.object != owner) return nullptr;
  return &settlement->holder.units;
}

}  // namespace

void EconomySystem::start(World& world) {
  // The seam `Settlement::Units` needs. Installed here rather than in
  // `sim/objlist.cpp` because the pool sits below this domain and must not
  // learn what a settlement is -- the same rule, and the same shape, as
  // `install_objlist_teardown`.
  objlist_pool_of(world).set_alias_resolver(&resolve_holder_roster, &world);
  if (stats_.size() < store_.size()) stats_.resize(store_.size());
  started_ = true;
}

// --------------------------------------------------------------------------
// the turn
// --------------------------------------------------------------------------

std::int32_t EconomySystem::interval(SettlementTimer which) const noexcept {
  switch (which) {
    case SettlementTimer::production: return rules_.production_interval;
    case SettlementTimer::growth: return rules_.population_growth_interval;
    case SettlementTimer::decrease: return rules_.population_decrease_interval;
    case SettlementTimer::burn: return rules_.burn_time;
    case SettlementTimer::repair: return rules_.repair_interval;
    case SettlementTimer::loyalty: return rules_.loyalty_cap_interval;
    case SettlementTimer::outpost_trade: return rules_.outpost_refresh;
    case SettlementTimer::loan: return rules_.loan_interest_interval;
    case SettlementTimer::count: break;
  }
  return 0;
}

void EconomySystem::advance(World& world, const Turn& turn) {
  if (turn.length <= 0) return;
  if (stats_.size() < store_.size()) stats_.resize(store_.size());

  std::int32_t left = turn.length;
  while (left > 0) {
    // 1. Step to the nearest thing that comes due, or to the end of the turn.
    std::int32_t step = left;
    for (const Settlement& s : store_.all()) {
      for (std::size_t i = 0; i < kSettlementTimerCount; ++i) {
        const std::int32_t remaining = s.timers[i];
        if (remaining > 0 && remaining < step) step = remaining;
      }
    }
    for (const Wagon& w : wagons_) {
      if (w.build_remaining > 0 && w.build_remaining < step) step = w.build_remaining;
    }
    if (step <= 0) step = left;

    // 2. Advance every clock by exactly that much.
    for (Settlement& s : store_.all()) {
      for (std::size_t i = 0; i < kSettlementTimerCount; ++i) {
        if (s.timers[i] > 0) s.timers[i] -= step;
      }
    }
    for (Wagon& w : wagons_) {
      if (w.build_remaining > 0) w.build_remaining -= step;
    }
    left -= step;

    // 3. Wagons that finished building unload first: a delivery is an input to
    //    the settlement it lands in, and inputs precede that settlement's own
    //    timers at the same instant.
    if (auto_deliver_) {
      for (std::size_t i = 0; i < wagons_.size();) {
        // A wagon following a unit has nowhere to unload; it is the squad's
        // larder from here on and the feeder empties it. See `Wagon::follow`.
        if (wagons_[i].build_remaining <= 0 && !wagons_[i].follows()) {
          const std::uint32_t wagon_id = wagons_[i].id;
          deliver_wagon(wagon_id);
          continue;  // deliver_wagon erased it; index i is the next wagon
        }
        ++i;
      }
    }

    // 4. Then every settlement, in `ID` order, and within a settlement every
    //    timer in `SettlementTimer` order. Both orders are world state.
    for (Settlement& s : store_.all()) {
      for (std::size_t i = 0; i < kSettlementTimerCount; ++i) {
        if (s.timers[i] != 0) continue;
        const std::int32_t period = interval(static_cast<SettlementTimer>(i));
        if (period <= 0) continue;
        fire(world, s, static_cast<SettlementTimer>(i));
        s.timers[i] = period;
      }
    }
  }
}

void EconomySystem::fire(World& world, Settlement& s, SettlementTimer which) {
  switch (which) {
    case SettlementTimer::production: produce(world, s); break;
    case SettlementTimer::growth: grow(s); break;
    case SettlementTimer::decrease: trim_population(s); break;
    case SettlementTimer::burn: burn(s); break;
    case SettlementTimer::repair: repair(world, s); break;
    case SettlementTimer::loyalty: tick_loyalty(s); break;
    case SettlementTimer::outpost_trade: tick_outpost_trade(world, s); break;
    case SettlementTimer::loan: tick_loan(s); break;
    case SettlementTimer::count: break;
  }
}

// --------------------------------------------------------------------------
// production
// --------------------------------------------------------------------------

/// `ProductionGoldBase = 24 ;percent from population`, every
/// `ProductionInterval`, scaled by the class's `efficiency` — which is 1 on
/// every producing class and 0 on outposts, shipyards and ruins, so it reads
/// as a multiplier and there is nothing in the shipped range to distinguish it
/// from one.
///
/// As gbr.exe's production tick does (0x005c1122): a settlement owned by
/// player 14 or 15 produces nothing, and food is made only by a population at
/// `MinPopulation` or above. Gold has no such floor.
void EconomySystem::produce(World& world, Settlement& s) {
  if (s.efficiency <= 0 || s.population <= 0) return;
  if (s.owner == kNeutralWildlife || s.owner == kNeutralPassive) return;
  if (s.gold_rate > 0) {
    const std::int32_t got = s.warehouse.store(Resource::gold, percent_of(s.population * s.efficiency, s.gold_rate));
    // The owner's *From taxes* on the end-of-match report; see
    // `PlayerScoreCounters::gold_townhall` for the reading this is.
    if (MatchSystem* match = match_system_of(world); match != nullptr) match->record_gold_produced(s.owner, got);
  }
  if (s.food_rate > 0 && s.population >= rules_.min_population) {
    s.warehouse.store(Resource::food, percent_of(s.population * s.efficiency, s.food_rate));
  }
}

// --------------------------------------------------------------------------
// population
// --------------------------------------------------------------------------

/// `PopulationGrowthRate = 1` every `PopulationGrowthInterval = 20000`, up to
/// `max_population`, read off gbr.exe's growth tick (0x005c0f60): a
/// settlement owned by player 14 or 15 never grows, nor does one below
/// `MinPopulation`, and growth costs nothing.
///
/// This used to charge `foodperpop` per head, an inference from the
/// property's name. The executable holds no such string, no shipped script
/// reads it, and the tick reads no food: the property is data nothing
/// consumes. Charging it starved every town hall the AI fed by wagon alone.
void EconomySystem::grow(Settlement& s) {
  if (s.owner == kNeutralWildlife || s.owner == kNeutralPassive) return;
  if (s.population >= s.max_population) return;
  if (s.population < rules_.min_population) return;
  s.population = std::min(s.population + rules_.population_growth_rate, s.max_population);
}

/// `PopulationDecreasePercent = 10` every `PopulationDecreaseInterval = 4000`,
/// read off gbr.exe's decrease tick (0x005c0fb0): it trims a population
/// *above* `max_population` by that percent of the excess, at least one head,
/// and it reads no food. `max_population` is not a hard ceiling --
/// `TOWNHALL_ADDPOP.VS` adds past it and a refund puts heads back -- and this
/// is what brings an overfull settlement back down to it.
///
/// This used to be starvation: a tenth of the population every four seconds
/// while the store was empty, floored at `MinPopulation`. That trigger was
/// inferred, and it held every computer player's town hall at ten heads
/// within minutes, which is ten heads' worth of gold and no recruits.
void EconomySystem::trim_population(Settlement& s) {
  if (s.population <= s.max_population) return;
  std::int32_t loss = percent_of(s.population - s.max_population, rules_.population_decrease_percent);
  if (loss < 1) loss = 1;
  s.population -= loss;
}

// --------------------------------------------------------------------------
// burning
// --------------------------------------------------------------------------

/// `GoldBurnAmount = 20` and `FoodBurnAmount = 20` every `BurnTime = 500`,
/// while the settlement is burning. What sets it burning is not in the shipped
/// scripts; the flag is exposed so that combat can.
void EconomySystem::burn(Settlement& s) {
  if (!s.burning) return;
  s.warehouse.take(Resource::gold, rules_.gold_burn_amount);
  s.warehouse.take(Resource::food, rules_.food_burn_amount);
}

// --------------------------------------------------------------------------
// repair
// --------------------------------------------------------------------------

/// `PopulationRepairBase = 2` per head, every `RepairInterval = 2000`, applied
/// to one building at a time.
///
/// `first to repair` is the dump's own name for the cursor and its behaviour is
/// the evidence for this shape: it takes eleven values in 0..40 across the
/// corpus and moves during a session, which is what a round-robin index does
/// and what a fixed target does not. That the budget is population-scaled is
/// what the constant's name says; the exact multiplication is INFERRED.
void EconomySystem::repair(World& world, Settlement& s) {
  if (s.buildings.empty() || s.population <= 0) return;
  const std::int32_t budget = s.population * rules_.population_repair_base;
  if (budget <= 0) return;

  const std::size_t count = s.buildings.size();
  const std::size_t cursor = static_cast<std::size_t>(s.first_to_repair) % count;
  for (std::size_t tried = 0; tried < count; ++tried) {
    const std::size_t at = (cursor + tried) % count;
    const SettlementBuilding& building = s.buildings[at];
    const ObjectState* state = world.state(building.object);
    if (state == nullptr || building.max_health <= 0) continue;
    if (state->health >= building.max_health) continue;
    world.set_health(building.object, std::min(state->health + budget, building.max_health));
    s.first_to_repair = static_cast<std::int32_t>((at + 1) % count);
    return;
  }
  s.first_to_repair = static_cast<std::int32_t>((cursor + 1) % count);
}

// --------------------------------------------------------------------------
// loyalty and capture
// --------------------------------------------------------------------------

void EconomySystem::tick_loyalty(Settlement& s) { s.loyalty_decreased = 0; }

// --------------------------------------------------------------------------
// outpost trade
// --------------------------------------------------------------------------

/// `OUTPOST_IDLE.VS`, both branches verbatim, on `OutpostRefresh`. Independent
/// outposts skip it, which is the script's first statement.
void EconomySystem::tick_outpost_trade(World& world, Settlement& s) {
  MatchSystem* match = match_system_of(world);
  if (s.independent()) return;
  switch (s.outpost_trade) {
    case OutpostTrade::none:
      return;
    case OutpostTrade::sell_food:
      if (s.warehouse.food > rules_.outpost_food_sales) {
        s.warehouse.take(Resource::food, rules_.outpost_food_sales);
        const std::int32_t got =
            s.warehouse.store(Resource::gold, rules_.outpost_food_gold_return);
        record_gold_converted(s.id, got);
        if (match != nullptr) match->record_gold_converted(s.owner, got);
      }
      return;
    case OutpostTrade::gold_interest:
      if (s.warehouse.gold >= rules_.outpost_required_gold) {
        const std::int32_t got = s.warehouse.store(Resource::gold, rules_.outpost_gold_return);
        record_gold_converted(s.id, got);
        if (match != nullptr) match->record_gold_converted(s.owner, got);
      }
      return;
  }
}

/// INFERRED. `TimeProductionForLoan = 180000 ;msec` and
/// `LoanInterestPercent = 10` are the only two loan constants no script reads,
/// and a debt with an interest percent and an interval is a debt that accrues.
/// The dumps cannot check it: `loan` is 0 in all 328 settlement blocks.
void EconomySystem::tick_loan(Settlement& s) {
  if (s.loan <= 0) return;
  s.loan += percent_of(s.loan, rules_.loan_interest_percent);
}

// --------------------------------------------------------------------------
// transport
// --------------------------------------------------------------------------

std::uint32_t EconomySystem::create_mule(SettlementId source, SettlementId destination,
                                         Resource r, std::int32_t amount) {
  if (amount <= 0) return 0;
  Settlement* src = store_.find(source);
  if (src == nullptr) return 0;
  const std::int32_t taken = src->warehouse.take(r, amount);
  if (taken <= 0) return 0;

  Wagon w;
  w.id = next_wagon_++;
  w.source = source;
  w.destination = destination;
  w.resource = r;
  w.amount = taken;
  w.build_remaining = rules_.wagon_build_time;
  wagons_.push_back(w);
  return w.id;
}

std::uint32_t EconomySystem::create_feeding_mule(SettlementId source, ObjectId follow,
                                                 std::int32_t amount) {
  if (follow == kNoObject) return 0;
  const std::uint32_t id = create_mule(source, kNoSettlement, Resource::food, amount);
  if (id == 0) return 0;
  wagons_.back().follow = follow;
  return id;
}

std::int32_t EconomySystem::draw_from_larder(std::span<const ObjectId> members,
                                             std::int32_t want) {
  if (want <= 0) return 0;
  for (std::size_t i = 0; i < wagons_.size(); ++i) {
    Wagon& w = wagons_[i];
    // The resource test is an invariant guard: `create_feeding_mule` is the
    // one writer of `follow` and loads food only. Kept so the larder stays a
    // food store if a gold one is ever made to follow; the sweep cannot tell.
    if (!w.follows() || !w.ready() || w.resource != Resource::food || w.amount <= 0) continue;
    if (std::find(members.begin(), members.end(), w.follow) == members.end()) continue;
    const std::int32_t taken = std::min(want, w.amount);
    w.amount -= taken;
    if (w.amount == 0) wagons_.erase(wagons_.begin() + static_cast<std::ptrdiff_t>(i));
    return taken;
  }
  return 0;
}

const Wagon* EconomySystem::find_wagon(std::uint32_t wagon) const noexcept {
  for (const Wagon& w : wagons_) {
    if (w.id == wagon) return &w;
  }
  return nullptr;
}

std::int32_t EconomySystem::deliver_wagon(std::uint32_t wagon) {
  for (std::size_t i = 0; i < wagons_.size(); ++i) {
    if (wagons_[i].id != wagon) continue;
    const Wagon w = wagons_[i];
    wagons_.erase(wagons_.begin() + static_cast<std::ptrdiff_t>(i));
    Settlement* dst = store_.find(w.destination);
    if (dst == nullptr) return 0;
    return dst->warehouse.store(w.resource, w.amount);
  }
  return 0;
}

bool EconomySystem::lose_wagon(std::uint32_t wagon) {
  for (std::size_t i = 0; i < wagons_.size(); ++i) {
    if (wagons_[i].id != wagon) continue;
    wagons_.erase(wagons_.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
  }
  return false;
}

bool EconomySystem::set_supplied(SettlementId id, SettlementId target) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->supplied = (target == id) ? kNoSettlement : target;
  return true;
}

// --------------------------------------------------------------------------
// resources
// --------------------------------------------------------------------------

std::int32_t EconomySystem::resource(SettlementId id, Resource r) const noexcept {
  const Settlement* s = store_.find(id);
  return s == nullptr ? 0 : s->warehouse.amount(r);
}

bool EconomySystem::set_resource(SettlementId id, Resource r, std::int32_t value) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->warehouse.set(r, value);
  return true;
}

std::int32_t EconomySystem::deposit(SettlementId id, Resource r, std::int32_t amount) {
  Settlement* s = store_.find(id);
  return s == nullptr ? 0 : s->warehouse.store(r, amount);
}

std::int32_t EconomySystem::withdraw(SettlementId id, Resource r, std::int32_t amount) {
  Settlement* s = store_.find(id);
  return s == nullptr ? 0 : s->warehouse.take(r, amount);
}

/// The shape of `VERIFY_CMDCOST_BUILDING.VS` and its four siblings:
///
///     (cmdcost_gold > settlement.gold) || (cmdcost_food > settlement.food) ||
///     (cmdcost_pop > 0 && cmdcost_pop + MinPopulation > settlement.population)
///
/// Note that the population clause only bites when the cost is positive: a
/// settlement already below `MinPopulation` can still buy things that cost no
/// population.
SpendResult EconomySystem::can_afford(SettlementId id, std::int32_t gold, std::int32_t food,
                                      std::int32_t population) const {
  const Settlement* s = store_.find(id);
  if (s == nullptr) return SpendResult::no_settlement;
  if (gold > s->warehouse.gold) return SpendResult::not_enough_gold;
  if (food > s->warehouse.food) return SpendResult::not_enough_food;
  if (population > 0 && population + rules_.min_population > s->population) {
    return SpendResult::not_enough_population;
  }
  return SpendResult::ok;
}

SpendResult EconomySystem::spend(SettlementId id, std::int32_t gold, std::int32_t food,
                                 std::int32_t population) {
  const SpendResult verdict = can_afford(id, gold, food, population);
  if (verdict != SpendResult::ok) return verdict;
  Settlement* s = store_.find(id);
  if (gold > 0) s->warehouse.take(Resource::gold, gold);
  if (food > 0) s->warehouse.take(Resource::food, food);
  if (population > 0) s->population -= population;
  return SpendResult::ok;
}

// --------------------------------------------------------------------------
// population
// --------------------------------------------------------------------------

bool EconomySystem::set_population(SettlementId id, std::int32_t value) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->population = std::max(0, std::min(value, s->max_population));
  return true;
}

bool EconomySystem::add_max_population(SettlementId id, std::int32_t delta) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->max_population = std::max(0, s->max_population + delta);
  return true;
}

bool EconomySystem::add_population(SettlementId id, std::int32_t delta) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->population = std::max(0, s->population + delta);
  return true;
}

// --------------------------------------------------------------------------
// capture and loyalty
// --------------------------------------------------------------------------

bool EconomySystem::is_valid_capture_target(const World& world, SettlementId id) const {
  const Settlement* s = store_.find(id);
  if (s == nullptr || !s->can_be_captured) return false;
  if (s->capture_health_percent >= 100) return true;
  if (s->anchor_max_health <= 0) return true;
  const ObjectState* state = world.state(s->anchor);
  if (state == nullptr) return false;
  return state->health * 100 <= s->anchor_max_health * s->capture_health_percent;
}

std::int32_t EconomySystem::decrease_loyalty(SettlementId id, std::int32_t amount) {
  Settlement* s = store_.find(id);
  if (s == nullptr || amount <= 0 || s->loyalty <= 0) return 0;
  std::int32_t allowed = amount;
  if (rules_.loyalty_change_cap > 0) {
    allowed = std::min(allowed, rules_.loyalty_change_cap - s->loyalty_decreased);
  }
  allowed = std::min(allowed, s->loyalty);
  if (allowed <= 0) return 0;
  s->loyalty -= allowed;
  s->loyalty_decreased += allowed;
  return allowed;
}

bool EconomySystem::set_loyalty(SettlementId id, std::int32_t value) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->loyalty = std::max(0, value);
  return true;
}

/// `UNIT_CAPTURE.VS`:
///
///     if (b.settlement.loyalty == 0) {
///         b.settlement.SetPlayer(.player);
///         b.settlement.SetLoyalty(11);
///     }
bool EconomySystem::capture(SettlementId id, PlayerId new_owner) {
  Settlement* s = store_.find(id);
  if (s == nullptr || !s->can_be_captured) return false;
  if (s->loyalty != 0) return false;
  s->owner = new_owner;
  s->loyalty = rules_.loyalty_after_capture;
  s->loyalty_decreased = 0;
  return true;
}

bool EconomySystem::set_owner(SettlementId id, PlayerId new_owner) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->owner = new_owner;
  return true;
}

bool EconomySystem::garrison_add(SettlementId id, ObjectId unit) {
  Settlement* s = store_.find(id);
  if (s == nullptr || !s->holder.add(unit)) return false;
  s->loyalty += rules_.loyalty_increase_per_unit;
  return true;
}

bool EconomySystem::garrison_force_add(SettlementId id, ObjectId unit) {
  Settlement* s = store_.find(id);
  if (s == nullptr || !s->holder.force_add(unit)) return false;
  s->loyalty += rules_.loyalty_increase_per_unit;
  return true;
}

bool EconomySystem::garrison_remove(SettlementId id, ObjectId unit) {
  Settlement* s = store_.find(id);
  if (s == nullptr || !s->holder.remove(unit)) return false;
  s->loyalty = std::max(0, s->loyalty - rules_.loyalty_increase_per_unit);
  return true;
}

bool garrison_enter(World& world, SettlementId id, ObjectId unit, bool force) {
  EconomySystem* economy = economy_of(world);
  if (economy == nullptr) return false;
  Settlement* s = economy->settlements().find(id);
  if (s == nullptr) return false;
  // Out of the holder it was in first (0x005d3e28-0x005d3e56), which for a
  // unit already on this roster is this one: the add below then re-admits it.
  (void)garrison_forget(world, unit);
  const bool admitted =
      force ? economy->garrison_force_add(id, unit) : economy->garrison_add(id, unit);
  if (!admitted) return false;
  if (s->holder.object == kNoObject || world.find(unit) == nullptr) return true;
  // What 0x005d3e8d-0x005d3ef9 does once the holder has taken it: its timers
  // and action cleared, its path dropped, and its position set to (-1, -1)
  // with the holder recorded.
  if (MovementSystem* movement = movement_system(world); movement != nullptr) {
    movement->stop(world, unit);
  }
  (void)world.put_in_holder(unit, s->holder.object);
  if (CombatSystem* combat = combat_system_of(world); combat != nullptr) {
    (void)combat->stop(unit);
    // Combat keeps its own copy of the position, refreshed at the top of its
    // turn; a garrison that stood on the map until then could still be struck.
    combat->set_position(unit, kHeldPosition);
  }
  return true;
}

bool garrison_forget(World& world, ObjectId unit) {
  EconomySystem* economy = economy_of(world);
  if (economy == nullptr) return false;
  bool held = false;
  for (Settlement& s : economy->settlements().all()) {
    if (!s.holder.contains(unit)) continue;
    held = true;
    (void)economy->garrison_remove(s.id, unit);
  }
  return held;
}

std::int32_t garrison_exit(World& world, ObjectId unit, Point toward, GameTime now,
                           bool throttled) {
  const WorldObject* slot = world.find(unit);
  EconomySystem* economy = economy_of(world);
  if (slot == nullptr || economy == nullptr || !slot->state.is_held()) return 0;
  Settlement* s = economy->settlements().for_object(slot->state.holder);
  if (s == nullptr || s->holder.object != slot->state.holder) return 0;

  if (throttled && s->last_unit_exit_time + s->exit_interval > now) {
    return static_cast<std::int32_t>(s->exit_interval - s->last_unit_exit_time + now);
  }

  if (FeederSystem* feeder = feeder_of(world);
      feeder != nullptr && feeder->has_food_record(unit)) {
    const std::int32_t food = feeder->food(unit);
    const std::int32_t want = feeder->max_food(unit) - food;
    const std::int32_t got = want > 0 ? economy->withdraw(s->id, Resource::food, want) : 0;
    (void)feeder->set_food(unit, food + got);
  }

  Point at = toward;
  if (s->anchor != kNoObject && world.find(s->anchor) != nullptr) {
    MovementSystem* movement = movement_system(world);
    std::vector<Point> doors;
    const bool water = slot->object != nullptr && slot->object->is_a(NativeClass::ship);
    building_doors(world, movement == nullptr ? nullptr : &movement->grid(), s->anchor,
                   kExitPointType, water, doors);
    const Point chosen = doors.empty() ? world.resolve_position(s->anchor)
                                       : pick_door_by_distance(doors, toward, world.rng());
    if (chosen != kNoClassPoint) at = chosen;
  }
  (void)world.remove_from_holder(unit, at);
  if (CombatSystem* combat = combat_system_of(world); combat != nullptr) {
    combat->set_position(unit, at);
  }
  (void)economy->garrison_remove(s->id, unit);
  s->last_unit_exit_time = now;
  return 0;
}

// --------------------------------------------------------------------------
// sentries and burning
// --------------------------------------------------------------------------

bool EconomySystem::add_max_sentries(SettlementId id, std::int32_t count) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->max_sentries = std::max(0, s->max_sentries + count);
  return true;
}

bool EconomySystem::add_sentries(SettlementId id, std::int32_t count) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  // 0x005c4ac0 raises the roster **and** the wall pool by the same amount, so
  // sentries that arrive arrive ready. A negative count takes from both.
  s->sentries = std::max(0, s->sentries + count);
  s->sentries_ready = std::max(0, s->sentries_ready + count);
  return true;
}

bool EconomySystem::set_burning(SettlementId id, bool burning) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->burning = burning;
  return true;
}

// --------------------------------------------------------------------------
// tavern
// --------------------------------------------------------------------------

bool EconomySystem::take_loan(SettlementId id) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->loan = rules_.loan_amount;
  s->warehouse.store(Resource::gold, rules_.loan_amount);
  return true;
}

bool EconomySystem::repay_loan(SettlementId id) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  if (s->warehouse.gold > s->loan) {
    s->warehouse.take(Resource::gold, s->loan);
    s->loan = 0;
  } else {
    s->loan -= s->warehouse.gold;
    s->warehouse.set(Resource::gold, 0);
  }
  return true;
}

bool EconomySystem::invest(SettlementId id) {
  if (spend(id, rules_.investment_cost, 0, 0) != SpendResult::ok) return false;
  const std::int32_t got = deposit(id, Resource::gold, rules_.investment_revenue);
  record_gold_converted(id, got - rules_.investment_cost);
  return true;
}

bool EconomySystem::buy_slaves(SettlementId id) {
  if (spend(id, rules_.slavery_gold, 0, 0) != SpendResult::ok) return false;
  Settlement* s = store_.find(id);
  s->population = s->max_population;
  return true;
}

bool EconomySystem::buy_food(SettlementId id, std::int32_t food) {
  Settlement* s = store_.find(id);
  if (s == nullptr) return false;
  s->warehouse.store(Resource::food, food);
  return true;
}

std::int32_t EconomySystem::deliver_spoils(SettlementId id, std::int32_t item_count) {
  if (item_count <= 0) return 0;
  const std::int32_t got =
      deposit(id, Resource::gold, item_count * rules_.spoils_of_war_gold);
  record_gold_converted(id, got);
  return got;
}

// --------------------------------------------------------------------------
// statistics
// --------------------------------------------------------------------------

void EconomySystem::record_gold_converted(SettlementId id, std::int32_t amount) {
  if (id >= stats_.size()) stats_.resize(store_.size());
  if (id >= stats_.size()) return;
  stats_[id].gold_converted += amount;
}

std::int32_t EconomySystem::gold_converted(SettlementId id) const {
  if (id >= stats_.size()) return 0;
  return stats_[id].gold_converted;
}

// --------------------------------------------------------------------------
// hashing
// --------------------------------------------------------------------------

void EconomySystem::hash(std::uint64_t& accumulator) const {
  constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  const auto mix = [&accumulator](std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      accumulator ^= static_cast<std::uint8_t>(value >> (i * 8));
      accumulator *= kFnvPrime;
    }
  };
  mix(store_.hash());
  // Wagons in flight are world state: two peers that disagree about a wagon
  // disagree about where a thousand food is going to land.
  for (const Wagon& w : wagons_) {
    mix(w.id);
    mix(w.source);
    mix(w.destination);
    mix(static_cast<std::uint64_t>(w.resource));
    mix(static_cast<std::uint32_t>(w.amount));
    mix(static_cast<std::uint32_t>(w.build_remaining));
    mix(w.object);
    mix(w.follow);
  }
  mix(next_wagon_);
}

// --------------------------------------------------------------------------
// host functions
// --------------------------------------------------------------------------
//
// Ordered by call frequency in docs/formats/vs-host-api.md, which is the order
// in which they pay for themselves. Members take the receiver as argument 0;
// the declared arity does not count it.
//
// A receiver that is not a settlement is an **error**, not a zero. Several of
// these names are shared with other domains -- `food` is read on `Unit` and
// `Squad`, `SetGold` is called on a `Wagon`, `Units` belongs to `Squad` too --
// and dispatch is on (kind, name, arity) with no receiver type, so those
// domains will widen these entry points when they arrive. Until they do, a
// script that reads a unit's food gets a trap naming the call rather than a
// settlement's number.

namespace {

using script::CallContext;
using script::CallKind;
using script::HostFn;
using script::HostOutcome;
using script::ObjectRef;
using script::Value;

/// `CallContext::user` is the `World*`, which is the contract `world_host.hpp`
/// sets and every domain shares. The economy is a system on that world, so it
/// is reached through the run-order list rather than through a second pointer:
/// one `user` convention, no way for two domains to disagree about it.
[[nodiscard]] EconomySystem* host_economy(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  return world == nullptr ? nullptr : economy_of(*world);
}

/// Resolve argument `index` as a settlement.
///
/// A settlement handle is `(kTypeSettlement, settlement object id)` -- the same
/// representation `WorldHost::m_settlement` hands out, so `.settlement.gold`
/// works across the seam without either side knowing about the other. An
/// ordinary object handle resolves through the world's back-link, and falls
/// back to the store's own scan for an object the loader has not linked.
[[nodiscard]] Settlement* settlement_at(CallContext& ctx, std::size_t index) noexcept {
  EconomySystem* economy = host_economy(ctx);
  if (economy == nullptr || ctx.count() <= index) return nullptr;
  const Value& value = ctx.arg(index);
  if (!value.is_object()) return nullptr;
  const ObjectRef ref = value.as_object();
  if (ref.type == kTypeSettlement) return economy->settlements().for_object(ref.id);
  if (ref.type != kTypeObj) return nullptr;
  World* world = world_of(ctx);
  if (world != nullptr) {
    const WorldObject* slot = world->find(ref.id);
    if (slot != nullptr && slot->settlement != kNoObject) {
      Settlement* s = economy->settlements().for_object(slot->settlement);
      if (s != nullptr) return s;
    }
  }
  return economy->settlements().for_object(ref.id);
}

[[nodiscard]] Settlement* receiver(CallContext& ctx) noexcept { return settlement_at(ctx, 0); }

/// Resolve argument 0 as a link of the feeder's `unit_chain`, or null.
///
/// `food` and `SetFood` are read on a `Unit` as well as on a `Settlement` and
/// the host table has no receiver type, so these two entry points try the
/// settlement first -- which is what 60 of `food`'s 109 sites and 18 of
/// `SetFood`'s 32 mean -- and fall through to here. A null `CallContext::user`
/// gives a null world and therefore a null link, so the caller fails rather
/// than dereferences.
[[nodiscard]] FeedingUnit* feeding_unit(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() == 0) return nullptr;
  const Value& value = ctx.arg(0);
  if (!value.is_object()) return nullptr;
  const ObjectRef ref = value.as_object();
  if (ref.type != kTypeObj) return nullptr;
  FeederSystem* feeder = feeder_of(*world);
  return feeder == nullptr ? nullptr : feeder->find(ref.id);
}

[[nodiscard]] std::int32_t int_arg(CallContext& ctx, std::size_t index) noexcept {
  if (ctx.count() <= index) return 0;
  const Value& value = ctx.arg(index);
  return value.is_integer() ? value.as_integer() : 0;
}

/// `LoadGold`/`LoadFood`. See the note at the registrations for the order the
/// original applies its four steps in.
template <Resource kKind>
HostOutcome load_cargo(CallContext& ctx) {
  World* world = world_of(ctx);
  EconomySystem* economy = host_economy(ctx);
  if (world == nullptr || economy == nullptr) return HostOutcome::failed("Load: no world");
  if (ctx.count() < 2 || !ctx.arg(0).is_object()) return HostOutcome::ok_void();
  WorldObject* mule = world->find(ctx.arg(0).as_object().id);
  if (mule == nullptr) return HostOutcome::ok_void();

  // 1. The settlement the mule is inside, by `GetHolderSett`'s own two hops.
  if (mule->state.holder == kNoObject) return HostOutcome::ok_void();
  Settlement* here = economy->settlements().for_object(mule->state.holder);
  if (here == nullptr) return HostOutcome::ok_void();

  // 2. Empty, or already carrying this kind.
  const std::int32_t kind = static_cast<std::int32_t>(kKind);
  if (mule->state.cargo != 0 && mule->state.cargo_resource != kind) {
    return HostOutcome::ok_void();
  }

  // 3. What is left of `max_load`, and never less than nothing.
  const std::int32_t capacity = class_int(*world, *mule, "max_load");
  std::int32_t want = int_arg(ctx, 1);
  const std::int32_t room = capacity - mule->state.cargo;
  // The negative clamp is the original's own step (0x005ebf83) and is kept as
  // that rather than as a guarantee: with the early return below, a negative
  // request loads nothing either way, and injecting a fault into this line
  // alone changes no test. `GetHolderSett`'s redundant holder check is kept on
  // the same footing.
  if (want < 0) want = 0;
  if (want > room) want = room;

  // The kind lands before the warehouse is asked, which is the original's
  // order and is observable: a mule that asked and got nothing still reports
  // the kind it asked for.
  mule->state.cargo_resource = kind;
  if (want <= 0) return HostOutcome::ok_void();

  // 4. Whatever the warehouse can pay.
  mule->state.cargo += here->warehouse.take(kKind, want);
  return HostOutcome::ok_void();
}

/// A receiver that names no settlement: **the type's zero, not a refusal.**
///
/// Every `Settlement::` member in `gbr.exe` opens the same way -- resolve the
/// handle through the object table, and when nothing comes back print `The
/// function 'settlement::<name>' called for an uninitialized or invalid
/// object.` (0x007cdc50, 0x007cdd58, one string per member) and push the
/// zero of the return type: `Settlement::health` (0x005c1e63) pushes 0,
/// `Settlement::IsFull` (0x005c1d43) pushes false, `Settlement::IsStronghold`
/// (0x0042508b) pushes false without even the print. This used to refuse,
/// on the argument that a silent zero was a divergence no test could see --
/// and the trap sweep saw it the first time the AI ran: `g.settlement.
/// IsStronghold` on a node with no settlement is the second line of
/// `GetArmyNeed.vs`, `CalcGAIKAPriority.vs` and `GS_KillEnemies.vs`, and
/// every one of them stopped there. 0 reads as `false`, as 0 and as nothing.
[[nodiscard]] HostOutcome no_settlement(CallContext& ctx) {
  // No world behind the call at all is still a refusal: that is a script
  // running outside an embedder, and every domain refuses it rather than
  // dereferencing. The zero is for a *handle* that names nothing.
  if (world_of(ctx) == nullptr) return HostOutcome::failed("settlement: no world behind CallContext::user");
  return HostOutcome::ok_with(Value::integer(0));
}

/// `Settlement::BestToSupply`'s body (0x0042c800), shared with `SupplyCount`:
/// the **nearest stronghold of the same owner in the same area**, or nothing.
///
/// The original resolves the receiver's own GAIKA node and takes the u16 at
/// `[node + 0x10]` -- `GAIKA::LSA`, the same word `GetDistToPlayers` compares
/// to decide whether to charge a crossing. If that is zero it answers nothing
/// at once. Otherwise it walks the world's settlement vector and keeps the
/// least straight-line distance between settlement centres among those that
/// pass three tests:
///
///   * `[s + 0x90] == [self + 0x90]` -- the same *player record*, so the same
///     owner exactly. Not an ally, not a shared-view partner;
///   * the central building is a heir of `BaseTownhall`, which is what
///     `SettlementKind::stronghold` already means here. `ES_VILLAGE.VS` says
///     so in a comment on the call: *"the return value is always
///     stronghold"*;
///   * the same `LSA`. A stronghold across water is not a supply destination,
///     and this is the whole reason the entry point exists rather than the
///     callers writing `NearestStronghold`.
///
/// **The comparison is by area, not by node**, and that matters here more
/// than anywhere else in this family. `sim/gaika.hpp` records that this
/// engine's node partition is an approximation -- one node per settlement --
/// while the LSA partition is measured and exact. Had the original compared
/// node identity, no two settlements would ever share one here and this would
/// answer nothing on every map; it compares the area, so the approximation
/// does not reach it.
///
/// **The receiver is not excluded**, and that is the original's. A stronghold
/// asking this of itself gets itself back, at distance zero. Neither shipped
/// `BestToSupply` caller can hit it -- one is a village and the other an
/// outpost, and both fail the town-hall test -- so the branch is reproduced
/// rather than guarded.
[[nodiscard]] const Settlement* supply_target(World& world, EconomySystem& economy,
                                              const Settlement& self) {
  const GaikaTable& table = world.gaika();
  const GaikaNode* mine = table.find(table.for_settlement(self.object));
  // `edi == 0` at 0x0042c82e: no node, or an area of zero, answers nothing.
  if (mine == nullptr || mine->lsa == kNoLsa) return nullptr;
  const Point from = world.resolve_position(self.anchor);

  const Settlement* best = nullptr;
  std::int64_t best_distance = 0;
  for (const Settlement& other : economy.settlements().all()) {
    if (other.owner != self.owner) continue;
    if (!other.is_stronghold()) continue;
    const GaikaNode* theirs = table.find(table.for_settlement(other.object));
    if (theirs == nullptr || theirs->lsa != mine->lsa) continue;
    const Point at = world.resolve_position(other.anchor);
    const std::int64_t dx = at.x - from.x;
    const std::int64_t dy = at.y - from.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    // Strictly less: a tie keeps the earlier settlement, which is store order
    // and therefore the map document's.
    if (best == nullptr || distance < best_distance) {
      best = &other;
      best_distance = distance;
    }
  }
  return best;
}

[[nodiscard]] HostOutcome integer(std::int32_t value) {
  return HostOutcome::ok_with(Value::integer(value));
}

[[nodiscard]] HostOutcome boolean(bool value) {
  return HostOutcome::ok_with(Value::boolean(value));
}

/// A unit has just been accepted into `s`'s garrison: run the class's
/// `onenter` hook on it. Nothing when the world is not reachable, when the
/// settlement has no handle of its own, or when the unit's class binds no
/// `onenter` -- which is every class but `Unit`.
///
/// **Handing the script the holder instead would read identically, and no test
/// here can separate the two.** `SettlementStore::for_object` maps all four
/// handles of a composite to one row, so `sett.gold` and every other member the
/// script could ask answers the same either way. The settlement's own handle is
/// passed because that is what the original resolves -- `0x005d3e10` follows
/// `[holder+0xc]`, the holder's back-reference to the composite -- and the
/// equivalence is recorded rather than relied on, since it is a property of
/// this engine's settlement store and not of the entry point.
void enter_settlement(CallContext& ctx, const Settlement& s, ObjectId unit) {
  World* world = world_of(ctx);
  if (world == nullptr || s.object == kNoObject) return;
  (void)fire_class_hook(*world, ClassHook::on_enter, unit, s.object);
}

/// An invalid handle: `type == kNoType`, which is what `.IsValid` reports on.
[[nodiscard]] Value invalid_handle() { return Value::object(ObjectRef{}); }

[[nodiscard]] Value settlement_handle(const Settlement& s) {
  if (s.object == kNoObject) return invalid_handle();
  return Value::object(kTypeSettlement, s.object);
}

[[nodiscard]] Value object_handle(ObjectId id) {
  if (id == kNoObject) return invalid_handle();
  return Value::object(kTypeObj, id);
}

/// `CreateBoatGold(n)` / `CreateBoatFood(n)`. See the note at the registrations.
template <Resource kKind>
HostOutcome create_boat(CallContext& ctx) {
  Settlement* s = receiver(ctx);
  World* world = world_of(ctx);
  const auto none = HostOutcome::ok_with(object_handle(kNoObject));
  if (s == nullptr || world == nullptr) return none;
  const ClassGraph* graph = world->class_graph();
  if (graph == nullptr) return none;
  const ClassIndex boat = graph->lookup("ShipS");
  if (boat == kNoClass) return none;

  // The class's `max_load` first, then whatever the warehouse can pay --
  // `Warehouse::take` is exactly the second clamp and the deduction in one.
  std::int32_t want = int_arg(ctx, 1);
  // `max_load` off the class the boat *will* be, resolved before it exists --
  // `class_int` needs an object, so this reads the resolved property directly.
  std::int32_t capacity = 0;
  {
    const std::string_view text = graph->property(boat, "max_load");
    for (const char c : text) {
      if (c < '0' || c > '9') { capacity = 0; break; }
      capacity = capacity * 10 + (c - '0');
    }
  }
  if (want > capacity) want = capacity;
  const std::int32_t taken = want > 0 ? s->warehouse.take(kKind, want) : 0;
  if (taken <= 0) return none;

  const ObjectId id = world->spawn_of_class(boat);
  if (id == kNoObject) return none;
  WorldObject* slot = world->find(id);
  if (slot == nullptr) return none;
  slot->state.cargo = taken;
  slot->state.cargo_resource = static_cast<std::int32_t>(kKind);
  world->set_owner(id, s->owner);
  // Into the settlement's holder, which is where the script's `Select` and
  // `SetCommand` find it and walk it out of.
  (void)world->put_in_holder(id, s->holder.object);
  return HostOutcome::ok_with(object_handle(id));
}


/// `GetEnterExit` -- **14 sites, one script, two receivers and one name.**
///
/// `SETTLEMENT_BEHAVIOR_AMBIENT.VS` uses both forms and uses them together:
///
///     pt1 = bDamaged.GetEnterExit;
///     b1 = .GetEnterExit;
///     if( b1.IsValid ) pt1 = b1.GetEnterExit;
///
/// On a **Settlement** it answers which building is the settlement's gate; on a
/// **Building** it answers where that building's gate is. `gbr.exe` registers
/// them as two entry points distinguished by receiver type (0x005c3570 returns a
/// `Building`, 0x005c54d0 returns a `point`); this registry keys on name and
/// arity alone, so the branch is on the handle the caller passed -- the same
/// arrangement `Obj::settlement` already uses to share a key with `GAIKA`.
///
/// Both are called without parentheses, which is why a naive `\.name\s*\(` grep
/// of the corpus finds zero sites for this name and why it sat unimplemented
/// while smaller ones were written.
///
/// ## Both draw from the world RNG, and that is the finding
///
/// Neither answers "the first". The shared helper behind the Building form
/// (0x005c5390) collects **every** type-12 point of the class that survives the
/// round trip and returns a uniformly random one; the Settlement form counts the
/// buildings whose class declares a type-12 point, draws once, and walks the
/// list again to that index. One draw each, whenever there is anything to
/// choose from, and none when there is not.
///
/// That is not a detail. 19 of the installation's 91 gate-bearing classes
/// declare more than one type-12 point, so the choice is observable -- and even
/// where it is not, *whether the call advances the stream* is synchronised
/// state, which is `Rng::below`'s own rule.
///
/// ## The Settlement form filters on nothing
///
/// No aliveness test, no under-construction test, no owner test, and -- the one
/// that bites -- **no test that the chosen building's point will actually
/// project inside the map**. So `.GetEnterExit` can hand back a building for
/// which `b1.GetEnterExit` then answers `(-1, -1)`, and the script's own
/// `if (b1.IsValid)` does not protect against it. Reproduced, because a
/// reimplementation that quietly filtered would answer a different building.
///
/// A dead or absent receiver answers the invalid handle or the sentinel. The
/// original dereferences null there and this does not, which is a fault of its
/// rather than a behaviour of its.
[[nodiscard]] HostOutcome enter_exit_of_building(CallContext& ctx, World& world) {
  const Value& value = ctx.arg(0);
  const Point missing = kNoClassPoint;
  if (!value.is_object()) return HostOutcome::ok_with(pack_point(missing));
  const ObjectId id = value.as_object().id;
  std::vector<Point> gates;
  class_points_of_type(world, id, kEnterExitPointType, gates);
  if (gates.empty()) return HostOutcome::ok_with(pack_point(missing));
  const std::int32_t at = world.rng().between(0, static_cast<std::int32_t>(gates.size()) - 1);
  return HostOutcome::ok_with(pack_point(gates[static_cast<std::size_t>(at)]));
}

[[nodiscard]] HostOutcome enter_exit_of_settlement(CallContext& ctx, World& world, Settlement* s) {
  if (s == nullptr) return HostOutcome::ok_with(invalid_handle());
  std::int32_t count = 0;
  for (const SettlementBuilding& b : s->buildings) {
    if (class_has_point_type(world, b.object, kEnterExitPointType)) ++count;
  }
  if (count == 0) return HostOutcome::ok_with(invalid_handle());
  std::int32_t wanted = world.rng().between(0, count - 1);
  for (const SettlementBuilding& b : s->buildings) {
    if (!class_has_point_type(world, b.object, kEnterExitPointType)) continue;
    if (wanted-- == 0) return HostOutcome::ok_with(object_handle(b.object));
  }
  return HostOutcome::ok_with(invalid_handle());
}

HostOutcome m_get_enter_exit(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetEnterExit: no world");
  // The receiver's handle type is the whole of the dispatch. A settlement
  // handle cannot be resolved through `settlement_at` here, because that walks
  // an *object* handle back to the settlement it belongs to -- which is right
  // for `set.gold` on a building and would make the Building arm unreachable.
  const bool on_settlement =
      ctx.arg(0).is_object() && ctx.arg(0).as_object().type == kTypeSettlement;
  if (!on_settlement) return enter_exit_of_building(ctx, *world);
  return enter_exit_of_settlement(ctx, *world, receiver(ctx));
}

/// `IsHeirOf`, defined below with the class walks that use it most.
[[nodiscard]] bool is_class(World& world, const WorldObject& slot, std::string_view name);

/// `set.WaterLsa` -- 7 sites, and it is not a settlement property at all.
///
/// `0x00426010` reads the settlement's **central building**, tests it with
/// `IsHeirOf("BaseShipyard")`, and only then answers. So this is *a shipyard's
/// cached water-area id*: which body of water this yard launches into. A
/// settlement whose central building is not a shipyard answers 0, and the two
/// shipped readers are exactly the two things that makes sense for:
///
///     ships = Ships(idPlayer, set.WaterLsa);              // ES_SHIPYARD.VS
///     if (.supplied.GetCentralBuilding.IsHeirOf("BaseShipyard")
///         && .supplied.WaterLsa == .WaterLsa)             // SHIPYARD_IDLE.VS
///
/// -- a key into the per-area ship bookkeeping, and "is my supplier on the same
/// sea as me, so I can send a boat rather than a mule". Neither compares the id
/// against a literal and neither carries it across a save, so **this engine's
/// own LSA numbering answers both**, which is the question `sim/gaika.hpp`'s
/// decision record makes worth asking of every LSA reader.
///
/// ## Where the id comes from, and it is not the settlement's position
///
/// The original caches it on the building (`+0x1f6`, with the land twin at
/// `+0x1f4`) and refreshes both whenever the partition is rebuilt. The value is
/// the area of the **first point of the shipyard's exit-water list** -- the
/// fourth of the four baked door lists, `sim/entrance.hpp`'s -- snapped to the
/// nearest water area and **excluding the settlement's own land area**. Not the
/// centre of the settlement, and not the *enter*-water list.
///
/// Computed on demand here rather than cached, for `sim/entrance.hpp`'s reason
/// and one more: the original's cache starts life as `0xffff` and surfaces to a
/// script as **65535** until the first partition rebuild touches it, which
/// `ES_SHIPYARD.VS` would happily use as a table key. Deriving it means that
/// state cannot exist.
///
/// **What is not reproduced.** When the door's own cell has no area, or has the
/// settlement's land area, the original spirals outward in 128-unit steps for a
/// water one. The spiral's order is not recovered and the order decides which
/// area wins, so nothing here spirals: that case answers 0. It needs a shipyard
/// whose water door stands in a stretch of sea too small for the partition to
/// keep, which no shipped map has. The first try's own water test is likewise
/// not settled -- the executable applies it to the spiral's candidates and the
/// reading of whether it also applies to the first is ambiguous -- and it
/// cannot matter for a door that `building_doors` already put on deep water.
HostOutcome m_water_lsa(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("WaterLsa: no world");
  const auto none = HostOutcome::ok_with(Value::integer(kNoLsa));
  const Settlement* s = receiver(ctx);
  if (s == nullptr || s->anchor == kNoObject) return none;
  const WorldObject* anchor = world->find(s->anchor);
  if (anchor == nullptr || !is_class(*world, *anchor, "BaseShipyard")) return none;

  MovementSystem* movement = movement_system(*world);
  std::vector<Point> doors;
  building_doors(*world, movement == nullptr ? nullptr : &movement->grid(), s->anchor,
                 kExitPointType, true, doors);
  // The original dereferences the empty list's null data pointer here. That is
  // a fault rather than a behaviour, and 0 is the answer its own "not a
  // shipyard" arm already gives.
  if (doors.empty()) return none;

  const LsaId land = world->lsa().at(world->resolve_position(s->anchor));
  const LsaId here = world->lsa().at(doors.front());
  if (here == kNoLsa || here == land) return none;
  return HostOutcome::ok_with(Value::integer(here));
}

/// `set.FindNearEnterExit(pt)` -- one site, and the deterministic sibling.
///
/// 0x005c3730 walks the same building list and, unlike `GetEnterExit`, considers
/// **every** type-12 point of every building rather than one per building --
/// so a class with four gates has all four competing. It keeps the nearest to
/// the reference point on a strict comparison, which sends a tie to the earlier
/// building and then the earlier slot. No RNG.
///
/// Its "nothing found" answer is `(-1, -1)`, reached by seeding the best
/// distance with ten million and never beating it. That seed is transcribed as
/// a flag rather than a number: a genuine point ten million units away would
/// read as "none" in the original, and no map is a fortieth that wide.
HostOutcome m_find_near_enter_exit(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindNearEnterExit: no world");
  const Settlement* s = receiver(ctx);
  const Point reference = ctx.count() > 1 && is_point(ctx.arg(1))
                              ? unpack_point(ctx.arg(1))
                              : Point{};
  Point best = kNoClassPoint;
  std::int64_t nearest = -1;
  if (s != nullptr) {
    std::vector<Point> gates;
    for (const SettlementBuilding& b : s->buildings) {
      gates.clear();
      class_points_of_type(*world, b.object, kEnterExitPointType, gates);
      for (const Point& gate : gates) {
        const std::int64_t dx = gate.x - reference.x;
        const std::int64_t dy = gate.y - reference.y;
        const std::int64_t d2 = dx * dx + dy * dy;
        if (nearest < 0 || d2 < nearest) {
          nearest = d2;
          best = gate;
        }
      }
    }
  }
  return HostOutcome::ok_with(pack_point(best));
}

// -- the outpost descriptor -------------------------------------------------
//
// Six entry points over 65 call sites, registered together at `0x00564540` and
// the whole of what `DATA\SUBAI\OUTPOST_BEHAVIOR.VS` was still missing. All six
// are `Building` members; five take a slot, `GetOutpostFood` takes nothing.
//
// ## What they read
//
// `<properties>` on the six race-specific outpost classes, in two slots. From
// `IOUTPOST.SC.XML`, the only one that declares both:
//
//     defender_cls_1  = "ISlinger"    defender_cls_2  = "IDefender"
//     defenders_max_1 = "12"          defenders_max_2 = "10"
//     defenders_out_1 = "4"           defenders_out_2 = "10"
//     start_level_1   = "8"           start_level_2   = "8"
//     end_level_1     = "20"          end_level_2     = "8"
//     settlement_food = "0"
//
// `CVXOutpost`'s constructor (0x00563f70) copies them out of the class
// descriptor into eleven fields of its own -- two `std::string`s at `+0x208`
// and `+0x224` and nine dwords at `+0x240 .. +0x260` -- and each getter reads
// exactly one of them with a plain `mov`. Nothing in `gbr.exe` writes those
// fields after construction: the whole outpost defender behaviour is in the
// `.vs` script, and these eleven are immutable per-instance copies of class
// data. So they are read from the class graph here, which is the same fact with
// one home instead of two and nothing for a save to carry.
//
// `GetOutpostFood` is **not** an outpost property. Descriptor `+0xa8c` is
// `settlement_food` -- `CVXTownHall`'s economy property, the one every
// settlement in `docs/data-model.md` starts its larder from -- and the getter
// reads it verbatim. What the script does with it is stock the outpost after a
// capture.
//
// ## The gate, and the two objects it is about
//
// Every one of the six opens the same way, and it does **not** ask about the
// receiver. It reads the receiving building's settlement (`building + 0x148`),
// hands that to `0x00440060`, and that function asks whether the *settlement's
// central building* is an heir of the class named `Outpost`. On failure the
// getter returns the zero it seeded its result register with, prints nothing,
// and refuses nothing.
//
// `0x00440060` is also the body of the registered `Settlement::IsOutpost`, so
// this is `s->is_outpost()` -- `SettlementKind::outpost` is derived at load by
// exactly that ancestry test (`kind_of`, sim/session.cpp), which is what makes
// the two spellings the same question.
//
// One divergence, deliberate. A **non-central** building inside an outpost
// settlement passes the gate in the original and then reads `+0x240` off an
// object that is not a `CVXOutpost` -- uninitialised memory, silently. That is
// a fault, not a behaviour, so this reads the receiver's own class and answers
// what that class declares. No shipped script can construct the case: the
// receiver is always the outpost tent itself.
//
// ## The slot is two literals, not an index
//
// `cmp ebp,1 / ... / cmp ebp,2 / ...` and no third branch, so `0`, `3`, `-1`
// and `INT32_MAX` all fall through to the pre-seeded zero. There is no clamp,
// no wrap and no read past the end -- and no shipped site passes anything but a
// literal 1 or 2 anyway.
//
// ## The sentinel that does not survive, and the one that does
//
// `CVXClass`'s constructor (0x005a6820) fills every one of these descriptor
// slots with `0xff1b1e40` -- read as a signed int, exactly **-15,000,000** --
// and the two strings with `"**Invalid**"`. It would be natural to conclude
// that an undeclared property reaches a script as that value. It does not, and
// which half of the block escapes is the finding.
//
// `ResolveInheritance` (0x0059c310) runs two passes per class, and both are
// inside its `if (parent != null)` branch:
//
//   1. inherit -- a slot still holding the sentinel takes the parent's value;
//   2. normalise -- a slot *still* holding the sentinel is written to `0`, and
//      a string still holding `"**Invalid**"` is assigned `""`.
//
// The normalise pass covers `+0xa4c .. +0xa68`: the eight outpost numbers. It
// does **not** cover `+0xa8c`, which is `settlement_food`. So:
//
//   * an undeclared slot answers `0` and an undeclared defender class answers
//     `""` -- which is why `OUTPOST_BEHAVIOR.VS`'s `if (sDefenderCls2 != "")`
//     does exactly what its author meant on the five outposts that declare one
//     slot, and why the second-slot spawn loop is skipped rather than run with
//     a class name nothing can place;
//   * an undeclared `settlement_food` answers **-15,000,000**, and
//     `OUTPOST_BEHAVIOR.VS` feeds it straight into `.settlement.SetFood`.
//
// The second is unobservable on shipped data -- all six race outposts declare
// `settlement_food`, and only their common base `Outpost` does not -- and it is
// implemented anyway, because it is the difference between transcribing the
// resolver and guessing that it is uniform.
//
// A **root** class keeps its sentinels: the whole `if (parent != null)` branch
// is skipped, so neither pass runs. No shipped outpost class is a root, and
// `Outpost` itself descends from `Building`. Transcribed for the same reason.

/// `0xff1b1e40`. See above: this reaches a script through `GetOutpostFood`, and
/// through the other five only for a class with no parent.
constexpr std::int32_t kOutpostUnresolved = -15000000;

/// The class whose descriptor the outpost block reads, or `kNoClass` when the
/// gate fails.
///
/// Two objects, not one: the numbers come from the receiving *building*'s class
/// and the gate asks about its settlement's *central* building. See above.
[[nodiscard]] ClassIndex outpost_class(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() == 0 || !ctx.arg(0).is_object()) return kNoClass;
  const ObjectRef ref = ctx.arg(0).as_object();
  if (ref.type != kTypeObj) return kNoClass;
  const WorldObject* slot = world->find(ref.id);
  if (slot == nullptr) return kNoClass;
  const Settlement* s = settlement_at(ctx, 0);
  if (s == nullptr || !s->is_outpost()) return kNoClass;
  return slot->class_index;
}

/// `stem` + `_1` or `stem` + `_2`, or empty for any other slot.
///
/// Empty is how the two-literal dispatch is spelled here: a key nothing can
/// declare is a miss, and a miss is the fallback the caller passes.
[[nodiscard]] std::string outpost_key(std::string_view stem, std::int32_t slot) {
  if (slot != 1 && slot != 2) return std::string();
  std::string key(stem);
  key.push_back('_');
  key.push_back(static_cast<char>('0' + slot));
  return key;
}

/// The value a class that declares nothing answers with.
///
/// `0` for a class with a parent -- the resolver's normalise pass -- and the
/// sentinel for a root, which that pass never reaches. `settlement_food` passes
/// `kOutpostUnresolved` for `normalised` and so answers the sentinel either
/// way, because the pass skips `+0xa8c`.
[[nodiscard]] std::int32_t outpost_fallback(const ClassGraph& graph, ClassIndex index,
                                            std::int32_t normalised) noexcept {
  if (index == kNoClass) return normalised;
  return graph.at(index).parent_index == kNoClass ? kOutpostUnresolved : normalised;
}

/// One numeric slot of the descriptor.
[[nodiscard]] HostOutcome outpost_number(CallContext& ctx, std::string_view stem,
                                         bool slotted, std::int32_t normalised) {
  World* world = world_of(ctx);
  const ClassGraph* graph = world == nullptr ? nullptr : world->class_graph();
  const ClassIndex index = outpost_class(ctx);
  // A failed gate is `0` whatever the field is: the result register is seeded
  // before the gate runs and the branch jumps straight to the emit.
  if (graph == nullptr || index == kNoClass) return integer(0);
  const std::string key = slotted ? outpost_key(stem, int_arg(ctx, 1)) : std::string(stem);
  const std::int32_t fallback = outpost_fallback(*graph, index, normalised);
  // A bad slot is the pre-seeded zero, not the class's fallback: the dispatch
  // never reaches a field at all.
  if (key.empty()) return integer(0);
  const std::string_view value = graph->property(index, key);
  if (value.empty()) return integer(fallback);
  std::int32_t parsed = 0;
  // The original parses with `atoi`, which stops at the first non-digit and
  // yields what it read. `parse_int` refuses the whole value instead, and the
  // difference reaches a script only through a class property that is not a
  // number -- of which the installation has none. Refusing is the reading this
  // tree already takes everywhere else a class property becomes an integer.
  if (!parse_int(value, parsed)) return integer(fallback);
  return integer(parsed);
}


/// The class a settlement's own sentries are, which the original **computes
/// rather than reads**: the race letter of its central building, with `Sentry`
/// after it.
///
/// 0x005c4830 takes the central building (`[set+0x8c]`, which is what
/// `GetCentralBuilding` hands back), asks its class for the race index at
/// `[class+0xb48]`, turns that into a letter through the eight-way table at
/// 0x005a7c00, and looks up the concatenation in the class registry. The
/// letters are `sim/player_host.hpp`'s `race_prefix`, and **the two tables were
/// derived independently and agree**: the jump table's strings are `G R C I M
/// B E T` in race order, which is `kRacePrefix` exactly.
///
/// The eight names it can produce -- `GSentry` through `TSentry` -- are all
/// real classes in the retail install, and there is a ninth, the raceless
/// `Sentry` they descend from, which this can never name because a race index
/// is needed to get a letter at all.
///
/// **It is not `Building::GetSentryClassName`**, which reads the class property
/// of the same name. That property is declared only on walls and gates -- 128
/// classes, `<X>Sentry` on the walls and `<X>Sentry1` on the gates -- and never
/// on a town hall, so the property route cannot answer for a settlement and the
/// computed route can. The two agree where both apply: a nation's walls declare
/// exactly the class its letter builds.
[[nodiscard]] ClassIndex sentry_class_of(const World& world, const Settlement& set) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || set.anchor == kNoObject) return kNoClass;
  const WorldObject* central = world.find(set.anchor);
  if (central == nullptr || central->class_index == kNoClass) return kNoClass;
  const std::int32_t race = race_from_name(graph->property(central->class_index, "race"));
  const std::string prefix = race_prefix(race, false);
  if (prefix.empty()) return kNoClass;
  return graph->find(prefix + "Sentry");
}

/// `int set.EvalSentries` -- 1 site, in `GS_SIEGE.VS`, and the second half of a
/// gate `GAIKA::Count/3` has been carrying with only its first.
///
/// The site is one line: `nInHolder += gaika.settlement.EvalSentries;`, added
/// to `UnitsInHolderEval()` and then weighed against the enemy strength the
/// census reports, so what it answers is a **strength** and not a count.
///
/// ## The formula
///
/// 0x005c4a60 is `perSentry * roster`, and refuses before it multiplies: a
/// roster of zero or less is zero. The roster is `[+0xfc]`, `Settlement::
/// sentries` -- what `GetNumSentries` answers -- and **not** `[+0xf4]`, the
/// number actually standing in the wall. The two are different numbers and the
/// original picks the roster here, while the strength it caches at `[+0xf8]`
/// (0x005c4930, rebuilt on every write) uses the wall count. Both are the same
/// per-sentry figure times a different tally.
///
/// `perSentry` is 0x005c4830, and every term is read:
///
///     quarter = (maxhealth * 5) / 4
///     power   = damage + armor_slash
///     level   = env int `SentriesLevel`, in the settlement's own scope
///     perSentry = (power * (level + 13) * quarter) / 1000 + 1
///
/// all of it on the **sentry class**, not on the settlement or its buildings.
/// The three class fields are `[class+0x294]`, `[class+0x2a0]` and
/// `[class+0x2ac]`, which the class loader fills from the attributes named
/// `maxhealth`, `damage` and `armor_slash` -- each read out of the loader's own
/// `strcmp` chain rather than assumed, and `maxhealth` agreeing with what
/// `MatchSystem::record_kill` already had for `+0x294`.
///
/// Both divisions truncate toward zero, which is what the shifts with their
/// sign corrections do (`cdq; and edx,3; add eax,edx; sar edi,2` for the
/// quarter, and the reciprocal multiply at 0x005c4913 for the thousand). The
/// `+ 1` is outside the division, so a settlement whose sentries evaluate to
/// nothing still contributes one point per body.
///
/// **The level is an environment integer and this engine already has the
/// scope.** `/%s/Settlement%d/SentriesLevel` is `EnvScope::for_settlement`
/// exactly, the same shape `GoldSpentOnArmy` and the outpost minima use. It is
/// unset on a freshly loaded map, so `level` is 0 and the factor is 13 until
/// something writes it -- and nothing in the 885 shipped scripts does, which
/// makes 13 the only factor the retail content ever sees.
///
/// A settlement whose central building names no race, or whose race names no
/// sentry class, answers **0**. The original would dereference the null the
/// registry hands back and crash there; refusing is the same departure this
/// file makes everywhere a class lookup can fail.
[[nodiscard]] std::int32_t eval_sentries(World& world, const Settlement& set) {
  if (set.sentries <= 0) return 0;
  const ClassGraph* graph = world.class_graph();
  const ClassIndex sentry = sentry_class_of(world, set);
  if (graph == nullptr || sentry == kNoClass) return 0;

  const auto number = [&](std::string_view key) {
    std::int32_t out = 0;
    return parse_int(graph->property(sentry, key), out) ? out : 0;
  };
  const std::int32_t quarter = (number("maxhealth") * 5) / 4;
  const std::int32_t power = number("damage") + number("armor_slash");
  std::int32_t level = 0;
  if (const EnvSystem* env = env_of(world); env != nullptr) {
    level = env->env().read_int(EnvScope::for_settlement(set.id), "SentriesLevel");
  }
  const std::int32_t per = (power * (level + 13) * quarter) / 1000 + 1;
  return per * set.sentries;
}

/// The string half. Same gate, same slot dispatch, `""` for every miss.
[[nodiscard]] HostOutcome outpost_name(CallContext& ctx, std::string_view stem) {
  World* world = world_of(ctx);
  const ClassGraph* graph = world == nullptr ? nullptr : world->class_graph();
  const ClassIndex index = outpost_class(ctx);
  const auto empty = [] { return HostOutcome::ok_with(Value::string(std::string())); };
  if (graph == nullptr || index == kNoClass) return empty();
  const std::string key = outpost_key(stem, int_arg(ctx, 1));
  if (key.empty()) return empty();
  const std::string_view value = graph->property(index, key);
  // The string half of the sentinel is normalised to `""` for any class with a
  // parent, and a root keeps `"**Invalid**"`. The five outposts with one slot
  // are the first case, which is what makes `if (sDefenderCls2 != "")` right.
  if (value.empty()) {
    return graph->at(index).parent_index == kNoClass
               ? HostOutcome::ok_with(Value::string("**Invalid**"))
               : empty();
  }
  return HostOutcome::ok_with(Value::string(std::string(value)));
}

// -- the economy's slice of `GetConst` -------------------------------------

struct NamedConstant {
  std::string_view name;
  std::int32_t EconomyRules::*field;
};

/// Every `CONST.INI` key the economy owns, spelled as the file spells it.
/// `GetConst` is case-sensitive in the corpus (`GetConst("MinPopulation")`).
constexpr NamedConstant kEconomyConstants[] = {
    {"PopulationGrowthRate", &EconomyRules::population_growth_rate},
    {"PopulationGrowthInterval", &EconomyRules::population_growth_interval},
    {"PopulationDecreasePercent", &EconomyRules::population_decrease_percent},
    {"PopulationDecreaseInterval", &EconomyRules::population_decrease_interval},
    {"MinPopulation", &EconomyRules::min_population},
    {"PopGroup", &EconomyRules::pop_group},
    {"ProductionGoldBase", &EconomyRules::production_gold_base},
    {"ProductionFoodBase", &EconomyRules::production_food_base},
    {"ProductionInterval", &EconomyRules::production_interval},
    {"StrongholdFoodProduction", &EconomyRules::stronghold_food_production},
    {"WagonBuildTime", &EconomyRules::wagon_build_time},
    {"MinResQtyToTransport", &EconomyRules::min_res_qty_to_transport},
    {"BurnTime", &EconomyRules::burn_time},
    {"GoldBurnAmount", &EconomyRules::gold_burn_amount},
    {"FoodBurnAmount", &EconomyRules::food_burn_amount},
    {"PopulationRepairBase", &EconomyRules::population_repair_base},
    {"RepairInterval", &EconomyRules::repair_interval},
    {"InitialStrongholdSentries", &EconomyRules::initial_stronghold_sentries},
    {"SentriesAddTime", &EconomyRules::sentries_add_time},
    {"SentriesAddCount", &EconomyRules::sentries_add_count},
    {"LoyaltyCapInterval", &EconomyRules::loyalty_cap_interval},
    {"LoyaltyChangeCap", &EconomyRules::loyalty_change_cap},
    {"LoyaltyIncreasePerUnit", &EconomyRules::loyalty_increase_per_unit},
    {"LoyaltySettlementsInitial", &EconomyRules::loyalty_settlements_initial},
    {"LoyaltyUnitsOutTreshold", &EconomyRules::loyalty_units_out_threshold},
    {"OutpostRefresh", &EconomyRules::outpost_refresh},
    {"GOutpostFoodSales", &EconomyRules::outpost_food_sales},
    {"GOutpostGoldReturn", &EconomyRules::outpost_food_gold_return},
    {"ROutpostRequiredGold", &EconomyRules::outpost_required_gold},
    {"ROutpostGoldReturn", &EconomyRules::outpost_gold_return},
    {"LoanAmount", &EconomyRules::loan_amount},
    {"LoanInterestPercent", &EconomyRules::loan_interest_percent},
    {"TimeProductionForLoan", &EconomyRules::loan_interest_interval},
    {"InvestmentGoldRevenue", &EconomyRules::investment_revenue},
    {"SlaveryGold", &EconomyRules::slavery_gold},
    {"SpoilsOfWarGold", &EconomyRules::spoils_of_war_gold},
    {"MercenaryPactGold", &EconomyRules::mercenary_pact_gold},
};

/// How many of a thing a settlement could pay for. `gbr.exe` 0x00425a10.
///
/// See the note above `CanAfford` for the arithmetic. `reserved` is the gold
/// the settlement is holding back for a research (`reserved_gold` below), and
/// it comes **off the gold store**: 0x00425a5b reads the warehouse's `+0x18`,
/// subtracts the 0x00425890 result when the flag says so, and divides by the
/// cost that `MaxAffordCount` loads from the row's `+0x1e8` -- the field
/// `CheckTechBudget` adds to `GoldSpentOnTech`, so it is the gold cost. (An
/// earlier note here put the reserve on food; `docs/plan.html` records the
/// correction.) Free rather than a member of `Settlement` because it reads the
/// warehouse and the population and writes nothing, and because every entry
/// point that uses it is a host function.
[[nodiscard]] std::int32_t affordable_count(const Settlement& s, std::int32_t gold,
                                            std::int32_t food, std::int32_t pop,
                                            std::int32_t reserved = 0) noexcept {
  std::int32_t best = 0x7FFFFFFF;
  if (pop > 0) {
    // `population - 1`: a settlement may not spend its last head.
    best = (s.population - 1) / pop;
    if (best <= 0) return 0;
  }
  if (food > 0) {
    const std::int32_t n = s.warehouse.food / food;
    if (n < best) best = n;
    if (best <= 0) return 0;
  }
  if (gold > 0) {
    const std::int32_t n = (s.warehouse.gold - reserved) / gold;
    if (n < best) best = n;
  }
  // **Belt and braces.** The original's own `test esi, esi / jle` (0x00425a7d),
  // and no caller can see it: a negative quotient needs a negative store or a
  // reserve above the store, the population and food terms return zero before
  // they can produce one, and all four entry points compare the result against
  // a positive number. Kept because this answers a *count*, and a negative
  // count is not a number a reader should have to think about.
  return best > 0 ? best : 0;
}

/// The `<cmd>` row a script named, or null. Case-insensitive, which is
/// `CommandTable::find`'s rule and the reason it is one.
[[nodiscard]] const CommandDef* command_row(CallContext& ctx, std::string_view name) {
  World* world = world_of(ctx);
  if (world == nullptr) return nullptr;
  const CommandSystem* commands = command_system(*world);
  return commands == nullptr ? nullptr : commands->table().find(name);
}

/// The settlement's building that offers `row`, or `kNoObject`.
///
/// Membership is `CommandDef::sources` matched against the class **tree**, so
/// `<src obj="GArena1"/>` finds a `GArena1` and anything descended from one.
/// The buildings come from the settlement's roll in its own order, and the
/// first match wins -- 0x0042d020 walks the deque forwards and returns on the
/// first hit.
[[nodiscard]] ObjectId find_lab(CallContext& /*ctx*/, World& world, const Settlement& s,
                                const CommandDef& row, bool must_be_idle) {
  return settlement_lab_for(world, s, row, must_be_idle);
}

}  // namespace

ObjectId settlement_lab_for(World& world, const Settlement& s, const CommandDef& row,
                            bool must_be_idle) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || row.sources.empty()) return kNoObject;
  const CommandSystem* commands = command_system(world);
  for (const SettlementBuilding& building : s.buildings) {
    const WorldObject* slot = world.find(building.object);
    if (slot == nullptr || slot->class_index == kNoClass) continue;
    if (must_be_idle && commands != nullptr) {
      // 0x0042d062 compares the running command's name against the four
      // characters of `"idle"`. A building with no queue at all is idle.
      const CommandQueue* queue = commands->find(building.object);
      const Command* running = queue == nullptr ? nullptr : queue->running();
      if (running != nullptr && running->verb != "idle") continue;
    }
    for (const std::string& source : row.sources) {
      const ClassFilter filter = ClassFilter::parse(source, graph);
      if (filter.match_all) continue;
      if (world.matches_filter(*slot, filter)) return building.object;
    }
  }
  return kNoObject;
}

namespace {

struct EconomyHostDef {
  CallKind kind;
  const char* name;
  std::uint16_t arity;
  HostFn fn;
};

// --------------------------------------------------------------------------
// the AI's own bookkeeping, and the barracks walk
// --------------------------------------------------------------------------

/// The relation code `NearestStronghold`'s filter matches against.
///
/// `gbr.exe` 0x0044e250 answers **one** of three values and never a mask:
/// `AI_OWN` (1) when the two players are the same, `AI_ALLY` (2) when `a`'s own
/// row grants `b` bit 0, and `AI_ENEMY` (4) otherwise. Either index outside
/// 0..15 gives 0, which no filter can intersect.
///
/// **One-directional**, like `Obj::IsEnemy` and unlike `are_allied`: it reads
/// `a`'s row for `b` and never the transpose, so a one-sided truce makes `a`
/// see an ally where `b` sees an enemy. The three constants are the same
/// `AI_OWN`/`AI_ALLY`/`AI_ENEMY` `sim/squad.cpp`'s census selects on.
[[nodiscard]] std::int32_t ai_relation(const PlayerTable& players, std::int32_t a,
                                       PlayerId b) noexcept {
  if (a < 0 || a >= static_cast<std::int32_t>(kPlayerCount)) return 0;
  if (!PlayerTable::is_valid(b)) return 0;
  const auto viewer = static_cast<PlayerId>(a);
  if (viewer == b) return 1;                                     // AI_OWN
  return players.has(viewer, b, Relation::allied) ? 2 : 4;       // AI_ALLY : AI_ENEMY
}

/// `NearestStronghold(pt, idPlayer[, filter])` -- 7 sites across two arities.
///
/// The nearest settlement to `pt` whose central building descends from
/// `BaseTownhall`, filtered by how `idPlayer` sees its owner. `gbr.exe`
/// registers three arities into one search at 0x0042da70; only the two the
/// corpus calls are bound here.
///
/// **The player argument is 1-based and the wrapper decrements it
/// unconditionally**, so `NearestStronghold(pt, 0)` asks about player -1, which
/// the search reads as *no filter at all* and answers with the nearest
/// stronghold on the map whoever owns it. That is a real code path, not a
/// degenerate one: the executable's own one-argument form is exactly this call
/// with -1 written in.
///
/// **The two-argument form filters on `AI_OWN`**, which the wrapper writes in
/// as the literal 1 (0x0042dba7). So the short form is "my nearest stronghold"
/// and not "any". `ES_OUTPOSTSELLGOLD.VS` uses both: line 9 finds its own base
/// and line 68 passes `AI_ALLY` to find an ally's.
///
/// Nearest is measured from the *central building*, through the integer square
/// root of the squared distance -- and the truncation is load bearing, because
/// it makes two settlements 100 and 101 squared units away tie, and a tie keeps
/// the settlement the store holds first.
HostOutcome nearest_stronghold_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  EconomySystem* economy = host_economy(ctx);
  if (world == nullptr || economy == nullptr) {
    return HostOutcome::failed("NearestStronghold: no economy");
  }
  if (ctx.count() < 2 || !is_point(ctx.arg(0))) return HostOutcome::ok_with(invalid_handle());
  const Point at = unpack_point(ctx.arg(0));
  // The raw decrement, not `player_from_script`: the two differ exactly where
  // this entry point's behaviour turns, and the difference is observable. A
  // *negative* result disables the filter, which is what the executable's own
  // one-argument form passes; a result that is merely out of range leaves the
  // filter on and matches nothing, because 0x0044e250 answers 0 for it.
  const std::int32_t viewer = int_arg(ctx, 1) - 1;
  const std::int32_t filter = ctx.count() > 2 ? int_arg(ctx, 2) : 1;  // AI_OWN

  const Settlement* best = nullptr;
  std::int64_t nearest = 0;
  for (const Settlement& s : economy->settlements().all()) {
    if (!s.is_stronghold()) continue;
    if (viewer >= 0 && (filter & ai_relation(world->players(), viewer, s.owner)) == 0) {
      continue;
    }
    const Point here = world->resolve_position(s.anchor);
    const std::int64_t dx = static_cast<std::int64_t>(here.x) - at.x;
    const std::int64_t dy = static_cast<std::int64_t>(here.y) - at.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    if (best != nullptr && distance >= nearest) continue;
    best = &s;
    nearest = distance;
  }
  return HostOutcome::ok_with(best == nullptr ? invalid_handle() : settlement_handle(*best));
}

/// `Reserve<owner>`, `ReserveFor<owner>`, `ReservedAt<owner>`: the three
/// slots the reserve lives in, under the settlement's scope with the owner's
/// zero-based index as the suffix -- the same shape as `GoldSpentOnArmy`.
[[nodiscard]] std::string reserve_key(std::string_view stem, const Settlement& s) {
  return std::string(stem) + std::to_string(static_cast<std::int32_t>(s.owner));
}

/// The gold a settlement is holding back for a research. `gbr.exe`
/// 0x00425890: `Reserve > 0 ? ((GoldSpentOnArmy - ReservedAt) * Reserve) / 100
/// : 0`. The percentage is of the army spending *since* the reservation, not of
/// the store, so a settlement that has bought nothing since reserving holds
/// nothing back, and the reserve grows as the army does.
[[nodiscard]] std::int32_t reserved_gold(const EnvStore& store, const Settlement& s) {
  const EnvScope scope = EnvScope::for_settlement(s.id);
  const std::int32_t percent = store.read_int(scope, reserve_key("Reserve", s));
  // `<= 0` is the original's `jg`; a zero would multiply to zero anyway, so a
  // fault that admits it survives the suite. Kept as the original's test.
  if (percent <= 0) return 0;
  const std::int32_t since = store.read_int(scope, reserve_key("GoldSpentOnArmy", s)) -
                             store.read_int(scope, reserve_key("ReservedAt", s));
  return since * percent / 100;
}

/// The reserve a command-shaped affordability question subtracts: the whole
/// of `reserved_gold`, unless the reserve is held for this very command
/// (0x00428c67..0x00428c8d, and the same nine instructions in `MaxAffordCount`
/// and `CanAfford/2`'s command form), in which case nothing is held back from
/// it. The stored name is compared through the command table rather than
/// character by character, which is the original's case-insensitive compare
/// (0x00426df0 is 0x00409350, the one `CommandTable::find` models) without a
/// second copy of it.
[[nodiscard]] std::int32_t reserved_gold_for(CallContext& ctx, const Settlement& s,
                                             std::string_view command) {
  World* world = world_of(ctx);
  const EnvSystem* env = world == nullptr ? nullptr : env_of(*world);
  if (env == nullptr) return 0;
  const std::int32_t held = reserved_gold(env->env(), s);
  if (held <= 0) return 0;
  const std::string_view held_for =
      env->env().read_string(EnvScope::for_settlement(s.id), reserve_key("ReserveFor", s));
  // An empty or unknown stored name is no row, so it exempts nothing.
  const CommandDef* row = command_row(ctx, held_for);
  return row != nullptr && row == command_row(ctx, command) ? 0 : held;
}

/// The weight and threshold `DATA\COUNTERUNITS.XML`'s parser gives a row.
///
/// The document carries only `class` and `coeff`; 0x0041c8e5-0x0041c9e0 fills
/// the rest of each `<unit>` row from the unit catalog: the class name is
/// looked up among the eight races' six types (0x0041bfc0 over the table at
/// 0x0081a388, then the arena and temple recruits at 0x0081a5c8 with type
/// `-1`), the row's train command is looked up in the command table, and the
/// **weight is `costgold + costfood`** of that command -- a tenth of it for an
/// Imperial Rome arena or temple recruit (race 4, type below 0). The
/// **threshold is `2000 / weight`**, floored at 1; a class with no train
/// command weighs 0 and has threshold 1. One name is patched on the way:
/// `TTeutonArcher` is looked up as `TTeutonRider` (0x007ac68c, 0x007ac67c),
/// a data fix the executable carries.
struct CounterRow {
  std::int32_t weight = 0;
  std::int32_t threshold = 1;
};

[[nodiscard]] CounterRow counter_row_for(CallContext& ctx, const ClassGraph& graph,
                                         const UnitCatalog& catalog, ClassIndex cls) {
  CounterRow row;
  // The class graph names classes by index only the other way round, so the
  // catalog's names are resolved to indices and compared with the row's.
  if (cls != kNoClass && cls == graph.lookup("TTeutonArcher")) cls = graph.lookup("TTeutonRider");
  std::int32_t race = -1;
  std::int32_t type = -1;
  std::string_view train;
  for (std::size_t r = 0; r < catalog.races().size() && train.empty(); ++r) {
    const RaceUnits& units = catalog.races()[r];
    for (std::size_t t = 0; t < units.rows.size(); ++t) {
      if (cls == kNoClass || graph.lookup(units.rows[t].type) != cls) continue;
      race = static_cast<std::int32_t>(r);
      type = static_cast<std::int32_t>(t);
      train = units.rows[t].train_cmd;
      break;
    }
    if (train.empty() && cls != kNoClass && graph.lookup(units.arena.type) == cls) {
      race = static_cast<std::int32_t>(r);
      train = units.arena.train_cmd;
    }
    if (train.empty() && cls != kNoClass && graph.lookup(units.temple.type) == cls) {
      race = static_cast<std::int32_t>(r);
      train = units.temple.train_cmd;
    }
  }
  const CommandDef* command = train.empty() ? nullptr : command_row(ctx, train);
  if (command != nullptr) {
    row.weight = command->cost_gold + command->cost_food;
    // Unreachable in the fixture, as is the archer fix above and the floor
    // below (no shipped recruit costs over 2,000): three equivalences the
    // sweep labels, each the executable's own arithmetic.
    if (race == 4 && type < 0) row.weight /= 10;
  }
  row.threshold = row.weight > 0 ? std::max(1, 2000 / row.weight) : 1;
  return row;
}

/// `GetCounterUnits(set, W)` -- 1 site, `ESH_COUNTERUNITS.VS`, which reads the
/// six weights back as the odds of training each of its race's unit types.
///
/// `0x0042eef0` resolves the settlement (an invalid one prints and returns
/// with `W` untouched), then tries the AI's ten army rows at `[ai+0x30]` --
/// the enemy formations it has *seen*, which this engine does not keep -- and,
/// when none of them answers, falls back to a census (0x0041c400 ->
/// 0x0041c320 -> 0x0041c0e0), which is what is reproduced here:
///
///   * for every countered class `C` of the counter table, `acc[C]` is the
///     number of `C` standing for every **active** player (`[p+0x290] == 1`)
///     the settlement's owner counts an **enemy** (its own row, bit 0 clear --
///     the one-way reading `Obj::IsEnemy` proved);
///   * the owner's own units count against it: `own[j] = count[j] * weight[j]`
///     over every table class `j`;
///   * for each `C` with `acc[C] >= threshold[C]`: `uncovered = (weight[C] *
///     acc[C] - sum of own[j] over the j that counter C) >> 10`, and when that
///     is positive each of the owner's race's six unit types `k` gains
///     `coeff(k counters C) * uncovered` -- `coeff` being the table's own
///     number, 0 for a pair it does not list.
///
/// `W` is **added to, never zeroed**: the zeroing is the army rows' path
/// (0x0041c420), and the census adds onto whatever the script declared, which
/// is an empty array. The `>> 10` is an arithmetic shift, so a mass under
/// 1,024 cost units contributes nothing, and a well-countered one is skipped
/// rather than subtracted. A settlement whose owner has no race in the
/// catalog answers nothing.
HostOutcome get_counter_units_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return no_settlement(ctx);
  const Settlement* s = receiver(ctx);
  if (s == nullptr) return no_settlement(ctx);
  if (ctx.count() < 2 || !is_script_array(ctx.arg(1))) return HostOutcome::ok_void();
  const ScriptArrayId out = array_of(ctx.arg(1));
  const ClassGraph* graph = world->class_graph();
  const CombatSystem* combat = combat_system_of(*world);
  const EnvSystem* env = env_of(*world);
  if (graph == nullptr || combat == nullptr || env == nullptr) return HostOutcome::ok_void();
  const CounterTable& table = combat->counter_table();
  const UnitCatalog& catalog = env->units();
  const PlayerTable& players = world->players();
  const PlayerId owner = s->owner;
  if (owner == kNoPlayer) return HostOutcome::ok_void();
  const std::int32_t race = race_from_name(players.setup(owner).race);
  const RaceUnits* mine = catalog.race(race);
  if (mine == nullptr) return HostOutcome::ok_void();

  // The census: live units by (owner, class), one pass.
  std::vector<std::pair<std::uint32_t, std::int32_t>> census;  // (owner << 16 | class) -> count
  const auto count_of = [&](PlayerId player, ClassIndex cls) -> std::int32_t {
    std::int32_t n = 0;
    for (const WorldObject& slot : world->objects()) {
      if (!slot.state.flags.is_unit || slot.state.health <= 0) continue;
      if (slot.state.owner == player && slot.class_index == cls) ++n;
    }
    return n;
  };
  (void)census;

  const std::vector<ClassIndex> rows = table.countered_classes();
  std::vector<CounterRow> shape;
  std::vector<std::int32_t> own;
  std::vector<std::int32_t> acc;
  shape.reserve(rows.size());
  for (const ClassIndex cls : rows) {
    shape.push_back(counter_row_for(ctx, *graph, catalog, cls));
    own.push_back(count_of(owner, cls) * shape.back().weight);
    std::int32_t seen = 0;
    for (PlayerId p = 0; p < kPlayerCount; ++p) {
      if (players.setup(p).control == PlayerControl::disabled) continue;
      if (!players.is_enemy(owner, p)) continue;
      seen += count_of(p, cls);
    }
    acc.push_back(seen);
  }

  ArrayPool& arrays = world->arrays();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (acc[i] < shape[i].threshold) continue;
    std::int64_t covered = 0;
    for (std::size_t j = 0; j < rows.size(); ++j) {
      if (table.strength(rows[j], rows[i]) != 0) covered += own[j];
    }
    const std::int64_t uncovered =
        (static_cast<std::int64_t>(shape[i].weight) * acc[i] - covered) >> 10;
    // A remainder of exactly zero adds nothing either way; skipping it is an
    // equivalence the sweep labels, kept because the original tests `jle`.
    if (uncovered <= 0) continue;
    for (std::size_t k = 0; k < mine->rows.size() && k < 6; ++k) {
      const ClassIndex ours = graph->lookup(mine->rows[k].type);
      const std::int32_t coeff = table.strength(ours, rows[i]);
      if (coeff <= 0) continue;
      const script::Value before = arrays.get(out, static_cast<std::int32_t>(k));
      const std::int64_t had = before.is_integer() ? before.as_integer() : 0;
      (void)arrays.set(out, static_cast<std::int32_t>(k),
                       Value::integer(static_cast<std::int32_t>(had + coeff * uncovered)));
    }
  }
  return HostOutcome::ok_void();
}

/// `FindRuins(around, dist, level)` -- 2 sites, `SQUADMONITOR.VS` and
/// `TS_ATTACKATWILL.VS`, both `FindRuins(pos, radius, leader.level)` before
/// sending a hero's squad to loot.
///
/// `0x0042cba0` walks the world's settlement vector and keeps the **last**
/// settlement -- there is no ranking -- that passes six tests in this order:
/// its position is within `dist` of the point (`isqrt` of the squares,
/// strictly less); its central building is an heir of `BaseRuins`
/// (0x007ae0b0); the env int `/<root>/Settlement<id>/Bld<bld>/minlevel`
/// (0x007ae090) -- the building scope's `minlevel`, which `RUIN_BEHAVIOR.VS`
/// writes from the class's `minlevel` and raises by `levelperitem` after each
/// gift -- is not above `level`; the building still holds an item (its
/// `vtbl+0x104`, the item count `Obj::item_count` reads); the building and the
/// point are in the same area (0x0043cb70 against 0x0043cb30); and a
/// **coin flip** comes up 1 (`rand(0, 1)` on the world's generator, one draw
/// per candidate that got this far). The answer is the central building, or
/// the invalid handle.
///
/// Here the settlement roll, the anchor, `class_is`, the building scope,
/// `ItemStore::count_for`, `LsaPartition::at` over the anchor's position and
/// the point's, and `Rng::between(0, 1)` -- the same stream every other draw
/// shares, advanced once per candidate as the original advances it.
HostOutcome find_ruins_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindRuins: no world");
  if (ctx.count() < 3 || !is_point(ctx.arg(0))) return HostOutcome::ok_with(invalid_handle());
  const Point around = unpack_point(ctx.arg(0));
  const std::int64_t dist = int_arg(ctx, 1);
  const std::int32_t level = int_arg(ctx, 2);
  const EconomySystem* economy = economy_of(*world);
  const EnvSystem* env = env_of(*world);
  const HeroSystem* heroes = hero_system_of(*world);
  if (economy == nullptr || env == nullptr || heroes == nullptr) {
    return HostOutcome::ok_with(invalid_handle());
  }
  const LsaId here = world->lsa().at(around);
  // Descent, not name equality: the shipped ruins are `GRuins`, `RRuins` and
  // the rest under `BaseRuins`.
  const ClassFilter ruins = ClassFilter::parse("BaseRuins", world->class_graph());
  if (ruins.match_all) return HostOutcome::ok_with(invalid_handle());
  ObjectId found = kNoObject;
  for (const Settlement& s : economy->settlements().all()) {
    const WorldObject* centre = world->find(s.anchor);
    if (centre == nullptr) continue;
    const Point at = world->resolve_position(s.anchor);
    const std::int64_t dx = at.x - around.x;
    const std::int64_t dy = at.y - around.y;
    if (isqrt(dx * dx + dy * dy) >= dist) continue;
    if (!world->matches_filter(*centre, ruins)) continue;
    if (env->env().read_int(EnvScope::for_building(s.anchor), "minlevel") > level) continue;
    if (heroes->items().count_for(s.anchor) <= 0) continue;
    // An equivalence over a world with no partition, where every point is
    // `kNoLsa`; the sweep labels it, and a real map is what it is for.
    if (world->lsa().at(at) != here) continue;
    if (world->rng().between(0, 1) != 1) continue;
    found = s.anchor;
  }
  return HostOutcome::ok_with(found == kNoObject ? invalid_handle() : object_handle(found));
}

/// The independents: player fourteen, whose outposts are what a warrior trains
/// against. The same slot `BestNoIndependentTargetInSquadSight` skips.
constexpr PlayerId kIndependents = 14;

}  // namespace

/// A unit's effective level as the executable's `vtbl+0x114` answers it: the
/// combatant's floored level plus the owner's difficulty addend, the pair
/// `sim/match.cpp` adds for the same slot. A unit the combat system does not
/// hold answers the hero system's level, and one it knows nothing of, 0.
///
/// Declared in the header: `Squad::EvalAttach` (sim/squad.cpp) reads the same
/// slot of a hero, and one reading of a virtual is one reading.
std::int32_t effective_level_of(World& world, ObjectId id) {
  if (const CombatSystem* combat = combat_system_of(world); combat != nullptr) {
    if (const Combatant* unit = combat->find(id); unit != nullptr) {
      return unit->base_effective_level() + combat->player_level_addend(unit->owner);
    }
  }
  if (const HeroSystem* heroes = hero_system_of(world); heroes != nullptr) {
    return heroes->level(id);
  }
  return 0;
}

namespace {

/// `AttackSetForTraining(u, center, radius)` -- 1 site, `TS_ATTACKATWILL.VS`,
/// and its sole blocker: the AI's warrior-at-leisure picking a fight it can
/// learn from, tried before the slow animals and the training camps.
///
/// `0x004397d0` walks the settlement roll **from the last to the first** and
/// keeps the best independents' outpost in the area. Per settlement: owned by
/// player fourteen; within `radius` of `center` (`isqrt`, inclusive); its
/// central building an heir of one of four classes, each with a rank and a
/// defender class -- `COutpost` 10 and `CMaceman`; `IOutpost` 9 and
/// `ISlinger`, with the *exclusive* engage; `TTent` 8 and both
/// `TTeutonArcher` and `TTeutonRider`, only for a unit whose effective level
/// (`vtbl+0x114`) is above 4; `TOutpost` 7 and `TValkyrie`, above 8. A rank
/// below the best so far is out; an equal rank is out unless the building is
/// nearer the unit than the best. Then the defenders: the settlement holder's
/// units, and when fewer than two of the defender class are inside, the units
/// of that class within the building's `range` (`[bld+0xd4]`) around it
/// (0x004233b0) -- **two** are needed, the last one met being the defender
/// remembered. The winner gets `approach` set on the unit, aimed at the
/// building (`vtbl+0xb8` with 1, `SetCommand`), then `engage` appended --
/// or, for the Iberian outpost, `engage_unit_type_exclusive` aimed at the
/// defender -- and the call answers **true**; with no winner, false and
/// nothing queued.
///
/// The walk here is the roll in reverse, as the original's is, so an equal
/// rank at an equal distance keeps the later settlement in both; the sweep
/// around the building is ascending id where the original's is cell order,
/// which can change *which* defender the exclusive engage names and nothing
/// else.
HostOutcome attack_set_for_training_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AttackSetForTraining: no world");
  if (ctx.count() < 3 || !ctx.arg(0).is_object() || !is_point(ctx.arg(1))) {
    return boolean(false);
  }
  const ObjectId unit = ctx.arg(0).as_object().id;
  const WorldObject* self = world->find(unit);
  if (self == nullptr) return boolean(false);
  const Point centre = unpack_point(ctx.arg(1));
  const std::int64_t radius = int_arg(ctx, 2);
  const EconomySystem* economy = economy_of(*world);
  CommandSystem* commands = command_system(*world);
  const ClassGraph* graph = world->class_graph();
  if (economy == nullptr || commands == nullptr || graph == nullptr) return boolean(false);

  struct Kind {
    const char* outpost;
    std::int32_t rank;
    std::int32_t needs_level;  // effective level must exceed this; 0 for none
    const char* defender;
    const char* second;
    bool exclusive;
  };
  const Kind kinds[] = {
      {"COutpost", 10, 0, "CMaceman", nullptr, false},
      {"IOutpost", 9, 0, "ISlinger", nullptr, true},
      {"TTent", 8, 4, "TTeutonArcher", "TTeutonRider", false},
      {"TOutpost", 7, 8, "TValkyrie", nullptr, false},
  };
  const std::int32_t level = effective_level_of(*world, unit);
  const Point at = world->resolve_position(unit);

  ObjectId best_building = kNoObject;
  ObjectId best_defender = kNoObject;
  std::int32_t best_rank = 0;
  std::int64_t best_distance = 0;
  bool best_exclusive = false;
  const std::span<const Settlement> roll = economy->settlements().all();
  for (std::size_t back = roll.size(); back > 0; --back) {
    const Settlement& s = roll[back - 1];
    if (s.owner != kIndependents) continue;
    const Point there = world->resolve_position(s.anchor);
    {
      const std::int64_t dx = there.x - centre.x;
      const std::int64_t dy = there.y - centre.y;
      if (isqrt(dx * dx + dy * dy) > radius) continue;
    }
    const WorldObject* building = world->find(s.anchor);
    if (building == nullptr) continue;
    const Kind* kind = nullptr;
    for (const Kind& candidate : kinds) {
      const ClassFilter filter = ClassFilter::parse(candidate.outpost, graph);
      if (filter.match_all || !world->matches_filter(*building, filter)) continue;
      kind = &candidate;
      break;
    }
    if (kind == nullptr) continue;
    if (kind->needs_level > 0 && level <= kind->needs_level) continue;
    if (kind->rank < best_rank) continue;
    const std::int64_t dx = there.x - at.x;
    const std::int64_t dy = there.y - at.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    if (kind->rank == best_rank && best_building != kNoObject && distance >= best_distance) continue;

    const ClassFilter defender = ClassFilter::parse(kind->defender, graph);
    const ClassFilter second =
        kind->second == nullptr ? ClassFilter{} : ClassFilter::parse(kind->second, graph);
    const auto is_defender = [&](const WorldObject& slot) {
      if (!defender.match_all && world->matches_filter(slot, defender)) return true;
      return kind->second != nullptr && !second.match_all && world->matches_filter(slot, second);
    };
    std::int32_t found = 0;
    ObjectId last = kNoObject;
    for (const ObjectId member : s.holder.units) {
      const WorldObject* slot = world->find(member);
      if (slot == nullptr || !is_defender(*slot)) continue;
      last = member;
      if (++found >= 2) break;
    }
    if (found < 2) {
      std::vector<ObjectId> around;
      world->objects_in_radius(there, class_int(*world, *building, "range"), ClassFilter{}, around);
      for (const ObjectId id : around) {
        const WorldObject* slot = world->find(id);
        if (slot == nullptr || !slot->state.flags.is_unit || !is_defender(*slot)) continue;
        // A unit inside a holder is located at the holder here, where the
        // original's cell sweep does not see it at all: the garrison was
        // counted above and is not counted twice.
        if (slot->state.holder != kNoObject || s.holder.contains(id)) continue;
        last = id;
        ++found;
      }
    }
    if (found < 2) continue;
    best_building = s.anchor;
    best_defender = last;
    best_rank = kind->rank;
    best_distance = distance;
    best_exclusive = kind->exclusive;
  }
  if (best_building == kNoObject) return boolean(false);

  Command approach;
  approach.arg_kind = CommandArgKind::object;
  approach.object = best_building;
  (void)commands->set_command(*world, unit, "approach", approach);
  Command engage;
  if (best_exclusive) {
    engage.arg_kind = CommandArgKind::object;
    engage.object = best_defender;
    (void)commands->add_command(*world, unit, /*front=*/false, "engage_unit_type_exclusive", engage);
  } else {
    (void)commands->add_command(*world, unit, /*front=*/false, "engage", engage);
  }
  return boolean(true);
}

/// `set.ReserveFor(tech, percent)` -- one site, `ESH_NEEDTECH.VS`, and the
/// write half of the reserve.
///
/// `gbr.exe` 0x0042cd60 looks the name up (an unknown row prints and answers
/// false), then, if a reserve is already running: answers **false** when it is
/// held for this same research, and **true without writing** when the
/// percentage asked for is the one already stored. Otherwise it stores the
/// three slots -- `Reserve<owner>` = percent, `ReserveFor<owner>` = the name,
/// `ReservedAt<owner>` = the current `GoldSpentOnArmy<owner>` -- and answers
/// true. So a second call for the same tech is the script's signal that the
/// reserve is still running, and a call with a different tech restarts the
/// measure from the current army spending.
HostOutcome reserve_for_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return no_settlement(ctx);
  Settlement* s = receiver(ctx);
  if (s == nullptr) return no_settlement(ctx);
  EnvSystem* env = env_of(*world);
  if (env == nullptr) return no_settlement(ctx);
  if (!ctx.arg(1).is_string()) return boolean(false);
  const CommandDef* row = command_row(ctx, ctx.arg(1).as_string());
  if (row == nullptr) return boolean(false);
  const std::int32_t percent = int_arg(ctx, 2);
  EnvStore& store = env->env();
  const EnvScope scope = EnvScope::for_settlement(s->id);
  const std::int32_t running = store.read_int(scope, reserve_key("Reserve", *s));
  if (running > 0) {
    if (command_row(ctx, store.read_string(scope, reserve_key("ReserveFor", *s))) == row) {
      return boolean(false);
    }
    if (percent == running) return boolean(true);
  }
  store.write_int(scope, reserve_key("Reserve", *s), percent);
  // The name as the script spelt it, which is what the script reads back
  // through `EnvReadString` and compares with its own; the row's spelling only
  // decides that it exists.
  store.write_string(scope, reserve_key("ReserveFor", *s), ctx.arg(1).as_string());
  store.write_int(scope, reserve_key("ReservedAt", *s),
                  store.read_int(scope, reserve_key("GoldSpentOnArmy", *s)));
  return boolean(true);
}

/// `set.CheckTechBudget(tech, percent)` -- 162 sites, all `ESH_NEEDTECH.VS`,
/// one per research the AI knows how to want.
///
/// `gbr.exe` 0x00428ef0 answers whether the research would keep technology
/// within `percent` of everything the settlement has spent or could spend:
///
///     cost = row.costgold
///     if (Reserve > 0 && ReserveFor == tech) {
///       if (cost <= reserved) return true;   // already paid for
///       cost -= reserved;
///     }
///     tech  = GoldSpentOnTech + cost
///     total = GoldSpentOnArmy + (gold - reserved) + tech
///     return tech <= total * percent / 100
///
/// The reserve is subtracted from the store in every case and credited against
/// the cost only when it is held for this research. An unknown row is false.
/// `reserved` is 0x00425890, the same figure `MaxAffordCount` subtracts.
HostOutcome check_tech_budget_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return no_settlement(ctx);
  Settlement* s = receiver(ctx);
  if (s == nullptr) return no_settlement(ctx);
  const EnvSystem* env = env_of(*world);
  if (env == nullptr) return no_settlement(ctx);
  if (!ctx.arg(1).is_string()) return boolean(false);
  const CommandDef* row = command_row(ctx, ctx.arg(1).as_string());
  if (row == nullptr) return boolean(false);
  const EnvStore& store = env->env();
  const EnvScope scope = EnvScope::for_settlement(s->id);
  const std::int32_t reserved = reserved_gold(store, *s);
  std::int32_t cost = row->cost_gold;
  if (reserved > 0 &&
      command_row(ctx, store.read_string(scope, reserve_key("ReserveFor", *s))) == row) {
    if (cost <= reserved) return boolean(true);
    cost -= reserved;
  }
  const std::int32_t tech = store.read_int(scope, reserve_key("GoldSpentOnTech", *s)) + cost;
  const std::int32_t total = store.read_int(scope, reserve_key("GoldSpentOnArmy", *s)) +
                             (s->warehouse.gold - reserved) + tech;
  return boolean(tech <= total * int_arg(ctx, 2) / 100);
}

/// `set.RepairAll()` -- one site, `ES_STRONGHOLD.VS`, once per pass.
///
/// `gbr.exe` 0x0042dec0 walks the settlement's buildings and, for each one
/// whose damage tier is 3 (`Building::IsBroken`, the same stored tier
/// `BestBarrack` skips), walks the building's class command list for a row
/// named `repair` -- compared with the same case-insensitive routine as
/// everything else, so `<method sig="repair">` on the class is the whole test.
/// It then runs the affordability count inline, with the reserve on gold and
/// no exemption, and when the count is at least one it queues the row on the
/// building through the same core `ExecDefaultCmd` reaches (0x004efc00), with
/// the replace flag set and the modifier clear, as the building's owner.
/// Nothing is charged here, on `ExecCmd`'s rule: the method script pays.
/// Always void; a receiver that names no settlement prints and returns.
HostOutcome repair_all_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return no_settlement(ctx);
  Settlement* s = receiver(ctx);
  if (s == nullptr) return no_settlement(ctx);
  CommandSystem* commands = command_system(*world);
  const EnvSystem* env = env_of(*world);
  if (commands == nullptr || env == nullptr) return HostOutcome::ok_void();
  const CommandDef* row = commands->table().find("repair");
  if (row == nullptr) return HostOutcome::ok_void();
  const std::int32_t reserved = reserved_gold(env->env(), *s);
  Command prototype;
  prototype.param = row->param;
  prototype.cost_gold = row->cost_gold;
  prototype.cost_food = row->cost_food;
  prototype.cost_pop = row->cost_pop;
  prototype.cost_stamina = row->cost_stamina;
  prototype.delay = row->exec_delay;
  for (const SettlementBuilding& building : s->buildings) {
    const WorldObject* slot = world->find(building.object);
    if (slot == nullptr || slot->state.damage_state != 3) continue;
    if (commands->script_for(*world, building.object, row->method).empty()) continue;
    if (affordable_count(*s, row->cost_gold, row->cost_food, row->cost_pop, reserved) < 1) {
      continue;
    }
    (void)commands->set_command(*world, building.object, row->method, prototype);
  }
  return HostOutcome::ok_void();
}

/// `set.StopReserving` -- 7 sites, and it is one `Env` write.
///
/// `gbr.exe` 0x00425980 formats `/<root>/Settlement<id>/Reserve<owner>` from
/// the literal at 0x007ad71c and writes **0** into it. Nothing else: no
/// clearing of the command it was reserving for, no counter, no refund.
///
/// **The reserve it stops is a gold reserve**, `reserved_gold` above: all three
/// terms are `Env` ints under the same settlement scope with the same
/// zero-based owner suffix, `Reserve<owner>` (the percentage, 0x007ad71c),
/// `GoldSpentOnArmy<owner>` (0x007ad6d4, which `SpentGoldOnArmy` below
/// maintains) and `ReservedAt<owner>` (0x007ad738, the army spending at the
/// moment of reserving). `ReserveFor` sets them; this clears the one that
/// switches the other two on, so a reservation that restarts measures from
/// where the spending actually is. `ESH_MARKET.VS` calls this on every pass of
/// its market loop -- five of the seven sites are in that one script -- while
/// what the scripts read back is the *string* key `ReserveFor<player>`, which
/// they write through `EnvWriteString` themselves.
HostOutcome stop_reserving_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return no_settlement(ctx);
  Settlement* s = receiver(ctx);
  if (s == nullptr) return no_settlement(ctx);
  EnvSystem* env = env_of(*world);
  if (env == nullptr) return no_settlement(ctx);
  env->env().write_int(EnvScope::for_settlement(s->id),
                       "Reserve" + std::to_string(static_cast<std::int32_t>(s->owner)), 0);
  return HostOutcome::ok_void();
}

/// `set.SpentGoldOnArmy(n)` (15 sites) / `set.SpentGoldOnTech(n)` (2).
///
/// **They are `Env` counters and nothing else.** `0x00425bb0` and `0x00425cb0`
/// are the same eleven-instruction body twice: format a key, `EnvReadInt` it,
/// add the argument, `EnvWriteInt` it back. The format strings are the
/// executable's own and settle the whole shape:
///
///     /%s/Settlement%d/GoldSpentOnArmy%d
///     /%s/Settlement%d/GoldSpentOnTech%d
///
/// filled with the environment root, the settlement's id, and **the owner's
/// zero-based index** -- `[[set+0x90]+8]`, the same field `Obj::player` reads
/// and then adds one to, without the add. There is no clamp, no cap and no
/// effect on the settlement's actual gold: the *spending* is `ExecCmd` and
/// `Research`, and these only record intent, so that other AI scripts can see
/// how much of a settlement's income is already committed this cycle.
///
/// This engine's `Env` is scoped rather than pathed -- `EnvScope::settlement`
/// *is* the `/Settlement<id>/` segment, which is what `scope_arg` builds for a
/// `kTypeSettlement` handle -- so the key is the trailing name and the owner
/// suffix. A script could therefore read the counter back with
/// `EnvReadInt(set, "GoldSpentOnArmy0")`, and **none does**: 17 writes across
/// the corpus and not one read, in either spelling. What reads them is
/// `Settlement::GoldSpentOnArmy` / `GoldSpentOnTech`, which `gbr.exe` registers
/// beside these two and which no shipped script calls -- so those two are not
/// bound here, on `ShowNotes`'s rule. Their existence is still what makes this
/// a stored value rather than a discarded one, which is `Unit::user`'s test.
///
/// **No new state.** `Env` is already modelled, already scoped by settlement,
/// already saved and already hashed. That is the whole reason these are cheap.
///
/// An unowned settlement writes under the suffix `255`, which is `kNoPlayer`
/// rather than a player. The original would dereference a null player record
/// there; a settlement in the shipped content always has an owner, and a key
/// nothing can name is the harmless end of the two.
template <bool kArmy>
HostOutcome spent_gold_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return no_settlement(ctx);
  Settlement* s = receiver(ctx);
  if (s == nullptr) return no_settlement(ctx);
  EnvSystem* env = env_of(*world);
  if (env == nullptr) return no_settlement(ctx);

  const EnvScope scope = EnvScope::for_settlement(s->id);
  const std::string key = std::string(kArmy ? "GoldSpentOnArmy" : "GoldSpentOnTech") +
                          std::to_string(static_cast<std::int32_t>(s->owner));
  env->env().write_int(scope, key, env->env().read_int(scope, key) + int_arg(ctx, 1));
  return HostOutcome::ok_void();
}

/// The eight class ids `gbr.exe` tests a barrack against.
///
/// `0x004403a0` is not the liveness check it looks like: it is eight
/// `IsHeirOf` calls in a row, against these names, with the two commonest
/// results cached in globals. Every one of them is a string in the
/// executable's own table, which is where these come from -- the class *files*
/// are game data this project does not carry, and nothing here reads them.
///
/// **The list is not `cpp_class="CVXBarrack"`, and that is the point.** The
/// same C++ class backs every temple, the druid house and two monuments; a
/// name-descent test over these eight selects the eight race barracks and
/// nothing else, which is why `Settlement::BestArena` and
/// `Settlement::BestTemple` exist separately with their own lists.
///
/// `Barrack` rather than `GBarracks` because `Barrack` is the string in the
/// executable, and the Gaul barracks is the class that carries it as its
/// `altid`. **Which of the two spellings is written here is unfalsifiable in
/// this engine**, and the fault sweep is what established that:
/// `ClassGraph::lookup` resolves an `id` and an `altid` alike, one class holds
/// both names, so swapping them changes no answer against any graph where that
/// class exists. It is the original's spelling, which is the only ground there
/// is to prefer one on.
constexpr std::string_view kBarrackClasses[] = {"Barrack",  "RBarracks", "MBarracks",
                                                "CBarracks", "IBarracks", "BBarracks",
                                                "EBarracks", "TBarracks"};

[[nodiscard]] bool is_barrack_class(World& world, const WorldObject& slot) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return false;
  for (const std::string_view name : kBarrackClasses) {
    // **No `match_all` guard here, and `find_lab` above needs one.** That guard
    // is for a name that arrives empty; `ClassFilter::parse` sets `match_all`
    // only when nothing was named at all, and a name the graph does not carry
    // parses to `count == 0` with `match_all` false, which `matches_filter`
    // already reads as matching nothing. Six of the eight names below are
    // absent from most maps' class graphs, so that path is the common one, not
    // the edge. A guard here would be dead code -- it was written, and the
    // fault sweep is what said so.
    if (world.matches_filter(slot, ClassFilter::parse(name, graph))) return true;
  }
  return false;
}

/// The arena and the temple: `BestBarrack`'s walk with one name instead of
/// eight, and two differences that are the reason they are separate entry
/// points at all.
///
/// `Settlement::BestArena` (0x004355f0) and `Settlement::BestTemple`
/// (0x00435750) are the same loop as `BestBarrack` except:
///
///   * **the seed is `UINT32_MAX`, not the argument.** `BestBarrack` takes its
///     cut-off from the caller, so `BestBarrack(5)` means "one that is not
///     already backed up"; these two take no argument and always answer a
///     candidate if one exists.
///   * **a building whose *running* command is `"idle"` counts one order
///     freer than its queue length says** (0x00435697, a `strcmp` against the
///     literal at 0x007add84). An idle building is not really working on the
///     thing at the head of its queue.
///
/// The class differs too, and not the same way for both. `BaseArena` is one
/// name (0x007ae710). A temple's is **per race**: the executable reads a race
/// index off the class descriptor at `+0xb48` and indexes the eight-entry
/// table at 0x0081a668, so `BestTemple` looks for a temple of the settlement's
/// *own* race.
constexpr std::string_view kArenaClass = "BaseArena";
constexpr std::string_view kTempleClasses[] = {
    "GDruidHouse", "RTemple",       "CTemple",                 "ITemple",
    "MTemple",     "BTempleOfThor", "ETempleOfHorusAndAnubis", "TTempleOfNeptus"};

/// The race index a class declares, or -1.
[[nodiscard]] std::int32_t race_of_class(World& world, ClassIndex which) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || which == kNoClass) return -1;
  const std::string_view text = graph->property(which, "race");
  if (text.size() != 1 || text[0] < '0' || text[0] > '7') return -1;
  return text[0] - '0';
}

[[nodiscard]] std::string_view temple_class_of(World& world, const Settlement& s) {
  const WorldObject* centre = world.find(s.anchor);
  if (centre == nullptr) return {};
  const std::int32_t race = race_of_class(world, centre->class_index);
  return race < 0 ? std::string_view{} : kTempleClasses[static_cast<std::size_t>(race)];
}

[[nodiscard]] bool is_class(World& world, const WorldObject& slot, std::string_view name) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || name.empty()) return false;
  return world.matches_filter(slot, ClassFilter::parse(name, graph));
}

[[nodiscard]] HostOutcome best_of_class(CallContext& ctx, World& world, Settlement* s,
                                        std::string_view name) {
  (void)ctx;
  if (s == nullptr || name.empty()) return HostOutcome::ok_with(invalid_handle());
  const CommandSystem* commands = command_system(world);
  std::uint32_t fewest = 0xFFFFFFFFu;  // no cut-off; see the note above
  ObjectId best = kNoObject;
  for (const SettlementBuilding& building : s->buildings) {
    const WorldObject* slot = world.find(building.object);
    if (slot == nullptr || !slot->state.flags.is_building) continue;
    if (slot->state.damage_state == 3) continue;
    if (!is_class(world, *slot, name)) continue;
    std::uint32_t queued =
        commands == nullptr ? 0u
                            : static_cast<std::uint32_t>(commands->command_count(building.object));
    if (queued != 0 && commands != nullptr) {
      const CommandQueue* queue = commands->find(building.object);
      if (queue != nullptr && !queue->entries.empty() && queue->entries.front().verb == "idle") {
        --queued;
      }
    }
    if (queued >= fewest) continue;
    fewest = queued;
    best = building.object;
  }
  return HostOutcome::ok_with(object_handle(best));
}

/// `TSGetAllArenae` / `TSGetAllTemples` -- the list forms, and they are **not**
/// the mirrors of the two above.
///
/// Both drop the ruin test, exactly as `TSGetAllBarracks` drops it against
/// `BestBarrack`. And `TSGetAllTemples` (0x004376f0) resolves the race **per
/// candidate building** rather than from the settlement's central building --
/// so it lists a temple of *any* race standing in the settlement, where
/// `BestTemple` only considers one of the settlement's own. On a captured
/// settlement the two disagree, and that is the original's arrangement.
[[nodiscard]] HostOutcome all_of_class(CallContext& ctx, World& world, Settlement* s,
                                       std::string_view name, bool any_race) {
  ObjListPool& pool = objlist_pool_of(world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* out = pool.mutable_items(list);
  if (out == nullptr) return HostOutcome::failed("the pool refused a new list");
  out->clear();
  if (s != nullptr) {
    for (const SettlementBuilding& building : s->buildings) {
      const WorldObject* slot = world.find(building.object);
      if (slot == nullptr || !slot->state.flags.is_building) continue;
      if (any_race) {
        const std::int32_t race = race_of_class(world, slot->class_index);
        if (race < 0) continue;
        if (!is_class(world, *slot, kTempleClasses[static_cast<std::size_t>(race)])) continue;
      } else if (!is_class(world, *slot, name)) {
        continue;
      }
      out->push_back(building.object);
    }
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `set.BestBarrack(nMinQueue)` -- 9 sites, and it is an argmin with a cut-off.
///
/// `0x004281a0` walks the settlement's member deque and keeps the barrack with
/// the **fewest queued commands**, considering only barracks whose queue is
/// shorter than `nMinQueue`. Per candidate it requires, in this order: the
/// eight-name descent test above; a downcast to the `CVXBarrack` C++ class
/// (the type descriptor at `0x0081b230`); and a damage tier at `[b+0x204]`
/// that is not 3.
///
/// **That third test is `Building::IsBroken`**, which this engine already has:
/// `ObjectState::damage_state` is the same field, tier 3 is *this building is
/// a ruin*, and `Building::RRepair` refuses anything else. So "not a ruin" is
/// not an inference here -- it is the predicate already written, read from the
/// other side.
///
/// The queue length is `[b+0x98]`, which is exactly what `Obj::CmdCount()`
/// (0x005aa2c0) returns, so the two cannot disagree and this reads through
/// `CommandSystem::command_count` rather than counting again.
///
/// **The comparison is unsigned**, `jae` at 0x00428271 against a running best
/// seeded from the argument itself. So `BestBarrack(100)` means "any barrack"
/// and `BestBarrack(5)` means "one that is not already backed up" -- the two
/// values the corpus passes -- and a *negative* argument would mean "any",
/// because `0xFFFFFFFF` is above every queue length. Nothing passes one; the
/// cast is here so that the answer would be the original's if anything did.
///
/// Ties go to the earlier building: the test is strictly less-than, and the
/// deque is walked forwards.
///
/// An unresolvable receiver answers the invalid handle with no diagnostic --
/// 0x004281d4 jumps straight to the `0xFFFF` writer -- which is what
/// `if (barrack.IsValid)` at every shipped site is written for.
HostOutcome best_barrack_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("BestBarrack: no world");
  Settlement* s = receiver(ctx);
  if (s == nullptr) return HostOutcome::ok_with(invalid_handle());

  const CommandSystem* commands = command_system(*world);
  std::uint32_t fewest = static_cast<std::uint32_t>(int_arg(ctx, 1));
  ObjectId best = kNoObject;
  for (const SettlementBuilding& building : s->buildings) {
    const WorldObject* slot = world->find(building.object);
    if (slot == nullptr || slot->object == nullptr) continue;
    if (!is_barrack_class(*world, *slot)) continue;
    // The original's second test, and the shipped class graph makes it
    // redundant: all eight names above carry `cpp_class="CVXBarrack"`. It is
    // reproduced because a map that subclassed one of them under a different
    // `cpp_class` would be told apart by the original and not by us.
    if (!slot->object->is_a(imperivm::core::NativeClass::barrack)) continue;
    if (slot->state.damage_state == 3) continue;
    const std::uint32_t queued =
        commands == nullptr
            ? 0u
            : static_cast<std::uint32_t>(commands->command_count(building.object));
    if (queued >= fewest) continue;
    fewest = queued;
    best = building.object;
  }
  return HostOutcome::ok_with(object_handle(best));
}

/// Whether a barrack could take one of the `Barrack Level` rows right now.
///
/// 0x00440f80 is the question the command bar asks before it lights a button:
/// a row with no `groupverifier` (`[cmd+0x16c]` null) answers **no**, and
/// otherwise the verifier decides. Every shipped `Barrack Level` row names
/// `VERIFY_RESEARCH.VS`, whose tests are `EnvSystem::can_research` -- the
/// ledger, then every `ReqSet`/`ReqPlr`/`NReqSet` -- plus the one test it
/// makes about the building itself, `EnvReadString(this, "researching") ==
/// "yes"`, which `RESEARCH.VS` writes while an upgrade is under way. Its
/// closing cost check returns **true** either way (it only sets the tooltip),
/// so cost is not a term here, and `ES_STRONGHOLD.VS` guards the call with
/// `set.gold >= 500` for that reason.
///
/// **Modelled, not run.** The original runs the verifier through the interface
/// layer's own plumbing (0x004eaa40), which registers a scratch handle and
/// reads the player off the session rather than off the settlement; that path
/// is not followed here, and the reading is the script's text, which is the
/// same reading `CanResearch` already carries.
[[nodiscard]] bool barrack_row_available(const EnvSystem& env, const Settlement& s,
                                         ObjectId barrack, const CommandDef* row) {
  if (row == nullptr) return false;
  if (!env.can_research(s, row->name)) return false;
  return env.env().read_string(EnvScope::for_building(barrack), "researching") != "yes";
}

/// `set.UpgradeBestBarrack(nMinQueue)` -- 2 sites, both `ES_STRONGHOLD.VS`, the
/// **sole blocker** of its 213 sites: `UpgradeBestBarrack(0)` when the town
/// holds more than 10,000 gold, `UpgradeBestBarrack(8)` above 500.
///
/// `gbr.exe` 0x00437030 is a walk over the settlement's members that keeps
/// the **busiest** barrack it can upgrade, then queues the upgrade there. Per
/// member: `is_barrack_class` (0x004403a0) and the `CVXBarrack` downcast, with
/// **no ruin test** -- `BestBarrack` has one, this does not. Then three rows
/// are tried in order, each through the availability question above:
///
///   1. `Barrack Level 1` (0x007ae82c);
///   2. `Barrack Level 2` (0x007ae81c) -- only when `nMinQueue <= 0` or the
///      clock is past **five minutes** (0x000493e0);
///   3. `Barrack Level 3`, spelt with the barrack's race letter in front for
///      races 0, 1 and 4 (`%sBarrack Level 3`, 0x007ae808, through the
///      `GetRaceStrPref` table) and bare (0x007ae7f8) for the other five -- the
///      switch at 0x00437420 -- only when `nMinQueue == 0` or the clock is past
///      **ten minutes** (0x000927c0), and only for a class whose race is 0..7:
///      a raceless barrack has no level 3.
///
/// A level-1 candidate is **taken outright**, the last one in the walk
/// winning; a level-2 or level-3 candidate only when its queue (`[obj+0x98]`,
/// what `CmdCount` reads) is **strictly longer** than the running best, which
/// the argument seeds -- an unsigned compare, `jbe` at 0x00437328, so a
/// negative argument means no barrack ever qualifies at level 2 or 3. That is
/// the whole meaning of the argument: `UpgradeBestBarrack(8)` upgrades past
/// level 1 only a barrack with more than eight orders on it, which is the one
/// the AI is actually training from, and `UpgradeBestBarrack(0)` any barrack
/// with an order at all.
///
/// Before the walk, a non-zero argument also refuses the first **minute** of
/// the match (0x0000ea60, `jbe` at 0x00437103). The clock is the game clock
/// `[0x00996ff4] + 0x1258`, `World::time()` here.
///
/// The winner gets the row queued through the `ExecDefaultCmd` core
/// (0x004efc00) with **both flags clear** -- appended behind what the barrack
/// is training, where `RepairAll` replaces, and in front of nothing if all it
/// is running is `idle` (`CommandSystem::append_order`, a labelled reading) --
/// as the barrack's owner, costs and
/// delay riding along the way `ExecCmd` sends them and nothing charged here:
/// the `research` method script pays. Answers true when something was queued,
/// false otherwise; a receiver that names no settlement prints
/// (0x007ae840) and answers false.
HostOutcome upgrade_best_barrack_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return no_settlement(ctx);
  Settlement* s = receiver(ctx);
  if (s == nullptr) return no_settlement(ctx);
  const std::int32_t min_queue = int_arg(ctx, 1);
  const GameTime now = world->time();
  if (min_queue != 0 && now <= 60000) return boolean(false);
  // 0x00437110's own early-out; the walk below answers the same on an empty
  // roll, so the fault sweep reads this as an equivalence. Kept for the shape.
  if (s->buildings.empty()) return boolean(false);
  CommandSystem* commands = command_system(*world);
  const EnvSystem* env = env_of(*world);
  const ClassGraph* graph = world->class_graph();
  if (commands == nullptr || env == nullptr || graph == nullptr) return boolean(false);
  const CommandTable& table = commands->table();
  const CommandDef* level1 = table.find("Barrack Level 1");
  const CommandDef* level2 = table.find("Barrack Level 2");

  std::uint32_t longest = static_cast<std::uint32_t>(min_queue);
  ObjectId best = kNoObject;
  const CommandDef* best_row = nullptr;
  for (const SettlementBuilding& building : s->buildings) {
    const ObjectId id = building.object;
    const WorldObject* slot = world->find(id);
    if (slot == nullptr || slot->object == nullptr) continue;
    if (!is_barrack_class(*world, *slot)) continue;
    // Redundant on the shipped graph for `BestBarrack`'s reason, and the
    // sweep confirms it: an equivalence here, kept for the same map.
    if (!slot->object->is_a(imperivm::core::NativeClass::barrack)) continue;

    const CommandDef* row = nullptr;
    int level = 0;
    if (barrack_row_available(*env, *s, id, level1)) {
      row = level1;
      level = 1;
    } else {
      // `jle` here and `je` below: the two gates read a *negative* argument
      // differently, and neither reading is visible, because the unsigned
      // compare at the foot of the loop lets nothing past a negative seed at
      // level 2 or 3. Both spellings are the executable's; the sweep labels
      // either swap an equivalence.
      if (min_queue > 0 && now < 300000) continue;
      if (barrack_row_available(*env, *s, id, level2)) {
        row = level2;
        level = 2;
      } else {
        if (min_queue != 0 && now < 600000) continue;
        const std::int32_t race = race_from_name(graph->property(slot->class_index, "race"));
        if (race < 0 || race > 7) continue;
        const bool prefixed = race == 0 || race == 1 || race == 4;
        // Upper case, as `GetRaceStrPref` spells it; `CommandTable::find` is
        // case-blind, so the case is unobservable and the sweep says so.
        const std::string name =
            (prefixed ? race_prefix(race, false) : std::string()) + "Barrack Level 3";
        const CommandDef* level3 = table.find(name);
        if (!barrack_row_available(*env, *s, id, level3)) continue;
        row = level3;
        level = 3;
      }
    }
    const std::uint32_t queued = static_cast<std::uint32_t>(commands->command_count(id));
    if (level != 1 && queued <= longest) continue;
    longest = queued;
    best = id;
    best_row = row;
  }
  if (best == kNoObject || best_row == nullptr) return boolean(false);

  Command order;
  order.param = best_row->param;
  order.cost_gold = best_row->cost_gold;
  order.cost_food = best_row->cost_food;
  order.cost_pop = best_row->cost_pop;
  order.cost_stamina = best_row->cost_stamina;
  order.delay = best_row->exec_delay;
  (void)commands->append_order(*world, best, best_row->method, order);
  return boolean(true);
}

/// `set.TSGetAllBarracks()` -- 2 sites, and **the name is wider than the
/// function**, which is worth saying because the name is all a reader has.
///
/// `0x00437430` walks the same deque and keeps every entry that has the
/// building bit (`[obj+0x2c] & 0x800000`, this engine's `kSyncBuilding`, the
/// same test `AsBuilding` and `AsTower` share) **and** passes the eight-name
/// barrack descent. It does *not* do `BestBarrack`'s C++ downcast and it does
/// *not* exclude ruins -- so a burnt-out barrack is in this list and is not
/// `BestBarrack`'s answer, and the two really do disagree.
///
/// The list is freshly minted rather than filled through a by-reference
/// argument, which is the shape `ObjList::FilterClosest` already has here, so
/// it goes through `acquire_temporary` for the same reason.
///
/// Both shipped callers use it only to sum `CmdCount(cmd)` across a
/// settlement, which is what makes the missing ruin test invisible to them.
HostOutcome ts_get_all_barracks_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("TSGetAllBarracks: no world");
  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* out = pool.mutable_items(list);
  if (out == nullptr) return HostOutcome::failed("TSGetAllBarracks: the pool refused a new list");
  out->clear();

  // A receiver that names no settlement still answers a list, and an empty one:
  // 0x00437498 tests the resolved pointer and jumps past the whole walk to the
  // same return the full path uses.
  if (Settlement* s = receiver(ctx); s != nullptr) {
    for (const SettlementBuilding& building : s->buildings) {
      const WorldObject* slot = world->find(building.object);
      if (slot == nullptr || !slot->state.flags.is_building) continue;
      if (!is_barrack_class(*world, *slot)) continue;
      out->push_back(building.object);
    }
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// Give one settlement to `owner`, the way `Settlement::SetPlayer`'s worker
/// (0x005c4eb0) does, and give its buildings to it too.
///
/// **`capture` is deliberately not called here.** `EconomySystem::capture`
/// enforces `loyalty == 0` and `can_be_captured` and then writes loyalty --
/// and 0x005c4eb0 tests neither and touches loyalty not at all. Those are the
/// *script's* preconditions, and the corpus says so plainly: of the 109
/// `SetPlayer` call sites in the installation only **5** have a loyalty test
/// within six lines above them, so routing this through `capture` would make
/// the other 104 silently do nothing -- `OUTPOST_BEHAVIOR.VS`,
/// `TTENT_BEHAVIOR.VS` and every mission sequence that hands a town over
/// outright. `UNIT_CAPTURE.VS` is the shape the rule was read from:
///
///     if (b.settlement.loyalty == 0) {
///         b.settlement.SetPlayer(.player);
///         b.settlement.SetLoyalty(11);
///     }
///
/// Both the guard *and* the loyalty reset are written in the script, on the
/// lines either side of the call.
///
/// What moves: the owner, the loan, and every building in the settlement's own
/// list. What does not: loyalty, gold, food, population, sentries, the timers,
/// and **the garrison** -- `AddUnit` files units in the holder, which the
/// buildings sweep never reaches, so garrisoned units keep their old owner and
/// the captor inherits somebody else's soldiers standing inside.
///
/// The holder and warehouse objects do not follow either. That is the
/// original's behaviour -- the worker touches neither as an object -- and it
/// is a labelled choice rather than an obvious one, because
/// `World::spawn_settlement` gives all three the same owner at creation. The
/// dumps are all tick-2 and contain no capture, so the corpus cannot decide
/// it; fidelity to the disassembly wins, and a reader who prefers composite
/// consistency has the competing case recorded here.
/// Whether a receiver reaches `SetPlayer`'s body that has no range check.
///
/// `Obj::SetPlayer` and `ObjList::SetPlayer` test the player number and refuse
/// it; `Query::SetPlayer` and `Settlement::SetPlayer` do not, and a `NamedObj`
/// arrives at the `Query` body because it is a registered subtype of one. The
/// asymmetry is transcribed rather than tidied -- it is observable, and the
/// same object reached two ways answers differently.
[[nodiscard]] bool skips_range_check(const Value& receiver) noexcept {
  if (!receiver.is_object()) return false;
  const auto type = receiver.as_object().type;
  return type == kTypeQuery || type == kTypeNamedObj;
}

void give_settlement_to(CallContext& ctx, EconomySystem& economy, World& world, Settlement& s,
                        PlayerId owner) {
  // 0x005c4ec1: setting the owner a settlement already has does nothing at
  // all -- not even the loan wipe, which is the only way to observe it.
  if (s.owner == owner) return;
  s.loan = 0;
  // The warehouse's gold changes hands on the report: 0x005c4f03 takes it
  // from the old owner's *captured* count and 0x005c4f24 gives it to the new.
  if (MatchSystem* match = match_system_of(world); match != nullptr) {
    const std::int32_t gold = s.warehouse.amount(Resource::gold);
    match->record_gold_captured(s.owner, -gold);
    match->record_gold_captured(owner, gold);
  }
  (void)economy.set_owner(s.id, owner);
  CommandSystem* commands = command_system(world);
  for (const SettlementBuilding& building : s.buildings) {
    if (building.object == kNoObject) continue;
    (void)world.set_owner(building.object, owner);
    if (commands != nullptr) (void)commands->clear_commands(world, building.object);
  }
  (void)ctx;
}

/// One object changes hands: `CObject::SetPlayer` (0x005aa0a0), and the
/// building override above it.
///
/// A building with a settlement link does not change hands alone. Its slot at
/// `vtbl + 0xA0` is 0x004dbb00, whose whole body reads the object's settlement
/// back-link and, when it resolves, tail-calls `Settlement::set_owner` instead
/// -- so `building.SetPlayer(p)` **captures the whole settlement**, and that is
/// the dominant shipped path: only 8 of the 116 sites pass a settlement
/// handle, while `TTENT_BEHAVIOR.VS` and its eighteen per-map copies,
/// `OUTPOST_BEHAVIOR.VS` and every `NO_Village`/`NO_Tent` name capture through
/// an anchor building.
///
/// **Labelled.** The override sits in eight vtables and keys on the field
/// `Building::settlement` reads, so "any building carrying a settlement link"
/// is the reading taken here. The competing one is that it fires only for a
/// settlement's *central* building. No shipped site separates them — every
/// building receiver in the corpus is an anchor — and the wider reading is
/// chosen because the narrow one would silently drop the outpost and tent
/// captures if any of those anchors turned out not to be central.
///
/// For anything else it is defection: clear the command queue, then write the
/// owner. The clear is conditional on the object *having* an owner, and it is
/// `vtbl + 0xC0`, the same slot `Obj::ClearCommands` calls. The owner write is
/// `ClearFlags(0xFFFF)` then `SetFlags(1 << player)` on the object's flag word
/// -- which is exactly `kSyncOwnerMask` and the one-hot encoding
/// `pack_sync_flags` already produces, an independent corroboration of the
/// mask in `sim/world.hpp`. Nothing above bit 15 is touched, so
/// **`SetPlayer` does not take an object out of its squad or party**.
///
/// What this engine cannot reproduce, and does not fake: the AI is told, three
/// different ways -- 0x0041e970 for a settlement, an engine message for an
/// object, and 0x0041f4d0 on the unit override. The AI here runs a player's
/// `Main.vs` rather than keeping a roster of objects, which is the same reason
/// `place_object` declines 0x0041f310.
void give_object_to(CallContext& ctx, EconomySystem* economy, World& world, ObjectId id,
                    PlayerId owner) {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr) return;
  if (economy != nullptr && slot->settlement != kNoObject) {
    Settlement* s = economy->settlements().for_object(slot->settlement);
    // Only a *building* redirects. A unit carries the same back-link in this
    // engine -- `units_in_settlement` filters on it -- so without this test
    // `u.SetPlayer(1)` would hand over the town the unit is standing in.
    if (s != nullptr && slot->state.flags.is_building) {
      give_settlement_to(ctx, *economy, world, *s, owner);
      return;
    }
  }
  // Two conditions, both the original's: the object must not be a spawn
  // template (bit 27, the flag `SpawnGroup` files objects on) and it must
  // *already have* an owner. An unowned object keeps its orders -- there is no
  // previous owner whose orders they were.
  if (!slot->state.flags.unspawned && slot->state.owner != kNoPlayer) {
    if (CommandSystem* commands = command_system(world)) {
      // **The whole queue, running command included, and that is a labelled
      // reading.** The slot at `vtbl + 0xC0` takes a bool, and the two callers
      // disagree about it: `Obj::ClearCommands` (0x005aa250) passes `false`
      // and is the one this engine already models as "drop the pending tail,
      // leave the runner alone", while `CObject::SetPlayer` passes `true`. So
      // `true` is read as *also take the runner* -- which is the reading that
      // makes the pair mean something, and the one that stops a defecting
      // unit from carrying on with its old owner's order. The competing
      // reading is that the flag means something else entirely and both
      // callers leave the runner; nothing in the corpus separates them,
      // because no shipped site reads a command back after a `SetPlayer`.
      (void)commands->clear_commands(world, id);
      (void)commands->kill_command(world, id);
    }
  }
  (void)world.set_owner(id, owner);
}


/// The settlement's gates: its buildings that are `CVXGate`s, ascending id.
/// See the gate entries in `kEconomyHosts` for what the order costs.

/// `bool gate.Inside(squad)` -- 1 site, in `GS_SIEGE.VS`, and the line that
/// decides whether a besieging squad is still outside the walls.
///
///     gate = set.BestGate(squad.pos);
///     if (gate.Inside(squad)) {  if (bEnemiesOut) SS_KillAll; else SS_Capture; }
///     else                    {  if (gate.IsVeryBroken) SS_Enter; else SS_Siege; }
///
/// ## Shape, which is certain
///
/// 0x00429310 walks the **squad's members** and asks one predicate (0x005295d0)
/// of each: **every** member must answer yes. An empty squad is therefore
/// inside, and a gate that resolves to nothing is a refusal with `false` and a
/// message. `Gate::Outside` (0x00429420) is the same walk with the answer
/// inverted -- every member must say no -- so the two are exact negations
/// sharing one predicate, which is what pins its polarity.
///
/// **`Outside` is registered by `gbr.exe` and is not bound here**, on the rule
/// `ShowNotes` set: no script in the installation calls it, so it is not in the
/// declared surface and binding it would widen the inventory the corpus asks
/// for. The predicate is shared, so the day a caller appears it is one line.
///
/// A member **inside a holder** answers no (`[unit+0x154] != 0xffff`), before
/// anything geometric is asked. A garrisoned unit is not standing inside the
/// walls; it is in a building.
///
/// ## The predicate
///
/// `inside_walls` in `sim/gate.hpp`, read off 0x005295d0: a member is inside
/// when its route to the town centre, every gate open, crosses no gate.
///
/// This used to ask whether the member and the central building stand in one
/// `LsaPartition` area. The partition counts a gate's gap as open ground, so
/// it merges a walled town's inside with its outside, and every besieger read
/// as inside: `GS_SIEGE.VS` turned every squad at the walls to `SS_Capture`,
/// and no gate was ever attacked. (The host body is with the gate members
/// below.)

[[nodiscard]] std::vector<ObjectId> settlement_gates(const World& world, const Settlement& s) {
  std::vector<ObjectId> buildings;
  world.buildings_in_settlement(s.object, ClassFilter{}, buildings);
  std::vector<ObjectId> gates;
  for (const ObjectId id : buildings) {
    const WorldObject* slot = world.find(id);
    if (slot != nullptr && slot->object != nullptr && slot->object->is_a(NativeClass::gate)) {
      gates.push_back(id);
    }
  }
  return gates;
}

constexpr EconomyHostDef kEconomyHosts[] = {
    // `Building::settlement` (432 call sites) is **not** here: the object model
    // owns it, resolves it from `WorldObject::settlement` in constant time, and
    // already hands back the `(kTypeSettlement, settlement object id)` handle
    // every member below accepts.
    // 171 sites. The anchor: the object whose class the dump prints.
    // `SetPlayer(int player)` -- 116 sites, the largest name outside the
    // AI-helper cluster, and the mechanic that decides campaign missions.
    //
    // **Registered four times in `gbr.exe`, with four bodies**: on `Obj`
    // (0x005b77bd -> 0x005ab2f0), `ObjList` (0x0056373a -> 0x0055e710),
    // `Query` (0x0057ad8e -> 0x00578de0) and `Settlement`
    // (0x005c5ff5 -> 0x005c4ff0). This registry keys on (kind, name, arity),
    // so one body answers all four and dispatches on the receiver's handle
    // type. A fifth `SetPlayer` is registered *free* as a debug command that
    // switches which player the human drives; it has zero shipped call sites
    // and is deliberately not implemented.
    //
    // **The settlement branch is chosen by the handle type, not by
    // `settlement_at`.** That helper maps any object with a settlement
    // back-link to its settlement, and in this engine units carry that link
    // too -- `units_in_settlement` filters on it -- so using it here would
    // make `u.SetPlayer(1)` hand over the town the unit is standing in.
    //
    // **Nothing traps.** Every one of the four bodies returns after printing
    // through the sink that is a bare `ret` in the retail build, so a bad
    // player number or a dead receiver is a silent no-op and the script runs
    // on. The `HERO_CAPTURE`/`UNIT_CAPTURE` loops that carry the campaign
    // would die on a refusal the original walks past.
    //
    // The range check is where the four disagree, and it is transcribed
    // rather than tidied: `Obj` and `ObjList` test the number and refuse it,
    // `Query` and `Settlement` have **no check at all**. So
    // `NO_Village1.obj.SetPlayer(99)` is refused and
    // `GetNamedObj("NO_Village1").SetPlayer(99)` -- the same object through a
    // `NamedObj`, which is a registered subtype of `Query` -- is not.
    {CallKind::member, "SetPlayer", 1,
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("SetPlayer: no world");
       if (ctx.count() < 2 || !ctx.arg(1).is_integer()) {
         return HostOutcome::failed("SetPlayer: expected a player number");
       }
       const std::int32_t asked = ctx.arg(1).as_integer();
       EconomySystem* economy = host_economy(ctx);
       const Value& receiver = ctx.arg(0);
       const bool settlement_handle =
           receiver.is_object() && receiver.as_object().type == kTypeSettlement;

       // 1-based in, 0-based stored, and confirmed three ways: every body
       // decrements before use, `Settlement::player` (0x005c2340) returns the
       // stored index plus one, and across the 116 sites the literal
       // arguments run 1..15 with **0 never appearing once**.
       const PlayerId owner = player_from_script(asked);
       if (!settlement_handle && !skips_range_check(receiver) && owner == kNoPlayer) {
         // The `Obj`/`ObjList` check. Refused by returning, not by failing.
         return HostOutcome::ok_void();
       }

       if (settlement_handle) {
         Settlement* s = economy == nullptr
                             ? nullptr
                             : economy->settlements().for_object(receiver.as_object().id);
         if (s == nullptr) return HostOutcome::ok_void();
         give_settlement_to(ctx, *economy, *world, *s, owner);
         return HostOutcome::ok_void();
       }

       if (!is_receiver(*world, receiver)) return HostOutcome::ok_void();
       // Copied before the loop: capturing a settlement clears command queues
       // and rewrites owners, and a view into a container that moves is a
       // use-after-free waiting for a bigger list.
       for (const ObjectId id : receiver_objects(*world, receiver)) {
         give_object_to(ctx, economy, *world, id, owner);
       }
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "GetCentralBuilding", 0,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return HostOutcome::ok_with(s == nullptr ? invalid_handle() : object_handle(s->anchor));
     }},
    {CallKind::member, "gold", 0,  // 125 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : integer(s->warehouse.gold);
     }},
    {CallKind::member, "food", 0,  // 109 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s != nullptr) return integer(s->warehouse.food);
       // A `Squad` receiver -- `sq.food` in `SQUADMONITOR.VS`, `s.food` in
       // `GETARMYNEED.VS`. `Squad::food` (0x004278c0) is the sum over the
       // members of the unit reading below; a member the feeder does not
       // hold adds nothing. See `squad_receiver`.
       if (World* world = world_of(ctx); world != nullptr && ctx.count() > 0) {
         if (const Squad* squad = squad_receiver(*world, ctx.arg(0)); squad != nullptr) {
           const FeederSystem* feeder = feeder_of(*world);
           std::int32_t total = 0;
           if (feeder != nullptr) {
             for (const ObjectId member : squad->members) {
               if (const FeedingUnit* link = feeder->find(member); link != nullptr) {
                 total += link->food;
               }
             }
           }
           return integer(total);
         }
       }
       // A `Unit` receiver -- `u.food` in `VILLAGE_BEHAVIOR_GIVEFOOD.VS` and in
       // the `RYE SPIKES` item script, `.AsUnit.food` in `UNIT.SC.XML`'s own
       // info bar. **Widened here rather than redefined in `sim/feeder.cpp`**:
       // dispatch is on (kind, name, arity) with no receiver type, and a second
       // `define()` on one triple replaces the first silently, so a second
       // definition would have made whichever domain `host_setup.cpp` ran first
       // into dead code that still looked live.
       const FeedingUnit* link = feeding_unit(ctx);
       return link == nullptr ? no_settlement(ctx) : integer(link->food);
     }},
    {CallKind::member, "population", 0,  // 50 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : integer(s->population);
     }},
    {CallKind::member, "SetFood", 1,  // 32 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s != nullptr) {
         s->warehouse.set(Resource::food, int_arg(ctx, 1));
         return HostOutcome::ok_void();
       }
       // 14 of the 32 sites are `Unit`, and they are the ordinary refill path:
       // `u.SetFood(u.food + give)` in `VILLAGE_BEHAVIOR_GIVEFOOD.VS`,
       // `u.SetFood(u.maxfood)` in `TTENT_BEHAVIOR.VS` and
       // `OUTPOST_BEHAVIOR.VS`. Same widening, same reason as `food` above.
       World* world = world_of(ctx);
       FeederSystem* feeder = world == nullptr ? nullptr : feeder_of(*world);
       if (feeder == nullptr) return no_settlement(ctx);
       const FeedingUnit* link = feeding_unit(ctx);
       if (link == nullptr) return no_settlement(ctx);
       feeder->set_food(link->unit, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    // `name/0` -- 61 sites, and **two entry points sharing one name**, which
    // this registry keys on `(kind, name, arity)` and so must serve from one
    // body. The same widening `food/0` above does, and for the same reason: a
    // second `define()` on one triple replaces the first silently.
    //
    // `Settlement::name` (`gbr.exe` 0x005c2700) is the dominant receiver, 47
    // of the 61: `set.name`, `Sett.name`, `.settlement.name`. It reads the
    // `std::string` at `settlement + 0xd0` -- the long/short branch at
    // 0x005c2774 tests `[settlement + 0xe4]` against 16, which is a MSVC
    // `std::string`'s `_Myres`, so the object begins at `+0xd0` and not at the
    // `+0xcc` this tree used to record. That is the same string `GetSettlement`
    // and `WaitSettlementCapture` scan for.
    //
    // `Obj::name` (0x005aea60) is the other 13, and it is not a field at all:
    // it is a **reverse lookup in the named-object manager**. 0x005aeae6 loads
    // the manager at `[0x009bdacc]` -- the same one `GetNamedObj` searches
    // forwards through 0x004c93c0 -- and calls 0x0054b3a0, which walks its
    // `std::map<std::string, ...>` comparing each node's mapped pointer against
    // the object and returns the first key that matches. So `o2.name ==
    // "NO_mercenary"` and `"A_" + bild.name` are asking which `<group
    // type="0">` this object is, which is exactly what those sites mean.
    //
    // **One indirection is read and not modelled.** The original reaches the
    // search value through `word [obj + 0x84]` rather than using the object
    // directly, so on a composite -- a settlement's holder, a ship's -- it may
    // be asking about a root rather than about the receiver. Every one of the
    // 13 shipped sites passes a plain top-level object, where the two readings
    // cannot differ, and nothing here has a field that could hold the link.
    // Recorded rather than guessed at.
    //
    // `Item::name` (0x00535260) is the third registration and has one call
    // site, `UNIT_ON_ENTER.VS:32`'s `item.name == "Gold"`. It is unreachable:
    // its receiver comes from `Obj::GetItem/1`, which has no body, so the
    // branch would be dead code either way. It falls through to the object
    // path below, which answers the empty string for a handle it cannot
    // resolve -- the same thing the original answers for an invalid item.
    //
    // A miss is the empty string, never a refusal: both bodies print through
    // 0x00686eb0, which is a bare `ret` in retail, and push the literal at
    // 0x007ab85a.
    {CallKind::member, "name", 0,  // 61 sites
     +[](CallContext& ctx) -> HostOutcome {
       if (const Settlement* s = receiver(ctx)) {
         return HostOutcome::ok_with(script::Value::string(s->name));
       }
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("name: no world");
       const script::Value& receiver_value = ctx.arg(0);
       const auto empty = [] {
         return HostOutcome::ok_with(script::Value::string(std::string()));
       };
       if (!receiver_value.is_object()) return empty();
       const script::ObjectRef ref = receiver_value.as_object();
       // `Obj` and, through the registered `NamedObj -> Obj` conversion at
       // 0x0055343e, a `NamedObj`. Not `ObjList` and not `Query`: the
       // registration at 0x005b7289 takes argument type 0x14, which is `Obj`
       // alone, and `receiver_objects` would widen it to whichever member of a
       // collection happened to come first.
       ObjectId id = kNoObject;
       if (ref.type == kTypeObj) {
         id = ref.id;
       } else if (ref.type == kTypeNamedObj) {
         id = world->named_objects().object(static_cast<std::int32_t>(ref.id));
       }
       if (world->find(id) == nullptr) return empty();
       return HostOutcome::ok_with(
           script::Value::string(std::string(world->named_objects().name_of(id))));
     }},
    // `units/0` -- 115 sites, the largest unimplemented name in the
    // installation until now, and **two entry points that disagree about what
    // a collection is**.
    //
    // `Settlement::Units` (`gbr.exe` 0x005c3c70) **aliases**. Its hit path
    // allocates nothing: 0x005c3cfa reads the holder handle at
    // `settlement + 0x5e`, resolves it, points at the list embedded at
    // `holder + 0x28`, increments its refcount at 0x005c3cd1 and pushes that.
    // So the roster a script receives *is* the garrison -- and
    // `Settlement::UnitsCount` (0x005c1d80) reads `holder + 0x40`, which is
    // that same list's size member, so the two cannot drift because they are
    // one object.
    //
    // **The decision the plan left open, made deliberately: alias.** Exactly
    // one shipped script can tell -- `TOWNHALL_BEHAVIOR_GUARD.VS` takes the
    // roster into `p` at line 67, calls `p.Clear()` at line 91 and rebuilds it
    // from at most 50 entries read out of `l`, which line 87 bound to the same
    // list. Under the faithful reading that empties the town hall's garrison
    // and then reads from the emptied list, and the file's own author left the
    // note that "the Queries are well known among the script programmers as
    // being very broken :)". Snapshotting would make that script do what its
    // author meant, which is exactly what the standing decision on the four
    // unhashed channels forbids: reproduce the original rather than improve on
    // it. It is also O(1) where a snapshot is O(n), at a site --
    // `TOWNHALL_AUTOTRAIN.VS` lines 68 and 70 -- that reads the roster twice
    // per iteration over a 10,000-cap list.
    //
    // The `.army` precedent points the other way and does not govern: that one
    // is recorded in this tree as a *divergence*, not as a rule.
    //
    // `Squad::Units` (0x0043ae40) **copies**. It allocates 0x2c bytes at
    // 0x0043ae71 and copy-constructs from `squad + 0x38` through 0x00438330,
    // which is a plain forward range copy, so the snapshot is join order
    // front-to-back and `[0]` is the squad's own `[0]`. `GS_CAPTURE.VS` proves
    // it independently at lines 224-231: it takes `ol = squad.Units`, calls
    // `Siege`, and subtracts the *new* count from `ol.count`, which is zero
    // and dead code unless the first is frozen.
    //
    // Two miss paths, and they differ. An invalid settlement prints
    // "The function 'settlement::getunits' called for an uninitialized or
    // invalid object." through the sink that is a bare `ret` in retail, then
    // allocates a **fresh empty list** and hands it back (0x005c3c99-0x005c3cc7)
    // -- so a miss is an empty roster, never a refusal. An invalid squad
    // handle has no defined behaviour at all: 0x00443e30 bounds-checks neither
    // the 12-bit index nor the result, and the caller copy-constructs from
    // whatever pointer came back. Refused by name here, because a crash is not
    // a behaviour worth reproducing and silence would be worse.
    {CallKind::member, "Units", 0,  // 115 sites
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("Units: no world");
       ObjListPool& pool = objlist_pool_of(*world);

       if (is_squad(ctx.arg(0))) {
         HeroSystem* heroes = hero_system_of(*world);
         const SquadKey key = unpack_squad(ctx.arg(0));
         const Squad* squad = heroes == nullptr ? nullptr : heroes->squads().find(key);
         if (squad == nullptr) return HostOutcome::failed("Units: receiver names no squad");
         const ObjListId id = pool.acquire_temporary(ctx.script);
         if (std::vector<ObjectId>* out = pool.mutable_items(id)) {
           *out = squad->members;
         }
         return HostOutcome::ok_with(make_objlist_value(id));
       }

       const Settlement* s = receiver(ctx);
       if (s == nullptr || s->holder.object == kNoObject) {
         // The diagnostic-and-empty-list path, not a refusal.
         return HostOutcome::ok_with(make_objlist_value(pool.acquire_temporary(ctx.script)));
       }
       return HostOutcome::ok_with(
           make_objlist_value(pool.acquire_alias(ctx.script, s->holder.object)));
     }},
    // -- what a settlement can pay for, and where it researches ----------
    //
    // `Settlement::CanAfford` is three registrations over one core, 0x00425a10,
    // which does not answer a bool at all: it answers **how many** of the thing
    // the settlement could buy, as the smallest of three quotients, and each
    // caller compares that against 1 or against its own count.
    //
    //     n = INT_MAX
    //     if (pop  > 0) { n = (population - 1) / pop;  if (n <= 0) return 0; }
    //     if (food > 0) { n = min(n, warehouse.food / food); if (n <= 0) return 0; }
    //     if (gold > 0) { n = min(n, (warehouse.gold - reserved) / gold); }
    //     return max(n, 0)
    //
    // Two details are the original's and are reproduced. The population term is
    // **`population - 1`**, not `population`: a settlement may not spend its
    // last head. And the order matters -- an early zero on population or food
    // returns before the gold division, which is why a settlement with no gold
    // store still answers for a gold-free command.
    //
    // **The reserve is a gold reserve, and the command forms exempt the
    // research it is held for.** 0x00425890 computes it as
    // `Reserve > 0 ? ((GoldSpentOnArmy - ReservedAt) * Reserve) / 100 : 0` from
    // three environment-store values under `/<player>/Settlement<n>/…`, which
    // `ReserveFor` writes and `StopReserving` clears. The `(gold, food)` form
    // passes the flag `0` and never subtracts it; the two command forms, like
    // `MaxAffordCount`, pass `1` unless `Reserve > 0` and the stored
    // `ReserveFor` names this very command (0x00428c67..0x00428c8d), in which
    // case the reserved gold is exactly what the command is allowed to spend.
    // `reserved_gold_for` is that rule in one place.
    {CallKind::member, "CanAfford", 2,  // 24 sites, with /1
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return boolean(false);
       // Two shapes at one arity, told apart by the argument's type -- the
       // arrangement `UnitsInSettlement` and `rollover/3` already have.
       // `(gold, food)` is 0x00425a90 and `(cmd, count)` is 0x00428d40.
       if (ctx.arg(1).is_string()) {
         const CommandDef* row = command_row(ctx, ctx.arg(1).as_string());
         if (row == nullptr) return boolean(false);
         const std::int32_t wanted = int_arg(ctx, 2);
         return boolean(affordable_count(*s, row->cost_gold, row->cost_food, row->cost_pop,
                                         reserved_gold_for(ctx, *s, row->name)) >=
                        (wanted > 0 ? wanted : 1));
       }
       return boolean(affordable_count(*s, int_arg(ctx, 1), int_arg(ctx, 2), 0) > 0);
     }},
    {CallKind::member, "CanAfford", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return boolean(false);
       const CommandDef* row =
           ctx.arg(1).is_string() ? command_row(ctx, ctx.arg(1).as_string()) : nullptr;
       if (row == nullptr) return boolean(false);
       return boolean(affordable_count(*s, row->cost_gold, row->cost_food, row->cost_pop,
                                       reserved_gold_for(ctx, *s, row->name)) > 0);
     }},
    // `set.MaxAffordCount(cmd)` -- 0x00428a10, the count `CanAfford` compares,
    // handed to the script. 32 sites, and 10 of them are `ES_STRONGHOLD.VS`
    // sizing a build order: `n = set.MaxAffordCount(cmd); if (n > want) n =
    // want; for (...) b.ExecCmd(cmd, ...)`. An unknown name prints
    // "No such command" and answers 0. A row with no costs at all answers
    // `INT_MAX`, which is the original's initial value falling through every
    // skipped term -- no shipped row is free, so no shipped site sees it.
    {CallKind::member, "MaxAffordCount", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return integer(0);
       const CommandDef* row =
           ctx.arg(1).is_string() ? command_row(ctx, ctx.arg(1).as_string()) : nullptr;
       if (row == nullptr) return integer(0);
       return integer(affordable_count(*s, row->cost_gold, row->cost_food, row->cost_pop,
                                       reserved_gold_for(ctx, *s, row->name)));
     }},
    // `set.FindResearchLab(tech)` -- 17 sites, and the class-to-command edge
    // runs from the command's side. See `CommandDef::sources`.
    //
    // 0x0042d010 walks the settlement's buildings and, for each, walks its
    // class's command list looking for this row. The `idle` test is a *flag*
    // the caller passes: `FindResearchLab` passes 0 and `Settlement::Research`
    // passes 1, so the script-visible finder answers a lab that is already
    // busy and the one that is about to queue work does not.
    {CallKind::member, "FindResearchLab", 1,  // 17 sites
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       Settlement* s = receiver(ctx);
       if (world == nullptr || s == nullptr || !ctx.arg(1).is_string()) {
         return HostOutcome::ok_with(invalid_handle());
       }
       const CommandDef* row = command_row(ctx, ctx.arg(1).as_string());
       if (row == nullptr) return HostOutcome::ok_with(invalid_handle());
       const ObjectId lab = find_lab(ctx, *world, *s, *row, /*must_be_idle=*/false);
       return HostOutcome::ok_with(lab == kNoObject ? invalid_handle() : object_handle(lab));
     }},
    // `set.Research(tech)` -- 41 sites in 27 scripts, and the sole blocker of
    // nine. It is a *behaviour*, not an accessor: 0x0042d1b0 finds the row,
    // asks the affordability core for at least one, finds an idle lab that
    // offers it, and issues the command there.
    //
    // Every refusal on the way is a **return**, printed into the sink that is a
    // bare `ret` in retail: an unknown upgrade name prints *"No such upgrade
    // %s"*, and an unaffordable one or a settlement with no lab prints nothing
    // at all. `ESH_BUILDARMY.VS` is written for that -- it asks `CanResearch`
    // and `CanAfford` first and calls this only when both say yes, so the
    // internal checks are belt and braces from the corpus's side and load
    // bearing from the executable's.
    //
    // **The command replaces what the lab is running** -- 0x0042d2b5 issues
    // it through 0x004efc00 with the replace flag set and the modifier clear,
    // the same pair `RepairAll` sends -- and not appended behind it. It was
    // appended for a while, and a lab's running command is its `idle`, which
    // never finishes: the research was queued and never started, on every
    // lab whose idle script was running, which is every lab on a loaded
    // map. `ESH_NEEDTECH.VS` then asked `CanResearch` again, the lab still
    // answered idle, and the AI queued the same upgrade once a pass forever.
    {CallKind::member, "Research", 1,  // 41 sites
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       Settlement* s = receiver(ctx);
       if (world == nullptr || s == nullptr || !ctx.arg(1).is_string()) {
         return HostOutcome::ok_void();
       }
       const CommandDef* row = command_row(ctx, ctx.arg(1).as_string());
       if (row == nullptr) return HostOutcome::ok_void();
       if (affordable_count(*s, row->cost_gold, row->cost_food, row->cost_pop) < 1) {
         return HostOutcome::ok_void();
       }
       const ObjectId lab = find_lab(ctx, *world, *s, *row, /*must_be_idle=*/true);
       if (lab == kNoObject) return HostOutcome::ok_void();
       CommandSystem* commands = command_system(*world);
       if (commands == nullptr) return HostOutcome::ok_void();
       // The costs travel with the command, the way `ExecCmd` sends them, so
       // that `cmdcost_gold` and `.cmddelay` answer inside the method script.
       Command order;
       order.param = row->param;
       order.cost_gold = row->cost_gold;
       order.cost_food = row->cost_food;
       order.cost_pop = row->cost_pop;
       order.cost_stamina = row->cost_stamina;
       order.delay = row->exec_delay;
       (void)commands->set_command(*world, lab, row->method, order);
       return HostOutcome::ok_void();
     }},
    // `set.Buildings()` -- 9 sites, and the sole blocker of two of them.
    //
    // `Settlement::Buildings` (`gbr.exe` 0x005c3d40) is the twin of `Units`
    // above and takes the same shortcut: its hit path allocates nothing, points
    // at the vector embedded at `settlement + 0x60`, bumps that vector's
    // refcount at 0x005c3dfe and pushes it. So the original **aliases** here
    // too, and its miss path is the same shape as `Units`' -- a printed
    // diagnostic and a **fresh empty list**, never a refusal.
    //
    // **This one snapshots, and the divergence is deliberate.** Three reasons,
    // in the order they decided it:
    //
    //   1. No shipped site can tell. All nine take the list and read it --
    //      `for (i=0; i<olBuildings.count; i+=1)` in five village and mule
    //      verifiers, `blds = sett.Buildings` in the two outpost scripts. Not
    //      one calls `Clear`, `Add`, `Remove` or `ClearDead` on it. `Units`
    //      aliases because `TOWNHALL_BEHAVIOR_GUARD.VS` *can* tell and the
    //      faithful reading is what that file gets; there is no such file here.
    //   2. `ObjListPool::AliasResolver` hands back a `std::vector<ObjectId>*`,
    //      and a settlement's roll is `std::vector<SettlementBuilding>` -- rows
    //      carrying a per-building `max_health` the anchor's capture arithmetic
    //      reads. Aliasing would mean keeping a parallel `vector<ObjectId>`
    //      beside it, which puts one fact in two places: exactly the drift the
    //      alias exists to prevent.
    //   3. The membership here is the **back-link**, not the roll, for the
    //      reason `World::buildings_in_settlement` records: on a retail map
    //      `Settlement::buildings` is empty at load and `WorldObject::settlement`
    //      is where the answer lives. A handle onto the empty roll would answer
    //      every shipped site with nothing.
    //
    // The filter is match-all: the registration is `[23, 1, 27]`, one argument
    // and it is the receiver, so there is no class to filter on.
    {CallKind::member, "Buildings", 0,  // 9 sites
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("Buildings: no world");
       ObjListPool& pool = objlist_pool_of(*world);
       const ObjListId id = pool.acquire_temporary(ctx.script);
       const Settlement* s = receiver(ctx);
       // The diagnostic-and-empty-list path, not a refusal.
       if (s != nullptr) {
         if (std::vector<ObjectId>* out = pool.mutable_items(id)) {
           world->buildings_in_settlement(s->object, ClassFilter{}, *out);
         }
       }
       return HostOutcome::ok_with(make_objlist_value(id));
     }},
    // `set.ObjectsAround("Military")` -- 16 sites in 11 scripts, the sole
    // blocker of seven, and **a fourth name for a collector this engine
    // already has**.
    //
    // `Settlement::ObjectsAround` (0x005c5b90) allocates a fresh 0x2c-byte list,
    // resolves the class name through the class registry at 0x009bdb00, and
    // calls `Settlement::CollectUnits` (0x005c58f0) with **both** of its flags
    // set. That is the same helper the three `UnitsInSettlement` names reach
    // through `CVXUnitsInSettlementQuery::Refresh`, and both-flags is the mode
    // `SettlementScope::both` encodes -- so `sim/query.hpp`'s note that mode 2
    // "is here because the mode is one field and leaving a hole in it would be
    // inventing a fourth encoding" now has a caller. It is this one, under a
    // different name, reached without a query object at all.
    //
    // The two halves and every filter in them are `World::units_in_settlement`'s
    // and are documented there, including the one divergence this inherits: the
    // garrison half walks the objects carrying a settlement back-link rather
    // than the holder's own vector, because on a retail map the holder is empty.
    // So is the sort-and-unique the original does at 0x005c5ac3, which is why
    // one soldier standing inside two towers' sight is healed once.
    //
    // **A list, not a query.** The return type word is 0x17 (`ObjList`) and the
    // body allocates, so what a script gets is a snapshot it may then mutate --
    // and `TOWNHALL_HEALING.VS` does exactly that on the next line:
    // `l = .ObjectsAround("Military"); l.AddList(.ObjectsAround("BaseMage"));`.
    // A query handle would have refused that.
    {CallKind::member, "ObjectsAround", 1,  // 16 sites
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("ObjectsAround: no world");
       ObjListPool& pool = objlist_pool_of(*world);
       const ObjListId id = pool.acquire_temporary(ctx.script);
       const Settlement* s = receiver(ctx);
       if (s != nullptr) {
         // A class name the graph does not know yields a filter that names
         // classes and resolved none, which matches nothing -- the same
         // reading `fn_units_in_settlement` records, and the same divergence
         // from the original, which refuses the whole query instead.
         const ClassFilter filter =
             ctx.arg(1).is_string() ? ClassFilter::parse(ctx.arg(1).as_string(), world->class_graph())
                                    : ClassFilter{};
         if (std::vector<ObjectId>* out = pool.mutable_items(id)) {
           world->units_in_settlement(s->object, filter, *out, SettlementScope::both);
         }
       }
       return HostOutcome::ok_with(make_objlist_value(id));
     }},
    // `UnitsCount` -- 31 sites, and **two receivers with one name**.
    //
    // `Settlement::UnitsCount` (`gbr.exe` 0x005c1d80) reads `holder + 0x40`,
    // which is the garrison list's own size member; `Ship::UnitsCount`
    // (0x005c6de0) reads `holder + 0x14` of the holder whose handle the ship
    // keeps at `+0x1dc`, which is that holder's occupancy. Two different
    // fields on two different holders, and the corpus calls both through this
    // one name: `UNIT_BOARD_COMMON.VS` asks a ship
    // `ship.UnitsCount == ship.UnitsMax` on the line after it boards, and
    // `SHIP_UNBOARD_ALL_VERIFY.VS` asks `.UnitsCount > 0`.
    //
    // The ship branch was a refusal until the boarding subsystem existed to
    // count -- `sim/world_host.cpp`'s `UnitsMax` note recorded it as such --
    // and it is a count of what is inside `ship + 1`, which is where
    // `sim/objlist.cpp`'s `GetUnitsOnBoard` says a ship's holder lives.
    {CallKind::member, "UnitsInHolderEval", 0,  // 1 site
     // 0x004246a0: the census valuation (`object_power`, 16 bits of it) summed
     // over the garrison -- the holder's list, in its order. A missing
     // settlement answers 0, with the original's diagnostic.
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return integer(0);
       World& world = *world_of(ctx);
       const CombatSystem* combat = combat_system_of(world);
       std::int64_t total = 0;
       for (const ObjectId id : s->holder.units) {
         const WorldObject* slot = world.find(id);
         if (slot != nullptr) total += object_power(world, combat, *slot);
       }
       return integer(static_cast<std::int32_t>(total));
     }},
    {CallKind::member, "UnitsCount", 0,  // 31 sites
     +[](CallContext& ctx) -> HostOutcome {
       if (Settlement* s = receiver(ctx)) return integer(s->holder.count());
       World* world = world_of(ctx);
       if (world == nullptr) return no_settlement(ctx);
       const Value& handle = ctx.arg(0);
       const WorldObject* slot =
           handle.is_object() ? world->find(handle.as_object().id) : nullptr;
       if (slot == nullptr || slot->object == nullptr ||
           !slot->object->is_a(NativeClass::ship)) {
         return no_settlement(ctx);
       }
       return integer(static_cast<std::int32_t>(units_on_board(*world, slot->id)));
     }},
    {CallKind::member, "IsIndependent", 0,  // 26 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->independent());
     }},
    {CallKind::member, "max_population", 0,  // 24 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : integer(s->max_population);
     }},
    {CallKind::member, "SetGold", 1,  // 22 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return no_settlement(ctx);
       s->warehouse.set(Resource::gold, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "IsOutpost", 0,  // 19 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->is_outpost());
     }},
    {CallKind::member, "IsVillage", 0,  // 17 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->is_village());
     }},
    // `set.BestToSupply()` -- 2 sites, the sole blocker of both, and the
    // **nearest town hall of the same owner in the same area**: `supply_target`
    // above is the body, shared with `SupplyCount`.
    {CallKind::member, "BestToSupply", 0,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* self = receiver(ctx);
       World* world = world_of(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (self == nullptr || world == nullptr || economy == nullptr) {
         return HostOutcome::ok_with(invalid_handle());
       }
       const Settlement* best = supply_target(*world, *economy, *self);
       return HostOutcome::ok_with(best == nullptr ? invalid_handle()
                                                   : settlement_handle(*best));
     }},
    // `set.SupplyCount(class)` -- 1 site, the sole blocker of `ESH_MARKET.VS`
    // and its 270 sites: **how many settlements supply this one**.
    //
    // 0x0042c910 walks the world's settlement vector and counts those that
    // share the receiver's player record (`[+0x90]`, the same owner exactly),
    // whose central building is a heir of the named class (0x0059c020 on the
    // class at `[obj+0x3c]`), and whose own `BestToSupply` (0x0042c800, the
    // helper above) **is the receiver**. `ESH_MARKET.VS` asks it with
    // `"BaseVillage"` to size a stronghold's market by the villages feeding
    // it. A dead receiver, or a class no graph knows, is 0.
    //
    // The receiver is not excluded from its own count, because `BestToSupply`
    // does not exclude it either: a stronghold asked for `"BaseTownhall"`
    // counts itself. No shipped call asks that.
    {CallKind::member, "SupplyCount", 1,  // 1 site
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* self = receiver(ctx);
       World* world = world_of(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (self == nullptr || world == nullptr || economy == nullptr) return integer(0);
       if (!ctx.arg(1).is_string()) return integer(0);
       const ClassGraph* graph = world->class_graph();
       const ClassIndex wanted = graph == nullptr ? kNoClass : graph->lookup(ctx.arg(1).as_string());
       if (wanted == kNoClass) return integer(0);
       std::int32_t count = 0;
       for (const Settlement& other : economy->settlements().all()) {
         // The original's own test, kept as its own: `supply_target` refuses
         // another owner's stronghold, so this decides nothing on its own.
         if (other.owner != self->owner) continue;
         const WorldObject* anchor = world->find(other.anchor);
         if (anchor == nullptr) continue;
         bool heir = false;
         for (const ClassIndex ancestor : graph->ancestry(anchor->class_index)) {
           if (ancestor == wanted) heir = true;
         }
         if (!heir) continue;
         if (supply_target(*world, *economy, other) == self) ++count;
       }
       return integer(count);
     }},
    // `set.IsCity()` -- 1 site, and it is a **hard-coded pair of class names**.
    //
    // 0x005c2890 resolves the settlement's central building, takes the class
    // descriptor's own name string, and runs two ten-byte `repe cmpsb`
    // comparisons against the literals `"GTownhall"` and `"RTownhall"`. Ten
    // bytes is nine characters and the terminator, so it is exact name
    // equality and not ancestry: a Gaulish or Roman town hall is a city and a
    // Briton, Egyptian, Iberian, Mercenary or Teuton one is not.
    //
    // Nothing about the *settlement* is consulted -- not its kind, not its
    // population, not its size. `SETTLEMENT_BEHAVIOR_AMBIENT.VS` reads it once
    // at start-up and uses it to pick which ambient peasants to spawn.
    // `set.GoldSpent(n)` -- 1 site, and the sole blocker of
    // `TAVERN_INVESTMENT.VS`. 0x005c29f0 takes the **settlement's own owner**
    // (`[set+0x90]` is the player record and `[record+8]` its index), walks to
    // that player's score block and adds the argument to `[+0x38]`, which is
    // `PlayerScoreCounters::gold`. `sim/match.hpp` has named this entry point
    // as one of that counter's three writers since `GetTeamOverallScore` was
    // decoded; this is it arriving.
    //
    // It adds, and it does not check the sign: a script saying it spent -50
    // gets -50. Nothing clamps it in the original either.
    {CallKind::member, "GoldSpent", 1,  // 1 site
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       World* world = world_of(ctx);
       if (s == nullptr || world == nullptr) return no_settlement(ctx);
       if (MatchSystem* match = match_system_of(*world); match != nullptr) {
         match->record_gold_spent(s->owner, int_arg(ctx, 1));
       }
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "IsCity", 0,  // 1 site
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       World* world = world_of(ctx);
       if (s == nullptr || world == nullptr) return boolean(false);
       const ClassGraph* graph = world->class_graph();
       const WorldObject* anchor = world->find(s->anchor);
       if (graph == nullptr || anchor == nullptr || anchor->class_index == kNoClass) {
         return boolean(false);
       }
       const std::string_view name = graph->at(anchor->class_index).id;
       return boolean(name == "GTownhall" || name == "RTownhall");
     }},
    // `set.PopulationDied()` -- 1 site, and **it is always zero**.
    //
    // 0x005c2930 is a plain read of the int at `[settlement + 0xf0]`, and that
    // field has no writer: a sweep of the whole `.text` for a `mov`/`inc`/
    // `dec`/`add`/`sub` into `[reg + 0xf0]` finds four, and all four are stack
    // slots (`[esp + 0xf0]`). It is the same shape as `Squad::IsEnemyInSquadSight`'s
    // `[squad+0x7c]` and `Squad::Eval`, and the same answer follows: the reader
    // is bound so that its one caller runs, and the value it reads is the one
    // the original also reads, because nothing in either engine ever writes it.
    //
    // `SETTLEMENT_BEHAVIOR_AMBIENT.VS` compares it against the population it
    // sampled at start-up, so a permanent zero means that comparison is never
    // true and the block behind it never runs -- there too.
    //
    // The original dereferences null on an invalid receiver (`[0 + 0xf0]`,
    // 0x005c2962) rather than reporting; this answers zero.
    {CallKind::member, "PopulationDied", 0,  // 1 site
     +[](CallContext& ctx) -> HostOutcome {
       (void)receiver(ctx);
       return integer(0);
     }},
    // `set.SetFoodProduction(n)` -- 1 site, the *Food Tax* research, and the
    // last of the three names `ONFINISH_RESEARCH.VS` and its 146 call sites
    // were blocked on.
    //
    // 0x005c25b0 writes the argument into `[settlement + 0x48]`, and that is
    // `food_rate`: `Settlement::food_rate` has carried the note "Mutable,
    // because the Food Tax upgrade sets `StrongholdFoodProduction` on a live
    // settlement" since the production block was written, and the one caller is
    // exactly `set.SetFoodProduction(GetConst("StrongholdFoodProduction"))`
    // under `if (name == "Food Tax")`.
    //
    // It is a **rate**, not a stock -- percent of population produced per
    // `ProductionInterval` -- so the value replaces the class's, and a
    // settlement that produced nothing starts producing.
    {CallKind::member, "SetFoodProduction", 1,  // 1 site
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return no_settlement(ctx);
       s->food_rate = int_arg(ctx, 1);
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "IsStronghold", 0,  // 33 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->is_stronghold());
     }},
    {CallKind::member, "GoldConverted", 1,  // 16 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       economy->record_gold_converted(s->id, int_arg(ctx, 1));
       // And the owner's *Other* gold on the report: 0x005c29d3 adds the
       // argument to the settlement's player's `gold_outpost`.
       if (World* world = world_of(ctx); world != nullptr) {
         if (MatchSystem* match = match_system_of(*world); match != nullptr) {
           match->record_gold_converted(s->owner, int_arg(ctx, 1));
         }
       }
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "loyalty", 0,  // 12 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : integer(s->loyalty);
     }},
    // The `onenter` hook rides on these two and **not** on `garrison_add`
    // itself, which is the distinction the original's own placement makes:
    // `0x005d3e10` fires only after the holder has accepted the object, and
    // only on the path a *unit entering* takes. A roster that gains a member
    // some other way is not an entry -- `read_save` restores a garrison
    // wholesale, and `UNIT_ON_ENTER.VS` converts the unit's Spoils of War into
    // settlement gold, which must happen once per entry and never again on a
    // reload. So the fire sits at the event and not at the mutation.
    {CallKind::member, "AddUnit", 1,  // 13 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       const Value& unit = ctx.arg(1);
       if (!unit.is_object()) return boolean(false);
       World* world = world_of(ctx);
       if (world == nullptr) return boolean(false);
       if (!garrison_enter(*world, s->id, unit.as_object().id, /*force=*/false)) {
         return boolean(false);
       }
       enter_settlement(ctx, *s, unit.as_object().id);
       return boolean(true);
     }},
    {CallKind::member, "ForceAddUnit", 1,  // 12 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       const Value& unit = ctx.arg(1);
       if (!unit.is_object()) return boolean(false);
       World* world = world_of(ctx);
       if (world == nullptr) return boolean(false);
       if (!garrison_enter(*world, s->id, unit.as_object().id, /*force=*/true)) {
         return boolean(false);
       }
       enter_settlement(ctx, *s, unit.as_object().id);
       return boolean(true);
     }},
    // `set.CreateShip(class)` -- 1 site, and it was the sole blocker of
    // `SHIPYARD_BUILD_SHIP.VS` (9 sites): the shipyard's `build` command,
    // `ship = .CreateShip(cmdparam); if (ship.IsValid) ship.ShowBuildAnimation(
    // .GetCentralBuilding.GetExitVector())`.
    //
    // `Settlement::CreateShip` (0x005ef240) is registered `[45, 2, 27, 11]`: a
    // `Ship`, from a settlement and a class name. An invalid receiver prints
    // `The function 'Settlement::CreateShip' called for an uninitialized or
    // invalid object.` and answers 0xffff. Otherwise, in order: the class is
    // resolved by name and an object of it created (0x005a66a0, with the
    // "current settlement" global at 0x8c8e58 cleared for the duration and
    // restored after, so the ship is not filed under whoever was building);
    // the settlement's owner is handed to it through `vtbl + 0xa0`, which is
    // `Place`'s own step; the central building's door list is built and its
    // **first water door** taken (0x004db650 with list 1 and a `(-1, -1)`
    // origin, so the nearest-wins picker `GetEnterPoint` uses answers the
    // door nearest the top-left corner); the ship is put there through
    // `SetPos`; and 0x005d3e10 -- the holder entry `AddUnit` fires, the one
    // `sim/hooks.hpp` records -- garrisons it in the settlement's holder.
    // The handle is read from `word [obj+8]`.
    //
    // So a new ship is born *inside* the settlement, at a berth the entrance
    // domain already derives, and the script's `ShowBuildAnimation` is what
    // the player sees of it. The door list, the garrison and the owner are
    // this engine's own: `building_doors` with the water list, and
    // `garrison_enter` followed by the entry hook, exactly as `AddUnit` does
    // it -- so the ship is born inside, at the held sentinel, and its first
    // `Goto` takes it out by the water door nearest where it is going
    // (`garrison_exit`). A class nothing declares, or one that is not a ship, answers the
    // invalid handle; the original would have created whatever the name
    // said, and every shipped `cmdparam` on a shipyard names a ship. A door
    // list that comes out empty puts the ship at the central building.
    {CallKind::member, "CreateShip", 1,  // 1 site
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       World* world = world_of(ctx);
       if (s == nullptr || economy == nullptr || world == nullptr) return HostOutcome::ok_with(invalid_handle());
       if (!ctx.arg(1).is_string()) return HostOutcome::ok_with(invalid_handle());
       const ClassGraph* graph = world->class_graph();
       if (graph == nullptr) return HostOutcome::ok_with(invalid_handle());
       const ClassIndex class_index = graph->lookup(ctx.arg(1).as_string());
       if (class_index == kNoClass) return HostOutcome::ok_with(invalid_handle());
       const Result<NativeClass> native = native_class_from_name(graph->at(class_index).cpp_class);
       if (!native || *native != NativeClass::ship) return HostOutcome::ok_with(invalid_handle());

       const ObjectId ship = world->spawn_of_class(class_index);
       if (ship == kNoObject) return HostOutcome::ok_with(invalid_handle());
       world->set_owner(ship, s->owner);

       Point berth = s->anchor != kNoObject ? world->resolve_position(s->anchor)
                                            : world->resolve_position(s->object);
       if (s->anchor != kNoObject) {
         MovementSystem* movement = movement_system(*world);
         std::vector<Point> doors;
         building_doors(*world, movement == nullptr ? nullptr : &movement->grid(), s->anchor,
                        kEnterPointType, /*water=*/true, doors);
         if (!doors.empty()) {
           // Nearest to `(-1, -1)`: the picker's origin at 0x004db650.
           Point best = doors.front();
           std::int64_t best_d2 = -1;
           for (const Point door : doors) {
             const std::int64_t dx = static_cast<std::int64_t>(door.x) + 1;
             const std::int64_t dy = static_cast<std::int64_t>(door.y) + 1;
             const std::int64_t d2 = dx * dx + dy * dy;
             if (best_d2 < 0 || d2 < best_d2) {
               best = door;
               best_d2 = d2;
             }
           }
           berth = best;
         }
       }
       world->set_position(ship, berth);
       if (garrison_enter(*world, s->id, ship, /*force=*/false)) enter_settlement(ctx, *s, ship);
       return HostOutcome::ok_with(Value::object(kTypeObj, ship));
     }},
    {CallKind::member, "IsTeutonTent", 0,  // 11 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->is_teuton_tent());
     }},
    // `IsTTent` and `IsTeutonTent` are two shipped entry points, not one name
    // spelled twice, and `gbr.exe` implements them differently: `IsTeutonTent`
    // (`0x00420650`) compares the central building's class *id* to the literal
    // `"TTent"` with `repe cmpsb`, while `IsTTent` (`0x00440100`) resolves the
    // class once and asks `IsHeirOf`. No shipped class declares `parent="TTent"`
    // so the two agree on every retail map, and `SettlementKind::teuton_tent`
    // -- which sim/session.cpp assigns by ancestry -- is the `IsTTent` reading.
    //
    // One condition of the original is deliberately not reproduced. Before the
    // class test, `IsTTent` fetches the settlement's unit holder (the same
    // `+0x5e` handle `Settlement::Units` returns) and answers false if
    // `0x005319a0` resolves on it -- the helper `Unit::InShip` uses to ask
    // whether a holder is a ship's. A settlement's holder is never a ship's in
    // this model, so the condition is not merely unreached, it is
    // unrepresentable; there is nothing here that could make it fire.
    {CallKind::member, "IsTTent", 0,  // 3 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->is_teuton_tent());
     }},
    // `IsIndependentGuarded` -- an independent outpost that *comes with a
    // garrison*, which is the one thing on a peaceful map you are allowed to
    // attack. It is the first half of `ATTACK_INDEPENDENT_VERIFY.VS`, and
    // therefore of every right click on a building: `attack_independent` leads
    // the `target="Building"` candidate list.
    //
    // `gbr.exe` `0x004401c0`, three conditions in this order:
    //
    //   1. `IsOutpost` -- literally the same function the entry point of that
    //      name calls (`0x00440060`), so the `IsOutpost()` the verify script
    //      writes around this call is exactly that: redundant.
    //   2. `IsIndependent` -- the same owner test, inlined: player 14 or 15.
    //   3. the central building's class declares a non-empty `defender_cls_1`
    //      or `defender_cls_2` (class descriptor `+0xa28` / `+0xa44`, the
    //      lengths of the two interned strings the class reader fills at
    //      `0x005a14c6` and `0x005a1534`).
    //
    // Only the six race-specific outpost classes carry `defender_cls_*` in the
    // shipped data, and `Outpost` itself carries neither -- so a plain outpost
    // is independent but not guarded, and answers false.
    {CallKind::member, "IsIndependentGuarded", 0,  // 4 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return no_settlement(ctx);
       if (!s->is_outpost() || !s->independent()) return boolean(false);
       World* world = world_of(ctx);
       if (world == nullptr) return boolean(false);
       const WorldObject* central = world->find(s->anchor);
       const ClassGraph* graph = world->class_graph();
       if (central == nullptr || graph == nullptr || central->class_index == kNoClass) {
         return boolean(false);
       }
       return boolean(!graph->property(central->class_index, "defender_cls_1").empty() ||
                      !graph->property(central->class_index, "defender_cls_2").empty());
     }},
    {CallKind::member, "CreateMuleGold", 1,  // 10 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       const std::uint32_t wagon =
           economy->create_mule(s->id, s->supplied, Resource::gold, int_arg(ctx, 1));
       const Wagon* w = economy->find_wagon(wagon);
       // The cargo and the endpoints are the economy's; the object that carries
       // them is movement's, and until one is spawned this is an invalid handle
       // rather than a fabricated id.
       return HostOutcome::ok_with(w == nullptr ? invalid_handle() : object_handle(w->object));
     }},
    {CallKind::member, "CreateMuleFood", 1,  // 8 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       const std::uint32_t wagon =
           economy->create_mule(s->id, s->supplied, Resource::food, int_arg(ctx, 1));
       const Wagon* w = economy->find_wagon(wagon);
       return HostOutcome::ok_with(w == nullptr ? invalid_handle() : object_handle(w->object));
     }},
    {CallKind::member, "StopSupply", 0,  // 7 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return no_settlement(ctx);
       s->supplied = kNoSettlement;
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "StartSupplyFood", 1,  // 6 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       Settlement* target = settlement_at(ctx, 1);
       return boolean(
           economy->set_supplied(s->id, target == nullptr ? kNoSettlement : target->id));
     }},
    {CallKind::member, "supplied", 0,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       Settlement* target = economy->find(s->supplied);
       return HostOutcome::ok_with(target == nullptr ? invalid_handle()
                                                     : settlement_handle(*target));
     }},
    {CallKind::member, "IsShipyard", 0,  // 6 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->is_shipyard());
     }},
    {CallKind::member, "max_units", 0,  // 5 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : integer(s->holder.max_units);
     }},
    {CallKind::member, "IsFull", 0,  // 4 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->holder.full());
     }},
    {CallKind::member, "SetLoan", 1,  // 3 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return no_settlement(ctx);
       s->loan = std::max(0, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "loan", 0,  // 3 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : integer(s->loan);
     }},
    {CallKind::member, "SetLoyalty", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       economy->set_loyalty(s->id, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "DecreaseLoyalty", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       return integer(economy->decrease_loyalty(s->id, int_arg(ctx, 1)));
     }},
    {CallKind::member, "CanBeCaptured", 0,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : boolean(s->can_be_captured);
     }},
    // `AllowCapture(bool)` -- 7 sites, and **it writes the field `CanBeCaptured`
    // reads**, which is why it needs no state of its own. `0x005c1ce0` stores
    // its argument at `[settlement + 0x3c]`, and `Settlement::CanBeCaptured`
    // (0x00424ff0) loads `[edi + 0x3c]`: one word, a setter and a getter. The
    // class property this engine seeds it from is the *initial* value, not a
    // separate thing.
    //
    // Every shipped use is an adventure script locking a settlement and
    // unlocking it later -- `1_Great_Losses_Rome:seq15.vs` closes
    // `S_TraitorTown`, waits two minutes and a conversation, then reopens it
    // and hands it over -- so the default has to stay the class's `true` and
    // the entry point has to be able to put it back.
    //
    // An unresolvable receiver prints and writes nothing.
    {CallKind::member, "AllowCapture", 1,  // 7 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return no_settlement(ctx);
       s->can_be_captured = int_arg(ctx, 1) != 0;
       return HostOutcome::ok_void();
     }},
    // The three that move sentries in and out of the wall.
    //
    // **`GetNumSentries` and `GetMaxSentries` are not these.** The settlement
    // carries four sentry numbers and the scripts read two of them: the roster
    // at `[+0xfc]` and its ceiling at `[+0x104]`. The other two are
    // `[+0xf4]`, how many of the roster are *inside the wall*, and `[+0x100]`,
    // which nothing reads at all.
    //
    // `set.GetSentry()` -- 4 sites -- takes one out of the wall and answers
    // whether there was one. 0x005c49b0 tests `[+0xf4] > 0`, lowers it by one
    // through the upkeep-keeping setter, and pushes **1**; an empty wall pushes
    // 0 and changes nothing. `GATE_PATROL.VS` is the shape it exists for:
    // `if (set.GetSentry()) sent = Place(sentry_class_name, ...)`.
    //
    // `set.PutSentry()` -- 1 site -- is the other half. `SENTRY_DISAPPEAR.VS`
    // is `if (wall.player == me.player) set.PutSentry(); me.Erase();`, a
    // sentry walking back into a wall of its own side. 0x005c4a10 raises
    // `[+0xf4]` by one **and** `[+0x100]`, and it does not check the roster or
    // the ceiling: a wall can end up holding more than it started with.
    //
    // `set.DelSentry()` -- 11 sites, and **it is dropped**. 0x005c2a60 is the
    // whole body: if `[+0x100] > 0`, lower it by one. That field is written in
    // three places -- here, `PutSentry`, and three constructor-shaped stores of
    // zero -- and a sweep of `.text` for a read of `[reg+0x100]` inside the
    // settlement code finds no other. Nothing in `gbr.exe` and nothing in 885
    // scripts asks what it holds. It is the shape `Squad::IsEnemyInSquadSight`
    // documents from the other side, and storing it would be state with no
    // consumer. The call sites are a sentry dying (`Damage(sent, 10000)` then
    // `DelSentry`) and a wall changing hands, so what it *meant* is legible
    // even though what reads it is not there.
    {CallKind::member, "GetSentry", 0,  // 4 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return no_settlement(ctx);
       if (s->sentries_ready <= 0) return boolean(false);
       s->sentries_ready -= 1;
       return boolean(true);
     }},
    {CallKind::member, "PutSentry", 0,  // 1 site
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return no_settlement(ctx);
       // No ceiling: the original raises the count and asks nothing.
       s->sentries_ready += 1;
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "DelSentry", 0,  // 11 sites, and nothing reads what it writes
     +[](CallContext& ctx) -> HostOutcome {
       (void)receiver(ctx);
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "GetNumSentries", 0,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : integer(s->sentries);
     }},
    {CallKind::member, "EvalSentries", 0,  // 1 site; see `eval_sentries`
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       Settlement* s = receiver(ctx);
       if (world == nullptr || s == nullptr) return no_settlement(ctx);
       return integer(eval_sentries(*world, *s));
     }},
    {CallKind::member, "GetMaxSentries", 0,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       return s == nullptr ? no_settlement(ctx) : integer(s->max_sentries);
     }},
    {CallKind::member, "AddSentries", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       economy->add_sentries(s->id, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "AddMaxSentries", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       economy->add_max_sentries(s->id, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "AddToPopulation", 1,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       economy->add_population(s->id, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "SetPopulation", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       economy->set_population(s->id, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    // 4 sites, three of them campaign sequences, and the sole blocker of
    // `6_Great_loses_Boudicca` map 1 sequence 10. It is `AddToPopulation`'s
    // sibling one field over -- and without it `ONFINISH_RESEARCH.VS`'s
    // "Housing" was the one researchable that did nothing at all.
    {CallKind::member, "AddToMaxPopulation", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       economy->add_max_population(s->id, int_arg(ctx, 1));
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "DonateFood", 1,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       return integer(economy->deposit(s->id, Resource::food, int_arg(ctx, 1)));
     }},
    {CallKind::member, "GetLoan", 0,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       return boolean(economy->take_loan(s->id));
     }},
    {CallKind::member, "RepayLoan", 0,
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (s == nullptr || economy == nullptr) return no_settlement(ctx);
       return boolean(economy->repay_loan(s->id));
     }},

    // -- free functions ---------------------------------------------------
    // `GetSettlement(name)` -- 118 sites, and the first executable line of
    // `DATA\AI HELPERS\SIEGE.VS` and `GUARD.VS`.
    //
    // A linear scan of the settlement vector in `ID` order comparing the map's
    // `<settlement name>` byte for byte, which is what `gbr.exe` 0x005c3ad0
    // does: 0x005c3b17 walks `[mgr+0x4c, mgr+0x50)`, 0x005c3b41 takes the
    // `std::string` at `settlement+0xcc`, and 0x005c3b63 compares it against
    // the argument with its exact length. Case-sensitive, and `find_by_name`
    // refuses the empty name so the 496 unnamed settlements stay unreachable
    // rather than all matching each other.
    //
    // **A miss is not a refusal.** 0x005c3bad prints "Could not find settlement
    // named '%s' in function 'GetSettlement'. Check the spelling." and pushes
    // the 0xffff sentinel -- an invalid handle -- and the script runs on. Both
    // helper scripts are written for exactly that: `set = GetSettlement(Target);
    // if (!set.IsValid) return;`. Trapping here would kill a helper the
    // original exits cleanly.
    {CallKind::free_function, "GetSettlement", 1,
     +[](CallContext& ctx) -> HostOutcome {
       EconomySystem* economy = host_economy(ctx);
       if (economy == nullptr) return HostOutcome::failed("GetSettlement: no economy");
       if (ctx.count() != 1 || !ctx.arg(0).is_string()) {
         return HostOutcome::failed("GetSettlement expects a settlement name");
       }
       const Settlement* s = economy->settlements().find_by_name(ctx.arg(0).as_string());
       return HostOutcome::ok_with(s == nullptr ? invalid_handle() : settlement_handle(*s));
     }},
    // `unit.GetHolderSett` -- 34 sites. The settlement of whatever is holding
    // this object, not the settlement it belongs to: `gbr.exe` 0x005d70a5 walks
    // `unit+0x154` (the holder handle) to the holder, then `holder+0xc` to the
    // settlement, then reads `settlement+0x8`. Either hop failing gives the
    // same 0xffff an invalid receiver gives, so this never traps.
    //
    // The chain is why it exists. `SIEGE.VS` ends with
    // `if (u.GetHolderSett.IsValid) { cat = u.GetHolderSett.GetCentralBuilding
    // .AsCatapult; if (cat.IsValid) cat.SetCommand("stop"); }` -- a crewman
    // inside a catapult, reaching the catapult through the settlement the
    // engine allocates for it. `sim/settlement.hpp` records `RCatapult` among
    // the 31 anchor classes the dumps show, from the other direction.
    // The mule's load. Four entry points over the two fields `ObjectState`
    // carries for them, and they are on the **object**: `Wagon::amount`
    // (0x005ebdb0) and `Wagon::restype` (0x005ebe10) are plain reads of
    // `[obj+0x1cc]` and `[obj+0x1d4]`, not lookups in the shipment table this
    // system keeps for settlement-to-settlement trade.
    //
    // `ES_OUTPOSTSELLGOLD.VS` is why that distinction is load-bearing and the
    // reason 73 call sites were blocked: it walks a *group of objects*
    // (`Group("GoldMules" + player)`), takes `AsWagon(o).amount` off each, and
    // sells when the total is worth the trip. There is no shipment record to
    // ask; there are mules standing in the world.
    {CallKind::member, "amount", 0,  // 4 sites
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("amount: no world");
       const WorldObject* slot =
           ctx.arg(0).is_object() ? world->find(ctx.arg(0).as_object().id) : nullptr;
       return integer(slot == nullptr ? 0 : slot->state.cargo);
     }},
    // `SelectionGold` and `SelectionFood` -- two bare identifiers with no call
    // site in any `.vs` file: the `Wagons` pseudo-class's `<value0>` and
    // `<value1>` (`return SelectionGold;`), which the info bar evaluates over
    // a selection of mules. Registered at 0x004cb01c/0x004cb02e as
    // zero-arity free functions, which is how a bare name resolves; the
    // bodies (0x004c8680, 0x004c8740) walk the local player's selection and
    // add `[obj+0x1cc]` -- `cargo` -- for every wagon whose `[obj+0x1d4]` is
    // the resource asked for.
    {CallKind::free_function, "SelectionGold", 0,
     +[](CallContext& ctx) -> HostOutcome { return integer(selection_cargo(ctx, Resource::gold)); }},
    {CallKind::free_function, "SelectionFood", 0,
     +[](CallContext& ctx) -> HostOutcome { return integer(selection_cargo(ctx, Resource::food)); }},
    {CallKind::member, "restype", 0,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("restype: no world");
       const WorldObject* slot =
           ctx.arg(0).is_object() ? world->find(ctx.arg(0).as_object().id) : nullptr;
       return integer(slot == nullptr ? 0 : slot->state.cargo_resource);
     }},
    // `w.LoadGold(n)` and `w.LoadFood(n)` -- 2 sites each, and between them the
    // whole of four shipped scripts: `WAGON_LOADGOLDBIG.VS` and its three
    // siblings are `AsWagon(This).LoadGold(1000)` and nothing else.
    //
    // 0x005ebfd0 and 0x005ebf10 are the same body with the resource swapped.
    // In order:
    //
    //   1. **the settlement the mule is standing in**, walked from the mule's
    //      holder the way `GetHolderSett` above walks it (0x005ebc90). No
    //      holder, or a holder with no settlement, and nothing happens.
    //   2. **the mule must be empty or already carrying this kind.** The test
    //      is `[+0x1cc] == 0 || [+0x1d4] == <this resource>`, so a half-loaded
    //      gold mule refuses food and a *spent* one takes either -- the kind
    //      field is not cleared when the cargo is.
    //   3. **the request is clamped to what is left of `max_load`**, the class
    //      property at descriptor `+0xa10`, and a negative request clamps to
    //      zero rather than unloading.
    //   4. the settlement's warehouse is drawn down by whatever it can pay, and
    //      the mule's cargo goes up by exactly that -- so an empty warehouse
    //      loads nothing and is not an error.
    //
    // The kind is written **before** the warehouse is asked, so a mule that
    // asked for food and got none still reports `restype == food`. Reproduced.
    // `set.CreateBoatGold(n)` / `CreateBoatFood(n)` -- 3 sites each, and
    // between them the whole of four scripts: `CREATE_GOLD_BOAT_BIG.VS` and its
    // three siblings differ only in `1000` against `250` and which of the two
    // they call.
    //
    // 0x005ef1c0 and 0x005ef0c0 are the same body with the resource swapped --
    // literally 0 and 1, `Resource`'s own numbering again -- handing the
    // settlement's warehouse to 0x005eed90:
    //
    //   1. the class is **`ShipS`**, always. 0x005eed10 has a per-race jump
    //      table but both of these pass the flag that takes its first arm, the
    //      one cached class at `[0x009c0974]`, which 0x005efdcf loads by that
    //      name;
    //   2. the request is clamped to that class's `max_load` (1000 shipped) and
    //      then to what the warehouse holds, and the warehouse is drawn down by
    //      exactly what is loaded;
    //   3. **nothing loaded, no boat.** A zero after both clamps returns an
    //      invalid handle, and `CREATE_FOOD_BOAT_BIG.VS` tests exactly that
    //      before it does anything else;
    //   4. otherwise a `ShipS` is spawned, given the cargo and the kind, owned
    //      by the settlement's own player, and **put inside the settlement's
    //      holder** -- which is where the script's `Select` and `SetCommand`
    //      then walk it out of.
    //
    // A **negative** request is the one thing not reproduced: the original
    // clamps only downwards, so `CreateBoatFood(-100)` would put 100 food *into*
    // the warehouse and hand back a boat carrying -100. No shipped site passes
    // one -- all six pass 1000 or 250 -- and importing it would mean writing
    // past `Warehouse::set`'s own clamp, which is a real invariant here and was
    // not one there.
    {CallKind::member, "CreateBoatGold", 1, &create_boat<Resource::gold>},  // 3 sites
    {CallKind::member, "CreateBoatFood", 1, &create_boat<Resource::food>},  // 3 sites
    {CallKind::member, "LoadGold", 1, &load_cargo<Resource::gold>},   // 2 sites
    {CallKind::member, "LoadFood", 1, &load_cargo<Resource::food>},   // 2 sites
    {CallKind::member, "GetHolderSett", 0,
     +[](CallContext& ctx) -> HostOutcome {
       EconomySystem* economy = host_economy(ctx);
       World* world = world_of(ctx);
       if (economy == nullptr || world == nullptr) {
         return HostOutcome::failed("GetHolderSett: no world");
       }
       if (ctx.count() == 0 || !ctx.arg(0).is_object()) {
         return HostOutcome::ok_with(invalid_handle());
       }
       const WorldObject* slot = world->find(ctx.arg(0).as_object().id);
       // The `holder == kNoObject` half is redundant with `for_object`'s own
       // null check and is kept as an early-out, not as a guarantee: injecting
       // a fault into it alone changes no test, because the other one catches
       // it. What must not be removed is *both*, and
       // `the_holders_settlement_is_a_two_hop_walk...` asserts the property
       // itself rather than either guard.
       if (slot == nullptr || slot->state.holder == kNoObject) {
         return HostOutcome::ok_with(invalid_handle());
       }
       const Settlement* s = economy->settlements().for_object(slot->state.holder);
       return HostOutcome::ok_with(s == nullptr ? invalid_handle() : settlement_handle(*s));
     }},
    // `GetConst`/1 is **not** here. It used to be, answering the economy's
    // slice of `CONST.INI` and trapping on everything else; it now lives in
    // `sim/env.cpp`, which reads the whole `[GamePlay]` section through the INI
    // reader. Two `define()` calls on one (kind, name, arity) do not coexist --
    // the second replaces the first silently -- and `sim/host_setup.cpp` runs
    // env after economy, so leaving this here would have made the economy's
    // definition dead code that still looked live. `economy_constant` below is
    // what env calls to keep `EconomySystem::set_rules` authoritative for the
    // keys `EconomyRules` holds as fields.
    // `IdxToSet/1` was here too, reading its argument as a `SettlementId` and
    // looking the settlement up by it. It has moved to `sim/objlist.cpp`, whole
    // and not merely deduplicated, and the reason is not that one reading of
    // the original beat the other.
    //
    // **`IdxToSet` and `MaxSetIdx` have to share one numbering.** Every one of
    // the five corpus sites is `for (i = 0; i < MaxSetIdx; i += 1) { s =
    // IdxToSet(i); ... }`, so a bound counted in one index space and a lookup
    // performed in another enumerate different things -- silently, and only
    // once the two spaces diverge. `MaxSetIdx` counts settlement objects in
    // world order; `IdxToSet` now indexes the same sequence.
    //
    // -- the outpost descriptor, six getters ------------------------------
    //
    // See the block comment above `kOutpostUnresolved` for the gate, the slot
    // dispatch, and which half of the class-descriptor sentinel survives the
    // inheritance resolver. Registration order here is `gbr.exe`'s at
    // `0x00564540`: Cls, Max, Out, Start, End, Food.
    //
    // `GetDefendersMax` is 38 of the 65 sites and `GetOutpostFood` 19, and the
    // reason is the same in both cases: eighteen sequences -- nine in
    // `2_Great_loses_Spain` map 2 and nine in `6_Great_loses_Boudicca` map 1 --
    // are hand-edited forks of `OUTPOST_BEHAVIOR.VS`, one per named outpost,
    // that hardcode the defender classes and the counts and keep only these
    // two calls. The other four names appear in the installation exactly twice
    // each, all four in the pack's original.
    {CallKind::member, "GetDefenderCls", 1,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome { return outpost_name(ctx, "defender_cls"); }},
    {CallKind::member, "GetDefendersMax", 1,  // 38 sites
     +[](CallContext& ctx) -> HostOutcome {
       return outpost_number(ctx, "defenders_max", true, 0);
     }},
    {CallKind::member, "GetDefendersOut", 1,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       return outpost_number(ctx, "defenders_out", true, 0);
     }},
    {CallKind::member, "GetStartLevel", 1,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       return outpost_number(ctx, "start_level", true, 0);
     }},
    {CallKind::member, "GetEndLevel", 1,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       return outpost_number(ctx, "end_level", true, 0);
     }},
    // The one with no slot, and the one field in the block the resolver's
    // normalise pass does not reach: an undeclared `settlement_food` answers
    // the sentinel rather than zero.
    {CallKind::member, "GetOutpostFood", 0,  // 19 sites
     +[](CallContext& ctx) -> HostOutcome {
       return outpost_number(ctx, "settlement_food", false, kOutpostUnresolved);
     }},
    // `InHolder(str)` -- **an int, not a bool**, and the arity is what says so.
    //
    // `gbr.exe` registers four `*InHolder` methods and no two of them are the
    // same function. `Unit::InHolder` (0x005d6ff0, arity 0) is the predicate
    // the object model already answers -- `word[unit+0x154] != 0xffff`, 154
    // sites. `Squad::InHolder` (0x00427670) is another predicate. But
    // `ObjList::InHolder` (0x0055eba0) and `Query::InHolder` (0x00579ef0) take
    // a **settlement name** and return `retType 1`, an int: they *count* how
    // many members of the collection are inside that settlement's holder.
    //
    // Both shipped sites compare the result against `.count`, which is what
    // settles it beyond the registration:
    //
    //     while (T_UnitedArmy.InHolder("S_Pelusio") < T_UnitedArmy.count)
    //     if (T_CleopatraArmy.count - T_CleopatraArmy.InHolder("S_Alejandria") != 0)
    //
    // A bool would make the first spin forever and the second always true. This
    // is the third name in the installation whose *arity* is the only thing
    // distinguishing two unrelated functions, after `Dist` and `PlayAnim`.
    //
    // A name no settlement carries is **zero**, not a refusal: 0x0055ebe6
    // formats "Could not find settlement named '%s' in function
    // 'ObjList::InHolder'. Check the spelling." into the sink that is a bare
    // `ret` and pushes 0. Members that are not units, and handles that do not
    // resolve, are skipped in silence.
    {CallKind::member, "InHolder", 1,  // 2 sites, both in 4_Great_Battles_Egypt
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       EconomySystem* economy = host_economy(ctx);
       if (world == nullptr) return HostOutcome::failed("InHolder: no world");
       if (economy == nullptr || !ctx.arg(1).is_string()) return integer(0);
       const Settlement* target =
           economy->settlements().find_by_name(ctx.arg(1).as_string());
       if (target == nullptr) return integer(0);
       const ObjectId holder = target->holder.object;
       if (holder == kNoObject) return integer(0);
       std::int32_t count = 0;
       for (const ObjectId id : receiver_objects(*world, ctx.arg(0))) {
         const WorldObject* slot = world->find(id);
         // The original casts each element to `Unit` through the vtable before
         // it compares, so a building in the list is skipped rather than
         // matched on its holder.
         if (slot == nullptr || !slot->state.flags.is_unit) continue;
         if (slot->state.holder == holder) ++count;
       }
       return integer(count);
     }},
    {CallKind::member, "SpentGoldOnArmy", 1, &spent_gold_impl<true>},   // 15
    {CallKind::member, "SpentGoldOnTech", 1, &spent_gold_impl<false>},  //  2
    {CallKind::member, "StopReserving", 0, &stop_reserving_impl},        //  7
    {CallKind::member, "ReserveFor", 2, &reserve_for_impl},              //  1
    {CallKind::member, "CheckTechBudget", 2, &check_tech_budget_impl},   // 162
    {CallKind::member, "RepairAll", 0, &repair_all_impl},                //  1
    // `GetTrainGold(cmd)` -- 1 site, `ESH_COUNTERUNITS.VS`: `GetTrainGold(UTrainCmd(i,
    // nRace)) / 10`, the gold a unit type costs to train, as a weight. 0x004229f0
    // pops the string, looks the row up by name through the command table
    // (0x009969dc, the lookup `UpgradeBestBarrack` uses) and answers the row's
    // `+0x1e8` -- `costgold` -- or 0 for a name no row carries. Nothing else.
    {CallKind::free_function, "FindRuins", 3, &find_ruins_impl},  // 2, SQUADMONITOR and TS_ATTACKATWILL
    {CallKind::free_function, "AttackSetForTraining", 3, &attack_set_for_training_impl},  // 1, and TS_ATTACKATWILL with it
    // `GetCounterUnits(set, W)` -- 1 site, `ESH_COUNTERUNITS.VS`, and its sole
    // blocker with `GetTrainGold`: the six weights the AI draws a unit type
    // from. See `get_counter_units_impl`.
    {CallKind::free_function, "GetCounterUnits", 2, &get_counter_units_impl},
    {CallKind::free_function, "GetTrainGold", 1,
     +[](CallContext& ctx) -> HostOutcome {
       if (!ctx.arg(0).is_string()) return HostOutcome::ok_with(Value::integer(0));
       const CommandDef* row = command_row(ctx, ctx.arg(0).as_string());
       return HostOutcome::ok_with(Value::integer(row == nullptr ? 0 : row->cost_gold));
     }},
    {CallKind::free_function, "NearestStronghold", 2, &nearest_stronghold_impl},  // 6
    {CallKind::free_function, "NearestStronghold", 3, &nearest_stronghold_impl},  // 1
    {CallKind::member, "BestBarrack", 1, &best_barrack_impl},           //  9
    {CallKind::member, "UpgradeBestBarrack", 1, &upgrade_best_barrack_impl},  // 2
    {CallKind::member, "TSGetAllBarracks", 0, &ts_get_all_barracks_impl},  // 2
    // The arena and the temple, one name each where the barracks walk has
    // eight. All four are one site, and all four are exactly what
    // `TSH_ArenaRecruit.vs` and `TSH_TempleRecruit.vs` need.
    // Two receivers on one name, and one deterministic sibling; see
    // `m_get_enter_exit`.
    {CallKind::member, "GetEnterExit", 0, &m_get_enter_exit},        // 14
    {CallKind::member, "WaterLsa", 0, &m_water_lsa},                //  7
    {CallKind::member, "FindNearEnterExit", 1, &m_find_near_enter_exit},  // 1
    {CallKind::member, "BestArena", 0,
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("BestArena: no world");
       return best_of_class(ctx, *world, receiver(ctx), kArenaClass);
     }},
    {CallKind::member, "BestTemple", 0,
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("BestTemple: no world");
       Settlement* s = receiver(ctx);
       return best_of_class(ctx, *world, s,
                            s == nullptr ? std::string_view{} : temple_class_of(*world, *s));
     }},
    {CallKind::member, "TSGetAllArenae", 0,
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("TSGetAllArenae: no world");
       return all_of_class(ctx, *world, receiver(ctx), kArenaClass, false);
     }},
    {CallKind::member, "TSGetAllTemples", 0,
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return HostOutcome::failed("TSGetAllTemples: no world");
       return all_of_class(ctx, *world, receiver(ctx), {}, true);
     }},
    // `Settlement::UpgradeBestBarrack/1` is above, beside `BestBarrack`: it
    // is the same walk with a different winner (the busiest, not the idlest)
    // and an `ExecCmd`-shaped issue at the end. This note used to say it
    // "prices it against the settlement", and it does not: the availability
    // question is the row's `groupverifier`, whose cost check only sets the
    // tooltip, so the price is the script's business (`set.gold >= 500`).
    // `BestArena/0` and `BestTemple/0`, above, are the same walk over two other
    // class lists. This used to say they had **no shipped call site at all**,
    // and that was wrong: each has exactly one, `TSH_ARENARECRUIT.VS:80` and
    // `TSH_TEMPLERECRUIT.VS:59`, which is the spend half of the recruiter the
    // AI domain now runs. One site is still one, so the `ShowNotes` rule was
    // never in question here -- but a claim of zero is the kind that stops
    // anybody looking again.
    // The open question is which index space the *original* used, and
    // `sim/objlist.cpp` carries it: `DATA\SUBAI\HEN_IDLE.VS` is the only site
    // that guards the result with `if (!set.IsValid) continue;`, which reads
    // like a table with holes rather than a dense enumeration. Nothing here
    // depends on the answer while this world's object vector has no holes.
    // -- the gates ----------------------------------------------------------
    //
    // A settlement's gates are the members of its building list that are
    // `CVXGate`s: 0x00428360 walks the deque at `[settlement+0x68]` and asks
    // each member through a dynamic type query. Here that is
    // `World::buildings_in_settlement` filtered on `NativeClass::gate`, which
    // answers in ascending id where the original answers in the order the
    // buildings joined -- observable only through `BestGate`'s tie, which
    // keeps the earlier of two equal scores.
    //
    // `Gate::Inside(squad)` (0x00429310) is **deliberately absent**. For every
    // member of the squad it runs a path search (0x005295d0, a pathfinder built
    // with flag 0x100) from the member to the settlement's central building
    // and answers true only when each search records nothing -- the reading
    // being that the flag makes the search record the gates it crosses, so a
    // unit is inside when no gate stands between it and the centre. This
    // engine's pathfinder has no notion of a gate crossing; the condition that
    // lifts this is one that does.
    // -- damage taken --------------------------------------------------------
    {CallKind::member, "ClearDamageTaken", 0,  // 2 sites, two receivers
     // `Building::ClearDamageTaken` (0x004ddbb0) zeroes the receiver's own
     // counter; `Settlement::ClearDamageTaken` (0x005c3a00) zeroes every
     // building's. One name in the registry, so the receiver decides.
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return no_settlement(ctx);
       // The handle's *type* decides, not what it resolves to: `receiver`
       // finds a building's settlement too, and `bld.ClearDamageTaken` must
       // not clear the whole town.
       const bool settlement =
           ctx.arg(0).is_object() && ctx.arg(0).as_object().type == kTypeSettlement;
       if (Settlement* s = settlement ? receiver(ctx) : nullptr) {
         std::vector<ObjectId> buildings;
         world->buildings_in_settlement(s->object, ClassFilter{}, buildings);
         for (const ObjectId id : buildings) {
           if (ObjectState* state = world->mutable_state(id)) state->damage_taken = 0;
         }
         return HostOutcome::ok_void();
       }
       const Value& handle = ctx.arg(0);
       if (!handle.is_object()) return HostOutcome::ok_void();
       if (ObjectState* state = world->mutable_state(handle.as_object().id)) {
         state->damage_taken = 0;
       }
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "MostDamagedBuilding", 0,  // 1 site
     // 0x005c38d0: over the settlement's buildings, the one with the most
     // damage taken -- strictly more than the running best, which starts at
     // zero, so an untouched settlement answers nothing -- among those whose
     // entity carries an enter/exit point (bit 12 of the presence mask at
     // `[class+0x63c]`, which is `kEnterExitPointType`). A building nobody can
     // enter is not one the AI sends a repair crew into.
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return HostOutcome::ok_with(invalid_handle());
       World& world = *world_of(ctx);
       std::vector<ObjectId> buildings;
       world.buildings_in_settlement(s->object, ClassFilter{}, buildings);
       ObjectId best = kNoObject;
       std::int32_t most = 0;
       for (const ObjectId id : buildings) {
         const WorldObject* slot = world.find(id);
         if (slot == nullptr || slot->state.damage_taken <= most) continue;
         if (!class_has_point_type(world, id, kEnterExitPointType)) continue;
         best = id;
         most = slot->state.damage_taken;
       }
       return HostOutcome::ok_with(best == kNoObject ? invalid_handle() : object_handle(best));
     }},
    {CallKind::free_function, "SettlementCount", 3,  // 1 site
     // 0x0042ca90: `SettlementCount(around, dist, class)` -- how many
     // settlements stand strictly within `dist` of the point, measured to the
     // central building, whose central building is a `class`. Store order,
     // every settlement, no owner test.
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       EconomySystem* economy = world == nullptr ? nullptr : economy_of(*world);
       if (economy == nullptr) return integer(0);
       if (!is_point(ctx.arg(0)) || !ctx.arg(1).is_integer() || !ctx.arg(2).is_string()) {
         return integer(0);
       }
       const Point around = unpack_point(ctx.arg(0));
       const std::int64_t within = ctx.arg(1).as_integer();
       const ClassGraph* graph = world->class_graph();
       const ClassIndex wanted = graph == nullptr ? kNoClass : graph->lookup(ctx.arg(2).as_string());
       if (wanted == kNoClass) return integer(0);
       std::int32_t count = 0;
       for (const Settlement& s : economy->settlements().all()) {
         const WorldObject* anchor = world->find(s.anchor);
         if (anchor == nullptr) continue;
         const Point at = world->resolve_position(anchor->id);
         const std::int64_t dx = static_cast<std::int64_t>(at.x) - around.x;
         const std::int64_t dy = static_cast<std::int64_t>(at.y) - around.y;
         if (isqrt(dx * dx + dy * dy) >= within) continue;
         bool heir = false;
         for (const ClassIndex ancestor : graph->ancestry(anchor->class_index)) {
           if (ancestor == wanted) heir = true;
         }
         if (heir) ++count;
       }
       return integer(count);
     }},
    {CallKind::member, "NumGates", 0,  // 2 sites
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return integer(0);
       return integer(static_cast<std::int32_t>(settlement_gates(*world_of(ctx), *s).size()));
     }},
    // `Gate::Inside(squad)` -- 1 site, and the receiver is the gate rather than
    // the settlement, which is why it sits with the gate members and not with
    // the settlement's. See `inside_walls` in `sim/gate.hpp`.
    {CallKind::member, "Inside", 1,  // 1 site
     +[](CallContext& ctx) -> HostOutcome {
       World* world = world_of(ctx);
       if (world == nullptr) return boolean(false);
       const WorldObject* gate =
           ctx.arg(0).is_object() ? world->find(ctx.arg(0).as_object().id) : nullptr;
       EconomySystem* economy = economy_of(*world);
       HeroSystem* heroes = hero_system_of(*world);
       if (gate == nullptr || economy == nullptr || heroes == nullptr) return boolean(false);
       const Settlement* set = economy->settlements().for_object(gate->settlement);
       if (set == nullptr) return boolean(false);
       const Squad* squad = heroes->squads().find(unpack_squad(ctx.arg(1)));
       // A handle naming no squad walks no members, which is the same answer an
       // empty one gets: every member of nothing is inside.
       if (squad == nullptr) return boolean(true);
       for (const ObjectId member : squad->members) {
         if (!inside_walls(*world, member, set->anchor)) return boolean(false);
       }
       return boolean(true);
     }},
    {CallKind::member, "BestGate", 1,  // 2 sites, and the sole blocker of SIEGE.VS
     // 0x004284e0: the gate with the least `distance + health / 5` from the
     // point, strictly less, from an initial `0x7fffffff`; nothing at all is
     // the invalid handle. Health divides by five signed and truncating, so a
     // damaged gate is preferred to a sound one at equal distance -- a
     // besieger's question, which is what both call sites are asking.
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr || !is_point(ctx.arg(1))) return HostOutcome::ok_with(invalid_handle());
       World& world = *world_of(ctx);
       const Point at = unpack_point(ctx.arg(1));
       ObjectId best = kNoObject;
       // The initial best is the original's `0x7fffffff`, and no gate reaches
       // it: a fifth of a 32-bit health plus a map-sized distance stays well
       // under. It is kept as the constant it is rather than as a wider one,
       // and a fault that widened it survived a sweep for exactly that reason.
       std::int64_t best_score = 0x7FFFFFFF;
       for (const ObjectId id : settlement_gates(world, *s)) {
         const WorldObject* gate = world.find(id);
         if (gate == nullptr) continue;
         const Point pos = world.resolve_position(id);
         const std::int64_t dx = static_cast<std::int64_t>(pos.x) - at.x;
         const std::int64_t dy = static_cast<std::int64_t>(pos.y) - at.y;
         const std::int64_t score = isqrt(dx * dx + dy * dy) + gate->state.health / 5;
         if (score < best_score) {
           best_score = score;
           best = id;
         }
       }
       return HostOutcome::ok_with(best == kNoObject ? invalid_handle() : object_handle(best));
     }},
    {CallKind::member, "OpenAllGates", 0,  // 1 site
     // 0x00428710: every gate that is not already open has its command
     // replaced by `opengate`. "Already open" is 0x00529070's pair of tests --
     // the target state is open *and* the animation stands at its last frame
     // -- and this engine carries the state alone, so a gate mid-swing counts
     // as open here and is not re-ordered.
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return HostOutcome::ok_void();
       World& world = *world_of(ctx);
       CommandSystem* commands = command_system(world);
       if (commands == nullptr) return HostOutcome::ok_void();
       for (const ObjectId id : settlement_gates(world, *s)) {
         const WorldObject* gate = world.find(id);
         if (gate == nullptr || gate->state.flags.gate_open) continue;
         // `vtbl+0xc0(1)` ends what the gate is doing, pending and running
         // both, before the order goes in.
         (void)commands->clear_commands(world, id);
         (void)commands->kill_command(world, id);
         // `set`, not `add`: the kill refills the queue with the class's
         // default verb when a scheduler is attached, and the order has to
         // replace that `idle`, not queue behind it. Without a scheduler the
         // two are the same call, which is why a sweep could not tell them
         // apart here; `kill_command`'s own tests carry the refill.
         (void)commands->set_command(world, id, "opengate", Command{});
       }
       return HostOutcome::ok_void();
     }},
    {CallKind::member, "IdleAllGates", 0,  // 2 sites
     // 0x00428640: every gate whose running command is not named `idle` has
     // its commands cleared, which is what returns a gate to `GATE_IDLE.VS`.
     +[](CallContext& ctx) -> HostOutcome {
       Settlement* s = receiver(ctx);
       if (s == nullptr) return HostOutcome::ok_void();
       World& world = *world_of(ctx);
       CommandSystem* commands = command_system(world);
       if (commands == nullptr) return HostOutcome::ok_void();
       for (const ObjectId id : settlement_gates(world, *s)) {
         if (commands->command_count(id) != 0 && commands->command_name(id, 0) == "idle") continue;
         // The same `vtbl+0xc0(1)`: pending dropped, the runner ended, and the
         // queue refilling with the class's default verb -- which is `idle`.
         (void)commands->clear_commands(world, id);
         (void)commands->kill_command(world, id);
       }
       return HostOutcome::ok_void();
     }},
};

}  // namespace

bool economy_constant(const EconomyRules& rules, std::string_view key,
                      std::int32_t& out) noexcept {
  for (const NamedConstant& c : kEconomyConstants) {
    if (c.name == key) {
      out = rules.*(c.field);
      return true;
    }
  }
  return false;
}

EconomySystem* economy_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "economy") {
      return static_cast<EconomySystem*>(system);
    }
  }
  return nullptr;
}

std::size_t register_economy_hosts(script::HostRegistry& registry) {
  for (const EconomyHostDef& def : kEconomyHosts) {
    registry.define(def.kind, def.name, def.arity, def.fn);
  }
  return economy_host_entry_count();
}

std::size_t economy_host_entry_count() noexcept {
  return sizeof(kEconomyHosts) / sizeof(kEconomyHosts[0]);
}

}  // namespace imperivm::core::sim
