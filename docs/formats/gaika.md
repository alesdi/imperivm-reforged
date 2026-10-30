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

One object, its pointer at `DAT_008c8e54`, created by 0x004509f0 from the
builder entry 0x00450aa0 that `LSA()` (0x006a8720) reaches. Every reader tests
the pointer first and answers "no node" (0, an empty list, `Empty` true)
without it. The manager holds:

| offset | what |
|---|---|
| `+0x14`..`+0x18` | the node records, `0x24` bytes each, indexed by node id (`GaikaId`); id 0 is no node |
| `+0x38` | the `MAIKA` list: the way-point records `SetMAIKA` adds (below) |
| `+0x58`, `+0x60` | the slot grids the builder labels (128-unit slots) |

The node under a point is a slot-grid lookup: 0x0044e3f0 takes a point,
divides it into the 128-unit slot, and reads the node id the builder wrote
there (`& 0xffff`), 0 off the map. `GetGAIKA(point)` (0x005bd090) is that;
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
| `+0x00`..`+0x06` | the bounding rectangle, four 16-bit coordinates |
| `+0x08` | the centre point, what `GetDestPoint` answers for a region node |
| `+0x10` | the passability area (`LSA`) the node lies in |
| `+0x14` | the settlement whose node this is, null for a region node (saved as the settlement's index) |
| `+0x18` | **the squad deque**: a pointer to a `std::deque` of packed squad handles, allocated on the first push and freed (and the pointer nulled) when the last entry is erased |
| `+0x1c` | a 16-bit coverage figure |
| `+0x1e`, `+0x20` | the neighbour count and the neighbour list (0x0044a3c0 links them) |

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

0x0044e050 partitions the map: it builds the passability areas (0x00453130,
the `LSA`), labels the slot grids, seeds a node from every settlement's
town-hall, outpost and gate buildings out to ten slots (0x0044d080), grows the
region nodes over the remaining ground through a chain of passes (0x0044d4c0
to 0x0044ddd0), links neighbours (0x0044a3c0) and closes with three clean-up
passes (0x0044aaf0, 0x0044b740, 0x0044b840, 0x0044bae0).

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
* **The table is an approximation** (`GaikaTable`): one node per settlement
  and one per passability area with no settlement, numbered settlements first.
  The partition 0x0044e050 is not reproduced, so node ids, their extents and
  their neighbours differ from the original's on any map with more than one
  region per area.
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
