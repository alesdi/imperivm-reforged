#pragma once

// Where the view draws a moving object between two turns. Presentation only:
// nothing in the simulation includes this, and nothing here writes the world.
//
// ## What the original does
//
// `gbr.exe` has one simulation clock and it moves every frame. A turn is a
// lockstep *window* -- `0x00528a80` opens it, `[clock+0x24]` to
// `[clock+0x28]`, worth the negotiated real length times the speed -- and
// the frame (`0x0051ea30`) asks `0x00528e40` where game time is now: the real
// clock's place inside the window's real span, scaled onto the window's game
// span and held below its end, so it reaches the window's last millisecond
// just as the next turn is due. `0x00528b40` then runs every scheduler up to
// that instant one millisecond at a time. So the original's game time crawls
// across a turn frame by frame instead of jumping a turn at the end of it.
//
// And what the frame draws is read off that clock. An object that moves is
// handed an animation with a destination (`0x0053e6c0`, called from some
// thirty places in the unit and building code: the start point, the
// destination written as its position at once, a start time and a duration),
// and what is drawn asks it where it is *now*: `GetCurrentPosition`
// (`0x0053d830`, slot `+0x40` of some thirty vtables) runs the point from the
// start to the destination by the game clock's place in that window, and the
// flying unit's visual update asks the same (`0x0051b272`). That the sprite
// frames step on the same clock is **inferred**: the clock is the only one a
// frame advances, and the drawing code that reads it was not followed.
//
// ## What this engine does instead, and why it may
//
// This engine runs a turn whole: `World::advance` takes the clock from one
// window's start to its end in one call, and the world then stands at the
// end for as long as the turn's real length lasts. Every position it holds is
// exact at the turn's end -- `MovementSystem` integrates nothing, see
// `sim/movement.hpp` -- and the view used to draw that, so a unit stood still
// for a turn and then jumped a turn's walk.
//
// The view now draws the world at the instant the original's clock would
// show: `at`, between the end of the turn before and the end of this one, by
// the real clock's place in the turn's real length (the application's to
// say). It does not run the simulation there -- it cannot, and it must not --
// it reads the two turn ends it has seen:
//
//   * **a position** runs in a straight line from where the object stood at
//     the end of the turn before to where it stands now, by `at`'s place
//     between the two ends. A route is a polyline walked at a constant rate,
//     so this is the original's answer exactly along a straight stretch, and
//     cuts a corner the route turned inside the turn by a chord's width;
//   * **an animation** runs its own clock: the elapsed time it had at the turn
//     before, carried on to `at` exactly as `World::run_turn` carries it
//     (`advance_elapsed`), so a walk cycle steps frame by frame instead of a
//     turn's worth at a time;
//   * **a bird on a leg** is drawn along it by that clock already
//     (`sim::flight_progress`); the clock is now `at`'s.
//
// Nothing glides that did not walk: an object glides only when it was on a
// route at one end or the other (`has_active_path`), stood on the map at both
// and moved no faster than `kGlideMaxSpeed`. A unit put down by a script, let
// out of a building or spawned this turn appears where it is, as before. An
// object something moved after the turn ended -- the editor's drag -- is where
// the world says.

#include <cstdint>
#include <vector>

#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

/// The fastest a glide may go, in world units per game millisecond. Faster is
/// a jump, not a walk: the quickest shipped walkers cover well under a third
/// of this (a `speed="150"` deer at factor 170 is 0.26 a millisecond).
inline constexpr std::int32_t kGlideMaxSpeed = 1;

/// The last two turn ends the view has seen, and where they put each object.
class TurnGlide {
 public:
  /// Take note of the world. Cheap when no turn has run since the last call;
  /// otherwise one pass over the objects. When exactly one turn -- or any
  /// number, after a slow frame -- has run on the same world since, the turn
  /// end seen last becomes the one before; anything else (a first look, a
  /// load, another world) forgets it, and the next turn glides from there.
  void observe(const World& world);
  /// Forget both turn ends.
  void reset() noexcept;

  /// The game time of the turn end before the world's: what `at` may go back
  /// to. The world's own time when there is none.
  [[nodiscard]] GameTime since() const noexcept { return before_time_; }

  /// Where `slot` is drawn at game time `at`, which is held to
  /// `[since(), world.time()]`. The world's own position when it does not
  /// glide (see the header).
  [[nodiscard]] Point position(const World& world, const WorldObject& slot, GameTime at) const noexcept;

  /// How far into its animation `slot` is at `at`: the elapsed time it had at
  /// the turn end before, carried on to `at` -- or its own elapsed time when
  /// the animation it plays now is not the one it played then, carried on.
  [[nodiscard]] std::int32_t anim_elapsed(const World& world, const WorldObject& slot,
                                          GameTime at) const noexcept;

 private:
  /// One object at one turn end.
  struct Mark {
    ObjectId id = kNoObject;
    Point position;
    bool moving = false;
    std::int32_t anim_slot = kNoAnim;
    std::int32_t elapsed = 0;
    std::int32_t cycle = 0;
    AnimRepeat repeat = AnimRepeat::loop;
  };

  static void capture(const World& world, std::vector<Mark>& out);
  [[nodiscard]] static const Mark* find(const std::vector<Mark>& marks, ObjectId id) noexcept;

  const World* world_ = nullptr;
  std::uint64_t turn_ = 0;
  GameTime now_time_ = 0;
  GameTime before_time_ = 0;
  std::vector<Mark> now_;
  std::vector<Mark> before_;
};

}  // namespace imperivm::core::sim
