// A gate bars a route per search, and a step stops before it.
//
// No game data. The wall, the gate, the soldiers and their classes are written
// here; the numbers are the original's (20, 70, 210, 1,300, 1,500) and every
// expected coordinate is worked out in the comment beside it.
//
// The world: a 2,048-unit square of open ground, cut across by a wall two
// collision cells deep -- rows 62 and 63, y 992..1023 -- with a gap at cells
// 60..64 (x 960..1039) and, on some benches, a second gap far to the west at
// cells 10..14 (x 160..239). The gate stands in the first gap at (1000, 1000):
// its type-7 markers 40 either side give it the axis (960,1000)..(1040,1000),
// whose line is cells 60..65 on rows 62 and 63.
//
// A soldier walks 100 units a second with a radius of 15 and a 40-unit stride,
// so it steps cooperatively, and is sent from (1000, 1300) to (1000, 700): a
// straight route of 600 that crosses the gate's axis 300 along it. A gate in
// its way is within reach once the crossing is no more than 15 + 210 = 225
// ahead, which the step decided at 80 along the route finds first (at 40 it is
// 260 ahead): a soldier that a gate bars stands at (1000, 1220).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/gate.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A soldier whose walk advances 40 units a cycle: the stride that makes its
/// route cooperative.
constexpr std::string_view kSoldierEntity = R"(<?xml version="1.0"?>
<entity name="soldier" type="vx/unit" variations="8">
  <images>
    <image idx="1" file="walk.rle" drawmode="player_color" remaping="none" rows="2" columns="8"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="1" anim_frame="1"/>
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
  </anims>
</entity>)";

/// A gate across an opening in a wall: its two type-7 ends 40 units either
/// side of it, its type-10 marker on the axis.
constexpr std::string_view kGateEntity =
    "<entity name=\"gate\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"1\" type=\"7\" x=\"-40\" y=\"0\"/>"
    "<point idx=\"2\" type=\"10\" x=\"0\" y=\"0\"/>"
    "<point idx=\"3\" type=\"7\" x=\"40\" y=\"0\"/>"
    "</points>"
    "</entity>";

constexpr Point kGatePoint{1000, 1000};
constexpr Point kSouth{1000, 1300};
constexpr Point kNorth{1000, 700};
/// Where a soldier the gate bars stands: 80 along the route from `kSouth`.
constexpr std::int32_t kStandY = 1220;

struct GateField {
  ClassGraph graph;
  Result<Entity> soldier = Entity::parse(bytes_of(kSoldierEntity));
  Result<Entity> gate_entity = Entity::parse(bytes_of(kGateEntity));
  World world;
  MovementSystem movement;
  ObjectId gate = kNoObject;

  explicit GateField(bool second_gap = false) {
    graph.add(bytes_of(R"(<class id="Soldier" cpp_class="CVXUnit" parent="">
      <properties speed="100" radius="15"/></class>)"),
              "soldier.sc.xml");
    graph.add(bytes_of(R"(<class id="Gate" cpp_class="CVXGate" parent="">
      <properties maxhealth="1000"/></class>)"),
              "gate.sc.xml");
    graph.link();
    REQUIRE(soldier.ok());
    REQUIRE(gate_entity.ok());
    world.set_class_graph(&graph);
    movement.set_class_graph(&graph);
    ObstructionGrid grid(128, 128);
    for (std::int32_t x = 0; x < 128; ++x) {
      if (x >= 60 && x <= 64) continue;
      if (second_gap && x >= 10 && x <= 14) continue;
      grid.set_cell(x, 62, true);
      grid.set_cell(x, 63, true);
    }
    movement.set_grid(std::move(grid));
    REQUIRE(world.add_system(&movement));
    world.start();
    // Players 0 and 1 at war, each its own friend.
    world.players().set(0, 1, Relation::allied, false);
    world.players().set(1, 0, Relation::allied, false);
    // After the start, so the gate's type-10 marker makes no ownerless lock.
    gate = world.spawn(NativeClass::gate, &gate_entity.value(), graph.find("Gate"));
    world.set_health(gate, 1000);
    world.set_owner(gate, 0);
    REQUIRE(world.set_position(gate, kGatePoint));
  }

  ObjectId unit(PlayerId owner, Point where, bool strides = true) {
    const ObjectId id = world.spawn(NativeClass::unit, strides ? &soldier.value() : nullptr,
                                    graph.find("Soldier"));
    CHECK(world.set_position(id, where));
    CHECK(world.set_health(id, 100));
    world.set_owner(id, owner);
    return id;
  }

  Point at(ObjectId id) { return world.resolve_position(id); }
  const MoveState& move(ObjectId id) {
    const MoveState* state = movement.find(id);
    CHECK(state != nullptr);
    return state == nullptr ? none : *state;
  }
  WorldObject& gate_slot() { return *world.find(gate); }
  /// `OpenNow` / `CloseNow`, as the script calls them.
  void swing(bool open) { gate_retarget(gate_slot(), open, world.time()); }
  /// Walk for `ms` in 100 ms turns, and say whether `id` was ever north of
  /// the gate's axis.
  bool walk(ObjectId id, int ms) {
    bool through = at(id).y < kGatePoint.y;
    for (int t = 0; t < ms; t += 100) {
      world.advance(100);
      through = through || at(id).y < kGatePoint.y;
    }
    return through;
  }
  MoveState none;
};

}  // namespace

// --------------------------------------------------------------------------
// the arithmetic
// --------------------------------------------------------------------------

/// 0x00529300 and 0x00529070: a gate lets units through once its portcullis
/// passes 20, both ways, or once it is destroyed; it is fully open only with
/// its target open and its portcullis at 70. Nothing that is not a gate does
/// either.
TEST(a_gate_lets_units_through_above_twenty_and_is_open_only_at_seventy) {
  GateField f;
  WorldObject& g = f.gate_slot();
  const GameTime t = f.world.time();
  CHECK(!gate_lets_through(g, t));
  CHECK(!gate_fully_open(g, t));
  // Opening from 0: 20 at 599 ms, 21 at 600, 70 at 2,000.
  f.swing(true);
  CHECK(gate_raise(g, t + 599) == 20);
  CHECK(!gate_lets_through(g, t + 599));
  CHECK(gate_lets_through(g, t + 600));
  CHECK(!gate_fully_open(g, t + 1999));
  CHECK(gate_fully_open(g, t + 2000));
  // Closing from 70: 21 at 1,428 ms, 20 at 1,429. Raised but closing is not
  // open.
  g.gate = GateMotion{t + 2000, 70};
  g.state.flags.gate_open = false;
  CHECK(gate_lets_through(g, t + 2000 + 1428));
  CHECK(!gate_lets_through(g, t + 2000 + 1429));
  CHECK(!gate_fully_open(g, t + 2000));
  // Destroyed: through, whatever the portcullis.
  f.world.set_health(f.gate, 0);
  CHECK(gate_lets_through(g, t + 10000));
  CHECK(!gate_fully_open(g, t + 10000));
  // Not a gate.
  const ObjectId unit = f.unit(0, Point{100, 100});
  f.world.mutable_state(unit)->flags.gate_open = true;
  CHECK(!gate_lets_through(*f.world.find(unit), t));
  CHECK(!gate_fully_open(*f.world.find(unit), t));
}

/// 0x00417ed0 over 0x0040aab0: the first leg that meets the segment, ends
/// included, and the distance to the meeting point -- each leg's length and
/// the last piece rounded down on their own, the point itself truncated.
TEST(a_route_crossing_is_the_distance_to_where_it_first_meets_the_axis) {
  const std::vector<Point> route{{0, 0}, {100, 0}, {100, 100}};
  CHECK(route_crossing(route, Point{50, -10}, Point{50, 10}) == 50);
  CHECK(route_crossing(route, Point{90, 50}, Point{110, 50}) == 150);
  // Either way round, the same.
  CHECK(route_crossing(route, Point{110, 50}, Point{90, 50}) == 150);
  // An end of the axis on the route counts, and so does a route ending on it.
  CHECK(route_crossing(route, Point{50, 0}, Point{50, 20}) == 50);
  CHECK(route_crossing(route, Point{90, 100}, Point{110, 100}) == 200);
  // Short of the route, and beside it.
  CHECK(route_crossing(route, Point{50, 1}, Point{50, 20}) == -1);
  CHECK(route_crossing(route, Point{150, -10}, Point{150, 10}) == -1);
  // Parallel to a leg crosses nothing, even on it (the original's own
  // branch for it was not followed).
  CHECK(route_crossing(route, Point{10, 0}, Point{20, 0}) == -1);
  // The first meeting, not the nearest: the axis meets both legs.
  CHECK(route_crossing(route, Point{40, -10}, Point{120, 70}) == 50);
  // Rounded down: (0,0)..(3,3) meets (0,2)..(2,0) at (1,1), 1.41 along.
  const std::vector<Point> diagonal{{0, 0}, {3, 3}};
  CHECK(route_crossing(diagonal, Point{0, 2}, Point{2, 0}) == 1);
  // A route of one point crosses nothing.
  const std::vector<Point> still{{50, 0}};
  CHECK(route_crossing(still, Point{50, -10}, Point{50, 10}) == -1);
}

/// The lines are learnt as gates appear and dropped as they go, the axis takes
/// the last type-7 marker for its second end, and a line's cells are each
/// listed once.
TEST(the_gate_lines_follow_the_gates_and_list_each_cell_once) {
  GateField f;
  f.world.gate_lines().refresh(f.world);
  REQUIRE(f.world.gate_lines().lines().size() == 1);
  const GateLines::Line* line = f.world.gate_lines().find(f.gate);
  REQUIRE(line != nullptr);
  CHECK(line->has_axis);
  CHECK((line->a == Point{960, 1000}));
  CHECK((line->b == Point{1040, 1000}));
  CHECK(line->cells.size() == 12);  // x 60..65, rows 62 and 63

  // A third type-7 marker is the axis's second end: 0x005293f0 keeps the
  // first and overwrites the second; with no type-10 marker, untranslated.
  const Result<Entity> three = Entity::parse(bytes_of(
      "<entity name=\"g\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
      "<points><point idx=\"1\" type=\"7\" x=\"-40\" y=\"0\"/>"
      "<point idx=\"2\" type=\"7\" x=\"0\" y=\"40\"/>"
      "<point idx=\"3\" type=\"7\" x=\"40\" y=\"0\"/></points></entity>"));
  REQUIRE(three.ok());
  const ObjectId late = f.world.spawn(NativeClass::gate, &three.value(), f.graph.find("Gate"));
  REQUIRE(f.world.set_position(late, Point{500, 500}));
  // Learnt on the next turn, after the first.
  f.world.advance(100);
  REQUIRE(f.world.gate_lines().lines().size() == 2);
  CHECK(f.world.gate_lines().lines()[1].gate == late);
  CHECK((f.world.gate_lines().lines()[1].a == Point{-40, 0}));
  CHECK((f.world.gate_lines().lines()[1].b == Point{40, 0}));
  // A gate that goes takes its line with it.
  REQUIRE(f.world.despawn(f.gate));
  f.world.advance(100);
  REQUIRE(f.world.gate_lines().lines().size() == 1);
  CHECK(f.world.gate_lines().find(f.gate) == nullptr);

  // Crossings come in route order, whatever the gate order: a later gate
  // across the route nearer its start comes first, and a twin on the same
  // axis ties and keeps the gate order.
  GateField g;
  const ObjectId nearer = g.world.spawn(NativeClass::gate, &g.gate_entity.value(),
                                        g.graph.find("Gate"));
  REQUIRE(g.world.set_position(nearer, Point{1000, 1200}));
  const ObjectId twin = g.world.spawn(NativeClass::gate, &g.gate_entity.value(),
                                      g.graph.find("Gate"));
  REQUIRE(g.world.set_position(twin, kGatePoint));
  g.world.gate_lines().refresh(g.world);
  std::vector<GateCrossing> crossings;
  const std::vector<Point> route{kSouth, kNorth};
  g.world.gate_lines().crossings(route, crossings);
  const std::vector<GateCrossing> expected{{nearer, 100}, {g.gate, 300}, {twin, 300}};
  CHECK(crossings == expected);
}

/// A search's own cells: blocked on every edge of their box and nowhere
/// beside it, each once.
TEST(a_cell_overlay_holds_exactly_its_cells) {
  CellOverlay overlay;
  CHECK(overlay.empty());
  CHECK(!overlay.contains(0, 0));
  overlay.add(3, 7);
  overlay.add(9, 2);
  overlay.add(3, 7);
  overlay.add(-4, 5);
  overlay.seal();
  CHECK(overlay.size() == 3);
  CHECK(overlay.contains(3, 7));   // bottom edge
  CHECK(overlay.contains(9, 2));   // right and top edges
  CHECK(overlay.contains(-4, 5));  // left edge
  CHECK(!overlay.contains(9, 7));  // inside the box, not a cell
  CHECK(!overlay.contains(10, 2));
  CHECK(!overlay.contains(-5, 5));
  CHECK(!overlay.contains(3, 8));
  CHECK(!overlay.contains(9, 1));
  overlay.clear();
  CHECK(overlay.empty());
  CHECK(!overlay.contains(3, 7));
}

/// 0x004193c0..0x0041946b: the lines laid for a mover are those of the gates
/// that count it an enemy, unless fully open, and unless destroyed within
/// 1,500 of it.
TEST(an_enemys_search_lays_every_gate_that_bars_it_but_an_open_one_and_near_rubble) {
  GateField f;
  GateLines& lines = f.world.gate_lines();
  lines.refresh(f.world);
  const auto laid = [&](PlayerId owner, Point from) {
    CellOverlay overlay;
    lines.lay_enemy_gates(f.world, owner, from, f.world.time(), overlay);
    overlay.seal();
    return overlay.size();
  };
  CHECK(laid(1, kSouth) == 12);
  CHECK(laid(0, kSouth) == 0);  // its own side: never
  // Opening is not open.
  f.swing(true);
  f.world.advance(1000);
  CHECK(laid(1, kSouth) == 12);
  f.world.advance(1000);  // at 70
  CHECK(laid(1, kSouth) == 0);
  f.swing(false);
  f.world.advance(100);
  CHECK(laid(1, kSouth) == 12);
  // Rubble: a way in from 1,500, laid like any gate from 1,501.
  f.world.set_health(f.gate, 0);
  CHECK(laid(1, Point{1000, 2500}) == 0);
  CHECK(laid(1, Point{1000, 2501}) == 12);
  // An ally of the gate's owner is not barred, on the gate's own row.
  f.world.set_health(f.gate, 1000);
  f.world.players().set(0, 1, Relation::allied, true);
  CHECK(laid(1, kSouth) == 0);
}

// --------------------------------------------------------------------------
// the routes and the step
// --------------------------------------------------------------------------

/// An enemy's straight route through a closed gate is searched again with the
/// gate's line laid, and goes round by the far gap; nothing is ever written
/// into the shared grid.
TEST(an_enemys_route_goes_round_a_closed_gate) {
  GateField f(/*second_gap=*/true);
  const ObjectId foe = f.unit(1, kSouth);
  f.movement.order_goto(f.world, foe, kNorth, 0);
  REQUIRE(f.move(foe).has_path);
  CHECK(f.move(foe).gate_crossings.empty());
  CHECK(f.move(foe).path_length > 1500);  // round by x 160..239
  CHECK(f.movement.avoidance().gate_searches == 1);
  CHECK(!f.movement.grid().blocked_cell(62, 62));  // the gap stays open
  CHECK(f.movement.grid().line_is_clear(kSouth, kNorth));
  // It never sets foot in the gate's passage, and gets there.
  bool in_passage = false;
  for (int t = 0; t < 40000; t += 100) {
    f.world.advance(100);
    const Point p = f.at(foe);
    in_passage = in_passage || (p.x >= 940 && p.x <= 1060 && p.y >= 960 && p.y <= 1040);
  }
  CHECK(!in_passage);
  CHECK(f.at(foe) == kNorth);
}

/// No way round, and a short route: the first route is kept (0x00419490), the
/// soldier walks up to the gate and stands within reach of it -- both kinds of
/// route, stepped and walked a turn at a time -- until the gate is opening,
/// when it walks on.
TEST(an_enemy_stands_before_a_closed_gate_it_cannot_go_round_until_it_opens) {
  for (const bool strides : {true, false}) {
    GateField f;
    const ObjectId foe = f.unit(1, kSouth, strides);
    f.movement.order_goto(f.world, foe, kNorth, 0);
    REQUIRE(f.move(foe).has_path);
    CHECK(f.move(foe).path_length == 600);
    REQUIRE(f.move(foe).gate_crossings.size() == 1);
    CHECK(f.move(foe).gate_crossings[0].gate == f.gate);
    CHECK(f.move(foe).gate_crossings[0].at == 300);
    CHECK(!f.walk(foe, 10000));
    CHECK(f.move(foe).has_path);  // waiting, not given up
    // Stepped, the step decided at 80 along; a turn at a time, 10 units a
    // turn, the first turn that starts at 80 or more. Either way, 80.
    CHECK((f.at(foe) == Point{1000, kStandY}));
    CHECK(f.movement.avoidance().gate_waits > 0);
    // Opening is enough, before the portcullis passes 20 (0x00418360).
    f.swing(true);
    f.world.advance(100);
    CHECK(gate_raise(f.gate_slot(), f.world.time()) < kGatePassableAbove);
    CHECK(f.walk(foe, 10000));
    CHECK(f.at(foe) == kNorth);
  }
}

/// `Unit::Stop` asked of a unit waiting before a gate that bars it: the path
/// follower's stop branch asks the gate after the free spot (0x00419f8a ..
/// 0x00419fa2) and takes no step when it bars, so the unit stops there even
/// on a taken spot -- where waiting for the gate, it would answer `Stop`'s
/// re-entry false for as long as the gate stayed shut.
TEST(a_unit_told_to_stop_before_a_gate_that_bars_it_stops_there) {
  GateField f;
  const ObjectId foe = f.unit(1, kSouth);
  f.movement.order_goto(f.world, foe, kNorth, 0, 0, kNoObject, /*lock_destination=*/true);
  REQUIRE(f.move(foe).has_path);
  CHECK(!f.walk(foe, 10000));
  REQUIRE(f.move(foe).has_path);
  REQUIRE((f.at(foe) == Point{1000, kStandY}));
  // Somebody stands on the spot, so it is not free.
  (void)f.unit(1, Point{1000, kStandY});
  REQUIRE(f.movement.request_stop(f.world, foe));
  f.world.advance(100);
  f.world.advance(100);
  CHECK(!f.move(foe).has_path);
  CHECK((f.at(foe) == Point{1000, kStandY}));
}

/// No way round and a long route (more than 1,300): the second search's
/// partial route is walked, and it ends at the gate's line, south of it.
/// The short-route exception is for exactly one enemy gate: across two --
/// here a twin on the same axis -- the second search's partial route is
/// walked, as for a long one.
TEST(a_short_enemy_route_across_two_gates_with_no_way_round_ends_at_them) {
  GateField f;
  const ObjectId twin = f.world.spawn(NativeClass::gate, &f.gate_entity.value(),
                                      f.graph.find("Gate"));
  f.world.set_owner(twin, 0);
  f.world.set_health(twin, 1000);
  REQUIRE(f.world.set_position(twin, kGatePoint));
  const ObjectId foe = f.unit(1, kSouth);
  f.movement.order_goto(f.world, foe, kNorth, 0);
  REQUIRE(f.move(foe).has_path);
  CHECK(f.move(foe).gate_crossings.empty());
  CHECK(!f.move(foe).path_complete);
  CHECK(!f.walk(foe, 10000));
  CHECK(f.move(foe).last_outcome == MoveOutcome::exhausted);
}

TEST(a_long_enemy_route_with_no_way_round_ends_at_the_closed_gate) {
  GateField f;
  const ObjectId foe = f.unit(1, Point{1000, 1900});
  f.movement.order_goto(f.world, foe, Point{1000, 100}, 0);
  REQUIRE(f.move(foe).has_path);
  CHECK(f.move(foe).gate_crossings.empty());
  CHECK(!f.move(foe).path_complete);
  CHECK(!f.walk(foe, 20000));
  CHECK(!f.move(foe).has_path);
  CHECK(f.move(foe).last_outcome == MoveOutcome::exhausted);
  // Stopped against the line: rows 62 and 63 are the line, so row 64.
  CHECK(f.at(foe).y >= 1024);
  CHECK(f.at(foe).y < 1100);
}

/// A friend's route goes through its own closed gate -- no search lays it --
/// and with no enemies near the gate the step waves it through, the
/// portcullis still down (0x00418260: friend, `[gate+0x210]` clear).
TEST(a_friend_walks_through_its_closed_gate_with_no_enemies_near) {
  GateField f(/*second_gap=*/true);
  const ObjectId friend_ = f.unit(0, kSouth);
  f.movement.order_goto(f.world, friend_, kNorth, 0);
  REQUIRE(f.move(friend_).gate_crossings.size() == 1);
  CHECK(f.move(friend_).path_length == 600);
  CHECK(f.movement.avoidance().gate_searches == 0);
  // Just through, an enemy turns up at the gate: a crossing passed is not
  // looked at again (0x00418210), and the friend walks on.
  while (f.at(friend_).y >= kGatePoint.y - 20) f.world.advance(100);
  f.gate_slot().state.flags.enemies_near = true;
  f.walk(friend_, 8000);
  CHECK(f.at(friend_) == kNorth);
  CHECK(gate_raise(f.gate_slot(), f.world.time()) == 0);
  CHECK(f.movement.avoidance().gate_waits == 0);
}

/// With an enemy near the gate, a friend stands before it while it is closed,
/// and walks on once the gate opens or the enemy goes.
TEST(a_friend_stands_before_its_closed_gate_while_enemies_are_near) {
  for (const bool by_opening : {false, true}) {
    GateField f;
    f.gate_slot().state.flags.enemies_near = true;
    const ObjectId friend_ = f.unit(0, kSouth);
    f.movement.order_goto(f.world, friend_, kNorth, 0);
    CHECK(f.movement.avoidance().gate_searches == 0);
    CHECK(!f.walk(friend_, 5000));
    CHECK((f.at(friend_) == Point{1000, kStandY}));
    if (by_opening) {
      f.swing(true);
    } else {
      f.gate_slot().state.flags.enemies_near = false;
    }
    CHECK(f.walk(friend_, 8000));
    CHECK(f.at(friend_) == kNorth);
  }
  // The rule itself: friend and none near; else letting through or opening.
  GateField f;
  WorldObject& g = f.gate_slot();
  const GameTime t = f.world.time();
  CHECK(gate_waves_through(f.world, g, 0, t));
  CHECK(!gate_waves_through(f.world, g, 1, t));
  g.state.flags.enemies_near = true;
  CHECK(!gate_waves_through(f.world, g, 0, t));
  g.state.flags.gate_open = true;  // opening, at 0
  CHECK(gate_waves_through(f.world, g, 0, t));
  CHECK(gate_waves_through(f.world, g, 1, t));
}

/// A gate standing fully open is laid for nobody: an enemy walks straight
/// through it although a way round exists. One still opening is laid for an
/// enemy's search -- not fully open -- and it goes round; and a destroyed gate
/// near the enemy is laid for nobody and lets everyone through.
TEST(an_open_gate_and_a_destroyed_one_let_an_enemy_through) {
  {
    GateField f(/*second_gap=*/true);
    f.swing(true);
    f.world.advance(2000);
    REQUIRE(gate_fully_open(f.gate_slot(), f.world.time()));
    const ObjectId foe = f.unit(1, kSouth);
    f.movement.order_goto(f.world, foe, kNorth, 0);
    CHECK(f.move(foe).path_length == 600);
    CHECK(f.move(foe).gate_crossings.size() == 1);
    CHECK(f.walk(foe, 8000));
    CHECK(f.at(foe) == kNorth);
  }
  {
    GateField f(/*second_gap=*/true);
    f.swing(true);
    f.world.advance(1000);  // at 35, opening
    const ObjectId foe = f.unit(1, kSouth);
    f.movement.order_goto(f.world, foe, kNorth, 0);
    CHECK(f.move(foe).gate_crossings.empty());
    CHECK(f.move(foe).path_length > 1500);
  }
  {
    GateField f(/*second_gap=*/true);
    f.world.set_health(f.gate, 0);
    const ObjectId foe = f.unit(1, kSouth);
    f.movement.order_goto(f.world, foe, kNorth, 0);
    CHECK(f.move(foe).path_length == 600);
    CHECK(f.walk(foe, 8000));
    CHECK(f.at(foe) == kNorth);
  }
}

/// A route laid through a gate standing open stops before it once the gate
/// has closed under it, and a save taken in the middle of the swing comes
/// back to stop it on the same turn: the portcullis, the route's crossings
/// and the hold all carried. Both kinds of route; the world hash is compared
/// for the one with no walk animation, since a test entity has no path for a
/// load to bind its animation by, and the positions for both.
TEST(a_save_mid_swing_stops_a_route_before_the_same_gate_on_the_same_turn) {
  for (const bool strides : {true, false}) {
    const auto set_up = [strides](GateField& f) {
      f.swing(true);
      f.world.advance(2000);
      const ObjectId foe = f.unit(1, Point{1000, 1700}, strides);
      f.movement.order_goto(f.world, foe, kNorth, 0);
      CHECK(f.move(foe).gate_crossings.size() == 1);  // laid through the open gate
      f.swing(false);
      // 70 down to 21 takes 1,428 ms; saved at 800, the portcullis at 42.
      f.world.advance(800);
      return foe;
    };
    GateField a;
    const ObjectId foe = set_up(a);
    CHECK(gate_raise(a.gate_slot(), a.world.time()) == 42);

    std::vector<std::byte> world_bytes;
    std::vector<std::byte> movement_bytes;
    a.world.serialize(world_bytes);
    a.movement.serialize(movement_bytes);
    GateField b;
    REQUIRE(b.world.deserialize(world_bytes).ok());
    b.world.find(a.gate)->object->entity = &b.gate_entity.value();
    if (strides) b.world.find(foe)->object->entity = &b.soldier.value();
    // The crossings must come from the section, not be left over.
    b.movement.state(foe).gate_crossings.clear();
    REQUIRE(b.movement.deserialize(movement_bytes).ok());
    CHECK(b.world.state_hash() == a.world.state_hash());
    CHECK(b.move(foe).gate_crossings == a.move(foe).gate_crossings);
    CHECK(gate_raise(b.gate_slot(), b.world.time()) == 42);

    for (int turn = 0; turn < 120; ++turn) {
      a.world.advance(100);
      b.world.advance(100);
      CHECK(a.at(foe) == b.at(foe));
      CHECK(a.move(foe).holding == b.move(foe).holding);
      if (!strides) CHECK(a.world.state_hash() == b.world.state_hash());
    }
    // Closed long before it got there -- the portcullis passes 20 some 140
    // units into the walk -- so it stands where any soldier the gate bars
    // stands: the crossing is 700 along, and the step at 480 finds it 220
    // ahead (stepped), or the turn that starts at 480 does (a turn at a time).
    CHECK(a.move(foe).has_path);
    CHECK((a.at(foe) == Point{1000, kStandY}));
    CHECK(b.at(foe) == a.at(foe));
  }
}

// --------------------------------------------------------------------------
// inside the walls
// --------------------------------------------------------------------------

/// 0x005295d0, `Gate::Inside`'s predicate: a unit is inside when its route to
/// the town centre, every gate open, crosses no gate. The town centre stands
/// north of the wall at (1000, 500).
TEST(a_unit_is_inside_when_its_route_to_the_town_centre_crosses_no_gate) {
  GateField f;
  const ObjectId centre = f.world.spawn(NativeClass::town_hall, nullptr, kNoClass);
  REQUIRE(f.world.set_position(centre, Point{1000, 500}));
  const ObjectId in_friend = f.unit(0, kNorth);
  const ObjectId in_enemy = f.unit(1, Point{900, 800});
  const ObjectId out_friend = f.unit(0, kSouth);
  const ObjectId out_enemy = f.unit(1, Point{1100, 1400});

  // Shut, as a gate with nobody near it stands. The search behind the
  // answer lays no gate as a barrier, so an enemy outside is outside
  // because its way in crosses the gate, not because the gate bars it.
  f.swing(false);
  for (int turn = 0; turn < 30; ++turn) f.world.advance(100);
  CHECK(inside_walls(f.world, in_friend, centre));
  CHECK(inside_walls(f.world, in_enemy, centre));
  CHECK(!inside_walls(f.world, out_friend, centre));
  CHECK(!inside_walls(f.world, out_enemy, centre));

  // Open, the same answers: it is the crossing that decides, not the gate's
  // state.
  f.swing(true);
  for (int turn = 0; turn < 30; ++turn) f.world.advance(100);
  CHECK(inside_walls(f.world, in_enemy, centre));
  CHECK(!inside_walls(f.world, out_enemy, centre));

  // In a holder is not inside: in a building, not in the streets.
  CHECK(f.world.put_in_holder(in_friend, centre));
  CHECK(!inside_walls(f.world, in_friend, centre));
}

/// A gap in the wall with no gate in it is no wall at all to this question:
/// a unit whose shortest way in is through it crosses no gate, and reads as
/// inside. That is the original's answer, which counts crossings, not walls.
TEST(a_unit_whose_way_in_is_a_gateless_gap_reads_as_inside) {
  GateField f(true);
  const ObjectId centre = f.world.spawn(NativeClass::town_hall, nullptr, kNoClass);
  REQUIRE(f.world.set_position(centre, Point{1000, 500}));
  // Through the western gap (x 160..239) is 1,243 or so; through the gate,
  // 1,354.
  const ObjectId west = f.unit(1, Point{200, 1300});
  const ObjectId south = f.unit(1, kSouth);
  CHECK(inside_walls(f.world, west, centre));
  CHECK(!inside_walls(f.world, south, centre));
}
