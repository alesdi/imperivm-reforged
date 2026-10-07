// Squads and GAIKAs: sim/squad.hpp, sim/gaika.hpp.
//
// The subject of this file is a *representation*, so most of it is about what
// survives a round trip rather than about behaviour. Two cases carry the
// reading and are worth naming:
//
//   * **A squad handle keeps the player.** Squad index 4 exists simultaneously
//     for players 0, 2, 3 and 14 in one desync dump (docs/engine/state-vector.md),
//     so a handle that carried only the index would silently alias four
//     different armies. `two_players_same_index_are_two_handles` is that.
//   * **`SetCmd` reaches every member, not the leader.** `SQUADMONITOR.VS`
//     line 324 orders a squad to its own leader's position, which is a no-op
//     under any other reading.
//
// No game data anywhere in this file: the class is a string literal written
// here, and the `SetCmd` argument shapes are transcriptions of shipped calls in
// `DATA\AI\SQUADMONITOR.VS`, `GS_CAPTURE.VS` and `AIOSENDSQUAD.VS`.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include <array>

#include "imperivm/core/sim/array.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/heading.hpp"
#include "imperivm/core/sim/gaika.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/item.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/script/bytecode.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;
using namespace imperivm::core::script;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// One unit class binding the handful of `<method sig>` rows the shipped
/// `SetCmd` calls name. `ai_killall` really is a method and not a `<cmd>`; see
/// sim/squad.hpp.
ClassGraph fixture_graph() {
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent="">
      <properties maxhealth="200" speed="50" sight="500" radius="15"/>
      <method sig="idle"       vs="data/subai/unit_idle.vs"/>
      <method sig="move"       vs="data/subai/unit_move.vs"/>
      <method sig="engage"     vs="data/subai/unit_engage.vs"/>
      <method sig="advance"    vs="data/subai/unit_advance.vs"/>
      <method sig="enter"      vs="data/subai/unit_enter.vs"/>
      <method sig="capture"    vs="data/subai/unit_capture.vs"/>
      <method sig="ai_killall" vs="data/subai/unit_ai_killall.vs"/>
      <method sig="getitems"   vs="data/subai/unit_getitems.vs"/>
    </class>)"),
            "unit.sc.xml");
  // Two descendants, so that "how many of this class" is a different question
  // from "how many members". `Count` is the only entry point here that needs it.
  graph.add(bytes(R"(<class id="Horse" cpp_class="CVXUnit" parent="Unit"/>)"), "horse.sc.xml");
  graph.add(bytes(R"(<class id="GDruid" cpp_class="CVXUnit" parent="Unit"/>)"), "druid.sc.xml");
  // A sentry's specials: `Unit::HasFreedom` answers from the class's
  // `freedom` bit whatever the unit record says.
  graph.add(bytes(R"(<class id="Sentry" cpp_class="CVXUnit" parent="Unit">
      <properties unit_specials="Keen sight, Freedom"/>
    </class>)"),
            "sentry.sc.xml");
  graph.link();
  return graph;
}

/// A world with a hero system (which owns the `SquadTable`) and a command
/// system (which `SetCmd` composes with). No scheduler: a command is then
/// queued and marked running and never retires, which is what a test of queue
/// shape wants.
struct Fixture {
  ClassGraph graph = fixture_graph();
  World world;
  HeroSystem heroes;
  CommandSystem commands;
  ClassIndex unit_class = kNoClass;

  Fixture() {
    world.set_class_graph(&graph);
    unit_class = graph.find("Unit");
    world.add_system(&heroes);
    world.add_system(&commands);
  }

  ObjectId spawn(Point at = Point{100, 100}, PlayerId owner = 1) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, unit_class);
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, 100);
    return id;
  }

  /// A squad of `n` freshly spawned units owned by `owner`.
  SquadKey squad_of(PlayerId owner, std::size_t n) {
    const SquadKey key = heroes.squads().create(owner);
    for (std::size_t i = 0; i < n; ++i) {
      (void)heroes.squads().join(key, spawn(Point{100 + static_cast<std::int32_t>(i) * 20, 100},
                                            owner));
    }
    return key;
  }
};

struct HostCall {
  std::vector<Value> arguments;
  CallContext context;
  imperivm::core::sim::HostContext context_state;

  HostCall(World& world, std::initializer_list<Value> args, bool bind = true) : arguments(args) {
    context_state.world = &world;
    context.arguments = arguments;
    context.user = bind ? &context_state : nullptr;
  }
};

HostOutcome invoke(const HostRegistry& registry, CallKind kind, std::string_view name,
                   std::uint16_t arity, HostCall& call) {
  const std::uint32_t index = registry.find(kind, name, arity);
  if (index == kUnresolvedHost) return HostOutcome::failed("not registered");
  const HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return HostOutcome::failed("not implemented");
  return entry.fn(call.context);
}

Value obj(ObjectId id) { return Value::object(ObjectRef{kTypeObj, id}); }

}  // namespace

// --------------------------------------------------------------------------
// the handle
// --------------------------------------------------------------------------

TEST(a_squad_handle_round_trips_over_the_whole_observed_range) {
  // The dumps show squad indices 0 through 91 and eight distinct player values
  // including `kNeutralWildlife` (14) and `kNeutralPassive` (15). Swept rather
  // than sampled: an off-by-one in the shift is invisible at small values.
  for (std::int32_t index = 1; index <= 96; ++index) {
    for (std::int32_t player = 0; player <= 15; ++player) {
      const SquadKey key{index, static_cast<PlayerId>(player)};
      const Value packed = pack_squad(key);
      CHECK(is_squad(packed));
      CHECK(unpack_squad(packed) == key);
    }
  }
}

TEST(two_players_same_index_are_two_handles) {
  // Index 4 exists at once for players 0, 2, 3 and 14 in one dump. A handle
  // that dropped the player would make those one squad.
  const Value a = pack_squad(SquadKey{4, 0});
  const Value b = pack_squad(SquadKey{4, 2});
  const Value c = pack_squad(SquadKey{4, 14});
  CHECK(!(a.as_object() == b.as_object()));
  CHECK(!(b.as_object() == c.as_object()));
  CHECK(unpack_squad(a).player == 0);
  CHECK(unpack_squad(c).player == 14);
  CHECK(unpack_squad(a).index == unpack_squad(c).index);
}

TEST(no_squad_packs_to_an_invalid_handle) {
  // `kNoSquad` is the dumps' `0(0)`. It has to read as invalid rather than as
  // squad 0 of player 255, so that `.IsValid` and `Host::truthy` answer without
  // a special case.
  const Value none = pack_squad(kNoSquad);
  CHECK(!is_squad(none));
  CHECK(!none.as_object().valid());
  CHECK(unpack_squad(none) == kNoSquad);
  // An index of zero with a real player is still "no squad".
  CHECK(unpack_squad(pack_squad(SquadKey{0, 3})) == kNoSquad);
}

TEST(a_squad_handle_is_not_an_object_or_a_point_handle) {
  CHECK(!is_squad(obj(7)));
  CHECK(!is_squad(pack_point(Point{4, 9})));
  CHECK(!is_squad(Value::integer(0)));
  CHECK(!is_squad(Value::nil()));
  // And the other way: a squad handle must not pass for an object, or a class
  // filter would accept it.
  const Value squad = pack_squad(SquadKey{4, 2});
  CHECK(squad.as_object().type != kTypeObj);
  CHECK(squad.as_object().type != kTypePoint);
  CHECK(squad.as_object().type != kTypeQuery);
  CHECK(squad.as_object().type != kTypeSettlement);
}

TEST(a_gaika_is_an_integer_and_zero_is_none) {
  // `SQUADMONITOR.VS` compares a GAIKA against the integer literal 0 three
  // times (`sq.AIDest > 0`, `sq.AIDest == 0`), which only a numeric value
  // supports. See sim/gaika.hpp for the six sources behind this.
  CHECK(gaika_value(kNoGaika).is_integer());
  CHECK(gaika_value(kNoGaika).as_integer() == 0);
  CHECK(gaika_value(17).as_integer() == 17);
  CHECK(gaika_of(gaika_value(17)) == 17);
  // Both corpus walks over the id space start at 1, so a non-positive id is
  // "no GAIKA" rather than a node.
  CHECK(gaika_value(-3).as_integer() == kNoGaika);
  CHECK(gaika_of(Value::integer(-1)) == kNoGaika);
  CHECK(gaika_of(Value::nil()) == kNoGaika);
  CHECK(gaika_of(obj(4)) == kNoGaika);
}

// --------------------------------------------------------------------------
// the table still holds up under the new fields
// --------------------------------------------------------------------------

TEST(the_ai_fields_default_to_nothing_and_stay_out_of_the_hash) {
  // Membership is hashed because every unit in a dump prints `squad=<n>(<p>)`.
  // The AI fields appear only in `Squad::Dump`, a debug console command, and in
  // none of the nine dumps in the retail install -- so hashing them would put a
  // subsystem in the determinism contract that the shipped build left out.
  SquadTable table;
  const SquadKey key = table.create(2);
  Squad* squad = table.find(key);
  REQUIRE(squad != nullptr);
  CHECK(squad->state == 0);
  CHECK(squad->flags == 0);
  CHECK(squad->ai_dest == kNoGaika);
  CHECK(squad->gaika_in == kNoGaika);
  CHECK(squad->src_gaika == kNoGaika);
  CHECK(squad->dest_gaika == kNoGaika);
  CHECK(squad->order_dest == kNoGaika);

  std::uint64_t before = 0;
  table.hash(before);
  squad->state = 5;
  squad->flags = 0x0104;
  squad->ai_dest = 9;
  squad->gaika_in = 3;
  squad->last_fight_time = 12345;
  squad->last_attacker = 77;
  std::uint64_t after = 0;
  table.hash(after);
  CHECK(before == after);

  // Membership, by contrast, is hashed.
  (void)table.join(key, 41);
  std::uint64_t joined = 0;
  table.hash(joined);
  CHECK(joined != after);

  // **Not hashed is not the same as not saved**, which is the whole point of
  // the split above: every one of these has to come back.
  std::vector<std::byte> bytes_out;
  table.serialize(bytes_out);
  SquadTable loaded;
  REQUIRE(loaded.deserialize(bytes_out).ok());
  const Squad* back = loaded.find(key);
  REQUIRE(back != nullptr);
  CHECK(back->state == 5);
  CHECK(back->flags == 0x0104);
  CHECK(back->ai_dest == 9);
  CHECK(back->gaika_in == 3);
  CHECK(back->last_fight_time == 12345);
  CHECK(back->last_attacker == 77);
  CHECK(back->members == table.find(key)->members);

  // Truncation at every length is refused rather than read past.
  for (std::size_t cut = 0; cut < bytes_out.size(); ++cut) {
    SquadTable partial;
    CHECK(!partial.deserialize(std::span(bytes_out).first(cut)).ok());
  }

  // A section written by the previous shape is refused. The literal 6 is the
  // point: if the section version had not moved for `last_attacker`, the bytes
  // above would already carry it and this patch would be a no-op.
  std::vector<std::byte> older = bytes_out;
  REQUIRE(older.size() > 8);
  older[4] = std::byte{6};
  older[5] = std::byte{0};
  older[6] = std::byte{0};
  older[7] = std::byte{0};
  SquadTable refused;
  CHECK(!refused.deserialize(older).ok());
}

/// **A squad that goes takes its AI order with it** (0x00444803): whether its
/// last member leaves, it is destroyed outright, or it is pruned having never
/// had one, its record goes back on the free chain -- otherwise the next squad
/// to be given its index would inherit an order nobody gave it.
TEST(a_squad_that_goes_frees_its_ai_order) {
  SquadTable table;
  const SquadKey left = table.create(2);
  const SquadKey destroyed = table.create(2);
  const SquadKey hollow = table.create(2);
  REQUIRE(table.join(left, 21));
  REQUIRE(table.join(destroyed, 22));
  REQUIRE(table.post_order(left, 4, 9));
  REQUIRE(table.post_order(destroyed, 4, 9));
  REQUIRE(table.post_order(hollow, 4, 9));
  const AiOrderQueue& queue = *table.orders(2);
  REQUIRE(queue.todo.size() == 3);

  REQUIRE(table.leave(left, 21));
  CHECK(table.find(left)->order == -1);
  CHECK(table.find(left)->order_dest == kNoGaika);
  CHECK(queue.todo[0].verb == 0);
  CHECK(queue.first_free == 0);

  REQUIRE(table.destroy(destroyed));
  CHECK(queue.todo[1].verb == 0);
  CHECK(queue.first_free == 1);
  CHECK(queue.todo[1].next_free == 0);

  table.prune_empty();
  CHECK(queue.todo[2].verb == 0);
  CHECK(queue.first_free == 2);

  // And the index a new squad is given carries nothing.
  const SquadKey reborn = table.create(2);
  CHECK(reborn == left);
  CHECK(table.find(reborn)->order == -1);
}

/// **The AI order queues ride in the squad section, and in its hash** -- every
/// slot of every player's queue, free ones with the priority they kept, the
/// free chain, the timer and the runner -- and a squad's index into its queue
/// comes back with it. A squad whose index names a record that is not its own
/// is refused rather than loaded.
TEST(save_squad_section_carries_the_ai_order_queues_and_hashes_them) {
  SquadTable table;
  const SquadKey a = table.create(1);
  const SquadKey b = table.create(1);
  const SquadKey c = table.create(3);
  REQUIRE(table.join(a, 11));
  REQUIRE(table.join(b, 12));
  REQUIRE(table.join(c, 13));
  REQUIRE(table.post_order(a, 5, 100));
  REQUIRE(table.post_order(b, 6, 7));
  REQUIRE(table.post_order(c, 8, 1));
  REQUIRE(table.delete_order(a));  // slot 0 of player 1 is free, priority kept
  AiOrderQueue* ones = table.mutable_orders(1);
  REQUIRE(ones != nullptr);
  CHECK(ones->first_free == 0);
  CHECK(ones->todo[0].priority == 100);
  ones->due = 1500;
  ones->runner = 42;

  std::uint64_t before = 0;
  table.hash(before);
  const auto hash_of = [&] {
    std::uint64_t h = 0;
    table.hash(h);
    return h;
  };
  ones->due = 2000;
  CHECK(hash_of() != before);
  ones->due = 1500;
  table.mutable_orders(3)->todo[0].priority = 2;
  CHECK(hash_of() != before);
  table.mutable_orders(3)->todo[0].priority = 1;
  ones->todo[0].priority = 99;  // a free slot's, too
  CHECK(hash_of() != before);
  ones->todo[0].priority = 100;
  REQUIRE(hash_of() == before);

  std::vector<std::byte> bytes_out;
  table.serialize(bytes_out);
  SquadTable loaded;
  REQUIRE(loaded.deserialize(bytes_out).ok());
  for (PlayerId p = 0; p < kPlayerCount; ++p) CHECK(*loaded.orders(p) == *table.orders(p));
  CHECK(loaded.find(a)->order == -1);
  CHECK(loaded.find(b)->order == 1);
  CHECK(loaded.find(b)->order_dest == 6);
  CHECK(loaded.find(c)->order == 0);
  std::uint64_t reloaded = 0;
  loaded.hash(reloaded);
  CHECK(reloaded == before);

  for (std::size_t cut = 0; cut < bytes_out.size(); ++cut) {
    SquadTable partial;
    CHECK(!partial.deserialize(std::span(bytes_out).first(cut)).ok());
  }

  // An index that names another squad's record.
  for (Squad& squad : table.mutable_squads()) {
    if (squad.key == b) squad.order = 0;
  }
  std::vector<std::byte> crossed;
  table.serialize(crossed);
  SquadTable refused;
  CHECK(!refused.deserialize(crossed).ok());
}

// --------------------------------------------------------------------------
// AIDest
// --------------------------------------------------------------------------

/// The pair `SQUADMONITOR.VS` reads together, and the stale handle it expects.
///
/// `if (sq.LastFightTime > GetTime - 3000) if (sq.GetLastAttacker.IsValid)` --
/// the script tests validity itself, which is what says the stored handle is
/// **not** re-checked on the way out. A squad that has never fought answers 0
/// and an invalid object, which is what makes the first comparison false
/// without a special case.
TEST(a_squad_reports_the_blow_it_last_took_and_who_landed_it) {
  Fixture f;
  HostRegistry registry;
  imperivm::core::script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 3);
  Squad* squad = f.heroes.squads().find(key);
  REQUIRE(squad != nullptr);

  const auto when = [&] {
    HostCall call(f.world, {pack_squad(key)});
    const HostOutcome out = invoke(registry, CallKind::member, "LastFightTime", 0, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.as_integer();
  };
  const auto who = [&] {
    HostCall call(f.world, {pack_squad(key)});
    const HostOutcome out = invoke(registry, CallKind::member, "GetLastAttacker", 0, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_object() ? out.value.as_object().id : kNoObject;
  };

  // Never fought.
  CHECK(when() == 0);
  CHECK(who() == kNoObject);

  const ObjectId tower = f.world.spawn(NativeClass::building, nullptr);
  squad->last_fight_time = 42000;
  squad->last_attacker = tower;
  CHECK(when() == 42000);
  CHECK(who() == tower);

  // The handle survives its object, which is the case the script's own
  // `IsValid` is there for.
  REQUIRE(f.world.despawn(tower));
  CHECK(who() == tower);

  // A handle that names no squad answers the same "never fought" pair rather
  // than refusing -- `State`'s rule, and the one every reader here follows.
  HostCall missing(f.world, {pack_squad(SquadKey{99, 7})});
  const HostOutcome no_time =
      invoke(registry, CallKind::member, "LastFightTime", 0, missing);
  CHECK(no_time.status == HostStatus::ok);
  CHECK(no_time.value.as_integer() == 0);
  HostCall missing2(f.world, {pack_squad(SquadKey{99, 7})});
  const HostOutcome no_one =
      invoke(registry, CallKind::member, "GetLastAttacker", 0, missing2);
  CHECK(no_one.status == HostStatus::ok);
  CHECK(no_one.value.as_object().id == kNoObject);
}

/// `Squad::Units` -- the other half of `units/0`, and the half that copies.
///
/// `gbr.exe` 0x0043ae40 allocates 0x2c bytes at 0x0043ae71 and copy-constructs
/// from `squad + 0x38` through 0x00438330, a plain forward range copy. So the
/// snapshot is join order, front to back, and `[0]` is the squad's own `[0]`.
/// `GS_CAPTURE.VS` proves it from the script side at lines 224-226: it takes
/// `ol = squad.Units`, calls `Siege`, and subtracts the *new* count from
/// `ol.count`, which is identically zero and dead code unless the first is
/// frozen.
///
/// The contrast is the point of the test. `Settlement::Units` aliases; this
/// one does not, and one body serves both.
TEST(a_squads_unit_list_is_a_snapshot_in_join_order) {
  Fixture f;
  HostRegistry registry;
  imperivm::core::script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);
  register_objlist_host(registry);

  EconomySystem economy;
  REQUIRE(f.world.add_system(&economy));

  const SquadKey key = f.squad_of(1, 3);
  const std::vector<ObjectId> before = f.heroes.squads().find(key)->members;
  REQUIRE(before.size() == 3);

  HostCall take(f.world, {pack_squad(key)});
  const HostOutcome out = invoke(registry, CallKind::member, "Units", 0, take);
  REQUIRE(out.status == HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  const ObjListId list = objlist_of(out.value);
  // Join order, element for element.
  const std::span<const ObjectId> got = objlist_pool_of(f.world).items(list);
  REQUIRE(got.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) CHECK(got[i] == before[i]);
  // It owns its storage; only a settlement's roster aliases.
  CHECK(objlist_pool_of(f.world).alias_of(list) == kNoObject);

  // Frozen: the squad changes and the snapshot does not. This is the assertion
  // `GS_CAPTURE.VS` depends on.
  (void)f.heroes.squads().join(key, f.spawn());
  CHECK(f.heroes.squads().find(key)->members.size() == 4);
  CHECK(objlist_pool_of(f.world).items(list).size() == 3);

  // A handle naming no squad is refused by name. The original has no defined
  // behaviour here at all -- 0x00443e30 bounds-checks neither the 12-bit index
  // nor the pointer it returns, and the caller copy-constructs from whatever
  // came back -- so there is nothing to reproduce, and silence would be worse
  // than a trap.
  HostCall stale(f.world, {pack_squad(SquadKey{77, 4})});
  CHECK(invoke(registry, CallKind::member, "Units", 0, stale).status == HostStatus::error);
}

TEST(ai_dest_reads_stored_state_and_answers_zero_when_there_is_none) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 3);
  HostCall none(f.world, {pack_squad(key)});
  const HostOutcome fresh = invoke(registry, CallKind::member, "AIDest", 0, none);
  CHECK(fresh.status == HostStatus::ok);
  CHECK(fresh.value.as_integer() == 0);

  f.heroes.squads().find(key)->ai_dest = 12;
  HostCall set(f.world, {pack_squad(key)});
  CHECK(invoke(registry, CallKind::member, "AIDest", 0, set).value.as_integer() == 12);

  // **The invalid handle names no squad; it is not a wrong type.** It is what
  // `Unit::GetSquad` answers for a unit in no squad -- `pack_squad(kNoSquad)`
  // is `(kNoType, 0)` on purpose, so `.IsValid` reads it -- and
  // `HERO_AI_KILLALL.VS` line 21 asks `.GetSquad.AIDest == .GetSquad.GAIKAIn`
  // of a hero that may have none. Six trap hits over the shipped maps said
  // this answered "receiver is not a squad handle" instead of zero. A value
  // of some other type is still refused, which is the other half of the rule.
  HostCall none_at_all(f.world, {pack_squad(kNoSquad)});
  const HostOutcome squadless = invoke(registry, CallKind::member, "AIDest", 0, none_at_all);
  CHECK(squadless.status == HostStatus::ok);
  CHECK(squadless.value.as_integer() == 0);
  HostCall squadless_in(f.world, {pack_squad(kNoSquad)});
  CHECK(invoke(registry, CallKind::member, "GAIKAIn", 0, squadless_in).value.as_integer() == 0);
  HostCall squadless_take(f.world, {pack_squad(kNoSquad), Value::integer(500)});
  const HostOutcome took =
      invoke(registry, CallKind::member, "TakeNearbyItems", 1, squadless_take);
  CHECK(took.status == HostStatus::ok);
  CHECK(!took.value.truthy_scalar());
  HostCall wrong_type(f.world, {obj(1)});
  CHECK(invoke(registry, CallKind::member, "AIDest", 0, wrong_type).status == HostStatus::error);
}

/// The three siblings of `AIDest`, and the one pair the corpus compares.
TEST(the_four_gaika_fields_on_a_squad_are_four_different_answers) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 2);
  Squad* squad = f.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  squad->src_gaika = 3;
  squad->dest_gaika = 5;
  squad->order_dest = 7;
  squad->ai_dest = 9;

  const auto read = [&](const char* name) {
    HostCall call(f.world, {pack_squad(key)});
    const HostOutcome out = invoke(registry, CallKind::member, name, 0, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.as_integer();
  };
  // Four adjacent `uint16`s on one record, and four entry points one
  // instruction apart. Distinct values because a body reading the wrong
  // neighbour is exactly the mistake available here.
  CHECK(read("SrcGAIKA") == 3);
  CHECK(read("DestGAIKA") == 5);
  CHECK(read("OrderDest") == 7);
  CHECK(read("AIDest") == 9);

  // `OrderDest` and `AIDest` are the two ends of one decision -- what the squad
  // was last *ordered* to, against what its AI is working toward -- and
  // `GS_SIEGE.VS` compares them to decide whether to re-order. That is only a
  // decision if they can differ, which is what the two values above assert.
  CHECK(read("OrderDest") != read("AIDest"));

  // A handle naming no squad is `kNoGaika` for all three, as for `AIDest`.
  HostCall stale(f.world, {pack_squad(SquadKey{77, 4})});
  CHECK(invoke(registry, CallKind::member, "SrcGAIKA", 0, stale).value.as_integer() == 0);
  HostCall stale2(f.world, {pack_squad(SquadKey{77, 4})});
  CHECK(invoke(registry, CallKind::member, "OrderDest", 0, stale2).status == HostStatus::ok);
}

/// `sq.Count(class)` -- a subset count, and the only computed one in the group.
TEST(squad_count_is_a_subset_of_the_membership_by_class_tree) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 2);  // two plain `Unit`s
  const ObjectId horse =
      f.world.spawn(NativeClass::unit, nullptr, f.graph.find("Horse"));
  f.world.set_owner(horse, 1);
  f.world.set_health(horse, 100);
  REQUIRE(f.heroes.squads().join(key, horse));

  const auto count = [&](const char* klass) {
    HostCall call(f.world, {pack_squad(key), Value::string(klass)});
    const HostOutcome out = invoke(registry, CallKind::member, "Count", 1, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.as_integer();
  };

  // `SQUADMONITOR.VS`'s `sq.Count("Horse") != sq.Size` is the shape that makes
  // this a subset count rather than anything else.
  CHECK(count("Horse") == 1);
  CHECK(static_cast<std::size_t>(count("Horse")) != f.heroes.squads().find(key)->size());
  // The tree, not the exact class: a `Horse` is a `Unit`.
  CHECK(count("Unit") == 3);
  // A class in the graph that nobody here is.
  CHECK(count("GDruid") == 0);
  // **A name the graph cannot resolve counts nothing, never everything** --
  // the rule `ClassFilter::parse` keeps and the one a hand-rolled matcher
  // would get wrong.
  CHECK(count("NoSuchClass") == 0);
  CHECK(count("") == 0);

  // A handle naming no squad is 0 rather than a trap, as elsewhere on the type.
  HostCall stale(f.world, {pack_squad(SquadKey{77, 4}), Value::string("Unit")});
  const HostOutcome gone = invoke(registry, CallKind::member, "Count", 1, stale);
  CHECK(gone.status == HostStatus::ok);
  CHECK(gone.value.as_integer() == 0);
}

/// `sq.Eval` reads a field nothing writes, and says so.
TEST(squad_eval_reads_stored_strength_and_is_zero_until_something_writes_it) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 3);
  HostCall fresh(f.world, {pack_squad(key)});
  const HostOutcome none = invoke(registry, CallKind::member, "Eval", 0, fresh);
  CHECK(none.status == HostStatus::ok);
  // **Zero, and not the member count.** Three units in the squad, and the
  // answer is still 0, because `Eval` is a stored strength rather than a size
  // -- `SQUADMONITOR.VS` reads both and they are different questions.
  CHECK(none.value.as_integer() == 0);
  CHECK(f.heroes.squads().find(key)->size() == 3);

  // It is a read of state, so a writer -- a loader, a test, or the strength
  // model when it lands -- is visible through it immediately.
  f.heroes.squads().find(key)->eval = 240;
  HostCall set(f.world, {pack_squad(key)});
  CHECK(invoke(registry, CallKind::member, "Eval", 0, set).value.as_integer() == 240);

  HostCall stale(f.world, {pack_squad(SquadKey{77, 4})});
  CHECK(invoke(registry, CallKind::member, "Eval", 0, stale).value.as_integer() == 0);
}

TEST(ai_dest_on_a_handle_naming_no_squad_is_no_gaika_rather_than_a_trap) {
  // `SQUADMONITOR.VS` guards on `if (sq.AIDest > 0)`, so "no destination" has
  // to be an answer the script can get. A squad destroyed between two AI passes
  // is exactly that case.
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);
  HostCall stale(f.world, {pack_squad(SquadKey{77, 4})});
  const HostOutcome outcome = invoke(registry, CallKind::member, "AIDest", 0, stale);
  CHECK(outcome.status == HostStatus::ok);
  CHECK(outcome.value.as_integer() == 0);

  // A receiver that is not a squad at all is still a trap: it is a script bug,
  // not a game state.
  HostCall wrong(f.world, {obj(3)});
  CHECK(invoke(registry, CallKind::member, "AIDest", 0, wrong).status == HostStatus::error);
}

TEST(test_flags_wants_every_bit_it_was_asked_for_and_not_merely_one) {
  // 0x00421762: `flags & mask`, `cmpw` against the mask, `sete`. **All**, not
  // any -- and its neighbour `Unit::GetFlags(mask)` (0x005d7e75) is `testl` +
  // `setne`, which *is* any. One instruction apart, opposite results, and the
  // corpus reaches both readings: `TestFlags(SF_PEACEFUL)` (10 sites, one bit,
  // indistinguishable) and `TestFlags(SF_PEACEFUL + SF_NOAI)` (2 sites, two
  // bits, where the two answers differ whenever exactly one bit is set).
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  constexpr std::int32_t kNoAi = 1;      // SF_NOAI
  constexpr std::int32_t kPeaceful = 4;  // SF_PEACEFUL
  const SquadKey key = f.squad_of(1, 3);
  const auto test = [&](std::int32_t mask) {
    HostCall call(f.world, {pack_squad(key), Value::integer(mask)});
    const HostOutcome out = invoke(registry, CallKind::member, "TestFlags", 1, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.as_integer() != 0;
  };

  CHECK(!test(kNoAi));
  f.heroes.squads().find(key)->flags = static_cast<std::uint16_t>(kNoAi);
  CHECK(test(kNoAi));
  CHECK(!test(kPeaceful));

  // The discriminating case, and the shipped one: one of the two bits set.
  // "any" would answer true here and "all" answers false.
  CHECK(!test(kNoAi + kPeaceful));
  f.heroes.squads().find(key)->flags = static_cast<std::uint16_t>(kNoAi | kPeaceful);
  CHECK(test(kNoAi + kPeaceful));
  CHECK(test(kNoAi));
  CHECK(test(kPeaceful));

  // A mask of zero is vacuously true under "all" and vacuously false under
  // "any". No shipped site passes it; pinned so the two readings stay apart.
  CHECK(test(0));

  // A handle naming no squad answers false rather than trapping -- every
  // shipped site is a guard with a written false branch -- while a receiver
  // that is not a squad at all is still a script bug.
  HostCall stale(f.world, {pack_squad(SquadKey{77, 4}), Value::integer(kNoAi)});
  const HostOutcome gone = invoke(registry, CallKind::member, "TestFlags", 1, stale);
  CHECK(gone.status == HostStatus::ok);
  CHECK(gone.value.as_integer() == 0);
  HostCall wrong(f.world, {obj(3), Value::integer(kNoAi)});
  CHECK(invoke(registry, CallKind::member, "TestFlags", 1, wrong).status == HostStatus::error);
}

// --------------------------------------------------------------------------
// SetCmd
// --------------------------------------------------------------------------

TEST(set_cmd_sets_the_state_and_masks_the_flags) {
  // `squad.SetCmd(SS_KillAll, 0, SF_ADVCHOOSER, "ai_killall")` -- the shape 27
  // of the 30 corpus sites use: set nothing, clear one flag.
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 2);
  Squad* squad = f.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  squad->flags = 0x0007;
  f.world.advance(2500);

  HostCall call(f.world, {pack_squad(key), Value::integer(6), Value::integer(0x0010),
                          Value::integer(0x0001), Value::string("ai_killall")});
  CHECK(invoke(registry, CallKind::member, "SetCmd", 4, call).status == HostStatus::ok);

  CHECK(squad->state == 6);
  // `(flags | set) & ~clear`.
  CHECK(squad->flags == 0x0016);
  CHECK(squad->state_time == f.world.time());
}

/// **The two masks are applied set-then-clear, so a bit in both ends clear.**
///
/// `(flags | set) & ~clear` and `(flags & ~clear) | set` agree on every input
/// except one -- a bit named by both masks -- and no shipped site names a bit
/// twice: 27 of the 30 `SetCmd` sites pass a set mask of literally `0`. So the
/// corpus cannot settle this and the order is a **stated choice**, pinned here
/// rather than left to whichever way the expression happened to be written.
///
/// The argument names are what it rests on: `gbr.exe`'s signature table calls
/// them `nSetFlags` and `nClrFlags` in that order, and both the entry point
/// names read the same way round -- `SetCmd` and `ClrCmd`, with the clearing
/// one named for what it does last.
TEST(set_cmd_and_clr_cmd_let_the_clear_mask_win_a_bit_named_by_both) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  for (const char* name : {"SetCmd", "ClrCmd"}) {
    const SquadKey key = f.squad_of(1, 1);
    Squad* squad = f.heroes.squads().find(key);
    REQUIRE(squad != nullptr);
    squad->flags = 0x0000;

    // `SF_ADVCHOOSER` (2) is in both masks, and `SF_NOAI` (1) in the set mask
    // alone, so one bit distinguishes the orders and the other proves the set
    // mask is applied at all.
    std::vector<Value> args{pack_squad(key), Value::integer(1), Value::integer(0x0003),
                            Value::integer(0x0002)};
    std::uint16_t arity = 3;
    if (std::string_view(name) == "SetCmd") {
      args.push_back(Value::string("ai_killall"));
      arity = 4;
    }
    HostCall call(f.world, {});
    call.arguments = args;
    call.context.arguments = call.arguments;
    CHECK(invoke(registry, CallKind::member, name, arity, call).status == HostStatus::ok);

    squad = f.heroes.squads().find(key);
    CHECK(squad->flags == 0x0001);       // set-then-clear
    CHECK(squad->flags != 0x0003);       // clear-then-set, which is the other order
  }
}

TEST(set_cmd_orders_every_member_not_just_the_leader) {
  // `SQUADMONITOR.VS` line 324: `sq.SetCmd(SS_IDLE, 0, SF_ADVCHOOSER, "move",
  // sqLeader.pos)`. Ordering the squad to its own leader's position is a no-op
  // unless the order reaches the members.
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 4);
  const Squad* squad = f.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  REQUIRE(squad->members.size() == 4);

  HostCall call(f.world, {pack_squad(key), Value::integer(1), Value::integer(0),
                          Value::integer(0x0001), Value::string("move"),
                          pack_point(Point{640, 480})});
  CHECK(invoke(registry, CallKind::member, "SetCmd", 5, call).status == HostStatus::ok);

  for (const ObjectId member : squad->members) {
    CHECK(f.commands.command_name(member) == "move");
    const CommandQueue* queue = f.commands.find(member);
    REQUIRE(queue != nullptr);
    REQUIRE(queue->running() != nullptr);
    CHECK(queue->running()->arg_kind == CommandArgKind::point);
    CHECK(queue->running()->point == (Point{640, 480}));
  }
}

TEST(set_cmd_takes_an_object_argument_as_well_as_a_point) {
  // `squad.SetCmd(SS_Capture, 0, SF_ADVCHOOSER, "capture",
  // set.GetCentralBuilding)` -- `GS_CAPTURE.VS` line 194. `gbr.exe` declares
  // the two argument shapes under separate names, `Squad::SetCmd` and
  // `Squad::SetCmdObj`, which is why both have to work at one arity.
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(2, 2);
  const ObjectId target = f.spawn(Point{900, 900}, 3);

  HostCall call(f.world, {pack_squad(key), Value::integer(2), Value::integer(0),
                          Value::integer(0x0001), Value::string("capture"), obj(target)});
  CHECK(invoke(registry, CallKind::member, "SetCmd", 5, call).status == HostStatus::ok);

  for (const ObjectId member : f.heroes.squads().find(key)->members) {
    const CommandQueue* queue = f.commands.find(member);
    REQUIRE(queue != nullptr);
    REQUIRE(queue->running() != nullptr);
    CHECK(queue->running()->verb == "capture");
    CHECK(queue->running()->arg_kind == CommandArgKind::object);
    CHECK(queue->running()->object == target);
  }
}

TEST(set_cmd_replaces_the_queue_rather_than_appending_to_it) {
  // The `Set`/`Clr` pair and `SetCommand`'s own behaviour (sim/command.hpp).
  // `SQUADMONITOR.VS` goes through `sq.Units.AddCommand(false, ...)` when it
  // wants an append, which is the other spelling and the reason this one is
  // not it.
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 2);
  const Squad* squad = f.heroes.squads().find(key);
  for (const ObjectId member : squad->members) {
    Command prototype;
    (void)f.commands.set_command(f.world, member, "engage", prototype);
    (void)f.commands.add_command(f.world, member, false, "advance", prototype);
    CHECK(f.commands.command_count(member) == 2);
  }

  HostCall call(f.world, {pack_squad(key), Value::integer(1), Value::integer(0),
                          Value::integer(0), Value::string("enter"), obj(f.spawn())});
  CHECK(invoke(registry, CallKind::member, "SetCmd", 5, call).status == HostStatus::ok);

  for (const ObjectId member : squad->members) {
    CHECK(f.commands.command_name(member) == "enter");
    CHECK(f.commands.command_count(member) == 1);
  }
}

TEST(set_cmd_on_an_empty_squad_changes_state_and_orders_nobody) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.heroes.squads().create(1);
  HostCall call(f.world, {pack_squad(key), Value::integer(9), Value::integer(0x0002),
                          Value::integer(0), Value::string("ai_killall")});
  CHECK(invoke(registry, CallKind::member, "SetCmd", 4, call).status == HostStatus::ok);
  CHECK(f.heroes.squads().find(key)->state == 9);
  CHECK(f.heroes.squads().find(key)->flags == 0x0002);
  CHECK(f.commands.tracked() == 0);
}

TEST(set_cmd_refuses_a_receiver_that_names_no_squad) {
  // Unlike `AIDest`, this one mutates: silently ordering nothing would be a
  // divergence rather than an answer.
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);
  HostCall stale(f.world, {pack_squad(SquadKey{31, 5}), Value::integer(1), Value::integer(0),
                           Value::integer(0), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "SetCmd", 4, stale).status == HostStatus::error);

  const SquadKey key = f.squad_of(1, 1);
  HostCall bad_verb(f.world, {pack_squad(key), Value::integer(1), Value::integer(0),
                              Value::integer(0), Value::integer(7)});
  CHECK(invoke(registry, CallKind::member, "SetCmd", 4, bad_verb).status == HostStatus::error);
}

// --------------------------------------------------------------------------
// registration
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// the readers
// --------------------------------------------------------------------------

/// `State` is the most-read member on the type, and a receiver naming no squad
/// answers `SS_IDLE` rather than trapping.
TEST(squad_state_reads_stored_state_and_idles_for_an_unknown_squad) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 2);
  f.heroes.squads().find(key)->state = 7;

  HostCall call(f.world, {pack_squad(key)});
  CHECK(invoke(registry, CallKind::member, "State", 0, call).value.as_integer() == 7);

  HostCall missing(f.world, {pack_squad(SquadKey{99, 3})});
  const HostOutcome out = invoke(registry, CallKind::member, "State", 0, missing);
  CHECK(out.status == HostStatus::ok);
  CHECK(out.value.as_integer() == 0);
}

/// **`StateTime` is elapsed time, not a timestamp**, which is the reading
/// `sim/squad.hpp` used to deny.
///
/// 0x00421400 returns `now - [squad+0x2c]`. `EVALRECRUIT.VS` divides the answer
/// by 1000 to get seconds and compares it against `1000 * AIVar(...)`; a
/// timestamp would be neither. A squad whose state was set at time 0 and read
/// at time 4,000 answers 4,000, and the stamp itself is 0 -- so the two
/// readings differ by the whole of the clock and this test tells them apart.
TEST(squad_state_time_is_elapsed_and_not_the_stamp) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 1);
  Squad* squad = f.heroes.squads().find(key);
  squad->state = 3;
  squad->state_time = 1000;
  f.world.advance_turns(10);  // 400 ms a turn: now is 4,000

  HostCall call(f.world, {pack_squad(key)});
  const std::int32_t answer =
      invoke(registry, CallKind::member, "StateTime", 0, call).value.as_integer();
  CHECK(answer == static_cast<std::int32_t>(f.world.time()) - 1000);
  CHECK(answer != 1000);  // the stamp, which is what the wrong reading returns
  CHECK(answer > 0);

  // A stamp in the future -- which a save from a longer session could carry --
  // is clamped to zero rather than answering a negative age.
  squad->state_time = f.world.time() + 5000;
  HostCall later(f.world, {pack_squad(key)});
  CHECK(invoke(registry, CallKind::member, "StateTime", 0, later).value.as_integer() == 0);
}

/// `Leader` is `members[0]`, so it cannot disagree with the membership.
TEST(squad_leader_is_the_first_member_or_an_invalid_object) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const ObjectId hero = f.spawn();
  const SquadKey key = f.heroes.squads().create(1, hero);
  (void)f.heroes.squads().join(key, f.spawn());

  HostCall call(f.world, {pack_squad(key)});
  const HostOutcome out = invoke(registry, CallKind::member, "Leader", 0, call);
  CHECK(out.value.is_object());
  CHECK(out.value.as_object().id == hero);

  // A squad built by `join` alone has no `leader` field and **still has a
  // Leader**: the front of its deque, which is what 0x00421310 answers and
  // what `SQUADMONITOR.VS` needs of every AI squad it walks. This used to
  // assert an invalid object here.
  const SquadKey headless = f.squad_of(2, 3);
  HostCall front(f.world, {pack_squad(headless)});
  const HostOutcome first = invoke(registry, CallKind::member, "Leader", 0, front);
  CHECK(first.status == HostStatus::ok);
  CHECK(first.value.is_object() && first.value.as_object().type == kTypeObj);
  CHECK(first.value.as_object().id == f.heroes.squads().find(headless)->members.front());

  // And only an empty squad answers an invalid object.
  const SquadKey hollow = f.heroes.squads().create(2);
  HostCall none(f.world, {pack_squad(hollow)});
  const HostOutcome empty = invoke(registry, CallKind::member, "Leader", 0, none);
  CHECK(empty.status == HostStatus::ok);
  CHECK(empty.value.as_object().type == script::kNoType);
}

/// `GAIKAIn` is a read of stored state and answers zero on a squad no loader
/// has filled, which is the case every shipped guard is written for.
TEST(squad_gaika_in_reads_stored_state) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 1);
  HostCall fresh(f.world, {pack_squad(key)});
  CHECK(invoke(registry, CallKind::member, "GAIKAIn", 0, fresh).value.as_integer() == kNoGaika);

  f.heroes.squads().find(key)->gaika_in = 12;
  HostCall filled(f.world, {pack_squad(key)});
  CHECK(invoke(registry, CallKind::member, "GAIKAIn", 0, filled).value.as_integer() == 12);
}

/// `ClrCmd` is `SetCmd` without a new command: state, clock, flags -- and
/// **every member's command ended** (0x004271d0 calls `vtbl+0xc0(1)` on each,
/// the call `IdleAllGates` makes on a gate). That is what stops a squad
/// `AIOSENDSQUAD.VS` sends to the node it already stands in, and a town's own
/// sentries are such a squad: left walking, they never went back to `idle`
/// for `WALL_PATROL.VS` to re-post.
TEST(squad_clr_cmd_sets_state_and_flags_and_ends_every_members_command) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 2);
  Squad* squad = f.heroes.squads().find(key);
  squad->flags = 0x0006;  // SF_ADVCHOOSER | SF_PEACEFUL
  squad->state_time = 0;
  const std::vector<ObjectId> members = squad->members;
  Command there;
  there.arg_kind = CommandArgKind::point;
  there.point = Point{900, 900};
  for (const ObjectId member : members) {
    (void)f.commands.set_command(f.world, member, "advance", there);
    (void)f.commands.add_command(f.world, member, false, "guard", Command{});
    REQUIRE(f.commands.command_count(member) == 2);
  }
  f.world.advance_turns(2);

  HostCall call(f.world, {pack_squad(key), Value::integer(9), Value::integer(0x0010),
                          Value::integer(0x0004)});
  CHECK(invoke(registry, CallKind::member, "ClrCmd", 3, call).status == HostStatus::ok);

  squad = f.heroes.squads().find(key);
  CHECK(squad->state == 9);
  CHECK(squad->flags == 0x0012);  // SF_PEACEFUL cleared, SF_WANTDRUIDS set, no lock left
  CHECK(squad->state_time == f.world.time());
  // The runner ended and the pending tail dropped: nobody is walking anywhere.
  for (const ObjectId member : members) {
    CHECK(f.commands.command_name(member) != "advance");
    CHECK(f.commands.command_name(member, 1) != "guard");
    CHECK(f.commands.command_count(member) <= 1);
  }
}

/// **A squad carrying `SF_NOAI` is refused whole** (`[squad+0x30] & 1` is the
/// first test after the squad resolves): no state, no clock, no flags, and its
/// members keep what a mission gave them.
TEST(squad_clr_cmd_refuses_a_squad_no_ai_may_touch) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 2);
  Squad* squad = f.heroes.squads().find(key);
  squad->flags = 0x0005;  // SF_NOAI | SF_PEACEFUL
  squad->state = 3;
  squad->state_time = 0;
  const std::vector<ObjectId> members = squad->members;
  for (const ObjectId member : members) {
    (void)f.commands.set_command(f.world, member, "guard", Command{});
  }
  f.world.advance_turns(2);

  HostCall call(f.world, {pack_squad(key), Value::integer(9), Value::integer(0x0010),
                          Value::integer(0x0004)});
  CHECK(invoke(registry, CallKind::member, "ClrCmd", 3, call).status == HostStatus::ok);

  squad = f.heroes.squads().find(key);
  CHECK(squad->state == 3);
  CHECK(squad->flags == 0x0005);
  CHECK(squad->state_time == 0);
  for (const ObjectId member : members) CHECK(f.commands.command_name(member) == "guard");
}

/// `sq.EvalAttach(leader, min)` -- four refusals, then a standing against
/// `min` times a level factor.
TEST(eval_attach_scores_a_heros_squad_by_its_standing_against_min_and_its_level) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  // The asking squad: three bodies, bound for node 5.
  const SquadKey ours = f.squad_of(1, 3);
  f.heroes.squads().find(ours)->ai_dest = 5;
  // A hero of level 23 with four warriors, bound the same way.
  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.unit_class);
  f.world.set_owner(hero, 1);
  f.world.set_health(hero, 200);
  f.heroes.register_hero(f.world, hero);
  CHECK(f.heroes.set_level(hero, 23));
  for (int i = 0; i < 4; ++i) REQUIRE(f.heroes.attach(f.world, f.spawn(), hero));
  const SquadKey army = f.heroes.hero(hero)->squad;
  REQUIRE(f.heroes.squads().find(army)->size() == 5);
  f.heroes.squads().find(army)->ai_dest = 5;

  const auto eval = [&](ObjectId leader, std::int32_t min) {
    HostCall call(f.world, {pack_squad(ours), obj(leader), Value::integer(min)});
    const HostOutcome out = invoke(registry, CallKind::member, "EvalAttach", 2, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  // Five bodies against a floor of 5: standing 1; level 23 is a factor of 3.
  CHECK(eval(hero, 5) == 3);
  // Against a floor of 2: three over, standing 4.
  CHECK(eval(hero, 2) == 12);
  // Short of a floor of 8 by three: 100 less the shortfall, 97.
  CHECK(eval(hero, 8) == 97 * 3);
  // The level factor truncates: 29 is still 3, 30 is 4, 9 is 1.
  CHECK(f.heroes.set_level(hero, 29));
  CHECK(eval(hero, 5) == 3);
  CHECK(f.heroes.set_level(hero, 30));
  CHECK(eval(hero, 5) == 4);
  CHECK(f.heroes.set_level(hero, 9));
  CHECK(eval(hero, 5) == 1);

  // **Bound elsewhere is 0**, and `AIDest` falls back to `DestGAIKA` on
  // either side.
  f.heroes.squads().find(army)->ai_dest = 6;
  CHECK(eval(hero, 5) == 0);
  f.heroes.squads().find(army)->ai_dest = kNoGaika;
  f.heroes.squads().find(army)->dest_gaika = 5;
  CHECK(eval(hero, 5) == 1);
  f.heroes.squads().find(ours)->ai_dest = kNoGaika;
  CHECK(eval(hero, 5) == 0);
  f.heroes.squads().find(ours)->dest_gaika = 5;
  CHECK(eval(hero, 5) == 1);

  // **A squad of more than fifty is 0**; fifty exactly is not.
  for (int i = 0; i < 45; ++i) REQUIRE(f.heroes.attach(f.world, f.spawn(), hero));
  REQUIRE(f.heroes.squads().find(army)->size() == 50);
  CHECK(eval(hero, 5) == 46);
  REQUIRE(f.heroes.attach(f.world, f.spawn(), hero));
  CHECK(eval(hero, 5) == 0);

  // **Not a hero is 0**: a plain unit leading a squad of the same shape.
  const SquadKey band = f.squad_of(1, 5);
  f.heroes.squads().find(band)->ai_dest = 5;
  CHECK(eval(f.heroes.squads().find(band)->members.front(), 5) == 0);
  // A hero in no squad, an object that is not there, and a receiver naming
  // no squad, are 0 too.
  const ObjectId loner = f.world.spawn(NativeClass::hero, nullptr, f.unit_class);
  f.world.set_owner(loner, 1);
  f.world.set_health(loner, 200);
  CHECK(eval(loner, 5) == 0);
  CHECK(eval(kNoObject, 5) == 0);
  {
    HostCall call(f.world, {pack_squad(SquadKey{9, 1}), obj(hero), Value::integer(5)});
    const HostOutcome out = invoke(registry, CallKind::member, "EvalAttach", 2, call);
    CHECK(out.status == HostStatus::ok);
    CHECK(out.value.as_integer() == 0);
  }
}

/// `sq.SendFoodWagon(amount, range)` and `sq.FoodComing`: the nearest own
/// settlement with food to spare within range loads a mule that follows the
/// leader, and the squad reads the cargo of every wagon following one of its
/// members as food on its way.
TEST(send_food_wagon_loads_at_the_nearest_own_store_and_food_coming_counts_it) {
  Fixture f;
  EconomySystem economy;
  REQUIRE(f.world.add_system(&economy));
  HostRegistry registry;
  register_squad_host(registry);
  economy.start(f.world);

  const auto town = [&](Point at, PlayerId owner, std::int32_t food) {
    const ObjectId anchor = f.world.spawn(NativeClass::building, nullptr);
    f.world.set_position(anchor, at);
    f.world.set_owner(anchor, owner);
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.anchor = anchor;
    init.owner = owner;
    init.max_food = 100000;
    init.food = food;
    return economy.create(f.world, init);
  };
  // The squad stands at (100, 100). A store 500 away, one 300 away that is
  // somebody else's, one 200 away with 99 food, and one 1000 away.
  const SettlementId near = town(Point{600, 100}, 1, 1000);
  (void)town(Point{400, 100}, 2, 1000);
  const SettlementId hungry = town(Point{300, 100}, 1, 99);
  const SettlementId far = town(Point{1100, 100}, 1, 1000);
  const SquadKey key = f.squad_of(1, 2);
  const ObjectId leader = f.heroes.squads().find(key)->members.front();

  const auto send = [&](SquadKey sq, std::int32_t amount, std::int32_t range) {
    HostCall call(f.world, {pack_squad(sq), Value::integer(amount), Value::integer(range)});
    const HostOutcome out = invoke(registry, CallKind::member, "SendFoodWagon", 2, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.truthy_scalar();
  };
  const auto coming = [&](SquadKey sq) {
    HostCall call(f.world, {pack_squad(sq)});
    const HostOutcome out = invoke(registry, CallKind::member, "FoodComing", 0, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  CHECK(coming(key) == 0);
  // Out of range of everything own with food: nothing.
  CHECK(!send(key, 300, 400));
  CHECK(economy.wagons().empty());
  // 500 is within 500: the near store loads 300 and the mule follows the
  // leader; the food is out of the store at once and counts as coming.
  CHECK(send(key, 300, 500));
  REQUIRE(economy.wagons().size() == 1);
  CHECK(economy.wagons()[0].follow == leader);
  CHECK(economy.wagons()[0].amount == 300);
  CHECK(economy.wagons()[0].resource == Resource::food);
  CHECK(economy.find(near)->warehouse.food == 700);
  CHECK(coming(key) == 300);

  // 100 food exactly is enough to load at, and the load is clamped to it.
  economy.set_resource(hungry, Resource::food, 100);
  CHECK(send(key, 300, 500));
  REQUIRE(economy.wagons().size() == 2);
  CHECK(economy.wagons()[1].amount == 100);
  CHECK(economy.find(hungry)->warehouse.food == 0);
  CHECK(coming(key) == 400);

  // Nothing to load is false, and a store that cannot pay is not chosen over
  // one that can: with the near store emptied the far one is out of range.
  economy.set_resource(near, Resource::food, 0);
  CHECK(!send(key, 300, 500));
  CHECK(send(key, 50, 1000));
  CHECK(economy.wagons().size() == 3);
  CHECK(economy.find(far)->warehouse.food == 950);
  CHECK(!send(key, 0, 1000));
  CHECK(economy.wagons().size() == 3);

  // Coming is by membership at the time of asking: a wagon after a member who
  // has left no longer counts, and another squad never counted it.
  const SquadKey others = f.squad_of(1, 1);
  CHECK(coming(others) == 0);
  CHECK(f.heroes.squads().leave(key, leader));
  CHECK(coming(key) == 0);
  CHECK(f.heroes.squads().join(others, leader));
  CHECK(coming(others) == 450);

  // An empty squad and a handle naming none answer false and 0.
  const SquadKey hollow = f.heroes.squads().create(1);
  CHECK(!send(hollow, 300, 5000));
  CHECK(!send(SquadKey{9, 1}, 300, 5000));
  CHECK(coming(SquadKey{9, 1}) == 0);
}

/// Seven names `gbr.exe` registers on `Squad` as well as on `Obj`, served by
/// the object bodies through `squad_receiver`: the player off the handle, the
/// front member's position, the sums, the all-of-them predicates.
TEST(the_object_readers_answer_a_squad_the_way_the_squad_bodies_do) {
  Fixture f;
  EconomySystem economy;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(f.world.add_system(&economy));
  REQUIRE(f.world.add_system(&feeder));
  HostRegistry registry;
  (void)register_all_hosts(registry);

  const auto ask = [&](const char* name, Value receiver) {
    HostCall call(f.world, {receiver});
    const HostOutcome out = invoke(registry, CallKind::member, name, 0, call);
    CHECK(out.status == HostStatus::ok);
    return out.value;
  };

  // `Player` is the handle's low nibble plus one, live squad or not.
  CHECK(ask("player", pack_squad(SquadKey{5, 3})).as_integer() == 4);
  CHECK(ask("Player", pack_squad(SquadKey{1, 0})).as_integer() == 1);

  // A squad of two, one fed and one hungry, with a free unit and a bound one.
  const ObjectId first = f.spawn(Point{120, 340}, 2);
  const ObjectId second = f.spawn(Point{900, 900}, 2);
  const SquadKey key = f.heroes.squads().create(2, first);
  REQUIRE(f.heroes.squads().join(key, second));
  f.world.set_health(first, 150);
  f.world.set_health(second, 60);
  FeedingUnit a;
  a.unit = first; a.food = 12; a.max_food = 20; a.feeds = true;
  FeedingUnit b = a;
  b.unit = second; b.food = 5;
  feeder.enrol(a);
  feeder.enrol(b);
  f.heroes.register_unit(f.world, first).has_freedom = true;
  f.heroes.register_unit(f.world, second).has_freedom = false;

  const Value sq = pack_squad(key);
  const Point front_at{120, 340};
  CHECK(unpack_point(ask("pos", sq)) == front_at);
  CHECK(ask("health", sq).as_integer() == 210);
  CHECK(ask("maxhealth", sq).as_integer() == 400);  // 200 from the class, twice
  CHECK(ask("food", sq).as_integer() == 17);
  CHECK(!ask("HasFreedom", sq).truthy_scalar());
  CHECK(!ask("InHolder", sq).truthy_scalar());
  f.heroes.unit(second)->has_freedom = true;
  CHECK(ask("HasFreedom", sq).truthy_scalar());

  // Held: both, and then the predicate; one is not enough.
  const ObjectId holder = f.world.spawn(NativeClass::building, nullptr);
  REQUIRE(f.world.put_in_holder(first, holder));
  CHECK(!ask("InHolder", sq).truthy_scalar());
  REQUIRE(f.world.put_in_holder(second, holder));
  CHECK(ask("InHolder", sq).truthy_scalar());
  // And a held front member's position is its holder's.
  CHECK(unpack_point(ask("pos", sq)) == f.world.resolve_position(holder));

  // A member that is gone adds nothing and does not decide `InHolder`.
  CHECK(f.world.despawn(second));
  CHECK(ask("health", sq).as_integer() == 150);
  CHECK(ask("maxhealth", sq).as_integer() == 200);
  CHECK(ask("InHolder", sq).truthy_scalar());

  // An empty squad: no position, sums of nothing, and both predicates true.
  const SquadKey hollow = f.heroes.squads().create(2);
  const Value none = pack_squad(hollow);
  const Point nowhere{-1, -1};
  CHECK(unpack_point(ask("pos", none)) == nowhere);
  CHECK(ask("health", none).as_integer() == 0);
  CHECK(ask("maxhealth", none).as_integer() == 0);
  CHECK(ask("food", none).as_integer() == 0);
  CHECK(ask("InHolder", none).truthy_scalar());
  CHECK(ask("HasFreedom", none).truthy_scalar());
  // Sentries are free by their class, with no flag on the record: the unit
  // asked alone, and a squad of them.
  const auto sentry = [&](Point at) {
    const ObjectId id = f.world.spawn(NativeClass::unit, nullptr, f.graph.find("Sentry"));
    f.world.set_owner(id, 2);
    f.world.set_position(id, at);
    f.world.set_health(id, 100);
    f.heroes.register_unit(f.world, id);
    return id;
  };
  const ObjectId watch = sentry(Point{400, 400});
  const ObjectId ward = sentry(Point{440, 400});
  CHECK(ask("HasFreedom", Value::object(kTypeObj, watch)).truthy_scalar());
  const SquadKey posts = f.heroes.squads().create(2, watch);
  REQUIRE(f.heroes.squads().join(posts, ward));
  CHECK(ask("HasFreedom", pack_squad(posts)).truthy_scalar());
  // And a handle naming no squad is the object bodies' own "not there".
  const Value stale = pack_squad(SquadKey{9, 2});
  CHECK(unpack_point(ask("pos", stale)) == nowhere);
  CHECK(ask("health", stale).as_integer() == 0);
  CHECK(!ask("InHolder", stale).truthy_scalar());
}

/// `DelOrder` frees the squad's order (0x00448a10 resets `[squad+0x26]`), and
/// `OrderDest` and `AIDest` both read through that index -- so both go, and
/// `AIDest` falls back to `DestGAIKA`. Where the squad is and its own
/// destination are not the order's.
TEST(squad_del_order_clears_the_order_and_the_ai_destination_it_carried) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(1, 1);
  REQUIRE(f.heroes.squads().post_order(key, 5, 3));
  Squad* squad = f.heroes.squads().find(key);
  CHECK(squad->order == 0);
  CHECK(squad->order_dest == 5);
  CHECK(squad->ai_dest == 5);
  squad->dest_gaika = 7;
  squad->gaika_in = 8;

  HostCall call(f.world, {pack_squad(key)});
  CHECK(invoke(registry, CallKind::member, "DelOrder", 0, call).status == HostStatus::ok);

  squad = f.heroes.squads().find(key);
  CHECK(squad->order == -1);
  CHECK(squad->order_dest == kNoGaika);
  CHECK(squad->ai_dest == kNoGaika);
  CHECK(squad_ai_dest(*squad) == 7);
  CHECK(squad->dest_gaika == 7);
  CHECK(squad->gaika_in == 8);
  // The record is a free slot now: no verb, at the head of the chain, so the
  // drain never takes it.
  const AiOrderQueue* queue = f.heroes.squads().orders(1);
  REQUIRE(queue != nullptr);
  REQUIRE(queue->todo.size() == 1);
  CHECK(queue->todo[0].verb == 0);
  CHECK(queue->first_free == 0);
  // And a second `DelOrder` has nothing to free.
  CHECK(!f.heroes.squads().delete_order(key));
}

/// **`IsEnemyInSquadSight` is always false, and so is the original's.**
///
/// 0x005d7400 reads `[squad+0x7c]`, and that field's only writers in the whole
/// executable are two squad constructors, both writing zero. 20 shipped sites
/// and 132 of this engine's remaining trap hits ask it; every one of them takes
/// the same branch here that it takes on the original.
TEST(squad_is_enemy_in_squad_sight_is_false_for_everyone) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const ObjectId lone = f.spawn();
  HostCall outside(f.world, {obj(lone)});
  const HostOutcome no_squad =
      invoke(registry, CallKind::member, "IsEnemyInSquadSight", 0, outside);
  CHECK(no_squad.status == HostStatus::ok);
  CHECK(no_squad.value.as_integer() == 0);

  // And a unit that is in a squad, with enemies of another player standing on
  // top of it, answers the same.
  const SquadKey key = f.squad_of(1, 2);
  const ObjectId member = f.heroes.squads().find(key)->members.front();
  for (int i = 0; i < 3; ++i) (void)f.spawn(Point{100, 100}, 2);
  HostCall inside(f.world, {obj(member)});
  CHECK(invoke(registry, CallKind::member, "IsEnemyInSquadSight", 0, inside).value.as_integer() ==
        0);
}

// --------------------------------------------------------------------------
// the SquadList cursor
// --------------------------------------------------------------------------

namespace {

/// A list bound to a declaration site, the way `SquadList SL;` binds one.
SquadListId make_list(World& world, script::ScriptId script = 7, std::uint32_t slot = 0) {
  return squadlist_pool_of(world).acquire(script, slot);
}

}  // namespace

/// **The cursor is the list's, not the caller's**, and `Cur`/`Next`/`EOL` are
/// the whole of how the corpus reads one.
///
/// `GS_GUARD.VS` is the idiom: `SL.Lock; while (SL.EOL == false) { squad =
/// SL.Cur; SL.Next(); ... } SL.Unlock;`. Walking off the end leaves the cursor
/// at the end rather than running it on, which is what makes `EOL` stay true.
TEST(squadlist_walks_with_a_cursor_the_list_carries) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey a = f.squad_of(1, 1);
  const SquadKey b = f.squad_of(1, 1);
  const SquadListId list = make_list(f.world);
  squadlist_pool_of(f.world).mutable_items(list)->assign({a, b});

  const auto eol = [&] {
    HostCall call(f.world, {make_squadlist_value(list)});
    return invoke(registry, CallKind::member, "EOL", 0, call).value.as_integer() != 0;
  };
  const auto cur = [&] {
    HostCall call(f.world, {make_squadlist_value(list)});
    return unpack_squad(invoke(registry, CallKind::member, "Cur", 0, call).value);
  };
  const auto next = [&] {
    HostCall call(f.world, {make_squadlist_value(list)});
    return invoke(registry, CallKind::member, "Next", 0, call).value.as_integer() != 0;
  };

  CHECK(!eol());
  CHECK(cur() == a);
  CHECK(next());
  CHECK(!eol());
  CHECK(cur() == b);
  // **Off the last squad `Next` answers false** (0x004201d0: true only when
  // the step did not land on the end). `AIOSENDSQUAD.VS` stops its walk on
  // exactly this answer -- `if (!l.Next()) break;` -- and with true it ran its
  // body once more on no squad.
  CHECK(!next());

  // At the end: `EOL` is true, `Cur` is no squad, and `Next` answers false and
  // moves nothing.
  CHECK(eol());
  CHECK(cur() == kNoSquad);
  CHECK(!next());
  CHECK(eol());

  // `Rewind` goes back to the first, and answers whether there was one.
  HostCall rewind(f.world, {make_squadlist_value(list)});
  CHECK(invoke(registry, CallKind::member, "Rewind", 0, rewind).value.as_integer() != 0);
  CHECK(!eol());
  CHECK(cur() == a);

  // **`Size` is the length, not what is left** -- checked with the cursor part
  // way through, because at the beginning the two readings agree.
  CHECK(next());
  CHECK(squadlist_pool_of(f.world).cursor(list) == 1);
  HostCall size(f.world, {make_squadlist_value(list)});
  CHECK(invoke(registry, CallKind::member, "Size", 0, size).value.as_integer() == 2);
}

/// An empty list is at its end from the start, and a handle that names nothing
/// reads the same way rather than trapping -- `while (SL.EOL == false)` on a
/// stale list has to terminate.
TEST(squadlist_empty_and_stale_lists_are_at_their_end) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadListId empty = make_list(f.world);
  HostCall on_empty(f.world, {make_squadlist_value(empty)});
  CHECK(invoke(registry, CallKind::member, "EOL", 0, on_empty).value.as_integer() != 0);
  HostCall rewind(f.world, {make_squadlist_value(empty)});
  CHECK(invoke(registry, CallKind::member, "Rewind", 0, rewind).value.as_integer() == 0);

  HostCall stale(f.world, {make_squadlist_value(9999)});
  CHECK(invoke(registry, CallKind::member, "EOL", 0, stale).value.as_integer() != 0);

  // **And a receiver that is not a `SquadList` at all**, which is a different
  // branch from a handle that names a dead one: the first never resolves, the
  // second resolves to an empty entry. Both have to read as "at the end",
  // because `while (SL.EOL == false)` has to terminate either way.
  HostCall wrong_type(f.world, {Value::integer(3)});
  CHECK(invoke(registry, CallKind::member, "EOL", 0, wrong_type).value.as_integer() != 0);
  HostCall wrong_cur(f.world, {Value::integer(3)});
  CHECK(unpack_squad(invoke(registry, CallKind::member, "Cur", 0, wrong_cur).value) == kNoSquad);
  HostCall wrong_next(f.world, {Value::integer(3)});
  CHECK(invoke(registry, CallKind::member, "Next", 0, wrong_next).value.as_integer() == 0);
  HostCall wrong_size(f.world, {Value::integer(3)});
  CHECK(invoke(registry, CallKind::member, "Size", 0, wrong_size).value.as_integer() == 0);
  HostCall wrong_rewind(f.world, {Value::integer(3)});
  CHECK(invoke(registry, CallKind::member, "Rewind", 0, wrong_rewind).value.as_integer() == 0);
  HostCall stale_size(f.world, {make_squadlist_value(9999)});
  CHECK(invoke(registry, CallKind::member, "Size", 0, stale_size).value.as_integer() == 0);
  HostCall stale_cur(f.world, {make_squadlist_value(9999)});
  CHECK(unpack_squad(invoke(registry, CallKind::member, "Cur", 0, stale_cur).value) == kNoSquad);
}

/// **One `Size`, four receivers**, and three of them used to answer 0.
///
/// `gbr.exe` registers the name on `Squad`, `SquadList`, `IntArray` and
/// `StrArray`; member lookup here is case-insensitive, so all four arrive at
/// one body and it answered only for the list. What that cost is in
/// `size_impl`'s comment and is not small: 17 of the 18 `Size` sites read a
/// `Squad`, and all five lower-case `size` sites read an array.
///
/// The two shapes that matter to the corpus are asserted together because
/// `SQUADMONITOR.VS:575` puts them in one line -- `if (sq.Count("Horse") !=
/// sq.Size)` -- and a `Count` that walks the membership beside a `Size` that
/// does not would make that comparison always true.
TEST(size_answers_for_a_squad_a_list_and_both_array_types) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  // The status is checked beside the value everywhere below, because a
  // refusal's `value` is a default-constructed 0 and would read as the right
  // answer through `as_integer()` alone -- two of the five zero cases here are
  // *about* answering rather than refusing, and a value-only assertion cannot
  // tell those two apart from the bug they are guarding against.
  const auto size_of = [&](Value receiver) {
    HostCall call(f.world, {receiver});
    const HostOutcome out = invoke(registry, CallKind::member, "Size", 0, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.as_integer();
  };

  // A `Squad`: the members, and it is `members.size()` rather than a stored
  // number -- `[squad+0x50]` is `_Mysize` of the deque whose header is at
  // `+0x40`, the one `Squad::Count` walks -- so joining a unit moves it.
  const SquadKey squad = f.squad_of(1, 3);
  CHECK(size_of(pack_squad(squad)) == 3);
  (void)f.heroes.squads().join(squad, f.spawn(Point{400, 400}, 1));
  CHECK(size_of(pack_squad(squad)) == 4);
  HostCall counted(f.world, {pack_squad(squad), Value::string("Unit")});
  CHECK(invoke(registry, CallKind::member, "Count", 1, counted).value.as_integer() == 4);

  // A `SquadList`: the squads.
  const SquadListId list = make_list(f.world);
  squadlist_pool_of(f.world).mutable_items(list)->assign({squad, f.squad_of(2, 1)});
  CHECK(size_of(make_squadlist_value(list)) == 2);

  // An `IntArray` and a `StrArray`: the elements, and an array grows by being
  // written past its end, so the answer is the high water mark rather than a
  // declared capacity.
  const ScriptArrayId ints = f.world.arrays().acquire(11, 0, false);
  const ScriptArrayId text = f.world.arrays().acquire(11, 1, true);
  CHECK(size_of(make_array_value(kTypeIntArray, ints)) == 0);
  CHECK(f.world.arrays().set(ints, 4, Value::integer(9)));
  CHECK(size_of(make_array_value(kTypeIntArray, ints)) == 5);
  CHECK(f.world.arrays().set(text, 1, Value::string("x")));
  CHECK(size_of(make_array_value(kTypeStrArray, text)) == 2);

  // Everything that names nothing answers 0 rather than refusing, because
  // every shipped reader is a loop bound or an arithmetic operand:
  // `for (i = 0; i < race_num.size; i += 1)` on a stale handle must run zero
  // times, not trap.
  CHECK(size_of(pack_squad(SquadKey{99, 3})) == 0);
  CHECK(size_of(make_squadlist_value(9999)) == 0);
  CHECK(size_of(make_array_value(kTypeIntArray, 9999)) == 0);
  CHECK(size_of(Value::integer(3)) == 0);
  CHECK(size_of(Value::string("not a receiver")) == 0);

  // A missing world is the one refusal: that is a broken host rather than a
  // stale handle, and it is the rule every other body in this file follows.
  HostCall worldless(f.world, {pack_squad(squad)}, false);
  CHECK(invoke(registry, CallKind::member, "Size", 0, worldless).status != HostStatus::ok);
}

/// **The region census, with squads that actually have nodes.**
///
/// Every one of these entry points answers zero on a real map today, because
/// nothing in this engine assigns a GAIKA to a squad. That makes them easy to
/// ship and easy to ship *wrong*: a body that returned a constant zero would
/// pass every corpus run and every trap sweep. So the fixture writes the three
/// GAIKA fields itself and the census is measured against them.
///
/// The four totals are `own, ally, enemy, enemy_hidden` in that order --
/// `gbr.exe` types all four as an `int` by reference and the corpus names them
/// in that order at all nineteen sites.
TEST(the_gaika_census_sums_strength_by_presence_and_by_side) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  // Player 0 is asking. 1 is its ally, 2 its enemy, 3 neither.
  PlayerTable& players = f.world.players();
  players.set(0, 1, Relation::allied, true);
  players.set(1, 0, Relation::allied, true);
  players.set(0, 2, Relation::ceasefire, false);
  players.set(0, 3, Relation::ceasefire, true);

  constexpr GaikaId kHere = 5;
  constexpr GaikaId kElsewhere = 9;
  const auto place = [&](PlayerId owner, std::size_t members, std::int32_t strength,
                         GaikaId in, GaikaId dest) {
    const SquadKey key = f.squad_of(owner, members);
    Squad* squad = f.heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad == nullptr) return;
    squad->eval = strength;
    squad->gaika_in = in;
    squad->dest_gaika = dest;
  };

  place(0, 3, 100, kHere, kNoGaika);       // own, staying
  place(1, 2, 40, kHere, kHere);           // ally, staying (destination is here)
  place(2, 5, 250, kHere, kElsewhere);     // enemy, leaving
  place(2, 1, 7, kElsewhere, kHere);       // enemy, coming
  place(3, 9, 999, kHere, kNoGaika);       // neither side: counted by nobody

  const auto census = [&](const char* name, std::uint16_t arity, std::int32_t presence) {
    HostCall call(f.world,
                  {gaika_value(kHere), Value::integer(presence), Value::integer(1),
                   Value::integer(-1), Value::integer(-1), Value::integer(-1),
                   Value::integer(-1)});
    const HostOutcome out = invoke(registry, CallKind::member, name, arity, call);
    CHECK(out.status == HostStatus::ok);
    return std::array<std::int32_t, 4>{call.arguments[3].as_integer(),
                                       call.arguments[4].as_integer(),
                                       call.arguments[5].as_integer(),
                                       call.arguments[6].as_integer()};
  };

  // Staying only: the own squad and the ally's. The enemies are leaving and
  // coming, so neither is here under this mask.
  {
    const auto totals = census("Eval", 6, 4 /*AI_STAYING*/);
    CHECK(totals[0] == 100);
    CHECK(totals[1] == 40);
    CHECK(totals[2] == 0);
    CHECK(totals[3] == 0);
  }
  // Leaving picks up the siege force; coming picks up the scout.
  CHECK(census("Eval", 6, 2 /*AI_LEAVING*/)[2] == 250);
  CHECK(census("Eval", 6, 1 /*AI_COMING*/)[2] == 7);
  // And the masks compose, which is how five of the shipped sites write them.
  CHECK(census("Eval", 6, 4 + 1)[2] == 7);
  CHECK(census("Eval", 6, 0xFFFF /*AI_ALL*/)[2] == 257);
  // `AI_NONE` selects nothing rather than everything.
  CHECK(census("Eval", 6, 0)[0] == 0);

  // **`Count` is bodies where `Eval` is strength**, which is the whole
  // difference between the two and the reason `GETGAIKASTRAT.VS` calls both on
  // one node in consecutive lines.
  {
    const auto bodies = census("Count", 6, 4);
    CHECK(bodies[0] == 3);
    CHECK(bodies[1] == 2);
    CHECK(bodies[2] == 0);
  }
  CHECK(census("Count", 6, 0xFFFF)[2] == 6);

  // The three- and two-argument forms select from the same census. The
  // two-argument form answers *own*, which is what `GSH_INTERRUPTPASSING.VS`
  // names it: `lown = g.Eval(AI_LEAVING, AIPlayer)`.
  const auto total = [&](std::uint16_t arity, std::int32_t presence, std::int32_t sides) {
    std::vector<Value> args{gaika_value(kHere), Value::integer(presence), Value::integer(1)};
    if (arity == 3) args.push_back(Value::integer(sides));
    HostCall call(f.world, {});
    call.arguments = args;
    call.context.arguments = call.arguments;
    const HostOutcome out = invoke(registry, CallKind::member, "Eval", arity, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.as_integer();
  };
  CHECK(total(3, 4, 4 /*AI_ENEMY*/) == 0);
  CHECK(total(3, 2, 4) == 250);
  CHECK(total(3, 4, 1 + 2 /*AI_FRIENDLY*/) == 140);
  CHECK(total(3, 4, 0xFFFF) == 140);
  CHECK(total(2, 4, 0) == 100);   // defaults to AI_OWN
  CHECK(total(2, 2, 0) == 0);

  // A squad with no strength is skipped by both, which is the original's first
  // test and the reason `Eval` and `Count` can disagree about whether a node is
  // occupied at all.
  place(0, 4, 0, kHere, kNoGaika);
  CHECK(census("Count", 6, 4)[0] == 3);
  // And an `SF_PEACEFUL` squad is nobody's army. **This used to be written as
  // "player 14 or 15"**, which is where the wildlife and the neutral-passive
  // live on a shipped map; the flag is what 0x0044e1f0 actually tests, and the
  // two readings part company in both directions. A peaceful squad belonging to
  // the asking player is skipped:
  {
    const SquadKey key = f.squad_of(0, 6);
    Squad* squad = f.heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad != nullptr) {
      squad->eval = 500;
      squad->gaika_in = kHere;
      squad->flags = kSquadFlagPeaceful;
    }
  }
  CHECK(census("Eval", 6, 0xFFFF)[0] == 100);
  // and a player-14 squad that carries no such flag is counted, where the old
  // rule refused it.
  place(14, 6, 500, kHere, kNoGaika);
  CHECK(census("Eval", 6, 0xFFFF)[2] == 257 + 500);

  // A player number outside 1..16 names nobody, so nothing is anyone's. **Both
  // ends of the range are load-bearing and the upper one is only visible far
  // out**: `PlayerId` is a byte, so 257 would wrap to 0 and hand back player
  // 0's own squads if the bound were only `>= 1`. That is the fault the sweep
  // could not otherwise reach.
  const auto asking = [&](std::int32_t who) {
    HostCall call(f.world, {gaika_value(kHere), Value::integer(0xFFFF), Value::integer(who),
                            Value::integer(-1), Value::integer(-1), Value::integer(-1),
                            Value::integer(-1)});
    CHECK(invoke(registry, CallKind::member, "Eval", 6, call).status == HostStatus::ok);
    return call.arguments[3].as_integer() + call.arguments[4].as_integer() +
           call.arguments[5].as_integer();
  };
  CHECK(asking(1) != 0);
  for (const std::int32_t who : {0, -1, 17, 257, 1000}) CHECK(asking(who) == 0);

  // **A squad in no node is not in node zero.** `kNoGaika` is 0 and every squad
  // on every real map carries it, so a presence rule that answered "staying"
  // for it would make node 0 hold the whole army list.
  {
    HostCall call(f.world, {gaika_value(kNoGaika), Value::integer(0xFFFF), Value::integer(1),
                            Value::integer(-1), Value::integer(-1), Value::integer(-1),
                            Value::integer(-1)});
    CHECK(invoke(registry, CallKind::member, "Eval", 6, call).status == HostStatus::ok);
    CHECK(call.arguments[3].as_integer() == 0);
    CHECK(call.arguments[4].as_integer() == 0);
    CHECK(call.arguments[5].as_integer() == 0);
  }
  // ...including squads whose `gaika_in` is itself `kNoGaika`, which is every
  // squad this engine creates.
  {
    const SquadKey drifting = f.squad_of(0, 2);
    Squad* squad = f.heroes.squads().find(drifting);
    REQUIRE(squad != nullptr);
    squad->eval = 60;
    HostCall call(f.world, {gaika_value(kNoGaika), Value::integer(0xFFFF), Value::integer(1),
                            Value::integer(-1), Value::integer(-1), Value::integer(-1),
                            Value::integer(-1)});
    CHECK(invoke(registry, CallKind::member, "Eval", 6, call).status == HostStatus::ok);
    CHECK(call.arguments[3].as_integer() == 0);
  }

  // `EvalNeighbors` needs the adjacency list a node record would carry, which
  // this engine does not model at all -- so it answers zero for a second reason
  // and says so where it lives. What is asserted is that it writes its three
  // out-parameters rather than leaving the caller's locals as they were.
  {
    HostCall call(f.world, {gaika_value(kHere), Value::integer(0xFFFF), Value::integer(1),
                            Value::integer(-1), Value::integer(-1), Value::integer(-1),
                            Value::boolean(true)});
    CHECK(invoke(registry, CallKind::member, "EvalNeighbors", 6, call).status == HostStatus::ok);
    CHECK(call.arguments[3].as_integer() == 0);
    CHECK(call.arguments[4].as_integer() == 0);
    CHECK(call.arguments[5].as_integer() == 0);
  }
}

/// `GAIKA::Count/3`, which is the census asked about **one class**.
///
/// The two shipped call sites are in `GS_CAPTURE.VS` and both read
/// `enemyC += gaika.Count(AI_ENEMY, AIPlayer, "Peasant")`, so what this
/// measures is the argument order the file's note argues for -- (sides, player,
/// class), with the player 1-based -- and the two rules that make the walk
/// differ from `Count/6`'s: presence is `AI_STAYING` outright, and a peaceful
/// squad is counted here where the census skips it.
///
/// The class tree is written here, not read from a map: `Unit` with `Peasant`
/// and `Sentry` under it and `RSentry` under `Sentry`, which is the shape the
/// tail needs -- the settlement contributes only when the asked-for class is
/// somewhere in the `Sentry` subtree.
TEST(gaika_count_by_class_walks_the_staying_squads_and_then_the_settlement) {
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent="">
      <properties maxhealth="200" speed="50" sight="500" radius="15"/>
    </class>)"),
            "unit.sc.xml");
  graph.add(bytes(R"(<class id="Peasant" cpp_class="CVXUnit" parent="Unit"/>)"), "peasant.sc.xml");
  graph.add(bytes(R"(<class id="Sentry" cpp_class="CVXUnit" parent="Unit"/>)"), "sentry.sc.xml");
  graph.add(bytes(R"(<class id="RSentry" cpp_class="CVXUnit" parent="Sentry"/>)"), "rsentry.sc.xml");
  graph.link();

  World world;
  EconomySystem economy;
  HeroSystem heroes;
  world.set_class_graph(&graph);
  world.add_system(&economy);
  world.add_system(&heroes);

  HostRegistry registry;
  register_squad_host(registry);

  // Player 0 asks. 1 is its ally, 2 its enemy.
  PlayerTable& players = world.players();
  players.set(0, 1, Relation::allied, true);
  players.set(1, 0, Relation::allied, true);
  players.set(0, 2, Relation::ceasefire, false);

  const auto spawn_of = [&](std::string_view class_name, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find(class_name));
    world.set_owner(id, owner);
    world.set_health(id, 100);
    return id;
  };
  /// A squad of `owner`'s, standing in `in`, made of the named classes.
  ///
  /// **It hands back a key rather than a pointer**, because `SquadTable` keeps
  /// its squads in a vector and every `create` can move the ones already in it.
  const auto squad_of = [&](PlayerId owner, GaikaId in,
                            std::initializer_list<std::string_view> classes) {
    const SquadKey key = heroes.squads().create(owner);
    for (const std::string_view name : classes) {
      (void)heroes.squads().join(key, spawn_of(name, owner));
    }
    Squad* squad = heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad != nullptr) {
      squad->eval = 10;
      squad->gaika_in = in;
    }
    return key;
  };
  const auto sq = [&](SquadKey key) -> Squad& { return *heroes.squads().find(key); };

  // The settlement is node 1: `GaikaTable::build` lays the settlements down
  // first, in store order, so the id is a function of the map rather than of
  // anything that happens in play.
  const ObjectId hall = world.spawn(NativeClass::building, nullptr);
  world.set_owner(hall, 2);
  SettlementInit init;
  init.settlement_object = hall;
  init.anchor = hall;
  init.owner = 2;
  init.kind = SettlementKind::stronghold;
  const SettlementId town = economy.settlements().create(init);
  // A second settlement, so that "the node's settlement" is a claim a test can
  // fail. With one of them every wrong node answers the right way.
  const ObjectId outpost = world.spawn(NativeClass::building, nullptr);
  world.set_owner(outpost, 1);
  SettlementInit second;
  second.settlement_object = outpost;
  second.anchor = outpost;
  second.owner = 1;
  second.kind = SettlementKind::stronghold;
  const SettlementId camp = economy.settlements().create(second);
  world.mutable_gaika().build(world, world.lsa(), economy.settlements());
  constexpr GaikaId kHere = 1;
  constexpr GaikaId kThere = 2;
  constexpr GaikaId kElsewhere = 7;
  REQUIRE(world.gaika().find(kHere) != nullptr);
  REQUIRE(world.gaika().find(kThere) != nullptr);
  CHECK(world.gaika().find(kHere)->settlement == hall);
  CHECK(world.gaika().find(kThere)->settlement == outpost);

  const auto count = [&](std::int32_t sides, std::int32_t player, const char* klass) {
    HostCall call(world, {gaika_value(kHere), Value::integer(sides), Value::integer(player),
                          Value::string(klass)});
    const HostOutcome out = invoke(registry, CallKind::member, "Count", 3, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.as_integer();
  };

  // Two enemy peasants and a horse-less escort standing in the node, plus one
  // more peasant of the enemy's that is only passing through.
  squad_of(2, kHere, {"Peasant", "Peasant", "Sentry"});
  const SquadKey passing = squad_of(2, kHere, {"Peasant"});
  sq(passing).dest_gaika = kElsewhere;  // leaving, so not standing here

  // The shipped line, exactly: `gaika.Count(AI_ENEMY, AIPlayer, "Peasant")`
  // with `AIPlayer` 1-based on a player-0 AI.
  CHECK(count(4 /*AI_ENEMY*/, 1, "Peasant") == 2);
  // The class test is the tree, not a leaf: `Unit` covers the sentry too.
  CHECK(count(4, 1, "Unit") == 3);
  // And the sides mask is the relation. The asking player owns none of this.
  CHECK(count(1 /*AI_OWN*/, 1, "Peasant") == 0);
  CHECK(count(1 + 2 + 4, 1, "Peasant") == 2);

  // **The player argument is 1-based**, which is the half of the argument order
  // the corpus could not settle: asking as player 2 -- the enemy's own number
  // read 1-based -- turns the same squads into its own.
  CHECK(count(4, 3, "Peasant") == 0);
  CHECK(count(1, 3, "Peasant") == 2);

  // A name the graph does not know counts nothing rather than everything, which
  // is what makes a typo in a shipped script safe.
  CHECK(count(0xFFFF, 1, "Elephant") == 0);

  // An ally's squad, and the mask picking it out on its own.
  squad_of(1, kHere, {"Peasant", "Peasant", "Peasant"});
  CHECK(count(2 /*AI_ALLY*/, 1, "Peasant") == 3);
  CHECK(count(4 + 2, 1, "Peasant") == 5);

  // **A squad with no strength is skipped**, the census's first test and this
  // one's too.
  const SquadKey hollow = squad_of(2, kHere, {"Peasant"});
  sq(hollow).eval = 0;
  CHECK(count(4, 1, "Peasant") == 2);
  sq(hollow).eval = 1;
  CHECK(count(4, 1, "Peasant") == 3);
  sq(hollow).eval = 0;

  // **A peaceful squad is counted here and is not counted by the census**, and
  // that asymmetry is the third argument to 0x0044e1f0: the census passes 0 and
  // gets the flag gate, this passes 1 and does not.
  const SquadKey wildlife = squad_of(2, kHere, {"Peasant", "Peasant"});
  sq(wildlife).flags = kSquadFlagPeaceful;
  CHECK(count(4, 1, "Peasant") == 4);

  // ## The tail
  //
  // The settlement's ready sentries, added only when the asked-for class is in
  // the `Sentry` tree and only when the settlement owner's relation is in the
  // mask. It is the half of this entry point that answers a real number today.
  Settlement* set = economy.settlements().find(town);
  REQUIRE(set != nullptr);
  set->sentries_ready = 6;
  CHECK(count(4, 1, "Sentry") == 6 + 1);   // the one standing sentry, plus six in the roster
  CHECK(count(4, 1, "RSentry") == 6);      // a descendant of `Sentry` still asks for them
  // **And `Unit` does not, though every sentry is a `Unit`.** The test runs the
  // wrong way round on purpose, because that is the way 0x0059c020 runs it: it
  // walks the *asked-for* class's ancestors looking for `Sentry`, so the tail
  // fires for `Sentry` and below and for nothing above it. A caller asking how
  // many units stand in a settlement is told about the ones on the field.
  CHECK(count(4, 1, "Unit") == 4 + 1);
  CHECK(count(4, 1, "Peasant") == 4);      // `Peasant` is beside `Sentry`, not under it

  // The settlement belongs to player 2, so it answers to `AI_ENEMY` and not to
  // the other two.
  CHECK(count(1 /*AI_OWN*/, 1, "Sentry") == 0);
  CHECK(count(2 /*AI_ALLY*/, 1, "Sentry") == 0);
  // An empty roster contributes nothing rather than a zero-strength entry, and
  // the gate is `> 0` rather than `!= 0`: the original's is a signed `jle` and
  // `sentries_ready` is a signed counter that `Settlement::AddSentries` adds a
  // caller's number straight into. A roster below zero must not subtract.
  set->sentries_ready = 0;
  CHECK(count(4, 1, "Sentry") == 1);
  set->sentries_ready = -3;
  CHECK(count(4, 1, "Sentry") == 1);
  set->sentries_ready = 6;

  // **The settlement is the node's own**, which is a claim about `GaikaTable`'s
  // numbering as much as about this walk: the settlements are laid down first
  // in store order, so the ally's outpost is node 2 and its roster answers only
  // there. No squad stands in it, so what comes back is the roster alone.
  Settlement* out = economy.settlements().find(camp);
  REQUIRE(out != nullptr);
  out->sentries_ready = 4;
  const auto count_at = [&](GaikaId node, std::int32_t sides, const char* klass) {
    HostCall call(world, {gaika_value(node), Value::integer(sides), Value::integer(1),
                          Value::string(klass)});
    const HostOutcome outcome = invoke(registry, CallKind::member, "Count", 3, call);
    CHECK(outcome.status == HostStatus::ok);
    return outcome.value.as_integer();
  };
  CHECK(count_at(kThere, 2 /*AI_ALLY*/, "Sentry") == 4);
  CHECK(count_at(kThere, 4 /*AI_ENEMY*/, "Sentry") == 0);
  CHECK(count_at(kHere, 2, "Sentry") == 0);
  CHECK(count_at(kHere, 4, "Sentry") == 6 + 1);

  // An unowned settlement is nobody's, and answers to no mask at all rather
  // than folding onto player 0's row.
  out->owner = kNoPlayer;
  CHECK(count_at(kThere, 0xFFFF, "Sentry") == 0);
  out->owner = 1;

  // **A caller naming no player gets `AI_OWN` for everything**, which is the
  // original's branch when the decremented index goes negative -- so
  // `Count(AI_OWN, 0, ...)` counts every squad in the node rather than none.
  CHECK(count(1, 0, "Peasant") == 4 + 3);
  CHECK(count(1, 0, "Sentry") == 1 + 6);
  CHECK(count(4 /*AI_ENEMY*/, 0, "Peasant") == 0);
  // A number past the sixteenth player names nobody either, and the alliance
  // table answers 0 for it, which selects nothing under any mask.
  CHECK(count(0xFFFF, 17, "Peasant") == 0);
  CHECK(count(0xFFFF, 257, "Peasant") == 0);

  // A node that is not this one has neither the squads nor the settlement.
  {
    HostCall call(world, {gaika_value(kElsewhere), Value::integer(0xFFFF), Value::integer(1),
                          Value::string("Unit")});
    const HostOutcome out = invoke(registry, CallKind::member, "Count", 3, call);
    CHECK(out.status == HostStatus::ok);
    CHECK(out.value.as_integer() == 0);
  }
}

/// `GAIKA::AllEnemiesInHolder/1`, the question `GS_CAPTURE.VS` asks before it
/// commits to a siege: is the garrison the only thing left?
///
/// Three claims carry the reading and each has a case here:
///
///   * the two early refusals -- no settlement, and a settlement whose owner is
///     not the asker's enemy -- both answer **false**, not true, so a node with
///     nothing hostile about it never reads as "everything is indoors";
///   * the enemy test is the **settlement owner's** view of the asker and not
///     the asker's of it, which matters because the shipped maps carry
///     one-sided truces; and
///   * a squad is judged by its **first member alone**.
TEST(all_enemies_in_holder_asks_the_settlement_first_and_then_the_squads_leaders) {
  ClassGraph graph = fixture_graph();
  World world;
  EconomySystem economy;
  HeroSystem heroes;
  world.set_class_graph(&graph);
  world.add_system(&economy);
  world.add_system(&heroes);

  HostRegistry registry;
  register_squad_host(registry);

  // Player 0 asks; player 2 holds the settlement. The rows are set one at a
  // time on purpose: `is_enemy` reads one direction only.
  PlayerTable& players = world.players();
  players.set(2, 0, Relation::ceasefire, false);  // player 2 is hostile to player 0
  players.set(0, 2, Relation::ceasefire, false);
  // And player 3 is nobody's enemy. A row nothing was written into grants no
  // ceasefire, which `is_enemy` reads as hostile, so "neutral" has to be said.
  players.set(0, 3, Relation::ceasefire, true);
  players.set(3, 0, Relation::ceasefire, true);

  const ObjectId hall = world.spawn(NativeClass::building, nullptr);
  world.set_owner(hall, 2);
  SettlementInit init;
  init.settlement_object = hall;
  init.anchor = hall;
  init.owner = 2;
  init.kind = SettlementKind::stronghold;
  const SettlementId town = economy.settlements().create(init);
  // A second settlement that never got its object handles -- the state
  // `sim/settlement.hpp` describes as "left null when the allocator has not
  // run". `GaikaTable::build` still lays a node down for it, and that node's
  // `settlement` is `kNoObject`, which is the *existing* node with nothing to
  // be inside that the first refusal is about. A node id nobody minted would
  // not test it, because a missing node refuses one line earlier.
  SettlementInit unbuilt;
  unbuilt.anchor = world.spawn(NativeClass::building, nullptr);
  unbuilt.owner = 2;
  unbuilt.kind = SettlementKind::stronghold;
  (void)economy.settlements().create(unbuilt);
  world.mutable_gaika().build(world, world.lsa(), economy.settlements());
  constexpr GaikaId kHere = 1;
  constexpr GaikaId kNoWalls = 2;
  constexpr GaikaId kElsewhere = 9;
  REQUIRE(world.gaika().find(kHere) != nullptr);
  REQUIRE(world.gaika().find(kNoWalls) != nullptr);
  CHECK(world.gaika().find(kHere)->settlement == hall);
  CHECK(world.gaika().find(kNoWalls)->settlement == kNoObject);

  const auto ask = [&](GaikaId node, std::int32_t who) {
    HostCall call(world, {gaika_value(node), Value::integer(who)});
    const HostOutcome out = invoke(registry, CallKind::member, "AllEnemiesInHolder", 1, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.truthy_scalar();
  };

  // Nothing hostile is standing in the open, so the garrison is all there is.
  CHECK(ask(kHere, 1) == true);

  // **A node with no settlement refuses, and it refuses with `false`.** Both
  // callers read `true` as "commit", so a region node must never say yes.
  CHECK(ask(kNoWalls, 1) == false);
  // And so does a node id nobody minted, one refusal earlier.
  CHECK(ask(kElsewhere, 1) == false);
  CHECK(ask(kNoGaika, 1) == false);
  // So does a player the argument does not name. The original decrements and
  // indexes the player table unguarded at both ends; refusing is this file's
  // standing answer to that.
  CHECK(ask(kHere, 0) == false);
  CHECK(ask(kHere, 17) == false);

  // **The enemy test runs the settlement owner's way round.** Player 2 grants
  // player 0 a ceasefire while player 0 grants none: under the asker's-view
  // reading this still answers, under the owner's-view reading it refuses, and
  // the owner's view is the engine's.
  players.set(2, 0, Relation::ceasefire, true);
  CHECK(ask(kHere, 1) == false);
  players.set(2, 0, Relation::ceasefire, false);
  CHECK(ask(kHere, 1) == true);

  // A squad of `owner`'s standing in `in`, whose members are held by `holder`.
  //
  // **Keys, not pointers.** `SquadTable` keeps its squads in a vector, so every
  // `create` can move the ones already in it; a `Squad*` held across one is a
  // dangling read that answers plausibly. This cost a debugging round.
  const auto garrison = [&](PlayerId owner, GaikaId in, std::size_t members,
                            ObjectId holder) {
    const SquadKey key = heroes.squads().create(owner);
    for (std::size_t i = 0; i < members; ++i) {
      const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
      world.set_owner(id, owner);
      world.set_health(id, 100);
      if (holder != kNoObject) CHECK(world.put_in_holder(id, holder));
      (void)heroes.squads().join(key, id);
    }
    Squad* squad = heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad != nullptr) {
      squad->eval = 10;
      squad->gaika_in = in;
    }
    return key;
  };
  // `garrison` has already asserted that every key it hands back resolves, so
  // this dereferences; a null would abort the binary, which is loud enough.
  const auto sq = [&](SquadKey key) -> Squad& { return *heroes.squads().find(key); };

  // An enemy squad indoors leaves the answer alone.
  garrison(2, kHere, 3, hall);
  CHECK(ask(kHere, 1) == true);

  // One in the open turns it over.
  const SquadKey outside = garrison(2, kHere, 2, kNoObject);
  CHECK(ask(kHere, 1) == false);

  // **And the squad is judged by its first member alone.** Putting the leader
  // indoors and leaving the rest in the street reads as indoors, which is the
  // original's rule and not an obvious one.
  const ObjectId leader = sq(outside).members.front();
  const ObjectId follower = sq(outside).members.back();
  CHECK(leader != follower);
  CHECK(world.put_in_holder(leader, hall));
  CHECK(ask(kHere, 1) == true);
  CHECK(world.remove_from_holder(leader, Point{100, 100}));
  CHECK(ask(kHere, 1) == false);
  // ...and the other way round: the followers indoors and the leader out is
  // still "somebody is out there".
  CHECK(world.put_in_holder(follower, hall));
  CHECK(ask(kHere, 1) == false);
  CHECK(world.remove_from_holder(follower, Point{100, 100}));

  // The four gates, each measured on the squad that is out in the open.
  //
  // Zero strength, as everywhere in this family.
  sq(outside).eval = 0;
  CHECK(ask(kHere, 1) == true);
  sq(outside).eval = 10;
  // `SF_PEACEFUL`: wildlife in the street does not stop a siege. This is the
  // classifier's flag gate, which `Count/3` beside it does *not* get.
  sq(outside).flags = kSquadFlagPeaceful;
  CHECK(ask(kHere, 1) == true);
  sq(outside).flags = 0;
  // Relation: only an enemy squad is asked about. Player 3 is nobody's enemy
  // here, so its squad standing in the open is not an obstacle.
  garrison(3, kHere, 1, kNoObject);
  CHECK(ask(kHere, 1) == false);  // still false, because `outside` is still out
  sq(outside).eval = 0;
  CHECK(ask(kHere, 1) == true);   // and player 3's squad on its own is not asked
  sq(outside).eval = 10;

  // **Presence is masked with 6, not 4** -- staying *or* leaving. A squad on
  // its way out is still standing here.
  sq(outside).dest_gaika = kElsewhere;  // leaving
  CHECK(ask(kHere, 1) == false);
  // ...and one merely on its way in is not here yet.
  sq(outside).gaika_in = kElsewhere;
  sq(outside).dest_gaika = kHere;  // coming
  CHECK(ask(kHere, 1) == true);

  // An empty squad decides nothing either way.
  garrison(2, kHere, 0, kNoObject);
  CHECK(ask(kHere, 1) == true);

  // A settlement that has changed hands stops being an enemy's, and the whole
  // question refuses again.
  Settlement* set = economy.settlements().find(town);
  REQUIRE(set != nullptr);
  set->owner = 0;
  CHECK(ask(kHere, 1) == false);
}

// --------------------------------------------------------------------------
// the squad former
// --------------------------------------------------------------------------
//
// **`Squadize` is the first entry point in this tree that decides which army a
// body belongs to.** Everything else here reads a membership somebody else
// built. So these cases are about the rule and not about the plumbing, and the
// numbers in them -- 256 world units, ten members -- are read out of `gbr.exe`
// rather than chosen.
//
// No game data: the class tree is written here and the positions are arithmetic
// picked to sit either side of the radius.

namespace {

/// A world, a hero system, an `ObjList` to hand in and a `SquadList` to get
/// back. Separate from `Fixture` above because a squad former needs a clean
/// table per case: squads persist between calls, and a case that inherited the
/// previous one's would be measuring the wrong thing.
ClassGraph former_graph() {
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent="">
      <properties maxhealth="200" speed="50" sight="500" radius="15"/>
    </class>)"),
            "unit.sc.xml");
  // `squad_flags_by_class` reads three names off the class tree, and the flags
  // word it produces is one of the nine clauses the merge predicate tests.
  graph.add(bytes(R"(<class id="Peaceful" cpp_class="CVXUnit" parent="Unit"/>)"), "p.sc.xml");
  graph.add(bytes(R"(<class id="Sentry" cpp_class="CVXUnit" parent="Unit"/>)"), "s.sc.xml");
  graph.link();
  return graph;
}

struct FormerBench {
  ClassGraph graph = former_graph();
  World world;
  HeroSystem heroes;
  HostRegistry registry;
  ObjListId bodies = 0;
  SquadListId out = 0;

  FormerBench() {
    world.set_class_graph(&graph);
    world.add_system(&heroes);
    register_squad_host(registry);
    bodies = objlist_pool_of(world).acquire(3, 0);
    out = squadlist_pool_of(world).acquire(3, 1);
  }

  ObjectId put(Point at, NativeClass native = NativeClass::unit, PlayerId owner = 1,
               std::string_view klass = "Unit") {
    const ObjectId id = world.spawn(native, nullptr, graph.find(klass));
    CHECK(world.set_position(id, at));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, 100));
    objlist_pool_of(world).mutable_items(bodies)->push_back(id);
    return id;
  }

  std::vector<SquadKey> squadize(std::int32_t state) {
    HostCall call(world, {make_objlist_value(bodies), make_squadlist_value(out),
                          Value::integer(state)});
    const HostOutcome outcome = invoke(registry, CallKind::free_function, "Squadize", 3, call);
    CHECK(outcome.status == HostStatus::ok);
    return *squadlist_pool_of(world).mutable_items(out);
  }

  std::vector<ObjectId> members(SquadKey key) {
    const Squad* squad = heroes.squads().find(key);
    return squad == nullptr ? std::vector<ObjectId>{} : squad->members;
  }
};

}  // namespace

/// The list that comes back is **one entry per squad, not one per body**, and
/// the squads are formed by proximity to a leader.
TEST(squadize_groups_by_proximity_and_lists_one_seed_per_squad) {
  FormerBench b;

  // Three men standing together, and one a long way off.
  const ObjectId a = b.put(Point{1000, 1000});
  const ObjectId near = b.put(Point{1100, 1000});   // 100 from `a`
  const ObjectId also = b.put(Point{1000, 1200});   // 200 from `a`
  const ObjectId away = b.put(Point{9000, 9000});

  const std::vector<SquadKey> seeds = b.squadize(7);
  REQUIRE(seeds.size() == 2);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({a, near, also}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({away}));

  // The caller's state lands on every squad the list names, and so does the
  // clock. The original stamps them in a second pass over the list; this
  // asserts the answer rather than the pass.
  for (const SquadKey key : seeds) {
    const Squad* squad = b.heroes.squads().find(key);
    REQUIRE(squad != nullptr);
    CHECK(squad->state == 7);
    CHECK(squad->state_time == b.world.time());
  }

  // **The membership record moves with the table.** `UnitRecord::squad` is
  // folded into the world hash, so a unit the table moved and the record did
  // not is a divergence and not a cosmetic drift.
  for (const ObjectId id : {a, near, also}) {
    const UnitRecord* record = b.heroes.unit(id);
    REQUIRE(record != nullptr);
    CHECK(record->squad == seeds[0]);
  }

  // Nothing is left behind: the fresh single-member squads the walk mints for
  // `near` and `also` are emptied by the merge and pruned.
  CHECK(b.heroes.squads().size() == 2);

  // And the `ObjList` it was handed is not consumed.
  CHECK(objlist_pool_of(b.world).items(b.bodies).size() == 4);
}

/// **256 world units, and the test is against the squad's leader** rather than
/// against its nearest member. Two `LsaPartition` slots; a squad is a knot of
/// men, not a front.
TEST(squadize_measures_256_units_to_the_leader) {
  FormerBench b;
  // Exactly on the radius, which the original admits: its comparison is `jle`.
  const ObjectId lead = b.put(Point{0, 0});
  const ObjectId edge = b.put(Point{0, 256});
  // And one step past it, far enough away not to see either of the two above.
  const ObjectId out_lead = b.put(Point{20000, 0});
  const ObjectId out_edge = b.put(Point{20000, 257});

  const std::vector<SquadKey> seeds = b.squadize(1);
  REQUIRE(seeds.size() == 3);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({lead, edge}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({out_lead}));
  CHECK(b.members(seeds[2]) == std::vector<ObjectId>({out_edge}));
}

/// Ten, and the eleventh starts a squad of its own -- `cmp [cand+0x50], 0xa` at
/// 0x00446dfa. Every one of the eleven is within the radius of the first, so
/// the cap is the only thing that can separate them.
TEST(squadize_stops_a_squad_at_ten_members) {
  FormerBench b;
  std::vector<ObjectId> all;
  for (std::int32_t i = 0; i < 11; ++i) all.push_back(b.put(Point{i * 10, 0}));

  const std::vector<SquadKey> seeds = b.squadize(1);
  REQUIRE(seeds.size() == 2);
  CHECK(b.members(seeds[0]).size() == 10);
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({all.back()}));
}

/// **A hero leads its own squad and nothing ever merges into one**, which is
/// the pivot of the whole rule: `is_hero_object` is asked three times in it.
/// `sim/hero.hpp` measured the same identity from the dumps -- a hero's squad
/// *is* its army -- before anything here could form one.
TEST(squadize_never_merges_into_a_heros_squad) {
  FormerBench b;
  const ObjectId hero = b.put(Point{0, 0}, NativeClass::hero);
  b.heroes.register_hero(b.world, hero);
  const ObjectId follower = b.put(Point{50, 0});

  const std::vector<SquadKey> seeds = b.squadize(3);
  REQUIRE(seeds.size() == 2);
  // The hero keeps the squad it was registered with, and the unit standing
  // fifty units away gets one of its own rather than joining it.
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({hero}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({follower}));
  const HeroRecord* record = b.heroes.hero(hero);
  REQUIRE(record != nullptr);
  CHECK(record->squad == seeds[0]);
}

/// A **ship** never merges, in either direction: it does not go looking, and
/// its squad is refused as a target because the leader test rejects one.
TEST(squadize_leaves_ships_to_themselves) {
  FormerBench b;
  const ObjectId first = b.put(Point{0, 0}, NativeClass::ship);
  const ObjectId second = b.put(Point{50, 0}, NativeClass::ship);
  const ObjectId walker = b.put(Point{60, 0});

  const std::vector<SquadKey> seeds = b.squadize(1);
  REQUIRE(seeds.size() == 3);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({first}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({second}));
  CHECK(b.members(seeds[2]) == std::vector<ObjectId>({walker}));
}

/// A unit **inside a holder** belongs wherever its holder belongs -- a crew
/// moves as the ship's, a garrison as the building's -- and belongs to
/// **nothing at all** when the holder belongs to nothing, which is a refusal
/// rather than "leave it where it is".
TEST(squadize_puts_a_held_unit_in_its_holders_squad) {
  FormerBench b;
  const ObjectId ship = b.put(Point{0, 0}, NativeClass::ship);
  const ObjectId crew = b.put(Point{0, 0});
  CHECK(b.world.put_in_holder(crew, ship));

  const std::vector<SquadKey> seeds = b.squadize(1);
  // One seed: the ship's. The crew is in it and is therefore not a seed.
  REQUIRE(seeds.size() == 1);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({ship, crew}));

  // **A passenger whose carrier is in no squad is named by nothing**, and the
  // original is worse than that: 0x00446be9 returns null and 0x0044756c
  // dereferences it at `[eax+0x50]`. This engine answers "no squad" and leaves
  // the body in the fresh one the walk gave it, which is what the original
  // leaves behind too -- it never takes that squad away either. The one
  // difference is that the list does not get an entry, and neither does a
  // crash.
  FormerBench alone;
  const ObjectId cart = alone.world.spawn(NativeClass::wagon, nullptr);
  CHECK(alone.world.set_owner(cart, 1));
  const ObjectId rider = alone.put(Point{0, 0});
  CHECK(alone.world.put_in_holder(rider, cart));
  CHECK(alone.squadize(1).empty());
  CHECK(alone.heroes.squads().squad_of(rider) != kNoSquad);
  CHECK(alone.members(alone.heroes.squads().squad_of(rider)) ==
        std::vector<ObjectId>({rider}));
}

/// **The interleaving is load-bearing.** A fresh single-member squad is itself
/// a merge candidate, so the second body's search sees the first body's settled
/// squad -- which is why the walk mints one fresh squad at a time rather than
/// all of them first. Two bodies 200 apart with a third between them make the
/// difference visible: under the interleaved rule the third joins the first's
/// squad, and under a batched one it would find two candidates already formed.
TEST(squadize_settles_each_body_before_the_next_one_looks) {
  FormerBench b;
  const ObjectId first = b.put(Point{0, 0});
  const ObjectId second = b.put(Point{500, 0});    // out of the first's reach
  const ObjectId between = b.put(Point{200, 0});   // in reach of the first only

  const std::vector<SquadKey> seeds = b.squadize(1);
  REQUIRE(seeds.size() == 2);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({first, between}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({second}));
}

/// Squads do not span players, and a body nobody owns is skipped before
/// anything else -- the squad it would get would belong to nobody.
TEST(squadize_keeps_players_apart_and_skips_the_unowned) {
  FormerBench b;
  const ObjectId mine = b.put(Point{0, 0}, NativeClass::unit, 1);
  const ObjectId theirs = b.put(Point{50, 0}, NativeClass::unit, 2);
  const ObjectId nobodys = b.put(Point{60, 0}, NativeClass::unit, kNoPlayer);

  const std::vector<SquadKey> seeds = b.squadize(1);
  REQUIRE(seeds.size() == 2);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({mine}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({theirs}));
  CHECK(seeds[0].player == 1);
  CHECK(seeds[1].player == 2);
  CHECK(b.heroes.squads().squad_of(nobodys) == kNoSquad);
}

/// The list is **emptied** and its cursor rewound, not appended to: 0x00447337
/// destroys the old contents before the walk starts. `GS_CAPTURE.VS` reaches
/// both of its call sites inside a loop with the same local.
TEST(squadize_empties_the_list_it_was_handed) {
  FormerBench b;
  squadlist_pool_of(b.world).mutable_items(b.out)->push_back(SquadKey{9, 9});
  squadlist_pool_of(b.world).set_cursor(b.out, 1);
  const ObjectId only = b.put(Point{0, 0});

  const std::vector<SquadKey> seeds = b.squadize(1);
  REQUIRE(seeds.size() == 1);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({only}));
  CHECK(squadlist_pool_of(b.world).cursor(b.out) == 0);
}

/// **`Squadize` joins the bodies it was handed to each other and to nothing
/// else**, and the mechanism is the placeholder state the walk stamps on every
/// fresh squad before the regrouper looks at it.
///
/// This is the case that caught the implementation getting it wrong. Passing
/// the caller's `nState` straight into step 1 looks equivalent -- every squad
/// the list names ends up carrying it either way -- and is not, because the
/// state is one of the nine clauses the merge predicate tests. `GS_CAPTURE.VS`
/// asks for `SS_KillAll` over a map full of squads already in `SS_KillAll`.
TEST(squadize_never_joins_a_squad_that_existed_before_the_call) {
  FormerBench b;

  // A squad already standing there, in the very state the call will ask for,
  // with everything else about it matching what a fresh squad would carry.
  const ObjectId veteran = b.world.spawn(NativeClass::unit, nullptr, b.graph.find("Unit"));
  CHECK(b.world.set_position(veteran, Point{0, 0}));
  CHECK(b.world.set_owner(veteran, 1));
  CHECK(b.world.set_health(veteran, 100));
  const SquadKey standing = b.heroes.squads().create(1);
  CHECK(b.heroes.squads().join(standing, veteran));
  Squad* was = b.heroes.squads().find(standing);
  REQUIRE(was != nullptr);
  was->state = 7;

  const ObjectId recruit = b.put(Point{50, 0});
  const std::vector<SquadKey> seeds = b.squadize(7);
  REQUIRE(seeds.size() == 1);
  CHECK(seeds[0] != standing);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({recruit}));
  CHECK(b.members(standing) == std::vector<ObjectId>({veteran}));
}

/// The rest of the merge predicate, each clause measured against a squad that
/// already exists and differs in exactly one way.
///
/// The state clause above makes every pre-existing squad unreachable, so these
/// put the *placeholder* on the standing squad -- which is the one value that
/// gets past it -- and then vary one other clause at a time. It is an
/// artificial state for a squad to be in and it is the only way to reach the
/// other eight from here.
TEST(squadize_checks_destination_flags_and_kind_before_it_merges) {
  const auto joins_a_standing_squad = [](std::int32_t dest, GaikaId ai_dest,
                                         std::uint16_t flags, NativeClass native,
                                         std::string_view klass) {
    FormerBench b;
    const ObjectId veteran = b.world.spawn(NativeClass::unit, nullptr, b.graph.find("Unit"));
    (void)b.world.set_position(veteran, Point{0, 0});
    (void)b.world.set_owner(veteran, 1);
    (void)b.world.set_health(veteran, 100);
    const SquadKey standing = b.heroes.squads().create(1);
    (void)b.heroes.squads().join(standing, veteran);
    Squad* was = b.heroes.squads().find(standing);
    if (was == nullptr) return false;
    was->state = 0xffff;  // the placeholder, so the state clause lets it past
    was->dest_gaika = static_cast<GaikaId>(dest);
    was->ai_dest = ai_dest;
    was->flags = flags;
    const ObjectId recruit = b.put(Point{50, 0}, native, 1, klass);
    const std::vector<SquadKey> seeds = b.squadize(3);
    // A recruit that walked in leaves the standing squad with two members and
    // the list **empty**: a squad two bodies are in is not a seed.
    if (b.heroes.squads().squad_of(recruit) != standing) return false;
    CHECK(seeds.empty());
    CHECK(b.members(standing) == std::vector<ObjectId>({veteran, recruit}));
    return true;
  };

  // The control: everything matching, and the recruit walks in.
  CHECK(joins_a_standing_squad(kNoGaika, kNoGaika, 0, NativeClass::unit, "Unit"));
  // Nor does one whose AI destination differs, which is a separate field:
  // `squad_ai_dest` reads `ai_dest` and falls back to `dest_gaika`.
  CHECK(!joins_a_standing_squad(kNoGaika, 6, 0, NativeClass::unit, "Unit"));
  // Nor one whose flags word differs -- here `SF_PEACEFUL`, which the recruit
  // does not carry because its class is not a `Peaceful`.
  CHECK(!joins_a_standing_squad(kNoGaika, kNoGaika, kSquadFlagPeaceful, NativeClass::unit,
                                "Unit"));
  // ...and the same word from the other side: a `Peaceful` recruit derives
  // `SF_PEACEFUL` from its class and no longer matches a squad without it.
  CHECK(!joins_a_standing_squad(kNoGaika, kNoGaika, 0, NativeClass::unit, "Peaceful"));
  CHECK(joins_a_standing_squad(kNoGaika, kNoGaika, kSquadFlagPeaceful, NativeClass::unit,
                               "Peaceful"));
  // **A druid does not mix with anything that is not one**, and the test is on
  // the native class rather than on the class tree.
  CHECK(!joins_a_standing_squad(kNoGaika, kNoGaika, 0, NativeClass::druid, "Unit"));
}

/// The **destination** clause on its own, which takes some arranging to see.
///
/// Setting a candidate's `dest_gaika` moves `squad_ai_dest` with it, so the
/// obvious case is really testing the AI destination twice. Isolating it needs
/// a candidate whose two fields disagree: it is headed nowhere, and its AI
/// destination is node 4 -- which is where the recruit is headed, so every
/// other clause matches and only the destination can refuse.
TEST(squadize_will_not_merge_into_a_squad_headed_somewhere_else) {
  FormerBench b;
  const ObjectId veteran = b.world.spawn(NativeClass::unit, nullptr, b.graph.find("Unit"));
  CHECK(b.world.set_position(veteran, Point{0, 0}));
  CHECK(b.world.set_owner(veteran, 1));
  CHECK(b.world.set_health(veteran, 100));
  const SquadKey standing = b.heroes.squads().create(1);
  CHECK(b.heroes.squads().join(standing, veteran));
  {
    Squad* was = b.heroes.squads().find(standing);
    REQUIRE(was != nullptr);
    was->state = 0xffff;
    was->dest_gaika = kNoGaika;
    was->ai_dest = 4;
  }

  // The recruit is marching to node 4, which it inherits through the squad it
  // was in when the call found it.
  const ObjectId recruit = b.put(Point{50, 0});
  const SquadKey before = b.heroes.squads().create(1);
  CHECK(b.heroes.squads().join(before, recruit));
  {
    Squad* was = b.heroes.squads().find(before);
    REQUIRE(was != nullptr);
    was->dest_gaika = 4;
  }

  const std::vector<SquadKey> seeds = b.squadize(3);
  REQUIRE(seeds.size() == 1);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({recruit}));
  CHECK(b.members(standing) == std::vector<ObjectId>({veteran}));
  // ...and with the candidate pointed at node 4 as well, it walks in.
  {
    Squad* was = b.heroes.squads().find(standing);
    REQUIRE(was != nullptr);
    was->dest_gaika = 4;
  }
  {
    Squad* was = b.heroes.squads().find(b.heroes.squads().squad_of(recruit));
    REQUIRE(was != nullptr);
    was->dest_gaika = 4;
  }
  CHECK(b.squadize(3).empty());
  CHECK(b.members(standing) == std::vector<ObjectId>({veteran, recruit}));
}

/// The distance is measured to the squad's **leader** -- the front of its
/// member deque -- and not to whichever member happens to be nearest. Three
/// men in a line make the difference visible.
TEST(squadize_measures_to_the_leader_and_not_to_the_nearest_member) {
  FormerBench b;
  const ObjectId lead = b.put(Point{0, 0});
  const ObjectId middle = b.put(Point{0, 200});   // 200 from the leader: joins
  const ObjectId tail = b.put(Point{0, 400});     // 400 from the leader, 200 from `middle`

  const std::vector<SquadKey> seeds = b.squadize(1);
  REQUIRE(seeds.size() == 2);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({lead, middle}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({tail}));
}

/// **A hero keeps its own squad even when there is one it could have joined**,
/// which is what the hero arm is for. The order matters: the unit is settled
/// first, so by the time the hero is asked there is a candidate standing next
/// to it that nothing but the hero test would refuse.
TEST(squadize_leaves_a_hero_alone_even_with_a_squad_beside_it) {
  FormerBench b;
  const ObjectId walker = b.put(Point{0, 0});
  const ObjectId hero = b.put(Point{50, 0}, NativeClass::hero);
  b.heroes.register_hero(b.world, hero);

  const std::vector<SquadKey> seeds = b.squadize(2);
  REQUIRE(seeds.size() == 2);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({walker}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({hero}));
  const HeroRecord* record = b.heroes.hero(hero);
  REQUIRE(record != nullptr);
  CHECK(record->squad == seeds[1]);
}

/// And **a ship does not go looking**, which is the other half of
/// `squadize_leaves_ships_to_themselves`: that one proves nothing joins a
/// ship's squad, this one proves a ship joins nobody else's.
TEST(squadize_leaves_a_ship_alone_even_with_a_squad_beside_it) {
  FormerBench b;
  const ObjectId walker = b.put(Point{0, 0});
  const ObjectId boat = b.put(Point{50, 0}, NativeClass::ship);

  const std::vector<SquadKey> seeds = b.squadize(1);
  REQUIRE(seeds.size() == 2);
  CHECK(b.members(seeds[0]) == std::vector<ObjectId>({walker}));
  CHECK(b.members(seeds[1]) == std::vector<ObjectId>({boat}));
}

/// **`GetSquads` and the census read membership through one function**, which
/// is what stops them drifting apart -- `GETGAIKASTRAT.VS` compares their
/// answers and `SQUADMONITOR.VS` walks one and then evaluates the other.
TEST(get_squads_and_the_census_agree_about_who_is_in_a_node) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);
  f.world.players().set(0, 1, Relation::allied, true);
  f.world.players().set(1, 0, Relation::allied, true);

  constexpr GaikaId kHere = 3;
  const auto place = [&](PlayerId owner, std::size_t members, GaikaId in, GaikaId dest) {
    const SquadKey key = f.squad_of(owner, members);
    Squad* squad = f.heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad == nullptr) return key;
    squad->eval = 10;
    squad->gaika_in = in;
    squad->dest_gaika = dest;
    return key;
  };
  const SquadKey mine = place(0, 2, kHere, kNoGaika);
  (void)place(1, 1, kHere, kNoGaika);
  (void)place(0, 1, 8, 8);

  const SquadListId list = make_list(f.world);
  HostCall filled(f.world, {gaika_value(kHere), make_squadlist_value(list), Value::integer(4),
                            Value::integer(1), Value::integer(1 /*AI_OWN*/)});
  CHECK(invoke(registry, CallKind::member, "GetSquads", 4, filled).status == HostStatus::ok);
  const std::span<const SquadKey> items = squadlist_pool_of(f.world).items(list);
  REQUIRE(items.size() == 1);
  CHECK(items[0] == mine);

  // The same node, the same presence mask, the same player: the census's own
  // total counts exactly the squads that list holds.
  HostCall counted(f.world, {gaika_value(kHere), Value::integer(4), Value::integer(1),
                             Value::integer(-1), Value::integer(-1), Value::integer(-1),
                             Value::integer(-1)});
  CHECK(invoke(registry, CallKind::member, "Count", 6, counted).status == HostStatus::ok);
  CHECK(counted.arguments[3].as_integer() == 2);   // the two members of `mine`
  CHECK(counted.arguments[4].as_integer() == 1);   // and the ally, whom the list omitted
}

/// **A squad with an order is filed under the order's node**, not under its own
/// `DestGAIKA`: posting re-files it from the one to the other (0x004494c0 into
/// 0x0041ea60) and `DelOrder` files it back. So presence reads `AIDest` -- the
/// order's node while there is one -- which is what lets `SendTo` leave
/// `DestGAIKA` alone and still have the squad counted as coming.
TEST(get_squads_files_a_squad_with_an_order_under_the_orders_node) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey key = f.squad_of(0, 1);
  Squad* squad = f.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  squad->eval = 10;
  squad->gaika_in = 3;
  squad->dest_gaika = 6;
  squad->order_dest = 5;
  squad->ai_dest = 5;

  const auto listed = [&](GaikaId node, std::int32_t presence) {
    const SquadListId list = make_list(f.world);
    HostCall call(f.world, {gaika_value(node), make_squadlist_value(list), Value::integer(presence),
                            Value::integer(1), Value::integer(1 /*AI_OWN*/)});
    CHECK(invoke(registry, CallKind::member, "GetSquads", 4, call).status == HostStatus::ok);
    return squadlist_pool_of(f.world).items(list).size();
  };
  CHECK(listed(5, 1 /*AI_COMING*/) == 1);
  CHECK(listed(6, 1 /*AI_COMING*/) == 0);
  CHECK(listed(3, 2 /*AI_LEAVING*/) == 1);

  // Without the order, `AIDest` falls back to `DestGAIKA`, and so does this.
  squad->order_dest = kNoGaika;
  squad->ai_dest = kNoGaika;
  CHECK(listed(5, 1 /*AI_COMING*/) == 0);
  CHECK(listed(6, 1 /*AI_COMING*/) == 1);
}

/// **`Lock` and `Unlock` write bit 3 of the squad flags word**, on either
/// receiver shape, and that bit is the hole in the `SF_*` constant run.
TEST(squadlist_lock_writes_the_flag_bit_on_every_member) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey a = f.squad_of(1, 1);
  const SquadKey b = f.squad_of(1, 1);
  const SquadKey untouched = f.squad_of(2, 1);
  const SquadListId list = make_list(f.world);
  squadlist_pool_of(f.world).mutable_items(list)->assign({a, b});

  const auto flags = [&](SquadKey k) { return f.heroes.squads().find(k)->flags; };
  f.heroes.squads().find(a)->flags = 0x0001;  // SF_NOAI, which must survive

  HostCall lock(f.world, {make_squadlist_value(list)});
  CHECK(invoke(registry, CallKind::member, "Lock", 0, lock).status == HostStatus::ok);
  CHECK(flags(a) == (0x0001 | kSquadLocked));
  CHECK(flags(b) == kSquadLocked);
  CHECK(flags(untouched) == 0);

  HostCall unlock(f.world, {make_squadlist_value(list)});
  CHECK(invoke(registry, CallKind::member, "Unlock", 0, unlock).status == HostStatus::ok);
  CHECK(flags(a) == 0x0001);  // and only the lock bit came off
  CHECK(flags(b) == 0);

  // The same two entry points on a bare `Squad` receiver, which is what
  // `gbr.exe` registers beside the list forms.
  HostCall one(f.world, {pack_squad(b)});
  CHECK(invoke(registry, CallKind::member, "Lock", 0, one).status == HostStatus::ok);
  CHECK(flags(b) == kSquadLocked);
  CHECK(flags(a) == 0x0001);
}

/// `GetSquads` fills the caller's list by reference, filtered by where a squad
/// stands relative to the node and by how its player stands to the asker.
///
/// **Every list a shipped script fills is empty**, because nothing here assigns
/// a GAIKA to a squad -- so the fields are driven directly to show the filter
/// works, and the last case is the shipped one.
TEST(squadlist_get_squads_filters_by_node_and_by_relation) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const SquadKey staying = f.squad_of(1, 1);
  const SquadKey coming = f.squad_of(1, 1);
  const SquadKey leaving = f.squad_of(1, 1);
  const SquadKey elsewhere = f.squad_of(1, 1);
  f.heroes.squads().find(staying)->gaika_in = 4;
  f.heroes.squads().find(coming)->dest_gaika = 4;
  f.heroes.squads().find(leaving)->gaika_in = 4;
  f.heroes.squads().find(leaving)->dest_gaika = 9;
  f.heroes.squads().find(elsewhere)->gaika_in = 7;

  const SquadListId list = make_list(f.world);
  const auto fill = [&](std::int32_t presence, std::int32_t player, std::int32_t relation) {
    HostCall call(f.world, {Value::integer(4), make_squadlist_value(list),
                            Value::integer(presence), Value::integer(player),
                            Value::integer(relation)});
    CHECK(invoke(registry, CallKind::member, "GetSquads", 4, call).status == HostStatus::ok);
    const std::span<const SquadKey> items = squadlist_pool_of(f.world).items(list);
    return std::vector<SquadKey>(items.begin(), items.end());
  };

  // AI_STAYING is 4, AI_COMING 1, AI_LEAVING 2, AI_ALL 65535; AI_OWN is 1.
  //
  // **The player argument is 1-based**, so these squads -- owned by `PlayerId`
  // 1 -- are selected by the number 2. That was read as 0-based here until
  // 0x004395a5's `dec` was noticed; see `player_arg`. Nothing caught it because
  // no squad carries a node on a real map and every list comes back empty
  // either way, which is exactly why this test drives the fields directly.
  CHECK(fill(4, 2, 1) == std::vector<SquadKey>{staying});
  CHECK(fill(1, 2, 1) == std::vector<SquadKey>{coming});
  CHECK(fill(2, 2, 1) == std::vector<SquadKey>{leaving});
  CHECK(fill(65535, 2, 1) == std::vector<SquadKey>({staying, coming, leaving}));

  // Another player's squads are not this player's, whatever the presence mask.
  CHECK(fill(65535, 1, 1).empty());
  CHECK(fill(65535, 3, 1).empty());
  // And a number outside 1..16 names nobody, at either end.
  CHECK(fill(65535, 0, 1).empty());
  CHECK(fill(65535, 17, 1).empty());
  // And `AI_NONE` selects nothing rather than everything.
  CHECK(fill(65535, 2, 0).empty());

  // **Refilling rewinds.** `GS_GUARD.VS` refills the same local every pass of
  // its outer loop and walks it from the beginning each time; a cursor left
  // where the last walk ended would make every pass after the first a no-op.
  //
  // The list has to be non-empty *before* the cursor is moved, or `set_cursor`
  // clamps it to zero and the check passes for the wrong reason -- which is
  // what it did until a fault injected into the rewind survived it.
  CHECK(fill(65535, 2, 1).size() == 3);
  squadlist_pool_of(f.world).set_cursor(list, 2);
  CHECK(squadlist_pool_of(f.world).cursor(list) == 2);
  (void)fill(65535, 2, 1);
  CHECK(squadlist_pool_of(f.world).cursor(list) == 0);

  // The shipped shape, on a world where nothing has assigned a GAIKA: an empty
  // list, and a loop that runs zero times and terminates.
  const SquadKey fresh = f.squad_of(3, 2);
  (void)fresh;
  CHECK(fill(65535, 3, 1).empty());
}

/// **`SquadList::Add` is the same name on another type**, and its body lives in
/// `sim/objlist.cpp` because member lookup is case-insensitive and 27 of the 29
/// shipped `.Add` receivers are `ObjList`s. `slTrain.Add`, in `DATA\AI`, is the
/// other two.
///
/// A squad already in the list is not added twice: these are built to walk with
/// a cursor, and a duplicate would be visited twice by something with no way to
/// tell.
TEST(squadlist_add_goes_through_the_objlist_domain) {
  Fixture f;
  HostRegistry registry;
  declare_shipped_surface(registry);
  register_squad_host(registry);
  register_objlist_host(registry);

  const SquadKey a = f.squad_of(1, 1);
  const SquadKey b = f.squad_of(1, 1);
  const SquadListId list = make_list(f.world);

  const auto add = [&](Value squad) {
    HostCall call(f.world, {make_squadlist_value(list), squad});
    return invoke(registry, CallKind::member, "Add", 1, call).status;
  };
  const auto items = [&] {
    const std::span<const SquadKey> got = squadlist_pool_of(f.world).items(list);
    return std::vector<SquadKey>(got.begin(), got.end());
  };

  CHECK(add(pack_squad(a)) == HostStatus::ok);
  CHECK(items() == std::vector<SquadKey>{a});
  CHECK(add(pack_squad(b)) == HostStatus::ok);
  CHECK(items() == std::vector<SquadKey>({a, b}));

  // Twice is once.
  CHECK(add(pack_squad(a)) == HostStatus::ok);
  CHECK(items() == std::vector<SquadKey>({a, b}));

  // And the `ObjList` half of the same entry point still works, which is the
  // half a `SquadList` branch could have broken.
  const ObjListId objects = objlist_pool_of(f.world).acquire(3, 0);
  const ObjectId unit = f.spawn();
  HostCall on_objlist(f.world, {make_objlist_value(objects), obj(unit)});
  CHECK(invoke(registry, CallKind::member, "Add", 1, on_objlist).status == HostStatus::ok);
  REQUIRE(objlist_pool_of(f.world).items(objects).size() == 1);
  CHECK(objlist_pool_of(f.world).items(objects)[0] == unit);
}

/// A list is released with the script that declared it, like an `ObjList` and
/// like an array -- one teardown hook, three pools, one event.
TEST(squadlist_entries_are_released_with_their_script) {
  Fixture f;
  const SquadListId mine = make_list(f.world, 11, 0);
  const SquadListId theirs = make_list(f.world, 12, 0);
  squadlist_pool_of(f.world).mutable_items(mine)->push_back(SquadKey{1, 1});

  CHECK(squadlist_pool_of(f.world).contains(mine));
  CHECK(squadlist_pool_of(f.world).owner_of(mine) == 11);

  squadlist_pool_of(f.world).release_script(11);
  CHECK(!squadlist_pool_of(f.world).contains(mine));
  CHECK(squadlist_pool_of(f.world).contains(theirs));

  // And the freed slot is reused before the pool grows, so the handle a save
  // wrote is the index it reads back.
  CHECK(make_list(f.world, 13, 0) == mine);
}

/// The same declaration site always names the same entry, cleared -- which is
/// what bounds the pool by the program text rather than by how long a script
/// runs, and what a re-entered scope means.
TEST(squadlist_a_declaration_site_names_one_entry) {
  Fixture f;
  const SquadListId first = make_list(f.world, 5, 2);
  squadlist_pool_of(f.world).mutable_items(first)->push_back(SquadKey{3, 1});
  squadlist_pool_of(f.world).set_cursor(first, 1);

  const SquadListId again = make_list(f.world, 5, 2);
  CHECK(again == first);
  CHECK(squadlist_pool_of(f.world).items(again).empty());
  CHECK(squadlist_pool_of(f.world).cursor(again) == 0);

  // A different slot in the same script is a different entry.
  CHECK(make_list(f.world, 5, 3) != first);
}

/// `NumSquads(player)` and `GetSquad(player, n)` -- one idiom in three scripts,
/// and `GetSquad` does no lookup at all.
///
/// The original packs `((n << 4) & 0xffff) | ((player - 1) & 0xf)` and returns
/// it; whether that handle names a live squad is the `Squad::*` accessors'
/// problem, later. Both wisdom scripts walk `0 .. NumSquads-1` and let
/// `s.Leader.AsHero.IsValid` throw away the misses.
TEST(num_squads_bounds_a_walk_and_get_squad_just_packs_the_handle) {
  Fixture f;
  HostRegistry registry;
  (void)register_squad_host(registry);

  // No squads at all: the bound is zero and the walk runs zero times.
  {
    HostCall call(f.world, {Value::integer(2)});
    const HostOutcome out = invoke(registry, CallKind::free_function, "NumSquads", 1, call);
    CHECK(out.status == HostStatus::ok);
    CHECK(out.value.as_integer() == 0);
  }

  const SquadKey first = f.squad_of(1, 2);
  const SquadKey second = f.squad_of(1, 1);
  const SquadKey theirs = f.squad_of(3, 1);
  // `SquadTable` allocates the lowest free index from 1, so these are 1 and 2 --
  // which is also why `SQUADMONITOR.VS` can start its walk at 1.
  CHECK(first.index == 1);
  CHECK(second.index == 2);

  const auto count = [&](std::int32_t player) {
    HostCall call(f.world, {Value::integer(player)});
    const HostOutcome out = invoke(registry, CallKind::free_function, "NumSquads", 1, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.as_integer();
  };
  // **The bound is the highest index plus one, not the population.** Player 2
  // (1-based) owns indices 1 and 2, so the walk has to reach 2.
  CHECK(count(2) == 3);
  CHECK(count(4) == 2);   // player 3, one squad at index 1
  CHECK(count(5) == 0);   // a player with none
  CHECK(count(0) == 0);   // and out of range, where the original walks off the table
  CHECK(count(17) == 0);

  const auto get = [&](std::int32_t player, std::int32_t index) {
    HostCall call(f.world, {Value::integer(player), Value::integer(index)});
    const HostOutcome out = invoke(registry, CallKind::free_function, "GetSquad", 2, call);
    CHECK(out.status == HostStatus::ok);
    return unpack_squad(out.value);
  };
  CHECK(get(2, 1) == first);
  CHECK(get(2, 2) == second);
  CHECK(get(4, 1) == theirs);
  // **No lookup.** An index nothing owns still packs, and the handle simply
  // names no squad when something later tries to resolve it.
  CHECK(get(2, 9) == (SquadKey{9, 1}));
  CHECK(f.heroes.squads().find(get(2, 9)) == nullptr);
  // Index 0 is nobody, which `SquadKey::valid` already says.
  CHECK(!get(2, 0).valid());
  // And the twelve-bit mask the original applies to the index.
  CHECK(get(2, 0x1234).index == 0x234);
  // An out-of-range player would fold onto another player's four bits there;
  // here it names no squad instead.
  CHECK(!get(0, 1).valid());
  CHECK(!get(17, 1).valid());
}

namespace {
constexpr std::string_view kLootXml = R"(<items>
  <item id="Rusty ring" level="0" name="Rusty ring" important="no">
    <bonus health="0" damage="1" armor_slash="0" armor_pierce="0" level="0" experience="0"/>
  </item>
</items>)";
}  // namespace

/// **`TakeNearbyItems` sends a hero for the nearest holder that still holds
/// something, ahead of what it was doing.** 0x00429530's five refusals, the
/// sweep at 0x00423630, and the `SneakCommand`-shaped push.
TEST(take_nearby_items_sneaks_getitems_at_the_nearest_full_holder) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kLootXml)).ok());
  f.heroes.items().set_catalog(&catalog);
  HostRegistry registry;
  register_squad_host(registry);
  register_command_host(registry);

  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.unit_class);
  f.world.set_position(hero, Point{1000, 1000});
  f.world.set_owner(hero, 1);
  f.world.set_health(hero, 100);
  const SquadKey key = f.heroes.squads().create(1, hero);
  REQUIRE(f.heroes.squads().join(key, hero));
  const Value squad = Value::object(kTypeSquad, (static_cast<std::uint32_t>(key.player) << 16) |
                                                    static_cast<std::uint32_t>(key.index));
  const auto holder_at = [&](Point at, int rings) -> ObjectId {
    const ObjectId id = f.world.spawn(NativeClass::item_holder, nullptr, f.unit_class);
    f.world.set_position(id, at);
    for (int i = 0; i < rings; ++i) CHECK(f.heroes.items().add(f.world, id, "Rusty ring") != kNoObject);
    return id;
  };
  const auto verbs = [&](ObjectId id) {
    std::vector<std::string> out;
    const CommandQueue* queue = f.commands.find(id);
    if (queue == nullptr) return out;
    for (const Command& command : queue->entries) out.emplace_back(command.verb);
    return out;
  };
  const ObjectId far_chest = holder_at(Point{1300, 1000}, 1);
  const ObjectId near_chest = holder_at(Point{1000, 1150}, 1);
  const ObjectId empty_chest = holder_at(Point{1050, 1000}, 0);
  const auto take = [&](std::int32_t range) -> int {
    HostCall call(f.world, {squad, Value::integer(range)});
    const HostOutcome out = invoke(registry, CallKind::member, "TakeNearbyItems", 1, call);
    CHECK(out.status == HostStatus::ok);
    return out.status == HostStatus::ok ? (out.value.truthy_scalar() ? 1 : 0) : -1;
  };
  (void)f.commands.set_command(f.world, hero, "move", Command{});

  // Out of reach: nothing.
  CHECK(take(100) == 0);
  CHECK(verbs(hero) == std::vector<std::string>{"move"});
  // The nearest holder that holds something, ahead of the march, which resumes.
  CHECK(take(400) == 1);
  CHECK(verbs(hero) == (std::vector<std::string>{"getitems", "move"}));
  const CommandQueue* q = f.commands.find(hero);
  REQUIRE(q != nullptr && !q->entries.empty());
  CHECK(q->entries[0].arg_kind == CommandArgKind::object);
  CHECK(q->entries[0].object == near_chest);
  (void)far_chest;
  (void)empty_chest;
  // Already fetching: no second order.
  CHECK(take(400) == 0);
  CHECK(f.commands.command_count(hero) == 2);
  // A full hero fetches nothing: four is the fallback cap.
  (void)f.commands.set_command(f.world, hero, "move", Command{});
  for (int i = 0; i < 4; ++i) REQUIRE(f.heroes.items().add(f.world, hero, "Rusty ring") != kNoObject);
  CHECK(take(400) == 0);
  REQUIRE(f.heroes.items().remove_all_of_type(f.world, hero, "Rusty ring") == 4);
  // Nor a squad whose leader is no hero, however close the loot.
  const ObjectId footman = f.spawn(Point{1000, 1100}, 1);
  const SquadKey plain = f.heroes.squads().create(1, footman);
  REQUIRE(f.heroes.squads().join(plain, footman));
  const Value plain_squad = Value::object(kTypeSquad, (static_cast<std::uint32_t>(plain.player) << 16) |
                                                          static_cast<std::uint32_t>(plain.index));
  HostCall call(f.world, {plain_squad, Value::integer(4000)});
  const HostOutcome out = invoke(registry, CallKind::member, "TakeNearbyItems", 1, call);
  CHECK(out.status == HostStatus::ok && !out.value.truthy_scalar());
  CHECK(f.commands.command_count(footman) == 0);
  // Nor a hero whose class does not bind `getitems`: the original prints and
  // answers false without queueing.
  const ObjectId classless = f.world.spawn(NativeClass::hero, nullptr, kNoClass);
  f.world.set_position(classless, Point{1000, 1100});
  f.world.set_owner(classless, 1);
  f.world.set_health(classless, 100);
  const SquadKey unbound = f.heroes.squads().create(1, classless);
  REQUIRE(f.heroes.squads().join(unbound, classless));
  HostCall bare(f.world, {Value::object(kTypeSquad, (static_cast<std::uint32_t>(unbound.player) << 16) |
                                                         static_cast<std::uint32_t>(unbound.index)),
                          Value::integer(4000)});
  const HostOutcome nothing = invoke(registry, CallKind::member, "TakeNearbyItems", 1, bare);
  CHECK(nothing.status == HostStatus::ok && !nothing.value.truthy_scalar());
  CHECK(f.commands.command_count(classless) == 0);
  // An empty queue takes the order alone.
  (void)f.commands.forget(hero);
  CHECK(take(400) == 1);
  CHECK(verbs(hero) == std::vector<std::string>{"getitems"});
  // A squad handle naming nothing is false, not a refusal.
  HostCall none(f.world, {Value::object(kTypeSquad, (1u << 16) | 999u), Value::integer(400)});
  const HostOutcome gone = invoke(registry, CallKind::member, "TakeNearbyItems", 1, none);
  CHECK(gone.status == HostStatus::ok && !gone.value.truthy_scalar());
}

TEST(register_squad_host_defines_exactly_what_it_says_it_does) {
  HostRegistry registry;
  declare_shipped_surface(registry);
  const std::size_t before = registry.implemented();
  const std::size_t defined = register_squad_host(registry);
  CHECK(defined == squad_host_entry_count());
  CHECK(registry.implemented() - before == defined);

  // Every one of them was already in the declared surface, so nothing here
  // widened it: `kUnresolvedHost` would mean a name or arity the corpus never
  // calls.
  const auto bound = [&registry](std::string_view name, std::uint16_t arity) {
    const std::uint32_t index = registry.find(CallKind::member, name, arity);
    return index != kUnresolvedHost && registry.entry(index).fn != nullptr;
  };
  CHECK(bound("AIDest", 0));
  CHECK(bound("SetCmd", 4));
  CHECK(bound("SetCmd", 5));
  CHECK(bound("State", 0));
  CHECK(bound("StateTime", 0));
  CHECK(bound("Leader", 0));
  CHECK(bound("GAIKAIn", 0));
  CHECK(bound("ClrCmd", 3));
  CHECK(bound("DelOrder", 0));
  CHECK(bound("IsEnemyInSquadSight", 0));
  CHECK(bound("ID", 0));
  CHECK(bound("Cur", 0));
  CHECK(bound("Next", 0));
  CHECK(bound("EOL", 0));
  CHECK(bound("Lock", 0));
  CHECK(bound("Unlock", 0));
  CHECK(bound("Size", 0));
  CHECK(bound("Rewind", 0));
  CHECK(bound("GetSquads", 4));
  CHECK(bound("GetSquads", 3));
}

TEST(the_squad_slice_collides_with_no_other_domain) {
  // `define` on an existing (kind, name, arity) replaces silently, and member
  // lookup is case-insensitive. This slice was kept to three entry points for
  // exactly that reason.
  //
  // The assertion is that the three are free once **every other** domain has
  // run -- so the manifest is walked with this one filtered out, rather than
  // calling `register_all_hosts`. An earlier version did call it, and passed
  // only because the squad domain was not yet in the manifest; the moment it
  // was wired in, re-registering added nothing and the test failed for a reason
  // that had nothing to do with a collision.
  HostRegistry registry;
  declare_shipped_surface(registry);
  bool in_manifest = false;
  for (const HostDomain& domain : host_domains()) {
    if (domain.name == "squad") {
      in_manifest = true;
      continue;
    }
    domain.define(registry);
  }
  // If it is not in the manifest, nothing here is reachable in a real engine.
  CHECK(in_manifest);

  const std::size_t before = registry.implemented();
  register_squad_host(registry);
  CHECK(registry.implemented() - before == squad_host_entry_count());
}

TEST(every_squad_entry_point_refuses_a_null_user_rather_than_dereferencing) {
  // `CallContext::user` is null whenever a script runs outside an embedder.
  HostRegistry registry;
  register_squad_host(registry);

  struct Case {
    const char* name;
    std::uint16_t arity;
    std::size_t args;
    CallKind kind = CallKind::member;
  };
  constexpr Case cases[] = {
      {"State", 0, 1},      {"GAIKAIn", 0, 1},   {"Leader", 0, 1},
      {"AIDest", 0, 1},     {"SetCmd", 4, 5},    {"SetCmd", 5, 6},
      {"IsEnemyInSquadSight", 0, 1},
      {"ClrCmd", 3, 4},     {"TestFlags", 1, 2}, {"StateTime", 0, 1},
      {"TakeNearbyItems", 1, 2},
      {"DelOrder", 0, 1},   {"Cur", 0, 1},       {"Next", 0, 1},
      {"EOL", 0, 1},        {"Lock", 0, 1},      {"Unlock", 0, 1},
      {"Size", 0, 1},       {"Rewind", 0, 1},    {"GetSquads", 1, 2},
      {"GetSquads", 2, 3},  {"GetSquads", 3, 4}, {"GetSquads", 4, 5},
      {"Eval", 0, 1},       {"Count", 1, 2},     {"SrcGAIKA", 0, 1},
      {"DestGAIKA", 0, 1},  {"OrderDest", 0, 1},
      // The region census: four arities of one shape, plus its neighbour form.
      {"Eval", 6, 7},       {"Eval", 3, 4},      {"Eval", 2, 3},
      {"Count", 6, 7},      {"EvalNeighbors", 6, 7},
      // The per-class count, whose class name is the fourth argument, and the
      // garrison predicate beside it.
      {"Count", 3, 4},    {"AllEnemiesInHolder", 1, 2},
      {"InvadeThroughGate", 2, 3}, {"UseTeleport", 4, 5},
      {"CalcGoAround", 0, 1},      {"EvalAttach", 2, 3},
      {"FoodComing", 0, 1},        {"SendFoodWagon", 2, 3},
      {"NearestHospital", 1, 1, CallKind::free_function},
      {"GetAIControlledUnits", 4, 5},
      // The pair `SQUADMONITOR.VS` reads together.
      {"LastFightTime", 0, 1}, {"GetLastAttacker", 0, 1},
      // The one writer of `ai_dest`.
      {"SendTo", 2, 3},
      {"Train", 5, 6},
      // The four free functions here that do look at a world.
      {"NumSquads", 1, 1, CallKind::free_function},
      {"MilEval", 1, 1, CallKind::free_function},
      // The squad former, which needs its two collections resolved.
      {"Squadize", 3, 3, CallKind::free_function},
      {"AllyMilEval", 1, 1, CallKind::free_function},
      {"EnemyMilEval", 1, 1, CallKind::free_function},
  };
  // **`ID`, `GetSquad` and `No` are the three entry points here that never look
  // at a world**, so none can dereference a null `user` and refusing would be
  // inventing an error. `GetSquad` packs a handle out of its two arguments and
  // does not resolve it; `ID` unpacks one; `No` unpacks one and keeps the index
  // half, which is what makes it answer for a handle naming no live squad. All
  // three are asserted below instead of here, and the count is three more than
  // the sweep so that a fourth cannot join them by accident.
  CHECK(std::size(cases) + 3 == squad_host_entry_count());

  for (const Case& c : cases) {
    std::vector<Value> arguments(c.args, Value::integer(0));
    arguments[0] = pack_squad(SquadKey{1, 1});
    CallContext context;
    context.arguments = arguments;
    context.user = nullptr;
    const std::uint32_t index = registry.find(c.kind, c.name, c.arity);
    REQUIRE(index != kUnresolvedHost);
    const HostEntry& entry = registry.entry(index);
    REQUIRE(entry.fn != nullptr);
    const HostOutcome outcome = entry.fn(context);
    if (outcome.status != HostStatus::error) {
      std::printf("  %s/%u did not refuse a null user\n", c.name, c.arity);
    }
    CHECK(outcome.status == HostStatus::error);
  }

  // `GAIKA::ID` is the identity on an integer: no world, no table, no receiver
  // to resolve. It answers without one, and answers `kNoGaika` for a receiver
  // that is not an integer rather than guessing at `Item::id`, the other entry
  // point that shares this name.
  {
    std::vector<Value> arguments{Value::integer(7)};
    CallContext context;
    context.arguments = arguments;
    context.user = nullptr;
    const std::uint32_t index = registry.find(CallKind::member, "ID", 0);
    REQUIRE(index != kUnresolvedHost);
    const HostEntry& entry = registry.entry(index);
    REQUIRE(entry.fn != nullptr);
    HostOutcome outcome = entry.fn(context);
    CHECK(outcome.status == HostStatus::ok);
    CHECK(outcome.value.as_integer() == 7);

    arguments[0] = Value::integer(-3);
    context.arguments = arguments;
    outcome = entry.fn(context);
    CHECK(outcome.value.as_integer() == kNoGaika);
  }

  // `Squad::No` is the other one. 0x004210f0 answers the index half of the
  // packed handle without resolving it, so a handle naming no live squad still
  // gets its number -- which is what makes it safe to call from the debug line
  // `GS_SIEGE.VS` builds after a squad has already been wound up.
  {
    const std::uint32_t index = registry.find(CallKind::member, "No", 0);
    REQUIRE(index != kUnresolvedHost);
    const HostEntry& entry = registry.entry(index);
    REQUIRE(entry.fn != nullptr);
    const auto number = [&](Value receiver) {
      std::vector<Value> arguments{receiver};
      CallContext context;
      context.arguments = arguments;
      context.user = nullptr;
      const HostOutcome outcome = entry.fn(context);
      CHECK(outcome.status == HostStatus::ok);
      return outcome.value.as_integer();
    };
    // The index, and **not** the player: the two halves must not be confused,
    // so the pair below differs in both.
    CHECK(number(pack_squad(SquadKey{5, 3})) == 5);
    CHECK(number(pack_squad(SquadKey{3, 5})) == 3);
    // No squad, and a receiver that is not a squad value at all.
    CHECK(number(pack_squad(kNoSquad)) == 0);
    CHECK(number(Value::integer(9)) == 0);
  }
}

/// The three military evaluations are one sum read three ways.
///
/// The sum is *fielded* strength: `Squad::Eval` over a player's squads, minus
/// the ones `SF_NOAI`, `SF_PEACEFUL` or `SF_SENTRIES` takes off the board --
/// mask 0x25 at 0x00443e98.
///
/// **The owners below are 0-based and the arguments are 1-based**, which is the
/// one thing this entry point family can get wrong invisibly, so the two
/// numbering are kept visibly apart.
TEST(the_military_evaluations_sum_fielded_strength_by_relation) {
  Fixture f;
  HostRegistry registry;
  register_squad_host(registry);

  const auto band = [&](PlayerId owner, std::int32_t eval, std::uint16_t flags) {
    const SquadKey key = f.heroes.squads().create(owner);
    Squad* squad = f.heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad != nullptr) {
      squad->eval = eval;
      squad->flags = flags;
    }
    return key;
  };
  const auto ask = [&](const char* name, std::int32_t player) {
    HostCall call(f.world, {Value::integer(player)});
    const HostOutcome out =
        invoke(registry, CallKind::free_function, name, 1, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  // Slot 0 -- script player **1** -- has 100 fielded and three that are not.
  band(0, 100, 0);
  band(0, 500, kSquadFlagNoAi);
  band(0, 500, kSquadFlagPeaceful);
  band(0, 500, kSquadFlagSentries);
  band(1, 40, 0);   // script player 2
  band(2, 7, 0);    // script player 3
  band(3, 3, 0);    // script player 4

  // Slot 0 grants a ceasefire to slots 1 and 3 and nothing to slot 2, so slot 2
  // is its only enemy among the four. Slot 3 is *not* an ally either -- the
  // ally sweep's test is `!= AI_ENEMY`, so a player with no ceasefire granted
  // would be an enemy and one with a ceasefire is not; slot 3 has one.
  f.world.players().set_relation_word(0, 1, kRelationFriendly);
  f.world.players().set_relation_word(0, 3, kRelationFriendly);
  // ...and every slot from 4 up has no squads, so it contributes nothing
  // whichever side it lands on.

  // One player's own, with the three flags excluded.
  CHECK(ask("MilEval", 1) == 100);
  CHECK(ask("MilEval", 2) == 40);
  CHECK(ask("MilEval", 3) == 7);
  CHECK(ask("MilEval", 4) == 3);

  const std::int32_t ally = ask("AllyMilEval", 1);
  const std::int32_t enemy = ask("EnemyMilEval", 1);
  // Every player is on exactly one side, so the two sum to the whole board.
  CHECK(ally + enemy == 100 + 40 + 7 + 3);
  // The asker is never its own enemy, so its own strength is on the ally side,
  // and so are the two it granted a ceasefire.
  CHECK(ally == 100 + 40 + 3);
  CHECK(enemy == 7);

  // Out of range answers zero rather than refusing.
  CHECK(ask("MilEval", 0) == 0);
  CHECK(ask("MilEval", 17) == 0);
  CHECK(ask("AllyMilEval", 0) == 0);
  CHECK(ask("EnemyMilEval", -1) == 0);
}

// --------------------------------------------------------------------------
// SquadList::Train
// --------------------------------------------------------------------------

/// **One call site, and it was the sole blocker of `GS_ENTERSETTLEMENT.VS`.**
///
/// `SquadList::Train` (0x004366c0) bins every unit of every listed squad into
/// *already training*, *able to train* and *the rest*, decides from the bin
/// sizes and two thresholds who trains and who goes inside, issues the
/// commands and regroups both sets into fresh squads under the two states.
/// What is under test is each of those steps against a class graph built for
/// it: a `Warrior` that binds `train`, a `BaseMage` that binds it to nothing,
/// a `Sentry`, and a `Hall` with a `radius` for the walk-out.
namespace {

ClassGraph training_graph() {
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent="">
      <properties maxhealth="200" speed="50" sight="500" radius="15"/>
      <method sig="idle"  vs="data/subai/unit_idle.vs"/>
      <method sig="move"  vs="data/subai/unit_move.vs"/>
      <method sig="enter" vs="data/subai/unit_enter.vs"/>
    </class>)"),
            "unit.sc.xml");
  graph.add(bytes(R"(<class id="Warrior" cpp_class="CVXUnit" parent="Unit">
      <method sig="train" vs="data/subai/unit_train.vs"/>
    </class>)"),
            "warrior.sc.xml");
  graph.add(bytes(R"(<class id="BaseMage" cpp_class="CVXUnit" parent="Unit">
      <method sig="train" vs="data/subai/basemage_do_nothing.vs"/>
    </class>)"),
            "basemage.sc.xml");
  graph.add(bytes(R"(<class id="Sentry" cpp_class="CVXUnit" parent="Warrior"/>)"),
            "sentry.sc.xml");
  graph.add(bytes(R"(<class id="Animal" cpp_class="CVXUnit" parent="Unit"/>)"),
            "animal.sc.xml");
  graph.add(bytes(R"(<class id="Peaceful" cpp_class="CVXUnit" parent="Unit"/>)"),
            "peaceful.sc.xml");
  graph.add(bytes(R"(<class id="Hall" cpp_class="CVXBuilding" parent="">
      <properties maxhealth="1000" radius="64"/>
    </class>)"),
            "hall.sc.xml");
  graph.link();
  return graph;
}

struct TrainFixture {
  ClassGraph graph = training_graph();
  World world;
  HeroSystem heroes;
  CommandSystem commands;
  EnvSystem env;
  EconomySystem economy;
  HostRegistry registry;
  ClassIndex warrior = kNoClass;
  ClassIndex mage = kNoClass;
  ClassIndex sentry = kNoClass;
  ClassIndex hall = kNoClass;

  TrainFixture() {
    world.set_class_graph(&graph);
    warrior = graph.find("Warrior");
    mage = graph.find("BaseMage");
    sentry = graph.find("Sentry");
    hall = graph.find("Hall");
    world.add_system(&heroes);
    world.add_system(&commands);
    world.add_system(&env);
    world.add_system(&economy);
    register_squad_host(registry);
    // `maxtrainlevel` 5: the cap the body keeps is one less, so levels 1..3
    // train and level 4 does not.
    env.env().write_int(EnvScope::for_player(1), "maxtrainlevel", 5);
    world.advance(3000);
  }

  ObjectId spawn(ClassIndex cls, std::int32_t level = 1, std::int32_t health = 200,
                 PlayerId owner = 1) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, cls);
    world.set_position(id, Point{100, 100});
    world.set_owner(id, owner);
    world.set_health(id, health);
    heroes.register_unit(world, id);
    heroes.set_level(id, level);
    return id;
  }

  SquadKey squad(std::initializer_list<ObjectId> members, PlayerId owner = 1) {
    const SquadKey key = heroes.squads().create(owner);
    for (const ObjectId id : members) {
      (void)heroes.squads().join(key, id);
      heroes.register_unit(world, id).squad = key;
    }
    return key;
  }

  SquadListId list_of(std::initializer_list<SquadKey> keys) {
    const SquadListId list = make_list(world);
    squadlist_pool_of(world).mutable_items(list)->assign(keys.begin(), keys.end());
    return list;
  }

  std::int32_t train(SquadListId list, std::int32_t stop_on, std::int32_t start_on,
                     ObjectId target = kNoObject, std::int32_t train_state = 6,
                     std::int32_t enter_state = 7) {
    HostCall call(world, {make_squadlist_value(list), Value::integer(train_state),
                          Value::integer(enter_state), Value::integer(stop_on),
                          Value::integer(start_on),
                          target == kNoObject ? Value::object(ObjectRef{kNoType, 0}) : obj(target)});
    const HostOutcome out = invoke(registry, CallKind::member, "Train", 5, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -99;
  }

  std::string_view verb_at(ObjectId id, std::size_t index) {
    const CommandQueue* queue = commands.find(id);
    if (queue == nullptr || queue->entries.size() <= index) return "<none>";
    return queue->entries[index].verb;
  }
  const Squad* squad_holding(ObjectId id) {
    return heroes.squads().find(heroes.squads().squad_of(id));
  }
};

}  // namespace

TEST(train_answers_zero_for_no_list_and_for_an_empty_one) {
  TrainFixture f;
  HostCall bad(f.world, {Value::integer(3), Value::integer(6), Value::integer(7),
                         Value::integer(4), Value::integer(10),
                         Value::object(ObjectRef{kNoType, 0})});
  const HostOutcome out = invoke(f.registry, CallKind::member, "Train", 5, bad);
  CHECK(out.status == HostStatus::ok);
  CHECK(out.value.is_integer() && out.value.as_integer() == 0);
  CHECK(f.train(f.list_of({}), 4, 10) == 0);
}

TEST(train_starts_when_the_trainable_reach_the_start_threshold) {
  TrainFixture f;
  const ObjectId a = f.spawn(f.warrior);
  const ObjectId b = f.spawn(f.warrior, 3);
  const ObjectId c = f.spawn(f.warrior, 2);
  const SquadKey old = f.squad({a, b, c});
  const SquadListId list = f.list_of({old});

  // Three able, four wanted: nothing decided, nothing issued, nothing moved.
  CHECK(f.train(list, 4, 4) == 0);
  CHECK(f.verb_at(a, 0) == "<none>");  // no scheduler: an untouched queue is empty
  CHECK(f.heroes.squads().find(old) != nullptr);
  CHECK(f.heroes.squads().find(old)->size() == 3);

  // Three able, three wanted: started.
  CHECK(f.train(list, 4, 3) == 1);
  for (const ObjectId id : {a, b, c}) {
    CHECK(f.verb_at(id, 0) == "train");
    const Squad* fresh = f.squad_holding(id);
    REQUIRE(fresh != nullptr);
    CHECK(fresh->size() == 1);
    CHECK(fresh->state == 6);
    CHECK(fresh->state_time == f.world.time());
    CHECK(f.heroes.squad_of(id) == fresh->key);
  }
  // The squad they came from is empty and pruned.
  CHECK(f.heroes.squads().find(old) == nullptr);
}

TEST(train_bins_by_every_one_of_the_five_conditions) {
  // Each unit fails exactly one of `can_train`'s tests and so lands in the
  // rest bin, where it is sent inside rather than trained: below half health,
  // at the lessened cap, a mage, a class with no `train` binding. The one that
  // passes all five is the only trainee, and the start threshold of 1 makes
  // it visible.
  TrainFixture f;
  const ObjectId hurt = f.spawn(f.warrior, 1, 99);      // 99 * 2 < 200
  const ObjectId capped = f.spawn(f.warrior, 4);        // 4 is not below 5 - 1
  const ObjectId under = f.spawn(f.warrior, 3);         // 3 is
  const ObjectId mage = f.spawn(f.mage);
  const ObjectId plain = f.spawn(f.graph.find("Unit"));
  const ObjectId sentry = f.spawn(f.sentry);            // a Warrior, so it trains
  const ObjectId hall = f.world.spawn(NativeClass::building, nullptr, f.hall);
  const SquadListId list = f.list_of({f.squad({hurt, capped, under, mage, plain, sentry})});

  CHECK(f.train(list, 4, 1, hall) == 1);
  for (const ObjectId id : {under, sentry}) {
    CHECK(f.verb_at(id, 0) == "train");
    CHECK(f.squad_holding(id)->state == 6);
  }
  for (const ObjectId id : {hurt, capped, mage, plain}) {
    CHECK(f.verb_at(id, 0) == "enter");
    const CommandQueue* queue = f.commands.find(id);
    REQUIRE(queue != nullptr && queue->running() != nullptr);
    CHECK(queue->running()->arg_kind == CommandArgKind::object);
    CHECK(queue->running()->object == hall);
    CHECK(f.squad_holding(id)->state == 7);
    CHECK(f.squad_holding(id)->size() == 1);
  }
  // Exactly one unit at half health is not below it.
  const ObjectId half = f.spawn(f.warrior, 1, 100);
  CHECK(f.train(f.list_of({f.squad({half})}), 4, 1) == 1);
  CHECK(f.verb_at(half, 0) == "train");
}

TEST(train_reads_maxtrainlevel_from_the_players_scope_and_keeps_one_less) {
  TrainFixture f;
  const ObjectId one = f.spawn(f.warrior, 1);
  const SquadListId list = f.list_of({f.squad({one})});
  // 2 keeps a cap of 1, and level 1 is not below it.
  f.env.env().write_int(EnvScope::for_player(1), "maxtrainlevel", 2);
  CHECK(f.train(list, 4, 1) == 4);  // sent inside instead
  CHECK(f.verb_at(one, 0) == "enter");
  // 3 keeps 2, and level 1 is below it.
  f.env.env().write_int(EnvScope::for_player(1), "maxtrainlevel", 3);
  const ObjectId two = f.spawn(f.warrior, 1);
  CHECK(f.train(f.list_of({f.squad({two})}), 4, 1) == 1);
  CHECK(f.verb_at(two, 0) == "train");
  // No value at all is 0, a cap of -1, and nobody trains.
  const ObjectId theirs = f.spawn(f.warrior, 1, 200, 2);
  CHECK(f.train(f.list_of({f.squad({theirs}, 2)}), 4, 1) == 4);
  CHECK(f.verb_at(theirs, 0) == "enter");
}

TEST(train_counts_the_train_family_of_verbs_as_already_training) {
  // `train`, `waitarmytrain` and `unittrain`, anywhere in the queue: with one
  // already training and one able, the total of two is not above a stop
  // threshold of 4, so training stops -- the answer is 3 and both go inside.
  TrainFixture f;
  for (const char* verb : {"train", "waitarmytrain", "unittrain"}) {
    const ObjectId busy = f.spawn(f.warrior);
    const Command none;
    (void)f.commands.add_command(f.world, busy, false, verb, none);
    const ObjectId able = f.spawn(f.warrior);
    const SquadListId list = f.list_of({f.squad({busy, able})});
    CHECK(f.train(list, 4, 10) == 3);
    CHECK(f.verb_at(busy, 0) == "enter");
    CHECK(f.verb_at(able, 0) == "enter");
    CHECK(f.squad_holding(busy)->state == 7);
    CHECK(f.squad_holding(able)->state == 7);
  }
}

TEST(train_sees_a_train_command_anywhere_in_the_queue_and_stops_on_equality) {
  // `is_training` walks the whole queue: a `train` queued behind a `move` is
  // still training. And the stop test is strict -- one training plus three
  // able is four, which is not above a stop threshold of four, so training
  // stops rather than continues.
  TrainFixture f;
  const ObjectId busy = f.spawn(f.warrior);
  const Command none;
  (void)f.commands.add_command(f.world, busy, false, "move", none);
  (void)f.commands.add_command(f.world, busy, false, "train", none);
  const ObjectId a = f.spawn(f.warrior);
  const ObjectId b = f.spawn(f.warrior);
  const ObjectId c = f.spawn(f.warrior);
  CHECK(f.train(f.list_of({f.squad({busy, a, b, c})}), 4, 10) == 3);
  for (const ObjectId id : {busy, a, b, c}) CHECK(f.verb_at(id, 0) == "enter");
}

TEST(train_replaces_a_trainees_queue_rather_than_appending) {
  // 0x004291d0 aborts the queue before it orders `train`, so a pending order
  // is gone afterwards and `train` runs, not waits.
  TrainFixture f;
  const ObjectId a = f.spawn(f.warrior);
  Command pending;
  pending.arg_kind = CommandArgKind::point;
  pending.point = Point{1, 2};
  (void)f.commands.add_command(f.world, a, false, "move", pending);
  CHECK(f.train(f.list_of({f.squad({a})}), 4, 1) == 1);
  CHECK(f.verb_at(a, 0) == "train");
  CHECK(f.verb_at(a, 1) == "<none>");
}

TEST(train_continues_above_the_stop_threshold_and_leaves_the_trainers_alone) {
  TrainFixture f;
  std::vector<ObjectId> busy;
  for (int i = 0; i < 3; ++i) {
    const ObjectId id = f.spawn(f.warrior);
    const Command none;
    (void)f.commands.add_command(f.world, id, false, "train", none);
    busy.push_back(id);
  }
  const ObjectId a = f.spawn(f.warrior);
  const ObjectId b = f.spawn(f.warrior);
  const SquadKey old = f.squad({busy[0], busy[1], busy[2], a, b});
  const SquadListId list = f.list_of({old});

  // 3 training + 2 able = 5, above 4: continue, the two join.
  CHECK(f.train(list, 4, 10) == 2);
  CHECK(f.verb_at(a, 0) == "train");
  CHECK(f.verb_at(b, 0) == "train");
  CHECK(f.squad_holding(a)->state == 6);
  // The three already training kept their queue and their squad.
  for (const ObjectId id : busy) {
    CHECK(f.verb_at(id, 0) == "train");
    CHECK(f.verb_at(id, 1) == "<none>");
    CHECK(f.heroes.squads().squad_of(id) == old);
  }
  CHECK(f.heroes.squads().find(old)->size() == 3);

  // Above the threshold with nobody able: nothing decided, nothing done.
  const ObjectId c = f.spawn(f.warrior);
  const Command none;
  (void)f.commands.add_command(f.world, c, false, "train", none);
  const SquadKey solo = f.squad({c});
  CHECK(f.train(f.list_of({solo}), 0, 10) == 0);
  CHECK(f.heroes.squads().squad_of(c) == solo);
}

TEST(train_sends_the_rest_inside_and_answers_four_only_when_nothing_else_happened) {
  TrainFixture f;
  const ObjectId mage = f.spawn(f.mage);
  const ObjectId held = f.spawn(f.mage);
  const ObjectId entering = f.spawn(f.mage);
  const ObjectId hall = f.world.spawn(NativeClass::building, nullptr, f.hall);
  const ObjectId box = f.world.spawn_settlement(1).holder;
  REQUIRE(f.world.put_in_holder(held, box));
  Command enter;
  enter.arg_kind = CommandArgKind::object;
  enter.object = hall;
  (void)f.commands.set_command(f.world, entering, "enter", enter);
  const std::uint32_t before = f.commands.find(entering)->running()->id;

  CHECK(f.train(f.list_of({f.squad({mage, held, entering})}), 4, 10, hall) == 4);
  CHECK(f.verb_at(mage, 0) == "enter");
  // Inside a holder: not ordered, but regrouped like the others.
  CHECK(f.verb_at(held, 0) == "<none>");
  CHECK(f.squad_holding(held)->state == 7);
  // Already entering: the order it had is the order it keeps.
  CHECK(f.commands.find(entering)->running()->id == before);

  // Nothing to order and nobody to train is a plain 0.
  const ObjectId still = f.spawn(f.mage);
  REQUIRE(f.world.put_in_holder(still, box));
  CHECK(f.train(f.list_of({f.squad({still})}), 4, 10, hall) == 0);
}

TEST(train_skips_no_ai_squads_and_handles_that_name_nothing) {
  TrainFixture f;
  const ObjectId a = f.spawn(f.warrior);
  const SquadKey off = f.squad({a});
  f.heroes.squads().find(off)->flags = kSquadFlagNoAi;
  const ObjectId b = f.spawn(f.warrior);
  const SquadKey on = f.squad({b});
  (void)f.heroes.squads().join(on, 9999);  // a member that resolves to nothing

  CHECK(f.train(f.list_of({off, on}), 4, 1) == 1);
  CHECK(f.verb_at(a, 0) == "<none>");
  CHECK(f.heroes.squads().squad_of(a) == off);
  CHECK(f.verb_at(b, 0) == "train");
}

TEST(train_detaches_a_trainee_from_its_hero_and_leaves_the_rest_attached) {
  TrainFixture f;
  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.warrior);
  f.world.set_owner(hero, 1);
  f.world.set_health(hero, 200);
  f.heroes.register_hero(f.world, hero);
  f.heroes.set_level(hero, 6);  // past the cap: the hero itself does not train
  const ObjectId warrior = f.spawn(f.warrior);
  const ObjectId mage = f.spawn(f.mage);
  REQUIRE(f.heroes.attach(f.world, warrior, hero));
  REQUIRE(f.heroes.attach(f.world, mage, hero));
  const SquadKey army = f.heroes.hero(hero)->squad;
  REQUIRE(f.heroes.squads().find(army)->size() == 3);

  CHECK(f.train(f.list_of({army}), 4, 1) == 1);
  // The trainee left the army and the squad, and trains in one of its own.
  CHECK(f.heroes.hero_of(warrior) == kNoObject);
  CHECK(f.verb_at(warrior, 0) == "train");
  CHECK(f.squad_holding(warrior)->key != army);
  CHECK(f.squad_holding(warrior)->state == 6);
  // The mage was sent inside and stays attached, in the hero's squad.
  CHECK(f.verb_at(mage, 0) == "enter");
  CHECK(f.heroes.hero_of(mage) == hero);
  CHECK(f.heroes.squads().squad_of(mage) == army);
  // The hero, in the rest bin, keeps its squad; the state lands on it.
  CHECK(f.heroes.hero(hero)->squad == army);
  CHECK(f.heroes.squads().find(army)->state == 7);
  CHECK(f.heroes.squads().find(army)->size() == 2);
}

TEST(train_regroups_with_the_old_squads_fields_or_the_class_defaults) {
  TrainFixture f;
  const ObjectId a = f.spawn(f.warrior);
  const SquadKey old = f.squad({a});
  Squad* squad = f.heroes.squads().find(old);
  squad->flags = static_cast<std::uint16_t>(kSquadFlagPeaceful | kSquadLocked);
  squad->dest_gaika = 5;
  squad->src_gaika = 3;
  squad->last_fight_time = 1234;
  // A sentry with no squad at all, and a no-AI warrior likewise.
  const ObjectId sentry = f.spawn(f.sentry);
  const ObjectId quiet = f.spawn(f.warrior);
  f.world.find(quiet)->state.flags.no_ai = true;
  const SquadKey holder = f.squad({sentry, quiet});
  f.heroes.squads().leave(holder, sentry);
  f.heroes.squads().leave(holder, quiet);
  f.heroes.squads().join(old, sentry);
  f.heroes.squads().join(old, quiet);

  CHECK(f.train(f.list_of({old}), 4, 1) == 1);
  const Squad* fresh = f.squad_holding(a);
  REQUIRE(fresh != nullptr);
  // Copied: peaceful, dest, src, last fight. Rewritten: bit 0 from the unit's
  // own no-AI flag (clear), and the lock bit dropped.
  CHECK(fresh->flags == kSquadFlagPeaceful);
  CHECK(fresh->dest_gaika == 5);
  CHECK(fresh->src_gaika == 3);
  CHECK(fresh->last_fight_time == 1234);
  // They all came from `old`, so the sentry inherits its flags too -- the
  // class default only applies to a unit with no squad, tested below.
  CHECK(f.squad_holding(sentry)->flags == kSquadFlagPeaceful);
  CHECK(f.squad_holding(quiet)->flags == (kSquadFlagPeaceful | kSquadFlagNoAi));
}

TEST(regroup_defaults_come_from_the_class_when_there_was_no_squad) {
  // Unreachable through `Train` -- every unit it sees came out of a listed
  // squad -- so the branch is exercised on the regroup directly, which is
  // 0x00447330's own entry and what `Squadize` will build on.
  TrainFixture f;
  const ObjectId sentry = f.spawn(f.sentry);
  const ObjectId hen = f.spawn(f.graph.find("Animal"));
  const ObjectId monk = f.spawn(f.graph.find("Peaceful"));
  const ObjectId quiet = f.spawn(f.warrior);
  f.world.find(quiet)->state.flags.no_ai = true;
  const ObjectId plain = f.spawn(f.warrior);
  const ObjectId units[] = {sentry, hen, monk, quiet, plain};
  regroup_into_fresh_squads(f.world, f.heroes, units, 9, f.world.time());

  CHECK(f.squad_holding(sentry)->flags == kSquadFlagSentries);
  CHECK(f.squad_holding(hen)->flags == kSquadFlagPeaceful);
  CHECK(f.squad_holding(monk)->flags == kSquadFlagPeaceful);
  CHECK(f.squad_holding(quiet)->flags == kSquadFlagNoAi);
  CHECK(f.squad_holding(plain)->flags == 0);
  for (const ObjectId id : units) {
    const Squad* fresh = f.squad_holding(id);
    REQUIRE(fresh != nullptr);
    CHECK(fresh->size() == 1);
    CHECK(fresh->state == 9);
    CHECK(fresh->state_time == f.world.time());
    CHECK(fresh->dest_gaika == kNoGaika);  // no node table under this world
    CHECK(fresh->src_gaika == kNoGaika);
    CHECK(fresh->last_fight_time == 0);
    CHECK(f.heroes.squad_of(id) == fresh->key);
  }
}

TEST(train_walks_a_garrisoned_trainee_out_to_a_drawn_point_first) {
  TrainFixture f;
  const World::SettlementIds ids = f.world.spawn_settlement(1);
  const ObjectId hall = f.world.spawn(NativeClass::building, nullptr, f.hall);
  f.world.set_position(hall, Point{500, 800});
  f.world.set_owner(hall, 1);
  SettlementInit init;
  init.settlement_object = ids.settlement;
  init.holder_object = ids.holder;
  init.warehouse_object = ids.warehouse;
  init.anchor = hall;
  init.owner = 1;
  init.kind = SettlementKind::stronghold;
  REQUIRE(f.economy.settlements().create(init) != kNoSettlement);
  const ObjectId inside = f.spawn(f.warrior);
  REQUIRE(f.world.put_in_holder(inside, ids.holder));
  const ObjectId outside = f.spawn(f.warrior);

  // Over a run of seeds, because the two draws are `rand(64, 128)` and
  // `rand(0, 359)` inclusive and a range off by one at either end agrees with
  // the right one on many seeds and not on all of them.
  for (std::uint32_t seed = 1; seed <= 24; ++seed) {
    f.world.rng().seed(seed);
    Rng model = f.world.rng();
    const std::int32_t reach = model.between(64, 128);
    const std::int32_t angle = model.between(0, 359);
    const Point offset = vec_of_angle(64 + reach, angle);
    const Point expected{500 + offset.x, 800 + offset.y};

    (void)f.commands.forget(inside);
    (void)f.commands.forget(outside);
    REQUIRE(f.world.put_in_holder(inside, ids.holder));
    CHECK(f.train(f.list_of({f.squad({inside, outside})}), 4, 2) == 1);
    // The held one: `move` to the drawn point, then `train`; two draws exactly.
    CHECK(f.verb_at(inside, 0) == "move");
    const CommandQueue* queue = f.commands.find(inside);
    REQUIRE(queue != nullptr && queue->running() != nullptr);
    CHECK(queue->running()->arg_kind == CommandArgKind::point);
    CHECK(queue->running()->point == expected);
    CHECK(f.verb_at(inside, 1) == "train");
    CHECK(f.world.rng().state() == model.state());
    // The one outside goes straight to `train` and draws nothing.
    CHECK(f.verb_at(outside, 0) == "train");
    CHECK(f.verb_at(outside, 1) == "<none>");
  }
}

// -- squads at placement -----------------------------------------------------
//
// 0x0041e820: while the AI manager exists, a unit that enters the world goes
// into a squad. `HeroSystem::advance` runs `enrol_new_units_in_squads` once a
// turn over the ids it has not seen.

TEST(units_placed_while_the_manager_exists_join_squads_with_their_class_flags) {
  TrainFixture f;
  AiSystem ai;
  f.world.add_system(&ai);

  // Before the manager exists, nothing is enrolled -- an editor placement, a
  // conformance run with no AI, and the original alike.
  const ObjectId early = f.spawn(f.warrior);
  f.world.advance(1000);
  CHECK(f.heroes.squads().squad_of(early) == kNoSquad);

  ai.start_manager();
  const ObjectId soldier = f.spawn(f.warrior);
  const ObjectId hen = f.spawn(f.graph.find("Animal"));
  const ObjectId quiet = f.spawn(f.warrior);
  f.world.find(quiet)->state.flags.no_ai = true;
  // A dead one, an unspawned one and an ownerless one stay out (the hook's
  // three tests after the manager's).
  const ObjectId corpse = f.spawn(f.warrior, 1, 0);
  const ObjectId ghost = f.spawn(f.warrior);
  f.world.find(ghost)->state.flags.unspawned = true;
  const ObjectId stray = f.spawn(f.warrior);
  f.world.set_owner(stray, kNoPlayer);
  // One that already has a squad keeps it.
  const ObjectId filed = f.spawn(f.warrior);
  const SquadKey own = f.squad({filed});

  f.world.advance(1000);
  // `early` was placed before the manager and **is** picked up once it exists:
  // the watermark does not move while there is no manager, which is how the
  // units a map placed before the match bootstrap -- every deer in the
  // reference dumps -- end up filed. Two soldiers at one point with the same
  // flags share a squad (`add_to_squad`'s merge arm), whose Leader is its
  // front member.
  REQUIRE(f.squad_holding(early) != nullptr);
  REQUIRE(f.squad_holding(soldier) != nullptr);
  CHECK(f.heroes.squads().squad_of(soldier) == f.heroes.squads().squad_of(early));
  CHECK(f.squad_holding(soldier)->flags == 0);
  REQUIRE(f.squad_holding(hen) != nullptr);
  CHECK(f.squad_holding(hen)->flags == kSquadFlagPeaceful);
  REQUIRE(f.squad_holding(quiet) != nullptr);
  CHECK(f.squad_holding(quiet)->flags == kSquadFlagNoAi);
  CHECK(f.heroes.squads().squad_of(corpse) == kNoSquad);
  CHECK(f.heroes.squads().squad_of(ghost) == kNoSquad);
  CHECK(f.heroes.squads().squad_of(stray) == kNoSquad);
  CHECK(f.heroes.squads().squad_of(filed) == own);
  HostCall call(f.world, {pack_squad(f.heroes.squads().squad_of(soldier))});
  const HostOutcome leader = invoke(f.registry, CallKind::member, "Leader", 0, call);
  CHECK(leader.status == HostStatus::ok);
  CHECK(leader.value.is_object() &&
        leader.value.as_object().id == f.squad_holding(soldier)->members.front());

  // A squad the unit already had is left as it was -- not re-homed through
  // `add_to_squad`, which would restamp its state.
  CHECK(f.heroes.squads().find(own)->state == 0);
  CHECK(f.heroes.squads().find(own)->dest_gaika == kNoGaika);

  // A building is not a unit and gets no squad, manager or not.
  const ObjectId tower = f.world.spawn(NativeClass::building, nullptr, f.hall);
  f.world.set_owner(tower, 1);
  f.world.set_health(tower, 100);
  f.world.advance(1000);
  CHECK(f.heroes.squads().squad_of(tower) == kNoSquad);

  // A later placement is enrolled on the next turn, and then left alone:
  // once it has been seen, leaving its squad does not put it back in one.
  // 0x0041e820 fires when a unit *enters the world*, and nothing else.
  const ObjectId late = f.spawn(f.warrior, 1, 200, 2);
  f.world.advance(1000);
  REQUIRE(f.squad_holding(late) != nullptr);
  const SquadKey first = f.heroes.squads().squad_of(late);
  CHECK(first != f.heroes.squads().squad_of(soldier));  // another owner's
  f.world.advance(1000);
  CHECK(f.heroes.squads().squad_of(late) == first);
  REQUIRE(f.heroes.squads().leave(first, late));
  f.world.advance(1000);
  CHECK(f.heroes.squads().squad_of(late) == kNoSquad);
}

TEST(a_unit_enrolled_at_placement_is_filed_under_the_node_it_stands_in) {
  TrainFixture f;
  AiSystem ai;
  f.world.add_system(&ai);
  // One settlement, so the node table has a node with a place in it and the
  // rest of the map is another. Node 1 is the settlement's (store order).
  const ObjectId hall = f.world.spawn(NativeClass::building, nullptr, f.hall);
  f.world.set_position(hall, Point{100, 100});
  f.world.set_owner(hall, 1);
  SettlementInit init;
  init.settlement_object = hall;
  init.anchor = hall;
  init.owner = 1;
  init.kind = SettlementKind::stronghold;
  (void)f.economy.settlements().create(init);
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  const GaikaId here = f.world.gaika().at(f.world.lsa(), Point{100, 100});
  REQUIRE(here != kNoGaika);

  ai.start_manager();
  const ObjectId soldier = f.spawn(f.warrior);  // at {100, 100}
  // One already filed under no node at all is not re-homed to the node it
  // stands in: it did not enter the world this turn.
  const ObjectId filed = f.spawn(f.warrior);
  const SquadKey own = f.squad({filed});
  f.world.advance(1000);
  const Squad* squad = f.squad_holding(soldier);
  REQUIRE(squad != nullptr);
  CHECK(squad->dest_gaika == here);
  CHECK(f.heroes.squads().squad_of(filed) == own);
  CHECK(f.heroes.squads().find(own)->dest_gaika == kNoGaika);
}

/// A garrisoned unit regrouped with no squad to copy a destination from is
/// sent to **its own town's node**: the regroup 0x00447330 asks 0x0044e650 (at
/// 0x00447495), which for a held unit reads its `posRH`, the town's central
/// building. It asked the holder walk for a while, which ends on the holder
/// record at (0, 0) -- the node nearest the map's corner, here another town's.
///
/// (Enrolment at placement, 0x0041e820, asks the same at 0x0041e877, and reads
/// it the same way now; but a held unit whose holder is in no squad gets no
/// squad there at all -- `add_to_squad`'s step 3 -- so no test can see it.)
TEST(a_garrisoned_unit_regrouped_from_no_squad_is_sent_to_its_towns_node) {
  TrainFixture f;
  const auto town = [&](Point at) {
    const World::SettlementIds ids = f.world.spawn_settlement(1);
    const ObjectId hall = f.world.spawn(NativeClass::building, nullptr, f.hall);
    f.world.set_position(hall, at);
    f.world.set_owner(hall, 1);
    SettlementInit init;
    init.settlement_object = ids.settlement;
    init.holder_object = ids.holder;
    init.warehouse_object = ids.warehouse;
    init.anchor = hall;
    init.owner = 1;
    init.kind = SettlementKind::stronghold;
    (void)f.economy.settlements().create(init);
    return ids.holder;
  };
  (void)town(Point{100, 100});  // the corner's town
  const ObjectId far_holder = town(Point{20'000, 20'000});
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  const GaikaId corner = f.world.gaika().at(f.world.lsa(), Point{0, 0});
  const GaikaId far_town = f.world.gaika().at(f.world.lsa(), Point{20'000, 20'000});
  REQUIRE(corner != kNoGaika);
  REQUIRE(far_town != kNoGaika);
  REQUIRE(corner != far_town);

  const ObjectId loose = f.spawn(f.warrior);
  REQUIRE(f.world.put_in_holder(loose, far_holder));
  REQUIRE(f.world.resolve_position(loose) == (Point{0, 0}));
  const ObjectId units[] = {loose};
  regroup_into_fresh_squads(f.world, f.heroes, units, 9, f.world.time());
  REQUIRE(f.squad_holding(loose) != nullptr);
  CHECK(f.squad_holding(loose)->dest_gaika == far_town);
}
