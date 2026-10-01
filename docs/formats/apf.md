# HMMSYS bitmap font (`.apf`)

**Status:** decoded and validated
**Reference reader:** [`src/imperivm/formats/apf.py`](../../src/imperivm/formats/apf.py)

Prerendered bitmap fonts. `Fonts.pak` holds six of them, each paired with an `.ini` file of
the same stem that records how the font was originally generated.

The fonts are greyscale coverage, not colour. Each pixel carries a 3-bit alpha value which
the engine multiplies against whatever ink colour the caller asks for, so one font serves
white body text, gold headings and red warnings alike.

All integers are little-endian; the ones that can carry a negative value are two's
complement. Strings are NUL-terminated and ASCII within cp1252. Offsets are byte offsets
from the start of the file unless a section says otherwise.

## The `.ini` companion

Plain text, and worth reading first — it names the source typeface, the point size, the
weight, and the exact set of code point ranges that were baked. `FONTS\TAHOMA13.INI`:

```
[main]
name=Tahoma

[formattings]
Font1

[font1]
size=13
weight=
bold=0
italic=0
charranges=[31, 127], [128, 255], [256, 383], [384, 591], [880, 1023], [1024, 1279],
           [7680, 7935], [7936, 8191], [8192, 8303], [8352, 8399]
```

(the `charranges` line is one line in the file; it is wrapped here for legibility)

Every shipped font uses the same ten ranges — Latin, Latin-1, Latin Extended A and B,
Greek, Cyrillic, Latin Extended Additional, Greek Extended, General Punctuation and
Currency Symbols — for a total of **1,633 glyphs each**. The ranges are inclusive of both
endpoints and reappear verbatim in the `.apf` range table, so the `.ini` is a cross-check
rather than a requirement for decoding.

## Header

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00     4    magic, "ABCF"
0x04     4    metrics_offset  u32
0x08     4    face_name_offset u32   the resolved GDI face, e.g. "Tahoma Bold"
0x0C     4    family_name_offset u32 the family, e.g. "Tahoma"
0x10     4    first_char      u32    lowest code point present
0x14     4    unknown, -1 in all six fonts
0x18     4    unknown,  1 in all six fonts
0x1C     4    request_offset  u32
0x20     -    string and record blob
```

The blob at `0x20` holds the two face-name strings back to back, followed by the record
`request_offset` points at:

```
size  field
----  ---------------------------------------------------------------
 4    point_size  i32   matches `size=` in the .ini
 4    italic      i32   matches `italic=`
 4    bold        i32   matches `bold=`
```

For `TAHOMA13.APF` the blob is `"Tahoma\0"` at `0x20`, `"Tahoma\0"` at `0x27`, and the
request record at `0x2E` reading `13, 0, 0`.

## Metrics

At `metrics_offset` sit fourteen `i32` fields, then the range table. The first seven are
identified; all measurements are in pixels.

```
index  field
-----  ---------------------------------------------------------------
  0    height             full cell height
  1    max_char_width     widest ink box in the font
  2    ave_char_width
  3    internal_leading
  4    external_leading
  5    ascent             baseline, measured down from the top of the cell
  6    descent
 7-13  unknown
```

`height == ascent + descent` holds for all six fonts, and `max_char_width` equals the
largest `ink_width` over the glyph table exactly, in all six. Field 7 is `-1` in five fonts
and `-4` in Courier New, which is the shape of a GDI `tmOverhang`, but nothing in the
shipped data pins it down. Fields 8–13 are small non-negative integers that grow with the
point size (`1,4,1,1,0,0` at 13pt, `2,7,2,1,0,1` at 20pt) and look like underline and
strikeout size and position; they are not needed to render.

| font | height | max_w | ave_w | ilead | elead | ascent | descent |
|------|-------:|------:|------:|------:|------:|-------:|--------:|
| `COURIERNEW16` | 18 | 11 | 7 | 2 | 0 | 14 | 4 |
| `TAHOMA13`     | 16 | 15 | 8 | 3 | 0 | 13 | 3 |
| `TAHOMA13B`    | 16 | 19 | 8 | 3 | 0 | 13 | 3 |
| `TAHOMA14B`    | 17 | 22 | 9 | 3 | 0 | 14 | 3 |
| `TAHOMA16B`    | 19 | 26 | 10 | 3 | 0 | 16 | 3 |
| `TAHOMA20B`    | 24 | 32 | 13 | 4 | 0 | 20 | 4 |

## Range table

Immediately after the fourteen metric fields:

```
size  field
----  ---------------------------------------------------------------
 4    range_count  u32
 -    range_count records of 16 bytes
```

Each record:

```
size  field
----  ---------------------------------------------------------------
 4    block_offset  u32   absolute, from the start of the file
 4    block_size    u32
 4    first_char    u32
 4    char_count    u32
```

The blocks are contiguous: the first starts immediately after the range table, each
following one starts where the previous ended, and the last ends on the final byte of the
file. `first_char` and `char_count` reproduce the `.ini` ranges exactly.

## Block

One block per range, self-contained: its own glyph table, its own kerning pairs, its own
pixel pool. Offsets inside a block are relative to the block's own start.

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00     4    kern_offset   u32   == 16 + 32 * char_count
0x04     4    kern_count    u32
0x08     4    pixel_offset  u32   == kern_offset + 12 * kern_count
0x0C     4    pixel_size    u32   == block_size - pixel_offset
0x10     -    glyph table, char_count records of 32 bytes
 ...          kerning table, kern_count records of 12 bytes
 ...          pixel pool, pixel_size bytes
```

### Glyph record

Eight `i32`, one record per code point, in ascending order from `first_char`.

```
index  field
-----  ---------------------------------------------------------------
  0    abc_a        left side bearing, may be negative
  1    ink_width    width of the stored bitmap
  2    abc_c        right side bearing, may be negative
  3    reserved     0 in every glyph of every shipped font
  4    top          first ink row, measured down from the top of the cell
  5    right        last ink column; == ink_width - 1 for every non-blank glyph
  6    bottom       last ink row, INCLUSIVE
  7    pixel_start  offset into the block's pixel pool
```

So the bitmap is `ink_width` wide by `bottom - top + 1` tall, and the advance width is
`abc_a + ink_width + abc_c` — the classic Win32 ABC triple.

A **blank glyph** (space, and the unused slots in the Extended ranges) is marked by
`bottom < top`. Blank glyphs store zero pixel bytes; this holds both ways across all 9,798
glyphs. Their `abc_a + ink_width + abc_c` is still the correct advance, so `U+0020` in
Tahoma 13 reads `0, 1, 3` for an advance of 4.

A glyph's pixel run ends where the next glyph's `pixel_start` begins; the last glyph in a
block runs to `pixel_size`.

### Kerning record

```
size  field
----  ---------------------------------------------------------------
 4    first   u32   code point
 4    second  u32   code point
 4    amount  i32   pixels to add to the pen between them
```

Only `TAHOMA13.APF` ships kerning — 184 pairs, all with `amount == -1`, and all the usual
suspects (`,` before `)`, `'` before `A`, `!` before a curly quote). The other five fonts
have `kern_count == 0` in every block. Kerning pairs live in the block of the *first*
character, but may name a `second` character from any range.

## Pixel encoding

A flat run-length stream over the glyph's ink box in row-major order. **Runs are free to
cross row boundaries** — there is no per-row padding or alignment. One byte per run:

```
bit  7 6 5   4 3 2 1 0
     alpha   run_length - 1
```

- `alpha = byte >> 5`, a 3-bit coverage value from 0 (transparent) to 7 (full ink).
- `run_length = (byte & 0x1F) + 1`, from 1 to 32 pixels.

Scale to 8-bit with `alpha * 255 // 7`.

The 3-bit split is easy to mis-read as a nibble pair, because in practice the mid-range
alphas never need a run longer than 16 and so almost every byte has an even high nibble.
The giveaway is a solid vertical stem: bold Tahoma's `l` is a single byte `0xF3`, which is
alpha 7 for 20 pixels, and only the 3/5 split decodes it.

The decoded run lengths sum to exactly `ink_width * (bottom - top + 1)` for all 9,798
glyphs across all six fonts, which is the check the reference reader performs.

## Worked example

`FONTS\TAHOMA13.APF`, the letter `o` (U+006F). It lives in range 0
(`first_char = 31, char_count = 97, block_offset = 286`), at glyph index `0x6F - 31 = 80`,
so its record is at file offset `286 + 16 + 32 * 80 = 0xB2E`:

```
0, 6, 1, 0, 6, 5, 12, 2271
```

`abc_a = 0`, `ink_width = 6`, `abc_c = 1` — advance 7. Rows 6 through 12 inclusive, so the
bitmap is 6 x 7. Block 0's header is `3120, 143, 4836, 2745`, so the pixel pool starts at
file offset `286 + 4836` and this glyph's 29 bytes begin `2271` into it:

```
00 80 e1 80 00  80 60 01 60 80  c0 03 c0  e0 03 e0  c0 03 c0  80 60 01 60 80  00 80 e1 80 00
```

Decoding — `0x00` is alpha 0 run 1, `0x80` is alpha 4 run 1, `0xE1` is alpha 7 run 2,
`0x01` is alpha 0 run 2, `0x03` is alpha 0 run 4 — gives 42 values, which wrap into the
6-wide box as:

```
. 4 7 7 4 .
4 3 . . 3 4
6 . . . . 6
7 . . . . 7
6 . . . . 6
4 3 . . 3 4
. 4 7 7 4 .
```

An antialiased lowercase `o`. Note how the third run of the second row (`0x01`, alpha 0,
length 2) sits entirely inside one row while the first row's `0xE1` spans two columns —
and how in wider glyphs the same mechanism happily runs off the end of one row into the
next.

## Rendering

To lay out a string at the top-left of a text box:

1. Start the pen at x = 0.
2. For each character after the first, add `kern(previous, current)` to the pen.
3. Blit the glyph's ink box with its top-left at `(pen + abc_a, top)`.
4. Advance the pen by `abc_a + ink_width + abc_c`.

The line box is `metrics.height` tall and the baseline sits `metrics.ascent` rows below its
top. Because `top` is already measured from the top of the cell, no per-glyph vertical
adjustment is needed.

## Validation

The reader asserts all of these, and they hold for every font in the retail install:

- `metrics.height == metrics.ascent + metrics.descent`.
- The largest `ink_width` in the glyph table equals `metrics.max_char_width`.
- `range[0].first_char` equals the header's `first_char`.
- Blocks are contiguous, start immediately after the range table, and the last one ends on
  the final byte of the file.
- Each block's `kern_offset`, `pixel_offset` and `pixel_size` are consistent with its
  `char_count` and `kern_count` and with the range's `block_size`.
- Every glyph's run lengths sum to `ink_width * (bottom - top + 1)`.
- A glyph is blank (`bottom < top`) if and only if it stores zero pixel bytes.

## What is still unknown

- Header `0x14` (`-1` in all six fonts) and `0x18` (`1` in all six). The `.ini`
  `[formattings]` section names exactly one formatting in every font, so `0x18` is
  plausibly a formatting count and `0x14` a default-character code point, but with no
  variation in the shipped data neither can be confirmed.
- Metric fields 7 through 13. Field 7 behaves like a GDI overhang; 8 through 13 scale with
  point size and are probably underline and strikeout geometry. None are needed to render.
- Glyph record field 3, zero in all 9,798 glyphs. It is positioned where a bounding-box
  left edge would go, paired with `right` at index 5, which would make the pair a
  `(left, right)` column range that the generator always left at `(0, ink_width - 1)`.
- Whether alpha 7 is meant to be fully opaque or the top of a 0..7 ramp that the engine
  scales differently. `alpha * 255 // 7` produces correct-looking antialiasing, but the
  engine's actual blend against RGB565 has not been checked.
- Whether the engine applies kerning at all. The data is present in exactly one font, so
  it may be vestigial.
