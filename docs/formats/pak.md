# HMMSYS PackFile (`.pak`)

**Status:** decoded and validated
**Reference reader:** [`src/imperivm/formats/pak.py`](../../src/imperivm/formats/pak.py)

The top-level archive format. Everything the game ships lives in one of these, except the
bulk sprite pixel store (`rle.mmp`), the music (`.ogg`), and the map and campaign containers
(`.bfhp`).

All integers are little-endian and unsigned. Names are byte strings; the shipped data is
ASCII within cp1252, uses `\` as the path separator, and is conventionally uppercase.

## Layout

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00    16    magic, "HMMSYS PackFile\n"
0x10     1    0x1A
0x11    15    zero padding
0x20     4    file_count       u32
0x24     4    name_table_bytes u32
0x28     -    entry table, file_count entries
 ...     -    timestamp table, file_count u32
 ...          file data, contiguous
```

The entry table starts immediately at `0x28` and occupies exactly `name_table_bytes` bytes.
The timestamp table follows it, and file data follows that.

## Entry table

Entries are variable length. Names are stored with front coding: each entry repeats a prefix
of the *previous* entry's full name and supplies only the differing suffix. Since the table
is sorted by path, this compresses shared directory prefixes well.

The sort is **bytewise ascending on the full uppercase name**, with `\` as the separator.
This is worth stating because the obvious alternative — sorting component by component — is
equally natural and gives a different answer, since `\` (0x5C) sorts after the digits and
before the uppercase letters. The two orders disagree in 5 of the 14 packs, and the retail
order is bytewise in all 5: `BUILDINGS\BWALLS\GATENE\…` precedes `BUILDINGS\BWALLS\GATEN\…`,
and `DATA\AI HELPERS\…` precedes `DATA\AI\…`. Locale-aware collation is separately ruled out:
`SANDSWAVE.VQ` precedes `SANDS_MIX512.VQ`.

Shared prefix lengths are always the maximal common prefix with the previous name, in all
14,692 entries.

```
size  field
----  ---------------------------------------------------------------
 1    total_name_len     u8   full length of this entry's name
 1    shared_prefix_len  u8   bytes to reuse from the previous name
 n    name_suffix             n = total_name_len - shared_prefix_len
 4    data_offset        u32  absolute, from start of file
 4    data_size          u32
```

Reconstruction is `name = previous_name[:shared_prefix_len] + name_suffix`. The first entry
always has `shared_prefix_len == 0`.

### Worked example

The first three entries of `Fonts.pak`:

| shared | suffix | reconstructed name |
|-------:|--------|--------------------|
| 0 | `FONTS\COURIERNEW16.APF` | `FONTS\COURIERNEW16.APF` |
| 19 | `INI` | `FONTS\COURIERNEW16.INI` |
| 6 | `TAHOMA13.APF` | `FONTS\TAHOMA13.APF` |

## Timestamp table

Immediately after the entry table, before the first file, sits an array of `file_count`
little-endian u32 values, one per entry in entry order. Each is an MS-DOS packed date and
time:

```
bits  field
----  ------------------------------
0-4   seconds / 2
5-10  minute
11-15 hour
16-20 day of month
21-24 month
25-31 year - 1980
```

All 14,692 stamps in the retail packs decode to valid calendar dates, spanning 1998 to 2006
and clustering in 2004 to 2006. Zero invalid decodes.

This table is the only part of a pack that cannot be reconstructed from file names and
contents, so any tool that intends to rebuild an archive faithfully has to carry it. `impk`
parks each stamp in the extracted file's modification time and reads it back when creating.

Interpret the stamps as **UTC**, not local time. The DOS convention is local time, but that
makes a rebuild non-deterministic across time zones and undefined in a daylight-saving
spring-forward gap, where the encoded wall-clock time does not exist. Treating them as UTC
makes stamp to mtime to stamp a bijection. The only cost is cosmetic: a file browser shows
extracted files shifted by the local UTC offset.

## Validation

The reader asserts all of these, and they hold for every pack in the retail install:

- The parse cursor lands exactly on `0x28 + name_table_bytes` after `file_count` entries.
- `data_offset` is monotonically non-decreasing across entries.
- Every `data_offset + data_size` is within the file.
- The final entry ends exactly on the last byte of the file.
- The first file begins exactly `file_count * 4` bytes after the entry table.
- Stored files are **contiguous**: every file begins where the previous one ended.

An earlier revision of this document claimed the opposite — that files were separated by
gaps. That was wrong. The apparent slack was the timestamp table above, which sits between
the entry table and the first file and had not yet been identified. Measured across all 14
pack images, all 14,678 consecutive pairs abut exactly, with no alignment and no padding.

The correction was found by building a writer: `impk create` over the output of
`impk extract` reproduces all 13 shipped packs byte for byte, which is a much harder test to
satisfy than reading, and it is what forced the layout to be understood exactly.

## Contents of the retail packs

Verified against the retail install. 14,602 files total.

| Pack | Files | Size | Principal contents |
|------|------:|-----:|--------------------|
| `UI.pak` | 4,141 | 43.1 MB | interface bitmaps, cursors, help thumbnails |
| `Minimap.pak` | 2,219 | 11.0 MB | minimap tiles |
| `Buildings.pak` | 1,899 | 3.2 MB | building entities, sprites, passability |
| `data.pak` | 1,797 | 2.4 MB | class graph, AI scripts, UI layout, balance |
| `Units.pak` | 1,755 | 20.0 MB | unit entities and sprite frame tables |
| `MapObjects.pak` | 1,534 | 1.8 MB | decor and scenery entities |
| `local/italian.pak` | 399 | 19.4 MB | localised speech and text |
| `Sounds.pak` | 314 | 136.5 MB | effects and ambience |
| `Outlines.pak` | 216 | 1.5 MB | selection outlines |
| `Visuals.pak` | 201 | 0.5 MB | effect entities |
| `Terrain.pak` | 111 | 6.4 MB | terrain textures (`.vq`) and bitmaps |
| `Fonts.pak` | 12 | 0.6 MB | bitmap fonts |
| `AdditionalArt.pak` | 4 | 0.0 MB | miscellaneous art |

`RandomMap.pak` is **not** in this format. It is LZIS-compressed as a whole file and must be
decompressed before parsing; see [`lzis.md`](lzis.md).

## File types inside the packs

| Extension | Count | Format |
|-----------|------:|--------|
| `.bmp` | 6,545 | Windows bitmap |
| `.rle.mmp` | 3,898 | sprite frame table, see [`rle.md`](rle.md) |
| `.xml` | 1,076 | class definitions (`.sc.xml`) and others |
| `.ent.xml` | 889 | entity definitions |
| `.wav` | 702 | RIFF audio |
| `.vs` | 577 | script source, plain text |
| `.pass` | 546 | passability mask, see [`pass.md`](pass.md) |
| `.ini` | 150 | UI layout and balance constants |
| `.vq` | 55 | terrain texture, see [`vq.md`](vq.md) |
| `.apf` | 6 | bitmap font, see [`apf.md`](apf.md) |
