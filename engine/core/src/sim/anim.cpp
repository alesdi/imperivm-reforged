// The animation entry points. See include/imperivm/core/sim/anim.hpp.

#include "imperivm/core/sim/anim.hpp"

#include <vector>

#include "imperivm/core/game/entity.hpp"
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

/// How long one full cycle of `slot`'s animation lasts, or 0.
///
/// **The timeline's number, not the document's `duration` attribute.** The
/// original reads a single nominal field; this engine has two -- the `duration`
/// attribute and the sum of the frame holds -- and they disagree in 127 of the
/// 1,168 shipped animations, with no evidence for which the original honours
/// (`docs/formats/ent-xml.md`). The timeline's is the one `TimeToAnimFinish`
/// counts down against, so answering with the other would be two numbers for
/// one question, which is the shape of defect this tree keeps finding.
[[nodiscard]] std::int32_t cycle_of(const WorldObject& slot, std::int32_t anim_slot) {
  if (slot.object == nullptr || slot.object->entity == nullptr) return 0;
  const EntityAnim* anim = slot.object->entity->anim(anim_slot);
  if (anim == nullptr) return 0;
  const AnimTimeline timeline = slot.object->entity->timeline(*anim);
  return timeline.valid() ? timeline.cycle() : 0;
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

/// Start `slot` on the receiver, facing the point at argument `at`.
///
/// `AnimRepeat::hold`, always. `entity.hpp` draws the line where the evidence
/// does: an animation named by a state's `anim_idx` is that pose's own loop,
/// and one a script starts runs once and hands control back. Every entry point
/// here is the second kind.
bool start(CallContext& ctx, World& world, const WorldObject& slot, std::int32_t anim_slot,
           std::size_t at) {
  face_unless_self(ctx, world, slot, at);
  return world.play_anim(slot.id, anim_slot, AnimRepeat::hold);
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
/// Start it, then hold the script for the animation's length. See the header
/// for the mechanism the original uses and why this engine cannot use it.
///
/// A slot the entity does not declare plays nothing and suspends for zero,
/// which still yields: `Scheduler::run_ready` resumes each script at most once
/// per pass, so a zero-length suspension costs a turn rather than spinning.
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
  out.suspend_for = playing_now ? cycle_of(*slot, anim_slot) : 0;
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
  out.suspend_for = playing_now ? cycle_of(*slot, anim_slot) : 0;
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
HostOutcome m_time_to_anim_finish(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("TimeToAnimFinish: no world");
  if (slot == nullptr) return integer(0);
  const EntityAnim* anim = playing(*slot);
  if (anim == nullptr) return integer(0);
  const std::int32_t total = cycle_of(*slot, slot->object->anim.anim_slot);
  const std::int32_t left = total - slot->object->anim.elapsed_ms;
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
  const std::int32_t left = anim->action_time - slot->object->anim.elapsed_ms;
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
  return integer(cycle_of(*slot, int_arg(ctx, 1)));
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
  return integer(cycle_of(*slot, int_arg(ctx, 1) + 1));
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
    squad->state = state;
    squad->state_time = world->time();
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
/// **What the executable says, and what it does not.** `0x005ad860` is
/// `0x0053cfc0 != 0`, and `0x0053cfc0` asks the object whether its component
/// list at `[obj+0xc]` holds an entry keyed 27 -- answering *true when it does
/// not*. What component 27 is has not been recovered: nothing in the executable
/// names it, and the only other constant that list is searched for is 23, from
/// an unrelated caller. So the reading here comes from the two places the
/// answer is used rather than from the field.
///
/// Both point the same way. `CROW_MOVE.VS` and `EAGLE_MOVE.VS` open with
///
///     while (! .IsInState) Sleep(50);
///
/// before their first `PlayAnim`, which is a bird waiting for whatever
/// `CROW_IDLE.VS` last started to run out; and `Flying::PickFlyingPoint`
/// (0x0051c530) takes the first flock-mate that is **not** in a state and *is*
/// in the air, which is a bird whose heading is live because it is mid-flight.
/// An entry that an action installs and its completion removes satisfies both,
/// and the name is then literal: an object *in* a state is one that has settled
/// into a pose.
///
/// This engine already has that bit, hashed and saved: `WorldObject::animating`
/// is set by `play_anim` and cleared by the turn loop when the sample finishes
/// (`world.cpp`). So `IsInState` is its negation, and the wait above terminates
/// for every animation this engine can start -- which the reading has to, or
/// two shipped scripts would sleep for the rest of the match.
///
/// **What is still unknown**: whether an object with no entity at all -- 579 of
/// the 889 declare no animation -- is in a state in the original. Here it is,
/// because `animating` is false for it, and that is the answer that keeps the
/// wait loop terminating.
HostOutcome m_is_in_state(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("IsInState: no world");
  // An unresolvable receiver reports and answers false, which is `setne` on the
  // zero the diagnostic path leaves behind.
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
