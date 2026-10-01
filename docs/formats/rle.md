# HMMSYS sprite (`*.rle.mmp` and `rle.mmp`)

**Status:** decoded and validated
**Reference reader:** [`src/imperivm/formats/rle.py`](../../src/imperivm/formats/rle.py)

Every animated or static sprite in the game — units, buildings, decor, effects and their
shadows — is stored in this format. It is split across two files:

- a **frame table**, `NAME.RLE.MMP`, stored inside a [pack](pak.md). It holds the grid
  shape, the per-frame geometry, the palette, and for each frame an offset into…
- the **pixel store**, the single 400,221,427-byte `rle.mmp` at the install root, which the
  engine memory-maps. It has no header of its own; it is nothing but the concatenated
  compressed pixel payloads of every frame in the game.

The `pamm` tag that separates the two halves of a frame record is `mmap` with its bytes
reversed, which is what named the store.

All integers are little-endian and unsigned.

## The frame table

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00     6    magic, "IMGRLE"
0x06     4    image_class  u32   pixel storage class, see below
0x0A     4    reserved     u32   always 0
0x0E     4    reserved     u32   always 0
0x12     4    columns      u32
0x16     4    rows         u32
0x1A     -    frame records, columns * rows of them
 ...     4    palette_len  u32   number of palette entries, 0..256
 ...     -    palette, 4 * palette_len bytes
 ...   512    15-bit lookup table, present only for image_class 2
```

The file ends exactly at the end of the palette, or of the lookup table when there is one.

Frames are stored row-major: frame `row * columns + column`. What the two axes mean is up
to the entity that references the image; in unit and building sprites the **column selects
the facing direction** (almost always 8 of them) and the **row selects the animation step**.
This is visually confirmed — decoding `UNITS\BBOWMAN\ATTACK.RLE.MMP` as an 8x15 sheet
produces eight consistent facings down the columns.

The matching `<image>` element in the entity XML also declares `rows` and `columns`, but it
is not reliable: 430 of the 4,033 image references in the retail data disagree with the file
they point at. **The frame table is authoritative.**

### Frame records

A populated frame is 54 bytes:

```
size  field
----  ---------------------------------------------------------------
 4    left          u32   inclusive bounding box, in canvas coordinates
 4    top           u32
 4    right         u32
 4    bottom        u32
 4    magic, "RLE2"
 4    width         u32
 4    height        u32
 2    pixel_format  u16
 4    data_size     u32   payload length in rle.mmp
 4    reserved      u32   always 0
 4    wide_rows     u32   0 or 1, see "row offset table"
 4    color_key     u32
 4    magic, "pamm"
 4    data_offset   u32   absolute offset into rle.mmp
```

`left + width - 1 == right` and `top + height - 1 == bottom` hold for every populated frame
in the retail data, so the box and the size are redundant with each other.

### Empty frames

A frame with no pixels at all is stored as **only the 16-byte bounding box**, with no `RLE2`
or `pamm` block following it. The encoder grows the box inwards from a degenerate starting
value and never touches it, so the stored box is `(n, n, 0, 0)` for some `n` between 170 and
450 — the initial value, larger than the canvas.

Detect this with `right < left or bottom < top` and skip the rest of the record. Do not test
for a particular sentinel: `n` varies per file, and in 43 of the 913 empty frames
`left != top`. 913 of the 197,432 frames in the retail data are empty.

### Palette

`palette_len` entries of 4 bytes each, in Windows `RGBQUAD` order:

```
size  field
----  -----------
 1    blue
 1    green
 1    red
 1    zero
```

The fourth byte is padding, not alpha: it is zero in all but 45 of the 633,606 palette
entries in the retail data, and all 45 exceptions are in the one corrupt file noted below.

Only 8-bit images carry a palette. `image_class` 2 always stores exactly 256 entries;
`image_class` 1 stores between 1 and 256; classes 0, 4 and 5 store `palette_len == 0`.

### 15-bit lookup table

`image_class` 2 files end with a further 512 bytes: 256 `u16` entries holding the palette
pre-converted to RGB555, `(r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)`. This is the table the
16-bit software rasteriser actually indexed with.

Entries **64..255 always match the palette**. Entries 0..63 are left as the filler value
`0x6666` in 1,651 of the 1,668 files — those are the player-colour slots, which the engine
fills in per player at load time (see below). The remaining 16 files populate them from the
palette anyway. One file, `BUILDINGS\EWALLS\WALLS\LAYER3.RLE.MMP`, has a table that
disagrees with its own palette in 75 of the 192 non-reserved entries. That same file is the
only one in the install with a non-zero palette padding byte, so it is best read as
locally corrupt rather than as a second variant of the format; it parses and decodes fine.

The table is redundant — it can be rebuilt from the palette — so a reimplementation can
ignore it entirely.

## The pixel store

`rle.mmp` has no header, no directory and no padding. Each frame's `data_offset` is an
absolute byte offset into it and `data_size` is the exact payload length.

The 196,519 populated frames of the retail install tile the file **exactly**: sorted by
offset, the first starts at 0, each subsequent payload begins where the previous one ended,
and the last ends on byte 400,221,427. There are no gaps and no overlaps, and no two frames
share a payload. The store is therefore a pure concatenation in a fixed order, and nothing
outside the frame tables is needed to address it.

## The compressed rows

A payload is a row offset table followed by the rows, in order.

### Row offset table

`height` entries giving the byte offset of each row's run data, measured from the start of
the payload. Entries are `u16` normally and `u32` when the frame record's `wide_rows` flag
is 1. The flag is set on exactly the 439 frames whose `data_size` exceeds 65,535 — it exists
solely because a `u16` offset could not reach the end of those payloads.

`offsets[0]` always equals `height * entry_size`, i.e. the table abuts the row data. Offsets
are monotonically non-decreasing. The last row runs to the end of the payload.

### Run encoding

Each row is a flat sequence of pairs, consuming the row left to right:

```
size  field
----  -----------------------------------------------
 1    gap     u8   transparent pixels to skip
 1    length  u8   covered pixels that follow
 n         payload, length * bytes_per_pixel
```

The row ends when its byte range is exhausted; there is no terminator. Because both counts
are single bytes, a gap wider than 255 is written as one or more `(255, 0)` pairs before the
real pair, and a run longer than 255 is split. A decoder that simply advances by
`gap + length` per pair handles both cases without special-casing them.

Transparency is carried entirely by the gaps. There is no alpha channel anywhere in the
format, and the `color_key` is never needed to decode.

### Pixel formats

| `pixel_format` | Meaning | Bytes per covered pixel | Frames |
|---:|---|---:|---:|
| 1 | palette index | 1 | 102,153 |
| 3 | RGB555, `0rrrrrgggggbbbbb`, little-endian | 2 | 671 |
| 7 | coverage mask, no payload at all | 0 | 93,695 |

Format 7 is how shadows are stored: the pairs still alternate gap and run, but a run carries
no bytes, so the payload is a pure 1-bit silhouette. It is very compact — a 45x45 shadow
frame fits in 302 bytes.

Note that the truecolour format is **RGB555, not RGB565**. The 15-bit lookup table appended
to `image_class` 2 files uses the same packing, which cross-confirms it.

## Image classes

`image_class` selects the storage strategy. It correlates with the `drawmode` attribute of
the entity XML `<image>` element, but again the XML is unreliable. Of the 4,033 image
references in the retail data, 274 label a 1-bit shadow mask as `player_color` (usually a
`shadow.rle` under a building), 186 label an image with no player-colour block as
`player_color`, and one labels a class 2 unit body as `shadow`. Use the file.

| `image_class` | `pixel_format` | Palette | LUT | Usual `drawmode` | Files |
|---:|---:|---|---|---|---:|
| 0 | 3 | – | – | `normal` (29/29) | 29 |
| 1 | 1 | 1..256 | – | `normal`, `index` | 816 |
| 2 | 1 | 256 | yes | `player_color` | 1,668 |
| 4 | 7 | – | – | `shadow` | 1,319 |
| 5 | 3 | – | – | `clouds` | 66 |
| 6 | 1 | – | – | none: not an entity image | 18 |

Class 3 does not occur in the retail data. Class 6 is not in the sprite packs: its 18 files
are `UI\SELECTIONS\<n>.RLE` in `UI.pak`, the selection rings (below), and every other count
in this document is of the `.rle.mmp` frame tables only. The executable's `drawmode` string table also
contains `alpha`, which no shipped image uses.

Every frame of a given file has the same `pixel_format`.

### `drawmode="player_color"` — class 2

The 256-entry palette of a class 2 image is not 256 independent colours. **Entries 0..63 are
a byte-identical copy of entries 64..127.** This holds for all 1,668 class 2 files and for no
class 1 file.

Slots 0..63 are the team-colour block. Pixels whose index falls below 64 are the parts of the
sprite that belong to the owning player — a tunic, a sash, a banner — and the shipped copy of
64..127 is only a neutral default so the sprite still reads correctly untinted. At runtime the
engine substitutes a 64-entry ramp for the owning player, which is exactly why the shipped
15-bit lookup table leaves its first 64 slots blank.

Decoding is therefore: build the palette, optionally overwrite entries 0..63 with the player's
ramp, then index it normally. Substituting a red ramp and a blue ramp into
`UNITS\BBOWMAN\ATTACK.RLE.MMP` recolours precisely the archer's sash and belt and nothing
else, which is the expected result.

The 64 slots are *not* a sorted gradient. Across 1,568 unit and building images, only 51% of
adjacent slot pairs increase in luminance — indistinguishable from random, so the slots are
in whatever order the quantiser produced. A correct tint therefore has to map slot to colour
using the shipped neutral colours as the reference, not by laying a generated dark-to-light
ramp over indices 0..63. The per-player ramps are not stored in any `.rle.mmp`; they live
elsewhere (the executable mentions a `PlayerColors` key). Sourcing them is out of scope for
this document.

### `drawmode="shadow"` — class 4

A class 4 image is a 1-bit coverage mask (`pixel_format` 7) with no palette. It is a separate
image from the body, referenced by its own layer in the entity XML with the same `offsetx` and
`offsety`, so the two share a canvas and composite by their bounding boxes without any extra
alignment maths. The engine draws it as a darkening pass under the body; the format itself
says nothing about the blend, only about coverage.

### `drawmode="clouds"` — class 5

RGB555 like class 0, but with `color_key == 0` rather than a green key, and the decoded
frames have a black surround inside their covered area — a fire burst decodes as a bright
core fading to black at the edges. That is the signature of additive blending. **The blend
mode is inferred from the pixel content, not proven from the bytes.**

### Class 6 — coverage images with the pixels inside

The selection rings, `UI\SELECTIONS\20.RLE` to `150.RLE` (eighteen files, the number being
the `selection_radius` each is for), are frame tables of image class 6, and they differ from
every other class in two ways:

- **the payload is in the file.** A populated frame record ends after `color_key`; instead of
  `pamm` and an offset into `rle.mmp`, its `data_size` bytes of row table and runs follow
  directly, and the next record starts after them. Empty frames are the usual 16 bytes.
- **there is no palette.** `palette_len` is 0, and the `pixel_format` 1 bytes are not indices
  but coverage, 1 to 255: the rings are soft-edged ellipses, and the game draws them in one
  colour, tinted per object (`gbr.exe` passes the colour to the image's draw at 0x0062b5b0).
  How the coverage blends with the picture below was not read; the reimplementation treats it
  as alpha (`platform/selection_marks.cpp`).

The nine files up to `60` hold two frames (`columns` 1, `rows` 2): the ellipse solid, and the
same ellipse dotted. Those from `70` up hold only the solid one.

### `color_key`

The colour the encoder treated as transparent in the source bitmap, expressed in the frame's
own pixel format:

- `pixel_format` 1: a palette index. Pure green `(0, 255, 0)` in 71,691 of 102,153 frames, a
  mid grey around `(125, 129, 130)` in most of the rest.
- `pixel_format` 3: a packed RGB555 value — `0x03E0` green, `0x7C1F` magenta, `0x7C00` red,
  or `0` for clouds.
- `pixel_format` 7: a 24-bit RGB value — `0x00FF00` green in 93,593 of 93,695 frames,
  `0xFF00FF` magenta in most of the remainder.

It is constant across every frame of a file, and the keyed value **never appears in the
compressed data** (verified over a 1,200-frame sample for 8-bit and over every RGB555 frame).
It is a record of how the sprite was authored and is not needed to decode.

## Geometry and anchoring

The four bounding box fields are not relative to the frame — they are absolute coordinates in
a canvas shared by every frame of the image, and by the matching shadow image. Compositing an
animation is therefore just "blit each frame's pixels at `(left, top)`".

The canvas origin is placed on screen using the `offsetx`/`offsety` of the entity XML
`<layer>` that references the image. Across the 2,388 layers that can be resolved this way,
the mean of `(left + right) / 2 + offsetx` over all frames of an image is -5.1 pixels and the
median -1.5, i.e. **`-offsetx` is the horizontal centre of the sprite** to within rounding.
The vertical relationship is looser (median 36.5 pixels between the bottom of the box and
`-offsety`) because ground shadows and building foundations extend well below the entity's
own footprint; the exact vertical anchor convention is not pinned down here.

## Worked example

`UNITS\BBOWMAN\ATTACK.RLE.MMP` from `Units.pak`, 8,046 bytes. The entity declares it
`drawmode="player_color" rows="15" columns="8"` on a layer at `offsetx="-93" offsety="-114"`.

| Offset | Bytes | Field | Value |
|-------:|-------|-------|------:|
| `0x00` | `49 4D 47 52 4C 45` | magic | `IMGRLE` |
| `0x06` | `02 00 00 00` | image_class | 2 |
| `0x0A` | `00 00 00 00` | reserved | 0 |
| `0x0E` | `00 00 00 00` | reserved | 0 |
| `0x12` | `08 00 00 00` | columns | 8 |
| `0x16` | `0F 00 00 00` | rows | 15 |

Frame 0 follows at `0x1A`:

| Offset | Bytes | Field | Value |
|-------:|-------|-------|------:|
| `0x1A` | `4F 00 00 00` | left | 79 |
| `0x1E` | `35 00 00 00` | top | 53 |
| `0x22` | `71 00 00 00` | right | 113 |
| `0x26` | `7C 00 00 00` | bottom | 124 |
| `0x2A` | `52 4C 45 32` | magic | `RLE2` |
| `0x2E` | `23 00 00 00` | width | 35 |
| `0x32` | `48 00 00 00` | height | 72 |
| `0x36` | `01 00` | pixel_format | 1 |
| `0x38` | `89 05 00 00` | data_size | 1417 |
| `0x3C` | `00 00 00 00` | reserved | 0 |
| `0x40` | `00 00 00 00` | wide_rows | 0 |
| `0x44` | `A1 00 00 00` | color_key | 161 |
| `0x48` | `70 61 6D 6D` | magic | `pamm` |
| `0x4C` | `00 00 00 00` | data_offset | 0 |

120 frames of 54 bytes end at `0x196A`, where `palette_len = 256`; the 1,024-byte palette runs
to `0x1D6E` and the 512-byte lookup table to `0x1F6E`, the end of the file. Palette entry 161
is `00 FF 00 00`, pure green — the colour key, as expected. Palette entries 0..63 are
identical to 64..127.

Frame 0's payload is the first 1,417 bytes of `rle.mmp`. It opens `90 00 96 00 A0 00 …`: a
72-entry `u16` row table whose first entry is 144, which is `2 * 72`. Row 0 therefore lives at
144..149 and reads `0B 02 BC BC 16 00` — skip 11, draw 2 pixels of index `0xBC`, skip 22, draw
0. That is `11 + 2 + 22 = 35` pixels, exactly the frame width. Palette entry `0xBC` is
`(167, 150, 128)`, and lookup table entry `0xBC` is `0x5250`, which is that colour in RGB555.

The frame decodes to a 35x72 Roman archer.

## Validation

The reference reader asserts all of the following, and they hold for every one of the 3,898
frame tables and 196,519 populated frames in the retail install:

- the magic is `IMGRLE`, and both reserved header words are zero;
- every populated frame record carries `RLE2` and `pamm` at the expected offsets, and its
  reserved word is zero;
- `left + width - 1 == right` and `top + height - 1 == bottom`;
- the parse cursor lands exactly on the end of the file after the palette and, for class 2,
  the 512-byte lookup table — no unexplained trailing bytes;
- all frames of a file share one `pixel_format`;
- 8-bit frames have a palette and a `color_key` inside it;
- `wide_rows` is set if and only if `data_size > 0xFFFF`;
- for class 2, palette entries 0..63 equal entries 64..127;
- the row offset table is monotonic and `offsets[0] == height * entry_size`;
- every row's pairs consume its byte range exactly, and its `gap + length` counts sum to
  exactly `width`;
- payloads tile `rle.mmp` with no gaps and no overlaps, from byte 0 to the last byte.

## Contents of the retail install

3,898 frame tables, 197,432 frames, 400,221,427 bytes of pixel data.

| Pack | Frame tables |
|------|-------------:|
| `Units.pak` | 1,587 |
| `Buildings.pak` | 1,339 |
| `MapObjects.pak` | 853 |
| `Visuals.pak` | 119 |

No other pack contains a `.rle.mmp`.

| Format | Payload bytes | Share | Pixels if expanded |
|--------|--------------:|------:|-------------------:|
| 8-bit indexed | 351,722,245 | 87.9% | 705,356,395 |
| 1-bit mask | 33,901,642 | 8.5% | 346,237,081 |
| RGB555 | 14,597,540 | 3.6% | 10,881,624 |

The largest single frame is 1939x1427, `BUILDINGS\MUTABLESTRONGHOLD\LAYER1.RLE.MMP` frame 0,
1,498,989 bytes.

## What is still unknown

- **The player-colour ramps.** The format proves that palette slots 0..63 are the remappable
  block, but the 64-entry ramp for each player is not in these files. It has to come from the
  balance data or the executable.
- **Blend modes.** Nothing in the sprite format says how a class is composited. `clouds`
  looking additive and `shadow` looking like a darkening pass are inferences from the decoded
  pixels and from the `drawmode` names, not facts read out of the bytes.
- **The vertical anchor.** `-offsetx` is the horizontal centre of the canvas to within a
  pixel or two; the vertical convention is not established.
- **Class 6's second frame.** Which of a ring's two frames the game draws, and when (a blink,
  a state), has not been read; the solid frame 0 is drawn.
- **`image_class` 3 and `drawmode="alpha"`.** Named in the executable, used by nothing in the
  retail data, so its storage is unknown.
- **The ordering of `rle.mmp`.** The payloads tile it exactly, but the *order* in which they
  were written has not been characterised, so a rebuilt store would not be byte-identical
  unless the order is reproduced. This does not affect reading.
- **Why the 15-bit lookup table exists in the file at all**, given it is derivable from the
  palette, and why 16 class 2 files fill in the reserved 64 slots while 1,651 do not.
- **`BUILDINGS\EWALLS\WALLS\LAYER3.RLE.MMP`**, whose lookup table does not match its palette.
  Everything else about the file parses and decodes normally.
