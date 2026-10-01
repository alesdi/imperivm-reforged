# `imfs` — block container tool

**Format specification:** [`../formats/bfhp.md`](../formats/bfhp.md)
**Implementation:** [`src/imperivm/cli/imfs.py`](../../src/imperivm/cli/imfs.py),
[`src/imperivm/formats/bfhp.py`](../../src/imperivm/formats/bfhp.py)

Lists, extracts, inspects, edits, builds and verifies HMMSYS `.bfhp` block containers — the
files that hold every scenario, adventure, campaign and saved game.

`imfs create` fed the output of `imfs extract` reproduces the original container **byte for
byte**. See [the round-trip guarantee](#the-round-trip-guarantee) for the measured result
and the one container that needs a caveat.

## `.bfhp` is a filesystem, not a map format

Everything else in this repository documents a format that holds one kind of thing: a pack
holds files, a `.vq` holds a texture, a `.rle.mmp` holds a sprite. A `.bfhp` holds a
**directory tree**. It is a small read/write virtual filesystem — a flat array of fixed-size
blocks with node blocks, an indirect block map and directory records — and the engine mounts
it and reads paths out of it exactly as if it were a folder on disk.

Nothing in the container is map-specific. A scenario, a Great Battles adventure, the
Mediterranean conquest campaign, the editor's blank templates and the adventure save slot
are all the same format with different trees inside. Two consequences worth stating plainly:

- **Saved games need no separate tooling.** `currentadv.bfhp` at the install root is the
  adventure save slot; in a fresh install it is a formatted but empty container, two 4 KB
  blocks holding a header and a zero root node. `imfs` reads and writes it like any other.
- **The interesting structure is one level down.** What distinguishes a scenario from a
  campaign is the XML and grid files inside, which is what [`info`](#info) summarises.

The engine addresses container contents through fixed virtual prefixes rather than raw
paths; the names appear verbatim in `gbr.exe`. Anyone using `imfs` to understand the engine
will want this mapping to hand:

| Prefix | Resolves to |
|--------|-------------|
| `CurrentGame/` | container root |
| `CurrentMap/` | `Maps/<current map>/` |
| `LocalGameData/` | `Local/<language>/` |
| `LocalMapData/` | `Local/<language>/Maps/<current map>/` |
| `AdvSaveGame/` | `currentadv.bfhp` |
| `ConquestTempFolder/` | scratch container used while a conquest is in progress |

So `LocalGameData/adventure.loc.xml` is `Local/Italian/adventure.loc.xml` inside whichever
container is mounted, and there is no path in the engine that names a `.bfhp` file directly.

## Usage

```
imfs list    <container> [--long]
imfs extract <container> [PATTERN...] [--out DIR] [--no-manifest]
imfs cat     <container> <path>
imfs info    <container>
imfs verify  <container>...
imfs create  <dir> <container> [--block-size N] [--manifest FILE] [--no-manifest]
imfs add     <container> <path> [FILE] [--directory]
imfs replace <container> <path> [FILE]
imfs rm      <container> <path> [-r]
```

Stored paths use `/`, but every command accepts either separator and matches
case-insensitively, because the engine's own lookup does: `Maps/1/map.xml` and
`MAPS\1\MAP.XML` name the same file. Path components that could escape a directory (`.`,
`..`, drive letters, embedded NULs) are rejected, and extraction re-checks the joined path
against the output directory so a planted symlink cannot redirect a later entry.

### `list`

The directory tree, indented, in stored order.

```
$ imfs list Scenarios/Crossroads.BFHP
Scenarios/Crossroads.BFHP: 1885 blocks of 512, 38 files, 11 directories, 913,226 bytes of payload
              Conversations/
          22  env.42
         328  game.xml
          21  itemsCustom.xml
              Local/
                Italian/
         681      adventure.loc.xml
          21      itemsCustom.loc.xml
              ...
```

`--long` prefixes each entry with its node block, the number of data blocks it occupies, and
its indirection level — which is how you see the block layout without a hex editor:

```
$ imfs list Scenarios/Crossroads.BFHP --long
    node  blocks lvl         size  name
       2       0   0               Conversations/
       4       1   0           22  env.42
       6       1   0          328  game.xml
      ...
      82     257   1      131,092  Terrain.decor.grid
     343     513   1      262,164  Terrain.height.grid
```

`lvl 0` means the node's own block holds the data block list directly, which reaches
`(block_size/4 - 2) * block_size` bytes — 64,512 with 512-byte blocks. `lvl 1` means those
words are indices of *index* blocks, each a full block of data block indices. The terrain
grids are the files that cross the threshold; 446 of the 2,075 entries in the retail
containers are level 1.

### `extract`

Writes the tree to a directory, default the container's stem.

```
$ imfs extract Scenarios/Crossroads.BFHP --out /tmp/crossroads
38 files, 913,226 bytes written to /tmp/crossroads
creation order recorded in /tmp/crossroads/.imfs-manifest.json
```

Patterns are `fnmatch` globs matched case-insensitively against the full stored path, and a
pattern with no wildcard also matches everything beneath it, so `Maps/1` selects that whole
directory:

```
$ imfs extract Scenarios/Crossroads.BFHP "Maps/1/*.grid" --out /tmp/grids
6 files, 884,856 bytes written to /tmp/grids
```

A full extraction also writes `.imfs-manifest.json` at the root, holding the container's
block size and the order in which its entries were created. That file is what makes the
round trip byte-exact — see [below](#creation-order-and-the-manifest). A partial extraction
does not write one, because a partial tree cannot rebuild the container anyway.
`--no-manifest` suppresses it.

### `cat`

One stored file to stdout, bytes unchanged.

```
$ imfs cat Scenarios/Crossroads.BFHP game.xml
	<game>
		<properties
			game_type="0"
			name="CrossRoads"
			author="Haemimont Games"
			...
```

### `info`

The header, the block accounting, and a summary of what the container actually holds. On a
scenario this is the fastest orientation available:

```
$ imfs info Scenarios/Crossroads.BFHP
Scenarios/Crossroads.BFHP
  magic          HPFS
  block size     512
  blocks         1885
  root node      1
  free head      -1
  reserved       0, 0
  unknown +0x18  8
  length         964,637 bytes = 1884 x 512 + 29

  block accounting
    header              1
    nodes              50
    index              18
    data             1816
    claimed          1885 of 1885
    unreferenced        0
    compaction   compacted, no gaps and no duplicates
    payload       913,226 bytes in 38 files
    overhead       51,411 bytes

  contents
    game         "CrossRoads" by Haemimont Games, type 0, season spring, start map 1
    maps         1
      Maps/1  "New Map", world 16384 x 16384
        Terrain.decor.grid       256 x 256 cells of 64 units, 16 bit(s)/cell
        Terrain.height.grid      512 x 512 cells of 32 units, 8 bit(s)/cell
        Terrain.light.grid       512 x 512 cells of 32 units, 8 bit(s)/cell
        Terrain.pass.grid        1024 x 1024 cells of 16 units, 1 bit(s)/cell
        Terrain.terrain.grid     256 x 256 cells of 64 units, 8 bit(s)/cell
        Terrain.trans.grid       256 x 256 cells of 64 units, 4 bit(s)/cell
    players      16 slots, 4 with a start position
    sequences    0 scripts (0 at game level, 0 in maps)
    localisation 1 language(s): Italian
```

Everything under `contents` is read out of the container's own files: the title line from
`game.xml`, the map names and world size from each `Maps/<n>/map.xml`, the grid geometry
from each `.grid` file's own `DIRG` header (and flagged `SIZE MISMATCH` if the header's
arithmetic does not match the stored length), the start positions by checking each
`player<n>.xml` for a non-zero `startx`/`starty`. A conquest campaign also reports its
territory count, and a container with recorded speech reports its `.wav` total.

The six grid layers are the map. `Terrain.pass.grid` at 1 bit per 16-unit cell is
passability, the heightmap and baked lighting are 8 bits per 32-unit cell, and terrain type,
decor and blend transitions are 64-unit cells. A 16384-unit world is 1024 × 1024 passability
cells. `imfs info` is usually the quickest way to answer "how big is this map and what is on
it" without extracting anything.

`info` exits 1 if the block accounting fails, printing `block accounting FAILED` instead of
the table.

### `verify`

Checks the specification's structural invariants across any number of containers.

```
$ imfs verify Scenarios/*.BFHP
ok   Scenarios/Balcans.BFHP  7468 blocks of 512, 38 files, every block claimed exactly once
ok   Scenarios/Crossroads.BFHP  1885 blocks of 512, 38 files, every block claimed exactly once
ok   Scenarios/Island War.BFHP  1924 blocks of 512, 38 files, every block claimed exactly once

3/3 containers verify
```

The checks are: the magic is `HPFS` and the block size is a power of two; the root node is
inside the array; every entry's kind and every node's level are legal values; the walk from
the root claims every block exactly once and leaves none unreferenced; the physical length
falls in the truncation window; each entry's declared size needs exactly the number of blocks
its node lists; no level-0 node exceeds the direct list; and no directory holds two names
that differ only in case, since the engine could not tell them apart. Failures are listed per
container, capped at ten. Exit status is 1 if any container fails.

**All 24 containers in the retail install pass.**

### `create`

Builds a container from a directory tree, undoing `extract`.

```
$ imfs create /tmp/crossroads rebuilt.bfhp
rebuilt.bfhp: 38 files, 11 directories, 964,637 bytes, 512-byte blocks
```

Block size comes from `.imfs-manifest.json` if there is one, then `--block-size`, then 512.
Symlinks and anything that is not a regular file are skipped with a warning on stderr.

### `add`, `replace`, `rm`

In-place edits. Each one reads the container, applies the change and **rewrites the whole
file compacted**, atomically via a temporary file, so a failure leaves the original in place.

```
$ imfs add    map.bfhp Notes2.xml note.txt
map.bfhp: added Notes2.xml (5 bytes)
$ imfs replace map.bfhp Notes.xml note.txt
map.bfhp: replaced Notes.xml (5 bytes)
$ imfs add    map.bfhp NewDir --directory
map.bfhp: added NewDir (directory)
$ imfs rm -r  map.bfhp Local
map.bfhp: removed 9 entrie(s) under Local
```

`add` creates missing parent directories and inserts each new entry at the position that
keeps its parent's records in case-insensitive ascending order, which is the order the game's
editor writes them in. It refuses to overwrite (`use 'imfs replace'`); `replace` refuses to
create (`use 'imfs add'`) and refuses to target a directory. `rm` needs `-r` to remove a
non-empty directory. The source file argument may be omitted or given as `-` to read stdin.

Rewriting compacted is not a shortcut — it is what the original writer does, so the result
is the same shape as a retail container rather than a fragmented one. An `add` followed by an
`rm` of the same entry reproduces the original container byte for byte.

## The round-trip guarantee

> `imfs create` fed the output of `imfs extract` reproduces the original container byte for
> byte.

**Measured result: 23 of the 23 uncompressed retail containers reproduce byte-for-byte
identically** (`cmp` against the shipped file), covering 1,747 stored files, 29 MB of terrain
grid, 64 MB of recorded speech, and containers from 8 KB to 19.7 MB. The largest of them,
`Adventures/Tutorial.BFHP`, is 38,520 blocks and exercises level-1 indirection on dozens of
files.

The twenty-fourth container is `Packs/RandomMapSettlements.bfhp`, and it is the one
exception — see [Transparent LZIS](#transparent-lzis) and
[the exception](#the-one-container-that-does-not-round-trip).

### What the writer has to get right

Reproducing the layout, rather than merely producing a valid container, needs three
behaviours of the original writer, all established against the retail data rather than
assumed. They are implemented in
[`bfhp.py`](../../src/imperivm/formats/bfhp.py) and documented here because they explain
every flag on `create`.

**Blocks come from one monotonic counter and nothing is ever freed.** The writer allocates
in the order it touches things, so the container comes out compacted with no free block map —
which is exactly what every retail container is, and what `verify` checks. `add` and `rm`
therefore rewrite rather than patch: there is no free list to patch into.

**Creation order fixes the layout, and it is not always a depth-first walk.** For each entry
the writer allocates the entry's node block, appends the record to the parent directory
(which may grow the parent), and only then writes the entry's own content — in that order.
Reversing any two of those steps shifts every subsequent block. See
[below](#creation-order-and-the-manifest).

**There is a single index block buffer, shared by every file and never cleared.** A partly
filled index block keeps whatever the previously flushed one held in its unused slots,
including across file boundaries, and a node promoted from level 0 to level 1 keeps the tail
of its old direct list in words 3 onward. Two files in the retail set only make sense under
that reading, and neither would reproduce without it. A reader must never scan a node's block
map for a zero terminator; the count comes from `size`.

### Creation order and the manifest

The order the writer created entries in is recoverable from a finished container, because the
block allocator is monotonic: a higher node block index means a later creation. That is what
`BlockFile.to_plan()` reconstructs, and it is what `extract` records in
`.imfs-manifest.json`:

```json
{ "tool": "imfs", "format": "bfhp", "container": "Crossroads.BFHP",
  "block_size": 512,
  "creation_order": ["Conversations", "env.42", "game.xml", "itemsCustom.xml",
                     "Local", "Local/Italian", "Local/Italian/adventure.loc.xml", "..."] }
```

Without it, `create` falls back to a case-insensitively sorted depth-first walk. That is
still a valid container, and for **15 of the 23** it happens to be byte-identical anyway.
The other eight need the manifest:

- **Seven need the recorded order** — the six Great Challenges adventures and the Tutorial.
  These are the containers that ship recorded speech, and their writer created every text
  file first and came back for the `.wav` files afterwards, even though the directory records
  interleave them. A depth-first walk cannot express that.
- **One needs the recorded block size** — `currentadv.bfhp`, which uses 4096-byte blocks
  where everything else uses 512. `imfs create dir out.bfhp --no-manifest --block-size 4096`
  reproduces it exactly, so this one is only about the block size.

Entries the manifest does not mention keep their sorted position at the end, so a tree that
gained files since extraction still builds.

The practical consequence: **the manifest is part of the extraction.** Deleting it, or
copying an extracted tree without its dot-files, does not break correctness but does change
the bytes for those eight containers.

### The one container that does not round-trip

`Packs/RandomMapSettlements.bfhp` fails twice over, and both failures are worth knowing.

First, it is an LZIS stream, and there is no LZIS compressor in this project, so it cannot be
re-wrapped — the same limitation `impk` has with `RandomMap.pak`. `add`, `replace` and `rm`
refuse it outright with an explanatory message rather than silently writing an uncompressed
file over it.

Second, and unlike `RandomMap.pak`, **its decompressed container image does not reproduce
either**. Two reasons:

- Its header's `reserved_b` field holds the ASCII tag `LZIS` rather than 0 — `info` prints it
  as `reserved 0, 1397316172  ("LZIS")` — and the writer here always writes 0 there. That is
  four bytes.
- Its writer's creation order is not recoverable from node indices. `Maps/1/map.obj.xml` has
  node block 13, near the front, but its 551 data blocks are 7133..7609, at the very end of
  the container: the entry was created early and its payload streamed last. Node-index order
  cannot express "create the entry now, write the content later", so the rebuild places that
  payload where the node sits and shifts everything after it.

The rebuild is a valid, fully compacted container with the same 39 entries and byte-identical
payloads for all 32 files — it verifies, and `imfs list` on it is indistinguishable except
for node numbers. It is simply not the same bytes. Counting only what a byte-for-byte claim
can honestly cover, the rate is 23 of 23.

## Things that are easy to get wrong

**Block size is 512 in most containers but 4096 in `currentadv.bfhp`.** Read it from the
header. Every derived quantity — the direct-list capacity, the pointers per index block, the
truncation window — moves with it.

**The file on disk is *shorter* than `block_size * block_count`, by up to one block.** The
writer truncates at the last byte actually used in the last block rather than padding out.
`info` shows the arithmetic:

```
  length         964,637 bytes = 1884 x 512 + 29
```

so the final block holds 29 payload bytes and 483 bytes that were never written. Across the
retail set the tail ranges from 25 to 4096 bytes. A reader must tolerate a short final block;
a writer that pads will not reproduce the original. (`currentadv.bfhp`'s tail is a full 4096
because its last block is the zero root node, written whole.)

**Containers are fully compacted, and that is checkable.** The walk from the root claims
every block exactly once and covers `0 .. block_count-1` with no gaps and no duplicates.
There is no free block map. `verify` asserts it and `info` reports it under `compaction`; a
container that fails it is either corrupt or was written by something other than the engine.

**Directory record order is a habit, not an invariant.** It is usually case-insensitive
ascending, but seven directories in the retail set — the localised `conversations/` folders,
where every `.conv.xml` precedes every `.wav` — are in insertion order instead. Build an
index; do not assume sorted.

**Empty files and empty directories are indistinguishable at the node level.** A node with
`size == 0` is entirely zero either way. Only the parent's record says which it is. No retail
container has a zero-length file, so this is untested territory.

### Transparent LZIS

`Packs/RandomMapSettlements.bfhp` carries the extension but is not a container: it is a
whole-file [LZIS](../formats/lzis.md) stream that decompresses to one. Every read command
detects the magic, decompresses in memory, and says so:

```
$ imfs info Packs/RandomMapSettlements.bfhp
Packs/RandomMapSettlements.bfhp
  storage        LZIS stream, 119,712 bytes on disk
  magic          HPFS
  block size     512
  blocks         7610
  ...
  length         3,895,903 bytes = 7609 x 512 + 95
```

No flag is needed. 119,712 bytes on disk expand to 3,895,903 — a 32:1 ratio, which is what
the random-map settlement templates being mostly terrain grid buys. It verifies like any
other container, which settles the format specification's open question about what is inside
it: 32 files, 7 directories, one map. Write commands refuse it, as described above.

## Measured results

Run against the retail install, Python 3.13 on macOS:

| | |
|---|---|
| containers found | **24** (23 plain + 1 LZIS-wrapped) |
| `imfs verify` | **24 of 24 pass**, all checks |
| entries / stored files | 2,075 / 1,779 |
| payload | 104,811,117 bytes |
| level-1 nodes | 446 |
| extract → create byte-identical | **23 of 23** uncompressed containers |
| identical without the manifest | 15 of 23 (7 need the order, 1 the block size) |
| `add` then `rm` of the same entry | byte-identical to the original |
| block sizes seen | 512 (23 containers), 4096 (`currentadv.bfhp`) |
| final-block tails | 25 to 4096 bytes, never a full block except `currentadv.bfhp` |

The round trip is the strongest available evidence that the format is understood: it is not
enough to parse the block chains, the rebuild has to allocate blocks in the same order, grow
directories at the same moments, and reproduce the stale tails of promoted nodes and of a
shared index buffer that is never cleared. Every one of those is a place where a plausible
implementation produces a valid container with different bytes.

## Limitations

- **No LZIS compressor.** `RandomMapSettlements.bfhp` can be read and its contents rebuilt as
  a plain container, but not re-wrapped. Whether the engine accepts an uncompressed
  substitute is untested.
- **`RandomMapSettlements.bfhp` does not round-trip even decompressed**, for the two reasons
  given [above](#the-one-container-that-does-not-round-trip).
- **No indirection level 2.** The reader and writer reject anything above level 1, which no
  retail container uses. With 512-byte blocks that caps a stored file at 8,257,536 bytes; the
  largest in the retail set is 1,048,596. Whether the engine supports deeper indirection, and
  what the layout would be, is unknown.
- **Edits rewrite the whole file.** `add`, `replace` and `rm` are O(container). On the 19.7 MB
  Tutorial that is noticeable. There is no incremental writer, because reproducing the
  original layout requires replaying the allocation from the start.
- **Header fields +0x0C, +0x10, +0x14 and +0x18 are written as constants** (`-1, 0, 0, 8`),
  matching every retail container. The free-list reading of `free_head = -1` is plausible but
  unconfirmed — no container in the install exercises it, and any container written by the
  in-game editor after a delete would settle it.
- **Nothing in the container is decoded beyond identification.** `info` reads the `DIRG` grid
  headers and pulls attributes out of the XML with a regular expression; the grid payloads,
  the `.vs` scripts and `env.42`'s LZIS payload are passed through untouched. See
  [`immask`](immask.md) for the passability grids.
- **The engine's own tolerance is unknown.** `imfs` reproduces the retail layout exactly; it
  has not been established which deviations from it the engine would still mount.
