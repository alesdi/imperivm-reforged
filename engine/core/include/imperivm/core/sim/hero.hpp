#pragma once

/// Heroes, the squads attached to them, their skills, and unit progression.
///
/// This is the mechanic that distinguishes Imperivm from its peers: a unit does
/// not fight for a player, it fights for a *hero*, and the hero's skills are
/// what make it better at it. `CVXUnit :: AttachHero` and
/// `CVXUnit :: DetachHero` both survive as strings in `gbr.exe`, so attachment
/// is a unit-side operation and it is spelled that way here.
///
/// ## Where every number below comes from
///
/// Two shipped files, and nothing invented between them.
///
/// **`DATA\SKILLS.INI`** declares the 25 skills, in the order this file's enum
/// uses, and its descriptions are quantitative:
///
/// > Administration -- *Increases the maximum number of warriors attached to the
/// > hero by 2 per skill point*
/// > Team Attack -- *Increases the damage of every attached warrior by 1 per
/// > skill point*
/// > Quick March -- *Increases the speed of every attached warrior by 5% per
/// > skill point*
/// > Discipline -- *Increases the level of every attached warrior so it is at
/// > least 2 plus the number of skill points*
///
/// **`DATA\CONST.INI`, section `[GamePlay]`** restates every one of those as an
/// engine constant, which is what actually ran:
///
/// ```ini
/// SpeedPercentPerQuickMarchLevel = 5      UnitsPerAdministrationLevel = 2
/// AttackPerTeamAttackLevel = 1            DefensePerTeamDefenseLevel = 1
/// HealthPerEpicEnduranceLevel = 100       DisciplineBonusBase = 2
/// ExpPercentPerLeadershipLevel = 2        DisciplineBonusStep = 1
/// ```
///
/// The two agree on every skill they both mention, which is why `HeroConstants`
/// carries the `CONST.INI` names verbatim: a loader can overwrite the whole
/// struct from the shipped file and the defaults are only there so a synthetic
/// world has the retail numbers without a data load.
///
/// **Where they disagree, it is recorded rather than resolved.** Rush is the one
/// case: `SKILLS.INI` says the health cost is 5 per point, the comment in
/// `DATA\SUBAI\HERO_SKILL_BEHAVIOUR.VS` says 10, and `CONST.INI` says
/// `RushDamage = 6`. The engine constant wins here; see `rush_damage`.
///
/// ## Skill ids
///
/// `HeroSkillId("Battle cry")` maps a name to an id, and `DATA\COMMANDS\HERO.XML`
/// passes exactly the `SKILLS.INI` section name as `param`. The *ordinal* is
/// inferred: `DATA\AI\HEROSKILL DEFAULT.VS` lists all 25 skill script names in
/// precisely the `SKILLS.INI` section order, and the 20 `hs*` constants the
/// shipped scripts use are exactly the 20 sections whose names appear there --
/// `hsAdministration`, `hsEpicAttack`, `hsEgoism`, `hsScout` and `hsEpicArmor`
/// are the five never referenced, and they are the five this enum has no
/// corpus evidence for. Nothing in the data prints a skill's numeric value, so
/// the mapping is consistent with everything and proven by nothing.
///
/// ## Attachment, measured
///
/// `Hero.SC.XML` declares `max_army="50"`. Across the four dumps that contain
/// heroes, 24 heroes have an army and **not one exceeds 50**. Administration
/// raises it by 2 a point, so the cap is
/// `max_army + UnitsPerAdministrationLevel * points`.
///
/// ## A hero's squad *is* its army
///
/// Measured over the same 24: every member of a hero's army prints the same
/// `squad=<n>(<p>)` as the hero, 24 of 24, and the squad's total membership is
/// exactly `1 + army size`, 24 of 24. See sim/squad.hpp.
///
/// ## Level is not experience
///
/// The host API has both `level` and `inherentlevel`, and `MUSHROOM.VS` writes
/// `.SetLevel(.inherentlevel + 1)` -- so `SetLevel` writes the *inherent* level
/// and `.level` reads an effective one that modifiers move. That is what makes
/// the dumps' `level = 24, experience = 74` possible at all:
/// `BONUSSCRIPTS\003 HERO.VS` places a hero with `SetLevel(12)`, and Discipline,
/// Battle Cry and item level bonuses lift the effective level above the earned
/// one without touching experience.
///
/// The map's `Level="L"` is the earned level less one: the unit loader sets
/// the raw counter to the threshold of level `L + 1` (see
/// `GameSession::Impl::apply_authored_attributes`), so a shipped `Level="9"`
/// legionary is level 10 and the 5,125 at `Level="0"` are level 1.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/sim/item.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::script {
class HostRegistry;
}

namespace imperivm::core::sim {

class World;
class HeroSystem;

// --------------------------------------------------------------------------
// skills
// --------------------------------------------------------------------------

/// The 25 hero skills, in `DATA\SKILLS.INI` section order.
///
/// The file's own header says the order is the UI sort order:
///
/// > ;; the order in which skills appear in this file is used for sorting their
/// > ;; icons in the user interface
///
/// and `HEROSKILL DEFAULT.VS` lists the 25 skill scripts in the same order.
enum class HeroSkill : std::uint8_t {
  administration = 0,  ///< +2 max attached warriors per point
  team_attack,         ///< +1 damage to every attached warrior per point
  team_defense,        ///< +1 armour per point
  quick_march,         ///< +5% speed per point
  epic_endurance,      ///< +100 hero max health per point
  discipline,          ///< every attached warrior is at least level 2 + points
  leadership,          ///< +2% experience the hero gives its warriors per point
  epic_attack,         ///< +5 hero damage per point
  battle_cry,          ///< active: +1 level per point for 5 s
  healing,             ///< active: +10 health per point to every warrior
  ceasefire,           ///< active: warriors neither inflict nor take damage
  vigor,               ///< +10%/point chance of 1 extra stamina while eating
  frenzy,              ///< active: health halved, damage x2, both while it lasts
  rush,                ///< active: +1 stamina per point, at a cost in health
  egoism,              ///< the hero drains a random warrior every 5 s
  wisdom,              ///< +5%/point chance of 1 hero exp when a warrior dies
  recovery,            ///< +10%/point chance of 5 extra health while eating
  survival,            ///< +10%/point chance to eat without spending food
  charge,              ///< active: +1 stamina per point
  scout,               ///< +50 hero sight per point
  assault,             ///< active: warriors ignore enemy armour
  epic_armor,          ///< +2 hero armour per point
  concealment,         ///< +2%/point chance to evade an attack
  defensive_cry,       ///< active: +20 armour to every warrior
  euphoria,            ///< +10%/point chance of 3 stamina on a kill
  count,
};

inline constexpr std::size_t kHeroSkillCount = static_cast<std::size_t>(HeroSkill::count);

/// The `SKILLS.INI` section name, which is also the `param` that
/// `DATA\COMMANDS\HERO.XML` passes to `HeroSkillId`.
[[nodiscard]] std::string_view hero_skill_name(HeroSkill skill) noexcept;

/// The `hs*` script constant, for the 20 skills the shipped corpus names. The
/// other five get their constructed spelling; nothing reads them.
[[nodiscard]] std::string_view hero_skill_constant(HeroSkill skill) noexcept;

/// `HeroSkillId(name)`. Accepts the section name and the `hs*` spelling, both
/// case-insensitively. `-1` when the name is not a skill, which is exactly what
/// `VERIFY_HERO_SKILL.VS` tests for.
[[nodiscard]] std::int32_t hero_skill_id(std::string_view name) noexcept;

/// Whether a skill is activated by a command rather than being always on. The
/// eight with a `command =` line in `SKILLS.INI`, which are the eight with a
/// `skill_*` entry in `DATA\COMMANDS\HERO.XML`.
[[nodiscard]] bool hero_skill_is_active(HeroSkill skill) noexcept;

/// The `coststamina` on the skill's command in `DATA\COMMANDS\HERO.XML`: 6 for
/// Battle Cry, Healing, Ceasefire, Charge and Assault; 4 for Frenzy, Rush and
/// Defensive Cry. Zero for a passive skill. `HERO_SKILL_BEHAVIOUR.VS` agrees --
/// `while (.stamina >= 4) // 4 is the minimal skill cost`.
[[nodiscard]] std::int32_t hero_skill_stamina_cost(HeroSkill skill) noexcept;

/// The tunables, named exactly as `DATA\CONST.INI` `[GamePlay]` names them.
///
/// Defaults are the retail values. A loader that reads `CONST.INI` overwrites
/// the struct; nothing here reads a file, because the core cannot.
struct HeroConstants {
  // -- attachment ------------------------------------------------------
  /// `Hero.SC.XML` `max_army="50"`. Not a `CONST.INI` value: it is a class
  /// property, so a modded hero class can raise it.
  std::int32_t default_max_army = 50;
  std::int32_t units_per_administration_level = 2;
  /// `DetachDistance` / `DetachDistanceEngage` -- how far a warrior may stray
  /// from its hero before `HERO_DETACH_BEHAVIOR.VS` cuts it loose. Movement
  /// owns the distance test; this is where the number lives.
  std::int32_t detach_distance = 700;
  std::int32_t detach_distance_engage = 700;
  std::int32_t hero_approach_distance = 400;

  // -- experience ------------------------------------------------------
  /// `exp_gain = target_level / LevelExpDivider + 1`, spelled out in the file.
  std::int32_t level_exp_divider = 8;
  std::int32_t exp_a = 50;
  std::int32_t exp_b = 125;
  std::int32_t exp_t = 1;
  /// The share of a warrior's experience that reaches its hero.
  std::int32_t exp_from_army_divider = 3;
  /// `ArmyBonusExperiencePercent`, and `heroarmyexpgain="40"` on `Hero.SC.XML`.
  /// The same number in two places.
  std::int32_t army_bonus_experience_percent = 40;
  std::int32_t exp_percent_per_leadership_level = 2;

  // -- passive skills --------------------------------------------------
  std::int32_t attack_per_team_attack_level = 1;
  std::int32_t defense_per_team_defense_level = 1;
  std::int32_t speed_percent_per_quick_march_level = 5;
  std::int32_t health_per_epic_endurance_level = 100;
  std::int32_t attack_per_epic_attack_level = 5;
  std::int32_t defense_per_epic_armor_level = 2;
  std::int32_t sight_per_scout_level = 50;
  std::int32_t discipline_bonus_base = 2;
  std::int32_t discipline_bonus_step = 1;
  std::int32_t avoid_chance_percent_per_concealment_level = 2;
  std::int32_t percent_per_wisdom_level = 5;
  std::int32_t percent_per_euphoria_level = 10;
  std::int32_t euphoria_stamina = 3;
  std::int32_t health_per_egoism_level = 20;
  /// The interval in `SKILLS.INI`'s "Once every 5 seconds"; `CONST.INI` has no
  /// constant for it, so this one is from the description alone.
  std::int32_t egoism_interval = 5000;

  /// **Not in `CONST.INI`.** Vigor, Recovery and Survival are described in
  /// `SKILLS.INI` as 10% per point each and have no engine constant, so these
  /// three are the description's numbers and nothing else.
  std::int32_t percent_per_vigor_level = 10;
  std::int32_t percent_per_recovery_level = 10;
  std::int32_t percent_per_survival_level = 10;
  /// `SKILLS.INI`: Recovery restores 5 additional health points.
  std::int32_t recovery_health = 5;

  // -- active skills ---------------------------------------------------
  std::int32_t health_per_healing_level = 10;
  std::int32_t battle_cry_time = 5000;
  std::int32_t levels_per_battle_cry_level = 1;
  std::int32_t defensive_cry_defense = 20;
  std::int32_t defensive_cry_base_time = 1000;
  std::int32_t time_per_defensive_cry_level = 2000;
  std::int32_t assault_base_time = 1000;
  std::int32_t time_per_assault_level = 1000;
  std::int32_t ceasefire_base_time = 1000;
  std::int32_t time_per_ceasefire_level = 1000;
  std::int32_t frenzy_base_time = 1000;
  std::int32_t time_per_frenzy_level = 1000;
  /// **Three sources disagree.** `SKILLS.INI` says Rush costs 5 health per
  /// point, the comment above the Rush branch of `HERO_SKILL_BEHAVIOUR.VS` says
  /// 10, and `CONST.INI` says `RushDamage = 6`. The AI's own weighting in that
  /// script uses 10 (`u.health - nRushing * 10`), but that is a heuristic
  /// estimate, not the effect. The engine constant is the one the engine read,
  /// so 6 it is -- and this comment is here because that choice deserves to be
  /// visible when conformance is checked against a real match.
  std::int32_t rush_damage = 6;
  /// `SKILLS.INI`: Rush and Charge both give 1 stamina per point.
  std::int32_t stamina_per_charge_level = 1;
  std::int32_t stamina_per_rush_level = 1;

  /// The highest a skill goes. **Inferred**: every AI tactic script sets its
  /// goal levels to 10 (`aSkillLevels[j] = 10`), and `HEROSKILL DEFAULT.VS`
  /// caps its own recommendation with `MIN(skill_points + 5, 10)`. Nothing
  /// states a hard limit.
  std::int32_t max_skill_points = 10;
};

// --------------------------------------------------------------------------
// the modifier seam
// --------------------------------------------------------------------------

/// `base + base * percent / 100`, truncating toward zero for a positive base.
///
/// The one arithmetic rule the whole file rests on: "+5% speed per point" is
/// this, three times over for three points, **not** a floating multiply. Rule 1
/// of docs/engine/architecture.md.
[[nodiscard]] constexpr std::int32_t apply_percent(std::int32_t base,
                                                   std::int32_t percent) noexcept {
  return base + static_cast<std::int32_t>(static_cast<std::int64_t>(base) * percent / 100);
}

/// Everything a hero's skills and a unit's items do to that unit.
///
/// **This is the boundary with the combat and movement systems.** Those systems
/// own applying a modifier to a hit or a step; this system owns computing one.
/// Nothing here reaches into them, and nothing there needs to know a skill
/// exists -- it asks `HeroSystem::modifiers_for(unit)` and applies what it gets.
struct UnitModifiers {
  std::int32_t max_health_add = 0;
  std::int32_t max_health_percent = 0;
  std::int32_t damage_add = 0;
  std::int32_t damage_percent = 0;
  std::int32_t armor_slash_add = 0;
  std::int32_t armor_slash_percent = 0;
  std::int32_t armor_pierce_add = 0;
  std::int32_t armor_pierce_percent = 0;
  /// Quick March, and nothing else. Applied as `apply_percent(speed, this)`.
  std::int32_t speed_percent = 0;
  /// Scout, on the hero itself.
  std::int32_t sight_add = 0;

  /// Additive level: item `<bonus level=...>` plus Battle Cry while it lasts.
  std::int32_t level_add = 0;
  /// Discipline's floor. The effective level is
  /// `max(inherent + level_add, level_floor)`, so a level-9 veteran is not
  /// dragged down to Discipline's 2 + points.
  std::int32_t level_floor = 0;

  /// Concealment: the chance in percent that an incoming attack misses.
  /// Combat rolls this against `World::rng()`; this system never rolls for it,
  /// because whether a draw happens is itself synchronised state.
  std::int32_t evade_percent = 0;
  /// Vigor, Recovery, Survival, Euphoria: chances in percent, rolled by the
  /// feeding and combat systems at the moment the event happens.
  std::int32_t vigor_percent = 0;
  std::int32_t recovery_percent = 0;
  std::int32_t survival_percent = 0;
  std::int32_t euphoria_percent = 0;
  /// The experience premium a hero's warriors earn:
  /// `ArmyBonusExperiencePercent + ExpPercentPerLeadershipLevel * points`.
  std::int32_t experience_percent = 0;

  /// Assault, while in effect: the warrior's attacks ignore enemy armour.
  bool ignore_enemy_armor = false;
  /// Ceasefire, while in effect: the warrior neither inflicts nor suffers
  /// damage.
  bool ceasefire = false;
  /// Frenzy, while in effect: `SKILLS.INI` -- "halves the health and doubles
  /// the damage of all attached warriors". The halving is a one-off applied
  /// when the skill fires; the doubling is this flag, which combat reads.
  bool frenzy = false;

  friend bool operator==(const UnitModifiers&, const UnitModifiers&) noexcept = default;
};

// --------------------------------------------------------------------------
// records
// --------------------------------------------------------------------------

/// A unit's own progression and army membership -- the tail of the `CVXUnit`
/// block that belongs to this domain.
struct UnitRecord {
  ObjectId id = kNoObject;
  /// The dump's `hero handle`. `kNoObject` is its `65535`.
  ObjectId hero = kNoObject;
  SquadKey squad;
  /// The dump's `experience`: progress toward the next level, **not** a running
  /// total. See `experience_to_next_level`.
  std::int32_t experience = 0;
  /// What `SetLevel` writes and `inherentlevel` reads. 1 at spawn.
  std::int32_t inherent_level = 1;
  /// `Unit.HasFreedom` -- `UNIT_ATTACH.VS` refuses to attach a unit that has
  /// it, before anything else is checked. Independent villagers and the like.
  bool has_freedom = false;
  /// The dump's `kills`, and it is here because of where it sits: `+0x188` in
  /// the `CVXUnit` block, immediately after `experience` (`+0x180`) and `level`
  /// (`+0x184`), and named by the same serialiser walk at 0x005dd710.
  ///
  /// **Nothing in the simulation reads it.** Its only reader anywhere in
  /// `gbr.exe` is the unit description at 0x006c53b0, which formats
  /// `'%d kills\n%d/%d experience to next level'` -- so veterancy is
  /// experience-driven and this number is beside it rather than behind it. The
  /// engine never increments it either: `+0x188` has four touch sites on a
  /// unit, and they are the reset to 0 at spawn, the `add` inside
  /// `Unit::IncKills`, the serialiser registration and that one format string.
  /// A kill counter that rises only because a mission script says so.
  ///
  /// Kept, saved and hashed all the same, on `ObjectState::traversed_by`'s
  /// precedent: it is authored per-unit state the original persists by name,
  /// and a field two peers could disagree about is a field the hash has to
  /// carry whether or not this engine has grown its reader yet.
  std::int32_t kills = 0;
};

/// A hero's own state on top of `UnitRecord`.
struct HeroRecord {
  ObjectId id = kNoObject;
  SquadKey squad;
  /// Attached warriors, in attach order. Excludes the hero itself; the squad
  /// holds the hero at member 0 and these after it.
  std::vector<ObjectId> army;
  /// Points spent per skill, indexed by `HeroSkill`.
  std::array<std::int32_t, kHeroSkillCount> skills{};
  /// The skills this hero's *class* offers, resolved from the `HeroSkills`
  /// property at registration.
  ///
  /// Fifteen classes declare it and each names exactly five, e.g.
  /// `ImperialRomanHero`: "Administration, Team attack, Team defense, Quick
  /// March, Discipline". The other 99 `CVXHero` classes inherit one of those
  /// fifteen. A class that declares none offers all 25, which is what a
  /// synthetic world with no class graph gets.
  std::array<bool, kHeroSkillCount> offered{};
  /// Points granted but not yet spent. `AvailableSkillPoints`.
  /// `max_army` from the class, resolved at registration. Administration is
  /// added on top and is not folded in here, so a skill change is not a
  /// re-resolution.
  std::int32_t max_army = 50;
  /// Game time at which each timed skill effect ends; 0 for "not in effect".
  /// `SkillInEffect(id)` is `end > now`.
  std::array<GameTime, kHeroSkillCount> effect_until{};

  /// `[hero+0x2b8]` -- when something in this hero's army was last **struck**.
  ///
  /// `Hero::TimePastLastAttack()` (0x0052ee40) is `now - this`, and that is the
  /// whole of its body. The stamp is written by the virtual at `vtbl + 0xa4`,
  /// which `0x005d5f80` calls as `target->slot0xa4(attacker)` from the strike
  /// path -- so `this` inside it is the **victim**. The implementation
  /// (0x005dc1f0) resolves `[victim+0x170]`, which is `Unit::hero`, and writes
  /// the clock there; a victim that is itself a hero (`kSyncHero` on
  /// `[+0x2c]`) writes on itself, and a victim with neither writes nowhere.
  ///
  /// **So it measures being attacked, not attacking**, and the three shipped
  /// readers all want exactly that: `HERO_SNEAK_VERIFY.VS` allows a sneak only
  /// after `> 2500`, `HERO_SMARTMOVE.VS` and `HERO_SNEAK.VS` branch on
  /// `< 2500`, and `HERO_RETREAT.VS` retreats after `> 5000`. A hero sneaks
  /// when nobody is shooting at it.
  ///
  /// Zero means never, and `now - 0` is a large number, which is "long ago" --
  /// the answer all four sites want from a hero that has not been in a fight.
  GameTime army_attacked_at = 0;

  /// `[hero+0x2bc]` -- **which** of this hero's units was struck, written in
  /// the same two instructions as the stamp above (0x005dc23f).
  ///
  /// `Hero::LastAttacker()` (0x0052eeb0) returns it, and the name is a
  /// misnomer in the original: the handle stored is the **victim's**
  /// (`word [victim+8]`), not the attacker's. Kept here because a stamp with no
  /// subject is half a record and because the pair is written by one statement;
  /// `LastAttacker` itself is not bound -- see `register_hero_host`.
  ObjectId army_attacked_unit = kNoObject;

  /// The party's requested and settled final orientations -- `[party+0x54/+0x58]`
  /// and `[party+0x5c/+0x60]`, where the party record is `[hero+0x2b0]`.
  ///
  /// **Two slots, not one, and the entry points do not read what each other
  /// write**: `SetFinalPartyOrientation` writes the *request*,
  /// `HasFinalPartyOrientationRequest` tests it, and
  /// `GetFinalPartyOrientation` reads the *settled* one. Something in the
  /// party-formation code moves the first to the second when a formation
  /// completes, and this engine does not model formation, so the settled slot
  /// is written by nothing and `Get` answers the unset value.
  ///
  /// That is the original's own answer for a hero whose party has not finished
  /// forming, and it is what all three shipped readers are written for:
  /// `dpt = .GetFinalPartyOrientation() - Point(1024, 1024); if (dpt.x != 0 ||
  /// dpt.y != 0)`. The `+1024` bias is the entry point's, applied on the way
  /// out and removed on the way in, and it exists so that "unset" can be zero
  /// in storage while the script sees a point with two positive components --
  /// the language has no nullable point.
  Point party_orientation_request{};
  Point party_orientation{};
};

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

/// The hero, squad and item system.
///
/// Registered with `World::add_system`. Per turn it expires timed skill
/// effects, runs Egoism's drain, and prunes the dead out of armies -- the
/// `.army.ClearDead` that both `HERO_SKILL_BEHAVIOUR.VS` and
/// `HERO_DETACH_BEHAVIOR.VS` open with.
///
/// Everything else here is called *by* other systems and by host functions, not
/// by the turn loop: attachment happens when a command completes, experience
/// when combat resolves a kill, item use when a script says so.
class HeroSystem final : public System {
 public:
  HeroSystem() : items_(nullptr) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "hero"; }

  void advance(World& world, const Turn& turn) override;
  void hash(std::uint64_t& accumulator) const override;
  void start(World& world) override;

  // -- configuration ---------------------------------------------------

  [[nodiscard]] HeroConstants& constants() noexcept { return constants_; }
  [[nodiscard]] const HeroConstants& constants() const noexcept { return constants_; }
  void set_constants(const HeroConstants& value) noexcept { constants_ = value; }

  [[nodiscard]] ItemStore& items() noexcept { return items_; }
  [[nodiscard]] const ItemStore& items() const noexcept { return items_; }
  [[nodiscard]] SquadTable& squads() noexcept { return squads_; }
  [[nodiscard]] const SquadTable& squads() const noexcept { return squads_; }

  // -- registration ----------------------------------------------------
  //
  // Objects are spawned by `World`; this is how they are made known here. A
  // world populated from a map registers every unit; a synthetic test registers
  // the two it cares about.

  /// Track a unit. Idempotent -- re-registering leaves the record alone, so
  /// `start()` can sweep a populated world without erasing progress.
  UnitRecord& register_unit(World& world, ObjectId id);

  /// Track a hero, and give it a squad of its own.
  ///
  /// A hero always has a squad: all 13 heroes across the four dumps print a
  /// non-zero `squad=<n>(<p>)`, including the ones with no army at all. The
  /// index comes from `SquadTable::create`, and `max_army` is read from the
  /// class graph's `max_army` property, falling back to
  /// `HeroConstants::default_max_army`.
  HeroRecord& register_hero(World& world, ObjectId id);

  /// Forget an object: detach it, take it out of its squad, destroy its items.
  /// A hero's army is disbanded, not inherited.
  void forget(World& world, ObjectId id);

  [[nodiscard]] const UnitRecord* unit(ObjectId id) const;
  [[nodiscard]] UnitRecord* unit(ObjectId id);
  [[nodiscard]] const HeroRecord* hero(ObjectId id) const;
  [[nodiscard]] HeroRecord* hero(ObjectId id);
  [[nodiscard]] bool is_hero(ObjectId id) const { return hero(id) != nullptr; }

  [[nodiscard]] std::span<const UnitRecord> units() const noexcept { return units_; }
  [[nodiscard]] std::span<const HeroRecord> heroes() const noexcept { return heroes_; }

  // -- attachment ------------------------------------------------------

  /// `Unit.AttachTo(hero)` -- the unit-side operation `CVXUnit :: AttachHero`
  /// names.
  ///
  /// Refuses, in the order `DATA\SUBAI\UNIT_ATTACH_VERIFY.VS` checks:
  /// `unit == hero`, either side unknown, a dead hero, a unit with
  /// `HasFreedom`, an enemy (different owner), or a full army. Re-attaching to
  /// the hero a unit is already with succeeds and changes nothing, which is
  /// what the verify script's `if (.hero == hero) return false` implies.
  ///
  /// Note what is *not* refused: a hero attaching to another hero. Both the
  /// attach script and its verifier have that check present and **commented
  /// out** (`//if (this.AsHero.IsValid()) return;`), so the shipped build
  /// allowed it.
  bool attach(World& world, ObjectId unit, ObjectId hero);

  /// `Unit.DetachFrom(hero)`. Also the path `HERO_DETACH_BEHAVIOR.VS` takes
  /// when a warrior strays past `DetachDistance`.
  bool detach(ObjectId unit);

  /// `Hero.DetachArmy()` -- `HERO_LEAVE_ARMY.VS` is nothing but this call.
  /// Every warrior leaves; the hero keeps its squad.
  std::size_t detach_army(ObjectId hero);

  // -- death ---------------------------------------------------------------
  //
  // `gbr.exe`'s unit death virtual (0x005db270, `CVXUnit` vtable slot +0xb0)
  // runs synchronously from the damage that killed the unit, and in this
  // order: the Wisdom roll; an unidentified list removal (0x00518c40);
  // `SetHealth(0)`; the shared routine 0x005141b0, which launches `ondie`
  // (synchronously; see `sim/hooks.hpp`); a unit in a holder is erased there
  // and then; `DetachHero` (0x005db190); `SetPath(null)`; the AI unregister
  // (0x0041eee0 -> 0x004448a0), which clears the squad handle and takes the
  // unit out of its squad; and last the base death, which is the dying state.
  // A hero's override (0x0052fbb0) runs 0x005141b0 first, then `DetachArmy`
  // (0x0052fb00), then the unit path. The two calls below are the parts of
  // that sequence this system owns; `CombatSystem::enter_dying` is the
  // sequence.

  /// The death virtual's first step. When the dying unit's hero has put points
  /// into Wisdom, draw `[0, 99]` from the world RNG and, when
  /// `PercentPerWisdomLevel * points` is greater than the draw, credit the hero
  /// one point of experience.
  ///
  /// **The draw is synchronised state and it is in the original's position**:
  /// before the hook, before anything else the death does, and only when the
  /// unit has a hero whose Wisdom is above zero -- no hero, no draw.
  ///
  /// **The credit is INFERRED to be experience, and one point of it.** The
  /// original adds the global at `0x821298` to `[hero+0x2c4]`. That global is
  /// `ExpFromArmyDivider`, lazily read from `CONST.INI` (0x00530190), and
  /// `[hero+0x2c4]` is the accumulator 0x0052fc50 drains into the hero's
  /// experience at one point per divider -- so the credit is worth exactly one
  /// point, which is also what `SKILLS.INI` promises. This engine's army share
  /// (`add_experience`) has no accumulator and drops the remainder on the
  /// spot, so the point is paid at once; when the original converts is not
  /// read.
  void roll_wisdom(World& world, ObjectId unit);

  /// Everything the death virtual undoes after the hook has run: a hero's
  /// army is detached (`DetachArmy`), the unit is detached from its hero, and
  /// it leaves its squad -- `UnitRecord::squad` becomes `kNoSquad`, and an
  /// emptied squad is dropped. All 15 dying units across the nine dumps print
  /// `hero handle 65535` and `squad=0(0)`, which is this.
  ///
  /// **Nothing for a unit in a holder.** The original erases a held unit on the
  /// spot rather than letting it die (step five above), and the erase does the
  /// detaching; see `on_erase`, which `CombatSystem` reaches when the corpse
  /// leaves the world.
  ///
  /// A hero's `HeroRecord::squad` is cleared too: the squad is gone, and a key
  /// left behind would name whichever squad `SquadTable::create` hands that
  /// index to next. INFERRED from the AI unregister clearing the unit's squad
  /// handle, which for a hero is the only squad handle it has.
  ///
  /// Idempotent: a second call finds nothing attached and does nothing, which
  /// is what the original's erase does when it runs the same detaches over a
  /// corpse that already went through them.
  void on_death(World& world, ObjectId id);

  /// The same detaches, from the erase path (0x005db230), and **without** the
  /// holder exception: an erase detaches whatever it erases.
  void on_erase(World& world, ObjectId id);

  /// `Hero.IsHeroArmyFull()`.
  [[nodiscard]] bool army_full(ObjectId hero) const;
  /// `Hero.HasArmy()`.
  [[nodiscard]] bool has_army(ObjectId hero) const;
  /// The cap: `max_army + UnitsPerAdministrationLevel * Administration points`.
  [[nodiscard]] std::int32_t max_army(ObjectId hero) const;
  [[nodiscard]] std::int32_t army_size(ObjectId hero) const;
  /// `HeroArmiesFullPerc(player)` -- how full a player's hero armies are, in
  /// percent of their combined caps. 0 when the player has no hero.
  [[nodiscard]] std::int32_t armies_full_percent(PlayerId player) const;

  /// The hero a unit belongs to, or `kNoObject`. The dump's `hero handle`.
  [[nodiscard]] ObjectId hero_of(ObjectId unit) const;
  /// The squad an object is in. The dump's `squad=<n>(<p>)`.
  [[nodiscard]] SquadKey squad_of(ObjectId id) const;

  // -- skills ----------------------------------------------------------

  /// `Hero.GetSkill(id)`. `-1` for an unknown hero or an out-of-range id --
  /// `HERO_SKILL_BEHAVIOUR.VS` tests `skillpoints < 0` explicitly.
  [[nodiscard]] std::int32_t skill(ObjectId hero, HeroSkill which) const;
  [[nodiscard]] std::int32_t skill(ObjectId hero, std::int32_t id) const;

  /// `Hero.SetSkill(id, points)`. Clamps to `[0, max_skill_points]` and
  /// refuses what the balance does not cover.
  bool set_skill(ObjectId hero, HeroSkill which, std::int32_t points);
  /// The map's `hs<Skill>="n"`: written straight into the record, spending
  /// nothing, which is what the hero loader does -- 0x00530810 matches each
  /// attribute against the `hs*` table (0x00824b68, the enum's order) and
  /// calls `Hero::SetSkill` (0x0052def0), which clamps to `[-1, 10]` and
  /// writes the byte at `[hero+0x1fc+skill]` with no look at the balance.
  /// Clamped to `[0, max_skill_points]` here; -1 is the exe's "not offered"
  /// marker and the `offered` table carries that.
  bool load_skill(ObjectId hero, HeroSkill which, std::int32_t points);
  /// Whether the hero's class offers the skill at all.
  ///
  /// **Not enforced by `set_skill`.** The `HeroSkills` list is what the skill
  /// panel offers and what the AI tactic scripts choose from; nothing in the
  /// corpus shows the engine refusing a `SetSkill` outside it, and silently
  /// dropping an AI's request would be a wrong answer rather than a refusal.
  /// So this is a question the UI and the AI ask, not a gate.
  [[nodiscard]] bool offers_skill(ObjectId hero, HeroSkill which) const;

  /// `AvailableSkillPoints` (0x0052da80): the balance as the original
  /// derives it rather than keeps it -- `min(10 per offered skill, level)
  /// - spent`, where `level` is `level_for_experience` (0x005d28c0) of the
  /// raw counter, **one point per level**. Nothing is stored: a level-up is
  /// a point, and a skill sold back is a point again.
  [[nodiscard]] std::int32_t available_skill_points(ObjectId hero) const;

  /// `Hero.UseSkill(id)` -- fire an active skill.
  ///
  /// Refuses a passive skill, a skill at zero points, and a skill already in
  /// effect, which is exactly what `VERIFY_HERO_SKILL.VS` checks. **Stamina is
  /// not checked or spent here**: `ACTIVATE_HERO_SKILL.VS` does that at the
  /// call site (`if (.UseSkill(...)) .SetStamina(.stamina - cmdcost_stamina)`),
  /// so the cost belongs to the command layer and `hero_skill_stamina_cost`
  /// exposes it.
  bool use_skill(World& world, ObjectId hero, HeroSkill which);

  /// `Hero.SkillInEffect(id)`.
  [[nodiscard]] bool skill_in_effect(ObjectId hero, HeroSkill which, GameTime now) const;

  /// How long an activation of `which` lasts, in game-time units.
  [[nodiscard]] std::int32_t skill_duration(ObjectId hero, HeroSkill which) const;

  // -- progression -----------------------------------------------------

  /// Experience needed to leave `level`.
  ///
  /// `CONST.INI` spells the recurrence out:
  /// `Exp[level] = Exp[level-1] + Pos(level - T) * ExpA + ExpB`, with
  /// `Pos(x) = x > 0 ? x : 0`, so the *increment* at `level` is
  /// `max(level - ExpT, 0) * ExpA + ExpB` and that is what this returns.
  ///
  /// **The reset reading is inferred.** Taken literally the recurrence is a
  /// cumulative table, but the dumps carry `level = 24` beside
  /// `experience = 74` and the first increment alone is 125, so the dumped
  /// `experience` cannot be a running total. Reading it as progress within the
  /// current level, reset on each level-up, is the only reading consistent with
  /// both the constants and the measurements.
  [[nodiscard]] std::int32_t experience_to_next_level(std::int32_t level) const;

  /// `exp_gain = target_level / LevelExpDivider + 1`, verbatim from the file.
  [[nodiscard]] std::int32_t experience_for_kill(std::int32_t target_level) const;

  /// Award experience to a unit, levelling it up as many times as it earns.
  ///
  /// An attached warrior's award is raised by its hero's
  /// `experience_percent` -- `heroarmyexpgain="40"` on the hero class plus
  /// Leadership -- and the hero itself receives
  /// `awarded / ExpFromArmyDivider` in turn. Returns the levels gained.
  std::int32_t add_experience(ObjectId unit, std::int32_t amount);

  /// `Unit.SetExperience(n)` -- writes the raw counter, no levelling.
  bool set_experience(ObjectId unit, std::int32_t value);
  /// `Unit.experience`.
  [[nodiscard]] std::int32_t experience(ObjectId unit) const;

  /// `Unit::IncKills(n)` -- add to the unit's kill tally.
  ///
  /// No clamp of any kind: `gbr.exe` 0x005d7870 is one `add`, and a negative
  /// argument subtracts. False for a unit this system has no record for.
  bool add_kills(ObjectId unit, std::int32_t amount);
  [[nodiscard]] std::int32_t kills(ObjectId unit) const;

  /// `Unit.SetLevel(n)` -- writes the *inherent* level, which is what
  /// `MUSHROOM.VS` does with `.SetLevel(.inherentlevel + 1)`.
  bool set_level(ObjectId unit, std::int32_t level);
  /// `Unit.inherentlevel`.
  [[nodiscard]] std::int32_t inherent_level(ObjectId unit) const;
  /// `Unit.level` -- the effective level:
  /// `max(inherent + level_add, level_floor)`.
  [[nodiscard]] std::int32_t level(ObjectId unit) const;

  // -- modifiers -------------------------------------------------------

  /// Everything the unit's hero and its own items do to it, right now.
  ///
  /// Computed rather than cached: a cache would need an invalidation rule that
  /// covers attach, detach, skill change, item pickup and effect expiry, and a
  /// stale entry would be a desync the hash could not see until much later.
  [[nodiscard]] UnitModifiers modifiers_for(ObjectId unit) const;
  [[nodiscard]] UnitModifiers modifiers_for(ObjectId unit, GameTime now) const;

  /// A hero's own modifiers: the Epic skills, Scout, and its items. Distinct
  /// from `modifiers_for` because Epic Endurance, Epic Attack, Epic Armor and
  /// Scout are on the hero and never reach its army.
  [[nodiscard]] UnitModifiers hero_modifiers(ObjectId hero) const;

  /// The class property `maxhealth` with this object's modifiers applied.
  ///
  /// The class graph is where the base lives (`Hero.SC.XML` declares
  /// `maxhealth="1000"`), so a world with no graph -- every synthetic test --
  /// gets 0, meaning "unknown", and the callers that would clamp against it do
  /// not clamp. Zero is not "this object has no health".
  [[nodiscard]] std::int32_t max_health_of(const World& world, ObjectId id) const;

  /// The class property `maxstamina`. `Unit.SC.XML` declares 10, which is the
  /// dominant value in the dumps, and `fallback` is what a graph-less world
  /// gets.
  [[nodiscard]] std::int32_t max_stamina_of(const World& world, ObjectId id,
                                            std::int32_t fallback = 10) const;

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
  /// **Written:** the unit records, the hero records, the squad table, the item
  /// store and `now_`.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** `HeroConstants`, and the `ItemCatalog*` the item store
  /// borrows -- game data, not world state.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  [[nodiscard]] std::size_t unit_slot(ObjectId id) const noexcept;
  [[nodiscard]] std::size_t hero_slot(ObjectId id) const noexcept;
  void apply_item_bonus(UnitModifiers& out, ObjectId owner) const;
  void expire_effects(GameTime now);
  void run_egoism(World& world, GameTime from, GameTime to);
  void clear_dead(World& world);
  /// The detaches `on_death` and `on_erase` share.
  void detach_for_death(ObjectId id);

  HeroConstants constants_{};
  SquadTable squads_;
  ItemStore items_;
  std::vector<UnitRecord> units_;   ///< sorted by id; iteration order is state
  std::vector<HeroRecord> heroes_;  ///< sorted by id
  GameTime now_ = 0;
  /// The highest object id `enrol_new_units_in_squads` has looked at. Not
  /// saved: a loaded system starts at 0 and looks at everything once, which
  /// skips every restored unit and costs one pass.
  ObjectId squad_watermark_ = 0;
};

// --------------------------------------------------------------------------
// host bindings
// --------------------------------------------------------------------------

/// `CallContext::user` is a `HostContext*`, for this domain and every other.
///
/// This slice used to expect a `HeroHostState` of its own -- world plus system
/// -- while movement, economy and the object model expected a bare `World*` and
/// combat a `CombatHostContext*`. Each domain's tests set `user` to the type
/// that domain expected, so all of them passed and no embedder could satisfy
/// two at once. `sim/host_context.hpp` settles it: there is one type behind
/// `user`, the host functions reach the world with `world_of` and find their
/// own system with `hero_system_of` below.

/// The `HeroSystem` registered with a world, or null.
///
/// Found by `System::name()`, which is "hero" and is documented as stable, over
/// `World::systems()` in registration order. Registration order is part of the
/// simulation's definition, so this is a deterministic lookup and not a search.
[[nodiscard]] HeroSystem* hero_system_of(World& world) noexcept;

/// Implement the hero, squad and item entry points on `registry`.
///
/// Ordered by the call frequencies in `docs/formats/vs-host-api.md`: `hero`
/// (151 sites), `experience` (101), `army` (80), `inherentlevel` (70),
/// `SetLevel` (35), `AddItem` (32), `SetExperience` (26), `HasItem` (26),
/// `level` (20), `GetSquad` (15), `GetSkill` (12), `ItemUsed` (12),
/// `UseItem` (11), then the tail.
///
/// Entry points whose return value is a host collection are deliberately **not**
/// defined here: the collection types belong to the script host, not to this
/// system, and a half-built `ObjList` would be worse than the trap an undefined
/// entry point produces.
///
/// That covered exactly one entry point, `Hero.army`, and it now has a body --
/// in `sim/objlist.cpp`, which *is* that host and owns the pool. The rule above
/// is unchanged; what changed is that satisfying it no longer means leaving 92
/// call sites trapping. `HeroSystem` still owns the membership and `m_army`
/// only copies it out, so nothing about a collection leaks into this header.
/// Record that `victim` was struck at `now`, on the hero whose army it is in.
///
/// The original does this from the strike path, through the virtual at
/// `vtbl + 0xa4`: `0x005d5faa` calls `target->slot(attacker)` and `0x005dc23f`
/// writes the clock and the victim's own handle onto `[victim+0x170]`, which is
/// `Unit::hero` -- or onto the victim itself when the victim is a hero and has
/// no hero of its own. A victim that is neither writes nowhere.
///
/// **A free function so that `CombatSystem` can call it without learning what a
/// hero is.** It is the narrowest coupling that reproduces the original's
/// timing: doing it a turn later off `CombatSystem::events()` would let a
/// sub-AI verifier read `TimePastLastAttack` in the turn its unit was hit and
/// see the wrong answer, which is the branch `HERO_SNEAK_VERIFY.VS` decides on.
///
/// A no-op when the world has no hero system, which every synthetic combat test
/// is.
void record_army_attacked(World& world, ObjectId victim, GameTime now) noexcept;

/// Stamp the victim's **squad** with the time and the attacker.
///
/// `gbr.exe` 0x0041e6c0, called from `CVXUnit::OnAttacked` immediately before
/// the hero stamp above and from the same place, so the two share a hook here
/// as well. In order it refuses a victim that is dead, one that is an unspawned
/// spawn template, and one carrying `0x800000` in the second flag word; finds
/// the victim's squad; and then -- **only if the squad's stored time is not
/// already the current tick** -- writes the time and the attacker's handle.
///
/// That last condition is the interesting one: a squad remembers the **first**
/// blow of a tick, not the last, so two attackers landing together leave the
/// earlier one on record. It is one comparison in the original and it is the
/// difference between "who is shooting at us" and "whoever shot last".
///
/// **The GAIKA notification at the tail is deliberately absent.** After
/// stamping, the original compares three node ids on the squad and tells the
/// node table that a squad was attacked moving between two of them
/// (0x0044fc60). `sim/gaika.hpp` does not model the node table and this does
/// not invent it.
///
/// The `0x800000` flag on the second word has no name here; it is one of the
/// `dwUnitFlags` bits `docs/formats/state-vector.md` still lists as unexplained,
/// so the guard is *not* reproduced -- a guard on a bit nothing sets would be a
/// no-op wearing the clothes of a transcription. Recorded rather than guessed.
void record_squad_attacked(World& world, ObjectId victim, ObjectId attacker,
                           GameTime now) noexcept;

void define_hero_host(script::HostRegistry& registry);

}  // namespace imperivm::core::sim
