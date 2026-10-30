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
/// The two suspend differently. `Obj::PlayAnim` starts nothing: it writes 6
/// into the object's action mode, arms the object's animation timer for 1 ms
/// when it is not pending and sleeps with no timed wake. The timer's firing
/// runs the object's end handler (0x0053f6a0 -> `vtbl+0x54`), whose mode 6 is
/// the continuation 0x005a76c0: it starts the animation the call named and,
/// at the next firing -- that animation's end -- wakes the script and sets the
/// mode back to 0. So an object already playing an animation runs it out
/// first, and a mode something else rewrote in the meantime leaves the script
/// asleep. Here the call's re-entries are the firings (`m_play_anim`).
/// `Flying::PlayAnim` starts the animation itself and suspends for exactly
/// its declared duration.
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
/// ## The animation clock
///
/// Every object carries the original's animation record (`AnimClock` on
/// `WorldObject`; `docs/formats/animation.md` is the description): the
/// playing slot, the transition played in front of it, the start, the main
/// start after the transition, the action moment and the end, the animation
/// timer, and the action mode `[obj+0x130]`. `StartAnimation` (0x0053e6c0,
/// `World::start_animation`) stops what plays into its end state, looks for
/// the transition from the state the object rests in, takes a start offset
/// (`StartDelayedAnim`, the idle's fidgets) and the `-1` that applies the
/// animation with no timer. The scripts' readers read the record at the
/// script's own millisecond. The cursor the renderer draws follows it.
///
/// ## What is left out, and named
///
///   * **No animation origin.** The original's `AnimX`/`AnimY` let an animation
///     play somewhere other than where the object stands; only the walk step
///     and `Flying::PlayAnim` pass one. `Flying::PlayAnim` puts the bird at its
///     goal at the call instead of at the main start.
///   * **No duration override** from a script: every entry point passes 0, so
///     the declared `duration` is the length. The walk step, the one caller
///     that passes one, is the movement system's loop here.
///   * **The walk is not animation steps.** The original walks by one-shot
///     steps on the animation timer (action mode 3, 0x005d3b50), so its timer
///     is pending all the way and `IsInState` is false while a unit walks.
///     Here the walk is the movement system's loop, which stops what the
///     clock plays when it starts.
///   * **A corpse is not put down by its own timer.** The end handler's death
///     sequence (mode 1, then 8 and the destroy) runs here when a script's
///     call reaches it (`anim_dispatch`); combat's death time removes the
///     bodies it kills.
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
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/sim/tick.hpp"

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

/// The running script's millisecond: the scheduler's `now`, or the world's
/// time outside a pass. Every animation-clock read and write a script makes
/// is at this time.
[[nodiscard]] GameTime script_now(const script::CallContext& ctx, const World& world);

/// `StartAnimation` (0x0053e6c0) as a script's call makes it: at the script's
/// millisecond, turning towards `face` first unless it is the invalid point
/// or the object is off the map. Answers what is left of the animation,
/// transition included, or 0 when nothing started (`World::start_animation`).
std::int32_t start_animation_facing(const script::CallContext& ctx, World& world,
                                    core::ObjectId id, std::int32_t anim_slot, Point face,
                                    GameTime delay);

/// The object's animation-end handler (`vtbl+0x54`: 0x005d5b60 for a unit,
/// 0x005b0f70 for every object) up to the point where its action mode's own
/// step would run, at `now`. A held unit runs nothing; a dead object runs the
/// death sequence -- mode 1 starts a death animation variant and ends the
/// running command, then mode 8 destroys it (`vtbl+0x94`) -- and runs no
/// step. True when the mode's own step (a walk step, an idle's, a
/// `PlayAnim`'s continuation) is what runs next. See
/// `docs/formats/animation.md`.
bool anim_dispatch(script::CallContext& ctx, World& world, core::ObjectId id, GameTime now);

}  // namespace imperivm::core::sim
