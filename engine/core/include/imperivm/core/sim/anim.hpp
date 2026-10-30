#pragma once

/// The animation entry points: what a script says to make an object move.
///
/// Ten names over 110 shipped call sites, every one of them in `data.pak` and
/// none in a campaign container -- this is object behaviour, not mission
/// scripting. `StartAnim/2` (35 sites), `TimeToAnimFinish/0` (29),
/// `TimeToActionMoment/0` (26), `PlayAnim/2` (13) and `PlayAnim/3` (13),
/// `SetState/1` (16, and see below), `GetAnimDuration/1` (2),
/// `GetAnimTime/1` (1), `StartDelayedAnim/3` (1). `GetAnim/0` is registered and
/// has **zero** shipped sites; it is bound anyway because `TimeToAnimFinish`'s
/// own guard reads it -- `if (.GetAnim == 17) Sleep(.TimeToAnimFinish);` is in
/// the corpus three times over as a *comment*, and the entry point is a
/// one-field read either way.
///
/// ## This is the caller `World::play_anim` never had
///
/// `World::play_anim` and `World::enter_state` were written, hashed, saved and
/// rendered: the turn loop advances every object's timeline, the renderer reads
/// the cursor, and `world.hpp` says in as many words that they are *"the shape
/// the script VM's `PlayAnim` host function will bind to"*. Nothing bound it.
/// So 28 shipped classes -- the blacksmith fires, the wells, the obelisks, the
/// barrack horses, every flame -- ran `DATA\SUBAI\ANIM.VS`, which is
///
///     while (1)
///       This.PlayAnim(1, This.pos);
///
/// against an entry point with no body, and that one file was **the largest
/// trap in the corpus**. The eighth instance of this project's signature
/// failure, and the first found by asking what a *hashed field* was for.
///
/// ## Only `PlayAnim` suspends, and that is what `ANIM.VS` needs
///
/// `ANIM.VS` has no `Sleep` in it. A `PlayAnim` that returned would spin the
/// instruction budget every tick forever -- the shape `Unit::Attack` was caught
/// in. `gbr.exe` settles it: `Obj::PlayAnim` (0x005a9ba0) and `Flying::PlayAnim`
/// (0x0051bb10) are the only two members of this family registered through the
/// **suspending** registrar 0x00699eb0; the other eight go through 0x00699bb0.
///
/// The two suspend differently, and only one of the two mechanisms survives the
/// crossing to this engine. `Obj::PlayAnim` starts nothing: it writes 6 into the
/// object's script-action mode, arms the object's animation timer and returns
/// the "suspend until this object's order completes" code, and a continuation
/// (0x005a76c0) starts the animation on the next action tick and wakes the
/// script when it ends. `Flying::PlayAnim` starts the animation itself and
/// suspends for exactly the animation's duration. This engine has no per-object
/// order queue for a script to wait on, so **both are implemented as
/// `Flying::PlayAnim`'s shape**: start now, suspend for what is left. The
/// difference is a tick of latency at the front -- and, when the object is
/// already playing an animation, the whole of what is left of it: the
/// original's armed animation timer keeps 0x005a9ba0 from arming its own, so
/// the continuation runs only when the current one ends (0x0053f6a0 calls the
/// action handler 0x005b0f70, whose mode 6 is 0x005a76c0). Here the new one
/// interrupts it. Every shipped `Obj::PlayAnim` loop calls it again only after
/// its last one ended; the druids' summoning lines call it once on a druid that
/// has just been ordered to cast.
///
/// ## The index is the entity's `<anim idx>`, and two names are off by one
///
/// The original keeps its animations in a dense array and `Obj::StartAnim`
/// decrements before it indexes, so array element *i* is the entity document's
/// `idx = i + 1`. This engine keys `Entity::anim` on the document's `idx`
/// directly. Therefore:
///
///   * `StartAnim`, `PlayAnim/2`, `StartDelayedAnim`, `GetAnim`, `GetAnimTime`
///     and `SetState` take the **document's number**, unchanged;
///   * `Flying::PlayAnim/3` and `Unit::GetAnimDuration/1` pass their argument
///     **raw** into the same array, so their number is **one less**.
///
/// The corpus proves it twice, and both times against the same fifteen slots
/// the shipped entities actually declare (1, 2, 3, 5, 9, 13, 14, 17..24).
/// `CROW_IDLE.VS` and `CROW_MOVE.VS` call `PlayAnim(0, ...)` and
/// `PlayAnim(16, ...)`, and **no entity declares 0 or 16** -- add one and they
/// are 1 and 17, which every crow declares. `EAGLE_MOVE.VS` computes
/// `.speed * .GetAnimDuration(4) / .GetAnimDuration(0)`, and 4 and 0 become 5
/// and 1: the attack animation over the walk. Under the direct reading every
/// crow and eagle animation in the game silently does nothing, which is exactly
/// what `world.hpp` recorded as *"`PlayAnim(0, …)` occurs in shipped script
/// code and no entity declares slot 0"* -- a miss that was really an
/// off-by-one.
///
/// ## `SetState` is two entry points wearing one name
///
/// Twelve of the sixteen `SetState/1` sites are `sq.SetState(SS_IDLE)` on a
/// **squad**, and four are `.SetState(2)` on a grave, stepping its decay
/// through the entity's states. `gbr.exe` registers them as two functions on
/// two receivers; this registry keys on (kind, name, arity), so one body
/// dispatches on the handle type. A body that only did the object half would
/// answer three quarters of the corpus by refusing.
///
/// ## What is left out, and named
///
/// **The state model is thinner here than in the original, in four places.**
/// `gbr.exe` keeps, per object: a combined action/state word (`Anim`, where
/// values below `0x1c` are "playing animation n" and above are "resting in
/// state n - 0x1c"), a facing vector, the playing index, a *transition*
/// animation index, an absolute start time, an animation origin point, and a
/// per-playback duration override. This engine keeps a state index and a slot
/// as two fields, an elapsed time rather than a start time, and neither a
/// transition, nor an origin, nor an override.
///
///   * **No transition animations.** `StartAnimation` looks for an animation
///     leading from the current state to the new one and plays it first;
///     nothing here does, so an object snaps.
///   * **No animation origin.** The original's `AnimX`/`AnimY` let an animation
///     play somewhere other than where the object stands. Every shipped site
///     passes the object's own position or a facing target, so the field is
///     never used for its own purpose in the corpus.
///   * **No duration override**, `StartAnimation`'s (0x0053e6c0) last
///     parameter; every entry point here passes 0, so the declared `duration`
///     is the length.
///   * **Elapsed, not absolute.** A reimplementation aiming at the original's
///     *own* hash would have to store the start time; this engine hashes its
///     own cursor and the two are equivalent under it.
///   * **`Flying::PlayAnim`'s Z** is no longer discarded: `sim/flying.hpp`
///     carries the flight state it writes, and `sim/anim.cpp` the order.
///
/// **A retail stack bug is deliberately not reproduced.** `Obj::SetState` is
/// registered `void` and its error path pushes an int anyway, leaving four
/// bytes on the VM stack that nothing pops. This engine's calling convention
/// has no way to express that and no reason to.

#include <cstddef>
#include <cstdint>

#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/script/host.hpp"

namespace imperivm::core::sim {

/// The sheet row a construction frame draws: `WorldObject::build_frame` on the
/// animation `anim`, whose playable schedule is `timeline`.
///
/// `Catapult::SetBuildFrame` counts in the entity's declared `frames`
/// (`[anim+0x48]`, which the entity loader fills from the attribute, capped at
/// 64: 0x00600fbf), and those include the zero-length entry marker that
/// `anim_frame_holds` drops. So a frame past that marker is the row one below
/// it. `CATAPULT_IDLE.VS` never hands it the last frame -- it calls
/// `SetBuilt` instead once nothing is left to build -- so the frames it does
/// hand it, 1 to `frames - 2`, are exactly the sheet's rows.
///
/// **Inferred:** that the sprite reads a frame index the way this engine's
/// timeline reads a step, `remaping` order included. The call at 0x00629500
/// writes the index to the sprite and stops its clock; how the sprite turns an
/// index into a sheet row was not read. Every shipped build sheet runs
/// forward, where the two readings agree. An animation with no entry marker
/// counts from step 0. Out of range is clamped to the cycle, and an invalid
/// timeline answers row 0.
[[nodiscard]] std::uint32_t build_frame_row(const EntityAnim& anim, const AnimTimeline& timeline,
                                            std::int32_t frame) noexcept;

/// Define this domain's slice of the host API and return how many entry points
/// were defined.
std::size_t register_anim_host(script::HostRegistry& registry);

/// The table's size, so a caller can assert against it.
[[nodiscard]] std::size_t anim_host_entry_count() noexcept;

class World;
struct WorldObject;

/// What is left, at the running script's millisecond, of a one-shot animation
/// the object is playing -- one started to run once and hand control back
/// (`AnimRepeat::hold`), as an attack, a death or a fidget is -- or 0 for
/// none. The original's animation timer (id 0x1b, armed by `StartAnimation`,
/// 0x0053e6c0) is pending for exactly that long, and the idle activity's
/// setter (0x005a7680) runs the activity at once only when it is not
/// (0x0053cfc0); see `docs/formats/idle.md`.
[[nodiscard]] std::int32_t one_shot_time_left(const script::CallContext& ctx, const World& world,
                                              const WorldObject& slot);

/// Start animation `anim_slot` on `id`, held, **at the running script's
/// millisecond**: the cursor is brought forward by the part of the turn the
/// world has already lived through, as the animation entry points do. False
/// when the entity declares no such animation.
bool start_anim_at_script_time(const script::CallContext& ctx, World& world, core::ObjectId id,
                               std::int32_t anim_slot);

}  // namespace imperivm::core::sim
