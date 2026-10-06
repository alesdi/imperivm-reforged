#include "imperivm/core/sim/glide.hpp"

#include <algorithm>

namespace imperivm::core::sim {

void TurnGlide::reset() noexcept {
  world_ = nullptr;
  turn_ = 0;
  now_time_ = 0;
  before_time_ = 0;
  now_.clear();
  before_.clear();
}

void TurnGlide::capture(const World& world, std::vector<Mark>& out) {
  out.clear();
  out.reserve(world.size());
  // In spawn order, which is ascending id: `find` searches it by halves.
  for (const WorldObject& slot : world.objects()) {
    if (slot.object == nullptr) continue;  // internal objects are never drawn
    Mark mark;
    mark.id = slot.id;
    mark.position = slot.state.position;
    mark.moving = slot.state.flags.has_active_path;
    mark.anim_slot = slot.object->anim.anim_slot;
    mark.elapsed = slot.object->anim.elapsed_ms;
    mark.cycle = slot.timeline.valid() ? slot.timeline.cycle() : 0;
    mark.repeat = slot.repeat;
    out.push_back(mark);
  }
}

const TurnGlide::Mark* TurnGlide::find(const std::vector<Mark>& marks, ObjectId id) noexcept {
  const auto it = std::lower_bound(marks.begin(), marks.end(), id,
                                   [](const Mark& mark, ObjectId key) { return mark.id < key; });
  return it != marks.end() && it->id == id ? &*it : nullptr;
}

void TurnGlide::observe(const World& world) {
  const bool same = world_ == &world;
  if (same && world.turns() == turn_ && world.time() == now_time_) return;
  if (same && world.turns() > turn_ && world.time() > now_time_) {
    // The turn end seen last is the one before now -- after a slow frame,
    // several turns before, and the glide spans them all.
    before_.swap(now_);
    before_time_ = now_time_;
  } else {
    before_.clear();
    before_time_ = world.time();
  }
  world_ = &world;
  turn_ = world.turns();
  now_time_ = world.time();
  capture(world, now_);
}

Point TurnGlide::position(const World& world, const WorldObject& slot, GameTime at) const noexcept {
  const Point here = slot.state.position;
  if (world_ != &world || world.time() != now_time_ || before_time_ >= now_time_) return here;
  const Mark* before = find(before_, slot.id);
  const Mark* now = find(now_, slot.id);
  if (before == nullptr || now == nullptr) return here;
  // Moved since the turn ended, by something that is not a turn.
  if (now->position != here) return here;
  if (!before->moving && !now->moving) return here;
  if (before->position == kHeldPosition || here == kHeldPosition) return here;
  if (slot.state.is_held()) return here;
  const GameTime span = now_time_ - before_time_;
  const std::int64_t dx = static_cast<std::int64_t>(here.x) - before->position.x;
  const std::int64_t dy = static_cast<std::int64_t>(here.y) - before->position.y;
  const std::int64_t reach = span * kGlideMaxSpeed;
  if (dx > reach || dx < -reach || dy > reach || dy < -reach) return here;
  const GameTime into = std::clamp(at, before_time_, now_time_) - before_time_;
  // Truncating towards zero per axis, as `FlightProgress::along` does.
  return Point{static_cast<std::int32_t>(before->position.x + dx * into / span),
               static_cast<std::int32_t>(before->position.y + dy * into / span)};
}

std::int32_t TurnGlide::anim_elapsed(const World& world, const WorldObject& slot,
                                     GameTime at) const noexcept {
  if (slot.object == nullptr) return 0;
  const std::int32_t elapsed = slot.object->anim.elapsed_ms;
  if (world_ != &world || world.time() != now_time_ || before_time_ >= now_time_) return elapsed;
  if (!slot.timeline.valid()) return elapsed;
  // A still cursor is drawn still, unless it is a held animation that ran to
  // its end -- which it may have done inside this turn, and is carried there.
  // A stopped loop never finished: its clock is frozen where it stopped.
  if (!slot.animating && (slot.repeat != AnimRepeat::hold || elapsed != slot.timeline.cycle())) {
    return elapsed;
  }
  const Mark* before = find(before_, slot.id);
  if (before == nullptr || before->anim_slot != slot.object->anim.anim_slot ||
      before->cycle != slot.timeline.cycle() || before->repeat != slot.repeat) {
    return elapsed;
  }
  // The same animation, run on without a restart: its clock then is its clock
  // now less the time between. Anything else -- started again, stopped and
  // held, swapped -- is drawn as the world has it.
  const GameTime span = now_time_ - before_time_;
  if (advance_elapsed(slot.timeline, before->elapsed, span, slot.repeat) != elapsed) return elapsed;
  const GameTime into = std::clamp(at, before_time_, now_time_) - before_time_;
  return advance_elapsed(slot.timeline, before->elapsed, into, slot.repeat);
}

}  // namespace imperivm::core::sim
