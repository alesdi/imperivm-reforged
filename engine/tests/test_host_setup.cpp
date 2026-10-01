// The host surface as one table: sim/host_setup.hpp.
//
// Two properties, and neither is visible from inside any single domain -- which
// is the point. Both failures this file is built to catch had already shipped
// past a full green suite once, because every domain tested its own slice on
// its own registry with its own idea of `CallContext::user`.
//
//   1. **No entry point is lost to a later domain.** `HostRegistry::define`
//      replaces silently, so two domains claiming the same (kind, name, arity)
//      produce a registry that behaves like whichever ran last, and the loser's
//      own tests keep passing. Measured by counting `implemented()`: a domain
//      measured alone contributes N, and if the full run gains fewer than N
//      when that domain runs, the difference was overwritten.
//
//   2. **One `ctx.user` serves every domain.** This is the regression test for
//      the `void* user` bug: heroes cast it to `HeroHostState*`, combat to
//      `CombatHostContext*`, movement and the object model to `World*`, so the
//      first script to call into two of them in one run reinterpreted one type
//      as another. `sim/host_context.hpp` settles it and the case below is what
//      keeps it settled -- it makes hero and movement calls alternately through
//      one registry with one unchanged `ctx.user`, and asserts on the
//      *receiver* of each, so a stub that ignored what it was called on could
//      not pass it.
//
// No game data: two systems, three hand-spawned objects, an empty grid.

#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;

/// A registry with one domain on it and nothing else.
void define_alone(const HostDomain& domain, script::HostRegistry& registry) {
  script::declare_shipped_surface(registry);
  domain.define(registry);
}

/// How many entry points `domain` implements when it is the only one to run.
std::size_t implemented_alone(const HostDomain& domain) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  const std::size_t before = registry.implemented();
  domain.define(registry);
  return registry.implemented() - before;
}

/// Does `domain`, on its own, implement this entry point?
bool claims(const HostDomain& domain, script::CallKind kind, std::string_view name,
            std::uint16_t arity) {
  script::HostRegistry registry;
  define_alone(domain, registry);
  const std::uint32_t index = registry.find(kind, name, arity);
  return index != script::kUnresolvedHost && registry.entry(index).fn != nullptr;
}

/// One entry point two domains both define, which nobody has settled yet.
///
/// **This list must only ever shrink.** An entry claimed twice behaves like
/// whichever domain registered last, and each domain's own tests keep passing
/// either way, so the count assertions below would silently absorb a new one if
/// they simply allowed for "some" overlap. Naming each collision instead means
/// an unnamed one still fails, and settling a named one fails too -- which is
/// the reminder to delete its line.
struct KnownCollision {
  script::CallKind kind;
  const char* name;
  std::uint16_t arity;
  /// The domain that defines it first, and the one that overwrites it. Both in
  /// `host_domains()` order.
  std::string_view loser;
  std::string_view winner;
  const char* why;
};

// Empty, and it has been non-empty twice.
//
// `IdxToSet/1` was here: the economy read its argument as a `SettlementId` and
// the collection slice as an ordinal position among settlement objects in world
// order, which agree only while settlement ids are a dense sequence from zero.
// It was settled without settling which reading matches the original -- the
// deciding argument is that `MaxSetIdx` and `IdxToSet` must share one numbering,
// since every corpus site is `for (i = 0; i < MaxSetIdx; i += 1) { s =
// IdxToSet(i); ... }`, and `MaxSetIdx` counts settlement objects. The economy's
// copy is gone and `sim/objlist.cpp` carries the open question.
//
// `m:IsEnemy/1` was here too: combat answered it from a symmetric ally table of
// its own, and the player domain from `World::players()`. That one had a right
// answer -- the relation is one-directional and the corpus calls the entry point
// with an integer player as well as an object -- so combat gave it up.
// `std::array` rather than a C array, because a zero-length C array is not
// valid C++ and this list is meant to reach zero.
constexpr std::array<KnownCollision, 0> kUnresolved{};

/// How many entry points `domain` is expected to take from an earlier one.
std::size_t expected_overwrites(std::string_view domain) {
  std::size_t n = 0;
  for (const KnownCollision& c : kUnresolved) {
    if (c.winner == domain) ++n;
  }
  return n;
}

}  // namespace

TEST(declaring_the_shipped_surface_implements_nothing) {
  // The premise the counting below rests on: `declare` names entry points and
  // attaches no behaviour, so every increment in `implemented()` afterwards is
  // a domain's and nobody else's.
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  CHECK(registry.size() > 0);
  CHECK(registry.implemented() == 0);
}

TEST(the_manifest_is_not_empty_and_names_its_domains) {
  const std::span<const HostDomain> domains = host_domains();
  REQUIRE(!domains.empty());
  for (const HostDomain& domain : domains) {
    CHECK(!domain.name.empty());
    CHECK(domain.define != nullptr);
  }
  // Order is the tie-break rule for `define`, so it is asserted rather than
  // assumed. Changing it changes which of two colliding definitions wins.
  CHECK(domains[0].name == "scheduler");
  CHECK(domains[1].name == "world");
}

TEST(register_all_hosts_loses_no_entry_point_to_a_later_domain) {
  // The full run, one domain at a time, in exactly the order
  // `register_all_hosts` uses.
  script::HostRegistry all;
  script::declare_shipped_surface(all);
  std::size_t running = all.implemented();
  std::size_t sum_alone = 0;

  for (const HostDomain& domain : host_domains()) {
    const std::size_t alone = implemented_alone(domain);
    // A domain in the manifest that defines nothing is a wiring mistake, not a
    // domain with nothing to say.
    CHECK(alone > 0);
    sum_alone += alone;

    domain.define(all);
    const std::size_t gained = all.implemented() - running;
    running = all.implemented();
    const std::size_t allowed = expected_overwrites(domain.name);
    if (gained + allowed != alone) {
      std::printf("  domain %.*s: %zu entry points alone, %zu added in sequence,"
                  " %zu collisions on record -- %zu unaccounted for\n",
                  static_cast<int>(domain.name.size()), domain.name.data(), alone, gained,
                  allowed, alone - gained - allowed);
    }
    CHECK(gained + allowed == alone);
  }

  // The same property stated once over the whole table: an entry point two
  // domains both define is counted twice on the left and once on the right, and
  // the difference is exactly the collisions on record.
  CHECK(sum_alone == running + std::size(kUnresolved));

  // And `register_all_hosts` is that sequence and nothing more.
  script::HostRegistry one_call;
  const std::size_t total = register_all_hosts(one_call);
  CHECK(total == one_call.implemented());
  CHECK(total == running);
  CHECK(one_call.size() == all.size());
}

TEST(every_unresolved_collision_is_named_and_no_other_exists) {
  // Each recorded collision really is claimed by exactly the two domains named,
  // and by nobody else. When one is settled -- one of the two stops defining it
  // -- this fails, which is the reminder to delete the line.
  for (const KnownCollision& c : kUnresolved) {
    std::vector<std::string_view> claimants;
    for (const HostDomain& domain : host_domains()) {
      if (claims(domain, c.kind, c.name, c.arity)) claimants.push_back(domain.name);
    }
    if (claimants.size() != 2) {
      std::printf("  %s: claimed by %zu domains, expected 2 (%s)\n", c.name, claimants.size(),
                  c.why);
    }
    REQUIRE(claimants.size() == 2);
    CHECK(claimants[0] == c.loser);
    CHECK(claimants[1] == c.winner);
  }
}

TEST(one_registry_serves_a_hero_call_and_a_movement_call_in_the_same_run) {
  script::HostRegistry registry;
  REQUIRE(register_all_hosts(registry) > 0);

  World world;
  HeroSystem heroes;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(64, 64));
  // Registration order is run order and is world state; both systems are found
  // by name through it, which is why neither needs a field on `HostContext`.
  world.add_system(&heroes);
  world.add_system(&movement);
  world.start();
  REQUIRE(hero_system_of(world) == &heroes);
  REQUIRE(movement_system(world) == &movement);

  const ObjectId hero = world.spawn(NativeClass::hero, nullptr);
  world.set_owner(hero, 1);
  world.set_health(hero, 1000);
  world.set_position(hero, Point{100, 100});
  heroes.register_hero(world, hero);

  const auto spawn_warrior = [&](Point at) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr);
    world.set_owner(id, 1);
    world.set_health(id, 200);
    world.set_position(id, at);
    heroes.register_unit(world, id);
    movement.state(id).speed = 100;
    return id;
  };
  const ObjectId attached = spawn_warrior(Point{200, 200});
  const ObjectId loner = spawn_warrior(Point{300, 300});

  // **One context, set once, never touched again.** Under the old convention
  // this single pointer could satisfy heroes or movement but never both.
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  const auto call = [&](script::CallKind kind, std::string_view name, std::uint16_t arity,
                        std::vector<script::Value> args) -> script::HostOutcome {
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
    return entry.fn(ctx);
  };

  const script::Value hero_ref = script::Value::object(kTypeObj, hero);
  const script::Value attached_ref = script::Value::object(kTypeObj, attached);
  const script::Value loner_ref = script::Value::object(kTypeObj, loner);

  // -- hero, then movement, then hero again, alternating on purpose ---------

  const script::HostOutcome joined =
      call(script::CallKind::member, "AttachTo", 1, {attached_ref, hero_ref});
  CHECK(joined.status == script::HostStatus::ok);
  CHECK(joined.value.as_integer() == 1);

  // Receiver-sensitive, so a stub that ignored argument 0 would fail here: the
  // unit that joined names the hero, the one that did not names nobody.
  const script::HostOutcome leader = call(script::CallKind::member, "hero", 0, {attached_ref});
  CHECK(leader.status == script::HostStatus::ok);
  CHECK(leader.value.as_object().id == hero);
  CHECK(call(script::CallKind::member, "hero", 0, {loner_ref}).value.as_object().id ==
        sim::kNoObject);

  // A movement call, same registry, same `ctx.user`, no reconstruction of
  // anything in between. This is the call that used to reinterpret a
  // `HeroHostState*` as a `World*`.
  const script::HostOutcome moved = call(script::CallKind::member, "SetPos", 1,
                                         {attached_ref, pack_point(Point{640, 480})});
  CHECK(moved.status == script::HostStatus::ok);
  // On the receiver and on nobody else.
  CHECK(world.resolve_position(attached) == (Point{640, 480}));
  CHECK(world.resolve_position(loner) == (Point{300, 300}));
  CHECK(world.resolve_position(hero) == (Point{100, 100}));

  const script::HostOutcome speed = call(script::CallKind::member, "speed", 0, {attached_ref});
  CHECK(speed.status == script::HostStatus::ok);
  CHECK(speed.value.as_integer() == 100);

  // Back into the hero domain, after movement has run, through the same
  // pointer. The army the first call built is still there.
  const script::HostOutcome army = call(script::CallKind::member, "HasArmy", 0, {hero_ref});
  CHECK(army.status == script::HostStatus::ok);
  CHECK(army.value.as_integer() == 1);
  CHECK(call(script::CallKind::member, "HasArmy", 0, {loner_ref}).value.as_integer() == 0);

  // And the object model, which is the third domain that reads the same
  // pointer: `.pos` must agree with what movement just wrote.
  const script::HostOutcome where = call(script::CallKind::member, "pos", 0, {attached_ref});
  CHECK(where.status == script::HostStatus::ok);
  CHECK(is_point(where.value));
  CHECK(unpack_point(where.value) == (Point{640, 480}));
}
