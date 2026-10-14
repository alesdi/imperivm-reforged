// The host audit's first world_host batch: each test encodes one rule read
// from an entry point's body in gbr.exe (docs/audit/README.md), at the address
// its doc comment in sim/world_host.cpp cites, and fails without the fix that
// made this engine answer the same.
//
// Synthetic throughout: no game data, so it runs in CI.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace script = imperivm::core::script;
using imperivm::core::NativeClass;

namespace {

/// A world with every host domain registered, and a way to call one entry.
struct Host {
  World world;
  script::HostRegistry registry;
  HostContext context;

  Host() {
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  script::HostOutcome call(script::CallKind kind, const char* name,
                           std::vector<script::Value> args) {
    const auto arity = static_cast<std::uint16_t>(
        kind == script::CallKind::member ? args.size() - 1 : args.size());
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
  }
  script::HostOutcome member(const char* name, std::vector<script::Value> args) {
    return call(script::CallKind::member, name, std::move(args));
  }
  script::HostOutcome free(const char* name, std::vector<script::Value> args) {
    return call(script::CallKind::free_function, name, std::move(args));
  }
  /// The answer as an integer, after checking the call did not trap.
  std::int64_t integer(const char* name, std::vector<script::Value> args) {
    const script::HostOutcome out = member(name, std::move(args));
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -999999;
  }
  bool truthy(const char* name, std::vector<script::Value> args) {
    return integer(name, std::move(args)) != 0;
  }

  ObjectId unit(PlayerId owner, Point at) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr);
    world.set_owner(id, owner);
    world.set_position(id, at);
    world.mutable_state(id)->health = 10;
    return id;
  }
  ObjectId spawn(NativeClass native, std::int32_t health) {
    const ObjectId id = world.spawn(native, nullptr);
    world.mutable_state(id)->health = health;
    return id;
  }
};

script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }
script::Value named(std::int32_t index) {
  return script::Value::object(kTypeNamedObj, static_cast<std::uint32_t>(index));
}
script::Value str(std::string_view s) { return script::Value::string(std::string(s)); }
script::Value num(std::int32_t v) { return script::Value::integer(v); }

}  // namespace

// --------------------------------------------------------------------------
// identity: IsAlive, IsDead, the named objects
// --------------------------------------------------------------------------

TEST(audit_is_alive_and_is_dead_ask_vtbl_0x50_where_a_building_never_dies) {
  // `Obj::IsAlive` (0x005aae00) and `Obj::IsDead` (0x005aae50) ask
  // `vtbl + 0x50`: health == 0 for units and the catapult (0x004e1f50),
  // "never" for the seven building vtables (0x00746ea0).
  Host h;
  const ObjectId fort = h.spawn(NativeClass::building, 0);
  const ObjectId engine = h.spawn(NativeClass::catapult, 0);
  const ObjectId soldier = h.spawn(NativeClass::unit, 0);
  REQUIRE(h.world.find(fort)->state.flags.is_building);
  REQUIRE(h.world.find(engine)->state.flags.is_building);

  CHECK(h.truthy("IsAlive", {obj(fort)}));
  CHECK(!h.truthy("IsDead", {obj(fort)}));
  CHECK(!h.truthy("IsAlive", {obj(engine)}));
  CHECK(h.truthy("IsDead", {obj(engine)}));
  CHECK(!h.truthy("IsAlive", {obj(soldier)}));
  CHECK(h.truthy("IsDead", {obj(soldier)}));
  CHECK(!h.truthy("IsAlive", {obj(9999)}));
  CHECK(h.truthy("IsDead", {obj(9999)}));
  // The handle check alone: a corpse is still a valid object.
  CHECK(h.truthy("IsValid", {obj(soldier)}));
}

TEST(audit_a_named_unit_that_died_is_not_reached_through_its_name) {
  // `NamedObj::obj` (0x005530a0) purges the dead and answers the invalid
  // handle when nothing is left; the same function is the `NamedObj -> Obj`
  // conversion, so members reached through the name see no object either.
  Host h;
  const ObjectId hero = h.unit(1, Point{300, 400});
  const ObjectId fort = h.spawn(NativeClass::building, 0);
  REQUIRE(h.world.named_objects().bind("NO_Hero", hero));
  REQUIRE(h.world.named_objects().bind("NO_Fort", fort));
  const std::int32_t hero_name = h.world.named_objects().find("NO_Hero");
  const std::int32_t fort_name = h.world.named_objects().find("NO_Fort");

  CHECK(h.integer("player", {named(hero_name)}) == 2);
  h.world.mutable_state(hero)->health = 0;
  const script::HostOutcome bound = h.member("obj", {named(hero_name)});
  REQUIRE(bound.status == script::HostStatus::ok);
  CHECK(!h.truthy("IsValid", {bound.value}));
  CHECK(h.integer("player", {named(hero_name)}) == -1);
  CHECK(h.truthy("IsDead", {named(hero_name)}));
  CHECK(h.truthy("IsValid", {named(hero_name)}));  // the name still exists

  // A named building at no health is broken, not dead.
  CHECK(!h.truthy("IsDead", {named(fort_name)}));
  const script::HostOutcome fort_bound = h.member("obj", {named(fort_name)});
  CHECK(h.truthy("IsValid", {fort_bound.value}));
}

TEST(audit_get_named_obj_mints_an_empty_entry_that_set_name_fills) {
  // `GetNamedObj` (0x00553230): a miss constructs an empty named query under
  // the name (0x005722a0) and answers it -- valid, dead, `.obj` invalid -- and
  // a later binding of that name fills the same entry.
  Host h;
  const script::HostOutcome early = h.free("GetNamedObj", {str("NO_Later")});
  REQUIRE(early.status == script::HostStatus::ok);
  CHECK(h.truthy("IsValid", {early.value}));
  CHECK(h.truthy("IsDead", {early.value}));
  CHECK(!h.truthy("IsValid", {h.member("obj", {early.value}).value}));

  const ObjectId later = h.unit(1, Point{10, 10});
  REQUIRE(h.world.named_objects().rebind("NO_Later", later));
  const script::HostOutcome now = h.member("obj", {early.value});
  REQUIRE(now.value.is_object());
  CHECK(now.value.as_object().id == later);
  CHECK(!h.truthy("IsDead", {early.value}));
}

TEST(audit_an_unbound_named_entry_survives_a_save) {
  Host h;
  (void)h.world.named_objects().intern("NO_Pending");
  const ObjectId bound = h.unit(1, Point{10, 10});
  REQUIRE(h.world.named_objects().bind("NO_Bound", bound));
  std::vector<std::byte> bytes;
  h.world.serialize(bytes);
  World back;
  REQUIRE(back.deserialize(bytes).ok());
  CHECK(back.named_objects().find("NO_Pending") != NamedObjectTable::kNoName);
  CHECK(back.named_objects().object("NO_Pending") == kNoObject);
  CHECK(back.named_objects().object("NO_Bound") == bound);
}

TEST(audit_squadlist_is_valid_asks_the_pool) {
  // `SquadList::IsValid` (0x00421a70): the index lies in the squad-list pool.
  Host h;
  const SquadListId id = h.world.squadlists().acquire(1, 0);
  CHECK(h.truthy("IsValid", {make_squadlist_value(id)}));
  CHECK(!h.truthy("IsValid", {make_squadlist_value(id + 99)}));
}

// --------------------------------------------------------------------------
// visibility: SetVisible, CanSee
// --------------------------------------------------------------------------

TEST(audit_set_visible_on_a_gone_object_is_a_no_op_and_takes_a_list) {
  // `Obj::SetVisible` (0x005ab920) prints and returns on a miss;
  // `ObjList::SetVisible` (0x0055ea60) writes every live member.
  Host h;
  CHECK(h.member("SetVisible", {obj(9999), num(1)}).status == script::HostStatus::ok);

  const ObjectId a = h.unit(1, Point{10, 10});
  const ObjectId b = h.unit(1, Point{20, 20});
  ObjListPool& pool = objlist_pool_of(h.world);
  const ObjListId list = pool.acquire(1, 0);
  pool.mutable_items(list)->assign({a, b});
  CHECK(h.member("SetVisible", {make_objlist_value(list), num(0)}).status ==
        script::HostStatus::ok);
  CHECK(h.world.find(a)->state.flags.hidden);
  CHECK(h.world.find(b)->state.flags.hidden);
}

TEST(audit_can_see_reads_the_target_first_and_a_hidden_one_through_shared_view) {
  // `Obj::CanSee` (0x005ab9d0): the argument is checked first; a visible one
  // is seen without the receiver; a hidden one when its owner shares view
  // with the observer's owner (record +0x18, filled by SetRelation 0x005652f0
  // from bit 0x10).
  Host h;
  const ObjectId looker = h.unit(0, Point{0, 0});
  const ObjectId sneak = h.unit(1, Point{900, 900});
  CHECK(h.truthy("CanSee", {obj(9999), obj(sneak)}));
  CHECK(!h.truthy("CanSee", {obj(looker), obj(9999)}));

  h.world.mutable_state(sneak)->flags.hidden = true;
  h.world.players().set_relation_word(1, 0, 0);
  CHECK(!h.truthy("CanSee", {obj(looker), obj(sneak)}));
  h.world.players().set_relation_word(1, 0, kRelationFriendly);  // carries 0x10
  CHECK(h.truthy("CanSee", {obj(looker), obj(sneak)}));
  // One-directional: the observer's grant to the target's owner is not it.
  h.world.players().set_relation_word(1, 0, 0);
  h.world.players().set_relation_word(0, 1, kRelationFriendly);
  CHECK(!h.truthy("CanSee", {obj(looker), obj(sneak)}));
  // A player sees its own stealthed units.
  const ObjectId mate = h.unit(1, Point{5, 5});
  CHECK(h.truthy("CanSee", {obj(mate), obj(sneak)}));
}

TEST(audit_query_set_messenger_status_skips_the_dead) {
  // `Query::SetMessengerStatus` (0x00579c70) skips members `vtbl + 0x50`
  // calls dead; `Obj::SetMessengerStatus` (0x005ab8d0) has no life test.
  Host h;
  const ObjectId live = h.unit(1, Point{100, 100});
  const ObjectId corpse = h.unit(1, Point{120, 100});
  h.world.mutable_state(corpse)->health = 0;
  const script::HostOutcome all = h.free("Group", {str("G")});
  REQUIRE(all.status == script::HostStatus::ok);
  CHECK(h.member("AddToGroup", {obj(live), str("G")}).status == script::HostStatus::ok);
  CHECK(h.member("AddToGroup", {obj(corpse), str("G")}).status == script::HostStatus::ok);
  CHECK(h.member("SetMessengerStatus", {all.value, num(1)}).status == script::HostStatus::ok);
  CHECK(h.world.find(live)->state.flags.messenger);
  CHECK(!h.world.find(corpse)->state.flags.messenger);
  CHECK(h.member("SetMessengerStatus", {obj(corpse), num(1)}).status == script::HostStatus::ok);
  CHECK(h.world.find(corpse)->state.flags.messenger);
}

TEST(audit_is_empty_answers_true_for_a_gone_query_and_false_for_a_sacrifice) {
  // `Query::IsEmpty` (0x00579320) and `Sacrifice::IsEmpty` (0x00594eb0) both
  // answer true on a miss rather than trapping.
  Host h;
  const script::HostOutcome gone =
      h.member("IsEmpty", {script::Value::object(kTypeQuery, 9999)});
  REQUIRE(gone.status == script::HostStatus::ok);
  CHECK(gone.value.as_integer() != 0);
  const ObjectId altar = h.spawn(NativeClass::sacrifice, 10);
  const script::HostOutcome sacrifice = h.member("IsEmpty", {obj(altar)});
  REQUIRE(sacrifice.status == script::HostStatus::ok);
  CHECK(sacrifice.value.as_integer() == 0);
}

TEST(audit_pos_and_pos_rh_answer_the_origin_for_an_invalid_receiver) {
  // 0x005add4d and 0x005ad8c6 write (0, 0), not the held marker.
  Host h;
  for (const char* name : {"pos", "posRH"}) {
    const script::HostOutcome out = h.member(name, {obj(9999)});
    REQUIRE(out.status == script::HostStatus::ok);
    CHECK(unpack_point(out.value) == (Point{0, 0}));
  }
}

// --------------------------------------------------------------------------
// distance and ancestry: DistTo, IsHeirOf
// --------------------------------------------------------------------------

TEST(audit_dist_to_measures_stored_positions_and_answers_minus_one_on_a_miss) {
  // `Obj::DistTo` (0x005aa350 point, 0x005aa3e0 object -> 0x005a77b0) reads
  // `[obj+0x24/0x28]` -- the (-1, -1) marker for a held unit -- and pushes -1
  // for an invalid receiver or argument. No class graph, so every radius is 0.
  Host h;
  const World::ShipIds ship = h.world.spawn_ship(nullptr);
  REQUIRE(h.world.set_position(ship.ship, Point{1200, 1500}));
  const ObjectId passenger = h.unit(1, Point{0, 0});
  REQUIRE(h.world.put_in_holder(passenger, ship.holder));
  const ObjectId other = h.unit(1, Point{11, 11});
  const ObjectId free = h.unit(1, Point{700, 900});

  CHECK(h.integer("DistTo", {obj(passenger), pack_point(Point{2, 3})}) == 5);
  CHECK(h.integer("DistTo", {obj(passenger), obj(other)}) == 16);  // floor(sqrt(288))
  CHECK(h.integer("DistTo", {obj(free), pack_point(Point{703, 904})}) == 5);
  CHECK(h.integer("DistTo", {obj(9999), pack_point(Point{1, 1})}) == -1);
  CHECK(h.integer("DistTo", {obj(free), obj(9999)}) == -1);
}

TEST(audit_is_heir_of_takes_the_whole_string_as_one_class_name) {
  // `Obj::IsHeirOf` (0x005ad450) looks the string up whole, exactly
  // (0x00409e40, 0x00409350): no comma list, no trimming.
  imperivm::core::ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties/></class>)",
      R"(<class id="Unit" parent="Object" cpp_class="CVXUnit"><properties/></class>)",
      R"(<class id="Soldier" parent="Unit" cpp_class="CVXUnit"><properties/></class>)",
      R"(<class id="Fort" parent="Object" cpp_class="CVXBuilding"><properties/></class>)",
  };
  const char* names[] = {"object.sc.xml", "unit.sc.xml", "soldier.sc.xml", "fort.sc.xml"};
  for (int i = 0; i < 4; ++i) {
    graph.add({reinterpret_cast<const std::byte*>(docs[i].data()), docs[i].size()}, names[i]);
  }
  graph.link();
  Host h;
  h.world.set_class_graph(&graph);
  const ObjectId soldier = h.world.spawn(NativeClass::unit, nullptr, graph.find("Soldier"));

  CHECK(h.truthy("IsHeirOf", {obj(soldier), str("Unit")}));
  CHECK(h.truthy("IsHeirOf", {obj(soldier), str("Soldier")}));
  CHECK(!h.truthy("IsHeirOf", {obj(soldier), str("Fort")}));
  CHECK(!h.truthy("IsHeirOf", {obj(soldier), str("Fort,Unit")}));
  CHECK(!h.truthy("IsHeirOf", {obj(soldier), str(" Unit")}));
  CHECK(!h.truthy("IsHeirOf", {obj(9999), str("Unit")}));
}

// --------------------------------------------------------------------------
// units and buildings: SetLastAttackTime, HasSpecial, Teleport::Traverse
// --------------------------------------------------------------------------

TEST(audit_set_last_attack_time_stamps_the_clock_time_without_attack_reads) {
  // `Unit::SetLastAttackTime` (0x005d7fc0) writes the game clock into
  // `[unit+0x1a4]`, which `TimeWithoutAttack` (0x005d7f50) reads back.
  Host h;
  CombatSystem combat;
  const ObjectId sneak = h.unit(1, Point{100, 100});
  Combatant record;
  record.id = sneak;
  record.owner = 1;
  record.health = 10;
  combat.add(record);
  REQUIRE(h.world.add_system(&combat));
  h.world.advance_turns(10);
  const GameTime now = h.world.time();
  REQUIRE(now > 0);

  CHECK(h.integer("TimeWithoutAttack", {obj(sneak)}) == now);
  CHECK(h.member("SetLastAttackTime", {obj(sneak)}).status == script::HostStatus::ok);
  CHECK(combat.find(sneak)->last_attack_time == now);
  CHECK(h.integer("TimeWithoutAttack", {obj(sneak)}) == 0);
  CHECK(h.member("SetLastAttackTime", {obj(9999)}).status == script::HostStatus::ok);
}

TEST(audit_has_special_answers_freedom_for_every_hero) {
  // `Unit::HasSpecial` (0x005d8d70) reads `[obj+0x198]`, where the hero
  // constructor (0x0052ffd0) sets bit 30, `freedom`, whatever the class says.
  Host h;
  const ObjectId hero = h.spawn(NativeClass::hero, 10);
  const ObjectId soldier = h.spawn(NativeClass::unit, 10);
  REQUIRE(h.world.find(hero)->state.flags.is_hero);
  CHECK(h.truthy("HasSpecial", {obj(hero), num(30)}));
  CHECK(!h.truthy("HasSpecial", {obj(soldier), num(30)}));
  CHECK(!h.truthy("HasSpecial", {obj(hero), num(31)}));
}

TEST(audit_set_no_ai_flag_regroups_the_unit_so_its_squad_carries_the_flag) {
  // Both `SetNoAIFlag` bodies (0x00435470 free, 0x005de600 member) write
  // `[unit+0x194] & 0x40000` and then hand the unit(s) to the Squadize worker
  // 0x00447330 with state 0, which gives each a fresh squad whose `SF_NOAI`
  // is the unit's own bit -- the bit the AI reads.
  Host h;
  HeroSystem heroes;
  REQUIRE(h.world.add_system(&heroes));
  const ObjectId a = h.unit(1, Point{100, 100});
  const ObjectId b = h.unit(1, Point{120, 100});
  SquadTable& squads = heroes.squads();
  const SquadKey shared = squads.create(1);
  REQUIRE(squads.join(shared, a));
  REQUIRE(squads.join(shared, b));
  REQUIRE((squads.find(shared)->flags & kSquadFlagNoAi) == 0);

  CHECK(h.member("SetNoAIFlag", {obj(a), num(1)}).status == script::HostStatus::ok);
  CHECK(h.world.find(a)->state.flags.no_ai);
  const Squad* own = squads.find(squads.squad_of(a));
  REQUIRE(own != nullptr);
  CHECK(squads.squad_of(a) != squads.squad_of(b));
  CHECK((own->flags & kSquadFlagNoAi) != 0);
  CHECK(own->state == 0);
  CHECK((squads.find(squads.squad_of(b))->flags & kSquadFlagNoAi) == 0);

  ObjListPool& pool = objlist_pool_of(h.world);
  const ObjListId list = pool.acquire(1, 0);
  pool.mutable_items(list)->assign({a});
  CHECK(h.free("SetNoAIFlag", {make_objlist_value(list), num(0)}).status ==
        script::HostStatus::ok);
  CHECK(!h.world.find(a)->state.flags.no_ai);
  CHECK((squads.find(squads.squad_of(a))->flags & kSquadFlagNoAi) == 0);
}

// --------------------------------------------------------------------------
// queries and groups
// --------------------------------------------------------------------------

namespace {

std::vector<ObjectId> members_of(Host& h, const script::Value& query) {
  std::vector<ObjectId> found;
  if (query.is_object()) h.world.evaluate_query(query.as_object().id, found);
  return found;
}

}  // namespace

TEST(audit_set_op_ordinals_and_orders_are_the_refreshs) {
  // `Union` passes 0 to 0x004fe490, `Intersect` 1, `Subtract` 2. Union is
  // sorted (0x004fb510 + merge); subtract keeps the left operand's order;
  // intersect walks the smaller operand in its own order.
  Host h;
  const ObjectId a = h.unit(1, Point{100, 100});
  const ObjectId b = h.unit(1, Point{120, 100});
  const ObjectId c = h.unit(1, Point{140, 100});
  CHECK(h.member("AddToGroup", {obj(b), str("G")}).status == script::HostStatus::ok);
  CHECK(h.member("AddToGroup", {obj(a), str("G")}).status == script::HostStatus::ok);
  const script::Value g = h.free("Group", {str("G")}).value;
  const script::Value empty = h.free("Group", {str("E")}).value;
  const script::Value circle = h.free("ObjsInCircle", {pack_point(Point{100, 100}), num(100), str("")}).value;
  REQUIRE(members_of(h, circle).size() == 3);
  (void)c;

  const script::Value u = h.free("Union", {g, empty}).value;
  const script::Value i = h.free("Intersect", {g, circle}).value;
  const script::Value d = h.free("Subtract", {g, empty}).value;
  CHECK(static_cast<int>(h.world.query_spec(u.as_object().id)->op) == 0);
  CHECK(static_cast<int>(h.world.query_spec(i.as_object().id)->op) == 1);
  CHECK(static_cast<int>(h.world.query_spec(d.as_object().id)->op) == 2);

  CHECK((members_of(h, u) == std::vector<ObjectId>{a, b}));
  CHECK((members_of(h, d) == std::vector<ObjectId>{b, a}));
  CHECK((members_of(h, i) == std::vector<ObjectId>{b, a}));
}

TEST(audit_area_queries_pass_over_the_dead_and_take_a_zero_radius_as_a_spot) {
  // 0x004fc980 (the map-area sweeps) refuses a candidate `vtbl + 0x50` calls
  // dead; the circle sweep 0x004fde60 is `dist² <= r²`, so r = 0 is the exact
  // spot and r < 0 nothing.
  Host h;
  const ObjectId here = h.unit(1, Point{1000, 1000});
  const ObjectId near = h.unit(1, Point{1005, 1000});
  const ObjectId corpse = h.unit(1, Point{1050, 1000});
  h.world.mutable_state(corpse)->health = 0;
  const ObjectId ruin = h.spawn(NativeClass::building, 0);
  REQUIRE(h.world.set_position(ruin, Point{1060, 1000}));

  const auto circle = [&](std::int32_t r) {
    return members_of(h, h.free("ObjsInCircle", {pack_point(Point{1000, 1000}), num(r), str("")}).value);
  };
  CHECK((circle(100) == std::vector<ObjectId>{here, near, ruin}));
  CHECK((circle(0) == std::vector<ObjectId>{here}));
  CHECK(circle(-5).empty());
}

TEST(audit_class_player_objs_never_lists_the_held_or_the_dead) {
  // 0x004fe860 with no area sweeps the whole map through the grid (0x004fd2d0),
  // where a held object has no cell, and 0x004fc6b0 refuses the dead.
  Host h;
  const ObjectId out = h.unit(0, Point{500, 500});
  const ObjectId corpse = h.unit(0, Point{600, 500});
  h.world.mutable_state(corpse)->health = 0;
  const World::ShipIds ship = h.world.spawn_ship(nullptr);
  const ObjectId aboard = h.unit(0, Point{0, 0});
  REQUIRE(h.world.put_in_holder(aboard, ship.holder));
  CHECK((members_of(h, h.free("ClassPlayerObjs", {str(""), num(1)}).value) ==
         std::vector<ObjectId>{out}));
}

TEST(audit_objs_in_sight_freezes_sight_less_one_hundred_and_ignores_a_dead_eye) {
  // 0x004fee10 turns radius -1 into `[obj+0xd0] - 100`, once; the refresh
  // 0x004ff080 answers nothing for a dead observer.
  Host h;
  const ObjectId eye = h.unit(1, Point{1000, 1000});
  h.world.find(eye)->sight = 300;
  const ObjectId close = h.unit(2, Point{1150, 1000});
  const ObjectId edge = h.unit(2, Point{1250, 1000});
  const script::Value q = h.free("ObjsInSight", {obj(eye), str("")}).value;
  CHECK((members_of(h, q) == std::vector<ObjectId>{close}));
  h.world.find(eye)->sight = 1000;  // a later SetSight does not widen it
  CHECK((members_of(h, q) == std::vector<ObjectId>{close}));
  (void)edge;
  h.world.mutable_state(eye)->health = 0;
  CHECK(members_of(h, q).empty());
}

TEST(audit_objs_in_range_is_a_sight_query_that_leaves_its_object_out) {
  // 0x00576130 calls 0x004fee10 with the script's radius.
  Host h;
  const ObjectId self = h.unit(1, Point{1000, 1000});
  const ObjectId other = h.unit(1, Point{1100, 1000});
  CHECK((members_of(h, h.free("ObjsInRange", {obj(self), str(""), num(500)}).value) ==
         std::vector<ObjectId>{other}));
  CHECK(members_of(h, h.free("ObjsInRange", {obj(self), str(""), num(0)}).value).empty());
}

TEST(audit_query_constructors_answer_invalid_for_a_class_that_does_not_resolve) {
  // 0x00409e40 resolves the whole string; a miss makes the constructor answer
  // the invalid handle. ObjsInSight and ObjsInCircle let a comma list through.
  imperivm::core::ClassGraph graph;
  const std::string doc = R"(<class id="Unit" cpp_class="CVXUnit"><properties/></class>)";
  graph.add({reinterpret_cast<const std::byte*>(doc.data()), doc.size()}, "unit.sc.xml");
  graph.link();
  Host h;
  h.world.set_class_graph(&graph);
  const ObjectId self = h.unit(1, Point{1000, 1000});
  const auto valid = [&](const script::HostOutcome& out) {
    return out.status == script::HostStatus::ok && h.truthy("IsValid", {out.value});
  };
  CHECK(valid(h.free("ClassPlayerObjs", {str("Unit"), num(2)})));
  CHECK(!valid(h.free("ClassPlayerObjs", {str("NoSuchClass"), num(2)})));
  CHECK(!valid(h.free("ClassPlayerObjs", {str("Unit,Unit"), num(2)})));
  CHECK(!valid(h.free("ObjsInRange", {obj(self), str("NoSuchClass"), num(100)})));
  CHECK(!valid(h.free("ObjsInCircle", {pack_point(Point{0, 0}), num(100), str("NoSuchClass")})));
  CHECK(valid(h.free("ObjsInCircle", {pack_point(Point{0, 0}), num(100), str("Unit,Nope")})));
  CHECK(!valid(h.free("ObjsInSight", {obj(self), str("NoSuchClass")})));
}

TEST(audit_group_membership_checks_the_receiver_and_a_list_skips_the_dead) {
  // `Obj::AddToGroup` (0x005b6730) returns on a bad handle before any group is
  // made; the `ObjList` forms (0x00561970, 0x0055f720) act on members that
  // resolve and are not dead.
  Host h;
  CHECK(h.member("AddToGroup", {obj(9999), str("Fresh")}).status == script::HostStatus::ok);
  CHECK(h.world.groups().find("Fresh") == GroupTable::kNoGroup);

  const ObjectId live = h.unit(1, Point{10, 10});
  const ObjectId corpse = h.unit(1, Point{20, 10});
  h.world.mutable_state(corpse)->health = 0;
  ObjListPool& pool = objlist_pool_of(h.world);
  const ObjListId list = pool.acquire(1, 0);
  pool.mutable_items(list)->assign({live, corpse});
  CHECK(h.member("AddToGroup", {make_objlist_value(list), str("L")}).status ==
        script::HostStatus::ok);
  CHECK(h.free("GetGroupSize", {str("L")}).value.as_integer() == 1);

  // A corpse already in a group is not taken out by the list form.
  CHECK(h.member("AddToGroup", {obj(corpse), str("L")}).status == script::HostStatus::ok);
  CHECK(h.member("RemoveFromGroup", {make_objlist_value(list), str("L")}).status ==
        script::HostStatus::ok);
  const std::int32_t l = h.world.groups().find("L");
  CHECK(h.world.groups().contains(l, corpse));
  CHECK(!h.world.groups().contains(l, live));
}

// --------------------------------------------------------------------------
// spawning and points
// --------------------------------------------------------------------------

TEST(audit_spawn_group_in_holder_enters_the_garrison_roster_past_its_cap) {
  // 0x005769b0 spawns through 0x005724a0, which enters each copy with
  // 0x005d3e10(holder, force = 1): onto the settlement's roster even when full.
  Host h;
  EconomySystem economy;
  REQUIRE(h.world.add_system(&economy));
  economy.start(h.world);
  SettlementInit init;
  init.owner = 0;
  init.kind = SettlementKind::stronghold;
  init.name = "S_T";
  init.max_units = 0;
  const SettlementId town = economy.create(h.world, init);
  REQUIRE(town != kNoSettlement);

  const ObjectId templ = h.unit(0, Point{300, 300});
  h.world.mutable_state(templ)->flags.unspawned = true;
  h.world.groups().add(h.world.group_index("G"), templ);

  const script::HostOutcome out = h.free("SpawnGroupInHolder", {str("G"), str("S_T")});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  const std::span<const ObjectId> copies = objlist_pool_of(h.world).items(objlist_of(out.value));
  REQUIRE(copies.size() == 1);
  const Settlement* s = economy.settlements().find(town);
  REQUIRE(s != nullptr);
  CHECK(s->holder.contains(copies[0]));
  CHECK(h.world.find(copies[0])->state.holder == s->holder.object);
}

TEST(audit_point_set_answers_the_point_and_lengths_wrap_at_32_bits) {
  // `point::Set` (0x00697c50) returns the point it stores; `Len` (0x00695360),
  // `Dist` (0x00695310) and `SetLen` (0x00697d10) square, add and multiply in
  // 32 bits and take the root of the sum read as unsigned.
  Host h;
  const script::HostOutcome set = h.member("Set", {pack_point(Point{1, 2}), num(5), num(6)});
  REQUIRE(set.status == script::HostStatus::ok);
  REQUIRE(is_point(set.value));
  CHECK(unpack_point(set.value) == (Point{5, 6}));

  CHECK(h.integer("Len", {pack_point(Point{70000, 0})}) == 24597);
  CHECK(h.free("Dist", {pack_point(Point{0, 0}), pack_point(Point{70000, 0})}).value.as_integer() ==
        24597);
  CHECK(h.integer("Len", {pack_point(Point{3, 4})}) == 5);
  const auto set_len = [&](Point p, std::int32_t length) -> Point {
    script::Value args[] = {pack_point(p), num(length)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &h.context;
    ctx.name = "SetLen";
    ctx.kind = script::CallKind::member;
    const std::uint32_t index = h.registry.find(script::CallKind::member, "SetLen", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return Point{};
    (void)h.registry.entry(index).fn(ctx);
    return unpack_point(args[0]);
  };
  CHECK(set_len(Point{16000, 0}, 200000) == (Point{-68435, 0}));
  CHECK(set_len(Point{65536, 0}, 10) == (Point{0, 10}));
  CHECK(set_len(Point{3, 4}, 10) == (Point{6, 8}));
}

TEST(audit_rot_keeps_the_residue_of_an_exact_half) {
  // `point::Rot` (0x00697d90) works on `d * K` with K just under pi/180, so
  // sin(30) is a hair under 1/2 and cos(120) a hair over -1/2.
  Host h;
  const auto rot = [&](Point p, std::int32_t degrees) -> Point {
    script::Value args[] = {pack_point(p), num(degrees)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &h.context;
    ctx.name = "Rot";
    ctx.kind = script::CallKind::member;
    const std::uint32_t index = h.registry.find(script::CallKind::member, "Rot", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return Point{};
    (void)h.registry.entry(index).fn(ctx);
    return unpack_point(args[0]);
  };
  // Each of these from a double-precision emulation of the original's
  // `trunc(x cos + y sin)`, `trunc(y cos - x sin)`.
  CHECK(rot(Point{0, 2}, 30) == (Point{0, 1}));
  CHECK(rot(Point{0, 2}, 210) == (Point{0, -1}));
  CHECK(rot(Point{0, 2}, 120) == (Point{1, 0}));
  CHECK(rot(Point{2, 0}, 60) == (Point{1, -1}));
  CHECK(rot(Point{0, 4}, 150) == (Point{2, -3}));
  CHECK(rot(Point{4, 0}, 300) == (Point{1, 3}));
  CHECK(rot(Point{0, 2}, -30) == (Point{0, 1}));
  CHECK(rot(Point{2, 0}, -60) == (Point{1, 1}));
  CHECK(rot(Point{0, 2}, 330) == (Point{-1, 1}));
  CHECK(rot(Point{2, 0}, 240) == (Point{-1, 1}));
  CHECK(rot(Point{2, 0}, 120) == (Point{0, -1}));
}

TEST(audit_traverse_records_the_travellers_whole_view_sharing_side) {
  // `Teleport::Traverse` (0x005cdcb0) ORs the player record's word +0x18 --
  // the players who share view with the traveller, itself included -- into
  // `[teleport+0x214]`.
  Host h;
  const ObjectId gate = h.spawn(NativeClass::teleport, 10);
  h.world.players().set_relation_word(1, 0, kRelationFriendly);  // 2 shares view with 1
  h.world.players().set_relation_word(0, 2, kRelationFriendly);  // 1 toward 3: not it
  CHECK(h.member("Traverse", {obj(gate), num(1)}).status == script::HostStatus::ok);
  CHECK(h.world.find(gate)->state.traversed_by == 0b011u);
  CHECK(h.member("Traverse", {obj(gate), num(0)}).status == script::HostStatus::ok);
  CHECK(h.world.find(gate)->state.traversed_by == 0b011u);
}
