# `imterrain` — terrain texture tool

**Format specification:** [`../formats/vq.md`](../formats/vq.md)
**Implementation:** [`src/imperivm/cli/imterrain.py`](../../src/imperivm/cli/imterrain.py)

Lists, describes and exports the 55 vector-quantised terrain textures in `Packs/Terrain.pak`
as RGB PNGs, and joins each one to the `<layer>` that declares it in `DATA\TERRAINS.XML`.

The `.vq` header does not describe a texture completely. Two things a consumer needs live
only in the XML: the terrain index `z` that map files store per tile, and `frames`, which is
the only statement anywhere that `DWATER.VQ` and `SWATER.VQ` are 15-frame filmstrips. So
this tool always reads `Packs/data.pak` alongside `Packs/Terrain.pak`, and the manifest it
writes carries `frames` and `frame_height`.

## Usage

```
imterrain [--game DIR] list
imterrain [--game DIR] info       <name>
imterrain [--game DIR] export     <name> [-o PATH] [--split-frames]
imterrain [--game DIR] export-all  -o DIR  [--split-frames] [-v]
```

`--game` is the installation directory — the one containing `Packs/`. It defaults to
`$IMPERIVM_ROOT`, then to the current directory, so running from inside the install needs no
flag. Pointing it straight at a `Packs/` directory also works.

`<name>` accepts the full member name, a season-relative path, or an unambiguous stem:
`TERRAIN\AUTUMN\GRASS1024.VQ`, `autumn/grass1024`, `swater`, `invalid`. A bare `grass1024`
is rejected with the three candidates listed, because all three seasons ship one.

### `list`

Every texture with its geometry, codebook and the terrain indices that use it.

```
$ imterrain list
member                                    size  codebook idx  frames  layers
TERRAIN\AUTUMN\GRASS1024.VQ           1024x682       256   1       1  z=3,34
TERRAIN\AUTUMN\GROUND1024.VQ           512x350      1024   2       1  z=0,33
TERRAIN\DWATER.VQ                     440x4800       256   1      15  z=13
TERRAIN\INVALID.VQ                      128x96        64   1       1  z=15
...
55 textures in Terrain.pak
```

Note `GROUND1024.VQ` at 512x350. **The number in a filename is the artist's name for the
source texture, not the header dimensions**, and heights are frequently not powers of two.
Never infer a size from a name.

Most textures are named by two `z` values, because winter and autumn art is reachable both
through the `%season%` substitution and as explicitly addressable extra layers (`z=21..40`).

### `info`

```
$ imterrain info swater
TERRAIN\SWATER.VQ: 440x4800 X1R5G5B5
  blocks       110 x 4800 of 4x1
  codebook     256 entries, 2048 bytes
  indices      528000 x 1 byte
  distinct     256 codebook entries referenced
  file         530092 bytes
  animation    15 frames of 440x320 (frames= in TERRAINS.XML)
  layer        z=12 type=4 'Shallow water' dark=1 frames=15 transition_terrain=10 animate_transition=1 transition=2
```

The `layer` lines reproduce every attribute of the matching `<layer>` element that is not
already shown, so `passable="0"` on Rocks 1 and the water transition settings are visible
here. A texture no layer references says so explicitly.

### `export`

Writes one RGB PNG.

```
$ imterrain export invalid -o /tmp/invalid.png
TERRAIN\INVALID.VQ: 128x96 -> /tmp/invalid.png

$ imterrain export swater --split-frames -o /tmp/swater
TERRAIN\SWATER.VQ: 15 frames of 440x320 -> /tmp/swater/
  /tmp/swater/00.png
  ...
```

Without `-o` the file is named after the stem and written to the current directory. With
`--split-frames` on an animated texture, `-o` names a **directory** and the frames are
written as `00.png` … `14.png`. `--split-frames` on a still texture is ignored.

### `export-all`

```
$ imterrain export-all -o /tmp/export
55 textures exported to /tmp/export
55 terrain entries in /tmp/export/manifest.json (2 animated)
```

Paths follow the pack tree in lowercase with forward slashes:
`terrain/autumn/grass1024.png`. The tool **merges** into an existing `manifest.json`,
replacing only the `terrain` section, so `imfont` and `immask` can write the same directory
in any order.

With `--split-frames`, `terrain/dwater.png` becomes `terrain/dwater/00.png` … `14.png` and
the manifest gains one `TerrainEntry` per frame — same `source`, same `frames` count, in
frame order. Without it there is one entry per texture whose `height` is the whole filmstrip
and whose `frame_height` is the period.

## Gotchas

**The quantisation unit is 4x1, not a square block.** A codebook entry is four horizontally
adjacent pixels. Decoding the same file as 2x2 blocks yields vertically smeared noise that
is obviously wrong once you look, and not obviously wrong from the arithmetic.

**Pixels are X1R5G5B5, not RGB565.** Reading them as 565 shifts every hue: greens go
yellow-brown and the water goes grey. Bit 15 is clear in all 141,568 retail codebook pixels.
Expand each 5-bit channel with `(v << 3) | (v >> 2)`.

**Zero is a legitimate colour, not a colour key.** `ROCKS1024.VQ` indexes an all-zero
codebook entry. Do not treat black as transparent.

**Frame count comes from the XML, not the file.** Nothing in a `.vq` header says how many
frames it holds. If you export water without consulting `TERRAINS.XML` you get a 440x4800
image with no way to know its period. The number is `frames="15"`, and 4800/15 = 320.

**Terrain indices 12 and 13 are hardcoded in the engine.** `TERRAINS.XML` says so in a
comment naming `DEEP_WATER_IDX` and `SHALLOW_WATER_IDX`. Any reimplementation that renumbers
layers has to change engine code with them.

**Exports are large.** The 55 textures are 17.5 million pixels; as PNG they come to about
10 MB, most of it the two 440x4800 water strips and the three 1024x800 cliff faces. They are
intermediate artefacts — keep them out of the repository.

## Measured results

Run against the retail install, Python 3.13 on macOS:

| | |
|---|---|
| textures found | **55**, all of which parse and pass `VQImage.validate()` |
| textures exported | **55** (0 failures) |
| animated | 2 — `DWATER.VQ` and `SWATER.VQ`, 15 frames of 440x320 each |
| total exported pixels | 17,481,472 |
| total PNG bytes | 10,192,351 |
| `export-all` wall time | 13.5 s |
| `export-all --split-frames` | 83 manifest entries: 53 stills + 2 x 15 frames |

The 15 exported water frames are pairwise distinct (15 distinct MD5s), which confirms the
strip is an animation rather than a tiled repeat.

**Decode oracle.** `TERRAIN\INVALID.VQ` exports as a 128x96 magenta field carrying the word
"invalid" in white serif type, legible at 1:1. It renders as legible text only under the 4x1
reading with X1R5G5B5 pixels, so it is the cheapest end-to-end check that the decoder is
right — run it first after touching anything in the path.

Spot checks on the rest: `AUTUMN\RROADS.VQ` is a coherent 512x512 Roman cobblestone road,
`WINTER\GRASS512.VQ` is snow over dead grass, `SWATER.VQ` frame 0 is a green-teal rippled
water surface. Textures look like their names.

## Limitations

- **Read-only.** There is no PNG-to-`.vq` path, which would need a vector quantiser.
- **`--split-frames` does not encode an animation.** It writes numbered stills; assembling a
  GIF or APNG is left to the consumer, which is why the period is in the manifest.
- **Season substitution is expanded, not resolved.** A layer whose `image` contains
  `%season%` is reported against all three season directories, because the choice is made
  per map and the map format is not consumed here.
- **The transition masks are out of scope.** The 56 `.bmp` files in `Terrain.pak` are the
  4-bit corner blend masks, not minimap swatches, and this tool ignores them.
- **No dithering.** The 5-bit-per-channel source is bit-replicated to 8, which is exact and
  reversible but leaves visible banding in smooth gradients. That banding is in the original
  data.
