// The gate's portcullis. See include/imperivm/core/sim/gate.hpp for what is
// read off the executable and what is inferred.

#include "imperivm/core/sim/gate.hpp"

#include <algorithm>
#include <span>

#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

std::int32_t gate_position(const GateMotion& motion, bool open, GameTime now) noexcept {
  const std::int32_t from =
      motion.from < 0 ? 0 : (motion.from > kGateRaised ? kGateRaised : motion.from);
  // Already there: the original's tick does not start a move at all, which is
  // also what keeps the duration below from ever being zero.
  if (open ? from == kGateRaised : from == 0) return from;
  const std::int64_t distance = open ? kGateRaised - from : from;
  const std::int64_t duration = distance * kGateTravelMs / kGateRaised;
  const GameTime elapsed = now > motion.start ? now - motion.start : 0;
  // Sixty-four bits where the original multiplies in thirty-two: it stops
  // evaluating this once the position arrives, and this keeps evaluating it
  // for as long as the gate stands, so the product has to be allowed to grow.
  const std::int64_t moved = duration == 0 ? distance : elapsed * distance / duration;
  if (open) return moved >= distance ? kGateRaised : from + static_cast<std::int32_t>(moved);
  return moved >= distance ? 0 : from - static_cast<std::int32_t>(moved);
}

std::int32_t gate_raise(const WorldObject& slot, GameTime now) noexcept {
  if (slot.object == nullptr || !slot.object->is_a(NativeClass::gate)) return 0;
  return gate_position(slot.gate, slot.state.flags.gate_open, now);
}

void gate_retarget(WorldObject& slot, bool open, GameTime now) noexcept {
  // Where the portcullis is under the *old* target, before the new one is
  // written: that is the position the move starts from.
  slot.gate.from = gate_raise(slot, now);
  slot.gate.start = now;
  slot.state.flags.gate_open = open;
}

bool gate_lets_through(const WorldObject& slot, GameTime now) noexcept {
  if (slot.object == nullptr || !slot.object->is_a(NativeClass::gate)) return false;
  if (slot.state.health <= 0) return true;
  return gate_raise(slot, now) > kGatePassableAbove;
}

bool gate_fully_open(const WorldObject& slot, GameTime now) noexcept {
  if (slot.object == nullptr || !slot.object->is_a(NativeClass::gate)) return false;
  return slot.state.flags.gate_open && gate_raise(slot, now) == kGateRaised;
}

bool gate_axis(const WorldObject& gate, Point& a, Point& b) {
  if (gate.object == nullptr || gate.object->entity == nullptr) return false;
  // The first type-7 marker is one end; every one after it overwrites the
  // other (0x005293f0: the first end is taken only while it is unset).
  bool first = false;
  bool second = false;
  std::int64_t sum_x = 0;
  std::int64_t sum_y = 0;
  std::int32_t tens = 0;
  for (const EntityPoint& point : gate.object->entity->points()) {
    if (point.type == 7) {
      if (!first) {
        a = Point{point.x, point.y};
        first = true;
      } else {
        b = Point{point.x, point.y};
        second = true;
      }
    } else if (point.type == 10) {
      sum_x += point.x;
      sum_y += point.y;
      ++tens;
    }
  }
  if (!second) return false;
  if (tens == 0) return true;
  const std::int64_t mean_x = sum_x / tens;
  const std::int64_t mean_y = sum_y / tens;
  const std::int64_t dx = static_cast<std::int64_t>(b.x) - a.x;
  const std::int64_t dy = static_cast<std::int64_t>(b.y) - a.y;
  const std::int64_t length_sq = dx * dx + dy * dy;
  if (length_sq == 0) return false;
  // The foot of the mean on the axis, in 10-bit fixed point as 0x00529478
  // keeps it. The shift back is biased before it rounds (`cdq; and edx, 0x3ff;
  // add; sar 10`), which is a division truncating toward zero, not a floor.
  const std::int64_t dot = (a.x - mean_x) * (a.x - b.x) + (a.y - mean_y) * (a.y - b.y);
  const std::int64_t t = (dot << 10) / length_sq;
  const std::int64_t foot_x = a.x + (dx * t) / 1024;
  const std::int64_t foot_y = a.y + (dy * t) / 1024;
  const std::int64_t off_x = mean_x - foot_x + gate.state.position.x;
  const std::int64_t off_y = mean_y - foot_y + gate.state.position.y;
  a = Point{static_cast<std::int32_t>(a.x + off_x), static_cast<std::int32_t>(a.y + off_y)};
  b = Point{static_cast<std::int32_t>(b.x + off_x), static_cast<std::int32_t>(b.y + off_y)};
  return true;
}

void gate_line_cells(Point a, Point b, std::vector<GateCell>& out) {
  // To cells, truncating towards zero as `cdq; and edx, 0xf; add; sar 4` does
  // -- which C++'s division does too.
  const std::int32_t x0 = a.x / kCollisionCellSize;
  const std::int32_t y0 = a.y / kCollisionCellSize;
  const std::int32_t x1 = b.x / kCollisionCellSize;
  const std::int32_t y1 = b.y / kCollisionCellSize;
  const std::int32_t dx = x1 - x0;
  const std::int32_t dy = y1 - y0;
  const std::int32_t adx = dx < 0 ? -dx : dx;
  const std::int32_t ady = dy < 0 ? -dy : dy;
  // 0x005298f0: the cell, and the one below it.
  const auto visit = [&out](std::int32_t x, std::int32_t y) {
    out.push_back(GateCell{x, y});
    out.push_back(GateCell{x, y + 1});
  };
  // One loop for the eight branches of 0x00529e40: the major coordinate runs
  // from the first end to the second inclusive, and the error starts at
  // `major - 2 * minor` and steps the minor coordinate when it is not positive.
  const bool x_major = adx > ady;
  const std::int32_t major = x_major ? adx : ady;
  const std::int32_t minor = x_major ? ady : adx;
  const std::int32_t major_step = x_major ? (dx > 0 ? 1 : -1) : (dy > 0 ? 1 : -1);
  const std::int32_t minor_step = x_major ? (dy < 0 ? -1 : 1) : (dx < 0 ? -1 : 1);
  std::int32_t error = major - 2 * minor;
  std::int32_t x = x0;
  std::int32_t y = y0;
  for (std::int32_t i = 0; i <= major; ++i) {
    visit(x, y);
    if (error > 0) {
      error -= 2 * minor;
    } else {
      error += 2 * (major - minor);
      (x_major ? y : x) += minor_step;
    }
    (x_major ? x : y) += major_step;
  }
}

// --------------------------------------------------------------------------
// the routes
// --------------------------------------------------------------------------

std::int64_t route_crossing(std::span<const Point> route, Point a, Point b) noexcept {
  // 0x0040aab0 with the gate as its first segment and the leg as its second:
  // `denominator` is the cross product of the two directions, and the meeting
  // point is taken along the leg from its far end, divided truncating.
  const std::int64_t gx = static_cast<std::int64_t>(a.x) - b.x;
  const std::int64_t gy = static_cast<std::int64_t>(a.y) - b.y;
  std::int64_t before = 0;
  for (std::size_t i = 1; i < route.size(); ++i) {
    const Point from = route[i - 1];
    const Point to = route[i];
    const std::int64_t lx = static_cast<std::int64_t>(from.x) - to.x;
    const std::int64_t ly = static_cast<std::int64_t>(from.y) - to.y;
    const std::int64_t denominator = gx * -ly + gy * lx;
    const auto within = [denominator](std::int64_t value) {
      return denominator > 0 ? value >= 0 && value <= denominator
                             : value <= 0 && value >= denominator;
    };
    if (denominator != 0) {
      const std::int64_t tx = static_cast<std::int64_t>(to.x) - b.x;
      const std::int64_t ty = static_cast<std::int64_t>(to.y) - b.y;
      const std::int64_t on_gate = tx * -ly + ty * lx;
      const std::int64_t on_leg = tx * -gy + ty * gx;
      if (within(on_gate) && within(on_leg)) {
        const Point meet{static_cast<std::int32_t>(to.x + lx * on_leg / denominator),
                         static_cast<std::int32_t>(to.y + ly * on_leg / denominator)};
        return before + distance(from, meet);
      }
    }
    before += distance(from, to);
  }
  return -1;
}

bool gate_bars(const World& world, const WorldObject& gate, PlayerId owner) noexcept {
  return world.players().is_enemy(gate.state.owner, owner);
}

bool gate_waves_through(const World& world, const WorldObject& gate, PlayerId owner,
                        GameTime now) noexcept {
  // A friend, with nobody hostile near the gate: through, whatever the
  // portcullis says. The action-name test is not reproduced; see the header.
  if (!gate_bars(world, gate, owner) && !gate.state.flags.enemies_near) return true;
  // Anyone else: through a gate that lets units through, or one opening.
  return gate_lets_through(gate, now) || gate.state.flags.gate_open;
}

void GateLines::refresh(const World& world) {
  // A gate that is gone takes its line with it.
  std::erase_if(lines_, [&world](const Line& line) { return world.find(line.gate) == nullptr; });
  if (world.next_id() <= scanned_) return;
  // Gates spawned since the last call, ascending: appended in id order, they
  // keep the list in it.
  const std::span<const WorldObject> objects = world.objects();
  auto from = std::lower_bound(objects.begin(), objects.end(), scanned_,
                               [](const WorldObject& slot, ObjectId id) { return slot.id < id; });
  for (; from != objects.end(); ++from) {
    if (from->object == nullptr || !from->object->is_a(NativeClass::gate)) continue;
    Line line;
    line.gate = from->id;
    line.has_axis = gate_axis(*from, line.a, line.b);
    std::vector<GateCell> cells;
    if (line.has_axis) gate_line_cells(line.a, line.b, cells);
    for (const GateCell& cell : cells) {
      if (std::find(line.cells.begin(), line.cells.end(), cell) == line.cells.end()) {
        line.cells.push_back(cell);
      }
    }
    lines_.push_back(std::move(line));
  }
  scanned_ = world.next_id();
}

void GateLines::forget() noexcept {
  lines_.clear();
  scanned_ = 0;
}

const GateLines::Line* GateLines::find(ObjectId gate) const noexcept {
  const auto at = std::lower_bound(lines_.begin(), lines_.end(), gate,
                                   [](const Line& line, ObjectId id) { return line.gate < id; });
  return at != lines_.end() && at->gate == gate ? &*at : nullptr;
}

void GateLines::crossings(std::span<const Point> route, std::vector<GateCrossing>& out) const {
  out.clear();
  for (const Line& line : lines_) {
    if (!line.has_axis) continue;
    const std::int64_t at = route_crossing(route, line.a, line.b);
    if (at >= 0) out.push_back(GateCrossing{line.gate, at});
  }
  // Route order; the gate order a tie keeps is the list's own, ascending id.
  std::stable_sort(out.begin(), out.end(),
                   [](const GateCrossing& x, const GateCrossing& y) { return x.at < y.at; });
}

void GateLines::lay_enemy_gates(const World& world, PlayerId owner, Point from, GameTime now,
                                CellOverlay& out) const {
  const std::int64_t rubble = kGateRubbleReach;
  for (const Line& line : lines_) {
    const WorldObject* gate = world.find(line.gate);
    if (gate == nullptr || !line.has_axis || !gate_bars(world, *gate, owner)) continue;
    // A broken gate near the mover is a way in; farther off it is laid
    // like any other (`isqrt` against 1,500, inclusive).
    if (gate->state.health <= 0 && distance(from, gate->state.position) <= rubble) continue;
    if (gate_fully_open(*gate, now)) continue;
    for (const GateCell& cell : line.cells) out.add(cell.x, cell.y);
  }
}

}  // namespace imperivm::core::sim
