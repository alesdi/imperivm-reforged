#pragma once

/// The siege planner: `ObjList::Siege/3` and `Squad::Siege/4`.
///
/// Both entry points are marshalling thunks over one body, `gbr.exe`
/// 0x004383c0 -- the routine behind the string `CSiegePlan::Execute - Command
/// %s not found` -- which takes a member list, a target, a catapult cap and a
/// squad state. `ObjList::Siege` (0x00438f20) hands it the list as it is;
/// `Squad::Siege` (0x00438e60) first writes its fifth argument into the
/// squad's state, stamps the state time, and hands the squad's own member
/// deque. `DATA\AI HELPERS\SIEGE.VS` and `SIEGE GATE.VS` call the first form
/// with a state of 0; `GS_SIEGE.VS` and `GS_CAPTURE.VS` call the second with
/// `SS_Catapult` and `SS_Siege`.
///
/// ## What the planner does, in the order it does it
///
/// 1. **Decides whether catapults are wanted at all.** Only for a target whose
///    settlement exists and is not owned by player index 15: always for a gate
///    and for a building that is not the settlement's central building; for
///    the central building itself, only while the settlement's holder has
///    somebody in it.
/// 2. **Splits the list into crews and attackers.** A unit becomes a crew
///    candidate when catapults are wanted and it is not a hero, cannot hurt
///    the target (`Unit::CanAttack`, below), is an heir of `Military` and does
///    not carry the `freedom` special. Everyone else is an attacker, and the
///    attackers that *can* hurt the target have their class `damage` summed.
/// 3. **Reconsiders, from that sum.** A target that is not the central
///    building keeps its catapults only if five times the sum is below its
///    health. The central building runs a five-round skirmish against the
///    garrison's total health instead: each round the sum splits between
///    building and garrison in proportion to the building's remaining health,
///    and if the garrison is gone within five rounds nothing is built.
/// 4. **Picks the engine.** The crews vote on a race by majority (lowest race
///    id on a tie, the map is walked in key order) and the race picks the
///    catapult class through the same eight-way table `PlaceCatapult` uses.
///    Britain and Germany are capped at one catapult whatever was asked.
/// 5. **Fills the catapults already aimed at the target**, walking the
///    settlement roll: every catapult of the caller's player whose target is
///    this target gets the nearest unassigned crews up to its holder's cap,
///    and its position becomes the plan's rally point.
/// 6. **Places new ones** while crews remain and the cap allows: the site is
///    the nearest free siege site to the *first unit's* position (the site
///    rule is below), the engine is placed exactly as `PlaceCatapult` places
///    it, aimed at the target, and crewed to its holder's cap.
/// 7. **Issues the orders.** Each crew leaves its hero, drops its pending
///    commands, queues `build_catapult` at its engine and ends what it was
///    doing; the crews are then regrouped into fresh squads carrying the
///    siege state (0x00447330, `regroup_into_fresh_squads`). Crews that found
///    no engine rejoin the attackers.
/// 8. **Sends the attackers.** With fewer than ten units crewing (the new
///    crews plus whoever was already inside), and either no catapults wanted
///    or a majority of melee units among the attackers, everyone attacks --
///    `ai_attack_gate` for a gate, `attack` otherwise. Otherwise everyone
///    stands back: a `move` to a point 200 then 400 units beyond the rally
///    point along the ray from the target, clamped to the map, followed by
///    `idle`. Both loops stop at the first hero they order.
///
/// ## The siege sites
///
/// 0x00448470 builds, per settlement, the candidate sites for a catapult of
/// the chosen class, from two shapes: **eight points on a circle** of radius
/// `range - 128` around the central building, for an `Outpost` only; and, for
/// every gate, **nine points along the gate's wall axis** pushed outward by
/// the same radius. The axis is the pair of type-7 markers on the gate's
/// entity, shifted so it passes through the mean of the type-10 markers
/// (0x00529380); "outward" is away from the central building. A site counts
/// when it is inside the map and the engine's footprint fits there, and the
/// query (0x00448660) takes the nearest to the asking point among the sites
/// attached to the target, skipping a target already broken.
///
/// ## What is approximated, and what is left out
///
///   * **The footprint test is `passable_3x3`.** The original lays the class's
///     `.pass` mask over the obstruction grid and refuses a site where any
///     blocked cell of the mask lands on a blocked cell of the map, then
///     queries the objects standing in the rectangle around it. This engine's
///     simulation does not load the masks -- the terrain layer arrives
///     pre-stamped -- so the site's cell and its eight neighbours stand in for
///     the mask, and no object query is made. A world with no obstruction grid
///     refuses nothing.
///   * **The circle uses integer arithmetic**, `round(r * 0.70711)` for the
///     diagonals where the original multiplies by a double cosine and rounds to
///     nearest. Equal on every shipped catapult range.
///   * **Ties in the crew ordering keep list order.** The original sorts its
///     plan with an unstable introsort keyed on (catapult handle, distance),
///     so two crews at the same distance from an engine can swap; here the
///     sort is stable and the swap cannot happen.
///   * **`Unit::CanAttack` is read from what the object model carries.** The
///     visibility mask, the two unit flag bits at 21 and 22 of `[unit+0x194]`,
///     the class flag at `[class+0xb30]`, and the settlement field at
///     `[settlement+0x40]` are not modelled and are treated as passing.
///   * **The catapult's target is `Catapult::SetTarget`'s.** 0x004389d5
///     writes the target handle into `[cat+0x228]`, the slot
///     `Catapult::GetCurrentTarget` reads back, which this engine keeps as the
///     combatant's target; an engine the combat system has not yet met is
///     reconciled first. `CombatSystem::act` acquires and strikes for every
///     combatant whose class can attack **except an engine not yet built**
///     (playtest #19): an engine at one point of health used to shoot here
///     before its crew had assembled it. The planner's aim changes which
///     target a built engine shoots at, not whether.
///   * **A site query with no catapult class is answered with no site.** The
///     original reaches it with a null class when no crew voted -- the race
///     comes from the first unit's pointer then -- and faults inside the
///     footprint test; no shipped call is known to get there.

#include <cstdint>
#include <span>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

/// What one run of the planner did, for the callers that measure it.
struct SiegeReport {
  /// Catapults newly placed by this run.
  std::int32_t placed = 0;
  /// Units ordered to `build_catapult` by this run (0x00436490's answer).
  std::int32_t crews = 0;
  /// Units already inside catapults aimed at the target when the run started.
  std::int32_t crews_inside = 0;
  /// Whether the run wanted catapults at all after step 3.
  bool wanted_catapults = false;
  /// Whether the attackers were sent to attack (true) or to stand back.
  bool attacked = false;
};

/// 0x004383c0: the planner, on an already-resolved member list.
///
/// `units` is copied before anything is ordered, so a squad's own member
/// vector may be passed. `now` stamps the fresh squads' state time.
SiegeReport run_siege_plan(World& world, std::span<const ObjectId> units, ObjectId target,
                           std::int32_t max_catapults, std::int32_t siege_state, GameTime now);

/// Define `ObjList::Siege/3` and `Squad::Siege/4`. Returns how many were newly
/// implemented.
std::size_t register_siege_host(script::HostRegistry& registry);

}  // namespace imperivm::core::sim
