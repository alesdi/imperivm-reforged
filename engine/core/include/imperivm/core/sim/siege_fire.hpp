#pragma once

/// Siege fire: the catapult record and the fake towers, as `gbr.exe` keeps
/// them. `docs/formats/siege-fire.md` is the model, with the addresses; this
/// header is the part of it the host entry points share.
///
/// **Nothing here fires on a clock.** A catapult shoots when a script calls
/// `Catapult::Attack(point)` and a tower when one calls `Building::Attack`;
/// `CombatSystem::act` leaves every building alone (see there). What lives
/// here is the state those calls read and write -- the catapult's own target
/// slot and the mirror of it onto its towers, the towers themselves, and the
/// two target sweeps -- so that the host functions in `sim/combat.cpp` and
/// `sim/world_host.cpp` stay thin.

#include <cstdint>

#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;
class CombatSystem;
struct Settlement;

/// Whether `id` is a siege engine: a `CVXCatapult`, what `AsCatapult` casts to.
[[nodiscard]] bool is_catapult(const World& world, ObjectId id) noexcept;

/// The settlement a building belongs to (`[bld+0x148]`): through its
/// back-link, else by membership of the roll, as a catapult is filed. Null
/// when it has none.
[[nodiscard]] Settlement* siege_settlement_of(World& world, ObjectId building);

/// `Catapult::SetTarget` (0x004e3600): store `target`'s handle in the slot --
/// none for a target the world does not hold -- unvalidated, then copy the
/// slot into every `CatapultTower` of the engine's settlement while
/// `has_towers` is set.
void catapult_set_target(World& world, ObjectId catapult, ObjectId target);

/// `Catapult::ClearTarget` (0x004e3710): the slot to none, and the towers'
/// with it under the same gate.
void catapult_clear_target(World& world, ObjectId catapult);

/// `Catapult::ClearTowerTarget` (0x004e37f0): the towers' targets to none
/// under the gate, the engine's own slot left alone.
void catapult_clear_tower_target(World& world, ObjectId catapult);

/// The duration of the entity's attack animation (`<anim idx="5">`'s
/// `duration`, the record 0x147c whose `+0x4c` `AttackWait` reads), or 0
/// for an object with no entity or no such animation.
[[nodiscard]] std::int32_t catapult_throw_ms(const World& world, ObjectId catapult) noexcept;

/// The same animation's `action_time`: when, into the throw, the shot leaves.
[[nodiscard]] std::int32_t catapult_release_ms(const World& world, ObjectId catapult) noexcept;

/// `Catapult::InRange(point)` (0x004e3230 -> 0x005a7940): the distance from
/// the engine's centre to `point`, less its own radius, inside
/// `[min_range, range]` -- no escape for a `min_range` under 1.
[[nodiscard]] bool catapult_point_in_range(const World& world, const CombatSystem& combat,
                                           ObjectId catapult, Point point) noexcept;

/// `Catapult::BestTarget` (0x004e2f60): see `docs/formats/siege-fire.md`.
[[nodiscard]] ObjectId catapult_best_target(const World& world, const CombatSystem& combat,
                                            ObjectId catapult);

/// `Building::BestTarget` (0x004dcc60): see `docs/formats/siege-fire.md`.
/// Reads (and so decays) the `danger` of the units it weighs, and raises the
/// answer's by one (`docs/formats/target-score.md`).
[[nodiscard]] ObjectId building_best_target(const World& world, CombatSystem& combat,
                                            ObjectId building);

/// The fire rate of a settlement's towers (0x005c2fb0): `isqrt(military +
/// min(others, 10)) * base_rate / 100` shots a second, over the units in
/// its holder. `base_rate` is `CONST.INI`'s `BaseFireRate`.
[[nodiscard]] std::int32_t settlement_fire_rate(const World& world, const Settlement& settlement,
                                                std::int32_t base_rate);

/// Place the fake towers `object` is owed, if it is owed any: a `FakeTower`
/// at every type-7 point of a `CVXOutpost`'s entity (0x005641d0), a
/// `CatapultTower` at every type-7 point of a `CVXCatapult`'s (0x004e38d0,
/// which also raises `has_towers`). Raises `towers_posted` either way.
/// Returns how many it placed.
std::size_t place_fake_towers(World& world, ObjectId object);

/// `place_fake_towers` for every object still owed its post, in id order.
/// The original posts message 0x3d from the construction virtual; this is
/// where the post is delivered. Returns how many towers it placed.
std::size_t post_fake_towers(World& world);

}  // namespace imperivm::core::sim
