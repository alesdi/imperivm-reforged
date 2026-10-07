// The object-model half of the .vs host API.
// See include/imperivm/core/sim/world_host.hpp.

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

#include <algorithm>
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
/// `kTypeNamedObj` resolves through the name table rather than being rejected,
/// and that is not a widening: `gbr.exe` registers a **type conversion**
/// `NamedObj -> Obj` at `0x0055343e` -- `0x00699800(from=0x22, to=0x14,
/// fn=0x005330a0, cost=10)`, through the same function that implements
/// `NamedObj::obj`. The registrar's three other uses are the language's
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
    return world.find(world.named_objects().object(index));
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
  const Point home = world.resolve_position(building);
  MovementSystem* movement = movement_system(const_cast<World&>(world));
  std::vector<Point> doors;
  building_doors(world, movement == nullptr ? nullptr : &movement->grid(), building,
                 kEnterPointType, water, doors);
  if (doors.empty()) return home;

  const Point from = world.resolve_position(unit);
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

/// `rand(n)` -- uniform in `[0, n)`. See sim/rng.hpp for the three call sites
/// in the shipped corpus that prove the bound is exclusive.
HostOutcome fn_rand(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("rand: no world");
  const Value& bound = ctx.arg(0);
  if (!bound.is_integer()) return HostOutcome::failed("rand: expected an integer bound");
  return HostOutcome::ok_with(Value::integer(world->rng().below(bound.as_integer())));
}

/// `ObjsInSight(observer, "class")`.
HostOutcome fn_objs_in_sight(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ObjsInSight: no world");
  const WorldObject* observer = object_of(*world, ctx.arg(0));
  if (observer == nullptr) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = objs_in_sight(observer->id, filter_arg(*world, ctx.arg(1)), false);
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

/// `VisibleObjsInSight(observer, "class")`. The visibility restriction is
/// recorded on the spec and **not applied**: fog of war is one of the four hash
/// channels the shipped build left at zero in all nine dumps, and filtering on
/// it here would pull it back into hashed state.
HostOutcome fn_visible_objs_in_sight(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("VisibleObjsInSight: no world");
  const WorldObject* observer = object_of(*world, ctx.arg(0));
  if (observer == nullptr) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = objs_in_sight(observer->id, filter_arg(*world, ctx.arg(1)), true);
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

/// `ObjsInCircle(point, radius, "class")`.
HostOutcome fn_objs_in_circle(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ObjsInCircle: no world");
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("ObjsInCircle: expected a point");
  if (!ctx.arg(1).is_integer()) return HostOutcome::failed("ObjsInCircle: expected a radius");
  const QuerySpec spec = objs_in_circle(unpack_point(ctx.arg(0)), ctx.arg(1).as_integer(),
                                        filter_arg(*world, ctx.arg(2)));
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

/// `ObjsInRange(object, "class", radius)` -- a circle that follows an object
/// rather than the point it happened to stand on.
HostOutcome fn_objs_in_range(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ObjsInRange: no world");
  const WorldObject* anchor = object_of(*world, ctx.arg(0));
  if (anchor == nullptr) return HostOutcome::ok_with(invalid_object());
  if (!ctx.arg(2).is_integer()) return HostOutcome::failed("ObjsInRange: expected a radius");
  const QuerySpec spec =
      objs_in_range(anchor->id, ctx.arg(2).as_integer(), filter_arg(*world, ctx.arg(1)));
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

/// `ClassPlayerObjs("class", player)`.
///
/// A player number that does not resolve selects **nothing**, and that is a
/// deliberate departure from what `QuerySpec` means on its own: in
/// `objects_of_class_for_player`, `kNoPlayer` means *any owner*. So passing an
/// out-of-range number straight through would not answer "no such player" -- it
/// would answer with **every object on the map**, which is a wrong set that
/// composes, hashes, and looks entirely plausible. The same widening is the
/// bug `EnemyObjs` shipped with. No shipped script passes an out-of-range
/// number, so neither reading is observable on retail data; that is a reason to
/// choose the safe one, not a reason not to choose.
HostOutcome fn_class_player_objs(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClassPlayerObjs: no world");
  const PlayerId player = player_arg(ctx.arg(1));
  // An invalid handle, which is this file's established answer for a miss --
  // `ObjsInRange` and `UnitsInSettlement` both use it when their anchor does
  // not resolve. Minting a real query with no owner would be the widening.
  if (player == kNoPlayer) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = class_player_area(filter_arg(*world, ctx.arg(0)), player, Point{}, 0);
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
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
/// 0xffff sentinel. Same for a class name the graph does not know, which
/// diverges here in a way worth naming: the original refuses the whole query
/// (0x00574e73, *"Could not find class named '%s' ..."*), while
/// `ClassFilter::parse` yields a filter that names classes and resolved none,
/// which `matches_filter` reads as matching nothing. The two differ only
/// through `.IsValid`, and no shipped site asks.
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

  const QuerySpec spec = units_in_settlement(settlement, filter_arg(*world, ctx.arg(1)), kScope);
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

/// `Group(n)`.
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
        world->create_query(group_query(world->group_index(ctx.arg(0).as_string())))));
  }
  if (!ctx.arg(0).is_integer()) return HostOutcome::failed("Group: expected an integer");
  return HostOutcome::ok_with(query_value(world->create_query(group_query(ctx.arg(0).as_integer()))));
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
/// and go through the registry's own int get and set. The read answers **1**
/// for anything stored below 1 -- an inn nobody has touched is open -- and
/// the write clamps to 1..3. What the scripts make of the numbers: 1 is open
/// for transport (`UNIT_TRANSPORT_VERIFY.VS`, `INN_TRANSPORT_REQUEST.VS`), 3
/// is what `INN_BEHAVIOR.VS` waits on before it announces the inn and sets 1.
///
/// The index this engine keys on is the object id: nothing here authors
/// `[inn+0x208]`, and the path is spelt by nobody but these two -- no script
/// reads it back through `EnvReadInt` -- so the observable is only that each
/// inn's state is its own, which the id gives. The `(str, str)` forms
/// `gbr.exe` registers beside these have no call site and stay unbound.
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
  return HostOutcome::ok_with(Value::integer(stored < 1 ? 1 : stored));
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
/// The list and query forms apply to every member, in the order the receiver
/// yields them -- which for a group query is the order they joined it.
[[nodiscard]] std::vector<ObjectId> membership_receivers(CallContext& ctx, World& world) {
  if (ctx.count() == 0) return {};
  return receiver_objects(world, ctx.arg(0));
}

/// `AddToGroup(name)` -- 179 member sites plus the free spellings.
///
/// **Creates the group it cannot find**, which is the one asymmetry in this
/// family: `gbr.exe` carries `Could not find group named '%s'` for every form
/// of `RemoveFromGroup` and for the three spawn entry points, and for **no**
/// form of `AddToGroup`. `Conquests\mediterranean` map 4 needs exactly that --
/// it builds `Oasis_Guards`, a name no map file declares, out of twelve
/// `AddToGroup` calls and then loops on `Group("Oasis_Guards").count`.
HostOutcome fn_add_to_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AddToGroup: no world");
  if (!ctx.arg(1).is_string()) return HostOutcome::failed("AddToGroup: expected a group name");
  const std::int32_t group = world->group_index(ctx.arg(1).as_string());
  for (const ObjectId id : membership_receivers(ctx, *world)) world->groups().add(group, id);
  return HostOutcome::ok_void();
}

/// `RemoveFromGroup(name)` -- 137 member sites.
///
/// Refuses the group it cannot find, and refusing means a printed diagnostic
/// and nothing else: `find`, not `intern`, so a misspelling does not leave an
/// empty group behind for a later `Group(...)` to resolve.
HostOutcome fn_remove_from_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RemoveFromGroup: no world");
  if (!ctx.arg(1).is_string()) {
    return HostOutcome::failed("RemoveFromGroup: expected a group name");
  }
  const std::int32_t group = world->groups().find(ctx.arg(1).as_string());
  if (group == GroupTable::kNoGroup) return HostOutcome::ok_void();
  for (const ObjectId id : membership_receivers(ctx, *world)) world->groups().remove(group, id);
  return HostOutcome::ok_void();
}

/// `RemoveFromAllGroups()` -- 22 sites.
/// `Group("GoldMules" + idPlayer).RemoveFromAllGroups()` in
/// `DATA\AI\ES_OUTPOSTSELLGOLD.VS` is the shipped shape, and it is why the
/// query receiver has to be expanded before anything is removed: the query
/// reads the group it is emptying.
HostOutcome fn_remove_from_all_groups(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RemoveFromAllGroups: no world");
  for (const ObjectId id : membership_receivers(ctx, *world)) world->groups().remove_from_all(id);
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

/// The body behind both spawn entry points.
///
/// `found` false means the name resolved to nothing -- an unknown group, an
/// unknown settlement, an invalid holder. All three print a diagnostic in the
/// original and hand back an empty list rather than refusing, so all three do
/// that here; `sim/world.hpp` records the addresses.
HostOutcome spawn_group_into_list(CallContext& ctx, World& world, std::string_view name,
                                  ObjectId holder, bool found = true) {
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
  //
  // The *filtering* half is redundant and is kept for shape rather than for
  // effect: `World::spawn_from_template` refuses anything that is not a
  // template, so passing it the group's whole membership selects the same set
  // and returns the same list. Fault injection says so -- swapping this for
  // `groups().members(group)` changes no test in the suite. Recorded rather
  // than papered over, because a redundancy nothing can observe is exactly
  // what stops being redundant the day one of the two rules moves.
  std::vector<ObjectId> templates;
  (void)world.templates_in_group(group, templates);
  items->reserve(templates.size());
  for (const ObjectId templ : templates) {
    const ObjectId copy = world.spawn_from_template(templ, holder);
    if (copy == kNoObject) continue;
    // Same reason as `Place`: a reinforcement that arrives mid-turn must be in
    // the feeding chain before the sequence that spawned it starts giving it
    // orders and food. See `feed_from_now`.
    feed_from_now(world, copy);
    items->push_back(copy);
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `SpawnGroup(name)` -- 187 call sites, all in the campaign containers.
///
/// Brings every template in the named group into play and hands back the
/// copies as an `ObjList`, which is what the numeric registrar says: the
/// registration at 0x00577814 declares the return type as 23, `ObjList`.
///
/// The miss is **not** a trap. `gbr.exe` prints `Could not find group named
/// '%s' in function 'SpawnGroup'. Check the spelling.` through 0x00686eb0 --
/// which in the retail build is a bare `ret`, so nothing prints -- and then
/// returns a freshly allocated *empty* list (0x00576887 allocates 0x2c bytes
/// on the miss path exactly as the hit path does). A script that spawns a name
/// no map declares gets an empty list and runs on, and `find` rather than
/// `intern` is what keeps this from minting the group as a side effect.
HostOutcome fn_spawn_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SpawnGroup: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("SpawnGroup: expected a group name");
  return spawn_group_into_list(ctx, *world, ctx.arg(0).as_string(), kNoObject);
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
  return HostOutcome::ok_with(Value::object(kTypeObj, copy));
}

/// `SpawnGroupInHolder(name, where)` -- 127 call sites.
///
/// **Registered twice at the same arity**, which is why one body answers both:
/// 0x0057782f takes `(str, str)` and 0x00577847 takes `(str, Obj)`, and this
/// engine's registry keys on `(kind, name, arity)`. The corpus reaches both --
/// 124 sites pass a settlement name (`"S_Utica"`, `set.name`) and one passes
/// an object (`5_Great_Battles_Britain` seq6's `SpawnGroupInHolder(sArmy,
/// ol[a])`, loading a ship) -- so the argument's runtime type is the
/// discriminator, exactly as it is for `GetGAIKA`.
///
/// The second body's own diagnostic calls it `SpawnGroupInShip`, and there is
/// no registration under that name anywhere in the image: the ship form ships
/// registered as `SpawnGroupInHolder`.
HostOutcome fn_spawn_group_in_holder(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SpawnGroupInHolder: no world");
  if (!ctx.arg(0).is_string()) {
    return HostOutcome::failed("SpawnGroupInHolder: expected a group name");
  }

  ObjectId holder = kNoObject;
  if (ctx.arg(1).is_string()) {
    // The settlement form. The name is the settlement's own, the same key
    // `GetSettlement/1` and `WaitSettlementCapture` scan for, and the copies go
    // into that settlement's holder rather than into the settlement object.
    const std::string_view name = ctx.arg(1).as_string();
    EconomySystem* economy = economy_of(*world);
    const Settlement* settlement =
        economy == nullptr ? nullptr : economy->settlements().find_by_name(name);
    if (settlement == nullptr) {
      // `Could not find settlement named '%s' in function 'SpawnGroupInHolder'`
      // -- a printed diagnostic and an empty list, not a refusal.
      return spawn_group_into_list(ctx, *world, ctx.arg(0).as_string(), kNoObject, false);
    }
    holder = settlement->holder.object;
  } else {
    const WorldObject* target = object_of(*world, ctx.arg(1));
    if (target == nullptr) {
      // `SpawnGroupInHolder invalid holder` (0x007c55d3).
      return spawn_group_into_list(ctx, *world, ctx.arg(0).as_string(), kNoObject, false);
    }
    holder = target->id;
  }
  return spawn_group_into_list(ctx, *world, ctx.arg(0).as_string(), holder);
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

  // Then the AI hand-off (0x0041f310), which the original performs for a
  // `CVXUnit` whose position reads back with `x >= 0` -- and which skips
  // anything carrying the spawn-template bit, corroborating bit 27 from a call
  // site that has nothing to do with groups.
  //
  // **This engine has no counterpart and does not fake one.** Its AI runs a
  // player's `Main.vs` rather than keeping a roster of objects, and what a
  // freshly placed unit actually needs -- its class's `idle` method -- it gets
  // from `GameSession`'s per-turn sweep for objects with no script, the same
  // sweep a `SpawnGroup` copy relies on. The divergence is that nothing here
  // reads the template bit at this point, and nothing needs to: a template is
  // never the thing `Place` just created.
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
/// `ObjectFlags::built` defaults to. The handle is read from `word [obj+8]`
/// and 0xffff answers a creation that failed.
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
/// `(2r)^2` for the sweep), for objects of that class -- the sweep tests the
/// class descriptor -- whose owner is in the player's **friendly mask**,
/// `[record+0x10] ^ 0xffff`: the complement of the enemy word
/// `PlayerTable::is_enemy` reads, as the caster searches in `sim/hero.cpp`
/// already established. Any hit is true.
///
/// A cover of mercy is placed by the script itself (`Place("CoverOfMercy",
/// pt, .player)`), so unlike a sacrifice it can exist here, and the sweep is
/// the engine's own: every live object of the class, the point inside its
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
    if (other.state.health <= 0 || !world->class_is_a(other.id, wanted)) continue;
    if (!PlayerTable::is_valid(other.state.owner)) continue;
    if (world->players().is_enemy(viewer, other.state.owner)) continue;
    if (!ring.contains_by_query_rule(world->resolve_position(other.id))) continue;
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
/// called for an uninitialized or invalid object.` and answers `(0, 0)`.
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
/// With no such building in sight it is a random direction (0x004e26c0):
/// `rand(0, 1000) - 500` on x, then the same on y, normalised to 1000 the
/// same way; a draw that lands on the origin exactly stays `(0, 0)`, which
/// `SetLen` then turns into `(0, len)` by its own zero rule. Two draws on the
/// world RNG, in that order, every time no threat is found.
///
/// **The tie rule is the one labelled divergence.** The original's sweep
/// visits 256-unit cells row by row and the objects within a cell in the
/// order they were entered; `World::objects_in_radius` answers in id order.
/// Two enemy towers at exactly the same squared distance -- which needs
/// symmetric placement to the unit -- would pick the lower id here and the
/// earlier-swept one there. Nothing shipped places towers that way, and the
/// sweep order is not worth a second index to reproduce.
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
  (void)world->objects_in_radius(here, self->sight, ClassFilter{}, around);
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

  std::int32_t dx = 0;
  std::int32_t dy = 0;
  if (threat != nullptr) {
    const Point at = world->resolve_position(threat->id);
    dx = here.x - at.x;
    dy = here.y - at.y;
  } else {
    dx = world->rng().between(0, 1000) - 500;
    dy = world->rng().between(0, 1000) - 500;
  }
  // `isqrt` is the floored root the table at 0x0067c3d0 answers; see `Dist`.
  const auto len = static_cast<std::int32_t>(
      isqrt(static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy));
  if (len <= 0) return HostOutcome::ok_with(pack_point(Point{0, 0}));
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
/// The tie rule is the same labelled divergence `CalcEscapeDirection`
/// carries: the sweep's cell order there, id order here.
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
  (void)world->objects_in_radius(here, self->sight + 512, ClassFilter{}, around);
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
/// -- 109. Together the sixth most-used unimplemented name in the install.
///
/// The flag says "this unit belongs to a script; the AI is to leave it alone",
/// and both bodies do the same one thing to it: `[unit+0x194]` cleared of
/// `0x00040000` and then or-ed back in when the argument is true. That is bit
/// 18 of a **second** flag word, not of the `+0x2c` `SyncFlags` every other
/// object flag here comes from -- see `ObjectFlags::no_ai`.
///
/// **The two forms are not the same function with a different receiver.** The
/// free form (0x00435470) walks the list and applies three filters the member
/// form (0x005de600) does not have:
///
///   1. a handle that resolves to nothing is skipped;
///   2. `[obj+0x2c] & 0x00400000` -- bit 22, which `docs/formats/map.md`
///      records as *the object is a mobile unit*, all 16,171 of them -- must
///      be set, so a building in the list is passed over rather than flagged;
///   3. the virtual at `vtbl + 0x50` must return zero. That slot is
///      `IsDead`: `Obj::IsAlive` (0x005aae00) is nothing but its negation,
///      and it has 229 call sites across the image. So **the dead are
///      skipped**.
///
/// The member form validates the handle and writes the bit, with no unit test
/// and no death test -- its receiver is typed `Unit` by the registrar, which
/// this engine's `(kind, name, arity)` registry does not reproduce. Both
/// asymmetries are the original's and are kept.
///
/// The member form also posts something through 0x00447330 afterwards that the
/// free form does not; it constructs a temporary with the vtable at 0x007ac828
/// and hands it to the AI. Not reproduced, because nothing here receives it.
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
    // `The function 'Unit::SetNoAIFlag' called for an uninitialized or invalid
    // object` (0x007d31a0), and then a return. Not a refusal.
    if (slot == nullptr) return HostOutcome::ok_void();
    ids.push_back(slot->id);
  }

  for (const ObjectId id : ids) {
    ObjectState* state = world->mutable_state(id);
    if (state == nullptr) continue;
    if (kFiltered) {
      if (!state->flags.is_unit) continue;
      if (state->health <= 0) continue;  // `vtbl + 0x50`, which is `IsDead`
    }
    state->flags.no_ai = wanted;
  }
  return HostOutcome::ok_void();
}

HostOutcome fn_set_no_ai_flag(CallContext& ctx) { return set_no_ai_flag<true>(ctx, 1); }
HostOutcome m_set_no_ai_flag(CallContext& ctx) { return set_no_ai_flag<false>(ctx, 1); }

/// The three set-algebra functions. Both spellings of subtract ship -- 8 and 2
/// call sites, in live scripts -- so both are the same entry point here.
template <SetOp kOp>
HostOutcome fn_set_op(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("set operation: no world");
  const WorldObject* lhs = object_of(*world, ctx.arg(0));
  const WorldObject* rhs = object_of(*world, ctx.arg(1));
  if (lhs == nullptr || rhs == nullptr) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = set_op(kOp, lhs->id, rhs->id);
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

/// `Count(player, "class")` -- how many objects of a class a player owns.
///
/// The one free function here that answers immediately rather than minting a
/// query object. It has to: the corpus assigns its result to an `int`, so no
/// handle escapes, and minting a query per call would leak a handle per call
/// and shift every later allocation. The dumps show no query type that would
/// correspond to it.
HostOutcome fn_count(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Count: no world");
  const PlayerId player = player_arg(ctx.arg(0));
  // See `fn_class_player_objs`: `kNoPlayer` here would mean *every* owner.
  if (player == kNoPlayer) return HostOutcome::ok_with(Value::integer(0));
  std::vector<ObjectId> found;
  const std::size_t n = world->objects_of_class_for_player(filter_arg(*world, ctx.arg(1)), player,
                                                           Point{}, 0, found);
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(n)));
}

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
  return world.named_objects().valid(index) ? index : NamedObjectTable::kNoName;
}

/// `GetNamedObj("...")` -- 66 of the 67 literal call sites in the shipped map
/// scripts, and the other one is a group name the editor should have caught.
///
/// An unknown name answers with an invalid handle rather than trapping.
/// `gbr.exe` prints `Could not find named object named '%s'` and carries on,
/// and `.IsValid` is what the scripts then test; trapping would stop a script
/// that the original merely warns at.
HostOutcome fn_get_named_obj(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetNamedObj: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("GetNamedObj: expected a name");
  return HostOutcome::ok_with(
      named_object_value(world->named_objects().find(ctx.arg(0).as_string())));
}

/// `NamedObj::obj`.
///
/// The bound object *as bound*, dead or alive: this is the read that has to
/// keep working after the object dies, or `IsDead` could never be true. An
/// unbound entry gives an invalid handle, which is what every other failed
/// lookup in this file answers with.
HostOutcome m_named_obj_obj(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("obj: no world");
  const std::int32_t index = named_index_of(*world, ctx.arg(0));
  if (index == NamedObjectTable::kNoName) return HostOutcome::ok_with(invalid_object());
  return HostOutcome::ok_with(object_value(world->named_objects().object(index)));
}

// --------------------------------------------------------------------------
// members: identity and state
// --------------------------------------------------------------------------

HostOutcome m_is_valid(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsValid: no world");
  // `NamedObj::IsValid` asks whether the *name* is bound, not whether what it
  // is bound to is alive -- `IsDead` is the separate member for that, and two
  // members that answered the same question would make one of them pointless.
  if (ctx.arg(0).is_object() && ctx.arg(0).as_object().type == kTypeNamedObj) {
    return HostOutcome::ok_with(
        Value::boolean(named_index_of(*world, ctx.arg(0)) != NamedObjectTable::kNoName));
  }
  return HostOutcome::ok_with(Value::boolean(object_of(*world, ctx.arg(0)) != nullptr));
}

/// `Obj::IsVisible` -- 56 call sites, and the top trap on every shipped map
/// once the target sweep stops failing before it.
///
/// `gbr.exe` (0x005ab970) returns `!((SyncFlags >> 21) & 1)`, registered at
/// 0x005b7721 as `bool, Obj`. **The stored bit means hidden**, so this is its
/// negation; the same bit gates a candidate out of the target sweep at
/// 0x005dba19, which is why an invisible unit cannot be attacked.
///
/// An object that does not resolve reads as not visible rather than trapping:
/// `UNIT_IDLE.VS` asks this of a target it has just acquired, and a target that
/// died in between is exactly the case the guard exists for.
HostOutcome m_is_visible(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsVisible: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && !slot->state.flags.hidden));
}

HostOutcome m_set_visible(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetVisible: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  WorldObject* slot = found == nullptr ? nullptr : world->find(found->id);
  if (slot == nullptr) return HostOutcome::failed("SetVisible: receiver does not resolve");
  // The argument is visibility; the stored bit is hiddenness.
  const bool visible = ctx.count() > 1 && ctx.arg(1).truthy_scalar();
  slot->state.flags.hidden = !visible;
  return HostOutcome::ok_void();
}

HostOutcome m_is_alive(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsAlive: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  // `action = 8` with an empty method is the dying state in the dumps, and all
  // 15 such objects have health 0 to 3. Health is the only part of that this
  // layer owns, so alive is health above zero and nothing more.
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && slot->state.health > 0));
}

HostOutcome m_is_dead(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsDead: no world");
  // `NamedObj::IsDead` asks about the object the name is bound to, which the
  // receiver is not: it is the table entry. `gbr.exe`'s message for it --
  // `Named unit %s is dead or not initialized!` -- names both halves, and an
  // unbound name reads as dead for the same reason an unresolvable handle does
  // below.
  const std::int32_t named = named_index_of(*world, ctx.arg(0));
  if (named != NamedObjectTable::kNoName) {
    const WorldObject* bound = world->find(world->named_objects().object(named));
    return HostOutcome::ok_with(Value::boolean(bound == nullptr || bound->state.health <= 0));
  }
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot == nullptr || slot->state.health <= 0));
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
///   * `Squad::pos` (0x00422cf0): the **front member's** position, and
///     `(-1, -1)` for a squad with none;
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
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(kHeldPosition));
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
/// pushes silently. Both push the same thing, so the difference is invisible
/// to a script and this engine keeps the two bodies apart anyway -- a
/// diagnostic that exists is worth having a place to go.
HostOutcome m_pos_rh(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("posRH: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(kHeldPosition));
  if (slot->state.flags.is_unit) {
    return HostOutcome::ok_with(pack_point(unit_pos_rh(*world, slot->id)));
  }
  return HostOutcome::ok_with(pack_point(slot->state.position));
}

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
/// The original reads `[obj+0xc8]`, which the object constructor fills from
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

HostOutcome m_stamina(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("stamina: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::integer(slot != nullptr ? slot->state.stamina : 0));
}

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
/// `WALL_PATROL.VS` and its 158 call sites**, the largest single script left.
///
/// 0x004dcb20 pops the three, refuses an object that does not resolve with
/// *"The function 'EnemyInRange' called for an uninitialized or invalid
/// object."* and `false`, and otherwise sweeps the object grid over
///
///     [x - range, x + range] x [y - range, y + range]
///
/// -- a **square**, built at 0x004dcba0 as four separate add/subtract pairs,
/// not the circle every `ObjsInCircle`-shaped entry point here uses. The
/// answer is whether the sweep found anything.
///
/// The per-object test is a functor 0x00514080 builds out of two words off the
/// asking object's player record: `[+0x10]`, the **enemyflags** mask that
/// `sim/player.hpp` records as the exact complement of the ceasefire bit, and
/// `[+0x18]`, the side mask. The first is `PlayerTable::is_enemy` and is what
/// this reproduces. **The second is carried and not modelled**: nothing
/// recovered here says how the functor combines them, and the shipped call --
/// a wall deciding whether to shoot, `EnemyInRange(.pos, nWallRange, list[0])`
/// -- cannot separate the two readings, because a wall and its patrol are
/// always on one side. Widening it if the mask turns out to narrow it would be
/// the error to watch for.
///
/// An object with no owner is nobody's enemy and is not found; the receiver's
/// own side is not searched for, because `is_enemy` is false for a player
/// against itself.
HostOutcome fn_enemy_in_range(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("EnemyInRange: no world");
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  if (ctx.count() < 3 || !is_point(ctx.arg(0)) || !ctx.arg(1).is_integer()) return no;
  const WorldObject* who = object_of(*world, ctx.arg(2));
  if (who == nullptr) return no;

  const Point centre = unpack_point(ctx.arg(0));
  const std::int32_t range = ctx.arg(1).as_integer();
  const PlayerTable& players = world->players();
  const PlayerId asking = who->state.owner;
  // The square, not a circle, over the object grid (0x004dbea0) -- so a held
  // object, which stands in no cell, is not there to be found: a garrison
  // resolved to its holder record at (0, 0) once made every wall near the
  // map's corner see an enemy. Only whether anything answers is returned, so
  // the order the sweep yields in cannot reach the result.
  std::vector<ObjectId> near;
  world->objects_located_in_rect(centre.x - range, centre.y - range, centre.x + range,
                                 centre.y + range, near);
  for (const ObjectId id : near) {
    const WorldObject* other = world->find(id);
    if (other == nullptr || other->state.flags.unspawned) continue;
    // No `owner == kNoPlayer` guard: `is_enemy` refuses an invalid player on
    // either side already, so one here could not be made to fail.
    if (!players.is_enemy(asking, other->state.owner)) continue;
    return HostOutcome::ok_with(Value::boolean(true));
  }
  return no;
}

HostOutcome m_radius(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("radius: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(class_int(*world, *slot, "radius")));
}

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
/// moves to this object. An empty string is refused by the worker's first
/// test and changes nothing.
HostOutcome m_set_name(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetName: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || !ctx.arg(1).is_string() || ctx.arg(1).as_string().empty()) {
    return HostOutcome::ok_void();
  }
  (void)world->named_objects().rebind(ctx.arg(1).as_string(), slot->id);
  return HostOutcome::ok_void();
}

/// `u.Disappear()` -- 1 site, `UNIT_DISAPPEAR.VS`, which then sleeps two
/// seconds and puts the unit back where it was.
///
/// 0x005d86d0: the unit's command object is destroyed and the path flag
/// cleared (0x005d3830), its record is reset, its animation is stopped
/// (0x0053d730), and it is placed at the static point `(-1, -1)` -- off the
/// map, which is what makes it vanish. Here that is: commands cleared, the
/// movement state stopped, and the position set to `kHeldPosition`, which the
/// world permits on an object that is not held.
HostOutcome m_disappear(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Disappear: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  const ObjectId id = slot->id;
  if (CommandSystem* commands = command_system(*world)) (void)commands->clear_commands(*world, id);
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
  // **Two registrations, one key.** `gbr.exe` declares `settlement/0` on `Obj`
  // (0x005ae7c0, 432 sites) and on `GAIKA` (0x004219a0), and this registry
  // keys on `(kind, name, arity)` -- so one body answers both and tells them
  // apart by what it is handed, exactly as `Size/0` does for its four
  // receivers. A GAIKA is a bare integer (`sim/gaika.hpp`), which is what makes
  // the discrimination possible at all.
  //
  // The GAIKA arm is the inverse of `Settlement::GetGaika`: a settlement node
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
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || slot->settlement == kNoObject) return HostOutcome::ok_with(invalid_object());
  return HostOutcome::ok_with(Value::object(ObjectRef{kTypeSettlement, slot->settlement}));
}

/// A class `radius` for a distance, floored at zero: a radius below zero -- how
/// the original's class constructor can leave a number no class declared --
/// must not make a distance grow.
[[nodiscard]] std::int32_t reach_radius(const World& world, const WorldObject& slot) {
  const std::int32_t radius = class_int(world, slot, "radius");
  return radius > 0 ? radius : 0;
}

/// `a.DistTo(b)` -- the gap between two edges, not two centres.
///
/// `gbr.exe` registers the name twice. The point form (0x005aa350) is
/// `isqrt(dx² + dy²)` **less the receiver's class `radius`** (`[class+0x2dc]`);
/// the object form (0x005aa3e0) calls 0x005a77b0, the same distance **less
/// both radii** -- the measure the conversation reach test and a hero's skill
/// reach are written in here already. Either can be negative, for a point
/// inside the receiver or two objects that overlap, and the original hands the
/// negative back.
///
/// It was centre to centre here, and that is what made a unit's `Goto` to an
/// object and its own `DistTo` disagree once `Goto` measured edges:
/// `SHAMAN_IDLE.VS` reads `if (.DistTo(.hero) > 250) { .Goto(.hero, 250, ...);
/// continue; }`, and a `Goto` that has arrived where `DistTo` says it has not
/// is a loop that never yields. The two measure the same gap in the original.
HostOutcome m_dist_to(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("DistTo: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(Value::integer(0));

  Point target{};
  std::int64_t reach = reach_radius(*world, *self);
  if (is_point(ctx.arg(1))) {
    target = unpack_point(ctx.arg(1));
  } else {
    const WorldObject* other = object_of(*world, ctx.arg(1));
    if (other == nullptr) return HostOutcome::ok_with(Value::integer(0));
    target = world->resolve_position(other->id);
    reach += reach_radius(*world, *other);
  }
  const Point here = world->resolve_position(self->id);
  const std::int64_t dx = static_cast<std::int64_t>(here.x) - target.x;
  const std::int64_t dy = static_cast<std::int64_t>(here.y) - target.y;
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(isqrt(dx * dx + dy * dy) - reach)));
}

/// `o.IsHeirOf("Class")` -- a test against the class *tree*, which is what the
/// dumps' queries filter on: they mix concrete classes with abstract ones.
HostOutcome m_is_heir_of(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsHeirOf: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || !ctx.arg(1).is_string()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const ClassFilter filter = ClassFilter::parse(ctx.arg(1).as_string(), world->class_graph());
  // An unresolvable name must not read as true: `match_all` with no class graph
  // would make every `IsHeirOf` succeed and every guard in the corpus vanish.
  if (filter.match_all) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(Value::boolean(world->matches_filter(*slot, filter)));
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
///   * `Building::minlevel/0` -- descriptor `+0x6bc`, the `minlevel` attribute.
///   * `Building::levelperitem/0` -- `+0x6c0`, `levelperitem`.
///   * `Building::itemtypes/0` -- the `std::string` at `+0x6a0`, `itemtype`.
///     The entry point is plural and the attribute is singular, because the
///     value is a comma-separated *list*: `RUIN_BEHAVIOR.VS` calls it twice and
///     runs `ParseStr` down it both times, once to count the entries and once
///     to walk to a random one.
///   * `Building::GetNumSentrySlots/0` -- `+0x2f4`, `num_sentry_slots`.
///   * `Building::GetSentryClassName/0` -- the `std::string` at `+0x2f8`,
///     `sentry_class_name`. The walls and gates declare it (`RSentry`,
///     `GSentry1`, ...); everything else inherits nothing and answers empty.
///   * `Obj::IsRam/0` and `Obj::IsPeasantAmbient/0` are 0x00540460 and
///     0x005403f0, which are both one `IsHeirOf` against a class the function
///     looks up once and caches in a file-static: `"RamUnit"` and
///     `"PeasantAmbient"`. Ancestry, not name equality, so a subclass matches.
///
/// **The invalid-receiver answers are the type's zero here, not `IsCentralBuliding`'s
/// surprising `true`.** Each thunk reports through 0x00686eb0 -- a bare `ret` in
/// retail -- and pushes 0, `false` or the empty string.
HostOutcome m_building_minlevel(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("minlevel: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::integer(slot == nullptr ? 0 : class_int(*world, *slot, "minlevel")));
}

HostOutcome m_building_level_per_item(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("levelperitem: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::integer(slot == nullptr ? 0 : class_int(*world, *slot, "levelperitem")));
}

HostOutcome m_building_num_sentry_slots(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetNumSentrySlots: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(
      Value::integer(slot == nullptr ? 0 : class_int(*world, *slot, "num_sentry_slots")));
}

/// One class property as a string, walking up the class tree, empty for
/// anything the graph cannot answer.
[[nodiscard]] HostOutcome class_text(CallContext& ctx, std::string_view key) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::ok_with(Value::string(std::string()));
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ClassGraph* graph = world->class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) {
    return HostOutcome::ok_with(Value::string(std::string()));
  }
  return HostOutcome::ok_with(
      Value::string(std::string(graph->property(slot->class_index, key))));
}

HostOutcome m_building_item_types(CallContext& ctx) { return class_text(ctx, "itemtype"); }

HostOutcome m_building_sentry_class_name(CallContext& ctx) {
  return class_text(ctx, "sentry_class_name");
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
/// **The string is a class name, and `projectile_class` is an inference.** What
/// is read is settled: 0x005a6c9d initialises a run of `std::string`s at
/// `+0x928`, `+0x944`, `+0x960`, `+0x97c`, `+0x998` (stride 0x1c) to that
/// sentinel; 0x0059c7c8 inherits each from the parent class when it is still
/// the sentinel; and 0x005b1d20 takes `+0x960`, looks it up **in the class
/// registry**, and reads a range-like number off the class it finds. So it
/// holds the name of another class. Which XML attribute fills it is *not*
/// settled: the class reader writes `projectile_shadow` into `+0x97c` and
/// `projectile_fire` into `+0x998`, and no instruction in the image writes
/// `+0x960` with that displacement at all -- the nearest evidence is the
/// literal `_projectile_class` at 0x007c8650, evidently composed with a prefix
/// at run time. `projectile_class` is the attribute that sits beside those two
/// in every shipped class that declares them, and it is what
/// `CombatProfile::projectile` already resolves from the other side, so it is
/// the reading taken here. The alternative worth naming is
/// `building_projectile_class`, which four shipped classes declare *as well*;
/// under that reading a tower would answer differently.
///
/// Either way this is not a range comparison and not a weapon-type test: a unit
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
/// sentinel -- then runs the nearest-match sweep at 0x005d4cd0 over the
/// unit's own **sight** (`[unit+0xd0]`) with the predicate at 0x005d3cb0:
///
///   * an **enemy** (0x005a78e0: the receiver's owner's row for the
///     candidate's owner, bit 0 clear) that can attack (`[cand+0xdc]`, the
///     class `damage`) is taken at once and ends the sweep -- that is how the
///     enemy reaches the script;
///   * otherwise the candidate must be a unit, not the receiver, carrying
///     `SyncFlags` bit 28 -- `ObjectFlags::training`, the bit `StartTraining`
///     sets -- and is scored: `40 * threat + distance`, plus 100 for a hero
///     (bit 24), plus 40 inside the receiver's `min_range`, plus 80 for a
///     unit with a path (bit 17); a candidate within `range` whose score is
///     under 50 is taken at once, and otherwise gets 100 off. The lowest
///     score wins, an earlier candidate keeping a tie.
///
/// `threat` is the decaying counter at `[unit+0x1b0]` (0x005d2800), which
/// this engine does not model -- `sim/combat.hpp` says so for the target
/// family that shares this formula -- so the term is 0 here and the score is
/// the distance with its four adjustments. The distance is centre to centre;
/// the original's is the same `isqrt` over the same squares (0x005a77b0).
/// The sweep here is in ascending id, the original's in grid order, which is
/// the tie the last clause names.
HostOutcome m_best_training_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("BestTrainingTarget: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(invalid_object());
  const Point at = world->resolve_position(self->id);
  if (at == kHeldPosition) return HostOutcome::ok_with(invalid_object());
  const std::int32_t max_range = class_int(*world, *self, "range");
  const std::int32_t min_range = class_int(*world, *self, "min_range");
  const PlayerTable& players = world->players();
  const PlayerId owner = self->state.owner;

  std::vector<ObjectId> around;
  world->objects_in_radius(at, self->sight, ClassFilter{}, around);
  ObjectId best = kNoObject;
  std::int64_t best_score = 0;
  for (const ObjectId id : around) {
    if (id == self->id) continue;
    const WorldObject* cand = world->find(id);
    if (cand == nullptr) continue;
    if (owner != kNoPlayer && cand->state.owner != kNoPlayer &&
        players.is_enemy(owner, cand->state.owner) && class_int(*world, *cand, "damage") > 0) {
      return HostOutcome::ok_with(object_value(id));
    }
    if (!cand->state.flags.is_unit || !cand->state.flags.training) continue;
    const Point there = world->resolve_position(id);
    const std::int64_t dx = there.x - at.x;
    const std::int64_t dy = there.y - at.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    std::int64_t score = distance;
    if (cand->state.flags.is_hero) score += 100;
    if (distance < min_range) score += 40;
    if (cand->state.flags.has_active_path) score += 80;
    if (distance <= max_range) {
      if (score < 50) return HostOutcome::ok_with(object_value(id));
      score -= 100;
    }
    if (best == kNoObject || score < best_score) {
      best = id;
      best_score = score;
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
/// **The gate is not reproduced.** `[cat+0x22c]` is the slot `RotateTo` reads
/// as a turn in progress, and this engine keeps no such slot -- its `RotateTo`
/// turns at once -- so the clear here is unconditional. The divergence is a
/// stopped catapult that was not mid-swing, whose towers keep their target
/// in the original; the target the catapult itself keeps is left alone in
/// both readings, which is what the script's comment asks for.
HostOutcome m_clear_tower_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClearTowerTarget: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  const EconomySystem* economy = economy_of(*world);
  const Settlement* set =
      economy == nullptr ? nullptr : economy->settlements().for_object(slot->id);
  if (set == nullptr) return HostOutcome::ok_void();
  for (const SettlementBuilding& building : set->buildings) {
    const WorldObject* member = world->find(building.object);
    if (member == nullptr || !class_is(*world, *member, "CatapultTower")) continue;
    if (ObjectState* state = world->mutable_state(building.object); state != nullptr) {
      state->ui_target = kNoObject;
    }
  }
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
/// **One term is not reproduced and is named.** When the catapult's action
/// state (`[obj+0x44]`) is between 4 and 7 -- a shot still in progress -- the
/// original (0x004e3326..0x004e3365) adds the time left on that state to the
/// answer. This engine keeps no such action state on a catapult, and the
/// three callers ask from idle, so the answer here is the interval less the
/// animation, floored at 0. A negative result is 0 there too (0x004e330c).
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

  wait -= attack_animation_ms(*slot);
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

/// Every `AsXxx` returns the handle unchanged when the object is of that native
/// class or derives from it, and an invalid handle otherwise. There is one
/// runtime representation for all handle types, so a downcast is a test, not a
/// conversion.
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

  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) {
    const SquadKey key = heroes->squads().squad_of(self);
    if (key.valid()) (void)heroes->squads().leave(*world, key, self);
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
/// spell manager at `[0x009a70a4]`: `GlobalSpellStart` (0x004dd440) appends
/// `{spell, player - 1, now, handle}` and `GlobalSpellStop` (0x004dd5b0)
/// removes the receiver's. The only registered reader is
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
///     has exactly two readers and both are in 0x00510b80..0x00510e24, the
///     block that prices a stonehenge spell against the caster: with the flag
///     set, one subtraction of `[0x00996e10]` is skipped. This engine has no
///     spell pricing to skip, so the flag would be state nothing could read --
///     which is the test `ObjectState::user` passes and this fails.
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
/// reads: a unit without `Parry` answers 0 whatever the field holds. That is
/// not redundancy here either -- `UNIT_LEAVE_PARRY_MODE_VERIFY.VS` asks
/// `u.GetParryMode() == 1` over a whole selection, and a unit that cannot parry
/// must not answer yes.
///
/// An unresolvable receiver answers 0 through the discard sink, like every
/// other getter in this file.
HostOutcome m_get_parry_mode(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetParryMode: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || !class_has_special(*world, *slot, 0)) {
    return HostOutcome::ok_with(Value::integer(0));
  }
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
/// what that returns. The core is the ritual's economy: 0 outright while the
/// stop flag at `[sacrifice+0x15c]` is set, otherwise `price` spread over the
/// druids on the list at `+0x14c` -- each paying from its health, none below
/// a tenth of its maximum -- and the health obtained is the answer. A ritual
/// with nobody on its list divides by that nobody at 0x00596204, which no
/// shipped path reaches: a sacrifice is started by the druid it lists.
///
/// **No druid can be on that list here**, for the reason `AddDruid` gives, so
/// the answer after the wait is what an empty ritual yields, 0, rather than
/// the fault -- the one reading available. The wait is reproduced because the
/// script's shape depends on it: one period, then the ghost dies. The receiver
/// is not checked, because the original does not check it either; it resolves
/// it or carries a null into the core.
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
/// 0x00542100 with the flag filter at 0x005c9f20, and for each collected
/// object that is a unit (`[obj+0x2c] & 0x400000`) whose tag at `[unit+0x14c]`
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
    if (slot == nullptr || !slot->state.flags.is_unit) continue;
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
/// `0x005ca6a0` resolves the receiver, casts it to the `CVXSacrifice` layout
/// (the type descriptor at 0x00824bcc) and hands it to 0x005ca5a0, which:
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
/// one-directional reading `Obj::IsEnemy` proved. An unowned unit is nobody's
/// enemy and is sheltered. An invalid receiver does nothing.
HostOutcome m_cover_of_mercy_action(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CoverOfMercyAction: no world");
  const WorldObject* cover = object_of(*world, ctx.arg(0));
  if (cover == nullptr) return HostOutcome::ok_void();
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
    // `is_enemy` answers false for `kNoPlayer` on its own, so the guard is an
    // equivalence the sweep labels; it is kept because the reading -- nobody's
    // unit is nobody's enemy -- is the point, not the table's edge.
    if (slot->state.owner != kNoPlayer && players.is_enemy(owner, slot->state.owner)) continue;
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

/// `Obj::IsMilitary` (0x005ad680) and `Obj::IsSentry` -- one shape twice.
///
/// Each holds a lazily-cached class handle -- `"Military"` and `"Sentry"` --
/// and runs an is-heir-of test against the receiver's class descriptor. An
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
/// building's *class* record, and `Teleport::exit_vector` (0x005cda00) reads two
/// instance fields that are copied verbatim from those same class scalars when
/// the teleport loads. Same numbers, no scaling, two accessors.
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
/// A class that declares neither anywhere up its chain answers `(-1, -1)`, and
/// so does a receiver that names nothing. That is a sentinel a caller could
/// test, and no caller does: all eleven sites scale it and add it to a position.
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
/// `0x005cdcb0` ORs the player's own bit into the mask at `[teleport+0x214]`.
/// The scripts do the movement themselves and call this on *both* ends of the
/// pair afterwards, which is what says it is a record rather than an action:
/// `HERO_TELEPORT.VS` writes `tel.Traverse(.player); tel.destination.Traverse
/// (.player); .SetPos(pt);` on three consecutive lines.
///
/// **The original's bounds check has no lower half.** `player - 1` is compared
/// against 16 with a signed test only, so `Traverse(0)` indexes one stride
/// below the player table. That is a fault and not a behaviour: this rejects
/// anything outside 1..16 and does nothing, which is also what the out-of-range
/// upper half does there.
///
/// A receiver that resolves to nothing is a no-op. The original prints on that
/// path -- and prints `Teleport::exit_vector`'s message rather than its own,
/// which is a copy-paste in the executable and not a clue about either.
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
  state->traversed_by |= 1u << static_cast<std::uint32_t>(player - 1);
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
/// `0x004dce60` ensures the building's enter lists are built, picks the deep
/// water list or the land list by whether the *unit* is water-borne, and returns
/// **the element nearest the unit's current position**. The picker (0x0056fba0)
/// is a linear scan taking an integer square root of each squared distance
/// before an unsigned compare, which orders identically to comparing the squared
/// distances -- so the root is cosmetic and is not reproduced.
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
///   * a building that names nothing or is dead answers **the unit's own
///     position** -- or `(0, 0)` when the unit is gone too;
///   * a unit argument that names nothing or is dead answers **the building's
///     position**;
///   * an empty list answers **the building's position**.
///
/// The original prints a diagnostic on each of the first two and a third
/// (`Buildilng::GetEnterPoint: No enter points found!`, the typo its own)
/// through a sink that is a bare `ret` in retail, so all three are ordinary
/// answers rather than failures.
///
/// **A `Teleport` is a `Building` here**: `HERO_TELEPORT.VS` calls this on one,
/// so the receiver test is "does this handle name an object", not a class test.
///
/// **Water-borne is read as "is a ship", and that is inferred.** The original
/// tests an authored flag on the unit's class record; which `.ent.xml` attribute
/// writes it is not recovered, and the native class is the only partition this
/// engine has that agrees with it on every shipped class.
///
/// Deterministic: no RNG anywhere on this path. Its sibling `GetExitPoint`
/// draws one, which is why that name is not here.
HostOutcome m_get_enter_point(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetEnterPoint: no world");
  const auto living = [&](const script::Value& value) -> const WorldObject* {
    const WorldObject* slot = object_of(*world, value);
    return (slot != nullptr && slot->state.health > 0) ? slot : nullptr;
  };
  const WorldObject* building = living(ctx.arg(0));
  const WorldObject* unit = ctx.count() > 1 ? living(ctx.arg(1)) : nullptr;

  if (building == nullptr) {
    return HostOutcome::ok_with(
        pack_point(unit == nullptr ? Point{0, 0} : world->resolve_position(unit->id)));
  }
  const Point home = world->resolve_position(building->id);
  if (unit == nullptr) return HostOutcome::ok_with(pack_point(home));

  const bool water = unit->object != nullptr && unit->object->is_a(NativeClass::ship);
  return HostOutcome::ok_with(pack_point(enter_point_near(*world, building->id, unit->id, water)));
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
/// The enter half above tests health and this does not, which is a difference
/// between two siblings and is left standing on purpose: 0x004dce60 has no such
/// test either, so either the original's handle table stops resolving a dead
/// object -- in which case both are right and the test is redundant -- or it does
/// not, in which case `GetEnterPoint`'s is a divergence. Nothing recovered so far
/// settles which. All 18 enter sites pass `this` as the unit -- the script's own
/// object, which is alive because it is running -- so only the *building* side
/// of that test is reachable at all from the shipped corpus.
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
    // The same authored class flag `GetEnterPoint` reads, and the same
    // inference standing in for it: the native class is the only partition this
    // engine has that agrees with it on every shipped class. The original
    // dereferences the argument's class record without checking the handle
    // first; an unresolvable one is land here rather than a crash.
    const WorldObject* mover = object_of(*world, ctx.arg(1));
    water = mover != nullptr && mover->object != nullptr && mover->object->is_a(NativeClass::ship);
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
[[nodiscard]] std::uint32_t pack_unit_flags(const ObjectFlags& flags) noexcept {
  return (flags.autocast ? 0x00000004u : 0u) |    // bit 2  Hero::SetAutocast
         (flags.no_ai ? 0x00040000u : 0u) |       // bit 18 UNITFLAG_NOAI
         (flags.entering ? 0x00100000u : 0u) |    // bit 20 Unit::SetEntering
         (flags.in_air ? 0x00400000u : 0u) |      // bit 22 Flying::IsInAir
         (flags.noselect ? 0x00800000u : 0u) |    // bit 23 Unit::SetNoselectFlag
         (flags.on_minimap ? 0x04000000u : 0u) |  // bit 26 Unit::SetMinimapFlag
         (flags.cursed ? 0x08000000u : 0u);       // bit 27 Unit::IsCursed
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
      Value::boolean((pack_unit_flags(found->state.flags) & mask) != 0));
}

/// `Unit::SetNoselectFlag(bool)` -- 6 sites, every one of them passing `true`.
///
/// `0x005d8750` writes bit 23 of `[obj+0x194]`, and **the mask is the same
/// `0x00800000` the Building bit uses on the other word**, which is the sort of
/// coincidence worth writing down once rather than rediscovering.
///
/// It follows the *member* shape of `SetNoAIFlag` (0x005de600) and not the free
/// one: the only gate is whether the receiver resolves. No class test, no
/// `IsDead` probe.
///
/// **One side effect is transcribed and one is not.** On a false-to-true
/// transition the original calls `0x0041eee0`, which drops the object out of
/// the current selection -- so flipping the bit on a selected unit deselects
/// it, and an implementation that only wrote the bit would leave it selected.
/// That is done here through `SelectionTable`. What is *not* done is the rest
/// of `0x0041eee0`, which also touches the minimap and the interface; those are
/// not this layer's.
HostOutcome m_set_noselect_flag(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetNoselectFlag: no world");
  const WorldObject* found = object_of(*world, ctx.arg(0));
  // An unresolvable receiver writes nothing and says nothing.
  if (found == nullptr) return HostOutcome::ok_void();
  const bool set = ctx.count() > 1 && ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  ObjectState* state = world->mutable_state(found->id);
  if (state == nullptr) return HostOutcome::ok_void();
  // The transition, not the value: `0x005d8750` tests the bit before it writes,
  // so setting a flag that is already set deselects nothing.
  if (set && !state->flags.noselect) {
    if (HostContext* context = host_context_of(ctx);
        context != nullptr && context->selections != nullptr) {
      (void)context->selections->forget(found->id);
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

/// `Unit::Curse()` and `Unit::IsCursed()` -- 3 sites each, one script between
/// them.
///
/// `SHAMAN_IDLE.VS` is the entire corpus for both, and it round-trips them:
/// `if (tgt.IsCursed) break;` guards the loop that eleven lines later runs
/// `tgt.Curse();`. That is the behaviour reproduced here, and
/// `ObjectFlags::cursed` says at length what is *not* established -- the
/// original's `Curse` hands off to the spell machinery at 0x005d4880 and no
/// write to the bit was found anywhere in `.text`.
///
/// `IsCursed` on an unresolvable receiver answers **false** after a complaint,
/// which is 0x005d896f writing a zero byte; the shaman's guard reads that as
/// "not cursed, go ahead", which is the branch it wants for a target that has
/// stopped existing.
HostOutcome m_curse(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Curse: no world");
  for (const ObjectId id : receiver_objects(*world, ctx.arg(0))) {
    if (ObjectState* state = world->mutable_state(id); state != nullptr) state->flags.cursed = true;
  }
  return HostOutcome::ok_void();
}

HostOutcome m_is_cursed(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsCursed: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && slot->state.flags.cursed));
}

/// `Unit::SetLastAttackTime()` -- 40 sites, and it stores nothing here.
///
/// 0x005d7fc0 is `[obj+0x1a4] = now` and nothing else. The field has a second
/// writer at 0x005d40ad -- the same `now`, on whatever path a unit actually
/// attacks -- and, in the whole executable, **one reader: `Unit::Dump`**, which
/// formats it into a diagnostic string (0x005d2bb0). No host entry point
/// returns it, so no script can read it back; a byte-pattern sweep for
/// `[reg+0x1a4]` across `.text` finds 42 references and every one that is on a
/// unit is one of those three.
///
/// So the field is unobservable from script, and storing it would be a hashed,
/// serialised int that nothing could ever look at. `Unit::user` is stored for
/// the opposite reason -- it has a registered getter, so a script writes it and
/// reads it back -- and the two cases are worth keeping apart.
///
/// **What the shipped sites want is a stealth timer this engine does not
/// have.** `UNIT_GIVEITEM.VS` writes `if (.HasSpecial(sneak))
/// .SetLastAttackTime(); // mountaineer patch`, and `SQUADMONITOR.VS` pairs it
/// with `SetVisible(true)` twice -- both of which read as *a unit that just
/// attacked cannot stay hidden*. `Obj::CanSee` here is the `hidden` flag and
/// nothing else (0x005ab9d0 is the same in retail), so there is no clock for
/// this to feed. The day one exists, this is where it writes.
///
/// The receiver is still resolved, because the original resolves it and
/// because an entry point that answers without looking is a test of nothing.
HostOutcome m_set_last_attack_time(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetLastAttackTime: no world");
  // 0x005d7fe4 complains and returns for a receiver that does not resolve.
  (void)object_of(*world, ctx.arg(0));
  return HostOutcome::ok_void();
}

/// `MIN(a, b)` and `MAX(a, b)` -- 3 and 2 shipped sites, all integers.
///
/// **Each name is registered twice in `gbr.exe`**, once over `(int, int)` at
/// 0x00694a60 / 0x00694a90 and once over a second type at 0x00695020 /
/// 0x00695050, and the second pair's type code is one the 35 `RegisterType`
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

/// `Unit::HasSpecial(int)` -- 205 hits in the corpus sweep over four scripts.
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
/// It takes the first match in the scan's cell order and stops, which is not the
/// nearest. This engine has no 256-unit object grid, so the order it can offer
/// is ascending object id -- spawn order, deterministic, and equally not the
/// nearest. The divergence is recorded rather than papered over: the shipped
/// caller, `CROW_IDLE.VS`, uses the answer only to pick a direction to fly in
/// when its own 900-unit query found no flock.
HostOutcome m_find_near_bird(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindNearBird: no world");
  const WorldObject* self = object_of(*world, ctx.arg(0));
  if (self == nullptr) return HostOutcome::ok_with(invalid_object());
  const Point from = world->resolve_position(self->id);
  constexpr std::int64_t kBirdRange = 2048;
  for (const WorldObject& other : world->objects()) {
    if (other.id == self->id) continue;
    if (other.internal != InternalKind::none) continue;
    // "The same entity class" is the class *pointer* in the original -- two
    // objects of one `<class>` -- which here is the class graph index.
    if (other.class_index == kNoClass || other.class_index != self->class_index) continue;
    const Point at = world->resolve_position(other.id);
    const std::int64_t dx = at.x - from.x;
    const std::int64_t dy = at.y - from.y;
    if (dx * dx + dy * dy > kBirdRange * kBirdRange) continue;
    return HostOutcome::ok_with(object_value(other.id));
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
/// `0x005ab9d0` is five instructions of substance: if the *observed* object does
/// not carry bit 21 of `[obj+0x2c]`, return true; otherwise AND the observer's
/// team-vision mask with the observed object's per-player visibility bits. No
/// line of sight, no radius, no distance, no terrain, no exploration state. A
/// unit that is not hiding is visible from anywhere on the map.
///
/// That is the finding, and it is a good one: the name promises fog and the
/// body does not need any. This engine models neither the sixteen visibility
/// bits nor the team-vision mask -- `ExploreArea` is unimplemented for the same
/// reason -- so a hidden object reads as invisible to everybody, which is the
/// conservative half of the original's answer and the one that cannot make a
/// druid heal something it should not be able to see.
///
/// **The argument is the observed object and the receiver is the observer.**
/// `0x005ab9d0` pops the argument first and complains about it by name --
/// *"Parameter #1 in function 'Obj::CanSee' is uninitialized or invalid
/// object."* -- and never checks the receiver at all: with a stealthed argument
/// it dereferences the receiver's owner unconditionally and faults. That is a
/// fault, not a behaviour, so an unresolvable observer answers false here.
///
/// Every caster script calls it twice per target -- once as a validity gate
/// before approaching and again after `Sleep(.TimeToActionMoment())`,
/// immediately before the effect lands -- which is why it leads the site census
/// among unimplemented members.
HostOutcome m_can_see(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CanSee: no world");
  const WorldObject* observer = object_of(*world, ctx.arg(0));
  const WorldObject* seen = object_of(*world, ctx.arg(1));
  if (observer == nullptr || seen == nullptr) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(Value::boolean(!seen->state.flags.hidden));
}

/// `SetMessengerStatus(bool)` -- 26 sites, registered on `Obj` and on `Query`.
///
/// Bit 26 of `[obj+0x2c]`, and see `ObjectFlags::messenger` for what reads it.
/// One body for both receivers, because this registry keys on
/// (kind, name, arity) and `receiver_objects` resolves all three handle shapes;
/// the original's `Query` form (`0x00579c70`) is the same write applied to each
/// member, with one extra complaint for an empty query that the retail sink
/// discards like every other.
HostOutcome m_set_messenger_status(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetMessengerStatus: no world");
  const bool on = ctx.count() > 1 && ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  for (const ObjectId id : receiver_objects(*world, ctx.arg(0))) {
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
// gate on the receiver's class being an heir of `Stonehenge`, both scan a circle
// of the building's **own `sight`** -- `entityDef + 0x2d0`, which the class
// reader fills from the `sight` property -- and both run the visitor at
// `0x004db580`.
//
// **The count is "mages of one alliance, provided nobody hostile is also
// here".** The visitor skips the dead and the spawn templates, then requires an
// heir of `Military` *or* `BaseMage`; the first qualifying object it meets sets
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
[[nodiscard]] std::int32_t stonehenge_mages(World& world, const WorldObject& henge,
                                            PlayerId reference) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return 0;
  const ClassFilter military = ClassFilter::parse("Military", graph);
  const ClassFilter mage = ClassFilter::parse("BaseMage", graph);
  if (military.match_all && mage.match_all) return 0;

  std::vector<ObjectId> found;
  world.objects_in_radius(world.resolve_position(henge.id), henge.sight, ClassFilter{}, found);
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
///   * skip an heir of `PeasantAmbient`, and of `Animal` -- see below;
///   * skip a class whose `ignore_passability` property is set;
///   * **friend** when bit 0 of the gate's own diplomacy row for that owner is
///     set, which is `PlayerTable::is_enemy` negated and is not the transpose;
///   * **enemy** otherwise, but only when the object is not hidden: a stealthed
///     enemy is passed over entirely rather than counted either way.
///
/// Both fields are set to the literal 1 rather than incremented, so they are
/// booleans in the original too and `!= 0` loses nothing.
///
/// **One clause is widened, deliberately.** The original skips an `Animal` only
/// when the dword at `[obj+0xdc]` is zero -- an animal with a non-zero one *is*
/// counted -- and that field has two comparison sites in the whole executable
/// and no identified meaning. Rather than invent a condition, every `Animal` is
/// skipped here, which is the reading that agrees with the original wherever
/// `[obj+0xdc]` is zero and is named where it might not be.
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
      if (class_is(world, *other, "Animal")) continue;
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
  // A receiver that resolves to nothing is a no-op, not a refusal: both bodies
  // format a complaint into `0x00686eb0`, which is a bare `ret` in retail.
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
  // It also reads before it refreshes, where every other `Query` member
  // refreshes first. That inversion is unobservable here: this engine caches no
  // query result at all (`World::evaluate_query`'s note says why), so there is
  // no stale list for the order to matter to.
  std::vector<ObjectId> found;
  world->evaluate_query(slot->id, found);
  for (const ObjectId id : found) {
    const WorldObject* target = world->find(id);
    if (target == nullptr || target->state.flags.is_building) continue;
    erase_one(ctx, *world, id);
  }
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

HostOutcome m_query_is_empty(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsEmpty: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  if (slot == nullptr || slot->internal != InternalKind::query) {
    return HostOutcome::failed("IsEmpty: receiver is not a Query");
  }
  std::vector<ObjectId> found;
  return HostOutcome::ok_with(Value::boolean(world->evaluate_query(slot->id, found) == 0));
}

// --------------------------------------------------------------------------
// members: point
// --------------------------------------------------------------------------

HostOutcome m_point_x(CallContext& ctx) {
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("x: receiver is not a point");
  return HostOutcome::ok_with(Value::integer(unpack_point(ctx.arg(0)).x));
}

HostOutcome m_point_y(CallContext& ctx) {
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("y: receiver is not a point");
  return HostOutcome::ok_with(Value::integer(unpack_point(ctx.arg(0)).y));
}

/// `pt.Set(x, y)` -- mutates the receiver, which is what the argument window's
/// mutability is for: the VM copies argument 0 back into the caller's local.
HostOutcome m_point_set(CallContext& ctx) {
  if (!ctx.arg(1).is_integer() || !ctx.arg(2).is_integer()) {
    return HostOutcome::failed("Set: expected two integers");
  }
  ctx.out(0) = pack_point(Point{ctx.arg(1).as_integer(), ctx.arg(2).as_integer()});
  return HostOutcome::ok_void();
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
// x87 floating point** -- `fildl`, `fmull 0x7ac490`, `fsin`, `fcos`, and two
// calls to the MSVC `_ftol2` truncation helper at 0x00763870 -- and the
// constant it multiplies by is 0.017453292519444445, which is not `pi/180` but
// `3.1415926535/180`, pi truncated to eleven digits. This core has no floating
// point and will not acquire any, so the reproduction is integer, and it is
// exact where the corpus lives and labelled where it is not.
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
// ## The cardinals are exact, and that is 82% of the corpus
//
// 316 of the 385 sites pass the literal 90, and there `cos(90 * K)` is not zero
// -- it is 4.4896592e-11 -- while `sin` is exactly 1. Because every coordinate
// a map can hold is under 32,768, the product `x * 4.49e-11` is under 6e-6, far
// inside one unit, so truncation toward zero turns the tiny term into an exact
// +/-1 adjustment decided by two signs. **`Rot(90)` is therefore not
// `(y, -x)`**: it is that, biased. `(3, -7)` becomes `(-6, -3)`, and the
// shipped four-`Rot(90)` patrol square does not close -- it drifts by one unit
// per corner. Reproducing that needs no floating point at all, only the sign of
// each vanishing term, which is what `Cardinal` below carries.
//
// `Rot(360)` is not the identity for the same reason, and one shipped site
// reaches it (`rand(361)`); `Rot(0)` is, because `sin(0)` and `cos(0)` are
// exact.
//
// ## Everything else is a Q30 table, and diverges by at most one unit
//
// The other 69 sites pass a random angle, where matching the x87 bit-for-bit
// would mean shipping `fsin`/`fcos`'s own outputs -- measurable only on an x87,
// not readable from `gbr.exe`. The table below is `sin(d * K)` and
// `cos(d * K)` at Q30, generated from **the retail constant** rather than from
// `pi/180`, so it carries the original's own eleven-digit truncation. A Q30
// product of a coordinate under 32,768 is within about 3e-5 of the double the
// original computes, so the two agree unless the true product falls that close
// to an integer. **That residue is the labelled divergence**: at most one unit
// per component, on angles that were drawn from the RNG in the first place.
constexpr std::int32_t kQ30Shift = 30;

constexpr std::int32_t kSinQ30[361] = {
    0, 18739379, 37473049, 56195305, 74900443, 93582766, 112236583, 130856211,
    149435979, 167970228, 186453311, 204879599, 223243478, 241539355, 259761657, 277904834,
    295963357, 313931728, 331804471, 349576144, 367241333, 384794656, 402230767, 419544355,
    436730145, 453782903, 470697435, 487468587, 504091252, 520560366, 536870912, 553017922,
    568996477, 584801711, 600428808, 615873009, 631129609, 646193961, 661061475, 675727625,
    690187940, 704438018, 718473518, 732290163, 745883746, 759250125, 772385229, 785285058,
    797945680, 810363241, 822533958, 834454122, 846120104, 857528349, 868675383, 879557810,
    890172315, 900515665, 910584710, 920376381, 929887697, 939115760, 948057759, 956710970,
    965072759, 973140576, 980911966, 988384560, 995556083, 1002424350, 1008987269, 1015242840,
    1021189159, 1026824413, 1032146887, 1037154959, 1041847103, 1046221891, 1050277989, 1054014162,
    1057429273, 1060522280, 1063292242, 1065738315, 1067859754, 1069655912, 1071126243, 1072270298,
    1073087729, 1073578288, 1073741824, 1073578288, 1073087729, 1072270298, 1071126243, 1069655912,
    1067859754, 1065738315, 1063292242, 1060522280, 1057429273, 1054014162, 1050277989, 1046221891,
    1041847103, 1037154959, 1032146887, 1026824413, 1021189159, 1015242840, 1008987269, 1002424350,
    995556083, 988384560, 980911966, 973140576, 965072759, 956710971, 948057759, 939115760,
    929887697, 920376381, 910584710, 900515665, 890172315, 879557810, 868675383, 857528349,
    846120104, 834454122, 822533958, 810363241, 797945680, 785285058, 772385229, 759250125,
    745883746, 732290163, 718473518, 704438018, 690187941, 675727625, 661061476, 646193961,
    631129609, 615873009, 600428808, 584801711, 568996477, 553017922, 536870912, 520560366,
    504091252, 487468587, 470697435, 453782903, 436730145, 419544355, 402230767, 384794656,
    367241333, 349576144, 331804471, 313931728, 295963357, 277904834, 259761658, 241539355,
    223243478, 204879599, 186453311, 167970228, 149435979, 130856211, 112236583, 93582766,
    74900443, 56195305, 37473049, 18739379, 0, -18739379, -37473049, -56195305,
    -74900443, -93582766, -112236583, -130856211, -149435979, -167970228, -186453311, -204879599,
    -223243478, -241539355, -259761657, -277904833, -295963357, -313931727, -331804471, -349576144,
    -367241332, -384794656, -402230767, -419544354, -436730145, -453782903, -470697435, -487468587,
    -504091252, -520560366, -536870912, -553017922, -568996477, -584801711, -600428808, -615873009,
    -631129609, -646193961, -661061475, -675727624, -690187940, -704438018, -718473518, -732290163,
    -745883746, -759250125, -772385229, -785285057, -797945680, -810363241, -822533958, -834454122,
    -846120104, -857528349, -868675383, -879557810, -890172315, -900515665, -910584710, -920376381,
    -929887697, -939115760, -948057759, -956710970, -965072759, -973140576, -980911966, -988384560,
    -995556083, -1002424350, -1008987269, -1015242840, -1021189158, -1026824413, -1032146887, -1037154959,
    -1041847103, -1046221891, -1050277989, -1054014162, -1057429273, -1060522280, -1063292242, -1065738315,
    -1067859754, -1069655912, -1071126243, -1072270298, -1073087729, -1073578288, -1073741824, -1073578288,
    -1073087729, -1072270298, -1071126243, -1069655912, -1067859754, -1065738315, -1063292242, -1060522280,
    -1057429273, -1054014162, -1050277989, -1046221891, -1041847103, -1037154959, -1032146887, -1026824413,
    -1021189159, -1015242840, -1008987269, -1002424350, -995556083, -988384560, -980911966, -973140576,
    -965072759, -956710971, -948057759, -939115760, -929887697, -920376381, -910584710, -900515665,
    -890172315, -879557810, -868675383, -857528349, -846120104, -834454122, -822533958, -810363241,
    -797945681, -785285058, -772385229, -759250125, -745883746, -732290163, -718473518, -704438019,
    -690187941, -675727625, -661061476, -646193961, -631129609, -615873009, -600428808, -584801711,
    -568996477, -553017922, -536870912, -520560366, -504091252, -487468587, -470697435, -453782903,
    -436730145, -419544355, -402230767, -384794656, -367241333, -349576144, -331804471, -313931728,
    -295963357, -277904834, -259761658, -241539356, -223243478, -204879599, -186453311, -167970228,
    -149435979, -130856211, -112236583, -93582766, -74900444, -56195305, -37473049, -18739379,
    0,
};

constexpr std::int32_t kCosQ30[361] = {
    1073741824, 1073578288, 1073087729, 1072270298, 1071126243, 1069655912, 1067859754, 1065738315,
    1063292242, 1060522280, 1057429273, 1054014162, 1050277989, 1046221891, 1041847103, 1037154959,
    1032146887, 1026824413, 1021189159, 1015242840, 1008987269, 1002424350, 995556083, 988384560,
    980911966, 973140576, 965072759, 956710970, 948057759, 939115760, 929887697, 920376381,
    910584710, 900515665, 890172315, 879557810, 868675383, 857528349, 846120104, 834454122,
    822533958, 810363241, 797945680, 785285058, 772385229, 759250125, 745883746, 732290163,
    718473518, 704438018, 690187940, 675727625, 661061476, 646193961, 631129609, 615873009,
    600428808, 584801711, 568996477, 553017922, 536870912, 520560366, 504091252, 487468587,
    470697435, 453782903, 436730145, 419544355, 402230767, 384794656, 367241333, 349576144,
    331804471, 313931728, 295963357, 277904834, 259761657, 241539355, 223243478, 204879599,
    186453311, 167970228, 149435979, 130856211, 112236583, 93582766, 74900443, 56195305,
    37473049, 18739379, 0, -18739379, -37473049, -56195305, -74900443, -93582766,
    -112236583, -130856211, -149435979, -167970228, -186453311, -204879599, -223243478, -241539355,
    -259761657, -277904834, -295963357, -313931727, -331804471, -349576144, -367241332, -384794656,
    -402230767, -419544355, -436730145, -453782903, -470697435, -487468587, -504091252, -520560366,
    -536870912, -553017922, -568996477, -584801711, -600428808, -615873009, -631129609, -646193961,
    -661061475, -675727625, -690187940, -704438018, -718473518, -732290163, -745883746, -759250125,
    -772385229, -785285058, -797945680, -810363241, -822533958, -834454122, -846120104, -857528349,
    -868675383, -879557810, -890172315, -900515665, -910584710, -920376381, -929887697, -939115760,
    -948057759, -956710970, -965072759, -973140576, -980911966, -988384560, -995556083, -1002424350,
    -1008987269, -1015242840, -1021189159, -1026824413, -1032146887, -1037154959, -1041847103, -1046221891,
    -1050277989, -1054014162, -1057429273, -1060522280, -1063292242, -1065738315, -1067859754, -1069655912,
    -1071126243, -1072270298, -1073087729, -1073578288, -1073741824, -1073578288, -1073087729, -1072270298,
    -1071126243, -1069655912, -1067859754, -1065738315, -1063292242, -1060522280, -1057429273, -1054014162,
    -1050277989, -1046221891, -1041847103, -1037154959, -1032146887, -1026824413, -1021189159, -1015242840,
    -1008987269, -1002424350, -995556083, -988384560, -980911966, -973140576, -965072759, -956710971,
    -948057759, -939115760, -929887697, -920376381, -910584710, -900515665, -890172315, -879557810,
    -868675383, -857528349, -846120104, -834454122, -822533958, -810363241, -797945680, -785285058,
    -772385229, -759250125, -745883746, -732290163, -718473518, -704438019, -690187941, -675727625,
    -661061476, -646193961, -631129609, -615873009, -600428808, -584801711, -568996477, -553017922,
    -536870912, -520560366, -504091252, -487468587, -470697435, -453782903, -436730145, -419544355,
    -402230767, -384794656, -367241333, -349576144, -331804471, -313931728, -295963357, -277904834,
    -259761658, -241539356, -223243478, -204879599, -186453311, -167970228, -149435979, -130856211,
    -112236583, -93582766, -74900443, -56195305, -37473049, -18739379, 0, 18739379,
    37473049, 56195305, 74900443, 93582766, 112236583, 130856211, 149435979, 167970228,
    186453311, 204879599, 223243478, 241539355, 259761657, 277904833, 295963357, 313931727,
    331804471, 349576144, 367241332, 384794656, 402230767, 419544354, 436730145, 453782903,
    470697435, 487468587, 504091252, 520560366, 536870912, 553017922, 568996477, 584801711,
    600428808, 615873009, 631129609, 646193961, 661061475, 675727624, 690187940, 704438018,
    718473518, 732290163, 745883746, 759250125, 772385229, 785285057, 797945680, 810363241,
    822533958, 834454122, 846120104, 857528349, 868675383, 879557810, 890172315, 900515665,
    910584710, 920376381, 929887697, 939115760, 948057759, 956710970, 965072759, 973140576,
    980911966, 988384560, 995556083, 1002424350, 1008987269, 1015242840, 1021189158, 1026824413,
    1032146887, 1037154959, 1041847103, 1046221891, 1050277989, 1054014162, 1057429273, 1060522280,
    1063292242, 1065738315, 1067859754, 1069655912, 1071126243, 1072270298, 1073087729, 1073578288,
    1073741824,
};

/// A cardinal angle, where the true cosine or sine is a vanishing quantity
/// rather than zero. `unit` is the exact part; `epsilon` is the *sign* of what
/// is left, and its magnitude never matters because it cannot reach one unit.
struct Cardinal {
  std::int32_t degrees;
  std::int32_t cos_unit;
  std::int32_t cos_epsilon;
  std::int32_t sin_unit;
  std::int32_t sin_epsilon;
};

/// Read off the five angles a shipped script can pass, evaluated with the
/// retail constant: cos(90) = +4.49e-11, sin(180) = +8.98e-11,
/// cos(270) = -1.35e-10, sin(360) = -1.80e-10. Only `Rot(0)` is clean.
constexpr Cardinal kCardinals[] = {
    {0, 1, 0, 0, 0},   {90, 0, +1, 1, 0},   {180, -1, 0, 0, +1},
    {270, 0, -1, -1, 0}, {360, 1, 0, 0, -1},
};

/// `trunc(exact + epsilon)` where `|epsilon| < 1` and `sign` is its sign.
///
/// Truncation is toward zero, so a vanishing term only ever moves the result
/// when it points *away* from zero from a non-zero integer.
[[nodiscard]] std::int64_t truncate_with(std::int64_t exact, std::int32_t sign) noexcept {
  if (sign > 0 && exact < 0) return exact + 1;
  if (sign < 0 && exact > 0) return exact - 1;
  return exact;
}

[[nodiscard]] constexpr std::int32_t sign_of(std::int64_t v) noexcept {
  return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

}  // namespace

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
  return id;
}

Point rotate_like_gbr(Point v, std::int32_t degrees) noexcept {
  const std::int64_t x = v.x;
  const std::int64_t y = v.y;

  for (const Cardinal& c : kCardinals) {
    // `sin` is odd and `cos` even, in the FPU as in the mathematics, so a
    // negative cardinal is the same angle with both sine terms flipped.
    for (const std::int32_t turn : {1, -1}) {
      if (degrees != c.degrees * turn) continue;
      const std::int64_t sin_unit = static_cast<std::int64_t>(c.sin_unit) * turn;
      const std::int64_t sin_eps = static_cast<std::int64_t>(c.sin_epsilon) * turn;
      // The vanishing terms, kept as signs. Their magnitudes are under 6e-6 for
      // any coordinate a map can hold, so they can only ever move a truncation
      // by one -- and only away from zero.
      const std::int32_t x_eps = sign_of(x * c.cos_epsilon + y * sin_eps);
      const std::int32_t y_eps = sign_of(y * c.cos_epsilon - x * sin_eps);
      return Point{
          static_cast<std::int32_t>(truncate_with(x * c.cos_unit + y * sin_unit, x_eps)),
          static_cast<std::int32_t>(truncate_with(y * c.cos_unit - x * sin_unit, y_eps))};
    }
  }

  // The table path. Negative angles use the odd/even symmetry, which is exact;
  // beyond a full turn the angle is reduced, which the original does *not* do
  // -- it hands the raw product to `fsin`, whose own argument reduction uses a
  // different pi. No shipped site is outside [-30, 360].
  std::int32_t d = degrees % 360;
  const bool negate = d < 0;
  if (negate) d = -d;
  const std::int64_t cos_q = kCosQ30[d];
  const std::int64_t sin_q = negate ? -static_cast<std::int64_t>(kSinQ30[d]) : kSinQ30[d];
  constexpr std::int64_t kOne = std::int64_t{1} << kQ30Shift;
  // Integer division truncates toward zero, which is what `_ftol2` does.
  return Point{static_cast<std::int32_t>((x * cos_q + y * sin_q) / kOne),
               static_cast<std::int32_t>((y * cos_q - x * sin_q) / kOne)};
}

namespace {

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
  const std::int64_t length = ctx.arg(1).as_integer();
  const std::int64_t x = v.x;
  const std::int64_t y = v.y;
  const std::int64_t current = isqrt(x * x + y * y);
  if (current == 0) {
    ctx.out(0) = pack_point(Point{0, static_cast<std::int32_t>(length)});
    return HostOutcome::ok_void();
  }
  ctx.out(0) = pack_point(Point{static_cast<std::int32_t>(x * length / current),
                                static_cast<std::int32_t>(y * length / current)});
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
  const std::int64_t x = v.x;
  const std::int64_t y = v.y;
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(isqrt(x * x + y * y))));
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
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
  const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(isqrt(dx * dx + dy * dy))));
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

std::vector<ObjectId> receiver_objects(World& world, const script::Value& receiver,
                                      bool accept_query) {
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
    if (accept_query) world.evaluate_query(slot->id, ids);
    return ids;
  }
  ids.push_back(slot->id);
  return ids;
}

void install_deferred_erase(script::Scheduler& scheduler) noexcept {
  scheduler.set_step_hook([](void* user, script::Scheduler& sched, script::ScriptId) {
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
    if (pending == kNoObject) return;
    // Cleared before the erase, not after. The original clears after
    // (0x005a7ad3) because its overrides read the latch to decide what to skip;
    // here nothing reads it, and clearing first makes a re-entrant erase
    // impossible rather than merely unlikely.
    perform_erase(&sched, world, pending);
    world.clear_deferred_erase();
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
  return Value::object(ObjectRef{kTypeObj, items[static_cast<std::size_t>(index)]});
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
  def(kFree, "ObjsInSight", 2, &fn_objs_in_sight);           // 36
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
  def(kFree, "VisibleObjsInSight", 2, &fn_visible_objs_in_sight);  // 4
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
  def(kMember, "ShowBuildAnimation", 1, &f_presentation_void);          // 1
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
