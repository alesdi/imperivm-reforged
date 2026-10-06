// Unit-versus-unit avoidance: the cooperative step and the ownerless locks.
//
// No game data. Every class, entity and grid is written here; the numbers the
// original uses (40, 50/75/100, 666, 5 per step, the six-lock ring) are the
// constants `sim/avoidance.hpp` names, and every expected coordinate is worked
// out by hand in the comment beside it.
//
// Two things are the point of the file:
//
//   * **The step is decided where the unit is at that instant.** A crowd
//     crossing itself must end on exactly the same coordinates, having drawn
//     exactly the same numbers, however the time was cut into turns -- which
//     only holds because decisions are taken in `(game time, id)` order at
//     exact instants rather than at turn ends.
//   * **What a decision decides is observable.** The wait is 50, then 75,
//     then 100 ms, stood still; a standing unit is walked round at once and
//     draws nothing; a march does not block itself; the dead, the held and the
//     airborne do not block at all. Each is asserted on a position, a clock
//     or the generator's state -- never on a private field alone.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/avoidance.hpp"
#include "imperivm/core/sim/entrance.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "builder.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A soldier whose walk (slot 1) advances 40 units a cycle and whose amble
/// (slot 13) advances 25: the two strides a route can be cut by.
constexpr std::string_view kSoldierEntity = R"(<?xml version="1.0"?>
<entity name="soldier" type="vx/unit" variations="8">
  <images>
    <image idx="1" file="walk.rle" drawmode="player_color" remaping="none" rows="2" columns="8"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="13" anim_frame="1"/>
  </states>
  <anims>
    <anim idx="1" name="walk" startstate="1" endstate="1" frames="4" duration="200"
          default_duration="0" action_time="0" step="40">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
      <frame idx="4" duration="0"/>
    </anim>
    <anim idx="13" name="amble" startstate="1" endstate="1" frames="4" duration="200"
          default_duration="0" action_time="0" step="25">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
      <frame idx="4" duration="0"/>
    </anim>
  </anims>
</entity>)";

/// A world `cells` x `cells` collision cells of open ground, one movement
/// system, and the classes: a walking soldier, a post that has a route and no
/// speed -- a *moving* unit, as far as `SyncFlags` bit 17 goes, that never
/// moves -- a big one of radius 60, a boat, and a grove that is not a unit.
struct Field {
  ClassGraph graph;
  Result<Entity> entity = Entity::parse(bytes_of(kSoldierEntity));
  World world;
  MovementSystem movement;

  explicit Field(std::int32_t cells = 128, bool start = true) {
    graph.add(bytes_of(R"(<class id="Soldier" cpp_class="CVXUnit" parent="">
      <properties speed="100" radius="15"/></class>)"),
              "soldier.sc.xml");
    graph.add(bytes_of(R"(<class id="Post" cpp_class="CVXUnit" parent="">
      <properties speed="0" radius="15"/></class>)"),
              "post.sc.xml");
    graph.add(bytes_of(R"(<class id="Big" cpp_class="CVXUnit" parent="">
      <properties speed="100" radius="60"/></class>)"),
              "big.sc.xml");
    graph.add(bytes_of(R"(<class id="Boat" cpp_class="CVXUnit" parent="">
      <properties speed="100" radius="15" water_unit="1"/></class>)"),
              "boat.sc.xml");
    graph.add(bytes_of(R"(<class id="Grove" cpp_class="CVXDecor" parent=""/>)"), "grove.sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    movement.set_class_graph(&graph);
    movement.set_grid(ObstructionGrid(cells, cells));
    REQUIRE(entity.ok());
    REQUIRE(world.add_system(&movement));
    if (start) world.start();
  }

  ObjectId unit(const char* class_name, Point where) {
    const ObjectId id = world.spawn(NativeClass::unit, &entity.value(), graph.find(class_name));
    CHECK(world.set_position(id, where));
    CHECK(world.set_health(id, 100));
    return id;
  }

  Point at(ObjectId id) { return world.resolve_position(id); }
  /// The unit's movement state; a failed check and a default one when it has
  /// none, since a helper that returns a value cannot `REQUIRE`.
  const MoveState& move(ObjectId id) {
    const MoveState* state = movement.find(id);
    CHECK(state != nullptr);
    return state == nullptr ? none : *state;
  }
  MoveState none;
};

/// A seed whose first `n` draws of `rand(1, 7)` are none of them 1, so that a
/// soft block escalates all the way instead of giving way early -- and, so
/// that the 7 is held to account, one under which a `rand(1, 6)` would have
/// hit 1 somewhere in the same `n`. Searched for rather than hard-coded, so
/// the test survives the generator being replaced. The 7 is a literal on
/// purpose: a test that asked `kEarlyGiveWayOdds` could not notice it moving.
std::uint32_t seed_without_early_give_way(int n) {
  for (std::uint32_t seed = 1;; ++seed) {
    Rng seven(seed);
    Rng six(seed);
    bool ok = true;
    bool six_hits = false;
    for (int i = 0; i < n && ok; ++i) {
      ok = seven.between(1, 7) != 1;
      if (six.between(1, 6) == 1) six_hits = true;
    }
    if (ok && six_hits) return seed;
  }
}

std::int64_t nearest_approach(Field& field, ObjectId mover, Point other, int turns, int length) {
  std::int64_t best = dist_sq(field.at(mover), other);
  for (int i = 0; i < turns; ++i) {
    field.world.advance(length);
    const std::int64_t d = dist_sq(field.at(mover), other);
    if (d < best) best = d;
  }
  return best;
}

}  // namespace

// --------------------------------------------------------------------------
// the arithmetic
// --------------------------------------------------------------------------

TEST(the_wait_climbs_fifty_seventy_five_a_hundred_then_gives_way) {
  CHECK(next_retry_time(0) == 50);
  CHECK(next_retry_time(50) == 75);
  CHECK(next_retry_time(75) == 100);
  CHECK(next_retry_time(100) == kGiveWay);
  CHECK(kGiveWay == 666);
  // The table's default arm: anything else stands.
  CHECK(next_retry_time(kGiveWay) == kGiveWay);
  CHECK(next_retry_time(60) == 60);
}

TEST(a_leg_is_cut_into_the_step_count_nearest_the_stride) {
  // 100 at 40: two steps of 50 (10 off) or three of 33.3 (6.7 off) -- three.
  CHECK(steps_in_leg(100, 40) == 3);
  // 90 at 40: two of 45 (5 off) or three of 30 (10 off) -- two.
  CHECK(steps_in_leg(90, 40) == 2);
  // 48 at 20: two of 24 and three of 16 are both 4 off. The longer count has
  // to be *strictly* nearer, so the tie keeps two.
  CHECK(steps_in_leg(48, 20) == 2);
  // Shorter than one stride is still one step; nothing is no steps.
  CHECK(steps_in_leg(30, 40) == 1);
  CHECK(steps_in_leg(0, 40) == 0);
  CHECK(steps_in_leg(40, 40) == 1);
}

TEST(step_boundaries_land_on_every_corner) {
  // An L: 100 east, then 90 north. Legs cut 3 and 2.
  const std::vector<Point> route = {{0, 0}, {100, 0}, {100, 90}};
  std::vector<std::int64_t> seen;
  std::int64_t at = 0;
  for (int guard = 0; guard < 10; ++guard) {
    at = next_step_boundary(route, 40, at);
    seen.push_back(at);
    if (at >= 190) break;
  }
  const std::vector<std::int64_t> expected = {33, 66, 100, 145, 190};
  CHECK(seen == expected);
  // Past the end there is nothing further: the length.
  CHECK(next_step_boundary(route, 40, 190) == 190);
  CHECK(next_step_boundary(route, 40, 500) == 190);
  // A zero-length leg is skipped rather than divided.
  const std::vector<Point> stutter = {{0, 0}, {0, 0}, {80, 0}};
  CHECK(next_step_boundary(stutter, 40, 0) == 40);
}

TEST(a_sidestep_goes_across_the_direction_of_travel) {
  const ObstructionGrid open(64, 64);
  const Grid no_terrain;
  // Walking east from (100, 100) to (140, 100); a neighbour of radius 15 on the
  // step's end. A quarter of the step turned a right angle is (0, 10), so the
  // candidates are y = 80, 90, 100, 110, 120, overlapping by 10, 20, 30, 20,
  // 10 and costing 10, 5, 0, 5, 10 more: 20, 25, 30, 25, 20. The tie keeps the
  // first, y = 80.
  const std::vector<Neighbour> one = {{Point{140, 100}, 15}};
  CHECK(pick_sidestep(Point{140, 100}, Point{100, 100}, 15, one, open, no_terrain, false) ==
        (Point{140, 80}));

  // Block that candidate's cell and the tie goes the other way.
  ObstructionGrid walled(64, 64);
  walled.set_cell(ObstructionGrid::cell_of(140), ObstructionGrid::cell_of(80), true);
  CHECK(pick_sidestep(Point{140, 100}, Point{100, 100}, 15, one, walled, no_terrain, false) ==
        (Point{140, 120}));

  // Nothing overlapping: every candidate's worst is 0 and the cost alone
  // decides, so the step's own end wins.
  // A neighbour off to one side, radius 20: the step's end overlaps it by 5
  // (distance 30, radii 35) and the candidate at y = 110 by none, which with a
  // cost of 5 a step is a tie the earlier candidate -- the step's end -- takes.
  // At any other cost the tie is broken, which is how the 5 is held to.
  const std::vector<Neighbour> aside = {{Point{120, 77}, 20}};
  CHECK(pick_sidestep(Point{140, 100}, Point{100, 100}, 15, aside, open, no_terrain, false) ==
        (Point{140, 100}));

  const std::vector<Neighbour> far = {{Point{400, 400}, 15}};
  CHECK(pick_sidestep(Point{140, 100}, Point{100, 100}, 15, far, open, no_terrain, false) ==
        (Point{140, 100}));

  // Standing on the step's end already: no sidestep at all.
  CHECK(pick_sidestep(Point{140, 100}, Point{140, 100}, 15, one, open, no_terrain, false) ==
        (Point{140, 100}));
}

TEST(a_sidestep_needs_its_midpoint_free_too) {
  const Grid no_terrain;
  // A step of 80 east: the quarter is (0, 20), candidates y = 60 .. 140.
  // Blocking the cell at (180, 80) removes k = -1 (the candidate itself) and
  // k = -2, whose midpoint with the step's end is (180, 80).
  ObstructionGrid grid(64, 64);
  grid.set_cell(ObstructionGrid::cell_of(180), ObstructionGrid::cell_of(80), true);
  const std::vector<Neighbour> one = {{Point{180, 100}, 15}};
  // Overlaps at y = 100, 120, 140 are 30, 10, 0: scores 30, 15, 10. k = +2.
  CHECK(pick_sidestep(Point{180, 100}, Point{100, 100}, 15, one, grid, no_terrain, false) ==
        (Point{180, 140}));
  // And the candidate's own cell counts on its own: block the cells of the two
  // outer candidates, (180, 60) and (180, 140), whose midpoints stay free. They
  // would have won at 10; k = -1 and +1 tie at 15 and the earlier takes it.
  ObstructionGrid far_cell(64, 64);
  far_cell.set_cell(ObstructionGrid::cell_of(180), ObstructionGrid::cell_of(140), true);
  far_cell.set_cell(ObstructionGrid::cell_of(180), ObstructionGrid::cell_of(60), true);
  CHECK(pick_sidestep(Point{180, 100}, Point{100, 100}, 15, one, far_cell, no_terrain, false) ==
        (Point{180, 80}));
}

TEST(a_sidestep_keeps_to_its_own_ground) {
  // Terrain in 64-unit cells: column 3 (x 192..255) is deep water, index 13,
  // everything else land. Sampled half a cell rounded, so x in [160, 224) reads
  // column 3... and the candidates below are chosen well inside.
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(64).u32(8).u32(1024).u32(1024);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) out.u8(x == 3 ? 13 : 1);
  }
  const std::vector<std::byte> bytes(out.span().begin(), out.span().end());
  const Result<Grid> terrain = Grid::parse(bytes);
  REQUIRE(terrain.ok());
  const ObstructionGrid open(64, 64);

  // Walking north up x = 200, the water column's middle, from (200, 300) to
  // (200, 260): the quarter across is (10, 0), so candidates are x = 180 ..
  // 220 -- all in the water column. A land unit may stand on none of them and
  // steps to the blocked end; a water unit takes the cheapest.
  const std::vector<Neighbour> one = {{Point{200, 260}, 15}};
  CHECK(pick_sidestep(Point{200, 260}, Point{200, 300}, 15, one, open, terrain.value(), false) ==
        (Point{200, 260}));
  CHECK(pick_sidestep(Point{200, 260}, Point{200, 300}, 15, one, open, terrain.value(), true) ==
        (Point{180, 260}));
}

TEST(a_lock_point_makes_one_lock_or_a_ring_of_six) {
  std::vector<StaticLock> locks;
  locks_of_point(Point{100, 100}, 9, locks);
  locks_of_point(Point{300, 300}, 10, locks);
  locks_of_point(Point{500, 500}, 12, locks);  // not a lock type
  locks_of_point(Point{700, 700}, 11, locks);
  REQUIRE(locks.size() == 8);
  CHECK(locks[0] == (StaticLock{Point{100, 100}, 20}));
  CHECK(locks[1] == (StaticLock{Point{300, 300}, 40}));
  // The ring: 40 out, at steps of 3.14/3, truncated per axis.
  const Point ring[6] = {{740, 700}, {720, 734}, {681, 734}, {661, 700}, {680, 666}, {719, 666}};
  for (int i = 0; i < 6; ++i) CHECK(locks[2 + i] == (StaticLock{ring[i], 40}));
}

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

TEST(a_route_with_no_stride_is_walked_as_before) {
  // No entity: no walk animation, no stride, no cooperative step. Two such
  // units walk straight through each other, which is what every unit did
  // before avoidance existed and what a route the original could not have
  // built still does here.
  Field field;
  const ObjectId a = field.world.spawn(NativeClass::unit, nullptr, field.graph.find("Soldier"));
  const ObjectId b = field.world.spawn(NativeClass::unit, nullptr, field.graph.find("Soldier"));
  CHECK(field.world.set_position(a, Point{100, 500}));
  CHECK(field.world.set_position(b, Point{900, 500}));
  CHECK(field.world.set_health(a, 100));
  CHECK(field.world.set_health(b, 100));
  field.movement.order_goto(field.world, a, Point{900, 500}, 0);
  field.movement.order_goto(field.world, b, Point{100, 500}, 0);
  CHECK(field.move(a).stride == 0);
  for (int i = 0; i < 12; ++i) field.world.advance(800);
  CHECK(field.at(a) == (Point{900, 500}));
  CHECK(field.at(b) == (Point{100, 500}));
  CHECK(field.movement.avoidance().decisions == 0);
}

TEST(the_stride_is_the_walk_animations_step_or_the_marchs) {
  Field field;
  const ObjectId hero = field.unit("Soldier", Point{100, 100});
  const ObjectId member = field.unit("Soldier", Point{100, 200});
  const ObjectId other = field.unit("Soldier", Point{100, 300});
  field.movement.order_goto(field.world, hero, Point{900, 100}, 0, 0, hero);
  field.movement.order_goto(field.world, member, Point{900, 200}, 0, 0, hero);
  field.movement.order_goto(field.world, other, Point{900, 300}, 0);
  // The walk slot's `step`; a march's follower takes the formation's 200, and
  // the hero leading it keeps its own.
  CHECK(field.move(hero).stride == 40);
  CHECK(field.move(member).stride == 200);
  CHECK(field.move(other).stride == 40);
  CHECK(field.move(member).party == hero);
  CHECK(field.move(other).party == kNoObject);
  // A different walk slot is a different stride, from the next route on.
  field.movement.state(other).walk_anim = 13;
  field.movement.order_goto(field.world, other, Point{900, 350}, 0);
  CHECK(field.move(other).stride == 25);
  // And an order that is not a march takes the unit out of one.
  field.movement.order_goto(field.world, member, Point{900, 250}, 0);
  CHECK(field.move(member).party == kNoObject);
  CHECK(field.move(member).stride == 40);
}

TEST(a_standing_unit_is_walked_round_at_once_and_draws_nothing) {
  Field field;
  // Steps of 40 from x = 100 end on x = 500, where the stander is. Tested
  // from x = 460, the end is taken by somebody with no route: a hard block,
  // which gives way at once -- no hold, no draw. The quarter across is (0, 10);
  // overlaps against the stander are 10, 20, 30, 20, 10 and the tie keeps
  // y = 480. The walk back from (500, 480) to the route at (540, 500) passes
  // the stander at 800 / sqrt(2000), 17.9 -- not 0, and not the 30 the radii
  // ask for: the original lets it brush.
  const ObjectId stander = field.unit("Soldier", Point{500, 500});
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
  const Rng before = field.world.rng();

  // The step it gives way on ends at x = 500 at t = 4000, a 10 ms turn's end:
  // on the side the tie picked, y = 480, not the mirror image.
  std::int64_t closest = dist_sq(field.at(walker), field.at(stander));
  bool seen_aside = false;
  for (int i = 0; i < 1200; ++i) {
    field.world.advance(10);
    if (field.at(walker) == (Point{500, 480})) seen_aside = true;
    closest = std::min(closest, dist_sq(field.at(walker), field.at(stander)));
  }
  CHECK(seen_aside);
  CHECK(closest >= 17 * 17);
  CHECK(closest <= 18 * 18);
  CHECK(field.world.rng() == before);
  const MovementSystem::AvoidanceCounters& a = field.movement.avoidance();
  CHECK(a.blocked == 1);
  CHECK(a.holds == 0);
  CHECK(a.sidesteps == 1);
  // It arrives where it was going, and the stander never moved.
  CHECK(field.at(walker) == (Point{900, 500}));
  CHECK(field.at(stander) == (Point{500, 500}));
}

TEST(a_soft_block_holds_fifty_then_seventy_five_then_a_hundred) {
  Field field;
  const std::uint32_t seed = seed_without_early_give_way(5);
  field.world.rng().seed(seed);

  // Five posts on the walker's line, each with a route and no speed: moving,
  // as far as bit 17 says, and going nowhere. Their own first steps end 40
  // units north of them, clear of each other and of the walker's line.
  for (const std::int32_t x : {300, 340, 380, 420, 460}) {
    const ObjectId post = field.unit("Post", Point{x, 500});
    field.movement.order_goto(field.world, post, Point{x, 100}, 0);
  }
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);

  // One-millisecond turns, so every decision lands at a turn's end and can be
  // read off the state it left. The holds follow one another with no step in
  // between, so a new one shows as a new `hold_until`, not as `holding` rising.
  std::vector<std::int32_t> holds;
  std::vector<Point> parked;
  GameTime last_hold = -1;
  for (int ms = 0; ms < 4000 && holds.size() < 4; ++ms) {
    field.world.advance(1);
    const MoveState& m = field.move(walker);
    if (m.holding && m.hold_until != last_hold) {
      holds.push_back(static_cast<std::int32_t>(m.hold_until - field.world.time()));
      parked.push_back(field.at(walker));
      last_hold = m.hold_until;
    }
    // Stood still for the whole of every hold.
    if (m.holding) CHECK(field.at(walker) == parked.back());
  }
  // Three holds at x = 260, the last step it was free to take; then the fourth
  // test climbed to 666 and gave way, across to (420, 460), and the wait went
  // back to nothing -- so the next block, by the post at 460, is a first hold
  // again, stood where the sidestep left it rather than back on the route.
  const std::vector<std::int32_t> expected = {50, 75, 100, 50};
  CHECK(holds == expected);
  REQUIRE(parked.size() == 4);
  CHECK(parked[0] == (Point{260, 500}));
  CHECK(parked[2] == (Point{260, 500}));
  CHECK(parked[3] == (Point{420, 460}));

  // Five soft blocks, five draws -- one each, taken whether or not it
  // mattered.
  Rng probe(seed);
  for (int i = 0; i < 5; ++i) (void)probe.between(1, 7);
  CHECK(field.world.rng() == probe);
  CHECK(field.movement.avoidance().holds == 4);
}

TEST(a_draw_of_one_gives_way_before_the_wait_runs_out) {
  // The same line of posts, under a seed whose first draw *is* 1: the very
  // first soft block gives way instead of holding.
  std::uint32_t seed = 1;
  for (;; ++seed) {
    Rng probe(seed);
    if (probe.between(1, 7) == 1) break;
  }
  Field field;
  field.world.rng().seed(seed);
  for (const std::int32_t x : {300, 340, 380, 420, 460}) {
    const ObjectId post = field.unit("Post", Point{x, 500});
    field.movement.order_goto(field.world, post, Point{x, 100}, 0);
  }
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
  for (int ms = 0; ms < 1700; ++ms) field.world.advance(1);
  const MovementSystem::AvoidanceCounters& a = field.movement.avoidance();
  REQUIRE(a.blocked >= 1);
  CHECK(a.early >= 1);
  // Nothing held first: the first block went straight to giving way.
  CHECK(a.sidesteps >= 1);
}

TEST(one_march_does_not_block_itself) {
  // A soldier of the march stands on the line another walks. Two walkers head
  // on at one speed would not do: each tests its step's end when the other is
  // exactly a stride away, and neither is ever in the way -- which is the
  // original's per-step test, and the reason this uses a stander.
  const auto walk_past = [](bool one_party, bool stander_marches) {
    Field field;
    const ObjectId hero = field.unit("Soldier", Point{100, 900});
    const ObjectId stander = field.unit("Soldier", Point{500, 500});
    const ObjectId walker = field.unit("Soldier", Point{100, 500});
    if (stander_marches) {
      // Its march order finds it already there: no route, but in the march.
      CHECK(field.movement.order_goto(field.world, stander, Point{500, 500}, 0, 0, hero) ==
            MoveOutcome::arrived);
    }
    field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0,
                              one_party ? hero : kNoObject);
    for (int i = 0; i < 12; ++i) field.world.advance(800);
    return field.movement.avoidance().blocked;
  };
  CHECK(walk_past(false, false) == 1);
  CHECK(walk_past(false, true) == 1);  // the stander marches, the walker does not
  CHECK(walk_past(true, false) == 1);  // the walker marches, the stander does not
  CHECK(walk_past(true, true) == 0);   // one march
}

TEST(the_dead_the_held_and_the_airborne_do_not_block) {
  const auto walk_past = [](int what) {
    Field field;
    const ObjectId obstacle = field.unit("Soldier", Point{500, 500});
    if (what == 1) CHECK(field.world.set_health(obstacle, 0));
    if (what == 2) {
      const ObjectId holder = field.world.spawn_internal(InternalKind::holder);
      CHECK(field.world.put_in_holder(obstacle, holder));
    }
    if (what == 3) field.world.mutable_state(obstacle)->flags.in_air = true;
    const ObjectId walker = field.unit("Soldier", Point{100, 500});
    field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
    for (int i = 0; i < 12; ++i) field.world.advance(800);
    CHECK(field.at(walker) == (Point{900, 500}));
    return field.movement.avoidance().blocked;
  };
  CHECK(walk_past(0) > 0);  // alive, on the ground, in the way
  CHECK(walk_past(1) == 0);
  CHECK(walk_past(2) == 0);
  CHECK(walk_past(3) == 0);
}

TEST(an_ownerless_lock_is_a_hard_block_measured_at_radius_zero_by_the_sidestep) {
  // A radius-40 lock 20 units off the walker's line at x = 500. The step ends
  // at x = 460, 500 and 540 are 44.7, 20 and 44.7 from it, all inside 15 + 40:
  // three blocks, all hard, so no draw. The sidestep then measures the lock by
  // the `DestLock` class's radius, 0: each step's own end overlaps it by
  // 15 - 20 or less, scores 0, and wins. So the walker gives way *onto the
  // points it was blocked at*, which is the original, and which scoring the
  // lock at 40 would have turned into a sidestep at x = 500.
  Field field;
  field.movement.set_static_locks({StaticLock{Point{500, 520}, 40}});
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
  const Rng before = field.world.rng();
  for (int i = 0; i < 12; ++i) field.world.advance(800);
  const MovementSystem::AvoidanceCounters& a = field.movement.avoidance();
  CHECK(a.blocked == 3);
  CHECK(a.holds == 0);
  CHECK(a.sidesteps == 0);
  CHECK(field.world.rng() == before);
  CHECK(field.at(walker) == (Point{900, 500}));
}

TEST(the_last_point_is_stepped_onto_regardless) {
  // Somebody stands on the destination. The last step is blocked, hard, and is
  // not sidestepped: the walker ends on the point it was sent to.
  Field field;
  const ObjectId stander = field.unit("Soldier", Point{900, 500});
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
  for (int i = 0; i < 12; ++i) field.world.advance(800);
  CHECK(field.at(walker) == (Point{900, 500}));
  CHECK(field.at(stander) == (Point{900, 500}));
  CHECK(field.movement.avoidance().blocked == 1);
  CHECK(field.movement.avoidance().sidesteps == 0);
  CHECK(field.move(walker).last_outcome == MoveOutcome::arrived);
}

TEST(a_stepped_route_stops_at_the_first_step_end_inside_its_range) {
  // From x = 100 towards 900 with a range of 100. The route runs to the
  // band's goal ring (`0x00417830`), whose point nearest the start is at 99
  // from the goal, x = 801 -- so it is 701 long, cut into 18 steps of about
  // 39, and no step end before the last is within 100 of the goal. Arrival is
  // asked before each step, as `CVXPathRetry` asks it, so the unit stops on
  // the ring point, wherever the turn boundaries fall. (A route to the goal
  // itself stepped 40 at a time and stopped at x = 820.)
  for (const std::int32_t length : {800, 300, 7}) {
    Field field;
    const ObjectId walker = field.unit("Soldier", Point{100, 500});
    field.movement.order_goto(field.world, walker, Point{900, 500}, 100);
    for (int t = 0; t < 12000; t += length) field.world.advance(length);
    CHECK(field.at(walker) == (Point{801, 500}));
    CHECK(field.move(walker).last_outcome == MoveOutcome::arrived);
  }
}

namespace {

/// A crowd: a column of three walking east in one file, 17 apart, so each
/// tests a step end its leader is standing near -- soft blocks; two walking
/// west on a lane 10 away, staggered the other way; a line of posts in the
/// eastbound lane, which never move and never stop being in the way; and a
/// walker further north into a stander and a lock -- hard ones.
struct Crowd {
  Field field;
  std::vector<ObjectId> units;
  Crowd() {
    field.world.rng().seed(0x5eed);
    field.movement.set_static_locks({StaticLock{Point{300, 705}, 40}});
    for (const std::int32_t x : {100, 83, 66}) {
      const ObjectId east = field.unit("Soldier", Point{x, 500});
      field.movement.order_goto(field.world, east, Point{900, 500}, 0);
      units.push_back(east);
    }
    for (const std::int32_t x : {900, 917}) {
      const ObjectId west = field.unit("Soldier", Point{x, 510});
      field.movement.order_goto(field.world, west, Point{100, 510}, 0);
      units.push_back(west);
    }
    for (const std::int32_t x : {400, 440}) {
      const ObjectId post = field.unit("Post", Point{x, 500});
      field.movement.order_goto(field.world, post, Point{x, 100}, 0);
      units.push_back(post);
    }
    units.push_back(field.unit("Soldier", Point{600, 700}));
    const ObjectId last = field.unit("Soldier", Point{100, 700});
    field.movement.order_goto(field.world, last, Point{900, 700}, 0);
    units.push_back(last);
  }
  std::uint64_t movement_hash() const {
    std::uint64_t h = 0;
    field.movement.hash(h);
    return h;
  }
};

}  // namespace

TEST(a_turn_partition_does_not_move_a_crowd) {
  Crowd whole;
  Crowd halves;
  Crowd quarters;
  Crowd ragged;
  for (int i = 0; i < 12; ++i) whole.field.world.advance(800);
  for (int i = 0; i < 24; ++i) halves.field.world.advance(400);
  for (int i = 0; i < 48; ++i) quarters.field.world.advance(200);
  for (const std::int32_t length : {799, 1, 400, 200, 200, 800, 1200, 1200, 3, 797, 2400, 1200,
                                    400}) {
    ragged.field.world.advance(length);
  }
  REQUIRE(whole.field.world.time() == ragged.field.world.time());

  // Not vacuous: the crowd really got in its own way, softly and hardly.
  const MovementSystem::AvoidanceCounters& reference = whole.field.movement.avoidance();
  CHECK(reference.blocked > 0);
  CHECK(reference.holds > 0);
  CHECK(reference.sidesteps > 0);

  for (Crowd* other : {&halves, &quarters, &ragged}) {
    for (std::size_t i = 0; i < whole.units.size(); ++i) {
      CHECK(other->field.at(other->units[i]) == whole.field.at(whole.units[i]));
    }
    CHECK(other->movement_hash() == whole.movement_hash());
    CHECK(other->field.world.rng() == whole.field.world.rng());
    const MovementSystem::AvoidanceCounters& a = other->field.movement.avoidance();
    CHECK(a.decisions == reference.decisions);
    CHECK(a.blocked == reference.blocked);
    CHECK(a.holds == reference.holds);
    CHECK(a.sidesteps == reference.sidesteps);
  }
}

TEST(the_step_state_is_not_hashed) {
  // Path media, the `pathfinder` channel: none of it goes into the hash.
  Crowd crowd;
  for (int i = 0; i < 3; ++i) crowd.field.world.advance(400);
  const std::uint64_t before = crowd.movement_hash();
  MoveState& m = crowd.field.movement.state(crowd.units.front());
  m.holding = !m.holding;
  m.hold_until += 17;
  m.retry_time = 75;
  m.step_start += 3;
  m.step_end += 5;
  m.offset_from = Point{4, -4};
  m.offset_to = Point{-9, 9};
  m.stride += 1;
  m.party = 12345;
  CHECK(crowd.movement_hash() == before);
}

TEST(a_save_taken_mid_step_resumes_exactly) {
  // Two identical crowds. One is saved and loaded back into itself at a turn
  // boundary where somebody is holding and somebody is mid-sidestep; both then
  // run on. Any field the section drops comes back as its default and moves a
  // unit somewhere the uninterrupted crowd did not.
  Crowd kept;
  Crowd reloaded;
  bool holding = false;
  bool drifting = false;
  int turns = 0;
  for (; turns < 600 && !(holding && drifting); ++turns) {
    kept.field.world.advance(20);
    reloaded.field.world.advance(20);
    holding = false;
    drifting = false;
    for (const ObjectId id : reloaded.units) {
      const MoveState* m = reloaded.field.movement.find(id);
      if (m == nullptr || !m->has_path) continue;
      if (m->holding) holding = true;
      // Walking back from a sidestep: the start of the step is off the route.
      if (!m->holding && m->offset_from != Point{}) drifting = true;
    }
  }
  REQUIRE(holding);
  REQUIRE(drifting);

  std::vector<std::byte> bytes;
  reloaded.field.movement.serialize(bytes);
  // Knock every avoidance field off before the load, so that one the section
  // failed to carry cannot survive by having been left alone.
  for (const ObjectId id : reloaded.units) {
    MoveState* m = reloaded.field.movement.find(id);
    if (m == nullptr) continue;
    m->holding = false;
    m->hold_until = 0;
    m->retry_time = 0;
    m->step_start = 0;
    m->step_end = 0;
    m->offset_from = Point{};
    m->offset_to = Point{};
    m->stride = 0;
    m->party = kNoObject;
  }
  reloaded.field.movement.set_static_locks({});
  REQUIRE(reloaded.field.movement.deserialize(bytes).ok());

  // Compared after every turn, not only at the end: by the end everybody has
  // arrived, and a unit that came back a few units off its sidestep would be
  // back on its route long before then.
  for (int i = 0; i < 200; ++i) {
    kept.field.world.advance(20);
    reloaded.field.world.advance(20);
    for (std::size_t u = 0; u < kept.units.size(); ++u) {
      CHECK(reloaded.field.at(reloaded.units[u]) == kept.field.at(kept.units[u]));
    }
  }
  for (int i = 0; i < 60; ++i) {
    kept.field.world.advance(400);
    reloaded.field.world.advance(400);
  }
  for (std::size_t i = 0; i < kept.units.size(); ++i) {
    CHECK(reloaded.field.at(reloaded.units[i]) == kept.field.at(kept.units[i]));
  }
  CHECK(reloaded.field.world.rng() == kept.field.world.rng());
  CHECK(reloaded.field.movement.static_locks() == kept.field.movement.static_locks());
}

TEST(a_march_keeps_its_party_through_a_save) {
  Field field;
  const ObjectId hero = field.unit("Soldier", Point{100, 100});
  const ObjectId member = field.unit("Soldier", Point{100, 200});
  field.movement.order_goto(field.world, member, Point{900, 200}, 0, 0, hero);
  std::vector<std::byte> bytes;
  field.movement.serialize(bytes);
  MovementSystem fresh;
  REQUIRE(fresh.deserialize(bytes).ok());
  REQUIRE(fresh.find(member) != nullptr);
  CHECK(fresh.find(member)->party == hero);
  CHECK(fresh.find(member)->stride == 200);
}

namespace {

/// A grove carrying one point of each lock type, and one that is not a lock.
constexpr std::string_view kGroveEntity =
    "<entity name=\"grove\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"9\" x=\"-100\" y=\"0\"/>"
    "<point idx=\"1\" type=\"10\" x=\"100\" y=\"0\"/>"
    "<point idx=\"2\" type=\"11\" x=\"0\" y=\"40\"/>"
    "<point idx=\"3\" type=\"12\" x=\"0\" y=\"-40\"/>"
    "</points>"
    "</entity>";

std::vector<std::byte> flat_height(std::uint32_t extent) {
  imperivm::test::Builder out;
  const std::uint32_t cells = extent / 32;
  out.text(kGridMagic).u32(32).u32(8).u32(extent).u32(extent);
  for (std::uint32_t i = 0; i < cells * cells; ++i) out.u8(0);
  return {out.span().begin(), out.span().end()};
}

}  // namespace

TEST(the_ownerless_locks_come_from_point_types_nine_ten_and_eleven) {
  Field field(128, /*start=*/false);
  const std::vector<std::byte> height_bytes = flat_height(2048);
  const Result<Grid> height = Grid::parse(height_bytes);
  REQUIRE(height.ok());
  field.world.set_height(height.value());
  const Result<Entity> grove_entity = Entity::parse(bytes_of(kGroveEntity));
  REQUIRE(grove_entity.ok());
  const ObjectId grove =
      field.world.spawn(NativeClass::decor, &grove_entity.value(), field.graph.find("Grove"));
  CHECK(field.world.set_position(grove, Point{1000, 1000}));
  // A spawn template is not in play and makes none.
  const ObjectId templ =
      field.world.spawn(NativeClass::decor, &grove_entity.value(), field.graph.find("Grove"));
  CHECK(field.world.set_position(templ, Point{400, 1000}));
  field.world.mutable_state(templ)->flags.unspawned = true;

  field.world.start();

  // What the round trip puts each point at, turned into locks by type.
  std::vector<StaticLock> expected;
  for (const std::int32_t type : {9, 10, 11}) {
    std::vector<Point> points;
    class_points_of_type(field.world, grove, type, points);
    REQUIRE(points.size() == 1);
    locks_of_point(points[0], type, expected);
  }
  REQUIRE(expected.size() == 8);
  CHECK(field.movement.static_locks() == expected);

  // Once a match: a second start does not make them again, and a save carries
  // both the locks and the fact that they were made.
  field.movement.start(field.world);
  CHECK(field.movement.static_locks().size() == 8);
  std::vector<std::byte> bytes;
  field.movement.serialize(bytes);
  MovementSystem fresh;
  REQUIRE(fresh.deserialize(bytes).ok());
  CHECK(fresh.static_locks() == expected);
  World empty;
  fresh.start(empty);  // an empty world would build nothing, and must not try
  CHECK(fresh.static_locks() == expected);
}

// --------------------------------------------------------------------------
// what the first fault sweep found missing
// --------------------------------------------------------------------------

TEST(a_hold_spends_the_step_it_tested) {
  // One post on the line at x = 300. Tested from x = 260 at t = 1600, it is a
  // soft block: a hold of 50. The sampled route has already moved on, so at
  // t = 1650 the test is at x = 340, which is free -- and the walker walks the
  // two strides from 260 to 340 in one step, **straight through the post**.
  // That is the original's, and it is what a hold that did not spend its step
  // would get wrong: it would test 300 again and climb to 75.
  Field field;
  const std::uint32_t seed = seed_without_early_give_way(2);
  field.world.rng().seed(seed);
  const ObjectId first = field.unit("Post", Point{300, 500});
  field.movement.order_goto(field.world, first, Point{300, 100}, 0);
  const ObjectId second = field.unit("Post", Point{700, 500});
  field.movement.order_goto(field.world, second, Point{700, 100}, 0);
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);

  std::vector<std::int32_t> holds;
  GameTime last_hold = -1;
  std::int64_t through = dist_sq(field.at(walker), Point{300, 500});
  for (int ms = 1; ms <= 9000; ++ms) {
    field.world.advance(1);
    const MoveState& m = field.move(walker);
    if (m.holding && m.hold_until != last_hold) {
      holds.push_back(static_cast<std::int32_t>(m.hold_until - field.world.time()));
      last_hold = m.hold_until;
    }
    through = std::min(through, dist_sq(field.at(walker), Point{300, 500}));
    // Resumed at 1650 exactly, not a millisecond late: five units by 1700.
    if (ms == 1700) CHECK(field.at(walker) == (Point{265, 500}));
  }
  CHECK(through <= 1);
  // The second post is a *first* hold again: every free step resets the wait.
  const std::vector<std::int32_t> expected = {50, 50};
  CHECK(holds == expected);
  CHECK(field.at(walker) == (Point{900, 500}));
}

TEST(a_short_stride_does_not_trip_over_its_own_feet) {
  // Radius 15, stride 25: every step's end is nearer the walker than the 30
  // its own radius and anybody's would sum to. It is not anybody.
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.state(walker).walk_anim = 13;
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
  REQUIRE(field.move(walker).stride == 25);
  for (int i = 0; i < 12; ++i) field.world.advance(800);
  CHECK(field.at(walker) == (Point{900, 500}));
  CHECK(field.movement.avoidance().decisions > 0);
  CHECK(field.movement.avoidance().blocked == 0);
}

TEST(two_marches_block_each_other) {
  // A walker of one hero's march and a stander of another's: not one march.
  Field field;
  const ObjectId hero_a = field.unit("Soldier", Point{100, 900});
  const ObjectId hero_b = field.unit("Soldier", Point{200, 900});
  const ObjectId stander = field.unit("Soldier", Point{500, 500});
  CHECK(field.movement.order_goto(field.world, stander, Point{500, 500}, 0, 0, hero_b) ==
        MoveOutcome::arrived);
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0, hero_a);
  for (int i = 0; i < 12; ++i) field.world.advance(800);
  CHECK(field.movement.avoidance().blocked == 1);
}

TEST(what_the_step_does_not_see) {
  // Every case puts something by the step ending at (500, 500) that the
  // original's test leaves out, and the walker must go by untroubled.
  const auto walk_past = [](int what) {
    Field field;
    switch (what) {
      case 0: {
        // Radius 60, 60 away: inside the summed 75, outside the query's
        // 15 + 40. Not seen, so not in the way.
        (void)field.unit("Big", Point{500, 560});
        break;
      }
      case 1:
        field.movement.set_static_locks({StaticLock{Point{500, 560}, 60}});
        break;
      case 2:
        // Exactly the summed radii away: touching is not overlapping.
        (void)field.unit("Soldier", Point{500, 530});
        break;
      case 3: {
        // Not a unit at all.
        const ObjectId grove =
            field.world.spawn(NativeClass::decor, nullptr, field.graph.find("Grove"));
        CHECK(field.world.set_position(grove, Point{500, 500}));
        CHECK(field.world.set_health(grove, 100));
        break;
      }
      case 4: {
        // A spawn template: placed, holding a handle, not in play.
        const ObjectId templ = field.unit("Soldier", Point{500, 500});
        field.world.mutable_state(templ)->flags.unspawned = true;
        break;
      }
      default:
        break;
    }
    const ObjectId walker = field.unit("Soldier", Point{100, 500});
    field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
    for (int i = 0; i < 12; ++i) field.world.advance(800);
    CHECK(field.at(walker) == (Point{900, 500}));
    return field.movement.avoidance().blocked;
  };
  for (int what = 0; what <= 4; ++what) CHECK(walk_past(what) == 0);
}

TEST(a_mover_taken_off_the_ground_stops_deciding) {
  // Put in a holder, or taken into the air, a unit's route stops asking.
  for (const bool held : {true, false}) {
    Field field;
    const ObjectId walker = field.unit("Soldier", Point{100, 500});
    field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
    field.world.advance(800);
    if (held) {
      const ObjectId holder = field.world.spawn_internal(InternalKind::holder);
      CHECK(field.world.put_in_holder(walker, holder));
    } else {
      field.world.mutable_state(walker)->flags.in_air = true;
    }
    const std::uint64_t before = field.movement.avoidance().decisions;
    for (int i = 0; i < 4; ++i) field.world.advance(800);
    CHECK(field.movement.avoidance().decisions == before);
  }
}

TEST(a_blocker_is_where_it_is_at_the_instant_not_at_the_turns_start) {
  // One long turn of 8000. A crosser walks south down x = 500 from y = 120; at
  // t = 3600 it is at (500, 480), and the walker, then at x = 460, tests its
  // step's end at (500, 500): 20 away, blocked. At the turn's start the
  // crosser was 380 away, several buckets off -- so this also holds the
  // search to how far a body can have come since.
  for (const bool crosser_steps : {true, false}) {
    Field field;
    const ObjectId crosser =
        crosser_steps
            ? field.unit("Soldier", Point{500, 120})
            : field.world.spawn(NativeClass::unit, nullptr, field.graph.find("Soldier"));
    if (!crosser_steps) {
      CHECK(field.world.set_position(crosser, Point{500, 120}));
      CHECK(field.world.set_health(crosser, 100));
    }
    field.movement.order_goto(field.world, crosser, Point{500, 900}, 0);
    CHECK((field.move(crosser).stride > 0) == crosser_steps);
    const ObjectId walker = field.unit("Soldier", Point{100, 500});
    field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
    field.world.advance(8000);
    CHECK(field.movement.avoidance().blocked >= 1);
  }
}

TEST(a_holding_blocker_stands_still) {
  // The walker of the escalation case holds at x = 260 from t = 1600 to 1825.
  // A second walker, set off from x = 154 at t = 1360, cuts its 746-unit leg
  // into 19 steps and tests the second one's end, (232, 500), at t = 1750: 28
  // from the holder, blocked. Had the holder been read as still walking since
  // its last decision at 1725, it would have been 2.5 further on and 30 away.
  Field field;
  const std::uint32_t seed = seed_without_early_give_way(5);
  field.world.rng().seed(seed);
  for (const std::int32_t x : {300, 340, 380, 420, 460}) {
    const ObjectId post = field.unit("Post", Point{x, 500});
    field.movement.order_goto(field.world, post, Point{x, 100}, 0);
  }
  const ObjectId leader = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, leader, Point{900, 500}, 0);
  for (int i = 0; i < 1360; ++i) field.world.advance(1);
  const ObjectId follower = field.unit("Soldier", Point{154, 500});
  field.movement.order_goto(field.world, follower, Point{900, 500}, 0);
  // Millisecond turns up to the holder's decision at 1725, then one turn of
  // 40: the follower asks at 1750, 25 ms into it.
  for (int i = 1360; i < 1725; ++i) field.world.advance(1);
  field.world.advance(40);
  CHECK(field.move(leader).holding);
  CHECK(field.move(follower).holding);
  CHECK(field.move(follower).hold_until == 1800);
}

TEST(same_instant_decisions_go_in_id_order) {
  // Two identical lanes, blocked at the same instant. The first draw goes to
  // the lower id: under a seed whose draws are (not 1, 1), the lower-id walker
  // holds and the higher gives way early.
  std::uint32_t seed = 1;
  for (;; ++seed) {
    Rng probe(seed);
    if (probe.between(1, 7) != 1 && probe.between(1, 7) == 1) break;
  }
  Field field;
  field.world.rng().seed(seed);
  for (const std::int32_t y : {500, 900}) {
    const ObjectId post = field.unit("Post", Point{300, y});
    field.movement.order_goto(field.world, post, Point{300, y - 400}, 0);
  }
  const ObjectId low = field.unit("Soldier", Point{100, 500});
  const ObjectId high = field.unit("Soldier", Point{100, 900});
  field.movement.order_goto(field.world, low, Point{900, 500}, 0);
  field.movement.order_goto(field.world, high, Point{900, 900}, 0);
  for (int i = 0; i < 1601; ++i) field.world.advance(1);
  CHECK(field.move(low).holding);
  CHECK(!field.move(high).holding);
  CHECK(field.movement.avoidance().early == 1);
}

TEST(a_step_starts_the_moment_its_route_is_laid) {
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
  field.world.advance(400);
  // Forty units in the first 400: the first step was decided at t = 0.
  CHECK(field.at(walker) == (Point{140, 500}));
  CHECK(field.move(walker).last_moved == 400);
  // And the route's end is reached on the last instant of a single long turn,
  // and counted as arrival in that turn rather than the next.
  field.world.advance(7600);
  CHECK(field.at(walker) == (Point{900, 500}));
  CHECK(!field.move(walker).has_path);
  CHECK(!field.world.state(walker)->flags.has_active_path);
}

TEST(a_follower_re_laid_at_a_turns_start_steps_at_once) {
  // Following a walker that is always more than a repath threshold ahead, the
  // follower's route is re-laid at the start of a turn, and its first step is
  // decided then: forty units in every 400, re-laid or not.
  Field field;
  const ObjectId leader = field.unit("Soldier", Point{700, 300});
  field.movement.order_goto(field.world, leader, Point{1900, 300}, 0);
  const ObjectId follower = field.unit("Soldier", Point{100, 300});
  field.movement.order_goto_object(field.world, follower, leader, 0);
  for (int i = 0; i < 10; ++i) {
    const Point before = field.at(follower);
    field.world.advance(400);
    CHECK(field.at(follower) == (Point{before.x + 40, 300}));
  }
}

TEST(a_water_unit_gives_way_onto_water) {
  // Terrain column 3 (x 192..255) is deep water. Two boats: one standing at
  // (192, 460), one sailing north up x = 192 from y = 900. Blocked by the
  // standing boat, it gives way across the water -- which a land unit could
  // not, every candidate being wet.
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(64).u32(8).u32(2048).u32(2048);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) out.u8(x == 3 ? 13 : 1);
  }
  const std::vector<std::byte> bytes(out.span().begin(), out.span().end());
  const Result<Grid> terrain = Grid::parse(bytes);
  REQUIRE(terrain.ok());
  Field field;
  field.world.set_terrain(terrain.value());
  (void)field.unit("Boat", Point{192, 460});
  const ObjectId sailor = field.unit("Boat", Point{192, 900});
  field.movement.order_goto(field.world, sailor, Point{192, 100}, 0);
  for (int i = 0; i < 12; ++i) field.world.advance(800);
  CHECK(field.movement.avoidance().blocked >= 1);
  CHECK(field.movement.avoidance().sidesteps >= 1);
}

TEST(an_object_order_leaves_the_march) {
  Field field;
  const ObjectId hero = field.unit("Soldier", Point{100, 100});
  const ObjectId member = field.unit("Soldier", Point{100, 200});
  field.movement.order_goto(field.world, member, Point{900, 200}, 0, 0, hero);
  REQUIRE(field.move(member).party == hero);
  field.movement.order_goto_object(field.world, member, hero, 50);
  CHECK(field.move(member).party == kNoObject);
}

TEST(locks_that_arrive_after_a_turn_are_seen) {
  // The lock buckets are built on the first turn that needs them; locks that
  // arrive afterwards -- set, loaded, or made -- must be seen from the next.
  const auto blocked_by_late_lock = [](int how) {
    Field field(128, /*start=*/false);
    const std::vector<std::byte> height_bytes = flat_height(2048);
    const Result<Grid> height = Grid::parse(height_bytes);
    CHECK(height.ok());
    field.world.set_height(height.value());
    field.world.start();
    const ObjectId early = field.unit("Soldier", Point{100, 1500});
    field.movement.order_goto(field.world, early, Point{900, 1500}, 0);
    field.world.advance(400);  // buckets built, with no locks in them
    const std::vector<StaticLock> late = {StaticLock{Point{500, 500}, 40}};
    if (how == 0) field.movement.set_static_locks(late);
    if (how == 1) {
      MovementSystem donor;
      donor.set_static_locks(late);
      std::vector<std::byte> bytes;
      donor.serialize(bytes);
      CHECK(field.movement.deserialize(bytes).ok());
    }
    if (how == 2) {
      const Result<Entity> grove_entity = Entity::parse(bytes_of(kGroveEntity));
      CHECK(grove_entity.ok());
      const ObjectId grove =
          field.world.spawn(NativeClass::decor, &grove_entity.value(), field.graph.find("Grove"));
      CHECK(field.world.set_position(grove, Point{500, 470}));
      field.movement.build_static_locks(field.world);
      CHECK(!field.movement.static_locks().empty());
    }
    const ObjectId walker = field.unit("Soldier", Point{100, 500});
    field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
    for (int i = 0; i < 12; ++i) field.world.advance(800);
    return field.movement.avoidance().blocked;
  };
  CHECK(blocked_by_late_lock(0) >= 1);
  CHECK(blocked_by_late_lock(1) >= 1);
  CHECK(blocked_by_late_lock(2) >= 1);
}

TEST(a_save_taken_mid_hold_keeps_its_wait) {
  // The escalation case saved at t = 1601, holding its first 50. Loaded back
  // with every avoidance field knocked off first, it must go on to 75 and 100,
  // not start again at 50.
  Field field;
  const std::uint32_t seed = seed_without_early_give_way(5);
  field.world.rng().seed(seed);
  for (const std::int32_t x : {300, 340, 380, 420, 460}) {
    const ObjectId post = field.unit("Post", Point{x, 500});
    field.movement.order_goto(field.world, post, Point{x, 100}, 0);
  }
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0);
  for (int i = 0; i < 1601; ++i) field.world.advance(1);
  REQUIRE(field.move(walker).holding);
  REQUIRE(field.move(walker).retry_time == 50);
  std::vector<std::byte> bytes;
  field.movement.serialize(bytes);
  MoveState& m = field.movement.state(walker);
  m.retry_time = 0;
  m.holding = false;
  m.hold_until = 0;
  m.step_end = 0;
  REQUIRE(field.movement.deserialize(bytes).ok());
  std::vector<std::int32_t> holds;
  GameTime last_hold = field.move(walker).hold_until;
  for (int i = 0; i < 400; ++i) {
    field.world.advance(1);
    const MoveState& now = field.move(walker);
    if (now.holding && now.hold_until != last_hold) {
      holds.push_back(static_cast<std::int32_t>(now.hold_until - field.world.time()));
      last_hold = now.hold_until;
    }
  }
  const std::vector<std::int32_t> expected = {75, 100};
  CHECK(holds == expected);
}

// ---------------------------------------------------------------------------
// destination locks: playtest report #13
// ---------------------------------------------------------------------------

namespace {

/// Every pair of `units` nearer than their summed radii (15 each here): the
/// free-spot test's own "taken" (`0x0040a96f`), with no tolerance.
int overlapping_pairs(Field& field, const std::vector<ObjectId>& units) {
  int pairs = 0;
  for (std::size_t i = 0; i < units.size(); ++i) {
    for (std::size_t j = i + 1; j < units.size(); ++j) {
      if (dist_sq(field.at(units[i]), field.at(units[j])) < 30 * 30) ++pairs;
    }
  }
  return pairs;
}

/// Six soldiers of one march, in a column abreast at x = 100, all sent to one
/// point. `lock` is `SetDest`'s flag, as `Goto` passes it.
std::vector<ObjectId> march_onto_one_point(Field& field, bool lock) {
  const ObjectId hero = field.unit("Soldier", Point{100, 900});
  std::vector<ObjectId> units;
  for (std::int32_t i = 0; i < 6; ++i) units.push_back(field.unit("Soldier", Point{100, 400 + 40 * i}));
  for (const ObjectId id : units) {
    field.movement.order_goto(field.world, id, Point{900, 500}, 0, 0, hero, lock);
  }
  for (int t = 0; t < 40; ++t) field.world.advance(800);
  return units;
}

}  // namespace

TEST(a_march_sent_onto_one_point_ends_on_as_many_free_spots) {
  // One march does not block itself on the way (`0x005d2f80`), and before the
  // locks nothing else kept it apart at the goal: all six stood on (900,500).
  // Each route now ends on a free spot, reserved by the owner's lock, and the
  // free-spot test (`0x004094d0`) asks nothing about marches.
  Field field;
  const std::vector<ObjectId> units = march_onto_one_point(field, true);
  std::vector<Point> spots;
  for (const ObjectId id : units) {
    CHECK(!field.move(id).has_path);
    spots.push_back(field.at(id));
  }
  for (std::size_t i = 0; i < spots.size(); ++i) {
    for (std::size_t j = i + 1; j < spots.size(); ++j) CHECK(spots[i] != spots[j]);
  }
  CHECK(overlapping_pairs(field, units) == 0);
  // The first laid takes the point itself; the rest stand short of it.
  CHECK(field.at(units.front()) == (Point{900, 500}));
  CHECK(field.move(units.front()).last_outcome == MoveOutcome::arrived);
  // Every one of them is within reach of the point: a queue, not a scatter.
  for (const ObjectId id : units) CHECK(within(field.at(id), Point{900, 500}, 6 * 30));
}

TEST(without_the_lock_flag_a_march_still_stacks_on_its_goal) {
  // The same march ordered the way `GotoEnter` and a formation march order
  // (flag 0): no lock, no free-spot test, and the six share one point. This
  // is what the test above would see if the locks did nothing.
  Field field;
  const std::vector<ObjectId> units = march_onto_one_point(field, false);
  for (const ObjectId id : units) CHECK(field.at(id) == (Point{900, 500}));
  CHECK(overlapping_pairs(field, units) == 15);
}

TEST(attackers_sent_at_one_target_end_on_distinct_spots_around_it) {
  // Six soldiers from three sides at one standing post, with `GotoAttack`'s
  // band: weapon reach 10 plus both radii. Each route runs to the band's goal
  // ring point nearest the attacker (`0x00417830`) and is cut back to a free
  // spot (`0x004141bd`); none ends on the post or on another attacker.
  Field field;
  const ObjectId post = field.unit("Post", Point{900, 900});
  std::vector<ObjectId> units;
  const Point starts[] = {{100, 880}, {100, 920}, {1700, 880}, {1700, 920}, {900, 100}, {880, 100}};
  for (const Point p : starts) units.push_back(field.unit("Soldier", p));
  for (const ObjectId id : units) {
    field.movement.order_goto_object(field.world, id, post, 10 + 15 + 15, 0, true);
  }
  for (int t = 0; t < 60; ++t) field.world.advance(800);
  std::vector<ObjectId> bodies = units;
  bodies.push_back(post);
  CHECK(overlapping_pairs(field, bodies) == 0);
  // One from each side reaches the band; the post never moved.
  int arrived = 0;
  for (const ObjectId id : units) {
    if (field.move(id).last_outcome == MoveOutcome::arrived) ++arrived;
  }
  CHECK(arrived >= 3);
  CHECK(field.at(post) == (Point{900, 900}));
}

TEST(a_unit_is_not_arrived_on_a_taken_spot) {
  // `IsArrived` (`0x00417c10`): inside the band is not enough for an order
  // that owns a lock. A soldier already standing within range, on top of
  // another, walks to the ring point nearest it rather than calling it done.
  Field field;
  const ObjectId stander = field.unit("Post", Point{500, 500});
  const ObjectId walker = field.unit("Soldier", Point{505, 500});
  CHECK(field.movement.order_goto(field.world, walker, Point{500, 500}, 200, 0, kNoObject,
                                  true) != MoveOutcome::arrived);
  for (int t = 0; t < 20; ++t) field.world.advance(800);
  CHECK(dist_sq(field.at(walker), field.at(stander)) >= 30 * 30);
  CHECK(field.move(walker).last_outcome == MoveOutcome::arrived);
  // Without the flag it is arrived where it stands.
  Field plain;
  plain.unit("Post", Point{500, 500});
  const ObjectId lazy = plain.unit("Soldier", Point{505, 500});
  CHECK(plain.movement.order_goto(plain.world, lazy, Point{500, 500}, 200) ==
        MoveOutcome::arrived);
}

TEST(a_lock_is_the_route_end_and_goes_with_the_route) {
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0, kNoObject, true);
  StaticLock lock;
  REQUIRE(field.movement.owned_lock(field.world, walker, lock));
  CHECK(lock.at == (Point{900, 500}));
  CHECK(lock.radius == 15);
  // Another unit may not arrive on it, but the owner's own lock is no obstacle.
  const ObjectId other = field.unit("Soldier", Point{300, 300});
  CHECK(!field.movement.spot_free(field.world, other, Point{910, 500}, 15));
  CHECK(field.movement.spot_free(field.world, walker, Point{910, 500}, 15));
  CHECK(field.movement.spot_free(field.world, other, Point{930, 500}, 15));
  field.movement.stop(field.world, walker);
  CHECK(!field.movement.owned_lock(field.world, walker, lock));
  CHECK(field.movement.spot_free(field.world, other, Point{910, 500}, 15));
}

TEST(a_class_that_ignores_passability_owns_no_lock) {
  // `0x004178d0` and `SetMedia` both refuse a lock to `[class+0x31c]`: a
  // sentry walking its wall reserves nothing and is never refused arrival.
  Field field;
  field.graph.add(bytes_of(R"(<class id="Sentry" cpp_class="CVXUnit" parent="">
      <properties speed="100" radius="15" ignore_passability="1"/></class>)"),
                  "sentry.sc.xml");
  field.graph.link();
  const ObjectId sentry = field.unit("Sentry", Point{100, 500});
  field.movement.order_goto(field.world, sentry, Point{900, 500}, 0, 0, kNoObject, true);
  CHECK(!field.move(sentry).dest_lock);
  StaticLock lock;
  CHECK(!field.movement.owned_lock(field.world, sentry, lock));
}

TEST(the_lock_flag_survives_a_save_and_is_not_hashed) {
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0, kNoObject, true);
  std::uint64_t with = 0;
  field.movement.hash(with);
  std::vector<std::byte> bytes;
  field.movement.serialize(bytes);
  field.movement.state(walker).dest_lock = false;
  std::uint64_t without = 0;
  field.movement.hash(without);
  CHECK(with == without);
  REQUIRE(field.movement.deserialize(bytes).ok());
  CHECK(field.move(walker).dest_lock);
}

TEST(a_band_route_ends_on_its_goal_ring) {
  // `0x00417830`: rings of sixteen at `r - 1`, `r` stepping 40 down to the
  // band's inner edge. Due west of the goal the point is (x - (r - 1), y);
  // the ninth direction's cosine is -0.99999873..., which `_ftol` truncates
  // towards zero, and its sine puts y a sixth of a unit off, truncated away.
  Field field;
  CHECK(field.movement.ring_goal(field.world, kNoObject, Point{100, 500}, Point{900, 500}, 100, 0).at ==
        (Point{801, 500}));
  // Due north-east, the third direction: (cos, sin)(0.785) of 99.
  CHECK(field.movement.ring_goal(field.world, kNoObject, Point{2000, 2000}, Point{900, 500}, 100, 0).at ==
        (Point{970, 569}));
  // A range above 1,000 is 1,000 (`SetDest`).
  CHECK(field.movement.ring_goal(field.world, kNoObject, Point{100, 500}, Point{1900, 500}, 5000, 0).at ==
        (Point{901, 501}));
  // No range, no ring: the goal itself.
  CHECK(field.movement.ring_goal(field.world, kNoObject, Point{100, 500}, Point{900, 500}, 0, 0).at ==
        (Point{900, 500}));
}

TEST(the_free_spot_test_counts_standing_units_and_every_lock_but_not_movers) {
  // `0x004094d0`: a unit with an active path is no obstacle to a spot, a
  // standing one is, and so is an ownerless lock (`[lock+0x70] == 0`).
  Field field;
  const ObjectId asker = field.unit("Soldier", Point{100, 100});
  const ObjectId mover = field.unit("Soldier", Point{500, 500});
  field.movement.order_goto(field.world, mover, Point{1500, 500}, 0);
  REQUIRE(field.move(mover).has_path);
  CHECK(field.movement.spot_free(field.world, asker, Point{500, 500}, 15));
  field.movement.stop(field.world, mover);
  CHECK(!field.movement.spot_free(field.world, asker, Point{500, 500}, 15));
  field.movement.set_static_locks({StaticLock{Point{800, 800}, 40}});
  CHECK(!field.movement.spot_free(field.world, asker, Point{850, 800}, 15));
  CHECK(field.movement.spot_free(field.world, asker, Point{855, 800}, 15));
}

TEST(a_route_cut_back_never_ends_within_a_cell_of_its_start) {
  // The cut stops before the start's cell (`0x00414255`): from x = 100 to a
  // taken x = 140, the points at 40 and 24 along are taken and the one at 8
  // is inside the first cell, so the unit stays where it is, not arrived.
  Field field;
  field.unit("Post", Point{140, 500});
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{140, 500}, 0, 0, kNoObject, true);
  for (int t = 0; t < 5; ++t) field.world.advance(800);
  CHECK(field.at(walker) == (Point{100, 500}));
  CHECK(field.move(walker).last_outcome != MoveOutcome::arrived);
}

TEST(a_route_whose_end_is_taken_on_the_way_is_not_arrived) {
  // Free when laid, taken by the time it is reached: `IsArrived` refuses it
  // and the route is laid again (`RecastPathfind`), which from a spot that is
  // itself the taken end has nowhere to go.
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0, kNoObject, true);
  field.world.advance(800);
  field.unit("Post", Point{900, 500});
  for (int t = 0; t < 20; ++t) field.world.advance(800);
  CHECK(!field.move(walker).has_path);
  CHECK(field.move(walker).last_outcome != MoveOutcome::arrived);
}

// ---------------------------------------------------------------------------
// going round a taken side: the struck goal points and the free-spot search
// ---------------------------------------------------------------------------

namespace {

/// The post every test below attacks, and `GotoAttack`'s band round it:
/// weapon reach 10 plus both radii. Its goal rings are sixteen points at 39
/// and the post's own centre (`r = 0`).
constexpr Point kTarget{900, 900};
constexpr std::int32_t kBand = 10 + 15 + 15;

/// A post on every second point of the 39 ring: each strikes the points
/// either side of it, 15.3 away, so none of the band's goal points is left.
std::vector<ObjectId> ring_of_posts(Field& field) {
  std::vector<ObjectId> posts;
  const Point ring[] = {{939, 900}, {927, 927}, {900, 939}, {872, 927},
                        {861, 900}, {872, 872}, {900, 861}, {927, 872}};
  for (const Point p : ring) posts.push_back(field.unit("Post", p));
  return posts;
}

/// A seed whose first `rand(0, 359)` heads east, away from the column of
/// posts west of the target: the step is `(r sin a, r cos a)`, mostly `+x`
/// for `a` in 60..120. Searched for, so the test survives the generator
/// being replaced.
std::uint32_t seed_heading_east() {
  for (std::uint32_t seed = 1;; ++seed) {
    Rng probe(seed);
    const std::int32_t heading = probe.between(0, 359);
    if (heading >= 60 && heading <= 120) return seed;
  }
}

}  // namespace

TEST(an_attacker_whose_side_is_taken_goes_round_to_a_free_spot) {
  // A post stands on the band's west point, and strikes off the goal points
  // within 30 of it (`0x0040a310`): the search runs to the nearest point
  // left, round the side, and the attacker arrives there. Before the goal
  // points were struck it ran to the taken west point, was cut back to the
  // free spot behind the post, and waited there out of reach.
  Field field;
  const ObjectId post = field.unit("Post", kTarget);
  const ObjectId blocker = field.unit("Post", Point{861, 900});
  const ObjectId attacker = field.unit("Soldier", Point{100, 900});
  field.movement.order_goto_object(field.world, attacker, post, kBand, 0, true);
  REQUIRE(field.move(attacker).has_path);
  const Point end = field.move(attacker).waypoints.back();
  CHECK(end.y != 900);
  CHECK(within(end, kTarget, kBand));
  for (int t = 0; t < 30; ++t) field.world.advance(800);
  CHECK(field.move(attacker).last_outcome == MoveOutcome::arrived);
  CHECK(within(field.at(attacker), kTarget, kBand));
  CHECK(overlapping_pairs(field, {post, blocker, attacker}) == 0);
  // No search was short of a goal point, so nothing was drawn for one.
  CHECK(!field.move(attacker).free_spot_tried);
}

TEST(a_goal_point_is_struck_by_a_body_at_the_two_radii) {
  // `0x00409650` strikes a goal point at the summed radii too, where the
  // free-spot test wants nearer. A post 30 west of the west ring point
  // (861,900) strikes it, and the route runs elsewhere; one a unit further
  // does not.
  Field touching;
  touching.unit("Post", Point{831, 900});
  const ObjectId near_asker = touching.unit("Soldier", Point{100, 900});
  const MovementSystem::RingGoal near =
      touching.movement.ring_goal(touching.world, near_asker, Point{100, 900}, kTarget, kBand, 0);
  CHECK(near.free);
  CHECK(near.at != (Point{861, 900}));
  Field clear;
  clear.unit("Post", Point{830, 900});
  const ObjectId far_asker = clear.unit("Soldier", Point{100, 900});
  const MovementSystem::RingGoal far =
      clear.movement.ring_goal(clear.world, far_asker, Point{100, 900}, kTarget, kBand, 0);
  CHECK(far.free);
  CHECK(far.at == (Point{861, 900}));
  // Without an owner -- an order that holds no lock -- nothing is struck.
  CHECK(touching.movement.ring_goal(touching.world, kNoObject, Point{100, 900}, kTarget, kBand, 0)
            .at == (Point{861, 900}));
}

TEST(a_band_with_every_goal_point_taken_heads_for_the_point_nearest_its_centre) {
  // Status 1: nothing is left. The search still heads somewhere: the goal
  // point nearest the centre as they were added (`[0x008bfc10]`), which for
  // a band whose rings reach 0 is the centre itself.
  Field field;
  field.unit("Post", kTarget);
  ring_of_posts(field);
  const ObjectId attacker = field.unit("Soldier", Point{100, 900});
  const MovementSystem::RingGoal goal =
      field.movement.ring_goal(field.world, attacker, Point{100, 900}, kTarget, kBand, 0);
  CHECK(!goal.free);
  CHECK(goal.at == kTarget);
  // A band of 39 has no ring at 0: the nearest of its sixteen points at 38,
  // which truncation makes the north-east one, 36.8 out where the rest are
  // 37 or more.
  const MovementSystem::RingGoal ring =
      field.movement.ring_goal(field.world, attacker, Point{100, 900}, kTarget, 39, 0);
  CHECK(!ring.free);
  CHECK(ring.at == (Point{926, 926}));
}

TEST(the_free_spot_search_draws_one_heading_and_steps_out_from_the_centre) {
  // `0x004180b0`: `rand(0, 359)` from the world's generator, the unit's
  // radius turned along it with `Rot`'s constant, and the first passable free
  // spot from the centre outward. A post on the centre takes the centre and
  // every probe nearer to it than 30.
  Field field;
  field.unit("Post", kTarget);
  const ObjectId asker = field.unit("Soldier", Point{100, 100});
  Rng expected = field.world.rng();
  const std::int32_t heading = expected.between(0, 359);
  const Point step = rotate_like_gbr(Point{0, 15}, heading);
  const std::int64_t stride = static_cast<std::int64_t>(step.x) * step.x +
                              static_cast<std::int64_t>(step.y) * step.y;
  REQUIRE(stride > 0);
  std::int32_t k = 1;
  while (static_cast<std::int64_t>(k) * k * stride < 30 * 30) ++k;
  Point spot;
  REQUIRE(field.movement.free_spot(field.world, asker, kTarget, spot));
  CHECK(spot == (Point{kTarget.x + k * step.x, kTarget.y + k * step.y}));
  // Exactly one draw.
  CHECK(field.world.rng() == expected);
  // A free centre is the answer, still for one draw.
  Field open;
  const ObjectId lone = open.unit("Soldier", Point{100, 100});
  Rng once = open.world.rng();
  (void)once.between(0, 359);
  REQUIRE(open.movement.free_spot(open.world, lone, kTarget, spot));
  CHECK(spot == kTarget);
  CHECK(open.world.rng() == once);
}

TEST(an_attacker_queued_behind_a_full_band_is_re_aimed_round_it_once) {
  // The band is full and a column of posts stands west of it, out to x = 610.
  // The attacker from the west is cut back to the column's end; asked again
  // from there it gets nowhere, with nothing left of the band, so the route
  // search asks the free-spot search (`0x004191bf`). The heading is east of
  // the target, where a spot is free some 300 units nearer than the attacker
  // stands -- more than 200 -- so the route is re-aimed at it, exactly
  // (`0x00419244`), and the attacker goes round.
  Field field;
  const ObjectId post = field.unit("Post", kTarget);
  std::vector<ObjectId> bodies = ring_of_posts(field);
  bodies.push_back(post);
  for (std::int32_t x = 610; x <= 830; x += 20) {
    for (const std::int32_t y : {870, 900, 930}) bodies.push_back(field.unit("Post", Point{x, y}));
  }
  const ObjectId attacker = field.unit("Soldier", Point{100, 900});
  field.movement.order_goto_object(field.world, attacker, post, kBand, 0, true);
  CHECK(!field.move(attacker).free_spot_tried);
  for (int t = 0; t < 30; ++t) field.world.advance(800);
  const Point queued = field.at(attacker);
  CHECK(queued.x < 610);
  CHECK(!field.move(attacker).has_path);
  CHECK(field.move(attacker).last_outcome != MoveOutcome::arrived);

  // The script's `GotoAttack` asks again: the same target, the same band.
  field.world.rng() = Rng(seed_heading_east());
  Rng drawn = field.world.rng();
  (void)drawn.between(0, 359);
  field.movement.order_goto_object(field.world, attacker, post, kBand, 0, true);
  CHECK(field.world.rng() == drawn);
  CHECK(field.move(attacker).free_spot_tried);
  CHECK(field.move(attacker).free_spot_aimed);
  REQUIRE(field.move(attacker).has_path);
  const Point spot = field.move(attacker).waypoints.back();
  CHECK(spot.x > kTarget.x);
  CHECK(isqrt(dist_sq(queued, kTarget)) - isqrt(dist_sq(spot, kTarget)) > 200);
  // The order's band is still the target's: the spot is not arrival.
  CHECK(field.move(attacker).target_object == post);
  CHECK(field.move(attacker).range == kBand);
  for (int t = 0; t < 40; ++t) field.world.advance(800);
  CHECK(field.at(attacker) == spot);
  CHECK(field.move(attacker).last_outcome != MoveOutcome::arrived);
  CHECK(!field.move(attacker).free_spot_aimed);
  // The posts of the ring touch one another; the attacker touches none.
  for (const ObjectId body : bodies) CHECK(dist_sq(field.at(body), spot) >= 30 * 30);

  // Once per destination: asked again, it searches the rings and draws
  // nothing.
  const Rng before = field.world.rng();
  field.movement.order_goto_object(field.world, attacker, post, kBand, 0, true);
  CHECK(field.world.rng() == before);
  CHECK(field.move(attacker).free_spot_tried);
  // An order anywhere else forgets it.
  field.movement.order_goto(field.world, attacker, Point{1500, 1500}, kBand, 0, kNoObject, true);
  CHECK(!field.move(attacker).free_spot_tried);
}

TEST(a_unit_already_at_a_full_band_draws_its_heading_and_stays) {
  // The spot must be more than 200 units nearer than the unit stands: one
  // waiting at the edge of a full band runs the search once and is not
  // re-aimed.
  Field field;
  const ObjectId post = field.unit("Post", kTarget);
  ring_of_posts(field);
  const ObjectId attacker = field.unit("Soldier", Point{830, 900});
  const Rng before = field.world.rng();
  field.movement.order_goto_object(field.world, attacker, post, kBand, 0, true);
  CHECK(!field.move(attacker).has_path);
  CHECK(field.move(attacker).free_spot_tried);
  CHECK(!field.move(attacker).free_spot_aimed);
  CHECK(!(field.world.rng() == before));
  CHECK(field.at(attacker) == (Point{830, 900}));
}

TEST(an_order_without_the_lock_flag_never_runs_the_free_spot_search) {
  // `0x00419110` asks it only for an owner (`0x004178d0`).
  Field field;
  const ObjectId post = field.unit("Post", kTarget);
  ring_of_posts(field);
  const ObjectId attacker = field.unit("Soldier", Point{830, 900});
  const Rng before = field.world.rng();
  field.movement.order_goto_object(field.world, attacker, post, kBand, 0, false);
  CHECK(field.world.rng() == before);
  CHECK(!field.move(attacker).free_spot_tried);
}

TEST(the_free_spot_flags_survive_a_save_and_are_not_hashed) {
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0, kNoObject, true);
  field.movement.state(walker).free_spot_tried = true;
  field.movement.state(walker).free_spot_aimed = true;
  std::uint64_t with = 0;
  field.movement.hash(with);
  std::vector<std::byte> bytes;
  field.movement.serialize(bytes);
  field.movement.state(walker).free_spot_tried = false;
  field.movement.state(walker).free_spot_aimed = false;
  std::uint64_t without = 0;
  field.movement.hash(without);
  CHECK(with == without);
  REQUIRE(field.movement.deserialize(bytes).ok());
  CHECK(field.move(walker).free_spot_tried);
  CHECK(field.move(walker).free_spot_aimed);
}

// --------------------------------------------------------------------------
// `Unit::Stop` on a walking unit, and a march's members at its end
// --------------------------------------------------------------------------

namespace {

/// A march member walking through a fellow member who stands on its line --
/// one march does not block itself -- and the hero, at (100,900), either
/// standing (the march is over) or walking far off (it is still on). Five
/// turns of 800 ms put the walker, from (100,500) at 100 units a second, on
/// the stander at (500,500).
struct ThroughAFellow {
  Field field;
  ObjectId hero = kNoObject;
  ObjectId stander = kNoObject;
  ObjectId walker = kNoObject;

  explicit ThroughAFellow(bool hero_marches) {
    hero = field.unit("Soldier", Point{100, 900});
    stander = field.unit("Soldier", Point{500, 500});
    walker = field.unit("Soldier", Point{100, 500});
    field.movement.state(hero).party = hero;
    if (hero_marches) {
      field.movement.order_goto(field.world, hero, Point{1900, 900}, 0, 0, hero);
    }
    CHECK(field.movement.order_goto(field.world, stander, Point{500, 500}, 0, 0, hero, true) ==
          MoveOutcome::arrived);
    field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0, hero, true);
    for (int i = 0; i < 5; ++i) field.world.advance(800);
  }
  std::int64_t gap() { return isqrt(dist_sq(field.at(walker), field.at(stander))); }
};

}  // namespace

TEST(a_unit_told_to_stop_walks_on_off_the_body_it_is_passing_through) {
  // `Unit::Stop` (0x005d6c90) on a unit with a route sets the stop bit on it
  // (0x004178b0), and the path follower (0x00419ee0) then stops on the first
  // point that is passable and a free spot (0x0040a990) -- for a route whose
  // retry names an owner (`0x004178d0`). A march member's names one once the
  // march is over. So the walker, told to stop on top of its fellow, walks on
  // to the step clear of him; it used to stop where it stood.
  ThroughAFellow f(/*hero_marches=*/false);
  REQUIRE(f.field.move(f.walker).has_path);
  CHECK(f.gap() < 30);  // drawn through each other, radii 15 and 15
  CHECK(f.field.movement.lock_owner(f.walker, f.field.move(f.walker)));
  CHECK(f.field.movement.request_stop(f.field.world, f.walker));
  for (int i = 0; i < 3; ++i) f.field.world.advance(800);
  CHECK(!f.field.move(f.walker).has_path);
  CHECK(!f.field.move(f.walker).stop_requested);
  CHECK(f.gap() >= 30);
  // And it stopped at the first step it tested after the request -- a march
  // walks `kFormationStride` steps, so (700,500) -- not at its route's end.
  CHECK(f.field.at(f.walker) == (Point{700, 500}));
  CHECK(f.field.move(f.walker).last_outcome == MoveOutcome::idle);
}

TEST(a_march_member_told_to_stop_while_the_march_is_on_stops_where_it_is) {
  // While the formation still has a path, `0x004178d0` names no owner for a
  // member's retry, and the path follower's stop branch takes no step at all
  // (0x00419ef8 -> 0x0041a019). The same walker stops on its fellow.
  ThroughAFellow f(/*hero_marches=*/true);
  REQUIRE(f.field.move(f.walker).has_path);
  REQUIRE(f.field.move(f.hero).has_path);
  CHECK(f.field.movement.marching(f.walker, f.field.move(f.walker)));
  CHECK(!f.field.movement.lock_owner(f.walker, f.field.move(f.walker)));
  const Point before = f.field.at(f.walker);
  CHECK(!f.field.movement.request_stop(f.field.world, f.walker));
  CHECK(!f.field.move(f.walker).has_path);
  f.field.world.advance(800);
  CHECK(f.field.at(f.walker) == before);
  CHECK(f.gap() < 30);
}

TEST(a_unit_told_to_stop_with_no_free_spot_ahead_stops_at_its_routes_end) {
  // The stop branch never asks `IsArrived`: it takes the route's next step
  // while the spot is taken, and at the end there is none to take, so the
  // unit stands there, taken or not -- where an order with no stop asked
  // would be laid again from there (`RecastPathfind`).
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  const ObjectId post = field.unit("Post", Point{300, 500});
  // A post with a route is a mover, which the step walks through (soft) and
  // the free-spot test does not count; give it none, so it is a stander.
  (void)post;
  field.movement.order_goto(field.world, walker, Point{300, 500}, 0, 0, kNoObject, false);
  REQUIRE(field.move(walker).has_path);
  field.movement.state(walker).dest_lock = true;
  REQUIRE(field.movement.request_stop(field.world, walker));
  for (int i = 0; i < 6; ++i) field.world.advance(800);
  CHECK(!field.move(walker).has_path);
  CHECK(field.move(walker).last_outcome == MoveOutcome::idle);
}

TEST(a_march_member_waits_at_its_station_until_the_march_is_over) {
  // The original member's route is the formation's (`SetFormation`,
  // 0x00417790), and it has a next point for as long as the formation's path
  // does; `UNIT_FORM_MOVE.VS`'s `while (.HasPath())` holds the member to the
  // march for exactly that long. Here a member at the end of its station's
  // route waits there with its route while its hero walks, and arrives --
  // free-spot tested, the march's flag being set -- once he stands.
  Field field;
  const ObjectId hero = field.unit("Soldier", Point{100, 900});
  const ObjectId member = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, hero, Point{1900, 900}, 0, 0, hero);
  field.movement.order_goto(field.world, member, Point{300, 500}, 0, 0, hero, true);
  for (int i = 0; i < 6; ++i) field.world.advance(800);
  CHECK(field.at(member) == (Point{300, 500}));
  CHECK(field.move(member).has_path);
  CHECK(field.world.state(member)->flags.has_active_path);
  field.movement.stop(field.world, hero);
  field.world.advance(800);
  CHECK(!field.move(member).has_path);
  CHECK(field.move(member).last_outcome == MoveOutcome::arrived);
  CHECK(field.at(member) == (Point{300, 500}));
}

TEST(the_stop_request_and_the_marchs_flag_survive_a_save_and_are_not_hashed) {
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0, kNoObject, true);
  REQUIRE(field.movement.request_stop(field.world, walker));
  field.movement.state(walker).form_lock = true;
  std::uint64_t with = 0;
  field.movement.hash(with);
  std::vector<std::byte> bytes;
  field.movement.serialize(bytes);
  field.movement.state(walker).stop_requested = false;
  field.movement.state(walker).form_lock = false;
  std::uint64_t without = 0;
  field.movement.hash(without);
  CHECK(with == without);
  REQUIRE(field.movement.deserialize(bytes).ok());
  CHECK(field.move(walker).stop_requested);
  CHECK(field.move(walker).form_lock);
}

TEST(a_new_order_clears_the_stop_request) {
  // `SetDest` clears the retry's stop bit (0x00417797 masks it off), so a unit
  // told to stop and then sent somewhere walks there.
  Field field;
  const ObjectId walker = field.unit("Soldier", Point{100, 500});
  field.movement.order_goto(field.world, walker, Point{900, 500}, 0, 0, kNoObject, true);
  REQUIRE(field.movement.request_stop(field.world, walker));
  field.movement.order_goto(field.world, walker, Point{900, 700}, 0, 0, kNoObject, true);
  CHECK(!field.move(walker).stop_requested);
  for (int i = 0; i < 3; ++i) field.world.advance(800);
  CHECK(field.move(walker).has_path);
}
