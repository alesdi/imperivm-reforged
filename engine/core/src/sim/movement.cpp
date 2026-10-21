// Movement, formations and the movement slice of the host API.
// See include/imperivm/core/sim/movement.hpp.

#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/game/class_graph.hpp"

#include "imperivm/core/formats/ini.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/entrance.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core::sim {
namespace {

constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

/// A decision that is not coming this turn, or at all.
constexpr GameTime kNever = std::numeric_limits<GameTime>::max();

/// The bucket the avoidance queries sort bodies into. The original's spatial
/// buckets are 256 units (`0x00417090` shifts by 8); nothing depends on this
/// one matching, since a bucket here only narrows a search whose every
/// candidate is then measured exactly.
constexpr std::int32_t kBucket = 256;

/// How a route is walked this turn.
enum : std::uint8_t { kStill = 0, kCooperative = 1, kWholeTurn = 2 };

[[nodiscard]] bool property_flag(std::string_view value) noexcept {
  std::int32_t parsed = 0;
  return parse_int(value, parsed) && parsed != 0;
}

void hash_u64(std::uint64_t& state, std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    state ^= (value >> shift) & 0xFF;
    state *= kFnvPrime;
  }
}

void hash_i32(std::uint64_t& state, std::int32_t value) noexcept {
  hash_u64(state, static_cast<std::uint32_t>(value));
}

[[nodiscard]] bool equal_fold(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// -- which way a heading is drawn -------------------------------------------
//
// **Not `sim/heading.hpp`'s `angle_of_dir`**, which is the nearest thing that
// already existed. That one reproduces `GetAngleByDir` to the degree, with its
// short pi and its truncation, because the birds that call it steer by the
// result; a sprite's facing wants a finer angle than a whole degree.

/// A full turn, in the fixed-point angle headings are measured in.
constexpr std::int64_t kTurn = 1 << 16;

/// `atan(numerator / denominator)` in 1/65536ths of a turn, for
/// `0 <= numerator <= denominator` and a non-zero denominator, so the answer is
/// in `[0, kTurn/8]`.
///
/// The approximation is the standard rational one,
/// `atan(z) = (pi/4)z + z(1 - z)(0.2447 + 0.0663z)`, with its coefficients
/// carried as 1/65536ths: 0.2447 and 0.0663 of a radian are 2552 and 691 of
/// these units. Its error is below 0.0015 radians, and it is exact at both
/// ends -- z = 0 gives 0 and z = 1 gives an eighth of a turn -- which is what
/// keeps the eight cardinal headings landing exactly on their own columns.
///
/// Integer because the simulation has no floating point: a heading is hashed
/// state, and hashed state may not depend on a compiler's rounding.
[[nodiscard]] std::int64_t atan_ratio(std::int64_t numerator, std::int64_t denominator) noexcept {
  const std::int64_t z = (numerator << 16) / denominator;  // [0, 65536]
  const std::int64_t linear = (z * (kTurn / 8)) >> 16;
  const std::int64_t bend = (z * (kTurn - z)) >> 16;  // z(1 - z)
  return linear + ((bend * (2552 + ((691 * z) >> 16))) >> 16);
}

/// `facing` as a fraction of a turn in `[0, kTurn)`, measured from `(0, +1)`
/// and increasing towards `(-1, 0)` -- the sheets' own direction of travel.
[[nodiscard]] std::int64_t turn_of(Point facing) noexcept {
  // The heading in the sheets' frame: `south` is the axis column 0 draws and
  // `west` is the way the columns turn.
  const std::int64_t south = facing.y;
  const std::int64_t west = -static_cast<std::int64_t>(facing.x);
  if (south == 0 && west == 0) return 0;

  const std::int64_t s = south < 0 ? -south : south;
  const std::int64_t w = west < 0 ? -west : west;
  // Reduced to the first octant, then unfolded: the ratio is always <= 1, which
  // is the range `atan_ratio` is accurate over.
  std::int64_t turn = s >= w ? atan_ratio(w, s) : kTurn / 4 - atan_ratio(s, w);
  if (south < 0) turn = kTurn / 2 - turn;
  if (west < 0) turn = kTurn - turn;
  return turn == kTurn ? 0 : turn;
}

}  // namespace

// --------------------------------------------------------------------------
// headings
// --------------------------------------------------------------------------

std::uint32_t facing_column(Point facing, std::int32_t variations) noexcept {
  if (variations < 2) return 0;
  const std::int64_t count = variations;
  const std::int64_t turn = turn_of(facing);
  // Round to the nearest wedge rather than truncating: the wedge is centred on
  // its own heading, so a unit walking due west is drawn due west and not on
  // the column half a wedge before it.
  const std::int64_t column = (turn * count + kTurn / 2) / kTurn;
  return static_cast<std::uint32_t>(column % count);
}

// --------------------------------------------------------------------------
// formations
// --------------------------------------------------------------------------

Result<FormationTable> FormationTable::parse(std::span<const std::byte> xml) {
  Result<XmlDocument> parsed = XmlDocument::parse(xml);
  if (!parsed) return parsed.error();
  const XmlDocument& doc = *parsed;
  if (doc.empty()) return FormatError::malformed;

  const NodeIndex root = doc.root();
  if (doc.node(root).name != "Formations") return FormatError::bad_magic;

  FormationTable table;
  const NodeIndex def = doc.child(root, "Default");
  if (def != kNoNode) {
    const std::string_view name = doc.attribute(def, "Name");
    if (!name.empty()) table.default_name_.assign(name);
  }

  for (NodeIndex n = doc.child(root, "FormationClass"); n != kNoNode;
       n = doc.next(n, "FormationClass")) {
    FormationClassDef formation;
    formation.name.assign(doc.attribute(n, "Name"));
    formation.description.assign(doc.attribute(n, "Description"));
    // `Block` declares neither Width nor Height, which is what makes 1:1 the
    // right default rather than an invention.
    formation.width = doc.attribute_int(n, "Width", 1);
    formation.height = doc.attribute_int(n, "Height", 1);
    if (formation.width <= 0) formation.width = 1;
    if (formation.height <= 0) formation.height = 1;
    formation.bonus_attack = doc.attribute_int(n, "BonusAttack", 0);
    // The bonuses proper are read in file order, because `BonusArmor` writes
    // both defence fields and which of it and `BonusDefenceSlash` wins is a
    // question of which comes later on the element. See `FormationClassDef`.
    const XmlNode& node = doc.node(n);
    for (std::uint32_t a = 0; a < node.attribute_count; ++a) {
      const XmlAttribute& attribute = doc.attributes()[node.attribute_begin + a];
      const std::int32_t value = doc.attribute_int(n, attribute.name, 0);
      if (attribute.name == "BonusLevel") {
        formation.bonus_level = value;
      } else if (attribute.name == "BonusDamage") {
        formation.bonus_damage = value;
      } else if (attribute.name == "BonusDefenceSlash") {
        formation.bonus_defence_slash = value;
      } else if (attribute.name == "BonusDefencePierce") {
        formation.bonus_defence_pierce = value;
      } else if (attribute.name == "BonusArmor") {
        formation.bonus_defence_slash = value;
        formation.bonus_defence_pierce = value;
      } else if (attribute.name == "BonusRange") {
        formation.bonus_range = value;
      }
    }
    formation.offset_front_line_y = doc.attribute_int(n, "OffsetFrontLineByY", 0);
    formation.offset_wings_x = doc.attribute_int(n, "OffsetWingsByX", 0);
    formation.offset_wings_y = doc.attribute_int(n, "OffsetWingsByY", 0);

    for (NodeIndex c = doc.child(n, "Class"); c != kNoNode; c = doc.next(c, "Class")) {
      FormationRole role;
      role.class_name.assign(doc.attribute(c, "Name"));
      role.central = doc.attribute_int(c, "CentralBlock", 0);
      role.front = doc.attribute_int(c, "FrontLine", 0);
      role.wings = doc.attribute_int(c, "Wings", 0);
      formation.roles.push_back(std::move(role));
    }
    table.formations_.push_back(std::move(formation));
  }
  return table;
}

const FormationClassDef* FormationTable::find(std::string_view name) const noexcept {
  for (const FormationClassDef& formation : formations_) {
    if (equal_fold(formation.name, name)) return &formation;
  }
  return nullptr;
}

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

void MovementSystem::start(World& world) {
  // Bring every tracked object's `SyncFlags` bit 17 into line with the state
  // here before the first turn, so a world loaded mid-path hashes correctly
  // from the outset rather than from its first movement turn.
  for (const Entry& entry : states_) mark_path(world, entry.id, entry.state.has_path);
  // The map-object loader's last act, once a match. A save restores the
  // list, and the flag with it, so a load does not make them twice.
  if (!static_locks_built_) build_static_locks(world);
}

void MovementSystem::build_static_locks(const World& world) {
  static_locks_.clear();
  std::vector<Point> points;
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none || slot.object == nullptr) continue;
    if (slot.state.is_held() || slot.state.flags.unspawned) continue;
    for (const std::int32_t type : {kLockPointSmall, kLockPointMedium, kLockPointRing}) {
      if (!class_has_point_type(world, slot.id, type)) continue;
      points.clear();
      class_points_of_type(world, slot.id, type, points);
      for (const Point& at : points) locks_of_point(at, type, static_locks_);
    }
  }
  static_locks_built_ = true;
  lock_buckets_valid_ = false;
}

void MovementSystem::set_static_locks(std::vector<StaticLock> locks) {
  static_locks_ = std::move(locks);
  static_locks_built_ = true;
  lock_buckets_valid_ = false;
}

void MovementSystem::set_grid(ObstructionGrid grid) {
  grid_ = std::move(grid);
  ++grid_generation_;
}

ObstructionGrid& MovementSystem::mutable_grid() noexcept {
  ++grid_generation_;
  return grid_;
}

void MovementSystem::start_smart_build() { finder_.start_smart_build(grid_); }

bool MovementSystem::smart_build(SpfCall call) { return finder_.smart_build(call, grid_); }

std::size_t MovementSystem::lower_bound(ObjectId id) const noexcept {
  std::size_t lo = 0;
  std::size_t hi = states_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (states_[mid].id < id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

MoveState& MovementSystem::state(ObjectId id) {
  const std::size_t at = lower_bound(id);
  if (at < states_.size() && states_[at].id == id) return states_[at].state;
  Entry entry;
  entry.id = id;
  return states_.insert(states_.begin() + static_cast<std::ptrdiff_t>(at), std::move(entry))
      ->state;
}

const MoveState* MovementSystem::find(ObjectId id) const noexcept {
  const std::size_t at = lower_bound(id);
  if (at < states_.size() && states_[at].id == id) return &states_[at].state;
  return nullptr;
}

MoveState* MovementSystem::find(ObjectId id) noexcept {
  return const_cast<MoveState*>(static_cast<const MovementSystem*>(this)->find(id));
}

void MovementSystem::forget(ObjectId id) {
  const std::size_t at = lower_bound(id);
  if (at < states_.size() && states_[at].id == id) {
    states_.erase(states_.begin() + static_cast<std::ptrdiff_t>(at));
  }
  drop_formation(id);
}

void MovementSystem::drop_formation(ObjectId hero) {
  const auto form = std::lower_bound(
      formation_objects_.begin(), formation_objects_.end(), hero,
      [](const Formation& f, ObjectId id) { return f.hero < id; });
  if (form != formation_objects_.end() && form->hero == hero) formation_objects_.erase(form);
}

const Formation* MovementSystem::formation(ObjectId hero) const noexcept {
  const auto it = std::lower_bound(
      formation_objects_.begin(), formation_objects_.end(), hero,
      [](const Formation& f, ObjectId id) { return f.hero < id; });
  return it != formation_objects_.end() && it->hero == hero ? &*it : nullptr;
}

Formation* MovementSystem::formation(ObjectId hero) noexcept {
  return const_cast<Formation*>(std::as_const(*this).formation(hero));
}

Formation& MovementSystem::formation_of(ObjectId hero) {
  const auto it = std::lower_bound(
      formation_objects_.begin(), formation_objects_.end(), hero,
      [](const Formation& f, ObjectId id) { return f.hero < id; });
  if (it != formation_objects_.end() && it->hero == hero) return *it;
  Formation made;
  made.hero = hero;
  return *formation_objects_.insert(it, std::move(made));
}

void MovementSystem::join_formation(ObjectId hero, ObjectId unit) {
  formation_add(formation_of(hero), unit);
}

void MovementSystem::leave_formation(World& world, ObjectId hero, ObjectId unit) {
  Formation* form = formation(hero);
  if (form == nullptr || !formation_remove(*form, unit)) return;
  if (find(unit) != nullptr) stop(world, unit);
}

Point MovementSystem::position(World& world, ObjectId id) const noexcept {
  // `resolve_position` follows the holder chain: a unit inside a ship is where
  // the ship is, and has no coordinate of its own to read.
  return world.resolve_position(id);
}

void MovementSystem::set_position(World& world, ObjectId id, Point where) const noexcept {
  world.set_position(id, where);
}

void MovementSystem::mark_path(World& world, ObjectId id, bool active) const noexcept {
  // `SyncFlags` bit 17 lives on the object, so the world hash covers it and
  // this system does not fold it in a second time.
  ObjectState* state = world.mutable_state(id);
  if (state != nullptr) state->flags.has_active_path = active;
}

/// Forget the step being walked and the wait: what a new route, a `Stop` and
/// an arrival all start from. `CVXPathRetry::SetMedia` deletes the old media
/// before it installs the new, and a fresh `CVXPathCoop` is born with
/// `retrytime` 0.
namespace {
void reset_cooperation(MoveState& move, GameTime now) noexcept {
  move.holding = true;
  move.hold_until = now;
  move.step_start = 0;
  move.step_end = 0;
  move.offset_from = Point{};
  move.offset_to = Point{};
  move.retry_time = 0;
}
}  // namespace

MoveOutcome MovementSystem::lay_path(World& world, ObjectId id, MoveState& move, Point dest,
                                     std::int32_t range, GameTime now, const Point* from_override) {
  const Point from = from_override != nullptr ? *from_override : position(world, id);

  // `RecastPathfind` (0x00419730) starts by clearing the follower's two stop
  // bits, `Unit::Stop`'s and its own failure's (`MoveState::route_failed`).
  move.stop_requested = false;
  move.route_failed = false;
  move.target = dest;
  move.range = range;
  // The route being walked, if any is left of it: a search that lays nothing
  // keeps it (below), so it is not touched until the search has answered.
  const bool walking_on = move.has_path && move.waypoints.size() >= 2 &&
                          move.travelled() < move.path_length;
  const auto fresh_media = [&]() {
    move.progress = 0;
    move.path_generation = grid_generation_;
    // The first step of the new route is decided the moment it is laid.
    reset_cooperation(move, now);
  };

  const auto arrived_here = [&]() {
    fresh_media();
    move.has_path = false;
    mark_path(world, id, false);
    move.waypoints.clear();
    move.gate_crossings.clear();
    move.path_length = 0;
    move.path_complete = true;
    move.holding = false;
    move.last_outcome = MoveOutcome::arrived;
    return MoveOutcome::arrived;
  };

  // `RecastPathfind` (`0x00419730`) asks `IsArrived` before it searches: inside
  // the band, and -- for an order that owns a lock -- on a free spot.
  if (within(from, dest, range) && arrival_spot_free(world, id, move, from)) return arrived_here();

  // The search's goal is the band's rings (`ring_goal`), not the band's
  // centre, and the route runs to the one nearest the unit -- for an order
  // that owns a lock, the nearest nobody stands on or has reserved, so a unit
  // whose own side of the target is taken goes round to another.
  const bool owner = lock_owner(id, move);
  RingGoal goal = ring_goal(world, owner ? id : kNoObject, dest, range, move.min_range);
  const OwnerSpot owner_test(*this, world, id);
  PathRequest request;
  request.start = from;
  request.goal = goal.at;
  request.goal_points = goal.points;
  request.goal_points_given = true;
  request.ignore_passability = ignores_passability(world, id);
  request.owner = owner ? &owner_test : nullptr;
  // One search, as 0x00419110 runs it: the gates the route crosses, and the
  // search again round those that bar this mover (a mover that ignores
  // passability walks its straight line regardless; see `sim/gate.hpp`). A
  // route that owns a lock and does not arrive is cut back to a free spot
  // inside the search (0x0041ba00).
  bool cut = false;
  const auto search = [&]() {
    Path found = finder_.find(grid_, request);
    if (!request.ignore_passability) {
      world.gate_lines().refresh(world);
      found = route_past_gates(world, id, from, request, std::move(found));
    }
    cut = found.cut;
    return found;
  };
  Path path = search();
  if (path.status == PathStatus::arrived) {
    // Standing on the ring point already. Arrived when the band says so; a
    // unit on a taken spot stays put, not arrived (a one-point route).
    if (within(from, dest, range) && arrival_spot_free(world, id, move, from)) return arrived_here();
    path.status = PathStatus::partial;
  }

  // Nowhere to go and no goal point left: the band is full. Once per
  // destination (`[retry+0x20] & 0x48`), the route search asks the free-spot
  // search for a spot out from the centre (`0x004191bf`) and re-aims at it --
  // exactly, with no band (`0x00419244`) -- when it is more than 200 units
  // nearer the centre than the unit stands (`0x00419230`), measured with the
  // integer square root. The order's band is unchanged: the unit there is
  // not arrived, and its next search is round the rings again.
  bool aimed = false;
  if ((!path.usable() || path.waypoints.size() < 2) && owner && !goal.free &&
      !move.free_spot_tried && !move.free_spot_aimed) {
    move.free_spot_tried = true;
    ++counters_.free_spot_searches;
    Point spot;
    if (free_spot(world, id, dest, spot) &&
        isqrt(dist_sq(from, dest)) - isqrt(dist_sq(spot, dest)) > kFreeSpotGain) {
      // The spot laid as a band of 0: itself, where a unit can stand, and
      // struck off like any goal point.
      goal = ring_goal(world, id, spot, 0, 0);
      request.goal = goal.at;
      request.goal_points = goal.points;
      path = search();
      if (path.status == PathStatus::arrived) path.status = PathStatus::partial;
      move.free_spot_aimed = true;
      aimed = true;
      ++counters_.free_spot_aims;
    }
  }

  if (!path.usable() || path.waypoints.size() < 2) {
    // The search laid nothing (0x00419110 answers 0). `RecastPathfind`
    // deletes the path it built and keeps the media the retry already had
    // (0x00419a0f); a retry not aimed at a formation -- every order but a
    // march's -- sets bit 2 (0x00419a17 .. 0x00419a1e) and answers with the
    // path follower's step, which with that bit set is the stop branch
    // (0x00419a48 .. 0x00419a5d, 0x00419ee9). So a unit that was walking
    // walks on along the route it had until it stands on a passable free
    // spot, as `Unit::Stop` makes it; and one whose order owns no lock, or
    // that already stands free, stops where it is.
    //
    // This is what keeps a column apart. Reinforcements that leave a
    // building by one door walk one route on top of one another; when a fight
    // in sight sends each at a target whose ring is taken and the free-spot
    // search refuses them (a squad in a big fight, `kFreeSpotCrowd`), every
    // route is cut back to where it stands. Stopped there, the whole column
    // stood on one point (Alesia: 197 touching pairs, 118 stacked); walking
    // on, the first stands free where it is and every other walks on past it
    // to the next free spot along the way.
    const bool formation = move.form_aimed;
    if (!formation) move.route_failed = true;
    if (!formation && walking_on &&
        !stop_branch_halts(world, id, move, from, move.travelled())) {
      move.last_outcome = MoveOutcome::moving;
      return move.last_outcome;
    }
    // A route aimed at a formation keeps what it had, and answers the failure
    // to the formation's step (0x00419b20), which takes no step on it.
    if (formation && walking_on) return MoveOutcome::blocked;
    fresh_media();
    move.has_path = false;
    mark_path(world, id, false);
    move.waypoints.clear();
    move.gate_crossings.clear();
    move.path_length = 0;
    move.path_complete = false;
    move.holding = false;
    // Cut back to where it stands: the way is open and the end is taken. The
    // unit is not arrived, and not walled off either.
    move.last_outcome = cut ? MoveOutcome::exhausted : MoveOutcome::blocked;
    return move.last_outcome;
  }

  fresh_media();
  move.waypoints = path.waypoints;
  move.path_length = path.length;
  // 0x00418fb0 over the route as it will be walked.
  move.gate_crossings.clear();
  if (!request.ignore_passability) world.gate_lines().crossings(move.waypoints, move.gate_crossings);
  // A route cut back from a taken end no longer reaches its goal, one
  // re-aimed at a free spot was never laid to the band, and one laid with no
  // goal point left -- status 0 from the straight line's walk to a free spot
  // (0x0040b580) -- ends on no goal point either, unless it ends inside the
  // band anyway (a mover that ignores passability, sent onto a wall).
  move.path_complete = path.status == PathStatus::found && !cut && !aimed &&
                       (goal.free || within(path.waypoints.back(), dest, range));
  move.has_path = true;
  mark_path(world, id, true);
  move.stride = formation_stride_or_walk(world, id, move);
  // Face along the first leg immediately, so a unit does not moonwalk out of
  // its first turn.
  const Point step{move.waypoints[1].x - from.x, move.waypoints[1].y - from.y};
  set_facing(world, id, move, step);
  move.last_outcome = move.path_complete ? MoveOutcome::moving : MoveOutcome::exhausted;
  return move.last_outcome;
}

Path MovementSystem::route_past_gates(World& world, ObjectId id, Point from,
                                      const PathRequest& request, Path first) {
  if (!first.usable() || first.waypoints.size() < 2) return first;
  const WorldObject* self = world.find(id);
  const GateLines& lines = world.gate_lines();
  if (self == nullptr || lines.lines().empty()) return first;
  // Which gates the route crosses (0x00418fb0), and how many of them count
  // the mover an enemy (0x004192f0..0x0041934b).
  lines.crossings(first.waypoints, crossings_);
  std::size_t enemies = 0;
  for (const GateCrossing& crossing : crossings_) {
    const WorldObject* gate = world.find(crossing.gate);
    if (gate != nullptr && gate_bars(world, *gate, self->state.owner)) ++enemies;
  }
  if (enemies == 0) return first;
  if (finder_.smart_installed()) {
    // The smart pathfinder is handed each gate that bars the mover (its
    // method 0x00414ae0, 0x0041945f..0x00419468): the connections whose
    // routes meet the gate's axis are closed for one search -- the grid is
    // not touched -- and opened again after it (0x00415950,
    // 0x00419537..0x00419548).
    gates_.clear();
    lines.enemy_gates(world, self->state.owner, from, world.time(), gates_);
    std::size_t closed = 0;
    for (const GateLines::Line* line : gates_) closed += finder_.close_gate(line->gate, line->a, line->b);
    // Nothing closed is the same search again, and the same route.
    if (closed == 0) {
      finder_.open_gates();
      return first;
    }
    ++counters_.gate_searches;
    Path second = finder_.find(grid_, request);
    finder_.open_gates();
    if (first.length <= kGateShortRoute && enemies == 1 && second.status != PathStatus::found) {
      return first;
    }
    return second;
  }
  // Every gate on the map that bars the mover, laid for one search.
  barrier_.clear();
  lines.lay_enemy_gates(world, self->state.owner, from, world.time(), barrier_);
  barrier_.seal();
  // Nothing laid -- every one of them standing fully open -- is the same
  // search again, and the same route.
  if (barrier_.empty()) return first;
  ++counters_.gate_searches;
  PathRequest again = request;
  again.barrier = &barrier_;
  Path second = finder_.find(grid_, again);
  // A short route through one enemy gate stands when going round does not
  // arrive (0x00419490, 0x0041954e): the unit walks up to the gate and waits.
  if (first.length <= kGateShortRoute && enemies == 1 && second.status != PathStatus::found) {
    return first;
  }
  return second;
}

bool MovementSystem::held_at_gate(const World& world, ObjectId id, const MoveState& move,
                                  std::int64_t travelled) const {
  // The first crossing not yet passed (0x00418210): the list is in route
  // order, so it is the first at or beyond where the unit has got to.
  for (const GateCrossing& crossing : move.gate_crossings) {
    if (crossing.at < travelled) continue;
    // A unit's own radius, or below zero none (0x004182b0). The route that
    // moves a formation is the formation's own, which takes the formation's
    // radius and never less than 275 (`formation_gate_bars`); every unit's,
    // the hero's included, is aimed at its station and takes its own.
    const std::int32_t radius = radius_of(world, id);
    const std::int64_t reach = static_cast<std::int64_t>(radius > 0 ? radius : 0) + kGateApproach;
    if (crossing.at - travelled > reach) return false;
    const WorldObject* gate = world.find(crossing.gate);
    const WorldObject* self = world.find(id);
    // A gate that has left the world bars nothing.
    if (gate == nullptr || self == nullptr) return false;
    return !gate_waves_through(world, *gate, self->state.owner, world.time());
  }
  return false;
}

/// Copy the class properties a `MoveState` carries, once.
///
/// `speed` is the one that matters: without it a unit lays a perfect path and
/// then walks none of it. A class that declares none leaves the default,
/// which is what an object with no class does too. (The formation's spacing,
/// `formation_radius`, used to ride along here; the layout reads it from the
/// class graph, `formation_class_traits`.)
void MovementSystem::resolve_class_fields(const World& world, ObjectId id, MoveState& move) {
  if (move.class_resolved) return;
  move.class_resolved = true;
  if (classes_ == nullptr) return;
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->class_index == kNoClass) return;
  std::int32_t value = 0;
  if (parse_int(classes_->property(slot->class_index, "speed"), value)) move.speed = value;
}

MoveOutcome MovementSystem::order_goto(World& world, ObjectId id, Point dest, std::int32_t range,
                                       std::int32_t min_range, ObjectId party,
                                       bool lock_destination) {
  MoveState& move = state(id);
  resolve_class_fields(world, id, move);
  // `SetDest` (`0x0041a4c0`) to anywhere else -- another point, band or
  // march -- forgets that the free-spot search has run (`0x0041a594`); so
  // does one to the same goal while a stop bit is set, which takes the same
  // branch (`test al, 6` at 0x0041a4da). Flag 0x40 survives both.
  if (!move.goto_active || move.stopping() || move.target_object != kNoObject ||
      move.target != dest || move.range != range || move.min_range != min_range ||
      move.party != party) {
    move.free_spot_tried = false;
  }
  move.target_object = kNoObject;
  move.min_range = min_range;
  move.goto_active = true;
  move.party = party;
  // An order that is not aimed at a formation walks at the unit's own pace.
  move.form_aimed = false;
  move.form_seen = -1;
  move.march_rate = 0;
  move.dest_lock = lock_destination && !ignores_passability(world, id);
  // `SetDest` and `SetFormation` both clear the stop request (0x00417797).
  move.stop_requested = false;
  return lay_path(world, id, move, dest, range, world.time());
}

MoveOutcome MovementSystem::order_goto_object(World& world, ObjectId id, ObjectId target,
                                              std::int32_t range, std::int32_t min_range,
                                              bool lock_destination) {
  MoveState& move = state(id);
  resolve_class_fields(world, id, move);
  // The object form (`0x0041a5c0`) asks after the object, not where it is,
  // and takes the new-goal branch on a stop bit too (0x0041a5da).
  if (!move.goto_active || move.stopping() || move.target_object != target ||
      move.range != range || move.min_range != min_range || move.party != kNoObject) {
    move.free_spot_tried = false;
  }
  move.target_object = target;
  move.min_range = min_range;
  move.goto_active = true;
  move.party = kNoObject;
  move.form_aimed = false;
  move.form_seen = -1;
  move.march_rate = 0;
  move.dest_lock = lock_destination && !ignores_passability(world, id);
  move.stop_requested = false;
  return lay_path(world, id, move, position(world, target), range, world.time());
}

void MovementSystem::stop(World& world, ObjectId id) {
  mark_path(world, id, false);
  MoveState* move = find(id);
  if (move == nullptr) return;
  move->has_path = false;
  move->goto_active = false;
  move->target_object = kNoObject;
  move->waypoints.clear();
  move->gate_crossings.clear();
  move->path_length = 0;
  move->progress = 0;
  // Every route after a stop is laid by `lay_path`, which resets this again,
  // so leaving it is unobservable; cleared so a saved stopped unit says so.
  reset_cooperation(*move, world.time());
  move->holding = false;
  // The retry goes with the order, and its flags with it.
  move->free_spot_tried = false;
  move->free_spot_aimed = false;
  move->stop_requested = false;
  move->route_failed = false;
  move->march_rate = 0;
  move->form_aimed = false;
  move->form_seen = -1;
  move->last_outcome = MoveOutcome::idle;
}

bool MovementSystem::stop_branch_halts(const World& world, ObjectId id, const MoveState& move,
                                       Point here, std::int64_t travelled) const {
  // 0x00419ee0 with `[retry+0x20] & 6`. It never asks `IsArrived`. An order
  // that names no owner (0x004178d0 at 0x00419ef1) takes no step at all; one
  // that does stops on a spot the passability grid leaves open (the bit tested
  // through 0x00623300) and the free-spot test passes (0x0040a990), and
  // otherwise takes the route's next step -- none at its end, so a unit that
  // runs out of route stops there, free spot or not. Nor does it step on
  // before a gate that bars it (0x00419f8a .. 0x00419fa2): where the walk on
  // would wait at the gate, the stop stops there.
  return !lock_owner(id, move) || travelled >= move.path_length ||
         (!grid_.blocked(here) && spot_free(world, id, here, radius_of(world, id))) ||
         held_at_gate(world, id, move, travelled);
}

void MovementSystem::halt(World& world, ObjectId id, MoveState& move) {
  // `Unit::Stop`'s: its re-entry (0x005d6df0) finds no next step and deletes
  // the route, the order with it.
  if (move.stop_requested) {
    stop(world, id);
    return;
  }
  // A failed search's: the follower answers no step and the retry stays,
  // bit 2 and all, until the next `SetDest` searches again. The walk ends;
  // the order does not, and is not arrived (`IsArrived` is false while the
  // bit is set).
  mark_path(world, id, false);
  move.has_path = false;
  move.waypoints.clear();
  move.gate_crossings.clear();
  move.path_length = 0;
  move.progress = 0;
  move.path_complete = false;
  reset_cooperation(move, world.time());
  move.holding = false;
  move.last_outcome = MoveOutcome::exhausted;
}

bool MovementSystem::marching(ObjectId id, const MoveState& move) const noexcept {
  (void)id;
  return move.form_aimed && move.party != kNoObject && formation_going_on(move.party);
}

bool MovementSystem::lock_owner(ObjectId id, const MoveState& move) const noexcept {
  // `0x004178d0`. A retry aimed at a formation (`[retry+0xc]`) names its unit
  // only when the formation's flag (`[form+0x8c]`) is set and the formation
  // reports its path spent (0x005f22f0: a path, and `[form+0x88]` clear,
  // which `CreateNextSample` writes once the path has nothing left,
  // 0x005f5cff); while the march is on it names nobody. Any other retry names
  // its unit when flag bit 0 is set and the unit's class does not ignore
  // passability -- which `dest_lock` already folds in.
  if (move.form_aimed) {
    const Formation* form = formation(move.party);
    return form != nullptr && form->should_lock && !marching(id, move);
  }
  return move.dest_lock;
}

bool MovementSystem::request_stop(World& world, ObjectId id) {
  MoveState* move = find(id);
  if (move == nullptr || !move->has_path || !lock_owner(id, *move)) {
    // With no owner named, the path follower's stop branch takes no step at
    // all (0x00419ef8 -> 0x0041a019): the unit stands where it is.
    stop(world, id);
    return false;
  }
  move->stop_requested = true;
  return true;
}

// --------------------------------------------------------------------------
// avoidance: the class properties a step reads
// --------------------------------------------------------------------------

MovementSystem::ClassTraits MovementSystem::traits_of(const World& world, ObjectId id) const {
  const WorldObject* slot = world.find(id);
  const ClassGraph* graph = world.class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) return ClassTraits{};
  if (graph != traits_graph_) {
    // A cache of the class graph, keyed on which graph it is. Not state: the
    // same graph always answers the same way.
    traits_.clear();
    traits_known_.clear();
    traits_graph_ = graph;
  }
  const std::size_t index = static_cast<std::size_t>(slot->class_index);
  if (index >= traits_.size()) {
    traits_.resize(index + 1);
    traits_known_.resize(index + 1, 0);
  }
  if (traits_known_[index] == 0) {
    ClassTraits traits;
    std::int32_t value = 0;
    if (parse_int(graph->property(slot->class_index, "radius"), value)) traits.radius = value;
    traits.water_unit = property_flag(graph->property(slot->class_index, "water_unit"));
    traits.ignore_passability =
        property_flag(graph->property(slot->class_index, "ignore_passability"));
    traits_[index] = traits;
    traits_known_[index] = 1;
  }
  return traits_[index];
}

std::int32_t MovementSystem::radius_of(const World& world, ObjectId id) const {
  return traits_of(world, id).radius;
}

bool MovementSystem::ignores_passability(const World& world, ObjectId id) const {
  return traits_of(world, id).ignore_passability;
}

// --------------------------------------------------------------------------
// destination locks and the free-spot test (sim/avoidance.hpp)
// --------------------------------------------------------------------------

namespace {

/// `0x0040a1e0` fills sixteen directions, `(cos, sin)` of `i * 0.3925` -- the
/// double at `0x007abdf0`, which is `3.14 / 8`, so the sixteenth stops short
/// of a full turn -- and `0x00409cf0` walks them. Here each is the retail
/// double scaled by 2^30 and rounded. For every radius below 5,104 the ring
/// point this gives truncates exactly as `_ftol` truncates the double (checked
/// over all sixteen directions); a band's radius is at most 1,000.
constexpr std::int64_t kRingDirections[16][2] = {
    {1073741824, 0},           {992089878, 410705708},    {759552370, 758947759},
    {411495605, 991762508},    {855049, 1073741484},      {-409915550, 992416619},
    {-758342667, 760156500},   {-991434508, 412285242},   {-1073740462, 1710098},
    {-992742731, -409125132},  {-760760147, -757737093},  {-413074617, -991105880},
    {-2565146, -1073738760},   {408334455, -993068214},   {757131039, -761363313},
    {990776623, -413863730}};
constexpr int kRingShift = 30;
/// Rings step 40 inwards (`0x0041787d`), and there are at most six (`0x00417858`).
constexpr std::int32_t kRingStep = 40;
constexpr int kRingCount = 6;
/// `SetDest` keeps a band's range under 1,000 (`0x0041a4c9`, `0x0041a572`).
constexpr std::int32_t kRingMaxRadius = 1000;

/// `_ftol`: towards zero. Only a negative coordinate tells it from a floor,
/// and a ring point off the map is never a goal (`0x00409b80`), so the two
/// are equivalent here -- a fault sweep cannot see the difference, and this
/// keeps the original's rounding anyway.
[[nodiscard]] std::int32_t truncate_fixed(std::int64_t v) noexcept {
  return static_cast<std::int32_t>(v >= 0 ? v >> kRingShift : -((-v) >> kRingShift));
}

}  // namespace

MovementSystem::RingGoal MovementSystem::ring_goal(const World& world, ObjectId owner,
                                                   Point centre, std::int32_t range,
                                                   std::int32_t min_range) const {
  // `0x00417830` gives the search a ring of sixteen points at `r - 1` from the
  // centre for `r = range, range - 40, ...` down to `min_range`, six at most,
  // each point a goal only where the grid lets a unit stand (`0x00409b80`).
  // A ring at `r = 0` is the centre once (`0x00409d03`), which is all a range
  // of 0 has. Without a range the goal is the centre whatever the grid says:
  // the search heads for it either way, and only a lock asks more of it.
  const std::int32_t r0 = range < kRingMaxRadius ? range : kRingMaxRadius;
  std::vector<Point>& goals = ring_points_;
  goals.clear();
  const auto add = [&](Point p) {
    const std::int32_t cx = ObstructionGrid::cell_of(p.x);
    const std::int32_t cy = ObstructionGrid::cell_of(p.y);
    if (grid_.in_bounds(cx, cy) && !grid_.blocked_cell(cx, cy)) goals.push_back(p);
  };
  std::int32_t r = r0;
  for (int ring = 0; ring < kRingCount && r >= min_range && r >= 0; ++ring, r -= kRingStep) {
    if (r == 0) {
      add(centre);
      break;
    }
    const std::int64_t reach = r - 1;
    for (const auto& dir : kRingDirections) {
      add(Point{
          truncate_fixed((static_cast<std::int64_t>(centre.x) << kRingShift) + reach * dir[0]),
          truncate_fixed((static_cast<std::int64_t>(centre.y) << kRingShift) + reach * dir[1])});
    }
  }

  // Where the search heads when no goal is left (`[0x008bfc10]`): the goal
  // point nearest the centre, the first on a tie, as each is added
  // (`0x00409c9e`) -- before any is struck off -- or the centre itself when
  // there was none (`0x0040a34b`).
  RingGoal out{centre, false};
  std::int64_t inner = -1;
  for (const Point& p : goals) {
    const std::int64_t gap = dist_sq(p, centre);
    if (inner < 0 || gap < inner) {
      inner = gap;
      out.at = p;
    }
  }

  // With an owner, the smart pathfinder first strikes off every goal point a
  // standing unit or a lock covers (`0x0040a310`, through `0x00409650`): the
  // free-spot test's takers (`0x004094d0`), each striking the points within
  // its radius plus the owner's -- **at that distance too**, where the
  // free-spot test wants nearer -- among those whose centre lies in the goal
  // set's box, `range` round the centre to whole cells, widened by twice the
  // free-spot margin (`0x0040a3f9`).
  std::vector<StaticLock>& takers = ring_takers_;
  takers.clear();
  std::int32_t own = 0;
  if (owner != kNoObject && !goals.empty()) {
    own = radius_of(world, owner);
    std::int32_t margin = kCoopQueryMargin;
    if (traits_of(world, owner).water_unit) {
      const ClassGraph* graph = world.class_graph();
      const ClassIndex ship = graph == nullptr ? kNoClass : graph->find("ShipBattle");
      std::int32_t value = 0;
      if (ship != kNoClass && parse_int(graph->property(ship, "radius"), value)) margin = value;
    }
    const std::int32_t cell = kCollisionCellSize;
    const std::int32_t x0 = ObstructionGrid::cell_of(centre.x - r0) * cell - 2 * margin;
    const std::int32_t y0 = ObstructionGrid::cell_of(centre.y - r0) * cell - 2 * margin;
    const std::int32_t x1 = ObstructionGrid::cell_of(centre.x + r0) * cell + cell - 1 + 2 * margin;
    const std::int32_t y1 = ObstructionGrid::cell_of(centre.y + r0) * cell + cell - 1 + 2 * margin;
    const auto in_box = [&](Point p) { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; };
    // Every corner of the box is within half its diagonal of the centre.
    const std::int64_t half = static_cast<std::int64_t>(r0) + cell + 2 * margin;
    std::vector<ObjectId>& near = spot_scratch_;
    world.objects_in_radius(centre, static_cast<std::int32_t>(half * 3 / 2), ClassFilter{}, near);
    for (const ObjectId other : near) {
      if (other == owner) continue;
      const WorldObject* slot = world.find(other);
      if (slot == nullptr) continue;
      const ObjectState& st = slot->state;
      if (!st.flags.is_unit || st.is_held() || st.flags.unspawned || st.flags.in_air) continue;
      if (st.health == 0 || st.flags.has_active_path || !in_box(st.position)) continue;
      takers.push_back(StaticLock{st.position, radius_of(world, other)});
    }
    for (const StaticLock& lock : static_locks_) {
      if (in_box(lock.at)) takers.push_back(lock);
    }
    for (const Entry& entry : states_) {
      StaticLock lock;
      if (entry.id != owner && owned_lock(world, entry.id, lock) && in_box(lock.at)) {
        takers.push_back(lock);
      }
    }
  }

  // Every point is handed to the search in the order it was laid, marked
  // live or struck: the search ends at the first popped cell whose first
  // point is live, and its straight line runs to the live point nearest the
  // start (`sim/grid_search.hpp`).
  ring_goal_points_.clear();
  for (const Point& p : goals) {
    bool struck = false;
    for (const StaticLock& taker : takers) {
      const std::int64_t sum = static_cast<std::int64_t>(own) + taker.radius;
      if (dist_sq(p, taker.at) <= sum * sum) {
        struck = true;
        break;
      }
    }
    ring_goal_points_.push_back(GoalPoint{p, !struck});
    if (!struck) out.free = true;
  }
  out.points = ring_goal_points_;
  return out;
}

bool MovementSystem::free_spot(World& world, ObjectId id, Point centre, Point& out) const {
  // `0x004180b0`, which the route search runs when an owner's search found no
  // goal point left and got nowhere (`0x00419197`). Its opening test refuses
  // a unit that has a target (`[unit+0x1a8]`, `Combatant::target`) and whose
  // squad's `Enemies` (`[squad+0x7c]`, `Squad::enemies`, what the squad's
  // look saw) holds more than 10: the answer is (-1, -1), with no draw, and
  // the unit stays queued where it is. A unit in a big fight does not walk
  // round it.
  if (const HeroSystem* heroes = hero_system_of(world); heroes != nullptr) {
    const Squad* squad = squad_containing(*heroes, id);
    const CombatSystem* combat = combat_system_of(world);
    const Combatant* self = combat != nullptr ? combat->find(id) : nullptr;
    if (squad != nullptr && squad->enemies.size() > kFreeSpotCrowd && self != nullptr &&
        self->target != kNoObject) {
      return false;
    }
  }
  //
  // One heading, `rand(0, 359)` degrees from the synchronised generator
  // (slot `0x14`), turned into a step of the unit's radius with `Rot`'s
  // constant: `(trunc(r * sin a), trunc(r * cos a))`, which is
  // `rotate_like_gbr` of `(0, r)`. The probe starts on the centre itself and
  // steps outward, forty times at most, and the first point whose cell is
  // passable and which is a free spot (`0x0040a990`) is the answer. A step
  // off the map ends the search, as running out of probes does.
  const std::int32_t radius = radius_of(world, id);
  const std::int32_t heading = world.rng().between(0, kFreeSpotHeadings - 1);
  const Point step = rotate_like_gbr(Point{0, radius}, heading);
  Point p = centre;
  for (std::int32_t probe = 0; probe < kFreeSpotProbes; ++probe) {
    if (!grid_.blocked(p) && spot_free(world, id, p, radius)) {
      out = p;
      return true;
    }
    p = Point{p.x + step.x, p.y + step.y};
    if (!grid_.in_bounds(ObstructionGrid::cell_of(p.x), ObstructionGrid::cell_of(p.y))) {
      return false;
    }
  }
  return false;
}

bool MovementSystem::owned_lock(const World& world, ObjectId id, StaticLock& lock) const {
  const MoveState* move = find(id);
  // A route that exists has at least two points.
  if (move == nullptr || !move->has_path || !lock_owner(id, *move)) return false;
  // `SetMedia` puts it at the route's last point (`vtbl+0x10` of the media),
  // and `0x0040a580` leaves its own radius at 0, which `0x0040a880` reads as
  // the owner's class radius.
  lock.at = move->waypoints.back();
  lock.radius = radius_of(world, id);
  return true;
}

bool MovementSystem::spot_free(const World& world, ObjectId asker, Point at,
                               std::int32_t radius) const {
  // `0x0040a990` asks the buckets for everything within the radius plus 40 --
  // plus the radius of class `ShipBattle` for a water unit (`[class+0xb30]`)
  // -- and `0x0040a880` measures each by its own radius. A blocker wider than
  // the margin whose centre is outside the query is not seen, as in the
  // cooperative step.
  std::int64_t margin = kCoopQueryMargin;
  if (traits_of(world, asker).water_unit) {
    const ClassGraph* graph = world.class_graph();
    const ClassIndex ship = graph == nullptr ? kNoClass : graph->find("ShipBattle");
    std::int32_t value = 0;
    if (ship != kNoClass && parse_int(graph->property(ship, "radius"), value)) margin = value;
  }
  const std::int64_t reach = radius + margin;
  const auto taken = [&](Point centre, std::int32_t other) {
    const std::int64_t gap = dist_sq(centre, at);
    if (gap > reach * reach) return false;
    const std::int64_t sum = static_cast<std::int64_t>(radius) + other;
    // `isqrt(d^2) < sum`, which for whole numbers is `d^2 < sum^2`.
    return gap < sum * sum;
  };

  // `0x004094d0`: a unit with a health, not the asker, without an active path.
  std::vector<ObjectId>& near = spot_scratch_;
  world.objects_in_radius(at, static_cast<std::int32_t>(reach), ClassFilter{}, near);
  for (const ObjectId other : near) {
    if (other == asker) continue;
    const WorldObject* slot = world.find(other);
    if (slot == nullptr) continue;
    const ObjectState& st = slot->state;
    if (!st.flags.is_unit || st.is_held() || st.flags.unspawned || st.flags.in_air) continue;
    if (st.health == 0 || st.flags.has_active_path) continue;
    if (taken(st.position, radius_of(world, other))) return false;
  }
  // `... or a lock that is not the asker's own.`
  for (const StaticLock& lock : static_locks_) {
    if (taken(lock.at, lock.radius)) return false;
  }
  for (const Entry& entry : states_) {
    if (entry.id == asker) continue;
    StaticLock lock;
    if (owned_lock(world, entry.id, lock) && taken(lock.at, lock.radius)) return false;
  }
  return true;
}

bool MovementSystem::arrival_spot_free(const World& world, ObjectId id, const MoveState& move,
                                       Point here) const {
  // `0x004178d0` names no owner for an order without the flag, and then
  // `IsArrived` asks nothing more than the band.
  if (!lock_owner(id, move)) return true;
  return spot_free(world, id, here, radius_of(world, id));
}

void MovementSystem::recast(World& world, std::size_t entry, Point here, GameTime now) {
  const ObjectId id = states_[entry].id;
  MoveState& move = states_[entry].state;
  const Point goal =
      move.target_object != kNoObject && world.find(move.target_object) != nullptr
          ? position(world, move.target_object)
          : move.target;
  set_position(world, id, here);
  lay_path(world, id, move, goal, move.range, now, &here);
  // The path follower clears the re-aim after the search it runs at a
  // route's end, whatever that search found (`0x0041a097`, `0x0041a0ae`).
  move.free_spot_aimed = false;
}

MovementSystem::OverlapCensus MovementSystem::overlap_census(const World& world) const {
  struct Standing {
    Point at;
    std::int32_t radius = 0;
  };
  std::vector<Standing> standing;
  std::int32_t widest = 0;
  for (const WorldObject& slot : world.objects()) {
    const ObjectState& s = slot.state;
    if (!s.flags.is_unit || s.is_held() || s.flags.unspawned || s.flags.in_air) continue;
    if (s.health == 0 || s.flags.has_active_path) continue;
    // `Disappear` stands a unit at `(-1, -1)`, off the map and drawn nowhere
    // (`UNIT_DISAPPEAR.VS`); villagers gone into their houses wait there.
    if (s.position == kHeldPosition) continue;
    const std::int32_t radius = radius_of(world, slot.id);
    if (radius <= 0) continue;
    standing.push_back(Standing{s.position, radius});
    widest = std::max(widest, radius);
  }
  // A sweep along x: two bodies further apart on that axis than the two
  // widest radii cannot touch.
  std::sort(standing.begin(), standing.end(), [](const Standing& a, const Standing& b) {
    return a.at.x != b.at.x ? a.at.x < b.at.x : a.at.y < b.at.y;
  });
  OverlapCensus census;
  census.bodies = standing.size();
  for (std::size_t i = 0; i < standing.size(); ++i) {
    for (std::size_t j = i + 1; j < standing.size(); ++j) {
      const std::int64_t dx = static_cast<std::int64_t>(standing[j].at.x) - standing[i].at.x;
      if (dx >= 2 * static_cast<std::int64_t>(widest)) break;
      const std::int64_t sum = static_cast<std::int64_t>(standing[i].radius) + standing[j].radius;
      const std::int64_t gap = dist_sq(standing[i].at, standing[j].at);
      if (gap >= sum * sum) continue;
      ++census.touching;
      if (4 * gap < sum * sum) ++census.stacked;
    }
  }
  return census;
}

std::int32_t MovementSystem::walk_step(const World& world, ObjectId id,
                                       const MoveState& move) const {
  // `0x005d2920`: the walk slot's animation record, `+0x2dc`, which the
  // entity loader fills from `step` (`0x00600ff6`).
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->object == nullptr || slot->object->entity == nullptr) return 0;
  const EntityAnim* walk = slot->object->entity->anim(move.walk_anim);
  return walk == nullptr || walk->step <= 0 ? 0 : walk->step;
}

std::int32_t MovementSystem::formation_stride_or_walk(const World& world, ObjectId id,
                                                      const MoveState& move) const {
  // **Engine choice, labelled: a march's followers step 200.** In
  // `gbr.exe` a route aimed at a formation is cut by the unit's own walk step
  // as any other is (`RecastPathfind`, 0x00419730, builds a mode-2 route's
  // media with 0x005d2920; only the formation's own record takes 200). Here
  // the stride is also the avoidance's step (`sim/avoidance.hpp`), which is
  // this engine's model and not the original's, and with the walk step a
  // column stopped in mid-march -- an AI squad's `ClrCmd` ends its
  // `form_move` where it walks -- stands in piles: Crossroads seed 5's worst
  // overlap was 82 against 36 with 200. The followers keep the 200 they had
  // while the march was the hero's route; the hero keeps his own. The step at
  // which a follower makes the next sample is the original's, its walk step
  // (`formation_step`).
  if (move.form_aimed && move.party != kNoObject && move.party != id) return kFormationStride;
  const std::int32_t step = walk_step(world, id, move);
  // A route aimed at a formation with no walk step still has to be stepped,
  // since its samples are made at its steps, and takes the formation's 200.
  if (step == 0 && move.form_aimed) return kFormationStride;
  return step;
}

void MovementSystem::face(World& world, ObjectId id, Point towards) {
  MoveState& move = state(id);
  const Point from = position(world, id);
  const Point delta{towards.x - from.x, towards.y - from.y};
  set_facing(world, id, move, delta);
}

std::int64_t MovementSystem::remaining(ObjectId id) const noexcept {
  const MoveState* move = find(id);
  if (move == nullptr || !move->has_path) return 0;
  const std::int64_t done = move->travelled();
  return done >= move->path_length ? 0 : move->path_length - done;
}

bool MovementSystem::is_arrived(const World& world, ObjectId id) const {
  const MoveState* move = find(id);
  if (move == nullptr || !move->goto_active) return false;
  // `IsArrived` answers 0 while either stop bit is set (0x00417c19): a unit
  // walking on to stop, or stopped after a failed search, is not arrived.
  if (move->stopping()) return false;
  const Point here = world.resolve_position(id);
  // A route aimed at a formation (mode 2) is arrived once the formation's
  // path is spent and the unit stands on its station exactly (0x00417c4a ..
  // 0x00417c73: 0x005f22f0, then the position against the slot's sample).
  if (move->form_aimed) {
    const Formation* form = formation(move->party);
    if (form == nullptr || formation_going_on(move->party)) return false;
    return here == station_of(*form, id);
  }
  const Point goal =
      move->target_object == kNoObject ? move->target : world.resolve_position(move->target_object);
  return within(here, goal, move->range) && arrival_spot_free(world, id, *move, here);
}

std::int64_t MovementSystem::eta(ObjectId id) const noexcept {
  const MoveState* move = find(id);
  if (move == nullptr || !move->has_path) return -1;
  const std::int64_t rate = move->rate();
  if (rate <= 0) return -1;
  const std::int64_t left = remaining(id);
  // Round up: a turn short of arrival is a turn the caller would have to
  // discover by polling.
  return (left * kSpeedScale + rate - 1) / rate;
}

void MovementSystem::prepare(World& world, Entry& entry, GameTime now) {
  MoveState& move = entry.state;
  if (!move.has_path) return;

  if (world.find(entry.id) == nullptr) {
    // The object died mid-move. Drop the path rather than keep integrating for
    // a handle that will never be reused.
    move.has_path = false;
    move.waypoints.clear();
    move.gate_crossings.clear();
    return;
  }
  // Nor does a corpse walk. `CombatSystem::enter_dying` stops it, but an order
  // can still reach it afterwards -- a hero's army keeps the member until the
  // object is gone, and `SetCommand` on the army routes all of it -- and the
  // dying state lasts the whole death animation. All 15 dying objects in the
  // nine dumps are without the active-path bit.
  if (const CombatSystem* combat = combat_system_of(world);
      combat != nullptr && combat->is_dying(entry.id)) {
    stop(world, entry.id);
    return;
  }

  // A route being walked on to a place to stop is the stop branch's alone:
  // the path follower takes it (0x00419ee9) before anything that would lay a
  // route again (0x0041a022 on), so neither a drifting target nor a changed
  // grid re-lays it.
  if (move.stopping()) return;

  // A moving target that has drifted gets a fresh route. Done before the step
  // so that the unit spends this turn on the new route, not the stale one.
  if (move.target_object != kNoObject) {
    const Point target_now = position(world, move.target_object);
    if (!within(target_now, move.target, repath_threshold())) {
      lay_path(world, entry.id, move, target_now, move.range, now);
      if (!move.has_path) return;
    }
  }

  // Re-validate only when the grid has changed under the route. A building
  // finished across a unit's path is the case this catches.
  // A mover that ignores passability is re-laid too, and its new route is the
  // same straight line from where it stands.
  if (move.path_generation != grid_generation_) {
    move.path_generation = grid_generation_;
    const Point here = position(world, entry.id);
    bool clear = true;
    std::size_t segment = 0;
    (void)point_along(move.waypoints, move.travelled(), &segment);
    for (std::size_t i = segment + 1; i < move.waypoints.size() && clear; ++i) {
      const Point from = i == segment + 1 ? here : move.waypoints[i - 1];
      clear = grid_.line_is_clear(from, move.waypoints[i]);
    }
    if (!clear) {
      // Try once to route around the new obstruction from where we stand.
      const Point dest = move.target;
      const std::int32_t range = move.range;
      if (lay_path(world, entry.id, move, dest, range, now) == MoveOutcome::blocked) {
        move.last_outcome = MoveOutcome::blocked;
        return;
      }
    }
  }
}

void MovementSystem::walk_turn(World& world, const Turn& turn, Entry& entry) {
  MoveState& move = entry.state;
  if (!move.has_path) return;

  // The stop branch, as `decide` reads it, tested once a turn here.
  if (move.stopping() &&
      stop_branch_halts(world, entry.id, move, position(world, entry.id), move.travelled())) {
    halt(world, entry.id, move);
    return;
  }

  const std::int64_t rate = move.rate();
  if (rate <= 0) {
    move.last_outcome = MoveOutcome::moving;
    return;
  }
  // A gate ahead that bars this mover: it stands the turn out before it.
  if (held_at_gate(world, entry.id, move, move.travelled())) {
    ++counters_.gate_waits;
    move.last_outcome = MoveOutcome::moving;
    return;
  }

  // The one line the whole design is about: an exact integer sum, never a
  // per-turn position increment. See the header.
  move.progress += rate * static_cast<std::int64_t>(turn.length);

  const std::int64_t travelled = move.travelled();
  std::size_t segment = 0;
  const Point next = point_along(move.waypoints, travelled, &segment);
  const Point previous = position(world, entry.id);
  set_position(world, entry.id, next);

  if (next.x != previous.x || next.y != previous.y) move.last_moved = turn.time;

  // Face along the *leg being walked*, which is a pure function of the distance
  // travelled -- never along the step just taken, which is a function of how the
  // interval was cut into turns. Facing is hashed state, so a step-derived
  // heading would leave one turn of 800 and two of 400 on the same coordinate
  // pointing different ways: a hash divergence with nothing visible behind it,
  // which is the worst kind to chase.
  //
  // Past the end of the route the leg clamps to the *last* one rather than
  // vanishing, for the same reason: otherwise the final heading would depend on
  // whether a turn boundary happened to fall inside the last leg.
  if (move.waypoints.size() >= 2) {
    const std::size_t leg_start =
        travelled >= move.path_length ? move.waypoints.size() - 2 : segment;
    const Point leg{move.waypoints[leg_start + 1].x - move.waypoints[leg_start].x,
                    move.waypoints[leg_start + 1].y - move.waypoints[leg_start].y};
    set_facing(world, entry.id, move, leg);
  }

  const bool at_end = travelled >= move.path_length;
  const bool in_range = move.range > 0 && within(next, move.target, move.range);
  if (move.stopping() && at_end) {
    halt(world, entry.id, move);
    return;
  }
  // `IsArrived`'s free spot, as `decide` asks it.
  if ((at_end || in_range) && !arrival_spot_free(world, entry.id, move, next)) {
    if (at_end) {
      const Point goal =
          move.target_object != kNoObject && world.find(move.target_object) != nullptr
              ? position(world, move.target_object)
              : move.target;
      lay_path(world, entry.id, move, goal, move.range, turn.time);
      move.free_spot_aimed = false;
      return;
    }
    move.last_outcome = MoveOutcome::moving;
    return;
  }
  if (at_end || in_range) {
    move.has_path = false;
    mark_path(world, entry.id, false);
    move.waypoints.clear();
    move.gate_crossings.clear();
    move.path_length = 0;
    move.progress = 0;
    move.holding = false;
    // A route that never reached the goal has run out, not arrived. The
    // distinction is what a caller's retry loop needs.
    move.last_outcome = (in_range || move.path_complete) ? MoveOutcome::arrived
                                                         : MoveOutcome::exhausted;
    return;
  }
  move.last_outcome = MoveOutcome::moving;
}

// --------------------------------------------------------------------------
// the cooperative step
// --------------------------------------------------------------------------

bool MovementSystem::cooperates(const World& world, const MoveState& move, ObjectId id) const {
  if (!move.has_path || move.stride <= 0) return false;
  const WorldObject* slot = world.find(id);
  return slot != nullptr && !slot->state.is_held() && !slot->state.flags.in_air;
}

Point MovementSystem::offset_at(const MoveState& move, std::int64_t travelled) const noexcept {
  const std::int64_t span = move.step_end - move.step_start;
  // Only a freshly laid route has no span, and both its offsets are zero; the
  // clamps below bite only on an accumulator a fraction of a unit past its
  // step. Equivalences, and the arithmetic's guards.
  if (span <= 0) return move.offset_to;
  std::int64_t done = travelled - move.step_start;
  if (done < 0) done = 0;
  if (done > span) done = span;
  return Point{
      static_cast<std::int32_t>(move.offset_from.x +
                                (static_cast<std::int64_t>(move.offset_to.x) - move.offset_from.x) *
                                    done / span),
      static_cast<std::int32_t>(move.offset_from.y +
                                (static_cast<std::int64_t>(move.offset_to.y) - move.offset_from.y) *
                                    done / span)};
}

Point MovementSystem::cooperative_position(const MoveState& move, std::int64_t progress) const {
  std::int64_t travelled = progress / kSpeedScale;
  if (travelled > move.path_length) travelled = move.path_length;
  const Point on_route = point_along(move.waypoints, travelled);
  const Point off = offset_at(move, travelled);
  return Point{on_route.x + off.x, on_route.y + off.y};
}

std::int64_t MovementSystem::progress_at(std::size_t entry, GameTime t) const {
  const MoveState& move = states_[entry].state;
  if (mode_[entry] == kCooperative) {
    if (move.holding) return move.progress;
    return move.progress + move.rate() * (t - since_[entry]);
  }
  // A route walked a whole turn at once is still where its accumulator says
  // at `t`: its account has not been touched since the turn began.
  const std::int64_t whole = move.path_length * kSpeedScale;
  const std::int64_t p = move.progress + move.rate() * (t - turn_t0_);
  return p > whole ? whole : p;
}

Point MovementSystem::body_position(const World& world, const Body& body, GameTime t) const {
  if (body.entry == kUntracked) return body.start;
  const MoveState& move = states_[body.entry].state;
  if (!move.has_path || mode_[body.entry] == kStill) {
    // Standing, or arrived earlier in this turn, which wrote the position.
    const WorldObject* slot = world.find(body.id);
    return slot == nullptr ? body.start : slot->state.position;
  }
  const std::int64_t p = progress_at(body.entry, t);
  if (mode_[body.entry] == kCooperative) return cooperative_position(move, p);
  return point_along(move.waypoints, p / kSpeedScale);
}

GameTime MovementSystem::next_decision(const MoveState& move, GameTime now) const noexcept {
  if (!move.has_path) return kNever;
  if (move.holding) return move.hold_until > now ? move.hold_until : now;
  const std::int64_t rate = move.rate();
  if (rate <= 0) return kNever;
  const std::int64_t need = move.step_end * kSpeedScale - move.progress;
  if (need <= 0) return now;
  // Exact: the first whole millisecond at which the accumulator has reached
  // the end of the step. However a turn boundary cuts the interval, the
  // ceiling lands on the same instant.
  return now + (need + rate - 1) / rate;
}

namespace {

/// The inclusive bucket range `[lo, hi]` a coordinate interval covers, clamped
/// to `count` buckets.
void bucket_span(std::int64_t from, std::int64_t to, std::int32_t count, std::int32_t& lo,
                 std::int32_t& hi) noexcept {
  const auto clamp = [count](std::int64_t v) {
    const std::int64_t b = v < 0 ? 0 : v / kBucket;
    return static_cast<std::int32_t>(b >= count ? count - 1 : b);
  };
  lo = clamp(from);
  hi = clamp(to);
}

}  // namespace

void MovementSystem::index_bodies(const World& world) {
  bodies_.clear();
  movers_.clear();
  std::int32_t max_x = grid_.extent_x();
  std::int32_t max_y = grid_.extent_y();
  for (const WorldObject& slot : world.objects()) {
    const ObjectState& s = slot.state;
    // `0x00416f60`: a unit, and not a dead one. Held units are not on the
    // ground at all -- their position is `kHeldPosition`, off every map, so
    // leaving them in would change nothing -- a spawn template is not in play,
    // and an airborne bird is inference 4.
    if (!s.flags.is_unit || s.is_held() || s.flags.unspawned || s.flags.in_air) continue;
    if (s.health == 0) continue;
    Body body;
    body.id = slot.id;
    body.start = s.position;
    body.radius = radius_of(world, slot.id);
    const std::size_t at = lower_bound(slot.id);
    body.entry = at < states_.size() && states_[at].id == slot.id ? at : kUntracked;
    // A body with a route is somewhere else by the time anybody asks, so it is
    // not bucketed by where it began: `decide` reads every one of them where
    // it is at that instant. Nothing starts moving in the middle of a turn --
    // orders arrive between turns -- so a body without a route stays where the
    // bucket says for the whole of it.
    if (body.entry != kUntracked && states_[body.entry].state.has_path) {
      movers_.push_back(body);
      continue;
    }
    bodies_.push_back(body);
    if (s.position.x > max_x) max_x = s.position.x;
    if (s.position.y > max_y) max_y = s.position.y;
  }

  Buckets& b = body_buckets_;
  b.columns = max_x / kBucket + 1;
  b.rows = max_y / kBucket + 1;
  const std::size_t cells = static_cast<std::size_t>(b.columns) * static_cast<std::size_t>(b.rows);
  b.first.assign(cells + 1, 0);
  const auto cell_of_body = [&b](Point p) {
    std::int32_t cx = 0, cy = 0, unused = 0;
    bucket_span(p.x, p.x, b.columns, cx, unused);
    bucket_span(p.y, p.y, b.rows, cy, unused);
    return static_cast<std::size_t>(cy) * static_cast<std::size_t>(b.columns) +
           static_cast<std::size_t>(cx);
  };
  for (const Body& body : bodies_) ++b.first[cell_of_body(body.start) + 1];
  for (std::size_t i = 1; i <= cells; ++i) b.first[i] += b.first[i - 1];
  b.items.assign(bodies_.size(), 0);
  std::vector<std::uint32_t> cursor(b.first.begin(), b.first.end() - 1);
  for (std::uint32_t i = 0; i < bodies_.size(); ++i) {
    b.items[cursor[cell_of_body(bodies_[i].start)]++] = i;
  }
}

void MovementSystem::index_static_locks() {
  Buckets& b = lock_buckets_;
  std::int32_t max_x = grid_.extent_x();
  std::int32_t max_y = grid_.extent_y();
  for (const StaticLock& lock : static_locks_) {
    if (lock.at.x > max_x) max_x = lock.at.x;
    if (lock.at.y > max_y) max_y = lock.at.y;
  }
  b.columns = max_x / kBucket + 1;
  b.rows = max_y / kBucket + 1;
  const std::size_t cells = static_cast<std::size_t>(b.columns) * static_cast<std::size_t>(b.rows);
  b.first.assign(cells + 1, 0);
  const auto cell_of_lock = [&b](Point p) {
    std::int32_t cx = 0, cy = 0, unused = 0;
    bucket_span(p.x, p.x, b.columns, cx, unused);
    bucket_span(p.y, p.y, b.rows, cy, unused);
    return static_cast<std::size_t>(cy) * static_cast<std::size_t>(b.columns) +
           static_cast<std::size_t>(cx);
  };
  for (const StaticLock& lock : static_locks_) ++b.first[cell_of_lock(lock.at) + 1];
  for (std::size_t i = 1; i <= cells; ++i) b.first[i] += b.first[i - 1];
  b.items.assign(static_locks_.size(), 0);
  std::vector<std::uint32_t> cursor(b.first.begin(), b.first.end() - 1);
  for (std::uint32_t i = 0; i < static_locks_.size(); ++i) {
    b.items[cursor[cell_of_lock(static_locks_[i].at)]++] = i;
  }
  lock_buckets_valid_ = true;
}

void MovementSystem::arrive(World& world, std::size_t entry, bool in_range) {
  Entry& e = states_[entry];
  MoveState& move = e.state;
  const std::int64_t travelled =
      std::min<std::int64_t>(move.progress / kSpeedScale, move.path_length);
  const Point where = cooperative_position(move, move.progress);
  // The facing `walk_turn` would have left: the leg being walked, or the last
  // one at the end of the route.
  if (move.waypoints.size() >= 2) {
    std::size_t segment = 0;
    (void)point_along(move.waypoints, travelled, &segment);
    const std::size_t leg_start =
        travelled >= move.path_length ? move.waypoints.size() - 2 : segment;
    const Point leg{move.waypoints[leg_start + 1].x - move.waypoints[leg_start].x,
                    move.waypoints[leg_start + 1].y - move.waypoints[leg_start].y};
    set_facing(world, e.id, move, leg);
  }
  set_position(world, e.id, where);
  move.has_path = false;
  mark_path(world, e.id, false);
  move.waypoints.clear();
  move.gate_crossings.clear();
  move.path_length = 0;
  move.progress = 0;
  move.holding = false;
  move.step_start = 0;
  move.step_end = 0;
  move.offset_from = Point{};
  move.offset_to = Point{};
  move.retry_time = 0;
  move.last_outcome =
      (in_range || move.path_complete) ? MoveOutcome::arrived : MoveOutcome::exhausted;
  // A route walked to its end, or arrived on, is no longer re-aimed
  // (`0x0041a0ae`); and the `Goto` family's arrival deletes the whole retry
  // (`0x005d3830(0)`), the free-spot search's own flag with it.
  move.free_spot_aimed = false;
  if (move.last_outcome == MoveOutcome::arrived) move.free_spot_tried = false;
}

void MovementSystem::decide(World& world, std::size_t entry, GameTime now) {
  const ObjectId id = states_[entry].id;
  MoveState& move = states_[entry].state;
  // A route aimed at a formation steps through 0x00419b20 first: laid again
  // to a new sample, more samples made as the unit nears its station, the
  // pace set. Laid again, the step is decided afresh at this same instant.
  if (move.form_aimed && !move.stopping()) {
    const std::vector<Point>::size_type before = move.waypoints.size();
    const std::int64_t length_before = move.path_length;
    const std::int32_t seen_before = move.form_seen;
    if (formation_step(world, entry, now) == FormStep::done) return;
    if (move.form_seen != seen_before || move.waypoints.size() != before ||
        move.path_length != length_before) {
      if (!move.has_path || move.holding) return;
    }
  }
  const std::int64_t travelled = move.progress / kSpeedScale;
  const Point here = cooperative_position(move, move.progress);

  // `CVXPathRetry` asks whether it has arrived before it steps: at the end of
  // the route, or inside the arrival annulus. A route that ever enters the
  // annulus is one the search `found`, so `arrive`'s two answers only differ
  // for a search that ran out of budget first.
  //
  // For an order that owns a lock, `IsArrived` (`0x00417c10`) also wants the
  // spot free (`0x0040a990`). Inside the band on a taken spot the route walks
  // on; at its end on one, it is laid again from here (`0x0041a056` ->
  // `RecastPathfind`), which cuts it back to a free spot or leaves the unit
  // where it stands, not arrived.
  const bool at_end = travelled >= move.path_length;
  const bool in_range = move.range > 0 && within(here, move.target, move.range);
  if (move.stopping()) {
    // `Unit::Stop` asked (`[retry+0x20] & 2`), or the last search laid
    // nothing (`& 4`, `MoveState::route_failed`): the path follower's other
    // branch (0x00419ee6), which never asks `IsArrived`
    // (`stop_branch_halts`). `request_stop` set bit 1 only for an owner, and
    // an order that has stopped owning a lock since takes no step at all
    // (0x00419ef8).
    if (stop_branch_halts(world, id, move, here, travelled)) {
      set_position(world, id, here);
      halt(world, id, move);
      return;
    }
  } else if (at_end || in_range) {
    if (arrival_spot_free(world, id, move, here)) {
      const bool aimed = move.form_aimed;
      arrive(world, entry, in_range);
      // At the end of a formation's spent path the unit faces the way the
      // formation does (0x0053f5f0 on `[form+0x38]`).
      if (aimed) {
        if (const Formation* form = formation(move.party); form != nullptr) {
          set_facing(world, id, move, form->dir);
        }
      }
      return;
    }
    if (at_end) {
      recast(world, entry, here, now);
      return;
    }
  }

  // A gate on the route ahead that bars this mover (0x00418260): it stands
  // where it is, the route kept, and asks again on the next turn, when the
  // gate is read again. The cooperative wait is not involved.
  if (held_at_gate(world, id, move, travelled)) {
    ++counters_.gate_waits;
    const Point standing = offset_at(move, travelled);
    move.holding = true;
    move.hold_until = turn_end_ + 1;
    move.step_start = travelled;
    move.step_end = travelled;
    move.offset_from = standing;
    move.offset_to = standing;
    return;
  }

  // The step to test: the one after the last one tested, which after a hold is
  // ahead of the unit (the sampled route advanced on every ask). With none
  // left, the last is tested again -- inference 1.
  const std::int64_t base = std::max(move.step_end, travelled);
  const std::int64_t boundary = base >= move.path_length
                                    ? move.path_length
                                    : next_step_boundary(move.waypoints, move.stride, base);
  const Point step_end = point_along(move.waypoints, boundary);
  const Point drift = offset_at(move, travelled);

  // `0x00416f60` over everything within the unit's radius plus 40.
  const std::int32_t radius = radius_of(world, id);
  const std::int64_t reach = static_cast<std::int64_t>(radius) + kCoopQueryMargin;
  bool blocked = false;
  bool hard = false;
  neighbours_.clear();

  const auto consider = [&](const Body& body) {
    if (body.id == id) return;
    // `0x005d2f80`: one march does not block itself.
    if (move.party != kNoObject && body.entry != kUntracked &&
        states_[body.entry].state.party == move.party) {
      return;
    }
    const Point at = body_position(world, body, now);
    const std::int64_t gap = dist_sq(at, step_end);
    if (gap > reach * reach) return;
    neighbours_.push_back(Neighbour{at, body.radius});
    const std::int64_t sum = static_cast<std::int64_t>(radius) + body.radius;
    if (gap < sum * sum) {
      blocked = true;
      // Bit 17 as it stands now: a mover that arrived earlier this turn is
      // standing, and a hard block.
      const bool moving = body.entry != kUntracked && states_[body.entry].state.has_path;
      if (!moving) hard = true;
    }
  };
  // Everybody with a route, wherever they have got to.
  for (const Body& body : movers_) consider(body);
  // Everybody standing, from the buckets they stand in.
  {
    const Buckets& b = body_buckets_;
    std::int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bucket_span(step_end.x - reach, step_end.x + reach, b.columns, x0, x1);
    bucket_span(step_end.y - reach, step_end.y + reach, b.rows, y0, y1);
    for (std::int32_t cy = y0; cy <= y1; ++cy) {
      for (std::int32_t cx = x0; cx <= x1; ++cx) {
        const std::size_t cell =
            static_cast<std::size_t>(cy) * static_cast<std::size_t>(b.columns) +
            static_cast<std::size_t>(cx);
        for (std::uint32_t k = b.first[cell]; k < b.first[cell + 1]; ++k) {
          consider(bodies_[b.items[k]]);
        }
      }
    }
  }
  if (!static_locks_.empty()) {
    const Buckets& b = lock_buckets_;
    std::int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bucket_span(step_end.x - reach, step_end.x + reach, b.columns, x0, x1);
    bucket_span(step_end.y - reach, step_end.y + reach, b.rows, y0, y1);
    for (std::int32_t cy = y0; cy <= y1; ++cy) {
      for (std::int32_t cx = x0; cx <= x1; ++cx) {
        const std::size_t cell =
            static_cast<std::size_t>(cy) * static_cast<std::size_t>(b.columns) +
            static_cast<std::size_t>(cx);
        for (std::uint32_t k = b.first[cell]; k < b.first[cell + 1]; ++k) {
          const StaticLock& lock = static_locks_[b.items[k]];
          const std::int64_t gap = dist_sq(lock.at, step_end);
          if (gap > reach * reach) continue;
          // The sidestep measures a lock by its class's radius, which is 0.
          neighbours_.push_back(Neighbour{lock.at, 0});
          const std::int64_t sum = static_cast<std::int64_t>(radius) + lock.radius;
          if (gap < sum * sum) {
            blocked = true;
            hard = true;
          }
        }
      }
    }
  }

  const auto take_step = [&](Point offset) {
    move.holding = false;
    move.step_start = travelled;
    move.step_end = boundary;
    move.offset_from = drift;
    move.offset_to = offset;
  };

  ++counters_.decisions;
  if (!blocked) {
    move.retry_time = 0;
    take_step(Point{});
    return;
  }
  ++counters_.blocked;

  move.retry_time = next_retry_time(move.retry_time);
  if (hard) {
    move.retry_time = kGiveWay;
  } else if (world.rng().between(1, kEarlyGiveWayOdds) == 1) {
    if (move.retry_time != kGiveWay) ++counters_.early;
    move.retry_time = kGiveWay;
  }

  if (move.retry_time != kGiveWay) {
    // A hold. The tested step is spent; the unit stands where it is.
    ++counters_.holds;
    move.holding = true;
    move.hold_until = now + move.retry_time;
    move.step_start = travelled;
    move.step_end = boundary;
    // The unit stays at `step_start`, where the interpolation reads
    // `offset_from` whatever `offset_to` holds; both are the drift so the
    // record says what the unit is doing.
    move.offset_from = drift;
    move.offset_to = drift;
    return;
  }

  // Give way: across the direction of travel, unless this is the route's
  // last point, which is stepped onto regardless.
  move.retry_time = 0;
  Point aim = step_end;
  if (boundary < move.path_length) {
    aim = pick_sidestep(step_end, here, radius, neighbours_, grid_, world.terrain(),
                        traits_of(world, id).water_unit);
  }
  if (aim != step_end) ++counters_.sidesteps;
  take_step(Point{aim.x - step_end.x, aim.y - step_end.y});
}

void MovementSystem::settle(World& world, const Turn& turn, std::size_t entry) {
  Entry& e = states_[entry];
  MoveState& move = e.state;
  if (!move.holding) move.progress += move.rate() * (turn.time - since_[entry]);
  since_[entry] = turn.time;
  const std::int64_t travelled =
      std::min<std::int64_t>(move.progress / kSpeedScale, move.path_length);
  set_position(world, e.id, cooperative_position(move, move.progress));
  if (move.waypoints.size() >= 2) {
    std::size_t segment = 0;
    (void)point_along(move.waypoints, travelled, &segment);
    const std::size_t leg_start =
        travelled >= move.path_length ? move.waypoints.size() - 2 : segment;
    const Point leg{move.waypoints[leg_start + 1].x - move.waypoints[leg_start].x,
                    move.waypoints[leg_start + 1].y - move.waypoints[leg_start].y};
    set_facing(world, e.id, move, leg);
  }
  move.last_outcome = MoveOutcome::moving;
}

void MovementSystem::advance(World& world, const Turn& turn) {
  if (turn.length <= 0) return;
  const GameTime t0 = turn.time - turn.length;
  turn_t0_ = t0;
  turn_end_ = turn.time;

  // Ascending id, which is spawn order. Iteration order is world state.
  for (Entry& entry : states_) prepare(world, entry, t0);

  const std::size_t count = states_.size();
  mode_.assign(count, kStill);
  since_.assign(count, t0);
  scheduled_.assign(count, kNever);
  turn_start_.resize(count);
  bool any = false;
  for (std::size_t i = 0; i < count; ++i) {
    const Entry& entry = states_[i];
    turn_start_[i] = position(world, entry.id);
    if (!entry.state.has_path) continue;
    if (cooperates(world, entry.state, entry.id)) {
      mode_[i] = kCooperative;
      any = true;
    } else {
      mode_[i] = kWholeTurn;
    }
  }

  // The decisions, in `(game time, id)` order. Only a route with a stride
  // makes them; everybody else is read where their accumulator puts them.
  if (any) {
    index_bodies(world);
    if (!lock_buckets_valid_) index_static_locks();
    using Due = std::tuple<GameTime, ObjectId, std::size_t>;
    std::priority_queue<Due, std::vector<Due>, std::greater<Due>> queue;
    for (std::size_t i = 0; i < count; ++i) {
      if (mode_[i] != kCooperative) continue;
      const GameTime at = next_decision(states_[i].state, t0);
      scheduled_[i] = at;
      if (at <= turn.time) queue.emplace(at, states_[i].id, i);
    }
    deciding_ = true;
    while (!queue.empty()) {
      const auto [at, id, i] = queue.top();
      queue.pop();
      (void)id;
      if (scheduled_[i] != at) continue;
      MoveState& move = states_[i].state;
      if (!move.has_path) continue;
      if (!move.holding) move.progress += move.rate() * (at - since_[i]);
      since_[i] = at;
      decide(world, i, at);
      const GameTime next = next_decision(move, at);
      scheduled_[i] = next;
      if (next <= turn.time) queue.emplace(next, states_[i].id, i);
    }
    deciding_ = false;
  }

  for (std::size_t i = 0; i < count; ++i) {
    Entry& entry = states_[i];
    if (mode_[i] == kCooperative) {
      if (entry.state.has_path) settle(world, turn, i);
      if (position(world, entry.id) != turn_start_[i]) entry.state.last_moved = turn.time;
    } else {
      walk_turn(world, turn, entry);
    }
    play_locomotion(world, entry.id, entry.state);
  }
}

/// Start the walk cycle when an object begins moving and stop it when it
/// arrives.
///
/// **`MoveState::walk_anim` was written by `SetWalkAnim` and read by nobody**,
/// so a unit under orders slid to its destination on a still frame. The slot is
/// the entity's locomotion animation -- 1 in every entity that declares one,
/// and the deer switches between 13 and 1 as it grazes and bolts.
///
/// **Only on the transition.** `World::play_anim` restarts the timeline at step
/// zero, so calling it every turn would pin the sprite on its first frame
/// forever -- which looks exactly like the bug this fixes. `walking` is the
/// memory that makes it a transition rather than a poll.
///
/// Stopping holds the last frame rather than entering an idle pose. What a
/// stationary object plays is its `<state>` machine's business and the scripts'
/// -- `StartAnim` and `enter_state` are how the corpus drives it -- and
/// choosing one here would be inventing a behaviour rather than restoring one.
///
/// **And only the walk.** Arrival stops the cursor only while it is still on
/// the walk slot. Something else may have taken it over in the meantime --
/// combat's swing, a death, a script's `PlayAnim` -- and freezing *that* on
/// the frame it had reached would be stopping an animation movement never
/// started. The case that showed it: a unit killed mid-walk is stopped by
/// `CombatSystem::enter_dying` and starts its death in the same turn, and the
/// next turn's transition here would have pinned the corpse on its first row.
void MovementSystem::play_locomotion(World& world, ObjectId id, MoveState& move) {
  const bool moving = move.has_path;
  if (moving == move.walking) return;
  move.walking = moving;
  if (moving) {
    (void)world.play_anim(id, move.walk_anim, AnimRepeat::loop);
    return;
  }
  const WorldObject* slot = world.find(id);
  if (slot != nullptr && slot->object != nullptr && slot->object->anim.anim_slot == move.walk_anim) {
    (void)world.stop_anim(id);
  }
}

void MovementSystem::set_facing(World& world, ObjectId id, MoveState& move,
                                Point heading) const {
  if (heading.x == 0 && heading.y == 0) return;  // names no direction
  move.facing = set_length(heading, kFacingLength);

  WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->object == nullptr) return;
  const Entity* entity = slot->object->entity;
  if (entity == nullptr) return;  // an object with no art has no column to pick
  slot->object->anim.variation = facing_column(move.facing, entity->variations());
}

void MovementSystem::hash(std::uint64_t& accumulator) const {
  // Positions, owners and `SyncFlags` -- including bit 17, the active-path flag
  // this system writes -- are already in `World::state_hash` over the object
  // table. Hashing them again here would only make a divergence report twice.
  // What is left is the movement state that has no home on the object.
  hash_u64(accumulator, states_.size());
  for (const Entry& entry : states_) {
    const MoveState& move = entry.state;
    hash_u64(accumulator, entry.id);
    hash_i32(accumulator, move.target.x);
    hash_i32(accumulator, move.target.y);
    hash_u64(accumulator, move.target_object);
    hash_i32(accumulator, move.range);
    hash_i32(accumulator, move.min_range);
    hash_i32(accumulator, move.facing.x);
    hash_i32(accumulator, move.facing.y);
    hash_i32(accumulator, move.speed);
    hash_i32(accumulator, move.speed_factor);
    // `walking` decides whether next turn restarts the walk cycle, which is
    // in the object hash a turn later; see `play_locomotion`.
    hash_u64(accumulator, move.walking ? 1u : 0u);
    // The accumulator decides where the unit will be next turn, so a
    // divergence in it is caught a turn before it becomes a divergence in a
    // coordinate. That is the whole value of hashing it.
    hash_u64(accumulator, static_cast<std::uint64_t>(move.progress));
    // How far a march's route looks ahead of a gate (`sim/gate.hpp`): the
    // radius of the formation this object leads, where it was hashed when it
    // lived on the movement record.
    const Formation* form = formation(entry.id);
    hash_i32(accumulator, form == nullptr ? 0 : form->radius);
    // Deliberately absent: `waypoints`, `path_length`, `path_generation`,
    // `last_outcome`. The route is the pathfinder's output, and the pathfinder
    // is not in the determinism contract -- `pathfinder` is zero in all nine
    // dumps and `WorldHashes` keeps it that way.
  }
}

MovementSystem* movement_system(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "movement") {
      return static_cast<MovementSystem*>(system);
    }
  }
  return nullptr;
}

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::Value;

// `CallContext::user` is a `HostContext*` for every domain; see
// `sim/host_context.hpp` for why it has to be one type and what went wrong
// while it was not.

/// The object a handle names, or `kNoObject`.
[[nodiscard]] ObjectId object_of(const Value& value) noexcept {
  if (!value.is_object() || value.as_object().type != kTypeObj) return kNoObject;
  return value.as_object().id;
}

/// A destination argument, which is a `point` at most call sites and an object
/// handle at the rest: `Goto(.hero, 250, 2000, false, -1)` paths to a moving
/// unit. Writes the object id when it was one, so the caller can track it.
[[nodiscard]] bool destination_of(World& world, const Value& value, Point& out,
                                  ObjectId& target) noexcept {
  if (is_point(value)) {
    out = unpack_point(value);
    target = kNoObject;
    return true;
  }
  const ObjectId id = object_of(value);
  if (id == kNoObject) return false;
  target = id;
  out = world.resolve_position(id);
  return true;
}

constexpr const char* kNoWorld = "movement: no World behind CallContext::user";
constexpr const char* kNoSystem = "movement: no movement system registered with the world";
constexpr const char* kNoReceiver = "movement: receiver is not an object";
constexpr const char* kNoPoint = "movement: argument is not a point or an object";

/// What every entry point below needs: the world, the system, and the receiver.
///
/// **A receiver that names nothing is answered, not trapped.** Every body in
/// this slice opens with the handle check -- the table slot at
/// `0x00a2a610[handle & 0xffff]` plus the offset, non-null -- and on a miss
/// formats `The function '<name>' called for an uninitialized or invalid
/// object.` into 0x00686eb0, a bare `ret` in retail, and pushes its default:
/// `false` for `Stop`, `HasPath`, `DoCarryNothing` and the object `Goto`,
/// **`true`** for the point `Goto` (0x005d61b0), `-1` for `PathTo`, `0` for
/// `TimeWithoutWalking`, `speed` and `FormRadius`, `(-1, -1)` for `dest`,
/// `(0, 1)` for `GetDir`, the empty string for `formation`, and nothing at
/// all for the setters. So the script goes on. Such a handle is `departed`
/// here (`receiver_departed`: object-shaped and naming nothing the world
/// holds); a value of another shape -- an integer, a point -- stays a trap,
/// since the compiler cannot produce one as a `Unit` receiver.
struct Self {
  World* world = nullptr;
  MovementSystem* movement = nullptr;
  ObjectId id = kNoObject;
  const char* error = nullptr;
  bool departed = false;

  [[nodiscard]] bool ok() const noexcept { return error == nullptr && !departed; }
  /// A missing world or system: this engine's wiring, never the script's.
  [[nodiscard]] bool broken() const noexcept { return error != nullptr; }
};

[[nodiscard]] Self resolve(CallContext& ctx) {
  Self self;
  self.world = world_of(ctx);
  if (self.world == nullptr) {
    self.error = kNoWorld;
    return self;
  }
  self.movement = movement_system(*self.world);
  if (self.movement == nullptr) {
    self.error = kNoSystem;
    return self;
  }
  if (ctx.count() == 0) {
    self.error = kNoReceiver;
    return self;
  }
  self.id = object_of(ctx.arg(0));
  if (self.id != kNoObject && self.world->find(self.id) != nullptr) return self;
  if (self.id != kNoObject || receiver_departed(*self.world, ctx.arg(0))) {
    self.departed = true;
  } else {
    self.error = kNoReceiver;
  }
  return self;
}

/// The answer an entry point gives when `resolve` did not produce a receiver:
/// a trap for this engine's own missing wiring, `fallback` for a departed one.
[[nodiscard]] HostOutcome unresolved(const Self& self, HostOutcome fallback) {
  if (self.broken()) return HostOutcome::failed(self.error);
  return fallback;
}

/// Game time now. The scheduler owns the clock when there is one; a bare `Vm`
/// run falls back to the world's.
[[nodiscard]] GameTime now_of(CallContext& ctx, World& world) {
  if (ctx.scheduler != nullptr) return ctx.scheduler->now();
  return world.time();
}

// -- Goto ------------------------------------------------------------------

/// What an object destination adds to `Goto`'s range: **both class radii**.
///
/// `gbr.exe`'s route record keeps its goal in one of two modes (`[rec+0x28]`):
/// 0 is a point and 1 an object, and the arrival test 0x00417c10 measures the
/// two differently. For a point it is `isqrt(dx² + dy²)` against the range; for
/// an object it is the same distance to the target **less the mover's class
/// `radius` and the target's** (`[class+0x2dc]` on each, through 0x00417920 and
/// the target's own class record), so the range of an object `Goto` is a gap
/// between two edges and not between two centres. The mode-1 setter 0x0041a5c0
/// is what the object overload of `Goto` (0x005d6b90) reaches.
///
/// What it decides on Crossroads: `UNIT_CAPTURE.VS:41` walks a unit to an
/// independent camp with `Goto(b, 0, ...)`. Measured centre to centre, a camp
/// whose footprint is open walked the unit onto the camp's anchor, and the
/// mirror walk at line 54 -- `Goto(b.pos + b.pos - .pos, ...)` -- then aimed
/// at the very point it stood on, answered at once without suspending, and
/// spun the script's whole instruction budget (916 traps in 4,000 turns).
/// Measured edge to edge, the unit stops where the camp begins.
///
/// **Labelled, not reproduced:** the inner bound. The original's test is an
/// annulus, `min <= gap <= range`, and `Goto` passes no minimum, so a mover
/// already overlapping its target (a negative gap) has *not* arrived there
/// and is routed out. This engine's movement carries a minimum and never acts
/// on one (`GotoAttack`'s is stored and ignored the same way), so an
/// overlapping mover counts as arrived here. The group radius 0x00417920
/// takes instead of the mover's when the route belongs to a formation is not
/// reproduced either: no script `Goto` has one.
[[nodiscard]] std::int32_t object_goto_reach(const World& world, ObjectId mover,
                                             ObjectId target) {
  // An equivalence, kept for the reading: `find(kNoObject)` is null anyway.
  if (target == kNoObject) return 0;
  const WorldObject* a = world.find(mover);
  const WorldObject* b = world.find(target);
  if (a == nullptr || b == nullptr) return 0;
  // Each clamped on its own, so that a radius below zero -- which is how the
  // original's class constructor can leave a number no class declared --
  // takes nothing off rather than eating the other one.
  const std::int32_t mine = class_int(world, *a, "radius");
  const std::int32_t theirs = class_int(world, *b, "radius");
  return (mine > 0 ? mine : 0) + (theirs > 0 ? theirs : 0);
}

/// `Goto/4` and `Goto/5`. Arguments after the receiver are `(destination,
/// range, slice, flag, give_up)`. See the header for which of those are
/// established and which are read from the shape of the call sites.
///
/// **Both forms drop the unit's combat target**, as `Stop` does, through the
/// same pair of writes (the handle at `[unit+0x1a8]` emptied and the attack
/// count zeroed, only when there is a target; `CombatSystem::drop_target`):
///
///   * the **object** form (0x005d6b90) on every entry, first or resumed,
///     before anything else -- the write at 0x005d6c2f precedes even the
///     worker it hands the call to;
///   * the **point** form (0x005d61b0) once the unit is out of any holder
///     (0x005d639a): on the first entry, or on a resume that has just stepped
///     out or finds no route. A resume that is walking a route keeps it.
///
/// One call here is one first entry and its resumes in the original (the
/// walking path answers the caller's loop rather than resuming), so both
/// forms drop the target on every call that gets as far as their write.
///
/// **What each body checks before it walks**, in its order:
///
///   * the receiver. The point form (0x005d61b0) answers **true** for a
///     handle naming nothing -- the script's loop `while (!.Goto(...))` ends
///     -- and the object form (0x005d6b90) answers false (`resolve`);
///   * **the point form clamps its destination to the map rectangle**
///     (`[map+0xb4]`..`[map+0xc0]`, which is `(0, 0, w-1, h-1)`), writing the
///     clamped point back into its own argument so every re-entry walks to
///     the same place; `ClipDestToMap` is the same clamp. A point off the map
///     used to be handed to the pathfinder as it was;
///   * the holder step-out, the `(-1, -1)` poll and the re-entry test, which
///     `run_goto` carries;
///   * **the object form routes only to a target it may approach**
///     (0x005d40c0 at 0x005d4291..0x005d42d7). A target that is the mover
///     itself answers true at once. A target that is a unit (bit 0x400000 of
///     `[obj+0x2c]`) is routed to only when it is in no holder and either not
///     hidden (bit 21) or hidden from nobody on the mover's side (its one-hot
///     owner bits against the side mask `[record+0x18]` of the mover's
///     owner). Anything else -- a handle naming nothing, a garrisoned or a
///     concealed enemy unit -- lays no route and does not stamp a failure:
///     the call clears its walking state (`[state] = 0`), waits the slice and
///     answers false at its re-entry. Here that is the slice's suspension with
///     `false`. **Approximated:** the side mask is not modelled, so a hidden
///     unit is approached only by its own player's units;
///   * for a target that is not a unit, the route is the point form's to the
///     target's position with both class radii added to the band
///     (0x005d42a6, `SetDest` 0x0041a4c0); for a unit, the object mode
///     (0x0041a5c0), whose arrival test subtracts the same radii
///     (`object_goto_reach`). Both measure the gap between the edges.
///
/// **The answer comes at the re-entry**, as `run_goto` describes for the
/// whole family: the first run waits the slice, the re-entry answers whether
/// the unit has arrived. **Not reproduced:** with no route laid the original
/// plays the "look around" animation when the call's fourth argument is set
/// (0x0053e6c0 with index 0x13, the script's slot 20) and waits the longer of
/// the slice and that animation; here the wait is the slice.
HostOutcome goto_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  const bool object_form = ctx.count() > 1 && !is_point(ctx.arg(1));
  if (!self.ok()) {
    return unresolved(self, HostOutcome::ok_with(Value::boolean(!object_form)));
  }

  GotoOrder order;
  const std::int32_t range = ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  order.slice = ctx.arg(3).is_integer() ? ctx.arg(3).as_integer() : 0;
  order.give_up = ctx.count() > 5 && ctx.arg(5).is_integer() ? ctx.arg(5).as_integer() : -1;
  order.lock_destination = true;

  if (!object_form) {
    if (ctx.count() < 2) return HostOutcome::failed(kNoPoint);
    // Clamped to the map and written back into the call's own argument, so
    // every re-entry walks to the same place (0x005d6225 .. 0x005d6289).
    order.dest = self.movement->grid().clamp_to_map(unpack_point(ctx.arg(1)));
    ctx.out(1) = pack_point(order.dest);
    order.range = point_route_range(range);
    order.drop_target = true;
    return run_goto(ctx, *self.world, *self.movement, self.id, order);
  }

  // The object form drops the target on every entry, before anything else
  // (0x005d6c2f).
  if (CombatSystem* combat = combat_system_of(*self.world); combat != nullptr) {
    (void)combat->drop_target(self.id);
  }
  const Value& argument = ctx.arg(1);
  const ObjectId target = object_of(argument);
  if (target == kNoObject && !receiver_departed(*self.world, argument)) {
    return HostOutcome::failed(kNoPoint);
  }
  order.object_form = true;
  const WorldObject* other = target == kNoObject ? nullptr : self.world->find(target);
  if (other != nullptr) {
    order.target = target;
    order.dest = self.world->resolve_position(target);
    // A unit is an object goal (0x0041a5c0), whose band is capped at 920 and
    // measured between the edges; anything else a point goal at its centre
    // with both radii added before `SetDest`'s cap of 999 (0x005d42a6).
    const std::int32_t reach = object_goto_reach(*self.world, self.id, target);
    order.range = other->state.flags.is_unit ? object_route_range(range) + reach
                                             : point_route_range(range + reach);
  }
  return run_goto(ctx, *self.world, *self.movement, self.id, order);
}

// -- the rest --------------------------------------------------------------

/// Whether the script making this call is `id`'s own command script -- the
/// runner at the head of its queue -- which is the test `Face` and `SetPos`
/// make before they end the object's commands: the thread in the object's
/// command slot (0x0069f950 with 0) against the running thread
/// (`[0xa77ea8]`, compared with the thread's `+4`).
[[nodiscard]] bool runs_own_command(const CallContext& ctx, World& world, ObjectId id) {
  if (ctx.script == script::kNoScript) return false;
  const CommandSystem* commands = command_system(world);
  const CommandQueue* queue = commands == nullptr ? nullptr : commands->find(id);
  return queue != nullptr && !queue->entries.empty() && queue->entries.front().script == ctx.script;
}

/// `vtbl+0xc0` with 1 (0x005b1120): every command from the last down to the
/// running one ended through 0x005b07d0, so the queue refills with the
/// class's default verb. `Settlement::IdleAllGates` makes the same call.
void end_commands(World& world, ObjectId id) {
  CommandSystem* commands = command_system(world);
  if (commands == nullptr) return;
  (void)commands->clear_commands(world, id);
  (void)commands->kill_command(world, id);
}

/// `Face(pt)` -- 16 sites, three bodies, one per receiver shape.
///
/// **`Obj::Face` (0x005ac4a0)**: a receiver that names nothing is answered
/// with nothing. Otherwise the object's animation is reset (0x0053d730), it
/// faces the point (0x0053f660: the point less its position, `(0, 1)` when
/// that is zero, written to `[obj+0x48]`), and **unless the call comes from
/// the object's own command script its commands are ended** (`vtbl+0xc0`
/// with 1, `end_commands`): a unit faced from a sequence or from another
/// object's script stops what it was doing. Every shipped site but one faces
/// the script's own unit from inside its command; `CATAPULT_IDLE.VS` faces
/// the crew it has just placed.
///
/// **`ObjList::Face` (0x0055e5d0) and `Query::Face` (0x005797a0)** do the
/// same to every live member and end every member's commands, with no
/// own-script test at all. No shipped site calls either. A query handle that
/// names nothing prints and does nothing; an empty list does nothing.
///
/// **Not reproduced:** the animation reset, as `Disappear` leaves it out --
/// the object model does not drive a unit's sprite from here; and the
/// facing's length. The original keeps the raw difference, this engine a
/// vector of `kFacingLength` in the same direction (`MoveState::facing`),
/// which `GetDir` hands back.
HostOutcome face_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  MovementSystem* movement = movement_system(*world);
  if (movement == nullptr) return HostOutcome::failed(kNoSystem);
  if (ctx.count() < 2) return HostOutcome::failed(kNoPoint);
  const Value& receiver = ctx.arg(0);
  const bool list_form = is_objlist(receiver) ||
                         (receiver.is_object() && receiver.as_object().type == kTypeQuery);
  if (list_form) {
    if (!is_point(ctx.arg(1))) return HostOutcome::failed(kNoPoint);
    for (const ObjectId id : receiver_objects(ctx, *world, receiver)) {
      movement->face(*world, id, unpack_point(ctx.arg(1)));
      end_commands(*world, id);
    }
    return HostOutcome::ok_void();
  }
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_void());
  Point towards{};
  ObjectId ignored = kNoObject;
  if (!destination_of(*self.world, ctx.arg(1), towards, ignored)) {
    return HostOutcome::failed(kNoPoint);
  }
  self.movement->face(*self.world, self.id, towards);
  if (!runs_own_command(ctx, *self.world, self.id)) end_commands(*self.world, self.id);
  return HostOutcome::ok_void();
}

/// `Unit::DoCarryNothing()` -- 3 sites in `SETTLEMENT_BEHAVIOR_AMBIENT.VS`,
/// where the villagers of a settlement are paired up and sent about, and one
/// of the two names that blocked its 210 sites; and `Unit::SetCarryWaterAnim()`
/// and `Unit::SetCarryGoodsAnim()` -- the sole blockers of `UNIT_GRAB_WATER.VS`
/// and `UNIT_GRAB_GOODS.VS`, which put a villager's bucket or bundle on its
/// back before the walk home.
///
/// All three are the walk-animation slot, the 16-bit index at `[unit+0x1bc]`
/// into the entity's animation table (`[entity+0x146c]`, 0x005d2920). **That
/// index is the script's slot less one**: `SetWalkAnim(n)` (0x005d8350) writes
/// `n - 1`, the no-argument form (0x005d8300) writes 0, and `StartAnim(n)`
/// hands 0x0053e6c0 `n - 1` the same way. `MoveState::walk_anim` keeps the
/// script's numbering, so the stored index `i` is slot `i + 1` here:
///
///   * `DoCarryNothing` (0x005d8670, `[7, 1, 21]`) answers whether the index
///     is **zero** -- slot 1 here, exactly. A deer on its grazing slot 13 is
///     carrying something by this rule, and so is a unit given
///     `SetWalkAnim(0)`, whose index is -1. This used to answer "at or below
///     1", which took the second for nothing carried;
///   * the two setters (0x005d83b0, 0x005d84b0, both `[0, 1, 21]`) compare the
///     unit's class **id** -- exact, terminator included -- against the eight
///     `?WVillagerAmbient` classes, one per race letter. A water carrier of
///     one of those gets index 16 (slot **17**); a goods carrier gets 17
///     (slot **18**), and a goods carrier of one of the eight plain
///     `?VillagerAmbient` classes gets 16 (slot 17). These used to be written
///     as slots 16 and 17, one below the animation the original plays. Any
///     other class leaves the slot alone, which is why a peasant grabbing
///     goods walks home empty-handed: the animation only exists on the
///     ambient villagers. Every one of the sixteen names ships.
///
/// A receiver that names nothing prints its own message and does nothing;
/// `DoCarryNothing` answers false for one, the byte the original pushes.
[[nodiscard]] bool class_id_is(const World& world, ObjectId id, std::string_view name) {
  const WorldObject* slot = world.find(id);
  const ClassGraph* graph = world.class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) return false;
  return graph->at(slot->class_index).id == name;
}

[[nodiscard]] bool is_ambient_villager(const World& world, ObjectId id, bool water_carrier) {
  static constexpr char kRaces[] = {'G', 'R', 'M', 'T', 'C', 'I', 'B', 'E'};
  for (const char race : kRaces) {
    std::string name(1, race);
    name += water_carrier ? "WVillagerAmbient" : "VillagerAmbient";
    if (class_id_is(world, id, name)) return true;
  }
  return false;
}

/// The walk slot an index the original stores names here: one more.
constexpr std::int32_t walk_slot_of_index(std::int32_t index) noexcept { return index + 1; }

HostOutcome do_carry_nothing_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(Value::boolean(false)));
  const MoveState* move = self.movement->find(self.id);
  const std::int32_t slot = move == nullptr ? walk_slot_of_index(0) : move->walk_anim;
  return HostOutcome::ok_with(Value::boolean(slot == walk_slot_of_index(0)));
}

template <bool kGoods>
HostOutcome set_carry_anim_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_void());
  if (is_ambient_villager(*self.world, self.id, /*water_carrier=*/true)) {
    self.movement->state(self.id).walk_anim = walk_slot_of_index(kGoods ? 0x11 : 0x10);
  } else if (kGoods && is_ambient_villager(*self.world, self.id, /*water_carrier=*/false)) {
    self.movement->state(self.id).walk_anim = walk_slot_of_index(0x10);
  }
  return HostOutcome::ok_void();
}

/// `SetSpeedFactor(n)` (0x005d7c30): the handle check, then `n` written whole
/// to `[unit+0x15c]`; nothing for a receiver that names nothing. **Engine
/// guard:** a negative factor is stored as 0 here, where the original would
/// walk the unit backwards along its route. The shipped values are 7, 150
/// and 170.
HostOutcome set_speed_factor_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_void());
  const std::int32_t factor = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  self.movement->state(self.id).speed_factor = factor < 0 ? 0 : factor;
  return HostOutcome::ok_void();
}

/// `SetWalkAnim()` and `SetWalkAnim(n)`: the walk index to 0, or to `n - 1`
/// -- slot 1, or slot `n`, in this engine's numbering (see `DoCarryNothing`
/// above). Nothing for a receiver that names nothing.
HostOutcome set_walk_anim_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_void());
  const std::int32_t slot = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer()
                                                                      : walk_slot_of_index(0);
  self.movement->state(self.id).walk_anim = slot;
  return HostOutcome::ok_void();
}

/// `Stop(ms)`: come to a stop, then report whether the unit has. See the note on `register_movement_host` for why this is a
/// suspending predicate and not the void procedure it first looked like: 34 of
/// its 53 sites are `while (!.Stop(1000));`, which a void `Stop` spins forever.
///
/// The suspension is a walking unit's only: a standing one is answered at once
/// (below), and the idle loops are paced by the `Idle` that follows.
///
/// **A unit with a route is read off 0x005d6c90, and it does not stop where it
/// stands.** The first entry, for a unit with a path object (`[unit+0x148]`)
/// that is neither held (`[unit+0x154]`) nor at `(-1, -1)`, calls the path's
/// slot 6 -- `CVXPathRetry` 0x004178b0, which sets flag bit 1 at
/// `[retry+0x20]` and passes the call down to its media -- sets the marching
/// activity, writes `ms` to the wait slot and suspends with the route in hand.
/// The path follower (0x00419ee0) then reads that bit before every step: when
/// the retry names an owner (`0x004178d0`, `MovementSystem::lock_owner`) it
/// stops on the first point that is passable and a free spot (0x0040a990) and
/// otherwise walks on; when it names none it takes no step. The re-entry asks
/// for the next step (`vtbl+8`): one left is a unit still walking, and the
/// answer is **false**, so `while (!.Stop(1000));` asks again; none, and the
/// route is deleted and the answer is true. So a column told to stop by
/// `UNIT_STAND_POSITION.VS`, or a unit that reached its hero in
/// `UNIT_ATTACH.VS` and stops in the middle of its `Goto`, does not freeze on
/// top of the bodies it was walking through: it walks on to the first spot
/// nobody stands on. Before this every such stop dropped the route at once,
/// and on Crossroads a hero's recruits that left one door together stood in
/// one pile at it for the rest of the match.
///
/// **A unit with no route stops at once and the call does not suspend**
/// (0x005d6d9b): the route slot is cleared (0x005d3830 with 0), the idle
/// state set, and the answer is true with `eax = 0`, this protocol's "done".
/// So is a held unit, and one at `(-1, -1)`, route or not. `ms` is never
/// waited here: it is written to the wait cell only on the walking path.
/// This used to hold still for `ms` whatever the unit was doing, and
/// `UNIT_IDLE.VS`'s pacing looked as if it rested on that. It does not: every
/// loop in the corpus that stops a standing unit suspends on something else
/// in the same pass -- `UNIT_IDLE.VS`, `HERO_IDLE.VS`, `GHOST_IDLE.VS`,
/// `PEACEFUL_IDLE.VS` and `UNIT_STAY_HIDDEN.VS` follow a true `Stop` with
/// `Idle(ms)`, `SHIP_IDLE.VS` with `Idle()`, `SENTRY_GUARD.VS` with a `Sleep`,
/// and the 34 `while (!.Stop(1000));` sites leave on the first true answer.
/// What changes is the waits that were only `Stop`'s: `UNIT_ENTER.VS` and
/// `HERO_ENTER.VS` end a refused entry with `.Stop(10000)`, which was ten
/// seconds of standing about, still flagged as entering, before the next
/// command; the original moves on in the same pass.
///
/// **The first entry drops the combat target**, before it looks at the route
/// (0x005d6cf7): the target setter's write (`CombatSystem::drop_target`). The
/// re-entry does not.
HostOutcome stop_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  // A receiver that names nothing: false, at once (0x005d6cd0).
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(Value::boolean(false)));
  const std::int64_t hold = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer()
                                                                      : 0;
  if (!ctx.first_call) {
    // The re-entry (0x005d6df0) asks the route for its next step: one still
    // there is a unit still walking to where it may stop, and the answer is
    // false with the route kept; none, and the route is deleted and the
    // answer is true.
    const MoveState* move = self.movement->find(self.id);
    if (move != nullptr && move->has_path) return HostOutcome::ok_with(Value::boolean(false));
    self.movement->stop(*self.world, self.id);
    return HostOutcome::ok_with(Value::boolean(true));
  }
  if (CombatSystem* combat = combat_system_of(*self.world); combat != nullptr) {
    (void)combat->drop_target(self.id);
  }
  // A unit with a route, on the map (0x005d6d16 .. 0x005d6d48), is asked to
  // stop and the call suspends for `ms` with the route in hand; the answer
  // comes at the re-entry.
  if (const ObjectState* state = self.world->state(self.id);
      state != nullptr && !state->is_held() && state->position != kHeldPosition &&
      self.movement->request_stop(*self.world, self.id)) {
    HostOutcome out;
    out.status = script::HostStatus::retry;
    out.suspend_for = hold > 0 ? hold : 0;
    return out;
  }
  // Anything else stops where it is and is answered at once (0x005d6d9b).
  self.movement->stop(*self.world, self.id);
  return HostOutcome::ok_with(Value::boolean(true));
}

/// `HasPath()` (0x005d6e50): false for a receiver that names nothing and for a
/// unit in a holder (`[unit+0x154]`); for a hero (bit 0x1000000 of
/// `[obj+0x2c]`) 0x0052fe40 -- its own route record, or **any member of its
/// army whose route follows the formation** (record mode `[rec+0x28] == 2`,
/// the route `SetFormation` 0x00417790 aims at the formation object); for any
/// other unit whether it has a route record at all (`[unit+0x148]`).
///
/// A member following the march is one whose route belongs to the hero's
/// party here (`MoveState::party`, which `aim_at_formation` sets). So a hero who has
/// reached the march's end goes on answering true while his column is still
/// walking to its stations -- `HERO_TELEPORT.VS` and the hero scripts loop
/// `while (.HasPath)` around `FormKeepMoving`. This used to read the hero's
/// own route alone.
HostOutcome has_path_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(Value::boolean(false)));
  const auto answer = [](bool has) { return HostOutcome::ok_with(Value::boolean(has)); };
  const ObjectState* state = self.world->state(self.id);
  if (state != nullptr && state->is_held()) return answer(false);
  const MoveState* move = self.movement->find(self.id);
  if (move != nullptr && move->has_path) return answer(true);
  if (state == nullptr || !state->flags.is_hero) return answer(false);
  const HeroSystem* heroes = hero_system_of(*self.world);
  const HeroRecord* record = heroes == nullptr ? nullptr : heroes->hero(self.id);
  if (record == nullptr) return answer(false);
  for (const ObjectId member : record->army) {
    const MoveState* marching = self.movement->find(member);
    if (marching != nullptr && marching->has_path && marching->form_aimed &&
        marching->party == self.id) {
      return answer(true);
    }
  }
  return answer(false);
}

/// `PathTo`'s goal points: the sixteen at exactly `range` round `dest`
/// (0x00409cf0), in ring order, each laid only where the grid lets a unit
/// stand (0x00409b80); `dest` alone for a `range` of exactly 0 -- the ring
/// builder stops after its first point only then, so a negative range lays
/// the same sixteen points with the ring started from the opposite direction
/// (`range * dir` turned round). Where the search heads
/// is the one nearest `dest` (the first on a tie, 0x00409c9e), or `dest` when
/// none is laid (0x0040a34b). `PathTo` names no owner, so none is struck off.
Point path_to_goals(const ObstructionGrid& grid, Point dest, std::int32_t range,
                    std::vector<GoalPoint>& out) {
  out.clear();
  const auto lay = [&](Point p) {
    const std::int32_t cx = ObstructionGrid::cell_of(p.x);
    const std::int32_t cy = ObstructionGrid::cell_of(p.y);
    if (grid.in_bounds(cx, cy) && !grid.blocked_cell(cx, cy)) out.push_back(GoalPoint{p, true});
  };
  if (range == 0) {
    lay(dest);
  } else {
    for (const auto& dir : kRingDirections) {
      lay(Point{truncate_fixed((static_cast<std::int64_t>(dest.x) << kRingShift) +
                               static_cast<std::int64_t>(range) * dir[0]),
                truncate_fixed((static_cast<std::int64_t>(dest.y) << kRingShift) +
                               static_cast<std::int64_t>(range) * dir[1])});
    }
  }
  Point heading = dest;
  std::int64_t inner = -1;
  for (const GoalPoint& p : out) {
    const std::int64_t gap = dist_sq(p.at, dest);
    if (inner < 0 || gap < inner) {
      inner = gap;
      heading = p.at;
    }
  }
  return heading;
}

/// Where `PathTo` searches from: the unit's position (`vtbl+0x3c`), unless
/// that is `(-1, -1)` -- a unit in a holder -- when it is the door of its
/// settlement's central building it would leave by towards `dest`
/// (0x005de2b1 .. 0x005de30b: the holder's settlement, `[holder+0xc]`, its
/// central building, `[settlement+0x8c]`, and that building's exit point,
/// 0x004db6c0, for the unit class's `water_unit`, `[class+0xb30]`). The exit
/// point takes the door picker's one draw (0x0056fa40,
/// `pick_door_by_distance`) when the building has doors and is the building's
/// own position when it has none. A holder that is no settlement's is
/// measured from where the unit resolves (a ship's passenger answered
/// 1,000,000 before this is asked).
Point path_to_start(World& world, const ObstructionGrid& grid, ObjectId id, Point dest) {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->state.position != kHeldPosition || !slot->state.is_held()) {
    return world.resolve_position(id);
  }
  const EconomySystem* economy = economy_of(world);
  const Settlement* s =
      economy == nullptr ? nullptr : economy->settlements().for_object(slot->state.holder);
  if (s == nullptr || s->holder.object != slot->state.holder || s->anchor == kNoObject ||
      world.find(s->anchor) == nullptr) {
    return world.resolve_position(id);
  }
  std::vector<Point> doors;
  building_doors(world, &grid, s->anchor, kExitPointType,
                 class_flag(world, *slot, "water_unit"), doors);
  if (doors.empty()) return world.resolve_position(s->anchor);
  return pick_door_by_distance(doors, dest, world.rng());
}

/// `PathTo(dest, range, ?)` -> the route's length in world units, or -1 when
/// there is no route. Two bodies, one per destination type:
///
///   * **the receiver**: one that names nothing answers -1 (0x005de360);
///   * **the object form** (0x005de390) answers -1 for a target that names
///     nothing (`Parameter #1 ... is uninitialized or invalid object`), and
///     otherwise goes to where the target stands for the questions that ask
///     where it is -- its `vtbl+0xc8`, `posRH`, so a garrisoned target is its
///     settlement's centre -- with **both class radii added to the range**
///     (`[class+0x2dc]` of each, as they stand: no clamp). Every shipped
///     object `PathTo` is a hero's `PathTo(x, .FormRadius, 0)`. This used to
///     search to the target's raw position with the range as given;
///   * **a unit aboard a ship answers 1,000,000** (0x005de1e1): its holder
///     belongs to an object (0x005319a0), and the search is not made;
///   * the third argument is popped and never read.
///
/// **The search** (docs/formats/pathfinder.md, "`PathTo`"). The original lays
/// the goal points as one ring of sixteen at exactly `range` round the goal
/// (0x00409cf0, the directions `ring_goal` uses; a `range` of 0 is the goal
/// alone), each a goal only where the grid lets a unit stand (0x00409b80),
/// asks the live pathfinder (`[0x8c528c]`, `vtbl+8`) with the unit's class
/// radius and its class's `+0x31c`, and answers the path record's `+0x34`
/// shifted down sixteen bits (0x005de354; the record is at `esp+0x20`, so
/// `esp+0x54` is `+0x34`): **the route's length**, the sum of its legs'
/// integer square roots (0x0041a8f0), **complete or not** -- a partial
/// route's length, with no 32,000. A search that answers 0, which is one
/// whose route has no length, answers -1 (0x005de34c). So a unit already
/// inside the band is answered the walk out to the ring point nearest it, and
/// one standing on that point -1. Here the ring point nearest the unit in a
/// straight line is the goal -- the inference `ring_goal` makes for the
/// first goal cell the search reaches -- or the goal itself when no point is
/// passable, and this engine's search runs to it.
///
/// A unit at `(-1, -1)` searches from the door it would leave its settlement
/// by (`path_to_start`). This used to answer 0 inside the band and -1 for a
/// partial route, and measured a garrisoned unit from its settlement's centre.
HostOutcome path_to_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(Value::integer(-1)));
  const auto none = HostOutcome::ok_with(Value::integer(-1));
  std::int32_t range = ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  Point dest{};
  if (is_point(ctx.arg(1))) {
    dest = unpack_point(ctx.arg(1));
  } else {
    const ObjectId target = object_of(ctx.arg(1));
    const WorldObject* other = target == kNoObject ? nullptr : self.world->find(target);
    if (other == nullptr) {
      if (target != kNoObject || receiver_departed(*self.world, ctx.arg(1))) return none;
      return HostOutcome::failed(kNoPoint);
    }
    dest = other->state.flags.is_unit ? unit_pos_rh(*self.world, target) : other->state.position;
    const WorldObject* mover = self.world->find(self.id);
    range += class_int(*self.world, *other, "radius") +
             (mover == nullptr ? 0 : class_int(*self.world, *mover, "radius"));
  }
  if (held_by_carrier(*self.world, self.id)) return HostOutcome::ok_with(Value::integer(1000000));
  PathRequest request;
  request.start = path_to_start(*self.world, self.movement->grid(), self.id, dest);
  std::vector<GoalPoint> goals;
  request.goal = path_to_goals(self.movement->grid(), dest, range, goals);
  request.goal_points = goals;
  request.goal_points_given = true;
  request.ignore_passability = self.movement->ignores_passability(*self.world, self.id);
  const Path path = self.movement->pathfinder().find(self.movement->grid(), request);
  if (!path.usable() || path.length <= 0) return none;
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(std::min<std::int64_t>(path.length, 0x7FFFFFFF))));
}

/// `PathDestFound` (0x005d8830) -> **the point the unit's route ends at, or
/// `(-1, -1)` when it has none.** The route record (`[unit+0x148]`) is asked
/// whether its media found a path (`vtbl+0x24`, which the route's sampled
/// path answers with itself) and, if so, for the media's last point
/// (`vtbl+0x10`, the end of the path's point vector, 0x0040bb80); no record,
/// or none found, is `(-1, -1)`. `SHIP_BOARD.VS`, the one caller, tests
/// `pos.x != -1` on it before it measures anything. This used to answer the
/// unit's own position with no route.
///
/// **The body makes no handle check**: a receiver that names nothing reads
/// the route field of a null object and faults. Refused by name here.
HostOutcome path_dest_found_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) {
    return unresolved(self, HostOutcome::failed(
                                "PathDestFound on a handle that names nothing faults in gbr.exe; "
                                "see sim/movement.cpp"));
  }
  const MoveState* move = self.movement->find(self.id);
  if (move == nullptr || !move->has_path || move->waypoints.empty()) {
    return HostOutcome::ok_with(pack_point(kHeldPosition));
  }
  return HostOutcome::ok_with(pack_point(move->waypoints.back()));
}

/// `dest` (0x005d6f50): `(-1, -1)` for a receiver that names nothing. A unit
/// with a route record answers the record's `vtbl+0x10` (0x00418560): the
/// end of the route its media found, or -- with no media -- where the unit
/// stands. A unit without one answers its own `vtbl+0xc8`, which is `posRH`:
/// its position, or for a unit in a holder its settlement's centre or the
/// ship it is aboard (`unit_pos_rh`).
///
/// So `dest` is **where the route goes, not where the order pointed**: a
/// partial route's end, short of the goal. This used to answer the ordered
/// destination with a route, and the holder walk without one. The priests'
/// and druids' `.hero.dest` is the reader: they keep to where their hero's
/// route will leave him.
HostOutcome dest_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(pack_point(kHeldPosition)));
  const MoveState* move = self.movement->find(self.id);
  if (move != nullptr && move->has_path && !move->waypoints.empty()) {
    return HostOutcome::ok_with(pack_point(move->waypoints.back()));
  }
  const WorldObject* slot = self.world->find(self.id);
  const Point here = slot != nullptr && slot->state.flags.is_unit
                         ? unit_pos_rh(*self.world, self.id)
                         : self.world->resolve_position(self.id);
  return HostOutcome::ok_with(pack_point(here));
}

/// `Unit::speed` -- **the class's `speed`, whether or not the unit has ever
/// walked.**
///
/// `0x005d8ad0` reads the receiver's class descriptor at `[obj+0x3c]` and
/// returns its `+0x2c8`, which is where the class reader stores the `speed`
/// property (`0x005a1b25`..`0x005a1b6e`). No movement record is consulted and
/// no `SetSpeedFactor` is applied: the answer is the declared number. A
/// receiver that names nothing answers 0.
///
/// This used to answer from the `MoveState`, and 0 when there was none. A unit
/// gets one the first time it is ordered to walk, and **an eagle never is**: it
/// flies by `Flying::PlayAnim` and nothing else. So `EAGLE_MOVE.VS`'s
/// `GetVecByDir(dir, .speed / 2)` and `speed = .speed` were zero-length steps,
/// every `PlayAnim` landed the bird where it already was, and every eagle on
/// every map stayed on its start point forever (playtest #18).
///
/// A class that declares no speed anywhere up its chain answers 0. An object
/// with no class at all -- a test bench's -- falls back to its movement
/// record's `speed`, which is that same property copied in, or set directly.
HostOutcome speed_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(Value::integer(0)));
  const WorldObject* slot = self.world->find(self.id);
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  const ClassGraph* classes = self.movement->class_graph();
  if (classes == nullptr) classes = self.world->class_graph();
  if (classes != nullptr && slot->class_index != kNoClass) {
    std::int32_t value = 0;
    if (!parse_int(classes->property(slot->class_index, "speed"), value)) value = 0;
    return HostOutcome::ok_with(Value::integer(value));
  }
  const MoveState* move = self.movement->find(self.id);
  return HostOutcome::ok_with(Value::integer(move != nullptr ? move->speed : 0));
}

/// `ClipDestToMap(pt)` (0x005ad900): the point clamped into the map rectangle
/// `[map+0xb4]..[map+0xc0]`, which is `(0, 0, w - 1, h - 1)`. The body never
/// looks at its receiver: a handle that names nothing clips all the same.
HostOutcome clip_dest_to_map_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  MovementSystem* movement = movement_system(*world);
  if (movement == nullptr) return HostOutcome::failed(kNoSystem);
  if (ctx.count() < 2 || !is_point(ctx.arg(1))) return HostOutcome::failed(kNoPoint);
  return HostOutcome::ok_with(pack_point(movement->grid().clamp_to_map(unpack_point(ctx.arg(1)))));
}

/// `IsPassable3x3(pt)` (0x004c67d0): 0x0053eb60 on the point, true or false.
/// `ObstructionGrid::passable_3x3` carries the footprint.
HostOutcome is_passable_3x3_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  MovementSystem* movement = movement_system(*world);
  if (movement == nullptr) return HostOutcome::failed(kNoSystem);
  if (ctx.count() == 0 || !is_point(ctx.arg(0))) return HostOutcome::failed(kNoPoint);
  return HostOutcome::ok_with(
      Value::boolean(movement->grid().passable_3x3(unpack_point(ctx.arg(0)))));
}

/// `TimeWithoutWalking` (0x005d6ee0) -- **how long the `Goto` family has been
/// failing to lay this unit a route**, and 0 while it is not: the clock
/// (`[[unit+0x18]]`) less the failure stamp `[unit+0x150]`, 0 when the stamp
/// is clear (-1). That stamp is `MoveState::goto_failed_at`, which the `Goto`
/// family sets at its first search that lays no route and clears when one is
/// laid or the unit arrives, and the end of the unit's command clears
/// (`GotoOrder::give_up`). A receiver that names nothing answers 0.
///
/// It used to answer how long the unit had stood still, which is a different
/// question with a different answer exactly where the scripts ask it.
/// `UNIT_PATROL.VS` walks `if (.Goto(pt, 0, 2000, true, -1)) ...; if
/// (.TimeWithoutWalking() > 0) break;` -- a patrol leaves its leg when the
/// route cannot be laid, not when a slice ends with the unit between steps;
/// the priests' and druids' `> 2500` give up on a walk that has found no
/// route for two and a half seconds, not on one held up in a crowd; and
/// `SHIP_IDLE.VS` leaves its `FindPointToStay` walk on the first failure.
///
/// Read on the scripts' clock, which is the one the stamp is taken on.
HostOutcome time_without_walking_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(Value::integer(0)));
  const MoveState* move = self.movement->find(self.id);
  if (move == nullptr || move->goto_failed_at == kNoGotoFailure) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  const GameTime since = now_of(ctx, *self.world) - move->goto_failed_at;
  return HostOutcome::ok_with(Value::integer(
      static_cast<std::int32_t>(std::clamp<GameTime>(since, 0, 0x7FFFFFFF))));
}

/// `GetDir` (0x005ac5e0): the facing at `[obj+0x48]`, `(0, 1)` for a receiver
/// that names nothing. The facing's length is this engine's
/// (`kFacingLength`, see `Face`).
HostOutcome get_dir_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(pack_point(Point{0, 1})));
  return HostOutcome::ok_with(pack_point(self.movement->state(self.id).facing));
}

/// `SetPos(pt)` (0x005ac130) -- 32 sites: nothing for a receiver that names
/// nothing; otherwise, in order:
///
///   * the animation reset (0x0053d730), not reproduced, as in `Face`;
///   * the unit's own `SetPos` (`vtbl+0x38`, 0x005d39e0): **a unit in a holder
///     is taken out of it first** when the point is not `(-1, -1)` -- the
///     holder removal 0x00531a80, the routine `ExitHolder` ends in, off the
///     settlement's roster too (`garrison_forget`) -- and then placed. This
///     used to leave a held unit where it was, because the world refuses a
///     position under a holder: `UNIT_TELEPORT.VS` and `HERO_TELEPORT.VS`
///     enter the teleport's settlement and then `SetPos` to the far exit;
///   * the activity cleared and, for a unit, its route dropped (0x005d3830
///     with null);
///   * **unless the call comes from the object's own command script, its
///     commands are ended** (`vtbl+0xc0` with 1), as `Face` does: the
///     campaign sequences that move their actors about stop what those actors
///     were doing, and the teleport scripts, which move their own unit, do
///     not.
///
/// **Not reproduced:** the point is first clamped into the camera's map
/// rectangle (`[0x9edd18]+0x628..0x634`, 32 units in from each side) in
/// screen space -- `y` scaled by 181/256 less the terrain's height at the
/// point (0x006245b0, 0x00422b70, 0x006243b0) -- and brought back to the map
/// when it was outside. Every shipped site passes a point on the map: an area
/// centre, an exit point, the unit's own position.
HostOutcome set_pos_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_void());
  if (!is_point(ctx.arg(1))) return HostOutcome::failed(kNoPoint);
  const Point to = unpack_point(ctx.arg(1));
  // Teleporting invalidates any route: it was laid from somewhere else.
  self.movement->stop(*self.world, self.id);
  const ObjectState* state = self.world->state(self.id);
  if (state != nullptr && state->is_held()) {
    if (to != kHeldPosition) {
      (void)self.world->remove_from_holder(self.id, to);
      (void)garrison_forget(*self.world, self.id);
    }
  } else {
    (void)self.world->set_position(self.id, to);
  }
  if (!runs_own_command(ctx, *self.world, self.id)) end_commands(*self.world, self.id);
  return HostOutcome::ok_void();
}

/// `Hero::SetFormation(name)` (0x0052eca0): nothing for a receiver that names
/// nothing; **a name `FORMATIONS.XML` does not declare is printed and
/// ignored** ("unknown formation name, check the spelling", into the dead
/// printer) -- the hero keeps the formation it had and the script goes on.
/// Otherwise the hero's formation object (`[hero+0x2b0]`) takes the shape
/// (0x005f2200: `[form+0x18]`, and the layout marked for a rerun). This used
/// to trap on an unknown name.
HostOutcome set_formation_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_void());
  if (!ctx.arg(1).is_string()) return HostOutcome::failed("SetFormation: not a formation name");
  const std::string& name = ctx.arg(1).as_string();
  if (self.movement->formations().find(name) == nullptr) return HostOutcome::ok_void();
  Formation& form = self.movement->formation_of(self.id);
  form.shape = name;
  form.rebuild = true;
  return HostOutcome::ok_void();
}

/// `Hero::formation` (0x0052ed60): the name of the hero's formation shape
/// (`[[hero+0x2b0]+0x18]`'s name), the empty string for a receiver that names
/// nothing. A hero never given one has the file's default.
HostOutcome formation_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(Value::string(std::string())));
  const Formation* form = self.movement->formation(self.id);
  const std::string_view name = form != nullptr && !form->shape.empty()
                                    ? std::string_view(form->shape)
                                    : self.movement->formations().default_name();
  return HostOutcome::ok_with(Value::string(std::string(name)));
}

/// `Hero::FormRadius` (0x0052ec40) -- 15 sites, every one a hero sizing its
/// army: `PathTo(x, .FormRadius, 0)`, `GotoAttack`'s reach. 0 for a receiver
/// that names nothing; otherwise 0x005f2350 on the hero's formation object:
/// **the formation's radius** (`[form+0x34]`, the farthest slot from the
/// centre the layout last laid) when the formation has a slot (`[form+0x14]`)
/// and that radius is not 0, and **the hero's class `radius`**
/// (`[class+0x2dc]`) otherwise (`formation_reach`).
///
/// The layout runs only when a sample is placed or `FormKeepMoving` updates
/// a march (docs/formats/formation.md), so a hero whose army changed since
/// answers the radius of the last layout, and a unit that joined since is not
/// a slot: a hero who has never marched answers his class radius whatever
/// his army.
HostOutcome form_radius_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return unresolved(self, HostOutcome::ok_with(Value::integer(0)));
  const WorldObject* slot = self.world->find(self.id);
  const std::int32_t own = slot == nullptr ? 0 : class_int(*self.world, *slot, "radius");
  return HostOutcome::ok_with(
      Value::integer(formation_reach(self.movement->formation(self.id), own)));
}

// --------------------------------------------------------------------------
// FormDescription
// --------------------------------------------------------------------------
//
// `FormDescription(name)` (0x005f6320) is the rollover text under a formation
// button: the two `HERO_FORM_*.VS` scripts append it to the reason text after
// the formation's translated name. It is a string built in a fixed order from
// the formation's bonus fields, and the shape of it is worth stating in full
// because two of its details are quirks a tidy rewrite would lose.
//
//   1. The heading is `Translate("Army bonus during stand ground")`, then a
//      newline. The formation's own `Description` attribute is not used.
//   2. Then one group per bonus that is set, each `<icon tag>`, a `+` if the
//      value is positive, the value in decimal, and a trailing space:
//        level    when `BonusLevel != 0`
//        damage   when `BonusDamage != 0`
//        defence  when `BonusDefencePierce != 0 || BonusDefenceSlash != 0`,
//                 and the number printed is **the slash value** either way --
//                 a pierce-only formation reads `defence 0`. No shipped
//                 formation is pierce-only, so the quirk is invisible in the
//                 game and reproduced here rather than corrected.
//        range    when `BonusRange != 0`, tagged with the *piercing* icon and
//                 followed by `% ` instead of a space: the range bonus is a
//                 percentage and the icon set has no range glyph.
//   3. A name no formation declares yields the empty string, not an error, and
//      not the heading: the original returns before building anything.
//
// A negative bonus prints with its own sign, so the `+` test is `> 0`, not
// `!= 0`. Nothing in the shipped file is negative.
//
// The icon tags are `<imagetransp path>` markup the rich-text renderer
// resolves; they are quoted here as the four literals the executable carries
// so the output byte-matches. The heading goes through the translation table
// on the context exactly as `Translate` would, and falls back to the key when
// no language pack is loaded.
//
// The original's lookup is a case-sensitive map find. `FormationTable::find`
// is case-insensitive, for `SetFormation`'s sake, and the difference cannot
// be observed from either shipped call site, which pass `cmdparam` after
// comparing it exactly against the same literals.

constexpr std::string_view kStandGroundHeading = "Army bonus during stand ground";
constexpr std::string_view kLevelIcon = "<imagetransp gameres/infobar/common/level_ico.bmp>";
constexpr std::string_view kDamageIcon = "<imagetransp gameres/infobar/common/atack ico.bmp>";
constexpr std::string_view kDefenceIcon = "<imagetransp gameres/infobar/common/defense ico.bmp>";
constexpr std::string_view kRangeIcon = "<imagetransp gameres/infobar/common/piercing ico.bmp>";

/// One `<icon>[+]value<tail>` group appended to `out`.
void append_bonus(std::string& out, std::string_view icon, std::int32_t value,
                  std::string_view tail) {
  out += icon;
  if (value > 0) out += '+';
  out += std::to_string(value);
  out += tail;
}

HostOutcome form_description_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  MovementSystem* movement = movement_system(*world);
  if (movement == nullptr) return HostOutcome::failed(kNoSystem);
  if (ctx.count() == 0 || !ctx.arg(0).is_string()) {
    return HostOutcome::failed("FormDescription: not a formation name");
  }
  const FormationClassDef* form = movement->formations().find(ctx.arg(0).as_string());
  if (form == nullptr) return HostOutcome::ok_with(Value::string(std::string()));

  const HostContext* context = host_context_of(ctx);
  const game::TranslationTable* table = context == nullptr ? nullptr : context->translations;
  std::string text(table == nullptr ? kStandGroundHeading : table->translate(kStandGroundHeading));
  text += '\n';
  if (form->bonus_level != 0) append_bonus(text, kLevelIcon, form->bonus_level, " ");
  if (form->bonus_damage != 0) append_bonus(text, kDamageIcon, form->bonus_damage, " ");
  if (form->bonus_defence_pierce != 0 || form->bonus_defence_slash != 0) {
    append_bonus(text, kDefenceIcon, form->bonus_defence_slash, " ");
  }
  if (form->bonus_range != 0) append_bonus(text, kRangeIcon, form->bonus_range, "% ");
  return HostOutcome::ok_with(Value::string(std::move(text)));
}

}  // namespace

// --------------------------------------------------------------------------
// the Goto family
// --------------------------------------------------------------------------

namespace {
/// What a ship's passenger waits before it asks again whether it may step
/// off: the 100 the `Goto` family writes to the wait cell (0x005d62c8).
constexpr std::int32_t kGotoCarrierPoll = 100;
/// What a unit standing nowhere waits before it asks again (0x005d6312).
constexpr std::int32_t kGotoOffMapPoll = 500;
}  // namespace

bool goto_may_approach(const World& world, ObjectId mover, ObjectId target) {
  const WorldObject* other = world.find(target);
  if (other == nullptr) return false;
  if (!other->state.flags.is_unit) return true;
  if (other->state.is_held()) return false;
  if (!other->state.flags.hidden) return true;
  const WorldObject* self = world.find(mover);
  return self != nullptr && other->state.owner == self->state.owner;
}

namespace {

/// The first run's wait: the call suspends and runs again whole -- its
/// re-entry -- with its word of state saying whether this run laid a route.
[[nodiscard]] HostOutcome goto_wait(CallContext& ctx, std::int64_t wait, bool laid) {
  if (ctx.scratch != nullptr) *ctx.scratch = laid ? 1 : 0;
  HostOutcome out;
  out.status = script::HostStatus::retry;
  out.suspend_for = wait > 0 ? wait : 0;
  return out;
}

}  // namespace

/// **The `Goto` family answers after its wait, at its re-entry** -- the
/// protocol of 0x005d61b0 (`Goto` to a point), 0x005d6620 (`GotoEnter`) and
/// 0x005d40c0 (the worker under `Goto` to an object and `GotoAttack`), all
/// registered through the suspending registrar, whose calls carry a
/// first-run byte and a word of their own (`CallContext::first_call`,
/// `scratch`):
///
///   1. A held unit steps out first; every wait on the way runs the call
///      again whole, and the run that steps it out counts as a first run.
///      A unit at `(-1, -1)` waits 500 ms and asks again.
///   2. **A re-entry** -- not the first run, not just stepped out -- **of a
///      unit that holds an order** (the route record `[unit+0x148]`, here
///      `goto_active`) answers at once and does nothing else: true when its
///      first run laid a route (the word) and the unit is now arrived
///      (`IsArrived`, 0x00417c10, `MovementSystem::is_arrived`), and then the
///      order is dropped (0x005d3830 with 0); false otherwise, the order kept,
///      so the script's loop calls again. A re-entry that finds no order --
///      something dropped it during the wait -- runs as a first run.
///   3. **A first run** drops the combat target (the point forms), measures,
///      routes and answers true at once when already arrived; otherwise it
///      lays the route, notes in the word whether it did, and **suspends for
///      the whole slice** -- the longer of the slice and the look-around
///      animation in the original (0x005d65e8), which is not reproduced --
///      however soon the unit will arrive. A first run that lays no route
///      stamps the failure and may end the script (`GotoOrder::give_up`).
///
/// This used to suspend for the lesser of the slice and the time to arrival
/// and answer false after it, leaving the loop's next call to find the unit
/// arrived. So a unit's script went on the moment it arrived instead of when
/// the slice ran out, and every arrival cost one more call -- another target
/// drop, another failure draw. The object forms' unapproachable target
/// (`goto_may_approach`) used to answer false after one slice; it lays no
/// route record, so the original's re-entry finds none and runs again: it
/// waits, slice after slice, until the target may be approached or the unit
/// already held an order to answer on.
HostOutcome run_goto(CallContext& ctx, World& world, MovementSystem& movement, ObjectId id,
                     const GotoOrder& order) {
  // **A held unit steps out of its holder first** (0x005d62a0-0x005d62ec),
  // before it measures or routes anything, and every wait on the way is the
  // original's return 1 -- suspend, and run the call again whole
  // (0x005d6606), which is `retry` here:
  //
  //   * **aboard a ship it polls every 100 ms** (0x005d62b9): when the
  //     holder belongs to an object (0x005319a0 resolves `[holder+0xe]`,
  //     `held_by_carrier`), the call writes 100 to the wait cell and does not
  //     try the exit at all, so a passenger stays aboard until the ship lets
  //     it off. `GotoEnter` (0x005d6729) and the worker `GotoAttack` and the
  //     object `Goto` share (0x005d4103) make the same poll;
  //   * otherwise it asks 0x005d3f20 to put it outside, towards where it is
  //     going, and waits whatever the settlement's exit timing asks
  //     (`garrison_exit`).
  //
  // **A unit at `(-1, -1)` that no holder holds waits 500 ms and asks
  // again** (0x005d6312; 0x005d4150 in the worker the object `Goto` and
  // `GotoAttack` share, 0x005d6729 area in `GotoEnter`): what `Disappear`
  // leaves (`m_disappear`), until something puts it back on the map.
  // **Not reproduced** for a unit still held after the step-out: the
  // original's exit routine takes a unit out of any holder, `garrison_exit`
  // only out of a settlement's, so a unit this engine fails to take out is
  // one the original would have, and a 500 ms poll here would hold it for
  // ever.
  const Point dest =
      order.target == kNoObject ? order.dest : world.resolve_position(order.target);
  bool stepped_out = false;
  if (const ObjectState* state = world.state(id); state != nullptr && state->is_held()) {
    const std::int32_t wait = held_by_carrier(world, id)
                                  ? kGotoCarrierPoll
                                  : garrison_exit(world, id, dest, now_of(ctx, world));
    if (wait > 0) {
      HostOutcome out;
      out.status = script::HostStatus::retry;
      out.suspend_for = wait;
      return out;
    }
    stepped_out = true;
  } else if (state != nullptr && state->position == kHeldPosition) {
    HostOutcome out;
    out.status = script::HostStatus::retry;
    out.suspend_for = kGotoOffMapPoll;
    return out;
  }

  MoveState& move = movement.state(id);
  // The re-entry (0x005d6340, 0x005d6779, 0x005d41b9).
  if (!ctx.first_call && !stepped_out && move.goto_active) {
    const bool laid = ctx.scratch != nullptr && *ctx.scratch != 0;
    const bool arrived = laid && movement.is_arrived(world, id);
    if (arrived) movement.stop(world, id);
    return HostOutcome::ok_with(Value::boolean(arrived));
  }

  if (order.drop_target) {
    if (CombatSystem* combat = combat_system_of(world); combat != nullptr) {
      (void)combat->drop_target(id);
    }
  }
  // The object forms' target (0x005d4270 ..): the mover itself is arrived
  // at once; a target it may not approach, or one naming nothing, lays no
  // route, stamps no failure and waits the slice.
  if (order.object_form) {
    if (order.target == id) return HostOutcome::ok_with(Value::boolean(true));
    if (order.target == kNoObject || !goto_may_approach(world, id, order.target)) {
      return goto_wait(ctx, order.slice, /*laid=*/false);
    }
  }

  const GameTime now = now_of(ctx, world);
  // **A unit that has been failing searches less often** (0x005d2eb0): with
  // the stamp set, every call draws `rand(0, 2n)`, `n` the whole 2,048 ms
  // spans since the stamp, at most 10, and anything but 0 tells `SetDest` not
  // to search the goal it already has (0x0041a513). The draw is taken whether
  // or not the goal has changed -- and before `SetDest` finds the unit arrived --
  // and `SetDest` reads it only when the goal has not changed.
  bool may_search = true;
  if (move.goto_failed_at != kNoGotoFailure) {
    const std::int64_t failing = now > move.goto_failed_at ? now - move.goto_failed_at : 0;
    const std::int64_t spans = std::min<std::int64_t>(failing >> 11, 10);
    may_search = world.rng().between(0, static_cast<std::int32_t>(2 * spans)) == 0;
  }

  const Point here = world.resolve_position(id);
  // `IsArrived` (`0x005d422b`, `0x005d43b9`): in the band, and on a free spot
  // when the order owns a lock. An arrival clears the failure stamp
  // (0x005d6482).
  if (within(here, dest, order.range) &&
      (!order.lock_destination || movement.ignores_passability(world, id) ||
       movement.spot_free(world, id, here, movement.radius_of(world, id)))) {
    movement.stop(world, id);
    move.goto_failed_at = kNoGotoFailure;
    return HostOutcome::ok_with(Value::boolean(true));
  }

  // Continuation is keyed on the *order*, not on whether a route exists: a
  // destination behind a wall leaves `has_path` false with the order still
  // outstanding, and the next call to the same place searches again from
  // where the unit stands rather than starting a new order. The order is the
  // goal **and its band**: `SetDest` (0x0041a4c0, 0x0041a5c0) compares the
  // range and the inner bound too, and a call that changes either is a new
  // goal. **Not reproduced:** an object goal's range above 919 is stored as
  // 920 and compared as up to 999, so the original finds every such call a
  // new goal and searches again each time.
  const bool continuing = move.goto_active && move.target == dest &&
                          move.target_object == order.target && move.range == order.range &&
                          move.min_range == order.min_range;

  // A new order always searches. So does a live one whose follower has a
  // stop bit set: `SetDest` tests them before anything else (`test al, 6` at
  // 0x0041a4da / 0x0041a5da) and takes its new-goal branch whenever one is,
  // so after a search that laid nothing (`MoveState::route_failed`) or a
  // `Stop` (`stop_requested`) the unit searches again on every call, whatever
  // the draw above says -- and that branch forgets the free-spot search's
  // flag 8 (0x0041a594, `MovementSystem::order_goto`). Otherwise a live order
  // searches again only when its route has run out -- walked to the end of a
  // partial route, or taken away by the grid -- since neither is arrival
  // (`GotoOrder::give_up`), and then only on the draw's 0 (0x0041a513).
  //
  // This waited on the search's budget: built over a search that gave up at
  // 20,000 nodes with a partial route, it sent a wave of Alesia's horsemen
  // (seed 1) into a queue along one and left them on their door, 150 bodies
  // touching. The search now reaches whatever its region holds
  // (`PathFinder`, docs/formats/pathfinder.md), as the original's does.
  if (!continuing || move.stopping() || (!move.has_path && may_search)) {
    const MoveOutcome outcome =
        order.target == kNoObject
            ? movement.order_goto(world, id, dest, order.range, order.min_range, kNoObject,
                                  order.lock_destination)
            : movement.order_goto_object(world, id, order.target, order.range, order.min_range,
                                         order.lock_destination);
    if (outcome == MoveOutcome::arrived) {
      move.goto_failed_at = kNoGotoFailure;
      movement.stop(world, id);
      return HostOutcome::ok_with(Value::boolean(true));
    }
  }

  if (move.has_path) {
    // A route is laid, so the failure stamp is clear (0x005d6528).
    move.goto_failed_at = kNoGotoFailure;
  } else {
    // No route and not arrived: stamp the failure if it is not stamped
    // already (0x005d64db), and once `give_up >= 0` has elapsed since, the
    // call returns 2 and the script ends at it (0x005d6504). `UNIT_ADVANCE.VS`
    // passes 0, so a unit whose route was cut back to where it stands leaves
    // the advance at once instead of searching again every two seconds.
    if (move.goto_failed_at == kNoGotoFailure) move.goto_failed_at = now;
    if (order.give_up >= 0 && now - move.goto_failed_at >= order.give_up) {
      movement.stop(world, id);
      return HostOutcome::end_script();
    }
  }

  // The whole slice, route or none (0x005d65e8); the answer is the re-entry's.
  return goto_wait(ctx, order.slice, move.has_path);
}

void register_movement_host(HostRegistry& registry) {
  const CallKind member = CallKind::member;
  const CallKind free_fn = CallKind::free_function;

  registry.define(member, "Goto", 4, &goto_impl);
  registry.define(member, "Goto", 5, &goto_impl);
  registry.define(member, "Face", 1, &face_impl);
  registry.define(member, "SetSpeedFactor", 1, &set_speed_factor_impl);
  registry.define(member, "SetWalkAnim", 0, &set_walk_anim_impl);
  registry.define(member, "SetWalkAnim", 1, &set_walk_anim_impl);
  registry.define(member, "DoCarryNothing", 0, &do_carry_nothing_impl);        //  3
  registry.define(member, "SetCarryWaterAnim", 0, &set_carry_anim_impl<false>);  //  1
  registry.define(member, "SetCarryGoodsAnim", 0, &set_carry_anim_impl<true>);   //  1
  registry.define(member, "Stop", 1, &stop_impl);
  registry.define(member, "HasPath", 0, &has_path_impl);
  registry.define(member, "PathTo", 3, &path_to_impl);
  registry.define(member, "PathDestFound", 0, &path_dest_found_impl);
  registry.define(member, "ClipDestToMap", 1, &clip_dest_to_map_impl);
  registry.define(member, "TimeWithoutWalking", 0, &time_without_walking_impl);
  registry.define(member, "GetDir", 0, &get_dir_impl);
  registry.define(member, "SetPos", 1, &set_pos_impl);
  registry.define(member, "SetFormation", 1, &set_formation_impl);
  registry.define(member, "dest", 0, &dest_impl);
  registry.define(member, "speed", 0, &speed_impl);
  registry.define(member, "formation", 0, &formation_impl);
  registry.define(member, "FormRadius", 0, &form_radius_impl);

  registry.define(free_fn, "IsPassable3x3", 1, &is_passable_3x3_impl);
  registry.define(free_fn, "FormDescription", 1, &form_description_impl);
}


// --------------------------------------------------------------------------
// the formation's route (docs/formats/formation.md, "The formation's route")
// --------------------------------------------------------------------------

namespace {

/// How far short of its station a unit aimed at a formation is timed to be,
/// and how near it may come before the next sample is made, while the
/// formation's path goes on: 0x85 (0x00419c78, 0x00419e7a, 0x005f589e).
constexpr std::int64_t kSampleShort = 133;

/// How near the end of the formation's path a settled orientation takes over
/// the formation's direction (0x005f5c7d, `< 200`).
constexpr std::int32_t kOrientationReach = 200;

/// The most samples one step makes before it gives up (0x00419b20's loop,
/// which prints "EndlessLoop" past it).
constexpr int kFormationStepPasses = 100;

}  // namespace

Point MovementSystem::station_of(const Formation& form, ObjectId id) noexcept {
  if (id == form.hero) return form.centre;
  const FormationSlot* slot = form.slot_of(id);
  return slot == nullptr ? kNoSample : slot->sample;
}

bool MovementSystem::formation_going_on(ObjectId hero) const noexcept {
  const Formation* form = formation(hero);
  // 0x005f22f0 answers "goes on" with no route object as well; a formation
  // that is gone altogether is this engine's case, and goes nowhere.
  if (form == nullptr) return false;
  return !form->path || form->going_on;
}

Point MovementSystem::formation_position(const World& world, const Formation& form) const {
  if (form.samples == 0) return world.resolve_position(form.hero);
  return form.centre;
}

bool MovementSystem::formation_arrived(const World& world, const Formation& form) const {
  const Point at = formation_position(world, form);
  const std::int64_t d = isqrt(dist_sq(at, form.goal));
  return d >= form.min_range && d <= form.range;
}

bool MovementSystem::formation_gate_bars(const World& world, const Formation& form) const {
  const FormationRoute& route = form.route;
  if (route.gate_crossings.empty()) return false;
  const WorldObject* hero = world.find(form.hero);
  if (hero == nullptr) return false;
  // The route's crossings are kept from its start; what is left of it says
  // how far along the centre has come.
  std::int64_t length = 0;
  for (std::size_t i = 0; i + 1 < route.points.size(); ++i) {
    length += isqrt(dist_sq(route.points[i], route.points[i + 1]));
  }
  const std::int64_t travelled = length - formation_route_left(route);
  const std::int32_t radius = formation_reach(&form, radius_of(world, form.hero));
  const std::int64_t reach =
      static_cast<std::int64_t>(radius > kGateGroupRadius ? radius : kGateGroupRadius) +
      kGateApproach;
  for (const GateCrossing& crossing : route.gate_crossings) {
    if (crossing.at < travelled) continue;
    if (crossing.at - travelled > reach) return false;
    const WorldObject* gate = world.find(crossing.gate);
    if (gate == nullptr) return false;
    return !gate_waves_through(world, *gate, hero->state.owner, world.time());
  }
  return false;
}

bool MovementSystem::formation_recast(World& world, Formation& form, Point start) {
  // Standing in its band already, the formation is not searched for, and the
  // answer is 0 (0x00419730 asks `IsArrived` first).
  {
    const std::int64_t d = isqrt(dist_sq(start, form.goal));
    if (d >= form.min_range && d <= form.range) return false;
  }
  RingGoal goal = ring_goal(world, kNoObject, form.goal, form.range, form.min_range);
  PathRequest request;
  request.start = start;
  request.goal = goal.at;
  request.goal_points = goal.points;
  request.goal_points_given = true;
  request.ignore_passability = false;
  request.owner = nullptr;
  Path found = finder_.find(grid_, request);
  world.gate_lines().refresh(world);
  found = route_past_gates(world, form.hero, start, request, std::move(found));
  if (!found.usable() || found.waypoints.size() < 2) {
    // The record built for the search is deleted and the route kept.
    return formation_route_left(form.route) != 0;
  }
  formation_route_lay(form.route, std::move(found.waypoints), found.status == PathStatus::found);
  world.gate_lines().crossings(form.route.points, form.route.gate_crossings);
  return formation_route_left(form.route) != 0;
}

bool MovementSystem::formation_set_dest(World& world, ObjectId hero, Point dest,
                                        std::int32_t range, std::int32_t min_range, bool lock) {
  Formation& form = formation_of(hero);
  form.centre = world.resolve_position(hero);
  form.should_lock = lock;
  form.samples = 0;
  const bool fresh = !form.path;
  form.path = true;
  bool laid = false;
  if (fresh || form.goal != dest || form.range != range || form.min_range != min_range) {
    // The new-goal branch (0x0041a4c0): the goal taken, and a search.
    form.goal = dest;
    form.range = range;
    form.min_range = min_range;
    if (fresh) form.route = FormationRoute{};
    laid = formation_recast(world, form, formation_position(world, form));
  } else if (!form.route.points.empty() && formation_route_left(form.route) != 0) {
    // The same goal with route left: kept, unless a gate ahead bars it.
    laid = !formation_gate_bars(world, form);
  } else if (!formation_arrived(world, form)) {
    laid = formation_recast(world, form, formation_position(world, form));
  } else {
    laid = !form.route.points.empty() && !formation_gate_bars(world, form);
  }
  form.going_on = laid;
  if (!laid) {
    form.path = false;
    form.route = FormationRoute{};
  }
  return laid;
}

bool MovementSystem::formation_retry_step(World& world, Formation& form, Point& out) {
  // The path follower (0x00419ee0) for a route aimed at no unit: the
  // formation in its band takes no step.
  if (formation_arrived(world, form)) return false;
  const Point from = formation_position(world, form);
  if (form.route.points.empty() && !formation_recast(world, form, from)) return false;
  if (!formation_route_step(form.route, out)) {
    // At the route's end, short of the band: searched again from where the
    // formation stands, and stepped along the new route unless a gate bars it.
    if (formation_arrived(world, form) || !formation_recast(world, form, from)) return false;
    if (formation_gate_bars(world, form)) return false;
    return formation_route_step(form.route, out);
  }
  if (!formation_gate_bars(world, form)) return true;
  // A gate ahead that bars the hero's side: searched again, and stepped along
  // the new route only if that one is not barred within reach as well.
  if (!formation_recast(world, form, from)) return false;
  if (formation_gate_bars(world, form)) return false;
  return formation_route_step(form.route, out);
}

Point MovementSystem::position_now(const World& world, ObjectId id, GameTime now) const {
  if (deciding_) {
    const std::size_t i = lower_bound(id);
    if (i < states_.size() && states_[i].id == id && i < mode_.size() &&
        mode_[i] == kCooperative && states_[i].state.has_path) {
      return cooperative_position(states_[i].state, progress_at(i, now));
    }
  }
  return world.resolve_position(id);
}

void MovementSystem::formation_time_sample(const World& world, Formation& form,
                                           GameTime now) const {
  // Who is timed: the hero and every member with a slot, on the map.
  struct Timed {
    std::int64_t rate;
    std::int64_t straight;
  };
  std::vector<Timed> timed;
  const bool going = !form.path || form.going_on;
  const std::int64_t short_of = going ? kSampleShort : 0;
  const auto add = [&](ObjectId id, Point station) {
    const WorldObject* slot = world.find(id);
    if (slot == nullptr || slot->state.is_held() || station == kNoSample) return;
    const MoveState* move = find(id);
    if (move == nullptr || move->own_rate() <= 0) return;
    const std::int64_t d = isqrt(dist_sq(position_now(world, id, now), station));
    timed.push_back({move->own_rate(), d > short_of ? d - short_of : 0});
  };
  add(form.hero, form.centre);
  for (const FormationSlot& slot : form.slots) add(slot.unit, slot.sample);
  if (timed.empty()) {
    form.speed = 0;
    form.deadline = now;
    return;
  }
  // `UpdateMinSpeed`: the mean speed, held to one and a half times the
  // slowest's.
  std::int64_t sum = 0;
  std::int64_t slowest = timed.front().rate;
  for (const Timed& t : timed) {
    sum += t.rate;
    slowest = std::min(slowest, t.rate);
  }
  const auto n = static_cast<std::int64_t>(timed.size());
  std::int64_t speed = sum / n;
  if (2 * speed > 3 * slowest) speed = 3 * slowest / 2;
  if (speed <= 0) speed = 1;
  form.speed = speed;
  // The deadline: now plus the mean time each needs at that speed.
  std::int64_t total = 0;
  for (const Timed& t : timed) total += t.straight * kSpeedScale / speed;
  form.deadline = now + (total + n / 2) / n;
}

std::int64_t MovementSystem::formation_rate(const Formation& form, const MoveState& move,
                                            std::int64_t left, GameTime now) const {
  const std::int64_t speed = form.speed > 0 ? form.speed : std::max<std::int64_t>(move.own_rate(), 1);
  const std::int64_t slow = std::max<std::int64_t>(speed / 2, 1);
  const std::int64_t fast = std::max<std::int64_t>(speed * 5 / 2, slow);
  const bool going = !form.path || form.going_on;
  const std::int64_t walk = left - (going ? kSampleShort : 0);
  const std::int64_t time = form.deadline - now;
  if (time < 1 || walk <= 0) return fast;
  return std::clamp(walk * kSpeedScale / time, slow, fast);
}

bool MovementSystem::formation_next_sample(World& world, ObjectId hero) {
  return formation_next_sample(world, hero, world.time());
}

bool MovementSystem::formation_next_sample(World& world, ObjectId hero, GameTime now) {
  Formation* form = formation(hero);
  if (form == nullptr || !form->path) return false;
  const Point old = form->centre;
  Point next = old;
  if (!formation_retry_step(world, *form, next) || next == old) {
    // No step: the path is spent (0x005f5cff). Before any sample that ends
    // the march, which the caller does (0x005f2c10).
    form->going_on = false;
    return false;
  }
  formation_route_commit(form->route);
  if (formation_route_left(form->route) == 0) (void)formation_recast(world, *form, next);
  form->going_on = formation_route_left(form->route) != 0;
  const Point step{next.x - old.x, next.y - old.y};
  // A step against the last one: "too radical change in direction".
  if (static_cast<std::int64_t>(form->dir.x) * step.x +
          static_cast<std::int64_t>(form->dir.y) * step.y <
      0) {
    form->reassign = true;
  }
  Point dir = step;
  if (const HeroSystem* heroes = hero_system_of(world); heroes != nullptr) {
    if (const HeroRecord* record = heroes->hero(hero); record != nullptr) {
      const Point settled = record->party_orientation;
      if ((settled.x != 0 || settled.y != 0) &&
          formation_route_left(form->route) < kOrientationReach) {
        dir = settled;
      }
    }
  }
  formation_place_sample(world, *form, formations_, next, dir, form->joined);
  ++form->samples;
  formation_time_sample(world, *form, now);
  return true;
}

void MovementSystem::stand_on_station(World& world, ObjectId id, MoveState& move, Point here,
                                      GameTime until) {
  move.waypoints.assign({here, here});
  move.stride = formation_stride_or_walk(world, id, move);
  move.path_length = 0;
  move.progress = 0;
  move.gate_crossings.clear();
  move.path_generation = grid_generation_;
  move.has_path = true;
  mark_path(world, id, true);
  reset_cooperation(move, until);
  move.last_outcome = MoveOutcome::moving;
}

void MovementSystem::aim_at_formation(World& world, ObjectId id, ObjectId hero, bool lock) {
  MoveState& move = state(id);
  resolve_class_fields(world, id, move);
  if (!move.goto_active || move.stopping() || move.party != hero || !move.form_aimed) {
    move.free_spot_tried = false;
  }
  move.target_object = kNoObject;
  move.min_range = 0;
  move.goto_active = true;
  move.party = hero;
  move.form_aimed = true;
  move.dest_lock = lock && !ignores_passability(world, id);
  move.stop_requested = false;
  move.route_failed = false;
  // Only the mode is set: the route to the station is laid at the unit's
  // next step (0x00419b20, its sample count not yet seen). A route being
  // walked is kept until then, as the retry keeps its media; with none the
  // unit stands, its order taken.
  move.form_seen = -1;
  const bool walking_on = move.has_path && move.waypoints.size() >= 2 &&
                          move.travelled() < move.path_length;
  if (!walking_on) stand_on_station(world, id, move, position(world, id), world.time());
}

void MovementSystem::reroute_to_station(World& world, ObjectId id) {
  MoveState* move = find(id);
  if (move == nullptr || !move->form_aimed) return;
  const Formation* form = formation(move->party);
  const Point here = position(world, id);
  const GameTime now = world.time();
  const Point station = form == nullptr ? kNoSample : station_of(*form, id);
  if (form == nullptr || station == kNoSample || here == station) {
    // Standing on its station, or with none yet: no route, the order kept.
    stand_on_station(world, id, *move, here, now);
    move->form_seen = form == nullptr ? -1 : form->samples;
    return;
  }
  move->form_seen = form->samples;
  if (lay_path(world, id, *move, station, 0, now) == MoveOutcome::blocked) move->form_seen = -1;
  if (!move->has_path) stand_on_station(world, id, *move, here, now);
}

MovementSystem::FormStep MovementSystem::formation_step(World& world, std::size_t entry,
                                                        GameTime now) {
  const ObjectId id = states_[entry].id;
  MoveState& move = states_[entry].state;
  const ObjectId hero = move.party;
  const Point here = cooperative_position(move, move.progress);
  // No step this time (the step answers 0): the route, if any, is kept, and
  // the unit asks again at its next step -- the next turn here.
  const auto pause = [&]() {
    const std::int64_t travelled = std::min(move.progress / kSpeedScale, move.path_length);
    const Point standing = offset_at(move, travelled);
    move.holding = true;
    move.hold_until = turn_end_ + 1;
    move.step_start = travelled;
    move.step_end = travelled;
    move.offset_from = standing;
    move.offset_to = standing;
  };
  // 0x00419b20 lays a route to the station afresh (`RecastPathfind`, its
  // sample count noted) or, standing on it, deletes the route. A search that
  // lays nothing keeps the route the unit had (0x00419a0f) and takes no step;
  // with the formation's path spent it marks the route arrived (`[retry+0x20]
  // |= 0x80`, which `IsArrived` answers 1 for, 0x00417c26), and the unit's
  // order ends where it stands.
  const auto relay = [&](Point station) {
    const Formation* form = formation(hero);
    move.form_seen = form == nullptr ? -1 : form->samples;
    set_position(world, id, here);
    if (here == station) {
      stand_on_station(world, id, move, here, now);
      return true;
    }
    if (lay_path(world, id, move, station, 0, now, &here) != MoveOutcome::blocked &&
        move.has_path) {
      return true;
    }
    move.form_seen = -1;
    if (!formation_going_on(hero)) {
      if (move.has_path) {
        arrive(world, entry, true);
      } else {
        stop(world, id);
      }
    } else if (move.has_path) {
      pause();
    } else {
      stand_on_station(world, id, move, here, turn_end_ + 1);
    }
    return false;
  };

  const Formation* form = formation(hero);
  if (form == nullptr) {
    move.form_aimed = false;
    return FormStep::walk;
  }
  if (move.form_seen != form->samples) {
    const Point station = station_of(*form, id);
    if (station == kNoSample) {
      // No place in the formation: none to walk to while it goes on, and
      // nothing to wait for once its path is spent.
      if (formation_going_on(hero)) {
        stand_on_station(world, id, move, here, turn_end_ + 1);
      } else {
        set_position(world, id, here);
        stop(world, id);
      }
      return FormStep::done;
    }
    if (!relay(station)) return FormStep::done;
  }
  for (int pass = 0; pass < kFormationStepPasses; ++pass) {
    form = formation(hero);
    const std::int64_t travelled = std::min(move.progress / kSpeedScale, move.path_length);
    const std::int64_t left = move.has_path ? move.path_length - travelled : 0;
    if (!formation_going_on(hero)) {
      // The path is spent: walk what is left, then arrive (`decide`), facing
      // the formation's way.
      break;
    }
    // The unit's own walk step, whatever its avoidance strides (0x005d4630:
    // the walk animation's step), or the formation's 200 with none.
    std::int32_t step = walk_step(world, id, move);
    if (step == 0) step = kFormationStride;
    if (left > step / 2 + kSampleShort) break;
    if (!formation_next_sample(world, hero, now)) {
      // No sample to be had: this step is not taken, and the path is spent.
      pause();
      return FormStep::done;
    }
    form = formation(hero);
    if (form == nullptr) return FormStep::done;
    const Point station = station_of(*form, id);
    if (station == kNoSample) {
      stand_on_station(world, id, move, here, turn_end_ + 1);
      return FormStep::done;
    }
    if (!relay(station)) return FormStep::done;
  }
  form = formation(hero);
  if (form != nullptr && move.has_path) {
    const std::int64_t travelled = std::min(move.progress / kSpeedScale, move.path_length);
    move.march_rate = formation_rate(*form, move, move.path_length - travelled, now);
  }
  return FormStep::walk;
}

}  // namespace imperivm::core::sim
