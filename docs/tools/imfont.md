# `imfont` — bitmap font tool

**Format specification:** [`../formats/apf.md`](../formats/apf.md)
**Implementation:** [`src/imperivm/cli/imfont.py`](../../src/imperivm/cli/imfont.py)

Describes, exports and rasterises the six `.apf` bitmap fonts in `Packs/Fonts.pak`. Glyph
sheets and rendered strings come out as 8-bit `GRAY` PNGs; per-glyph metrics come out as a
JSON sidecar, because a sheet without its ABC table cannot be laid out.

A glyph is **coverage, not colour**: three bits of alpha, 0 to 7, that the engine multiplies
against whatever ink colour the caller asks for. One font serves white body text, gold
headings and red warnings. So the PNG value *is* the alpha, and white-on-black is the honest
rendering; `--invert` exists only for reading on paper.

## Usage

```
imfont [--game DIR] list
imfont [--game DIR] info       <font>
imfont [--game DIR] export     <font> [-o PATH] [--json PATH] [--columns N]
                                      [--padding N] [--codes SPEC] [--invert] [--scale N]
imfont [--game DIR] render     <font> --text STR [-o PATH] [--line-gap N]
                                      [--invert] [--scale N]
imfont [--game DIR] export-all  -o DIR [--columns N] [--padding N] [-v]
```

`--game` is the installation directory containing `Packs/`, defaulting to `$IMPERIVM_ROOT`
then the current directory. `<font>` accepts `tahoma13`, `TAHOMA13.APF` or the full
`FONTS\TAHOMA13.APF`.

### `list`

```
$ imfont list
member                     face            pt  cell  asc  desc  glyphs  kern
FONTS\COURIERNEW16.APF     Courier New     16    18   14     4    1633     0
FONTS\TAHOMA13.APF         Tahoma          13    16   13     3    1633   184
FONTS\TAHOMA13B.APF        Tahoma Bold     13    16   13     3    1633     0
FONTS\TAHOMA14B.APF        Tahoma Bold     14    17   14     3    1633     0
FONTS\TAHOMA16B.APF        Tahoma Bold     16    19   16     3    1633     0
FONTS\TAHOMA20B.APF        Tahoma Bold     20    24   20     4    1633     0

6 fonts in Fonts.pak
```

`face` is the resolved GDI face, so the bold fonts report `Tahoma Bold` while their family
stays `Tahoma`. Note that only `TAHOMA13` ships kerning pairs.

### `info`

Face, family, the original generation request, cell metrics, the ten code point ranges, and
the `.ini` companion verbatim.

```
$ imfont info tahoma13
FONTS\TAHOMA13.APF: face 'Tahoma' family 'Tahoma'
  request      13pt bold=0 italic=0
  cell         16px  ascent 13  descent 3
  widths       max 15  average 8
  leading      internal 3  external 0
  glyphs       1633 in 10 ranges (13 blank), 184 kerning pairs
    U+001F..U+007F     97 glyphs      7581 bytes
    U+0080..U+00FF    128 glyphs      8363 bytes
    ...
  FONTS\TAHOMA13.INI:
    [main]
    name=Tahoma
    ...
    charranges=[31, 127], [128, 255], [256, 383], ...
```

The `.ini` `charranges` line and the `.apf` range table agree exactly in all six fonts, so
printing both makes the cross-check visible.

### `export`

A fixed-cell glyph sheet plus its metrics.

```
$ imfont export tahoma16b -o sheet.png --codes "32-126,0x391-0x3A9,0x410-0x44F" --columns 24
FONTS\TAHOMA16B.APF: 184 glyphs, 456x160 -> sheet.png
  metrics -> sheet.glyphs.json
```

- `--codes` takes comma-separated code points and ranges, decimal or `0x`-prefixed. Omit it
  for all 1,633 glyphs.
- `--columns` (default 32) and `--padding` (default 1) set the grid.
- `--scale` magnifies by nearest neighbour and `--invert` gives black ink on white. Both are
  for looking at the sheet, not for consuming it — `export-all` never applies either.

The sidecar JSON is the point of the command. It carries the font metrics, the sheet grid,
the ranges, every kerning pair, and one record per glyph:

```json
{
  "code": 65, "char": "A", "row": 1, "column": 2,
  "x": 34, "y": 21, "width": 9, "height": 9,
  "abc_a": -1, "abc_b": 9, "abc_c": 0,
  "top": 4, "bottom": 12, "advance": 8, "blank": false
}
```

`x`/`y` locate the ink in the sheet; `abc_*`, `top` and `bottom` are the original metrics.
Note `A` has `abc_a = -1` and `advance = 8` for a 9-pixel-wide ink box: the glyph overhangs
its own advance on both sides. That is normal and it is why the table is needed.

### `render`

Rasterises a string with the real ABC advances and kerning.

```
$ imfont render tahoma16b --text "Ave, Imperator!"
                     @@@@@@
   #@#                 @@
  .@# #@.  -@*    *@- .+#@@*:   ...

$ imfont render tahoma13 --text "Alea iacta est.\nAVE IMPERATOR" -o line.png --scale 3 --invert
FONTS\TAHOMA13.APF: 'Alea iacta est.\nAVE IMPERATOR' -> line.png (774x96, advance width 258px)
```

With no `-o` it prints ASCII art, which is the fastest way to check a font from a terminal.
`\n` starts a new line; `--line-gap` adds leading between lines. The reported *advance
width* is the pre-scale pixel width of the laid-out text.

### `export-all`

```
$ imfont export-all -o /tmp/export
6 fonts exported to /tmp/export
6 font entries in /tmp/export/manifest.json
```

Writes `fonts/<name>.png` and `fonts/<name>.glyphs.json` for each font, all 1,633 glyphs, at
1:1 with no inversion. It **merges** into an existing `manifest.json`, replacing only the
`fonts` section, so `imterrain` and `immask` can write the same directory in any order.

## Gotchas

**Advance is `A + B + C`, and `A` is often negative.** Between 36 and 116 glyphs per font
have a negative left side bearing. A layout that pins the first glyph's ink to x = 0 clips
it. This tool measures the ink bounds of the whole string first and shifts the origin, which
is why `render` reports a width that can exceed the sum of the advances.

**`bottom < top` marks a blank glyph**, and such a glyph stores zero bytes. It is not a
zero-height ink box, it is a deliberately inverted one; a decoder that computes
`bottom - top + 1` without checking gets a negative height. Every font has 13 blanks except
Courier New, which has 2.

**The RLE crosses row boundaries.** Each byte is `(alpha3 << 5) | (run - 1)`: three bits of
coverage, five bits of run length 1 to 32. It is one flat stream over the whole ink box, not
one stream per row, so a decoder that restarts at each row desynchronises after the first
glyph wide enough to matter.

**Kerning is per pair and mostly absent.** `TAHOMA13` has 184 pairs; the other five have
none. Apply the pair adjustment *before* the advance.

**Ink can sit outside the cell width.** `max_char_width` is the widest ink box, not the
widest advance, and the sheet's cell is sized from `max(0, A) + B` over the glyphs actually
selected. Do not assume the cell equals the advance.

**The `.ini` is a cross-check, not an input.** Everything needed to decode is in the `.apf`.
The `.ini` is worth reading because it names the source typeface and the requested weight,
which the binary only records as a resolved face string.

## Measured results

Run against the retail install, Python 3.13 on macOS:

| | |
|---|---|
| fonts found | **6**, all of which parse and pass `ApfFont.validate()` |
| fonts exported | **6** (0 failures) |
| glyphs per font | 1,633, in 10 ranges, identical across all six |
| glyphs exported | 9,798 |
| `export-all` wall time | 0.65 s |
| total sheet PNG bytes | 147,720 |

| font | blank | kern pairs | sheet at 32 columns | cell |
|------|------:|-----------:|--------------------:|-----:|
| `COURIERNEW16` | 2 | 0 | 384 x 988 | 12 x 19 |
| `TAHOMA13` | 13 | 184 | 544 x 884 | 17 x 17 |
| `TAHOMA13B` | 13 | 0 | 640 x 884 | 20 x 17 |
| `TAHOMA14B` | 13 | 0 | 736 x 936 | 23 x 18 |
| `TAHOMA16B` | 13 | 0 | 864 x 1040 | 27 x 20 |
| `TAHOMA20B` | 13 | 0 | 1088 x 1300 | 34 x 25 |

**Visual check.** `render tahoma13 --text "Alea iacta est. Veni, vidi, vici. / AVE
IMPERATOR, morituri te salutant. / WAVY Tj \"quoted\" 1234567890 - typography."` produces
three legible lines at 13pt with correct word spacing, no overlap and no clipping, including
the `AV`, `WA` and `Tj` pairs that expose a bad bearing or a missed kern. The full
`TAHOMA16B` sheet is legible across Latin, Greek and Cyrillic, which is the check that the
range table and the RLE are both being read correctly.

## Limitations

- **Read-only.** There is no PNG-to-`.apf` path.
- **No shaping, no bidi, no combining marks.** Layout is left to right, one glyph per code
  point, kern pairs applied between neighbours. Adequate for Latin, Greek and Cyrillic, which
  is all the game ships.
- **A code point outside the font is an error**, not a fallback box. The ranges are wide
  (1,633 glyphs) but do not cover CJK, and there is no `.notdef`.
- **Coverage is quantised to 8 levels on the way out.** `alpha * 255 // 7` is exact and
  reversible, but an editor that paints intermediate values cannot round-trip them back into
  a 3-bit field.
- **`--scale` is nearest-neighbour only.** That is deliberate — it keeps the pixel grid
  visible — but it is not a resampler.
- **Metric fields 7 to 13 are not exposed.** They are unidentified in the specification;
  field 7 looks like a GDI `tmOverhang` and 8 to 13 like underline and strikeout geometry.
  None is needed to render, so the sidecar omits them.
