#pragma once

/// Combat: damage resolution, target acquisition, death, and ballistics.
///
/// **Everything here that is derived from shipped data cites the file it came
/// from. Everything that is not is marked INVENTED or INFERRED, in those
/// words.** Combat is the easiest system in an RTS to fake convincingly,
/// because any arithmetic at all produces numbers that look like a fight, so
/// the provenance of each term matters more than the completeness of the model.
///
/// ## Where the numbers come from
///
/// The stat set is **not** the conventional attack/armour/health triple. What
/// the 845-class graph actually declares on a unit (`RHastatus`, resolved) is:
///
///     damage = 16          damage_type = slash
///     armor_slash = 12     armor_pierce = 12
///     maxhealth = 200      maxstamina = 10
///     range = 17           min_range = 2
///     radius = 15          selection_radius = 15      sight = 500
///     target_factor = 100  unit_specials = Deflection
///
/// So the data declares two armour channels and three damage types plus a
/// null one: `damage_type` takes `slash` (the `Unit` default), `pierce` (the
/// `Ranged` default), `siege` (catapults and rams) and `none` (peasants,
/// wagons, wildlife). `armor_slash` and `armor_pierce` are declared on 102
/// classes each and are equal in every one of them.
///
/// **The blow reads neither the damage type nor the pierce channel.** The
/// calculator (0x0050ff30) takes the slash armour `[obj+0xe4]` off every
/// blow, a sword's and an arrow's alike; the damage type decides only what a
/// unit may attack (`attack_virtual`, clause 12), and a type-`none` class is
/// refused every target there rather than dealt a blow of 0.
///
/// ## The blow
///
/// `DATA\CONST.INI` `[GamePlay]` supplies the arithmetic constants:
///
///     MinPercentOfAttackersDamage = 20
///     MinDamage = 3
///     LevelDifferenceDamageTable = 1,5 5,20 10,40 20,60 40,80 100,90
///     ChargeDamageFactor = 8
///
/// The dealer 0x00511550 takes a training blow between units that are not
/// enemies (`CombatSystem::training_blow`), and otherwise calls the
/// calculator 0x0050ff30, which `calculate_blow` reproduces whole, and then
/// applies what it answers (`CombatSystem::deal`). The counter
/// table (`DATA\COUNTERUNITS.XML`) is not in the blow: its readers are the
/// AI's and a catapult's ground shot (0x004da370). Nor is `DamageAmplify`,
/// whose one reader is a building's damage virtual sharing a blow with its
/// garrison (0x004db950 -> 0x005c2d00).
///
/// ### `FullArmor = 8` is dead
///
/// It is the obvious candidate for a percentage-of-armour rule and it is not
/// used: the string `FullArmor` does not occur anywhere in `gbr.exe`, while
/// `MinDamage`, `MinPercentOfAttackersDamage`, `LevelDifferenceDamageTable`,
/// `DamageAmplify` and `ChargeDamageFactor` each occur exactly once. A model
/// built on it would be building on a constant the shipped engine never reads.
/// Armour is subtractive and floored by `MinPercentOfAttackersDamage`
/// (0x00510d86, 0x00510df9).
///
/// ## Morale: there is none
///
/// Asked for, and the data refuses it. `gbr.exe` contains no occurrence of
/// `morale`, `flee`, `panic`, `rout` or `courage`; `CONST.INI` declares no
/// morale constant; no class in the graph declares a morale property; and the
/// desync dumps' unit block (docs/engine/state-vector.md) has no morale field.
/// The one field that looks like it — `danger`, 0..6 — is not morale either:
/// it counts how many target searches have recently chosen the unit, decays
/// a point a second, and makes the next search 40 a point less keen on it
/// (`Combatant::danger`, `docs/formats/target-score.md`). **Imperivm units
/// fight to the death and this file models that.**
///
/// ## What is deliberately absent
///
/// The attack *cadence* of a melee unit is not in `CONST.INI` and not in the
/// class graph: only `Catapult` (500) and `FakeTower` (400) declare
/// `attack_delay`. The original takes it from the attack animation — the host
/// API exposes `TimeToActionMoment`, and the dumps show `Anim=4` on
/// `engage`/`attack` — so `CombatProfile::from_class` reads the entity's slot-5
/// attack animation when one is given. With neither, it falls back to
/// `kDefaultAttackInterval`, which is **INVENTED**.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/process.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;
struct WorldObject;

/// Whether a world object belongs in `CombatSystem`.
///
/// **The line is `is_unit || is_building`**, which `flags_for_native_class`
/// derives from the native class hierarchy. Three independent readings put it
/// in the same place:
///
///   1. **`gbr.exe` gives the other families no health field at all.**
///      `Obj::health` is registered at `0x005b714c` and is `0x005adc60`, whose
///      whole body is `return *(int*)(obj + 0xE8)` (`0x005adca3`) -- a plain
///      field read, no virtual. The object factory at `0x0059dd79` allocates
///      `CVXDecor` and `CVXMapObj` with **`new(0x78)`** (`0x0059e168`,
///      `0x0059e24b`) and `CVXFeedback` with `new(0x8c)`; all three are smaller
///      than `0xE8`, so those objects physically cannot carry a health. Every
///      other instantiable class the same factory builds is `0x150` or larger.
///   2. **Only a unit can attack.** `Obj::IsValidTarget` (registered at
///      `0x005b76d7`, wrapper `0x005aafd0`) forwards to virtual slot `+0xB4`,
///      which is `0x005b1da0` for both the `CVXUnit` vtable (`0x007d0af8`) and
///      the `CVXBuilding` vtable (`0x007ba488`). That function loads
///      `0x00400000` -- `SyncFlags` bit 22, "is a unit" -- into `edi` at
///      `0x005b1e05` and tests it against the **attacker's** flag word at
///      `0x005b1e10`; an attacker without it takes the `xor ecx, ecx` path at
///      `0x005b1edd`. The candidate is tested for the same bit at `0x005b1e6f`
///      and a candidate without it simply skips the unit-only clauses, which is
///      how a building stays targetable.
///   3. The class graph declares `target_priority` on `Building` (100),
///      `FakeTower` (150) and `Gate` (200) and on nothing else, which is only
///      meaningful if buildings are targets.
///
/// Internal objects (settlements, holders, warehouses, queries) and objects
/// whose class did not resolve are excluded: the first have no class and no
/// position of their own, the second have no profile to fight with.
///
/// **Not excluded here:** `SyncFlags` bit 26, `Obj::SetMessengerStatus`'s
/// (`ObjectFlags::messenger`). The `CanAttack` virtual rejects a candidate
/// *or* an attacker carrying `0x04000000` outright (`0x005b1f38` and
/// `0x005b1f4b`), so a messenger is in the fight's roster and neither strikes
/// nor is struck (`CombatSystem::attack_virtual`).
[[nodiscard]] bool is_combat_object(const WorldObject& slot) noexcept;

// ---------------------------------------------------------------------------
// damage types
// ---------------------------------------------------------------------------

/// The four values `damage_type` takes across the shipped class graph.
///
/// `slash` is `Unit`'s default and `pierce` is `Ranged`'s; `siege` is declared
/// by the seven catapult classes and `RamUnit`; `none` by `Peaceful`, `Crow`,
/// `Deer`, `Eagle`, `Fish`, `Hen`, `Peasant`, `Wagon`.
enum class DamageType : std::uint8_t {
  none = 0,
  slash,
  pierce,
  /// Meets no armour: no `armor_siege` property exists on any of the 845
  /// classes, and the two channels that do exist are named for the other two
  /// types.
  siege,
};

[[nodiscard]] DamageType damage_type_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view damage_type_name(DamageType type) noexcept;

// ---------------------------------------------------------------------------
// the action enum and its animations
// ---------------------------------------------------------------------------

/// `action=` from the desync dumps. **The names are ours; the numbers are the
/// data's** (docs/engine/state-vector.md: "`action` is a small enum but its
/// exact names are unknown; 1 and 7 never occur"). Only the four values combat
/// can put an object into are declared here.
enum class Action : std::uint8_t {
  /// 4,473 blocks, co-occurring with `idle`, `guard`, `move`, `engage`.
  idle = 0,
  /// 92 blocks, co-occurring with `engage` (90) and `attack` (2).
  engaging = 2,
  /// 1,299 blocks; lines up with `SyncFlags` bit 17. Combat never sets it —
  /// movement owns it — but a combatant can arrive carrying it.
  moving = 3,
  /// 15 blocks, all with an **empty** method, `health` 0-3 and `Anim=8`. This
  /// is the dying state, and it is a state rather than an instant: the object
  /// is still in the world with the death animation running.
  dying = 8,
};

/// `Anim=` values the dumps pair with combat states. 4 accompanies
/// `engage`/`attack`, 8 accompanies `action=8`, 28 is every idle and static
/// object. These are animation *indices as printed*, not entity slot numbers —
/// the entity slot for an attack is `kAnimAttack = 5` (game/entity.hpp).
inline constexpr std::int32_t kAnimEngage = 4;
inline constexpr std::int32_t kAnimDying = 8;
inline constexpr std::int32_t kAnimIdle = 28;

// ---------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------

/// `LevelDifferenceDamageTable = 1,5 5,20 10,40 20,60 40,80 100,90`, as the
/// original builds it (0x0050fd40) and applies it (0x0050fe60).
///
/// The string is read as `difference,percent` pairs (a C `%i`, so a leading
/// `0` is octal and `0x` hex). From the point `(0, 0)`, each pair `(x1, p1)`
/// fills the entries `x0 .. x1 - 1` with
/// `256 + ((p1 - p0) * k + span * p0) * 256 / (span * 100)`, `k` the step from
/// `x0` and `span = x1 - x0`, one signed division toward zero: the percent
/// interpolated in 256ths in one step. A pair that does not read is `(100,
/// p0)`. The walk stops at the first `x1` of 100 or more, and entry 100 is
/// `256 + p * 256 / 100` of the last percent read. So the shipped table gives
/// 256 at 0, 268 at 1, 288 at 3 and 486 at 100.
///
/// **Labelled:** a breakpoint past 100 writes past the original's 101 entries
/// (into the flag that says the table is built); here the walk stops at 100.
struct LevelDifferenceTable {
  static constexpr std::int32_t kSteps = 101;

  /// The factor in 256ths, by magnitude of the difference, 0 to 100.
  std::array<std::int32_t, kSteps> factors{};

  /// 0x0050fe60: the factor for a signed difference -- its magnitude kept to
  /// 100, and `512 - factor` when the difference is negative.
  [[nodiscard]] std::int32_t factor(std::int32_t difference) const noexcept;

  /// `factor(difference) * value / 256`, signed, toward zero, in 32 bits.
  [[nodiscard]] std::int32_t scale(std::int32_t value, std::int32_t difference) const noexcept;

  /// The retail table, compiled in.
  [[nodiscard]] static LevelDifferenceTable shipped() noexcept;

  /// Build from the `CONST.INI` spelling, as 0x0050fd40 does.
  [[nodiscard]] static LevelDifferenceTable parse(std::string_view text) noexcept;

  friend bool operator==(const LevelDifferenceTable&, const LevelDifferenceTable&) = default;
};

/// The three difficulty settings `CONST.INI` names.
enum class Difficulty : std::uint8_t { easy, normal, hard };

/// `[GamePlay]`, verbatim. Every field names the key it reads.
///
/// `from_const_ini` parses the real file; `shipped()` is the same values
/// compiled in so that a synthetic test needs no game data.
struct CombatConstants {
  // -- damage ---------------------------------------------------------
  std::int32_t min_damage = 3;                        ///< `MinDamage`
  std::int32_t min_percent_of_attackers_damage = 20;  ///< `MinPercentOfAttackersDamage`
  std::int32_t damage_amplify = 200;                  ///< `DamageAmplify` (in settlements)
  std::int32_t charge_damage_factor = 8;              ///< `ChargeDamageFactor`
  LevelDifferenceTable level_difference = LevelDifferenceTable::shipped();

  // -- difficulty -----------------------------------------------------
  //
  // `;difficulty level adjustments` is the comment above them. The calculator
  // reads one of the three by the game's difficulty into its match cache and
  // gives it to one player, the favoured one: added to that player's
  // attacker's level, taken off the difference when that player defends
  // (`CombatSystem::DamageFormulaParams`, `docs/formats/blow.md`).
  std::int32_t easy_level_addend = 10;    ///< `EasyDifficultyLevelAddend`
  std::int32_t normal_level_addend = 5;   ///< `NormalDifficultyLevelAddend`
  std::int32_t hard_level_addend = 0;     ///< `HardDifficultyLevelAddend`

  // -- experience -----------------------------------------------------
  //
  // `CONST.INI` gives both of these as comments containing the formula, which
  // is as close to a specification as this data gets:
  //
  //     LevelExpDivider = 8 ; exp_gain = target_level / LevelExpDivider + 1;
  //     ExpA=50  ExpB=125  ExpT=1
  //     ; Exp[level] = Exp[Level-1] + Pos(level - T) * ExpA + ExpB
  //     ; Where Pos(x) is (X > 0 ? X : 0)
  //
  // **The comment's formula is dead in the executable.** The unit constructor
  // (0x005d3170) reads `LevelExpDivider` into a global nothing reads again;
  // what a kill pays is `CombatSystem::kill_gain`'s rule (0x00511550), and a
  // plain blow pays nothing (`docs/formats/unit-stats.md`, "What a blow
  // pays"). The two `experience_gain` overloads below are the comment's
  // formula, kept for its own tests and read by nothing in the simulation.
  std::int32_t level_exp_divider = 8;  ///< `LevelExpDivider`
  std::int32_t exp_a = 50;             ///< `ExpA`
  std::int32_t exp_b = 125;            ///< `ExpB`
  std::int32_t exp_t = 1;              ///< `ExpT`
  /// `BattleTacticsExpModifier`, a percentage the *Battle tactics* research
  /// hands `SetExperienceModifier(player, c)` through `GetConst`. The
  /// executable never reads it; a player nobody has set has no modifier.
  std::int32_t experience_modifier = 100;

  // -- catapults ------------------------------------------------------
  std::int32_t catapult_base_fire_rate = 40;  ///< `CatapultBaseFireRate`, shots per 10 min
  std::int32_t catapult_add_fire_rate = 28;   ///< `CatapultAddFireRate`, shots per 10 min
  std::int32_t catapult_max_units = 10;       ///< `CatapultMaxUnits`
  /// `BaseFireRate` (100): a tower settlement's shots a second per square
  /// root of its garrison, in percent (0x005c2fb0, the global 0x009bf7b0).
  std::int32_t base_fire_rate = 100;

  /// `TrainingExpChance` (75): the training blow's chance in a hundred of a
  /// point of experience (0x00511658, the global 0x00996e34).
  std::int32_t training_exp_chance = 75;

  // -- the calculator's specials ----------------------------------------
  //
  // The rest of the block the calculator reads once per process (0x0050ff30,
  // guarded by `AvoidChancePercentPerConcealmentLevel`'s global still at -1).
  // There is no `RevengeCost`, `FerocityCost`, `DeterminationCost` or
  // `CombatSkillCost`: those four are free.
  std::int32_t avoid_chance_percent_per_concealment_level = 2;  ///< `AvoidChancePercentPerConcealmentLevel`
  std::int32_t percent_per_euphoria_level = 10;  ///< `PercentPerEuphoriaLevel`
  std::int32_t euphoria_stamina = 3;             ///< `EuphoriaStamina`
  std::int32_t charge_time = 5000;               ///< `ChargeTime`, ms since the last attack
  std::int32_t offensive_tactics_cost = 1;       ///< `OffensiveTacticsCost`
  std::int32_t defensive_tactics_cost = 1;       ///< `DefensiveTacticsCost`
  std::int32_t triple_strike_cost = 1;           ///< `TrippleStrikeCost` (sic)
  std::int32_t deflection_cost = 1;              ///< `DeflectionCost`
  std::int32_t penetration_cost = 1;             ///< `PenetrationCost`
  std::int32_t attack_skill_cost = 1;            ///< `AttackSkillCost`
  std::int32_t defense_skill_cost = 1;           ///< `DefenseSkillCost`
  std::int32_t death_blow_cost = 4;              ///< `DeathBlowCost`
  std::int32_t power_strike_cost = 4;            ///< `PowerStrikeCost`
  std::int32_t expertise_cost = 4;               ///< `ExpertiseCost`
  std::int32_t rage_cost = 4;                    ///< `RageCost`
  std::int32_t bleeding_attack_cost = 1;         ///< `BleedingAttackCost`
  std::int32_t life_steal_cost = 1;              ///< `LifeStealCost`
  std::int32_t drain_cost = 1;                   ///< `DrainCost`
  std::int32_t crippling_attack_cost = 1;        ///< `CripplingAttackCost`
  std::int32_t spike_armor_cost = 1;             ///< `SpikeArmorCost`
  std::int32_t charge_cost = 4;                  ///< `ChargeCost`
  std::int32_t triumph_cost = 3;                 ///< `TriumphCost`
  std::int32_t learning_cost = 1;                ///< `LearningCost`
  std::int32_t disease_cost = 1;                 ///< `DiseaseCost`
  std::int32_t archer_against_hero_chance = 30;  ///< `ArcherAgainstHeroChance`

  // -- pacing ---------------------------------------------------------
  /// How often a unit with nothing to hit looks for something. `SquadLookIvl`,
  /// the AI's own look interval, is 1000 and is the only cadence of this shape
  /// in the file; using it here is **INFERRED**.
  std::int32_t acquire_interval = 1000;

  [[nodiscard]] static CombatConstants shipped() noexcept { return CombatConstants{}; }

  /// Parse `DATA\CONST.INI`. Unknown keys and other sections are ignored;
  /// keys that are absent keep their shipped value, so a truncated file
  /// degrades to the compiled-in balance rather than to zeroes.
  [[nodiscard]] static CombatConstants from_const_ini(std::span<const std::byte> text) noexcept;

  [[nodiscard]] std::int32_t difficulty_addend(Difficulty level) const noexcept;

  /// `Exp[level]` as the recurrence reads **without** the original's division
  /// by 100 -- kept for its own tests, and read by nothing in the simulation:
  /// a unit levels by `HeroSystem::level_for_experience` (0x005d28c0), whose
  /// table is the executable's. Saturates rather than overflowing.
  [[nodiscard]] std::int32_t experience_for_level(std::int32_t level) const noexcept;
  /// The same, with an explicit modifier percentage in place of the global
  /// `BattleTacticsExpModifier`. `SetExperienceModifier(player, c)` makes that
  /// number **per player**, so the constant is only the default.
  [[nodiscard]] std::int32_t experience_gain(std::int32_t target_level,
                                             std::int32_t percent) const noexcept;

  /// The highest level whose threshold `experience` has reached, at least 1.
  [[nodiscard]] std::int32_t level_for_experience(std::int32_t experience) const noexcept;
  /// `exp_gain = target_level / LevelExpDivider + 1`, scaled by
  /// `experience_modifier` percent.
  [[nodiscard]] std::int32_t experience_gain(std::int32_t target_level) const noexcept;
};

// ---------------------------------------------------------------------------
// the counter-unit table
// ---------------------------------------------------------------------------

/// How `DATA\COUNTERUNITS.XML` is applied to a damage calculation.
///
/// **No blow reads it** (0x0050ff30, `docs/formats/blow.md`). Its one
/// consumer in this file is a catapult's ground shot (`land_on_ground`,
/// 0x004da370), and the mode is that shot's alone. What follows is the
/// measurement of the table that motivated the two readings.
///
/// The table is 52 unit classes and 1,023 directed entries of the shape
///
///     <unit class="RHastatus"> <counter class="GSwordsman" coeff="33"/> ... </unit>
///
/// with `coeff` in [31, 100]. What is *measured* about it:
///
///   * **It is a genuine rock-paper-scissors graph, not a hierarchy.** Reading
///     each entry as a directed edge, 50 of the 52 classes fall into a single
///     strongly connected component and there are 535 distinct three-cycles.
///     Whichever way round the edges point, the data really does describe a
///     combat triangle; nothing in this file has to manufacture one.
///   * The relation is almost perfectly antisymmetric: of 1,023 entries only
///     six have a mirror, and those six pair up as three couples summing to
///     90, 107 and 107.
///   * No coefficient is below 31 and none is above 100; 15 sit exactly at 100.
///   * A coefficient is a function of the *listed* class and barely of the
///     enclosing one: it correlates +0.41 with the listed class's log health
///     and +0.28 with its armour, while the enclosing class's health and armour
///     correlate -0.03 and -0.09. Big, well-armoured classes are the ones that
///     appear with high coefficients.
///   * **Its only demonstrable consumer in the shipped game is the AI.** The
///     host function `GetCounterUnits(Settlement, IntArray)` fills a
///     six-element weight vector that `DATA\AI\ESH_COUNTERUNITS.VS` uses to
///     pick which unit type to train. `gbr.exe`'s string table mentions
///     `data/CounterUnits.xml` exactly once, next to `GetCounterUnits`, and no
///     `.vs` script in `data.pak` connects the table to damage.
///
/// **So applying it to damage at all is an inference, and its orientation is
/// undecided by the data available here.** Two readings survive:
///
///   * `counter_bonus` — the listed class counters the enclosing one, so its
///     damage against it is scaled by `(100 + coeff) / 100`. This is what the
///     element names say (`<counter>` inside `<unit>`, in a file called
///     COUNTERUNITS) and it explains the floor at 31 as a pruning threshold:
///     a hand-authored 0-100 strength score whose weak entries were not worth
///     a row. It has to accept that the game's heaviest units -- the war
///     elephant counters the Hastatus at 94 -- get the largest bonuses.
///   * `countered_penalty` — the enclosing class's damage against a listed one
///     is scaled to `coeff / 100`. This explains the correlation as
///     compensation for subtractive armour, since the fragile classes are the
///     ones that receive the deepest cuts, and it makes a 94 a near-no-op. It
///     has to accept that 123 entries at 90 or above were recorded while every
///     entry below 31 was not.
///
/// Both are implemented, neither is asserted, and `off` is always available for
/// an answer that rests on decoded data alone. The choice is not cosmetic:
/// across the 1,326 pairs of the 52 classes, 163 change winner when the table
/// is applied at all, and the two readings disagree about many of them.
enum class CounterMode : std::uint8_t {
  /// The table does not touch damage. Use this when the answer has to be
  /// defensible from decoded data alone; the table is still loaded and still
  /// answers `GetCounterUnits`-shaped queries.
  off,
  /// The default, chosen on the element names and the pruning threshold and
  /// not on anything stronger: a counter unit's damage against the class it
  /// counters is scaled by `(100 + coeff) / 100`, up to double.
  counter_bonus,
  /// The other reading: the *enclosing* class's damage against a listed class
  /// is scaled by `coeff / 100`, a penalty of up to 69%. Equally implemented
  /// and equally unproven; see above.
  countered_penalty,
};

/// `DATA\COUNTERUNITS.XML`, resolved against the class graph.
///
/// Entries are held sorted by `(counter, countered)` and looked up by binary
/// search: iteration order is world state (docs/engine/architecture.md) and a
/// hash map would make it depend on the allocator.
class CounterTable {
 public:
  /// Parse the document and resolve both class names through `graph`.
  /// Unresolvable names are counted in `unresolved()` and dropped, in the
  /// spirit of the class graph's own tolerant loading: the retail data has 38
  /// dangling references and the retail engine ships with them.
  Result<void> load(std::span<const std::byte> document, const ClassGraph& graph);

  /// Add one relation directly, for tests that have no game data.
  /// `counter` counters `countered` with strength `coeff`.
  void add(ClassIndex counter, ClassIndex countered, std::int32_t coeff);

  /// The strength with which `counter` counters `countered`, or 0 when the
  /// pair is not in the table. Exact classes only: the table names concrete
  /// unit classes and inheritance would make `Melee` counter everything.
  [[nodiscard]] std::int32_t strength(ClassIndex counter, ClassIndex countered) const noexcept;

  /// The multiplier `mode` puts on a damage calculation, as a percentage of
  /// normal. 100 when the mode is `off` or the pair is absent.
  [[nodiscard]] std::int32_t damage_percent(ClassIndex attacker, ClassIndex defender,
                                            CounterMode mode) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t unresolved() const noexcept { return unresolved_; }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  void clear() noexcept;

  /// One `<counter class=counter coeff/>` under `<unit class=countered>`.
  struct Entry {
    ClassIndex counter = kNoClass;
    ClassIndex countered = kNoClass;
    std::int32_t coeff = 0;
  };
  /// Every entry, sorted by `(counter, countered)`. What `GetCounterUnits`
  /// walks: the table's rows are its countered classes, and the counters of
  /// one row are the entries that name it.
  [[nodiscard]] std::span<const Entry> entries() const noexcept { return entries_; }
  /// The distinct countered classes -- the `<unit>` rows -- in ascending
  /// class index, which is the only order this table can promise.
  [[nodiscard]] std::vector<ClassIndex> countered_classes() const;

 private:
  /// Sorted by `(counter, countered)`; `add` inserts in place.
  std::vector<Entry> entries_;
  std::size_t unresolved_ = 0;
};

// ---------------------------------------------------------------------------
// damage resolution
// ---------------------------------------------------------------------------

/// The specials the blow reads: bits of the low word of the unit's specials
/// (`[unit+0x198]`, the 36-name table `UNIT_SPECIALS.INI` orders, which
/// `CombatProfile::specials` reads off the class). The high word -- Healing,
/// Keen sight, Trample, Curse -- is loaded by the calculator and never tested.
namespace special {
inline constexpr std::uint32_t kParry = 1u << 0;
inline constexpr std::uint32_t kDrain = 1u << 1;
inline constexpr std::uint32_t kFerocity = 1u << 2;
inline constexpr std::uint32_t kTripleStrike = 1u << 3;
inline constexpr std::uint32_t kDeflection = 1u << 4;
inline constexpr std::uint32_t kLifeSteal = 1u << 6;
inline constexpr std::uint32_t kDetermination = 1u << 7;
inline constexpr std::uint32_t kCombatSkill = 1u << 8;
inline constexpr std::uint32_t kOffensiveTactics = 1u << 10;
inline constexpr std::uint32_t kDefensiveTactics = 1u << 11;
inline constexpr std::uint32_t kPenetration = 1u << 12;
inline constexpr std::uint32_t kSpikeArmor = 1u << 13;
inline constexpr std::uint32_t kBleedingAttack = 1u << 14;
inline constexpr std::uint32_t kAttackSkill = 1u << 15;
inline constexpr std::uint32_t kDefenseSkill = 1u << 16;
inline constexpr std::uint32_t kDeathBlow = 1u << 17;
inline constexpr std::uint32_t kPowerStrike = 1u << 18;
inline constexpr std::uint32_t kExpertise = 1u << 19;
inline constexpr std::uint32_t kCharge = 1u << 20;
inline constexpr std::uint32_t kRage = 1u << 22;
inline constexpr std::uint32_t kRevenge = 1u << 23;
inline constexpr std::uint32_t kTriumph = 1u << 24;
inline constexpr std::uint32_t kDiseaseAttack = 1u << 25;
inline constexpr std::uint32_t kLearning = 1u << 28;
inline constexpr std::uint32_t kCripple = 1u << 31;
}  // namespace special

/// The feedback ids the calculator lists for the dealer to spawn on either
/// side (the factory 0x00518650's table at 0x00821180). **Only listed here:**
/// each spawn is a `CVXFeedback` object, which takes its handle from the
/// original's **asynchronous** free list (40,000 and up, 0x0067b480) and runs
/// on the thread manager that has no checksum word -- so it moves neither
/// the handles units take nor the hash, and this engine, which has one handle
/// space, allocates nothing for it (`CreateFeedback` stores nothing either;
/// docs/formats/blow.md, the effect ids). The list is what a test reads to
/// see which special fired.
enum class BlowEffect : std::uint8_t {
  heal = 0,
  experience = 2,
  death = 3,
  deflection = 5,
  stamina_boost = 6,
  stamina_loss = 7,
  spiked_armor = 8,
  bleeding_attack = 9,
  rage = 10,
  revenge = 11,
  offensive_tactics = 12,
  charge = 13,
  penetration = 14,
  triple_strike = 15,
  attack_skill = 21,  ///< drawn as `OffensiveTactics` too
  defense_skill = 22, ///< `AdditionalDefense`
  defensive_tactics = 23,  ///< `AdditionalDefense`
};

/// One side's list. Not capped at the dealer's eight slots: with every
/// special at once the defender's list holds eleven, which in the original
/// runs into the stack beside it.
struct BlowEffects {
  std::array<BlowEffect, 16> ids{};
  std::uint8_t count = 0;
  void push(BlowEffect id) noexcept {
    if (count < ids.size()) ids[count++] = id;
  }
  [[nodiscard]] bool has(BlowEffect id) const noexcept {
    for (std::uint8_t i = 0; i < count; ++i) {
      if (ids[i] == id) return true;
    }
    return false;
  }
};

/// One side of a blow as the calculator 0x0050ff30 reads it off its object.
/// Flat, so that a test can state a case without a world; `CombatSystem`
/// fills it from the world for every real blow.
struct BlowSide {
  /// The unit bit (`[obj+0x2c] & 0x400000`): A-unit / D-unit.
  bool unit = true;
  /// The hero bit.
  bool hero = false;
  /// IsRanged (0x0053f5b0): a `projectile_class` given.
  bool shoots = false;
  /// Heir of `RamUnit` (0x00540460), of `Sentry` (0x005400c0).
  bool ram = false;
  bool sentry = false;
  std::int32_t health = 0;       ///< `[obj+0xc0]`
  std::int32_t stamina = 0;      ///< `[obj+0xc4]`
  std::int32_t max_health = 0;   ///< `[obj+0xc8]`
  std::int32_t max_stamina = 0;  ///< `[obj+0xcc]`
  std::int32_t attack = 0;       ///< `[obj+0xdc]`
  /// `[obj+0xe4]`, the slash armour: **the one armour the blow reads**, for a
  /// sword and an arrow alike (nothing in it reads `[obj+0xe8]`).
  std::int32_t armour = 0;
  std::int32_t level = 1;        ///< the stored level, `vtbl+0x114`
  std::int32_t experience = 0;   ///< `[unit+0x180]`, the running total
  std::int32_t class_damage = 0; ///< the class's `damage`, `[class+0x2a0]`
  std::uint32_t specials = 0;    ///< `special::k*`
  bool parrying = false;         ///< parry mode, `[unit+0x1a0]`
  bool cursed = false;           ///< bit 27 of `[unit+0x194]`
  bool blessed = false;          ///< bit 0
  bool diseased = false;         ///< bit 1
  bool sheltered = false;        ///< bit 28, `ObjectFlags::half_damage`
  /// The timed skills of this side's hero (itself, or the hero it follows):
  /// `active(X, Ceasefire)`, `active(X, Assault)` (0x005d37f0).
  bool ceasefire = false;
  bool assault = false;
  /// `points(X, s)` (0x005d37b0): -1 with no hero, or one not offered it.
  std::int32_t concealment = -1;
  std::int32_t euphoria = -1;
  GameTime last_attack_time = 0;  ///< `[unit+0x1a4]`
  /// What the match cache holds for this side's owner.
  bool favoured = false;
  bool attack_focus = false;
  bool learning = false;
  bool ferocity = false;
  std::int32_t gamblers_luck = 0;
  std::int32_t gamblers_test = 0;
};

/// Where the calculator left the blow: which gate returned, or a hit.
enum class BlowPath : std::uint8_t {
  ceasefire,   ///< C3, C4: either side under its hero's Ceasefire
  dodged,      ///< C5: the Concealment draw
  cursed,      ///< C6: a cursed attacker's blow, which lifts the curse
  parrying,    ///< C7: an attacker in parry mode
  not_a_unit,  ///< C9: a defender that is not a unit
  ram,         ///< C10: a `RamUnit` defender
  parried,     ///< C12: a defender in parry mode with stamina
  deflected,   ///< C13
  missed,      ///< C14, C15: a shot that misses
  struck,      ///< C16..C47
};

/// What the calculator hands the dealer (its out-parameters; the table in
/// `docs/formats/blow.md`, Part 3), and what it did to the world itself.
struct BlowOutcome {
  BlowPath path = BlowPath::struck;
  std::int32_t attacker_health = 0;   ///< p3, a delta
  std::int32_t defender_health = 0;   ///< p4, a delta: minus the damage
  std::int32_t attacker_stamina = 0;  ///< p5, a delta
  std::int32_t defender_stamina = 0;  ///< p6, a delta
  std::int32_t learning = 0;          ///< p7: Learning's point
  BlowEffects on_attacker;            ///< p9
  BlowEffects on_defender;            ///< p10
  /// 0x005d4800 ran on the attacker: its blessing, else its curse, lifted.
  bool cure_attacker = false;
  /// Disease attack set the defender's disease bit (C41).
  bool disease_defender = false;
  /// The working of a hit, for a test: whether armour was met (C20), and
  /// the difference the factor was taken at (C22).
  bool armour_met = false;
  std::int32_t level_difference = 0;

  /// The damage: what the defender's health loses.
  [[nodiscard]] std::int32_t damage() const noexcept { return -defender_health; }
};

/// The rest of what the calculator reads: the match cache's addend, the
/// bloodlust byte and the game clock.
struct BlowRules {
  std::int32_t addend = 0;
  bool bloodlust = false;
  GameTime now = 0;
};

/// The calculator 0x0050ff30, whole, in the binary's order: the gates C1..C12,
/// the two other branches C9 and C10, Deflection C13, a shot's miss C14..C15,
/// and the hit C16..C47 with every special and its stamina cost, the clamps
/// and the deltas (`docs/formats/blow.md`, Part 2). Every draw is on `rng`.
[[nodiscard]] BlowOutcome calculate_blow(const BlowSide& attacker, const BlowSide& defender,
                                         const BlowRules& rules, const CombatConstants& constants,
                                         Rng& rng) noexcept;

// ---------------------------------------------------------------------------
// per-class profile
// ---------------------------------------------------------------------------

/// INVENTED. Nothing in `CONST.INI` or the class graph gives a melee unit's
/// attack cadence; the original reads it off the attack animation. Used only
/// when `from_class` is given neither an entity with a slot-5 animation nor an
/// `attack_delay` property.
inline constexpr std::int32_t kDefaultAttackInterval = 1000;

/// The combat-relevant half of a class, read once and shared by every instance.
struct CombatProfile {
  ClassIndex class_index = kNoClass;

  std::int32_t damage = 0;
  DamageType damage_type = DamageType::slash;
  std::int32_t armor_slash = 0;
  std::int32_t armor_pierce = 0;
  std::int32_t max_health = 0;
  std::int32_t max_stamina = 0;

  /// `range`: 17 on melee classes, 300 on javelins, 500 on archers, 700-800 on
  /// catapults. `min_range`: 2 on units, 201-301 on catapults, which really is
  /// a dead zone a catapult cannot shoot into.
  std::int32_t range = 0;
  std::int32_t min_range = 0;
  std::int32_t radius = 0;
  std::int32_t selection_radius = 0;
  std::int32_t sight = 0;

  /// `target_factor`. **Higher is less attractive**, which the class graph
  /// proves on itself: `Peaceful` (peasants, wagons) sets 1000 and its own
  /// child `BaseMage` — the druid/priest/enchantress line, the thing you most
  /// want dead — overrides it back down to 100. `Hero` and both ghost classes
  /// sit at 200, `Object` at 100, and the six wildlife classes at -1, which is
  /// "never a target".
  std::int32_t target_factor = 100;
  /// `target_priority`, declared only on `Building` (100), `FakeTower` (150)
  /// and `Gate` (200). **Higher is more attractive**: a gate is the thing an
  /// army has to break. Defaults to 100 so that units and buildings score on
  /// the same scale.
  std::int32_t target_priority = 100;

  /// Milliseconds between strikes. See `kDefaultAttackInterval`.
  std::int32_t attack_interval = kDefaultAttackInterval;
  /// `splash_radius`, declared only by `Catapult` (100).
  std::int32_t splash_radius = 0;
  /// `shot_height`, `[class+0xa0c]` (0x005a4647): how far above the ground a
  /// catapult's shot leaves, which also moves the ground end of its flight
  /// that far south (`siege-fire.md`, "A catapult's shot"). Declared by the
  /// catapults (100, 150).
  std::int32_t shot_height = 0;
  /// `projectile_class`, resolved. `kNoClass` for a unit that hits directly.
  ClassIndex projectile = kNoClass;

  bool can_be_attacked = true;
  bool is_projectile = false;

  /// `projectile_class` declared and not the loader's `**Invalid**` default:
  /// the test 0x0053f5b0 makes, which is what lets a `slash` or `pierce`
  /// unit hit a building at all. Not `is_ranged()`, which asks whether the
  /// name *resolved*; the original asks only whether it was given.
  bool shoots = false;
  /// `water_unit`, `[class+0xb30]` (the field `IsWaterUnit` reads): the
  /// `CanAttack` virtual lets a receiver whose class is not one reach one
  /// only if it shoots (0x005b1f0b).
  bool water_unit = false;
  /// The low word of the class's specials (`unit_specials`, which the unit
  /// constructor copies into `[unit+0x198]`): `special::k*`.
  std::uint32_t specials = 0;

  /// The classes `Unit::CanAttack` (0x005b1da0) asks about by name, answered
  /// once per class off its ancestry rather than once per candidate. Bits of
  /// `kLine*`. See `CombatSystem::unit_can_attack`.
  std::uint16_t lineage = 0;
  static constexpr std::uint16_t kLineRamUnit = 1u << 0;    ///< 0x00540460
  static constexpr std::uint16_t kLineSentry = 1u << 1;     ///< 0x005400c0
  static constexpr std::uint16_t kLineFakeTower = 1u << 2;  ///< the string at 0x007ba860
  static constexpr std::uint16_t kLineGate = 1u << 3;       ///< 0x005404d0
  static constexpr std::uint16_t kLineCatapult = 1u << 4;   ///< the type at 0x0081b24c
  /// `Wolf`, `Bear`, `Boar`, `LionM` or `LionF`: the five that may not
  /// attack a catapult (0x005b2040 onwards).
  static constexpr std::uint16_t kLineBeast = 1u << 5;
  /// The class *is* `Fish` -- an exact name compare (0x005b1ef5), not an
  /// ancestry test.
  static constexpr std::uint16_t kLineFish = 1u << 6;
  static constexpr std::uint16_t kLineOutpost = 1u << 7;       ///< 0x00440060
  static constexpr std::uint16_t kLineBaseShipyard = 1u << 8;  ///< 0x004400b0
  [[nodiscard]] bool is(std::uint16_t line) const noexcept { return (lineage & line) != 0; }

  /// The channel `type` meets. `siege` and `none` meet nothing.
  [[nodiscard]] std::int32_t armour_against(DamageType type) const noexcept;
  /// A class that shoots rather than reaches: `range` beyond melee reach.
  [[nodiscard]] bool is_ranged() const noexcept { return projectile != kNoClass; }
  /// `target_factor` of -1.
  [[nodiscard]] bool never_targeted() const noexcept { return target_factor < 0; }
  [[nodiscard]] bool can_attack() const noexcept {
    return damage > 0 && damage_type != DamageType::none;
  }

  /// Read every property above off the resolved class. `entity`, when given,
  /// supplies the attack cadence from animation slot `kAnimAttack`.
  [[nodiscard]] static CombatProfile from_class(const ClassGraph& graph, ClassIndex index,
                                                const Entity* entity = nullptr);
};

// ---------------------------------------------------------------------------
// per-object state
// ---------------------------------------------------------------------------

/// `Unit::AddBonus`'s record: five addends over a unit's class numbers.
///
/// The original keeps these in a side table of its own -- a map from object id
/// to record, held by the singleton at `0x9c0914` -- because the numbers it
/// modifies are *cached* on the object (`[obj+0xc8]` maxhealth, `[obj+0xcc]`
/// maxstamina, `[obj+0xdc]` attack, `[obj+0xe4]` armour against slash,
/// `[obj+0xe8]` against pierce) and every one of those caches is rebuilt from
/// the class the moment anything changes. `Unit::RecalcBonuses`'s virtual
/// (`vtbl+0xc4`, `0x005d9e53`) is that rebuild: it copies the five class
/// properties over the five fields, then looks this record up by the object's
/// own id (`[obj+8]`) and adds it back on top, and only then goes on to items
/// and skills. The record is what survives a rebuild that a written-through
/// field would not.
///
/// **Here it is a field on the combatant instead of a side table, and that is
/// an equivalence rather than a shortcut.** The record survives every rebuild
/// of `Combatant::stats` here as it does there, because the rebuild reads it
/// and never writes it; what the side table buys the original is that the
/// entry dies with the object, and a field on `Combatant` gets that from
/// `reconcile`, which drops the row of any object that has left the world.
/// The original does it explicitly, in the object teardown at `0x005db50f`.
struct StatBonus {
  /// Argument 1. Added to `[obj+0xdc]`, which `Obj::attack` and `Obj::damage`
  /// are both the same function over.
  std::int32_t attack = 0;
  /// Argument 2, `[obj+0xe4]`.
  std::int32_t armour_slash = 0;
  /// Argument 3, `[obj+0xe8]`.
  std::int32_t armour_pierce = 0;
  /// Argument 4, `[obj+0xc8]`.
  std::int32_t max_health = 0;
  /// Argument 5, `[obj+0xcc]` -- **and the one the entry point never reads.**
  /// See `CombatSystem::add_bonus`.
  std::int32_t max_stamina = 0;

  /// Every addend zero, which is the state in which the original erases the
  /// record from its table.
  [[nodiscard]] bool empty() const noexcept {
    return attack == 0 && armour_slash == 0 && armour_pierce == 0 && max_health == 0 &&
           max_stamina == 0;
  }
};

/// The six numbers a unit fights with, as the original stores them on the
/// object: `[obj+0xc8]` maximum health, `[obj+0xcc]` maximum stamina,
/// `[obj+0xd4]` range, `[obj+0xdc]` attack, `[obj+0xe4]` and `[obj+0xe8]` the
/// armour against slash and pierce. `docs/formats/unit-stats.md` has the
/// whole model; `CombatSystem::class_stats` is its first two steps.
struct StatBlock {
  std::int32_t max_health = 0;
  std::int32_t max_stamina = 0;
  std::int32_t range = 0;
  std::int32_t attack = 0;
  std::int32_t armour_slash = 0;
  std::int32_t armour_pierce = 0;

  friend bool operator==(const StatBlock&, const StatBlock&) noexcept = default;
};

/// The combat half of a unit's state vector, in the dumps' own terms.
///
/// The field list is `docs/engine/state-vector.md`'s `CVXUnit` block minus the
/// parts other systems own (`method`, the command queue, the path, `food`,
/// `squad`, `hero handle`, `holder handle`). `damage magic handle` is absent
/// because it is null in all 3,432 unit blocks and its meaning is unknown — a
/// field we cannot explain is left out rather than guessed.
struct Combatant {
  ObjectId id = kNoObject;
  ClassIndex class_index = kNoClass;
  PlayerId owner = kNoPlayer;
  /// World units. Movement owns this; combat reads it and writes it only
  /// through `set_position`.
  Point position;

  std::int32_t health = 0;
  std::int32_t stamina = 0;
  /// `[unit+0x184]`, the level the game uses: `Unit::level` (`vtbl+0x114`)
  /// and both sides of the blow read it. 1..24 in the dumps. **Stored, and
  /// written only by the rebuild** (`recalc_bonuses`): the level the running
  /// experience stands at, plus the items', Battle Cry's and the formation's,
  /// at least 1, then Discipline's floor. The running total itself is
  /// `UnitRecord::experience` (`[unit+0x180]`), which every award goes to.
  std::int32_t level = 1;

  /// `last attack time`, in game-time milliseconds. 0..1,738,000 in the dumps.
  GameTime last_attack_time = 0;
  /// `target handle`. Printed in hex where every other handle is decimal.
  ObjectId target = kNoObject;
  /// `number of attacks`, 0..77. The dumps' note that it counts attacks
  /// against the *current* target is unproven, so it is reset on target change
  /// here and that choice is flagged.
  std::int32_t attacks = 0;
  /// `unit flags`: 20 distinct values, bits 16-26 only, 9 of 11 bits
  /// unexplained. Carried verbatim and hashed; nothing here reads it.
  std::uint32_t unit_flags = 0;

  /// `danger`, `[unit+0x1b0]`, 0..6: how many target searches have recently
  /// answered this unit. Raised by 1 on every answer (0x005d2850), read by
  /// the unit score, the tower's search and the training search, and decayed
  /// by every read and raise -- a point per whole 1024 ms since
  /// `danger_time` -- through `CombatSystem::danger`. Zero from the
  /// constructor (0x005d3170); saved by the persist stream (0x005dd710).
  /// `docs/formats/target-score.md`.
  std::int32_t danger = 0;
  /// `lastdangertime`, `[unit+0x1b4]`: when `danger` last decayed. A decay
  /// moves it to the time of the read, dropping the part of a second it did
  /// not count.
  GameTime danger_time = 0;

  Action action = Action::idle;
  std::int32_t anim = kAnimIdle;

  /// When the dying animation ends and the object leaves the world. Meaningful
  /// only while `action == Action::dying`.
  GameTime death_time = 0;

  /// The next game time this combatant acts at. Absolute, so that where turn
  /// boundaries fall cannot change what happens: see `CombatSystem::advance`.
  GameTime next_action_time = 0;

  /// The numbers the original stores on the object and every reader reads:
  /// maximum health and stamina, range, attack and the two armours. Filled
  /// from the class when the combatant is enrolled and rebuilt by
  /// `recalc_bonuses` (`sim/stats.hpp`, 0x005d9df0) at the moments the
  /// original rebuilds them -- so between two of those moments they are
  /// stale exactly as the original's are. See `StatBlock`.
  StatBlock stats;

  // -- modifiers other systems write ----------------------------------
  //
  // Nothing writes `cannot_fight` (Ceasefire is read off the hero by the
  // blow itself, as Assault is). The hero
  // skills that used to be fields here -- Team Attack, Team Defense,
  // Discipline -- land in `stats` and in the stored level through the
  // rebuild, and Frenzy's doubling is the rebuild's too.
  //
  // `damage_multiplier_percent` and `in_settlement` are read by nothing: the
  // blow has no flat multiplier and no `DamageAmplify` (`docs/formats/blow.md`).
  // They stay in the save until its next change.
  std::int32_t damage_multiplier_percent = 100;
  bool cannot_fight = false;
  bool in_settlement = false;

  /// `Unit::AddBonus`'s five addends. Reset by `reclass`, for the reason the
  /// three fields above it are: the original mutates by minting a fresh object,
  /// and the record is keyed on the id that goes away with the old one.
  StatBonus bonus;

  bool alive = true;

  /// The level the damage formula sees, before the owner's difficulty addend
  /// the system supplies.
  [[nodiscard]] std::int32_t base_effective_level() const noexcept { return level; }
};

/// A projectile in flight. `CVXCatapultShot` is a native class in its own right
/// (3 shipped classes: `Gule`, `CGule`, `IGule`) and archers name a
/// `projectile_class` too (`Arrow`, `Javelin`, `Axe`, `Slingstone`,
/// `Big_Arrow`), so a shot is an object with a lifetime rather than an
/// instantaneous hit.
///
/// The arc is not modelled: the position here interpolates the ground track
/// and the height is left to the renderer. A catapult's ground shot keeps the
/// original's clock -- ten units of track every 100 ms, so it lands
/// `1 + 100 * (S / 10)` ms after it leaves for a flight of `S` units
/// (`launch_at_point`, `siege-fire.md`). **An arrow's flight time is still
/// INVENTED**: `kProjectileSpeed` world units per second.
///
/// **The two kinds stand differently in the handle space**, as in the
/// original (`docs/formats/blow.md`, "The missile"). An arrow -- a unit's
/// shot (0x005dc650) or a tower's (0x004df650), both the missile
/// `CVXArrowShot` (0x004d9740) -- asks its base for no handle and is known
/// only by its place in `CombatSystem::projectiles()`, launch order; its `id`
/// is `kNoObject`, the original's 0xffff. A catapult's ground shot, a
/// `CVXCatapultShot` (0x004e47c0), takes a synchronous handle like a unit,
/// and its `id` is the world object that stands for it.
struct Projectile {
  /// The world object of a ground shot; `kNoObject` for an arrow, which has
  /// none (see above).
  ObjectId id = kNoObject;
  ObjectId shooter = kNoObject;
  ObjectId target = kNoObject;
  ClassIndex class_index = kNoClass;

  Point origin;
  Point aim;      ///< where it is going: the target's position at launch
  Point position; ///< interpolated

  GameTime launched = 0;
  GameTime impact = 0;

  /// The shooter's attack and level at launch: the launch event's number and
  /// the save's. **A targeted shot does not strike with them**: its landing
  /// (0x004d9bb0) calls the dealer with the live shooter, which reads both
  /// afresh, and does nothing once the shooter is dead.
  std::int32_t attack = 0;
  DamageType type = DamageType::slash;
  std::int32_t attacker_level = 1;
  std::int32_t splash_radius = 0;
  /// A catapult's shot at a point (`Catapult::Attack(point)`): it lands on
  /// the ground at `aim` and hurts what stands there by the impact rule of
  /// `docs/formats/siege-fire.md` -- everyone, raw, by distance -- rather
  /// than striking `target`, which is none.
  bool ground = false;
  /// The class of whoever fired a ground shot, for its counter percent: the
  /// shooter may be gone when it lands.
  ClassIndex shooter_class = kNoClass;
};

/// INVENTED. World units per second of an arrow's (a unit's or a tower's)
/// flight. A catapult's ground shot does not use it.
inline constexpr std::int32_t kProjectileSpeed = 800;

/// A catapult shot's step (0x004e4650): `10.0` units of track
/// (`[0x007edb08]`) per message, the message re-armed every 100 ms and first
/// due 1 ms after the shot is placed (0x004e4160 -> 0x00688240).
inline constexpr std::int32_t kCatapultShotStep = 10;
inline constexpr std::int32_t kCatapultShotStepMs = 100;

/// The flight time of a catapult shot whose track is `distance` units: the
/// shot lands on the first message whose step carries it past the end, which
/// is message `distance / 10 + 1`.
[[nodiscard]] constexpr GameTime catapult_shot_flight(std::int64_t distance) noexcept {
  return 1 + kCatapultShotStepMs * (distance / kCatapultShotStep);
}

// ---------------------------------------------------------------------------
// the target-selection family
// ---------------------------------------------------------------------------

/// The knobs the executable's *one* target-acquisition core actually has.
///
/// ## This is read out of `gbr.exe`, not inferred
///
/// The corpus calls nine `Best*Target*` members and the shipped engine answers
/// six of them with a single function, `0x005dc950`, called with five extra
/// arguments. The six wrappers are byte-for-byte identical apart from those
/// arguments, so the *whole* difference between the variants is five flags:
///
/// | Host entry point (`Unit::`)                | `0x005dc950` call        | wrapper    |
/// |---|---|---|
/// | `BestTargetInSquadSight/0`                 | `(u, 0, 0, 0, 0, 0)`     | `0x5dcbc0` |
/// | `BestTargetInSquadSightMisZeroDamage/0`    | `(u, 0, 0, 0, 0, **1**)` | `0x5dcb00` |
/// | `BestNoIndependentTargetInSquadSight/0`    | `(u, 0, 0, **1**, 0, 0)` | `0x5dcc80` |
/// | `BestTargetInSquadSight_PreferUndiseased/0`| `(u, 0, **1**, 0, 0, 0)` | `0x5dcd40` |
/// | `BestTargetInSquadSight/1(str)`            | `(u, **s**, 0, 0, 0, 0)` | `0x5dce00` |
/// | `BestTargetInSquadSightExclusive/1(str)`   | `(u, **s**, 0, 0, **1**, 0)` | `0x5dcf30` |
///
/// The argument types are the executable's own registration table, not a guess
/// from call sites: the table at `0x00699bb0` is called with
/// `(registry, fn, name, ret, argc, argtypes...)` and gives every one of these
/// `ret = 0x14` (`Obj`), first argument `0x15` (`Unit`), and `0x0b` (`str`) for
/// the second argument of the two `/1` forms.
///
/// `0x005dc950` builds a predicate object (constructor `0x005d45a0`), sweeps
/// the spatial grid with it (`0x005db8c0`) and offers each candidate to
/// `0x005db7f0` -> `0x005db650`, which is where the five flags are read. What
/// each one does, at the instruction:
///
///   * **`MisZeroDamage`** (`0x005db69e`): `if (candidate->damage <= 0) skip`.
///     The field is `[obj+0xdc]`, which is exactly what `Obj::damage` and
///     `Obj::attack` return -- **both host properties are the same function**,
///     `0x005add80`. So the name means *omit candidates that deal no damage*,
///     the candidate's own stat, and **not** "omit candidates I would do zero
///     damage to". `DATA\SUBAI\UNIT_IDLE.VS` corroborates from the other side:
///     it takes the result and immediately re-tests `if (u.attack > 0)`, which
///     under this reading is the same predicate written twice.
///   * **`NoIndependent`** (`0x005d45a6`): the attacker's enemy-player mask is
///     `and`ed with `0xffffbfff` before the sweep, clearing **bit 14 only**.
///     Player 14 is `kNeutralWildlife`; bit 15 is left alone.
///   * **`PreferUndiseased`** (`0x005db6d7`): a candidate with bit 1 of
///     `[obj+0x194]` set -- which is exactly what `Unit::IsDiseased`
///     (`0x005d89a0`) tests -- has **200 added to its score**. A penalty, not
///     an exclusion: a diseased target 200 nearer still wins.
///   * **the class string** (`0x005db6f4`): the candidate's class name is
///     compared with the argument. On a *mismatch*, `Exclusive` rejects the
///     candidate outright (`0x005db774`) and the plain `/1` form instead adds
///     200 to its score (`0x005db789`).
///
/// ## The score they share
///
/// The two soft variants add **200 to the executable's own score**, and that
/// score (`0x005d3630`) is `target_factor + 40 * danger + surface_distance`
/// with three adjustments -- a linear sum in world units, on which 200 is a
/// couple of hundred units' nudge. `docs/formats/target-score.md` has the
/// whole of it: the score, the predicate's in-range bonus and early stop, the
/// danger counter, the catapult fallback and the sweeps.
/// `CombatSystem::best_target` and its siblings are that model; each
/// variant's flags reach it through this type.
///
/// **Where the flags apply depends on the path.** On the squad path (a unit
/// in a squad, walking its squad's `Enemies`, `best_target_in_list`)
/// 0x005dc950 passes them where their names say. On the circle path it builds
/// the predicate one argument over (0x005dca31..0x005dca5a): the damage flag
/// lands in the exclusive slot and does nothing without a name, the
/// independents flag is dropped, and the exclusive flag lands in the
/// independents slot. `squad_sight_target` in `combat.cpp` builds each path's
/// filter, and answers a held or dead unit before either is consulted.
struct TargetFilter {
  /// The predicate's armed slot (`[search+0x30]`): skip a candidate whose own
  /// attack is not strictly positive. Measured against `Obj::attack`, the
  /// stored `[obj+0xdc]`.
  bool require_armed = false;

  /// The no-independents flag: the enemy mask loses bit 14, so candidates
  /// owned by `kNeutralWildlife` are not offered. Only that player.
  bool exclude_independents = false;

  /// The predicate's class-name slot (`[search+0x34]`) is set. With it, a
  /// candidate whose class is not `named_class` is a mismatch: rejected when
  /// `exclusive`, otherwise 200 worse and denied the in-range bonus. A name
  /// that resolves to nothing (`kNoClass`) mismatches every candidate.
  bool named = false;
  /// What the name resolves to. Exact class, not ancestry: the executable
  /// compares the class object's own name string.
  ClassIndex named_class = kNoClass;
  /// The predicate's exclusive slot (`[search+0x2c]`): a class mismatch is
  /// rejected (0x005db774) rather than charged 200 (0x005db789).
  bool exclusive = false;

  /// The prefer-undiseased slot (`[search+0x28]`): a diseased candidate
  /// (`ObjectFlags::diseased`, bit 1 of `[obj+0x194]`) is 200 worse.
  bool prefer_undiseased = false;

  /// `BestTargetForPos`'s in-range slot (`[search+0x38]`): keep only
  /// candidates whose surface distance from the attacker is within
  /// `[min_range, range]` (0x005db820), both ends surface to surface.
  bool require_in_attack_range = false;

  /// `BestTargetInRange`. Sweep a circle around `centre` of radius `radius`
  /// instead of around the attacker out to its `sight`. Scoring still measures
  /// from the attacker: `0x005dd060` passes the point as the sweep centre and
  /// the attacker's own position as the predicate's origin.
  bool centre_on_point = false;
  Point centre{};
  /// Zero or less means the attacker's `sight`, which is what `0x005dd11d`
  /// substitutes -- as it does for any radius when the attacker shoots
  /// (`CombatProfile::shoots`, 0x005dd123). A centre whose x is -1 is the
  /// attacker's own position (0x005dd0f8).
  std::int32_t radius = 0;

  /// The `*InSquadSight` circle path (0x005dca1d): sweep around the
  /// attacker's hero, out to the hero's sight, when it has one.
  bool around_hero = false;
};

// ---------------------------------------------------------------------------
// the system
// ---------------------------------------------------------------------------

/// One strike, as it happened. Emitted for tests, replays and the UI.
struct CombatEvent {
  enum class Kind : std::uint8_t { strike, launch, impact, death };

  Kind kind = Kind::strike;
  GameTime time = 0;
  ObjectId attacker = kNoObject;
  ObjectId defender = kNoObject;
  std::int32_t damage = 0;
  std::int32_t remaining_health = 0;
};

/// Combat, as a `System`.
///
/// ## Why the turn loop is event-driven
///
/// `System::advance` promises nothing about turn length — 200, 400, 799 and 800
/// all occur in the retail dumps, and the length is renegotiated mid-session.
/// A system that does one strike per turn would therefore make the outcome of a
/// battle depend on the network latency of the peers watching it.
///
/// So nothing here is per-turn. Every combatant carries an absolute
/// `next_action_time` and every projectile an absolute `impact`, and `advance`
/// drains the events falling inside `(turn.start - 1, turn.end]` in time order,
/// breaking ties by kind and then by object id. Running turns of 400+400 and
/// one turn of 800 execute exactly the same events in exactly the same order —
/// which is the property `test_combat.cpp` asserts, and the reason this is
/// written as an event queue rather than as the obvious loop over units.
///
/// ## Who owns what
///
/// The system holds its combatants in a vector in registration order and finds
/// them by binary search on id. Its shots are a list of its own, in launch
/// order: an arrow takes no world handle, as the original's missile takes
/// none (0x004d9740), and only a catapult's ground shot, which takes a
/// synchronous handle in the original (0x004e47c0), is spawned and despawned
/// as a world object.
class CombatSystem final : public System {
 public:
  CombatSystem() = default;
  explicit CombatSystem(CombatConstants constants) noexcept : constants_(constants) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "combat"; }
  void advance(World& world, const Turn& turn) override;
  void hash(std::uint64_t& accumulator) const override;

  // -- configuration ---------------------------------------------------

  [[nodiscard]] const CombatConstants& constants() const noexcept { return constants_; }
  void set_constants(const CombatConstants& constants) noexcept { constants_ = constants; }

  /// The graph profiles are read from. Not owned; must outlive the system.
  void set_class_graph(const ClassGraph* graph) noexcept { graph_ = graph; }
  [[nodiscard]] const ClassGraph* class_graph() const noexcept { return graph_; }

  void set_counter_table(CounterTable table) { counters_ = std::move(table); }
  [[nodiscard]] const CounterTable& counter_table() const noexcept { return counters_; }
  void set_counter_mode(CounterMode mode) noexcept { counter_mode_ = mode; }
  [[nodiscard]] CounterMode counter_mode() const noexcept { return counter_mode_; }

  /// The calculator's match cache (0x0050ff30, 0x00510351..0x005106b0;
  /// `docs/formats/blow.md`, "The match cache"): filled by the first blow
  /// that reaches the calculator after a clear, and frozen until the next.
  ///
  /// **Not saved and not hashed, as the original's is not.** It is a copy of
  /// what the match, `CONST.INI` and the environment ledger held at the fill,
  /// and a load clears it (0x00524460) so that the next blow fills it again
  /// from the loaded game. So is a new game (0x00525dd0, a fresh system
  /// here), `InvalidateDamageFormulaParams` (0x004c6b10) and a difficulty
  /// change (0x006e71c0, which `SetDifficulty` reaches).
  struct DamageFormulaParams {
    /// Filled since the last clear: the dword at 0x00996ae8.
    bool valid = false;
    /// Whose units the addend favours, 0-based; `kNoPlayer` for the
    /// original's -10 (no single-player game, or no local player) and -11
    /// (an addend of 0).
    PlayerId favoured = kNoPlayer;
    /// `GamblersLuckChance` and `GamblersTestChance` of the ledger's
    /// `/<map>/Player<n>/`, by 0-based player (`EnvReadInt`).
    std::array<std::int32_t, kPlayerCount> gamblers_luck{};
    std::array<std::int32_t, kPlayerCount> gamblers_test{};
    /// The three researches, each true when its ledger string is exactly
    /// `researched`.
    std::array<bool, kPlayerCount> attack_focus{};
    std::array<bool, kPlayerCount> learning{};
    std::array<bool, kPlayerCount> ferocity{};
  };
  /// The cache, filled first if a clear left it empty: the favoured player
  /// is the match's human in a game that is not a networked one (the
  /// original's `[game+0xa0] == 1`, believed single-player; **inferred**)
  /// and its addend the `CONST.INI` one of the match's difficulty; then the
  /// sixteen players' ledgers.
  const DamageFormulaParams& damage_formula_params(World& world);
  /// The favoured player's addend, the global at 0x00996dc8. The fill writes it
  /// only for a difficulty of 0, 1 or 2, so it outlives a clear.
  [[nodiscard]] std::int32_t favoured_addend() const noexcept { return favoured_addend_; }
  /// `InvalidateDamageFormulaParams()`: the next blow fills the cache again.
  void invalidate_damage_formula_params() noexcept { params_.valid = false; }

  /// `SetGlobalBloodlust(bool)` (0x004c6610): the byte at 0x00996ae4 that the
  /// calculator's armour step (C20) and a missed shot's Penetration (C15)
  /// read. **A process global** (`sim/process.hpp`): written through to the
  /// `ProcessGlobals` this system was given, else to its own; neither saved
  /// nor hashed, so a `deserialize` leaves it as it was.
  void set_bloodlust(bool on) noexcept {
    (process_ != nullptr ? process_->bloodlust : own_process_.bloodlust) = on;
  }
  [[nodiscard]] bool bloodlust() const noexcept {
    return process_ != nullptr ? process_->bloodlust : own_process_.bloodlust;
  }
  /// The process this system's globals live in. Null (the default) is a
  /// process of its own, which starts at zero. `GameSession::create` passes
  /// `SessionInputs::process`, or the session's own. Not owned.
  void set_process(ProcessGlobals* process) noexcept { process_ = process; }

  /// `SetExperienceModifier(player, percent)` -- the *Battle tactics* research,
  /// which `ONFINISH_RESEARCH.VS` completes with `GetConst(
  /// "BattleTacticsExpModifier")`.
  ///
  /// **Per player**, because `gbr.exe` 0x005d2fb0 writes it into the player
  /// record at `+0x84` and not into a global -- and therefore turn state, so it
  /// is saved and hashed. A player nobody has set reads 0, no modifier: the player
  /// record's constructor (0x00565a00) zeroes the word. What it does to a
  /// kill's gain is `kill_gain`'s.
  void set_player_experience_modifier(PlayerId player, std::int32_t percent) noexcept;
  [[nodiscard]] std::int32_t player_experience_modifier(PlayerId player) const noexcept;

  /// Register a class profile explicitly. Tests with no class graph use this;
  /// with a graph, profiles are read on demand and cached.
  void set_profile(ClassIndex index, const CombatProfile& profile);
  /// By value, deliberately: the cache fills lazily, so a reference into it
  /// could be invalidated by the very next lookup. A profile is twenty ints.
  [[nodiscard]] CombatProfile profile(ClassIndex index) const;

  // -- population ------------------------------------------------------

  /// Add a combatant. Ids must be unique and are kept in ascending order, so
  /// that iteration order is spawn order and therefore world state.
  Combatant& add(const Combatant& combatant);
  [[nodiscard]] Combatant* find(ObjectId id) noexcept;
  [[nodiscard]] const Combatant* find(ObjectId id) const noexcept;
  [[nodiscard]] std::span<const Combatant> combatants() const noexcept { return units_; }
  [[nodiscard]] std::span<const Projectile> projectiles() const noexcept { return shots_; }
  /// Drop a combatant outright, without a death animation. Removal on death
  /// goes through the dying state instead.
  bool remove(ObjectId id);

  void set_position(ObjectId id, Point position) noexcept;

  /// Exchange position, owner and health with `World::state` every turn.
  ///
  /// **Off by default, and that is not timidity.** A combatant is keyed by
  /// `ObjectId`, and binding is only correct when those ids really are world
  /// object ids. A system populated with ids of its own -- every synthetic
  /// test, and any embedder that has not wired the two together yet -- would
  /// otherwise have its combatants silently overwritten by whatever object
  /// happens to hold the same handle, including the projectile objects this
  /// system spawns itself. Turn it on when the ids are the world's; movement
  /// then owns position and combat owns health, with no shadow copy of either.
  void set_world_bound(bool bound) noexcept { world_bound_ = bound; }
  [[nodiscard]] bool world_bound() const noexcept { return world_bound_; }

  /// Make the population equal the set of objects `is_combat_object` accepts.
  ///
  /// Enrols every eligible object that is not here yet and drops every
  /// combatant whose object has left the world, and returns the resulting
  /// population. A no-op unless `world_bound_`, for the reason above: without
  /// world ids there is nothing to reconcile against.
  ///
  /// **This is called from `start` and again at the top of every `advance`**,
  /// which is what makes the population follow the world rather than snapshot
  /// it. Objects spawn and die during a match -- combat despawns its own dead,
  /// items despawn when taken, and any future `Create`/`SpawnGroup` will spawn
  /// -- and a load-time-only population would answer for a world that no longer
  /// exists. Both sequences are sorted ascending by id (`World::objects()`
  /// because `despawn` only erases, `units_` because `add` inserts in order),
  /// so this is a linear merge and the enrolment order is exactly ascending
  /// `ObjectId`, which is spawn order, which is a deterministic function of the
  /// map.
  ///
  /// A new combatant is seeded from the world: `state.health` (which the loader
  /// resolved from the map's `healthperc`), `state.owner`, `state.position`,
  /// and `maxstamina` off the class. **`experience` starts at 0 and `level` at
  /// 1** -- see the note in `sim/combat.cpp` on why the map's `Level` attribute
  /// is not read.
  std::size_t reconcile(World& world);

  // -- alliances -------------------------------------------------------
  //
  // Two objects are enemies when their owners differ, neither is `kNoPlayer`,
  // and the pair is not declared allied. That is the whole model: alliances
  // belong to the diplomacy layer, and this only needs the predicate.

  /// Declare two players allied in this system's own table.
  ///
  /// **A fallback, not the truth.** Diplomacy lives in `World::players()`, is
  /// one-directional, and is read straight out of the map; this symmetric table
  /// predates that and survives only so a `CombatSystem` built without a world
  /// -- which is most of `test_combat.cpp` -- can still express hostility.
  /// Once `start` has seen a world, `is_enemy` ignores it entirely.
  void set_allied(PlayerId a, PlayerId b, bool allied);

  /// Whether `a` treats `b` as an enemy.
  ///
  /// Consults `World::players()` when this system has been started against a
  /// world, and the local ally table otherwise. The two disagree in a way that
  /// matters: the real relation is **one-directional** (`sim/player.hpp` has
  /// the disassembly), and a symmetric table cannot express the one-sided
  /// truces the shipped maps contain.
  [[nodiscard]] bool is_enemy(const Combatant& a, const Combatant& b) const noexcept;

  /// Bind the player table. Called by `start`; exposed so a test can bind one
  /// without building a whole world.
  void set_players(const PlayerTable* players) noexcept { players_ = players; }

  void start(World& world) override;

  // -- orders and queries ----------------------------------------------

  /// Order an attack. Fails when either id is unknown, the target cannot be
  /// attacked, the pair are not enemies, or **either is dying**. A corpse
  /// neither fights nor is fought: without the second half, a dying unit's
  /// own `while (.Attack(u))` would put it back into the engaged state and it
  /// would swing from the grave.
  bool order_attack(ObjectId attacker, ObjectId target);

  /// What the two siege target sweeps and `Building::Attack` ask of a
  /// candidate: alive, not inside a holder, an enemy, and the `CanAttack`
  /// virtual (`attack_virtual`).
  [[nodiscard]] bool valid_target(const Combatant& self, const Combatant& other) const;

  // -- the attack on the animation timer (docs/formats/animation.md, "The
  //    attack") --------------------------------------------------------

  /// The `CanAttack` virtual (`vtbl+0xb4`, 0x005b1da0) between two
  /// combatants, by id: `attack_virtual` on their world objects, or, for a
  /// combat system bound to no world, a living enemy whose class may be
  /// attacked. What `Attack`'s and `CanAttack`'s gates, `IsValidTarget` and
  /// the blow ask.
  [[nodiscard]] bool can_attack(ObjectId self, ObjectId target) const;
  /// 0x005a7800, measured where the two stand now: the floored root of the
  /// squared centre distance less both class radii, at most the receiver's
  /// stored range `[unit+0xd4]` and, when its minimum `[unit+0xd8]` is 1 or
  /// more, at least that.
  [[nodiscard]] bool in_range(ObjectId self, ObjectId target) const;
  /// 0x005a77b0: centre to centre, floored root, less both class radii.
  [[nodiscard]] std::int64_t edge_distance(ObjectId self, ObjectId target) const;
  /// `[unit+0xd8]`: the class's `min_range`, which the constructor stores and
  /// no rebuild touches.
  [[nodiscard]] std::int32_t min_range_of(const Combatant& unit) const;
  /// Start the attack: the frame (`target`, word 0) written on the unit's
  /// clock with the script to wake, and the action mode set to 2 through the
  /// setter (0x005a7680), which runs the end handler at once when no timer is
  /// pending. `Attack`, `TrainAttack`.
  void begin_attack(World& world, script::Scheduler* scheduler, ObjectId id, ObjectId target,
                    std::uint32_t waiter, GameTime now);
  /// The end handler's mode 2 (`vtbl+0x11c`, 0x005d5ed0): with the frame's
  /// word still 0 and its target there, a swing -- one of the attack
  /// variants, its length over the speed factor, turned to the target -- and
  /// the target told it is attacked (`vtbl+0xa4`); otherwise the call's
  /// answer (the blow landed and the target lives) pushed, its script woken
  /// and the mode back to 0.
  void attack_end(World& world, script::Scheduler* scheduler, ObjectId id, GameTime now);
  /// The action handler's mode 2 (`vtbl+0x120`, 0x005dc900): the blow at the
  /// frame's target, its word 1 when it landed and 2 when not, and the
  /// unit's attack count `[unit+0x1ac]` raised.
  void attack_action(World& world, ObjectId id, GameTime now);
  /// One blow (0x005dc650): refused by the `CanAttack` virtual or beyond the
  /// stored range plus 150; then the shot or the melee blow (0x00511550), the
  /// last-attack time `[unit+0x1a4]` and a hidden attacker shown. True when
  /// it was dealt.
  bool blow(World& world, ObjectId id, ObjectId target, GameTime now);

  /// `Catapult::Attack(point)`'s shot (0x004e22a0): a ground shot of the
  /// engine's class leaving `from` at `release` -- the attack animation's
  /// action moment, which may lie ahead -- for `at`, carrying the class's
  /// `damage` and `splash_radius`. `from` is the launch point
  /// (`catapult_shot_origin`). The flight is the original's (0x004e47c0,
  /// 0x004e4650): `catapult_shot_flight` of the distance to `at` from `from`
  /// moved `shot_height` south. False when `catapult` is no combatant.
  /// See `docs/formats/siege-fire.md`.
  bool launch_at_point(World& world, ObjectId catapult, Point from, Point at, GameTime release);

  /// `Building::Attack`'s arrow (0x004df650): `tower` shoots `target` now,
  /// the arrow resolved as any ranged attacker's is. Stamps the tower's last
  /// shot and the target's squad; no swing, no army stamp. False when either
  /// is no combatant.
  bool fire_arrow(World& world, ObjectId tower, ObjectId target, GameTime now);

  /// The `CanAttack` virtual (`vtbl+0xb4`, body 0x005b1da0): **whether this
  /// object may strike that one at all.** Playtest #24 was its absence -- a
  /// swordsman ordered onto a fort hit it and its health fell.
  ///
  /// **One body for every object family.** The same function sits at
  /// `+0xb4` in all 22 of the executable's object tables -- `CVXUnit`
  /// (0x007d0af8), `CVXBuilding` (0x007ba488), the catapult's, the flying
  /// unit's, the ship's, the hero's and the rest -- so a tower or an engine
  /// asks exactly what a soldier asks, from the receiver's side: only the
  /// unit-receiver clauses below are skipped for it.
  ///
  /// It is the first half of every gate the original puts in front of a
  /// strike. `Unit::Attack` (0x005ddb50) passes its target through 0x005a78a0,
  /// which is this and then the range band (0x005a7800); `Obj::IsValidTarget`
  /// (0x005aafd0) is this alone; `Obj::CanAttack` (0x005ab060) is this and the
  /// band; the blow (0x005dc650) is this and the range plus 150; and the
  /// target sweep's predicate (0x005db650), `EnemiesInSight` (0x005d5180),
  /// `EnemyInRange` (0x004dcb20), the siege planner's split and the towers'
  /// sweeps ask it of every candidate.
  ///
  /// The rule, in the original's order (receiver R, target T):
  ///
  ///   1. T dead (`vtbl+0x50`: a unit's or a catapult's health gone; a
  ///      building never is): no.
  ///   2. T hidden (bit 21 of `[obj+0x2c]`) and its owner's bit not in R's
  ///      side mask (`[record+0x18]`, `seeflags`: R's own player and every
  ///      player sharing its view with R): no.
  ///   3. Unless R is a unit carrying `strikes_anyone`: T an enemy of R's
  ///      owner (bit 0 of R's row clear), or training, or a building with
  ///      health -- else no. A non-enemy building passes this, so the
  ///      virtual alone lets an archer aim at its own wall; the sweeps add
  ///      their own enemy masks.
  ///   4. R a unit under its hero's ceasefire (0x005d37f0, skill 10): no.
  ///   5. T a unit: in the air (bit 22 of `[unit+0x194]`) or a `Sentry` heir,
  ///      only to an R that shoots (0x0053f5b0); under a ceasefire of its
  ///      own, no; and never to a `RamUnit`.
  ///   6. T's class *is* `Fish`, by name: no.
  ///   7. T's class a `water_unit` and R's not: only if R shoots.
  ///   8. R or T a messenger (bit 26 of `[obj+0x2c]`): no.
  ///   9. T a building, through its settlement S: at no health only when S's
  ///      central building is an `Outpost` or `BaseShipyard` heir and S's
  ///      holder holds someone; never a `FakeTower`; never when S's
  ///      `canbeattacked` (`[settlement+0x40]`) is clear; and a `RamUnit` R
  ///      stops here -- yes for an outpost's or shipyard's S or a `Gate`, no
  ///      otherwise.
  ///  10. T neither unit nor building: no.
  ///  11. T a catapult: anyone but the five wild beasts.
  ///  12. R's `damage_type` (`[class+0x2a4]`): `slash` and `pierce` take a
  ///      unit always and a building only when R shoots; `siege` anything;
  ///      `none` nothing. **So a swordsman is refused every building, an
  ///      archer, slinger or javelin thrower is not, and neither is
  ///      `BritonHero`, whose class has a `projectile_class`.**
  ///
  /// The catapult of clause 11 is the native class's in the original (the
  /// type at 0x0081b24c) and the `Catapult` line here; they are the same
  /// classes in the retail install. **Labelled:** a building with no
  /// settlement, which the original never has (it reads S unguarded), is
  /// judged by its own class's `can_be_attacked`.
  [[nodiscard]] bool attack_virtual(const WorldObject& self, const WorldObject& target) const;
  /// `attack_virtual` between two combatants' world objects; true for a
  /// combat system bound to no world, whose fixtures have no flags or
  /// settlements to ask.
  [[nodiscard]] bool unit_can_attack(const Combatant& self, const Combatant& target) const;
  /// Drop the current target and go idle.
  bool stop(ObjectId id);
  /// Drop the current target and nothing else: `gbr.exe`'s target setter
  /// (0x005d2c70) handed the empty handle -- the write `Unit::Stop`,
  /// `Unit::Idle`, `Unit::Taunt`, `Unit::FormKeepMoving`,
  /// `Unit::FormAcceptMove` and both `Goto` bodies make, through the setter or
  /// inline. `[unit+0x1a8]` is the dump's `target handle` (the printer at
  /// 0x005d2bd8) and `[unit+0x1ac]` its `number of attacks`; both are written
  /// only when the handle changes, so a unit with no target keeps its count.
  /// The action is left alone: the setter does not touch it. False for an
  /// unknown id.
  bool drop_target(ObjectId id);
  /// The same setter handed a handle (0x005d2c70's shape, inline in
  /// `Unit::GotoAttack` at 0x005d6b10 .. 0x005d6b2d): `[unit+0x1a8]` takes
  /// `target` -- `kNoObject` for a handle naming nothing -- and the attack
  /// count is zeroed, only when the handle changes. The action is left alone.
  /// False for an unknown id.
  bool hand_target(ObjectId id, ObjectId target);
  /// Whether `id` is a combatant in its dying state: still in the world as a
  /// corpse, and no longer a unit anything should drive. `reap_departed` reads
  /// it to end the corpse's own scripts.
  [[nodiscard]] bool is_dying(ObjectId id) const noexcept;
  /// Whether anything has entered its dying state since the last
  /// `forget_fresh_deaths`. The scheduler's step hook asks it after every
  /// script slice, so that a unit a script kills -- `Obj::Damage`,
  /// `WALL_PATROL.VS` putting down its sentries when its wall changes hands --
  /// loses its own scripts before any of them runs again in the same pass.
  /// Derived bookkeeping, never hashed or saved: it is cleared every slice.
  [[nodiscard]] bool has_fresh_deaths() const noexcept { return fresh_deaths_; }
  void forget_fresh_deaths() noexcept { fresh_deaths_ = false; }

  /// This engine's own acquisition: what `act` gives a unit with no target
  /// between its scripted orders, and what `BestTarget` answers on a unit
  /// receiver (which has no body of its own in the original).
  ///
  /// **Not the original's.** Nothing in `gbr.exe` acquires a target for a
  /// unit outside a script's search, so this has no counterpart to match; it
  /// keeps the rule it was written with -- `(distance^2 + 1) * target_factor
  /// / target_priority`, smallest wins, lower id on a tie, buildings
  /// included, within the class `sight` -- and changes no counter.
  [[nodiscard]] ObjectId auto_target(ObjectId id) const;

  /// The unit target family's search, as the original runs it
  /// (`docs/formats/target-score.md`): the sweep, the catapult fallback, the
  /// predicate 0x005db650 and the score 0x005d3630, under `filter`'s slots.
  /// A circle sweep in grid order that stops at the predicate's early take.
  ///
  /// **Reads, writes nothing**: the `danger` each score reads is decayed for
  /// the reading and not stored, and the answer's is not raised. `acquire`
  /// is the same search as the entry points run it. With no world, every
  /// combatant counts as a unit and the sweep is the roll in id order.
  [[nodiscard]] ObjectId best_target(ObjectId id) const;
  [[nodiscard]] ObjectId best_target(ObjectId id, const TargetFilter& filter) const;

  /// `best_target` as the entry points run it: every candidate's `danger`
  /// read decays it in place (0x005d3630's own read), and a unit answer has
  /// its `danger` raised by one (0x005d2850, from 0x005dc950 and 0x005dd060).
  ObjectId acquire(ObjectId id, const TargetFilter& filter);

  /// The predicate and score over an **explicit** candidate set, with **no
  /// reach** and no early stop, writing as `acquire` does except that the
  /// answer's `danger` is not raised: `Unit::BestTargetInGAIKA`'s squad walk
  /// (0x004282c0), whose core's raise was not read.
  ///
  /// `sim/ai.cpp` is the only caller; it lives there because the candidate set
  /// is GAIKA's, not combat's.
  ObjectId best_target_among(ObjectId id, std::span<const ObjectId> candidates);

  /// The squad path of the `BestTargetInSquadSight` core (0x005dc950 ->
  /// 0x005dbb20): every entry of the attacker's squad's `Enemies`, in order,
  /// offered to the predicate (0x005db7f0 -> 0x005db650) with **no reach** --
  /// the list is what the squad's look saw, and nothing measures it again --
  /// under `filter`'s slots. The walk never stops early, because 0x005dbb20
  /// discards what the predicate answers, so a later candidate can still
  /// displace an early take. Writes as `acquire` does, raise included.
  ObjectId best_target_in_list(ObjectId id, std::span<const ObjectId> candidates,
                               const TargetFilter& filter);

  /// `danger` as 0x005d2800 reads it at `now`: decayed one point per whole
  /// 1024 ms since `danger_time`, the decay stored. 0 for no combatant.
  std::int32_t danger(ObjectId id, GameTime now);
  /// 0x005d2850: the same decay, then `amount` added and the sum kept in
  /// 0..6. Nothing for no combatant.
  void raise_danger(ObjectId id, std::int32_t amount, GameTime now);
  /// The decay without the store: what a read at `now` would answer.
  [[nodiscard]] std::int32_t danger_at(ObjectId id, GameTime now) const;

  /// Whether `attacker` can strike `defender` from where they stand: squared
  /// distance between `min_range^2` and `(range + both radii)^2`. Comparing
  /// squared distances keeps it integral — no square root, no rounding.
  [[nodiscard]] bool in_attack_range(const Combatant& attacker, const Combatant& defender) const;

  /// What the calculator would answer for one blow of `attacker` on
  /// `defender` at `now`, drawn on a copy of the world's generator: nothing
  /// is changed but the match cache, which it fills as a blow would.
  [[nodiscard]] BlowOutcome preview(World& world, ObjectId attacker, ObjectId defender,
                                    GameTime now);
  /// The calculator's last answer to the dealer: what a test reads to see
  /// which gate returned and which special fired. Not state.
  [[nodiscard]] const BlowOutcome& last_blow() const noexcept { return last_blow_; }

  /// Apply damage directly, as the host's `Obj::Damage(n)` does. Returns the
  /// health actually removed. Kills through `enter_dying`.
  ///
  /// **Takes the world because a kill fires the class's `ondie` hook**, and the
  /// hook needs a launcher this system does not hold. `Obj::Damage(n)` is the
  /// head of the original's `Damage -> SetHealth -> IsDead -> vtbl[0xB0]`
  /// chain, and `vtbl[0xB0]` is where the death virtual fires it, so a script
  /// that damages something to death pays out exactly as a spear does --
  /// `GHOST_SAC_IDLE.VS` kills its ghost this way and nothing else. That is
  /// the opposite of the score and `record_army_attacked`, which are the
  /// blow's alone (`deal`, `strike`); see their notes for why the two differ.
  std::int32_t apply_damage(World& world, ObjectId victim, std::int32_t amount,
                            ObjectId source = kNoObject);
  /// A unit's death virtual (`vtbl+0xb0`, 0x005db270) with no blow behind it:
  /// the health goes to zero (its `SetHealth(0)`, `vtbl+0x98`) and the unit
  /// dies exactly as `apply_damage` kills -- `ondie` included. The wagon's
  /// cargo setter (0x005eb0a0) is the caller: a wagon emptied to zero dies.
  /// False, and nothing done, for an id that is not a live combatant.
  bool kill(World& world, ObjectId id);
  /// `Obj::Heal(n)`, clamped to `maxhealth`.
  std::int32_t heal(ObjectId id, std::int32_t amount);

  [[nodiscard]] std::int32_t health(ObjectId id) const noexcept;
  [[nodiscard]] std::int32_t max_health(ObjectId id) const noexcept;
  [[nodiscard]] bool is_alive(ObjectId id) const noexcept;

  // -- the numbers the original stores on the object ----------------------
  //
  // `Combatant::stats`, read where the original reads the field. Every reader
  // takes the stored number, so a bonus reaches them only once a rebuild has
  // put it there -- `docs/formats/unit-stats.md`.

  /// `[obj+0xdc]`. `Obj::attack` and `Obj::damage` are the same function
  /// over this field (0x005add80), and the blow (0x0050ff30) reads it too.
  [[nodiscard]] std::int32_t attack_of(const Combatant& unit) const;
  /// `[obj+0xe4]` for `slash` and `[obj+0xe8]` for `pierce`; `siege` and `none`
  /// meet no armour at all and get 0, which is `CombatProfile::armour_against`'s
  /// own rule.
  [[nodiscard]] std::int32_t armour_of(const Combatant& unit, DamageType type) const;
  /// `[obj+0xc8]`.
  [[nodiscard]] std::int32_t max_health_of(const Combatant& unit) const;
  /// `[obj+0xcc]`.
  [[nodiscard]] std::int32_t max_stamina_of(const Combatant& unit) const;
  /// `[obj+0xd4]`: what `Obj::range` answers and what `Unit::InRange`
  /// (0x005a7800) measures the far edge of the reach with.
  [[nodiscard]] std::int32_t range_of(const Combatant& unit) const;

  /// The rebuild's first two steps (0x005d9f25..0x005d9fdb): the class's six
  /// numbers, then `Unit::AddBonus`'s record on top. Everything a building
  /// ever has, and the start of every unit's rebuild.
  [[nodiscard]] StatBlock class_stats(const Combatant& unit) const;
  /// The rebuild's last step: store `next` and rescale health and stamina by
  /// the new maximum over the old, truncating (0x005da815..0x005da84a) --
  /// stamina only when the old maximum was above 0, health only when it was
  /// (where the original would divide by zero). The world object's health and
  /// stamina follow at once when the system is world-bound, so a script that
  /// reads `.health` after the call sees the rescaled number.
  void store_stats(Combatant& unit, const StatBlock& next);
  /// Rebuild `id`'s stored stats now: `recalc_bonuses` through the world the
  /// system started with when it is world-bound, otherwise the class and the
  /// record alone (a synthetic system has no hero, item or formation to add).
  void recalc(ObjectId id);

  /// `Unit::AddBonus(attack, slash, pierce, maxhealth, maxstamina)` -- add
  /// five addends to whatever this object already carries. Answers false for
  /// an id this system has no combatant for.
  ///
  /// **Accumulates rather than replaces.** `0x005e0c7b` is the arm the entry
  /// point takes when the table already holds a record for the id, and it adds
  /// each of the five into the stored one; the other arm inserts the record it
  /// built on the stack. Two calls of `+5` leave `+10`, and there is no way to
  /// ask what a single caller contributed -- which is what makes
  /// `remove_bonus` the only way back and its floor the trap it is.
  bool add_bonus(ObjectId id, const StatBonus& delta);

  /// `Unit::RemoveBonus` -- subtract five addends, **then floor each of the
  /// five at zero**, and drop the record when all five have reached it.
  ///
  /// The floor is the original's (`0x005e076c` onwards: `setle` / `dec` /
  /// `and`, five times over) and it is not symmetric with `add_bonus`, which
  /// has no such clamp. So a negative bonus cannot be taken back by subtracting
  /// it: `RemoveBonus(0, 0, 0, 0, 0)` against a record holding `-10` leaves
  /// `0`, not `-10`. **Any** call wipes every negative addend on the object.
  ///
  /// **Implemented and deliberately not bound.** No script in the installation
  /// calls it, so binding it would widen `declare_shipped_surface`'s declared
  /// surface, which Part 5 does not do. It is here because it is what makes
  /// the record a quantity that can go back down rather than a one-way
  /// accumulator, and because the asymmetry above is a fact about the entry
  /// point that is worth holding rather than rediscovering.
  bool remove_bonus(ObjectId id, const StatBonus& delta);

  /// Events from the most recent `advance`, in the order they happened.
  [[nodiscard]] std::span<const CombatEvent> events() const noexcept { return events_; }
  void clear_events() noexcept { events_.clear(); }

  /// How long the dying state lasts for an object whose entity has no death
  /// animation to measure. **INVENTED**, and now only a fallback.
  ///
  /// The dying state lasts **one cycle of the entity's slot-9 (`die`)
  /// animation** -- `death_duration_of` -- and ends with the object leaving
  /// the world. The reading and its evidence:
  ///
  /// * the death strips are the corpse. 145 of the 148 unit entities declare
  ///   slot 9, and its tail rows are held far longer than any motion frame --
  ///   `GHORSEMAN`'s is `…, 180, 500, 1000, 1000, 2000, 3000` of 8,800 ms, the
  ///   Briton infantry's `…, 500, 1000, 2000, 4000, 4000, 4000` of 16,424 --
  ///   and the rows drawn under those holds are a body lying on the ground,
  ///   then bones, then almost nothing (`GHORSEMAN_DEATH` row 19 is under a
  ///   sixth of row 15's pixels). A corpse that outlived the animation would
  ///   have nothing left to show;
  /// * the dumps hold the object for that long. The one battle dump has 14
  ///   objects in `action=8`, and the time since each one's own last attack
  ///   is 1.1 to 14.4 s: inside a 16 s death, and impossible under a 1 s one
  ///   unless every one of them died in the last second.
  ///
  /// What is **not** evidenced: that the original frees the object at the
  /// exact end of the strip rather than some fixed time after. Nothing in the
  /// data names a separate time, and freeing it later would show nothing.
  static constexpr GameTime kDefaultDeathDuration = 1000;
  void set_death_duration(GameTime ms) noexcept { death_duration_ = ms > 0 ? ms : 1; }

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
  /// **Written:** the combatants, the projectiles in flight, the pending event
  /// list, the alliance overrides that `set_allied` records, `death_duration_`, `world_bound_` and `now_`. The
  /// events are state rather than a log: a caller drains them with
  /// `clear_events`, so an undrained event is work still owed.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** `CombatConstants`, the class graph, the counter table
  /// and mode, and the `PlayerTable` pointer -- all rebuilt by whoever built
  /// this system, from the same game data. Nor the bloodlust, a process
  /// global (`set_process`), which a load leaves as it was. Nor `profiles_`, which is a lazily
  /// filled cache of the class graph and would be a second copy of game data
  /// that could disagree with the game data.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

  /// `Unit::Mutate`'s half of the replacement: this combatant is now one of
  /// `to`, **without being replaced**.
  ///
  /// The original mints a fresh object and copies `[obj+0x180]` -- experience
  /// -- into it, so a mutated unit keeps what it has learnt and a combatant
  /// built from scratch would not. What the fresh object *does* lose is
  /// everything the destroyed one was in the middle of and every bonus an
  /// effect had put on it, because effects belong to the object that is gone:
  /// the target, the action, the swing, the record and the modifier fields are
  /// reset here, and `health`, `stamina`, `owner`, `position`, `experience`
  /// and `level` are left alone. The stats are rebuilt for the new class, as
  /// the fresh object's are (0x005deef0 calls the rebuild).
  ///
  /// False when no combatant answers to `id`. See `sim/world_host.cpp`.
  bool reclass(ObjectId id, ClassIndex to);

 private:
  [[nodiscard]] Combatant* mutable_find(ObjectId id) noexcept;

  /// The four hard rejects every variant of the family shares, read off
  /// `0x005db650`: the candidate is alive and not dying, it is an enemy, its
  /// class allows being attacked, and it is not `target_factor < 0` -- and,
  /// asked first there, the attacker's `CanAttack` (`unit_can_attack`). Split
  /// out so that the sweeping form and `best_target_among` cannot drift.
  [[nodiscard]] bool targetable(const Combatant& self, const Combatant& other) const;
  /// `auto_target`'s score: `(distance^2 + 1) * target_factor /
  /// target_priority`, always measured from the attacker's own position.
  /// Smallest wins. This engine's, not the original's.
  [[nodiscard]] std::int64_t target_score(const Combatant& self,
                                          const Combatant& other) const noexcept;

  /// The family's one search: `best_target`, `acquire`, `best_target_in_list`
  /// and `best_target_among` are this with different candidates and modes.
  /// `commit` stores every danger decay; `raise` raises a unit answer's.
  struct Search;
  ObjectId search(ObjectId id, const TargetFilter& filter,
                  std::span<const ObjectId> list, bool use_list, bool commit, bool raise);
  /// The current game time for a counter: the world's clock when there is
  /// one, else the time this system last acted at.
  [[nodiscard]] GameTime clock() const noexcept;
  void act(World& world, Combatant& unit, GameTime now);
  void strike(World& world, Combatant& attacker, Combatant& defender, GameTime now);
  void launch(World& world, Combatant& attacker, const Combatant& defender, GameTime now);
  /// The missile's start (`vtbl+0x2c`, 0x004d9a40), which both shooters run
  /// as soon as the arrow is built: the synchronised draws its look takes,
  /// two for the offset of its drawn end when `target` is a building, then
  /// one for its animation variant. Only the draws are kept; what they pick
  /// is presentation.
  void start_missile(World& world, ObjectId target);
  void land(World& world, const Projectile& shot, GameTime now);
  void land_on_ground(World& world, const Projectile& shot, GameTime now);
  /// One side of a blow, read off the world as the calculator reads its
  /// object (`BlowSide`). `params` is the match cache, already filled.
  [[nodiscard]] BlowSide blow_side(World& world, const Combatant& who,
                                   const DamageFormulaParams& params, GameTime now) const;
  /// Take the world's stamina into `who`: the one stamina the original has.
  /// Hero skills (Charge, Rush, Revitalize) write the world's copy alone.
  void pull_stamina(const World& world, Combatant& who) const;
  /// Store `value` as both copies of `id`'s stamina (`SetStamina`, 0x005d2960).
  void store_stamina(World& world, ObjectId id, std::int32_t value);
  /// The object's health virtual `vtbl+0xac` (0x004d6690) with `delta`: a
  /// negative one is the damage routine 0x005a7720 (clamped to the health
  /// left, stored, and an object now dead through its death virtual); zero or
  /// more is a heal capped at the maximum, through the health store
  /// (0x005d3a80, which lifts the disease at full health). Answers whether
  /// the object died of it.
  bool take_health(World& world, ObjectId id, std::int32_t delta, GameTime now);
  /// The dealer 0x00511550: the training blow, or the calculator and then
  /// D5..D11 (`docs/formats/blow.md`, Part 3). `attacker` and `defender` are
  /// live combatants.
  void deal(World& world, ObjectId attacker, ObjectId defender, GameTime now);
  /// The training blow (0x00511550's first branch, 0x00511621 .. 0x0051177c):
  /// between two units that are not enemies, the attack less the target's
  /// slash armour scaled by the level difference, at least the floors and
  /// at most what leaves the target a tenth of its maximum; nothing more at
  /// 0, else one point of experience on a draw under `TrainingExpChance`
  /// and the damage dealt.
  void training_blow(World& world, Combatant& attacker, Combatant& defender, GameTime now);

  /// Enter the death animation, and fire the class's `ondie` hook.
  ///
  /// The hook goes here rather than at the removal three lines into `advance`'s
  /// drain, because the original fires it from the death virtual -- the moment
  /// health reaches zero -- and leaves the object standing as a corpse
  /// afterwards. A hook fired at removal instead would run a whole death animation
  /// later, against a world the battle had moved on from, and
  /// `DRUID_ONDIE.VS`'s mass heal reads `ObjsInCircle(.pos, .sight)`.
  void enter_dying(World& world, Combatant& unit, GameTime now);
  /// The rest of a siege engine's death, after its hook: built, and every
  /// crewman inside dies with it. See the definition for the evidence.
  void die_as_engine(World& world, ObjectId id, GameTime now);
  /// How long `unit`'s dying state lasts: its entity's `die` cycle, or
  /// `death_duration_` when there is none. See `kDefaultDeathDuration`.
  [[nodiscard]] GameTime death_duration_of(const World& world, const Combatant& unit) const;
  /// The swing: turn `attacker` towards `defender` and start one cycle of its
  /// `attack` animation, as of the blow's own instant `now`. See the
  /// definition for the evidence.
  void show_swing(World& world, const Combatant& attacker, const Combatant& defender,
                  GameTime now) const;
  /// Hand a unit that has stopped swinging back to its idle pose through the
  /// entity's `toidle` transition.
  void settle_swing(World& world, const Combatant& unit, GameTime now) const;
  /// What a kill earns its killer (0x00511976..0x005119e0): `levels / 6 + 1`
  /// on the two running totals' levels, 1..9, scaled by the owner's
  /// experience modifier, and pooled for the killer's hero too. Zero unless
  /// both are units and the victim is no `Sentry`. `victim_experience` is the
  /// victim's total as the blow found it.
  [[nodiscard]] std::int32_t kill_gain(World& world, ObjectId attacker, ObjectId victim,
                                       std::int32_t victim_experience);
  /// Pay `amount` into `attacker`'s running total, held at its owner's
  /// `max_battle_level` unless it is a hero or training (0x005119f3).
  void pay_experience(World& world, ObjectId attacker, std::int32_t amount);

  CombatConstants constants_{};
  const ClassGraph* graph_ = nullptr;
  CounterTable counters_;
  CounterMode counter_mode_ = CounterMode::counter_bonus;

  /// Sorted by id; iteration order is registration order, which is spawn order.
  std::vector<Combatant> units_;
  std::vector<Projectile> shots_;
  std::vector<CombatEvent> events_;
  bool fresh_deaths_ = false;

  /// Sorted by `class_index`. `mutable` so `profile()` can fill it lazily
  /// without forcing every caller to be non-const; the cache is derived from
  /// the class graph and is not world state, so it is not hashed.
  mutable std::vector<CombatProfile> profiles_;
  /// The calculator's match cache and its addend: derived, not saved, not
  /// hashed (`damage_formula_params`).
  DamageFormulaParams params_{};
  BlowOutcome last_blow_{};
  std::int32_t favoured_addend_ = 0;
  /// The process globals (`set_process`), and the ones used when none was
  /// given. Neither is serialized or hashed: the original's bloodlust is a
  /// process global that no save writes and no load resets.
  ProcessGlobals* process_ = nullptr;
  ProcessGlobals own_process_{};
  /// Sorted by player id. Turn state: research writes it mid-game.
  std::vector<std::pair<PlayerId, std::int32_t>> experience_modifiers_;
  /// Sorted pairs, low player first.
  std::vector<std::pair<PlayerId, PlayerId>> allies_;
  /// The world's diplomacy, or null before `start`. Not owned.
  const PlayerTable* players_ = nullptr;
  /// The world `start` saw, or null. Not owned, not state: `unit_can_attack`
  /// reads the object model (unit or building, the settlement's centre)
  /// through it.
  World* world_ = nullptr;

  GameTime death_duration_ = kDefaultDeathDuration;
  bool world_bound_ = false;
  /// The game time the system last acted at, so that a host call arriving
  /// between events timestamps its own effects consistently.
  GameTime now_ = 0;
};

// ---------------------------------------------------------------------------
// host functions
// ---------------------------------------------------------------------------

/// `CallContext::user` is a `HostContext*`, for this domain and every other.
///
/// This slice used to expect a `CombatHostContext*` -- system plus object type
/// id -- while movement, economy and the object model expected a bare `World*`
/// and heroes a `HeroHostState*`. Every domain's tests set `user` to the type
/// that domain expected, so all of them passed and no embedder could satisfy
/// two at once. `sim/host_context.hpp` settles it: the host functions reach the
/// world with `world_of`, find their system with `combat_system_of` below, and
/// take the type id handles carry from `HostContext::object_type`.

/// The `CombatSystem` registered with a world, or null.
///
/// Found by `System::name()`, which is "combat" and is documented as stable,
/// over `World::systems()` in registration order. Registration order is part of
/// the simulation's definition, so this is a deterministic lookup and not a
/// search. The same shape as `hero_system_of` and `movement_system`, and for
/// the same reason: a `CombatSystem*` field on `HostContext` would have to be
/// added again for every domain, and kept correct across a save and reload,
/// which a name lookup does not.
[[nodiscard]] CombatSystem* combat_system_of(World& world) noexcept;

/// Where `Catapult::Attack(point)`'s shot leaves the engine for `at`
/// (0x004e22a0): the engine's position plus one of its entity's type-3
/// points, taken raw and in table order, the one at the column the engine is
/// drawn facing `at` -- `(n / 2 + sector) % n` over the entity's `n`
/// variations, the same rule 0x0053db30 draws the engine by, which this
/// engine's `facing_column` stands for. The engine's own position when it
/// has no entity or no such point (the original would read a point it never
/// wrote; no shipped firing catapult is short of one per facing).
[[nodiscard]] Point catapult_shot_origin(const World& world, ObjectId catapult, Point at) noexcept;

/// The object grid's cell, in world units: the `>> 8` of every target sweep
/// (0x005db8c0, 0x005d4cd0).
inline constexpr std::int32_t kTargetSweepCell = 256;

/// The circle the target sweeps walk (0x005db8c0 and 0x005d4cd0), into `out`
/// in their order: objects standing in no holder whose centre is within
/// `radius` of `centre` (squared, inclusive), in the world's grid walk
/// (`SweepOrder::grid`: rows of `kTargetSweepCell` cells top to bottom, cells
/// left to right, each cell's list in the order its objects came in). A
/// hidden object is found only when its owner shares its view with `viewer`
/// (the side mask, `[player+0x18]`, `seeflags`), or is `viewer`. Whose owner
/// bits the caller wants is the caller's test.
void target_sweep(const World& world, Point centre, std::int32_t radius, PlayerId viewer,
                  std::vector<ObjectId>& out);

/// Define the combat slice of the host surface with `HostRegistry::define`.
///
/// Ordered by the call frequencies in `docs/formats/vs-host-api.md`, which is
/// the build order that document asks for: `IsEnemy` (158), `IsValidTarget`
/// (81), `BestTargetInSquadSight` (69), `Damage` (44), `Attack` (40), `attack`
/// (16), `range` (10), `CanAttack` (6), `maxstamina` (6), `SetHealth` (1) and
/// the rest.
///
/// **This slice deliberately does not claim twelve entry points it could
/// answer.** `health`, `maxhealth`, `stamina`, `IsAlive`, `IsDead`, `radius`
/// and `sight` belong to the object model, which answers them for every object
/// rather than only for registered combatants; `level`, `inherentlevel`,
/// `experience`, `SetLevel` and `SetExperience` belong to heroes, which owns
/// unit progression and the level floor a hero's Discipline puts under its
/// army. `HostRegistry::define` replaces silently, so two domains claiming one
/// entry point is a divergence that no single-domain test can see; the reasons
/// per entry point are at the foot of `sim/combat.cpp`, the fixed order is in
/// `sim/host_setup.hpp`, and `engine/tests/test_host_setup.cpp` asserts no
/// domain loses an entry to a later one.
///
/// Entries are `define`d, not `declare`d, so `HostRegistry::implemented()`
/// counts them and a script reaching one that is still missing traps with its
/// name rather than getting a plausible zero.
void define_combat_host(script::HostRegistry& registry);

}  // namespace imperivm::core::sim
