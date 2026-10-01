#pragma once

/// The settlement: the object a town's economy and capture state live on.
///
/// Evidence: `docs/engine/state-vector.md` (the `CVXSettlement` block, n=328
/// over nine dumps), `docs/data-model.md` (the `CVXTownHall` property set),
/// `DATA\CONST.INI` and the shipped `DATA\SUBAI\*.vs` behaviours. Where a
/// constant and a script disagree the script wins, and every rule below says
/// which of the three it came from.
///
/// ## A settlement is not a town hall
///
/// The dumps settle this. A `CVXSettlement`'s `class` field names its *anchor*
/// object, and the 31 values it takes include `ROutpost`, `Teleport_1`,
/// `RCatapult`, `Ruins1` and `Stonehenge1` as well as the town halls and
/// shipyards; settlements outnumber town halls in every dump (39 vs 19). So the
/// settlement is a separate engine-internal record that an anchor building owns,
/// and a ruin owns one that produces nothing. That is why `SettlementKind`
/// exists and why almost every rule here is gated on a rate or a flag being
/// non-zero rather than on the anchor's class.
///
/// ## Settlement, holder, warehouse
///
/// The three are allocated as one unit at consecutive handles, 328 of 328 with
/// no exceptions, and they divide the state cleanly:
///
///   * the **settlement** carries population, the production rates, capture and
///     loyalty, and the timers;
///   * the **holder** carries the garrison — it is the general "this contains
///     units" container, and ships have one too;
///   * the **warehouse** carries the stored gold and food. The dumper folds its
///     contents onto the settlement's `gold/food` line, which is the only place
///     they are visible, but the store is the warehouse's.
///
/// This header reproduces that split rather than flattening it, because the
/// three object ids are world state and a wagon unloads into a warehouse.
///
/// **No floating point.** Every percentage below is integer arithmetic:
/// `value * percent / 100`, truncating, evaluated in that order.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// A settlement's index in the world's settlement array.
///
/// The dumps print it as `ID`, dense and ascending within each file, one per
/// settlement, and it is explicitly *not* a handle: `ID` runs 0..42 while the
/// handles it sits beside run into the thousands. Maps declare it too, as the
/// `id` attribute of `<settlement>`.
using SettlementId = std::uint32_t;

inline constexpr SettlementId kNoSettlement = 0xFFFFFFFFu;

/// The two resources. The values are the engine's own: `WAGON_UNLOAD.VS` reads
/// `if (.restype == 1) { … food } else { … gold }`.
enum class Resource : std::uint8_t {
  gold = 0,
  food = 1,
};

/// What the settlement's anchor is, to the extent the shipped data says.
///
/// The script API asks exactly these questions — `IsStronghold`, `IsVillage`,
/// `IsOutpost`, `IsShipyard`, `IsTeutonTent`, `IsCity` — so they are state
/// rather than a class-graph lookup at every call site. `other` covers the
/// anchors that own a settlement but answer no to all of them: ruins,
/// teleports, catapults, stonehenges, inns.
enum class SettlementKind : std::uint8_t {
  other = 0,
  stronghold,  ///< a town hall: `BaseTownhall` and its races
  village,     ///< `BaseVillage`: produces food, supplies a stronghold
  outpost,     ///< tributes to another settlement, spawns mules
  shipyard,    ///< `BaseShipyard`
  teuton_tent, ///< `TTent`
};

/// The settlement's resource store, at `settlement handle + 2`.
///
/// The dump prints only its handle, so nothing here is contradicted by the
/// corpus; the caps come from the class graph (`settlement_maxgold`,
/// `settlement_maxfood`) and from the map, whose `<settlement>` element carries
/// `maxgold` and `maxfood` per instance.
struct Warehouse {
  ObjectId object = kNoObject;
  std::int32_t gold = 0;
  std::int32_t food = 0;
  /// A zero cap means "no store": ruins and teleports ship `settlement_maxgold=0`
  /// and must not accumulate. It does **not** mean unbounded.
  std::int32_t max_gold = 0;
  std::int32_t max_food = 0;

  [[nodiscard]] std::int32_t amount(Resource r) const noexcept {
    return r == Resource::food ? food : gold;
  }
  [[nodiscard]] std::int32_t capacity(Resource r) const noexcept {
    return r == Resource::food ? max_food : max_gold;
  }

  /// Add up to `amount`, saturating at the cap. Returns what was accepted.
  std::int32_t store(Resource r, std::int32_t amount) noexcept;
  /// Remove up to `amount`, never below zero. Returns what was taken.
  std::int32_t take(Resource r, std::int32_t amount) noexcept;
  /// Overwrite, clamped to `[0, cap]`. The shape `SetGold`/`SetFood` need.
  void set(Resource r, std::int32_t value) noexcept;
};

/// The garrison container, at `settlement handle + 1`.
///
/// `CVXHolder` is the general case and ships carry one as well, which is why it
/// is a type of its own here rather than a vector on the settlement.
struct Holder {
  ObjectId object = kNoObject;
  /// `max_units` from the class graph: 10,000 on a town hall, 0 on a village.
  std::int32_t max_units = 0;
  /// Spawn order, never a set: iteration order is world state.
  std::vector<ObjectId> units;

  [[nodiscard]] std::int32_t count() const noexcept {
    return static_cast<std::int32_t>(units.size());
  }
  [[nodiscard]] bool full() const noexcept { return count() >= max_units; }
  [[nodiscard]] bool contains(ObjectId id) const noexcept;
  /// `AddUnit`: refused when full.
  bool add(ObjectId id);
  /// `ForceAddUnit`: admitted past the cap. Outposts place their whole garrison
  /// this way (`OUTPOST_BEHAVIOR.VS`), and tents "force add even though the tent
  /// does not allow units inside".
  bool force_add(ObjectId id);
  bool remove(ObjectId id);
};

/// The intervals a settlement keeps, in the order ties between them are broken.
///
/// The order is part of the simulation's definition: when two timers come due at
/// the same instant they fire in this sequence, every time, on every peer. It is
/// chosen so that a settlement's inputs land before its outputs — production
/// before growth spends food, growth before the starvation check, and the
/// transport that empties the store last.
enum class SettlementTimer : std::uint8_t {
  production = 0,  ///< `ProductionInterval`
  growth,          ///< `PopulationGrowthInterval`
  decrease,        ///< `PopulationDecreaseInterval`
  burn,            ///< `BurnTime`
  repair,          ///< `RepairInterval`
  loyalty,         ///< `LoyaltyCapInterval`: rolls the decrease-cap window over
  outpost_trade,   ///< `OutpostRefresh`
  loan,            ///< `TimeProductionForLoan`
  count,
};

inline constexpr std::size_t kSettlementTimerCount =
    static_cast<std::size_t>(SettlementTimer::count);

/// What an outpost does with the resources it holds, per `OUTPOST_IDLE.VS`.
///
/// Both branches are literally in the shipped script and both are gated on a
/// per-settlement environment string the player sets from the command bar.
enum class OutpostTrade : std::uint8_t {
  none = 0,
  /// Gaulish: `food -= GOutpostFoodSales; gold += GOutpostGoldReturn`, every
  /// `OutpostRefresh`, while food exceeds the sale size.
  sell_food,
  /// Roman and Egyptian: `gold += ROutpostGoldReturn` every `OutpostRefresh`
  /// while `gold >= ROutpostRequiredGold`. Interest on a deposit.
  gold_interest,
};

/// One of a settlement's buildings, as the economy needs it.
///
/// The maximum health is copied here rather than looked up. The economy has no
/// class graph and no combat table, repair needs a ceiling to stop at, and
/// `capture_health_percent` needs one to compare against; carrying the number
/// with the binding keeps the domain self-contained and its provenance obvious
/// (`maxhealth` on the class, `healthperc` on the map object).
struct SettlementBuilding {
  ObjectId object = kNoObject;
  std::int32_t max_health = 0;
};

/// What a settlement is created from: the map's `<settlement>` attributes and
/// the anchor class's resolved properties, in one place.
///
/// The map is the authority where the two overlap. `map.obj.xml` carries
/// `population`, `maxpopulation`, `gold`, `food`, `maxgold`, `maxfood` and
/// `extrasentries` per settlement, and the tick-2 dumps show exactly those
/// values, not the class defaults.
struct SettlementInit {
  /// The three consecutive handles. Left null when the allocator has not run;
  /// nothing here dereferences them.
  ObjectId settlement_object = kNoObject;
  ObjectId holder_object = kNoObject;
  ObjectId warehouse_object = kNoObject;
  /// The anchor: the object whose `ScriptClass` the dump prints as `class`, and
  /// what `GetCentralBuilding` returns.
  ObjectId anchor = kNoObject;

  PlayerId owner = kNoPlayer;
  SettlementKind kind = SettlementKind::other;

  /// `<settlement name="...">`, verbatim and possibly empty.
  ///
  /// 188 of the installation's 684 settlements carry one, with no duplicate and
  /// no case-only collision inside any single map. It is what
  /// `WaitSettlementCapture("S_Yard", 1, -1)` and `GetSettlement/1` look up.
  std::string name;

  // -- from the anchor class's properties -------------------------------
  bool can_be_captured = false;
  bool can_be_attacked = false;
  bool produces_gold = false;
  bool produces_food = false;
  /// `efficiency`: 1 on every producing class, 0 on outposts, shipyards and
  /// ruins. Used as a multiplier, which is all the shipped range of {0,1}
  /// can distinguish.
  std::int32_t efficiency = 0;
  /// `foodperpop`: 45 or 100 depending on race, declared by every village and
  /// town hall class and by nothing else.
  std::int32_t food_per_pop = 0;
  /// `capture_health_percent`: 100 on `Building`, 50 on `Outpost`.
  std::int32_t capture_health_percent = 100;
  /// The anchor's `maxhealth`. 5,000 on an outpost; the dumps show buildings at
  /// 5,000, 1,500 and 1,000.
  std::int32_t anchor_max_health = 0;
  std::int32_t max_units = 0;

  // -- from the map's <settlement> --------------------------------------
  std::int32_t population = 0;
  std::int32_t max_population = 0;
  std::int32_t gold = 0;
  std::int32_t food = 0;
  std::int32_t max_gold = 0;
  std::int32_t max_food = 0;
  /// `extrasentries`, added on top of `InitialStrongholdSentries`.
  std::int32_t extra_sentries = 0;

  /// `exit interval` in the dump: 20 in 324 of 328 cases, 1,000 in four.
  std::int32_t exit_interval = 20;
};

/// Fill the class-derived half of a `SettlementInit` from an anchor class:
/// kind, the four flags, `efficiency`, `foodperpop`, `capture_health_percent`,
/// `maxhealth`, `max_units`, and the six economy numbers the class declares
/// as `population`, `max_population`, `settlement_gold`, `settlement_food`,
/// `settlement_maxgold`, `settlement_maxfood` -- which are what the settlement
/// constructor (0x005c3e60) falls back to when the map's element says -1 for
/// one of them. The map's own numbers, where it has them, are laid over these
/// by the caller. `extra_sentries` is left alone: no class declares one.
///
/// Kind is by ancestry rather than by name, because each race spells its own
/// -- `RStronghold`, `BStronghold`, `MutableStronghold` -- and all of them
/// descend from the one base `gbr.exe` also names. Outposts are tested before
/// the two producers because an outpost is the more specific thing.
void fill_settlement_class_defaults(const ClassGraph& graph, ClassIndex class_index,
                                    SettlementInit& init);

/// One settlement: the fields the dump prints, in the dump's terms, plus the
/// ones it does not print but the scripts read.
///
/// Everything here is world state and everything here is hashed.
struct Settlement {
  // -- identity ---------------------------------------------------------
  ObjectId object = kNoObject;  ///< `handle`
  SettlementId id = kNoSettlement;  ///< `ID`
  ObjectId anchor = kNoObject;      ///< the object whose class the dump prints
  PlayerId owner = kNoPlayer;
  SettlementKind kind = SettlementKind::other;
  /// The map's `<settlement name>`. **Identity, not state**: it comes from the
  /// map and nothing mutates it, so it is saved but deliberately *not* hashed
  /// -- see `SettlementStore::hash`. `gbr.exe` keeps it on the settlement too,
  /// as the `std::string` at `settlement+0xcc`, and both `GetSettlement`
  /// (0x005c3ad0) and `WaitSettlementCapture` (0x005eda80) find a settlement by
  /// scanning the same vector for it.
  std::string name;

  // -- the `capture/attack/gold/food` quad -------------------------------
  //
  // Four integers on one line, seven combinations across 328 blocks. The first
  // two are the `can_be_captured` / `can_be_attacked` class booleans. The last
  // two are **rates, not stock** — stock is the separate `gold/food` line — and
  // they take exactly the values `ProductionGoldBase` and `ProductionFoodBase`
  // declare: `1 1 24 0` on every town hall, `1 1 0 24` on every village,
  // `1 1 0 0` on everything that produces nothing.
  bool can_be_captured = false;
  bool can_be_attacked = false;
  /// Percent of population produced per `ProductionInterval`. Mutable, because
  /// the Food Tax upgrade sets `StrongholdFoodProduction` on a live settlement.
  std::int32_t gold_rate = 0;
  std::int32_t food_rate = 0;

  // -- population --------------------------------------------------------
  std::int32_t population = 0;
  /// `maxpop`. Not a hard ceiling: `TOWNHALL_ADDPOP.VS` adds 10 unconditionally
  /// and the late dumps show `101 100` and `165 150`. It bounds *growth* only.
  std::int32_t max_population = 0;

  // -- the sub-objects ---------------------------------------------------
  Warehouse warehouse;
  Holder holder;

  // -- the rest of the printed block -------------------------------------
  /// A round-robin cursor into `buildings`, advanced once per `RepairInterval`.
  /// The dumps show it taking 11 values in 0..40 and moving over a session,
  /// which is what a cursor does and what a fixed target would not.
  std::int32_t first_to_repair = 0;
  GameTime last_tower_fire_time = 0;
  GameTime last_unit_exit_time = 0;
  /// 0 in all 328 blocks. Reproduced because it is in the sync set; nothing
  /// here writes it.
  GameTime last_capture_query_time = 0;
  std::int32_t exit_interval = 20;
  /// Tavern debt. 0 in all 328 blocks — no dump caught a player mid-loan.
  std::int32_t loan = 0;

  // -- not printed, but read by the scripts ------------------------------
  /// `LoyaltySettlementsInitial` at creation, plus `LoyaltyIncreasePerUnit` for
  /// every unit garrisoned; `DecreaseLoyalty(1)` per capture beat; reset to 11
  /// the moment a settlement changes hands.
  std::int32_t loyalty = 0;
  /// How much loyalty has already been lost inside the current
  /// `LoyaltyCapInterval` window. `LoyaltyChangeCap` is a cap on *decrease* —
  /// "No more than … loyalty percents decreased in LoyaltyCapInterval" — so it
  /// is tracked per window and reset when the window rolls over.
  std::int32_t loyalty_decreased = 0;
  std::int32_t efficiency = 0;
  std::int32_t food_per_pop = 0;
  std::int32_t capture_health_percent = 100;
  /// The anchor's `maxhealth`, copied from the class at creation.
  std::int32_t anchor_max_health = 0;
  /// `Settlement::GetNumSentries` -- `[settlement+0xfc]`, the roster.
  std::int32_t sentries = 0;
  /// `Settlement::GetMaxSentries` -- `[settlement+0x104]`, what the roster
  /// grows towards.
  std::int32_t max_sentries = 0;
  /// **How many of the roster are inside the wall**, `[settlement+0xf4]`, and
  /// the number `GetSentry` and `PutSentry` move.
  ///
  /// A separate count from `sentries` because a sentry that has been spawned as
  /// a unit is still on the roster and no longer in the wall: `GATE_PATROL.VS`
  /// is `if (set.GetSentry()) Place(sentry_class_name, ...)` and
  /// `SENTRY_DISAPPEAR.VS` is `set.PutSentry(); me.Erase()`. `AddSentries`
  /// (0x005c4ac0) raises both, which is why they start equal and diverge only
  /// by what is standing outside.
  ///
  /// The original charges the owner's upkeep off *this* number rather than off
  /// the roster (0x005c4930 keeps a player-wide total beside it), which is not
  /// modelled: this engine has no per-player sentry upkeep to charge.
  std::int32_t sentries_ready = 0;
  /// The settlement this one ships its surplus to, or `kNoSettlement`.
  SettlementId supplied = kNoSettlement;
  OutpostTrade outpost_trade = OutpostTrade::none;
  /// While set, the warehouse loses `GoldBurnAmount`/`FoodBurnAmount` every
  /// `BurnTime`.
  bool burning = false;
  /// The settlement's buildings, in placement order. The repair cursor walks
  /// this; `Buildings()` returns it.
  std::vector<SettlementBuilding> buildings;

  // -- timers ------------------------------------------------------------
  //
  // Game time remaining until each interval next comes due. Counting down
  // rather than up is what makes a turn splittable: `advance` steps to the
  // nearest due timer instead of to the end of the turn, so a 800-unit turn and
  // two 400-unit turns fire the same events in the same order.
  std::array<std::int32_t, kSettlementTimerCount> timers{};

  [[nodiscard]] std::int32_t gold() const noexcept { return warehouse.gold; }
  [[nodiscard]] std::int32_t food() const noexcept { return warehouse.food; }
  [[nodiscard]] bool produces() const noexcept {
    return efficiency > 0 && (gold_rate > 0 || food_rate > 0);
  }
  /// `IsIndependent`: owned by nobody or by one of the two engine-reserved
  /// neutrals. The dumps encode 14 (wildlife and map garrisons) and 15 (a
  /// smaller passive set) as owners like any other player.
  [[nodiscard]] bool independent() const noexcept {
    return owner == kNoPlayer || owner >= kNeutralWildlife;
  }
  [[nodiscard]] bool is_stronghold() const noexcept {
    return kind == SettlementKind::stronghold;
  }
  [[nodiscard]] bool is_village() const noexcept { return kind == SettlementKind::village; }
  [[nodiscard]] bool is_outpost() const noexcept { return kind == SettlementKind::outpost; }
  [[nodiscard]] bool is_shipyard() const noexcept { return kind == SettlementKind::shipyard; }
  [[nodiscard]] bool is_teuton_tent() const noexcept {
    return kind == SettlementKind::teuton_tent;
  }

  [[nodiscard]] std::int32_t timer(SettlementTimer t) const noexcept {
    return timers[static_cast<std::size_t>(t)];
  }
};

/// The settlements of a world, in `ID` order.
///
/// Dense and ascending because the dumps are: `ID` is an array index, so this
/// is an array. Creation appends; nothing removes, because a razed settlement
/// becomes a ruin rather than a hole (the dumps carry `Ruins1` settlements with
/// every field zeroed, still occupying their `ID`).
class SettlementStore {
 public:
  /// Append a settlement and return its `ID`.
  SettlementId create(const SettlementInit& init);
  /// Rebuild the settlement at `id` from `init` as if it had just been created
  /// -- same `ID`, everything else from `init`. False for an unknown `id`.
  ///
  /// For a `Mutable` settlement made into a race's: 0x00551bc0 destroys the
  /// placeholder's record and creates a fresh one, and the fresh record is what
  /// this leaves behind. The `ID` is kept because here it is the index, and the
  /// GAIKA table and the AI's per-settlement rows are keyed on it.
  bool replace(SettlementId id, const SettlementInit& init);

  [[nodiscard]] std::size_t size() const noexcept { return settlements_.size(); }
  [[nodiscard]] bool empty() const noexcept { return settlements_.empty(); }
  [[nodiscard]] std::span<const Settlement> all() const noexcept { return settlements_; }
  [[nodiscard]] std::span<Settlement> all() noexcept { return settlements_; }

  [[nodiscard]] const Settlement* find(SettlementId id) const noexcept;
  [[nodiscard]] Settlement* find(SettlementId id) noexcept;

  /// The settlement whose settlement, holder, warehouse or anchor handle is
  /// `object`. This is `Building::settlement`, the most-called member in the
  /// whole host API at 432 sites.
  ///
  /// `World` keeps the same link on `WorldObject::settlement` and reaches it in
  /// constant time; this is the answer for a store with no world behind it, and
  /// the fallback when that link has not been written.
  [[nodiscard]] const Settlement* for_object(ObjectId object) const noexcept;
  [[nodiscard]] Settlement* for_object(ObjectId object) noexcept;

  /// The settlement whose map name is `name`, or null.
  ///
  /// A linear scan in `ID` order, which is what the original does: 0x005c3b17
  /// (`GetSettlement`) and 0x005edb03 (`WaitSettlementCapture`) both walk the
  /// settlement vector comparing the name in place. Byte-exact and therefore
  /// case-sensitive; an empty `name` never matches, so the 496 unnamed
  /// settlements are unreachable this way rather than all equal.
  [[nodiscard]] const Settlement* find_by_name(std::string_view name) const noexcept;

  /// Bind another of the settlement's buildings to it, so that
  /// `Building::settlement` resolves and the repair cursor can reach it.
  bool add_building(SettlementId id, ObjectId building, std::int32_t max_health);

  /// FNV-1a over every settlement in `ID` order. Folded into the world hash by
  /// `EconomySystem::hash`.
  [[nodiscard]] std::uint64_t hash() const noexcept;

  // -- the saved game ----------------------------------------------------

  /// Append the whole store to `out`, with its own magic and version.
  ///
  /// A pair rather than a rebuild through `create`, because `create` derives an
  /// id and a starting state from a `SettlementInit` and a saved settlement is
  /// neither: it is a row that has been running for an hour, timers and loan
  /// and loyalty and garrison included. Replaying the constructor and then
  /// overwriting every field through `all()` would be the same bytes reached by
  /// a longer route that a future change to `create` could quietly break.
  ///
  /// Definitions in `src/sim/save_systems.cpp`, next to the systems that own
  /// this store. Layout: docs/formats/save.md.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace it with the one in `bytes`. **Atomic**: decoded into a local and
  /// moved in only once every row has read cleanly.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  std::vector<Settlement> settlements_;
};

}  // namespace imperivm::core::sim
