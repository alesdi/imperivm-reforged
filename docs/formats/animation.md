# The animation record

**Status:** read from `gbr.exe`; reproduced on `WorldObject::clock`
(`engine/core/include/imperivm/core/sim/world.hpp`, `AnimClock`) and the
entry points in `engine/core/src/sim/anim.cpp`, with the gaps listed at the end.

Every object in the original keeps a small record of what it is animating:
which animation plays, when it started, when its action moment and its end
fall, the transition played in front of it, and one timer. Scripts read the
record (`TimeToAnimFinish`, `TimeToActionMoment`, `GetAnim`, `IsInState`).
Commands write it (`StartAnim`, `PlayAnim`, the idle, the `Goto` family, a
swing). The timer's firing is how most of what an object does over time is
scheduled: a fidget's end, an idle's hold, a `PlayAnim` continuation and each
step of a walk. This page describes the record as the executable keeps it. It
describes a runtime behaviour, not a file. Nothing here is code from the
executable (`docs/legal.md`).

## The animations an entity declares

The entity loader keeps an entity's animations in an array of 28 entries,
indexed by the document's `<anim idx>` less one. An `idx` above 28 is refused
when the entity loads. Each entry holds the following fields:

* the state the animation starts from and the one it ends in (`startstate`
  and `endstate`, each less one);
* its frames;
* its declared `duration`;
* its `default_duration` (100 when absent);
* its `action_time`.

Every length a script sees is the declared `duration`, never the frame
strip's sum.

## The record

Per object:

* **The Anim word.** Below 0x1c it is the index of the animation that plays.
  From 0x1c up, it is the state the object rests in, plus 0x1c.
* **The facing.**
* **The playing slot**, -1 when nothing has been started.
* **The transition** in front of it.
* **The start**: the instant it was asked for, plus any delay.
* **The animation origin.**
* **The playback duration**: an override, or the declared `duration`.
* **The main start**: the start plus the transition's duration.
* **The action moment**: the main start plus `action_time` scaled by the
  playback duration over the declared one.
* **The action mode** at `[obj+0x130]`. This is the object's current
  activity: 0 none, 1 dying, 3 walking, 4 idling, 5 taunting, 6 a `PlayAnim`
  continuation, 8 put down.

The end is the main start plus the playback duration. The timer is message 27
on the object's own queue. "Not pending" (0x0053cfc0) is what `IsInState`
answers.

## StartAnimation (0x0053e6c0)

`StartAnimation(slot, origin, face, delay, duration)` runs these steps in
order:

1. **Off the map** (the object's position is the invalid point), it cancels
   the timer and answers 0. Nothing turns and nothing starts.
2. **An animation already playing is stopped** (0x0053d730). The timer is
   cancelled and the position is committed. The slot becomes -1 and the
   cached times are recomputed. If something was playing, the word becomes
   that animation's end state, so the object rests where it was going.
3. The slot, start, origin and duration are written. The object turns towards
   `face` when that is a valid point and differs from the current facing.
4. **The transition.** If the animation starts from the state the object
   rests in, there is none. Otherwise 0x005a8170 searches the array from
   index 0 for the first animation that leads from the resting state to the
   new animation's start state. When one is found, it plays first.
5. **The delay.** With `-1` the word becomes the slot at once, the start is
   one millisecond in the past, and **no timer is armed**. Such an animation
   never ends: `IsInState` is true at once, and the slot stays until
   something stops it. With any other delay the timer is armed at
   `now + delay`, and the object keeps its pose until then.
6. The cached times are computed (0x0053cf30).
7. **The answer** is what is left of the animation from now: the delay, then
   the transition, then the animation itself.

## The timer's firing (0x0053f6a0)

The timer's firing does one of five things, depending on the time:

* With no slot, it runs the object's end handler (`vtbl+0x54`).
* At the start, the word becomes the transition, and the timer is re-armed
  for the main start.
* At the main start, the word becomes the slot, the object moves to the
  animation's origin, and the timer is re-armed for the action moment.
* At the action moment, it runs `vtbl+0x58`, the object's "action": a blow
  lands, a step is counted. It is then re-armed for the end.
* At the end, the word becomes the end state plus 0x1c, the slot becomes -1,
  and the end handler runs.

**The end handler** of every object is 0x005b0f70:

* A dead object that is not yet put down gets mode 1 when its entity declares
  a death animation, and mode 8 otherwise.
* Mode 1 starts a death variant, one draw from the synchronised generator
  over `idx` 9 to 12 (0x005a81b0). It ends the command the object runs, when
  its command thread lives. The mode then becomes 8.
* Mode 6 is the `PlayAnim` continuation (0x005a76c0). It starts the animation
  the call named, the first time. The next time, it wakes the script and the
  mode becomes 0.
* Mode 8 destroys the object (`vtbl+0x94`).

**A unit's end handler** is 0x005d5b60. In order:

1. It does nothing while the unit is in a holder.
2. It turns a dead unit's mode to 1.
3. It runs the unit's own modes:
   * 2: its attack;
   * 3: its walk step (0x005d3b50);
   * 4 and 5: the idle activity (0x005d2ca0, `docs/formats/idle.md`).
4. It hands over to 0x005b0f70.

## SetState (0x0053e860)

`SetState(state, transition)` does nothing when the object already rests in
`state`. Without a transition, the slot becomes -1 and the word the state. The
timer is not touched. With one, it searches for an animation from the
resting state to `state`, and starts it when its index is above 0. Otherwise
it behaves as without a transition.

## The entry points

* **`StartAnim(slot, point)`** (0x005a9ce0) and **`StartDelayedAnim(slot,
  point, delay)`** (0x005a9da0) work alike. A point equal to the object's own
  position means "do not turn". They set the mode to 0. When the timer is not
  pending they run the end handler (a dead object's handler puts it down
  first). Then they call `StartAnimation(slot - 1, no origin, point, delay,
  0)`. `StartAnim`'s delay is 0.
* **`PlayAnim(slot, point)`** (0x005a9ba0) sets mode 6. When the timer is not
  pending, it arms the timer for 1 ms. It then sleeps with no timed wake, and
  the continuation does the rest.
* **`Flying::PlayAnim(slot, point, z)`** (0x0051bb10) checks its arguments
  and clamps the point to the map. It writes the flight's altitudes, sets
  mode 0 and stops what plays. It starts the slot (one lower than the
  document's) with the point as both the origin and the facing. It then
  suspends for the declared duration.
* **`TimeToAnimFinish`** (0x005a9f20) and **`TimeToActionMoment`**
  (0x005a9e70) answer 0 with no slot. Otherwise they answer the end, or the
  action moment, less now, and never less than 0.
* **`IsInState`** (0x005ad860) answers whether the timer is not pending.
* **`GetAnim`** answers the slot plus one, or 0 with no slot.

The `Goto` family's tail is described in `docs/formats/pathfinder.md` and
`run_goto`, and covers the walk's mode 3, the look-around and the arrival's
wait.

## What this engine keeps, and what it leaves out

The record is `AnimClock` on each object, hashed and saved (world version
37). It is kept with absolute times and lazily: a reader asks it at its own
millisecond, and an animation that has run out is read as ended without any
event having fired. The world settles it when a writer touches it. The cursor
the renderer draws follows the record at each turn's end.

Departures, each one for what the engine does not yet do:

* **The walk is not animation steps.** The original walks by one-shot steps
  on the timer (mode 3, 0x005d3b50). Each step carries its own duration
  override and the next point as its origin. So the timer is pending all the
  way, `IsInState` is false while a unit walks, and a `Goto` wakes its script
  at the step that arrives (bit 0x10000 of `[unit+0x194]`). Here the walk is
  the movement system's loop. Its start stops what the record plays, and a
  `Goto` waits its slice.
* **A dead object's later firings.** A corpse's death animation ends with
  the destroy (mode 8). Here the death sequence runs when a script's call
  reaches the end handler, and combat's death time removes the bodies it
  kills.
* **`Flying::PlayAnim` arrives at the call**, not at the main start.
* **The view after an end.** The original draws the end state's pose from
  the end. Here the strip plays to its last frame and holds it. This is a
  renderer-only difference.
* **A timer re-armed in the middle of an animation.** Arming replaces the
  pending message (0x00688240). The original's firing at the new time then
  runs whichever phase that time has reached. Here the animation is not run
  out by itself after such a re-arm: it plays until something stops it. No
  ported writer re-arms over a playing animation. The `Goto` family and
  `PlayAnim` arm only when the timer is not pending, and the idle holds only
  after its fidget has ended.
* **Order at one millisecond.** A firing due at a script's millisecond is
  taken as delivered before the script runs.
