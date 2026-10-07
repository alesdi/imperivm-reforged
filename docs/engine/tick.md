# The tick model

**Companion:** [`architecture.md`](architecture.md) (why the core has no clock),
[`../formats/ent-xml.md`](../formats/ent-xml.md) (the animation data this drives)

**Code:** `engine/core/include/imperivm/core/sim/tick.hpp`,
`engine/core/include/imperivm/core/sim/world.hpp`, and the animation playback in
`engine/core/include/imperivm/core/game/entity.hpp`

Two things are settled here: **how the simulation advances time**, and **which of the two
durations in the animation data drives playback**. Both are argued from the retail install —
`DATA\CONST.INI` out of `data.pak`, the nine `Logs/*/desync.txt` dumps, and the 1,168
animations across 889 entity documents — and both name what the evidence does not reach.

---

## 1. Game time

### A game-time unit is a millisecond at 100% speed

`DATA\CONST.INI` declares

```ini
[VXTIME]
GameSpeed = 1000
```

and, further down, the speed the UI offers:

```ini
SlowSpeed = 700
NormalSpeed = 1000
FastSpeed = 1400
FastestSpeed = 2000
VariableSpeed = -1
```

so `GameSpeed` is a **per-mille rate**, not an enumeration: 1000 is real time. Every duration
in the same file is stated in milliseconds and several say so
(`TimeProductionForLoan = 180000 ;msec`), as are the `<anim duration>` and `<frame duration>`
attributes in the entity data. Nothing in the engine converts between two time bases; there
is one clock and the content is already denominated in it.

Every desync dump repeats the live value as `gamespeed`. Eight of the nine read 1000. The
ninth reads **999**, which is what a rate looks like and not what an enumeration looks like.

### Game time has unit resolution

The dumps record `gametimetickstart = gametime + 1` — the engine names a game time exactly
one unit after the current one — and `[HASHHISTORY]` lists command-pump boundaries at values
like 3751, 3894 and 3964. No coarser quantum divides them.

---

## 2. There is no fixed timestep

This is the part it is easiest to get wrong, and this document originally did.

The dump's `[GAMETIME]` block records the turn that is about to run. Across the nine dumps:

| `gamespeed` | turn length | dumps |
|---|---:|---:|
| 1000 | 800 | 4 |
| 1000 | 400 | 3 |
| 1000 | 200 | 1 |
| 999 | 799 | 1 |

and `[HASHHISTORY]`, which lists every pump boundary of the session, shows a **single session
renegotiating it mid-game**:

```
CVXCmdPump::Tick --- Time: 200, 600, 1000, 1400, 1800, 2200, 2600, 3000,   (Δ 400)
                           3460,                                            (Δ 460)
                           4260, 5060, 5860, …                              (Δ 800)
```

and in a 2025 log:

```
… 3000, 3460, 3751, 3964, 4164, 4364, 4564, 4764, …   (Δ 460, 291, 213, 200, 200, 200, 200)
```

This is a lockstep turn length tracking measured network latency. So **the quantum is
declared per turn and is part of world state**: two peers that disagree about the length of
turn *N* are running different simulations, which is exactly why the dump records it beside
the state hash.

Determinism does not come from the step being constant. It comes from it being **exact,
integral and agreed**. Game time is the running sum of the declared lengths, and integer
addition does not care where the interval was cut.

### `gamespeed` scales the turn

The one off-speed dump is decisive:

```
floor(800 × 999 / 1000) = 799
```

The turn is negotiated in **real** milliseconds — 800 ms is a plausible lockstep turn for
2003 dial-up — and converted into game time by the speed. `turn_length_from_real_ms` does
exactly that division, rounding down. This is the clearest single piece of evidence in the
data that `gamespeed` is a rate.

### The dumps' `Tick` counts turns

```
Tick == (number of "CVXCmdPump::Tick --- Time:" lines) + 1
```

exactly, in all nine dumps, from `Tick=0x2` with one recorded boundary to `Tick=0x2179` with
8,568. And `gametime` equals the last recorded boundary in all nine. So the dump is written
at the head of a turn that has not run yet, and **the pump turn is the only tick the original
has**. There is no sub-turn simulation step in it, and there is none here.

The turn window, all three relations exact in all nine dumps:

```
gametimetickstart = gametime + 1
gametimetickend   = gametimetickstart + turn_length
gametime          = the boundary the turn began at
```

`Turn` in `sim/tick.hpp` carries those four fields under those names.

### What this looks like in code

```cpp
World world;                       // opens at 400 units, every dump's first period
world.advance();                   // one turn at the current length
world.advance(800);                // one turn of a declared length; 800 becomes current
world.advance(lengths);            // a recorded sequence, e.g. from a replay
```

**In a networked match the speed changes by agreement.** A `set_speed` order
(`sim/netcmds.hpp`) agreed for turn `t` writes the clock's `game_speed` as it is
applied, and the negotiator converts turns `t + 1` onwards at it: the original's
clock (0x00528a80) converts with the speed its command wrote while the turn ran.

### Where the 999 came from

The dump's 999 is not a negotiated speed and not a setting anybody chose. It is the
options screen's round trip, read in `gbr.exe`:

* **A match starts at `NormalSpeed`.** With the settings' `gamespeed` at −1 (variable), the
  start (0x0052683b) reads CONST.INI's `[GamePlay] NormalSpeed`, 1000, clamps it to
  1..100000 and writes the clock's speed. Any other `gamespeed` fixes the speed instead and
  sets the flag `SetSpeed` checks.
* **It writes the options' position back from that speed** (0x005268e7 → 0x006e71e0):
  `(speed − 700) × 100 / 2301`, truncating. 1000 gives **13**, which is why the shipped
  `Settings.ini` says `GameSpeed=13`: it is `NormalSpeed` on its way back.
* **OK on the options screen sends the position forward** as a `CVXCmdSetSpeed`
  (0x006e7ff0), moved or not, in any match whose speed is variable:
  `700 + option × 2301 / 100`, truncating. 13 gives **999**.

So a player who opened the options during a match and pressed OK without touching the
slider moved the speed from 1000 to 999, and the next 800 ms turn was 799 units long.
`sim::game_speed_from_option` and `sim::option_from_game_speed` (`sim/tick.hpp`) are the two
directions, and the only place either formula is written; the options screen, the start of
a match and a networked peer's `set_speed` all go through them. `test_gamespeed.cpp` pins
13 → 999 → 799 and 1000 → 13, and `tests/test_corpus_app_speed.py` reproduces it in the
app: a match opens at 1000, OK posts 999, and the turn after the one it was applied with is
799 units.

**In the app's single-player match** the speed is an order too, through `sim::LocalOrders`:
posted orders wait for the next turn, whose length is converted *before* they are applied,
exactly as the negotiator converts turn `t` before `t`'s orders run. The real length of an
unnetworked turn is `--turn-interval` (100 ms by default, **this engine's number**, since
no single-player turn length was read), converted at the clock's speed, so a match at 1000
runs in real time. `--turn-length N` still forces N units a turn for a scripted
fast-forward. **Inferred:** that a single-player match's speed is always variable; the
settings' `gamespeed` was read for the lobby's record (−1 by default), and the
single-player screens that fill it were not followed.

The app used to read `GameSpeed` as a 0..25 turn-interval knob with 13 as normal, and ran
800 game units every 100 ms — eight times real time. `GameSpeed` no longer sets anything
at the start of a match, as in the original.

`TickConfig` holds `turn_length` and `game_speed`, and **both are world state**. Nothing in
the core asks what time it is; the platform decides when to call `advance` and what the next
length should be, and `turn_length_from_real_ms` is the only thing that knows the two scales
are related.

### What is not pinned

The dumps sample only at turn boundaries, so they say nothing about whether the original
engine subdivided a turn internally — whether a unit crossing an 800-unit turn moved in one
jump or in eight steps of 100. The dumps do not, and `gbr.exe` does: a unit walks a route
in **steps**, each handed out with its own duration by the path object it owns (the
sampled path `CVXSPath`, `0x0041a740`, wrapped by the cooperative layer `CVXPathCoop`,
`0x004172b0`), and a unit blocked by another is told to stand for 50, 75 or 100 ms. So
the original's movement is event-timed inside the turn, and whether one unit's step is
free depends on where the others are *at that instant*.

Movement is therefore the one system here that subdivides a turn, and it takes no step of
its own to do it. `MovementSystem::advance` plays a turn out as a queue of decisions
ordered by `(game time, id)`; each is due when a unit's exact progress accumulator reaches
the end of its step (a ceiling division) or when its hold runs out (a whole number of
milliseconds). Neither depends on where a turn boundary falls, so the property the next
section rests on still holds: `[800]` and `[400, 400]` take the same decisions at the same
instants, draw the same numbers from the generator in the same order, and leave every unit
on the same coordinate. `test_avoidance.cpp` asserts it over a crowd that blocks itself
softly and hardly, cut four ways. See `docs/engine/state-vector.md` and
`sim/avoidance.hpp` for what a decision is.

Every other system still advances once per turn. A later system that needs sub-turn
resolution should do what movement does — derive its instants from its own exact state —
rather than invent a step length.

### What is drawn between two turns

**The original's game clock moves every frame; this engine's moves a turn at a time.** Read
in `gbr.exe`: the frame (`0x0051ea30`) hands the real clock to `0x00528e40`, which puts game
time the real clock's share of the way across the turn's window — `[clock+0x24]` to
`[clock+0x28]`, opened by `0x00528a80` — held below the window's end, and `0x00528b40` runs
the schedulers up to it one millisecond at a time. (Both frame routines, `0x0051ea30` and the
one at `0x004ccc60`, open the next window themselves with **150 ms** of real time when the
real clock reaches the last one's end, `0x0051ea97` and `0x004cccca`; the networked transport
opens it with the agreed length, `0x00407fe7`. So the unnetworked turn this section calls
"not read" above looks to be 150 ms, not this engine's 100. Which frame routine a
single-player match runs was not followed, and the app's default is left alone here.) A
moving object is drawn where its animation has it at that instant:
`GetCurrentPosition` (`0x0053d830`, slot `+0x40` of some thirty vtables) runs the point from
where the animation started to its destination by the clock's place in the animation's
window, and the flying visual (`0x0051b272`) asks the same. So between two turns the
original's units walk frame by frame, and its turn boundaries are invisible.

Here `World::advance` takes the clock to the window's end in one call, and the world then
stands there for the turn's real length. The view used to draw that, so every unit stood for
a turn and jumped a turn's walk — 100 ms at a time in a single-player match, up to 800 in a
networked one. **The view now draws the instant the original's clock would show**, between the
turn end before and the world's, by the real clock's share of the turn (`sim/glide.hpp`,
`WorldView::set_turn_fraction`). It cannot run the simulation there, and must not; it reads
the two turn ends it has seen. A position runs in a straight line between them — the
original's answer along a straight stretch of a route, a chord across a corner turned inside
the turn — and an animation's clock is carried on from the turn end before exactly as
`run_turn` carries it, so a walk cycle steps frame by frame; a bird's leg and lift and a
portcullis are read at the same instant. Only what walked glides: on a route at one end or the
other, on the map at both, and slower than one world unit a millisecond. Rings, bars, shadows
and both picks are placed from the same anchor. A paused or held clock (a menu, `turn:N`)
draws the world as it stands until the next turn runs. Nothing the view derives reaches the
world: `test_glide.cpp` holds the hash, and the app is held to `imrun`'s.

### The determinism contract is narrower than it looks

The dumps carry eight hash channels. Across all nine:

| channel | non-zero in |
|---|---:|
| `slots` | 9 |
| `threads` | 9 |
| `netcmds` | 7 |
| `extrahash` | 1 |
| `pathfinder` | **0** |
| `exploration` | **0** |
| `scriptstate` | **0** |
| `aihash` | **0** |

Pathfinding, fog of war, script state and AI were deliberately excluded from the shipped
determinism contract. That is a constraint to respect, not a gap to fill: those subsystems
must never feed back into hashed state, or a conformance replay will diverge on something the
original never compared. `World::state_hash` is the `slots` channel and covers object state
only.

---

## 3. Animation playback

An animation is a strip of per-row holds played over a sprite sheet in one of three orders.
Three separate pieces of data meet, and each contributes exactly one thing:

| piece | contributes | why not the others |
|---|---|---|
| `<frame duration>` strip | the per-row hold | the only per-frame timing in the format |
| the `.rle.mmp` frame table | the row count | the XML's `rows` is wrong for 430 of 4,033 images |
| `<image remaping>` | the step cycle | `forward`/`reverse` have `rows` steps, `pingpong` has `2·rows − 2` |

`AnimTimeline` is that join. It exposes `steps()`, `cycle()`, `hold_of_step()`,
`row_of_step()` and `sample(elapsed, repeat)`.

### Playback is a pure function of elapsed time

Nothing accumulates a frame index. `sample()` walks the cumulative holds from the start of the
cycle every time, and a cursor stores only its elapsed time, reduced modulo the cycle. That is
what makes the turn length unobservable: `(a + b) mod c` does not depend on where the sum was
split, so a sequence of turns leaves a cursor exactly where the single turn of their sum
would. `test_tick.cpp` asserts this over seven different partitions of a 1,400-unit interval,
including `[400, 800, 200]`.

### Pingpong

A hold belongs to the **row**, not to the step, so row 3 of a pingpong sheet is held for the
same time going up as coming back down, and the cycle is *not* twice the strip sum — the two
endpoints are visited once each:

```
rows = 4, holds = 10 20 40 80
steps        0   1   2   3   4   5
row          0   1   2   3   2   1
hold        10  20  40  80  40  20     cycle = 210, not 300
```

**This is an inference.** The data gives one duration per row and never says what the return
leg costs; indexing the strip by *step* instead would need entries for steps `rows`…`2·rows−3`
that do not exist. It is also where an off-by-one hides: get the period wrong and an endpoint
is held for two steps, which is invisible in a still frame and obvious in a sequence, so
`test_anim.cpp` checks thirteen consecutive steps rather than one.

### Loop or hold

**The data does not encode this.** `startstate == endstate` looks like the discriminator and
is not — it holds for all 154 `die` animations, which plainly do not loop. What the data does
support is a split by *how the animation was reached*:

* an animation named by `<state anim_idx>` is the pose's own loop. 181 states name one, and
  172 of those name slot 13 (idle) or slot 5 (attack) — slots whose 289 animations are
  same-state without exception;
* an animation started by a script's `PlayAnim` runs once and hands control back.

So `World::enter_state` loops and `World::play_anim` defaults to holding, and `AnimRepeat` is
an explicit parameter rather than something guessed from the file.

### Slots

Animations are addressed by **numeric slot**, not by name or position: 13 is idle, 9 die, 5
attack, 19 toattack. `.vs` code calls `PlayAnim(0, …)` and `PlayAnim(16, …)` and **no entity
declares either**, so a lookup miss is ordinary. `Entity::anim`, `timeline_for_slot`,
`World::play_anim` and `World::has_anim` all return null / false / an invalid timeline rather
than an error, and `play_anim` leaves the cursor untouched on a miss so that a failed script
call cannot freeze a unit mid-walk.

---

## 4. The duration decision

`<anim duration>` disagrees with the sum of the `<frame duration>` strip in **127 of 1,168**
retail animations, across **86 of the 310** entities that declare any animation. Which one
the engine honours was open. It is now decided:

> **Playback follows the frame strip. `@duration` is carried and ignored.**

### The evidence

**1. `@duration` cannot produce the schedule.** 294 of the 1,168 animations have a
non-uniform strip — `HEROGRAVE`'s death is `100, 300, 700, 2000, 4000, 4000`; the swaying
trees are `140, 200, 300, 140, 400, 500, …`. A single total cannot say when each of those
frames ends. The most `@duration` could do is *rescale* the strip, which is the hypothesis the
next point tests.

**2. `action_time` is measured on the un-rescaled strip.** `action_time` is the offset at
which the gameplay effect fires — the arrow leaves the bow — and it is the only other absolute
time offset in the format. 221 animations carry a non-zero one. Among the 16 that *also*
disagree about duration:

| | on a raw strip boundary | on a strip rescaled to `@duration` |
|---|---:|---:|
| all 16 | 5 | **0** |
| the 5 unit animations among them | **5** | **0** |

The five hits are exact and unmissable: `action_time` 1122 = 17 × 66, 1056 = 16 × 66,
528 = 8 × 66, 330 = 5 × 66. The eleven misses are all `action_time="300"` on decor fire loops
— a round hand-typed number that is not aligned to anything, and 26% of even the *agreeing*
animations miss their boundaries too (151 of 205 hit). If the engine normalised the strip to
`@duration`, `action_time` would have to be in normalised units and those five exact multiples
of the frame time would be coincidences.

**3. `@duration` is derived and sometimes stale.** In **44** of the 127 disagreements it is
exactly `floor(rows × 1000 / 15)` while the strip is `rows × 66` — the exporter wrote the
real-valued 15 fps total and the truncated integer per-frame value. `EBALISTA`'s attack:
13 × 66 = 858 in the strip, `duration="866"` = `floor(13 × 66.67)`. Both encode "15 fps"; the
strip is the one a runtime can step. The distortion is under 1.5% and rescaling to `@duration`
would reintroduce non-integer frame times, which is a determinism hazard for no fidelity gain.

**4. The strip needs no division.** Rescaling means a divide per frame boundary, and every
divide in a lockstep simulation is a rounding rule that has to be identical everywhere.

### Where the decision could be wrong

Stated plainly, because it is not a clean sweep.

**The stale-strip family.** 85 animations have a strip *longer* than the sheet has rows: the
temple fires list eleven holds over a six-row sheet. The implementation takes the row count
from the sheet, which is authoritative, so the extra entries are simply never played — and for
**5 of those** the resulting length then equals `@duration` exactly (six rows × 200 = 1200,
which is what `BTEMPLEOFTHOR\TEMPLEFIRE1` declares; `ETEMPLEOFOSIRISFIRE` is five × 200 =
1000, likewise). That is a real, if small, argument that `@duration` was regenerated after the
sheet was re-exported and the strip was not. It does not change the decision — the sheet still
wins, and honouring `@duration` for the total would not tell you *which* frames to drop — but
it is the family where a different reading is most defensible. `TBLACKSMITHFIRE` breaks even
that pattern (declares 1400 against six rows × 200 = 1200).

**The `test_tree` family.** 42 identical copies under `MAPOBJECTS\DECORS` declare
`duration="6200"` against a 13-entry strip summing to 3,460 over a **six-row** sheet, with no
zero bookends. Neither reading reconciles those numbers and the animation is called
`test_tree`. They are treated as ordinary data and they animate; whether they animate as the
artist meant is unknowable from the file.

**`duration="0"`.** 7 animations declare it, all on ships. `default_duration` equals the strip
sum in 4 of them and is a per-frame value in the other 3, so it is not a reliable fallback
either. Under the strip reading none of this matters, which is a mild point in its favour.

**Short strips.** 46 animations have *fewer* holds than the sheet has rows. The missing rows
are played, taking the last stated hold — or `default_duration` when there is no last hold.
**Both are guesses.** The data does not say what the engine does with a short strip, and this
is the least evidenced decision on the page.

### What the corpus does after the decision

Measured by running the core readers over a retail install:

```
889 entity documents, all parsed
310 declare at least one animation; 579 declare none
310 of 310 have every animation build a valid, advancing timeline   (0 broken)
1,168 animations, 1,168 valid timelines
127 animations (86 entities) disagree about @duration and are played from the strip
 85 have a strip longer than the sheet has rows   (truncated to the sheet)
 46 have a strip shorter                          (padded with the last hold)
```

---

## 5. What is still unknown

* **How the original orders two units' steps that fall on the same instant.** Its movement
  is event-timed (see "What is not pinned"); this engine breaks the tie by object id. The
  dumps only ever sample at boundaries, so the order of two events inside a turn is not
  observable in them.
* **`default_duration`.** Present on every animation, non-zero on 538, equal to the total in
  some cases and to a single frame's hold in others. Nothing here depends on it except the
  short-strip fallback.
* **The correct hold for a row a short strip does not cover.** See above.
* **Whether the stale-strip family should honour `@duration` instead.** 85 animations, all
  decor; the argument is set out above and is not settled.
* **The pingpong return-leg timing.** Inference, not measurement.
* **How the turn length is negotiated.** The dumps show what it became, never why. A
  conformance replay must drive the sequence from the recording rather than regenerate it,
  which is why `advance` takes a length.
