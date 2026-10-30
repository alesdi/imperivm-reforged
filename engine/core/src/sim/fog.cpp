// The explored map. See include/imperivm/core/sim/fog.hpp.

#include "imperivm/core/sim/fog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

// --------------------------------------------------------------------------
// the map
// --------------------------------------------------------------------------

void ExplorationMap::resize(std::int32_t extent) {
  // A fresh map, as the constructor (0x005179d0) makes one: the flag down.
  everywhere_ = false;
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
  // Then 0x00515950 itself. Its slot is **unsigned**: anything past the
  // eighth -- and so also the -1 that `IsExplored(pt, 0)` decrements to -- is
  // explored everywhere (0x00515958).
  if (static_cast<std::uint32_t>(slot) >= static_cast<std::uint32_t>(kSlots)) return true;
  const std::int32_t index = index_of(at);
  if (index < 0) return false;
  switch (state_at(index, slot)) {
    case State::never:
      return false;
    case State::full:
      return true;
    case State::partial:
    case State::unwritten:
      break;
  }
  // **A partial cell answers its fine record at the point, not "seen".** For
  // states 1 and 2 0x00515950 walks the slot's record list (`[expl + 0x50 +
  // slot * 12]`) for the record whose rectangle holds the point -- none is
  // false -- and reads the four-bit value of the 32-unit sub-cell the point
  // falls in (0x00514a60: row `dy >> 5`, column `dx >> 5` from the record's
  // origin); explored iff it is not zero. A record here is its cell's, at the
  // cell's origin, so that is `fine_at` at `(x >> 5, y >> 5)`. State 1 is
  // never written and has no record, so it answers false.
  const auto it = fine_[static_cast<std::size_t>(slot)].find(index);
  if (it == fine_[static_cast<std::size_t>(slot)].end()) return false;
  const std::int32_t i = (at.x % kCellSize) / kFineSpacing;
  const std::int32_t j = (at.y % kCellSize) / kFineSpacing;
  return it->second.values[static_cast<std::size_t>(j) * kFineSide + i] != 0;
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
  // 0x00516450 returns at once for a radius of **zero** or an empty mask. A
  // negative radius is refused here too, which the original does not do (it
  // runs its cell loop over a negative box); no caller passes one.
  if (side_ == 0 || slot < 0 || slot >= kSlots || radius <= 0) return;
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
  // every slot, and no record has anything left to say. It raises the
  // explored-everywhere flag first, whatever the grid holds.
  everywhere_ = true;
  std::fill(cells_.begin(), cells_.end(), static_cast<std::uint16_t>(0xFFFFu));
  for (auto& records : fine_) records.clear();
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
    // **Belt and braces.** An unowned object has no record and so no
    // `viewflags`; `fog_view_mask` answers 0 for it and the stamp is skipped
    // below anyway. It is here because "an unowned object reveals nothing" is
    // the rule, and reading it off the width of `PlayerId` is not reading it
    // off anything.
    if (slot.state.owner == kNoPlayer) continue;
    if (slot.state.is_held()) continue;
    if (slot.sight <= 0) continue;
    // **The stamp is idempotent, so a standing object stamps once.** Every
    // write it makes is a `max` or a promotion, and the map only ever grows
    // between stamps, so stamping the same circle for the same slots again
    // changes nothing -- and doing it for every one of a map's objects every
    // turn would cost more than the turn. An object that has moved, changed
    // owner or sight, whose owner's `viewflags` changed, or that was never
    // stamped since the map was last installed, is stamped. Ids are never
    // reused, so the key is safe.
    const std::uint32_t mask = fog_view_mask(world, slot.state.owner);
    const Stamp now{slot.state.position, slot.sight, slot.state.owner, mask};
    Stamp& last = stamped_[slot.id];
    if (last == now) continue;
    // The object's own stamps (0x005ab360, 0x0052def0, and 0x00517ad0 from
    // 0x005aa0a0) all pass the owner record's `viewflags` (`[[obj+0x70] +
    // 0x14]`), so a unit explores for every player its owner shares its view
    // with, not only for itself.
    map_.explore_circle_mask(slot.state.position, slot.sight, mask);
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

std::uint32_t fog_view_mask(const World& world, PlayerId player) noexcept {
  if (!PlayerTable::is_valid(player)) return 0;
  // The player's own bit. `SetRelation` (0x005652f0) never writes the
  // diagonal, so the own bit is the record constructor's, which was not
  // located; **inferred** from the fact that without it no unit would explore
  // for its own owner.
  std::uint32_t mask = 1u << player;
  // `SetRelation(A, B, word)`: share-view (0x10) in A's word for B sets B's
  // `playerflag` in A's `viewflags` (`record + 0x14`), and clears it without.
  // So A's view mask is A plus every B A's own row grants share-view.
  for (PlayerId other = 0; other < static_cast<PlayerId>(ExplorationMap::kSlots); ++other) {
    if (other == player) continue;
    if (world.players().has(player, other, Relation::share_view)) mask |= 1u << other;
  }
  // The stamp keeps the low byte (0x00516474): players 9..16 have no fog slot.
  return mask & 0xffu;
}

void ExplorationMap::explore_circle_mask(Point centre, std::int32_t radius, std::uint32_t mask) {
  // 0x00516450 walks the mask's bits within each cell, two state bits a slot;
  // the slots are independent, so slot by slot is the same writes.
  for (std::int32_t slot = 0; slot < kSlots; ++slot) {
    if (((mask >> slot) & 1u) != 0) explore_circle(centre, radius, slot);
  }
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
/// `0x004c6820` pops the player, then the point. No exploration manager
/// (`[0x009a721c] == 0` or its `+0xb0` null) answers **false**. Otherwise it
/// hands the point and `player - 1` to 0x00515950 -- `ExplorationMap::explored`
/// -- whose slot is unsigned, so player 0 (slot -1) and every player past the
/// eighth answer **true**, and a partially explored cell answers its fine
/// record at the point. No argument is validated.
HostOutcome fn_is_explored(CallContext& ctx) {
  const ExplorationMap* map = host_map(ctx);
  if (map == nullptr || ctx.count() < 2) return HostOutcome::ok_with(Value::boolean(false));
  if (!is_point(ctx.arg(0)) || !ctx.arg(1).is_integer()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const std::int32_t slot = ctx.arg(1).as_integer() - 1;
  return HostOutcome::ok_with(Value::boolean(map->explored(unpack_point(ctx.arg(0)), slot)));
}

/// The `viewflags` a script's player number reaches in `ExploreCircle` and
/// `ExploreArea`: both bounds-check `0 <= player < 16` and read
/// `[globals + 0xfc0 + player * 0x320]`, which is `record[player - 1] +
/// 0x14` (see the header). Player 0 passes the check and reads the word
/// before the record array; that is a fault rather than a behaviour, so it
/// explores nothing here. Player 16 fails the check and explores nothing in
/// both.
[[nodiscard]] std::uint32_t script_view_mask(const World& world, std::int32_t player) noexcept {
  if (player < 1 || player >= 16) return 0;
  return fog_view_mask(world, static_cast<PlayerId>(player - 1));
}

/// `ExploreCircle(player, pt, radius)` -- 3 sites: a settlement's bought map, a
/// tower's own sight, and the research that widens it.
///
/// `0x004c6470` pops the radius, the point and the player; a player outside
/// `0..15` does nothing, and otherwise the circle stamp (0x00516450) runs with
/// that player's `viewflags` -- its own slot and every slot it shares its view
/// with. A radius of zero stamps nothing (the stamp's own first test).
HostOutcome fn_explore_circle(CallContext& ctx) {
  World* world = host_world(ctx);
  ExplorationMap* map = host_map(ctx);
  if (world == nullptr || map == nullptr || ctx.count() < 3) return HostOutcome::ok_void();
  if (!ctx.arg(0).is_integer() || !is_point(ctx.arg(1)) || !ctx.arg(2).is_integer()) {
    return HostOutcome::ok_void();
  }
  map->explore_circle_mask(unpack_point(ctx.arg(1)), ctx.arg(2).as_integer(),
                           script_view_mask(*world, ctx.arg(0).as_integer()));
  return HostOutcome::ok_void();
}

/// `ExploreArea(player, area)` -- 24 sites, every one inside a map container.
///
/// `0x004c9430` pops the name, then the player. It resolves the name in the
/// named-group dictionary (0x004c93c0) and takes the group's front member as
/// the area; a name that matches nothing, or a front member whose handle is
/// dead, prints *"Could not find area named '%s' in function 'ExploreArea'.
/// Check the spelling."* and explores nothing, which is a return and not a
/// refusal. A player outside `0..15` explores nothing either. Otherwise the
/// circle stamp runs at the area's centre (0x004d67a0: the circle's own
/// centre, or the rectangle's truncated midpoints) with radius **the bounding
/// box's bottom less the centre's y** (0x004c95c1: `[box + 0xc] - [centre +
/// 4]`, the box from 0x004d6800) -- a circle's own radius, a rectangle's half
/// *height*, not a radius that covers it -- and the player's `viewflags`.
///
/// The original takes a rectangle's corners as authored where `AreaShape`
/// sorts them; for a rectangle authored bottom-up the original's radius is
/// negative. No shipped area is.
HostOutcome fn_explore_area(CallContext& ctx) {
  World* world = host_world(ctx);
  ExplorationMap* map = host_map(ctx);
  if (world == nullptr || map == nullptr || ctx.count() < 2) return HostOutcome::ok_void();
  if (!ctx.arg(0).is_integer() || !ctx.arg(1).is_string()) return HostOutcome::ok_void();
  const AreaShape* shape = area_named(*world, ctx.arg(1).as_string());
  if (shape == nullptr) return HostOutcome::ok_void();
  const Point centre = shape->centre();
  const std::int32_t radius = shape->bounds().bottom - centre.y;
  map->explore_circle_mask(centre, radius, script_view_mask(*world, ctx.arg(0).as_integer()));
  return HostOutcome::ok_void();
}

/// `ExploreAll()` -- 9 sites, and every one of them is a mission ending or a
/// cutscene that wants the whole board visible. 0x006a42a0 calls 0x005157c0
/// and nothing else.
HostOutcome fn_explore_all(CallContext& ctx) {
  if (ExplorationMap* map = host_map(ctx); map != nullptr) map->explore_all();
  return HostOutcome::ok_void();
}

/// The terrain-type bytes `GetUnexploredPoint` treats as water (0x005d9b9e,
/// `cmp 0xc` and `cmp 0xd`). `IsPointInWater` names only 13.
constexpr std::uint32_t kExploreWaterLow = 12;
constexpr std::uint32_t kExploreWaterHigh = 13;
/// The sample grid's step and the most candidates kept.
constexpr std::int32_t kExploreStep = 300;
constexpr std::int32_t kExploreMaxCandidates = 100;

/// `Unit::GetUnexploredPoint(radius)` -- 1 site, `UNIT_EXPLORE.VS`, which
/// passes **-1** and tests the answer's `x` against -1.
///
/// What 0x005d99e0 does, in its order:
///
///   1. A receiver handle that names no object prints "...called for an
///      uninitialized or invalid object." and answers `(-1, -1)`. Nothing else
///      about the receiver is checked.
///   2. The radius: an argument below the unit's `sight` (`[obj+0xd0]`) or
///      above 800,000 becomes `sight * 4`, raised to 512; then anything above
///      2,999 becomes 3,000.
///   3. A grid of offsets from `-r/2` (C division) up to but not including
///      `r/2` in steps of 300, **x in the outer loop, y in the inner**, each
///      added to the unit's position (`vtbl+0x3c`) and clamped into the map
///      rectangle (`[0x009a721c]+0xb4..0xc0`).
///   4. A sample is a candidate when the owner's slot (`[[obj+0x70]+8]`) has
///      not explored it (0x00515950), its 16-unit obstruction cell
///      (`[0x009a721c]+0x60`, the grid the area sampler tests) is clear, and
///      its terrain-type byte -- the 64-unit layer, no half-cell bias -- is
///      12 or 13 for a `water_unit` class (`[class+0xb30]`) and neither for
///      any other. The first 100 candidates are kept and the scan stops at
///      the hundredth.
///   5. None: `(-1, -1)`. Otherwise **one draw**, `Range(0, n - 1)` on the
///      game's generator (`[0x00996ff4]+0x12a0`, `vtbl+0x14`), picks the
///      answer -- which, with one candidate, draws nothing.
///
/// This used to answer the nearest never-seen cell's centre within the
/// radius, with no draw: a different point, and the world's random stream one
/// draw short whenever more than one candidate stood.
HostOutcome fn_get_unexplored_point(CallContext& ctx) {
  World* world = host_world(ctx);
  const ExplorationMap* map = host_map(ctx);
  const auto nowhere = [] { return HostOutcome::ok_with(pack_point(Point{-1, -1})); };
  if (world == nullptr || map == nullptr) return nowhere();
  if (!ctx.arg(0).is_object()) return nowhere();
  const WorldObject* slot = world->find(ctx.arg(0).as_object().id);
  if (slot == nullptr) return nowhere();

  std::int32_t radius = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : -1;
  if (radius < slot->sight || radius > 800000) {
    radius = std::max<std::int32_t>(slot->sight * 4, 512);
  }
  radius = std::min<std::int32_t>(radius, 3000);

  // The map rectangle, `(0, 0, size - 1, size - 1)`.
  const MatchSystem* match = match_system_of(*world);
  const std::int32_t high = match != nullptr && match->rules().map_size > 0
                                ? match->rules().map_size - 1
                                : map->cells() * ExplorationMap::kCellSize - 1;
  const MovementSystem* movement = movement_system(*world);
  const ObstructionGrid* grid = movement != nullptr ? &movement->grid() : nullptr;
  if (grid != nullptr && grid->empty()) grid = nullptr;
  const Grid& terrain = world->terrain();
  const bool water_unit = class_flag(*world, *slot, "water_unit");
  // An unowned object's slot is past the eighth, which 0x00515950 answers
  // explored everywhere: no candidates.
  const std::int32_t fog_slot = slot->state.owner == kNoPlayer
                                    ? ExplorationMap::kSlots
                                    : static_cast<std::int32_t>(slot->state.owner);

  const Point from = world->resolve_position(slot->id);
  const std::int32_t half = radius / 2;
  std::vector<Point> candidates;
  for (std::int32_t ox = -half; ox < half; ox += kExploreStep) {
    for (std::int32_t oy = -half; oy < half; oy += kExploreStep) {
      const Point at{std::clamp(from.x + ox, 0, high), std::clamp(from.y + oy, 0, high)};
      if (map->explored(at, fog_slot)) continue;
      if (grid != nullptr && grid->blocked(at)) continue;
      std::uint32_t type = 0;
      if (terrain.cell_size() != 0) {
        const auto cx = static_cast<std::uint32_t>(at.x) / terrain.cell_size();
        const auto cy = static_cast<std::uint32_t>(at.y) / terrain.cell_size();
        if (cx < terrain.width() && cy < terrain.height()) type = terrain.cell(cx, cy);
      }
      const bool water = type == kExploreWaterLow || type == kExploreWaterHigh;
      if (water != water_unit) continue;
      candidates.push_back(at);
      if (static_cast<std::int32_t>(candidates.size()) == kExploreMaxCandidates) break;
    }
    if (static_cast<std::int32_t>(candidates.size()) == kExploreMaxCandidates) break;
  }
  if (candidates.empty()) return nowhere();
  const std::int32_t pick =
      world->rng().between(0, static_cast<std::int32_t>(candidates.size()) - 1);
  return HostOutcome::ok_with(pack_point(candidates[static_cast<std::size_t>(pick)]));
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
