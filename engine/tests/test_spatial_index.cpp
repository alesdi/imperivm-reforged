// The spatial index answers exactly what the straight scan answered, and in
// the original's grid order when asked to.
//
// `World`'s spatial queries used to walk every object in spawn order. They now
// take candidates from a grid (sim/spatial_index.hpp) and put them back into
// spawn order. That is only allowed to be faster: iteration order is state, and
// a query that returns one id more, one fewer, or the same ids in another order
// is a desync. So the scans are kept here, verbatim in substance, as the oracle,
// and a long randomised run compares every query against them while objects
// move, spawn, die, change hands and go in and out of holders -- and across a
// save and load, which rebuilds the grid from nothing.

#include <algorithm>
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

/// Where an area query sees an object: its own position, which for a held one
/// is `(-1, -1)` -- the marker every scan below skips, since a held object
/// stands in no grid cell (`sim/spatial_index.hpp`). These scans resolved a
/// held object to its holder until that was found not to be the original's.
Point located(const World&, const WorldObject& slot) {
  return slot.state.is_held() ? kHeldPosition : slot.state.position;
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
    if (slot.internal != InternalKind::none || slot.state.is_held()) continue;
    const Point at = slot.state.position;
    if (at.x < l || at.x > r || at.y < t || at.y > b) continue;
    out.push_back(slot.id);
  }
  return out;
}

std::vector<ObjectId> scan_sight(const World& w, ObjectId observer, std::int32_t radius,
                                 const ClassFilter& filter) {
  std::vector<ObjectId> out;
  const WorldObject* watcher = w.find(observer);
  if (watcher == nullptr || radius < 0) return out;
  const Point eye = watcher->state.position;  // `(-1, -1)` for a held observer
  const std::int64_t limit = std::int64_t{radius} * radius;
  for (const WorldObject& slot : w.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.id == observer) continue;
    const Point at = located(w, slot);
    if (at == kHeldPosition) continue;
    if (d2(at, eye) > limit) continue;
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

/// True when `ids` are in the grid's walk order: rows of 256-unit cells top to
/// bottom, cells left to right, and inside one cell the order the objects came
/// into it (`sim/spatial_index.hpp`).
bool in_grid_order(const World& w, const std::vector<ObjectId>& ids) {
  for (std::size_t i = 1; i < ids.size(); ++i) {
    const WorldObject* a = w.find(ids[i - 1]);
    const WorldObject* b = w.find(ids[i]);
    if (a == nullptr || b == nullptr) return false;
    const std::int32_t ca = SpatialIndex::cell_of(a->state.position);
    const std::int32_t cb = SpatialIndex::cell_of(b->state.position);
    if (ca > cb) return false;
    if (ca == cb && a->spatial.arrival >= b->spatial.arrival) return false;
  }
  return true;
}

struct Tally {
  std::uint64_t queries = 0;
  std::uint64_t mismatches = 0;
  std::uint64_t nonempty = 0;
  std::uint64_t reordered = 0;  ///< grid answers that were not in the grid's order
  void compare(const std::vector<ObjectId>& got, const std::vector<ObjectId>& want) {
    ++queries;
    if (!want.empty()) ++nonempty;
    if (got != want) ++mismatches;
  }
  /// A grid-order answer: the scan's set, in the walk's order.
  void compare_grid(const World& w, const std::vector<ObjectId>& got,
                    const std::vector<ObjectId>& want) {
    std::vector<ObjectId> sorted = got;
    std::sort(sorted.begin(), sorted.end());
    compare(sorted, want);
    if (!in_grid_order(w, got)) ++reordered;
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
    w.objects_in_sight(eye, r, f, got);
    tally.compare(got, scan_sight(w, eye, r, f));

    const PlayerId player = rng.chance(20) ? kNoPlayer : static_cast<PlayerId>(rng.in(0, 3));
    w.objects_of_class_for_player(f, player, c, r, got);
    tally.compare(got, scan_player(w, f, player, c, r));
    w.objects_of_class_for_player_in_rect(f, player, l, t, rr, b, got);
    tally.compare(got, scan_player_rect(w, f, player, l, t, rr, b));

    // The same questions in the grid's order.
    constexpr SweepOrder grid = SweepOrder::grid;
    w.objects_in_radius(c, r, f, got, grid);
    tally.compare_grid(w, got, scan_radius(w, c, r, f));
    w.objects_in_rect(l, t, rr, b, f, got, grid);
    tally.compare_grid(w, got, scan_rect(w, l, t, rr, b, f));
    w.objects_located_in_rect(l, t, rr, b, got, grid);
    tally.compare_grid(w, got, scan_located(w, l, t, rr, b));
    w.objects_in_sight(eye, r, f, got, grid);
    tally.compare_grid(w, got, scan_sight(w, eye, r, f));
    w.objects_of_class_for_player(f, player, c, r, got, grid);
    std::vector<ObjectId> want = scan_player(w, f, player, c, r);
    // The whole map through the grid finds nothing held, nor anything at the
    // held marker.
    if (r <= 0) {
      want.erase(std::remove_if(want.begin(), want.end(),
                                [&](ObjectId id) {
                                  const WorldObject* slot = w.find(id);
                                  return slot->state.is_held() ||
                                         slot->state.position == kHeldPosition;
                                }),
                 want.end());
    }
    tally.compare_grid(w, got, want);
    w.objects_of_class_for_player_in_rect(f, player, l, t, rr, b, got, grid);
    tally.compare_grid(w, got, scan_player_rect(w, f, player, l, t, rr, b));
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
      CHECK(after.reordered == 0);
      CHECK(after.nonempty > 0);
      // The order inside every cell came back with the stamps: the whole
      // map's walk is the saved world's, id for id.
      std::vector<ObjectId> before;
      std::vector<ObjectId> since;
      world.objects_located_in_rect(-40000, -40000, 40000, 40000, before, SweepOrder::grid);
      loaded.objects_located_in_rect(-40000, -40000, 40000, 40000, since, SweepOrder::grid);
      CHECK(before == since);
    }
  }
  CHECK(tally.mismatches == 0);
  CHECK(tally.reordered == 0);
  CHECK(inconsistent == 0);
  // And the run was a real one: queries answered with something, and objects
  // were held while they were asked about.
  CHECK(tally.nonempty > tally.queries / 4);
  CHECK(held_seen > 1000);
}

TEST(spatial_index_finds_no_held_object_and_files_none) {
  // A held object stands in no grid cell: its position is `(-1, -1)`, which
  // 0x0053d030 maps to no cell, so no area sweep in the original reaches it.
  // This index once kept held objects in a list of their own and resolved each
  // to its holder at every query -- found here at the fort, and in a real
  // match, where a garrison's holder record stands at (0, 0), in the corner.
  World world;
  const ObjectId fort = world.spawn(NativeClass::building, nullptr);
  const ObjectId inside = world.spawn(NativeClass::unit, nullptr);
  (void)world.set_position(fort, Point{1000, 1000});
  CHECK(world.put_in_holder(inside, fort));
  std::vector<ObjectId> found;
  world.objects_in_radius(Point{1000, 1000}, 10, ClassFilter{}, found);
  CHECK(found.size() == 1 && found[0] == fort);
  world.objects_in_rect(-10, -10, 10, 10, ClassFilter{}, found);
  CHECK(found.empty());
  world.objects_located_in_rect(-10, -10, 1010, 1010, found);
  CHECK(found.size() == 1 && found[0] == fort);
  CHECK(world.spatial_index_consistent());

  // Out, and found where it was put.
  CHECK(world.remove_from_holder(inside, Point{200, 200}));
  world.objects_in_radius(Point{200, 200}, 0 + 1, ClassFilter{}, found);
  CHECK(found.size() == 1 && found[0] == inside);
  CHECK(world.spatial_index_consistent());
}

}  // namespace

namespace {

/// Four objects in two cells of row 0 and one of row 1, spawned `a`, `b`,
/// `c`, `d`.
struct Quad {
  World world;
  ObjectId a = kNoObject;
  ObjectId b = kNoObject;
  ObjectId c = kNoObject;
  ObjectId d = kNoObject;
  Quad() {
    a = world.spawn(NativeClass::unit, nullptr);
    b = world.spawn(NativeClass::unit, nullptr);
    c = world.spawn(NativeClass::unit, nullptr);
    d = world.spawn(NativeClass::unit, nullptr);
    (void)world.set_position(a, Point{600, 50});  // row 0, cell 2
    (void)world.set_position(b, Point{50, 300});  // row 1, cell 0
    (void)world.set_position(c, Point{10, 10});   // row 0, cell 0
    (void)world.set_position(d, Point{20, 20});   // row 0, cell 0, after c
    for (const ObjectId id : {a, b, c, d}) (void)world.set_health(id, 100);  // alive
  }
  std::vector<ObjectId> walk(SweepOrder order) const {
    std::vector<ObjectId> out;
    world.objects_in_rect(0, 0, 1000, 1000, ClassFilter{}, out, order);
    return out;
  }
};

}  // namespace

TEST(the_grid_walks_rows_then_cells_then_each_cells_list) {
  // 0x0053d030: 256-unit cells, 128 to a row. Every sweep walks the box's
  // rows top to bottom and each row's cells left to right (0x004fde60 and the
  // rest), and a cell's list from its head.
  Quad q;
  CHECK((q.walk(SweepOrder::grid) == std::vector<ObjectId>{q.c, q.d, q.a, q.b}));
  CHECK((q.walk(SweepOrder::spawn) == std::vector<ObjectId>{q.a, q.b, q.c, q.d}));
  // A 255/256 border: x = 255 is cell 0, x = 256 cell 1.
  (void)q.world.set_position(q.a, Point{256, 0});
  (void)q.world.set_position(q.b, Point{255, 0});
  CHECK((q.walk(SweepOrder::grid) == std::vector<ObjectId>{q.c, q.d, q.b, q.a}));
}

TEST(a_cell_lists_its_objects_in_the_order_they_came_into_it) {
  // 0x0053f000 relinks an object only when its new point is in another cell:
  // out of the old list (0x0053d630) and onto the new one's tail (0x0053ce50).
  Quad q;
  // Inside its cell: the list is not touched, so `c` stays ahead of `d`.
  (void)q.world.set_position(q.c, Point{200, 200});
  CHECK((q.walk(SweepOrder::grid) == std::vector<ObjectId>{q.c, q.d, q.a, q.b}));
  // Out and back: `c` comes in behind `d`.
  (void)q.world.set_position(q.c, Point{5000, 5000});
  (void)q.world.set_position(q.c, Point{30, 30});
  CHECK((q.walk(SweepOrder::grid) == std::vector<ObjectId>{q.d, q.c, q.a, q.b}));
  // Into a holder and out again: the same, a held object being in no cell.
  const ObjectId fort = q.world.spawn(NativeClass::building, nullptr);
  (void)q.world.set_position(fort, Point{9000, 9000});
  CHECK(q.world.put_in_holder(q.d, fort));
  CHECK(q.world.remove_from_holder(q.d, Point{40, 40}));
  CHECK((q.walk(SweepOrder::grid) == std::vector<ObjectId>{q.c, q.d, q.a, q.b}));
  CHECK(q.world.spatial_index_consistent());
}

TEST(a_query_answers_in_grid_order_and_a_load_keeps_it) {
  // The area queries' refreshes walk the grid (0x004fde60 for a circle), so
  // `ObjsInCircle` lists in the walk's order, cell lists and all; and the
  // order of a cell's list, being history, is saved.
  Quad q;
  (void)q.world.set_position(q.c, Point{5000, 5000});
  (void)q.world.set_position(q.c, Point{30, 30});
  const std::vector<ObjectId> expected{q.d, q.c, q.a, q.b};
  const ObjectId circle = q.world.create_query(objs_in_circle(Point{0, 0}, 2000, ClassFilter{}));
  std::vector<ObjectId> got;
  q.world.evaluate_query(circle, got);
  CHECK(got == expected);

  std::vector<std::byte> bytes;
  q.world.serialize(bytes);
  World loaded;
  CHECK(loaded.deserialize(bytes).ok());
  CHECK(loaded.spatial_index_consistent());
  std::vector<ObjectId> again;
  loaded.objects_in_rect(0, 0, 1000, 1000, ClassFilter{}, again, SweepOrder::grid);
  CHECK(again == expected);
  // And the next arrival is stamped after everything already there.
  (void)loaded.set_position(q.d, Point{5000, 5000});
  (void)loaded.set_position(q.d, Point{40, 40});
  loaded.objects_in_rect(0, 0, 1000, 1000, ClassFilter{}, again, SweepOrder::grid);
  CHECK((again == std::vector<ObjectId>{q.c, q.d, q.a, q.b}));
}
