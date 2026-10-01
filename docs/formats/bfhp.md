# HMMSYS block container (`.bfhp`, magic `HPFS`)

**Status:** decoded and validated
**Reference reader:** [`src/imperivm/formats/bfhp.py`](../../src/imperivm/formats/bfhp.py)

The container the player actually plays: every scenario, adventure, campaign map and
saved game is one of these. Unlike [`.pak`](pak.md), which is a read-only archive, this is
a small read/write **virtual filesystem** — a flat array of fixed-size blocks holding a
directory tree with node blocks and an indirect block map. The game mounts it and reads
paths out of it exactly as if it were a directory on disk.

The engine calls the container `CBFHP` and the filesystem inside it `CHPFS` / `CHPFSFile`.
The file extension is `.bfhp`, the magic is `HPFS`; both spellings are used below in the
sense the engine uses them.

All integers are little-endian, unsigned unless noted. Names are byte strings within
cp1252 and use `/` as the path separator (note: the opposite of `.pak`, which uses `\`).
Case is inconsistent in the shipped data and lookup must be case-insensitive.

## Layout

The file is an array of `block_count` blocks of `block_size` bytes. Block 0 is the header;
the rest of block 0 is zero padding. Every other block is one of four things: a **node
block**, an **index block**, a **data block**, or a **directory data block** (a data block
that happens to belong to a directory node).

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00     4    magic, "HPFS"
0x04     4    block_size    u32   bytes per block; power of two
0x08     4    block_count   u32   total blocks, including the header block
0x0C     4    free_head     i32   -1 in every retail container, see below
0x10     4    reserved_a    u32   0 in every retail container
0x14     4    reserved_b    u32   0 in every retail container
0x18     4    unknown       u32   8 in every retail container
0x1C     4    root_node     u32   block index of the root directory node; always 1
```

`block_size` is 512 in every map, scenario and campaign, and 4096 in `currentadv.bfhp`.
The reader must not assume 512.

### Physical length

`block_size * block_count` is up to one block **larger** than the file on disk. The writer
truncates the file at the last byte actually used in the last block rather than padding it
out. Verified for all 23 retail containers: the physical length always equals
`(block_count - 1) * block_size + n`, where `n` is exactly the number of payload bytes the
owning file has in that final block. A reader must therefore tolerate a short final block.

## Node blocks

A node describes one stored file or one directory. It occupies exactly one whole block,
read as an array of `block_size / 4` u32 words:

```
word  field
----  ---------------------------------------------------------------
 0    size        u32   payload length in bytes
 1    level       u32   indirection level, 0 or 1
 2..  block map         meaning depends on level
```

Let `needed = ceil(size / block_size)`.

- **level 0** — words `2 .. 2+needed-1` are data block indices, in order. The maximum
  direct file is `(block_size/4 - 2) * block_size` bytes; with 512-byte blocks that is
  126 blocks = 64,512 bytes.
- **level 1** — words `2 .. 2+ceil(needed / (block_size/4))-1` are indices of **index
  blocks**. Each index block is a full block of u32 data block indices. Concatenating
  them, truncated to `needed` entries, gives the data block list.

Only the first `needed` (or `ceil(needed/ptrs)`) words are meaningful. Beyond that:

- level-0 nodes always have a zero-filled tail (1,597 of 1,597 in the retail set);
- level-1 nodes always have a **non-zero, stale** tail (439 of 439), and it is a leftover
  direct block list. The unused tail of the last index block is stale in the same way.

This is consistent with a writer that builds a direct list first and, when the file grows
past the direct limit, overwrites only words 2..n with index block indices. **Never scan
for a zero terminator in a node's block map — always compute the count from `size`.**

The observed threshold matches exactly: the largest level-0 file in the retail set is
61,530 bytes and the smallest level-1 file is 64,602 bytes, straddling 64,512.

No level 2 occurs in the retail data; the largest stored file is 715,653 bytes and level 1
with 512-byte blocks reaches 8,257,536. Whether the engine supports deeper indirection is
unknown; the reader rejects any level above 1.

## Directories

A directory's payload is a packed list of variable-length records, `size` bytes long:

```
size  field
----  ---------------------------------------------------------------
 4    node       u32   block index of this entry's node block
 2    kind       u16   0 = file, 1 = directory
 2    name_len   u16
 n    name             n = name_len, no terminator
```

The list ends at `size`, or earlier at a record whose `node` is 0. Trailing bytes in the
last block are zero.

The root directory has no name; its node is `root_node` from the header (block 1). An
empty directory has `size == 0` and its node block is entirely zero.

Entry order is *usually* case-insensitive ASCII ascending, but this is a habit of the
editor rather than an invariant — seven directories in the retail set (the localised
`conversations/` folders, where all `.conv.xml` precede all `.wav`) are in insertion order
instead. Do not rely on the ordering; build an index.

## Allocation

There is no free block map. Every block in every retail container is claimed exactly once
by the walk from the root, and the claimed set is exactly `0 .. block_count-1` with no
gaps and no duplicates. The containers are written out compacted.

`free_head = -1` and `reserved_a = 0` are what a free-list head and a free-block count
would hold in a fully packed container, which is the natural reading, but with no
counter-example in the shipped data this remains **unconfirmed**.

## Worked example

`Scenarios/Crossroads.BFHP`, 964,637 bytes.

Header: `HPFS`, `block_size=512`, `block_count=1885`, `free_head=-1`, `0`, `0`, `8`,
`root_node=1`. Physical check: `1884 * 512 + 29 = 964,637` — the last block holds 29
payload bytes.

Block 1, the root node, reads `size=460`, `level=0`, `blocks=[3]`. Block 3 holds the root
directory's 460 bytes, which decode to 25 records:

| node | kind | name | bytes |
|-----:|------|------|------:|
| 2 | 1 | `Conversations` | 21 |
| 4 | 0 | `env.42` | 14 |
| 6 | 0 | `game.xml` | 16 |
| 8 | 0 | `itemsCustom.xml` | 23 |
| 10 | 1 | `Local` | 13 |
| 29 | 1 | `Maps` | 12 |
| 1846 | 0 | `Notes.xml` | 17 |
| 1848 … 1878 | 0 | `player0.xml` … `player15.xml` (16 entries) | 310 |
| 1880 | 1 | `Resources` | 17 |
| 1881 | 1 | `Sequences` | 17 |

`21+14+16+23+13+12+17+310+17+17 = 460`, matching the node's declared size exactly.

Block 2 (`Conversations`) is all zeros — an empty directory. Block 6 (`game.xml`) reads
`size=328`, `level=0`, `blocks=[7]`; block 7 holds 328 bytes of XML beginning
`\t<game>\r\n\t\t<properties\r\n\t\t\tgame_type="0"\r\n`.

A level-1 example from the same file: `Maps/1/Terrain.decor.grid`, node block 82, reads
`size=131092`, `level=1`. It needs 257 data blocks, so 3 index blocks: words 2..4 are
`210, 213, 342`. Index block 210 lists blocks 83–209 and 211 (128 entries), block 213
lists 212 and 214–340 (128 entries), block 342 contributes only its first entry, 341.
Words 5 onward of node 82 (`86, 87, 88, …`) and entries 1 onward of index block 342 are
stale and must be ignored.

## Validation

The reader asserts all of these, and they hold for every container in the retail install:

- Magic is `HPFS` and `block_size` is a power of two.
- `root_node` is inside the block array.
- Every entry's `kind` is 0 or 1, and every node's `level` is 0 or 1.
- Walking from the root claims each block exactly once and covers `0 .. block_count-1`
  with no unreferenced blocks.
- The physical length lies in `(block_count-1)*block_size < len <= block_count*block_size`.

As independent corroboration that the block chains are reconstructed correctly, every
extracted payload was checked against its own internal length field: all 323 `.wav` files
have `RIFF` size `+ 8` equal to the declared entry size, and all 168 `.grid` files satisfy
`20 + ceil(cells * bits_per_cell / 8) == size` from their own header.

## The retail containers

23 containers, 1,747 stored files, 96.3 MB of payload.

| Container | Blocks | File size | Files | Dirs | Maps |
|-----------|-------:|----------:|------:|-----:|-----:|
| `Adventures/Tutorial.BFHP` | 38520 | 19,721,818 | 173 | 12 | 1 |
| `Adventures/GreatBattles/5_Great_Battles_Britain.bfhp` | 22182 | 11,356,701 | 139 | 16 | 1 |
| `Adventures/GreatChallenges/6_Great_loses_Boudicca.BFHP` | 15987 | 8,184,922 | 120 | 12 | 1 |
| `Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp` | 14393 | 7,368,733 | 147 | 20 | 1 |
| `Conquests/mediterranean.BFHP` | 14206 | 7,273,060 | 184 | 44 | 7 |
| `Adventures/GreatBattles/4_Great_Battles_Egypt.bfhp` | 13358 | 6,838,813 | 94 | 16 | 1 |
| `Adventures/GreatBattles/1_Great_Battles_Zama.bfhp` | 10838 | 5,548,573 | 96 | 12 | 1 |
| `Adventures/GreatChallenges/2_Great_loses_Spain.BFHP` | 10678 | 5,466,714 | 93 | 12 | 1 |
| `Adventures/GreatChallenges/3_Great_Losses_Egypt.bfhp` | 9010 | 4,612,698 | 83 | 12 | 1 |
| `Adventures/GreatBattles/6_Great_Battles_Danube.bfhp` | 8776 | 4,492,829 | 91 | 16 | 1 |
| `Adventures/GreatChallenges/1_Great_Losses_Rome.BFHP` | 7936 | 4,062,810 | 92 | 12 | 1 |
| `Adventures/GreatBattles/3_Great_Battles_Alesia.bfhp` | 7830 | 4,008,477 | 88 | 16 | 1 |
| `Scenarios/Balcans.BFHP` | 7468 | 3,823,133 | 38 | 11 | 1 |
| `Adventures/GreatChallenges/5_Great_Loses_German.BFHP` | 4513 | 2,310,456 | 73 | 12 | 1 |
| `Adventures/GreatChallenges/4_Great_Loses_Gaul.BFHP` | 4222 | 2,161,242 | 73 | 12 | 1 |
| `Scenarios/Island War.BFHP` | 1924 | 984,605 | 38 | 11 | 1 |
| `Scenarios/Crossroads.BFHP` | 1885 | 964,637 | 38 | 11 | 1 |
| `Packs/randommap.BFHP` | 1769 | 905,241 | 31 | 7 | 1 |
| `Packs/emptyadv.bfhp` | 1728 | 884,249 | 15 | 7 | 1 |
| `Packs/emptyconquest.bfhp` | 1728 | 884,249 | 15 | 7 | 1 |
| `Packs/emptyscn.bfhp` | 1728 | 884,249 | 15 | 7 | 1 |
| `Packs/newmap.BFHP` | 1716 | 878,450 | 11 | 4 | 1 |
| `currentadv.bfhp` | 2 | 8,192 | 0 | 0 | 0 |

`Packs/RandomMapSettlements.bfhp` carries the `.bfhp` extension but is **not** in this
format — it is a whole-file `LZIS` stream, exactly like `RandomMap.pak`. Decompress it
first; the result is presumably a container, but that is unverified pending
[`lzis.md`](lzis.md).

`currentadv.bfhp` is a formatted but **empty** container: two 4 KB blocks, a header and a
zero root node. It is the adventure save slot (`AdvSaveGame/currentadv.bfhp` in the
engine), unwritten in a fresh install. Nothing in the format is map-specific — the same
container also holds the empty template packs and the conquest campaign — so it should be
read as a generic VFS.

The three `Packs/empty*.bfhp` templates are byte-identical in structure: same 1728 blocks,
same 15 files, same tree, same block assignments. They differ only in payload — `game.xml`
(280 vs 278 bytes) and a few kilobytes inside the terrain grids. They are the editor's
blank map for adventure / scenario / conquest.

## What a map contains

Every container has the same shape. Paths below are literal; `<n>` is a map number and
`<lang>` a language folder (`Italian` and `Spanish` in this install, plus one leftover
`TempLanguage`).

### Container root — the game

| Path | n | Content |
|------|--:|---------|
| `game.xml` | 21 | Title, author, description, `game_type`, victory condition, season, start map. The scenario/adventure header. |
| `player0.xml` … `player15.xml` | 288 | One per player slot: race, colour, AI, difficulty, start coordinates, a 128-hex-digit `relations` matrix. |
| `players.xml` | 1 | Same records wrapped in `<playersdatasection>`; only in `Packs/randommap.BFHP`. |
| `territories.xml` | 1 | Conquest campaign map: `<conquestmap>` with per-`<territory>` id, index, state, visual name, bonus and description. Only in `Conquests/mediterranean.BFHP`. |
| `itemsCustom.xml` | 21 | Custom item definitions. Empty (`<items></items>`) throughout the retail set. |
| `Notes.xml` | 21 | Designer's notes. |
| `env.42` | 17 | 22 bytes, an `LZIS` stream. Environment/lighting settings; payload opaque here, see [`lzis.md`](lzis.md). |
| `Sequences/seq<k>.vs` | 8 | Game-level trigger scripts (plain text). |
| `Sequences/sequences.xml` | 21 | Index of the above; empty in most containers. |
| `Conversations/` | 21 | Game-level conversations; empty except one `cnv0.conv.xml`. |
| `Resources/` | 21 | Always present, always empty in the retail set. |

### `Maps/<n>/` — the playable map

| Path | n | Content |
|------|--:|---------|
| `map.xml` | 28 | Map header: name, world `<size x= y=>` (8192, 16384 or 32768), explored-art bitmap, fog flags, start point. |
| `map.obj.xml` | 28 | **The object list.** Settlements, buildings and units as `<scriptobj class= player= x= y= …>`. The largest text file in a container (up to 716 KB). |
| `Terrain.terrain.grid` | 28 | Terrain type index per cell. |
| `Terrain.height.grid` | 28 | Heightmap. |
| `Terrain.light.grid` | 28 | Baked lighting. |
| `Terrain.pass.grid` | 28 | Passability bitmask. |
| `Terrain.decor.grid` | 28 | Decor/scenery placement. |
| `Terrain.trans.grid` | 28 | Terrain blend/transition index. |
| `warehouse.rle` | 28 | 2,418 bytes in every container, magic `IMGRLE` — a sprite in the [`.rle`](rle.md) frame format. Presumably the minimap/warehouse overlay art. |
| `Sequences/seq<k>.vs` | 300 | Trigger scripts, C-like plain text: `PlayMovie(...)`, `ClearDiplomacy(...)`, etc. |
| `Sequences/sequences.xml` | 28 | Index of the map's sequences. |
| `Conversations/cnv<k>.conv.xml` | 109 | Source (untranslated) conversation trees. |
| `labels.xml` | 23 | Named map locations. Empty in the multiplayer scenarios. |
| `Notes.xml` | 27 | Designer's notes for the map. |

### `Local/<lang>/` — localisation, shipped inside the map

| Path | n | Content |
|------|--:|---------|
| `adventure.loc.xml` | 23 | `<translationtable>` for the container's own strings: adventure name, author, description. |
| `itemsCustom.loc.xml` | 23 | Translation table for custom items; empty throughout. |
| `notes.xml`, `Maps/notes.xml`, `Maps/<n>/notes.xml` | 84 | Translated notes. |
| `Maps/<n>/conversations/cnv<k>.conv.xml` | 155 | Translated conversation trees. |
| `Maps/<n>/conversations/cnv<k>_phrase<p>.wav` | 323 | **Recorded voice lines**, RIFF / Microsoft ADPCM, mono 44.1 kHz. 64.4 MB — 64% of all container payload. |

### Aggregate file types

| Extension | Count | Bytes | Format |
|-----------|------:|------:|--------|
| `.xml` (plain) | 564 | 387,419 | game, map, player, notes, labels, sequences indexes |
| `.wav` | 323 | 64,365,894 | RIFF, Microsoft ADPCM |
| `.vs` | 308 | 607,613 | script source, plain text (same language as the `.vs` in `data.pak`) |
| `.conv.xml` | 265 | 227,577 | conversation trees / translation tables |
| `.loc.xml` | 46 | 88,895 | translation tables |
| `.obj.xml` | 28 | 5,978,825 | map object list |
| `.grid` | 168 | 29,256,992 | `DIRG` cell grid, see below |
| `.rle` | 28 | 67,704 | `IMGRLE`, see [`rle.md`](rle.md) |
| `.42` | 17 | 374 | `LZIS` stream, see [`lzis.md`](lzis.md) |

### `DIRG` grids — identification only

Not decoded here, but the header is unambiguous from size arithmetic and is worth
recording because it fixes the map's cell dimensions:

```
offset  size  field
------  ----  ---------------------------------------------------------
0x00     4    magic, "DIRG"  ("GRID" as a little-endian FourCC)
0x04     4    cell_size       u32  world units per cell
0x08     4    bits_per_cell   u32
0x0C     4    world_width     u32  matches <size x> in map.xml
0x10     4    world_height    u32
0x14     -    packed cells, row-major
```

`20 + ceil((world_width/cell_size) * (world_height/cell_size) * bits_per_cell / 8)` equals
the stored size for all 168 grids. The per-layer parameters are fixed across the whole
retail set:

| Layer | cell_size | bits_per_cell | Cells on a 16384 map |
|-------|----------:|--------------:|---------------------:|
| `Terrain.pass.grid` | 16 | 1 | 1024 × 1024 |
| `Terrain.height.grid` | 32 | 8 | 512 × 512 |
| `Terrain.light.grid` | 32 | 8 | 512 × 512 |
| `Terrain.terrain.grid` | 64 | 8 (4 in the blank templates) | 256 × 256 |
| `Terrain.decor.grid` | 64 | 16 | 256 × 256 |
| `Terrain.trans.grid` | 64 | 4 | 256 × 256 |

## How the engine mounts a container

The engine addresses container contents through fixed virtual prefixes rather than raw
paths; the names appear verbatim in `gbr.exe`:

| Prefix | Resolves to |
|--------|-------------|
| `CurrentGame/` | container root |
| `CurrentMap/` | `Maps/<current map>/` |
| `CurMap/groups/` | `Maps/<current map>/groups/` (editor only; absent from retail data) |
| `LocalGameData/` | `Local/<language>/` |
| `LocalMapData/` | `Local/<language>/Maps/<current map>/` |
| `AdvSaveGame/` | `currentadv.bfhp` |
| `ConquestTempFolder/` | scratch container used while a conquest is in progress |

The editor creates a new map by copying `packs/emptyscn`, `packs/emptyadv` or
`packs/emptyconquest` and rewriting the contents in place — which is why the container is
a read/write filesystem rather than an archive.

## What is still unknown

- **`unknown` (header +0x18) = 8.** Constant in all 23 containers. It equals the directory
  record header size, which is suggestive but unproven; a format version or a node-header
  word count fit equally well.
- **`free_head` (+0x0C) = -1 and `reserved_a`/`reserved_b` (+0x10, +0x14) = 0.** The
  free-list reading is plausible because every container is fully packed, but no container
  in the install exercises it. A container written by the in-game editor after a delete
  would settle this.
- **Indirection level 2.** Never occurs. Whether the engine supports it, and what the
  layout would be, is unknown. Files above ~8.2 MB with 512-byte blocks would need it.
- **Whether a node stores anything besides `size` and `level`.** No timestamps, attributes
  or name back-pointers were observed; words 2 onward are always block indices. A file of
  size 0 would produce a node indistinguishable from an empty directory, and no such file
  exists in the retail set to check against.
- **`Resources/`** is present and empty in every container. What the editor would put
  there is unknown.
- **`env.42`.** The name's `42` is constant across all 17 containers that have it, so it
  is presumably a fixed environment id rather than a counter. The 22-byte `LZIS` payload
  is not decompressed here.
- **`Packs/RandomMapSettlements.bfhp`** is an `LZIS` stream whose decompressed content has
  not been checked against this format.
