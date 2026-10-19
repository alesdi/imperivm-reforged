# The route search

**Status:** read from `gbr.exe` for the host audit's twelfth round. The engine models what a
search answers -- a goal it can reach is reached, one it cannot is answered with a partial
route to the reachable point nearest it, and the "incomplete" mark that follows the route --
and when a unit searches again (`engine/core/src/sim/path.cpp`, `sim/movement.cpp`). It does
not model the two search structures themselves, nor the switch from one to the other a few
seconds into a match; the last section lists what differs.
**Not a file format.** Nothing here is on disk, except that the live structure is written into
a saved game (below). It is the runtime model behind every route the scripts ask for, kept
beside the host API ([`vs-host-api.md`](vs-host-api.md)) because `Goto`, `PathTo`,
`FormPathLeft` and the nine `SPF*` commands answer through it.

Prose only, with addresses so each claim can be checked; [`../legal.md`](../legal.md) rule 2.

## Two searches, one pointer

Every route search goes through one object, the pointer at `0x008c528c`, by its third virtual
slot (`vtbl + 8`). Which object that is depends on `NewPF` in `config.ini`'s `[system]`
section (read at 0x0074cd20 into `0x00a877b0`); the shipped file says `NewPF = 1`. At map
load (0x00526200 -> 0x0040acb0, unless the session has none):

* **0** makes a *grid search* (constructed at 0x0041ab90, vtable 0x007ac12c, search
  0x0041bc60) and nothing else;
* **1**, the shipped value, makes the same grid search the live object **and** starts the
  build of the *smart pathfinder* (0x004165a0): a builder at `0x008c529c` and the script
  `data/pathfinderinit.vs`, attached to the world at priority 100 (0x006a07e0). The script
  drives the build a slice at a time with `Sleep` between slices; its last command, `SPFDone`
  (0x0040f8c0), destroys the grid search, installs the built structure as the live object and
  frees the builder;
* **2** builds the smart pathfinder at once, before play (0x00416380 with its build flag),
  and prints the memory it took.

So on a shipped install a match routes with the grid search for as long as the script takes --
one area per `SPFFindAreas_Quant` and 30 ms of sleep, one connect row per
`SPFCalcConnectHighRes_Quant` and 23 ms, and so on -- and with the smart pathfinder from then
on. A second entry with no caller in the executable (0x006a4140, a console command's body)
starts the same build again. The live object is saved with the game under the name
`pathfinder` (0x0040b130), and a debug pair writes and reads it as `pfsave.sav`
(0x006a4480).

## What every search is handed

The caller lays out a request in globals before the call: the goal point the route is for
(`0x008bfc10`/`0x008bfc14`), the band's goal points round it (the point list at `0x008c517c`,
filled by 0x00417830 for a route and by 0x00409b20 for `PathTo`), and a grid of goal cells
(`0x008379f0`, a 132-wide window) that maps a cell to the goal point it stands for. Before it
searches, every search strikes off the goal points a standing unit or another unit's
destination lock covers (0x0040a310; `sim/avoidance.hpp`).

The search fills a path record (`CVXPath`, constructed at 0x0040bcf0):

| offset | what |
|---|---|
| `+0x08` | a constant the constructor is given (10 for `PathTo`, a speed-derived value or 200 for a route) |
| `+0x18` | `+0x08` shifted up sixteen bits (0x0041a8f0) |
| `+0x28..+0x30` | the route, a vector of points; the first is the start |
| `+0x34` | the route's length in 16.16: the sum over its legs of each leg's integer square root (0x0041a8f0) |
| `+0x38` | the status: **0** complete, **1** no goal point was left, **2** incomplete |

Its slot `+0x14` answers the length and `+0x20` the status. The search answers **whether the
length is non-zero**: a route that goes nowhere is no route.

## The grid search (0x0041bc60)

1. **A straight line first** (0x0040b580). The goal is the goal point nearest the start (by
   squared distance), or the request's goal point when none is left. If the line from start
   to it crosses no blocked cell (0x0040b2b0) the route is those two points, status 0. With
   no goal point left and an owner (a route that holds a destination lock), it instead walks
   from the goal point towards the start in steps of the unit's radius for the first free spot
   (0x0040a990) and, finding none, lays a route that goes nowhere, status 1.
2. **Otherwise A\*** (0x0041ba00 -> 0x0041b140) over the grid's cells, **four-connected**:
   a step costs 2.0 and the estimate is twice the straight distance to the goal point's cell,
   both in 16.16, so the search is A\* with an exact Euclidean heuristic; of two equal-cost
   parents, the cell's coordinate parity decides. The start's cell is made passable for the
   search and restored after. A popped cell in the goal-cell window ends the search at that
   cell's goal point, status 0. **The budget is 8,000 expanded cells** (`[this+0x20]` set at
   0x0041b155). When it runs out, or nothing is left to expand, the search answers the
   expanded cell nearest the goal point -- the smallest estimate -- with status 2; with no goal
   points at all it answers status 1.
3. **An owner's unfinished route is cut back** (0x0041ba00): when the search did not reach a
   goal cell and the request names an owner, the route's end steps back along the parents
   until the free-spot test passes.
4. The cells become points and the route is straightened (0x0040c6b0).

## The smart pathfinder (0x004138d0)

The structure the script builds partitions the free cells into **areas**, each grown from a
seed up to 48 cells out (`SPFFindAreas_Quant` -> 0x00411af0 -> 0x00411110 with 0x30), records
in every cell a two-bit direction that leads back to its area's seed (walked by 0x00412040),
and joins touching areas by **connections** that carry a precomputed run of points between
the two seeds (`SPFCalcConnectHighRes_Quant`, laid by 0x00411ee0). A search:

1. **A start walled in is refused.** If the start's cell is blocked and so are twelve probes
   round it (16 and 32 units along each axis, 16 along each diagonal; 0x0040dc90), status 2
   and no route.
2. **The start's area and every area joined to it are marked** (`+0x1b` of the 44-byte area
   record), and every area holding a goal point (`+0x1a`).
3. **A goal point in a marked area, with the start unmoved, goes to the grid search** above
   (`[[this+8]]`'s own slot `+8`), budget and all. A request with the ignore-passability flag
   goes there too, and the grid search's straight line answers it.
4. **Otherwise the straight line** (0x0040b580) as in the grid search.
5. **Otherwise the area search** (0x00413690): A\* over areas from the start's, the open list
   ordered by the cost so far (the connections' stored costs) plus the straight distance from
   the area's seed to the goal point (0x0040e0f0), **budgeted at 65,536 areas**
   (`[this+0x20] = 0x10000`), which no map comes near. It stops at the first area holding a
   goal point. If none is reached it answers **the expanded area whose seed is nearest the
   goal point** and a flag that it did not arrive.
6. **The route's end.** Reached: of the goal points in the reached area, the one nearest the
   previous area's seed (the start, when the goal area is the start's), provided the direction
   field leads from it to the area's seed. Not reached: the first passable cell of the best
   area on the line from the goal point towards its seed (0x0040e500, testing with
   0x0040e3d0), or the seed itself if the line finds none.
7. **The route.** Start to its seed by the direction field, seed to seed by the connections'
   runs, the last seed to the end by the field reversed; Bresenham lines (0x00412a70) fill
   the gaps cell by cell, and a pass straightens it (0x00412260).
8. **An owner's unfinished route is cut back**, as in the grid search: when the area search
   did not arrive, the end steps back cell by cell until the free-spot test passes
   (0x004141bd .. 0x00414263).
9. **Status**: no goal point left, 1; arrived, 0; not arrived, 2. A route that failed to be
   laid at all (the end not on the direction field) is the start twice, status 1 or 2, and
   answers 0.

## The "incomplete" mark

`CVXPathRetry`'s 0x004185a0 answers the route's remaining length, plus **32,000** when its
status is not 0. `Hero::FormPathLeft` (0x0052ebc0) answers that for the hero's formation
route, which is how `HERO_ENTER.VS` and `HERO_TELEPORT.VS` tell a route that will not arrive
from one that is nearly there.

## `PathTo`

`Unit::PathTo` (0x005de190, point; 0x005de390, object) builds a path record with constant 10,
lays the goal points (0x00409b20, 0x00409cf0) and asks the live object, from the unit's
position (or, for a unit at `(-1,-1)` in a holder, an exit point, 0x004db6c0), with its class
radius and its class's ignore-passability flag. It answers **`+0x34` shifted down sixteen
bits** -- the length of whatever route the search laid, complete or not, with no 32,000 --
and -1 when the search answered 0. A unit aboard a ship answers 1,000,000 without searching.

## Who searches, and how often

Only `RecastPathfind` (0x00419730) searches for a route a unit walks, through 0x00419110,
and it is called from `SetDest` (point 0x0041a4c0, object 0x0041a5c0) and from a formation's
sample step (0x005f5bd0, 0x005f5e30, 0x00419b20). It first clears the follower's two stop
bits (`[retry+0x20] & 6`); a unit already arrived (`IsArrived`, 0x00417c10) is not searched
for. When 0x00419110 answers 0 the retry keeps the route it was walking, and a retry not aimed
at a formation sets bit 2 (0x00419a1e); the follower (0x00419ee0) then walks that old route
on to the first free spot, as `Unit::Stop`'s bit 1 does.

`SetDest` is the `Goto` family's, the march's and the formation's way in. It caps the range
at 999 and then takes its **new-goal branch** -- forget flags 1, 2, 3 and 0x80 (flag 8 is the
free-spot search's), keep 0x40, set 0x10, search -- **when either stop bit is set**, or the
goal, band, inner bound or formation differ from the retry's (`test al, 6` at 0x0041a4da and
0x0041a5da, before any comparison). Otherwise it clears the stop bits and searches only when
its first argument is 0 and the retry has no route left (`vtbl+0x14` of the media answers 0)
and is not arrived (0x0041a513). The `Goto` family passes as that argument the failing unit's
draw (0x005d2eb0): with its failure stamp set, `rand(0, 2n)`, `n` the whole 2,048 ms spans
since the stamp, at most 10. So:

* a new goal, or the same goal after a search that laid nothing, or after `Stop`, is searched
  **at every call**, whatever the draw;
* the same goal whose route has run out without arriving is searched **on the draw's 0**;
* the same goal with a route still to walk is not searched.

## The `SPF*` commands

The nine commands are the build's driver, and `data/pathfinderinit.vs` is their only caller:
`SPFFindAreas_Quant` (0x004129a0, one area: 0x00411af0, answers whether more remain),
`SPFInitData` (0x004166b0 -> 0x00416520, the structure's record), `SPFInitDirectionData`
(0x0040dd10 -> 0x0040db30, the direction field), `SPFInitPointToAreaData` (0x00412980 ->
0x00411c00, the cell-to-area map), `SPFInitConnectData` (0x0040d1d0, the connect pass's range
over the map less a three-cell margin), `SPFCalcConnectHighRes_Quant` (0x00415a40, one row of
connections, 0x00415880), `SPFIncreaseConnect` (0x004161d0 -> 0x00415e30) and
`SPFIncreaseConnect_Quant` (0x00415a90 -> 0x004159b0), and `SPFDone` (0x0040f8c0, install).
Each reads the builder at `0x008c529c` without a null test.

The searches the AI makes for its own plans do **not** use this structure: `CheckLsaPath`
(0x00423120 -> 0x00441fe0 -> 0x00441b70) and `PrepareAiTransportShip` (0x0042e180 ->
0x00442010) walk the large-scale areas (`sim/lsa.hpp`, 0x00453130), which are another
partition built at load. The smart pathfinder serves `RecastPathfind`, `PathTo`, and a debug
entry (0x006aea70).

## What this engine does

It has neither structure. One A\* (`PathFinder`, `sim/path.hpp`) over the obstruction grid,
eight-connected with no corner cutting (which reaches exactly the four-connected component of
the start), octile costs, answers every request, and the grid's **regions** -- its free cells'
four-connected components, numbered at the first search after a change
(`ObstructionGrid::region_of`) -- stand for the area search's reach:

* **a goal in a region the start reaches is searched for with no budget**, and found, as the
  area search finds every goal it can reach;
* **a goal in another region is not searched for**: the route runs to the reachable cell whose
  centre is nearest the goal point and is partial, status 2's counterpart, and the
  "incomplete" mark follows it (`MoveState::path_complete`, `FormPathLeft`'s 32,000);
* **a search round a gate's line** -- the original lays the line into its one grid for the
  search (`sim/gate.hpp`) -- is the one the regions cannot settle, and it keeps a budget of
  20,000 cells and answers the expanded cell nearest the goal when that runs out;
* `SetDest`'s new-goal branch on a stop bit is reproduced (`run_goto`, `order_goto`).

## What differs, and is still unknown

* **The grid search's budget is not reproduced.** Before the smart pathfinder is installed, and
  afterwards for a goal in the start's or a joined area, the original's search stops at 8,000
  expanded cells; this engine's reaches whatever its region holds. A short route rarely needs
  more; a goal near the start but round a long wall would.
* **The switch is not modelled.** For the first seconds of a match the original routes with the
  grid search alone; this engine has one search throughout.
* **The partial route's end is the nearest reachable cell**, where the original's is the entry
  into the area whose seed is nearest the goal; the two agree when the nearest area is the one
  that holds that cell, and need not otherwise.
* **The route's shape differs.** The original's routes follow seeds, connection runs and
  Bresenham lines, straightened; this engine's follow its A\* and a windowed line-of-sight pass.
  Where a unit walks between two points is this engine's own.
* **The cut-back.** The original cuts an owner's route back to a free spot only when the
  search did not arrive (steps 3 and 8); this engine's `truncate_to_free` tests every route an
  owner lays. A route that arrived ends on a goal point no unit or lock covers, which the free
  test usually passes too.
* The straightening passes (0x0040c6b0, 0x00412260), the area search's open-list order on
  ties, and the direction field's construction (0x0040db30) were not read.
* The saved structure (`pathfinder`, 0x0040b130) is not in this engine's saves; nothing here
  needs it, since the regions are recomputed from the grid.
