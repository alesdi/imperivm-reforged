// Map areas: the named circles and rectangles scripts mean by a place.
// See include/imperivm/core/sim/area.hpp for what is proven and what is not.

#include "imperivm/core/sim/area.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/world/map.hpp"

namespace imperivm::core::sim {
namespace {

/// Truncating integer division toward zero, which is what C already does for
/// `int32` and what `cdq; sub; sar 1` computes in the executable. Written out
/// so the reader does not have to take the sign rule on trust.
[[nodiscard]] constexpr std::int32_t midpoint(std::int32_t a, std::int32_t b) noexcept {
  return (a + b) / 2;
}

}  // namespace

// --------------------------------------------------------------------------
// the shape
// --------------------------------------------------------------------------

AreaShape AreaShape::of_circle(Point center, std::int32_t radius) noexcept {
  AreaShape out;
  out.kind = AreaKind::circle;
  out.center = center;
  out.radius = radius < 0 ? 0 : radius;
  return out;
}

AreaShape AreaShape::of_rectangle(std::int32_t left, std::int32_t top, std::int32_t right,
                                  std::int32_t bottom) noexcept {
  AreaShape out;
  out.kind = AreaKind::rectangle;
  out.left = std::min(left, right);
  out.right = std::max(left, right);
  out.top = std::min(top, bottom);
  out.bottom = std::max(top, bottom);
  return out;
}

Point AreaShape::centre() const noexcept {
  if (kind == AreaKind::circle) return center;
  return Point{midpoint(left, right), midpoint(top, bottom)};
}

AreaBounds AreaShape::bounds() const noexcept {
  if (kind == AreaKind::circle) {
    return AreaBounds{center.x - radius, center.y - radius, center.x + radius,
                      center.y + radius};
  }
  return AreaBounds{left, top, right, bottom};
}

bool AreaShape::contains(Point p) const noexcept {
  if (kind == AreaKind::circle) {
    const std::int64_t dx = static_cast<std::int64_t>(p.x) - center.x;
    const std::int64_t dy = static_cast<std::int64_t>(p.y) - center.y;
    // `isqrt(dx*dx + dy*dy) <= r`, spelt without the square root. See the
    // header: the executable's test truncates, so the rim shell at exactly
    // `r*r < d2 < (r+1)^2` is *inside*, and `d2 <= r*r` would exclude it.
    const std::int64_t reach = static_cast<std::int64_t>(radius) + 1;
    return dx * dx + dy * dy < reach * reach;
  }
  return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom;
}

bool AreaShape::contains_by_query_rule(Point p) const noexcept {
  if (kind == AreaKind::circle) {
    const std::int64_t dx = static_cast<std::int64_t>(p.x) - center.x;
    const std::int64_t dy = static_cast<std::int64_t>(p.y) - center.y;
    // `r*r`, not `(r+1)*(r+1)`. The one-unit difference from `contains` above
    // is deliberate and is the whole reason both functions exist.
    const std::int64_t limit = static_cast<std::int64_t>(radius) * radius;
    return dx * dx + dy * dy <= limit;
  }
  return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom;
}

std::int32_t AreaShape::distance_to(Point p) const noexcept {
  std::int64_t distance = 0;
  if (kind == AreaKind::circle) {
    const std::int64_t dx = static_cast<std::int64_t>(p.x) - center.x;
    const std::int64_t dy = static_cast<std::int64_t>(p.y) - center.y;
    distance = isqrt(dx * dx + dy * dy) - radius;
  } else {
    // Chebyshev, not Euclidean, and not a mistake: 0x004d8b6e keeps the larger
    // of the two axis gaps. Which side of each axis `p` falls on decides the
    // sign, exactly as the two `cmp/jge` pairs above it do.
    const std::int64_t dx = p.x < left ? static_cast<std::int64_t>(left) - p.x
                                       : static_cast<std::int64_t>(p.x) - right;
    const std::int64_t dy = p.y < top ? static_cast<std::int64_t>(top) - p.y
                                      : static_cast<std::int64_t>(p.y) - bottom;
    distance = std::max(dx, dy);
  }
  if (distance < 0) distance = 0;
  return static_cast<std::int32_t>(distance);
}

AreaShape area_shape_from_map(const MapArea& authored) noexcept {
  if (authored.is_circle()) {
    return AreaShape::of_circle(Point{authored.ptx, authored.pty}, authored.radius);
  }
  return AreaShape::of_rectangle(authored.left, authored.top, authored.right,
                                 authored.bottom);
}

// --------------------------------------------------------------------------
// the table
// --------------------------------------------------------------------------

std::size_t AreaTable::lower_bound(ObjectId id) const noexcept {
  const auto it = std::lower_bound(
      entries_.begin(), entries_.end(), id,
      [](const Entry& entry, ObjectId key) { return entry.id < key; });
  return static_cast<std::size_t>(it - entries_.begin());
}

bool AreaTable::bind(ObjectId id, const AreaShape& shape) {
  if (id == kNoObject) return false;
  const std::size_t at = lower_bound(id);
  if (at < entries_.size() && entries_[at].id == id) return false;
  entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(at), Entry{id, shape});
  return true;
}

const AreaShape* AreaTable::find(ObjectId id) const noexcept {
  if (id == kNoObject) return nullptr;
  const std::size_t at = lower_bound(id);
  if (at >= entries_.size() || entries_[at].id != id) return nullptr;
  return &entries_[at].shape;
}

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

void AreaSystem::advance(World& world, const Turn& turn) {
  // An area is geometry. Nothing about it changes as the game runs, and this
  // is not a placeholder for something that will.
  (void)world;
  (void)turn;
}

AreaSystem* area_system_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "areas") {
      return static_cast<AreaSystem*>(system);
    }
  }
  return nullptr;
}

AreaLoadReport load_areas(const MapObjectList& map, const World& world, AreaTable& out) {
  AreaLoadReport report;
  report.authored = map.areas().size();
  if (report.authored == 0) return report;

  // Which map `num`s have been claimed by an alias, so that an area named by
  // nothing can be counted rather than assumed away.
  std::vector<std::int32_t> named;
  named.reserve(map.areas().size());

  // Names already seen, in document order, so that a repeated alias resolves
  // the way `NamedObjectTable::bind` resolved it: first wins.
  std::vector<std::string_view> seen;

  for (const MapGroup& group : map.groups()) {
    if (group.type != imperivm::core::kGroupAlias) continue;
    if (group.members.empty()) continue;
    if (std::find(seen.begin(), seen.end(), std::string_view{group.name}) != seen.end()) {
      continue;
    }
    seen.emplace_back(group.name);

    const MapArea* authored = map.find_area(group.members.front());
    if (authored == nullptr) continue;
    named.push_back(authored->num);

    const ObjectId id = world.named_object(group.name);
    if (id == kNoObject) {
      // The alias named an object the loader did not spawn -- an unresolved
      // class. The area exists in the file and nowhere else.
      ++report.unresolved;
      continue;
    }
    if (out.bind(id, area_shape_from_map(*authored))) ++report.bound;
  }

  for (const MapArea& authored : map.areas()) {
    if (std::find(named.begin(), named.end(), authored.num) == named.end()) ++report.unnamed;
  }
  return report;
}

// --------------------------------------------------------------------------
// geometry against the world
// --------------------------------------------------------------------------

const AreaShape* area_named(World& world, std::string_view name) noexcept {
  if (name.empty()) return nullptr;
  const AreaSystem* areas = area_system_of(world);
  if (areas == nullptr) return nullptr;
  return areas->areas().find(world.named_object(name));
}

Point random_point_in_area(World& world, const AreaShape& shape, std::int32_t attempts) {
  // 1. No attempts at all is the centre, and draws nothing. 0x004d6ecb reaches
  //    the same return the successful path does, with the buffer still holding
  //    what the centre helper wrote into it.
  if (attempts <= 0) return shape.centre();

  const AreaBounds box = shape.bounds();
  const MovementSystem* movement = movement_system(world);
  const ObstructionGrid* grid = movement != nullptr ? &movement->grid() : nullptr;
  if (grid != nullptr && grid->empty()) grid = nullptr;

  Point drawn = shape.centre();
  std::int32_t used = 0;
  // A circle rejection does not consume an attempt, so the loop needs its own
  // ceiling or a degenerate shape could spin forever. The executable has no
  // such bound -- it jumps straight back over the counter -- and gets away
  // with it because the bounding box of a circle is never more than 4/pi times
  // its area. This bound is generous enough never to bite on a real area and
  // is documented as a divergence rather than hidden as a tweak.
  constexpr std::int32_t kMaxDraws = 4096;
  for (std::int32_t draws = 0; draws < kMaxDraws; ++draws) {
    // 2. Two draws per attempt, x first. Which order the two come off the
    //    stream is world state, so it is not a detail.
    const std::int32_t x = world.rng().between(box.left, box.right);
    const std::int32_t y = world.rng().between(box.top, box.bottom);
    drawn = Point{x, y};

    // 3. Outside the shape: redraw, without consuming an attempt.
    if (!shape.contains(drawn)) continue;

    // 4. Blocked ground: consume an attempt and redraw.
    if (grid == nullptr || !grid->blocked(drawn)) return drawn;
    if (++used >= attempts) break;
  }
  // 5. Out of attempts. The executable returns the last point it drew anyway,
  //    blocked or not: a unit ordered onto blocked ground walks as close as it
  //    can, which is better than one that refuses to move.
  return drawn;
}

// --------------------------------------------------------------------------
// the host slice -- see the declaration in sim/area.hpp
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::Value;

constexpr const char* kNoWorld = "area: no World behind CallContext::user";
constexpr const char* kNoName = "area: argument is not an area name";
constexpr const char* kNoQuery = "area: receiver is not a query handle";
constexpr const char* kNoCommands = "area: no command system registered with the world";

/// What `AreaCenter` answers for a name that resolves to nothing: 0x004d8cb7
/// writes `0xffffffff` into both components of the returned point.
constexpr Point kUnknownAreaCentre{-1, -1};
/// And what `GetRandomPointInArea` answers: 0x004d91dc loads `0x3e8` into both.
constexpr Point kUnknownAreaPoint{1000, 1000};

/// The area a string argument names, or null.
[[nodiscard]] const AreaShape* area_arg(World& world, const Value& value) noexcept {
  if (!value.is_string()) return nullptr;
  return area_named(world, value.as_string());
}

/// `AreaCenter(name)`. 279 sites.
HostOutcome fn_area_centre(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (!ctx.arg(0).is_string()) return HostOutcome::failed(kNoName);
  const AreaShape* shape = area_arg(*world, ctx.arg(0));
  // A missing area is a diagnostic in the original, not a fault: it prints
  // "Could not find area named '%s' in function 'AreaCenter'. Check the
  // spelling." and returns (-1, -1). Reproducing the value matters because
  // scripts run on past it.
  if (shape == nullptr) return HostOutcome::ok_with(pack_point(kUnknownAreaCentre));
  return HostOutcome::ok_with(pack_point(shape->centre()));
}

/// `GetRandomPointInArea(name)`. 31 sites. Draws from `World::rng()`.
HostOutcome fn_random_point_in_area(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (!ctx.arg(0).is_string()) return HostOutcome::failed(kNoName);
  const AreaShape* shape = area_arg(*world, ctx.arg(0));
  // (1000, 1000) rather than (-1, -1): a different function, a different
  // sentinel, and both are in the executable.
  if (shape == nullptr) return HostOutcome::ok_with(pack_point(kUnknownAreaPoint));
  return HostOutcome::ok_with(pack_point(random_point_in_area(*world, *shape)));
}

/// `AreaDistTo(point, name)`. 1 site.
HostOutcome fn_area_dist_to(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("AreaDistTo: expected a point");
  if (!ctx.arg(1).is_string()) return HostOutcome::failed(kNoName);
  const AreaShape* shape = area_arg(*world, ctx.arg(1));
  // The original's miss path is a copy of `AreaCenter`'s -- same message, same
  // `0xffffffff` -- written into a slot the caller reads as an int. -1 is the
  // low half of what it writes and is the only reading that is not garbage.
  if (shape == nullptr) return HostOutcome::ok_with(Value::integer(-1));
  return HostOutcome::ok_with(Value::integer(shape->distance_to(unpack_point(ctx.arg(0)))));
}

/// The shared body of `AttackArea` and `MoveToArea`.
///
/// 0x0057a2e0. Both entry points are two instructions that push a verb and
/// tail into this, so there is one function here for the same reason.
HostOutcome order_query_to_area(CallContext& ctx, std::string_view verb) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (!ctx.arg(0).is_object() || ctx.arg(0).as_object().type != kTypeQuery) {
    return HostOutcome::failed(kNoQuery);
  }
  if (!ctx.arg(1).is_string()) return HostOutcome::failed(kNoName);

  const AreaShape* shape = area_arg(*world, ctx.arg(1));
  // "Could not find area named '%s' in function 'Query::AttackArea or
  // Query::MoveToArea'. Check the spelling." -- printed, and nothing ordered.
  if (shape == nullptr) return HostOutcome::ok_void();

  CommandSystem* commands = command_system(*world);
  if (commands == nullptr) return HostOutcome::failed(kNoCommands);

  std::vector<ObjectId> members;
  world->evaluate_query(ctx.arg(0).as_object().id, members);

  const AreaShape region = *shape;
  for (const ObjectId member : members) {
    // One sample per member, so the query spreads out over the area instead of
    // stacking on its centre. That is the executable's shape, not a nicety:
    // the loop at 0x0057a440 calls the sampler inside the body.
    Command order;
    order.arg_kind = CommandArgKind::point;
    order.point = random_point_in_area(*world, region);
    (void)commands->set_command(*world, member, verb, order);
  }
  return HostOutcome::ok_void();
}

/// `AttackArea(query, name)`. 263 sites. The verb is `advance`, proven at
/// 0x0057a514.
HostOutcome fn_attack_area(CallContext& ctx) { return order_query_to_area(ctx, "advance"); }

/// `MoveToArea(query, name)`. 8 sites. `move`, proven at 0x0057a534.
HostOutcome fn_move_to_area(CallContext& ctx) { return order_query_to_area(ctx, "move"); }

/// A class filter from a script argument. `world_host.cpp` has the same three
/// lines in its own anonymous namespace; sharing them would mean exporting a
/// helper from a file this slice does not own.
[[nodiscard]] ClassFilter filter_arg(World& world, const Value& value) {
  if (!value.is_string()) return ClassFilter{};  // match_all
  return ClassFilter::parse(value.as_string(), world.class_graph());
}

[[nodiscard]] Value query_value(ObjectId id) {
  if (id == kNoObject) return Value::object(script::ObjectRef{script::kNoType, 0});
  return Value::object(script::ObjectRef{kTypeQuery, id});
}

[[nodiscard]] Value invalid_object() {
  return Value::object(script::ObjectRef{script::kNoType, 0});
}

/// `AreaObjs("area", "class")`. 193 sites, of which 84 of the 187 whose name
/// resolves in their own map declare a **rectangle**.
///
/// Both shapes are exact rather than approximated, because the executable's
/// query type is a template over the area type and both instantiations exist:
/// `CVXMapAreaQuery<TCircleArea>` at 0x007bc814 and `CVXMapAreaQuery<TRectArea>`
/// at 0x007bc834. This is what it builds here (0x00577278), and
/// `QueryKind::map_area_circle` and `QueryKind::map_area_rect` are the two.
HostOutcome fn_area_objs(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (!ctx.arg(0).is_string()) return HostOutcome::failed(kNoName);
  const AreaShape* shape = area_arg(*world, ctx.arg(0));
  // 0x00577196 returns a Query handle whose id is 0xffff -- an invalid handle,
  // not a fault. `.count` on it is zero and `.IsEmpty` is true.
  if (shape == nullptr) return HostOutcome::ok_with(invalid_object());

  const ClassFilter filter = filter_arg(*world, ctx.arg(1));
  // The area's own corners, not its bounding box: `AreaShape::of_rectangle`
  // already sorted them, and `bounds()` on a *circle* would silently turn one
  // into a square, which is the approximation this entry point refuses to make.
  const QuerySpec spec = shape->kind == AreaKind::circle
                             ? objs_in_circle(shape->centre(), shape->radius, filter)
                             : objs_in_rect(shape->left, shape->top, shape->right,
                                            shape->bottom, filter);
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

/// `ClassPlayerAreaObjs("class", player, "area")`. 274 sites.
///
/// The player argument is **1-based**, like every other player number a script
/// sees (`sim/player_host.hpp` gathers the evidence, including five error
/// strings in the executable that say "should be between 1 and 16"). Every
/// corpus site passes 1..5. Note that `ClassPlayerObjs/2` in
/// `sim/world_host.cpp` reads the same argument as 0-based; that file records
/// the disagreement as a bug of its own and it is not compensated for here.
HostOutcome fn_class_player_area_objs(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (!ctx.arg(2).is_string()) return HostOutcome::failed(kNoName);
  const AreaShape* shape = area_arg(*world, ctx.arg(2));
  if (shape == nullptr) return HostOutcome::ok_with(invalid_object());

  const PlayerId player =
      ctx.arg(1).is_integer() ? player_from_script(ctx.arg(1).as_integer()) : kNoPlayer;
  const ClassFilter filter = filter_arg(*world, ctx.arg(0));
  // One query type, two shapes. `CVXClassPlayerAreaQuery::Refresh` (0x004fe860)
  // reads the area's `[area+0x148]` discriminator and calls 0x004fd060 for a
  // circle or 0x004fd2d0 for a rectangle; `QuerySpec::area_is_rect` is that
  // discriminator, and its comment carries the rest of the evidence.
  const QuerySpec spec =
      shape->kind == AreaKind::circle
          ? class_player_area(filter, player, shape->centre(), shape->radius)
          : class_player_area_rect(filter, player, shape->left, shape->top, shape->right,
                                   shape->bottom);
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

/// The number of `define` calls `register_area_host` makes. Kept next to the
/// list so the two cannot drift.
constexpr std::size_t kEntryCount = 7;

}  // namespace

std::size_t area_host_entry_count() noexcept { return kEntryCount; }

std::size_t register_area_host(HostRegistry& registry) {
  std::size_t defined = 0;
  const auto free_fn = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::free_function, name, arity, fn);
    ++defined;
  };

  // Descending corpus call frequency over the whole 885-script install.
  free_fn("AreaCenter", 1, &fn_area_centre);                       // 279
  free_fn("ClassPlayerAreaObjs", 3, &fn_class_player_area_objs);   // 274
  free_fn("AttackArea", 2, &fn_attack_area);                       // 263
  free_fn("AreaObjs", 2, &fn_area_objs);                           // 193
  free_fn("GetRandomPointInArea", 1, &fn_random_point_in_area);    // 31
  free_fn("MoveToArea", 2, &fn_move_to_area);                      // 8
  free_fn("AreaDistTo", 2, &fn_area_dist_to);                      // 1

  return defined;
}

}  // namespace imperivm::core::sim
