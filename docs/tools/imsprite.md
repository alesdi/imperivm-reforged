# `imsprite` — sprite tool

**Format specification:** [`../formats/rle.md`](../formats/rle.md)
**Implementation:** [`src/imperivm/cli/imsprite.py`](../../src/imperivm/cli/imsprite.py),
[`src/imperivm/sprite.py`](../../src/imperivm/sprite.py)

Inspects, exports and re-encodes the game's sprites: every unit, building, map object,
effect and shadow in Imperivm, 3,898 images and 197,432 frames.

`imsprite export-all` writes all 3,898 of them with **zero failures**, decoding
**1,062,475,100 frame pixels** out of the 400 MB pixel store in about 33 seconds. See
[Measured results](#measured-results).

## A sprite lives in two files

This is the first thing to get right, because neither half is usable alone.

- The **frame table**, `NAME.RLE.MMP`, is inside a pack — `Units.pak`, `Buildings.pak`,
  `MapObjects.pak` or `Visuals.pak`, and nowhere else. It holds the grid shape, the
  per-frame geometry, the palette, and for each frame an offset and length.
- The **pixel store** is the single 400,221,427-byte `rle.mmp` at the root of the
  installation. It has no header and no directory; it is nothing but every frame's
  compressed pixels, concatenated.

So every command that touches pixels needs both. `--game DIR` (the install directory) or
`--mmp PATH` names the store explicitly; without either, the tool looks for `rle.mmp` beside
the pack and one directory above it, which means running against `Packs/Units.pak` inside a
normal install needs no flag at all. If it cannot find the store it says so and stops rather
than guessing.

`info` without `--frames` reads only the frame table, so it works with no store present.

## Usage

```
imsprite info       <pack> <entry> [--frames] [--mmp PATH] [--game DIR]
imsprite export     <pack> <entry> --out DIR [--rgba] [--team COLOUR]
                                   [--replace-manifest] [--mmp PATH] [--game DIR]
imsprite export-all --game DIR --out DIR [--rgba] [--team COLOUR]
                                   [--packs NAME...] [--verify] [--compress N] [--mmp PATH]
imsprite sheet      <pack> <entry> --out FILE [--shadow ENTRY] [--no-shadow]
                                   [--shadow-alpha N] [--team COLOUR] [--background COLOUR]
                                   [--grid COLOUR] [--mmp PATH] [--game DIR]
imsprite import     <png> <manifest> --out FILE [--store FILE] [--entry PATH]
                                   [--base-offset N] [--image-class N] [--color-key N]
```

`<entry>` is a pack member name. Either separator works, case is ignored, and the
`.RLE.MMP` extension is optional, so `UNITS\BBOWMAN\ATTACK.RLE.MMP` and
`units/bbowman/attack` name the same image.

A `COLOUR` is a name (`red`, `blue`, `green`, `yellow`, `orange`, `purple`, `cyan`,
`magenta`, `white`, `black`, `grey`, `brown`, `teal`, `pink`), `#rrggbb`, or `r,g,b`.

### `info`

Everything the frame table says, without decoding a pixel.

```
$ imsprite info Packs/Units.pak "UNITS\BBOWMAN\ATTACK.RLE.MMP"
UNITS\BBOWMAN\ATTACK.RLE.MMP
  image class    2 (player_color)
  grid           8 columns x 15 rows = 120 frames (0 empty)
  pixel format   indexed8 (1 bytes/pixel)
  canvas         138 x 130
  bounding union (48, 32)-(137, 129)
  largest frame  69 x 93
  payload        172033 bytes in rle.mmp
  player colour  yes, palette slots 0..63 are the team block
  palette        256 entries, 15-bit lut present
                 0..63 mirror 64..127: yes
  colour key     index 161 = rgb(0, 255, 0)
```

That is the archer of the format specification's worked example, and it is worth reading
line by line because almost every field is a decision a consumer has to make.

**`grid 8 columns x 15 rows`** — the columns are the eight facing directions and the rows
are the animation steps. Frames are stored row-major, so frame `row * columns + column`.
**`canvas 138 x 130`** and **`bounding union`** are the shared coordinate space the frames
are positioned in; **`largest frame 69 x 93`** is the biggest single frame, which is what
sets the export's cell size. **`player colour yes`** plus **`0..63 mirror 64..127: yes`** is
the team-colour signature: the shipped palette's first 64 entries are a copy of the next 64,
because the engine overwrites them per player. **`colour key index 161`** is pure green,
the colour the artist painted the background in; the export reuses it as the transparent
index.

`--frames` adds one line per frame, and needs the pixel store because the coverage count is
computed by walking the run encoding:

```
$ imsprite info Packs/Units.pak "UNITS\BBOWMAN\ATTACK.RLE.MMP" --frames
...
    idx  row  col        size                 box   covered
      0    0    0 35x72         (79,53)-(113,124)       931
      1    0    1 49x72         (66,54)-(114,125)       802
      2    0    2 45x65         (67,58)-(111,122)       968
      ...
```

Read a column downwards and you are watching one facing animate; read a row across and you
are turning on the spot. The `box` changes from frame to frame and that change *is* the
motion — see [Bounding boxes are the animation](#bounding-boxes-are-the-animation).

An empty frame prints `empty` instead of a size. 913 of the 197,432 frames in the retail
data are empty, and 44 whole images are empty throughout.

### `export`

One sheet PNG plus a manifest entry.

```
$ imsprite export Packs/Units.pak "UNITS\BBOWMAN\ATTACK.RLE.MMP" --out /tmp/sprites
/tmp/sprites/units/bbowman/attack.png: 552x1395 indexed, 8x15 frames
  transparent palette index 161 (tRNS)
  manifest /tmp/sprites/manifest.json
```

The sheet is a grid of uniform cells the size of the largest frame, with each frame at its
cell's top-left. Cells are not the canvas: 8 × 69 = 552 and 15 × 93 = 1395. The frame's real
position on the canvas is in the manifest, not in the image, which keeps the file to the
pixels that exist instead of 120 copies of a 138 × 130 canvas.

The output path mirrors the pack tree, lowercased with forward slashes and the double
extension dropped: `UNITS\BBOWMAN\ATTACK.RLE.MMP` → `units/bbowman/attack.png`. `export`
**merges** into an existing `manifest.json`, replacing only the entry with the same path, so
`imterrain`, `imfont` and `immask` can write the same directory in any order.
`--replace-manifest` starts a fresh one.

`--rgba` flattens instead. `--team red` bakes a preview tint — see
[Team colour](#team-colour-is-a-palette-swap).

### `export-all`

Every sprite in the installation, in one pass over the memory-mapped store.

```
$ imsprite export-all --game . --out /tmp/sprites
3898/3898 sprites  0 failed  118.5/s    32.9s
exported 3898/3898 sprites to /tmp/sprites
  1,062,475,100 frame pixels from 400,221,427 bytes of rle.mmp
  manifest /tmp/sprites/manifest.json
```

The progress line goes to stderr and rewrites itself, so redirecting stdout gives just the
summary. A sprite that fails is reported by name and the run continues; the exit status is 1
if anything failed. Nothing accumulates in memory but the manifest — the sheets and frame
buffers are released per sprite — so the peak footprint is set by the largest single sheet,
not by the 3,898 of them.

`--packs` restricts the run (default `Units Buildings MapObjects Visuals`, which is every
pack that contains a `.rle.mmp`). `--compress` is the zlib level, default 6 rather than 9
because a full export writes about 280 MB of PNG and the last three levels cost several
times the time for a couple of percent of size.

`--verify` additionally re-encodes every decoded frame and compares the result with the
original bytes from `rle.mmp`. That is the strongest available check that the run encoding
is understood — it is a decode and an encode meeting in the middle:

```
$ imsprite export-all --game . --out /tmp/sprites --verify
exported 3898/3898 sprites to /tmp/sprites
  1,062,475,100 frame pixels from 400,221,427 bytes of rle.mmp
  manifest /tmp/sprites/manifest.json
  re-encode check: 196,519/196,519 frames byte-identical
```

### `sheet`

A contact sheet for looking at, as opposed to a sheet for consuming. Here the frames *are*
laid out in canvas coordinates, so the animation reads correctly on screen, and the shadow
is composited underneath.

```
$ imsprite sheet Packs/Units.pak "UNITS\BBOWMAN\ATTACK.RLE.MMP" --out /tmp/attack.png \
      --team red --grid 40,40,40
/tmp/attack.png: 920x1470, 8x15 cells of 115x98
  shadow UNITS\BBOWMAN\ATTACK_SHADOW.RLE.MMP composited underneath at alpha 110
```

The output is always RGBA. `--background` fills behind the sprites, `--grid` draws cell
separators, `--shadow-alpha` scales the shadow's opacity (default 110 of 255).

The shadow companion is guessed from the naming, because the format does not link the two
files — the entity XML does. `X` pairs with `X_SHADOW`, and a building directory commonly
keeps one `SHADOW` or `<DIR>_SHADOW` beside several body layers, so
`BUILDINGS\RBARRACKS\RBARRACKS_A_W.RLE.MMP` finds
`BUILDINGS\RBARRACKS\RBARRACKS_SHADOW.RLE.MMP`. Name one explicitly with `--shadow`, which
is an error if it is not in the pack, or suppress the search with `--no-shadow`. When the
guess finds nothing the tool says `no shadow sheet found next to this one` rather than
silently drawing an unshadowed sprite.

### `import`

Turns a sheet PNG and its manifest entry back into a frame table plus a payload file.

```
$ imsprite import /tmp/sprites/units/bbowman/attack.png /tmp/sprites/manifest.json \
      --out /tmp/attack.rle.mmp
/tmp/attack.rle.mmp: 8046 bytes, 8x15 frames (120 populated), class 2
/tmp/attack.rle.mmp.store: 172033 bytes of payload, offsets from 0
  re-read and validated as a frame table
  note: data_offset addresses the companion store, not the game's shared rle.mmp
```

Both numbers are the retail file's own: the frame table and the payload of that sprite come
back **byte for byte identical** to the shipped `ATTACK.RLE.MMP` and to bytes 0..172,032 of
`rle.mmp`. Indexed, greyscale and RGBA sheets are all accepted; the image class, the colour
key and the palette are inferred from the manifest and the PNG, and can be overridden with
`--image-class` and `--color-key`.

**`import` is a working encoder, not a mod installer, and the gap between those two is
the point of this section.** It writes a *companion* store — `OUT.store` by default, or
`--store PATH` — whose offsets start at `--base-offset` (default 0). It does not and cannot
splice a payload into the game's shared 400 MB `rle.mmp`, because the shipped store is a
gapless concatenation in an order that has not been characterised: changing any frame's
size shifts every subsequent frame in the file and invalidates the `data_offset` of every
other sprite in the game. Building a whole replacement store, and whatever the engine needs
to accept one, is not implemented here.

Three fields also cannot survive the PNG round trip and are reconstructed rather than
recovered:

- **The degenerate box of an empty frame.** The retail encoder leaves an untouched starting
  value that varies per file (`(n, n, 0, 0)` for some `n` between 170 and 450, and in 43 of
  the 913 empty frames `left != top`). `import` writes `(1, 1, 0, 0)`, which reads back as
  empty — the only property that matters — but is not the original bytes.
- **The 15-bit lookup table** of a class 2 image is rebuilt from the palette with the filler
  `0x6666` in slots 0..63, which is what 1,651 of the 1,668 retail class 2 files hold. The
  16 that fill those slots in, and the one file whose table disagrees with its own palette,
  are not reproduced.
- **`data_offset`**, as above.

Sprites without empty frames and without those quirks — the archer above is one — therefore
round-trip exactly; sprites with them round-trip to a valid, correctly decoding file that is
not byte-identical.

## What the export preserves, and why

### Indexed is the default, and `--rgba` is lossy

An 8-bit sprite is exported as an **indexed PNG with its palette intact**. This is not a
size optimisation, and `--rgba` is not merely a convenience.

Team colour in this engine is a palette swap. Palette entries 0..63 of a `player_color`
image are the block the engine overwrites with the owning player's 64-entry ramp at load
time; the shipped values there are a byte-identical copy of entries 64..127, a neutral
default so the sprite still reads correctly untinted. The pixels that belong to the player —
a tunic, a sash, a banner — are exactly the pixels whose index is below 64.

Flatten that to RGBA and the indices are gone. What is left is one particular colouring of
the sprite with no way to recover which pixels were recolourable, because the neutral block
and the real block are the same colours. **`--rgba` therefore destroys recolouring**, and it
is the right choice only for a consumer that cannot read a `PLTE` chunk at all. 2,483 of the
3,898 exported sheets are indexed; 1,668 of them carry a team block.

Transparency needs an index, since the sprite format has no alpha channel — gaps in the run
encoding are the only transparency there is. The export reserves one palette index and marks
it in a `tRNS` chunk. The candidate is the frame's own `color_key`, and it is *checked*:
`choose_transparent_index` uses it only if no covered pixel in the image actually uses that
index, falls back to the lowest unused index at or above 64 so that a team slot is never
quietly reserved, and appends a fresh entry if the palette has room. If every index is
genuinely in use the sheet falls back to RGBA rather than being written with no transparency
at all. No retail image needs that fallback.

Shadows are exported as 8-bit greyscale coverage, and truecolour and cloud images as RGBA —
those have no palette to preserve.

### Bounding boxes are the animation

Each frame's `left`/`top` is its position on the canvas that every frame of the image, and
its shadow image, share. Two frames of one animation differ by their bounding box, and that
difference is the motion: the archer's box moves and resizes from frame to frame as he draws
and looses.

The manifest records `x`/`y` (where the frame sits in the exported sheet) *and*
`width`/`height`/`left`/`top` (where it sits on the canvas) for every frame, empty ones
included, so that frame indices stay aligned with the original grid:

```json
{ "row": 0, "column": 1, "x": 69, "y": 0,
  "width": 49, "height": 72, "left": 66, "top": 54, "empty": false }
```

**Do not normalise the boxes away.** Re-centring each frame in its cell, or cropping each
frame to its own content and discarding `left`/`top`, produces a sheet that looks fine as a
grid of pictures and visibly jitters the moment it is animated.

### Shadows are separate images that composite by bounding box

A shadow is its own `.rle.mmp`: image class 4, pixel format 7, a 1-bit coverage mask with no
palette and no pixel payload at all — a run carries a length and no bytes. It is referenced
by its own layer in the entity XML with the same `offsetx`/`offsety` as the body, so the two
share a canvas and line up by their boxes with no further arithmetic. That is exactly what
`sheet` exploits: it takes the union box over body and shadow, lays both out against it, and
composites.

1,319 of the 3,898 images are shadows, and they are 8.5% of the payload bytes but 33% of the
pixels, because a silhouette compresses to almost nothing.

### Team colour is a palette swap

`--team` re-hues the 64 neutral team slots towards a colour, for previewing.

It is honest about what it is. The real per-player ramps are not in any `.rle.mmp` — they
live in the executable or the balance data and have not been sourced. And the 64 slots are
not a sorted gradient: across 1,568 unit and building images only 51% of adjacent slot pairs
increase in luminance, which is indistinguishable from random, so laying a generated
dark-to-light ramp over indices 0..63 would scramble the shading. `--team` therefore keeps
each slot's own brightness and takes only hue and saturation from the given colour, so folds
and highlights survive and the cloth changes colour.

That is a faithful *preview*, not a particular player's palette. On an indexed export the
tint is applied to the palette, so the sheet is still fully re-tintable afterwards; it is
only baked in irreversibly when combined with `--rgba`.

### The frame table is authoritative, not the entity XML

The `<image>` element that references a sprite also declares `rows` and `columns`. It is
wrong often enough to be useless: **430 of the 4,033 image references in the retail data
declare a grid that disagrees with the file they point at** (independently reproduced here
against all four packs). The `drawmode` attribute is wrong in the same way — 274 references
label a 1-bit shadow mask `player_color`.

`imsprite` never reads the entity XML. Every number it reports comes from the frame table.

## Measured results

Run against the retail install, Python 3.13 on macOS (Apple silicon):

| | |
|---|---|
| frame tables found | **3,898** — 1,587 `Units`, 1,339 `Buildings`, 853 `MapObjects`, 119 `Visuals` |
| sprites exported | **3,898 (0 failures)** |
| frames | 197,432, of which 913 empty |
| frame pixels decoded | **1,062,475,100** |
| payload read | 400,221,427 bytes — the whole of `rle.mmp` |
| `export-all` wall time | **33.3 s** (118 sprites/s) |
| PNG bytes written | 292,871,737 (330 MB on disk) |
| indexed sheets | 2,483 (1,668 with a team block) |
| greyscale shadow sheets | 1,319 |
| RGBA sheets | 96 |
| `export-all --verify` | **196,519 / 196,519 frames re-encode byte-identical**, 74.1 s |
| single-sprite `import` | frame table and payload both byte-identical to retail for `UNITS\BBOWMAN\ATTACK.RLE.MMP` |

The re-encode check is the load-bearing number. It decodes every populated frame in the game
to coverage plus pixels, encodes it again from scratch, and compares with the original bytes
in `rle.mmp` — gap and run splitting, the `(255, 0)` long-gap idiom, the row offset table and
the 32-bit variant on the 439 oversized frames included. All 196,519 match.

`VISUALS\MIST_PLACING\EMPTY.RLE.MMP` is a 1 × 1 grid whose one frame is empty, and 43 shadow
images are empty throughout. These 44 have no populated frame and therefore no pixel format
of their own; they are exported as a 1 × 1 transparent PNG so that the manifest entry points
somewhere, and their `pixel_format` in the manifest is inferred from the image class rather
than from a frame.

## Limitations

- **The shared pixel store cannot be rewritten.** `import` produces a companion store, not a
  patch to `rle.mmp`; see [`import`](#import) for why. Without that, `imsprite` is an
  exporter with a validated encoder attached, not a modding pipeline.
- **Empty-frame boxes, the 15-bit lookup table and `data_offset` are reconstructed**, not
  recovered, so `import` is not byte-exact for every image. The three cases are listed above.
- **Blend modes are not exported.** Nothing in the sprite format says how a class is
  composited. `sheet` draws shadows as a semi-transparent dark pass and `clouds` images
  normally; the real engine almost certainly blends `clouds` additively, but that is an
  inference from the pixels, not a fact read out of the bytes.
- **The vertical anchor is not established.** `-offsetx` from the entity XML is the canvas's
  horizontal centre to within a pixel or two; the vertical convention is not pinned down, so
  the manifest carries canvas coordinates and leaves the on-screen origin to the consumer.
- **Player ramps are not real.** `--team` previews a tint. The shipped per-player ramps are
  not in these files.
- **Exports are large.** A full export is 330 MB of intermediate PNG. Keep it out of the
  repository.
- **`sheet` is deliberately slow and RGBA-only.** It composites in Python a pixel at a time.
  It is for looking at one sprite, not for bulk work.
