#include "imperivm/core/sim/stats.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/item.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

/// `Toughness`'s index in the 36-entry specials table (`[unit+0x198]` bit 9).
constexpr std::int32_t kSpecialToughness = 9;

/// The formation loader (0x005f7c30) clamps each bonus it reads to
/// `[-1000, 1000]`; `FormationTable` keeps the file's number, so the clamp is
/// taken where the rebuild reads it.
constexpr std::int32_t kFormationBonusLimit = 1000;

[[nodiscard]] std::int32_t formation_bonus(std::int32_t value) noexcept {
  return std::clamp(value, -kFormationBonusLimit, kFormationBonusLimit);
}

/// `value * percent / 100`, truncated toward zero: the multiply-by-reciprocal
/// the original divides by 100 with rounds toward zero for either sign, which
/// is what C++'s integer division does.
[[nodiscard]] std::int32_t percent_of(std::int32_t value, std::int32_t percent) noexcept {
  return static_cast<std::int32_t>(static_cast<std::int64_t>(value) * percent / 100);
}

/// Whether the unit's current command is `stand_position`: the command at the
/// head of its queue, once it has started, by its exact name. The original
/// keeps the name in `[obj+0x110]`, written when a command starts and cleared
/// when it ends (0x005b4f20), which is the started head here.
[[nodiscard]] bool standing_in_formation(World& world, ObjectId id) {
  const CommandSystem* commands = command_system(world);
  if (commands == nullptr) return false;
  const CommandQueue* queue = commands->find(id);
  if (queue == nullptr) return false;
  const Command* head = queue->running();
  return head != nullptr && head->started && head->verb == kStandPositionCommand;
}

/// The leader's party formation: `[[leader+0x2b0]+0x18]`, the shape
/// `Hero::SetFormation` chose, which is the movement record's `formation`
/// here, or the file's default for a hero never given one.
[[nodiscard]] const FormationClassDef* party_formation(World& world, ObjectId leader) {
  const MovementSystem* movement = movement_system(world);
  if (movement == nullptr) return nullptr;
  const MoveState* move = movement->find(leader);
  if (move != nullptr && !move->formation.empty()) {
    if (const FormationClassDef* chosen = movement->formations().find(move->formation)) {
      return chosen;
    }
  }
  return movement->formations().default_formation();
}

/// Whether the owner's environment entry `Statue of Amon-Ra` reads exactly
/// `researched` -- the key `/<map>/Player<n>/Statue of Amon-Ra`, compared
/// byte for byte (0x005da29e..0x005da328).
[[nodiscard]] bool owner_has_amon_ra(World& world, PlayerId owner) {
  if (owner == kNoPlayer) return false;
  const EnvSystem* env = env_of(world);
  if (env == nullptr) return false;
  const EnvScope scope = EnvScope::for_player(player_to_script(owner));
  return env->env().read_string(scope, "Statue of Amon-Ra") == "researched";
}

[[nodiscard]] std::int32_t points(const HeroRecord& hero, HeroSkill which) noexcept {
  return hero.skills[static_cast<std::size_t>(which)];
}

/// A timed skill's slot is set: the original tests `[hero+0x218+4*skill]`
/// against -1, so a skill is "in effect" for the rebuild from its start until
/// the expiry pass clears the slot, not until a clock passes its end.
[[nodiscard]] bool in_effect(const HeroRecord& hero, HeroSkill which) noexcept {
  return hero.effect_until[static_cast<std::size_t>(which)] != 0;
}

}  // namespace

void recalc_bonuses(World& world, ObjectId id) {
  CombatSystem* combat = combat_system_of(world);
  if (combat == nullptr) return;
  if (!combat->world_bound()) {
    combat->recalc(id);
    return;
  }
  Combatant* unit = combat->find(id);
  const WorldObject* slot = world.find(id);
  if (unit == nullptr || slot == nullptr || !slot->state.flags.is_unit) return;
  // `vtbl+0x50`: a dead unit keeps what it had.
  if (!unit->alive || unit->action == Action::dying) return;

  const CombatProfile cls = combat->profile(unit->class_index);
  HeroSystem* heroes = hero_system_of(world);
  const HeroConstants constants = heroes != nullptr ? heroes->constants() : HeroConstants{};

  // The class, then the `AddBonus` record.
  StatBlock next = combat->class_stats(*unit);
  // The experience the level is read from -- the running total plus, under a
  // hero, the hero's army addend -- and the additive levels.
  std::int32_t experience = heroes != nullptr ? heroes->experience(id) : 0;
  if (heroes != nullptr) {
    if (const HeroRecord* own_hero = heroes->hero(heroes->hero_of(id)); own_hero != nullptr) {
      experience += own_hero->army_addend;
    }
  }
  std::int32_t level_add = 0;

  // The items, each kind once, percents of the class's own numbers.
  if (heroes != nullptr && heroes->items().catalog() != nullptr) {
    const ItemStore& items = heroes->items();
    const ItemCatalog& catalog = *items.catalog();
    std::vector<ObjectId> held;
    items.contents_for(id, held);
    std::vector<const ItemDefinition*> earlier;
    for (const ObjectId item : held) {
      const ItemInstance* instance = items.find(item);
      const ItemDefinition* def = instance != nullptr ? catalog.at(instance->type) : nullptr;
      if (def == nullptr) continue;
      const bool repeated = std::any_of(earlier.begin(), earlier.end(),
                                        [def](const ItemDefinition* seen) {
                                          return seen->id == def->id;
                                        });
      earlier.push_back(def);
      if (repeated) continue;
      const ItemBonus& b = def->bonus;
      next.attack += percent_of(cls.damage, b.damage_percent);
      next.armour_slash += percent_of(cls.armor_slash, b.armor_slash_percent);
      next.armour_pierce += percent_of(cls.armor_pierce, b.armor_pierce_percent);
      next.max_health += percent_of(cls.max_health, b.health_percent);
      next.attack += b.damage;
      next.armour_slash += b.armor_slash;
      next.armour_pierce += b.armor_pierce;
      next.max_health += b.health;
      level_add += b.level;
      experience += b.experience;
    }
  }

  // Toughness and the Statue of Amon-Ra, by the level the unit's own running
  // experience stands at.
  const std::int32_t own_level =
      heroes != nullptr ? heroes->level_for_experience(heroes->experience(id)) : 1;
  if (object_has_special(world, id, kSpecialToughness)) {
    next.max_health += (own_level - 1) * constants.toughness_health_increase;
  }
  if (owner_has_amon_ra(world, slot->state.owner)) {
    next.max_health += (own_level - 1) * constants.statue_of_amon_ra_health_increase;
  }

  // A hero's own skills, and from here on the hero is its own leader.
  const bool is_hero = slot->state.flags.is_hero;
  const HeroRecord* leader = nullptr;
  if (heroes != nullptr) {
    leader = is_hero ? heroes->hero(id) : heroes->hero(heroes->hero_of(id));
  }
  if (is_hero && leader != nullptr) {
    next.max_health +=
        constants.health_per_epic_endurance_level * points(*leader, HeroSkill::epic_endurance);
    next.attack += constants.attack_per_epic_attack_level * points(*leader, HeroSkill::epic_attack);
    const std::int32_t armour =
        constants.defense_per_epic_armor_level * points(*leader, HeroSkill::epic_armor);
    next.armour_slash += armour;
    next.armour_pierce += armour;
  }

  if (leader != nullptr) {
    // The formation, while the unit stands in it; the floor at zero is this
    // branch's alone.
    if (standing_in_formation(world, id)) {
      if (const FormationClassDef* form = party_formation(world, leader->id)) {
        next.attack += formation_bonus(form->bonus_damage);
        next.armour_slash += formation_bonus(form->bonus_defence_slash);
        next.armour_pierce += formation_bonus(form->bonus_defence_pierce);
        next.range += percent_of(next.range, formation_bonus(form->bonus_range));
        if (!is_hero) level_add += formation_bonus(form->bonus_level);
      }
      next.attack = std::max(next.attack, 0);
      next.armour_slash = std::max(next.armour_slash, 0);
      next.armour_pierce = std::max(next.armour_pierce, 0);
      next.range = std::max(next.range, 0);
    }
    // The leader's Team skills -- a hero's reach itself.
    next.attack += constants.attack_per_team_attack_level * points(*leader, HeroSkill::team_attack);
    const std::int32_t defense =
        constants.defense_per_team_defense_level * points(*leader, HeroSkill::team_defense);
    next.armour_slash += defense;
    next.armour_pierce += defense;
    if (in_effect(*leader, HeroSkill::defensive_cry)) {
      next.armour_slash += constants.defensive_cry_defense;
      next.armour_pierce += constants.defensive_cry_defense;
    }
    if (!is_hero && in_effect(*leader, HeroSkill::battle_cry)) {
      level_add += constants.levels_per_battle_cry_level * points(*leader, HeroSkill::battle_cry);
    }
    if (!is_hero && in_effect(*leader, HeroSkill::frenzy)) next.attack *= 2;
  }

  // The level: the table's, plus the additive levels, at least 1, then the
  // unit's own hero's Discipline as a floor (never a hero's: it has none).
  if (heroes != nullptr) {
    std::int32_t level = heroes->level_for_experience(experience) + level_add;
    if (level < 1) level = 1;
    if (const HeroRecord* own_hero = heroes->hero(heroes->hero_of(id)); own_hero != nullptr) {
      const std::int32_t discipline = points(*own_hero, HeroSkill::discipline);
      if (discipline > 0) {
        level = std::max(level, constants.discipline_bonus_base +
                                    constants.discipline_bonus_step * discipline);
      }
    }
    // The sprite refresh a changed level asks for (`vtbl+0x5c`) is
    // presentation and is not modelled.
    unit->level = level;
  }
  combat->store_stats(*unit, next);
}

void recalc_party(World& world, ObjectId hero) {
  const HeroSystem* heroes = hero_system_of(world);
  if (heroes == nullptr) return;
  const HeroRecord* record = heroes->hero(hero);
  if (record == nullptr) return;
  // A snapshot: a rebuild touches nothing in the army, but the copy keeps the
  // walk honest against any that did.
  const std::vector<ObjectId> army = record->army;
  recalc_bonuses(world, hero);
  for (const ObjectId member : army) recalc_bonuses(world, member);
}

}  // namespace imperivm::core::sim
