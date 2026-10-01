// The explored map. See include/imperivm/core/sim/fog.hpp.

#include "imperivm/core/sim/fog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

// --------------------------------------------------------------------------
// the map
// --------------------------------------------------------------------------

void ExplorationMap::resize(std::int32_t extent) {
  for (auto& records : fine_) records.clear();
  if (extent <= 0) {
    side_ = 0;
    cells_.clear();
    return;
  }
  // Round up: a map whose extent is not a multiple of the cell size still has a
  // partial cell on each far edge, and `x >> 10` will index it.
  side_ = (extent + kCellSize - 1) / kCellSize;
  cells_.assign(static_cast<std::size_t>(side_) * static_cast<std::size_t>(side_), 0u);
}

std::int32_t ExplorationMap::index_of(Point at) const noexcept {
  if (side_ == 0) return -1;
  if (at.x < 0 || at.y < 0) return -1;
  const std::int32_t cx = at.x / kCellSize;
  const std::int32_t cy = at.y / kCellSize;
  if (cx >= side_ || cy >= side_) return -1;
  return cy * side_ + cx;
}

ExplorationMap::State ExplorationMap::state_at(std::int32_t index,
                                               std::int32_t slot) const noexcept {
  return static_cast<State>((cells_[static_cast<std::size_t>(index)] >> (2 * slot)) & 3u);
}

void ExplorationMap::set_state(std::int32_t index, std::int32_t slot, State state) noexcept {
  std::uint16_t& cell = cells_[static_cast<std::size_t>(index)];
  cell = static_cast<std::uint16_t>((cell & ~(3u << (2 * slot))) |
                                    (static_cast<unsigned>(state) << (2 * slot)));
}

ExplorationMap::FineRecord& ExplorationMap::record_for(std::int32_t index, std::int32_t slot) {
  return fine_[static_cast<std::size_t>(slot)][index];
}

void ExplorationMap::drop_record(std::int32_t index, std::int32_t slot) {
  fine_[static_cast<std::size_t>(slot)].erase(index);
}

void ExplorationMap::normalise(std::int32_t index, std::int32_t slot) {
  auto& records = fine_[static_cast<std::size_t>(slot)];
  const auto it = records.find(index);
  if (it == records.end()) return;
  for (const std::uint8_t value : it->second.values) {
    if (value != kFineMax) return;
  }
  records.erase(it);
  set_state(index, slot, State::full);
}

bool ExplorationMap::explored(Point at, std::int32_t slot) const noexcept {
  // The missing-manager path first: with no grid nothing is explored, for
  // anybody. 0x004c684f answers false before it ever reaches the slot check.
  if (side_ == 0) return false;
  // And then the short circuit at the top of 0x00515950: a player past the
  // eighth has no fog, so every point is explored for it.
  if (slot < 0) return false;
  if (slot >= kSlots) return true;
  const std::int32_t index = index_of(at);
  if (index < 0) return false;
  return state_at(index, slot) != State::never;
}

ExplorationMap::State ExplorationMap::state(std::int32_t cx, std::int32_t cy,
                                            std::int32_t slot) const noexcept {
  if (slot < 0 || slot >= kSlots) return State::never;
  if (cx < 0 || cy < 0 || cx >= side_ || cy >= side_) return State::never;
  return state_at(cy * side_ + cx, slot);
}

const ExplorationMap::FineRecord* ExplorationMap::fine(std::int32_t cx, std::int32_t cy,
                                                       std::int32_t slot) const noexcept {
  if (state(cx, cy, slot) != State::partial) return nullptr;
  const auto& records = fine_[static_cast<std::size_t>(slot)];
  const auto it = records.find(cy * side_ + cx);
  return it == records.end() ? nullptr : &it->second;
}

std::int32_t ExplorationMap::fine_at(std::int32_t fx, std::int32_t fy,
                                     std::int32_t slot) const noexcept {
  if (fx < 0 || fy < 0) return 0;
  const std::int32_t cx = fx / kFineSide;
  const std::int32_t cy = fy / kFineSide;
  switch (state(cx, cy, slot)) {
    case State::never:
      return 0;
    case State::full:
    case State::unwritten:
      return kFineMax;
    case State::partial: {
      const FineRecord* record = fine(cx, cy, slot);
      if (record == nullptr) return 0;
      const std::int32_t i = fx - cx * kFineSide;
      const std::int32_t j = fy - cy * kFineSide;
      return record->values[static_cast<std::size_t>(j) * kFineSide + i];
    }
  }
  return 0;
}

void ExplorationMap::explore(Point at, std::int32_t slot) {
  if (slot < 0 || slot >= kSlots) return;
  const std::int32_t index = index_of(at);
  if (index < 0) return;
  drop_record(index, slot);
  set_state(index, slot, State::full);
}

namespace {

/// The integer square root the rim needs, exact and fast. `sim::isqrt` is
/// Newton's method from `value` down and costs thirty divisions; this is one
/// `sqrt` and a correction, and the correction is what makes it the same
/// function: the largest `r` with `r * r <= value`, on every machine. The
/// values here are squared distances under a few thousand units, far inside
/// the range where a double holds them exactly.
[[nodiscard]] std::int64_t rim_isqrt(std::int64_t value) noexcept {
  if (value <= 0) return 0;
  auto r = static_cast<std::int64_t>(std::sqrt(static_cast<double>(value)));  // imperivm: allow-float: a seed only; the two loops below make the result the exact integer root whatever the rounding
  while (r * r > value) --r;
  while ((r + 1) * (r + 1) <= value) ++r;
  return r;
}

}  // namespace

void ExplorationMap::explore_circle(Point centre, std::int32_t radius, std::int32_t slot) {
  if (side_ == 0 || slot < 0 || slot >= kSlots || radius < 0) return;
  const std::int64_t outer = static_cast<std::int64_t>(radius) + kInner;
  const std::int64_t inner = static_cast<std::int64_t>(radius) - kInner;
  const std::int64_t outer2 = outer * outer;
  const std::int64_t inner2 = inner < 0 ? -1 : inner * inner;
  const std::int32_t lo_x = static_cast<std::int32_t>(
      std::max<std::int64_t>(0, (centre.x - outer) / kCellSize));
  const std::int32_t lo_y = static_cast<std::int32_t>(
      std::max<std::int64_t>(0, (centre.y - outer) / kCellSize));
  const std::int32_t hi_x = static_cast<std::int32_t>(
      std::min<std::int64_t>(side_ - 1, (centre.x + outer) / kCellSize));
  const std::int32_t hi_y = static_cast<std::int32_t>(
      std::min<std::int64_t>(side_ - 1, (centre.y + outer) / kCellSize));
  for (std::int32_t cy = lo_y; cy <= hi_y; ++cy) {
    for (std::int32_t cx = lo_x; cx <= hi_x; ++cx) {
      const std::int32_t index = cy * side_ + cx;
      const State state = state_at(index, slot);
      // The stamp acts on 0 and 2 and leaves 1 and 3 alone.
      if (state == State::unwritten || state == State::full) continue;
      const std::int64_t x0 = static_cast<std::int64_t>(cx) * kCellSize;
      const std::int64_t y0 = static_cast<std::int64_t>(cy) * kCellSize;
      const std::int64_t x1 = x0 + kCellSize - 1;
      const std::int64_t y1 = y0 + kCellSize - 1;
      // Entirely outside `R`: the cell's nearest point is beyond it.
      const std::int64_t nx = std::clamp<std::int64_t>(centre.x, x0, x1) - centre.x;
      const std::int64_t ny = std::clamp<std::int64_t>(centre.y, y0, y1) - centre.y;
      if (nx * nx + ny * ny > outer2) continue;
      // Entirely inside `r - 48`: the cell's farthest corner is within it.
      const std::int64_t fx = std::max(std::abs(x0 - centre.x), std::abs(x1 - centre.x));
      const std::int64_t fy = std::max(std::abs(y0 - centre.y), std::abs(y1 - centre.y));
      if (inner2 >= 0 && fx * fx + fy * fy <= inner2) {
        drop_record(index, slot);
        set_state(index, slot, State::full);
        continue;
      }
      // The annulus: the rim into the record, allocated zeroed on the way from
      // `never`.
      // The stamp (0x00514b80): a lattice point at distance `d` gets 0 beyond
      // `R`, 15 inside `R - 96`, `(R - d) * 15 / 96` between, `max`ed with
      // what was there. The two flat bands are decided on the squared
      // distance; only the ramp itself takes a root.
      const std::int64_t flat = outer - kRamp;
      const std::int64_t flat2 = flat <= 0 ? -1 : flat * flat;
      FineRecord& record = record_for(index, slot);
      set_state(index, slot, State::partial);
      for (std::int32_t j = 0; j < kFineSide; ++j) {
        const std::int64_t dy = y0 + static_cast<std::int64_t>(j) * kFineSpacing - centre.y;
        if (dy > outer || dy < -outer) continue;
        for (std::int32_t i = 0; i < kFineSide; ++i) {
          const std::int64_t dx = x0 + static_cast<std::int64_t>(i) * kFineSpacing - centre.x;
          if (dx > outer || dx < -outer) continue;
          std::uint8_t& nibble = record.values[static_cast<std::size_t>(j) * kFineSide + i];
          if (nibble == kFineMax) continue;
          const std::int64_t d2 = dx * dx + dy * dy;
          if (d2 > outer2) continue;
          // `d < R - 96` is the flat 15, which is `d2 < flat2`; the equality
          // lands on the ramp.
          std::int32_t value = kFineMax;
          if (flat2 < 0 || d2 >= flat2) {
            value = static_cast<std::int32_t>((outer - rim_isqrt(d2)) * kFineMax / kRamp);
          }
          if (value > nibble) nibble = static_cast<std::uint8_t>(value);
        }
      }
      normalise(index, slot);
    }
  }
}

void ExplorationMap::explore_all() {
  // `ExploreAll()` (0x005157c0) fills every half-word with 0xFFFF: state 3 for
  // every slot, and no record has anything left to say.
  std::fill(cells_.begin(), cells_.end(), static_cast<std::uint16_t>(0xFFFFu));
  for (auto& records : fine_) records.clear();
}

ExplorationMap::Search ExplorationMap::nearest_unexplored(Point from, std::int32_t radius,
                                                          std::int32_t slot) const noexcept {
  Search best;
  if (side_ == 0 || slot < 0 || slot >= kSlots || radius < 0) return best;
  const std::int64_t limit = static_cast<std::int64_t>(radius) * radius;
  const std::int32_t half = kCellSize / 2;
  std::int64_t closest = 0;
  for (std::int32_t cy = 0; cy < side_; ++cy) {
    for (std::int32_t cx = 0; cx < side_; ++cx) {
      if (state_at(cy * side_ + cx, slot) != State::never) continue;
      const std::int32_t px = cx * kCellSize + half;
      const std::int32_t py = cy * kCellSize + half;
      const std::int64_t dx = static_cast<std::int64_t>(px) - from.x;
      const std::int64_t dy = static_cast<std::int64_t>(py) - from.y;
      const std::int64_t d2 = dx * dx + dy * dy;
      if (d2 > limit) continue;
      // Strictly less, so a tie keeps the lower cell index -- row-major order.
      if (best.found && d2 >= closest) continue;
      closest = d2;
      best.point = Point{px, py};
      best.found = true;
    }
  }
  return best;
}

std::vector<ExplorationMap::FineEntry> ExplorationMap::fine_entries() const {
  std::vector<FineEntry> out;
  for (std::int32_t slot = 0; slot < kSlots; ++slot) {
    for (const auto& [index, record] : fine_[static_cast<std::size_t>(slot)]) {
      out.push_back(FineEntry{slot, index, &record});
    }
  }
  return out;
}

bool ExplorationMap::set_raw(std::int32_t side, std::span<const std::uint16_t> cells,
                             std::span<const FineEntry> records) {
  if (side < 0) return false;
  if (cells.size() != static_cast<std::size_t>(side) * static_cast<std::size_t>(side)) {
    return false;
  }
  std::array<std::map<std::int32_t, FineRecord>, kSlots> fine;
  for (const FineEntry& entry : records) {
    if (entry.record == nullptr || entry.slot < 0 || entry.slot >= kSlots) return false;
    if (entry.index < 0 || static_cast<std::size_t>(entry.index) >= cells.size()) return false;
    const auto state = static_cast<State>((cells[static_cast<std::size_t>(entry.index)] >>
                                           (2 * entry.slot)) & 3u);
    if (state != State::partial) return false;
    for (const std::uint8_t value : entry.record->values) {
      if (value > kFineMax) return false;
    }
    if (!fine[static_cast<std::size_t>(entry.slot)].emplace(entry.index, *entry.record).second) {
      return false;
    }
  }
  // And the other direction: a partial cell without its record is a grid this
  // writer could not have produced.
  for (std::size_t index = 0; index < cells.size(); ++index) {
    for (std::int32_t slot = 0; slot < kSlots; ++slot) {
      if (((cells[index] >> (2 * slot)) & 3u) != static_cast<unsigned>(State::partial)) continue;
      if (!fine[static_cast<std::size_t>(slot)].contains(static_cast<std::int32_t>(index))) {
        return false;
      }
    }
  }
  side_ = side;
  cells_.assign(cells.begin(), cells.end());
  fine_ = std::move(fine);
  return true;
}

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

void FogSystem::sweep(World& world) {
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none) continue;
    // **Belt and braces.** `kNoPlayer` is 0xFF, which is a slot far past the
    // eighth, and `explore_circle` drops a slot outside 0..7 -- so no fault
    // injected into this line changes an answer and no test can catch it. It
    // is here because "an unowned object reveals nothing" is the rule, and
    // reading it off the width of `PlayerId` is not reading it off anything.
    if (slot.state.owner == kNoPlayer) continue;
    if (slot.state.is_held()) continue;
    if (slot.sight <= 0) continue;
    // **The stamp is idempotent, so a standing object stamps once.** Every
    // write it makes is a `max` or a promotion, and the map only ever grows
    // between stamps, so stamping the same circle for the same slot again
    // changes nothing -- and doing it for every one of a map's objects every
    // turn would cost more than the turn. An object that has moved, changed
    // owner or sight, or was never stamped since the map was last installed,
    // is stamped. Ids are never reused, so the key is safe.
    const Stamp now{slot.state.position, slot.sight, slot.state.owner};
    Stamp& last = stamped_[slot.id];
    if (last == now) continue;
    map_.explore_circle(slot.state.position, slot.sight,
                        static_cast<std::int32_t>(slot.state.owner));
    last = now;
  }
}

void FogSystem::start(World& world) {
  const MatchSystem* match = match_system_of(world);
  map_.resize(match == nullptr ? 0 : match->rules().map_size);
  stamped_.clear();
  sweep(world);
}

void FogSystem::advance(World& world, const Turn& turn) {
  (void)turn;
  sweep(world);
}

FogSystem* fog_system_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "fog") return static_cast<FogSystem*>(system);
  }
  return nullptr;
}

std::int32_t fog_slot_from_script(std::int32_t player) noexcept {
  return player >= 1 && player <= 16 ? player - 1 : -1;
}

// --------------------------------------------------------------------------
// the host entry points
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::HostOutcome;
using script::Value;

[[nodiscard]] World* host_world(CallContext& ctx) {
  return ctx.user == nullptr ? nullptr : static_cast<HostContext*>(ctx.user)->world;
}

[[nodiscard]] ExplorationMap* host_map(CallContext& ctx) {
  World* world = host_world(ctx);
  if (world == nullptr) return nullptr;
  FogSystem* fog = fog_system_of(*world);
  return fog == nullptr ? nullptr : &fog->map();
}

/// `IsExplored(pt, player)` -- 5 sites, all of them a tutorial hint deciding
/// whether the player has found the thing it is about to talk about.
///
/// `0x004c6820`: no grid or no fog manager answers **false**; otherwise the
/// slot is `player - 1` and a slot past the eighth answers **true**. Both are
/// `ExplorationMap::explored`'s, and both matter -- the tutorials run as
/// player 1 and the answer for player 9 is a different question.
HostOutcome fn_is_explored(CallContext& ctx) {
  const ExplorationMap* map = host_map(ctx);
  if (map == nullptr || ctx.count() < 2) return HostOutcome::ok_with(Value::boolean(false));
  if (!is_point(ctx.arg(0)) || !ctx.arg(1).is_integer()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const std::int32_t slot = fog_slot_from_script(ctx.arg(1).as_integer());
  return HostOutcome::ok_with(Value::boolean(map->explored(unpack_point(ctx.arg(0)), slot)));
}

/// `ExploreCircle(player, pt, radius)` -- 3 sites: a settlement's bought map, a
/// tower's own sight, and the research that widens it.
HostOutcome fn_explore_circle(CallContext& ctx) {
  ExplorationMap* map = host_map(ctx);
  if (map == nullptr || ctx.count() < 3) return HostOutcome::ok_void();
  if (!ctx.arg(0).is_integer() || !is_point(ctx.arg(1)) || !ctx.arg(2).is_integer()) {
    return HostOutcome::ok_void();
  }
  map->explore_circle(unpack_point(ctx.arg(1)), ctx.arg(2).as_integer(),
                      fog_slot_from_script(ctx.arg(0).as_integer()));
  return HostOutcome::ok_void();
}

/// `ExploreArea(player, area)` -- 24 sites, every one inside a map container.
///
/// The original reaches the named area, takes its centre and a radius from its
/// own extent, and hands both to the **same circle stamp** `ExploreCircle`
/// uses (0x004c95e0). A name that matches nothing prints *"Could not find area
/// named '%s' in function 'ExploreArea'. Check the spelling."* and explores
/// nothing, which is a return and not a refusal.
HostOutcome fn_explore_area(CallContext& ctx) {
  World* world = host_world(ctx);
  ExplorationMap* map = host_map(ctx);
  if (world == nullptr || map == nullptr || ctx.count() < 2) return HostOutcome::ok_void();
  if (!ctx.arg(0).is_integer() || !ctx.arg(1).is_string()) return HostOutcome::ok_void();
  const AreaShape* shape = area_named(*world, ctx.arg(1).as_string());
  if (shape == nullptr) return HostOutcome::ok_void();
  const AreaBounds box = shape->bounds();
  // The radius that covers the shape from its own centre. A circle area gives
  // back its own radius exactly; a rectangle gives the half-diagonal, which is
  // the smallest circle that holds it.
  const Point centre = shape->centre();
  const std::int64_t dx = std::max<std::int64_t>(centre.x - box.left, box.right - centre.x);
  const std::int64_t dy = std::max<std::int64_t>(centre.y - box.top, box.bottom - centre.y);
  const std::int32_t radius = static_cast<std::int32_t>(isqrt(dx * dx + dy * dy));
  map->explore_circle(centre, radius, fog_slot_from_script(ctx.arg(0).as_integer()));
  return HostOutcome::ok_void();
}

/// `ExploreAll()` -- 9 sites, and every one of them is a mission ending or a
/// cutscene that wants the whole board visible.
HostOutcome fn_explore_all(CallContext& ctx) {
  if (ExplorationMap* map = host_map(ctx); map != nullptr) map->explore_all();
  return HostOutcome::ok_void();
}

/// `Unit::GetUnexploredPoint(radius)` -- 1 site, and its argument is a *clamp*
/// rather than a radius.
///
/// `0x005d9a45`: when the argument is below the unit's own `sight` or above
/// 800,000 it becomes `max(sight * 4, 512)`. `UNIT_EXPLORE.VS` passes **-1**,
/// so every shipped call takes that branch. A miss answers `(-1, -1)`, which is
/// exactly what the script tests: `if (pt.x == -1) break;`.
HostOutcome fn_get_unexplored_point(CallContext& ctx) {
  World* world = host_world(ctx);
  const ExplorationMap* map = host_map(ctx);
  const auto nowhere = [] { return HostOutcome::ok_with(pack_point(Point{-1, -1})); };
  if (world == nullptr || map == nullptr) return nowhere();
  // The receiver, resolved the way every collection member in this tree does:
  // any object handle the model mints, and nothing else.
  if (!ctx.arg(0).is_object()) return nowhere();
  const WorldObject* slot = world->find(ctx.arg(0).as_object().id);
  if (slot == nullptr || slot->state.owner == kNoPlayer) return nowhere();

  std::int32_t radius = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : -1;
  if (radius < slot->sight || radius > 800000) {
    radius = std::max<std::int32_t>(slot->sight * 4, 512);
  }
  const ExplorationMap::Search found = map->nearest_unexplored(
      world->resolve_position(slot->id), radius, static_cast<std::int32_t>(slot->state.owner));
  return found.found ? HostOutcome::ok_with(pack_point(found.point)) : nowhere();
}

constexpr std::size_t kEntryCount = 5;

}  // namespace

std::size_t fog_host_entry_count() noexcept { return kEntryCount; }

std::size_t register_fog_host(script::HostRegistry& registry) {
  const std::size_t before = registry.implemented();
  constexpr script::CallKind kFree = script::CallKind::free_function;
  registry.define(kFree, "IsExplored", 2, &fn_is_explored);        //  5
  registry.define(kFree, "ExploreArea", 2, &fn_explore_area);      // 24
  registry.define(kFree, "ExploreCircle", 3, &fn_explore_circle);  //  3
  registry.define(kFree, "ExploreAll", 0, &fn_explore_all);        //  9
  registry.define(script::CallKind::member, "GetUnexploredPoint", 1,
                  &fn_get_unexplored_point);  //  1
  // `RecreateExploration/0` and `ToggleFog/0` are registered in `gbr.exe` and
  // are not bound here; the header says why for each.
  // `RevealHiddenEnemyUnits/3` is bound, in `sim/player_host.cpp`: it is a
  // diplomacy test and a hidden bit, and never looks at this grid.
  return registry.implemented() - before;
}

}  // namespace imperivm::core::sim
