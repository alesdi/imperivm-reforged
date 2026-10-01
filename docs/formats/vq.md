# HMMSYS vector-quantised bitmap (`.vq`)

**Status:** decoded and validated
**Reference reader:** [`src/imperivm/formats/vq.py`](../../src/imperivm/formats/vq.py)

The terrain texture format. All 55 `.vq` files ship in `Terrain.pak` and are referenced by
`DATA\TERRAINS.XML` in `data.pak`. Nothing else in the install uses the format.

Despite the name, this is not block vector quantisation in the S3TC sense. The quantisation
unit is a **run of four horizontally adjacent pixels**. An image is a grid of such runs; each
run is replaced by an index into a shared codebook. That gives a fixed 4:1 (8-bit index) or
2:1 (16-bit index) compression on top of the 16-bit pixel format, and costs the decoder one
lookup and one 8-byte copy per four pixels — cheap for a software rasteriser.

All integers are little-endian and unsigned. The magic reads `mbqv` as bytes; it is the
four-character code `vqbm` — "vector quantised bitmap" — stored as a little-endian `u32`,
which is how the engine compares it.

## Layout

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00     4    magic, "mbqv" (6D 62 71 76)
0x04     4    index_size_log2  u32   0 or 1
0x08     4    codebook_len     u32   entries, always a power of two
0x0C     4    index_size       u32   bytes per index, 1 or 2
0x10     4    bytes_per_pixel  u32   always 2
0x14     4    block_width      u32   always 4
0x18     4    block_height     u32   always 1
0x1C     4    width            u32   pixels
0x20     4    height           u32   pixels
0x24     4    blocks_x         u32   width  / block_width
0x28     4    blocks_y         u32   height / block_height
0x2C     -    codebook, codebook_len entries of 8 bytes
 ...          index array, blocks_x * blocks_y indices of index_size bytes
```

The header is 44 bytes. There is no padding anywhere: the codebook starts at `0x2C`, the
index array starts immediately after it, and the last index is the last byte of the file.

`index_size == 1 << index_size_log2` in every shipped file, so the two fields are redundant.
`index_size` is 1 exactly when `codebook_len <= 256`.

`block_width` and `block_height` are constant across the retail data, so their assignment to
those two header words is inferred rather than proven — but the *geometry* they describe is
proven (see [Validation](#validation)). The same applies to `bytes_per_pixel`: what is proven
is that a codebook entry is 8 bytes covering 4 pixels.

## Codebook

`codebook_len` entries, 8 bytes each, at offset `0x2C`. Each entry is four `u16` pixels, in
left-to-right order.

Pixels are **X1R5G5B5**, not RGB565:

```
bit  15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
      x  r  r  r  r  r  g  g  g  g  g  b  b  b  b  b
```

Bit 15 is never set — verified across all 141,568 codebook pixels in the retail data, whose
maximum value is exactly `0x7FFF`. Expand a 5-bit channel to 8 bits with `(v << 3) | (v >> 2)`.

Codebook length is always a power of two but is not always fully used; unused entries are
zero-filled. Zero (`0x0000`, opaque black) is a legitimate colour — `ROCKS1024.VQ` references
an all-zero entry from its index array — so it is *not* a colour key.

## Index array

`blocks_y` rows of `blocks_x` indices, top row first, left to right, no row padding. Each
index selects a codebook entry, which supplies pixels `x = 4*bx .. 4*bx+3` of row `by`.

Indices are 1 byte when `index_size == 1`, otherwise a little-endian `u16`. No index in the
retail data is out of range.

Decoding is therefore:

```
for by in range(blocks_y):
    for bx in range(blocks_x):
        pixels[by][4*bx : 4*bx+4] = codebook[index[by][bx]]
```

## Worked example

`TERRAIN\INVALID.VQ`, 3,628 bytes — the placeholder tile the editor shows for an unassigned
terrain layer.

| offset | value | meaning |
|--------|-------|---------|
| 0x00 | `mbqv` | magic |
| 0x04 | 0 | 1-byte indices |
| 0x08 | 64 | 64 codebook entries |
| 0x0C | 1 | index size |
| 0x10 | 2 | bytes per pixel |
| 0x14 | 4 | block width |
| 0x18 | 1 | block height |
| 0x1C | 128 | width |
| 0x20 | 96 | height |
| 0x24 | 32 | blocks per row |
| 0x28 | 96 | block rows |

Size check: `44 + 64*8 + 32*96*1 = 44 + 512 + 3072 = 3628`. Exact.

Codebook entry 55 is `1f 7c 1f 7c 1f 7c 1f 7c`, i.e. four pixels of `0x7C1F` = R31 G0 B31 =
pure magenta. The first index row is 32 copies of `55`, so row 0 of the image is 128 pixels of
magenta. Row 20 reads `55 55 54 56 55 55 ...`, the left edge of a glyph. Decoded, the image is
a 128x96 magenta field with the word "invalid" in white serif type.

## Validation

The reader asserts all of these, and they hold for every one of the 55 retail `.vq` files:

- Magic is `mbqv`.
- `index_size == 1 << index_size_log2`, and `index_size` is 1 or 2.
- `codebook_len` is a power of two, and `index_size == 1` implies `codebook_len <= 256`.
- `blocks_x * block_width == width` and `blocks_y * block_height == height`.
- `0x2C + codebook_len*8 + blocks_x*blocks_y*index_size == len(file)`, exactly.
- Every index is `< codebook_len`.
- No codebook pixel has bit 15 set.

Two independent facts pin the 4x1 block geometry rather than the more usual 2x2:

1. **Autocorrelation.** The index array's strongest self-similarity lag is exactly `blocks_x`
   in every file tested (e.g. 0.020 at lag 256 for `AUTUMN\GRASS1024.VQ` against a 0.008
   background), with secondary peaks at `blocks_x ± 1` and `2*blocks_x`. That is the signature
   of an image whose row stride is `blocks_x` blocks.
2. **Visual.** Decoded at `width = 4*blocks_x`, `AUTUMN\RROADS.VQ` is a coherent 512x512
   Roman cobblestone road and `ROCKS1024.VQ` is a 1024x800 cliff face. Decoded at
   `width = 2*blocks_x` with 2x2 blocks, both are vertically smeared noise. `INVALID.VQ`
   renders legible text only under the 4x1 reading.

## Image dimensions

Widths are always powers of two; heights are not, and are frequently not even multiples of
anything obvious (682, 800, 350, 199). These are photographic source textures cropped to
whatever the artist produced, sampled by the terrain renderer with an explicit modulo rather
than by power-of-two masking.

Sizes present in the retail data:

| Dimensions | Files |
|------------|-------|
| 256 x 200 | `roads`, `iroads`, `ground256`, `rocks256`, `sandswave` |
| 256 x 199 | `waves` |
| 512 x 300 | `rocks2` |
| 512 x 350 | `ground1024` |
| 512 x 400 | `sroads` |
| 512 x 440 | `grass512`, `ground`, `sands`, `sand_grass512`, `sands_mix512` |
| 512 x 512 | `rroads` |
| 1024 x 682 | `grass1024`, `swamp1024` |
| 1024 x 800 | `rocks1024` |
| 440 x 4800 | `dwater`, `swater` |
| 128 x 96 | `invalid` |

Note that the number in a filename is the artist's naming convention for the source texture
resolution and does **not** match the header — `AUTUMN\GROUND1024.VQ` is 512x350.

### Animated water

`DWATER.VQ` and `SWATER.VQ` are 440x4800. `TERRAINS.XML` declares `frames="15"` on both, so
each is a vertical strip of 15 frames of 440x320. The `.vq` header carries no frame count;
the split is imposed by the terrain layer declaration. Autocorrelation on the index array
confirms it independently: the dominant vertical period is 320 rows (0.107 against a 0.02
background), with harmonics at 640 and 960.

## How terrains are organised

`DATA\TERRAINS.XML` in `data.pak` is plain XML: a `<terrain>` root containing 41 `<layer>`
elements, `z="0"` through `z="40"`. The `z` attribute is the terrain index stored in map
files. Attributes seen:

| Attribute | Meaning |
|-----------|---------|
| `z` | terrain index |
| `type` | terrain class, 0–7 (see below) |
| `display` | editor label |
| `image` | path to the `.vq`, relative to the pack root |
| `minimap` | path to a `.bmp` minimap swatch, in `Minimap.pak` |
| `passable` | `0` marks impassable terrain (`Rocks 1`) |
| `passable_water` | set on deep water |
| `dark` | set on both waters |
| `frames` | animation frame count (15, waters only) |
| `transition`, `transition_terrain`, `animate_transition` | edge blending control |

Observed `type` values group the layers: 0 invalid, 1 grass/marsh, 2 ground, 3 sand,
4 water, 5 rocks, 6 roads, 7 waves. Deep and shallow water are additionally hardcoded in the
engine — a comment in the file names the constants `DEEP_WATER_IDX` and `SHALLOW_WATER_IDX`
and warns that reordering them requires an engine change.

### Seasons

Layers `z=0..20` write their paths with a `%season%` placeholder, e.g.
`terrain/%season%/grass1024.vq`. The pack contains three season directories — `SPRING`,
`AUTUMN`, `WINTER` — each holding the same 17 texture names, and the placeholder is
substituted per map. The remaining 51 - 17*3 = 4 files (`dwater`, `swater`, `waves`,
`invalid`) are season-independent and live directly under `TERRAIN\`.

On top of that, layers `z=21..32` and `z=33..40` hardcode `terrain/Winter/...` and
`terrain/Autumn/...` respectively. So winter and autumn textures are reachable two ways: via
the season substitution, and as explicitly addressable extra layers a map can mix into any
season. Autumn exposes only 8 such layers and winter 12, which is why the autumn and winter
directories are still complete 17-file sets.

### Transition masks

The 56 `.bmp` files alongside the `.vq` files in `Terrain.pak` are **not** the minimap
swatches referenced by `TERRAINS.XML` (those are in `Minimap.pak`). They are
`TERRAIN\TRANSITIONS\*.BMP`, all 64x46 and 8-bit, named after 4-bit corner patterns
(`A0001`, `D1100`, ...). They are the blend masks used where two terrain layers meet.
Their exact use is out of scope here.

## What is still unknown

- **Field naming at `0x10`–`0x18`.** The three words are 2, 4, 1 in every retail file. Their
  product is the 8-byte codebook entry size and `0x24 * 0x14 == width`, which is consistent
  with (bytes per pixel, block width, block height), but nothing in the data varies to prove
  it. A decoder that hardcodes 4x1x2 and merely *checks* these words is safe.
- **`index_size_log2` at `0x04`.** Redundant with `index_size` at `0x0C` in all shipped data.
  Which one the engine actually reads is unknown; the reference reader requires agreement.
- **Whether the format can carry an alpha or key channel.** Bit 15 is always clear and no
  reserved colour is skipped. Terrain blending is done with the separate transition masks, so
  the `.vq` itself appears to be strictly opaque, but this has not been confirmed against the
  renderer.
- **Whether non-terrain `.vq` files exist.** None ship with the retail install, so nothing is
  known about e.g. non-power-of-two `codebook_len` or `block_height > 1`.
- **The `type` attribute semantics in `TERRAINS.XML`** beyond the grouping observed above —
  in particular what gameplay behaviour, if any, is attached to each class.
