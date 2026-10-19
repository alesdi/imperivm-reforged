# The object grid and the order the world is visited in

**Status:** read from `gbr.exe` for the host audit's twelfth round; the engine models the
grid, its cell lists and the walk (`engine/core/src/sim/spatial_index.cpp`,
`sim/world.cpp`'s sweeps). Where a section's model is only partly the engine's, the section
says so.
**Not a file format.** Nothing here is on disk in the original; it is the runtime structure
every area search walks, and so the order every one of them answers in. The engine saves the
one part of it that is history (`save.md`, world section version 36).

Prose only, with addresses so each claim can be checked; [`../legal.md`](../legal.md) rule 2.

## The grid

The world (`[0x009a721c]`) keeps an array of cells at `+0x24`, 128 to a row, each cell
`0x1c` bytes. The cell of a point is found by 0x0053d030:

* the point `(x, y)` is in no cell when `x == -1` (only `x` is tested);
* otherwise the column is `x / 256` and the row `y / 256`, both divisions truncating toward
  zero, and the cell is `row * 128 + column`.

So a cell is 256 by 256 world units and the grid covers 32,768 units a side, the largest map
there is. No bound is checked: a point past the grid would index past the array, and no
object stands there.

A cell holds a **doubly linked list** of the objects standing in it -- head at cell `+0x08`,
tail at `+0x0c` -- through two links on the object, next at `[obj+0x34]` and previous at
`[obj+0x38]`; the object keeps its cell at `[obj+0x30]`. The cell also keeps at `+0x10` the
union of its members' category flags (`[obj+0x2c]`), so a sweep can pass over a cell that
holds nothing of the kinds it wants: it is a shortcut and never changes an answer.

## Who links and unlinks

* **The position setter** 0x0053f000 (five callers, the movement and placement paths among
  them) writes the new point, then asks 0x0053d030 for its cell. Only when that cell differs
  from `[obj+0x30]` does it relink: out of the old cell's list (0x0053d630, an ordinary
  unlink that leaves the others in their order and recomputes the cell's flag union) and
  onto the **tail** of the new cell's list (0x0053ce50), then `[obj+0x30]` becomes the new
  cell. A move inside one cell does not touch the list.
* **A holder** sets its passenger's point to `(-1, -1)` (0x005d3e10, the write at 0x005d3ef9)
  through the same setter; that point is in no cell, so the object leaves its list and is
  linked nowhere until it is set down again, when it joins the tail of the cell it lands in.
* **Destruction** (0x0053d6a0) unlinks the object from its cell.
* **A load** (the object's serializer 0x00540540, reading) clears the three links and, unless
  the loaded point is `(-1, -1)`, links the object onto its cell's tail. So in the original a
  cell's list after a load is in the order its objects were loaded, which is the order of the
  file, not the order they had arrived in.

So **a cell's list is in the order its objects last came into it**. A map's own objects are
linked as they load, in the map's order.

## The walk

Every area search walks the grid the same way. The query sweeps (the class-player circle
0x004fd060 and rectangle 0x004fd2d0, sight 0x004fd9c0, the map-area rectangle 0x004fdc20 and
circle 0x004fde60), the nearest-by-grid search 0x0053e290, and the forty-odd sweeps that share
the cell-against-circle test 0x00416810 (0x004233b0, 0x0051b840, 0x005d5bf0's, the food
search 0x005db8c0 among them):

1. take the box of half-side `r` about the centre, clamped to the map's rectangle
   (`[world+0xb4..+0xc0]`);
2. walk its rows of cells **top to bottom**, and in each row its cells **left to right**;
3. pass over a cell whose flag union has none of the sweep's mask bits; ask 0x00416810 how the
   circle meets the cell: not at all (skip it), partly (test each member's
   `dx² + dy² <= r²`), or wholly (take every member without the distance test);
4. walk the cell's list **from its head**, testing the mask against each member's own flags
   and then the sweep's own predicate, which may stop the walk.

So an area answer is in **grid order**: by row, then by column, then by arrival in the cell.
A whole-map sweep (`ClassPlayerObjs` with no area, 0x004fe860 over the map's rectangle) is
the same walk over every cell.

## The engine's model

`SpatialIndex` (`sim/spatial_index.hpp`) is this grid: 256-unit cells, 128 a side, row-major,
each cell a list in arrival order. `World::set_position`, `put_in_holder`,
`remove_from_holder`, spawn, despawn and the template copy refile an object; refiling leaves
it where it is in its list when the cell has not changed, and otherwise moves it to the new
cell's tail. `gather` answers in the walk's order. `World`'s spatial queries take a
`SweepOrder`: `grid` answers in that order; `spawn` puts the same set back into ascending id
for the callers not yet read against a grid sweep.

**What the engine keeps that the original does not.** The original rebuilds every list in
load order, so a loaded game's cells are in file order and the game after a load is not the
game the save was taken from. The engine keeps the order across a save instead: each object
carries the stamp of its last arrival (`SpatialSlot::arrival`), the world section saves it
with the next stamp, and a load files each cell's objects in stamp order. A loaded game then
plays exactly as the saved one would have, which lockstep and the save tests need. The stamps
are not hashed; what the order changes, the hashed state shows.

**Edges the engine does not copy.** A point with `x == -1` and any other `y` is in no cell in
the original; here only a held object is out of the grid, and a free object at `x == -1` is
filed in column 0. Points outside the grid are filed in the border cell here and would run off
the array there. A shipped map puts nothing in either place.

## A holder's roster

A holder is out of the grid, and keeps its members another way: every `CVXHolder` has a deque
of handles at `[holder+0x30]`. The entry 0x00532160 (from the unit's 0x005d3e10) searches it
and appends the unit when it is not already there; the exit 0x00531a80 erases the unit where it
stands, so the others close up in their order. `GetUnitsOnBoard` (0x005c89f0) and
`GetUnitsInSameHolder` (0x005df620) copy that deque front to back, and `UnboardAllUnits`
(0x005c84b0) drops from its front. So a holder lists its members **in the order they came in**.

The engine keeps no list on the holder; the member's back-link (`ObjectState::holder`) is the
membership and the same arrival stamp is the order: `put_in_holder` stamps a unit that joins a
holder it was not in, and `World::contents_of` answers the members in stamp order. The stamp is
saved, so a loaded holder keeps its order too. A settlement's holder has its own roster
(`Holder::units`, in garrison order), which those entry points read first.

## What is still unknown

* Whether any position write bypasses the setter 0x0053f000 (a direct write to
  `[obj+0x24]`), which would leave an object in a stale cell. None was found among the
  setter's callers' paths; the whole binary was not searched.
* The original's own load order (which decides its post-load cell order) was not read; the
  engine does not reproduce it, by design (above).
