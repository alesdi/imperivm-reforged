# `imview` — the asset viewer

Builds or serves a browser front end over an export directory, so you can look at the game's
art without launching anything. It is a developer and modder tool: the point is to see what a
sprite actually does, not to present it prettily.

## Input

`imview` reads an **export directory** — a `manifest.json` and the PNGs beside it, as written
by `imsprite`, `imterrain`, `imfont` and `immask`. The manifest is the contract; see
[`src/imperivm/manifest.py`](../../src/imperivm/manifest.py) for why it exists and what it
carries that a PNG cannot.

Produce one first:

```
imsprite  export-all --game /path/to/Imperivm --out /tmp/export
imterrain --game /path/to/Imperivm export-all --out /tmp/export
imfont    --game /path/to/Imperivm export-all --out /tmp/export
immask    --game /path/to/Imperivm export-all --out /tmp/export
```

The four merge into one manifest rather than overwriting each other, so order does not
matter. A full export is 4,557 entries and about 343 MB.

## Commands

### `serve`

```
imview serve /tmp/export [--port 8765] [--host 127.0.0.1] [--open] [--filter GLOB]
```

The recommended path for a full export. Serves the viewer and the PNGs over
`http.server`, so images load lazily and nothing has to be embedded.

### `build`

```
imview build /tmp/export [-o viewer.html] [--inline] [--filter GLOB]
```

Writes a single HTML file. Without `--inline` it references the PNGs relative to the export
directory, so the file must stay next to them. With `--inline` the images are embedded as
data URIs and the file is portable on its own — but only use that with `--filter`, because
inlining a whole export produces a file no browser will enjoy. `--max-inline-bytes` warns
above 50 MB.

```
imview build /tmp/export --inline --filter 'units/rpraetorian/*' -o praetorian.html
```

## What it does

- **Browse and filter** the manifest across sprites, terrain, fonts and masks.
- **Animate sprites.** Rows are animation steps, columns are the eight facing directions, so
  the viewer plays a row while you pick a facing. Frames are positioned by their **per-frame
  bounding boxes** from the manifest, not re-centred — two frames of one animation differ by
  their box, and that difference is the motion.
- **Composite shadows** beneath the body when a matching `_shadow` entry exists. Shadows are
  separate one-bit images sharing the body's canvas, so they line up by bounding box alone.
- **Recolour team sprites.** Player-colour sprites export as indexed PNGs, so the viewer
  decodes the palette and remaps indices 0–63 the way the engine does.
- **Tile terrain** to show how a texture repeats, and play animated water as a filmstrip.

## Limitations

- **The manifest is large.** A full export's `manifest.json` is about 45 MB, because it
  carries a record for every one of the 197,432 frames. The page takes a few seconds to
  become interactive. `--filter` is the remedy when you only care about a subset.
- `build --inline` is impractical for a full export; filter first.
- Fonts and masks get simple viewers, with none of the animation machinery.
- The viewer reads an export, never the game. It cannot open a `.pak` or `rle.mmp` directly.

## Requirements

Python 3.11+ and a browser. No JavaScript dependencies, no CDN, no build step; it works
offline.
