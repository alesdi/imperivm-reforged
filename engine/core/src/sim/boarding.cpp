// The list of units a ship is waiting for.
// See include/imperivm/core/sim/boarding.hpp.

#include "imperivm/core/sim/boarding.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#include <utility>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/entrance.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::ObjectRef;
using script::Value;

constexpr script::TypeId kTypeObj = 1;

/// FNV-1a, the same constants every other table in this tree folds with.
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix(std::uint64_t& accumulator, std::uint64_t value) noexcept {
  accumulator ^= value;
  accumulator *= kFnvPrime;
}

[[nodiscard]] Value object_value(ObjectId id) {
  if (id == kNoObject) return Value::object(ObjectRef{script::kNoType, 0});
  return Value::object(ObjectRef{kTypeObj, id});
}

[[nodiscard]] Value invalid_object() { return Value::object(ObjectRef{script::kNoType, 0}); }

/// The object a handle names, or null. Deliberately narrower than
/// `world_host.cpp`'s: every receiver and argument in this family arrives from
/// an `.AsShip` or an `.AsUnit`, so there is no `NamedObj` to convert.
[[nodiscard]] const WorldObject* object_of(const World& world, const Value& value) noexcept {
  if (!value.is_object()) return nullptr;
  const ObjectRef ref = value.as_object();
  if (ref.type == script::kNoType) return nullptr;
  return world.find(ref.id);
}

/// Alive, by the rule `Obj::IsDead` uses: the object exists and has health.
[[nodiscard]] bool alive(const World& world, ObjectId id) noexcept {
  const WorldObject* slot = world.find(id);
  return slot != nullptr && slot->state.health > 0;
}

/// The ship's holder, which is the object at `ship + 1`.
[[nodiscard]] ObjectId holder_of_ship(const World& world, ObjectId ship) noexcept {
  const WorldObject* holder = world.find(ship + 1);
  if (holder == nullptr || holder->internal != InternalKind::holder) return kNoObject;
  return holder->id;
}

/// `max_units_to_board`, the class property `CVXShip::Init` seeds the holder's
/// capacity from. `sim/world_host.cpp`'s `UnitsMax` reads the same one, and
/// says why the class beats a stored field.
/// A class property as a non-negative integer, or 0. Shared by the capacity
/// and by `FindPointToStay`'s reach, which reads a radius off a class that no
/// object need be an instance of.
[[nodiscard]] std::int32_t property_int(const ClassGraph& graph, ClassIndex index,
                                        std::string_view key) noexcept {
  if (index == kNoClass) return 0;
  const std::string_view text = graph.property(index, key);
  std::int64_t value = 0;
  std::size_t i = 0;
  if (i < text.size() && text[i] == '+') ++i;
  if (i >= text.size()) return 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return 0;
    value = value * 10 + (text[i] - '0');
    if (value > 0x7FFFFFFFLL) return 0;
  }
  return static_cast<std::int32_t>(value);
}

[[nodiscard]] std::int32_t capacity_of(const World& world, const WorldObject& ship) noexcept {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return 0;
  return property_int(*graph, ship.class_index, "max_units_to_board");
}

/// The three verbs `NotifyShipBoardingCancel` kills, in the original's order.
///
/// 0x005c76c0 compares the running command's name against each in turn and
/// stops the command on the first that matches. The three overlap under a
/// prefix reading -- `boardshiphero` and `boardshipcommon` both begin with
/// `boardship` -- and the original tests all three anyway, which is why the
/// comparison here is for equality rather than for a prefix: only an equality
/// reading makes the second and third tests do anything.
constexpr std::string_view kBoardingVerbs[] = {"boardship", "boardshiphero", "boardshipcommon"};

[[nodiscard]] bool is_boarding_verb(std::string_view verb) noexcept {
  for (const std::string_view candidate : kBoardingVerbs) {
    if (verb == candidate) return true;
  }
  return false;
}

}  // namespace

// --------------------------------------------------------------------------
// the table
// --------------------------------------------------------------------------

std::size_t BoardingTable::lower_bound(ObjectId ship) const noexcept {
  const auto at = std::lower_bound(rows_.begin(), rows_.end(), ship,
                                   [](const Row& row, ObjectId key) { return row.ship < key; });
  return static_cast<std::size_t>(at - rows_.begin());
}

bool BoardingTable::notify(ObjectId ship, ObjectId unit) {
  if (ship == kNoObject || unit == kNoObject) return false;
  const std::size_t at = lower_bound(ship);
  if (at < rows_.size() && rows_[at].ship == ship) {
    std::vector<ObjectId>& members = rows_[at].members;
    if (std::find(members.begin(), members.end(), unit) != members.end()) return false;
    members.push_back(unit);
    return true;
  }
  rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(at), Row{ship, {unit}});
  return true;
}

bool BoardingTable::cancel(ObjectId ship, ObjectId unit) {
  const std::size_t at = lower_bound(ship);
  if (at >= rows_.size() || rows_[at].ship != ship) return false;
  std::vector<ObjectId>& members = rows_[at].members;
  const auto it = std::find(members.begin(), members.end(), unit);
  if (it == members.end()) return false;
  members.erase(it);
  // An empty row is no row: two worlds that differ only by a row nobody is in
  // must not hash differently.
  if (members.empty()) rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(at));
  return true;
}

void BoardingTable::clear(ObjectId ship) {
  const std::size_t at = lower_bound(ship);
  if (at < rows_.size() && rows_[at].ship == ship) {
    rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(at));
  }
}

void BoardingTable::forget(ObjectId id) { clear(id); }

std::span<const ObjectId> BoardingTable::expected(ObjectId ship) const noexcept {
  const std::size_t at = lower_bound(ship);
  if (at >= rows_.size() || rows_[at].ship != ship) return {};
  return rows_[at].members;
}

std::size_t BoardingTable::count(ObjectId ship) const noexcept { return expected(ship).size(); }

bool BoardingTable::listed(ObjectId ship, ObjectId unit) const noexcept {
  const std::span<const ObjectId> members = expected(ship);
  return std::find(members.begin(), members.end(), unit) != members.end();
}

void BoardingTable::assign(ObjectId ship, std::span<const ObjectId> members) {
  if (ship == kNoObject) return;
  if (members.empty()) {
    clear(ship);
    return;
  }
  const std::size_t at = lower_bound(ship);
  if (at < rows_.size() && rows_[at].ship == ship) {
    rows_[at].members.assign(members.begin(), members.end());
    return;
  }
  rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(at),
               Row{ship, std::vector<ObjectId>(members.begin(), members.end())});
}

ObjectId BoardingTable::ship_at(std::size_t index) const noexcept {
  return index < rows_.size() ? rows_[index].ship : kNoObject;
}

std::span<const ObjectId> BoardingTable::members_at(std::size_t index) const noexcept {
  if (index >= rows_.size()) return {};
  return rows_[index].members;
}

void BoardingTable::hash(std::uint64_t& accumulator) const noexcept {
  mix(accumulator, rows_.size());
  for (const Row& row : rows_) {
    mix(accumulator, row.ship);
    mix(accumulator, row.members.size());
    for (const ObjectId id : row.members) mix(accumulator, id);
  }
}

// --------------------------------------------------------------------------
// the two free helpers
// --------------------------------------------------------------------------

std::size_t prune_boarding_list(World& world, ObjectId ship) {
  const std::span<const ObjectId> members = world.boarding().expected(ship);
  if (members.empty()) return 0;
  std::vector<ObjectId> live;
  live.reserve(members.size());
  for (const ObjectId id : members) {
    if (alive(world, id)) live.push_back(id);
  }
  if (live.size() != members.size()) world.mutable_boarding().assign(ship, live);
  return live.size();
}

std::size_t units_on_board(const World& world, ObjectId ship) {
  const ObjectId holder = holder_of_ship(world, ship);
  if (holder == kNoObject) return 0;
  std::vector<ObjectId> inside;
  return world.contents_of(holder, inside);
}

// --------------------------------------------------------------------------
// the unboarding half
// --------------------------------------------------------------------------

std::vector<ObjectId> passengers_of(const World& world, ObjectId ship) {
  std::vector<ObjectId> aboard;
  const ObjectId holder = holder_of_ship(world, ship);
  if (holder != kNoObject) (void)world.contents_of(holder, aboard);
  return aboard;
}

std::size_t unboard_all(World& world, ObjectId ship) {
  const ObjectId holder = holder_of_ship(world, ship);
  if (holder == kNoObject) return 0;
  std::vector<Point> points;
  (void)unboarding_points(world, world.resolve_position(ship), points);
  // Copied: the drop rewrites the very back-links this was collected from.
  std::vector<ObjectId> aboard;
  (void)world.contents_of(holder, aboard);
  std::size_t dropped = 0;
  for (const ObjectId unit : aboard) {
    if (points.empty()) break;
    const Point at = take_random_point(world, points);
    if (world.remove_from_holder(unit, at)) ++dropped;
  }
  return dropped;
}

std::size_t unboarding_points(World& world, Point centre, std::vector<Point>& out) {
  out.clear();
  const MovementSystem* movement = movement_system(world);
  if (movement == nullptr) return 0;
  const ObstructionGrid& grid = movement->grid();
  if (grid.empty()) return 0;
  const Grid& terrain = world.terrain();
  // Row by row, x fastest: the order 0x005c8010 pushes in, and the order the
  // random index counts through.
  for (std::int32_t row = 0; row < kUnboardingSpan; ++row) {
    const std::int32_t y = centre.y - kUnboardingReach + row * kCollisionCellSize;
    for (std::int32_t column = 0; column < kUnboardingSpan; ++column) {
      const std::int32_t x = centre.x - kUnboardingReach + column * kCollisionCellSize;
      const std::int32_t cx = ObstructionGrid::cell_of(x);
      const std::int32_t cy = ObstructionGrid::cell_of(y);
      // "Inside the map" is the original's four comparisons against the world's
      // bounds, and the grid's cell bounds are the same square. A point off
      // the grid is skipped, not clamped -- the clamp 0x005c8010 does perform
      // is only ever applied to a point it then rejects -- and `blocked_cell`
      // answers blocked for an off-grid cell, so one test covers both. A
      // separate `in_bounds` guard here survived a fault sweep untouched.
      if (grid.blocked_cell(cx, cy)) continue;
      if (terrain_at(terrain, Point{x, y}) == kDeepWaterIndex) continue;
      out.push_back(Point{x, y});
    }
  }
  return out.size();
}

Point take_random_point(World& world, std::vector<Point>& points) {
  if (points.empty()) return kHeldPosition;
  const auto count = static_cast<std::int32_t>(points.size());
  const std::int32_t index = world.rng().between(0, count - 1);
  const Point chosen = points[static_cast<std::size_t>(index)];
  // Ordered erase, not swap-with-last: the survivors' order is what the next
  // draw indexes, and two peers must agree on it.
  points.erase(points.begin() + index);
  return chosen;
}

bool stand_point_free(World& world, const WorldObject& self, Point where) {
  // 0x0040a990 is the free-spot test the movement code asks too, and
  // `MovementSystem::spot_free` is where it is read: the query window is the
  // receiver's own radius plus `ShipBattle`'s for a water unit, 40 otherwise,
  // but a neighbour blocks only nearer than **the two radii summed**
  // (0x0040a880, handed the receiver's class radius) -- and a route lock
  // that is not the receiver's own blocks the same way.
  const MovementSystem* movement = movement_system(world);
  if (movement == nullptr) return true;
  return movement->spot_free(world, self.id, where, movement->radius_of(world, self.id));
}

// --------------------------------------------------------------------------
// the entry points
// --------------------------------------------------------------------------

namespace {

/// The receiver as a ship this engine can answer for.
///
/// Every diagnostic in this family -- `"The function 'NotifyBoardUnit' called
/// for an uninitialized or invalid object."` and its eight siblings -- is
/// printed and then the entry point returns a default. So an unresolvable
/// receiver is a *real case* with a defined answer rather than a trap, and each
/// entry point below says what its default is.
[[nodiscard]] const WorldObject* ship_receiver(const World& world, const Value& value) noexcept {
  return object_of(world, value);
}

/// 0x005c75c0, the one cancel both `NotifyBoardUnitCancel` and
/// `CancelArmyBoard` reach: **only a unit that is on the list and not already
/// aboard** is touched. It finds the unit in the ship's deque, then in the
/// ship's holder; on a hit in the first and a miss in the second it erases the
/// unit from the deque and writes the invalid handle into the unit's
/// `[unit+0x18c]` -- `ship_to_board` -- whatever ship that named. A unit that
/// is aboard keeps its place in the list. The `[ship+0x1d8]` latch that makes
/// it a no-op while `NotifyShipBoardingCancel` runs is the header's subject.
bool cancel_boarding(World& world, ObjectId ship, ObjectId unit) {
  if (!world.boarding().listed(ship, unit)) return false;
  WorldObject* slot = world.find(unit);
  const ObjectId holder = holder_of_ship(world, ship);
  if (slot != nullptr && holder != kNoObject && slot->state.holder == holder) return false;
  (void)world.mutable_boarding().cancel(ship, unit);
  if (slot != nullptr) slot->state.ship_to_board = kNoObject;
  return true;
}

/// "In a ship" is `Unit::InShip`'s question (0x005319a0, the holder's ship
/// slot), answered here the way `sim/world_host.cpp` answers it: the holder of
/// a ship is `ship + 1`.
[[nodiscard]] bool in_ship(const World& world, ObjectId id) {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->state.holder == kNoObject) return false;
  const WorldObject* before = world.find(slot->state.holder - 1);
  return before != nullptr && before->object != nullptr && before->object->is_a(NativeClass::ship);
}

/// 0x0052fd60, `CancelArmyBoard`'s body, which `NotifyShipBoardingCancel`
/// also runs for every hero aboard: one boolean for the hero -- is it in a
/// ship -- and the same for each army member, and every member whose answer
/// differs is detached (0x005db190; the walk starts again from the front after
/// each, so the order does not matter). Then, only for a hero that is **not**
/// in a ship and whose `ship_to_board` resolves, the cancel above.
void split_army_at_the_gangplank(World& world, ObjectId hero) {
  HeroSystem* heroes = hero_system_of(world);
  if (heroes == nullptr) return;
  const HeroRecord* record = heroes->hero(hero);
  if (record == nullptr) return;
  const bool hero_aboard = in_ship(world, hero);
  // Copied out: `detach` writes the very list this walks.
  const std::vector<ObjectId> army = record->army;
  for (const ObjectId member : army) {
    if (world.find(member) == nullptr) continue;
    if (in_ship(world, member) != hero_aboard) (void)heroes->detach(world, member);
  }
  if (hero_aboard) return;
  const WorldObject* slot = world.find(hero);
  if (slot == nullptr) return;
  const ObjectId ship = slot->state.ship_to_board;
  if (world.find(ship) != nullptr) (void)cancel_boarding(world, ship, hero);
}

/// `Ship::NotifyBoardUnit(u)` -- 3 sites, and the sole blocker of `HERO_BOARD.VS`
/// and `UNIT_BOARD.VS`.
///
/// Two refusals and three effects. The refusals: a unit already inside the
/// ship's holder, and a unit already on the list -- 0x005c7fa0 runs two
/// separate `find`s, one over the holder's contents and one over the ship's own
/// deque, and either hit exits before the stamp. The effects: the unit joins
/// the list, the ship is put on its `boardunit` command, and the unit's
/// `ship_to_board` is stamped.
///
/// The two `find`s run the other way round in the original -- deque first,
/// holder second -- and the order cannot be observed, because a unit that is
/// both listed and aboard leaves under either reading with nothing changed.
///
/// **The command is set only when the ship is not already running it**, which
/// is the point of the name comparison at 0x005c7f6a: a second unit notified
/// while `SHIP_BOARD.VS` is mid-loop must not restart that script from the top.
/// A ship with no command at all gets one, which is how the *first* notify
/// starts the loop.
HostOutcome m_notify_board_unit(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("NotifyBoardUnit: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  const WorldObject* unit = object_of(*world, ctx.arg(1));
  // Either half missing is a diagnostic and nothing else in the original.
  if (ship == nullptr || unit == nullptr) return HostOutcome::ok_void();
  if (unit->state.holder != kNoObject && unit->state.holder == holder_of_ship(*world, ship->id)) {
    return HostOutcome::ok_void();
  }
  // The "already listed" refusal is `notify`'s own -- it answers false for a
  // unit it already holds -- rather than a second test beside it. Asking
  // `listed` first and then appending would be two copies of one rule, and a
  // fault that broke the table's half would be invisible behind the other.
  if (!world->mutable_boarding().notify(ship->id, unit->id)) return HostOutcome::ok_void();

  if (CommandSystem* commands = command_system(*world)) {
    const bool idle = commands->command_count(ship->id) == 0;
    if (idle || commands->command_name(ship->id, 0) != "boardunit") {
      commands->set_command(*world, ship->id, "boardunit", Command{});
    }
  }

  if (WorldObject* slot = world->find(unit->id)) slot->state.ship_to_board = ship->id;
  return HostOutcome::ok_void();
}

/// `Ship::NotifyBoardUnitCancel(u)` -- 2 sites, both of them a boarding
/// command's `..._ONFINISH` undoing itself.
///
/// 0x005c7d00 prints a diagnostic and does nothing when the receiver or the
/// argument names nothing, and otherwise runs `cancel_boarding`: off the list
/// and `ship_to_board` cleared, unless the unit is already aboard.
HostOutcome m_notify_board_unit_cancel(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("NotifyBoardUnitCancel: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  const WorldObject* unit = object_of(*world, ctx.arg(1));
  if (ship == nullptr || unit == nullptr) return HostOutcome::ok_void();
  (void)cancel_boarding(*world, ship->id, unit->id);
  return HostOutcome::ok_void();
}

/// `Ship::NotifyShipBoardingCancel()` -- 1 site, and it is
/// `SHIP_BOARD_ONFINISH.VS` entire: the ship's own boarding command was
/// cancelled, so everybody walking towards it should stop.
///
/// Each listed unit whose *running* command is one of the three boarding verbs
/// has that command killed; a unit that has moved on to something else is left
/// alone. **Then every hero inside the ship's holder** (bit 0x1000000 of
/// `[obj+0x2c]`, 0x005c77bf) runs `CancelArmyBoard`'s body (0x005c7818 ->
/// 0x0052fd60): a hero that made it aboard drops the followers still ashore.
/// Last the list is emptied whatever happened (0x005c7837).
///
/// The kill pass skips a member whose handle no longer resolves or that runs
/// no command (`[unit+0x98]`); a dead unit's queue is empty in the original,
/// which is what the `alive` test below stands for.
///
/// The row is copied before anything is killed. In the original, killing one of
/// these commands launches its `..._ONFINISH` script, which calls straight back
/// into `NotifyBoardUnitCancel` on the very deque this loop is walking -- and
/// the `[ship+0x1d8]` latch is what stops that from being a mutation under an
/// iterator. `CommandSystem::kill_command` here launches no such script, so the
/// callback cannot occur at all; the copy is what makes that a property of this
/// code rather than of the command system's current shape.
HostOutcome m_notify_ship_boarding_cancel(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("NotifyShipBoardingCancel: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  if (ship == nullptr) return HostOutcome::ok_void();
  const ObjectId id = ship->id;

  const std::span<const ObjectId> listed = world->boarding().expected(id);
  const std::vector<ObjectId> members(listed.begin(), listed.end());
  if (CommandSystem* commands = command_system(*world)) {
    for (const ObjectId unit : members) {
      if (!alive(*world, unit)) continue;
      if (commands->command_count(unit) == 0) continue;
      if (!is_boarding_verb(commands->command_name(unit, 0))) continue;
      commands->kill_command(*world, unit);
    }
  }
  for (const ObjectId passenger : passengers_of(*world, id)) {
    if ((world->sync_flags(passenger) & kSyncHero) == 0) continue;
    split_army_at_the_gangplank(*world, passenger);
  }
  world->mutable_boarding().clear(id);
  return HostOutcome::ok_void();
}

/// `Ship::AreUnitsToBoard()` -- 1 site, and it is `SHIP_BOARD.VS`'s outer loop
/// condition, so it is asked once per unit the ship collects.
///
/// Three steps, in the original's order: empty the list outright when the ship
/// is exactly full, prune the dead, answer "is the list non-empty". The
/// equality test is deliberate -- see the header.
///
/// A receiver that does not resolve answers **false**, which is what the
/// diagnostic path at 0x005c794d pushes, and it is also what ends the loop.
HostOutcome m_are_units_to_board(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AreUnitsToBoard: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  if (ship == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const ObjectId id = ship->id;

  const std::int32_t capacity = capacity_of(*world, *ship);
  if (static_cast<std::int32_t>(units_on_board(*world, id)) == capacity) {
    world->mutable_boarding().clear(id);
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(Value::boolean(prune_boarding_list(*world, id) != 0));
}

/// `Ship::NumUnitsToBoard()` -- 2 sites, and **the one reader that does not
/// prune**. 0x005c6f60 is a bare load of the deque's size member; see the
/// header for why the difference is kept.
///
/// An unresolvable receiver answers 0, which is the value 0x005c6f8f pushes.
HostOutcome m_num_units_to_board(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("NumUnitsToBoard: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  if (ship == nullptr) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(world->boarding().count(ship->id))));
}

/// `Ship::BestCandidateToBoard()` -- 2 sites, both in `SHIP_BOARD.VS`: the unit
/// the ship should steer towards next.
///
/// Prune the dead, then the smallest `isqrt(dx² + dy²)` from the ship's own
/// position. The comparison is strict, so **the earlier member wins a tie**,
/// which is the order the original's deque walk gives and the reason this table
/// keeps notify order. The running best starts at 900,000,000 (0x35a4e900), far
/// past any map, so a list of one always yields it.
///
/// No candidate -- an empty list, or one whose members have all died -- is the
/// invalid handle, and `SHIP_BOARD.VS` reads it straight into `u1` and asks
/// `u1.IsAlive` two statements later.
HostOutcome m_best_candidate_to_board(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("BestCandidateToBoard: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  if (ship == nullptr) return HostOutcome::ok_with(invalid_object());
  const ObjectId id = ship->id;
  (void)prune_boarding_list(*world, id);

  const Point from = world->resolve_position(id);
  std::int64_t best = 900000000;
  ObjectId winner = kNoObject;
  for (const ObjectId candidate : world->boarding().expected(id)) {
    const WorldObject* slot = world->find(candidate);
    if (slot == nullptr) continue;
    const Point at = world->resolve_position(candidate);
    const std::int64_t dx = at.x - from.x;
    const std::int64_t dy = at.y - from.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    if (distance >= best) continue;
    best = distance;
    winner = candidate;
  }
  return HostOutcome::ok_with(object_value(winner));
}

/// `Ship::BoardUnit(u)` -- 1 site, and the sole blocker of
/// `UNIT_BOARD_COMMON.VS`: the walking unit asks, every two seconds, whether it
/// has arrived.
///
/// 0x005c7c00 resolves the argument, then the receiver, and answers false
/// with a diagnostic when either names nothing. It measures
/// `isqrt(dx² + dy²)` between the two `pos` and answers false at 150 or more.
/// Within that, 0x005c7840 refuses a unit already inside the ship's holder and
/// otherwise enters it through the holder entry 0x005d3e10 **without the force
/// flag**, so **a full ship refuses**: 0x00532160 answers 0 when the holder's
/// occupancy `[holder+0x14]` has reached its capacity `[holder+0x10]`
/// (`max_units_to_board`). This used to board regardless of capacity.
///
/// An admitted unit has its path and action dropped and stands at `(-1, -1)`
/// in the holder -- what `garrison_enter` does for a settlement's -- and then
/// comes off the list (0x0041ee70 on the deque, 0x005c78ef) and has its route
/// cleared (0x005d3830). It keeps its `ship_to_board`.
///
/// **Not reproduced:** the holder entry takes the unit out of any old holder
/// before it asks for room, so a held unit refused here would be left in
/// none. A held unit's `pos` is `(-1, -1)`, which is 150 from a ship only at
/// the map's corner. And the entry cancels the unit's feeder timer (`0x1f`),
/// which `FeederSystem` does not keep.
HostOutcome m_board_unit(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("BoardUnit: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  const WorldObject* unit = object_of(*world, ctx.arg(1));
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  if (ship == nullptr || unit == nullptr) return no;

  const ObjectId ship_id = ship->id;
  const ObjectId unit_id = unit->id;
  const ObjectId holder = holder_of_ship(*world, ship_id);
  if (holder == kNoObject) return no;

  const Point a = world->resolve_position(ship_id);
  const Point b = world->resolve_position(unit_id);
  const std::int64_t dx = b.x - a.x;
  const std::int64_t dy = b.y - a.y;
  if (isqrt(dx * dx + dy * dy) >= 150) return no;

  if (unit->state.holder == holder) return no;
  if (static_cast<std::int32_t>(units_on_board(*world, ship_id)) >= capacity_of(*world, *ship)) {
    return no;
  }
  if (MovementSystem* movement = movement_system(*world); movement != nullptr) {
    movement->stop(*world, unit_id);
  }
  if (!world->put_in_holder(unit_id, holder)) return no;
  if (CombatSystem* combat = combat_system_of(*world); combat != nullptr) {
    (void)combat->stop(unit_id);
    combat->set_position(unit_id, kHeldPosition);
  }
  world->mutable_boarding().cancel(ship_id, unit_id);
  return HostOutcome::ok_with(Value::boolean(true));
}

/// `Unit::GetShipToBoard()` -- 4 sites, and a plain read of
/// `ObjectState::ship_to_board`.
///
/// 0x005d80d0 pushes the stored handle without resolving it and without asking
/// anything about what it names, so a script gets back whatever
/// `NotifyBoardUnit` last stamped -- including a ship that has since sunk,
/// which is what the two `..._ONFINISH` scripts' `.IsValid` guard is for. This
/// resolves it only far enough to answer the invalid handle for an id the world
/// no longer holds, because `kNoObject` and "a dead ship" are the same value on
/// this side and `.IsValid` must tell them apart from a live one.
HostOutcome m_get_ship_to_board(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetShipToBoard: no world");
  const WorldObject* unit = object_of(*world, ctx.arg(0));
  if (unit == nullptr) return HostOutcome::ok_with(invalid_object());
  const ObjectId ship = unit->state.ship_to_board;
  if (world->find(ship) == nullptr) return HostOutcome::ok_with(invalid_object());
  return HostOutcome::ok_with(object_value(ship));
}

/// `Hero::CancelArmyBoard()` -- 1 site, and it is `HERO_BOARD_ONFINISH.VS`
/// entire.
///
/// A hero orders its whole army aboard (`HERO_BOARD.VS`'s
/// `.army.SetCommand("boardship", ship)`) and the boarding is then cancelled
/// part-way, with some warriors already inside the ship and some still on the
/// beach. 0x0052fd60 answers the question that leaves: **the ones on the wrong
/// side of the gangplank stop being the hero's**.
///
/// It computes one boolean for the hero -- is it inside a ship's holder -- and
/// the same boolean for each army member, and detaches every member whose
/// answer differs. So a hero that boarded keeps the followers who boarded with
/// it, and a hero still ashore keeps the ones still ashore.
///
/// Then, and only when the hero is **not** aboard, it cancels the hero's own
/// place in its ship's list, through the same `ship_to_board` the two unit
/// `..._ONFINISH` scripts use, which clears that field too. A hero that made
/// it aboard has nothing to cancel. `split_army_at_the_gangplank` is the
/// body; 0x0052fec0 only resolves the receiver, and does nothing past a
/// diagnostic when it names nothing.
HostOutcome m_cancel_army_board(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CancelArmyBoard: no world");
  const WorldObject* hero = object_of(*world, ctx.arg(0));
  if (hero == nullptr) return HostOutcome::ok_void();
  split_army_at_the_gangplank(*world, hero->id);
  return HostOutcome::ok_void();
}

/// `Ship::UnboardAllUnits()` -- 1 site, `SHIP_UNBOARD_ALL.VS` entire once the
/// ship has arrived.
///
/// The header says what it does; what it does *not* do is worth one line: it
/// does not touch the boarding list, the units' `ship_to_board`, or the ship's
/// command queue. A dropped unit is simply ashore.
HostOutcome m_unboard_all_units(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("UnboardAllUnits: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  if (ship == nullptr) return HostOutcome::ok_void();
  (void)unboard_all(*world, ship->id);
  return HostOutcome::ok_void();
}

/// `Ship::UnboardUnits(ol)` -- 1 site, `SHIP_UNBOARD.VS`.
///
/// The list's order is the drop order, and only members inside *this* ship's
/// holder move. 0x005c82f0 tests the point supply before each list member,
/// including one it then skips, and so does this.
HostOutcome m_unboard_units(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("UnboardUnits: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  if (ship == nullptr) return HostOutcome::ok_void();
  const ObjectId holder = holder_of_ship(*world, ship->id);
  if (holder == kNoObject) return HostOutcome::ok_void();
  if (ctx.count() < 2) return HostOutcome::failed("UnboardUnits: no list");
  const ObjListId list = objlist_of(ctx.arg(1));
  // Copied for the same reason `aboard` is above, and because the pool may
  // reallocate under a span.
  const std::span<const ObjectId> view = world->objlists().items(list);
  const std::vector<ObjectId> listed(view.begin(), view.end());

  std::vector<Point> points;
  (void)unboarding_points(*world, world->resolve_position(ship->id), points);
  for (const ObjectId id : listed) {
    if (points.empty()) break;
    const WorldObject* unit = world->find(id);
    if (unit == nullptr || unit->state.holder != holder) continue;
    const Point at = take_random_point(*world, points);
    (void)world->remove_from_holder(id, at);
  }
  return HostOutcome::ok_void();
}

/// `Ship::FindPointToStay()` -- 20 sites, all of them `SHIP_IDLE.VS`'s
/// `.Goto(.FindPointToStay, 0, 2000, 0, -1)`.
///
/// The receiver's area is taken once, before the first throw (0x0043cb70).
/// Each try draws `dy` then `dx` from the world generator (0x005c709c,
/// 0x005c70b9) and passes the point when its area is the ship's, its
/// passability cell is free, and 0x0040a990 finds the spot free for a body of
/// the ship's class radius (`stand_point_free`). The hundredth miss answers
/// the ship's `pos`.
///
/// **An unresolvable receiver is a fault in the original**, not an answer:
/// there is no validity test, and the area lookup dereferences the null
/// object. This answers `(0, 0)` without drawing; the one shipped site asks
/// it of the ship running its own idle.
HostOutcome m_find_point_to_stay(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindPointToStay: no world");
  const WorldObject* ship = ship_receiver(*world, ctx.arg(0));
  if (ship == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));

  const Point origin = world->resolve_position(ship->id);
  const LsaPartition& lsa = world->lsa();
  const LsaId home = lsa.at(origin);
  const MovementSystem* movement = movement_system(*world);
  for (std::int32_t attempt = 0; attempt < kStayAttempts; ++attempt) {
    // y first, then x: the order the two draws are made in.
    const std::int32_t dy = world->rng().between(-kStayThrow, kStayThrow);
    const std::int32_t dx = world->rng().between(-kStayThrow, kStayThrow);
    const Point candidate{origin.x + dx, origin.y + dy};
    if (lsa.at(candidate) != home) continue;
    if (movement == nullptr || movement->grid().blocked(candidate)) continue;
    if (!stand_point_free(*world, *ship, candidate)) continue;
    return HostOutcome::ok_with(pack_point(candidate));
  }
  return HostOutcome::ok_with(pack_point(origin));
}

constexpr std::size_t kEntryCount = 12;

}  // namespace

std::size_t boarding_host_entry_count() noexcept { return kEntryCount; }

std::size_t register_boarding_host(script::HostRegistry& registry) {
  std::size_t defined = 0;
  const auto def = [&](CallKind kind, std::string_view name, std::uint16_t arity,
                       script::HostFn fn) {
    registry.define(kind, name, arity, fn);
    ++defined;
  };

  // Descending corpus call frequency.
  def(CallKind::member, "GetShipToBoard", 0, &m_get_ship_to_board);                 // 4
  def(CallKind::member, "NotifyBoardUnit", 1, &m_notify_board_unit);                // 3
  def(CallKind::member, "NotifyBoardUnitCancel", 1, &m_notify_board_unit_cancel);   // 2
  def(CallKind::member, "NumUnitsToBoard", 0, &m_num_units_to_board);               // 2
  def(CallKind::member, "BestCandidateToBoard", 0, &m_best_candidate_to_board);     // 2
  def(CallKind::member, "AreUnitsToBoard", 0, &m_are_units_to_board);               // 1
  def(CallKind::member, "BoardUnit", 1, &m_board_unit);                             // 1
  def(CallKind::member, "NotifyShipBoardingCancel", 0, &m_notify_ship_boarding_cancel);  // 1
  def(CallKind::member, "FindPointToStay", 0, &m_find_point_to_stay);               // 20
  def(CallKind::member, "UnboardUnits", 1, &m_unboard_units);                       // 1
  def(CallKind::member, "UnboardAllUnits", 0, &m_unboard_all_units);                // 1
  def(CallKind::member, "CancelArmyBoard", 0, &m_cancel_army_board);                // 1
  return defined;
}

}  // namespace imperivm::core::sim
