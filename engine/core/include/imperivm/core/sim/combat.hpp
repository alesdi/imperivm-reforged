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
/// So there are **two armour channels, not one**, and three damage types plus a
/// null one: `damage_type` takes `slash` (the `Unit` default), `pierce` (the
/// `Ranged` default), `siege` (catapults and rams) and `none` (peasants,
/// wagons, wildlife) across the 30 classes that declare it. There is no
/// `armor_siege` anywhere in the corpus, so **siege damage meets no armour at
/// all** — that is a property of the shipped data, not a simplification here.
///
/// `armor_slash` and `armor_pierce` are declared on 102 classes each and are
/// *equal in every one of them*. The channels are therefore real in the engine
/// (the executable carries `armor_pierce_percent` and `armor_slash_percent`
/// too, which no shipped class uses) and unexercised in the retail balance.
///
/// ## The damage pipeline, and what fixes its order
///
/// `DATA\CONST.INI` `[GamePlay]` supplies the arithmetic constants:
///
///     MinPercentOfAttackersDamage = 20
///     MinDamage = 3
///     LevelDifferenceDamageTable = 1,5 5,20 10,40 20,60 40,80 100,90
///     DamageAmplify = 200                     ; "Damage in settlements"
///     ChargeDamageFactor = 8
///     FullArmor = 8                           ; see below — dead
///
/// `DATA\UNIT_SPECIALS.INI` then describes the pipeline in prose, and two of
/// its entries pin the order down:
///
///   * **Expertise** — "Instantly kills an enemy when the warrior damage is
///     increased *after adjustments with enemy armor and level modifier*".
///     Armour and the level modifier are two separate, ordered adjustments,
///     armour first.
///   * **Charge** — "The warrior deals 8 times his normal damage *(after enemy
///     armor is applied)*". Flat multipliers sit downstream of armour.
///
/// and the additive terms are named by their own descriptions: *Offensive
/// tactics* "adds the warrior's level to his damage", *Defensive tactics*
/// "adds the warrior's level to his armor", *Attack skill* "increases the
/// warrior's damage by twice his current stamina", *Penetration* "ignores the
/// enemy armor". Damage and armour are plain integers that things add to.
/// `DATA\SKILLS.INI` says the same for heroes: Team Attack "increases the
/// damage of every attached warrior by 1 per skill point"
/// (`AttackPerTeamAttackLevel = 1`), Team Defense the armour
/// (`DefensePerTeamDefenseLevel = 1`).
///
/// So `resolve_damage` runs, in order:
///
///     1. attack        = damage + additive bonuses            (caller-supplied)
///     2. after armour  = attack - armour                       [Expertise]
///     3. floor         = max(after armour, attack * 20 / 100)  [MinPercentOfAttackersDamage]
///     4. counter unit  = * (100 + strength) / 100               INFERRED, see below
///     5. level         = * (100 +/- table(|dL|)) / 100          [Expertise]
///     6. multiplier    = * multiplier_percent / 100            [Charge]
///     7. settlement    = * DamageAmplify / 100                 [DamageAmplify]
///     8. final         = max(result, MinDamage)                [MinDamage]
///
/// Steps 1-3, 5-8 are the shipped constants applied where the shipped text puts
/// them. **Step 4 is the inference in this file** and is switchable; see
/// `CounterMode`. The *placement* of step 4 (a multiplier, therefore after
/// armour, alongside Charge) is inference even if the multiplier itself is
/// granted.
///
/// Division is integer and truncating throughout, on non-negative values, so it
/// is floor division and identical on every target. No floating point appears
/// anywhere in this header or its implementation.
///
/// ### `FullArmor = 8` is dead
///
/// It is the obvious candidate for a percentage-of-armour rule and it is not
/// used: the string `FullArmor` does not occur anywhere in `gbr.exe`, while
/// `MinDamage`, `MinPercentOfAttackersDamage`, `LevelDifferenceDamageTable`,
/// `DamageAmplify` and `ChargeDamageFactor` each occur exactly once. A model
/// built on it would be building on a constant the shipped engine never reads.
/// Armour here is subtractive, floored by `MinPercentOfAttackersDamage`, which
/// is what a constant literally named "minimum percent of the attacker's
/// damage" is for. That reading is not proven, but it is the only one that
/// gives every surviving constant a job.
///
/// ## Morale: there is none
///
/// Asked for, and the data refuses it. `gbr.exe` contains no occurrence of
/// `morale`, `flee`, `panic`, `rout` or `courage`; `CONST.INI` declares no
/// morale constant; no class in the graph declares a morale property; and the
/// desync dumps' unit block (docs/engine/state-vector.md) has no morale field.
/// The one field that looks like it — `danger`, 0..6 — belongs to spatial
/// threat assessment: the executable prints it as `Danger %d, Foe danger %d`
/// alongside `lastdangertime` and `Min of MaxDangers`, which is pathfinding
/// telemetry. **Imperivm units fight to the death and this file models that.**
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
/// **Not excluded here:** `SyncFlags` bit 26. `IsValidTarget` rejects a
/// candidate *or* an attacker carrying `0x04000000` outright (`0x005b1f38` and
/// `0x005b1f4b`), which settles `docs/engine/state-vector.md`'s inference that
/// bit 26 marks an ambient civilian -- 140 objects, `*VillagerAmbient` and
/// `OldManE`. `ObjectState` does not carry that bit today (`pack_sync_flags`
/// deliberately does not produce it), so it is recorded here and not acted on.
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

/// `LevelDifferenceDamageTable = 1,5 5,20 10,40 20,60 40,80 100,90`.
///
/// Space-separated `difference,percent` pairs, ascending in both columns and
/// saturating: a level advantage of 100 is worth 90%. Between breakpoints the
/// value is linearly interpolated with integer arithmetic — **INFERRED**; the
/// data supplies six points and no interpolation rule, and a step function
/// would fit them equally well. Below the first breakpoint the modifier is
/// zero, which the table's own first entry (a difference of 1 is worth 5%)
/// makes the only consistent reading.
struct LevelDifferenceTable {
  struct Breakpoint {
    std::int32_t difference = 0;
    std::int32_t percent = 0;

    friend bool operator==(const Breakpoint&, const Breakpoint&) = default;
  };

  static constexpr std::size_t kMaxBreakpoints = 8;

  std::array<Breakpoint, kMaxBreakpoints> points{};
  std::uint8_t count = 0;

  /// The percentage for a *magnitude* of level difference. Sign is the caller's
  /// business: the attacker gains it and the defender's excess costs it.
  [[nodiscard]] std::int32_t percent(std::int32_t difference) const noexcept;

  /// The retail table, compiled in.
  [[nodiscard]] static LevelDifferenceTable shipped() noexcept;

  /// Parse the `CONST.INI` spelling. Returns the shipped table on garbage
  /// rather than an empty one, because an empty table silently disables the
  /// level modifier and that is a worse failure than a stale one.
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
  // `;difficulty level adjustments` is the comment above them, and they are
  // spelled "LevelAddend", so they are an addend on a *level*. Which side
  // receives it is **not** stated anywhere in the data, so this file does not
  // decide: `CombatSystem::set_player_level_addend` takes it per player and
  // defaults to zero, and `difficulty_addend` only reports the number.
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
  // What the comment does *not* say is what event pays `exp_gain`. Per
  // damaging attack is the reading here, on the strength of the `Learning`
  // special — "receives 1 experience *when attacking* a more experienced
  // enemy" — and of the dumps' observed range (experience 0..74 against a
  // level-2 threshold of 175, which a per-kill rate of 1-4 could never fill in
  // a session that reaches level 24). **INFERRED.**
  std::int32_t level_exp_divider = 8;  ///< `LevelExpDivider`
  std::int32_t exp_a = 50;             ///< `ExpA`
  std::int32_t exp_b = 125;            ///< `ExpB`
  std::int32_t exp_t = 1;              ///< `ExpT`
  /// `BattleTacticsExpModifier`, a percentage; the research sets it through
  /// `SetExperienceModifier(player, c)`.
  std::int32_t experience_modifier = 100;

  // -- catapults ------------------------------------------------------
  std::int32_t catapult_base_fire_rate = 40;  ///< `CatapultBaseFireRate`, shots per 10 min
  std::int32_t catapult_add_fire_rate = 28;   ///< `CatapultAddFireRate`, shots per 10 min
  std::int32_t catapult_max_units = 10;       ///< `CatapultMaxUnits`

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

  /// `Exp[level]`, the cumulative experience the recurrence puts a level at.
  /// `Exp[1] = 0`. Saturates rather than overflowing.
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
/// **Read this before trusting any number this file produces about a matchup.**
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

/// Everything one strike needs, flattened.
///
/// Flat and pure on purpose: this is the function the whole system is judged
/// on, so it takes no world, no object, and no hidden state, and a test can
/// state a case in one initialiser.
struct DamageInputs {
  /// `damage` plus every additive bonus the caller has already applied: hero
  /// Team Attack (`AttackPerTeamAttackLevel` per point), Offensive Tactics
  /// (the attacker's level), Attack Skill (twice current stamina).
  std::int32_t attack = 0;
  DamageType type = DamageType::slash;
  /// The defender's armour on the attacker's channel, plus Team Defense
  /// (`DefensePerTeamDefenseLevel` per point) and Defensive Tactics.
  /// Ignored entirely for `siege`, which has no armour channel.
  std::int32_t armour = 0;
  std::int32_t attacker_level = 1;
  std::int32_t defender_level = 1;
  /// `CounterTable::damage_percent`, already resolved. 100 is neutral.
  std::int32_t counter_percent = 100;
  /// Penetration, or a hero's Assault cry: "allows all attached warriors to
  /// ignore enemy armor".
  bool ignores_armour = false;
  /// `DamageAmplify`, the settlement multiplier.
  bool in_settlement = false;
  /// Charge (`ChargeDamageFactor` = 8, so 800), Frenzy ("doubles the damage",
  /// 200), Triple Strike ("triples his damage", 300). 100 is neutral.
  std::int32_t multiplier_percent = 100;
  /// `ObjectFlags::half_damage` on the victim: a unit under a cover of mercy.
  /// `hit` reads it off the world; `inputs_for` has no world and leaves it
  /// clear, so `preview` shows the unsheltered blow.
  bool halved = false;
};

/// Every intermediate, so a test can assert on the step rather than the total
/// and a report can show its working.
struct DamageBreakdown {
  std::int32_t attack = 0;
  std::int32_t armour = 0;
  std::int32_t after_armour = 0;
  std::int32_t after_counter = 0;
  std::int32_t level_difference = 0;  ///< attacker minus defender, signed
  std::int32_t level_percent = 0;     ///< the table's value for its magnitude
  std::int32_t after_level = 0;
  /// Whether the victim's `half_damage` flag took half of what was left.
  bool halved = false;
  std::int32_t final_damage = 0;
  bool floored_by_min_percent = false;  ///< `MinPercentOfAttackersDamage` bit
  bool floored_by_min_damage = false;   ///< `MinDamage` bit
};

/// The pipeline documented at the top of this file. Pure, integral, total.
[[nodiscard]] DamageBreakdown resolve_damage(const DamageInputs& in,
                                             const CombatConstants& constants) noexcept;

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
  /// `projectile_class`, resolved. `kNoClass` for a unit that hits directly.
  ClassIndex projectile = kNoClass;

  bool can_be_attacked = true;
  bool is_projectile = false;

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
/// an equivalence rather than a shortcut.** Nothing in this engine caches a
/// derived stat -- `attack/0` reads `profile.damage + bonuses` on the spot --
/// so there is no rebuild for a record to survive; what the side table buys
/// the original is that the entry dies with the object, and a field on
/// `Combatant` gets that from `reconcile`, which drops the row of any object
/// that has left the world. The original does it explicitly, in the object
/// teardown at `0x005db50f`.
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

/// The combat half of a unit's state vector, in the dumps' own terms.
///
/// The field list is `docs/engine/state-vector.md`'s `CVXUnit` block minus the
/// parts other systems own (`method`, the command queue, the path, `food`,
/// `squad`, `hero handle`, `holder handle`). `danger` is absent because it is
/// pathfinding telemetry, and `damage magic handle` is absent because it is
/// null in all 3,432 unit blocks and its meaning is unknown — a field we cannot
/// explain is left out rather than guessed.
struct Combatant {
  ObjectId id = kNoObject;
  ClassIndex class_index = kNoClass;
  PlayerId owner = kNoPlayer;
  /// World units. Movement owns this; combat reads it and writes it only
  /// through `set_position`.
  Point position;

  std::int32_t health = 0;
  std::int32_t stamina = 0;
  std::int32_t experience = 0;
  /// 1..24 in the dumps.
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

  Action action = Action::idle;
  std::int32_t anim = kAnimIdle;

  /// When the dying animation ends and the object leaves the world. Meaningful
  /// only while `action == Action::dying`.
  GameTime death_time = 0;

  /// The next game time this combatant acts at. Absolute, so that where turn
  /// boundaries fall cannot change what happens: see `CombatSystem::advance`.
  GameTime next_action_time = 0;

  // -- modifiers other systems write ----------------------------------
  //
  // The hero agent owns skills; this owns how they land. Team Attack writes
  // `attack_bonus`, Team Defense `armour_bonus`, Discipline `level_floor`
  // ("increases the level of every attached warrior so it is at least 2 plus
  // the number of skill points", `DisciplineBonusBase` 2 +
  // `DisciplineBonusStep` 1 per point). Assault writes `ignores_armour`,
  // Ceasefire `cannot_fight`, Frenzy/Charge `damage_multiplier_percent`.
  std::int32_t attack_bonus = 0;
  std::int32_t armour_bonus = 0;
  std::int32_t level_floor = 0;
  std::int32_t damage_multiplier_percent = 100;
  bool ignores_armour = false;
  bool cannot_fight = false;
  /// `DamageAmplify` applies to damage dealt inside a settlement. Economy owns
  /// settlements; this is the flag it sets.
  bool in_settlement = false;

  /// `Unit::AddBonus`'s five addends. Reset by `reclass`, for the reason the
  /// four fields above it are: the original mutates by minting a fresh object,
  /// and the record is keyed on the id that goes away with the old one.
  StatBonus bonus;

  bool alive = true;

  /// The level the damage formula sees: `max(level, level_floor)` plus the
  /// owner's difficulty addend, which the system supplies.
  [[nodiscard]] std::int32_t base_effective_level() const noexcept {
    return level > level_floor ? level : level_floor;
  }
};

/// A projectile in flight. `CVXCatapultShot` is a native class in its own right
/// (3 shipped classes: `Gule`, `CGule`, `IGule`) and archers name a
/// `projectile_class` too (`Arrow`, `Javelin`, `Axe`, `Slingstone`,
/// `Big_Arrow`), so a shot is an object with a lifetime rather than an
/// instantaneous hit.
///
/// The arc is not modelled. `shot_tan` (600, 900, 2000) and `shot_height` (100,
/// 150) describe one, and nothing in the data says what units they are in or
/// how they convert to a flight time, so the position here interpolates the
/// ground track and the arc is left to the renderer. **The flight time is
/// INVENTED**: `kProjectileSpeed` world units per second, chosen so a
/// catapult's 800-unit shot takes about a second.
struct Projectile {
  ObjectId id = kNoObject;
  ObjectId shooter = kNoObject;
  ObjectId target = kNoObject;
  ClassIndex class_index = kNoClass;

  Point origin;
  Point aim;      ///< where it is going: the target's position at launch
  Point position; ///< interpolated

  GameTime launched = 0;
  GameTime impact = 0;

  /// Resolved at launch, not at impact: the shooter may be dead by then, and
  /// an arrow already loosed still lands.
  std::int32_t attack = 0;
  DamageType type = DamageType::slash;
  std::int32_t attacker_level = 1;
  std::int32_t splash_radius = 0;
  bool ignores_armour = false;
};

/// INVENTED. World units per second of flight.
inline constexpr std::int32_t kProjectileSpeed = 800;

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
/// ## Which of those five this type can express, and which it cannot
///
/// The two soft variants add **200 to the executable's own score**, and the
/// executable's score (`0x005d3630`) is
/// `target_factor + 40 * recent_target_count + surface_distance` -- a linear
/// sum, not this system's `distance^2 * target_factor / target_priority`. 200
/// on that scale is a nudge worth a couple of hundred world units; 200 on this
/// system's scale is nothing at all. **A soft preference therefore cannot be
/// transplanted**, and `BestTargetInSquadSight/1` is left trapping for that
/// reason and no other -- its rule is known exactly and is not expressible
/// here. `_PreferUndiseased` is answered, because this engine models no
/// disease at all (nothing sets it; `Unit::Disease` and `Unit::IsDiseased` are
/// both unimplemented), so no candidate can carry the penalty and the two
/// entry points return the *same* object rather than a similar one. **The day
/// disease exists, that stops being true** and this filter must gain the
/// preference.
///
/// The three hard filters -- damage, independents, exclusive class -- are exact.
struct TargetFilter {
  /// `BestTargetInSquadSightMisZeroDamage`. Skip a candidate whose own attack
  /// is not strictly positive. Measured against `Obj::attack`, so it is
  /// `profile.damage + attack_bonus`: the number a script reading `.attack`
  /// off the same object sees.
  bool require_armed = false;

  /// `BestNoIndependentTargetInSquadSight`. Skip candidates owned by
  /// `kNeutralWildlife`. Only that player: the mask clears bit 14 alone.
  bool exclude_independents = false;

  /// `BestTargetInSquadSightExclusive`. When not `kNoClass`, a candidate of
  /// any other class is rejected. Exact class, not ancestry: the executable
  /// compares the class object's own name string.
  ClassIndex only_class = kNoClass;

  /// `BestTargetInSquadSight/1`. When not `kNoClass`, a candidate of that class
  /// outranks every candidate that is not one, whatever the two scores say.
  ///
  /// **The one filter here that is a stated approximation.** The two arity-1
  /// entry points reach the same core (0x005dc950) and differ by one argument:
  /// the exclusive flag `only_class` above carries. With it clear, 0x005db774
  /// does not reject a mismatch -- it adds 200 to the candidate's score, and a
  /// *match* that also falls inside `[cand+0xd4]` takes 100 off, or wins
  /// outright when its score is already under 50. So the original prefers by
  /// about 300 on a scale that is `class_base + 40 * threat + surface distance`
  /// (0x005d3630): linear world units, where 300 is a third of a footman's
  /// sight and a preference a nearer wrong-class target can outbid.
  ///
  /// `target_score` here is `distance² * factor / priority`. There is no
  /// exchange rate between the two, which is the standing decision at the head
  /// of the target-selection block in `sim/combat.cpp` -- a 200-point nudge on
  /// the wrong scale is not the same rule. So the preference is taken to its
  /// limit instead of being scaled: a candidate of the named class always beats
  /// one that is not, and the score orders within each group.
  ///
  /// That is exact wherever the original's 300 decides the answer and wherever
  /// no candidate matches at all. It differs inside the band where a closer
  /// wrong-class target outbids the preference -- the original takes that one,
  /// this does not. Both shipped callers are `..._ENGAGE_UNIT_TYPE.VS`, which
  /// exist to make a unit fight the kind of thing it was pointed at.
  ClassIndex prefer_class = kNoClass;

  /// `BestTargetForPos`. Keep only candidates the attacker could strike from
  /// where it stands. The executable applies this as `surface_distance` inside
  /// `[min_range, range]` (`0x005db820`); `in_attack_range` is that test.
  bool require_in_attack_range = false;

  /// `BestTargetInRange`. Sweep a circle around `centre` of radius `radius`
  /// instead of around the attacker out to its `sight`. Scoring still measures
  /// from the attacker: `0x005dd060` passes the point as the sweep centre and
  /// the attacker's own position as the predicate's origin.
  bool centre_on_point = false;
  Point centre{};
  /// Zero or less means the attacker's `sight`, which is what `0x005dd118`
  /// substitutes.
  std::int32_t radius = 0;
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
/// them by binary search on id. It reaches the world only to spawn and despawn
/// projectile objects, so that a shot occupies a handle in the same monotonic
/// space as everything else — which the dumps say it must.
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

  /// The difficulty addend for one player's units, applied to the level the
  /// damage formula sees. Zero by default; see `CombatConstants`.
  void set_player_level_addend(PlayerId player, std::int32_t addend) noexcept;
  [[nodiscard]] std::int32_t player_level_addend(PlayerId player) const noexcept;

  /// `SetExperienceModifier(player, percent)` -- the *Battle tactics* research,
  /// which `ONFINISH_RESEARCH.VS` completes with `GetConst(
  /// "BattleTacticsExpModifier")`.
  ///
  /// **Per player**, because `gbr.exe` 0x005d2fb0 writes it into the player
  /// record at `+0x84` and not into a global -- and therefore turn state, so it
  /// is saved and hashed, unlike `level_addends_` beside it which match setup
  /// writes once. A player nobody has set reads `CombatConstants::
  /// experience_modifier`, which is the same 100 the constants ship.
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
  /// Drop the current target and go idle.
  bool stop(ObjectId id);
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

  /// The target `id` would pick right now, or `kNoObject`.
  ///
  /// Scoring is `distance^2 * target_factor / target_priority`, smallest wins,
  /// ties broken by lower object id. The *directions* are evidenced (see
  /// `CombatProfile`); combining the two properties as a ratio is **INFERRED**,
  /// and is now known to be wrong -- see the note above `TargetFilter`.
  /// Candidates must be enemies, alive, attackable, not `target_factor < 0`,
  /// and within the attacker's `sight`.
  [[nodiscard]] ObjectId best_target(ObjectId id) const;

  /// The same acquisition under one of the variant filters. See `TargetFilter`.
  [[nodiscard]] ObjectId best_target(ObjectId id, const TargetFilter& filter) const;

  /// The plain filter's eligibility and score over an **explicit** candidate
  /// set, and **with no reach at all**.
  ///
  /// `Unit::BestTargetInGAIKA` is the one member of the family that does not
  /// sweep a circle: `0x00431090` hands the search object one candidate at a
  /// time out of the squads standing in a GAIKA node, so the radius every other
  /// variant gets from `0x005dd060`'s grid query is simply never applied and a
  /// target on the far side of the node is as eligible as one underfoot. The
  /// tie rule differs for the same reason -- there is no ascending sweep to
  /// give "lowest id", so the **first candidate offered** keeps a tie, which is
  /// node order, then squad order, then join order.
  ///
  /// `sim/ai.cpp` is the only caller; it lives there because the candidate set
  /// is GAIKA's, not combat's.
  [[nodiscard]] ObjectId best_target_among(ObjectId id,
                                           std::span<const ObjectId> candidates) const;

  /// Whether `attacker` can strike `defender` from where they stand: squared
  /// distance between `min_range^2` and `(range + both radii)^2`. Comparing
  /// squared distances keeps it integral — no square root, no rounding.
  [[nodiscard]] bool in_attack_range(const Combatant& attacker, const Combatant& defender) const;

  /// What one strike would do, without doing it.
  [[nodiscard]] DamageBreakdown preview(ObjectId attacker, ObjectId defender) const;

  /// Apply damage directly, as the host's `Obj::Damage(n)` does. Returns the
  /// health actually removed. Kills through `enter_dying`.
  ///
  /// **Takes the world because a kill fires the class's `ondie` hook**, and the
  /// hook needs a launcher this system does not hold. `Obj::Damage(n)` is the
  /// head of the original's `Damage -> SetHealth -> IsDead -> vtbl[0xB0]`
  /// chain, and `vtbl[0xB0]` is where the death virtual fires it, so a script
  /// that damages something to death pays out exactly as a spear does --
  /// `GHOST_SAC_IDLE.VS` kills its ghost this way and nothing else. That is
  /// the opposite of `record_score` and `record_army_attacked`, which are
  /// `hit`-only on purpose; see their notes for why the two differ.
  std::int32_t apply_damage(World& world, ObjectId victim, std::int32_t amount,
                            ObjectId source = kNoObject);
  /// `Obj::Heal(n)`, clamped to `maxhealth`.
  std::int32_t heal(ObjectId id, std::int32_t amount);

  [[nodiscard]] std::int32_t health(ObjectId id) const noexcept;
  [[nodiscard]] std::int32_t max_health(ObjectId id) const noexcept;
  [[nodiscard]] bool is_alive(ObjectId id) const noexcept;

  // -- the numbers the original caches on the object ----------------------
  //
  // Five reads that exist so that `Unit::AddBonus`'s record reaches every
  // caller of the number it modifies. The original has no such problem: it
  // writes the sum into `[obj+0xc8]`, `[obj+0xcc]`, `[obj+0xdc]`, `[obj+0xe4]`
  // and `[obj+0xe8]` and every reader takes it from there. This engine derives
  // each on the spot, so the addition has to live in one place per number or
  // it will live in six and drift.

  /// `[obj+0xdc]`: the class's `damage`, plus `attack_bonus`, plus the record.
  /// `Obj::attack` and `Obj::damage` are the same function over this field.
  [[nodiscard]] std::int32_t attack_of(const Combatant& unit) const;
  /// `[obj+0xe4]` for `slash` and `[obj+0xe8]` for `pierce`; `siege` and `none`
  /// meet no armour at all and get 0, which is `CombatProfile::armour_against`'s
  /// own rule. `armour_bonus` is included for the same reason `attack_of`
  /// includes `attack_bonus`.
  [[nodiscard]] std::int32_t armour_of(const Combatant& unit, DamageType type) const;
  /// `[obj+0xc8]`.
  [[nodiscard]] std::int32_t max_health_of(const Combatant& unit) const;
  /// `[obj+0xcc]`.
  [[nodiscard]] std::int32_t max_stamina_of(const Combatant& unit) const;

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
  /// list, the per-player level addends, the alliance overrides that
  /// `set_allied` records, `death_duration_`, `world_bound_` and `now_`. The
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
  /// this system, from the same game data. Nor `profiles_`, which is a lazily
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
  /// the target, the action, the swing and the four bonus fields are reset
  /// here, and `health`, `stamina`, `owner`, `position`, `experience` and
  /// `level` are left alone.
  ///
  /// False when no combatant answers to `id`. See `sim/world_host.cpp`.
  bool reclass(ObjectId id, ClassIndex to) noexcept;

 private:
  [[nodiscard]] Combatant* mutable_find(ObjectId id) noexcept;

  /// The four hard rejects every variant of the family shares, read off
  /// `0x005db650`: the candidate is alive and not dying, it is an enemy, its
  /// class allows being attacked, and it is not `target_factor < 0`. Split out
  /// so that the sweeping form and `best_target_among` cannot drift.
  [[nodiscard]] bool targetable(const Combatant& self, const Combatant& other) const;
  /// `(distance^2 + 1) * target_factor / target_priority`, always measured from
  /// the attacker's own position. Smallest wins.
  [[nodiscard]] std::int64_t target_score(const Combatant& self,
                                          const Combatant& other) const noexcept;
  void act(World& world, Combatant& unit, GameTime now);
  void strike(World& world, Combatant& attacker, Combatant& defender, GameTime now);
  void launch(World& world, Combatant& attacker, const Combatant& defender, GameTime now);
  void land(World& world, Projectile& shot, GameTime now);
  void hit(World& world, Combatant& defender, const DamageInputs& inputs, ObjectId source,
           GameTime now);

  /// Credit a landed blow to the two owners' score counters.
  ///
  /// `MatchSystem` owns the counters and `CombatSystem` is their only writer,
  /// which is why this reaches sideways through the world rather than keeping
  /// a tally of its own -- the same shape `record_army_attacked` uses to stamp
  /// a hero's clock. A world with no `MatchSystem` books nothing.
  ///
  /// **`hit` only, never `apply_damage`.** `Obj::Damage(n)` is a script dealing
  /// damage out of nowhere, and the counters in `gbr.exe` are written by the
  /// combat resolver and by the three effect loops; a script's `Damage` reaches
  /// none of them. The same reasoning already keeps `record_army_attacked` out
  /// of `apply_damage`.
  void record_score(World& world, ObjectId source, const Combatant& defender,
                    std::int32_t removed, bool killed) const;
  /// Enter the death animation, and fire the class's `ondie` hook.
  ///
  /// The hook goes here rather than at the removal three lines into `advance`'s
  /// drain, because the original fires it from the death virtual -- the moment
  /// health reaches zero -- and leaves the object standing as a corpse
  /// afterwards. A hook fired at removal instead would run a whole death animation
  /// later, against a world the battle had moved on from, and
  /// `DRUID_ONDIE.VS`'s mass heal reads `ObjsInCircle(.pos, .sight)`.
  void enter_dying(World& world, Combatant& unit, GameTime now);
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
  void award_experience(Combatant& attacker, const Combatant& defender);
  [[nodiscard]] std::int32_t effective_level(const Combatant& unit) const noexcept;
  [[nodiscard]] DamageInputs inputs_for(const Combatant& attacker,
                                        const Combatant& defender) const;

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
  /// Sorted by player id.
  std::vector<std::pair<PlayerId, std::int32_t>> level_addends_;
  /// Sorted by player id. Turn state: research writes it mid-game.
  std::vector<std::pair<PlayerId, std::int32_t>> experience_modifiers_;
  /// Sorted pairs, low player first.
  std::vector<std::pair<PlayerId, PlayerId>> allies_;
  /// The world's diplomacy, or null before `start`. Not owned.
  const PlayerTable* players_ = nullptr;

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
