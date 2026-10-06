// The simulation foundation: the object model, the composite allocations, the
// query objects, the spatial queries, the system registry, the hash and the RNG.
//
// Deliberately synthetic. Every fact these tests assert is either an invariant
// the desync dumps measured (and the comment says which measurement) or a
// property the determinism contract needs; none of them needs a byte of game
// data, so they run in CI on a machine that has never seen the game.

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/gate.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

// `imperivm::core` and `imperivm::core::sim` both declare ObjectId, PlayerId,
// kNoObject and kNoPlayer -- the sim ones are the contract in sim/system.hpp,
// the core ones predate it in game/registry.hpp. Importing both wholesale makes
// every use ambiguous, so only the sim namespace comes in wholesale.
using namespace imperivm::core::sim;

using imperivm::core::ClassGraph;
using imperivm::core::Grid;
using imperivm::core::Result;
using imperivm::core::ClassIndex;
using imperivm::core::kNoClass;
using imperivm::core::NativeClass;

namespace {

/// `pt(x, y)` inside a CHECK would look like two macro arguments, so points
/// are built through a function throughout.
constexpr Point pt(std::int32_t x, std::int32_t y) noexcept { return Point{x, y}; }

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A four-class graph: Object -> Unit -> Soldier, and Object -> Building.
///
/// Enough to exercise a class *tree* filter, which is what the dumps show
/// queries matching on -- they mix concrete classes with abstract ones.
struct TinyGraph {
  ClassGraph graph;
  ClassIndex object = kNoClass;
  ClassIndex unit = kNoClass;
  ClassIndex soldier = kNoClass;
  ClassIndex building = kNoClass;

  TinyGraph() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="Unit" parent="Object" cpp_class="CVXUnit"><properties sight="100" maxhealth="50"/></class>)",
        R"(<class id="Soldier" parent="Unit" cpp_class="CVXUnit"><properties sight="200" maxhealth="80"/></class>)",
        R"(<class id="Fort" parent="Object" cpp_class="CVXBuilding"><properties maxhealth="5000"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "unit.sc.xml", "soldier.sc.xml", "fort.sc.xml"};
    for (int i = 0; i < 4; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    object = graph.find("Object");
    unit = graph.find("Unit");
    soldier = graph.find("Soldier");
    building = graph.find("Fort");
  }
};

/// A system that records the order it was run in and folds a value into the
/// hash, so run order and hash folding can both be observed.
class RecordingSystem : public System {
 public:
  RecordingSystem(const char* name, std::vector<std::string>* log, std::uint64_t salt)
      : name_(name), log_(log), salt_(salt) {}

  std::string_view name() const noexcept override { return name_; }
  void start(World&) override { log_->push_back(std::string("start:") + name_); }
  void advance(World&, const Turn&) override {
    ++turns_;
    log_->push_back(std::string("advance:") + name_);
  }
  void hash(std::uint64_t& accumulator) const override { accumulator ^= salt_ + turns_; }

  std::uint64_t turns_ = 0;

 private:
  const char* name_;
  std::vector<std::string>* log_;
  std::uint64_t salt_;
};

}  // namespace

// ==========================================================================
// identity
// ==========================================================================

TEST(sim_ids_are_monotonic_and_never_reused) {
  World world;
  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr);
  const ObjectId c = world.spawn(NativeClass::building, nullptr);
  CHECK(a == 1 && b == 2 && c == 3);

  CHECK(world.despawn(b));
  CHECK(world.find(b) == nullptr);

  // The dumps settle this: handle density falls monotonically with game age --
  // 0.93 at tick 2, 0.052 after 8,569 ticks -- so gaps accumulate and are never
  // refilled. A stale reference must read as dead, not as somebody else.
  const ObjectId d = world.spawn(NativeClass::unit, nullptr);
  CHECK(d == 4);
  CHECK(world.size() == 3);
}

TEST(sim_objects_stay_in_ascending_id_order_after_despawn) {
  World world;
  std::vector<ObjectId> ids;
  for (int i = 0; i < 8; ++i) ids.push_back(world.spawn(NativeClass::unit, nullptr));
  world.despawn(ids[2]);
  world.despawn(ids[5]);
  world.spawn(NativeClass::unit, nullptr);

  // Iteration order is world state. It must be spawn order, which -- because
  // ids are monotonic and despawn only erases -- is ascending id order.
  ObjectId previous = 0;
  for (const WorldObject& slot : world.objects()) {
    CHECK(slot.id > previous);
    previous = slot.id;
  }
  CHECK(world.size() == 7);
}

TEST(sim_internal_objects_share_the_handle_space) {
  World world;
  const ObjectId unit = world.spawn(NativeClass::unit, nullptr);
  const ObjectId settlement = world.spawn_internal(InternalKind::settlement);
  const ObjectId next = world.spawn(NativeClass::building, nullptr);

  // One flat id space for every simulated thing -- units, buildings, decor,
  // settlements, holders, warehouses, items, singletons *and queries*. Not a
  // per-type table. The dumps settle it: no handle is used by two types in the
  // same file, across 9,075 blocks.
  CHECK(unit == 1 && settlement == 2 && next == 3);
  REQUIRE(world.find(settlement) != nullptr);
  CHECK(world.find(settlement)->object == nullptr);
  CHECK(world.find(settlement)->internal == InternalKind::settlement);
  CHECK(internal_type_name(InternalKind::settlement) == "CVXSettlement");
}

// ==========================================================================
// the held-position invariant
// ==========================================================================

TEST(sim_a_held_object_has_no_position_of_its_own) {
  World world;
  const ObjectId building = world.spawn(NativeClass::building, nullptr);
  const ObjectId holder = world.spawn_internal(InternalKind::holder);
  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr);

  CHECK(world.set_position(building, pt(1000, 2000)));
  CHECK(world.set_position(soldier, pt(1500, 2500)));

  CHECK(world.put_in_holder(holder, building));
  CHECK(world.put_in_holder(soldier, holder));

  // 1,217 of the 1,225 objects at (-1,-1) in the dumps have a holder, and of
  // the 6,355 with a real position, not one does.
  REQUIRE(world.state(soldier) != nullptr);
  CHECK(world.state(soldier)->position == kHeldPosition);
  CHECK(world.state(soldier)->holder == holder);
  CHECK(world.state(soldier)->is_held());

  // Its location is its holder's, through however many levels of container.
  CHECK(world.resolve_position(soldier) == pt(1000, 2000));
  CHECK(world.resolve_position(holder) == pt(1000, 2000));
}

TEST(sim_setting_a_position_on_a_held_object_is_refused) {
  World world;
  const ObjectId box = world.spawn_internal(InternalKind::holder);
  const ObjectId item = world.spawn(NativeClass::unit, nullptr);
  world.set_position(box, pt(500, 500));
  CHECK(world.put_in_holder(item, box));

  // The refusal is the point. Writing a shadow position under a holder gives
  // two answers to one question, and the disagreement is a desync.
  CHECK(!world.set_position(item, pt(9, 9)));
  CHECK(world.state(item)->position == kHeldPosition);

  CHECK(world.remove_from_holder(item, pt(600, 700)));
  CHECK(world.state(item)->holder == kNoObject);
  CHECK(world.state(item)->position == pt(600, 700));
  CHECK(world.set_position(item, pt(9, 9)));
}

TEST(sim_holder_cycles_are_refused) {
  World world;
  const ObjectId a = world.spawn_internal(InternalKind::holder);
  const ObjectId b = world.spawn_internal(InternalKind::holder);
  world.set_position(a, pt(10, 10));
  CHECK(world.put_in_holder(b, a));
  // a inside b would leave the pair with no position between them.
  CHECK(!world.put_in_holder(a, b));
  CHECK(!world.put_in_holder(a, a));
  CHECK(world.resolve_position(b) == pt(10, 10));
}

// ==========================================================================
// SyncFlags
// ==========================================================================

TEST(sim_sync_flags_encode_a_one_hot_owner) {
  ObjectState state;
  state.owner = kNoPlayer;
  // 1,111 unowned objects in the dumps, all CVXDecorObj, all 0x80000000.
  CHECK(pack_sync_flags(state) == 0x80000000u);

  const std::pair<PlayerId, std::uint32_t> expected[] = {
      {0, 0x0001u}, {1, 0x0002u}, {2, 0x0004u}, {3, 0x0008u},
      {4, 0x0010u}, {kNeutralWildlife, 0x4000u}, {kNeutralPassive, 0x8000u},
  };
  for (const auto& [player, bit] : expected) {
    state.owner = player;
    const std::uint32_t word = pack_sync_flags(state);
    CHECK((word & kSyncOwnerMask) == bit);
    CHECK(unpack_owner(word) == player);
  }

  // Not one object in 7,580 has two owner bits set, so a word that does is
  // malformed and must not read as the lowest of them.
  CHECK(unpack_owner(0x80000003u) == kNoPlayer);
}

TEST(sim_unit_and_building_bits_partition_the_class_tree) {
  // 3,484 units + 2,551 buildings = 6,035, and the remaining 1,545 objects are
  // exactly the decor, item holders and script objects that are neither.
  const NativeClass units[] = {NativeClass::unit,  NativeClass::hero, NativeClass::druid,
                               NativeClass::wagon, NativeClass::ship, NativeClass::flying_unit};
  const NativeClass buildings[] = {NativeClass::building, NativeClass::town_hall,
                                   NativeClass::outpost,  NativeClass::barrack,
                                   NativeClass::tavern,   NativeClass::gate,
                                   NativeClass::catapult, NativeClass::teleport};
  const NativeClass neither[] = {NativeClass::decor, NativeClass::item_holder,
                                 NativeClass::script_obj};

  for (NativeClass c : units) {
    const ObjectFlags f = flags_for_native_class(c);
    CHECK(f.is_unit && !f.is_building);
  }
  for (NativeClass c : buildings) {
    const ObjectFlags f = flags_for_native_class(c);
    CHECK(f.is_building && !f.is_unit);
  }
  for (NativeClass c : neither) {
    const ObjectFlags f = flags_for_native_class(c);
    CHECK(!f.is_building && !f.is_unit);
  }
  // Bit 24 is set on all 31 heroes in the corpus and on nothing else.
  CHECK(flags_for_native_class(NativeClass::hero).is_hero);
  CHECK(!flags_for_native_class(NativeClass::unit).is_hero);
}

TEST(sim_sync_flags_round_trip) {
  ObjectState state;
  state.owner = 3;
  state.flags.is_unit = true;
  state.flags.is_hero = true;
  state.flags.has_active_path = true;
  const std::uint32_t word = pack_sync_flags(state);
  CHECK((word & kSyncLive) != 0);
  const ObjectFlags back = unpack_flags(word);
  CHECK(back.is_unit && back.is_hero && back.has_active_path && !back.is_building);
  CHECK(unpack_owner(word) == 3);
}

TEST(sim_spawning_caches_the_category_bits) {
  World world;
  const ObjectId hero = world.spawn(NativeClass::hero, nullptr);
  const ObjectId gate = world.spawn(NativeClass::gate, nullptr);
  const ObjectId decor = world.spawn(NativeClass::decor, nullptr);
  CHECK(world.state(hero)->flags.is_unit && world.state(hero)->flags.is_hero);
  CHECK(world.state(gate)->flags.is_building);
  CHECK(!world.state(decor)->flags.is_unit && !world.state(decor)->flags.is_building);
}

// ==========================================================================
// composite allocation
// ==========================================================================

TEST(sim_settlement_composites_take_three_consecutive_ids) {
  World world;
  world.spawn(NativeClass::decor, nullptr);  // offset, so 1/2/3 is not a fluke

  const World::SettlementIds a = world.spawn_settlement(2);
  // 328 of 328 across all nine dumps, no exceptions: a CVXSettlement is always
  // immediately followed by a CVXHolder at handle+1 and a CVXWarehouse at
  // handle+2. Allocation order is observable state.
  CHECK(a.holder == a.settlement + 1);
  CHECK(a.warehouse == a.settlement + 2);
  CHECK(world.find(a.settlement)->internal == InternalKind::settlement);
  CHECK(world.find(a.holder)->internal == InternalKind::holder);
  CHECK(world.find(a.warehouse)->internal == InternalKind::warehouse);
  CHECK(world.state(a.warehouse)->owner == 2);

  const World::SettlementIds b = world.spawn_settlement(kNoPlayer);
  CHECK(b.settlement == a.warehouse + 1);
  CHECK(b.holder == b.settlement + 1 && b.warehouse == b.settlement + 2);
}

TEST(sim_ships_take_a_holder_at_the_next_id) {
  World world;
  world.spawn(NativeClass::unit, nullptr);
  const World::ShipIds ship = world.spawn_ship(nullptr);
  // 3 of 3 in the corpus, and the only place a standalone CVXHolder occurs.
  CHECK(ship.holder == ship.ship + 1);
  CHECK(world.find(ship.ship)->object->native_class() == NativeClass::ship);
  CHECK(world.find(ship.holder)->internal == InternalKind::holder);
}

TEST(sim_singletons_take_three_consecutive_ids) {
  World world;
  world.spawn(NativeClass::decor, nullptr);
  const World::SingletonIds s = world.spawn_singletons();
  // Exactly one group per session in all nine dumps (779/780/781, 640/641/642).
  CHECK(s.player_bonus == s.ai_helper + 1);
  CHECK(s.player_scripts == s.ai_helper + 2);
  CHECK(internal_type_name(InternalKind::ai_helper) == "CVXAIHelper");
  CHECK(internal_type_name(InternalKind::player_bonus) == "CVXPlayerBonus");
  CHECK(internal_type_name(InternalKind::player_scripts) == "CVXPlayerScripts");
}

// ==========================================================================
// queries as objects
// ==========================================================================

TEST(sim_a_query_is_an_object_with_a_handle) {
  World world;
  const ObjectId unit = world.spawn(NativeClass::unit, nullptr);
  const ObjectId q = world.create_query(objs_in_circle(pt(0, 0), 100, ClassFilter{}));
  const ObjectId after = world.spawn(NativeClass::unit, nullptr);

  // This is the finding that makes a stateless find() wrong: a query takes a
  // handle out of the same monotonic counter as a unit, so its allocation
  // shifts every later id and feeds the slot hash.
  CHECK(q == unit + 1);
  CHECK(after == q + 1);
  REQUIRE(world.find(q) != nullptr);
  CHECK(world.find(q)->internal == InternalKind::query);
  REQUIRE(world.query_spec(q) != nullptr);
  CHECK(world.query_spec(q)->kind == QueryKind::map_area_circle);
  CHECK(world.query_spec(q)->radius == 100);
  CHECK(query_type_name(QueryKind::map_area_circle) == "CVXMapAreaQuery<TCircleArea>");
}

TEST(sim_a_query_lives_and_dies_like_any_object) {
  World world;
  const ObjectId q = world.create_query(group_query(7));
  CHECK(world.query_spec(q) != nullptr);
  CHECK(world.destroy_query(q));
  CHECK(world.find(q) == nullptr);
  CHECK(world.query_spec(q) == nullptr);
  // And the id is not handed out again.
  CHECK(world.spawn(NativeClass::unit, nullptr) != q);
  // A non-query object is not a query, whatever its handle.
  const ObjectId unit = world.spawn(NativeClass::unit, nullptr);
  CHECK(world.query_spec(unit) == nullptr);
}

TEST(sim_queries_re_evaluate_rather_than_snapshot) {
  World world;
  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  world.set_position(a, pt(10, 10));
  const ObjectId q = world.create_query(objs_in_circle(pt(0, 0), 100, ClassFilter{}));

  std::vector<ObjectId> found;
  CHECK(world.evaluate_query(q, found) == 1);

  const ObjectId b = world.spawn(NativeClass::unit, nullptr);
  world.set_position(b, pt(20, 20));
  // WaitNonEmptyQuery(q, ms) in the host API only makes sense against something
  // that re-reads the world, which is what a persistent query object is for.
  CHECK(world.evaluate_query(q, found) == 2);
  CHECK(found[0] == a && found[1] == b);

  world.set_position(b, pt(9000, 9000));
  CHECK(world.evaluate_query(q, found) == 1);
}

TEST(sim_set_op_queries_compose_two_query_handles) {
  World world;
  const ObjectId near = world.spawn(NativeClass::unit, nullptr);
  const ObjectId far = world.spawn(NativeClass::unit, nullptr);
  const ObjectId both = world.spawn(NativeClass::unit, nullptr);
  world.set_position(near, pt(10, 0));
  world.set_position(far, pt(900, 0));
  world.set_position(both, pt(100, 0));
  world.set_owner(near, 1);
  world.set_owner(far, 1);
  world.set_owner(both, 1);

  const ObjectId circle = world.create_query(objs_in_circle(pt(0, 0), 200, ClassFilter{}));
  const ObjectId ring = world.create_query(objs_in_circle(pt(950, 0), 200, ClassFilter{}));

  std::vector<ObjectId> found;
  const ObjectId u = world.create_query(set_op(SetOp::set_union, circle, ring));
  CHECK(world.evaluate_query(u, found) == 3);
  // A set operation's result is still in ascending id order: both operands are,
  // and the algebra is a merge.
  CHECK(found[0] == near && found[1] == far && found[2] == both);

  const ObjectId i = world.create_query(set_op(SetOp::intersect, circle, ring));
  CHECK(world.evaluate_query(i, found) == 0);

  const ObjectId d = world.create_query(set_op(SetOp::subtract, u, ring));
  CHECK(world.evaluate_query(d, found) == 2);
  CHECK(found[0] == near && found[1] == both);

  // Set ops nest: their operands are query handles, not results.
  CHECK(world.query_spec(d)->lhs == u);
  CHECK(world.query_spec(u)->lhs == circle);
}

TEST(sim_a_query_over_a_dead_operand_is_empty_not_wrong) {
  World world;
  const ObjectId a = world.create_query(group_query(1));
  const ObjectId composed = world.create_query(set_op(SetOp::set_union, a, kNoObject));
  std::vector<ObjectId> found;
  CHECK(world.evaluate_query(composed, found) == 0);
}

// ==========================================================================
// class filters
// ==========================================================================

TEST(sim_class_filters_match_the_class_tree) {
  TinyGraph tiny;
  World world;
  world.set_class_graph(&tiny.graph);

  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr, tiny.soldier);
  const ObjectId plain = world.spawn(NativeClass::unit, nullptr, tiny.unit);
  const ObjectId fort = world.spawn(NativeClass::building, nullptr, tiny.building);
  for (ObjectId id : {soldier, plain, fort}) world.set_position(id, pt(0, 0));

  std::vector<ObjectId> found;
  // Abstract classes match their whole subtree: the dumps' sight queries filter
  // on `Unit`, `Building` and `Object` as well as on `Crow` and `Hen`.
  world.objects_in_radius(pt(0, 0), 10, ClassFilter::parse("Unit", &tiny.graph), found);
  CHECK(found.size() == 2 && found[0] == soldier && found[1] == plain);

  world.objects_in_radius(pt(0, 0), 10, ClassFilter::parse("Soldier", &tiny.graph), found);
  CHECK(found.size() == 1 && found[0] == soldier);

  // Comma-separated lists ship: ObjsInSight(this, "Military,Tower").
  world.objects_in_radius(pt(0, 0), 10, ClassFilter::parse("Soldier,Fort", &tiny.graph), found);
  CHECK(found.size() == 2 && found[0] == soldier && found[1] == fort);

  // An empty filter matches everything.
  world.objects_in_radius(pt(0, 0), 10, ClassFilter{}, found);
  CHECK(found.size() == 3);

  // A filter that named classes and resolved none matches *nothing*, so that a
  // typo in the data does not silently become a world-wide query.
  world.objects_in_radius(pt(0, 0), 10, ClassFilter::parse("Nonesuch", &tiny.graph), found);
  CHECK(found.empty());

  // Deduplication keeps two spellings of one filter the same state, and
  // therefore the same hash.
  CHECK(ClassFilter::parse("Unit,Unit", &tiny.graph) == ClassFilter::parse("Unit", &tiny.graph));
  CHECK(ClassFilter::parse(" Unit , Fort ", &tiny.graph) ==
        ClassFilter::parse("Unit,Fort", &tiny.graph));
}

// ==========================================================================
// spatial queries
// ==========================================================================

TEST(sim_radius_queries_are_inclusive_and_ordered) {
  World world;
  std::vector<ObjectId> ids;
  for (int i = 0; i < 5; ++i) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr);
    world.set_position(id, pt(i * 100, 0));
    ids.push_back(id);
  }
  std::vector<ObjectId> found;
  // Exactly on the boundary is inside: the comparison is `d2 > r2`.
  CHECK(world.objects_in_radius(pt(0, 0), 200, ClassFilter{}, found) == 3);
  CHECK(found[0] == ids[0] && found[1] == ids[1] && found[2] == ids[2]);
  // Radius 0 means the whole map, which is what a query with no bound wants.
  CHECK(world.objects_in_radius(pt(0, 0), 0, ClassFilter{}, found) == 5);
}

TEST(sim_rect_queries_include_their_corners_and_stay_ordered) {
  World world;
  std::vector<ObjectId> ids;
  // A cross of five: the centre, and one just inside each edge of the box
  // below, plus two outside it.
  const Point places[] = {pt(100, 100), pt(300, 300), pt(500, 500),
                          pt(99, 300),  pt(300, 501)};
  for (const Point& place : places) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr);
    world.set_position(id, place);
    ids.push_back(id);
  }

  std::vector<ObjectId> found;
  // Corners inclusive, both of them: (100,100) is the top-left corner and
  // (500,500) the bottom-right, and both are in. `sim/area.hpp` has the
  // executable's own rule, and `test_area.cpp` asserts this sweep agrees with
  // `AreaShape::contains` point for point rather than letting the two drift.
  CHECK(world.objects_in_rect(100, 100, 500, 500, ClassFilter{}, found) == 3);
  REQUIRE(found.size() == 3);
  CHECK(found[0] == ids[0]);
  CHECK(found[1] == ids[1]);
  CHECK(found[2] == ids[2]);
  // Ascending id, which is spawn order, which is the order every other query
  // in this file yields.
  CHECK(found[0] < found[1]);
  CHECK(found[1] < found[2]);

  // One unit outside on either axis is outside. `ids[3]` is at x=99 against a
  // left edge of 100 and `ids[4]` at y=501 against a bottom of 500, so a test
  // written with `<` where it should be `<=` -- or the other way round -- moves
  // one of these across the line.
  CHECK(world.objects_in_rect(99, 100, 500, 500, ClassFilter{}, found) == 4);
  CHECK(world.objects_in_rect(100, 100, 500, 501, ClassFilter{}, found) == 4);

  // A degenerate box holds exactly what stands on it, which is what makes the
  // inclusive reading observable at all: an exclusive one would answer nothing
  // here for every rectangle the maps declare with a zero-width axis.
  CHECK(world.objects_in_rect(100, 100, 100, 100, ClassFilter{}, found) == 1);
  CHECK(found[0] == ids[0]);
}

TEST(sim_a_rect_query_normalises_the_corners_the_map_gave_it) {
  // `map.obj.xml` promises nothing about corner order, and an inverted pair run
  // through the containment test straight would select **nothing** -- an empty
  // set, which reads exactly like a correct answer about an empty region. So
  // `objs_in_rect` sorts, and it is the spec that carries the sorted pair,
  // which is what the save and the hash then see.
  const QuerySpec forward = objs_in_rect(100, 100, 500, 500, ClassFilter{});
  const QuerySpec reversed = objs_in_rect(500, 500, 100, 100, ClassFilter{});
  CHECK(forward == reversed);
  CHECK(reversed.left == 100);
  CHECK(reversed.top == 100);
  CHECK(reversed.right == 500);
  CHECK(reversed.bottom == 500);
  // One axis inverted and not the other, which is the case a single
  // `if (left > right) swap(all four)` would get wrong.
  const QuerySpec mixed = objs_in_rect(500, 100, 100, 500, ClassFilter{});
  CHECK(mixed == forward);

  World world;
  const ObjectId inside = world.spawn(NativeClass::unit, nullptr);
  world.set_position(inside, pt(300, 300));
  std::vector<ObjectId> found;
  // `REQUIRE`: without normalisation this selects nothing, and indexing an
  // empty vector on the next line turns a clean failure into a SIGSEGV that
  // kills the run before the harness prints which test failed.
  REQUIRE(world.evaluate_query(world.create_query(reversed), found) == 1);
  CHECK(found[0] == inside);
}

TEST(sim_a_rect_query_evaluates_as_a_rectangle_and_not_as_a_circle) {
  // Through `evaluate_query`, which is the only path a script takes, and with a
  // layout that tells the two branches apart. **The obvious version of this
  // does not:** a rectangle spec leaves `center` at the origin and `radius` at
  // zero, and `objects_in_radius` reads a zero radius as *the whole map* -- so
  // a `map_area_rect` arm wired to the circle sweep answers with everything and
  // still passes any case where everything happens to be the right answer.
  World world;
  const ObjectId inside = world.spawn(NativeClass::unit, nullptr);
  const ObjectId outside = world.spawn(NativeClass::unit, nullptr);
  const ObjectId far_off = world.spawn(NativeClass::unit, nullptr);
  world.set_position(inside, pt(300, 300));
  world.set_position(outside, pt(900, 300));
  world.set_position(far_off, pt(300, 900));

  const ObjectId q = world.create_query(objs_in_rect(100, 100, 500, 500, ClassFilter{}));
  std::vector<ObjectId> found;
  CHECK(world.evaluate_query(q, found) == 1);
  REQUIRE(found.size() == 1);
  CHECK(found[0] == inside);
  // Named so the failure says which branch ran: the circle arm would give all
  // three, and a bounding-circle approximation of this box would give two.
  CHECK(found[0] != outside);
  CHECK(found[0] != far_off);
}

TEST(sim_no_area_query_finds_a_garrison) {
  // A held object's own position is (-1,-1), and in the original that is no
  // grid cell (0x0053d030): every area sweep walks the cells, so a garrison is
  // found by none -- not where its building is, and not where its holder is.
  // These queries once resolved it to its holder, and a settlement's holder
  // record stands at (0, 0): every garrison on the map was found in the
  // corner, by `ObjsInCircle`, `ObjsInRect`, `EnemyInRange` and the rest.
  World world;
  const World::SettlementIds town = world.spawn_settlement(0);
  const ObjectId fort = world.spawn(NativeClass::building, nullptr);
  const ObjectId guard = world.spawn(NativeClass::unit, nullptr);
  world.set_position(fort, pt(5000, 5000));
  world.set_position(guard, pt(5010, 5000));
  REQUIRE(world.resolve_position(town.holder) == pt(0, 0));

  std::vector<ObjectId> found;
  CHECK(world.objects_in_radius(pt(5000, 5000), 20, ClassFilter{}, found) == 2);
  REQUIRE(world.put_in_holder(guard, town.holder));
  REQUIRE(world.resolve_position(guard) == pt(0, 0));

  CHECK(world.objects_in_radius(pt(5000, 5000), 20, ClassFilter{}, found) == 1);
  CHECK(found.size() == 1 && found[0] == fort);
  CHECK(world.objects_in_radius(pt(0, 0), 10, ClassFilter{}, found) == 0);
  CHECK(world.objects_in_rect(4900, 4900, 5100, 5100, ClassFilter{}, found) == 1);
  CHECK(world.objects_in_rect(-100, -100, 100, 100, ClassFilter{}, found) == 0);
  CHECK(world.objects_of_class_for_player(ClassFilter{}, kNoPlayer, pt(0, 0), 10, found) == 0);
  CHECK(world.objects_of_class_for_player_in_rect(ClassFilter{}, kNoPlayer, -100, -100, 100,
                                                  100, found) == 0);
  // The raw sweep `EnemyInRange` takes, over a box that covers both the
  // corner and the `(-1, -1)` marker itself.
  CHECK(world.objects_located_in_rect(-100, -100, 100, 100, found) == 0);
  // Still the holder's: what it holds is a different question.
  CHECK(world.contents_of(town.holder, found) == 1 && found[0] == guard);
}

TEST(sim_sight_queries_use_the_observers_sight_property) {
  TinyGraph tiny;
  World world;
  world.set_class_graph(&tiny.graph);

  const ObjectId watcher = world.spawn(NativeClass::unit, nullptr, tiny.unit);   // sight 100
  const ObjectId sharp = world.spawn(NativeClass::unit, nullptr, tiny.soldier);  // sight 200
  const ObjectId close = world.spawn(NativeClass::unit, nullptr, tiny.unit);
  const ObjectId distant = world.spawn(NativeClass::unit, nullptr, tiny.unit);
  world.set_position(watcher, pt(0, 0));
  world.set_position(sharp, pt(0, 0));
  world.set_position(close, pt(50, 0));
  world.set_position(distant, pt(150, 0));

  std::vector<ObjectId> found;
  CHECK(world.objects_in_sight(watcher, ClassFilter{}, found) == 2);  // sharp, close
  CHECK(world.objects_in_sight(sharp, ClassFilter{}, found) == 3);
  // Nobody sees themselves.
  for (ObjectId id : found) CHECK(id != sharp);
}

TEST(sim_class_player_area_selects_on_owner_and_class) {
  TinyGraph tiny;
  World world;
  world.set_class_graph(&tiny.graph);

  const ObjectId mine = world.spawn(NativeClass::unit, nullptr, tiny.soldier);
  const ObjectId theirs = world.spawn(NativeClass::unit, nullptr, tiny.soldier);
  const ObjectId my_fort = world.spawn(NativeClass::building, nullptr, tiny.building);
  world.set_owner(mine, 1);
  world.set_owner(theirs, 2);
  world.set_owner(my_fort, 1);
  for (ObjectId id : {mine, theirs, my_fort}) world.set_position(id, pt(100, 100));

  std::vector<ObjectId> found;
  CHECK(world.objects_of_class_for_player(ClassFilter{}, 1, Point{}, 0, found) == 2);
  CHECK(world.objects_of_class_for_player(ClassFilter::parse("Soldier", &tiny.graph), 1, Point{}, 0,
                                          found) == 1);
  CHECK(found[0] == mine);
  // kNoPlayer means any owner.
  CHECK(world.objects_of_class_for_player(ClassFilter{}, kNoPlayer, Point{}, 0, found) == 3);
  // And the circle still applies.
  CHECK(world.objects_of_class_for_player(ClassFilter{}, kNoPlayer, pt(9000, 9000), 10, found) ==
        0);
}

/// `Catapult::IsBuilt` and `Catapult::SetBuilt` -- the flag, its writer, and
/// the two miss paths, which disagree with each other in the original.
TEST(a_catapult_is_born_unbuilt_and_only_setbuilt_changes_that) {
  imperivm::core::script::HostRegistry registry;
  imperivm::core::script::declare_shipped_surface(registry);
  (void)register_world_host(registry);

  World world;
  const ObjectId engine_obj = world.spawn(NativeClass::catapult, nullptr);

  const auto call = [&](const char* name,
                        imperivm::core::script::Value receiver)
      -> imperivm::core::script::HostOutcome {
    const std::uint32_t index = registry.find(imperivm::core::script::CallKind::member, name, 0);
    if (index == imperivm::core::script::kUnresolvedHost) {
      return imperivm::core::script::HostOutcome::failed("not declared");
    }
    const imperivm::core::script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return imperivm::core::script::HostOutcome::failed("no body");
    imperivm::core::script::CallContext ctx;
    HostContext state;
    state.world = &world;
    std::vector<imperivm::core::script::Value> args{receiver};
    ctx.arguments = args;
    ctx.user = &state;
    ctx.name = name;
    ctx.kind = imperivm::core::script::CallKind::member;
    return entry.fn(ctx);
  };
  const imperivm::core::script::Value self = imperivm::core::script::Value::object(kTypeObj, engine_obj);

  // The `CVXCatapult` constructor zeroes `+0x208` at 0x004e1ede.
  CHECK(call("IsBuilt", self).value.as_integer() == 0);
  CHECK(call("SetBuilt", self).status == imperivm::core::script::HostStatus::ok);
  CHECK(call("IsBuilt", self).value.as_integer() == 1);

  // **The two miss paths differ, and both are `ok`.** `IsBuilt` (0x004e2dc0)
  // answers false; `SetBuilt` (0x004e2e20) returns having written nothing.
  // Neither traps -- and neither prints, because 0x00686eb0 is a bare `ret`
  // in the retail build.
  const imperivm::core::script::Value nobody = imperivm::core::script::Value::object(kTypeObj, 9999);
  const imperivm::core::script::HostOutcome missed_read = call("IsBuilt", nobody);
  CHECK(missed_read.status == imperivm::core::script::HostStatus::ok);
  CHECK(missed_read.value.as_integer() == 0);
  CHECK(call("SetBuilt", nobody).status == imperivm::core::script::HostStatus::ok);
  // ...and the write really did not land anywhere: the live catapult is the
  // only object in the world that could have taken it.
  CHECK(call("IsBuilt", self).value.as_integer() == 1);
}

/// `Catapult::SetBuildFrame/1` **is bound, and moves no simulation state.**
///
/// `gbr.exe` 0x004e2ed0 maps a remaining-damage count against the class's
/// `maxhealth` at `[obj+0xc8]` onto an animation frame and stores it in
/// `[obj+0x20c]`. It touches `+0x208` not at all, so it is not part of the
/// built flag; it is the *appearance* of a half-assembled catapult. It used to
/// be dropped, and playtest #19 is what that drew: a finished engine at one
/// point of health. It is kept now as `WorldObject::build_frame`, presentation
/// (`test_anim.cpp` holds the frame), so the hash still does not move -- and an
/// object with no entity, as here, has no frame to keep at all.
TEST(sim_setbuildframe_is_bound_and_moves_no_hashed_state) {
  imperivm::core::script::HostRegistry registry;
  imperivm::core::script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  const std::uint32_t index =
      registry.find(imperivm::core::script::CallKind::member, "SetBuildFrame", 1);
  REQUIRE(index != imperivm::core::script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);

  World world;
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;
  const ObjectId engine = world.spawn(NativeClass::catapult, nullptr);
  const std::uint64_t before = world.state_hash();

  std::vector<imperivm::core::script::Value> args{
      imperivm::core::script::Value::object(kTypeObj, engine),
      imperivm::core::script::Value::integer(400)};
  imperivm::core::script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "SetBuildFrame";
  ctx.kind = imperivm::core::script::CallKind::member;
  CHECK(registry.entry(index).fn(ctx).status == imperivm::core::script::HostStatus::ok);
  // No hashed state moves -- the hash is where it would show if a later change
  // made the frame simulation state quietly.
  CHECK(world.state_hash() == before);
  CHECK(world.find(engine)->build_frame == -1);

  // A receiver naming no object runs too, rather than refusing: the original
  // reports into a bare `ret` and returns.
  args[0] = imperivm::core::script::Value::object(kTypeObj, 9999);
  ctx.arguments = args;
  CHECK(registry.entry(index).fn(ctx).status == imperivm::core::script::HostStatus::ok);
}

/// The flag is state: it has to survive a save and it has to move the hash.
///
/// `pack_object_flags` grew a ninth bit for it, and the word it is packed into
/// grew from one byte to two. Both halves are asserted here because the last
/// two flags added to that word -- `in_party` and `unspawned` -- were each
/// dropped by a save for a while, and what hid it was a fixture that never set
/// them.
TEST(sim_the_built_flag_is_hashed_and_round_trips) {
  World plain;
  const ObjectId a = plain.spawn(NativeClass::catapult, nullptr);
  const std::uint64_t unbuilt = plain.state_hash();
  plain.find(a)->state.flags.built = true;
  CHECK(plain.state_hash() != unbuilt);

  // Every flag the packer can write, set at once, so that a packer that
  // dropped one is caught by this fixture rather than by the next one.
  World world;
  const ObjectId id = world.spawn(NativeClass::catapult, nullptr);
  ObjectFlags& flags = world.find(id)->state.flags;
  flags.is_unit = true;
  flags.is_building = true;
  flags.is_hero = true;
  flags.has_active_path = true;
  flags.hidden = true;
  flags.in_party = true;
  flags.unspawned = true;
  flags.no_ai = true;
  flags.built = true;
  flags.in_air = true;
  flags.noselect = true;
  flags.building = true;
  flags.autocast = true;
  flags.on_minimap = true;
  flags.enemies_near = true;
  flags.friends_near = true;
  flags.gate_open = true;
  flags.messenger = true;

  std::vector<std::byte> bytes;
  world.serialize(bytes);
  World loaded;
  REQUIRE(loaded.deserialize(bytes).ok());
  const WorldObject* back = loaded.find(id);
  REQUIRE(back != nullptr);
  CHECK(back->state.flags.is_unit && back->state.flags.is_building);
  CHECK(back->state.flags.is_hero && back->state.flags.has_active_path);
  CHECK(back->state.flags.hidden && back->state.flags.in_party);
  CHECK(back->state.flags.unspawned && back->state.flags.no_ai);
  CHECK(back->state.flags.built);
  CHECK(back->state.flags.in_air && back->state.flags.noselect);
  CHECK(back->state.flags.building && back->state.flags.autocast);
  CHECK(back->state.flags.on_minimap);
  CHECK(back->state.flags.enemies_near && back->state.flags.friends_near);
  CHECK(back->state.flags.gate_open && back->state.flags.messenger);
  CHECK(loaded.state_hash() == world.state_hash());
}

TEST(sim_settlement_queries_split_units_from_buildings) {
  World world;
  const World::SettlementIds town = world.spawn_settlement(1);
  const ObjectId hall = world.spawn(NativeClass::town_hall, nullptr);
  const ObjectId guard = world.spawn(NativeClass::unit, nullptr);
  const ObjectId stray = world.spawn(NativeClass::unit, nullptr);
  world.find(hall)->settlement = town.settlement;
  world.find(guard)->settlement = town.settlement;

  std::vector<ObjectId> found;
  CHECK(world.units_in_settlement(town.settlement, ClassFilter{}, found) == 1);
  CHECK(found[0] == guard);
  CHECK(world.buildings_in_settlement(town.settlement, ClassFilter{}, found) == 1);
  CHECK(found[0] == hall);
  CHECK(world.units_in_settlement(kNoObject, ClassFilter{}, found) == 0);
  (void)stray;
}

/// The ring: `UnitsAroundSettlement`, which is the same query kind under a
/// different mode and is the harder half of the three.
///
/// Every assertion here is one of the collector's own steps at `gbr.exe`
/// 0x005c58f0, and the fixture is built so that a fault in any one of them
/// changes an answer. Two towers rather than one, because a single circle
/// cannot tell "each building's own sight" from "the settlement's"; two towers
/// whose circles overlap, because that is the only thing the dedup can be seen
/// through; and a garrisoned unit standing inside a tower's circle, because
/// `resolve_position` would otherwise put the garrison in the ring and collapse
/// mode 0 into mode 1.
TEST(sim_the_settlement_ring_sweeps_each_buildings_own_sight) {
  TinyGraph tiny;
  World world;
  world.set_class_graph(&tiny.graph);
  const World::SettlementIds town = world.spawn_settlement(1);

  // Two towers, 1,000 apart, each seeing 400. Their circles do not meet, so
  // anything found near the second one was found *by* the second one.
  const ObjectId west = world.spawn(NativeClass::building, nullptr, tiny.building);
  const ObjectId east = world.spawn(NativeClass::building, nullptr, tiny.building);
  world.set_position(west, pt(0, 0));
  world.set_position(east, pt(1000, 0));
  for (ObjectId id : {west, east}) {
    world.find(id)->settlement = town.settlement;
    world.find(id)->sight = 400;
    // `World::spawn` leaves health at zero, and the ring skips the dead, so a
    // tower with no health is a tower the sweep cannot see out of its own
    // circle -- which is a property of this fixture, not of the collector.
    world.find(id)->state.health = 5000;
  }

  const auto place = [&](std::int32_t x, std::int32_t y) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, tiny.soldier);
    world.set_position(id, pt(x, y));
    world.find(id)->state.health = 10;
    return id;
  };

  const ObjectId near_west = place(100, 0);
  const ObjectId near_east = place(900, 0);
  const ObjectId between = place(500, 0);  // outside both: 500 > 400
  const ObjectId on_the_edge = place(401, 0);  // one unit outside, to start

  std::vector<ObjectId> found;
  REQUIRE(world.units_in_settlement(town.settlement, ClassFilter{}, found,
                                    SettlementScope::ring) == 4);
  // The two towers themselves are inside their own circles and carry no class
  // filter to exclude them -- the original excludes nobody here, unlike
  // `objects_in_sight`, which skips its observer.
  CHECK(found[0] == west && found[1] == east);
  CHECK(found[2] == near_west && found[3] == near_east);
  // 500 from either tower is outside both.
  for (ObjectId id : found) CHECK(id != between);
  // ...and exactly 400 is inside: 0x005c5304 is `cmp dist2, r2` / `jg skip`.
  CHECK(world.units_in_settlement(town.settlement, ClassFilter::of(tiny.soldier), found,
                                  SettlementScope::ring) == 2);
  CHECK(found[0] == near_west && found[1] == near_east);
  world.set_position(on_the_edge, pt(400, 0));
  CHECK(world.units_in_settlement(town.settlement, ClassFilter::of(tiny.soldier), found,
                                  SettlementScope::ring) == 3);
  CHECK(found[2] == on_the_edge);
  world.set_position(on_the_edge, pt(401, 0));
  CHECK(world.units_in_settlement(town.settlement, ClassFilter::of(tiny.soldier), found,
                                  SettlementScope::ring) == 2);
}

TEST(sim_the_settlement_ring_deduplicates_sorts_and_skips_the_dead) {
  TinyGraph tiny;
  World world;
  world.set_class_graph(&tiny.graph);
  const World::SettlementIds town = world.spawn_settlement(1);

  // Overlapping circles this time: 200 apart, each seeing 400, so the middle
  // of the map is inside both.
  const ObjectId west = world.spawn(NativeClass::building, nullptr, tiny.building);
  const ObjectId east = world.spawn(NativeClass::building, nullptr, tiny.building);
  world.set_position(west, pt(0, 0));
  world.set_position(east, pt(200, 0));
  for (ObjectId id : {west, east}) {
    world.find(id)->settlement = town.settlement;
    world.find(id)->sight = 400;
    world.find(id)->state.health = 5000;
  }

  const auto place = [&](std::int32_t x, std::int32_t health) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, tiny.soldier);
    world.set_position(id, pt(x, 0));
    world.find(id)->state.health = health;
    return id;
  };

  // Spawned late-to-early in position, so an unsorted result would come out
  // in a different order from a sorted one.
  const ObjectId seen_twice = place(100, 10);
  const ObjectId corpse = place(110, 0);
  const ObjectId template_unit = place(120, 10);
  world.find(template_unit)->state.flags.unspawned = true;

  const ClassFilter soldiers = ClassFilter::of(tiny.soldier);
  std::vector<ObjectId> found;
  // Inside both circles and reported once: 0x005c5ac3 sorts (0x004fb510) and
  // uniques (0x0050e300) before returning.
  REQUIRE(world.units_in_settlement(town.settlement, soldiers, found,
                                    SettlementScope::ring) == 1);
  CHECK(found[0] == seen_twice);
  // The dead are skipped -- `vtbl + 0x50` at 0x005c5298 -- and so are spawn
  // templates, which is `collect`'s bit-27 test.
  for (ObjectId id : found) CHECK(id != corpse && id != template_unit);
  world.find(corpse)->state.health = 1;
  CHECK(world.units_in_settlement(town.settlement, soldiers, found,
                                  SettlementScope::ring) == 2);
  CHECK(found[0] < found[1]);
}

TEST(sim_the_three_settlement_scopes_are_three_different_answers) {
  TinyGraph tiny;
  World world;
  world.set_class_graph(&tiny.graph);
  const World::SettlementIds town = world.spawn_settlement(1);

  const ObjectId tower = world.spawn(NativeClass::building, nullptr, tiny.building);
  world.set_position(tower, pt(0, 0));
  world.find(tower)->settlement = town.settlement;
  world.find(tower)->sight = 400;
  world.find(tower)->state.health = 5000;

  // The garrison: linked to the settlement and standing inside the tower.
  // Spawned *after* the outsider so that `both`'s sort is observable -- an
  // implementation that sorted only the part the ring appended would leave
  // this one first.
  const ObjectId outsider = world.spawn(NativeClass::unit, nullptr, tiny.soldier);
  const ObjectId garrison = world.spawn(NativeClass::unit, nullptr, tiny.soldier);
  world.set_position(outsider, pt(100, 0));
  world.set_position(garrison, pt(50, 0));
  world.find(outsider)->state.health = 10;
  world.find(garrison)->state.health = 10;
  world.find(garrison)->settlement = town.settlement;

  const ClassFilter soldiers = ClassFilter::of(tiny.soldier);
  std::vector<ObjectId> found;
  CHECK(world.units_in_settlement(town.settlement, soldiers, found,
                                  SettlementScope::garrison) == 1);
  CHECK(found[0] == garrison);
  CHECK(world.units_in_settlement(town.settlement, soldiers, found,
                                  SettlementScope::ring) == 2);
  CHECK(world.units_in_settlement(town.settlement, soldiers, found,
                                  SettlementScope::both) == 2);
  // `both` is a union, not a concatenation: the garrison entry the ring also
  // found collapses to one, and the whole list comes out ascending because
  // 0x005c5ac3 sorts from the list's `begin`, not from where the ring started.
  CHECK(found[0] == outsider && found[1] == garrison);
  CHECK(found[0] < found[1]);

  // A garrisoned unit -- one actually inside a holder -- is *not* in the ring.
  // It holds `(-1, -1)` and occupies no grid cell in the original, and
  // resolving it to its holder's position here would make the ring a superset
  // of the garrison and collapse the distinction the mode encodes.
  const ObjectId inside = world.spawn(NativeClass::unit, nullptr, tiny.soldier);
  world.set_position(inside, pt(10, 0));
  world.find(inside)->state.health = 10;
  REQUIRE(world.put_in_holder(inside, town.holder));
  CHECK(world.units_in_settlement(town.settlement, soldiers, found,
                                  SettlementScope::ring) == 2);
  for (ObjectId id : found) CHECK(id != inside);
}

/// The scope is interned state in the original -- 0x004fe0fb compares it before
/// handing back an existing query -- so two worlds whose queries differ only in
/// it are two different worlds, and the hash has to say so.
TEST(sim_the_settlement_scope_is_hashed_and_survives_a_save) {
  const auto world_with = [](SettlementScope scope) {
    World world;
    const World::SettlementIds town = world.spawn_settlement(1);
    world.create_query(units_in_settlement(town.settlement, ClassFilter{}, scope));
    return world.state_hash();
  };
  CHECK(world_with(SettlementScope::garrison) != world_with(SettlementScope::ring));
  CHECK(world_with(SettlementScope::ring) != world_with(SettlementScope::both));

  World world;
  const World::SettlementIds town = world.spawn_settlement(1);
  const ObjectId query =
      world.create_query(units_in_settlement(town.settlement, ClassFilter{},
                                             SettlementScope::both));
  std::vector<std::byte> bytes;
  world.serialize(bytes);
  World loaded;
  REQUIRE(loaded.deserialize(bytes).ok());
  const QuerySpec* spec = loaded.query_spec(query);
  REQUIRE(spec != nullptr);
  CHECK(spec->settlement_scope == SettlementScope::both);
  CHECK(loaded.state_hash() == world.state_hash());
}

TEST(sim_group_queries_read_the_worlds_group_table) {
  // The eighth query kind. `test_group.cpp` covers the table itself and the
  // `map.obj.xml` that seeds it; this is the seam between the two -- that
  // `QuerySpec::group` is a `GroupTable` index and nothing else.
  World world;
  const ObjectId first = world.spawn(NativeClass::unit, nullptr);
  const ObjectId second = world.spawn(NativeClass::unit, nullptr);
  world.spawn(NativeClass::unit, nullptr);  // in no group

  const std::int32_t attackers = world.group_index("Attackers");
  world.groups().add(attackers, second);
  world.groups().add(attackers, first);

  const ObjectId q = world.create_query(group_query(attackers));
  std::vector<ObjectId> found;
  REQUIRE(world.evaluate_query(q, found) == 2);
  // First-add order, not ascending: a group query is this engine's one
  // exception to `sim/query.hpp`'s ascending rule, because 32 shipped
  // `Group(...).SetCommand` sites and 25 `.GetObjList` sites apply an
  // operation per member in evaluation order and that order reaches the
  // command queue. See `GroupTable` in `sim/world.hpp`.
  CHECK(found[0] == second);
  CHECK(found[1] == first);

  // An index no table holds evaluates empty rather than reading past the end.
  const ObjectId bogus = world.create_query(group_query(99));
  CHECK(world.evaluate_query(bogus, found) == 0);
}

TEST(sim_internal_objects_are_not_query_candidates) {
  World world;
  const World::SettlementIds town = world.spawn_settlement(1);
  world.set_position(town.settlement, pt(0, 0));
  const ObjectId q = world.create_query(objs_in_circle(pt(0, 0), 1000, ClassFilter{}));
  std::vector<ObjectId> found;
  // A settlement, a holder, a warehouse and the query itself all sit at the
  // origin's default position; none of them is a thing in the world.
  CHECK(world.evaluate_query(q, found) == 0);

  // The rectangle sweep has the same reject and needs its own case: it is a
  // separate loop, and the box below straddles the origin every internal object
  // defaults to, so a sweep that forgot the reject would return all four.
  //
  // The authority for the reject is the shared `collect`, not the loop guard in
  // each sweep -- those are a fast path that saves a `resolve_position` per
  // internal object, and deleting one changes nothing observable. Said here so
  // that nobody deletes the reject in `collect` on the grounds that the loops
  // already do it.
  const ObjectId box = world.create_query(objs_in_rect(-1000, -1000, 1000, 1000, ClassFilter{}));
  CHECK(world.evaluate_query(box, found) == 0);
  CHECK(world.objects_in_rect(-1000, -1000, 1000, 1000, ClassFilter{}, found) == 0);
  // The precondition: there really are internal objects standing in that box.
  std::size_t internals = 0;
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none) ++internals;
  }
  CHECK(internals >= 4);
}

// ==========================================================================
// the system registry
// ==========================================================================

TEST(sim_systems_run_in_registration_order) {
  std::vector<std::string> log;
  RecordingSystem first("movement", &log, 1);
  RecordingSystem second("combat", &log, 2);
  RecordingSystem third("economy", &log, 3);

  World world;
  CHECK(world.add_system(&first));
  CHECK(world.add_system(&second));
  CHECK(world.add_system(&third));
  // Registering twice would run it twice and fold its hash twice.
  CHECK(!world.add_system(&second));
  CHECK(!world.add_system(nullptr));
  CHECK(world.systems().size() == 3);

  world.start();
  world.advance(400);
  world.advance(400);

  const std::vector<std::string> expected = {
      "start:movement",   "start:combat",     "start:economy",
      "advance:movement", "advance:combat",   "advance:economy",
      "advance:movement", "advance:combat",   "advance:economy",
  };
  CHECK(log == expected);
  CHECK(first.turns_ == 2 && third.turns_ == 2);
}

TEST(sim_a_system_sees_the_turn_it_is_running_in) {
  struct TurnWatcher : System {
    std::string_view name() const noexcept override { return "watcher"; }
    void advance(World& world, const Turn& turn) override {
      lengths.push_back(turn.length);
      turns.push_back(world.turns());
    }
    std::vector<std::int32_t> lengths;
    std::vector<std::uint64_t> turns;
  } watcher;

  World world;
  world.add_system(&watcher);
  // The turn length is negotiated per turn, never constant: 800, 400, 799 and
  // 200 all occur in the nine dumps.
  const std::int32_t lengths[] = {400, 800, 200};
  world.advance(std::span<const std::int32_t>(lengths, 3));
  CHECK(watcher.lengths.size() == 3);
  CHECK(watcher.lengths[0] == 400 && watcher.lengths[1] == 800 && watcher.lengths[2] == 200);
  CHECK(watcher.turns[0] == 1 && watcher.turns[2] == 3);
  CHECK(world.time() == 1400);
}

// ==========================================================================
// the hash
// ==========================================================================

namespace {

/// Two worlds built by the same steps, so that a hash difference is the thing
/// under test rather than an artefact of construction.
void build(World& world) {
  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = world.spawn(NativeClass::building, nullptr);
  world.set_position(a, pt(100, 200));
  world.set_position(b, pt(300, 400));
  world.set_owner(a, 1);
  world.set_owner(b, 2);
  world.set_health(a, 50);
  world.set_health(b, 5000);
}

}  // namespace

TEST(sim_hash_ignores_the_order_of_independent_mutations) {
  World left;
  World right;
  build(left);
  build(right);
  CHECK(left.state_hash() == right.state_hash());

  // Touching two different objects in either order is the same world.
  left.set_health(1, 40);
  left.set_health(2, 4000);
  right.set_health(2, 4000);
  right.set_health(1, 40);
  CHECK(left.state_hash() == right.state_hash());

  // And a value written twice is the value, not the writing.
  left.set_stamina(1, 3);
  left.set_stamina(1, 7);
  right.set_stamina(1, 7);
  CHECK(left.state_hash() == right.state_hash());
}

TEST(sim_hash_changes_when_the_state_changes) {
  World world;
  build(world);
  const std::uint64_t base = world.state_hash();

  const auto differs = [&](auto&& mutate) {
    World w;
    build(w);
    mutate(w);
    return w.state_hash() != base;
  };

  CHECK(differs([](World& w) { w.set_position(1, pt(101, 200)); }));
  CHECK(differs([](World& w) { w.set_owner(1, 3); }));
  CHECK(differs([](World& w) { w.set_health(1, 49); }));
  CHECK(differs([](World& w) { w.set_stamina(1, 1); }));
  CHECK(differs([](World& w) { w.spawn(NativeClass::decor, nullptr); }));
  CHECK(differs([](World& w) { w.despawn(1); }));
  CHECK(differs([](World& w) { w.advance(400); }));
  CHECK(differs([](World& w) { w.rng().next(); }));
  CHECK(differs([](World& w) { w.next_command_id(); }));
  // The category bits are cached on the object and are hashed with it.
  CHECK(differs([](World& w) { w.mutable_state(1)->flags.has_active_path = true; }));
  // A holder link is state even though the position it implies is not stored.
  CHECK(differs([](World& w) {
    const ObjectId h = w.spawn_internal(InternalKind::holder);
    w.put_in_holder(1, h);
  }));
}

TEST(sim_hash_sees_the_query_table) {
  World a;
  World b;
  build(a);
  build(b);
  a.create_query(objs_in_circle(pt(0, 0), 100, ClassFilter{}));
  b.create_query(objs_in_circle(pt(0, 0), 101, ClassFilter{}));
  // Two worlds whose queries ask different questions are different worlds even
  // before either query is evaluated.
  CHECK(a.state_hash() != b.state_hash());

  // The rectangle is hashed too, and each corner separately: a spec that
  // carried the same four numbers in a different arrangement asks a different
  // question, and folding only, say, the width would make two of them equal.
  World rect_a;
  World rect_b;
  build(rect_a);
  build(rect_b);
  rect_a.create_query(objs_in_rect(0, 0, 100, 200, ClassFilter{}));
  rect_b.create_query(objs_in_rect(0, 0, 200, 100, ClassFilter{}));
  CHECK(rect_a.state_hash() != rect_b.state_hash());

  // And a rectangle is not its bounding circle, nor anything else: the two
  // kinds are distinct even when every other field agrees.
  World kind_a;
  World kind_b;
  build(kind_a);
  build(kind_b);
  kind_a.create_query(objs_in_rect(0, 0, 100, 100, ClassFilter{}));
  QuerySpec circled = objs_in_circle(pt(0, 0), 0, ClassFilter{});
  circled.right = 100;
  circled.bottom = 100;
  kind_b.create_query(circled);
  CHECK(kind_a.state_hash() != kind_b.state_hash());

  // And the shape discriminator on `class_player_area`, which is one bool
  // deciding whether the corners or the circle bound it. Two specs identical
  // but for that bool ask different questions.
  World shape_a;
  World shape_b;
  build(shape_a);
  build(shape_b);
  shape_a.create_query(class_player_area_rect(ClassFilter{}, 1, 0, 0, 100, 100));
  QuerySpec as_circle = class_player_area_rect(ClassFilter{}, 1, 0, 0, 100, 100);
  as_circle.area_is_rect = false;
  shape_b.create_query(as_circle);
  CHECK(shape_a.state_hash() != shape_b.state_hash());

  World c;
  World d;
  build(c);
  build(d);
  // And allocating a query at all shifts the world, because it took a handle.
  c.create_query(group_query(1));
  CHECK(c.state_hash() != d.state_hash());
  CHECK(c.next_id() == d.next_id() + 1);
}

TEST(sim_hash_sees_system_registration_order) {
  std::vector<std::string> log;
  RecordingSystem alpha("alpha", &log, 0x1111);
  RecordingSystem beta("beta", &log, 0x2222);

  World forward;
  forward.add_system(&alpha);
  forward.add_system(&beta);

  RecordingSystem alpha2("alpha", &log, 0x1111);
  RecordingSystem beta2("beta", &log, 0x2222);
  World reversed;
  reversed.add_system(&beta2);
  reversed.add_system(&alpha2);

  forward.advance(400);
  reversed.advance(400);
  // The fold is not commutative on purpose. Run order is part of the
  // simulation's definition, so a silently reordered pipeline must show up as a
  // hash difference rather than as a subtle behavioural one.
  CHECK(forward.state_hash() != reversed.state_hash());
}

TEST(sim_hashes_block_keeps_the_dead_channels_dead) {
  World world;
  build(world);
  const WorldHashes h = world.hashes();
  CHECK(h.slots == world.state_hash());
  CHECK(h.slots != 0);
  // pathfinder, exploration, scriptstate and aihash are zero in all nine dumps:
  // the shipped build kept those subsystems out of the determinism contract,
  // and a non-zero value here would mean one had been pulled back in.
  CHECK(h.pathfinder == 0 && h.exploration == 0 && h.scriptstate == 0 && h.aihash == 0);
  CHECK(h.hash_of_hashes != 0);
}

// ==========================================================================
// the RNG
// ==========================================================================

TEST(sim_rng_is_reproducible_from_its_seed) {
  Rng a(12345);
  Rng b(12345);
  for (int i = 0; i < 100; ++i) CHECK(a.next() == b.next());

  // The state is the whole serialised surface: the dump's `syncseed`.
  Rng c(999);
  for (int i = 0; i < 10; ++i) c.next();
  Rng d(c.state());
  for (int i = 0; i < 10; ++i) CHECK(c.next() == d.next());
}

TEST(sim_rand_is_exclusive_of_its_bound) {
  // `rand(ol.count)` indexes an ObjList, `pt.Rot(rand(360))` covers a circle,
  // and `if (rand(100) < 25)` is exactly 25% -- all three require [0, n).
  Rng rng(7);
  bool saw_zero = false;
  bool saw_top = false;
  for (int i = 0; i < 20000; ++i) {
    const std::int32_t v = rng.below(10);
    CHECK(v >= 0 && v < 10);
    if (v == 0) saw_zero = true;
    if (v == 9) saw_top = true;
  }
  CHECK(saw_zero && saw_top);

  Rng coin(3);
  int heads = 0;
  for (int i = 0; i < 10000; ++i) heads += coin.below(2);
  // `if (rand(2))` is a coin flip in the corpus. A wide band: this is a
  // sanity check on the low bits, not a statistical test.
  CHECK(heads > 4000 && heads < 6000);
}

TEST(sim_rand_draws_even_when_the_bound_is_degenerate) {
  // Whether a call advances the stream is itself synchronised state, so a bound
  // of 1 or 0 must still move it -- otherwise the peers' stream positions
  // depend on a value one of them computed differently.
  Rng a(42);
  Rng b(42);
  CHECK(a.below(1) == 0);
  CHECK(a.below(0) == 0);
  b.next();
  b.next();
  CHECK(a.state() == b.state());
}

TEST(sim_rng_between_is_inclusive_at_both_ends) {
  Rng rng(11);
  bool low = false;
  bool high = false;
  for (int i = 0; i < 5000; ++i) {
    const std::int32_t v = rng.between(-3, 3);
    CHECK(v >= -3 && v <= 3);
    if (v == -3) low = true;
    if (v == 3) high = true;
  }
  CHECK(low && high);
}

TEST(sim_the_world_owns_exactly_one_generator) {
  World world;
  world.seed(0xABCDEF01u);
  const std::uint32_t first = world.rng().next();
  world.seed(0xABCDEF01u);
  CHECK(world.rng().next() == first);

  // The command id allocator is a separate, monotone counter -- the dump's
  // `cmdidseed` -- and not a generator.
  world.set_command_id_seed(0x168);
  CHECK(world.next_command_id() == 0x168);
  CHECK(world.next_command_id() == 0x169);
  CHECK(world.command_id_seed() == 0x16A);
}

// ==========================================================================
// host bindings
// ==========================================================================

TEST(sim_points_pack_and_unpack_losslessly) {
  const Point cases[] = {{0, 0}, {1, 2}, {16383, 16140}, {-1, -1}, {-32768, 32767}};
  for (Point p : cases) {
    const auto value = pack_point(p);
    CHECK(is_point(value));
    CHECK(unpack_point(value) == p);
  }
  // The held marker survives the round trip, which is the case that matters.
  CHECK(unpack_point(pack_point(kHeldPosition)) == kHeldPosition);
}

TEST(sim_isqrt_is_exact_and_floors) {
  CHECK(isqrt(0) == 0);
  CHECK(isqrt(1) == 1);
  CHECK(isqrt(-5) == 0);
  for (std::int64_t n = 1; n < 2000; ++n) {
    const std::int64_t r = isqrt(n);
    CHECK(r * r <= n);
    CHECK((r + 1) * (r + 1) > n);
  }
  // A full-map diagonal, squared, is well inside the range this must handle.
  CHECK(isqrt(16383LL * 16383LL) == 16383);
}

TEST(sim_host_registration_covers_the_object_model) {
  imperivm::core::script::HostRegistry registry;
  imperivm::core::script::declare_shipped_surface(registry);
  const std::size_t before = registry.implemented();
  const std::size_t defined = register_world_host(registry);
  CHECK(defined >= 40);
  CHECK(registry.implemented() >= before + 30);

  // The entries have to land on the *declared* surface, not beside it: a name
  // or arity that does not match a call site would compile and never run.
  using imperivm::core::script::CallKind;
  const auto found = [&](CallKind kind, const char* name, std::uint16_t arity) {
    const std::uint32_t index = registry.find(kind, name, arity);
    return index != imperivm::core::script::kUnresolvedHost &&
           registry.entry(index).fn != nullptr;
  };
  CHECK(found(CallKind::free_function, "rand", 1));
  CHECK(found(CallKind::free_function, "ObjsInSight", 2));
  CHECK(found(CallKind::free_function, "Intersect", 2));
  CHECK(found(CallKind::free_function, "Union", 2));
  CHECK(found(CallKind::free_function, "Subtract", 2));
  CHECK(found(CallKind::free_function, "Substract", 2));
  CHECK(found(CallKind::member, "IsValid", 0));
  CHECK(found(CallKind::member, "pos", 0));
  CHECK(found(CallKind::member, "player", 0));
  CHECK(found(CallKind::member, "health", 0));
  CHECK(found(CallKind::member, "AsUnit", 0));
}

TEST(sim_host_point_arithmetic_is_integral) {
  World world;
  WorldHost host(world);
  using imperivm::core::script::BinaryOp;

  const auto a = pack_point(pt(100, 200));
  const auto b = pack_point(pt(10, 20));
  const auto sum = host.binary(BinaryOp::add, a, b);
  REQUIRE(sum.ok());
  CHECK(unpack_point(sum.value()) == pt(110, 220));

  const auto diff = host.binary(BinaryOp::sub, a, b);
  REQUIRE(diff.ok());
  CHECK(unpack_point(diff.value()) == pt(90, 180));

  const auto scaled =
      host.binary(BinaryOp::mul, a, imperivm::core::script::Value::integer(3));
  REQUIRE(scaled.ok());
  CHECK(unpack_point(scaled.value()) == pt(300, 600));

  // Integer division, truncating -- there is no other definition available to
  // be inconsistent with, and no floating point anywhere near it.
  const auto halved =
      host.binary(BinaryOp::div, a, imperivm::core::script::Value::integer(3));
  REQUIRE(halved.ok());
  CHECK(unpack_point(halved.value()) == pt(33, 66));

  // Division by zero refuses rather than trapping the process.
  CHECK(!host.binary(BinaryOp::div, a, imperivm::core::script::Value::integer(0)).ok());

  CHECK(host.default_value("point").kind() == imperivm::core::script::ValueKind::object);
  CHECK(unpack_point(host.default_value("point")) == pt(0, 0));
  CHECK(!host.default_value("Unit").as_object().valid());
}

/// **A point is two 32-bit integers, and an intermediate may leave the map.**
///
/// `gbr.exe`'s point operators pop eight bytes per point and multiply with a
/// 32-bit `imul` (`0x00696f00`). `SENTRY_PATROL.VS` walks between
/// `(route1*4 + route0)/5` and `(route0*4 + route1)/5`; for a wall at
/// x = 14,480 -- p1's town on Crossroads -- `route1*4` is 57,920, and a point
/// of sixteen-bit halves wrapped it to -7,616, sending the sentry to x = 1,372,
/// across the map.
TEST(sim_host_point_arithmetic_keeps_thirty_two_bits) {
  World world;
  WorldHost host(world);
  using imperivm::core::script::BinaryOp;
  using imperivm::core::script::Value;

  const auto route0 = pack_point(pt(14480, 2242));
  const auto route1 = pack_point(pt(14482, 2322));
  const auto four = host.binary(BinaryOp::mul, route1, Value::integer(4));
  REQUIRE(four.ok());
  CHECK(unpack_point(four.value()) == pt(57928, 9288));
  const auto sum = host.binary(BinaryOp::add, four.value(), route0);
  REQUIRE(sum.ok());
  CHECK(unpack_point(sum.value()) == pt(72408, 11530));
  const auto fifth = host.binary(BinaryOp::div, sum.value(), Value::integer(5));
  REQUIRE(fifth.ok());
  CHECK(unpack_point(fifth.value()) == pt(14481, 2306));
  // `int * point` and a difference below -32,768 keep their width too.
  const auto left = host.binary(BinaryOp::mul, Value::integer(3), route0);
  REQUIRE(left.ok());
  CHECK(unpack_point(left.value()) == pt(43440, 6726));
  const auto below = host.binary(BinaryOp::sub, pack_point(pt(0, 0)), four.value());
  REQUIRE(below.ok());
  CHECK(unpack_point(below.value()) == pt(-57928, -9288));
  // Two points equal only when both words are: y is not dropped.
  CHECK(!(pack_point(pt(5, 1)) == pack_point(pt(5, 2))));
  CHECK(pack_point(pt(-70000, 70000)) == pack_point(pt(-70000, 70000)));
  CHECK(unpack_point(pack_point(pt(-70000, 70000))) == pt(-70000, 70000));
}

TEST(sim_a_self_referential_query_terminates) {
  World world;
  const ObjectId a = world.create_query(group_query(1));
  const ObjectId loop = world.create_query(set_op(SetOp::set_union, a, a));
  // Make it name itself. Nothing in the language stops a script from doing
  // this, and an unbounded recursion would be a hang rather than a desync --
  // which is worse, because a hang cannot be diffed against a dump.
  world.mutable_query_spec(loop)->lhs = loop;
  std::vector<ObjectId> found;
  CHECK(world.evaluate_query(loop, found) == 0);
}

TEST(sim_a_player_flags_query_selects_by_relation_not_by_owner) {
  // This selected the *viewer's own* objects for a while, which made
  // `EnemyObjs(p, cls)` return exactly the wrong set while still composing,
  // hashing, and passing every test that checked only its shape.
  World world;
  const ObjectId mine = world.spawn(NativeClass::unit, nullptr);
  const ObjectId theirs = world.spawn(NativeClass::unit, nullptr);
  const ObjectId neutral = world.spawn(NativeClass::unit, nullptr);
  world.set_owner(mine, 0);
  world.set_owner(theirs, 1);
  world.set_owner(neutral, 2);

  // Player 0 has a ceasefire with 2 and nothing with 1. Hostility is
  // one-directional, so only player 0's row matters here.
  world.players().set(0, 2, Relation::ceasefire, true);

  std::vector<ObjectId> found;

  const ObjectId enemies = world.create_query(player_flags(0, ClassFilter{}, 2));
  world.evaluate_query(enemies, found);
  REQUIRE(found.size() == 1);
  CHECK(found[0] == theirs);

  const ObjectId friends = world.create_query(player_flags(0, ClassFilter{}, 1));
  world.evaluate_query(friends, found);
  // Player 0's own object and the one it has a truce with; not the enemy.
  REQUIRE(found.size() == 2);
  CHECK(found[0] == mine);
  CHECK(found[1] == neutral);

  // `ControllableObjs`: own objects only, until somebody grants share-control.
  const ObjectId controllable = world.create_query(player_flags(0, ClassFilter{}, 3));
  world.evaluate_query(controllable, found);
  REQUIRE(found.size() == 1);
  CHECK(found[0] == mine);

  world.players().set(1, 0, Relation::share_control, true);
  world.evaluate_query(controllable, found);
  REQUIRE(found.size() == 2);
  CHECK(found[1] == theirs);

  // An unrecognised type selects nothing. A query that silently widened would
  // be the harder failure to notice.
  const ObjectId nonsense = world.create_query(player_flags(0, ClassFilter{}, 9));
  CHECK(world.evaluate_query(nonsense, found) == 0);
}

// --------------------------------------------------------------------------
// clicking on a building
// --------------------------------------------------------------------------
//
// The four predicates below (`IsIndependentGuarded`, `IsTTent`,
// `IsValidCaptureTarget`, `IsCentralBuliding`) exist for exactly one reason:
// every `<defaultcmd target="Building">` list begins with `attack_independent`,
// whose `verify=` script needs the first two, and the two candidates behind it
// need the other two. A trap anywhere in that chain is an `unknown` verdict,
// and `resolve_default_order` blocks on `unknown` rather than falling through
// -- so until all four answer, a right click on any building resolves to
// nothing at all.
//
// This section therefore does not test the predicates. It resolves a click,
// which is the only thing that proves the path the data takes is joined end to
// end: shipped `<defaultcmd>` order, shipped `<commands>` rows, the shipped
// `verify=` sources compiled and run, and the host entry points they call.
//
// The four verifier scripts are written for this test and reach every entry
// point the shipped ones reach; see the note above them for why they are not
// copies of the shipped files and what is asserted instead.

namespace {

namespace script = imperivm::core::script;

using imperivm::core::script::CompileError;
using imperivm::core::script::Diagnostic;
using imperivm::core::script::HostRegistry;
using imperivm::core::script::Scheduler;

/// `COMMANDS\*.XML` rows for the five verbs a unit offers against a building,
/// plus the ground `move` that proves the actor is otherwise ordinary.
constexpr std::string_view kClickCommandsXml = R"(<commands>
<cmd name="move" priority="0" offset="1" key="m"/>
<cmd name="attack" priority="1" offset="1" key="a" cursor="attack"/>
<cmd name="approach" priority="1"/>
<cmd name="attack_independent" priority="1" cursor="do_something"/>
<cmd name="capture" priority="2" offset="1" key="u" cursor="do_something"/>
<cmd name="enter" priority="4" cursor="do_something"/>
</commands>)";

// The four verifiers below are **written for this test**, not copied.
//
// `docs/legal.md` rule 1 forbids the alternative -- no game assets in the
// repository, "not as test fixtures" -- and `tools/check_fixtures.py` refuses
// it now. They used to be transcriptions of `ATTACK_INDEPENDENT_VERIFY.VS`,
// `UNIT_CAPTURE_VERIFY.VS`, `UNIT_ATTACK_VERIFY.VS` and `UNIT_ENTER_VERIFY.VS`,
// comments and commented-out lines included.
//
// **What the copies were for is preserved by construction.** The claim was
// never "these particular bytes"; it was *"a right click resolves through a
// compiled verifier, and the entry points the shipped ones reach are all
// there"*. So each one below asks the same questions of the same receivers as
// the file it stands in for -- `AsUnit`, `AsBuilding`, `AsHero`, `IsValid`,
// `IsEnemy`, `IsHeirOf`, `IsValidTarget`, `IsValidCaptureTarget`,
// `settlement.IsOutpost`, `settlement.IsIndependentGuarded`,
// `settlement.IsTTent`, `settlement.max_units`, `IsCentralBuliding` (the
// misspelling is the engine's entry-point name and has to be) and
// `DiplGetShareView` -- so a missing or mis-aritied one still fails to compile
// here, and the priority order still has to come out of the data.
//
// That the *shipped* verifiers compile is asserted over all 885 scripts by the
// corpus tier, which reads them out of the installation.

/// Stands in for `ATTACK_INDEPENDENT_VERIFY.VS`: an outpost only an
/// independent garrison holds, or a tent that is hostile.
constexpr std::string_view kAttackIndependentVerify = R"(//bool, Obj this, Obj other
Unit actor;
Building target;

actor = this.AsUnit();
target = other.AsBuilding();
if (!actor.IsValid()) return false;
if (!target.IsValid()) return false;

if (target.settlement.IsOutpost())
	if (target.settlement.IsIndependentGuarded())
		return true;
if (target.settlement.IsTTent())
	return actor.IsEnemy(target);
return false;
)";

/// Stands in for `UNIT_CAPTURE_VERIFY.VS`: a capturable building held by an
/// enemy, or a wagon, which is not a building at all.
constexpr std::string_view kCaptureVerify = R"(//bool, Obj this, Obj other
Unit actor;
Building target;

actor = this.AsUnit();
target = other.AsBuilding();
if (!actor.IsValid()) return false;

if (target.IsValid()) {
	if (actor.IsValidCaptureTarget(target))
		return actor.IsEnemy(target);
	return false;
}
if (other.IsHeirOf("Wagon"))
	return actor.IsEnemy(other);
return false;
)";

/// Stands in for `UNIT_ATTACK_VERIFY.VS`: anything hostile that is a legal
/// target, unit or building, in that order.
constexpr std::string_view kAttackVerify = R"(//bool, Obj this, Obj other
Unit actor;
Unit victim;
Building target;

actor = this.AsUnit();
if (!actor.IsValid()) return false;

victim = other.AsUnit();
if (victim.IsValid())
	return actor.IsEnemy(victim) && actor.IsValidTarget(victim);

target = other.AsBuilding();
if (target.IsValid())
	return actor.IsEnemy(target) && actor.IsValidTarget(target);
return false;
)";

/// Stands in for `UNIT_ENTER_VERIFY.VS`, which is the fussiest of the four:
/// ruins admit only a hero, a catapult admits only a non-hero soldier, a
/// building with no room admits nobody, and the two players have to be sharing
/// sight of each other.
constexpr std::string_view kEnterVerify = R"(// bool, Obj this, Obj other
Unit actor;
Building door;

actor = this.AsUnit();
door = other.AsBuilding();

if (other.IsHeirOf("BaseRuins"))
	return .AsHero().IsValid;

if (!actor.IsValid()) return false;
if (!door.IsValid()) return false;
if (!door.IsCentralBuliding()) return false;

if (door.IsHeirOf("Catapult")) {
	if (!actor.IsHeirOf("Military")) return false;
	if (actor.IsHeirOf("Hero")) return false;
}

if (door.settlement.max_units == 0) return false;
if (!DiplGetShareView(door.player, actor.player)) return false;
return !actor.IsEnemy(door);
)";

/// Everything a right click needs: a class tree with the shipped
/// `<defaultcmd>` block on it, the command table, the compiled verifiers, and
/// an economy holding the settlements the verifiers ask about.
struct ClickFixture {
  ClassGraph graph;
  World world;
  EconomySystem economy;
  CommandTable table;
  HostRegistry registry;
  Scheduler scheduler;
  HostContext context;

  ClassIndex military = kNoClass;
  ClassIndex outpost_class = kNoClass;
  ClassIndex plain_outpost_class = kNoClass;
  ClassIndex townhall_class = kNoClass;
  ClassIndex tower_class = kNoClass;
  ClassIndex ship_battle_class = kNoClass;
  ClassIndex ship_rome_class = kNoClass;
  bool built = true;

  ClickFixture() {
    // Class ids, parents, `cpp_class` and the properties under test are the
    // shipped ones; the art, sounds and scripts every real class also carries
    // are not, because nothing here reads them.
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor" parent="">
             <properties is_central_building="0"/>
           </class>)",
        R"(<class id="Unit" cpp_class="CVXUnit" parent="Object">
             <method sig="approach" vs="data/subai/unit_approach.vs"/>
             <method sig="attack"  vs="data/subai/unit_attack.vs" verify="data/subai/unit_attack_verify.vs"/>
             <method sig="enter"   vs="data/subai/unit_enter.vs" verify="data/subai/unit_enter_verify.vs"/>
             <method sig="capture" vs="data/subai/unit_capture.vs" verify="data/subai/unit_capture_verify.vs"/>
             <method sig="attack_independent" vs="data/subai/unit_capture.vs" verify="data/subai/attack_independent_verify.vs"/>
             <defaultcmd target="Building">
               <cmd name="attack_independent"/>
               <cmd name="capture"/>
               <cmd name="attack"/>
               <cmd name="enter"/>
               <cmd name="approach"/>
             </defaultcmd>
           </class>)",
        R"(<class id="Military" cpp_class="CVXUnit" parent="Unit"/>)",
        R"(<class id="Building" cpp_class="CVXBuilding" parent="Object"/>)",
        R"(<class id="Tower" cpp_class="CVXBuilding" parent="Building"/>)",
        // `OUTPOST.SC.XML`: central, capturable, and *no* `defender_cls_*`.
        R"(<class id="Outpost" cpp_class="CVXTownHall" parent="Building">
             <properties is_central_building="1" can_be_captured="1" max_units="10000"/>
           </class>)",
        // `ROUTPOST.SC.XML`: the guard roster is the whole difference.
        R"(<class id="ROutpost" cpp_class="CVXTownHall" parent="Outpost">
             <properties defender_cls_1="RLiberatus" defenders_max_1="20"/>
           </class>)",
        R"(<class id="BaseTownhall" cpp_class="CVXTownHall" parent="Building">
             <properties is_central_building="1" can_be_captured="1" max_units="10000"/>
           </class>)",
        R"(<class id="Ranged" cpp_class="CVXUnit" parent="Military"/>)",
        // `SHIP BATTLE.SC.XML`: the *only* class in `data.pak` that declares
        // `max_units_to_board`, and the parent of the other three `CVXShip`
        // classes, which declare nothing and inherit the 60.
        R"(<class id="ShipBattle" cpp_class="CVXShip" parent="Ranged">
             <properties max_units_to_board="60"/>
           </class>)",
        R"(<class id="ShipRome" cpp_class="CVXShip" parent="ShipBattle"/>)",
    };
    const char* names[] = {"object.sc.xml",     "unit.sc.xml",        "military.sc.xml",
                           "building.sc.xml",   "tower.sc.xml",       "outpost.sc.xml",
                           "routpost.sc.xml",   "basetownhall.sc.xml", "ranged.sc.xml",
                           "ship battle.sc.xml", "shiprome.sc.xml"};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
      if (!graph.add(bytes_of(docs[i]), names[i]).ok()) built = false;
    }
    graph.link();
    world.set_class_graph(&graph);
    military = graph.find("Military");
    outpost_class = graph.find("ROutpost");
    plain_outpost_class = graph.find("Outpost");
    townhall_class = graph.find("BaseTownhall");
    tower_class = graph.find("Tower");
    ship_battle_class = graph.find("ShipBattle");
    ship_rome_class = graph.find("ShipRome");

    if (!table.merge(bytes_of(kClickCommandsXml)).ok()) built = false;
    world.add_system(&economy);

    // Player 1 is the human, player 2 an enemy, player 14 the independents.
    // Every player controls and sees for itself; nobody is allied.
    for (PlayerId p = 0; p < 16; ++p) {
      world.players().set(p, p, Relation::share_control, true);
      world.players().set(p, p, Relation::share_view, true);
    }

    register_all_hosts(registry);
    scheduler.set_registry(&registry);
    context.world = &world;
    scheduler.set_user(&context);
    add(kAttackIndependentVerify, "data/subai/attack_independent_verify.vs");
    add(kCaptureVerify, "data/subai/unit_capture_verify.vs");
    add(kAttackVerify, "data/subai/unit_attack_verify.vs");
    add(kEnterVerify, "data/subai/unit_enter_verify.vs");
    economy.start(world);
  }

  void add(std::string_view source, const char* name) {
    Diagnostic diagnostic;
    const auto parsed = imperivm::core::script::parse(bytes_of(source), name, &diagnostic);
    if (!parsed.ok()) {
      std::printf("  parse %s:%u: %.*s\n", name, diagnostic.line,
                  static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
      built = false;
      return;
    }
    CompileError error;
    auto chunk = imperivm::core::script::compile(parsed.value(), &registry, &error);
    if (!chunk.ok()) {
      std::printf("  compile %s:%u: %s\n", name, error.line, error.message.c_str());
      built = false;
      return;
    }
    scheduler.add_chunk(std::move(chunk.value()));
  }

  ObjectId soldier(PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, military);
    world.set_position(id, Point{100, 100});
    world.set_owner(id, owner);
    world.set_health(id, 100);
    return id;
  }

  /// A building that is the centre of a settlement of `kind`, owned by
  /// `owner`, and linked to it the way the map loader links one.
  ObjectId centre(ClassIndex cls, SettlementKind kind, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::town_hall, nullptr, cls);
    world.set_position(id, Point{400, 400});
    world.set_owner(id, owner);
    world.set_health(id, 5000);
    SettlementInit init;
    init.anchor = id;
    init.owner = owner;
    init.kind = kind;
    init.can_be_captured = true;
    init.can_be_attacked = true;
    init.max_units = 10000;
    init.anchor_max_health = 5000;
    economy.create(world, init);
    return id;
  }

  std::string click(ObjectId actor, ObjectId target) {
    ScriptOrderVerifier verifier(scheduler, context);
    const DefaultOrder order = resolve_default_order(
        world, table, actor, OrderTarget{Point{400, 400}, target}, false, &verifier);
    if (order.status == DefaultOrderStatus::blocked) return std::string("blocked:") + std::string(order.blocked_by);
    if (order.status == DefaultOrderStatus::none) return "none";
    return std::string(order.command);
  }
};

}  // namespace

TEST(a_right_click_on_a_building_resolves_to_a_verb) {
  ClickFixture f;
  REQUIRE(f.built);
  REQUIRE(f.military != kNoClass);
  REQUIRE(f.outpost_class != kNoClass);

  const ObjectId me = f.soldier(1);

  // 1. An independent guarded outpost -- a Roman fort nobody owns, with a
  //    `defender_cls_1` roster. `attack_independent` leads the list and its
  //    verifier is the only thing that can say yes to it.
  const ObjectId fort = f.centre(f.outpost_class, SettlementKind::outpost, kNeutralWildlife);
  CHECK(f.click(me, fort) == "attack_independent");

  // 2. The same outpost class without the roster: independent, an outpost, and
  //    *not* guarded. `attack_independent` must decline, and the click falls
  //    through to `capture` -- an independent player is nobody's declared
  //    friend, so `UNIT_CAPTURE_VERIFY.VS`'s `me.IsEnemy(bld)` holds. The verb
  //    differing from case 1's is the whole point: the roster is what turns a
  //    capture into an assault.
  const ObjectId quiet =
      f.centre(f.plain_outpost_class, SettlementKind::outpost, kNeutralWildlife);
  CHECK(f.click(me, quiet) == "capture");

  // 3. An enemy town hall: not an outpost, not a tent, so `attack_independent`
  //    declines; `capture` is next and `IsValidCaptureTarget` allows it.
  const ObjectId theirs = f.centre(f.townhall_class, SettlementKind::stronghold, 2);
  CHECK(f.click(me, theirs) == "capture");

  // 4. Our own town hall: nothing to attack and nothing to capture, so the
  //    click means "go inside", which is what `IsCentralBuliding` gates.
  const ObjectId home = f.centre(f.townhall_class, SettlementKind::stronghold, 1);
  CHECK(f.click(me, home) == "enter");

  // None of the four may be `blocked`: that is the verdict a trap produces,
  // and it is what every one of these clicks did before the four predicates
  // were implemented.
  for (const ObjectId target : {fort, quiet, theirs, home}) {
    const std::string verb = f.click(me, target);
    CHECK(verb.compare(0, 8, "blocked:") != 0);
  }
}

TEST(pos_is_the_stored_field_and_pos_rh_differs_only_for_a_held_unit) {
  // `Obj::pos` (0x005add20) copies `[obj+0x24]`/`[obj+0x28]`. `Obj::posRH`
  // (0x005ad8a0) calls `vtbl+0xC8`, which for every non-unit class reaches the
  // same two fields (0x005a75d0 -> 0x0063d900) and for a unit is 0x005d3db0:
  // in a holder that is not a settlement's, the position of the object the
  // holder belongs to -- the ship, for a passenger. So the two agree on
  // everything but a held unit, and there `pos` is the `(-1, -1)` the holder
  // entry wrote. Both answered the holder walk for a while, which a ship's
  // holder record, an internal object of its own, ends at (0, 0).
  World world;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);

  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto member = [&](const char* name, script::Value receiver) -> script::HostOutcome {
    const std::uint32_t index = registry.find(script::CallKind::member, name, 0);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::Value args[] = {receiver};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return entry.fn(ctx);
  };
  const auto obj = [](ObjectId id) { return script::Value::object(kTypeObj, id); };
  const auto answers = [&](script::Value receiver, Point pos, Point pos_rh) {
    const script::HostOutcome plain = member("pos", receiver);
    const script::HostOutcome rh = member("posRH", receiver);
    REQUIRE(plain.status == script::HostStatus::ok);
    REQUIRE(rh.status == script::HostStatus::ok);
    REQUIRE(is_point(plain.value));
    REQUIRE(is_point(rh.value));
    CHECK(unpack_point(plain.value) == pos);
    CHECK(unpack_point(rh.value) == pos_rh);
  };

  const ObjectId free_standing = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(world.set_position(free_standing, Point{700, 900}));
  answers(obj(free_standing), Point{700, 900}, Point{700, 900});

  // A passenger: `pos` is the marker, `posRH` the ship.
  const World::ShipIds ship = world.spawn_ship(nullptr);
  REQUIRE(world.set_position(ship.ship, Point{1200, 1500}));
  const ObjectId passenger = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(world.find(passenger)->state.flags.is_unit);
  REQUIRE(world.put_in_holder(passenger, ship.holder));
  REQUIRE(world.resolve_position(passenger) == (Point{0, 0}));
  answers(obj(passenger), kHeldPosition, Point{1200, 1500});
  CHECK(unit_pos_rh(world, passenger) == (Point{1200, 1500}));

  // A held object that is not a unit has no override: both are the marker.
  const ObjectId crate = world.spawn(NativeClass::decor, nullptr);
  REQUIRE(world.put_in_holder(crate, ship.holder));
  answers(obj(crate), kHeldPosition, kHeldPosition);

  // The invalid receiver, which is the one place the two bodies differ in
  // `gbr.exe` -- `pos` prints a diagnostic first and `posRH` does not -- and
  // the one place they still have to return the same thing.
  answers(obj(kNoObject), kHeldPosition, kHeldPosition);
  answers(obj(static_cast<ObjectId>(9999)), kHeldPosition, kHeldPosition);
  answers(script::Value::integer(3), kHeldPosition, kHeldPosition);
}

TEST(the_building_predicates_answer_what_gbr_exe_answers) {
  ClickFixture f;
  REQUIRE(f.built);

  const ObjectId me = f.soldier(1);
  const ObjectId fort = f.centre(f.outpost_class, SettlementKind::outpost, kNeutralWildlife);
  const ObjectId quiet =
      f.centre(f.plain_outpost_class, SettlementKind::outpost, kNeutralWildlife);
  const ObjectId owned = f.centre(f.outpost_class, SettlementKind::outpost, 2);
  const ObjectId home = f.centre(f.townhall_class, SettlementKind::stronghold, 1);

  const auto member = [&](const char* name,
                          std::vector<script::Value> args) -> script::HostOutcome {
    const std::uint32_t index =
        f.registry.find(script::CallKind::member, name, static_cast<std::uint16_t>(args.size() - 1));
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = f.registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &f.context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return entry.fn(ctx);
  };
  const auto obj = [](ObjectId id) { return script::Value::object(kTypeObj, id); };
  const auto truthy = [](const script::HostOutcome& out) {
    return out.status == script::HostStatus::ok && out.value.truthy_scalar();
  };

  // `IsIndependentGuarded`: outpost + independent + a `defender_cls_*` roster
  // on the central building's class. Drop any one and it is false.
  CHECK(truthy(member("IsIndependentGuarded", {obj(fort)})));
  CHECK(!truthy(member("IsIndependentGuarded", {obj(quiet)})));   // no roster
  CHECK(!truthy(member("IsIndependentGuarded", {obj(owned)})));   // player 2 owns it
  CHECK(!truthy(member("IsIndependentGuarded", {obj(home)})));    // not an outpost

  // `IsCentralBuliding` is a question about the *class*: `Outpost` and
  // `BaseTownhall` declare `is_central_building="1"`, `Object` declares 0, and
  // nothing about the settlement is consulted.
  CHECK(truthy(member("IsCentralBuliding", {obj(fort)})));
  CHECK(truthy(member("IsCentralBuliding", {obj(home)})));
  CHECK(!truthy(member("IsCentralBuliding", {obj(me)})));

  // `IsValidCaptureTarget` ignores ownership -- `UNIT_CAPTURE_VERIFY.VS` asks
  // `IsEnemy` separately -- and ignores `can_be_captured`. It wants a military
  // receiver and a building that belongs to a settlement.
  CHECK(truthy(member("IsValidCaptureTarget", {obj(me), obj(home)})));
  CHECK(truthy(member("IsValidCaptureTarget", {obj(me), obj(owned)})));
  // A building is not a valid capturer, and a unit is not a valid target.
  CHECK(!truthy(member("IsValidCaptureTarget", {obj(home), obj(owned)})));
  CHECK(!truthy(member("IsValidCaptureTarget", {obj(me), obj(me)})));

  // A tower is a building, is excluded by name, and has no settlement either:
  // `UNIT.SC.XML`'s `<!-- no capture for towers, gates and walls -->`.
  const ObjectId tower = f.world.spawn(NativeClass::town_hall, nullptr, f.tower_class);
  f.world.set_owner(tower, 2);
  CHECK(!truthy(member("IsValidCaptureTarget", {obj(me), obj(tower)})));

  // `IsTTent` is `SettlementKind::teuton_tent`, which none of these are.
  CHECK(!truthy(member("IsTTent", {obj(fort)})));
  CHECK(!truthy(member("IsTTent", {obj(home)})));

  // The invalid-receiver asymmetry `gbr.exe` ships: `IsCentralBuliding`
  // answers *true* for a handle that does not resolve (0x004dd2f3),
  // `IsValidCaptureTarget` answers false (0x005aaeec), and `IsWaterUnit`
  // answers false (0x005ac6d3).
  const script::Value nowhere = script::Value::object(script::ObjectRef{});
  CHECK(truthy(member("IsCentralBuliding", {nowhere})));
  CHECK(!truthy(member("IsValidCaptureTarget", {obj(me), nowhere})));
  CHECK(!truthy(member("IsWaterUnit", {nowhere})));

  // A null `CallContext::user` must fail, never dereference.
  for (const char* name : {"IsCentralBuliding", "IsWaterUnit"}) {
    const std::uint32_t index = f.registry.find(script::CallKind::member, name, 0);
    REQUIRE(index != script::kUnresolvedHost);
    REQUIRE(f.registry.entry(index).fn != nullptr);
    script::CallContext ctx;
    std::vector<script::Value> args{obj(home)};
    ctx.arguments = args;
    ctx.user = nullptr;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    CHECK(f.registry.entry(index).fn(ctx).status == script::HostStatus::error);
  }
}

// --------------------------------------------------------------------------
//
// Two more entry points off the same blocked list, and the two smallest facts
// in `gbr.exe`: one class property and one flag bit.
//
// `UnitsMax` was refused once for lack of evidence. The evidence is the chain
// `sim/world_host.cpp` records instruction by instruction --
// `max_units_to_board` -> class descriptor `+0xb38` -> the holder constructor's
// capacity argument -> `holder + 0x10` -> `Ship::UnitsMax`. `GetParty` is
// `SyncFlags` bit 19, `kSyncParty`, and nothing else.

TEST(units_max_is_the_ship_class_capacity_and_get_party_is_sync_bit_19) {
  ClickFixture f;
  REQUIRE(f.built);
  REQUIRE(f.ship_battle_class != kNoClass);
  REQUIRE(f.ship_rome_class != kNoClass);

  const auto member = [&](const char* name,
                          std::vector<script::Value> args) -> script::HostOutcome {
    const std::uint32_t index =
        f.registry.find(script::CallKind::member, name, static_cast<std::uint16_t>(args.size() - 1));
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = f.registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &f.context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return entry.fn(ctx);
  };
  const auto obj = [](ObjectId id) { return script::Value::object(kTypeObj, id); };
  const auto truthy = [](const script::HostOutcome& out) {
    return out.status == script::HostStatus::ok && out.value.truthy_scalar();
  };
  const auto number = [](const script::HostOutcome& out) {
    return out.status == script::HostStatus::ok && out.value.is_integer() ? out.value.as_integer()
                                                                         : -1;
  };
  const script::Value nowhere = script::Value::object(script::ObjectRef{});

  // -- UnitsMax ------------------------------------------------------------
  //
  // `ShipBattle` declares `max_units_to_board="60"` and is the only shipped
  // class that declares it at all; `ShipRome`, `ShipEgypt` and `ShipL` are its
  // children and declare nothing, so the answer has to come down the tree.
  const World::ShipIds battle = f.world.spawn_ship(nullptr, f.ship_battle_class);
  const World::ShipIds rome = f.world.spawn_ship(nullptr, f.ship_rome_class);
  CHECK(number(member("UnitsMax", {obj(battle.ship)})) == 60);
  CHECK(number(member("UnitsMax", {obj(rome.ship)})) == 60);

  // The three shipped call sites all read `x.AsShip.UnitsMax`, and `AsShip` on
  // a non-ship yields an unresolvable handle. `gbr.exe` reports and pushes 0
  // for that (0x005c6e63-0x005c6e82), so this must be 0 and not a trap --
  // `SHIP_BOARD_VERIFY.VS` compares it against `UnitsCount`.
  CHECK(number(member("UnitsMax", {nowhere})) == 0);

  // A receiver that resolves but is not a ship has no `+0x1dc` to follow. The
  // original would read whatever is at that offset; there is no behaviour to
  // reproduce, so this refuses rather than inventing a capacity.
  const ObjectId me = f.soldier(1);
  CHECK(member("UnitsMax", {obj(me)}).status == script::HostStatus::error);

  // A ship class that declares nothing anywhere up its tree answers 0, the
  // same as an unresolvable receiver -- `class_int` on a missing property.
  const World::ShipIds bare = f.world.spawn_ship(nullptr, f.military);
  f.world.find(bare.ship)->class_index = f.military;
  CHECK(number(member("UnitsMax", {obj(bare.ship)})) == 0);

  // -- GetParty ------------------------------------------------------------
  //
  // Nothing sets bit 19 at load: `<scriptobj flags>` bits 16..21 are clear on
  // all 27,070 objects in the 29 shipped maps, and the only writers are
  // `Unit::SetParty` and `Query::SetParty`, neither of which exists yet.
  CHECK(!truthy(member("GetParty", {obj(me)})));
  CHECK(!truthy(member("GetParty", {nowhere})));

  // Set the bit the way `PartyMgr::Add` (0x004cebd0) does and the entry point
  // must see it. This is the whole of `Unit::GetParty`'s body.
  f.world.find(me)->state.flags.in_party = true;
  CHECK(truthy(member("GetParty", {obj(me)})));

  // And it must be *that* bit, in the word the dumps print: `pack_sync_flags`
  // has to put it at 0x00080000 or a save and a desync dump disagree about
  // what the object is.
  CHECK((f.world.sync_flags(me) & kSyncParty) == kSyncParty);
  CHECK(kSyncParty == 0x00080000u);
  CHECK(unpack_flags(f.world.sync_flags(me)).in_party);
  CHECK(!unpack_flags(f.world.sync_flags(me) & ~kSyncParty).in_party);

  // Bit 19 is independent of bit 21: `IsVisible` must not move when a unit
  // joins the party, and the two are read by different entry points off the
  // same word (0x005ab9ae vs 0x005d7b4e).
  CHECK(truthy(member("IsVisible", {obj(me)})));
  f.world.find(me)->state.flags.hidden = true;
  CHECK(truthy(member("GetParty", {obj(me)})));
  CHECK(!truthy(member("IsVisible", {obj(me)})));

  // A null `CallContext::user` must fail, never dereference.
  for (const char* name : {"UnitsMax", "GetParty"}) {
    const std::uint32_t index = f.registry.find(script::CallKind::member, name, 0);
    REQUIRE(index != script::kUnresolvedHost);
    REQUIRE(f.registry.entry(index).fn != nullptr);
    script::CallContext ctx;
    std::vector<script::Value> args{obj(me)};
    ctx.arguments = args;
    ctx.user = nullptr;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    CHECK(f.registry.entry(index).fn(ctx).status == script::HostStatus::error);
  }
}

// --------------------------------------------------------------------------
// the player argument
// --------------------------------------------------------------------------

TEST(the_point_arithmetic_members_are_integer_euclidean_and_floored) {
  // `Len` (0x00695360) and `Dist` (0x00695310) are the two point members that
  // carry no ambiguity at all: both square their components with `imul`, add,
  // and hand the sum to the table-driven square root at 0x0067c3d0 -- the same
  // routine `AreaDistTo`'s circle branch uses. No floating point, and the
  // result is floored, not rounded.
  //
  // The metric is worth pinning because a neighbour disagrees: a *rectangle*
  // `AreaDistTo` is Chebyshev (sim/area.hpp), and these are Euclidean. They
  // share the square root and nothing else.
  World world;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto call = [&](const char* name, script::CallKind kind,
                        std::vector<script::Value> args) -> script::HostOutcome {
    const std::uint16_t arity = static_cast<std::uint16_t>(
        args.size() - (kind == script::CallKind::member ? 1u : 0u));
    const std::uint32_t index = registry.find(kind, name, arity);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return entry.fn(ctx);
  };
  const auto pt = [](std::int32_t x, std::int32_t y) { return pack_point(Point{x, y}); };
  const auto len = [&](std::int32_t x, std::int32_t y) {
    return call("Len", script::CallKind::member, {pt(x, y)}).value.as_integer();
  };

  CHECK(len(3, 4) == 5);        // exact
  CHECK(len(0, 0) == 0);
  CHECK(len(-3, -4) == 5);      // sign-independent
  CHECK(len(1, 1) == 1);        // floor(1.414), not 1.414 rounded to 1
  CHECK(len(2, 2) == 2);        // floor(2.828) == 2, and rounding would give 3
  CHECK(len(0, 100) == 100);

  // `Dist` is the same shape over a difference, and the member and the free
  // function are **one body registered twice** in gbr.exe (0x006990f3 and
  // 0x00699171), so they cannot disagree. This asserts that they do not.
  const auto member_dist = [&](Point a, Point b) {
    return call("Dist", script::CallKind::member, {pack_point(a), pack_point(b)})
        .value.as_integer();
  };
  const auto free_dist = [&](Point a, Point b) {
    return call("Dist", script::CallKind::free_function, {pack_point(a), pack_point(b)})
        .value.as_integer();
  };
  const Point cases[][2] = {{{0, 0}, {3, 4}}, {{100, 100}, {103, 104}},
                            {{-50, 20}, {-50, 20}}, {{5, 5}, {4, 4}}};
  for (const auto& pair : cases) {
    CHECK(member_dist(pair[0], pair[1]) == free_dist(pair[0], pair[1]));
  }
  CHECK(member_dist(Point{0, 0}, Point{3, 4}) == 5);
  CHECK(member_dist(Point{5, 5}, Point{4, 4}) == 1);   // floor(1.414)
  CHECK(member_dist(Point{-50, 20}, Point{-50, 20}) == 0);
  // Order does not matter: the difference is squared.
  CHECK(member_dist(Point{3, 4}, Point{0, 0}) == member_dist(Point{0, 0}, Point{3, 4}));
}

TEST(no_two_handle_types_share_a_type_code) {
  // A `script::Value` object handle is `(TypeId, id)` and the TypeId is the
  // *only* thing that says how to read the id. The codes are handed out in
  // four different headers -- `world_host.hpp` (1..4), `objlist.hpp` (5),
  // `squad.hpp` (6), `globals.hpp` (7) -- so nothing in the compiler stops two
  // of them landing on the same number, and no other test in this suite ever
  // passes a value of one handle type to an entry point expecting another.
  // That is exactly how `kTypeRect` was appended "after 4" onto `kTypeObjList`.
  //
  // Pinned here as a set rather than as pairwise inequalities so that the next
  // type added fails this test by name if it collides.
  const script::TypeId codes[] = {kTypeObj,     kTypePoint, kTypeQuery,    kTypeSettlement,
                                  kTypeObjList, kTypeSquad, kTypeNamedObj, kTypeRect};
  const std::size_t n = sizeof(codes) / sizeof(codes[0]);
  for (std::size_t i = 0; i < n; ++i) {
    // Zero is `kNoType`, the invalid handle every `.IsValid` reports on.
    CHECK(codes[i] != 0);
    for (std::size_t j = i + 1; j < n; ++j) {
      if (codes[i] == codes[j]) {
        std::printf("    type codes %zu and %zu are both %d\n", i, j,
                    static_cast<int>(codes[i]));
      }
      CHECK(codes[i] != codes[j]);
    }
  }
}

TEST(an_objlist_is_not_a_rectangle_and_the_rect_members_say_so) {
  // The behavioural half of the test above: with a shared code the predicates
  // cannot tell the two apart, so `IntoRect` would read an `ObjList`'s pool id
  // as an index into the rect table and clamp against whatever geometry
  // happened to be interned there -- silently, with no diagnostic anywhere.
  World world;
  (void)world.rects().intern(RectTable::Rect{10, 10, 20, 20});
  const script::Value list = make_objlist_value(1);
  CHECK(is_objlist(list));
  CHECK(!is_rect(list));
  CHECK(unpack_rect(world, list) == nullptr);
}

TEST(a_rect_is_interned_by_value_so_two_equal_rectangles_are_one_entry) {
  // A VS `rect` is a value -- `rc2 = rc1` copies four integers -- and four
  // int32s do not fit in a `script::Value`'s object id, so it lives in a table.
  // Interning by value is what makes the table behave like a value: equal
  // geometry is one index, so a copy shares it and nothing needs cloning on
  // assignment. That is the opposite of `ObjListPool`, whose entries are
  // mutable and must not be shared, and getting the two backwards would be a
  // silent divergence.
  World world;
  const std::uint32_t a = world.rects().intern(RectTable::Rect{0, 0, 100, 100});
  const std::uint32_t b = world.rects().intern(RectTable::Rect{0, 0, 100, 100});
  const std::uint32_t c = world.rects().intern(RectTable::Rect{0, 0, 100, 101});
  CHECK(a == b);
  CHECK(a != c);
  CHECK(world.rects().size() == 2);
  REQUIRE(world.rects().find(a) != nullptr);
  CHECK(world.rects().find(a)->right == 100);
  CHECK(world.rects().find(99) == nullptr);
}

TEST(the_rectangle_entry_points_are_inclusive_on_all_four_edges) {
  // Four independent readings of gbr.exe agree on this and they are the reason
  // it gets its own test: `width` is `right - left + 1` (0x00696c73), `InRect`
  // uses `jl` on the low edges and `jg` on the high ones (0x00696faf),
  // `IntoRect` clamps only when `x > right` (0x00697cef), and the map rectangle
  // is built as `(0, 0, w-1, h-1)` (0x00541f25) -- which only covers the map if
  // the high edges are in.
  World world;
  MatchSystem match;
  REQUIRE(world.add_system(&match));
  MatchRules rules;
  rules.map_size = 1000;
  match.configure(rules, 0, false);

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto call = [&](const char* name, script::CallKind kind,
                        std::vector<script::Value> args) -> script::HostOutcome {
    const std::uint16_t arity = static_cast<std::uint16_t>(
        args.size() - (kind == script::CallKind::member ? 1u : 0u));
    const std::uint32_t index = registry.find(kind, name, arity);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return entry.fn(ctx);
  };

  const script::HostOutcome rect = call("GetMapRect", script::CallKind::free_function, {});
  REQUIRE(rect.status == script::HostStatus::ok);
  const script::Value rc = rect.value;
  REQUIRE(is_rect(rc));

  // `(0, 0, size - 1, size - 1)`: the map rectangle's high edges are the last
  // legal coordinate, which is the same quantity `MapSize()` answers.
  CHECK(call("left", script::CallKind::member, {rc}).value.as_integer() == 0);
  CHECK(call("top", script::CallKind::member, {rc}).value.as_integer() == 0);
  CHECK(call("right", script::CallKind::member, {rc}).value.as_integer() == 999);
  CHECK(call("bottom", script::CallKind::member, {rc}).value.as_integer() == 999);
  // ...and `width` adds the one back, which is the inclusive bound showing
  // through: the map is 1000 units across, not 999.
  CHECK(call("width", script::CallKind::member, {rc}).value.as_integer() == 1000);
  CHECK(call("height", script::CallKind::member, {rc}).value.as_integer() == 1000);

  const auto in_rect = [&](std::int32_t x, std::int32_t y) {
    return call("InRect", script::CallKind::member, {pack_point(Point{x, y}), rc})
               .value.as_integer() != 0;
  };
  CHECK(in_rect(0, 0));
  CHECK(in_rect(999, 999));    // the far corner is inside
  CHECK(!in_rect(1000, 999));
  CHECK(!in_rect(-1, 0));

  const auto into_rect = [&](std::int32_t x, std::int32_t y) {
    std::vector<script::Value> args{pack_point(Point{x, y}), rc};
    const std::uint32_t index = registry.find(script::CallKind::member, "IntoRect", 1);
    const script::HostEntry& entry = registry.entry(index);
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "IntoRect";
    ctx.kind = script::CallKind::member;
    (void)entry.fn(ctx);
    return unpack_point(ctx.arguments[0]);
  };
  const auto is = [](Point p, std::int32_t x, std::int32_t y) { return p.x == x && p.y == y; };
  CHECK(is(into_rect(500, 500), 500, 500));      // inside, untouched
  CHECK(is(into_rect(-40, 500), 0, 500));
  CHECK(is(into_rect(5000, 500), 999, 500));     // clamped to `right`, not `right - 1`
  CHECK(is(into_rect(5000, -40), 999, 0));
  CHECK(is(into_rect(999, 999), 999, 999));      // already on the edge

  // `ClampToMap` is NOT `IntoRect(GetMapRect())`, and 0x005bd3fd is the
  // difference: it clamps to `right - 1`, one unit tighter, and returns a new
  // point instead of mutating.
  const script::HostOutcome clamped =
      call("ClampToMap", script::CallKind::member, {pack_point(Point{5000, -40})});
  REQUIRE(clamped.status == script::HostStatus::ok);
  CHECK(is(unpack_point(clamped.value), 998, 0));
}

TEST(into_rect_does_not_normalise_a_reversed_rectangle) {
  // The clamps run low-edge-then-high-edge per axis with no `min`/`max` on the
  // rectangle itself (0x00697ce9..0x00697cfd), so on a reversed rectangle the
  // second clamp wins and every point lands on `(right, bottom)`.
  //
  // This is transcribed, not chosen, and the executable does not agree with
  // itself: `IntersectRects` (0x00696cd0) manufactures reversed rectangles from
  // disjoint inputs with no emptiness check, `AddRects` (0x0048c5dd) treats a
  // reversed one as empty, and this treats it as neither. Nothing shipped
  // exercises it -- all 29 corpus rectangles are `GetMapRect()` -- so
  // reproducing this exactly costs nothing and guessing would have been free
  // to be wrong.
  World world;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const script::Value reversed = pack_rect(world, RectTable::Rect{800, 700, 100, 50});
  const auto into = [&](std::int32_t x, std::int32_t y) {
    std::vector<script::Value> args{pack_point(Point{x, y}), reversed};
    const std::uint32_t index = registry.find(script::CallKind::member, "IntoRect", 1);
    const script::HostEntry& entry = registry.entry(index);
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "IntoRect";
    ctx.kind = script::CallKind::member;
    (void)entry.fn(ctx);
    return unpack_point(ctx.arguments[0]);
  };
  // Every point, wherever it started, ends on the high corner.
  for (const Point start : {Point{0, 0}, Point{500, 400}, Point{9999, 9999}, Point{-9, -9}}) {
    const Point out = into(start.x, start.y);
    CHECK(out.x == 100 && out.y == 50);
  }
}

TEST(rotating_by_ninety_reproduces_the_originals_floating_point_bias) {
  // **`Rot(90)` is not `(y, -x)`.** 0x00697d90 is x87 floating point, and the
  // constant it multiplies by (0x007ac490) is pi truncated to eleven digits,
  // so `cos(90 * K)` comes out as 4.4896592e-11 rather than zero. The result
  // truncates toward zero, so that vanishing term becomes an exact +/-1
  // decided by two signs -- and reproducing it needs no floating point, only
  // the sign.
  //
  // Every expectation below was computed twice: once from the instruction
  // sequence by hand, and once by evaluating the same doubles independently.
  // The two agreed, which is why they are written as literals here.
  World world;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto rot = [&](Point v, std::int32_t degrees) -> Point {
    const std::uint32_t index = registry.find(script::CallKind::member, "Rot", 1);
    if (index == script::kUnresolvedHost) return Point{-12345, -12345};
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return Point{-12345, -12345};
    std::vector<script::Value> args{pack_point(v), script::Value::integer(degrees)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "Rot";
    ctx.kind = script::CallKind::member;
    (void)entry.fn(ctx);
    // In place: the VM copies argument 0 back into the caller's local, which is
    // what the by-reference receiver type 0x106 means at registration.
    return unpack_point(ctx.arguments[0]);
  };
  const auto is = [](Point p, std::int32_t x, std::int32_t y) { return p.x == x && p.y == y; };

  CHECK(is(rot(Point{1000, 1000}, 90), 1000, -999));      // not -1000
  CHECK(is(rot(Point{1000, -1000}, 90), -999, -1000));    // not -1000
  CHECK(is(rot(Point{-1000, 1000}, 90), 999, 1000));      // not 1000
  CHECK(is(rot(Point{3, -7}, 90), -6, -3));               // not -7
  // Where a component is zero the bias vanishes, which is why a test that only
  // used axis-aligned vectors would pass against the naive rotation too.
  CHECK(is(rot(Point{500, 0}, 90), 0, -500));
  CHECK(is(rot(Point{0, 500}, 90), 500, 0));
  CHECK(is(rot(Point{0, 0}, 90), 0, 0));
  CHECK(is(rot(Point{12345, -6789}, 90), -6788, -12345));

  // The consequence, and the reason this is worth reproducing: the shipped
  // patrol idiom turns a vector through four right angles and expects to get
  // it back. It does not.
  Point p{3, -7};
  for (int i = 0; i < 4; ++i) p = rot(p, 90);
  CHECK(is(p, 3, -3));  // began at (3, -7)
}

TEST(set_len_gives_the_zero_vector_a_direction_instead_of_leaving_it_zero) {
  // 0x00697d81 is the branch that matters: with a zero length the original
  // writes `L` into *y* and zero into *x*, so a direction-less vector comes
  // back pointing north. `sim/path.hpp` says the opposite in as many words --
  // "a zero vector has no direction and stays zero" -- and cites DEER_IDLE.VS
  // calling `.GetDir` first as the evidence. The reasoning is good and the
  // conclusion is wrong; the original picks a direction.
  //
  // Everything else about it is plain integer: isqrt, then `x*L/len` with
  // `idivl`, which truncates toward zero.
  World world;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto set_len = [&](Point v, std::int32_t length) -> Point {
    const std::uint32_t index = registry.find(script::CallKind::member, "SetLen", 1);
    const script::HostEntry& entry = registry.entry(index);
    std::vector<script::Value> args{pack_point(v), script::Value::integer(length)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "SetLen";
    ctx.kind = script::CallKind::member;
    (void)entry.fn(ctx);
    return unpack_point(ctx.arguments[0]);
  };
  const auto is = [](Point p, std::int32_t x, std::int32_t y) { return p.x == x && p.y == y; };

  CHECK(is(set_len(Point{0, 0}, 500), 0, 500));   // north, NOT (0, 0)
  CHECK(is(set_len(Point{0, 0}, 0), 0, 0));       // and a zero length is still zero
  CHECK(is(set_len(Point{100, 0}, 50), 50, 0));
  CHECK(is(set_len(Point{0, -100}, 50), 0, -50));
  // Truncation toward zero, not rounding: |(3,4)| is 5, so (3,4) at length 7
  // is (4.2, 5.6) -> (4, 5).
  CHECK(is(set_len(Point{3, 4}, 7), 4, 5));
  // And the idiom this exists for: grow a vector by a fixed amount.
  const Point v{300, 400};
  CHECK(is(set_len(v, 500 + 50), 330, 440));
}

TEST(the_other_cardinal_angles_carry_their_own_vanishing_terms) {
  // `Rot(0)` is the identity because sin(0) and cos(0) are exact. None of the
  // others is clean: sin(180) is +8.98e-11, cos(270) is -1.35e-10 and sin(360)
  // is -1.80e-10, each with its own sign, so each biases a different component
  // in a different direction. **`Rot(360)` is not the identity**, and a shipped
  // site reaches it -- `vect.Rot(rand(361))` in Numantia's seq23.
  World world;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto rot = [&](Point v, std::int32_t degrees) -> Point {
    const std::uint32_t index = registry.find(script::CallKind::member, "Rot", 1);
    const script::HostEntry& entry = registry.entry(index);
    std::vector<script::Value> args{pack_point(v), script::Value::integer(degrees)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "Rot";
    ctx.kind = script::CallKind::member;
    (void)entry.fn(ctx);
    return unpack_point(ctx.arguments[0]);
  };
  const auto is = [](Point p, std::int32_t x, std::int32_t y) { return p.x == x && p.y == y; };

  CHECK(is(rot(Point{1000, 1000}, 0), 1000, 1000));      // the only clean one
  CHECK(is(rot(Point{1000, 1000}, 180), -999, -1000));
  CHECK(is(rot(Point{1000, 1000}, 270), -1000, 999));
  CHECK(is(rot(Point{1000, 1000}, 360), 999, 1000));     // NOT the identity

  // A non-cardinal angle goes through the Q30 table, which is generated from
  // the retail constant rather than from pi/180. Exactness is not claimed here
  // -- see the note in world_host.cpp -- but the geometry must still be right:
  // 45 degrees off the x axis is the diagonal, and the handedness is the
  // original's transpose, so y goes negative.
  CHECK(is(rot(Point{1000, 0}, 45), 707, -707));
}

TEST(the_point_constructor_does_nothing_but_pack_two_integers) {
  // 0x00696e50 is fifteen instructions with **no branch in them**: no clamp,
  // no validity marker, no `(0, 0)` fixup. The last of those is the one worth
  // a test. `BARRACK_TRAIN.VS` passes `Point(0, 0)` to `Place` deliberately,
  // and the `(0,0)` -> `y = 1` adjustment lives downstream in the position
  // setter (0x0053f60e); a constructor that fixed it up here would make that
  // adjustment unreachable and change where trained units appear.
  World world;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto make = [&](std::int32_t x, std::int32_t y) -> script::HostOutcome {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "Point", 2);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    std::vector<script::Value> args{script::Value::integer(x), script::Value::integer(y)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "Point";
    ctx.kind = script::CallKind::free_function;
    return entry.fn(ctx);
  };

  const script::HostOutcome origin = make(0, 0);
  REQUIRE(origin.status == script::HostStatus::ok);
  CHECK(is_point(origin.value));
  CHECK(unpack_point(origin.value).x == 0);
  CHECK(unpack_point(origin.value).y == 0);  // NOT (0, 1)

  // Argument order is (x, y): the first argument is the one `point::x` reads.
  const script::HostOutcome asymmetric = make(7, -9);
  REQUIRE(asymmetric.status == script::HostStatus::ok);
  CHECK(unpack_point(asymmetric.value).x == 7);
  CHECK(unpack_point(asymmetric.value).y == -9);

  // Negatives survive: the packing sign-extends both halves.
  const script::HostOutcome corner = make(-1, -1);
  REQUIRE(corner.status == script::HostStatus::ok);
  CHECK(unpack_point(corner.value).x == -1);
  CHECK(unpack_point(corner.value).y == -1);
}

TEST(world_host_reads_a_player_argument_as_the_script_writes_it_one_based) {
  // A wrong answer that looked right, and nothing here caught it.
  //
  // `m_player` returns `player_to_script(owner)` and carries a long comment
  // about script player numbers being 1..16 -- that direction was fixed across
  // 531 call sites. The *argument* direction in this same file was not: the
  // local `player_arg` took the script's integer raw, so `ClassPlayerObjs`,
  // `ClassPlayerAreaObjs` and `Count` each answered for the slot one below the
  // one the script named. For `player 1`, the human, that is player 0 -- a
  // populated slot on most maps, so the result was a plausible non-empty set.
  //
  // The whole C++ suite passed before and after the fix. This test is the
  // thing that was missing, and it is written to fail if `player_arg` ever
  // stops converting: with the raw reading, `Count(1, ...)` counts player 1's
  // objects and the two `CHECK`s below swap.
  //
  // The corpus is what settles which reading is right, and the control is the
  // interesting half: of the 33 sites in all 885 shipped scripts that pass a
  // literal to one of the three, the values are 1, 2, 3, 4 and 5 -- and 0 does
  // not occur once. Read 0-based, 0 would be the commonest value.
  World world;
  const ObjectId first = world.spawn(NativeClass::unit, nullptr);
  const ObjectId second = world.spawn(NativeClass::unit, nullptr);
  const ObjectId third = world.spawn(NativeClass::unit, nullptr);
  world.set_owner(first, 0);
  world.set_owner(second, 1);
  world.set_owner(third, 1);

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);

  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  // Explicit return type: `REQUIRE` expands to a `return` on failure, which
  // makes a deduced-return lambda see both `void` and `HostOutcome`.
  const auto call = [&](const char* name,
                        std::vector<script::Value> args) -> script::HostOutcome {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, name,
                      static_cast<std::uint16_t>(args.size()));
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::free_function;
    return entry.fn(ctx);
  };

  // `Count(player, "class")` answers immediately with an integer, so it needs
  // no query handle and reads the miscount directly.
  const script::HostOutcome one =
      call("Count", {script::Value::integer(1), script::Value::string("")});
  REQUIRE(one.status == script::HostStatus::ok);
  CHECK(one.value.as_integer() == 1);  // script 1 is PlayerId 0, which owns one

  const script::HostOutcome two =
      call("Count", {script::Value::integer(2), script::Value::string("")});
  REQUIRE(two.status == script::HostStatus::ok);
  CHECK(two.value.as_integer() == 2);  // script 2 is PlayerId 1, which owns two

  // 0 is not a player, and the answer must be *nothing* rather than
  // *everything*. `objects_of_class_for_player` reads `kNoPlayer` as "any
  // owner", so handing an unresolved number straight to it would answer with
  // every object on the map -- a wrong set that composes and hashes, which is
  // the shape of the `EnemyObjs` bug. Read raw, as this file did before, 0 was
  // `PlayerId 0` and answered 1. All three readings differ, and only one is
  // safe; no shipped script passes 0, which is a reason to choose deliberately
  // rather than a reason not to choose.
  const script::HostOutcome zero =
      call("Count", {script::Value::integer(0), script::Value::string("")});
  REQUIRE(zero.status == script::HostStatus::ok);
  CHECK(zero.value.as_integer() == 0);

  // 16 is the last valid script number and must not be rejected; 17 must be.
  const ObjectId last = world.spawn(NativeClass::unit, nullptr);
  world.set_owner(last, 15);
  const script::HostOutcome sixteen =
      call("Count", {script::Value::integer(16), script::Value::string("")});
  REQUIRE(sixteen.status == script::HostStatus::ok);
  CHECK(sixteen.value.as_integer() == 1);
  const script::HostOutcome seventeen =
      call("Count", {script::Value::integer(17), script::Value::string("")});
  REQUIRE(seventeen.status == script::HostStatus::ok);
  CHECK(seventeen.value.as_integer() == 0);

  // The same conversion, through a query rather than a count.
  const script::HostOutcome objs =
      call("ClassPlayerObjs", {script::Value::string(""), script::Value::integer(2)});
  REQUIRE(objs.status == script::HostStatus::ok);
  std::vector<ObjectId> found;
  world.evaluate_query(objs.value.as_object().id, found);
  REQUIRE(found.size() == 2);
  CHECK(found[0] == second);
  CHECK(found[1] == third);
}

// ==========================================================================
// Erase
// ==========================================================================

namespace {

/// Parse and compile one script into `scheduler`, saying why if it fails.
/// A silently unbuilt chunk would make every case below pass by never running.
std::uint32_t build_chunk(script::Scheduler& scheduler, const script::HostRegistry& registry,
                          std::string_view source, const char* name) {
  script::Diagnostic diagnostic;
  const auto parsed = script::parse(bytes_of(source), name, &diagnostic);
  if (!parsed.ok()) {
    std::printf("  parse %s:%u: %.*s\n", name, diagnostic.line,
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
    return script::kNoChunk;
  }
  script::CompileError error;
  auto chunk = script::compile(parsed.value(), &registry, &error);
  if (!chunk.ok()) {
    std::printf("  compile %s:%u: %s\n", name, error.line, error.message.c_str());
    return script::kNoChunk;
  }
  return scheduler.add_chunk(std::move(chunk.value()));
}

/// A world, the whole host surface, and a scheduler with both lifetime hooks on
/// it -- which is what `GameSession` builds, and what a self-erase needs.
struct EraseBench {
  TinyGraph classes;
  World world;
  EnvSystem env;
  script::HostRegistry registry;
  HostContext context;
  WorldHost host{world};
  script::Scheduler scheduler;

  EraseBench() {
    world.set_class_graph(&classes.graph);
    world.add_system(&env);
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    scheduler.set_user(&context);
    install_deferred_erase(scheduler);
  }

  ObjectId unit() {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, classes.unit);
    if (ObjectState* state = world.mutable_state(id)) state->flags.is_unit = true;
    return id;
  }
  ObjectId fort() {
    const ObjectId id = world.spawn(NativeClass::building, nullptr, classes.building);
    if (ObjectState* state = world.mutable_state(id)) state->flags.is_building = true;
    return id;
  }

  /// Start `source` as `owner`'s behaviour script, the way an idle script runs.
  script::ScriptId run_on(ObjectId owner, std::string_view source, const char* name) {
    const std::uint32_t chunk = build_chunk(scheduler, registry, source, name);
    if (chunk == script::kNoChunk) return script::kNoScript;
    const script::ObjectRef self{kTypeObj, owner};
    const script::Value args[] = {script::Value::object(self)};
    return scheduler.spawn(chunk, args, self);
  }

  /// Call `Erase` directly, with no script behind it.
  script::HostOutcome erase(script::Value receiver) {
    const std::uint32_t index = registry.find(script::CallKind::member, "Erase", 0);
    if (index == script::kUnresolvedHost) {
      return script::HostOutcome::failed("Erase is not declared");
    }
    script::CallContext ctx;
    std::vector<script::Value> args{receiver};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.scheduler = &scheduler;
    ctx.name = "Erase";
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  }

  [[nodiscard]] bool alive(ObjectId id) const { return world.find(id) != nullptr; }
  [[nodiscard]] std::int32_t flag(const char* key) const {
    return env.env().read_int(EnvScope::root(), key);
  }
};

}  // namespace

/// The plain form: the object leaves the world, and its group memberships go
/// with it.
TEST(erase_takes_the_object_out_of_the_world) {
  EraseBench b;
  const ObjectId a = b.unit();
  const ObjectId c = b.unit();
  const std::int32_t group = b.world.groups().intern("Doomed");
  REQUIRE(b.world.groups().add(group, a));
  REQUIRE(b.world.groups().add(group, c));
  REQUIRE(b.world.groups().members(group).size() == 2);

  const script::HostOutcome out = b.erase(script::Value::object(kTypeObj, a));
  CHECK(out.status == script::HostStatus::ok);
  CHECK(!b.alive(a));
  CHECK(b.alive(c));
  // `despawn` drops the membership, which is what `0x005141b0`'s control-group
  // sweep and its two back-reference fixups amount to.
  CHECK(b.world.groups().members(group).size() == 1);
}

/// **Every coroutine the object owns stops.**
///
/// `0x00687c80` cancels the object's pending scheduled messages, and a
/// cancelled message is a `Sleep` that never wakes. Without this an erased
/// object's idle script polls a dead handle for the rest of the match, and a
/// test that only checked the object was gone would not notice.
TEST(erase_stops_every_coroutine_the_object_owns) {
  EraseBench b;
  const ObjectId doomed = b.unit();
  const ObjectId bystander = b.unit();

  const script::ScriptId one =
      b.run_on(doomed, "// void, Obj This\nwhile (1) Sleep(1000);\n", "one.vs");
  const script::ScriptId two =
      b.run_on(doomed, "// void, Obj This\nwhile (1) Sleep(1000);\n", "two.vs");
  const script::ScriptId other =
      b.run_on(bystander, "// void, Obj This\nwhile (1) Sleep(1000);\n", "other.vs");
  REQUIRE(one != script::kNoScript);
  REQUIRE(two != script::kNoScript);
  REQUIRE(other != script::kNoScript);
  b.scheduler.advance(10);
  REQUIRE(b.scheduler.alive(one));

  b.erase(script::Value::object(kTypeObj, doomed));
  CHECK(!b.scheduler.alive(one));
  CHECK(!b.scheduler.alive(two));
  // And nobody else's.
  CHECK(b.scheduler.alive(other));
}

/// A self-erase defers: the object survives the rest of its own slice.
///
/// `CObject::Erase` (0x005a7580) compares the receiver against the running
/// script object and sets the latch at `[0x009bdb14]` instead of destroying
/// anything. Six of the 47 shipped sites are a self-erase with nothing after
/// it; two -- `CATAPULT_IDLE.VS:108` and `HEROGRAVE_BEHAVIOR.VS:107` -- have a
/// statement after it and depend on the order.
TEST(a_script_erasing_its_own_object_defers_until_the_slice_ends) {
  EraseBench b;
  const ObjectId self = b.unit();
  const script::ScriptId script = b.run_on(self,
                                           "// void, Obj This\n"
                                           "int seen;\n"
                                           "This.Erase();\n"
                                           "if (This.IsValid) seen = 1; else seen = 2;\n"
                                           "EnvWriteInt(\"/seen\", seen);\n",
                                           "self.vs");
  REQUIRE(script != script::kNoScript);
  // Armed, not fired: nothing has run yet.
  CHECK(b.alive(self));
  b.scheduler.advance(10);
  // The statement after `.Erase()` still saw its own object.
  CHECK(b.flag("/seen") == 1);
  // And by the end of the slice it is gone.
  CHECK(!b.alive(self));
  CHECK(b.world.deferred_erase() == kNoObject);
}

/// `HEROGRAVE_BEHAVIOR.VS`'s ending, in miniature: `.Erase(); Sleep(100000);`.
///
/// The sleep schedules a wake-up, the deferred erase cancels it, and the script
/// is never resumed. An implementation that despawned the object and left the
/// coroutine alone would have the grave's script sitting in the scheduler for a
/// hundred seconds of game time and then running on with a dead receiver.
TEST(a_sleep_after_a_self_erase_never_wakes) {
  EraseBench b;
  const ObjectId grave = b.unit();
  const script::ScriptId script = b.run_on(grave,
                                           "// void, Obj This\n"
                                           "This.Erase();\n"
                                           "Sleep(100000);\n"
                                           "EnvWriteInt(\"/woke\", 1);\n",
                                           "grave.vs");
  REQUIRE(script != script::kNoScript);
  b.scheduler.advance(10);
  CHECK(!b.alive(grave));
  CHECK(!b.scheduler.alive(script));
  b.scheduler.advance(200000);
  CHECK(b.flag("/woke") == 0);
}

/// The deferral is over before the **next** script runs, not at the end of the
/// pass.
///
/// The original performs it from the runner's per-step hook (`0x0069f7a0` calls
/// `vtbl[0x24]` the moment the bytecode returns), so a second coroutine in the
/// same turn sees an object that is already gone. A pass hook would be one turn
/// late and would only ever diverge once two scripts touched the same object in
/// one turn -- which is the sort of thing that is found six months later.
TEST(a_deferred_erase_is_over_before_the_next_script_in_the_same_pass_runs) {
  EraseBench b;
  const ObjectId self = b.unit();
  // Spawned first, so it runs first: ids are issued in order and the pass walks
  // them in that order.
  REQUIRE(b.run_on(self,
                   "// void, Obj This\n"
                   "This.Erase();\n",
                   "first.vs") != script::kNoScript);
  const ObjectId watcher = b.unit();
  REQUIRE(b.run_on(watcher,
                   "// void, Obj This\n"
                   "Obj o;\n"
                   "o = GetNamedObj(\"NO_target\").obj;\n"
                   "if (o.IsValid) EnvWriteInt(\"/still_there\", 1);\n",
                   "second.vs") != script::kNoScript);
  b.world.named_objects().bind("NO_target", self);

  b.scheduler.advance(10);
  CHECK(!b.alive(self));
  CHECK(b.flag("/still_there") == 0);
}

/// The query form erases every element **except the buildings**.
///
/// `0x00578c40` skips any element carrying bit 23 of `[obj+0x2c]` -- the bit
/// `Obj::AsBuilding` tests and this engine keeps as `ObjectFlags::is_building`
/// -- as a `continue` and not an abort. So a query holding six units and a wall
/// loses the six and keeps the wall, silently.
TEST(the_query_form_of_erase_skips_buildings_and_keeps_going) {
  EraseBench b;
  const ObjectId first = b.unit();
  const ObjectId wall = b.fort();
  const ObjectId last = b.unit();
  for (const ObjectId id : {first, wall, last}) {
    REQUIRE(b.world.set_position(id, Point{0, 0}));
  }

  const ObjectId query =
      b.world.create_query(objs_in_circle(Point{0, 0}, 100, ClassFilter{}));
  REQUIRE(query != kNoObject);
  std::vector<ObjectId> found;
  REQUIRE(b.world.evaluate_query(query, found) == 3);

  const script::HostOutcome out = b.erase(script::Value::object(kTypeObj, query));
  CHECK(out.status == script::HostStatus::ok);
  CHECK(!b.alive(first));
  CHECK(!b.alive(last));
  // The building is still standing.
  CHECK(b.alive(wall));
  // And the query object itself survives: `0x00578c40` releases one reference
  // on it and erases nothing.
  CHECK(b.alive(query));
  found.clear();
  CHECK(b.world.evaluate_query(query, found) == 1);
}

/// A receiver that resolves to nothing is a no-op, not a refusal.
///
/// Both bodies format a complaint into `0x00686eb0`, which is a bare `ret` in
/// the retail build. And an `ObjList` receiver is one of those cases: there is
/// no `ObjList::Erase` in the image at all, so a script that wrote one would
/// have found the same silence.
TEST(erase_refuses_nothing_and_answers_nothing) {
  EraseBench b;
  const ObjectId survivor = b.unit();
  const auto quiet = [&](script::Value receiver) {
    const script::HostOutcome out = b.erase(receiver);
    return out.status == script::HostStatus::ok;
  };
  CHECK(quiet(script::Value::object(kTypeObj, 9999)));  // a dead id
  CHECK(quiet(script::Value::integer(3)));              // not a handle
  CHECK(quiet(script::Value::string("nope")));          // not a handle either
  CHECK(b.alive(survivor));

  // And with no world behind the call at all it fails rather than dereferences,
  // which is the rule every entry point in this tree is held to.
  script::CallContext bare;
  std::vector<script::Value> args{script::Value::object(kTypeObj, survivor)};
  bare.arguments = args;
  bare.user = nullptr;
  const std::uint32_t index = b.registry.find(script::CallKind::member, "Erase", 0);
  REQUIRE(index != script::kUnresolvedHost);
  CHECK(b.registry.entry(index).fn(bare).status == script::HostStatus::error);
  CHECK(b.alive(survivor));
}

/// The two spellings the corpus actually uses.
///
/// `UNIT_DISMISS.VS` is three lines and the third is `This.Erase;` -- **no
/// parentheses** -- and `COUTPOST_BEHAVIOR.VS:21` writes `ol[i].Erase;` the
/// same way, so the statement form has to parse identically to a call. The
/// other six self-erases go through the implicit receiver `.`, which binds to
/// the variable named `this`; that is how `HEROGRAVE_BEHAVIOR.VS` and
/// `CATAPULT_IDLE.VS` write it.
TEST(erase_parses_in_both_shipped_spellings) {
  {
    EraseBench b;
    const ObjectId id = b.unit();
    REQUIRE(b.run_on(id, "// void, Obj This\nThis.Erase;\n", "bare.vs") != script::kNoScript);
    b.scheduler.advance(10);
    CHECK(!b.alive(id));
  }
  {
    EraseBench b;
    const ObjectId id = b.unit();
    REQUIRE(b.run_on(id,
                     "// void, Obj This\n"
                     "Obj this;\n"
                     "this = This;\n"
                     ".Erase();\n",
                     "implicit.vs") != script::kNoScript);
    b.scheduler.advance(10);
    CHECK(!b.alive(id));
  }
}

// ==========================================================================
// the small predicates
// ==========================================================================

namespace {

/// A world with the whole host surface and the systems the four names touch.
struct PredicateBench {
  TinyGraph classes;
  World world;
  EconomySystem economy;
  // The ship's transport order lives on the AI system, which is why a bench
  // that tests `HasAiTransport` needs one; see `sim/ai.hpp`.
  AiSystem ai;
  SelectionTable selections;
  script::HostRegistry registry;
  HostContext context;

  PredicateBench() {
    world.set_class_graph(&classes.graph);
    REQUIRE(world.add_system(&economy));
    REQUIRE(world.add_system(&ai));
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
    context.selections = &selections;
    economy.start(world);
  }

  script::HostOutcome call(script::CallKind kind, const char* name, std::uint16_t arity,
                           std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(kind, name, arity);
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
    ctx.kind = kind;
    return registry.entry(index).fn(ctx);
  }

  bool predicate(const char* name, script::Value receiver) {  // NOLINT
    const script::HostOutcome out = call(script::CallKind::member, name, 0, {receiver});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }

  static script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }
};

}  // namespace

/// `IsInAir` and `IsBuilding` read one bit each, and both answer **false** for a
/// receiver that does not resolve rather than refusing.
///
/// Neither is the predicate its name suggests to a reader of the call sites.
/// `IsInAir` is registered once, on `Flying`, and is bit 22 of the *second*
/// flag word `[obj+0x194]` -- there is no `Settlement::IsInAir`, which an
/// earlier census of this project's had. `IsBuilding` is registered on `Ship`
/// and is `[ship+0x230] > 0`, a construction counter: it means *still being
/// built*, and it has nothing to do with the `0x00800000` Building bit that
/// `Obj::AsBuilding` tests. `SHIP_IDLE.VS` settles that from the corpus side --
/// `if (.IsBuilding()) { while (.IsBuilding()) Sleep(100); ... }` would never
/// terminate under a class test.
/// `AsTower` is `AsBuilding` -- the **same function** in `gbr.exe`, registered
/// twice under two names and two declared return types.
///
/// The claim this pins is that the cast admits every building and rejects
/// everything else, rather than looking for some tower-shaped class. There is
/// no tower `cpp_class` to look for: `sim/world_host.cpp` records the body
/// address the two registrations share (0x005aa5e0) and the single bit it
/// tests. `OUTPOST_ATTACK_VERIFY.VS` reads the same way from the corpus side --
/// it casts every building in the settlement and *then* asks `IsHeirOf`.
/// The damage tier, as arithmetic. `75 / 50 / 25` with a hysteresis of 3, the
/// numbers `CONST.INI` ships.
TEST(sim_building_damage_tiers_follow_the_thresholds) {
  const BuildingStateRules rules;
  const auto tier = [&](std::int32_t from, std::int32_t pct) {
    return next_building_state(from, pct, 100, rules);
  };

  // From undamaged, downwards. Each step needs the boundary cleared by the
  // hysteresis as well, so 74 is not yet tier 1 -- 72 is.
  CHECK(tier(0, 100) == 0);
  CHECK(tier(0, 75) == 0);   // not *below* the threshold
  CHECK(tier(0, 74) == 0);   // below it, but inside the hysteresis band
  CHECK(tier(0, 72) == 1);
  CHECK(tier(1, 49) == 1);
  CHECK(tier(1, 47) == 2);
  CHECK(tier(2, 24) == 2);
  CHECK(tier(2, 22) == 3);

  // Back up. The band is on the *new* tier's boundary, so leaving tier 1 needs
  // 78, not 75.
  CHECK(tier(1, 76) == 1);
  CHECK(tier(1, 78) == 0);
  CHECK(tier(3, 27) == 3);
  CHECK(tier(3, 28) == 2);

  // **A jump of more than one step ignores the hysteresis entirely.** A wall
  // taken from full health to 24% in one blow is tier 3 at once, not tier 1
  // waiting for two more hits. 0x004db41b tests `raw == old + 1` and
  // `raw == old - 1` and nothing else.
  CHECK(tier(0, 24) == 3);
  CHECK(tier(3, 100) == 0);

  // The comparisons are **strict**, and a multi-step jump is where that shows:
  // exactly 25% is not below `threshold2`, so it is tier 2 and not tier 3, and
  // no hysteresis band is in the way to hide the difference.
  CHECK(tier(0, 25) == 2);
  CHECK(tier(0, 50) == 1);

  // No maximum is no percentage: a decor holds whatever tier it had.
  CHECK(next_building_state(2, 5, 0, rules) == 2);

  // The division is integer and truncating, and the comparison is strict.
  // 3749/5000 is 74.98%, which truncates to 74 -- inside the band, not below it.
  CHECK(next_building_state(0, 3749, 5000, rules) == 0);
  CHECK(next_building_state(0, 3599, 5000, rules) == 1);

  // **The band is for a single step and for nothing else.** With the shipped
  // numbers that is invisible -- a two-step fall is always far past the first
  // boundary's band anyway -- so it is measured with a band wide enough to
  // overlap the next threshold. 0x004db41b tests `raw == old + 1` exactly, and
  // a `raw > old` in its place answers 0 here.
  const BuildingStateRules wide{75, 50, 25, 30};
  CHECK(next_building_state(0, 48, 100, wide) == 2);
  // And the single step it does govern still holds, with the same numbers.
  CHECK(next_building_state(0, 60, 100, wide) == 0);
}

/// `IsBroken` is the stored tier and `IsVeryBroken` is an instantaneous ratio,
/// and **the two can disagree in both directions**, which is the finding.
TEST(sim_is_broken_and_is_very_broken_are_two_different_mechanisms) {
  PredicateBench b;
  const ObjectId fort = b.world.spawn(NativeClass::building, nullptr, b.classes.building);
  // `Fort` declares `maxhealth="5000"`.
  b.world.set_health(fort, 5000);
  CHECK(!b.predicate("IsBroken", PredicateBench::obj(fort)));
  CHECK(!b.predicate("IsVeryBroken", PredicateBench::obj(fort)));

  // 20% of 5000. Under tier 3's 25% threshold, above the ratio's 5%: broken
  // and not very broken.
  b.world.set_health(fort, 1000);
  CHECK(b.predicate("IsBroken", PredicateBench::obj(fort)));
  CHECK(!b.predicate("IsVeryBroken", PredicateBench::obj(fort)));

  // 4%. Both.
  b.world.set_health(fort, 200);
  CHECK(b.predicate("IsBroken", PredicateBench::obj(fort)));
  CHECK(b.predicate("IsVeryBroken", PredicateBench::obj(fort)));

  // 8%: the ratio has cleared, the tier has not -- 8 is under `25 + 3`, so the
  // one-step climb out of tier 3 is refused.
  b.world.set_health(fort, 400);
  CHECK(b.predicate("IsBroken", PredicateBench::obj(fort)));
  CHECK(!b.predicate("IsVeryBroken", PredicateBench::obj(fort)));

  // And at 30% the tier finally moves.
  b.world.set_health(fort, 1500);
  CHECK(!b.predicate("IsBroken", PredicateBench::obj(fort)));

  // **The divisor is twenty, not ten**, and it was ten here until
  // `Squad::InvadeThroughGate` turned up guarding on the identical five
  // instructions. `mov eax, 0x66666667; imul; sar edx, 3` is a divide by
  // twenty; `sar 2` would be the ten this used to say, and `sar 1` the five
  // the constant on its own suggests.
  b.world.set_health(fort, 400);  // 8%, clear of a twentieth
  CHECK(!b.predicate("IsVeryBroken", PredicateBench::obj(fort)));

  // The comparison is **strict**: exactly a twentieth is not very broken.
  b.world.set_health(fort, 250);
  CHECK(!b.predicate("IsVeryBroken", PredicateBench::obj(fort)));
  b.world.set_health(fort, 249);
  CHECK(b.predicate("IsVeryBroken", PredicateBench::obj(fort)));
}

/// A unit is not a building and never tiers, however hurt it is.
TEST(sim_only_a_building_carries_a_damage_tier) {
  PredicateBench b;
  const ObjectId soldier = b.world.spawn(NativeClass::unit, nullptr, b.classes.soldier);
  b.world.set_health(soldier, 1);  // `Soldier` declares maxhealth 80
  REQUIRE(b.world.find(soldier) != nullptr);
  CHECK(b.world.find(soldier)->state.damage_state == 0);
  CHECK(!b.predicate("IsBroken", PredicateBench::obj(soldier)));
  // The ratio is not gated on the building bit, and answers for anything with a
  // `maxhealth` -- which is what `GATE_PATROL.VS` needs from a gate.
  CHECK(b.predicate("IsVeryBroken", PredicateBench::obj(soldier)));
}

/// The two miss paths differ, and the difference is read off the two bodies.
TEST(sim_is_broken_answers_true_for_a_handle_it_cannot_resolve) {
  PredicateBench b;
  const script::Value nowhere = PredicateBench::obj(999999);
  // 0x004dd22f writes the literal 1 into the return slot on the miss path --
  // the opposite of every other predicate in the object model.
  CHECK(b.predicate("IsBroken", nowhere));
  // 0x00424fd9 writes the comparison's own result, through a null base; that is
  // a fault rather than a behaviour, so this one takes the ordinary miss.
  CHECK(!b.predicate("IsVeryBroken", nowhere));
}

/// Parry mode is gated on the `Parry` special **on both sides**.
TEST(sim_parry_mode_needs_the_parry_special) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
      R"(<class id="Shieldman" parent="Object" cpp_class="CVXUnit">
           <properties maxhealth="80" unit_specials="Parry, Sneak"/></class>)",
      R"(<class id="Archer" parent="Object" cpp_class="CVXUnit">
           <properties maxhealth="60" unit_specials="Sneak"/></class>)",
  };
  const char* names[] = {"object.sc.xml", "shieldman.sc.xml", "archer.sc.xml"};
  for (int i = 0; i < 3; ++i) graph.add(bytes_of(docs[i]), names[i]);
  graph.link();

  PredicateBench b;
  b.world.set_class_graph(&graph);
  const ObjectId shield = b.world.spawn(NativeClass::unit, nullptr, graph.find("Shieldman"));
  const ObjectId archer = b.world.spawn(NativeClass::unit, nullptr, graph.find("Archer"));

  const auto set = [&](ObjectId id, bool on) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "SetParryMode", 1,
               {PredicateBench::obj(id), script::Value::integer(on ? 1 : 0)});
    CHECK(out.status == script::HostStatus::ok);
  };
  const auto get = [&](ObjectId id) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "GetParryMode", 0, {PredicateBench::obj(id)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  CHECK(get(shield) == 0);
  set(shield, true);
  CHECK(get(shield) == 1);
  set(shield, false);
  CHECK(get(shield) == 0);

  // The archer's class does not offer `Parry`. The setter writes nothing.
  set(archer, true);
  CHECK(get(archer) == 0);
  REQUIRE(b.world.find(archer) != nullptr);
  CHECK(b.world.find(archer)->state.parry_mode == 0);

  // And the *getter* is gated too, independently: a field written behind the
  // setter's back still reads 0 on a unit that cannot parry.
  b.world.find(archer)->state.parry_mode = 1;
  CHECK(get(archer) == 0);
  // While the same write on a unit that can parry reads back.
  b.world.find(shield)->state.parry_mode = 1;
  CHECK(get(shield) == 1);

  // A handle that resolves to nothing answers 0 rather than refusing.
  CHECK(get(999999) == 0);
}

/// `GetSacrifice` answers the invalid handle, and `IsInvisibility` tests a
/// class name no shipped class has.
TEST(sim_the_sacrifice_pair_answers_what_the_shipped_data_can_produce) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
      R"(<class id="Invisibility" parent="Object" cpp_class="CVXSacrifice"/>)",
      // The name the predicate actually tests, which the installation does not
      // ship. Declared here so the *rule* is measured rather than the absence.
      R"(<class id="hIDe" parent="Object" cpp_class="CVXSacrifice"/>)",
  };
  const char* names[] = {"object.sc.xml", "invisibility.sc.xml", "hide.sc.xml"};
  for (int i = 0; i < 3; ++i) graph.add(bytes_of(docs[i]), names[i]);
  graph.link();

  PredicateBench b;
  b.world.set_class_graph(&graph);
  const ObjectId unit = b.world.spawn(NativeClass::unit, nullptr, graph.find("Object"));
  const ObjectId spell = b.world.spawn(NativeClass::sacrifice, nullptr, graph.find("Invisibility"));
  const ObjectId hide = b.world.spawn(NativeClass::sacrifice, nullptr, graph.find("hIDe"));

  const script::HostOutcome got =
      b.call(script::CallKind::member, "GetSacrifice", 0, {PredicateBench::obj(unit)});
  REQUIRE(got.status == script::HostStatus::ok);
  REQUIRE(got.value.is_object());
  CHECK(got.value.as_object().type == script::kNoType);

  // The comparison is case-insensitive -- the CRT's `_stricmp` -- so `hIDe`
  // hits. `Invisibility`, which is the class the effect factory really spawns,
  // does not.
  CHECK(b.predicate("IsInvisibility", PredicateBench::obj(hide)));
  CHECK(!b.predicate("IsInvisibility", PredicateBench::obj(spell)));
  CHECK(!b.predicate("IsInvisibility", PredicateBench::obj(unit)));
  CHECK(!b.predicate("IsInvisibility", PredicateBench::obj(999999)));

  // `AddDruid`: the half the corpus can reach is a receiver that is not a
  // sacrifice, which answers false without a trap -- the original's own
  // invalid-object path. A live sacrifice, which only this world can hold,
  // answers true for a druid that resolves and false for one that does not,
  // and stores nothing: `GetSacrifice` on the druid still answers the base.
  const auto add = [&](ObjectId receiver, ObjectId druid) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "AddDruid", 2,
               {PredicateBench::obj(receiver), PredicateBench::obj(druid),
                script::Value::integer(1200)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  };
  CHECK(!add(unit, unit));
  CHECK(!add(999999, unit));
  CHECK(add(spell, unit));
  CHECK(add(hide, unit));
  CHECK(!add(spell, 999999));
  const script::HostOutcome after =
      b.call(script::CallKind::member, "GetSacrifice", 0, {PredicateBench::obj(unit)});
  REQUIRE(after.value.is_object());
  CHECK(after.value.as_object().type == script::kNoType);
}

/// `.class` is the class's `id`, which is the string the shipped comparisons
/// are written against.
/// The stonehenge's dispatch pair. Nine sites and one, and **what they promise
/// is that the script gets past them**.
///
/// The five `*_DISPATCH.VS` scripts are one line each and the two research
/// scripts pair a start with a stop; nothing in all 885 files reads what either
/// writes, and the only registered reader has zero call sites. So there is no
/// state to assert, and what is asserted is what the entry points are for: they
/// are bound, they answer, they do not trap on any shape a script can produce,
/// and they move nothing.
TEST(sim_the_global_spell_pair_answers_and_changes_nothing) {
  PredicateBench b;
  const ObjectId hall = b.world.spawn(NativeClass::town_hall, nullptr, b.classes.building);
  const ObjectId soldier = b.world.spawn(NativeClass::unit, nullptr, b.classes.soldier);
  const std::uint64_t before = b.world.hashes().hash_of_hashes;

  const auto start = [&](script::Value receiver, std::int32_t spell, std::int32_t player) {
    return b.call(script::CallKind::member, "GlobalSpellStart", 2,
                  {receiver, script::Value::integer(spell), script::Value::integer(player)});
  };
  const auto stop = [&](script::Value receiver) {
    return b.call(script::CallKind::member, "GlobalSpellStop", 0, {receiver});
  };

  // The shipped shape: `gsTribute` is 5 and the player is 1-based.
  CHECK(start(PredicateBench::obj(hall), 5, 1).status == script::HostStatus::ok);
  CHECK(stop(PredicateBench::obj(hall)).status == script::HostStatus::ok);
  // A receiver that is not a town hall, one that does not resolve, a spell id
  // past the sixth and a player outside 1..16 -- all of them a return, never a
  // trap, because the original prints into a sink that is a bare `ret`.
  CHECK(start(PredicateBench::obj(soldier), 0, 1).status == script::HostStatus::ok);
  CHECK(start(PredicateBench::obj(999999), 0, 1).status == script::HostStatus::ok);
  CHECK(start(PredicateBench::obj(hall), 6, 1).status == script::HostStatus::ok);
  CHECK(start(PredicateBench::obj(hall), -1, 1).status == script::HostStatus::ok);
  CHECK(start(PredicateBench::obj(hall), 0, 0).status == script::HostStatus::ok);
  CHECK(start(PredicateBench::obj(hall), 0, 17).status == script::HostStatus::ok);
  CHECK(stop(PredicateBench::obj(soldier)).status == script::HostStatus::ok);
  CHECK(stop(PredicateBench::obj(999999)).status == script::HostStatus::ok);

  // Both are `void`, so a body that answered a value would be visible here --
  // and nothing in the world moved, which is the other half of what they claim.
  CHECK(!start(PredicateBench::obj(hall), 5, 1).value.is_integer());
  CHECK(!stop(PredicateBench::obj(hall)).value.is_integer());
  CHECK(b.world.hashes().hash_of_hashes == before);
  // The two are **one body**, and that is deliberate: with nothing stored they
  // differ in nothing, and a split that no fault can reach reads as a claim
  // this engine is not making. See `m_global_spell`.
}

TEST(sim_class_answers_the_class_id_and_the_empty_string_for_a_miss) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
      R"(<class id="BVikingLord" altid="Lord" parent="Object" cpp_class="CVXHero">
           <properties maxhealth="900"/></class>)",
  };
  const char* names[] = {"object.sc.xml", "lord.sc.xml"};
  for (int i = 0; i < 2; ++i) graph.add(bytes_of(docs[i]), names[i]);
  graph.link();

  PredicateBench b;
  b.world.set_class_graph(&graph);
  const ObjectId lord = b.world.spawn(NativeClass::hero, nullptr, graph.find("BVikingLord"));
  const ObjectId nameless = b.world.spawn(NativeClass::unit, nullptr);

  const auto class_of = [&](ObjectId id) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "class", 0, {PredicateBench::obj(id)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_string() ? out.value.as_string() : std::string("<not a string>");
  };

  // `EVALRECRUIT.VS` is `if (sq.Leader.class == "BVikingLord")`, so it is the
  // `id` and not the `cpp_class` (`CVXHero`) or the `altid` (`Lord`).
  CHECK(class_of(lord) == "BVikingLord");
  CHECK(class_of(lord) != "CVXHero");
  CHECK(class_of(lord) != "Lord");
  // An object with no class, and a handle that resolves to nothing, both answer
  // the empty string rather than refusing.
  CHECK(class_of(nameless).empty());
  CHECK(class_of(999999).empty());
}

/// `SetEntering(bool)` is one bit, it round-trips through a save, and it is
/// **not** any of the bits beside it.
TEST(sim_set_entering_writes_one_bit_of_the_second_flag_word) {
  PredicateBench b;
  const ObjectId u = b.world.spawn(NativeClass::unit, nullptr, b.classes.soldier);
  REQUIRE(b.world.find(u) != nullptr);
  CHECK(!b.world.find(u)->state.flags.entering);

  const auto set = [&](bool on) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "SetEntering", 1,
               {PredicateBench::obj(u), script::Value::integer(on ? 1 : 0)});
    CHECK(out.status == script::HostStatus::ok);
  };

  set(true);
  CHECK(b.world.find(u)->state.flags.entering);
  // And nothing else on the word moved. `no_ai`, `noselect`, `in_air`,
  // `autocast` and `cursed` all live on `[obj+0x194]` beside it, and a mask
  // that was one bit out would take one of them with it.
  CHECK(!b.world.find(u)->state.flags.no_ai);
  CHECK(!b.world.find(u)->state.flags.noselect);
  CHECK(!b.world.find(u)->state.flags.in_air);
  CHECK(!b.world.find(u)->state.flags.autocast);
  CHECK(!b.world.find(u)->state.flags.cursed);
  CHECK(!b.world.find(u)->state.flags.on_minimap);

  set(false);
  CHECK(!b.world.find(u)->state.flags.entering);

  // A handle that resolves to nothing writes nothing and does not trap.
  const script::HostOutcome miss =
      b.call(script::CallKind::member, "SetEntering", 1,
             {PredicateBench::obj(999999), script::Value::integer(1)});
  CHECK(miss.status == script::HostStatus::ok);
}

TEST(sim_as_tower_is_as_building_under_another_name) {
  PredicateBench b;
  const ObjectId fort = b.world.spawn(NativeClass::building, nullptr, b.classes.building);
  const ObjectId soldier = b.world.spawn(NativeClass::unit, nullptr, b.classes.soldier);

  const auto cast = [&](const char* name, ObjectId id) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, name, 0, {PredicateBench::obj(id)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value;
  };

  // Both names, both receivers, and the two answers are identical -- which is
  // the whole assertion. A tower-specific test would make the first line pass
  // and the second fail.
  CHECK(cast("AsTower", fort) == cast("AsBuilding", fort));
  CHECK(cast("AsTower", soldier) == cast("AsBuilding", soldier));

  // And they are the answers they should be: the building casts to itself, the
  // soldier casts to the invalid object.
  REQUIRE(cast("AsTower", fort).is_object());
  CHECK(cast("AsTower", fort).as_object().id == fort);
  CHECK(cast("AsTower", soldier).as_object().type == script::kNoType);
}

TEST(sim_is_in_air_and_is_building_are_one_bit_each) {
  PredicateBench b;
  const ObjectId crow = b.world.spawn(NativeClass::flying_unit, nullptr, b.classes.unit);
  const ObjectId ship = b.world.spawn_ship(nullptr, b.classes.unit).ship;

  // Both start false, which is what a map-placed crow and a map-placed ship
  // are: on the ground, and finished.
  CHECK(!b.predicate("IsInAir", PredicateBench::obj(crow)));
  CHECK(!b.predicate("IsBuilding", PredicateBench::obj(ship)));

  b.world.mutable_state(crow)->flags.in_air = true;
  b.world.mutable_state(ship)->flags.building = true;
  CHECK(b.predicate("IsInAir", PredicateBench::obj(crow)));
  CHECK(b.predicate("IsBuilding", PredicateBench::obj(ship)));

  // The two bits are independent of each other and of the Building class bit,
  // whose mask is the same 0x00800000 on the other word.
  CHECK(!b.predicate("IsInAir", PredicateBench::obj(ship)));
  const ObjectId fort = b.world.spawn(NativeClass::building, nullptr, b.classes.building);
  b.world.mutable_state(fort)->flags.is_building = true;
  CHECK(!b.predicate("IsBuilding", PredicateBench::obj(fort)));

  // And a receiver that resolves to nothing is an answer, not a refusal.
  for (const char* name : {"IsInAir", "IsBuilding"}) {
    CHECK(!b.predicate(name, PredicateBench::obj(9999)));
    CHECK(!b.predicate(name, script::Value::integer(3)));
  }
}

/// `Curse` and `IsCursed` are a matched pair on one bit, and `SHAMAN_IDLE.VS`
/// is the whole corpus for both -- it reads its own write to decide whether to
/// curse a target again.
///
/// See `ObjectFlags::cursed` for what is *not* established: the original's
/// `Curse` hands off to the spell machinery and no write to bit 27 of
/// `[obj+0x194]` was found anywhere in `.text`.
TEST(sim_curse_and_is_cursed_round_trip_one_bit) {
  PredicateBench b;
  const ObjectId target = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  CHECK(!b.predicate("IsCursed", PredicateBench::obj(target)));

  b.call(script::CallKind::member, "Curse", 0, {PredicateBench::obj(target)});
  CHECK(b.predicate("IsCursed", PredicateBench::obj(target)));
  CHECK(b.world.find(target)->state.flags.cursed);

  // Cursing twice is not a toggle -- the shipped guard reads the bit precisely
  // so that a shaman does not repeat itself, and a toggle would defeat it.
  b.call(script::CallKind::member, "Curse", 0, {PredicateBench::obj(target)});
  CHECK(b.predicate("IsCursed", PredicateBench::obj(target)));

  // Another object is untouched: the bit is per-object, not global.
  const ObjectId bystander = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  CHECK(!b.predicate("IsCursed", PredicateBench::obj(bystander)));

  // An unresolvable receiver answers false rather than trapping, which is the
  // branch the shaman wants for a target that has stopped existing.
  CHECK(b.world.despawn(target));
  CHECK(!b.predicate("IsCursed", PredicateBench::obj(target)));
}

/// `SetLastAttackTime` stores nothing, and that is the finding rather than an
/// omission: its field has one reader in the whole executable (`Unit::Dump`)
/// and no host getter, so nothing a script can do could observe it. See the
/// body for the stealth timer the shipped sites are actually reaching for.
TEST(sim_set_last_attack_time_resolves_its_receiver_and_stores_nothing) {
  PredicateBench b;
  const ObjectId unit = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const std::uint64_t before = b.world.state_hash();
  const script::HostOutcome out =
      b.call(script::CallKind::member, "SetLastAttackTime", 0, {PredicateBench::obj(unit)});
  CHECK(out.status == script::HostStatus::ok);
  // Nothing hashed moved, which is the whole claim.
  CHECK(b.world.state_hash() == before);

  // And it does not trap on a receiver that no longer resolves.
  CHECK(b.world.despawn(unit));
  CHECK(b.call(script::CallKind::member, "SetLastAttackTime", 0, {PredicateBench::obj(unit)})
            .status == script::HostStatus::ok);
}


/// `InShip` asks about the **holder**, not about the object.
///
/// `0x005d70f0` resolves the unit's holder and asks that holder record for its
/// *ship* slot; `Unit::GetHolderSett` does the identical thing with the slot
/// beside it. So a holder is a settlement's or a ship's, and this is how a
/// script tells them apart -- six of the nine shipped sites are one idiom,
/// `if (!.InShip && .InHolder) bldEnter = .GetHolderSett.GetCentralBuilding;`,
/// which is exactly "in a holder that is not a ship, therefore it has a
/// settlement".
TEST(sim_in_ship_asks_what_kind_of_holder_the_unit_is_in) {
  PredicateBench b;
  const World::ShipIds ship = b.world.spawn_ship(nullptr, b.classes.unit);
  const World::SettlementIds town = b.world.spawn_settlement(1);

  const ObjectId sailor = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId guard = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId walker = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  REQUIRE(b.world.put_in_holder(sailor, ship.holder));
  REQUIRE(b.world.put_in_holder(guard, town.holder));

  CHECK(b.predicate("InShip", PredicateBench::obj(sailor)));
  // In a holder, and not a ship's: the other half of the shipped idiom.
  CHECK(!b.predicate("InShip", PredicateBench::obj(guard)));
  CHECK(b.predicate("InHolder", PredicateBench::obj(guard)));
  // In no holder at all.
  CHECK(!b.predicate("InShip", PredicateBench::obj(walker)));
  CHECK(!b.predicate("InHolder", PredicateBench::obj(walker)));

  // The original dereferences null here -- it is the one member of its family
  // that skips the validity check. That is a fault, not a behaviour.
  CHECK(!b.predicate("InShip", PredicateBench::obj(9999)));
  CHECK(!b.predicate("InShip", script::Value::string("no")));
}

/// `SameHolderAs` compares the field, so two units outside every holder are in
/// the same one.
///
/// `gbr.exe` 0x005d8120 is `a->holder == b->holder` on the raw 16-bit field,
/// and "no holder" is `0xffff` on both sides. Both shipped sites guard with
/// `InHolder` first, so the retail data never sees it -- which is exactly why
/// it is worth pinning rather than tidying.
TEST(sim_same_holder_as_compares_the_field_and_not_the_holders_existence) {
  PredicateBench b;
  const World::ShipIds ship = b.world.spawn_ship(nullptr, b.classes.unit);
  const World::SettlementIds town = b.world.spawn_settlement(1);
  const ObjectId a = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId c = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId d = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId loose1 = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId loose2 = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  REQUIRE(b.world.put_in_holder(a, ship.holder));
  REQUIRE(b.world.put_in_holder(c, ship.holder));
  REQUIRE(b.world.put_in_holder(d, town.holder));

  const auto same = [&](ObjectId x, ObjectId y) {
    return b.call(script::CallKind::member, "SameHolderAs", 1,
                  {PredicateBench::obj(x), PredicateBench::obj(y)});
  };
  CHECK(same(a, c).value.truthy_scalar());
  CHECK(!same(a, d).value.truthy_scalar());
  CHECK(same(a, a).value.truthy_scalar());
  // Two units standing in the open. `0xffff == 0xffff`, and the answer is yes.
  CHECK(same(loose1, loose2).value.truthy_scalar());
  CHECK(!same(a, loose1).value.truthy_scalar());

  // A handle that resolves to nothing answers false rather than joining the
  // units in the open: `kNoObject` stands in for `0xffff` on the field, so an
  // unresolvable handle and a unit outside a holder would otherwise be the
  // same value read two ways.
  CHECK(same(a, static_cast<ObjectId>(9999)).status == script::HostStatus::ok);
  CHECK(!same(loose1, static_cast<ObjectId>(9999)).value.truthy_scalar());
  CHECK(!same(static_cast<ObjectId>(9999), static_cast<ObjectId>(9998))
             .value.truthy_scalar());
}

/// `SetNoselectFlag(true)` writes a bit **and** drops the object out of every
/// selection -- but only on the transition.
///
/// `0x005d8750` tests the bit before it writes and calls the deselect path only
/// when it was clear, so setting a flag that is already set changes nothing.
/// All six shipped sites pass `true`, on ambient props -- hens, fish, the
/// settlement's wandering villagers -- always beside `SetMinimapFlag(true)`.
/// The `false` path is never exercised by shipped content and is transcribed
/// anyway: it clears the bit and deselects nothing.
TEST(sim_set_noselect_flag_deselects_only_on_the_transition) {
  PredicateBench b;
  const ObjectId hen = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  b.world.mutable_state(hen)->flags.is_unit = true;
  REQUIRE(b.selections.select(1, hen));
  REQUIRE(b.selections.player(1).contains(hen));

  const auto set = [&](bool value) {
    return b.call(script::CallKind::member, "SetNoselectFlag", 1,
                  {PredicateBench::obj(hen), script::Value::boolean(value)});
  };

  CHECK(set(true).status == script::HostStatus::ok);
  CHECK(b.world.state(hen)->flags.noselect);
  CHECK(!b.selections.player(1).contains(hen));

  // Selected again, then flagged again: the bit is already set, so the deselect
  // path is not reached and the selection survives.
  REQUIRE(b.selections.select(1, hen));
  CHECK(set(true).status == script::HostStatus::ok);
  CHECK(b.selections.player(1).contains(hen));

  // Clearing writes the bit and deselects nothing.
  CHECK(set(false).status == script::HostStatus::ok);
  CHECK(!b.world.state(hen)->flags.noselect);
  CHECK(b.selections.player(1).contains(hen));

  // An unresolvable receiver writes nothing and says nothing.
  const script::HostOutcome miss =
      b.call(script::CallKind::member, "SetNoselectFlag", 1,
             {PredicateBench::obj(9999), script::Value::boolean(true)});
  CHECK(miss.status == script::HostStatus::ok);
}

/// `abs(INT32_MIN)` is `INT32_MIN`.
///
/// `0x00694be0` is a `test` and a `neg`. `neg 0x80000000` is `0x80000000`; the
/// overflow flag is set and ignored. Reproduced explicitly rather than left to
/// a language that might trap on it or promote its way out.
/// `EnterHolder` resolves a holder three ways and refuses a held receiver;
/// `ExitHolder` is the unboarding routine's kernel.
TEST(sim_enter_holder_joins_a_ship_a_settlement_or_a_units_own_holder) {
  PredicateBench b;
  const script::CallKind member = script::CallKind::member;
  const World::ShipIds ship = b.world.spawn_ship(nullptr, b.classes.unit);
  const World::SettlementIds town = b.world.spawn_settlement(1);
  const ObjectId sailor = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId guard = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId fort = b.world.spawn(NativeClass::building, nullptr, b.classes.graph.find("Fort"));
  const ObjectId a = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId c = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId d = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId e = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  REQUIRE(b.world.put_in_holder(guard, town.holder));

  // A ship: its own holder.
  CHECK(b.call(member, "EnterHolder", 1, {PredicateBench::obj(a), PredicateBench::obj(ship.ship)})
            .status == script::HostStatus::ok);
  CHECK(b.world.find(a)->state.holder == ship.holder);
  // A held unit: the holder it is in.
  b.call(member, "EnterHolder", 1, {PredicateBench::obj(c), PredicateBench::obj(guard)});
  CHECK(b.world.find(c)->state.holder == town.holder);
  // A building: its settlement's holder, through the economy's record.
  b.world.find(fort)->settlement = town.settlement;
  SettlementInit init;
  init.settlement_object = town.settlement;
  init.holder_object = town.holder;
  init.anchor = fort;
  init.owner = 1;
  init.kind = SettlementKind::village;
  init.max_units = 10;
  (void)b.economy.settlements().create(init);
  b.call(member, "EnterHolder", 1, {PredicateBench::obj(d), PredicateBench::obj(fort)});
  CHECK(b.world.find(d)->state.holder == town.holder);
  // A free-standing unit carries no holder and is in none: nothing happens.
  b.call(member, "EnterHolder", 1, {PredicateBench::obj(e), PredicateBench::obj(sailor)});
  CHECK(!b.world.find(e)->state.is_held());
  // A receiver already held stays where it is.
  b.call(member, "EnterHolder", 1, {PredicateBench::obj(a), PredicateBench::obj(guard)});
  CHECK(b.world.find(a)->state.holder == ship.holder);

  // ExitHolder: out, at the point; a unit not held is left alone.
  b.call(member, "ExitHolder", 1, {PredicateBench::obj(a), pack_point(Point{70, 80})});
  CHECK(!b.world.find(a)->state.is_held());
  CHECK((b.world.resolve_position(a) == Point{70, 80}));
  b.world.set_position(e, Point{5, 5});
  b.call(member, "ExitHolder", 1, {PredicateBench::obj(e), pack_point(Point{70, 80})});
  CHECK((b.world.resolve_position(e) == Point{5, 5}));
  (void)sailor;
}

/// `SetName` binds, and rebinds a name already taken; an empty name is
/// nothing.
TEST(sim_set_name_binds_and_moves_a_taken_name) {
  PredicateBench b;
  const ObjectId x = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId y = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  b.call(script::CallKind::member, "SetName", 1,
         {PredicateBench::obj(x), script::Value::string("NO_Scipio")});
  CHECK(b.world.named_objects().object("NO_Scipio") == x);
  b.call(script::CallKind::member, "SetName", 1,
         {PredicateBench::obj(y), script::Value::string("NO_Scipio")});
  CHECK(b.world.named_objects().object("NO_Scipio") == y);
  const std::size_t names = b.world.named_objects().size();
  b.call(script::CallKind::member, "SetName", 1, {PredicateBench::obj(x), script::Value::string("")});
  CHECK(b.world.named_objects().size() == names);
}

/// `Disappear` takes the unit off the map; `Disease` and `IsDiseased` are one
/// bit; the store bin is accepted and dropped.
TEST(sim_disappear_disease_and_the_store_bin) {
  PredicateBench b;
  const script::CallKind member = script::CallKind::member;
  const ObjectId u = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  b.world.set_position(u, Point{300, 300});
  b.world.mutable_state(u)->flags.has_active_path = true;
  CHECK(b.call(member, "Disappear", 0, {PredicateBench::obj(u)}).status == script::HostStatus::ok);
  CHECK((b.world.resolve_position(u) == kHeldPosition));
  CHECK(!b.world.find(u)->state.flags.has_active_path);
  CHECK(!b.world.find(u)->state.is_held());
  // And back, the way `UNIT_DISAPPEAR.VS` puts it back.
  CHECK(b.world.set_position(u, Point{300, 300}));

  CHECK(!b.predicate("IsDiseased", PredicateBench::obj(u)));
  CHECK(b.call(member, "Disease", 0, {PredicateBench::obj(u)}).status == script::HostStatus::ok);
  CHECK(b.predicate("IsDiseased", PredicateBench::obj(u)));
  CHECK(b.world.find(u)->state.flags.diseased);
  // Nothing a script can reach clears it.
  CHECK(b.call(member, "Disease", 0, {PredicateBench::obj(u)}).status == script::HostStatus::ok);
  CHECK(b.predicate("IsDiseased", PredicateBench::obj(u)));

  const std::uint64_t before = b.world.state_hash();
  CHECK(b.call(member, "AddToStoreBin", 0, {PredicateBench::obj(u)}).status ==
        script::HostStatus::ok);
  CHECK(b.call(member, "RemoveFromStoreBin", 0, {PredicateBench::obj(u)}).status ==
        script::HostStatus::ok);
  CHECK(b.world.state_hash() == before);
}

TEST(sim_abs_wraps_on_the_most_negative_integer) {
  PredicateBench b;
  const auto of = [&](std::int32_t v) {
    const script::HostOutcome out = b.call(script::CallKind::free_function, "abs", 1,
                                           {script::Value::integer(v)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : 0;
  };
  CHECK(of(7) == 7);
  CHECK(of(-7) == 7);
  CHECK(of(0) == 0);
  CHECK(of(2147483647) == 2147483647);
  CHECK(of(-2147483647) == 2147483647);
  CHECK(of(-2147483647 - 1) == -2147483647 - 1);
}

/// `InHolder(str)` is a **count**, and the arity is the only thing that says so.
///
/// `gbr.exe` registers four `*InHolder` methods and no two are the same
/// function. The arity-0 form is `Unit::InHolder`, a predicate over 154 sites.
/// The arity-1 form lives on `ObjList` and `Query`, takes a settlement name,
/// and returns `retType 1` -- an int. Both shipped sites compare it against
/// `.count`:
///
///     while (T_UnitedArmy.InHolder("S_Pelusio") < T_UnitedArmy.count)
///
/// A bool would make that spin forever.
TEST(sim_in_holder_with_a_name_counts_rather_than_answering_yes) {
  PredicateBench b;
  const World::SettlementIds town = b.world.spawn_settlement(1);
  SettlementInit init;
  init.settlement_object = town.settlement;
  init.holder_object = town.holder;
  init.warehouse_object = town.warehouse;
  init.name = "S_Pelusio";
  init.max_units = 10000;
  (void)b.economy.create(b.world, init);

  std::vector<ObjectId> army;
  for (int i = 0; i < 4; ++i) {
    const ObjectId unit = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
    b.world.mutable_state(unit)->flags.is_unit = true;
    army.push_back(unit);
  }
  // A building in the list, which the original casts away before it compares.
  const ObjectId shed = b.world.spawn(NativeClass::building, nullptr, b.classes.building);
  b.world.mutable_state(shed)->flags.is_building = true;
  REQUIRE(b.world.put_in_holder(shed, town.holder));
  army.push_back(shed);

  REQUIRE(b.world.put_in_holder(army[0], town.holder));
  REQUIRE(b.world.put_in_holder(army[1], town.holder));

  ObjListPool& pool = objlist_pool_of(b.world);
  const ObjListId list = pool.acquire(7, 0);
  REQUIRE(list != kNoObjList);
  *pool.mutable_items(list) = army;

  const auto count = [&](const char* name) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "InHolder", 1,
               {make_objlist_value(list), script::Value::string(name)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  // Two of the five members are units inside that holder. The building is not
  // counted even though it is in the same holder.
  CHECK(count("S_Pelusio") == 2);

  REQUIRE(b.world.put_in_holder(army[2], town.holder));
  CHECK(count("S_Pelusio") == 3);

  // A name no settlement carries is zero, not a refusal: the original formats a
  // "Check the spelling" complaint into a sink that is a bare `ret` and pushes
  // zero.
  CHECK(count("S_Nowhere") == 0);
  CHECK(count("") == 0);
}

// ==========================================================================
// specials, the minimap flag, and the two searches
// ==========================================================================

namespace {

/// Classes that declare `unit_specials`, in the shipped spelling: display names
/// with spaces and capitals, comma-separated, where the script-side constants
/// are lowercase identifiers with underscores.
struct SpecialsGraph {
  ClassGraph graph;
  ClassIndex crow = kNoClass;
  ClassIndex veteran = kNoClass;
  ClassIndex plain = kNoClass;
  ClassIndex sentry = kNoClass;
  ClassIndex ram = kNoClass;
  ClassIndex peaceful = kNoClass;
  ClassIndex peasant = kNoClass;
  ClassIndex animal = kNoClass;
  ClassIndex lion = kNoClass;
  ClassIndex bowman = kNoClass;
  ClassIndex veteran_bowman = kNoClass;
  ClassIndex broken_bowman = kNoClass;

  SpecialsGraph() {
    const std::string docs[] = {
        R"(<class id="Unit" cpp_class="CVXUnit"><properties sight="900"/></class>)",
        R"(<class id="Crow" parent="Unit" cpp_class="CVXFlying"><properties sight="900"/></class>)",
        // `Revitalize` is in the corpus and **not** in the executable's 36-name
        // table; a token that resolves to nothing has to be skipped in silence.
        R"(<class id="Veteran" parent="Unit" cpp_class="CVXUnit"><properties
             unit_specials="Rage, Defense skill, Revitalize"/></class>)",
        R"(<class id="Plain" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="Sentry" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="RamUnit" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="Peaceful" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="Peasant" parent="Peaceful" cpp_class="CVXUnit"/>)",
        // The animal half of `Obj::IsPeaceful`, and the shipped tree's own
        // shape: `Animal` hangs off `BaseAnimal`, and so do the lions, the
        // attack animals and the summoned creatures -- which therefore are
        // *not* `Animal` and *not* peaceful.
        R"(<class id="BaseAnimal" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="Animal" parent="BaseAnimal" cpp_class="CVXUnit"/>)",
        R"(<class id="Deer" parent="Animal" cpp_class="CVXUnit"/>)",
        R"(<class id="LionM" parent="BaseAnimal" cpp_class="CVXUnit"/>)",
        // `Obj::IsRanged`: the class names something to throw, or it does not.
        R"(<class id="Bowman" parent="Unit" cpp_class="CVXUnit"><properties
             projectile_class="Arrow"/></class>)",
        R"(<class id="VeteranBowman" parent="Bowman" cpp_class="CVXUnit"/>)",
        // The original's uninitialised-string default, which no shipped class
        // declares and which reads as "no projectile" all the same.
        R"(<class id="BrokenBowman" parent="Unit" cpp_class="CVXUnit"><properties
             projectile_class="**Invalid**"/></class>)",
    };
    const char* names[] = {"unit.sc.xml",     "crow.sc.xml",   "veteran.sc.xml",
                           "plain.sc.xml",    "sentry.sc.xml", "ram.sc.xml",
                           "peaceful.sc.xml", "peasant.sc.xml", "baseanimal.sc.xml",
                           "animal.sc.xml",   "deer.sc.xml",   "lionm.sc.xml",
                           "bowman.sc.xml",   "vbowman.sc.xml", "bbowman.sc.xml"};
    for (int i = 0; i < 15; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    crow = graph.find("Crow");
    veteran = graph.find("Veteran");
    plain = graph.find("Plain");
    sentry = graph.find("Sentry");
    ram = graph.find("RamUnit");
    peaceful = graph.find("Peaceful");
    peasant = graph.find("Peasant");
    animal = graph.find("Deer");
    lion = graph.find("LionM");
    bowman = graph.find("Bowman");
    veteran_bowman = graph.find("VeteranBowman");
    broken_bowman = graph.find("BrokenBowman");
  }
};

struct SpecialsBench {
  SpecialsGraph classes;
  World world;
  HeroSystem heroes;
  script::HostRegistry registry;
  HostContext context;

  SpecialsBench() {
    world.set_class_graph(&classes.graph);
    REQUIRE(world.add_system(&heroes));
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  ObjectId unit(ClassIndex which, Point at, std::int32_t experience = 0) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, which);
    world.set_health(id, 100);
    (void)world.set_position(id, at);
    heroes.register_unit(world, id);
    heroes.set_experience(id, experience);
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

  bool has(ObjectId id, std::int32_t special) {
    const script::HostOutcome out =
        call("HasSpecial", 1, {script::Value::object(kTypeObj, id),
                               script::Value::integer(special)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }
  ObjectId found(const char* name, ObjectId self) {
    const script::HostOutcome out =
        call(name, 0, {script::Value::object(kTypeObj, self)});
    CHECK(out.status == script::HostStatus::ok);
    if (!out.value.is_object() || out.value.as_object().type == script::kNoType) {
      return kNoObject;
    }
    return out.value.as_object().id;
  }
};

/// The script-side special constants, from `imcheck globals`. They are indices
/// into the executable's 36-entry name table and the class property's tokens are
/// that table's display names -- one table seen from two sides.
constexpr std::int32_t kRage = 22;
constexpr std::int32_t kDefenseSkill = 16;
constexpr std::int32_t kSneak = 26;

}  // namespace

/// `HasSpecial(n)` reads the receiver's class `unit_specials` list, and the
/// script's constant and the property's token are the same table.
///
/// `curse` is 35 in `sim/globals.cpp` and `Curse` is entry 35 in the
/// executable's table; lowercasing and turning spaces into underscores is the
/// whole of the difference between the two spellings. A token that resolves to
/// nothing is skipped in silence, which is what the reader must do with
/// `Revitalize` -- the one special the corpus names that the executable's table
/// does not have, and which `sim/globals.cpp` already refuses by name.
TEST(sim_has_special_reads_the_classes_unit_specials_list) {
  SpecialsBench b;
  const ObjectId veteran = b.unit(b.classes.veteran, Point{0, 0});
  const ObjectId plain = b.unit(b.classes.plain, Point{0, 0});

  CHECK(b.has(veteran, kRage));
  CHECK(b.has(veteran, kDefenseSkill));
  CHECK(!b.has(veteran, kSneak));
  // A class that declares none has none, rather than answering for its parent's
  // or for nothing at all.
  CHECK(!b.has(plain, kRage));

  // `0x005d8d70` refuses an index of 36 or more *before* it shifts, and a
  // negative one is refused here rather than reproducing a shift by a negative
  // count.
  CHECK(!b.has(veteran, 36));
  CHECK(!b.has(veteran, 1000));
  CHECK(!b.has(veteran, -1));

  // An unresolvable receiver is false, not a refusal.
  const script::HostOutcome miss =
      b.call("HasSpecial", 1, {script::Value::object(kTypeObj, 9999),
                               script::Value::integer(kRage)});
  CHECK(miss.status == script::HostStatus::ok);
  CHECK(miss.value.as_integer() == 0);
}

/// `SetMinimapFlag` is one bit, and it is `SetNoselectFlag`'s neighbour.
/// `Obj::IsPeaceful` is two `IsHeirOf` calls and nothing else.
///
/// `gbr.exe` 0x005402f0 is `IsHeirOf("Peaceful") || IsHeirOf("Animal")`, both
/// looked up as class *ids* and both walked up the parent chain. Every class in
/// this graph is `cpp_class="CVXUnit"`, exactly as the shipped ones are, so a
/// C++-class reading would answer yes for all of them and the plain unit's
/// `false` is what separates the two readings.
///
/// The lion is the case worth having a test for. `ANIMAL.SC.XML` hangs `Animal`
/// off `BaseAnimal`, and `LIONM`, `LIONF`, `ATTACKANIMAL` and `SUMMONINGUNIT`
/// hang off `BaseAnimal` *beside* it -- so a predicate written against the
/// obvious-looking root would call a lion peaceful, and the executable's does
/// not.
/// `Obj::IsRanged` asks whether the class names a projectile, and nothing else.
///
/// `gbr.exe` 0x0053f5b0 is two tests on the class descriptor's `std::string` at
/// `+0x960`: non-empty (its length slot at `+0x974`), and not equal to
/// `"**Invalid**"` -- the original's uninitialised-string default, an 11-byte
/// literal at 0x007ae000. The property is `projectile_class`, which
/// `CombatProfile::projectile` already resolves from the other side.
///
/// So it is not a range comparison, and it inherits: a class that declares
/// nothing but descends from one that does is ranged too.
TEST(sim_is_ranged_is_the_projectile_class_property_and_not_a_range) {
  SpecialsBench b;
  const auto ranged = [&](ClassIndex which) {
    const script::HostOutcome out =
        b.call("IsRanged", 0, {script::Value::object(kTypeObj, b.unit(which, Point{0, 0}))});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.truthy_scalar();
  };

  CHECK(ranged(b.classes.bowman));
  CHECK(ranged(b.classes.veteran_bowman));  // inherited, not declared
  CHECK(!ranged(b.classes.plain));
  // The original's own "unset" spelling, which no shipped class declares and
  // which the executable tests for by name.
  CHECK(!ranged(b.classes.broken_bowman));

  // An invalid receiver reports and answers false; it does not refuse.
  const script::HostOutcome nobody =
      b.call("IsRanged", 0, {script::Value::object(kTypeObj, kNoObject)});
  CHECK(nobody.status == script::HostStatus::ok);
  CHECK(!nobody.value.truthy_scalar());
}

TEST(sim_is_peaceful_is_the_peaceful_and_animal_subtrees_and_not_their_root) {
  SpecialsBench b;
  const auto peaceful = [&](ObjectId id) {
    const script::HostOutcome out =
        b.call("IsPeaceful", 0, {script::Value::object(kTypeObj, id)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.truthy_scalar();
  };

  CHECK(peaceful(b.unit(b.classes.peasant, Point{0, 0})));
  CHECK(peaceful(b.unit(b.classes.peaceful, Point{0, 0})));
  CHECK(peaceful(b.unit(b.classes.animal, Point{0, 0})));

  // Under `BaseAnimal`, not under `Animal`.
  CHECK(!peaceful(b.unit(b.classes.lion, Point{0, 0})));
  // Under `Unit` and nothing else -- and `cpp_class="CVXUnit"` like the rest.
  CHECK(!peaceful(b.unit(b.classes.plain, Point{0, 0})));

  // Registered on `Obj`, so the question is asked of the class and not of what
  // the object happens to be: a *building* of a peaceful class answers yes.
  const ObjectId shed = b.world.spawn(NativeClass::building, nullptr, b.classes.peasant);
  CHECK(peaceful(shed));

  // An invalid receiver reports and answers false; it does not refuse.
  CHECK(!peaceful(kNoObject));
  CHECK(!peaceful(static_cast<ObjectId>(9999)));
  const script::HostOutcome not_an_object =
      b.call("IsPeaceful", 0, {script::Value::integer(3)});
  CHECK(not_an_object.status == script::HostStatus::ok);
  CHECK(!not_an_object.value.truthy_scalar());
}

TEST(sim_set_minimap_flag_is_one_bit_on_the_second_flag_word) {
  SpecialsBench b;
  const ObjectId hen = b.unit(b.classes.plain, Point{0, 0});
  CHECK(!b.world.state(hen)->flags.on_minimap);
  CHECK(b.call("SetMinimapFlag", 1, {script::Value::object(kTypeObj, hen),
                                     script::Value::boolean(true)})
            .status == script::HostStatus::ok);
  CHECK(b.world.state(hen)->flags.on_minimap);
  // And it does not disturb the flag every shipped call site sets beside it.
  CHECK(!b.world.state(hen)->flags.noselect);

  CHECK(b.call("SetMinimapFlag", 1, {script::Value::object(kTypeObj, hen),
                                     script::Value::boolean(false)})
            .status == script::HostStatus::ok);
  CHECK(!b.world.state(hen)->flags.on_minimap);
  CHECK(b.call("SetMinimapFlag", 1, {script::Value::object(kTypeObj, 9999),
                                     script::Value::boolean(true)})
            .status == script::HostStatus::ok);
}

/// `FindNearBird` filters on **nothing but the class and the distance**.
///
/// The scan mask is `0xffffffff`, so no category filter runs, and the visitor
/// tests only that the class pointer matches and the object is not the receiver.
/// No aliveness test, no visibility test, no diplomacy test -- a crow will fly
/// toward a dead crow, and that is transcribed rather than tidied.
TEST(sim_find_near_bird_tests_only_the_class_and_the_radius) {
  SpecialsBench b;
  const ObjectId self = b.unit(b.classes.crow, Point{1000, 1000});
  // Another class inside the radius, which must not match.
  (void)b.unit(b.classes.plain, Point{1010, 1000});
  CHECK(b.found("FindNearBird", self) == kNoObject);

  // A crow just outside 2,048 units.
  const ObjectId far = b.unit(b.classes.crow, Point{1000, 1000 + 2049});
  CHECK(b.found("FindNearBird", self) == kNoObject);
  (void)far;

  // And one just inside, which is dead -- and is still the answer.
  const ObjectId near = b.unit(b.classes.crow, Point{1000, 1000 + 2048});
  b.world.set_health(near, 0);
  CHECK(b.found("FindNearBird", self) == near);

  // Never itself.
  CHECK(b.found("FindNearBird", near) == self);
  CHECK(b.found("FindNearBird", 9999) == kNoObject);
}

/// `FindUnitToLearn` wants **strictly** more experience, and skips four kinds of
/// unit that the executable names one at a time.
///
/// The predicate is `0x00511ff0`, and the three class exclusions are lazily
/// resolved by name: `Sentry` (`0x007ad494`), `RamUnit` (`0x007c2410`) and
/// `Peaceful` (`0x007c23fc`). Add the dead, the garrisoned, ships and the druid
/// itself and that is the whole of it.
TEST(sim_find_unit_to_learn_wants_strictly_more_experience) {
  SpecialsBench b;
  const ObjectId druid = b.unit(b.classes.plain, Point{0, 0}, /*experience=*/50);

  // Equal experience is not more: the script's own guard is
  // `if (tgt.experience <= .experience) break;`.
  (void)b.unit(b.classes.plain, Point{100, 0}, 50);
  (void)b.unit(b.classes.plain, Point{100, 0}, 10);
  CHECK(b.found("FindUnitToLearn", druid) == kNoObject);

  // Each of the three named classes is skipped even when it qualifies on
  // experience.
  (void)b.unit(b.classes.sentry, Point{100, 0}, 900);
  (void)b.unit(b.classes.ram, Point{100, 0}, 900);
  (void)b.unit(b.classes.peaceful, Point{100, 0}, 900);
  CHECK(b.found("FindUnitToLearn", druid) == kNoObject);

  // So is a dead one, and one inside a holder.
  const ObjectId corpse = b.unit(b.classes.plain, Point{100, 0}, 900);
  b.world.set_health(corpse, 0);
  const World::SettlementIds town = b.world.spawn_settlement(1);
  const ObjectId garrisoned = b.unit(b.classes.plain, Point{100, 0}, 900);
  REQUIRE(b.world.put_in_holder(garrisoned, town.holder));
  CHECK(b.found("FindUnitToLearn", druid) == kNoObject);

  // And one out of sight: the radius is the druid's own `sight`, 900 here.
  const ObjectId distant = b.unit(b.classes.plain, Point{901, 0}, 900);
  CHECK(b.found("FindUnitToLearn", druid) == kNoObject);

  // One that qualifies on every clause is found.
  const ObjectId teacher = b.unit(b.classes.plain, Point{100, 0}, 51);
  CHECK(b.found("FindUnitToLearn", druid) == teacher);
  // The radius is the *receiver's* sight and not a constant: from the teacher
  // at x=100 the distant one at x=901 is 801 away, inside 900, so the teacher
  // finds what the druid could not.
  CHECK(b.found("FindUnitToLearn", teacher) == distant);
  CHECK(b.found("FindUnitToLearn", 9999) == kNoObject);
}

// ==========================================================================
// the gate
// ==========================================================================

namespace {

/// A gate, and the four kinds of thing its scan has an opinion about.
struct GateClasses {
  ClassGraph graph;
  ClassIndex gate = kNoClass;
  ClassIndex plain = kNoClass;
  ClassIndex animal = kNoClass;
  ClassIndex ambient = kNoClass;
  ClassIndex ghostly = kNoClass;

  GateClasses() {
    const std::string docs[] = {
        R"(<class id="Unit" cpp_class="CVXUnit"><properties sight="900"/></class>)",
        R"(<class id="Gate" cpp_class="CVXGate"><properties maxhealth="1000"/></class>)",
        R"(<class id="Plain" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="Animal" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="PeasantAmbient" parent="Unit" cpp_class="CVXUnit"/>)",
        // Not named by the visitor: skipped for its property instead.
        R"(<class id="Ghostly" parent="Unit" cpp_class="CVXUnit"><properties
             ignore_passability="1"/></class>)",
    };
    const char* names[] = {"unit.sc.xml",   "gate.sc.xml",    "plain.sc.xml",
                           "animal.sc.xml", "ambient.sc.xml", "ghostly.sc.xml"};
    for (int i = 0; i < 6; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    gate = graph.find("Gate");
    plain = graph.find("Plain");
    animal = graph.find("Animal");
    ambient = graph.find("PeasantAmbient");
    ghostly = graph.find("Ghostly");
  }
};

struct GateBench {
  GateClasses classes;
  World world;
  script::HostRegistry registry;
  HostContext context;

  GateBench() {
    world.set_class_graph(&classes.graph);
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  ObjectId gate(Point at, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::gate, nullptr, classes.gate);
    world.set_health(id, 1000);
    world.set_owner(id, owner);
    (void)world.set_position(id, at);
    return id;
  }
  ObjectId unit(ClassIndex which, Point at, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, which);
    world.set_health(id, 100);
    world.set_owner(id, owner);
    (void)world.set_position(id, at);
    return id;
  }

  static script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }

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

  void look(ObjectId id, std::int32_t radius) {
    CHECK(call("LookAround", 1, {obj(id), script::Value::integer(radius)}).status ==
          script::HostStatus::ok);
  }
  bool saw(ObjectId id, const char* name) {
    const script::HostOutcome out = call(name, 0, {obj(id)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }
};

}  // namespace

/// `LookAround` writes two booleans and the two predicates read them -- so they
/// are **stale until it runs**, which is why `GATE_IDLE.VS` calls it as the
/// first statement of its loop body and why the pair is meaningless without it.
///
/// Each clause of the visitor at `0x00529510` is asserted here: the dead are
/// skipped, `Animal` and `PeasantAmbient` are skipped by class, a class that
/// declares `ignore_passability` is skipped, friend is bit 0 of the *gate's own*
/// diplomacy row and not the transpose, and a hidden enemy is passed over
/// entirely rather than counted either way.
TEST(sim_look_around_is_what_makes_the_two_gate_predicates_mean_anything) {
  GateBench b;
  const ObjectId gate = b.gate(Point{0, 0}, /*owner=*/0);
  // Both start false, and stay false until a scan runs.
  CHECK(!b.saw(gate, "AreEnemiesAround"));
  CHECK(!b.saw(gate, "AreFriendsAround"));

  const ObjectId foe = b.unit(b.classes.plain, Point{100, 0}, /*owner=*/1);
  b.world.players().set(0, 1, Relation::allied, false);
  CHECK(!b.saw(gate, "AreEnemiesAround"));  // still stale
  b.look(gate, 350);
  CHECK(b.saw(gate, "AreEnemiesAround"));
  CHECK(!b.saw(gate, "AreFriendsAround"));

  // Out of the radius, and the scan clears what it found before.
  REQUIRE(b.world.set_position(foe, Point{400, 0}));
  b.look(gate, 350);
  CHECK(!b.saw(gate, "AreEnemiesAround"));

  // An ally is a friend, on the gate's own row.
  REQUIRE(b.world.set_position(foe, Point{100, 0}));
  b.world.players().set(0, 1, Relation::allied, true);
  b.look(gate, 350);
  CHECK(b.saw(gate, "AreFriendsAround"));
  CHECK(!b.saw(gate, "AreEnemiesAround"));

  // A hidden enemy is passed over entirely -- neither friend nor enemy.
  b.world.players().set(0, 1, Relation::allied, false);
  b.world.mutable_state(foe)->flags.hidden = true;
  b.look(gate, 350);
  CHECK(!b.saw(gate, "AreEnemiesAround"));
  CHECK(!b.saw(gate, "AreFriendsAround"));
  b.world.mutable_state(foe)->flags.hidden = false;

  // So is a dead one.
  b.world.set_health(foe, 0);
  b.look(gate, 350);
  CHECK(!b.saw(gate, "AreEnemiesAround"));
  b.world.set_health(foe, 100);

  // And so are the three the visitor names by class or by property.
  for (const ClassIndex which : {b.classes.animal, b.classes.ambient, b.classes.ghostly}) {
    GateBench fresh;
    const ObjectId g = fresh.gate(Point{0, 0}, 0);
    (void)fresh.unit(which, Point{100, 0}, 1);
    fresh.world.players().set(0, 1, Relation::allied, false);
    fresh.look(g, 350);
    CHECK(!fresh.saw(g, "AreEnemiesAround"));
    CHECK(!fresh.saw(g, "AreFriendsAround"));
  }

  // An unresolvable receiver leaves both fields alone -- it does not even zero
  // them -- and both predicates answer false for one.
  b.look(gate, 350);
  CHECK(b.saw(gate, "AreEnemiesAround"));
  b.look(9999, 350);
  CHECK(b.saw(gate, "AreEnemiesAround"));
  CHECK(!b.saw(9999, "AreEnemiesAround"));
  CHECK(!b.saw(9999, "AreFriendsAround"));
}

/// Playtest #16: a gate never closed. The scan is a walk over **units**
/// (0x00529bb0 hands the grid walk the `kSyncUnit` mask), and ours took every
/// object in the radius, so a gate's own walls and towers -- always within 350
/// of it, always its owner's -- made it a friend's gate for good. A garrison
/// inside a tower is off the map and is not a friend at the gate either.
TEST(sim_look_around_counts_units_on_the_map_and_not_the_gates_own_walls) {
  GateBench b;
  const ObjectId gate = b.gate(Point{0, 0}, /*owner=*/0);
  // The gate's neighbours on its own side: a wall piece, a tower, another gate.
  const ObjectId wall = b.world.spawn(NativeClass::building, nullptr, kNoClass);
  const ObjectId tower = b.world.spawn(NativeClass::building, nullptr, kNoClass);
  const ObjectId other = b.gate(Point{200, 0}, 0);
  for (const ObjectId id : {wall, tower}) {
    b.world.set_health(id, 500);
    b.world.set_owner(id, 0);
  }
  REQUIRE(b.world.set_position(wall, Point{80, 0}));
  REQUIRE(b.world.set_position(tower, Point{0, 120}));
  b.look(gate, 350);
  CHECK(!b.saw(gate, "AreFriendsAround"));
  CHECK(!b.saw(gate, "AreEnemiesAround"));
  // And so an enemy's building beside it is not an enemy there.
  b.world.set_owner(other, 1);
  b.world.players().set(0, 1, Relation::allied, false);
  b.look(gate, 350);
  CHECK(!b.saw(gate, "AreEnemiesAround"));

  // A unit in the tower is off the map: neither friend nor enemy.
  const ObjectId inside = b.unit(b.classes.plain, Point{0, 120}, 0);
  REQUIRE(b.world.put_in_holder(inside, tower));
  b.look(gate, 350);
  CHECK(!b.saw(gate, "AreFriendsAround"));
  // Out on the ground beside it, it is a friend.
  REQUIRE(b.world.remove_from_holder(inside, Point{0, 120}));
  b.look(gate, 350);
  CHECK(b.saw(gate, "AreFriendsAround"));
}

/// `OpenNow` and `CloseNow` write a target and **do not suspend**, although both
/// are registered through the suspending registrar.
///
/// Both bodies return the "did not suspend" code on every path. That is worth a
/// test of its own because the registrar is otherwise a reliable signal and this
/// project has been caught once by ignoring it -- and because a `CloseNow` that
/// suspended would stall `GATE_IDLE.VS` for two seconds a call.
TEST(sim_open_now_and_close_now_write_a_target_and_never_suspend) {
  GateBench b;
  const ObjectId gate = b.gate(Point{0, 0}, 0);
  CHECK(!b.world.state(gate)->flags.gate_open);

  const script::HostOutcome opened = b.call("OpenNow", 0, {GateBench::obj(gate)});
  CHECK(opened.status == script::HostStatus::ok);
  CHECK(opened.status != script::HostStatus::suspend);
  CHECK(b.world.state(gate)->flags.gate_open);

  // Already open is a silent no-op, not a refusal.
  CHECK(b.call("OpenNow", 0, {GateBench::obj(gate)}).status == script::HostStatus::ok);
  CHECK(b.world.state(gate)->flags.gate_open);

  const script::HostOutcome closed = b.call("CloseNow", 0, {GateBench::obj(gate)});
  CHECK(closed.status == script::HostStatus::ok);
  CHECK(!b.world.state(gate)->flags.gate_open);

  // A destroyed gate refuses both -- the original tests its building mode, and
  // zero health is the nearest thing this engine has.
  b.world.set_health(gate, 0);
  CHECK(b.call("OpenNow", 0, {GateBench::obj(gate)}).status == script::HostStatus::ok);
  CHECK(!b.world.state(gate)->flags.gate_open);

  CHECK(b.call("OpenNow", 0, {GateBench::obj(9999)}).status == script::HostStatus::ok);
  CHECK(b.call("CloseNow", 0, {script::Value::integer(2)}).status == script::HostStatus::ok);
}

/// The portcullis's travel, as 0x00529090 computes it: 70 positions in 2,000
/// ms, a part of the travel in its share of that (`distance * 2000 / 70`,
/// truncated), the position `from +- elapsed * distance / duration`, truncated,
/// and clamped at both ends. See `sim/gate.hpp`.
TEST(sim_gate_position_is_seventy_steps_in_two_seconds_from_wherever_it_starts) {
  // A full opening and a full closing.
  CHECK(gate_position(GateMotion{0, 0}, true, 0) == 0);
  CHECK(gate_position(GateMotion{0, 0}, true, 1000) == 35);
  CHECK(gate_position(GateMotion{0, 0}, true, 1999) == 69);
  CHECK(gate_position(GateMotion{0, 0}, true, 2000) == 70);
  CHECK(gate_position(GateMotion{0, 70}, false, 1000) == 35);
  CHECK(gate_position(GateMotion{0, 70}, false, 2000) == 0);
  // Measured from the move's own start.
  CHECK(gate_position(GateMotion{5000, 0}, true, 6000) == 35);
  CHECK(gate_position(GateMotion{5000, 0}, true, 4000) == 0);  // before it began
  // A part of the travel takes its share: 35 to go is 1,000 ms.
  CHECK(gate_position(GateMotion{0, 35}, true, 500) == 52);
  CHECK(gate_position(GateMotion{0, 35}, true, 1000) == 70);
  CHECK(gate_position(GateMotion{0, 35}, false, 500) == 18);
  // Both truncations: 69 to go is 138,000 / 70 = 1,971 ms, not 1,971.4, and
  // 1,000 ms of it is 69,000 / 1,971 = 35 steps, not 35.007.
  CHECK(gate_position(GateMotion{0, 1}, true, 1000) == 36);
  CHECK(gate_position(GateMotion{0, 1}, true, 1970) == 69);
  CHECK(gate_position(GateMotion{0, 1}, true, 1971) == 70);
  // Clamped at the end however long it stands, which the original never has
  // to do because it stops evaluating on arrival.
  CHECK(gate_position(GateMotion{0, 0}, true, GameTime{1} << 40) == 70);
  CHECK(gate_position(GateMotion{0, 70}, false, GameTime{1} << 40) == 0);
  // Already at the target, it does not move.
  CHECK(gate_position(GateMotion{0, 70}, true, 0) == 70);
  CHECK(gate_position(GateMotion{0, 0}, false, 1000) == 0);
  // The defaults are a gate that has stood at its target since time 0: every
  // gate on a fresh map and every gate after a load.
  CHECK(gate_position(GateMotion{}, true, 2000) == 70);
  CHECK(gate_position(GateMotion{}, false, 2000) == 0);
}

/// `OpenNow` and `CloseNow` start the portcullis moving from wherever it stands
/// at the call; `gate_raise` reads it for the view and the barrier. It is in the
/// hash, and nothing that is not a gate is ever raised.
TEST(sim_open_now_raises_the_portcullis_over_two_seconds_and_close_now_lowers_it) {
  GateBench b;
  const ObjectId gate = b.gate(Point{0, 0}, 0);
  const auto raise = [&] { return gate_raise(*b.world.find(gate), b.world.time()); };
  b.world.advance(1000);
  CHECK(raise() == 0);

  REQUIRE(b.call("OpenNow", 0, {GateBench::obj(gate)}).status == script::HostStatus::ok);
  CHECK(raise() == 0);  // it starts at the call
  b.world.advance(1000);
  CHECK(raise() == 35);
  // A repeat, as GATE_IDLE.VS makes twice a second, restarts the move from
  // where it is -- which leaves the trajectory where it was.
  REQUIRE(b.call("OpenNow", 0, {GateBench::obj(gate)}).status == script::HostStatus::ok);
  CHECK(raise() == 35);
  b.world.advance(500);
  CHECK(raise() == 52);

  // Reversed halfway: down from 52, 52 steps in 1,485 ms.
  REQUIRE(b.call("CloseNow", 0, {GateBench::obj(gate)}).status == script::HostStatus::ok);
  CHECK(raise() == 52);
  b.world.advance(500);
  CHECK(raise() == 52 - 500 * 52 / (52 * 2000 / 70));
  b.world.advance(2000);
  CHECK(raise() == 0);

  // State: where the portcullis stands decides who walks through it, so the
  // same world with the portcullis somewhere else hashes differently.
  REQUIRE(b.call("OpenNow", 0, {GateBench::obj(gate)}).status == script::HostStatus::ok);
  const std::uint64_t before = b.world.state_hash();
  b.world.find(gate)->gate = GateMotion{b.world.time(), 33};
  CHECK(b.world.state_hash() != before);
  CHECK(raise() == 33);

  // A destroyed gate refuses the call, and its portcullis stays where it was.
  b.world.advance(3000);
  CHECK(raise() == 70);
  b.world.set_health(gate, 0);
  REQUIRE(b.call("CloseNow", 0, {GateBench::obj(gate)}).status == script::HostStatus::ok);
  b.world.advance(3000);
  CHECK(raise() == 70);

  // Only a gate has a portcullis, whatever its flags say.
  const ObjectId unit = b.unit(b.classes.plain, Point{100, 0}, 0);
  b.world.mutable_state(unit)->flags.gate_open = true;
  CHECK(gate_raise(*b.world.find(unit), b.world.time()) == 0);
}

/// The closed gate's line, as 0x00529e40 walks it and 0x005298f0 lays each
/// cell: ends to cells truncating towards zero, x the major axis only when it
/// is strictly longer, and every cell with the one below it.
TEST(sim_gate_line_is_a_bresenham_line_of_cell_pairs) {
  std::vector<GateCell> cells;
  gate_line_cells(Point{0, 0}, Point{48, 32}, cells);
  const std::vector<GateCell> diagonal = {{0, 0}, {0, 1}, {1, 1}, {1, 2},
                                          {2, 1}, {2, 2}, {3, 2}, {3, 3}};
  CHECK(cells == diagonal);
  // Backwards along the same line from the other end: the error steps the
  // minor coordinate at other cells, as the original's eight branches do.
  cells.clear();
  gate_line_cells(Point{48, 32}, Point{0, 0}, cells);
  const std::vector<GateCell> back = {{3, 2}, {3, 3}, {2, 1}, {2, 2},
                                      {1, 1}, {1, 2}, {0, 0}, {0, 1}};
  CHECK(cells == back);
  // A tie goes to y; -20 is cell -1 (truncated), not -2 (floored).
  cells.clear();
  gate_line_cells(Point{-20, 0}, Point{16, 32}, cells);
  const std::vector<GateCell> tie = {{-1, 0}, {-1, 1}, {0, 1}, {0, 2}, {1, 2}, {1, 3}};
  CHECK(cells == tie);
  // An error of exactly zero steps the minor coordinate.
  cells.clear();
  gate_line_cells(Point{0, 0}, Point{64, 32}, cells);
  const std::vector<GateCell> zero = {{0, 0}, {0, 1}, {1, 1}, {1, 2}, {2, 1},
                                      {2, 2}, {3, 2}, {3, 3}, {4, 2}, {4, 3}};
  CHECK(cells == zero);
}

// The barrier, the routes and the step: `test_gate.cpp`.

// ==========================================================================
// IsPointInWater
// ==========================================================================

namespace {

/// A 4x4 terrain layer of 64-unit cells, with water at cell (1, 2).
///
/// Built rather than loaded: the shipped layer for a 16,384-unit map is 65,556
/// bytes and this needs four numbers out of it.
struct TerrainBench {
  std::vector<std::byte> bytes;
  Grid grid;
  World world;
  script::HostRegistry registry;
  HostContext context;

  TerrainBench() {
    constexpr std::uint32_t kCell = 64;
    constexpr std::uint32_t kCells = 4;
    const auto put = [this](std::uint32_t value) {
      for (int i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
      }
    };
    for (const char c : std::string_view("DIRG")) bytes.push_back(static_cast<std::byte>(c));
    put(kCell);
    put(8);  // one byte per cell
    put(kCell * kCells);
    put(kCell * kCells);
    for (std::uint32_t y = 0; y < kCells; ++y) {
      for (std::uint32_t x = 0; x < kCells; ++x) {
        // 13 is water; 3 is whatever else.
        bytes.push_back(static_cast<std::byte>(x == 1 && y == 2 ? 13 : 3));
      }
    }
    const Result<Grid> parsed = Grid::parse(bytes);
    REQUIRE(parsed.ok());
    if (parsed.ok()) grid = parsed.value();
    world.set_terrain(grid);
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  bool water(std::int32_t x, std::int32_t y) {
    const std::uint32_t index = registry.find(script::CallKind::free_function, "IsPointInWater", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return false;
    script::CallContext ctx;
    std::vector<script::Value> args{pack_point(Point{x, y})};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "IsPointInWater";
    ctx.kind = script::CallKind::free_function;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }
};

}  // namespace

/// `IsPointInWater` reads the terrain-type layer, water is type 13, and the
/// point is biased by **half a cell** before it divides.
///
/// `0x005c6d40` consults that layer and nothing else -- not the passability
/// grid, not any object -- and adds 32 to each coordinate at `0x005c6d5b`
/// before shifting, so a point rounds to the nearest cell rather than to the
/// one it sits inside. That is a real difference on a boundary and it is
/// transcribed rather than dropped: with a 64-unit cell, the cell that *covers*
/// 64..127 is the one a point in 32..95 lands in.
TEST(sim_is_point_in_water_reads_the_terrain_layer_and_rounds_to_the_nearest_cell) {
  TerrainBench b;
  // Cell (1, 2) is water. **With the bias the window is not the cell**: a point
  // lands in cell `n` when `(p + 32) / 64 == n`, so cell 1 spans x in 32..95 and
  // cell 2 spans y in 96..159 -- each shifted half a cell down from the
  // 64..127 / 128..191 the cell itself covers. Getting this backwards is a
  // half-cell error that looks right in the middle of a lake and wrong at its
  // edge, which is exactly the kind of thing a test writes down.
  CHECK(b.water(64, 128));  // comfortably inside both windows
  CHECK(!b.water(0, 0));
  CHECK(!b.water(64, 0));
  CHECK(!b.water(0, 128));

  // Both edges of both windows.
  CHECK(b.water(32, 128));
  CHECK(!b.water(31, 128));
  CHECK(b.water(95, 128));
  CHECK(!b.water(96, 128));
  CHECK(b.water(64, 96));
  CHECK(!b.water(64, 95));
  CHECK(b.water(64, 159));
  CHECK(!b.water(64, 160));

  // Off the grid is dry, silently, on every side.
  CHECK(!b.water(-1, 128));
  CHECK(!b.water(1000, 128));
  CHECK(!b.water(64, -1000));
  CHECK(!b.water(64, 10000));

  // And a session with no layer at all answers "not water" everywhere, which is
  // what every session did before the payload was wired -- the point being that
  // it degrades rather than refusing.
  World bare;
  HostContext bare_context;
  bare_context.world = &bare;
  const std::uint32_t index =
      b.registry.find(script::CallKind::free_function, "IsPointInWater", 1);
  script::CallContext ctx;
  std::vector<script::Value> args{
      pack_point(Point{64, 128})};
  ctx.arguments = args;
  ctx.user = &bare_context;
  ctx.kind = script::CallKind::free_function;
  const script::HostOutcome out = b.registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.as_integer() == 0);

  // A receiver that is not a point is dry rather than a refusal.
  std::vector<script::Value> wrong{script::Value::integer(3)};
  ctx.arguments = wrong;
  ctx.user = &b.context;
  CHECK(b.registry.entry(index).fn(ctx).status == script::HostStatus::ok);
}

/// The ship's AI transport: a pending order, its three readers, and the use
/// that clears it.
///
/// **This test used to pin a justification rather than a behaviour.** The three
/// readers answered constants because `PrepareAiTransportShip/5` -- their only
/// writer -- had no body, so the values were *derived* (no reachable path could
/// change the field) rather than guessed, and the note here said the derivation
/// would expire the day the writer got one. It has, and this is the test that
/// expiry turns into.
TEST(sim_the_ships_ai_transport_carries_what_the_ai_wrote_and_loses_it_on_apply) {
  PredicateBench b;
  const World::ShipIds ship = b.world.spawn_ship(nullptr, b.classes.unit);
  const Point nowhere{-1, -1};

  // A ship nobody has tasked reads as the constructed state, which is what
  // every ship on a shipped map still reads as until the AI asks for a
  // crossing.
  CHECK(!b.predicate("HasAiTransport", PredicateBench::obj(ship.ship)));
  {
    const script::HostOutcome where =
        b.call(script::CallKind::member, "GetTransPt", 0, {PredicateBench::obj(ship.ship)});
    CHECK(where.status == script::HostStatus::ok);
    CHECK(is_point(where.value));
    CHECK(unpack_point(where.value) == nowhere);
  }

  // An order the AI wrote is visible through all three.
  const Point beach{640, 960};
  b.ai.set_ship_transport(ship.ship, "advance", beach);
  CHECK(b.predicate("HasAiTransport", PredicateBench::obj(ship.ship)));
  {
    const script::HostOutcome where =
        b.call(script::CallKind::member, "GetTransPt", 0, {PredicateBench::obj(ship.ship)});
    CHECK(where.status == script::HostStatus::ok);
    CHECK(unpack_point(where.value) == beach);
  }
  // ...and it is the *ship's*, not the table's: a second ship has none.
  const World::ShipIds other = b.world.spawn_ship(nullptr, b.classes.unit);
  CHECK(!b.predicate("HasAiTransport", PredicateBench::obj(other.ship)));

  // `ClearAiTransport` puts it back, both halves.
  CHECK(b.call(script::CallKind::member, "ClearAiTransport", 0,
               {PredicateBench::obj(ship.ship)})
            .status == script::HostStatus::ok);
  CHECK(!b.predicate("HasAiTransport", PredicateBench::obj(ship.ship)));
  {
    const script::HostOutcome where =
        b.call(script::CallKind::member, "GetTransPt", 0, {PredicateBench::obj(ship.ship)});
    CHECK(unpack_point(where.value) == nowhere);
  }

  // **`HasAiTransport` is "the string is not empty" and nothing else** -- an
  // order with a real destination and an empty verb still reads as none, which
  // is 0x005c7190's whole body.
  b.ai.set_ship_transport(ship.ship, "", beach);
  CHECK(!b.predicate("HasAiTransport", PredicateBench::obj(ship.ship)));
  {
    const script::HostOutcome where =
        b.call(script::CallKind::member, "GetTransPt", 0, {PredicateBench::obj(ship.ship)});
    CHECK(unpack_point(where.value) == beach);
  }

  // And the fourth, the *use*: it ends by clearing the record. What it does
  // before that -- landing the passengers and ordering their squads on -- is
  // `test_boarding.cpp`'s `apply_ai_transport_lands_the_passengers...`; a ship
  // with nobody aboard, as here, has only the clear to do.
  b.ai.set_ship_transport(ship.ship, "advance", beach);
  CHECK(b.predicate("HasAiTransport", PredicateBench::obj(ship.ship)));
  CHECK(b.call(script::CallKind::member, "ApplyAiTransport", 0, {PredicateBench::obj(ship.ship)})
            .status == script::HostStatus::ok);
  CHECK(!b.predicate("HasAiTransport", PredicateBench::obj(ship.ship)));

  // Every one of the four survives a receiver that resolves to nothing.
  CHECK(!b.predicate("HasAiTransport", PredicateBench::obj(9999)));
  CHECK(b.call(script::CallKind::member, "ClearAiTransport", 0,
               {script::Value::integer(1)})
            .status == script::HostStatus::ok);
  CHECK(b.call(script::CallKind::member, "ApplyAiTransport", 0, {script::Value::integer(1)})
            .status == script::HostStatus::ok);
  {
    const script::HostOutcome where =
        b.call(script::CallKind::member, "GetTransPt", 0, {script::Value::integer(1)});
    CHECK(where.status == script::HostStatus::ok);
    CHECK(unpack_point(where.value) == nowhere);
  }
}

/// **`Consume` waits one period on a first call with the flag set, and then
/// yields what an empty ritual yields.** 0x00596440's first-call byte and
/// sleep slot, then the core's answer over a druid list nothing here can fill.
TEST(sim_consume_waits_one_period_and_an_empty_ritual_yields_nothing) {
  PredicateBench b;
  const ObjectId ritual = b.world.spawn(NativeClass::sacrifice, nullptr, b.classes.object);
  const auto consume = [&](bool first_call, std::int64_t period, bool wait) -> script::HostOutcome {
    const std::uint32_t index = b.registry.find(script::CallKind::member, "Consume", 3);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    std::vector<script::Value> args{PredicateBench::obj(ritual), script::Value::integer(50),
                                    script::Value::integer(period),
                                    script::Value::boolean(wait)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &b.context;
    ctx.name = "Consume";
    ctx.kind = script::CallKind::member;
    ctx.first_call = first_call;
    return b.registry.entry(index).fn(ctx);
  };
  // `.Consume(price, 1000, true)`: the first call sleeps the period -- the
  // argument's, asked with a number no constant could stand in for ...
  script::HostOutcome out = consume(true, 700, true);
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 700);
  // ... and the re-entry answers 0, which ends the ghost's loop.
  out = consume(false, 700, true);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.is_integer() && out.value.as_integer() == 0);
  // With the flag clear there is no wait at all.
  out = consume(true, 1000, false);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.is_integer() && out.value.as_integer() == 0);
  // A period that is not a wait is no wait.
  out = consume(true, -5, true);
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 0);
}

// ==========================================================================
// the stonehenge
// ==========================================================================

namespace {

struct HengeBench {
  ClassGraph graph;
  bool has_stonehenge_class = true;
  World world;
  script::HostRegistry registry;
  HostContext context;
  ClassIndex henge_class = kNoClass;
  ClassIndex mage_class = kNoClass;
  ClassIndex soldier_class = kNoClass;
  ClassIndex civilian_class = kNoClass;

  explicit HengeBench(bool with_stonehenge = true) : has_stonehenge_class(with_stonehenge) {
    const std::string docs[] = {
        R"(<class id="Building" cpp_class="CVXBuilding"/>)",
        // Named `Stonehenge` or not: a world whose graph has no such class must
        // not answer for every building instead.
        with_stonehenge
            ? R"(<class id="Stonehenge" parent="Building" cpp_class="CVXTownHall"><properties
             sight="900" maxhealth="5000"/></class>)"
            : R"(<class id="Henge" parent="Building" cpp_class="CVXTownHall"><properties
             sight="900" maxhealth="5000"/></class>)",
        R"(<class id="Military" cpp_class="CVXUnit"/>)",
        R"(<class id="BaseMage" parent="Military" cpp_class="CVXDruid"/>)",
        R"(<class id="Soldier" parent="Military" cpp_class="CVXUnit"/>)",
        // Neither `Military` nor `BaseMage`: the visitor passes over it.
        R"(<class id="Villager" cpp_class="CVXUnit"/>)",
    };
    const char* names[] = {"building.sc.xml", "henge.sc.xml",   "military.sc.xml",
                           "mage.sc.xml",     "soldier.sc.xml", "villager.sc.xml"};
    for (int i = 0; i < 6; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    world.set_class_graph(&graph);
    henge_class = graph.find(with_stonehenge ? "Stonehenge" : "Henge");
    mage_class = graph.find("BaseMage");
    soldier_class = graph.find("Soldier");
    civilian_class = graph.find("Villager");
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  ObjectId henge(Point at) {
    const ObjectId id = world.spawn(NativeClass::town_hall, nullptr, henge_class);
    world.set_health(id, 5000);
    (void)world.set_position(id, at);
    world.find(id)->sight = 900;
    return id;
  }
  ObjectId unit(ClassIndex which, Point at, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, which);
    world.set_health(id, 100);
    world.set_owner(id, owner);
    (void)world.set_position(id, at);
    return id;
  }

  script::HostOutcome call(const char* name, script::Value receiver) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    std::vector<script::Value> args{receiver};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  }
  std::int32_t mages(ObjectId id) {
    const script::HostOutcome out =
        call("StonehengeNumControllingMages", script::Value::object(kTypeObj, id));
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  }
  bool controlable(ObjectId id) {
    const script::HostOutcome out =
        call("IsStonehengeControlable", script::Value::object(kTypeObj, id));
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }
};

}  // namespace

/// The count is **"mages of one alliance, provided nobody hostile is also
/// here"** -- and a single hostile soldier zeroes it.
///
/// That is why `Military` non-mages are in the visitor's predicate at all: they
/// cannot add to the count, and they exist to spoil it. A friendly player's
/// mages contribute only when the row carries shared control, which is bit 5 and
/// `Relation::share_control` here.
TEST(sim_the_stonehenge_count_is_one_alliances_mages_and_a_hostile_zeroes_it) {
  HengeBench b;
  const ObjectId henge = b.henge(Point{0, 0});
  CHECK(b.mages(henge) == 0);

  // Two mages of one player, and a soldier of the same, which does not count.
  (void)b.unit(b.mage_class, Point{100, 0}, 1);
  (void)b.unit(b.mage_class, Point{200, 0}, 1);
  (void)b.unit(b.soldier_class, Point{300, 0}, 1);
  CHECK(b.mages(henge) == 2);

  // A villager is neither `Military` nor `BaseMage` and is passed over
  // entirely -- it neither counts nor spoils, whoever owns it.
  (void)b.unit(b.civilian_class, Point{100, 0}, 5);
  CHECK(b.mages(henge) == 2);

  // Out of the henge's own sight, and it does not count.
  (void)b.unit(b.mage_class, Point{901, 0}, 1);
  CHECK(b.mages(henge) == 2);

  // A dead mage does not count, and neither does a spawn template -- the
  // visitor's second and third clauses, and neither was in this fixture until a
  // fault injected into each survived the whole suite.
  const ObjectId corpse = b.unit(b.mage_class, Point{100, 0}, 1);
  b.world.set_health(corpse, 0);
  CHECK(b.mages(henge) == 2);
  // A spawn template does not count either, and that is `World::collect`'s
  // doing rather than this scan's: every spatial query filters templates in one
  // place, so the visitor's own test is not repeated here.
  const ObjectId waiting = b.unit(b.mage_class, Point{100, 0}, 1);
  b.world.mutable_state(waiting)->flags.unspawned = true;
  CHECK(b.mages(henge) == 2);

  // **The counter does not pin the local player.** It adopts whoever is
  // standing there, which is the one thing that separates it from
  // `IsStonehengeControlable` -- so setting a local player who owns none of
  // these mages must change nothing.
  b.context.local_player = 2;
  CHECK(b.mages(henge) == 2);
  b.context.local_player = kNoPlayer;

  // An allied player's mage counts only with shared control.
  const ObjectId ally_mage = b.unit(b.mage_class, Point{150, 0}, 2);
  b.world.players().set(2, 1, Relation::allied, true);
  b.world.players().set(1, 2, Relation::allied, true);
  CHECK(b.mages(henge) == 2);  // allied, but no shared control
  b.world.players().set(2, 1, Relation::share_control, true);
  CHECK(b.mages(henge) == 3);
  (void)ally_mage;

  // And one hostile soldier zeroes the whole thing.
  (void)b.unit(b.soldier_class, Point{250, 0}, 7);
  CHECK(b.mages(henge) == 0);
}

/// The two entry points differ in **one thing**: which player the row is read
/// against.
///
/// The counter adopts whoever is standing there -- *how many mages does
/// whichever single alliance is here have* -- and the predicate pins it to the
/// commanding player -- *can I control this right now*. So
/// `IsStonehengeControlable` answers differently on different peers, and that is
/// correct: `HostContext::local_player` is outside the world for exactly this
/// reason, and the five call sites are the five spell verify scripts, which
/// decide whether *this* player's right click does anything.
TEST(sim_is_stonehenge_controlable_is_the_same_scan_against_the_local_player) {
  HengeBench b;
  const ObjectId henge = b.henge(Point{0, 0});
  (void)b.unit(b.mage_class, Point{100, 0}, 1);
  CHECK(b.mages(henge) == 1);

  // A headless run has no local player, and "can nobody control it" is false.
  CHECK(b.context.local_player == kNoPlayer);
  CHECK(!b.controlable(henge));

  // The player whose mage it is can.
  b.context.local_player = 1;
  CHECK(b.controlable(henge));
  // Another cannot -- the mage is not its enemy's to command, and its row
  // carries no shared control.
  b.context.local_player = 2;
  CHECK(!b.controlable(henge));

  // Neither answers for a receiver that is not a stonehenge, or for one that
  // resolves to nothing -- 0 and false, each after a complaint the retail sink
  // discards.
  const ObjectId soldier = b.unit(b.soldier_class, Point{0, 0}, 1);
  b.context.local_player = 1;
  CHECK(b.mages(soldier) == 0);
  CHECK(!b.controlable(soldier));
  CHECK(b.mages(9999) == 0);
  CHECK(!b.controlable(9999));
}

/// A world whose class graph has no `Stonehenge` answers 0 and false for
/// everything, rather than treating every building as one.
///
/// `ClassFilter::parse` reports `match_all` for a name the graph does not know,
/// and `match_all` here would make `IsHeirOf`'s own rule -- *an unresolvable
/// name must not read as true* -- fail in the one place it matters most. The
/// case is unreachable on shipped data, where `Stonehenge` is a real class, and
/// a fault injected into the guard survived the whole suite until this existed.
TEST(sim_a_world_with_no_stonehenge_class_has_no_stonehenges) {
  // The same fixture with its henge class named something else, so that
  // `Military` and `BaseMage` still resolve -- without them the scan would
  // answer 0 for a reason of its own and the test would measure nothing. That
  // is not hypothetical: swapping the graph out from under a built world does
  // exactly that, because the objects' class indices then name rows in the
  // wrong table, and the fault survived until this fixture stopped doing it.
  HengeBench yes(true);
  const ObjectId real = yes.henge(Point{0, 0});
  (void)yes.unit(yes.mage_class, Point{100, 0}, 1);
  yes.context.local_player = 1;
  CHECK(yes.mages(real) == 1);
  CHECK(yes.controlable(real));

  HengeBench no(false);
  const ObjectId impostor = no.henge(Point{0, 0});
  (void)no.unit(no.mage_class, Point{100, 0}, 1);
  no.context.local_player = 1;
  CHECK(no.mages(impostor) == 0);
  CHECK(!no.controlable(impostor));
}

/// `SetMessengerStatus` writes **every** member of its receiver.
///
/// `gbr.exe` registers it twice, on `Obj` and on `Query`, and the second is the
/// same write applied to each member. One body serves both here because the
/// registry keys on (kind, name, arity) and `receiver_objects` resolves all
/// three handle shapes -- but a body that only wrote `arg(0)` passes every test
/// written against an object receiver, and did.
TEST(sim_set_messenger_status_writes_every_member_of_its_receiver) {
  PredicateBench b;
  const ObjectId one = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId two = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId three = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);

  // The object form.
  CHECK(b.call(script::CallKind::member, "SetMessengerStatus", 1,
               {PredicateBench::obj(one), script::Value::boolean(true)})
            .status == script::HostStatus::ok);
  CHECK(b.world.state(one)->flags.messenger);
  CHECK(!b.world.state(two)->flags.messenger);

  // The list form, which is the one that matters.
  ObjListPool& pool = objlist_pool_of(b.world);
  const ObjListId list = pool.acquire(5, 0);
  REQUIRE(list != kNoObjList);
  *pool.mutable_items(list) = std::vector<ObjectId>{one, two, three};
  CHECK(b.call(script::CallKind::member, "SetMessengerStatus", 1,
               {make_objlist_value(list), script::Value::boolean(true)})
            .status == script::HostStatus::ok);
  CHECK(b.world.state(one)->flags.messenger);
  CHECK(b.world.state(two)->flags.messenger);
  CHECK(b.world.state(three)->flags.messenger);

  // And clearing goes the same way.
  CHECK(b.call(script::CallKind::member, "SetMessengerStatus", 1,
               {make_objlist_value(list), script::Value::boolean(false)})
            .status == script::HostStatus::ok);
  CHECK(!b.world.state(one)->flags.messenger);
  CHECK(!b.world.state(two)->flags.messenger);
  CHECK(!b.world.state(three)->flags.messenger);

  CHECK(b.call(script::CallKind::member, "SetMessengerStatus", 1,
               {script::Value::integer(4), script::Value::boolean(true)})
            .status == script::HostStatus::ok);
}

/// Seven entry points that are one class property or one ancestry test, and
/// nothing else.
///
/// The tree here is the shape the shipped one has where it matters:
/// `levelperitem`, `minlevel` and `itemtype` are declared on `BaseRuins` and
/// inherited by `Ruins1`; `num_sentry_slots` and `sentry_class_name` on a wall
/// and inherited by a gate; and `RamUnit` and `PeasantAmbient` have subclasses,
/// because the two predicates test **descent** rather than the name.
TEST(the_class_property_readers_answer_the_class_and_walk_up_the_tree) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)",
      R"(<class id="Building" cpp_class="CVXBuilding" parent="Object"/>)",
      // `BASERUINS.SC.XML`'s four, and the value of `itemtype` is a *list*.
      // The shipped `minlevel` and `levelperitem` are **both 4**; they differ
      // here on purpose, because two readers of the same number cannot be told
      // apart by a fixture that gives them the same one.
      R"(<class id="BaseRuins" cpp_class="CVXBuilding" parent="Building">
           <properties minlevel="4" levelperitem="3" itemmanage="0"
                       itemtype="Ring of Power,Sword of Kings"/>
         </class>)",
      R"(<class id="Ruins1" cpp_class="CVXBuilding" parent="BaseRuins"/>)",
      R"(<class id="RWallsE" cpp_class="CVXBuilding" parent="Building">
           <properties num_sentry_slots="4" sentry_class_name="RSentry"/>
         </class>)",
      R"(<class id="Gate" cpp_class="CVXBuilding" parent="RWallsE">
           <properties sentry_class_name="RSentry1"/>
         </class>)",
      R"(<class id="Unit" cpp_class="CVXUnit" parent="Object"/>)",
      R"(<class id="RamUnit" cpp_class="CVXUnit" parent="Unit"/>)",
      R"(<class id="RRam" cpp_class="CVXUnit" parent="RamUnit"/>)",
      R"(<class id="PeasantAmbient" cpp_class="CVXUnit" parent="Unit"/>)",
      R"(<class id="GPeasantAmbient" cpp_class="CVXUnit" parent="PeasantAmbient"/>)",
  };
  const char* names[] = {"object.sc.xml",   "building.sc.xml", "baseruins.sc.xml",
                         "ruins1.sc.xml",   "rwallse.sc.xml",  "gate.sc.xml",
                         "unit.sc.xml",     "ramunit.sc.xml",  "rram.sc.xml",
                         "peasantambient.sc.xml", "gpeasantambient.sc.xml"};
  for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    REQUIRE(graph.add(bytes_of(docs[i]), names[i]).ok());
  }
  graph.link();

  World world;
  world.set_class_graph(&graph);
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto member = [&](const char* name, script::Value receiver) -> script::HostOutcome {
    const std::uint32_t index = registry.find(script::CallKind::member, name, 0);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::Value args[] = {receiver};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return entry.fn(ctx);
  };
  const auto number = [&](const char* name, script::Value receiver) {
    const script::HostOutcome out = member(name, receiver);
    CHECK(out.status == script::HostStatus::ok);
    return out.status == script::HostStatus::ok && out.value.is_integer()
               ? out.value.as_integer()
               : -1;
  };
  const auto text = [&](const char* name, script::Value receiver) {
    const script::HostOutcome out = member(name, receiver);
    CHECK(out.status == script::HostStatus::ok);
    return out.status == script::HostStatus::ok && out.value.is_string()
               ? std::string(out.value.as_string())
               : std::string("<refused>");
  };
  const auto yes = [&](const char* name, script::Value receiver) {
    const script::HostOutcome out = member(name, receiver);
    CHECK(out.status == script::HostStatus::ok);
    return out.status == script::HostStatus::ok && out.value.is_integer() &&
           out.value.as_integer() != 0;
  };
  const auto obj = [](ObjectId id) { return script::Value::object(kTypeObj, id); };

  const ObjectId ruin = world.spawn(NativeClass::building, nullptr, graph.find("Ruins1"));
  const ObjectId wall = world.spawn(NativeClass::building, nullptr, graph.find("RWallsE"));
  const ObjectId gate = world.spawn(NativeClass::building, nullptr, graph.find("Gate"));
  const ObjectId plain = world.spawn(NativeClass::building, nullptr, graph.find("Building"));
  const ObjectId ram = world.spawn(NativeClass::unit, nullptr, graph.find("RRam"));
  const ObjectId peasant =
      world.spawn(NativeClass::unit, nullptr, graph.find("GPeasantAmbient"));
  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));

  // Inherited from `BaseRuins`, which is the whole point of the subclass.
  CHECK(number("minlevel", obj(ruin)) == 4);
  CHECK(number("levelperitem", obj(ruin)) == 3);
  CHECK(text("itemtypes", obj(ruin)) == "Ring of Power,Sword of Kings");
  // A building that declares none of them reads the type's zero, not a refusal.
  CHECK(number("minlevel", obj(plain)) == 0);
  CHECK(number("levelperitem", obj(plain)) == 0);
  CHECK(text("itemtypes", obj(plain)).empty());

  CHECK(number("GetNumSentrySlots", obj(wall)) == 4);
  CHECK(text("GetSentryClassName", obj(wall)) == "RSentry");
  // The gate overrides the name and inherits the count -- so a reader that
  // stopped at the receiver's own class would get the count wrong, and one
  // that always walked to the root would get the name wrong.
  CHECK(number("GetNumSentrySlots", obj(gate)) == 4);
  CHECK(text("GetSentryClassName", obj(gate)) == "RSentry1");
  CHECK(text("GetSentryClassName", obj(plain)).empty());

  // Descent, not equality.
  CHECK(yes("IsRam", obj(ram)));
  CHECK(yes("IsPeasantAmbient", obj(peasant)));
  CHECK(!yes("IsRam", obj(soldier)));
  CHECK(!yes("IsPeasantAmbient", obj(soldier)));
  // ...and the two do not answer for each other.
  CHECK(!yes("IsRam", obj(peasant)));
  CHECK(!yes("IsPeasantAmbient", obj(ram)));

  // A receiver naming no object is the type's zero everywhere, which is what
  // each thunk pushes after its diagnostic through the bare-`ret` reporter.
  const script::Value nobody = obj(static_cast<ObjectId>(9999));
  CHECK(number("minlevel", nobody) == 0);
  CHECK(number("levelperitem", nobody) == 0);
  CHECK(number("GetNumSentrySlots", nobody) == 0);
  CHECK(text("itemtypes", nobody).empty());
  CHECK(text("GetSentryClassName", nobody).empty());
  CHECK(!yes("IsRam", nobody));
  CHECK(!yes("IsPeasantAmbient", nobody));
}

/// The three remaining presentation requests run and store nothing.
///
/// The assertion that matters is that they *run*: the alternative to a body
/// here is the calling script stopping, and for `UNIT_GRAB_GOODS.VS`'s siblings
/// the call is the whole script.
TEST(the_presentation_requests_run_and_change_no_state) {
  World world;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const ObjectId inn = world.spawn(NativeClass::building, nullptr);
  const ObjectId ship = world.spawn(NativeClass::ship, nullptr);
  const ObjectId anyone = world.spawn(NativeClass::unit, nullptr);
  const std::uint64_t before = world.state_hash();

  const auto call = [&](const char* name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(
        script::CallKind::member, name, static_cast<std::uint16_t>(args.size() - 1));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return entry.fn(ctx);
  };
  const auto obj = [](ObjectId id) { return script::Value::object(kTypeObj, id); };

  CHECK(call("PopTransportationUI", {obj(inn)}).status == script::HostStatus::ok);
  CHECK(call("ShowBuildAnimation", {obj(ship), pack_point(Point{40, 0})}).status ==
        script::HostStatus::ok);
  CHECK(call("SetDebug", {obj(anyone), script::Value::boolean(true)}).status ==
        script::HostStatus::ok);
  CHECK(world.state_hash() == before);

  // A receiver naming no object runs too: each thunk reports into a sink that
  // is a bare `ret` in retail and returns.
  const script::Value nobody = obj(static_cast<ObjectId>(9999));
  CHECK(call("PopTransportationUI", {nobody}).status == script::HostStatus::ok);
  CHECK(call("ShowBuildAnimation", {nobody, pack_point(Point{})}).status ==
        script::HostStatus::ok);
  CHECK(call("SetDebug", {nobody, script::Value::boolean(false)}).status ==
        script::HostStatus::ok);
  CHECK(world.state_hash() == before);

  // ...and a null world refuses rather than dereferencing, which is what every
  // entry point in this file does.
  script::CallContext bare;
  std::vector<script::Value> args{obj(inn)};
  bare.arguments = args;
  bare.user = nullptr;
  bare.name = "PopTransportationUI";
  bare.kind = script::CallKind::member;
  const std::uint32_t index =
      registry.find(script::CallKind::member, "PopTransportationUI", 0);
  REQUIRE(index != script::kUnresolvedHost);
  CHECK(registry.entry(index).fn(bare).status == script::HostStatus::error);
}

/// `b.RRepair()` heals a ruin to one percent past the point where it stops
/// being a ruin, and refuses everything else.
TEST(rrepair_lifts_a_broken_building_just_clear_of_broken) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)",
      R"(<class id="BaseVillage" cpp_class="CVXTownHall" parent="Object"><properties maxhealth="1000"/></class>)",
      R"(<class id="BaseTownhall" cpp_class="CVXTownHall" parent="Object"><properties maxhealth="1000"/></class>)",
      // The two halves of the `auto_repair` gate, with the same health so the
      // arithmetic cannot tell them apart.
      R"(<class id="Hut" cpp_class="CVXBuilding" parent="Object"><properties maxhealth="1000" auto_repair="no"/></class>)",
      R"(<class id="Tower" cpp_class="CVXBuilding" parent="Object"><properties maxhealth="1000" auto_repair="yes"/></class>)",
  };
  const char* names[] = {"object.sc.xml", "basevillage.sc.xml", "basetownhall.sc.xml",
                         "hut.sc.xml", "tower.sc.xml"};
  for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    REQUIRE(graph.add(bytes_of(docs[i]), names[i]).ok());
  }
  graph.link();

  World world;
  EconomySystem economy;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  world.set_class_graph(&graph);
  REQUIRE(world.add_system(&economy));
  economy.start(world);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto settlement_of = [&](const char* anchor_class) {
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.owner = 1;
    init.anchor = world.spawn(NativeClass::town_hall, nullptr, graph.find(anchor_class));
    world.set_health(init.anchor, 1000);
    return economy.create(world, init);
  };
  const SettlementId village = settlement_of("BaseVillage");
  const SettlementId town = settlement_of("BaseTownhall");
  REQUIRE(economy.find(village) != nullptr);
  REQUIRE(economy.find(town) != nullptr);

  const auto build = [&](const char* class_name, SettlementId where) {
    const ObjectId id = world.spawn(NativeClass::building, nullptr, graph.find(class_name));
    world.find(id)->settlement = economy.find(where)->object;
    world.set_health(id, 1000);
    return id;
  };
  const auto repair = [&](ObjectId id) {
    const std::uint32_t index = registry.find(script::CallKind::member, "RRepair", 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    std::vector<script::Value> args{script::Value::object(kTypeObj, id)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "RRepair";
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };

  // The shipped thresholds: tier 3 below 25%, hysteresis 3, so the repair
  // target is 29% of 1000. Asserted against the rules rather than the literal.
  const BuildingStateRules& rules = world.building_state_rules();
  const std::int32_t target =
      (rules.threshold2 + rules.hysteresis + 1) * 1000 / 100;
  CHECK(target == 290);

  // A building that is merely damaged is left alone -- this is not "repair a
  // bit". 400/1000 is 40%, which is tier 1.
  const ObjectId scratched = build("Hut", town);
  world.set_health(scratched, 400);
  REQUIRE(world.find(scratched)->state.damage_state != 3);
  CHECK(repair(scratched).status == script::HostStatus::ok);
  CHECK(world.find(scratched)->state.health == 400);

  // A ruin outside a village, of a class that does not auto-repair: healed,
  // and healed exactly clear of broken.
  const ObjectId ruin = build("Hut", town);
  world.set_health(ruin, 100);
  REQUIRE(world.find(ruin)->state.damage_state == 3);
  CHECK(repair(ruin).status == script::HostStatus::ok);
  CHECK(world.find(ruin)->state.health == target);
  // ...and it is no longer broken, because `set_health` re-tiers it.
  CHECK(world.find(ruin)->state.damage_state != 3);

  // The same class, already healthy, is untouched a second time.
  CHECK(repair(ruin).status == script::HostStatus::ok);
  CHECK(world.find(ruin)->state.health == target);

  // An `auto_repair="yes"` class outside a village is **not** repaired...
  const ObjectId tower = build("Tower", town);
  world.set_health(tower, 100);
  REQUIRE(world.find(tower)->state.damage_state == 3);
  CHECK(repair(tower).status == script::HostStatus::ok);
  CHECK(world.find(tower)->state.health == 100);

  // ...and inside one it is.
  const ObjectId village_tower = build("Tower", village);
  world.set_health(village_tower, 100);
  CHECK(repair(village_tower).status == script::HostStatus::ok);
  CHECK(world.find(village_tower)->state.health == target);

  // A building belonging to no settlement takes the same refusal, because the
  // gate needs a village and there is none.
  const ObjectId orphan = world.spawn(NativeClass::building, nullptr, graph.find("Tower"));
  world.set_health(orphan, 100);
  CHECK(repair(orphan).status == script::HostStatus::ok);
  CHECK(world.find(orphan)->state.health == 100);

  // A receiver naming no object runs and does nothing.
  CHECK(repair(static_cast<ObjectId>(9999)).status == script::HostStatus::ok);
}

/// The Jupiter-anger pair: the setter validates nothing, the getter validates
/// twice.
TEST(the_jupiter_anger_target_is_stored_raw_and_checked_on_the_way_out) {
  World world;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const ObjectId priest = world.spawn(NativeClass::unit, nullptr);
  const ObjectId victim = world.spawn(NativeClass::unit, nullptr);
  world.find(victim)->state.flags.is_unit = true;
  const ObjectId keep = world.spawn(NativeClass::building, nullptr);
  world.find(keep)->state.flags.is_building = true;

  const auto call = [&](const char* name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(
        script::CallKind::member, name, static_cast<std::uint16_t>(args.size() - 1));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };
  const auto obj = [](ObjectId id) { return script::Value::object(kTypeObj, id); };
  const auto target_of = [&](ObjectId who) {
    const script::HostOutcome out = call("GetJupiterAngerTarget", {obj(who)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() && out.value.as_object().valid()
               ? static_cast<ObjectId>(out.value.as_object().id)
               : kNoObject;
  };

  // Nothing set: the invalid handle, which is what `IsAlive` is written for.
  CHECK(target_of(priest) == kNoObject);

  CHECK(call("SetJupiterAngerTarget", {obj(priest), obj(victim)}).status ==
        script::HostStatus::ok);
  CHECK(world.find(priest)->state.jupiter_target == victim);
  CHECK(target_of(victim) == kNoObject);  // the target has no grudge of its own
  CHECK(target_of(priest) == victim);

  // **The setter validates nothing**: a building is stored happily...
  CHECK(call("SetJupiterAngerTarget", {obj(priest), obj(keep)}).status ==
        script::HostStatus::ok);
  CHECK(world.find(priest)->state.jupiter_target == keep);
  // ...and the *getter* is where it fails the `is_unit` test.
  CHECK(target_of(priest) == kNoObject);

  // A target that has gone reads as nothing, which is the case the pair exists
  // for -- the grudge outlives the object.
  CHECK(call("SetJupiterAngerTarget", {obj(priest), obj(victim)}).status ==
        script::HostStatus::ok);
  CHECK(target_of(priest) == victim);
  CHECK(world.despawn(victim));
  CHECK(world.find(priest)->state.jupiter_target == victim);
  CHECK(target_of(priest) == kNoObject);

  // Storing nothing is storing nothing, not leaving the old grudge.
  CHECK(call("SetJupiterAngerTarget", {obj(priest), obj(9999)}).status ==
        script::HostStatus::ok);
  CHECK(world.find(priest)->state.jupiter_target == kNoObject);

  // It is state: it moves the hash and survives a save.
  const ObjectId other = world.spawn(NativeClass::unit, nullptr);
  world.find(other)->state.flags.is_unit = true;
  const std::uint64_t before = world.state_hash();
  world.find(priest)->state.jupiter_target = other;
  CHECK(world.state_hash() != before);
  std::vector<std::byte> bytes;
  world.serialize(bytes);
  World back;
  REQUIRE(back.deserialize(bytes).ok());
  REQUIRE(back.find(priest) != nullptr);
  CHECK(back.find(priest)->state.jupiter_target == other);
  CHECK(back.state_hash() == world.state_hash());
}

/// `EnemyInRange(pt, range, who)` sweeps a **square**, not a circle.
TEST(enemy_in_range_sweeps_a_square_around_the_point) {
  World world;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const ObjectId watcher = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(world.set_owner(watcher, 0));
  REQUIRE(world.set_position(watcher, Point{5000, 5000}));

  const auto ask = [&](Point at, std::int32_t range) {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "EnemyInRange", 3);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return false;
    std::vector<script::Value> args{pack_point(at), script::Value::integer(range),
                                    script::Value::object(kTypeObj, watcher)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "EnemyInRange";
    ctx.kind = script::CallKind::free_function;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.truthy_scalar();
  };

  CHECK(!ask(Point{5000, 5000}, 500));

  // Player 0 grants player 1 nothing, so player 1 is its enemy by the default
  // zero row; player 2 gets a ceasefire and is not.
  const ObjectId hostile = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(world.set_owner(hostile, 1));
  const ObjectId friendly = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(world.set_owner(friendly, 2));
  world.players().set_relation_word(0, 2, kRelationFriendly);

  REQUIRE(world.set_position(hostile, Point{5400, 5000}));
  CHECK(ask(Point{5000, 5000}, 500));
  CHECK(!ask(Point{5000, 5000}, 300));

  // **A square.** The corner is 400 out on each axis -- inside the square of
  // half-side 500, and outside a circle of radius 500, which is 565 away.
  REQUIRE(world.set_position(hostile, Point{5400, 5400}));
  CHECK(ask(Point{5000, 5000}, 500));

  // ...and one axis past the edge is out, however close the other is.
  REQUIRE(world.set_position(hostile, Point{5501, 5000}));
  CHECK(!ask(Point{5000, 5000}, 500));
  REQUIRE(world.set_position(hostile, Point{5500, 5000}));
  CHECK(ask(Point{5000, 5000}, 500));  // the edge itself is in

  // ...and the **other** axis is tested too, which is the one thing a square
  // written as two ranges can quietly lose half of.
  REQUIRE(world.set_position(hostile, Point{5000, 5501}));
  CHECK(!ask(Point{5000, 5000}, 500));
  REQUIRE(world.set_position(hostile, Point{5000, 5500}));
  CHECK(ask(Point{5000, 5000}, 500));
  REQUIRE(world.set_position(hostile, Point{5000, 4499}));
  CHECK(!ask(Point{5000, 5000}, 500));

  // A player the asker has a ceasefire with is not an enemy, at any distance.
  REQUIRE(world.set_position(hostile, Point{9000, 9000}));
  REQUIRE(world.set_position(friendly, Point{5000, 5000}));
  CHECK(!ask(Point{5000, 5000}, 500));
  // ...and neither is the asker's own side.
  const ObjectId mate = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(world.set_owner(mate, 0));
  REQUIRE(world.set_position(mate, Point{5000, 5000}));
  CHECK(!ask(Point{5000, 5000}, 500));

  // An object with no owner is nobody's enemy.
  const ObjectId rock = world.spawn(NativeClass::decor, nullptr);
  REQUIRE(world.set_position(rock, Point{5000, 5000}));
  CHECK(!ask(Point{5000, 5000}, 500));

  // The point is the argument's, not the asking object's -- the shipped caller
  // passes `.pos` and could not tell the two apart, so this does.
  REQUIRE(world.set_position(hostile, Point{100, 100}));
  CHECK(ask(Point{100, 100}, 200));
  CHECK(!ask(Point{5000, 5000}, 200));

  // An asking object that does not resolve is false, not a refusal.
  const std::uint32_t index =
      registry.find(script::CallKind::free_function, "EnemyInRange", 3);
  REQUIRE(index != script::kUnresolvedHost);
  std::vector<script::Value> args{pack_point(Point{100, 100}), script::Value::integer(200),
                                  script::Value::object(kTypeObj, 9999)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "EnemyInRange";
  ctx.kind = script::CallKind::free_function;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(!out.value.truthy_scalar());
}

// --------------------------------------------------------------------------
// the catapult pair: AttackWait and RotateTo
// --------------------------------------------------------------------------

namespace {

/// A catapult entity: an `attack` state whose animation is one second long,
/// as every shipped catapult's is. The sheet has as many rows as the strip
/// has frames, because the timeline fits the strip to the sheet and a taller
/// sheet would stretch the cycle.
constexpr std::string_view kCatapultEntityXml = R"(<?xml version="1.0"?>
<entity name="catapult" type="vx/building" variations="8">
  <images>
    <image idx="1" file="cat.rle" drawmode="player_color" remaping="none" rows="2" columns="8"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="0" anim_idx="3" anim_frame="1"/>
    <state idx="2" name="attack" image_idx="1" image_row="0" anim_idx="5" anim_frame="1"/>
  </states>
  <anims>
    <anim idx="3" name="Idle" startstate="1" endstate="1" frames="2" duration="400"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="200"/>
      <frame idx="2" duration="200"/>
    </anim>
    <anim idx="5" name="Attack" startstate="2" endstate="2" frames="2" duration="1000"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="500"/>
      <frame idx="2" duration="500"/>
    </anim>
  </anims>
</entity>)";

}  // namespace

TEST(attack_wait_is_the_shot_interval_for_the_crew_less_the_attack_animation) {
  const Result<imperivm::core::Entity> entity = imperivm::core::Entity::parse(bytes_of(kCatapultEntityXml));
  REQUIRE(entity.ok());
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="RCatapult" cpp_class="CVXCatapult"><properties sight="150" maxhealth="400" max_units="10"/></class>)"),
            "rcatapult.sc.xml");
  graph.link();

  PredicateBench b;
  b.world.set_class_graph(&graph);
  // The bench already runs an economy; a second one on the same world would
  // be a different store from the one the body finds by name.
  EconomySystem& economy = b.economy;
  CombatSystem combat;
  REQUIRE(b.world.add_system(&combat));
  const ObjectId cat = b.world.spawn(NativeClass::catapult, &entity.value(), graph.find("RCatapult"));
  b.world.set_owner(cat, 1);
  b.world.set_health(cat, 400);
  const World::SettlementIds ids = b.world.spawn_settlement(1);
  SettlementInit init;
  init.settlement_object = ids.settlement;
  init.holder_object = ids.holder;
  init.warehouse_object = ids.warehouse;
  init.anchor = cat;
  init.owner = 1;
  init.max_units = 10;
  const SettlementId id = economy.create(b.world, init);
  REQUIRE(id != kNoSettlement);

  const auto wait = [&] {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "AttackWait", 0, {PredicateBench::obj(cat)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  // No crew: the original's -1, which the caller sees as 0.
  CHECK(wait() == 0);
  // One crewman: 600000 / 40 = 15000, less the one-second attack animation.
  std::vector<ObjectId> crew;
  for (int i = 0; i < 10; ++i) {
    const ObjectId u = b.world.spawn(NativeClass::unit, nullptr, graph.find("RCatapult"));
    crew.push_back(u);
  }
  REQUIRE(economy.garrison_add(id, crew[0]));
  CHECK(wait() == 14000);
  // Two: 600000 / 68 = 8823, less 1000.
  REQUIRE(economy.garrison_add(id, crew[1]));
  CHECK(wait() == 7823);
  // Ten: 600000 / 292 = 2054, less 1000.
  for (int i = 2; i < 10; ++i) REQUIRE(economy.garrison_add(id, crew[i]));
  CHECK(wait() == 1054);
  // The constants are the world's: a faster base rate shortens every wait.
  CombatConstants faster = combat.constants();
  faster.catapult_base_fire_rate = 400;
  faster.catapult_add_fire_rate = 0;
  combat.set_constants(faster);
  CHECK(wait() == 500);
  // A catapult with no settlement behind it has no crew at all -- asked at
  // the shipped rates, where a crew of one would answer 14000.
  combat.set_constants(CombatConstants{});
  const ObjectId loose = b.world.spawn(NativeClass::catapult, &entity.value(), graph.find("RCatapult"));
  b.world.set_owner(loose, 1);
  b.world.set_health(loose, 400);
  const script::HostOutcome none =
      b.call(script::CallKind::member, "AttackWait", 0, {PredicateBench::obj(loose)});
  CHECK(none.status == script::HostStatus::ok);
  CHECK(none.value.is_integer() && none.value.as_integer() == 0);
  // An interval shorter than the animation is 0, never negative.
  faster.catapult_base_fire_rate = 1200;
  combat.set_constants(faster);
  CHECK(wait() == 0);
  // A handle to nothing is 0.
  const script::HostOutcome gone =
      b.call(script::CallKind::member, "AttackWait", 0, {PredicateBench::obj(999999)});
  CHECK(gone.status == script::HostStatus::ok);
  CHECK(gone.value.is_integer() && gone.value.as_integer() == 0);
}

/// **`ClearTowerTarget` takes the target off every `CatapultTower` heir in
/// the catapult's own settlement, and nothing else.** 0x004e37f0's walk over
/// the settlement roll at `+0x68`, the `IsHeirOf("CatapultTower")` test, and
/// the `0xffff` written to `[bld+0x1fc]`, which is `ui_target`.
TEST(clear_tower_target_clears_the_settlements_catapult_towers) {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="RCatapult" cpp_class="CVXCatapult"><properties sight="150" maxhealth="400" max_units="10"/></class>)"),
            "rcatapult.sc.xml");
  graph.add(bytes_of(R"(<class id="CatapultTower" cpp_class="CVXBuilding"><properties maxhealth="100"/></class>)"),
            "catapulttower.sc.xml");
  // A descendant, so that descent is measurable against the exact name.
  graph.add(bytes_of(R"(<class id="RCatapultTower" parent="CatapultTower" cpp_class="CVXBuilding"/>)"),
            "rcatapulttower.sc.xml");
  graph.add(bytes_of(R"(<class id="Fort" cpp_class="CVXBuilding"><properties maxhealth="100"/></class>)"),
            "fort.sc.xml");
  graph.link();

  PredicateBench b;
  b.world.set_class_graph(&graph);
  EconomySystem& economy = b.economy;
  const auto settle = [&](ObjectId anchor) -> SettlementId {
    const World::SettlementIds ids = b.world.spawn_settlement(1);
    SettlementInit init;
    init.settlement_object = ids.settlement;
    init.holder_object = ids.holder;
    init.warehouse_object = ids.warehouse;
    init.anchor = anchor;
    init.owner = 1;
    init.max_units = 10;
    const SettlementId id = economy.create(b.world, init);
    CHECK(id != kNoSettlement);
    return id;
  };
  const auto building = [&](SettlementId town, ClassIndex which, ObjectId target) -> ObjectId {
    const ObjectId id = b.world.spawn(NativeClass::building, nullptr, which);
    b.world.set_health(id, 100);
    CHECK(economy.add_building(b.world, town, id, 100));
    b.world.mutable_state(id)->ui_target = target;
    return id;
  };
  const ObjectId cat = b.world.spawn(NativeClass::catapult, nullptr, graph.find("RCatapult"));
  b.world.set_owner(cat, 1);
  b.world.set_health(cat, 400);
  const SettlementId mine = settle(cat);
  const ObjectId other_cat = b.world.spawn(NativeClass::catapult, nullptr, graph.find("RCatapult"));
  b.world.set_owner(other_cat, 1);
  b.world.set_health(other_cat, 400);
  const SettlementId theirs = settle(other_cat);
  const ObjectId prey = b.world.spawn(NativeClass::unit, nullptr, graph.find("Fort"));

  const ObjectId tower = building(mine, graph.find("CatapultTower"), prey);
  const ObjectId roman = building(mine, graph.find("RCatapultTower"), prey);
  const ObjectId fort = building(mine, graph.find("Fort"), prey);
  const ObjectId far = building(theirs, graph.find("CatapultTower"), prey);
  b.world.mutable_state(cat)->ui_target = prey;

  const auto clear = [&](script::Value receiver) {
    return b.call(script::CallKind::member, "ClearTowerTarget", 0, {receiver}).status ==
           script::HostStatus::ok;
  };
  CHECK(clear(PredicateBench::obj(cat)));
  CHECK(b.world.state(tower)->ui_target == kNoObject);
  CHECK(b.world.state(roman)->ui_target == kNoObject);   // an heir counts
  CHECK(b.world.state(fort)->ui_target == prey);          // not a tower
  CHECK(b.world.state(far)->ui_target == prey);           // another settlement
  CHECK(b.world.state(cat)->ui_target == prey);           // the catapult's own stays

  // A catapult with no settlement behind it has no towers; a handle to
  // nothing prints and returns. Neither refuses.
  const ObjectId loose = b.world.spawn(NativeClass::catapult, nullptr, graph.find("RCatapult"));
  CHECK(clear(PredicateBench::obj(loose)));
  CHECK(clear(PredicateBench::obj(999999)));
  CHECK(b.world.state(far)->ui_target == prey);
}

/// **`StartTraining` and `StopTraining` are one bit of the sync word, and not
/// the one the damage formula halves on.** 0x005d5de0 / 0x005d5e30 with
/// `0x10000000` through `vtbl+0x44` / `+0x48`, the `SyncFlags` pair.
TEST(start_and_stop_training_are_the_sync_words_training_bit) {
  PredicateBench b;
  const ObjectId unit = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const auto call = [&](const char* name, script::Value receiver) {
    return b.call(script::CallKind::member, name, 0, {receiver}).status == script::HostStatus::ok;
  };
  CHECK(!b.world.state(unit)->flags.training);
  CHECK(call("StartTraining", PredicateBench::obj(unit)));
  CHECK(b.world.state(unit)->flags.training);
  CHECK(!b.world.state(unit)->flags.half_damage);           // a training unit takes full damage
  CHECK((pack_sync_flags(b.world.state(unit)[0]) & kSyncTraining) != 0);  // and the dumps would show it
  CHECK(call("StartTraining", PredicateBench::obj(unit)));  // idempotent
  CHECK(b.world.state(unit)->flags.training);
  CHECK(call("StopTraining", PredicateBench::obj(unit)));
  CHECK(!b.world.state(unit)->flags.training);
  CHECK((pack_sync_flags(b.world.state(unit)[0]) & kSyncTraining) == 0);
  // A handle to nothing prints and returns, on both.
  CHECK(call("StartTraining", PredicateBench::obj(999999)));
  CHECK(call("StopTraining", script::Value::integer(3)));
  // The bit is on the packed word, so it is hashed and saved with the rest:
  // `test_save.cpp`'s flag fixture sets and clears it.
}

/// **An inn's state is one env int, open by default, clamped to 1..3, and its
/// own.** 0x00533ae0 / 0x00533c00: the `Inn` descent test, `/Inns/<n>/state`,
/// the read that answers 1 below 1, the write that clamps.
TEST(inn_state_is_one_env_int_per_inn_open_by_default_and_clamped) {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Inn" cpp_class="CVXBuilding"><properties maxhealth="100"/></class>)"),
            "inn.sc.xml");
  graph.add(bytes_of(R"(<class id="RInn" parent="Inn" cpp_class="CVXBuilding"/>)"), "rinn.sc.xml");
  graph.add(bytes_of(R"(<class id="Fort" cpp_class="CVXBuilding"><properties maxhealth="100"/></class>)"),
            "fort.sc.xml");
  graph.link();
  PredicateBench b;
  b.world.set_class_graph(&graph);
  EnvSystem env;
  REQUIRE(b.world.add_system(&env));
  env.start(b.world);
  const ObjectId inn = b.world.spawn(NativeClass::building, nullptr, graph.find("Inn"));
  const ObjectId roman = b.world.spawn(NativeClass::building, nullptr, graph.find("RInn"));
  const ObjectId fort = b.world.spawn(NativeClass::building, nullptr, graph.find("Fort"));

  const auto get = [&](script::Value receiver) {
    const script::HostOutcome out =
        b.call(script::CallKind::free_function, "GetInnState", 1, {receiver});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  const auto set = [&](script::Value receiver, std::int32_t state) {
    return b.call(script::CallKind::free_function, "SetInnState", 2,
                  {receiver, script::Value::integer(state)})
               .status == script::HostStatus::ok;
  };
  // Untouched: open.
  CHECK(get(PredicateBench::obj(inn)) == 1);
  CHECK(get(PredicateBench::obj(roman)) == 1);  // an heir is an inn
  // Written, clamped, and each inn's own.
  CHECK(set(PredicateBench::obj(inn), 3));
  CHECK(get(PredicateBench::obj(inn)) == 3);
  CHECK(get(PredicateBench::obj(roman)) == 1);
  CHECK(set(PredicateBench::obj(inn), 7));
  CHECK(get(PredicateBench::obj(inn)) == 3);
  CHECK(set(PredicateBench::obj(inn), 2));
  CHECK(get(PredicateBench::obj(inn)) == 2);
  CHECK(set(PredicateBench::obj(inn), 0));
  CHECK(get(PredicateBench::obj(inn)) == 1);
  CHECK(set(PredicateBench::obj(roman), -4));
  CHECK(get(PredicateBench::obj(roman)) == 1);
  // The write clamps on the way in, not the read on the way out: the store
  // holds 1, which only the store can show.
  CHECK(env.env().read_int(EnvScope::root(), "/Inns/" + std::to_string(roman) + "/state") == 1);
  // The key is the registry's own, under the root, so the store shows it.
  CHECK(set(PredicateBench::obj(roman), 3));
  CHECK(env.env().read_int(EnvScope::root(), "/Inns/" + std::to_string(roman) + "/state") == 3);
  // Not an inn, or not anything: 2 from the read, nothing from the write.
  CHECK(get(PredicateBench::obj(fort)) == 2);
  CHECK(set(PredicateBench::obj(fort), 3));
  CHECK(get(PredicateBench::obj(fort)) == 2);
  CHECK(env.env().read_int(EnvScope::root(), "/Inns/" + std::to_string(fort) + "/state") == 0);
  CHECK(get(PredicateBench::obj(999999)) == 2);
  CHECK(get(script::Value::integer(1)) == 2);
  CHECK(set(script::Value::integer(1), 3));
}

/// **`PartyQuery` is the party as a live query.** 0x005731e0 hands over the
/// party object's own handle; here that is a `QueryKind::party`, which
/// evaluates to `Party()`'s membership and follows it.
TEST(party_query_is_the_party_as_a_live_query) {
  PredicateBench b;
  const ObjectId first = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId second = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  const ObjectId third = b.world.spawn(NativeClass::unit, nullptr, b.classes.unit);
  b.world.mutable_state(third)->flags.in_party = true;
  b.world.mutable_state(first)->flags.in_party = true;

  const script::HostOutcome out = b.call(script::CallKind::free_function, "PartyQuery", 0, {});
  CHECK(out.status == script::HostStatus::ok);
  REQUIRE(out.value.is_object());
  CHECK(out.value.as_object().type == kTypeQuery);
  const ObjectId q = out.value.as_object().id;
  REQUIRE(q != kNoObject);
  std::vector<ObjectId> found;
  CHECK(b.world.evaluate_query(q, found) == 2);
  REQUIRE(found.size() == 2);
  CHECK(found[0] == first);   // ascending, as `Party()` lists them
  CHECK(found[1] == third);
  CHECK(second != first);
  // Live: a member who joins after the query was minted is in it.
  b.world.mutable_state(second)->flags.in_party = true;
  found.clear();
  CHECK(b.world.evaluate_query(q, found) == 3);
  // The runtime type is the executable's own name for it.
  CHECK(query_type_name(QueryKind::party) == "CVXPartyQuery");
  // No party: a valid, empty query rather than the invalid handle.
  for (const ObjectId id : {first, second, third}) b.world.mutable_state(id)->flags.in_party = false;
  const script::HostOutcome none = b.call(script::CallKind::free_function, "PartyQuery", 0, {});
  REQUIRE(none.status == script::HostStatus::ok && none.value.is_object());
  CHECK(none.value.as_object().type == kTypeQuery);
  found.clear();
  CHECK(b.world.evaluate_query(none.value.as_object().id, found) == 0);
}

namespace {

/// A mist and a cover, each declaring the reach the executable reads, and a
/// unit class for the combatants beneath them.
ClassGraph effect_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Mist" cpp_class="CVXScriptObj"><properties radius="100"/><properties range="200"/></class>)"),
            "mist.sc.xml");
  graph.add(bytes_of(R"(<class id="CoverOfMercy" cpp_class="CVXAreaEffect"><properties radius="300"/><properties range="200"/></class>)"),
            "coverofmercy.sc.xml");
  graph.add(bytes_of(R"(<class id="Unit" cpp_class="CVXUnit"><properties maxhealth="100" sight="100"/></class>)"),
            "unit.sc.xml");
  graph.add(bytes_of(R"(<class id="Fort" cpp_class="CVXBuilding"><properties maxhealth="100"/></class>)"),
            "fort.sc.xml");
  graph.link();
  return graph;
}

}  // namespace

/// **`MistAction` burns every unit within the mist's `range` that no other
/// live mist has tagged, tags it, and books the argument on both owners.**
/// 0x005ca050: the query over `[obj+0xd4]`, the unit flag, the tag at
/// `[unit+0x14c]`, `vtbl+0xa8` with the argument, and the two counters.
TEST(mist_action_burns_the_untagged_units_in_range_and_tags_them) {
  const ClassGraph graph = effect_graph();
  PredicateBench b;
  b.world.set_class_graph(&graph);
  CombatSystem combat;
  combat.set_world_bound(true);
  REQUIRE(b.world.add_system(&combat));
  MatchSystem match;
  REQUIRE(b.world.add_system(&match));
  CombatProfile profile;
  profile.max_health = 100;
  combat.set_profile(graph.find("Unit"), profile);

  const auto unit_at = [&](Point at, PlayerId owner) -> ObjectId {
    const ObjectId id = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
    b.world.set_position(id, at);
    b.world.set_owner(id, owner);
    b.world.set_health(id, 100);
    Combatant c;
    c.id = id;
    c.class_index = graph.find("Unit");
    c.owner = owner;
    c.health = 100;
    c.position = at;
    combat.add(c);
    return id;
  };
  const ObjectId mist = b.world.spawn(NativeClass::script_obj, nullptr, graph.find("Mist"));
  b.world.set_position(mist, Point{0, 0});
  b.world.set_owner(mist, 1);
  const ObjectId other = b.world.spawn(NativeClass::script_obj, nullptr, graph.find("Mist"));
  b.world.set_position(other, Point{5000, 5000});
  const ObjectId inside = unit_at(Point{150, 0}, 2);
  const ObjectId edge = unit_at(Point{0, 199}, 2);
  const ObjectId outside = unit_at(Point{300, 0}, 2);    // past `range`, inside nothing
  const ObjectId taken = unit_at(Point{0, 100}, 2);      // another live mist's
  const ObjectId freed = unit_at(Point{100, 100}, 3);    // a dead mist's
  b.world.mutable_state(taken)->mist = other;
  b.world.mutable_state(freed)->mist = 424242;
  const ObjectId fort = b.world.spawn(NativeClass::building, nullptr, graph.find("Fort"));
  b.world.set_position(fort, Point{50, 50});
  b.world.set_health(fort, 100);

  const auto act = [&](script::Value receiver, std::int32_t damage) {
    return b.call(script::CallKind::member, "MistAction", 1, {receiver, script::Value::integer(damage)})
               .status == script::HostStatus::ok;
  };
  CHECK(act(PredicateBench::obj(mist), 30));
  CHECK(combat.health(inside) == 70);
  CHECK(b.world.state(inside)->health == 70);     // published, as `Obj::Damage` publishes
  CHECK(b.world.state(inside)->mist == mist);
  CHECK(combat.health(edge) == 70);
  CHECK(combat.health(outside) == 100);
  CHECK(b.world.state(outside)->mist == kNoObject);
  CHECK(combat.health(taken) == 100);             // the other mist's
  CHECK(b.world.state(taken)->mist == other);
  CHECK(combat.health(freed) == 70);              // its mist is gone
  CHECK(b.world.state(freed)->mist == mist);
  CHECK(b.world.state(fort)->health == 100);      // not a unit
  // The argument is what the counters take: three units, 30 each.
  CHECK(match.score(1).damage_inflicted == 90);
  CHECK(match.score(2).damage_taken == 60);
  CHECK(match.score(3).damage_taken == 30);
  // Its own units burn again on the next animation; the other mist's still do not.
  CHECK(act(PredicateBench::obj(mist), 30));
  CHECK(combat.health(inside) == 40);
  CHECK(combat.health(taken) == 100);
  // Once the other mist is gone its unit is free to take.
  REQUIRE(b.world.despawn(other));
  CHECK(act(PredicateBench::obj(mist), 30));
  CHECK(combat.health(taken) == 70);
  CHECK(b.world.state(taken)->mist == mist);
  // A handle to nothing does nothing.
  CHECK(act(PredicateBench::obj(999999), 30));
  CHECK(combat.health(inside) == 10);
}

/// **A cover of mercy raises the half-damage bit on every friendly unit
/// under it and lowers it on those who stepped out.** 0x005ca5a0: the old
/// list lowered, the player query over the complement of the owner's war
/// mask, the class `radius`, the new list raised.
TEST(cover_of_mercy_action_shelters_the_friendly_units_under_it) {
  const ClassGraph graph = effect_graph();
  PredicateBench b;
  b.world.set_class_graph(&graph);
  // Player 1 is at peace with 2 and at war with 3 -- the one-way row
  // `Obj::IsEnemy` reads.
  b.world.players().set(1, 2, Relation::ceasefire, true);
  const auto unit_at = [&](Point at, PlayerId owner) -> ObjectId {
    const ObjectId id = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
    b.world.set_position(id, at);
    b.world.set_owner(id, owner);
    return id;
  };
  const ObjectId cover = b.world.spawn(NativeClass::area_effect, nullptr, graph.find("CoverOfMercy"));
  b.world.set_position(cover, Point{0, 0});
  b.world.set_owner(cover, 1);
  const ObjectId own = unit_at(Point{100, 0}, 1);
  const ObjectId ally = unit_at(Point{0, 200}, 2);
  const ObjectId foe = unit_at(Point{50, 50}, 3);
  const ObjectId nobodys = unit_at(Point{-100, 0}, kNoPlayer);
  const ObjectId far = unit_at(Point{400, 0}, 1);
  // Between the class's `range` (200) and its `radius` (300): the reach is
  // the radius, and this is the unit that says so.
  const ObjectId ring = unit_at(Point{250, 0}, 1);
  const ObjectId fort = b.world.spawn(NativeClass::building, nullptr, graph.find("Fort"));
  b.world.set_position(fort, Point{10, 10});
  b.world.set_owner(fort, 1);

  const auto act = [&](ObjectId who) {
    return b.call(script::CallKind::member, "CoverOfMercyAction", 0, {PredicateBench::obj(who)})
               .status == script::HostStatus::ok;
  };
  const auto sheltered = [&](ObjectId id) { return b.world.state(id)->flags.half_damage; };
  CHECK(act(cover));
  CHECK(sheltered(own));
  CHECK(sheltered(ally));
  CHECK(sheltered(nobodys));
  CHECK(sheltered(ring));
  CHECK(!sheltered(foe));
  CHECK(!sheltered(far));
  CHECK(!sheltered(fort));
  CHECK(b.world.state(own)->sheltered_by == cover);
  CHECK(b.world.state(foe)->sheltered_by == kNoObject);

  // Step out, and the next animation lowers it; step in, and it is raised.
  REQUIRE(b.world.set_position(own, Point{400, 0}));
  REQUIRE(b.world.set_position(far, Point{0, -100}));
  CHECK(act(cover));
  CHECK(!sheltered(own));
  CHECK(b.world.state(own)->sheltered_by == kNoObject);
  CHECK(sheltered(far));
  CHECK(sheltered(ally));

  // A training unit under a cover keeps its training bit when it steps out
  // and loses only the shelter: two bits on two words, each with one writer.
  REQUIRE(b.call(script::CallKind::member, "StartTraining", 0, {PredicateBench::obj(ally)}).status ==
          script::HostStatus::ok);
  REQUIRE(b.world.set_position(ally, Point{0, 900}));
  CHECK(act(cover));
  CHECK(!sheltered(ally));
  CHECK(b.world.state(ally)->flags.training);

  // Two covers over one unit: the tag follows the last to act, and the first
  // no longer lowers what it did not last raise -- the labelled reading.
  const ObjectId second = b.world.spawn(NativeClass::area_effect, nullptr, graph.find("CoverOfMercy"));
  b.world.set_position(second, Point{0, -50});
  b.world.set_owner(second, 1);
  CHECK(act(second));
  CHECK(b.world.state(far)->sheltered_by == second);
  REQUIRE(b.world.set_position(far, Point{0, -320}));  // under `second` only
  CHECK(act(cover));
  CHECK(sheltered(far));
  CHECK(b.world.state(far)->sheltered_by == second);

  // A handle to nothing does nothing.
  CHECK(act(999999));
  CHECK(sheltered(far));
}

namespace {

/// A unit class with a reach, a sight and a blow; a peasant that cannot hit;
/// and a summoned thing with a stamina clock -- what the training and
/// summoning entry points read off the class.
ClassGraph training_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Unit" cpp_class="CVXUnit"><properties maxhealth="100" sight="500" range="30" min_range="20" damage="10"/></class>)"),
            "unit.sc.xml");
  graph.add(bytes_of(R"(<class id="Peasant" cpp_class="CVXUnit"><properties maxhealth="100" sight="500" damage="0"/></class>)"),
            "peasant.sc.xml");
  graph.add(bytes_of(R"(<class id="Mist" cpp_class="CVXScriptObj"><properties stamina_dec_time="1200"/></class>)"),
            "mist.sc.xml");
  graph.add(bytes_of(R"(<class id="Sour" cpp_class="CVXScriptObj"><properties stamina_dec_time="-5"/></class>)"),
            "sour.sc.xml");
  graph.link();
  return graph;
}

}  // namespace

/// **`BestTrainingTarget` is the nearest training unit in sight, scored, and an
/// armed enemy ends the search.** 0x005d3cb0's predicate under 0x005d4cd0's
/// sweep over the unit's own sight.
TEST(best_training_target_is_the_nearest_training_unit_unless_an_enemy_is_armed) {
  const ClassGraph graph = training_graph();
  PredicateBench b;
  b.world.set_class_graph(&graph);
  b.world.players().set(1, 2, Relation::ceasefire, true);  // 2 is a friend, 3 an enemy
  const auto unit_at = [&](Point at, PlayerId owner, const char* cls = "Unit",
                           NativeClass native = NativeClass::unit) -> ObjectId {
    const ObjectId id = b.world.spawn(native, nullptr, graph.find(cls));
    b.world.set_position(id, at);
    b.world.set_owner(id, owner);
    b.world.set_health(id, 100);
    return id;
  };
  const auto best = [&](ObjectId who) -> ObjectId {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "BestTrainingTarget", 0, {PredicateBench::obj(who)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() && out.value.as_object().valid() ? out.value.as_object().id
                                                                  : kNoObject;
  };
  const auto train = [&](ObjectId who) { b.world.mutable_state(who)->flags.training = true; };
  // The class: `range` 30, `min_range` 20, `sight` 500. Every term of the score
  // is arranged to decide one answer below, and the sweep is in id order, so
  // the order of spawning is part of every arrangement.

  const ObjectId self = unit_at(Point{0, 0}, 1);
  train(self);
  // Alone, or beside a unit that is not training: nothing -- and never self.
  const ObjectId idle = unit_at(Point{100, 0}, 1);
  CHECK(best(self) == kNoObject);
  // A training unit is a partner; the nearer of two wins; a friend's counts.
  const ObjectId far_partner = unit_at(Point{300, 0}, 1);
  train(far_partner);
  CHECK(best(self) == far_partner);
  // A tie keeps the one met first.
  const ObjectId twin = unit_at(Point{-300, 0}, 1);
  train(twin);
  CHECK(best(self) == far_partner);
  CHECK(twin != far_partner);
  const ObjectId near_partner = unit_at(Point{0, 120}, 2);
  train(near_partner);
  CHECK(best(self) == near_partner);
  // A hero costs 100: a training hero at 50 scores 150 and loses to 120.
  const ObjectId hero_out = unit_at(Point{50, 0}, 1, "Unit", NativeClass::hero);
  train(hero_out);
  CHECK(b.world.state(hero_out)->flags.is_hero);
  CHECK(best(self) == near_partner);
  // A unit on the move costs 80: 120 walking is 200, and the hero's 150 wins.
  b.world.mutable_state(near_partner)->flags.has_active_path = true;
  CHECK(best(self) == hero_out);
  b.world.mutable_state(near_partner)->flags.has_active_path = false;
  // Out of sight is out of the sweep.
  const ObjectId beyond = unit_at(Point{600, 0}, 1);
  train(beyond);
  CHECK(best(self) == near_partner);
  // Within reach takes 100 off, once the score is 50 or more: a hero at 25
  // is 125, then 25, and beats the 120 it would otherwise lose to.
  const ObjectId hero_in = unit_at(Point{0, 25}, 1, "Unit", NativeClass::hero);
  train(hero_in);
  CHECK(best(self) == hero_in);
  // Inside `min_range` costs 40: a partner at 15 is 55, not under 50, and the
  // partner at 28 met after it is under 50 and taken at once. Without the 40
  // the one at 15 would have been the shortcut, being met first.
  const ObjectId inner = unit_at(Point{15, 0}, 1);
  train(inner);
  CHECK(best(self) == inner);  // -45 beats 25
  const ObjectId competitor = unit_at(Point{0, 28}, 1);
  train(competitor);
  CHECK(best(self) == competitor);
  // An enemy that cannot hit changes nothing; an armed one ends the sweep
  // where it is met and is what comes back -- after the shortcut here, since
  // both end the sweep where they are met and the partner has the lower id.
  const ObjectId harmless = unit_at(Point{200, 200}, 3, "Peasant");
  CHECK(best(self) == competitor);
  const ObjectId foe = unit_at(Point{400, 0}, 3);
  CHECK(best(self) == competitor);
  REQUIRE(b.world.set_position(competitor, Point{5000, 0}));
  REQUIRE(b.world.set_position(inner, Point{5000, 100}));
  CHECK(best(self) == foe);  // met before the rest, hero_in included
  REQUIRE(b.world.set_position(foe, Point{5000, 200}));
  CHECK(best(self) == hero_in);
  (void)idle;
  (void)harmless;
  // A handle to nothing prints and answers the invalid handle.
  CHECK(best(999999) == kNoObject);
}

/// **`TrainAttack` is one swing's wait and then true, after three refusals.**
/// 0x005d5d10: the held position, the partner that does not resolve, the
/// receiver at one point of health; then action state 2 and the interpreter's
/// suspend-until-clear.
TEST(train_attack_waits_one_swing_and_answers_true) {
  const Result<imperivm::core::Entity> entity = imperivm::core::Entity::parse(bytes_of(kCatapultEntityXml));
  REQUIRE(entity.ok());
  const ClassGraph graph = training_graph();
  PredicateBench b;
  b.world.set_class_graph(&graph);
  const ObjectId self = b.world.spawn(NativeClass::unit, &entity.value(), graph.find("Unit"));
  b.world.set_position(self, Point{0, 0});
  b.world.set_health(self, 100);
  const ObjectId partner = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  b.world.set_position(partner, Point{20, 0});
  b.world.set_health(partner, 100);
  const ObjectId bare = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  b.world.set_position(bare, Point{40, 0});
  b.world.set_health(bare, 100);

  const auto call = [&](ObjectId who, script::Value other, bool first_call) -> script::HostOutcome {
    const std::uint32_t index = b.registry.find(script::CallKind::member, "TrainAttack", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    std::vector<script::Value> args{PredicateBench::obj(who), other};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &b.context;
    ctx.name = "TrainAttack";
    ctx.kind = script::CallKind::member;
    ctx.first_call = first_call;
    return b.registry.entry(index).fn(ctx);
  };
  const auto truth = [&](const script::HostOutcome& out) {
    return out.status == script::HostStatus::ok && out.value.truthy_scalar();
  };
  // The swing: the entity's one-second attack, then true.
  script::HostOutcome out = call(self, PredicateBench::obj(partner), true);
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 1000);
  CHECK(truth(call(self, PredicateBench::obj(partner), false)));
  // No attack animation to wait on: still a suspend, to the next pass, and
  // then true. Answering true at once let `UNIT_TRAIN.VS`'s loop run its
  // instruction budget out (236 times on Balcans in 4,000 turns).
  out = call(bare, PredicateBench::obj(partner), true);
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 0);
  CHECK(truth(call(bare, PredicateBench::obj(partner), false)));
  // The three refusals, none of them a trap.
  out = call(self, PredicateBench::obj(999999), true);
  CHECK(out.status == script::HostStatus::ok && !out.value.truthy_scalar());
  b.world.set_health(self, 1);
  out = call(self, PredicateBench::obj(partner), true);
  CHECK(out.status == script::HostStatus::ok && !out.value.truthy_scalar());
  b.world.set_health(self, 2);
  CHECK(call(self, PredicateBench::obj(partner), true).status == script::HostStatus::retry);
  REQUIRE(b.world.set_position(self, kHeldPosition));
  out = call(self, PredicateBench::obj(partner), true);
  CHECK(out.status == script::HostStatus::ok && !out.value.truthy_scalar());
  out = call(999999, PredicateBench::obj(partner), true);
  CHECK(out.status == script::HostStatus::ok && !out.value.truthy_scalar());
}

/// **`GetStaminaDecTime` is the class's `stamina_dec_time`, floored at 0, and
/// `MagicActionEnd` releases every unit the receiver was burning.**
TEST(get_stamina_dec_time_and_magic_action_end_read_and_release) {
  const ClassGraph graph = training_graph();
  PredicateBench b;
  b.world.set_class_graph(&graph);
  const ObjectId mist = b.world.spawn(NativeClass::script_obj, nullptr, graph.find("Mist"));
  const ObjectId sour = b.world.spawn(NativeClass::script_obj, nullptr, graph.find("Sour"));
  const ObjectId plain = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  const auto dec = [&](script::Value who) -> std::int32_t {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "GetStaminaDecTime", 0, {who});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  CHECK(dec(PredicateBench::obj(mist)) == 1200);
  CHECK(dec(PredicateBench::obj(sour)) == 0);   // negative reads 0
  CHECK(dec(PredicateBench::obj(plain)) == 0);  // undeclared reads 0
  CHECK(dec(PredicateBench::obj(999999)) == 0);

  const ObjectId other = b.world.spawn(NativeClass::script_obj, nullptr, graph.find("Mist"));
  const ObjectId a = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  const ObjectId c = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  const ObjectId d = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  b.world.mutable_state(a)->mist = mist;
  b.world.mutable_state(c)->mist = mist;
  b.world.mutable_state(d)->mist = other;
  CHECK(b.call(script::CallKind::member, "MagicActionEnd", 0, {PredicateBench::obj(mist)}).status ==
        script::HostStatus::ok);
  CHECK(b.world.state(a)->mist == kNoObject);
  CHECK(b.world.state(c)->mist == kNoObject);
  CHECK(b.world.state(d)->mist == other);  // another mist's
  CHECK(b.call(script::CallKind::member, "MagicActionEnd", 0, {PredicateBench::obj(999999)}).status ==
        script::HostStatus::ok);
  CHECK(b.world.state(d)->mist == other);
}

/// **A starving army is a local-player unit the feeder has found at zero food
/// for at least the asked age.** 0x00559c20 / 0x00559d50 over the `army
/// starving` records the food tick posts; here the feeder's own stamp.
TEST(has_starving_army_reads_the_feeders_hunger_stamp_for_the_local_player) {
  const ClassGraph graph = training_graph();
  PredicateBench b;
  b.world.set_class_graph(&graph);
  FeederSystem feeder;
  feeder.set_world_bound(false);
  feeder.set_draws_from_warehouse(false);
  REQUIRE(b.world.add_system(&feeder));
  const auto soldier = [&](PlayerId owner, std::int32_t food) -> ObjectId {
    const ObjectId id = b.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
    b.world.set_position(id, Point{100, 100});
    b.world.set_owner(id, owner);
    b.world.set_health(id, 100);
    FeedingUnit link;
    link.unit = id;
    link.food = food;
    link.max_food = 20;
    link.max_health = 100;
    feeder.enrol(link);
    return id;
  };
  const ObjectId mine = soldier(1, 0);
  const ObjectId theirs = soldier(2, 0);
  const ObjectId fed = soldier(1, 20);
  feeder.start(b.world);
  b.context.local_player = 1;

  const auto has = [&](std::int64_t ms) -> int {
    const script::HostOutcome out = b.call(script::CallKind::free_function, "HasStarvingArmy", 1,
                                           {script::Value::integer(ms)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? (out.value.as_integer() != 0 ? 1 : 0) : -1;
  };
  const auto where = [&](std::int64_t ms) -> ObjectId {
    const script::HostOutcome out = b.call(script::CallKind::free_function, "StarvingArmyPos", 1,
                                           {script::Value::integer(ms)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() && out.value.as_object().valid() ? out.value.as_object().id
                                                                  : kNoObject;
  };
  // Nothing has ticked: no stamp, no army. The stamp lands on the food
  // cursor's pass, one per `DropFoodByOneIvl` (10,000 ms) over the chain --
  // the same tick the original posts from -- so 12,000 ms sees every unit.
  CHECK(has(0) == 0);
  CHECK(where(0) == kNoObject);
  for (int i = 0; i < 30; ++i) b.world.advance(400);  // 12,000 ms
  const FeedingUnit* link = feeder.find(mine);
  REQUIRE(link != nullptr);
  CHECK(link->hungry_since > 0);
  CHECK(link->hungry_since <= 12000);
  const GameTime since = link->hungry_since;
  // The stamp is the *first* tick that found the unit at zero: later ticks
  // leave it alone, which is what makes the age grow.
  for (int i = 0; i < 30; ++i) b.world.advance(400);
  CHECK(link->hungry_since == since);
  const GameTime age = b.world.time() - since;
  CHECK(has(age) == 1);
  CHECK(has(age + 1) == 0);
  CHECK(where(age) == mine);       // the local player's, not player 2's
  CHECK(where(age + 1) == kNoObject);
  CHECK(feeder.find(fed)->hungry_since == 0);
  // Another local player sees the other army.
  b.context.local_player = 2;
  CHECK(where(0) == theirs);
  b.context.local_player = kNoPlayer;
  CHECK(has(0) == 0);
  b.context.local_player = 1;
  // Fed again, the stamp is gone; hungry again, it is a fresh one.
  REQUIRE(feeder.set_food(mine, 5));
  CHECK(feeder.find(mine)->hungry_since == 0);
  CHECK(has(0) == 0);
  REQUIRE(feeder.set_food(mine, 0));
  for (int i = 0; i < 30; ++i) b.world.advance(400);
  CHECK(feeder.find(mine)->hungry_since > since);
  CHECK(has(0) == 1);
}

TEST(rotate_to_turns_the_catapult_at_once_and_finishes) {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="RCatapult" cpp_class="CVXCatapult"><properties sight="150" maxhealth="400"/></class>)"),
            "rcatapult.sc.xml");
  graph.link();
  PredicateBench b;
  b.world.set_class_graph(&graph);
  MovementSystem movement;
  REQUIRE(b.world.add_system(&movement));
  const ObjectId cat = b.world.spawn(NativeClass::catapult, nullptr, graph.find("RCatapult"));
  b.world.set_position(cat, Point{1000, 1000});
  b.world.set_owner(cat, 1);
  b.world.set_health(cat, 400);
  const Point before = movement.state(cat).facing;

  const auto rotate = [&](Point to) {
    return b.call(script::CallKind::member, "RotateTo", 1, {PredicateBench::obj(cat), pack_point(to)});
  };
  // A turn: finishes, and the facing now points east.
  const script::HostOutcome east = rotate(Point{3000, 1000});
  CHECK(east.status == script::HostStatus::ok);
  CHECK(movement.state(cat).facing.x > 0);
  CHECK(movement.state(cat).facing.y == 0);
  const Point after = movement.state(cat).facing;
  CHECK(!(after == before));
  // The catapult's own position is not a turn.
  CHECK(rotate(Point{1000, 1000}).status == script::HostStatus::ok);
  CHECK(movement.state(cat).facing == after);
  // A handle to nothing finishes too.
  CHECK(b.call(script::CallKind::member, "RotateTo", 1,
               {PredicateBench::obj(999999), script::Value::object(script::ObjectRef{kTypePoint, 0})})
            .status == script::HostStatus::ok);
}

// ==========================================================================
// the mutation
// ==========================================================================

namespace {

/// Two unit classes that differ in every class-derived figure, so that what is
/// carried and what is re-read can be told apart in one call.
struct MutateBench {
  ClassGraph graph;
  World world;
  CombatSystem combat;
  HeroSystem heroes;
  CommandSystem commands;
  script::HostRegistry registry;
  HostContext context;
  ClassIndex rider = kNoClass;
  ClassIndex archer = kNoClass;

  MutateBench() {
    const std::string docs[] = {
        R"(<class id="Military" cpp_class="CVXUnit"/>)",
        R"(<class id="TTeutonRider" parent="Military" cpp_class="CVXUnit"><properties
             sight="400" maxhealth="200"/></class>)",
        // An `altid`, because the original resolves a class name by `id` and
        // then by `altid` and nothing else here would tell the two apart.
        R"(<class id="TTeutonArcher" parent="Military" cpp_class="CVXUnit" altid="Teuton Archer"><properties
             sight="700" maxhealth="100"/></class>)",
        // A class the graph knows and the native resolver does not, which is
        // the second of the two refusals.
        R"(<class id="Nothing" cpp_class="CVXNoSuchThing"/>)",
    };
    const char* names[] = {"military.sc.xml", "rider.sc.xml", "archer.sc.xml", "nothing.sc.xml"};
    for (int i = 0; i < 4; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    world.set_class_graph(&graph);
    rider = graph.find("TTeutonRider");
    archer = graph.find("TTeutonArcher");
    combat.set_class_graph(&graph);
    combat.set_world_bound(true);
    REQUIRE(world.add_system(&combat));
    REQUIRE(world.add_system(&heroes));
    REQUIRE(world.add_system(&commands));
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  ObjectId unit(ClassIndex which, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, which);
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, 150));
    CHECK(world.set_position(id, Point{300, 400}));
    return id;
  }

  void arm() {
    CombatProfile p;
    p.damage = 30;
    p.damage_type = DamageType::slash;
    p.max_health = 200;
    p.range = 17;
    p.attack_interval = 1000;
    combat.set_profile(rider, p);
    p.damage = 10;
    p.max_health = 100;
    combat.set_profile(archer, p);
    combat.start(world);
  }

  /// `u.Mutate(name)`, through the registry.
  void mutate(ObjectId id, const char* name) {
    const std::uint32_t index = registry.find(script::CallKind::member, "Mutate", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return;
    std::vector<script::Value> args{script::Value::object(kTypeObj, id),
                                    script::Value::string(name)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "Mutate";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
  }
};

}  // namespace

/// The object becomes one of the named class **and keeps its id**, which is
/// what the original's handle swap is for: every script reference, every group
/// and every id-keyed record still names the survivor.
TEST(mutate_keeps_the_id_and_re_reads_everything_the_class_decides) {
  MutateBench b;
  const ObjectId who = b.unit(b.rider, 1);
  b.arm();
  const ObjectId named = who;
  REQUIRE(b.world.named_objects().bind("Konrad", who));
  REQUIRE(b.world.find(who)->sight == 400);

  b.mutate(who, "TTeutonArcher");

  const WorldObject* slot = b.world.find(who);
  REQUIRE(slot != nullptr);
  CHECK(slot->id == named);
  CHECK(slot->class_index == b.archer);
  CHECK(slot->object != nullptr);
  CHECK(slot->object->class_index == b.archer);
  // Sight is a class property re-read at spawn, so it is the new class's.
  CHECK(slot->sight == 700);
  // And the name binding is by id, so it survives without being copied.
  CHECK(b.world.named_object("Konrad") == who);
}

/// The field list the copy carries, and the one field beside it that it does
/// not: `user` is `[obj+0x164]` and is nowhere in the copy.
TEST(mutate_carries_health_stamina_position_owner_and_two_flag_bits) {
  MutateBench b;
  const ObjectId who = b.unit(b.rider, 2);
  b.arm();
  ObjectState* state = b.world.mutable_state(who);
  REQUIRE(state != nullptr);
  state->stamina = 45;
  state->user = 7;
  state->flags.in_party = true;
  state->flags.messenger = true;
  state->flags.cursed = true;
  state->flags.hidden = true;

  b.mutate(who, "TTeutonArcher");

  const WorldObject* slot = b.world.find(who);
  REQUIRE(slot != nullptr);
  CHECK(slot->state.position == (Point{300, 400}));
  CHECK(slot->state.owner == 2);
  CHECK(slot->state.stamina == 45);
  // Health carries whole, and it is now **above** the new class's maximum of
  // 100 -- the copy does not clamp and neither does this.
  CHECK(slot->state.health == 150);
  CHECK(b.combat.profile(slot->class_index).max_health == 100);
  // Bit 19 and bit 26 of the `+0x2c` word, and only those two.
  CHECK(slot->state.flags.in_party);
  CHECK(slot->state.flags.messenger);
  CHECK(!slot->state.flags.cursed);
  CHECK(!slot->state.flags.hidden);
  // And the object is still a unit, because the new class is one.
  CHECK(slot->state.flags.is_unit);
  // `user` is not in the copy list.
  CHECK(slot->state.user == 0);
}

/// The squad is cleared outright, the commands go with the object that had
/// them, and the combatant is re-classed rather than replaced -- so what it has
/// learnt survives and what it was doing does not.
TEST(mutate_clears_the_squad_and_the_commands_and_re_classes_the_combatant) {
  MutateBench b;
  const ObjectId who = b.unit(b.rider, 1);
  const ObjectId prey = b.unit(b.archer, 2);
  b.arm();

  const SquadKey key = b.heroes.squads().create(1, who);
  REQUIRE(b.heroes.squads().squad_of(who) == key);
  Command order;
  (void)b.commands.add_command(b.world, who, false, "move", order);
  REQUIRE(b.commands.command_count(who) > 0);

  Combatant* before = const_cast<Combatant*>(b.combat.find(who));
  REQUIRE(before != nullptr);
  before->experience = 4200;
  before->level = 5;
  before->target = prey;
  before->attacks = 3;

  b.mutate(who, "TTeutonArcher");

  CHECK(!b.heroes.squads().squad_of(who).valid());
  CHECK(b.commands.command_count(who) == 0);
  const Combatant* after = b.combat.find(who);
  REQUIRE(after != nullptr);
  CHECK(after->class_index == b.archer);
  // `[obj+0x180]` is one of the copied fields, so a converted Teuton keeps
  // what it has learnt; a combatant rebuilt from scratch would start at 1.
  CHECK(after->experience == 4200);
  CHECK(after->level == 5);
  // And everything the destroyed object was in the middle of is gone.
  CHECK(after->target == kNoObject);
  CHECK(after->attacks == 0);
}

/// Both refusals are messages in the original and changes nothing here: a name
/// no class answers to, and a class whose `cpp_class` names no native class.
TEST(mutate_refuses_a_name_no_class_answers_to) {
  MutateBench b;
  const ObjectId who = b.unit(b.rider, 1);
  b.arm();

  // A refusal leaves everything alone, and "everything" includes the three
  // things the caller puts right on a success.
  const SquadKey key = b.heroes.squads().create(1, who);
  Command order;
  (void)b.commands.add_command(b.world, who, false, "move", order);
  REQUIRE(b.commands.command_count(who) > 0);

  b.mutate(who, "TTeutonHorseWizard");
  CHECK(b.world.find(who)->class_index == b.rider);
  CHECK(b.world.find(who)->sight == 400);
  CHECK(b.heroes.squads().squad_of(who) == key);
  CHECK(b.commands.command_count(who) > 0);

  // A class the graph knows whose `cpp_class` names no native class is the
  // second refusal, and it is refused just as completely.
  b.mutate(who, "Nothing");
  CHECK(b.world.find(who)->class_index == b.rider);
  CHECK(b.heroes.squads().squad_of(who) == key);
  CHECK(b.commands.command_count(who) > 0);

  // And a receiver that is not there at all.
  b.mutate(kNoObject, "TTeutonArcher");
  CHECK(b.world.find(who)->class_index == b.rider);

  // The name is resolved by `id` and then by `altid`, so the alias works.
  b.mutate(who, "Teuton Archer");
  CHECK(b.world.find(who)->class_index == b.archer);
}

/// **`.maxhealth` is the object's number, not its class's**, and the split
/// between the two is the whole reason the object model asks the combat system
/// rather than answering from the class graph on its own.
///
/// The original has no split: `[obj+0xc8]` is filled from the class at spawn
/// and rebuilt from the class plus `Unit::AddBonus`'s record on every
/// recalculation, and every one of the 109 readers takes it from there. Here
/// the class property is right for anything with no record -- a building, a
/// wagon, an item holder, and every unit a script has left alone.
TEST(sim_max_health_reads_the_bonus_a_script_put_on_the_object) {
  TinyGraph classes;
  World world;
  CombatSystem combat;
  combat.set_class_graph(&classes.graph);
  world.set_class_graph(&classes.graph);
  REQUIRE(world.add_system(&combat));

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const std::uint32_t index = registry.find(script::CallKind::member, "maxhealth", 0);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);

  const auto max_health_of = [&](ObjectId id) -> std::int32_t {
    std::vector<script::Value> args{script::Value::object(kTypeObj, id)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "maxhealth";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr, classes.soldier);
  const ObjectId fort = world.spawn(NativeClass::building, nullptr, classes.building);
  Combatant c;
  c.id = soldier;
  c.class_index = classes.soldier;
  c.health = 80;
  combat.add(c);

  CHECK(max_health_of(soldier) == 80);   // `Soldier` declares 80
  CHECK(max_health_of(fort) == 5000);    // `Fort` declares 5000, and has no row

  StatBonus put;
  put.max_health = 45;
  CHECK(combat.add_bonus(soldier, put));
  CHECK(max_health_of(soldier) == 125);
  // The building was never a combatant, so nothing about it moved.
  CHECK(max_health_of(fort) == 5000);
  // Nor did the two numbers get crossed: the class property is still 80.
  CHECK(combat.profile(classes.soldier).max_health == 80);
}
