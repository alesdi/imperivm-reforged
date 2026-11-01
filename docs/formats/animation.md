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
38). **The timer fires.** Every pending timer is an entry on the scripts'
wheel, ordered by its time and its place in the arming order, which the
scripts' own re-arms share (`World::set_timer`, `script::TimerSource`); the
scheduler's pass sweeps both kinds in that one order, so a timer and a
script due on one millisecond run in the order they were armed, as the
original's bucket does (0x00688060, 0x006879a0). Each firing runs the phase
its time has reached (`World::fire_timer`): the start re-arms for the main
start; the main start moves the object to the origin, keeps where it came
from in the origin field and re-arms for the action moment; the action
moment re-arms for the end; the end rests the object in the end state, drops
the slot and runs the end handler (`anim_end_handler`). A wake the handler
sends (0x0069f8f0) re-arms the waiting script at that millisecond, behind
everything armed before it (`Scheduler::wake`).

The end handler is the original's, mode by mode: a held unit runs nothing; a
dead object takes mode 1 or 8; mode 1 draws a death variant, ends the
command and becomes 8; mode 8 destroys the object; modes 4 and 5 are the
idle's step and its wake (`docs/formats/idle.md`); mode 6 is `PlayAnim`'s
continuation and its wake. A flying unit's own step runs first (0x0051b450):
a climb or a glide leaves `z_from` at `z_to`, a landing clears the airborne
bit; `Flying::z` (0x0051b040) reads the pending timer and the main window.
A unit's death is the original's base death (0x005b16f0): the animation
stopped, the first state at once, the timer armed for 1 ms and mode 1, and
the corpse is destroyed when its death animation ends. A unit that dies in a
holder is destroyed on the spot (0x005db270).

The readers ask the record at their own millisecond. A script reads inside
the pass, where everything due before it has fired. A system reads ahead of
the pass (its turn runs first): an animation whose timer was to end it
before then reads as ended, and a writer at that time first fires whatever
of that object's timer was due before it, at its own time
(`World::settle_anim`). A world that no scheduler sweeps fires its timers at
the end of each turn, after the systems (`World::set_timers_swept`). The
cursor the renderer draws follows the record at each turn's end.

## The walk

A unit walks by one-shot steps on the timer, as the original does
(`MovementSystem::walk_step`, 0x005d3b50, run by the end handler in action
mode 3). An order lays its route and sets mode 3, arming the timer for 1 ms
when nothing is pending (`MovementSystem::start_walk`; the `Goto` family's
tail, `Stop`, `FormKeepMoving`). At each firing in mode 3 the step:

1. takes the default step length: the walk animation's `step` times 1,000
   over the class `speed` (0x005d38e0), shortened for a hero or a unit of
   one by its quick-march level (100 over 100 plus
   `SpeedPercentPerQuickMarchLevel` per level), then times 100 over the
   speed factor `[unit+0x15c]`;
2. asks the route for its next point and the step's length
   (`CVXPathRetry` 0x00419ee0): the sampled route's next point
   (`CVXSPath` 0x0041a740), let through by the cooperative step with its
   distance's share of the default length, held where the unit stands for
   50, 75 or 100 ms, or turned aside for the default length
   (`CVXPathCoop` 0x004172b0, 0x005d39c0); a march member's step timed to
   its station's deadline (0x00419b20);
3. on a point where the unit stands, arms the timer for the length (a hold,
   0x0053ced0); on any other, starts the walk animation (`[unit+0x1bc]`)
   for exactly that long, turned towards the point (the raw difference) and
   with the point as its origin, so the object is put there at the main
   start through `SetPos` and the move hook; then tells the route the step
   was taken, which commits it unless the cooperative step is holding;
4. with no step, deletes the route when no call waits on the walk, and
   otherwise wakes the waiting call now when the unit is arrived (bit
   0x10000 of `[unit+0x194]`, `MoveState::wake_on_arrival`, raised by the
   `Goto` family, `Stop` and `FormKeepMoving` as they suspend and dropped at
   their re-entry).

So the timer is pending all the way, `IsInState` is false while a unit
walks, a `Goto` wakes its script at the step that arrives, a `Stop` leaves
the step in flight to finish, and an `Idle` called right after waits it out.

Departures, each one for what the engine does not yet do:

* **An animation started with no origin does not move the object at its
  main start.** The original puts the object back where it stood at the
  call; here a writer outside the clock (`SetPos`, a holder's exit) may
  have moved it since, so the object is left where it is and the origin
  field takes that point. The walk is on the timer, so for it the two agree.
* **The attack's action moment** (mode 2's `vtbl+0x120`) is combat's own
  schedule; the action handler's other work is sound.
* **The command thread's wake at a death.** The base death cancels the
  command thread's pending wake (0x0069f890, 0x0069f8d0); here the command
  is ended 1 ms later, at mode 1, and a wake due in between runs.
* **The waiter.** The original wakes the object's command thread; here the
  activity wakes the script that called `Idle`, `Taunt` or `PlayAnim`
  (`AnimClock::waiter`), which is that thread for every shipped caller.
* **An animation declared 0 long.** The original divides by the declared
  length when it places the action moment (0x0053cf30, at 0x0053cfa9), so
  starting one is an integer-divide fault there. Here the action moment is
  the main start and the animation ends on the millisecond it starts; the
  idle's fidget does not start one (`docs/formats/idle.md`).
* **The view after an end.** The original draws the end state's pose from
  the end. Here the strip plays to its last frame and holds it. This is a
  renderer-only difference.
