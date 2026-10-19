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
what `FormSetupAndMoveTo` (0x0052e5e0, 0x0052e890) calls. It puts the
centre on the hero, keeps the lock flag, moves a requested orientation into
the settled pair and clears the request, zeroes the sample count and the
blocking gate, and lays the formation's own route (`+0x84`) from there --
at most 999 of the range (0x0041a4c0). A route laid sets `+0x88` and gives
every member with a slot `form_move` with the replace flag after deleting its
route and setting its sample to `(-1, -1)` (0x005f2b50); none laid clears
`+0x88`, ends every member's running `form_move` (0x005f2c10) and deletes the
route object. The hero is then given a route of his own aimed at the
formation (0x00417790) and the first sample is made.

`CreateNextSample` (0x005f5bd0) steps the centre along the formation's route
(200 a step); when it has moved, the new direction is the step's, a step
against the old direction (negative dot product) marks a reassign, and within
200 of the route's end a settled orientation, if any, takes over the
direction. Then `PlaceSample` and the next deadline (0x005f5850). At the
route's end it clears `+0x88`, and a route that never laid a sample ends the
members' `form_move`. A sample is made when a unit on a route aimed at the
formation comes within 133 of its station (0x00419b20).

`UpdateMove` (0x005f5e30), the hero's `FormKeepMoving` on its first entry,
does nothing without a route object. With one, it runs `UpdateRebuild`; when
that found the layout marked and a sample had been laid, it lays the current
sample again where it is (`PlaceSample` with the stored centre and
direction) and re-searches every member's route; otherwise it runs
`UpdateReassign`.

**The pace** (0x005f5260, 0x005f5850, 0x00419e52) is described at
`pace_march` in `sim/command.cpp`.

**The claim on the stations** (0x005f3fe0, with the gates near the hero
opened for its flood by 0x005f41b0 and closed again by 0x005f3240) is
described at `sim/movement.hpp`'s inference 3.

## What this engine models

`sim/formation.hpp` keeps one `Formation` per hero in the movement system:
the shape, the slot list with each slot's class, offset and sample, the
pending list, both marks, the radius, the centre and direction, the sample
count, whether the route object exists, and the lock flag. Joining and
leaving are the hero system's `attach` and `detach` (leaving deletes the
member's route); a hero's death drops the object without removing its
members one by one. The layout, `PlaceSample`, `UpdateReassign` and
`FormRadius` are the original's rules above, in integers. The march
(`place_army` in `sim/command.cpp`) is `SetDest`'s hand-out and settle,
`UpdateMove` on each `FormKeepMoving`, and `CreateNextSample`; the members
are sent to their slots' samples, `FormAcceptMove` steps a held member out
towards its sample and ends the script of one with no slot, and the gate
step and the stats rebuild read the radius and the shape from the object.

Still approximated:

* **The formation's route is the hero's.** The engine lays no route for the
  formation itself: the hero walks his own, the centre is put
  `200 + 133` ahead of him along it whenever `place_army` runs (the setup
  and each `FormKeepMoving`), and a sample is made when that centre has
  moved. The original's sample is made when a unit nears its station, and
  the hero walks to the centre.
* **The members walk their own routes to their samples**, not a route aimed
  at the formation, and arrive under `MovementSystem::marching`. The pace
  (`pace_march`) times those routes to the sample's deadline.
* **The rotation is exact.** The original multiplies by single-precision
  factors in the FPU and truncates; the engine takes the exact quotient and
  truncates (`formation_turn`), which can differ by one unit where a product
  lands within rounding of an integer.
* **The claim on the stations is not wired**, as before
  (`sim/movement.hpp`, inference 3), nor is the blocking gate (`+0x90`).

## What is still unknown

* Whether the FPU runs at single or double precision when the rotation is
  taken (Direct3D sets single unless told otherwise); it decides the rare
  off-by-one above.
* What a `<Class>` whose three weights are all 0 does: the chooser answers
  no group and the record is written before the first group; no shipped
  entry does it. The engine puts such a record in the central block.
