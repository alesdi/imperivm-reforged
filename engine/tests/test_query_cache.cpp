// The query cache: docs/formats/query-model.md.
//
// `gbr.exe` keeps on every query object but a group the list its last refresh
// made and the time the next one falls due, refreshes only when a read finds
// it due, and answers `IsEmpty` and `Contains` on a due query either by a
// refresh or by a live test that stores nothing, by kind. Each test below
// encodes one of those rules and fails against an engine that evaluates every
// query on every read, which is what this one did before the query-model round.

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/wait.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;

constexpr Point pt(std::int32_t x, std::int32_t y) noexcept { return Point{x, y}; }

ObjectId unit_at(World& world, Point at, PlayerId owner = 0) {
  const ObjectId id = world.spawn(NativeClass::unit, nullptr);
  (void)world.set_health(id, 100);  // alive: see object_is_dead
  world.set_position(id, at);
  (void)world.set_owner(id, owner);
  return id;
}

std::vector<ObjectId> read(World& world, ObjectId query, GameTime now,
                           World::QueryRead how = World::QueryRead::purged) {
  std::vector<ObjectId> out;
  world.read_query(query, now, out, how);
  return out;
}

using Ids = std::vector<ObjectId>;

}  // namespace

TEST(a_query_read_inside_its_period_answers_its_last_refresh) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100));
  const ObjectId circle = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));

  // The first read refreshes: a query never refreshed is due.
  CHECK(read(world, circle, 0) == Ids{a});
  const ObjectId b = unit_at(world, pt(110, 110));
  // 0x004f949f: below the due time the accessor hands back the cached list.
  CHECK(read(world, circle, 999) == Ids{a});
  // At it, the read refreshes and the due time moves on by the period.
  CHECK(read(world, circle, 1000) == (Ids{a, b}));
  CHECK(world.query_next_refresh(circle, 1000) == 2000);
  // The fresh evaluation, which is what the refresh does, sees b at once.
  std::vector<ObjectId> fresh;
  world.evaluate_query(circle, fresh);
  CHECK(fresh == (Ids{a, b}));
}

TEST(each_query_kind_refreshes_at_the_period_its_constructor_writes) {
  World world;
  (void)unit_at(world, pt(100, 100));
  const ObjectId circle = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  const ObjectId enemies = world.create_query(player_flags(1, ClassFilter{}, 2));
  const ObjectId both = world.create_query(set_op(SetOp::set_union, circle, enemies));
  const std::int32_t g = world.group_index("Raiders");
  const ObjectId group = world.create_query(group_query(g));

  CHECK(query_refresh_period(QueryKind::player_flags) == 4000);
  CHECK(query_refresh_period(QueryKind::set_op) == 2000);
  CHECK(query_refresh_period(QueryKind::units_in_settlement) == 2000);
  CHECK(query_refresh_period(QueryKind::objs_in_sight) == 1000);
  CHECK(query_refresh_period(QueryKind::group) == 0);

  (void)read(world, enemies, 300);
  CHECK(world.query_next_refresh(enemies, 300) == 4300);
  (void)read(world, both, 300);
  CHECK(world.query_next_refresh(both, 300) == 2300);
  // The union read its operand, which was due, so it refreshed too.
  CHECK(world.query_next_refresh(circle, 300) == 1300);
  // A group's list is the group: its hint is always the clock (0x00571700).
  (void)read(world, group, 300);
  CHECK(world.query_next_refresh(group, 700) == 700);
}

TEST(the_purge_drops_the_dead_from_a_cached_list_and_the_wait_read_keeps_them) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100));
  const ObjectId b = unit_at(world, pt(120, 100));
  const ObjectId circle = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  CHECK(read(world, circle, 0) == (Ids{a, b}));
  (void)world.set_health(b, 0);
  // The wait bodies read the accessor alone: the stale list keeps its dead.
  CHECK(read(world, circle, 10, World::QueryRead::as_cached) == (Ids{a, b}));
  // The members' pair, purge then accessor (0x00572c10), drops it -- for good.
  CHECK(read(world, circle, 20) == Ids{a});
  CHECK(read(world, circle, 30, World::QueryRead::as_cached) == Ids{a});
}

TEST(a_stale_circle_query_answers_contains_by_the_circle_alone_and_stores_nothing) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100));
  const ObjectId circle = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  CHECK(read(world, circle, 0) == Ids{a});

  const ObjectId corpse = unit_at(world, pt(150, 100));
  (void)world.set_health(corpse, 0);
  const ObjectId outside = unit_at(world, pt(900, 900));
  // Fresh: the cached list, which holds neither.
  CHECK(!world.query_contains(circle, corpse, 500));
  // Due: 0x004f8c60 -- resolves and `d² <= r²`, no death test, no refresh.
  CHECK(world.query_contains(circle, corpse, 1000));
  CHECK(!world.query_contains(circle, outside, 1000));
  CHECK(world.query_next_refresh(circle, 1000) == 1000);
  CHECK(world.query_cache(circle)->members == Ids{a});
  // The refresh itself refuses the dead (0x004fc980).
  CHECK(read(world, circle, 1000) == Ids{a});
}

TEST(a_stale_class_player_query_refreshes_to_answer_contains) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100), 1);
  const ObjectId mine =
      world.create_query(class_player_area(ClassFilter{}, 1, pt(100, 100), 500));
  CHECK(read(world, mine, 0) == Ids{a});
  const ObjectId b = unit_at(world, pt(130, 100), 1);
  CHECK(!world.query_contains(mine, b, 999));
  // The base slot (0x004f8230 -> 0x00571870): refresh, then scan.
  CHECK(world.query_contains(mine, b, 1000));
  CHECK(world.query_next_refresh(mine, 1000) == 2000);
  CHECK(world.query_cache(mine)->members == (Ids{a, b}));
}

TEST(a_stale_area_query_answers_is_empty_by_a_sweep_that_stores_nothing) {
  World world;
  const ObjectId circle = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  CHECK(world.query_is_empty(circle, 0));  // never refreshed: due, swept live
  CHECK(read(world, circle, 0).empty());
  (void)unit_at(world, pt(100, 100));
  CHECK(world.query_is_empty(circle, 500));  // fresh: the cached list
  // Due: the counting sweep (0x004ff7b0, the collector in mode 0).
  CHECK(!world.query_is_empty(circle, 1000));
  CHECK(world.query_next_refresh(circle, 1000) == 1000);
  CHECK(world.query_cache(circle)->members.empty());

  // A set operation leaves the slot at the base's, which refreshes.
  const ObjectId other = world.create_query(objs_in_circle(pt(5000, 5000), 10, ClassFilter{}));
  const ObjectId both = world.create_query(set_op(SetOp::set_union, circle, other));
  CHECK(!world.query_is_empty(both, 1000));
  CHECK(world.query_next_refresh(both, 1000) == 3000);
}

TEST(intersect_walks_the_operand_with_the_shorter_refresh_period) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100));
  const ObjectId b = unit_at(world, pt(110, 100));
  const ObjectId c = unit_at(world, pt(120, 100));
  const ObjectId x = unit_at(world, pt(4000, 4000));
  const std::int32_t g = world.group_index("Column");
  for (const ObjectId id : {c, b, a, x}) world.groups().add(g, id);
  const ObjectId group = world.create_query(group_query(g));
  const ObjectId circle = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));

  // The circle has three members and the group four, so walking the smaller
  // would walk the circle and answer ascending. 0x004ffe93 compares the
  // operands' `vtbl + 0x30` -- the period, 0 for a group -- and walks the group
  // in its joining order.
  const ObjectId both = world.create_query(set_op(SetOp::intersect, circle, group));
  CHECK(read(world, both, 0) == (Ids{c, b, a}));
  std::vector<ObjectId> fresh;
  world.evaluate_query(both, fresh);
  CHECK(fresh == (Ids{c, b, a}));
}

TEST(a_set_operation_composes_its_operands_cached_lists) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100));
  const ObjectId left = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  const ObjectId right = world.create_query(objs_in_circle(pt(5000, 5000), 10, ClassFilter{}));
  CHECK(read(world, left, 0) == Ids{a});
  const ObjectId b = unit_at(world, pt(130, 100));
  const ObjectId both = world.create_query(set_op(SetOp::set_union, left, right));
  // 0x004ffbe0 reads each operand through its accessor: `left` is fresh until
  // 1000, so the union made at 500 holds what `left` held at 0.
  CHECK(read(world, both, 500) == Ids{a});
  // The union is now fresh until 2500; `left` falls due at 1000 by itself.
  CHECK(read(world, left, 1000) == (Ids{a, b}));
  CHECK(read(world, both, 2000) == Ids{a});
  CHECK(read(world, both, 2500) == (Ids{a, b}));
}

TEST(a_purge_of_a_group_takes_its_dead_out_of_the_group) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100));
  const ObjectId b = unit_at(world, pt(110, 100));
  const std::int32_t g = world.group_index("Guards");
  world.groups().add(g, a);
  world.groups().add(g, b);
  const ObjectId group = world.create_query(group_query(g));
  (void)world.set_health(a, 0);
  CHECK(read(world, group, 0, World::QueryRead::as_cached) == (Ids{a, b}));
  // 0x00572c10 on the group's own list, which is the membership.
  world.purge_query(group);
  CHECK(world.groups().members(g).size() == 1);
  CHECK(read(world, group, 0, World::QueryRead::as_cached) == Ids{b});
}

// --------------------------------------------------------------------------
// the waits poll at the query's cadence
// --------------------------------------------------------------------------

namespace {

struct WaitCall {
  std::vector<script::Value> arguments;
  script::CallContext context;
  HostContext context_state;

  WaitCall(World& world, std::initializer_list<script::Value> args) : arguments(args) {
    context_state.world = &world;
    context.arguments = arguments;
    context.user = &context_state;
    context.now = world.time();
    context.waiting_since = world.time();
    context.first_call = true;
  }
};

script::HostOutcome wait(World& world, std::string_view name,
                         std::initializer_list<script::Value> args) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_wait_host(registry);
  WaitCall call(world, args);
  const std::uint32_t index = registry.find(
      script::CallKind::free_function, name, static_cast<std::uint16_t>(args.size()));
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not registered");
  return registry.entry(index).fn(call.context);
}

script::Value q(ObjectId id) {
  return script::Value::object(script::ObjectRef{kTypeQuery, static_cast<std::uint32_t>(id)});
}

}  // namespace

TEST(a_query_wait_sleeps_until_the_querys_next_refresh) {
  World world;
  const ObjectId circle = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  const ObjectId enemies = world.create_query(player_flags(1, ClassFilter{}, 2));
  const std::int32_t g = world.group_index("Empty");
  const ObjectId group = world.create_query(group_query(g));
  const script::Value forever = script::Value::integer(-1);

  // `WaitQueryCountBetween` reads the list accessor, which refreshed the list,
  // so the hint is the whole period (0x005eca10).
  const script::Value one = script::Value::integer(1);
  const script::Value open = script::Value::integer(-1);
  script::HostOutcome out = wait(world, "WaitQueryCountBetween", {q(circle), one, open, forever});
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 1000);
  out = wait(world, "WaitQueryCountBetween", {q(enemies), one, open, forever});
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 4000);
  // A group's hint is the clock: the 100 ms floor.
  out = wait(world, "WaitNonEmptyQuery", {q(group), forever});
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 100);
  // The two-query helper (0x005eca70) takes the smaller hint, floored.
  out = wait(world, "WaitCommonObjects", {q(circle), q(enemies), forever});
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 1000);
  // And a finite timeout still clips the slice.
  out = wait(world, "WaitQueryCountBetween", {q(enemies), one, open, script::Value::integer(250)});
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 250);

  // `WaitNonEmptyQuery` asks the emptiness slot, and an area query that is due
  // answers it by a sweep that refreshes nothing (0x004ff7b0): the due time
  // stays behind the clock, the hint is under 101, and the wait polls at the
  // floor until something else reads the list.
  world.advance(5000);
  out = wait(world, "WaitNonEmptyQuery", {q(circle), forever});
  CHECK(out.status == script::HostStatus::retry);
  CHECK(out.suspend_for == 100);
  CHECK(world.query_next_refresh(circle, world.time()) == 1000);
}

TEST(a_saved_world_keeps_each_querys_cache) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100));
  const ObjectId circle = world.create_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  CHECK(read(world, circle, 300) == Ids{a});
  (void)unit_at(world, pt(110, 100));

  std::vector<std::byte> bytes;
  world.serialize(bytes);
  World restored;
  REQUIRE(restored.deserialize(bytes).ok());
  REQUIRE(restored.query_cache(circle) != nullptr);
  CHECK(restored.query_cache(circle)->members == Ids{a});
  CHECK(restored.query_next_refresh(circle, 500) == 1300);
  CHECK(read(restored, circle, 500) == Ids{a});
}

// --------------------------------------------------------------------------
// interning and the reference count
// --------------------------------------------------------------------------

TEST(equal_query_constructions_answer_one_object_and_share_its_cache) {
  World world;
  const ObjectId a = unit_at(world, pt(100, 100));
  // 0x004ff860 scans the circle pool for (x, y, r, class) before it allocates.
  const ObjectId first = world.intern_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  const ObjectId again = world.intern_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  const ObjectId wider = world.intern_query(objs_in_circle(pt(100, 100), 600, ClassFilter{}));
  CHECK(first == again);
  CHECK(first != wider);
  CHECK(read(world, first, 0) == Ids{a});
  const ObjectId b = unit_at(world, pt(120, 100));
  // One object, so one cache: the second construction reads the first's list.
  CHECK(read(world, again, 500) == Ids{a});
  (void)b;
  // Gone, the definition makes a fresh object, which refreshes on first read.
  REQUIRE(world.destroy_query(first));
  const ObjectId fresh = world.intern_query(objs_in_circle(pt(100, 100), 500, ClassFilter{}));
  CHECK(fresh != first);
  CHECK(read(world, fresh, 500).size() == 2);
}

TEST(a_script_query_nothing_reaches_is_destroyed_and_its_operands_live_through_it) {
  World world;
  const ObjectId left = world.intern_query(objs_in_circle(pt(0, 0), 100, ClassFilter{}));
  const ObjectId right = world.intern_query(objs_in_circle(pt(500, 0), 100, ClassFilter{}));
  const ObjectId both = world.intern_query(set_op(SetOp::set_union, left, right));
  const ObjectId loose = world.intern_query(objs_in_circle(pt(900, 0), 100, ClassFilter{}));
  const ObjectId mine = world.create_query(objs_in_circle(pt(900, 900), 100, ClassFilter{}));
  const ObjectId group = world.intern_query(group_query(world.group_index("Army")));

  // A variable holds the union; the set operation holds a reference on each
  // operand (0x004fb1b0); nothing holds `loose`.
  const std::vector<ObjectId> roots{both};
  CHECK(world.collect_queries(roots) == 1);
  CHECK(world.query_spec(loose) == nullptr);
  CHECK(world.query_spec(both) != nullptr);
  CHECK(world.query_spec(left) != nullptr);
  CHECK(world.query_spec(right) != nullptr);
  // The embedder's query and the group are nobody's to collect.
  CHECK(world.query_spec(mine) != nullptr);
  CHECK(world.query_spec(group) != nullptr);

  CHECK(world.collect_queries({}) == 3);
  CHECK(world.query_spec(left) == nullptr);
  CHECK(world.query_spec(mine) != nullptr);
  CHECK(world.query_spec(group) != nullptr);
}

TEST(the_interning_pool_survives_a_save) {
  World world;
  const ObjectId q = world.intern_query(objs_in_circle(pt(0, 0), 100, ClassFilter{}));
  std::vector<std::byte> bytes;
  world.serialize(bytes);
  World restored;
  REQUIRE(restored.deserialize(bytes).ok());
  CHECK(restored.intern_query(objs_in_circle(pt(0, 0), 100, ClassFilter{})) == q);
  // And it is still the scripts', so still collectable.
  CHECK(restored.collect_queries({}) == 1);
}
