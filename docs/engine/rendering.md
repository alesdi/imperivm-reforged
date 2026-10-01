# Rendering

**Status:** the engine renders shipped maps — ground, transitions, objects, shadows, depth
sort, camera. Flat ground, first pose, no animation, no fog, no UI.
**Reads:** [`projection.md`](projection.md) for the mapping and the depth rule,
[`../formats/map.md`](../formats/map.md) for what a map holds,
[`architecture.md`](architecture.md) for why the split below is where it is.

```
./build/engine/app/imperivm --game $GAME --map Scenarios/Crossroads.BFHP
./build/engine/app/imperivm --game $GAME --map Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp
```

Pan with the arrow keys (shift to go faster) or by dragging. `--at X,Y` opens the view on a
world position, `--map-index N` picks a map inside a multi-map container, `--screenshot FILE`
writes a PNG and `--frames N` quits after N frames, which together make the renderer testable
from a shell script. Setting `IMPERIVM_DEBUG_SHEETS` logs every sprite sheet as it is uploaded
with its storage class and palette size, which is the first thing to look at when a sprite
comes out the wrong colour.

Every shipped container renders. `Scenarios/Crossroads.BFHP` is a skirmish map and its 42
buildings are all `Mutable*` — the race-agnostic placeholder art, which really is a white sheet
draped over each shape until a race is chosen. That is the data, not a bug; open one of the
Great Battles for a scene with buildings in it.

## The split

| Concern | Where |
|---|---|
| Parse `map.xml`, `map.obj.xml`, the six `Terrain.*.grid` layers | `core/world/map.{hpp,cpp}` |
| Parse `DATA\TERRAINS.XML`, `DECORS.INI`, `player<i>.xml`, `game.xml` | same |
| The projection, and its inverse for picking | same |
| Which layers a ground tile blends, and through which mask (`terrain_tile`) | same |
| Open the `.bfhp`, gather the payloads | `platform/map_source.{hpp,cpp}` |
| Composite the ground, place the sprites, sort them, drive the GPU | `platform/map_renderer.{hpp,cpp}` |
| Wire the two together | `app/main.cpp` |

Core takes byte spans and returns state; it never learns where a byte came from. That is the
rule in [`architecture.md`](architecture.md), and the reason the projection is in core rather
than in the renderer is that **picking is its inverse and picking is simulation**: a click
resolves to a world position on the deterministic side of the boundary, so the mapping has to
live there. It is integer arithmetic for the same reason everything else in core is.

## The GPU stack

SDL3's GPU API, targeting Vulkan and Metal. Shaders are compiled **offline** and committed
(`engine/platform/shaders/pack_shaders.py` → `src/generated/sprite_shaders.inc`), so building
the engine needs a C++ compiler and SDL3 and nothing else; only someone editing a shader needs
`glslangValidator`. SPIR-V is generated, the MSL half is hand-written. There is no DXIL, so
D3D12 is not a backend yet — on Windows SDL picks Vulkan.

Everything is drawn into an offscreen `RenderTarget` and blitted to the swapchain. That costs
one full-screen blit a frame and buys the thing that makes a renderer testable at all: the
frame can be read back and *looked at*. Every screenshot in this document was produced that
way, and the acceptance check below is a pixel diff against an independent implementation.

## Sprites: one atlas of indices, one palette row per player

Every sprite in the game is 8-bit palette indexed, and team colour is a swap of palette slots
0..63. Flattening a sprite to RGBA bakes in one player's colours, so four players would mean
four copies of a 400 MB (2.8 GB expanded) sprite store. Instead the atlas is **R8 — the palette
index itself** — and the fragment shader does the lookup the original's software rasteriser did
with a table in L1. What varies per player is one kilobyte of palette.

- **Transparency** arrives through the same lookup. The format has no alpha channel; gaps in
  the run encoding are the only transparency. The atlas is cleared to an index the image does
  not use (the frame's own `color_key` where that is safe) and that entry is given alpha 0.
- **Shadows** are 1-bit coverage masks sharing the body's canvas. They take the identical path:
  covered pixels become index 1, and a two-entry palette makes 0 transparent and 1 black. The
  modulate alpha decides how dark the pass lands — 110/255 here, chosen to match the reference
  render because the value is not recorded anywhere in the data.
- **Team colour** keeps each slot's brightness and takes hue and saturation from the player's
  colour. The real per-player ramps are in the executable and have not been sourced; slots
  0..63 are not a sorted gradient, so laying a generated ramp over them would scramble the
  shading. The player colour itself *is* shipped data — `player<i>.xml`'s `color`, RGB555
  packed into a decimal integer — so the hue is right even though the ramp is a preview.
- **Only frame (0, 0) of each sheet is uploaded.** A map draws several hundred distinct sheets
  and holds every entity in its first pose; uploading every facing and every animation step of
  all of them costs tens of megabytes of atlas for pixels nothing samples. Numantia's 1,542
  objects and 3,971 decorations come to 338 sheets and 20 MiB of atlas across five pages.

Two changes to the sprite renderer were needed to serve a scene rather than a sheet.

**Draw order is queue order.** It previously grouped instances by atlas page to cut draw
calls. With one sheet on one page that is invisible; with a depth-sorted scene on five pages
and no depth buffer it reorders sprites that were sorted for a reason, and a tree jumps in
front of the building it stands behind. The loop still coalesces every *consecutive* run on one
page into a single draw, which is most of the win.

**The palette lookup texture is 4,096 rows**, not 256, and **a row is its contents**. A map
asks for one row per (sheet, player) pairing, and `upload_palette` answers a request for entries
a row already holds with that row, since rows are never rewritten: the animation sheets of one
unit type mostly share a palette, and the map's renderer and the live world's prepare many of
the same sheets. Before rows were shared, Balcans' sixteen seats filled 2,048 rows by turn 10;
every sheet prepared after that had no palette, so no art, and the units drawn from it were
neither drawn nor picked while buildings and props, prepared earlier, still were (playtest
report #3). Shared, the same match takes about 920 rows. A refusal is logged once, and a
screenshot's `drawn:` line counts rows used, shared and refused. Rows are staged and only reach the GPU at `commit_uploads`, so every row a scene can
ask for is claimed during load — a row created during a draw would be sampled a frame before
it existed, and the sprite would come out of the shader as whatever the texture happened to
hold. (It comes out white, which is a memorable way to learn this.)

## The ground is composited on the CPU

The ground is **textured, not tiled**: each terrain type is one large tileable field, 128 × 96
to 1024 × 800, sampled continuously in screen space so it runs across cell boundaries unbroken.
Neighbouring types are blended through 64 × 46 corner masks. So one cell is one to four texture
samples plus an alpha composite, and

1. it never changes — a cell's contents are a property of the map;
2. doing it on the GPU would mean binding an atlas of all 41 terrain textures *and* all 56
   masks at once, plus a second pipeline and a second pair of shaders.

So a finished 64 × 46 cell is cached, panning is a row copy, and the viewport reaches the GPU
as one RGBA texture and one blit. Sprites then load over it in the existing pipeline. The cache
is bounded at 4,000 cells — a 1500 × 1000 view holds about 600 — and dropped wholesale when it
overflows rather than tracked for recency.

**The ground is composed the way `gbr.exe` composes it** -- read off its software compositor,
0x0061f8a0, which blends one 64-unit cell into the 16-bit back buffer per call; the original
has no GPU path. `Terrain.trans.grid` is zero in every shipped cell and is not read. What is
read (`core::terrain_tile`):

- **The terrain byte is a vertex.** Cell *(cx, cy)*'s type belongs to the world point
  *(64·cx, 64·cy)*, and the tile drawn over the square to its right and below blends the four
  vertices at its corners -- *(cx, cy)*, *(cx+1, cy)*, *(cx+1, cy+1)*, *(cx, cy+1)*, the far ones
  clamped. It is the half-cell bias `IsPointInWater` and the passability bake already apply
  (`docs/formats/pass.md`): a point's terrain is its nearest vertex.
- **The base** is the lowest layer present at a corner, drawn whole -- shallow water (12)
  excepted, which is passed over unless all four corners are water, so a shore is land with the
  water laid over it. **The overlays** follow in three passes: every other present layer
  ascending, neither water; then shallow water; then deep water. So priority is the layer
  number, as the reference render had assumed, with the waters above every land layer.
- **The mask** of an overlay is `TERRAIN\TRANSITIONS\<style><code>.BMP` where `<code>` is the
  four corners the overlay does **not** hold (0x0061ffc1) and the file's grey is its opacity
  with no inversion -- a set bit is black, so the mask is white at the overlay's own corners.
  The old reading, `255 − mask[code]`, is a different image: the shipped masks are not
  complements of one another (mean difference 11.8/255 for C, 16.5 for A).
- **The style** is the layer's `transition` plus the tile row's parity (0x0061ffb6). Every
  layer is built from one template whose `transition` is 2 (0x0061f6d0) and the XML's only
  `transition="2"` repeats it, so every shipped overlay is **C on even rows and D on odd** --
  the soft ramps, with the mirror breaking up the repeat. A and B are never drawn from their
  files: they feed two animated styles the loader generates (0x0061e760, A with C and B with
  D, fourteen frames) for `animate_transition`, which is not reproduced. The minimap shipping
  only its C and D rows is the same fact from the other side.

The masks are 8-bit Windows BMPs, the only BMP the renderer must read, so `map_renderer.cpp`
carries a thirty-line decoder for that one variant rather than a general one.

### The zoom map's ground

The zoom map (Space) is the same compositor at a small scale, from its own art
(`core/world/zoom_ground.hpp`, drawn by `MapRenderer::compose_zoom`). Read off `gbr.exe`'s
builder, 0x00616f10:

- **The level** is the smallest `L` with `2^L` at least the map's width over 1024 -- 8, 16 or
  32 for the shipped sizes. The picture is composed at **twice** that scale (0x006186a0,
  called from 0x0061710e with `L - 1`), lit by the zoom map's warp (0x00617bd0) and **halved**
  (0x00617820, each pixel the mean of a 2 × 2 block). So `MINIMAP\ZOOM16`'s 8 × 6 masks are a
  cell at an eighth, as their size says.
- **The art is `Minimap.pak`'s.** Every `<layer>` of `DATA\TERRAINS.XML` names a `minimap`
  tile (the attribute is stored at 0x00622913), loaded as `minimap/zoom%d/terrain/%s`
  (0x006183b0, format at 0x007d6d60) -- 24-bit BMPs, the ground textures redrawn small, not the
  ground's `.vq` fields -- with the fourteen C and fourteen D masks beside them. Every layer's
  tile ships at all three levels but the waves' (10), whose `%season%/waves.bmp` is in no
  season's directory; 10 is on no shipped map. The waters' tiles are single images, not the
  fifteen stacked frames of their `.vq`.
- **The tile over a cell** (0x006180c0) is a dual-grid tile as on the ground: the four
  vertices at its corners, the far ones clamped at the edge. The base is the lowest layer at
  any corner, drawn whole (0x00617990); every higher layer present follows **in ascending
  order**, through the mask naming the corners it does not hold, C on even cell rows and D on
  odd (0x00617a50, the row parity is `(y / 64) & 1`), blended as
  `(over · a + under · (256 − a)) >> 8`. Unlike the ground's compositor, the waters are not
  lifted above the land layers: a road meeting shallow water is drawn over it here. A tile is
  sampled at the picture's own pixel, so a layer is one continuous field across cells; a layer
  with no tile, or a mask that does not load, is skipped.
- **The light** is the ground's: the baked level bilinear across 32-unit quads, its integer
  part choosing the gain `(L + 4) / 20` (the same `c + c·(L − 16)/20` as 0x0061e210).

So a road is drawn wherever a road vertex is, through the soft ramps, as on the ground. The
reading before this coloured a cell by the mean of one texture -- its base, replaced only by a
layer holding all four corners -- and a road one vertex wide holds two corners of the cells
either side of it and never four, so the zoom map drew none (playtest report #20). On
Crossroads the tracks between towns are *Ground* (layers 0 to 2) in the file and showed
already; its road layers, 7 and 19, 2,248 vertices, are laid at load by the settlement
templates, and are what now shows. The app prints a `zoom map:` line with the pixels at least
half road (26,035 of 1024 × 736 on Crossroads).

**Readings, labelled.** The rows a cell covers are the engine's projection's (46/64), not the
builder's 181/256, so the picture registers with the objects and the camera frame drawn on it.
The original's row loop (0x006182b0) computes a row's end from the previous row's top, so from
the third row on terrain row `k` is drawn over row `k − 1`'s span and the second is never
drawn -- read as an off-by-one, not reproduced. The warp also lifts the picture by the height
layer; this picture stays flat, as the objects placed on it are.

## Depth sorting

Straight from `DATA\ZBINS.XML` via `core::ZBins`, which already existed. A layer's `z` selects
the last bin whose `startz` is ≤ it; bins draw in declaration order; a bin flagged `sort="1"`
is depth-sorted inside itself and `sort="0"` keeps emission order. The key inside a sorted bin
is

```
(screen_y + layer.sortoffsety, screen_x + layer.sortoffsetx)
```

so ground decals draw first, then **all shadows unsorted**, then everything solid sorted, then
effects. Each layer of a multi-piece building sorts independently, which is what lets a unit
stand between the two halves of a bridge.

## The camera

The view is held in **screen pixels**, which is the space the ground rectangles and the sprite
offsets are already in; world coordinates only ever enter through the projection. It is clamped
to the map, which in screen space is `size_x` by `size_y · 46/64`. The play view is 1:1 and has
no zoom, so a map window asks SDL for window resolution rather than the HiDPI backing store —
the backing store would not magnify anything, it would show twice as much world at half the
apparent size.

## What this renders, checked against an independent implementation

`render_final_numantia.png` and `render_final_alesia.png` were produced by the Python analysis
renderer written against [`projection.md`](projection.md), from the same containers. Rendering
the same two views through the C++ engine and diffing:

| view | mean absolute difference per channel |
|---|---|
| `numantia` at (10600, 1600) — the hill wall with diagonal jogs | **2.9 / 255** |
| `alesia` at (8600, 7300) — the Gaulish camp | 8.3 / 255 |

with the terrain **pixel-identical** in both when the two sample the light layer the same way.
Three understood differences account for the rest, and two of them are deliberate.

- The C++ renderer **tints units by their owner's shipped colour**; the Python one drew every
  sprite through its neutral palette.
- The two disagree by at most one pixel vertically, because `world_y · 46 / 64` **truncates**
  here — as the specification's implementable summary says, and as an engine with no floating
  point must — and rounds there. Numantia's objects mostly sit on coordinates where the two
  agree, which is why its sprite residual is a third of alesia's.
- The C++ renderer samples the light layer at **its own 32-unit resolution**, four samples to a
  terrain cell, where the Python one takes one sample per 64-unit cell. This is the one change
  that moves terrain pixels: it raises the numantia figure above from 2.9 to 4.3 while making
  the ground visibly better, because three quarters of the layer is no longer discarded. The
  table reports the like-for-like number; 4.3 is what the shipped renderer scores.

**This comparison is history now, and cannot be rerun here.** The two reference PNGs were never
in the tree (they are renders of game data) and no test reads them. Both renderers implemented
the same *guess* at the ground -- the terrain byte covering its cell, A masks inverted, the light
a per-cell step -- and the ground is now composed as `gbr.exe` composes it (above), which moves
every terrain pixel near a boundary or a slope by design: the terrain half a cell up and left,
the masks C/D, the light interpolated. A figure against the Python render would measure how far
the engine has moved from the old guess, not how close it is to the game.

Placement is otherwise exact: towers and diagonal wall pieces meet with no gap and no overlap
at every join, buildings sit on the ground, units in front of a building occlude it and units
behind are occluded, and adjacent thatched roofs overlap in the right order.

## What is not rendered

- **Elevation is rendered now, as a mesh.** `kHeightScaleNumerator` is 1 -- one pixel of
  screen up per unit of height, which `gbr.exe` subtracts verbatim (0x005c11a0,
  [`projection.md`](projection.md)) -- and the ground is no longer a grid of rigid cells.
  `MapRenderer::compose_ground` draws every terrain cell as four quads of 32 world units whose
  corners are lifted by `Terrain.height.grid` at that corner (the layer holds the height at
  32-unit corners, as the bilinear sample the simulation uses reads it). A quad's left and
  right edges stay vertical because x passes through the projection unchanged, so each screen
  column of it is the tile's column stretched between the top and bottom edges interpolated
  at that column -- the bilinear surface the sample answers, with no seam between neighbours
  and no background showing through. Everything standing on the ground is lifted by the
  height sampled under it (`Camera::project`, `MapRenderer::add_sprite`), a click finds the
  ground it is on by the scan 0x006243b0 runs (`Camera::unproject`), and a view is centred
  on a point's lifted position. Rows up to twelve cells below the view are composed, since
  the tallest ground lifts 255 rows into it. The original
  tessellates the same way -- 32-unit quads, the height and the light both read at their
  corners (0x006217f0) -- and interpolates the light across each; see below.
- **The fog of war follows the mesh.** It was one picture stretched flat over the view, which a
  lifted ground slides out from under. It is applied the way the original applies it
  (0x00604a40): `MapRenderer::shade_ground` darkens every composed ground pixel by the fog
  sampled -- bilinearly over the 16-unit light grid -- at the world point the mesh put under
  that pixel, and `WorldView` darkens every sprite by the fog at the point it stands on. The
  ground is re-shaded when the fog grid moves, which is once a tick, and re-uploaded then.
- **Animation.** Every entity is drawn in row 0 of its frame table, because animation playback
  belongs to the simulation and a static map has none. `WorldView` draws the row the world's
  animation cursor names.

  Facing is a different matter, and is no longer open. The frame table's column selects one, and
  the mapping from a `dir.x`/`dir.y` vector to a column index **is** established: column 0 is the
  heading `(0, +1)`, towards the viewer, and the index increases towards world -x, so an
  eight-column sheet has west at 2, north at 4 and east at 6. It was read off the walk sheets of
  `UNITS\BOAR` (8 columns) and `UNITS\CNUMIDIANRIDER` (12), which show the animal head-on,
  in left profile a quarter of the way round, and from behind at the halfway column.
  `core::sim::facing_column` is the mapping and `MovementSystem` writes its result onto the
  animation cursor; `WorldView` reads it and nothing else re-derives it. Column 0 stays the right
  answer *here*, because a map object is authored `dir=(0,1)` and that is the column it names.
- **Water animation.** `dwater.vq` and `swater.vq` are fifteen stacked 440 × 320 frames; the
  renderer uses the first and does not advance it.
- **RGB555 sprites.** Classes 0 and 5 — cursors, clouds, fire — have no palette and cannot go
  in an index atlas. They need a second, truecolour path, which is not written. Nothing on a
  static map needs one.
- **The light is applied in the warp, per pixel** (0x006217f0), not to the cached tile. For each
  32-unit quad of the mesh the level at the four corners -- the same vertices as the height --
  is interpolated in 8.8 fixed point along the top and bottom edges per column and then down
  the column per screen row, and the **integer part** looks up a gain (0x0061e210):
  `c + trunc(c · (level − 16) / 20)`, clamped -- `(level + 4) / 20`, unity at 16, 0.2 at 0,
  1.25 at 21. A quad neutral at every corner is copied, as the original's fast path does. So the
  light is continuous across quads and moves in whole levels, 5% of brightness a band. **Left
  out, labelled:** the original tables the gain over 5-bit channels and blends in RGB555; this
  applies it to 8-bit channels, and its row interpolation is proportional where the original
  accumulates a truncated per-row step, which can differ by one level at a band's edge.
- **Fog of war** is drawn as the exe's fog manager draws it (`CVXFog`, 0x00606970; the
  arithmetic is `engine/core/include/imperivm/core/sim/fog_light.hpp`, the application's hold on
  it `engine/platform/include/imperivm/platform/fog_view.hpp`). A 16-unit light grid for the
  local player, one word a cell: `0x2000` is the explored floor, `0x3e00` inside a sight
  circle, `0x3c00` a 256-cell lit wholesale, 0 never seen, and a partially explored 1024-cell
  is capped at its fine record's nibble times 1024 -- the 96-unit rim of every circle ever
  stamped. Rebuilt every fourth tick of a `min(100, 100000 / speed)` ms timer over the view
  grown by 2048 (0x00607190) -- the floor, then every spawned, living object with a sight on
  the local player's side lights its circle at the cell corners with a 32-unit ramp, then the
  exploration map caps it -- and slid to that target over the four ticks; what a scroll exposes
  is snapped. The draw: a word above `0x3a00` draws nothing, below `0x400` is solid black, and
  between them the pixel is scaled by `floor(68 L / 31) / 32` for `L = word >> 10` -- 17/32 at
  the floor. The hide test (0x00605e10) is applied to units and projectiles only: buildings are
  never hidden; a unit is hidden below `0x3000` unless its owner shares its view with the
  local player, or it is an `Animal` on explored ground. Here the factor is applied per ground
  pixel at the world point under it, the grid sampled bilinearly between corners, and per
  sprite at the point it stands on (above, "The fog of war follows the mesh"). **Left out,
  labelled:** the draw's ordered dither; the sampler's own off-corner arithmetic. The zoom map does not read this
  grid at all: it paints from the exploration map, a 32-unit sub-cell explored iff its nibble
  is not zero, never-seen ground in `Const.ini`'s `MinimapEmptyColor`, and an object wherever
  the ground is explored, seen or not (0x0060fd70). `--no-fog` draws everything.
- **Selection and the command bar.** The selection rings and the bars are drawn (the
  interface layer). There is no minimap in the bar: the "Minimap (Space)" button opens the
  zoom map, and `commonini/MiniMap.ini` is the editor's floating window, reachable in-game only
  through the `MiniMap()` cheat entry (0x006148e0).
- **Objects with no art.** All 55 unresolved objects in numantia are `AdvArea`, the invisible
  script area class. That is correct behaviour, not a gap.
- **Decorations in the live view.** `WorldView` draws the map's `Terrain.decor.grid` beside
  the simulated objects — one entity per non-zero cell at the cell's corner plus its two
  four-unit nibbles, the resting pose in the neutral palette, sorted into the same depth bins
  so a unit walks behind a trunk — because the simulation has no object for a tree: nothing
  scripts one and the shipped passability already carries its footprint. Until this the play
  view drew none of the 47,930 stamps. The grid is read every frame, so a cell the editor
  paints shows at once; a decoration is never picked, so a click through a crown lands on
  what stands under it.
- **The editor's marks.** Over the fog and under the bars, pixel for pixel: every area's
  outline in the area tool's green (0x00480070 draws 0x7fe0), the selected one's handles as
  15-pixel squares (0x00458d20), and — this engine's — the brush's footprint as the outer
  edges of the cells it would touch.

## Known cosmetic issues

- The shore animation (`animate_transition`, `transition_terrain="10"` on shallow water) is
  not drawn: the original's two generated mask styles and the waves layer under them are
  read as far as their loader (0x0061e760) and no further.
- The terrain cell cache is dropped wholesale on overflow, so a long diagonal pan can stutter
  once. A recency policy would fix it; nothing has needed one yet.
