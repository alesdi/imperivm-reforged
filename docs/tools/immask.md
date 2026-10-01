# `immask` — GRID and passability mask tool

**Format specification:** [`../formats/pass.md`](../formats/pass.md)
**Implementation:** [`src/imperivm/cli/immask.py`](../../src/imperivm/cli/immask.py)

Finds, describes and exports every HMMSYS grid in the install as an 8-bit `GRAY` PNG. That
is 597 per-entity passability masks plus `DATA\RANDOM_MAP.TERRAIN.GRID`.

`.pass` is not a format. The magic is `DIRG` — the FourCC `GRID` stored little-endian — and
it is a small general-purpose container addressed in **world units**: `cell_size`,
`bits_per_cell`, `extent_x`, `extent_y`, then row-major cell data. A `.pass` file is the
configuration `16, 1, 2048, 2048`, giving 128 x 128 bits in 2,068 bytes. The random map
terrain grid is the same container at `64, 8, 16384, 16384`. This tool reads both, and any
other configuration the arithmetic supports.

## Usage

```
immask [--game DIR] list                              [--pack P]... [--all-packs]
immask [--game DIR] info       <name> [--ascii]       [--pack P]... [--all-packs]
immask [--game DIR] export     <name> [-o PATH] [--normalise] [--scale N] [--pack P]...
immask [--game DIR] export-all  -o DIR [--normalise] [-v]     [--pack P]... [--all-packs]
```

`--game` is the installation directory containing `Packs/`, defaulting to `$IMPERIVM_ROOT`
then the current directory.

By default every command scans `Buildings.pak`, `MapObjects.pak`, `Units.pak` and
`data.pak`, which is where all 598 grids live. `--pack` restricts the scan; `--all-packs`
widens it to every pack that parses, at the cost of reading a few hundred megabytes.

`<name>` accepts the full member name, a tail of it, or an unambiguous stem: `bhouse01`,
`BUILDINGS\BWALLS\GATEE\GATEE.PASS`, `random_map.terrain`. Ambiguity is an error listing the
candidates — `gatee` matches seven walls sets — and a near miss suggests alternatives.

### `list`

```
$ immask list
pack             member                                              grid bits     set  box
Buildings.pak    BUILDINGS\BHOUSE01\BHOUSE01.PASS                 128x128    1     161  15x17
Buildings.pak    BUILDINGS\BWALLS\GATEE\GATEE.PASS                128x128    1     210  15x35
MapObjects.pak   MAPOBJECTS\DECORS\TR1\A\PASS                     128x128    1       8  4x3
Units.pak        UNITS\TAXEMAN\TAXEMAN.PASS                       128x128    1       0  -
data.pak         DATA\RANDOM_MAP.TERRAIN.GRID                     256x256    8   65311  256x256

598 grids: 546 named *.PASS, 51 named plainly PASS, 1 other
```

`set` is the number of non-zero cells and `box` is their bounding box, so an all-clear mask
is immediately visible as `0` and `-`.

### `info`

```
$ immask info bhouse01 --ascii
BUILDINGS\BHOUSE01\BHOUSE01.PASS  (Buildings.pak, 2068 bytes)
  grid         128 x 128 cells of 16 world units
  storage      1 bit(s) per cell, stride 16 bytes
  extent       2048 x 2048 world units
  non-zero     161 of 16384 cells
  bounds       x 57..71, y 56..72  (15 x 17 cells, 240 x 272 world units)
  anchor       top-left of the box is (-112, -128) from the entity origin

  ......#####....
  .....#######...
  ....#########..
  ....##########.
  ...############
  ....###########
  .....##########
  .#....##.######
  ###....########
  ####...#######.
  ####...#######.
  ####...######..
  ############...
  ###########....
  ##########.....
  .########......
  ..####.........
```

The `anchor` line converts the bounding box to world offsets from the entity origin, taking
the anchor to be the grid centre. **Treat the vertical half of that as provisional** — the
specification measures it as grid centre plus a possible one-cell correction, and this tool
does not resolve the ambiguity, it only applies the documented formula.

`--ascii` prints the box with `#` for blocked. For a byte grid it prints the cell values in
base 36 instead.

### `export`

```
$ immask export bhouse01 -o house.png --scale 3
BUILDINGS\BHOUSE01\BHOUSE01.PASS: 128x128 -> house.png (161 cells set)
```

One-bit grids map **set (blocked) to white, clear to black**. `--scale` magnifies by nearest
neighbour, which is what makes a 128 x 128 footprint readable on screen; it is a viewing
aid and `export-all` never applies it.

Byte grids are written **unchanged**, because their cell values are data — terrain indices,
in the one shipped example — not brightness. `--normalise` stretches them to 0..255 so the
structure is visible; do not feed a normalised export back into anything.

### `export-all`

```
$ immask export-all -o /tmp/export
598 grids exported to /tmp/export
  597 one-bit passability masks (22 entirely clear)
  1 other grid(s)
598 mask entries in /tmp/export/manifest.json
```

Paths follow the pack tree in lowercase with forward slashes, with the `.pass` or `.grid`
suffix replaced by `.png`: `buildings/bhouse01/bhouse01.png`,
`mapobjects/decors/tr1/a/pass.png`, `data/random_map.terrain.png`. The tool **merges** into
an existing `manifest.json`, replacing only the `masks` section, so `imterrain` and `imfont`
can write the same directory in any order. Colliding export paths are reported on stderr;
none occur in the retail data.

## Gotchas

**Discover by content, not by name.** 546 masks are named `*.PASS`, but 51 decor objects in
`MapObjects.pak` store theirs in a file called plainly `PASS` with no extension
(`MAPOBJECTS\DECORS\TR1\A\PASS`). Globbing for `*.pass` silently loses 51 of 597 — and
silently is the problem, since nothing errors. Every command here sniffs the `DIRG` magic.

**One-bit rows are packed least-significant-bit first.** Cell `x` lives in bit `x % 8` of
byte `x / 8`. Getting this backwards does **not** produce obvious noise. It produces
plausible-looking blobs shot through with diagonal tearing, which is exactly the failure mode
that survives a code review and dies in the renderer. Export `BHOUSE01` and check it is a
coherent house.

**A set bit means blocked.** The polarity is not stated in the data. It is unambiguous
anyway: masks are overwhelmingly zero, set cells form one compact blob the size of the
object, and every unit mask is entirely clear.

**The axes are world axes, not the isometric projection.** Round objects stay round — trees
give 2x2, 4x4, 6x6 and 8x8 discs — and the median bounding-box aspect ratio over all
map-object masks is exactly 1.0. A 2:1 isometric reading would flatten every one of them.
The exported PNG is therefore a top-down stamp; do not un-project it.

**The grid is always 128 x 128, whatever the object.** A hut and a shipyard both get the full
canvas and leave most of it clear. Nothing in the file records the footprint dimensions; the
footprint *is* the set of blocked cells. This is why every export is the same size and why
`--scale` exists.

**Footprints have holes.** `BHOUSE01` has an interior courtyard and the gates have a
deliberate gap. A pathfinder that reduces a mask to its bounding rectangle gets the gates
wrong in the way players notice.

**Units contribute nothing.** All 18 masks in `Units.pak` are entirely clear except
`UNITS\TARCHER\WALLSW.PASS`, a stray wall mask no entity references. Unit-versus-unit
avoidance is dynamic and is not in this format.

## Measured results

Run against the retail install, Python 3.13 on macOS:

| | |
|---|---|
| grids found by `DIRG` magic | **598** |
| one-bit passability masks | **597** — 269 in `Buildings.pak`, 310 in `MapObjects.pak`, 18 in `Units.pak` |
| named `*.PASS` | 546 |
| named plainly `PASS`, no extension | 51, all in `MapObjects.pak` |
| other grids | 1 — `DATA\RANDOM_MAP.TERRAIN.GRID`, 256 x 256 at 8 bits |
| grids exported | **598** (0 failures, 0 path collisions) |
| entirely clear masks | 22 |
| `export-all` wall time | 1.6 s |
| total PNG bytes | 85,735 |

Every one of the 597 masks has the identical header `16, 1, 2048, 2048` and is exactly 2,068
bytes, and all 598 grids pass `Grid.validate()`.

**Visual checks.** `BUILDINGS\BHOUSE01\BHOUSE01.PASS` exports as a single coherent building
footprint, 161 cells in a 15 x 17 box with a visible interior courtyard, matching the worked
example in the specification cell for cell. `BUILDINGS\BWALLS\GATEE\GATEE.PASS` exports as
two wall stubs separated by a clear diagonal gap — the gap a gate should leave.
`MAPOBJECTS\DECORS\TR1\A\PASS`, one of the extension-less 51, is a 4 x 3 disc, i.e. a tree.
`DATA\RANDOM_MAP.TERRAIN.GRID` under `--normalise` is a coherent terrain layout: a field of
one value with rounded patches of rock and sand, not noise.

## Limitations

- **Read-only.** There is no PNG-to-`.pass` path.
- **The vertical anchor is unresolved.** `info` prints world offsets using `extent / 2` in
  both axes. The horizontal half is solid; the vertical half may be off by one cell. It needs
  entity placements from the `.bfhp` map container to settle, which is out of scope here.
- **Axis direction is inferred.** That columns run east and rows south comes from the
  `WALLN`/`WALLE` naming, not from anything in the data.
- **`RandomMap.pak` is skipped** by `--all-packs`. It is an LZIS-compressed stream wrapping a
  pack, and this tool does not decompress; use `impk` if a grid is ever found in there.
- **Only 1 and 8 bits per cell are decoded.** The container's field is general; the reader
  rejects other widths rather than guessing at a packing order.
- **No entity join.** The mask is not connected to the `.ent.xml` that names it through
  `pass_file`, so an unreferenced mask (there is at least one) looks like any other.
