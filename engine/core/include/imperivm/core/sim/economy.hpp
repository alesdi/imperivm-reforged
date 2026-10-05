#pragma once

/// The economy: settlements that produce, store and trade.
///
/// A `System` in the sense of `sim/system.hpp` — the turn loop drives it, it
/// owns its own state, and it reaches objects through the world rather than
/// holding pointers across turns.
///
/// ## Where the numbers come from
///
/// Every constant in `EconomyRules` is a line of `DATA\CONST.INI`, quoted in
/// the comment beside it. Every *rule* is either a shipped `.vs` behaviour, a
/// class property, or marked INFERRED. The distinction matters: `CONST.INI`
/// names `PopulationDecreasePercent` but nothing in the shipped scripts says
/// what triggers it, so the trigger is a reading and is labelled as one.
///
/// ## Time is accumulated, never counted in turns
///
/// The turn length is renegotiated mid-session and takes at least four values
/// in the retail dumps (200, 400, 799, 800), so an interval cannot be a turn
/// count. It also cannot be a simple "add the turn, drain the accumulator"
/// loop, because a settlement has ten intervals and a long turn would run all
/// of one before any of another while short turns interleave them — the two
/// would diverge, and the whole point of the conformance harness is that they
/// must not.
///
/// So `advance` is **event-stepped**: it walks to the nearest timer that comes
/// due, applies exactly the timers due at that instant in `SettlementTimer`
/// order, and repeats until the turn is spent. The sequence of events over an
/// interval is then a function of the interval alone, and any partition of a
/// turn into shorter turns produces bit-identical state. `test_economy.cpp`
/// asserts that over [400, 800, 200] against one turn of 1,400.
///
/// ## Integers only
///
/// A percentage is `value * percent / 100`, evaluated in that order, truncating
/// toward zero. Nothing here draws randomness: none of the rules the shipped
/// data supports needs any, and a system that does not draw cannot desynchronise
/// the world's generator by drawing in a different order.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

/// The balance constants, all of them from `DATA\CONST.INI` in `data.pak`.
///
/// Held as data rather than hard-coded because `CONST.INI` is a shipped file
/// that a mod may replace, and because a test that wants a 10-unit interval
/// should not have to simulate twenty seconds to see one tick.
struct EconomyRules {
  // -- population --------------------------------------------------------
  /// `PopulationGrowthRate = 1`, `PopulationGrowthInterval = 20000`.
  std::int32_t population_growth_rate = 1;
  std::int32_t population_growth_interval = 20000;
  /// `PopulationDecreasePercent = 10`, `PopulationDecreaseInterval = 4000`.
  std::int32_t population_decrease_percent = 10;
  std::int32_t population_decrease_interval = 4000;
  /// `MinPopulation = 10`. Two distinct jobs in the shipped data: the floor a
  /// command's population cost may not breach (`VERIFY_CMDCOST_BUILDING.VS`
  /// and four siblings test `cmdcost_pop + MinPopulation > population`), and
  /// the level `VILLAGE_REPOPULATE.VS` restores a dead village to.
  std::int32_t min_population = 10;
  /// `PopGroup = 5` — "peasants come in groups of this many".
  /// `VILLAGE_TRAINPEASANT.VS` places exactly this many villagers per command.
  std::int32_t pop_group = 5;

  // -- production --------------------------------------------------------
  /// `ProductionGoldBase = 24 ;percent from population`.
  std::int32_t production_gold_base = 24;
  /// `ProductionFoodBase = 24 ;percent from population`.
  std::int32_t production_food_base = 24;
  /// `ProductionInterval = 2000`. **Note** the shipped value is 2,000 game-time
  /// units (two seconds), not 20; anything faster puts a town hall's gold two
  /// orders of magnitude above what the dumps show.
  std::int32_t production_interval = 2000;
  /// `StrongholdFoodProduction = 10 ; Food Tax upgrade`. Not applied here —
  /// `ONFINISH_RESEARCH.VS` applies it — but this is the value it sets
  /// `food_rate` to.
  std::int32_t stronghold_food_production = 10;

  // -- warehouse and transport ------------------------------------------
  /// `WagonBuildTime = 7000`: how long a dispatched wagon takes to exist.
  std::int32_t wagon_build_time = 7000;
  /// `MinResQtyToTransport = 100`. Both uses in the corpus are strict:
  /// `if (nSendGold > min_resource)` in `VILLAGE_BEHAVIOR_SUPPLY.VS`, and
  /// `if (.food < nMinRes) return false` in `CREATE_FOOD_MULE_VERIFY.VS`.
  std::int32_t min_res_qty_to_transport = 100;
  /// `MIN(1000, nSendGold)` in `VILLAGE_BEHAVIOR_SUPPLY.VS`, and the big-mule
  /// commands load exactly 1,000 (`WAGON_LOADFOODBIG.VS`). The small ones load
  /// 250 (`WAGON_LOADGOLDSMALL.VS`).
  std::int32_t wagon_capacity = 1000;
  std::int32_t wagon_capacity_small = 250;

  // -- burning -----------------------------------------------------------
  /// `BurnTime = 500`, `GoldBurnAmount = 20`, `FoodBurnAmount = 20`.
  std::int32_t burn_time = 500;
  std::int32_t gold_burn_amount = 20;
  std::int32_t food_burn_amount = 20;

  // -- repair ------------------------------------------------------------
  /// `PopulationRepairBase = 2`, `RepairInterval = 2000`.
  std::int32_t population_repair_base = 2;
  std::int32_t repair_interval = 2000;

  // -- sentries ----------------------------------------------------------
  /// `InitialStrongholdSentries = 20`, `SentriesAddTime = 30000`,
  /// `SentriesAddCount = 4`. Held only so `GetConst` answers them: the whole
  /// mechanism is `TOWNHALL_SENTRIES_CONTROL.VS`, a town hall's behaviour,
  /// which reads the three and does the rest itself.
  std::int32_t initial_stronghold_sentries = 20;
  std::int32_t sentries_add_time = 30000;
  std::int32_t sentries_add_count = 4;

  // -- loyalty and capture ----------------------------------------------
  /// `LoyaltyCapInterval = 2000`, `LoyaltyChangeCap = 10`,
  /// `LoyaltyIncreasePerUnit = 10`, `LoyaltySettlementsInitial = 10`.
  /// The cap's own comment says what it caps: "No more than 'LoyaltyDecreaseCap'
  /// loyalty percents **decreased** in 'LoyaltyCapInterval'".
  std::int32_t loyalty_cap_interval = 2000;
  std::int32_t loyalty_change_cap = 10;
  std::int32_t loyalty_increase_per_unit = 10;
  std::int32_t loyalty_settlements_initial = 10;
  /// Not a constant: `UNIT_CAPTURE.VS` writes `SetLoyalty(11)` literally, the
  /// instant a settlement changes hands.
  std::int32_t loyalty_after_capture = 11;
  /// `LoyaltyUnitsOutTreshold = 151`.
  std::int32_t loyalty_units_out_threshold = 151;

  // -- outposts ----------------------------------------------------------
  /// `OutpostRefresh = 2000`, `GOutpostFoodSales = 20`,
  /// `GOutpostGoldReturn = 10`, `ROutpostRequiredGold = 2000`,
  /// `ROutpostGoldReturn = 8`.
  std::int32_t outpost_refresh = 2000;
  std::int32_t outpost_food_sales = 20;
  std::int32_t outpost_food_gold_return = 10;
  std::int32_t outpost_required_gold = 2000;
  std::int32_t outpost_gold_return = 8;

  // -- tavern ------------------------------------------------------------
  /// `LoanAmount = 4000`, `LoanInterestPercent = 10`,
  /// `TimeProductionForLoan = 180000 ;msec`.
  std::int32_t loan_amount = 4000;
  std::int32_t loan_interest_percent = 10;
  std::int32_t loan_interest_interval = 180000;
  /// `InvestmentGoldRevenue = 6000` against the 4,000 `TAVERN_INVESTMENT.VS`
  /// hands back to the gold-spent statistic.
  std::int32_t investment_cost = 4000;
  std::int32_t investment_revenue = 6000;
  /// `SlaveryGold = 2000` — the cost of `EMARKET_BUYSLAVES.VS`, which sets the
  /// settlement's population to its maximum.
  std::int32_t slavery_gold = 2000;

  // -- items -------------------------------------------------------------
  /// `SpoilsOfWarGold = 100` per "Spoils of War" item a unit carries into a
  /// settlement (`UNIT_ON_ENTER.VS`), `MercenaryPactGold = 40`.
  std::int32_t spoils_of_war_gold = 100;
  std::int32_t mercenary_pact_gold = 40;
};

/// A wagon carrying resources between two settlements.
///
/// The economy owns the cargo and the two endpoints; it does not own movement.
/// `WagonBuildTime` elapses first — the wagon does not exist before that — and
/// then the wagon is `ready`. Who moves it and how long it takes is the
/// movement system's business: it calls `deliver_wagon` when the wagon reaches
/// its destination, or `lose_wagon` when it is captured or killed on the way.
/// With no movement system attached, `auto_deliver` makes a ready wagon unload
/// immediately so that the economy is closed and testable on its own.
struct Wagon {
  /// Dense, never reused, assigned from 1. Zero is not a wagon.
  std::uint32_t id = 0;
  SettlementId source = kNoSettlement;
  SettlementId destination = kNoSettlement;
  Resource resource = Resource::gold;
  std::int32_t amount = 0;
  /// Game time left before the wagon exists. Counts down to zero and stops.
  std::int32_t build_remaining = 0;
  /// The world object once one has been spawned for it, or null.
  ObjectId object = kNoObject;
  /// `Squad::SendFoodWagon`: the unit this wagon was sent after, and
  /// `kNoObject` for a wagon bound for a settlement. **A wagon with one has no
  /// destination and is never delivered**: when its build time runs out it
  /// stays in the table as the larder of whatever squad that unit is then in,
  /// and the feeder's food search draws it down until it is empty. That is
  /// the original's mule following the army with the journey abstracted away,
  /// which is what this table already does to every other wagon.
  ObjectId follow = kNoObject;

  [[nodiscard]] bool ready() const noexcept { return build_remaining <= 0; }
  [[nodiscard]] bool follows() const noexcept { return follow != kNoObject; }
};

/// Why a spend was refused. `ok` is the only success.
enum class SpendResult : std::uint8_t {
  ok = 0,
  no_settlement,
  not_enough_gold,
  not_enough_food,
  /// `cmdcost_pop > 0 && cmdcost_pop + MinPopulation > population`, the exact
  /// test the five `*_VERIFY.VS` scripts share.
  not_enough_population,
};

/// The settlement economy, advanced once per turn.
class EconomySystem final : public System {
 public:
  EconomySystem() = default;
  explicit EconomySystem(const EconomyRules& rules) : rules_(rules) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "economy"; }

  /// Seeds every settlement's timers and loyalty. Idempotent, and safe to call
  /// again after more settlements are created.
  void start(World& world) override;

  void advance(World& world, const Turn& turn) override;

  void hash(std::uint64_t& accumulator) const override;

  // -- configuration -----------------------------------------------------

  [[nodiscard]] const EconomyRules& rules() const noexcept { return rules_; }
  void set_rules(const EconomyRules& rules) noexcept { rules_ = rules; }
  /// Deliver a wagon's cargo the moment it is built, rather than waiting for a
  /// movement system to carry it. Defaults to true so that the economy runs
  /// closed; a build with movement turns it off.
  void set_auto_deliver(bool enabled) noexcept { auto_deliver_ = enabled; }
  [[nodiscard]] bool auto_deliver() const noexcept { return auto_deliver_; }

  // -- settlements -------------------------------------------------------

  [[nodiscard]] SettlementStore& settlements() noexcept { return store_; }
  [[nodiscard]] const SettlementStore& settlements() const noexcept { return store_; }

  /// Create a settlement and derive its production rates from `init`.
  SettlementId create(const SettlementInit& init);

  /// The same, minting the Settlement/Holder/Warehouse triple in the world when
  /// `init` does not already name one, and writing `WorldObject::settlement` on
  /// every object the settlement owns.
  ///
  /// The triple's three consecutive ids are `World`'s to allocate — allocation
  /// order is observable state and the dumps pin it — so this defers to
  /// `World::spawn_settlement` rather than spawning anything itself.
  SettlementId create(World& world, SettlementInit init);

  /// Rebuild the settlement at `id` from `init` -- same `ID`, same three
  /// handles, everything else as `create` would make it: production rates,
  /// sentry cap, loyalty at its initial value, the timers at their periods,
  /// the statistics row cleared. `init`'s three handles are overwritten with
  /// the settlement's own. False for an unknown `id`.
  ///
  /// For `materialise_mutable_settlements`, which does to a placeholder
  /// settlement what 0x00551bc0 does: destroys its record and creates a race's
  /// in its place. See `SettlementStore::replace` for why the `ID` is kept.
  bool recreate(World& world, SettlementId id, SettlementInit init);

  /// Bind a building to a settlement and to the world's back-link at once.
  /// `max_health` is the class's `maxhealth`: repair needs a ceiling and the
  /// economy has no class graph.
  bool add_building(World& world, SettlementId id, ObjectId building,
                    std::int32_t max_health);

  [[nodiscard]] Settlement* find(SettlementId id) noexcept { return store_.find(id); }
  [[nodiscard]] const Settlement* find(SettlementId id) const noexcept { return store_.find(id); }

  // -- resources ---------------------------------------------------------

  [[nodiscard]] std::int32_t resource(SettlementId id, Resource r) const noexcept;
  /// `SetGold` / `SetFood`: clamped to `[0, cap]`.
  bool set_resource(SettlementId id, Resource r, std::int32_t value);
  /// Add, saturating at the cap. Returns what was accepted.
  std::int32_t deposit(SettlementId id, Resource r, std::int32_t amount);
  /// Remove, never below zero. Returns what was taken.
  std::int32_t withdraw(SettlementId id, Resource r, std::int32_t amount);

  /// The command-cost test the five `*_VERIFY.VS` scripts share, in their
  /// order: gold, then food, then the population floor.
  [[nodiscard]] SpendResult can_afford(SettlementId id, std::int32_t gold, std::int32_t food,
                                       std::int32_t population) const;
  /// `can_afford` and then charge. Nothing is charged when it refuses.
  SpendResult spend(SettlementId id, std::int32_t gold, std::int32_t food,
                    std::int32_t population);

  // -- population --------------------------------------------------------

  /// `SetPopulation`, which **is** capped at `max_population`.
  ///
  /// `gbr.exe` 0x005c2200 stores `min(value, max_population)` -- a signed
  /// compare against the settlement's `+0x4c` before the write to `+0x50`.
  /// This said "not capped" for a while, on the strength of the late dumps
  /// showing `101 100` and `165 150`; those are real, and they are not
  /// evidence about this entry point. `TOWNHALL_ADDPOP.VS` and
  /// `PEASANT_ENTER.VS` reach the population through `AddToPopulation`, which
  /// is genuinely uncapped in the original (0x005c2250 is one `add`), and that
  /// is where a settlement gets past its ceiling. The one script the
  /// difference is visible in is `ONFINISH_RESEARCH.VS`: "Free Beer" is
  /// `SetPopulation(population + 20)` and stops at the ceiling, while "Housing"
  /// raises the ceiling with `AddToMaxPopulation`.
  ///
  /// The lower clamp at zero is this engine's, not the original's -- 0x005c2200
  /// stores a negative straight through. No shipped call site can reach one:
  /// the five sites pass `max_population`, `population + 20`, `population + 10`,
  /// the literal 100 and `GetConst("MinPopulation")`.
  bool set_population(SettlementId id, std::int32_t value);
  /// `AddToPopulation`, which is not capped in either direction -- see above.
  bool add_population(SettlementId id, std::int32_t delta);
  /// `AddToMaxPopulation`: raise (or lower) the ceiling itself.
  ///
  /// `gbr.exe` 0x005c2290 is one `add` to `settlement + 0x4c` and no clamp of
  /// any kind. The four shipped sites all raise it -- `ONFINISH_RESEARCH.VS`'s
  /// "Housing" by 20, and three campaign sequences -- and the zero floor here
  /// is the same belt-and-braces guard `add_population` carries.
  bool add_max_population(SettlementId id, std::int32_t delta);

  // -- transport ---------------------------------------------------------

  /// Name the settlement this one ships its surplus to. `StartSupplyFood` /
  /// `StopSupply` with `kNoSettlement`.
  bool set_supplied(SettlementId id, SettlementId target);

  /// `CreateMuleGold` / `CreateMuleFood`: take `amount` out of the warehouse
  /// now and put a wagon on the build timer. Returns 0 when the settlement
  /// cannot pay or the amount is not worth transporting.
  std::uint32_t create_mule(SettlementId source, SettlementId destination, Resource r,
                            std::int32_t amount);
  /// `Squad::SendFoodWagon`: take `amount` food out of the warehouse now and
  /// put a wagon on the build timer that will follow `follow` rather than go
  /// anywhere. Returns 0 when the settlement cannot pay. See `Wagon::follow`.
  std::uint32_t create_feeding_mule(SettlementId source, ObjectId follow, std::int32_t amount);
  /// The feeder's second path: the first built food wagon following any of
  /// `members`, drawn down by up to `want`. Returns what was taken; a wagon
  /// emptied by the draw is erased. Ascending by wagon id, which is state.
  std::int32_t draw_from_larder(std::span<const ObjectId> members, std::int32_t want);

  [[nodiscard]] std::span<const Wagon> wagons() const noexcept { return wagons_; }
  [[nodiscard]] const Wagon* find_wagon(std::uint32_t wagon) const noexcept;

  /// `WAGON_UNLOAD.VS`: the cargo goes into the destination's warehouse and the
  /// wagon is erased. Returns what the warehouse accepted.
  std::int32_t deliver_wagon(std::uint32_t wagon);
  /// Destroyed or captured in transit: the cargo is gone.
  bool lose_wagon(std::uint32_t wagon);

  // -- capture and loyalty ----------------------------------------------

  /// `IsValidCaptureTarget`: capturable at all, and — for the classes that
  /// declare a threshold below 100, which in the retail data is `Outpost` at
  /// 50 — damaged to at or below `capture_health_percent` of the anchor's
  /// maximum health. INFERRED: the property's name and its two shipped values
  /// support this reading, and no script states it.
  [[nodiscard]] bool is_valid_capture_target(const World& world, SettlementId id) const;

  /// `DecreaseLoyalty(n)`. The capturing unit's script calls this once per
  /// two-second taunt; loyalty stops at zero, and no more than
  /// `LoyaltyChangeCap` is lost inside one `LoyaltyCapInterval` window.
  /// Returns what was actually taken off.
  std::int32_t decrease_loyalty(SettlementId id, std::int32_t amount);
  bool set_loyalty(SettlementId id, std::int32_t value);

  /// Garrison a unit: `AddUnit` (refused when the holder is full) and
  /// `ForceAddUnit` (admitted past the cap, which is what outposts and tents
  /// do). Both raise loyalty by `LoyaltyIncreasePerUnit`, and leaving lowers it
  /// again — INFERRED, but it is what makes `LoyaltyUnitsOutTreshold = 151`
  /// mean something: a settlement holding fifteen units sits above it and keeps
  /// them in, and one holding fewer sends them out to fight.
  bool garrison_add(SettlementId id, ObjectId unit);
  bool garrison_force_add(SettlementId id, ObjectId unit);
  bool garrison_remove(SettlementId id, ObjectId unit);

  /// `SetPlayer` from `UNIT_CAPTURE.VS`: refused unless loyalty has reached
  /// zero, then the settlement changes hands and loyalty is set to 11.
  bool capture(SettlementId id, PlayerId new_owner);
  /// An unconditional transfer — an allied unit walking in, or a scenario
  /// script. `UNIT_CAPTURE.VS` takes this path for a non-enemy owner.
  bool set_owner(SettlementId id, PlayerId new_owner);

  // -- sentries ----------------------------------------------------------

  bool add_max_sentries(SettlementId id, std::int32_t count);
  bool add_sentries(SettlementId id, std::int32_t count);

  // -- burning -----------------------------------------------------------

  bool set_burning(SettlementId id, bool burning);

  // -- tavern ------------------------------------------------------------

  /// `TAVERN_GETLOAN.VS`: `SetLoan(LoanAmount); SetGold(gold + LoanAmount)`.
  /// The script assigns rather than adds, so a second loan replaces the debt.
  bool take_loan(SettlementId id);
  /// `TAVERN_REPAYLOAN.VS`, line for line.
  bool repay_loan(SettlementId id);
  /// `TAVERN_INVESTMENT.VS`: pay `investment_cost`, receive
  /// `investment_revenue`. Refused when the settlement cannot pay.
  bool invest(SettlementId id);
  /// `EMARKET_BUYSLAVES.VS`: population becomes `max_population`, for
  /// `SlaveryGold`.
  bool buy_slaves(SettlementId id);
  /// `TAVERN_BUYFOOD.VS`: the food arrives; the gold is the command's cost and
  /// is charged by the caller.
  bool buy_food(SettlementId id, std::int32_t food);

  /// `UNIT_ON_ENTER.VS`: `SpoilsOfWarGold` per item carried in.
  std::int32_t deliver_spoils(SettlementId id, std::int32_t item_count);

  // -- statistics --------------------------------------------------------
  //
  // `GoldConverted`, `GoldSpent`, `SpentGoldOnArmy` and `SpentGoldOnTech` are
  // called all over the AI scripts and feed the end-of-game report. They are
  // per-settlement running totals and nothing reads them back inside the
  // simulation, so they are kept but deliberately left out of the hash.

  void record_gold_converted(SettlementId id, std::int32_t amount);
  [[nodiscard]] std::int32_t gold_converted(SettlementId id) const;

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
  /// **Written:** the settlement store, the wagons in transit, the wagon id
  /// counter, the per-settlement statistics and the `start`ed flag.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** `EconomyRules` and `auto_deliver_`, which are the
  /// embedder's configuration and are the same on both sides of a load.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  struct Statistics {
    std::int32_t gold_converted = 0;
  };

  /// What `create` does after the store has a row: rates, sentries, loyalty,
  /// timers, the statistics row. Shared with `recreate`.
  void finish_creation(SettlementId id, const SettlementInit& init);

  /// Run every timer that comes due inside `[0, length)`, stepping to each due
  /// instant in turn. See the header comment: this is what makes a turn
  /// splittable.
  void advance_settlement(World& world, Settlement& s, std::int32_t length);
  void fire(World& world, Settlement& s, SettlementTimer which);
  [[nodiscard]] std::int32_t interval(SettlementTimer which) const noexcept;

  void produce(World& world, Settlement& s);
  void grow(Settlement& s);
  void trim_population(Settlement& s);
  void burn(Settlement& s);
  void repair(World& world, Settlement& s);
  void tick_loyalty(Settlement& s);
  void tick_outpost_trade(World& world, Settlement& s);
  void tick_loan(Settlement& s);

  EconomyRules rules_{};
  SettlementStore store_;
  std::vector<Wagon> wagons_;
  std::vector<Statistics> stats_;
  std::uint32_t next_wagon_ = 1;
  bool auto_deliver_ = true;
  bool started_ = false;
};

/// Derive the `capture/attack/gold/food` quad's last two fields from the
/// anchor's `produces_gold` / `produces_food` class properties.
///
/// Fitted to all 328 dumped settlements, whose only four non-zero rate pairs
/// are `24 0` (every town hall), `0 24` (every village), `0 0` (everything that
/// produces nothing) and `12 24` — which occurs twice and only on `TTownhall`,
/// the one shipped class that sets both flags. So a settlement that produces
/// food as well as gold produces gold at half rate. **The halving is fitted,
/// not explained**: no shipped file states it, and two samples is two samples.
void settlement_production_rates(const EconomyRules& rules, bool produces_gold,
                                 bool produces_food, std::int32_t& gold_rate,
                                 std::int32_t& food_rate) noexcept;

// --------------------------------------------------------------------------
// host functions
// --------------------------------------------------------------------------

/// The economy system registered on `world`, or null.
///
/// Host functions receive a `HostContext*` in `CallContext::user` -- one type
/// for every domain, which `sim/host_context.hpp` explains at length because it
/// was once one type per domain -- and the economy is a system on that world
/// rather than a field of it, so this is how it is reached.
/// Systems are few and the lookup is by `name()`, which is already part of the
/// run-order manifest.
struct CommandDef;

/// The settlement's building that offers `row`, or `kNoObject`: the walk of
/// 0x0042d010 over the settlement's building roll, first hit wins, matching
/// `<src obj>` against the class tree, and -- with `must_be_idle` -- skipping
/// a building whose running command is not `idle`. `FindResearchLab` passes
/// false, `Settlement::Research` and `CanResearch` true.
[[nodiscard]] ObjectId settlement_lab_for(World& world, const Settlement& s,
                                          const CommandDef& row, bool must_be_idle);

[[nodiscard]] EconomySystem* economy_of(World& world) noexcept;

// -- the garrison ----------------------------------------------------------
//
// A settlement's garrison is its holder (`settlement handle + 1`): the units
// in `Holder::units` are *inside* it, off the map at `kHeldPosition` with
// `ObjectState::holder` naming the holder object, where nothing sweeps, sees
// or strikes them. They go in through `AddUnit` and `ForceAddUnit` and come
// out when they are told to go somewhere.

/// `0x005d3e10`, the holder entry `AddUnit` (0x005c1b20) and `ForceAddUnit`
/// (0x005c1bc0) both call: out of whatever holder `unit` was in, onto the
/// roster (`force` admits it past `max_units`), and then -- only once the
/// holder has accepted it -- its timers, its action and its route are
/// dropped and it is put at the held sentinel inside the holder. False when
/// the roster refused it, which leaves the unit where it was.
///
/// The `onenter` hook is the caller's, after this (`sim/hooks.hpp`): the
/// original fires it last, from the same routine, and only on this path.
bool garrison_enter(World& world, SettlementId id, ObjectId unit, bool force);

/// `0x005d3f20`: a unit inside a settlement's holder steps out, towards
/// `toward`. What `Goto` (0x005d62d6), `FormSetupAndMoveTo` and
/// `FormAcceptMove` do first when their unit is held.
///
/// Returns the milliseconds the caller has to wait before the unit may go,
/// or 0 once it is out (and at once for a unit that is not in a settlement's
/// holder, which this leaves alone -- a ship's passengers leave through
/// `sim/boarding.hpp`). With `throttled`, a settlement lets one unit out per
/// `exit_interval` (`[settlement+0xa0]`, 20 ms in 324 of the 328 dumped
/// blocks) after the last (`[settlement+0x9c]`, `last_unit_exit_time`); the
/// wait it answers is `exit_interval - last_unit_exit_time + now`, the
/// original's arithmetic as it stands (0x005d3f60), which is at least the
/// interval rather than what is left of it. Going out, the unit takes what
/// food it lacks from the settlement's store (`maxfood - food`, 0x005eec70),
/// and it is put at the central building's exit door chosen towards
/// `toward` by the same worker `GetExitPoint` uses (0x004db6c0, an RNG
/// draw), or at `toward` itself for a settlement with no central building.
std::int32_t garrison_exit(World& world, ObjectId unit, Point toward, GameTime now,
                           bool throttled = true);

/// Take `unit` off every settlement roster that holds it, lowering loyalty as
/// leaving does. For the dead and the erased: the holder removal (0x00531a80)
/// is the same routine whatever took the unit away. Returns whether any
/// roster held it.
bool garrison_forget(World& world, ObjectId unit);

/// A unit's effective level as the executable's `vtbl+0x114` answers it: the
/// combatant's floored level plus its owner's difficulty addend; a unit the
/// combat system does not hold answers the hero system's level, and one it
/// knows nothing of, 0. `sim/economy.cpp` carries the reading.
[[nodiscard]] std::int32_t effective_level_of(World& world, ObjectId id);

/// The economy's slice of `DATA\CONST.INI`, by key.
///
/// ~40 of the section's keys are `EconomyRules` fields rather than table rows,
/// so that `set_rules` can retune the economy for a test without a data file.
/// `GetConst` lives in `sim/env.cpp` and asks here first, which is what keeps
/// the two from answering differently for the same key. Returns false when the
/// key is not one of the economy's.
[[nodiscard]] bool economy_constant(const EconomyRules& rules, std::string_view key,
                                    std::int32_t& out) noexcept;

/// Define the economy slice of the host API. Returns how many entry points were
/// defined, so a caller can assert against the count rather than trust it.
///
/// Ordered by call frequency in `docs/formats/vs-host-api.md`, because that is
/// the order in which they pay: `gold` is 125 call sites, `food` 109,
/// `population` 50. `Building::settlement` (432) and `player` (531) are the
/// object model's and are defined by `register_world_host`; a settlement handle
/// is `(kTypeSettlement, settlement object id)` on both sides of that seam.
///
/// Everything defined here is already declared by `declare_shipped_surface`
/// with the arities the corpus uses, so this adds behaviour and no names.
std::size_t register_economy_hosts(script::HostRegistry& registry);

/// The number of entry points `register_economy_hosts` defines. Used by the
/// tests to notice a silent regression in the table.
[[nodiscard]] std::size_t economy_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
