// The spatial index answers exactly what the straight scan answered.
//
// `World`'s spatial queries used to walk every object in spawn order. They now
// take candidates from a grid (sim/spatial_index.hpp) and put them back into
// spawn order. That is only allowed to be faster: iteration order is state, and
// a query that returns one id more, one fewer, or the same ids in another order
// is a desync. So the scans are kept here, verbatim in substance, as the oracle,
// and a long randomised run compares every query against them while objects
// move, spawn, die, change hands and go in and out of holders -- and across a
// save and load, which rebuilds the grid from nothing.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;
using imperivm::core::ClassGraph;
using imperivm::core::ClassIndex;
using imperivm::core::kNoClass;
using imperivm::core::NativeClass;

std::span<const std::byte> text_bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

struct Graph {
  ClassGraph graph;
  ClassIndex object = kNoClass;
  ClassIndex unit = kNoClass;
  ClassIndex soldier = kNoClass;
  ClassIndex fort = kNoClass;

  Graph() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="Unit" parent="Object" cpp_class="CVXUnit"><properties sight="700" maxhealth="50"/></class>)",
        R"(<class id="Soldier" parent="Unit" cpp_class="CVXUnit"><properties sight="1500" maxhealth="80"/></class>)",
        R"(<class id="Fort" parent="Object" cpp_class="CVXBuilding"><properties sight="400" maxhealth="5000"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "unit.sc.xml", "soldier.sc.xml", "fort.sc.xml"};
    for (int i = 0; i < 4; ++i) graph.add(text_bytes(docs[i]), names[i]);
    graph.link();
    object = graph.find("Object");
    unit = graph.find("Unit");
    soldier = graph.find("Soldier");
    fort = graph.find("Fort");
  }
};

/// A fixed-seed generator of its own, so the run is the same run everywhere.
struct Lcg {
  std::uint64_t state;
  std::uint32_t next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint32_t>(state >> 33);
  }
  std::int32_t in(std::int32_t lo, std::int32_t hi) {  // inclusive
    return lo + static_cast<std::int32_t>(next() % static_cast<std::uint32_t>(hi - lo + 1));
  }
  bool chance(std::uint32_t percent) { return next() % 100 < percent; }
};

// --------------------------------------------------------------------------
// the oracle: the linear scans the grid replaced
// --------------------------------------------------------------------------

std::int64_t d2(Point a, Point b) {
  const std::int64_t dx = std::int64_t{a.x} - b.x;
  const std::int64_t dy = std::int64_t{a.y} - b.y;
  return dx * dx + dy * dy;
}

Point located(const World& w, const WorldObject& slot) {
  return slot.state.is_held() ? w.resolve_position(slot.id) : slot.state.position;
}

void take(const World& w, const WorldObject& slot, const ClassFilter& filter,
          std::vector<ObjectId>& out) {
  if (slot.internal != InternalKind::none) return;
  if (slot.state.flags.unspawned) return;
  if (!w.matches_filter(slot, filter)) return;
  out.push_back(slot.id);
}

std::vector<ObjectId> scan_radius(const World& w, Point center, std::int32_t radius,
                                  const ClassFilter& filter) {
  std::vector<ObjectId> out;
  const std::int64_t limit = std::int64_t{radius} * radius;
  for (const WorldObject& slot : w.objects()) {
    if (slot.internal != InternalKind::none) continue;
    const Point at = located(w, slot);
    if (at == kHeldPosition) continue;
    if (radius > 0 && d2(at, center) > limit) continue;
    take(w, slot, filter, out);
  }
  return out;
}

std::vector<ObjectId> scan_rect(const World& w, std::int32_t l, std::int32_t t, std::int32_t r,
                                std::int32_t b, const ClassFilter& filter) {
  std::vector<ObjectId> out;
  for (const WorldObject& slot : w.objects()) {
    if (slot.internal != InternalKind::none) continue;
    const Point at = located(w, slot);
    if (at == kHeldPosition) continue;
    if (at.x < l || at.x > r || at.y < t || at.y > b) continue;
    take(w, slot, filter, out);
  }
  return out;
}

std::vector<ObjectId> scan_located(const World& w, std::int32_t l, std::int32_t t,
                                   std::int32_t r, std::int32_t b) {
  std::vector<ObjectId> out;
  for (const WorldObject& slot : w.objects()) {
    if (slot.internal != InternalKind::none) continue;
    const Point at = w.resolve_position(slot.id);
    if (at.x < l || at.x > r || at.y < t || at.y > b) continue;
    out.push_back(slot.id);
  }
  return out;
}

std::vector<ObjectId> scan_sight(const World& w, ObjectId observer, const ClassFilter& filter) {
  std::vector<ObjectId> out;
  const WorldObject* watcher = w.find(observer);
  if (watcher == nullptr) return out;
  const Point eye = w.resolve_position(observer);
  if (eye == kHeldPosition) return out;
  const std::int64_t limit = std::int64_t{watcher->sight} * watcher->sight;
  for (const WorldObject& slot : w.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.id == observer) continue;
    const Point at = located(w, slot);
    if (at == kHeldPosition) continue;
    if (watcher->sight > 0 && d2(at, eye) > limit) continue;
    take(w, slot, filter, out);
  }
  return out;
}

std::vector<ObjectId> scan_player(const World& w, const ClassFilter& filter, PlayerId player,
                                  Point center, std::int32_t radius) {
  std::vector<ObjectId> out;
  const std::int64_t limit = std::int64_t{radius} * radius;
  for (const WorldObject& slot : w.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (player != kNoPlayer && slot.state.owner != player) continue;
    if (radius > 0) {
      const Point at = located(w, slot);
      if (at == kHeldPosition || d2(at, center) > limit) continue;
    }
    take(w, slot, filter, out);
  }
  return out;
}

std::vector<ObjectId> scan_player_rect(const World& w, const ClassFilter& filter,
                                       PlayerId player, std::int32_t l, std::int32_t t,
                                       std::int32_t r, std::int32_t b) {
  std::vector<ObjectId> out;
  for (const WorldObject& slot : w.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (player != kNoPlayer && slot.state.owner != player) continue;
    const Point at = located(w, slot);
    if (at == kHeldPosition) continue;
    if (at.x < l || at.x > r || at.y < t || at.y > b) continue;
    take(w, slot, filter, out);
  }
  return out;
}

// --------------------------------------------------------------------------
// the randomised run
// --------------------------------------------------------------------------

/// A coordinate that likes the places a grid gets wrong: cell borders, just
/// either side of them, the far edges of the map and past them, and `(-1,-1)`.
std::int32_t coordinate(Lcg& rng) {
  switch (rng.in(0, 9)) {
    case 0: return rng.in(-1, 1) + 512 * rng.in(0, 64);
    case 1: return rng.in(-3000, -1);
    case 2: return rng.in(32768 - 600, 40000);
    case 3: return -1;
    default: return rng.in(0, 16383);
  }
}

Point somewhere(Lcg& rng) { return Point{coordinate(rng), coordinate(rng)}; }

struct Tally {
  std::uint64_t queries = 0;
  std::uint64_t mismatches = 0;
  std::uint64_t nonempty = 0;
  void compare(const std::vector<ObjectId>& got, const std::vector<ObjectId>& want) {
    ++queries;
    if (!want.empty()) ++nonempty;
    if (got != want) ++mismatches;
  }
};

ObjectId pick(Lcg& rng, const World& w) {
  const auto objects = w.objects();
  if (objects.empty()) return kNoObject;
  return objects[static_cast<std::size_t>(rng.in(0, static_cast<std::int32_t>(objects.size()) - 1))].id;
}

ClassFilter any_filter(Lcg& rng, const Graph& g) {
  switch (rng.in(0, 4)) {
    case 0: return ClassFilter::parse("Unit", &g.graph);
    case 1: return ClassFilter::parse("Soldier,Fort", &g.graph);
    case 2: return ClassFilter::parse("Nonesuch", &g.graph);
    default: return ClassFilter{};
  }
}

std::int32_t radius_of(Lcg& rng) {
  switch (rng.in(0, 7)) {
    case 0: return 0;
    case 1: return -rng.in(1, 50);
    case 2: return rng.in(8000, 40000);  // most of the map: the scan path
    case 3: return rng.in(0, 3);
    default: return rng.in(1, 2500);
  }
}

void ask_everything(Lcg& rng, const World& w, const Graph& g, Tally& tally) {
  std::vector<ObjectId> got;
  for (int i = 0; i < 4; ++i) {
    const Point c = somewhere(rng);
    const std::int32_t r = radius_of(rng);
    const ClassFilter f = any_filter(rng, g);
    w.objects_in_radius(c, r, f, got);
    tally.compare(got, scan_radius(w, c, r, f));

    std::int32_t l = coordinate(rng), t = coordinate(rng);
    std::int32_t rr = l + rng.in(-50, 3000), b = t + rng.in(-50, 3000);
    if (rng.chance(10)) { l = -5; t = -5; rr = 3; b = 3; }  // covers (-1,-1)
    if (rng.chance(5)) { l = -40000; t = -40000; rr = 40000; b = 40000; }
    w.objects_in_rect(l, t, rr, b, f, got);
    tally.compare(got, scan_rect(w, l, t, rr, b, f));
    w.objects_located_in_rect(l, t, rr, b, got);
    tally.compare(got, scan_located(w, l, t, rr, b));

    const ObjectId eye = pick(rng, w);
    w.objects_in_sight(eye, f, got);
    tally.compare(got, scan_sight(w, eye, f));

    const PlayerId player = rng.chance(20) ? kNoPlayer : static_cast<PlayerId>(rng.in(0, 3));
    w.objects_of_class_for_player(f, player, c, r, got);
    tally.compare(got, scan_player(w, f, player, c, r));
    w.objects_of_class_for_player_in_rect(f, player, l, t, rr, b, got);
    tally.compare(got, scan_player_rect(w, f, player, l, t, rr, b));
  }
}

void step(Lcg& rng, World& w, const Graph& g, std::vector<ObjectId>& holders) {
  const ObjectId id = pick(rng, w);
  const WorldObject* slot = w.find(id);
  switch (rng.in(0, 15)) {
    case 0:
    case 1:
    case 2:
    case 3: {  // movement, most of what happens; small steps and teleports
      const ObjectState* s = w.state(id);
      if (s == nullptr) break;
      if (rng.chance(70)) {
        (void)w.set_position(id, Point{s->position.x + rng.in(-300, 300),
                                       s->position.y + rng.in(-300, 300)});
      } else {
        (void)w.set_position(id, somewhere(rng));
      }
      break;
    }
    case 4:
    case 5: {  // into a holder: a settlement's, a ship's, or another object
      ObjectId holder = rng.chance(60) && !holders.empty()
                            ? holders[static_cast<std::size_t>(rng.in(0, static_cast<std::int32_t>(holders.size()) - 1))]
                            : pick(rng, w);
      (void)w.put_in_holder(id, holder);
      break;
    }
    case 6:
    case 7:  // out again, somewhere
      if (slot != nullptr && slot->state.is_held()) (void)w.remove_from_holder(id, somewhere(rng));
      break;
    case 8: {  // a spawn
      const ClassIndex classes[] = {g.object, g.unit, g.soldier, g.fort};
      const ClassIndex c = classes[rng.in(0, 3)];
      const ObjectId fresh = w.spawn(c == g.fort ? NativeClass::building : NativeClass::unit,
                                     nullptr, c);
      (void)w.set_position(fresh, somewhere(rng));
      (void)w.set_owner(fresh, static_cast<PlayerId>(rng.in(0, 3)));
      break;
    }
    case 9: {  // a settlement, whose holder is internal and has a position
      const World::SettlementIds ids = w.spawn_settlement(static_cast<PlayerId>(rng.in(0, 3)));
      (void)w.set_position(ids.holder, somewhere(rng));
      holders.push_back(ids.holder);
      break;
    }
    case 10:  // a death -- a holder's too, which strands what it held
      if (id != kNoObject && rng.chance(60)) (void)w.despawn(id);
      break;
    case 11:
      (void)w.set_owner(id, rng.chance(10) ? kNoPlayer : static_cast<PlayerId>(rng.in(0, 3)));
      break;
    case 12:  // a template, or back into play
      if (ObjectState* s = w.mutable_state(id)) s->flags.unspawned = !s->flags.unspawned;
      break;
    case 13:  // a copy of a template, sometimes straight into a holder
      if (slot != nullptr && slot->state.flags.unspawned) {
        (void)w.spawn_from_template(id, rng.chance(30) ? pick(rng, w) : kNoObject);
      }
      break;
    case 14:  // a change of class, which rebuilds the state around the position
      (void)w.mutate_class(id, rng.chance(50) ? g.soldier : g.unit);
      break;
    case 15:  // `Obj::SetSight`
      if (WorldObject* mutable_slot = w.find(id)) mutable_slot->sight = rng.in(-10, 3000);
      break;
  }
}

TEST(spatial_index_answers_every_query_exactly_as_the_straight_scan) {
  Graph g;
  World world;
  world.set_class_graph(&g.graph);
  Lcg rng{0x5eedULL};
  std::vector<ObjectId> holders;

  for (int i = 0; i < 400; ++i) {
    const ClassIndex classes[] = {g.object, g.unit, g.soldier, g.fort};
    const ClassIndex c = classes[rng.in(0, 3)];
    const ObjectId id = world.spawn(c == g.fort ? NativeClass::building : NativeClass::unit,
                                    nullptr, c);
    (void)world.set_position(id, somewhere(rng));
    (void)world.set_owner(id, static_cast<PlayerId>(rng.in(0, 3)));
    if (i % 40 == 0) {
      const World::SettlementIds ids = world.spawn_settlement(static_cast<PlayerId>(rng.in(0, 3)));
      (void)world.set_position(ids.holder, somewhere(rng));
      holders.push_back(ids.holder);
    }
  }

  Tally tally;
  std::uint64_t inconsistent = 0;
  std::uint64_t held_seen = 0;
  for (int turn = 0; turn < 3000; ++turn) {
    for (int k = 0; k < 6; ++k) step(rng, world, g, holders);
    if (!world.spatial_index_consistent()) ++inconsistent;
    for (const WorldObject& slot : world.objects()) held_seen += slot.state.is_held() ? 1 : 0;
    ask_everything(rng, world, g, tally);

    // A save and a load, into a world that has never filed anything: the grid
    // is not in the save, so this is the load's rebuild or nothing.
    if (turn % 500 == 499) {
      std::vector<std::byte> bytes;
      world.serialize(bytes);
      World loaded;
      loaded.set_class_graph(&g.graph);
      CHECK(loaded.deserialize(bytes).ok());
      CHECK(loaded.spatial_index_consistent());
      Lcg fork = rng;
      Tally after;
      ask_everything(fork, loaded, g, after);
      CHECK(after.mismatches == 0);
      CHECK(after.nonempty > 0);
    }
  }
  CHECK(tally.mismatches == 0);
  CHECK(inconsistent == 0);
  // And the run was a real one: queries answered with something, and objects
  // were held while they were asked about.
  CHECK(tally.nonempty > tally.queries / 4);
  CHECK(held_seen > 1000);
}

TEST(spatial_index_finds_a_garrison_where_its_holder_moved_to) {
  // A held object is not in the grid; it is resolved at every query. So a
  // holder that moves takes its garrison with it without anyone telling the
  // index about the garrison.
  World world;
  const ObjectId fort = world.spawn(NativeClass::building, nullptr);
  const ObjectId inside = world.spawn(NativeClass::unit, nullptr);
  (void)world.set_position(fort, Point{1000, 1000});
  CHECK(world.put_in_holder(inside, fort));
  std::vector<ObjectId> found;
  world.objects_in_radius(Point{1000, 1000}, 10, ClassFilter{}, found);
  CHECK(found.size() == 2 && found[0] == fort && found[1] == inside);

  CHECK(world.set_position(fort, Point{9000, 9000}));
  world.objects_in_radius(Point{1000, 1000}, 10, ClassFilter{}, found);
  CHECK(found.empty());
  world.objects_in_radius(Point{9000, 9000}, 10, ClassFilter{}, found);
  CHECK(found.size() == 2 && found[0] == fort && found[1] == inside);

  // Out, and found where it was put.
  CHECK(world.remove_from_holder(inside, Point{200, 200}));
  world.objects_in_radius(Point{200, 200}, 0 + 1, ClassFilter{}, found);
  CHECK(found.size() == 1 && found[0] == inside);
  CHECK(world.spatial_index_consistent());
}

}  // namespace
