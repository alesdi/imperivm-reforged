// The animation entry points. See include/imperivm/core/sim/anim.hpp.

#include "imperivm/core/sim/anim.hpp"

#include <vector>

#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/flying.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::Value;

/// The receiver as a live object, or null.
[[nodiscard]] const WorldObject* receiver_object(CallContext& ctx, World*& world) noexcept {
  world = world_of(ctx);
  if (world == nullptr || ctx.count() == 0) return nullptr;
  const script::Value& value = ctx.arg(0);
  if (!value.is_object()) return nullptr;
  const script::ObjectRef ref = value.as_object();
  if (ref.type != kTypeObj) return nullptr;
  return world->find(ref.id);
}

[[nodiscard]] std::int32_t int_arg(CallContext& ctx, std::size_t index) noexcept {
  if (ctx.count() <= index) return 0;
  const Value& value = ctx.arg(index);
  return value.is_integer() ? value.as_integer() : 0;
}

[[nodiscard]] HostOutcome integer(std::int32_t value) {
  return HostOutcome::ok_with(Value::integer(value));
}

/// The animation an object is playing, or null.
[[nodiscard]] const EntityAnim* playing(const WorldObject& slot) noexcept {
  if (slot.object == nullptr || slot.object->entity == nullptr) return nullptr;
  if (slot.object->anim.anim_slot == kNoAnim) return nullptr;
  return slot.object->entity->anim(slot.object->anim.anim_slot);
}

/// How long the animation at `anim_slot` lasts as far as a script can tell:
/// the entity document's `duration` attribute, or 0.
///
/// **The declared `duration`, not the frame strip's sum.** The loader
/// (0x006004d0) stores the `<anim duration>` attribute at `[anim+0x4c]`, and
/// that one field is every script-visible length in this family:
/// `StartAnimation` (0x0053e6c0) takes it as the playback length `[obj+0x64]`
/// unless a caller overrides it, the end time `TimeToAnimFinish` counts down to
/// is the start plus it (0x0053cef0, filled by 0x0053cf30), the animation's own
/// timer ends it there (0x0053f6a0), and `GetAnimTime` (0x005a9c50),
/// `Unit::GetAnimDuration` (0x0051c180) and `Flying::PlayAnim`'s suspension
/// (0x0051bcba, written into the VM's wait cell) read it verbatim. The two
/// disagree in 127 of the 1,168 shipped animations -- the barrack horses' idle
/// declares 1000 over a 1200 strip, the hero grave's crumble 6486 over 12100.
/// The strip still drives which sprite row is drawn (`AnimTimeline`); this is
/// the clock the scripts see.
[[nodiscard]] std::int32_t length_of(const WorldObject& slot, std::int32_t anim_slot) {
  if (slot.object == nullptr || slot.object->entity == nullptr) return 0;
  const EntityAnim* anim = slot.object->entity->anim(anim_slot);
  return anim == nullptr ? 0 : anim->duration;
}

/// Turn to face `point`, unless it is the object's own position.
///
/// `0x005a9ce0` replaces the argument with the engine's invalid-point sentinel
/// when it equals the receiver's position, and `StartAnimation` then leaves the
/// facing alone. That is what `StartAnim(9, .pos)` means -- *play the death
/// animation and do not turn* -- and it is the form 30 of the 35 shipped sites
/// use. The other five pass `tgt.pos`, which is a turn.
///
/// **The test is belt and braces and is kept knowingly**: `MovementSystem::face`
/// returns early on a zero delta of its own accord, so removing this changes no
/// behaviour and a fault injected into it survives the whole suite. The
/// original writes the check here, so this does too -- and a call site that
/// leans on somebody else's guarantee without saying so is one refactor away
/// from leaning on nothing.
void face_unless_self(CallContext& ctx, World& world, const WorldObject& slot, std::size_t at) {
  if (ctx.count() <= at || !is_point(ctx.arg(at))) return;
  const Point towards = unpack_point(ctx.arg(at));
  if (towards == world.resolve_position(slot.id)) return;
  if (MovementSystem* movement = movement_system(world); movement != nullptr) {
    movement->face(world, slot.id, towards);
  }
}

/// How far the world's clock stands ahead of the running script's.
///
/// A script runs at its own wake time inside a pass, and the world it acts on
/// has already been advanced to the turn's end (`Scheduler`'s "A script wakes
/// on its own millisecond"). Zero outside a pass, and for a script running at
/// the turn's end.
[[nodiscard]] GameTime script_lag(const CallContext& ctx, const World& world) {
  if (ctx.scheduler == nullptr) return 0;
  const GameTime lag = world.time() - ctx.scheduler->now();
  return lag > 0 ? lag : 0;
}

/// Start `slot` on the receiver, facing the point at argument `at`.
///
/// `AnimRepeat::hold`, always. `entity.hpp` draws the line where the evidence
/// does: an animation named by a state's `anim_idx` is that pose's own loop,
/// and one a script starts runs once and hands control back. Every entry point
/// here is the second kind.
///
/// **It starts on the script's millisecond.** The original starts the
/// animation at the instant the script runs; here the world already stands at
/// the turn's end, so the cursor is brought forward by the part of the turn it
/// has lived through, as combat does for a blow (`start_anim_at`). Otherwise a
/// script that waits out its animation wakes while the cursor is still short
/// of the end -- a bird's next leg began before the last one was drawn to its
/// end, a jump of the rest of the leg.
bool start(CallContext& ctx, World& world, const WorldObject& slot, std::int32_t anim_slot,
           std::size_t at) {
  face_unless_self(ctx, world, slot, at);
  const ObjectId id = slot.id;
  if (!world.play_anim(id, anim_slot, AnimRepeat::hold)) return false;
  const GameTime lag = script_lag(ctx, world);
  WorldObject* object = world.find(id);
  if (lag <= 0 || object == nullptr || object->object == nullptr) return true;
  AnimCursor& cursor = object->object->anim;
  cursor.elapsed_ms = advance_elapsed(object->timeline, cursor.elapsed_ms,
                                      static_cast<std::int32_t>(lag), object->repeat);
  const AnimSample sample = object->timeline.sample(cursor.elapsed_ms, object->repeat);
  cursor.step = sample.step;
  if (sample.finished) object->animating = false;
  // Still the one this call started: the caller suspends for its length,
  // counted from the script's millisecond, whether or not the world's clock
  // has already seen it end.
  return true;
}

// --------------------------------------------------------------------------
// the entry points
// --------------------------------------------------------------------------

/// `StartAnim(int slot, point face)` -- 35 sites. Void, and it does not block.
HostOutcome m_start_anim(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("StartAnim: no world");
  // A receiver that resolves to nothing formats a complaint into the sink that
  // is a bare `ret` in retail and returns. Nothing prints, nothing refuses.
  if (slot == nullptr) return HostOutcome::ok_void();
  (void)start(ctx, *world, *slot, int_arg(ctx, 1), 2);
  return HostOutcome::ok_void();
}

/// `StartDelayedAnim(int slot, point face, int delay)` -- one site, and its
/// delay is `-1`.
///
/// `CATAPULT_IDLE.VS:32` is `.StartDelayedAnim(1, .pos, -1);`, and `-1` is the
/// original's own special case: it takes the branch that applies the animation
/// immediately and locally instead of arming the object's timer. So the one
/// shipped call is `StartAnim` with a longer name.
///
/// **A positive delay is not deferred here**, because there is no per-object
/// animation timer to arm -- the original's is timer id `0x1b` on the object's
/// own pending-message list, and this engine's objects have no such list. No
/// shipped site passes one, so the divergence is unreachable through shipped
/// content; it is written down rather than made to look like a decision.
HostOutcome m_start_delayed_anim(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("StartDelayedAnim: no world");
  if (slot == nullptr) return HostOutcome::ok_void();
  (void)start(ctx, *world, *slot, int_arg(ctx, 1), 2);
  return HostOutcome::ok_void();
}

/// `PlayAnim(int slot, point face)` -- **suspending**. 13 sites.
///
/// Start it, then hold the script for the animation's declared length. See the
/// header for the mechanism the original uses and why this engine cannot use
/// it.
///
/// A slot the entity does not declare plays nothing and suspends for zero,
/// which still yields: the scheduler runs a zero-length suspension again at
/// the next pass's end, not on the instant it began, so it costs a turn
/// rather than spinning.
/// That is what keeps `ANIM.VS` -- `while (1) This.PlayAnim(1, This.pos);` --
/// bounded even on an object whose entity is missing.
HostOutcome m_play_anim(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("PlayAnim: no world");
  if (slot == nullptr) return HostOutcome::ok_void();

  const std::int32_t anim_slot = int_arg(ctx, 1);
  const bool playing_now = start(ctx, *world, *slot, anim_slot, 2);

  HostOutcome out;
  out.status = script::HostStatus::suspend;
  out.suspend_for = playing_now ? length_of(*slot, anim_slot) : 0;
  if (out.suspend_for < 0) out.suspend_for = 0;
  return out;
}

/// `Flying::PlayAnim(int slot, point at, int z)` -- **suspending**. 13 sites,
/// all crows and eagles.
///
/// Three differences from the `Obj` form, and two of them are transcribed:
///
///   * **the slot is one less**, because this body passes its argument raw into
///     the array `Obj::PlayAnim` decrements into. See the header.
///   * **the Z is range-checked**: `-1 .. 1024`, and out of range is a refusal
///     that starts nothing (0x0051bb6f). `-1` means *use the terrain height and
///     land*.
///   * the point is the animation's *origin* as well as the facing target, and
///     is clamped into the map rather than ignored. This engine has no
///     animation origin, so it is only a facing target here.
///
/// **The Z is the whole flight model**, and used to be read and discarded here.
/// `sim/flying.hpp` owns what it means; this call site owns the order. The
/// original writes the flight state *before* it starts the animation
/// (0x0051bbed..0x0051bc6d, then 0x0051bc9a), and that order is load-bearing
/// twice over: `z_from` is `Flying::z` of the animation that is still playing,
/// and `in_air` is compared against the height at the position the object still
/// has. Starting first would read the new animation's clock and the new
/// facing.
HostOutcome m_play_anim_flying(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("PlayAnim: no world");
  if (slot == nullptr) return HostOutcome::ok_void();

  const std::int32_t z = int_arg(ctx, 3);
  // *"Invalid Z value ... must be between -1 and 1024"*, printed into a sink
  // that is a bare `ret`, and then nothing happens.
  if (z < -1 || z > 1024) return HostOutcome::ok_void();

  // The point is the animation's *origin* in the original and its facing target
  // here; either way it is where the descent's terrain height is read from, so
  // an absent or malformed one falls back to the object's own position rather
  // than to the origin of the map.
  const Point destination = ctx.count() > 2 && is_point(ctx.arg(2))
                                ? unpack_point(ctx.arg(2))
                                : world->resolve_position(slot->id);
  const ObjectId id = slot->id;
  begin_flight_anim(*world, id, destination, z);
  slot = world->find(id);
  if (slot == nullptr) return HostOutcome::ok_void();

  const std::int32_t anim_slot = int_arg(ctx, 1) + 1;
  const bool playing_now = start(ctx, *world, *slot, anim_slot, 2);

  // **And the object arrives.** 0x0053e6c0 starts the animation with `at` as
  // its own end point and the original's next `StopAnimation` (0x0053d730)
  // commits it into `[obj+0x24]`, which is what `Obj::pos` reads -- so a retail
  // crow's script-visible position catches up one animation late. This engine
  // has no animation-driven position to interpolate, so the arrival happens
  // here instead, *after* the facing has been taken from the old one.
  //
  // The difference is a phase, not a path: every quantity the original computes
  // at a `PlayAnim` -- the facing, the terrain height under the bird, the
  // `in_air` comparison -- it computes at the previous call's destination, and
  // so does this, because that is where the object already is. What differs is
  // only what `.pos` answers *between* two calls, and `CROW_MOVE.VS`'s
  // `while (.DistTo(pt) > 100)` is why the alternative is not an option: a bird
  // that never arrives never leaves that loop.
  //
  // What it arrives *along* is the view's: the leg is kept beside the object
  // for the draw, which runs the bird from one end to the other over the
  // animation (`flight_progress`, `sim/flying.hpp`). Presentation, and written
  // only once an animation is playing to carry it.
  const Point from = world->resolve_position(id);
  (void)world->set_position(id, destination);
  if (playing_now) {
    if (WorldObject* moved = world->find(id); moved != nullptr) {
      moved->flight = FlightLeg{from, world->resolve_position(id), true};
    }
  }

  HostOutcome out;
  out.status = script::HostStatus::suspend;
  out.suspend_for = playing_now ? length_of(*slot, anim_slot) : 0;
  if (out.suspend_for < 0) out.suspend_for = 0;
  return out;
}

/// `TimeToAnimFinish()` -- 29 sites, and **the answer is never large**.
///
/// `Sleep(.TimeToAnimFinish())` is the shipped idiom, at 20 of the 29 sites
/// verbatim. `0x005a9f20` answers **0** in three distinct degenerate cases --
/// nothing playing, already finished, and an unresolvable receiver -- because
/// `GetAnimStatus` fills its output with `-1` when the slot is `-1` and the
/// caller then clamps. So the failure mode of this entry point is a script that
/// runs a turn early, never one that hangs for a minute, and a wrong large
/// number here would be much worse than a wrong zero.
///
/// **The clamp at the bottom is a second guard on a property something else
/// already holds**, and saying so is better than leaving it looking untested:
/// `AnimTimeline::normalise` clamps a held animation's elapsed time to the
/// cycle, so `total - elapsed` cannot go negative and a fault injected into the
/// clamp survives every test in the suite. It is written the way the original
/// writes it, because the guarantee belongs to the timeline and not to here.
///
/// The end is the start plus the declared `duration` (`length_of`). **Two
/// approximations, neither reached by a shipped site** (every one calls this
/// right after its own `StartAnim`): the original answers 0 for an object
/// resting in a state, whose loop is not a playing animation (`CObj::SetState`
/// sets the slot to -1), where this counts down the loop's cursor; and an
/// animation whose strip runs out before its declared length answers 0 from
/// then on here, where the original keeps counting to the declared end.
HostOutcome m_time_to_anim_finish(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("TimeToAnimFinish: no world");
  if (slot == nullptr) return integer(0);
  const EntityAnim* anim = playing(*slot);
  if (anim == nullptr) return integer(0);
  const std::int32_t total = anim->duration;
  const std::int32_t elapsed = slot->object->anim.elapsed_ms;
  // A strip that ran out before the declared length leaves a cursor that has
  // stopped counting, so how far past it the clock now stands is not kept.
  // Zero, by the asymmetry above.
  if (!slot->animating && total > elapsed) return integer(0);
  // At the script's millisecond, which the world's clock may be ahead of.
  const std::int32_t left =
      total - elapsed + static_cast<std::int32_t>(script_lag(ctx, *world));
  return integer(left > 0 ? left : 0);
}

/// `TimeToActionMoment()` -- 26 sites, and the moment is the entity's.
///
/// `0x005a9e70` is `TimeToAnimFinish` against a different cached time:
/// `AnimStart + lead + actMoment * playbackDuration / nominalDuration`. With no
/// duration override the ratio is one, and this engine has no override, so it
/// is `action_time` into the animation -- the `<anim action_time>` the entity
/// document declares, which `game/entity.hpp` already reads as *"ms into the
/// animation at which the effect fires"*.
///
/// Same three zeroes as `TimeToAnimFinish`, and one more: an animation whose
/// action moment has already passed answers 0 rather than a negative.
HostOutcome m_time_to_action_moment(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("TimeToActionMoment: no world");
  if (slot == nullptr) return integer(0);
  const EntityAnim* anim = playing(*slot);
  if (anim == nullptr) return integer(0);
  const std::int32_t left = anim->action_time - slot->object->anim.elapsed_ms +
                            static_cast<std::int32_t>(script_lag(ctx, *world));
  return integer(left > 0 ? left : 0);
}

/// `GetAnim()` -- the slot playing now, or **0** for nothing.
///
/// `0x005a9fd0` is `push (slot < 0) ? 0 : slot + 1`, so zero is the sentinel
/// and every real answer is a document number. Zero shipped call sites; bound
/// because it costs one field read and because the corpus comments show the
/// guard the family expects to write against it.
HostOutcome m_get_anim(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("GetAnim: no world");
  if (slot == nullptr || slot->object == nullptr) return integer(0);
  const std::int32_t playing_slot = slot->object->anim.anim_slot;
  return integer(playing_slot == kNoAnim ? 0 : playing_slot);
}

/// `GetAnimTime(int slot)` -- how long that animation lasts, whether or not it
/// is playing. One site: `s.AddDruid(this, .GetAnimTime(18));`.
HostOutcome m_get_anim_time(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("GetAnimTime: no world");
  if (slot == nullptr) return integer(0);
  return integer(length_of(*slot, int_arg(ctx, 1)));
}

/// `Unit::GetAnimDuration(int slot)` -- the same number, **one slot lower**.
///
/// Two sites, one line: `EAGLE_MOVE.VS:86` is
/// `speed = .speed * .GetAnimDuration(4) / .GetAnimDuration(0);`, and 4 and 0
/// become 5 and 1 -- the attack animation over the walk, which is a ratio that
/// means something. Under the `Obj::` numbering they would be 4 and 0, and no
/// entity declares either.
///
/// The original does not check its receiver here and dereferences null. That is
/// a fault; this answers 0.
HostOutcome m_get_anim_duration(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("GetAnimDuration: no world");
  if (slot == nullptr) return integer(0);
  return integer(length_of(*slot, int_arg(ctx, 1) + 1));
}

/// `SetState(int)` -- **two entry points wearing one name**.
///
/// Twelve of the sixteen shipped sites are `sq.SetState(SS_IDLE)` on a squad;
/// four are `.SetState(2)` on a hero's grave, stepping its decay through the
/// entity's four later states. `gbr.exe` registers two functions on two
/// receivers and this registry keys on (kind, name, arity), so the handle type
/// picks the half.
///
/// **The object half hard-resets the cursor.** `CObj::SetState` (0x0053e860)
/// returns immediately when the state is already current, and otherwise sets
/// the playing slot to `-1` and recomputes -- so entering a state stops
/// whatever was playing. It is called with `useTransition = 0` from the host,
/// so the transition animation the engine-side caller would play is skipped;
/// `World::enter_state` matches, and loops the state's own `anim_idx` when it
/// names one.
///
/// **The squad half writes the time too.** `Squad::StateTime` is persisted as
/// `StateSetTime` and its three corpus sites all compare it against `GetTime()`,
/// so it is when the state was last set and not how long it has been held.
/// 0x004211c0 splits the handle word (low four bits the player, the rest the
/// index), looks the record up (0x00443e30) and writes the state as a 16-bit
/// word at `+0x28` and the time at `+0x2c`, with no null check and no `SF_NOAI`
/// refusal; a handle naming no squad faults there and is a no-op here. The
/// state is kept 32 bits wide here, as every other writer keeps it; no shipped
/// state needs more than 16.
///
/// `Squad::SetState(nState, nSetFlags, nClrFlags)` (0x00421380) is registered
/// too, and is not bound: it is the same with `flags = (flags | set) & ~clr`,
/// and both of its corpus sites (`SQUADMONITOR.VS`) are inside a comment.
HostOutcome m_set_state(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetState: no world");
  const std::int32_t state = int_arg(ctx, 1);

  if (ctx.count() > 0 && is_squad(ctx.arg(0))) {
    HeroSystem* heroes = hero_system_of(*world);
    if (heroes == nullptr) return HostOutcome::ok_void();
    Squad* squad = heroes->squads().find(unpack_squad(ctx.arg(0)));
    if (squad == nullptr) return HostOutcome::ok_void();
    // Unconditionally: 0x004211c0 writes the state and then the clock with no
    // compare between them, so `sq.SetState(SS_IDLE)` on a squad already idle
    // restarts `StateTime`. This used to write only on a change, which is the
    // object half's rule and not the squad's; `SQUADMONITOR.VS` times its
    // idling from the stamp.
    //
    // The stamp is the game's running time (`[game+0x1258]`), which is the
    // millisecond the script woke on -- the scheduler's `now`, as `GetTime`
    // answers it -- not the turn's end the world already stands at. The three
    // sites that read it subtract it from `GetTime()`.
    squad->state = state;
    squad->state_time = ctx.scheduler != nullptr ? ctx.scheduler->now() : world->time();
    return HostOutcome::ok_void();
  }

  World* ignored = nullptr;
  const WorldObject* slot = receiver_object(ctx, ignored);
  if (slot == nullptr) return HostOutcome::ok_void();
  (void)world->enter_state(slot->id, state);
  return HostOutcome::ok_void();
}

/// `Obj::IsInState()` -- whether the object is holding a state rather than
/// playing an animation. 2 sites, and one internal caller.
///
/// **What it reads.** `0x005ad860` is `0x0053cfc0 != 0`, and `0x0053cfc0` asks
/// whether the object's pending-message list (`[obj+0xc]`, searched by
/// 0x00687820) holds **no** message with id 27 -- the animation timer.
/// `StartAnimation` (0x0053e6c0) arms it (0x00688240) for every animation it
/// starts, the timer's handler (0x0053f6a0) re-arms it through the
/// animation's phases and lets it lapse at the end, and `StopAnimation`
/// (0x0053d730) and an off-map start cancel it (0x00687ce0). So an object is
/// *in a state* exactly when no animation is playing on it.
///
/// `CROW_MOVE.VS` and `EAGLE_MOVE.VS` open with
///
///     while (! .IsInState) Sleep(50);
///
/// before their first `PlayAnim`: a bird waiting for whatever `CROW_IDLE.VS`
/// last started to run out. `Flying::PickFlyingPoint` (0x0051c530) reads the
/// same test.
///
/// This engine's bit for it is `WorldObject::animating`, set by `play_anim`
/// and cleared by the turn loop when the strip finishes (`world.cpp`); so
/// `IsInState` is its negation. **Approximated** in when the animation ends:
/// the original's timer lapses at the declared `duration`, the strip's end
/// here (`length_of`). An object with no entity at all is in a state in both.
///
/// The receiver is not checked: a handle naming nothing hands a null object to
/// the list search, which faults. This answers false.
HostOutcome m_is_in_state(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("IsInState: no world");
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && !slot->animating));
}

struct AnimHostDef {
  CallKind kind;
  std::string_view name;
  std::uint16_t arity;
  script::HostFn fn;
};

constexpr AnimHostDef kAnimHosts[] = {
    {CallKind::member, "StartAnim", 2, &m_start_anim},                  // 35
    {CallKind::member, "TimeToAnimFinish", 0, &m_time_to_anim_finish},  // 29
    {CallKind::member, "TimeToActionMoment", 0, &m_time_to_action_moment},  // 26
    {CallKind::member, "SetState", 1, &m_set_state},                    // 16
    // The two suspending ones, and the two arities are two receivers rather
    // than two shapes of one call: `Obj` and `Flying`.
    {CallKind::member, "PlayAnim", 2, &m_play_anim},                    // 13
    {CallKind::member, "PlayAnim", 3, &m_play_anim_flying},             // 13
    {CallKind::member, "GetAnimDuration", 1, &m_get_anim_duration},     //  2
    {CallKind::member, "GetAnimTime", 1, &m_get_anim_time},             //  1
    {CallKind::member, "StartDelayedAnim", 3, &m_start_delayed_anim},   //  1
    {CallKind::member, "GetAnim", 0, &m_get_anim},                      //  0
    // Not an animation entry point by name, and one by every other measure:
    // the only state it can read is the animation cursor. See its comment.
    {CallKind::member, "IsInState", 0, &m_is_in_state},                 //  2
};

}  // namespace

std::uint32_t build_frame_row(const EntityAnim& anim, const AnimTimeline& timeline,
                              std::int32_t frame) noexcept {
  // An equivalence, kept knowingly: `row_of_step` answers 0 for a timeline
  // with no steps too, so a fault that drops this survives the suite. It is
  // here so that the clamp below never sees a cycle of -1 steps.
  if (!timeline.valid()) return 0;
  const bool entry_marker = !anim.frame_durations.empty() && anim.frame_durations.front() == 0;
  std::int64_t step = static_cast<std::int64_t>(frame) - (entry_marker ? 1 : 0);
  const std::int64_t last = static_cast<std::int64_t>(timeline.steps()) - 1;
  if (step < 0) step = 0;
  if (step > last) step = last;
  return timeline.row_of_step(static_cast<std::uint32_t>(step));
}

std::size_t register_anim_host(HostRegistry& registry) {
  for (const AnimHostDef& def : kAnimHosts) {
    registry.define(def.kind, def.name, def.arity, def.fn);
  }
  return anim_host_entry_count();
}

std::size_t anim_host_entry_count() noexcept {
  return sizeof(kAnimHosts) / sizeof(kAnimHosts[0]);
}

}  // namespace imperivm::core::sim
