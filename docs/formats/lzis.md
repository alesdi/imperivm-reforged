# LZIS compression

**Status:** decoded and validated
**Reference decompressor:** [`src/imperivm/formats/lzis.py`](../../src/imperivm/formats/lzis.py)

A general-purpose compressed-file wrapper. The engine exposes it as a stream class
(`CLzisFile`), one of two codecs derived from a common compressed-file base; the other is
`CLzss`, which uses the magic `LZSS` and is not used by any shipped file. A `LZIS` stream is
self-describing: it stores its uncompressed size and is randomly seekable at chunk
granularity, so the engine can open a compressed file and read from the middle of it.

The compressed payload is DEFLATE-family: LZ77 with a sliding window, Huffman-coded literals
and match lengths, Huffman-coded match distances. It uses DEFLATE's exact length and distance
tables. It is **not** DEFLATE and zlib cannot read it — the bit order is reversed, the code
length tables are transmitted differently, the literal/length alphabet is shifted by one
symbol, and the distance alphabet is twice as large.

All integers are little-endian and unsigned.

## Stream layout

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00     4    magic, "LZIS"
0x04     4    uncompressed_size  u32  total, across all chunks
0x08     4    chunk_size         u32  uncompressed bytes per chunk
0x0C     1    level              u8   encoder effort, see below
0x0D     1    padding
0x0E     -    chunk table, chunk_count u32 file offsets
 ...          chunk payloads
```

`chunk_count` is `ceil(uncompressed_size / chunk_size)`, and at least 1 — a zero-length
stream still carries one (empty) chunk entry.

The chunk table holds the absolute file offset of each chunk's payload. There is no
terminating entry: the last chunk runs to the end of the stream, and the reader supplies
`len(stream)` as the sentinel. The first entry therefore always equals `0x0E + 4 *
chunk_count`, the first byte after the table.

`level` selects how hard the *encoder* searched for matches (1, 2, or 3 — successively
larger match-finder budgets). It does not change the bitstream format and the decoder
ignores it. Every stream in the retail install is level 2. `padding` is never read; it is
zero in shipped files and holds uninitialised stack bytes in files the game writes at
runtime.

## Chunks

Each chunk decodes independently — the LZ77 window is reset at every chunk boundary and no
match may reference data from a previous chunk. This is what makes the format seekable.

A chunk covers `chunk_size` uncompressed bytes, except the last, which covers
`uncompressed_size - chunk_size * index`.

A chunk that did not compress is stored verbatim. There is no flag for this: the reader
compares the chunk's stored length against the number of uncompressed bytes it must produce,
and if the stored length is **greater than or equal to** the uncompressed length the chunk is
raw. Only the leading `uncompressed length` bytes belong to it; anything after that is
padding.

The engine sizes its work buffer at `chunk_size * 4 / 3 + 218` bytes, so a compressed chunk
never exceeds that.

## Block format

A compressed chunk is a single block: two Huffman code length tables, then a token stream.

Bits are read **most significant first** within each byte, and bytes in order. (DEFLATE reads
least significant first. This is the single most likely thing to get wrong.)

### Code length tables

The two tables are transmitted back to back, literal/length first, and each is a plain array
of fixed-width fields with no run-length coding:

```
size  field
----  ---------------------------------------------------------------
 2    field_width - 2   the width of every code length that follows
 n*w  code lengths      one per symbol, w = field_width, in symbol order
```

`w` is therefore 2 to 5 bits, and a code length is 0 to 31. The literal/length table has 286
symbols, the distance table 60. The two tables carry their own width fields and frequently
differ.

A code length of 0 means the symbol is unused. Codes are canonical: assigned in order of
increasing length, and within one length in increasing symbol order, starting at zero and
shifted left on each step up in length. The set of codes must be exactly saturated; the
engine rejects the stream otherwise. A table with no used symbols at all is legal — a chunk
containing no matches transmits an empty distance table.

### Literal/length alphabet — 286 symbols

| Symbol | Meaning |
|--------|---------|
| 0–255 | emit this literal byte |
| 256–284 | start a match of this length, then read a distance symbol |
| 285 | end of block |

Note the off-by-one against DEFLATE, where lengths start at symbol 257 and 256 is the
terminator. The bases and extra-bit counts are otherwise DEFLATE's:

| Symbol | 256 | 257 | 258 | 259 | 260 | 261 | 262 | 263 | 264 | 265 | 266 | 267 | 268 | 269 |
|--------|----|----|----|----|----|----|----|----|----|----|----|----|----|----|
| base | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 13 | 15 | 17 | 19 | 23 |
| extra bits | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 2 | 2 |

| Symbol | 270 | 271 | 272 | 273 | 274 | 275 | 276 | 277 | 278 | 279 | 280 | 281 | 282 | 283 | 284 |
|--------|----|----|----|----|----|----|----|----|----|----|----|----|----|----|----|
| base | 27 | 31 | 35 | 43 | 51 | 59 | 67 | 83 | 99 | 115 | 131 | 163 | 195 | 227 | 258 |
| extra bits | 2 | 2 | 3 | 3 | 3 | 3 | 4 | 4 | 4 | 4 | 5 | 5 | 5 | 5 | 0 |

The extra bits follow the symbol immediately and are added to the base. Match lengths run
from 3 to 258.

### Distance alphabet — 60 symbols

LZIS reuses DEFLATE's 30 distance *slots* but splits each one by parity, giving 60 symbols.
Symbol `s` means slot `s >> 1` with parity `s & 1`:

```
slot        = symbol >> 1
parity      = symbol & 1
extra_bits  = max(deflate_extra[slot] - 1, 0)
distance    = deflate_base[slot] + parity + 2 * read(extra_bits)
```

Splitting the odd and even distances into separate symbols lets the Huffman coder model them
separately and saves one extra bit per slot.

| Slot | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 |
|------|---|---|---|---|---|---|---|---|---|---|----|----|----|----|----|
| base | 1 | 2 | 3 | 4 | 5 | 7 | 9 | 13 | 17 | 25 | 33 | 49 | 65 | 97 | 129 |
| DEFLATE extra | 0 | 0 | 0 | 0 | 1 | 1 | 2 | 2 | 3 | 3 | 4 | 4 | 5 | 5 | 6 |

| Slot | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 | 24 | 25 | 26 | 27 | 28 | 29 |
|------|----|----|----|----|----|----|----|----|----|----|----|----|----|----|----|
| base | 193 | 257 | 385 | 513 | 769 | 1025 | 1537 | 2049 | 3073 | 4097 | 6145 | 8193 | 12289 | 16385 | 24577 |
| DEFLATE extra | 6 | 7 | 7 | 8 | 8 | 9 | 9 | 10 | 10 | 11 | 11 | 12 | 12 | 13 | 13 |

Slots 0–3 have no extra bits to give up, so the parity bit makes them redundant: distance 3,
for example, is expressible as slot 1 parity 1 or slot 2 parity 0. Both are valid and a
decoder must accept either.

Matches may overlap the current output position (`distance < length`), so the copy must run
one byte at a time.

## Worked example

`config.ini` — 1,067 bytes on disk, one chunk, uncompressed to 2,032 bytes of INI text.

Header: `uncompressed_size = 2032`, `chunk_size = 32768`, `level = 2`. One chunk, so the
table is a single entry at `0x0E` holding `18` — the byte right after it. The chunk payload
is the remaining 1,049 bytes and begins `80 00 00 00 02 58 01 80`.

The first byte is `0x80` = `10000000`. The leading two bits, `10` = 2, give a literal/length
field width of `2 + 2 = 4`. The 286 four-bit code lengths that follow occupy the next 1,144
bits; most are zero (only 93 of the 286 symbols are used, and the longest code is 11 bits),
which is why the payload opens with a long run of zero bytes. The distance table's own width
field reads `01` = 1, so its 60 code lengths are 3 bits each.

Tokens start at bit 1,328, exactly on byte 166. Decoding:

| Bit | Symbol | Action | Output so far |
|----:|-------:|--------|---------------|
| 1328 | 91 | literal `[` | `[` |
| 1337 | 115 | literal `s` | `[s` |
| 1342 | 121 | literal `y` | `[sy` |
| 1349 | 115 | literal `s` | `[sys` |
| 1354 | 116 | literal `t` | `[syst` |
| 1359 | 101 | literal `e` | `[syste` |
| 1363 | 109 | literal `m` | `[system` |
| 1369 | 93 | literal `]` | `[system]` |
| 1378 | 13 | literal CR | … |
| 1384 | 10 | literal LF | `[system]\r\n` |

Note the varying gaps: `e` (symbol 101) has a 4-bit code, `[` (symbol 91) an 8-bit one.

The first match arrives at output offset 25, after `[system]\r\n;WindowX = 1024`:

- Literal/length symbol **262** → base 9, 0 extra bits → length **9**.
- Distance symbol **16** → slot 8, parity 0. Slot 8's base is 17 and DEFLATE gives it 3 extra
  bits, so LZIS reads `3 - 1 = 2`, which are `00`. Distance = `17 + 0 + 2 * 0` = **17**.
- Copy 9 bytes from 17 back: `\r\n;Window`, producing `…1024\r\n;Window`.

A later token shows the parity bit in use: distance symbol **15** → slot 7, parity 1, base 13,
`2 - 1 = 1` extra bit reading 0 → distance `13 + 1 + 0` = **14**.

## Validation

The reference decompressor asserts all of these, and they hold for every LZIS stream in the
retail install:

- The magic is `LZIS` and the stream is at least 14 bytes.
- `chunk_size` is non-zero, and every chunk table entry lies between the end of the table and
  the following entry.
- Each Huffman code length table forms a complete, exactly saturated canonical code.
- No match distance reaches back before the start of its own chunk.
- No chunk overruns its expected uncompressed length, and every chunk produces exactly that
  many bytes.
- The concatenated chunks total exactly `uncompressed_size`.

Two independent end-to-end checks confirm the decoder rather than just its self-consistency:
`config.ini` decompresses to readable INI text, and `RandomMap.pak` (1,441 chunks, 47.2 MB
out) decompresses to a pack that [`pak.py`](../../src/imperivm/formats/pak.py) parses and
validates — 90 entries, correct front coding, final entry ending exactly on the last byte.

## Where the codec is used

LZIS is applied to whole files, never to individual entries inside a `.pak`. Nothing in any
retail pack is LZIS-compressed.

| Stream | On disk | Decompressed | Chunks | Contents |
|--------|--------:|-------------:|-------:|----------|
| `config.ini` | 1,067 | 2,032 | 1 | engine configuration, INI text |
| `Packs/RandomMap.pak` | 1,499,379 | 47,192,703 | 1,441 | a [HMMSYS pack](pak.md), 90 `.bmp` |
| `Packs/RandomMapSettlements.bfhp` | 119,712 | 3,895,903 | 119 | an `HPFS` container |
| `Profiles/lastsettings.usr` | 503 | 2,219 | 1 | last-played settings, written at runtime |
| `Profiles/lastconquest.usr` | 615 | 4,695 | 1 | last conquest state, written at runtime |
| embedded in 17 `.bfhp` files | 22 | 4 | 1 | four zero bytes, stored uncompressed |

The last row is a stub: every populated map, scenario, adventure and conquest container holds
a single 22-byte LZIS stream as one of its records (at offset 2560 in all of them except
`3_Great_Losses_Egypt.bfhp`, where the record lands at 4096). All 17 are byte-identical and
decode to four zero bytes via the stored-chunk path. They are the only place in the install
where an LZIS stream is nested inside another container, and the only exercise of the stored
path in shipped data.

Note that the two `.usr` files use a 128 KB chunk size while everything else uses 32 KB, so a
reader must take the chunk size from the header rather than assuming one.

## What is still unknown

- **The encoder is not implemented.** The format is fully specified above, so writing one is
  possible, but nothing here has been validated by round-tripping through the game.
- **Levels 1 and 3 are unobserved.** Every shipped stream is level 2. The three levels differ
  only in the encoder's match-search budget and produce the same bitstream grammar, so this
  is very unlikely to matter, but it has not been confirmed against real data.
- **Multi-block chunks do not exist, as far as can be told.** The decoder reads exactly one
  pair of Huffman tables per chunk and stops at the first end-of-block symbol, so a second
  block in the same chunk would be unreachable. No shipped chunk contains one.
- **The stored-chunk path is barely exercised.** The only stored chunks in the install are
  the 4-byte `.bfhp` stubs. The rule (stored when the payload length is at least the
  uncompressed length) is taken from the engine's own comparison, not from a large sample.
- **Byte `0x0D` of the header has no known meaning.** It is never read back. It is zero in
  shipped files and varies in files the game writes, which is consistent with uninitialised
  padding.
- **Which distance encoding an encoder should prefer for distances 1–5** is unspecified,
  since slots 0–3 overlap. Decoding is unambiguous either way.
- **`CLzss`** — the sibling codec with magic `LZSS` — has not been decoded. No file in the
  retail install uses it, so it may only matter for user-made or patched content.
