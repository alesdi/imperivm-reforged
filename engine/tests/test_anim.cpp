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
#include "imperivm/core/sim/anim.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

/// Three animations: slot 13 (264 units), slot 19 (300, action moment 100) and
/// slot 1, whose sheet is `pingpong`. Slot 5 is deliberately absent, because a
/// missing slot is an ordinary case and not an error.
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
    <anim idx="1" name="fire" startstate="1" endstate="1" frames="6" duration="0"
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
    ctx.name = name;
    ctx.kind = script::CallKind::member;
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

/// `PlayAnim` **suspends**, and for the animation's own length.
///
/// `DATA\SUBAI\ANIM.VS` is `while (1) This.PlayAnim(1, This.pos);` with no
/// `Sleep` in it, and 28 shipped classes bind it -- the blacksmith fires, the
/// wells, the obelisks, the barrack horses. A `PlayAnim` that returned would
/// spin the instruction budget every tick forever, which is the shape
/// `Unit::Attack` was caught in. `gbr.exe` settles it: `Obj::PlayAnim` is one
/// of only two members of this family registered through the *suspending*
/// registrar.
TEST(anim_play_anim_suspends_for_the_length_of_what_it_started) {
  AnimBench b;
  const ObjectId id = b.spawn();
  const script::HostOutcome out =
      b.call("PlayAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                             AnimBench::point(100, 100)});
  CHECK(out.status == script::HostStatus::suspend);
  // Slot 13's four held frames are 66 units each.
  CHECK(out.suspend_for == 264);
  CHECK(b.world.find(id)->object->anim.anim_slot == 13);

  // A slot nothing declares plays nothing and suspends for zero -- which still
  // yields, because the scheduler resumes each script at most once per pass. So
  // `ANIM.VS` stays bounded even on an object whose entity is missing.
  const script::HostOutcome miss =
      b.call("PlayAnim", 2, {AnimBench::obj(id), script::Value::integer(5),
                             AnimBench::point(100, 100)});
  CHECK(miss.status == script::HostStatus::suspend);
  CHECK(miss.suspend_for == 0);

  // And an unresolvable receiver does not suspend at all: there is nothing to
  // wait for.
  const script::HostOutcome dead =
      b.call("PlayAnim", 2, {AnimBench::obj(9999), script::Value::integer(13),
                             AnimBench::point(0, 0)});
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

  // 0 here is slot 1, which the fixture declares: four frames of 50.
  const script::HostOutcome out =
      b.call("PlayAnim", 3, {AnimBench::obj(id), script::Value::integer(0),
                             AnimBench::point(100, 100), script::Value::integer(-1)});
  CHECK(out.status == script::HostStatus::suspend);
  CHECK(out.suspend_for == 200);
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

  // An animation whose moment is at zero answers zero from the start.
  b.call("StartAnim", 2, {AnimBench::obj(id), script::Value::integer(13),
                          AnimBench::point(100, 100)});
  CHECK(b.number("TimeToActionMoment", 0, {AnimBench::obj(id)}) == 0);
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
  CHECK(b.number("GetAnimDuration", 1, {AnimBench::obj(id), script::Value::integer(0)}) == 200);
  // Which is to say the two disagree about every number.
  CHECK(b.number("GetAnimTime", 1, {AnimBench::obj(id), script::Value::integer(12)}) == 0);

  // The original dereferences null here rather than checking; this answers 0.
  CHECK(b.number("GetAnimDuration", 1, {AnimBench::obj(9999), script::Value::integer(0)}) == 0);
  CHECK(b.number("GetAnim", 0, {AnimBench::obj(9999)}) == 0);
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

/// `StartDelayedAnim(slot, point, delay)` -- one site, and its delay is `-1`.
///
/// `CATAPULT_IDLE.VS:32` is `.StartDelayedAnim(1, .pos, -1);`, and `-1` is the
/// original's own special case: apply the animation immediately and locally
/// rather than arming the object's timer. So the one shipped call is
/// `StartAnim` under a longer name, and that is what this is.
TEST(anim_start_delayed_anim_is_start_anim_at_the_one_delay_that_ships) {
  AnimBench b;
  const ObjectId id = b.spawn();
  CHECK(b.call("StartDelayedAnim", 3,
               {AnimBench::obj(id), script::Value::integer(13), AnimBench::point(100, 100),
                script::Value::integer(-1)})
            .status == script::HostStatus::ok);
  CHECK(b.world.find(id)->object->anim.anim_slot == 13);
  CHECK(b.world.find(id)->object->anim.elapsed_ms == 0);
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
