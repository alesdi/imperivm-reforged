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
#include "imperivm/core/sim/globals.hpp"
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
