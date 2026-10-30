# The idle and taunt activity

**Status:** read from `gbr.exe`; reproduced in `engine/core/src/sim/command.cpp`
(`idle_activity`, `idle_step`) with the gaps listed at the end.

`Unit::Idle(ms)` (0x005d6030), `Unit::Taunt(ms)` (0x005d60f0) and
`Ship::Idle()` (0x005c6ea0) do not wait by themselves. They hand the unit an
*activity* and let it spend the time, and the script wakes when the activity
says so. This page describes that activity (0x005d2ca0) and the three entry
points around it, as read from the executable, so the engine's model can be
checked against it. It is a runtime behaviour, not a file; nothing here is
code from the executable (`docs/legal.md`).

## The entry points

`Unit::Idle` and `Unit::Taunt` are one body with one constant changed: the
activity they set, 4 for an idle and 5 for a taunt.

1. **A held receiver** -- its position reads back as the invalid point, as a
   unit inside a building or a ship does -- writes `ms` into the script's wait
   cell and answers "run me again"; the re-entry pops the arguments and is
   done. A plain timed wait.
2. **Anything on the map** pops the arguments, sets the activity (the setter
   0x005a7680), drops the combat target (the handle at `[unit+0x1a8]` emptied
   and the attack count beside it zeroed, only when there is a target) and
   answers 3: done, but end the slice with no timed wake. The script sleeps
   until something wakes it, and that something is the activity.

The activity reads its budget from the script stack slot just above the
stack pointer, which is where `ms` was before the pop.

`Ship::Idle` has no argument. It writes its own budget into that slot -- the
length (`+0x4c`, the `<anim duration>` attribute) of the ship entity's idle
animation, array index 12 -- and then, **only when `[ship+0x1cc]` is zero**,
raises the force flag `[ship+0x1b8]`, sets activity 4 and drops the target.
It answers 3 either way. `[ship+0x1cc]` is the shipyard launch's flag: the
launch routine (0x005c6a60) raises it while it slides a new hull out of the
yard and drops it when the slide is over, so a ship told to idle mid-launch
gets no activity and its script waits for a wake that nothing sends.

`Unit::ForceIdle()` (0x005d88b0) writes 1 into `[unit+0x1b8]` and does
nothing else: no class test, no dead test. Its only reader is the activity.

## The setter

The activity setter stores the activity in `[unit+0x130]` and, **when the
unit's animation timer (timer id 0x1b) is not pending** (0x0053cfc0), runs
the unit's animation-end handler (`vtbl+0x54`) at once, which reaches the
activity dispatch (0x005d5b60). When the timer is pending -- an animation is
in flight, started by `StartAnimation` (0x0053e6c0) -- the activity waits:
the animation's end (0x0053f6a0) calls the same handler.

The dispatch does nothing while the unit is in a holder (`[unit+0x154]` is a
handle), turns a dead unit's activity into 1, and runs activities 4 and 5
through 0x005d2ca0.

## The activity, one step

Each run is one step, taken with the budget `B` left (the stack slot):

1. The base animation is array index 12 for an idle (activity 4) and 19 for
   a taunt (the entity document's `idx` 13 and 20). If the entity has none,
   go to step 4.
2. An idle picks a variant: `n` is 1, 2, 3 or 4 as the indices 13, 14 and
   15 after the base are present in turn (0x005a81b0), and the animation is
   `12 + rand(0, n - 1)` from the synchronised generator. A taunt draws
   nothing here.
3. If that animation's length `L` is at most `B`, then for `t` = 0, 100,
   200, ... while `t <= B - L`: when the unit's walk index (the short at
   `[unit+0x1bc]`) is 0, draw `rand(1, 15)` -- `rand(1, 3)` for a taunt --
   and **if it is 1, or the force flag is up**, drop the flag, start the
   animation `t` ms from now (0x0053e6c0 with `t` as its delay), take its
   answer off `B`, and end the step. The answer is `t` plus any transition
   animation's length plus `L`. The step after runs when that animation
   ends. A unit on another walk index draws nothing at all, and the flag is
   not read.
4. Otherwise, with budget left, hold the pose for all of it (arm timer 0x1b
   for `B`, 0x0053ced0) and set `B` to 0; the step after runs when the
   timer fires. With none left, wake the script at once (0x0069f8f0) and
   set the activity back to 0.

So the draw is spent before the flag is looked at, and the flag only turns a
"no" into a "yes". A flag the activity never draws for -- a budget shorter
than the animation, a unit carrying a bundle on another walk index -- stays
up for the next idle.

An idle on the map therefore costs at least its `ms`: the whole budget is
spent in fidgets and holds, plus whatever was left of an animation in flight
when it was called.

## What this engine models

* The steps are the call's re-entries. The first run drops the target and
  takes the first step; each step that waits answers "again in so many ms"
  and keeps the budget and a fidget still to start in the call's word; the
  step that wakes the script answers done.
* The force flag is `ObjectFlags::force_idle`, on the object, hashed and
  saved. `ForceIdle` and `Ship::Idle` raise it; the step that plays a fidget
  drops it.
* An animation in flight is a one-shot animation the object is playing
  (`AnimRepeat::hold`: an attack, a death, a fidget, anything a script
  started). The first step waits out what is left of it, counted from the
  script's millisecond. A state's own loop is a pose and is not in flight.
* A fidget starts on the script's millisecond: the cursor is brought forward
  by the part of the turn the world has already run, as the animation entry
  points do.

## What differs, and why

* **No transition animations.** The engine has none (see `sim/anim.hpp`), so
  a fidget costs `t + L`; the original adds the length of the transition
  from the unit's state into the fidget, when the entity declares one.
* **A walk cycle is not "in flight".** The engine's walk is a looping cursor,
  not a one-shot animation with an end, so an `Idle` called on a unit just
  stopped mid-stride takes its first step at once; whether the original's
  walk leaves timer 0x1b pending after a stop was not read.
* **A unit put into a holder mid-idle.** The original's dispatch runs no
  activity while the unit is held, so its script is never woken by it; here
  the call's re-entries go on and the call ends when the budget is spent.
* **The launch gate.** This engine does not run the shipyard launch, so
  `[ship+0x1cc]` is never up and `Ship::Idle` always takes its activity.

## What is still unknown

* Whether the original's walk animation leaves the animation timer pending
  after `Stop`, which decides whether the first step of an idle that follows
  a stop waits out a walk cycle.
