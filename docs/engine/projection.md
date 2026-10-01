# World-to-screen projection

**Status:** the mapping is derived and validated by rendering shipped maps; the vertical
scale is exact to the extent an engine-hardcoded asset can make it, the elevation scale is
recovered from the executable (1) and the renderer draws it (`rendering.md`, "Elevation")
**Consumers:** the renderer in `engine/`; see the implementation summary at the end

Imperivm looks isometric and is not. The world is a square grid of unsigned coordinates
measured from the top-left corner ([`../formats/map.md`](../formats/map.md)), and the camera
is a fixed oblique view straight down the world's Y axis: **X passes through unchanged, Y is
squashed, nothing rotates**. A terrain cell is an axis-aligned rectangle on screen, not a
diamond. Every artefact in the install agrees on this, and it is the first thing to get right
because a diamond-tile assumption fails against the data immediately: round objects are round
in the passability masks, not 2:1 ellipses, and the terrain blend masks are rectangles.

Everything below is measured against the retail install. Where a number is inferred rather
than measured it says so.

## The mapping

```
screen_x = world_x                                   - view_x
screen_y = world_y * 46 / 64 - elevation * HEIGHT_Z  - view_y
```

as a matrix, with the elevation term separated because it was the last to be established:

```
| screen_x |   | 1      0     0        | | world_x   |
| screen_y | = | 0   46/64   -HEIGHT_Z | | world_y   |
                                         | elevation |
```

| Quantity | Value | Status |
|---|---|---|
| horizontal scale | **1 px per world unit, exactly** | measured, exact |
| vertical scale `k` | **181 / 256 = 0.70703** in the code; 46 / 64 = 0.71875 in the art | **recovered from `gbr.exe`**; see below |
| `HEIGHT_Z` | **1** — elevation is subtracted verbatim, with no scale at all | **recovered from `gbr.exe`** |
| rotation, shear | none | measured |
| zoom | the play view is 1:1; the zoom map has three levels at 1/4, 1/8, 1/16 | measured |

**Two of those were open and are now closed, from the executable rather than from the art.**
`gbr.exe` 0x005c11a0 forms a screen point from a world point as

```
screen_x = world_x
screen_y = world_y * 181 / 256 - terrain_height(world_x, world_y)
```

so `HEIGHT_Z` is exactly **1**, and the vertical scale the *simulation* uses is 181/256, not
46/64. The two are the same angle seen twice: 181/256 = 0.70703 is 1/√2 in eight-bit fixed
point — a tilt of 45.00° — and 46/64 = 0.71875 is `64 · cos 45° = 45.25` **rounded up to a whole
pixel** for the tile art. So the sentence below about the camera being "essentially at 45°" is
right, and the code is the unrounded half of it. A renderer must keep 46/64, because that is
what the tiles were drawn to; the simulation must keep 181/256, because that is what the
entrance and exit markers were placed against.

The inverse is `sim/projection.hpp`, and it is a **scan** rather than algebra: the height term
makes the mapping many-to-one, and 0x006243b0 brackets the answer in 32-unit steps and closes
with one linear interpolation. It is accurate to one screen unit and deliberately not better —
see that header for why solving the algebra would be the wrong function.

The tilt implied by `k` is `acos(0.71875)` = **44.06°** from vertical, which is to say the
camera is essentially at 45° and the tile height was rounded from `64 · cos 45° = 45.25` up to
46. That is worth knowing but it is not the derivation; the derivation is below.

There is **no camera or projection constant anywhere in the shipped data**. A grep of all
1,797 members of `data.pak` for `scale`, `zoom`, `tilt`, `angle`, `elevation`, `perspective`,
`camera`, `projection`, `isometric` and `grid` returns nothing relevant. The projection is
hard-coded in `gbr.exe`, and the four-byte IEEE patterns for 0.71875, 0.6953125 and 0.7071 do
not appear in the executable either, so it is integer arithmetic. The numbers have to be
recovered from assets.

## Where 46/64 comes from

`DATA\CONST.INI` names the terrain cell size twice, in comments that solve for it exactly:

```
SYMinDistSameBound = 48   ;3072/TERRAIN_TILE_SIZE - distance between two shipyards ...
MinLakeIslandArea  = 1024 ;2048*2048/(TERRAIN_TILE_SIZE*TERRAIN_TILE_SIZE)
```

`3072/48 = 64` and `4194304/1024 = 4096 = 64²`. **`TERRAIN_TILE_SIZE` is 64 world units**,
which is also the cell size of `Terrain.terrain.grid`, `Terrain.decor.grid` and
`Terrain.trans.grid`.

`Terrain.pak` holds 56 terrain blend masks, `TERRAIN\TRANSITIONS\{A,B,C,D}<4 bits>.BMP`.
**All 56 are exactly 64 × 46 pixels**, 8-bit greyscale. Each one covers exactly one terrain
cell — that is what a corner-pattern blend mask is — and the whole list of 56 paths sits in
`gbr.exe` as a literal static string table, so the set is compiled in, not discovered at load.

One cell is 64 world units and one cell is 64 × 46 pixels. Therefore the horizontal scale is
**1** and the vertical scale is **46/64 = 0.71875**, both exactly, and both from the same
artefact. The minimap copies of the same masks are 16 × 12, 8 × 6 and 4 × 3 at `ZOOM8`,
`ZOOM16` and `ZOOM32` — 64 × 46 at a quarter, an eighth and a sixteenth, with 46 rounded up
each time — which fixes the three zoom-map levels at the same time.

### Corroboration from the art, and its accuracy

Two independent art measurements, neither of which uses the transition masks:

**Diagonal wall pieces.** A diagonal fortification run steps `(220, 216)` world units from
tower to tower (see the lattice below), so any straight edge of a diagonal wall piece — its
base line on the ground, or any edge of its ground-plane shadow mask — must have screen slope
`k · 216 / 220`. Shadow layers are especially good witnesses because a shadow is a flat mask
on the ground plane: the light shifts and widens the strip, it cannot rotate it. Fitting the
top and bottom silhouette edges by least squares with outlier rejection, keeping only fits
with an RMS residual below 1.2 px over at least 60 columns, gives **19 usable edges across
seven wall sets** (the `M*` and `R*` sets share art and are counted once):

| statistic | value |
|---|---|
| n | 19 |
| mean implied `k` | 0.7398 |
| sd | 0.0244 |
| standard error | 0.0056 |
| range | 0.6983 … 0.7875 |

**Ground-plane circles.** A circle lying flat on the ground projects to an axis-aligned
ellipse whose height/width *is* `k`, with no reference to world coordinates at all. Scanning
all 829 renderable entities for silhouettes that are genuinely elliptical (IoU ≥ 0.90 against
their own second-moment ellipse) and genuinely axis-aligned (major axis within 3° of
horizontal) finds exactly six, all of them ground effect rings — `CEASEFIRE`, `CEASEFIRE2`,
`DISEASE`, `DISEASE2`, `FRENZY`, `FRENZY2` (three artworks at two sizes each):

| statistic | value |
|---|---|
| n | 6 |
| mean aspect | 0.6889 |
| sd | 0.0115 |
| standard error | 0.0047 |

The two populations sit 7% apart — 0.740 against 0.689 — which is the honest measure of how
accurately this art was drawn, and it is much larger than either population's internal
scatter. Pooling all 25 samples:

**pooled mean 0.7276, sd 0.0311, sem 0.0062, 95% CI [0.7154, 0.7398].** 46/64 = 0.71875 sits
1.4 standard errors below the pooled mean, inside the interval.

Swept against candidate values on the 1/64 grid, the pooled RMS deviation is:

| candidate `k` | vs wall edges | vs circles | pooled RMS |
|---|---:|---:|---:|
| 32/64 = 0.5 | 0.2410 | 0.1892 | 0.2296 |
| 40/64 = 0.625 | 0.1173 | 0.0647 | 0.1070 |
| 44/64 = 0.6875 | 0.0575 | **0.0106** | 0.0504 |
| 45/64 = 0.703125 | 0.0437 | 0.0177 | 0.0391 |
| **46/64 = 0.71875** | 0.0318 | 0.0316 | **0.0317** |
| 47/64 = 0.734375 | **0.0244** | 0.0467 | 0.0312 |
| 48/64 = 0.75 | 0.0259 | 0.0620 | 0.0378 |
| 52/64 = 0.8125 | 0.0765 | 0.1241 | 0.0902 |
| 64/64 = 1.0 | 0.2613 | 0.3113 | 0.2741 |

46/64 is at the pooled minimum, effectively tied with 47/64 and clearly better than 45/64 and
48/64. **The art alone establishes k = 0.72 ± 0.03 and cannot distinguish 46/64 from 47/64.**
What makes 46/64 exact rather than approximate is the transition mask: it is one cell, it is
compiled into the executable, and 46 is not a number anyone rounded.

### What the wall-join test can and cannot do

The obvious test — place a run of wall pieces and find the scale at which they stop joining —
is much weaker than it looks, and this is worth recording so nobody spends a day on it again.

Taking every pair of fortification objects (wall, tower or gate) within 160 world units of
each other across nine shipped maps — **945 adjacent pairs** — and asking whether the two
sprites' silhouettes still touch at a candidate `k`:

| `k` | pairs still joined |
|---|---:|
| 0.500 | 99.26% |
| 0.625 | 99.37% |
| 0.6875 | 99.26% |
| 0.71875 | 91.53% |
| 0.750 | 91.11% |
| 0.8125 | 91.11% |
| 1.000 | 93.54% |
| 1.500 | 87.62% |
| 2.000 | 70.58% |

The curve is flat: **at least 88% of adjacent pairs still touch anywhere from k = 0.5 to
k = 2.0.** Fortification art overlaps its neighbours generously by design — a tower sprite is
230 px tall where the lattice step is 78 px — so daylight never appears and the test has no
knee to find. It bounds nothing useful.

Rendering the same runs and looking at them does better, because what breaks is not a gap but
the alignment of the wall's end against the tower it butts into. Sweeping a diagonal wall jog
in `numantia` at 40, 44, 46, 48 and 52 sixty-fourths, the joins are visibly wrong at 40/64
(the wall drives through the tower) and at 52/64 (the wall floats off the tower's parapet),
and are all defensible in between. That is a bracket of **[0.6875, 0.75]** by eye, consistent
with everything above and with nothing outside it.

## The fortification lattice

Not part of the projection, but it is the measurement the wall evidence rests on and it is
useful in its own right. Over 1,093 wall, tower and gate objects in nine maps, the axis-aligned
displacements between same-class objects closer than 700 units are:

| axis | displacements observed | counts |
|---|---|---|
| pure X | 220, 440, 660 | 174, 80, 35 — and nothing else |
| pure Y | 216, 432, 648 | 203, 102, 49 — plus 19 stragglers out of 373 |

The editor snaps fortifications to a **220 × 216 world-unit lattice**. Towers sit on the full
lattice, wall pieces on the half step: a wall run reads tower, wall, tower, wall at 108 units
of Y, or 110 of X, and a diagonal run steps `(±220, ±216)` per tower with a wall piece at the
midpoint. On screen that is a 220 × 155 px step, a diagonal at 35.2°.

## Elevation

`Terrain.height.grid` is one unsigned byte per 32 × 32 world units, full 0..255 range, 0 at
sea level ([`../formats/map.md`](../formats/map.md)).

**The byte is in world units.** `DATA\SUBAI\CROW_MOVE.VS` clamps a flying object's `.z`
against the terrain:

```
minHeight = 80;
maxHeight = 120;
...
th = GetTerrainHeight( newPos );
newZ = .z;
if( th + minHeight > newZ )  newZ = th + minHeight;
if( th + maxHeight < newZ )  newZ = th + maxHeight;
```

A crow flies 80 to 120 units above the ground, in the same units the terrain height is
measured in and the same units `PlayAnim(slot, pos, z)` takes. So elevation is a world-space
quantity on the same scale as X and Y, not an opaque index. Note this object `.z` is a
completely different thing from the `<layer z>` draw-order value; do not conflate them.

**What one unit of elevation is worth in pixels is not recovered.** Three values are
defensible and the data does not choose between them:

| candidate | rationale |
|---|---|
| `1.0` | elevation shares the pixel scale of X, the simplest integer implementation |
| `46/64 = 0.71875` | the engine reuses one constant, i.e. `screen_y = (world_y - z) · 46/64` |
| `sqrt(1 - k²) = 0.6953` | the geometrically consistent value for a camera tilted `acos k` |

Nothing distinguishes them, because the terrain and the objects standing on it are displaced
by the same factor: get it wrong and hills are steeper or shallower, but nothing tears, and
there is no shipped artefact that pins a slope. It needs a screenshot of the original engine
on known terrain, or the disassembly.

One thing the data does settle: **the ground cannot be drawn as rigid displaced rectangles.**
Displacing each 64 × 46 cell bodily by its own height tears the ground into disjoint blocks
with the background showing through — verified by rendering it at both 1.0 and 46/64 over the
steepest terrain in `numantia`. The ground must be a mesh whose *corners* carry the
displacement, so a cell becomes a quadrilateral with its texture warped into it, degenerating
to the exact 64 × 46 rectangle only where the four corner heights agree. The height layer's
32-unit cells are half the terrain cell, which suggests the mesh is finer than the texture
cell — a terrain cell spanning 2 × 2 height cells. **It is per-vertex**, read off the original's
ground warp (0x006217f0): it draws 32-unit quads and reads the height, and the light, at each
quad's four corners.

## Sprites

An entity's art is a list of `<layer>` elements, each pointing at an image whose frames come
from a `.rle.mmp` table ([`../formats/ent-xml.md`](../formats/ent-xml.md),
[`../formats/rle.md`](../formats/rle.md)). The anchor convention is simple and there is no
scaling anywhere in it:

```
origin_x = world_x - view_x                                     the entity's origin, on screen
origin_y = world_y * 46 / 64 - elevation * HEIGHT_Z - view_y

blit_x   = origin_x + layer.offsetx + frame.left
blit_y   = origin_y + layer.offsety + frame.top
```

- `offsetx`/`offsety` are **screen pixels, entity-local**, and they are per layer. Every layer
  of one entity usually shares the same pair, because they are the offset of the shared frame
  canvas, not of the individual piece.
- `frame.left`/`frame.top` are the frame's own bounds inside that shared canvas
  ([`../formats/rle.md`](../formats/rle.md)); a frame stores only its non-empty box.
- Sprites are never scaled, rotated or flipped. Facing is a column of the frame table, not a
  transform.
- The `<image drawmode>` attribute is not authoritative about shadows: **275 of 2,430 layers
  (11.3%) disagree with the `image_class` in the frame table they point at.** Read the frame
  table. Shadow frames carry no colour — they decode to a coverage mask and must be drawn as
  translucent black; `DATA\CONST.INI` sets `XRayTranslucency = 25` for the related `xray` flag,
  but the shadow's own opacity is not recorded anywhere.

## Depth sorting

`DATA\ZBINS.XML` is the whole draw-order configuration, and it reads in full:

```xml
<zbins>
	<zbin startz="0"     sort="1"/>
	<zbin startz="750"   sort="0"/>
	<zbin startz="900"   sort="1"/>
	<zbin startz="1080"  sort="0"/>
	<zbin startz="10000" sort="1"/>
</zbins>
```

A layer's `z` selects the **last bin whose `startz` is ≤ z**. Bins are drawn in order. Inside
a bin, `sort="1"` means the layers are depth-sorted against each other and `sort="0"` means
they are not — they keep whatever order the engine emits them in.

Across all 889 entity documents (2,430 layers, 18 distinct `z` values):

| bin | `sort` | `z` values present | layers | share |
|---|---:|---|---:|---:|
| [0, 750) | 1 | 0, 20, 100, 300, 400, 500, 520, 550 | 191 | 7.86% |
| [750, 900) | 0 | 800 only | 667 | 27.45% |
| [900, 1080) | 1 | 900, 950, 1000, 1001, 1050 | 1,510 | 62.14% |
| [1080, 10000) | 0 | 1100, 1200, 1500, 2000 | 62 | 2.55% |
| [10000, ∞) | 1 | — | **0** | 0.00% |

So the layout is: ground decals and underlays, then **all shadows, unsorted**, then
**everything solid, sorted**, then **effects and projectiles, unsorted**, then a top bin that
no shipped entity uses. `z` is never negative and never exceeds 2000.

The correspondences the bins were designed around hold to the percent:

- **z = 800 is the shadow bin.** 663 of the 718 layers whose frame table says `SHADOW` are at
  z = 800 (92.3%), and 663 of the 667 layers at z = 800 are `SHADOW` sheets (99.4%). The four
  exceptions are authoring mistakes; the 55 shadows elsewhere are deliberate pairings — 13
  sentry units whose shadow sorts *with* the body at 1000, 13 reed props at 520 paired with
  trunks at 550, 12 at 0 paired with bodies at 100, 5 projectile shadows at 1200 under their
  arrows at 1500.
- **z = 1000 is the body.** 146 of the 153 non-shadow layers in `Units.pak` are exactly 1000
  (95.4%). The exceptions are the four ship reflections at 950 (`percent="50"`), a fish at 900,
  an eagle at 1050.
- **`z` also orders within a bin.** In 412 of 887 entities `z` decreases at some point as the
  layer index increases, so index order is not draw order; and pairs like ship hull 1000 over
  reflection 950, or arrow 1500 over its shadow 1200, sit inside a *single* bin each and can
  only be ordered by `z`.

### `sortoffsetx` / `sortoffsety`

These move a layer's **sort position**, not its pixels. They are present on 2,264 of 2,430
layers and non-zero on 790 (x) and 1,030 (y), spread over 349 of 887 entities. The values are
continuous, not an enum: x ranges −310..367, y −243..266, with ~270 and ~280 distinct values
and no meaningful mode.

They are screen-pixel offsets in the same entity-local frame as `offsetx`/`offsety`, and they
mark **that layer's own ground contact point**:

| test | correlation | median difference |
|---|---:|---|
| `sortoffsetx` against the layer's content centre x | 0.888 | +1.0 px |
| `sortoffsety` against the layer's content bottom | 0.76 | −22 px |
| `sortoffsety` against the layer's content centre y | 0.78 | +51 px |

X lands on the piece's horizontal centre almost exactly. Y lands between the piece's centre
and its lowest pixel — typically 15 to 25 px above the bottom row, which is where a piece meets
the ground rather than where its front-most pixel is. Within an entity, the ordering of
`sortoffsety` across the z ≥ 900 layers agrees with the ordering of the layers' sprite bottoms
in 1,753 of 2,310 pairs (75.9%).

The clearest case is `MAPOBJECTS\STONEHENGE\STONEHENGE.ENT.XML`: 22 standing stones on 22
layers, all sharing `offsetx/offsety = -300/-300` and z = 1000, with sort offsets tracing the
ring — median `sortoffsetx − content centre x` = 0.0 (98% within ±20 px), median
`sortoffsety − content bottom` = −12.5 (83% within ±30 px). `MAPOBJECTS\BRIDGE01` is the same
idea pushed further: two halves whose pixels overlap heavily, given sort anchors 430 px apart
along the depth axis so the near half sorts in front of anything on the near bank and the far
half behind. A multi-piece building therefore behaves like several independent sprites, each
planted at its own footprint, which is how a unit can walk between a gate's two leaves.

Where they never occur is as informative: **no unit carries a non-zero `sortoffsetx`** (0 of
294 layers in `Units.pak`), and **no shadow-class layer carries one** (0 of 718). Units are
sorted by their position, full stop.

**The sort key.** Bin first, then within a sorted bin:

```
key = (screen_y + layer.sortoffsety, screen_x + layer.sortoffsetx)
```

Depth increases down the screen, so the greater `screen_y` draws later. On flat ground
`screen_y = world_y · 46/64` and sorting by world Y is equivalent, so the shipped data cannot
distinguish the two; screen space is the consistent choice because the offsets being added are
screen pixels. The X term is a tie-break only. This is what the acceptance render uses and the
sort holds everywhere in it: units in front of walls, wall pieces in front of each other along
a run, roofs over the walls they sit on, shadows beneath everything solid.

## Terrain assembly

The ground is **textured, not tiled**. There is no atlas of small tiles anywhere in the
install.

1. `Terrain.terrain.grid` gives one byte per 64 × 64 world-unit cell; the value is the `z` of a
   `<layer>` in `DATA\TERRAINS.XML`, which defines 41 layers, z = 0..40. Each names a `.vq`
   texture, a minimap bitmap and a `type` group (0 invalid, 1 grass, 2 ground, 3 sand, 4 water,
   5 rock, 6 road, 7 waves). `%season%` in the path is substituted from `game.xml`'s `season`.
   Layers 12 and 13 are hardcoded in the engine as `SHALLOW_WATER_IDX` and `DEEP_WATER_IDX`,
   which the file says in a comment of its own.
2. The cell occupies the screen rectangle `(cx · 64 - view_x, cy · 46 - view_y, 64, 46)`.
3. Fill it from the layer's texture, sampled with wraparound at
   `(u, v) = (cx · 64 mod W, cy · 46 mod H)` — i.e. continuously in screen space, so the
   texture runs across cell boundaries. The 55 `.vq` textures are single large tileable fields
   from 128 × 96 to 1024 × 800; their sizes are not multiples of the cell and their aspect
   ratios range 0.586 to 1.000, so they are wallpaper, not pre-squashed tiles. Wrap-seam mean
   absolute difference over interior-adjacent difference is 1.00 to 1.55 for every ground
   texture, confirming they were authored to tile. `waves.vq` (6.4×) and `invalid.vq` are the
   two that do not tile and are not fills.
4. Water animates: `dwater.vq` and `swater.vq` are 440 × 4800, which is **15 stacked frames of
   440 × 320**, matching `frames="15"` in the XML. Frame boundaries show as row discontinuities
   at every multiple of 320.
5. Modulate by `Terrain.light.grid`, one byte per 32 units, values 0..21 around a neutral of
   16. **Read off `gbr.exe`** (0x006217f0, the ground warp, and 0x0061e210, its lookup): the
   level is interpolated per pixel across each 32-unit quad from its four corners, bilinear in
   8.8 fixed point, and the integer level scales each channel by `c + trunc(c · (L − 16) / 20)`
   clamped, a gain of `(L + 4) / 20`.

### The transition masks

**Read off `gbr.exe`'s ground compositor** (0x0061f8a0; the masks are loaded into a
32-style × 16-code table by 0x0061ed80). What was below this heading was a reconstruction that
rendered plausibly and was wrong in three ways; the original does this:

**The terrain byte is a vertex, not a cell.** The tile drawn over the world square
`[64·cx, 64·cx + 64) × [64·cy, 64·cy + 64)` blends the four terrain bytes at its corners:
`T(cx, cy)` top-left, `T(cx+1, cy)` top-right, `T(cx+1, cy+1)` bottom-right, `T(cx, cy+1)`
bottom-left, the far column and row clamped. So cell *n*'s type is centred on world `64n`, and
its window is `64n − 32 … 64n + 31` -- which is the half-cell bias `IsPointInWater` applies
before it divides ([`../formats/pass.md`](../formats/pass.md)), found independently. The
reconstruction drew each type over its own square and so sat half a cell down and right, with
an edge where the original has a vertex.

**Which layer is drawn whole, and in what order.** The base is the lowest layer number present
at any of the four corners -- except shallow water (12), which is skipped unless all four
corners are water (12 or 13). Every other present layer is then composited over it in three
passes: all but the two waters in ascending order, then 12, then 13. Priority is the layer
number, as the reconstruction had guessed, with water lifted over every land layer; there is
no table.

**Which mask.** For an overlay layer *L*:

```
code  = the corners whose vertex is L        bit 3 TL, bit 2 TR, bit 1 BR, bit 0 BL
file  = TERRAIN\TRANSITIONS\<style><code XOR 0xF>.BMP
style = 'A' + layer[L].transition + ((y / 64) & 1)
alpha = the file's grey, as it stands (the original takes it >> 3 and blends in 5 bits)
```

The four characters of a name are `TL TR BR BL` and a set bit is black at that corner, as
measured before (`A1000`: TL 5.1, TR 252.1, BL 255.0, BR 255.0) -- so the file naming the
corners the overlay does *not* hold is white exactly where it does, and is its opacity with no
inversion. The reconstruction used `255 − mask[code]`; the masks are not complements of each
other (mean absolute difference 11.8/255 over the fourteen C pairs, 16.5 for A), so that was
a different picture, not the same one reached another way.

**Every layer's `transition` is 2.** The loader builds the whole layer array from one
template (0x0061f6d0: `transition` 2, `transition_terrain` −1, `animate_transition` 0) and the
XML attribute only overrides it; the two waters' `transition="2"` repeats the default. So the
original draws every boundary in the whole game through **C on even tile rows and D on odd
ones** -- the soft ramps -- and D being C's mirror is the variation between rows. A and B are
loaded but drawn only through two further styles the loader generates from them at start-up
(0x0061e760: A with C into style 4, B with D into 5, fourteen frames each), which is the
shore animation `animate_transition="1"` asks for on shallow water. The minimap shipping only
its `c` and `d` rows is the same fact seen from the other side.

- 4 styles × 14 patterns = **56 masks**. `0000` and `1111` are absent and never asked for: a
  layer holding all four corners is the base.
- **B is the pixel-exact horizontal mirror of A** under the corner relabel TL↔TR, BL↔BR (MAD
  0.00 for 13 of 14 patterns), and D is the mirror of C the same way (0.00 for all 14). A and B
  are hard, ragged, organic edges (51% of pixels fully 0 or 255); C and D are soft gaussian
  ramps (55% mid-tone).
- The minimap only ever ships the **C and D** rows (`gbr.exe` formats
  `minimap/zoom%d/terrain/transitions/c%c%c%c%c.bmp` and the `d` variant, and no others), at
  16 × 12, 8 × 6 and 4 × 3.
- `Terrain.trans.grid` is **zero in every cell of every map** -- a full census, 2,441,216 cells
  across all 29 maps in 23 containers, with an empty non-zero histogram. The compositor never
  reads it; the renderer holds it as `transitions` beside `height`, `light` and `terrain`
  (0x0061e5f2) and that is all.

## The mask anchor, checked against the engine's own output

This one is a collision question, not a rendering question — sprites are positioned by the
entity origin and the layer offsets, and never by the passability mask — but it is the only
place where engine-generated output can be diffed against a reconstruction, so it is the
strongest evidence in this document about *placement*.

`Terrain.pass.grid` is baked by the editor from the objects in `map.obj.xml` plus the terrain
itself. Rebuilding it and diffing cell by cell tests the anchor rule directly, and false
positives and false negatives separate the failure modes: a wrong anchor moves the whole stamp
and produces both, a missing contributor produces only false negatives.

Method: stamp each object's `.pass` mask ([`../formats/pass.md`](../formats/pass.md)) at its
world position, add one stamp per non-zero cell of `Terrain.decor.grid`, add terrain types 6
and 26 (the two the terrain table marks `passable="0"`), add the map rim. Water is *not* a
contributor: terrain types 12 and 13 are blocked in only 9.9% and 4.5% of their cells, against
94.6% and 99.6% for the two rock types, so water passability is a separate domain — which is
what `passable_water="1"` on layer 13 says.

**Contribution of each source**, pooled over nine maps, 2,170,470 blocked cells:

| reconstruction | recall | false negatives | false positives |
|---|---:|---:|---:|
| object masks only | 16.42% | 83.58% | 5.49% |
| + decor layer | 28.28% | 71.72% | 8.05% |
| + rock terrain (types 6, 26) | 66.81% | 33.19% | 10.34% |
| + map rim | **70.54%** | **29.46%** | **10.34%** |

(false positives as a percentage of the blocked-cell count, so the two columns are comparable.)

**Per map**, best rule:

| map | objects | decor stamps | blocked cells | recall | FN | FP | IoU |
|---|---:|---:|---:|---:|---:|---:|---:|
| `Crossroads.BFHP` | 22 | 0 | 400,835 | 90.49% | 9.51% | 4.88% | 0.863 |
| `numantia` | 599 | 3,969 | 322,056 | 81.94% | 18.06% | 9.50% | 0.748 |
| `danube` | 154 | 1,805 | 249,025 | 82.06% | 17.94% | 7.28% | 0.765 |
| `alesia` | 405 | 827 | 147,670 | 69.24% | 30.76% | 12.02% | 0.618 |
| `britain` | 572 | 2,436 | 356,901 | 62.68% | 37.32% | 11.79% | 0.561 |
| `zama` | 198 | 703 | 137,536 | 60.45% | 39.55% | 9.11% | 0.554 |
| `balcans` | 759 | 6,761 | 357,179 | 54.47% | 45.53% | 15.93% | 0.470 |
| `egypt` | 280 | 1,043 | 101,360 | 49.48% | 50.52% | 15.95% | 0.427 |
| `islandwar` | 110 | 1,332 | 97,908 | 47.24% | 52.76% | 11.02% | 0.426 |

### What this settles about the anchor

[`pass.md`](../formats/pass.md) records the horizontal anchor as solid and the vertical anchor
as "grid centre plus a possible one-cell correction", unproven. Registering each isolated
object's mask against the shipped layer independently — sliding it ±12 cells and taking the
offset of best IoU, discarding objects within 640 units of another masked object or sitting on
impassable terrain — gives 93 clean registrations (IoU ≥ 0.75, |Δx| ≤ 3 and |Δy| ≤ 6 cells):

| axis | rule | mean error | RMS error | exact |
|---|---|---:|---:|---:|
| X | anchor = **grid centre** | +0.215 cells | **0.639** | **62.4%** |
| X | anchor = mask bounding-box centre | +0.075 | 0.922 | 21.5% |
| Y | anchor = grid centre | −0.828 | 2.508 | 9.7% |
| Y | anchor = grid centre − 1 cell | +0.172 | 2.374 | 35.5% |
| Y | anchor = **mask bounding-box centre** | +0.446 | **1.527** | 19.4% |

**Horizontally the anchor is the grid centre**, confirmed. **Vertically it is the mask's own
bounding-box centre**: the registered offset correlates with the mask's bbox-centre offset at
r = 0.80 with a regression slope of −1.37 (R² = 0.67), and the objects that register perfectly
land exactly on it: `ScullTree` and `DeadTree` have their bbox centre one cell below the grid
centre and register one cell up, `Boulder` sits one and a half cells below and registers two
cells up, all three at IoU 1.00. Switching the whole reconstruction from the grid-centre rule
to the bbox-centre rule improves recall *and* reduces false positives at the same time —
pooled 67.77% / 13.07% becomes
70.54% / 10.34%, and the precision of the object stamps alone rises from 70.23% to 77.84% —
which is the signature of a corrected anchor rather than a fudged one.

The scatter is real: per-object registrations spread ±1.5 cells (24 world units) around the
rule, so it is not exact. **Use the bounding-box centre in Y and the grid centre in X**, and
expect the reconstruction to be approximate.

### The residual, and what is in the layer that nothing accounts for

29% of blocked cells are still unexplained, and the shape of the residual says why. Diffed
visually, it is (a) a one-cell fringe around every stamp, which is sub-cell rounding — the
stamps' interior cells are 84% correct against 66% for their perimeter cells — and (b) **thin
one-cell-wide closed curves** that follow terrain-type boundaries: a coastline in `islandwar`,
a loop around a village in `balcans`. 42% to 57% of the residual per map lies on a
terrain-type boundary, but no particular pair of types dominates and only 1% of it lies near
water, so it is not a shoreline rule.

The likely explanation is that the editor has a passability brush and the designers used it.
The engine's own invisible blocker class exists (`Impassable`, a 24-cell mask with no art) and
is used **zero times** in all nine maps, so the barriers were painted, not placed.
`Terrain.pass.grid` is therefore **authored data that cannot be regenerated** from the object
list, which matters for anyone planning to rebuild maps: ship the layer.

One clean by-product: every map carries a fixed impassable rim — **2 cells (32 world units) on
the left and right and 4 cells (64 units) at the top, in all nine maps**, with a variable
bottom margin of 10 to 26 cells.

## Acceptance test

A renderer was written against this document and run over shipped maps: terrain cells textured
and corner-blended, decorations from the decor layer, every object from `map.obj.xml`, sprites
composited from their layers, everything sorted through the ZBINS bins.

- `alesia` at (8600, 7300) — the Gaulish camp: roundhouses, palisades, a smith, and four
  formations of infantry. Buildings sit on the ground, shadows fall beneath them, units in
  front of a building occlude it and units behind are occluded, and the thatched roofs of
  adjacent huts overlap in the right order.
- `numantia` at (10600, 1600) — a hill wall with diagonal jogs. Towers and diagonal wall pieces
  meet exactly: the wall's stone end lands on the tower's parapet with no gap and no visible
  overlap at every join in frame, which is the sensitive case and the reason to render it.
  The rock-to-grass boundary is a ragged organic edge rather than a staircase, which is the
  transition masks working.

Both were rendered at `k = 46/64` with `HEIGHT_Z = 0` and inspected. The known cosmetic
shortfalls were the light layer sampled per terrain cell rather than interpolated (visible as
soft rectangular patches), no elevation, and every entity drawn in its first pose. The
light and the transitions have since been read off the executable and are drawn as it draws
them; the "ragged organic edge" this render showed was the A masks the original does not use.

## What is still unknown

- **`HEIGHT_Z`, the pixels-per-elevation-unit factor.** Not determined by any shipped artefact.
  1.0, 46/64 and 0.6953 are all self-consistent. Needs the original engine's output or the
  disassembly.
- ~~How the ground mesh is built from the height layer.~~ Read (0x006217f0): the flat
  composite is warped in 32-unit quads whose corners carry the height and the light.
- ~~The light layer's gain.~~ Read (0x0061e210): `c + trunc(c · (L − 16) / 20)` per channel,
  the level interpolated per pixel; see "The ground", step 5.
- ~~Which transition style a cell uses.~~ Read (0x0061ffb6): `transition + row parity`, every
  layer's `transition` 2, so C and D alternating by row.
- ~~Terrain priority.~~ Read (0x0061f8a0): the layer number, the two waters drawn last.
- **The shore animation.** `animate_transition` and `transition_terrain="10"` on shallow water,
  and the two mask styles generated from A/C and B/D for them (0x0061e760), are read as far as
  the loader. How a frame is chosen and where the waves layer is drawn is not.
- **The ground warp's vertical scale.** The original steps a quad 32 · 181 / 256 = 22.6 rows
  (0x006218f0 adds `0x16a0` to a `y · 181` accumulator); this engine's 46/64 steps 23. See
  "The mapping" for why the art says 46/64.
- **Ordering inside an unsorted bin.** `sort="0"` says the bin is not depth-sorted; it does not
  say what order the engine emits. Entity order, object order and z within the bin are all
  candidates. It matters for overlapping shadows.
- **`sortoffsety` on shadow layers.** 35 shadow layers at z = 800 carry a non-zero value —
  wall shadows −20/−30, gate shadows −60 — in a bin that is not sorted. Either the value still
  participates in some ordering or it is inert.
- **The top z bin.** `startz=10000` is empty in the entire retail corpus. Reserved, or dead.
- **Sub-cell mask stamping.** The one-cell fringe in the passability reconstruction is a
  rounding rule this document does not pin down; neither `floor` nor `round` reproduces the
  shipped layer exactly.
- **The vertical mask anchor's residual scatter.** The bounding-box-centre rule leaves ±1.5
  cells of per-object spread. Something smaller is going on inside it.
- **Pose and facing selection.** The frame table's column selects a facing and its row an
  animation frame, but the mapping from a `dir.x`/`dir.y` vector to a column index is not
  established here; the acceptance render draws frame (0, 0) for everything.
- **The zoom map.** Three levels exist as art (1/4, 1/8, 1/16) and `DATA\CONST.INI` has
  `[zoommap] ToggleTreshold = 1200`, but how the view switches and whether the play view can
  zoom at all is not investigated.

## What the C++ renderer must consume

Implementable summary. Constants first:

```cpp
constexpr int TERRAIN_TILE_SIZE = 64;   // world units per terrain cell
constexpr int TILE_W            = 64;   // and its screen width, in pixels
constexpr int TILE_H            = 46;   // and its screen height: k = 46/64 exactly
constexpr float HEIGHT_Z        = 1.0f; // UNKNOWN - see above; make it one named constant
```

**1. Projection.** Integer, no rotation, no scaling:

```cpp
inline int screen_x(int world_x)             { return world_x - view_x; }
inline int screen_y(int world_y, int height) {
    return (world_y * TILE_H) / TILE_W - int(height * HEIGHT_Z) - view_y;
}
```

The inverse for picking is the same expression solved for `world_y`; note `world_y` recovers
exactly only for multiples of 64, so round.

**2. Ground.** For each visible terrain cell `(cx, cy)`:
its rectangle is `(cx*64 - view_x, cy*46 - view_y, 64, 46)`; its texture is the `.vq` named by
`DATA\TERRAINS.XML` for the layer index in `Terrain.terrain.grid`, with `%season%` substituted;
sample it with wraparound at `(cx*64 mod W, cy*46 mod H)`. Water layers 12 and 13 select one of
15 stacked 440 × 320 frames by the animation clock. The cell's four corners are the terrain
bytes of `(cx, cy)`, `(cx+1, cy)`, `(cx+1, cy+1)`, `(cx, cy+1)`: draw the lowest present layer
(shallow water only if every corner is water) whole, then every other present layer through
`TERRAIN\TRANSITIONS\<'A' + transition + (cy & 1)><corners NOT that layer>.BMP` as its
opacity -- land ascending, then 12, then 13. Modulate by `Terrain.light.grid` per pixel,
interpolated across each 32-unit quad, gain `(L + 4) / 20`. Do not read `Terrain.trans.grid`;
it is zero.
Once elevation is switched on the cell stops being a rectangle: displace its corners and warp
the texture into the resulting quad, or the ground tears.

**3. Sprite anchor.** For each object, project its world position to `(origin_x, origin_y)`,
then draw each layer's frame at `origin + (layer.offsetx + frame.left, layer.offsety +
frame.top)`. No scaling, no rotation. Take the draw mode from the `.rle.mmp` frame table's
`image_class`, never from the XML's `drawmode`.

**4. Draw order.** Give every layer of every visible object a bin: the last `<zbin>` whose
`startz` is ≤ the layer's `z`. Draw the bins in the order they are declared:

| # | range | depth-sorted? |
|---|---|---|
| 0 | z < 750 | yes |
| 1 | 750 ≤ z < 900 | no — emission order |
| 2 | 900 ≤ z < 1080 | yes |
| 3 | 1080 ≤ z < 10000 | no — emission order |
| 4 | z ≥ 10000 | yes (never used by shipped art) |

The sort key inside a sorted bin is

```cpp
key = { origin_y + layer.sortoffsety, origin_x + layer.sortoffsetx };   // ascending
```

so a layer with a larger key draws later, i.e. in front. Each layer of a multi-piece building
sorts independently — that is the point of the per-layer offsets, and it is what lets a unit
stand between the two halves of a bridge.

**5. Collision, not rendering.** Stamp a `.pass` mask with its **bounding-box centre in Y and
the grid centre in X** at the object's world position. Do not expect to regenerate
`Terrain.pass.grid` from the object list — it carries hand-painted content — so load the
shipped layer and stamp only what is built or destroyed during play.
