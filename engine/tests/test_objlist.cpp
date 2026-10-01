// `ObjList` -- the pool, the host entry points, and the lifetime rule.
//
// `count` is the second most used member in the whole host API (541 sites) and
// `ol[i]` is how nearly every script reaches an object it was not handed, so
// this file leans hard on two things the project has been bitten by before.
//
//   * **Receivers are asserted, always.** A compiler defect once corrupted every
//     nested call's receiver while all 577 scripts still compiled, because stub
//     host functions ignore what they are called on. `Clear/0` and `Add/1` are
//     shared with other types here, so every entry point is called with a wrong
//     receiver as well as a right one.
//   * **A null `CallContext::user` must refuse, not dereference.** Every entry
//     point this domain defines is called with `user == nullptr` in one loop.
//
// Synthetic throughout: nothing here needs a byte of game data.

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/bytecode.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace script = imperivm::core::script;

using imperivm::core::NativeClass;

namespace {

constexpr Point pt(std::int32_t x, std::int32_t y) noexcept { return Point{x, y}; }

/// A world, a registry with this domain (and the object model it sits on)
/// defined, and the `HostContext` every host function is reached through.
struct Fixture {
  World world;
  script::HostRegistry registry;
  HostContext context;

  Fixture() {
    script::declare_shipped_surface(registry);
    // Order matters and is the same order sim/host_setup.hpp fixes: the object
    // model first, then this domain, which takes `count/0` over for both of its
    // receivers.
    (void)register_world_host(registry);
    (void)register_objlist_host(registry);
    context.world = &world;
  }

  /// Call an entry point. `args` is the whole window, receiver first for a
  /// member, and is passed by reference so a test can read an out-parameter
  /// back -- which is how a lazily bound `ObjList` handle gets home.
  script::HostOutcome call_window(script::CallKind kind, std::string_view name,
                                  std::vector<script::Value>& args, bool with_world = true) {
    const std::uint16_t arity =
        static_cast<std::uint16_t>(args.size() - (kind == script::CallKind::member ? 1 : 0));
    const std::uint32_t index = registry.find(kind, name, arity);
    // Not REQUIRE: this is not a `void` test body, and a missing entry point is
    // reported by the caller's CHECK on the outcome.
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = with_world ? &context : nullptr;
    ctx.name = name;
    ctx.kind = kind;
    ctx.script = 7;  // any live script; the pool keys temporaries on it
    return entry.fn(ctx);
  }

  script::HostOutcome call(script::CallKind kind, std::string_view name,
                           std::vector<script::Value> args) {
    return call_window(kind, name, args, true);
  }

  [[nodiscard]] script::Value obj(ObjectId id) const {
    return script::Value::object(script::ObjectRef{kTypeObj, id});
  }
};

/// Parse and compile one script into `scheduler`, or `kNoChunk` with the reason
/// printed. The reason matters: a silently unbuilt chunk would make the two
/// scheduler tests below pass by never running anything.
std::uint32_t build(script::Scheduler& scheduler, const script::HostRegistry& registry,
                    std::string_view source, const char* name) {
  const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(source.data()),
                                         source.size()};
  script::Diagnostic diagnostic;
  const auto parsed = script::parse(bytes, name, &diagnostic);
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

/// A bound, empty list owned by script 7 -- the state `ObjList ol;` reaches
/// after its first mutating call.
script::Value fresh_list(World& world) {
  return make_objlist_value(objlist_pool_of(world).acquire_temporary(7));
}

}  // namespace

// --------------------------------------------------------------------------
// the pool
// --------------------------------------------------------------------------

TEST(objlist_pool_keys_an_entry_by_declaration_site) {
  ObjListPool pool;
  const ObjListId first = pool.acquire(1, 3);
  CHECK(first != kNoObjList);
  pool.mutable_items(first)->push_back(42);
  CHECK(pool.items(first).size() == 1);

  // The case `TS_CARTHAGETACTIC.VS` forces: `ObjList ol;` inside a loop body.
  // Re-declaring reuses the same entry and clears it, so a tactic script that
  // runs for the whole match holds one list, not one per iteration.
  const ObjListId again = pool.acquire(1, 3);
  CHECK(again == first);
  CHECK(pool.items(again).empty());
  CHECK(pool.size() == 1);

  // A different slot, and the same slot in a different script, are different
  // lists.
  CHECK(pool.acquire(1, 4) != first);
  CHECK(pool.acquire(2, 3) != first);
  CHECK(pool.size() == 3);
}

TEST(objlist_pool_temporaries_are_distinct_and_never_zero) {
  ObjListPool pool;
  const ObjListId a = pool.acquire_temporary(1);
  const ObjListId b = pool.acquire_temporary(1);
  CHECK(a != kNoObjList);
  CHECK(b != kNoObjList);
  CHECK(a != b);
  // A temporary must not collide with a declaration. The pool reserves
  // `0xFFFFFFFF` for temporaries and relies on no local slot index reaching it,
  // which a chunk with four billion locals would have to do; every index a real
  // program can produce is distinct from both.
  CHECK(pool.acquire(1, 0xFFFFFFFEu) != a);
  CHECK(pool.acquire(1, 0xFFFFFFFEu) != b);
  CHECK(pool.acquire(1, 0u) != a);
}

TEST(objlist_pool_release_frees_a_scripts_lists_and_reuses_the_slots) {
  ObjListPool pool;
  const ObjListId mine = pool.acquire(1, 0);
  const ObjListId theirs = pool.acquire(2, 0);
  pool.mutable_items(mine)->push_back(9);
  CHECK(pool.size() == 2);

  pool.release_script(1);
  CHECK(pool.size() == 1);
  CHECK(!pool.contains(mine));
  CHECK(pool.contains(theirs));
  // A stale handle reads as an empty list rather than trapping: `ol.count` on
  // one is zero, which is what the header promises and what keeps a script that
  // outlived its list from dying.
  CHECK(pool.items(mine).empty());
  CHECK(pool.mutable_items(mine) == nullptr);

  // Handle allocation is reproducible: a dead slot is reused before the vector
  // grows, so the same sequence of acquisitions always yields the same handles.
  // A suspended script's local slot holds one of these, and suspended scripts
  // are world state.
  CHECK(pool.acquire(3, 0) == mine);
}

TEST(objlist_pool_allocation_is_reproducible) {
  const auto run = [] {
    ObjListPool pool;
    std::vector<ObjListId> issued;
    for (script::ScriptId s = 1; s <= 4; ++s) {
      for (std::uint32_t slot = 0; slot < 3; ++slot) issued.push_back(pool.acquire(s, slot));
      issued.push_back(pool.acquire_temporary(s));
    }
    pool.release_script(2);
    for (std::uint32_t slot = 0; slot < 3; ++slot) issued.push_back(pool.acquire(9, slot));
    return issued;
  };
  CHECK(run() == run());
}

// --------------------------------------------------------------------------
// registration
// --------------------------------------------------------------------------

TEST(objlist_registration_lands_on_the_declared_surface) {
  Fixture f;
  const auto found = [&](script::CallKind kind, const char* name, std::uint16_t arity) {
    const std::uint32_t index = f.registry.find(kind, name, arity);
    return index != script::kUnresolvedHost && f.registry.entry(index).fn != nullptr;
  };
  CHECK(found(script::CallKind::member, "count", 0));       // 541
  CHECK(found(script::CallKind::member, "ClearDead", 0));   // 173
  CHECK(found(script::CallKind::member, "GetObjList", 0));  // 103
  CHECK(found(script::CallKind::member, "Add", 1));         // 83
  CHECK(found(script::CallKind::member, "AddList", 1));     // 67
  CHECK(found(script::CallKind::member, "Clear", 0));       // 34
  CHECK(found(script::CallKind::member, "RemoveList", 1));  // 29
  CHECK(found(script::CallKind::member, "Remove", 1));      // 17
  CHECK(found(script::CallKind::member, "Contains", 1));    // 12
  CHECK(found(script::CallKind::member, "FilterClosest", 2));
  CHECK(found(script::CallKind::member, "ObjEnemy", 1));
  CHECK(found(script::CallKind::member, "ObjAlly", 1));
  CHECK(found(script::CallKind::member, "ObjClass", 1));
  CHECK(found(script::CallKind::member, "ObjInjured", 0));
  CHECK(found(script::CallKind::free_function, "IdxToSet", 1));
  CHECK(found(script::CallKind::free_function, "MaxSetIdx", 0));
  // `sel.Count()` in DEBUG_DUMP.VS is the same entry as `ol.count`: member
  // lookup is case-insensitive and parentheses are optional.
  CHECK(f.registry.find(script::CallKind::member, "Count", 0) ==
        f.registry.find(script::CallKind::member, "count", 0));
}

TEST(objlist_every_entry_point_refuses_a_null_world) {
  Fixture f;
  const script::Value list = fresh_list(f.world);
  const script::Value one = script::Value::integer(1);
  const script::Value point = pack_point(pt(0, 0));

  struct Case {
    script::CallKind kind;
    const char* name;
    std::vector<script::Value> args;
  };
  const Case cases[] = {
      {script::CallKind::member, "count", {list}},
      {script::CallKind::member, "ClearDead", {list}},
      {script::CallKind::member, "GetObjList", {list}},
      {script::CallKind::member, "Add", {list, one}},
      {script::CallKind::member, "AddList", {list, list}},
      {script::CallKind::member, "Clear", {list}},
      {script::CallKind::member, "RemoveList", {list, list}},
      {script::CallKind::member, "Remove", {list, one}},
      {script::CallKind::member, "Contains", {list, one}},
      {script::CallKind::member, "FilterClosest", {list, point, one}},
      {script::CallKind::free_function, "IdxToSet", {one}},
      {script::CallKind::free_function, "MaxSetIdx", {}},
  };
  for (const Case& c : cases) {
    std::vector<script::Value> args = c.args;
    // Null `user` is a real case, not a defensive check: it must produce
    // `HostOutcome::failed` rather than dereference.
    CHECK(f.call_window(c.kind, c.name, args, false).status == script::HostStatus::error);
  }
}

TEST(objlist_every_member_asserts_on_its_receiver) {
  Fixture f;
  const ObjectId unit = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_health(unit, 10);
  // Not an ObjList, and for `Clear/0` and `Add/1` not even hypothetically one:
  // `bldEnter.Clear` and `slTrain.Add` both ship, on other types.
  const script::Value wrong = f.obj(unit);
  const script::Value one = script::Value::integer(1);
  const script::Value point = pack_point(pt(0, 0));

  const auto refuses = [&](script::CallKind kind, const char* name,
                           std::vector<script::Value> args) {
    return f.call(kind, name, args).status == script::HostStatus::error;
  };
  CHECK(refuses(script::CallKind::member, "count", {wrong}));
  CHECK(refuses(script::CallKind::member, "ClearDead", {wrong}));
  CHECK(refuses(script::CallKind::member, "GetObjList", {wrong}));
  CHECK(refuses(script::CallKind::member, "Add", {wrong, wrong}));
  CHECK(refuses(script::CallKind::member, "AddList", {wrong, wrong}));
  // **`Clear/0` no longer refuses an object**, and that is the whole of it now
  // being two entry points: `Obj::Clear` invalidates the handle it was given,
  // which is what `CROW_IDLE.VS`'s `bird.Clear(); ... if (bird.IsValid())`
  // needs. The list form still refuses everything that is not a list.
  CHECK(refuses(script::CallKind::member, "RemoveList", {wrong, wrong}));
  CHECK(refuses(script::CallKind::member, "Remove", {wrong, wrong}));
  CHECK(refuses(script::CallKind::member, "Contains", {wrong, wrong}));
  CHECK(refuses(script::CallKind::member, "FilterClosest", {wrong, point, one}));
  CHECK(refuses(script::CallKind::member, "ObjEnemy", {wrong, one}));
  CHECK(refuses(script::CallKind::member, "ObjAlly", {wrong, one}));
  CHECK(refuses(script::CallKind::member, "ObjClass", {wrong, script::Value::string("Unit")}));
  CHECK(refuses(script::CallKind::member, "ObjInjured", {wrong}));

  // `Clear` on an object writes the invalid sentinel back into the receiver
  // slot rather than refusing, and the *slot* is what carries the answer: the
  // out-parameter is the mechanism, and 207 corpus hits per pass came of not
  // having it.
  {
    std::vector<script::Value> args{wrong};
    const script::HostOutcome cleared =
        f.call_window(script::CallKind::member, "Clear", args, true);
    CHECK(cleared.status == script::HostStatus::ok);
    CHECK(args[0].is_object());
    CHECK(args[0].as_object().type == script::kNoType);
  }

  // And the mirror image: `GetObjList` on an ObjList is not a copy, it is a
  // refusal -- all 103 corpus receivers are queries.
  const script::Value list = fresh_list(f.world);
  CHECK(refuses(script::CallKind::member, "GetObjList", {list}));
}

// --------------------------------------------------------------------------
// count, on both receivers
// --------------------------------------------------------------------------

TEST(objlist_count_answers_for_a_list_and_for_a_query) {
  Fixture f;
  const ObjectId a = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_position(a, pt(10, 10));
  f.world.set_position(b, pt(20, 20));

  const script::Value list = fresh_list(f.world);
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 0);
  f.call(script::CallKind::member, "Add", {list, f.obj(a)});
  f.call(script::CallKind::member, "Add", {list, f.obj(b)});
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 2);

  // The same entry point, on a query, evaluating rather than reading.
  const ObjectId query = f.world.create_query(objs_in_circle(pt(15, 15), 1000, ClassFilter{}));
  const script::Value query_value = script::Value::object(script::ObjectRef{kTypeQuery, query});
  CHECK(f.call(script::CallKind::member, "count", {query_value}).value.as_integer() == 2);

  // An unbound list -- `ObjList ol;` with nothing done to it yet -- reads zero
  // rather than trapping. `for (i = 0; i < ol.count; ...)` on one is ordinary.
  const script::Value unbound = make_objlist_value(kNoObjList);
  CHECK(f.call(script::CallKind::member, "count", {unbound}).value.as_integer() == 0);

  // And so does a handle naming nothing at all: `Query::count` prints "called
  // for an uninitialized or invalid object" and pushes 0 (0x00577be4), which
  // is what `Intersect(...).count` comes to in `LION_LEAD.VS` when an operand
  // of the intersection was invalid. A live object that is not a query is
  // still a refusal, as the receiver sweep above says.
  const script::Value nothing = script::Value::object(script::ObjectRef{script::kNoType, 0});
  const script::HostOutcome none = f.call(script::CallKind::member, "count", {nothing});
  CHECK(none.status == script::HostStatus::ok);
  CHECK(none.value.as_integer() == 0);
  const script::Value stale = script::Value::object(script::ObjectRef{kTypeQuery, 9999});
  CHECK(f.call(script::CallKind::member, "count", {stale}).value.as_integer() == 0);

  // `GetObjList` on the same handle is a fresh empty list (0x0057a81d), and
  // on a stale query id likewise; a live non-query still refuses (above).
  const script::HostOutcome empty = f.call(script::CallKind::member, "GetObjList", {nothing});
  CHECK(empty.status == script::HostStatus::ok);
  CHECK(f.call(script::CallKind::member, "count", {empty.value}).value.as_integer() == 0);
  CHECK(f.call(script::CallKind::member, "GetObjList", {stale}).status == script::HostStatus::ok);
}

// --------------------------------------------------------------------------
// the mutators
// --------------------------------------------------------------------------

TEST(objlist_add_appends_in_order_and_ignores_a_dead_handle) {
  Fixture f;
  const ObjectId a = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = f.world.spawn(NativeClass::unit, nullptr);
  const script::Value list = fresh_list(f.world);

  f.call(script::CallKind::member, "Add", {list, f.obj(b)});
  f.call(script::CallKind::member, "Add", {list, f.obj(a)});
  // Insertion order, not id order: an ObjList is a list, and iteration order is
  // state.
  const std::span<const ObjectId> items = objlist_pool_of(f.world).items(objlist_of(list));
  REQUIRE(items.size() == 2);
  CHECK(items[0] == b);
  CHECK(items[1] == a);

  // An invalid handle is a no-op, not an error: 331 files declare a handle and
  // assign it several statements later.
  f.call(script::CallKind::member, "Add",
         {list, script::Value::object(script::ObjectRef{script::kNoType, 0})});
  f.world.despawn(a);
  f.call(script::CallKind::member, "Add", {list, f.obj(a)});
  CHECK(objlist_pool_of(f.world).items(objlist_of(list)).size() == 2);
}

TEST(objlist_a_declaration_binds_its_entry_at_the_declaration_site) {
  Fixture f;
  const ObjectId a = f.world.spawn(NativeClass::unit, nullptr);
  WorldHost host(f.world);

  // Exactly what `ObjList OL_UnattachedVillages;` in BUILDINGSADVICE4.VS is:
  // declared, never assigned, only `.Contains`ed and `.Add`ed. It arrives bound
  // to a pool entry, so the first `.Add` has somewhere to put its object.
  const script::Value declared =
      host.default_value("ObjList", script::Host::DeclarationSite{7, 2});
  CHECK(is_objlist(declared));
  CHECK(objlist_of(declared) != kNoObjList);
  CHECK(objlist_pool_of(f.world).contains(objlist_of(declared)));
  CHECK(!f.call(script::CallKind::member, "Contains", {declared, f.obj(a)}).value.as_integer());

  std::vector<script::Value> args{declared, f.obj(a)};
  CHECK(f.call_window(script::CallKind::member, "Add", args).status ==
        script::HostStatus::ok);
  // No writeback is needed and none happens: the handle was already the
  // caller's. The lazy-binding workaround this replaced had to write argument 0
  // back through the out-parameter path; nothing does now.
  CHECK(args[0] == declared);
  CHECK(f.call(script::CallKind::member, "count", {declared}).value.as_integer() == 1);

  // The site is the key. Re-declaring the *same* site -- `ObjList ol;` reached a
  // second time round a loop, which TS_CARTHAGETACTIC.VS does three times over
  // -- reuses the entry and clears it, so the pool does not grow.
  const std::size_t before = objlist_pool_of(f.world).size();
  const script::Value again =
      host.default_value("ObjList", script::Host::DeclarationSite{7, 2});
  CHECK(again == declared);
  CHECK(objlist_pool_of(f.world).size() == before);
  CHECK(f.call(script::CallKind::member, "count", {again}).value.as_integer() == 0);

  // A different slot, and the same slot in another script, are different lists.
  CHECK(host.default_value("ObjList", script::Host::DeclarationSite{7, 3}) != declared);
  CHECK(host.default_value("ObjList", script::Host::DeclarationSite{8, 2}) != declared);
}

TEST(objlist_default_value_without_a_site_refuses) {
  World world;
  WorldHost host(world);
  // The one-argument form has no key to acquire under, and handing back a list
  // nothing owns is how the pool would start leaking again. Every other type
  // still answers there, and the two-argument form forwards to it.
  CHECK(host.default_value("ObjList").is_nil());
  CHECK(unpack_point(host.default_value("point", script::Host::DeclarationSite{1, 0})) ==
        pt(0, 0));
  CHECK(host.default_value("int", script::Host::DeclarationSite{1, 0}).as_integer() == 0);
}

TEST(objlist_addlist_and_removelist_are_set_moves_not_copies) {
  Fixture f;
  std::vector<ObjectId> ids;
  for (int i = 0; i < 4; ++i) ids.push_back(f.world.spawn(NativeClass::unit, nullptr));

  const script::Value left = fresh_list(f.world);
  const script::Value right = fresh_list(f.world);
  for (int i = 0; i < 2; ++i) f.call(script::CallKind::member, "Add", {left, f.obj(ids[i])});
  for (int i = 1; i < 4; ++i) f.call(script::CallKind::member, "Add", {right, f.obj(ids[i])});

  f.call(script::CallKind::member, "AddList", {left, right});
  CHECK(f.call(script::CallKind::member, "count", {left}).value.as_integer() == 5);
  // `right` is untouched: `AddList` reads it, and the two handles stay separate
  // lists.
  CHECK(f.call(script::CallKind::member, "count", {right}).value.as_integer() == 3);

  f.call(script::CallKind::member, "RemoveList", {left, right});
  // Every occurrence goes, including the duplicate `AddList` introduced.
  const std::span<const ObjectId> items = objlist_pool_of(f.world).items(objlist_of(left));
  REQUIRE(items.size() == 1);
  CHECK(items[0] == ids[0]);

  // Self-append is legal in the language and must not read a vector it is
  // growing.
  f.call(script::CallKind::member, "AddList", {left, left});
  CHECK(f.call(script::CallKind::member, "count", {left}).value.as_integer() == 2);
}

TEST(objlist_remove_takes_an_object_or_a_list) {
  Fixture f;
  std::vector<ObjectId> ids;
  for (int i = 0; i < 3; ++i) ids.push_back(f.world.spawn(NativeClass::unit, nullptr));
  const script::Value list = fresh_list(f.world);
  for (const ObjectId id : ids) f.call(script::CallKind::member, "Add", {list, f.obj(id)});

  f.call(script::CallKind::member, "Remove", {list, f.obj(ids[1])});
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 2);
  CHECK(f.call(script::CallKind::member, "Contains", {list, f.obj(ids[1])})
            .value.as_integer() == 0);

  // Removing an object that has already been despawned is what a script does
  // between two `ClearDead`s, so `Remove` must not resolve the handle first.
  f.world.despawn(ids[0]);
  f.call(script::CallKind::member, "Remove", {list, f.obj(ids[0])});
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 1);

  // `ol.Remove(ol.ObjEnemy(AIPlayer))` -- an ObjList argument, from the one
  // (commented-out) corpus shape that uses it.
  const script::Value other = fresh_list(f.world);
  f.call(script::CallKind::member, "Add", {other, f.obj(ids[2])});
  f.call(script::CallKind::member, "Remove", {list, other});
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 0);
}

TEST(objlist_clear_empties_the_list_and_keeps_the_handle) {
  Fixture f;
  const ObjectId a = f.world.spawn(NativeClass::unit, nullptr);
  const script::Value list = fresh_list(f.world);
  f.call(script::CallKind::member, "Add", {list, f.obj(a)});

  std::vector<script::Value> args{list};
  CHECK(f.call_window(script::CallKind::member, "Clear", args).status ==
        script::HostStatus::ok);
  // The handle is unchanged, because `TSH_RECRUITARMY.VS` says the caller is
  // holding the same one: rebinding it here would silently orphan the caller.
  CHECK(args[0] == list);
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 0);
  f.call(script::CallKind::member, "Add", {list, f.obj(a)});
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 1);
}

TEST(objlist_cleardead_drops_missing_and_zero_health_objects) {
  Fixture f;
  const ObjectId alive = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId hurt = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId gone = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_health(alive, 10);
  f.world.set_health(hurt, 0);
  f.world.set_health(gone, 10);

  const script::Value list = fresh_list(f.world);
  for (const ObjectId id : {alive, hurt, gone}) {
    f.call(script::CallKind::member, "Add", {list, f.obj(id)});
  }
  f.world.despawn(gone);

  f.call(script::CallKind::member, "ClearDead", {list});
  // The invariant that fixes the reading: after `ClearDead`, nothing left in
  // the list answers true to `IsDead`, which sim/world_host.cpp defines as
  // "missing, or health at or below zero". Every call site immediately commands
  // what survives.
  const std::span<const ObjectId> items = objlist_pool_of(f.world).items(objlist_of(list));
  REQUIRE(items.size() == 1);
  CHECK(items[0] == alive);

  // Order among the survivors is preserved.
  const ObjectId second = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_health(second, 5);
  f.call(script::CallKind::member, "Add", {list, f.obj(second)});
  f.call(script::CallKind::member, "ClearDead", {list});
  const std::span<const ObjectId> after = objlist_pool_of(f.world).items(objlist_of(list));
  REQUIRE(after.size() == 2);
  CHECK(after[0] == alive);
  CHECK(after[1] == second);

  // A stale or unbound handle reads as an empty list rather than trapping --
  // `ClearDead` on one has nothing to do and says so quietly.
  CHECK(f.call(script::CallKind::member, "ClearDead", {make_objlist_value(kNoObjList)})
            .status == script::HostStatus::ok);
  // A *mutator* on one refuses instead. With declarations bound eagerly the
  // only way to hold an unbound ObjList is to have outlived the list, and
  // silently dropping the write would lose it.
  CHECK(f.call(script::CallKind::member, "Add",
               {make_objlist_value(kNoObjList), f.obj(alive)})
            .status == script::HostStatus::error);
}

// --------------------------------------------------------------------------
// GetObjList
// --------------------------------------------------------------------------

TEST(objlist_getobjlist_is_a_snapshot_and_a_query_is_not) {
  Fixture f;
  const ObjectId a = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_position(a, pt(10, 10));
  const ObjectId query = f.world.create_query(objs_in_circle(pt(10, 10), 500, ClassFilter{}));
  const script::Value query_value = script::Value::object(script::ObjectRef{kTypeQuery, query});

  const script::HostOutcome out = f.call(script::CallKind::member, "GetObjList", {query_value});
  REQUIRE(out.status == script::HostStatus::ok);
  const script::Value list = out.value;
  CHECK(is_objlist(list));
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 1);

  // The whole distinction between the two types: the query re-evaluates, the
  // list does not. This is why GUARD.VS re-reads `ol = qryDef.GetObjList();` at
  // the top of every pass of its loop.
  const ObjectId b = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_position(b, pt(11, 11));
  CHECK(f.call(script::CallKind::member, "count", {query_value}).value.as_integer() == 2);
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 1);

  // Ascending id order, which is spawn order (architecture rule 3).
  const script::Value again =
      f.call(script::CallKind::member, "GetObjList", {query_value}).value;
  const std::span<const ObjectId> items = objlist_pool_of(f.world).items(objlist_of(again));
  REQUIRE(items.size() == 2);
  CHECK(items[0] == a);
  CHECK(items[1] == b);
}

TEST(objlist_filterclosest_ranks_by_distance_then_by_id) {
  Fixture f;
  const ObjectId near = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId far = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId tie = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_position(near, pt(10, 0));
  f.world.set_position(far, pt(500, 0));
  f.world.set_position(tie, pt(10, 0));

  const script::Value list = fresh_list(f.world);
  for (const ObjectId id : {far, tie, near}) {
    f.call(script::CallKind::member, "Add", {list, f.obj(id)});
  }

  const script::Value one =
      f.call(script::CallKind::member, "FilterClosest",
             {list, pack_point(pt(0, 0)), script::Value::integer(1)})
          .value;
  const std::span<const ObjectId> best = objlist_pool_of(f.world).items(objlist_of(one));
  REQUIRE(best.size() == 1);
  // Two objects sit at the same distance; the lower id wins, because that is
  // spawn order and every other collection here comes out in it.
  CHECK(best[0] == near);

  const script::Value two =
      f.call(script::CallKind::member, "FilterClosest",
             {list, pack_point(pt(0, 0)), script::Value::integer(9)})
          .value;
  const std::span<const ObjectId> all = objlist_pool_of(f.world).items(objlist_of(two));
  REQUIRE(all.size() == 3);
  CHECK(all[0] == near);
  CHECK(all[1] == tie);
  CHECK(all[2] == far);

  // The receiver is untouched: FilterClosest returns a new list.
  CHECK(f.call(script::CallKind::member, "count", {list}).value.as_integer() == 3);
}

// --------------------------------------------------------------------------
// the settlement index
// --------------------------------------------------------------------------

TEST(objlist_maxsetidx_and_idxtoset_walk_the_settlements) {
  Fixture f;
  CHECK(f.call(script::CallKind::free_function, "MaxSetIdx", {}).value.as_integer() == 0);
  const World::SettlementIds first = f.world.spawn_settlement(0);
  (void)f.world.spawn(NativeClass::unit, nullptr);
  const World::SettlementIds second = f.world.spawn_settlement(1);

  CHECK(f.call(script::CallKind::free_function, "MaxSetIdx", {}).value.as_integer() == 2);
  const script::Value zero =
      f.call(script::CallKind::free_function, "IdxToSet", {script::Value::integer(0)}).value;
  const script::Value one =
      f.call(script::CallKind::free_function, "IdxToSet", {script::Value::integer(1)}).value;
  // A settlement carries its own type id, so a bare `Obj` handle to one cannot
  // pass a class filter it should not.
  CHECK(zero.as_object().type == kTypeSettlement);
  CHECK(zero.as_object().id == first.settlement);
  CHECK(one.as_object().id == second.settlement);

  // Out of range, and negative, read as an invalid handle -- HEN_IDLE.VS tests
  // exactly that with `if (!set.IsValid) continue;`.
  CHECK(!f.call(script::CallKind::free_function, "IdxToSet", {script::Value::integer(2)})
             .value.as_object()
             .valid());
  CHECK(!f.call(script::CallKind::free_function, "IdxToSet", {script::Value::integer(-1)})
             .value.as_object()
             .valid());
}

// --------------------------------------------------------------------------
// the Host hooks
// --------------------------------------------------------------------------

TEST(objlist_is_a_reference_and_assignment_does_not_clone_it) {
  World world;
  WorldHost host(world);
  const script::Value list = fresh_list(world);
  // `TSH_RECRUITARMY.VS`: "ol IS USED TO RETURN THE OBJECTS ... ol MUST BE
  // CHANGED USEING ONLY THE Add/Remove METHODS". Mutation is shared, so a clone
  // on assignment would make that script a no-op for its caller.
  CHECK(host.clone_for_assign(list) == list);
  // A point, by contrast, is packed into the value itself, so assignment
  // already copies it -- there is nothing to clone either way.
  const script::Value point = pack_point(pt(3, 4));
  CHECK(host.clone_for_assign(point) == point);
}

TEST(objlist_index_get_yields_an_object_and_refuses_nothing_else) {
  World world;
  WorldHost host(world);
  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr);
  const script::Value list = fresh_list(world);
  std::vector<ObjectId>* items = objlist_pool_of(world).mutable_items(objlist_of(list));
  items->push_back(a);
  items->push_back(b);

  const auto at = [&](std::int32_t i) { return host.index_get(list, script::Value::integer(i)); };
  REQUIRE(at(0).ok());
  CHECK(at(0).value().as_object().type == kTypeObj);
  CHECK(at(0).value().as_object().id == a);
  CHECK(at(1).value().as_object().id == b);

  // Out of range is an invalid handle, not a trap: a list can be emptied by a
  // `ClearDead` between the guard and the read, and `.IsValid` is what every
  // other failed lookup in this host answers with.
  REQUIRE(at(2).ok());
  CHECK(!at(2).value().as_object().valid());
  REQUIRE(at(-1).ok());
  CHECK(!at(-1).value().as_object().valid());

  // A non-integer key, and a container that is not an ObjList, refuse.
  CHECK(!host.index_get(list, script::Value::string("x")).ok());
  CHECK(!host.index_get(script::Value::integer(3), script::Value::integer(0)).ok());
}

TEST(objlist_index_set_writes_in_range_and_refuses_past_the_end) {
  World world;
  WorldHost host(world);
  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr);
  script::Value list = fresh_list(world);
  objlist_pool_of(world).mutable_items(objlist_of(list))->push_back(a);

  const script::Value other = script::Value::object(script::ObjectRef{kTypeObj, b});
  CHECK(host.index_set(list, script::Value::integer(0), other).ok());
  CHECK(host.index_get(list, script::Value::integer(0)).value().as_object().id == b);

  // No fill value exists, and an ObjList is not an array: `Add` is how a script
  // makes one longer, at 83 sites.
  CHECK(!host.index_set(list, script::Value::integer(1), other).ok());
  CHECK(!host.index_set(list, script::Value::integer(-1), other).ok());
  CHECK(host.index_get(list, script::Value::integer(0)).value().as_object().id == b);
}

TEST(objlist_truthiness_follows_the_handle_not_the_contents) {
  World world;
  WorldHost host(world);
  const script::Value bound = fresh_list(world);
  const script::Value unbound = make_objlist_value(kNoObjList);
  // Inferred: no shipped script puts a bare ObjList in a condition. A bound
  // list is true even when empty, which is the rule every other handle here
  // follows and the only one that does not make emptiness decide a branch.
  REQUIRE(host.truthy(bound).ok());
  CHECK(host.truthy(bound).value());
  REQUIRE(host.truthy(unbound).ok());
  CHECK(!host.truthy(unbound).value());
}

// --------------------------------------------------------------------------
// the lifetime rule, end to end
// --------------------------------------------------------------------------

TEST(objlist_the_scheduler_releases_a_scripts_lists_when_it_tears_it_down) {
  World world;
  HostContext context;
  context.world = &world;

  script::Scheduler scheduler;
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);

  // No chunks, so `spawn` refuses; drive the hook through the ids the pool
  // keys on instead, which is exactly what the scheduler hands it.
  ObjListPool& pool = objlist_pool_of(world);
  const ObjListId theirs = pool.acquire(2, 0);
  const ObjListId mine = pool.acquire(1, 0);
  const ObjListId temporary = pool.acquire_temporary(1);
  CHECK(pool.size() == 3);

  REQUIRE(scheduler.teardown_hook() != nullptr);
  scheduler.teardown_hook()(&context, 1);
  // Declarations *and* temporaries go: without this the pool grows for the life
  // of the match.
  CHECK(!pool.contains(mine));
  CHECK(!pool.contains(temporary));
  CHECK(pool.contains(theirs));
  CHECK(pool.size() == 1);

  // A null user must not dereference -- the hook runs from `compact`, which a
  // scheduler with no embedder still reaches.
  scheduler.teardown_hook()(nullptr, 2);
  CHECK(pool.size() == 1);
}

TEST(objlist_a_finished_script_releases_its_lists_through_compact) {
  World world;
  HostContext context;
  context.world = &world;

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  (void)register_objlist_host(registry);
  script::register_scheduler_builtins(registry);

  WorldHost host(world);
  script::Scheduler scheduler;
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);

  // A script that declares a list, fills it, and returns.
  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  world.set_health(a, 10);
  const std::string source = "// void, Obj first\nObjList ol;\nol.Add(first);\n";
  const std::uint32_t index = build(scheduler, registry, source, "objlist_test.vs");
  REQUIRE(index != script::kNoChunk);

  const script::Value args[] = {script::Value::object(script::ObjectRef{kTypeObj, a})};
  const script::ScriptId id = scheduler.spawn(index, args);
  REQUIRE(id != script::kNoScript);

  const script::RunReport report = scheduler.run_ready();
  for (const script::FailedScript& trap : report.traps) {
    std::printf("  trap %s:%u: %s\n", trap.source_name.c_str(), trap.trap.line,
                trap.trap.detail.c_str());
  }
  CHECK(report.traps.empty());
  CHECK(report.completed == 1);
  CHECK(scheduler.live_count() == 0);
  // `compact` fired the hook, so the list the script declared is gone with it.
  CHECK(objlist_pool_of(world).size() == 0);
}

TEST(objlist_a_script_reads_its_own_list_through_the_vm) {
  World world;
  HostContext context;
  context.world = &world;

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  (void)register_objlist_host(registry);
  script::register_scheduler_builtins(registry);

  WorldHost host(world);
  script::Scheduler scheduler;
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);

  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr);
  world.set_health(a, 10);
  world.set_health(b, 10);

  // `ol[i]` and `ol.count` in one statement, over a list the script built
  // itself -- the shape 541 `count` sites and every indexed read share. The
  // return value proves the handle survived the declaration, the two `Add`s and
  // the subscript, which is the whole out-parameter path.
  const std::string source =
      "// int, Obj first, Obj second\n"
      "ObjList ol;\n"
      "int i, total;\n"
      "ol.Add(first);\n"
      "ol.Add(second);\n"
      "for (i = 0; i < ol.count; i += 1) total += ol[i].health;\n"
      "return total + ol.count;\n";
  const std::uint32_t index = build(scheduler, registry, source, "objlist_read.vs");
  REQUIRE(index != script::kNoChunk);

  const script::Value args[] = {script::Value::object(script::ObjectRef{kTypeObj, a}),
                                script::Value::object(script::ObjectRef{kTypeObj, b})};
  const script::ScriptId id = scheduler.spawn(index, args);
  REQUIRE(id != script::kNoScript);
  const script::RunReport report = scheduler.run_ready();
  for (const script::FailedScript& trap : report.traps) {
    std::printf("  trap %s:%u: %s\n", trap.source_name.c_str(), trap.trap.line,
                trap.trap.detail.c_str());
  }
  CHECK(report.traps.empty());
  CHECK(report.completed == 1);
  CHECK(objlist_pool_of(world).size() == 0);
}

TEST(objlist_the_sweep_bounds_a_loop_that_refetches_a_query) {
  World world;
  HostContext context;
  context.world = &world;

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  (void)register_objlist_host(registry);
  script::register_scheduler_builtins(registry);

  WorldHost host(world);
  script::Scheduler scheduler;
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);

  const ObjectId unit = world.spawn(NativeClass::unit, nullptr);
  world.set_health(unit, 10);
  world.set_position(unit, pt(10, 10));

  // `DATA\AI HELPERS\GUARD.VS`, reduced to the shape that leaked: a query in a
  // local, re-snapshotted at the top of every pass of a sleeping loop. Each
  // `GetObjList` mints a temporary, and before the sweep every one of them
  // stayed live for as long as the script did.
  const std::string source =
      "// void, Obj anchor\n"
      "Query q;\n"
      "ObjList ol;\n"
      "int i;\n"
      "q = ObjsInCircle(anchor.pos, 500, \"Object\");\n"
      "for (i = 0; i < 40; i += 1) {\n"
      "  ol = q.GetObjList();\n"
      "  Sleep(1000);\n"
      "}\n";
  const std::uint32_t index = build(scheduler, registry, source, "objlist_sweep.vs");
  REQUIRE(index != script::kNoChunk);

  const script::Value args[] = {script::Value::object(script::ObjectRef{kTypeObj, unit})};
  REQUIRE(scheduler.spawn(index, args) != script::kNoScript);

  std::size_t high_water = 0;
  for (int tick = 0; tick < 45; ++tick) {
    const script::RunReport report = scheduler.advance(1000);
    for (const script::FailedScript& trap : report.traps) {
      std::printf("  trap %s:%u: %s\n", trap.source_name.c_str(), trap.trap.line,
                  trap.trap.detail.c_str());
    }
    CHECK(report.traps.empty());
    const std::size_t size = objlist_pool_of(world).size();
    if (size > high_water) high_water = size;
  }

  // The script ran 40 iterations and minted 40 temporaries. What survives at any
  // moment is the one `ol` names plus the declared `ol` entry itself -- bounded
  // by the program text, not by how long the loop ran. Without the sweep this
  // was 41.
  CHECK(high_water <= 3);
  // And it finished, so everything it held is gone with it.
  CHECK(scheduler.live_count() == 0);
  CHECK(objlist_pool_of(world).size() == 0);
}

TEST(objlist_the_sweep_keeps_what_a_suspended_script_still_holds) {
  World world;
  HostContext context;
  context.world = &world;

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  (void)register_objlist_host(registry);
  script::register_scheduler_builtins(registry);

  WorldHost host(world);
  script::Scheduler scheduler;
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);

  const ObjectId unit = world.spawn(NativeClass::unit, nullptr);
  world.set_health(unit, 10);
  world.set_position(unit, pt(10, 10));

  // The list is snapshotted, then the script sleeps holding it, then reads it
  // again. A sweep that missed a suspended script's locals would give the
  // second read a zero, and the return value says so.
  const std::string source =
      "// int, Obj anchor\n"
      "Query q;\n"
      "ObjList ol;\n"
      "q = ObjsInCircle(anchor.pos, 500, \"Object\");\n"
      "ol = q.GetObjList();\n"
      "Sleep(5000);\n"
      "return ol.count;\n";
  const std::uint32_t index = build(scheduler, registry, source, "objlist_hold.vs");
  REQUIRE(index != script::kNoChunk);

  const script::Value args[] = {script::Value::object(script::ObjectRef{kTypeObj, unit})};
  const script::ScriptId id = scheduler.spawn(index, args);
  REQUIRE(id != script::kNoScript);

  scheduler.run_ready();
  // Suspended, mid-script, holding a temporary in a local. Several sweeps run
  // over it while it sleeps.
  for (int tick = 0; tick < 4; ++tick) (void)scheduler.advance(1000);
  const script::ScriptRecord* record = scheduler.find(id);
  REQUIRE(record != nullptr);
  CHECK(objlist_pool_of(world).size() >= 1);

  const script::RunReport report = scheduler.advance(2000);
  CHECK(report.traps.empty());
  CHECK(report.completed == 1);
}

TEST(objlist_the_sweep_leaves_a_list_no_live_script_owns_alone) {
  World world;
  HostContext context;
  context.world = &world;
  script::Scheduler scheduler;
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);

  // A list minted outside any coroutine -- a command's `groupverifier` runs
  // under a bare `Vm`, with no record in this scheduler. Its roots are
  // somewhere the sweep cannot see, so it must not be collected.
  ObjListPool& pool = objlist_pool_of(world);
  const ObjListId orphan = pool.acquire_temporary(script::kNoScript);
  const ObjListId stranger = pool.acquire_temporary(4242);
  CHECK(sweep_objlists(scheduler, pool) == 0);
  CHECK(pool.contains(orphan));
  CHECK(pool.contains(stranger));

  // An empty pool is not an error either.
  ObjListPool empty;
  CHECK(sweep_objlists(scheduler, empty) == 0);
}

// --------------------------------------------------------------------------
// the receiver `count` and `GetObjList` were refusing
// --------------------------------------------------------------------------

TEST(objlist_a_named_object_counts_as_the_one_member_query_it_upcasts_to) {
  // `NO_x.count` and `NO_x.GetObjList()` ship -- 1 and 8 sites -- and both
  // refused, because `live_object_of` takes `Obj`, `Query` and `Settlement`
  // and a `<group type="0">` name resolves to a `NamedObj`. `gbr.exe` needs no
  // registration for it: `NamedObj` is a registered **subtype** of `Query`
  // (0x005b6d96), so the compiler upcasts at zero cost and the receiver is a
  // query of exactly one object. `sim/wait.cpp` already relies on that edge.
  Fixture f;
  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr);
  CHECK(f.world.named_objects().bind("NO_Scipio", hero));
  const std::int32_t index = f.world.named_objects().find("NO_Scipio");
  REQUIRE(index >= 0);
  const script::Value named =
      script::Value::object(script::ObjectRef{kTypeNamedObj, static_cast<std::uint32_t>(index)});

  std::vector<script::Value> count_args = {named};
  const script::HostOutcome counted = f.call_window(script::CallKind::member, "count", count_args);
  REQUIRE(counted.status == script::HostStatus::ok);
  CHECK(counted.value.as_integer() == 1);

  std::vector<script::Value> list_args = {named};
  const script::HostOutcome listed =
      f.call_window(script::CallKind::member, "GetObjList", list_args);
  REQUIRE(listed.status == script::HostStatus::ok);
  REQUIRE(is_objlist(listed.value));
  const std::span<const ObjectId> items =
      objlist_pool_of(f.world).items(objlist_of(listed.value));
  REQUIRE(items.size() == 1);
  CHECK(items[0] == hero);
}

TEST(objlist_a_dead_named_object_counts_zero_rather_than_refusing) {
  // The binding outlives the object -- handles are never reused, so the name
  // stays bound to a handle that is now nobody. The original's
  // `CVXNamedObjQuery` goes *empty* rather than invalid, and
  // `while (i < NO_x.count)` over it has to terminate rather than kill the
  // script, which is the same rule a stale `ObjList` handle already follows
  // three functions up.
  Fixture f;
  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr);
  CHECK(f.world.named_objects().bind("NO_Scipio", hero));
  const std::int32_t index = f.world.named_objects().find("NO_Scipio");
  REQUIRE(index >= 0);
  const script::Value named =
      script::Value::object(script::ObjectRef{kTypeNamedObj, static_cast<std::uint32_t>(index)});
  CHECK(f.world.despawn(hero));

  std::vector<script::Value> count_args = {named};
  const script::HostOutcome counted = f.call_window(script::CallKind::member, "count", count_args);
  REQUIRE(counted.status == script::HostStatus::ok);
  CHECK(counted.value.as_integer() == 0);

  std::vector<script::Value> list_args = {named};
  const script::HostOutcome listed =
      f.call_window(script::CallKind::member, "GetObjList", list_args);
  REQUIRE(listed.status == script::HostStatus::ok);
  REQUIRE(is_objlist(listed.value));
  CHECK(objlist_pool_of(f.world).items(objlist_of(listed.value)).empty());
}

// --------------------------------------------------------------------------
// NearestObj
// --------------------------------------------------------------------------

TEST(objlist_nearestobj_scores_to_the_edge_and_not_to_the_centre) {
  // Two classes with different radii, so that the subtraction is observable:
  // the *further* centre wins when its object is fat enough.
  imperivm::core::ClassGraph graph;
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  graph.add(bytes(R"(<class id="Object" cpp_class="CVXDecor"><properties radius="0"/></class>)"),
            "object.sc.xml");
  graph.add(bytes(R"(<class id="Small" parent="Object" cpp_class="CVXUnit">
      <properties radius="10"/></class>)"),
            "small.sc.xml");
  graph.add(bytes(R"(<class id="Fat" parent="Object" cpp_class="CVXUnit">
      <properties radius="400"/></class>)"),
            "fat.sc.xml");
  graph.link();

  Fixture f;
  f.world.set_class_graph(&graph);
  const imperivm::core::ClassIndex small = graph.find("Small");
  const imperivm::core::ClassIndex fat = graph.find("Fat");

  const ObjectId near_small = f.world.spawn(NativeClass::unit, nullptr, small);
  const ObjectId far_fat = f.world.spawn(NativeClass::unit, nullptr, fat);
  f.world.set_position(near_small, pt(100, 0));   // centre 100 away, edge  90
  f.world.set_position(far_fat, pt(300, 0));      // centre 300 away, edge -100

  const ObjectId query = f.world.create_query(objs_in_circle(pt(0, 0), 5000, ClassFilter{}));
  const script::Value q = script::Value::object(script::ObjectRef{kTypeQuery, query});

  const script::HostOutcome out =
      f.call(script::CallKind::member, "NearestObj", {q, pack_point(pt(0, 0))});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(out.value.is_object());
  // The fat one, three times further away by centre. A centre-distance scan
  // would answer `near_small` here, which is exactly the mistake this catches.
  CHECK(out.value.as_object().id == far_fat);
}

TEST(objlist_nearestobj_takes_an_object_argument_and_subtracts_the_askers_radius_too) {
  imperivm::core::ClassGraph graph;
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  graph.add(bytes(R"(<class id="Object" cpp_class="CVXDecor"><properties radius="0"/></class>)"),
            "object.sc.xml");
  graph.add(bytes(R"(<class id="Wide" parent="Object" cpp_class="CVXUnit">
      <properties radius="1000"/></class>)"),
            "wide.sc.xml");
  graph.link();

  Fixture f;
  f.world.set_class_graph(&graph);
  const imperivm::core::ClassIndex plain = graph.find("Object");
  const imperivm::core::ClassIndex wide = graph.find("Wide");

  // **The asker's radius is only ever observable at the cap.** It is subtracted
  // from every candidate alike, so it cannot reorder them; the one thing it can
  // do is drag a candidate from just outside the ten-million seed to just
  // inside. So that is what this measures, and it is the only test that can
  // tell `0x005a77b0` (both radii) from `0x005a7900` (one).
  const ObjectId asker = f.world.spawn(NativeClass::unit, nullptr, wide);
  const ObjectId only = f.world.spawn(NativeClass::unit, nullptr, plain);
  f.world.set_position(asker, pt(0, 0));
  f.world.set_position(only, pt(10'000'500, 0));

  const ObjectId query =
      f.world.create_query(objs_in_circle(pt(10'000'500, 0), 10, ClassFilter{}));
  const script::Value q = script::Value::object(script::ObjectRef{kTypeQuery, query});
  REQUIRE(f.call(script::CallKind::member, "count", {q}).value.as_integer() == 1);

  // Object form: 10,000,500 - 0 - 1000 = 9,999,500, under the seed. Found.
  const script::HostOutcome by_object =
      f.call(script::CallKind::member, "NearestObj", {q, f.obj(asker)});
  REQUIRE(by_object.value.is_object());
  CHECK(by_object.value.as_object().id == only);

  // Point form, from the asker's own position: 10,000,500, over the seed. Not
  // found. The two differ by exactly the asker's radius and by nothing else.
  const script::HostOutcome by_point =
      f.call(script::CallKind::member, "NearestObj", {q, pack_point(pt(0, 0))});
  REQUIRE(by_point.value.is_object());
  CHECK(by_point.value.as_object().type == script::kNoType);
}

TEST(objlist_nearestobj_has_a_ten_million_cap_and_answers_invalid_past_it) {
  Fixture f;
  const ObjectId lonely = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_position(lonely, pt(20'000'000, 0));

  const ObjectId query =
      f.world.create_query(objs_in_circle(pt(20'000'000, 0), 10, ClassFilter{}));
  const script::Value q = script::Value::object(script::ObjectRef{kTypeQuery, query});

  // The candidate is in the query -- assert that first, so a failure below
  // cannot be an empty query wearing the cap's clothes.
  CHECK(f.call(script::CallKind::member, "count", {q}).value.as_integer() == 1);

  const script::HostOutcome out =
      f.call(script::CallKind::member, "NearestObj", {q, pack_point(pt(0, 0))});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(out.value.is_object());
  // `0x00579576` seeds the running best at 10,000,000 and the comparison is
  // `jge skip`, so a candidate at or past it never displaces the seed.
  CHECK(out.value.as_object().type == script::kNoType);

  // The same object, brought inside the cap, is found.
  f.world.set_position(lonely, pt(1'000, 0));
  const ObjectId close =
      f.world.create_query(objs_in_circle(pt(1'000, 0), 10, ClassFilter{}));
  const script::Value near = script::Value::object(script::ObjectRef{kTypeQuery, close});
  const script::HostOutcome found =
      f.call(script::CallKind::member, "NearestObj", {near, pack_point(pt(0, 0))});
  REQUIRE(found.value.is_object());
  CHECK(found.value.as_object().id == lonely);
}

TEST(objlist_nearestobj_answers_invalid_for_a_receiver_that_is_not_a_query) {
  Fixture f;
  const ObjectId a = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_position(a, pt(10, 10));

  // An `ObjList` receiver. The original's two bodies begin by calling the
  // receiver's `Refresh`, so a list is not a receiver they can serve; both
  // print and push the invalid object rather than refusing, which is what a
  // script's `if (!o.IsValid)` on the next line expects to see.
  const script::Value list = fresh_list(f.world);
  f.call(script::CallKind::member, "Add", {list, f.obj(a)});
  const script::HostOutcome on_list =
      f.call(script::CallKind::member, "NearestObj", {list, pack_point(pt(0, 0))});
  REQUIRE(on_list.status == script::HostStatus::ok);
  REQUIRE(on_list.value.is_object());
  CHECK(on_list.value.as_object().type == script::kNoType);

  // A plain object receiver, and a stale query handle, read the same way.
  const script::HostOutcome on_object =
      f.call(script::CallKind::member, "NearestObj", {f.obj(a), pack_point(pt(0, 0))});
  CHECK(on_object.value.as_object().type == script::kNoType);

  // An argument that is neither a point nor an object is refused by name: the
  // compiler cannot produce one, so silence would hide a defect rather than a
  // script.
  const script::HostOutcome bad_arg = f.call(script::CallKind::member, "NearestObj",
                                             {f.obj(a), script::Value::integer(3)});
  CHECK(bad_arg.status == script::HostStatus::error);
}

TEST(objlist_nearestobj_skips_a_member_whose_handle_has_gone) {
  Fixture f;
  const ObjectId gone = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId here = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_position(gone, pt(10, 0));
  f.world.set_position(here, pt(900, 0));

  const script::Value list = fresh_list(f.world);
  f.call(script::CallKind::member, "Add", {list, f.obj(gone)});
  f.call(script::CallKind::member, "Add", {list, f.obj(here)});

  // A `NamedObj` is the one receiver besides a query that this entry point
  // serves, through the same upcast `count` and `GetObjList` take.
  CHECK(f.world.named_objects().bind("NO_Post", gone));
  const std::int32_t index = f.world.named_objects().find("NO_Post");
  REQUIRE(index >= 0);
  const script::Value named =
      script::Value::object(script::ObjectRef{kTypeNamedObj, static_cast<std::uint32_t>(index)});
  const script::HostOutcome bound =
      f.call(script::CallKind::member, "NearestObj", {named, pack_point(pt(0, 0))});
  REQUIRE(bound.value.is_object());
  CHECK(bound.value.as_object().id == gone);

  // Despawned, the one-member query goes empty rather than invalid, and the
  // scan finds nothing to beat the seed.
  CHECK(f.world.despawn(gone));
  const script::HostOutcome after =
      f.call(script::CallKind::member, "NearestObj", {named, pack_point(pt(0, 0))});
  REQUIRE(after.value.is_object());
  CHECK(after.value.as_object().type == script::kNoType);
}

/// Ties keep the **first** member the query yields, not the last.
///
/// `0x005795ca` is `cmp score, best` / `jge skip`, so an equal score never
/// displaces the one already held. The observable form of that here is the
/// lower object id, because `World::evaluate_query` yields ascending id; the
/// *rule* is query order, and the two coincide for every producer this engine
/// has. A `>` in place of `>=` answers the other one, which is what this pins.
TEST(objlist_nearestobj_ties_keep_the_first_member) {
  Fixture f;
  const ObjectId first = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId second = f.world.spawn(NativeClass::unit, nullptr);
  CHECK(first < second);
  f.world.set_position(first, pt(-300, 0));
  f.world.set_position(second, pt(300, 0));

  const ObjectId query = f.world.create_query(objs_in_circle(pt(0, 0), 5000, ClassFilter{}));
  const script::Value q = script::Value::object(script::ObjectRef{kTypeQuery, query});
  REQUIRE(f.call(script::CallKind::member, "count", {q}).value.as_integer() == 2);

  const script::HostOutcome out =
      f.call(script::CallKind::member, "NearestObj", {q, pack_point(pt(0, 0))});
  REQUIRE(out.value.is_object());
  CHECK(out.value.as_object().id == first);
}

/// An object argument is read through `resolve_position`, so a garrisoned asker
/// measures from its holder rather than from the `(-1, -1)` a held object
/// stores.
TEST(objlist_nearestobj_measures_from_a_held_askers_holder) {
  Fixture f;
  const ObjectId barracks = f.world.spawn(NativeClass::building, nullptr);
  const ObjectId asker = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId near_barracks = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId near_origin = f.world.spawn(NativeClass::unit, nullptr);
  // The barracks itself sits outside the query's band, so it cannot win.
  f.world.set_position(barracks, pt(10'000, 500));
  f.world.set_position(near_barracks, pt(10'100, 0));
  f.world.set_position(near_origin, pt(100, 0));
  REQUIRE(f.world.put_in_holder(asker, barracks));

  const script::Value list = fresh_list(f.world);
  f.call(script::CallKind::member, "Add", {list, f.obj(near_origin)});
  f.call(script::CallKind::member, "Add", {list, f.obj(near_barracks)});
  const ObjectId query =
      f.world.create_query(objs_in_rect(0, -50, 20'000, 50, ClassFilter{}));
  const script::Value q = script::Value::object(script::ObjectRef{kTypeQuery, query});

  const script::HostOutcome out =
      f.call(script::CallKind::member, "NearestObj", {q, f.obj(asker)});
  REQUIRE(out.value.is_object());
  // The one beside the barracks. Reading `state.position` instead would measure
  // from `kHeldPosition` and answer the other.
  CHECK(out.value.as_object().id == near_barracks);
  CHECK(objlist_pool_of(f.world).items(objlist_of(list)).size() == 2);
}

// --------------------------------------------------------------------------
// GetUnitsOnBoard
// --------------------------------------------------------------------------

TEST(objlist_get_units_on_board_snapshots_the_ships_holder) {
  Fixture f;
  const World::ShipIds ship = f.world.spawn_ship(nullptr);
  REQUIRE(ship.ship != kNoObject);
  // The invariant the entry point relies on, asserted rather than assumed:
  // `spawn_ship` mints the holder immediately after the ship.
  CHECK(ship.holder == ship.ship + 1);

  // A second ship, so that "held by *this* holder" is a real restriction and
  // not the same thing as "held at all".
  const World::ShipIds other = f.world.spawn_ship(nullptr);
  const ObjectId a = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId ashore = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId elsewhere = f.world.spawn(NativeClass::unit, nullptr);
  REQUIRE(f.world.put_in_holder(a, ship.holder));
  REQUIRE(f.world.put_in_holder(b, ship.holder));
  REQUIRE(f.world.put_in_holder(elsewhere, other.holder));

  const script::HostOutcome out =
      f.call(script::CallKind::member, "GetUnitsOnBoard", {f.obj(ship.ship)});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  const ObjListId id = objlist_of(out.value);
  const std::span<const ObjectId> items = objlist_pool_of(f.world).items(id);
  REQUIRE(items.size() == 2);
  CHECK(items[0] == a);
  CHECK(items[1] == b);
  CHECK(std::find(items.begin(), items.end(), ashore) == items.end());
  CHECK(std::find(items.begin(), items.end(), elsewhere) == items.end());

  // **A copy, not an alias.** Clearing what the script got back leaves the
  // ship loaded, which is the half that separates this from
  // `Settlement::Units`.
  CHECK(objlist_pool_of(f.world).alias_of(id) == kNoObject);
  objlist_pool_of(f.world).mutable_items(id)->clear();
  const script::HostOutcome again =
      f.call(script::CallKind::member, "GetUnitsOnBoard", {f.obj(ship.ship)});
  CHECK(objlist_pool_of(f.world).items(objlist_of(again.value)).size() == 2);

  // Taking one off the ship takes it off the manifest.
  REQUIRE(f.world.remove_from_holder(a, pt(10, 10)));
  const script::HostOutcome after =
      f.call(script::CallKind::member, "GetUnitsOnBoard", {f.obj(ship.ship)});
  REQUIRE(objlist_pool_of(f.world).items(objlist_of(after.value)).size() == 1);
  CHECK(objlist_pool_of(f.world).items(objlist_of(after.value))[0] == b);
}

/// `GetUnitsInSameHolder` is the holder's whole membership, receiver included.
///
/// `gbr.exe` 0x005df620 assigns the holder's member vector into the new list
/// with one call and no predicate at all -- no owner test, no self-exclusion,
/// no class filter. The two things worth a test are therefore the two that a
/// tidier body would get wrong: the receiver is in its own answer, and so is an
/// enemy standing in the same building.
TEST(objlist_get_units_in_same_holder_is_the_whole_holder_including_the_asker) {
  Fixture f;
  const ObjectId barracks = f.world.spawn(NativeClass::building, nullptr);
  const ObjectId elsewhere = f.world.spawn(NativeClass::building, nullptr);
  const ObjectId me = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId mate = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId foe = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId outside = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId other_holder = f.world.spawn(NativeClass::unit, nullptr);
  f.world.set_owner(me, 1);
  f.world.set_owner(mate, 1);
  f.world.set_owner(foe, 2);
  REQUIRE(f.world.put_in_holder(me, barracks));
  REQUIRE(f.world.put_in_holder(mate, barracks));
  REQUIRE(f.world.put_in_holder(foe, barracks));
  REQUIRE(f.world.put_in_holder(other_holder, elsewhere));

  const script::HostOutcome out =
      f.call(script::CallKind::member, "GetUnitsInSameHolder", {f.obj(me)});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  const std::span<const ObjectId> items =
      objlist_pool_of(f.world).items(objlist_of(out.value));
  REQUIRE(items.size() == 3);
  CHECK(items[0] == me);    // the asker is in its own answer
  CHECK(items[1] == mate);
  CHECK(items[2] == foe);   // and so is the enemy beside it
  CHECK(std::find(items.begin(), items.end(), outside) == items.end());
  CHECK(std::find(items.begin(), items.end(), other_holder) == items.end());

  // A unit in no holder gets an empty list, not a refusal: the original
  // indexes the handle table with `0xffff` and relies on the slot being null.
  const script::HostOutcome ashore =
      f.call(script::CallKind::member, "GetUnitsInSameHolder", {f.obj(outside)});
  CHECK(ashore.status == script::HostStatus::ok);
  CHECK(objlist_pool_of(f.world).items(objlist_of(ashore.value)).empty());

  // And so does a receiver that resolves to nothing.
  const script::HostOutcome nobody =
      f.call(script::CallKind::member, "GetUnitsInSameHolder", {f.obj(kNoObject)});
  CHECK(nobody.status == script::HostStatus::ok);
  CHECK(objlist_pool_of(f.world).items(objlist_of(nobody.value)).empty());
}

TEST(objlist_get_units_on_board_answers_empty_for_anything_that_is_not_a_ship) {
  Fixture f;
  const ObjectId plain = f.world.spawn(NativeClass::unit, nullptr);
  const ObjectId barracks = f.world.spawn(NativeClass::building, nullptr);
  const ObjectId rider = f.world.spawn(NativeClass::unit, nullptr);
  // `plain + 1` exists, is *not* a holder, and **is holding something** --
  // which is the case that separates "the neighbour is the holder" from "the
  // neighbour is whatever came next". Without the second half a body that
  // skipped the kind check answers empty here for the wrong reason.
  REQUIRE(plain + 1 == barracks);
  REQUIRE(f.world.put_in_holder(rider, barracks));

  for (const script::Value receiver :
       {f.obj(plain), f.obj(999999), script::Value::object(script::ObjectRef{script::kNoType, 0})}) {
    const script::HostOutcome out =
        f.call(script::CallKind::member, "GetUnitsOnBoard", {receiver});
    REQUIRE(out.status == script::HostStatus::ok);
    REQUIRE(is_objlist(out.value));
    CHECK(objlist_pool_of(f.world).items(objlist_of(out.value)).empty());
  }
}

// --------------------------------------------------------------------------
// the four filters
// --------------------------------------------------------------------------

/// Each filter mints a new list of the members that pass, in the receiver's
/// order, and leaves the receiver alone.
TEST(objlist_filters_mint_a_new_list_of_the_members_that_pass) {
  Fixture f;
  imperivm::core::ClassGraph graph;
  const auto bytes_of = [](const std::string& text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
      R"(<class id="Unit" parent="Object" cpp_class="CVXUnit"><properties maxhealth="100"/></class>)",
      R"(<class id="Horse" parent="Unit" cpp_class="CVXUnit"/>)",
  };
  const char* names[] = {"object.sc.xml", "unit.sc.xml", "horse.sc.xml"};
  for (int i = 0; i < 3; ++i) graph.add(bytes_of(docs[i]), names[i]);
  graph.link();
  f.world.set_class_graph(&graph);

  // Owners 0 and 1. A fresh table is a free-for-all, so the peace has to be
  // declared: player 0 stays at war with player 2 and makes peace with 1;
  // player 1 makes peace with 2.
  f.world.players().set_relation_word(0, 2, 0);
  f.world.players().set_relation_word(0, 1, 1);
  f.world.players().set_relation_word(1, 2, 1);
  const ObjectId a = f.world.spawn(NativeClass::unit, nullptr, graph.find("Horse"));
  const ObjectId b = f.world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  const ObjectId c = f.world.spawn(NativeClass::unit, nullptr, graph.find("Horse"));
  for (const ObjectId id : {a, b, c}) f.world.set_health(id, 100);
  CHECK(f.world.set_owner(a, 0));
  CHECK(f.world.set_owner(b, 1));
  CHECK(f.world.set_owner(c, 0));
  f.world.set_health(b, 40);

  const script::Value list = fresh_list(f.world);
  std::vector<ObjectId>* items = objlist_pool_of(f.world).mutable_items(objlist_of(list));
  REQUIRE(items != nullptr);
  *items = {c, b, a};

  const auto members = [&](script::HostOutcome out) {
    CHECK(out.status == script::HostStatus::ok);
    const std::span<const ObjectId> view = objlist_pool_of(f.world).items(objlist_of(out.value));
    return std::vector<ObjectId>(view.begin(), view.end());
  };
  const script::CallKind member = script::CallKind::member;

  // ObjEnemy(3): script player 3 is table player 2, whom owner 0 fights.
  CHECK(members(f.call(member, "ObjEnemy", {list, script::Value::integer(3)})) ==
        std::vector<ObjectId>({c, a}));
  // ObjAlly is the complement: everyone not at war with the player.
  CHECK(members(f.call(member, "ObjAlly", {list, script::Value::integer(3)})) ==
        std::vector<ObjectId>({b}));
  CHECK(members(f.call(member, "ObjAlly", {list, script::Value::integer(2)})) ==
        std::vector<ObjectId>({c, b, a}));
  // A player outside the table keeps nothing either way.
  CHECK(members(f.call(member, "ObjEnemy", {list, script::Value::integer(40)})).empty());
  CHECK(members(f.call(member, "ObjAlly", {list, script::Value::integer(40)})).empty());

  // ObjClass is an is-a test: `Unit` takes every member, `Horse` the horses,
  // and a name no class carries takes none.
  CHECK(members(f.call(member, "ObjClass", {list, script::Value::string("Unit")})) ==
        std::vector<ObjectId>({c, b, a}));
  CHECK(members(f.call(member, "ObjClass", {list, script::Value::string("Horse")})) ==
        std::vector<ObjectId>({c, a}));
  CHECK(members(f.call(member, "ObjClass", {list, script::Value::string("Nonesuch")})).empty());

  // ObjInjured: health under maxhealth.
  CHECK(members(f.call(member, "ObjInjured", {list})) == std::vector<ObjectId>({b}));

  // The receiver is untouched, and the answers are fresh lists.
  CHECK(objlist_pool_of(f.world).items(objlist_of(list)).size() == 3);
  const script::HostOutcome one = f.call(member, "ObjInjured", {list});
  const script::HostOutcome two = f.call(member, "ObjInjured", {list});
  CHECK(objlist_of(one.value) != objlist_of(two.value));
  CHECK(objlist_of(one.value) != objlist_of(list));
}
