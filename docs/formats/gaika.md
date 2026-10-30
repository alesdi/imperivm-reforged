# The AI's map nodes as `gbr.exe` keeps them

The computer players do not reason about the map tile by tile. They reason
about **nodes** -- `GAIKA` in the scripts, a region of passable ground or a
settlement with the ground around it -- and about which squads stand in each
node and which are on their way to it. This page describes the original's
node table and the per-node lists around it, as read from `gbr.exe`, so the
engine's model (`sim/gaika_table.hpp`, `sim/squad.hpp`) can be checked against
it. Addresses are given so each claim can be checked; nothing here is code
from the executable (`docs/legal.md`).

It is a description of a runtime structure, not of a file. The original
rebuilds the table when a map loads and persists the per-node squad lists in a
save; what this engine saves is in [`save.md`](save.md).

## The node manager

One object of `0x4c` bytes, its pointer in the global at 0x008c8e54, created
by 0x004509f0 from the builder entry 0x00450aa0 that `LSA()` (0x006a8720) and
the AI manager's start reach. Every reader tests the pointer first and answers
"no node" (0, an empty list, `Empty` true) without it. The manager holds:

| offset | what |
|---|---|
| `+0x04`..`+0x0c` | one 32-bit entry per settlement, by the settlement's index `[set+0x1c]`: the settlement's node id in the low half, 0 for a settlement that has none (`Settlement::GetGaika`, 0x0044e9e0) |
| `+0x14`..`+0x18` | the node records, `0x24` bytes each, indexed by node id (`GaikaId`); record 0 is a real record that names no place |
| `+0x24`..`+0x2c` | the passability-area records, `0xc0` bytes each, by area id; the type word at `+0` is 1 for water and 2 for ground |
| `+0x30`, `+0x34`, `+0x36` | the **node slot grid**: one 16-bit node id per 128-unit slot, row by row, and its width and height in slots |
| `+0x38` | the `MAIKA` list: the way-point records `SetMAIKA` adds (below) |

The node under a point is a slot-grid lookup: 0x0044e3f0 takes a point,
answers 0 when `x` is -1 or either coordinate is negative, and otherwise reads
the grid at `(y >> 7) * width + (x >> 7)`, each shifted coordinate cut to 16
bits, with no test against the grid's far edges: a point past the right edge
reads the next row's slot, and one past the bottom reads beyond the grid.
`GetGAIKA(point)` (0x005bd090) is that;
`GetGAIKA(x, y)` (0x00421910) is the same with two integers; the object form
`GetGAIKA(Obj)` (0x00424660 -> 0x0044e650) reads the object's raw position,
or its position in the right-handed frame when the raw `x` is negative (a held
unit), and answers 0 for a negative point. The three `LSA` forms (0x00422600,
0x00422630, 0x00425fd0 -> 0x0043cb30 / 0x0043cb70) answer the passability
area of the node at the point, `[record+0x10]`.

## The node record

Read from its persist function 0x0044ff00 and from the readers:

| offset | what |
|---|---|
| `+0x00`..`+0x06` | the bounding rectangle in slots: left, top, right, bottom, 16-bit and inclusive |
| `+0x08` | the centre point in world units, what `Center` and `GetDestPoint` answer for a region node |
| `+0x10` | the passability area (`LSA`) the node lies in |
| `+0x14` | the settlement whose node this is, null for a region node (saved as the settlement's index) |
| `+0x18` | **the squad deque**: a pointer to a `std::deque` of packed squad handles, allocated on the first push and freed (and the pointer nulled) when the last entry is erased |
| `+0x1c` | the coverage: how many slots of the slot grid carry the node's id |
| `+0x1e`, `+0x20` | the neighbour count and the neighbour list (0x0044a3c0 links them) |

Record 0 is built like the others and never filled: its rectangle is
`(0, 0, -1, -1)`, its centre `(-1, -1)`, its area, settlement and coverage 0.
`Center(0)` therefore answers `(-1, -1)`.

The packed squad handle is the squad record's own (`[squad+0x34]`): the
player in bits 0-3 and the squad's number in bits 4-15 (see
[`squad.md`](squad.md)).

## Which squads a node's deque holds

The deque is the node's own record of the squads that concern it. A squad is
on node N's deque **exactly when N is the node it stands in or the node it is
heading for**, and the order of the deque is the order the squads were filed.
Three squad fields decide it (`sim/squad.hpp` names them):

* `GAIKAIn` `[squad+0x22]`, the node the squad's front member stands in;
* `DestGAIKA` `[squad+0x24]`, the node the squad is bound for;
* the AI order record's node, when the squad has an order (0x00444140 reads
  it); the squad's **heading** is that node when there is an order and
  `DestGAIKA` otherwise.

Two functions keep the deques, and nothing else pushes to or erases from them:

* **0x004501a0 (heading change, `from` -> `to`).** Nothing when the two are
  equal. Otherwise the squad is erased from `from`'s deque unless `from` is 0
  or is the node it stands in, and pushed on the back of `to`'s deque unless
  `to` is 0 or is the node it stands in. Erasing (0x0044f300) finds the first
  matching handle and removes it, freeing the deque when it empties; pushing is
  0x00437870 -> 0x00435d90, a plain `push_back`.
* **0x00450240 (standing change, `GAIKAIn` -> `to`).** Nothing when equal.
  Otherwise the squad is erased from the old standing node unless that is 0 or
  is its heading, pushed on the back of `to`'s deque unless `to` is 0 or is
  its heading, and `GAIKAIn` becomes `to`. Its wrapper 0x0041eac0 needs the
  node manager and also stamps the squad's source node `[squad+0x20]` from
  `GAIKAIn` when the source is still 0.

Their callers are what makes a squad move between deques:

* **`DestGAIKA` changes** through 0x00444210, which refiles through 0x004501a0
  (via 0x0041ea10) **only while the squad has no AI order** -- with an order,
  `DestGAIKA` is not the heading and moving it files nothing.
* **An AI order is posted** (0x004494c0): the previous record is freed first
  (0x00448a80 -> 0x00448a10, which refiles from that record's node back to
  `DestGAIKA`), then the squad is refiled from `DestGAIKA` to the new order's
  node (0x0041ea60, which replaces a 0 at either end by `DestGAIKA`). So a
  squad sent again to the node it was already sent to is erased and pushed
  again: it goes to the **back** of that node's deque.
* **An order is freed** (0x00448a10): refiled from the order's node to
  `DestGAIKA`.
* **A member leaves** (0x00444750): if the squad's strength is then 0 it is
  stood down -- its timers killed, its order freed, filed under no node
  (0x0041eac0 with 0) and bound for none (0x00444210 with 0), which takes it
  off every deque. If the front member left and there is a new one, the squad
  is refiled to the new front's node (0x0044e650 of its position).
* **The first member joins** (0x00445a50): the squad is filed under the node
  the unit stands in (0x0041eac0).
* **A unit crosses a slot boundary** (the position tracker 0x0041f530, called
  from the position setters 0x005d39e0 and 0x005d3e10, and only when the
  128-unit slot changes): the unit's squad assignment is reassessed
  (0x00446a70, with the node being the squad's `DestGAIKA` for a free member
  of a squad and the node under the new position otherwise), and when the
  mover is its squad's front member and its node changed, the squad is refiled
  to that node (0x0041eac0). Ship passengers are carried along. The tracker
  then runs the visibility sweep (below) on the old node and the new.

A squad is only destroyed after it has been taken off every deque, so a
handle on a deque always names a live squad.

## Who walks the deque

Every per-node squad question walks the deque front to back and **applies its
own tests to each squad**; none of them filters by the fields above, because
membership already says the squad concerns the node:

* `GetSquads` /1..4 (0x00437a20, 0x00439000, 0x00439230, 0x00439500) -- the
  list comes back in deque order;
* `Eval` /1, /2, /3, /6 (0x0042f1a0, 0x0042f3d0, 0x0042f670, 0x0042fc40) and
  `Count` /6 (0x00430a30), `Count` /3 (0x004330b0), `EvalNeighbors` per node
  (0x004302a0), `CountClass` (0x0043a920 with 0x0044e1f0);
* `Unit::BestTargetInGAIKA` (0x00431650 -> 0x00431090);
* `GAIKA::Empty` (0x00422dc0), which is just "the deque pointer is null".

The presence a census assigns a squad (coming, leaving, staying; 0x0044e1f0)
is computed from the same three fields; an enemy's is then the asking AI's
view of it (0x0044eb80).

## `Count` /6 and /3

`Count/6` (0x00430a30) is `Eval/6` counting heads: the same walk, category
(0x0044e250 from the viewer's row, everything `AI_OWN` for a negative
viewer), presence and sight, the same accumulator 0x00427e90 (a squad of
messengers is nobody's army, a hidden enemy front fills the hidden total), and
the same settlement tail 0x00420c00 -- handed the squad's member count
`[squad+0x50]` where `Eval` hands its strength, and the wall's head count
`[settlement+0xf4]` where `Eval` adds the wall's strength `[+0xf8]`. It has no
memory tail. `Count/3` (0x004330b0) counts, in each staying squad of the
selected sides (the enemy's presence again the AI's view), the members whose
class descends from the named one (0x00427fc0), then adds the wall's head
count when 0x00420c00 offers a wall -- a settlement whose central building
descends from `BaseTownhall` (0x0043ffc0) with a strength above 0 -- and the
named class descends from `Sentry`.

## What a node remembers per player

Each AI player's record keeps one `0x18`-byte record per node (`[ai+0x30]`,
mapped by the 16-bit table at `[ai+0x40]`): a flags byte at `+2`, the enemy
strength last seen at `+8`, and the turn it was last seen at `+0xc`.

* `Explored` (0x0042ae70 -> 0x0041cb80) answers true when exploration is off
  for the session, else bit `0x02`.
* `Revealed` (0x0042b270 -> 0x0041cbd0) answers bit `0x04` when fog of war is
  on, else `Explored`.
* `LastSeen` (0x0042b320) answers `+0xc`.

All three are written by one sweep, 0x0044ecc0, run on a node when a unit
enters or leaves it (from 0x0041f530, 0x0041e970, 0x0041eee0, 0x0041f310) and
on every node once the table is built (0x0044f2b0). For each AI player, the
node is **visible** when that player, or an ally whose relation row carries
flag `0x10`, has a squad standing in it or owns its settlement. Visible: the
flags byte gains `0x06` (explored and revealed). Not visible but marked
revealed: bit `0x04` is cleared, `LastSeen` becomes the current turn, and the
enemy strength remembered becomes the strength of the enemy squads standing
there (peaceful squads of players 14 and 15 aside) plus the settlement's wall.

That memory is what `Eval/6` (0x0042fc40), `Eval/3` (0x0042f670, when its
sides include the enemy) and `EvalNeighbors`' per-node census (0x004302a0) add
back when the mask has `AI_STAYING` and the node is not revealed to the asking
AI: the remembered strength less what can still be seen there, so the answer
for the enemy staying in an unwatched node is what was there when it was last
watched. `EvalNeighbors` (0x00430800) sums that census over the node's
neighbours, passing over a stronghold, a Teuton tent or an independent guarded
outpost, and -- with its flag -- any neighbour not revealed to the asker.

## The `MAIKA` list

A deque of (source, destination, way-point) triples at the manager's `+0x38`,
six bytes each, **newest at the front**. 0x0044fc60 adds one: a triple equal
in all three ids already on the list is moved to the front (left alone if it
is the front already); a new one is pushed on the front, after the back is
dropped when the list already holds 512. Two writers call it:

* `SetMAIKA(src, dst, waypoint)` (0x00421b70), in argument order, when there
  is a node table and the three ids are non-zero and distinct. The shipped
  caller is `SQUADMONITOR.VS`, recording the node a squad detoured through;
* the attacked hook 0x0041e6c0, the first time in a tick a squad's member is
  hit: when the squad's source node, the node it stands in and its heading
  (the order's node, else `DestGAIKA`) are all non-zero and distinct, it
  records (source, heading, standing) -- the node a marching squad was hit in.

Its reader is `GAIKA::Recruit` (0x00437b80). When the node is not the one the
player's `AssUnderFireGaika` names and the list is not empty, each live
candidate standing in a node other than the recruiting one is checked, once
per source node: 0x0043eda0 walks the list front to back for records from that
node to the recruiting one, and runs `CheckMAIKA.vs(player, src, dst, way)` on
each (its trampoline 0x0043d670 answers true when the script cannot run); the
first way-point the script refuses is the answer. A squad whose source has an
answer is dropped from the recruit, and if what is dropped takes the total
below the minimum the recruit is abandoned.

## How the table is built

0x0044e050 partitions the map once, when the AI manager starts (0x00450aa0),
after every map object exists. It works on two private slot grids the size of
the area builder's (one slot per 128 world units, each side rounded up),
called here the **claim grid** and the **node grid**, and on two lists built
side by side: the node records the manager keeps, and a list of **seeds**, one
`0x20`-byte working record each. Both lists start with a placeholder at 0, so
the first real seed and the first real node are 1. A seed holds a slot, a
radius, a rectangle of slots, whether it is a settlement's central building,
its settlement (none for a region seed), a weight, and the passability area of
its slot.

Every scan below is row by row from the top, left to right within a row. Every
flood is breadth first from one slot, first in first out: a slot is tested
when it is taken off the queue, and when it passes, its four neighbours are
queued left, right, up, down. A slot is "inside the grid" when both its
coordinates run from 0 to the grid's size less one.

1. **The passability areas** (0x00453130, `sim/lsa.hpp`), which also give the
   area that first claimed each slot and each area's coverage (the slots no
   other area claimed first). The claim grid starts at 0xffff on every slot an
   area claimed and 0 elsewhere; the node grid starts all 0 (0x00449650).

2. **Settlement seeds** (0x0044d080). For each settlement, in the world's
   order, whose central building descends from `BaseTownhall`, `BaseVillage`,
   `Outpost`, `BaseShipyard` or `TTent`, every building in the settlement's
   own list, in that list's order, is placed:
   * its slot is the slot of its land entrance nearest the point `(-1, -1)`
     (the enter-point list on ground; nearest by the floored square root of
     the squared distance, the earlier entry keeping a tie; the building's own
     position when it has no land entrance), and
   * that slot is **snapped** (0x0044ab60): of the slot itself and twenty
     around it out to two slots -- in the order (0,0), (-1,0), (1,0), (0,-1),
     (0,1), (-1,1), (1,-1), (-1,-1), (1,1), (-2,0), (2,0), (0,-2), (0,2),
     (1,2), (-1,2), (1,-2), (-1,-2), (2,1), (2,-1), (-2,1), (-2,-1) -- each
     inside the grid and claimed by a ground area, the one whose area has the
     greatest coverage, the earlier keeping a tie. With none, the building
     places no seed.
   A placed building makes a seed of radius 10: its rectangle is the snapped
   slot 10 each way, cut to the grid; its weight is 3 for the central building
   or a building descending from `Tower`, 2 otherwise; its area is the one
   that claimed the snapped slot. Before it is listed it **stakes** its slots:
   a flood from its slot over the slots inside its rectangle claimed by its
   own area, writing its number into the claim grid wherever the number there
   is not already its own -- over another seed's as readily as over 0xffff.
   When the central building descends from `Outpost`, a second seed follows:
   from the first of the building's land entrances, in list order, whose slot
   is not the first seed's snapped slot, snapped the same way; radius 10, not
   central, and the same settlement, weight and area as the first seed (the
   area is not looked up again). It stakes and is listed the same way. With no
   such entrance, or no snap, there is no second seed.

3. **Region seeds** (0x0044d4c0). Every slot of the claim grid still at
   0xffff, in scan order, makes a seed there of radius 15, weight 1, no
   settlement and the slot's area, which stakes its rectangle as above before
   the scan goes on; so a slot staked by an earlier seed is passed over.

4. **The weighted claim** (0x0044d670). Every non-zero slot of the claim grid
   goes back to 0xffff. Then each seed from 1 in order floods from its own slot
   over the slots inside its rectangle claimed by its own area, visiting each
   slot at most once, and takes each slot it reaches that is 0xffff or whose
   present seed scores no more than it does there. A seed's **score** at a
   slot is `((15 - |dx|)^2 + (15 - |dy|)^2) * weight`, `dx` and `dy` being the
   slot's offset from the seed's own slot; the nearer and the heavier seed
   wins, and the later seed takes a tie.

5. **Settlement nodes** (0x0044d760). For each central seed in seed order, a
   node is made with the next id. A flood from the seed's slot over the whole
   grid takes every slot whose claim is a seed of the **same settlement** and
   whose area is the central seed's: it writes the node id into the node grid,
   sets the claim back to 0xffff, counts the slot and widens a rectangle that
   starts as the seed's slot alone. The record gets that rectangle, the count
   as its coverage, the seed's area and settlement, and the **middle of the
   seed's slot** (`slot * 128 + 64` on each axis) as its centre -- the
   snapped door's slot, not the building's. Then the slot under the central
   building's own position is given the node id in the node grid, and the
   settlement's entry in the manager's per-settlement table becomes the id.

6. **Region nodes** (0x0044d940). For each seed from 1 in order, the slots of
   its rectangle are scanned (the grid index cut to 16 bits); each slot still
   claimed by that seed starts a new node with the next id: a flood over the
   whole grid through the slots claimed by the same seed (no area test) sets
   each one's claim to 0xffff, writes the id into the node grid and widens a
   rectangle that starts at the first slot. The record gets that rectangle,
   the seed's area, no settlement, and the middle of the first slot as its
   centre. A seed whose slots fell apart makes one node per piece.

7. **Smoothing** (0x0044c5d0), over the node grid. Every slot with a node
   and off the grid's outer ring, in scan order, is tested; a slot the test
   changes is queued. The queue is then worked first in first out: for a slot
   taken off it, its four neighbours -- below, above, right, left -- are tried
   in that order, each when it has a node, its column is above 0, and the
   taken slot's row is above 0 and its column and row are below the last; the
   first neighbour the test changes is queued, and the rest are not tried.
   The **test**: tally the node ids of the slot's four neighbours (below,
   above, right, left, each inside the grid) in order of first appearance,
   keeping for each id the area of the first neighbour that showed it. Bring
   the most frequent of the first three entries to the front by two
   exchanges (the front with the second when the second has more votes, then
   the front with the third when the third has more). If the front id is a
   node, the slot's own area is the front's, the slot is not already that
   node, and either the front has 3 votes, or it has 2 and the second entry
   does not also have 2 with the slot already being that second node: the
   slot becomes the front's node. Otherwise, when all four neighbours differ,
   tally the eight neighbours (the row above left to right, the row below left
   to right, then left and right, each inside the grid) the same way, sort the
   tally by votes, most first (an exchange sort that only moves an entry on
   strictly more votes), and the slot becomes the leader's node when the
   leader is a node, shares the slot's area, has strictly more votes than the
   runner-up, and is not already the slot's node. When a slot changes, the
   winning node's rectangle is widened to take it -- except in the two-vote
   case, where the rectangle widened by is `(column, row, row, row)`, which is
   empty when the row is less than the column and is then ignored.

8. **Merging small regions** (0x0044ddd0). The records are walked from node 1.
   A region node whose rectangle covers fewer than 30 slots looks for a host
   (0x0044db20): over each of its slots within its rectangle, each of the
   eight neighbours (up-left, down-right, left, right, down-left, up-right,
   up, down) inside the grid with another node counts one vote for that node;
   a node is entered in the tally, in order of first appearance, only when it
   lies in the same area. The host is the node with the most votes, the
   earlier keeping a tie. With a host, the small node's slots within its
   rectangle become the host's and the host's rectangle is widened by the
   small one's; then, unless the small node is the last, the last node's slots
   within its own rectangle are renamed to the small node's id, the last
   record is copied over the small one's, and, when it is a settlement's, the
   settlement's entry follows it. The last record is dropped, and the walk
   starts again **from node 2**. A small node with no host stays.

9. **Neighbours and coverage** (0x0044a3c0). Every node's coverage and
   neighbour list are cleared; then each slot with a node counts one towards
   that node's coverage, and each of its eight neighbours inside the grid with
   another node makes the two nodes neighbours. Each list holds the neighbours
   in ascending id.

10. **Region centres** (0x0044aaf0 -> 0x0044a8a0), for each node from 1 that is
    not a settlement's. Take the middle of its rectangle (on each axis the sum
    of the two edges halved, rounding toward zero). If that slot carries the
    node and is **wholly free** -- its eight-by-eight passability cells all
    passable (0x00449d30) -- the centre is that slot's **corner**,
    `slot * 128`. Otherwise, of the slots up to five away on each axis (columns
    from left to right, within a column rows from top to bottom), inside the
    grid, carrying the node and wholly free, the nearest by squared offset (the
    first keeping a tie) gives the centre: the middle of **its column**,
    `column * 128 + 64`, but the middle of the **rectangle's middle row**,
    `row * 128 + 64`, whichever row the slot found is in -- the search
    remembers the column it found and not the row. Failing that, the first slot of the rectangle in scan order that carries the
    node and whose three-by-three block, where it is inside the grid, all
    carries the node and is wholly free (0x00449e50) gives the centre, at its
    corner. Failing everything, the centre stays as step 6 set it.

11. The node grid becomes the manager's slot grid. Each settlement whose
    central building descends from `BaseShipyard` then caches two areas on that
    building (0x0044b740, `[building+0x1f4]` and `[+0x1f6]`): its node's area,
    and a water area found from its exit door through the node grid
    (0x0044b440). 0x0044b840 walks the settlements and changes nothing.

Every step is integer arithmetic. The partition depends on the map's
passability and terrain and on its settlements' buildings, their classes and
their entrances as they stand when the AI manager starts; the AI's save
(`CVXGlobalAI::Persist`) keeps the result -- the records, `slotresx`,
`slotresy` and `GAIKASlots` -- rather than building it again.

## What this engine does

* **The deques are modelled** (`SquadTable::filed_under`, `sim/squad.hpp`):
  one list per node of squad keys, pushed and erased by the two rules above
  (`move_in` is 0x0041eac0 -> 0x00450240, `set_dest` is 0x00444210, the order
  post and free refile as 0x004494c0 and 0x00448a10 do, a stood-down squad
  leaves every list), saved in the squad section in filing order (state vector
  45). Every walker above iterates them in order. They are not part of the
  state hash; the squads they name are.
* **`Count/6` and `Count/3`** are the walks described above (twelfth round).
* **The position tracker** is run from the engine's position writes
  (squad.md, "The move hook"; fourteenth round): the reassessment
  0x00446a70 and the front member's refile happen at the write that crosses
  a slot. A walk writes its position at each step's decision and at the
  tick's end rather than at each step's start, so a crossing is seen up to a
  step late. Where a squad stands is also refreshed once a tick, for every
  squad in table order, from its front member's position
  (`revalue_squads`), which covers the refiles at a member's leave and at a
  stand-down.
* **The table is the original's partition** (`GaikaTable`, fifteenth round):
  every step above, in its order, with the 16-bit index arithmetic where the
  original truncates. `GetGAIKA` reads the slot grid as 0x0044e3f0 does,
  including the read into the next row past the right edge; past the whole
  grid it answers 0 where the original reads beyond its array. Built when the
  session seeds its settlements, which is where the AI manager starts, and
  saved with its slot grid (`save.md`, the session section). The time of the
  cut matters: a building's entrances are the doors the passability layer
  lets a unit stand on, so the same world cut a turn later can give other
  slots (on Crossroads, seed 1, 347 of 16,384 and one node fewer). Whether
  the original cuts before or after the first turn's moves has not been
  pinned against a dump of its table. On Crossroads the cut takes about
  3.5 ms. Three things
  differ: the "wholly free" test reads a passability cell past the grid's
  width or height as blocked, where the original reads its bitmap's row
  padding; a `TTent` settlement is seeded without the original's extra test
  on its holder, which this engine does not model; and an outpost whose central
  building has no land entrance gets no second seed, where the original
  compares two uninitialised locals. The shipyard's cached areas (step 11)
  are not kept; `WaterLsa` derives its answer (`sim/economy.cpp`).
* **The per-player memory is the sweep's** (`sweep_gaika`), run on every
  node once a tick after the squads' standing is refreshed, where the
  original runs it on the nodes a unit crosses between. `Explored` and
  `Revealed` read the match's exploration and fog rules first; the scenario
  path's explored-everywhere flag, which a script's `ExploreAll` sets in a
  match with exploration on, is not kept. `Eval/6`, `Eval/3` and
  `EvalNeighbors` add the memory tail.
* **The `MAIKA` list is kept** (`SquadTable::routes`, saved, newest first,
  512 at most): `SetMAIKA` and the attacked hook (`record_squad_attacked`)
  record into it, and `Recruit` runs the routing filter over it.
