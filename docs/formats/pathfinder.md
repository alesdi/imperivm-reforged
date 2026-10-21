# The route search

**Status:** read from `gbr.exe` for the host audit's twelfth and thirteenth rounds. The engine
reproduces the **grid search** (0x0041bc60, `engine/core/src/sim/grid_search.cpp`) -- the
straight line, the A\* with its budget and tie-breaks, the owner's cut-back and the
straightening -- and routes every request with it. It does not build the **smart
pathfinder**, nor switch to it a few seconds into a match; the last two sections say what that
changes.
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
(`0x008379f0`, a 132-wide window) that maps a cell to the goal points it stands for. Before it
searches, every search strikes off the goal points a standing unit or another unit's
destination lock covers (0x0040a310; `sim/avoidance.hpp`).

**The goal window** (0x00409b80, per point). A point is registered only where the grid lets a
unit stand; each cell holds **at most four**, and a fifth in the same cell is not registered at
all -- not as a goal cell, and not in the point list either. While points are registered, the
one nearest the band's centre (the centre of the window's rectangle, by squared distance; the
first on a tie) becomes **where the search heads**, `0x008bfc10`; the strike-off (0x0040a310)
puts the centre itself there when no point was registered. It is chosen before anything is
struck off, so it can be a taken point. The strike-off then rebuilds the point list from the
points still live, in the order they were registered. A cell counts as a goal cell when the
**first** point registered in it is still live; its other slots are never read by the search.

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

1. **A straight line first** (0x0040b580). Its target is the live goal point nearest the start
   (squared distance in 32 bits, the first on a tie), or where the search heads when none is
   left. Unless the request ignores passability, the **cell line** from the start's cell to the
   target's (0x0040b2b0, a Bresenham walk written out for each of the eight octants, with its
   own error terms) must be clear: each cell it visits free, and no diagonal step from one cell
   to the next passing between **two** blocked corners -- one free corner lets it through
   (0x0040b1b0; the walk's "previous cell" starts at (-100,-100), so the first cell is never a
   diagonal step). If the line is not clear the A\* follows. If it is, and the request has an
   owner and no goal point is left, the route walks from the target back towards the start in
   `n` equal steps, `n` the integer square root of the distance over the owner's radius, each
   step a whole-number division of the difference: the first probe that is a free spot
   (0x0040a990) ends the route, status 0; with none (or `n` of 0) the route is the start twice,
   status 1. Otherwise the route is start and target, status 0.
2. **Otherwise A\*** (0x0041ba00 -> 0x0041b140), with the start's cell made passable for the
   search and restored after the route is laid:
   * **cells, four-connected**, visited in the order (x, y+1), (x-1, y), (x, y-1), (x+1, y);
     a blocked cell (the grid bit set) is never entered;
   * **costs in 16.16**: a step costs 2.0; the estimate is twice the straight distance from the
     cell to the heading's cell (each coordinate shifted down four bits), computed in floating
     point and truncated, `trunc(sqrt(dx^2 + dy^2) * 131072)`;
   * a neighbour is relaxed when the new cost is **no greater** than the one it has, as
     unsigned numbers; on an **equal** cost only when the neighbour's x and y differ in parity
     (`(x ^ y) & 1`). A relaxed neighbour that is not waiting in the heap -- never seen, or
     already expanded -- is given a fresh estimate and pushed (an expanded cell can be pushed
     again, and so popped again); one already waiting only has its cost and parent changed,
     **without being moved in the heap**;
   * **the heap** is the compiler's library heap (push 0x0041b0b0, pop 0x0041adf0/0x0041ada0)
     as a min-heap of cells, each comparison reading a cell's cost plus estimate as it stands
     then, as a 32-bit unsigned sum. A push moves the new cell up past every parent strictly
     greater; a pop moves the last cell to the top's place by the library's hole method --
     down by the smaller child, taking the right one unless the left is strictly smaller, then
     back up -- so equal keys come out in an order that depends on the heap's history;
   * **the budget** is 8,000 (`[this+0x20]` set at 0x0041b155), counted down after each pop;
     the pop that finds it already at 0 ends the search, so **8,001 cells are popped at most**;
   * after a pop, the search ends **at a goal cell** (its first point live) at that point,
     status 0. Otherwise the cell's neighbours are relaxed and then, if its estimate is the
     smallest seen (strictly; the first wins), it becomes the best cell; it is then marked
     expanded. When the heap is empty or the budget is spent, the answer is **the best cell**:
     the expanded cell with the smallest estimate, status 2 -- status 1 instead when no goal
     point was live at all.
3. **Where the route ends** (0x0041ba00). At a goal cell, the goal point. Otherwise the best
   cell's centre -- and for an **owner**, the centre of the first cell walking back along the
   parents from the best cell whose centre is a free spot (0x0040a990); none (back to the
   start) ends the route at the start.
4. **The cell chain** runs from the end's cell back along the parents; where a cell's
   grandparent is a diagonal step away, the parent between them is skipped, so every
   L-shaped corner is cut. Reversed, it runs from the start.
5. **Straightening** (0x0040c6b0) turns the chain into points: a chain whose first cell is its
   last is that cell's centre. Otherwise, from an anchor -- the first cell to begin with -- it
   looks for the farthest chain cell visible from it, scanning down from the chain's end and
   testing a cell when it is the last or differs from the anchor. With the live grid search's
   flag clear it tests the cell line **from the candidate back to the anchor** only (the line is
   not symmetric, so the other direction may graze a corner); with the flag set (the smart
   pathfinder's build) it needs both directions clear. The scan steps down by one, or by a
   seventh of the distance once that is at least the limit (50 to begin with); each step adds
   250 to the limit, and past 500 it gives up: the last point laid is taken back, the anchor
   cell's centre laid and made the anchor, and the limit reset. A scan that reaches the
   chain's end lays the anchor's centre and stops. Otherwise the anchor's centre is laid, and
   the next anchor is found: with the flag set, the visible cell itself; with it clear, the
   first cell beyond the visible one whose line back to the anchor is blocked is found (one
   line state carried across those tests), a box of two cells either way round the blocked
   cell is drawn, and the walk from the cell before it towards the anchor (0x0040c3f0) takes
   its first cell inside that box -- or the visible cell, if none is -- as the next anchor,
   with the limit reset.
6. The end point is added, the first point is set to the start, and the length is the sum of
   the legs' integer square roots shifted up sixteen bits (0x0041a8f0). The search answers
   whether that is non-zero.

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

The goal points are one ring of sixteen at exactly the range round the goal (0x00409cf0,
each tested passable, 0x00409b80), or the goal alone for a range of 0, so a unit already
inside the band is answered the walk out to the ring. The start of a unit at `(-1,-1)` is
the exit point of its holder's settlement's central building towards the goal
(0x005de2b1 .. 0x005de30b: `[holder+0xc]`, `[settlement+0x8c]`, 0x004db6c0 with the class's
`water_unit`), which takes the door picker's one draw (0x0056fa40) when the building has
doors and is the building's position when it has none.

This engine does the same (`path_to_goals`, `path_to_start`, `path_to_impl` in
`sim/movement.cpp`): the whole ring, in ring order, goes to the grid search with the point
nearest the goal as where it heads.

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

**The grid search, reproduced** (`sim/grid_search.hpp`, `GridSearch`), and called for every
route -- `RecastPathfind`'s (`MovementSystem::lay_path`) and `PathTo`'s -- through
`PathFinder::find` (`sim/path.hpp`). Everything in "The grid search" above is built from this
prose: the straight line and its cell line, the four-connected A\* with its 16.16 costs, its
estimate, its parity rule, its re-pushed and un-moved cells and the library heap's order, the
8,000-cell budget and the best cell, the owner's cut-back, the corner-cutting chain and the
straightening with the live search's flag clear. The estimate's floating-point square root is
taken as the exact integer `isqrt(n << 34)`, which is `trunc(sqrt(n) * 131072)` for every
distance under 1,024 cells, assuming the x87 rounds to 53 bits as the C runtime sets it.

**The request** carries what the original lays out in globals: `ring_goal` lays the band's
rings and marks each point live or struck (`0x00417830`, `0x0040a310`), the goal window keeps
four to a cell and reads each cell's first slot (`GoalCells`), and where the search heads is
the point nearest the centre before any is struck. The owner is the unit when its order holds
a lock, asked through `spot_free` with its radius.

**A gate's line** is laid for the search round it as the original lays it into its grid
(`PathRequest::barrier`, `sim/gate.hpp`); the search is the same grid search, budget and all.

`SetDest`'s new-goal branch on a stop bit is reproduced (`run_goto`, `order_goto`).

## What differs, and is still unknown

* **The smart pathfinder is not built, and the switch is not modelled.** On a shipped install
  (`NewPF = 1`) the original routes with the grid search only until `data/pathfinderinit.vs`
  has built the smart pathfinder and `SPFDone` installs it -- some seconds to a minute or more
  of a match, by the script's sleeps and the map's size. From then on a route whose goal lies
  outside the start's area and those joined to it is laid by the area search: **no 8,000-cell
  budget**, a partial route that ends where the line from the goal enters the area whose seed
  is nearest it, and a route along seeds and connection runs. This engine routes with the grid
  search throughout, as the original does with `NewPF = 0`. So a far goal round a long wall,
  which the original's smart pathfinder reaches, can run out of budget here and be answered
  with a partial route, and every far route's shape -- and `PathTo`'s length -- is the grid
  search's.
* **What the build would need**, read for this round but not written: the areas (0x00410fd0
  seeds on a 4-cell lattice, 0x00411110 grows each breadth-first in 4x4 blocks offset by two,
  crossing a block's edge only within 48 cells of the seed, with rules for taking cells from a
  neighbour that were not fully read), the cells no area took **set blocked in the live grid**,
  the direction field (0x0040db30), the 4x4 cell-to-area map with its overflow table
  (0x00411c00), each pair of touching areas joined by a run laid with the structure's own grid
  search with the straightening flag set (0x00415620), and the extra pairs `SPFIncreaseConnect`
  adds where going through a middle area costs more than 1.3 times the straight distance
  (0x00415e30). The search's end (0x0040e500, 0x00412040), its route assembly (0x00411ee0,
  0x00412a70, 0x00412260) and the area search's tie order were not read closely enough to
  write.
* **When the structure changes.** The original builds it once per map load (0x0040acb0); the
  other caller (0x006a4140) is a console command. Nothing patches it as the grid changes, and
  this engine's grid does not change in play either.
* **The "incomplete" mark is not the status alone.** This engine keeps whether a route is
  complete (`MoveState::path_complete`) for arrival as well as for `FormPathLeft`, and counts a
  route incomplete when it was re-aimed at a free spot or laid with no goal point left -- even
  where the straight line's walk back to a free spot gave it status 0 -- unless it ends inside
  the band. The original's `FormPathLeft` reads the status, so for those two routes it adds no
  32,000 where this engine does.
* The live structure is saved with the game (`pathfinder`, 0x0040b130); this engine has nothing
  to save, the grid search holding no state between searches but scratch.
