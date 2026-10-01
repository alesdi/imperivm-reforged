#pragma once

/// The `Feeder`: the food sink the economy was missing.
///
/// Until this existed nothing in the simulation ever *ate*. A stronghold's food
/// could only climb, because its warehouse had inflow (wagons, `TAVERN_BUYFOOD`,
/// research) and no outflow at all, and the reference dumps do not look like
/// that. `gbr.exe` carries a serialised subsystem called `Feeder` whose save
/// fields are, in the executable's own string table and in this order:
///
///     unit_chain  total_unit_count  next_tick  ch_quant  cf_quant  curr_quant
///
/// and the constants adjacent to it in that same table are exactly
///
///     FastMetabolismSpeed  FoodSearchFreqVar  FoodSearchFreq
///     HealthDecreaseStep   HealthDropBoundPercent
///     HealthDropIvl        DropFoodByOneIvl    FeedinQuantIvl
///
/// That cluster is the specification. Note which constants are **not** in it:
/// `HealthIncreaseStep`, `StaminaIncreaseStep`, `HealthInHolderFactor` and
/// `StaminaInHolderFactor` sit in a separate cluster beside the `CVXUnit`
/// member errors, so regeneration is the *unit's* business and not the feeder's.
/// This system therefore drains and starves; it does not heal.
///
/// ## What one interval does
///
/// The feeder is stepped every `FeedinQuantIvl = 100` ms and walks one chain of
/// every feeding unit in the world with two independent cursors:
///
///   * the **food** cursor completes one full pass every
///     `DropFoodByOneIvl = 10000` ms, and each unit it visits loses one food;
///   * the **health** cursor completes one full pass every
///     `HealthDropIvl = 5000` ms, and each unit it visits that has no food
///     loses `HealthDecreaseStep = 5` health, down to a floor of
///     `HealthDropBoundPercent = 10` percent of its maximum. Starvation does not
///     kill.
///
/// Spreading the pass over the interval rather than doing it in one burst is
/// what `ch_quant`/`cf_quant` are for, and it is why the interval is 100 ms and
/// not 10,000: `n` units and a 100 ms step means `n / 100` units are touched per
/// step, which is a fixed cost per frame instead of a stall every ten seconds.
/// The residues are carried as integers (`accumulator += n * FeedinQuantIvl`,
/// then divide and take the remainder), so the pass rate is exact and no
/// floating point is involved.
///
/// A hungry unit then looks for food every `FoodSearchFreq = 1000` ±
/// `FoodSearchFreqVar = 1000` ms. That is the one place in this system that
/// draws randomness, and it draws from `World::rng()` -- rule 2 of
/// `docs/engine/architecture.md`. See `schedule_search` for what the ± is read
/// as and why.
///
/// ## Where the food comes from
///
/// Two paths, and only one of them is engine-internal.
///
///   * **The script path.** `DATA\CLASSES\BASEVILLAGE.SC.XML` binds
///     `<behavior script="data/subai/village_behavior_givefood.vs"/>`, and that
///     script tops every friendly unit in sight up to 20 out of the village's
///     own store, every two seconds. `TTENT_BEHAVIOR.VS` and
///     `OUTPOST_BEHAVIOR.VS` do the same for their garrisons.
///     `outpost_feeding_behavior.vs` is bound but commented out. This system
///     does not implement any of that; it is script, it runs on the VM, and it
///     reaches the units through `SetFood`.
///   * **The engine path**, which is what `FoodSearchFreq` schedules: a hungry
///     unit *inside a settlement's holder* draws from that settlement's
///     warehouse. That is the reading `Unit::GetHolderSett` supports and it is
///     what makes a town hall's store a sink: `BASETOWNHALL.SC.XML` ships
///     `produces_food="0"` and `max_units="10000"`, so a stronghold garrison
///     eats a store that has no production behind it, only wagons. **A unit
///     standing in the open is deliberately not fed by this system**: the
///     original may search a radius, but no shipped file names one, and
///     inventing a radius would be inventing a balance rule.
///
/// ## Rates, checked
///
/// `DATA\CLASSES\UNIT.SC.XML` ships `max_food="20"` and `feeds="1"`, so the
/// baseline is one food per unit per ten seconds regardless of capacity --
/// `CWARELEPHANT.SC.XML` raises `max_food` to 100 and `FISH.SC.XML` lowers it to
/// 10, but neither changes the drain rate, only how long a top-up lasts.
/// `feeds="0"` is shipped by `SENTRY`, `RAMUNIT`, `WAGON`, `BASEANIMAL`,
/// `EAGLE`, `CROW`, `HEN2`, `IMOUNTAINEER`, `SHAMANGHOST` and `GGHOST`, and a
/// unit that does not feed is in the chain but is skipped by both cursors --
/// which is exactly what `Unit::SetFeeding(false)` does at runtime, called from
/// five shipped behaviours.
///
/// A village is `population=12`, `food_rate=24`: `12 * 24 / 100 = 2` per
/// `ProductionInterval = 2000`, so 10 food per ten seconds, so ten units. The
/// AI agrees with the arithmetic from the other side --
/// `DATA\AI\ES_VILLAGE.VS` computes its food target as
/// `MilUnits(idPlayer) * 20`, which is `max_food` per military unit.
///
/// ## Determinism
///
/// The chain is a vector in ascending `ObjectId`, never a set and never
/// unordered. The two cursors are stored as the `ObjectId` to resume *at*
/// rather than as an index, so removing a dead unit from the middle of the
/// chain does not silently make the cursor skip or repeat its neighbours. Every
/// field below is hashed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/sim/tick.hpp"

namespace imperivm::core::sim {

class World;

/// The feeding constants, all of them from the `;Feeding Constants` and
/// `;Unit Feeding` blocks of `DATA\CONST.INI`, and every one of them present in
/// `gbr.exe`'s string table -- checked, because `FullArmor = 8` is in
/// `CONST.INI` and appears in the executable zero times, and a constant the
/// shipped build never reads is not a rule.
struct FeederRules {
  /// `FeedinQuantIvl = 100`. How often the feeder is stepped.
  std::int32_t feed_quant_interval = 100;
  /// `DropFoodByOneIvl = 10000`. One full pass of the food cursor.
  std::int32_t drop_food_by_one_interval = 10000;
  /// `HealthDropIvl = 5000`. One full pass of the health cursor.
  std::int32_t health_drop_interval = 5000;
  /// `HealthDecreaseStep = 5 ; when the unit starves`.
  std::int32_t health_decrease_step = 5;
  /// `HealthDropBoundPercent = 10`: the floor starvation stops at, as a percent
  /// of the unit's maximum health. INFERRED from the name and from the fact
  /// that the constant sits in the feeder's own cluster next to
  /// `HealthDecreaseStep`; no shipped script states it. It is what makes
  /// starvation a debuff rather than an attrition kill.
  std::int32_t health_drop_bound_percent = 10;
  /// `FoodSearchFreq = 1000`, `FoodSearchFreqVar = 1000`.
  std::int32_t food_search_freq = 1000;
  std::int32_t food_search_freq_var = 1000;
  /// `FastMetabolismSpeed = 5`, filed under `;unit specialities` in
  /// `CONST.INI` and present in the feeder's constant cluster. **Carried and
  /// not applied**: nothing in `data.pak` names the special that would set it,
  /// `UNIT_SPECIALS.INI` does not mention metabolism, and whether it multiplies
  /// the drain or divides the interval is a guess either way. Here so that the
  /// number has a home when the special is found.
  std::int32_t fast_metabolism_speed = 5;

  /// `DATA\CLASSES\UNIT.SC.XML`: `max_food="20"`, `feeds="1"`. The fallback for
  /// a unit whose class the graph cannot answer for.
  std::int32_t default_max_food = 20;
  bool default_feeds = true;
};

/// One link of `unit_chain`.
///
/// `food` is the dump's per-unit `food` field: `docs/engine/state-vector.md`
/// records it in the `CVXUnit` block on the `food | squad | experience | level`
/// line with a domain of 0..20, which is `max_food` exactly.
struct FeedingUnit {
  ObjectId unit = kNoObject;
  /// 0 .. `max_food`.
  std::int32_t food = 0;
  /// The class property `max_food`.
  std::int32_t max_food = 0;
  /// The class property `maxhealth`, for the starvation floor. Zero disables
  /// the health cursor for this unit, because a floor of "10 percent of
  /// nothing" is not a floor.
  std::int32_t max_health = 0;
  /// The class property `feeds`, and what `Unit::SetFeeding` writes. A unit
  /// that does not feed stays in the chain -- it can be switched back on -- and
  /// is skipped by both cursors.
  bool feeds = true;
  /// Absolute game time at which this unit next looks for food. Only meaningful
  /// while the unit is hungry.
  GameTime search_due = 0;
  /// Game time at which the feeder's tick first found this unit at zero food,
  /// or 0 while it has food. The original posts an `army starving`
  /// notification from the same tick (0x005dbedf, for the local player's
  /// units) and `HasStarvingArmy(ms)` / `StarvingArmyPos(ms)` read that
  /// registry's age; here the age is read off this stamp. Hashed and saved
  /// with the rest of the record.
  GameTime hungry_since = 0;
};

/// The unit feeding subsystem.
class FeederSystem final : public System {
 public:
  FeederSystem() = default;
  explicit FeederSystem(const FeederRules& rules) noexcept : rules_(rules) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "feeder"; }

  /// Enrol every feeding unit already in the world and give each a full store.
  /// Idempotent; a unit already in the chain keeps the food it has.
  void start(World& world) override;

  void advance(World& world, const Turn& turn) override;

  void hash(std::uint64_t& accumulator) const override;

  // -- configuration -----------------------------------------------------

  [[nodiscard]] const FeederRules& rules() const noexcept { return rules_; }
  void set_rules(const FeederRules& rules) noexcept { rules_ = rules; }

  /// Reconcile the chain against `World::objects()` at the head of every turn:
  /// enrol units that have appeared, drop the ones that are gone or dead.
  ///
  /// **On by default**, unlike `CombatSystem::set_world_bound`, and the
  /// difference is deliberate. Combat is keyed by an id it may have minted
  /// itself, so binding it to the world can overwrite a synthetic combatant
  /// with an unrelated object. The feeder never mints an id: it only ever reads
  /// `state.flags.is_unit` off objects the world already owns, and a feeder
  /// that has to be switched on is a feeder that silently does nothing in the
  /// assembled engine -- which is precisely the bug this system exists to fix.
  /// A test that wants a closed chain of ids of its own turns it off.
  void set_world_bound(bool bound) noexcept { world_bound_ = bound; }
  [[nodiscard]] bool world_bound() const noexcept { return world_bound_; }

  /// Let a hungry unit inside a settlement's holder draw from that settlement's
  /// warehouse. On by default; off makes the feeder a pure sink, which is what
  /// a test measuring the drain alone wants.
  void set_draws_from_warehouse(bool enabled) noexcept { draws_from_warehouse_ = enabled; }
  [[nodiscard]] bool draws_from_warehouse() const noexcept { return draws_from_warehouse_; }

  // -- the chain ---------------------------------------------------------

  /// Enrol a unit, reading `max_food`, `feeds` and `maxhealth` from its class.
  /// Returns the link, whether it was created now or already present.
  FeedingUnit& enrol(World& world, ObjectId unit);
  /// Enrol `unit` if the world says it is one -- the same test `sync_with_world`
  /// applies, in one place so the two cannot drift.
  ///
  /// **For the moment an object comes into being.** The reconcile runs at the
  /// head of a turn, and a script that places a unit and feeds it two
  /// statements later never reaches a turn boundary in between:
  /// `OUTPOST_BEHAVIOR.VS` is `u1 = Place(...); u1.SetFood(20);
  /// u1.SetFeeding(false);`, and without this the two setters are handed an id
  /// the chain has never heard of. The original has no chain to be late for --
  /// a `CVXUnit` *is* a feeding unit from construction -- so this is where the
  /// reconcile model has to pay for itself. Returns whether it enrolled.
  bool enrol_if_eligible(World& world, ObjectId unit);
  /// Enrol with explicit numbers, for a test with no class graph.
  FeedingUnit& enrol(const FeedingUnit& link);
  bool remove(ObjectId unit);

  [[nodiscard]] FeedingUnit* find(ObjectId unit) noexcept;
  [[nodiscard]] const FeedingUnit* find(ObjectId unit) const noexcept;
  [[nodiscard]] std::span<const FeedingUnit> chain() const noexcept { return chain_; }
  /// `total_unit_count`.
  [[nodiscard]] std::size_t total_unit_count() const noexcept { return chain_.size(); }

  // -- per-unit food -----------------------------------------------------

  /// `Unit::food`. Zero for a unit that is not in the chain; `has_food` is how
  /// a caller tells that apart from a unit that is genuinely empty.
  [[nodiscard]] std::int32_t food(ObjectId unit) const noexcept;
  [[nodiscard]] bool has_food_record(ObjectId unit) const noexcept;
  /// `Unit::maxfood`.
  [[nodiscard]] std::int32_t max_food(ObjectId unit) const noexcept;
  /// `Unit::SetFood`, clamped to `[0, max_food]`. Returns false for a unit that
  /// is not in the chain.
  bool set_food(ObjectId unit, std::int32_t value);
  /// `Unit::SetFeeding`.
  bool set_feeding(ObjectId unit, bool feeding);
  [[nodiscard]] bool feeding(ObjectId unit) const noexcept;

  /// The units currently at zero food, in ascending id order. State: their
  /// order is the order they are offered food in.
  [[nodiscard]] std::span<const ObjectId> hungry() const noexcept { return hungry_; }

  // -- the saved game ----------------------------------------------------
  //
  // Layout and rationale: docs/formats/save.md. The definitions live together
  // in `src/sim/save_systems.cpp` rather than in this domain's own `.cpp`, so
  // that the ten systems' state vectors are one file to audit: adding a field
  // here and forgetting its section shows up as a diff that does not touch the
  // one place every section is written.

  /// Append this system's state to `out`, as one self-describing section with
  /// its own magic and version -- the shape `Scheduler::serialize` established.
  ///
  /// **Written:** the feeding chain, the hungry list, `next_tick_`, the two
  /// accumulators and the two cursors. Those six line up name for name with
  /// what `gbr.exe`'s own persist stream registers under `Feeder`
  /// (`unit_chain`, `total_unit_count`, `next_tick`, `ch_quant`, `cf_quant`,
  /// `curr_quant`), which is a cross-check on a system built from other
  /// evidence entirely.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** `FeederRules`, `world_bound_` and
  /// `draws_from_warehouse_`, which are configuration.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  /// One `FeedinQuantIvl`. `now` is the game time **of this step**, not of the
  /// end of the turn containing it; see `advance`.
  void step(World& world, GameTime now);
  /// Advance `cursor` over `count` links, applying `apply` to each, wrapping at
  /// the end of the chain. Returns the id to resume at.
  template <typename Apply>
  void walk(ObjectId& cursor, std::int64_t count, Apply apply);

  void drop_food(FeedingUnit& link, GameTime now);
  void drop_health(World& world, FeedingUnit& link);
  void run_searches(World& world, GameTime now);
  /// `FoodSearchFreq` ± `FoodSearchFreqVar`, drawn from the world's generator.
  void schedule_search(World& world, FeedingUnit& link, GameTime now);
  void mark_hungry(ObjectId unit);
  void clear_hungry(ObjectId unit);
  void sync_with_world(World& world);

  FeederRules rules_{};

  /// `unit_chain`, ascending by `ObjectId`. Iteration order is world state.
  std::vector<FeedingUnit> chain_;
  /// The links at zero food, ascending. Derived from `chain_`, kept beside it
  /// so that the search pass is proportional to the number of hungry units
  /// rather than to the whole world.
  std::vector<ObjectId> hungry_;

  /// `next_tick`: game time left before the next `FeedinQuantIvl` step.
  std::int32_t next_tick_ = 0;
  /// `cf_quant` / `ch_quant`: the unit-milliseconds carried between steps, so
  /// that a pass takes exactly its interval however the turn is chopped up.
  std::int64_t food_quant_ = 0;
  std::int64_t health_quant_ = 0;
  /// The two halves of `curr_quant`: the id each cursor resumes at. Stored as
  /// an id and not an index so that a death in the middle of the chain does not
  /// shift the cursor onto a unit it has already visited.
  ObjectId food_cursor_ = 0;
  ObjectId health_cursor_ = 0;

  bool world_bound_ = true;
  bool draws_from_warehouse_ = true;
};

// --------------------------------------------------------------------------
// host functions
// --------------------------------------------------------------------------

/// The feeder registered on `world`, or null. Same shape and same reason as
/// `economy_of`: systems are reached through the run-order list, so there is
/// only ever one `CallContext::user` convention for two domains to disagree
/// about.
[[nodiscard]] FeederSystem* feeder_of(World& world) noexcept;

/// The feeder's slice of `DATA\CONST.INI`, by key. `sim/env.cpp` asks
/// `economy_constant` first so that `set_rules` stays authoritative for the
/// economy's keys; this is the same hook for the feeder's, and env needs one
/// line to call it. Returns false when the key is not one of the feeder's.
[[nodiscard]] bool feeder_constant(const FeederRules& rules, std::string_view key,
                                   std::int32_t& out) noexcept;

/// Define the feeder slice of the host API: `maxfood/0` and `SetFeeding/1`.
///
/// `food/0` and `SetFood/1` are **not** here. Both are already defined by
/// `register_economy_hosts` for a settlement receiver, and a second `define()`
/// on the same (kind, name, arity) replaces the first silently -- so they are
/// widened in `sim/economy.cpp` to fall through to the unit chain instead of
/// being redefined here. Both names are declared by `declare_shipped_surface`
/// with the arities the corpus uses; this adds behaviour and no names.
std::size_t register_feeder_hosts(script::HostRegistry& registry);

/// The number of entry points `register_feeder_hosts` defines.
[[nodiscard]] std::size_t feeder_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
