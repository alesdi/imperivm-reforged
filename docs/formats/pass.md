# HMMSYS grid and passability mask (`.pass`)

**Status:** decoded and validated; how a mask is laid on the ground is read off the
executable and proven against every shipped map layer (see "How a mask is stamped")
**Reference reader:** [`src/imperivm/formats/pass_mask.py`](../../src/imperivm/formats/pass_mask.py)

A `.pass` file records which patch of ground an entity occupies, so the pathfinder can
refuse to route units through it. One file per entity, stored alongside the entity's
`.ent.xml` and sprites, and named by the `pass_file` attribute on the `<entity>` element.

`.pass` is not a format of its own. It is one configuration of a small general-purpose
container whose magic is the FourCC `GRID`; `DATA\RANDOM_MAP.TERRAIN.GRID` in `data.pak`
is the same container carrying a byte per cell instead of a bit. Decoding the container
gets you both.

All integers are little-endian and unsigned.

## Layout

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00     4    magic, "DIRG"          the FourCC 'GRID' stored little-endian
0x04     4    cell_size      u32     world units covered by one cell
0x08     4    bits_per_cell  u32
0x0C     4    extent_x       u32     world units spanned, horizontally
0x10     4    extent_y       u32     world units spanned, vertically
0x14     -    cell data, row-major
```

Everything else follows from the header:

```
width   = extent_x / cell_size
height  = extent_y / cell_size
stride  = width * bits_per_cell / 8      bytes per stored row
filesize = 20 + stride * height
```

Rows are stored top to bottom with no padding beyond `stride`. There is no compression and
no trailer.

### One-bit cells

For `bits_per_cell == 1` the bits within a byte run **least significant first**: cell `x`
of a row lives in bit `x % 8` of byte `x / 8`. This is the one thing in the format that is
easy to get backwards, and getting it backwards produces masks that still look
*plausible* — coherent-ish blobs shot through with diagonal tearing — rather than obvious
noise. Render a building and check it looks like a building.

A set bit means the cell is **blocked**. Clear means free. The polarity is not stated
anywhere in the data, but it is unambiguous: masks are overwhelmingly zero, the set cells
form a compact blob the size of the object, and units (below) are all zero.

## The two configurations in the retail install

| | `.pass` | `DATA\RANDOM_MAP.TERRAIN.GRID` |
|---|---|---|
| `cell_size` | 16 | 64 |
| `bits_per_cell` | 1 | 8 |
| `extent_x`, `extent_y` | 2048, 2048 | 16384, 16384 |
| grid | 128 x 128 | 256 x 256 |
| file size | 2,068 | 65,556 |

**Every one of the 597 passability grids in the retail install is byte-for-byte identical
in its header** — `16, 1, 2048, 2048` — and therefore exactly 2,068 bytes. The size does
not vary with the object; a hut and a shipyard both get the full 128 x 128 canvas and
simply leave most of it clear. Nothing in the file records the object's footprint
dimensions; the footprint *is* the set of blocked cells.

Note that 597 is more than the 546 files whose name ends in `.pass`: 51 decor objects in
`MapObjects.pak` store theirs in a file called plainly `PASS`, with no extension
(`MAPOBJECTS\DECORS\TR1\A\PASS`). Match on content, not on the name.

The terrain grid's cell values are small integers that line up with the `z` attributes of
the `<layer>` elements in `DATA\TERRAINS.XML` — it is a terrain-type-per-cell map for the
random map generator, with `3` (grass) and `15` covering most of it.

## Anchor

Cell indices are not world coordinates; the grid is a stamp that gets placed relative to
the entity's origin. The anchor is the **centre of the grid**: the corner shared by cells
(63, 64) and (64, 65) — equivalently, world offset `(extent/2, extent/2)` = `(1024, 1024)`
measured from the top-left corner of cell (0, 0). So

```
world_x_of_cell_left_edge(x) = x * cell_size - extent_x / 2
world_y_of_cell_top_edge(y)  = y * cell_size - extent_y / 2
```

The horizontal half of this is solid. Taking every mask that is mirror-symmetric about
some vertical axis (135 of the 597), 121 of them mirror about exactly x = 63.5, i.e. the
cell boundary at 64 — the geometric centre of a 128-wide grid. The remaining 14 sit within
one cell of it.

The vertical half is weaker. The 82 masks that are mirror-symmetric about a horizontal
axis spread across y = 63.5 to y = 66, with the mode at 64.5 rather than 63.5. That is
consistent with the anchor being the grid centre and objects simply sitting slightly
forward of it — an entity is anchored at its ground contact point, not at the centroid of
its footprint, so there is no reason to expect vertical symmetry — but it is also
consistent with a genuine one-cell offset in the vertical anchor. Perfectly round decor
(`ITEMHOLDERS\DEAD_TREE.PASS`, a 6 x 6 disc) lands on x 61..66 and y 62..67, symmetric in x
about 63.5 and in y about 64.5, which is the cleanest statement of the discrepancy.

Attempting to recover the anchor from 180-degree rotation pairs (`WALLN` against `WALLS`
and so on) does not settle it: the pairs only reach an IoU of 0.6 to 0.8 because they are
separately drawn art, and the wall pieces are elongated, so the recovered centre is poorly
constrained along the long axis.

**That discrepancy is the vertical flip, and it is settled** -- see "How a mask is
stamped" below. The stamp maps row `r` of the file to world row `64 - r` from the anchor,
mirrored, so a mask symmetric about file row 64.5 is symmetric about the anchor's own row
once stamped. Both halves of the anchor are the grid centre; the file is simply stored
upside down relative to the world.

## Axes

The grid is a square, axis-aligned, top-down world grid — **not** the isometric screen
projection. Two independent checks:

- Round objects stay round. Trees and tree stumps produce 2 x 2, 4 x 4, 6 x 6 and 8 x 8
  discs, and the median bounding-box aspect ratio over all 305 map-object masks is exactly
  1.0. A 2:1 isometric projection would flatten every one of them to 2:1.
- The eight wall pieces in `BUILDINGS\BWALLS\` behave the way compass-named pieces should
  on a world grid: `WALLE` and `WALLW` are near-vertical bars, `WALLN` and `WALLS` are
  horizontal blocks, and the four diagonal names are parallelograms leaning the
  corresponding way, with `NE`/`NW` and `SE`/`SW` mirroring each other about the vertical.

That fixes the axes but not their sign; the mapping of "columns increase eastward, rows
increase southward" is inferred from those names and is not proven.

## How a mask is stamped

The engine does not lay a mask on the ground as a square. `gbr.exe` 0x00546a80 takes
every set cell of the mask and forms a **screen** point from it:

```
screen_x = anchor_x + 16 * col - 1016
screen_y = (anchor_y * 181 / 256 - height(anchor)) + (1032 - 16 * row) * 181 / 256
```

then asks the camera's inverse projection (0x006243b0, `docs/engine/projection.md`) for
the world point under that screen point, terrain height and all, and sets the 16-unit
passability cell there. The anchor is the object's position; `height()` is the bilinear
sample of the height layer, zero outside the map. Three things follow, and every one of
them is visible in the shipped layers:

- **The rows are mirrored.** The row term is `1032 - 16 * row`: file row 64 lands on the
  anchor's row and row 63 lands one cell *below* it in world `y`, so a mask is stamped
  upside down relative to its storage order. This is the whole of the "vertical anchor"
  discrepancy measured above.
- **The columns are not.** `16 * col - 1016` puts column 64 eight units right of the
  anchor, which is the centre of cell 64 measured from the grid's centre line.
- **On a slope the footprint stretches.** Where the ground rises, the inverse projection
  turns one screen row into a farther world row, so consecutive rows of the mask land more
  than a cell apart; the stamp fills the column between each stamped cell and the one
  before it (0x00546cc0) so the footprint stays solid. The screen point is first clamped
  to the map's own screen rectangle (`x` to `map.x1 - 32`, `y` to `project(map.y1) - 287`).

The proof is the corpus. `immap passability` (`engine/tools/immap.cpp`) rebuilds a map's
`Terrain.pass.grid` from nothing -- the terrain rules below, every object's mask through
this stamp, every decoration's, and the frame -- and compares it with the stored layer.
**All 23 maps a player can open rebuild bit for bit** (`tests/test_corpus_passability.py`);
a mask stamped the other way up, or laid flat, disagrees on every building on every map.
The C++ lives in `core/world/editor.hpp` under "passability", the executable's addresses
beside each rule.

## The map's passability layer, and how the editor keeps it

`Maps/<n>/Terrain.pass.grid` is one bit per 16-unit cell, set is blocked, and it is
**never rebuilt at save time**: the original's editor keeps it incrementally, one rebuild
over a rectangle after every stroke, placement or removal (0x00547700), and writes the
live grid. A rebuild clears the rectangle, bakes the terrain's own bits back, re-stamps
every object standing within 1024 units of it (into the rectangle only) and every
decoration whose cell lies within 1024 units (against the whole map), then re-imposes the
frame. The terrain's rules (0x00547090), per 16-unit cell whose corner is `(x, y)`:

- the layer is the one under `(x + 32, y + 32)` -- the same half-cell bias `IsPointInWater`
  uses -- and the loop stops 32 short of the map's far edge, so the last two columns and
  rows are the frame's alone;
- `passable="0"` (the two rock layers) blocks;
- `passable_water="1"` (deep water, 13) blocks the **rim** of its 64-unit cell: the four
  pass cells along an edge whenever the terrain cell across that edge is not deep water,
  the corner cell also asking the diagonal one (0x00545100);
- any other layer blocks where one of its eight neighbouring 16-unit cells samples deep
  water -- so a deep lake carries a one-cell shore on both sides of its edge.

The frame (0x00544bd0): the first two and the last two columns on every row; per column,
a top band from row 0 to 48 units past the world row the map's top screen edge unprojects
to, and a bottom band from 32 units above the world row that `project(map.y1) - 287`
unprojects to (28 rows on a flat 16,384-unit map, lower where the ground at the edge
rises). The 287 is 255 + 32: the tallest ground plus one cell. This is why every shipped
layer is blocked along its edges and across a wide strip at the bottom.

Two consequences of the scheme being incremental. A removal rebuilds the mask's *flat*
extent (0x005fe080: the set cells' bounding box, mirrored like the stamp), so a cell the
slope or the one-unit rounding of the inverse projection carried outside that box stays
set -- the original leaves the same bit, and the 23 shipped layers show none, which says
how rarely it happens. The height tools rebuild nothing at all (0x0049d970 commits the
height and the light and stops), and the water leaf's levelling rebuilds only its own
square, although a change of height moves where every footprint within reach lands; a
painted hill therefore leaves the footprints on it where they were until a later rebuild
touches them. And at match start, outside the editor, the game replaces the
`Mutable` settlements with their templates and rebuilds the **whole** layer once
(0x00552e95), so the grid a match is played on is not the shipped layer but that rebuild
over the world as it stands -- which the simulation here does too (`GameSession::
start_match`, `SessionInputs::masks`), and carries in its saves. The editor's own map-open
path rebuilds the whole layer as well (0x00494cca), which is why the shipped layers are so
consistent with a rebuild from nothing: every save follows a load that rebuilt it.

## Worked example

`BUILDINGS\BHOUSE01\BHOUSE01.PASS`, referenced from `BHOUSE01.ENT.XML` as
`pass_file="BHouse01.pass"`. Header:

```
44 49 52 47  10 00 00 00  01 00 00 00  00 08 00 00  00 08 00 00
"DIRG"       cell 16      1 bit/cell   extent_x 2048 extent_y 2048
```

128 x 128 cells, stride 16, so 20 + 16 * 128 = 2,068 bytes. 161 cells are set, in the box
x 57..71, y 56..72 — 15 by 17 cells, or 240 by 272 world units. Printed with `#` for
blocked and `+` marking cell (64, 64):

```
......#####....
.....#######...
....#########..
....##########.
...############
....###########
.....##########
.#....##.######
###....+#######
####...#######.
####...#######.
####...######..
############...
###########....
##########.....
.########......
..####.........
```

A single coherent building footprint with a small interior courtyard. The gate pieces are
the other useful sanity check: `BUILDINGS\BWALLS\GATEE\GATEE.PASS` renders as two wall
stubs with a clear diagonal gap between them, which is exactly what a gate should block.

## Validation

The reader asserts all of these, and they hold for all 597 grids plus the terrain grid:

- Magic is `DIRG`.
- `extent_x` and `extent_y` are whole multiples of `cell_size`.
- For one-bit grids, `width` is a whole number of bytes.
- `20 + stride * height` equals the file size exactly.

## Writing

A writer emits the 20-byte header from the grid's four fields — `cell_size`,
`bits_per_cell` and the two extents **in world units**, not the derived width and height —
followed by the rows top to bottom, each exactly `stride` bytes, with no padding, no
alignment between rows and no trailer. Sub-byte cells are packed least significant first,
the same way they are read: cell `x` of a one-bit row is bit `x % 8` of byte `x / 8`, and
the even cell of a four-bit row is the low nibble. Sixteen-bit cells are little-endian.
Nothing else is in the file, so nothing else has to be invented.

The claim a writer can make is therefore the strongest one: **what was read is what is
written.** `pass_mask.write(pass_mask.parse(data)) == data` for every one of the **772**
grids in the retail install — the 597 passability masks, `DATA\RANDOM_MAP.TERRAIN.GRID`,
and the six terrain layers of each of the 29 map directories inside the `.bfhp`
containers, at all four depths the data ships (1, 4, 8, 16). The C++ port
(`core::write_grid`, `engine/core/include/imperivm/core/formats/grid.hpp`) is held to the
same 772 through `immap grid-roundtrip`, and to one thing more: a fresh grid of the same
geometry, painted cell by cell through `Grid::set_cell` from the parsed values, is the
stored bytes too. That is the check that the setter's packing is the original editor's,
not merely the reader's inverse; a setter and a getter that were *both* most-significant-first
would round-trip each other and agree with nothing on disk. `tests/test_corpus_grid.py`
runs both; `tests/test_grid.py` and `engine/tests/test_grid.cpp` pin the bit positions
synthetically, against an independent packer.

**What the corpus establishes, and what it does not.** The one-bit order is proven by the
597 masks and the 29 passability layers — flip it and every one of them differs. The
byte and word depths carry no packing question. **The four-bit nibble order is not
established by the data.** Every four-bit byte in the retail install has two *equal*
nibbles: the 29 transition layers are entirely zero, and the terrain layer of the five
blank templates is `0x33` throughout (terrain type 3, `Grass 1`, in both halves), so a
writer that put the even cell in the *high* nibble round-trips all 34 of them just as
well. Injecting exactly that fault caught nothing in the corpus and only the synthetic
tests. Both implementations put the even cell in the low nibble, by analogy with the
one-bit layout, and that is an **assumption**; it will be settled by the first authored
map whose four-bit layer is not uniform, or by reading the editor.

Two things a writer must not do, because the reader would then misread the result:

- **Emit a value the depth cannot hold.** The terrain-type layer is 8 bits on the 24
  authored maps and 4 on the five blank templates in `Packs/`; a type of 16 written into a
  4-bit layer would come back as 0 (`Ground 1`), a different terrain rather than an
  approximation. Both implementations refuse it rather than clamp or mask.
- **Write a sub-byte row that is not a whole number of bytes.** The reader refuses such a
  header, so the factories that make a fresh grid (`Grid.from_cells`, `OwnedGrid::create`)
  refuse the same geometry.

## Consequences for the engine

- **Collision granularity is 16 world units.** That is four times finer than the 64-unit
  cell the random map generator uses for terrain type, so the static obstruction grid and
  the terrain grid are not the same resolution and cannot share an index.
- **Static obstruction is a bitmap, not a rectangle.** Footprints are irregular, include
  interior holes (the house above has a courtyard; gates have a deliberate gap), and are
  not derivable from any width/height pair in the entity XML. The pathfinder has to consume
  the stamp.
- **Units do not contribute to the static grid.** All 18 `.pass` files in `Units.pak` are
  either entirely clear or, in one case
  (`UNITS\TARCHER\WALLSW.PASS`), a stray wall mask that no entity references — the unit
  entities in that folder declare `pass_file=""`. The nine unit entities that do name a
  `pass_file` point at masks that are all zero, or at files that are not shipped at all.
  Unit-versus-unit avoidance therefore has to be dynamic, presumably from the `radius`
  property on the class, and is not part of this format.
- Five map-object masks are entirely clear as well
  (`PATCHES\PATCH09`, `TOBJ11`, `TOBJ15`, `DESERT BUSH 2`, `DECORS\NIVAG1`), meaning those
  decorations are walk-through.

## The map's terrain-type layer, and the one thing the simulation reads it for

`Maps/<n>/Terrain.terrain.grid` inside a `.bfhp` is the same container with `cell_size`
64 and `bits_per_cell` 8: one byte of terrain type per 64-unit cell, so a 16,384-unit map
carries a 256×256 layer in 65,556 bytes.

**Terrain type 13 is water.** `gbr.exe`'s `IsPointInWater` (`0x005c6d40`) is the whole of
the evidence and the whole of the simulation's interest in this layer: it bounds-checks
the point against the world rectangle, indexes the layer, and compares the byte against
13. Nothing else in the executable names the constant, and no other type value has a
meaning this project can attach to it.

Two details of that lookup are worth having here rather than only in the code:

- **The point is biased by half a cell before it divides** — `x + 32`, then `>> 6`. A
  point therefore lands in the cell it is *nearest to*, not the one it sits inside, and
  the window for cell *n* is `64n − 32 … 64n + 31` rather than `64n … 64n + 63`. On a
  boundary the two readings differ by a whole cell.
- **The layer is addressed in world coordinates directly**, with no centre anchor — which
  is the answer to the last question in the list below, at least for this configuration
  of the container. The `.pass` masks are anchored at their centre because they are
  stamps; a map layer is not.

## What is still unknown

- The nibble order of four-bit cells. Every four-bit byte the install ships has equal
  nibbles, so the low-nibble-first reading is an assumption by analogy with the one-bit
  layout, not a measurement (see "Writing").
- Which way the world axes point (which grid direction is north). The `WALLN`/`WALLE`
  naming implies columns run east and rows run south, but nothing in the data proves it.
- ~~Whether the engine rotates the stamp at placement time.~~ It does not: the stamp
  reads the mask as stored, mirrored top to bottom and nothing else, and the corpus
  agrees on every wall piece.
- Whether a single bit is the whole story semantically. There is only one bit plane, so
  there is no room for a separate "buildable" or "shoot-through" layer in this file; if the
  engine distinguishes those, the information lives elsewhere.
- Why the container carries `cell_size` and `extent` in world units rather than a plain
  width and height. The redundancy is harmless but suggests the grid is meant to be
  addressed in world coordinates directly, which would be worth matching in the
  reimplementation. **Answered**: the map's layers are addressed that way, with no centre
  anchor; a `.pass` stamp is addressed from its centre, through the projection, as "How a
  mask is stamped" describes.
- **The rest of the terrain type table.** Only 13 has a meaning this project can name,
  from the one function that tests it. The other values are the renderer's -- they select
  ground textures -- and nothing in the simulation reads them.
