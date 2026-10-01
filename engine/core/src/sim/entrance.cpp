#include "imperivm/core/sim/entrance.hpp"

#include <algorithm>
#include <string_view>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/projection.hpp"

namespace imperivm::core::sim {
namespace {

/// The largest a terrain height byte can be, which is the margin the screen
/// rectangle's southern edge carries.
constexpr std::int32_t kMaxTerrainHeight = 255;

/// The class whose descendants get the empty-list fixup, and the only class
/// name this file knows.
constexpr std::string_view kShipyardClass = "BaseShipyard";

/// The map's inclusive high corner, in world units.
///
/// **Restated rather than shared.** `sim/flying.cpp` keeps a private copy for
/// its own clamping and `sim/ai.cpp` restates `normalise` for the same reason: a
/// second definition that agreed is cheaper than an export, and a second one
/// that *disagreed* would be worse than either. The rule is `GetMapRect`'s --
/// `(0, 0, w - 1, h - 1)` from the match rules, falling back to the height
/// layer's extent, which is the same square by construction.
[[nodiscard]] std::int32_t map_high(const World& world) noexcept {
  if (const MatchSystem* match = match_system_of(world); match != nullptr) {
    const std::int32_t size = match->rules().map_size;
    if (size > 0) return size - 1;
  }
  const std::uint32_t extent = world.height().extent_x();
  return extent > 0 ? static_cast<std::int32_t>(extent) - 1 : 0;
}

/// The entity record behind an object, or null.
[[nodiscard]] const Entity* entity_of(const World& world, ObjectId object) noexcept {
  const WorldObject* slot = world.find(object);
  if (slot == nullptr || slot->object == nullptr) return nullptr;
  return slot->object->entity;
}

/// The terrain byte under a world point, with the round-to-nearest the original
/// applies before it divides. Off the layer answers 0, which is not deep water.
///
/// The same sampling `sim/lsa.cpp` documents, and it has to stay the same: the
/// two disagree only about points within half a cell of a coast, which is
/// exactly where a shipyard's doors are.
}  // namespace

std::uint32_t terrain_at(const Grid& terrain, Point where) noexcept {
  if (terrain.cell_size() == 0) return 0;
  const auto half = static_cast<std::int32_t>(terrain.cell_size() / 2);
  const std::int32_t x = where.x + half;
  const std::int32_t y = where.y + half;
  if (x < 0 || y < 0) return 0;
  const auto cx = static_cast<std::uint32_t>(x) / terrain.cell_size();
  const auto cy = static_cast<std::uint32_t>(y) / terrain.cell_size();
  if (cx >= terrain.width() || cy >= terrain.height()) return 0;
  return terrain.cell(cx, cy);
}

namespace {

[[nodiscard]] bool is_deep_water(const World& world, Point where) noexcept {
  return terrain_at(world.terrain(), where) == kDeepWaterIndex;
}

/// Every class slot of `object` whose round trip lands somewhere, of **every**
/// type and before any passability test.
///
/// This is the baker's fifth list, and it exists only to feed the shipyard
/// fixup. Calling it anything else would invite it to be used as "the building's
/// points", which it is not: a type-3 patrol station is in here.
void landing_slots(const World& world, ObjectId object, std::vector<Point>& out) {
  const Entity* entity = entity_of(world, object);
  if (entity == nullptr) return;
  for (const EntityPoint& point : entity->points()) {
    const Point landed = class_point_in_world(world, object, point.x, point.y);
    if (landed != kNoClassPoint) out.push_back(landed);
  }
}

/// The eight neighbours the fixup tries, in the order it tries them, in
/// passability cells. West, east, north, south, then the diagonals south-west,
/// north-west, north-east, south-east -- and the order is the whole of the
/// answer, because the first hit wins.
constexpr std::int32_t kFixupSteps[8][2] = {{-1, 0}, {1, 0},   {0, -1}, {0, 1},
                                            {-1, 1}, {-1, -1}, {1, -1}, {1, 1}};

/// The `BaseShipyard` fixup: one point, appended to an otherwise empty list.
void fill_empty_shipyard_list(const World& world, ObjectId building, bool water,
                              std::vector<Point>& out) {
  std::vector<Point> candidates;
  landing_slots(world, building, candidates);
  const std::int32_t high = map_high(world);
  for (const Point& from : candidates) {
    for (const auto& step : kFixupSteps) {
      const Point at{from.x + step[0] * kCollisionCellSize,
                     from.y + step[1] * kCollisionCellSize};
      // **Strict**, where the baker's own bounds test is inclusive. Transcribed
      // rather than harmonised: the two really do differ in the executable.
      if (at.x <= 0 || at.y <= 0 || at.x >= high || at.y >= high) continue;
      // And no passability test at all here, which the baker does apply. A
      // shipyard can be given a door on blocked ground, and is.
      if (is_deep_water(world, at) != water) continue;
      out.push_back(at);
      return;
    }
  }
}

/// `IsHeirOf`, for the one class name this file tests.
[[nodiscard]] bool descends_from(const World& world, ObjectId object,
                                 std::string_view name) noexcept {
  const WorldObject* slot = world.find(object);
  if (slot == nullptr) return false;
  const ClassFilter filter = ClassFilter::parse(name, world.class_graph());
  if (filter.match_all) return false;
  return world.matches_filter(*slot, filter);
}

/// The fixed-point shift the weighted picker carries its ratios in.
///
/// Twenty bits, which is what keeps the whole of `pick_door_by_distance` inside
/// `std::int64_t`. The two products that could overflow are the numerator
/// `nearest << 20`, safe for any squared distance under `2^43` where the widest
/// shipped map reaches about `2^31`, and the squaring of a ratio that is `2^20`
/// at its largest.
constexpr int kWeightShift = 20;
constexpr std::int64_t kWeightOne = std::int64_t{1} << kWeightShift;

/// The original's `rand(0, 10000)`, and it is inclusive at *both* ends.
///
/// The shipped `rand(n)` (`0x004c63a0`) reaches the same generator entry point
/// with `n - 1` as its upper argument, and `sim/rng.hpp` records the three call
/// sites that prove `rand(n)` is `[0, n)`. So the entry point is `[low, high]`
/// and this draw has ten thousand and *one* outcomes, not ten thousand.
constexpr std::int32_t kWeightDrawRange = 10000;

[[nodiscard]] std::int64_t squared_distance(Point a, Point b) noexcept {
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
  const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
  return dx * dx + dy * dy;
}

/// A door's weight as a fraction of the nearest door's, in `kWeightShift`
/// fixed point: the fourth power of the distance ratio, floored at one.
///
/// **The floor is not rounding slack.** The original's weights are `double` and
/// never reach zero, so its `threshold == 0` draw always answers the *first*
/// element; a weight that truncated to zero here would skip that element and
/// answer a later one on a draw that happens one time in ten thousand and one.
/// One part in a million is also close enough to "never" for the far door it
/// describes, which is what the original's own arithmetic says about it.
[[nodiscard]] std::int64_t door_weight(std::int64_t d2, std::int64_t nearest) noexcept {
  // A door standing exactly on the reference point: the original's `1e-7` makes
  // it ten million times its closest possible rival, integer coordinates making
  // `d2 == 1` the closest a rival can be. It wins, and ties with its equals.
  if (nearest == 0) return d2 == 0 ? kWeightOne : 1;
  const std::int64_t ratio = (nearest << kWeightShift) / d2;
  return std::max<std::int64_t>(1, (ratio * ratio) >> kWeightShift);
}

}  // namespace

ScreenBounds screen_bounds(const World& world) noexcept {
  const std::int32_t high = map_high(world);
  ScreenBounds bounds;
  // The map rectangle's low corner is the origin, so `min_x` is 0 and `min_y`
  // is the projection of 0, which is also 0. Written out rather than assumed,
  // because the rule is "project the map rectangle" and a map that did not
  // start at the origin would need both.
  bounds.min_x = 0;
  bounds.min_y = project_scale(0);
  bounds.max_x = high - kHeightCellSize;
  bounds.max_y = project_scale(high) - kMaxTerrainHeight - kHeightCellSize;
  return bounds;
}

Point class_point_in_world(const World& world, ObjectId object, std::int32_t dx,
                           std::int32_t dy) noexcept {
  const WorldObject* slot = world.find(object);
  if (slot == nullptr) return kNoClassPoint;

  // Project, offset, unproject. The offset is in **screen** pixels -- that is
  // what `<point x= y=>` authors -- so it has to be applied between the two.
  const Point screen = world_to_screen(world, world.resolve_position(object));
  const Point at{screen.x + dx, screen.y + dy};

  const ScreenBounds bounds = screen_bounds(world);
  if (at.x < bounds.min_x || at.x > bounds.max_x) return kNoClassPoint;
  if (at.y < bounds.min_y || at.y > bounds.max_y) return kNoClassPoint;

  const Point landed = screen_to_world(world, at);
  // **Belt and braces, and the original's too.** The screen rectangle's two
  // margins exist precisely so that this can never fire: a screen row at or
  // below `project(max_y) - 255 - 32` unprojects inside the map for *every*
  // terrain height, because 255 is the largest one there is. So a fault that
  // deletes this line survives the whole suite, and it is kept for the same
  // reason the original keeps it -- it is the check that would catch a height
  // layer that broke that assumption, and it is one comparison.
  const std::int32_t high = map_high(world);
  if (landed.x < 0 || landed.y < 0 || landed.x > high || landed.y > high) return kNoClassPoint;
  return landed;
}

void class_points_of_type(const World& world, ObjectId object, std::int32_t type,
                          std::vector<Point>& out) {
  const Entity* entity = entity_of(world, object);
  if (entity == nullptr) return;
  for (const EntityPoint& point : entity->points()) {
    if (point.type != type) continue;
    const Point landed = class_point_in_world(world, object, point.x, point.y);
    if (landed != kNoClassPoint) out.push_back(landed);
  }
}

bool class_has_point_type(const World& world, ObjectId object, std::int32_t type) noexcept {
  const Entity* entity = entity_of(world, object);
  if (entity == nullptr) return false;
  for (const EntityPoint& point : entity->points()) {
    if (point.type == type) return true;
  }
  return false;
}

void building_doors(const World& world, const ObstructionGrid* passability, ObjectId building,
                    std::int32_t type, bool water, std::vector<Point>& out) {
  const Entity* entity = entity_of(world, building);
  if (entity == nullptr) return;
  const std::size_t before = out.size();
  for (const EntityPoint& point : entity->points()) {
    if (point.type != type) continue;
    const Point landed = class_point_in_world(world, building, point.x, point.y);
    if (landed == kNoClassPoint) continue;
    // **Impassable is discarded outright**, before the land/water split rather
    // than after it: a door nobody can stand in is not a door of either kind.
    if (passability != nullptr &&
        passability->blocked_cell(ObstructionGrid::cell_of(landed.x),
                                  ObstructionGrid::cell_of(landed.y))) {
      continue;
    }
    if (is_deep_water(world, landed) != water) continue;
    out.push_back(landed);
  }
  if (out.size() == before && descends_from(world, building, kShipyardClass)) {
    fill_empty_shipyard_list(world, building, water, out);
  }
}

Point pick_door_by_distance(const std::vector<Point>& doors, Point from, Rng& rng) {
  if (doors.empty()) return kNoClassPoint;

  std::int64_t nearest = squared_distance(doors.front(), from);
  for (const Point& door : doors) nearest = std::min(nearest, squared_distance(door, from));

  std::int64_t total = 0;
  for (const Point& door : doors) total += door_weight(squared_distance(door, from), nearest);

  // The draw happens here whatever the list looks like, because whether the
  // stream advanced is synchronised state and a one-door building must cost the
  // same as a four-door one.
  const std::int64_t draw = rng.between(0, kWeightDrawRange);

  // `running / total > draw / kWeightDrawRange`, cross-multiplied. The weights
  // are floored at one, so `total` is never zero and the division the original
  // does is never needed.
  const std::int64_t bar = draw * total;
  std::int64_t running = 0;
  for (const Point& door : doors) {
    running += door_weight(squared_distance(door, from), nearest);
    if (running * kWeightDrawRange > bar) return door;
  }
  // Only reachable on the largest draw, and the original answers the same way.
  return doors.back();
}

}  // namespace imperivm::core::sim
