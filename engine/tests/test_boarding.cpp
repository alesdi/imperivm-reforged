// Boarding: sim/boarding.hpp, and the two entry points elsewhere that the same
// reading unblocked -- `Ship::UnitsCount` in `economy` and `ObjectState::
// ship_to_board`'s round trip through the world save.
//
// No game data anywhere in this file. The one class document below declares
// `max_units_to_board="2"` so that "the ship is exactly full" is two units
// rather than sixty, and every distance in the file is a number the case's own
// coordinates make obvious.
//
// The cases that carry the reading are:
//
//   * **The unit's field is not the ship's list.** A cancelled unit is off the
//     list and still names the ship, which is the whole reason `BoardingTable`
//     exists beside `ObjectState::ship_to_board` instead of being derived from
//     it. `UNIT_BOARD_ONFINISH.VS` is written for exactly that.
//   * **`NumUnitsToBoard` does not prune and `AreUnitsToBoard` does**, so a
//     ship whose expected party has died reports them until something else
//     asks. Both readings are in the corpus.
//   * **The full-ship clear is an equality test**, so a ship one over capacity
//     never clears -- and `BoardUnit` is what puts it there, because it checks
//     distance and not capacity.
//   * **A distance tie goes to the earlier member**, which is why the table
//     keeps notify order rather than sorting by id.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "builder.hpp"
#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/boarding.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/lsa.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

constexpr script::TypeId kTypeObj = 1;

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A world with the systems the boarding family reaches for: commands (the
/// ship's `boardunit`), heroes (`CancelArmyBoard`'s army) and the economy
/// (`UnitsCount`'s settlement half, so the ship half is measured against a live
/// alternative rather than against nothing).
struct Fixture {
  World world;
  ClassGraph graph;
  CommandSystem commands;
  HeroSystem heroes;
  EconomySystem economy;
  MovementSystem movement;
  script::HostRegistry registry;
  HostContext context;
  ClassIndex ship_class = kNoClass;
  ClassIndex unit_class = kNoClass;
  /// `Grid` is a view over its bytes, so the terrain layer's buffer lives here.
  std::vector<std::byte> terrain_bytes;

  Fixture() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        // Two, not sixty: the shipped `ShipBattle` declares 60 and this file
        // needs "exactly full" to be reachable in two lines.
        // The name matters: `FindPointToStay` adds the radius of the class
        // called `ShipBattle` to a water unit's reach, whatever class the
        // receiver is.
        R"(<class id="ShipBattle" parent="Object" cpp_class="CVXShip">
             <properties radius="90" water_unit="1"/></class>)",
        R"(<class id="Barge" parent="Object" cpp_class="CVXShip">
             <properties maxhealth="500" max_units_to_board="2"/></class>)",
        R"(<class id="Soldier" parent="Object" cpp_class="CVXUnit">
             <properties maxhealth="100"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "shipbattle.sc.xml", "barge.sc.xml",
                           "soldier.sc.xml"};
    for (int i = 0; i < 4; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    world.set_class_graph(&graph);
    ship_class = graph.find("Barge");
    unit_class = graph.find("Soldier");

    world.add_system(&commands);
    world.add_system(&heroes);
    world.add_system(&economy);
    world.add_system(&movement);
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  script::HostOutcome call(script::CallKind kind, std::string_view name,
                           std::vector<script::Value> args) {
    const std::uint16_t arity =
        static_cast<std::uint16_t>(args.size() - (kind == script::CallKind::member ? 1 : 0));
    const std::uint32_t index = registry.find(kind, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    ctx.script = 7;
    return entry.fn(ctx);
  }

  [[nodiscard]] script::Value obj(ObjectId id) const {
    return script::Value::object(script::ObjectRef{kTypeObj, id});
  }

  [[nodiscard]] std::int32_t number(const script::HostOutcome& out) const {
    CHECK(out.status == script::HostStatus::ok);
    return out.status == script::HostStatus::ok && out.value.is_integer() ? out.value.as_integer()
                                                                         : -12345;
  }

  [[nodiscard]] bool truthy(const script::HostOutcome& out) const {
    return out.status == script::HostStatus::ok && out.value.truthy_scalar();
  }

  /// A ship of the two-berth class, at `where`.
  World::ShipIds make_ship(Point where) {
    const World::ShipIds ids = world.spawn_ship(nullptr, ship_class);
    world.set_position(ids.ship, where);
    world.set_health(ids.ship, 500);
    return ids;
  }

  /// A living soldier at `where`. Living matters: `AreUnitsToBoard` prunes on
  /// exactly the rule `Obj::IsAlive` uses, which is `health > 0`.
  ObjectId make_unit(Point where) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, unit_class);
    world.set_position(id, where);
    world.set_health(id, 100);
    return id;
  }

  /// A `cells` x `cells` passability grid, everything free except the one
  /// 16-unit column `wall` when it is given.
  void open_field(std::int32_t cells, std::int32_t wall = -1) {
    ObstructionGrid grid(cells, cells);
    if (wall >= 0) {
      for (std::int32_t y = 0; y < cells; ++y) grid.set_cell(wall, y, true);
    }
    movement.set_grid(std::move(grid));
  }

  /// A 1024-unit terrain layer, 16 cells across, land west of terrain column
  /// `sea_from` and deep water from it on. Written the way `test_entrance.cpp`
  /// writes its seas.
  void terrain_with_a_sea_from(std::uint32_t sea_from) {
    imperivm::test::Builder out;
    constexpr std::uint32_t kExtent = 1024;
    constexpr std::uint32_t kCell = 64;
    const std::uint32_t cells = kExtent / kCell;
    out.text(kGridMagic).u32(kCell).u32(8).u32(kExtent).u32(kExtent);
    for (std::uint32_t y = 0; y < cells; ++y) {
      for (std::uint32_t x = 0; x < cells; ++x) out.u8(x >= sea_from ? 13 : 1);
    }
    terrain_bytes.assign(out.span().begin(), out.span().end());
    const Result<Grid> parsed = Grid::parse(terrain_bytes);
    REQUIRE(parsed.ok());
    world.set_terrain(parsed.value());
  }

  /// `unit` is inside `ship`'s holder.
  [[nodiscard]] bool aboard(ObjectId unit, const World::ShipIds& ship) const {
    const WorldObject* slot = world.find(unit);
    return slot != nullptr && slot->state.holder == ship.holder;
  }

  /// A script list holding `ids`, in that order.
  [[nodiscard]] script::Value list_of(std::initializer_list<ObjectId> ids) {
    ObjListPool& pool = objlist_pool_of(world);
    const ObjListId id = pool.acquire_temporary(7);
    for (const ObjectId item : ids) pool.mutable_items(id)->push_back(item);
    return make_objlist_value(id);
  }
};

[[nodiscard]] Point pt(std::int32_t x, std::int32_t y) { return Point{x, y}; }

}  // namespace

// --------------------------------------------------------------------------
// the list, and the field beside it
// --------------------------------------------------------------------------

/// `NotifyBoardUnit` appends once, starts the ship's command once, and stamps
/// the unit -- and the three are separable.
TEST(boarding_notify_appends_once_and_starts_one_boardunit_command) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  // The invariant every ship-side answer here rests on, asserted rather than
  // assumed: `spawn_ship` mints the holder immediately after the ship.
  REQUIRE(ship.holder == ship.ship + 1);
  const ObjectId a = f.make_unit(pt(1000, 0));
  const ObjectId b = f.make_unit(pt(2000, 0));

  CHECK(f.commands.command_count(ship.ship) == 0);
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(a)});
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 1);
  CHECK(f.commands.command_count(ship.ship) == 1);
  CHECK(f.commands.command_name(ship.ship, 0) == "boardunit");
  // `REQUIRE`, not `CHECK`: the identity below dereferences the queue, and a
  // fault that stopped the command being issued at all would take the test
  // process down with no output rather than reporting a failure. That happened.
  REQUIRE(f.commands.find(ship.ship) != nullptr);
  REQUIRE(!f.commands.find(ship.ship)->entries.empty());
  const std::uint32_t first_command = f.commands.find(ship.ship)->entries.front().id;

  // A second notify for the *same* unit changes nothing at all.
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(a)});
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 1);

  // A second unit joins the list, and **does not restart the command**. That is
  // the point of the name comparison at 0x005c7f6a: `SHIP_BOARD.VS` is mid-loop
  // and must not begin again from the top.
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(b)});
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 2);
  CHECK(f.commands.find(ship.ship)->entries.front().id == first_command);

  // Notify order, which is what a distance tie is broken by.
  const std::span<const ObjectId> members = f.world.boarding().expected(ship.ship);
  REQUIRE(members.size() == 2);
  CHECK(members[0] == a);
  CHECK(members[1] == b);

  // And the unit-side field, which is a different thing.
  CHECK(f.world.find(a)->state.ship_to_board == ship.ship);
  CHECK(f.world.find(b)->state.ship_to_board == ship.ship);

  // **A repeat notify does not re-stamp the unit either.** Ask a second ship for
  // `a`, so its field names that one, and then ask the first again: the deque
  // `find` at 0x005c7fa0 exits before the `[unit+0x18c]` write, so the field
  // keeps naming the ship that asked most recently *and got a hit*.
  const World::ShipIds rival = f.make_ship(pt(8000, 0));
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(rival.ship), f.obj(a)});
  CHECK(f.world.find(a)->state.ship_to_board == rival.ship);
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(a)});
  CHECK(f.world.find(a)->state.ship_to_board == rival.ship);

  // A unit that is already aboard is not asked for again -- the second `find`
  // at 0x005c7ecd, over the holder rather than over the deque.
  const ObjectId aboard = f.make_unit(pt(5, 5));
  REQUIRE(f.world.put_in_holder(aboard, ship.holder));
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(aboard)});
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 2);
  CHECK(f.world.find(aboard)->state.ship_to_board == kNoObject);

  // A ship already running something else is put on `boardunit`. `SHIP_IDLE.VS`
  // is what it would have been running.
  const World::ShipIds idler = f.make_ship(pt(9000, 0));
  f.commands.set_command(f.world, idler.ship, "idle", Command{});
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(idler.ship), f.obj(b)});
  CHECK(f.commands.command_name(idler.ship, 0) == "boardunit");
  // And `b` is now expected by two ships at once, which the original permits
  // and which is the case a single back-link on the unit could not carry.
  CHECK(f.world.boarding().listed(ship.ship, b));
  CHECK(f.world.boarding().listed(idler.ship, b));
  CHECK(f.world.find(b)->state.ship_to_board == idler.ship);
}

/// A cancel takes the unit off the list and **leaves its field alone**.
///
/// This is the asymmetry `UNIT_BOARD_ONFINISH.VS` is built on: it reaches the
/// ship back through `.GetShipToBoard` precisely because the cancel it is about
/// to perform has not yet run, and the field survives the one before it too.
TEST(boarding_cancel_clears_the_list_and_not_the_units_own_field) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  const ObjectId a = f.make_unit(pt(1000, 0));
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(a)});

  const script::HostOutcome named =
      f.call(script::CallKind::member, "GetShipToBoard", {f.obj(a)});
  REQUIRE(named.value.is_object());
  CHECK(named.value.as_object().id == ship.ship);

  f.call(script::CallKind::member, "NotifyBoardUnitCancel", {f.obj(ship.ship), f.obj(a)});
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 0);
  // Still named. Nothing in `gbr.exe` clears `[unit+0x18c]`.
  const script::HostOutcome after =
      f.call(script::CallKind::member, "GetShipToBoard", {f.obj(a)});
  REQUIRE(after.value.is_object());
  CHECK(after.value.as_object().id == ship.ship);

  // **A ship whose last member cancels holds no row at all.** The original has
  // no such thing as a row -- every ship carries its deque, empty or not -- so
  // the row is this table's bookkeeping and must not leak into the state two
  // peers compare. A world that emptied a list and one that never filled it are
  // the same world, and they have to hash alike.
  CHECK(f.world.boarding().rows() == 0);

  // Cancelling a unit that is not on the list is a no-op rather than an error:
  // it is what the second `..._ONFINISH` of a pair does.
  f.call(script::CallKind::member, "NotifyBoardUnitCancel", {f.obj(ship.ship), f.obj(a)});
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 0);

  // A ship that has sunk reads as the invalid handle, which is what the
  // `.IsValid` guard in both `..._ONFINISH` scripts is for.
  REQUIRE(f.world.despawn(ship.ship));
  const script::HostOutcome sunk =
      f.call(script::CallKind::member, "GetShipToBoard", {f.obj(a)});
  CHECK(sunk.value.is_object());
  CHECK(sunk.value.as_object().type == script::kNoType);
}

// --------------------------------------------------------------------------
// who prunes, and who does not
// --------------------------------------------------------------------------

/// `NumUnitsToBoard` is a bare read of the size; `AreUnitsToBoard` prunes first.
///
/// 0x005c6f60 is four instructions and touches nothing. 0x005c79a0 walks the
/// deque dropping every member whose handle no longer resolves or whose
/// `IsAlive` slot says no. The difference is observable, so it is kept.
TEST(boarding_only_are_units_to_board_prunes_the_dead) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  const ObjectId alive = f.make_unit(pt(1000, 0));
  const ObjectId killed = f.make_unit(pt(1100, 0));
  const ObjectId removed = f.make_unit(pt(1200, 0));
  for (const ObjectId id : {alive, killed, removed}) {
    f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(id)});
  }
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 3);

  f.world.set_health(killed, 0);
  REQUIRE(f.world.despawn(removed));

  // Still three: the count does not look.
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 3);
  // And now one, because this one does. Both halves of the prune fire here --
  // a resolvable-but-dead member and a member the world no longer holds.
  CHECK(f.truthy(f.call(script::CallKind::member, "AreUnitsToBoard", {f.obj(ship.ship)})));
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 1);
  // Same reason as above: indexing the span has to be guarded, or a fault that
  // emptied the row crashes instead of failing.
  REQUIRE(f.world.boarding().expected(ship.ship).size() == 1);
  CHECK(f.world.boarding().expected(ship.ship)[0] == alive);

  // The last live member dies: the list empties and the loop ends.
  f.world.set_health(alive, 0);
  CHECK(!f.truthy(f.call(script::CallKind::member, "AreUnitsToBoard", {f.obj(ship.ship)})));
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 0);
}

/// The full-ship clear compares for **equality**, and `BoardUnit` is what can
/// put a ship past the number it compares against.
TEST(boarding_full_ship_clear_is_an_equality_test) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  const ObjectId waiting = f.make_unit(pt(1000, 0));
  const ObjectId one = f.make_unit(pt(1, 0));
  const ObjectId two = f.make_unit(pt(2, 0));
  const ObjectId three = f.make_unit(pt(3, 0));

  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(waiting)});
  REQUIRE(f.world.put_in_holder(one, ship.holder));
  // One of two aboard: not full, so the list survives.
  CHECK(f.truthy(f.call(script::CallKind::member, "AreUnitsToBoard", {f.obj(ship.ship)})));

  REQUIRE(f.world.put_in_holder(two, ship.holder));
  // Exactly two of two: the list is emptied outright, dead or not.
  CHECK(!f.truthy(f.call(script::CallKind::member, "AreUnitsToBoard", {f.obj(ship.ship)})));
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 0);

  // Three of two, which `BoardUnit` will do because it checks distance and not
  // capacity: the equality fails, so a fresh list is *not* cleared. That is the
  // original's behaviour and the reason `>=` is not written here.
  REQUIRE(f.world.put_in_holder(three, ship.holder));
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(waiting)});
  CHECK(f.truthy(f.call(script::CallKind::member, "AreUnitsToBoard", {f.obj(ship.ship)})));
}

// --------------------------------------------------------------------------
// choosing, and arriving
// --------------------------------------------------------------------------

/// The nearest, with the earlier member winning a tie.
TEST(boarding_best_candidate_is_the_nearest_and_ties_go_to_the_earlier) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  const ObjectId far = f.make_unit(pt(3000, 4000));   // 5000
  const ObjectId near = f.make_unit(pt(300, 400));    // 500
  const ObjectId tie = f.make_unit(pt(-300, -400));   // 500 as well
  for (const ObjectId id : {far, near, tie}) {
    f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(id)});
  }

  const script::HostOutcome best =
      f.call(script::CallKind::member, "BestCandidateToBoard", {f.obj(ship.ship)});
  REQUIRE(best.value.is_object());
  // `near` was notified before `tie` and the comparison is strict, so it keeps
  // the win. Sorting the row by id would give the same answer here only by
  // accident, which is why the tie is built out of two equal distances.
  CHECK(best.value.as_object().id == near);

  // It prunes on the way in, and it answers the invalid handle for a list with
  // nobody live left -- `SHIP_BOARD.VS` reads that straight into `u1` and asks
  // `u1.IsAlive` two statements later.
  for (const ObjectId id : {far, near, tie}) f.world.set_health(id, 0);
  const script::HostOutcome none =
      f.call(script::CallKind::member, "BestCandidateToBoard", {f.obj(ship.ship)});
  CHECK(none.value.is_object());
  CHECK(none.value.as_object().type == script::kNoType);
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 0);
}

/// `BoardUnit` is a 150-unit radius, a holder move, and a delisting.
TEST(boarding_board_unit_needs_150_units_and_no_capacity) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  const ObjectId walker = f.make_unit(pt(200, 0));
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(walker)});

  // Too far, and nothing happens.
  CHECK(!f.truthy(f.call(script::CallKind::member, "BoardUnit", {f.obj(ship.ship), f.obj(walker)})));
  CHECK(f.world.find(walker)->state.holder == kNoObject);
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 1);

  // 150 exactly is still too far: `jae` at 0x005c7cd2 takes the failing branch
  // on equality, so the rim does not count.
  f.world.set_position(walker, pt(150, 0));
  CHECK(!f.truthy(f.call(script::CallKind::member, "BoardUnit", {f.obj(ship.ship), f.obj(walker)})));

  // 149 boards.
  f.world.set_position(walker, pt(149, 0));
  CHECK(f.truthy(f.call(script::CallKind::member, "BoardUnit", {f.obj(ship.ship), f.obj(walker)})));
  CHECK(f.world.find(walker)->state.holder == ship.holder);
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 0);
  // The field it was stamped with survives the boarding, as it survives a
  // cancel: nothing clears it.
  CHECK(f.world.find(walker)->state.ship_to_board == ship.ship);

  // Boarding again is false, because it is already inside -- the holder `find`
  // at 0x005c7889, not a capacity test.
  CHECK(!f.truthy(f.call(script::CallKind::member, "BoardUnit", {f.obj(ship.ship), f.obj(walker)})));

  // **Past capacity, and it still boards.** Two berths, two aboard, and a third
  // walks on. This is the case that makes the equality test above reachable.
  const ObjectId second = f.make_unit(pt(10, 0));
  const ObjectId third = f.make_unit(pt(20, 0));
  CHECK(f.truthy(f.call(script::CallKind::member, "BoardUnit", {f.obj(ship.ship), f.obj(second)})));
  CHECK(f.truthy(f.call(script::CallKind::member, "BoardUnit", {f.obj(ship.ship), f.obj(third)})));
  CHECK(f.number(f.call(script::CallKind::member, "UnitsCount", {f.obj(ship.ship)})) == 3);
  CHECK(f.number(f.call(script::CallKind::member, "UnitsMax", {f.obj(ship.ship)})) == 2);
}

// --------------------------------------------------------------------------
// the two cancels that call it all off
// --------------------------------------------------------------------------

/// `NotifyShipBoardingCancel` stops the units that are still walking towards
/// the ship, and only those.
TEST(boarding_ship_cancel_kills_only_the_boarding_commands) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  const ObjectId walker = f.make_unit(pt(1000, 0));
  const ObjectId hero_side = f.make_unit(pt(1100, 0));
  const ObjectId common = f.make_unit(pt(1200, 0));
  const ObjectId distracted = f.make_unit(pt(1300, 0));
  const ObjectId idle_hands = f.make_unit(pt(1400, 0));
  for (const ObjectId id : {walker, hero_side, common, distracted, idle_hands}) {
    f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(id)});
  }
  f.commands.set_command(f.world, walker, "boardship", Command{});
  f.commands.set_command(f.world, hero_side, "boardshiphero", Command{});
  f.commands.set_command(f.world, common, "boardshipcommon", Command{});
  // Told to do something else since. 0x005c76c0 reads the *running* command
  // only, so this one is left alone.
  f.commands.set_command(f.world, distracted, "attack", Command{});
  // And one with no command at all, which the `[unit+0x98]` test skips.

  f.call(script::CallKind::member, "NotifyShipBoardingCancel", {f.obj(ship.ship)});

  CHECK(f.commands.command_count(walker) == 0);
  CHECK(f.commands.command_count(hero_side) == 0);
  CHECK(f.commands.command_count(common) == 0);
  CHECK(f.commands.command_name(distracted, 0) == "attack");
  CHECK(f.commands.command_count(idle_hands) == 0);
  // Emptied whatever happened to the commands.
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 0);
}

/// `CancelArmyBoard` splits the hero's army along the gangplank.
TEST(boarding_cancel_army_board_drops_the_followers_on_the_other_side) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.unit_class);
  f.world.set_health(hero, 400);
  f.world.set_position(hero, pt(50, 0));
  f.heroes.register_hero(f.world, hero);

  // `REQUIRE` returns from the enclosing function, so the attach is checked
  // out here rather than inside a helper lambda.
  const auto follower = [&](Point where) -> ObjectId {
    const ObjectId id = f.make_unit(where);
    f.heroes.register_unit(f.world, id);
    return f.heroes.attach(f.world, id, hero) ? id : kNoObject;
  };
  const ObjectId with_him = follower(pt(60, 0));
  const ObjectId ashore = follower(pt(900, 0));
  REQUIRE(with_him != kNoObject);
  REQUIRE(ashore != kNoObject);
  REQUIRE(f.heroes.army_size(hero) == 2);

  // The hero and one warrior are aboard; the other is still on the beach.
  // The hero was asked for before it boarded, so it has a place in the ship's
  // queue -- which is what makes the `!hero_aboard` guard below observable.
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(hero)});
  REQUIRE(f.world.put_in_holder(hero, ship.holder));
  REQUIRE(f.world.put_in_holder(with_him, ship.holder));
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 1);
  f.call(script::CallKind::member, "CancelArmyBoard", {f.obj(hero)});

  // The one that made it stays; the one that did not is no longer his.
  CHECK(f.heroes.army_size(hero) == 1);
  CHECK(f.heroes.hero_of(with_him) == hero);
  CHECK(f.heroes.hero_of(ashore) == kNoObject);
  // **And the queue is untouched**, because the hero is aboard. 0x0052fe18
  // tests the same boolean it split the army on, and a hero that made it has
  // nothing left to cancel.
  CHECK(f.number(f.call(script::CallKind::member, "NumUnitsToBoard", {f.obj(ship.ship)})) == 1);

  // The mirror case: a hero still ashore keeps the ones ashore, and gives up
  // its own place in the ship's queue -- which the aboard case does not do.
  Fixture g;
  const World::ShipIds boat = g.make_ship(pt(0, 0));
  const ObjectId chief = g.world.spawn(NativeClass::hero, nullptr, g.unit_class);
  g.world.set_health(chief, 400);
  g.world.set_position(chief, pt(900, 0));
  g.heroes.register_hero(g.world, chief);
  const ObjectId beached = g.make_unit(pt(910, 0));
  g.heroes.register_unit(g.world, beached);
  REQUIRE(g.heroes.attach(g.world, beached, chief));
  const ObjectId embarked = g.make_unit(pt(5, 0));
  g.heroes.register_unit(g.world, embarked);
  REQUIRE(g.heroes.attach(g.world, embarked, chief));
  REQUIRE(g.world.put_in_holder(embarked, boat.holder));

  g.call(script::CallKind::member, "NotifyBoardUnit", {g.obj(boat.ship), g.obj(chief)});
  CHECK(g.number(g.call(script::CallKind::member, "NumUnitsToBoard", {g.obj(boat.ship)})) == 1);
  g.call(script::CallKind::member, "CancelArmyBoard", {g.obj(chief)});
  CHECK(g.heroes.army_size(chief) == 1);
  CHECK(g.heroes.hero_of(beached) == chief);
  CHECK(g.heroes.hero_of(embarked) == kNoObject);
  // Off the queue, because the hero is not aboard.
  CHECK(g.number(g.call(script::CallKind::member, "NumUnitsToBoard", {g.obj(boat.ship)})) == 0);
}

// --------------------------------------------------------------------------
// the table as world state
// --------------------------------------------------------------------------

/// `UnitsCount` answers a settlement and a ship, from two different holders.
TEST(boarding_units_count_reads_the_ships_holder_too) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  CHECK(f.number(f.call(script::CallKind::member, "UnitsCount", {f.obj(ship.ship)})) == 0);
  const ObjectId a = f.make_unit(pt(1, 0));
  REQUIRE(f.world.put_in_holder(a, ship.holder));
  CHECK(f.number(f.call(script::CallKind::member, "UnitsCount", {f.obj(ship.ship)})) == 1);

  // A receiver that is neither a settlement nor a ship is the settlement
  // members' zero -- what `Settlement::UnitsCount` (0x005c1da3) prints and
  // pushes for a handle that resolves to nothing -- and not a ship's count:
  // the ship branch is an addition, not a widening.
  const ObjectId soldier = f.make_unit(pt(500, 0));
  const script::HostOutcome none =
      f.call(script::CallKind::member, "UnitsCount", {f.obj(soldier)});
  CHECK(none.status == script::HostStatus::ok);
  CHECK(none.value.as_integer() == 0);
}

/// The table is hashed and it survives a save.
TEST(boarding_table_is_hashed_and_round_trips) {
  Fixture f;
  const World::ShipIds ship = f.make_ship(pt(0, 0));
  const ObjectId a = f.make_unit(pt(1000, 0));
  const ObjectId b = f.make_unit(pt(2000, 0));

  const std::uint64_t empty = f.world.state_hash();
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(a)});
  const std::uint64_t one = f.world.state_hash();
  CHECK(one != empty);
  // And back, exactly -- of the table, not of the world, because the notify also
  // put the ship on a command and the command queues are hashed too. A list
  // that was filled and emptied has to be the same *state* as one that was
  // never filled, not merely one that behaves the same, and that is what makes
  // dropping the emptied row a determinism property rather than tidiness.
  BoardingTable fresh;
  BoardingTable used;
  used.notify(ship.ship, a);
  used.notify(ship.ship, b);
  CHECK(used.cancel(ship.ship, a));
  CHECK(used.cancel(ship.ship, b));
  std::uint64_t fresh_hash = 0;
  std::uint64_t used_hash = 0;
  fresh.hash(fresh_hash);
  used.hash(used_hash);
  CHECK(fresh.rows() == 0);
  CHECK(used.rows() == 0);
  CHECK(fresh_hash == used_hash);
  f.call(script::CallKind::member, "NotifyBoardUnit", {f.obj(ship.ship), f.obj(b)});
  const std::uint64_t two = f.world.state_hash();
  CHECK(two != one);

  std::vector<std::byte> bytes;
  f.world.serialize(bytes);

  World back;
  CommandSystem commands;
  HeroSystem heroes;
  EconomySystem economy;
  MovementSystem movement;
  back.set_class_graph(&f.graph);
  back.add_system(&commands);
  back.add_system(&heroes);
  back.add_system(&economy);
  back.add_system(&movement);
  REQUIRE(back.deserialize(bytes).ok());
  CHECK(back.state_hash() == two);
  const std::span<const ObjectId> members = back.boarding().expected(ship.ship);
  REQUIRE(members.size() == 2);
  // Notify order, across the save. Order is state here, so the save has to
  // carry it rather than rebuild it.
  CHECK(members[0] == a);
  CHECK(members[1] == b);
  CHECK(back.find(a)->state.ship_to_board == ship.ship);

  // A ship that sinks takes its row with it -- unlike a member that dies, which
  // stays until something prunes. `sim/boarding.hpp` records the asymmetry.
  CHECK(f.world.boarding().rows() == 1);
  REQUIRE(f.world.despawn(ship.ship));
  CHECK(f.world.boarding().rows() == 0);
}

// --------------------------------------------------------------------------
// the unboarding half
// --------------------------------------------------------------------------

/// 0x005c8010: 25 x 25 steps of 16 around the centre, row by row, minus the
/// wall, the sea and the edge of the map.
TEST(unboarding_points_is_the_lattice_minus_walls_water_and_the_map_edge) {
  Fixture f;
  // No grid at all: nothing to land on, and no crash.
  std::vector<Point> none;
  CHECK(unboarding_points(f.world, pt(512, 512), none) == 0);

  // A wall down the 16-unit column at x = 480, and deep water from x = 640 --
  // which the half-cell sampling pulls back to x >= 608.
  f.open_field(64, 30);
  f.terrain_with_a_sea_from(10);
  std::vector<Point> points;
  CHECK(unboarding_points(f.world, pt(512, 512), points) == 425);
  REQUIRE(points.size() == 425);
  // Row by row, x fastest, from the north-west corner.
  CHECK(points[0] == pt(320, 320));
  CHECK(points[1] == pt(336, 320));
  // The first row is 25 columns less the wall and the seven water columns.
  CHECK(points[17] == pt(320, 336));
  CHECK(points.back() == pt(592, 704));
  const auto has = [&](Point p) {
    for (const Point q : points) {
      if (q == p) return true;
    }
    return false;
  };
  CHECK(has(pt(496, 320)));
  CHECK(!has(pt(480, 320)));   // the wall
  CHECK(!has(pt(608, 320)));   // water, by the half-cell rule
  CHECK(has(pt(592, 320)));

  // At the map corner only the cells that exist are kept: 13 columns of 13.
  CHECK(unboarding_points(f.world, pt(0, 0), points) == 169);
  CHECK(points[0] == pt(0, 0));
}

/// 0x005c81d0: one draw from the world generator, the chosen point out, the
/// rest in their order.
TEST(take_random_point_draws_once_and_keeps_the_survivors_in_order) {
  Fixture f;
  f.world.seed(0x5eed);
  std::vector<Point> points = {pt(1, 0), pt(2, 0), pt(3, 0), pt(4, 0)};
  const std::vector<Point> before = points;

  Rng oracle = f.world.rng();
  const std::int32_t expected = oracle.between(0, 3);
  const Point chosen = take_random_point(f.world, points);
  CHECK(chosen == before[static_cast<std::size_t>(expected)]);
  CHECK(f.world.rng().state() == oracle.state());
  REQUIRE(points.size() == 3);
  // Ordered erase: everything after the hole moved down one, nothing swapped.
  std::size_t k = 0;
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (static_cast<std::int32_t>(i) == expected) continue;
    CHECK(points[k++] == before[i]);
  }

  // Empty: no draw, and a value nothing can mistake for a place.
  std::vector<Point> empty;
  const std::uint32_t state = f.world.rng().state();
  CHECK(take_random_point(f.world, empty) == kHeldPosition);
  CHECK(f.world.rng().state() == state);
}

/// `UnboardAllUnits` drops every passenger, in holder order, each on the point
/// the next draw names.
TEST(unboard_all_units_drops_every_passenger_on_a_drawn_point) {
  Fixture f;
  f.open_field(64, 30);
  f.terrain_with_a_sea_from(10);
  f.world.seed(77);
  const World::ShipIds ship = f.make_ship(pt(512, 512));
  const ObjectId a = f.make_unit(pt(0, 0));
  const ObjectId b = f.make_unit(pt(0, 0));
  const ObjectId c = f.make_unit(pt(0, 0));
  for (const ObjectId id : {a, b, c}) REQUIRE(f.world.put_in_holder(id, ship.holder));

  // The oracle: the same lattice, and the same three draws in id order.
  std::vector<Point> lattice;
  REQUIRE(unboarding_points(f.world, pt(512, 512), lattice) == 425);
  Rng oracle = f.world.rng();
  Point expected[3];
  for (Point& p : expected) {
    const std::int32_t index = oracle.between(0, static_cast<std::int32_t>(lattice.size()) - 1);
    p = lattice[static_cast<std::size_t>(index)];
    lattice.erase(lattice.begin() + index);
  }

  const script::HostOutcome out =
      f.call(script::CallKind::member, "UnboardAllUnits", {f.obj(ship.ship)});
  CHECK(out.status == script::HostStatus::ok);
  CHECK(f.world.rng().state() == oracle.state());
  CHECK(!f.aboard(a, ship));
  CHECK(!f.aboard(b, ship));
  CHECK(!f.aboard(c, ship));
  CHECK(f.world.resolve_position(a) == expected[0]);
  CHECK(f.world.resolve_position(b) == expected[1]);
  CHECK(f.world.resolve_position(c) == expected[2]);
  // Nothing else moved: not the list, not the field, not the ship.
  CHECK(f.world.boarding().count(ship.ship) == 0);
  CHECK(f.world.resolve_position(ship.ship) == pt(512, 512));

  // An unresolvable receiver is the printed diagnostic and nothing else.
  const std::uint32_t state = f.world.rng().state();
  CHECK(f.call(script::CallKind::member, "UnboardAllUnits", {f.obj(kNoObject)}).status ==
        script::HostStatus::ok);
  CHECK(f.world.rng().state() == state);
}

/// `Ship::ApplyAiTransport` lands the passengers the way `UnboardAllUnits`
/// does and then orders each of their squads on with the crossing's verb at
/// its point -- once per squad, in order of first appearance, keeping the
/// state and clearing `SF_ADVCHOOSER` -- and clears the order last.
TEST(apply_ai_transport_lands_the_passengers_and_orders_their_squads_on) {
  Fixture f;
  AiSystem ai;
  REQUIRE(f.world.add_system(&ai));
  f.open_field(64, 30);
  f.terrain_with_a_sea_from(10);
  f.world.seed(77);
  // The soldier class binds `advance`, so the order has somewhere to land.
  f.graph.add(bytes_of(R"(<class id="Marine" parent="Soldier" cpp_class="CVXUnit">
        <method sig="advance" vs="data/subai/unit_advance.vs"/>
      </class>)"),
              "marine.sc.xml");
  f.graph.link();
  const ClassIndex marine = f.graph.find("Marine");
  const auto make_marine = [&]() {
    const ObjectId id = f.world.spawn(NativeClass::unit, nullptr, marine);
    f.world.set_position(id, pt(0, 0));
    f.world.set_owner(id, 1);
    f.world.set_health(id, 100);
    return id;
  };
  const World::ShipIds ship = f.make_ship(pt(512, 512));
  // Two squads aboard -- the first with two members, the second with one --
  // plus a passenger in no squad and a building-shaped stowaway in a squad
  // of its own, which the unit test at 0x0044290e leaves out.
  const ObjectId a = make_marine();
  const ObjectId b = make_marine();
  const ObjectId c = make_marine();
  const ObjectId loner = make_marine();
  const ObjectId crate = f.world.spawn(NativeClass::building, nullptr, marine);
  f.world.set_owner(crate, 1);
  f.world.set_health(crate, 100);
  const SquadKey first = f.heroes.squads().create(1, a);
  REQUIRE(f.heroes.squads().join(first, b));
  const SquadKey second = f.heroes.squads().create(1, c);
  const SquadKey cargo = f.heroes.squads().create(1, crate);
  f.heroes.squads().find(first)->state = 6;
  f.heroes.squads().find(first)->flags = kSquadFlagAdvChooser | kSquadFlagPeaceful;
  f.heroes.squads().find(second)->state = 3;
  f.heroes.squads().find(second)->flags = kSquadFlagAdvChooser;
  f.heroes.squads().find(cargo)->flags = kSquadFlagAdvChooser;
  for (const ObjectId id : {a, b, c, loner, crate}) REQUIRE(f.world.put_in_holder(id, ship.holder));
  const Point beach{700, 300};
  ai.set_ship_transport(ship.ship, "advance", beach);
  f.world.advance_turns(2);
  const GameTime now = f.world.time();
  // A probe order on a bystander, so the ids the landing hands out can be
  // counted from a known one.
  const ObjectId bystander = make_marine();
  (void)f.commands.set_command(f.world, bystander, "advance", Command{});
  const std::uint32_t probe = f.commands.find(bystander)->entries.front().id;

  const script::HostOutcome out =
      f.call(script::CallKind::member, "ApplyAiTransport", {f.obj(ship.ship)});
  CHECK(out.status == script::HostStatus::ok);
  // Everybody is ashore.
  for (const ObjectId id : {a, b, c, loner}) CHECK(!f.aboard(id, ship));
  // Each squad: the same state, restamped; `SF_ADVCHOOSER` gone and nothing
  // else touched; every member ordered `advance` to the beach.
  const Squad* one = f.heroes.squads().find(first);
  const Squad* two = f.heroes.squads().find(second);
  REQUIRE(one != nullptr && two != nullptr);
  CHECK(one->state == 6);
  CHECK(one->state_time == now);
  CHECK(one->flags == kSquadFlagPeaceful);
  CHECK(two->state == 3);
  CHECK(two->flags == 0);
  std::uint32_t ids[3] = {0, 0, 0};
  int slot = 0;
  for (const ObjectId id : {a, b, c}) {
    const CommandQueue* q = f.commands.find(id);
    REQUIRE(q != nullptr && q->entries.size() == 1);
    CHECK(q->entries.front().verb == "advance");
    CHECK(q->entries.front().point == beach);
    ids[slot++] = q->entries.front().id;
  }
  // Once per squad, not once per member: three orders issued in all, the
  // next three ids after the probe's -- a squad ordered twice would have
  // replaced its members' orders and pushed the ids along.
  CHECK(ids[0] == probe + 1);
  CHECK(ids[1] == probe + 2);
  CHECK(ids[2] == probe + 3);
  // The passenger in no squad got no order, the crate's squad was not one of
  // the landed, and the order is cleared.
  CHECK(f.commands.command_count(loner) == 0);
  CHECK(f.heroes.squads().find(cargo)->flags == kSquadFlagAdvChooser);
  CHECK(ai.ship_transport(ship.ship).order.empty());

  // Nothing to apply: an empty ship, and a ship with no order, both land
  // and order nobody -- and a receiver that is not there does nothing.
  const World::ShipIds empty = f.make_ship(pt(512, 512));
  ai.set_ship_transport(empty.ship, "move", beach);
  CHECK(f.call(script::CallKind::member, "ApplyAiTransport", {f.obj(empty.ship)}).status ==
        script::HostStatus::ok);
  CHECK(ai.ship_transport(empty.ship).order.empty());
  CHECK(f.call(script::CallKind::member, "ApplyAiTransport", {f.obj(kNoObject)}).status ==
        script::HostStatus::ok);
}

/// A beach with sixteen points and twenty passengers leaves four aboard.
TEST(unboard_all_units_stops_when_the_points_run_out) {
  Fixture f;
  // A 64-unit map: the lattice reaches every cell, and there are sixteen.
  f.open_field(4);
  const World::ShipIds ship = f.make_ship(pt(32, 32));
  std::vector<ObjectId> units;
  for (int i = 0; i < 20; ++i) {
    units.push_back(f.make_unit(pt(0, 0)));
    REQUIRE(f.world.put_in_holder(units.back(), ship.holder));
  }
  f.call(script::CallKind::member, "UnboardAllUnits", {f.obj(ship.ship)});
  std::size_t ashore = 0;
  for (std::size_t i = 0; i < units.size(); ++i) {
    const bool still = f.aboard(units[i], ship);
    if (!still) ++ashore;
    // Holder order is id order here; the last four boarded are the ones left.
    CHECK(still == (i >= 16));
  }
  CHECK(ashore == 16);
  // Sixteen distinct points: every cell of the map, none twice.
  for (std::size_t i = 0; i < 16; ++i) {
    for (std::size_t j = i + 1; j < 16; ++j) {
      CHECK(f.world.resolve_position(units[i]) != f.world.resolve_position(units[j]));
    }
  }
}

/// `UnboardUnits(ol)` walks the list, drops the members aboard this ship, and
/// tests the point supply before each member -- including one it then skips.
TEST(unboard_units_drops_only_the_listed_passengers_of_this_ship) {
  Fixture f;
  f.open_field(64);
  f.world.seed(5);
  const World::ShipIds ship = f.make_ship(pt(512, 512));
  const World::ShipIds rival = f.make_ship(pt(512, 800));
  const ObjectId a = f.make_unit(pt(0, 0));
  const ObjectId b = f.make_unit(pt(0, 0));
  const ObjectId c = f.make_unit(pt(50, 50));
  const ObjectId d = f.make_unit(pt(0, 0));
  REQUIRE(f.world.put_in_holder(a, ship.holder));
  REQUIRE(f.world.put_in_holder(b, rival.holder));
  REQUIRE(f.world.put_in_holder(d, ship.holder));

  Rng oracle = f.world.rng();
  (void)oracle.between(0, 624);  // exactly one draw: `a` is the only hit
  const script::HostOutcome out = f.call(script::CallKind::member, "UnboardUnits",
                                         {f.obj(ship.ship), f.list_of({c, b, a, kNoObject})});
  CHECK(out.status == script::HostStatus::ok);
  CHECK(f.world.rng().state() == oracle.state());
  CHECK(!f.aboard(a, ship));
  CHECK(f.aboard(b, rival));                     // another ship's passenger
  CHECK(f.world.resolve_position(c) == pt(50, 50));  // ashore already
  CHECK(f.aboard(d, ship));                      // aboard, and not listed

  // The supply is tested before each member: sixteen points, sixteen listed
  // passengers, then a bystander, then one more passenger who stays aboard.
  Fixture g;
  g.open_field(4);
  const World::ShipIds barge = g.make_ship(pt(32, 32));
  std::vector<ObjectId> listed;
  for (int i = 0; i < 16; ++i) {
    listed.push_back(g.make_unit(pt(0, 0)));
    REQUIRE(g.world.put_in_holder(listed.back(), barge.holder));
  }
  const ObjectId bystander = g.make_unit(pt(40, 40));
  const ObjectId last = g.make_unit(pt(0, 0));
  REQUIRE(g.world.put_in_holder(last, barge.holder));
  ObjListPool& pool = objlist_pool_of(g.world);
  const ObjListId id = pool.acquire_temporary(7);
  for (const ObjectId item : listed) pool.mutable_items(id)->push_back(item);
  pool.mutable_items(id)->push_back(bystander);
  pool.mutable_items(id)->push_back(last);
  g.call(script::CallKind::member, "UnboardUnits", {g.obj(barge.ship), make_objlist_value(id)});
  for (const ObjectId item : listed) CHECK(!g.aboard(item, barge));
  CHECK(g.aboard(last, barge));
}

/// `FindPointToStay`: two draws per try, y first; the first candidate that is
/// in the ship's area, unblocked and clear of standing units wins; a hundred
/// misses answer the ship's own position.
TEST(find_point_to_stay_is_the_first_candidate_that_passes_three_tests) {
  Fixture f;
  f.open_field(64);
  const World::ShipIds ship = f.make_ship(pt(512, 512));

  // Open water, nobody about: the first throw lands.
  f.world.seed(31);
  Rng oracle = f.world.rng();
  const std::int32_t dy1 = oracle.between(-500, 500);
  const std::int32_t dx1 = oracle.between(-500, 500);
  const Point first = pt(512 + dx1, 512 + dy1);
  script::HostOutcome out =
      f.call(script::CallKind::member, "FindPointToStay", {f.obj(ship.ship)});
  CHECK(out.status == script::HostStatus::ok);
  CHECK(unpack_point(out.value) == first);
  CHECK(f.world.rng().state() == oracle.state());

  // A living soldier standing on that very point blocks it: the Barge
  // declares no radius and is not a water unit, so its reach is 40, and the
  // soldier is at distance 0. The second throw is taken instead.
  const std::int32_t dy2 = oracle.between(-500, 500);
  const std::int32_t dx2 = oracle.between(-500, 500);
  const Point second = pt(512 + dx2, 512 + dy2);
  {
    const std::int64_t ddx = second.x - first.x;
    const std::int64_t ddy = second.y - first.y;
    // The seed was chosen so the second throw is clear of the first; a
    // different generator would need a different seed here.
    REQUIRE(ddx * ddx + ddy * ddy >= 40 * 40);
  }
  const ObjectId sentry = f.make_unit(first);
  f.world.seed(31);
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(ship.ship)});
  CHECK(unpack_point(out.value) == second);
  CHECK(f.world.rng().state() == oracle.state());

  // The same soldier walking does not block: 0x004094d0 admits only a unit
  // with no path.
  f.world.find(sentry)->state.flags.has_active_path = true;
  f.world.seed(31);
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(ship.ship)});
  CHECK(unpack_point(out.value) == first);
  f.world.find(sentry)->state.flags.has_active_path = false;

  // The blocking test is strict: a soldier at exactly the reach is clear.
  f.world.set_position(sentry, pt(first.x + 40, first.y));
  f.world.seed(31);
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(ship.ship)});
  CHECK(unpack_point(out.value) == first);
  f.world.set_position(sentry, pt(first.x + 39, first.y));
  f.world.seed(31);
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(ship.ship)});
  CHECK(unpack_point(out.value) == second);
  f.world.set_position(sentry, first);

  // Nor does a dead one.
  f.world.set_health(sentry, 0);
  f.world.seed(31);
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(ship.ship)});
  CHECK(unpack_point(out.value) == first);
  f.world.set_health(sentry, 100);

  // A water unit's reach is its own radius plus `ShipBattle`'s -- 90 and 90,
  // so 180 -- and a soldier 200 away is clear where one 170 away is not. The
  // barge is moved off the map first: a ship is a living unit too, and the
  // one standing where the warship is would otherwise block its throws.
  f.world.set_position(ship.ship, pt(5000, 5000));
  const World::ShipIds warship = f.world.spawn_ship(nullptr, f.graph.find("ShipBattle"));
  f.world.set_position(warship.ship, pt(512, 512));
  f.world.set_health(warship.ship, 100);
  f.world.set_position(sentry, pt(first.x + 200, first.y));
  f.world.seed(31);
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(warship.ship)});
  CHECK(unpack_point(out.value) == first);
  f.world.set_position(sentry, pt(first.x + 170, first.y));
  f.world.seed(31);
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(warship.ship)});
  CHECK(unpack_point(out.value) == second);
  f.world.set_position(sentry, pt(0, 0));
  f.world.set_position(ship.ship, pt(512, 512));
  f.world.set_position(warship.ship, pt(5000, 5000));

  // Everything blocked: a hundred tries, two hundred draws, and the ship's own
  // position.
  f.open_field(1);
  f.world.seed(31);
  Rng spent = f.world.rng();
  for (int i = 0; i < 200; ++i) (void)spent.between(-500, 500);
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(ship.ship)});
  CHECK(unpack_point(out.value) == pt(512, 512));
  CHECK(f.world.rng().state() == spent.state());

  // No receiver: `(0, 0)`, and no draw.
  const std::uint32_t state = f.world.rng().state();
  out = f.call(script::CallKind::member, "FindPointToStay", {f.obj(kNoObject)});
  CHECK(unpack_point(out.value) == pt(0, 0));
  CHECK(f.world.rng().state() == state);
}

/// A candidate across a wall is in another area and is passed over, however
/// free it is.
TEST(find_point_to_stay_stays_in_the_ships_own_area) {
  Fixture f;
  // A wall down the middle, and a terrain layer so the partition builds.
  f.open_field(64, 32);
  f.terrain_with_a_sea_from(99);
  f.world.mutable_lsa().build(f.world.terrain(), f.movement.grid());
  REQUIRE(f.world.lsa().size() >= 2);
  const World::ShipIds ship = f.make_ship(pt(300, 512));
  const LsaId home = f.world.lsa().at(pt(300, 512));
  REQUIRE(home != kNoLsa);
  REQUIRE(f.world.lsa().at(pt(700, 512)) != home);

  // A seed whose first throw lands east of the wall.
  std::uint32_t seed = 1;
  Point first{};
  for (;; ++seed) {
    Rng probe(seed);
    const std::int32_t dy = probe.between(-500, 500);
    const std::int32_t dx = probe.between(-500, 500);
    first = pt(300 + dx, 512 + dy);
    if (first.x >= 528 && first.y >= 0 && first.y < 1024) break;
    REQUIRE(seed < 100000);
  }
  f.world.seed(seed);
  const script::HostOutcome out =
      f.call(script::CallKind::member, "FindPointToStay", {f.obj(ship.ship)});
  const Point chosen = unpack_point(out.value);
  CHECK(chosen != first);
  CHECK(chosen.x < 512);
  CHECK(f.world.lsa().at(chosen) == home);
}
