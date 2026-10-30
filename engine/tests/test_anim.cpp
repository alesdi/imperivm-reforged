// The animation entry points: sim/anim.hpp.
//
// The entity fixture is the one `test_tick.cpp` uses, repeated rather than
// shared because these tests turn on the *numbers* in it -- slot 13 lasts 264
// game-time units, slot 19 lasts 300 with its action moment at 100 -- and a
// fixture two files can edit is a fixture neither owns.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/anim.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

/// Four animations: slot 13 (264 units, state 1 to 1), slot 19 (300, action
/// moment 100, state 1 to 2), slot 18 (120, state 2 to 1: the transition back
/// in front of anything that starts from state 1) and slot 1, whose sheet is
/// `pingpong` and which declares 150 over a 200 strip. Slot 5 is deliberately
/// absent, because a missing slot is an ordinary case and not an error.
constexpr std::string_view kEntityXml = R"(<?xml version="1.0"?>
<entity name="fixture" type="vx/unit" variations="8">
  <images>
    <image idx="1" file="idle.rle" drawmode="player_color" remaping="none" rows="4" columns="8"/>
    <image idx="2" file="toattack.rle" drawmode="player_color" remaping="none" rows="3" columns="8"/>
    <image idx="3" file="fire.rle" drawmode="clouds" remaping="none" rows="4" columns="1"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="13" anim_frame="1"/>
    <state idx="2" name="attack" image_idx="1" image_row="0" anim_idx="65536" anim_frame="65536"/>
    <state idx="3" name="broken" image_idx="1" image_row="2" anim_idx="65536" anim_frame="65536"/>
  </states>
  <anims>
    <anim idx="13" name="idle" startstate="1" endstate="1" frames="6" duration="264"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="66"/>
      <frame idx="3" duration="66"/>
      <frame idx="4" duration="66"/>
      <frame idx="5" duration="66"/>
      <frame idx="6" duration="0"/>
    </anim>
    <anim idx="19" name="ToAttack" startstate="1" endstate="2" frames="5" duration="300"
          default_duration="0" action_time="100" step="0">
      <replace layer="1" image="2"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
      <frame idx="4" duration="100"/>
      <frame idx="5" duration="0"/>
    </anim>
    <anim idx="18" name="ToIdle" startstate="2" endstate="1" frames="4" duration="120"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="2"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="60"/>
      <frame idx="3" duration="60"/>
      <frame idx="4" duration="0"/>
    </anim>
    <anim idx="1" name="fire" startstate="1" endstate="1" frames="6" duration="150"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="3"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="50"/>
      <frame idx="3" duration="50"/>
      <frame idx="4" duration="50"/>
      <frame idx="5" duration="50"/>
      <frame idx="6" duration="0"/>
    </anim>
  </anims>
</entity>)";

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A world with a movement system and a hero system on it -- the first because
/// `StartAnim`'s point argument is a facing, the second because `SetState`'s
/// squad half lives in the squad table `HeroSystem` owns.
struct AnimBench {
  Result<Entity> entity = Entity::parse(bytes_of(kEntityXml));
  World world{TickConfig{100, kDefaultGameSpeed}};
  MovementSystem movement;
  HeroSystem heroes;
  script::HostRegistry registry;
  HostContext context;
  /// The scripts' clock, when a test puts a call inside a pass.
  script::Scheduler* scheduler = nullptr;

  AnimBench() {
    REQUIRE(entity.ok());
    REQUIRE(world.add_system(&movement));
    REQUIRE(world.add_system(&heroes));
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  ObjectId spawn() {
    const ObjectId id = world.spawn(NativeClass::unit, &entity.value());
    (void)world.set_position(id, Point{100, 100});
    // Alive: an object at no health is dead to the animation-end handler,
    // which puts it down (`anim_dispatch`).
    (void)world.set_health(id, 100);
    return id;
  }

  script::HostOutcome call(const char* name, std::uint16_t arity,
                           std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    CHECK(registry.entry(index).fn != nullptr);
    if (registry.entry(index).fn == nullptr) {
      return script::HostOutcome::failed("not implemented");
    }
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.scheduler = scheduler;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  }

  /// A suspending call's run: the first, or a re-entry with its word.
  script::HostOutcome rerun(const char* name, std::uint16_t arity,
                            std::vector<script::Value> args, bool first, std::int32_t& word) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.scheduler = scheduler;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    ctx.first_call = first;
    ctx.scratch = &word;
    return registry.entry(index).fn(ctx);
  }

  std::int32_t number(const char* name, std::uint16_t arity, std::vector<script::Value> args) {
    const script::HostOutcome out = call(name, arity, args);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  }

  static script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }
  static script::Value point(std::int32_t x, std::int32_t y) {
    return pack_point(Point{x, y});
  }
};

}  // namespace

/// `StartAnim(slot, point)` plays the slot the entity document declares, with
/// no adjustment.
///
/// The original's array is one lower than the document's `idx` and its body
/// decrements before it indexes, so the two cancel and the script's number is
/// the document's. That is what makes `StartAnim(9, .pos)` the death animation
/// and `StartAnim(19, tgt.pos)` the lead-in to an attack: 9 and 19 are `<anim
/// idx>` values, and 30 of the 35 shipped sites name one.
TEST(anim_start_anim_plays_the_slot_the_document_declares) {
  AnimBench b;
  const ObjectId id = b.spawn();
  CHECK(b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                                AnimBench::point(100, 100)})
            .status == script::HostStatus::ok);
  const WorldObject* slot = b.world.find(id);
  REQUIRE(slot != nullptr);
  CHECK(slot->object->anim.anim_slot == 13);
  CHECK(slot->object->anim.elapsed_ms == 0);
  CHECK(slot->animating);

  // And a slot the entity does not declare leaves the cursor where it was --
  // ordinary, not an error, and the rule `World::play_anim` already states.
  CHECK(b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(5),
                                AnimBench::point(100, 100)})
            .status == script::HostStatus::ok);
  CHECK(slot->object->anim.anim_slot == 13);

  // A receiver that resolves to nothing is a silent no-op, never a refusal.
  CHECK(b.call("StartAnim", 2, {AnimBench::obj(9999), script::Value::integer(13),
                                AnimBench::point(0, 0)})
            .status == script::HostStatus::ok);
  CHECK(b.call("StartAnim", 2, {script::Value::integer(4), script::Value::integer(13),
                                AnimBench::point(0, 0)})
            .status == script::HostStatus::ok);
}

/// The point argument is a **facing**, and the object's own position means
/// *don't turn*.
///
/// `0x005a9ce0` replaces the argument with the engine's invalid-point sentinel
/// when it equals the receiver's position, and the facing is then left alone.
/// `StartAnim(9, .pos)` -- the dominant shipped form -- is therefore "play this
/// and keep looking where you are looking", and `StartAnim(19, tgt.pos)` is a
/// turn. An implementation that faced unconditionally would snap every unit in
/// the game to face itself, which is a direction with no meaning.
TEST(anim_the_point_is_a_facing_and_the_objects_own_position_means_do_not_turn) {
  AnimBench b;
  const ObjectId id = b.spawn();
  const Point before = b.movement.state(id).facing;

  CHECK(b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                                AnimBench::point(100, 100)})
            .status == script::HostStatus::ok);
  CHECK(b.movement.state(id).facing == before);

  // A different point turns it.
  CHECK(b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                                AnimBench::point(900, 100)})
            .status == script::HostStatus::ok);
  CHECK(b.movement.state(id).facing != before);
}

/// `PlayAnim` **suspends**, starts nothing itself, and wakes when what it
/// started has ended.
///
/// `DATA\SUBAI\ANIM.VS` is `while (1) This.PlayAnim(1, This.pos);` with no
/// `Sleep` in it, and 28 shipped classes bind it. 0x005a9ba0 writes 6 into the
/// action mode, arms the animation timer for 1 ms and sleeps with no timed
/// wake; the timer's firing runs mode 6 (0x005a76c0), which starts the
/// animation; its end runs mode 6 again, which wakes the script and sets the
/// mode back to 0.
TEST(anim_play_anim_suspends_for_the_length_of_what_it_started) {
  AnimBench b;
  const ObjectId id = b.spawn();
  std::int32_t word = 0;
  const std::vector<script::Value> args{AnimBench::obj(id), script::Value::integer(13),
                                        AnimBench::point(100, 100)};
  const script::HostOutcome first = b.rerun("PlayAnim", 2, args, true, word);
  CHECK(first.status == script::HostStatus::retry);
  CHECK(first.suspend_for == 1);
  CHECK(b.world.find(id)->clock.mode == 6);
  CHECK(b.world.find(id)->clock.slot == kNoAnim);
  CHECK(!b.call("IsInState", 0, {AnimBench::obj(id)}).value.as_integer() != 0);

  // The timer fires: the animation starts, and the call waits its length.
  b.world.advance_turns(1);
  const script::HostOutcome start = b.rerun("PlayAnim", 2, args, false, word);
  CHECK(start.status == script::HostStatus::retry);
  CHECK(start.suspend_for == 264);
  CHECK(b.world.find(id)->object->anim.anim_slot == 13);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 13);

  // Its end wakes the script, and the mode is 0 again.
  b.world.advance_turns(3);
  const script::HostOutcome wake = b.rerun("PlayAnim", 2, args, false, word);
  CHECK(wake.status == script::HostStatus::ok);
  CHECK(b.world.find(id)->clock.mode == 0);

  // A mode something else rewrote runs no continuation: a `StartAnim` from
  // another script while the first millisecond is pending leaves the call
  // waiting for a wake that never comes.
  std::int32_t other = 0;
  (void)b.rerun("PlayAnim", 2, args, true, other);
  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(1),
                          AnimBench::point(100, 100)});
  b.world.advance_turns(3);
  const script::HostOutcome orphan = b.rerun("PlayAnim", 2, args, false, other);
  CHECK(orphan.status == script::HostStatus::retry);
  CHECK(orphan.suspend_for > 1000000);

  // A slot nothing declares starts nothing, arms no timer, and nothing wakes
  // the script: it sleeps rather than spins.
  std::int32_t miss_word = 0;
  const std::vector<script::Value> miss{AnimBench::obj(id), script::Value::integer(5),
                                        AnimBench::point(100, 100)};
  (void)b.rerun("PlayAnim", 2, miss, true, miss_word);
  b.world.advance_turns(1);
  const script::HostOutcome none = b.rerun("PlayAnim", 2, miss, false, miss_word);
  CHECK(none.status == script::HostStatus::retry);
  CHECK(none.suspend_for > 1000000);

  // And an unresolvable receiver does not suspend at all.
  std::int32_t dead_word = 0;
  const script::HostOutcome dead = b.rerun(
      "PlayAnim", 2, {AnimBench::obj(9999), script::Value::integer(13), AnimBench::point(0, 0)},
      true, dead_word);
  CHECK(dead.status == script::HostStatus::ok);
}

/// `PlayAnim/3` is `Flying`'s, and **its slot is one lower**.
///
/// The `Obj` body decrements before it indexes the animation array and this one
/// does not, so the same array element is `n` here and `n + 1` there. The
/// corpus proves it: `CROW_IDLE.VS` and `CROW_MOVE.VS` call `PlayAnim(0, …)`
/// and `PlayAnim(16, …)`, and **no shipped entity declares slot 0 or slot 16**
/// -- add one and they are 1 and 17, which every crow declares. Under the
/// direct reading every crow and eagle animation in the game silently does
/// nothing, and this project recorded exactly that as an ordinary miss.
TEST(anim_the_flying_play_anim_numbers_its_slots_one_lower) {
  AnimBench b;
  const ObjectId id = b.spawn();

  // 0 here is slot 1, which the fixture declares: `duration="150"`.
  const script::HostOutcome out =
      b.call("PlayAnim", 3, {AnimBench::obj(id), script::Value::integer(0),
                             AnimBench::point(100, 100), script::Value::integer(-1)});
  CHECK(out.status == script::HostStatus::suspend);
  CHECK(out.suspend_for == 150);
  CHECK(b.world.find(id)->object->anim.anim_slot == 1);

  // 18 here is slot 19, which it also declares.
  const script::HostOutcome nineteen =
      b.call("PlayAnim", 3, {AnimBench::obj(id), script::Value::integer(18),
                             AnimBench::point(100, 100), script::Value::integer(-1)});
  CHECK(nineteen.suspend_for == 300);
  CHECK(b.world.find(id)->object->anim.anim_slot == 19);

  // The Z is range-checked, `-1 .. 1024`, and out of range starts nothing --
  // not even the animation. The value itself is read and discarded: there is no
  // height in this engine's object state.
  for (const std::int32_t z : {-2, 1025, 100000}) {
    const script::HostOutcome refused =
        b.call("PlayAnim", 3, {AnimBench::obj(id), script::Value::integer(0),
                               AnimBench::point(100, 100), script::Value::integer(z)});
    CHECK(refused.status == script::HostStatus::ok);
    CHECK(b.world.find(id)->object->anim.anim_slot == 19);  // unchanged
  }
  for (const std::int32_t z : {-1, 0, 1024}) {
    const script::HostOutcome ok =
        b.call("PlayAnim", 3, {AnimBench::obj(id), script::Value::integer(0),
                               AnimBench::point(100, 100), script::Value::integer(z)});
    CHECK(ok.status == script::HostStatus::suspend);
  }
}

/// `Flying::PlayAnim` checks its slot before it writes anything, and clamps its
/// point into the map.
///
/// 0x0051bb10 range-checks Z, then the slot (negative, past the entity's last,
/// or naming nothing prints and returns without suspending), and only then
/// clamps the point into the map rectangle (0x00422b70) and writes the flight
/// state. A bad slot used to fly the bird to the point and suspend.
TEST(anim_flying_play_anim_refuses_a_bad_slot_before_it_moves_and_clamps_its_point) {
  AnimBench b;
  b.movement.set_grid(ObstructionGrid(64, 64));
  const ObjectId id = b.spawn();
  const Point before = b.world.resolve_position(id);

  for (const std::int32_t bad : {-1, 4, 200}) {
    const script::HostOutcome out =
        b.call("PlayAnim", 3, {AnimBench::obj(id), script::Value::integer(bad),
                               AnimBench::point(900, 900), script::Value::integer(-1)});
    CHECK(out.status == script::HostStatus::ok);
    CHECK(b.world.resolve_position(id) == before);
  }

  const script::HostOutcome edge =
      b.call("PlayAnim", 3, {AnimBench::obj(id), script::Value::integer(0),
                             AnimBench::point(-500, 300), script::Value::integer(-1)});
  CHECK(edge.status == script::HostStatus::suspend);
  CHECK((b.world.resolve_position(id) == Point{0, 300}));
}

/// An object off the map -- inside a holder, or taken away -- plays nothing.
///
/// `StartAnimation` (0x0053e6c0) returns before it starts anything when the
/// object's own position is the invalid point.
TEST(anim_an_object_off_the_map_plays_nothing) {
  AnimBench b;
  const ObjectId id = b.spawn();
  (void)b.world.set_position(id, kHeldPosition);
  CHECK(b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(19),
                                AnimBench::point(500, 500)})
            .status == script::HostStatus::ok);
  CHECK(b.world.find(id)->object->anim.anim_slot != 19);
  CHECK(!b.world.find(id)->animating);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 0);
}

/// `TimeToAnimFinish` counts down, and answers **0** in every degenerate case.
///
/// `Sleep(.TimeToAnimFinish())` is 20 of its 29 shipped sites verbatim. The
/// original answers zero when nothing is playing, when the animation has
/// already finished, and when the receiver does not resolve -- so the failure
/// mode is a script that runs a turn early, never one that hangs. A wrong large
/// number here would be far worse than a wrong zero, and that asymmetry is the
/// reason this has a test of its own.
TEST(anim_time_to_anim_finish_counts_down_and_bottoms_out_at_zero) {
  AnimBench b;
  const ObjectId id = b.spawn();

  // Nothing playing yet.
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 0);

  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 264);
  b.world.advance_turns(1);  // 100 units
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 164);
  b.world.advance_turns(1);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 64);

  // Past the end it is zero, not negative, and it stays zero.
  b.world.advance_turns(4);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 0);

  // And a receiver that resolves to nothing.
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(9999)}) == 0);
  CHECK(b.number("TimeToAnimFinish", 0, {script::Value::integer(2)}) == 0);
}

/// An animation a script starts mid-turn starts on the script's millisecond.
///
/// A script runs at its own wake time inside a pass while the world already
/// stands at the turn's end. The original starts the animation at the
/// script's instant, so here the cursor is brought forward by the part of the
/// turn it has lived through, and the two countdowns are read at the script's
/// instant -- so `Sleep(.TimeToAnimFinish())` wakes the script when the
/// animation ends, not up to a turn early. A bird that flew its next leg
/// before the last was drawn to its end is how this showed.
TEST(anim_a_script_mid_turn_starts_an_animation_on_its_own_millisecond) {
  AnimBench b;
  script::Scheduler clock;
  b.scheduler = &clock;
  const ObjectId id = b.spawn();
  b.world.advance_turns(8);  // the world at 800
  REQUIRE(b.world.time() == 800);
  clock.set_now(700);  // a script woke at 700, inside the turn that ended at 800

  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(19),
                          AnimBench::point(100, 100)});
  // 100 of its 300 already played by the turn's end.
  CHECK(b.world.find(id)->object->anim.elapsed_ms == 100);
  // And counted from the script's millisecond, the whole of it is left.
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 300);
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(id)}) == 100);

  // At the turn's end itself nothing is brought forward.
  // The lead-in is stopped where it stands and leaves the unit in its end
  // state, 2, so the fidget has the 2-to-1 edge (120) in front.
  clock.set_now(800);
  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.world.find(id)->object->anim.anim_slot == 18);
  CHECK(b.world.find(id)->object->anim.elapsed_ms == 0);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 120 + 264);
}

/// `TimeToActionMoment` counts down to the entity's `action_time`, not to the
/// end.
///
/// 26 sites. `0x005a9e70` is `TimeToAnimFinish` against a different cached
/// time: the start plus `action_time` scaled by the playback duration over the
/// nominal one, which with no duration override is `action_time` itself.
TEST(anim_time_to_action_moment_counts_down_to_the_declared_moment) {
  AnimBench b;
  const ObjectId id = b.spawn();
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(id)}) == 0);

  // Slot 19 declares `action_time="100"` against a 300-unit length.
  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(19),
                          AnimBench::point(100, 100)});
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(id)}) == 100);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 300);
  b.world.advance_turns(1);
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(id)}) == 0);
  // The animation is not over, though: the two are different questions.
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 200);

  // An animation whose moment is at zero answers zero from the start, on a
  // unit resting where it starts; after the lead-in, the moment is past the
  // 2-to-1 edge in front of it (0x0053cf30 counts from the main start).
  const ObjectId fresh = b.spawn();
  b.call("StartAnim", 2, {AnimBench::obj(fresh), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(fresh)}) == 0);
  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(id)}) == 120);
}

/// `GetAnim` is the slot, with **0** for nothing playing; `GetAnimTime` and
/// `GetAnimDuration` are the same length under two different numberings.
///
/// `EAGLE_MOVE.VS:86` is the whole of `GetAnimDuration`'s corpus:
/// `speed = .speed * .GetAnimDuration(4) / .GetAnimDuration(0);` -- 4 and 0,
/// which are slots 5 and 1, the attack over the walk. A ratio that means
/// something under one numbering and divides by a missing animation under the
/// other.
TEST(anim_get_anim_and_the_two_duration_readers) {
  AnimBench b;
  const ObjectId id = b.spawn();
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 0);

  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(19),
                          AnimBench::point(100, 100)});
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 19);

  // `GetAnimTime` takes the document's number.
  CHECK(b.number("GetAnimTime", 1, {AnimBench::obj(id), script::Value::integer(13)}) == 264);
  CHECK(b.number("GetAnimTime", 1, {AnimBench::obj(id), script::Value::integer(19)}) == 300);
  // A slot nothing declares is zero, which is what the original's null
  // descriptor check gives.
  CHECK(b.number("GetAnimTime", 1, {AnimBench::obj(id), script::Value::integer(5)}) == 0);

  // `GetAnimDuration` takes one less for the same animation.
  CHECK(b.number("GetAnimDuration", 1, {AnimBench::obj(id), script::Value::integer(12)}) == 264);
  CHECK(b.number("GetAnimDuration", 1, {AnimBench::obj(id), script::Value::integer(18)}) == 300);
  CHECK(b.number("GetAnimDuration", 1, {AnimBench::obj(id), script::Value::integer(0)}) == 150);
  // Which is to say the two disagree about every number.
  CHECK(b.number("GetAnimTime", 1, {AnimBench::obj(id), script::Value::integer(12)}) == 0);

  // The original dereferences null here rather than checking; this answers 0.
  CHECK(b.number("GetAnimDuration", 1, {AnimBench::obj(9999), script::Value::integer(0)}) == 0);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(9999)}) == 0);
}

/// Every length a script sees is the entity's declared `duration`, not the
/// frame strip's sum.
///
/// The loader stores `<anim duration>` at `[anim+0x4c]` and the whole family
/// reads that one field: `GetAnimTime` (0x005a9c50), `GetAnimDuration`
/// (0x0051c180), `Flying::PlayAnim`'s suspension (0x0051bcba), and the end
/// `TimeToAnimFinish` counts down to, which `StartAnimation` (0x0053e6c0)
/// sets at the start plus it. The two disagree in 127 shipped animations --
/// the barrack horses' idle is 1000 over a 1200 strip. Slot 1 here declares
/// 150 over four frames of 50.
TEST(anim_scripts_see_the_declared_duration_not_the_frame_strip) {
  AnimBench b;
  const ObjectId id = b.spawn();
  CHECK(b.number("GetAnimTime", 1, {AnimBench::obj(id), script::Value::integer(1)}) == 150);
  CHECK(b.number("GetAnimDuration", 1, {AnimBench::obj(id), script::Value::integer(0)}) == 150);

  std::int32_t word = 0;
  const std::vector<script::Value> args{AnimBench::obj(id), script::Value::integer(1),
                                        AnimBench::point(100, 100)};
  (void)b.rerun("PlayAnim", 2, args, true, word);
  b.world.advance_turns(1);
  const script::HostOutcome play = b.rerun("PlayAnim", 2, args, false, word);
  CHECK(play.status == script::HostStatus::retry);
  CHECK(play.suspend_for == 150);
  b.world.advance_turns(2);
  CHECK(b.rerun("PlayAnim", 2, args, false, word).status == script::HostStatus::ok);

  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(1),
                          AnimBench::point(100, 100)});
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 150);
  b.world.advance_turns(1);
  // The strip is still running (100 of its 200), the declared length nearly over.
  CHECK(b.world.find(id)->animating);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 50);
  b.world.advance_turns(1);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 0);
}

/// `SetState` is two entry points wearing one name, and the squad half is the
/// majority.
///
/// Twelve of the sixteen shipped sites are `sq.SetState(SS_IDLE)`; four are
/// `.SetState(2)` on a hero's grave, stepping its decay through the entity's
/// later states. A body that only did the object half would answer three
/// quarters of the corpus by refusing.
TEST(anim_set_state_dispatches_on_the_handle_type) {
  AnimBench b;
  const ObjectId id = b.spawn();

  // The object half. Entering a state stops whatever was playing -- the
  // original sets the playing slot to -1 and recomputes.
  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(19),
                          AnimBench::point(100, 100)});
  REQUIRE(b.world.find(id)->object->anim.anim_slot == 19);
  CHECK(b.call("SetState", 1, {AnimBench::obj(id), script::Value::integer(3)}).status ==
        script::HostStatus::ok);
  CHECK(b.world.find(id)->object->anim.state_idx == 3);
  // State 3 names no animation, so nothing is playing now.
  CHECK(b.world.find(id)->object->anim.anim_slot == kNoAnim);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 0);

  // A state that names one loops it.
  CHECK(b.call("SetState", 1, {AnimBench::obj(id), script::Value::integer(1)}).status ==
        script::HostStatus::ok);
  CHECK(b.world.find(id)->object->anim.state_idx == 1);
  CHECK(b.world.find(id)->object->anim.anim_slot == 13);

  // Resting in it already, `CObj::SetState` returns before it writes
  // anything (0x0053e860): the pose's loop runs on.
  b.world.advance_turns(1);
  const std::int32_t looped = b.world.find(id)->object->anim.elapsed_ms;
  REQUIRE(looped != 0);
  b.call("SetState", 1, {AnimBench::obj(id), script::Value::integer(1)});
  CHECK(b.world.find(id)->object->anim.elapsed_ms == looped);

  // The squad half, on a squad handle rather than an object one.
  const ObjectId leader = b.spawn();
  const SquadKey key = b.heroes.squads().create(1, leader);
  REQUIRE(key.valid());
  b.world.advance_turns(3);
  CHECK(b.call("SetState", 1, {pack_squad(key), script::Value::integer(7)}).status ==
        script::HostStatus::ok);
  const Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  CHECK(squad->state == 7);
  // `StateTime` is *when* it was set, which its three corpus sites compare
  // against `GetTime()`, and not how long it has been held.
  CHECK(squad->state_time == b.world.time());

  // Setting the state it already holds **restarts the clock**: `Squad::SetState`
  // (0x004211c0) writes the state and then the time with no compare between
  // them. That is the opposite of the object half, where `CObj::SetState`
  // returns before it writes, and this test used to assert the object half's
  // rule of the squad; `SQUADMONITOR.VS` times its idling from the stamp.
  const GameTime stamped = squad->state_time;
  b.world.advance_turns(2);
  b.call("SetState", 1, {pack_squad(key), script::Value::integer(7)});
  CHECK(squad->state_time == b.world.time());
  CHECK(squad->state_time != stamped);

  // A squad handle naming no squad is a no-op, not a refusal.
  CHECK(b.call("SetState", 1, {script::Value::object(kTypeSquad, 0x7FFF),
                               script::Value::integer(1)})
            .status == script::HostStatus::ok);
}

/// A squad's `StateTime` is the millisecond the script set it on, which is what
/// `GetTime()` answers -- not the turn's end the world already stands at.
///
/// 0x004211c0 stamps the game's running time (`[game+0x1258]`), the clock the
/// scripts' wheel moves one millisecond at a time; `SQUADMONITOR.VS` and
/// `EVALRECRUIT.VS` subtract the stamp from `GetTime()`.
TEST(anim_squad_set_state_stamps_the_scripts_millisecond) {
  AnimBench b;
  script::Scheduler clock;
  b.scheduler = &clock;
  const ObjectId leader = b.spawn();
  const SquadKey key = b.heroes.squads().create(1, leader);
  REQUIRE(key.valid());
  b.world.advance_turns(8);
  REQUIRE(b.world.time() == 800);
  clock.set_now(730);
  b.call("SetState", 1, {pack_squad(key), script::Value::integer(7)});
  const Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  CHECK(squad->state == 7);
  CHECK(squad->state_time == 730);
}

/// `SetState(state, set, clear)` on a squad (0x00421380): null-checked, the
/// state written, the sixteen-bit flags word at `+0x30` set and then
/// cleared, and the game's running time stamped. Registered by the binary on
/// the squad receiver; the corpus's two sites are commented out, so it is
/// bound for what a script could call.
TEST(anim_squad_set_state_with_flags_sets_then_clears) {
  AnimBench b;
  script::Scheduler clock;
  b.scheduler = &clock;
  const ObjectId leader = b.spawn();
  const SquadKey key = b.heroes.squads().create(1, leader);
  REQUIRE(key.valid());
  Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  squad->flags = 0x0101;
  clock.set_now(450);
  CHECK(b.call("SetState", 3, {pack_squad(key), script::Value::integer(3),
                               script::Value::integer(0x0030), script::Value::integer(0x0110)})
            .status == script::HostStatus::ok);
  CHECK(squad->state == 3);
  // (0x0101 | 0x0030) & ~0x0110 = 0x0021; a set bit the clear names goes.
  CHECK(squad->flags == 0x0021);
  CHECK(squad->state_time == 450);
  // Only sixteen bits are kept.
  CHECK(b.call("SetState", 3, {pack_squad(key), script::Value::integer(3),
                               script::Value::integer(0x10004), script::Value::integer(0)})
            .status == script::HostStatus::ok);
  CHECK(squad->flags == 0x0025);
  // A squad naming nothing writes nothing.
  CHECK(b.call("SetState", 3, {pack_squad(SquadKey{}), script::Value::integer(9),
                               script::Value::integer(1), script::Value::integer(0)})
            .status == script::HostStatus::ok);
}

/// A state's own loop is the pose the object rests in, not a playing animation,
/// and neither is one that has run to its end.
///
/// The original's playing slot `[obj+0x50]` is set by `StartAnimation`
/// (0x0053e6c0) and cleared to -1 by entering a state (0x0053e860) and by the
/// animation's end (0x0053f6a0); `GetAnim` (0x005a9fd0) answers 0 for -1, and
/// `TimeToAnimFinish` (0x005a9f20) and `TimeToActionMoment` answer 0.
TEST(anim_a_states_loop_and_a_finished_animation_are_not_playing) {
  AnimBench b;
  const ObjectId id = b.spawn();
  REQUIRE(b.world.enter_state(id, 1));
  REQUIRE(b.world.find(id)->object->anim.anim_slot == 13);
  REQUIRE(b.world.find(id)->animating);
  b.world.advance_turns(1);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 0);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 0);

  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 13);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 264);
  b.world.advance_turns(3);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 0);
}

/// `StartDelayedAnim(slot, point, delay)` -- the delay is `StartAnimation`'s
/// start offset.
///
/// `CATAPULT_IDLE.VS:32` is `.StartDelayedAnim(1, .pos, -1);`, and `-1` is
/// the original's own special case (0x0053e7f7): the animation is applied at
/// once, its start a millisecond in the past, and **no timer is armed**. So it
/// never ends: `IsInState` is true at once, `GetAnim` keeps answering it, and
/// `TimeToAnimFinish` counts down to its start less one plus its length and
/// stays at 0. A positive delay arms the timer for the start: the object rests
/// where it is until then, with the slot already written.
TEST(anim_start_delayed_anim_is_start_anim_at_the_one_delay_that_ships) {
  AnimBench b;
  const ObjectId id = b.spawn();
  CHECK(b.call("StartDelayedAnim", 3,
               {AnimBench::obj(id), script::Value::integer(13), AnimBench::point(100, 100),
                script::Value::integer(-1)})
            .status == script::HostStatus::ok);
  CHECK(b.world.find(id)->object->anim.anim_slot == 13);
  CHECK(b.world.find(id)->object->anim.elapsed_ms == 1);
  CHECK(b.call("IsInState", 0, {AnimBench::obj(id)}).value.as_integer() != 0);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 263);
  b.world.advance_turns(5);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 13);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 0);

  // A positive delay: the slot at once, the timer pending, the pose kept.
  const ObjectId later = b.spawn();
  REQUIRE(b.world.enter_state(later, 3));
  CHECK(b.call("StartDelayedAnim", 3,
               {AnimBench::obj(later), script::Value::integer(13), AnimBench::point(100, 100),
                script::Value::integer(150)})
            .status == script::HostStatus::ok);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(later)}) == 13);
  CHECK(!b.call("IsInState", 0, {AnimBench::obj(later)}).value.as_integer() != 0);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(later)}) == 150 + 264);
  b.world.advance_turns(1);
  CHECK(b.world.find(later)->object->anim.anim_slot == kNoAnim);
  b.world.advance_turns(1);
  CHECK(b.world.find(later)->object->anim.anim_slot == 13);
}

/// **A transition plays in front.** `StartAnimation` (0x0053e6c0) looks for
/// the first animation leading from the state the object rests in to the
/// state the new one starts from (0x005a8170) and plays it first; the end and
/// the action moment the scripts wait on are counted after it (0x0053cf30).
/// After the 1-to-2 lead-in the unit rests in state 2, so the 1-to-1 fidget
/// is the 2-to-1 edge (120) and then itself (264).
TEST(anim_a_transition_plays_in_front_and_the_scripts_wait_for_it) {
  AnimBench b;
  const ObjectId id = b.spawn();
  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(19),
                          AnimBench::point(100, 100)});
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 300);
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(id)}) == 100);
  b.world.advance_turns(3);
  CHECK(b.world.anim_rest_state_at(*b.world.find(id), b.world.time()) == 2);
  CHECK(b.call("IsInState", 0, {AnimBench::obj(id)}).value.as_integer() != 0);

  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.world.find(id)->clock.transition == 18);
  CHECK(b.number("TimeToAnimFinish", 0, {AnimBench::obj(id)}) == 120 + 264);
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(id)}) == 120);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 13);
  // The view plays the edge first, then the fidget.
  CHECK(b.world.find(id)->object->anim.anim_slot == 18);
  b.world.advance_turns(2);
  CHECK(b.world.find(id)->object->anim.anim_slot == 13);
  CHECK(b.world.find(id)->object->anim.elapsed_ms == 80);
  b.world.advance_turns(2);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 0);
  CHECK(b.world.anim_rest_state_at(*b.world.find(id), b.world.time()) == 1);

  // `SetState` to where it rests is nothing; elsewhere it rests there at once,
  // the slot dropped.
  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  b.call("SetState", 1, {AnimBench::obj(id), script::Value::integer(3)});
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(id)}) == 0);
  CHECK(b.world.find(id)->object->anim.state_idx == 3);
}

/// **`StartAnim` hands off to a dead object's handler first.** With no timer
/// pending, 0x005a9ce0 sets the action mode to 0 and runs `vtbl+0x54`; for an
/// object at no health that is the death sequence (0x005b0f70): with no death
/// animation, straight to its destroy (`vtbl+0x94`). A living one is only
/// given mode 0.
TEST(anim_start_anim_on_a_dead_object_runs_its_end_first) {
  AnimBench b;
  CommandSystem commands;
  REQUIRE(b.world.add_system(&commands));
  const ObjectId living = b.spawn();
  b.world.set_anim_mode(living, 4);
  b.call("StartAnim", 2, {AnimBench::obj(living), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.world.find(living) != nullptr);
  CHECK(b.world.find(living)->clock.mode == 0);

  const ObjectId dead = b.spawn();
  (void)b.world.set_health(dead, 0);
  b.call("StartAnim", 2, {AnimBench::obj(dead), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.world.find(dead) == nullptr);
}

// --------------------------------------------------------------------------
// Catapult::SetBuildFrame: the stage a siege engine under construction shows
// --------------------------------------------------------------------------

namespace {

/// A siege engine's art: slot 1 is its construction, nine declared frames with
/// the entry and exit markers around seven rows, as every shipped engine's
/// `build` animation is laid out; slot 5 a second animation, with no markers.
constexpr std::string_view kEngineXml = R"(<?xml version="1.0"?>
<entity name="engine" type="vx/unit" variations="1">
  <images>
    <image idx="1" file="stages.rle" drawmode="index" remaping="none" rows="7" columns="1"/>
    <image idx="2" file="shot.rle" drawmode="index" remaping="none" rows="3" columns="1"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="0" anim_idx="65536" anim_frame="65536"/>
  </states>
  <anims>
    <anim idx="1" name="build" startstate="1" endstate="1" frames="9" duration="350"
          default_duration="50" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="50"/>
      <frame idx="3" duration="50"/>
      <frame idx="4" duration="50"/>
      <frame idx="5" duration="50"/>
      <frame idx="6" duration="50"/>
      <frame idx="7" duration="50"/>
      <frame idx="8" duration="50"/>
      <frame idx="9" duration="0"/>
    </anim>
    <anim idx="5" name="shot" startstate="1" endstate="1" frames="3" duration="150"
          default_duration="50" action_time="0" step="0">
      <replace layer="1" image="2"/>
      <frame idx="1" duration="50"/>
      <frame idx="2" duration="50"/>
      <frame idx="3" duration="50"/>
    </anim>
  </anims>
</entity>)";

struct EngineBench {
  Result<Entity> entity = Entity::parse(bytes_of(kEngineXml));
  ClassGraph graph;
  World world{TickConfig{100, kDefaultGameSpeed}};
  script::HostRegistry registry;
  HostContext context;

  EngineBench() {
    REQUIRE(entity.ok());
    graph.add(bytes_of(R"(<class id="Engine" cpp_class="CVXCatapult"><properties maxhealth="1000"/></class>)"),
              "engine.sc.xml");
    graph.add(bytes_of(R"(<class id="Flimsy" cpp_class="CVXCatapult"><properties maxhealth="1"/></class>)"),
              "flimsy.sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  ObjectId spawn(const char* cls) {
    const ObjectId id = world.spawn(NativeClass::catapult, &entity.value(), graph.find(cls));
    (void)world.set_position(id, Point{100, 100});
    // Standing: a siege engine at no health is dead to the animation-end
    // handler `StartDelayedAnim` runs first, which puts it down.
    (void)world.set_health(id, 1);
    return id;
  }

  script::HostOutcome call(const char* name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(script::CallKind::member, name,
                                              static_cast<std::uint16_t>(args.size() - 1));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  }

  void set_frame(ObjectId id, std::int32_t left) {
    CHECK(call("SetBuildFrame", {AnimBench::obj(id), script::Value::integer(left)}).status ==
          script::HostStatus::ok);
  }

  /// The row the view draws for `id`'s build frame.
  std::uint32_t row(ObjectId id) {
    const WorldObject* slot = world.find(id);
    const EntityAnim* playing = slot->object->entity->anim(slot->object->anim.anim_slot);
    CHECK(playing != nullptr);
    if (playing == nullptr) return 99;
    return build_frame_row(*playing, slot->timeline, slot->build_frame);
  }
};

}  // namespace

/// Playtest #19: an engine placed with the build command was drawn finished
/// while its builders were still walking to it. `CATAPULT_IDLE.VS` starts the
/// build animation and then calls `SetBuildFrame(healthleft)` every cycle;
/// with that call dropped, the animation ran its course and held its last
/// row. 0x004e2ed0: `(frames - 2) * (max - n - 1) / (max - 1) + 1`, frozen.
TEST(set_build_frame_freezes_an_engine_on_the_stage_its_health_has_reached) {
  EngineBench b;
  const ObjectId id = b.spawn("Engine");
  CHECK(b.world.find(id)->build_frame == -1);
  CHECK(b.call("StartDelayedAnim", {AnimBench::obj(id), script::Value::integer(1),
                                    AnimBench::point(100, 100), script::Value::integer(-1)})
            .status == script::HostStatus::ok);
  REQUIRE(b.world.find(id)->object->anim.anim_slot == 1);
  const std::uint64_t before = b.world.state_hash();

  // Nothing built yet: `healthleft` is `maxhealth - 1`, frame 1, the first row.
  b.set_frame(id, 999);
  CHECK(b.world.find(id)->build_frame == 1);
  CHECK(b.row(id) == 0);
  // Presentation: the hash does not see it.
  CHECK(b.world.state_hash() == before);

  // The clock runs on and the animation finishes, and the engine still shows
  // its first stage -- what a builder walking up to it should see.
  for (int i = 0; i < 10; ++i) b.world.advance(100);
  CHECK(!b.world.find(id)->animating);
  CHECK(b.row(id) == 0);

  // Half built: 7 * 499 / 999 + 1 = 4, the fourth row.
  b.set_frame(id, 500);
  CHECK(b.world.find(id)->build_frame == 4);
  CHECK(b.row(id) == 3);
  // One point left: 7 * 998 / 999 + 1 = 7, the last row. The script never
  // asks for 0; it calls `SetBuilt` then.
  b.set_frame(id, 1);
  CHECK(b.world.find(id)->build_frame == 7);
  CHECK(b.row(id) == 6);

  // `SetBuilt` writes -1 back (0x004e2e20).
  CHECK(b.call("SetBuilt", {AnimBench::obj(id)}).status == script::HostStatus::ok);
  CHECK(b.world.find(id)->build_frame == -1);
  CHECK(b.world.find(id)->state.flags.built);
}

/// The receivers it stores nothing for, all `ok`: no animation playing, the
/// original's division by zero (a maximum of 1), and a handle naming nothing.
TEST(set_build_frame_stores_nothing_without_an_animation_or_a_maximum) {
  EngineBench b;
  const ObjectId idle = b.spawn("Engine");
  b.set_frame(idle, 500);
  CHECK(b.world.find(idle)->build_frame == -1);

  const ObjectId flimsy = b.spawn("Flimsy");
  CHECK(b.world.play_anim(flimsy, 1, AnimRepeat::hold));
  b.set_frame(flimsy, 0);
  CHECK(b.world.find(flimsy)->build_frame == -1);

  CHECK(b.call("SetBuildFrame", {script::Value::object(kTypeObj, 9999), script::Value::integer(5)})
            .status == script::HostStatus::ok);
}

/// The row a frame draws: past the entry marker, clamped to the sheet. An
/// animation whose strip has no entry marker counts from row 0.
TEST(build_frame_row_skips_the_entry_marker_and_clamps_to_the_sheet) {
  EngineBench b;
  const EntityAnim* build = b.entity.value().anim(1);
  const EntityAnim* shot = b.entity.value().anim(5);
  REQUIRE(build != nullptr);
  REQUIRE(shot != nullptr);
  const AnimTimeline build_line = b.entity.value().timeline(*build);
  const AnimTimeline shot_line = b.entity.value().timeline(*shot);
  CHECK(build_frame_row(*build, build_line, 0) == 0);
  CHECK(build_frame_row(*build, build_line, 1) == 0);
  CHECK(build_frame_row(*build, build_line, 2) == 1);
  CHECK(build_frame_row(*build, build_line, 7) == 6);
  CHECK(build_frame_row(*build, build_line, 8) == 6);
  CHECK(build_frame_row(*build, build_line, 40) == 6);
  CHECK(build_frame_row(*build, build_line, -3) == 0);
  CHECK(build_frame_row(*shot, shot_line, 0) == 0);
  CHECK(build_frame_row(*shot, shot_line, 1) == 1);
  CHECK(build_frame_row(*shot, shot_line, 2) == 2);
  CHECK(build_frame_row(*shot, shot_line, 3) == 2);
  CHECK(build_frame_row(*build, AnimTimeline(), 4) == 0);
}

/// **The idle activity spends draws and plays its fidget** (0x005d2ca0 behind
/// `Unit::Idle`, 0x005d6030). This fixture declares the idle base (`idx` 13,
/// 264 ms) and no variant after it, so there is no variant draw; then for
/// `t` = 0, 100, ... up to the budget less 264 the activity draws
/// `rand(1, 15)` from the world's generator until one is 1, starts the fidget
/// `t` ms on and waits it out; with no 1 it holds its pose for the whole
/// budget. Every step after is a re-entry, and the call is done when the
/// budget is spent. A unit walking on another slot draws nothing.
TEST(idle_draws_for_a_fidget_every_hundred_ms_and_spends_its_whole_budget) {
  AnimBench b;
  CommandSystem commands;
  REQUIRE(b.world.add_system(&commands));
  const ObjectId unit = b.spawn();
  b.world.rng().seed(12345);
  std::int32_t word = 0;
  std::vector<script::Value> args{script::Value::object(kTypeObj, unit),
                                  script::Value::integer(1000)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &b.context;
  ctx.name = "Idle";
  ctx.kind = script::CallKind::member;
  ctx.scratch = &word;
  const std::uint32_t index = b.registry.find(script::CallKind::member, "Idle", 1);
  REQUIRE(index != script::kUnresolvedHost);
  const script::HostFn idle = b.registry.entry(index).fn;

  // What the first step must do, computed beside it.
  Rng probe(12345);
  std::int64_t first_t = -1;
  for (std::int64_t t = 0; t <= 1000 - 264; t += 100) {
    if (probe.between(1, 15) == 1) {
      first_t = t;
      break;
    }
  }
  const script::HostOutcome first = idle(ctx);
  CHECK(first.status == script::HostStatus::retry);
  CHECK(b.world.rng().state() == probe.state());
  if (first_t < 0) {
    CHECK(first.suspend_for == 1000);
  } else {
    // The fidget starts `t` on: the wait is its start offset plus its length.
    CHECK(first.suspend_for == first_t + 264);
  }

  // Run the steps out, the game's time moving as the waits do: they add up to
  // the budget, and the call ends.
  std::int64_t waited = first.suspend_for;
  std::int64_t last = first.suspend_for;
  ctx.first_call = false;
  for (int i = 0; i < 50; ++i) {
    b.world.advance(static_cast<std::int32_t>(last));
    const script::HostOutcome next = idle(ctx);
    if (next.status != script::HostStatus::retry) {
      CHECK(next.status == script::HostStatus::ok);
      break;
    }
    waited += next.suspend_for;
    last = next.suspend_for;
  }
  CHECK(waited == 1000);

  // On another walk slot -- a villager with its bundle -- no draw at all.
  const ObjectId carrier = b.spawn();
  b.movement.state(carrier).walk_anim = 18;
  const std::uint32_t before = b.world.rng().state();
  std::vector<script::Value> other{script::Value::object(kTypeObj, carrier),
                                   script::Value::integer(1000)};
  script::CallContext cctx = ctx;
  cctx.arguments = other;
  cctx.first_call = true;
  std::int32_t cword = 0;
  cctx.scratch = &cword;
  const script::HostOutcome held = idle(cctx);
  CHECK(held.status == script::HostStatus::retry);
  CHECK(held.suspend_for == 1000);
  CHECK(b.world.rng().state() == before);
}

namespace {

/// One `Idle(ms)` call on `unit` through the registry, first run or re-entry.
struct IdleCall {
  AnimBench& b;
  std::vector<script::Value> args;
  std::int32_t word = 0;
  script::CallContext ctx;
  script::HostFn fn = nullptr;

  IdleCall(AnimBench& bench, ObjectId unit, std::int32_t ms) : b(bench) {
    args = {script::Value::object(kTypeObj, unit), script::Value::integer(ms)};
    ctx.arguments = args;
    ctx.user = &b.context;
    ctx.name = "Idle";
    ctx.kind = script::CallKind::member;
    ctx.scratch = &word;
    const std::uint32_t index = b.registry.find(script::CallKind::member, "Idle", 1);
    REQUIRE(index != script::kUnresolvedHost);
    fn = b.registry.entry(index).fn;
  }

  script::HostOutcome run() {
    const script::HostOutcome out = fn(ctx);
    ctx.first_call = false;
    return out;
  }
};

/// The first `t` at which `rand(1, 15)` says 1 from `seed`, or -1 within
/// `budget` less the 264 ms fidget.
std::int64_t first_fidget(std::uint32_t seed, std::int64_t budget) {
  Rng probe(seed);
  for (std::int64_t t = 0; t <= budget - 264; t += 100) {
    if (probe.between(1, 15) == 1) return t;
  }
  return -1;
}

}  // namespace

/// **`ForceIdle`'s flag is kept on the unit and spent by the next draw**
/// (0x005d88b0 writes `[unit+0x1b8]`; 0x005d2ca0 reads it). With it up, the
/// first draw plays the fidget at once whatever it says -- and is still
/// drawn -- and the flag drops. A seed whose first draw is not 1 tells the
/// two apart: unforced, that idle waits past `t = 0`.
TEST(force_idle_keeps_its_flag_until_a_draw_spends_it) {
  // A seed whose first `rand(1, 15)` is not 1.
  std::uint32_t seed = 1;
  while (first_fidget(seed, 1000) == 0) ++seed;

  AnimBench b;
  CommandSystem commands;
  REQUIRE(b.world.add_system(&commands));
  const ObjectId unit = b.spawn();
  CHECK(b.call("ForceIdle", 0, {AnimBench::obj(unit)}).status == script::HostStatus::ok);
  CHECK(b.world.find(unit)->state.flags.force_idle);

  b.world.rng().seed(seed);
  Rng probe(seed);
  (void)probe.between(1, 15);
  IdleCall idle(b, unit, 1000);
  const script::HostOutcome first = idle.run();
  CHECK(first.status == script::HostStatus::retry);
  // The fidget at `t = 0`: wait its length; one draw spent; the flag down.
  CHECK(first.suspend_for == 264);
  CHECK(b.world.rng().state() == probe.state());
  CHECK(!b.world.find(unit)->state.flags.force_idle);
  CHECK(b.world.find(unit)->object->anim.anim_slot == 13);

  // A budget shorter than the fidget draws nothing, and the flag waits.
  const ObjectId other = b.spawn();
  CHECK(b.call("ForceIdle", 0, {AnimBench::obj(other)}).status == script::HostStatus::ok);
  const std::uint32_t before = b.world.rng().state();
  IdleCall brief(b, other, 200);
  const script::HostOutcome held = brief.run();
  CHECK(held.status == script::HostStatus::retry);
  CHECK(held.suspend_for == 200);
  CHECK(b.world.rng().state() == before);
  CHECK(b.world.find(other)->state.flags.force_idle);
}

/// **The first step waits out a one-shot animation already in flight.** The
/// activity setter (0x005a7680) runs the activity at once only when the
/// unit's animation timer is not pending (0x0053cfc0); otherwise the
/// animation's end runs it (0x0053f6a0). So an `Idle` called 100 ms into a
/// 300 ms attack lead-in waits 200 ms with no draw, then takes its first step
/// with the whole budget. The state's own loop is a pose, not in flight.
TEST(idle_waits_out_an_animation_already_in_flight_before_its_first_step) {
  AnimBench b;
  CommandSystem commands;
  REQUIRE(b.world.add_system(&commands));
  const ObjectId unit = b.spawn();
  REQUIRE(b.world.start_animation(unit, 19, b.world.time()) == 300);
  b.world.advance(100);
  REQUIRE(b.world.find(unit)->animating);

  b.world.rng().seed(777);
  const std::uint32_t before = b.world.rng().state();
  IdleCall idle(b, unit, 1000);
  const script::HostOutcome first = idle.run();
  CHECK(first.status == script::HostStatus::retry);
  CHECK(first.suspend_for == 200);
  CHECK(b.world.rng().state() == before);

  // The re-entry is the first step, over the whole 1,000. The lead-in left
  // the unit in state 2, so a fidget has the 2-to-1 edge (120) in front.
  b.world.advance(200);
  const std::int64_t t = first_fidget(777, 1000);
  const script::HostOutcome step = idle.run();
  CHECK(step.status == script::HostStatus::retry);
  if (t < 0) {
    CHECK(step.suspend_for == 1000);
  } else {
    CHECK(step.suspend_for == t + 120 + 264);
  }
  CHECK(b.world.rng().state() != before);

  // A unit resting in its state's loop is not in flight: the step is at once.
  const ObjectId resting = b.spawn();
  REQUIRE(b.world.enter_state(resting, 1));
  b.world.rng().seed(777);
  IdleCall now(b, resting, 1000);
  const script::HostOutcome at_once = now.run();
  CHECK(b.world.rng().state() != Rng(777).state());
  CHECK(at_once.status == script::HostStatus::retry);
}

/// `Ship::Idle` raises the same flag (0x005c6ea0 writes `[ship+0x1b8] = 1`),
/// so a ship's idle plays its fidget at its first draw; and the flag is the
/// unit's, so a draw spends it whoever raised it.
TEST(ship_idle_raises_the_force_flag_its_first_draw_spends) {
  AnimBench b;
  CommandSystem commands;
  REQUIRE(b.world.add_system(&commands));
  const ObjectId ship = b.spawn();
  b.world.rng().seed(4);
  const std::uint32_t index = b.registry.find(script::CallKind::member, "Idle", 0);
  REQUIRE(index != script::kUnresolvedHost);
  std::vector<script::Value> args{AnimBench::obj(ship)};
  std::int32_t word = 0;
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &b.context;
  ctx.name = "Idle";
  ctx.kind = script::CallKind::member;
  ctx.scratch = &word;
  const script::HostOutcome first = b.registry.entry(index).fn(ctx);
  // The budget is the idle animation's own 264 ms, and the fidget fills it.
  CHECK(first.status == script::HostStatus::retry);
  CHECK(first.suspend_for == 264);
  CHECK(!b.world.find(ship)->state.flags.force_idle);
}

/// **An idle whose mode something else rewrote never wakes its script.** The
/// activity runs only from the unit's end handler and only while the action
/// mode is still 4 (0x005d5b60); `StartAnim` sets it to 0 (0x005a9ce0), so
/// the idle's next firing runs no step, and nothing sends the wake.
TEST(idle_whose_mode_was_rewritten_never_wakes) {
  AnimBench b;
  CommandSystem commands;
  REQUIRE(b.world.add_system(&commands));
  const ObjectId unit = b.spawn();
  b.world.rng().seed(1);
  IdleCall idle(b, unit, 1000);
  const script::HostOutcome first = idle.run();
  REQUIRE(first.status == script::HostStatus::retry);
  CHECK(b.world.find(unit)->clock.mode == 4);
  b.call("StartAnim", 2, {AnimBench::obj(unit), script::Value::integer(1),
                          AnimBench::point(100, 100)});
  CHECK(b.world.find(unit)->clock.mode == 0);
  b.world.advance(1500);
  const script::HostOutcome orphan = idle.run();
  CHECK(orphan.status == script::HostStatus::retry);
  CHECK(orphan.suspend_for > 1000000);
}
