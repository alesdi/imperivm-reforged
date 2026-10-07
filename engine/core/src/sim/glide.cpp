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
    mark.animating = slot.animating;
    mark.anim_slot = slot.object->anim.anim_slot;
    mark.elapsed = slot.object->anim.elapsed_ms;
    mark.cycle = slot.timeline.valid() ? slot.timeline.cycle() : 0;
    mark.repeat = slot.repeat;
    if (slot.object->is_a(NativeClass::flying_unit)) mark.flight = flight_pose(world, slot);
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

const TurnGlide::Mark* TurnGlide::walked(const World& world, const WorldObject& slot) const noexcept {
  if (world_ != &world || world.time() != now_time_ || before_time_ >= now_time_) return nullptr;
  const Point here = slot.state.position;
  const Mark* before = find(before_, slot.id);
  const Mark* now = find(now_, slot.id);
  if (before == nullptr || now == nullptr) return nullptr;
  // Moved since the turn ended, by something that is not a turn.
  if (now->position != here) return nullptr;
  if (!before->moving && !now->moving) return nullptr;
  if (before->position == kHeldPosition || here == kHeldPosition) return nullptr;
  if (slot.state.is_held()) return nullptr;
  const std::int64_t reach = (now_time_ - before_time_) * kGlideMaxSpeed;
  const std::int64_t dx = static_cast<std::int64_t>(here.x) - before->position.x;
  const std::int64_t dy = static_cast<std::int64_t>(here.y) - before->position.y;
  if (dx > reach || dx < -reach || dy > reach || dy < -reach) return nullptr;
  return before;
}

Point TurnGlide::position(const World& world, const WorldObject& slot, GameTime at) const noexcept {
  const Point here = slot.state.position;
  const Mark* before = walked(world, slot);
  if (before == nullptr) return here;
  const GameTime span = now_time_ - before_time_;
  const std::int64_t dx = static_cast<std::int64_t>(here.x) - before->position.x;
  const std::int64_t dy = static_cast<std::int64_t>(here.y) - before->position.y;
  const GameTime into = std::clamp(at, before_time_, now_time_) - before_time_;
  // Truncating towards zero per axis, as `FlightProgress::along` does.
  return Point{static_cast<std::int32_t>(before->position.x + dx * into / span),
               static_cast<std::int32_t>(before->position.y + dy * into / span)};
}

TurnGlide::Anim TurnGlide::anim(const World& world, const WorldObject& slot,
                                GameTime at) const noexcept {
  Anim out;
  if (slot.object == nullptr) return out;
  const std::int32_t elapsed = slot.object->anim.elapsed_ms;
  out.anim_slot = slot.object->anim.anim_slot;
  out.elapsed = elapsed;
  out.repeat = slot.repeat;
  out.flight = flight_pose(world, slot);
  if (world_ != &world || world.time() != now_time_ || before_time_ >= now_time_) return out;
  if (!slot.timeline.valid()) return out;
  const std::int32_t cycle = slot.timeline.cycle();
  // A still cursor is drawn still, unless it is a held animation that ran to
  // its end -- which it may have done inside this turn, and is carried there.
  // A stopped loop never finished: its clock is frozen where it stopped.
  if (!slot.animating && (slot.repeat != AnimRepeat::hold || elapsed != cycle)) return out;
  const GameTime span = now_time_ - before_time_;
  const GameTime when = std::clamp(at, before_time_, now_time_);
  const Mark* before = find(before_, slot.id);
  // The same animation, run on without a restart: its clock then is its clock
  // now less the time between.
  if (before != nullptr && before->anim_slot == out.anim_slot && before->cycle == cycle &&
      before->repeat == slot.repeat &&
      advance_elapsed(slot.timeline, before->elapsed, span, slot.repeat) == elapsed) {
    out.elapsed = advance_elapsed(slot.timeline, before->elapsed, when - before_time_, slot.repeat);
    return out;
  }
  // Begun since -- started again, or swapped. A held one that already ran out
  // has lost the time it began, and is drawn at its end.
  if (slot.repeat == AnimRepeat::hold && elapsed >= cycle) return out;
  // Otherwise its clock says when: `elapsed` before the turn's end, which is
  // where `World::run_turn` and the systems after it leave a cursor started
  // inside the turn (0 for one a script started, the time since for a swing
  // combat began at its blow). From then on it is drawn on that clock, as the
  // original's visual draws an animation from its own start time (0x0062a0e0
  // reads the global clock less the start 0x0053e6c0 recorded). **A loop
  // that wrapped since it began cannot say so**, and is taken to have begun
  // the remainder before the turn's end. Every restart the systems and the
  // scripts make is at the turn's end, after the cursors ran, so none has
  // wrapped; only one started between two turns -- by the editor, or a test
  // -- and shorter than the turn can be drawn a little early.
  const GameTime began = now_time_ - std::min<GameTime>(elapsed, span);
  if (when >= began) {
    out.elapsed = static_cast<std::int32_t>(elapsed - (now_time_ - when));
    return out;
  }
  // And before then, what the turn end before saw it playing, carried on to
  // `at` -- a bird the rest of the leg its script has since replaced. Not for
  // an object walked across the turn, though: a walk is started at the turn's
  // end only because `MovementSystem::play_locomotion` runs after the unit
  // has moved, and what it stood in before it set off would slide. It walks
  // on its walk's first frame, as it did.
  out.elapsed = 0;
  if (before == nullptr || before->anim_slot == kNoAnim || before->cycle <= 0) return out;
  if (walked(world, slot) != nullptr && slot.state.position != before->position) return out;
  out.earlier = true;
  out.anim_slot = before->anim_slot;
  out.repeat = before->repeat;
  out.flight = before->flight;
  out.elapsed = before->animating ? advance_elapsed(before->cycle, before->elapsed,
                                                    when - before_time_, before->repeat)
                                  : before->elapsed;
  return out;
}

std::int32_t TurnGlide::anim_elapsed(const World& world, const WorldObject& slot,
                                     GameTime at) const noexcept {
  const Anim drawn = anim(world, slot, at);
  return drawn.earlier ? 0 : drawn.elapsed;
}

}  // namespace imperivm::core::sim
