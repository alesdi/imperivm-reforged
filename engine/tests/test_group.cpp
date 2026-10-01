// Named object groups: the table, the map that seeds it, and the query over it.
//
// Two halves, and the split matters. The first exercises `sim::GroupTable` as a
// container -- interning, set semantics, ordering, hashing -- with no map and no
// game data at all. The second drives `World::populate_from_map` over a
// `map.obj.xml` fragment.
//
// **That fragment is not invented.** Its element names, attribute spellings,
// nesting and formatting are transcribed from the shipped
// `Adventures\Tutorial.BFHP` `Maps\1\map.obj.xml`, and the one shape that looks
// like a mistake -- two `type="0"` elements sharing the name `NO_Marker` -- is
// copied from real data too: `5_Great_Loses_German` repeats `NO_Invisible`
// eleven times, and `4_Great_Battles_Egypt`, `3_Great_Losses_Egypt` and
// `RandomMapSettlements` each repeat one twice. A fixture that quietly dropped
// that case would be a fixture asserting the reader is wrong.
//
// `type` selects the table: `type="1"` seeds `GroupTable`, `type="0"` seeds
// `NamedObjectTable`. That split is `gbr.exe`'s, not an inference from the call
// sites -- see `sim/world.hpp`.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/world/map.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace script = imperivm::core::script;

using imperivm::core::ClassGraph;
using imperivm::core::kGroupAlias;
using imperivm::core::kGroupArmy;
using imperivm::core::MapObjectList;
using imperivm::core::Result;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Two classes, spelled the way `DATA\CLASSES\*.SC.XML` spells them.
/// A teleport and a unit. `CVXTeleport` is what `native_class_from_name` maps
/// to `NativeClass::teleport`, and the shipped teleports are `Teleport_1` and
/// `Teleport_2` -- two classes, one mechanism.
struct TeleportGraph {
  ClassGraph graph;
  imperivm::core::ClassIndex teleport = imperivm::core::kNoClass;
  imperivm::core::ClassIndex unit = imperivm::core::kNoClass;

  TeleportGraph() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="Teleport_1" parent="Object" cpp_class="CVXTeleport"><properties maxhealth="500"/></class>)",
        R"(<class id="RHastatus" parent="Object" cpp_class="CVXUnit"><properties sight="100" maxhealth="50"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "teleport.sc.xml", "hastatus.sc.xml"};
    for (int i = 0; i < 3; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    teleport = graph.find("Teleport_1");
    unit = graph.find("RHastatus");
  }
};

struct GroupGraph {
  ClassGraph graph;

  GroupGraph() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="RHastatus" parent="Object" cpp_class="CVXUnit"><properties sight="100" maxhealth="50"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "hastatus.sc.xml"};
    for (int i = 0; i < 2; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
  }
};

/// Six objects and six `<group>`s, in the shipped layout: `<scriptobj>`s first,
/// then the groups, every one a direct child of `<mapobject>`.
///
///   * `CampDefense` is a `type="1"` army over three objects, listed in
///     authoring order (3, 0, 1) rather than ascending, as the shipped file
///     lists `CampDefense` as 8, 3, 19, 22, ...
///   * `Reserve` is a second army, overlapping `CampDefense` on one object.
///     20 of the 21 shipped maps that declare groups have such an overlap.
///   * `NO_Marker` is a `type="0"` alias -- a *named object*, not a group --
///     and occurs **twice**, over two different objects.
///   * `Caesar` is the ordinary single-object alias, 1,378 of which ship.
///   * `Ghosts` is an army naming an object whose class the graph does not
///     know. No shipped map does this -- 23,408 of 23,408 memberships resolve
///     -- so it is here to pin the behaviour rather than to reproduce anything.
constexpr std::string_view kMapDocument = R"(<mapobject>
	<scriptobj
		class="RHastatus"
		num="0"
	player="1"
	x="100"
	y="100"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="RHastatus"
		num="1"
	player="1"
	x="200"
	y="100"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="RHastatus"
		num="2"
	player="2"
	x="300"
	y="100"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="RHastatus"
		num="3"
	player="2"
	x="400"
	y="100"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="NoSuchClass"
		num="4"
	player="2"
	x="500"
	y="100"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="RHastatus"
		num="5"
	player="2"
	x="600"
	y="100"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<group
		name="CampDefense"
		type="1">
		<obj
			num="3"/>
		<obj
			num="0"/>
		<obj
			num="1"/>
	</group>
	<group
		name="NO_Marker"
		type="0">
		<obj
			num="2"/>
	</group>
	<group
		name="Caesar"
		type="0">
		<obj
			num="5"/>
	</group>
	<group
		name="NO_Marker"
		type="0">
		<obj
			num="3"/>
	</group>
	<group
		name="Reserve"
		type="1">
		<obj
			num="5"/>
		<obj
			num="3"/>
	</group>
	<group
		name="Ghosts"
		type="1">
		<obj
			num="4"/>
	</group>
</mapobject>)";

/// A populated world plus the report `populate_from_map` returned.
struct LoadedMap {
  GroupGraph classes;
  MapObjectList map;
  World world;
  World::PopulateReport report;

  LoadedMap() {
    auto parsed = MapObjectList::parse(bytes_of(kMapDocument));
    map = std::move(parsed.value());
    report = world.populate_from_map(map, classes.graph);
  }

  /// The world object spawned for `<scriptobj num>`. The five objects whose
  /// class resolves take ids 1..5 in document order, which is `num` order with
  /// the unresolved `num="4"` skipped.
  [[nodiscard]] ObjectId spawned(std::int32_t num) const {
    return num < 4 ? static_cast<ObjectId>(num + 1) : static_cast<ObjectId>(num);
  }
};

std::vector<ObjectId> members_of(const World& world, std::string_view name) {
  const std::span<const ObjectId> list = world.groups().members(world.groups().find(name));
  return std::vector<ObjectId>(list.begin(), list.end());
}

}  // namespace

// --------------------------------------------------------------------------
// the table
// --------------------------------------------------------------------------

TEST(a_group_name_is_interned_once_and_keeps_its_index) {
  GroupTable table;
  CHECK(table.find("Attackers") == GroupTable::kNoGroup);
  CHECK(table.size() == 0);

  const std::int32_t first = table.intern("Attackers");
  const std::int32_t second = table.intern("Defenders");
  CHECK(first == 0);
  CHECK(second == 1);
  CHECK(table.size() == 2);

  // Idempotent: `Group("Attackers")` evaluated a thousand times is one group.
  CHECK(table.intern("Attackers") == first);
  CHECK(table.size() == 2);
  CHECK(table.find("Attackers") == first);
  CHECK(table.name(first) == "Attackers");
  CHECK(table.name(second) == "Defenders");
}

TEST(an_index_outside_the_table_names_nothing) {
  GroupTable table;
  table.intern("Only");
  CHECK(!table.valid(-1));
  CHECK(!table.valid(1));
  CHECK(table.valid(0));
  CHECK(table.name(GroupTable::kNoGroup).empty());
  CHECK(table.members(GroupTable::kNoGroup).empty());
  CHECK(table.members(7).empty());
  CHECK(!table.contains(GroupTable::kNoGroup, 1));
  // The two mutators refuse rather than growing the table sideways.
  CHECK(!table.add(GroupTable::kNoGroup, 1));
  CHECK(!table.remove(4, 1));
}

TEST(a_group_is_a_set_kept_in_first_add_order) {
  GroupTable table;
  const std::int32_t g = table.intern("Q_Invaders");

  // Added out of order, as a script does: `mediterranean` map 4's
  // `Q_Interceptors1..4.AddToGroup("Q_Slingers")` folds four cohorts into one
  // name in the order the script names them, which is not the order their ids
  // run in. `CVXGroup::Add` (0x00572410) appends after a linear `std::find`
  // dedup and compares no ids, so that authored order is what survives.
  CHECK(table.add(g, 9));
  CHECK(table.add(g, 2));
  CHECK(table.add(g, 5));
  // No duplicates: the 23,408 authored memberships contain none, and a query
  // that returned an object twice would break `count`. Refused whether the
  // repeat is the newest member or the oldest -- a dedup that only looked at
  // the back of the list would pass the first of these and fail the second,
  // and it survived the injection run until the second one was here.
  CHECK(!table.add(g, 5));
  CHECK(!table.add(g, 9));
  CHECK(!table.add(g, 2));
  CHECK(!table.add(g, kNoObject));

  const std::span<const ObjectId> list = table.members(g);
  REQUIRE(list.size() == 3);
  CHECK(list[0] == 9);
  CHECK(list[1] == 2);
  CHECK(list[2] == 5);
  CHECK(table.contains(g, 5));
  CHECK(!table.contains(g, 6));
}

TEST(removing_a_member_leaves_the_survivors_in_their_order) {
  // `CVXGroup::Remove` (0x00572210) erases the one element it found and moves
  // nothing else. A swap-with-last would be cheaper, would pass every
  // membership assertion in this file, and would reorder a list whose order is
  // hashed -- so the order is what this asserts, not the membership.
  // The removed member is the **first**, deliberately. Taking out the
  // second-to-last one instead makes swap-with-last and erase produce the same
  // list, and the obvious version of this test did exactly that and passed
  // with the fault injected.
  GroupTable table;
  const std::int32_t g = table.intern("Q_Slingers");
  for (const ObjectId id : {40u, 10u, 30u, 20u}) table.add(g, id);

  CHECK(table.remove(g, 40));

  const std::span<const ObjectId> list = table.members(g);
  REQUIRE(list.size() == 3);
  CHECK(list[0] == 10);
  CHECK(list[1] == 30);
  CHECK(list[2] == 20);

  // And re-adding puts it at the back rather than back where it was.
  CHECK(table.add(g, 40));
  REQUIRE(table.members(g).size() == 4);
  CHECK(table.members(g)[3] == 40);
}

TEST(removing_takes_one_membership_and_leaves_the_rest) {
  GroupTable table;
  const std::int32_t a = table.intern("A");
  const std::int32_t b = table.intern("B");
  for (ObjectId id : {1u, 2u, 3u}) {
    table.add(a, id);
    table.add(b, id);
  }

  CHECK(table.remove(a, 2));
  CHECK(!table.remove(a, 2));  // already gone
  CHECK(table.members(a).size() == 2);
  CHECK(table.members(b).size() == 3);
  CHECK(!table.contains(a, 2));
  CHECK(table.contains(b, 2));
}

TEST(remove_from_all_groups_clears_every_membership_and_keeps_the_names) {
  // `Group("GoldMules" + idPlayer).RemoveFromAllGroups()` in
  // `DATA\AI\ES_OUTPOSTSELLGOLD.VS`, twice.
  GroupTable table;
  const std::int32_t a = table.intern("GoldMules0");
  const std::int32_t b = table.intern("Attackers");
  const std::int32_t c = table.intern("Idle");
  table.add(a, 7);
  table.add(b, 7);
  table.add(b, 8);
  // `Wide` is here for the order rather than the count: 7 is its **first**
  // member, so an implementation that filled the hole with the back element
  // instead of erasing would leave a differently ordered list of the same
  // size. Every size assertion below passes either way.
  const std::int32_t wide = table.intern("Wide");
  for (const ObjectId id : {7u, 11u, 12u, 13u}) table.add(wide, id);

  CHECK(table.remove_from_all(7) == 3);
  CHECK(table.remove_from_all(7) == 0);
  CHECK(table.remove_from_all(kNoObject) == 0);
  CHECK(table.members(a).empty());
  CHECK(table.members(b).size() == 1);
  CHECK(table.members(c).empty());

  // The survivors keep their order, which is the half a size check cannot see.
  const std::span<const ObjectId> rest = table.members(wide);
  REQUIRE(rest.size() == 3);
  CHECK(rest[0] == 11);
  CHECK(rest[1] == 12);
  CHECK(rest[2] == 13);

  // The names survive with their indices: a live query object holds one, and
  // renumbering would point it somewhere else.
  CHECK(table.size() == 4);
  CHECK(table.find("GoldMules0") == a);
  CHECK(table.find("Idle") == c);
}

// --------------------------------------------------------------------------
// the named-object table
// --------------------------------------------------------------------------

TEST(a_named_object_binds_one_object_and_never_creates_on_lookup) {
  NamedObjectTable table;
  CHECK(table.find("Caesar") == NamedObjectTable::kNoName);
  CHECK(table.object("Caesar") == kNoObject);
  // Reading does not intern. Every consumer in `gbr.exe` refuses an unknown
  // name -- `Could not find named object named '%s' in function 'SpawnNamed'` --
  // rather than minting one, which is the opposite of `GroupTable::intern`.
  CHECK(table.size() == 0);

  CHECK(table.bind("Caesar", 4));
  CHECK(table.find("Caesar") == 0);
  CHECK(table.object("Caesar") == 4);
  CHECK(table.object(0) == 4);
  CHECK(table.name(0) == "Caesar");

  // First binding wins; a second is refused rather than replacing.
  CHECK(!table.bind("Caesar", 9));
  CHECK(table.object("Caesar") == 4);
  CHECK(table.size() == 1);

  CHECK(!table.bind("Nobody", kNoObject));
  CHECK(table.size() == 1);
  CHECK(!table.valid(-1));
  CHECK(!table.valid(1));
  CHECK(table.name(3).empty());
  CHECK(table.object(3) == kNoObject);
}

// --------------------------------------------------------------------------
// hashing
// --------------------------------------------------------------------------

namespace {

std::uint64_t hash_of(const GroupTable& table) {
  std::uint64_t state = 0;
  table.hash(state);
  return state;
}

}  // namespace

TEST(the_group_hash_separates_membership_names_and_order) {
  GroupTable empty;
  const std::uint64_t none = hash_of(empty);

  GroupTable one_empty_group;
  one_empty_group.intern("A");
  CHECK(hash_of(one_empty_group) != none);

  GroupTable with_member;
  with_member.intern("A");
  with_member.add(0, 1);
  CHECK(hash_of(with_member) != hash_of(one_empty_group));

  // Same set, different name. Two such worlds are different worlds: the next
  // `Group("...")` resolves differently in each.
  GroupTable renamed;
  renamed.intern("B");
  renamed.add(0, 1);
  CHECK(hash_of(renamed) != hash_of(with_member));

  // Same names, different index order.
  GroupTable ab;
  ab.intern("A");
  ab.intern("B");
  GroupTable ba;
  ba.intern("B");
  ba.intern("A");
  CHECK(hash_of(ab) != hash_of(ba));

  // And two tables holding the same membership under the same name in
  // *different* orders are different worlds, because the order is the order
  // things joined and `SpawnGroup` mints ids by walking it. This assertion was
  // the other way round for as long as the table sorted, and inverting it is
  // the point of the change rather than a consequence of it.
  GroupTable forward;
  forward.intern("A");
  forward.add(0, 3);
  forward.add(0, 8);
  GroupTable backward;
  backward.intern("A");
  backward.add(0, 8);
  backward.add(0, 3);
  CHECK(hash_of(forward) != hash_of(backward));

  // The membership itself is still what it was: same names, same size, same
  // set. Only the order separates them, which is what makes this a real check
  // rather than a restatement of the one above it.
  CHECK(forward.members(0).size() == backward.members(0).size());
  CHECK(forward.contains(0, 3) && forward.contains(0, 8));
  CHECK(backward.contains(0, 3) && backward.contains(0, 8));
}

TEST(the_world_hash_moves_when_a_group_does) {
  World world;
  const ObjectId a = world.spawn(imperivm::core::NativeClass::unit, nullptr);
  const std::uint64_t before = world.state_hash();

  const std::int32_t g = world.group_index("Attackers");
  const std::uint64_t interned = world.state_hash();
  CHECK(interned != before);

  world.groups().add(g, a);
  const std::uint64_t populated = world.state_hash();
  CHECK(populated != interned);

  // And the named-object table is in the hash too: a peer that disagrees about
  // `Village2.obj` diverges on the next command issued through it.
  world.named_objects().bind("Caesar", a);
  CHECK(world.state_hash() != populated);
}

// --------------------------------------------------------------------------
// seeding from a map
// --------------------------------------------------------------------------

TEST(the_map_reader_keeps_one_entry_per_group_element) {
  auto parsed = MapObjectList::parse(bytes_of(kMapDocument));
  CHECK(parsed.ok());
  const MapObjectList& map = parsed.value();

  CHECK(map.objects().size() == 6);
  // Six elements, not five: the reader transcribes the file, and the file has
  // `NO_Marker` twice. Resolving that is the world's job, not the reader's.
  CHECK(map.groups().size() == 6);
  CHECK(map.groups()[0].name == "CampDefense");
  CHECK(map.groups()[0].type == kGroupArmy);
  CHECK(map.groups()[0].members.size() == 3);
  // Authoring order, not ascending.
  CHECK(map.groups()[0].members[0] == 3);
  CHECK(map.groups()[1].name == "NO_Marker");
  CHECK(map.groups()[1].type == kGroupAlias);
  CHECK(map.groups()[3].name == "NO_Marker");
  CHECK(map.groups()[3].type == kGroupAlias);
}

TEST(populating_a_world_seeds_both_tables_by_type) {
  const LoadedMap loaded;

  CHECK(loaded.report.map_groups == 6);
  // Three armies and two named objects out of six elements: the second
  // `NO_Marker` is a duplicate binding, not a fifth name.
  CHECK(loaded.report.groups == 3);
  CHECK(loaded.world.groups().size() == 3);
  CHECK(loaded.report.named_objects == 2);
  CHECK(loaded.world.named_objects().size() == 2);
  CHECK(loaded.report.duplicate_names == 1);
  // Three in `CampDefense`, two in `Reserve`, one each for the first
  // `NO_Marker` and `Caesar`.
  CHECK(loaded.report.memberships == 7);
  // `Ghosts` names `num="4"`, whose class is not in the graph, so it was never
  // spawned and cannot be a member of anything.
  CHECK(loaded.report.unresolved_members == 1);
  CHECK(loaded.report.unresolved_class == 1);

  // Armies interned in document order; named objects bound in document order.
  CHECK(loaded.world.groups().find("CampDefense") == 0);
  CHECK(loaded.world.groups().find("Reserve") == 1);
  CHECK(loaded.world.groups().find("Ghosts") == 2);
  CHECK(loaded.world.named_objects().find("NO_Marker") == 0);
  CHECK(loaded.world.named_objects().find("Caesar") == 1);

  // And neither table answers for the other. This is the whole point of the
  // split: `Group("Caesar")` finds nothing in the retail engine, and
  // `GetNamedObj("CampDefense")` finds nothing either.
  CHECK(loaded.world.groups().find("Caesar") == GroupTable::kNoGroup);
  CHECK(loaded.world.groups().find("NO_Marker") == GroupTable::kNoGroup);
  CHECK(loaded.world.named_objects().find("CampDefense") == NamedObjectTable::kNoName);
  CHECK(loaded.world.named_objects().find("Reserve") == NamedObjectTable::kNoName);
  CHECK(loaded.world.groups().find("NotAGroup") == GroupTable::kNoGroup);
}

TEST(a_group_member_num_resolves_to_the_object_spawned_for_it) {
  const LoadedMap loaded;

  const std::vector<ObjectId> camp = members_of(loaded.world, "CampDefense");
  REQUIRE(camp.size() == 3);
  // Authoring order, which the file gives as 3, 0, 1 -- not ascending. The
  // loader walks `<obj num>` in document order and each `add` appends, so the
  // file's order is what the group ends up holding, and it is what a later
  // `SpawnGroup` would mint ids in.
  CHECK(camp[0] == loaded.spawned(3));
  CHECK(camp[1] == loaded.spawned(0));
  CHECK(camp[2] == loaded.spawned(1));

  // `Reserve` is authored 5, 3 -- descending -- so it separates the two
  // readings on its own, and it overlaps `CampDefense` on object 3, which
  // holds a different position in each. One object, two groups, two orders.
  const std::vector<ObjectId> reserve = members_of(loaded.world, "Reserve");
  REQUIRE(reserve.size() == 2);
  CHECK(reserve[0] == loaded.spawned(5));
  CHECK(reserve[1] == loaded.spawned(3));

  CHECK(members_of(loaded.world, "Ghosts").empty());
}

TEST(a_repeated_alias_name_keeps_its_first_binding) {
  const LoadedMap loaded;
  // The eleven `NO_Invisible` aliases of `5_Great_Loses_German`, in miniature.
  // A named object is one object, so the second element cannot widen the first
  // into a set; first in document order wins, and that tie-break is inference.
  CHECK(loaded.world.named_object("NO_Marker") == loaded.spawned(2));
  CHECK(loaded.report.duplicate_names == 1);
}

TEST(an_object_may_belong_to_several_groups) {
  // 20 of the 21 shipped maps that declare groups have at least one object in
  // more than one of them.
  const LoadedMap loaded;
  const ObjectId shared = loaded.spawned(3);
  CHECK(loaded.world.groups().contains(loaded.world.groups().find("CampDefense"), shared));
  CHECK(loaded.world.groups().contains(loaded.world.groups().find("Reserve"), shared));
}

TEST(a_type_zero_alias_names_its_object) {
  const LoadedMap loaded;
  CHECK(loaded.world.named_object("Caesar") == loaded.spawned(5));
  CHECK(loaded.world.named_object_alive("Caesar") == loaded.spawned(5));
  CHECK(loaded.world.named_object("NoSuchName") == kNoObject);
  // `Ghosts` is an army, and armies are not in this table at all.
  CHECK(loaded.world.named_object("Ghosts") == kNoObject);
}

TEST(a_named_object_binding_outlives_the_object) {
  // `gbr.exe` gives `NamedObj` an `IsDead` member and the message
  // `Named unit %s is dead or not initialized!`. Neither means anything unless
  // the name still resolves after the object dies, so `despawn` must not
  // unbind -- which is the opposite of what it does to a group.
  LoadedMap loaded;
  const ObjectId caesar = loaded.spawned(5);
  CHECK(loaded.world.despawn(caesar));

  CHECK(loaded.world.named_objects().size() == 2);
  CHECK(loaded.world.named_object("Caesar") == caesar);   // still bound
  CHECK(loaded.world.named_object_alive("Caesar") == kNoObject);  // but dead
  // The group table did drop him.
  CHECK(members_of(loaded.world, "Reserve") == std::vector<ObjectId>{loaded.spawned(3)});
}

// --------------------------------------------------------------------------
// the query
// --------------------------------------------------------------------------

TEST(a_group_query_yields_its_members_in_first_add_order) {
  // The one query kind that does not come out ascending, because the original
  // does not sort either and 57 shipped sites apply an operation per member in
  // whatever order they arrive in. See `GroupTable` in `sim/world.hpp`.
  LoadedMap loaded;
  const ObjectId q =
      loaded.world.create_query(group_query(loaded.world.group_index("CampDefense")));

  std::vector<ObjectId> found;
  REQUIRE(loaded.world.evaluate_query(q, found) == 3);
  CHECK(found == members_of(loaded.world, "CampDefense"));
  // Spelled out as well as compared, so that the two agreeing on the *wrong*
  // order cannot pass: `CampDefense` is authored 3, 0, 1.
  CHECK(found[0] == loaded.spawned(3));
  CHECK(found[1] == loaded.spawned(0));
  CHECK(found[2] == loaded.spawned(1));

  // The query object persists and re-evaluates: that is the whole distinction
  // between a `Query` and an `ObjList`, and it is why `Group(...)` mints a
  // handle. Adding a member is visible through the same handle.
  loaded.world.groups().add(loaded.world.groups().find("CampDefense"), loaded.spawned(5));
  CHECK(loaded.world.evaluate_query(q, found) == 4);
  CHECK(found[3] == loaded.spawned(5));
}

TEST(a_group_query_over_an_index_no_table_holds_is_empty_not_wrong) {
  World world;
  const ObjectId q = world.create_query(group_query(17));
  std::vector<ObjectId> found;
  CHECK(world.evaluate_query(q, found) == 0);
  CHECK(found.empty());
}

TEST(a_group_named_only_by_a_script_starts_empty_and_fills_up) {
  // `Conquests\mediterranean` map 4 opens by folding twelve authored groups
  // into `Oasis_Guards`, which the map file does not declare, then loops on
  // `Group("Oasis_Guards").count`. The name has to be usable before anything
  // is in it.
  LoadedMap loaded;
  const std::int32_t runtime = loaded.world.group_index("Oasis_Guards");
  CHECK(runtime == 3);  // appended after the map's three armies
  CHECK(loaded.world.groups().size() == 4);

  const ObjectId q = loaded.world.create_query(group_query(runtime));
  std::vector<ObjectId> found;
  CHECK(loaded.world.evaluate_query(q, found) == 0);

  for (const ObjectId id : members_of(loaded.world, "CampDefense")) {
    loaded.world.groups().add(runtime, id);
  }
  CHECK(loaded.world.evaluate_query(q, found) == 3);
}

TEST(despawning_an_object_takes_it_out_of_every_group) {
  // `while (Group("Oasis_Guards").count != 0)` in `mediterranean` map 4 only
  // terminates if a dead member stops counting.
  LoadedMap loaded;
  const ObjectId doomed = loaded.spawned(3);
  const ObjectId q =
      loaded.world.create_query(group_query(loaded.world.group_index("CampDefense")));

  std::vector<ObjectId> found;
  CHECK(loaded.world.evaluate_query(q, found) == 3);

  CHECK(loaded.world.despawn(doomed));
  CHECK(!loaded.world.groups().contains(loaded.world.groups().find("CampDefense"), doomed));
  CHECK(!loaded.world.groups().contains(loaded.world.groups().find("Reserve"), doomed));
  CHECK(loaded.world.evaluate_query(q, found) == 2);
  CHECK(members_of(loaded.world, "Reserve").size() == 1);
}

// --------------------------------------------------------------------------
// spawn templates
// --------------------------------------------------------------------------

/// Four objects and three groups, in the shape a campaign map authors.
///
/// `flags` bit 27 (`0x08000000`) is the unspawned bit; the rest of the word is
/// copied from the live fixture above, so the two documents differ only in the
/// thing under test. `Garrison` is a template group, `Standing` a live one, and
/// `Both` overlaps them -- which is what proves a copy inherits *every*
/// membership its template had and not just the group being spawned.
constexpr std::string_view kTemplateDocument = R"(<mapobject>
	<scriptobj
		class="RHastatus"
		num="0"
	player="1"
	x="100"
	y="100"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="RHastatus"
		num="1"
	player="2"
	x="700"
	y="900"
	flags="0xA8400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="RHastatus"
		num="2"
	player="2"
	x="800"
	y="950"
	flags="0xA8400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="RHastatus"
		num="3"
	player="1"
	x="300"
	y="300"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<group
		name="Garrison"
		type="1">
		<obj
			num="2"/>
		<obj
			num="1"/>
	</group>
	<group
		name="Standing"
		type="1">
		<obj
			num="0"/>
		<obj
			num="3"/>
	</group>
	<group
		name="Both"
		type="1">
		<obj
			num="1"/>
		<obj
			num="0"/>
	</group>
	<group
		name="NO_Scipio"
		type="0">
		<obj
			num="1"/>
	</group>
	<group
		name="NO_Standing"
		type="0">
		<obj
			num="0"/>
	</group>
</mapobject>)";

struct TemplateMap {
  GroupGraph classes;
  MapObjectList map;
  World world;

  TemplateMap() {
    auto parsed = MapObjectList::parse(bytes_of(kTemplateDocument));
    map = std::move(parsed.value());
    world.populate_from_map(map, classes.graph);
  }

  /// Objects 0..3 take ids 1..4: every class resolves here.
  [[nodiscard]] static ObjectId spawned(std::int32_t num) {
    return static_cast<ObjectId>(num + 1);
  }
};

TEST(a_map_marks_its_spawn_templates_and_they_are_not_in_play) {
  TemplateMap m;
  CHECK(!m.world.find(TemplateMap::spawned(0))->state.flags.unspawned);
  CHECK(m.world.find(TemplateMap::spawned(1))->state.flags.unspawned);
  CHECK(m.world.find(TemplateMap::spawned(2))->state.flags.unspawned);
  CHECK(!m.world.find(TemplateMap::spawned(3))->state.flags.unspawned);

  // Placed, handled, and invisible to every collection path -- which is one
  // test here because `World::collect` is the funnel all of them share.
  const ObjectId q = m.world.create_query(group_query(m.world.group_index("Garrison")));
  std::vector<ObjectId> found;
  CHECK(m.world.evaluate_query(q, found) == 0);

  // But still in the world, holding their handles: a template is a real object
  // that `SpawnGroup` reads, not a placeholder the loader threw away.
  CHECK(m.world.find(TemplateMap::spawned(1)) != nullptr);
  CHECK(m.world.groups().contains(m.world.groups().find("Garrison"),
                                  TemplateMap::spawned(1)));
}

TEST(templates_in_group_yields_the_template_half_in_stored_order) {
  TemplateMap m;
  std::vector<ObjectId> out;

  // `Garrison` is authored 2, 1 -- both templates, and descending, so this
  // separates stored order from ascending order as well as from the live half.
  REQUIRE(m.world.templates_in_group(m.world.groups().find("Garrison"), out) == 2);
  CHECK(out[0] == TemplateMap::spawned(2));
  CHECK(out[1] == TemplateMap::spawned(1));

  // `Standing` has no template half at all, and `Both` has exactly one -- the
  // template it shares with `Garrison`.
  CHECK(m.world.templates_in_group(m.world.groups().find("Standing"), out) == 0);
  REQUIRE(m.world.templates_in_group(m.world.groups().find("Both"), out) == 1);
  CHECK(out[0] == TemplateMap::spawned(1));
}

TEST(spawning_a_template_copies_it_into_play_and_leaves_it_behind) {
  TemplateMap m;
  const ObjectId templ = TemplateMap::spawned(1);
  const ObjectId copy = m.world.spawn_from_template(templ);
  REQUIRE(copy != kNoObject);
  CHECK(copy != templ);

  // The copy is the template's class, place and owner, and is in play.
  const WorldObject* fresh = m.world.find(copy);
  REQUIRE(fresh != nullptr);
  CHECK(!fresh->state.flags.unspawned);
  const Point placed{700, 900};  // braces inside CHECK(...) read as a macro comma
  CHECK(fresh->state.position == placed);
  CHECK(fresh->state.owner == 1);
  CHECK(fresh->class_index == m.world.find(templ)->class_index);

  // The template survives, still out of play. It is spawned *from*, not
  // spawned: a group can be spawned more than once, and 20 of the shipped
  // sequences do exactly that in a loop.
  CHECK(m.world.find(templ) != nullptr);
  CHECK(m.world.find(templ)->state.flags.unspawned);
  CHECK(m.world.spawn_from_template(templ) != kNoObject);

  // And nothing spawns twice from a live object: only a template can be one.
  CHECK(m.world.spawn_from_template(copy) == kNoObject);
  CHECK(m.world.spawn_from_template(TemplateMap::spawned(0)) == kNoObject);
  CHECK(m.world.spawn_from_template(kNoObject) == kNoObject);
}

TEST(a_spawned_copy_inherits_every_group_its_template_was_in) {
  // The half of `CVXGroup::Spawn` that is easiest to get wrong: it walks the
  // *whole* group table testing each group's template deque, so a template in
  // two groups yields a copy in two groups. Adding the copy only to the group
  // being spawned would satisfy every count in the spawning group and quietly
  // lose the other membership.
  TemplateMap m;
  const ObjectId templ = TemplateMap::spawned(1);
  const ObjectId copy = m.world.spawn_from_template(templ);
  REQUIRE(copy != kNoObject);

  const std::int32_t garrison = m.world.groups().find("Garrison");
  const std::int32_t both = m.world.groups().find("Both");
  CHECK(m.world.groups().contains(garrison, copy));
  CHECK(m.world.groups().contains(both, copy));
  CHECK(!m.world.groups().contains(m.world.groups().find("Standing"), copy));

  // The copy is live, so it is the one a query sees -- and it is appended, so
  // it comes last in each group.
  std::vector<ObjectId> found;
  const ObjectId q = m.world.create_query(group_query(garrison));
  REQUIRE(m.world.evaluate_query(q, found) == 1);
  CHECK(found[0] == copy);

  const ObjectId qboth = m.world.create_query(group_query(both));
  REQUIRE(m.world.evaluate_query(qboth, found) == 2);
  CHECK(found[0] == TemplateMap::spawned(0));  // the live member, authored first
  CHECK(found[1] == copy);

  // And the template half is unchanged: spawning does not consume it.
  std::vector<ObjectId> templates;
  CHECK(m.world.templates_in_group(garrison, templates) == 2);
}

TEST(a_spawned_copy_takes_over_its_templates_name) {
  // `docs/formats/map.md` records "whether a named-object binding can be
  // rewritten" as unknown. It can, and this is the case that does it:
  // `NO_Scipio` names the template at load and the copy afterwards, which is
  // why `GetNamedObj("NO_Scipio").obj` is nothing until `SpawnGroup("Q_Scipio")`
  // has run in `1_Great_Battles_Zama`.
  TemplateMap m;
  const ObjectId templ = TemplateMap::spawned(1);
  CHECK(m.world.named_objects().object("NO_Scipio") == templ);
  const std::size_t before = m.world.named_objects().size();

  const ObjectId copy = m.world.spawn_from_template(templ);
  REQUIRE(copy != kNoObject);
  CHECK(m.world.named_objects().object("NO_Scipio") == copy);
  // The same entry, not a second one: the name moved rather than being added
  // beside itself.
  CHECK(m.world.named_objects().size() == before);
}

TEST(a_template_is_not_a_combatant_until_it_is_spawned) {
  // `CombatSystem::reconcile` walks `World::objects()` directly rather than
  // through `World::collect`, so the template test has to be made there too --
  // and this is what says so. Without it Zama enrols its whole reinforcement
  // schedule at turn zero: 607 of its 1,119 objects are templates.
  TemplateMap m;
  CombatSystem combat;
  combat.set_world_bound(true);
  CHECK(combat.reconcile(m.world) == 2);  // the two live objects, not four

  const ObjectId copy = m.world.spawn_from_template(TemplateMap::spawned(1));
  REQUIRE(copy != kNoObject);
  CHECK(combat.reconcile(m.world) == 3);
  CHECK(combat.find(copy) != nullptr);
  CHECK(combat.find(TemplateMap::spawned(1)) == nullptr);
}

// --------------------------------------------------------------------------
// the two spawn entry points
// --------------------------------------------------------------------------

/// A world with the template map loaded and the object-model host defined, so
/// `SpawnGroup` can be called the way a script calls it.
struct SpawnFixture {
  TemplateMap map;
  script::HostRegistry registry;
  HostContext context;

  SpawnFixture() {
    script::declare_shipped_surface(registry);
    (void)register_world_host(registry);
    (void)register_objlist_host(registry);
    context.world = &map.world;
  }

  script::HostOutcome call_member(std::string_view name, std::vector<script::Value> args) {
    return dispatch(script::CallKind::member, name,
                    static_cast<std::uint16_t>(args.size() - 1), args);
  }

  script::HostOutcome call(std::string_view name, std::vector<script::Value> args) {
    return dispatch(script::CallKind::free_function, name,
                    static_cast<std::uint16_t>(args.size()), args);
  }

  script::HostOutcome dispatch(script::CallKind kind, std::string_view name,
                               std::uint16_t arity, std::vector<script::Value>& args) {
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

  [[nodiscard]] std::span<const ObjectId> list_of(const script::Value& value) {
    return objlist_pool_of(map.world).items(objlist_of(value));
  }
};

/// `SpawnFixture` with a feeder in the world, so that "the copy joins the
/// feeding chain from this turn" is measurable at all -- `feed_from_now` is a
/// no-op in a world that has no `FeederSystem`.
///
/// It is a separate fixture rather than a field on `SpawnFixture` because two
/// of that fixture's users serialize their world and deserialize it into a bare
/// `World`. A save carries one section per registered system, so a fixture that
/// quietly gained one would make those round trips refuse -- which is exactly
/// what happened when this started life as a field.
struct SpawnFeedFixture : SpawnFixture {
  FeederSystem feeder;
  SpawnFeedFixture() { REQUIRE(map.world.add_system(&feeder)); }
};

TEST(spawn_group_returns_the_copies_it_made_as_an_objlist) {
  SpawnFixture f;
  const script::HostOutcome out = f.call("SpawnGroup", {script::Value::string("Garrison")});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));

  // Two templates, two copies, in the order the group holds them -- authored
  // 2, 1, which is neither ascending nor the order their copies' ids run in
  // being decided by anything else.
  const std::span<const ObjectId> made = f.list_of(out.value);
  REQUIRE(made.size() == 2);
  CHECK(f.map.world.find(made[0])->state.position.x == 800);  // template num=2
  CHECK(f.map.world.find(made[1])->state.position.x == 700);  // template num=1

  // And they are in play, where the templates still are not.
  CHECK(!f.map.world.find(made[0])->state.flags.unspawned);
  CHECK(f.map.world.find(TemplateMap::spawned(2))->state.flags.unspawned);
}

TEST(spawn_group_does_not_mint_the_group_it_fails_to_find) {
  // The miss is a printed diagnostic and an empty list in the original, not a
  // refusal -- and `find` rather than `intern` is what keeps a failed spawn
  // from leaving a new empty group behind for `Group(...)` to resolve later.
  SpawnFixture f;
  const std::size_t before = f.map.world.groups().size();

  const script::HostOutcome out = f.call("SpawnGroup", {script::Value::string("NoSuchGroup")});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  CHECK(f.list_of(out.value).empty());
  CHECK(f.map.world.groups().size() == before);
  CHECK(f.map.world.groups().find("NoSuchGroup") == GroupTable::kNoGroup);
}

TEST(spawn_group_does_not_respawn_the_copies_it_just_made) {
  // The copies join the group being spawned, because a copy inherits its
  // template's memberships. Walking the group's template half as it grows
  // would be fine -- copies are not templates -- but walking the *members*
  // would spawn forever, and snapshotting is what makes the distinction
  // impossible to get wrong.
  SpawnFixture f;
  const std::int32_t garrison = f.map.world.groups().find("Garrison");
  CHECK(f.map.world.groups().members(garrison).size() == 2);

  const script::HostOutcome first = f.call("SpawnGroup", {script::Value::string("Garrison")});
  REQUIRE(first.status == script::HostStatus::ok);
  CHECK(f.list_of(first.value).size() == 2);
  CHECK(f.map.world.groups().members(garrison).size() == 4);

  // Spawning again spawns the two templates again, not the four members.
  const script::HostOutcome second = f.call("SpawnGroup", {script::Value::string("Garrison")});
  REQUIRE(second.status == script::HostStatus::ok);
  CHECK(f.list_of(second.value).size() == 2);
  CHECK(f.map.world.groups().members(garrison).size() == 6);
}

/// `SpawnNamed` is `SpawnGroup` for a name that holds exactly one object, and
/// it hands back the copy rather than a list.
///
/// The three ways it answers with nothing are all `ok` and none of them
/// refuses: an unknown name, a name whose object has gone, and -- the one with
/// no diagnostic behind it in `gbr.exe` -- a name bound to something that is
/// not a template. The third is what makes a second `SpawnNamed` of the same
/// name a no-op: the first call rebinds the name onto the copy it just made,
/// and a copy is not a template.
TEST(spawn_named_brings_one_template_in_and_moves_the_name_onto_the_copy) {
  SpawnFeedFixture f;
  const ObjectId templ = TemplateMap::spawned(1);
  REQUIRE(f.map.world.named_objects().object("NO_Scipio") == templ);

  const script::HostOutcome out = f.call("SpawnNamed", {script::Value::string("NO_Scipio")});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(out.value.is_object());
  const ObjectId copy = out.value.as_object().id;
  REQUIRE(copy != kNoObject);
  CHECK(copy != templ);

  // `Obj`, not `NamedObj`: the registration declares return type 20.
  CHECK(out.value.as_object().type == kTypeObj);

  // In play, where its template still is not, and carrying what the template
  // carried.
  CHECK(!f.map.world.find(copy)->state.flags.unspawned);
  CHECK(f.map.world.find(templ)->state.flags.unspawned);
  CHECK(f.map.world.find(copy)->state.position.x == 700);

  // The name followed it, and the group memberships came with it -- both
  // through `spawn_from_template`, which is why `SpawnNamed` does not have to
  // decide either.
  CHECK(f.map.world.named_objects().object("NO_Scipio") == copy);
  CHECK(f.map.world.groups().contains(f.map.world.groups().find("Garrison"), copy));
  CHECK(f.map.world.groups().contains(f.map.world.groups().find("Both"), copy));

  // And it is in the feeding chain from this turn, not from the next
  // reconcile: a reinforcement that arrives mid-sequence has to be fed before
  // the sequence that spawned it starts giving it orders.
  CHECK(f.feeder.find(copy) != nullptr);
  CHECK(f.feeder.find(templ) == nullptr);

  // Again: the name now points at the copy, and a copy is not a template.
  const std::size_t objects = f.map.world.objects().size();
  const script::HostOutcome again = f.call("SpawnNamed", {script::Value::string("NO_Scipio")});
  CHECK(again.status == script::HostStatus::ok);
  CHECK(again.value.is_object() && again.value.as_object().type == script::kNoType);
  CHECK(f.map.world.objects().size() == objects);
}

TEST(spawn_named_answers_nothing_for_a_name_that_names_nothing_spawnable) {
  SpawnFixture f;

  // A name no map declares. The original prints and hands back the invalid
  // handle; it does not refuse and it does not mint the name.
  const script::HostOutcome missing =
      f.call("SpawnNamed", {script::Value::string("NO_NotDeclared")});
  CHECK(missing.status == script::HostStatus::ok);
  CHECK(missing.value.is_object() && missing.value.as_object().type == script::kNoType);
  CHECK(f.map.world.named_objects().find("NO_NotDeclared") == NamedObjectTable::kNoName);

  // A name that is declared and bound to an object already in play.
  REQUIRE(f.map.world.named_objects().object("NO_Standing") == TemplateMap::spawned(0));
  const std::size_t objects = f.map.world.objects().size();
  const script::HostOutcome live =
      f.call("SpawnNamed", {script::Value::string("NO_Standing")});
  CHECK(live.status == script::HostStatus::ok);
  CHECK(live.value.is_object() && live.value.as_object().type == script::kNoType);
  CHECK(f.map.world.objects().size() == objects);
  CHECK(f.map.world.named_objects().object("NO_Standing") == TemplateMap::spawned(0));
}

TEST(spawn_group_in_holder_dispatches_on_its_second_arguments_type) {
  // Registered twice at arity 2 in `gbr.exe` -- `(str, str)` at 0x0057782f and
  // `(str, Obj)` at 0x00577847 -- and this registry keys on
  // `(kind, name, arity)`, so one body has to tell them apart by the value it
  // is handed. Both forms ship: 124 corpus sites pass a settlement name and
  // one passes an object.
  SpawnFixture f;

  // The object form: the copies go inside the holder, which means they have no
  // position of their own.
  const ObjectId holder = TemplateMap::spawned(0);
  const script::HostOutcome held = f.call(
      "SpawnGroupInHolder",
      {script::Value::string("Garrison"),
       script::Value::object(script::ObjectRef{kTypeObj, holder})});
  REQUIRE(held.status == script::HostStatus::ok);
  const std::span<const ObjectId> inside = f.list_of(held.value);
  REQUIRE(inside.size() == 2);
  CHECK(f.map.world.state(inside[0])->holder == holder);
  CHECK(f.map.world.resolve_position(inside[0]) == f.map.world.resolve_position(holder));

  // The string form with a name no settlement carries: a diagnostic and an
  // empty list, the same shape as an unknown group.
  const script::HostOutcome missing =
      f.call("SpawnGroupInHolder",
             {script::Value::string("Garrison"), script::Value::string("S_NoSuchTown")});
  REQUIRE(missing.status == script::HostStatus::ok);
  CHECK(f.list_of(missing.value).empty());
}

// --------------------------------------------------------------------------
// the session seam
// --------------------------------------------------------------------------
//
// The tests above build a world by hand, so they would all still pass with
// `GameSession` doing nothing about templates at all -- which is the shape of
// failure this project keeps finding. These go through `GameSession::create`
// and `advance`, which is the only path a shipped map takes.

namespace {

/// A `ScriptResolver` over one in-memory file.
class OneScript final : public ScriptResolver {
 public:
  OneScript(std::string path, std::string text)
      : path_(std::move(path)), text_(std::move(text)) {}
  std::span<const std::byte> source(std::string_view path) override {
    return path == path_ ? bytes_of(text_) : std::span<const std::byte>{};
  }

 private:
  std::string path_;
  std::string text_;
};

/// Two classes, both binding an `idle` method, so that every object the map
/// places wants a script and the only thing deciding whether it gets one is
/// whether it is in play.
ClassGraph idle_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "test_group.cpp");
  graph.add(bytes_of(R"(<class id="RHastatus" parent="Object" cpp_class="CVXUnit">
      <properties sight="100" maxhealth="50" maxstamina="10"/>
      <method sig="idle" vs="unit_idle.vs"/>
    </class>)"),
            "test_group.cpp");
  graph.link();
  return graph;
}

}  // namespace

TEST(a_session_leaves_templates_out_and_picks_up_what_is_spawned) {
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = idle_graph();
  OneScript scripts("unit_idle.vs", "//void, Obj This\nSleep(100000);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kTemplateDocument);

  auto session = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();

  // Two of the four objects are templates, so two idle scripts start, not
  // four. Without the test in `start_object_scripts` a campaign map puts its
  // whole reinforcement schedule on the scheduler at turn zero.
  // Opt in, the way `imrun` and the app do. `GameSession::create` does not do
  // it for you, and the per-turn sweep respects that.
  REQUIRE(run.start_object_scripts() == 2);
  CHECK(run.report().scripts_started == 2);
  const CombatSystem* fight = combat_system_of(run.world());
  REQUIRE(fight != nullptr);
  CHECK(fight->combatants().size() == 2);

  // Turns pass and nothing is started again. The sweep runs every turn now, so
  // without a watermark it would re-spawn a script per object per turn -- and
  // the count is what says so, because the scheduler would happily take them.
  run.advance(3, 400);
  CHECK(run.report().scripts_started == 2);

  // Now bring one template in, the way `SpawnGroup` does.
  const ObjectId copy =
      run.world().spawn_from_template(static_cast<ObjectId>(2));
  REQUIRE(copy != kNoObject);

  // It has no script yet: spawning is `World`'s and scripts are the session's.
  CHECK(run.report().scripts_started == 2);

  // One turn later it does, and it has enrolled. Without the per-turn sweep a
  // spawned unit stands still for the rest of the match with its class's
  // behaviour never running.
  run.advance(1, 400);
  CHECK(run.report().scripts_started == 3);
  CHECK(fight->combatants().size() == 3);
  CHECK(fight->find(copy) != nullptr);

  // And still nothing more on the turn after that.
  run.advance(1, 400);
  CHECK(run.report().scripts_started == 3);
}

TEST(a_load_restores_the_script_watermark_rather_than_the_fresh_sessions) {
  // The save sweep's Balcans and Crossroads divergence, in four objects. A
  // session's script watermark -- the first object id `start_object_scripts`
  // has not considered -- was process state the save did not carry. The
  // restored session had been `create`d and started over the *map's* objects,
  // so its watermark was the map's `next_id()`; after the load put back a world
  // with objects spawned since, the first `advance` offered every one of them
  // `idle` again. A trained unit walking into its town under an `enter`
  // command, whose own idle had been retired, got a second `UNIT_IDLE.VS`, and
  // that script's `Stop(1000)` dropped the route the original was still
  // walking. Nothing hashed it: a script's existence is not in any channel
  // until it does something.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = idle_graph();
  OneScript scripts("unit_idle.vs", "//void, Obj This\nSleep(100000);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kTemplateDocument);

  auto original = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(original.ok());
  GameSession& run = *original.value();
  REQUIRE(run.start_object_scripts() == 2);
  run.advance(1, 400);

  // A spawn, and a turn for the sweep to give it its idle: three scripts, and
  // the watermark past the copy.
  const ObjectId copy = run.world().spawn_from_template(static_cast<ObjectId>(2));
  REQUIRE(copy != kNoObject);
  run.advance(1, 400);
  REQUIRE(run.report().scripts_started == 3);
  REQUIRE(run.script_watermark() == run.world().next_id());
  // Retire the copy's idle the way a command does, so that the only thing
  // that could bring one back is the sweep running over it a second time.
  {
    std::size_t killed = 0;
    for (const script::ScriptRecord& record : run.scheduler().scripts()) {
      if (record.owner.id == copy && !record.dead) {
        REQUIRE(run.scheduler().kill(record.id));
        ++killed;
      }
    }
    REQUIRE(killed == 1);
  }
  run.advance(1, 400);
  const std::size_t live_before = run.scheduler().live_count();
  REQUIRE(live_before == 2);

  const Result<std::vector<std::byte>> saved = run.save("test");
  REQUIRE(saved.ok());

  // The fresh session, the way every embedder builds one: created and started
  // over the map, which puts its watermark at the map's `next_id()` -- one
  // below the copy's id.
  auto fresh = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(fresh.ok());
  GameSession& restored = *fresh.value();
  REQUIRE(restored.start_object_scripts() == 2);
  REQUIRE(restored.script_watermark() < copy + 1);

  SessionLoadReport report;
  REQUIRE(restored.load(saved.value(), "test", &report).ok());
  CHECK(report.unconsumed.empty());
  CHECK(restored.script_watermark() == run.script_watermark());
  CHECK(restored.scheduler().live_count() == live_before);
  // `load` resets the started counter to what it loaded, so a turn that starts
  // nothing leaves it there.
  const std::size_t started_at_load = restored.report().scripts_started;
  CHECK(started_at_load == live_before);

  // The observable: a turn after the load starts nothing on either side. With
  // the watermark left at the fresh session's, the restored side would start
  // one -- and on a shipped map, that one calls `Stop`.
  run.advance(1, 400);
  restored.advance(1, 400);
  CHECK(restored.scheduler().live_count() == run.scheduler().live_count());
  CHECK(restored.scheduler().live_count() == 2);
  CHECK(restored.report().scripts_started == started_at_load);

  // Loaded into a session nobody started -- which `load` says is fine, and
  // what a late joiner's harness does -- the sweep still runs, because the
  // saved game ran it: a copy spawned after the load gets its `idle` on both
  // sides. Taken from the loading session, it did not, and the joiner parted
  // from its peers a few turns into a war (`imconform netjoin --skirmish`).
  {
    auto cold = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(cold.ok());
    GameSession& joiner = *cold.value();
    REQUIRE(joiner.load(saved.value(), "test").ok());
    const std::size_t running_before = run.scheduler().live_count();
    REQUIRE(joiner.scheduler().live_count() == running_before);
    const ObjectId late = run.world().spawn_from_template(static_cast<ObjectId>(2));
    REQUIRE(joiner.world().spawn_from_template(static_cast<ObjectId>(2)) == late);
    run.advance(1, 400);
    joiner.advance(1, 400);
    CHECK(run.scheduler().live_count() == running_before + 1);
    CHECK(joiner.scheduler().live_count() == run.scheduler().live_count());
  }

  // The converse: a save of a game that never ran object scripts does not
  // start running them because the session it is loaded into had.
  {
    auto bare = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(bare.ok());
    bare.value()->advance(1, 400);
    const Result<std::vector<std::byte>> unscripted = bare.value()->save("test");
    REQUIRE(unscripted.ok());
    auto started = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(started.ok());
    REQUIRE(started.value()->start_object_scripts() == 2);
    REQUIRE(started.value()->load(unscripted.value(), "test").ok());
    CHECK(started.value()->scheduler().live_count() == 0);
    started.value()->advance(1, 400);
    CHECK(started.value()->scheduler().live_count() == 0);
  }

  // A save without the section is a game this build cannot resume: refused,
  // not defaulted. Built by hand, because nothing in this tree can write one.
  {
    std::vector<std::byte> bytes = saved.value();
    const Result<SaveReader> reader = SaveReader::open(bytes);
    REQUIRE(reader.ok());
    SaveWriter writer(reader->meta());
    for (const std::string& name : reader->names()) {
      if (name == "meta" || name == kSessionSection) continue;
      REQUIRE(writer.add(name, reader->section(name)).ok());
    }
    const std::vector<std::byte> without = writer.finish();
    auto again = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(again.ok());
    REQUIRE(again.value()->start_object_scripts() == 2);
    CHECK(!again.value()->load(without, "test").ok());
  }
}

TEST(a_session_that_never_asked_for_object_scripts_still_does_not_get_them) {
  // The per-turn sweep exists for spawned objects, and it must not turn into a
  // second way of starting scripts nobody asked for. `GameSession::create`
  // deliberately does not start them -- a conformance run drives the systems
  // with scripts off on purpose -- so `advance` has to inherit that choice.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = idle_graph();
  OneScript scripts("unit_idle.vs", "//void, Obj This\nSleep(100000);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kTemplateDocument);

  auto session = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();

  run.advance(5, 400);
  CHECK(run.report().scripts_started == 0);

  // And a spawn does not smuggle one in either.
  REQUIRE(run.world().spawn_from_template(static_cast<ObjectId>(2)) != kNoObject);
  run.advance(2, 400);
  CHECK(run.report().scripts_started == 0);
}

TEST(add_to_group_creates_the_group_and_remove_from_group_refuses_it) {
  // The one asymmetry in the family, and it is `gbr.exe`'s: a
  // `Could not find group named '%s'` diagnostic exists for every form of
  // `RemoveFromGroup` and for none of `AddToGroup`. `mediterranean` map 4
  // needs it -- twelve `AddToGroup` calls build `Oasis_Guards`, which no map
  // file declares, and then it loops on that group's count.
  SpawnFixture f;
  World& world = f.map.world;
  const std::size_t before = world.groups().size();
  const ObjectId live = TemplateMap::spawned(0);

  REQUIRE(f.call_member("AddToGroup", {f.obj(live), script::Value::string("Oasis_Guards")})
              .status == script::HostStatus::ok);
  CHECK(world.groups().size() == before + 1);
  const std::int32_t oasis = world.groups().find("Oasis_Guards");
  REQUIRE(oasis != GroupTable::kNoGroup);
  CHECK(world.groups().contains(oasis, live));

  // Removing from a name nothing holds mints nothing and changes nothing.
  REQUIRE(f.call_member("RemoveFromGroup", {f.obj(live), script::Value::string("NoSuchGroup")})
              .status == script::HostStatus::ok);
  CHECK(world.groups().size() == before + 1);
  CHECK(world.groups().find("NoSuchGroup") == GroupTable::kNoGroup);

  // And removing from one that exists takes exactly that membership.
  REQUIRE(f.call_member("RemoveFromGroup", {f.obj(live), script::Value::string("Oasis_Guards")})
              .status == script::HostStatus::ok);
  CHECK(!world.groups().contains(oasis, live));
  CHECK(world.groups().size() == before + 1);  // the empty group keeps its index
}

TEST(membership_takes_an_object_a_list_or_a_query_as_its_receiver) {
  // Each of these is three registrations in `gbr.exe` -- `Obj`, `ObjList` and
  // `Query` -- and one entry here, because the registry keys on
  // (kind, name, arity). One body, three receivers, and if it handled only the
  // object form the other two would silently do nothing.
  SpawnFixture f;
  World& world = f.map.world;

  // The query form: `Group("Standing").AddToGroup("Muster")` folds one whole
  // group into another, which is `mediterranean`'s shape exactly.
  const ObjectId standing = world.create_query(group_query(world.group_index("Standing")));
  REQUIRE(f.call_member("AddToGroup",
                        {script::Value::object(script::ObjectRef{kTypeObj, standing}),
                         script::Value::string("Muster")})
              .status == script::HostStatus::ok);
  const std::int32_t muster = world.groups().find("Muster");
  REQUIRE(muster != GroupTable::kNoGroup);
  // `Standing` is authored 0, 3 -- so this also says the receiver was expanded
  // in the query's order rather than sorted on the way through.
  REQUIRE(world.groups().members(muster).size() == 2);
  CHECK(world.groups().members(muster)[0] == TemplateMap::spawned(0));
  CHECK(world.groups().members(muster)[1] == TemplateMap::spawned(3));

  // And `RemoveFromAllGroups` on a list empties every membership it has.
  ObjListPool& pool = objlist_pool_of(world);
  const ObjListId list = pool.acquire_temporary(7);
  *pool.mutable_items(list) = {TemplateMap::spawned(0), TemplateMap::spawned(3)};
  REQUIRE(f.call_member("RemoveFromAllGroups", {make_objlist_value(list)}).status ==
          script::HostStatus::ok);
  CHECK(world.groups().members(muster).empty());
  CHECK(world.groups().members(world.groups().find("Standing")).empty());
}

/// A map that authors `UnitFlags`, which is the second flag word and not the
/// `flags` attribute beside it.
///
/// The four values are the ones the retail install actually contains: the
/// ordinary soldier's `0x00040000`, the 708 units carrying nothing, an airborne
/// bird's `0x02400000` (bit 22 beside the unexplained bit 25), and `0x00060000`
/// -- bit 18 with bit 17, which nothing explains and which must not disturb the
/// bit beside it. The fifth object carries no attribute at all, and the sixth
/// carries `0x02000000` alone: bit 25 is on 253 of the 254 flying units and
/// only 46 of those are in the air, so it is the bit a reader that took the
/// wrong one of the pair would key off.
constexpr std::string_view kUnitFlagsDocument = R"(<mapobject>
	<scriptobj class="RHastatus" num="0" player="1" x="100" y="100"
		flags="0xA0400002" UnitFlags="262144" dir.x="0" dir.y="1"/>
	<scriptobj class="RHastatus" num="1" player="1" x="200" y="200"
		flags="0xA0400002" UnitFlags="0" dir.x="0" dir.y="1"/>
	<scriptobj class="RHastatus" num="2" player="1" x="300" y="300"
		flags="0xA0400002" UnitFlags="37748736" dir.x="0" dir.y="1"/>
	<scriptobj class="RHastatus" num="3" player="1" x="400" y="400"
		flags="0xA0400002" UnitFlags="393216" dir.x="0" dir.y="1"/>
	<scriptobj class="RHastatus" num="4" player="1" x="500" y="500"
		flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RHastatus" num="5" player="1" x="600" y="600"
		flags="0xA0400002" UnitFlags="33554432" dir.x="0" dir.y="1"/>
</mapobject>)";

/// Three settlements whose `id` attributes are 7, 3 and 9 at document positions
/// 0, 1 and 2.
///
/// **The mismatch is the point.** `MapObject::settlement` is an index and
/// `destination_set` is an `id`, and the two disagree on 97 settlements across
/// the retail install, so a reader that subscripted with the attribute would
/// pair the wrong teleports on a real map and the right ones on any fixture
/// where the two happen to line up.
///
/// The middle settlement holds no teleport, so a pairing that walked settlement
/// *positions* rather than ids would find it and stop.
constexpr std::string_view kTeleportDocument = R"(<mapobject>
	<settlement id="7" player="1" classoffirstbuilding="Teleport_1" name="north">
		<scriptobj class="Teleport_1" num="0" destination_set="9" player="1"
			x="100" y="100" flags="0x80800008" dir.x="0" dir.y="1"/>
	</settlement>
	<settlement id="3" player="1" classoffirstbuilding="RHastatus" name="camp">
		<scriptobj class="RHastatus" num="1" player="1" x="200" y="200"
			flags="0xA0400002" dir.x="0" dir.y="1"/>
	</settlement>
	<settlement id="9" player="1" classoffirstbuilding="Teleport_1" name="south">
		<scriptobj class="Teleport_1" num="2" destination_set="7" player="1"
			x="300" y="300" flags="0x80800008" dir.x="0" dir.y="1"/>
	</settlement>
	<scriptobj class="Teleport_1" num="3" destination_set="4" player="1"
		x="400" y="400" flags="0x80800008" dir.x="0" dir.y="1"/>
</mapobject>)";

/// **The map authors the teleport pair, through the settlement it belongs to.**
///
/// `Teleport::destination` is one field read on the instance; what took a
/// census was where the field comes from. A teleport's `<scriptobj>` carries
/// `destination_set`, which names the far teleport's *settlement*, and the
/// three properties that make that resolvable -- every teleport in a
/// settlement, no settlement with two, the relation symmetric -- hold in 54 of
/// 54 across the retail install.
TEST(the_map_pairs_teleports_through_their_settlements_own_id) {
  TeleportGraph classes;
  auto parsed = MapObjectList::parse(bytes_of(kTeleportDocument));
  REQUIRE(static_cast<bool>(parsed));
  MapObjectList map = std::move(parsed.value());

  // The attribute reaches the reader at all, and absence is -1 rather than 0 --
  // settlement 0 is a real settlement on every shipped map.
  CHECK(map.objects()[0].destination_set == 9);
  CHECK(map.objects()[1].destination_set == -1);
  CHECK(map.objects()[3].destination_set == 4);

  World world;
  const World::PopulateReport report = world.populate_from_map(map, classes.graph);
  CHECK(report.teleport_pairs == 2);

  const auto pair_of = [&](ObjectId id) -> ObjectId {
    const WorldObject* slot = world.find(id);
    if (slot == nullptr) {
      CHECK(false);
      return kNoObject;
    }
    return slot->state.teleport_destination;
  };

  // The two teleports point at each other. Ids are 1-based in spawn order and
  // the settlement composites are allocated ahead of their members, so the
  // objects are not 1, 2, 3 -- they are found by walking rather than assumed.
  ObjectId north = kNoObject;
  ObjectId south = kNoObject;
  ObjectId orphan = kNoObject;
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.class_index != classes.teleport) continue;
    if (slot.state.position.x == 100) north = slot.id;
    if (slot.state.position.x == 300) south = slot.id;
    if (slot.state.position.x == 400) orphan = slot.id;
  }
  REQUIRE(north != kNoObject);
  REQUIRE(south != kNoObject);
  REQUIRE(orphan != kNoObject);
  CHECK(pair_of(north) == south);
  CHECK(pair_of(south) == north);

  // A teleport outside any settlement, naming a settlement that holds none, is
  // left unpaired rather than pointing at whatever object happened to be there.
  CHECK(pair_of(orphan) == kNoObject);

  // And the object that is not a teleport carries no pair.
  for (const WorldObject& slot : world.objects()) {
    if (slot.class_index == classes.unit) CHECK(slot.state.teleport_destination == kNoObject);
  }
}

TEST(the_map_authors_the_no_ai_flag_and_forty_six_birds_start_airborne) {
  GroupGraph classes;
  auto parsed = MapObjectList::parse(bytes_of(kUnitFlagsDocument));
  REQUIRE(static_cast<bool>(parsed));
  MapObjectList map = std::move(parsed.value());
  World world;
  world.populate_from_map(map, classes.graph);

  const auto flags_of = [&](std::int32_t num) -> ObjectFlags {
    const WorldObject* slot = world.find(static_cast<ObjectId>(num + 1));
    if (slot == nullptr) {
      CHECK(false);
      return ObjectFlags{};
    }
    return slot->state.flags;
  };

  // Bit 18 is the ordinary case and the whole reason this is read at all: a
  // world that left it false would put every soldier a campaign places into
  // `TOWNHALL_AUTOTRAIN.VS`'s training list on the first tick.
  CHECK(flags_of(0).no_ai);
  CHECK(!flags_of(0).in_air);
  // No bits, an authored zero, and no attribute at all are the same answer.
  CHECK(!flags_of(1).no_ai);
  CHECK(!flags_of(4).no_ai);
  // Bit 22 without bit 18: airborne and the AI's.
  CHECK(flags_of(2).in_air);
  CHECK(!flags_of(2).no_ai);
  // And bit 25 *without* bit 22 is a flying unit standing on the ground, which
  // is 207 of the install's 253. The two travel together on the 46 that are up.
  CHECK(!flags_of(5).in_air);
  CHECK(!flags_of(5).no_ai);
  // And bit 17, which nothing explains, neither sets nor clears its neighbour.
  CHECK(flags_of(3).no_ai);
  CHECK(!flags_of(3).in_air);
}

/// The teleport pair as the scripts see it, and the mask that records who has
/// been through.
///
/// `Traverse` is the odd one: it is the only entry point in this family that
/// *writes*, and it writes something no shipped script reads back. The six
/// sites call it on both ends of a pair immediately after moving a unit
/// themselves, so what it does is keep a record; see
/// `ObjectState::traversed_by`.
TEST(the_teleport_pair_and_its_traversal_mask_round_trip_and_are_hashed) {
  SpawnFixture f;
  World& world = f.map.world;
  const ObjectId here = TemplateMap::spawned(0);
  const ObjectId there = TemplateMap::spawned(3);

  const auto destination_of = [&](ObjectId id) {
    const script::HostOutcome out = f.call_member("destination", {f.obj(id)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() ? out.value.as_object().id : kNoObject;
  };

  // Unpaired -- the map fixture has no teleports -- and the answer is the
  // invalid handle rather than a refusal, because every shipped site chains
  // straight off it (`tel.destination.pos`, `tel.destination.settlement`).
  CHECK(destination_of(here) == kNoObject);
  CHECK(f.call_member("destination", {f.obj(kNoObject)}).status == script::HostStatus::ok);

  world.mutable_state(here)->teleport_destination = there;
  CHECK(destination_of(here) == there);
  // One direction only: the map is what makes the relation symmetric, not this.
  CHECK(destination_of(there) == kNoObject);

  // A pair whose far end has been destroyed reads as nothing rather than as a
  // live object. Ids are monotone and never reused, so `find` answering null
  // is the whole check.
  REQUIRE(world.despawn(there));
  CHECK(destination_of(here) == kNoObject);
  CHECK(world.state(here)->teleport_destination == there);  // the field is untouched

  // The mask. Players are 1-based on the way in and the bit is 0-based.
  CHECK(world.state(here)->traversed_by == 0);
  REQUIRE(f.call_member("Traverse", {f.obj(here), script::Value::integer(1)}).status ==
          script::HostStatus::ok);
  CHECK(world.state(here)->traversed_by == 0x1u);
  REQUIRE(f.call_member("Traverse", {f.obj(here), script::Value::integer(16)}).status ==
          script::HostStatus::ok);
  CHECK(world.state(here)->traversed_by == 0x8001u);
  // It ORs: a second visit by the same player changes nothing.
  REQUIRE(f.call_member("Traverse", {f.obj(here), script::Value::integer(1)}).status ==
          script::HostStatus::ok);
  CHECK(world.state(here)->traversed_by == 0x8001u);

  // **The original's bounds check has no lower half**, so `Traverse(0)` indexes
  // one stride below the player table there. Guarded rather than mirrored, and
  // both ends of the range answer the same way: nothing happens, quietly.
  for (const std::int32_t bad : {0, -1, 17, 1000}) {
    CHECK(f.call_member("Traverse", {f.obj(here), script::Value::integer(bad)}).status ==
          script::HostStatus::ok);
    CHECK(world.state(here)->traversed_by == 0x8001u);
  }
  // A receiver that names nothing is silence, not a refusal.
  CHECK(f.call_member("Traverse", {f.obj(kNoObject), script::Value::integer(2)}).status ==
        script::HostStatus::ok);

  // **Both are hashed**, which is the claim `ObjectState::teleport_destination`
  // makes and the reason the golden trace moved when they landed.
  const std::uint64_t before = world.state_hash();
  (void)f.call_member("Traverse", {f.obj(here), script::Value::integer(3)});
  const std::uint64_t after = world.state_hash();
  CHECK(after != before);
  world.mutable_state(here)->teleport_destination = kNoObject;
  CHECK(world.state_hash() != after);

  // And both survive the round trip.
  world.mutable_state(here)->teleport_destination = here;
  std::vector<std::byte> saved;
  world.serialize(saved);
  World restored;
  restored.set_class_graph(world.class_graph());
  REQUIRE(restored.deserialize(saved).ok());
  const ObjectState* back = restored.state(here);
  REQUIRE(back != nullptr);
  CHECK(back->teleport_destination == here);
  CHECK(back->traversed_by == 0x8005u);
  CHECK(restored.state_hash() == world.state_hash());

  // A version-22 world has neither field, so its object records are eight bytes
  // shorter apiece and there is no honest way to widen one.
  std::vector<std::byte> older = saved;
  REQUIRE(older.size() > 8);
  older[4] = std::byte{22};
  older[5] = std::byte{0};
  older[6] = std::byte{0};
  older[7] = std::byte{0};
  World refused;
  refused.set_class_graph(world.class_graph());
  CHECK(!refused.deserialize(older).ok());
}

/// The tower's manual target and the druid's transformation flag: two new
/// fields on the object header, one of them the only thing here the *interface*
/// writes and the simulation reads.
TEST(the_ui_target_and_the_summoning_flag_round_trip_and_are_hashed) {
  SpawnFixture f;
  World& world = f.map.world;
  const ObjectId tower = TemplateMap::spawned(0);
  const ObjectId prey = TemplateMap::spawned(3);

  const auto target_of = [&] {
    const script::HostOutcome out = f.call_member("GetUITarget", {f.obj(tower)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() ? out.value.as_object().id : kNoObject;
  };

  // Nothing clicked yet, and the answer is the invalid handle rather than a
  // refusal -- `TOWER_GUARD.VS` writes `target = .GetUITarget.AsUnit; if
  // (target.IsAlive)`, so it has to flow through `AsUnit` and fail there.
  CHECK(target_of() == kNoObject);
  CHECK(f.call_member("GetUITarget", {f.obj(kNoObject)}).status == script::HostStatus::ok);

  REQUIRE(f.call_member("SetUITarget", {f.obj(tower), f.obj(prey)}).status ==
          script::HostStatus::ok);
  CHECK(target_of() == prey);
  CHECK(world.state(tower)->ui_target == prey);
  // On the receiver and nobody else.
  CHECK(world.state(prey)->ui_target == kNoObject);
  // A handle that names nothing clears it rather than storing rubbish.
  REQUIRE(f.call_member("SetUITarget", {f.obj(tower), f.obj(kNoObject)}).status ==
          script::HostStatus::ok);
  CHECK(target_of() == kNoObject);

  // The druid's flag. **The setter has no guard and no message**, which is what
  // the missing receiver checks here: silence, not a refusal.
  const auto summoning = [&](ObjectId id) {
    return f.call_member("IsSummoningDeath", {f.obj(id)}).value.as_integer() != 0;
  };
  CHECK(!summoning(tower));
  REQUIRE(f.call_member("SetSummoningDeath", {f.obj(tower), script::Value::boolean(true)})
              .status == script::HostStatus::ok);
  CHECK(summoning(tower));
  CHECK(!summoning(prey));
  CHECK(f.call_member("SetSummoningDeath", {f.obj(kNoObject), script::Value::boolean(true)})
            .status == script::HostStatus::ok);
  // It clears as well as sets: a body that only ever set it would pass
  // everything above.
  REQUIRE(f.call_member("SetSummoningDeath", {f.obj(tower), script::Value::boolean(false)})
              .status == script::HostStatus::ok);
  CHECK(!summoning(tower));

  // **Both are hashed**, which is the claim `ObjectState::ui_target` makes and
  // the reason the golden trace moved when they landed.
  const std::uint64_t clean = world.state_hash();
  (void)f.call_member("SetUITarget", {f.obj(tower), f.obj(prey)});
  const std::uint64_t targeted = world.state_hash();
  CHECK(targeted != clean);
  (void)f.call_member("SetSummoningDeath", {f.obj(tower), script::Value::boolean(true)});
  CHECK(world.state_hash() != targeted);

  // And both survive the round trip, which is what the version bump buys.
  std::vector<std::byte> saved;
  world.serialize(saved);
  World restored;
  restored.set_class_graph(world.class_graph());
  REQUIRE(restored.deserialize(saved).ok());
  const ObjectState* back = restored.state(tower);
  REQUIRE(back != nullptr);
  CHECK(back->ui_target == prey);
  CHECK(back->flags.summoning_death);
  CHECK(restored.state_hash() == world.state_hash());

  // **And the version had to move with them.** A version-21 world has neither
  // field, so its object records are four bytes shorter apiece and there is no
  // honest way to widen one; the header refuses it rather than mis-decoding.
  // Without this the only thing pinning the bump would be a comment.
  REQUIRE(saved.size() > 8);
  CHECK(static_cast<char>(saved[0]) == 'I');
  CHECK(static_cast<char>(saved[3]) == 'D');
  std::vector<std::byte> older = saved;
  older[4] = std::byte{21};
  older[5] = std::byte{0};
  older[6] = std::byte{0};
  older[7] = std::byte{0};
  World refused;
  refused.set_class_graph(world.class_graph());
  CHECK(!refused.deserialize(older).ok());
}

TEST(clamp_returns_early_below_the_low_bound_and_the_idiom_does_not) {
  SpawnFixture f;
  const auto clamp = [&](std::int64_t v, std::int64_t lo, std::int64_t hi) {
    return f.call("CLAMP", {script::Value::integer(v), script::Value::integer(lo),
                            script::Value::integer(hi)})
        .value.as_integer();
  };

  // The ordinary three cases, value first.
  CHECK(clamp(5, 0, 10) == 5);
  CHECK(clamp(-3, 0, 10) == 0);
  CHECK(clamp(99, 0, 10) == 10);
  CHECK(clamp(0, 0, 10) == 0);   // inclusive at both bounds
  CHECK(clamp(10, 0, 10) == 10);

  // **The trap.** `0x00696070` returns at once when `v < lo` and never compares
  // `hi`, so with the bounds crossed the *high* bound wins for any value at or
  // above `lo` -- which `max(lo, min(v, hi))` gets backwards.
  CHECK(clamp(9, 5, 2) == 2);    // the idiom answers 5
  CHECK(clamp(1, 5, 2) == 5);    // below `lo`: the early return, and they agree
  CHECK(clamp(3, 5, 2) == 5);
  CHECK(clamp(14, 10, -10) == -10);  // at or above `lo`: the high bound wins
  CHECK(clamp(4, 10, -10) == 10);    // below it: the early return, and `hi` is never read
  // And the case that separates `<` from `<=`: sitting exactly on a `lo` that
  // is above `hi`. The comparison is strict, so this falls through to the high
  // bound; an inclusive one would return `lo` and every other case would agree
  // with it.
  CHECK(clamp(5, 5, 2) == 2);

  // The shipped shape: `lo = 0` with a possibly-negative value, which is the
  // one case where the early return fires on real data.
  CHECK(clamp(-7, 0, 40) == 0);
}

TEST(is_military_and_is_sentry_are_one_shape_over_two_class_names) {
  SpawnFixture f;
  const ObjectId soldier = TemplateMap::spawned(0);

  // `RHastatus` descends from `Object` and from nothing called `Military` or
  // `Sentry` in this graph, so both are false and neither is a refusal.
  const script::HostOutcome military = f.call_member("IsMilitary", {f.obj(soldier)});
  CHECK(military.status == script::HostStatus::ok);
  CHECK(military.value.as_integer() == 0);
  CHECK(f.call_member("IsSentry", {f.obj(soldier)}).value.as_integer() == 0);

  // An unresolvable receiver prints and answers false. Not a refusal: the
  // shipped callers put this inside `if` chains that must keep running.
  const script::HostOutcome missing = f.call_member("IsMilitary", {f.obj(kNoObject)});
  CHECK(missing.status == script::HostStatus::ok);
  CHECK(missing.value.as_integer() == 0);
  CHECK(f.call_member("IsSentry", {f.obj(kNoObject)}).value.as_integer() == 0);

  // And they are two different questions, which a single shared body would
  // hide: a class named `Military` answers one and not the other.
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
      R"(<class id="Military" parent="Object" cpp_class="CVXDecor"/>)",
      R"(<class id="RVelit" parent="Military" cpp_class="CVXDecor"/>)",
      R"(<class id="Sentry" parent="Object" cpp_class="CVXDecor"/>)",
      R"(<class id="BWatchman" parent="Sentry" cpp_class="CVXDecor"/>)",
  };
  const char* names[] = {"o.sc.xml", "m.sc.xml", "v.sc.xml", "s.sc.xml", "w.sc.xml"};
  for (int i = 0; i < 5; ++i) graph.add(bytes_of(docs[i]), names[i]);
  graph.link();
  World& world = f.map.world;
  world.set_class_graph(&graph);
  const ObjectId velit =
      world.spawn(imperivm::core::NativeClass::decor, nullptr, graph.lookup("RVelit"));
  const ObjectId watch =
      world.spawn(imperivm::core::NativeClass::decor, nullptr, graph.lookup("BWatchman"));

  CHECK(f.call_member("IsMilitary", {f.obj(velit)}).value.as_integer() != 0);
  CHECK(f.call_member("IsSentry", {f.obj(velit)}).value.as_integer() == 0);
  CHECK(f.call_member("IsSentry", {f.obj(watch)}).value.as_integer() != 0);
  CHECK(f.call_member("IsMilitary", {f.obj(watch)}).value.as_integer() == 0);
}

/// A class carrying a `<points>` table, which is the whole of what `GetPoint`
/// reads. Four points, three of one type, so "the *n*th of that type" is a
/// different question from "the *n*th point".
///
/// The `y` values are chosen to pin the scale and its rounding: 1024 must come
/// back as exactly 1448, -1024 as -1448 (truncation toward zero, not floor),
/// and 74 as 104 rather than 105.
constexpr std::string_view kPointsEntity =
    "<entity name=\"gate\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"3\" x=\"10\" y=\"1024\"/>"
    "<point idx=\"1\" type=\"2\" x=\"-70\" y=\"74\"/>"
    "<point idx=\"2\" type=\"3\" x=\"-4\" y=\"-1024\"/>"
    "<point idx=\"3\" type=\"3\" x=\"7\" y=\"0\"/>"
    "</points>"
    "</entity>";

TEST(get_point_answers_the_nth_point_of_a_type_as_a_relative_offset) {
  SpawnFixture f;
  World& world = f.map.world;
  const auto entity = imperivm::core::Entity::parse(bytes_of(kPointsEntity));
  REQUIRE(entity.ok());
  const ObjectId gate = world.spawn(imperivm::core::NativeClass::building, &entity.value());
  const ObjectId bare = TemplateMap::spawned(0);  // no entity at all

  const auto at = [&](std::int64_t type, std::int64_t n) {
    return unpack_point(
        f.call_member("GetPoint", {f.obj(gate), script::Value::integer(type),
                                   script::Value::integer(n)})
            .value);
  };
  constexpr std::int32_t kMiss = -32768;

  // `x` verbatim, `y` scaled by 1448/1024 -- the reciprocal of the projection's
  // 181/256, at finer fixed point.
  CHECK(at(3, 0).x == 10);
  CHECK(at(3, 0).y == 1448);
  // Truncation toward zero, which is what makes the negative one worth having.
  CHECK(at(3, 1).x == -4);
  CHECK(at(3, 1).y == -1448);
  CHECK(at(3, 2).x == 7);
  CHECK(at(3, 2).y == 0);
  // The index counts within the type, not across the table: type 2's only
  // point is the table's *second*, and it is this type's *zeroth*.
  CHECK(at(2, 0).x == -70);
  CHECK(at(2, 0).y == 104);  // 74 * 1448 / 1024 = 104.65, truncated
  CHECK(at(2, 1).x == kMiss);

  // The sentinel, which `RAM_ATTACK.VS` walks `n` upward until it sees. It has
  // to be exact and it has to be the same for all three ways of missing.
  CHECK(at(3, 3).x == kMiss);
  CHECK(at(3, 3).y == kMiss);
  CHECK(at(9, 0).x == kMiss);   // a type the class declares none of
  CHECK(at(3, -1).x == kMiss);  // and an index that cannot count up to
  const Point no_entity = unpack_point(
      f.call_member("GetPoint", {f.obj(bare), script::Value::integer(3),
                                 script::Value::integer(0)})
          .value);
  CHECK(no_entity.x == kMiss);
  CHECK(no_entity.y == kMiss);
  const Point no_object = unpack_point(
      f.call_member("GetPoint", {f.obj(kNoObject), script::Value::integer(3),
                                 script::Value::integer(0)})
          .value);
  CHECK(no_object.x == kMiss);
  CHECK(no_object.y == kMiss);
}

/// `Unit::GetFlags(mask)` -- a predicate, not an accessor.
TEST(get_flags_tests_a_mask_against_the_second_flag_word) {
  SpawnFixture f;
  World& world = f.map.world;
  const ObjectId soldier = TemplateMap::spawned(0);

  constexpr std::int64_t kNoAI = 0x00040000;
  constexpr std::int64_t kInAir = 0x00400000;

  // `kTemplateDocument` authors no `UnitFlags`, so every bit starts clear.
  CHECK(f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(kNoAI)})
            .value.as_integer() == 0);

  ObjectState* state = world.mutable_state(soldier);
  REQUIRE(state != nullptr);
  state->flags.no_ai = true;
  // **Exactly one.** `0x005d7e30` ends in `setne cl` and the registration's
  // return type is 7, so the answer is a bool and not the masked word -- which
  // for this mask would be 262144, and would pass every `!= 0` written here.
  CHECK(f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(kNoAI)})
            .value.as_integer() == 1);
  // The mask selects: a bit that is set does not answer for one that is not.
  CHECK(f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(kInAir)})
            .value.as_integer() == 0);
  state->flags.in_air = true;
  CHECK(f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(kInAir)})
            .value.as_integer() != 0);
  // Several bits at once is an *any*, which is what `setne` on a `test` gives.
  CHECK(f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(kNoAI + kInAir)})
            .value.as_integer() != 0);
  state->flags.no_ai = false;
  state->flags.in_air = false;
  CHECK(f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(kNoAI + kInAir)})
            .value.as_integer() == 0);
  // An empty mask is false however the word reads.
  state->flags.no_ai = true;
  CHECK(f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(0)})
            .value.as_integer() == 0);

  // The word carries every bit this engine models, not only the two the map
  // authors: `Hero::SetAutocast` is bit 2 and `Unit::IsCursed` bit 27.
  // Each is asked for by its own mask *and* denied by its neighbour's, because
  // two bits that are always set together are two bits nothing can tell apart.
  state->flags.no_ai = false;
  const auto only = [&](std::int64_t mask) {
    return f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(mask)})
        .value.as_integer();
  };
  state->flags.autocast = true;
  CHECK(only(0x00000004) == 1);
  CHECK(only(0x00040000) == 0);
  state->flags.entering = true;
  CHECK(only(0x00100000) == 1);
  CHECK(only(0x00400000) == 0);
  state->flags.noselect = true;
  CHECK(only(0x00800000) == 1);
  CHECK(only(0x04000000) == 0);  // not the minimap bit
  state->flags.noselect = false;
  state->flags.on_minimap = true;
  CHECK(only(0x04000000) == 1);
  CHECK(only(0x00800000) == 0);  // and the minimap bit is not that one
  state->flags.cursed = true;
  CHECK(only(0x08000000) == 1);

  // An unresolvable receiver answers false rather than refusing. It is not a
  // liveness test: `SQUADMONITOR.VS` asks this of a squad leader that may have
  // died since the list was built, and 0x005d7e30 has no `IsDead` probe.
  const script::HostOutcome missing =
      f.call_member("GetFlags", {f.obj(kNoObject), script::Value::integer(kNoAI)});
  CHECK(missing.status == script::HostStatus::ok);
  CHECK(missing.value.as_integer() == 0);
  state->flags.no_ai = true;
  state->health = 0;
  CHECK(f.call_member("GetFlags", {f.obj(soldier), script::Value::integer(kNoAI)})
            .value.as_integer() == 1);
}

TEST(set_no_ai_flag_writes_the_unit_and_the_list_forms_filter) {
  SpawnFixture f;
  World& world = f.map.world;

  const ObjectId soldier = TemplateMap::spawned(0);
  const ObjectId other = TemplateMap::spawned(3);
  REQUIRE(world.state(soldier) != nullptr);
  REQUIRE(!world.state(soldier)->flags.no_ai);

  // The member form: one receiver, and no filter of any kind. 0x005de600
  // validates the handle and writes the bit.
  REQUIRE(f.call_member("SetNoAIFlag", {f.obj(soldier), script::Value::boolean(true)}).status ==
          script::HostStatus::ok);
  CHECK(world.state(soldier)->flags.no_ai);
  CHECK(!world.state(other)->flags.no_ai);  // and on nobody else

  // It clears as well as sets -- the original ands the bit out first and only
  // ors it back when the argument is true, so a body that only ever set it
  // would pass every assertion above.
  REQUIRE(f.call_member("SetNoAIFlag", {f.obj(soldier), script::Value::boolean(false)}).status ==
          script::HostStatus::ok);
  CHECK(!world.state(soldier)->flags.no_ai);

  // An invalid receiver prints and returns. Not a refusal, and not a crash.
  CHECK(f.call_member("SetNoAIFlag", {f.obj(kNoObject), script::Value::boolean(true)}).status ==
        script::HostStatus::ok);

  // The list form's three filters. A building and a corpse go in the list
  // beside two live units, and only the two units come out flagged.
  ObjectState* corpse = world.mutable_state(other);
  REQUIRE(corpse != nullptr);
  REQUIRE(corpse->flags.is_unit);
  corpse->health = 0;

  const ObjectId wall = world.spawn(imperivm::core::NativeClass::building, nullptr);
  REQUIRE(world.mutable_state(wall) != nullptr);
  world.mutable_state(wall)->health = 100;
  REQUIRE(!world.state(wall)->flags.is_unit);

  const ObjectId second = TemplateMap::spawned(2);
  REQUIRE(world.state(second) != nullptr);

  ObjListPool& pool = objlist_pool_of(world);
  const ObjListId list = pool.acquire_temporary(7);
  *pool.mutable_items(list) = {soldier, wall, other, second, kNoObject};

  REQUIRE(f.call("SetNoAIFlag", {make_objlist_value(list), script::Value::boolean(true)}).status ==
          script::HostStatus::ok);
  CHECK(world.state(soldier)->flags.no_ai);
  CHECK(world.state(second)->flags.no_ai);
  CHECK(!world.state(wall)->flags.no_ai);   // bit 22: not a mobile unit
  CHECK(!world.state(other)->flags.no_ai);  // `vtbl + 0x50`: dead

  // And the member form has none of those filters, which is the asymmetry
  // worth pinning: the same building and the same corpse take the flag when
  // they are named directly.
  REQUIRE(f.call_member("SetNoAIFlag", {f.obj(wall), script::Value::boolean(true)}).status ==
          script::HostStatus::ok);
  REQUIRE(f.call_member("SetNoAIFlag", {f.obj(other), script::Value::boolean(true)}).status ==
          script::HostStatus::ok);
  CHECK(world.state(wall)->flags.no_ai);
  CHECK(world.state(other)->flags.no_ai);
}

TEST(is_in_group_and_get_group_size_read_the_live_half) {
  SpawnFixture f;
  World& world = f.map.world;
  const ObjectId live = TemplateMap::spawned(0);
  const ObjectId templ = TemplateMap::spawned(1);

  // `Both` is authored over one live object and one template.
  CHECK(f.call_member("IsInGroup", {f.obj(live), script::Value::string("Both")}).value.as_integer() != 0);
  // The template is a member of the table and not of the live half, so it
  // answers false -- the same answer `Group("Both")` gives about it.
  CHECK(world.groups().contains(world.groups().find("Both"), templ));
  CHECK(f.call_member("IsInGroup", {f.obj(templ), script::Value::string("Both")})
            .value.as_integer() == 0);

  // An unknown group is false, not a refusal.
  CHECK(f.call_member("IsInGroup", {f.obj(live), script::Value::string("Nope")})
            .value.as_integer() == 0);

  // `GetGroupSize` counts the same half: one of `Both`'s two members.
  CHECK(f.call("GetGroupSize", {script::Value::string("Both")}).value.as_integer() == 1);
  CHECK(f.call("GetGroupSize", {script::Value::string("Garrison")}).value.as_integer() == 0);
  // And zero for a group that does not exist, with nothing minted for it --
  // 0x00573a80 writes 0 with no diagnostic where its neighbours print one.
  const std::size_t before = world.groups().size();
  CHECK(f.call("GetGroupSize", {script::Value::string("Nope")}).value.as_integer() == 0);
  CHECK(world.groups().size() == before);

  // Spawning moves both answers, because it moves the live half.
  REQUIRE(world.spawn_from_template(templ) != kNoObject);
  CHECK(f.call("GetGroupSize", {script::Value::string("Both")}).value.as_integer() == 2);
  CHECK(f.call("GetGroupSize", {script::Value::string("Garrison")}).value.as_integer() == 1);
}

TEST(set_algebra_composes_over_group_queries) {
  // `Substract(Group("T_S_Sett1"), UnitsInSettlement(...))` ships; a set-op
  // query over a group query has to merge cleanly, and a group query is the
  // one kind that does **not** hand the merge ascending ids -- its members
  // come out in the order they joined. `World::evaluate_query` sorts copies
  // for that reason, and this is what says so: `CampDefense` is authored
  // 3, 0, 1 and `Reserve` 5, 3, so both operands arrive out of order and
  // overlap on exactly object 3.
  //
  // Counts alone do not settle it. Dropping either sort leaves
  // `std::set_intersection` and friends reading unsorted input, which is
  // undefined and here comes out as a wrong *count* on one side and a segfault
  // on the other -- so every index below is guarded, per the note on REQUIRE
  // in `test.hpp`, and the element order is spelled out rather than assumed.
  LoadedMap loaded;
  const ObjectId camp =
      loaded.world.create_query(group_query(loaded.world.group_index("CampDefense")));
  const ObjectId reserve =
      loaded.world.create_query(group_query(loaded.world.group_index("Reserve")));

  std::vector<ObjectId> found;
  const ObjectId both = loaded.world.create_query(set_op(SetOp::intersect, camp, reserve));
  REQUIRE(loaded.world.evaluate_query(both, found) == 1);
  CHECK(found[0] == loaded.spawned(3));

  const ObjectId either = loaded.world.create_query(set_op(SetOp::set_union, camp, reserve));
  REQUIRE(loaded.world.evaluate_query(either, found) == 4);
  CHECK(found[0] == loaded.spawned(0));
  CHECK(found[1] == loaded.spawned(1));
  CHECK(found[2] == loaded.spawned(3));
  CHECK(found[3] == loaded.spawned(5));

  const ObjectId camp_only = loaded.world.create_query(set_op(SetOp::subtract, camp, reserve));
  REQUIRE(loaded.world.evaluate_query(camp_only, found) == 2);
  CHECK(found[0] == loaded.spawned(0));
  CHECK(found[1] == loaded.spawned(1));
}

// --------------------------------------------------------------------------
// Place and _PlaceEx: an object from a class name
// --------------------------------------------------------------------------

/// Four classes spanning the branch `Place` takes on its `CVXScriptObj` cast.
///
/// `RHastatus` is a unit and `WatchEye` a script object, which are the two
/// shapes 46 of the corpus's 173 literal call sites name; `ChimneySmoke` is a
/// bare `CVXDecor`, which is what the other 19 name and what the cast rejects.
/// Every one of them declares its own `cpp_class`, because every shipped class
/// does: `ClassGraph::add` refuses an element without one, and all **845** of
/// `DATA\CLASSES\*.SC.XML` carry the attribute. So does the Python reader, and
/// it parses the same 845. Worth writing down because the plan's corrections
/// table says "only 821 of 845 declare `cpp_class`; 24 inherit it" -- which is
/// not true of the class files, and a fixture built on it was the thing that
/// found out.
struct PlaceGraph {
  ClassGraph graph;

  PlaceGraph() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="RHastatus" parent="Object" cpp_class="CVXUnit"><properties sight="100" maxhealth="50"/></class>)",
        R"(<class id="WatchEye" parent="Object" cpp_class="CVXScriptObj"><properties sight="900" maxhealth="1"/></class>)",
        R"(<class id="ChimneySmoke" parent="Object" cpp_class="CVXDecor"><properties sight="0" maxhealth="7"/></class>)",
        R"(<class id="Trireme" parent="Object" cpp_class="CVXShip"><properties sight="200" maxhealth="300"/></class>)",
        // Two of the eight race catapults `PlaceCatapult` chooses between; the
        // other six are absent so an unknown class is a case rather than a gap.
        R"(<class id="RCatapult" parent="Object" cpp_class="CVXCatapult"><properties sight="150" maxhealth="400"/></class>)",
        R"(<class id="GCatapult" parent="Object" cpp_class="CVXCatapult"><properties sight="150" maxhealth="400"/></class>)",
        // The protective effect `IsProtected` looks for, with the radius it
        // doubles.
        R"(<class id="CoverOfMercy" parent="Object" cpp_class="CVXScriptObj"><properties sight="0" maxhealth="10" radius="400"/></class>)",
        // A heir, because the sweep tests the class tree and not the leaf.
        R"(<class id="GreaterCover" parent="CoverOfMercy" cpp_class="CVXScriptObj"/>)",
        // The two building families `CalcEscapeDirection` runs from, one of
        // them through an heir, and a building that is neither.
        R"(<class id="Tower" parent="Object" cpp_class="CVXBuilding"><properties sight="300" maxhealth="500"/></class>)",
        R"(<class id="Outpost" parent="Object" cpp_class="CVXBuilding"><properties sight="300" maxhealth="500"/></class>)",
        R"(<class id="GaulOutpost" parent="Outpost" cpp_class="CVXBuilding"/>)",
        R"(<class id="Barn" parent="Object" cpp_class="CVXBuilding"><properties sight="300" maxhealth="500"/></class>)",
        // The other two things a ram is for.
        R"(<class id="Gate" parent="Object" cpp_class="CVXGate"><properties sight="300" maxhealth="500"/></class>)",
        R"(<class id="BaseShipyard" parent="Object" cpp_class="CVXBuilding"><properties sight="300" maxhealth="500"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "hastatus.sc.xml", "watcheye.sc.xml",
                           "smoke.sc.xml", "trireme.sc.xml", "rcatapult.sc.xml",
                           "gcatapult.sc.xml", "coverofmercy.sc.xml", "greatercover.sc.xml",
                           "tower.sc.xml", "outpost.sc.xml", "gauloutpost.sc.xml", "barn.sc.xml",
                           "gate.sc.xml", "baseshipyard.sc.xml"};
    for (int i = 0; i < 15; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
  }
};

/// A world big enough to have edges, with the match that supplies them.
///
/// `map_size` is 8192, the smallest of the three the shipped maps use, so the
/// rectangle is `[0, 8191]` and `_PlaceEx`'s inset band is `[48, 8143]`.
struct PlaceFixture {
  PlaceGraph classes;
  World world;
  MatchSystem match;
  FeederSystem feeder;
  script::HostRegistry registry;
  HostContext context;

  PlaceFixture() {
    world.set_class_graph(&classes.graph);
    world.add_system(&match);
    world.add_system(&feeder);
    MatchRules rules;
    rules.map_size = 8192;
    (void)setup_match(world, match, rules, MatchOptions{});
    script::declare_shipped_surface(registry);
    (void)register_world_host(registry);
    (void)register_objlist_host(registry);
    context.world = &world;
  }

  script::HostOutcome call(std::string_view name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(script::CallKind::free_function, name,
                                              static_cast<std::uint16_t>(args.size()));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::free_function;
    ctx.script = 7;
    return entry.fn(ctx);
  }

  static script::Value point(std::int32_t x, std::int32_t y) {
    return pack_point(Point{x, y});
  }

  [[nodiscard]] static ObjectId id_of(const script::Value& value) {
    return value.is_object() && value.as_object().type == kTypeObj ? value.as_object().id
                                                                   : kNoObject;
  }
};

/// A placed unit is in the feeding chain **now**, not at the head of the next
/// turn.
///
/// `FeederSystem` reconciles against the world once a turn, and the shipped
/// script that made this matter never reaches a turn boundary in between:
/// `OUTPOST_BEHAVIOR.VS` is `u1 = Place(sDefenderCls1, .pos, .player);` and
/// then `u1.SetFood(20); u1.SetFeeding(false);` two lines later. Before this,
/// both setters were handed an id the chain had never heard of and both refused
/// by name -- and *nothing advanced a turn* in between, so no amount of
/// reconciling would have helped. The original has no chain to be late for: a
/// `CVXUnit` is a feeding unit from construction.
TEST(a_placed_unit_joins_the_feeding_chain_in_the_same_turn) {
  PlaceFixture f;
  REQUIRE(f.feeder.total_unit_count() == 0);

  const script::HostOutcome out =
      f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(700, 900),
                       script::Value::integer(3)});
  REQUIRE(out.status == script::HostStatus::ok);
  const ObjectId made = PlaceFixture::id_of(out.value);
  REQUIRE(made != kNoObject);

  // No turn has been advanced and no `sync_with_world` has run.
  CHECK(f.feeder.total_unit_count() == 1);
  REQUIRE(f.feeder.find(made) != nullptr);
  // And it arrives fed, which is the enrolment rule and not a placement one.
  CHECK(f.feeder.find(made)->food == f.feeder.find(made)->max_food);
  // So the setter the script reaches next resolves.
  CHECK(f.feeder.set_feeding(made, false));

  // A decor object is not a feeding unit and must not be enrolled by placing
  // one -- `flags_for_native_class` is what the enrolment test reads, and it is
  // the same test `sync_with_world` applies.
  const script::HostOutcome fort =
      f.call("Place", {script::Value::string("ChimneySmoke"), PlaceFixture::point(700, 1200),
                       script::Value::integer(3)});
  if (fort.status == script::HostStatus::ok && PlaceFixture::id_of(fort.value) != kNoObject) {
    CHECK(f.feeder.find(PlaceFixture::id_of(fort.value)) == nullptr);
  }
  CHECK(f.feeder.total_unit_count() == 1);
}

TEST(place_mints_an_object_of_the_class_it_names) {
  PlaceFixture f;
  const std::size_t before = f.world.size();

  const script::HostOutcome out =
      f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(700, 900),
                       script::Value::integer(3)});
  REQUIRE(out.status == script::HostStatus::ok);
  const ObjectId made = PlaceFixture::id_of(out.value);
  REQUIRE(made != kNoObject);
  CHECK(f.world.size() == before + 1);

  const WorldObject* slot = f.world.find(made);
  REQUIRE(slot != nullptr);
  CHECK(slot->state.position.x == 700);
  CHECK(slot->state.position.y == 900);
  // 1-based in, 0-based stored. `SetPlayer(player - 1)` through `vtbl + 0xa0`,
  // which is the same convention `<scriptobj player>` already carries.
  CHECK(slot->state.owner == 2);
  // Undamaged, from the class rather than from an authored `healthperc`.
  CHECK(slot->state.health == 50);
  CHECK(f.world.class_is_a(made, f.classes.graph.lookup("RHastatus")));
}

TEST(place_hands_back_no_handle_for_a_class_the_graph_never_took) {
  // `ClassGraph::add` refuses a `<class>` with no `cpp_class`, so such a class
  // is not in the graph to be looked up and `Place` cannot tell it from a
  // misspelling. Both are the same printed diagnostic in the original --
  // `Could not find class named '%s' in function 'Place'. Check the spelling.`
  // -- and the same sentinel, so nothing is lost by not distinguishing them.
  PlaceGraph classes;
  CHECK(classes.graph.lookup("Nameless") == imperivm::core::kNoClass);

  PlaceFixture f;
  const std::size_t before = f.world.size();
  const script::HostOutcome out =
      f.call("Place", {script::Value::string("Nameless"), PlaceFixture::point(64, 64),
                       script::Value::integer(1)});
  REQUIRE(out.status == script::HostStatus::ok);
  CHECK(PlaceFixture::id_of(out.value) == kNoObject);
  CHECK(f.world.size() == before);
}

TEST(place_refuses_by_sentinel_rather_than_by_trapping) {
  // Every one of the three misses prints and runs on in `gbr.exe`. Trapping
  // would kill scripts the original finishes -- the same rule `GetSettlement`
  // is implemented under.
  PlaceFixture f;
  const std::size_t before = f.world.size();

  const script::Value ok_point = PlaceFixture::point(100, 100);
  struct Miss {
    const char* what;
    std::vector<script::Value> args;
  };
  const std::vector<Miss> misses = {
      {"player 0", {script::Value::string("RHastatus"), ok_point, script::Value::integer(0)}},
      {"player 17", {script::Value::string("RHastatus"), ok_point, script::Value::integer(17)}},
      {"no such class",
       {script::Value::string("NoSuchClass"), ok_point, script::Value::integer(1)}},
      {"off the map",
       {script::Value::string("RHastatus"), PlaceFixture::point(9000, 100),
        script::Value::integer(1)}},
  };
  for (const Miss& miss : misses) {
    const script::HostOutcome out = f.call("Place", miss.args);
    CHECK(out.status == script::HostStatus::ok);
    CHECK(PlaceFixture::id_of(out.value) == kNoObject);
  }
  // And not one of them left an object behind. The player check and the bounds
  // check both run *before* the class is looked up, so a body that created
  // first and validated afterwards would litter the world with four objects
  // while every assertion above still passed.
  CHECK(f.world.size() == before);
}

TEST(place_accepts_the_held_position_that_settles_the_rectangle) {
  // `(-1, -1)` skips the bounds test (0x005a87ab). The escape hatch is the
  // proof the low edges are zero: were they negative it would be dead code.
  PlaceFixture f;
  const script::HostOutcome out =
      f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(-1, -1),
                       script::Value::integer(1)});
  REQUIRE(out.status == script::HostStatus::ok);
  CHECK(PlaceFixture::id_of(out.value) != kNoObject);

  // One axis is not the other: `(-1, 5)` is outside and refused.
  const script::HostOutcome half =
      f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(-1, 5),
                       script::Value::integer(1)});
  REQUIRE(half.status == script::HostStatus::ok);
  CHECK(PlaceFixture::id_of(half.value) == kNoObject);
}

TEST(place_includes_both_edges_of_the_map_rectangle) {
  // Inclusive on all four, agreed by four independent readings -- see the note
  // above `fn_get_map_rect`. `map_size - 1` is inside and `map_size` is not.
  PlaceFixture f;
  const script::HostOutcome edge =
      f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(8191, 8191),
                       script::Value::integer(1)});
  REQUIRE(edge.status == script::HostStatus::ok);
  CHECK(PlaceFixture::id_of(edge.value) != kNoObject);

  const script::HostOutcome past =
      f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(8192, 0),
                       script::Value::integer(1)});
  REQUIRE(past.status == script::HostStatus::ok);
  CHECK(PlaceFixture::id_of(past.value) == kNoObject);
}

TEST(place_hands_back_no_handle_for_a_class_that_is_not_a_script_object) {
  // The branch, and its two halves are not the same thing. A bare `CVXDecor`
  // *is* placed -- `_PlaceEx` logs `MapObj created (%s)` as it goes -- but it
  // gets no owner and the script gets the invalid handle. 0 of the 19 shipped
  // sites naming such a class keeps the result, against 46 of 46 that do.
  PlaceFixture f;
  const std::size_t before = f.world.size();

  const script::HostOutcome out =
      f.call("Place", {script::Value::string("ChimneySmoke"), PlaceFixture::point(300, 400),
                       script::Value::integer(3)});
  REQUIRE(out.status == script::HostStatus::ok);
  CHECK(PlaceFixture::id_of(out.value) == kNoObject);

  // It exists all the same, positioned and unowned.
  REQUIRE(f.world.size() == before + 1);
  const WorldObject& smoke = f.world.objects().back();
  CHECK(smoke.state.position.x == 300);
  CHECK(smoke.state.position.y == 400);
  CHECK(smoke.state.owner == kNoPlayer);
  CHECK(smoke.state.health == 7);
}

TEST(place_a_script_object_class_keeps_its_owner_and_its_handle) {
  // The other side of the same branch, and the one that makes the test above a
  // measurement rather than an assertion that `Place` sometimes fails:
  // `WatchEye` is 92 of the corpus's 173 literal sites.
  PlaceFixture f;
  const script::HostOutcome out =
      f.call("Place", {script::Value::string("WatchEye"), PlaceFixture::point(300, 400),
                       script::Value::integer(3)});
  REQUIRE(out.status == script::HostStatus::ok);
  const ObjectId made = PlaceFixture::id_of(out.value);
  REQUIRE(made != kNoObject);
  const WorldObject* eye = f.world.find(made);
  REQUIRE(eye != nullptr);
  CHECK(eye->state.owner == 2);
}

TEST(place_ex_clamps_where_place_refuses) {
  // The difference that is not a detail. `Place` rejects a point outside the
  // rectangle; `_PlaceEx` moves it inside one inset by 48 on every side.
  PlaceFixture f;
  const script::HostOutcome out =
      f.call("_PlaceEx", {script::Value::string("RHastatus"), script::Value::integer(-500),
                          script::Value::integer(99999), script::Value::integer(2)});
  REQUIRE(out.status == script::HostStatus::ok);
  const ObjectId made = PlaceFixture::id_of(out.value);
  REQUIRE(made != kNoObject);
  const WorldObject* slot = f.world.find(made);
  REQUIRE(slot != nullptr);
  CHECK(slot->state.position.x == 48);         // 0 + 48
  CHECK(slot->state.position.y == 8191 - 48);  // map_size - 1 - 48
  CHECK(slot->state.owner == 1);
}

TEST(place_ex_leaves_an_interior_point_where_it_is) {
  // The inset is a clamp, not a shrink: a point already inside the band is not
  // moved, and a point in the 48-unit margin is.
  PlaceFixture f;
  const script::HostOutcome inside =
      f.call("_PlaceEx", {script::Value::string("RHastatus"), script::Value::integer(4000),
                          script::Value::integer(4000), script::Value::integer(1)});
  REQUIRE(inside.status == script::HostStatus::ok);
  const WorldObject* interior = f.world.find(PlaceFixture::id_of(inside.value));
  REQUIRE(interior != nullptr);
  CHECK(interior->state.position.x == 4000);

  const script::HostOutcome margin =
      f.call("_PlaceEx", {script::Value::string("RHastatus"), script::Value::integer(47),
                          script::Value::integer(4000), script::Value::integer(1)});
  REQUIRE(margin.status == script::HostStatus::ok);
  const WorldObject* moved = f.world.find(PlaceFixture::id_of(margin.value));
  REQUIRE(moved != nullptr);
  CHECK(moved->state.position.x == 48);
}

TEST(place_ex_does_not_range_check_the_player_and_place_does) {
  // `_PlaceEx` has no `cmp 1` / `cmp 16` in it at all; the decrement at
  // 0x005a8b08 is applied to whatever arrived. Transcribed rather than
  // tidied, and it is the sharpest observable difference between the two
  // bodies: the same arguments give an object from one and a sentinel from the
  // other.
  PlaceFixture f;
  const script::HostOutcome refused =
      f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(100, 100),
                       script::Value::integer(17)});
  REQUIRE(refused.status == script::HostStatus::ok);
  CHECK(PlaceFixture::id_of(refused.value) == kNoObject);

  const script::HostOutcome allowed =
      f.call("_PlaceEx", {script::Value::string("RHastatus"), script::Value::integer(100),
                          script::Value::integer(100), script::Value::integer(17)});
  REQUIRE(allowed.status == script::HostStatus::ok);
  CHECK(PlaceFixture::id_of(allowed.value) != kNoObject);

  // And the low side, which is where the missing check has a *consequence*
  // rather than just an absence: player 0 decrements to -1, so the object comes
  // back owned by nobody. Injecting a range check into `_PlaceEx` survived a
  // version of this test that only tried 17, because 17 is refused by the
  // decrement's own arithmetic nowhere -- it is the untested end that the
  // injected fault chose.
  const script::HostOutcome unowned =
      f.call("_PlaceEx", {script::Value::string("RHastatus"), script::Value::integer(100),
                          script::Value::integer(100), script::Value::integer(0)});
  REQUIRE(unowned.status == script::HostStatus::ok);
  const ObjectId nobodys = PlaceFixture::id_of(unowned.value);
  REQUIRE(nobodys != kNoObject);
  const WorldObject* slot = f.world.find(nobodys);
  REQUIRE(slot != nullptr);
  CHECK(slot->state.owner == kNoPlayer);
}

TEST(placing_a_ship_gives_it_the_holder_that_ships_carry) {
  // A ship is a composite: the vessel, then a `CVXHolder` for its boarded units
  // at `handle + 1`. 3 of 3 in the corpus, and the only place a standalone
  // holder occurs -- so a `Place` that used the plain spawn would leave a ship
  // with nowhere to put anybody, and allocation order is observable state.
  //
  // No shipped call site names a ship class: all 37 of the literal ones are
  // units, script objects, decor or an area effect. This case exists because
  // fault injection found that nothing covered the branch -- 63 of the 236
  // sites pass a class name the corpus computes rather than spells, and one of
  // those can be anything the class graph holds.
  PlaceFixture f;
  const ObjectId next = f.world.next_id();
  const script::HostOutcome out =
      f.call("Place", {script::Value::string("Trireme"), PlaceFixture::point(500, 500),
                       script::Value::integer(2)});
  REQUIRE(out.status == script::HostStatus::ok);
  const ObjectId ship = PlaceFixture::id_of(out.value);
  REQUIRE(ship == next);

  const WorldObject* hull = f.world.find(ship);
  REQUIRE(hull != nullptr);
  CHECK(hull->state.owner == 1);
  CHECK(hull->state.health == 300);

  // Contiguous, which is what the composite rule says and what 328 of 328
  // shipped composites do.
  const WorldObject* holder = f.world.find(ship + 1);
  REQUIRE(holder != nullptr);
  CHECK(holder->internal == InternalKind::holder);
}

TEST(a_placed_object_is_not_a_spawn_template) {
  // Which is what lets `GameSession`'s per-turn sweep give it its class's
  // `idle` method, and what keeps it out of the template half of every group.
  // The bit is 39% of the objects a map places and 0% of the objects a script
  // places -- there is no attribute for `Place` to read it from.
  PlaceFixture f;
  const script::HostOutcome out =
      f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(100, 100),
                       script::Value::integer(1)});
  REQUIRE(out.status == script::HostStatus::ok);
  const ObjectId made = PlaceFixture::id_of(out.value);
  REQUIRE(made != kNoObject);
  const WorldObject* placed = f.world.find(made);
  REQUIRE(placed != nullptr);
  CHECK(!placed->state.flags.unspawned);
}

TEST(placing_moves_the_world_hash_and_a_refusal_does_not) {
  // Objects are hashed state, so a placement is a divergence if two peers
  // disagree about whether it happened -- and a *refusal* must therefore leave
  // the hash exactly where it was. The bare-`CVXDecor` branch is the
  // interesting one: it hands back no handle and still moves the hash, because
  // it still made an object.
  PlaceFixture f;
  const std::uint64_t start = f.world.state_hash();

  (void)f.call("Place", {script::Value::string("NoSuchClass"), PlaceFixture::point(100, 100),
                         script::Value::integer(1)});
  CHECK(f.world.state_hash() == start);

  (void)f.call("Place", {script::Value::string("ChimneySmoke"), PlaceFixture::point(100, 100),
                         script::Value::integer(1)});
  const std::uint64_t after_smoke = f.world.state_hash();
  CHECK(after_smoke != start);

  (void)f.call("Place", {script::Value::string("RHastatus"), PlaceFixture::point(100, 100),
                         script::Value::integer(1)});
  CHECK(f.world.state_hash() != after_smoke);
}

/// `PlaceCatapult(x, y, player, race)`: the race picks the class, the player
/// owns it, the site is where the click was, and the engine is born unbuilt
/// at one point of health for its builders to raise.
TEST(place_catapult_places_the_races_engine_unbuilt_at_one_health) {
  PlaceFixture f;
  const auto place = [&](std::int32_t x, std::int32_t y, std::int32_t player, std::int32_t race) {
    const script::HostOutcome out =
        f.call("PlaceCatapult", {script::Value::integer(x), script::Value::integer(y),
                                 script::Value::integer(player), script::Value::integer(race)});
    CHECK(out.status == script::HostStatus::ok);
    return PlaceFixture::id_of(out.value);
  };
  // Republican Rome (1) and Imperial Rome (4) share `RCatapult`; Gaul (0) is
  // `GCatapult`.
  const ObjectId roman = place(700, 900, 3, 1);
  REQUIRE(roman != kNoObject);
  const WorldObject* slot = f.world.find(roman);
  REQUIRE(slot != nullptr);
  CHECK(slot->class_index == f.classes.graph.find("RCatapult"));
  CHECK(slot->state.owner == 2);  // the script's 1-based 3
  CHECK(slot->state.position == (Point{700, 900}));
  CHECK(slot->state.health == 1);
  CHECK(!slot->state.flags.built);
  const ObjectId imperial = place(100, 100, 1, 4);
  REQUIRE(imperial != kNoObject);
  CHECK(f.world.find(imperial)->class_index == f.classes.graph.find("RCatapult"));
  const ObjectId gaul = place(100, 100, 1, 0);
  REQUIRE(gaul != kNoObject);
  CHECK(f.world.find(gaul)->class_index == f.classes.graph.find("GCatapult"));

  // No bounds test on the point, unlike `Place`: a site past the map edge is
  // still a site.
  CHECK(place(9000, 9000, 1, 1) != kNoObject);
  // A race off the table is the original's null class -- a fault there, the
  // invalid handle here -- and so is a race whose class this graph lacks.
  CHECK(place(100, 100, 1, 8) == kNoObject);
  CHECK(place(100, 100, 1, -1) == kNoObject);
  CHECK(place(100, 100, 1, 2) == kNoObject);
}

/// The build sight is `CONST.INI`'s `GamePlay/CatapultBuildSight`, and it is
/// what the site reveals at placement -- to the placing player only.
TEST(place_catapult_reveals_the_site_to_the_build_sight) {
  PlaceFixture f;
  EnvSystem env;
  env.set_constant("CatapultBuildSight", 3000);
  FogSystem fog;
  fog.map().resize(8192);
  f.world.add_system(&env);
  f.world.add_system(&fog);
  const std::int32_t mine = fog_slot_from_script(3);
  const std::int32_t theirs = fog_slot_from_script(4);
  CHECK(!fog.map().explored(Point{4000, 4000}, mine));

  const script::HostOutcome out =
      f.call("PlaceCatapult", {script::Value::integer(4000), script::Value::integer(4000),
                               script::Value::integer(3), script::Value::integer(1)});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(PlaceFixture::id_of(out.value) != kNoObject);
  CHECK(fog.map().explored(Point{4000, 4000}, mine));
  CHECK(fog.map().explored(Point{4000, 6500}, mine));    // within 3000
  CHECK(!fog.map().explored(Point{4000, 7800}, mine));   // past it
  CHECK(!fog.map().explored(Point{4000, 4000}, theirs));

  // No value in the store is a sight of zero, and nothing is revealed.
  env.set_constant("CatapultBuildSight", 0);
  const script::HostOutcome again =
      f.call("PlaceCatapult", {script::Value::integer(500), script::Value::integer(500),
                               script::Value::integer(3), script::Value::integer(1)});
  REQUIRE(again.status == script::HostStatus::ok);
  CHECK(!fog.map().explored(Point{500, 500}, mine));
}

/// `IsProtected(player, pos, class)`: a live object of the class, owned by
/// somebody the player is not at war with, within twice the class's radius of
/// the point by the query circle rule. The early exits all answer true.
TEST(is_protected_finds_a_friendly_effect_within_twice_its_radius) {
  PlaceFixture f;
  const auto ask = [&](std::int32_t player, std::int32_t x, std::int32_t y, const char* cls) {
    const script::HostOutcome out =
        f.call("IsProtected", {script::Value::integer(player), PlaceFixture::point(x, y),
                               script::Value::string(cls)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  };
  // Player 3 (script numbering) is at peace with player 4 and at war with
  // player 5: the row is the viewer's own.
  f.world.players().set(2, 3, Relation::ceasefire, true);
  CHECK(!ask(3, 1000, 1000, "CoverOfMercy"));

  // A cover placed by player 4 at (1000, 1000), radius 400: protected out to
  // 800 -- the query rule puts (1800, 1000) inside -- and not beyond.
  const script::HostOutcome placed =
      f.call("Place", {script::Value::string("CoverOfMercy"), PlaceFixture::point(1000, 1000),
                       script::Value::integer(4)});
  REQUIRE(placed.status == script::HostStatus::ok);
  const ObjectId cover = PlaceFixture::id_of(placed.value);
  REQUIRE(cover != kNoObject);
  CHECK(ask(3, 1000, 1000, "CoverOfMercy"));
  CHECK(ask(3, 1800, 1000, "CoverOfMercy"));
  CHECK(!ask(3, 1801, 1000, "CoverOfMercy"));
  // The query circle rule, not the sampler's: (800, 1) off the centre has
  // `d2 = 640001 > 800^2` and is outside by the sweep's test where the plain
  // rule's one-unit shell still admits it -- the probe `sim/area.hpp` records
  // as the only point that separates the two.
  CHECK(!ask(3, 1800, 1001, "CoverOfMercy"));
  CHECK(ask(4, 1500, 1300, "CoverOfMercy"));
  // Player 5 is at war with player 4: the cover is nothing to it.
  CHECK(!ask(5, 1000, 1000, "CoverOfMercy"));
  // A point past the map is clamped onto it first.
  CHECK(!ask(3, 9000, 1000, "CoverOfMercy"));
  f.world.set_position(cover, Point{8100, 1000});
  CHECK(ask(3, 9000, 1000, "CoverOfMercy"));
  // A dead effect protects nobody.
  f.world.set_health(cover, 0);
  CHECK(!ask(3, 8100, 1000, "CoverOfMercy"));

  // A heir of the class counts: the sweep tests the class descriptor's tree.
  const script::HostOutcome greater =
      f.call("Place", {script::Value::string("GreaterCover"), PlaceFixture::point(3000, 3000),
                       script::Value::integer(4)});
  REQUIRE(greater.status == script::HostStatus::ok);
  REQUIRE(PlaceFixture::id_of(greater.value) != kNoObject);
  CHECK(ask(3, 3000, 3000, "CoverOfMercy"));

  // The early exits answer true: a class nothing declares, a player off the
  // table. The original prints for the first and writes 1 for both.
  CHECK(ask(3, 1000, 1000, "NoSuchEffect"));
  CHECK(ask(0, 1000, 1000, "CoverOfMercy"));
  CHECK(ask(17, 1000, 1000, "CoverOfMercy"));
}

/// `CalcEscapeDirection`: a length-1000 vector pointing from the nearest
/// enemy tower or outpost in sight to the catapult, or a random direction
/// when there is none.
TEST(calc_escape_direction_points_away_from_the_nearest_enemy_tower) {
  PlaceFixture f;
  const auto building = [&](const char* cls, Point at, PlayerId owner) {
    const ObjectId id = f.world.spawn(imperivm::core::NativeClass::building, nullptr, f.classes.graph.find(cls));
    f.world.set_position(id, at);
    f.world.set_owner(id, owner);
    f.world.set_health(id, 500);
    return id;
  };
  const auto ask = [&](ObjectId who) {
    const std::uint32_t index = f.registry.find(script::CallKind::member, "CalcEscapeDirection", 0);
    CHECK(index != script::kUnresolvedHost);
    std::vector<script::Value> args = {script::Value::object(script::ObjectRef{kTypeObj, who})};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &f.context;
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = f.registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return unpack_point(out.value);
  };
  // Player 0's catapult; player 1 is at war with it, player 2 has a ceasefire
  // with player 0 on its own row, which is the row the predicate reads.
  f.world.players().set(2, 0, Relation::ceasefire, true);
  const ObjectId cat = f.world.spawn(imperivm::core::NativeClass::catapult, nullptr, f.classes.graph.find("RCatapult"));
  f.world.set_position(cat, Point{2000, 2000});
  f.world.set_owner(cat, 0);
  f.world.set_health(cat, 400);
  REQUIRE(f.world.find(cat)->sight == 150);
  f.world.find(cat)->sight = 1500;

  // An enemy tower due west: the answer points due east, length 1000.
  const ObjectId west = building("Tower", Point{1000, 2000}, 1);
  CHECK(ask(cat) == (Point{1000, 0}));
  // A nearer enemy outpost heir to the north-west wins: (500, 400) over a
  // floored root of 640 is (781, 625), truncating division on each axis.
  const ObjectId near = building("GaulOutpost", Point{1500, 1600}, 1);
  CHECK(ask(cat) == (Point{781, 625}));
  // A still nearer barn is not a threat, and neither is a tower whose owner
  // is not at war with the catapult's, nor one out of sight.
  (void)building("Barn", Point{2100, 2000}, 1);
  (void)building("Tower", Point{2200, 2000}, 2);
  (void)building("Tower", Point{4000, 2000}, 1);
  CHECK(ask(cat) == (Point{781, 625}));
  f.world.set_health(near, 0);
  f.world.despawn(near);
  CHECK(ask(cat) == (Point{1000, 0}));
  // A second tower at exactly the west tower's distance does not replace it:
  // an equal distance keeps the first the sweep met, which here is the lower
  // id -- the labelled tie rule.
  const ObjectId east = building("Tower", Point{3000, 2000}, 1);
  CHECK(ask(cat) == (Point{1000, 0}));
  f.world.despawn(east);

  // Nothing in sight: two draws on the world RNG, `rand(0, 1000) - 500` on
  // each axis, normalised to 1000 the same way.
  f.world.despawn(west);
  for (std::uint32_t seed = 1; seed <= 12; ++seed) {
    f.world.rng().seed(seed);
    Rng model = f.world.rng();
    const std::int32_t dx = model.between(0, 1000) - 500;
    const std::int32_t dy = model.between(0, 1000) - 500;
    const auto len = static_cast<std::int32_t>(isqrt(std::int64_t{dx} * dx + std::int64_t{dy} * dy));
    const Point expected = len > 0 ? Point{dx * 1000 / len, dy * 1000 / len} : Point{0, 0};
    CHECK(ask(cat) == expected);
    CHECK(f.world.rng().state() == model.state());
  }
  // A handle to nothing is `(0, 0)` and no draw.
  const std::uint32_t before = f.world.rng().state();
  CHECK(ask(999999) == (Point{0, 0}));
  CHECK(f.world.rng().state() == before);
}

/// `RamBestTarget`: the nearest unbroken enemy gate, outpost or shipyard
/// within the ram's sight plus 512 and under 1000 units, or nothing.
TEST(ram_best_target_is_the_nearest_unbroken_enemy_gate_outpost_or_shipyard) {
  PlaceFixture f;
  const auto building = [&](const char* cls, Point at, PlayerId owner) {
    const ObjectId id = f.world.spawn(imperivm::core::NativeClass::building, nullptr,
                                      f.classes.graph.find(cls));
    f.world.set_position(id, at);
    f.world.set_owner(id, owner);
    f.world.set_health(id, 500);
    return id;
  };
  const auto ask = [&](ObjectId who) {
    const std::uint32_t index = f.registry.find(script::CallKind::member, "RamBestTarget", 0);
    CHECK(index != script::kUnresolvedHost);
    std::vector<script::Value> args = {script::Value::object(script::ObjectRef{kTypeObj, who})};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &f.context;
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = f.registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return PlaceFixture::id_of(out.value);
  };
  f.world.players().set(2, 0, Relation::ceasefire, true);
  const ObjectId ram = f.world.spawn(imperivm::core::NativeClass::unit, nullptr,
                                     f.classes.graph.find("RHastatus"));
  f.world.set_position(ram, Point{2000, 2000});
  f.world.set_owner(ram, 0);
  f.world.set_health(ram, 50);
  f.world.find(ram)->sight = 400;  // reach 912

  CHECK(ask(ram) == kNoObject);
  // A gate at 900: in reach, and the target.
  const ObjectId gate = building("Gate", Point{2900, 2000}, 1);
  CHECK(ask(ram) == gate);
  // Nearer things that are not targets: a tower, a barn, a friendly outpost,
  // a broken gate. Then a nearer shipyard, which is.
  (void)building("Tower", Point{2300, 2000}, 1);
  (void)building("Barn", Point{2200, 2000}, 1);
  (void)building("GaulOutpost", Point{2250, 2000}, 2);
  const ObjectId ruin = building("Gate", Point{2100, 2000}, 1);
  f.world.find(ruin)->state.damage_state = 3;
  CHECK(ask(ram) == gate);
  const ObjectId yard = building("BaseShipyard", Point{2500, 2000}, 1);
  CHECK(ask(ram) == yard);
  const ObjectId post = building("GaulOutpost", Point{2000, 2400}, 1);
  CHECK(ask(ram) == post);
  // An equal distance keeps the first met -- the lower id.
  const ObjectId west = building("Gate", Point{1600, 2000}, 1);
  CHECK(ask(ram) == post);

  // The reach is the sight plus 512; a target at 913 is out of it even
  // though 1000 would admit it, and one at 999 is in once the sight allows.
  f.world.despawn(post);
  f.world.despawn(yard);
  f.world.despawn(west);
  f.world.set_position(gate, Point{2913, 2000});
  CHECK(ask(ram) == kNoObject);
  f.world.find(ram)->sight = 2000;
  CHECK(ask(ram) == gate);
  // But never 1000 or more, whatever the sight says.
  f.world.set_position(gate, Point{3000, 2000});
  CHECK(ask(ram) == kNoObject);
  f.world.set_position(gate, Point{2999, 2000});
  CHECK(ask(ram) == gate);

  CHECK(ask(999999) == kNoObject);
}
