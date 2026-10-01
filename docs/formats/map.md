# Map and scenario contents

**Status:** decoded and validated; two `<scriptobj flags>` bits and the transition layer remain unexplained
**Reference reader:** [`src/imperivm/formats/map.py`](../../src/imperivm/formats/map.py)

A playable map is not a file. It is a directory tree inside a [`.bfhp`](bfhp.md) container,
made of XML documents and six binary [`GRID`](pass.md) layers. [`bfhp.md`](bfhp.md) explains
how to get the bytes out; this document says what they mean.

Everything here is **world space**. World coordinates are unsigned, measured from the
top-left corner of the map, and a map is always a square of 8192, 16384 or 32768 world
units. How that space reaches the screen is [`../engine/projection.md`](../engine/projection.md)
and is deliberately not discussed here.

Documents are byte strings within cp1252, use `\n` escaped as the two characters `\` and
`n` inside attribute values, and are indented with tabs. Every one of them is a flat
attribute bag: **there is no element text anywhere in the format** — even multi-paragraph
mission briefings and inline script bodies are attribute values. Booleans are `"0"`/`"1"`,
except where noted.

## What a container holds

Three container flavours share one layout, distinguished only by `game.xml`'s `game_type`.

```
game.xml                    the whole container: title, author, victory, season
player0.xml .. player15.xml sixteen player slots, one document each
itemsCustom.xml             scenario-local inventory items
Notes.xml                   journal entries scripts can award
env.42                      the persistent script variable store (empty as shipped)
Sequences/sequences.xml     container-level trigger scripts
Sequences/seq<k>.vs         their sources
territories.xml             conquests only: the campaign map
Resources/                  always empty in the retail data
Maps/<n>/                   one playable map, see below
Local/<lang>/               translation tables and recorded speech
```

and under each `Maps/<n>/`:

```
map.xml                     world size, camera bookmark, fog switches
map.obj.xml                 every object placed in the world
Terrain.pass.grid           six terrain layers
Terrain.height.grid
Terrain.light.grid
Terrain.terrain.grid
Terrain.decor.grid
Terrain.trans.grid
warehouse.rle               a stock sprite table, identical in all 29 maps
labels.xml                  place names pinned to world positions
Notes.xml                   journal entries scoped to this map
Sequences/sequences.xml     trigger scripts, and seq<k>.vs beside it
Conversations/cnv<k>.conv.xml   scripted dialogue
```

The map numbers are **not contiguous**. Each shipped adventure holds exactly one map, but
under a directory numbered anywhere from 1 to 7 — `Great_Battles_Zama` keeps its only map
in `Maps/6` — and the conquest holds seven, numbered 3, 4 and 6 to 10. Enumerate the
directories; do not count.

## `game.xml`

One `<game>` with one `<properties>` and nothing else.

```xml
<game>
	<properties
		game_type="1" name="Tutorial" author="Haemimont Games"
		description="Learn the basics of the game."
		last_edited_map="1" start_map="1"
		victory_condition="0" victory_threshold="0"
		single_only="0" start_player="0"
		season="spring" user_interface="1"/>
</game>
```

| Attribute | Required | Domain | Meaning |
|---|---|---|---|
| `game_type` | yes | 0, 1, 2 | 0 scenario (6 files), 1 adventure (14), 2 conquest (2) |
| `name` | yes | free text | title shown in the map browser |
| `author` | yes | free text | 15 of 22 say `Haemimont Games` |
| `description` | yes | free text | the briefing; embeds `\n` |
| `last_edited_map` | yes | 1..7 | editor bookmark, no run-time effect |
| `start_map` | yes | a `Maps/<n>` number | the map play begins on |
| `victory_condition` | yes | see below | victory rule |
| `victory_threshold` | yes | `0` or empty | operand of the rule; never set in the retail data |
| `single_only` | yes | `0` | `1` never occurs |
| `start_player` | yes | 0, 1 | the player slot the human takes |
| `season` | no | `spring` | absent in the four blank templates; art selection, see `entity_<season>` in [`sc-xml.md`](sc-xml.md) |
| `user_interface` | no | 0, 1, 2, 4, 6, 7 | which command bar to show; correlates with the human player's race |

`victory_condition` is **not** an integer despite looking like one. Nineteen files carry
`"0"`; the three hand-made scenarios carry the string `"1 Elimination"` — a rule index and
its display name in one field, separated by a space. Parse it as a string and split on the
first space if you need the index.

## `player<i>.xml`

Sixteen documents per map, `player0.xml` through `player15.xml`, each a bare `<playerdata>`
root with no children. Nineteen containers carry a set, for 304 documents in all; the four
blank templates (`emptyadv`, `emptyconquest`, `emptyscn`, `newmap`) and the empty
`currentadv.bfhp` carry none. `id` is a permutation of 0..15 in every one of the nineteen.

One of the nineteen is easy to miss: `Packs/RandomMapSettlements.bfhp` is an
[LZIS](lzis.md) stream wrapping the container rather than a bare `HPFS`, so a scan that
sniffs the magic without decompressing first finds eighteen sets and undercounts everything
below by sixteen documents.

```xml
<playerdata
	id="0" name="Player 1" race="Mutable" AllowedRaces="All"
	control="Both" AI="" difficulty="0"
	startx="2635" starty="2047" color="31810"
	allied="0" bonus="-1"
	relations="0000003500000000…0000"/>
```

| Attribute | Required | Domain | Meaning |
|---|---|---|---|
| `id` | yes | 0..15 | must equal the filename's index |
| `name` | yes | free text | display name; defaults to `Player <id+1>` |
| `race` | yes | `Random` (136), `Carthage` (26), `Gaul` (26), `Germany` (23), `ImperialRome` (20), `Mutable` (20), `Egypt` (16), `Select` (16), `RepublicanRome` (11), `Iberia` (8), `Britain` (2) | eight races that match the `race` property in the class graph, plus three meta values (`Random`, `Mutable`, `Select`) |
| `AllowedRaces` | no | `All` | absent in two containers; restricts the picker |
| `control` | yes | `Both`, `Computer`, `Disabled` | `Disabled` means the slot is unused |
| `AI` | yes | empty, `DEFENSIVE`, `CHAOTIC` | AI personality; empty in 301 of 304 |
| `difficulty` | no | 0, 2, 3 | absent in `randommap.BFHP` |
| `startx`, `starty` | yes | world units | initial camera, 0 for 284 of 304 slots |
| `color` | yes | 0..32767 | player colour, **RGB555**: `R = c >> 10 & 31`, `G = c >> 5 & 31`, `B = c & 31` |
| `allied` | yes | 0, 1 | shared victory; `players.xml` calls the same field `sharedvictory` |
| `bonus` | no | `-1` | never set |
| `relations` | yes | 128 hex digits | the diplomacy matrix, below |

The eleven `race` values above are the complete vocabulary of all 304 documents, counted by
scanning every `<playerdata>` the install contains. `German` does **not** occur here — the
class graph uses that spelling, `player<i>.xml` does not — and `RepublicanRome` does, so the
two Romes are distinguished in the map data exactly as [`sc-xml.md`](sc-xml.md) describes.

`Random`, `Mutable` and `Select` are match-setup markers rather than races: resolving them
needs the RNG and the lobby, so a loader keeps them as written and leaves them for match
setup. `GetPlayerRace` on an unresolved slot answers -1 (see below).

### The relations matrix

`relations` is the diplomacy state: **sixteen 32-bit words, one per player slot, in slot
order**, each written as eight hexadecimal digits most significant first, for 128 digits in
all. Word `j` of player `i`'s document is player `i`'s relation **to** player `j`.

The engine keeps it as a 64-byte array at offset `0x24` of its per-player record. The
serialiser at `gbr.exe` 0x00564843 passes exactly that address and a length of `0x40` under
the attribute name `relations`, and the five `Dipl*` getters index it as
`[player_record + other_id*4 + 0x24]` — the same sixteen dwords, read as dwords.

Only the low byte of each word is ever non-zero: all 320 non-zero bytes across the install
sit there, so the useful value for player `j` is at hex offset `8*j + 6`. Nothing is known
to live in the upper 24 bits.

#### What the bits mean

**This is settled.** The assignment below is read directly out of `gbr.exe`, from the
getters and the setters independently, and both agree.

| Bit | Mask | Relation | Read by | Written by |
|---:|---|---|---|---|
| 0 | `0x01` | ceasefire — and alliance, which is the same bit | `DiplGetCeaseFire` (0x00564990) `and al, 1`; `DiplAreAllied` (0x00564c10) tests it in **both** directions | `_PlayersAlly` (0x006a4a90) `or 1`; `_PlayersMakeEnemies` (0x006a4c90) `and ~1` |
| 1 | `0x02` | — | nothing | nothing |
| 2 | `0x04` | share support | `DiplGetShareSupport` (0x00564a30) `shr eax, 2; and al, 1` | `_PlayersShareSupport` (0x006a4c10) `or 4` |
| 3 | `0x08` | — | nothing | nothing |
| 4 | `0x10` | share view | `DiplGetShareView` (0x00564ad0) `shr eax, 4` | `_PlayersShareView` (0x006a4b90) `or 0x10` |
| 5 | `0x20` | share control | `DiplGetShareControl` (0x00564b70) `shr eax, 5` | `_PlayersShareControl` (0x006a4b10) `or 0x20` |
| 6..31 | | — | nothing | nothing |

Five named relations over four bits, because **`allied` and `ceasefire` are not two
relations**. `_PlayersAlly` sets bit 0 and `_PlayersMakeEnemies` clears it; `DiplGetCeaseFire`
reads it one way round and `DiplAreAllied` reads it both ways round. There is no separate
alliance bit anywhere in the record.

Bits 1 and 3 are the ones that would have to carry meaning under the competing reading of
this field — three adjacent two-bit fields, which fits the four observed byte values just as
well as five flags do, because the set bits are 0, 2, 4, 5 and the gaps are exactly 1 and 3.
The disassembly rules it out: every one of the ten functions above tests or sets a single
bit with a single mask, and no code path touches bit 1 or bit 3.

`SetRelation` (0x005652f0) shows what each bit is *for*, because it maintains the five
per-player masks the serialiser names — `enemyflags`, `viewflags`, `seeflags`,
`supportflags`, `controlflags` — as it writes the word:

- bit 0 **clear** sets the other player's bit in this player's `enemyflags`, and bit 0 set
  clears it. `enemyflags` is the exact complement of this one bit, which is why hostility is
  one-directional and why a zero word means war.
- bit 4 set adds the other player to this player's `viewflags` *and* adds this player to the
  other's `seeflags` — the two halves of shared vision.
- bit 2 set adds the other player to `supportflags`; bit 5 to `controlflags`.

#### What the retail data contains

Counted over every `<playerdata>` in the install — 304 `player<i>.xml` documents in 19 map
directories across 19 containers, 4,864 relation words:

| Value | Occurrences | Where |
|---|---:|---|
| `0x00` | 4,544 | everything not listed below |
| `0x35` | 304 | the diagonal, in every row of every map, and **only** the diagonal |
| `0x11` | 9 | ceasefire + shared view |
| `0x15` | 6 | ceasefire + shared view + shared support; the pairs an editor marks `allied` |
| `0x01` | 1 | `3_Great_Losses_Egypt.bfhp`, player 2 toward player 3 |

Decoded under the table above the four values nest exactly, which is the check that the
assignment is not accidental: `0x35` grants everything, `0x15` withholds control, `0x11`
withholds support as well, `0x01` is the bare ceasefire.

Only **three** of the 19 map sets set anything off the diagonal at all:

| Container | Off-diagonal entries |
|---|---|
| `3_Great_Losses_Egypt.bfhp` | `[0][1]=0x15`, `[1][0]=0x15`, `[0][2]=0x11`, `[2][0]=0x11`, `[1][2]=0x11`, `[2][1]=0x11`, `[0][5]=0x11`, `[5][0]=0x11`, `[2][3]=0x01`, `[3][2]=0x11`, `[3][5]=0x11` |
| `Tutorial.BFHP` | `[0][1]=0x15`, `[1][0]=0x15`, `[0][3]=0x11` |
| `mediterranean.BFHP` | `[1][2]=0x15`, `[2][1]=0x15` |

The other sixteen leave the whole off-diagonal at zero, which is to say every player is
hostile to every other by default. That is the reading the engine's `enemyflags` requires:
peace is what a map declares, war is what it does not.

#### The matrix is not symmetric

Three pairs disagree with their transpose, and a loader must store the full 16×16 rather
than folding it:

| Container | Pair | `[i][j]` | `[j][i]` |
|---|---|---|---|
| `3_Great_Losses_Egypt.bfhp` | (2, 3) | `0x01` | `0x11` |
| `3_Great_Losses_Egypt.bfhp` | (3, 5) | `0x11` | `0x00` |
| `Tutorial.BFHP` | (0, 3) | `0x11` | `0x00` |

The first is an asymmetry in the sharing bits only: both sides set bit 0, so the pair is at
peace, but player 3 shares vision with player 2 and player 2 does not reciprocate.

The other two are one-sided truces, and they are why hostility has to be evaluated
one-directionally. Player 3 grants player 5 a ceasefire; player 5 grants nothing back. Under
a mutual rule ("enemies when neither grants the other a ceasefire") neither would be the
other's enemy and the map would play wrong. The engine asks one row: `Obj::IsEnemy`
(0x005aa480) tests bit 0 of the **receiver's owner's** row at the parameter's owner's column,
and `Settlement::IsEnemy` (0x00425250) tests bit 0 of the row of the player named in its
**integer argument**. Neither consults the other direction.

`Settlement::IsAlly` (0x004251e0) is the exact complement of `Settlement::IsEnemy`, the same
row, the same bit, the opposite branch.

#### The `allied` attribute is not the relation

`playerdata/@allied` is `1` on three documents in the whole install: player 1 of
`3_Great_Losses_Egypt.bfhp`, and players 1 and 2 of `mediterranean.BFHP`. Every one of them
is in a mutual `0x15` pair — but so is player 0 of Egypt, which has `allied="0"`, and so are
players 0 and 1 of `Tutorial.BFHP`, where no document sets it at all.

So it correlates with `0x15` and does not determine it. It is a per-player editor marker
(`players.xml` spells the same field `sharedvictory`); the matrix is the source of truth.

#### Player numbers on the wire are 1..16

Every player number a `.vs` script passes or receives is `id + 1`, while `playerdata/@id`
and every index into the matrix is 0-based. This is the single easiest thing to get wrong
about this file:

- `Obj::player` (0x005ab290) reads the owner record's `ID` and returns `ID + 1`; an unowned
  object yields `-1`.
- `Settlement::IsOwn` (0x004252c0), `Outposts` (0x0042d770), `Strongholds` (0x0042d2f0) and
  `MilUnits` (0x00421ee0) all decrement their argument before using it.
- Five error strings say so: *"Function DiplGetCeaseFire: Player number should be between 1
  and 16"*, and the same for `DiplAreAllied`, `DiplShareSupport`, `EnemyObjs` and
  `ControllableObjs`.
- The scripts agree without the executable. `STONEHENGE_WISDOM.VS` walks every player as
  `for (k = 1; k <= 16; k += 1) GetPlayerUnits(k)`, and `ESH_MARKET.VS` reads the two engine
  neutrals as `Outposts(15) + Outposts(16)` with the comment `VX_PLAYER_NEUTRAL +
  VX_PLAYER_RESCUE` — slots 14 and 15.

`Outposts(0)` is special and deliberate: the implementation branches on the sign of the
decremented argument and counts every player's outposts. `Strongholds(0)` and `MilUnits(0)`
have no such branch and read out of bounds in the original, so they are not a way to ask the
same question.

#### The script surface over this file

| Entry point | Reads |
|---|---|
| `DiplGetCeaseFire(p1, p2)` | bit 0 of `relations[p1-1][p2-1]` |
| `DiplAreAllied(p1, p2)` | bit 0 of both `[p1-1][p2-1]` and `[p2-1][p1-1]` |
| `DiplGetShareSupport(p1, p2)` | bit 2 of `relations[p1-1][p2-1]` |
| `DiplGetShareView(p1, p2)` | bit 4 |
| `DiplGetShareControl(p1, p2)` | bit 5 |
| `obj.IsEnemy(other)` | bit 0 of `relations[obj.owner][other.owner]`, negated |
| `set.IsEnemy(p)` / `set.IsAlly(p)` | bit 0 of `relations[p-1][set.owner]`, negated / not |
| `set.IsOwn(p)` | `set.owner == p - 1`; no relation involved |
| `EnemyObjs(p, class)` | a `CVXPlayerFlagsQuery` with `type = 2` |
| `FriendlyObjs(p, class)` | the same query with `type = 1` |
| `ControllableObjs(p, class)` | the same query with `type = 3` |

The three `type` values are read off the three constructors, which push 2, 1 and 3
respectively into the shared query factory at 0x004fe990. [`vs-host-api.md`](vs-host-api.md)
records the field's observed domain as {1, 2} because those are the only values the nine
desync dumps contain; 3 is real and simply never instantiated in them.

The five setters — `_PlayersMakeEnemies`, `_PlayersAlly`, `_PlayersShareView`,
`_PlayersShareControl`, `_PlayersShareSupport` — all take `(int p1, int p2)` and all write
**both** rows, so a script can only create a symmetric relation. The asymmetries in the three
maps above were authored in the editor, not produced at run time.

#### Race numbering

`GetPlayerRace` and `obj.race` answer with an integer, not a string. The eight constants are
registered consecutively at `gbr.exe` 0x005b70b2:

| Value | Constant |
|---:|---|
| 0 | `Gaul` |
| 1 | `Rome`, `RepublicanRome` (two names, one value) |
| 2 | `Carthage` |
| 3 | `Iberia` |
| 4 | `ImperialRome` |
| 5 | `Britain` |
| 6 | `Egypt` |
| 7 | `Germany` |

`obj.race` (0x005ada20) reads the **class** descriptor, not the owner, so a captured Egyptian
town hall still answers `Egypt`.

#### What is still unknown

- **The upper 24 bits of a relation word, and bits 1 and 3 of the low byte.** Never set in
  any shipped map, never read or written by any of the ten functions above. They may be
  padding, or relations the shipped content never used. Do not invent a meaning for them.
- **What `share support` and `share control` actually do.** The bits are certain and the
  masks they feed (`supportflags`, `controlflags`) are named by the serialiser, but no
  shipped map sets either off the diagonal, so there is no observed behaviour to reproduce.
  `share support` in particular has never been seen granted between two different players.
- **The per-player dword at record offset `0x64`.** `DiplAreAllied` requires it to be
  non-zero on *both* players before it will answer yes, so it gates every alliance. It is not
  one of the nine fields the serialiser names, nothing in the shipped data reaches it, and
  its meaning was not recovered. The reimplementation does not model it, which means it may
  answer `true` where the original answers `false` for a player in whatever state that field
  records.
- **Which mask each `CVXPlayerFlagsQuery` type consults.** The three `type` values are
  proven; that type 2 filters on `enemyflags`, type 1 on its complement and type 3 on
  `controlflags` is inference from the function names. The query's match function was not
  disassembled.
- **What integer a raceless class carries.** `obj.race` returns a class-descriptor field
  verbatim and the class XML writes `race="None"` for the generic classes, but the value the
  engine stores for `None` was not recovered.
- **Whether `AI` and `difficulty` belong to the same subsystem as the matrix.** They sit on
  the same element; [`ai-ini.md`](ai-ini.md) documents both.

### `players.xml`

`randommap.BFHP` alone additionally carries a bundled `players.xml`: a
`<playersdatasection>` holding sixteen `<playerdata>` elements with the same attributes,
except that `allied` is spelled `sharedvictory` and `AllowedRaces`, `difficulty` and
`bonus` are absent. All sixteen are placeholders — `id="-1"`, `race="Select"`, and
`relations` filled with 64 bytes of `0xCD`, the MSVC uninitialised-heap pattern that
[`data-model.md`](../data-model.md) already records leaking into seven `<state>` offsets.
A loader must not choke on it.

## `map.xml`

```xml
<map name="Zama" displayname="Battle for Zama" descr="" persist_state="1">
	<size x="16384" y="16384"/>
	<user_art explored="CurrentMap/zoommap.bmp"/>
	<expl NoFog="0" NoExplore="0"/>
	<start_pt x="12091" y="10979"/>
</map>
```

| Element / attribute | Required | Domain | Meaning |
|---|---|---|---|
| `map/@name` | yes | free text | internal name; `New Map` in 8 of 29 |
| `map/@displayname` | no | free text | shown to the player; absent in one file |
| `map/@descr` | yes | free text | non-empty in exactly one map |
| `map/@persist_state` | no | 0, 1 | carry unit state to the next map; absent in 5 |
| `size/@x`, `@y` | yes | 8192, 16384, 32768 | the world square; always `x == y` |
| `user_art/@explored` | yes | `CurrentMap/zoommap.bmp` | the same string in all 29, and the file is **not** in any container |
| `expl/@NoFog` | yes | 0 | fog of war off; never used |
| `expl/@NoExplore` | yes | 0, 1 | whole map pre-explored; 2 maps |
| `start_pt/@x`, `@y` | yes | world units | initial camera position |

Twenty-five of the twenty-nine maps are 16384 units square, three are 32768 and the
tutorial is 8192.

`randommap.BFHP`'s `start_pt` is `-842150451` in both axes, which is `0xCDCDCDCD` read as
a signed 32-bit integer: uninitialised memory serialised straight into the file. Treat it
as "no bookmark", not as a coordinate.

## `map.obj.xml`

The important one. Up to 716 KB, and it places every object in the world. A `<mapobject>`
root with three kinds of child, freely interleaved:

```
<mapobject>
  <scriptobj .../>              a free-standing object
  <settlement ...>              a town, wrapping the buildings that make it up
    <scriptobj .../>
  </settlement>
  <group name="..." type="1">   a script-visible name bound to a set of objects
    <obj num="17"/>
  </group>
</mapobject>
```

Across the 29 maps: 27,070 `<scriptobj>`, 748 `<settlement>`, 2,181 `<group>` holding
23,408 `<obj>` references.

### `<scriptobj>`

Six attributes are on every object; the rest come in groups selected by what the object
*is*.

| Attribute | On | Domain | Meaning |
|---|---|---|---|
| `class` | all 27,070 | 504 distinct | the class to instantiate |
| `num` | all | 0..n-1 | identity within the map |
| `x`, `y` | all | world units | position |
| `flags` | all | `0x…` hex | see below |
| `dir.x`, `dir.y` | 27,069 | signed | facing, as a vector |

`num` is **contiguous from 0 in document order** in every one of the 29 maps, counting
objects inside settlements. It is what `<group><obj num="…"/>` and the script host refer
to. All 23,408 group references resolve.

`dir.x`/`dir.y` is a direction *vector*, not an angle and not normalised: magnitudes run
from 1 to 17,765. Only the direction matters. 19,851 objects carry the resting default
`(0, 1)`, one object carries `(0, 0)`, and one object has no direction attributes at all.

The **owning player** is 1-based here: `player="1"` is the slot `player0.xml` describes.
Subtract one before indexing the player table. Values 1..8, 15 and 16 occur; 15 and 16 are
the neutral and wildlife owners.

#### `flags`

A 32-bit hexadecimal word, 64 distinct values in the retail data. It decomposes cleanly.

| Bits | Established meaning |
|---|---|
| 0..15 | **one-hot owner mask, `1 << (player - 1)`.** Exact for all 27,070 objects: zero when there is no `player` attribute, and `1 << (player-1)` when there is. Bit 14 is player 15, bit 15 is player 16. |
| 22 (`0x00400000`) | the object is a mobile unit. Exactly equals "carries `Level`, `UnitFlags` and `stamina`", all 16,171 of them. |
| 23 (`0x00800000`) | the object is a `<settlement>` member. Exactly equals it, all 4,721. |
| 24 (`0x01000000`) | the class is a `CVXHero`. Exactly equals it, all 164. |
| 27 (`0x08000000`) | **the object is an unspawned spawn template.** 10,610 of 27,070 — 39% of every object the shipped maps place. See below. |
| 31 (`0x80000000`) | set on every object in every map. |

Bits 26 and 29 are set on units only and are **not explained**; see "What is still
unknown". Bits 16..21, 25, 28 and 30 are never set.

##### Bit 27: the spawn template

An object carrying it is placed, holds a handle, and is **not in play**. It is the
unspawned half of a `<group type="1">`, and `SpawnGroup(name)` is what brings copies of
its members onto the field. `gbr.exe` shows both ends of that:

* `CVXGroup::Add` (`0x00572410`) routes on it. It tests `[obj + 0x2c] & 0x08000000` —
  the same word the `SyncFlags` of a desync dump prints — and files the object in the
  group's *template* deque at `+0x54` when the bit is set and in its live deque at
  `+0x24` when it is clear. `CVXGroup::Spawn` (`0x00572760`) iterates the template
  deque, and clears this bit on each copy it mints so the copy files itself live.
* Every collection path skips it: the area-query grid sweep at `0x004fc6f4`, the
  `ObjList` collector at `0x0041f4e1`, and some forty other sites, all
  `test [obj + 0x2c], 0x8000000` / `jne skip`.

The shipped data agrees, and a control tells this bit apart from its neighbours. Across
the 29 maps it partitions the 803 type-1 `<group>` elements **576 all-marked / 209
none-marked / 18 mixed**, and of the **214** groups a script spawns by literal name,
212 are all-marked and **none** is none-marked — against 55.7 expected if the bit were
unrelated to spawning. Every other bit in the word either fails that test or is
degenerate: bit 22 splits the groups 794/9 and bit 29 splits them 789/12, so neither has
a side to be wrong about. Bimodality alone proves nothing here — group members are
homogeneous by construction — which is why the discriminating figure is the zero.

It is not a rarity. Per map it runs from 16% of `6_Great_loses_Boudicca` to **77% of
`5_Great_Battles_Britain`**, and only 15 of the 10,610 marked objects are in no type-1
group at all. A reader that treats templates as ordinary objects hands its caller a map
with half again as many units as the game has, every one of them fightable.

The nine desync dumps carry it on 0 of 7,580 objects, which is *consistent and not
confirming*: they are random-map games, which author no groups and therefore have no
templates. Bit 29 is absent from them for the same uninformative reason.

The owner mask is redundant with the `player` attribute and agrees with it everywhere, so
either can be read. It is worth checking as a cheap integrity test.

#### Attribute groups

Which extra attributes appear is determined by the class, and the presence of a group is
mirrored in `flags`.

**Ownable** — anything with an owner (22,629 objects): `player`, `healthperc`,
`inventorysize`. `healthperc` is 100 except for two objects (30 and 40); `inventorysize`
is 0 except for 205. **What the loader does with them** (`Obj`'s attribute reader,
0x005af280): `healthperc` scales the class `maxhealth` (0x005af698); `health` is written
raw into `[obj+0xc0]`; `stamina` raw into `[obj+0xc4]` (0x005af4e9), which is why the
dumps show 20 on buildings whose class says `maxstamina="0"`; `inventorysize` is parsed and
the result **dropped** (0x005af576) — the class's `inventory_size` bounds the holder.

**Unit** (16,171, flag bit 22), adds: `Level`, `UnitFlags`, `stamina`.

- `Level` 0..59 (5,125 at 0; 3,797 at 9; 1,279 at 19; 43 at 49; 20 at 59), veterancy,
  and **0-based**: the unit loader (0x005dd3a0) parses it at 0x005dd510 and calls the
  unit's `SetExperience` (`vtbl+0x118`, 0x005dbf70, `[unit+0x180] = arg`) with
  `exp_table[Level + 1]` — the running-total threshold table at 0x009bf87c, indexed from
  1 — and `level_for_experience` (0x005d28c0) of that is `Level + 1`. A `Level="9"`
  legionary is level 10; `Level="0"` is level 1. This engine loads it as the earned
  (`inherent`) level `Level + 1` with the progress counter at zero.
- `stamina` is **10 for every single unit** and 20 for every building, so it carries no
  information in the shipped data at all. It is a serialiser field, not a design knob —
  and the loader honours it (above).
- `UnitFlags` is a bit set with five bits in use, and it is the **second** flag word --
  `[obj+0x194]` in `gbr.exe`, not the `flags` attribute. `0x00040000` on 14,586 (the
  ordinary case), `0x00020000` on 2,028, `0x02000000` on 253 of the 254 `CVXFlyingUnit`
  instances, `0x00400000` on 46 flying units, `0x00200000` on 169. **Two of the five are
  named now**; the other three are still unresolved.

  - `0x00040000` is bit 18, **`UNITFLAG_NOAI`** — the constant the script language exposes
    and the only mask any shipped `Unit::GetFlags` call site passes. The correlation
    settles it: the bit is clear on *every wild animal in the install* (226 deer, 207
    crows, 101 wolves, 80 fish, 46 eagles, 25 boars, no exceptions) and set on the
    soldiers. A campaign places its troops under the mission script, which hands them to
    the AI with `SetNoAIFlag(list, false)`; wildlife is the AI's from the first tick.
  - `0x00400000` is bit 22, **`Flying::IsInAir`** (`0x0051bd20`). It appears only on flying
    units, so 46 birds begin their map airborne.

  `0x02000000` is still only *correlated* with flying rather than explained — 253 of 254 is
  not 254 of 254, and no reader of bit 25 at `[obj+0x194]` has been found.

**Building** (4,721, flag bit 23), adds `stamina="20"` and, on 407 of the 408 town halls
and nothing else, `data="-1"`. The value never varies, so `data`'s meaning cannot be
recovered from the data.

**Hero** (164, flag bit 24), adds up to five `hs<Skill>` ratings out of 25 known skill
names — `hsAdministration`, `hsAssault`, `hsBattleCry`, `hsCeasefire`, `hsCharge`,
`hsConcealment`, `hsDefensiveCry`, `hsDiscipline`, `hsEgoism`, `hsEpicArmor`,
`hsEpicAttack`, `hsEpicEndurance`, `hsEuphoria`, `hsFrenzy`, `hsHealing`, `hsLeadership`,
`hsQuickMarch`, `hsRecovery`, `hsRush`, `hsScout`, `hsSurvival`, `hsTeamAttack`,
`hsTeamDefense`, `hsVigor`, `hsWisdom` — each rated 0..10. Heroes may also carry
`display_name` and `Icon` (a `gameres/icons/*.bmp` path; all 17 resolve against the packs).

The hero loader (0x00530810) matches each attribute against the `hs*` name table at
0x00824b68 — 25 entries in `SKILLS.INI` order, the same order the skill enum has — and
calls `Hero::SetSkill(id, points)` (0x0052def0), which clamps to `[-1, 10]` and writes the
byte at `[hero+0x1fc+id]` **without looking at the point balance**: the balance is not
stored at all but derived — `AvailableSkillPoints` (0x0052da80) is
`min(10 per offered skill, level_for_experience(exp)) - spent`, one point per level.
`display_name` is assigned by the unit loader into the string at `[unit+0xa0]`
(0x005dd485), over the class's display name, and is what the info bar shows for the
object (93 in the shipped maps: `Aeneas`, `PtolomaeusXI`, `Grand priest`).

```xml
<scriptobj class="RHero2" num="1"
	hsAdministration="0" hsTeamAttack="0" hsTeamDefense="0"
	hsQuickMarch="10" hsEpicEndurance="0"
	Level="9" UnitFlags="262144" player="4" healthperc="100"
	stamina="10" inventorysize="0"
	x="423" y="168" flags="0xA9400008" dir.x="51" dir.y="102"/>
```

**Inventory** — `slot0`, `slot1`, `slot2`, `slot3`, dense from 0, naming an item by `id`.
259 slots are filled across all maps, using 21 distinct items. Every one resolves against
`DATA\ITEMS.XML` (42 stock items) or the container's own `itemsCustom.xml`. The loader
(0x005af5d0) takes every attribute whose name starts `slot`, in document order, mints the
item by id (0x00535640 tries the catalogue by one key and then the other) and hands it to
`AddItem` (`vtbl+0xe8`); a full holder refuses, as it does for a script.

**Area** (904, all of class `AdvArea`), adds `type`, `nextmap`, `targetarea` and either a
circle or a rectangle. `type="1"` is a circle with `ptx`, `pty`, `r`; `type="0"` is a
rectangle with `left`, `top`, `right`, `bottom`. The split is perfectly clean: 661 circles
and 243 rectangles, and no area has attributes of the wrong shape. `nextmap` and
`targetarea` are the empty string on all 904.

The object's own `x`/`y` is **exactly the centre of its area** in all 904 cases — the
circle centre, or the midpoint of the rectangle. Areas are what scripts mean by a place:
`AreaCenter("Ruins")`, `WaitUnitsInArea(G1, "Campfire", 100)`.

```xml
<scriptobj class="AdvArea" num="132"
	nextmap="" targetarea="" type="0"
	top="366" bottom="640" right="2472" left="2268"
	player="1" healthperc="100" inventorysize="0"
	x="2370" y="503" flags="0x80000001" dir.x="0" dir.y="1"/>
```

**Cargo** (26 objects, all `CVXWagon`), adds `type` and `amount`. `type` here is a cargo
kind, not an area shape — the two share the attribute name and are told apart by the
company they keep. `amount` runs 0..3000, and the wagon loader (0x005ec690) writes it into
`[obj+0x1cc]` at 0x005ec704 — the field `Wagon::amount` (0x005ebdb0) reads back, this
engine's `ObjectState::cargo`. Which cargo `type="0"` and `type="1"` select is not
determined by the data.

**Teleport** (44 `CVXTeleport`), adds `destination_set`, and it is **the settlement the far
teleport belongs to, named by that settlement's `id` attribute** — not an object `num` and not
a settlement *index*. `id` differs from document position on 97 settlements across the
install, so a reader that subscripted `<settlement>` with it pairs the wrong teleports.

Three properties hold across all 54 teleports in the 28 `map.obj.xml` documents, and together
they are what makes the pairing well defined:

- every teleport is inside a `<settlement>` — 54 of 54;
- **no settlement holds two of them**, so "the teleport of settlement *n*" names exactly one
  object; and
- the relation is **symmetric** in 54 of 54: if A names B's settlement then B names A's. The
  executable stores only one direction — a 16-bit object id per instance, read by
  `Teleport::destination` — so a one-way pair would have been legal and simply does not occur.

Each teleport is the sole member of its own `<settlement>`, whose
`classoffirstbuilding` is the teleport's class, which is why the settlement is available as
the pairing key at all.

One object carries `health="2000"` — an absolute figure rather than the usual
`healthperc`. It is the only instance in the corpus.

#### Class references

`class` names a class in the 845-class graph, resolved through `GameData.lookup`, which
accepts an `id` or an `altid`. **All 27,070 references resolve**: 27,069 by `id` and one
by `altid`. 504 distinct classes are used, out of 845 defined.

`<settlement classoffirstbuilding>` also names a class, and all 747 of those resolve too
(39 distinct). This is *not* the `importsettlement` trap: `importsettlement` names a
settlement template, but `classoffirstbuilding` really is a class reference and should be
validated as one.

The trap in this format lives elsewhere — see `territories.xml` below.

### Writing

`core::write_map_objects` (`engine/core/include/imperivm/core/world/map_writer.hpp`) writes
the document back, and the property it has is the one the pack rebuilds have: **every one
of the 28 shipped documents comes back byte for byte** (`imcheck objects`,
`tests/test_corpus_map_writer.py`). The layout is fixed and is the same in all 28: CRLF
line ends, `\t<mapobject>`, a `<settlement` at two tabs with each attribute on its own line
at three, every `<scriptobj` at one tab with `class` and `num` at two and the rest at one,
`<group` at two with `<obj` at three and its `num` at four; free objects first, then the
settlements with their members, then the groups (`o+S+g+` in every document that has all
three). Hex `flags` are `0x%08X`, upper case. `&`, `<` and `"` are escaped and nothing else
— `slot1="King's Belt"` ships with its apostrophe raw, and no shipped document holds an
entity.

The attribute order is the element's own. The original serialises each class's attributes
between `num` and `player` in an order the class hierarchy decides — a hero's `hs*` skills,
then `display_name`, `Level`, `UnitFlags`, `Icon`; an area's `nextmap`, `targetarea`, `type`
and shape; a teleport's `destination_set`; a wagon's `amount`, `type` — and a writer that
emitted only the attributes this engine reads would strip all of that on the first save.
So the reader keeps every attribute, verbatim and in order, on `MapObject::attributes` and
`MapSettlement::attributes`, and the writer emits that list after `sync_attributes` has
written the typed fields (`num`, `x`, `y`, `dir.x`, `dir.y`, `player`, `flags`,
`healthperc`/`health`, `UnitFlags`, `destination_set`, the settlement's economy fields)
back into it. An object the editor *creates* has an empty list and gets the shipped tail
alone — `class`, `num`, `player`, `healthperc`, `x`, `y`, `flags`, `dir.x`, `dir.y`, and
`UnitFlags`, `health`, `destination_set` or an area's shape when they carry a value.
`stamina`, `inventorysize`, `Level` and `data` are **not invented** for it.

**The six terrain layers write back too.** `core::write_grid` (`core/formats/grid.hpp`)
emits a `GRID` container from a grid's own header fields and its rows — see
[`pass.md`](pass.md), "Writing" — and `core::WorldMap` hands each layer out writable
(`terrain_mut()`, `passability_mut()`, …) over the bytes it owns, so painting a cell and
saving the layer is `set_cell` followed by `write_grid` on the same object. A setter keeps
the layer's own depth: the terrain layer is 4 bits on the five blank templates and 8 on
the authored maps, and a value that does not fit is refused, not clamped. For every one
of the 174 layers across the 29 map directories, `write_grid(parse(bytes)) == bytes`
(`tests/test_corpus_grid.py`, alongside the 598 grids in the packs).

**The container's other documents are patched, not rewritten.** `game.xml`, `player<i>.xml`,
`map.xml`, `Notes.xml` and `sequences.xml` are one or two elements deep with everything in
attributes, and the editor's explorer changes one attribute of one element at a time. Rather
than a writer per document that would have to carry every attribute this engine does not read
(`AllowedRaces`, `bonus`, the 128-digit `relations` string), `core::xml_patch`
(`engine/core/include/imperivm/core/xml_patch.hpp`) rewrites the source's own text: an
attribute's value where it stands, a new attribute on its own line at the last one's
indentation, an element cut out with the whitespace before it, a new element laid in before
its parent's closing tag in its siblings' shape. Targets are paths (`map/expl`,
`notes/note[id=GOAL]`). `gamedata::MapEdits::documents` carries the patched texts into the
container by path, replaced where the source holds them and appended where it does not.
What was not edited comes back byte for byte.

`gamedata::write_map_container` copies a container entry for entry through
`BlockFileBuilder` with any subset of one map's documents regenerated — `map.obj.xml`
from an object list, and each of `Terrain.pass.grid`, `Terrain.height.grid`,
`Terrain.light.grid`, `Terrain.terrain.grid`, `Terrain.decor.grid` and
`Terrain.trans.grid` from a grid — through a `MapEdits` struct in which a null member
means "the source's, untouched". `immap rewrite` is its front end: it passes the object
list and all six layers through, regenerated from their parsed selves, and `imrun` reaches
the same hash over the original and the copy; the same rewrite comes back identical in all
seven documents for every one of the 29 map directories. The copy is a different
arrangement of the same blocks, not the same bytes: the original's block order is not
reproduced.

**What the editor's brushes write into the layers** is read off `gbr.exe`'s tool windows
and reproduced in `core/world/editor.hpp`, which names the addresses. In short: a brush is
a disc of cells with `i² + j² ≤ r²` for `r` in {0, 1, 2, 3, 5} terrain or decor cells and
{1, 3, 5, 7, 9} height cells (the five slots of `BRUSHES.BMP`); a terrain stroke writes the
leaf's layer or one of the group's per cell, with deep water (13) allowed only where every
cell within four is water (or under Shift), and ends with the passability rebuilt over the
disc's rectangle (0x00547700: cleared, the terrain's rules baked back, the objects' and
decorations' masks re-stamped from a rectangle grown by 1024, the frame re-imposed — every
rule is in [`pass.md`](pass.md), "The map's passability layer", and the rebuild reproduces
all 23 shipped layers bit for bit); a water *leaf* first levels the ground under the
brush's square to sea level, removes the decorations there and rebuilds that square
(0x004b3dcd) — the Water group does not; the height tools set `Level · 256 / 101`, add
`≈0.64 · Amount` once per stroke, or smooth by `(64 c + 26 Σ neighbours) / (64 + 26 n)`,
and every stroke re-bakes the light layer (above); the decoration brush stamps one kind
exactly (offsets the position's remainder in four-unit steps) or scatters kinds over the
disc at `Density` percent per empty cell with random offsets, each new decoration stamping
its mask and each one written over or deleted giving its footprint back through a rebuild
of the mask's flat extent (0x00547a30); an object placed stamps its mask within its flat
extent (0x004a3fc0), and one moved or deleted gives its old footprint back the same way.
Every brush snapshots the rectangle it is about to touch first, into the editor's undo
(0x0049a850: up to four layers, tagged with a generation so that one undo takes back one
stroke; `core::edit::UndoStack`), and nothing the object or area tools do is undoable — the
original's choice. The terrain is written back at 8 bits whatever depth it came in at
(0x0054a260); the grids are saved as they stand, nothing is rebuilt at save time. **Not reproduced, and labelled:**
0x00547c00, which widens a terrain stroke's rebuild to a shipyard's extent when one stands
near — on a consistent layer the wider rebuild lands on the same bits.

What is not read: whether the original rewrites `flags` on save from the object's live
state or keeps the authored word (the writer keeps what it read, updated only where the
editor moved the object or changed its owner); what a class without `stamina`,
`inventorysize` or `Level` loads as, which is why a fresh object is written without them;
and whether `gbr.exe`'s loader accepts `Packs/randommap.BFHP` written plain rather than
LZIS-wrapped (this engine reads both; it has no LZIS encoder).

### `<settlement>`

A town: a store of population, gold and food, plus the buildings that constitute it. The
buildings are `<scriptobj>` children, and they are the only objects that carry flag bit 23.

```xml
<settlement id="0" player="4" classoffirstbuilding="CTownhall"
	maxpopulation="80" extrasentries="0"
	maxgold="100000" maxfood="100000"
	population="40" gold="3000" food="0" icon="" name="S_Utica">
	<scriptobj class="CWallsNW" num="1065" player="4" healthperc="100"
		stamina="20" inventorysize="0" x="13250" y="9137"
		flags="0x80800008" dir.x="0" dir.y="1"/>
	…
</settlement>
```

| Attribute | Required | Domain | Meaning |
|---|---|---|---|
| `id` | yes | 0..110 | unique within the map |
| `player` | yes | 1..16 | owner; **every member building carries the same value**, 4,721 of 4,721 |
| `name` | yes | free text | script handle, e.g. `S_Utica`; empty on 496 of 748 |
| `icon` | yes | empty | never set |
| `classoffirstbuilding` | 747/748 | 39 classes | the class that defines the settlement |
| `maxpopulation`, `population` | yes | 0..100 | garrison capacity and current |
| `maxgold`, `gold` | yes | 0..100000000 | treasury |
| `maxfood`, `food` | yes | 0..100000000 | stores |
| `extrasentries` | 747/748 | `0` | never set |

`classoffirstbuilding` is the class of the literally-first child in 505 cases and appears
somewhere among the children in 747 — so read it as "the building that makes this a town",
not as a positional reference.

One settlement, in `randommap.BFHP`, uses a different attribute set — `type`, `efficiency`,
`maxunits`, and no `classoffirstbuilding` or `extrasentries`. It is the random-map
generator's template, and a loader should tolerate the variation rather than require the
common set.

### `<group>`

A name bound to a set of objects, by `num`. This is the only naming mechanism in the
format, and it is what every script identifier resolves through. One element spelling, but
`type` selects between two different things — see below.

| `type` | Count | Members | Role | Engine table |
|---|---:|---|---|---|
| 1 | 803 | 1..922, mean 27 | an object list: `G1`, `Enemies1`, `Q_HannibalArmy` | groups |
| 0 | 1,378 | **exactly 1, in every case** | a name for a single object | named objects |

Type-0 entries are named objects. All 904 areas are named by one, as are town halls, gates
and the heroes conversations speak through — `NO_Invisible`, `A_Village1`, `Q_Hero1`.
Type-1 entries are the armies scripts spawn, order and wait on. An object may belong to
several groups — 20 of the 21 maps that declare any group have at least one object in two
of them — and 2,052 of the 2,181 names are unique across the corpus.

The element is as flat as it looks. `name` and `type` are the only attributes, on 2,181 of
2,181; `<obj num>` is the only child element, 23,408 of 23,408; a `<group>` never nests, is
never self-closing, and is always a direct child of `<mapobject>` — **never inside a
`<settlement>`, in 0 of 2,181.** So a group is not a scope and not a hierarchy. It is also
not per-player: the per-player groups the scripts use are a naming convention applied on
top of a flat table — `Group("Player" + .player + groupname)` in
`DATA\SUBAI\ARENA_BEHAVIOR.VS`, `Group("GoldMules" + idPlayer)` in
`DATA\AI\ES_OUTPOSTSELLGOLD.VS`.

#### `type` selects between two engine tables, not two flavours of one

This is `gbr.exe`'s, and it corrects a reading that the call sites alone suggest. The
executable keeps the two apart everywhere it mentions them:

- the runtime type list holds **`CVXGroupQuery` and `CVXNamedObjQuery`**, side by side;
- `NamedObj` is its own script type, with `obj`, `IsValid` and `IsDead` members, its own
  `Could not find named object named '%s' in function 'SpawnNamed'` diagnostic, and its own
  `Named unit %s is dead or not initialized!`;
- the editor's integrity checker writes `<name>=Group("` for one kind and
  `<name>=GetNamedObj("` for the other, and carries separate diagnostic codes
  `GROUP_NAME_REUSED` and `NAMEDOBJ_OVERRIDES_GROUP` — the latter warning that a name
  "used for both a group and a named object" leaves you unable "to access **the group**
  from your sequence scripts".

The shipped scripts agree without an exception. Counting every call site in the map scripts
that passes a string literal, and resolving it against its own map's `<group>` elements:

| Call | Sites | type 1 | type 0 |
|---|---:|---:|---:|
| `Group("…")` | 42 | **42** | 0 |
| `SpawnGroup("…")` | 150 | **150** | 0 |
| `AddToGroup("…")` | 125 | **125** | 0 |
| `RemoveFromGroup("…")` | 100 | **100** | 0 |
| `GetNamedObj("…")` | 66 | 0 | **66** |

483 typed resolutions, zero crossover. (`Group`'s other 45 literal sites and
`AddToGroup`'s other 44 name groups the map file does not declare — see below. The one
`GetNamedObj` miss, `NO_EliteLeader` in `1_Great_Losses_Rome`, names nothing at all.)

So a loader needs **two** tables: `<group type="1">` seeds the group table that `Group`,
`SpawnGroup`, `AddToGroup` and `RemoveFromGroup` share, and `<group type="0">` seeds a
named-object table that only `GetNamedObj` reads. Neither answers for the other, and merging
them makes `Group("Caesar")` resolve an alias the retail engine would not find. The scripts
also reach named objects through a bare identifier with an `.obj` member —
`Village2.obj.player`, `britVillage2.obj.SetCommand("tribute", britTown1.obj)` — because the
editor writes a `Name = GetNamedObj("Name")` preamble into each map's sequence scripts.

#### Which operations create a group and which refuse one

`gbr.exe` carries `Could not find group named '%s' in function '…'. Check the spelling.` for
`ObjList::RemoveFromGroup`, `Query::RemoveFromGroup`, `SpawnGroup`, `SpawnGroupInHolder` and
`SpawnGroupInShip` — and for **no** form of `AddToGroup`, on `Obj`, `ObjList` or `Query`.
`AddToGroup` is therefore the one that creates a group whose name does not yet exist, which
is exactly what `mediterranean` map 4 needs. `Group()` itself carries only `Invalid group
name`, not a not-found message, so naming a group that does not exist yet is legal there too.

#### Names are not unique within a map

Four maps repeat a name, and **every repeat is a type-0 alias**; no shipped map repeats a
type-1 group name.

| Map | Name | Elements |
|---|---|---:|
| `5_Great_Loses_German` | `NO_Invisible` | 11 |
| `4_Great_Battles_Egypt` | `NO_OUT8` | 2 |
| `3_Great_Losses_Egypt` | `Village2` | 2 |
| `RandomMapSettlements` | `sandruins2` | 2 |

Each element names a different object, and a named object is *one* object — so the second
occurrence cannot widen the first into a set the way a group would. Which one wins is not
established; see the unknowns section.

#### The table is a starting state, not a fixed one

This is the part the file cannot tell you on its own, and it decides whether the group table
is load-time data or hashed world state. **It is world state.** Scripts add objects to
groups, take them out again, and name groups that appear in no map file:

- `DATA\AI\ES_OUTPOSTSELLGOLD.VS` reads `Group("GoldMules" + idPlayer)`, fills it with
  `wagon.AddToGroup("GoldMules" + idPlayer)`, and empties it with `RemoveFromAllGroups()`.
  No map declares a `GoldMules<n>`.
- `DATA\SUBAI\ARENA_BEHAVIOR.VS` runs `newunit.AddToGroup("Player" + .player + groupname)`
  on every unit an arena produces — the group `DATA\AI\GETARMYNEED.VS` then reads with
  `EvalGroup("Player" + idPlayer + "BVikingLord")`.
- `Conquests\mediterranean` map 4 opens its second sequence by folding twelve authored
  groups into one the file does not declare, then loops on it:
  `Group("Oasis_Guards1").AddToGroup("Oasis_Guards"); … while (Group("Oasis_Guards").count != 0)`.

Across all 885 `.vs` files in the retail install — the 577 in `data.pak` plus the 308 stored
inside the 24 adventure, conquest and scenario containers — the mutators are as common as
the reader: `Group` 216 call sites, `AddToGroup` 185, `RemoveFromGroup` 138,
`RemoveFromAllGroups` 22. And of the 528 literal `Group("…")` sites in map scripts, 111 —
21 distinct names — name a group their own `map.obj.xml` does not contain. **All 21 are the
target of an `AddToGroup` in the same map's scripts**, which is the correspondence that
settles it: those groups exist because a script made them.

Two consequences for the engine. The group table has to be hashed and iterated like any
other world state — `sim::GroupTable` in `engine/core` is it, keyed by name in first-mention
order — and a member that dies has to leave it, or
`while (Group("Oasis_Guards").count != 0)` never terminates.

The **named-object** table is different on that last point. `NamedObj::IsDead` and
`Named unit %s is dead or not initialized!` only mean anything if a name still resolves
after its object dies, so a binding outlives its object and despawning must not clear it.
`sim::NamedObjectTable` holds those, and it is hashed too: it is object state, and two peers
that disagree about `Village2.obj` diverge on the next command issued through it.

## The terrain layers

Six `GRID` files per map, the same container as `.pass` — see [`pass.md`](pass.md) for the
20-byte header and the bit packing. All six cover the same world square as `map.xml`
declares, verified for every layer of every map.

| Layer | Cell | Bits | Cells at 16384 | Meaning |
|---|---:|---:|---|---|
| `Terrain.pass.grid` | 16 | 1 | 1024 × 1024 | passability; set bit = blocked |
| `Terrain.height.grid` | 32 | 8 | 512 × 512 | terrain elevation |
| `Terrain.light.grid` | 32 | 8 | 512 × 512 | baked brightness |
| `Terrain.terrain.grid` | 64 | 8 | 256 × 256 | terrain type |
| `Terrain.decor.grid` | 64 | 16 | 256 × 256 | scattered decoration |
| `Terrain.trans.grid` | 64 | 4 | 256 × 256 | unknown; zero everywhere |

**The terrain-type layer is not always 8 bits.** The 24 authored maps use 8, but the five
blank template containers in `Packs/` — `emptyadv`, `emptyconquest`, `emptyscn`, `newmap`
and `randommap` — store it at 4 bits per cell. Both are structurally valid `GRID` files and
a reader must take the depth from the header rather than assuming.

### Passability

One bit per 16 world units, set meaning blocked, exactly as a `.pass` mask. This layer is
**pre-baked**: it already contains both the terrain's own impassability and the footprints
of the objects placed on the map. Terrain type 6 (`Rocks 1`, which `DATA\TERRAINS.XML`
marks `passable="0"`) is 92% blocked in the layer; ordinary grass runs 5–30% blocked
depending on how much is built on it. The engine does not have to stamp static object masks
into this grid at load time — the editor already did — but it does have to keep the object
masks around for anything built or destroyed during play. **How the editor bakes it is
read and proven**: the terrain's rules, the mask stamp through the projection, the
decorations' offsets and the frame along the edges are all in [`pass.md`](pass.md), "The
map's passability layer", and `immap passability` rebuilds every shipped layer from
nothing and gets the stored bytes back on all 23 playable maps.

### Height

One unsigned byte per 32 world units, full 0..255 range used.

**0 is sea level.** Every cell under shallow or deep water is 0, in 1,673,853 of the
1,676,000 water cells across all maps; the 1,100-odd exceptions are 1. 48.8% of all height
cells are 0. The mapping from these 256 steps to world units is a projection question and
is not recoverable from the map files alone.

**The simulation reads this layer for exactly one question**, and it never has to answer
that projection question to do it. `GetTerrainHeight(pt)` (`gbr.exe` 0x0051b4b0) clamps the
point into the map rectangle and hands it to a **bilinear** sampler at 0x0053dc00: two
horizontal interpolations and one vertical, each `(b - a) * f / 32` with a divide that
truncates toward zero, and the cell to the right and the row below clamped at the last one
so the far edge repeats rather than wrapping. Every consumer is a bird, `Flying::PlayAnim`
range-checks its altitude argument to `-1 .. 1024`, and every shipped call site builds one
out of this function — so the whole of the flight arithmetic stays inside the layer's own
0..255 and never converts to world units at all. See `sim/flying.hpp`.

### Light

One unsigned byte per 32 world units, but only values **0..21** ever occur, and the mean is
almost exactly 16 on every terrain type. This is a baked brightness *offset* around a
neutral of 16, not an absolute intensity.

Two facts pin the reading down. Deep water (type 13) is 16 in 1,399,392 of 1,399,396 cells,
and the "Invalid" terrain (type 15) is 16 in all 972,868 — surfaces the editor never lights
keep the neutral value. Rock (types 6, 8, 26) is where the extremes live: a heavy spike at
21 and a second cluster at 0 and 6, which is what cliff faces lit and shadowed from one
direction look like. 71% of all cells are exactly 16, and the layer is smooth, averaging
0.3 to 1.1 steps of change between horizontally adjacent cells.

What the 22 steps mean numerically is not determined by the map data; it is read off the
renderer, below. **What they are is read off the editor**: the layer is a slope, not a light. `gbr.exe`'s height commit (0x00544e10, run after
every stroke of a height brush) writes

```
light[x, y] = clamp(H[x, y] - (H[x + 1, y] + H[x, y + 1]) / 2 + 16, 0, 21)
```

over the 32-unit height cells, and 16 on the far row and column where there is no neighbour
— which is why flat ground and every water cell sit at 16, why the extremes live on rock
where the height steps, and why the layer is smooth. The loader recomputes it from the height
the same way when `.light.grid` is missing (0x0054b4d0).

**How the renderer draws it is read too.** The ground warp (0x006217f0) takes the level at the
four 32-unit corners of each quad it draws -- the same vertices the height is read at --
interpolates it in 8.8 fixed point along the quad's top and bottom edges and then down each
screen column, and looks the **integer** level up in a table built at 0x0061e210:

```
out = clamp(c + trunc(c * (level - 16) / 20), 0, max)      per colour channel
```

a gain of `(level + 4) / 20`: unity at 16, 0.2 at 0, 1.25 at 21. The table covers 32 levels
over 5-bit channels (the original draws in RGB555), and a quad whose four corners are all 16
is copied without it. So the light is bilinear per pixel and continuous between quads, moving
in whole levels -- one level is 5% of brightness.

### Terrain type

One byte (or nibble) per 64 world units. The value is the `z` attribute of a `<layer>`
element in `DATA\TERRAINS.XML`, which defines 41 layers, `z = 0..40`. Each layer names a
texture, a minimap tile, and a `type` group:

| `type` | Layers | Group |
|---:|---|---|
| 0 | 15 | Invalid |
| 1 | 3, 4, 18, 23, 24, 30, 34, 35 | grass |
| 2 | 0, 1, 2, 21, 22, 33 | ground |
| 3 | 5, 9, 16, 17, 25, 40 | sand |
| 4 | 12, 13 | water |
| 5 | 6, 8, 11, 26, 28 | rocks |
| 6 | 7, 14, 19, 20, 27, 29, 31, 32, 36, 37, 38, 39 | roads |
| 7 | 10 | waves |

`z = 0..20` are the spring/summer set, `21..32` winter, `33..40` autumn. Two layers carry
extra attributes the engine reads directly: 6 and 26 are `passable="0"`, and 12 and 13 are
`dark="1"` with `frames="15"` (animated) — the file itself notes that these two are
hardcoded in the engine as `SHALLOW_WATER_IDX` and `DEEP_WATER_IDX`.

The `minimap` tile is what the zoom map is drawn from, not the texture: `gbr.exe` loads it as
`minimap/zoom%d/terrain/<minimap>` for the level in use (0x006183b0) and composes the zoom
map's ground from those tiles through the minimap's own C and D masks, cell by cell as the
ground is composed ([../engine/rendering.md](../engine/rendering.md), "The zoom map's
ground"). All 41 layers name one, and `Minimap.pak` ships every one at `ZOOM8`, `ZOOM16` and
`ZOOM32` for each season but layer 10's `%season%/waves.bmp`.

Across all 29 maps, 31 of the 41 layers are used. `z = 10, 24, 25, 28, 30, 31, 32, 37, 38,
39` never appear — the waves layer and most of the winter and autumn road sets, which is
consistent with every shipped container declaring `season="spring"`. Nonetheless spring maps
*do* mix in autumn and winter layers by hand (33, 34, 35, 36, 40, 21, 22, 23, 26, 27, 29 all
occur), so the terrain layer is not season-filtered at load time; the season only selects
which `%season%` texture path a layer resolves to.

The commonest values are 3 (Grass 1, 33%), 13 (deep water, 14%), 0 (Ground 1, 13%) and 15
(Invalid, 10%). "Invalid" filling a tenth of the world is normal: it is what lies outside
the playable region.

### Decor

Sixteen bits per 64-unit cell, and 98% of cells are zero. A non-zero cell means one
decoration is stamped there. The layout is:

```
bits    field
-----   -------------------------------------------------------------
0..7    decor kind: the `type` number of a section in
        MAPOBJECTS\DECORS\DECORS.INI. 1..199. 0 means an empty cell.
8..11   sub-cell X offset, in units of 4 world units (0..60)
12..15  sub-cell Y offset, in units of 4 world units (0..60)
```

`DECORS.INI` (in `MapObjects.pak`) holds 199 sections with `type = 1..199`, all distinct,
each naming an `entity` path plus a `group`/`subgroup` for the editor's brush palette and a
`season`. Types 1..166 are spring, 30..175 winter, 176..199 autumn — the ranges overlap, so
the season is a property of the entry, not of the number. All 117 distinct kinds used across
the maps resolve to a section.

The world position of a decoration is therefore
`(cell_x * 64 + offset_x, cell_y * 64 + offset_y)`.

The offset reading is measured, not assumed. Isolated decorations produce an identifiable
blob in the passability layer; regressing the blob's centroid against each nibble over 953
well-separated samples in fourteen decor kinds gives a slope of **4.15 in X and 4.06 in Y**
with intercepts of 2.0 and −4.2 world units, against a predicted slope of exactly 4 and an
intercept of 0. Per-kind slopes range from 3.93 to 4.47. The residual is the footprint
asymmetry of the art, not a different scale. The high byte is also uniform — each of the 256
values occurs 145 to 248 times out of 47,930 stamps — which is what a positional jitter
looks like and what a kind index would not.

47,930 decorations are stamped across the 29 maps. Six maps have none at all.

### Transitions

Four bits per 64-unit cell. **Every one of the 2,441,216 transition cells in the retail
install is zero.** The layer is allocated, sized and shipped, and carries no information.

Its name and its resolution suggest it holds the blend mask between adjacent terrain types,
and `DATA\TERRAINS.XML` supports that reading — layers 12 and 13 carry `transition="2"`,
and layer 12 additionally carries `transition_terrain="10"` and `animate_transition="1"`,
which is the shore-foam effect. But nothing in the shipped maps exercises it, so the four
bits' meaning cannot be recovered from the data. A reimplementation should read it, keep it,
and generate transitions some other way.

**The original's renderer does not read it either.** It holds the layer, under the name
`transitions` beside `height`, `light` and `terrain` (0x0061e5f2), and its ground compositor
(0x0061f8a0) derives every blend from the terrain layer at draw time: the terrain byte is a
vertex, a tile blends the four at its corners, and the mask is chosen by the overlay's
corners and its layer's `transition` attribute (default 2) plus the tile row's parity -- C and
D alternating. The rule is in [`../engine/projection.md`](../engine/projection.md), "The
transition masks". What the four stored bits would have meant stays unknown.

## The rest

### `labels.xml`

A `<root>` of `<label>` elements — the only document in the format whose root element is
generically named. Present in 23 of the 29 maps, 77 labels in total, and 6 of the 23 files
are empty.

| Attribute | Domain |
|---|---|
| `text` | place name, e.g. `Memphis`, `Roman camp` |
| `x`, `y` | world units; all 77 are inside their map |

The labels are the editor's. `gbr.exe` loads the document through one routine
(0x0049f590, `CurrentMap/labels.xml`) whose two callers are the editor's label manager
(the *Overlay text* tab of `AdvCurMap.ini` and `AdvScenario.ini`) and the editor's floating
minimap window, and draws them through one (0x0049f7a0, in `Fonts/tahoma16b.apf`) onto that
minimap and onto the *Place Label* screen. The zoom map and the bars never read it, so a
player sees a label only through the `MiniMap()` cheat's window (see
[../engine/rendering.md](../engine/rendering.md)); the `map label` translation category
exists because the editor's strings are translated like the rest. The editor writes the document back whole
(0x0049fb20: `<root>`, one `<label` per entry, `\r\n` line ends); this engine patches it
in place, and seeds `<root>` for the six maps that ship without the file when a label is
first laid in.

### `Notes.xml`

A `<notes>` of `<note>`: journal entries that scripts award with `GiveNote("id")`. One at
the container root and one per map; 122 notes across the corpus.

| Attribute | Domain | Meaning |
|---|---|---|
| `id` | free text | the handle scripts use |
| `title`, `text` | free text | heading and body |
| `icon` | empty or `gameres/noteicons/triangle.bmp` | 15 of 122 set it |
| `map` | a map's `name` | which map the note belongs to; empty on 66 |
| `show_on_minimap` | 0, 1 | 44 of 122 |
| `locationx`, `locationy` | world units or `-1` | where to pin it; `-1` means no location, 65 of 122 |

### `sequences.xml`

A `<sequences>` of `<sequence>`: the trigger scripts. One at the container root, one per
map, 308 references in total.

| Attribute | Domain | Meaning |
|---|---|---|
| `name` | free text | handle for `RunSequence("…")` / `IsRunning("…")` |
| `script` | `CurrentMap/sequences/seq<k>.vs` or `CurrentGame/sequences/seq<k>.vs` | the source |
| `wizard` | empty | never set |
| `autorunallowed` | absent or `no` | 46 of 308 opt out of autorun |

The path root is virtual: `CurrentMap/` is the sequence's own `Maps/<n>/` directory,
`CurrentGame/` is the container root. `gbr.exe` carries both spellings verbatim
(`CurrentGame/Sequences/sequences.xml`, `CurrentMap/Sequences/sequences.xml`), along with
`autorun`, `autorunallowed`, `RunSequence` and the per-sequence status key `/SequenceStatus/%s`.
All 308 references resolve to a stored `.vs` file — note that the attribute usually spells the
directory `sequences` where the entry is `Sequences`, so resolution has to fold case. The
language is [`vs-language.md`](vs-language.md); the host calls are
[`vs-host-api.md`](vs-host-api.md).

**What `autorunallowed="no"` means, with the count that shows it.** The 46 that opt out are the
ones another script starts. Across the 24 containers, 32 of those 46 are named by a
`RunSequence` / `IsRunning` / `IsWaiting` / `IsFinished` string literal somewhere in their own
container, against **5 of the 262** that allow autorun.

Those four are the family. An earlier revision of this paragraph searched for `RunSequence` /
`StopSequence` / `IsSequenceRunning` and reported 31 against 2 — and reproduces, because
**neither `StopSequence` nor `IsSequenceRunning` exists**: neither string occurs anywhere in
`gbr.exe` and neither has a call site in the 885 shipped scripts, so both contributed nothing to
the count and the error was invisible. The five entry points the sequence manager really
registers, in one block at `0x005bcb90`, are `RunSequence`, `IsRunning`, `IsWaiting`,
`IsFinished` and the debug dump `_SequencesStatus`. The conclusion is unchanged and the
numerator is slightly stronger. So a sequence that allows autorun starts
with its map, and one that forbids it waits to be named. `engine/core/src/sim/campaign.cpp`
(`parse_sequences`, `sequence_entry_path`) reads the manifest and
`GameSession::start_sequences` runs it; `engine/tools/imrun` is the thing that exercises the
whole path over a real container.

### `Conversations/cnv<k>.conv.xml`

109 documents across the corpus, all under `Maps/<n>/Conversations/`, plus one stray at a
container root.

```xml
<conversation name="1 Welcome" startup="first" restore_view="0">
	<actor name="Instructor"/>
	<phrase actor="Instructor" text="Welcome, commander." followup="first"/>
	<phrase actor="Instructor" text="Shall we begin?" followup="choice"
		choice_text="Yes" followup_phrases="Commands;Selection;Done"/>
</conversation>
```

`<conversation>`: `name` (the handle for `RunConv`), `startup` (always `first`),
`startup_phrases` (a `;`-separated list, on 9 of 110), `restore_view` (0 or 1).

`<actor name>`: 134 elements, 63 distinct names. **Actor names are the names of type-0
object groups**, which is how a line is attached to the object that speaks it. 10 of the 63
are not bound by any group in the map and must be bound at run time by a script; that is
normal, not a fault.

`<phrase>`: `actor`, `text`, and `followup` (`first` 221, `end` 28, `choice` 11), plus
optional `label`, `followup_phrases` (`;`-separated), `choice_text` for a player choice, and
three inline `.vs` fragments — `condition`, `action` and `return` — carried as attribute
values with `<`, `>` and `&` escaped as `\l`, `\g` and `\a`.

### `itemsCustom.xml`

An `<items>` of `<item>`, each with a `<bonus>` child. Empty in 17 of the 22 containers;
five items are defined across the whole install.

`<item>`: `id` (what a `slot<n>` names), `name`, `image`, `sound`, `level`, `description`,
`usecount` (0 = unlimited), `customdata`, `important` (`yes`/`no`), `cursed`, and eight
`*_script` attributes carrying inline `.vs` source — `use_script`, `location_script`,
`object_script`, `kill_script`, `die_script`, `attachedkill_script`, `attacheddie_script`,
`equip_script`, `remove_script`.

`<bonus>`: ten integer attributes — `health`, `damage`, `armor_slash`, `armor_pierce`, the
four matching `*_percent` variants, `level` and `experience`.

### `territories.xml`

Conquests only; one file in the retail install. A `<conquestmap>` of `<territory>`.

`<conquestmap>`: `name`, `data` (a path to a `ConquestMaps/<n>/` directory outside the
container, holding the campaign-map bitmaps), `choose`, `ConqueredOrder`, `interface`, and
twelve colourisation knobs — `owned_colorize`, `enemy_colorize`, `disabled_colorize` and
their `_hue` and `_sat` companions.

`<territory>`: `id`, `index`, `state`, `visualname` (the Latin name shown to the player),
`mapname`, `description`, `bonus`, `bonus_descr`, `neighbours` (a comma-separated list of
territory `id`s — all 7 territories' neighbour lists resolve), `interface`.

**`bonus` is the trap in this format.** Its seven values — `rIberia`, `rBritain`, `rGaul`,
`rRepublicanRome`, `rCarthage`, `rEgypt`, `rGermany` — look exactly like class names,
consisting of a prefix and one of the race names the class graph uses. None of them is a
class. They are the `name`s of `<sequence>` elements in the **container-root**
`Sequences/sequences.xml`, all seven marked `autorunallowed="no"` so that they only run when
the territory is conquered. Validating `bonus` as a class reference produces seven phantom
failures, in the same way `importsettlement` produces five (see
[`data-model.md`](../data-model.md)).

### `Local/<lang>/`

Translation tables mirroring the container's own structure, plus the recorded voice lines.
Three languages appear: `Italian` (17 containers), `Spanish` (6) and `TempLanguage` (1, a
leftover).

```
Local/<lang>/adventure.loc.xml            game.xml + labels + player names + sequence text
Local/<lang>/itemsCustom.loc.xml          the custom items
Local/<lang>/notes.xml                    container-level notes
Local/<lang>/Maps/notes.xml               a roll-up
Local/<lang>/Maps/<n>/notes.xml           per-map notes
Local/<lang>/Maps/<n>/conversations/cnv<k>.conv.xml   dialogue
Local/<lang>/Maps/<n>/conversations/cnv<k>_phrase<p>.wav  recorded speech
```

Every one of these is a `<translationtable>` of `<translationtableentry>`, never the source
document's own schema — so a `.conv.xml` under `Local/` is *not* a conversation. The entry
attributes are `id` (the lookup key), `text` (the English source), `result` (the
translation, or the literal string `(translate)` for 64 untranslated entries), plus `map`,
and for conversations `convname`, `number`, `sound` and sometimes `actor`.

`adventure.loc.xml` keys are typed rather than unique strings: `adventure name`,
`adventure author`, `adventure description`, `adventure player name`, `map display name`,
`map label`, `actor name`, `text from a sequence` and three more. Conversation and note keys
are structured paths — `conv-Numantia-Begin-NO_Cornelius-0`,
`note-Numantia-Forts-text-0`.

### `warehouse.rle` and `env.42`

`warehouse.rle` is a 2,418-byte `IMGRLE` sprite frame table (see [`rle.md`](rle.md)), and it
is **byte-for-byte identical in all 29 maps**. It is not map data; it is a stock asset the
editor writes into every map it saves.

`env.42` is a 22-byte [LZIS](lzis.md) stream that decompresses to four zero bytes. It is the
persisted script environment — the store behind `EnvReadInt` / `EnvWriteInt` / `EnvWriteString`
— saved empty. Eighteen containers carry one; the raw bytes differ only in two bytes of the
LZIS header. `Resources/` next to it is always an empty directory.

## What a real scenario contains

Every container in the retail install, loaded through the reference reader. "objs" counts
`<scriptobj>` including settlement members; heroes are a subset of units.

| Container | maps | size | active players | objs | units | heroes | buildings | settlements | decor | areas | other | groups | seqs | convs | labels |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `GreatBattles/1_…_Zama` | 1 | 16384 | 16 | 1,119 | 860 | 7 | 54 | 9 | 137 | 55 | 13 | 142 | 17 | 9 | 5 |
| `GreatBattles/2_…_Numantia` | 1 | 16384 | 16 | 1,542 | 888 | 19 | 315 | 45 | 279 | 55 | 5 | 165 | 27 | 10 | 4 |
| `GreatBattles/3_…_Alesia` | 1 | 16384 | 16 | 2,973 | 2,524 | 4 | 275 | 11 | 130 | 44 | 0 | 160 | 15 | 4 | 9 |
| `GreatBattles/4_…_Egypt` | 1 | 16384 | 16 | 1,414 | 1,108 | 9 | 214 | 28 | 60 | 26 | 6 | 85 | 15 | 6 | 6 |
| `GreatBattles/5_…_Britain` | 1 | 32768 | 16 | 3,028 | 2,382 | 26 | 456 | 54 | 113 | 73 | 4 | 233 | 22 | 10 | 6 |
| `GreatBattles/6_…_Danube` | 1 | 16384 | 16 | 1,012 | 827 | 4 | 85 | 13 | 63 | 26 | 11 | 91 | 14 | 6 | 4 |
| `GreatChallenges/1_…_Rome` | 1 | 16384 | 9 | 3,540 | 2,115 | 20 | 263 | 16 | 1,056 | 104 | 2 | 279 | 33 | 6 | 8 |
| `GreatChallenges/2_…_Spain` | 1 | 16384 | 9 | 1,023 | 796 | 13 | 148 | 22 | 53 | 22 | 4 | 83 | 21 | 7 | 5 |
| `GreatChallenges/3_…_Egypt` | 1 | 16384 | 16 | 1,400 | 930 | 24 | 217 | 16 | 115 | 32 | 106 | 104 | 14 | 8 | 6 |
| `GreatChallenges/4_…_Gaul` | 1 | 16384 | 9 | 1,207 | 951 | 3 | 45 | 11 | 157 | 24 | 30 | 77 | 23 | 3 | 9 |
| `GreatChallenges/5_…_German` | 1 | 16384 | 16 | 1,075 | 538 | 9 | 96 | 16 | 185 | 244 | 12 | 339 | 20 | 4 | 7 |
| `GreatChallenges/6_…_Boudicca` | 1 | 16384 | 9 | 1,136 | 721 | 15 | 219 | 31 | 135 | 46 | 15 | 125 | 26 | 13 | 6 |
| `Tutorial` | 1 | 8192 | 9 | 297 | 123 | 2 | 84 | 4 | 63 | 15 | 12 | 33 | 5 | 23 | 2 |
| `Conquests/mediterranean` | 7 | 16384 | 9 | 3,467 | 1,130 | 9 | 915 | 222 | 988 | 80 | 354 | 207 | 48 | 0 | 0 |
| `Packs/RandomMapSettlements` | 1 | 32768 | 9 | 1,587 | 0 | 0 | 890 | 64 | 508 | 58 | 131 | 58 | 0 | 0 | 0 |
| `Packs/emptyadv` | 1 | 16384 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `Packs/emptyconquest` | 1 | 16384 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `Packs/emptyscn` | 1 | 16384 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `Packs/newmap` | 1 | 16384 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `Packs/randommap` | 1 | 16384 | 16 | 1 | 0 | 0 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `Scenarios/Balcans` | 1 | 32768 | 16 | 1,024 | 220 | 0 | 360 | 111 | 341 | 0 | 103 | 0 | 0 | 0 | 0 |
| `Scenarios/Crossroads` | 1 | 16384 | 16 | 42 | 0 | 0 | 42 | 42 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `Scenarios/Island War` | 1 | 16384 | 12 | 183 | 58 | 0 | 42 | 32 | 58 | 0 | 25 | 0 | 0 | 0 | 0 |
| **total** | **29** | | | **27,070** | **16,171** | **164** | **4,721** | **748** | **4,441** | **904** | **833** | **2,181** | **300** | **109** | **77** |

The shape of the workload this implies:

- A **typical campaign map** is 16384 units square and holds 1,000 to 3,500 objects: roughly
  two-thirds units, a few hundred buildings in 10 to 55 settlements, 100 to 250 decor
  objects, and 20 to 100 trigger areas. Plus 47,930 decor-layer stamps map-wide, which are
  cheaper objects than the `<scriptobj>` decor.
- The **worst case shipped** is `GreatChallenges/1_Great_Losses_Rome`: 3,540 objects, of
  which 2,115 units, on a 16384 map — and `GreatBattles/5_…_Britain` on a 32768 map with
  2,382 units and 456 buildings.
- **Scenarios and campaigns differ in kind.** The three hand-made skirmish scenarios carry
  no scripts, no conversations, no labels and no groups at all — just terrain, settlements
  and start positions. Every campaign map is script-driven: 300 `.vs` sequences and 2,181
  named groups across thirteen adventures and one conquest.
- Sixteen player slots always exist; between 9 and 16 are not `Disabled`, and in practice one
  or two are human-playable. Player 15 and 16 are the neutral and wildlife owners and hold
  1,692 and 243 objects respectively.
- **Conversations are heavily localised**: 109 English conversations become 155 translation
  tables, and voice lines ship as individual WAVs named after the phrase they speak.

## Validation

The reference reader asserts all of these, and every one holds for all 24 containers in the
retail install:

- `game.xml`'s `start_map` names a map that exists in the container.
- Sixteen `player<i>.xml`, `id` matching the filename; `relations` is exactly 64 bytes with
  nothing outside byte 3 of each four-byte record, and the diagonal is `0x35`.
- `map.xml`'s world is square, one of 8192/16384/32768, and the start point is inside it
  (or is the `0xCDCDCDCD` sentinel).
- Object `num` values are unique and contiguous from 0, counting settlement members.
- Every object's `flags` has bit 31 set, has an owner mask that agrees with its `player`,
  and has bit 22 set exactly when the unit attributes are present.
- Every object's position is inside the declared world. All 27,070 are.
- Every area object's `x`/`y` is exactly the centre of its area.
- Every settlement `id` is unique per map, and every member building carries the
  settlement's `player` and flag bit 23.
- Every type-0 group has exactly one member, and every `<obj num>` resolves to an object.
- All six terrain layers are present, their headers are self-consistent, their cell sizes are
  16/32/32/64/64/64 and their depths 1/8/8/8-or-4/16/4, and their extents equal the world
  size `map.xml` declares.
- Every `<sequence script>` resolves to a `.vs` file stored in the container. 308 of 308.
- Every conquest territory's `neighbours` resolve to declared territories, and its `bonus`
  resolves to a container-root sequence name.

Cross-checks against the class graph, run separately because they need the packs:

- **27,817 class references — 27,070 `<scriptobj class>` and 747
  `<settlement classoffirstbuilding>` — resolve without a single dangling name.** 27,069 by
  `id` and one by `altid`. This is a cleaner result than the class graph's own internal
  integrity, which carries 38 dangling resource references.
- All 259 `slot<n>` item references resolve against `DATA\ITEMS.XML` or the container's
  `itemsCustom.xml`. All 17 hero `Icon` paths resolve against the packs.
- All 117 decor kinds used in the decor layer resolve to a section of
  `MAPOBJECTS\DECORS\DECORS.INI`.
- All 31 terrain values used resolve to a `<layer z>` in `DATA\TERRAINS.XML`.

## Consequences for the engine

- **The object list is flat and pre-resolved.** No inheritance, no templates, no
  back-references except by `num`. A loader can instantiate objects in document order in one
  pass; groups and settlements only need a `num` index built as it goes.
- **The passability layer is authoritative at load time.** It already includes static object
  footprints, so a fresh map needs no stamping pass. Object masks are still needed for
  anything constructed or destroyed later.
- **Naming is entirely by group.** Scripts, conversations and areas all address the world
  through `<group name>`; nothing else in the format carries a script-visible identifier.
  Build **two** name tables before running any sequence — `type="1"` groups and `type="0"`
  named objects are separate and neither answers for the other — and then keep the group
  table as world state, hashed like the object table: the scripts add to it, take from it
  and extend it with names the file never had. See [`type` selects between two engine
  tables](#type-selects-between-two-engine-tables-not-two-flavours-of-one) and [The table is
  a starting state](#the-table-is-a-starting-state-not-a-fixed-one) above.
- **Player indices differ by document.** `player<i>.xml` is 0-based, `<scriptobj player>` is
  1-based. Getting this wrong silently misattributes every object by one slot.
- **Four fields carry no information in the shipped data** and can be ignored until a map
  editor needs to round-trip them: `stamina` (always 10 or 20), `extrasentries`, `data`
  (always `-1`), and the whole transition layer.
- **Tolerate garbage.** One map's start point and one container's whole relations matrix are
  uninitialised heap. The retail game ships and runs with both.

## What is still unknown

- **`flags` bit 29.** Set on units only. Bit 29 (15,433 objects)
  is class-determined for 503 of the 504 classes used — `Trader` is the only class that
  varies — and is clear for exactly the ambient wildlife: every `Deer`, `Crow`, `Wolf`,
  `Eagle`, `Boar`, `Bear`, `Lion` and `Ghost`, 738 objects. That is consistent with "counts
  as a faction army unit" but no class property predicts it, so it is not merely a cached
  class trait.

  ~~**Bit 26**~~ **is settled by the editor's property sheet**: `AdvObjProps.ini`'s
  `STA_Messenger` check box (id `0x4000008`) reads and writes bit 26 (handler
  0x004add20), so the 47 objects that carry it are the ones an author marked *messenger*.
  What the simulation does with the mark is not read. The same sheet names two more:
  `STA_Party` (`0x400000a`) is bit 19, and `STA_Template` (`0x4000006`) is the spawn
  template bit 27 below. And **bit 16 is the editor's selection**: set on a selected object
  in the running editor (0x0048f3f0) and never in a file.

  **Bit 27 was the third of these and is now the spawn-template bit** ([above](#bit-27-the-spawn-template)).
  This entry had the shape of the answer and stopped one step short of it: "nearly a
  subset of *belongs to a type-1 group* — 10,598 of 10,610 do — but 5,268 group members
  do not carry it, so it is not that either". The 5,268 are the **live** halves of their
  groups. A group has two halves and the bit is which one a member is in; looking for a
  property of *objects* could not find a property of *memberships*, and what settled it
  was reading `CVXGroup::Add` rather than counting harder.
- **The transition layer.** Four bits per 64 units, zero in all 2,441,216 cells shipped.
  Terrain-blend mask is the obvious reading, and `DATA\TERRAINS.XML`'s `transition`,
  `transition_terrain` and `animate_transition` attributes point that way, but the data
  proves nothing.
- ~~**The light layer's numeric scale at draw time.**~~ Read: `(level + 4) / 20` per channel,
  the level interpolated per pixel across 32-unit quads (0x006217f0, 0x0061e210); see
  **Light** above.
- **The height layer's world-unit scale.** 0 is sea level, the range is 0..255, and the step
  size is a projection question.
- **`UnitFlags`.** Five bits in use, **two still open**. `0x02000000` is flying (253 of
  the 254 `CVXFlyingUnit` instances) but no reader of that bit has been found, so the
  correlation is not an explanation. `0x00200000` (169) has no visible correlate. Three are
  settled: `0x00040000` is `UNITFLAG_NOAI` and `0x00400000` is `Flying::IsInAir`, described
  under **Unit** above, and **`0x00020000` is the *inverse* of "Unit doesn't eat"** — the
  editor's `STA_NoFeeding` check box (`0x4000021`) shows `!bit17` (0x004aa2f4 reads the
  bit and negates it) and ticking it *clears* the bit (0x004ab130), so the 2,028 units that
  carry it are the ones that **do** eat, and the 14,143 without it are excused from the
  feeder. This entry used to have the polarity backwards, from the check box's name alone.
- **`data="-1"` on town halls.** Present on 407 of 408, never any other value and never on
  any other class, so nothing can be inferred.
- **What the original writes for an object the editor places** — which of `stamina`,
  `inventorysize`, `Level`, `data` a fresh element gets and from where. What is read: the
  save (0x0054c1a0) rewrites the whole `<mapobject>` from the live objects, sweeping the
  spatial grid cell by cell — free objects, then each settlement's, then the groups — each
  object serialised by its class chain (`healthperc` is `health * 100 / maxhealth`,
  `stamina` the raw field), and `num` is a running counter in output order. So a saved map's
  numbering is the sweep's, not the author's; this engine keeps the loaded order and numbers
  new objects after the last, which every reader accepts and no shipped file distinguishes.
  See [Writing](#writing).
- **Wagon `type`.** 0 and 1, with an `amount` from 0 to 3000 alongside. Which is gold and
  which is food — or whether either is — is not determined.
- **`victory_condition` beyond `0` and `1 Elimination`.** Two values in a corpus of 22, and
  `victory_threshold` is never used, so the rule catalogue is unknown.
- **`user_interface`.** Six values (0, 1, 2, 4, 6, 7) that correlate with the human player's
  race but do not match any index in the class graph or the interface `.ini` set exactly.
- **The relations bits that no map sets.** The bit layout itself is no longer unknown — it is
  read out of `gbr.exe` in [The relations matrix](#the-relations-matrix) above — but bits 1
  and 3, the upper 24 bits of each word, the gate at record offset `0x64`, and the behaviour
  of `share support` and `share control` are all unexercised by the shipped content.
- **`user_art/@explored`.** All 29 maps name `CurrentMap/zoommap.bmp` and **no container
  stores that file**. Either the engine generates the explored-map bitmap at run time and the
  attribute is a naming convention, or it is a dead field.
- **`persist_state`, `single_only`, `ConqueredOrder`, `state`, `index`** and the twelve
  conquest colourisation knobs: single-valued or near-single-valued in the retail data, so
  their domains are not established.
- **The number `EvalGroup(name)` returns.** Its *signature* is no longer unknown —
  `gbr.exe` declares it as `int, str group`, so it takes a group name and returns an `int`
  — but the arithmetic behind that `int` is. One call site in the whole install:
  `vikings = EvalGroup("Player" + idPlayer + "BVikingLord")` in `DATA\AI\GETARMYNEED.VS`,
  used as `if (min < vikings + 3000) min = vikings + 3000` against a `min` drawn from
  {0, 2000, 3000, 5000, 6000} and against stronghold constants of 5000 and 15000. So it is a
  military-strength figure on the same scale as that script's `nEval`, and on the same scale
  as `MilEval(int idPlayer)`, `AllyMilEval`, `EnemyMilEval`,
  `EnemyPlayersEval(int, int*, int*, int*)`, `Squad.Eval`, `GAIKA.Eval` and the `int MinEval`
  parameter of `Unit::BestProtPos` / `Unit::BestMDPos` — eight further entry points sharing
  one unrecovered per-unit strength function. **No per-class strength number exists anywhere
  in the shipped data**: `DATA\CLASSES\*.SC.XML` has no such property, and
  `DATA\COUNTERUNITS.XML`'s 0..100 coefficients (`sim::CounterTable::strength`) are a
  per-matchup damage multiplier, three orders of magnitude away and not a substitute. The
  formula is compiled into `gbr.exe` and the string table does not carry it. Until it is
  read out of the code, `EvalGroup` stays declared-and-unimplemented rather than returning a
  plausible number — a wrong strength here silently changes every AI attack decision.
- **Which object a repeated type-0 alias names.** `5_Great_Loses_German` has eleven
  `<group name="NO_Invisible" type="0">` elements over eleven different objects, and a named
  object is one object, so `GetNamedObj("NO_Invisible")` has eleven candidates. Neither the
  file nor `gbr.exe` says which the original picks — `GROUP_NAME_REUSED` is about a name
  reused *across maps*, not within one. `sim::NamedObjectTable::bind` keeps the first in
  document order, and that is inference. It bites on four maps only, and the one shipped
  script that touches a repeated name uses it as a conversation-actor string
  (`C_Conv.SetActor("NO_Invisible", …)`) rather than as an object, so nothing in the corpus
  distinguishes the readings.
- **Whether `Group(name)` allocates a group or merely names one.** A name has to be usable
  before anything is in it — `mediterranean` map 4 evaluates `Group("Oasis_Guards")` on a
  name no map declares — and `Group()` carries no `Could not find group named` diagnostic,
  unlike `RemoveFromGroup` and `SpawnGroup`, so it certainly does not refuse. Whether the
  original allocated the table entry there or only at the first `AddToGroup` is invisible:
  every shipped sequence that reads such a name also writes it. `sim::GroupTable::intern`
  allocates on lookup, which is the reading that keeps a `Group(…)` handle answering once
  the group fills up.
- ~~**Whether a named-object binding can be rewritten.**~~ **Settled: it can, and
  spawning is what rewrites it.** `CVXGroup::Spawn`'s inner loop
  (`0x005728c1`–`0x0057291c`) walks the named-object table, asks each entry through a
  virtual whether it names the *template* (`vtbl + 0x24`, against the template's id at
  `[templ + 8]`), and on a hit calls `Obj::SetName` (`0x005b6120`) on the **copy** with
  that entry's string. So the name follows the object into play. The
  spawn-from-template mechanism `TemplatesList`, `JustSpawnedList` and the editor's
  `TEMPLATE_UNIT_WITHOUT_GROUP` check pointed at is the `flags` bit 27 machinery above.
  `1_Great_Battles_Zama` is the worked example: `NO_Scipio` names an unspawned template
  at load and the spawned hero after `SpawnGroup("Q_Scipio")` runs in its `seq2`.
  **The narrower question of whether `SpawnNamed` rebinds too is now settled as well: it
  does, and it does not decide it.** `SpawnNamed` (`0x00573c70`) resolves the name to a
  record and hands the spawn itself to `CVXNamedObjQuery::Spawn` (`0x00571d40`), which is
  `CVXGroup::Spawn`'s body for one element: the same `0x08000000` template test, the same
  clone through the object's own `vtbl + 0x30`, the same clear of the template bit on the
  copy, and then the same two walks — the group table at `0x009bdad0` and the named-object
  table at `0x009bdac8` — filing the copy wherever the template was a member. The rebind is
  in the shared spawn, not in either caller, which is why one `World::spawn_from_template`
  answers for both.

  It has a consequence worth stating, because it makes the entry point single-shot: after
  the first `SpawnNamed(n)` the name points at the copy, and a copy is not a template, so a
  second `SpawnNamed(n)` spawns nothing and hands back the invalid handle — with no
  diagnostic, since the "could not find" message is reached only when the name resolves to
  no object at all.
