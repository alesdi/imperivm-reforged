#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/player_host.hpp"

#include "imperivm/core/sim/squad.hpp"

#include <algorithm>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

void fold(std::uint64_t& h, std::uint64_t value) noexcept {
  for (int byte = 0; byte < 8; ++byte) {
    h ^= static_cast<std::uint64_t>((value >> (byte * 8)) & 0xFF);
    h *= kFnvPrime;
  }
}

void fold_i32(std::uint64_t& h, std::int32_t value) noexcept {
  fold(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

/// `DATA\SKILLS.INI` section names, in file order. The order is the enum.
constexpr std::string_view kSkillNames[kHeroSkillCount] = {
    "Administration", "Team attack", "Team defense",  "Quick March", "Epic endurance",
    "Discipline",     "Leadership",  "Epic attack",   "Battle cry",  "Healing",
    "Ceasefire",      "Vigor",       "Frenzy",        "Rush",        "Egoism",
    "Wisdom",         "Recovery",    "Survival",      "Charge",      "Scout",
    "Assault",        "Epic armor",  "Concealment",   "Defensive cry", "Euphoria",
};

/// The `hs*` constants. Twenty of these occur in the shipped scripts and are
/// listed in `docs/formats/vs-host-api.md`; the five that do not
/// (`hsAdministration`, `hsEpicAttack`, `hsEgoism`, `hsScout`, `hsEpicArmor`)
/// are spelled to match and are read by nothing.
constexpr std::string_view kSkillConstants[kHeroSkillCount] = {
    "hsAdministration", "hsTeamAttack",  "hsTeamDefense", "hsQuickMarch", "hsEpicEndurance",
    "hsDiscipline",     "hsLeadership",  "hsEpicAttack",  "hsBattleCry",  "hsHealing",
    "hsCeasefire",      "hsVigor",       "hsFrenzy",      "hsRush",       "hsEgoism",
    "hsWisdom",         "hsRecovery",    "hsSurvival",    "hsCharge",     "hsScout",
    "hsAssault",        "hsEpicArmor",   "hsConcealment", "hsDefensiveCry", "hsEuphoria",
};

/// `coststamina` on the eight `skill_*` commands in `DATA\COMMANDS\HERO.XML`.
/// Zero means the skill has no command and is passive.
constexpr std::int32_t kStaminaCost[kHeroSkillCount] = {
    0, 0, 0, 0, 0,  // administration .. epic_endurance
    0, 0, 0,        // discipline, leadership, epic_attack
    6,              // battle_cry
    6,              // healing
    6,              // ceasefire
    0,              // vigor
    4,              // frenzy
    4,              // rush
    0, 0, 0, 0,     // egoism, wisdom, recovery, survival
    6,              // charge
    0,              // scout
    6,              // assault
    0,              // epic_armor
    0,              // concealment
    4,              // defensive_cry
    0,              // euphoria
};

std::int32_t parse_int(std::string_view text, std::int32_t fallback) noexcept {
  if (text.empty()) return fallback;
  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    i = 1;
  }
  if (i >= text.size()) return fallback;
  std::int32_t out = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return fallback;
    out = out * 10 + (text[i] - '0');
  }
  return negative ? -out : out;
}

std::string_view class_property(const World& world, ObjectId id, std::string_view key) {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr) return {};
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot->class_index == kNoClass) return {};
  return graph->property(slot->class_index, key);
}

std::int32_t class_int_property(const World& world, ObjectId id, std::string_view key,
                                std::int32_t fallback) {
  return parse_int(class_property(world, id, key), fallback);
}

/// A skill's index, for array access. `count` is never passed in.
constexpr std::size_t idx(HeroSkill skill) noexcept { return static_cast<std::size_t>(skill); }

}  // namespace

// --------------------------------------------------------------------------
// skill metadata
// --------------------------------------------------------------------------

std::string_view hero_skill_name(HeroSkill skill) noexcept {
  const std::size_t i = idx(skill);
  return i < kHeroSkillCount ? kSkillNames[i] : std::string_view{};
}

std::string_view hero_skill_constant(HeroSkill skill) noexcept {
  const std::size_t i = idx(skill);
  return i < kHeroSkillCount ? kSkillConstants[i] : std::string_view{};
}

std::int32_t hero_skill_id(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kHeroSkillCount; ++i) {
    if (iequal(kSkillNames[i], name) || iequal(kSkillConstants[i], name)) {
      return static_cast<std::int32_t>(i);
    }
  }
  return -1;
}

bool hero_skill_is_active(HeroSkill skill) noexcept {
  const std::size_t i = idx(skill);
  return i < kHeroSkillCount && kStaminaCost[i] > 0;
}

std::int32_t hero_skill_stamina_cost(HeroSkill skill) noexcept {
  const std::size_t i = idx(skill);
  return i < kHeroSkillCount ? kStaminaCost[i] : 0;
}

// --------------------------------------------------------------------------
// record lookup
// --------------------------------------------------------------------------

std::size_t HeroSystem::unit_slot(ObjectId id) const noexcept {
  const auto it = std::lower_bound(units_.begin(), units_.end(), id,
                                   [](const UnitRecord& r, ObjectId v) { return r.id < v; });
  return static_cast<std::size_t>(it - units_.begin());
}

std::size_t HeroSystem::hero_slot(ObjectId id) const noexcept {
  const auto it = std::lower_bound(heroes_.begin(), heroes_.end(), id,
                                   [](const HeroRecord& r, ObjectId v) { return r.id < v; });
  return static_cast<std::size_t>(it - heroes_.begin());
}

const UnitRecord* HeroSystem::unit(ObjectId id) const {
  const std::size_t at = unit_slot(id);
  if (at >= units_.size() || units_[at].id != id) return nullptr;
  return &units_[at];
}

UnitRecord* HeroSystem::unit(ObjectId id) {
  return const_cast<UnitRecord*>(static_cast<const HeroSystem*>(this)->unit(id));
}

const HeroRecord* HeroSystem::hero(ObjectId id) const {
  const std::size_t at = hero_slot(id);
  if (at >= heroes_.size() || heroes_[at].id != id) return nullptr;
  return &heroes_[at];
}

HeroRecord* HeroSystem::hero(ObjectId id) {
  return const_cast<HeroRecord*>(static_cast<const HeroSystem*>(this)->hero(id));
}

// --------------------------------------------------------------------------
// registration
// --------------------------------------------------------------------------

UnitRecord& HeroSystem::register_unit(World& world, ObjectId id) {
  (void)world;
  const std::size_t at = unit_slot(id);
  if (at < units_.size() && units_[at].id == id) return units_[at];
  UnitRecord record;
  record.id = id;
  return *units_.insert(units_.begin() + static_cast<std::ptrdiff_t>(at), record);
}

HeroRecord& HeroSystem::register_hero(World& world, ObjectId id) {
  const std::size_t at = hero_slot(id);
  if (at < heroes_.size() && heroes_[at].id == id) return heroes_[at];

  HeroRecord record;
  record.id = id;
  record.max_army = class_int_property(world, id, "max_army", constants_.default_max_army);

  // `HeroSkills="Leadership, Epic attack, ..."` -- a comma-separated list of
  // `SKILLS.INI` section names. Absent means the class offers everything.
  const std::string_view offered = class_property(world, id, "HeroSkills");
  if (offered.empty()) {
    record.offered.fill(true);
  } else {
    for (std::size_t begin = 0; begin <= offered.size();) {
      std::size_t end = offered.find(',', begin);
      if (end == std::string_view::npos) end = offered.size();
      std::string_view name = offered.substr(begin, end - begin);
      while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
      while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
      const std::int32_t which = hero_skill_id(name);
      if (which >= 0) record.offered[static_cast<std::size_t>(which)] = true;
      begin = end + 1;
    }
  }

  PlayerId owner = kNoPlayer;
  if (const ObjectState* state = world.state(id)) owner = state->owner;
  // A hero is in one squad, the one it leads: there its squad is its army
  // (`squad_is_the_hero_plus_its_army`). `start` registers only the heroes the
  // map placed, so one a script placed afterwards -- a tavern's hire -- can be
  // squadded before it is registered, by `hero_squad` (0x00446ad7's first
  // half), which mints it a squad it leads. That squad is adopted here rather
  // than a second one minted beside it: a hero heading two squads had its
  // army split between them, and `GetSquad`, `EvalAttach` and the recruiter
  // each judged it by whichever they found first -- the hero alone.
  SquadKey existing = squads_.squad_of(id);
  if (Squad* squad = squads_.find(existing); squad != nullptr && squad->members.front() != id) {
    (void)squads_.leave(world, existing, id);
    existing = kNoSquad;
  }
  if (existing != kNoSquad) {
    squads_.find(existing)->leader = id;
    record.squad = existing;
  } else {
    record.squad = squads_.create(owner, id);
  }

  HeroRecord& stored =
      *heroes_.insert(heroes_.begin() + static_cast<std::ptrdiff_t>(at), std::move(record));

  // A hero prints the full `CVXUnit` block -- experience, level, squad -- so it
  // is a unit here too, and shares the squad it leads.
  UnitRecord& as_unit = register_unit(world, id);
  as_unit.squad = stored.squad;
  return stored;
}

void HeroSystem::forget(World& world, ObjectId id) {
  if (HeroRecord* record = hero(id)) {
    const std::vector<ObjectId> army = record->army;
    for (const ObjectId member : army) detach(world, member);
    squads_.destroy(record->squad);
    heroes_.erase(heroes_.begin() + static_cast<std::ptrdiff_t>(hero_slot(id)));
  }
  if (const UnitRecord* record = unit(id)) {
    if (record->hero != kNoObject) detach(world, id);
    const std::size_t at = unit_slot(id);
    squads_.leave(world, units_[at].squad, id);
    units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(at));
  }
  items_.drop_owner(world, id);
  squads_.prune_empty();
}

void HeroSystem::start(World& world) {
  // Adopt whatever the world was populated with: heroes first, so that a unit
  // registered afterwards can already find its hero's squad.
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.state.flags.is_hero) register_hero(world, slot.id);
  }
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.state.flags.is_unit && !slot.state.flags.is_hero) register_unit(world, slot.id);
  }
  now_ = world.time();
}

// --------------------------------------------------------------------------
// attachment
// --------------------------------------------------------------------------

std::int32_t HeroSystem::max_army(ObjectId id) const {
  const HeroRecord* record = hero(id);
  if (record == nullptr) return 0;
  return record->max_army + constants_.units_per_administration_level *
                                record->skills[idx(HeroSkill::administration)];
}

std::int32_t HeroSystem::army_size(ObjectId id) const {
  const HeroRecord* record = hero(id);
  return record != nullptr ? static_cast<std::int32_t>(record->army.size()) : 0;
}

bool HeroSystem::army_full(ObjectId id) const {
  const HeroRecord* record = hero(id);
  if (record == nullptr) return true;
  return static_cast<std::int32_t>(record->army.size()) >= max_army(id);
}

bool HeroSystem::has_army(ObjectId id) const { return army_size(id) > 0; }

std::int32_t HeroSystem::armies_full_percent(PlayerId player) const {
  std::int64_t held = 0;
  std::int64_t capacity = 0;
  for (const HeroRecord& record : heroes_) {
    if (record.squad.player != player) continue;
    held += static_cast<std::int64_t>(record.army.size());
    capacity += max_army(record.id);
  }
  if (capacity <= 0) return 0;
  return static_cast<std::int32_t>(held * 100 / capacity);
}

ObjectId HeroSystem::hero_of(ObjectId id) const {
  const UnitRecord* record = unit(id);
  return record != nullptr ? record->hero : kNoObject;
}

SquadKey HeroSystem::squad_of(ObjectId id) const {
  const UnitRecord* record = unit(id);
  return record != nullptr ? record->squad : kNoSquad;
}

bool HeroSystem::attach(World& world, ObjectId unit_id, ObjectId hero_id) {
  // The order of these refusals is `DATA\SUBAI\UNIT_ATTACH_VERIFY.VS`'s.
  if (unit_id == hero_id) return false;
  // Registered on demand. `start()` adopts what the map placed, and nothing
  // adopts what a script places afterwards -- so a warrior from `Place` or
  // `SpawnGroup` had no record, `AttachTo` refused it by that alone, and
  // `UNIT_ATTACH.VS` spun on the refusal: 622,684 refusals in sixty turns of
  // one map, every one of them "no record". The original has no registry to
  // be late for -- a `CVXUnit` can attach from construction -- which is the
  // reasoning `feed_from_now` in `sim/world_host.cpp` already applies to the
  // feeder. A record is minted only for an object that *is* a unit or a hero;
  // anything else still has none and still refuses.
  const ObjectState* unit_flags = world.state(unit_id);
  const ObjectState* hero_flags = world.state(hero_id);
  if (unit(unit_id) == nullptr && unit_flags != nullptr && unit_flags->flags.is_unit) {
    (void)register_unit(world, unit_id);
  }
  if (hero(hero_id) == nullptr && hero_flags != nullptr && hero_flags->flags.is_hero) {
    (void)register_hero(world, hero_id);
  }
  UnitRecord* record = unit(unit_id);
  HeroRecord* leader = hero(hero_id);
  if (record == nullptr || leader == nullptr) return false;
  if (record->hero == hero_id) return true;  // `if (.hero == hero) return false`
  if (record->has_freedom || unit_has_freedom(world, unit_id)) return false;

  const ObjectState* hero_state = world.state(hero_id);
  const ObjectState* unit_state = world.state(unit_id);
  if (hero_state == nullptr || unit_state == nullptr) return false;
  // `if (!hero.IsAlive()) return;`
  if (hero_state->health <= 0 && max_health_of(world, hero_id) > 0) return false;
  // `if (this.IsEnemy(hero)) return;` -- a different owner is an enemy, and the
  // squad player field is derived from the owner, so a mixed squad could not be
  // represented anyway.
  if (unit_state->owner != hero_state->owner) return false;
  if (army_full(hero_id)) return false;

  if (record->hero != kNoObject) detach(world, unit_id);

  record->hero = hero_id;
  record->squad = leader->squad;
  leader->army.push_back(unit_id);
  squads_.join(leader->squad, unit_id);
  return true;
}

bool HeroSystem::detach(World& world, ObjectId unit_id) {
  UnitRecord* record = unit(unit_id);
  if (record == nullptr || record->hero == kNoObject) return false;

  if (HeroRecord* leader = hero(record->hero)) {
    const auto it = std::find(leader->army.begin(), leader->army.end(), unit_id);
    if (it != leader->army.end()) leader->army.erase(it);
    squads_.leave(world, leader->squad, unit_id);
  }
  record->hero = kNoObject;
  record->squad = kNoSquad;
  return true;
}

std::size_t HeroSystem::detach_army(World& world, ObjectId hero_id) {
  HeroRecord* leader = hero(hero_id);
  if (leader == nullptr) return 0;
  const std::vector<ObjectId> army = leader->army;
  for (const ObjectId member : army) detach(world, member);
  return army.size();
}

// --------------------------------------------------------------------------
// death
// --------------------------------------------------------------------------

void HeroSystem::roll_wisdom(World& world, ObjectId unit_id) {
  // `[unit+0x170]` resolved to an object, then its Wisdom byte
  // (`[hero+0x1fc+15]`), and no draw at all unless both are there.
  const UnitRecord* record = unit(unit_id);
  if (record == nullptr || record->hero == kNoObject) return;
  const ObjectId leader = record->hero;
  const HeroRecord* h = hero(leader);
  if (h == nullptr) return;
  const std::int32_t points = h->skills[idx(HeroSkill::wisdom)];
  if (points <= 0) return;
  // `(0, 99)` to the generator entry, which is inclusive at both ends -- the
  // same entry `rand(n)` reaches with `n - 1`.
  const std::int32_t draw = world.rng().between(0, 99);
  if (constants_.percent_per_wisdom_level * points > draw) (void)add_experience(leader, 1);
}

void HeroSystem::detach_for_death(World& world, ObjectId id) {
  // `DetachArmy` first, as the hero's override has it: every warrior leaves the
  // hero, and with it the hero's squad.
  if (hero(id) != nullptr) (void)detach_army(world, id);
  // `DetachHero`: out of the army deque, hero handle to 65535.
  if (const UnitRecord* record = unit(id); record != nullptr && record->hero != kNoObject) {
    (void)detach(world, id);
  }
  // The AI unregister: the squad handle cleared and the unit out of its squad.
  // Re-found, because `detach` may already have cleared it.
  if (UnitRecord* record = unit(id); record != nullptr) {
    if (record->squad.valid()) (void)squads_.leave(world, record->squad, id);
    record->squad = kNoSquad;
  }
  // And out of whatever squad the table lists it in, which is the squad the
  // original unregisters it from. The record is not enough: a unit placed
  // after `start` and squadded by the AI before anything registered it is in
  // the table with no record, and leaving by the record left it listed -- on
  // Crossroads, 1,716 dead or erased members in 4,000 turns, squads of
  // corpses the recruiter and the squad monitor walked for the rest of the
  // match.
  if (const SquadKey listed = squads_.squad_of(id); listed.valid()) (void)squads_.leave(world, listed, id);
  if (HeroRecord* record = hero(id); record != nullptr) record->squad = kNoSquad;
  squads_.prune_empty();
}

void HeroSystem::on_death(World& world, ObjectId id) {
  // A held unit takes the erase route in the original, and the erase detaches.
  if (const ObjectState* state = world.state(id); state != nullptr && state->is_held()) return;
  detach_for_death(world, id);
}

void HeroSystem::on_erase(World& world, ObjectId id) {
  (void)world;
  detach_for_death(world, id);
}

// --------------------------------------------------------------------------
// skills
// --------------------------------------------------------------------------

std::int32_t HeroSystem::skill(ObjectId hero_id, HeroSkill which) const {
  const HeroRecord* record = hero(hero_id);
  if (record == nullptr || idx(which) >= kHeroSkillCount) return -1;
  return record->skills[idx(which)];
}

std::int32_t HeroSystem::skill(ObjectId hero_id, std::int32_t id) const {
  if (id < 0 || static_cast<std::size_t>(id) >= kHeroSkillCount) return -1;
  return skill(hero_id, static_cast<HeroSkill>(id));
}

bool HeroSystem::set_skill(ObjectId hero_id, HeroSkill which, std::int32_t points) {
  HeroRecord* record = hero(hero_id);
  if (record == nullptr || idx(which) >= kHeroSkillCount) return false;
  if (points < 0) points = 0;
  if (points > constants_.max_skill_points) points = constants_.max_skill_points;

  const std::int32_t before = record->skills[idx(which)];
  const std::int32_t delta = points - before;
  // Spending more than is available is refused rather than clamped: the shipped
  // AI computes `skillpoints + available` itself and would silently under-spend
  // if we quietly truncated.
  if (delta > available_skill_points(hero_id)) return false;
  record->skills[idx(which)] = points;
  return true;
}

bool HeroSystem::load_skill(ObjectId hero_id, HeroSkill which, std::int32_t points) {
  HeroRecord* record = hero(hero_id);
  if (record == nullptr || idx(which) >= kHeroSkillCount) return false;
  if (points < 0) points = 0;
  if (points > constants_.max_skill_points) points = constants_.max_skill_points;
  record->skills[idx(which)] = points;
  return true;
}


bool HeroSystem::offers_skill(ObjectId hero_id, HeroSkill which) const {
  const HeroRecord* record = hero(hero_id);
  if (record == nullptr || idx(which) >= kHeroSkillCount) return false;
  return record->offered[idx(which)];
}

std::int32_t HeroSystem::available_skill_points(ObjectId hero_id) const {
  const HeroRecord* record = hero(hero_id);
  if (record == nullptr) return 0;
  // Derived, as 0x0052da80 derives it: `min(10 per offered skill, level)
  // - spent`, the level being `level_for_experience` of the raw counter --
  // one point per level, and a level-up is a point without anyone granting
  // it. Nothing is stored, so nothing can drift from the skills and the
  // level that are.
  std::int32_t spent = 0;
  std::int32_t capacity = 0;
  for (std::size_t i = 0; i < kHeroSkillCount; ++i) {
    spent += record->skills[i];
    if (record->offered[i]) capacity += constants_.max_skill_points;
  }
  const std::int32_t allowance = std::min(capacity, inherent_level(hero_id));
  return allowance > spent ? allowance - spent : 0;
}

std::int32_t HeroSystem::skill_duration(ObjectId hero_id, HeroSkill which) const {
  const std::int32_t points = skill(hero_id, which);
  if (points <= 0) return 0;
  const HeroConstants& c = constants_;
  switch (which) {
    // "for 5 seconds", and `BattleCryTime = 5000`. Flat, not per point.
    case HeroSkill::battle_cry: return c.battle_cry_time;
    // "1 second plus 1 additional second per skill point"
    case HeroSkill::ceasefire: return c.ceasefire_base_time + c.time_per_ceasefire_level * points;
    case HeroSkill::assault: return c.assault_base_time + c.time_per_assault_level * points;
    case HeroSkill::frenzy: return c.frenzy_base_time + c.time_per_frenzy_level * points;
    // "1 second plus 2 additional seconds per skill point"
    case HeroSkill::defensive_cry:
      return c.defensive_cry_base_time + c.time_per_defensive_cry_level * points;
    default: return 0;  // healing, charge and rush are instantaneous
  }
}

bool HeroSystem::skill_in_effect(ObjectId hero_id, HeroSkill which, GameTime now) const {
  const HeroRecord* record = hero(hero_id);
  if (record == nullptr || idx(which) >= kHeroSkillCount) return false;
  return record->effect_until[idx(which)] > now;
}

bool HeroSystem::use_skill(World& world, ObjectId hero_id, HeroSkill which) {
  HeroRecord* record = hero(hero_id);
  if (record == nullptr) return false;
  if (!hero_skill_is_active(which)) return false;
  const std::int32_t points = record->skills[idx(which)];
  if (points <= 0) return false;  // "You need to have at least 1 point"
  const GameTime now = world.time();
  if (record->effect_until[idx(which)] > now) return false;  // "already in effect"

  const std::int32_t duration = skill_duration(hero_id, which);
  if (duration > 0) record->effect_until[idx(which)] = now + duration;

  const HeroConstants& c = constants_;
  switch (which) {
    case HeroSkill::healing: {
      const std::int32_t amount = c.health_per_healing_level * points;
      for (const ObjectId member : record->army) {
        const ObjectState* state = world.state(member);
        if (state == nullptr) continue;
        const std::int32_t cap = max_health_of(world, member);
        std::int32_t healed = state->health + amount;
        if (cap > 0 && healed > cap) healed = cap;
        world.set_health(member, healed);
      }
      break;
    }
    case HeroSkill::charge: {
      const std::int32_t amount = c.stamina_per_charge_level * points;
      for (const ObjectId member : record->army) {
        const ObjectState* state = world.state(member);
        if (state == nullptr) continue;
        const std::int32_t cap = max_stamina_of(world, member);
        std::int32_t value = state->stamina + amount;
        if (cap > 0 && value > cap) value = cap;
        world.set_stamina(member, value);
      }
      break;
    }
    case HeroSkill::rush: {
      // "increases the stamina of all attached warriors by 1 per skill point,
      // while decreasing their health by 5 per skill point" -- and the health
      // cost is `RushDamage` from CONST.INI. See HeroConstants::rush_damage.
      const std::int32_t stamina = c.stamina_per_rush_level * points;
      const std::int32_t damage = c.rush_damage * points;
      for (const ObjectId member : record->army) {
        const ObjectState* state = world.state(member);
        if (state == nullptr) continue;
        const std::int32_t cap = max_stamina_of(world, member);
        std::int32_t value = state->stamina + stamina;
        if (cap > 0 && value > cap) value = cap;
        world.set_stamina(member, value);
        std::int32_t health = state->health - damage;
        if (health < 1) health = 1;  // Rush is a cost, not an execution
        world.set_health(member, health);
      }
      break;
    }
    case HeroSkill::frenzy: {
      // "halves the health ... of all attached warriors". The halving happens
      // once, when the cry goes up; the damage doubling is a modifier that
      // lasts as long as the effect does.
      for (const ObjectId member : record->army) {
        const ObjectState* state = world.state(member);
        if (state == nullptr) continue;
        std::int32_t health = state->health / 2;
        if (health < 1) health = 1;
        world.set_health(member, health);
      }
      break;
    }
    default: break;  // battle_cry, ceasefire, assault, defensive_cry: timed only
  }
  return true;
}

// --------------------------------------------------------------------------
// progression
// --------------------------------------------------------------------------

std::int32_t HeroSystem::experience_to_next_level(std::int32_t level) const {
  const std::int32_t positive = level - constants_.exp_t > 0 ? level - constants_.exp_t : 0;
  return positive * constants_.exp_a + constants_.exp_b;
}

std::int32_t HeroSystem::experience_for_kill(std::int32_t target_level) const {
  if (constants_.level_exp_divider <= 0) return 1;
  return target_level / constants_.level_exp_divider + 1;
}

std::int32_t HeroSystem::experience(ObjectId id) const {
  const UnitRecord* record = unit(id);
  return record != nullptr ? record->experience : 0;
}

std::int32_t HeroSystem::kills(ObjectId id) const {
  const UnitRecord* record = unit(id);
  return record != nullptr ? record->kills : 0;
}

bool HeroSystem::add_kills(ObjectId id, std::int32_t amount) {
  UnitRecord* record = unit(id);
  if (record == nullptr) return false;
  record->kills += amount;
  return true;
}

bool HeroSystem::set_experience(ObjectId id, std::int32_t value) {
  UnitRecord* record = unit(id);
  if (record == nullptr) return false;
  record->experience = value < 0 ? 0 : value;
  return true;
}

std::int32_t HeroSystem::inherent_level(ObjectId id) const {
  const UnitRecord* record = unit(id);
  return record != nullptr ? record->inherent_level : 0;
}

bool HeroSystem::set_level(ObjectId id, std::int32_t value) {
  UnitRecord* record = unit(id);
  if (record == nullptr) return false;
  record->inherent_level = value < 1 ? 1 : value;
  return true;
}

std::int32_t HeroSystem::level(ObjectId id) const {
  const UnitRecord* record = unit(id);
  if (record == nullptr) return 0;
  const UnitModifiers mods = modifiers_for(id);
  const std::int32_t effective = record->inherent_level + mods.level_add;
  return effective > mods.level_floor ? effective : mods.level_floor;
}

std::int32_t HeroSystem::add_experience(ObjectId id, std::int32_t amount) {
  UnitRecord* record = unit(id);
  if (record == nullptr || amount <= 0) return 0;

  // A hero's warriors earn a premium: `heroarmyexpgain` on the hero class plus
  // Leadership. The hero then takes a share of what its warrior earned.
  std::int32_t awarded = amount;
  const ObjectId leader = record->hero;
  if (leader != kNoObject) {
    const std::int32_t percent =
        constants_.army_bonus_experience_percent +
        constants_.exp_percent_per_leadership_level * skill(leader, HeroSkill::leadership);
    awarded = apply_percent(amount, percent);
  }

  record->experience += awarded;
  std::int32_t gained = 0;
  for (std::int32_t threshold = experience_to_next_level(record->inherent_level);
       threshold > 0 && record->experience >= threshold;
       threshold = experience_to_next_level(record->inherent_level)) {
    record->experience -= threshold;
    ++record->inherent_level;
    ++gained;
  }

  if (leader != kNoObject && constants_.exp_from_army_divider > 0) {
    const std::int32_t share = awarded / constants_.exp_from_army_divider;
    if (share > 0) {
      // Recurses one level: a hero's own `hero` is null (`hero handle` is
      // 65535 on every one of the 31 dumped heroes), so this terminates.
      add_experience(leader, share);
    }
  }
  return gained;
}

// --------------------------------------------------------------------------
// modifiers
// --------------------------------------------------------------------------

void HeroSystem::apply_item_bonus(UnitModifiers& out, ObjectId owner) const {
  const ItemBonus bonus = items_.total_bonus(owner);
  out.max_health_add += bonus.health;
  out.max_health_percent += bonus.health_percent;
  out.damage_add += bonus.damage;
  out.damage_percent += bonus.damage_percent;
  out.armor_slash_add += bonus.armor_slash;
  out.armor_slash_percent += bonus.armor_slash_percent;
  out.armor_pierce_add += bonus.armor_pierce;
  out.armor_pierce_percent += bonus.armor_pierce_percent;
  out.level_add += bonus.level;
}

UnitModifiers HeroSystem::modifiers_for(ObjectId id) const { return modifiers_for(id, now_); }

UnitModifiers HeroSystem::modifiers_for(ObjectId id, GameTime now) const {
  UnitModifiers out;
  const UnitRecord* record = unit(id);
  if (record == nullptr) return out;

  apply_item_bonus(out, id);

  const HeroRecord* leader = hero(record->hero);
  if (leader == nullptr) return out;
  const HeroConstants& c = constants_;
  const auto& s = leader->skills;

  // Always-on skills.
  out.damage_add += c.attack_per_team_attack_level * s[idx(HeroSkill::team_attack)];
  const std::int32_t defense = c.defense_per_team_defense_level * s[idx(HeroSkill::team_defense)];
  out.armor_slash_add += defense;
  out.armor_pierce_add += defense;
  out.speed_percent += c.speed_percent_per_quick_march_level * s[idx(HeroSkill::quick_march)];
  out.evade_percent += c.avoid_chance_percent_per_concealment_level * s[idx(HeroSkill::concealment)];
  out.vigor_percent += c.percent_per_vigor_level * s[idx(HeroSkill::vigor)];
  out.recovery_percent += c.percent_per_recovery_level * s[idx(HeroSkill::recovery)];
  out.survival_percent += c.percent_per_survival_level * s[idx(HeroSkill::survival)];
  out.euphoria_percent += c.percent_per_euphoria_level * s[idx(HeroSkill::euphoria)];
  out.experience_percent += c.army_bonus_experience_percent +
                            c.exp_percent_per_leadership_level * s[idx(HeroSkill::leadership)];

  // "Increases the level of every attached warrior so it is at least 2 plus the
  // number of skill points" -- a floor, so a level-9 veteran keeps its 9.
  const std::int32_t discipline = s[idx(HeroSkill::discipline)];
  if (discipline > 0) {
    out.level_floor = c.discipline_bonus_base + c.discipline_bonus_step * discipline;
  }

  // Timed effects.
  if (leader->effect_until[idx(HeroSkill::battle_cry)] > now) {
    out.level_add += c.levels_per_battle_cry_level * s[idx(HeroSkill::battle_cry)];
  }
  if (leader->effect_until[idx(HeroSkill::defensive_cry)] > now) {
    out.armor_slash_add += c.defensive_cry_defense;
    out.armor_pierce_add += c.defensive_cry_defense;
  }
  out.ignore_enemy_armor = leader->effect_until[idx(HeroSkill::assault)] > now;
  out.ceasefire = leader->effect_until[idx(HeroSkill::ceasefire)] > now;
  out.frenzy = leader->effect_until[idx(HeroSkill::frenzy)] > now;
  if (out.frenzy) out.damage_percent += 100;  // "doubles the damage"
  return out;
}

UnitModifiers HeroSystem::hero_modifiers(ObjectId hero_id) const {
  UnitModifiers out;
  const HeroRecord* record = hero(hero_id);
  if (record == nullptr) return out;
  apply_item_bonus(out, hero_id);

  const HeroConstants& c = constants_;
  const auto& s = record->skills;
  out.max_health_add += c.health_per_epic_endurance_level * s[idx(HeroSkill::epic_endurance)];
  out.damage_add += c.attack_per_epic_attack_level * s[idx(HeroSkill::epic_attack)];
  const std::int32_t armour = c.defense_per_epic_armor_level * s[idx(HeroSkill::epic_armor)];
  out.armor_slash_add += armour;
  out.armor_pierce_add += armour;
  out.sight_add += c.sight_per_scout_level * s[idx(HeroSkill::scout)];
  return out;
}

std::int32_t HeroSystem::max_health_of(const World& world, ObjectId id) const {
  const std::int32_t base = class_int_property(world, id, "maxhealth", 0);
  if (base <= 0) return 0;
  const UnitModifiers mods = is_hero(id) ? hero_modifiers(id) : modifiers_for(id);
  return apply_percent(base + mods.max_health_add, mods.max_health_percent);
}

std::int32_t HeroSystem::max_stamina_of(const World& world, ObjectId id,
                                        std::int32_t fallback) const {
  return class_int_property(world, id, "maxstamina", fallback);
}

// --------------------------------------------------------------------------
// the turn
// --------------------------------------------------------------------------

void HeroSystem::expire_effects(GameTime now) {
  for (HeroRecord& record : heroes_) {
    for (GameTime& until : record.effect_until) {
      if (until != 0 && until <= now) until = 0;
    }
  }
}

void HeroSystem::clear_dead(World& world) {
  // `.army.ClearDead` -- both hero behaviour scripts open with it. Only objects
  // the world no longer has are cleared: a unit at zero health is still an
  // object, and deciding when it stops being one is the combat system's call.
  //
  // **A death no longer waits for this.** `on_death` detaches a dying unit in
  // the same instant it dies, and `on_erase` an erased one, so what is left
  // for this pass is an object that left the world by some third route -- a
  // bare `World::despawn` -- and would otherwise stay listed forever.
  for (HeroRecord& record : heroes_) {
    std::vector<ObjectId>& army = record.army;
    const auto gone = std::remove_if(army.begin(), army.end(), [&](ObjectId member) {
      if (world.find(member) != nullptr) return false;
      squads_.leave(world, record.squad, member);
      if (UnitRecord* unit_record = unit(member)) {
        unit_record->hero = kNoObject;
        unit_record->squad = kNoSquad;
      }
      return true;
    });
    army.erase(gone, army.end());
  }
}

void HeroSystem::run_egoism(World& world, GameTime from, GameTime to) {
  // "Once every 5 seconds the hero steals up to 20 health points per skill
  // point from a random warrior attached to him." `HealthPerEgoismLevel = 20`.
  //
  // The cadence is anchored on **absolute game time**, not on when the hero
  // learned the skill: a drain happens at every multiple of `egoism_interval`,
  // and this call handles the multiples in `(from, to]`. That is what makes the
  // whole system partition-invariant -- one turn of 60,000 units and 150 turns
  // of 400 cover exactly the same multiples, in the same order, and therefore
  // draw from the world RNG the same number of times in the same sequence.
  //
  // **The anchor is inferred.** The original has no script for Egoism, so
  // nothing in the data says whether its five seconds run from the world clock
  // or from the moment the skill was bought. A per-hero anchor would make the
  // drain order depend on how the turn was cut, which the lockstep contract
  // forbids, so the world clock is the only anchor that can be right.
  const std::int32_t interval = constants_.egoism_interval;
  if (interval <= 0 || to <= from) return;

  for (GameTime at = (from / interval + 1) * interval; at <= to; at += interval) {
    // Heroes in id order, which is spawn order: iteration order is state.
    for (HeroRecord& record : heroes_) {
      const std::int32_t points = record.skills[idx(HeroSkill::egoism)];
      if (points <= 0 || record.army.empty()) continue;

      const std::int32_t amount = constants_.health_per_egoism_level * points;
      const std::int32_t pick = world.rng().below(static_cast<std::int32_t>(record.army.size()));
      const ObjectId victim = record.army[static_cast<std::size_t>(pick)];

      const ObjectState* victim_state = world.state(victim);
      const ObjectState* hero_state = world.state(record.id);
      if (victim_state == nullptr || hero_state == nullptr) continue;
      // "steals *up to*": the victim is drained no further than it has, and a
      // hero does not kill its own warrior to top itself up.
      std::int32_t stolen = amount;
      if (stolen > victim_state->health - 1) stolen = victim_state->health - 1;
      if (stolen <= 0) continue;
      world.set_health(victim, victim_state->health - stolen);
      const std::int32_t cap = max_health_of(world, record.id);
      std::int32_t healed = hero_state->health + stolen;
      if (cap > 0 && healed > cap) healed = cap;
      world.set_health(record.id, healed);
    }
  }
}

void HeroSystem::advance(World& world, const Turn& turn) {
  // The turn covers `(start - 1, end]`: the dumps record
  // `gametimetickstart = gametime + 1`, so the boundary the turn began at is
  // `start - 1` and the interval is half-open at the bottom. Composing that
  // over any partition of an interval covers each instant exactly once.
  const GameTime from = turn.start - 1;
  now_ = turn.end;
  clear_dead(world);
  // Units that entered the world since last turn join a squad, while the AI
  // manager exists: 0x0041e820's job, done once a turn. See sim/squad.hpp.
  squad_watermark_ = enrol_new_units_in_squads(world, *this, squad_watermark_);
  run_egoism(world, from, now_);
  expire_effects(now_);
  squads_.prune_empty();
  // The two AI fields nothing used to write. `sim/squad.hpp` carries the two
  // chains this stands in for and why once a turn is the same answer.
  revalue_squads(world, squads_);
}

void HeroSystem::hash(std::uint64_t& accumulator) const {
  for (const UnitRecord& record : units_) {
    fold(accumulator, record.id);
    fold(accumulator, record.hero);
    fold_i32(accumulator, record.squad.index);
    fold(accumulator, record.squad.player);
    fold_i32(accumulator, record.experience);
    fold_i32(accumulator, record.inherent_level);
    fold(accumulator, record.has_freedom ? 1u : 0u);
    fold_i32(accumulator, record.kills);
  }
  for (const HeroRecord& record : heroes_) {
    fold(accumulator, record.id);
    fold_i32(accumulator, record.max_army);
    for (const std::int32_t points : record.skills) fold_i32(accumulator, points);
    for (const bool offered : record.offered) fold(accumulator, offered ? 1u : 0u);
    for (const GameTime until : record.effect_until) {
      fold(accumulator, static_cast<std::uint64_t>(until));
    }
    for (const ObjectId member : record.army) fold(accumulator, member);
    // When the army was last struck, and which member. Both are hashed: three
    // shipped sub-AI scripts branch on the first, so two peers that disagree
    // send a hero two different ways.
    fold(accumulator, static_cast<std::uint64_t>(record.army_attacked_at));
    fold(accumulator, record.army_attacked_unit);
    // The party orientations. Hashed because three shipped sub-AI scripts
    // branch on the settled one and a fourth writes the request, so two peers
    // that disagree form two different battle lines.
    fold_i32(accumulator, record.party_orientation_request.x);
    fold_i32(accumulator, record.party_orientation_request.y);
    fold_i32(accumulator, record.party_orientation.x);
    fold_i32(accumulator, record.party_orientation.y);
  }
  squads_.hash(accumulator);
  items_.hash(accumulator);
}

// --------------------------------------------------------------------------
// host bindings
// --------------------------------------------------------------------------

HeroSystem* hero_system_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "hero") return static_cast<HeroSystem*>(system);
  }
  return nullptr;
}

void record_squad_attacked(World& world, ObjectId victim, ObjectId attacker,
                           GameTime now) noexcept {
  HeroSystem* heroes = hero_system_of(world);
  if (heroes == nullptr || victim == kNoObject) return;
  const WorldObject* slot = world.find(victim);
  if (slot == nullptr || slot->state.health <= 0 || slot->state.flags.unspawned) return;
  Squad* squad = heroes->squads().find(heroes->squad_of(victim));
  if (squad == nullptr) return;
  // Once per tick, and the first blow wins. See the header.
  if (squad->last_fight_time == now) return;
  squad->last_fight_time = now;
  squad->last_attacker = attacker;
}

void record_army_attacked(World& world, ObjectId victim, GameTime now) noexcept {
  HeroSystem* heroes = hero_system_of(world);
  if (heroes == nullptr || victim == kNoObject) return;
  // The victim's hero first; a victim that is itself a hero and has no hero of
  // its own stamps on itself. 0x005dc219-0x005dc239, in that order: the
  // `kSyncHero` branch is only reached when the handle at `[+0x170]` does not
  // resolve, so a hero attached to another hero's army stamps the *leader*.
  ObjectId leader = heroes->hero_of(victim);
  if (leader == kNoObject) {
    const WorldObject* slot = world.find(victim);
    if (slot != nullptr && slot->state.flags.is_hero) leader = victim;
  }
  HeroRecord* record = leader == kNoObject ? nullptr : heroes->hero(leader);
  if (record == nullptr) return;
  record->army_attacked_at = now;
  record->army_attacked_unit = victim;
}

namespace {

using script::CallContext;
using script::HostOutcome;
using script::Value;

constexpr const char* kNoWorld = "hero: no World behind CallContext::user";
constexpr const char* kNoSystem = "hero: no hero system registered with the world";

/// The world and the hero system behind a call, or the reason there is neither.
///
/// `CallContext::user` is a `HostContext*` (sim/host_context.hpp) and the system
/// is found by name over `World::systems()`, in registration order. Null is a
/// real case, not a defensive check: `engine/tests` runs every entry point with
/// `user == nullptr` to prove it refuses rather than dereferences.
struct HeroSelf {
  World* world = nullptr;
  HeroSystem* heroes = nullptr;
  const char* error = nullptr;
};

HeroSelf resolve(CallContext& ctx) {
  HeroSelf self;
  self.world = world_of(ctx);
  if (self.world == nullptr) {
    self.error = kNoWorld;
    return self;
  }
  self.heroes = hero_system_of(*self.world);
  if (self.heroes == nullptr) self.error = kNoSystem;
  return self;
}

/// The receiver of a member call, as an object id. Argument 0 is the receiver.
ObjectId receiver(CallContext& ctx) {
  if (ctx.arguments.empty() || !ctx.arg(0).is_object()) return kNoObject;
  return ctx.arg(0).as_object().id;
}

ObjectId object_arg(CallContext& ctx, std::size_t i) {
  if (i >= ctx.count() || !ctx.arg(i).is_object()) return kNoObject;
  return ctx.arg(i).as_object().id;
}

std::int32_t int_arg(CallContext& ctx, std::size_t i) {
  if (i >= ctx.count() || !ctx.arg(i).is_integer()) return 0;
  return ctx.arg(i).as_integer();
}

std::string_view string_arg(CallContext& ctx, std::size_t i) {
  if (i >= ctx.count() || !ctx.arg(i).is_string()) return {};
  return ctx.arg(i).as_string();
}

/// The `TypeId` a hero handle carries. The script host assigns type ids; until
/// it does, an object value only has to round-trip its id, and reusing the
/// receiver's type does exactly that.
script::TypeId receiver_type(CallContext& ctx) {
  if (ctx.arguments.empty() || !ctx.arg(0).is_object()) return script::kNoType;
  return ctx.arg(0).as_object().type;
}

/// Resolve the world and the system or refuse. `host` stays the name the entry
/// points below use, so the migration off `HeroHostState` changed the lookup
/// and nothing else.
#define HERO_STATE(ctx)                                                        \
  const HeroSelf self = resolve(ctx);                                          \
  const HeroSelf* const host = &self;                                          \
  if (host->error != nullptr) return HostOutcome::failed(host->error)

// -- attachment ------------------------------------------------------------

HostOutcome fn_attach_to(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(
      Value::boolean(host->heroes->attach(*host->world, receiver(ctx), object_arg(ctx, 1))));
}

HostOutcome fn_detach_from(CallContext& ctx) {
  HERO_STATE(ctx);
  const ObjectId unit = receiver(ctx);
  // `DetachFrom(hero)` names the hero it is leaving; a unit is in one army, so
  // the argument is a check rather than a selector.
  const ObjectId named = object_arg(ctx, 1);
  if (named != kNoObject && host->heroes->hero_of(unit) != named) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(Value::boolean(host->heroes->detach(*host->world, unit)));
}

HostOutcome fn_detach_army(CallContext& ctx) {
  HERO_STATE(ctx);
  host->heroes->detach_army(*host->world, receiver(ctx));
  return HostOutcome::ok_void();
}

HostOutcome fn_has_army(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::boolean(host->heroes->has_army(receiver(ctx))));
}

/// `Hero::IsArmyOutside()` -- 3 sites, the sole blocker of all three, and all
/// three spell the same idiom: `while (!.IsArmyOutside()) .FormKeepMoving(500);`
/// after a `FormSetupAndMoveTo`. `HERO_ADVANCE.VS` even writes the comment --
/// *"Wait for army to exit"*.
///
/// **The question is whether anybody is still indoors, and nothing else.**
/// `0x0052ff00` walks the deque of handles at `[hero+0x1d4]` -- the hero's army,
/// the same membership `HeroRecord::army` holds -- and for each member resolves
/// the handle and tests one word: `[member+0x154] != 0xffff` returns **false**
/// at once. That word is the holder handle, `0xffff` for none, which this file
/// already reads that way twice in `fn_find_unit_to_heal`'s neighbourhood. No
/// distance, no gate, no settlement identity, no path: a unit garrisoned in a
/// building on the far side of the map keeps the answer false, and a unit
/// standing in the doorway makes it true.
///
/// Two details of the walk are the original's and are kept:
///
///   * **a member handle that no longer resolves is skipped**, not counted as
///     indoors -- 0x0052ff96 jumps to the increment. An army that has been
///     killed off answers *outside*, which is what lets the shipped
///     `while` loops terminate rather than spin on a dead army;
///   * **an empty army answers true.** 0x0052ff64 tests the deque's bounds
///     before the first iteration and goes straight to the `1` at 0x0052ffbd,
///     so a hero with nobody to wait for does not wait.
///
/// An unresolvable receiver answers **false**, which is the original's error
/// path rather than a choice: 0x0052ff2b complains -- *"The function
/// 'Hero::IsArmyOutside' called for an uninitialized or invalid object."* -- and
/// pushes zero. A receiver that resolves but carries no `HeroRecord` has no
/// members to walk and therefore answers true, by the empty-army rule above and
/// not by a second one.
HostOutcome fn_is_army_outside(CallContext& ctx) {
  HERO_STATE(ctx);
  const ObjectId id = receiver(ctx);
  if (host->world->find(id) == nullptr) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const HeroRecord* record = host->heroes->hero(id);
  if (record != nullptr) {
    for (const ObjectId member : record->army) {
      const WorldObject* slot = host->world->find(member);
      // `[member+0x154] != 0xffff` -- inside a holder, so the army is not out.
      if (slot != nullptr && slot->state.holder != kNoObject) {
        return HostOutcome::ok_with(Value::boolean(false));
      }
    }
  }
  return HostOutcome::ok_with(Value::boolean(true));
}

/// `Hero::TimePastLastAttack()` -- 3 sites, the sole blocker of all three, and
/// **it is about being attacked, not about attacking**.
///
/// `0x0052ee40` is `now - [hero+0x2b8]`, three instructions. What writes that
/// stamp is the virtual at `vtbl + 0xa4`, called as `target->slot(attacker)`
/// from the strike path at 0x005d5faa -- so the receiver inside it is the
/// victim, and 0x005dc23f writes the clock onto the victim's *hero*
/// (`[victim+0x170]`, which is `Unit::hero`) or onto the victim itself when the
/// victim is a hero. `HeroRecord::army_attacked_at` carries the reasoning.
///
/// **The original's own author wrote it down.** `HERO_SNEAK.VS` line 13 reads
/// `attacker = .LastAttacker(); // this is in fact the unit from the army last
/// being attacked` -- inside a `/* */` block the file no longer runs, which is
/// why neither name appears in the census, and which makes it a note left for
/// whoever came next rather than a call site. It agrees with the disassembly on
/// both halves: the *army*, and *being attacked*.
///
/// **`Unit::TimeWithoutAttack` is a different field and a different question.**
/// That one is `now - [unit+0x1a4]`, per-unit, written by
/// `Unit::SetLastAttackTime`, and it means *how long since I last struck*.
/// The two look like the same thing and have been confused in this tree
/// before; they are 0x1a4 on the unit and 0x2b8 on the hero.
///
/// Clamped at zero for the reason `host_time_without_attack` gives: this engine
/// reads two clocks where the original reads one, and a negative is a value no
/// original ever returned.
///
/// A receiver that is not a registered hero answers `now` -- which is what the
/// original answers for a hero that has never been struck, because the stamp
/// starts at zero, and what all four readers want to hear.
HostOutcome fn_time_past_last_attack(CallContext& ctx) {
  HERO_STATE(ctx);
  const HeroRecord* record = host->heroes->hero(receiver(ctx));
  const GameTime stamp = record == nullptr ? 0 : record->army_attacked_at;
  const GameTime since = host->world->time() - stamp;
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(since > 0 ? since : 0)));
}

/// `GetRandomHeroClass(base, player)` -- 4 sites in two scripts, and the sole
/// blocker of both: `ARENA_HIREHERO.VS` (45 sites) and `003 HERO.VS` (9).
///
/// Both callers feed the answer straight into `Place(class, pt, player)`, so
/// what comes back is a class **id**, and an empty string is a `Place` that
/// spawns nothing. 0x00531440 pops the two arguments, **decrements the player**
/// like every per-player argument here, and hands them to 0x00531060; a miss
/// reports *"No heroes found"* through the bare-`ret` sink and pushes `""`.
///
/// ## The two exclusions, both read
///
/// 0x00531060 sweeps the map for the asking player's **heroes** -- the
/// predicate at 0x0052d8a0 is bit 24 of `[obj+0x2c]` plus a cast to `CVXHero`,
/// which is `ObjectFlags::is_hero` here -- and then walks candidate classes
/// keeping two lists:
///
///   * a class the player **already has a hero of** is dropped from both lists
///     (`cmp [hero+0x3c], ebp` / `je` straight to the next class);
///   * a class whose **icon** the player already has on screen is kept in the
///     wide list but left out of the narrow one. `[class+0x704]` is the loaded
///     bitmap the class reader fills from the `icon` attribute at 0x005a350e,
///     and two classes that name the same file share the pointer -- `BHero1`
///     and `BHero1a` are two different Britons, *Gawain* and *Thrydwulf*, on
///     one `gameres/icons/BHero1.bmp`. A class that declares no `icon` at all
///     has a null there and never clashes, which is why the test is
///     `[ebp+0x704] != 0` first.
///
/// The pick is `rand(size)` over the **narrow** list, and over the wide one
/// only when the narrow list came out empty -- so the icon rule is a
/// preference and not a filter. Comparing the `icon` *string* is the same
/// relation without loading a byte of art.
///
/// ## What is inferred, and it is the candidate set
///
/// **INFERRED: the candidates are the named class's subtree.** The collection
/// 0x00531060 walks is built by 0x00530e40, which iterates the whole class
/// registry and keeps entries whose key equals their own class id -- an
/// alias fold, with no name filter in it that this pass could find. But the
/// name cannot be ignored: `003 HERO.VS` chooses between `"BritonHero"`,
/// `"EgyptianHero"`, `"GermanHero"` and five more on the player's race and
/// passes the result here, and that branch is pointless unless it narrows the
/// answer. The shipped tree makes the reading obvious -- `BritonHero` is a
/// class with twelve childless children, `BHero1` .. `BHero3c` -- so the
/// subtree is what this takes. What is *not* settled is whether the base class
/// itself is a candidate; it is included, because nothing in the recovered
/// half excludes it and `BritonHero` is as spawnable as its children.
///
/// The other simplification is stated rather than hidden: 0x0059a350 gates each
/// class on `[class+0x88]`, indexed by the campaign at `[globals+0x174]` --
/// a per-campaign availability table this engine does not have. Every class in
/// the subtree is therefore available here, which is what a skirmish gives.
HostOutcome fn_get_random_hero_class(CallContext& ctx) {
  HERO_STATE(ctx);
  World& world = *host->world;
  const auto none = [] { return HostOutcome::ok_with(Value::string(std::string())); };
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || ctx.count() < 2 || !ctx.arg(0).is_string()) return none();
  const ClassIndex base = graph->lookup(ctx.arg(0).as_string());
  if (base == kNoClass) return none();
  const PlayerId player =
      ctx.arg(1).is_integer() ? player_from_script(ctx.arg(1).as_integer()) : kNoPlayer;

  // What the sweep finds: this player's heroes, by class and by face.
  std::vector<ClassIndex> owned;
  std::vector<std::string_view> faces;
  for (const WorldObject& slot : world.objects()) {
    if (!slot.state.flags.is_hero) continue;
    if (slot.state.owner != player) continue;
    if (slot.class_index == kNoClass) continue;
    owned.push_back(slot.class_index);
    // Every hero's icon, **including the empty one**. The original compares
    // loaded bitmap pointers and two classes that declare no icon both hold
    // null, which would compare equal -- so what stops a faceless candidate
    // clashing with a faceless hero is the candidate-side `!= 0` test below,
    // and filtering here would quietly move that guard.
    faces.push_back(graph->property(slot.class_index, "icon"));
  }

  // The subtree, base first and then children in id order, which is the order
  // `ClassGraph::link` puts them in and therefore deterministic.
  std::vector<ClassIndex> wide;
  std::vector<ClassIndex> narrow;
  std::vector<ClassIndex> stack{base};
  while (!stack.empty()) {
    const ClassIndex here = stack.back();
    stack.pop_back();
    for (auto it = graph->children(here).rbegin(); it != graph->children(here).rend(); ++it) {
      stack.push_back(*it);
    }
    if (std::find(owned.begin(), owned.end(), here) != owned.end()) continue;
    wide.push_back(here);
    const std::string_view icon = graph->property(here, "icon");
    if (icon.empty() || std::find(faces.begin(), faces.end(), icon) == faces.end()) {
      narrow.push_back(here);
    }
  }

  const std::vector<ClassIndex>& pool = narrow.empty() ? wide : narrow;
  if (pool.empty()) return none();
  const std::int32_t pick = world.rng().below(static_cast<std::int32_t>(pool.size()));
  return HostOutcome::ok_with(
      Value::string(std::string(graph->at(pool[static_cast<std::size_t>(pick)]).id)));
}

HostOutcome fn_is_hero_army_full(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::boolean(host->heroes->army_full(receiver(ctx))));
}

/// `h.maxarmy` -- the **cap**, administration and all, not the class number.
///
/// 0x0052e380 is a plain read of `[hero+0x1f8]`, and the field it reads is the
/// same one `IsHeroArmyFull` (0x0052e320) compares the army size at `[+0x1e4]`
/// against. So it is the resolved ceiling rather than the `max_army` attribute
/// the class declares, and `HeroSystem::max_army` -- which is `max_army +
/// UnitsPerAdministrationLevel * points` -- is exactly it.
///
/// A receiver that is not a registered hero answers 0, which is the thunk's
/// zero after its diagnostic; `IsHeroArmyFull`'s surprising `true` on the same
/// miss is that one entry point's own and is not copied here. There is no
/// explicit guard because `HeroSystem::max_army` already answers 0 for a
/// receiver it cannot find -- one was written, injected as a fault, and could
/// not be made to fail.
HostOutcome fn_max_army(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::integer(host->heroes->max_army(receiver(ctx))));
}

/// `Hero::SetAutocast(bool)` -- 34 sites, all inside map containers.
///
/// Bit 2 of `[hero+0x194]` (0x0052f790), on the same second flag word `no_ai`,
/// `in_air` and `noselect` live on. Twenty-nine of the thirty-four pass `true`,
/// on a hero a mission has just spawned or handed over; the five that pass
/// `false` are one block in `5_Great_Battles_Britain` map 3, where a hero
/// changes sides and is quietened first.
///
/// An unresolvable receiver writes nothing and says nothing: the complaint goes
/// into the sink that is a bare `ret` in retail.
HostOutcome fn_set_autocast(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetAutocast: no world");
  ObjectState* state = world->mutable_state(receiver(ctx));
  if (state == nullptr) return HostOutcome::ok_void();
  state->flags.autocast =
      ctx.count() > 1 && ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  return HostOutcome::ok_void();
}

/// `Hero::autocast` -- the same bit, shifted down by two (0x0052f740).
///
/// Zero shipped call sites, and bound anyway: a flag a script can set and never
/// read is a flag nothing can be written against, and the getter is one field.
///
/// **A retail stack bug is deliberately not reproduced.** On an unresolvable
/// receiver `Hero::autocast` pops its eight-byte receiver and returns without
/// pushing the bool and without restoring the stack pointer, leaving the VM
/// stack eight bytes low with no result -- a corruption, not a behaviour. Its
/// sibling `SetAutocast` gets the same case right, and so do
/// `Gate::AreEnemiesAround` and `AreFriendsAround`, which push `false`. This
/// answers false.
HostOutcome fn_autocast(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("autocast: no world");
  const ObjectState* state = world->state(receiver(ctx));
  return HostOutcome::ok_with(Value::boolean(state != nullptr && state->flags.autocast));
}

/// `Druid::FindUnitToLearn` -- **the single largest trap in the corpus sweep**,
/// 480 hits from `DATA\SUBAI\BDRUID_IDLE.VS` alone.
///
/// The barbarian druid's *learn* ability raises its own experience toward a
/// more experienced neighbour's, and this is how it picks one. The script's
/// loop is `tgt = .FindUnitToLearn(); ... StartAnim(19, tgt.pos); exp = min(
/// .experience + (.inherentlevel + 3) / 4, tgt.experience)`, gated on the
/// `Ritual Chamber` research; it re-targets constantly, which is why one script
/// produced a fifth of the whole tally.
///
/// `0x00513ce0` scans a circle of the druid's own **sight** and takes the first
/// candidate the scan reaches, then stops. `0x00511ff0` is the predicate, and
/// every clause of it is transcribed here:
///
///   * alive, and not the druid itself;
///   * `[obj+0x180] > [self+0x180]` -- **strictly** more experienced, which is
///     what makes the script's `if (tgt.experience <= .experience) break;`
///     agree with it;
///   * not an heir of `Sentry`, `RamUnit` or `Peaceful` -- three lazily
///     resolved class names at `0x007ad494`, `0x007c2410` and `0x007c23fc`;
///   * `[obj+0x154] == 0xffff` -- not inside a holder;
///   * not a `CVXShip`.
///
/// **Two things are not reproduced, and both are named.** The scan's cell mask
/// is the complement of the druid's own player-visibility bit, so the original
/// only visits objects at least one *other* player can see -- this engine has
/// no fog and `ExploreArea` is unimplemented for the same reason, so there is
/// no visibility to filter on. And the original takes the first match in its
/// 256-unit grid's cell order, which is not the nearest and not reproducible
/// without that grid; this takes the first in ascending object id, which is
/// spawn order, equally not the nearest, and identical on every peer.
///
/// A miss is an invalid handle, which `BDRUID_IDLE.VS` guards with
/// `if (!tgt.IsValid())`.
HostOutcome fn_find_unit_to_learn(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindUnitToLearn: no world");
  HeroSystem* heroes = hero_system_of(*world);
  const auto none = HostOutcome::ok_with(Value::object(script::kNoType, 0));
  if (heroes == nullptr) return none;
  const ObjectId self = receiver(ctx);
  const WorldObject* druid = world->find(self);
  if (druid == nullptr) return none;

  const std::int32_t sight = druid->sight;
  const Point from = world->resolve_position(self);
  const std::int32_t mine = heroes->experience(self);
  const ClassGraph* graph = world->class_graph();

  for (const WorldObject& other : world->objects()) {
    // The self test is the original's and is redundant *given* the strict
    // comparison below -- nothing has more experience than itself -- so a fault
    // injected into it survives the suite. Kept because the original keeps it
    // and because the redundancy is between two clauses that could move
    // independently.
    if (other.id == self) continue;
    if (other.internal != InternalKind::none) continue;
    if (other.state.health <= 0) continue;             // `vtbl[0x50]`, IsDead
    if (other.state.holder != kNoObject) continue;     // `[obj+0x154] != 0xffff`
    if (other.object != nullptr && other.object->is_a(NativeClass::ship)) continue;
    if (heroes->experience(other.id) <= mine) continue;
    // Three class exclusions, each tested the way `sim/world_host.cpp`'s
    // `class_is` states the rule: a name the graph cannot resolve reads as
    // false. `ClassFilter::parse` keeps that guarantee on its own -- it leaves
    // `match_all` false with count 0 for an unresolved name -- so there is no
    // guard to write here beyond the parse itself.
    if (graph != nullptr && other.class_index != kNoClass) {
      bool excluded = false;
      for (const std::string_view base : {"Sentry", "RamUnit", "Peaceful"}) {
        if (world->matches_filter(other, ClassFilter::parse(base, graph))) excluded = true;
      }
      if (excluded) continue;
    }
    const Point at = world->resolve_position(other.id);
    const std::int64_t dx = at.x - from.x;
    const std::int64_t dy = at.y - from.y;
    if (dx * dx + dy * dy > static_cast<std::int64_t>(sight) * sight) continue;
    return HostOutcome::ok_with(Value::object(receiver_type(ctx), other.id));
  }
  return none;
}

// --------------------------------------------------------------------------
// the caster searches
// --------------------------------------------------------------------------
//
// Four entry points, one shape, and between them the top of the corpus trap
// tally: `FindUnitToHeal/0` (136 hits), `GetBestCurseTarget/0` (36),
// `FindUnitBelowILevel/1` (23) and `GetBestCrippleTarget/0` (10). Every one is
// a spell-caster's idle script asking *who do I do this to*, every one scans a
// circle of the caster's own **sight**, and they sit beside `FindUnitToLearn`,
// which is already here.
//
// The scripts share one skeleton -- `RPRIEST_IDLE.VS` says so in a comment,
// *"THIS SCRIPT IS SHARED BY THE ROMAN PRIEST AND THE GAUL DRUID"* -- and each
// alternates its own search with `FindUnitToLearn` through `action = 4 - action`
// so that a caster teaches when it has nothing to cast on. That alternation is
// why one script produces a hundred and thirty-six hits.
//
// ## Two of them score and two do not
//
// `FindUnitToLearn` and `FindUnitBelowILevel` take a match and stop.
// `FindUnitToHeal`, `GetBestCurseTarget` and `GetBestCrippleTarget` walk
// everything and keep a best, and their scores are the interesting part:
//
//   * **heal minimises** `300 * health / maxhealth + max(0, edgeDistance -
//     range)` -- a health *fraction*, so the most wounded wins, with a penalty
//     for every unit beyond the healer's own reach. A candidate too lightly
//     wounded to be worth a whole heal is parked at a flat **1000** rather than
//     rejected, so it still wins if nothing better is standing there.
//   * **curse maximises** the candidate's `attack`, and nothing else -- no
//     distance, no health, no aliveness.
//   * **cripple maximises** the candidate's `stamina`, behind a real predicate.
//
// The heal threshold is not a constant: it is `self.level + GetConst(
// "HealAmount")`, which is **exactly the amount the script is about to heal**
// (`amount = GetConst("HealAmount") + .level; tgt.Heal(amount);`). Engine and
// script agree on the number from two directions.
//
// ## What is not reproduced, and why
//
// **The visibility clauses.** Each predicate opens by testing the candidate
// against a per-player mask -- allies for heal and teach, enemies for curse and
// cripple -- and then, for a stealthed candidate, against the caster's
// team-vision bits. This engine models neither: `Obj::CanSee` is already
// implemented as *not hidden* for the same reason, and `ExploreArea` is
// unimplemented for it too. The ally/enemy half is kept, through
// `PlayerTable::is_enemy`, because the relation matrix is real here; the
// stealth half becomes "a hidden object is not a candidate", which is the
// conservative side.
//
// **The AI-roster fallback.** When the circle finds nothing, heal makes a
// second unbounded pass over the caster's AI player's friendly roster, and
// curse and cripple scan the *enemy* roster **instead of** the circle when the
// caster belongs to one. Those rosters are the GAIKA machinery's, which
// `sim/gaika.hpp` refuses to model. So every search here is the circle and only
// the circle, and a caster under an AI reaches fewer targets than it would.
//
// **The retail caster/candidate mix-up in curse is not reproduced.** Four
// consecutive clauses of `0x005d4760` test `pred[8]` -- the caster -- where the
// cripple predicate at `0x00512260` tests the candidate: is it a messenger, is
// it peaceful, is it a ship, is it a ram. For a shaman they are loop-invariant
// and always false, so the bug is invisible on shipped data; if the caster ever
// were one of those four, `GetBestCurseTarget` would return nothing for ever.
// Transcribing it would mean writing four tests that cannot fire and one that
// silently disables the entry point, and the corpus cannot tell the two
// readings apart. The candidate is tested here, and this paragraph is the
// record.

/// Everything the four searches need about one object.
struct Candidate {
  const WorldObject* slot = nullptr;
  std::int32_t max_health = 0;
  std::int32_t radius = 0;
};

[[nodiscard]] bool caster_candidate_basics(const World& world, const WorldObject& other,
                                           ObjectId self) {
  if (other.id == self) return false;
  if (other.internal != InternalKind::none) return false;
  if (other.state.health <= 0) return false;            // `vtbl[0x50]`
  if (other.state.holder != kNoObject) return false;    // `[obj+0x154] != 0xffff`
  if (other.state.flags.hidden) return false;           // see the section note
  if (other.object != nullptr && other.object->is_a(NativeClass::ship)) return false;
  return true;
}

/// `IsA` against a name, with `ClassFilter::parse`'s own guarantee that a name
/// the graph cannot resolve matches nothing.
[[nodiscard]] bool is_a(const World& world, const WorldObject& slot, std::string_view name) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass) return false;
  return world.matches_filter(slot, ClassFilter::parse(name, graph));
}

[[nodiscard]] std::int32_t caster_sight(const World& world, ObjectId id) {
  const WorldObject* slot = world.find(id);
  return slot == nullptr ? 0 : slot->sight;
}

/// `isqrt(dx² + dy²) − their radius − my radius`, which is what `0x005a77b0`
/// computes and what the heal score's distance term is measured in.
[[nodiscard]] std::int64_t edge_distance(const World& world, const WorldObject& a,
                                         const WorldObject& b) {
  const Point pa = world.resolve_position(a.id);
  const Point pb = world.resolve_position(b.id);
  const std::int64_t dx = pa.x - pb.x;
  const std::int64_t dy = pa.y - pb.y;
  const std::int64_t straight = isqrt(dx * dx + dy * dy);
  const std::int64_t radii = class_int_property(world, a.id, "radius", 0) +
                             class_int_property(world, b.id, "radius", 0);
  return straight - radii;
}

/// `Druid::FindUnitToHeal` -- 136 hits, and the score is the finding.
HostOutcome fn_find_unit_to_heal(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindUnitToHeal: no world");
  const auto none = HostOutcome::ok_with(Value::object(script::kNoType, 0));
  const ObjectId self = receiver(ctx);
  const WorldObject* caster = world->find(self);
  if (caster == nullptr) return none;

  // `self.level + GetConst("HealAmount")`: the very amount the script will heal.
  std::int32_t heal_amount = 0;
  if (EnvSystem* env = env_of(*world); env != nullptr) {
    (void)env->constant("HealAmount", heal_amount);
  }
  const std::int32_t threshold =
      class_int_property(*world, self, "level", 0) + heal_amount;
  const std::int32_t range = class_int_property(*world, self, "range", 0);
  const std::int32_t sight = caster_sight(*world, self);

  ObjectId best = kNoObject;
  std::int64_t best_score = 0;
  std::vector<ObjectId> found;
  world->objects_in_radius(world->resolve_position(self), sight, ClassFilter{}, found);
  for (const ObjectId id : found) {
    const WorldObject* other = world->find(id);
    if (other == nullptr || !caster_candidate_basics(*world, *other, self)) continue;
    // Allies only. `Sentry` and `Peaceful` are **not** tested here, unlike
    // `FindUnitToLearn` -- two entry points three instructions apart in the
    // executable, with different exclusion sets.
    if (world->players().is_enemy(caster->state.owner, other->state.owner)) continue;
    if (is_a(*world, *other, "RamUnit")) continue;
    if (other->state.flags.messenger) continue;
    const std::int32_t max_health = class_int_property(*world, id, "maxhealth", 0);
    if (max_health <= 0 || other->state.health >= max_health) continue;

    const std::int32_t deficit = max_health - other->state.health;
    std::int64_t score = 0;
    if (deficit < threshold) {
      // Too lightly wounded to be worth a whole heal: parked, not rejected.
      score = 1000;
    } else {
      const std::int64_t reach = edge_distance(*world, *caster, *other) - range;
      score = (300LL * other->state.health) / max_health + (reach > 0 ? reach : 0);
    }
    if (best == kNoObject || score < best_score) {
      best = id;
      best_score = score;
    }
  }
  if (best == kNoObject) return none;
  return HostOutcome::ok_with(Value::object(receiver_type(ctx), best));
}

/// `Druid::FindUnitToRevitalize` -- 7 sites, the sole blocker of two scripts,
/// and **heal's twin down to the flat 1000**.
///
/// `0x00513aa0` is `FindUnitToHeal`'s shape exactly: build a predicate from the
/// caster, scan a circle of the caster's own `sight`, keep the *least* score,
/// and fall back to the AI roster when the circle is empty -- which this family
/// declines to model for the reason the section note gives. What differs is the
/// predicate and the score, and both are worth having in full.
///
/// **The predicate** (`0x00511f00`, which never reads the caster it is handed):
/// alive; `health < maxhealth`; **not a `CVXDruid`** and not a `CVXShip`, both
/// tested against the RTTI descriptor rather than by name; not inside a holder;
/// not a heir of `RamUnit`; not a messenger. The scan adds the two it applies
/// itself: the candidate must carry `kSyncUnit`, and its owner must be in the
/// caster's **friendly** mask -- `[record + 0x10] ^ 0xffff`, and `record + 0x10`
/// is the `enemyflags` word `PlayerTable::is_enemy`'s note already names as the
/// exact complement of the relation bit, so the two spellings are one question.
///
/// **The score minimises, and its three terms are all divergent from heal's:**
///
///   * a candidate that `RevitalizeAmount` would take *past* its own
///     `maxhealth` is parked at a flat **1000** rather than rejected -- the same
///     device heal uses for the too-lightly-wounded, pointing the other way. It
///     still wins if nothing better is standing there.
///   * everything else scores `max(0, edgeDistance - range) + 256`. The `+256`
///     is a floor, not a tie-break: it puts every real candidate below the 1000
///     no matter how far away, so the over-heal case really is last.
///   * and a **hero halves its score** (`kSyncHero`, `>> 1`). Nothing in heal
///     does this. A hero has to be more than twice as far away as a warrior
///     before the warrior is preferred.
///
/// `RevitalizeAmount` comes from `CONST.INI [GamePlay]`, read at construction
/// (0x00511fc6) exactly as heal reads `HealAmount` -- and, as there, it is the
/// amount the calling script is about to apply.
HostOutcome fn_find_unit_to_revitalize(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindUnitToRevitalize: no world");
  const auto none = HostOutcome::ok_with(Value::object(script::kNoType, 0));
  const ObjectId self = receiver(ctx);
  const WorldObject* caster = world->find(self);
  if (caster == nullptr) return none;

  std::int32_t revitalize_amount = 0;
  if (EnvSystem* env = env_of(*world); env != nullptr) {
    (void)env->constant("RevitalizeAmount", revitalize_amount);
  }
  const std::int32_t range = class_int_property(*world, self, "range", 0);

  ObjectId best = kNoObject;
  std::int64_t best_score = 0;
  std::vector<ObjectId> found;
  world->objects_in_radius(world->resolve_position(self), caster_sight(*world, self),
                           ClassFilter{}, found);
  for (const ObjectId id : found) {
    const WorldObject* other = world->find(id);
    // The basics carry alive, not-self, not-held, not-hidden and not-a-ship,
    // which is five of this predicate's eleven clauses; the six below are the
    // rest, and two of them (`kSyncUnit` and the side mask) belong to the scan
    // rather than to the predicate in the original.
    if (other == nullptr || !caster_candidate_basics(*world, *other, self)) continue;
    if (!other->state.flags.is_unit) continue;
    if (world->players().is_enemy(caster->state.owner, other->state.owner)) continue;
    if (other->object != nullptr && other->object->is_a(NativeClass::druid)) continue;
    if (is_a(*world, *other, "RamUnit")) continue;
    if (other->state.flags.messenger) continue;
    const std::int32_t max_health = class_int_property(*world, id, "maxhealth", 0);
    if (max_health <= 0 || other->state.health >= max_health) continue;

    std::int64_t score = 0;
    if (other->state.health + revitalize_amount > max_health) {
      // Would overshoot the ceiling: parked at the top of the range, not
      // rejected. Heal's flat 1000 is the same device for the opposite case.
      score = 1000;
    } else {
      const std::int64_t reach = edge_distance(*world, *caster, *other) - range;
      score = (reach > 0 ? reach : 0) + 256;
      if (other->state.flags.is_hero) score /= 2;
    }
    if (best == kNoObject || score < best_score) {
      best = id;
      best_score = score;
    }
  }
  if (best == kNoObject) return none;
  return HostOutcome::ok_with(Value::object(receiver_type(ctx), best));
}

/// `Druid::FindUnitToHide` -- 4 sites, and **it cannot return a target on any
/// data, in the retail build**. It is bound anyway, and the reason it is bound
/// is that the original does not trap.
///
/// `0x00513bb0` is `FindUnitToRevitalize`'s skeleton again: build the predicate,
/// scan a circle of the caster's `sight`, keep the least score. The score is
/// revitalize's without the health term -- `max(0, edgeDistance - range) + 256`,
/// halved for a hero -- and there is no `GetConst` and no park.
///
/// ## The two clauses that decide it are exact complements
///
/// The scan admits a candidate whose owner one-hot is in the mask at
/// `pred[0]`, which `0x00513c12` builds as `[record + 0x10] ^ 0xffff`. The
/// predicate `0x00512140` then calls `0x005a78e0` -- `Obj::IsEnemy`, the same
/// `not`/`and 1` over bit 0 of the caster's own relation row that
/// `PlayerTable::is_enemy` reads -- and **requires it true**.
///
/// `[record + 0x10]` is the enemy mask, and three independent things say so.
/// `sim/player.cpp` already records that `SetRelation` (0x005652f0) maintains a
/// per-player `enemyflags` word as the exact complement of that one bit.
/// `GetBestCrippleTarget`, which targets enemies, passes the **unxored** word
/// (0x00513e6f) to the same `0x00514080` constructor and its predicate
/// (0x00512260) calls the same `0x005a78e0`. And `FindUnitToRevitalize`, which
/// targets allies, passes the xored one and its predicate tests no relation at
/// all.
///
/// So this entry point admits exactly the players it then rejects. A candidate
/// in the friendly mask has bit 0 set and is not an enemy; a candidate that is
/// an enemy is not in the mask. **No object can satisfy both**, and the answer
/// is an invalid handle for every caster on every map.
///
/// It reads as a copy-paste: cripple's predicate with heal's mask. This is the
/// second dead thing in the same feature -- `Sacrifice::IsInvisibility`
/// compares a class name against `"Hide"` and no class named `Hide` ships --
/// and the druid's hide spell is what both of them are for.
///
/// ## Why a body rather than a refusal
///
/// A declared name with no body **traps**, and the original does not: it
/// returns 0xffff and `DRUID_HIDE_GROUND.VS` spends its `RetryCount` retries on
/// it and walks on. Answering nothing is the transcription; trapping is not.
/// The two clauses are written out rather than collapsed into `return none`, so
/// that the emptiness is *derived* from the reading and a revised reading is a
/// one-line change rather than a rewrite.
///
/// **Three exclusions are not transcribed and cannot matter.** The predicate
/// also calls 0x005400c0, 0x005403f0 and 0x00540280, each of which resolves a
/// class by name once and caches it, exactly as the `RamUnit` test at
/// 0x00540460 does; the names are not recovered here. They sit *after* the two
/// clauses above in a conjunction that is already empty, so no data can tell
/// whether they are present. Writing them would be writing tests that cannot
/// fire, which this section already refused once for curse.
HostOutcome fn_find_unit_to_hide(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindUnitToHide: no world");
  const auto none = HostOutcome::ok_with(Value::object(script::kNoType, 0));
  const ObjectId self = receiver(ctx);
  const WorldObject* caster = world->find(self);
  if (caster == nullptr) return none;
  const std::int32_t range = class_int_property(*world, self, "range", 0);

  ObjectId best = kNoObject;
  std::int64_t best_score = 0;
  std::vector<ObjectId> found;
  world->objects_in_radius(world->resolve_position(self), caster_sight(*world, self),
                           ClassFilter{}, found);
  for (const ObjectId id : found) {
    const WorldObject* other = world->find(id);
    if (other == nullptr || !caster_candidate_basics(*world, *other, self)) continue;
    if (!other->state.flags.is_unit) continue;
    if (is_a(*world, *other, "RamUnit")) continue;
    if (other->state.flags.messenger) continue;
    // The scan's mask: allies only.
    if (world->players().is_enemy(caster->state.owner, other->state.owner)) continue;
    // The predicate's `0x005a78e0`: enemies only. Nothing survives both, and
    // the header says at length why that is the reading rather than a slip
    // here. These two lines are the whole finding.
    if (!world->players().is_enemy(caster->state.owner, other->state.owner)) continue;

    const std::int64_t reach = edge_distance(*world, *caster, *other) - range;
    std::int64_t score = (reach > 0 ? reach : 0) + 256;
    if (other->state.flags.is_hero) score /= 2;
    if (best == kNoObject || score < best_score) {
      best = id;
      best_score = score;
    }
  }
  if (best == kNoObject) return none;
  return HostOutcome::ok_with(Value::object(receiver_type(ctx), best));
}

/// `Unit::GetBestCurseTarget` -- the enemy with the highest `attack`.
///
/// No aliveness test, no health, no distance beyond the scan's own radius. The
/// only thing that can lose is an `attack` of zero, because the best starts
/// there and the comparison is strictly greater.
HostOutcome fn_get_best_curse_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetBestCurseTarget: no world");
  const auto none = HostOutcome::ok_with(Value::object(script::kNoType, 0));
  const ObjectId self = receiver(ctx);
  const WorldObject* caster = world->find(self);
  if (caster == nullptr) return none;
  CombatSystem* combat = combat_system_of(*world);

  ObjectId best = kNoObject;
  std::int32_t best_attack = 0;
  std::vector<ObjectId> found;
  world->objects_in_radius(world->resolve_position(self), caster_sight(*world, self),
                           ClassFilter{}, found);
  for (const ObjectId id : found) {
    const WorldObject* other = world->find(id);
    if (other == nullptr || other->id == self) continue;
    if (other->internal != InternalKind::none) continue;
    if (other->state.flags.hidden) continue;
    if (!world->players().is_enemy(caster->state.owner, other->state.owner)) continue;
    if (is_a(*world, *other, "BaseMage")) continue;
    const Combatant* stats = combat == nullptr ? nullptr : combat->find(id);
    const std::int32_t attack =
        stats == nullptr ? 0 : combat->profile(stats->class_index).damage + stats->attack_bonus;
    if (attack <= best_attack) continue;
    best = id;
    best_attack = attack;
  }
  if (best == kNoObject) return none;
  return HostOutcome::ok_with(Value::object(receiver_type(ctx), best));
}

/// `Druid::GetBestCrippleTarget` -- the enemy with the most **stamina**, behind
/// a real predicate.
///
/// Curse's twin and evidently the function it was copied from or to: the same
/// dispatch, the same failure value, a different stat, and -- unlike curse -- a
/// predicate that tests the candidate on every clause.
HostOutcome fn_get_best_cripple_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetBestCrippleTarget: no world");
  const auto none = HostOutcome::ok_with(Value::object(script::kNoType, 0));
  const ObjectId self = receiver(ctx);
  const WorldObject* caster = world->find(self);
  if (caster == nullptr) return none;

  ObjectId best = kNoObject;
  std::int32_t best_stamina = 0;
  std::vector<ObjectId> found;
  world->objects_in_radius(world->resolve_position(self), caster_sight(*world, self),
                           ClassFilter{}, found);
  for (const ObjectId id : found) {
    const WorldObject* other = world->find(id);
    if (other == nullptr || !caster_candidate_basics(*world, *other, self)) continue;
    // **Mutually redundant with the score's own floor, knowingly.**
    // `best_stamina` starts at zero and the comparison below is strictly
    // greater, so an object with no stamina cannot be selected even without
    // this line -- and equally, this line makes that comparison's strictness
    // unobservable. A fault in either survives the suite; only the behaviour
    // can be pinned, and a test pins it. The original writes both, in the
    // predicate and in the score, and so does this.
    if (other->state.stamina <= 0) continue;
    if (!world->players().is_enemy(caster->state.owner, other->state.owner)) continue;
    if (is_a(*world, *other, "RamUnit")) continue;
    if (is_a(*world, *other, "Peaceful")) continue;
    if (other->state.flags.messenger) continue;
    if (other->state.stamina <= best_stamina) continue;
    best = id;
    best_stamina = other->state.stamina;
  }
  if (best == kNoObject) return none;
  return HostOutcome::ok_with(Value::object(receiver_type(ctx), best));
}

/// `Druid::FindUnitBelowILevel(int)` -- an ally whose **inherent** level is
/// strictly below the argument.
///
/// The argument is an absolute ceiling, and the enchantress's script computes it
/// from two constants: `level = GetConst("TeachingLevel1")`, raised to
/// `TeachingLevel2` once `Ancestral Knowledge` is researched.
///
/// *ILevel* is `inherentlevel` and not `level` -- two different numbers that the
/// same script uses side by side, `.level` for a heal amount and
/// `.inherentlevel` for a teaching increment. The original registers a second
/// overload taking an explicit radius; it has no shipped call site and is not
/// bound, on the rectangle family's rule.
HostOutcome fn_find_unit_below_ilevel(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindUnitBelowILevel: no world");
  HeroSystem* heroes = hero_system_of(*world);
  const auto none = HostOutcome::ok_with(Value::object(script::kNoType, 0));
  if (heroes == nullptr) return none;
  const ObjectId self = receiver(ctx);
  const WorldObject* caster = world->find(self);
  if (caster == nullptr) return none;
  const std::int32_t ceiling = int_arg(ctx, 1);

  std::vector<ObjectId> found;
  world->objects_in_radius(world->resolve_position(self), caster_sight(*world, self),
                           ClassFilter{}, found);
  for (const ObjectId id : found) {
    const WorldObject* other = world->find(id);
    if (other == nullptr || !caster_candidate_basics(*world, *other, self)) continue;
    if (world->players().is_enemy(caster->state.owner, other->state.owner)) continue;
    if (is_a(*world, *other, "Sentry")) continue;
    if (other->state.flags.messenger) continue;
    if (!is_a(*world, *other, "Military") && !is_a(*world, *other, "BaseMage")) continue;
    if (heroes->inherent_level(id) >= ceiling) continue;
    return HostOutcome::ok_with(Value::object(receiver_type(ctx), id));
  }
  return none;
}

HostOutcome fn_hero(CallContext& ctx) {
  HERO_STATE(ctx);
  const ObjectId leader = host->heroes->hero_of(receiver(ctx));
  if (leader == kNoObject) return HostOutcome::ok_with(Value::object(script::kNoType, 0));
  return HostOutcome::ok_with(Value::object(receiver_type(ctx), leader));
}

HostOutcome fn_has_freedom(CallContext& ctx) {
  HERO_STATE(ctx);
  // `Squad::HasFreedom` (0x00427aa0): **every** member passes the unit test
  // -- the flag at `[unit+0x198]` or the class fallback, the pair
  // `Unit::HasFreedom` (0x005d7d30) asks in the same order -- and an empty
  // squad vacuously does. `EVALRECRUIT.VS`'s two sites. See `squad_receiver`.
  if (ctx.count() > 0) {
    if (const Squad* squad = squad_receiver(*host->world, ctx.arg(0)); squad != nullptr) {
      for (const ObjectId member : squad->members) {
        const UnitRecord* record = host->heroes->unit(member);
        if (record == nullptr ||
            !(record->has_freedom || unit_has_freedom(*host->world, member))) {
          return HostOutcome::ok_with(Value::boolean(false));
        }
      }
      return HostOutcome::ok_with(Value::boolean(true));
    }
  }
  // The unit test, `Unit::HasFreedom` (0x005d7d30): the specials word's
  // `freedom` bit, which the class fills, or a `RamUnit`. See
  // `unit_has_freedom`. The record's own flag is kept beside it: nothing sets
  // it, and it is what a save carries.
  const ObjectId unit = receiver(ctx);
  const UnitRecord* record = host->heroes->unit(unit);
  return HostOutcome::ok_with(Value::boolean(
      (record != nullptr && record->has_freedom) || unit_has_freedom(*host->world, unit)));
}

HostOutcome fn_hero_armies_full_perc(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::integer(
      host->heroes->armies_full_percent(static_cast<PlayerId>(int_arg(ctx, 0)))));
}

// -- squads ----------------------------------------------------------------

HostOutcome fn_get_squad(CallContext& ctx) {
  HERO_STATE(ctx);
  // `Unit.GetSquad` yields a `Squad` handle, and this is the only place one is
  // minted.
  //
  // It used to hand back `(receiver_type, index)`, which was wrong twice over:
  // it dropped the player, so squad 4 of player 0 and squad 4 of player 14
  // collapsed to one handle -- and the dumps show index 4 live for four players
  // at once -- and it carried an object type id, so a squad would have passed
  // an object class filter. `sim/squad.hpp` packs both halves of `SquadKey`
  // under its own type id.
  const SquadKey key = host->heroes->squad_of(receiver(ctx));
  return HostOutcome::ok_with(pack_squad(key));
}

// -- skills ----------------------------------------------------------------

HostOutcome fn_hero_skill_id(CallContext& ctx) {
  return HostOutcome::ok_with(Value::integer(hero_skill_id(string_arg(ctx, 0))));
}

HostOutcome fn_get_skill(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(
      Value::integer(host->heroes->skill(receiver(ctx), int_arg(ctx, 1))));
}

HostOutcome fn_set_skill(CallContext& ctx) {
  HERO_STATE(ctx);
  const std::int32_t id = int_arg(ctx, 1);
  if (id < 0 || static_cast<std::size_t>(id) >= kHeroSkillCount) return HostOutcome::ok_void();
  host->heroes->set_skill(receiver(ctx), static_cast<HeroSkill>(id), int_arg(ctx, 2));
  return HostOutcome::ok_void();
}

HostOutcome fn_available_skill_points(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(
      Value::integer(host->heroes->available_skill_points(receiver(ctx))));
}

HostOutcome fn_use_skill(CallContext& ctx) {
  HERO_STATE(ctx);
  const std::int32_t id = int_arg(ctx, 1);
  if (id < 0 || static_cast<std::size_t>(id) >= kHeroSkillCount) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(Value::boolean(
      host->heroes->use_skill(*host->world, receiver(ctx), static_cast<HeroSkill>(id))));
}

HostOutcome fn_skill_in_effect(CallContext& ctx) {
  HERO_STATE(ctx);
  const GameTime now = host->world->time();
  const std::int32_t id = int_arg(ctx, 1);
  if (id < 0 || static_cast<std::size_t>(id) >= kHeroSkillCount) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(Value::boolean(
      host->heroes->skill_in_effect(receiver(ctx), static_cast<HeroSkill>(id), now)));
}

// -- progression -----------------------------------------------------------

HostOutcome fn_experience(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::integer(host->heroes->experience(receiver(ctx))));
}

HostOutcome fn_set_experience(CallContext& ctx) {
  HERO_STATE(ctx);
  host->heroes->set_experience(receiver(ctx), int_arg(ctx, 1));
  return HostOutcome::ok_void();
}

/// `u.IncKills(n)` -- one shipped site, `UNIT_ON_KILL.VS`, and it is the sole
/// blocker of that script's 83 call sites.
///
/// `gbr.exe` 0x005d7810 is one `add` to `unit + 0x188`, the field the
/// serialiser walk at 0x005dd710 names `kills` -- the slot immediately after
/// `experience` and `level`, which is why it lives on `UnitRecord` here.
///
/// **Two refusals, both silent.** An unresolvable receiver reports *"The
/// function 'Unit::IncKills' called for an uninitialized or invalid object."*
/// and a dead one reports *"called for a dead object"*, and both then return
/// having written nothing. The dead test is the unit's `IsDead` virtual
/// (vtable `+0x50`, `health == 0`), the same one `Obj::IsDead` negates -- so a
/// unit that dies in the same tick as the kill it scored does not get the
/// credit.
///
/// The counter is not clamped in either direction, because the original does
/// not clamp it. The one shipped site passes 1.
HostOutcome fn_inc_kills(CallContext& ctx) {
  HERO_STATE(ctx);
  const ObjectId id = receiver(ctx);
  const WorldObject* slot = host->world->find(id);
  if (slot == nullptr || slot->state.health <= 0) return HostOutcome::ok_void();
  (void)host->heroes->add_kills(id, int_arg(ctx, 1));
  return HostOutcome::ok_void();
}

HostOutcome fn_inherent_level(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::integer(host->heroes->inherent_level(receiver(ctx))));
}

HostOutcome fn_level(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::integer(host->heroes->level(receiver(ctx))));
}

HostOutcome fn_set_level(CallContext& ctx) {
  HERO_STATE(ctx);
  host->heroes->set_level(receiver(ctx), int_arg(ctx, 1));
  return HostOutcome::ok_void();
}

// -- items -----------------------------------------------------------------

HostOutcome fn_add_item(CallContext& ctx) {
  HERO_STATE(ctx);
  const ObjectId item =
      host->heroes->items().add(*host->world, receiver(ctx), string_arg(ctx, 1));
  return HostOutcome::ok_with(Value::boolean(item != kNoObject));
}

HostOutcome fn_has_item(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(
      Value::boolean(host->heroes->items().has_type(receiver(ctx), string_arg(ctx, 1))));
}

HostOutcome fn_find_item(CallContext& ctx) {
  HERO_STATE(ctx);
  const ObjectId item = host->heroes->items().find_of_type(receiver(ctx), string_arg(ctx, 1));
  if (item == kNoObject) return HostOutcome::ok_with(Value::object(script::kNoType, 0));
  return HostOutcome::ok_with(Value::object(receiver_type(ctx), item));
}

/// `o.GetItem(n)` -- 1 site, and the sole blocker of `UNIT_ON_ENTER.VS` and its
/// 23 call sites.
///
/// 0x005ad310 **decrements the argument** and hands it to `vtbl+0xf4`, so the
/// index is 1-based: `GetItem(1)` is the first item. An index outside the
/// inventory answers the invalid handle `0xffff`, which is what the miss path
/// three lines above pushes for a receiver that does not resolve -- so a script
/// walking `for (i = 1; i <= o.item_count; ++i)` and one walking past the end
/// get the same shape of answer.
///
/// The order is `ItemStore::contents_for`'s, which is what `RemoveItem`
/// already indexes into from the other side.
HostOutcome fn_get_item(CallContext& ctx) {
  HERO_STATE(ctx);
  const auto none = HostOutcome::ok_with(Value::object(script::kNoType, 0));
  if (ctx.count() < 2 || !ctx.arg(1).is_integer()) return none;
  const std::int32_t asked = ctx.arg(1).as_integer();
  // Redundant with the bound below -- an index of zero or less wraps the
  // subtraction to a huge `size_t` and fails it -- and kept because it is the
  // rule rather than a consequence of one. The original's `dec` has the same
  // property and the same reliance on the callee to bounds-check.
  if (asked < 1) return none;
  std::vector<ObjectId> contents;
  host->heroes->items().contents_for(receiver(ctx), contents);
  const std::size_t at = static_cast<std::size_t>(asked - 1);
  if (at >= contents.size()) return none;
  return HostOutcome::ok_with(Value::object(receiver_type(ctx), contents[at]));
}

HostOutcome fn_item_count(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::integer(host->heroes->items().count_for(receiver(ctx))));
}

/// `u.OpenItemHolder(holder)` -- 2 sites, and it is a **transfer**, not a
/// window.
///
/// `gbr.exe` 0x005acba0 guards three things and then hands the work to
/// 0x0053c350, which is the whole of it:
///
///     for (i = holder.slots - 1; i >= 0; --i) {
///         if (receiver.count >= receiver.capacity) break;
///         item = holder.at(i); holder.clear(i);
///         if (receiver.add(item)) { ++moved; notify both sides; }
///     }
///     if (moved && holder.class.delete_empty == 1 && holder.slots == 0)
///         holder.destroy();
///     return moved;
///
/// So **the bool is "at least one item actually moved"**, not "a screen
/// opened", and the loop runs **back to front** -- which is observable the
/// moment the receiver fills up, because the holder keeps its lowest slots.
///
/// **The one thing here that is deliberately left out is the only thing that
/// was ever user interface.** On success the original also calls
/// 0x005a93a0(receiver, "ItemGotFromHolder"), which projects the object into
/// screen space against the camera rectangle, tests the *local* player's fog,
/// and floats a caption above the unit. Nothing about the return value or the
/// world state depends on it.
///
/// **A rejected item is dropped in the original and is not here.** It clears
/// the holder's slot *before* offering the item, so an `add` that refuses for a
/// reason other than fullness loses the item; `ItemStore::give` is atomic and
/// leaves it where it was. The loop's own capacity check makes fullness
/// unreachable as a rejection, so no shipped call can tell the two apart.
///
/// Three refusals, all answering **false** and all silent: an invalid receiver,
/// a *dead* receiver (the `IsDead` virtual, the same one `IncKills` consults),
/// and an invalid holder. There is no distance check anywhere -- both shipped
/// sites `Goto` the holder themselves first.
HostOutcome fn_open_item_holder(CallContext& ctx) {
  HERO_STATE(ctx);
  const ObjectId taker_id = receiver(ctx);
  const ObjectId holder = object_arg(ctx, 1);
  const WorldObject* taker = host->world->find(taker_id);
  const WorldObject* chest = host->world->find(holder);
  if (taker == nullptr || chest == nullptr || taker->state.health <= 0) {
    return HostOutcome::ok_with(Value::boolean(false));
  }

  ItemStore& items = host->heroes->items();
  std::vector<ObjectId> contents;
  (void)items.contents_for(holder, contents);
  // **The capacity check is the original's structure and is redundant here**,
  // which fault injection says outright: `ItemStore::give` refuses a full
  // destination itself, so removing the `break` -- or reading the *holder's*
  // capacity instead of the taker's -- moves exactly the same items and leaves
  // exactly the same ones behind. It is kept because it is the loop's stated
  // stop condition in 0x0053c350 and because the original's `add` is not known
  // to make the same check; recorded rather than dropped, on
  // `spawn_group_into_list`'s precedent for a redundancy nothing can observe.
  const std::int32_t capacity = items.capacity_of(*host->world, taker_id);
  std::int32_t moved = 0;
  for (std::size_t i = contents.size(); i-- > 0;) {
    if (items.count_for(taker_id) >= capacity) break;
    if (items.give(*host->world, contents[i], taker_id)) ++moved;
  }

  // `delete_empty="1"` -- `DEFITEMHOLDER.SC.XML`, the prop a dropped item
  // makes, against `DEADTREE` and `TREETRUNK` which declare 0 and stay.
  if (moved > 0 && items.count_for(holder) == 0 &&
      class_flag(*host->world, *chest, "delete_empty")) {
    (void)host->world->despawn(holder);
  }
  return HostOutcome::ok_with(Value::boolean(moved > 0));
}

HostOutcome fn_give_item(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::boolean(
      host->heroes->items().give(*host->world, object_arg(ctx, 1), object_arg(ctx, 2))));
}

HostOutcome fn_put_item(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::boolean(
      host->heroes->items().put(*host->world, object_arg(ctx, 1), object_arg(ctx, 2))));
}

/// The **ambient item**: what `This` is for a script the engine started on an
/// item, which is the receiver the three free item entry points take.
///
/// `GetUseCount()` and `ItemUsed(n)` have **no arguments naming an item**. They
/// read the running frame's `This` out of a global (0xa77ebc), cast it to the
/// item interface and follow the item's own object handle at `+0x24`; if that
/// cast fails they print -- *"Function GetUseCount called in object that is not
/// item"* -- and answer 0, or in `ItemUsed`'s case return having done nothing.
///
/// Here that is the running script's owner, which is the same thing: every
/// shipped item script -- `HEALING WATER.VS`, `RING OF POWER.VS` -- is started
/// on the item it belongs to.
///
/// **The "is it actually an item" test is deliberately not here.** It was, and
/// no injected fault could reach it: `ItemStore::use_count` answers 0 and
/// `ItemStore::spend` answers false for a handle that names no item, so
/// checking first changed no answer anywhere. A branch nothing can reach reads
/// as a claim this code is not making. The guard is the store's, and this
/// returns the owner as it stands -- `kNoObject` for a script with none.
[[nodiscard]] ObjectId ambient_item(CallContext& ctx) noexcept {
  if (ctx.scheduler == nullptr) return kNoObject;
  const script::ScriptRecord* record = ctx.scheduler->find(ctx.script);
  return record == nullptr ? kNoObject : record->owner.id;
}

/// `GetUseCount()` -- 10 sites, every one of them in an item's own script.
///
/// Not an item, or no item behind the handle, is **0** and not a refusal:
/// `0x00534aa0` prints and pushes zero, and `HEALING WATER.VS` runs straight
/// on into `if (nHealth == 0)`.
HostOutcome fn_get_use_count(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(
      Value::integer(host->heroes->items().use_count(ambient_item(ctx))));
}

/// `ItemUsed(n)` -- 12 sites. See `ItemStore::spend` for the rule, which is not
/// `use`'s: no clamp, no consumable test, and `n` may be negative.
HostOutcome fn_item_used(CallContext& ctx) {
  HERO_STATE(ctx);
  const std::int32_t charges =
      ctx.arg(0).is_integer() ? static_cast<std::int32_t>(ctx.arg(0).as_integer()) : 0;
  (void)host->heroes->items().spend(*host->world, ambient_item(ctx), charges);
  return HostOutcome::ok_void();
}

/// `Item::use_count` (0x00535000) and `Item::SetUseCount` (0x00535100) -- the
/// same counter as the free forms, reached by an explicit handle instead of by
/// the ambient `This`. `CMERCENARY_ONDIE.VS` uses the pair as a purse:
/// `item.SetUseCount(item.use_count + gold)`.
///
/// **The two ways of missing answer differently**, which is the one thing here
/// worth a test: an unresolvable *handle* is **-1**, while a handle that
/// resolves to something that is not an item is **0**. Both print the same
/// message, and it names `Item::GetUseCount` even though the entry point is
/// spelled `use_count`.
HostOutcome m_item_use_count(CallContext& ctx) {
  HERO_STATE(ctx);
  const ObjectId receiver_id = object_arg(ctx, 0);
  if (host->world->find(receiver_id) == nullptr) {
    return HostOutcome::ok_with(Value::integer(-1));
  }
  const ItemInstance* instance = host->heroes->items().find(receiver_id);
  return HostOutcome::ok_with(Value::integer(instance == nullptr ? 0 : instance->usecount));
}

/// `Item::SetUseCount(v)` -- a plain store, **without the clamp**
/// `ItemStore::set_use_count` applies: `0x00535100` writes `[obj+0x28] = v` and
/// nothing else, so a script can park a negative there and read it back.
HostOutcome m_item_set_use_count(CallContext& ctx) {
  HERO_STATE(ctx);
  ItemInstance* instance = host->heroes->items().find(object_arg(ctx, 0));
  if (instance == nullptr) return HostOutcome::ok_void();
  instance->usecount =
      ctx.arg(1).is_integer() ? static_cast<std::int32_t>(ctx.arg(1).as_integer()) : 0;
  return HostOutcome::ok_void();
}

/// `Obj::max_items` (0x005ad3f0) -- inventory capacity, and the other half of
/// the two comparisons the corpus makes: `if (u.item_count < u.max_items)` in
/// `TS_ATTACKATWILL.VS` and `RUIN_BEHAVIOR.VS`, which is *"has this unit room
/// for the loot it is about to walk to"*.
///
/// A virtual on the object returning an int, which for every class the shipped
/// content asks about is the class's own `inventory_size`. An unresolvable
/// receiver prints and answers **0** -- which is the safe direction here, since
/// `item_count < 0` is false and the unit is simply not sent for the item.
HostOutcome fn_max_items(CallContext& ctx) {
  HERO_STATE(ctx);
  const WorldObject* slot = host->world->find(object_arg(ctx, 0));
  const ClassGraph* graph = host->world->class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  return HostOutcome::ok_with(Value::integer(
      parse_int(graph->property(slot->class_index, "inventory_size"), 0)));
}

HostOutcome fn_use_item(CallContext& ctx) {
  HERO_STATE(ctx);
  // `Unit.UseItem(item)` runs the item's `use_script`, which is the script
  // scheduler's job; the charge it spends is this one's. A `use_script` that
  // calls `ItemUsed(n)` spends more, which is why this is one charge and not
  // the whole count.
  return HostOutcome::ok_with(
      Value::boolean(host->heroes->items().use(*host->world, object_arg(ctx, 1), 1)));
}

HostOutcome fn_remove_item(CallContext& ctx) {
  HERO_STATE(ctx);
  // Overloaded at the call sites: `RemoveItem(index)` in `Spoils of War`,
  // `RemoveItem("God's Gift")` in the kill script for it.
  if (!ctx.arg(1).is_integer()) {
    return HostOutcome::ok_with(Value::boolean(
        host->heroes->items().remove_all_of_type(*host->world, receiver(ctx),
                                                 string_arg(ctx, 1)) > 0));
  }
  return HostOutcome::ok_with(Value::boolean(host->heroes->items().remove_at(
      *host->world, receiver(ctx), static_cast<std::size_t>(int_arg(ctx, 1)))));
}

HostOutcome fn_remove_items_of_type(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::integer(host->heroes->items().remove_all_of_type(
      *host->world, receiver(ctx), string_arg(ctx, 1))));
}

HostOutcome fn_get_item_index(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(
      host->heroes->items().index_of(receiver(ctx), object_arg(ctx, 1)))));
}

HostOutcome fn_exchange_item(CallContext& ctx) {
  HERO_STATE(ctx);
  return HostOutcome::ok_with(
      Value::boolean(host->heroes->items().exchange(object_arg(ctx, 1), string_arg(ctx, 2))));
}

HostOutcome fn_drop_item(CallContext& ctx) {
  HERO_STATE(ctx);
  // The point argument is a host `point` value this system does not own, so the
  // item is dropped where its owner stands, which is where the shipped scripts
  // walk to before calling it anyway.
  const ObjectId item = object_arg(ctx, 1);
  const Point at = host->world->resolve_position(receiver(ctx));
  return HostOutcome::ok_with(
      Value::boolean(host->heroes->items().drop(*host->world, item, at)));
}

// `GetUseCount()` and `ItemUsed(n)` are free functions with no receiver: an
// item script runs *as* the item, so the item it acts on is ambient. Which item
// that is belongs to the script scheduler's frame, not to this system, so these
// two are deliberately left undefined and will trap by name until the scheduler
// can say. Defining them to guess would be a silent wrong answer.

/// `hero.GetFinalPartyOrientation()` (3 sites), `SetFinalPartyOrientation(pt)`
/// (1) and `HasFinalPartyOrientationRequest()` (1) -- **three entry points over
/// two slots, and they do not read each other's**.
///
/// Measured at 0x0052f6e0, 0x0052f7e0 and 0x0052f850, all on the party record
/// `[hero+0x2b0]`. `Set` writes `pt - (1024, 1024)` into `+0x54/+0x58`; `Has`
/// answers `(+0x54 | +0x58) != 0`; `Get` reads a **different pair**,
/// `+0x5c/+0x60`, and adds `(1024, 1024)` back. Between them sits the
/// party-formation code, which settles a request into the second pair when a
/// formation completes -- and this engine does not model formation, so the
/// settled slot is written by nothing.
///
/// **So `Get` answers `(1024, 1024)` today, and that is the right answer rather
/// than a placeholder.** It is what the original answers for any hero whose
/// party has not finished forming, which is most heroes most of the time, and
/// it is exactly what the three readers are shaped for:
///
///     dpt = .GetFinalPartyOrientation() - Point(1024, 1024);
///     if (dpt.x != 0 || dpt.y != 0) ...
///
/// The bias is why: the language has no nullable point, so "unset" is zero in
/// storage and a positive pair to the script.
///
/// A receiver that names nothing answers `(-1, -1)` from `Get` -- the original
/// prints and pushes that -- `false` from `Has`, and does nothing in `Set`.
/// `HERO_MOVE.VS` subtracts the bias from `(-1, -1)` and gets a large negative
/// point, which is not zero, so a dead hero takes the *oriented* branch there.
/// Transcribed rather than tidied: it is what the original does.
constexpr std::int32_t kPartyOrientationBias = 1024;

HostOutcome fn_get_final_party_orientation(CallContext& ctx) {
  HERO_STATE(ctx);
  const HeroRecord* record = host->heroes->hero(receiver(ctx));
  if (record == nullptr) return HostOutcome::ok_with(pack_point(Point{-1, -1}));
  return HostOutcome::ok_with(pack_point(Point{
      record->party_orientation.x + kPartyOrientationBias,
      record->party_orientation.y + kPartyOrientationBias}));
}

HostOutcome fn_set_final_party_orientation(CallContext& ctx) {
  HERO_STATE(ctx);
  HeroRecord* record = host->heroes->hero(receiver(ctx));
  if (record == nullptr) return HostOutcome::ok_void();
  if (ctx.count() < 2 || !is_point(ctx.arg(1))) return HostOutcome::ok_void();
  const Point wanted = unpack_point(ctx.arg(1));
  record->party_orientation_request =
      Point{wanted.x - kPartyOrientationBias, wanted.y - kPartyOrientationBias};
  return HostOutcome::ok_void();
}

HostOutcome fn_has_final_party_orientation_request(CallContext& ctx) {
  HERO_STATE(ctx);
  const HeroRecord* record = host->heroes->hero(receiver(ctx));
  if (record == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  // The original ORs the two words and tests the result, so a request of
  // exactly `(1024, 1024)` -- which stores as `(0, 0)` -- reads as no request
  // at all. That is the bias's whole purpose and not an edge case.
  return HostOutcome::ok_with(Value::boolean(record->party_orientation_request.x != 0 ||
                                                     record->party_orientation_request.y != 0));
}

#undef HERO_STATE

}  // namespace

void define_hero_host(script::HostRegistry& registry) {
  using script::CallKind;
  const auto member = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::member, name, arity, fn);
  };
  const auto free_fn = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::free_function, name, arity, fn);
  };

  // Ordered by call frequency in docs/formats/vs-host-api.md.
  member("hero", 0, fn_hero);                        // 151
  member("experience", 0, fn_experience);            // 101
  member("inherentlevel", 0, fn_inherent_level);     // 70
  member("SetLevel", 1, fn_set_level);               // 35
  member("AddItem", 1, fn_add_item);                 // 32
  member("SetExperience", 1, fn_set_experience);     // 26
  member("IncKills", 1, fn_inc_kills);               //  1
  member("HasItem", 1, fn_has_item);                 // 26
  member("level", 0, fn_level);                      // 20
  member("GetSquad", 0, fn_get_squad);               // 15
  member("GetSkill", 1, fn_get_skill);               // 12
  member("UseItem", 1, fn_use_item);                 // 11
  member("HasFreedom", 0, fn_has_freedom);           // 10
  member("DetachFrom", 1, fn_detach_from);           // 8
  member("SkillInEffect", 1, fn_skill_in_effect);    // 6
  member("DetachArmy", 0, fn_detach_army);           // 6
  member("RemoveItemsOfType", 1, fn_remove_items_of_type);  // 5
  member("item_count", 0, fn_item_count);            // 5
  member("GetItem", 1, fn_get_item);                 // 1, and it unblocks 23
  member("use_count", 0, m_item_use_count);          // 3
  member("SetUseCount", 1, m_item_set_use_count);    // 3
  member("max_items", 0, fn_max_items);              // 3
  registry.define(CallKind::free_function, "ItemUsed", 1, &fn_item_used);        // 12
  registry.define(CallKind::free_function, "GetUseCount", 0, &fn_get_use_count); // 10
  member("SetSkill", 2, fn_set_skill);               // 4
  member("FindItem", 1, fn_find_item);               // 4
  member("HasArmy", 0, fn_has_army);                 // 4
  member("IsArmyOutside", 0, fn_is_army_outside);    // 3, the sole blocker of all three
  member("GiveItem", 2, fn_give_item);               // 4
  member("OpenItemHolder", 1, fn_open_item_holder);  //  2
  member("SetAutocast", 1, fn_set_autocast);        // 34, all in containers
  member("autocast", 0, fn_autocast);
  member("FindUnitToLearn", 0, fn_find_unit_to_learn);  // 480 hits, one script
  // The four caster searches. See the section comment for the two that score.
  member("FindUnitToHeal", 0, fn_find_unit_to_heal);              // 136 hits
  member("FindUnitToRevitalize", 0, fn_find_unit_to_revitalize);  //   7 sites
  member("FindUnitToHide", 0, fn_find_unit_to_hide);              //   4, and dead in retail
  member("GetBestCurseTarget", 0, fn_get_best_curse_target);      //  36
  member("FindUnitBelowILevel", 1, fn_find_unit_below_ilevel);    //  23
  member("GetBestCrippleTarget", 0, fn_get_best_cripple_target);  //  10
  member("AttachTo", 1, fn_attach_to);
  member("IsHeroArmyFull", 0, fn_is_hero_army_full);
  member("maxarmy", 0, fn_max_army);                 // 1
  registry.define(CallKind::free_function, "GetRandomHeroClass", 2,
                  &fn_get_random_hero_class);  // 4, and it unblocks 54
  member("TimePastLastAttack", 0, fn_time_past_last_attack);  // 4
  // Three entry points over two slots; see `fn_get_final_party_orientation`.
  member("GetFinalPartyOrientation", 0, fn_get_final_party_orientation);            // 3
  member("SetFinalPartyOrientation", 1, fn_set_final_party_orientation);            // 1
  member("HasFinalPartyOrientationRequest", 0, fn_has_final_party_orientation_request);  // 1
  // `Hero::LastAttacker/0` is **not** bound, and is not even declared: its one
  // shipped line sits inside `HERO_SNEAK.VS`'s `/* */` block, so the census
  // sees no call site and `declare_shipped_surface` has no entry to fill. On
  // `ShowNotes`'s precedent that settles it. The field it would read is
  // `HeroRecord::army_attacked_unit`, which is kept because the original writes
  // it in the same two instructions as the stamp and because the comment beside
  // that dead call is the best evidence in the corpus for what either means.

  member("AvailableSkillPoints", 0, fn_available_skill_points);
  member("UseSkill", 1, fn_use_skill);
  member("PutItem", 2, fn_put_item);
  member("DropItem", 2, fn_drop_item);
  member("RemoveItem", 1, fn_remove_item);
  member("ExchangeItem", 2, fn_exchange_item);
  member("GetItemIndex", 1, fn_get_item_index);

  free_fn("HeroSkillId", 1, fn_hero_skill_id);       // 2
  free_fn("HeroArmiesFullPerc", 1, fn_hero_armies_full_perc);  // 1
}

}  // namespace imperivm::core::sim
