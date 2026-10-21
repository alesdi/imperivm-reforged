# The route search

**Status:** read from `gbr.exe` for the host audit's twelfth and thirteenth rounds. The engine
reproduces both route searches -- the **grid search** (0x0041bc60,
`engine/core/src/sim/grid_search.cpp`) and the **smart pathfinder** (0x004138d0,
`engine/core/src/sim/smart_path.cpp`) with its whole build -- and the switch from the one to the
other when `data/pathfinderinit.vs` has built the second. The last section lists what is
guarded or chosen here.
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

## The smart pathfinder: the build

The structure (0x9c bytes, made by 0x00416200) partitions the free cells into **areas**, keeps
in every cell a two-bit **direction** back to its area's seed, maps every cell to its area
through **4x4 blocks**, and joins areas by **connections** that carry a cost and a run of
points between their two seeds. It is built in the stages the nine `SPF*` commands drive
(below), on a builder (0x0040ff80) that holds the smart object, an **areas finder**
(0x0040fe00), the connect pass's range and counter, and the list `SPFIncreaseConnect` fills.

**The areas finder** (0x0040fe00) works on a square of `N x N` cells, `N` the map's size in
cells (the world's extent plus one, over 16). It takes a **copy of the live grid** as it is
when the builder is made, and every later test of "free" in the finder reads that copy. Each
cell holds an area id (16 bits) and a distance (16 bits), both all-ones to begin with
("unassigned"). It keeps a list of **candidate cells**, the list of **seeds** found so far, the
number of areas, and a lattice cursor that starts at cell (3, 3).

**One area per call** (`SPFFindAreas_Quant` -> 0x00411af0):

1. **The seed** (0x00410fd0). The candidate list is filtered in place, keeping in order only
   the cells whose distance is **exactly 48** now. If any is left, the seed is the first (it
   stays in the list; once grown it holds distance 0 and the next filter drops it).
   Otherwise the lattice cursor scans on: along a row in steps of four cells, and at the row's
   end back to x = 3 and four rows down, for a cell that is free and unassigned; the cursor
   stays on the cell it answers. Nothing found: every cell of the square whose area is
   unassigned is **set blocked in the live grid**, and the call answers 0.
2. **The growth** (0x00411110, limit 48), the new area's id being the number of areas so far.
   The seed is set to distance 0 and put on a first-in first-out queue with distance 0. For
   each cell taken off the queue, with `d` the cell's queue distance plus one, its four
   neighbours are looked at in the order (x, y+1), (x, y-1), (x+1, y), (x-1, y); a neighbour
   blocked in the copy is passed over. For a free one, the step **crosses a block edge** when
   it leaves the 4x4 block the cell is in (blocks start at cell 2, so a step from y to y+1
   crosses when y is 1 more than a multiple of four, from y to y-1 when y is 2 more, and the
   same along x). The neighbour is **taken** -- set to this area at distance `d` and queued
   with `d` -- unless both
   * the step crosses a block edge, or the neighbour already belongs to this area; and
   * `d` is over 48, or the neighbour's distance is already `d` or less.

   So inside its block a growth takes every free cell, whoever held it and however far; across
   a block edge it takes a cell only within 48 steps and only by a shorter way. When it does
   not take a neighbour that belongs to **another** area, it looks whether that neighbour
   still has a way back to its own seed: a neighbour of it (left, right, then the two along y)
   in its area at one step less. With none -- its way back was taken from under it -- the
   neighbour is taken after all. Otherwise, and for a neighbour of this area or an unassigned
   one, the neighbour is added to the candidate list when `d` is over 48.
3. The seed is added to the seeds and the count goes up; the call answers 1.

The script calls it until it answers 0. **`SPFInitData`** (0x004166b0 -> 0x00416520 ->
0x00416200) then makes the structure: one 44-byte record per seed, in seed order, its id that
index and its point the seed cell's centre, with an empty list of neighbours; a grid search of
its own (0x0041ab90) with the straightening flag `[+0x18]` **set**; an empty connection list.

**`SPFInitDirectionData`** (0x0040dd10 -> 0x0040db30): for every cell with x and y from 2 to
`N - 3`, the direction is the first neighbour, in the order (x-1, y), (x+1, y), (x, y-1),
that holds the same area at one step less (0, 1, 2), and otherwise (x, y+1) (3). Cells outside
that range keep 0.

**`SPFInitPointToAreaData`** (0x00412980 -> 0x00411c00): a block per 4x4 cells, block (i, j)
covering cells 2 + 4i .. 5 + 4i across and 2 + 4j .. 5 + 4j down, all set to all-ones. The
cells with x and y from 2 to `N - 3` are visited row by row: a block still all-ones takes the
cell's area (all-ones too, if so); a block holding another real area under 0x8000 when the
cell has a real area of its own **overflows** -- the sixteen area ids of its cells, column by
column, go to the end of an overflow list, and the block becomes 0x8000 plus that list index.
**The lookup** (0x0040e340) from a world point: the cell less two along each axis, its block;
under 0x8000 that is the area, otherwise the entry of the block's overflow table for the
cell's place in the block.

**`SPFInitConnectData`** (0x0040d1d0) sets the connect pass's range: x and y from 2 to `N - 3`,
the row counter at 2.

**`SPFCalcConnectHighRes_Quant`** (0x00415a40 -> 0x00415880): the counter's row. Along it,
every cell with a real area is **joined** to the areas of its four neighbours in the order
(x-1, y), (x, y-1), (x+1, y), (x, y+1). The counter goes up; the answer is whether it is still
within the last row.

**Joining two areas** (0x00415620): nothing when the two are the same, or the other is
all-ones or 0x8000 and over. The lower id is `a`, the higher `b`; nothing when `b` is in `a`'s
list already. The connection's index is the number of connections; `a`'s list gets
(`b`, index) and `b`'s gets (`a`, index). The run is **the structure's grid search** from
`a`'s seed with the goal window at `b`'s seed alone (0x00409b20 radius 0, 0x00409b80), no
owner, passability not ignored, straightening flag set: the connection's **cost** is that
route's length (the sum of its legs' integer square roots) cut to 16 bits, its run the route's
points less the first and the last, so the run goes from `a`'s seed to `b`'s, and it starts
enabled.

**`SPFIncreaseConnect`** (0x004161d0 -> 0x00415e30) collects pairs: for each area `A` in id
order, for each (`B`, connection) in its list in order, for each (`C`, connection) in `B`'s
list in order, the pair (`A`, `C`) when 1.3 times the integer square root of the squared
distance between `A`'s and `C`'s seeds is under the two connections' costs added (a double,
`1.3` at 0x007ac2f0: exactly `13 * d < 10 * sum` for these integers). `C` may be `A` or
already a neighbour; joining skips those. **`SPFIncreaseConnect_Quant`** (0x00415a90 ->
0x004159b0) joins the next ten pairs and answers whether any are left.

**`SPFDone`** (0x0040f8c0) destroys the live grid search and installs the smart object; the
builder and the finder with its cells go.

## The smart pathfinder: a search (0x004138d0)

Every record's two flags are cleared, its estimate set to 0 and its cost to all-ones, and the
structure's grid search has its straightening flag **cleared** (0x0040e260) -- so every grid
search the smart pathfinder makes from now on straightens as the live one does. The goal
points are struck off (0x0040a310). Then:

1. **Ignoring passability**: the grid search answers.
2. **A walled-in start** (its cell blocked in the live grid) is moved to the first of twelve
   probes that is on the map and free: 16 units down, up, right, left, then 32, then the four
   diagonals (+16,+16), (-16,-16), (+16,-16), (-16,+16) (0x0040dc90). With none, the route is
   the start twice, status 2, and the search answers 0.
3. **The start's area** (the lookup of the possibly moved start) is marked joined (`+0x1b`),
   and so is every area in its list. Every live goal point's area is marked a goal (`+0x1a`);
   one that is marked joined makes the goal **near**. With no live goal point, the area under
   where the search heads is marked a goal instead, when that point's cell is more than one
   cell from the top and left edges, on the map and free.
4. **A near goal, the start not moved**: the grid search answers, budget and all.
5. **The straight line** (0x0040b580) as in the grid search; if it lays the route the search
   answers whether its length is non-zero.
6. **The area search** (0x00413690): A\* over the records from the start's area, each node
   costing its parent's cost plus the connection's cost, its estimate the straight distance in
   world units from its seed to where the search heads (the floating-point root of the
   squared distance, truncated: the integer square root), the heap the library heap of the
   grid search (0x0040e0f0 up, 0x0040e140 down) keyed on cost plus estimate as they stand.
   Budget 65,536 pops (the 65,537th ends it). A popped area marked a goal ends it, **arrived**.
   Otherwise each neighbour whose connection is enabled is relaxed when the new cost is
   **strictly** lower; one waiting in the heap (an estimate neither 0 nor all-ones) only takes
   the cost and the parent, any other is given its estimate and pushed. Then the popped area
   becomes the best if its estimate is strictly the smallest so far (the start's counts), and
   is marked expanded (estimate all-ones). An empty heap or the budget ends it **not arrived**,
   at the best area. With no live goal point it is never arrived.
7. **The route's end.**
   * **Arrived**: of the live goal points in the reached area, the one nearest (integer square
     root, the first on a tie) to the reached area's parent's seed -- the start, when the
     start's area is the goal's. The **walk** (0x00412040) from it to the reached area's seed
     must succeed; otherwise the route fails.
   * **Not arrived**: a cell line from where the search heads (its cell) towards the best
     area's seed's cell (0x0040e500, the same eight octants), stopping at the first cell that
     is more than one cell from the top and left edges, on the map, free, and whose lookup is
     the best area (0x0040e3d0). Its centre is the end, and the walk from it to the seed must
     succeed or the route fails. With no such cell, the end is the seed itself.
   * **The walk** (0x00412040) from a point to an area's seed follows the directions from the
     point's cell -- 0 left, 1 right, 2 up, 3 down -- adding each new cell's centre, until it
     reaches the seed's cell; a walk that comes back to a point it has laid (the test is made
     once it is over a hundred points long) fails.
8. **The route's points**, end first: the end's walk to its area's seed; then for each area
   from the reached one back to the start's, its seed followed by the run of the connection to
   its parent (as stored when the area's id is the lower, reversed otherwise); then the start's
   area's seed; then the walk from the start to that seed, reversed. A failed walk fails the
   route: the route is the start twice, status 1 with no live goal point and 2 otherwise, and
   the search answers 0.
9. **The cells.** Between each pair of consecutive points, from the last pair to the first, a
   cell line (0x00412a70, the same octants) lays every cell, the last cell of each line but the
   final one dropped; so the cells run from the start to the end.
10. **The patch** (0x00412260). The cells are copied while free in the live grid. At a blocked
    one, the cell before it is remembered, the blocked ones are skipped to the next free cell
    -- none, and the rest are dropped -- and the gap is filled by **the grid search's A\*
    alone** (0x0041ba00, no straight line, no straightening) from the cell before to the free
    cell, with the goal window reset to that cell's centre alone and no owner; its cells less
    the first and the last are copied in, and the copying goes on from the free cell. A patch
    leaves the goal window holding that one point.
11. **An owner's route that did not arrive** is cut back: while more than one cell is left and
    the last cell's centre is not a free spot for the owner (0x0040a990), the last cell is
    dropped. The end becomes the last cell's centre, or the start when one cell is left.
12. **Straightening** (0x0040c6b0) with the flag clear and the live grid, as in the grid
    search; the first point becomes the start, the end is added, and the length is taken.
    **Status**: no live goal point (in the window as the patches left it), 1; arrived, 0;
    otherwise 2. The search answers whether the length is non-zero.

### Gates, for the smart pathfinder

The route caller's second search, the one with every enemy gate laid (`sim/gate.hpp`), lays
and lifts each gate through the installed search's own methods (its table's `+0x10` before
the search, `+0xc` after, 0x0041945f and 0x00419537). The grid search's write the line's
cells into its grid. **The smart pathfinder's touch no cell**:

* **Laying** (0x00414ae0) looks the gate up by its id in a map the structure keeps; the
  first time, it lists the gate's connections (0x00412540): for each area in id order, for
  each entry of its list whose neighbour has a higher id, the connection's route is laid out
  -- the area's seed, the run, the neighbour's seed -- and its legs are met one by one against
  the gate's axis (0x00529380) by the same segment test the route's crossings use
  (0x0040aab0, ends included); the first leg that meets it lists the connection. Then every
  listed connection is **closed** (its record's byte `+2` set), and the area search does not
  follow a closed connection.
* **Lifting** (0x00415950) finds the gate in the same map and opens its connections again.

So with the smart pathfinder a gate bars the areas' joins that run through it and nothing
else: the near hand-off to the grid search, the straight line and the patches all read the
grid without it.

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
inside the band is answered the walk out to the ring. The ring builder stops after its first
point only for a range of exactly 0: a negative range lays all sixteen points, each the range
times its direction, which is the same ring at the range's size started from the opposite
side. So `PathTo(pt, -100)` is answered the walk to the ring of radius 100, not to the point;
which point is nearest is the same either way, and only a tie between two is broken the other
way round. The rest of the call adds nothing: the third argument is popped and never read,
the object form's goal is the target's `posRH` (`vtbl+0xc8`, the position for anything that
is not a unit) with both class radii added to the range, and no owner is named, so no goal
point is struck off as taken (0x0040a310 strikes only for an owner). The held start reads
the holder's settlement and its central building without testing either for null; this
engine falls back to where the unit resolves instead of faulting. The start of a unit at `(-1,-1)` is
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

**Both searches, reproduced, and the switch between them.** `PathFinder::find`
(`sim/path.hpp`) answers every route -- `RecastPathfind`'s (`MovementSystem::lay_path`) and
`PathTo`'s -- with the grid search (`sim/grid_search.hpp`, `GridSearch`) until the smart
pathfinder (`sim/smart_path.hpp`, `SmartPathfinder`) is installed, and with the smart
pathfinder from then on, which hands its near goals back to the grid search. Everything in
"The grid search" and the two "smart pathfinder" sections above is built from this prose.

**The grid search**: the straight line and its cell line, the four-connected A\* with its 16.16
costs, its estimate, its parity rule, its re-pushed and un-moved cells and the library heap's
order, the 8,000-cell budget and the best cell, the owner's cut-back, the corner-cutting chain
and the straightening with either flag. The estimate's floating-point square root is taken as
the exact integer `isqrt(n << 34)`, which is `trunc(sqrt(n) * 131072)` for every distance under
1,024 cells, assuming the x87 rounds to 53 bits as the C runtime sets it.

**The build** is the shipped configuration's (`NewPF = 1`): `GameSession::start_match` makes
the builder over the grid as the match start leaves it -- after the passability layer is rebuilt
-- and spawns `data/pathfinderinit.vs`, which drives it through the nine commands
(`sim/ai.cpp`) with its own sleeps on the scheduler's clock, so the grid search answers for as
long as the script runs and the smart pathfinder after its `SPFDone`. The areas finder works on
its copy, sets the cells no area took blocked in the live grid (without a new grid generation:
nothing is searched again for it), and the connections are laid by the grid search with the
straightening flag set. The 1.3 test is the exact integer `13 d < 10 sum`.

**The smart search** in full: the twelve probes, the joined and goal marks, the near hand-off,
the straight line, the area A\* with its heap, budget and best area, both ends (the nearest goal
point by integer root; the cell line towards the best seed), the direction walk, the route of
seeds and runs, the cell lines, the patch with the grid search's A\* alone, the owner's
cut-back, the straightening and the status as the patched goal window leaves it.

**A saved game** keeps the build as the calls made on it (`PathFinder::smart_calls`, the
movement section); a load makes the builder over the saved map's grid and makes the same calls
again, which gives the same areas, the same blocked cells and the same connections, because the
grid this engine searches does not change in play.

**The request** carries what the original lays out in globals: `ring_goal` lays the band's
rings and marks each point live or struck (`0x00417830`, `0x0040a310`), the goal window keeps
four to a cell and reads each cell's first slot (`GoalCells`), and where the search heads is
the point nearest the centre before any is struck. The owner is the unit when its order holds
a lock, asked through `spot_free` with its radius.

**A gate** that bars the mover is handed to whichever search is installed, as the original
hands it (`sim/gate.hpp`): to the grid search as its line's cells (`PathRequest::barrier`),
read wherever it reads the live grid; to the smart pathfinder as the connections whose
routes meet the gate's axis, closed for the one search (`PathFinder::close_gate`), the grid
left bare. Each gate's connections are found once and kept by its id.

`SetDest`'s new-goal branch on a stop bit is reproduced (`run_goto`, `order_goto`).

## What differs, and is still unknown

* **Guards where the original reads memory it never wrote.** A point within two cells of the
  map's top or left edge, or past the block map's last block, has no area here (the original
  reads outside its block map); a start with no area goes to the grid search, and a goal point
  or heading with none marks nothing. A direction walk that leaves the field fails. A command
  with no builder does nothing and answers false, and an `SPFDone` before `SPFInitData`
  installs nothing (the original would install a null structure). None of these is reached on
  a map with a blocked border.
* **A walk that loops** is caught by the cells it has walked, where the original looks for a
  point it has laid once the walk is over a hundred points long. The step depends on the cell
  alone, so both fail exactly the walks that loop and lay the same points for the rest.
* **What is saved.** The original writes its live structure into the saved game
  (`pathfinder`, 0x0040b130); this engine writes the build's calls and makes it again on a
  load. The two agree while the grid is the one the build copied, which in this engine it
  always is; whether the original restarts its script on a load that restores a finished
  structure was not read.
* **When the structure changes.** The original builds it once per map load (0x0040acb0); the
  other caller (0x006a4140) is a console command this engine does not have. Nothing patches it
  as the grid changes, and this engine's grid does not change in play either.
* **A gate's connections are kept by its id** as the original keeps them, and found again
  here if the gate's axis has changed since, which no gate's does. The map is not saved; a
  load finds them again from the same axes.
* **`NewPF` is not read**: the shipped value, 1, is the only one modelled. The original reads
  it from `config.ini` (0x0074cd20); 0 would route with the grid search for good and 2 build
  at once before play.
* **The script's place among the scripts.** `data/pathfinderinit.vs` is spawned before the
  victory scripts at match start; the original attaches it to the world at priority 100
  (0x006a07e0), and the scheduler's order of two threads due at the same moment is this
  engine's.
* **The "incomplete" mark is not the status alone.** This engine keeps whether a route is
  complete (`MoveState::path_complete`) for arrival as well as for `FormPathLeft`, and counts a
  route incomplete when it was re-aimed at a free spot or laid with no goal point left -- even
  where the straight line's walk back to a free spot gave it status 0 -- unless it ends inside
  the band. The original's `FormPathLeft` reads the status, so for those two routes it adds no
  32,000 where this engine does.
