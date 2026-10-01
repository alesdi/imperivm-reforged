// Tick loop tests.
//
// No game data. The numbers the fixtures are built around are measured, and
// where they are, the comment says from what: `DATA\CONST.INI` for the game
// speed scale, and the nine `Logs/*/desync.txt` dumps for the turn length,
// the turn window and the `Tick` field. docs/engine/tick.md has the
// derivation.
//
// The tests that matter most are the last three. The turn length is **not**
// constant in this engine — the dumps show 200, 400, 799 and 800, renegotiated
// mid-session — so the property to defend is not "N identical ticks equal one
// batch of N". It is the stronger one: **any sequence of declared turn lengths
// must leave the world where the single turn of their sum would.** If that
// ever fails, something has started integrating instead of computing, and the
// conformance harness is worthless until it is found.

#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/tick.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A unit-shaped entity with a looping idle (slot 13, four rows at 66) and a
/// one-shot transition (slot 19, three rows at 100). Modelled on BBowman,
/// whose idle really is 66 units a row.
constexpr std::string_view kUnit = R"(<?xml version="1.0"?>
<entity name="fixture" type="vx/unit" variations="8">
  <images>
    <image idx="1" file="idle.rle" drawmode="player_color" remaping="none" rows="4" columns="8"/>
    <image idx="2" file="toattack.rle" drawmode="player_color" remaping="none" rows="3" columns="8"/>
    <image idx="3" file="fire.rle" drawmode="clouds" remaping="pingpong" rows="4" columns="1"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="13" anim_frame="1"/>
    <state idx="2" name="attack" image_idx="1" image_row="0" anim_idx="65536" anim_frame="65536"/>
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
      <frame idx="2" duration="140"/>
      <frame idx="3" duration="200"/>
      <frame idx="4" duration="300"/>
      <frame idx="5" duration="500"/>
      <frame idx="6" duration="0"/>
    </anim>
  </anims>
</entity>)";

Result<Entity> unit() { return Entity::parse(bytes(kUnit)); }

}  // namespace

// --------------------------------------------------------------------------
// the clock
// --------------------------------------------------------------------------

TEST(game_time_is_the_sum_of_the_declared_turn_lengths) {
  Clock clock;
  CHECK(clock.turns() == 0);
  CHECK(clock.time() == 0);
  CHECK(clock.turn_length() == kDefaultTurnLength);
  CHECK(clock.config().game_speed == kDefaultGameSpeed);

  clock.advance();
  CHECK(clock.turns() == 1);
  CHECK(clock.time() == 400);

  // Renegotiated, as the hash histories show a live session doing.
  clock.advance(800);
  CHECK(clock.turns() == 2);
  CHECK(clock.time() == 1200);
  CHECK(clock.turn_length() == 800);  // the declared length becomes current

  clock.advance();  // still 800
  CHECK(clock.time() == 2000);
  clock.advance(200);
  CHECK(clock.time() == 2200);
  CHECK(clock.turns() == 4);
}

TEST(a_turn_names_the_window_the_dump_records) {
  // `[GAMETIME]`: gametimetickstart = gametime + 1, gametimetickend =
  // gametimetickstart + length. Both hold exactly in all nine dumps.
  Clock clock;
  // By value: `Clock::turn()` returns a reference to the live record, and the
  // next advance overwrites it.
  const Turn first = clock.advance(400);
  CHECK(first.index == 1);
  CHECK(first.length == 400);
  CHECK(first.start == 1);
  CHECK(first.end == 401);
  CHECK(first.time == 400);

  const Turn second = clock.advance(800);
  CHECK(second.index == 2);
  CHECK(second.start == 401);
  CHECK(second.end == 1201);
  CHECK(second.time == 1200);
  CHECK(second.start == first.time + 1);
  CHECK(second.end == second.start + second.length);
}

TEST(the_dumps_tick_field_is_one_more_than_the_turns_that_ran) {
  // Measured across all nine dumps, exactly: Tick=0x2 with one pump turn
  // recorded, Tick=0x2179 with 8,568. The dump is written at the head of the
  // turn that has not run yet.
  Clock clock;
  clock.advance();
  CHECK(clock.turns() + 1 == 2);
  for (int i = 0; i < 8567; ++i) clock.advance();
  CHECK(clock.turns() == 8568);
  CHECK(clock.turns() + 1 == 8569);  // 0x2179
}

TEST(the_turn_length_is_world_state_and_survives_being_read_back) {
  Clock clock(TickConfig{800, kDefaultGameSpeed});
  CHECK(clock.turn_length() == 800);
  clock.set_turn_length(460);  // the value one 2020 log renegotiates to
  CHECK(clock.turn_length() == 460);
  clock.advance();
  CHECK(clock.time() == 460);
  // Refused rather than obeyed: a zero-length turn would advance nothing.
  clock.set_turn_length(0);
  CHECK(clock.turn_length() == 460);
  clock.set_turn_length(-5);
  CHECK(clock.turn_length() == 460);
  clock.advance(0);
  CHECK(clock.time() == 920);
}

TEST(an_invalid_configuration_falls_back_rather_than_stalling) {
  Clock zero(TickConfig{0, 0});
  CHECK(zero.config().turn_length == kDefaultTurnLength);
  CHECK(zero.config().game_speed == kDefaultGameSpeed);
  Clock negative(TickConfig{-5, kDefaultGameSpeed});
  CHECK(negative.config().turn_length == kDefaultTurnLength);
}

TEST(a_reset_clock_starts_over) {
  Clock clock;
  clock.advance(800);
  clock.advance(800);
  REQUIRE(clock.time() == 1600);
  clock.reset();
  CHECK(clock.time() == 0);
  CHECK(clock.turns() == 0);
  CHECK(clock.turn_length() == 800);  // the negotiated length is not undone
}

// --------------------------------------------------------------------------
// game speed
// --------------------------------------------------------------------------

TEST(game_speed_scales_a_real_time_turn_into_game_time) {
  // The one measurement that pins this down: eight dumps run at gamespeed
  // 1000 with turn lengths 200/400/800, and the ninth runs at 999 with a turn
  // length of 799. floor(800 * 999 / 1000) = 799.
  CHECK(turn_length_from_real_ms(800, 999) == 799);
  CHECK(turn_length_from_real_ms(800, 1000) == 800);
  CHECK(turn_length_from_real_ms(400, 1000) == 400);
  CHECK(turn_length_from_real_ms(200, 1000) == 200);

  // The `CONST.INI` speed presets, on a 400 ms turn.
  CHECK(turn_length_from_real_ms(400, 700) == 280);   // SlowSpeed
  CHECK(turn_length_from_real_ms(400, 1400) == 560);  // FastSpeed
  CHECK(turn_length_from_real_ms(400, 2000) == 800);  // FastestSpeed
}

TEST(a_turn_never_scales_away_to_nothing) {
  CHECK(turn_length_from_real_ms(1, 1) >= 1);
  CHECK(turn_length_from_real_ms(0, 1000) >= 1);
  CHECK(turn_length_from_real_ms(-5, 1000) >= 1);
  CHECK(turn_length_from_real_ms(100, 0) >= 1);
}

TEST(changing_speed_does_not_disturb_accumulated_time) {
  Clock clock;
  clock.advance(400);
  clock.advance(400);
  REQUIRE(clock.time() == 800);
  clock.set_game_speed(2000);
  CHECK(clock.time() == 800);
  CHECK(clock.config().game_speed == 2000);
  clock.set_game_speed(0);  // refused
  CHECK(clock.config().game_speed == 2000);
}

// --------------------------------------------------------------------------
// the world
// --------------------------------------------------------------------------

TEST(objects_are_ticked_in_spawn_order) {
  // Iteration order is world state (docs/engine/architecture.md). Ids are
  // assigned from 1 and the vector holds them in the order they arrived.
  auto entity = unit();
  REQUIRE(entity.ok());
  World world;
  const ObjectId first = world.spawn(NativeClass::unit, &*entity);
  const ObjectId second = world.spawn(NativeClass::building, &*entity);
  const ObjectId third = world.spawn(NativeClass::decor, nullptr);
  CHECK(first == 1);
  CHECK(second == 2);
  CHECK(third == 3);
  REQUIRE(world.objects().size() == 3);
  CHECK(world.objects()[0].object->id == 1);
  CHECK(world.objects()[1].object->id == 2);
  CHECK(world.objects()[2].object->id == 3);
  CHECK(world.objects()[1].object->native_class() == NativeClass::building);

  // Removing the middle one leaves the rest in order, and its id is not reused.
  CHECK(world.despawn(second));
  REQUIRE(world.objects().size() == 2);
  CHECK(world.objects()[0].object->id == 1);
  CHECK(world.objects()[1].object->id == 3);
  CHECK(world.spawn(NativeClass::decor, nullptr) == 4);
  CHECK(world.find(second) == nullptr);
  CHECK(!world.despawn(second));
}

TEST(playing_a_slot_starts_it_at_the_first_frame) {
  auto entity = unit();
  REQUIRE(entity.ok());
  World world;
  const ObjectId id = world.spawn(NativeClass::unit, &*entity);
  REQUIRE(world.play_anim(id, kAnimIdle, AnimRepeat::loop));

  const WorldObject* object = world.find(id);
  REQUIRE(object != nullptr);
  CHECK(object->animating);
  CHECK(object->object->anim.anim_slot == kAnimIdle);
  CHECK(object->object->anim.elapsed_ms == 0);
  CHECK(object->object->anim.step == 0);
  CHECK(object->timeline.cycle() == 264);
}

TEST(playing_an_undeclared_slot_fails_without_disturbing_anything) {
  // `PlayAnim(0, …)` and `PlayAnim(16, …)` are in shipped script code and no
  // entity declares either. A miss must leave the unit walking, not freeze it.
  auto entity = unit();
  REQUIRE(entity.ok());
  World world(TickConfig{33, kDefaultGameSpeed});
  const ObjectId id = world.spawn(NativeClass::unit, &*entity);
  REQUIRE(world.play_anim(id, kAnimIdle, AnimRepeat::loop));
  world.advance_turns(3);

  const WorldObject* object = world.find(id);
  REQUIRE(object != nullptr);
  const std::int32_t elapsed = object->object->anim.elapsed_ms;
  CHECK(elapsed == 99);

  CHECK(!world.play_anim(id, 0));
  CHECK(!world.play_anim(id, 16));
  CHECK(!world.has_anim(id, 0));
  CHECK(!world.has_anim(id, 16));
  CHECK(world.has_anim(id, kAnimIdle));
  CHECK(object->object->anim.anim_slot == kAnimIdle);
  CHECK(object->object->anim.elapsed_ms == elapsed);
  CHECK(object->animating);

  // And on an object with no entity at all, which six retail classes produce.
  const ObjectId bare = world.spawn(NativeClass::decor, nullptr);
  CHECK(!world.play_anim(bare, kAnimIdle));
  CHECK(!world.has_anim(bare, kAnimIdle));
  CHECK(!world.play_anim(999, kAnimIdle));  // no such object
}

TEST(entering_a_state_loops_the_animation_it_names) {
  auto entity = unit();
  REQUIRE(entity.ok());
  World world;
  const ObjectId id = world.spawn(NativeClass::unit, &*entity);

  REQUIRE(world.enter_state(id, 1));
  const WorldObject* object = world.find(id);
  REQUIRE(object != nullptr);
  CHECK(object->object->anim.state_idx == 1);
  CHECK(object->object->anim.anim_slot == kAnimIdle);
  CHECK(object->repeat == AnimRepeat::loop);
  CHECK(object->animating);

  // State 2 is a still pose: 987 of the retail states are.
  REQUIRE(world.enter_state(id, 2));
  CHECK(object->object->anim.state_idx == 2);
  CHECK(object->object->anim.anim_slot == kNoAnim);
  CHECK(!object->animating);

  CHECK(!world.enter_state(id, 77));
}

TEST(a_looping_animation_walks_its_frames_over_game_time) {
  // Four rows at 66. Turns of 33: frame boundaries at 66, 132, 198, 264.
  auto entity = unit();
  REQUIRE(entity.ok());
  World world(TickConfig{33, kDefaultGameSpeed});
  const ObjectId id = world.spawn(NativeClass::unit, &*entity);
  REQUIRE(world.play_anim(id, kAnimIdle, AnimRepeat::loop));
  const WorldObject* object = world.find(id);
  REQUIRE(object != nullptr);

  const std::uint32_t expected[9] = {0, 1, 1, 2, 2, 3, 3, 0, 0};
  for (std::uint32_t turn = 1; turn <= 9; ++turn) {
    world.advance();
    CHECK(object->object->anim.step == expected[turn - 1]);
  }
  CHECK(object->animating);  // a loop never finishes
  CHECK(world.turns() == 9);
  CHECK(world.time() == 297);
}

TEST(a_one_shot_animation_holds_its_last_frame_and_stops) {
  auto entity = unit();
  REQUIRE(entity.ok());
  World world(TickConfig{100, kDefaultGameSpeed});
  const ObjectId id = world.spawn(NativeClass::unit, &*entity);
  REQUIRE(world.play_anim(id, kAnimToAttack, AnimRepeat::hold));
  const WorldObject* object = world.find(id);
  REQUIRE(object != nullptr);
  REQUIRE(object->timeline.cycle() == 300);

  world.advance();  // 100
  CHECK(object->object->anim.step == 1);
  world.advance();  // 200
  CHECK(object->object->anim.step == 2);
  CHECK(object->animating);
  world.advance();  // 300: done
  CHECK(object->object->anim.step == 2);
  CHECK(!object->animating);

  // And it stays put however long the world runs on.
  world.advance_turns(100000);
  CHECK(object->object->anim.step == 2);
  CHECK(object->object->anim.elapsed_ms == 300);
}

TEST(a_pingpong_animation_advances_without_snapping_back) {
  // Non-uniform holds on a pingpong sheet: 140, 200, 300, 500 up, then 300 and
  // 200 back. Cycle 1640, not 2 x 1140.
  auto entity = unit();
  REQUIRE(entity.ok());
  World world(TickConfig{10, kDefaultGameSpeed});
  const ObjectId id = world.spawn(NativeClass::unit, &*entity);
  REQUIRE(world.play_anim(id, kAnimWalk, AnimRepeat::loop));
  const WorldObject* object = world.find(id);
  REQUIRE(object != nullptr);
  REQUIRE(object->timeline.steps() == 6);
  REQUIRE(object->timeline.cycle() == 1640);

  // Boundaries at 140, 340, 640, 1140, 1440, 1640.
  const std::int32_t at[7] = {0, 140, 340, 640, 1140, 1440, 1640};
  const std::uint32_t rows[7] = {0, 1, 2, 3, 2, 1, 0};
  for (int i = 0; i < 7; ++i) {
    CHECK(object->timeline.sample(at[i], AnimRepeat::loop).row == rows[i]);
  }

  world.advance_turns(64);  // 640 units in
  CHECK(object->object->anim.step == 3);
  world.advance_turns(50);  // 1140
  CHECK(object->object->anim.step == 4);
  CHECK(object->timeline.row_of_step(object->object->anim.step) == 2);
}

TEST(an_object_with_no_animation_is_ticked_and_left_alone) {
  auto entity = unit();
  REQUIRE(entity.ok());
  World world;
  const ObjectId id = world.spawn(NativeClass::decor, &*entity);
  const WorldObject* object = world.find(id);
  REQUIRE(object != nullptr);
  CHECK(!object->animating);
  world.advance_turns(500);
  CHECK(object->object->anim.step == 0);
  CHECK(object->object->anim.elapsed_ms == 0);
  CHECK(world.turns() == 500);
  CHECK(world.time() == 500 * kDefaultTurnLength);
}

TEST(the_world_records_the_turn_the_clock_ran) {
  auto entity = unit();
  REQUIRE(entity.ok());
  World world;
  world.spawn(NativeClass::unit, &*entity);
  const Turn& turn = world.advance(800);
  CHECK(turn.index == 1);
  CHECK(turn.length == 800);
  CHECK(world.time() == 800);
  CHECK(world.turn().end == 801);
  CHECK(world.turns() == 1);
}

// --------------------------------------------------------------------------
// step equivalence over a sequence of declared turn lengths
// --------------------------------------------------------------------------

namespace {

/// A world with one of everything the tick loop can do to a cursor: a loop, a
/// one-shot, a pingpong loop and a still pose.
World populated(const Entity& entity) {
  World world;
  const ObjectId looping = world.spawn(NativeClass::unit, &entity);
  const ObjectId once = world.spawn(NativeClass::unit, &entity);
  const ObjectId ping = world.spawn(NativeClass::decor, &entity);
  const ObjectId still = world.spawn(NativeClass::building, &entity);
  world.play_anim(looping, kAnimIdle, AnimRepeat::loop);
  world.play_anim(once, kAnimToAttack, AnimRepeat::hold);
  world.play_anim(ping, kAnimWalk, AnimRepeat::loop);
  world.enter_state(still, 2);
  return world;
}

/// Everything a turn is allowed to change about the animation cursors, in
/// object order. Compared instead of the whole state hash where two runs
/// legitimately differ in turn *count*.
std::vector<std::int64_t> cursors(const World& world) {
  std::vector<std::int64_t> out;
  for (const WorldObject& slot : world.objects()) {
    out.push_back(slot.object->anim.elapsed_ms);
    out.push_back(slot.object->anim.step);
    out.push_back(slot.object->anim.anim_slot);
    out.push_back(slot.animating ? 1 : 0);
  }
  return out;
}

}  // namespace

TEST(a_sequence_of_turns_equals_the_same_sequence_run_one_call_at_a_time) {
  // The lengths the dumps actually show, in an order one of them actually
  // renegotiated through.
  auto entity = unit();
  REQUIRE(entity.ok());
  const std::vector<std::int32_t> lengths = {400, 400, 460, 800, 800, 200, 799, 200, 400};

  World batched = populated(*entity);
  World stepped = populated(*entity);
  REQUIRE(batched.state_hash() == stepped.state_hash());

  batched.advance(std::span<const std::int32_t>(lengths));
  for (const std::int32_t length : lengths) stepped.advance(length);

  CHECK(batched.time() == stepped.time());
  CHECK(batched.turns() == stepped.turns());
  CHECK(batched.turns() == lengths.size());
  CHECK(batched.state_hash() == stepped.state_hash());
  CHECK(cursors(batched) == cursors(stepped));
}

TEST(a_sequence_of_turns_leaves_the_cursors_where_one_turn_of_their_sum_would) {
  // The real property. The turn length varies in this engine, so what has to
  // hold is that *how* an interval was cut up cannot be observed — only its
  // total. If an accumulator ever creeps into playback, this is what catches
  // it, and nothing else here would.
  auto entity = unit();
  REQUIRE(entity.ok());

  const std::vector<std::vector<std::int32_t>> partitions = {
      {1400},
      {400, 800, 200},
      {200, 200, 200, 200, 200, 200, 200},
      {1, 1399},
      {1399, 1},
      {799, 400, 200, 1},
      {7, 11, 13, 1369},
  };

  std::vector<std::int64_t> reference;
  for (std::size_t i = 0; i < partitions.size(); ++i) {
    World world = populated(*entity);
    std::int64_t total = 0;
    for (const std::int32_t length : partitions[i]) {
      world.advance(length);
      total += length;
    }
    REQUIRE(total == 1400);
    CHECK(world.time() == 1400);
    if (i == 0) {
      reference = cursors(world);
      REQUIRE(!reference.empty());
    } else {
      CHECK(cursors(world) == reference);
    }
  }
}

TEST(a_thousand_turns_singly_equal_a_thousand_turns_batched) {
  auto entity = unit();
  REQUIRE(entity.ok());

  World stepped = populated(*entity);
  World batched = populated(*entity);
  REQUIRE(stepped.state_hash() == batched.state_hash());

  const std::vector<std::int32_t> lengths(1000, kDefaultTurnLength);
  for (int i = 0; i < 1000; ++i) stepped.advance();
  batched.advance(std::span<const std::int32_t>(lengths));

  CHECK(stepped.turns() == 1000);
  CHECK(batched.turns() == 1000);
  CHECK(stepped.time() == batched.time());
  CHECK(stepped.time() == 400000);
  CHECK(stepped.state_hash() == batched.state_hash());

  REQUIRE(stepped.objects().size() == batched.objects().size());
  for (std::size_t i = 0; i < stepped.objects().size(); ++i) {
    const AnimCursor& a = stepped.objects()[i].object->anim;
    const AnimCursor& b = batched.objects()[i].object->anim;
    CHECK(a.elapsed_ms == b.elapsed_ms);
    CHECK(a.step == b.step);
    CHECK(a.anim_slot == b.anim_slot);
    CHECK(stepped.objects()[i].animating == batched.objects()[i].animating);
  }
}

TEST(advancing_no_turns_changes_nothing) {
  auto entity = unit();
  REQUIRE(entity.ok());
  World world = populated(*entity);
  world.advance_turns(37);
  const std::uint64_t before = world.state_hash();
  world.advance_turns(0);
  world.advance(std::span<const std::int32_t>{});
  CHECK(world.state_hash() == before);
  CHECK(world.turns() == 37);
}

TEST(a_turn_longer_than_a_looping_cycle_does_not_overflow) {
  // Elapsed time is 32 bits and a run of turns is not bounded by anything.
  // Reducing the delta before adding it is what keeps this in range.
  auto entity = unit();
  REQUIRE(entity.ok());
  World world;
  const ObjectId id = world.spawn(NativeClass::unit, &*entity);
  REQUIRE(world.play_anim(id, kAnimIdle, AnimRepeat::loop));
  const WorldObject* object = world.find(id);
  REQUIRE(object != nullptr);

  world.advance_turns(1000000);
  CHECK(object->object->anim.elapsed_ms >= 0);
  CHECK(object->object->anim.elapsed_ms < object->timeline.cycle());
  CHECK(object->object->anim.step < object->timeline.steps());

  // 1e6 turns x 400 units = 4e8 units; 4e8 mod 264 = 136.
  CHECK(world.time() == 400000000);
  CHECK(object->object->anim.elapsed_ms == 136);

  // And one enormous single turn lands in the same place.
  World once;
  const ObjectId other = once.spawn(NativeClass::unit, &*entity);
  once.play_anim(other, kAnimIdle, AnimRepeat::loop);
  once.advance(400000000);
  CHECK(once.find(other)->object->anim.elapsed_ms == 136);
}

TEST(the_state_hash_notices_what_a_turn_can_change) {
  auto entity = unit();
  REQUIRE(entity.ok());
  World world;
  const ObjectId id = world.spawn(NativeClass::unit, &*entity);
  world.play_anim(id, kAnimIdle, AnimRepeat::loop);

  const std::uint64_t start = world.state_hash();
  world.advance();
  CHECK(world.state_hash() != start);

  World other;
  const ObjectId same = other.spawn(NativeClass::unit, &*entity);
  other.play_anim(same, kAnimIdle, AnimRepeat::loop);
  other.advance();
  CHECK(other.state_hash() == world.state_hash());

  // A different repeat mode is a different simulation.
  World third;
  const ObjectId held = third.spawn(NativeClass::unit, &*entity);
  third.play_anim(held, kAnimIdle, AnimRepeat::hold);
  third.advance();
  CHECK(third.state_hash() != world.state_hash());

  // So is a different turn length, even at the same accumulated time.
  World split;
  const ObjectId parts = split.spawn(NativeClass::unit, &*entity);
  split.play_anim(parts, kAnimIdle, AnimRepeat::loop);
  split.advance(200);
  split.advance(200);
  CHECK(split.time() == world.time());
  CHECK(split.state_hash() != world.state_hash());  // two turns, not one
}
