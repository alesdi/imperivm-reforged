#pragma once

// Ordering a fight in a test without a script.
//
// In `gbr.exe` no unit strikes on its own: a script calls `Attack` and the
// swing runs on the animation timer (`docs/formats/animation.md`, "The
// attack"). `UNIT_ENGAGE.VS` and its kin hold the fight with
// `while (.Attack(u));`. These helpers are that loop for a test that has a
// world and a combat system but no scheduler: a fresh swing each time the
// last one is over, for as long as `Attack`'s gate (0x005a78a0) lets it
// through.

#include <cstdint>

#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::test {

/// One `Attack(target)` whose answer nobody waits for: refused, as the
/// original refuses it, when the attacker is mid-swing (its mode is not 0 --
/// the call would not be made by a script still waiting on the last one),
/// cannot attack the target, or has it out of range.
inline bool swing(core::sim::World& world, core::sim::CombatSystem& combat, core::ObjectId attacker,
                  core::ObjectId target) {
  const core::sim::WorldObject* slot = world.find(attacker);
  if (slot == nullptr || slot->clock.mode != 0) return false;
  // The target stored before the gate (`[unit+0x1a8]`), as `Attack` does.
  (void)combat.hand_target(attacker, target);
  if (!combat.can_attack(attacker, target) || !combat.in_range(attacker, target)) return false;
  combat.begin_attack(world, nullptr, attacker, target, 0, world.time());
  return true;
}

/// `while (.Attack(target));` for up to `turns` turns: a swing offered at the
/// head of every turn. Returns the swings started.
inline int fight(core::sim::World& world, core::sim::CombatSystem& combat, core::ObjectId attacker,
                 core::ObjectId target, int turns) {
  int started = 0;
  for (int turn = 0; turn < turns; ++turn) {
    if (swing(world, combat, attacker, target)) ++started;
    world.advance();
  }
  return started;
}

}  // namespace imperivm::test
