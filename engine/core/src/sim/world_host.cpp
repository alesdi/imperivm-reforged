// The object-model half of the .vs host API.
// See include/imperivm/core/sim/world_host.hpp.

#include "imperivm/core/sim/siege_fire.hpp"
#include "imperivm/core/sim/gaika.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/boarding.hpp"
#include "imperivm/core/sim/area.hpp"

#include "imperivm/core/sim/array.hpp"
#include "imperivm/core/sim/entrance.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/hooks.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/match.hpp"

#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/objlist.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>
#include <vector>

#include "imperivm/core/script/scheduler.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::HostOutcome;
using script::ObjectRef;
using script::Value;

constexpr script::CallKind kFree = script::CallKind::free_function;
constexpr script::CallKind kMember = script::CallKind::member;

// `CallContext::user` is a `HostContext*`; `sim/host_context.hpp` supplies the
// accessor and records why every domain must agree on the type.

/// The object a value refers to, or null.
///
/// **The handle check every `Obj`-family body opens with.** A handle is eight
/// bytes: a table slot in the low 16 bits of the first word and an offset in
/// the second. The object is the pointer in the table at 0x00a2a610 for that
/// slot plus the offset, and the handle is invalid when the slot is empty or
/// the sum is null -- `Obj::IsValid` (0x005ab1d0) is exactly that test and
/// nothing else, so a corpse still lying in the world is valid. A body whose
/// check fails formats *"... called for an uninitialized or invalid object"*
/// into 0x00686eb0, a bare `ret` in retail, and pushes a default: it never
/// traps. Each member's doc says which default.
///
/// `kTypeNamedObj` resolves through the name table rather than being rejected,
/// and that is not a widening: `gbr.exe` registers a **type conversion**
/// `NamedObj -> Obj` -- `0x00699800(from=0x22, to=0x14, fn=0x005530a0,
/// cost=10)`, pushed at 0x0055343e..0x00553447 -- whose function is the body
/// of `NamedObj::obj` itself. So the conversion answers what `.obj` answers:
/// **nothing for a named object that has died** (see `m_named_obj_obj`), and a
/// member reached through it -- `NO_X.pos`, `.player`, `.Erase` -- sees an
/// invalid receiver. The registrar's three other uses are the language's
/// primitive conversions (`bool -> int` cost 15, `int -> str` cost 50), so this
/// is the ordinary mechanism and not a special case. It is what lets
/// `Maps/10/Sequences/seq0.vs` write `NO_Hero1.SetCommand("stand_position")`
/// on a name its own `map.obj.xml` declares `<group type="0">`.
///
/// A member registered on `NamedObj` itself still wins, because an exact
/// receiver costs nothing and the conversion costs ten; `m_is_valid` and
/// `m_is_dead` below take their `NamedObj` branch first for that reason.
void feed_from_now(World& world, ObjectId id);

[[nodiscard]] const WorldObject* object_of(World& world, const Value& value) noexcept {
  if (!value.is_object()) return nullptr;
  const ObjectRef ref = value.as_object();
  if (ref.type == kTypeNamedObj) {
    const auto index = static_cast<std::int32_t>(ref.id);
    if (!world.named_objects().valid(index)) return nullptr;
    const WorldObject* bound = world.find(world.named_objects().object(index));
    if (bound == nullptr || object_is_dead(*bound)) return nullptr;
    return bound;
  }
  if (ref.type != kTypeObj && ref.type != kTypeQuery && ref.type != kTypeSettlement) return nullptr;
  return world.find(ref.id);
}

[[nodiscard]] Value object_value(ObjectId id) {
  // An id of zero is `kNoObject`, which must read as an invalid handle rather
  // than as object zero -- `.IsValid` is the most used member in the corpus and
  // 331 files declare a handle and assign it several statements later.
  if (id == kNoObject) return Value::object(ObjectRef{script::kNoType, 0});
  return Value::object(ObjectRef{kTypeObj, id});
}

[[nodiscard]] Value query_value(ObjectId id) {
  if (id == kNoObject) return Value::object(ObjectRef{script::kNoType, 0});
  return Value::object(ObjectRef{kTypeQuery, id});
}

[[nodiscard]] Value invalid_object() { return Value::object(ObjectRef{script::kNoType, 0}); }

/// `_PlaceEx`'s clamp inset, in world units. `0x30` at 0x005a8a7d..0x005a8a86,
/// added to both low edges and subtracted from both high ones. `Place` has no
/// counterpart: it refuses an outside point rather than moving it.
inline constexpr std::int32_t kPlaceExInset = 48;

/// A class filter from a script argument, resolved against the world's graph.
[[nodiscard]] ClassFilter filter_arg(World& world, const Value& value) {
  if (!value.is_string()) return ClassFilter{};  // match_all
  return ClassFilter::parse(value.as_string(), world.class_graph());
}

/// A script's player number as a `PlayerId`.
///
/// **1..16, not 0..15.** This file read the argument raw for a long time while
/// `m_player` 250 lines below returned `player_to_script(...)` and explained at
/// length why the *return* direction is 1-based. The argument direction was
/// simply missed, so `ClassPlayerObjs`, `ClassPlayerAreaObjs` and `Count` each
/// answered for the player one slot below the one the script named -- and for
/// `player 1`, the human, that is player 0's objects, which on most maps is a
/// populated slot rather than an empty one. A wrong answer that looks right.
///
/// The corpus settles it on its own, and the control is the interesting half:
/// across the 33 sites in all 885 scripts that pass a *literal* to one of the
/// three, the values used are 1, 2, 3, 4 and 5, and **`0` does not occur once**.
/// In a sixteen-player game read 0-based, 0 would be the commonest value. Four
/// more scripts pass their own 1-based `player` parameter straight through.
/// `gbr.exe` agrees in words: five error strings say "between 1 and 16".
[[nodiscard]] PlayerId player_arg(const Value& value) noexcept {
  if (!value.is_integer()) return kNoPlayer;
  return player_from_script(value.as_integer());
}

}  // namespace

namespace {
/// One cycle of the entity's `attack` state; defined beside `AttackWait`.
[[nodiscard]] std::int32_t attack_animation_ms(const WorldObject& slot);
}  // namespace

/// A class property as an integer, walking up the class tree.
Point enter_point_near(const World& world, ObjectId building, ObjectId unit, bool water) {
  return enter_point_near(world, building, world.resolve_position(unit), water);
}

Point enter_point_near(const World& world, ObjectId building, Point from, bool water) {
  const Point home = world.resolve_position(building);
  MovementSystem* movement = movement_system(const_cast<World&>(world));
  std::vector<Point> doors;
  building_doors(world, movement == nullptr ? nullptr : &movement->grid(), building,
                 kEnterPointType, water, doors);
  if (doors.empty()) return home;

  Point best = doors.front();
  std::int64_t nearest = -1;
  for (const Point& door : doors) {
    const std::int64_t dx = door.x - from.x;
    const std::int64_t dy = door.y - from.y;
    const std::int64_t d2 = dx * dx + dy * dy;
    // Strictly nearer, so ties go to the earlier door and the answer follows
    // the class table's order rather than the scan's.
    if (nearest < 0 || d2 < nearest) {
      nearest = d2;
      best = door;
    }
  }
  return best;
}

bool is_very_broken(const World& world, const WorldObject& slot) noexcept {
  return slot.state.health < class_int(world, slot, "maxhealth") / 20;
}

std::int32_t class_int(const World& world, const WorldObject& slot,
                       std::string_view key) noexcept {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass) return 0;
  const std::string_view text = graph->property(slot.class_index, key);
  std::int64_t value = 0;
  bool negative = false;
  std::size_t i = 0;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    negative = text[i] == '-';
    ++i;
  }
  if (i >= text.size()) return 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return 0;
    value = value * 10 + (text[i] - '0');
    if (value > 0x7FFFFFFFLL) return 0;
  }
  return static_cast<std::int32_t>(negative ? -value : value);
}

/// Whether any class from `index` up the chain declares `key` -- the
/// question `ClassGraph::property` folds into an empty answer.
[[nodiscard]] bool class_declares(const ClassGraph& graph, ClassIndex index,
                                  std::string_view key) noexcept {
  for (ClassIndex at = index; at != kNoClass; at = graph.at(at).parent_index) {
    for (const ClassProperty& property : graph.at(at).properties) {
      if (property.key == key) return true;
    }
    if (graph.at(at).parent_index == at) break;
  }
  return false;
}

/// The class record's "absent" marker for an int slot (`0xff1b1e40`).
constexpr std::int32_t kClassIntAbsent = -15000000;

/// One class int that keeps the absent marker when nothing declares it.
[[nodiscard]] std::int32_t class_int_or_marker(const World& world, const WorldObject& slot,
                                               std::string_view key) noexcept {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass ||
      !class_declares(*graph, slot.class_index, key)) {
    return kClassIntAbsent;
  }
  return class_int(world, slot, key);
}

/// A class property as a flag, walking up the class tree.
///
/// `gbr.exe` parses these into one `int` slot per key on the class descriptor
/// (the reader is at `0x005a4660`, one `strcmp`/store pair per property), and
/// every predicate below tests that slot for non-zero rather than for the
/// literal `"1"`. Absent, empty and `"0"` are false; anything else is true.
bool class_flag(const World& world, const WorldObject& slot, std::string_view key) noexcept {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass) return false;
  const std::string_view text = graph->property(slot.class_index, key);
  return !text.empty() && text != "0";
}

namespace {

/// `IsHeirOf` as a predicate rather than as an entry point, for the host
/// functions below that test ancestry against a name they carry themselves.
///
/// Same rule as `m_is_heir_of`: a name the graph cannot resolve reads as false,
/// never as "matches everything".
///
/// **The `match_all` test bites on a null graph and an empty name and on
/// nothing else**, because `ClassFilter::parse` already leaves `match_all` false
/// with count 0 for a name it could not resolve -- *"a filter that named classes
/// and resolved none of them selects nothing, and must not silently widen to
/// everything"*. Kept because the rule is this call site's to state and the
/// guarantee is `parse`'s to keep, and because writing it out is what stops the
/// next caller hand-rolling a copy. One did, and a fault survived it.
/// The map's inclusive high corner, in world units.
///
/// `GetMapRect`'s rule -- `(0, 0, w - 1, h - 1)` from the match rules, falling
/// back to the height layer's extent, which is the same square by construction.
/// Restated rather than shared with `sim/flying.cpp` and `sim/entrance.cpp` for
/// the reason those two say: a second definition that agreed is cheaper than an
/// export, and one that disagreed would be worse than either.
[[nodiscard]] std::int32_t map_high_corner(const World& world) noexcept {
  if (const MatchSystem* match = match_system_of(world); match != nullptr) {
    const std::int32_t size = match->rules().map_size;
    if (size > 0) return size - 1;
  }
  const std::uint32_t extent = world.height().extent_x();
  return extent > 0 ? static_cast<std::int32_t>(extent) - 1 : 0;
}

[[nodiscard]] bool class_is(const World& world, const WorldObject& slot,
                            std::string_view name) noexcept {
  const ClassFilter filter = ClassFilter::parse(name, world.class_graph());
  if (filter.match_all) return false;
  return world.matches_filter(slot, filter);
}

// --------------------------------------------------------------------------
// free functions
// --------------------------------------------------------------------------

/// `rand(n)` (0x004c63a0): pops the integer and answers the game generator's
/// Range(0, n - 1) (`[game+0x12a0]`, vtable slot `+0x14`, 0x0063ff00) -- one
/// call, nothing else. So `rand(n)` is uniform in `[0, n)` for `n >= 2`,
/// `rand(1)` is 0 **without advancing the stream**, and `rand(0)` or a
/// negative `n` is Range's swapped arm: it draws, and answers in `[n - 1, 0]`
/// (`rand(0)` is -1 or 0). `Rng::below` is exactly this; sim/rng.hpp has the
/// generator and the corpus evidence for the exclusive bound. A non-integer
/// cannot reach the body (the compiler types the argument), so the refusal
/// below is this engine's guard, not a behaviour.
HostOutcome fn_rand(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("rand: no world");
  const Value& bound = ctx.arg(0);
  if (!bound.is_integer()) return HostOutcome::failed("rand: expected an integer bound");
  return HostOutcome::ok_with(Value::integer(world->rng().below(bound.as_integer())));
}

/// The class argument of the query constructors, as 0x00409e40 takes it: the
/// **whole string as one class name** in the class map (the lookup
/// `IsHeirOf` makes), and a miss prints *"Could not find class named '%s'"*
/// and makes the entry point answer the invalid handle rather than a query.
/// `ObjsInSight`, `VisibleObjsInSight` and `ObjsInCircle` skip the check when
/// the string holds a comma (`strchr`), which is how `"Military,Tower"` gets
/// through to the list parser; `ClassPlayerObjs`, `ObjsInRange` and the
/// settlement forms do not, so a list there is an invalid handle. A world with
/// no class graph (every synthetic test) resolves everything, as
/// `ClassFilter::parse` does.
[[nodiscard]] bool class_arg_resolves(const World& world, const Value& value, bool list_allowed) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || !value.is_string()) return true;
  const std::string_view name = value.as_string();
  if (list_allowed && name.find(',') != std::string_view::npos) return true;
  return graph->lookup(name) != kNoClass;
}

/// The radius a sight query is made with when the script gives none, or a
/// negative one: 0x004fee10 stores **the observer's sight less 100**
/// (`[obj+0xd0] - 100`), once, at construction -- a later `SetSight` does not
/// widen a query already made.
[[nodiscard]] std::int32_t sight_query_radius(const WorldObject& observer, std::int32_t given) {
  return given < 0 ? observer.sight - 100 : given;
}

/// `ObjsInSight(observer, "class")` (0x00575cb0) and `VisibleObjsInSight`
/// (0x00575ef0).
///
/// The observer is checked first (a miss prints and answers the invalid
/// handle), then the class (`class_arg_resolves`, a comma list allowed), and
/// then 0x004fee10 makes -- or finds, see `fn_set_op` -- a sight query with
/// radius -1, which it turns into the observer's sight less 100. The query
/// (`CVXObjsInSightQuery`, refreshed every second) answers nothing while its
/// observer is gone or dead, and otherwise every living, spawned object of the
/// class within the radius of the observer's stored position, inclusive, the
/// observer itself excluded (`World::evaluate_query`). This read the live
/// sight, whole, and swept the whole map when it was 0, until the host audit.
///
/// The visibility restriction of the `Visible` form is recorded on the spec and
/// **not applied**: fog of war is one of the four hash channels the shipped
/// build left at zero in all nine dumps, and filtering on it here would pull it
/// back into hashed state.
template <bool kVisible>
HostOutcome fn_objs_in_sight(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ObjsInSight: no world");
  const WorldObject* observer = object_of(*world, ctx.arg(0));
  if (observer == nullptr) return HostOutcome::ok_with(invalid_object());
  if (!class_arg_resolves(*world, ctx.arg(1), true)) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = objs_in_sight(observer->id, sight_query_radius(*observer, -1),
                                       filter_arg(*world, ctx.arg(1)), kVisible);
  return HostOutcome::ok_with(query_value(world->intern_query(spec)));
}

/// `ObjsInCircle(point, radius, "class")` (0x005775e0).
///
/// The class (a comma list allowed), then 0x004ff860 makes or finds a
/// `CVXMapAreaQuery<TCircleArea>` keyed on `(x, y, r, class)` (0x004fac20
/// stores `r` and `r²`). Its refresh sweeps every owner's objects with
/// `dist² <= r²` (0x004fde60), the dead and the templates left out; a radius
/// of 0 holds only the exact spot and a negative one nothing
/// (`World::evaluate_query`).
HostOutcome fn_objs_in_circle(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ObjsInCircle: no world");
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("ObjsInCircle: expected a point");
  if (!ctx.arg(1).is_integer()) return HostOutcome::failed("ObjsInCircle: expected a radius");
  if (!class_arg_resolves(*world, ctx.arg(2), true)) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = objs_in_circle(unpack_point(ctx.arg(0)), ctx.arg(1).as_integer(),
                                        filter_arg(*world, ctx.arg(2)));
  return HostOutcome::ok_with(query_value(world->intern_query(spec)));
}

/// `ObjsInRange(object, "class", radius)` (0x00576130) -- **a sight query with
/// the radius given**, not a circle: the body (whose diagnostics still say
/// `ObjsInSight`) checks the object, then the class with **no** comma bypass,
/// and calls 0x004fee10 exactly as `ObjsInSight` does but with the script's
/// radius, a negative one again meaning sight less 100. So it follows the
/// object, never lists the object itself, and answers nothing once the object
/// is dead. It was a circle anchored on the object here, which listed the
/// anchor and swept the whole map for a radius of 0.
HostOutcome fn_objs_in_range(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ObjsInRange: no world");
  const WorldObject* anchor = object_of(*world, ctx.arg(0));
  if (anchor == nullptr) return HostOutcome::ok_with(invalid_object());
  if (!ctx.arg(2).is_integer()) return HostOutcome::failed("ObjsInRange: expected a radius");
  if (!class_arg_resolves(*world, ctx.arg(1), false)) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = objs_in_range(anchor->id, sight_query_radius(*anchor, ctx.arg(2).as_integer()),
                                       filter_arg(*world, ctx.arg(1)));
  return HostOutcome::ok_with(query_value(world->intern_query(spec)));
}

/// `ClassPlayerObjs("class", player)` (0x005754b0).
///
/// A player outside 1..16 prints and answers the invalid handle; so does a
/// class that does not resolve whole (`class_arg_resolves`, no comma bypass).
/// Then 0x004fe730 makes or finds a `CVXClassPlayerAreaQuery` with no area,
/// whose refresh (0x004fe860) sweeps the whole map **through the grid** for
/// objects carrying the player's owner bit: so the held -- a garrison, a
/// ship's passengers -- and the dead are never in it (`World::evaluate_query`).
///
/// A player number that does not resolve selects **nothing** rather than
/// minting a query with `kNoPlayer`, which in `objects_of_class_for_player`
/// means *any owner* -- every object on the map, a wrong set that composes,
/// hashes, and looks entirely plausible.
HostOutcome fn_class_player_objs(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClassPlayerObjs: no world");
  const PlayerId player = player_arg(ctx.arg(1));
  if (player == kNoPlayer) return HostOutcome::ok_with(invalid_object());
  if (!class_arg_resolves(*world, ctx.arg(0), false)) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = class_player_area(filter_arg(*world, ctx.arg(0)), player, Point{}, 0);
  return HostOutcome::ok_with(query_value(world->intern_query(spec)));
}

/// `UnitsInSettlement`, `UnitsAroundSettlement` and `UnitsGuardingSettlement`.
///
/// **Three names, one body, and each of the three is registered twice.**
/// `gbr.exe` 0x00577934-0x005779c2 registers six entry points in one run --
/// each name at arity 2 with a `(str, str)` signature and again with a
/// `(Settlement, str)` one -- and all six bodies converge on 0x004fe080 with a
/// mode literal that is the only thing that differs between them: 0 for
/// `UnitsInSettlement` (0x00574cd8 pushes `ebx`, which is zero), 1 for
/// `UnitsAroundSettlement` (0x00574ee8), 2 for `UnitsGuardingSettlement`
/// (0x00575108). This registry keys on `(kind, name, arity)`, so the two
/// signatures share one entry and the argument's runtime type discriminates,
/// exactly as it does for `SpawnGroupInHolder`.
///
/// **The string form is the one the corpus uses**, which is why it is here at
/// all: `DATA\AI HELPERS\GUARD.VS` declares `str Target` in its header and
/// calls `UnitsAroundSettlement(Target, "Unit")`, and every one of the
/// container sites -- `5_Great_Loses_German`'s `seq5` and `seq14` alone hold
/// dozens -- passes a literal settlement name. Only
/// `DATA\SUBAI\TOWNHALL_BEHAVIOR_GUARD.VS` passes a handle, as
/// `UnitsInSettlement(.settlement, "Unit")`. The object form was the only one
/// implemented here for a long time and the name lookup is what makes the
/// container half reachable.
///
/// A name that matches no settlement is a printed diagnostic and an invalid
/// handle, not a refusal: 0x00574c69 prints *"Could not find settlement named
/// '%s' in function 'UnitsInSettlement'. Check the spelling."* and pushes the
/// 0xffff sentinel. The handle form checks its receiver the same way. So does a
/// class name that does not resolve whole (0x00574e73, *"Could not find class
/// named '%s' ..."*; `class_arg_resolves`, no comma bypass) -- this used to
/// mint a query that matched nothing instead. Then 0x004fe080 makes or finds
/// the query, refreshed every two seconds.
template <SettlementScope kScope>
HostOutcome fn_units_in_settlement(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("UnitsInSettlement: no world");

  ObjectId settlement = kNoObject;
  if (ctx.arg(0).is_string()) {
    // The same scan `GetSettlement/1` and `WaitSettlementCapture` make, over
    // the same `settlement + 0xcc` string: byte-exact and case-sensitive.
    EconomySystem* economy = economy_of(*world);
    const Settlement* found =
        economy == nullptr ? nullptr : economy->settlements().find_by_name(ctx.arg(0).as_string());
    if (found == nullptr) return HostOutcome::ok_with(invalid_object());
    settlement = found->object;
  } else {
    const WorldObject* slot = object_of(*world, ctx.arg(0));
    if (slot == nullptr) return HostOutcome::ok_with(invalid_object());
    settlement = slot->id;
  }

  if (!class_arg_resolves(*world, ctx.arg(1), false)) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = units_in_settlement(settlement, filter_arg(*world, ctx.arg(1)), kScope);
  return HostOutcome::ok_with(query_value(world->intern_query(spec)));
}

/// `Group(n)` (0x005765a0).
///
/// Looks the name up in the group dictionary at `[0x009bdac8]` (0x004c93c0),
/// creating a `CVXGroup` (0x005720c0) and inserting it (0x0049c0a0) on a miss,
/// and answers **the group object's own handle** with a reference added -- the
/// same handle for the same name every time, whose list is the live deque
/// (0x005716f0, no cache). Here the name is interned the same way
/// (`World::group_index`) but each call mints a fresh query over it: the
/// members agree, the handles do not (the query-model gap at `fn_set_op`).
///
/// **Every one of the nine call sites in the shipped corpus passes a string**,
/// not an integer: `Group(GroupName)` in the four AI-helper scripts,
/// `Group("GoldMules" + idPlayer)` in `ES_OUTPOSTSELLGOLD.VS`, `Group("Player" +
/// .player + groupname)` in `ARENA_BEHAVIOR.VS`. `QuerySpec::group` is an index
/// into `map.obj.xml`'s `<group>` table and `World` does not carry that table
/// yet (`world.cpp` evaluates every group query as empty and says so), so a
/// name cannot be resolved to an index here and is refused by name rather than
/// answered with a number nothing evidences. The integer form is kept because
/// it is what the spec is built from and what the tests construct.
HostOutcome fn_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Group: no world");
  if (ctx.arg(0).is_string()) {
    // All 42 literal `Group("...")` sites in the corpus resolve against a
    // type-1 `<group>` in their own map, and none against a type-0 one -- those
    // are named objects, a separate engine table. `World::group_index` interns
    // by name in first-mention order, which is deterministic because script
    // execution order is. See `docs/formats/map.md`.
    return HostOutcome::ok_with(query_value(
        world->intern_query(group_query(world->group_index(ctx.arg(0).as_string())))));
  }
  if (!ctx.arg(0).is_integer()) return HostOutcome::failed("Group: expected an integer");
  return HostOutcome::ok_with(query_value(world->intern_query(group_query(ctx.arg(0).as_integer()))));
}

/// `GetInnState(inn)` / `SetInnState(inn, state)` -- 3 sites and 1, and the
/// first is the whole of what `UNIT_TRANSPORT_VERIFY.VS` was waiting for.
///
/// Both resolve the object (0x00533ae0, 0x00533c00) and, for an invalid
/// receiver or one that is not an heir of `Inn` (0x007b4a30, through the
/// class registry's descent test), print and answer **2** -- or print and
/// return. Otherwise they read or write one integer in the env registry under
/// `/Inns/<n>/state`: 0x00532780 formats `/Inns/%d` from the inn's index at
/// `[inn+0x208]`, minting a fresh one into the same registry when the slot is
/// `-1` (0x005327d0), and 0x00533050 / 0x005330e0 join `/state` (0x007b1428)
/// and go through the registry's own int get and set. **Both clamp to 1..3**:
/// the read answers 1 for anything stored below 1 -- an inn nobody has touched
/// is open -- and 3 for anything above, and the write stores the clamped
/// value. What the scripts make of the numbers: 1 is open
/// for transport (`UNIT_TRANSPORT_VERIFY.VS`, `INN_TRANSPORT_REQUEST.VS`), 3
/// is what `INN_BEHAVIOR.VS` waits on before it announces the inn and sets 1.
///
/// The index this engine keys on is the object id: nothing here authors
/// `[inn+0x208]`, and the path is spelt by nobody but these two -- no script
/// reads it back through `EnvReadInt` -- so the observable is only that each
/// inn's state is its own, which the id gives. The `(str, str)` forms
/// `gbr.exe` registers beside these (0x00533170, 0x00533640: a map name and an
/// inn name, looked up through the campaign's map list; an unknown map or inn
/// prints and the read answers 3) have no call site -- all four shipped sites
/// pass the inn object -- and stay unbound.
[[nodiscard]] std::string inn_state_key(ObjectId inn) {
  return "/Inns/" + std::to_string(inn) + "/state";
}

HostOutcome fn_get_inn_state(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetInnState: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || !class_is(*world, *slot, "Inn")) {
    return HostOutcome::ok_with(Value::integer(2));
  }
  const EnvSystem* env = env_of(*world);
  const std::int32_t stored =
      env == nullptr ? 0 : env->env().read_int(EnvScope::root(), inn_state_key(slot->id));
  return HostOutcome::ok_with(Value::integer(stored < 1 ? 1 : (stored > 3 ? 3 : stored)));
}

HostOutcome fn_set_inn_state(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetInnState: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || !class_is(*world, *slot, "Inn")) return HostOutcome::ok_void();
  EnvSystem* env = env_of(*world);
  if (env == nullptr) return HostOutcome::ok_void();
  std::int32_t state = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  if (state < 1) state = 1;
  if (state > 3) state = 3;
  env->env().write_int(EnvScope::root(), inn_state_key(slot->id), state);
  return HostOutcome::ok_void();
}

/// The receiver of a membership call, which the corpus writes three ways.
///
/// `AddToGroup`, `RemoveFromGroup` and `RemoveFromAllGroups` are each
/// registered three times in `gbr.exe` -- on `Obj` (0x005b78ff, 0x005b7917,
/// 0x005b7930), on `ObjList` (0x00563541, 0x00563559, 0x00563572) and on
/// `Query` (0x0057b05d, 0x0057b075, 0x0057b08b) -- and this registry keys on
/// `(kind, name, arity)`, so the three collapse to one entry per name and one
/// body has to take all three receivers. `wagon.AddToGroup("GoldMules0")`,
/// `ol.RemoveFromGroup("Q_Slingers")` and
/// `Group("Oasis_Guards1").AddToGroup("Oasis_Guards")` all ship.
///
/// The three bodies of each name differ in what they check, the same way for
/// all three names:
///
///   * the **`Obj`** form (0x005b6730, 0x005b4630, 0x005b69b0) checks the
///     handle and prints and returns on a miss -- before any group is looked
///     up, so `AddToGroup` on a gone object creates nothing;
///   * the **`ObjList`** form (0x00561970, 0x0055f720, 0x00563250) does no
///     receiver check and applies the call to every member that resolves **and
///     is not dead** (`vtbl + 0x50`, `object_is_dead`);
///   * the **`Query`** form (0x0057a550, 0x0057a970, 0x0057ab80) checks the
///     handle like the `Obj` form, then copies the query's list and applies the
///     call to every member that resolves, dead or not.
///
/// `found` says whether the receiver passed its check; the list is the members
/// the call applies to, in the receiver's order -- for a group query, the order
/// they joined it.
struct MembershipReceiver {
  bool found = false;
  std::vector<ObjectId> members;
};

/// Bit 27 of `[obj+0x2c]`: an object `CVXGroup::Add` files in the template
/// deque and `CVXGroup::Remove` never finds.
[[nodiscard]] bool is_spawn_template(const World& world, ObjectId id) {
  const WorldObject* slot = world.find(id);
  return slot != nullptr && slot->state.flags.unspawned;
}

[[nodiscard]] MembershipReceiver membership_receiver(CallContext& ctx, World& world) {
  MembershipReceiver out;
  if (ctx.count() == 0) return out;
  const Value& receiver = ctx.arg(0);
  if (is_objlist(receiver)) {
    out.found = true;
    for (const ObjectId id : receiver_objects(ctx, world, receiver)) {
      const WorldObject* slot = world.find(id);
      if (slot != nullptr && !object_is_dead(*slot)) out.members.push_back(id);
    }
    return out;
  }
  if (object_of(world, receiver) == nullptr) return out;
  out.found = true;
  out.members = receiver_objects(ctx, world, receiver);
  return out;
}

/// `AddToGroup(name)` -- 179 member sites plus the free spellings.
///
/// **Creates the group it cannot find** once the receiver has passed its
/// check (`membership_receiver`), which is the one asymmetry in this family:
/// `gbr.exe` carries `Could not find group named '%s'` for every form of
/// `RemoveFromGroup` and for the three spawn entry points, and for **no** form
/// of `AddToGroup`. `Conquests\mediterranean` map 4 needs exactly that -- it
/// builds `Oasis_Guards`, a name no map file declares, out of twelve
/// `AddToGroup` calls and then loops on `Group("Oasis_Guards").count`.
///
/// The add itself is `CVXGroup::Add` (vtbl+0x3c, 0x00572410): a linear find in
/// the group's deque, then an append if absent. **Which deque** is decided
/// first: while the map editor's latch `[0x00a8734c]` is down -- always, in a
/// game -- a spawn template (bit 27) goes to the template deque (`+0x54`)
/// and anything else to the live one (`+0x24`); in the editor everything goes
/// to the live one. Here the one list keeps both, the template bit tells them
/// apart, and every reader asks for the half it reads, which `GroupTable`
/// documents. docs/formats/map.md, "Bit 27".
HostOutcome fn_add_to_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AddToGroup: no world");
  if (!ctx.arg(1).is_string()) return HostOutcome::failed("AddToGroup: expected a group name");
  const MembershipReceiver receiver = membership_receiver(ctx, *world);
  if (!receiver.found) return HostOutcome::ok_void();
  const std::int32_t group = world->group_index(ctx.arg(1).as_string());
  for (const ObjectId id : receiver.members) world->groups().add(group, id);
  return HostOutcome::ok_void();
}

/// `RemoveFromGroup(name)` -- 137 member sites.
///
/// Refuses the group it cannot find, and refusing means a printed diagnostic
/// and nothing else: `find`, not `intern`, so a misspelling does not leave an
/// empty group behind for a later `Group(...)` to resolve. The removal is
/// `CVXGroup::Remove` (0x00572210): the first match in the live deque, erased
/// in place, order kept. **The live deque only**: a spawn template stays in the
/// group it was authored into, and `SpawnGroup` goes on cloning it.
HostOutcome fn_remove_from_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RemoveFromGroup: no world");
  if (!ctx.arg(1).is_string()) {
    return HostOutcome::failed("RemoveFromGroup: expected a group name");
  }
  const MembershipReceiver receiver = membership_receiver(ctx, *world);
  if (!receiver.found) return HostOutcome::ok_void();
  const std::int32_t group = world->groups().find(ctx.arg(1).as_string());
  if (group == GroupTable::kNoGroup) return HostOutcome::ok_void();
  for (const ObjectId id : receiver.members) {
    if (is_spawn_template(*world, id)) continue;  // not in the live deque
    world->groups().remove(group, id);
  }
  return HostOutcome::ok_void();
}

/// `RemoveFromAllGroups()` -- 22 sites. Collects every group in the dictionary
/// (0x004909f0) and applies `Remove` to each, under the receiver rules above.
/// `Group("GoldMules" + idPlayer).RemoveFromAllGroups()` in
/// `DATA\AI\ES_OUTPOSTSELLGOLD.VS` is the shipped shape, and it is why the
/// query receiver has to be expanded before anything is removed: the query
/// reads the group it is emptying.
HostOutcome fn_remove_from_all_groups(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RemoveFromAllGroups: no world");
  const MembershipReceiver receiver = membership_receiver(ctx, *world);
  for (const ObjectId id : receiver.members) {
    // `Remove` on each group, which reads the live deque: a template keeps
    // every group it is a template of.
    if (is_spawn_template(*world, id)) continue;
    world->groups().remove_from_all(id);
  }
  return HostOutcome::ok_void();
}

/// `obj.IsInGroup(name)` -- registered on `Obj` alone (0x005b78e7, returning
/// type 7, `bool`). 0x005b45fa dispatches through the group's `vtbl + 0x38`,
/// which is `CVXGroup::Contains(Obj*)` at 0x005718e0 and reads the **live**
/// deque -- so a template answers false for a group it is authored into, which
/// is the same answer `Group(name)` gives about it.
HostOutcome fn_is_in_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsInGroup: no world");
  if (!ctx.arg(1).is_string()) return HostOutcome::failed("IsInGroup: expected a group name");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const std::int32_t group = world->groups().find(ctx.arg(1).as_string());
  if (slot == nullptr || group == GroupTable::kNoGroup) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(
      Value::boolean(!slot->state.flags.unspawned && world->groups().contains(group, slot->id)));
}

/// `GetGroupSize(name)` -- free, one argument, returning `int`.
///
/// The live half's size, and **zero for a group that does not exist** rather
/// than a refusal: 0x00573a80 writes 0 on the miss path with no diagnostic at
/// all, where its neighbours print one. On a hit it reads the live `ObjList`
/// through `vtbl + 0x20` and takes that deque's size (0x00573a86).
HostOutcome fn_get_group_size(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetGroupSize: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("GetGroupSize: expected a group name");
  const std::int32_t group = world->groups().find(ctx.arg(0).as_string());
  if (group == GroupTable::kNoGroup) return HostOutcome::ok_with(Value::integer(0));
  std::vector<ObjectId> live;
  world->objects_in_group(group, ClassFilter{}, live);
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(live.size())));
}

/// Where a spawned copy goes. `holder` alone is a carrier's holder record --
/// the copy is put in it (`put_in_holder`) as the template is spawned;
/// `settlement` set means a settlement's garrison, entered after the spawn.
struct SpawnDestination {
  ObjectId holder = kNoObject;
  SettlementId settlement = kNoSettlement;
  ObjectId settlement_object = kNoObject;
};

/// The body behind both spawn entry points: `CVXGroup::Spawn` (0x00572760) and
/// its holder twin (0x005724a0).
///
/// `found` false means the name resolved to nothing -- an unknown group, an
/// unknown settlement, an invalid holder. All three print a diagnostic in the
/// original and hand back an empty list rather than refusing, so all three do
/// that here; `sim/world.hpp` records the addresses.
///
/// The spawn walks the group's template deque in order and clones each
/// template through its own `vtbl + 0x30`; the copy is cast to `CVXUnit`
/// (class id 0x00824d10, at 0x0057280a) and **a copy that is not a unit is
/// skipped** -- here, a template without the unit flag is not spawned. (All
/// 15,601 grouped templates in the shipped maps carry it, so this is never
/// reached on retail data.) No random draws.
///
/// The holder twin enters each copy through 0x005d3e10 with the force flag
/// instead of placing it: out of any old holder, onto the holder's roster past
/// its cap (0x00532160), timers, action and path dropped, the `(-1, -1)`
/// marker, and the class's `onenter` hook against the holder's composite. For a
/// settlement's holder that is `garrison_enter(..., force)` and the hook, as
/// `Settlement::AddUnit`'s path fires it; a ship's holder keeps no roster
/// here, so its copies are put in it and no hook runs (its composite is no
/// settlement). The original enters the holder before it files the copy under
/// the template's names and groups; here the names and groups come first,
/// which no hook a shipped class binds can tell apart.
HostOutcome spawn_group_into_list(CallContext& ctx, World& world, std::string_view name,
                                  const SpawnDestination& to, bool found = true) {
  ObjListPool& pool = objlist_pool_of(world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* items = pool.mutable_items(list);
  if (items == nullptr) return HostOutcome::failed("SpawnGroup: the pool refused a new list");
  if (!found) return HostOutcome::ok_with(make_objlist_value(list));

  // `find`, never `intern`: a spawn of a name no map declares must not mint the
  // group on the way past. `AddToGroup` is the creating form and this is not.
  const std::int32_t group = world.groups().find(name);
  if (group == GroupTable::kNoGroup) {
    return HostOutcome::ok_with(make_objlist_value(list));
  }

  // Snapshotted before the loop, because spawning adds the copies to this very
  // group -- the copy inherits the template's memberships -- and iterating a
  // list that grows underneath the loop is undefined however it is filtered.
  // The original is safe from that for free: its two deques are separate
  // objects, so appending live members cannot disturb the template deque being
  // walked.
  std::vector<ObjectId> templates;
  (void)world.templates_in_group(group, templates);
  items->reserve(templates.size());
  for (const ObjectId templ : templates) {
    const WorldObject* source = world.find(templ);
    if (source == nullptr || !source->state.flags.is_unit) continue;  // the CVXUnit cast
    const bool garrison = to.settlement != kNoSettlement;
    const ObjectId copy = world.spawn_from_template(templ, garrison ? kNoObject : to.holder);
    if (copy == kNoObject) continue;
    if (garrison && garrison_enter(world, to.settlement, copy, /*force=*/true)) {
      (void)fire_class_hook(world, ClassHook::on_enter, copy, to.settlement_object);
    }
    // Same reason as `Place`: a reinforcement that arrives mid-turn must be in
    // the feeding chain before the sequence that spawned it starts giving it
    // orders and food. See `feed_from_now`.
    feed_from_now(world, copy);
    items->push_back(copy);
    // The copy's AI register, last (0x005729f0, 0x00572720): the spawn holds
    // the AI world pointer [0x008c8e50] at zero while it clones, places,
    // enters and files, so nothing those steps run registers it, and puts it
    // back for this one call. Tally and squad, at once.
    if (HeroSystem* heroes = hero_system_of(world); heroes != nullptr) {
      heroes->ai_register(world, copy);
    }
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `SpawnGroup(name)` (0x00576790) -- 187 call sites, all in the campaign
/// containers.
///
/// Looks the name up in the group map at `[0x009bdac8]`, runs the group's spawn
/// (`spawn_group_into_list`), and hands back the copies as an `ObjList` --
/// the registration at 0x00577814 declares the return type as 23, `ObjList`,
/// and the body assigns the group's spawned list into a fresh one (0x00438230).
///
/// The miss is **not** a trap. `gbr.exe` prints `Could not find group named
/// '%s' in function 'SpawnGroup'. Check the spelling.` through 0x00686eb0 --
/// which in the retail build is a bare `ret`, so nothing prints -- and then
/// returns a freshly allocated *empty* list (0x00576887 allocates 0x2c bytes
/// on the miss path exactly as the hit path does). A script that spawns a name
/// no map declares gets an empty list and runs on, and `find` rather than
/// `intern` is what keeps this from minting the group as a side effect.
///
/// **Partial:** each copy is also handed to 0x0041f310, which files it on a
/// pending list behind global gates that no script reads (labelled at
/// `place_object`), and its behaviours start at the next turn's sweep.
HostOutcome fn_spawn_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SpawnGroup: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("SpawnGroup: expected a group name");
  return spawn_group_into_list(ctx, *world, ctx.arg(0).as_string(), SpawnDestination{});
}

/// `SpawnNamed(name)` -- 3 call sites, and the sole blocker of the tutorial.
///
/// The named-object twin of `SpawnGroup`. `<group type="0">` names exactly one
/// object -- 1,378 of 1,378 across the shipped maps -- and 10,610 of the
/// 27,070 placed objects are unspawned templates, so a name can be bound to a
/// template that is not yet in play. This brings that one object in and hands
/// back the *copy*, as an `Obj`: the registration at 0x00573c70 declares the
/// return type as 20, which is `Obj` and not `NamedObj`.
///
/// `gbr.exe` reaches it through `CVXNamedObjQuery::Spawn` (0x00571d40), which
/// is `CVXGroup::Spawn`'s body for one element: test the bound object's
/// 0x08000000 flag, clone it through the object's own `vtbl + 0x30`, clear the
/// template bit on the copy, then walk **both** global tables -- the group
/// table at 0x009bdad0 and the named-object table at 0x009bdac8 -- filing the
/// copy wherever the template was a member. `World::spawn_from_template` is
/// already that function, rebinding names included, because `SpawnGroup` needs
/// the same walk; `docs/formats/map.md` left "whether `SpawnNamed` rebinds too"
/// open and the answer is that it does not decide it -- the rebind is in the
/// shared spawn, not in either caller.
///
/// **Three ways to get an invalid handle back, and none of them refuses.**
/// A name the map does not declare prints `Could not find named object named
/// '%s' in function 'SpawnNamed'. Check the spelling.` (0x007c4b00) and pushes
/// the invalid handle; a name whose object has gone takes the same path
/// (0x00573e0a); and a name bound to an object that is *not* a template gets
/// no diagnostic at all, because `CVXNamedObjQuery::Spawn` returns null and the
/// caller pushes whatever the handle of null is. `2_Great_loses_Spain` seq14
/// calls `SetVisible` on the result through the *name* rather than through the
/// returned handle, so the retail scripts never read the third case.
HostOutcome fn_spawn_named(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SpawnNamed: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("SpawnNamed: expected a name");
  const ObjectId bound = world->named_objects().object(ctx.arg(0).as_string());
  const ObjectId copy = world->spawn_from_template(bound);
  if (copy == kNoObject) return HostOutcome::ok_with(invalid_object());
  // The same reason `spawn_group_into_list` feeds its copies: a reinforcement
  // that arrives mid-turn has to be in the feeding chain before the sequence
  // that spawned it starts giving it orders.
  feed_from_now(*world, copy);
  // The AI register last, as `SpawnGroup`'s copies have it (0x00571f39).
  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) {
    heroes->ai_register(*world, copy);
  }
  return HostOutcome::ok_with(Value::object(kTypeObj, copy));
}

/// `SpawnGroupInHolder(name, where)` -- 127 call sites.
///
/// **Registered twice at the same arity**, which is why one body answers both:
/// 0x0057782f takes `(str, str)` and 0x00577847 takes `(str, Obj)`, and this
/// engine's registry keys on `(kind, name, arity)`. The argument's runtime type
/// is the discriminator, exactly as it is for `GetGAIKA`.
///
///   * **`(str, str)`** (0x005769b0): the group first (a miss: *"Could not find
///     group named..."* and an empty list), then the first settlement in the
///     settlement vector whose name equals the argument, compared exactly
///     (0x00576bb8); a miss is *"Could not find settlement named..."* and an
///     empty list. Then the holder spawn over the settlement's holder
///     (`[settlement+0x5e]`), entering each copy into the garrison.
///   * **`(str, Obj)`** (0x00576e00, whose own diagnostic calls it
///     `SpawnGroupInShip`): the group first, then **the object's holder** --
///     its `vtbl + 0xe4`, the virtual the `UIHolder` strip asks: a ship's
///     holder record, the settlement holder of an object that belongs to a
///     settlement, nothing otherwise. Nothing is *"SpawnGroupInHolder invalid
///     holder"* and an empty list, **and nothing spawns**. An invalid handle
///     faults the original; an empty list here.
///
/// The `Obj` form put the copies in the *ship object itself* for a while, so
/// `GetUnitsOnBoard` (which reads the holder at ship + 1) never saw them --
/// `4_Great_Battles_Egypt` seq2 hands the boarded army to player 1 that way --
/// and it spawned into any object at all. A holder object passed directly
/// answers itself here; that arm is inferred, and no shipped site takes it.
HostOutcome fn_spawn_group_in_holder(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SpawnGroupInHolder: no world");
  if (!ctx.arg(0).is_string()) {
    return HostOutcome::failed("SpawnGroupInHolder: expected a group name");
  }
  const std::string_view group = ctx.arg(0).as_string();
  const auto nowhere = [&] {
    return spawn_group_into_list(ctx, *world, group, SpawnDestination{}, false);
  };
  if (world->groups().find(group) == GroupTable::kNoGroup) return nowhere();

  EconomySystem* economy = economy_of(*world);
  const auto into_settlement = [&](const Settlement& s) {
    SpawnDestination to;
    to.holder = s.holder.object;
    to.settlement = s.id;
    to.settlement_object = s.object;
    return spawn_group_into_list(ctx, *world, group, to);
  };

  if (ctx.arg(1).is_string()) {
    const Settlement* settlement =
        economy == nullptr ? nullptr : economy->settlements().find_by_name(ctx.arg(1).as_string());
    if (settlement == nullptr) return nowhere();
    return into_settlement(*settlement);
  }

  const WorldObject* target = object_of(*world, ctx.arg(1));
  if (target == nullptr) return nowhere();
  if (target->object != nullptr && target->object->is_a(NativeClass::ship)) {
    // A ship's holder record is the internal object minted just after it
    // (`World::spawn_ship`), as `holder_of_ship` in sim/boarding.cpp finds it.
    const WorldObject* holder = world->find(target->id + 1);
    if (holder == nullptr || holder->internal != InternalKind::holder) return nowhere();
    SpawnDestination to;
    to.holder = holder->id;
    return spawn_group_into_list(ctx, *world, group, to);
  }
  if (target->internal == InternalKind::holder) {
    SpawnDestination to;
    to.holder = target->id;
    return spawn_group_into_list(ctx, *world, group, to);
  }
  if (target->settlement != kNoObject && economy != nullptr) {
    if (const Settlement* s = economy->settlements().for_object(target->settlement); s != nullptr) {
      return into_settlement(*s);
    }
  }
  return nowhere();
}

/// Whether `Place`'s cast to `CVXScriptObj` succeeds for a class.
///
/// Both entry points look their class up, create the object, and then ask
/// whether the result casts to `CVXScriptObj` (the class-id global at
/// 0x00821350, whose string is at 0x007c82e0). The answer decides two
/// observable things and nothing else: on a hit the object gets `SetPlayer`
/// and the script gets its handle back; on a miss the object is placed all the
/// same, no owner is written, and the script gets the invalid handle --
/// `_PlaceEx` logging `MapObj created (%s)` (0x007c8f88) as it goes. That
/// message is not an error. It is the executable naming the excluded set.
///
/// **The obvious spelling of this test is wrong here.** `NativeClass`'s parent
/// table is the shipped data's `cpp_class` hierarchy from docs/data-model.md,
/// in which `script_obj` and `unit` are *siblings* under `decor`; `gbr.exe`'s
/// own C++ inheritance is not that tree, and
/// `native_class_is_a(native, script_obj)` would refuse every unit the corpus
/// places.
///
/// The corpus settles which way round it is, and the control is the half that
/// makes it a measurement. Of the 173 call sites naming a class literally,
/// resolved through the class graph to a `cpp_class`:
///
/// | `cpp_class`     | handle used | handle discarded |
/// |-----------------|-------------|------------------|
/// | `CVXUnit`       |          32 |                0 |
/// | `CVXGhost`      |           7 |                0 |
/// | `CVXHero`       |           1 |                0 |
/// | `CVXFlyingUnit` |           1 |                0 |
/// | `CVXWagon`      |           1 |                0 |
/// | `CVXAreaEffect` |           1 |                0 |
/// | `CVXScriptObj`  |           3 |               94 |
/// | `CVXDecor`      |           0 |               19 |
///
/// **Nought of nineteen** bare-`CVXDecor` sites keeps what `Place` hands back,
/// against 46 of 46 elsewhere that do -- and a script that assigns the result
/// of a call it knows returns nothing is not a thing anybody writes. The 19 are
/// `ChimneySmoke`, `Mist`, the blacksmith and temple fires, the barrack horses:
/// animated scenery, which is what `CVXDecor` is the base of.
///
/// So the excluded set is read as **bare `CVXDecor` and `CVXMapObj`**: the
/// executable's own diagnostic names the second, no class-id global for
/// `CVXDecor` exists anywhere in the image -- nothing ever casts *to* it, which
/// is what a root looks like -- and `CVXMapObj` is the one the class table
/// already describes as drawn but not simulated.
///
/// **Labelled inference, and here is the alternative.** No shipped call site
/// names a `CVXBuilding`, `CVXGate`, `CVXItemHolder`, `CVXFeedback` or
/// `CVXCatapultShot` class, so the corpus cannot say which side those fall.
/// This reads them as script objects, because every one of them declares an
/// `idle` method in the class graph and being scriptable is what the name says.
/// The competing reading -- that the subtree is only `CVXScriptObj` and
/// `CVXUnit` and their descendants -- explains the same table, and the way to
/// separate them is `CVXUnit`'s `Cast` chain in the image, which has not been
/// walked. Erring towards the handle is deliberate: a wrong sentinel kills a
/// script that assigns it, while a wrong handle places an object the original
/// places too.
[[nodiscard]] bool places_as_script_object(NativeClass native) noexcept {
  return native != NativeClass::decor && native != NativeClass::map_obj;
}

/// The half `Place` and `_PlaceEx` do the same way: create, own, position.
///
/// `player` is the script's own 1-based number and is decremented here, which
/// is the one place the two entry points agree about it -- `Place` range-checks
/// it first and `_PlaceEx` does not, but both reach `SetPlayer(player - 1)`
/// through `vtbl + 0xa0` (0x005a88ac, 0x005a8b0c).
///
/// Order is transcribed rather than chosen: owner, then position, then the
/// handle read out of `word [obj+8]`. On the non-script-object branch the owner
/// step does not happen at all and the position step still does, which is why
/// this takes the branch as an argument rather than leaving it to the caller.
/// A unit that has just come into being feeds from now, not from the next turn.
///
/// `FeederSystem` reconciles against the world at the head of every turn, and a
/// script that places a unit and feeds it two statements later never reaches a
/// turn boundary in between. `OUTPOST_BEHAVIOR.VS` is exactly that --
/// `u1 = Place(sDefenderCls1, .pos, .player); ... u1.SetFood(20);
/// u1.SetFeeding(false);` -- and its eighteen per-map copies are the same lines.
/// Without this the two setters are handed an id the chain has never heard of,
/// and both refuse by name.
///
/// The original has no chain to be late for: a `CVXUnit` *is* a feeding unit
/// from construction. This is the reconcile model paying for itself at the one
/// place a script can outrun it.
void feed_from_now(World& world, ObjectId id) {
  if (FeederSystem* feeder = feeder_of(world); feeder != nullptr) {
    (void)feeder->enrol_if_eligible(world, id);
  }
}

HostOutcome place_object(World& world, ClassIndex class_index, NativeClass native, Point at,
                         std::int32_t player) {
  const ObjectId id = world.spawn_of_class(class_index);
  if (id == kNoObject) return HostOutcome::ok_with(invalid_object());

  const bool scriptable = places_as_script_object(native);
  if (scriptable) world.set_owner(id, static_cast<PlayerId>(player - 1));
  world.set_position(id, at);
  feed_from_now(world, id);
  if (!scriptable) return HostOutcome::ok_with(invalid_object());

  // Then the AI hand-off (0x0041f310): the unit's own `SetPos` (0x005d39e0)
  // runs the move hook (0x0041f530), which registers a unit whose squad
  // handle is empty, and `Place` calls the register again for a `CVXUnit`
  // whose position reads back with `x >= 0`, which finds it registered. Both
  // skip anything carrying the spawn-template bit, corroborating bit 27 from
  // a call site that has nothing to do with groups.
  //
  // **The tally at once, the squad at the next turn**
  // (`HeroSystem::ai_register_placed`): the owner's class tally counts the
  // unit before `Place` returns, so `BARRACK_TRAIN.VS`'s
  // `Count(.player, carCmdParam)` on the next lines counts the recruit it has
  // just placed. The original also files the unit in a squad here; this
  // engine files it at the next turn's sweep, and not at all if it has gone
  // into a holder by then -- a labelled difference, measured in
  // docs/formats/class-tally.md ("What differs"): filed at once, recruits
  // walking into their hall are caught by the orders of the squad they
  // joined, and Crossroads production falls by a third.
  if (const WorldObject* placed = world.find(id); placed != nullptr && placed->state.flags.is_unit) {
    if (HeroSystem* heroes = hero_system_of(world); heroes != nullptr) {
      heroes->ai_register_placed(world, id);
    }
  }
  return HostOutcome::ok_with(object_value(id));
}

/// `Place(str cls, Pos, int player)` -- 131 sites, 71 of them in `data.pak`.
///
/// 0x005a8710. Four refusals, in the order it applies them, and every one of
/// them hands back the invalid handle rather than trapping -- each prints and
/// runs on, so refusing by name here would kill scripts the original finishes:
///
///   1. the player number, before anything else: `Function "Place": Player
///      number should be between 1 and 16` (0x007c8ed0), tested as `< 1` or
///      `> 16` at 0x005a8763;
///   2. the position, against the map rectangle at `[0x9a721c] + 0xb4 .. +0xc0`
///      -- `Function Place used with invalid position` (0x007c8f5c);
///   3. the class name: `Could not find class named '%s' in function 'Place'.
///      Check the spelling.` (0x007c8f10);
///   4. and the `CVXScriptObj` cast, which is not a refusal so much as a
///      different object; see `places_as_script_object`.
///
/// **`(-1, -1)` skips the bounds test** (0x005a87ab), and that escape hatch is
/// what settles the rectangle's minimum without parsing anything new: if the
/// low edges were negative the branch would be dead code. So the rectangle is
/// `[0, MapSize()]` on both axes -- `MapSize()` (0x0051b120) returns the same
/// `+0xbc` this compares against, and `sim/match.hpp` already holds it as
/// `map_size - 1`.
HostOutcome fn_place(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Place: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("Place: expected a class name");
  if (!is_point(ctx.arg(1))) return HostOutcome::failed("Place: expected a position");
  if (!ctx.arg(2).is_integer()) return HostOutcome::failed("Place: expected a player number");

  const std::int32_t player = ctx.arg(2).as_integer();
  if (player < 1 || player > static_cast<std::int32_t>(kPlayerCount)) {
    return HostOutcome::ok_with(invalid_object());
  }

  const Point at = unpack_point(ctx.arg(1));
  MatchSystem* match = match_system_of(*world);
  const std::int32_t high =
      match == nullptr ? 0 : (match->rules().map_size > 0 ? match->rules().map_size - 1 : 0);
  const bool held = at.x == -1 && at.y == -1;
  const bool inside = at.x >= 0 && at.x <= high && at.y >= 0 && at.y <= high;
  if (!held && !inside) return HostOutcome::ok_with(invalid_object());

  const ClassGraph* graph = world->class_graph();
  if (graph == nullptr) return HostOutcome::ok_with(invalid_object());
  const ClassIndex class_index = graph->lookup(ctx.arg(0).as_string());
  if (class_index == kNoClass) return HostOutcome::ok_with(invalid_object());
  const Result<NativeClass> native = native_class_from_name(graph->at(class_index).cpp_class);
  if (!native) return HostOutcome::ok_with(invalid_object());

  return place_object(*world, class_index, *native, at, player);
}

/// `PlaceCatapult(x, y, player, race)` -- 1 site, and it was the sole blocker
/// of `BUILD_CATAPULT.VS` (16 sites), the `groupdispatch=` script of the
/// `build_catapult` command: the closest `CatapultMaxUnits` of the selection
/// vote on a race by majority, the engine is placed at the click, and every
/// builder gets `SetCommand("build_catapult", cat)`.
///
/// `0x004e3dc0` is registered `[38, 4, 1, 1, 1, 1]`: a `Catapult`, from four
/// ints. It is two calls. 0x004e3b30 turns the race into a class through an
/// eight-way table -- `GCatapult`, `RCatapult`, `CCatapult`, `ICatapult`,
/// `RCatapult` again for Imperial Rome, `BCatapult`, `ECatapult`, `TCatapult`,
/// in `Race` order -- and answers null for anything else, which the second
/// call then dereferences: an unknown race is a fault there and the invalid
/// handle here. 0x004e3be0 creates an object of that class (0x0059dcd0),
/// hands it `player - 1` through `vtbl + 0xa0` -- `Place`'s own step -- reads
/// `GamePlay/CatapultBuildSight` from `CONST.INI` into the sight slot at
/// `[obj+0xd0]` for the duration of the `SetPos` at `vtbl + 0x38`, so the
/// site is revealed to that radius rather than to the class's, then puts the
/// class sight (`[class+0x2d0]`) back, and finally `SetHealth(1)` through
/// `vtbl + 0x98`: an engine under construction, at one point of health, for
/// the builders to raise. **No bounds test** on the point, unlike `Place`; and
/// the `CVXCatapult` constructor leaves it unbuilt, which is what
/// `ObjectFlags::built` defaults to. Between the two it builds the engine's
/// own settlement (0x00541980; see `place_catapult`), and last it writes `1`
/// to the env registry under `/<map>/Player<n>/CatapultBuilt`, `n` the
/// script's player number -- the flag `BUILDINGSADVICE5.VS` polls with
/// `EnvReadInt(playerid, "CatapultBuilt")`. The handle is read from `word
/// [obj+8]` and 0xffff answers a creation that failed.
///
/// The reveal is the one step reproduced by a different mechanism: this
/// engine's `SetPos` does not explore, its fog is stamped by `FogSystem`, so
/// the build sight goes straight to `explore_circle` on the player's slot,
/// and a world with no fog or no `CONST.INI` value simply does not reveal --
/// where the original's sight of 0 would not either.
HostOutcome fn_place_catapult(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("PlaceCatapult: no world");
  if (ctx.count() < 4 || !ctx.arg(0).is_integer() || !ctx.arg(1).is_integer() ||
      !ctx.arg(2).is_integer() || !ctx.arg(3).is_integer()) {
    return HostOutcome::failed("PlaceCatapult: expected x, y, player, race");
  }
  const std::string_view name = catapult_class_for_race(ctx.arg(3).as_integer());
  const ClassGraph* graph = world->class_graph();
  if (name.empty() || graph == nullptr) return HostOutcome::ok_with(invalid_object());
  const ClassIndex class_index = graph->lookup(name);
  if (class_index == kNoClass) return HostOutcome::ok_with(invalid_object());

  const Point at{ctx.arg(0).as_integer(), ctx.arg(1).as_integer()};
  const ObjectId id = place_catapult(*world, class_index, at, ctx.arg(2).as_integer());
  return HostOutcome::ok_with(id == kNoObject ? invalid_object() : object_value(id));
}

/// `IsProtected(player, pos, class)` -- 2 sites, both the enchantress's cover
/// of mercy: `ENCHANTRESS_COVEROFMERCY.VS` opens with `if (.AI)
/// if (IsProtected(.player, .pos, "CoverOfMercy")) return;` and
/// `ENCHANTRESS_IDLE.VS` casts only where `!IsProtected(.player, o.posRH,
/// "CoverOfMercy")`. It was the sole blocker of the first file's 15 sites.
///
/// `gbr.exe` registers it textually as `bool, int player, point pos, str
/// class` (0x00435250), and `sim/area.hpp` was right to refuse it as an area
/// entry point: it is a **presence test for a protective effect over a
/// point**. The body: the player is 1-based and one outside 1..16 answers
/// **true** (0x004353dc writes 1), as does a class name nothing declares --
/// `IsProtected called with invalid class name` printed first. So does every
/// other early exit; the false answers come only from the query. The point is
/// clamped into the map rectangle (`[0x9a721c]+0xb4..0xc0`, low edge then
/// high per axis, `_PlaceEx`'s rule). Then a circle query (0x00423ef0, the
/// same cell sweep `ObjsInCircle` rides on) over the clamped point with radius
/// **twice the class's `radius`** attribute (`[class+0x2dc]`, squared as
/// `(2r)^2` for the sweep), for objects **of exactly that class** -- the sweep
/// compares `[obj+0x3c]` with the descriptor for equality (0x0042403d,
/// 0x0042408b), so an heir does not count -- with **no life test**, whose
/// owner bit in `[obj+0x2c]` is in the player's **friendly mask**,
/// `[record+0x10] ^ 0xffff`: the complement of the enemy word
/// `PlayerTable::is_enemy` reads, as the caster searches in `sim/hero.cpp`
/// already established. Any hit is true. A held object is not on the grid the
/// sweep walks, so it is not found.
///
/// A cover of mercy is placed by the script itself (`Place("CoverOfMercy",
/// pt, .player)`), so unlike a sacrifice it can exist here, and the sweep is
/// the engine's own: every object of the class, the point inside its
/// doubled radius by the query circle rule (`contains_by_query_rule`, which is
/// what the cell sweep answers), and an owner the player is not at war with.
/// The doubling is the original's and is why a single effect covers a ring
/// the enchantress does not have to stand inside.
HostOutcome fn_is_protected(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsProtected: no world");
  if (ctx.count() < 3 || !ctx.arg(0).is_integer() || !is_point(ctx.arg(1)) ||
      !ctx.arg(2).is_string()) {
    return HostOutcome::failed("IsProtected: expected a player, a point and a class name");
  }
  const std::int32_t player = ctx.arg(0).as_integer();
  if (player < 1 || player > static_cast<std::int32_t>(kPlayerCount)) {
    return HostOutcome::ok_with(Value::boolean(true));
  }
  const ClassGraph* graph = world->class_graph();
  const ClassIndex wanted = graph == nullptr ? kNoClass : graph->lookup(ctx.arg(2).as_string());
  if (wanted == kNoClass) return HostOutcome::ok_with(Value::boolean(true));

  Point at = unpack_point(ctx.arg(1));
  MatchSystem* match = match_system_of(*world);
  const std::int32_t high =
      match == nullptr ? 0 : (match->rules().map_size > 0 ? match->rules().map_size - 1 : 0);
  if (high > 0) {
    const auto clamp = [high](std::int32_t v) {
      if (v < 0) v = 0;
      if (v > high) v = high;
      return v;
    };
    at = Point{clamp(at.x), clamp(at.y)};
  }
  std::int32_t radius = 0;
  for (const char c : graph->property(wanted, "radius")) {
    if (c < '0' || c > '9') break;
    radius = radius * 10 + (c - '0');
  }
  const std::int32_t reach = 2 * radius;
  const AreaShape ring = AreaShape::of_circle(at, reach);
  const auto viewer = static_cast<PlayerId>(player - 1);
  for (const WorldObject& other : world->objects()) {
    if (other.internal != InternalKind::none || other.class_index != wanted) continue;
    if (!PlayerTable::is_valid(other.state.owner)) continue;
    if (world->players().is_enemy(viewer, other.state.owner)) continue;
    if (other.state.position == kHeldPosition) continue;
    if (!ring.contains_by_query_rule(other.state.position)) continue;
    return HostOutcome::ok_with(Value::boolean(true));
  }
  return HostOutcome::ok_with(Value::boolean(false));
}

/// `Catapult::CalcEscapeDirection()` -- 1 site, and it was the sole blocker
/// of `CATAPULT_DISBAND.VS` (13 sites): the crew of a disbanding engine is
/// sent `ptBase = .CalcEscapeDirection(); ptBase.SetLen(.radius * 4);` and
/// then scattered along it by `pt.Rot(30 - rand(60))`, one member at a time.
///
/// `0x004e3380` is registered `[6, 1, 38]`: a point, from a `Catapult`. An
/// invalid receiver prints `The function 'Catapult::CalcEscapeDirection'
/// called for an uninitialized or invalid object.` (see below for its answer).
/// Otherwise it runs a cell sweep (0x004e29c0) over the catapult's own sight
/// (`[obj+0xd0]`, the number `.sight` reports) whose predicate (0x004e2620)
/// keeps the **nearest** object that is a building (`[obj+0x2c] & 0x800000`,
/// this engine's `is_building`), an heir of `Tower` or of `Outpost`, and whose
/// owner's enemy word names the catapult's owner -- `[other.player+0x10] &
/// [this.player+0xc]`, which is `is_enemy` read from the *other's* row.
/// Nearest by squared distance, and an equal distance does not replace an
/// earlier find (`jle` at 0x004e269d), so a tie goes to the first the sweep
/// met. The answer is the vector **from that building to the catapult**
/// scaled to length 1000 -- `dx * 1000 / len`, `dy * 1000 / len` with the
/// table square root and truncating division -- so `SetLen` downstream
/// only rescales a direction that already points away from the threat.
///
/// With no such building in sight -- or one standing exactly on the
/// catapult, whose zero-length vector has no direction -- it is a random
/// direction (0x004e26c0): `rand(0, 1000) - 500` on x, then the same on y,
/// normalised to 1000 the same way; a draw that lands on the origin exactly
/// answers `(1000, 0)`, the worker's own fallback. Two draws on the world RNG,
/// in that order, every time.
///
/// An invalid receiver's answer is the original's own fault: it pushes four
/// bytes of zero where a point is eight, and `(0, 0)` stands in.
///
/// The sweep (0x004e2760) walks the world's grid -- 256-unit cells row by
/// row, and the objects within a cell in the order they came into it -- and
/// so does this one (`SweepOrder::grid`, `docs/formats/spatial-index.md`), so
/// of two enemy towers at exactly the same squared distance the earlier-swept
/// one wins, as there.
HostOutcome m_calc_escape_direction(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CalcEscapeDirection: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));
  const Point here = world->resolve_position(self->id);

  const ClassGraph* graph = world->class_graph();
  const ClassIndex tower = graph == nullptr ? kNoClass : graph->lookup("Tower");
  const ClassIndex outpost = graph == nullptr ? kNoClass : graph->lookup("Outpost");

  std::vector<ObjectId> around;
  (void)world->objects_in_radius(here, self->sight, ClassFilter{}, around, SweepOrder::grid);
  const WorldObject* threat = nullptr;
  std::int64_t best = 0;
  for (const ObjectId id : around) {
    const WorldObject* other = world->find(id);
    // The building bit is the original's first test; an heir of `Tower` or
    // `Outpost` is a building by class, so a fault that drops it survives the
    // suite as an equivalence on any data the class tree can produce.
    if (other == nullptr || other->id == self->id || !other->state.flags.is_building) continue;
    const bool tower_like = (tower != kNoClass && world->class_is_a(id, tower)) ||
                            (outpost != kNoClass && world->class_is_a(id, outpost));
    if (!tower_like) continue;
    if (!PlayerTable::is_valid(other->state.owner) || !PlayerTable::is_valid(self->state.owner) ||
        !world->players().is_enemy(other->state.owner, self->state.owner)) {
      continue;
    }
    const Point at = world->resolve_position(id);
    const std::int64_t dx = static_cast<std::int64_t>(here.x) - at.x;
    const std::int64_t dy = static_cast<std::int64_t>(here.y) - at.y;
    const std::int64_t d2 = dx * dx + dy * dy;
    if (threat != nullptr && best <= d2) continue;
    threat = other;
    best = d2;
  }

  // `isqrt` is the floored root the table at 0x0067c3d0 answers; see `Dist`.
  const auto length = [](std::int32_t x, std::int32_t y) {
    return static_cast<std::int32_t>(
        isqrt(static_cast<std::int64_t>(x) * x + static_cast<std::int64_t>(y) * y));
  };
  if (threat != nullptr) {
    const Point at = world->resolve_position(threat->id);
    const std::int32_t dx = here.x - at.x;
    const std::int32_t dy = here.y - at.y;
    const std::int32_t len = length(dx, dy);
    if (len > 0) return HostOutcome::ok_with(pack_point(Point{dx * 1000 / len, dy * 1000 / len}));
    // A threat on the catapult's own point: on to the random direction.
  }
  const std::int32_t dx = world->rng().between(0, 1000) - 500;
  const std::int32_t dy = world->rng().between(0, 1000) - 500;
  const std::int32_t len = length(dx, dy);
  if (len <= 0) return HostOutcome::ok_with(pack_point(Point{1000, 0}));
  return HostOutcome::ok_with(pack_point(Point{dx * 1000 / len, dy * 1000 / len}));
}

/// `Unit::RamBestTarget()` -- 1 site, and it was the sole blocker of
/// `RAM_IDLE.VS` (9 sites): `target = .RamBestTarget(); if (!target.IsValid())
/// while (1) Sleep(100000);` and, past a broken one, `.SetCommand("attack",
/// target)`. A ram with nothing to hit sleeps forever, by the script's own
/// choice.
///
/// `0x005d91c0` is registered `[20, 1, 21]`: an `Obj`, from a `Unit`. An
/// invalid receiver prints `The function 'Unit::RamBestTarget' called for an
/// uninitialized or invalid object.` and answers the invalid handle. The
/// live half is one cell sweep (0x005d56b0) over a circle of the ram's sight
/// **plus 512** (`[obj+0xd0] + 0x200`), buildings only (`0x800000`), with the
/// best score seeded at **1,000,000** -- a squared distance, so whatever the
/// sight says the ram never picks a target 1000 or more units away. The
/// predicate (0x005d4980) keeps the nearest candidate that:
///
///   * has an owner whose relation row names the ram's owner an enemy --
///     `[other.player + ram.index * 4 + 0x24] & 1` clear, which is
///     `is_enemy` read from the *other's* row;
///   * is not already broken (`[obj+0x204] != 3`, `ObjectState::damage_state`
///     at the tier `Building::IsBroken` answers);
///   * is an heir of `Gate`, `Outpost` or `BaseShipyard` -- the three things
///     a battering ram is for;
///   * and is strictly nearer than the best so far (`jge` skips an equal
///     distance), so a tie keeps the first the sweep met.
///
/// The sweep walks the world's grid in its order, as `CalcEscapeDirection`'s
/// does, so a tie keeps the first the grid walk met.
HostOutcome m_ram_best_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RamBestTarget: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(invalid_object());
  const Point here = world->resolve_position(self->id);
  const ClassGraph* graph = world->class_graph();
  if (graph == nullptr) return HostOutcome::ok_with(invalid_object());
  ClassIndex wanted[3];
  std::size_t count = 0;
  for (const std::string_view name : {"Gate", "Outpost", "BaseShipyard"}) {
    const ClassIndex index = graph->lookup(name);
    if (index != kNoClass) wanted[count++] = index;
  }

  std::vector<ObjectId> around;
  (void)world->objects_in_radius(here, self->sight + 512, ClassFilter{}, around,
                                 SweepOrder::grid);
  ObjectId best = kNoObject;
  std::int64_t best_d2 = 1000000;
  for (const ObjectId id : around) {
    const WorldObject* other = world->find(id);
    if (other == nullptr || other->id == self->id || !other->state.flags.is_building) continue;
    if (!PlayerTable::is_valid(other->state.owner) || !PlayerTable::is_valid(self->state.owner) ||
        !world->players().is_enemy(other->state.owner, self->state.owner)) {
      continue;
    }
    if (other->state.damage_state == 3) continue;
    bool fit = false;
    for (std::size_t i = 0; i < count && !fit; ++i) fit = world->class_is_a(id, wanted[i]);
    if (!fit) continue;
    const Point at = world->resolve_position(id);
    const std::int64_t dx = static_cast<std::int64_t>(at.x) - here.x;
    const std::int64_t dy = static_cast<std::int64_t>(at.y) - here.y;
    const std::int64_t d2 = dx * dx + dy * dy;
    if (d2 >= best_d2) continue;
    best = id;
    best_d2 = d2;
  }
  return HostOutcome::ok_with(best == kNoObject ? invalid_object() : object_value(best));
}

/// `_PlaceEx(str cls, int x, int y, int player)` -- 89 sites, all in containers.
///
/// 0x005a8980, and it is `Place` with three deliberate differences rather than
/// a second implementation of it.
///
///   * **It does not range-check the player.** There is no `cmp 1` / `cmp 16`
///     anywhere in it; the decrement at 0x005a8b08 is applied to whatever
///     arrived.
///   * **It clamps instead of refusing.** The map rectangle inset by `0x30` --
///     48 world units -- on all four sides, low edge applied first and high
///     edge second per axis (0x005a8a7d..0x005a8aaf), which is `IntoRect`'s
///     order and therefore has `IntoRect`'s behaviour on a reversed rectangle.
///     A map smaller than 96 units is the only way to reverse it, and none
///     ships.
///   * **Its miss goes somewhere else.** `PlaceEx cannot create object '%s'!`
///     (0x007c8f9c) is written to the game log rather than to the script
///     channel, and the class lookup happens *before* the clamp here where the
///     bounds test happens *before* the lookup in `Place`. Transcribed for the
///     order rather than for any effect it has.
HostOutcome fn_place_ex(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("_PlaceEx: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("_PlaceEx: expected a class name");
  if (!ctx.arg(1).is_integer() || !ctx.arg(2).is_integer()) {
    return HostOutcome::failed("_PlaceEx: expected two coordinates");
  }
  if (!ctx.arg(3).is_integer()) return HostOutcome::failed("_PlaceEx: expected a player number");

  const ClassGraph* graph = world->class_graph();
  if (graph == nullptr) return HostOutcome::ok_with(invalid_object());
  const ClassIndex class_index = graph->lookup(ctx.arg(0).as_string());
  if (class_index == kNoClass) return HostOutcome::ok_with(invalid_object());
  const Result<NativeClass> native = native_class_from_name(graph->at(class_index).cpp_class);
  if (!native) return HostOutcome::ok_with(invalid_object());

  MatchSystem* match = match_system_of(*world);
  const std::int32_t extent = match == nullptr ? 0 : match->rules().map_size;
  const std::int32_t low = 0 + kPlaceExInset;
  const std::int32_t high = (extent > 0 ? extent - 1 : 0) - kPlaceExInset;
  Point at{ctx.arg(1).as_integer(), ctx.arg(2).as_integer()};
  if (at.x < low) at.x = low;
  if (at.x > high) at.x = high;
  if (at.y < low) at.y = low;
  if (at.y > high) at.y = high;

  return place_object(*world, class_index, *native, at, ctx.arg(3).as_integer());
}

/// `SetNoAIFlag(ObjList, bool)` -- 121 sites -- and `Unit::SetNoAIFlag(bool)`
/// -- 109.
///
/// The flag says "this unit belongs to a script; the AI is to leave it alone",
/// and both bodies write it the same way: `[unit+0x194]` cleared of
/// `0x00040000` and then or-ed back in when the argument is true. That is bit
/// 18 of a **second** flag word, not of the `+0x2c` `SyncFlags` every other
/// object flag here comes from -- see `ObjectFlags::no_ai`.
///
/// **The two forms differ in what they write the bit on.** The free form
/// (0x00435470) walks the list and writes it only on a member that resolves,
/// carries the unit bit (`[obj+0x2c] & 0x00400000`) and is not dead
/// (`vtbl + 0x50`, `object_is_dead`). The member form (0x005de600) checks the
/// handle -- *"The function 'Unit::SetNoAIFlag' called for an uninitialized or
/// invalid object"* (0x007d31a0), then a return, not a refusal -- and writes it
/// with no unit test and no death test.
///
/// **Then both regroup.** The free form hands the *whole, unfiltered* list,
/// and the member form a one-element list of the unit it built (vtable
/// 0x007ac828, append 0x0041f290), to 0x00447330 with state 0 -- the
/// `Squadize` worker (`squadize_bodies` in sim/squad.cpp): each owned body
/// gets a fresh squad whose `SF_NOAI` bit is copied from the unit's own flag,
/// goes through `AddToSquad`, and the single-member squads it seeds take state
/// 0. That is what makes the flag bite: the AI reads the *squad's* bit, so a
/// tactic script's `SetNoAIFlag(ol, false)` is what hands its army back to the
/// AI, and `true` is what takes a unit out of an AI squad. This engine wrote
/// the unit's bit and stopped there until the host audit read the bodies, on
/// a reading of the call as a post to the AI that nothing received.
///
/// **The worker takes every member, unit or not.** 0x00447330 asks each
/// member only that it resolves and has an owner (`[obj+0x70]`); there is no
/// unit test and no death test in it. So a building or a corpse in the list
/// is regrouped like a unit -- a squad of its own, `AddToSquad` -- exactly as
/// `squadize_bodies` does for anything it is handed. No shipped list holds
/// one.
template <bool kFiltered>
HostOutcome set_no_ai_flag(CallContext& ctx, std::size_t value_index) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetNoAIFlag: no world");
  const bool wanted = ctx.arg(value_index).truthy_scalar();

  std::vector<ObjectId> ids;
  if (kFiltered) {
    if (!is_objlist(ctx.arg(0))) return HostOutcome::failed("SetNoAIFlag: expected an ObjList");
    const std::span<const ObjectId> items = objlist_pool_of(*world).items(objlist_of(ctx.arg(0)));
    ids.assign(items.begin(), items.end());
  } else {
    const WorldObject* slot = object_of(*world, ctx.arg(0));
    if (slot == nullptr) return HostOutcome::ok_void();
    ids.push_back(slot->id);
  }

  std::vector<ObjectId> bodies;
  for (const ObjectId id : ids) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr) continue;
    if (!kFiltered || (slot->state.flags.is_unit && !object_is_dead(*slot))) {
      if (ObjectState* state = world->mutable_state(id); state != nullptr) state->flags.no_ai = wanted;
    }
    bodies.push_back(id);
  }

  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr && !bodies.empty()) {
    std::vector<SquadKey> seeds;
    squadize_bodies(*world, *heroes, bodies, 0, seeds);
  }
  return HostOutcome::ok_void();
}

HostOutcome fn_set_no_ai_flag(CallContext& ctx) { return set_no_ai_flag<true>(ctx, 1); }
HostOutcome m_set_no_ai_flag(CallContext& ctx) { return set_no_ai_flag<false>(ctx, 1); }

/// The three set-algebra functions: `Union` (0x00573260), `Intersect`
/// (0x005733f0) and `Subtract`/`Substract` (0x00573580, both spellings ship, 8
/// and 2 call sites). Each pops its second operand, then its first; an operand
/// that is not a live query prints *"Parameter #N ..."* and answers the invalid
/// handle (first operand checked first). Then 0x004fe490 makes the query with
/// its op literal -- see `SetOp` -- taking a reference on each operand, and the
/// refresh's per-op order is `World::evaluate_query`'s.
///
/// ## The query model this engine does not have (a known gap)
///
/// Every query constructor here -- these three, `ObjsInSight`, `ObjsInRange`,
/// `ObjsInCircle`, `ClassPlayerObjs`, `UnitsInSettlement` -- does three things
/// in `gbr.exe` that this engine's queries do not:
///
///   * **interning**: it scans a per-type pool first and answers the *existing*
///     query when the key matches (set-op 0x00996a34 on `(op, a, b)`, class-
///     player 0x00996a44, sight 0x00996a84 on `(observer, class, radius,
///     visible)`, circle 0x00996aa4 on `(x, y, r, class)`, settlement 0x00996a70);
///   * **a timed cache**: a query refreshes its list only when the game clock
///     reaches its next-due time (`+0x4c`), every 4000 ms for a player-flags
///     query, 2000 for a set-op or units-in-settlement query and 1000 for the
///     others (`GetList`, 0x004f9490), so a read can be seconds stale; and
///   * **a refcounted lifetime** (0x00571670 adds, 0x00571680 releases and
///     destroys at zero).
///
/// The timed cache is modelled (`World::read_query`, docs/formats/
/// query-model.md); interning and the reference count are not yet: every call
/// mints a fresh query object, refreshed on its first read and never freed
/// (`World::create_query`). A temporary therefore reads as fresh, as the
/// original's freshly made one does; what differs is that two equal calls
/// share one cache there and two here, and that the handles a script sees are
/// allocated differently. An area operand's list is in the original's grid
/// order (`sim/spatial_index.hpp`), and the operation walks it in that order.
template <SetOp kOp>
HostOutcome fn_set_op(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("set operation: no world");
  const WorldObject* lhs = object_of(*world, ctx.arg(0));
  const WorldObject* rhs = object_of(*world, ctx.arg(1));
  if (lhs == nullptr || rhs == nullptr) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = set_op(kOp, lhs->id, rhs->id);
  return HostOutcome::ok_with(query_value(world->intern_query(spec)));
}

/// `Count(player, "class")` -- what the player's class tally holds for the
/// class.
///
/// The one free function here that answers immediately rather than minting a
/// query object. It has to: the corpus assigns its result to an `int`, so no
/// handle escapes, and minting a query per call would leak a handle per call
/// and shift every later allocation.
///
/// **The original does not count; it reads a counter.** 0x00422300 takes the
/// player record at `player - 1` (guarded only by `player - 1 < 16`, so 0 and
/// below read a record that is not there; answered 0 here) and asks 0x004441e0
/// on the record's squad holder (`[record+0x8c]`), which looks the class-name
/// string up in the holder's map (0x00444030) and answers the number in the
/// node, or 0 when the name has no node. The map is the AI's class tally:
/// the register (0x0041f310 -> 0x00447200) adds one for the unit's class and
/// each ancestor, the unregister (0x0041eee0 -> 0x004448a0) takes one away,
/// both on the owner of the moment. `HeroSystem::class_tally` is it, and
/// `docs/formats/class-tally.md` says where it parts from a census: a unit
/// counts from its registration (at once for `Place`, at the turn's sweep
/// for other births) to its death, inside a holder as on the map, and a unit
/// that changed hands stays in its first owner's count and leaves its new
/// owner's when it dies.
///
/// The name is looked up as a class **id**, exactly: the map is keyed on the
/// class's own name, so an `altid` has no node.
HostOutcome fn_count(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Count: no world");
  const PlayerId player = player_arg(ctx.arg(0));
  if (player == kNoPlayer || !ctx.arg(1).is_string()) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(
      Value::integer(player_class_count(*world, player, ctx.arg(1).as_string())));
}

}  // namespace

std::int32_t player_class_count(World& world, PlayerId player, std::string_view class_name) {
  if (player == kNoPlayer) return 0;
  const HeroSystem* heroes = hero_system_of(world);
  const ClassGraph* graph = world.class_graph();
  if (heroes == nullptr || graph == nullptr) return 0;
  const ClassIndex cls = graph->find(class_name);
  if (cls == kNoClass) return 0;
  return heroes->class_tally(player, cls);
}

namespace {

// --------------------------------------------------------------------------
// named objects: `map.obj.xml`'s `<group type="0">`
// --------------------------------------------------------------------------
//
// `NamedObj` is its own script type in `gbr.exe` -- code `0x22`, three members
// and one constructor, all registered in one block at `0x0055342c`:
//
//     GetNamedObj        0x0055342c   returns 0x22, (str)
//     NamedObj::IsValid  0x0055345b   returns 7,    (0x22)
//     NamedObj::IsDead   0x00553471   returns 7,    (0x22)
//     NamedObj::obj      0x0055348d   returns 0x14, (0x22)   -- 0x14 is `Obj`
//
// and it is the type a bare `<group type="0">` name has in a sequence script,
// because `RunSequence`'s prologue builder (`0x005bc280`) declares each one as
// `NamedObj <n>; <n>=GetNamedObj("<n>");`. That is why `.obj` is 1,174 sites in
// the corpus and 1,173 of them are inside the map containers: it is how every
// one of those names reaches the object behind it. See `sim/globals.hpp`.
//
// A handle carries the *table index*, not the object id, because the binding
// outlives its object -- `World::despawn` prunes the group table and leaves
// this one alone -- and `IsDead` has to have an entry to ask about.

[[nodiscard]] Value named_object_value(std::int32_t index) {
  if (index == NamedObjectTable::kNoName) return invalid_object();
  return Value::object(ObjectRef{kTypeNamedObj, static_cast<std::uint32_t>(index)});
}

/// The table index a `NamedObj` handle names, or `kNoName` for anything else.
[[nodiscard]] std::int32_t named_index_of(const World& world, const Value& value) noexcept {
  if (!value.is_object()) return NamedObjectTable::kNoName;
  const ObjectRef ref = value.as_object();
  if (ref.type != kTypeNamedObj) return NamedObjectTable::kNoName;
  const auto index = static_cast<std::int32_t>(ref.id);
  return world.named_objects().live(index) ? index : NamedObjectTable::kNoName;
}

/// `GetNamedObj("...")` (0x00553230) -- 66 of the 67 literal call sites in the
/// shipped map scripts, and the other one is a group name the editor should
/// have caught.
///
/// Looks the name up in the named map at `[0x009bdacc]` (0x004c93c0) and, **on
/// a miss, mints one**: an empty `CVXNamedObjQuery` (0x005722a0) registered
/// (0x00571650) and inserted under the name (0x004cf1c0). It then takes a
/// reference and answers the handle; the invalid handle is pushed only if the
/// allocation failed. So an unknown name is a *valid, empty* named object --
/// `IsValid` true, `IsDead` true, `.obj` invalid -- and a later `SetName` of
/// that name (here `NamedObjectTable::rebind`) fills the same entry, so a
/// handle taken before the object existed resolves once it does. This
/// answered the invalid handle for a miss until the host audit read the body.
HostOutcome fn_get_named_obj(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetNamedObj: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("GetNamedObj: expected a name");
  return HostOutcome::ok_with(
      named_object_value(world->named_objects().intern(ctx.arg(0).as_string())));
}

/// The object a named entry currently answers with: its bound object if that
/// is still in the world and not dead, else nothing.
///
/// `CVXNamedObjQuery`'s refresh (vtbl+0x1c, 0x00572c10 through 0x005719d0)
/// purges from its deque, in place and for good, every slot whose table entry
/// is gone or whose `vtbl + 0x50` says dead (`object_is_dead`); the list
/// (vtbl+0x20, 0x005716f0) and the emptiness test (vtbl+0x28, 0x00571710)
/// read what is left. The purge cannot be observed separately -- death is
/// final -- so it is modelled by asking at each read.
[[nodiscard]] const WorldObject* named_live_object(World& world, std::int32_t index) {
  const WorldObject* bound = world.find(world.named_objects().object(index));
  if (bound == nullptr || object_is_dead(*bound)) return nullptr;
  return bound;
}

/// `NamedObj::obj` (0x005530a0), which is also the registered `NamedObj -> Obj`
/// conversion (see `object_of`).
///
/// An invalid handle prints `named::obj` and answers the invalid handle.
/// Otherwise it purges the dead from the entry's deque (`named_live_object`);
/// an entry left empty prints *"Named unit %s is dead or not initialized!"*
/// and answers the invalid handle, and a live one answers its front object as
/// an `Obj` handle. **A named unit that has died is not reachable through its
/// name**, corpse or not; a named building at no health still is, because a
/// building does not die (`object_is_dead`). This answered the bound object
/// dead or alive for a while, on the reading that `IsDead` needed it -- it
/// does not: `IsDead` asks the entry, not the object.
HostOutcome m_named_obj_obj(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("obj: no world");
  const std::int32_t index = named_index_of(*world, ctx.arg(0));
  if (index == NamedObjectTable::kNoName) return HostOutcome::ok_with(invalid_object());
  const WorldObject* live = named_live_object(*world, index);
  return HostOutcome::ok_with(live == nullptr ? invalid_object() : object_value(live->id));
}

// --------------------------------------------------------------------------
// members: identity and state
// --------------------------------------------------------------------------

/// `IsValid/0` -- one name over four bodies, and they ask different things.
///
///   * `Item`/`Obj`/`Settlement::IsValid` (0x005ab1d0): the handle check and
///     nothing else (`object_of`'s doc) -- no message, false on a miss. A
///     corpse still in the table is valid.
///   * `NamedObj`/`Query::IsValid` (0x00577af0): the handle check, then the
///     query's `vtbl + 0x2c`, "am I registered". For a named object that is
///     membership of the named map (0x00571ac0 via 0x00571a40), which every
///     handle `GetNamedObj` gives holds, so it asks whether the *name* exists,
///     not whether its object lives -- `IsDead` is that question. A group
///     query asks the group map (0x00571c10); every other kind -- sight,
///     units in settlement, set operation, player-flags, class-player,
///     circle, rectangle -- answers 1 (0x00444560). The two maps drop an
///     entry only when both are replaced on entering a new map (0x004d04d0),
///     which only `ChangeMap` causes; it has no shipped call and is not
///     implemented. So within a map a query whose handle resolves is valid,
///     and that is the answer here: a named handle while its table entry
///     exists, a query while its object resolves.
///     docs/formats/query-model.md.
///   * `SquadList::IsValid` (0x00421a70): the receiver is an index into the
///     squad-list pool (0x008c8dc4/0x008c8dc8, 16-byte entries, 0x0043ed70),
///     valid while it lies inside the pool -- and the pool is a vector of
///     1,024 entries sized once by its static constructor (0x007a8920) and
///     never resized, so every index from 0 to 1,023 is valid, a released
///     list's included (the release, 0x0043ff60, only frees the deque and
///     clears the in-use word at `+0xc`). The claim (0x0043ed10) answers -1
///     when all 1,024 are in use, and -1 is the one handle that is invalid.
///     Here a handle is the index plus one. docs/formats/squad.md.
HostOutcome m_is_valid(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsValid: no world");
  if (ctx.arg(0).is_object() && ctx.arg(0).as_object().type == kTypeNamedObj) {
    return HostOutcome::ok_with(
        Value::boolean(named_index_of(*world, ctx.arg(0)) != NamedObjectTable::kNoName));
  }
  if (is_squadlist(ctx.arg(0))) {
    const SquadListId list = squadlist_of(ctx.arg(0));
    return HostOutcome::ok_with(
        Value::boolean(list != kNoSquadList && list <= kSquadListPoolEntries));
  }
  return HostOutcome::ok_with(Value::boolean(object_of(*world, ctx.arg(0)) != nullptr));
}

/// `Obj::IsVisible` (0x005ab970) -- 56 call sites, and the top trap on every
/// shipped map once the target sweep stops failing before it.
///
/// The handle check, then **not** bit 21 of the sync flags at `[obj+0x2c]`;
/// registered at 0x005b7721 as `bool, Obj`. **The stored bit means hidden**,
/// so this is its negation; the same bit gates a candidate out of the target
/// sweep at 0x005dba19, which is why an invisible unit cannot be attacked. A
/// miss prints and answers false: `UNIT_IDLE.VS` asks this of a target it has
/// just acquired, and a target that died in between is exactly that case.
HostOutcome m_is_visible(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsVisible: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && !slot->state.flags.hidden));
}

/// `SetVisible(bool)` -- two bodies, one registration here.
///
///   * `Obj::SetVisible` (0x005ab920): the handle check -- a miss prints and
///     returns, **it does not trap**, so `Instructor.obj.SetVisible(false)` in
///     the tutorial and `NO_SecondDruid.obj.AsUnit().SetVisible(true)` in Great
///     Losses Spain go on when the object has gone -- then 0x005a7600: a visual
///     effect when the bit changes (presentation), the bit itself set or
///     cleared through `vtbl + 0x44`/`+0x48` (0x0040a500/0x0040a520, mirrored
///     into the record at `[[obj+0x30]+0x10]`), and a sprite and selection
///     refresh (presentation). The argument is visibility; the bit is
///     hiddenness.
///   * `ObjList::SetVisible` (0x0055ea60): first, for every member that
///     resolves and carries the hero bit, the same 0x005a7600 on every live
///     member of the hero's army (the list at hero+0x1cc); then on every live
///     member of the list. No shipped site has an `ObjList` receiver.
HostOutcome m_set_visible(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetVisible: no world");
  const bool visible = ctx.count() > 1 && ctx.arg(1).truthy_scalar();
  const auto apply = [&](ObjectId id) {
    if (ObjectState* state = world->mutable_state(id); state != nullptr) state->flags.hidden = !visible;
  };
  if (is_objlist(ctx.arg(0))) {
    const std::span<const ObjectId> items = objlist_pool_of(*world).items(objlist_of(ctx.arg(0)));
    const std::vector<ObjectId> members(items.begin(), items.end());
    if (const HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) {
      for (const ObjectId id : members) {
        const HeroRecord* hero = world->find(id) == nullptr ? nullptr : heroes->hero(id);
        if (hero == nullptr) continue;
        for (const ObjectId soldier : hero->army) apply(soldier);
      }
    }
    for (const ObjectId id : members) apply(id);
    return HostOutcome::ok_void();
  }
  if (const WorldObject* slot = object_of(*world, ctx.arg(0)); slot != nullptr) apply(slot->id);
  return HostOutcome::ok_void();
}

/// `Obj::IsAlive` (0x005aae00): the handle check, then `vtbl + 0x50` negated
/// -- valid and not dead (`object_is_dead`). A miss answers false, silently.
/// So a building at no health is alive: it is broken, and stays its owner's.
HostOutcome m_is_alive(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsAlive: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && !object_is_dead(*slot)));
}

/// `IsDead/0` -- two bodies.
///
///   * `Obj::IsDead` (0x005aae50): invalid, or `vtbl + 0x50` says dead
///     (`object_is_dead`). No message.
///   * `NamedObj::IsDead` (0x00553000): an invalid handle is dead; otherwise
///     the entry's purge (`named_live_object`) and then "is the deque empty".
///     So an entry whose object died, and one `GetNamedObj` minted that
///     nothing carries yet, are both dead; a named building at no health is
///     not.
HostOutcome m_is_dead(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsDead: no world");
  const std::int32_t named = named_index_of(*world, ctx.arg(0));
  if (named != NamedObjectTable::kNoName) {
    return HostOutcome::ok_with(Value::boolean(named_live_object(*world, named) == nullptr));
  }
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot == nullptr || object_is_dead(*slot)));
}

/// `player`, `pos`, `health`, `maxhealth` and `InHolder` are **registered on
/// `Squad` too**, and for a while every one of them answered a squad the way
/// it answers an object that is not there -- `-1`, `(-1, -1)`, 0, 0, false --
/// silently, at some sixty sites across the AI scripts (`sq.Player` alone is
/// thirty of them, and `AIOSENDSQUAD.VS` hands it to `PrepareAiTransportShip`).
/// `squad_receiver` in sim/squad.hpp is the rule; the squad halves are:
///
///   * `Squad::Player` (0x004210c0): the handle's low nibble plus one, with no
///     lookup at all -- a handle naming no live squad still has an owner;
///   * `Squad::pos` (0x00422cf0): the squad from the handle (0x00443e30, the
///     low nibble and the index above it), its front member (0x00443df0,
///     null when the count at `+0x50` is 0 or the slot is gone), and that
///     member's `vtbl + 0xc8` -- `posRH`; `(-1, -1)` for a squad with no front.
///     A handle naming no squad faults in the original (no null check after
///     the lookup), and answers `(-1, -1)` here;
///   * `Squad::health` / `maxhealth` (0x00427740, 0x00427800): the **sum** over
///     the members of what each would answer on its own;
///   * `Squad::InHolder` (0x00427670): **every** member held, and an empty
///     squad vacuously so.
HostOutcome m_player(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("player: no world");
  if (is_squad(ctx.arg(0))) {
    return HostOutcome::ok_with(Value::integer(player_to_script(unpack_squad(ctx.arg(0)).player)));
  }
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  // `Obj::player` (0x005ab290): the handle check, then the owner record's
  // index (`[[obj+0x70]+8]`) plus one; a miss prints and answers -1.
  // `Settlement::player` (0x005c2340) is the same through the settlement's
  // record at `+0x90`.
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(-1));
  // **Script player numbers are 1..16, not 0..15**, and this returned the raw
  // index for a while. `Obj::player` in `gbr.exe` (0x005ab290) returns `ID + 1`;
  // `Settlement::IsOwn`, `Outposts`, `Strongholds` and `MilUnits` all `dec`
  // their argument; five of the executable's error strings say "between 1 and
  // 16" in words; and the corpus agrees on its own -- `STONEHENGE_WISDOM.VS`
  // walks every player as `for (k = 1; k <= 16; k += 1)`.
  //
  // 531 call sites read this, and an off-by-one here would have made every one
  // of them name the wrong player. `player_to_script` is the only place the
  // adjustment is written; see `sim/player_host.hpp`.
  return HostOutcome::ok_with(Value::integer(player_to_script(slot->state.owner)));
}

HostOutcome m_pos(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("pos: no world");
  if (const Squad* squad = squad_receiver(*world, ctx.arg(0)); squad != nullptr) {
    if (squad->members.empty() || world->find(squad->members.front()) == nullptr) {
      return HostOutcome::ok_with(pack_point(kHeldPosition));
    }
    // The front member's `vtbl+0xc8` (0x00422d37), which for a garrisoned
    // unit is its settlement's central building rather than the holder walk.
    return HostOutcome::ok_with(
        pack_point(unit_pos_rh(*world, squad->members.front())));
  }
  if (is_squad(ctx.arg(0))) return HostOutcome::ok_with(pack_point(kHeldPosition));
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));
  // **Raw**: 0x005add20 copies `[obj+0x24]`/`[obj+0x28]` and asks nothing
  // else, so a held object answers the `(-1, -1)` its holder entry wrote
  // (0x005d3e10 sets it through `vtbl+0x38` at 0x005d3ef9) -- which is what
  // `SETTLEMENT_BEHAVIOR_AMBIENT.VS` tests `ol[i].pos.x == -1` for, to erase a
  // villager that has already gone inside. This answered the holder walk for
  // a while, and the walk ends on the holder record at (0, 0): the villager
  // was sent to the door nearest the map's corner instead.
  return HostOutcome::ok_with(pack_point(slot->state.position));
}

/// `Obj::posRH` -- 141 call sites, and **the same value as `pos` for
/// everything but a held unit**.
///
/// `Obj::pos` (0x005add20) reads the object's stored fields at `+0x24`/`+0x28`
/// directly; `Obj::posRH` (0x005ad8a0) calls the virtual at `vtbl + 0xC8`. In
/// **all 15** classes carrying the shared `SetPos` at `vtbl + 0x38` that slot
/// is one thunk, 0x005a75d0, forwarding to `vtbl + 0x3c` -- the same function
/// in all 15, 0x0063d900, whose entire body is `out = {[obj+0x24],
/// [obj+0x28]}` -- so for those `posRH` is `pos` reached through two
/// indirections. (The census is anchored on the `SetPos` pointer rather than
/// on a guessed vtable start, which is what makes it exhaustive.)
///
/// **The units are the seven classes it leaves out**, and they override the
/// slot: a unit's `SetPos` is 0x005d39e0 and its `vtbl + 0xC8` is 0x005d3db0,
/// which for a unit in a holder answers its settlement's central building, or
/// the ship it is aboard -- `unit_pos_rh` in `sim/economy.hpp`. So a garrisoned
/// or embarked unit's `posRH` is a place on the map while its `pos` is
/// `(-1, -1)`. Both answered the holder walk here for a while, which ends on
/// the holder record at (0, 0): `UNIT_TRAIN.VS`'s notification, a druid's
/// summons from inside a town (`Place("GGhost", .posRH, …)`) and the item
/// scripts' "cannot use in holder" all went to the map's corner.
///
/// What also differs is the miss. `pos` prints `The function 'Obj::pos' called
/// for an uninitialized or invalid object` before pushing its result; `posRH`
/// pushes silently. Both push **(0, 0)** -- not the `(-1, -1)` held marker --
/// at 0x005add4d and 0x005ad8c6, so the difference is invisible
/// to a script and this engine keeps the two bodies apart anyway -- a
/// diagnostic that exists is worth having a place to go.
HostOutcome m_pos_rh(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("posRH: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));
  if (slot->state.flags.is_unit) {
    return HostOutcome::ok_with(pack_point(unit_pos_rh(*world, slot->id)));
  }
  return HostOutcome::ok_with(pack_point(slot->state.position));
}

/// `health/0` -- `Obj::health` (0x005ada80) is the handle check and
/// `[obj+0xc0]`, 0 on a miss. `Squad::health` (0x00427740) splits the handle
/// into player and index, looks the squad up without a bounds check
/// (0x00443e30), and sums `[+0xc0]` over the members whose table slot is live;
/// an empty squad is 0, and nothing is printed.
///
/// `Settlement::health` (0x005c1e40) and `Settlement::stamina` (0x005c1f00)
/// read the settlement's **garrison holder** -- the 16-bit handle at
/// `[settlement+0x5e]`, the holder `UnitsCount` and `max_units` read -- at
/// `+0x1c` and `+0x24`, 0 on a miss. **Both are always 0.** The holder
/// constructor (0x00531de0) zeroes `+0x14..+0x24`, and the one other writer
/// of the two fields is the holder's own archive exchange (0x00531fb0, the
/// `Holder` record's `health` and `stamina`), which loads back what a save
/// wrote from the same zero; no map file carries a `Holder` record. So a
/// settlement receiver answers 0 here, whatever its objects hold.
/// docs/formats/settlement.md.
HostOutcome m_health(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("health: no world");
  if (const Squad* squad = squad_receiver(*world, ctx.arg(0)); squad != nullptr) {
    std::int32_t total = 0;
    for (const ObjectId member : squad->members) {
      if (const WorldObject* slot = world->find(member); slot != nullptr) {
        total += slot->state.health;
      }
    }
    return HostOutcome::ok_with(Value::integer(total));
  }
  if (ctx.arg(0).is_object() && ctx.arg(0).as_object().type == kTypeSettlement) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::integer(slot != nullptr ? slot->state.health : 0));
}

/// One object's `maxhealth`, the rule `m_max_health` below documents.
[[nodiscard]] std::int32_t max_health_of_object(World& world, const WorldObject& slot) {
  if (const CombatSystem* combat = combat_system_of(world); combat != nullptr) {
    if (const Combatant* unit = combat->find(slot.id); unit != nullptr) {
      return combat->max_health_of(*unit);
    }
  }
  return class_int(world, slot, "maxhealth");
}

/// `.maxhealth` -- 109 sites, and the class property on every object that is
/// not a registered combatant.
///
/// `Obj::maxhealth` (0x005adb40) reads `[obj+0xc8]` (0 on a miss), which the
/// object constructor fills from
/// the class and `Unit::RecalcBonuses`'s virtual rebuilds -- from the class
/// again, plus whatever `Unit::AddBonus` has put on that object. Buildings,
/// wagons and item holders carry the field too and nothing ever adds to
/// theirs, so the class property is the same number for them; a combatant is
/// asked through `CombatSystem`, which is the one place that knows what the
/// record holds. This is the same split `health/0` above already lives with,
/// and it is why the combat system is consulted rather than duplicated.
HostOutcome m_max_health(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("maxhealth: no world");
  if (const Squad* squad = squad_receiver(*world, ctx.arg(0)); squad != nullptr) {
    std::int32_t total = 0;
    for (const ObjectId member : squad->members) {
      if (const WorldObject* slot = world->find(member); slot != nullptr) {
        total += max_health_of_object(*world, *slot);
      }
    }
    return HostOutcome::ok_with(Value::integer(total));
  }
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(max_health_of_object(*world, *slot)));
}

/// `stamina/0` -- `Obj::stamina` (0x005adae0) and `Unit::stamina`
/// (0x005d8b90) both read `[obj+0xc4]` after the handle check, 0 on a miss.
/// The object constructor (0x005b4ca0) seeds it from the class `maxstamina`
/// (class `+0x298`). This engine seeds it for combatants only; no shipped
/// class outside the unit family declares a non-zero `maxstamina`, so the
/// difference cannot be seen. `Settlement::stamina` (0x005c1f00) reads its
/// garrison holder's `+0x24` instead (see `m_health`).
HostOutcome m_stamina(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("stamina: no world");
  // `Settlement::stamina`: the garrison holder's `+0x24`, always 0 (`m_health`).
  if (ctx.arg(0).is_object() && ctx.arg(0).as_object().type == kTypeSettlement) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::integer(slot != nullptr ? slot->state.stamina : 0));
}

/// `Obj::sight` (0x005adcc0): the handle check, then `[obj+0xd0]`, 0 on a miss.
/// The constructor seeds it from class `+0x2d0` and `SetSight` moves it; that
/// is `WorldObject::sight`.
HostOutcome m_sight(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("sight: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::integer(slot != nullptr ? slot->sight : 0));
}

/// `o.SetSight(n)` -- 4 sites, and the sole blocker of `TAVERN_EXPEDITION.VS`,
/// `EXPLORE_AREA.VS` and (with `SetBuildFrame` now bound) `CATAPULT_IDLE.VS`.
///
/// 0x005ab360, in the order it runs:
///
///   1. **`n < 100` is refused** -- *"Obj::SetSight: Sight must be no smaller
///      than 100"* -- and **`n > 2048` is refused** -- *"...must no greater than
///      2048"*, the typo included. Both print into the sink that is a bare
///      `ret` in retail and return **without storing**, so a script that asks
///      for 50 keeps whatever sight it had.
///   2. **only a widening explores.** `cmp [obj+0xd0], edi` / `jge` skips the
///      reveal when the object already sees at least that far, so narrowing is
///      a store and nothing else.
///   3. the reveal is the same circle stamp `ExploreCircle` uses -- the
///      object's own position, the *new* radius, the object's owner -- and it
///      is skipped for an object with no position of its own, which is the
///      `(-1, -1)` held marker.
///   4. **the store happens either way**, including after a refused reveal:
///      0x005ab42f is on the fall-through path from the `jge`.
///
/// **This makes `WorldObject::sight` turn state**, which it was not before: it
/// was resolved from the class at spawn, saved so that a reload would not lose
/// a value it could not recompute, and deliberately left out of `state_hash`.
/// A script can now move it, and two peers that disagree about how far a scout
/// sees will disagree about what it finds -- so it is hashed from here on, and
/// the conformance golden moved with it.
HostOutcome m_set_sight(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetSight: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  WorldObject* slot = found == nullptr ? nullptr : world->find(found->id);
  if (slot == nullptr || !ctx.arg(1).is_integer()) return HostOutcome::ok_void();
  const std::int32_t want = ctx.arg(1).as_integer();
  // The two refusals store nothing at all, which is what separates them from a
  // clamp: `SetSight(50)` leaves the old radius standing.
  if (want < 100 || want > 2048) return HostOutcome::ok_void();

  if (want > slot->sight && slot->state.position != kHeldPosition) {
    if (FogSystem* fog = fog_system_of(*world); fog != nullptr) {
      // The slot is the owner's, and `fog_slot_from_script` takes the 1-based
      // number a script would pass -- so the owner goes in one-based.
      fog->map().explore_circle(slot->state.position, want,
                                fog_slot_from_script(slot->state.owner + 1));
    }
  }
  slot->sight = want;
  return HostOutcome::ok_void();
}

/// `b.RRepair()` -- 3 sites, and the sole blocker of `BUILDING_REPAIR.VS` and
/// `VILLAGE_REPOPULATE.VS` between them.
///
/// 0x004dd250 is one gate and one arithmetic:
///
///   * **the building must be broken**, `[obj+0x204] == 3`, which is
///     `ObjectState::damage_state` and the same tier `Building::IsBroken`
///     answers. Anything less damaged is left alone -- this is not "repair a
///     bit", it is "put a ruin back on its feet".
///   * **a class that declares `auto_repair="yes"` is only repaired inside a
///     village.** `[class+0x2ec]` is that attribute (the reader at 0x005a33f9
///     parses `"no"` to 0 and `"yes"` to 1) and 0x00440010 is
///     `IsHeirOf(settlement.GetCentralBuilding, "BaseVillage")`. A class that
///     says `auto_repair="no"` skips the settlement test entirely and is
///     repaired wherever it stands. 146 shipped classes say no and 149 say yes.
///   * then 0x004db520 **sets** the health -- `Obj::SetHealth` is the same
///     `vtbl+0x98` -- to `(BuildingStateThreshold2 + BuildingStateHysteresis +
///     1) * maxhealth / 100`, which is 29% of the shipped numbers.
///
/// That last figure is not a balance constant: it is **exactly one percent past
/// the point where the building stops being broken**. `damage_state`'s
/// hysteresis needs `pct >= threshold2 + h` to step down out of tier 3, so the
/// repair heals to the smallest number that clears it and not a point more.
/// Writing it as the two constants rather than as 29 is what keeps that true
/// when a `CONST.INI` moves either.
///
/// Setting the health is also what re-tiers the building, because
/// `World::set_health` is where `damage_state` is recomputed -- so nothing here
/// touches the tier itself.
HostOutcome m_rrepair(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RRepair: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  if (found == nullptr) return HostOutcome::ok_void();
  const ObjectId id = found->id;
  // Broken, and nothing else.
  if (found->state.damage_state != 3) return HostOutcome::ok_void();

  const ClassGraph* graph = world->class_graph();
  const bool auto_repair =
      graph != nullptr && found->class_index != kNoClass &&
      graph->property(found->class_index, "auto_repair") == "yes";
  if (auto_repair) {
    const EconomySystem* economy = economy_of(*world);
    const Settlement* home =
        economy == nullptr ? nullptr : economy->settlements().for_object(found->settlement);
    const WorldObject* centre = home == nullptr ? nullptr : world->find(home->anchor);
    if (centre == nullptr || !class_is(*world, *centre, "BaseVillage")) {
      return HostOutcome::ok_void();
    }
  }

  const BuildingStateRules& rules = world->building_state_rules();
  const std::int64_t percent = static_cast<std::int64_t>(rules.threshold2) + rules.hysteresis + 1;
  const std::int64_t full = class_int(*world, *found, "maxhealth");
  world->set_health(id, static_cast<std::int32_t>(percent * full / 100));
  return HostOutcome::ok_void();
}

/// `d.SetJupiterAngerTarget(o)` and `d.GetJupiterAngerTarget()` -- 2 sites, and
/// between them the whole of `PRIEST_JUPITER_ANGER.VS` (9 sites) and
/// `PRIEST_ONDIE.VS` (10).
///
/// One field, `[obj+0x1d0]`, and the asymmetry is the interesting half. The
/// setter (0x00513540) stores the target's handle and asks nothing at all --
/// not whether it is alive, not whether it is a unit. The getter (0x005134b0)
/// resolves the handle and answers it **only** when the object still exists
/// and carries bit 22 of `[obj+0x2c]`, which is `is_unit`; anything else is the
/// invalid handle `0xffff`.
///
/// So the validation is on the way out, and it has to be: the priest sets its
/// grudge while the spell runs and reads it back at its own death, by which
/// time the target may be gone. `PRIEST_ONDIE.VS`'s `if (u.IsAlive())` is
/// written for exactly that.
HostOutcome m_set_jupiter_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetJupiterAngerTarget: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_void();
  WorldObject* slot = world->find(self->id);
  if (slot == nullptr) return HostOutcome::ok_void();
  // Whatever the argument names, stored as-is. The setter validates nothing.
  const WorldObject* target = object_of(*world, ctx.arg(1));
  slot->state.jupiter_target = target == nullptr ? kNoObject : target->id;
  return HostOutcome::ok_void();
}

HostOutcome m_get_jupiter_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetJupiterAngerTarget: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(invalid_object());
  const WorldObject* target = world->find(self->state.jupiter_target);
  if (target == nullptr || !target->state.flags.is_unit) {
    return HostOutcome::ok_with(invalid_object());
  }
  return HostOutcome::ok_with(object_value(target->id));
}

/// `EnemyInRange(pt, range, who)` -- 1 site, and the **sole blocker of
/// `WALL_PATROL.VS` and its 158 call sites**, the largest single script left:
/// `EnemyInRange(.pos, nWallRange, list[0])`, a wall deciding whether its
/// patrol has anything to shoot at.
///
/// 0x004dcb20 pops the three and refuses an object that does not resolve with
/// *"The function 'EnemyInRange' called for an uninitialized or invalid
/// object."* and `false`. Otherwise it sweeps the object grid (0x004dbea0)
/// over the square `[x - range, x + range] x [y - range, y + range]`, built
/// at 0x004dcba0, with a functor (0x00514080) holding the asker's owner's
/// enemy word (`[record+0x10]`, `PlayerTable::is_enemy`'s complement of the
/// ceasefire bit) and side mask (`[record+0x18]`), the point, the range and
/// the asker. An object is found -- and the sweep stops -- when:
///
///   * its owner's bit is in the enemy word;
///   * it is not hidden (bit 21 of `[obj+0x2c]`; the side mask lets a hidden
///     object past the first test, and the second then refuses it anyway);
///   * it is a **unit**: anything else is handed to the asker's `CanAttack`
///     virtual (`vtbl+0xb4`) as null, and refused;
///   * the asker can attack it -- that virtual, which is `unit_can_attack`;
///   * its class's `target_factor` (`[cls+0xb2c]`) is not negative -- crows,
///     deer, eagles, fish and hens declare -1, and the absent marker is
///     negative too;
///   * and **its distance from the point, less its class `radius`**
///     (0x005a7900: `isqrt(dx^2 + dy^2) - [cls+0x2dc]`), is at most `range`.
///
/// So the square only bounds the sweep: the test is a circle widened by the
/// target's own radius. An asker or a candidate the combat system does not
/// hold cannot answer `CanAttack` and finds nothing.
HostOutcome fn_enemy_in_range(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("EnemyInRange: no world");
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  if (ctx.count() < 3 || !is_point(ctx.arg(0)) || !ctx.arg(1).is_integer()) return no;
  const WorldObject* who = object_of(*world, ctx.arg(2));
  if (who == nullptr) return no;
  const CombatSystem* combat = combat_system_of(*world);
  const Combatant* asker = combat == nullptr ? nullptr : combat->find(who->id);
  if (asker == nullptr) return no;

  const Point centre = unpack_point(ctx.arg(0));
  const std::int32_t range = ctx.arg(1).as_integer();
  const PlayerTable& players = world->players();
  const PlayerId asking = who->state.owner;
  // The grid sweep: a held object stands in no cell and is not there to be
  // found.
  std::vector<ObjectId> near;
  world->objects_located_in_rect(centre.x - range, centre.y - range, centre.x + range,
                                 centre.y + range, near);
  for (const ObjectId id : near) {
    const WorldObject* other = world->find(id);
    if (other == nullptr || other->state.flags.unspawned) continue;
    if (!players.is_enemy(asking, other->state.owner)) continue;
    if (other->state.flags.hidden || !other->state.flags.is_unit) continue;
    const Combatant* target = combat->find(id);
    if (target == nullptr || !combat->unit_can_attack(*asker, *target)) continue;
    if (class_int_or_marker(*world, *other, "target_factor") < 0) continue;
    const Point at = other->state.position;
    const std::int64_t dx = static_cast<std::int64_t>(at.x) - centre.x;
    const std::int64_t dy = static_cast<std::int64_t>(at.y) - centre.y;
    const std::int64_t reach = isqrt(dx * dx + dy * dy) - class_int(*world, *other, "radius");
    if (reach > range) continue;
    return HostOutcome::ok_with(Value::boolean(true));
  }
  return no;
}

/// `Obj::radius` (0x005aa520): the handle check -- a miss prints and answers 0
/// -- then the class descriptor's `+0x2dc`, which the class reader fills from
/// the `radius` property through 0x00660f00 (0x005a1a8c), inherited from the
/// parent when a class does not declare it. `class_int` reads the same
/// property up the same tree; it answers 0 for a value with trailing junk where
/// the reader's integer parse would keep the leading digits, and no shipped
/// class has one.
HostOutcome m_radius(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("radius: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(class_int(*world, *slot, "radius")));
}

/// `InHolder` with no argument, two bodies on one name. `Unit::InHolder`
/// (0x005d6ff0) is `[unit+0x154] != 0xffff`, false with a print for an invalid
/// receiver. `Squad::InHolder` (0x00427670) walks the squad's member deque and
/// answers false at the first member that resolves and is not held; a member
/// that does not resolve is stepped over, so an empty squad is in its holder.
/// The squad handle itself is not checked (0x00443e30). The arity-1 form, a
/// count against a settlement name, is `sim/economy.cpp`'s.
HostOutcome m_in_holder(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("InHolder: no world");
  if (const Squad* squad = squad_receiver(*world, ctx.arg(0)); squad != nullptr) {
    // Every member, and a member that is not there is skipped rather than
    // counted against: 0x004276fb steps over a null slot.
    for (const ObjectId member : squad->members) {
      const WorldObject* slot = world->find(member);
      if (slot != nullptr && !slot->state.is_held()) {
        return HostOutcome::ok_with(Value::boolean(false));
      }
    }
    return HostOutcome::ok_with(Value::boolean(true));
  }
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && slot->state.is_held()));
}

/// `u.EnterHolder(o)` -- 1 site, `GROUP_CONVERT2PEASANT.VS`, where a freshly
/// placed villager joins the holder of the unit it replaces.
///
/// 0x005d7210 refuses a receiver that is already held, then resolves the
/// holder from the argument three ways: a **building** contributes its
/// settlement's holder (`[bld+0x148]` -> `[settlement+0x5e]`); anything else
/// contributes the holder it *carries* (`vtbl+0xe4`, which a ship answers with
/// its own) or, carrying none, the holder it is *in*. So `v.EnterHolder(u)`
/// on a garrisoned `u` puts `v` beside it. No holder resolved is a no-op.
HostOutcome m_enter_holder(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("EnterHolder: no world");
  const WorldObject* unit = object_of(*world, ctx.arg(0));
  const WorldObject* target = object_of(*world, ctx.arg(1));
  if (unit == nullptr || target == nullptr || unit->state.is_held()) return HostOutcome::ok_void();

  ObjectId holder = kNoObject;
  if (target->state.flags.is_building) {
    if (EconomySystem* economy = economy_of(*world)) {
      const Settlement* s = economy->settlements().for_object(target->settlement);
      if (s != nullptr) holder = s->holder.object;
    }
  } else if (target->object != nullptr && target->object->is_a(NativeClass::ship)) {
    const WorldObject* carried = world->find(target->id + 1);
    if (carried != nullptr && carried->internal == InternalKind::holder) holder = carried->id;
  } else {
    holder = target->state.holder;
  }
  if (holder == kNoObject) return HostOutcome::ok_void();
  (void)world->put_in_holder(unit->id, holder);
  return HostOutcome::ok_void();
}

/// `u.ExitHolder(pt)` -- 1 site, the other half of the same script. 0x005d71c0
/// runs 0x005d3f20 -- the routine the ship unboarding uses, and
/// `sim/boarding.hpp` lists the three clauses of it that are not reproduced --
/// when the receiver is held, and nothing otherwise.
HostOutcome m_exit_holder(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ExitHolder: no world");
  const WorldObject* unit = object_of(*world, ctx.arg(0));
  if (unit == nullptr || !unit->state.is_held()) return HostOutcome::ok_void();
  if (!is_point(ctx.arg(1))) return HostOutcome::failed("ExitHolder: argument is not a point");
  (void)world->remove_from_holder(unit->id, unpack_point(ctx.arg(1)));
  // Off the settlement's roster too, which is the holder's own list: the
  // original's removal (0x00531a80) is one routine for both.
  (void)garrison_forget(*world, unit->id);
  return HostOutcome::ok_void();
}

/// `o.SetName(str)` -- 1 site. 0x005b6920 hands the string to 0x005b6120, the
/// worker `CVXGroup::Spawn` uses to carry a name onto a spawned copy, and
/// `NamedObjectTable::rebind` is that worker's contract: a name already bound
/// moves to this object.
///
/// **An empty string removes the object's name.** The worker's first test
/// takes the object's own name record at `[obj+0x84]`, unlinks it from the
/// named-object manager (0x004fc150), deletes it and writes `0xffff` back --
/// so a handle taken before is no longer valid, `GetNamedObj` of that name
/// mints a fresh, empty entry whose `.obj` is nothing, and a later spawn of
/// the template it came from is not named either. The one shipped site is
/// exactly that: `2_Great_loses_Spain` seq12 renames nobody, it calls
/// `GetNamedObj("RomeHero"+n).obj.SetName("")` once the hero has joined the
/// army, and the `SetLevel` two lines on reaches nothing in the original.
/// Here every name bound to the object is taken away (`unbind_object`); the
/// original keeps one record per object, and a shipped object carrying two
/// names is not known.
///
/// **One arm is not reproduced**: an object that already has a name and is
/// given a different one has its record *renamed* (0x00572a30), so the old
/// name stops resolving; here the old name stays bound beside the new. No
/// shipped site names an object twice.
HostOutcome m_set_name(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetName: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || !ctx.arg(1).is_string()) return HostOutcome::ok_void();
  if (ctx.arg(1).as_string().empty()) {
    (void)world->named_objects().unbind_object(slot->id);
    return HostOutcome::ok_void();
  }
  (void)world->named_objects().rebind(ctx.arg(1).as_string(), slot->id);
  return HostOutcome::ok_void();
}

/// `u.Disappear()` -- 1 site, `UNIT_DISAPPEAR.VS`, which then sleeps two
/// seconds and puts the unit back where it was.
///
/// 0x005d86d0: the unit's transient activity (`[obj+0x130]`, not saved) goes
/// to 0, its path is dropped (0x005d3830 with null), its sprite is told so
/// (`[obj+0x40]`'s `vtbl+4`), its animation is reset (0x0053d730), and it is
/// placed at the static point `(-1, -1)` -- off the map, which is what makes
/// it vanish. **The command queue is not touched**: `disappear` is itself a
/// queued command, and the ambient villagers queue their errand behind it
/// (`grab_water`, then `moveenter` home), which a cleared queue lost. Here:
/// the movement state stopped and the position set to `kHeldPosition`, which
/// the world permits on an object that is not held. An unresolvable receiver
/// prints and pushes a stray byte in the original; here it is silence.
HostOutcome m_disappear(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Disappear: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  const ObjectId id = slot->id;
  if (MovementSystem* movement = movement_system(*world)) movement->stop(*world, id);
  if (ObjectState* state = world->mutable_state(id)) state->flags.has_active_path = false;
  if (!slot->state.is_held()) (void)world->set_position(id, kHeldPosition);
  return HostOutcome::ok_void();
}

/// `u.IsDiseased()` -- 1 site. Bit 1 of the second flag word; see
/// `ObjectFlags::diseased`.
HostOutcome m_is_diseased(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsDiseased: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && slot->state.flags.diseased));
}

/// `u.Disease()` -- 1 site. Sets the bit; the visual it also spawns is not
/// reproduced (see `ObjectFlags::diseased`).
HostOutcome m_disease(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Disease: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  if (ObjectState* state = world->mutable_state(slot->id)) state->flags.diseased = true;
  return HostOutcome::ok_void();
}

/// `o.AddToStoreBin()` / `o.RemoveFromStoreBin()` -- 1 site each, both in
/// `UNIT_DISAPPEAR.VS` around its two-second sleep.
///
/// **Accepted and dropped.** The bin is a global `std::set` of handles at
/// 0x009bdb24: 0x005b5750 inserts, 0x005b0e30 erases, and 0x005b0da0 prunes
/// members whose `vtbl+0x50` answers non-zero. Its one reader beyond that
/// upkeep is 0x005b11a0, a by-name search over the members which no script
/// entry point reaches, so nothing a script can observe depends on the bin's
/// contents. A set that is only ever written is not state; the condition that
/// would make it one is an entry point that reads it.
HostOutcome m_store_bin(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("StoreBin: no world");
  return HostOutcome::ok_void();
}

HostOutcome m_settlement(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("settlement: no world");
  // **Two registrations, one key.** `gbr.exe` declares `settlement/0` on
  // `Building` (0x004dcdd0, 671 sites) and on `GAIKA` (0x004219a0), and this
  // registry keys on `(kind, name, arity)` -- so one body answers both and
  // tells them apart by what it is handed, exactly as `Size/0` does for its
  // four receivers. A GAIKA is a bare integer (`sim/gaika.hpp`), which is
  // what makes the discrimination possible at all.
  //
  // `GAIKA::settlement`: when the node table (0x008c8e54) exists, the node at
  // `index * 0x24` (no bounds check, 0x0044e8e0), and its settlement pointer
  // at `+0x14` as a handle, or the invalid handle when it is null. Nothing is
  // printed. It is the inverse of `Settlement::GetGaika`: a settlement node
  // answers its settlement and a region node answers nothing.
  // `GetGAIKAStrat.vs` opens with `Set = gaika.settlement;` and then branches
  // on `Set.IsValid`, so "nothing" is a case it is written for.
  if (ctx.count() > 0 && ctx.arg(0).is_integer()) {
    const GaikaNode* node = world->gaika().find(gaika_of(ctx.arg(0)));
    if (node == nullptr || node->settlement == kNoObject) {
      return HostOutcome::ok_with(invalid_object());
    }
    return HostOutcome::ok_with(Value::object(ObjectRef{kTypeSettlement, node->settlement}));
  }
  // `Building::settlement`: the handle check -- a miss prints and answers the
  // invalid handle -- then the 16-bit settlement slot at `[bld+0x148]` as a
  // handle. The constructor leaves that slot 0xffff and the body does not test
  // for it, so a building outside every settlement reads an empty table slot
  // and faults; the invalid handle is the only answer to model that with. A
  // unit receiver is accepted here and cannot be reached from compiled code.
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || slot->settlement == kNoObject) return HostOutcome::ok_with(invalid_object());
  return HostOutcome::ok_with(Value::object(ObjectRef{kTypeSettlement, slot->settlement}));
}

/// A class `radius` for a distance, floored at zero: a radius below zero -- how
/// the original's class constructor can leave a number no class declared (the
/// placeholder at 0x0059c310) -- must not make a distance grow. Every one of
/// the 845 shipped classes resolves a radius of zero or more, so the floor is
/// never reached on retail data.
[[nodiscard]] std::int32_t reach_radius(const World& world, const WorldObject& slot) {
  const std::int32_t radius = class_int(world, slot, "radius");
  return radius > 0 ? radius : 0;
}

/// `a.DistTo(b)` -- the gap between two edges, not two centres.
///
/// `gbr.exe` registers the name twice, and both measure from the **stored**
/// position, `[obj+0x24]`/`[obj+0x28]` -- the `(-1, -1)` marker for a held
/// object, not where its holder stands:
///
///   * the point form (0x005aa350) pops the point, then the receiver; a miss
///     prints and answers **-1**. Otherwise the floor square root (0x0067c3d0)
///     of `dx² + dy²`, **less the receiver's class `radius`** (`[class+0x2dc]`);
///   * the object form (0x005aa3e0) checks the argument first ("Parameter #1",
///     -1), then the receiver (-1), and calls 0x005a77b0: the same distance
///     between the two stored positions **less both radii** -- the measure the
///     conversation reach test and a hero's skill reach are written in here.
///
/// Either can be negative, for a point inside the receiver or two objects that
/// overlap, and the original hands the negative back. The sums are 32-bit
/// there and 64-bit here, which agree for any two points on a map.
///
/// It was centre to centre here once, and that is what made a unit's `Goto`
/// to an object and its own `DistTo` disagree once `Goto` measured edges:
/// `SHAMAN_IDLE.VS` reads `if (.DistTo(.hero) > 250) { .Goto(.hero, 250, ...);
/// continue; }`, and a `Goto` that has arrived where `DistTo` says it has not
/// is a loop that never yields. It also measured from the holder and answered
/// 0 for a miss, until the host audit read both bodies.
HostOutcome m_dist_to(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("DistTo: no world");
  const auto miss = [] { return HostOutcome::ok_with(Value::integer(-1)); };

  Point target{};
  const WorldObject* other = nullptr;
  if (is_point(ctx.arg(1))) {
    target = unpack_point(ctx.arg(1));
  } else {
    other = object_of(*world, ctx.arg(1));
    if (other == nullptr) return miss();
    target = other->state.position;
  }
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return miss();
  std::int64_t reach = reach_radius(*world, *self);
  if (other != nullptr) reach += reach_radius(*world, *other);
  const Point here = self->state.position;
  const std::int64_t dx = static_cast<std::int64_t>(here.x) - target.x;
  const std::int64_t dy = static_cast<std::int64_t>(here.y) - target.y;
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(isqrt(dx * dx + dy * dy) - reach)));
}

/// `o.IsHeirOf("Class")` (0x005ad450) -- a test against the class *tree*.
///
/// The handle check (a miss prints and answers false), then the **whole
/// string as one key** in the class map at 0x009bdb00 (0x00409e40 and
/// 0x00431f10), compared exactly and case-sensitively (0x00409350): no comma
/// list, no trimming. No match answers false; otherwise it walks the parent
/// links (`[class+0xe4]`) up from the object's class (`[obj+0x3c]`) and answers
/// whether it reaches the found one. `ClassGraph::lookup` also falls back to
/// an `altid`; whether the original's map is keyed by those too was not
/// traced. This parsed a comma list until the host audit, so
/// `IsHeirOf("Fort,Unit")` was true of a soldier; no shipped literal has a
/// comma or a space, and the sites that pass a variable pass one name.
HostOutcome m_is_heir_of(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsHeirOf: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ClassGraph* graph = world->class_graph();
  if (slot == nullptr || graph == nullptr || !ctx.arg(1).is_string()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const ClassIndex base = graph->lookup(ctx.arg(1).as_string());
  if (base == kNoClass) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(Value::boolean(world->class_is_a(slot->id, base)));
}

// --------------------------------------------------------------------------
// members: what a class says an object is
// --------------------------------------------------------------------------

/// `b.IsCentralBuliding` -- the shipped spelling, transposition and all.
///
/// `gbr.exe` inlines the whole body into the VM thunk at `0x004dd2d0`: it takes
/// the receiver's class descriptor (`obj + 0x3c`) and tests the one slot at
/// `+0xa74`, which the class reader at `0x005a4670` fills from the
/// `is_central_building` property. Nothing else is consulted -- not the
/// settlement, not the anchor link, not ownership. So this is a question about
/// the *class*, and a building of a central-building class answers yes whether
/// or not it is currently the centre of anything.
///
/// The invalid-receiver result is **true**, not false: the thunk reports
/// "The function 'Building::IsCentralBuliding' called for an uninitialized or
/// invalid object." and then pushes 1 (`0x004dd2f3`-`0x004dd2ff`). That is
/// surprising enough to be worth reproducing rather than tidying -- the two
/// live call sites (`UNIT_ENTER_VERIFY.VS`, `CATAPULT_AUTOFIRE.VS`) both guard
/// with `IsValid` first, so the retail data never observes it, but a script
/// that does not guard must diverge the same way the original does.
HostOutcome m_is_central_building(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsCentralBuliding: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::boolean(true));
  return HostOutcome::ok_with(
      Value::boolean(class_flag(*world, *slot, "is_central_building")));
}


/// Five reads off the class descriptor, and two ancestry tests that carry
/// their own name.
///
/// Every one of these is a *class* question in `gbr.exe` too: the thunk
/// resolves the receiver, takes `obj + 0x3c`, and reads one slot the class
/// reader filled from one XML attribute. None of them consults the object.
///
///   * `Building::minlevel/0` (0x004ddaf0) -- descriptor `+0x6bc`, the
///     `minlevel` attribute.
///   * `Building::levelperitem/0` (0x004ddb50) -- `+0x6c0`, `levelperitem`.
///   * `Building::itemtypes/0` (0x004dda10) -- the `std::string` at `+0x6a0`,
///     `itemtype`. The entry point is plural and the attribute is singular,
///     because the value is a *list*: the class reader appends each repeated
///     `itemtype` after `", "` (see `ClassGraph`), and `RUIN_BEHAVIOR.VS` calls
///     this twice and runs `ParseStr` down it both times, once to count the
///     entries and once to walk to a random one.
///   * `Building::GetNumSentrySlots/0` (0x004ddca0) -- `+0x2f4`,
///     `num_sentry_slots`.
///   * `Building::GetSentryClassName/0` (0x004ddce0) -- the `std::string` at
///     `+0x2f8`, `sentry_class_name`. The walls and gates declare it
///     (`RSentry`, `GSentry1`, ...).
///
/// **A class that declares none of them up its whole chain** answers what the
/// class record was built with, since inheritance (0x0059c310) copies only
/// over the "absent" marker: the root branch there resets `num_sentry_slots`
/// to 0 and `sentry_class_name` to `""`, but leaves `minlevel` and
/// `levelperitem` at the marker `-15000000` (the record constructor 0x005a6820
/// writes it) and `itemtype` at `"**Invalid**"` -- the last INFERRED from the
/// constructor's run of `"**Invalid**"` strings, whose targets Ghidra does not
/// keep. Every shipped call asks a ruin, a wall or a gate, which declare them.
///   * `Obj::IsRam/0` and `Obj::IsPeasantAmbient/0` are 0x00540460 and
///     0x005403f0, which are both one `IsHeirOf` against a class the function
///     looks up once and caches in a file-static: `"RamUnit"` and
///     `"PeasantAmbient"`. Ancestry, not name equality, so a subclass matches.
///
/// **The invalid-receiver answers are the type's zero here, not `IsCentralBuliding`'s
/// surprising `true`.** `minlevel`, `levelperitem` and `itemtypes` report
/// through 0x00686eb0 -- a bare `ret` in retail, and all three under the name
/// `Building::Starvation` -- and push 0 or the empty string. **The two sentry
/// readers do not test the receiver at all**: they read `+0x3c` of a null
/// object and fault. 0 and the empty string stand in for that here.
HostOutcome m_building_minlevel(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("minlevel: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::integer(slot == nullptr ? 0 : class_int_or_marker(*world, *slot, "minlevel")));
}

HostOutcome m_building_level_per_item(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("levelperitem: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::integer(slot == nullptr ? 0 : class_int_or_marker(*world, *slot, "levelperitem")));
}

HostOutcome m_building_num_sentry_slots(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetNumSentrySlots: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::integer(slot == nullptr ? 0 : class_int(*world, *slot, "num_sentry_slots")));
}

/// One class property as a string, walking up the class tree; `absent` when
/// no class up the chain declares it, empty for a receiver that resolves to
/// nothing.
[[nodiscard]] HostOutcome class_text(CallContext& ctx, std::string_view key,
                                     std::string_view absent) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::ok_with(Value::string(std::string()));
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ClassGraph* graph = world->class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) {
    return HostOutcome::ok_with(Value::string(std::string()));
  }
  if (!class_declares(*graph, slot->class_index, key)) {
    return HostOutcome::ok_with(Value::string(std::string(absent)));
  }
  return HostOutcome::ok_with(
      Value::string(std::string(graph->property(slot->class_index, key))));
}

HostOutcome m_building_item_types(CallContext& ctx) {
  return class_text(ctx, "itemtype", "**Invalid**");
}

HostOutcome m_building_sentry_class_name(CallContext& ctx) {
  return class_text(ctx, "sentry_class_name", "");
}

HostOutcome m_is_ram(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsRam: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::boolean(slot != nullptr && class_is(*world, *slot, "RamUnit")));
}

HostOutcome m_is_peasant_ambient(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsPeasantAmbient: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::boolean(slot != nullptr && class_is(*world, *slot, "PeasantAmbient")));
}

/// `o.IsRanged` -- does the class name a projectile?
///
/// `gbr.exe` 0x005ad620 hands the receiver to 0x0053f5b0, which is two tests on
/// one `std::string` field of the class descriptor at `+0x960`: its size (read
/// at `+0x974`, the string's own length slot) must be non-zero, **and** its
/// text must not be `"**Invalid**"` (0x007ae000, compared with 11 characters).
///
/// **The string is a class name, and the attribute is `projectile_class`.**
/// 0x005a6c9d initialises a run of `std::string`s at `+0x928`, `+0x944`,
/// `+0x960`, `+0x97c`, `+0x998` (stride 0x1c) to the `"**Invalid**"`
/// sentinel; the class reader's `projectile_class` arm (the compare at
/// 0x005a421a, the store into `+0x960` at 0x005a4254) fills it; 0x0059c7c8
/// inherits it from the parent class while it is still the sentinel; and
/// 0x005b1d20 looks it up **in the class registry** and reads a range-like
/// number off the class it finds. This used to be recorded as an inference,
/// because the literal sits inside `_projectile_class` (0x007c8650) and a
/// search for its address found nothing; the arm pushes 0x007c86c0, a
/// separate copy. `building_projectile_class` is a different slot.
///
/// This is not a range comparison and not a weapon-type test: a unit
/// is ranged when it has something to throw.
///
/// **`"**Invalid**"` is the original's uninitialised-string default, not an
/// authored value**, and no shipped class declares it -- so the second test is
/// unfalsifiable against retail content and is written anyway, because it is
/// what the executable does and because a class that did declare it would
/// answer false there and true here.
///
/// An invalid receiver reports and answers false. The two shipped sites are
/// `UNIT_STAND_POSITION.VS`, which is the whole of that script's blocker, and
/// `GDRUID_STAND_POSITION.VS`.
HostOutcome m_is_ranged(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsRanged: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ClassGraph* graph = world->class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const std::string_view projectile = graph->property(slot->class_index, "projectile_class");
  return HostOutcome::ok_with(
      Value::boolean(!projectile.empty() && projectile != "**Invalid**"));
}

/// `o.IsWaterUnit` -- the `water_unit` class property, and only that.
///
/// `gbr.exe` `0x005ac6b0`: class descriptor `+0xb30`, which the class reader
/// fills from `water_unit`. An invalid receiver reports and answers false.
HostOutcome m_is_water_unit(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsWaterUnit: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(Value::boolean(class_flag(*world, *slot, "water_unit")));
}

/// `o.IsPeaceful` -- two class-tree tests, and nothing else.
///
/// `gbr.exe` 0x005ad740 resolves the receiver and hands it to 0x005402f0,
/// which is
///
///     IsHeirOf("Peaceful") || IsHeirOf("Animal")
///
/// -- two class names looked up once each into a file-scope cache and then
/// walked up the class chain by the same parent-pointer loop `IsHeirOf`
/// itself uses (0x0059a220, `class + 0xe4` until it matches or runs out).
/// There is no ownership test, no flag bit, no unit/building distinction: a
/// building of a `Peaceful`-descended class would answer yes, and the entry
/// point is registered on `Obj`, not on `Unit`, so nothing stops one being
/// asked.
///
/// **The two names are class *identifiers*, not C++ classes**, and the
/// distinction bites here exactly as it bit `Settlement::BestBarrack`: the
/// shipped tree puts `cpp_class="CVXUnit"` on `Peaceful`, on `Animal` and on
/// most of the army, so a C++-class test would answer yes for everything.
/// `Peaceful` roots peasants, wagons, ambient peasants, the shaman ghost and
/// (through `BaseMage`) the druids; `Animal` roots the crows, deer, eagles,
/// fish and hens. Note that `Animal`'s own parent is `BaseAnimal` -- a class
/// descended from `BaseAnimal` but not from `Animal` answers **no**, and the
/// second test is written against `Animal` because that is the name the
/// executable holds.
///
/// An invalid receiver reports *"The function 'Obj::IsPeaceful' called for an
/// uninitialized or invalid object."* and answers **false**.
///
/// The one shipped call site is `ESH_RESEARCHTRAINING.VS:335`, counting the
/// soldiers standing around a settlement's central building to decide whether
/// to research military training; peasants and deer must not be counted as an
/// army. It is the sole blocker of that script's 93 call sites.
HostOutcome m_is_peaceful(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsPeaceful: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const ClassGraph* graph = world->class_graph();
  if (graph == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  for (const std::string_view name : {std::string_view("Peaceful"), std::string_view("Animal")}) {
    if (world->matches_filter(*slot, ClassFilter::parse(name, graph))) {
      return HostOutcome::ok_with(Value::boolean(true));
    }
  }
  return HostOutcome::ok_with(Value::boolean(false));
}

/// `Catapult::IsBuilt` and `Catapult::SetBuilt` -- a siege engine's assembly.
///
/// `gbr.exe` registers both on the `Catapult` type (0x26): `IsBuilt`
/// (name 0x007bb704, registered 0x004e3e57, body **0x004e2dc0**) returns
/// `bool` and `SetBuilt` (name 0x007bb6f0, body **0x004e2e20**) returns void,
/// and between them they are the whole of the flag. `IsBuilt` is nine
/// instructions: resolve the receiver, `mov edx,[obj+0x208]`, `test edx,edx`,
/// `setne`. `SetBuilt` writes the literal 1 and then does four more things,
/// three of which this engine cannot yet reproduce and one of which it can --
/// see below.
///
/// **The two disagree about a bad receiver, and both are transcribed.**
/// `IsBuilt` checks both halves of the handle resolution
/// (`test ecx,ecx / je` **and** `add ecx,edx / jne`, 0x004e2ddb-0x004e2de1),
/// prints *"The function 'Catapult::IsBuilt' called for an uninitialized or
/// invalid object."* and answers **false**. `SetBuilt` makes the same check,
/// prints its own message (0x007bb138) and returns without writing. Neither
/// traps -- and the print itself is inert, because 0x00686eb0 is a bare `ret`
/// in the retail build.
///
/// **A catapult is born unbuilt**: the `CVXCatapult` constructor zeroes
/// `+0x208` at 0x004e1ede, beside `+0x20c` and `+0x210..+0x21c`. So this is
/// the one flag in `ObjectFlags` whose starting value is a fact rather than a
/// convention.
///
/// What `SetBuilt` does beyond the flag, and what is left out:
///
///   * `[[obj+0x40]+0x80] = 1` -- the sprite's clock set running again, the
///     field `SetBuildFrame` stops (0x00629500 writes it 0); with the next
///     item, the one presentation half that lands here.
///   * `vtbl[0x48](0x2000000)` -- the flag-setting virtual `SpawnGroup`
///     already uses to clear bit 27 on a copy. Bit 25 is unexplained in
///     `docs/engine/state-vector.md`, so setting it here would be inventing a
///     meaning; left out and named rather than silently dropped.
///   * `[obj+0x20c] = -1` -- the build frame `SetBuildFrame` writes, cleared:
///     `WorldObject::build_frame`, presentation, so a built engine is drawn
///     by its animation again rather than frozen on a construction stage.
///   * `0x0053d730` and `0x0053e860(1, 0)` -- animation, which the object model
///     does not drive.
///
/// So what lands is the flag and the two miss paths, which is exactly what the
/// three shipped call sites read: `GUARD.VS:79`
/// (`olAttackers[j].AsCatapult.IsBuilt`) and `CATAPULT_IDLE.VS` lines 16 and
/// 35. The first of those is the last unimplemented name in `GUARD.VS`.
/// `Obj::SetCmdEnable(bool)` -- 1 site, and with `DoCarryNothing` it finishes
/// the 210-site `SETTLEMENT_BEHAVIOR_AMBIENT.VS`: a villager sent home is
/// `AddCommand(false, "dismiss"); SetCmdEnable(false)`.
///
/// `0x005ac710` is registered `[0, 2, 20, 7]`. An invalid receiver prints
/// `The function 'Obj::SetCmdEnable' called for an uninitialized or invalid
/// object.`; a **dead** one -- `vtbl + 0x50`, the slot `Obj::IsAlive` negates
/// -- prints `The function 'Obj::SetCmdEnable' called for a dead object`, and
/// both return without writing. Otherwise the argument, as 0 or 1, is written
/// whole to `[obj+0x9c]`. `ObjectFlags::commands_disabled` is that word with
/// the polarity turned round, and `sim/orders.hpp` says who reads it.
HostOutcome m_set_cmd_enable(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetCmdEnable: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  WorldObject* slot = found == nullptr ? nullptr : world->find(found->id);
  if (slot == nullptr || slot->state.health <= 0) return HostOutcome::ok_void();
  slot->state.flags.commands_disabled = !(ctx.count() > 1 && ctx.arg(1).truthy_scalar());
  return HostOutcome::ok_void();
}

/// `Unit::StartTraining()` / `Unit::StopTraining()` -- 1 site each:
/// `UNIT_TRAIN.VS`'s first line, and the whole of `UNIT_TRAIN_ONFINISH.VS`.
///
/// `0x005d5de0` and `0x005d5e30` resolve the receiver -- an invalid one prints
/// and returns -- and call the object's `vtbl+0x44` / `vtbl+0x48` with
/// `0x10000000`. Those two slots are the **`SyncFlags`** set and clear --
/// `Unit::SetParty` drives bit 19 of `[obj+0x2c]` through the same pair, and
/// the food tick drives bit 29 -- so the bit is bit 28 of the sync word,
/// `ObjectFlags::training`, whose one reader is `BestTrainingTarget`'s
/// predicate: a sparring partner is a unit that is itself training.
///
/// **This used to write `half_damage`**, on the reading that the pair drove
/// the second word at `[obj+0x194]`, where the damage formula's bit 28 lives.
/// It does not: that bit has one writer, the cover of mercy, and a training
/// unit takes full damage in the original. The correction is recorded in
/// `docs/plan.html`.
template <bool kStart>
HostOutcome m_set_training(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("StartTraining: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  WorldObject* slot = found == nullptr ? nullptr : world->find(found->id);
  if (slot == nullptr) return HostOutcome::ok_void();
  slot->state.flags.training = kStart;
  return HostOutcome::ok_void();
}

/// `Unit::BestTrainingTarget()` -- 2 sites, both `UNIT_TRAIN.VS`: the partner
/// a training unit walks up to and swings at, or the enemy that ends the
/// lesson (`if (.IsEnemy(u)) { UserNotification("cannot train", ...); return; }`).
///
/// `0x005d5bf0` (registered under the internal misspelling
/// `GetBestTrainigTarget`) resolves the receiver -- an invalid one prints and
/// answers the invalid handle, and so does one whose position is the held
/// sentinel -- then runs the grid sweep at 0x005d4cd0 over the unit's own
/// **sight** (`[unit+0xd0]`) with the predicate at 0x005d3cb0
/// (`docs/formats/target-score.md`):
///
///   * an **enemy** (0x005a78e0: the receiver's owner's row for the
///     candidate's owner, bit 0 clear) whose `damage` (`[cand+0xdc]`) is not
///     zero is taken at once and ends the sweep -- that is how the enemy
///     reaches the script;
///   * otherwise the candidate must be a unit, not the receiver, carrying
///     `SyncFlags` bit 28 -- `ObjectFlags::training`, the bit `StartTraining`
///     sets -- and is scored from its **surface** distance (0x005a77b0, both
///     class radii off): `40 * danger + distance`, plus 100 for a hero (bit
///     24), plus 40 inside the receiver's `min_range`, plus 80 for a unit
///     with a path (bit 17); a candidate within `range` whose score is under
///     50 is taken at once, and otherwise gets 100 off. The lowest score
///     wins, and **a later candidate takes a tie** (`<=`).
///
/// The sweep visits **units only**: 0x005d3c50 seeds it with the `kSyncUnit`
/// mask, so neither clause ever sees a building; a hidden unit only when its
/// owner shares its view with the receiver's player (`target_sweep`). When
/// the answer is a unit, 0x005d2850 adds one to its `danger`, and every
/// `danger` the scores read decays as it is read (`CombatSystem::danger`).
/// The sweep (0x005d4cd0) walks the world's grid in its order (`SweepOrder::grid`):
/// that order decides the tie, and which enemy ends the sweep first.
HostOutcome m_best_training_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("BestTrainingTarget: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(invalid_object());
  const Point at = world->resolve_position(self->id);
  if (at == kHeldPosition) return HostOutcome::ok_with(invalid_object());
  const std::int32_t max_range = class_int(*world, *self, "range");
  const std::int32_t min_range = class_int(*world, *self, "min_range");
  const std::int32_t my_radius = class_int(*world, *self, "radius");
  const PlayerTable& players = world->players();
  const PlayerId owner = self->state.owner;
  CombatSystem* combat = combat_system_of(*world);
  const GameTime now = world->time();

  std::vector<ObjectId> around;
  target_sweep(*world, at, self->sight, owner, around);
  ObjectId best = kNoObject;
  std::int64_t best_score = 0;
  for (const ObjectId id : around) {
    const WorldObject* cand = world->find(id);
    if (cand == nullptr) continue;
    // The sweep's mask (0x005d3c50 seeds it with `0x400000`, `kSyncUnit`):
    // only units are visited at all, so an armed enemy *building* -- a tower
    // -- neither ends the lesson nor is a partner.
    if (!cand->state.flags.is_unit) continue;
    if (owner != kNoPlayer && cand->state.owner != kNoPlayer &&
        players.is_enemy(owner, cand->state.owner) && class_int(*world, *cand, "damage") != 0) {
      best = id;
      break;
    }
    if (id == self->id || !cand->state.flags.training) continue;
    const Point there = world->resolve_position(id);
    const std::int64_t dx = there.x - at.x;
    const std::int64_t dy = there.y - at.y;
    const std::int64_t distance =
        isqrt(dx * dx + dy * dy) - my_radius - class_int(*world, *cand, "radius");
    const std::int32_t danger = combat != nullptr ? combat->danger(id, now) : 0;
    std::int64_t score = distance + static_cast<std::int64_t>(danger) * 40;
    if (cand->state.flags.is_hero) score += 100;
    if (distance < min_range) score += 40;
    if (cand->state.flags.has_active_path) score += 80;
    if (distance <= max_range) {
      if (score < 50) {
        best = id;
        break;
      }
      score -= 100;
    }
    if (best == kNoObject || score <= best_score) {
      best = id;
      best_score = score;
    }
  }
  if (best != kNoObject && combat != nullptr) {
    if (const WorldObject* answer = world->find(best);
        answer != nullptr && answer->state.flags.is_unit) {
      combat->raise_danger(best, 1, now);
    }
  }
  return HostOutcome::ok_with(object_value(best));
}

/// `Unit::TrainAttack(partner)` -- 1 site, `UNIT_TRAIN.VS`, three times over
/// after `GotoAttack(u, ...)` has brought the unit to its partner:
/// `if (!.TrainAttack(u)) break;`.
///
/// `0x005d5d10` is registered suspending. It answers **false** without a word
/// for a receiver whose position is the held sentinel, prints and answers
/// false for a partner that does not resolve, and answers false for a
/// receiver with one point of health or less (`[unit+0xc0]`). Otherwise it
/// puts the receiver into action state 2 -- the attack -- through 0x005a7680
/// and answers the interpreter's 3, *suspend until this object's action
/// state clears*, which is one swing of the attack animation; the partner is
/// validated and not stored, because the swing goes to whatever
/// `GotoAttack` made the target. So the call is one attack's worth of wait,
/// and the strike itself is the combat system's, here as there.
///
/// Here: the same three refusals, then a `retry` for one cycle of the
/// entity's `attack` state (`attack_animation_ms`, what `AttackWait` measures),
/// and **true** on the re-entry.
///
/// **A unit with no attack animation to wait on still suspends**, until the
/// next pass. Past the refusals 0x005d5d10 has no path that answers without
/// the 3, so the call always gives up the slice. It used to answer true at
/// once here, and `UNIT_TRAIN.VS`'s loop -- `GotoAttack` already in reach
/// answers at once in both engines, then three `TrainAttack`s -- then never
/// yielded: 236 runaways on Balcans in 4,000 turns. The partners there were
/// Gaul sentries (`GSentry1`), sent to train by `TOWNHALL_AUTOTRAIN.VS`, whose
/// entity (`Units/GArcher/GSentry1.ent.xml`) declares only `idle` and `fight`
/// states and so has no `attack` cycle to measure. **Labelled:** how long
/// action state 2 lasts with no animation behind it was not read; one pass is
/// the shortest wait the 3 can mean, and it is what this answers.
HostOutcome m_train_attack(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("TrainAttack: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  if (world->resolve_position(self->id) == kHeldPosition) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  if (ctx.count() < 2 || object_of(*world, ctx.arg(1)) == nullptr) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  if (self->state.health <= 1) return HostOutcome::ok_with(Value::boolean(false));
  const std::int32_t swing = attack_animation_ms(*self);
  if (ctx.first_call) {
    HostOutcome outcome;
    outcome.status = script::HostStatus::retry;
    outcome.suspend_for = swing > 0 ? swing : 0;
    return outcome;
  }
  return HostOutcome::ok_with(Value::boolean(true));
}

/// `Obj::GetStaminaDecTime()` / `Unit::GetStaminaDecTime()` -- 2 sites, both
/// `SUMMONING_BEHAVIOR.VS`, which sleeps that long between taking one point
/// of stamina off a summoned thing and puts it down at zero.
///
/// `0x005ab580` reads `[obj+0xec]` and `0x005d8080` reads `[unit+0x17c]`, the
/// same value at the two classes' offsets: the class's `stamina_dec_time`,
/// which every `SummoningObj` heir declares (`Mist` 1200, `CoverOfMercy`
/// 2000). A negative slot answers **0**, and the script reads 0 as *put it
/// down now*. An invalid receiver reads through a null in the original; here
/// it answers 0, the same exit.
HostOutcome m_get_stamina_dec_time(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetStaminaDecTime: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const std::int32_t time = slot == nullptr ? 0 : class_int(*world, *slot, "stamina_dec_time");
  return HostOutcome::ok_with(Value::integer(time < 0 ? 0 : time));
}

/// `Catapult::ClearTowerTarget()` -- 1 site, and it is the whole of
/// `CATAPULT_STOP.VS`: `me.AsCatapult.ClearTowerTarget();` under the comment
/// *make the fake towers shut up without forgetting the target (it will
/// remain in m_hTarget of the catapult itself)*.
///
/// `0x004e37f0` resolves the receiver -- an invalid one prints and pushes a
/// zero -- then, while the slot at `[cat+0x22c]` is non-null, resolves the
/// settlement handle at `[cat+0x148]`, the settlement every placed siege
/// engine anchors, and walks its member deque at `+0x68` (the roll
/// `UpgradeBestBarrack` walks), writing `0xffff` to `[bld+0x1fc]` on every
/// member that is an heir of `CatapultTower` (0x007bb478). `+0x1fc` is
/// `ObjectState::ui_target`, the slot `Building::GetUITarget` reads and
/// `TOWER_ATTACK.VS` writes with `Bld.SetUITarget(target)`: the fake towers
/// are the engine's guns, and taking their target away is what stops them.
///
/// **The gate is `has_towers`.** `[cat+0x22c]` is the engine's `HasTowers`
/// (the persist stream names it, 0x004e1c90), raised when the tower
/// placement put a `CatapultTower` down -- not a turn in progress, as this
/// note once read it. An engine with no towers has nothing to silence. The
/// walk is `catapult_clear_tower_target` (`sim/siege_fire.cpp`).
HostOutcome m_clear_tower_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClearTowerTarget: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  catapult_clear_tower_target(*world, slot->id);
  return HostOutcome::ok_void();
}

HostOutcome m_is_built(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsBuilt: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && slot->state.flags.built));
}

/// `Catapult::AttackWait()` -- 3 sites, one per catapult attack script, and
/// with `RotateTo` it finishes all three: `CATAPULT_ATTACK.VS`,
/// `CATAPULT_ATTACK_GROUND.VS` and `CATAPULT_AUTOFIRE.VS` each open a shot
/// with `ttw = .AttackWait; if (ttw > 0) Sleep(ttw);`.
///
/// `0x004e32b0` is registered `[1, 1, 38]`: an int, from a `Catapult`. An
/// invalid receiver prints `The function 'Catapult::AttackWait' called for an
/// uninitialized or invalid object.` and answers 0. The live half is
/// 0x004e2030: the crew is the unit count of the holder the catapult carries
/// (`vtbl + 0xe4`, then `[holder+0x40]`) -- the holder of the settlement the
/// engine allocates for every catapult, which `GetHolderSett` on a crewman
/// already walks -- and **no crew answers -1**, which the caller turns into
/// 0. Otherwise the shot interval is `600000 / (CatapultBaseFireRate +
/// (crew - 1) * CatapultAddFireRate)` milliseconds -- ten minutes over the
/// shots-per-ten-minutes the two `CONST.INI` lines declare (`0x927c0`, the
/// two cached constants at 0x81b250 and 0x81b254) -- less the length of the
/// entity's attack animation, read through the class's entity resource
/// (0x0059a780) at the record `[+0x147c]` whose `+0x4c` is a duration: the
/// `attack` state's animation, one second on every shipped catapult. So a
/// one-man Roman catapult waits 14,000 ms between the start of one shot and
/// the start of the next, and a full crew of ten waits 1,000 less than
/// `600000 / 292`.
///
/// **One term cannot arise here.** When the catapult's action state is
/// between 4 and 7 the original (0x004e3326..0x004e3365) adds the time left
/// on it to the answer. `Catapult::Attack` suspends its script to the end of
/// the throw and `RotateTo` turns at once, so none of the three callers can
/// ask with a state in progress. A negative result is 0 (0x004e330c).
/// One cycle of the entity's `attack` state, in game-time milliseconds, or 0
/// for an object with no entity or no such state. What `AttackWait` takes off
/// the shot interval, and how long `TrainAttack`'s swing lasts.
std::int32_t attack_animation_ms(const WorldObject& slot) {
  if (slot.object == nullptr || slot.object->entity == nullptr) return 0;
  const Entity& entity = *slot.object->entity;
  for (const EntityState& state : entity.states()) {
    if (state.name != "attack" || !state.has_anim()) continue;
    if (const EntityAnim* anim = entity.anim(state.anim_idx); anim != nullptr) {
      const AnimTimeline timeline = entity.timeline(*anim);
      if (timeline.valid()) return timeline.cycle();
    }
    break;
  }
  return 0;
}

HostOutcome m_attack_wait(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AttackWait: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  const EconomySystem* economy = economy_of(*world);
  // A catapult is its settlement's anchor, which the store files among the
  // settlement's buildings, so the object lookup answers for it.
  const Settlement* set = economy == nullptr ? nullptr : economy->settlements().for_object(slot->id);
  const std::int32_t crew = set == nullptr ? 0 : set->holder.count();
  if (crew <= 0) return HostOutcome::ok_with(Value::integer(0));

  std::int32_t base = 40;
  std::int32_t add = 28;
  if (const CombatSystem* combat = combat_system_of(*world); combat != nullptr) {
    base = combat->constants().catapult_base_fire_rate;
    add = combat->constants().catapult_add_fire_rate;
  }
  const std::int32_t rate = base + (crew - 1) * add;
  if (rate <= 0) return HostOutcome::ok_with(Value::integer(0));
  std::int32_t wait = 600000 / rate;

  // The attack animation's `duration` attribute: the record at
  // `[entity+0x147c]` is `<anim idx="5">` (the table at `+0x146c` is indexed
  // by `idx - 1`) and its `+0x4c` is `duration` (the writer 0x005fd720 prints
  // the fields in that order). Not the `attack` *state*, which only one
  // shipped catapult entity names.
  wait -= catapult_throw_ms(*world, slot->id);
  return HostOutcome::ok_with(Value::integer(wait < 0 ? 0 : wait));
}

/// `Catapult::RotateTo(pt)` -- 3 sites, the other half of the same three
/// scripts: `if (bRotated == false) { .RotateTo(ptNew); bRotated = true; }`
/// before the first shot at a target.
///
/// `0x004e2c50` is registered as suspending over a `Catapult` and a point.
/// An invalid receiver prints `The function 'Catapult::RotateTo' called for an
/// uninitialized or invalid object.` and finishes. So does a point equal to
/// the catapult's own position, and a catapult already turning (`[obj+0x22c]`
/// set). Otherwise the target point is stored at `[obj+0x218]`, the request
/// flag at `[obj+0x220]` is raised, the action state is set to 7 -- turning
/// -- through 0x005a7680, and the call answers **3**: the interpreter's
/// "suspend until this object's action state clears", which the catapult's
/// own update ends when its facing reaches the point.
///
/// This engine has no turn rate on a catapult: `MovementSystem::face` writes
/// the facing at once, which is what `Face` does for every other object here
/// and what the three scripts see once the wait is over. So the turn is
/// instant and the call finishes, and the time a real catapult takes to
/// swing round is the labelled divergence -- a shot fired a moment earlier
/// than the original would fire it, with nothing else different.
HostOutcome m_rotate_to(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RotateTo: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || ctx.count() < 2 || !is_point(ctx.arg(1))) return HostOutcome::ok_void();
  const Point towards = unpack_point(ctx.arg(1));
  // Belt and braces, kept knowingly: `MovementSystem::face` returns early on a
  // zero delta of its own accord, so this test changes no behaviour and a
  // fault that drops it survives the suite. The original writes the test
  // here (0x004e2ca7), so this does too.
  if (towards == world->resolve_position(slot->id)) return HostOutcome::ok_void();
  // And an engine with towers does not turn: the same `[cat+0x22c]` gate
  // `Catapult::Attack` has, read in the same place (0x004e2c50).
  if (slot->state.flags.has_towers) return HostOutcome::ok_void();
  if (MovementSystem* movement = movement_system(*world); movement != nullptr) {
    movement->face(*world, slot->id, towards);
  }
  return HostOutcome::ok_void();
}

HostOutcome m_set_built(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetBuilt: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  WorldObject* slot = found == nullptr ? nullptr : world->find(found->id);
  if (slot == nullptr) return HostOutcome::ok_void();
  slot->state.flags.built = true;
  slot->build_frame = -1;
  return HostOutcome::ok_void();
}

/// `u.GetParty` -- is this unit part of the hero's travelling party?
///
/// `gbr.exe` `0x005d7b10` is four instructions of body:
/// `mov ecx,[obj+0x2c] / shr ecx,0x13 / and cl,1 / mov [out],cl`. So it is
/// `SyncFlags` bit 19 and nothing else -- `sim/world.hpp`'s `kSyncParty`
/// carries the whole chain from there to `Unit::SetParty`, the party singleton
/// at `[0x00996ff4]`, the free entry point `Party()` and the engine's own
/// `"Party unit of class %s"` diagnostic.
///
/// **Nothing in this engine sets the bit yet**, because `SetParty` is not
/// written, so this answers false for every object a map loads. That is not a
/// stub: it is what the original answers too, since `<scriptobj flags>` bits
/// 16..21 are clear on all 27,070 shipped map objects and the only writers are
/// `Unit::SetParty` and `Query::SetParty`.
///
/// An unresolvable receiver reports and pushes 0 in the original
/// (`0x005d7b33`-`0x005d7b4d`, message at `0x007d15c0`), so false here.
HostOutcome m_get_party(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetParty: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && slot->state.flags.in_party));
}

/// `ship.UnitsMax` -- how many units this ship can carry.
///
/// Refused once before for lack of evidence. The evidence is a four-link chain
/// in `gbr.exe`, and every link is a single instruction:
///
///   1. The class-property reader matches `"max_units_to_board"` at
///      `0x005a4fb1` and stores the parsed integer at class descriptor
///      `+0xb38` (`0x005a4ffa`). Inheritance is the usual "unset means take the
///      parent's": `0x0059d38f` copies `+0xb38` down whenever the child still
///      holds the sentinel `0xff1b1e40`.
///   2. `CVXShip::Init` reads that slot (`0x005c73a5`) and passes it as the
///      capacity argument to the holder constructor `0x00531de0`
///      (`0x005c73bc`), then stores the holder's handle at `ship + 0x1dc`
///      (`0x005c73ec`).
///   3. The constructor stores that argument at `holder + 0x10`
///      (`0x00531f72`) and zeroes the occupancy at `holder + 0x14`.
///   4. `Ship::UnitsMax` (`0x005c6e40`, registered at `0x005c8c16` as
///      `int UnitsMax(Ship)`) resolves `ship + 0x1dc` and returns
///      `holder + 0x10` (`0x005c6e83`-`0x005c6e94`). `Ship::UnitsCount`
///      (`0x005c6de0`) returns `holder + 0x14` from the same holder.
///
/// `Settlement::max_units` (`0x005c1de0`) reads `holder + 0x10` of the
/// settlement's holder, so **this is the same field under a second name** --
/// the two differ only in which class property seeds it (`max_units` at
/// descriptor `+0xaa4` for a settlement, `max_units_to_board` for a ship).
///
/// This reads the class rather than a holder because the capacity is written
/// once, at construction, from the class, and the ship path -- unlike the
/// settlement path at `0x005c410f`, which honours a `-1` override from its
/// caller -- has no override at all. Nothing in the original can change it
/// afterwards.
///
/// In the shipped data exactly one class declares the property:
/// `ShipBattle` (`DATA\CLASSES\SHIP BATTLE.SC.XML`, `max_units_to_board="60"`),
/// and the other three `CVXShip` classes -- `ShipEgypt`, `ShipL`, `ShipRome` --
/// inherit from it. So every retail ship answers 60.
///
/// An unresolvable receiver reports and pushes 0 (`0x005c6e63`-`0x005c6e82`,
/// message `"The function 'UnitsMax' called for an uninitialized or invalid
/// object."` at `0x007cf458`), which is what all three call sites reach through
/// `.AsShip` on a non-ship. A receiver that resolves but is not a ship is a
/// different thing: the original would read `+0x1dc` of an object that has no
/// such field, so there is no behaviour to reproduce and this refuses instead.
HostOutcome m_units_max(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("UnitsMax: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  if (slot->object == nullptr || !slot->object->is_a(NativeClass::ship)) {
    return HostOutcome::failed("UnitsMax: receiver is not a Ship");
  }
  return HostOutcome::ok_with(Value::integer(class_int(*world, *slot, "max_units_to_board")));
}

/// `me.IsValidCaptureTarget(it)` -- may `me` capture `it` at all?
///
/// Ownership is **not** part of it. `UNIT_CAPTURE_VERIFY.VS` reads
/// `if (me.IsValidCaptureTarget(bld)) return me.IsEnemy(bld);` -- this entry
/// point answers "is that a capturable kind of thing", and the caller decides
/// whose it is. `can_be_captured` is not consulted either.
///
/// From `gbr.exe`'s thunk at `0x005aaea0`, in its order:
///
/// - parameter #1 (the target) must resolve; then the receiver must resolve.
///   Both report and answer false otherwise, which is why this returns false
///   for a null receiver where `IsCentralBuliding` returns true.
/// - `0x00540130`: the *receiver* must be an heir of `Military`.
/// - the target must be a building (flag `0x800000` at `obj + 0x2c`, the same
///   bit `AsBuilding` tests) and its settlement handle at `+0x148` must resolve.
///   The original dereferences both unconditionally; refusing is the only
///   reading of that which is not a crash.
/// - the target must be an heir of `Building` and of none of `Tower`, `Wall`,
///   `Gate`, `Catapult`.
HostOutcome m_is_valid_capture_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsValidCaptureTarget: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  const WorldObject* target = object_of(*world, ctx.arg(1));
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  if (self == nullptr || target == nullptr) return no;
  if (!class_is(*world, *self, "Military")) return no;
  if (!target->state.flags.is_building) return no;
  if (target->settlement == kNoObject || world->find(target->settlement) == nullptr) return no;
  if (!class_is(*world, *target, "Building")) return no;
  for (const std::string_view excluded : {"Tower", "Wall", "Gate", "Catapult"}) {
    if (class_is(*world, *target, excluded)) return no;
  }
  return HostOutcome::ok_with(Value::boolean(true));
}

// --------------------------------------------------------------------------
// members: downcasts
// --------------------------------------------------------------------------

/// Every `AsXxx` answers a handle to the same object when it is of that native
/// class or derives from it, and the invalid handle otherwise -- for a failed
/// cast and for a receiver that does not resolve alike, and silently. There is
/// one runtime representation for all handle types, so a downcast is a test,
/// not a conversion. In `gbr.exe` the test is one of two kinds:
///
///   * **a sync-flag bit** at `[obj+0x2c]` for three of them: `AsUnit`
///     (0x005aa580) tests 0x400000, which the `CVXUnit` constructor (0x005d3170)
///     sets; `AsBuilding`/`AsTower` (0x005aa5e0) test 0x800000, set by the
///     `CVXBuilding` constructor (0x004e00d0); `AsHero` (0x005aa640) tests
///     0x1000000. Every subclass runs its base constructor, so each bit covers
///     exactly that C++ family;
///   * **the object's virtual cast** with a C++ class-name pointer for the
///     rest -- `AsShip` 0x005aa9b0, `AsDruid` 0x005aa6a0, `AsCatapult`
///     0x005aa780, `AsWagon` 0x005aa7f0, `AsGate` 0x00529980, `AsFlying`
///     0x005aaa20, `AsItemHolder` 0x005aa860. Each class's cast compares the
///     pointer with its own name and otherwise defers to its parent's: ship,
///     druid, wagon, flying unit, ghost and hero to the unit's (0x005d27d0);
///     catapult, gate, town hall, outpost, barrack, tavern and teleport to the
///     building's (0x004db2a0); item holder straight to the script object's.
///
/// So a druid is not a hero, catapults and gates are buildings, and nothing
/// derives from ship, gate, flying unit or item holder -- which is the tree
/// `NativeObject::is_a` walks, for all ten. A hit pushes the object's own
/// 16-bit handle `[obj+8]` with offset 0. (One edge outside these ten
/// differs: the outpost's cast goes straight to the building's, where this
/// engine's native tree puts the outpost under the town hall.)
///
/// `AsTeleport` (0x005cdb50), `AsSacrifice` (0x005aa940) and `AsBarrack`
/// (0x005aa8d0) are the virtual-cast shape too; none of the three has a native
/// child (`CVXTeleport` and `CVXBarrack` sit under `CVXBuilding`,
/// `CVXSacrifice` under `CVXScriptObj`), so each admits exactly its own
/// `cpp_class`.
template <NativeClass kClass>
HostOutcome m_as(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("downcast: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || slot->object == nullptr) return HostOutcome::ok_with(invalid_object());
  if (!slot->object->is_a(kClass)) return HostOutcome::ok_with(invalid_object());
  return HostOutcome::ok_with(object_value(slot->id));
}

// --------------------------------------------------------------------------
// members: the small predicates
// --------------------------------------------------------------------------
//
// Four names that are one field or one bit each, and every one of them turned
// out to be registered on a different receiver from the one the corpus census
// suggested. The census reads a name off a call site and attributes it to the
// static type of the expression; `gbr.exe` says what the receiver really is.

/// `Flying::IsInAir` -- 7 sites, all in `CROW_MOVE.VS` and `CROW_IDLE.VS`.
///
/// `0x0051bd20`: resolve, shift `[obj+0x194]` right by 22, mask 1. That is it.
/// An unresolvable receiver answers **false** after formatting a complaint into
/// the sink that is a bare `ret`, so nothing prints.
///
/// **Registered once, on `Flying`, and nowhere else.** The name string
/// `"Flying::IsInAir"` has exactly one immediate cross-reference; there is no
/// `Settlement::IsInAir`, which an earlier census of this project's had. One
/// shipped site spells the receiver out -- `ol[i].AsFlying().IsInAir()` -- and
/// that is the one that settles it.
HostOutcome m_is_in_air(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsInAir: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::boolean(slot != nullptr && slot->state.flags.in_air));
}

/// `Unit::user` -- the script scratch int, read.
///
/// `0x005d8bf0` is `[obj+0x164]` and nothing else; an unresolvable receiver
/// reports *"called for an uninitialized or invalid object"* into the discard
/// sink and answers 0. Eight sites: three crows reading each other's landing
/// decision, five in the Britain ship sequences. See `ObjectState::user` for
/// why a field the engine never reads is hashed anyway.
HostOutcome m_user(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("user: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::integer(slot == nullptr ? 0 : slot->state.user));
}

/// `Unit::Mutate(str)` -- 2 sites, and the sole blocker of
/// `DATA\SUBAI\TEUTON_CONVERT.VS`, which is the whole of what it is for: a
/// Teuton rider becomes an archer and an archer becomes a rider.
///
/// 0x005df420 pops the class name, resolves the receiver, and looks the name
/// up in the class graph. A receiver that does not resolve and a name no class
/// answers to are both messages and nothing else -- the second one prints the
/// name it was given. Then it either mutates now or **defers**.
///
/// ## The deferral, and why there is none here
///
/// `if (the object whose script is running == this object)` the routine stores
/// the new class in a global and returns 2, and the mutation happens later, at
/// the per-object hook on `vtbl+0x24` -- the same hook that performs a deferred
/// `Erase`. It has to: 0x005deef0 *destroys* the object, and the running script
/// holds a pointer to it.
///
/// A script here holds an `ObjectId` and looks the object up on every use, so
/// there is nothing to dangle and the mutation is done where it is asked for.
/// The difference is one of ordering, and **the shipped call cannot see it**:
/// `Mutate` is the last statement on both of its branches, and the branches are
/// an `if`/`else`, so nothing runs afterwards to notice either way.
///
/// ## What it does to the object
///
/// `World::mutate_class` carries the field list and says what is left behind.
/// Three things belong to the systems rather than to the object, and this is
/// where they are put right:
///
///   * **the squad is cleared** -- the copy writes `[new+0x174] = 0`;
///   * **the commands and the running script go** with the object that had
///     them;
///   * **the combatant is re-classed rather than replaced**, because
///     experience (`[obj+0x180]`) is one of the fields the copy carries, and a
///     fresh combatant would start at level 1. What does reset is everything
///     the destroyed object was in the middle of -- its target, its action,
///     its swing -- and every bonus an effect had put on it, because the new
///     object has no effects.
HostOutcome m_mutate(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Mutate: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  // `CVXUnit::Mutate called for an uninitialized or invalid object`, which is a
  // message and not a trap.
  if (slot == nullptr) return HostOutcome::ok_void();
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) {
    return HostOutcome::failed("Mutate: expected a class name");
  }
  const ClassGraph* graph = world->class_graph();
  if (graph == nullptr) return HostOutcome::ok_void();
  // `lookup` rather than `find`, for `ClassFilter::parse`'s reason: the
  // original resolves a class name by `id` and then by `altid`.
  const ClassIndex to = graph->lookup(ctx.arg(1).as_string());
  // Where the original prints the name it could not find and stops. Refusing
  // here is an equivalence -- `World::mutate_class` refuses `kNoClass` on its
  // own -- and it is kept because this is the message's place.
  if (to == kNoClass) return HostOutcome::ok_void();

  const ObjectId self = slot->id;
  const ClassIndex from = slot->class_index;
  if (!world->mutate_class(self, to)) return HostOutcome::ok_void();

  // The destroyed object's behaviours go with it and the new one starts its
  // class's own, from the start virtual the factory calls (0x005aec40; see
  // `start_behaviors`). No shipped mutation reaches this -- neither Teuton
  // class has a `<behavior>` -- so it is the original's structure, kept.
  HostContext* host = host_context_of(ctx);
  if (ctx.scheduler != nullptr && host != nullptr && host->library != nullptr) {
    (void)stop_behaviors(*ctx.scheduler, *host->library, *world, self, from);
    (void)start_behaviors(*ctx.scheduler, *host->library, *world, self);
  }

  // The AI's half (0x005df1e4 and 0x005df287): the old object is taken out
  // of its owner's class tally and its squad (0x004448a0), and the new one is
  // put into both (0x00447200: the tally, then `AddToSquad` towards the node
  // under it). Neither runs for a unit carrying `SetNoselectFlag`'s bit; the
  // squad leave here does, as it always has. `docs/formats/class-tally.md`.
  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) {
    const SquadKey key = heroes->squads().squad_of(self);
    if (key.valid()) (void)heroes->squads().leave(*world, key, self);
    const WorldObject* now = world->find(self);
    if (now != nullptr && !now->state.flags.noselect && now->state.owner != kNoPlayer) {
      heroes->ai_mutate(*world, self, from);
      const GaikaId under = world->gaika().at(world->lsa(), unit_pos_rh(*world, self));
      (void)add_to_squad(*world, *heroes, self, under);
    }
  }
  if (CommandSystem* commands = command_system(*world); commands != nullptr) {
    (void)commands->clear_commands(*world, self);
    (void)commands->kill_command(*world, self);
  }
  if (CombatSystem* combat = combat_system_of(*world); combat != nullptr) {
    (void)combat->reclass(self, to);
  }
  return HostOutcome::ok_void();
}

/// `Unit::SetUser(int)` -- the same field, written.
///
/// `0x005d7c80` is two instructions of substance: resolve, then
/// `[obj+0x164] = value`. **No clamp and no notification**, and the sibling one
/// entry along in the table (`0x005d7cd0`, which writes `[obj+0x16c]`) does
/// have a side effect, so the absence here is a reading rather than an
/// omission. An unresolvable receiver writes nothing.
HostOutcome m_set_user(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetUser: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  if (found == nullptr) return HostOutcome::ok_void();
  if (ctx.count() < 2 || !ctx.arg(1).is_integer()) {
    return HostOutcome::failed("SetUser: expected an integer");
  }
  ObjectState* state = world->mutable_state(found->id);
  if (state == nullptr) return HostOutcome::ok_void();
  state->user = ctx.arg(1).as_integer();
  return HostOutcome::ok_void();
}

// Defined below, beside `Unit::HasSpecial`, which is the entry point it exists
// for. Declared here because the parry pair is gated on special 0 and belongs
// next to `user` rather than 250 lines away from it.
[[nodiscard]] bool class_has_special(World& world, const WorldObject& slot, std::int32_t index);

/// `Building::GlobalSpellStart(spell, player)` -- 9 sites, the sole blocker of
/// seven scripts -- and `GlobalSpellStop()` -- 1.
///
/// The other half of the stonehenge is already here: `IsStonehengeControlable`
/// and `StonehengeNumControllingMages` are 800 lines below, with the sweep they
/// share. These two are the dispatch side -- the five `*_DISPATCH.VS` scripts
/// are one line each, `.GlobalSpellStart(gs<Name>, player)`.
///
/// **Validated and dropped, the way `PlayMovie` is**, and the reason is that
/// **nothing reads what they write.** The pair maintains a record on the global
/// spell manager at `[0x009a70a4]`: `GlobalSpellStart` (0x004dd440, and
/// 0x004dd4f0 with a data argument besides) requires a town-hall receiver and
/// a spell id 0..5, then appends `{spell, player - 1, now, handle}` through
/// 0x0052d3e0, which also posts message 0x184 once, moves a UI panel, and sets
/// a global flag while a bloodlust record (spell 4) is live -- a flag with no
/// reader found. `GlobalSpellStop` (0x004dd5b0) removes the receiver's. The only registered reader is
/// `Building::GetGlobalSpell` (0x004dd6a0), which has **zero call sites** in
/// all 885 scripts -- and cannot distinguish "no spell" from `gsWindOfWisdom`
/// anyway, because 0x0052c260 answers 0 for both. So storing the record would
/// be state with no consumer, which is the test `ObjectState::user` passes and
/// this fails.
///
/// What the record is *for* in the original is the effect: `WindOfWisdom`,
/// `Starvation`, `SoothingRain` and `DivineSacrifice` are four more registered
/// entry points, each with zero call sites, because the engine applies them
/// itself from the manager. This engine has no global spells to apply.
///
/// The guards are reproduced because they are what the scripts can see, and
/// one of them is looser than its own diagnostic. The receiver must cast to
/// **`CVXTownHall`** (`[0x00824be4]`, whose descriptor names that class) and
/// nothing more -- there is no `IsHeirOf("Stonehenge")` here, unlike the two
/// counters below, which make both tests. `RESEARCH.VS` is why: it calls
/// `th.GlobalSpellStart(gsTribute, .player)` on a **town hall**, and
/// `ONFINISH_RESEARCH.VS` stops it there. So *"called for an object that is not
/// a stonehenge"* is a message about the intent rather than about the check.
///
/// The spell id is checked `0 <= id < 6` (0x004dd4bb), which is exactly the six
/// `gs*` constants `sim/globals.cpp` already carries -- `gsWindOfWisdom` 0
/// through `gsTribute` 5 -- and the player is 1-based (`dec edi` at
/// 0x004dd4c8). All three refusals print into the sink that is a bare `ret` in
/// retail and return, so a script never sees one.
/// **One body for both**, which is the same arrangement `ShowTutorial` and
/// `ShowHint` have and for the same reason: the two differ only in what the
/// original *stores*, and this stores nothing. A `kStart` template parameter
/// was written and removed -- it made the two functions differ in nothing an
/// injected fault could reach, which is a distinction that reads as a claim and
/// is not one. The split is the first thing to restore if a global spell ever
/// gets an effect.
HostOutcome m_global_spell(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GlobalSpell: no world");
  // Resolve what the original looks at, and stop there -- `CreateFeedback`'s
  // gesture, and for its reason: naming the input is what keeps this honest
  // about being a request the interface drops rather than a body that happens
  // to compile. **The original's three guards are deliberately not reproduced**
  // and are written out above instead: with nothing stored, none of them can
  // change an answer, and a guard that cannot is validation theatre. They are
  // the first thing to restore alongside the split.
  (void)object_of(*world, ctx.arg(0));
  return HostOutcome::ok_void();
}

/// Three globals with no consumer here, on `GlobalSpellStart`'s precedent:
/// **the input is named, and nothing is stored.**
///
///   * `SetGlobalBloodlust(bool)` -- 2 sites, both in
///     `STONEHENGE_BLOODLUST.VS`, and the sole blocker of it. 0x004c6610
///     writes the byte argument into the global at `0x00996ae4`. That global
///     has exactly two readers and both are in 0x0050ff30, which the blow
///     resolution 0x00511550 calls to decide which of the attacker's specials
///     a blow uses and to charge each one's stamina cost: with the flag set,
///     the special behind mask `0x1000` (effect code `0xe`) is never charged
///     its `[0x00996e10]`. This engine's blows charge no special any stamina,
///     so the flag would be state nothing could read -- which is the test
///     `ObjectState::user` passes and this fails. **A known gap, by design**:
///     it lands with the specials' stamina costs, not before.
///   * `GetResearchHack()` -- 1 site, and it is **false**. 0x004db630 answers
///     `[0x009969a8] != 0`, and the only writer of that global anywhere in the
///     image is 0x004dfdf0, a debug path that sets it and clears it again
///     inside one function. Nothing a script or a map can do turns it on, so
///     the honest answer here is the one the retail build gives: no.
///   * `InvalidateRegenConsts()` -- 2 sites, and it is a **cache clear**.
///     0x005d30b0 writes zero to `[0x009c0828]`, which 0x005d30c0 uses as the
///     resume point of a linear scan over the regeneration table at
///     `0x009bf880`. There is no such cache here -- the constants are read
///     where they are used -- so there is nothing to invalidate, and a no-op is
///     the same observable behaviour rather than an omission.
///
/// All three are bound rather than left trapping for the reason the whole
/// project binds this shape: a declared-but-unimplemented name stops the script,
/// and every one of these is a statement rather than a question.
HostOutcome f_set_global_bloodlust(CallContext& ctx) {
  // Named, and dropped. `CreateFeedback`'s gesture.
  (void)ctx.arg(0);
  return HostOutcome::ok_void();
}

HostOutcome f_get_research_hack(CallContext& ctx) {
  (void)ctx;
  return HostOutcome::ok_with(Value::boolean(false));
}

HostOutcome f_invalidate_regen_consts(CallContext& ctx) {
  (void)ctx;
  return HostOutcome::ok_void();
}

/// `Catapult::SetBuildFrame(n)` -- 2 sites, both in `CATAPULT_IDLE.VS`, and
/// what a siege engine under construction looks like: playtest #19.
///
/// 0x004e2ed0 turns `n`, the health still to be built, into a frame of the
/// animation playing -- `(frames - 2) * (max - n - 1) / (max - 1) + 1`, in
/// 32-bit integers with a truncating divide, `max` the object's health maximum
/// at `[obj+0xc8]` and `frames` the declared frame count of the animation at
/// `[obj+0x44]` (`[anim+0x48]`) -- writes it to `[obj+0x20c]`, and hands it to
/// the sprite, which shows that frame and stops its own clock (0x00629500
/// zeroes `[sprite+0x80]`). An invalid receiver prints into the bare-`ret`
/// sink and returns.
///
/// **Why it matters.** The script opens construction with
/// `.StartDelayedAnim(1, .pos, -1)`, the build animation, and then calls this
/// every 500 ms cycle -- with no crew inside as well as with one -- so the
/// animation never runs: the engine is drawn at the stage its health has
/// reached, frame 1 at one point of health. For as long as this was dropped,
/// the build animation played through its 385 ms and held its last frame, a
/// finished engine, while the builders were still walking to it and its health
/// stood at 1: the machine "built itself before its crew had entered".
///
/// Here the frame is `WorldObject::build_frame`, presentation, which the view
/// draws through `build_frame_row`. The simulation's side of construction --
/// the health, `IsBuilt`, the crew `UnitsCount` counts -- never depended on
/// it. A receiver with no entity or no animation playing stores nothing: the
/// original would read the entity's table at whatever `[obj+0x44]` held, and
/// this engine has nothing to read there. A `max` of 1 or less is the
/// original's division by zero and stores nothing either. **Labelled:** the
/// object's maximum is taken as `.maxhealth` answers it.
HostOutcome m_set_build_frame(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetBuildFrame: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  WorldObject* slot = found == nullptr ? nullptr : world->find(found->id);
  if (slot == nullptr || slot->object == nullptr || slot->object->entity == nullptr) {
    return HostOutcome::ok_void();
  }
  const std::int32_t playing = slot->object->anim.anim_slot;
  const EntityAnim* anim = playing == kNoAnim ? nullptr : slot->object->entity->anim(playing);
  if (anim == nullptr) return HostOutcome::ok_void();
  const std::int32_t max = max_health_of_object(*world, *slot);
  if (max <= 1) return HostOutcome::ok_void();
  const std::int32_t left = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  // The loader caps a declared count at 64 (0x00600fbf).
  const std::int32_t frames = anim->frames < 64 ? anim->frames : 64;
  const std::int64_t scaled = static_cast<std::int64_t>(frames - 2) * (max - left - 1);
  slot->build_frame = static_cast<std::int32_t>(scaled / (max - 1) + 1);
  return HostOutcome::ok_void();
}

/// Three more that are **presentation**, accepted and dropped for the reason
/// `GlobalSpellStart` and `CreateFeedback` are: the input is named, nothing is
/// stored, and there is no simulation state on the other side of any of them.
///
///   * `Building::PopTransportationUI/0` -- 2 sites, both in
///     `INN_TRANSPORT_REQUEST.VS`, and its sole blocker. 0x004dd3b0 refuses a
///     receiver that is not an heir of `"Inn"` and otherwise hands the inn to
///     0x006ffd20, which opens the transport window. A window.
///   * `Ship::ShowBuildAnimation/1` -- 1 site, `SHIPYARD_BUILD_SHIP.VS`, which
///     passes the shipyard's exit vector so the hull slides out along it.
///     **Not only presentation, and a labelled gap.** 0x005c6a60 is a
///     suspending entry point (it answers "again in 50 ms" until done): on
///     its first pass it takes the new ship out of the shipyard's holder
///     (0x005d3f20) at the holder's exit point (0x004db6c0), raises the build
///     counter `[ship+0x230]` that `Ship::IsBuilding` reads, then slides the
///     ship along the vector over `ShipBuildAnimTime` ms with a
///     floating-point cosine easing, holding `[ship+0x230]` at the build
///     frame; once the animation's last state has run it drops the counter
///     to 0, restores the idle animation and hands the ship to the AI
///     (0x0041f310). Here the ship stays in the holder `CreateShip` put it in
///     until its first order takes it out, and `IsBuilding` is never true.
///     Reproducing it needs a suspending entry with its own state and a
///     fixed-point stand-in for the easing; recorded for a round of its own.
///     **The AI hand-off is reproduced** (`m_show_build_animation`): the ship
///     `CreateShip` built with the AI manager held off (0x005ef240) is
///     registered -- tallied and filed in a squad -- by this call, at once
///     rather than at the end of the slide.
///   * `Obj::SetDebug/1` -- 1 site, and it is `DEBUG_SELECTION.VS` walking the
///     current selection to turn a debug overlay on. 0x005abcb0 normalises the
///     argument to 0 or 1 and calls the object's `vtbl+0xd4`.
///
/// Each is bound rather than left trapping because the alternative is not "no
/// window", it is the calling script stopping -- and for two of the three the
/// call *is* the script. (`SetBuildFrame` was a fourth until playtest #19 showed
/// what dropping it drew; it has its own body above.)
HostOutcome f_presentation_void(CallContext& ctx) {
  // Resolve what the original resolves and stop there, so that the drop is a
  // decision rather than a body that happens to compile.
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("presentation: no world");
  (void)object_of(*world, ctx.arg(0));
  return HostOutcome::ok_void();
}

/// `Ship::ShowBuildAnimation`'s simulation half: the AI register at its end
/// (0x0041f310, after the slide), which is the only register the new ship
/// gets -- `CreateShip` holds the AI manager at zero while it sets the owner,
/// the position and the holder. The slide itself is the labelled gap above.
HostOutcome m_show_build_animation(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ShowBuildAnimation: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) heroes->ai_register(*world, slot->id);
  return HostOutcome::ok_void();
}

/// `Obj::class` -- 22 sites in 17 scripts, and it is the class's **id**.
///
/// `0x005abda0` reads the class descriptor at `[obj+0x3c]` and returns the
/// `std::string` at `+4` -- the long/short branch at 0x005abe07 tests `+0x1c`
/// against 16, so it is an MSVC `std::string` and the object begins there. The
/// same field `Sacrifice::IsInvisibility` compares against `"Hide"`.
///
/// `EVALRECRUIT.VS`'s `if (sq.Leader.class == "BVikingLord")` is what the
/// answer has to satisfy, and `<class id="BVikingLord">` is where that string
/// comes from -- so this is `ClassDefinition::id` and not the `cpp_class`, the
/// entity path or the `altid`.
///
/// A receiver that does not resolve answers the **empty string**: 0x005abdd0
/// pushes the literal at 0x007ab85a, which is `""`, after printing into the
/// discard sink. So does an object with no class, which every synthetic one is.
HostOutcome m_class(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("class: no world");
  const auto empty = [] { return HostOutcome::ok_with(Value::string(std::string())); };
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ClassGraph* graph = world->class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) return empty();
  return HostOutcome::ok_with(Value::string(std::string(graph->at(slot->class_index).id)));
}

/// `Unit::SetEntering(bool)` -- 21 sites, and one bit.
///
/// `0x005d8240` is `[obj+0x194] &= ~0x100000` then `|= 0x100000` when the
/// argument is true, and nothing else. See `ObjectFlags::entering` for what it
/// means and why a flag no registered entry point reads is stored anyway.
///
/// An unresolvable receiver writes nothing and does not trap.
HostOutcome m_set_entering(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetEntering: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  if (found == nullptr) return HostOutcome::ok_void();
  ObjectState* state = world->mutable_state(found->id);
  if (state == nullptr) return HostOutcome::ok_void();
  state->flags.entering = ctx.count() > 1 && ctx.arg(1).truthy_scalar();
  return HostOutcome::ok_void();
}

/// `Unit::SetParryMode(bool)` -- 31 sites, and the sole blocker of 15 scripts.
///
/// `0x005d8f80`, and it has three parts. The gate is
/// **`[unit+0x198] & 1`** -- bit 0 of the 64-bit specials mask, which is
/// `Parry`, entry 0 of the same 36-name table `HasSpecial` above reads. A unit
/// whose class does not offer the special writes nothing at all. Then a
/// same-value write is discarded (0x005d8fd9), and only then does the field at
/// `[unit+0x1a0]` change.
///
/// **The third part is not reproduced and is named rather than skipped.**
/// Entering parry mode also spawns an effect object of class `Parry` --
/// 0x00518650's entry 4, the same 28-entry factory `Invisibility`, `Heal` and
/// `Curse` come out of -- and stores its handle at `[unit+0x1c8]`; leaving
/// destroys it and writes the 0xffff sentinel back. That is the spell-effect
/// machinery `Sacrifice` belongs to, which this engine does not have. The
/// factory picks the class's `2` variant (`DefensiveCry2`, `Assault2`, …) when
/// the receiver's own class radius exceeds 20, which is how a big unit gets a
/// big effect; recorded here because it is the only place in this file that
/// mechanism is written down.
///
/// The 31 shipped sites are almost all one line: `.SetParryMode(false)` as the
/// opening statement of a sub-AI command, clearing the stance before the unit
/// is told to do something else.
HostOutcome m_set_parry_mode(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetParryMode: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  if (found == nullptr) return HostOutcome::ok_void();
  // The `Parry` special. Index 0, and spelled as an index rather than as a
  // name because that is what the mask is: `sim/globals.cpp`'s `parry` constant
  // and this bit are one table seen from two sides.
  if (!class_has_special(*world, *found, 0)) return HostOutcome::ok_void();
  const bool wanted = ctx.count() >= 2 && ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  ObjectState* state = world->mutable_state(found->id);
  if (state == nullptr) return HostOutcome::ok_void();
  state->parry_mode = wanted ? 1 : 0;
  return HostOutcome::ok_void();
}

/// `Unit::GetParryMode()` -- 2 sites, and the reason the field is stored.
///
/// `0x005d9050` carries **the same specials gate as the setter**, before it
/// reads -- bit 0 of the 64-bit specials word at `[obj+0x198]` -- and a unit
/// without `Parry` answers **-1** whatever the field `[obj+0x1a0]` holds. Both
/// verifiers depend on that: `UNIT_LEAVE_PARRY_MODE_VERIFY.VS` asks
/// `GetParryMode() == 1` and `UNIT_ENTER_PARRY_MODE_VERIFY.VS` asks `== 0` over
/// a whole selection, so a unit that cannot parry must answer neither.
///
/// An unresolvable receiver prints *"called for an uninitialized or invalid
/// object"* and **pushes nothing**, so the caller reads whatever was on the
/// stack; 0 stands in for that here.
HostOutcome m_get_parry_mode(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetParryMode: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  if (!class_has_special(*world, *slot, 0)) return HostOutcome::ok_with(Value::integer(-1));
  return HostOutcome::ok_with(Value::integer(slot->state.parry_mode));
}

/// `Building::IsBroken()` -- 9 sites, and it is a **stored tier**, not a
/// health test.
///
/// `0x004dd200` is `[building+0x204] == 3` and nothing else. The tier and the
/// hysteresis that makes it state are documented on `ObjectState::damage_state`
/// and computed by `next_building_state`; `Building::RRepair` (0x004dd250)
/// tests the same `== 3` before it does anything, so tier 3 is exactly *this
/// building is a ruin*.
///
/// An unresolvable receiver answers **true**: 0x004dd22f writes the literal 1
/// into the return slot on the miss path, which is the opposite of what the
/// other predicates in this file do and is reproduced because
/// `VILLAGE_REPOPULATE_VERIFY.VS` walks a list it has not cleared of the dead.
HostOutcome m_is_broken(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsBroken: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::boolean(true));
  return HostOutcome::ok_with(Value::boolean(slot->state.damage_state == 3));
}

/// `Building::IsVeryBroken()` -- 10 sites, and **a different mechanism from
/// its neighbour**, which is the finding.
///
/// `0x00424f90` divides: `health < maxhealth / 20`, computed on the spot with
/// no tier, no threshold and no hysteresis. So the two predicates the shipped
/// verifiers write side by side -- `if (b.IsBroken() || b.IsVeryBroken())` in
/// five files, `if (b.IsBroken() && b.IsVeryBroken())` in a sixth -- are a
/// stored tier and an instantaneous ratio, and they can disagree in both
/// directions: a wall dropped from full health to 20% is under tier 3's 25%
/// but above 5%, and one repaired from 2% to 6% is still in tier 3 while the
/// ratio has already cleared.
///
/// **The divisor was 10 here and it is 20**, which is a correction and not a
/// tightening. The sequence is `mov eax, 0x66666667; imul edx; sar edx, 3`,
/// and that shift is the tell: `sar 1` after that multiply is a divide by
/// five, `sar 2` by ten, `sar 3` by **twenty**. Ten was read off the magic
/// constant without counting the shift. `Squad::InvadeThroughGate` settles it
/// from a second direction -- it guards on the identical five instructions
/// before it will order a squad through a hole -- which is why the two now
/// share `is_very_broken` rather than each carrying the arithmetic.
///
/// The division is integer and signed. A class with no `maxhealth` gives
/// `0 / 20 = 0`, and `health < 0` is false for anything alive, so a decor
/// answers false.
///
/// An unresolvable receiver answers **false** here, not true: 0x00424fd9 writes
/// the comparison's own result, and the comparison reads through a null base.
/// That is a fault in the original rather than a behaviour, so the miss path
/// is the ordinary one.
HostOutcome m_is_very_broken(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsVeryBroken: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(Value::boolean(is_very_broken(*world, *slot)));
}

/// `Unit::GetSacrifice()` -- 5 sites, and it answers the invalid handle.
///
/// **That is the base implementation, not a stub.** `0x005d7bc0` calls the
/// receiver's virtual at `vtbl + 0x12c`, and in every `CVXUnit`-family vtable
/// in the executable that slot holds `0x0051af80`, whose whole body is
/// `mov word [out], 0xffff; ret 4`. Exactly one class overrides it --
/// `0x00511c80`, reading the handle at `[obj+0x1cc]` -- and that class is
/// `CVXDruid`: its constructor (0x00511be0) chains to the `CVXUnit` constructor
/// and then clears three handles at `+0x1cc`, `+0x1ce` and `+0x1d0`.
///
/// `[druid+0x1cc]` is *the sacrifice this druid is serving*, written by
/// `Sacrifice::AddDruid` and cleared back to 0xffff by the sacrifice's own
/// teardown, which walks its druid list at `[sacrifice+0x14c]` and unhooks
/// every member (0x00595021-0x00595037). This engine has no sacrifices --
/// nothing spawns one, because the ritual and spell machinery is not here --
/// so no object can be attached to one and the answer is the base's.
///
/// **What that makes true of the corpus is worth saying.** `UNIT_STAY_VERIFY.VS`
/// ends `if (!s.IsValid()) return false;` and `DRUID_HELP_VERIFY.VS` ends
/// `return (s.IsValid());`, so both verifiers answer no and their commands
/// never start -- which is the same thing that happens in the retail game to a
/// druid standing next to a unit that is not performing a ritual.
HostOutcome m_get_sacrifice(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetSacrifice: no world");
  (void)object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(invalid_object());
}

/// `Sacrifice::AddDruid(druid, time)` -- 2 sites, both the one line of
/// `DRUID_HELP_SACRIFICE.VS` that a druid loops on once it has reached a
/// ritual: `s.AddDruid(this, .GetAnimTime(18))` behind `s.IsValid()`. That
/// name was the sole blocker of the file's 35 sites.
///
/// `0x00595af0` is registered `[7, 3, 39, 35, 1]`: bool, a `Sacrifice`
/// receiver, a `Druid` and an `int`. An invalid receiver prints `The function
/// 'Sacrifice::AddUnit' called for an uninitialized or invalid object.` --
/// the internal name -- and answers **false**. Otherwise it is one call into
/// 0x00595a30 with the druid and `now + time`, where `now` is read through
/// the clock pointer the sacrifice keeps at `[sacrifice+0x18]`, and the answer
/// is whether that returned non-zero, which it always does. What 0x00595a30
/// does: if the druid is already serving a *different* sacrifice
/// (`[druid+0x1cc]`, the handle `GetSacrifice` reads), it is unhooked from
/// that one first (0x005950e0); the druid's handle is written to
/// `[druid+0x1cc]`; the sacrifice's druid list at `[sacrifice+0x14c]` is
/// walked for an entry carrying the druid, and a hit only refreshes that
/// entry's deadline at `+0xc` (0x00595ace) while a miss inserts a new entry
/// and bumps the sacrifice's own count (0x00595600 with 1). So the second
/// argument is a lease: the druid stays attached until `now + time` unless it
/// calls again, which the script does every animation.
///
/// **No sacrifice exists in this engine**, for the reason `GetSacrifice` gives:
/// the ritual and spell machinery that spawns one is not here. So the valid
/// branch is unreachable from any shipped script and nothing is stored for
/// it -- a live `NativeClass::sacrifice` object, which only a synthetic world
/// can hold, answers true and remembers nothing, and `GetSacrifice` on the
/// druid keeps answering the invalid handle. Recording the lease would be
/// state that no reader can observe, which is the test `SetLastAttackTime`
/// already settled the other way. What is bound is the half the corpus can
/// reach: a receiver that is not a sacrifice answers false without a trap,
/// which is what lets the helper's own guard, and not this entry point, decide
/// when the druid stops.
HostOutcome m_add_druid(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AddDruid: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || slot->object == nullptr || !slot->object->is_a(NativeClass::sacrifice)) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  // The original reads through the druid pointer unguarded; a druid that
  // resolves to nothing is a fault there and a false here.
  const bool druid = ctx.count() > 1 && object_of(*world, ctx.arg(1)) != nullptr;
  return HostOutcome::ok_with(Value::boolean(druid));
}

/// `Sacrifice::IsInvisibility()` -- 3 sites, and it tests a class name that
/// **no shipped class has**.
///
/// `Sacrifice::Consume(price, period, wait)` -- 1 site, `GHOST_SAC_IDLE.VS`:
/// `life = .Consume(price, 1000, true)`, looped while `life > 0` and the
/// ghost it placed is alive, then the ghost is put down and the ritual
/// `.Erase()`d.
///
/// `0x00596440` is registered suspending, `[1, 4, 39, 1, 1, 7]`: int, a
/// `Sacrifice` receiver, two ints and a bool. It reads the interpreter's
/// first-call byte -- the one `CallContext::first_call` carries -- and, when
/// the bool is set on a first call, writes `period` into the interpreter's
/// sleep slot (0x00a77eac) and answers the suspend code, so the script sleeps
/// `period` and re-enters; on the re-entry, or at once when the bool is
/// clear, it hands `price` and `period` to the core at 0x00596050 and pushes
/// what that returns. The core is the ritual's economy, and it opens with the
/// druid-list prune (0x00595150, with the sacrifice clock's now): every entry
/// whose druid is gone, is down to a tenth of its maximum health or whose
/// lease has run out leaves the list (and its druid's `[+0x1cc]` goes back to
/// 0xffff), and **the stop flag `[sacrifice+0x15c]` is written as "the list is
/// empty"**. Then 0 outright while the flag is set, otherwise `price` spread
/// over the druids left -- each paying from its health, none below a tenth of
/// its maximum -- and the health obtained is the answer. So an empty list
/// never reaches the division at 0x00596204: the prune has stopped the ritual
/// first. docs/formats/sacrifice.md.
///
/// **No druid can be on that list here**, for the reason `AddDruid` gives, so
/// the prune always finds it empty: the flag goes up on the sacrifice, which
/// `IsEmpty` then answers, and the answer is 0. The wait is reproduced because
/// the script's shape depends on it: one period, then the ghost dies. The
/// receiver is not checked, because the original does not check it either; it
/// resolves it or carries a null into the core.
HostOutcome m_consume(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Consume: no world");
  const bool wait = ctx.count() > 3 && ctx.arg(3).truthy_scalar();
  if (wait && ctx.first_call) {
    HostOutcome outcome;
    outcome.status = script::HostStatus::retry;
    outcome.suspend_for =
        ctx.count() > 2 && ctx.arg(2).is_integer() && ctx.arg(2).as_integer() > 0
            ? ctx.arg(2).as_integer()
            : 0;
    return outcome;
  }
  if (const WorldObject* slot = object_of(*world, ctx.arg(0));
      slot != nullptr && slot->object != nullptr && slot->object->is_a(NativeClass::sacrifice)) {
    if (ObjectState* state = world->mutable_state(slot->id); state != nullptr) {
      state->flags.sacrifice_stopped = true;
    }
  }
  return HostOutcome::ok_with(Value::integer(0));
}

/// `Obj::MistAction(damage)` -- 1 site, `MIST_IDLE.VS`, once per animation
/// while the mist has stamina: `This.MistAction(GetConst("MistDamage"))`.
///
/// `0x005ca050` (and `Sacrifice::MistAction` at 0x00595360, the same body
/// over the other receiver type) resolves the receiver, then collects every
/// object within `[obj+0xd4]` of its position -- the object's **range**, the
/// slot `Catapult::GetPointOnTarget` reads as the maximum range, which the
/// `Mist` class declares as `range="200"` -- through the spatial query at
/// 0x00542100 with the filter at 0x005c9f20 -- a unit (`[obj+0x2c] &
/// 0x400000`) that is not dead (`vtbl+0x50`) -- and for each one whose tag at
/// `[unit+0x14c]`
/// resolves to nothing or to this very mist: folds a 1,600 ms marker into the
/// sync hash (0x005d2980, bookkeeping, not behaviour), writes its own handle
/// into the tag, calls the unit's `vtbl+0xa8` with `damage` -- the same
/// virtual `Obj::Damage` reaches at 0x005ab66c -- and adds `damage` to its
/// owner's `damage_inflicted` and the unit's owner's `damage_taken`
/// (`[player+0xa4]`, `[player+0xa0]`), the argument rather than the health
/// removed, which is the one place the two readings of that counter differ.
///
/// Here: `World::objects_in_radius` over a match-all filter, the unit flag,
/// `ObjectState::mist` as the tag, `CombatSystem::apply_damage` as the
/// virtual, and `MatchSystem::record_damage` for the two counters, given the
/// argument as the original gives it. An invalid receiver does nothing; the
/// original resolves it to a null it then reads through, which no shipped
/// path reaches.
HostOutcome m_mist_action(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("MistAction: no world");
  const WorldObject* mist = object_of(*world, ctx.arg(0));
  if (mist == nullptr) return HostOutcome::ok_void();
  const ObjectId self = mist->id;
  const PlayerId owner = mist->state.owner;
  const std::int32_t damage =
      ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  CombatSystem* combat = combat_system_of(*world);
  MatchSystem* match = match_system_of(*world);

  std::vector<ObjectId> around;
  world->objects_in_radius(world->resolve_position(self), class_int(*world, *mist, "range"),
                           ClassFilter{}, around);
  for (const ObjectId id : around) {
    const WorldObject* slot = world->find(id);
    // The collection's filter (0x005c9f20): a unit, and not dead.
    if (slot == nullptr || !slot->state.flags.is_unit || slot->state.health <= 0) continue;
    // Another live mist's unit is not this one's to burn.
    if (slot->state.mist != kNoObject && slot->state.mist != self &&
        world->find(slot->state.mist) != nullptr) {
      continue;
    }
    if (ObjectState* state = world->mutable_state(id); state != nullptr) state->mist = self;
    if (combat != nullptr) {
      combat->apply_damage(*world, id, damage < 0 ? 0 : damage);
      if (combat->world_bound() && combat->find(id) != nullptr) {
        (void)world->set_health(id, combat->health(id));
      }
    }
    if (match != nullptr) match->record_damage(*world, owner, slot->state.owner, damage);
  }
  return HostOutcome::ok_void();
}

/// `Obj::CoverOfMercyAction()` -- 1 site, `COVEROFMERCY_IDLE.VS`, once per
/// animation while the cover has stamina.
///
/// `0x005ca6a0` resolves the receiver, casts it to **`CVXAreaEffect`** (the
/// name at 0x00824bcc) -- anything else does nothing -- and hands it to
/// 0x005ca5a0, which:
/// walks the list at `[cover+0x14c]`..`[+0x150]` of handles it sheltered last
/// time and clears `0x10000000` -- `ObjectFlags::half_damage` -- on every one
/// that still resolves; builds a player query (0x00514080) from the cover's
/// owner and the complement of that owner's war mask at `[player+0x10]`,
/// so the sheltered are the units of every player the owner is **not at war
/// with**, the owner included; collects them within the class's `radius`
/// (`[class+0x2dc]`, `radius="1000"` on the shipped `CoverOfMercy`) of the
/// cover's position through 0x005ca220, which raises the bit on each as it
/// collects (`or [unit+0x194], 0x10000000` at 0x005ca3ba and 0x005ca4c0);
/// and replaces the list with the new set (0x005958f0).
///
/// Here the list is kept the other way round, one `ObjectState::sheltered_by`
/// per unit -- see that field for the one arrangement where the two differ
/// -- and the war test is `PlayerTable::is_enemy` from the owner's side, the
/// one-directional reading `Obj::IsEnemy` proved. The collector (0x005ca220)
/// tests each unit's owner bit, the low half of `[obj+0x2c]`, against that
/// mask, so **an unowned unit, whose owner half is zero, is not sheltered**;
/// it also skips templates and lets a hidden unit through only when its
/// owner is in the cover owner's side mask (`[player+0x18]`), which is not
/// modelled -- a hidden friendly unit is sheltered here. No life test. An
/// invalid receiver does nothing.
HostOutcome m_cover_of_mercy_action(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CoverOfMercyAction: no world");
  const WorldObject* cover = object_of(*world, ctx.arg(0));
  if (cover == nullptr || cover->object == nullptr ||
      !cover->object->is_a(NativeClass::area_effect)) {
    return HostOutcome::ok_void();
  }
  const ObjectId self = cover->id;
  const PlayerId owner = cover->state.owner;
  const Point centre = world->resolve_position(self);
  const std::int32_t radius = class_int(*world, *cover, "radius");

  // Lower the bit on everyone this cover sheltered last time ...
  for (const WorldObject& slot : world->objects()) {
    if (slot.state.sheltered_by != self) continue;
    if (ObjectState* state = world->mutable_state(slot.id); state != nullptr) {
      state->flags.half_damage = false;
      state->sheltered_by = kNoObject;
    }
  }
  // ... and raise it on every friendly unit standing under it now.
  std::vector<ObjectId> around;
  world->objects_in_radius(centre, radius, ClassFilter{}, around);
  const PlayerTable& players = world->players();
  for (const ObjectId id : around) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr || !slot->state.flags.is_unit) continue;
    // The owner bit against the friendly mask: no owner, no bit, no shelter.
    if (!PlayerTable::is_valid(slot->state.owner) || players.is_enemy(owner, slot->state.owner)) {
      continue;
    }
    if (ObjectState* state = world->mutable_state(id); state != nullptr) {
      state->flags.half_damage = true;
      state->sheltered_by = self;
    }
  }
  return HostOutcome::ok_void();
}

/// `Obj::MagicActionEnd()` -- 1 site, `SUMMONING_BEHAVIOR.VS`, on a ghost or a
/// mist whose stamina has run out, just before it is put down.
///
/// `0x005c9f40` resolves the receiver -- an invalid one does nothing -- and
/// hands its handle to 0x005d5aa0, which sweeps the whole map's grid
/// (0x005d4a50 over the world rect) and, for every unit whose tag at
/// `[unit+0x14c]` is that handle, writes `0xffff` into the tag
/// (0x005d4bf9). That is the release of everyone the mist was burning, so
/// another mist may take them; the sacrifice's own teardown (0x00595008)
/// reaches the same routine. Here: every `ObjectState::mist` equal to the
/// receiver is cleared. A dead mist's tag stops resolving on its own, which
/// is why the sweep is not load-bearing for a mist that dies, but the script
/// calls this *before* the death and the clear is visible in between.
HostOutcome m_magic_action_end(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("MagicActionEnd: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_void();
  const ObjectId id = self->id;
  for (const WorldObject& slot : world->objects()) {
    if (slot.state.mist != id) continue;
    if (ObjectState* state = world->mutable_state(slot.id); state != nullptr) state->mist = kNoObject;
  }
  return HostOutcome::ok_void();
}

/// `HasStarvingArmy(ms)` / `StarvingArmyPos(ms)` -- 1 site each, both the
/// tutorial `UNITSADVICE1.VS`: `if (HasStarvingArmy(20000)) { obj =
/// StarvingArmyPos(20000); ShowHint(..., obj); }`.
///
/// `0x00559c20` and `0x00559d50` walk the session's notification registry --
/// the map at `[0x009bdaac]+0x14`, keyed by name -- to the entry named
/// `army starving` (0x007c2d9c), and through its records to the first whose
/// stamp at `+0x34` is at least `ms` behind the game clock: the predicate
/// answers whether there is one, the position form answers that record's
/// object, or the invalid handle. The records are posted by the food tick
/// (0x005dbedf) for every unit of the **local player** found at zero food.
///
/// Here the registry is not kept -- `UserNotification` is presentation -- and
/// the age is read off `FeedingUnit::hungry_since`, the stamp the feeder's
/// own tick writes at the same moment, over the units of
/// `HostContext::local_player`. A session with no local player has no
/// starving army, which is what a headless run of the original also reports,
/// its notification list being the screen's.
///
/// **Two parts of the position form are not reproduced**, both of the
/// registry this does not keep: on a hit `StarvingArmyPos` consumes the
/// record (0x00559430) and answers, of the record's objects, the one nearest
/// a point it holds, where this answers the first hungry unit. And
/// `HasStarvingArmy` has a zero-argument form (0x0055a7c0) that answers
/// whether the `army starving` key is in the registry at all; no shipped site
/// calls it and it is not bound.
template <bool kPosition>
HostOutcome fn_starving_army(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("HasStarvingArmy: no world");
  const HostContext* host = host_context_of(ctx);
  const PlayerId local = host == nullptr ? kNoPlayer : host->local_player;
  const FeederSystem* feeder = feeder_of(*world);
  const std::int64_t age =
      ctx.count() > 0 && ctx.arg(0).is_integer() ? ctx.arg(0).as_integer() : 0;
  ObjectId found = kNoObject;
  // The `local != kNoPlayer` half is an equivalence over any world whose
  // hungry units are owned -- the owner test below refuses them all -- and
  // is kept for the reading: nobody's screen sees nobody's army.
  if (feeder != nullptr && local != kNoPlayer) {
    const GameTime now = world->time();
    for (const FeedingUnit& link : feeder->chain()) {
      if (!link.feeds || link.food > 0 || link.hungry_since <= 0) continue;
      const WorldObject* slot = world->find(link.unit);
      if (slot == nullptr || slot->state.owner != local) continue;
      if (now - link.hungry_since < age) continue;
      found = link.unit;
      break;
    }
  }
  if (kPosition) return HostOutcome::ok_with(object_value(found));
  return HostOutcome::ok_with(Value::boolean(found != kNoObject));
}

/// `0x00594f10` reads the receiver's class descriptor at `[obj+0x3c]`, takes
/// the `std::string` at `+4` -- the same object `Obj::class` (0x005abda0)
/// returns, long/short branch and all -- and hands it to the CRT's
/// case-insensitive compare against the literal `"Hide"` at 0x007c7664. The
/// answer is `== 0`.
///
/// There is no `HIDE.SC.XML` anywhere in the installation. The invisibility
/// effect the 28-entry factory at 0x00821180 spawns is class `Invisibility`,
/// and `INVISIBILITY.SC.XML` ships; `Hide` does not, in `data.pak` or in any
/// container. So this predicate is **false for every object the shipped data
/// can produce**, and `UNIT_STAY_VERIFY.VS`, whose last statement is
/// `return s.IsInvisibility;`, can never verify. Recorded rather than repaired:
/// a name that was renamed late and left behind in one comparison is exactly
/// the kind of thing this project reproduces rather than fixes.
///
/// The rule itself is real and is implemented as the rule, so a class named
/// `Hide` -- which a mod or a test may declare -- answers true.
HostOutcome m_is_invisibility(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsInvisibility: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ClassGraph* graph = world->class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const std::string_view name = graph->at(slot->class_index).id;
  if (name.size() != 4) return HostOutcome::ok_with(Value::boolean(false));
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  const bool hit = lower(name[0]) == 'h' && lower(name[1]) == 'i' && lower(name[2]) == 'd' &&
                   lower(name[3]) == 'e';
  return HostOutcome::ok_with(Value::boolean(hit));
}

/// `Ship::IsBuilding(bool)` -- **not** a class test, and not the `Building` bit.
///
/// `0x005c6fc0` reads the dword at `[ship+0x230]` and answers `> 0`, signed. It
/// is a construction counter and the name is a progressive verb: *this ship is
/// still being built*. `Obj::AsBuilding` (0x005aa5e0), which is the
/// `[obj+0x2c] & 0x00800000` class test, shares nothing with it.
///
/// `SHIP_IDLE.VS` is what settles the reading from the corpus side: it opens
/// `if (.IsBuilding()) { while (.IsBuilding()) Sleep(100); ... }`, and a class
/// test would make that loop run forever.
HostOutcome m_is_building(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsBuilding: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::boolean(slot != nullptr && slot->state.flags.building));
}

/// `CLAMP(v, lo, hi)` -- 3 sites, and one behaviour that is not the idiom.
///
/// `0x00696070` is two branches and an early return: **if `v < lo` the result is
/// `lo` and the function returns there**, without ever comparing `hi`. Only
/// then does `v > hi` give `hi`.
///
/// Written as `max(lo, min(v, hi))` that is wrong whenever the bounds are
/// inverted: with `lo = 5`, `hi = 2`, `v = 9` the original answers **2** -- `v`
/// is not below `lo`, then `v > hi` -- where the idiom answers 5. **The high
/// bound wins when the bounds cross and the value is at or above `lo`.** The
/// two-branch form is transcribed rather than tidied for exactly that reason.
///
/// All three shipped sites are in `HERO_SKILL_BEHAVIOUR.VS` and pass `lo = 0`
/// with a possibly-negative value -- `CLAMP(u.maxhealth - u.health, 0,
/// nHealing)` -- which is the one case where the early return fires on shipped
/// data.
///
/// A second body (0x00696bc0) registers the same name and arity over the
/// floating type. It is unreachable from the corpus and `engine/core` is
/// freestanding with no floating point, so it is deliberately not written.
HostOutcome fn_clamp(CallContext& ctx) {
  if (!ctx.arg(0).is_integer() || !ctx.arg(1).is_integer() || !ctx.arg(2).is_integer()) {
    return HostOutcome::failed("CLAMP: expected three integers");
  }
  const std::int32_t value = ctx.arg(0).as_integer();
  const std::int32_t low = ctx.arg(1).as_integer();
  const std::int32_t high = ctx.arg(2).as_integer();
  if (value < low) return HostOutcome::ok_with(Value::integer(low));
  if (value > high) return HostOutcome::ok_with(Value::integer(high));
  return HostOutcome::ok_with(Value::integer(value));
}

/// `Obj::IsMilitary` (0x005ad680) and `Obj::IsSentry` (0x005ad500) -- one
/// shape twice.
///
/// Each holds a lazily-cached class handle -- `"Military"` (0x00540130) and
/// `"Sentry"` (0x005400c0), looked up by name once (0x00409e40) -- and runs the
/// is-heir-of test (0x0059a220): the receiver's class descriptor `[obj+0x3c]`,
/// itself included, then up `[cls+0xe4]`, compared against the handle; a name
/// the lookup did not find is a null handle and false. No native-class test. An
/// unresolvable receiver prints its own *"called for an uninitialized or invalid
/// object"* into the discard sink and answers **false**, which is an ordinary
/// return here.
///
/// A class name the graph cannot resolve reads as false and never as "matches
/// everything": see `class_is`, which is where that rule is kept.
template <bool kMilitary>
HostOutcome m_class_predicate(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsMilitary: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(
      Value::boolean(class_is(*world, *slot, kMilitary ? "Military" : "Sentry")));
}

/// `Building::GetUITarget()` (0x004ddc50) and `Building::SetUITarget(obj)` --
/// 3 sites and 1, and they are the player's manual target for a tower.
///
/// The getter hands back the handle at `[obj+0x1fc]` with a sub-object offset
/// of zero, and answers the **invalid handle with no message** when the
/// receiver does not resolve -- unusually quiet for this family, and the two
/// tower scripts depend on it: `TOWER_GUARD.VS` writes
/// `target = .GetUITarget.AsUnit; if (target.IsAlive) ...`, so an invalid
/// handle has to flow through `AsUnit` and fail `IsAlive` rather than trap.
///
/// The setter (0x004ddbf0) resolves the argument first and the receiver
/// second; an unresolvable receiver writes nothing and says nothing, and an
/// unresolvable argument stores `0xffff`. Neither tests a class or a life.
///
/// See `ObjectState::ui_target` for why it is hashed.
HostOutcome m_get_ui_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetUITarget: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ObjectId target = slot == nullptr ? kNoObject : slot->state.ui_target;
  return HostOutcome::ok_with(Value::object(script::ObjectRef{kTypeObj, target}));
}

HostOutcome m_set_ui_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetUITarget: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  ObjectState* state = world->mutable_state(slot->id);
  if (state == nullptr) return HostOutcome::ok_void();
  // The argument is stored as the handle it is, live or not: the tower script
  // is what tests `IsAlive`, and clearing a target on the target's death is the
  // interface's business rather than this one's.
  const WorldObject* target = object_of(*world, ctx.arg(1));
  state->ui_target = target != nullptr ? target->id : kNoObject;
  return HostOutcome::ok_void();
}

/// `Teleport::destination` -- 20 sites, and the root of the teleport family.
///
/// `0x005cdbc0` reads the 16-bit object id at `[teleport+0x208]` and hands it
/// back as a handle; a receiver that resolves to nothing logs *"The function
/// 'Teleport::destination' called for an uninitialized or invalid object."* and
/// answers the invalid handle. There is no type test on the receiver: the
/// argument code says `Teleport`, and what the body does is read a field at a
/// fixed offset, so a non-teleport handle reads whatever is there. This answers
/// `kNoObject` for one instead, which is the same thing every reader tests for
/// and is the only safe translation of an out-of-bounds read.
///
/// The pairing comes from the map and is resolved once at load; see
/// `ObjectState::teleport_destination` for the census that makes it well
/// defined, and `World::populate_from_map` for the resolution.
HostOutcome m_teleport_destination(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("destination: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ObjectId far = slot == nullptr ? kNoObject : slot->state.teleport_destination;
  // The pair is stored as a handle and the far end can die -- a teleport is a
  // building and buildings are destroyed. A stale id resolves to nothing here
  // rather than to a live object with a reused id, because ids are monotone and
  // never reused, so `find` answering null is the whole check.
  if (far != kNoObject && world->find(far) == nullptr) {
    return HostOutcome::ok_with(Value::object(script::ObjectRef{}));
  }
  return HostOutcome::ok_with(Value::object(script::ObjectRef{kTypeObj, far}));
}

/// `building.GetExitVector()` -- 5 sites -- and `teleport.exit_vector` -- 6.
/// **Two names, two entry points in `gbr.exe`, and one value.**
///
/// It is the odd one out in the entrance family and the header of
/// `sim/entrance.hpp` sets the four side by side: this reads **nothing from the
/// point table**. `0x004dd330` is a plain read of two consecutive scalars on the
/// building's *class* record, and `Teleport::exit_vector` (0x005cdc40) reads two
/// instance fields, `[tp+0x20c]`/`[tp+0x210]`, that the teleport loader
/// (0x005cd950) copies verbatim from those same class scalars. Same numbers, no
/// scaling, two accessors -- but the instance read has no absent-marker test,
/// and a miss pushes a half-written point (0xffff in x's low half, whatever was
/// on the stack above it, and y = 0) where this answers `(-1, -1)`. Every
/// shipped teleport class declares the pair, so the two agree on retail data.
///
/// **The class attributes are `exit_vector_x` and `exit_vector_y`**, and they
/// are `.sc.xml`'s rather than `.ent.xml`'s -- which is why the entity format's
/// 39-attribute census does not have them and `docs/formats/sc-xml.md` does.
/// Only shipyards and teleports author them: `(-120, 80)`, `(120, -100)`,
/// `(-250, 250)` and so on, ten classes in all, and every other class in the
/// installation inherits the "absent" marker.
///
/// **World units, not a unit direction.** The magnitudes are 80 to 250, against
/// class radii of 350 to 410, so the corpus's
///
///     ptGo = .GetCentralBuilding.GetExitVector();
///     ptGo.Set( ptGo.x * 5, ptGo.y * 5 );
///     wagon.AddCommand( false, "move", This.pos + ptGo );
///
/// is a 600-to-1250-unit push out past the dock, not a five-unit nudge. `+x` is
/// east and `+y` is south, which the shipyards' own `SW`/`SE`/`NE`/`NW` art
/// names confirm four ways round.
///
/// **The pair is inherited as a pair**, which is the one subtlety. The original
/// tests `exit_vector_x` against its engine-wide "attribute absent" marker
/// (0xff1b1e40, the same one 23 other class scalars use) and, if *either* half
/// is absent, copies **both** from the parent. So the value comes from the
/// nearest ancestor that declares the two together -- not, as a per-key
/// resolution would have it, from two different ancestors. No shipped class
/// declares one without the other, so the two readings agree everywhere in the
/// installation; this is the original's rule because a class that split them
/// would be answered wrongly by the other one, and silently.
///
/// A class that declares neither anywhere up its chain answers `(-1, -1)`. That
/// is a sentinel a caller could test, and no caller does: all eleven sites
/// scale it and add it to a position.
///
/// **Read against the body (`0x004dd330`).** It resolves the receiver through
/// the object table, takes the class record at `[obj+0x3c]` and answers
/// `([cls+0xacc], [cls+0xad0])`, or `(-1, -1)` when the first is the absent
/// marker. **No class test on the receiver**: anything with a class answers.
/// The pair is filled by the class loader (0x0059f540, `atol` of each
/// attribute) and inherited as a pair at 0x0059c310 -- either half absent,
/// both copied from the parent. `atol` would take `"80px"` as 80 where the
/// strict parse here skips to the parent; all ten shipped declarations are
/// plain integers, so the two agree on the installation.
///
/// **An unresolvable receiver is the original's own fault, and `(-1, -1)` stands
/// in for it.** That arm prints *"called for an uninitialized or invalid
/// object"* into the discard sink and pushes a *one-byte* `1` where an
/// eight-byte point belongs -- the bool-returning failure tail -- so the
/// caller reads seven bytes of whatever was below the receiver. There is no
/// value to reproduce.
HostOutcome m_exit_vector(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("exit_vector: no world");
  static constexpr Point kNoVector{-1, -1};
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ClassGraph* graph = world->class_graph();
  if (slot == nullptr || graph == nullptr) return HostOutcome::ok_with(pack_point(kNoVector));

  // The class's **own** declaration, not the resolved one: the pair rule needs
  // to know which class declared the two together, which a resolution that
  // walks each key independently cannot say.
  const auto declared = [&](ClassIndex at, std::string_view key, std::string_view& out) {
    for (const ClassProperty& property : graph->at(at).properties) {
      if (property.key != key) continue;
      out = property.value;
      return true;
    }
    return false;
  };

  for (ClassIndex at = slot->class_index; at != kNoClass; at = graph->at(at).parent_index) {
    std::string_view sx;
    std::string_view sy;
    if (!declared(at, "exit_vector_x", sx)) continue;
    if (!declared(at, "exit_vector_y", sy)) continue;
    std::int32_t x = 0;
    std::int32_t y = 0;
    if (!parse_int(sx, x) || !parse_int(sy, y)) continue;
    return HostOutcome::ok_with(pack_point(Point{x, y}));
  }
  return HostOutcome::ok_with(pack_point(kNoVector));
}

/// `GetCatapultAttackPoint(pt)` -- 3 sites, and it is the shot-scatter model.
///
/// `0x004e23d0` reads `CatapultVariationRadius` from `DATA\CONST.INI`'s
/// `[GamePlay]` (200 in the shipped file), **rejection-samples a uniform offset
/// inside a disc of that radius**, adds it to the argument and clamps the
/// result into the map rectangle. `CATAPULT_ATTACK.VS` and
/// `CATAPULT_AUTOFIRE.VS` both apply it to the *aim* point before rotating.
///
/// **The draw count is part of the state vector**, which is the whole risk in
/// this one: two draws per rejection round, and a divergent implementation
/// desynchronises everything downstream of it rather than only the shot. So the
/// loop is transcribed rather than replaced by a closed form:
///
///   * the acceptance test is `isqrt(dx*dx + dy*dy) <= r`, **not**
///     `dx*dx + dy*dy <= r*r`. Those differ: `isqrt` floors, so a squared
///     length anywhere in `[r*r + 1, (r+1)*(r+1) - 1]` passes the first test
///     and fails the second, and the disc the original samples is that much
///     wider;
///   * each round draws `x` then `y`, in that order, from the world RNG's
///     inclusive `[-r, r]`;
///   * and there is a **pre-test with the corner** `(-r, -r)`: if
///     `isqrt(2*r*r) <= r` the loop never runs at all and the offset is
///     `(r, r)`. That is true for `r` of 0, 1 and 2 -- `isqrt(2)` and
///     `isqrt(8)` are 1 and 2 -- so a small enough radius makes this function
///     deterministic and draws nothing. The shipped 200 is not small enough,
///     but a modded `CONST.INI` could be, and a body that always looped would
///     consume draws the original does not.
///
/// The clamp is the map rectangle's, inclusive on all four edges, and it is
/// applied per axis in the order low then high -- `point::IntoRect`'s rule, and
/// the same one `sim/flying.cpp` follows.
HostOutcome fn_get_catapult_attack_point(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetCatapultAttackPoint: no world");
  const Point aim = ctx.count() > 0 && is_point(ctx.arg(0)) ? unpack_point(ctx.arg(0)) : Point{};

  std::int32_t radius = 0;
  if (const EnvSystem* env = env_of(*world); env != nullptr) {
    (void)env->constant("CatapultVariationRadius", radius);
  }
  // A negative radius would make `between(-r, r)` an empty range; the original
  // reaches the same place through `isqrt` of a positive square, so both leave
  // the loop unentered.
  if (radius < 0) radius = 0;

  const auto r64 = static_cast<std::int64_t>(radius);
  std::int32_t dx = radius;
  std::int32_t dy = radius;
  if (isqrt(2 * r64 * r64) > r64) {
    do {
      dx = world->rng().between(-radius, radius);
      dy = world->rng().between(-radius, radius);
    } while (isqrt(static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy) >
             r64);
  }

  const std::int32_t high = map_high_corner(*world);
  const auto clamp = [high](std::int32_t v) {
    if (v < 0) v = 0;
    if (v > high) v = high;
    return v;
  };
  return HostOutcome::ok_with(pack_point(Point{clamp(aim.x + dx), clamp(aim.y + dy)}));
}

/// `Teleport::Traverse(player)` -- 6 sites, and **it moves nothing**.
///
/// `0x005cdcb0` ORs the player record's word `+0x18` into the mask at
/// `[teleport+0x214]` (0x005cdd2b..0x005cdd2e) -- not the player's one bit but
/// the whole of that word, which is the set of players who share view with
/// this one (SetRelation, 0x005652f0, fills it from bit 0x10 of each word
/// *toward* the player; see `m_can_see`), its own bit included. Its reader,
/// 0x005cda20, asks whether a player's word and the mask share a bit. So a
/// traversal is recorded for the traveller's whole view-sharing side. The
/// scripts do the movement themselves and call this on *both* ends of the
/// pair afterwards, which is what says it is a record rather than an action:
/// `HERO_TELEPORT.VS` writes `tel.Traverse(.player); tel.destination.Traverse
/// (.player); .SetPos(pt);` on three consecutive lines.
///
/// **The original's bounds check has no lower half.** `player - 1` is compared
/// against 16 with a signed test only, so `Traverse(0)` indexes one stride
/// below the player table, and above 16 it reads through a null record. Both
/// are faults and not behaviours: this rejects anything outside 1..16 and
/// does nothing.
///
/// A receiver that resolves to nothing is a no-op. The original prints on that
/// path -- and prints `Teleport::exit_vector`'s message rather than its own,
/// which is a copy-paste in the executable and not a clue about either. This
/// ORed the player's own bit alone until the host audit read the body.
HostOutcome m_teleport_traverse(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Traverse: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  if (ctx.count() < 2 || !ctx.arg(1).is_integer()) return HostOutcome::ok_void();
  const std::int32_t player = ctx.arg(1).as_integer();
  if (player < 1 || player > 16) return HostOutcome::ok_void();
  ObjectState* state = world->mutable_state(slot->id);
  if (state == nullptr) return HostOutcome::ok_void();
  const PlayerId traveller = player_from_script(player);
  std::uint32_t side = 0;
  for (PlayerId other = 0; other < kPlayerCount; ++other) {
    if (other == traveller || world->players().has(other, traveller, Relation::share_view)) {
      side |= 1u << other;
    }
  }
  state->traversed_by |= side;
  return HostOutcome::ok_void();
}

/// `Druid::SetSummoningDeath(bool)` (0x005135a0) and `IsSummoningDeath()` --
/// 3 sites and 2. See `ObjectFlags::summoning_death`.
///
/// **The setter has no guard and no message at all**, which is rare enough here
/// to be worth transcribing rather than tidying: an unresolvable receiver
/// silently does nothing, where every other flag setter in this file at least
/// prints.
HostOutcome m_set_summoning_death(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetSummoningDeath: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  ObjectState* state = world->mutable_state(slot->id);
  if (state == nullptr) return HostOutcome::ok_void();
  state->flags.summoning_death =
      ctx.count() > 1 && ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  return HostOutcome::ok_void();
}

HostOutcome m_is_summoning_death(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsSummoningDeath: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::boolean(slot != nullptr && slot->state.flags.summoning_death));
}

/// `Building::GetPoint(int type, int n)` -- 32 sites, and the largest name in
/// the entrance/exit family by a wide margin.
///
/// **It is also the one that needs none of that family's machinery.**
/// `0x004dd150` reads the class's own `<points>` table and does not project,
/// does not sample terrain, does not bake a list and does not draw a random
/// number. The arguments are `(type, n)`: the point's role tag, and which one
/// of that type, counting from zero in table order.
///
/// **The answer is a relative offset, not a world position**, and every shipped
/// caller adds the object's own position back: `aa1 = .pos + .GetPoint(3,
/// inds[0])` in `GATE_PATROL.VS`. `x` comes back verbatim and `y` is scaled by
/// **1448/1024**, which is the reciprocal of the 181/256 the isometric
/// projection uses, at finer fixed point. The height term that makes the full
/// unprojection iterative cancels here because an offset is a *difference*: the
/// point and the object that owns it stand on the same ground.
///
/// **`1448/1024` and `362/256` are the same function** -- integer division by
/// `4b` of `4a` is division by `b` of `a` -- so no test can tell them apart and
/// a fault that substitutes one for the other survives by arithmetic rather
/// than by a gap. The wider pair is written here because it is the pair
/// `0x004dd150` itself uses; the narrower one appears in the routines that
/// unproject.
///
/// `SENTRY_GUARD.VS` settles that reading from the other side by using the
/// result as a **direction** rather than a position, which is consistent only
/// with a relative vector.
///
/// **The miss sentinel is `(-32768, -32768)` and it has to be exact**, because
/// the dominant corpus shape is a walk that terminates on it --
/// `RAM_ATTACK.VS` counts `k` upward through `GetPoint(17, k)` until `pt.x`
/// comes back as the sentinel. A dead or absent receiver answers the same way,
/// as does a type the class declares no points of.
///
/// **One difference from the original is known and deliberate.** It walks all
/// 64 table slots rather than stopping at the count, so a class with fewer
/// declares zero-filled slots that read as type 0 at (0, 0); this table holds
/// only the authored points. No shipped call site asks for type 0 -- the
/// corpus asks for 1, 2, 3, 8, 10 and 17 -- so the two agree everywhere the
/// content goes, and inventing 64-count phantom points to match would be
/// reproducing a buffer rather than a behaviour.
constexpr std::int32_t kNoPointCoord = -32768;

HostOutcome m_get_point(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetPoint: no world");
  const Point missing{kNoPointCoord, kNoPointCoord};
  const WorldObject* found = object_of(*world, ctx.arg(0));
  if (found == nullptr || found->object == nullptr || found->object->entity == nullptr) {
    return HostOutcome::ok_with(pack_point(missing));
  }
  const auto whole = [&](std::size_t at) -> std::int64_t {
    return ctx.count() > at && ctx.arg(at).is_integer() ? ctx.arg(at).as_integer() : 0;
  };
  const std::int64_t type = whole(1);
  std::int64_t wanted = whole(2);
  // A negative index cannot match: the original counts up from zero and never
  // reaches one.
  if (wanted < 0) return HostOutcome::ok_with(pack_point(missing));
  for (const EntityPoint& point : found->object->entity->points()) {
    if (point.type != type) continue;
    if (wanted-- > 0) continue;
    return HostOutcome::ok_with(pack_point(
        Point{point.x, static_cast<std::int32_t>(static_cast<std::int64_t>(point.y) * 1448 / 1024)}));
  }
  return HostOutcome::ok_with(pack_point(missing));
}

/// `building.GetEnterPoint(unit)` -- **18 sites**, and the door a unit walks to.
///
/// `0x004dce60` resolves the unit argument, then the receiver, and in the
/// valid case ensures the building's enter lists are built (0x004db650), picks
/// the deep-water list or the land list by the **unit class's `water_unit`**
/// (class `+0xb30`, the field `IsWaterUnit` reads, 0x005ac6b0), and returns
/// **the element nearest the unit's stored position** -- its `vtbl + 0x3c`,
/// which for a unit as for everything else is `[obj+0x24]`/`[obj+0x28]`, the
/// `(-1, -1)` marker for a held one. The picker (0x0056fba0) is a linear scan
/// taking an integer square root of each squared distance before an unsigned
/// strict compare, which orders identically to comparing the squared
/// distances, ties to the first -- so the root is cosmetic and not reproduced.
///
/// Where the list comes from is `sim/entrance.hpp`: the class's type-1 points,
/// each run through the height-aware round trip, each discarded if it lands off
/// the map or somewhere nothing can stand, and each filed by the ground it
/// landed on. That header also records the one deliberate difference -- the
/// original bakes the list once per building and this derives it per call.
///
/// **Every fallback is a real point, never a sentinel**, which is why not one of
/// the 18 sites tests the result before walking to it. In the original's order:
///
///   * a receiver that names nothing answers **the unit's stored position** --
///     or `(0, 0)` when the unit names nothing too;
///   * a unit argument that names nothing answers **the building's position**;
///   * an empty list answers **the building's position**.
///
/// The original prints a diagnostic on each (`Buildilng::GetEnterPoint: No
/// enter points found!`, the typo its own, for the third) through a sink that
/// is a bare `ret` in retail, so all three are ordinary answers. **There is no
/// life test**: a corpse or a broken building still resolves, and this tested
/// `health > 0` on both until the host audit read the body.
///
/// **A `Teleport` is a `Building` here**: `HERO_TELEPORT.VS` calls this on one,
/// so the receiver test is "does this handle name an object", not a class test.
///
/// Deterministic: no RNG anywhere on this path. Its sibling `GetExitPoint`
/// draws one, which is why that name is not here.
HostOutcome m_get_enter_point(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetEnterPoint: no world");
  const WorldObject* unit = ctx.count() > 1 ? object_of(*world, ctx.arg(1)) : nullptr;
  const WorldObject* building = object_of(*world, ctx.arg(0));

  if (building == nullptr) {
    return HostOutcome::ok_with(pack_point(unit == nullptr ? Point{0, 0} : unit->state.position));
  }
  const Point home = building->state.position;
  if (unit == nullptr) return HostOutcome::ok_with(pack_point(home));

  const bool water = class_flag(*world, *unit, "water_unit");
  return HostOutcome::ok_with(
      pack_point(enter_point_near(*world, building->id, unit->state.position, water)));
}

/// `building.GetExitPoint(...)` -- **7 sites**, and the door a unit leaves by.
///
/// Two overloads, both arity two, and the corpus uses both: `gbr.exe` registers
/// `(Obj, point)` at 0x004dcfa0 and `(point, bool)` at 0x004dd080. They differ
/// in **where the water flag comes from and nothing else** -- the first reads it
/// off the object argument's class record, the second is handed it -- and both
/// hand the same reference point to the same worker (0x004db6c0). Six of the
/// seven sites pass `(point, false)`; `RUIN_BEHAVIOR.VS` passes `(hero, .pos)`.
/// So the form is chosen here by whether the first argument is a point, which is
/// the only thing that separates them.
///
/// **The reference point is an argument, not the receiver's position, and that
/// is the whole difference from `GetEnterPoint`.** The enter half measures from
/// the unit that is walking in; this measures from wherever the caller says, and
/// three of the seven sites say "the settlement's central building" while three
/// more say "a point I just rotated by a random angle" -- a ring around the
/// central building in `VILLAGE_TRAINPEASANT.VS`, the destination teleport's own
/// exit vector in the two teleport scripts. The list it picks from is the
/// class's type-2 points put
/// through the same round trip -- `sim/entrance.hpp` -- and the pick is
/// `pick_door_by_distance`, weighted by inverse fourth power rather than
/// nearest-wins. **This draws from the RNG and its sibling does not.**
///
/// **Every fallback is the caller's own point**, which is why not one of the
/// seven sites tests the result for a sentinel *the engine can produce*:
///
///   * an unresolvable receiver answers **the point argument**, after a
///     diagnostic (*"The function 'Building::GetExitPoint' called for an
///     uninitialized or invalid object."*);
///   * an empty door list answers **the building's own position**, from the
///     worker rather than from here; and
///   * a result of exactly `(-1, -1)` answers **the point argument**, after a
///     second diagnostic (`Buildilng::GetExitPoint: No exit points found!`,
///     carrying the same typo as the enter half's).
///
/// Both diagnostics go to 0x00686eb0, a bare `ret` in the retail build, so all
/// three are ordinary answers rather than failures.
///
/// **The `(-1, -1)` two shipped scripts test for is their own input coming
/// back.** `ARENA_HIREHERO.VS` and `BARRACK_TRAIN.VS` both write
/// `ptExit = .GetExitPoint(.settlement.GetCentralBuilding.pos, false)` and then
/// `if (ptExit.x == -1 && ptExit.y == -1)`. Nothing on this path *invents* that
/// pair -- the last fallback returns the argument -- so the test can only pass
/// when the argument was already `(-1, -1)`, which is what `.pos` answers for a
/// settlement with no central building. The check reads as a sentinel test and
/// is a missing-building test, and the branch it guards agrees: it places the
/// unit at `Point(0, 0)` and lets the settlement sort it out.
///
/// **The receiver is resolved and not tested for life.** Neither 0x004dcfa0 nor
/// 0x004dd080 asks anything of the object once the handle resolves, so a dead
/// building still answers with its doors.
///
/// Neither does the enter half: 0x004dce60 has no life test either, and the
/// handle table keeps resolving a dead object until it is destroyed.
///
/// **The water flag of the `(Obj, point)` form is the object's class
/// `water_unit`** (class `+0xb30`, the field `IsWaterUnit` reads), dereferenced
/// without checking the handle -- an unresolvable one faults the original and
/// is land here.
///
/// **Partial: the weights.** The picker (0x0056fa40) takes one draw,
/// `rand(0, 10000)`, and walks the doors summing `1 / (d^4 + 1e-7)` in double
/// precision; `pick_door_by_distance` does the same in fixed point, because the
/// core has no floating point, and is labelled there.
HostOutcome m_get_exit_point(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetExitPoint: no world");
  // Receiver first, so the two script arguments are 1 and 2.
  if (ctx.count() < 3) return HostOutcome::failed("GetExitPoint: expected two arguments");

  // `(Obj, point)` or `(point, bool)`, told apart by the first argument.
  const bool by_object = !is_point(ctx.arg(1));
  const script::Value& where = by_object ? ctx.arg(2) : ctx.arg(1);
  if (!is_point(where)) return HostOutcome::failed("GetExitPoint: expected a point");
  const Point from = unpack_point(where);

  const WorldObject* building = object_of(*world, ctx.arg(0));
  if (building == nullptr) return HostOutcome::ok_with(pack_point(from));

  bool water = false;
  if (by_object) {
    // The same class flag `GetEnterPoint` reads, `water_unit`. The original
    // dereferences the argument's class record without checking the handle
    // first; an unresolvable one is land here rather than a crash.
    const WorldObject* mover = object_of(*world, ctx.arg(1));
    water = mover != nullptr && class_flag(*world, *mover, "water_unit");
  } else {
    water = ctx.arg(2).truthy_scalar();
  }

  MovementSystem* movement = movement_system(*world);
  std::vector<Point> doors;
  building_doors(*world, movement == nullptr ? nullptr : &movement->grid(), building->id,
                 kExitPointType, water, doors);
  // The worker's own empty-list answer, and it is the building rather than a
  // sentinel. It costs no draw, because the original never reaches its picker.
  const Point chosen =
      doors.empty() ? world->resolve_position(building->id)
                    : pick_door_by_distance(doors, from, world->rng());
  return HostOutcome::ok_with(pack_point(chosen == kNoClassPoint ? from : chosen));
}

/// `Unit::GetFlags(int mask)` -- 5 sites, and **the sole blocker of
/// `TOWNHALL_AUTOTRAIN.VS`**, which is 282 call sites and the largest
/// one-blocker script in the corpus.
///
/// Not a getter despite the name: `0x005d7e30` returns **bool**, not the word.
/// It resolves the receiver, tests `[obj+0x194] & mask`, and `setne`s the
/// answer. The registration agrees -- return type 7.
///
/// **Two gates and both are silent.** An unresolvable handle prints
/// *"called for an uninitialized or invalid object"* into the discard sink and
/// answers false; there is no second one. In particular **it does not test
/// whether the object is alive**, which `TOWNHALL_AUTOTRAIN.VS` compensates for
/// itself -- `if (u.IsAlive) ... if (!u.GetFlags(UNITFLAG_NOAI))` -- and
/// `SQUADMONITOR.VS` does not, asking it of a squad leader that may have died
/// since the list was built.
///
/// **Every shipped site passes `UNITFLAG_NOAI`** and no other mask, in all five
/// places across three scripts. The word is composed here rather than stored,
/// from the seven bits of `[obj+0x194]` this engine models; the three the map
/// authors and nothing explains -- bits 17, 21 and 25, see
/// `docs/formats/map.md` -- are **not** in it, so a mask naming one would be
/// answered `false` rather than correctly. That is a real limit and it is
/// written down rather than hidden, on the evidence that no shipped script
/// names one and that inventing a meaning for them would be worse.
///
/// **`Squad::GetFlags` is another entry point, and nothing reaches it.**
/// `0x004216d0` takes no argument -- its signature is an int return and the
/// squad receiver alone -- and answers the squad's flags word `[squad+0x30]`
/// (`Squad::flags` here) widened to an int, through the squad table lookup
/// 0x00443e30, with no validity test. All five shipped `GetFlags` sites pass a
/// mask, on `squad.Leader` or a unit, so they are `Unit::GetFlags/1`; the
/// zero-argument form has no caller and is not bound, as `UnitsGuardingSettlement`
/// is not.
[[nodiscard]] std::uint32_t pack_unit_flags(const ObjectState& state, GameTime now) noexcept {
  const ObjectFlags& flags = state.flags;
  return (flags.autocast ? 0x00000004u : 0u) |    // bit 2  Hero::SetAutocast
         (flags.no_ai ? 0x00040000u : 0u) |       // bit 18 UNITFLAG_NOAI
         (flags.entering ? 0x00100000u : 0u) |    // bit 20 Unit::SetEntering
         (flags.in_air ? 0x00400000u : 0u) |      // bit 22 Flying::IsInAir
         (flags.noselect ? 0x00800000u : 0u) |    // bit 23 Unit::SetNoselectFlag
         (flags.on_minimap ? 0x04000000u : 0u) |  // bit 26 Unit::SetMinimapFlag
         (state.cursed_at(now) ? 0x08000000u : 0u);  // bit 27 Unit::IsCursed
}

HostOutcome m_get_flags(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetFlags: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  if (found == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const std::uint32_t mask =
      ctx.count() > 1 && ctx.arg(1).is_integer()
          ? static_cast<std::uint32_t>(static_cast<std::uint64_t>(ctx.arg(1).as_integer()))
          : 0u;
  return HostOutcome::ok_with(
      Value::boolean((pack_unit_flags(found->state, world->time()) & mask) != 0));
}

/// `Unit::SetNoselectFlag(bool)` -- 6 sites, every one of them passing `true`,
/// and every one an ambient walker: `FISH_IDLE.VS` on its fish and
/// `SETTLEMENT_BEHAVIOR_AMBIENT.VS` on the hens, villagers and messengers it
/// places.
///
/// `0x005d8750` pops a one-byte bool and the receiver handle, and resolves the
/// handle through the object table; an unresolvable one prints *"called for an
/// uninitialized or invalid object"* into the discard sink and returns. No
/// class test, no `IsDead` probe. Otherwise it writes bit 23 of `[obj+0x194]`
/// (`0x00800000`, the same mask the Building bit uses on the other word),
/// set or cleared by the bool.
///
/// **What the bit means is "not the AI's".** Before it writes, and only when
/// the bool is true and the bit is still clear, it calls 0x0041eee0 -- the
/// same AI unregister the unit death virtual (0x005db270) and the erase
/// (0x005db230) end with, and *not* a deselection. 0x0041eee0 does nothing
/// unless the AI manager exists (`[0x8c8e58]`), the object is not a spawn
/// template (`[obj+0x2c] & 0x8000000`), the bit is clear, it has an owner
/// (`[obj+0x70]`) and a squad handle (`[obj+0x174]`); then 0x004448a0 clears
/// the handle and takes the unit out of its squad (0x00444750), and takes its
/// worth off the owner AI's running tallies. The bit then keeps it out: the
/// placement hook (0x0041e820) and `AddToSquad` itself (0x00446a70, its first
/// test) refuse an object carrying it, so a noselect walker never joins a
/// squad again. That is `HeroSystem::ai_unregister` here, and the two
/// refusals are in sim/squad.cpp.
///
/// The selection is not touched: a selected unit stays selected. What keeps a
/// flagged unit from being *picked* is the select paths' own test of the bit
/// (sim/orders.cpp).
///
/// **One gap, labelled.** The owner AI's tallies 0x004448a0 decrements -- a
/// count and a summed power per kind at `[[owner+0x8c]+0x24..0x30]`, and a
/// count per class name -- are not running counters in this engine; nothing
/// here keeps them for the unregister to correct.
HostOutcome m_set_noselect_flag(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetNoselectFlag: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  if (found == nullptr) return HostOutcome::ok_void();
  const bool set = ctx.count() > 1 && ctx.arg(1).truthy_scalar();
  ObjectState* state = world->mutable_state(found->id);
  if (state == nullptr) return HostOutcome::ok_void();
  if (set && !state->flags.noselect && !state->flags.unspawned && state->owner != kNoPlayer) {
    const AiSystem* ai = ai_system_of(*world);
    HeroSystem* heroes = hero_system_of(*world);
    if (ai != nullptr && ai->manager_started() && heroes != nullptr) {
      heroes->ai_unregister(*world, found->id);
    }
  }
  state->flags.noselect = set;
  return HostOutcome::ok_void();
}

/// `Unit::InShip` -- 9 sites, and it is a question about the *holder*.
///
/// `0x005d70f0` resolves the unit's holder handle at `[unit+0x154]`, then asks
/// that holder record for its **ship** slot (`0x005319a0`, `[holder+0x0e]`
/// through the handle table) and answers whether it resolves. Its two siblings
/// make the shape unmistakable: `Unit::GetHolderSett` (0x005d7050) does the
/// identical thing with `[holder+0x0c]`, and `Unit::GetShip` (0x005d7150) calls
/// the very same resolver and hands the ship back.
///
/// So a holder is a settlement's *or* a ship's, and this is how a script tells
/// them apart. Six shipped sites are one idiom --
/// `if (!.InShip && .InHolder) bldEnter = .GetHolderSett.GetCentralBuilding;`
/// -- which is exactly "in a holder that is not a ship, therefore it has a
/// settlement".
///
/// Here the split is the composite allocation rather than a field: a ship's
/// holder is the object minted immediately after the ship (`spawn_ship`, and
/// 3 of 3 in the corpus), where a settlement's is in the economy's table. The
/// ship test is the one asked, because it is the one the entry point asks.
///
/// **The original crashes on an unresolvable receiver** -- unlike its
/// neighbours it skips the validity check and dereferences null. That is a
/// fault, not a behaviour: this answers false.
HostOutcome m_in_ship(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("InShip: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  if (slot == nullptr || slot->state.holder == kNoObject) return no;
  // The holder of a ship is `ship + 1`. Nothing else in the world allocates a
  // standalone holder, so a holder whose predecessor is a ship is that ship's.
  const WorldObject* before = world->find(slot->state.holder - 1);
  if (before == nullptr || before->object == nullptr) return no;
  return HostOutcome::ok_with(
      Value::boolean(before->object->is_a(NativeClass::ship)));
}

/// `a.SameHolderAs(b)` -- two sites, and it is one comparison.
///
/// `gbr.exe` 0x005d8120 pops two `Unit` handles and returns
/// `a->holder == b->holder` on the raw 16-bit field, so **two units that are
/// both outside any holder answer true**: `0xffff == 0xffff`. That is not a
/// tidy answer and it is the one the entry point gives. `SQUADMONITOR.VS`
/// guards with `BestHero.InHolder` first; `UNIT_ATTACH.VS` does not, so a unit
/// in the open sent to attach to a hero in the open attaches at once, at any
/// distance, and the script's next test -- both outside, within 1,500 -- is
/// never reached. On Crossroads every one of 178 attaches in 3,000 turns takes
/// that first branch.
///
/// A receiver or argument that does not resolve answers false. `kNoObject`
/// stands in for `0xffff` on the field, so an unresolvable *handle* and a unit
/// *outside* a holder would otherwise be the same value read two ways.
HostOutcome m_same_holder_as(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SameHolderAs: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  const WorldObject* other = object_of(*world, ctx.arg(1));
  if (self == nullptr || other == nullptr) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(Value::boolean(self->state.holder == other->state.holder));
}

/// `abs(int)` -- one shipped call site, and `INT32_MIN` is the whole finding.
///
/// `0x00694be0` is nine instructions: pop, `test`, `neg` when negative, write
/// back. `neg 0x80000000` is `0x80000000`; the overflow flag is set and
/// ignored, so `abs(INT32_MIN)` is `INT32_MIN`. Reproduced explicitly rather
/// than left to a language that might trap or promote.
HostOutcome fn_abs(CallContext& ctx) {
  if (!ctx.arg(0).is_integer()) return HostOutcome::failed("abs: expected an integer");
  const std::int32_t value = ctx.arg(0).as_integer();
  const auto wide = static_cast<std::uint32_t>(value);
  const std::int32_t out =
      value < 0 ? static_cast<std::int32_t>(~wide + 1u) : value;
  return HostOutcome::ok_with(Value::integer(out));
}

/// `Unit::Curse()` and `Unit::IsCursed()` -- 3 sites each, all in the three
/// `SHAMAN_*.VS` scripts, which round-trip them: `if (tgt.IsCursed) break;`
/// guards the loop that later runs `tgt.Curse();`.
///
/// `Curse` (0x005d90c0) resolves the receiver -- an unresolvable one prints
/// *"called for an uninitialized or invalid object"* and returns -- and calls
/// 0x005d4880 with 0, the worker it shares with `Unit::Bless` (0x005d9100,
/// which passes 1 and works bit 0). For a curse that worker, **only when bit
/// 27 of `[obj+0x194]` is clear**:
///
///   1. cures a blessing first: when bit 0 is set it runs the cure 0x005d4800,
///      which clears it and drops the effect object at `[obj+0x1c6]`;
///   2. sets bit 27 (a masked set, which is why a sweep for an `or` on the word
///      found no writer);
///   3. spawns the curse's feedback (class index 0x18) and keeps its handle at
///      `[obj+0x1c6]` -- presentation, not reproduced;
///   4. arms the object's timer `0x19ec6e2` for `CurseTimeout` ms (`[GamePlay]`
///      in `CONST.INI`, 15,000, read once and cached), re-arming it if one is
///      pending; its handler (0x00512090 -> 0x005d4970) is the cure, which
///      clears bit 27 again.
///
/// So **a curse lasts fifteen seconds**, and a shaman whose target's curse
/// has lifted curses it again. Here the timer is `ObjectState::curse_until`
/// and the bit is live while the clock is short of it (`cursed_at`).
/// `Bless` has no shipped site and is not bound, so step 1 never has a
/// blessing to cure.
///
/// `IsCursed` (0x005d8940) answers bit 27, and **false** after a complaint on
/// an unresolvable receiver; the shaman's guard reads that as "not cursed, go
/// ahead", which is the branch it wants for a target that has stopped
/// existing.
HostOutcome m_curse(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Curse: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  ObjectState* state = world->mutable_state(slot->id);
  if (state == nullptr) return HostOutcome::ok_void();
  const GameTime now = world->time();
  if (state->cursed_at(now)) return HostOutcome::ok_void();
  std::int32_t timeout = 0;
  if (const EnvSystem* env = env_of(*world); env != nullptr) {
    (void)env->constant("CurseTimeout", timeout);
  }
  state->flags.cursed = true;
  state->curse_until = now + timeout;
  return HostOutcome::ok_void();
}

HostOutcome m_is_cursed(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsCursed: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::boolean(slot != nullptr && slot->state.cursed_at(world->time())));
}

/// `Unit::SetLastAttackTime()` (0x005d7fc0) -- 40 sites.
///
/// The handle check (a miss prints and returns), then the game clock
/// (`[0x00996ff4] + 0x1258`) into `[unit+0x1a4]`, and nothing else; the body
/// does not check that the receiver is a unit. The same field is stamped with
/// the same clock whenever a unit actually attacks (0x005d40ad), and
/// **`Unit::TimeWithoutAttack` (0x005d7f50) reads it back** as the clock less
/// the stamp -- `CombatSystem`'s `Combatant::last_attack_time`, which
/// `host_time_without_attack` in sim/combat.cpp answers from. So the stamp is
/// observable, and it is what the shipped sites rely on:
/// `SNEAK_BEHAVIOR.VS` re-hides a sneak once `TimeWithoutAttack` reaches
/// 20000, and `UNIT_GIVEITEM.VS`'s `if (.HasSpecial(sneak))
/// .SetLastAttackTime(); // mountaineer patch` restarts that wait.
///
/// This stored nothing for a while, on a census of `[reg+0x1a4]` that missed
/// the reader. An object with no combat record has nothing to stamp here: in
/// the original that is a non-unit receiver, whose `+0x1a4` is some other
/// field, and no shipped site sends one.
HostOutcome m_set_last_attack_time(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetLastAttackTime: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  if (CombatSystem* combat = combat_system_of(*world); combat != nullptr) {
    if (Combatant* unit = combat->find(slot->id); unit != nullptr) {
      unit->last_attack_time = world->time();
    }
  }
  return HostOutcome::ok_void();
}

/// `MIN(a, b)` and `MAX(a, b)` -- 3 and 2 shipped sites, all integers.
///
/// **Each name is registered twice in `gbr.exe`**, once over `(int, int)` at
/// 0x00694a60 / 0x00694a90 and once over a second type at 0x00695020 /
/// 0x00695060 -- `double`s, eight bytes each on the VM stack, the same
/// comparisons -- and the second pair's type code is one the 35 `RegisterType`
/// sites do not cover. This registry keys on `(kind, name, arity)` and the
/// argument's runtime type discriminates -- the same arrangement
/// `UnitsInSettlement` and `SpawnGroupInHolder` already use -- so one body
/// serves both spellings and the integer form is the one the corpus reaches.
///
/// The bodies are three instructions of substance and the tie-break is worth
/// naming: `MIN` keeps the *second* argument when the two are equal (`jge` at
/// 0x00694a77) and `MAX` keeps it too (`jl` at 0x00694aa7). Unobservable for
/// integers, and stated because it is the sort of thing a rewrite silently
/// flips.
HostOutcome fn_min(CallContext& ctx) {
  if (!ctx.arg(0).is_integer() || !ctx.arg(1).is_integer()) {
    return HostOutcome::failed("MIN: expected two integers");
  }
  const std::int32_t a = ctx.arg(0).as_integer();
  const std::int32_t b = ctx.arg(1).as_integer();
  return HostOutcome::ok_with(Value::integer(a < b ? a : b));
}

HostOutcome fn_max(CallContext& ctx) {
  if (!ctx.arg(0).is_integer() || !ctx.arg(1).is_integer()) {
    return HostOutcome::failed("MAX: expected two integers");
  }
  const std::int32_t a = ctx.arg(0).as_integer();
  const std::int32_t b = ctx.arg(1).as_integer();
  return HostOutcome::ok_with(Value::integer(a > b ? a : b));
}

/// `Unit::SetMinimapFlag(bool)` -- bit 26 of `[obj+0x194]`, forcing the object
/// onto the minimap.
///
/// `0x005d87d0`, and it is `SetNoselectFlag`'s neighbour in every sense: the
/// same second flag word, the same body shape, and every one of its eighty
/// shipped calls sits on the line after a `SetNoselectFlag(true)`. Both are on
/// ambient decoration -- the settlement's hens, `FISH_IDLE.VS`'s fish, the
/// wandering villagers -- which should be visible on the minimap and not
/// clickable.
///
/// The bit has four readers in `gbr.exe` and all four are in the minimap and
/// interface code; no getter is registered. Nothing here draws a minimap yet,
/// so this is state a script sets and the interface layer will read.
HostOutcome m_set_minimap_flag(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetMinimapFlag: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  if (found == nullptr) return HostOutcome::ok_void();
  ObjectState* state = world->mutable_state(found->id);
  if (state == nullptr) return HostOutcome::ok_void();
  state->flags.on_minimap =
      ctx.count() > 1 && ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  return HostOutcome::ok_void();
}

/// The 36 unit specials, as a mask read off the receiver's class.
///
/// `Unit::HasSpecial(int)` (`0x005d8d70`) tests one bit of a 64-bit word at
/// `[obj+0x198]`/`[obj+0x19c]`, refusing an index of 36 or more before it
/// shifts. The word is filled at construction from the class's `unit_specials`
/// property, whose comma-separated tokens the class reader resolves against a
/// 36-entry name table -- **the same table `sim/globals.cpp` already carries**,
/// because the script-side constants `sneak`, `curse`, `cripple` and the rest
/// are indices into it. `curse` is 35 there and `Curse` is entry 35 here; they
/// are one table seen from two sides.
///
/// So this reads the class rather than a per-object copy. `Unit::SetSpecial`
/// exists in the executable and could make the two differ, and it has **zero
/// shipped call sites** -- it is not in `declare_shipped_surface` and nothing
/// can reach it, so the copy could not diverge from the class if it existed.
///
/// The property's tokens are display names (`"Rage, Defense skill"`) where the
/// constants are identifiers (`defense_skill`); lowercasing and turning spaces
/// into underscores is the whole of the difference, and it is checked against
/// the table rather than assumed -- a token that resolves to nothing is skipped
/// in silence, which is what the reader must do with `revitalize`, the one
/// special the corpus names that the executable's table does not have.
/// `freedom`'s bit in the specials word, which the hero constructor sets.
inline constexpr std::int32_t kSpecialFreedom = 30;

[[nodiscard]] bool class_has_special(World& world, const WorldObject& slot,
                                     std::int32_t index) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot.class_index == kNoClass) return false;
  const std::string_view list = graph->property(slot.class_index, "unit_specials");
  std::size_t at = 0;
  while (at <= list.size()) {
    const std::size_t comma = list.find(',', at);
    std::string_view token = list.substr(at, comma == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : comma - at);
    at = comma == std::string_view::npos ? list.size() + 1 : comma + 1;
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
      token.remove_prefix(1);
    }
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
      token.remove_suffix(1);
    }
    if (token.empty()) continue;
    std::string key;
    key.reserve(token.size());
    for (const char c : token) {
      key.push_back(c == ' ' ? '_' : static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    }
    std::int32_t value = 0;
    if (engine_constant(key, value) && value == index) return true;
  }
  return false;
}

/// `Unit::HasSpecial(int)` (0x005d8d70) -- 205 hits in the corpus sweep over
/// four scripts.
///
/// The handle check (a miss prints and answers false), then bit `n`, for an
/// unsigned `n` below 36, of the 64-bit specials word at `[obj+0x198]`. The
/// unit constructor (0x005d3170) copies that word from the class's
/// `unit_specials` (`class_has_special`), and **the hero constructor
/// (0x0052ffd0) then sets bit 30, `freedom`, on every hero** whatever its class
/// says. No shipped `HasSpecial` site asks for freedom -- they ask cripple,
/// curse, healing, invisibility, parry, revitalize, sneak and teaching -- so
/// that bit is invisible through this entry point, and is answered anyway.
/// (`unit_has_freedom`, `HasFreedom`'s rule, reads the class alone and has the
/// same gap; it is another entry point's.)
HostOutcome m_has_special(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("HasSpecial: no world");
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || !ctx.arg(1).is_integer()) return no;
  const std::int32_t index = ctx.arg(1).as_integer();
  // `n >= 0x24` is refused before the shift, and a negative one would shift by
  // a negative amount -- the original's `__allshl` treats the count modulo 64,
  // so this refuses rather than reproducing an accident.
  //
  // **Belt and braces here, and knowingly.** The original needs the guard
  // because it is about to shift; this looks a name up in a 36-entry table, so
  // an index outside it matches nothing and a fault injected into this line
  // survives the whole suite. It is written the way the original writes it
  // because the guarantee is the table's size, not this line's, and because a
  // representation that made the guard load-bearing again is one refactor
  // away.
  if (index < 0 || index >= kUnitSpecialsCount) return no;
  if (index == kSpecialFreedom && slot->state.flags.is_hero) {
    return HostOutcome::ok_with(Value::boolean(true));
  }
  return HostOutcome::ok_with(Value::boolean(class_has_special(*world, *slot, index)));
}

/// `Flying::FindNearBird` -- the first object of the **same entity class** within
/// 2,048 units, or an invalid handle.
///
/// `0x0051c0a0`, and what is remarkable is what it does *not* test: the scan
/// mask is `0xffffffff`, so no category filter runs, and the visitor checks only
/// that the class pointer matches and the object is not the receiver. **No
/// aliveness test, no visibility test, no diplomacy test** -- a crow will fly
/// toward an enemy crow, or a dead one.
///
/// It takes the first match in the sweep's order and stops, which is not the
/// nearest: the grid walk 0x0051b840 -- rows of 256-unit cells, cells left to
/// right, each cell's list in the order its objects came into it
/// (`docs/formats/spatial-index.md`) -- over the circle of 2,048 about the
/// receiver's own point, squared distance at most 2048 squared. A held object
/// is in no cell and is never found; a spawn template is (no `0x08000000`
/// test).
HostOutcome m_find_near_bird(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindNearBird: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(invalid_object());
  const Point from = self->state.position;
  constexpr std::int32_t kBirdRange = 2048;
  std::vector<ObjectId> around;
  world->objects_located_in_rect(from.x - kBirdRange, from.y - kBirdRange, from.x + kBirdRange,
                                 from.y + kBirdRange, around, SweepOrder::grid);
  for (const ObjectId id : around) {
    if (id == self->id) continue;
    const WorldObject* other = world->find(id);
    // "The same entity class" is the class *pointer* in the original -- two
    // objects of one `<class>` -- which here is the class graph index.
    if (other == nullptr || other->class_index == kNoClass ||
        other->class_index != self->class_index) {
      continue;
    }
    const std::int64_t dx = std::int64_t{other->state.position.x} - from.x;
    const std::int64_t dy = std::int64_t{other->state.position.y} - from.y;
    if (dx * dx + dy * dy > std::int64_t{kBirdRange} * kBirdRange) continue;
    return HostOutcome::ok_with(object_value(id));
  }
  return HostOutcome::ok_with(invalid_object());
}

/// The terrain-type byte `IsPointInWater` tests for. `0x005c6d8f` compares
/// against 13 and nothing else in the executable names it.
constexpr std::uint32_t kTerrainWater = 13;

/// `IsPointInWater(point)` -- 127 hits over `LION_LEAD.VS`, `WOLF_LEAD.VS`,
/// `BEAR_IDLE.VS`, `FISH_IDLE.VS` and `SHIP_UNBOARD_ALL_VERIFY.VS`.
///
/// `0x005c6d40` consults **the map's terrain-type layer and nothing else** --
/// not the passability grid, not any object. The layer is 64 units per cell and
/// one byte per cell, packed four cells to a dword, and **water is type 13**.
/// Out of the world rectangle is `false`, silently.
///
/// The `+32` is the original's, at `0x005c6d5b`: it biases by half a cell
/// before it divides, so a point rounds to the nearest cell rather than the one
/// it happens to sit inside. That is a real difference on a boundary and it is
/// transcribed rather than dropped.
///
/// Note the cell size: 64 here, where the object grid the scans walk is 256 and
/// the obstruction bitmap is 16. Three grids, three resolutions, and the number
/// belongs to the layer rather than to the map.
///
/// The bounds test is on the *biased* point against the inclusive world
/// rectangle `(0, 0, w - 1, h - 1)` at `[grid+0xb4..0xc0]`; the layer's extent
/// is `w / 64` cells, so `cx < width` here is that same test on every shipped
/// map, whose sizes (8192, 16384, 32768) are all multiples of 64. The cell is
/// `((x + 32) >> 6, (y + 32) >> 6)`: one byte of a row of dwords, `[layer +
/// 0x10a4]` dwords to a row.
HostOutcome fn_is_point_in_water(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsPointInWater: no world");
  const auto dry = HostOutcome::ok_with(Value::boolean(false));
  if (!is_point(ctx.arg(0))) return dry;
  const Grid& terrain = world->terrain();
  if (terrain.cell_size() == 0) return dry;  // no layer loaded: nothing is water
  const Point at = unpack_point(ctx.arg(0));
  const std::int32_t half = static_cast<std::int32_t>(terrain.cell_size() / 2);
  const std::int32_t x = at.x + half;
  const std::int32_t y = at.y + half;
  // Both of these are belt and braces and are kept knowingly: `Grid::cell`
  // answers 0 outside the grid on purpose -- "callers stamp overlapping grids
  // against each other and clipping at the edge is the useful behaviour" -- and
  // 0 is not water, so faults injected into either survive the whole suite. The
  // original writes the bounds test against the *world* rectangle before it
  // touches the layer, and this writes it against the layer because that is the
  // thing this engine has. Leaning on `Grid`'s clipping without saying so would
  // be leaning on a contract that belongs to another file.
  if (x < 0 || y < 0) return dry;
  const auto cx = static_cast<std::uint32_t>(x) / terrain.cell_size();
  const auto cy = static_cast<std::uint32_t>(y) / terrain.cell_size();
  if (cx >= terrain.width() || cy >= terrain.height()) return dry;
  return HostOutcome::ok_with(Value::boolean(terrain.cell(cx, cy) == kTerrainWater));
}

/// `Obj::CanSee(Obj)` -- 57 sites over 24 files, and **it is a stealth test and
/// nothing else**.
///
/// 0x005ab9d0, in its order:
///
///   1. **The argument is checked first**, and it is the *observed* object: a
///      miss prints *"Parameter #1 in function 'Obj::CanSee' is uninitialized
///      or invalid object."* and answers false.
///   2. An observed object without the hidden bit (bit 21 of `[obj+0x2c]`) is
///      seen: **true, without looking at the receiver at all**. No line of
///      sight, no radius, no terrain, no exploration state -- a unit that is
///      not hiding is visible from anywhere on the map, to anybody.
///   3. A hidden one is seen when the observer's player record word `+0x18`
///      shares a bit with the observed object's owner bits (the low 16 of its
///      sync flags). That word is filled by `SetRelation` (0x005652f0): when
///      player P's word toward Q carries bit 0x10 -- `Relation::share_view` --
///      P's one-hot bit is ORed into Q's `+0x18`. So a hidden object is seen
///      by an observer whose owner it **shares view with**:
///      `has(observed.owner, observer.owner, share_view)`.
///   4. The receiver is never checked: with a hidden argument the original
///      reads the receiver's record through a null object and faults. That is
///      a fault, not a behaviour, and answers false here.
///
/// **A player's own bit** is inferred to be in its own word: SetRelation
/// skips the diagonal (0x005653ae), and the record's initialiser was not
/// found. The other reader of the same word settles it in practice --
/// `RevealHiddenEnemyUnits` (sim/player_host.cpp) reveals the hidden objects
/// whose owner bit is *not* in it, which without the self bit would unmask a
/// player's own stealthed units -- so the same owner sees its own.
///
/// Every caster script calls it twice per target -- once as a validity gate
/// before approaching and again after `Sleep(.TimeToActionMoment())`,
/// immediately before the effect lands. This answered false for every hidden
/// object, and for a dead observer even when the target was not hiding, until
/// the host audit read the body.
HostOutcome m_can_see(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CanSee: no world");
  const WorldObject* seen = object_of(*world, ctx.arg(1));
  if (seen == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  if (!seen->state.flags.hidden) return HostOutcome::ok_with(Value::boolean(true));
  const WorldObject* observer = object_of(*world, ctx.arg(0));
  if (observer == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const PlayerId owner = seen->state.owner;
  const PlayerId viewer = observer->state.owner;
  if (owner == kNoPlayer || viewer == kNoPlayer) return HostOutcome::ok_with(Value::boolean(false));
  const bool shared =
      owner == viewer || world->players().has(owner, viewer, Relation::share_view);
  return HostOutcome::ok_with(Value::boolean(shared));
}

/// `SetMessengerStatus(bool)` -- 26 sites, registered on `Obj` and on `Query`.
///
/// Bit 26 of `[obj+0x2c]`, and see `ObjectFlags::messenger` for what reads it.
///
///   * `Obj::SetMessengerStatus` (0x005ab8d0): the handle check (a miss
///     prints and returns), then true sets the bit through `vtbl + 0x44` and
///     false clears it through `vtbl + 0x48` (0x0040a500/0x0040a520). No life
///     test: a corpse's bit is written too.
///   * `Query::SetMessengerStatus` (0x00579c70): the handle check, then an
///     empty result prints and does nothing; otherwise every member that
///     resolves, is a script object and **is not dead** (`vtbl + 0x50`,
///     `object_is_dead`) gets the same write.
///
/// One body for both, because this registry keys on (kind, name, arity).
HostOutcome m_set_messenger_status(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetMessengerStatus: no world");
  const bool on = ctx.count() > 1 && ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  const bool query = ctx.arg(0).is_object() && ctx.arg(0).as_object().type == kTypeQuery;
  for (const ObjectId id : receiver_objects(ctx, *world, ctx.arg(0))) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr || (query && object_is_dead(*slot))) continue;
    if (ObjectState* state = world->mutable_state(id); state != nullptr) {
      state->flags.messenger = on;
    }
  }
  return HostOutcome::ok_void();
}

// --------------------------------------------------------------------------
// members: the stonehenge
// --------------------------------------------------------------------------
//
// Two entry points over one scan, and the count is not what its name says.
//
// `StonehengeNumControllingMages` (`0x004dde80`) and `IsStonehengeControlable`
// (`0x004ddd70`) are byte-for-byte the same function but for two lines. Both
// gate on the receiver being a `CVXTownHall` whose class is an heir of
// `Stonehenge` (0x0059c020), both scan a circle of the building's **class
// `sight`** -- `[cls+0x2d0]`, which the class reader fills from the `sight`
// property, and not the instance radius `[obj+0xd0]` that `SetSight` moves --
// and both run the visitor at `0x004db580` over the object grid (0x004dc6f0,
// cells in row order, only objects carrying `kSyncUnit` -- the visitor's
// first word, `0x400000`).
//
// **The visitor's state is three words** -- that mask, a player record, a
// count -- and the two entry points differ only in the player word's seed: the
// counter seeds none, the predicate the local player's record
// (`[world+0x12c8]`, 0x004dddfa). Read in the disassembly; Ghidra's stack
// slots overlap the query's there.
//
// A receiver that does not resolve, or is not a stonehenge, prints one of two
// complaints into the discard sink and answers 0 or false.
//
// **The count is "mages of one alliance, provided nobody hostile is also
// here".** The visitor skips the dead and the spawn templates, then requires an
// heir of `Military` *or* `BaseMage` (0x00540130, 0x005401a0; anything else is
// passed over without a word); the first qualifying object it meets sets
// the reference player, and after that each one's diplomacy row against that
// reference decides:
//
//   * **bit 0 clear -- not even a ceasefire -- zeroes the count and aborts the
//     whole scan**. A single hostile soldier standing in the circle makes the
//     stonehenge answer nothing, and that is why `Military` non-mages are in
//     the predicate at all: they exist to spoil.
//   * **bit 5 clear -- no shared control -- skips that object** without
//     spoiling. So a friendly player's mages contribute only to a player it
//     shares control with, which is `Relation::share_control` here.
//   * only then, and only for a `BaseMage`, does the count go up.
//
// The two differ in the reference player and in what they push. The counter
// starts with **none**, so it adopts whoever is standing there: *how many mages
// does whichever single alliance is here have*. The predicate pins it to the
// commanding player: *can I control this right now*. That is why its five sites
// are the five `STONEHENGE_*_VERIFY.VS` scripts, each guarding with
// `Translate("Stonehenge isn't under your control")`.
//
// **`IsStonehengeControlable` therefore answers differently on different peers,
// and that is correct.** `HostContext::local_player` is deliberately outside the
// world for exactly this reason -- see `sim/host_context.hpp` -- and a command
// verify script is interface-side by construction: it decides whether *this*
// player's right click does anything. A headless run has no local player and
// the predicate answers false, which is the right answer to "can nobody control
// it".

/// Walk the mages in a stonehenge's circle. Returns the count, or 0 when the
/// scan aborted on a hostile.
///
/// `reference` is the player the row is read against, or `kNoPlayer` to adopt
/// the first qualifying object's owner -- which is the whole difference between
/// the two entry points.
///
/// **The order of the walk** is the grid's (0x004dc6f0): cells row by row and
/// each cell's list in the order its objects came into it
/// (`docs/formats/spatial-index.md`), and so is this one's. The predicate is
/// unaffected -- its reference is pinned -- but the counter adopts the *first*
/// qualifying owner, and `share_control` is one-directional, so with two
/// players' mages under a one-sided grant the order decides the count.
[[nodiscard]] std::int32_t stonehenge_mages(World& world, const WorldObject& henge,
                                            PlayerId reference) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return 0;
  const ClassFilter military = ClassFilter::parse("Military", graph);
  const ClassFilter mage = ClassFilter::parse("BaseMage", graph);
  if (military.match_all && mage.match_all) return 0;

  // The class record's sight (`[cls+0x2d0]`), not the instance's `[obj+0xd0]`
  // that `SetSight` moves: the two start equal, and only the class's is read.
  std::int32_t radius = 0;
  if (henge.class_index == kNoClass ||
      !parse_int(graph->property(henge.class_index, "sight"), radius)) {
    radius = 0;
  }
  std::vector<ObjectId> found;
  world.objects_in_radius(world.resolve_position(henge.id), radius, ClassFilter{}, found,
                          SweepOrder::grid);
  std::int32_t count = 0;
  for (const ObjectId id : found) {
    const WorldObject* other = world.find(id);
    if (other == nullptr || other->internal != InternalKind::none) continue;
    if (other->state.health <= 0) continue;
    // The template test the visitor makes (`[obj+0x2c] & 0x08000000`) is not
    // repeated here: `World::collect` makes it for every spatial query, and its
    // own comment says why -- "making it once is making it everywhere". A copy
    // here would be a second place for one rule to live.
    const bool is_mage = !mage.match_all && world.matches_filter(*other, mage);
    const bool is_military = !military.match_all && world.matches_filter(*other, military);
    if (!is_mage && !is_military) continue;

    if (reference == kNoPlayer) {
      reference = other->state.owner;
    } else {
      // Not even a ceasefire: the count is zeroed and the scan stops. One
      // hostile soldier is enough.
      if (world.players().is_enemy(other->state.owner, reference)) return 0;
      // Friendly but not sharing control: skipped, without spoiling.
      if (!world.players().has(other->state.owner, reference, Relation::share_control)) {
        continue;
      }
    }
    if (is_mage) ++count;
  }
  return count;
}

/// The receiver, when it is a stonehenge.
[[nodiscard]] const WorldObject* stonehenge_receiver(CallContext& ctx, World& world) {
  const WorldObject* slot = object_of(world, ctx.arg(0));
  if (slot == nullptr) return nullptr;
  // `class_is`, not a second copy of it: a name the graph cannot resolve must
  // read as false rather than as "matches everything", and that rule is written
  // once at the top of this file. A hand-rolled copy here was the first thing a
  // fault survived, because its own `match_all` guard is unreachable --
  // `ClassFilter::parse` leaves `match_all` false with count 0 for a name it
  // could not resolve, and says so where it does it.
  return class_is(world, *slot, "Stonehenge") ? slot : nullptr;
}

/// `StonehengeNumControllingMages()` -- 18 hits, all `STONEHENGE_IDLE.VS`.
///
/// That script is a charging loop: with *n* mages in range it adds a point of
/// stamina every second up to 100, and sleeps `12500 / n - 250` extra when *n*
/// is under fifty -- so the count sets the charge rate and a wrong zero simply
/// never charges.
HostOutcome m_stonehenge_mages(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("StonehengeNumControllingMages: no world");
  const WorldObject* henge = stonehenge_receiver(ctx, *world);
  // Both a bad receiver and one that is not a stonehenge answer 0, each after a
  // complaint the retail sink discards.
  if (henge == nullptr) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(stonehenge_mages(*world, *henge, kNoPlayer)));
}

/// `IsStonehengeControlable()` -- 5 sites, the five spell verify scripts.
HostOutcome m_stonehenge_controlable(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsStonehengeControlable: no world");
  const WorldObject* henge = stonehenge_receiver(ctx, *world);
  if (henge == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  HostContext* context = host_context_of(ctx);
  const PlayerId who = context == nullptr ? kNoPlayer : context->local_player;
  // A headless run has no local player, and "can nobody control it" is false.
  if (who == kNoPlayer) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(
      Value::boolean(stonehenge_mages(*world, *henge, who) > 0));
}

// --------------------------------------------------------------------------
// members: the ship's AI transport
// --------------------------------------------------------------------------
//
// A `CVXShip` carries a pending transport order as a `std::string` at
// `[ship+0x20c]` and its destination as the point at `[ship+0x228]`/`[+0x22c]`,
// constructed empty and `(-1, -1)`. `Ship::HasAiTransport` (0x005c7190) is
// `str != ""`; `Ship::ClearAiTransport` (0x005c7e10) assigns `""` and puts the
// point back to `(-1, -1)`; `Ship::GetTransPt` hands the point over. The writer
// is the free `PrepareAiTransportShip/5`, whose **single shipped call site** is
// `DATA\AI\AIOSENDSQUAD.VS:82`:
//
//     ship = PrepareAiTransportShip( l.Cur.GAIKAIn.LSA, g.LSA, l.Cur.Player, cmd, pt );
//
// **This section used to answer three constants, and the note beside them said
// the justification would expire.** It has. The writer has a body
// (`sim/squad.cpp`), the pair it writes is `AiSystem::ShipTransport`, and these
// three read it. The old note is worth keeping in outline because the reasoning
// was sound while it lasted: the values were *derived* rather than guessed --
// no reachable path could change the field, so its value was the one it was
// born with -- which is a different thing from `GAIKACount`, where a plausible
// answer would have been an invention. A derivation that rests on "nothing
// reaches this" is exactly the kind that expires, and this is what paying it
// looks like.
//
// What it unblocked then, and still does, is `SHIP_IDLE.VS` -- every ship's
// behaviour script, 60 hits a pass. Its `if (.HasAiTransport)` now opens for a
// ship the AI has tasked and stays shut for every other, which is the shape the
// script was written around.

/// The ship's pending order, or the constructed state for a ship with none.
[[nodiscard]] const AiSystem::ShipTransport& transport_of(World& world, const script::Value& value) {
  static const AiSystem::ShipTransport kNone{};
  const WorldObject* ship = object_of(world, value);
  const AiSystem* ai = ai_system_of(world);
  if (ship == nullptr || ai == nullptr) return kNone;
  return ai->ship_transport(ship->id);
}

/// `Ship::HasAiTransport` -- `str != ""`, which is 0x005c7190's whole body.
HostOutcome m_has_ai_transport(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("HasAiTransport: no world");
  return HostOutcome::ok_with(Value::boolean(!transport_of(*world, ctx.arg(0)).order.empty()));
}

/// `Ship::ClearAiTransport` -- a no-op, and **the one entry point in its family
/// that says nothing at all about a bad receiver**: 0x005c7e10 branches straight
/// to its return without even formatting a complaint, where its two siblings
/// format one into the sink that is a bare `ret`.
HostOutcome m_clear_ai_transport(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClearAiTransport: no world");
  const WorldObject* ship = object_of(*world, ctx.arg(0));
  AiSystem* ai = ai_system_of(*world);
  if (ship != nullptr && ai != nullptr) ai->clear_ship_transport(ship->id);
  return HostOutcome::ok_void();
}

/// `Ship::GetTransPt` -- the destination, and `(-1, -1)` for a ship with no
/// order. That sentinel is also `kHeldPosition`, and the coincidence is worth
/// naming: they are two different meanings of one pair of numbers, and nothing
/// confuses them because a real transport point is a place on the map.
HostOutcome m_get_trans_pt(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetTransPt: no world");
  return HostOutcome::ok_with(pack_point(transport_of(*world, ctx.arg(0)).where));
}

/// `Ship::ApplyAiTransport()` -- 1 site, the last line of
/// `SHIP_AITRANSPORT.VS`: the ship has crossed, and the crossing's order is
/// handed to the armies it carried.
///
/// 0x005c8710 resolves the receiver -- an invalid one returns without a word,
/// as `ClearAiTransport` does -- and then, in order:
///
///   1. **snapshots the passengers**: the ship's holder's unit list
///      (`[ship+0x1dc]`, `+0x28`) copied into a local (0x00438330), before
///      anything moves;
///   2. **lands them**: 0x005c84b0, which is `UnboardAllUnits`'s body -- every
///      passenger onto a random free land point nearby, in the holder's order,
///      until the points run out -- and `unboard_all` here;
///   3. **finds their squads**: 0x00442880 walks the snapshot, takes each unit
///      that is one (the class test at 0x0044290e), asks its squad
///      (0x00444190) and appends the squad's handle to a deque **once**, in
///      order of first appearance (0x0043f570 before 0x00437870). An earlier
///      note here had this as a string parse; it takes the list;
///   4. **orders each squad on**: 0x0043ec00, the core all three
///      `Squad::SetCmd` overloads share, with the squad's **own current
///      state**, no flags set, `SF_ADVCHOOSER` cleared, and the crossing's
///      verb at its point -- the `cmd` and `pt` `PrepareAiTransportShip` was
///      handed, `AiSystem::ShipTransport` here. So a squad that boarded as
///      `advance` to a beach is told `advance` to that beach again from the
///      far shore, and stops choosing its own route;
///   5. **clears the order**: `""` and `(-1, -1)`, which is what `SHIP_IDLE.VS`
///      reads on its next pass.
///
/// The verb reaches each member through `squad_set_cmd`, which is what
/// `SetCmd` itself runs; a class binding no such verb gets no order, which
/// is the command system's standing rule. Step 4's state write restamps
/// `StateTime` with the same state, as the core does for every caller.
///
/// **This body used to do step 5 alone, and said so** -- the note called it
/// a debt narrowed rather than paid, because the squads are ordered
/// `boardship` by `AIOSENDSQUAD.VS` two statements after the ship is asked
/// for, so the landing order was a second route to a dispatch that had
/// already happened. It was, and it is also the only route to the *far
/// shore*: a `boardship` order ends aboard, and without step 4 an army that
/// has crossed stands on the beach with `SF_ADVCHOOSER` still set and no
/// order at all.
HostOutcome m_apply_ai_transport(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ApplyAiTransport: no world");
  const WorldObject* ship = object_of(*world, ctx.arg(0));
  AiSystem* ai = ai_system_of(*world);
  if (ship == nullptr || ai == nullptr) return HostOutcome::ok_void();
  const ObjectId ship_id = ship->id;
  // The order is read before the clear, and the passengers before the drop.
  const AiSystem::ShipTransport order = ai->ship_transport(ship_id);
  const std::vector<ObjectId> aboard = passengers_of(*world, ship_id);
  (void)unboard_all(*world, ship_id);

  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) {
    std::vector<SquadKey> landed;
    for (const ObjectId unit : aboard) {
      const WorldObject* slot = world->find(unit);
      if (slot == nullptr || !slot->state.flags.is_unit) continue;
      const SquadKey key = heroes->squads().squad_of(unit);
      if (!key.valid()) continue;
      if (std::find(landed.begin(), landed.end(), key) != landed.end()) continue;
      landed.push_back(key);
    }
    Command prototype;
    prototype.arg_kind = CommandArgKind::point;
    prototype.point = order.where;
    for (const SquadKey key : landed) {
      // Re-found per squad: `squad_set_cmd` runs scripts, and a script can
      // create a squad, which can move the table under a pointer.
      Squad* squad = heroes->squads().find(key);
      if (squad == nullptr) continue;
      squad_set_cmd(*world, *squad, squad->state, 0, kSquadFlagAdvChooser, order.order, prototype,
                    world->time());
    }
  }
  ai->clear_ship_transport(ship_id);
  return HostOutcome::ok_void();
}

// --------------------------------------------------------------------------
// members: the gate
// --------------------------------------------------------------------------
//
// Five declared entry points, and `DATA\SUBAI\GATE_IDLE.VS` is the whole of
// their corpus. That file is every gate's behaviour script and it is eleven
// lines:
//
//     while (1) {
//       .LookAround(350);
//       if (.AreEnemiesAround())      .CloseNow();
//       else if (.AreFriendsAround()) .OpenNow();
//       else                          .CloseNow();
//       Sleep(500);
//     }
//
// -- twice a second, rescan a 350-unit neighbourhood, close on any enemy, open
// for friends alone, close for an empty street. `LookAround` was 224 hits in
// the corpus sweep, second only to the druid's, because that loop never stops.
//
// `Gate::IsOpened` and `Gate::IsClosed` are registered in `gbr.exe`
// (`0x005299f0`, `0x00529a70`) and are deliberately **not** bound: neither has
// a call site anywhere in the installation, which is the rectangle family's
// rule. Their bodies are read and recorded here for the day one appears --
// `IsOpened` is `target == 1 && position == 0x46`, `IsClosed` is
// `target == 0 && position == 0`, and the pair disagrees about a bad receiver
// in a way worth keeping: `IsOpened` answers **true** and `IsClosed` false, so
// a broken handle reads as an open gate.
//
// ## Two fields of the original's five, and why
//
// A gate carries a target state at `[gate+0x208]`, a raised position 0..70 at
// `[gate+0x20c]`, a transition latch at `[gate+0x228]`, a building mode at
// `[gate+0x204]` and the two scan booleans. `OpenNow`/`CloseNow` write only the
// target and clear the latch; the motion is a 2,000 ms interpolation the gate's
// own tick performs, flipping passability as the position crosses 20 and
// playing `"GateOpen"` / `"GateClose"` when the transition starts.
//
// **The position and the passability are modelled; the sounds are not.**
// `sim/gate.hpp` carries the position's motion -- the portcullis layer is
// drawn raised by it -- and the barrier it decides: the map's obstruction
// layer leaves every gate's passage open, an enemy's route search lays the
// gate's line across it unless the gate stands fully open, and a route's step
// stops before a gate that bars its mover (0x00418260, 0x00529300) -- see
// `sim/gate.hpp`, "The barrier". Since that decides where units walk, the
// motion is hashed and saved. The latch is not a field: whether the gate lets
// units through is read off the motion at the turn's time.
//
// ## Neither `OpenNow` nor `CloseNow` suspends
//
// Both are registered through the **suspending** registrar `0x00699eb0`, and
// both `xor eax,eax; ret` on every path -- the "did not suspend" code. That is
// worth an explicit note because the registrar is otherwise a reliable signal
// and this project has already been caught once by ignoring it: a `PlayAnim`
// that returned spins `ANIM.VS`, and a `CloseNow` that suspended would stall
// `GATE_IDLE.VS` for two seconds a call.

/// The scan `Gate::LookAround(radius)` performs.
///
/// **It walks units, and nothing else.** The body (0x00529bb0) hands the map's
/// grid walk (0x005296d0) a mask of `0x400000` (0x00529c09), which is
/// `kSyncUnit`, bit 22 of the word at `[obj+0x2c]`: the walk skips a cell whose
/// mask lacks it (0x005297f4) and an object that lacks it (0x00529820,
/// 0x0052986a) before the visitor is called. So a gate's own walls, towers and
/// neighbouring buildings are never seen -- they are not units -- and the
/// scan used to see them: over every object in 350 units, anything not an
/// enemy was a friend, a gate always had its own walls in range, and it never
/// closed (playtest #16). A unit inside a building is not on the map's grid
/// either (`kHeldPosition`, `sim/economy.hpp`), so a tower's garrison beside
/// a gate is not a friend at it; here the spatial query finds held objects at
/// their holder and they are passed over by hand.
///
/// `0x00529510` is the visitor, and each of its clauses is here:
///
///   * skip the dead (`vtbl[0x50]`);
///   * skip an heir of `PeasantAmbient`, and an heir of `Animal` **that does
///     no damage** -- `[obj+0xdc]`, the instance field the object's persist
///     table (0x005b5df0) names `damage` and `Obj::damage` reads: a deer is
///     passed over, a wolf at the gate is somebody;
///   * skip a class whose `ignore_passability` property is set;
///   * **friend** when bit 0 of the gate's own diplomacy row for that owner is
///     set, which is `PlayerTable::is_enemy` negated and is not the transpose;
///   * **enemy** otherwise, but only when the object is not hidden: a stealthed
///     enemy is passed over entirely rather than counted either way.
///
/// Both fields are set to the literal 1 rather than incremented, so they are
/// booleans in the original too and `!= 0` loses nothing.
///
/// `[obj+0xdc]`, the object's own damage: the combatant's, when the combat
/// system holds the object, which is what `Obj::damage` answers; otherwise the
/// class's `damage`, which is what the object was built with.
[[nodiscard]] std::int32_t object_damage(World& world, const WorldObject& slot) {
  if (const CombatSystem* combat = combat_system_of(world); combat != nullptr) {
    if (const Combatant* unit = combat->find(slot.id); unit != nullptr) {
      return combat->attack_of(*unit);
    }
  }
  return class_int(world, slot, "damage");
}

void gate_look_around(World& world, const WorldObject& gate, std::int32_t radius) {
  ObjectState* state = world.mutable_state(gate.id);
  if (state == nullptr) return;
  state->flags.enemies_near = false;
  state->flags.friends_near = false;

  const ClassGraph* graph = world.class_graph();
  std::vector<ObjectId> found;
  world.objects_in_radius(world.resolve_position(gate.id), radius, ClassFilter{}, found);
  for (const ObjectId id : found) {
    const WorldObject* other = world.find(id);
    if (other == nullptr) continue;
    // The walk's mask: units only, and only those on the map.
    if (!other->state.flags.is_unit || other->state.is_held()) continue;
    if (other->state.health <= 0) continue;
    if (graph != nullptr && other->class_index != kNoClass) {
      const std::string_view ignores = graph->property(other->class_index, "ignore_passability");
      if (!ignores.empty() && ignores != "0") continue;
      if (class_is(world, *other, "Animal") && object_damage(world, *other) == 0) continue;
      if (class_is(world, *other, "PeasantAmbient")) continue;
    }
    if (!world.players().is_enemy(state->owner, other->state.owner)) {
      state->flags.friends_near = true;
      continue;
    }
    // A hidden enemy is passed over rather than counted, which is the one place
    // the two halves are not symmetric.
    if (!other->state.flags.hidden) state->flags.enemies_near = true;
  }
}

HostOutcome m_look_around(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("LookAround: no world");
  const WorldObject* gate = object_of(*world, ctx.arg(0));
  // An unresolvable receiver leaves both fields alone -- it does not even zero
  // them -- and says nothing.
  if (gate == nullptr) return HostOutcome::ok_void();
  gate_look_around(*world, *gate, ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0);
  return HostOutcome::ok_void();
}

/// `Gate::AreEnemiesAround` / `AreFriendsAround` -- `!= 0` on the two fields
/// `LookAround` writes, and **stale until it runs**. Both answer false for a
/// receiver that does not resolve.
template <bool ObjectFlags::*Field>
HostOutcome m_gate_saw(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("gate: no world");
  const WorldObject* gate = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::boolean(gate != nullptr && gate->state.flags.*Field));
}

/// `Gate::OpenNow` / `CloseNow` -- write the target and return. Neither
/// suspends; see the section note.
///
/// `0x00529c50` (open) and `0x00529cd0` (close): an unresolvable receiver
/// prints *"called for an uninitialized or invalid object"* into the discard
/// sink. Otherwise, **only when the gate's building mode `[gate+0x204]` is not
/// 3 and its target `[gate+0x208]` is not already the one asked for**, the
/// target is written (1 open, 0 closed) and the move latch `[gate+0x228]` is
/// cleared, which makes the gate's tick start a move from where the portcullis
/// stands. A call that repeats the current target changes nothing -- not the
/// target, not the latch, not the move in progress. No class test on the
/// receiver: a non-gate's fields at those offsets would be written.
template <bool kOpen>
HostOutcome m_gate_set(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("gate: no world");
  const WorldObject* gate = object_of(*world, ctx.arg(0));
  if (gate == nullptr) return HostOutcome::ok_void();
  WorldObject* slot = world->find(gate->id);
  if (slot == nullptr) return HostOutcome::ok_void();
  // The original refuses while the gate's building mode is 3 -- destroyed --
  // and this engine has no building mode; a destroyed gate here is one whose
  // health has reached zero, which is the nearest thing it can test and is
  // named as an inference rather than a transcription.
  if (slot->state.health <= 0) return HostOutcome::ok_void();
  // Already the target: nothing is written and the move in progress runs on.
  if (slot->state.flags.gate_open == kOpen) return HostOutcome::ok_void();
  // The target, and the portcullis's move towards it from wherever it stands
  // now -- the latch the original clears here restarts that move on its next
  // tick. Where the move has got to decides the barrier; see `sim/gate.hpp`.
  gate_retarget(*slot, kOpen, world->time());
  return HostOutcome::ok_void();
}

// --------------------------------------------------------------------------
// members: Erase
// --------------------------------------------------------------------------
//
// `Erase/0`, 47 call sites, and the one entry point in the object model that
// takes an object out of the world without killing it.
//
// ## Two registrations, one body, and no `ObjList`
//
// `gbr.exe` registers it exactly twice -- `Obj::Erase` (0x005ab250, arg type
// 0x14) and `Query::Erase` (0x00578c40, arg type 0x20), both void, both through
// the *ordinary* registrar, so it does not suspend. There is no
// `ObjList::Erase` string in the image at all. This registry keys on
// (kind, name, arity), so one body serves both and dispatches on the receiver.
//
// ## Erase is not death
//
// The two paths share exactly one routine, `0x005141b0`. Death runs
// `Damage -> SetHealth -> IsDead -> vtbl[0xB0]`, which scores a penalty against
// the holding settlement, drives health to zero and sets the dead marker at
// `[obj+0x130]` -- and **leaves the object allocated**, as a corpse with its
// handle intact. Erase enters at `vtbl[0x94]`, never touches health, never sets
// that marker, never calls the death virtual, and ends at the scalar deleting
// destructor. No attacker is passed on either path, so no kill credit exists to
// give; nothing in the erase call graph drops loot. `GHOST_SAC_IDLE.VS` uses
// both verbs in adjacent lines -- it *damages* the ghost to death and *erases*
// the sacrifice -- which is the corpus stating the distinction outright.
//
// ## What an erase does here
//
// Two things, and the second is the one that would be missed.
//
// **The object leaves the world.** `World::despawn` erases the slot and drops
// every group membership, which is what `0x005141b0`'s control-group sweep and
// the two back-reference fixups (`0x004f9640`, `0x004f96b0`) amount to. Named
// bindings are deliberately left alone, for the reason `despawn` gives.
//
// **Every coroutine the object owns stops.** `0x00687c80` walks the object's
// pending scheduled-message array and cancels each one, and a cancelled message
// is a `Sleep` that never wakes. That is not a detail: it is what ends
// `HEROGRAVE_BEHAVIOR.VS`, whose last two statements are `.Erase();` and
// `Sleep(100000);` -- the sleep schedules a wake-up, the erase cancels it, and
// the script is simply never resumed. `CATAPULT_IDLE.VS` ends its loop the same
// way. An implementation that despawned the object and left its scripts running
// would leave both of them polling a dead handle forever, and both would look
// correct in a test that only checked the object was gone.
//
// ## Deferral, and why it is per-step rather than per-pass
//
// `Erase` on the object whose script is running destroys nothing: it sets the
// latch at `[0x009bdb14]` and returns. The runner (`0x0069f7a0`) calls the
// object's `vtbl[0x24]` after the bytecode returns, and *that* performs the
// erase. Six of the 47 sites are a self-erase with nothing after it; two --
// `CATAPULT_IDLE.VS:108` and `HEROGRAVE_BEHAVIOR.VS:107` -- have a `Sleep`
// after it and depend on the order.
//
// See `script::Scheduler::StepHook`. A pass hook would be wrong: the deferral
// is over before the next coroutine runs, so a pass hook would let every other
// script in the same turn observe an object the original had already destroyed.
//
// ## The third thing an erase does, and what is still left out
//
// **The `"ondie"` class hook**, which this paragraph used to say was omitted.
// `0x005141b0` looks the class's `ondie` script up (`0x0059b060`) and launches
// it, and `Military` -- the base of every soldier -- binds it to
// `DATA\SUBAI\MILITARY_ONDIE.VS`, the Warrior Tales research payout. It was
// omitted because `0x005141b0` is shared: the hook belongs to the **death**
// path as much as to this one, and this engine's death path is
// `CombatSystem::advance`, which has a world and no scheduler -- so firing it
// from `Erase` alone would have put a mechanism on one of its two callers.
// `sim/hooks.hpp` is the seam that closes that, and `perform_erase` fires it
// below on the same terms `CombatSystem::enter_dying` does.
//
// **Holder eviction.** `0x0052fc20`, the holder family's override, evicts its
// contents (`0x52fb00`) before erasing -- and skips that when the deferral latch
// is set, so a holder that erases itself from its own script orphans what it
// holds and one erased from outside does not. Nothing in the corpus erases a
// holder: holders are the settlement's internal sub-objects and a ship's
// composite half, and all 47 sites name a unit, a sacrifice, a wagon, a grave,
// a ship or a query.
//
// Two more of the original's differences between an immediate and a deferred
// erase are unobservable here rather than omitted, and that is worth saying
// plainly. The unit override skips `RemoveFromHolder` and `SetAlive(false)` when
// the latch is set; here the object is gone from the world either way, its
// holder link is a field on the object that went with it, and nothing reads the
// liveness of an id that no longer resolves.

/// Take one object out of the world, now.
///
/// `ctx.scheduler` may be null -- a bare `Vm` run has none -- in which case the
/// coroutines simply are not there to stop.
void perform_erase(script::Scheduler* scheduler, World& world, ObjectId id) {
  // The `ondie` hook, first, because that is where `0x005141b0` puts it: ahead
  // of the control-group sweep and both back-reference fixups.
  //
  // **A hook script is detached, and the sweep below is why that is not a
  // detail.** `0x006a0360` passes the object's handle as the script's first
  // *argument* and binds no owner, so a hook is a peer of the object rather
  // than one of its coroutines. Spawning it as the object's own -- the shape
  // `start_object_scripts` uses for `idle` -- would have the loop three lines
  // down kill the script this line just started, on this path and only on this
  // path, which is a subsystem that is complete and unreachable in the exact
  // way this project keeps finding.
  //
  // What the hook sees is the second half of the same fact. **The hook runs
  // here, to completion, before anything below** -- `0x006a0360` hands the
  // script to an interpreter run that returns only when the script has (see
  // `sim/hooks.hpp` and `script::Scheduler::call`). So it sees the object
  // whole: still in the world, still in its hero's army and its squad. This
  // comment used to say the opposite -- that the hook ran on a later pass
  // against a handle that no longer resolved, and that the original did the
  // same -- and both halves were wrong. `CMERCENARY_ONDIE.VS` pays its gold to
  // `.hero`, which only works if the hero is still there to be read.
  (void)fire_class_hook(world, ClassHook::on_die, id);
  // Then the detaches 0x005db230 runs after 0x005141b0: out of the hero's army
  // (a hero's own army first), out of the squad. A corpse that already went
  // through them at death finds nothing, which is the original's second pass
  // over the same routines.
  if (HeroSystem* heroes = hero_system_of(world); heroes != nullptr) heroes->on_erase(world, id);
  // And out of any settlement's holder, whose roster would otherwise go on
  // counting an object that is gone.
  (void)garrison_forget(world, id);
  if (scheduler != nullptr) {
    // Every coroutine this object owns. `kill` is safe on the script that is
    // running right now, which is exactly the deferred case. The record vector
    // is copied out first: `kill` marks rather than erases, but a hook that
    // spawned would move it, and this is the cheaper invariant to state.
    std::vector<script::ScriptId> owned;
    for (const script::ScriptRecord& record : scheduler->scripts()) {
      if (record.dead) continue;
      if (record.owner.type != kTypeObj || record.owner.id != id) continue;
      owned.push_back(record.id);
    }
    for (const script::ScriptId id_to_kill : owned) scheduler->kill(id_to_kill);
  }
  world.despawn(id);
}

/// The object the running script belongs to, or `kNoObject`.
///
/// `[0x00a77ebc]` in the original, written by the runner at the top of every
/// script step and cleared the instant the bytecode returns.
[[nodiscard]] ObjectId running_script_object(CallContext& ctx) noexcept {
  if (ctx.scheduler == nullptr || ctx.script == script::kNoScript) return kNoObject;
  const script::ScriptRecord* record = ctx.scheduler->find(ctx.script);
  if (record == nullptr || record->owner.type != kTypeObj) return kNoObject;
  return record->owner.id;
}

void erase_one(CallContext& ctx, World& world, ObjectId id) {
  if (id != kNoObject && id == running_script_object(ctx)) {
    world.defer_erase(id);
    return;
  }
  perform_erase(ctx.scheduler, world, id);
}

HostOutcome m_erase(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Erase: no world");
  // `Obj::Erase` (0x005ab250) is the handle check and then the object's
  // `vtbl + 0x94`, its destroy. A receiver that resolves to nothing is a
  // no-op, not a refusal: both bodies format a complaint into `0x00686eb0`,
  // which is a bare `ret` in retail. A named object that has died resolves to
  // nothing (`object_of`), so `NO_X.Erase()` on one erases no corpse.
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();

  if (slot->internal != InternalKind::query) {
    erase_one(ctx, *world, slot->id);
    return HostOutcome::ok_void();
  }

  // The query form. `0x00578c40` reads the query's list, walks it, and skips
  // any element carrying bit 23 of `[obj+0x2c]` -- which `Obj::AsBuilding`
  // (0x005aa5e0) tests and which this engine keeps as `ObjectFlags::is_building`
  // -- as a `continue` and not an abort. So a query holding six units and a
  // wall loses the six.
  //
  // It also reads before it purges, where every other `Query` member purges
  // first: the list accessor (`vtbl + 0x20`, refreshed when due) and only then
  // `vtbl + 0x1c`. So a stale list hands the walk its dead too, which the
  // building test above lets through to `erase_one`.
  std::vector<ObjectId> found;
  world->read_query(slot->id, script_clock(ctx, *world), found, World::QueryRead::as_cached);
  for (const ObjectId id : found) {
    const WorldObject* target = world->find(id);
    if (target == nullptr || target->state.flags.is_building) continue;
    erase_one(ctx, *world, id);
  }
  world->purge_query(slot->id);
  // And the query itself survives. `0x00578c40` releases one reference on it and
  // erases nothing.
  return HostOutcome::ok_void();
}

// --------------------------------------------------------------------------
// members: queries
// --------------------------------------------------------------------------

// `count/0` used to live here, answering for a `Query` receiver and refusing an
// `ObjList` one. It now lives in sim/objlist.cpp, which answers for both: the
// registry keys on (kind, name, arity) and there is exactly one `count/0`, so
// the two receivers cannot be split across two domains without one of them
// silently replacing the other. `register_objlist_host` must therefore run
// after this function -- sim/host_setup.hpp's manifest is where that is fixed.

/// `IsEmpty/0` -- two bodies, and neither traps.
///
///   * `Query::IsEmpty` (0x00579320): the handle check -- a miss prints and
///     answers **true** -- then the query's refresh (`vtbl + 0x1c`, which for
///     a group or named query purges the dead) and its emptiness test
///     (`vtbl + 0x28`, count == 0; a sight query refreshes first if stale).
///     Here that is `World::purge_query` and `World::query_is_empty`: the
///     cached list while it is fresh, and once due a refresh -- or, for the
///     area, sight, player-flags and party kinds, a sweep that only counts.
///   * `Sacrifice::IsEmpty` (0x00594eb0): the handle check -- a miss answers
///     true -- then the stop flag at `[sacrifice+0x15c]`
///     (`ObjectFlags::sacrifice_stopped`): zero from the constructor
///     (0x00595290) and written only by the druid-list prune at the head of
///     `Consume`'s core (0x00595150), as "no druid is left". So a sacrifice
///     answers false until its first `Consume`, and after that whether that
///     `Consume` found its list empty -- which, with no druid able to join one
///     here, it always does (`m_consume`). The two share this registration
///     because the registry keys on (kind, name, arity); an `Obj` receiver
///     that is not a query is the sacrifice form.
///
/// Both used to trap, which stopped a script on a handle whose object had
/// gone where the original answers "empty" and goes on.
HostOutcome m_query_is_empty(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsEmpty: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::boolean(true));
  if (slot->internal != InternalKind::query) {
    return HostOutcome::ok_with(Value::boolean(slot->state.flags.sacrifice_stopped));
  }
  world->purge_query(slot->id);
  return HostOutcome::ok_with(
      Value::boolean(world->query_is_empty(slot->id, script_clock(ctx, *world))));
}

// --------------------------------------------------------------------------
// members: point
// --------------------------------------------------------------------------

/// `pt.x` and `pt.y` -- four bodies over two names. `point::x` (0x006952b0) and
/// `point::y` (0x006952e0) take the point by value and answer its first or
/// second dword. `point::x`/`rect::left` (0x006950e0) and `point::y`/`rect::top`
/// (0x00695290) are the by-reference forms -- the receiver reference itself, or
/// it at `+4` -- that let `pt.x = ...` and `rc.top = ...` assign through them.
/// Here a member answers the read, which is the same number; no shipped script
/// assigns through `.x`, `.y`, `.left`, `.top`, `.right` or `.bottom`.
HostOutcome m_point_x(CallContext& ctx) {
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("x: receiver is not a point");
  return HostOutcome::ok_with(Value::integer(unpack_point(ctx.arg(0)).x));
}

HostOutcome m_point_y(CallContext& ctx) {
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("y: receiver is not a point");
  return HostOutcome::ok_with(Value::integer(unpack_point(ctx.arg(0)).y));
}

/// `pt.Set(x, y)` (0x00697c50) -- registered `point, point& , int, int`: it
/// stores `y` and then `x` into the referenced point **and answers the new
/// point**. The store is the argument window's copy-back (the VM copies
/// argument 0 into the caller's local); every one of the 106 shipped sites is a
/// bare statement, so the answer was never read -- and was nothing here until
/// the host audit.
///
/// `rect::Set` (0x00697aa0) is a different entry point that the ledger joins to
/// this name: `void, rect&, int, int, int, int`, arity **four**, storing
/// left, top, right and bottom. No shipped site calls it and it is not bound,
/// as this file does for every registered name the corpus never reaches.
HostOutcome m_point_set(CallContext& ctx) {
  if (!ctx.arg(1).is_integer() || !ctx.arg(2).is_integer()) {
    return HostOutcome::failed("Set: expected two integers");
  }
  const Value point = pack_point(Point{ctx.arg(1).as_integer(), ctx.arg(2).as_integer()});
  ctx.out(0) = point;
  return HostOutcome::ok_with(point);
}

// -- rectangles -------------------------------------------------------------
//
// A `rect` is script type code 0x08 in `gbr.exe` and a bare 16-byte POD on the
// operand stack, `{left, top, right, bottom}` at offsets 0/4/8/0xC -- read off
// `rect::left` (0x00695140), `top` (0x0069519a), `right` (0x006951e0) and
// `bottom` (0x00695221), each of which pops exactly 0x10 bytes. Here it is an
// index into `World::rects()`, because four `int32`s do not fit in a
// `script::Value`.
//
// **Bounds are inclusive on all four edges**, which four independent readings
// agree on: `rect::width` is `right - left + 1` (0x00696c73), `point::InRect`
// uses `jl` on the low edges and `jg` on the high ones (0x00696faf), `IntoRect`
// clamps only when `x > right` (0x00697cef), and the map rectangle is built as
// `(0, 0, w - 1, h - 1)` (0x00541f25), which only covers the map if the high
// edges are included.

/// `GetMapRect()` -- the whole map, in world units, inclusive.
///
/// 0x004c6650 is a verbatim four-dword copy from `[world + 0xB4]`, and the map
/// constructor writes those four at 0x00541eef..0x00541f25 as `(0, 0, w-1,
/// h-1)`. World units, not cells: `IsPointInWater` (0x005c6d40) compares a
/// script `Pos` against the same four fields before dividing down to a terrain
/// index, so the rectangle and a position are the same coordinate space.
HostOutcome fn_get_map_rect(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetMapRect: no world");
  MatchSystem* match = match_system_of(*world);
  if (match == nullptr) return HostOutcome::failed("GetMapRect: no match system");
  const std::int32_t extent = match->rules().map_size;
  const std::int32_t high = extent > 0 ? extent - 1 : 0;
  return HostOutcome::ok_with(pack_rect(*world, RectTable::Rect{0, 0, high, high}));
}

/// `pt.IntoRect(rc)` -- clamps the receiver into the rectangle, in place.
///
/// 0x00697cb0, and the order of the four comparisons is the whole of it: the
/// low edge is applied first and the high edge second, per axis, with **no
/// normalisation of the rectangle itself**. On a reversed rectangle the second
/// clamp therefore wins and every point lands on `(right, bottom)`.
///
/// That is transcribed rather than chosen, and it is worth saying that the
/// executable does not agree with itself here: `IntersectRects` (0x00696cd0)
/// *manufactures* reversed rectangles from disjoint inputs with no emptiness
/// check, `AddRects` (0x0048c5dd) treats a reversed rectangle as empty, and
/// this treats it as neither. No shipped rectangle is reversed -- all 29 `rect`
/// declarations in the corpus are `GetMapRect()` -- so nothing exercises the
/// disagreement, and reproducing this one exactly costs nothing.
HostOutcome m_point_into_rect(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IntoRect: no world");
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("IntoRect: receiver is not a point");
  const RectTable::Rect* r = unpack_rect(*world, ctx.arg(1));
  if (r == nullptr) return HostOutcome::failed("IntoRect: expected a rect");
  Point p = unpack_point(ctx.arg(0));
  if (p.x < r->left) p.x = r->left;
  if (p.x > r->right) p.x = r->right;
  if (p.y < r->top) p.y = r->top;
  if (p.y > r->bottom) p.y = r->bottom;
  ctx.out(0) = pack_point(p);
  return HostOutcome::ok_void();
}

/// `pt.InRect(rc)` -- 0x00696f80, inclusive on all four edges.
HostOutcome m_point_in_rect(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("InRect: no world");
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("InRect: receiver is not a point");
  const RectTable::Rect* r = unpack_rect(*world, ctx.arg(1));
  if (r == nullptr) return HostOutcome::failed("InRect: expected a rect");
  const Point p = unpack_point(ctx.arg(0));
  const bool inside = p.x >= r->left && p.x <= r->right && p.y >= r->top && p.y <= r->bottom;
  return HostOutcome::ok_with(Value::integer(inside ? 1 : 0));
}

/// `pt.ClampToMap()` -- 0x005bd3e0, and **not** `IntoRect(GetMapRect())`.
///
/// Three deliberate differences, all in eight instructions: it clamps to
/// `right - 1` and `bottom - 1` rather than to `right` and `bottom`; its low
/// bound is the literal 0 rather than the rectangle's own `left`/`top`; and it
/// takes its receiver by value and **returns a new point** (`ptGoto =
/// ptGoto.ClampToMap;` is the shipped spelling) instead of mutating in place.
HostOutcome m_point_clamp_to_map(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClampToMap: no world");
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("ClampToMap: receiver is not a point");
  Point p = unpack_point(ctx.arg(0));
  MatchSystem* match = match_system_of(*world);
  // 0x005bd3f2: with no map at all the point comes back unchanged.
  if (match == nullptr) return HostOutcome::ok_with(pack_point(p));
  const std::int32_t extent = match->rules().map_size;
  const std::int32_t high = extent > 1 ? extent - 2 : 0;
  if (p.x < 0) p.x = 0;
  if (p.x > high) p.x = high;
  if (p.y < 0) p.y = 0;
  if (p.y > high) p.y = high;
  return HostOutcome::ok_with(pack_point(p));
}

/// The four edges, and the two extents that are one more than a difference.
///
/// A rect is four ints on the VM stack -- left, top, right, bottom. The
/// by-value bodies (`left` 0x00695140, `top` 0x00695180, `right` 0x006951c0,
/// `bottom` 0x00695200, `width` 0x00696c50, `height` 0x00696c90) push one int.
/// `gbr.exe` registers a second form of each edge over a rect *reference*
/// (`right` 0x00695100 and `bottom` 0x00695120 among them) that answers a
/// reference to the field, so `rc.right = x` would write through it; no
/// shipped script assigns an edge, every one of the sites reads, and a read of
/// either form is the same number.
HostOutcome m_rect_edge(CallContext& ctx, int which) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("rect: no world");
  const RectTable::Rect* r = unpack_rect(*world, ctx.arg(0));
  if (r == nullptr) return HostOutcome::failed("rect: receiver is not a rect");
  switch (which) {
    case 0: return HostOutcome::ok_with(Value::integer(r->left));
    case 1: return HostOutcome::ok_with(Value::integer(r->top));
    case 2: return HostOutcome::ok_with(Value::integer(r->right));
    case 3: return HostOutcome::ok_with(Value::integer(r->bottom));
    // `right - left + 1`, off 0x00696c73. The `+ 1` is the inclusive bound
    // showing through, and it is what makes `width` on the map rectangle answer
    // the extent rather than one less than it.
    case 4: return HostOutcome::ok_with(Value::integer(r->right - r->left + 1));
    default: return HostOutcome::ok_with(Value::integer(r->bottom - r->top + 1));
  }
}

HostOutcome m_rect_left(CallContext& ctx) { return m_rect_edge(ctx, 0); }
HostOutcome m_rect_top(CallContext& ctx) { return m_rect_edge(ctx, 1); }
HostOutcome m_rect_right(CallContext& ctx) { return m_rect_edge(ctx, 2); }
HostOutcome m_rect_bottom(CallContext& ctx) { return m_rect_edge(ctx, 3); }
HostOutcome m_rect_width(CallContext& ctx) { return m_rect_edge(ctx, 4); }
HostOutcome m_rect_height(CallContext& ctx) { return m_rect_edge(ctx, 5); }

/// `rc.Center` -- 0x00697ba0, `((l+r)/2, (t+b)/2)` with C division truncating
/// toward zero. The same idiom `AreaCenter` uses at 0x004d8ce9, derived
/// independently of it.
///
/// **Served by `m_gaika_center` (`sim/ai.cpp`) since the tenth round**, which
/// answers this rule for a `rect` receiver now that the GAIKA half exists; this
/// body stays as the record of how it was left unbound until then.
///
/// **Deliberately not registered.** `rect::Center` has *zero* call sites in the
/// installation, and every shipped `.Center` is on a GAIKA (`g.Center`,
/// `sq.AIDest.Center`). Binding this body to the name would answer all of them
/// with "receiver is not a rect" -- a refusal that claims the entry point
/// exists -- where they trap today as `host_not_implemented`, which is both
/// true and the thing `declare_shipped_surface` exists to guarantee. It is kept
/// here, unbound, because the reading is done and the day a GAIKA `Center`
/// lands this is the other half of it.
///
/// **This was nearly bound anyway.** `Center/0` sits at 23 sites in the
/// coverage ranking with no body, which is exactly the shape of a cheap win --
/// and the ranking cannot see which *receiver* those 23 sites use. The note
/// above is what stopped it, which is the whole reason a decision like this is
/// written down beside the code rather than in a commit message.
[[maybe_unused]] HostOutcome m_rect_center(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Center: no world");
  const RectTable::Rect* r = unpack_rect(*world, ctx.arg(0));
  if (r == nullptr) return HostOutcome::failed("Center: receiver is not a rect");
  return HostOutcome::ok_with(pack_point(Point{(r->left + r->right) / 2, (r->top + r->bottom) / 2}));
}

// -- rotation ---------------------------------------------------------------
//
// `point::Rot(degrees)`, body 0x00697d90, 385 shipped sites. **The original is
// x87 floating point** -- the degrees loaded as an integer, multiplied by the
// double at 0x7ac490, the FPU's sine and cosine of the product, and two calls
// to the MSVC `_ftol2` truncation helper at 0x00763870 -- and the constant it
// multiplies by is 0.017453292519444445, which is not `pi/180` but
// `3.1415926535/180`, pi truncated to eleven digits. This core has no floating
// point and will not acquire any, so the reproduction is integer, and exact.
//
// The rotation is
//
//     x' = trunc(x*cos(a) + y*sin(a))
//     y' = trunc(y*cos(a) - x*sin(a))            a = degrees * K
//
// which is the **transpose** of the textbook counter-clockwise matrix, so
// `Rot(90)` sends `(x, y)` toward `(y, -x)`. `sim/path.cpp`'s `rotate_degrees`
// uses the other handedness and rounds half-away-from-zero; it is not reachable
// from any host entry point and its movement callers are not disturbed here,
// but the two conventions now coexist and that is recorded rather than hidden.
//
// ## Reproduced exactly, in integers
//
// The FPU's values are not a mystery to be approximated. The angle is
// `d * K` rounded to 53 bits (the multiply runs under the default precision
// control); the FPU's sine and cosine of it keep the x87's 64-bit
// significand, precision control not applying to them; each product with an integer
// coordinate is rounded to 53 bits, so is the sum, and `_ftol2` truncates
// toward zero. `tools/gen_rot_table.py` computes the two 64-bit values for
// every whole degree from 0 to 360 offline, in exact rational arithmetic,
// into `rot_table.inc`; the three roundings and the truncation are done here
// on integers (`X87Number`). docs/formats/point-rot.md.
//
// That is what makes `Rot(90)` biased -- `cos(90 * K)` is 4.4896592e-11, so
// the product of a coordinate under 32,768 with it survives the 53-bit sum and
// decides the truncation by its sign -- and it is why `Rot(30)` of `(0, 2)` is
// `(0, 1)`: `sin(30 * K)` is a hair under one half. Both fall out of the
// arithmetic rather than being special cases.
//
// **Labelled.** The table assumes the FPU's sine and cosine return the correctly
// rounded value of the true function at the rounded angle; the x87's 66-bit
// pi moves the two near 180 and 360 degrees by about 1e-20, which no
// truncation here can see. A negative angle uses `sin(-a) = -sin(a)` and
// `cos(-a) = cos(a)`, which the FPU keeps exactly. Beyond a full turn the
// angle is reduced to within one, where the original hands the raw product to
// the FPU; no shipped site is outside [-30, 360].

/// One x87 value as the table stores it: sign * significand * 2^exponent.
struct X87Table {
  std::int8_t sign;
  std::uint64_t significand;
  std::int16_t exponent;
};

struct X87SinCos {
  X87Table sin;
  X87Table cos;
};

constexpr X87SinCos kRotTable[361] = {
#include "rot_table.inc"
};

__extension__ typedef unsigned __int128 U128;

/// An exact binary number, sign * magnitude * 2^exponent, with the roundings
/// the FPU applies. After `round53` the magnitude is either zero or exactly 53
/// bits wide, so the exponent is the weight of its last bit.
struct X87Number {
  std::int32_t sign = 0;
  U128 magnitude = 0;
  std::int32_t exponent = 0;
};

[[nodiscard]] std::int32_t bit_length(U128 v) noexcept {
  const auto high = static_cast<std::uint64_t>(v >> 64);
  if (high != 0) return 128 - std::countl_zero(high);
  return 64 - std::countl_zero(static_cast<std::uint64_t>(v));
}

/// Round to a 53-bit significand, nearest-even, and normalise to exactly 53.
[[nodiscard]] X87Number round53(X87Number v) noexcept {
  if (v.magnitude == 0) return X87Number{};
  const std::int32_t length = bit_length(v.magnitude);
  if (length > 53) {
    const std::int32_t shift = length - 53;
    U128 keep = v.magnitude >> shift;
    const U128 rest = v.magnitude - (keep << shift);
    const U128 half = U128{1} << (shift - 1);
    if (rest > half || (rest == half && (keep & 1) != 0)) ++keep;
    v.exponent += shift;
    if (keep == (U128{1} << 53)) {
      keep >>= 1;
      ++v.exponent;
    }
    v.magnitude = keep;
  } else if (length < 53) {
    v.magnitude <<= (53 - length);
    v.exponent -= 53 - length;
  }
  return v;
}

/// An integer coordinate times a table value, rounded to 53 bits as the FPU's
/// multiply rounds it under the default precision control.
[[nodiscard]] X87Number x87_mul(std::int64_t x, const X87Table& t) noexcept {
  if (x == 0 || t.sign == 0) return X87Number{};
  X87Number out;
  out.sign = (x < 0 ? -1 : 1) * t.sign;
  out.magnitude = static_cast<U128>(x < 0 ? -x : x) * t.significand;
  out.exponent = t.exponent;
  return round53(out);
}

/// `a + b` (`b` negated first when `subtract`), rounded to 53 bits as the FPU's
/// add rounds it.
/// Both operands are `round53`'s output.
[[nodiscard]] X87Number x87_add(X87Number a, X87Number b, bool subtract) noexcept {
  if (subtract) b.sign = -b.sign;
  if (b.sign == 0) return a;
  if (a.sign == 0) return b;
  if (a.exponent < b.exponent) std::swap(a, b);
  const std::int32_t shift = a.exponent - b.exponent;
  // Past 72 bits `b` is under a quarter of `a`'s last bit and cannot move the
  // rounding, and shifting further would not fit.
  if (shift > 72) return a;
  const U128 big = a.magnitude << shift;
  X87Number out;
  out.exponent = b.exponent;
  if (a.sign == b.sign) {
    out.sign = a.sign;
    out.magnitude = big + b.magnitude;
  } else if (big >= b.magnitude) {
    out.sign = a.sign;
    out.magnitude = big - b.magnitude;
  } else {
    out.sign = b.sign;
    out.magnitude = b.magnitude - big;
  }
  return round53(out);
}

/// `_ftol2` (0x00763870): truncate toward zero; outside `int32` the integer
/// indefinite, 0x80000000.
[[nodiscard]] std::int32_t x87_truncate(const X87Number& v) noexcept {
  constexpr std::int32_t kIndefinite = std::numeric_limits<std::int32_t>::min();
  if (v.sign == 0) return 0;
  U128 whole = 0;
  if (v.exponent >= 0) {
    if (bit_length(v.magnitude) + v.exponent > 32) return kIndefinite;
    whole = v.magnitude << v.exponent;
  } else {
    whole = v.exponent <= -128 ? U128{0} : v.magnitude >> (-v.exponent);
  }
  if (v.sign > 0) {
    if (whole > static_cast<U128>(std::numeric_limits<std::int32_t>::max())) return kIndefinite;
    return static_cast<std::int32_t>(whole);
  }
  if (whole > (U128{1} << 31)) return kIndefinite;
  return static_cast<std::int32_t>(-static_cast<std::int64_t>(whole));
}

}  // namespace

void erase_object(CallContext& ctx, World& world, ObjectId id) { erase_one(ctx, world, id); }

std::int32_t object_max_health(World& world, const WorldObject& slot) {
  return max_health_of_object(world, slot);
}

bool object_has_special(World& world, ObjectId id, std::int32_t index) {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || index < 0 || index >= kUnitSpecialsCount) return false;
  if (index == kSpecialFreedom && slot->state.flags.is_hero) return true;
  return class_has_special(world, *slot, index);
}

bool unit_has_freedom(World& world, ObjectId id) {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr) return false;
  // Bit 30 of the specials word is `freedom` (the constant is 30), and the
  // word is the class's `unit_specials`; see `class_has_special`.
  if (class_has_special(world, *slot, 30)) return true;
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return false;
  const ClassIndex ram = graph->find("RamUnit");
  return ram != kNoClass && world.class_is_a(id, ram);
}

std::string_view catapult_class_for_race(std::int32_t race) noexcept {
  static constexpr std::string_view kByRace[] = {"GCatapult", "RCatapult", "CCatapult",
                                                 "ICatapult", "RCatapult", "BCatapult",
                                                 "ECatapult", "TCatapult"};
  // The upper bound guards a read past the table that no test can see
  // without a sanitiser; the original has the same eight-way table and
  // answers null past it.
  if (race < 0 || race >= static_cast<std::int32_t>(std::size(kByRace))) return {};
  return kByRace[race];
}

ObjectId place_catapult(World& world, ClassIndex class_index, Point at, std::int32_t player) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || class_index == kNoClass) return kNoObject;
  const Result<NativeClass> native = native_class_from_name(graph->at(class_index).cpp_class);
  if (!native) return kNoObject;

  const HostOutcome placed = place_object(world, class_index, *native, at, player);
  if (placed.status != script::HostStatus::ok || !placed.value.is_object() ||
      placed.value.as_object().type != kTypeObj) {
    return kNoObject;
  }
  const ObjectId id = placed.value.as_object().id;
  if (id == kNoObject) return kNoObject;
  world.set_health(id, 1);

  std::int32_t build_sight = 0;
  if (EnvSystem* env = env_of(world); env != nullptr) {
    (void)env->constant("CatapultBuildSight", build_sight);
  }
  // A sight of 0 stamps nothing -- `explore_circle` marks cell centres within
  // the radius, and a site is never exactly on one -- so the test is an
  // equivalence a fault survives; kept because it says what a missing value
  // means.
  if (build_sight > 0) {
    if (FogSystem* fog = fog_system_of(world); fog != nullptr) {
      fog->map().explore_circle(at, build_sight, fog_slot_from_script(player));
    }
  }

  // The settlement. `is_central_building = 1` on `Catapult` is what makes a
  // placed engine own one in the original -- `[cat+0x148]` is valid on every
  // catapult `ClearTowerTarget` and `AttackWait` read, and `GetHolderSett` on
  // a crewman reaches the engine through it. The map loader builds these
  // records for the buildings a scenario ships with; an engine placed mid-game
  // gets its own here, from the class's properties the way the loader reads
  // them, with the engine as anchor and as the first member of the roll. The
  // holder's `max_units` -- 10 on every shipped catapult -- is the crew cap.
  if (EconomySystem* economy = economy_of(world); economy != nullptr) {
    const WorldObject* slot = world.find(id);
    if (slot != nullptr && slot->settlement == kNoObject) {
      SettlementInit init;
      init.anchor = id;
      init.owner = slot->state.owner;
      init.kind = SettlementKind::other;
      init.can_be_attacked = class_flag(world, *slot, "can_be_attacked");
      init.can_be_captured = class_flag(world, *slot, "can_be_captured");
      init.anchor_max_health = class_int(world, *slot, "maxhealth");
      init.max_units = class_int(world, *slot, "max_units");
      const std::string_view capture = graph->property(class_index, "capture_health_percent");
      init.capture_health_percent = capture.empty() ? 100 : class_int(world, *slot, "capture_health_percent");
      const SettlementId town = economy->create(world, init);
      if (town != kNoSettlement) (void)economy->add_building(world, town, id, init.anchor_max_health);
    }
  }
  // The last thing 0x004e3be0 does: `1` into the player's env scope under
  // `CatapultBuilt` (0x004e3d7a..0x004e3da1, `/%s/Player%d/%s` with the
  // script's 1-based number). `BUILDINGSADVICE5.VS` waits on exactly that.
  if (EnvSystem* env = env_of(world); env != nullptr) {
    env->env().write_int(EnvScope::for_player(player), "CatapultBuilt", 1);
  }
  return id;
}

Point rotate_like_gbr(Point v, std::int32_t degrees) noexcept {
  // Within a turn the table holds the FPU's own values; 360 is its own row
  // (`sin(360 * K)` is -1.8e-10, not 0). Beyond a turn: reduced, labelled.
  std::int32_t d = degrees;
  if (d > 360 || d < -360) d %= 360;
  const bool negative = d < 0;
  const X87SinCos& row = kRotTable[negative ? -d : d];
  X87Table sine = row.sin;
  if (negative) sine.sign = static_cast<std::int8_t>(-sine.sign);
  const X87Table& cosine = row.cos;
  const std::int64_t x = v.x;
  const std::int64_t y = v.y;
  // x' = x cos + y sin, y' = y cos - x sin: each product rounded, then the sum.
  const X87Number nx = x87_add(x87_mul(x, cosine), x87_mul(y, sine), false);
  const X87Number ny = x87_add(x87_mul(y, cosine), x87_mul(x, sine), true);
  return Point{x87_truncate(nx), x87_truncate(ny)};
}

namespace {

/// The length of `(x, y)` as the point bodies compute it: each component
/// squared with a 32-bit `imul`, the two added in 32 bits (0x00695373..
/// 0x0069537d), and the sum handed to the square root at 0x0067c3d0 -- a
/// table-seeded estimate with Newton steps that is exactly the floor root of
/// the sum **read as unsigned**. So the arithmetic wraps where a 64-bit one
/// would not: `(70000, 0)` has length 24597. Every map coordinate is far
/// inside the range where the two agree.
[[nodiscard]] std::int32_t wrapped_length(std::int32_t x, std::int32_t y) noexcept {
  const std::uint32_t ux = static_cast<std::uint32_t>(x);
  const std::uint32_t uy = static_cast<std::uint32_t>(y);
  const std::uint32_t sum = ux * ux + uy * uy;
  return static_cast<std::int32_t>(isqrt(static_cast<std::int64_t>(sum)));
}

/// `pt.SetLen(length)` -- rescale the receiver to `length`, in place.
///
/// The other half of every `Rot` idiom: 73 shipped sites, and the shape is
/// always `pos.SetLen(n); pos.Rot(a); p = pos + .pos;`. Body 0x00697d10, and
/// unlike `Rot` it is **entirely integer**: `isqrt(x*x + y*y)` then `x*L/len`
/// and `y*L/len` with `idivl`, which truncates toward zero (0x00697d66 and
/// 0x00697d74).
///
/// **The zero vector does not stay zero.** 0x00697d5d tests the length and
/// 0x00697d81 takes a branch that writes the requested length into *y* and zero
/// into *x*, so a direction-less vector is given one, pointing north. `sim/path.hpp`
/// states the opposite -- "a zero vector has no direction and stays zero, the
/// caller has to notice" -- and cites `DEER_IDLE.VS` calling `.GetDir` first as
/// the evidence. The reasoning is sound and the conclusion is wrong: the
/// original picks a direction. `path.cpp`'s `set_length` is left alone here
/// because its callers are in movement and changing it would move every map's
/// world hash; that it now disagrees with this is recorded rather than hidden.
HostOutcome m_point_set_len(CallContext& ctx) {
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("SetLen: receiver is not a point");
  if (!ctx.arg(1).is_integer()) return HostOutcome::failed("SetLen: expected a length");
  const Point v = unpack_point(ctx.arg(0));
  const std::int32_t length = ctx.arg(1).as_integer();
  const std::int32_t current = wrapped_length(v.x, v.y);
  if (current == 0) {
    ctx.out(0) = pack_point(Point{0, length});
    return HostOutcome::ok_void();
  }
  // `x * L` with a 32-bit `imul`, then `idiv` by the length: the product
  // wraps before the division, as it does there.
  const auto scaled = [&](std::int32_t c) {
    const auto product = static_cast<std::int32_t>(static_cast<std::uint32_t>(c) *
                                                   static_cast<std::uint32_t>(length));
    return static_cast<std::int32_t>(product / current);
  };
  ctx.out(0) = pack_point(Point{scaled(v.x), scaled(v.y)});
  return HostOutcome::ok_void();
}

/// `pt.Rot(degrees)` -- rotates the receiver in place and returns nothing.
///
/// In place is not an inference: the entry point is registered with the
/// by-reference receiver type 0x106 and the body stores through the resolved
/// pointer at 0x00697dfd and 0x00697e01, and all 385 shipped sites are bare
/// statements that use the receiver afterwards.
HostOutcome m_point_rot(CallContext& ctx) {
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("Rot: receiver is not a point");
  if (!ctx.arg(1).is_integer()) return HostOutcome::failed("Rot: expected an angle");
  ctx.out(0) = pack_point(rotate_like_gbr(unpack_point(ctx.arg(0)), ctx.arg(1).as_integer()));
  return HostOutcome::ok_void();
}

/// `pt.Len` -- the vector's Euclidean length, floored. Body 0x00695360.
///
/// Pure integer in the original too: `imul` each component by itself, add, and
/// hand the sum to the table-driven square root at 0x0067c3d0 -- the same
/// routine `AreaDistTo`'s circle branch uses, and semantically `isqrt`. Ten of
/// its eleven shipped sites are written **without parentheses** (`pt.SetLen(pt
/// .Len + 30)`), which is why the entry point is arity 0 and why a coverage
/// tool matching on `Name(` undercounts it by ten.
HostOutcome m_point_len(CallContext& ctx) {
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("Len: receiver is not a point");
  const Point v = unpack_point(ctx.arg(0));
  return HostOutcome::ok_with(Value::integer(wrapped_length(v.x, v.y)));
}

/// `a.Dist(b)` and `Dist(a, b)` -- the distance between two points, floored.
///
/// **One body, registered twice**: 0x00695310 is both the member (0x006990f3)
/// and the free function (0x00699171), with identical argument types, so the
/// two spellings cannot disagree and the corpus uses both for the same job.
/// The metric is **Euclidean**, which is worth saying because the neighbouring
/// `AreaShape::distance_to` is Chebyshev for a rectangle -- the two share the
/// square root and nothing else.
HostOutcome fn_point_dist(CallContext& ctx) {
  if (!is_point(ctx.arg(0)) || !is_point(ctx.arg(1))) {
    return HostOutcome::failed("Dist: expected two points");
  }
  const Point a = unpack_point(ctx.arg(0));
  const Point b = unpack_point(ctx.arg(1));
  // The difference in 32 bits too, then the same wrapped length.
  const auto dx = static_cast<std::int32_t>(static_cast<std::uint32_t>(a.x) -
                                            static_cast<std::uint32_t>(b.x));
  const auto dy = static_cast<std::int32_t>(static_cast<std::uint32_t>(a.y) -
                                            static_cast<std::uint32_t>(b.y));
  return HostOutcome::ok_with(Value::integer(wrapped_length(dx, dy)));
}

/// `Point(x, y)`. Body 0x00696e50, and it is fifteen instructions with no
/// branch in them: no clamp, no validity marker, no `(0, 0)` fixup.
///
/// The last of those matters. `BARRACK_TRAIN.VS` passes `Point(0, 0)` to
/// `Place` on purpose, and the `(0,0)` -> `y = 1` adjustment lives downstream
/// in the position setter (0x0053f60e), so a constructor that "helpfully"
/// fixed it here would make that adjustment unreachable.
HostOutcome fn_point_make(CallContext& ctx) {
  if (!ctx.arg(0).is_integer() || !ctx.arg(1).is_integer()) {
    return HostOutcome::failed("Point: expected two integers");
  }
  return HostOutcome::ok_with(pack_point(Point{ctx.arg(0).as_integer(), ctx.arg(1).as_integer()}));
}

}  // namespace

// --------------------------------------------------------------------------
// points
// --------------------------------------------------------------------------

bool is_receiver(World& world, const script::Value& receiver, bool accept_query) noexcept {
  if (is_objlist(receiver)) return true;
  const WorldObject* slot = object_of(world, receiver);
  if (slot == nullptr) return false;
  return accept_query || slot->internal != InternalKind::query;
}

bool receiver_departed(World& world, const script::Value& receiver) noexcept {
  if (!receiver.is_object() || is_objlist(receiver)) return false;
  // The invalid handle, and the handle types `object_of` resolves: a point,
  // a rectangle or an array is a different shape, not a departed one.
  const std::uint32_t type = receiver.as_object().type;
  if (type != script::kNoType && type != kTypeObj && type != kTypeNamedObj &&
      type != kTypeQuery && type != kTypeSettlement) {
    return false;
  }
  return object_of(world, receiver) == nullptr;
}

std::size_t reap_departed(script::Scheduler& scheduler, World& world) {
  std::vector<script::ScriptId> gone;
  std::vector<ObjectId> owners;
  // A corpse counts as departed for its own scripts. See the header.
  const CombatSystem* combat = combat_system_of(world);
  for (const script::ScriptRecord& record : scheduler.scripts()) {
    if (record.dead || record.owner.type != kTypeObj) continue;
    if (world.find(record.owner.id) != nullptr &&
        (combat == nullptr || !combat->is_dying(record.owner.id))) {
      continue;
    }
    gone.push_back(record.id);
    owners.push_back(record.owner.id);
  }
  for (const script::ScriptId id : gone) scheduler.kill(id);
  // The garrisons, for the same reason and on the same terms: a corpse or a
  // departed object is off every roster (0x00531a80 is the holder removal
  // whatever took the unit away), so `UnitsCount` stops counting it --
  // `OUTPOST_BEHAVIOR.VS` hands an outpost over only at zero. Settlement
  // order, then roster order: both are state.
  if (EconomySystem* economy = economy_of(world); economy != nullptr) {
    std::vector<ObjectId> departed;
    for (const Settlement& s : economy->settlements().all()) {
      for (const ObjectId member : s.holder.units) {
        if (world.find(member) != nullptr && (combat == nullptr || !combat->is_dying(member))) {
          continue;
        }
        departed.push_back(member);
      }
    }
    for (const ObjectId member : departed) (void)garrison_forget(world, member);
  }
  if (CommandSystem* commands = command_system(world); commands != nullptr) {
    for (const ObjectId id : owners) commands->forget(id);
    // And the queues of the departed that owned no script at all.
    (void)commands->forget_departed(world);
  }
  return gone.size();
}

std::size_t start_behaviors(script::Scheduler& scheduler, ScriptLibrary& library,
                            const World& world, ObjectId id) {
  const WorldObject* slot = world.find(id);
  const ClassGraph* graph = world.class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) return 0;
  const script::ObjectRef owner{kTypeObj, id};
  const script::Value args[] = {script::Value::object(owner)};
  std::size_t started = 0;
  for (const std::string_view path : graph->resolved_behaviors(slot->class_index)) {
    const std::uint32_t chunk = library.chunk_for(path);
    if (chunk == script::kNoChunk) continue;
    if (scheduler.spawn(chunk, args, owner) != script::kNoScript) ++started;
  }
  return started;
}

std::size_t stop_behaviors(script::Scheduler& scheduler, ScriptLibrary& library,
                           const World& world, ObjectId id, ClassIndex class_index) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || class_index == kNoClass) return 0;
  std::vector<std::uint32_t> chunks;
  for (const std::string_view path : graph->resolved_behaviors(class_index)) {
    const std::uint32_t chunk = library.chunk_for(path);
    if (chunk != script::kNoChunk) chunks.push_back(chunk);
  }
  std::vector<script::ScriptId> doomed;
  for (const script::ScriptRecord& record : scheduler.scripts()) {
    if (record.dead || record.parent != script::kNoScript) continue;
    if (record.owner.type != kTypeObj || record.owner.id != id) continue;
    if (std::find(chunks.begin(), chunks.end(), record.chunk_index) == chunks.end()) continue;
    doomed.push_back(record.id);
  }
  for (const script::ScriptId script_id : doomed) scheduler.kill(script_id);
  return doomed.size();
}

GameTime script_clock(const script::CallContext& ctx, const World& world) noexcept {
  return ctx.scheduler != nullptr ? ctx.scheduler->now() : world.time();
}

namespace {

std::vector<ObjectId> receiver_objects_at(World& world, const script::Value& receiver,
                                          bool accept_query, GameTime now);

}  // namespace

std::vector<ObjectId> receiver_objects(script::CallContext& ctx, World& world,
                                      const script::Value& receiver, bool accept_query) {
  return receiver_objects_at(world, receiver, accept_query, script_clock(ctx, world));
}

std::vector<ObjectId> receiver_objects(World& world, const script::Value& receiver,
                                      bool accept_query) {
  return receiver_objects_at(world, receiver, accept_query, world.time());
}

namespace {

std::vector<ObjectId> receiver_objects_at(World& world, const script::Value& receiver,
                                          bool accept_query, GameTime now) {
  std::vector<ObjectId> ids;
  if (is_objlist(receiver)) {
    const std::span<const ObjectId> items = objlist_pool_of(world).items(objlist_of(receiver));
    ids.assign(items.begin(), items.end());
    return ids;
  }
  // `object_of` is what resolves a `NamedObj` through the name table, by the
  // registered `NamedObj -> Obj` conversion documented at the top of this file.
  // A dead binding resolves to nothing, which is what the original's
  // `CVXNamedObjQuery` does too: it goes *empty* rather than invalid.
  const WorldObject* slot = object_of(world, receiver);
  if (slot == nullptr) return ids;
  if (slot->internal == InternalKind::query) {
    // The `Query` registrations' opening pair: purge, then the list accessor,
    // which refreshes only when due (`World::read_query`).
    if (accept_query) world.read_query(slot->id, now, ids);
    return ids;
  }
  ids.push_back(slot->id);
  return ids;
}

}  // namespace

void install_deferred_erase(script::Scheduler& scheduler) noexcept {
  scheduler.set_step_hook([](void* user, script::Scheduler& sched, script::ScriptId ran) {
    auto* context = static_cast<HostContext*>(user);
    if (context == nullptr || context->world == nullptr) return;
    World& world = *context->world;
    // **A unit a script killed loses its scripts now**, not at the next
    // turn's reap. The dumps' dying objects all have an empty method, so the
    // original's death has stopped the unit's behaviour by the time it lies
    // there; waiting for the turn boundary let a corpse's own scripts resume
    // later in the same pass. `SENTRY_GUARD.VS`'s inner loop is where it
    // showed: a sentry its wall put down (`Damage(10000)` when the wall is
    // very broken or taken) still had a live, valid target in range, its
    // `Attack` was refused because it was dying, and a refused `Attack` does
    // not suspend (0x005ddbd2), so the loop ran its instruction budget out.
    if (CombatSystem* combat = combat_system_of(world);
        combat != nullptr && combat->has_fresh_deaths()) {
      combat->forget_fresh_deaths();
      (void)reap_departed(sched, world);
    }
    const ObjectId pending = world.deferred_erase();
    if (pending != kNoObject) {
      // Cleared before the erase, not after. The original clears after
      // (0x005a7ad3) because its overrides read the latch to decide what to
      // skip; here nothing reads it, and clearing first makes a re-entrant
      // erase impossible rather than merely unlikely.
      perform_erase(&sched, world, pending);
      world.clear_deferred_erase();
    }
    // 0x0069f7a0's order: the owner's deferred action (`vtbl+0x24`, the erase
    // above) first, and then the thread's end (`vtbl+0x1c`): a command whose
    // script just ended starts the next one here, before any other script
    // runs. An erased or reaped object's queue is gone with it, so
    // `after_slice` then finds nothing, as `vtbl+0x24` answering 1 skips it.
    if (CommandSystem* commands = command_system(world)) commands->after_slice(world, ran);
  });
}

script::Value pack_point(Point p) noexcept {
  return Value::object(
      ObjectRef{kTypePoint, static_cast<std::uint32_t>(p.x), static_cast<std::uint32_t>(p.y)});
}

script::Value pack_rect(World& world, const RectTable::Rect& r) {
  return Value::object(ObjectRef{kTypeRect, world.rects().intern(r)});
}

bool is_rect(const script::Value& value) noexcept {
  return value.is_object() && value.as_object().type == kTypeRect;
}

const RectTable::Rect* unpack_rect(const World& world, const script::Value& value) noexcept {
  if (!is_rect(value)) return nullptr;
  return world.rects().find(value.as_object().id);
}

Point unpack_point(const script::Value& value) noexcept {
  if (!is_point(value)) return Point{};
  const ObjectRef ref = value.as_object();
  return Point{static_cast<std::int32_t>(ref.id), static_cast<std::int32_t>(ref.aux)};
}

bool is_point(const script::Value& value) noexcept {
  return value.is_object() && value.as_object().type == kTypePoint;
}

std::int64_t isqrt(std::int64_t value) noexcept {
  if (value <= 0) return 0;
  // Newton's method on integers, converging from above. Exact: the loop leaves
  // the largest r with r*r <= value, and it terminates because the sequence is
  // strictly decreasing until it does. No floating point, and therefore the
  // same answer on every compiler and architecture -- which is the whole point.
  std::int64_t r = value;
  std::int64_t next = (r + 1) / 2;
  while (next < r) {
    r = next;
    next = (r + value / r) / 2;
  }
  return r;
}

// --------------------------------------------------------------------------
// WorldHost
// --------------------------------------------------------------------------

script::Value WorldHost::default_value(std::string_view type_name) {
  // `point` starts at the origin; every handle type starts invalid, which is
  // what 331 files that declare a handle and assign it later depend on.
  if (type_name == "point") return pack_point(Point{});
  if (type_name == "int" || type_name == "bool") return Value::integer(0);
  if (type_name == "str") return Value::string(std::string());
  if (type_name == "Query") return Value::object(ObjectRef{script::kNoType, 0});
  if (type_name == "Settlement") return Value::object(ObjectRef{script::kNoType, 0});
  if (type_name == "Obj" || type_name == "Unit" || type_name == "Building" ||
      type_name == "Hero" || type_name == "Druid" || type_name == "Wagon" ||
      type_name == "Catapult" || type_name == "Ship" || type_name == "Barrack" ||
      type_name == "Gate" || type_name == "Flying" || type_name == "Sacrifice" ||
      type_name == "ItemHolder" || type_name == "Tower" || type_name == "Teleport" ||
      type_name == "Squad") {
    // `Squad` is a handle like the rest -- `sim/squad.hpp` gives it its own
    // `kTypeSquad` -- and an unbound one is the dumps' `0(0)`.
    return Value::object(ObjectRef{script::kNoType, 0});
  }
  // **`GAIKA` is a plain integer, not a handle**, so its default is `0` and it
  // carries no `TypeId` at all. `sim/gaika.hpp` records the sources; the two
  // that settle it are that `gbr.exe` declares the same operation with each
  // type side by side (`SetMAIKA: void, GAIKA gSrc, ...` beside `DumpMAIKA:
  // void, int Src, ...`) and that three corpus sites *order*-compare one,
  // `sq.AIDest > 0`, which an opaque handle does not support.
  if (type_name == "GAIKA") return Value::integer(0);
  // `rect rc;` is 29 of the corpus's declarations and 28 of them are assigned
  // from `GetMapRect()` on the next line, so the default is barely observable
  // -- but it has to exist, and an all-zero rectangle is what the original's
  // zero-initialised 16 bytes hold. It interns like any other, so the empty
  // rectangle costs one table entry however many scripts declare one.
  if (type_name == "rect") return pack_rect(*world_, RectTable::Rect{});
  // `ObjList` is deliberately absent: it needs the declaration site and is
  // answered by the two-argument overload below. Reaching this one with an
  // `ObjList` means someone asked for a default with no site to key it on, and
  // refusing says so rather than handing back a list nothing owns.
  //
  // `Item` -- another domain's. Refusing is the designed behaviour.
  // `SquadList`, `IntArray` and `StrArray` are answered by the two-argument
  // overload below, for the same reason `ObjList` is: they need the declaration
  // site to key an entry on. Reaching *this* one with any of them means someone
  // asked for a default with no site, and refusing says so.
  return script::Host::default_value(type_name);
}

script::Value WorldHost::default_value(std::string_view type_name, DeclarationSite site) {
  // `ObjList ol;` binds a pool entry here and now, keyed by the declaration
  // site rather than by this execution of it. `TS_CARTHAGETACTIC.VS` declares
  // one inside a loop body three times over; keying by the site means that
  // scope re-entry clears and reuses the one entry, which is exactly what the
  // declaration means to the script and what bounds the pool by the program
  // text instead of by how long the tactic runs.
  if (type_name == "ObjList") {
    return make_objlist_value(objlist_pool_of(*world_).acquire(site.script, site.slot));
  }
  // And the two script-side arrays, keyed the same way and for the same
  // reason: `TS_CARTHAGETACTIC.VS` declares one inside a loop body too.
  if (type_name == "IntArray" || type_name == "StrArray") {
    const bool strings = type_name == "StrArray";
    const script::TypeId type = strings ? kTypeStrArray : kTypeIntArray;
    return make_array_value(type, world_->arrays().acquire(site.script, site.slot, strings));
  }
  // And `SquadList SL;`, which every script in `DATA\AI` that walks the AI's
  // squads opens with. Keyed on the site like the rest: `GS_GUARD.VS` declares
  // one outside its `while (666)` and refills it every pass, so one entry
  // serves the whole run of the script rather than one per iteration.
  if (type_name == "SquadList") {
    return make_squadlist_value(squadlist_pool_of(*world_).acquire(site.script, site.slot));
  }
  // And `Conversation C_Conv;`, which every campaign script that talks opens
  // with. Keyed on the site like the rest: `1_Great_Battles_Zama`'s `seq15.vs`
  // declares one at the top and runs two different conversations through it,
  // so one entry serves the whole script and `Init` is what rebinds it.
  if (type_name == "Conversation") {
    return make_conversation_value(conversation_pool_of(*world_).acquire(site.script, site.slot));
  }
  return default_value(type_name);
}

script::Value WorldHost::clone_for_assign(const script::Value& value) {
  // Nothing here needs cloning, and that is a property of the representation
  // rather than an omission: a point is packed into the value itself, so
  // assignment already copies it, and object handles are references that must
  // *not* be cloned.
  //
  // `ObjList` is the case that would be a silent disaster, and the corpus says
  // so out loud. `DATA\AI\TSH_RECRUITARMY.VS` opens with "BE CAREFUL NOT TO
  // DIRECTLY ASSIGN TO ol ... ASSIGNING TO IT WILL NOT CHANGE THE ORIGINAL LIST
  // ... ol MUST BE CHANGED USEING ONLY THE Add/Remove METHODS": mutation is
  // shared, assignment rebinds. Cloning here would break the first half and
  // make that script a no-op for its caller.
  return value;
}

Result<bool> WorldHost::truthy(const script::Value& value) {
  // **Inferred, and untested by the corpus**: no shipped script puts a bare
  // `ObjList` in a condition -- every one of them writes `ol.count`, `.IsValid`
  // or a comparison. A bound list is true, an unbound one false, which is the
  // same rule every other handle here follows and the only one that does not
  // make the emptiness of a list decide a branch. A corpus site of the form
  // `if (ol)` would settle it; there is none.
  if (is_objlist(value)) return objlist_pool_of(*world_).contains(objlist_of(value));
  if (value.is_object()) return value.as_object().valid();
  return script::Host::truthy(value);
}

Result<script::Value> WorldHost::index_get(const script::Value& container,
                                           const script::Value& key) {
  // `a[i]` on a script-side array. Out of range answers the element type's zero
  // -- 0 or `""` -- and grows nothing; see `sim/array.hpp` for why that is the
  // reading and what makes the alternative unobservable.
  if (is_script_array(container)) {
    if (!key.is_integer()) return FormatError::not_found;
    return world_->arrays().get(array_of(container), key.as_integer());
  }
  // `ol[i]` -- how nearly every script reaches an object it was not handed.
  if (!is_objlist(container)) return FormatError::not_found;
  if (!key.is_integer()) return FormatError::not_found;
  const std::span<const ObjectId> items = objlist_pool_of(*world_).items(objlist_of(container));
  const std::int32_t index = key.as_integer();
  // Out of range is an *invalid handle*, not a trap. The corpus guards its
  // subscripts -- `if (!wells.IsEmpty()) well = wells.GetObjList()
  // .FilterClosest(u.pos, 1)[0];` -- but a list can be emptied by a `ClearDead`
  // between the guard and the read, and an invalid handle is what every other
  // failed lookup in this host answers with. `.IsValid` then reports it.
  if (index < 0 || static_cast<std::size_t>(index) >= items.size()) {
    return Value::object(ObjectRef{script::kNoType, 0});
  }
  // A list can hold the invalid handle -- `ObjList::Add` (0x005601a0) stores
  // whatever it is given -- and reading it back is the invalid handle again.
  return object_value(items[static_cast<std::size_t>(index)]);
}

Status WorldHost::index_set(script::Value& container, const script::Value& key,
                            const script::Value& value) {
  // `a[i] = v` on a script-side array, which is **all 197 indexed assignment
  // targets in the corpus** -- `aSkills[i]`, `MaxConsts[nType]`,
  // `strTechMarket[nRace]`. Assignment grows: four shipped sequences open by
  // writing past the end of an array they have just declared empty.
  if (is_script_array(container)) {
    if (!key.is_integer()) return FormatError::not_found;
    if (!world_->arrays().set(array_of(container), key.as_integer(), value)) {
      return FormatError::not_found;
    }
    return Status{};
  }
  // No shipped script assigns through an `ObjList` subscript, so the rest of
  // this exists to make the shape well defined rather than to serve a call
  // site.
  if (!is_objlist(container)) return FormatError::not_found;
  if (!key.is_integer() || !value.is_object()) return FormatError::not_found;
  std::vector<ObjectId>* items = objlist_pool_of(*world_).mutable_items(objlist_of(container));
  if (items == nullptr) return FormatError::not_found;
  const std::int32_t index = key.as_integer();
  // Refuses rather than growing: a list has no defined fill value, and an
  // `ObjList` is not an array. `Add` is how a script makes one longer, at 83
  // sites.
  if (index < 0 || static_cast<std::size_t>(index) >= items->size()) {
    return FormatError::not_found;
  }
  (*items)[static_cast<std::size_t>(index)] = value.as_object().id;
  return Status{};
}

Result<script::Value> WorldHost::binary(script::BinaryOp op, const script::Value& lhs,
                                        const script::Value& rhs) {
  const bool lp = is_point(lhs);
  const bool rp = is_point(rhs);

  // Point arithmetic, integral throughout:
  // `ptCenter + ol[i].pos`, `pt / ol.count`, `(sqLeader.pos - ptBld) * nSight`.
  if (lp && rp) {
    const Point a = unpack_point(lhs);
    const Point b = unpack_point(rhs);
    switch (op) {
      case script::BinaryOp::add: return pack_point(Point{a.x + b.x, a.y + b.y});
      case script::BinaryOp::sub: return pack_point(Point{a.x - b.x, a.y - b.y});
      case script::BinaryOp::eq: return Value::boolean(a == b);
      case script::BinaryOp::ne: return Value::boolean(!(a == b));
      default: break;
    }
    return FormatError::not_found;
  }
  if (lp && rhs.is_integer()) {
    const Point a = unpack_point(lhs);
    const std::int32_t k = rhs.as_integer();
    switch (op) {
      case script::BinaryOp::mul: return pack_point(Point{a.x * k, a.y * k});
      // Integer division, truncating toward zero, because that is what C++ does
      // and there is no other definition available to be inconsistent with.
      case script::BinaryOp::div:
        if (k == 0) return FormatError::not_found;
        return pack_point(Point{a.x / k, a.y / k});
      default: break;
    }
    return FormatError::not_found;
  }
  if (lhs.is_integer() && rp && op == script::BinaryOp::mul) {
    const Point b = unpack_point(rhs);
    const std::int32_t k = lhs.as_integer();
    return pack_point(Point{b.x * k, b.y * k});
  }

  // Handle comparison: two handles are equal when they name the same object.
  if (lhs.is_object() && rhs.is_object() && !lp && !rp) {
    if (op == script::BinaryOp::eq) return Value::boolean(lhs.as_object() == rhs.as_object());
    if (op == script::BinaryOp::ne) return Value::boolean(!(lhs.as_object() == rhs.as_object()));
  }

  return script::Host::binary(op, lhs, rhs);
}

Result<std::string> WorldHost::to_string(const script::Value& value) {
  if (is_point(value)) {
    const Point p = unpack_point(value);
    return std::string("(") + script::to_decimal(p.x) + "," + script::to_decimal(p.y) + ")";
  }
  if (value.is_object()) {
    const ObjectRef ref = value.as_object();
    if (!ref.valid()) return std::string("<invalid>");
    // A `NamedObj` carries a table index, not an object id, and printing the
    // index would put a number in a string that names nothing. `int -> str` is
    // reached through the same `NamedObj -> Obj` conversion `object_of`
    // documents, so print what that conversion yields.
    if (ref.type == kTypeNamedObj) {
      const WorldObject* slot = object_of(*world_, value);
      if (slot == nullptr) return std::string("<invalid>");
      return script::to_decimal(static_cast<std::int32_t>(slot->id));
    }
    return script::to_decimal(static_cast<std::int32_t>(ref.id));
  }
  return script::Host::to_string(value);
}

Result<script::Value> WorldHost::global(std::string_view name) {
  // Which family owns which name, where each family's numbering was read from,
  // and why a name nobody owns must refuse rather than read as zero: all of it
  // is in sim/globals.hpp. Nothing is decided here.
  return resolve_global(name, world_, ai_profile_);
}

// --------------------------------------------------------------------------
// registration
// --------------------------------------------------------------------------

std::size_t register_world_host(script::HostRegistry& registry) {
  std::size_t defined = 0;
  const auto def = [&](script::CallKind kind, std::string_view name, std::uint16_t arity,
                       script::HostFn fn) {
    registry.define(kind, name, arity, fn);
    ++defined;
  };

  // -- free functions, in descending corpus call frequency -----------------
  def(kFree, "rand", 1, &fn_rand);                          // 341 call sites
  def(kFree, "Intersect", 2, &fn_set_op<SetOp::intersect>);  // 64
  def(kFree, "Count", 2, &fn_count);                         // 56
  def(kFree, "ClassPlayerObjs", 2, &fn_class_player_objs);   // 37
  def(kFree, "ObjsInSight", 2, &fn_objs_in_sight<false>);           // 36
  def(kFree, "Union", 2, &fn_set_op<SetOp::set_union>);      // 36
  def(kFree, "ObjsInCircle", 3, &fn_objs_in_circle);         // 32
  def(kFree, "ObjsInRange", 3, &fn_objs_in_range);           // 25
  def(kFree, "Group", 1, &fn_group);                         // 9
  def(kFree, "GetInnState", 1, &fn_get_inn_state);           // 3, and a whole file behind it
  def(kFree, "HasStarvingArmy", 1, &fn_starving_army<false>); // 1
  def(kFree, "StarvingArmyPos", 1, &fn_starving_army<true>);  // 1, and a whole file with the one above
  def(kFree, "SetInnState", 2, &fn_set_inn_state);           // 1
  def(kFree, "SetNoAIFlag", 2, &fn_set_no_ai_flag);          // 121
  // The campaign's way of bringing a map's reinforcement schedule into play.
  // Both are container-only: 0 sites in `data.pak`, 314 across the 24
  // containers.
  def(kFree, "SpawnGroup", 1, &fn_spawn_group);              // 187
  def(kFree, "SpawnGroupInHolder", 2, &fn_spawn_group_in_holder);  // 127
  // Three sites, and it is the sole blocker of `Tutorial` map 1 sequence 0 --
  // 292 call sites, the largest single script in the installation.
  def(kFree, "SpawnNamed", 1, &fn_spawn_named);              // 3
  // The other way an object comes into being: a class name rather than a
  // template. One body each, and they differ in three places -- see the two
  // doc comments, and `places_as_script_object` for the branch they share.
  def(kFree, "Place", 3, &fn_place);        // 131
  def(kFree, "_PlaceEx", 4, &fn_place_ex);  // 89
  def(kFree, "PlaceCatapult", 4, &fn_place_catapult);  //  1, and a whole file behind it
  def(kFree, "IsProtected", 3, &fn_is_protected);      //  2, and a whole file behind it
  def(kMember, "CalcEscapeDirection", 0, &m_calc_escape_direction);  // 1, and a whole file behind it
  def(kMember, "RamBestTarget", 0, &m_ram_best_target);  // 1, and a whole file behind it
  // Membership from the object's side. Each is registered three times in
  // `gbr.exe` -- on `Obj`, `ObjList` and `Query` -- and once here, because the
  // registry keys on (kind, name, arity) and the body takes all three.
  def(kMember, "AddToGroup", 1, &fn_add_to_group);            // 179
  def(kMember, "RemoveFromGroup", 1, &fn_remove_from_group);  // 137
  def(kMember, "RemoveFromAllGroups", 0, &fn_remove_from_all_groups);  // 22
  def(kMember, "IsInGroup", 1, &fn_is_in_group);              // 8
  def(kFree, "GetGroupSize", 1, &fn_get_group_size);          // 2
  // 67 literal call sites in the map containers, and one more per
  // `<group type="0">` in every sequence the original starts -- its prologue
  // builder (`0x005bc760`) writes a `GetNamedObj` call for each of them.
  def(kFree, "GetNamedObj", 1, &fn_get_named_obj);            // 67
  def(kFree, "Subtract", 2, &fn_set_op<SetOp::subtract>);    // 8
  def(kFree, "VisibleObjsInSight", 2, &fn_objs_in_sight<true>);  // 4
  // Both spellings ship, in live scripts, so both are bound to one behaviour.
  def(kFree, "Substract", 2, &fn_set_op<SetOp::subtract>);   // 2
  // The settlement-unit family: one runtime type in `gbr.exe`, three names, and
  // the mode is a field on the query rather than a type of its own.
  //
  // **`UnitsGuardingSettlement` is deliberately not registered.** It is the
  // third mode, it is read to the instruction (0x00575108 pushes the literal
  // 2), and it has zero call sites in the installation -- so binding it would
  // add an entry point the shipped inventory does not declare, which is the
  // rule the rectangle family's tail already follows. `SettlementScope::both`
  // exists anyway, because the mode field has to hold every value the
  // executable can put in it and leaving a hole would invent a fourth
  // encoding; `World::units_in_settlement` implements it and a test exercises
  // it directly.
  def(kFree, "UnitsInSettlement", 2,
      &fn_units_in_settlement<SettlementScope::garrison>);  // 93, 92 of them in containers
  def(kFree, "UnitsAroundSettlement", 2,
      &fn_units_in_settlement<SettlementScope::ring>);  // 1, and it is GUARD.VS

  // -- members, in descending corpus call frequency ------------------------
  def(kMember, "IsValid", 0, &m_is_valid);      // 797
  def(kMember, "player", 0, &m_player);         // 531
  def(kMember, "pos", 0, &m_pos);               // 509
  def(kMember, "posRH", 0, &m_pos_rh);          // 141
  def(kMember, "SetNoAIFlag", 1, &m_set_no_ai_flag);  // 109
  def(kMember, "settlement", 0, &m_settlement); // 432
  def(kMember, "IsAlive", 0, &m_is_alive);      // 366
  def(kMember, "IsVisible", 0, &m_is_visible);  //  56
  def(kMember, "SetVisible", 1, &m_set_visible);  // 51
  def(kMember, "InHolder", 0, &m_in_holder);    // 154
  def(kMember, "EnterHolder", 1, &m_enter_holder);  // 1
  def(kMember, "ExitHolder", 1, &m_exit_holder);    // 1
  def(kMember, "SetName", 1, &m_set_name);          // 1
  def(kMember, "Disappear", 0, &m_disappear);       // 1
  def(kMember, "IsDiseased", 0, &m_is_diseased);    // 1
  def(kMember, "Disease", 0, &m_disease);           // 1
  def(kMember, "AddToStoreBin", 0, &m_store_bin);       // 1
  def(kMember, "RemoveFromStoreBin", 0, &m_store_bin);  // 1
  def(kMember, "SameHolderAs", 1, &m_same_holder_as);  // 2
  def(kMember, "stamina", 0, &m_stamina);       // 150
  def(kMember, "x", 0, &m_point_x);             // 137
  def(kMember, "health", 0, &m_health);         // 129
  def(kMember, "sight", 0, &m_sight);           // 118
  def(kMember, "maxhealth", 0, &m_max_health);  // 109
  def(kMember, "y", 0, &m_point_y);             // 106
  def(kMember, "DistTo", 1, &m_dist_to);        // 90
  def(kMember, "IsHeirOf", 1, &m_is_heir_of);   // 71
  // Three questions a class answers about an object. All three gate a
  // `verify=` script a right click reaches, so a trap here is a click that
  // does nothing at all: `IsValidCaptureTarget` gates `capture`,
  // `IsCentralBuliding` gates `enter`, `IsWaterUnit` gates boarding.
  def(kMember, "IsValidCaptureTarget", 1, &m_is_valid_capture_target);  // 6
  def(kMember, "IsCentralBuliding", 0, &m_is_central_building);         // 3
  def(kMember, "IsWaterUnit", 0, &m_is_water_unit);                     // 2
  def(kMember, "IsRanged", 0, &m_is_ranged);                            // 2
  // Seven more of exactly the same shape: one class property, or one ancestry
  // test the executable carries the name for. `minlevel`, `levelperitem` and
  // `itemtypes` are the whole of what `RUIN_BEHAVIOR.VS` was blocked on.
  def(kMember, "itemtypes", 0, &m_building_item_types);                 // 2
  def(kMember, "GetSentryClassName", 0, &m_building_sentry_class_name);  // 2
  def(kMember, "minlevel", 0, &m_building_minlevel);                    // 1
  def(kMember, "levelperitem", 0, &m_building_level_per_item);          // 1
  def(kMember, "GetNumSentrySlots", 0, &m_building_num_sentry_slots);   // 1
  def(kMember, "IsRam", 0, &m_is_ram);                                  // 1
  // Three globals this engine has no consumer for; see the note on the bodies.
  def(kFree, "SetGlobalBloodlust", 1, &f_set_global_bloodlust);         // 2
  def(kFree, "InvalidateRegenConsts", 0, &f_invalidate_regen_consts);   // 2
  def(kFree, "GetResearchHack", 0, &f_get_research_hack);               // 1
  // Three presentation requests, one body; see the note above it.
  def(kMember, "PopTransportationUI", 0, &f_presentation_void);         // 2
  def(kMember, "SetBuildFrame", 1, &m_set_build_frame);                 // 2
  def(kMember, "ShowBuildAnimation", 1, &m_show_build_animation);          // 1
  def(kMember, "SetDebug", 1, &f_presentation_void);                    // 1
  def(kMember, "IsPeasantAmbient", 0, &m_is_peasant_ambient);           // 1
  def(kMember, "SetSight", 1, &m_set_sight);                            // 4
  def(kMember, "RRepair", 0, &m_rrepair);                               // 3
  def(kMember, "SetJupiterAngerTarget", 1, &m_set_jupiter_target);      // 1
  def(kMember, "GetJupiterAngerTarget", 0, &m_get_jupiter_target);      // 1
  def(kFree, "EnemyInRange", 3, &fn_enemy_in_range);                    // 1
  // One site, and it is the sole blocker of `ESH_RESEARCHTRAINING.VS` -- 93
  // call sites behind a two-name class test.
  def(kMember, "IsPeaceful", 0, &m_is_peaceful);                       // 1
  // Two more of the same shape, each one class property or one flag bit.
  // Neither unblocks its verifier on its own, and saying so is the point:
  // `SHIP_BOARD_VERIFY.VS` and `UNIT_BOARD_VERIFY.VS` compare `UnitsMax`
  // against `UnitsCount`, which sim/economy.cpp answers for a `Settlement`
  // receiver only and *traps* for a `Ship` one -- `Ship::UnitsCount`
  // (`gbr.exe` `0x005c6de0`) is `holder + 0x14`, the boarded count, and this
  // engine has no boarding to count. `UNIT_TRANSPORT_VERIFY.VS` still needs
  // `GetInnState/1` and `class/0` beside `GetParty`.
  def(kMember, "UnitsMax", 0, &m_units_max);                            // 3
  def(kMember, "GetParty", 0, &m_get_party);                            // 1
  def(kMember, "IsDead", 0, &m_is_dead);        // 69
  // Two registrations in `gbr.exe` -- `Obj` and `Query`, and deliberately not
  // `ObjList`, which has no `Erase` at all. One body, dispatching on the
  // receiver; see the section comment for what an erase is and is not.
  def(kMember, "Erase", 0, &m_erase);            // 47
  // Four one-field predicates, and each is registered in `gbr.exe` on a
  // receiver the corpus census got wrong. See their doc comments.
  def(kMember, "IsInAir", 0, &m_is_in_air);      // 7, all on crows
  def(kMember, "IsBuilding", 0, &m_is_building); // 2, both in SHIP_IDLE.VS
  def(kMember, "InShip", 0, &m_in_ship);         // 9
  def(kMember, "GetPoint", 2, &m_get_point);          //  32
  def(kMember, "GetEnterPoint", 1, &m_get_enter_point);  // 18
  def(kMember, "GetExitPoint", 2, &m_get_exit_point);    //  7, two argument forms
  // Two names, one value, and it is not in the point table at all.
  def(kMember, "GetExitVector", 0, &m_exit_vector);   //   5
  def(kFree, "GetCatapultAttackPoint", 1, &fn_get_catapult_attack_point);  // 3
  def(kMember, "exit_vector", 0, &m_exit_vector);     //   6
  def(kMember, "destination", 0, &m_teleport_destination);   //  20
  def(kMember, "Traverse", 1, &m_teleport_traverse);         //   6
  def(kMember, "GetUITarget", 0, &m_get_ui_target);          //   3
  def(kMember, "SetUITarget", 1, &m_set_ui_target);          //   1
  def(kMember, "SetSummoningDeath", 1, &m_set_summoning_death);  //   3
  def(kMember, "IsSummoningDeath", 0, &m_is_summoning_death);    //   2
  def(kMember, "IsMilitary", 0, &m_class_predicate<true>);   //   5
  def(kMember, "IsSentry", 0, &m_class_predicate<false>);    //   5
  def(kMember, "GetFlags", 1, &m_get_flags);          //   5
  def(kMember, "SetNoselectFlag", 1, &m_set_noselect_flag);  // 6
  // The script scratch int. Two entry points, one field, no engine reader.
  def(kMember, "SetUser", 1, &m_set_user);       // 13
  def(kMember, "Mutate", 1, &m_mutate);          //  2
  def(kMember, "user", 0, &m_user);              // 8
  def(kMember, "class", 0, &m_class);                   //  22
  def(kMember, "GlobalSpellStart", 2, &m_global_spell);  //   9
  def(kMember, "GlobalSpellStop", 0, &m_global_spell);   //   1, the same body
  // `GetGlobalSpell/0`, `SetGlobalSpellData/1`, `CountMages/0`,
  // `WindOfWisdom/1`, `Starvation/0`, `SoothingRain/1`, `DivineSacrifice/0`
  // and `GetSoothingRainObjects/0` are registered in `gbr.exe` with **zero**
  // call sites each and are not bound; see `m_global_spell`.
  def(kMember, "SetEntering", 1, &m_set_entering);      //  21
  def(kMember, "SetParryMode", 1, &m_set_parry_mode);  // 31
  def(kMember, "GetParryMode", 0, &m_get_parry_mode);  //  2
  def(kMember, "IsBroken", 0, &m_is_broken);           //  9
  def(kMember, "IsVeryBroken", 0, &m_is_very_broken);  // 10
  def(kMember, "GetSacrifice", 0, &m_get_sacrifice);   //  5
  def(kMember, "IsInvisibility", 0, &m_is_invisibility);  // 3
  def(kMember, "AddDruid", 2, &m_add_druid);        //  2, and a whole file behind it
  def(kMember, "Consume", 3, &m_consume);           //  1, and a whole file behind it
  def(kMember, "MistAction", 1, &m_mist_action);    //  1, and a whole file behind it
  def(kMember, "CoverOfMercyAction", 0, &m_cover_of_mercy_action);  // 1, and a whole file behind it
  def(kMember, "MagicActionEnd", 0, &m_magic_action_end);    //  1, and a whole file with GetStaminaDecTime
  def(kFree, "abs", 1, &fn_abs);                 // 1
  def(kMember, "SetLastAttackTime", 0, &m_set_last_attack_time);  // 40
  def(kMember, "Curse", 0, &m_curse);            // 3, SHAMAN_IDLE.VS
  def(kMember, "IsCursed", 0, &m_is_cursed);     // 3, the same script
  def(kFree, "CLAMP", 3, &fn_clamp);             // 3
  def(kFree, "MIN", 2, &fn_min);                 // 3
  def(kFree, "MAX", 2, &fn_max);                 // 2
  def(kFree, "IsPointInWater", 1, &fn_is_point_in_water);  // 127 hits, five scripts
  // The ship's AI transport, whose only writer is unreachable. See the section.
  def(kMember, "HasAiTransport", 0, &m_has_ai_transport);      // 60 hits
  def(kMember, "ClearAiTransport", 0, &m_clear_ai_transport);  // 1
  def(kMember, "GetTransPt", 0, &m_get_trans_pt);              // 1
  def(kMember, "ApplyAiTransport", 0, &m_apply_ai_transport);  // 1, and a whole file behind it
  // The stonehenge's two, over one scan. See the section for what the count is.
  def(kMember, "StonehengeNumControllingMages", 0, &m_stonehenge_mages);   // 18 hits
  def(kMember, "IsStonehengeControlable", 0, &m_stonehenge_controlable);   // 5
  def(kMember, "CanSee", 1, &m_can_see);                        // 57 sites
  def(kMember, "SetMessengerStatus", 1, &m_set_messenger_status);  // 26
  def(kMember, "SetMinimapFlag", 1, &m_set_minimap_flag);  // 80 hits, one script
  def(kMember, "HasSpecial", 1, &m_has_special);          // 205 hits, four scripts
  def(kMember, "FindNearBird", 0, &m_find_near_bird);     // 1, and it is CROW_IDLE.VS
  // The gate's five, and `GATE_IDLE.VS` is the whole of their corpus.
  def(kMember, "LookAround", 1, &m_look_around);                        // 224 hits
  def(kMember, "AreEnemiesAround", 0, &m_gate_saw<&ObjectFlags::enemies_near>);  // 2
  def(kMember, "AreFriendsAround", 0, &m_gate_saw<&ObjectFlags::friends_near>);  // 1
  def(kMember, "OpenNow", 0, &m_gate_set<true>);                        // 3
  def(kMember, "CloseNow", 0, &m_gate_set<false>);                      // 4
  // The catapult's assembly flag and its setter. `IsBuilt` is the last
  // unimplemented name in `DATA\AI HELPERS\GUARD.VS`; `SetBuilt` is what
  // would ever make it true, and shipping the reader without the writer would
  // be a flag that is false forever with nothing saying why.
  def(kMember, "IsBuilt", 0, &m_is_built);      // 3
  def(kMember, "SetCmdEnable", 1, &m_set_cmd_enable);  // 1, and a 210-site file with DoCarryNothing
  def(kMember, "StartTraining", 0, &m_set_training<true>);   // 1
  def(kMember, "StopTraining", 0, &m_set_training<false>);   // 1, and a whole file behind it
  def(kMember, "BestTrainingTarget", 0, &m_best_training_target);  // 2
  def(kMember, "TrainAttack", 1, &m_train_attack);           // 1, and a whole file with the two above
  def(kMember, "GetStaminaDecTime", 0, &m_get_stamina_dec_time);  // 2, both receivers
  def(kMember, "SetBuilt", 0, &m_set_built);    // 1
  def(kMember, "AttackWait", 0, &m_attack_wait);  // 3, and three files with RotateTo
  def(kMember, "ClearTowerTarget", 0, &m_clear_tower_target);  // 1, and a whole file behind it
  def(kMember, "RotateTo", 1, &m_rotate_to);      // 3
  // `NamedObj::obj`. 1,174 sites, 1,173 of them inside the map containers,
  // because it is how a `<group type="0">` name reaches its object.
  def(kMember, "obj", 0, &m_named_obj_obj);     // 1174
  def(kMember, "Set", 2, &m_point_set);         // 66
  def(kMember, "Rot", 1, &m_point_rot);         // 385
  def(kMember, "SetLen", 1, &m_point_set_len);  // 73
  def(kMember, "IntoRect", 1, &m_point_into_rect);   // 398
  def(kFree, "GetMapRect", 0, &fn_get_map_rect);     // 39
  def(kMember, "InRect", 1, &m_point_in_rect);       // 14
  def(kMember, "ClampToMap", 0, &m_point_clamp_to_map);  // 8
  def(kMember, "left", 0, &m_rect_left);
  def(kMember, "top", 0, &m_rect_top);
  def(kMember, "right", 0, &m_rect_right);
  def(kMember, "bottom", 0, &m_rect_bottom);
  def(kMember, "width", 0, &m_rect_width);
  def(kMember, "height", 0, &m_rect_height);
  def(kMember, "Len", 0, &m_point_len);         // 11, ten of them without parentheses
  def(kMember, "Dist", 1, &fn_point_dist);      // 26
  def(kFree, "Dist", 2, &fn_point_dist);        // 11, and the *same body* in gbr.exe
  def(kFree, "Point", 2, &fn_point_make);       // 53
  def(kMember, "radius", 0, &m_radius);
  // `count/0` (541 sites) is sim/objlist.cpp's: one entry point, two receivers.
  def(kMember, "IsEmpty", 0, &m_query_is_empty);  // 27

  // Downcasts.
  //
  // **`AsTower` is `AsBuilding`.** Not "close enough to it" -- the same
  // function: `gbr.exe` registers both names at body `0x005aa5e0`, and the
  // body's whole test is `[obj + 0x2c] & 0x800000`, which is `kSyncBuilding`.
  // The two registrations differ only in the declared *return* type word (0x19
  // `Tower` against 0x16 `Building`), which the compiler uses to decide which
  // members the result may be sent and the call boundary never inspects. This
  // slice used to record the opposite -- "`Tower` has no distinct `cpp_class`,
  // so there is nothing to test against" -- which was the right observation
  // about the class graph and the wrong conclusion about the entry point: the
  // original does not test for a tower either.
  //
  // `OUTPOST_ATTACK_VERIFY.VS` is what the three shipped sites look like, and
  // it makes the reading plain -- `w = blds[i].AsTower(); if (w.IsValid()) if
  // (w.IsHeirOf("FakeTower"))`. The cast admits every building and the *class*
  // question is asked afterwards, by name, by the script.
  def(kMember, "AsUnit", 0, &m_as<NativeClass::unit>);              // 405
  def(kMember, "AsBuilding", 0, &m_as<NativeClass::building>);      // 213
  def(kMember, "AsTower", 0, &m_as<NativeClass::building>);         //   3
  def(kMember, "AsHero", 0, &m_as<NativeClass::hero>);              // 150
  def(kMember, "AsDruid", 0, &m_as<NativeClass::druid>);            // 49
  def(kMember, "AsShip", 0, &m_as<NativeClass::ship>);              // 28
  def(kMember, "AsCatapult", 0, &m_as<NativeClass::catapult>);      // 20
  def(kMember, "AsWagon", 0, &m_as<NativeClass::wagon>);            // 13
  def(kMember, "AsGate", 0, &m_as<NativeClass::gate>);              // 12
  def(kMember, "AsFlying", 0, &m_as<NativeClass::flying_unit>);     // 9
  def(kMember, "AsTeleport", 0, &m_as<NativeClass::teleport>);      // 5
  def(kMember, "AsSacrifice", 0, &m_as<NativeClass::sacrifice>);    // 4
  def(kMember, "AsItemHolder", 0, &m_as<NativeClass::item_holder>); // 4
  def(kMember, "AsBarrack", 0, &m_as<NativeClass::barrack>);        // 1

  // The ambient command names -- `cmdparam` and the four `cmdcost_*`. They are
  // registered here rather than by the command domain because the object model
  // is what owns `Host::global`'s side of the 245-name inventory, and because
  // `gbr.exe` registers them as zero-argument *functions* rather than as
  // constants (sim/globals.hpp has the two addresses). They are counted in the
  // return value like everything else.
  defined += register_global_hosts(registry);

  return defined;
}

}  // namespace imperivm::core::sim
