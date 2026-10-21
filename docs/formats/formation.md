# The formation object (`CVXFormObj`)

A hero marches his army in formation. `gbr.exe` keeps, for every hero, one
formation object at `[hero+0x2b0]`: who is in it, where each member stands
relative to the hero, where the formation is now and which way it faces, and
the formation's own route. This page describes that object as read from
`gbr.exe`, so the engine's model (`sim/formation.hpp`) can be checked against
it. Addresses are given so each claim can be checked; nothing here is code
from the executable (`docs/legal.md`).

It is a description of a runtime structure, not of a file. `FORMATIONS.XML`,
the file the shapes come from, is described where it is read
(`FormationTable` in `sim/movement.hpp`), and the loader's rules that matter
to the layout are repeated below.

## Construction and lifetime

The hero's constructor (0x0052ffd0) makes the object (0x005f3a80) with the
file's default shape and two shape parameters, `shape_dist` and
`shape_chaos`, both **0**; nothing else ever writes them, so every formation
in the game is laid with no extra spacing and no jitter. The constructor
gives the hero a slot record of his own (`[hero+0x1c0]`, see below), sets
the sample direction to the hero's own direction (`[hero+0x48]`,
`[hero+0x4c]`) and the reassign flag, and nothing else: no member, no
route. The hero's destructor (0x005304b0) deletes it.

## The fields

The persisted names are the save writer's (0x005f4b80).

| offset | name | what |
|---|---|---|
| `+0x08` | | the hero |
| `+0x0c` | | the **slot list**: a list of slot records, `+0x10` its head, `+0x14` its size -- the members that have a place |
| `+0x18` | `class` | the shape, a `<FormationClass>` of `FORMATIONS.XML` |
| `+0x1c` | `reassign_flag` | the members' places are to be chosen again (1 at construction) |
| `+0x20` | `rebuild_flag` | the layout is to be made again |
| `+0x24`, `+0x28` | `units_alive`, `units_died` | the slot count at the last layout, and the members removed since |
| `+0x2c`, `+0x30` | `shape_dist`, `shape_chaos` | always 0 (above) |
| `+0x34` | `radius` | the farthest any slot stood from the hero at the last layout |
| `+0x38`, `+0x3c` | `sample_dir_x/y` | the way the formation faces |
| `+0x40`, `+0x44` | `sample_center_x/y` | where the formation stands: the hero's own station |
| `+0x48` | | how many samples the current route has laid |
| `+0x4c`, `+0x50` | `sample_start_time`, `sample_end_time` | the current sample's deadline (the pace, below) |
| `+0x54`, `+0x58` | `orientation2_x/y` | a requested final orientation (`SetFinalPartyOrientation`) |
| `+0x5c`, `+0x60` | `orientation_x/y` | the settled one (`GetFinalPartyOrientation`) |
| `+0x64`, `+0x68` | `min_step`, `min_duration` | the formation's speed (the pace, below) |
| `+0x6c`, `+0x70` | `minunit`, `maxunit` | its slowest and fastest unit |
| `+0x74` | | the **pending list**: a vector (`+0x78`..`+0x7c`) of units that joined since the last layout |
| `+0x84` | `samplepath` | the formation's own route, a path object with no unit |
| `+0x88` | | whether that route has further to go |
| `+0x8c` | `should_lock` | `FormSetupAndMoveTo`'s fourth argument |
| `+0x90` | `blocking_gate` | the gate the formation waits on |

A **slot record** is 0x18 bytes: the member (or, while the layout runs, its
class), the formation, the slot's **offset** from the hero in the formation's
own frame (`+0x08`, `+0x0c`), and its **sample** (`+0x10`, `+0x14`), the point
on the map the member is to stand on. A member points back at its record at
`[unit+0x1c0]`. The hero's own record (made by the constructor, offset
`(0, 0)`) is not in the slot list; its sample is the formation's centre.

`FormRadius` (0x0052ec40 -> 0x005f2350) answers `radius` when the slot list
is not empty and `radius` is not 0, and the hero class's `radius`
(`[class+0x2dc]`) otherwise. The same reader sizes the formation's route at a
gate (0x00418260).

## Joining and leaving

**Joining** (0x005f47e0, from the hero's `AddUnit` 0x0052e3e0, which
`AttachHero` reaches): the unit first leaves any formation it has a slot in
(its `vtbl+0x128`: 0x005d2f00 for a unit, which removes it when
`[unit+0x1c0]` is set), then it is pushed on the **pending list**, the layout
is marked (`rebuild_flag`), and `minunit`/`maxunit` are cleared. It has no
slot, and `FormRadius` does not count it, until the layout runs.

**Leaving** (0x005f3290, from the hero's `RemoveUnit` 0x0052db70, which
`DetachHero` and a death reach, and from the unit's `vtbl+0x128`): the unit's
slot is taken out of the slot list (`units_alive` less one, `units_died`
one more) or, if it had none, the unit is taken out of the pending list;
`minunit`/`maxunit` are cleared if they named it; its slot record is freed
and `[unit+0x1c0]` cleared; **its route is deleted** (0x005d3830 with 0); and
the layout is marked. The radius is left as it was until the layout runs.

## The layout

`UpdateRebuild` (0x005f4390) does nothing unless `rebuild_flag` is set. Then
it copies the pending list, runs the layout (0x005f1f60), and gives each unit
that was pending the `form_move` command with the replace flag
(0x005aef20, 0x005b4e90 with 1) -- a unit that joins an army on the march
takes its place in it at the next layout.

The layout:

1. With no slot and nothing pending, `radius` becomes 0 and nothing else
   happens.
2. Every slot is taken out of the slot list (0x005f45f0): its member is put
   on a list of units, `[unit+0x1c0]` is cleared, and the record keeps the
   member's class. Then a new record is made for every pending unit
   (0x005f1ad0), its unit put on the same list after them, and the pending
   list is emptied. So the units are in the old slot order, then in the order
   they joined.
3. The records are sorted into four **groups** -- the central block, the front
   line, the left wing and the right wing -- by the shape's `<Class>` entries
   (0x005f1d30). The loader (0x005f6ca0) keeps each entry's class and three
   integer **weights**, `CentralBlock`, `FrontLine` and `Wings`, and inserts
   every entry before the first entry already there whose class it is or
   descends from, so a class is met before its ancestors. For each entry in
   that order, the first remaining record whose class is the entry's class or
   descends from it names a class; every remaining record of exactly that
   class is taken, in order, and each goes to the group whose count over
   weight is lowest -- weights central, front, half the wings, half the wings;
   a group of weight 0 never; the earliest group on a tie -- and that group's
   count goes up by one (0x005f01f0). The four counts run on across all the
   entries. Then the next record matching the same entry names the next class,
   until none is left, and the next entry is taken. Within a group, records
   are ordered by their class's `formation_priority` (`[class+0xa70]`; a class
   that declares none has -15,000,000), lowest first, then in the order they
   were taken (0x005f1c20, 0x005f1890). A record no entry matches is laid
   nowhere and dropped.
4. Each group's spacing **S** is twice the largest `formation_radius`
   (`[class+0x2e4]`; a negative one gives way to `selection_radius`,
   `[class+0x2e0]`) of the classes in it (0x005f0470), plus `shape_dist`. The
   hero's **R** is his class's `radius`. Every slot's offset is set through
   one helper (0x005f03f0) that adds a jitter drawn from the game's generator
   in `[-shape_chaos, shape_chaos]` to each coordinate; with `shape_chaos` 0
   that draw is the empty range and takes nothing from the generator. Forward
   is **-y** (below, "the sample").

   * **The central block** (0x005f04b0) is laid in square rings round the
     hero. Ring `i` (from 1) has half-side `h = R + i*S` and room for `8i`
     slots; each takes `k = min(left, 8i)`. A full ring (`k >= 4`) has its
     four corners, and `k - 4 = 4q + m` more along its sides: `q` along the
     back (`y = +h`), `q` pairs down the two sides (`x = -h` then `x = +h`),
     `q` along the front (`y = -h`); a remainder of 1 adds one to the front;
     of 3, one to the front and a pair to the sides; of 2, a pair to the sides
     when the front line has members and otherwise one each to the front and
     the back. Along a side of `n` slots the `j`-th (from 1) stands at
     `2hj/(n+1) - h`, divided as integers. The order is the back, the side
     pairs (left then right, front to back), the front, then the corners
     back-left, back-right, front-left, front-right. A last ring of fewer than
     4 lays one slot at the front centre `(0, -h)`; two at `(-h, 0)` and
     `(h, 0)` when the front line has members, else at `(0, -h)` and
     `(0, h)`; three at the front corners and the back centre. The block
     answers its half-width and its depth to the front: `h` of its last ring,
     less `S` where a short last ring did not widen it that way; `R` both, for
     an empty block.
   * **The front line** (0x005f0b60) stands ahead of the block. With `n`
     slots, spacing `S` and the shape's `Width` and `Height` (each defaults to
     1; the loader refuses a 0), it has `cols x rows` slots: both grow by
     `Width` and `Height` until they hold `n`, then each is cut back while one
     column (then one row) fewer still holds `n` (0x005f02e0). Its half-width
     is `(cols/2)*S`, less `S/2` for an even `cols`, at least the block's;
     its depth is `rows*S` past the block's depth plus `OffsetFrontLineByY`.
     The first row stands at `y` = minus that depth, each next row `S`
     behind. A row of `p` slots puts one at `x = 0` when `p` is odd, then
     `p/2` pairs: one pair at the half-width's two edges, or pairs spread
     from the centre out to the edges, left of each pair first. With no front
     line, the half-width is the block's and the depth the block's plus
     `OffsetFrontLineByY`.
   * **The wings** (0x005f0d90) stand off the front line's two edges,
     `OffsetWingsByX` further out, their first row level with the front
     line's first row and `OffsetWingsByY` further ahead:
     `q = isqrt(n)` columns (at least 1), one more row and maybe one more
     column when `q*q < n`; each row laid outwards from the edge in steps of
     `S`, each next row `S` behind.

   `radius` is the integer square root (0x0067c3d0) of the largest squared
   offset laid. The groups are put back as the slot list in the order
   central, front, left, right.
5. Units are given slots (0x005f4b30): every slot's sample is set to
   `(0, 0)` and each unit takes the first free slot of exactly its own
   class, in the units' order. `units_alive` becomes the slot count,
   `units_died` 0, `rebuild_flag` is cleared and `reassign_flag` set.

## The sample

`PlaceSample` (0x005f5a20) puts the formation down at a point facing a
direction: the hero's own record and the formation's centre take the point,
the layout is run if it is marked, and every slot's sample becomes the point
plus its offset turned into the direction (0x005f2900). With `d` the
direction scaled to length 1, an offset `(ox, oy)` lands at
`(-(ox*dy + oy*dx), ox*dx - oy*dy)` from the point, each coordinate truncated
toward zero: local `-y` lands on `d` and local `+x` on `(-dy, dx)`. The products are taken in the FPU from single-precision
factors. Then the claim on the stations runs (below), and then
`UpdateReassign`.

`UpdateReassign` (0x005f56d0) does nothing unless `reassign_flag` is set and
there is a slot. It takes every slot out as the layout does, and gives each
unit, class by class, the free slot of its class whose sample's offset from
the mean of all the samples is nearest the unit's offset from the mean of all
the members' positions (0x005f49a0, 0x005f3020): units in order, the first
slot on a tie, means divided as integers. The slots go back in the same
order, and the flag is cleared.

## The route and the march

`SetDest` (0x005f2cc0; the object form 0x005f2ee0 for an object goal) is
what `FormSetupAndMoveTo` (0x0052e5e0, 0x0052e890) calls, after it has
stepped the hero out of any holder and deleted the hero's own route
(0x005d3830 with 0). `SetDest` puts the centre on the hero, keeps the lock
flag, moves a requested orientation into the settled pair and clears the
request, zeroes the sample count and the blocking gate, and lays the
formation's own route (`+0x84`, below) from there. A route laid sets
`+0x88` and gives every member with a slot `form_move` with the replace
flag after deleting its route and setting its sample to `(-1, -1)`
(0x005f2b50); none laid clears `+0x88`, ends every member's running
`form_move` (0x005f2c10) and deletes the route object. With a route, the
setup then aims the hero's own route at the formation (0x00417790, below)
and makes the first sample.

### The formation's route

The route object is a route record like a unit's, aimed at no unit, whose
path is stepped 200 at a time (`RecastPathfind`, 0x00419730, builds the
path with that stride). Its goal is kept in the record, as a unit's is:

* **The point form** (0x0041a4c0) keeps at most 999 of the range.
* **The object form** (0x0041a5c0) keeps at most 999, and, when the goal is
  new, at most 920 (0x0041a672 .. 0x0041a68e). The engine applies the 920 to
  every object setup.

A goal, range and minimum range all equal to the record's last ones keeps
the route as it is, unless it is barred by a gate, and searches again only
when it is not arrived; a new one searches afresh. The search is the route
search every unit uses, from the centre, with no unit to test passability
for: the formation's goal is the ring at its range round the point, and a
formation already within its band lays nothing and is arrived (the search
asks `IsArrived` first, 0x00419730). The search also lists the gates the
route crosses (0x00418fb0), each as what is left of the route where it
crosses.

**The path is stepped by legs.** When a route is laid, what is left of it
(`+0x34`, in 16.16) is the sum of the integer roots of its legs' squared
lengths. A leg is started when the last one's steps are spent: its length
`L` (the integer root again) is cut into `n = L / 200` steps or `n + 1`,
whichever makes a step whose length comes nearer 200 (0x0041a740; `n + 1`
when `n` is 0). The step's offset is the leg's offset in 16.16 divided by
that count, and its length the leg's length in 16.16 divided by it. A leg
of no length takes no step. The next point is the leg's start plus the
offset times the step number, each coordinate's 16.16 product shifted
right (floored, not truncated) -- except the leg's last step, which is the
leg's end exactly. Taking the step (0x0041a700) takes the step's length
off what is left, and the very last step of the route leaves nothing.
What is left is answered in whole units as the high word of the 16.16
value, read as a signed 16-bit number (0x0040b890); `FormPathLeft`
(0x0052ebc0) answers that, plus 32,000 when the search ended short of its
goal (0x004185a0), and 0 with no route object.

### The sample

`CreateNextSample` (0x005f5bd0) first sizes the formation's speed
(`UpdateMinSpeed`, 0x005f5640: the mean of the hero's and every placed
member's speed, held to one and a half times the slowest), then asks the
route for its next point through the same retry step a unit's route takes
(below), so a gate within the formation's reach stops the centre. A point
answered is taken (the route committed), and then:

* if nothing is left of the route, it is searched again from there, which
  finds the formation arrived and lays nothing when the point was the
  goal's;
* `+0x88` is set to whether anything is left;
* the step's offset from the old centre is the new direction, and a step
  against the old direction (negative dot product) marks a reassign;
* within 200 of the route's end a settled orientation, if any, is the
  direction instead;
* `PlaceSample` at the new point, the sample count goes up by one, and the
  next deadline is timed (0x005f5850).

No point answered, and no sample yet laid, ends the members' `form_move`
(0x005f2c10); either way `+0x88` is cleared and the call answers 0. So a
march whose next centre a gate bars is spent there: the hero walks to the
last centre and arrives, and only a new `FormSetupAndMoveTo` goes on.

**The gate.** The step finds the first gate crossing not yet passed: the
first one the list holds whose remaining-at-crossing is no more than what is
left of the route now (0x00418210). It bars the step while the gate bars the
hero's owner and the crossing is within reach of where the centre stands
(0x00418260): the formation's radius (`FormRadius`, or the hero class's
radius with no slot or a radius of 0), never less than 275, plus 210. A
barred step searches the route again once and asks again; still barred,
it answers no point.

### The route aimed at the formation

`SetFormation` (0x00417790) aims a unit's route at a formation (the retry's
mode 2): the hero's own by the setup, and each member's by `FormAcceptMove`
(0x005d7880), the script `form_move` runs once the member is out of any
holder. Only the mode is set; the route to the unit's station (its slot's
sample, the centre for the hero) is laid at the unit's next step.

At each step of such a route (0x00419b20): when a sample has been made since
the route was laid (the count the route noted, `[retry+0x10]`), it is laid
again to the station, or deleted when the unit stands on it. Then, for up to
100 passes: while the formation's path goes on and what is left of the
unit's route is within half the unit's step and 133, the next sample is made
and the route laid again to the new station. A sample that cannot be made
takes no step. The step is then timed: its pace is what is left of the
route, less 133 while the formation goes on, over the time to the sample's
deadline, held to between half and two and a half times the formation's
speed, and the fast end past the deadline (0x00419e52 .. 0x00419ecd,
0x005f2130). Such a route is arrived when the formation's path is spent and
the unit stands on its station (0x00417c4a .. 0x00417c73), and faces the
formation's direction there. A search that lays nothing keeps the route the
unit had and takes no step; with the formation's path spent it marks the
route arrived (`[retry+0x20] |= 0x80`), and the unit stops where it stands.

### The blocking gate

The unit's step has a gate test of its own, after the samples and before
the timing (0x00419ca0): the first crossing of the **unit's** route not yet
passed (0x00418210), within the unit's own radius plus 210 (mode 2,
0x00418260), whose gate bars the unit. Barred, the step writes that gate's
handle into the formation's `blocking_gate` (`+0x90`) and takes no step:
the unit stands, its route kept, and clears its walking bit
(`[unit+0x194]` bit 16).

With `+0x90` naming a gate, every unit's step then asks 0x00417dc0 before it
is timed. A unit whose route has not yet seen that gate notes it (the
route record's `+0x18`) and compares its side of the gate with the hero's
(0x005295d0, below); on different sides, the unit's commands are ended
(`vtbl+0xc0` with 1) and it is detached from the hero (`DetachHero`,
0x005db190), and the step takes no step. So when a gate shuts on a march,
the members cut off from the hero by it leave his army; the rest go on.
`SetDest` sets `+0x90` back to none.

**The side test** (0x005295d0, a member of the gate) answers false for a
unit in a holder or none, and otherwise lays a throw-away route record for
the unit to the central building of the gate's settlement (the gate's
`+0x148`, that object's `+0x8c`) through the point form of `SetDest`, and
answers whether that route crosses no gate at all: inside the walls is
true.

### Keeping it moving

`UpdateMove` (0x005f5e30), the hero's `FormKeepMoving` on its first entry,
does nothing without a route object. With one, it reads the layout's mark,
runs `UpdateRebuild`, and when the mark was set and a sample had been laid,
lays the current sample again where it is (`PlaceSample` with the stored
centre and direction) and searches every member's route to the formation
again (`[unit+0x148]`, `RecastPathfind`); otherwise it runs
`UpdateReassign`.

**The pace** (0x005f5640, 0x005f5850): the deadline is the time the
formation, at its speed, takes to cover the mean over the hero and its
placed members of the straight distance from each to its station, less 133
while the path goes on.

**The claim on the stations** (0x005f3fe0) is described at
`sim/movement.hpp`'s inference 3. `PlaceSample` brackets it with the gates
near the centre (0x005f5b62, 0x005f5b85). Before the flood, 0x005f41b0
walks every object of every player and picks each gate whose position lies
within 1,600 of the centre on both axes (a box 3,200 across, not a circle)
and that is either one the hero's owner may pass (0x005a78e0, the
diplomacy bit) or one that lets no unit through now (0x00529300: not broken
to its last tier and raised no more than 20) and is not being opened
(`[gate+0x208]` not 1). Each gate picked is listed, and its cells on the
collision grid are saved and rewritten for the flood (0x0052b130: the cells
of its box in 16-unit cells, kept at `[gate+0x230]`, 0x0052adf0 and
0x00529e40). After the claim, 0x005f3240 walks the list backwards and puts
each gate's saved cells back (0x0052a540 -> 0x0052a4c0). What the rewrite
writes into the cells -- passable or blocked -- was not read (below).

## What this engine models

`sim/formation.hpp` keeps one `Formation` per hero in the movement system:
the shape, the slot list with each slot's class, offset and sample, the
pending list, both marks, the radius, the centre and direction, the sample
count, the route object with its goal, path and gate crossings, whether the
path goes on, the deadline and speed, and the lock flag. Joining and leaving
are the hero system's `attach` and `detach` (leaving deletes the member's
route); a hero's death drops the object without removing its members one by
one. The layout, `PlaceSample`, `UpdateReassign`, `FormRadius`, the route's
stepping and the sample are the original's rules above, in integers
(`formation_route_step`, `MovementSystem::formation_next_sample`). The
setup, `UpdateMove` and the hand-out are `FormSetupAndMoveTo` and
`FormKeepMoving` in `sim/command.cpp`; the step of a route aimed at the
formation is `MovementSystem::formation_step`.

Still approximated:

* **A sample made inside the movement step defers its `form_move`.** The
  layout runs when the sample is placed, and a member it gives a place then
  is handed `form_move` at the hero's next `FormKeepMoving` rather than at
  once, since scripts are not run from inside the movement step. A member
  on the march that is not yet aimed at the formation is aimed there too
  (`aim_marchers`).
* **An object goal is its position at the setup.** The original's object
  form keeps following the object; the engine lays the route to where the
  object stood.
* **A follower strides 200.** The original cuts a route aimed at the
  formation by the unit's own walk step (0x00419730 with 0x005d2920), as
  any unit's. Here the stride is also the avoidance's step, which is this
  engine's model; with the walk step a column whose `form_move` an AI
  squad's `ClrCmd` ends mid-march stood in piles (Crossroads seed 5's worst
  overlap 82, against 36 with 200). The followers keep the 200; the hero
  strides his walk step. The half-step test that makes the next sample is
  the original's, the unit's walk step, and a unit with none takes 200.
* **The deadline has no animation time** and is not snapped to 100 ms, and
  the mean speed is an integer mean.
* **A member standing on its station has a route of no length**, so
  `HasPath` still answers while the formation goes on; and a member whose
  search lays nothing while the path goes on stands until the next turn.
* **The rotation is exact.** The original multiplies by single-precision
  factors in the FPU and truncates; the engine takes the exact quotient and
  truncates (`formation_turn`), which can differ by one unit where a product
  lands within rounding of an integer.
* **The claim on the stations is not wired**, as before
  (`sim/movement.hpp`, inference 3), nor is the blocking gate (`+0x90`)
  with its detach of the members a shut gate cuts off. A member barred on
  its own route holds there under this engine's own gate step
  (`MovementSystem::held_at_gate`), whose reach is the original's mode-2
  one; nobody leaves the army for it.

## What is still unknown

* Whether the FPU runs at single or double precision when the rotation is
  taken (Direct3D sets single unless told otherwise); it decides the rare
  off-by-one above.
* What the gate bracket round the claim writes into a gate's cells
  (0x00529e40), and so whether the flood goes through the gates near the
  centre or round them. The claim is not wired, so nothing here depends on
  it yet.
* Whether the object form's 920 applies only to a new goal: the cap was
  read in the new-goal branch, and the engine applies it to every object
  setup.
* What a `<Class>` whose three weights are all 0 does: the chooser answers
  no group and the record is written before the first group; no shipped
  entry does it. The engine puts such a record in the central block.
