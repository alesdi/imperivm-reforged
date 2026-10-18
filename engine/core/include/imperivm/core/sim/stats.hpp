#pragma once
/// A unit's stored stats and their rebuild -- `Unit::RecalcBonuses`'s virtual.
///
/// The original keeps maximum health and stamina, range, attack, the two
/// armours and the level on the object, and every reader -- the host entry
/// points, the blow, the reach test -- reads the stored field. A bonus
/// reaches them only when something rebuilds the fields, so the rebuild and
/// the moments it runs are as much the rule as the arithmetic.
/// `docs/formats/unit-stats.md` is the model, with the address of each step
/// and each trigger; this is the engine's implementation of it.
#include <string_view>

#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

/// Rebuild unit `id`'s stored stats (`Combatant::stats`) from everything the
/// original's rebuild (0x005d9df0) reads, in its order:
///
///   1. the class's six numbers, then `Unit::AddBonus`'s record on top
///      (`CombatSystem::class_stats`);
///   2. the items it holds, in inventory order, **each kind once** -- an item
///      of a kind already counted earlier in the inventory adds nothing --
///      every percent of the *class* value and truncated, then the flat terms;
///   3. `Toughness` (special 9) and the owner's researched `Statue of
///      Amon-Ra`: `(level - 1) * 5` maximum health each, the level being the
///      one the unit's own running experience stands at;
///   4. a hero's Epic Endurance, Epic Attack and Epic Armor, on itself;
///   5. with a leader -- a hero is its own, anyone else its hero, and a unit
///      with neither stops here -- the formation bonus while the unit's
///      running command is `stand_position` (damage, both armours, range by a
///      percentage, then those four floored at 0), the leader's Team Attack
///      and Team Defense, Defensive Cry while it lasts, and, for a unit that
///      is not a hero, Frenzy's doubled attack while it lasts;
///   6. the store, which rescales health and stamina by the new maximum over
///      the old (`CombatSystem::store_stats`).
///
/// The level steps of the same rebuild -- items' level and experience,
/// Battle Cry, the formation's level, the hero's army addend, Discipline --
/// are not stored yet: `HeroSystem::level` still computes them when asked.
///
/// Nothing for an id that is not a living combatant unit of a world-bound
/// combat system: a building keeps its class numbers, a dead unit what it had.
void recalc_bonuses(World& world, ObjectId id);

/// The same for every member of `hero`'s party, the hero first and then its
/// army in attach order: what 0x005f5520 does with the rebuild thunk
/// 0x0052d8e0 when a Team skill or Discipline is written and when a timed
/// skill starts or ends.
void recalc_party(World& world, ObjectId hero);

/// The command name that puts a unit in formation for the rebuild: compared
/// exactly, as the original compares the fifteen bytes of `stand_position`
/// and its terminator with the unit's current command (0x005da46b).
inline constexpr std::string_view kStandPositionCommand = "stand_position";

}  // namespace imperivm::core::sim
