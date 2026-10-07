// Where the view draws the world between two turns (`sim/glide.hpp`).
//
// `gbr.exe` advances its game clock every frame inside the turn's lockstep
// window (0x0051ea30 -> 0x00528e40 -> 0x00528b40) and draws a moving object
// where its animation has it at that instant (0x0053d830). This engine runs a
// turn whole, so the view draws the instant between the turn end before and
// the world's: a walk part of the way along, an animation's clock carried on.
// Nothing here may write the world, and the last test says so with the hash.

#include <cstdint>
#include <span>
#include <string_view>

#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/glide.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

[[nodiscard]] Point pt(std::int32_t x, std::int32_t y) { return Point{x, y}; }

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// One animation of three 100 ms rows: a 300 ms cycle.
constexpr std::string_view kEntityXml = R"(<?xml version="1.0"?>
<entity name="walker" type="vx/unit" variations="1">
  <images>
    <image idx="1" file="walk.rle" drawmode="player_color" remaping="none" rows="3" columns="8"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="1" anim_frame="1"/>
  </states>
  <anims>
    <anim idx="1" name="walk" startstate="1" endstate="1" frames="4" duration="300"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
      <frame idx="4" duration="100"/>
    </anim>
  </anims>
</entity>)";

/// An open field and a unit walking east across it at 100 units a second.
struct Walk {
  World world;
  MovementSystem movement;
  ObjectId unit = kNoObject;

  explicit Walk(Point from = pt(100, 100), Point to = pt(1500, 100)) {
    movement.set_grid(ObstructionGrid(128, 128));
    REQUIRE(world.add_system(&movement));
    world.start();
    unit = spawn(from);
    REQUIRE(movement.order_goto(world, unit, to, 0) == MoveOutcome::moving);
  }

  ObjectId spawn(Point at) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr);
    (void)world.set_position(id, at);
    movement.state(id).speed = 100;
    return id;
  }

  const WorldObject& slot(ObjectId id) const { return *world.find(id); }
};

}  // namespace

/// A walking unit is drawn on the straight line between where it stood at the
/// turn end before and where it stands now, by the drawn instant's place
/// between the two -- the original's anchor, run by its clock.
TEST(glide_draws_a_walking_unit_between_its_two_turn_ends) {
  Walk w;
  TurnGlide glide;
  glide.observe(w.world);
  // No turn end before the first one seen: drawn where it is.
  CHECK(glide.position(w.world, w.slot(w.unit), 0) == pt(100, 100));

  w.world.advance(400);
  glide.observe(w.world);
  REQUIRE(w.world.resolve_position(w.unit) == pt(140, 100));
  CHECK(glide.since() == 0);
  CHECK(glide.position(w.world, w.slot(w.unit), 0) == pt(100, 100));
  CHECK(glide.position(w.world, w.slot(w.unit), 100) == pt(110, 100));
  CHECK(glide.position(w.world, w.slot(w.unit), 200) == pt(120, 100));
  CHECK(glide.position(w.world, w.slot(w.unit), 399) == pt(139, 100));
  CHECK(glide.position(w.world, w.slot(w.unit), 400) == pt(140, 100));
  // Outside the window: held to its ends.
  CHECK(glide.position(w.world, w.slot(w.unit), -50) == pt(100, 100));
  CHECK(glide.position(w.world, w.slot(w.unit), 900) == pt(140, 100));

  // Observing again without a turn changes nothing.
  glide.observe(w.world);
  CHECK(glide.position(w.world, w.slot(w.unit), 200) == pt(120, 100));

  // The next turn is an 800: the window is that turn's.
  w.world.advance(800);
  glide.observe(w.world);
  REQUIRE(w.world.resolve_position(w.unit) == pt(220, 100));
  CHECK(glide.since() == 400);
  CHECK(glide.position(w.world, w.slot(w.unit), 800) == pt(180, 100));
  CHECK(glide.position(w.world, w.slot(w.unit), 1200) == pt(220, 100));

  // A slow frame that saw two turns run glides across both.
  w.world.advance(400);
  w.world.advance(400);
  glide.observe(w.world);
  REQUIRE(w.world.resolve_position(w.unit) == pt(300, 100));
  CHECK(glide.since() == 1200);
  CHECK(glide.position(w.world, w.slot(w.unit), 1600) == pt(260, 100));
}

/// Nothing glides that did not walk: a unit put down somewhere is drawn there
/// at once, at any instant, as it always was.
TEST(glide_leaves_a_unit_that_did_not_walk_where_it_is) {
  Walk w;
  const ObjectId still = w.spawn(pt(500, 500));
  // And one that goes further in a turn than any walk does: two units a
  // millisecond is twice `kGlideMaxSpeed`.
  const ObjectId fast = w.spawn(pt(100, 300));
  w.movement.state(fast).speed = 2000;
  REQUIRE(w.movement.order_goto(w.world, fast, pt(1900, 300), 0) == MoveOutcome::moving);
  TurnGlide glide;
  w.world.advance(400);
  glide.observe(w.world);
  REQUIRE(w.world.resolve_position(fast) == pt(900, 300));

  // Put down twenty units away by something that is not a route.
  (void)w.world.set_position(still, pt(520, 500));
  w.world.advance(400);
  glide.observe(w.world);
  CHECK(!w.slot(still).state.flags.has_active_path);
  CHECK(glide.position(w.world, w.slot(still), 400) == pt(520, 500));
  CHECK(glide.position(w.world, w.slot(still), 600) == pt(520, 500));
  // The walker beside it glides.
  CHECK(glide.position(w.world, w.slot(w.unit), 600) == pt(160, 100));
  // The fast one jumps, route or no route.
  REQUIRE(w.slot(fast).state.flags.has_active_path);
  REQUIRE(w.world.resolve_position(fast) == pt(1700, 300));
  CHECK(glide.position(w.world, w.slot(fast), 600) == pt(1700, 300));

  // Moved after the turn ended -- the editor's drag, five units on -- it is
  // where the world says, not on its way from the turn before.
  w.world.advance(400);
  glide.observe(w.world);
  REQUIRE(w.world.resolve_position(w.unit) == pt(220, 100));
  CHECK(glide.position(w.world, w.slot(w.unit), 1000) == pt(200, 100));
  (void)w.world.set_position(w.unit, pt(225, 100));
  CHECK(glide.position(w.world, w.slot(w.unit), 1000) == pt(225, 100));
}

/// A glide is between two turn ends of one world. A world seen for the first
/// time, or one whose clock went back (a load), is drawn as it stands until
/// its next turn.
TEST(glide_forgets_a_turn_end_of_another_world_or_an_earlier_clock) {
  Walk a;
  TurnGlide glide;
  glide.observe(a.world);
  a.world.advance(400);
  glide.observe(a.world);
  CHECK(glide.position(a.world, a.slot(a.unit), 200) == pt(120, 100));

  Walk b(pt(100, 100), pt(100, 1500));
  b.world.advance(400);
  b.world.advance(400);
  glide.observe(b.world);
  CHECK(glide.since() == b.world.time());
  CHECK(glide.position(b.world, b.slot(b.unit), 600) == b.world.resolve_position(b.unit));
  // And a world it no longer watches is drawn as it stands.
  CHECK(glide.position(a.world, a.slot(a.unit), 200) == pt(140, 100));

  b.world.advance(400);
  glide.observe(b.world);
  CHECK(glide.position(b.world, b.slot(b.unit), 1000) == pt(100, 200));

  glide.reset();
  CHECK(glide.position(b.world, b.slot(b.unit), 1000) == pt(100, 220));
}

/// An animation's clock is carried on from the turn end before to the drawn
/// instant exactly as `run_turn` carries it -- wrapping a loop -- so a walk
/// cycle steps frame by frame. One that started again, or stopped, is drawn
/// as the world has it.
TEST(glide_carries_an_animation_clock_from_the_turn_end_before) {
  Result<Entity> entity = Entity::parse(bytes_of(kEntityXml));
  REQUIRE(entity.ok());
  World world;
  world.start();
  const ObjectId id = world.spawn(NativeClass::unit, &entity.value());
  (void)world.set_position(id, pt(100, 100));
  REQUIRE(world.play_anim(id, 1, AnimRepeat::loop));
  REQUIRE(world.find(id)->timeline.cycle() == 300);

  TurnGlide glide;
  world.advance(200);
  glide.observe(world);
  world.advance(200);
  glide.observe(world);
  const WorldObject& slot = *world.find(id);
  REQUIRE(slot.object->anim.elapsed_ms == 100);  // 400 around a 300 cycle
  CHECK(glide.anim_elapsed(world, slot, 200) == 200);
  CHECK(glide.anim_elapsed(world, slot, 250) == 250);
  CHECK(glide.anim_elapsed(world, slot, 350) == 50);
  CHECK(glide.anim_elapsed(world, slot, 400) == 100);

  // Started again at this turn's end: from its start, at any instant.
  REQUIRE(world.play_anim(id, 1, AnimRepeat::loop));
  CHECK(glide.anim_elapsed(world, *world.find(id), 250) == 0);
  // The turn end before saw the run that was cut short, so the clock it
  // shows now is not that run's carried on: it is the new run's, which began
  // when the turn did -- its 200 ms are the whole turn -- and is drawn from
  // there, not as the world has it at the turn's end.
  world.advance(200);
  glide.observe(world);
  CHECK(glide.anim_elapsed(world, *world.find(id), 500) == 100);
  CHECK(glide.anim_elapsed(world, *world.find(id), 600) == 200);
  // And from the next turn on it is carried again.
  world.advance(200);
  glide.observe(world);
  CHECK(glide.anim_elapsed(world, *world.find(id), 750) == 50);

  // Stopped: frozen where it stopped, whatever the clock says.
  REQUIRE(world.stop_anim(id));
  world.advance(300);
  glide.observe(world);
  world.advance(300);
  glide.observe(world);
  CHECK(glide.anim_elapsed(world, *world.find(id), world.time() - 100) ==
        world.find(id)->object->anim.elapsed_ms);

  // A held animation that ran out inside the turn is drawn short of its end
  // before it got there, and at its end after.
  REQUIRE(world.play_anim(id, 1, AnimRepeat::hold));
  world.advance(200);
  glide.observe(world);
  world.advance(200);
  glide.observe(world);
  REQUIRE(!world.find(id)->animating);
  REQUIRE(world.find(id)->object->anim.elapsed_ms == 300);
  REQUIRE(world.time() == 1800);
  CHECK(glide.anim_elapsed(world, *world.find(id), 1650) == 250);
  CHECK(glide.anim_elapsed(world, *world.find(id), 1700) == 300);
  CHECK(glide.anim_elapsed(world, *world.find(id), 1800) == 300);
}

/// An animation begun at a turn's end -- where a script's `PlayAnim` and the
/// systems start one -- had not begun at any instant inside that turn: what
/// the turn end before saw is drawn there, carried on. One begun earlier in
/// the turn, as combat starts a swing at its blow, is drawn from its own
/// start, which its clock at the turn's end gives. The original's visual runs
/// an animation from the start time it was handed (0x0053e6c0, 0x0062a0e0).
TEST(glide_draws_an_animation_begun_inside_the_turn_from_its_own_start) {
  Result<Entity> entity = Entity::parse(bytes_of(kEntityXml));
  REQUIRE(entity.ok());
  World world;
  world.start();
  const ObjectId id = world.spawn(NativeClass::unit, &entity.value());
  (void)world.set_position(id, pt(100, 100));
  REQUIRE(world.play_anim(id, 1, AnimRepeat::hold));

  TurnGlide glide;
  world.advance(100);
  glide.observe(world);  // 100 ms into the first run
  world.advance(100);
  REQUIRE(world.play_anim(id, 1, AnimRepeat::hold));  // again, at the turn's end
  glide.observe(world);
  REQUIRE(world.time() == 200);
  const TurnGlide::Anim mid = glide.anim(world, *world.find(id), 150);
  CHECK(mid.earlier);
  CHECK(mid.anim_slot == 1);
  CHECK(mid.elapsed == 150);
  CHECK(glide.anim_elapsed(world, *world.find(id), 150) == 0);
  const TurnGlide::Anim end = glide.anim(world, *world.find(id), 200);
  CHECK(!end.earlier);
  CHECK(end.elapsed == 0);

  // Begun 40 ms before the turn's end: from then on on its own clock, and
  // before then the first run's.
  world.advance(100);
  glide.observe(world);  // the second run, 100 ms in
  world.advance(100);
  REQUIRE(world.play_anim(id, 1, AnimRepeat::hold));
  world.find(id)->object->anim.elapsed_ms = 40;
  glide.observe(world);
  REQUIRE(world.time() == 400);
  CHECK(!glide.anim(world, *world.find(id), 380).earlier);
  CHECK(glide.anim(world, *world.find(id), 380).elapsed == 20);
  CHECK(glide.anim(world, *world.find(id), 360).elapsed == 0);
  const TurnGlide::Anim before = glide.anim(world, *world.find(id), 350);
  CHECK(before.earlier);
  CHECK(before.elapsed == 150);  // the second run, carried on
}

/// A bird's leg replaced at a turn's end is flown to its end inside that
/// turn, not jumped over: the glide keeps the leg it saw at the turn end
/// before. Before this the bird was drawn at the new leg's start -- the old
/// leg's end -- for the whole of that turn.
TEST(glide_flies_a_replaced_leg_to_its_end) {
  Result<Entity> entity = Entity::parse(bytes_of(kEntityXml));
  REQUIRE(entity.ok());
  World world;
  world.start();
  const ObjectId id = world.spawn(NativeClass::flying_unit, &entity.value());
  const auto fly = [&](Point from, Point to) {
    REQUIRE(world.play_anim(id, 1, AnimRepeat::hold));
    (void)world.set_position(id, to);
    world.find(id)->flight = FlightLeg{from, to, true};
  };
  fly(pt(100, 100), pt(400, 100));  // 300 ms, a unit a millisecond

  TurnGlide glide;
  glide.observe(world);
  world.advance(200);
  glide.observe(world);
  world.advance(200);  // the leg ends at 300
  fly(pt(400, 100), pt(400, 400));  // and the script starts the next at 400
  glide.observe(world);
  REQUIRE(world.time() == 400);

  const auto drawn_at = [&](GameTime at) {
    const TurnGlide::Anim drawn = glide.anim(world, *world.find(id), at);
    const FlightProgress leg = flight_progress(drawn.flight, drawn.elapsed);
    return leg.moving() ? pt(leg.along(leg.from.x, leg.to.x), leg.along(leg.from.y, leg.to.y))
                        : drawn.flight.at;
  };
  CHECK(drawn_at(200) == pt(300, 100));
  CHECK(drawn_at(250) == pt(350, 100));
  CHECK(drawn_at(300) == pt(400, 100));
  CHECK(drawn_at(350) == pt(400, 100));  // the old leg's end, until the new one begins
  CHECK(drawn_at(400) == pt(400, 100));
  // And from the next turn the new leg runs on its own clock.
  world.advance(200);
  glide.observe(world);
  CHECK(drawn_at(500) == pt(400, 200));
}

/// A walk begun at the turn's end on a unit that already moved inside it --
/// `play_locomotion` runs after the unit has moved -- is drawn on its first
/// frame across that turn, not with what the unit stood playing before it set
/// off: the glide is walking it, and a standing pose would slide.
TEST(glide_walks_a_unit_that_set_off_inside_the_turn_on_its_walk) {
  Result<Entity> entity = Entity::parse(bytes_of(kEntityXml));
  REQUIRE(entity.ok());
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(128, 128));
  World world;
  REQUIRE(world.add_system(&movement));
  world.start();
  const ObjectId id = world.spawn(NativeClass::unit, &entity.value());
  (void)world.set_position(id, pt(100, 100));
  movement.state(id).speed = 100;
  REQUIRE(world.play_anim(id, 1, AnimRepeat::hold));  // standing, 50 ms into something

  TurnGlide glide;
  world.advance(50);
  glide.observe(world);
  REQUIRE(movement.order_goto(world, id, pt(1500, 100), 0) == MoveOutcome::moving);
  world.advance(200);
  glide.observe(world);
  const WorldObject& slot = *world.find(id);
  REQUIRE(slot.state.position != pt(100, 100));
  REQUIRE(slot.object->anim.elapsed_ms == 0);  // the walk, started at the turn's end
  const TurnGlide::Anim drawn = glide.anim(world, slot, 150);
  CHECK(!drawn.earlier);
  CHECK(drawn.elapsed == 0);
  CHECK(glide.position(world, slot, 150) != pt(100, 100));
}

/// Presentation: observing a world and asking where things are drawn reads it
/// and writes nothing -- the hash is the hash.
TEST(glide_writes_nothing_the_simulation_hashes) {
  Walk w;
  TurnGlide glide;
  glide.observe(w.world);
  w.world.advance(400);
  const std::uint64_t before = w.world.state_hash();
  glide.observe(w.world);
  (void)glide.position(w.world, w.slot(w.unit), 200);
  (void)glide.anim_elapsed(w.world, w.slot(w.unit), 200);
  CHECK(w.world.state_hash() == before);

  // And a world drawn between its turns runs on to the same place as one
  // that never was.
  Walk plain;
  plain.world.advance(400);
  for (int i = 0; i < 5; ++i) {
    w.world.advance(400);
    plain.world.advance(400);
    glide.observe(w.world);
    (void)glide.position(w.world, w.slot(w.unit), w.world.time() - 100);
  }
  CHECK(w.world.state_hash() == plain.world.state_hash());
}
