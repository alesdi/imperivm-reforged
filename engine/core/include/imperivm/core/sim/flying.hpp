#pragma once

/// Flight: the four entry points that decide where a bird goes, the altitude
/// they steer by, and the terrain layer underneath all of it.
///
/// One line of `CROW_IDLE.VS` -- `.PlayAnim(16, .PickFlyingPoint(120), 80 +
/// GetTerrainHeight(.pos))` -- was the largest single entry in this project's
/// trap tally for as long as the tally has existed, and everything in this file
/// is downstream of it. Eleven entry points:
///
///   | entry point                     | sites | body       |
///   |---------------------------------|------:|------------|
///   | `GetTerrainHeight/1`            |     9 | 0x0051b4b0 |
///   | `Flying::z/0`                   |     5 | 0x0051bdf0 |
///   | `RandomOffset/1`                |     2 | 0x004c7c50 |
///   | `Flying::PickLandingPoint/1`    |     2 | 0x0051c6a0 |
///   | `Unit::dir/0`                   |     2 | 0x0051c1d0 |
///   | `Flying::PickFlyingPoint/1`     |     1 | 0x0051c3d0 |
///   | `Flying::AdjustFlyDir/1`        |     1 | 0x0051be50 |
///   | `Query::GetAverageDirection/0`  |     1 | 0x005799d0 |
///   | `Flying::IsLanding/0`           |     0 | 0x0051bd80 |
///
/// (`MIN/2`, `MAX/2` and `Unit::ForceIdle/0` came out of the same reading and
/// live where they belong -- the first two in `sim/world_host.cpp`, the third
/// beside `Idle` in `sim/command.cpp`.)
///
/// ## The altitude the whole family is written in is the height layer's byte
///
/// `Flying::PlayAnim` range-checks its `z` argument to `-1 .. 1024` and every
/// shipped site builds one out of `GetTerrainHeight`, which answers 0..255 --
/// so the scale question `docs/formats/map.md` leaves open ("the step size from
/// those 256 levels to world units is a projection question") never has to be
/// answered here. Every number in this file is in the layer's own units, from
/// the sample to the interpolation to the crow's `targetz = 260 + rand(100)`.
///
/// ## A bird has no stored altitude
///
/// `Flying::z` (0x0051b040) computes one on demand, from three states:
///
///   * **on the ground** (`in_air` clear) -- the terrain height under it;
///   * **mid-animation** -- `z_from + (z_to - z_from) * elapsed / cycle`;
///   * **otherwise** -- `z_from`, the altitude the last animation ended at.
///
/// `ObjectState::z_from` and `z_to` are written by `Flying::PlayAnim` and by
/// nothing else, `z_from` from `Flying::z` itself so that each animation starts
/// where the last one left off.
///
/// ## `ObjectFlags::in_air` finally has a writer, and it is not a state machine
///
/// This flag has been declared, hashed, saved and **false in every session this
/// engine has ever run**: `Flying::IsInAir` reads it at seven shipped sites,
/// `CROW_IDLE.VS`'s `if (!.IsInAir())` was therefore always true, and no crow in
/// the game had ever left the ground. It was the ninth instance of this
/// project's signature failure -- a mechanism complete except for the one line
/// that would let anything reach it.
///
/// The writer is one expression at 0x0051bc4e, inside `Flying::PlayAnim`:
///
///     in_air = !(z_from == GetTerrainHeight(pos) && z == -1)
///
/// which reads as *a bird is in the air unless it is descending and already at
/// ground level*. `PlayAnim(16, ..., 80 + h)` takes off because `z != -1`;
/// `PlayAnim(17, ptLand, -1)` keeps it up because it is still high; and
/// `PlayAnim(18, .pos, -1)` -- the crow's transition-to-idle, run after the
/// landing animation has already put `z_from` on the ground -- is the one that
/// clears it. There is no separate take-off or touch-down event to miss.
///
/// `Flying::IsLanding` is `in_air && landing`, where `landing` is
/// `[obj+0x1d4]`, written by the same call as `z == -1`.
///
/// ## What is still unknown
///
///   * **The retail query is interned and this one is not.** `0x004fee10` walks
///     a pool keyed on (object, class, radius, flag) and hands back the same
///     `CVXMapAreaQuery` every time, so a retail crow mints exactly one query
///     object in its whole life and that object occupies a handle in the
///     hashed slot table. This engine has no interning pool, and minting a
///     query per call would grow the table without bound; the flock is
///     evaluated directly instead. Handle numbering already differs from
///     retail's for every internal query the original makes and this engine
///     does not, so what is lost is a correspondence this project never had --
///     but it is a real difference and it is written down rather than implied.
///   * **`PickFlyingPoint` can return whatever was on the stack.** 0x0051c559
///     seeds its best-so-far *x* from an uninitialised slot and only writes it
///     when a candidate scores under 10,000; about 197 airborne creatures
///     within 100 units of every one of the twelve candidates would reach it.
///     Unreachable on shipped content, unreproducible by construction, and
///     answered here with the zero vector -- which `SetLen` then turns into a
///     heading of due north, exactly as the original's `SetLen` would.
///   * **When a flying animation moves the object.** The original starts the
///     animation with the destination as its own end point and commits that
///     into `[obj+0x24]` -- which is what `Obj::pos` reads -- only at the *next*
///     `StopAnimation`, so a retail crow's script-visible position runs one
///     animation behind. `sim/anim.cpp` moves it at the end of the call
///     instead, and says there why: the `in_air` test, the facing and the
///     terrain height all still come out at the previous call's destination,
///     and only what `.pos` answers between two calls differs.
///   * **What `Unit::ForceIdle`'s flag does to an idle animation.** See
///     `sim/command.cpp`: retail's `[obj+0x1b8]` cuts the idle loop's wait
///     short and starts a fresh idle animation, and this engine's `Idle` does
///     not animate at all, so there is nothing here for the flag to cut short.

#include <cstddef>
#include <cstdint>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

/// Bilinear sample of a height layer at a world point, in the layer's units.
///
/// 0x0053dc00, and it is exact integer arithmetic in the original too: two
/// horizontal lerps and one vertical, each `(b - a) * f / cell` with a
/// truncating divide (`cdq; and edx, 0x1f; add; sar 5`, which is division by 32
/// rounded toward zero). The cell to the right and the row below are clamped at
/// the last one, so the far edge is flat rather than wrapped.
///
/// A layer with no cell size answers 0, which is sea level.
[[nodiscard]] std::int32_t sample_height(const Grid& layer, Point at) noexcept;

/// `GetTerrainHeight(pt)` -- the point clamped into the map rectangle first.
[[nodiscard]] std::int32_t terrain_height(const World& world, Point at) noexcept;

/// `Flying::z` -- the interpolated altitude of one object.
[[nodiscard]] std::int32_t flying_z(const World& world, const WorldObject& slot) noexcept;

/// How far above the ground a bird's body is drawn, in screen pixels -- which
/// are the height layer's own units, one pixel a step (`core::world_to_screen_y`).
///
/// `0x0051b240`, the flying unit's visual update: when the airborne bit is set
/// it takes `GetTerrainHeight(pos) - Flying::z`, clamps it to no more than 0,
/// and hands `(0, that)` to the per-layer offset setter (0x0062a4f0) for every
/// layer whose depth is 1000 or 1050 -- see `flying_lifts_layer`. On the ground
/// the offset is `(0, 0)`. So the body rises by the altitude above the terrain
/// under the bird and the shadow, at depth 800, stays on the ground below it.
///
/// Zero for anything that is not a flying unit or not in the air. Nothing in
/// the simulation reads it: it is what `z_from`, `z_to` and the animation clock
/// -- all hashed and saved already -- look like.
[[nodiscard]] std::int32_t flying_lift(const World& world, const WorldObject& slot) noexcept;

/// Whether `flying_lift` moves a layer drawn at depth `z`: the two constants
/// 0x0051b2b3 and 0x0051b2ba compare against. The crow's and the eagle's
/// bodies are 1000 and their shadows 800.
[[nodiscard]] constexpr bool flying_lifts_layer(std::int32_t z) noexcept {
  return z == 1000 || z == 1050;
}

/// What `Flying::PlayAnim` does to the flight state, before it starts anything.
///
/// Writes `z_from`, `z_to`, `landing` and `in_air`, in that order and by the
/// rules at the head of this file. `at` is the animation's destination, already
/// clamped into the map; `z` is the call's own argument, `-1` meaning *land*.
/// Called by `sim/anim.cpp`, which owns the animation half.
void begin_flight_anim(World& world, ObjectId id, Point at, std::int32_t z) noexcept;

/// One of `PickFlyingPoint`'s twelve candidate offsets: `index` is 0..11 and
/// stands for `30 * index` degrees.
///
/// The scan is the one place the executable's **third** approximation of pi
/// appears: `0x7beb58` is 6.28/360, and `trunc(R * cos(a * that))` is what the
/// candidate's x is. See `sim/heading.hpp` for the other two and for why they
/// are not interchangeable.
[[nodiscard]] Point scan_offset(std::int32_t radius, std::int32_t index) noexcept;

/// `0x0051c210` -- may `slot` put itself down at `at`?
///
/// Three tests: inside the map rectangle (twice, once biased by half a terrain
/// cell on both axes), the terrain-type byte at the biased point being water
/// **if and only if** the class is a water unit, and the obstruction cell under
/// the raw point being free.
///
/// Exported rather than left inside `PickLandingPoint` because it is the only
/// way to test it: the picker reaches it through five jittered candidates and a
/// give-up roll, so a test that went the long way round would be asserting
/// about the generator rather than about the landing rule.
[[nodiscard]] bool can_land(World& world, const WorldObject& slot, Point at) noexcept;

/// `0x0051b600` -- is `at` a sensible distance and direction from a bird at
/// `from` heading `heading`?
///
/// Between half and twice `reference` away, and within about 55 degrees of the
/// heading. Both comparisons are in percent with a truncating divide, and the
/// denominator is forced to 1 when it would be zero rather than guarded with a
/// branch (0x0051b674) -- which matters because a bird with no heading would
/// otherwise divide by nothing. Exported for the same reason `can_land` is.
[[nodiscard]] bool heading_ok(Point from, Point heading, Point at,
                              std::int32_t reference) noexcept;

/// `Flying::AdjustFlyDir(p)` -- the new value of `p`, given the bird's heading
/// and position and the map's inclusive high corner.
///
/// Within 800 units of an edge, rotate the whole problem by ninety degrees
/// until the near edge is the one at low *y*, and then: if the requested
/// heading already points away, leave it; otherwise turn the *bird's* heading
/// fifteen degrees -- toward whichever side its own *x* says -- and take the
/// turn if it improves. Away from every edge, `p` is returned untouched.
[[nodiscard]] Point adjust_fly_dir(Point heading, Point p, Point pos,
                                   std::int32_t map_high) noexcept;

/// Define the entry points this domain owns. Returns the number defined.
std::size_t register_flying_host(script::HostRegistry& registry);

/// The number of `define` calls `register_flying_host` makes.
[[nodiscard]] std::size_t flying_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
