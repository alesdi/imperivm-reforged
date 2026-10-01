# `impk` — pack archive tool

**Format specification:** [`../formats/pak.md`](../formats/pak.md)
**Implementation:** [`src/imperivm/cli/impk.py`](../../src/imperivm/cli/impk.py)

Lists, extracts, creates and verifies HMMSYS pack archives — the `.pak` files that hold
almost everything the game ships.

`impk create` fed the output of `impk extract` reproduces the original pack **byte for
byte**. See [the round-trip guarantee](#the-round-trip-guarantee) for the measured result
and the one case that needs a caveat.

## Usage

```
impk list    <pack> [PATTERN...] [--long]
impk extract <pack> [PATTERN...] [--out DIR] [-v]
impk create  <dir> <pack> [--include-hidden]
impk verify  <pack>
```

### `list`

Entries with their absolute data offset and size, in storage order.

```
$ impk list Packs/Fonts.pak
Packs/Fonts.pak: 649191 bytes
         284      96198  FONTS\COURIERNEW16.APF
       96482        238  FONTS\COURIERNEW16.INI
       ...
12 of 12 files
```

`--long` adds each entry's timestamp, then totals and a breakdown by file type. The
breakdown treats `.rle.mmp`, `.ent.xml` and `.sc.xml` as single extensions, so sprite frame
tables are not miscounted as `.mmp`.

```
$ impk list Packs/Fonts.pak --long
...
  files          12 of 12
  file data      648907 bytes (633.7 KB)
  header/tables  284 bytes (entry table 196, timestamps 48)
  timestamps     2002-10-03 11:34:12 to 2002-10-03 11:34:12

  type breakdown
    .apf              6    632.3 KB
    .ini              6      1.4 KB
```

### `extract`

Writes the stored tree to `--out` (default: a directory named after the pack). Stored names
are uppercase and `\`-separated; they are written **lowercase with `/`**, which is exactly
what `create` undoes.

```
$ impk extract Packs/Terrain.pak --out /tmp/terrain
$ impk extract Packs/Terrain.pak "terrain/autumn" --out /tmp/autumn
$ impk extract Packs/Terrain.pak "*.vq" --out /tmp/vq
```

Each extracted file's modification time is set from the pack's timestamp table — this is
load-bearing for the round trip, see below.

### `create`

Builds a pack from a directory tree, undoing `extract`.

```
$ impk create /tmp/terrain rebuilt.pak
rebuilt.pak: 111 files, 6367209 bytes
```

Dot-files are skipped by default, so a stray `.DS_Store` or `.gitignore` does not end up in
the archive; pass `--include-hidden` to pack them anyway. Symlinks are skipped.

### `verify`

Runs the specification's structural invariants and reports each one.

```
$ impk verify Packs/RandomMap.pak
Packs/RandomMap.pak: LZIS-compressed, decompressed in memory to 47192703 bytes
  [ok] magic and header
  [ok] entry table size matches the header — 90 entries in 1523 bytes
  [ok] data offsets are monotonically non-decreasing
  [ok] every entry lies inside the file
  [ok] last entry ends on the last byte of the pack — ends at 47192703, image is 47192703 bytes
  [ok] file data starts after the entry and timestamp tables — first file at 1923, tables end at 1923
  [ok] stored files are contiguous — no gaps
  [ok] names are in strict bytewise ascending order
  [ok] front coding uses the maximal shared prefix — all entries
  [ok] every timestamp decodes to a real date and time — all valid
passed: 90 entries checked
```

Exit status is 1 if any invariant fails, 2 on a malformed pack. All fourteen packs in the
retail install pass all ten checks.

### Patterns

Patterns are matched case-insensitively against the extraction-form path (`terrain/autumn/
grass1024.vq`), and either separator is accepted, so `TERRAIN\AUTUMN\*` and
`terrain/autumn/*` behave identically. A pattern containing no wildcard also matches
everything beneath it, so `terrain/autumn` selects that whole directory. With no pattern,
everything is selected.

### LZIS packs

`Packs/RandomMap.pak` is not a bare pack: it is a whole-file
[LZIS](../formats/lzis.md) stream wrapping one. Every command detects the LZIS magic,
decompresses in memory, and says so on the first line of its output. No flag is needed.

## The round-trip guarantee

> `impk create` fed the output of `impk extract` reproduces the original pack byte for byte.

**Measured result: 13 of 13 uncompressed retail packs reproduce byte-for-byte identically**
(`cmp` against the shipped file), covering 14,602 stored files.

The fourteenth pack, `RandomMap.pak`, is the LZIS-wrapped one. Its **decompressed pack image
reproduces byte-for-byte** — the archive itself is rebuilt perfectly — but `impk` cannot
re-emit the outer LZIS container, because the project has an LZIS *de*compressor and no
compressor. So the pack round-trips; the compression wrapper around it does not. Counting
pack images rather than files on disk, the rate is 14 of 14.

That is the only exception. Nothing else is approximate.

### What the writer has to get right

Three things determine the byte layout, and all three were established empirically against
the retail packs rather than assumed.

**Entry order is plain bytewise ascending** on the full uppercase `\`-separated name. This
matters because the name table is front-coded — each entry stores only its differing suffix
plus a shared-prefix length against the previous name — so a different order produces a
different table size and shifts every subsequent offset.

Bytewise order is not the only candidate; sorting path components separately is at least as
natural, and the two disagree because `\` (0x5C) sorts after the uppercase letters. They
disagree in 5 of the 14 packs, and in all 5 the retail order is the bytewise one:

| Pack | Bytewise order (matches retail) | Component-wise order (does not) |
|------|------|------|
| `Buildings.pak` | `BUILDINGS\BWALLS\GATENE\…` | `BUILDINGS\BWALLS\GATEN\…` |
| `Units.pak` | `UNITS\EGUARDIAN2\ATTACK.RLE.MMP` | `UNITS\EGUARDIAN\ATTACK.RLE.MMP` |
| `data.pak` | `DATA\AI HELPERS\GUARD AREA.VS` | `DATA\AI\AI.INI` |

Locale-aware collation is ruled out by the same corpus: `SANDSWAVE.VQ` precedes
`SANDS_MIX512.VQ` (`W` = 0x57 < `_` = 0x5F), which is the opposite of what a collation that
ignores punctuation would produce. The shared-prefix length is always the *maximal* common
prefix, in all 14,692 entries.

**Stored files are contiguous.** The specification warns that files are not contiguous and
that sizes must not be inferred from neighbouring offsets. The second half of that advice is
good practice, but measured across all 14 packs, every gap between the end of one file and
the start of the next is exactly zero — 14,678 consecutive pairs, no exceptions. There is no
alignment and no padding.

**The apparent gap is an undocumented table.** What looks like slack is a single block
between the entry table and the first stored file, and it is exactly `file_count * 4` bytes
in every pack. It is an array of `file_count` little-endian `u32` MS-DOS packed date/time
stamps, one per entry, in entry order:

```
bits   field
-----  -----------------------------
31-25  year - 1980
24-21  month, 1-12
20-16  day, 1-31
15-11  hour, 0-23
10-5   minute, 0-59
 4-0   second / 2, 0-29
```

All 14,692 stamps in the retail packs decode to valid calendar dates, spanning 1998 to 2006
and clustering in 2004-2006, consistent with the game's development. So the full layout is:

```
0x00                    header, 0x28 bytes
0x28                    entry table, name_table_bytes
0x28 + name_table_bytes timestamp table, file_count * 4    <- not in pak.md
                        file data, contiguous
```

### Why the timestamps need the filesystem

The timestamp table is the only part of a pack that cannot be recovered from the file names
and contents. `extract` therefore writes each stamp to the extracted file's modification
time, and `create` reads it back. Without that, the round trip would be impossible.

Stamps are interpreted as **UTC** rather than local time. The format carries no zone, so
either reading is defensible, but UTC makes stamp → mtime → stamp a bijection with no
daylight-saving gap in which a wall-clock time does not exist. This was verified: the
`Terrain.pak` round trip is byte-identical under `TZ` of `UTC`, `Pacific/Kiritimati`,
`America/Los_Angeles` and `Australia/Lord_Howe` (a half-hour DST zone), and also when
extracting under one zone and creating under another. The cost is cosmetic — a file browser
shows an extracted file's date shifted by the local UTC offset.

The practical consequence: **anything that discards modification times breaks the round
trip.** Copying an extracted tree with `cp` without `-p`, checking it into git, or moving it
through a zip that rewrites timestamps will change the timestamp table and therefore the
bytes. The file data will still be correct and the pack will still be valid; it will simply
not be identical to the original.

## Limitations

- **No LZIS compressor.** `impk create` always writes an uncompressed pack. `RandomMap.pak`
  can be rebuilt as a pack but not re-wrapped in its LZIS container. Whether the engine
  accepts an uncompressed `RandomMap.pak` is untested.
- **Timestamps ride on file mtimes**, with the fragility described above.
- **Names are constrained by the format**: at most 255 bytes, encodable as cp1252. `create`
  rejects anything longer or unencodable rather than truncating. Names are uppercased, so a
  tree containing both `foo.txt` and `FOO.TXT` is rejected as a collision.
- **Timestamps must fall in 1980-2107**, the MS-DOS representable range. `create` reports an
  error rather than silently clamping.
- **Extraction refuses hostile names.** Absolute paths, drive letters, UNC roots and `..`
  components abort the extraction, and the joined path is re-checked against the output
  directory so that a planted symlink cannot redirect a later entry. A malicious pack cannot
  write outside `--out`. No retail pack triggers this.
- **The engine's own tolerance is unknown.** `impk` reproduces the retail layout exactly; it
  has not been established which deviations from it the engine would still load.
