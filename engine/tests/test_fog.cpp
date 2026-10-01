// The explored map. See include/imperivm/core/sim/fog.hpp.
//
// Synthetic throughout: the grid is arithmetic and the entry points are five
// small functions over it, so nothing here needs a byte of game data.

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace script = imperivm::core::script;
using imperivm::core::ClassGraph;
using imperivm::core::NativeClass;

namespace {

constexpr Point pt(std::int32_t x, std::int32_t y) noexcept { return Point{x, y}; }

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(static_cast<std::byte>(c));
  return out;
}

/// A world with a match (which is where the map extent comes from), an area
/// table and the fog system, plus the whole host surface.
struct Bench {
  script::HostRegistry registry;
  World world;
  MatchSystem match;
  AreaSystem areas;
  FogSystem fog;
  HostContext context;

  explicit Bench(std::int32_t extent = 8192) {
    script::declare_shipped_surface(registry);
    (void)register_all_hosts(registry);
    MatchRules rules;
    rules.map_size = extent;
    match.configure(rules, /*human=*/0, /*multiplayer=*/false);
    REQUIRE(world.add_system(&match));
    REQUIRE(world.add_system(&areas));
    REQUIRE(world.add_system(&fog));
    context.world = &world;
    context.object_type = kTypeObj;
    world.start();
  }

  script::HostOutcome call(script::CallKind kind, const char* name, std::uint16_t arity,
                           std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(kind, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost || registry.entry(index).fn == nullptr) {
      return script::HostOutcome::failed("not implemented");
    }
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return registry.entry(index).fn(ctx);
  }

  bool is_explored(Point where, std::int32_t player) {
    const script::HostOutcome out =
        call(script::CallKind::free_function, "IsExplored", 2,
             {pack_point(where), script::Value::integer(player)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }
};

}  // namespace

// --------------------------------------------------------------------------
// the grid
// --------------------------------------------------------------------------

TEST(fog_a_cell_is_a_thousand_and_twenty_four_units_square) {
  ExplorationMap map;
  map.resize(8192);
  CHECK(map.cells() == 8);

  // `y >> 10` and `x >> 11` with a half-word select is 1024 on both axes.
  map.explore(pt(0, 0), 0);
  CHECK(map.explored(pt(0, 0), 0));
  CHECK(map.explored(pt(1023, 1023), 0));
  CHECK(!map.explored(pt(1024, 0), 0));
  CHECK(!map.explored(pt(0, 1024), 0));

  // The two cells that share a `dword` in the original are the two that differ
  // in bit 10 of x, so they must not alias.
  map.explore(pt(1024, 0), 3);
  CHECK(map.explored(pt(1024, 0), 3));
  CHECK(!map.explored(pt(0, 0), 3));
  CHECK(!map.explored(pt(1024, 0), 0));

  // An extent that is not a multiple of the cell size still has its far edge.
  ExplorationMap ragged;
  ragged.resize(8193);
  CHECK(ragged.cells() == 9);
}

TEST(fog_the_slots_are_eight_and_the_ninth_player_has_no_fog_at_all) {
  ExplorationMap map;
  map.resize(4096);

  // Sixteen players, eight slots. 0x00515958 short-circuits **true** for a slot
  // at or past the eighth, so player 9 sees the whole board and player 8 does
  // not -- and that asymmetry is the point.
  for (std::int32_t slot = 0; slot < ExplorationMap::kSlots; ++slot) {
    CHECK(!map.explored(pt(2000, 2000), slot));
  }
  for (std::int32_t slot = ExplorationMap::kSlots; slot < 16; ++slot) {
    CHECK(map.explored(pt(2000, 2000), slot));
  }

  // A slot is one player's and nobody else's.
  map.explore(pt(2000, 2000), 5);
  CHECK(map.explored(pt(2000, 2000), 5));
  CHECK(!map.explored(pt(2000, 2000), 4));
  CHECK(!map.explored(pt(2000, 2000), 6));
}

TEST(fog_an_unsized_map_is_explored_by_nobody_including_the_ninth_player) {
  // The missing-manager path, 0x004c684f: it answers false *before* the slot
  // check, so even a player with no fog sees nothing. Both halves matter --
  // this is the state a session with no match extent is in.
  ExplorationMap map;
  CHECK(map.empty());
  CHECK(!map.explored(pt(0, 0), 0));
  CHECK(!map.explored(pt(0, 0), 12));
  map.explore(pt(0, 0), 0);
  CHECK(!map.explored(pt(0, 0), 0));
}

TEST(fog_a_point_off_the_grid_is_not_explored) {
  ExplorationMap map;
  map.resize(4096);
  map.explore_all();
  CHECK(map.explored(pt(4095, 4095), 0));
  CHECK(!map.explored(pt(4096, 0), 0));
  CHECK(!map.explored(pt(0, 4096), 0));
  CHECK(!map.explored(pt(-1, 0), 0));
  CHECK(!map.explored(pt(0, -1), 0));
}

TEST(fog_explore_circle_is_the_stamp_full_inside_a_rim_around_and_nothing_beyond) {
  using State = ExplorationMap::State;
  ExplorationMap map;
  map.resize(8192);
  // Centred on cell (2,2)'s own centre, radius 1024. Fully explored to
  // `r - 48 = 976`: cell (2,2)'s farthest corner is 724 away, so it is full.
  // The four orthogonal neighbours' nearest edges are 513 away and their far
  // corners 1619, so they meet the annulus and are partial; so are the
  // diagonals, whose nearest corners are 725 away -- inside `R = 1072`.
  map.explore_circle(pt(2560, 2560), 1024, 1);
  CHECK(map.state(2, 2, 1) == State::full);
  CHECK(map.fine(2, 2, 1) == nullptr);
  for (const auto [cx, cy] : {std::pair{1, 2}, std::pair{3, 2}, std::pair{2, 1}, std::pair{2, 3},
                              std::pair{1, 1}, std::pair{3, 3}, std::pair{1, 3}, std::pair{3, 1}}) {
    CHECK(map.state(cx, cy, 1) == State::partial);
    CHECK(map.fine(cx, cy, 1) != nullptr);
    CHECK(map.explored(pt(cx * 1024 + 512, cy * 1024 + 512), 1));
  }
  // Two cells out is beyond `R` on every side.
  CHECK(map.state(0, 2, 1) == State::never);
  CHECK(map.state(4, 2, 1) == State::never);
  CHECK(map.state(0, 0, 1) == State::never);
  CHECK(!map.explored(pt(512, 2560), 1));

  // The rim, in the left neighbour's record: the lattice point on its right
  // edge (x = 2016, 544 from the centre) is inside `R - 96 = 976`, so 15; the
  // point at x = 1568 is 992 away, on the ramp: `(1072 - 992) * 15 / 96 = 12`;
  // the point at x = 1504 is 1056 away, `16 * 15 / 96 = 2`; x = 1472 is
  // 1088 away, beyond `R`, so 0.
  const ExplorationMap::FineRecord* left = map.fine(1, 2, 1);
  REQUIRE(left != nullptr);
  const auto value = [&](std::int32_t i, std::int32_t j) {
    return static_cast<int>(left->values[static_cast<std::size_t>(j) * 32 + i]);
  };
  CHECK(value(31, 16) == 15);
  CHECK(value(17, 16) == 12);
  CHECK(value(15, 16) == 2);
  CHECK(value(14, 16) == 0);
  // And `fine_at` reads the same lattice in map-wide fine coordinates: 15 in
  // the full cell, the record's value in the partial one, 0 where nothing
  // has been.
  CHECK(map.fine_at(2 * 32, 2 * 32 + 16, 1) == 15);
  CHECK(map.fine_at(1 * 32 + 17, 2 * 32 + 16, 1) == 12);
  CHECK(map.fine_at(0, 0, 1) == 0);

  // A second stamp only grows a record: the same circle again changes
  // nothing, a larger one lifts the values, never lowers them.
  const ExplorationMap::FineRecord before = *left;
  map.explore_circle(pt(2560, 2560), 1024, 1);
  REQUIRE(map.fine(1, 2, 1) != nullptr);
  CHECK(map.fine(1, 2, 1)->values == before.values);
  map.explore_circle(pt(2560, 2560), 1100, 1);
  REQUIRE(map.fine(1, 2, 1) != nullptr);
  for (std::size_t k = 0; k < before.values.size(); ++k) {
    CHECK(map.fine(1, 2, 1)->values[k] >= before.values[k]);
  }
  CHECK(map.fine_at(1 * 32 + 17, 2 * 32 + 16, 1) == 15);

  // A stamp that fills a partial cell promotes it to full and drops the
  // record: from (2560, 2560), 2100 reaches (1,2)'s far corner at 1619 with
  // 48 to spare.
  map.explore_circle(pt(2560, 2560), 2100, 1);
  CHECK(map.state(1, 2, 1) == State::full);
  CHECK(map.fine(1, 2, 1) == nullptr);

  // **The equivalence**: a record whose every lattice point reads 15 is a
  // full cell. From cell (5,5)'s corner (5120, 5120) with radius 1900 the
  // cell's far corner (6143, 6143) is 1447 away -- inside `r - 48 = 1852` --
  // so it is full outright; but from (5120 + 512, 5120 + 512) with radius
  // 1470 the far corners are 723 away, inside 1422, and the *near* corners
  // 512 away, also inside -- yet cell (4,5) to the left has its far corner
  // (4096, 6143) at 1244, inside 1422, and its... it is full too. The case
  // that isolates the promotion is a radius where the corner at +1023 is out
  // and the lattice's last point at +992 is in: from (5120, 5120), a radius
  // of 1451 puts `r - 48 = 1403` between the lattice corner (992, 992) at
  // 1402.9 and the cell corner at 1446.
  ExplorationMap edge;
  edge.resize(8192);
  edge.explore_circle(pt(5120, 5120), 1451, 0);
  CHECK(edge.state(5, 5, 0) == State::full);
  CHECK(edge.fine(5, 5, 0) == nullptr);

  // Nobody else's slot moved.
  CHECK(!map.explored(pt(2560, 2560), 0));
  CHECK(!map.explored(pt(2560, 2560), 2));
  // A negative radius stamps nothing; zero stamps a 48-unit rim around the
  // point, which makes the cell it stands in partial.
  ExplorationMap none;
  none.resize(4096);
  none.explore_circle(pt(2000, 2000), -1, 0);
  CHECK(none.state(1, 1, 0) == State::never);
  none.explore_circle(pt(2000, 2000), 0, 0);
  CHECK(none.state(1, 1, 0) == State::partial);
  CHECK(none.explored(pt(2000, 2000), 0));
  // ...and `explore` of a point is the full state outright.
  none.explore(pt(2000, 2000), 0);
  CHECK(none.state(1, 1, 0) == State::full);
  CHECK(none.fine(1, 1, 0) == nullptr);
}

TEST(fog_set_raw_refuses_a_grid_its_writer_could_not_have_produced) {
  using State = ExplorationMap::State;
  ExplorationMap map;
  map.resize(2048);
  map.explore_circle(pt(1024, 1024), 900, 3);
  const std::vector<ExplorationMap::FineEntry> entries = map.fine_entries();
  REQUIRE(!entries.empty());
  const std::vector<std::uint16_t> cells(map.raw().begin(), map.raw().end());

  ExplorationMap copy;
  REQUIRE(copy.set_raw(map.cells(), cells, entries));
  CHECK(copy.state(0, 0, 3) == map.state(0, 0, 3));
  REQUIRE(copy.fine(entries[0].index % 2, entries[0].index / 2, 3) != nullptr);
  CHECK(copy.fine(entries[0].index % 2, entries[0].index / 2, 3)->values == entries[0].record->values);

  // A partial cell with no record.
  ExplorationMap bare;
  CHECK(!bare.set_raw(map.cells(), cells, {}));
  CHECK(bare.empty());
  // A record on a cell that is not partial.
  std::vector<std::uint16_t> full(cells.size(), 0xFFFFu);
  CHECK(!bare.set_raw(map.cells(), full, entries));
  CHECK(bare.empty());
  // A count that is not the side squared.
  CHECK(!bare.set_raw(3, cells, entries));
  CHECK(bare.empty());
  // A value past 15.
  ExplorationMap::FineRecord loud = *entries[0].record;
  loud.values[0] = 16;
  std::vector<ExplorationMap::FineEntry> bad = entries;
  bad[0].record = &loud;
  CHECK(!bare.set_raw(map.cells(), cells, bad));
  CHECK(bare.empty());
  (void)State::never;
}

TEST(fog_explore_all_takes_every_cell_for_every_slot) {
  ExplorationMap map;
  map.resize(4096);
  map.explore_all();
  for (std::int32_t slot = 0; slot < ExplorationMap::kSlots; ++slot) {
    CHECK(map.explored(pt(10, 10), slot));
    CHECK(map.explored(pt(4000, 4000), slot));
  }
}

// --------------------------------------------------------------------------
// the entry points
// --------------------------------------------------------------------------

TEST(fog_is_explored_takes_a_one_based_player) {
  Bench b;
  b.fog.map().explore(pt(3000, 3000), 0);  // slot 0
  // `IsExplored` decrements: script player 1 is slot 0.
  CHECK(b.is_explored(pt(3000, 3000), 1));
  CHECK(!b.is_explored(pt(3000, 3000), 2));
  // And out of range answers false rather than reading a slot it should not.
  CHECK(!b.is_explored(pt(3000, 3000), 0));
  CHECK(!b.is_explored(pt(3000, 3000), 17));
  // Player 9 has no fog: true everywhere, on a sized map.
  CHECK(b.is_explored(pt(7000, 7000), 9));
  CHECK(!b.is_explored(pt(7000, 7000), 1));
}

TEST(fog_explore_circle_and_explore_all_are_reachable_from_a_script) {
  Bench b;
  CHECK(!b.is_explored(pt(1500, 1500), 3));
  b.call(script::CallKind::free_function, "ExploreCircle", 3,
         {script::Value::integer(3), pack_point(pt(1500, 1500)), script::Value::integer(600)});
  CHECK(b.is_explored(pt(1500, 1500), 3));
  // Player 3 is slot 2, and slot 3 (player 4) did not move.
  CHECK(!b.is_explored(pt(1500, 1500), 4));

  CHECK(!b.is_explored(pt(7000, 7000), 4));
  b.call(script::CallKind::free_function, "ExploreAll", 0, {});
  CHECK(b.is_explored(pt(7000, 7000), 4));
  CHECK(b.is_explored(pt(7000, 7000), 1));
}

TEST(fog_explore_area_reveals_the_named_shape_and_nothing_when_the_name_is_wrong) {
  Bench b;
  const ObjectId marker = b.world.spawn(NativeClass::area, nullptr);
  b.world.set_position(marker, pt(5000, 5000));
  CHECK(b.world.named_objects().bind("A_Explore1", marker));
  CHECK(b.areas.areas().bind(marker, AreaShape::of_circle(pt(5000, 5000), 900)));

  CHECK(!b.is_explored(pt(5000, 5000), 1));
  b.call(script::CallKind::free_function, "ExploreArea", 2,
         {script::Value::integer(1), script::Value::string("A_Explore1")});
  CHECK(b.is_explored(pt(5000, 5000), 1));
  // Far outside the shape, untouched.
  CHECK(!b.is_explored(pt(1000, 1000), 1));

  // A name that matches nothing prints and explores nothing -- a return, not a
  // refusal, so the script carries on.
  const script::HostOutcome miss =
      b.call(script::CallKind::free_function, "ExploreArea", 2,
             {script::Value::integer(2), script::Value::string("A_Nowhere")});
  CHECK(miss.status == script::HostStatus::ok);
  CHECK(!b.is_explored(pt(5000, 5000), 2));
}

TEST(fog_the_sweep_reveals_what_an_object_can_see_and_only_for_its_owner) {
  imperivm::core::ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
      R"(<class id="Scout" parent="Object" cpp_class="CVXUnit">
           <properties sight="1500" maxhealth="80"/></class>)",
      // A sight smaller than half a cell: the rim the stamp writes around it
      // is the only thing that makes its own cell explored.
      R"(<class id="Mole" parent="Object" cpp_class="CVXUnit">
           <properties sight="40" maxhealth="80"/></class>)",
  };
  const char* names[] = {"object.sc.xml", "scout.sc.xml", "mole.sc.xml"};
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  for (int i = 0; i < 3; ++i) graph.add(bytes(docs[i]), names[i]);
  graph.link();

  Bench b;
  b.world.set_class_graph(&graph);
  const ObjectId scout = b.world.spawn(NativeClass::unit, nullptr, graph.find("Scout"));
  b.world.set_position(scout, pt(4500, 4500));
  b.world.set_owner(scout, 0);  // slot 0, script player 1

  // Nothing yet: `start` ran before the scout existed.
  CHECK(!b.is_explored(pt(4500, 4500), 1));
  b.world.advance(400);
  CHECK(b.is_explored(pt(4500, 4500), 1));
  // Within sight, and not beyond it.
  CHECK(b.is_explored(pt(3500, 4500), 1));
  CHECK(!b.is_explored(pt(500, 500), 1));
  // Its owner's slot and nobody else's.
  CHECK(!b.is_explored(pt(4500, 4500), 2));

  // Walking reveals the ground it reaches, not the ground it left -- which is
  // why the system runs after movement.
  b.world.set_position(scout, pt(500, 500));
  b.world.advance(400);
  CHECK(b.is_explored(pt(500, 500), 1));

  // **A unit inside a holder reveals nothing**, and the case has to be built
  // from a unit that has never stood anywhere: a held object's position is
  // `kHeldPosition`, so a sweep that forgot the guard would reveal the cells
  // around `(-1, -1)` -- the corner of the map -- rather than the holder's.
  const ObjectId ship = b.world.spawn(NativeClass::building, nullptr);
  b.world.set_position(ship, pt(7500, 7500));
  const ObjectId stowaway = b.world.spawn(NativeClass::unit, nullptr, graph.find("Scout"));
  b.world.set_owner(stowaway, 1);  // slot 1, script player 2
  REQUIRE(b.world.put_in_holder(stowaway, ship));
  b.world.advance(400);
  CHECK(!b.is_explored(pt(7500, 7500), 2));
  // The corner, which is what `kHeldPosition` would have revealed.
  CHECK(!b.is_explored(pt(512, 512), 2));
  CHECK(!b.is_explored(pt(0, 0), 2));

  // Taken out again, it reveals from where it is put.
  REQUIRE(b.world.remove_from_holder(stowaway, pt(7500, 7500)));
  b.world.advance(400);
  CHECK(b.is_explored(pt(7500, 7500), 2));

  // **A unit whose sight is smaller than half a cell still reveals the ground
  // under its feet.** Standing at (2100, 2100) with a sight of 40, the stamp's
  // rim reaches 88 units, which makes the cell it stands in partially explored
  // -- and `IsExplored` is *not never*, so it answers true there and nowhere
  // else.
  const ObjectId mole = b.world.spawn(NativeClass::unit, nullptr, graph.find("Mole"));
  b.world.set_position(mole, pt(2100, 2100));
  b.world.set_owner(mole, 2);  // slot 2, script player 3
  CHECK(!b.is_explored(pt(2100, 2100), 3));
  b.world.advance(400);
  CHECK(b.is_explored(pt(2100, 2100), 3));
  CHECK(!b.is_explored(pt(3100, 2100), 3));

  // **A sight of zero is not "sees its own cell", it is "sees nothing".** The
  // ground-under-its-feet stamp above is for a unit whose circle misses every
  // cell centre, not for one that has no circle: a flag or a rock owned by a
  // player reveals nothing at all.
  const ObjectId flag = b.world.spawn(NativeClass::unit, nullptr, graph.find("Object"));
  b.world.set_position(flag, pt(6100, 100));
  b.world.set_owner(flag, 3);  // slot 3, script player 4
  b.world.advance(400);
  CHECK(!b.is_explored(pt(6100, 100), 4));
}

TEST(fog_get_unexplored_point_clamps_its_argument_and_answers_minus_one) {
  imperivm::core::ClassGraph graph;
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  graph.add(bytes(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "object.sc.xml");
  graph.add(bytes(R"(<class id="Scout" parent="Object" cpp_class="CVXUnit">
           <properties sight="300" maxhealth="80"/></class>)"),
            "scout.sc.xml");
  graph.link();

  Bench b(4096);
  b.world.set_class_graph(&graph);
  const ObjectId scout = b.world.spawn(NativeClass::unit, nullptr, graph.find("Scout"));
  b.world.set_position(scout, pt(512, 512));
  b.world.set_owner(scout, 0);

  const auto ask = [&](std::int32_t argument) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "GetUnexploredPoint", 1,
               {script::Value::object(kTypeObj, scout), script::Value::integer(argument)});
    CHECK(out.status == script::HostStatus::ok);
    CHECK(is_point(out.value));
    return unpack_point(out.value);
  };

  // `-1` is below the unit's own sight, so the radius becomes
  // `max(sight * 4, 512)` = 1200. The nearest unexplored cell centre is its
  // own, at (512, 512).
  CHECK((ask(-1) == pt(512, 512)));

  // Reveal that cell and the answer moves to the next nearest inside 1200 --
  // (1536, 512) and (512, 1536) are both 1024 away, and the tie goes to the
  // lower cell index, which is row-major: (1536, 512).
  b.fog.map().explore(pt(512, 512), 0);
  CHECK((ask(-1) == pt(1536, 512)));

  // With everything explored there is nowhere to go, and the answer is the
  // sentinel `UNIT_EXPLORE.VS` tests: `if (pt.x == -1) break;`.
  b.fog.map().explore_all();
  CHECK((ask(-1) == pt(-1, -1)));

  // A receiver that is not an object, and one with no owner, answer the same
  // sentinel rather than refusing.
  const script::HostOutcome bad =
      b.call(script::CallKind::member, "GetUnexploredPoint", 1,
             {script::Value::integer(3), script::Value::integer(-1)});
  CHECK(bad.status == script::HostStatus::ok);
  CHECK((unpack_point(bad.value) == pt(-1, -1)));
}

TEST(fog_the_map_survives_a_save_and_is_not_in_the_hash) {
  Bench b;
  b.fog.map().explore_circle(pt(2000, 2000), 1500, 0);
  b.fog.map().explore(pt(6000, 6000), 4);

  const std::uint64_t before = b.world.hashes().hash_of_hashes;
  std::uint64_t folded = 0;
  b.fog.hash(folded);
  // **Deliberately empty**: `exploration` is one of the four channels the
  // shipped build leaves at zero, so folding anything here would put this
  // engine's determinism contract somewhere the original's is not.
  CHECK(folded == 0);
  b.fog.map().explore_all();
  CHECK(b.world.hashes().hash_of_hashes == before);

  // Saved anyway. A reloaded game whose map had gone dark is a different game.
  Bench restored;
  std::vector<std::byte> bytes;
  b.fog.serialize(bytes);
  REQUIRE(restored.fog.deserialize(bytes).ok());
  CHECK(restored.fog.map().cells() == b.fog.map().cells());
  CHECK(restored.fog.map().explored(pt(6000, 6000), 4));
  CHECK(restored.fog.map().explored(pt(10, 10), 7));

  // The rim goes with it: on a map that is not all full, slot 5's circle
  // leaves partial cells whose records must come back nibble for nibble.
  Bench rim;
  rim.fog.map().explore_circle(pt(3000, 3000), 700, 5);
  rim.fog.map().explore_circle(pt(6000, 1000), 1500, 0);
  REQUIRE(!rim.fog.map().fine_entries().empty());
  Bench again;
  std::vector<std::byte> rim_bytes;
  rim.fog.serialize(rim_bytes);
  REQUIRE(again.fog.deserialize(rim_bytes).ok());
  CHECK(std::equal(again.fog.map().raw().begin(), again.fog.map().raw().end(),
                   rim.fog.map().raw().begin(), rim.fog.map().raw().end()));
  const auto mine = rim.fog.map().fine_entries();
  const auto theirs = again.fog.map().fine_entries();
  REQUIRE(mine.size() == theirs.size());
  for (std::size_t i = 0; i < mine.size(); ++i) {
    CHECK(mine[i].slot == theirs[i].slot);
    CHECK(mine[i].index == theirs[i].index);
    CHECK(mine[i].record->values == theirs[i].record->values);
  }
  // A record on a cell the grid says is full is a file this writer could
  // not have produced: flip one cell's slot-5 bits and the load is refused.
  std::vector<std::byte> forged = rim_bytes;
  {
    // The first cell word sits after the 8-byte section header, the side and
    // the count; find slot 5's first partial cell and set it to full.
    const std::size_t index = static_cast<std::size_t>(mine[0].index);
    const std::size_t at = 8 + 4 + 4 + index * 2;
    auto word = static_cast<std::uint16_t>(static_cast<std::uint8_t>(forged[at]) |
                                           (static_cast<std::uint8_t>(forged[at + 1]) << 8));
    word = static_cast<std::uint16_t>(word | (3u << (2 * mine[0].slot)));
    forged[at] = static_cast<std::byte>(word & 0xFF);
    forged[at + 1] = static_cast<std::byte>(word >> 8);
  }
  Bench refused;
  refused.fog.map().explore(pt(100, 100), 2);
  CHECK(!refused.fog.deserialize(forged).ok());
  // ...and, like a truncation, it leaves the map it was loading into alone.
  CHECK(refused.fog.map().explored(pt(100, 100), 2));
  CHECK(!refused.fog.map().explored(pt(3000, 3000), 5));

  // And a truncated payload leaves the map it was loading into untouched, at
  // every cut -- the rule every other store in this tree follows.
  Bench target;
  for (std::size_t cut = 0; cut < bytes.size(); cut += 7) {
    CHECK(!target.fog.deserialize(std::span(bytes).first(cut)).ok());
  }
  CHECK(!target.fog.map().explored(pt(6000, 6000), 4));
}

/// `o.SetSight(n)` -- two refusals, a widening that explores, and a store that
/// happens either way.
TEST(set_sight_refuses_the_two_extremes_and_only_a_widening_explores) {
  ClassGraph graph;
  REQUIRE(graph
              .add(bytes_of(R"(<class id="Scout" cpp_class="CVXUnit"><properties sight="400"/></class>)"),
                   "scout.sc.xml")
              .ok());
  graph.link();

  Bench b;
  b.world.set_class_graph(&graph);
  const ObjectId scout = b.world.spawn(NativeClass::unit, nullptr, graph.find("Scout"));
  REQUIRE(b.world.set_owner(scout, 0));
  REQUIRE(b.world.set_position(scout, pt(4000, 4000)));
  REQUIRE(b.world.find(scout)->sight == 400);

  const auto set = [&](ObjectId id, std::int32_t n) {
    return b.call(script::CallKind::member, "SetSight", 1,
                  {script::Value::object(kTypeObj, id), script::Value::integer(n)});
  };

  // Neither refusal stores anything -- that is what makes them refusals rather
  // than clamps.
  CHECK(set(scout, 99).status == script::HostStatus::ok);
  CHECK(b.world.find(scout)->sight == 400);
  CHECK(set(scout, 2049).status == script::HostStatus::ok);
  CHECK(b.world.find(scout)->sight == 400);
  CHECK(set(scout, -1).status == script::HostStatus::ok);
  CHECK(b.world.find(scout)->sight == 400);

  // The edges themselves are accepted.
  CHECK(set(scout, 100).status == script::HostStatus::ok);
  CHECK(b.world.find(scout)->sight == 100);
  CHECK(set(scout, 2048).status == script::HostStatus::ok);
  CHECK(b.world.find(scout)->sight == 2048);

  // A widening explores, on a world of its own so that nothing above has
  // already revealed the ground under test. Player 0 is `IsExplored(1)`.
  Bench w;
  w.world.set_class_graph(&graph);
  const ObjectId watcher = w.world.spawn(NativeClass::unit, nullptr, graph.find("Scout"));
  REQUIRE(w.world.set_owner(watcher, 0));
  REQUIRE(w.world.set_position(watcher, pt(4000, 4000)));
  const auto widen = [&](std::int32_t n) {
    return w.call(script::CallKind::member, "SetSight", 1,
                  {script::Value::object(kTypeObj, watcher), script::Value::integer(n)});
  };
  const Point far_out = pt(4000 + 900, 4000);
  CHECK(!w.is_explored(far_out, 1));
  CHECK(widen(1000).status == script::HostStatus::ok);
  CHECK(w.is_explored(far_out, 1));
  CHECK(w.world.find(watcher)->sight == 1000);

  // **A narrowing stores and does not explore.** The point below is inside the
  // radius the call asks for and outside everything already known, so a body
  // that stamped on every call would reveal it.
  const Point elsewhere = pt(4000, 4000 - 900);
  Bench n;
  n.world.set_class_graph(&graph);
  const ObjectId shrinking = n.world.spawn(NativeClass::unit, nullptr, graph.find("Scout"));
  REQUIRE(n.world.set_owner(shrinking, 0));
  REQUIRE(n.world.set_position(shrinking, pt(4000, 4000)));
  n.world.find(shrinking)->sight = 2000;
  CHECK(!n.is_explored(elsewhere, 1));
  CHECK(n.call(script::CallKind::member, "SetSight", 1,
               {script::Value::object(kTypeObj, shrinking), script::Value::integer(1000)})
            .status == script::HostStatus::ok);
  CHECK(n.world.find(shrinking)->sight == 1000);
  CHECK(!n.is_explored(elsewhere, 1));

  // The reveal is skipped for an object with no position of its own.
  Bench h;
  h.world.set_class_graph(&graph);
  const ObjectId held = h.world.spawn(NativeClass::unit, nullptr, graph.find("Scout"));
  const ObjectId keep = h.world.spawn(NativeClass::building, nullptr);
  REQUIRE(h.world.set_owner(held, 0));
  REQUIRE(h.world.set_position(keep, pt(2000, 2000)));
  REQUIRE(h.world.put_in_holder(held, keep));
  const Point beside = pt(2000 + 900, 2000);
  CHECK(!h.is_explored(beside, 1));
  CHECK(h.call(script::CallKind::member, "SetSight", 1,
               {script::Value::object(kTypeObj, held), script::Value::integer(1000)})
            .status == script::HostStatus::ok);
  CHECK(h.world.find(held)->sight == 1000);
  CHECK(!h.is_explored(beside, 1));
  // ...and it does not explore around the **held marker** either, which is
  // where a body that skipped the position test but kept the stamp would
  // reveal: `kHeldPosition` is `(-1, -1)`, and a circle of 1000 around it
  // covers this point.
  CHECK(!h.is_explored(pt(400, 0), 1));

  // A receiver naming no object stores nothing and does not refuse.
  CHECK(set(static_cast<ObjectId>(9999), 500).status == script::HostStatus::ok);
}

/// The sight radius is hashed now, because a script can move it.
TEST(the_sight_radius_moves_the_hash_and_round_trips) {
  World world;
  const ObjectId scout = world.spawn(NativeClass::unit, nullptr);
  const std::uint64_t before = world.state_hash();
  world.find(scout)->sight = 1700;
  CHECK(world.state_hash() != before);

  std::vector<std::byte> bytes;
  world.serialize(bytes);
  World back;
  REQUIRE(back.deserialize(bytes).ok());
  REQUIRE(back.find(scout) != nullptr);
  CHECK(back.find(scout)->sight == 1700);
  CHECK(back.state_hash() == world.state_hash());
}
