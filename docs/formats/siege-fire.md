# Siege fire: how catapults and towers shoot

What `gbr.exe` does when a siege engine or a tower fires, read from the
executable and the shipped scripts, and what this engine reproduces. Addresses
are `gbr.exe`'s, cited so each claim can be checked; nothing here is the
executable's code (`docs/legal.md`, rule 2). The implementation is
`engine/core/src/sim/siege_fire.cpp` and the catapult and tower arms of the
host entry points in `sim/combat.cpp` and `sim/world_host.cpp`.

## The shape of it: the scripts pull the trigger

Nothing in the original fires a siege engine or a tower on a timer of its
own. Every shot is a host call made by a shipped script:

| who | its scripts | the call that shoots |
|---|---|---|
| a catapult (`GCatapult`, `ICatapult`, `CCatapult`, `ECatapult`, `Catapult`) | `CATAPULT_IDLE.VS` queues `autofire` once the engine is built; `CATAPULT_AUTOFIRE.VS`, `CATAPULT_ATTACK.VS`, `CATAPULT_ATTACK_GROUND.VS` | `Catapult::Attack(point)` (0x004e2d10) |
| an outpost's guns (`FakeTower`) | `TOWER_GUARD.VS`, the class's `idle`; `TOWER_ATTACK.VS` sets its target, `OUTPOST_ATTACK.VS` hands every tower of the outpost an `attack` | `Building::Attack(obj)` (0x004df650) |
| the Roman tower's guns (`CatapultTower`, on `RCatapult`) | `CATAPULT_TOWER_GUARD.VS`, the class's `idle`; `RCATAPULT_AUTOFIRE.VS` and `RCATAPULT_ATTACK.VS` only aim | `Building::Attack(obj)` |

So a catapult with no crew, a tower in an empty outpost and a catapult whose
script has stopped do not shoot, and how often anything shoots is what those
scripts and the two entry points' waits make it.

## The catapult record

`CVXCatapult` (constructor 0x004e1e10) carries, by the names its persist
stream gives them (0x004e1c90):

| offset | name | what it is |
|---|---|---|
| `+0x208` | `Built` | `Catapult::IsBuilt` / `SetBuilt` |
| `+0x210` | `Attack` | the point of the shot asked for |
| `+0x218` | `Rotate` | the point of the turn asked for |
| `+0x220` | `RotateFirstRun` | the turn has not started yet |
| `+0x224` | `AttackFirstRun` | the shot's animation has not started yet |
| `+0x228` | `target` | a 16-bit handle, 0xffff from the constructor |
| `+0x22c` | `HasTowers` | at least one `CatapultTower` was placed for it |

`+0x228` is a **slot**, not an engagement. `Catapult::SetTarget(obj)`
(0x004e3600) stores the handle of the argument (0xffff for one that does not
resolve) without testing anything; `ClearTarget` (0x004e3710) stores 0xffff;
`GetCurrentTarget` (0x004e3500) pushes the slot back. Both writers then, **only
while `HasTowers` is set**, walk the roll of the engine's own settlement
(`[cat+0x148]`, the member deque at `+0x68`) and copy the slot into
`[bld+0x1fc]` -- `Building::GetUITarget` -- of every member that is an heir of
`CatapultTower`. `ClearTowerTarget` (0x004e37f0, the whole of
`CATAPULT_STOP.VS`) does only that walk, writing 0xffff, under the same gate.
The siege planner writes the slot directly (0x004389d5), with no walk.

## The fake towers

A `CVXOutpost` and a `CVXCatapult` place invisible gun objects on their own
entity's **type-7 points**. The construction virtual (`vtbl+0x2c`,
0x004e1b30, shared by both) posts message 0x3d to the object unless a saved
game is being restored (`[0x00a8734c]`); the message handler (`vtbl+0xc`,
0x005640d0) calls `vtbl+0x12c`:

* `CVXOutpost` (0x005641d0): a `FakeTower` at every type-7 point;
* `CVXCatapult` (0x004e38d0): a `CatapultTower` at every type-7 point, and
  `HasTowers` is raised for each one placed.

Each point is an offset in screen pixels from the object's projected position
(the same round trip `docs/formats/ent-xml.md` describes for the other
points); a point that lands outside the map rectangle is skipped. Each tower
is created with the object's owner and added to the object's own settlement,
which is how the walks above and the fire rate below find it.

On the shipped data: every race outpost carries three or four type-7 points;
of the six catapult entities only `RCatapult`'s carries one. So the Roman
"tower" is a catapult that never fires itself and shoots arrows from one
`CatapultTower`, and every outpost shoots arrows from three or four
`FakeTower`s. The base classes declare it: `FakeTower` is damage 40 pierce,
range 600, `min_range` 100, sight 600, `attack_delay` 400, `projectile_class`
`Arrow`; `CatapultTower` narrows it to range 800, `min_range` 150, sight 0,
and shoots `Fire_Arrow` at buildings.

**This engine** raises `ObjectFlags::towers_posted` where the original posts
the message, and places the towers at the start of the next combat step
(`post_fake_towers`), which is also where an object a script placed in the
middle of a turn is first seen. The screen round trip is
`class_point_in_world`'s, which refuses a point the original would clamp onto
the screen edge first; no shipped outpost stands near enough to an edge for
the two to differ.

## A catapult's shot

`Catapult::Attack(point)` (0x004e2d10, a suspending entry point) refuses --
returns at once, nothing pushed -- when the point is the engine's own
position or `HasTowers` is set. Otherwise it stores the point at `+0x210`,
raises `AttackFirstRun`, sets action state 2 (0x005a7680) and answers 3:
suspend until the action state clears.

The state machine is the animation's. With nothing playing, setting the
state runs the animation-finished virtual (`vtbl+0x54`, 0x004e2c00 ->
0x004e1b80) at once, which, `AttackFirstRun` set, starts the entity's attack
animation (zero-based slot 4, `<anim idx="5">`) and lowers the flag. At that
animation's `action_time` the frame virtual (`vtbl+0x58`, 0x004e2c30 ->
0x004e22a0) creates the shot (0x004e47c0), a `CVXCatapultShot` of the class's
`projectile_class` flying from one of the entity's type-3 points (picked by
the engine's facing) to the stored point. When the animation ends the
finished virtual runs again with the flag down, releases the waiting script
(0x0069f8f0) and clears the action state. So one `Attack` is one shot, and
the script resumes when the throw is over.

**The shot lands on the ground, not on a target.** Its impact (0x004e4520 ->
0x005ac640 -> 0x005a9770 -> 0x005a8e10) walks every unit and building in the
box `splash_radius + 512` about the point, inside the circle of that radius,
with no test of owner, side or visibility -- a catapult hurts its own side --
and for each (a building only where its settlement's word at `+0x40` is set,
not identified and treated as always set here) takes the surface distance
`d = isqrt(|point - centre|^2) - radius` of the victim's class. Where `d` is
under the catapult class's `splash_radius` `R`, the blow is the class's
`damage` `D` for `d < 1` and `D * (R - d)^2 / R^2` otherwise, then scaled by
the counter percent of the catapult's class against the victim's (0x004da370)
and divided by 100, and applied through the victim's damage virtual
(`vtbl+0xa8`) -- the one `Obj::Damage` uses, with no armour, no level
difference, no experience and nobody credited. A building also gets fire
marks there (0x004e0b90, presentation).

**Where the shot leaves.** 0x004e22a0 collects the entity's type-3 points
(0x0053cee0 -> 0x005fd290: raw offsets, table order, at most 64) and picks
the one at `(n / 2 + sector) % n`, where `n` is the entity's variation count
(`[entity+0x284]`) and `sector` is 0x005f8530 of the heading to the stored
point with its y negated. That is the same expression 0x0053db30 sets the
engine's drawn column with, so the shot leaves from the point the sheet
draws for the way the engine faces. The offset is added to the engine's
position as it stands, with no screen round trip. The shipped firing
catapults have eight variations and eight type-3 points, a ring about 120
units north of the centre (`Catapult`: `(0, -159)` ... `(-47, -150)`).

**How it flies.** The shot (0x004e47c0) takes the launch point, the aim and
the catapult class's `shot_tan` (`[class+0xa08]`, read times 0.001) and
`shot_height` (`[class+0xa0c]`). The ground end of its track is the launch
point moved `shot_height` south, the ground under a point drawn that high;
its length `S` is the float distance from there to the aim, and the arc's
height over the track is a quadratic in the distance travelled, fitted from
the two heights and the slope. Placing the shot (`vtbl+0x2c`, 0x004e4160)
posts message `0x33` 1 ms ahead (0x00688240). Its handler (0x004e4650) adds
10.0 (`[0x007edb08]`) to the distance travelled; while that is still at
most `S` it re-posts itself 100 ms ahead (0x00688220) and moves the shot;
past `S` it lands (0x004e4520) and the shot is deleted. So the shot lands
`1 + 100 * floor(S / 10)` ms after it leaves -- 100 units a second, about
eight seconds across a catapult's 800-unit range -- and anything that walks
out of the splash meanwhile is missed.

**This engine** launches the shot when `Attack` is called, timed to leave at
the attack animation's `action_time` (`Projectile::launched` in the future),
from `catapult_shot_origin` -- the type-3 point above, with
`facing_column` standing for 0x005f8530 as it does for every drawn column --
and suspends the script for the animation's `duration`. The flight is the
original's, `catapult_shot_flight` of `isqrt` of the squared track: the
float root differs from it only within a ten-thousandth of a multiple of
ten. The shot's position interpolates the ground track over the flight; the
arc is the renderer's.

## A catapult's other entry points

* `AttackWait` (0x004e32b0): `600000 / (CatapultBaseFireRate + (crew - 1) *
  CatapultAddFireRate)` milliseconds, less the attack animation's `duration`
  (the record `[entity+0x147c]`, whose `+0x4c` is the `duration` attribute --
  2,234 ms on the four catapults, 866 on the Egyptian ballista), floored at 0; no crew answers 0. The
  term the original adds for an action state 4..7 still in progress cannot
  arise here, because `Attack` suspends to the end of its throw and `RotateTo`
  turns at once.
* `InRange(point)` (0x004e3230 -> 0x005a7940): `isqrt(|pos - point|^2) -
  radius` between `min_range` and `range` inclusive, with no escape for a
  `min_range` under 1.
* `BestTarget` (0x004e2f60 -> 0x004e2760 -> 0x004e2510): over the box and
  circle `sight + 512`, the **buildings** the engine may attack (`vtbl+0xb4`)
  whose surface distance is within `sight` and inside `[min_range, range]`,
  not heirs of `Wall`, and not nearly destroyed (damage tier 3 and under a
  twentieth of the maximum) unless the building is its settlement's centre
  with somebody in its holder; the highest `target_priority` wins, the first
  met keeping a tie. **Units** are kept on the side by the same range tests
  and the same priority rule, and answered only when no building was found
  **and** `HasTowers` is set. So a catapult's autofire never picks a unit; the
  Roman tower's does.
* `RotateTo(point)` (0x004e2c50): refused for `HasTowers` as `Attack` is.

## A tower's shot

`Building::Attack(obj)` (0x004df650, suspending) is called with the tower as
the receiver.

1. An invalid receiver or argument prints and answers false.
2. **The settlement's fire rate** (0x005c2fb0, over the tower's settlement
   `[bld+0x148]`): walking the settlement's holder (`+0x5e`), count the units
   that are heirs of `Military` and the others, the others capped at 10, and
   answer `isqrt(military + others) * BaseFireRate / 100` -- `BaseFireRate`
   is 100 in `CONST.INI`, so the rate is `isqrt` of the garrison in shots per
   second. **No garrison, no fire:** a rate of 0 answers false.
3. The tower must be able to hit the target (`vtbl+0xb4`, the
   `IsValidTarget` virtual) and the target must be in range (0x005a7800: the
   surface distance `isqrt(d^2) - both radii` within `range`, and at or past
   `min_range` when that is 1 or more). Either failing answers false.
4. **The tower's own delay**: when less than the class's `attack_delay` has
   passed since the tower's last shot (`[bld+0x200]`), the call suspends for
   what is left and is made again.
5. **The settlement's cadence** (0x005c3050): when less than `1000 / rate`
   milliseconds have passed since the last shot of any tower of the settlement
   (`[settlement+0x98]`, reset if it lies in the future), the call suspends
   for what is left and is made again.
6. Otherwise both clocks are stamped with now (0x005c1050), an arrow of the
   class's `building_projectile_class` (for a building target) or
   `projectile_class` (for anything else) is created at the tower aimed at the
   target (0x004d9740), the target's squad is told it was attacked by the
   tower (0x0041e6c0), and the call answers **true without suspending**.

`Building::BestTarget` (0x004dcc60 -> 0x004dc1e0 -> 0x004dbb20) sweeps the
box `±sight` about the tower for **units** the tower may attack, whose class
`target_factor` is not negative, that are not hidden, and whose surface
distance is within `sight`. The first such unit is taken, and sets a "low
danger" mark when its danger counter (`[unit+0x1b0]`, 0..6, decaying one a
second; `docs/formats/target-score.md`) is under 4. After that a unit
nearer than `min_range` is skipped, one within `range` is taken and **ends
the sweep**, and one beyond `range` is skipped when its danger is over 3 and,
while the mark stands, when it is farther than the best so far; otherwise it
replaces it. The unit answered has its danger raised by one (0x005d2850).
This engine keeps the counter (`Combatant::danger`). The sweep walks the
world's grid as the original's does: cell rows, then cells, then each cell's
list in the order its objects came into it ([`spatial-index.md`](spatial-index.md)).

## What is still unknown

* `[settlement+0x40]`, which the impact and the `IsValidTarget` virtual both
  require to be set for a building to be hit.
* What the catapult's state machine does when another animation is already
  playing when `Attack` is called: the attack animation starts when that one
  ends, and the shot and the script's resume move with it. Here the throw
  always starts at once.
* Whether `facing_column` and 0x005f8530 (a tangent table built by
  0x005f81d0) agree on every heading. Both put a wedge on each of the
  entity's facings; a heading on a wedge's edge can pick the neighbouring
  type-3 point, which moves the launch by a few tens of units.
