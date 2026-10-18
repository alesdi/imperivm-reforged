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
class PlayerTable;

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
  /// `WagonBuildTime = 7000`. Held only so `GetConst` answers it: nothing in
  /// `gbr.exe` names the key and no shipped script asks for it. A settlement's
  /// mule exists the moment `CreateMuleGold` makes it (0x005eed90); there is
  /// no build time.
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
  /// `LoyaltyIncreasePerUnit = 10`, `LoyaltySettlementsInitial = 10`,
  /// `LoyaltyInterval = 2000`, `LoyaltyRadius = 1000`,
  /// `LoyaltyRadiusTownhall = 1500`. `gbr.exe` caches the first five at
  /// 0x005c0da3..0x005c0e85 and the two radii on first use at 0x005c56cf and
  /// 0x005c5713. The cap bounds the net change of a window both ways (see the
  /// loyalty note on `EconomySystem`), although the constant's comment speaks
  /// only of decrease. `LoyaltyIncreasePerUnit` is in tenths of a point.
  std::int32_t loyalty_cap_interval = 2000;
  std::int32_t loyalty_change_cap = 10;
  std::int32_t loyalty_increase_per_unit = 10;
  std::int32_t loyalty_settlements_initial = 10;
  std::int32_t loyalty_interval = 2000;
  std::int32_t loyalty_radius = 1000;
  std::int32_t loyalty_radius_townhall = 1500;
  /// Not a constant: `UNIT_CAPTURE.VS` writes `SetLoyalty(11)` literally, the
  /// instant a settlement changes hands.
  std::int32_t loyalty_after_capture = 11;
  /// `LoyaltyUnitsOutTreshold = 151`.
  std::int32_t loyalty_units_out_threshold = 151;
  /// A wagon's own five: `WagonLoyaltyInterval = 1000`,
  /// `WagonLoyaltyCapInterval = 500`, `WagonLoyaltyChangeCap = 5`,
  /// `WagonLoyaltyIncreasePerUnit = 10`, `LoyaltyWagonsInitial = 100`, cached
  /// together by 0x005eaef0 the first time a wagon is built or loaded. **The
  /// initial value is used as tenths, unmultiplied** -- the wagon constructor
  /// (0x005eae80) stores it straight into `[wagon+0x1d8]`, where the
  /// settlement's stores ten times its own -- so a fresh wagon reads
  /// `loyalty` 10, and the cap tick holds a wagon at or below it. See
  /// `WagonLoyalty`.
  std::int32_t wagon_loyalty_interval = 1000;
  std::int32_t wagon_loyalty_cap_interval = 500;
  std::int32_t wagon_loyalty_change_cap = 5;
  std::int32_t wagon_loyalty_increase_per_unit = 10;
  std::int32_t loyalty_wagons_initial = 100;

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

/// A wagon object's loyalty, as `gbr.exe` keeps it on the `CVXWagon` itself.
/// `docs/formats/wagon.md` is the whole model; this is the part of it this
/// table holds.
///
/// Three fields on the object and two of its timers: the loyalty in tenths
/// (`[wagon+0x1d8]`, which `Wagon::loyalty` answers over ten), the pending
/// change (`[+0x1dc]`, the save's `loyalty-cap`), the capture flag
/// (`[+0x1d0]`), and timers 9 and 10, which run exactly while the flag is up.
/// **Kept here, keyed by the wagon's handle, rather than on every object**,
/// because the constructor (0x005eae80) gives every wagon the same state --
/// `LoyaltyWagonsInitial` tenths, nothing pending, no capture, no timer --
/// and only three entry points ever move it from there: so a wagon with no
/// row is in that state, and a row exists only for a wagon a script has
/// touched. A row whose object is gone is dropped by `advance` (handles are
/// never reused, so a stale row could never be read; dropping it keeps the
/// hash honest).
///
/// The timers count down in the economy's own event-stepped clock, so the
/// same instants fire whatever the turn lengths. `gain_remaining` is timer 9
/// (`WagonLoyaltyInterval`, the guard count 0x005ec410) and `cap_remaining`
/// timer 10 (`WagonLoyaltyCapInterval`, the cap 0x005eb100); both are zero
/// whenever `capturing` is false, and both positive while it is true.
struct WagonLoyalty {
  ObjectId wagon = kNoObject;
  std::int32_t loyalty_tenths = 0;
  std::int32_t pending = 0;
  bool capturing = false;
  std::int32_t gain_remaining = 0;
  std::int32_t cap_remaining = 0;
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
  ///
  /// `gbr.exe` 0x005c2250: a receiver that does not resolve prints and does
  /// nothing; otherwise one `add` to `settlement + 0x50`, so a negative delta
  /// can take the population below zero. Every shipped site adds (1, 10, 59,
  /// 99 or a conquest's carried population), so no shipped play reaches one.
  bool add_population(SettlementId id, std::int32_t delta);
  /// `AddToMaxPopulation`: raise (or lower) the ceiling itself.
  ///
  /// `gbr.exe` 0x005c2290 is one `add` to `settlement + 0x4c` and no clamp of
  /// any kind, after the same print-and-return for a receiver that does not
  /// resolve. The four shipped sites all raise it -- `ONFINISH_RESEARCH.VS`'s
  /// "Housing" by 20, and three campaign sequences.
  bool add_max_population(SettlementId id, std::int32_t delta);

  // -- transport ---------------------------------------------------------

  /// Name the settlement this one ships its surplus to. `StartSupplyFood` /
  /// `StopSupply` with `kNoSettlement`. Any id is stored as given, the
  /// settlement's own included (0x005ef380 tests nothing).
  bool set_supplied(SettlementId id, SettlementId target);

  // -- capture and loyalty ----------------------------------------------

  /// `IsValidCaptureTarget`: capturable at all, and — for the classes that
  /// declare a threshold below 100, which in the retail data is `Outpost` at
  /// 50 — damaged to at or below `capture_health_percent` of the anchor's
  /// maximum health. INFERRED: the property's name and its two shipped values
  /// support this reading, and no script states it.
  [[nodiscard]] bool is_valid_capture_target(const World& world, SettlementId id) const;

  /// ## Loyalty, as `gbr.exe` keeps it
  ///
  /// A percentage held in tenths (`Settlement::loyalty_tenths`, `[+0x54]`)
  /// beside a pending change (`loyalty_pending`, `[+0x58]`). Five places write
  /// the two fields, and nothing else in the settlement code does: the two
  /// constructors (0x005c3fac, 0x005c4c4f: `10 * LoyaltySettlementsInitial`
  /// and nothing pending), `SetLoyalty`, `DecreaseLoyalty`, and two timers
  /// every settlement starts with the rest (0x005c0ea0):
  ///
  ///   * **the gain tick**, every `LoyaltyInterval` (0x005c5690): it counts
  ///     the soldiers -- live heirs of `Military`, not hidden, not unspawned,
  ///     not a `Sentry` or a `RamUnit` -- of every player the owner is not at
  ///     war with, standing within `LoyaltyRadius` of the central building, or
  ///     `LoyaltyRadiusTownhall` when that is a `BaseTownhall` (the sweep
  ///     0x005eb9d0, whose only other caller is the wagon's); then every live
  ///     `Military` unit in the settlement's own holder (`[+0x5e]`, the deque
  ///     `UnitsCount` reads). The pending change grows by
  ///     `LoyaltyIncreasePerUnit` per soldier counted, and 3.
  ///   * **the cap tick**, every `LoyaltyCapInterval` (0x005c0f00): the pending
  ///     change, clamped to `10 * LoyaltyChangeCap` either way, is added to the
  ///     loyalty, which is then held between 0 and 1,000 tenths, and the
  ///     pending change is cleared.
  ///
  /// So a town is captured by out-taunting the soldiers who stand by it: each
  /// taunt (`UNIT_CAPTURE.VS`'s `DecreaseLoyalty(1)` every two seconds) is a
  /// point against, each friendly soldier near the hall or inside it a point
  /// for, and the net moves at most ten points a window.
  ///
  /// Garrisoning a unit writes no loyalty (it counts in the gain tick instead),
  /// and loyalty never passes 100, so `TOWNHALL_BEHAVIOR_GUARD.VS`'s
  /// `loyalty < GetConst("LoyaltyUnitsOutTreshold")` -- 151 -- always holds and
  /// a town hall with enemies in sight always sends its garrison out.
  ///
  /// Labelled, not settled: which of the two timers fires first when both
  /// come due at one instant (here the gain, so it lands in the same window);
  /// and the sweep's cell walk, which accepts a cell wholly inside the circle
  /// without testing each object and tests the rest by `d^2 <= r^2` -- taken
  /// here as `d^2 <= r^2` for every object, which is the same set.
  ///
  /// `DecreaseLoyalty(n)`: `10 * n` off the pending change; a negative `n` is
  /// refused (the original prints and writes nothing). Returns whether it was
  /// taken.
  bool decrease_loyalty(SettlementId id, std::int32_t amount);
  /// `SetLoyalty(v)`: `v * 10`, for `v` from 0 to 100; anything else is
  /// refused and writes nothing, as 0x005c280d refuses it. The pending change
  /// is left alone.
  bool set_loyalty(SettlementId id, std::int32_t value);

  /// Garrison a unit: `AddUnit` (refused when the holder is full) and
  /// `ForceAddUnit` (admitted past the cap, which is what outposts and tents
  /// do). Neither writes loyalty -- see the loyalty note above.
  bool garrison_add(SettlementId id, ObjectId unit);
  bool garrison_force_add(SettlementId id, ObjectId unit);
  bool garrison_remove(SettlementId id, ObjectId unit);

  /// `SetPlayer` from `UNIT_CAPTURE.VS`: refused unless loyalty has reached
  /// zero, then the settlement changes hands and loyalty is set to 11 (110
  /// tenths); the pending change is left, as `SetLoyalty` leaves it.
  bool capture(SettlementId id, PlayerId new_owner);
  /// An unconditional transfer — an allied unit walking in, or a scenario
  /// script. `UNIT_CAPTURE.VS` takes this path for a non-enemy owner.
  bool set_owner(SettlementId id, PlayerId new_owner);

  // -- a wagon's loyalty -------------------------------------------------
  //
  // The three `Wagon::` entry points and the two timers they arm. See
  // `WagonLoyalty` for where the state lives and `docs/formats/wagon.md` for
  // the model.

  /// `[wagon+0x1d8]`: the row's tenths, or `LoyaltyWagonsInitial` for a wagon
  /// with no row. `Wagon::loyalty` (0x005ec090) answers this over ten.
  [[nodiscard]] std::int32_t wagon_loyalty_tenths(ObjectId wagon) const noexcept;
  /// The row, or null for a wagon still in its constructed state.
  [[nodiscard]] const WagonLoyalty* find_wagon_loyalty(ObjectId wagon) const noexcept;
  [[nodiscard]] std::span<const WagonLoyalty> wagon_loyalties() const noexcept {
    return wagon_loyalty_;
  }
  /// `Wagon::SetLoyalty(v)` (0x005ec100): `v * 10` for `v` from 0 to 100,
  /// refused (and nothing written) outside. Neither the pending change nor
  /// the capture is touched.
  bool set_wagon_loyalty(ObjectId wagon, std::int32_t value);
  /// `Wagon::DecreaseLoyalty(n)` (0x005ec160): a negative `n` is refused;
  /// otherwise, when the capture flag is down, timers 9 and 10 are armed and
  /// the flag raised, and then the pending change drops by `10 * n`.
  ///
  /// `late` is how far behind the economy's clock the caller stands -- a
  /// script runs at an instant inside the turn the economy has already
  /// stepped to its end -- so the timers fall due where the original's
  /// would, `interval` after the call; one already overdue falls due on the
  /// first millisecond of the next turn.
  bool decrease_wagon_loyalty(ObjectId wagon, std::int32_t amount, std::int32_t late = 0);

  // -- sentries ----------------------------------------------------------

  /// `AddMaxSentries(n)`, 0x005c2b30: one `add` to the ceiling at
  /// `[set+0x104]`. No clamp, and a receiver that does not resolve is
  /// ignored without a print.
  bool add_max_sentries(SettlementId id, std::int32_t count);
  /// `AddSentries(n)`, 0x005c4ac0: one `add` to the roster at `[set+0xfc]`,
  /// and the wall pool `[set+0xf4]` moved by the same `n` through its
  /// upkeep-keeping setter (0x005c4930). **No clamp in either**: a negative
  /// `n` takes both below zero, and that is what
  /// `2_Great_Battles_Numantia` map 2's first sequence relies on --
  /// `GetSettlement("Numantia").AddSentries(-12)` before the town hall's
  /// `TOWNHALL_SENTRIES_CONTROL.VS` has added any, so the town starts twelve
  /// sentries in debt and the control loop (`max - curr`, at most
  /// `SentriesAddCount` a pass) takes that many more passes to man its walls.
  /// Clamping at zero made the call do nothing at all. A receiver that does
  /// not resolve is ignored without a print.
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
  /// **Written:** the settlement store, the per-settlement statistics, the
  /// wagons' loyalty rows and the `start`ed flag. A wagon's cargo is on the
  /// wagon object, in the world's section.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** `EconomyRules`, which is the
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
  void tick_loyalty_gain(World& world, Settlement& s);
  /// A wagon's timer 9 (0x005ec410) and timer 10 (0x005eb100).
  void tick_wagon_guard(World& world, WagonLoyalty& w);
  void tick_wagon_cap(WagonLoyalty& w);
  /// The row for `wagon`, made in its constructed state if there is none.
  WagonLoyalty& wagon_loyalty_row(ObjectId wagon);
  void tick_outpost_trade(World& world, Settlement& s);

  EconomyRules rules_{};
  SettlementStore store_;
  /// Ascending by wagon handle, which is the order `advance` ticks them in.
  std::vector<WagonLoyalty> wagon_loyalty_;
  std::vector<Statistics> stats_;
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

/// The wagon's cargo setter, 0x005eb0a0 -- every write of a wagon's cargo
/// goes through it (`docs/formats/wagon.md`): `ObjectState::cargo` takes
/// `value` unclamped, and **a value of exactly zero kills a wagon that is not
/// already dead** through its death virtual (`CombatSystem::kill`, `ondie`
/// and all). The kind is the caller's to write first. The carrying-food flag
/// the original also sets here (`0x20000000`) is not modelled: nothing but
/// the state dump reads it. The editor guard (`[0x00a8734c]`) is not either:
/// no script runs under it here. Nothing for an id that is not an object.
void set_wagon_cargo(World& world, ObjectId wagon, std::int32_t value);

/// 0x005eed90, behind `CreateMuleGold`/`CreateMuleFood` (`boat` false) and
/// `CreateBoatGold`/`CreateBoatFood` (`boat` true), and the load step of
/// `Squad::SendFoodWagon`: the carrier's class (`ShipS`, or the settlement's
/// race's mule), `amount` clamped to that class's `max_load` and to the
/// store, the store drawn by it, and -- for a nonzero load -- the carrier
/// made, given the kind and the cargo, owned by the settlement's player and
/// forced into the settlement's holder. Answers the carrier, or `kNoObject`
/// for nothing loaded. **A negative request loads nothing here**; the original
/// clamps only downwards and would grow the store and hand back a carrier
/// with a negative load, which no shipped site asks for.
ObjectId make_mule(World& world, SettlementId id, Resource kind, std::int32_t amount, bool boat);

/// The feeder's second path: a hungry unit eats from a food wagon following
/// its squad. The original's food search (0x005dbba0 with the predicate
/// 0x005d58d0) sweeps around the unit for a store, a food wagon -- the fullest
/// -- or a fed unit, and draws `min(want, cargo)` from the wagon through the
/// cargo setter (0x005d3700), so the wagon that gives its last food dies.
/// **What is approximated is the reach**: that search is the feeder's design
/// and is not modelled here, so "within reach" is "a live food wagon whose
/// running command is aimed at one of `members`" -- what `SendFoodWagon`
/// makes, a mule following the army at 70 (`WAGON_FOLLOW.VS`). The fullest
/// such wagon, the lowest handle of two equally full. Answers what was taken.
std::int32_t draw_from_food_wagon(World& world, std::span<const ObjectId> members,
                                  std::int32_t want);

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

/// Whether `unit` is held by an object rather than by a settlement: what
/// `Goto` (0x005d62b9), `Hero::FormSetupAndMoveTo` (0x0052e674) and
/// `Unit::FormAcceptMove` (0x005d791d) ask of a held unit's holder before
/// they try `garrison_exit`. 0x005319a0 resolves the handle at
/// `[holder+0xe]`, the object the holder belongs to; a settlement's holder
/// names none, a ship's names the ship. When it resolves the caller does not
/// step out at all: it waits 100 ms and asks again, so a passenger stays
/// aboard until the ship lets it off. The one such holder this engine mints is
/// a ship's, at the ship's handle plus one (`World::spawn_ship`), which is how
/// it is recognised here, as `Unit::InShip` recognises it.
[[nodiscard]] bool held_by_carrier(const World& world, ObjectId unit);

/// `Unit::posRH` (0x005d3db0, the unit's own `vtbl+0xc8`): where a unit
/// stands for the questions that ask which node it is in. A unit on the map
/// answers its own position. A unit in a holder answers **its settlement's
/// central building position** (0x005c1060: the object at `[settlement+0x8c]`,
/// read through `vtbl+0x3c`) when the holder belongs to a settlement
/// (`[holder+0xc]`), and otherwise the position of the object the holder
/// belongs to (0x005319a0, `[holder+0xe]`) -- a ship's passengers are where
/// the ship is. `World::resolve_position` does not follow that: a ship's
/// holder record is a separate internal object standing at (0, 0), so the
/// ship is found here as the object just before its holder.
///
/// Two readers in `gbr.exe` take it: the squad's node tracker 0x0041f530,
/// which the holder entry 0x005d3e10 (`garrison_enter`'s original, at
/// 0x005d3ed9) and the unit's `SetPos` 0x005d39e0 (at 0x005d3a6f) run with
/// the old and the new `posRH`, and `GetGAIKA(Obj)` (0x00424660 ->
/// 0x0044e650), which turns to it when the raw position is off the map; and
/// `Squad::pos` (0x00422cf0, the front member's `vtbl+0xc8`).
/// `World::resolve_position` stops at the holder record instead, an internal
/// object that stands at (0, 0), so before this every garrisoned squad -- in a
/// town, a tower or a siege engine -- was filed under whichever node lies
/// nearest the map's corner: an army at home looked away from home and was
/// sent back to its own gate, and a siege engine's crew vanished from the node
/// it was besieging the moment it climbed in.
///
/// **For units only**, as the override is: an item or another held object
/// keeps `resolve_position` here, which is not what its own `posRH` answers
/// (that is its raw position, `(-1, -1)` while held; see `m_pos_rh` in
/// `sim/world_host.cpp`) and is not this function's question.
[[nodiscard]] Point unit_pos_rh(World& world, ObjectId unit);

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

/// 0x0044e250: how 0-based player `a` sees player `b` -- 1 (`AI_OWN`), 2
/// (`AI_ALLY`) or 4 (`AI_ENEMY`) from `a`'s row alone, 0 for an `a` outside
/// the table or an invalid `b`. Shared by `NearestStronghold` and the region
/// census (`sim/squad.cpp`).
[[nodiscard]] std::int32_t ai_relation(const PlayerTable& players, std::int32_t a,
                                       PlayerId b) noexcept;

/// `[settlement+0xf8]`, the sentry strength the original caches on every
/// write of the ready-wall count; see the definition.
[[nodiscard]] std::int32_t sentry_strength(World& world, const Settlement& set);

}  // namespace imperivm::core::sim
