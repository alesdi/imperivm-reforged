// The animation entry points. See include/imperivm/core/sim/anim.hpp and
// docs/formats/animation.md.

#include "imperivm/core/sim/anim.hpp"

#include <limits>
#include <vector>

#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
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

/// How long the animation at `anim_slot` lasts as far as a script can tell:
/// the entity document's `duration` attribute, or 0.
///
/// **The declared `duration`, not the frame strip's sum.** The loader
/// (0x006004d0) stores the `<anim duration>` attribute at `[anim+0x4c]`, and
/// that one field is every script-visible length in this family:
/// `StartAnimation` (0x0053e6c0) takes it as the playback length `[obj+0x64]`
/// unless a caller overrides it, the timer ends the animation there
/// (0x0053f6a0), and `GetAnimTime` (0x005a9c50), `Unit::GetAnimDuration`
/// (0x0051c180) and `Flying::PlayAnim`'s suspension (0x0051bcba) read it
/// verbatim. The two disagree in 127 of the 1,168 shipped animations -- the
/// barrack horses' idle declares 1000 over a 1200 strip, the hero grave's
/// crumble 6486 over 12100. The strip still drives which sprite row is drawn
/// (`AnimTimeline`); this is the clock the scripts see.
[[nodiscard]] std::int32_t length_of(const WorldObject& slot, std::int32_t anim_slot) {
  if (slot.object == nullptr || slot.object->entity == nullptr) return 0;
  const EntityAnim* anim = slot.object->entity->anim(anim_slot);
  return anim == nullptr ? 0 : anim->duration;
}

/// The facing argument at `at` as `StartAnimation` takes it: the invalid
/// point when it is absent or is the object's own position.
///
/// Every entry point here replaces the argument with the invalid point when
/// it equals the receiver's position (0x005a9ce0, 0x005a9da0, 0x005a9ba0), and
/// `StartAnimation` then leaves the facing alone. That is what
/// `StartAnim(1, .pos)` means -- *play it and do not turn* -- and the sites
/// that pass `tgt.pos` turn.
[[nodiscard]] Point facing_argument(CallContext& ctx, World& world, const WorldObject& slot,
                                    std::size_t at) {
  if (ctx.count() <= at || !is_point(ctx.arg(at))) return kHeldPosition;
  const Point towards = unpack_point(ctx.arg(at));
  if (towards == world.resolve_position(slot.id)) return kHeldPosition;
  return towards;
}

/// A wait for a wake that never comes: the original's script sleeps with no
/// timed wake, and what was to wake it will not run. Long enough to outlast
/// any match, short enough not to overflow a time.
constexpr std::int64_t kNoWake = std::numeric_limits<std::int32_t>::max();

[[nodiscard]] HostOutcome again_in(std::int64_t ms) {
  HostOutcome out;
  out.status = script::HostStatus::retry;
  out.suspend_for = ms > 0 ? ms : 0;
  return out;
}

}  // namespace

GameTime script_now(const CallContext& ctx, const World& world) {
  return ctx.scheduler != nullptr ? ctx.scheduler->now() : world.time();
}

std::int32_t start_animation_facing_at(World& world, ObjectId id, std::int32_t anim_slot,
                                       Point face, GameTime now, GameTime delay, Point origin) {
  // Off the map the timer is cancelled and nothing starts -- and nothing
  // turns: that test opens 0x0053e6c0, and the facing is set inside it.
  if (const ObjectState* state = world.state(id);
      state != nullptr && state->position != kHeldPosition && face != kHeldPosition) {
    if (MovementSystem* movement = movement_system(world); movement != nullptr) {
      movement->face(world, id, face);
    }
  }
  return world.start_animation(id, anim_slot, now, delay, 0, origin);
}

std::int32_t start_animation_facing(const CallContext& ctx, World& world, ObjectId id,
                                    std::int32_t anim_slot, Point face, GameTime delay) {
  return start_animation_facing_at(world, id, anim_slot, face, script_now(ctx, world), delay);
}

bool wake_waiter(World& world, script::Scheduler* scheduler, ObjectId id, GameTime now) {
  WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->clock.waiter == script::kNoScript) return false;
  const script::ScriptId waiter = slot->clock.waiter;
  slot->clock.waiter = script::kNoScript;
  return scheduler != nullptr && scheduler->wake(waiter, now);
}

void anim_end_handler(World& world, script::Scheduler* scheduler, CallContext* ctx, ObjectId id,
                      GameTime now) {
  // A flying unit's own step first (0x0051b450), then the unit's handler.
  flying_anim_end(world, id);
  WorldObject* slot = world.find(id);
  if (slot == nullptr) return;
  // A unit's handler (0x005d5b60) does nothing at all while it is held.
  if (slot->state.flags.is_unit && slot->state.is_held()) return;
  // Dead (`vtbl+0x50`, health gone) and not yet put down: the death
  // animation when the entity declares one (mode 1), else straight to the
  // end (mode 8) -- 0x005b0f70, and 0x005d5b75 before it for a unit.
  if (object_is_dead(*slot) && slot->clock.mode != 8) {
    const bool has_death = slot->object != nullptr && slot->object->entity != nullptr &&
                           slot->object->entity->has_anim(kAnimDie);
    slot->clock.mode = has_death ? 1 : 8;
  }
  switch (slot->clock.mode) {
    case 1: {
      // One of the death animation's variants (0x005a81b0 over idx 9..12,
      // one draw on the synchronised generator), the command the object
      // runs ended (0x005b07d0, when its command thread lives: 0x0069f950),
      // and the object marked put down.
      const Entity& entity = *slot->object->entity;
      std::int32_t variants = 1;
      if (entity.has_anim(kAnimDie + 1)) {
        variants = 2;
        if (entity.has_anim(kAnimDie + 2)) variants = entity.has_anim(kAnimDie + 3) ? 4 : 3;
      }
      const std::int32_t chosen = kAnimDie + world.rng().between(0, variants - 1);
      (void)world.start_animation(id, chosen, now);
      if (CommandSystem* commands = command_system(world); commands != nullptr) {
        const script::ScriptId previous =
            commands->set_caller(ctx != nullptr ? ctx->script : script::kNoScript);
        (void)commands->kill_command(world, id);
        (void)commands->set_caller(previous);
      }
      world.set_anim_mode(id, 8);
      return;
    }
    case 8:
      // The object's destroy (`vtbl+0x94`): from a script's call, deferred to
      // the end of its slice when the object is the script's own.
      if (ctx != nullptr) {
        erase_from_script(*ctx, world, id);
      } else {
        destroy_object(scheduler, world, id);
      }
      return;
    case 4:
    case 5:
      (void)idle_activity_step(world, scheduler, id, now);
      return;
    case 6: {
      // `PlayAnim`'s continuation (0x005a76c0): the animation from the
      // call's arguments, marked used; at its end, the wake and mode 0.
      if (slot->clock.play_started) {
        world.set_anim_mode(id, 0);
        (void)wake_waiter(world, scheduler, id, now);
        return;
      }
      slot->clock.play_started = true;
      (void)start_animation_facing_at(world, id, slot->clock.play_slot, slot->clock.play_face, now,
                                      0);
      return;
    }
    case 3:
      // The walk step (0x005d3b50).
      if (MovementSystem* movement = movement_system(world); movement != nullptr) {
        movement->walk_step(world, scheduler, id, now);
      }
      return;
    case 2:
      // The attack's swing, or its answer (`vtbl+0x11c`, 0x005d5ed0).
      if (slot->state.flags.is_unit) {
        if (CombatSystem* combat = combat_system_of(world); combat != nullptr) {
          combat->attack_end(world, scheduler, id, now);
        }
      }
      return;
    default:
      // 0: nothing.
      return;
  }
}

void anim_action_handler(World& world, script::Scheduler* scheduler, ObjectId id, GameTime now) {
  (void)scheduler;
  const WorldObject* slot = world.find(id);
  // A unit's action handler (0x005d9550): the blow in the attack's mode
  // (`vtbl+0x120`); its sounds are the renderer's.
  if (slot == nullptr || !slot->state.flags.is_unit || slot->clock.mode != 2) return;
  if (CombatSystem* combat = combat_system_of(world); combat != nullptr) {
    combat->attack_action(world, id, now);
  }
}

void AnimEvents::install() {
  world_->set_anim_end_handler(this);
  world_->set_arm_source(
      [](void* user) { return static_cast<script::Scheduler*>(user)->take_arm(); }, scheduler_);
  world_->set_timers_swept(true);
  scheduler_->set_timer_source(this);
}

void AnimEvents::anim_end(World& world, ObjectId id, GameTime now) {
  anim_end_handler(world, scheduler_, nullptr, id, now);
}

void AnimEvents::anim_action(World& world, ObjectId id, GameTime now) {
  anim_action_handler(world, scheduler_, id, now);
}

bool AnimEvents::next_timer(std::int64_t& at, std::uint64_t& armed) const {
  ObjectId id = kNoObject;
  return world_->next_anim_timer(at, armed, id);
}

void AnimEvents::fire_timer() { world_->fire_anim_timer(); }

namespace {

// --------------------------------------------------------------------------
// the entry points
// --------------------------------------------------------------------------

/// `StartAnim(int slot, point face)` and `StartDelayedAnim(int slot, point
/// face, int delay)` -- 35 sites and one. Void, and they do not block.
///
/// 0x005a9ce0 and 0x005a9da0, in their order: the receiver (one naming
/// nothing prints into the discarded sink and returns), the facing argument
/// made invalid when it is the object's own position, **the action mode
/// `[obj+0x130]` set to 0**, then, **when the animation timer is not
/// pending** (0x0053cfc0), the object's animation-end handler (`vtbl+0x54`)
/// with that mode: nothing for a living object, the death sequence for a
/// dead one (`anim_end_handler`). Then `StartAnimation` with the document's
/// number less one -- this engine keys on the document's number -- no
/// origin, and the delay: 0 for `StartAnim`, the argument for
/// `StartDelayedAnim`. A delay of -1 starts it at once and arms no timer, so
/// it never ends (`CATAPULT_IDLE.VS:32`, the one shipped delay); a positive
/// one leaves the object resting where it is until the delay is over.
HostOutcome start_anim_common(CallContext& ctx, GameTime delay) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("StartAnim: no world");
  if (slot == nullptr) return HostOutcome::ok_void();
  const ObjectId id = slot->id;
  const Point face = facing_argument(ctx, *world, *slot, 2);
  const GameTime now = script_now(ctx, *world);
  world->set_anim_mode(id, 0);
  if (!world->anim_timer_pending(*slot)) anim_end_handler(*world, ctx.scheduler, &ctx, id, now);
  if (world->find(id) == nullptr) return HostOutcome::ok_void();
  (void)start_animation_facing(ctx, *world, id, int_arg(ctx, 1), face, delay);
  return HostOutcome::ok_void();
}

HostOutcome m_start_anim(CallContext& ctx) { return start_anim_common(ctx, 0); }

HostOutcome m_start_delayed_anim(CallContext& ctx) {
  return start_anim_common(ctx, int_arg(ctx, 3));
}

/// `PlayAnim(int slot, point face)` -- **suspending**. 13 sites.
///
/// 0x005a9ba0 starts nothing. It writes 6 into the action mode, arms the
/// animation timer for 1 ms when none is pending (0x0053ced0), and answers
/// 3: the script sleeps with no timed wake. When the timer fires -- 1 ms on,
/// or at the end of whatever was in flight -- the object's handler runs mode
/// 6 (0x005b0f70 -> 0x005a76c0), which starts the animation from the
/// arguments still on the script's stack and marks them used; when that one
/// ends, the handler runs mode 6 again, finds them used, wakes the script
/// (0x0069f8f0) and sets the mode back to 0.
///
/// The firings are the timer's (`anim_end_handler`, mode 6), and the
/// arguments are kept on the object's clock in place of the script's stack
/// (`AnimClock::play_slot`, `play_face`, `play_started`), with the script
/// to wake (`waiter`). The handler's own gates come first: a held unit, a
/// dead object, or a mode something else has rewritten since -- another
/// script's `StartAnim`, an `Idle` -- runs no continuation, and the script
/// is never woken by it. A timer moved later by another start is waited
/// out. An object off the map at the start starts nothing and has no
/// timer, so nothing wakes the script either.
HostOutcome m_play_anim(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("PlayAnim: no world");
  if (slot == nullptr) return HostOutcome::ok_void();
  const ObjectId id = slot->id;
  const GameTime now = script_now(ctx, *world);
  const Point face = facing_argument(ctx, *world, *slot, 2);
  world->set_anim_mode(id, 6);
  if (WorldObject* own = world->find(id); own != nullptr) {
    own->clock.play_slot = int_arg(ctx, 1);
    own->clock.play_face = face;
    own->clock.play_started = false;
    own->clock.waiter = ctx.script;
    if (!world->anim_timer_pending(*own)) world->arm_anim_timer(id, now, 1);
  }
  // 3: the call is done and the slice ends with no timed wake.
  HostOutcome out;
  out.status = script::HostStatus::suspend;
  out.suspend_for = kNoWake;
  return out;
}

/// `Flying::PlayAnim(int slot, point at, int z)` -- **suspending**. 13 sites,
/// all crows and eagles.
///
/// Four differences from the `Obj` form:
///
///   * **the slot is one less**, because this body passes its argument raw into
///     the array `Obj::PlayAnim` decrements into. See the header.
///   * **the Z is range-checked**: `-1 .. 1024`, and out of range is a refusal
///     that starts nothing (0x0051bb6f). `-1` means *use the terrain height and
///     land*.
///   * the slot is checked before anything is written, and a bad one returns
///     without suspending;
///   * it starts the animation itself: the action mode to 0, `StopAnimation`,
///     then `StartAnimation` with the point as the animation's *origin* as
///     well as its facing, and the script waits the animation's declared
///     length (0x0051bcba) -- not the transition in front of it.
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
  // The re-entry returns (0x0051bb2c's first-run byte).
  if (!ctx.first_call) return HostOutcome::ok_void();

  const std::int32_t z = int_arg(ctx, 3);
  // *"Invalid Z value ... must be between -1 and 1024"*, printed into a sink
  // that is a bare `ret`, and then nothing happens.
  if (z < -1 || z > 1024) return HostOutcome::ok_void();

  // Then the slot, before anything is written (0x0051bb7d..0x0051bbb5): a
  // negative argument, one past the entity's last, or one naming no animation
  // prints *"Invalid animation specified"* and returns without suspending --
  // no flight state, no move, no wait.
  const std::int32_t raw_slot = int_arg(ctx, 1);
  const std::int32_t anim_slot = raw_slot + 1;
  if (raw_slot < 0 || slot->object == nullptr || slot->object->entity == nullptr ||
      slot->object->entity->anim(anim_slot) == nullptr) {
    return HostOutcome::ok_void();
  }

  // The point is the animation's *origin* in the original and its facing target
  // here; either way it is where the descent's terrain height is read from, so
  // an absent or malformed one falls back to the object's own position rather
  // than to the origin of the map. **Clamped into the map rectangle** first
  // (0x004092b0, then 0x00422b70 against `[map+0xb4..0xc0]`), in place, so
  // the bird lands on the edge rather than off it.
  Point destination = ctx.count() > 2 && is_point(ctx.arg(2))
                          ? unpack_point(ctx.arg(2))
                          : world->resolve_position(slot->id);
  if (const MovementSystem* movement = movement_system(*world); movement != nullptr) {
    destination = movement->grid().clamp_to_map(destination);
  }
  const ObjectId id = slot->id;
  const GameTime now = script_now(ctx, *world);
  begin_flight_anim(*world, id, destination, z, now);
  world->set_anim_mode(id, 0);
  world->stop_animation(id, now);
  slot = world->find(id);
  if (slot == nullptr) return HostOutcome::ok_void();

  const Point face = destination == world->resolve_position(id) ? kHeldPosition : destination;
  // **The point is the animation's origin**: the timer puts the bird there
  // at the animation's main start, after any transition (0x0053f6d3, the
  // origin and the position swapped), and the drawn point runs to it over
  // the animation from where it was (`World::current_position`).
  //
  // `CROW_MOVE.VS`'s `while (.DistTo(pt) > 100)` is why a bird must arrive:
  // one that never does never leaves that loop. What it arrives *along* is
  // the view's: the leg is kept beside the object for the draw, which runs
  // the bird from one end to the other over the animation (`flight_progress`,
  // `sim/flying.hpp`). Presentation, and written only once an animation is
  // playing to carry it.
  const Point from = world->resolve_position(id);
  (void)start_animation_facing_at(*world, id, anim_slot, face, now, 0, destination);
  const WorldObject* started = world->find(id);
  if (started == nullptr) return HostOutcome::ok_void();
  if (started->clock.slot == anim_slot) {
    if (WorldObject* moved = world->find(id); moved != nullptr) {
      moved->flight = FlightLeg{from, destination, true};
    }
  }

  HostOutcome out;
  out.status = script::HostStatus::suspend;
  out.suspend_for = length_of(*world->find(id), anim_slot);
  if (out.suspend_for < 0) out.suspend_for = 0;
  return out;
}

/// `TimeToAnimFinish()` -- 29 sites -- and `TimeToActionMoment()` -- 26.
///
/// 0x005a9f20 and 0x005a9e70 read the animation's cached times
/// (`GetAnimStatus`, 0x0053cef0): with the slot at -1 -- nothing started, or
/// the animation's timer has run to its end -- *"Function called with no
/// animation playing"* and 0; otherwise the end (`[obj+0x68] + [obj+0x64]`,
/// the transition included) or the action moment (`[obj+0x6c]`) less the
/// game's time when that is later, else 0. Read at the script's millisecond.
/// A receiver naming nothing answers 0.
HostOutcome time_to(CallContext& ctx, const char* no_world, bool action) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed(no_world);
  if (slot == nullptr) return integer(0);
  const GameTime now = script_now(ctx, *world);
  if (world->anim_slot_at(*slot, now) == kNoAnim) return integer(0);
  const GameTime at = action ? slot->clock.action_at : slot->clock.end();
  return integer(at > now ? static_cast<std::int32_t>(at - now) : 0);
}

HostOutcome m_time_to_anim_finish(CallContext& ctx) {
  return time_to(ctx, "TimeToAnimFinish: no world", false);
}

HostOutcome m_time_to_action_moment(CallContext& ctx) {
  return time_to(ctx, "TimeToActionMoment: no world", true);
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
  if (slot == nullptr) return integer(0);
  const std::int32_t playing = world->anim_slot_at(*slot, script_now(ctx, *world));
  return integer(playing == kNoAnim ? 0 : playing);
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
/// means something.
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

/// The squad half of `SetState`: the state, the flags `(flags | set) & ~clr`
/// when given, and the time.
///
/// `Squad::SetState(nState)` (0x004211c0) splits the handle word (low four
/// bits the player, the rest the index), looks the record up (0x00443e30)
/// and writes the state as a 16-bit word at `+0x28` and the game's running
/// time at `+0x2c` -- unconditionally, so `sq.SetState(SS_IDLE)` on an idle
/// squad restarts `StateTime` -- with no null check (a handle naming no squad
/// faults there and is a no-op here) and no `SF_NOAI` refusal.
/// `Squad::SetState(nState, nSetFlags, nClrFlags)` (0x00421380) is the same
/// record, null-checked, with the flags word at `+0x30` set and cleared
/// between the state and the time. The state is kept 32 bits wide
/// here, as every other writer keeps it.
void set_squad_state(CallContext& ctx, World& world, bool with_flags) {
  HeroSystem* heroes = hero_system_of(world);
  if (heroes == nullptr) return;
  Squad* squad = heroes->squads().find(unpack_squad(ctx.arg(0)));
  if (squad == nullptr) return;
  squad->state = int_arg(ctx, 1);
  if (with_flags) {
    // The flags word at `+0x30`, sixteen bits: set, then clear.
    const auto set = static_cast<std::uint16_t>(int_arg(ctx, 2));
    const auto clear = static_cast<std::uint16_t>(int_arg(ctx, 3));
    squad->flags = static_cast<std::uint16_t>((squad->flags | set) & ~clear);
  }
  // The game's running time (`[game+0x1258]`): the millisecond the script
  // woke on, which is what `GetTime` answers.
  squad->state_time = script_now(ctx, world);
}

/// `SetState(int)` -- **two entry points wearing one name**, and a third.
///
/// Twelve of the sixteen shipped sites are `sq.SetState(SS_IDLE)` on a squad;
/// four are `.SetState(2)` on a hero's grave, stepping its decay through the
/// entity's four later states. `gbr.exe` registers two functions on two
/// receivers and this registry keys on (kind, name, arity), so the handle type
/// picks the half.
///
/// **The object half** (0x005aa040) checks the handle (a dead one reports and
/// pushes an int, see the header) and calls `CObj::SetState` (0x0053e860)
/// with the script's number less one -- the state array is the document's
/// `idx - 1` -- and no transition: nothing when the object already rests in
/// the state, otherwise the slot dropped and the state written
/// (`World::set_anim_state`).
HostOutcome m_set_state(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetState: no world");
  if (ctx.count() > 0 && is_squad(ctx.arg(0))) {
    set_squad_state(ctx, *world, false);
    return HostOutcome::ok_void();
  }
  World* ignored = nullptr;
  const WorldObject* slot = receiver_object(ctx, ignored);
  if (slot == nullptr) return HostOutcome::ok_void();
  (void)world->set_anim_state(slot->id, int_arg(ctx, 1), false, script_now(ctx, *world));
  return HostOutcome::ok_void();
}

/// `Squad::SetState(nState, nSetFlags, nClrFlags)` (0x00421380). Registered,
/// and both of its corpus sites (`SQUADMONITOR.VS:337`, `:339`) are inside a
/// comment -- bound all the same, because a registered name a script may call
/// is the host API, and the body is the three-argument half above.
HostOutcome m_set_state_flags(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetState: no world");
  if (ctx.count() > 0 && is_squad(ctx.arg(0))) set_squad_state(ctx, *world, true);
  return HostOutcome::ok_void();
}

/// `Obj::IsInState()` -- whether the object rests rather than waits on its
/// animation timer. 2 sites, and one internal caller.
///
/// `0x005ad860` is `0x0053cfc0 != 0`, and `0x0053cfc0` asks whether the
/// object's pending-message list (`[obj+0xc]`, searched by 0x00687820)
/// holds **no** message with id 27 -- the animation timer. `StartAnimation`
/// (0x0053e6c0) arms it for every animation it starts and it runs to the
/// animation's end; a hold (0x0053ced0: an idle's pose, `PlayAnim`'s first
/// millisecond) arms it too; `StopAnimation` (0x0053d730) and an off-map
/// start cancel it (0x00687ce0). `CROW_MOVE.VS` and `EAGLE_MOVE.VS` open with
/// `while (! .IsInState) Sleep(50);` before their first `PlayAnim`.
///
/// The receiver is not checked: a handle naming nothing hands a null object to
/// the list search, which faults. This answers false.
HostOutcome m_is_in_state(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("IsInState: no world");
  return HostOutcome::ok_with(Value::boolean(
      slot != nullptr && !world->anim_timer_pending(*slot)));
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
    {CallKind::member, "SetState", 3, &m_set_state_flags},              //  0 (2 in a comment)
    // The two suspending ones, and the two arities are two receivers rather
    // than two shapes of one call: `Obj` and `Flying`.
    {CallKind::member, "PlayAnim", 2, &m_play_anim},                    // 13
    {CallKind::member, "PlayAnim", 3, &m_play_anim_flying},             // 13
    {CallKind::member, "GetAnimDuration", 1, &m_get_anim_duration},     //  2
    {CallKind::member, "GetAnimTime", 1, &m_get_anim_time},             //  1
    {CallKind::member, "StartDelayedAnim", 3, &m_start_delayed_anim},   //  1
    {CallKind::member, "GetAnim", 0, &m_get_anim},                      //  0
    // Not an animation entry point by name, and one by every other measure:
    // the only state it can read is the animation timer. See its comment.
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
