# The simulation state vector

**Status:** derived from retail desync dumps; structure decoded, several field meanings open
**Source:** `Logs/*/desync.txt` in the retail install — **9 files**, not 14
**Scope:** what the original engine considered *synchronised state*, per runtime class

When two clients in a lockstep session disagreed, HMMSYS wrote a `desync.txt`: a header of
tick/seed/hash blocks followed by a **complete labelled dump of every live object**. That dump is
the engine specifying its own state vector class by class, and it is the highest-confidence
evidence we have for Part 5's object model.

An earlier revision of the plan claimed these files are a replayable command stream. They are not.
They contain no command payloads at all — only a one-line-per-tick pump trace and a full world
snapshot.

## Corpus

Nine `desync.txt` files survive, from five distinct machines. The task brief said 14; the extra
directories under `Logs/` contain only `vx.log` and `screen.bmp`, with no desync dump.

| Directory | Lines | Blocks | With `handle` | Types | `Tick` | `gametime` |
|---|---:|---:|---:|---:|---:|---:|
| `ASUS-Filippo-21.15.30-25.98.53.128` | 10,237 | 877 | 837 | 21 | 2 | 200 |
| `DESKTOP-2JUN9G3-19.52.29-…194` (×2 suffix) | 18,973 | 1,483 | 968 | 26 | 25 | 14,933 |
| `DESKTOP-2JUN9G3-19.52.29-…194` (×3 suffix) | 20,215 | 1,630 | 1,103 | 26 | 28 | 17,489 |
| `DESKTOP-2JUN9G3-19.52.29-…194` (×4 suffix) | 19,618 | 1,392 | 1,025 | 26 | 85 | 63,329 |
| `DESKTOP-2JUN9G3-21.34.07-…113` (×5 suffix) | 29,409 | 1,505 | 1,189 | 31 | 1,392 | 1,107,758 |
| `LAPTOP-TOOG4MUE-21.27.09-172.26.5.136` | 40,641 | 2,198 | 1,747 | 31 | 2,141 | 1,338,613 |
| `NB-013-20.07.17-192.168.1.247-…247` | 34,572 | 1,405 | 1,125 | 29 | 8,569 | 1,735,619 |
| `NB-013-20.34.13-192.168.1.247-…247` | 7,113 | 585 | 537 | 21 | 2 | 200 |
| `NB-013-20.34.13-192.168.1.247` | 7,118 | 595 | 544 | 21 | 2 | 200 |
| **total** | 187,896 | **11,670** | **9,075** | **32** | | |

The "1,103 objects / 31 types" figure quoted in the brief conflates two files: 1,103 is the
handle-bearing count of the `…194×3` dump (which has 26 types); 31 types occurs in the two
late-game dumps. Across all nine files there are **32 distinct runtime types**.

The corpus spans a useful range: three dumps at tick 2 (world as loaded, before any command has
executed) and three past tick 1,000 (heroes, items, ships, catapults present). Types absent from
the early dumps — `CVXItem`, `CVXHero`, `CVXDruid`, `CVXWagon`, `CVXShip`, `CVXCatapult`,
`CVXItemScript`, and four of the six query types — appear only in the late ones, which is why
generalising from a single file is unsafe.

## File layout

```
[DESYNC]        the divergence itself
[GAMETIME]      clock
[CMDPUMP]       command pump counters + pending local/net commands
[SEEDS]         deterministic RNG / id-allocator state
[HASHES]        per-subsystem hashes for this tick
[HASHHISTORY]   one line per pump tick since game start
[CMDEXEC]       always empty in all 9 dumps
[<RuntimeType>] object block, repeated, in ascending handle order
...
```

Object blocks run to the next `[` header. Fields are `key = value` or `key=value` (the engine is
inconsistent, and both forms occur for the same field in different classes). Three lines are
positional rather than keyed: `command queue`, `PathType …`, and the `script=…` queue entries.

## Header blocks

### `[DESYNC]`

```
Tick=0x00000002
Hash=0xc0afe726
ComparedTo=0xaff2b210
```

`Hash` is the local `hashofthehashes` for this tick; `ComparedTo` is the peer's. In all 9 dumps
`Hash == [HASHES].hashofthehashes` and `Hash != ComparedTo`. `Tick` is the pump tick index, not a
frame counter.

### `[GAMETIME]`

| Field | Domain (n=9) | Meaning |
|---|---|---|
| `gamespeed` | 1000 (×8), 999 (×1) | millisecond scale; 1000 = real time |
| `gametime` | hex ms, 200 … 1,735,619 | current game clock in ms |
| `gametimetickstart` | always `gametime + 1` | first ms of the tick being executed |
| `gametimetickend` | `start + 199/200/400/799/800` | last ms of the tick |
| `paused` | 0 (all) | — |

**The tick length is not constant.** `gametimetickend - gametimetickstart` is 400 in the three
tick-2 dumps, 800 in four, 799 in one, and 200 in the longest-running one. `gametime / Tick`
ranges from 100 to 796 across the corpus. This is a negotiated turn length, not a fixed timestep,
and it is confirmed independently by `[HASHHISTORY]` below.

### `[CMDPUMP]`

```
cmdsprocessed = 00000015
Local Commands
Net Commands
```

`cmdsprocessed` is a monotone count of commands executed since game start (0 … 5,072 observed).
`Local Commands` and `Net Commands` are section labels for the *pending* queues; in 8 of 9 dumps
both are empty. The one populated case lists entries as:

```
cmd0 = id(4),player(3)
cmd1 = id(1),player(3)
```

so a queued command carries at minimum a command id and an issuing player. **The payload is not
printed** — this is the reason these files cannot be replayed.

### `[SEEDS]`

| Field | Domain | Meaning |
|---|---|---|
| `cmdidseed` | 0x168 … 0x1a30b | next command id to allocate; grows with `cmdsprocessed` |
| `syncseed` | 32-bit, unrelated across files | the deterministic RNG state |

Both must be part of any save and any join-in-progress handshake. This engine's late join
(`sim/netjoin.hpp`, `netjoin.md`) carries them the only way it carries anything: in the host's
`GameSession::save()`, which holds the generator's state and `World::next_command_id`. The one piece
of synchronised state a save does not hold is the running `netcmds` fold, which lives beside the
world; the join header carries it, and the joiner's stream resumes from it
(`CommandStream::first`/`prior`).

### `[HASHES]`

Nine named sub-hashes plus a roll-up. Observed across the 9 dumps:

| Field | Non-zero in | Comment |
|---|---:|---|
| `slots` | 9/9 | the object table — always non-zero, always distinct |
| `threads` | 9/9 | script coroutine states |
| `netcmds` | 7/9 | zero in the two dumps with `cmdsprocessed = 0` |
| `extrahash` | 1/9 | purpose unknown |
| `pathfinder` | 0/9 | present but never computed in retail |
| `exploration` | 0/9 | ditto |
| `scriptstate` | 0/9 | ditto |
| `aihash` | 0/9 | ditto |
| `hashofthehashes` | 9/9 | equals `[DESYNC].Hash` in all 9 |

That four of the nine channels are permanently zero is itself a finding: the shipped build hashed
**object slots, script threads and the net-command stream**, and nothing else. The pathfinder and
the exploration/fog state were deliberately excluded from the sync check — either because they are
derived, or because they were known to be non-deterministic and were given up on.

### `[HASHHISTORY]`

Three line kinds, over 24,508 lines total:

| Count | Form |
|---:|---|
| 12,237 | `CVXCmdPump::Tick --- Time: <gametime>` |
| 9,653 | `CVXCmdPump::Tick() --- CmdNotUI, idx: <n>` |
| 2,618 | `CVXCmdPump::Tick() --- Cmd: <name>` |

The `Time:` markers are a complete record: their count equals `Tick - 1` in every file
(8,568 for `Tick=8569`, 2,140 for `Tick=2141`, 1,391 for `Tick=1392`), and the last marker equals
`gametime`. So this block is the whole tick history of the session, not a recent window.

Successive `Time:` deltas give the turn length directly:

| Dump | Delta histogram |
|---|---|
| `…194×4` | 800 ×74, 400 ×7, 460 ×1, 669 ×1 |
| `…113×5` | 800 ×1376, 400 ×7, 460 ×1, six one-off values 440–715 |
| `LAPTOP-…` | 799 ×605, 800 ×198, 400 ×11, 785 ×10, 200 ×18, others |
| `NB-013-20.07` | 200 ×8182, 202 ×35, then a long tail 211–224 |

Every session starts at `Time: 200` and runs 400 ms turns for the first seven or so ticks, then
settles on a longer or shorter period. **The tick model is: a variable-length command turn, whose
length the pump adjusts at runtime.** Part 5 must not assume a fixed timestep between commands;
what it can assume is that `gametime` advances in whole milliseconds and that all simulation
stepping is a function of `(gametimetickstart, gametimetickend)`.

`CmdNotUI, idx: n` (n observed 0–8) precedes command execution and appears 0–3 times per named
command; the index correlates with player slot in the samples but is not confirmed.
`Cmd: <name>` names the command by its script-visible label (`trainMPraetorian`, `hireheroM`,
`Barrack Level 2`, `tribute`, `buyfoodforgold`) — the same identifiers used in `.vs` command tables.

## The object model

### Runtime types vs declared `cpp_class`

`docs/data-model.md` establishes that all 845 classes in `DATA\CLASSES\*.SC.XML` resolve to one of
26 `cpp_class` values. The dumps show 32 runtime type names. They are not the same set, and the
relationship is now settled:

**Every `ScriptClass` that appears in any dump resolves, through the class XML inheritance chain,
to a `cpp_class` that is exactly the runtime type name printed for it — 366 of 366 distinct
`ScriptClass` values, zero exceptions, with one systematic rename.**

| Runtime type in dump | Resolved `cpp_class` | Distinct `ScriptClass` observed |
|---|---|---:|
| `CVXDecorObj` | `CVXDecor` | 86 |
| `CVXBuilding` | `CVXBuilding` | 88 |
| `CVXUnit` | `CVXUnit` | 59 |
| `CVXGate` | `CVXGate` | 24 |
| `CVXTownHall` | `CVXTownHall` | 21 |
| `CVXItemHolder` | `CVXItemHolder` | 20 |
| `CVXHero` | `CVXHero` | 19 |
| `CVXBarrack` | `CVXBarrack` | 13 |
| `CVXScriptObj` | `CVXScriptObj` | 12 |
| `CVXOutpost` | `CVXOutpost` | 7 |
| `CVXTavern` | `CVXTavern` | 6 |
| `CVXDruid`, `CVXWagon` | same | 3, 3 |
| `CVXFlyingUnit` | same | 2 |
| `CVXCatapult`, `CVXShip`, `CVXTeleport` | same | 1 each |

`CVXDecorObj` is the runtime name of the class the XML calls `CVXDecor`. This is a naming
inconsistency in the original codebase, not a second class: the 86 `ScriptClass` values printed as
`CVXDecorObj` are all resolved-`CVXDecor` classes (`Crops1`, `CampFire`, `Haystack Small`,
`COLUMN ALONE1`, …), and a `CVXDecorObj` block prints exactly the fields `data-model.md` attributes
to `CVXDecor` — position, art, nothing else. Treat them as one class.

So 16 of the 26 declared classes are directly instantiated in the corpus. **Ten declared classes
never appear**: `CVXFeedback`, `CVXMapObj`, `CVXCatapultShot`, `CVXGhost`, `CVXSacrifice`,
`CVXArea`, `CVXAdvArea`, `CVXAreaEffect`, `CVXDestLock` — and `CVXDecor` under its XML name. The
first three of those are transient (a visual cue, a placement ghost, a projectile in flight); it is
plausible but **unproven** that they are excluded from the sync set entirely. `CVXArea`/`CVXAdvArea`
are adventure-mode triggers and these are all skirmish dumps.

### The 16 runtime types that are not declared classes

Sixteen of the 32 runtime type names are not `cpp_class` values. One of them, `CVXDecorObj`, is the
rename of `CVXDecor` established above. The other fifteen have no `cpp_class` anywhere in
`DATA\CLASSES` (verified by direct scan of `data.pak`: the
only hit for the string `CVXItem` is inside `CVXItemHolder`). They fall into three groups.

**Group A — real state, allocated from the same handle space (8 types).**

| Type | Count | Files | Role |
|---|---:|---:|---|
| `CVXSettlement` | 328 | 9/9 | a town's economy and capture state |
| `CVXHolder` | 331 | 9/9 | a container of garrisoned units |
| `CVXWarehouse` | 328 | 9/9 | a settlement's resource store |
| `CVXItem` | 170 | 3/9 | an inventory item instance |
| `CVXItemScript` | 1 | 1/9 | a script attached to an item |
| `CVXAIHelper` | 9 | 9/9 | exactly one per session |
| `CVXPlayerBonus` | 9 | 9/9 | exactly one per session |
| `CVXPlayerScripts` | 9 | 9/9 | exactly one per session |

These are engine-internal objects with no data-driven class, which is why they have no
`cpp_class`. They are unambiguously **state**, not views: they occupy handles, they are referenced
by other objects' handle fields, and `CVXSettlement` and `CVXItem` print substantial field sets.

The allocation pattern is rigid and is the clearest structural finding in the dumps:

- **A `CVXSettlement` is always immediately followed by a `CVXHolder` at `handle+1` and a
  `CVXWarehouse` at `handle+2`. 328 of 328, in all nine files, no exceptions.** The three are
  allocated as one unit when a settlement is created.
- `CVXHolder` also appears standalone: 3 orphan holders across the corpus, each at `handle+1` of a
  `CVXShip`. Garrisoned `RArcher` units reference them via `holder handle`. So `CVXHolder` is the
  general "this object contains units" container, attached to settlements and ships alike.
- `CVXAIHelper`, `CVXPlayerBonus`, `CVXPlayerScripts` are always three consecutive handles, exactly
  once per dump (e.g. 779/780/781, 640/641/642). They are singletons created together at load.
- `CVXWarehouse` prints **only** its handle and is referenced by nothing. Its contents are printed
  on the `CVXSettlement` block (`gold/food`). Either it is a pure sub-object of the settlement whose
  state the dumper folds into the parent, or the dumper is incomplete for it. **Unresolved** — but
  a warehouse's state must be serialised somewhere, and the settlement is where it is visible.

**Group B — query objects, allocated from the same handle space (7 types).**

| Type | Count | Files | Printed fields |
|---|---:|---:|---|
| `CVXObjsInSightQuery` | 1,338 | 9/9 | `object handle`, `class` |
| `CVXSetOpQuery` | 1,098 | 6/9 | `operation`, `1st query handle`, `2nd query handle` |
| `CVXMapAreaQuery<TCircleArea>` | 200 | 9/9 | `handle` only |
| `CVXPlayerFlagsQuery` | 158 | 6/9 | `type`, `player`, `class` |
| `CVXClassPlayerAreaQuery` | 56 | 9/9 | `handle` only |
| `CVXGroupQuery` | 31 | 6/9 | `handle` only |
| `CVXUnitsInSettlementQuery` | 24 | 6/9 | `handle` only |

These are **persistent, handle-addressed objects, not transient views**, and this matters for
Part 5. Three lines of evidence:

1. `CVXMapAreaQuery`, `CVXClassPlayerAreaQuery`, `CVXGroupQuery` and `CVXUnitsInSettlementQuery`
   print a `handle` that slots into the global ascending sequence like any unit.
2. `CVXObjsInSightQuery`, `CVXSetOpQuery` and `CVXPlayerFlagsQuery` do *not* print a handle — but
   they still occupy one. In the three tick-2 dumps and one other, the number of handle-less blocks
   between two printed handles is **exactly** the gap between them (5, 5, 4 and 5 gap-groups
   matched exactly, zero over-runs). In the busier dumps the match is partial only because
   destroyed objects also leave gaps; there is never a case with more handle-less blocks than the
   gap can hold, in any file.
3. `CVXSetOpQuery` references other queries by handle. Of 2,196 such references across the corpus,
   **2,194 point either at a printed query handle (212) or into a gap below the file's
   maximum handle (1,982)**, and only 2 point above it. A set-op query composes two other queries by handle, and those
   operands are the un-printed query objects.

So the runtime object model is genuinely larger than the declared one: a query is a first-class,
handle-bearing, persistently-registered object that a script holds and re-evaluates. `CVXSetOpQuery`
composes them (`operation` ∈ {0: 611, 1: 299, 2: 188} — three set operations, presumably union /
intersection / difference, **order unproven**).

`CVXObjsInSightQuery` binds an observer to a class filter: its `object handle` resolves to a
`CVXFlyingUnit` (crows and eagles, watching for units and buildings), `CVXItemHolder` (54 each of
`LionF`/`LionM`/`Bear`/`Boar`/`Wolf` and their `*Unit` variants — a lair watching for prey),
`CVXTownHall` or `CVXOutpost`. `class` takes 14 values, mixing concrete classes (`Crow`, `Hen`)
with abstract ones (`Unit`, `Building`, `Object`) — so queries filter on the class *tree*, not on
leaf classes.

`CVXPlayerFlagsQuery` carries `type` ∈ {1, 2}, `player` ∈ {0,1,2,3,4,14,15} and a class filter
(`BaseMage`, `Unit`, `Hen`, `Animal`, `Peaceful`, `Object`, `Military`, `Sentry`, `Catapult`,
`Outpost`). `type` is **unknown**; only one instance of `type=1` exists in the corpus.

**Group C — none.** 16 declared + 8 state + 7 query + 1 rename = 32. There is no third group:
every runtime type in the dumps is either a declared `cpp_class`, an engine-internal state object,
or a query. None is a pure view.

## Handles

**The handle space is flat, global, 16-bit, sparse, and monotonically allocated.**

| Property | Evidence |
|---|---|
| One space for all types | Blocks appear in strictly ascending handle order in all 9 files; no handle is used by two types in the same file (0 collisions in 9,075 blocks) |
| Includes queries | See Group B above |
| Starts at 0 | Every dump's lowest handle is 0, and it is always a `CVXSettlement` |
| 16-bit | `65535` (decimal) and `ffff` (hex) are the null sentinel; max observed live handle is 21,754 |
| Not slot indices | Live count 1,747 with max handle 20,505; 1,125 with max 21,754. If handles were reused slots, max would track the live count |
| Stable identifiers | Density falls monotonically with game age: 0.93 / 0.89 / 0.89 at tick 2, 0.60 / 0.51 mid-game, 0.085 / 0.065 / 0.052 late. Gaps accumulate and are never refilled |

Density by dump (distinct handles ÷ `max+1`):

| Tick | 2 | 2 | 2 | 25 | 28 | 85 | 1,392 | 2,141 | 8,569 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Density | 0.929 | 0.886 | 0.892 | 0.596 | 0.606 | 0.503 | 0.065 | 0.085 | 0.052 |

Handles are **allocated from a monotonically increasing counter and never reused within a
session**. That is the single most important consequence for Part 5: an object reference is a
stable id, so a save file can store raw handles, and a destroyed object's handle can safely be
treated as permanently dead (a dangling reference resolves to nothing, not to a different object).

The counter is 16-bit. The longest dump reached 21,754 after 8,569 ticks (≈29 minutes of game
time), so a very long game could in principle exhaust it; whether the original wrapped, and what
it did if so, is **unknown**. Part 5 should use a 32-bit id with a 16-bit compatibility view rather
than reproduce the risk.

Even the tick-2 dumps have 12–14 gaps, so a handful of objects are created and destroyed during
map load itself.

### Handle-typed fields and what they resolve to

Measured over the whole corpus:

| Field | Null form | Resolves to |
|---|---|---|
| `holder handle` | `65535` | `CVXHolder`, always (1,217 non-null, 0 exceptions) |
| `hero handle` | `65535` | `CVXHero`, always (865 non-null, 0 exceptions) |
| `damage magic handle` | `65535` | never non-null in the corpus — **meaning unknown** |
| `target handle` | `ffff` (hex!) | `CVXUnit` or `CVXTownHall`; 1 unresolved (a destroyed target) |
| `owner handle` (`CVXItem`) | — | `CVXUnit` 138, `CVXItemHolder` 25, `CVXBuilding` 3, `CVXHero` 1 |
| `object handle` (sight query) | — | always a live object |
| `1st`/`2nd query handle` | — | a query object, usually un-printed |
| `PathType handle` | — | a live object being pathed to |

**`target handle` is printed in hex while every other handle field is printed in decimal.** A
parser must special-case it. `ffff` and `65535` are the same value.

## Per-type field specification

Field order below is the dump's own order, which is stable per type. "n" is the number of blocks
observed across all 9 files. `x = A, y = B` is printed as a single line and is two fields;
`Anim = N, x = A, y = B` is one line and three fields.

### Common object header

Every type derived from `CVXDecor` starts with the same five fields:

| Field | Type | Domain | Meaning |
|---|---|---|---|
| `handle` | u16 | 0 … 21,754 | stable object id (see above) |
| `ScriptClass` | string | 366 distinct | the class id from `DATA\CLASSES` |
| `x`, `y` | i16 | 51 … 16,309 / 65 … 16,140, or `-1, -1` | world position; **`(-1,-1)` means "inside a holder"** |
| `SyncFlags` | u32 hex | 49 distinct | see next section |
| `Anim` | i32 | 12 distinct: 0,4,8,12,16,17,18,19,28,29,30,31 | animation state |
| `Anim.x`, `Anim.y` | i32 | −? … 21,343 | see below |

`(-1,-1)` and garrisoning agree almost perfectly: of 1,225 objects at `(-1,-1)`, 1,217 have a
non-null `holder handle`; the 8 that do not are 6 units and 2 wagons, presumably mid-transition.
Of the 6,355 objects with a real position, none has a holder. **A garrisoned object's position is
not stored** — it is `(-1,-1)`, and its location is its holder's.

The map coordinate space is bounded by 16,383 (2¹⁴−1); the highest observed coordinate is 16,309
and `PathType point` values reach exactly 16,383. This is consistent with the 16,384-unit map
extent in `docs/formats/map.md`.

`Anim` correlates strongly with the running method: 28 for every static object and every idle one
(5,760 of 7,580), 0 for movers, 4 for `engage`/`attack`, 8 only for the dying (`action=8`), 12 for
`stand_position`/`talk`, 19 for `capture`. It is the current animation index.

`Anim.x` / `Anim.y` are a vector and are **not fully explained**. What is established:

- For every static object — all 1,982 buildings, all 1,111 decor, all gates, outposts, taverns,
  teleports — it is exactly `(0, 1)`, never anything else.
- For moving units it points roughly along the direction of travel: of 449 movers with `Anim=0` and
  a `PathType point`, the angle between `(Anim.x, Anim.y)` and (target − position) is under 15° for
  267, under 30° for 393, and over 90° for only 8.
- Its magnitude scales with the class: `Eagle` mean 113, `Deer` 62, `IMilitiaman` 82, villagers
  38–52. That is consistent with a per-tick velocity.
- For garrisoned units at `(-1,-1)` it holds values in map-coordinate range (`6752, 6429`), i.e. it
  looks like stale or reinterpreted memory in that state.

The best-supported reading is a velocity or step vector, **but the angular scatter and the
static-object `(0,1)` both argue against a plain velocity** (a stationary object should be `(0,0)`).
Marked **unknown**; do not build on it.

### `CVXUnit` — n=3,159, 9/9 files, 8 shapes

The full unit block, in order. Field counts below are over the 3,432 blocks of the whole unit
family (`CVXUnit` 3,159 + `CVXFlyingUnit` 192 + `CVXDruid` 47 + `CVXHero` 31 + `CVXShip` 3) where a
field is shared, and over 3,159 `CVXUnit` blocks where the row says so:

```
handle | ScriptClass | x,y | SyncFlags | Anim,x,y
method | health | stamina | action
command queue
  script=<path> id=<n> cmd=<name><params>       (0–5 entries)
  PathType point=(x,y) | PathType handle=<h> | PathType formation sample   (optional)
  Min Range | Range                              (only with point/handle)
hero handle | holder handle | damage magic handle
food | squad | experience | level | unit flags
last attack time | target handle | number of attacks | danger
```

| Field | Domain | Meaning |
|---|---|---|
| `method` | `CVXUnit`: 18 values; `idle` 1,850, `form_move` 339, `guard` 213, `patrol` 203 | the currently-running script method, matching a `<method sig=…>` in the class XML |
| `health` | `CVXUnit`: 83 values, 0 … 15,000 | current hit points |
| `stamina` | `CVXUnit`: 10 values; 10 dominant, then 0 and 1–9 | current stamina |
| `action` | 6 values; see table below | coarse activity state |
| `food` | 0 … 20 (22 values) | carried/consumed food |
| `squad` | `<n>(<p>)`, `CVXUnit`: 229 distinct pairs | squad index and owning player — see below |
| `experience` | 0 … ~74 | experience points |
| `level` | 1 … 24; 1 is 1,545 of 3,432 | unit level |
| `unit flags` | 20 distinct hex, bits 16–26 only | see below |
| `last attack time` | 0 … 1,738,000, in `gametime` ms | timestamp of last attack |
| `target handle` | hex, `ffff` = none | current attack target |
| `number of attacks` | 0 … 77 | attacks delivered (against the current target — unproven) |
| `danger` | 0 … 6 | threat assessment; 0 in 3,166 of 3,432 |
| `hero handle` | `65535` or a `CVXHero` | the hero whose army this unit belongs to |
| `holder handle` | `65535` or a `CVXHolder` | garrison container |
| `damage magic handle` | `65535` in all 3,432 cases | **unknown** — presumably an active damage-over-time effect |
| `Min Range`, `Range` | 0 … 425 / 0 … 510 | path arrival tolerance; present only with a point/handle path |

`action` values, cross-tabulated against `method` over 6,469 blocks of all types:

| `action` | n | Co-occurring methods |
|---:|---:|---|
| 0 | 4,473 | `idle`, `guard`, `research`, `train`, `hirehero`, `invest`, `move`, `engage`, `disappear` |
| 2 | 92 | `engage` (90), `attack` (2) |
| 3 | 1,299 | `form_move`, `patrol`, `moveenter`, `capture`, `enter`, `unload`, `attach`, `move` |
| 4 | 321 | `idle`, `stand_position`, `lead`, `talk` |
| 5 | 19 | `capture` |
| 6 | 250 | `idle` — only on `CVXScriptObj` and `CVXItemHolder` |
| 8 | 15 | `method` is **empty**, `health` is 0 or near 0 |

`action=8` with an empty method is the dying/dead state: all 15 cases are units with `health` 0–3
and `Anim=8`. `action=3` means "moving"; it lines up with `SyncFlags` bit 17 (below).
`action` is a small enum but its exact names are **unknown**; 1 and 7 never occur.

`squad=<n>(<p>)`: the first component is a squad index (0 … 91, 85 distinct; 0 means *no squad*),
the second is the owning player. **`p` equals the player bit set in `SyncFlags` in all 2,972 cases
where `n != 0`, with zero exceptions.** All 417 disagreements have `n == 0`, where both components
print 0. Squad indices are per-player: index 4 exists simultaneously for players 0, 2, 3 and 14 in
the same dump. `p` is therefore derived, not independent state.

`unit flags` is a **separate word from `SyncFlags`** — it is not a subset (2,597 of 3,432 have bits
`SyncFlags` lacks), and 76 distinct `(SyncFlags, unit flags)` pairs occur. Only bits 16–26 are ever
set; the low 16 bits are always zero. Bit correlations, n=3,432:

| Bit | n | Correlates with |
|---:|---:|---|
| 16 | 1,088 | `form_move`, `moveenter`, `capture` — movement orders |
| 17 | 1,281 | `form_move`, `engage`, `capture` |
| 18 | 417 | `guard` (213) and `patrol` (203) — sentries, near-exclusively |
| 19 | 1,069 | `form_move`, `engage`, `capture` |
| 20 | 204 | `moveenter`, `enter`, `build_catapult`, `attach` — entering something |
| 21 | 141 | `idle`, `lead` |
| 22 | 148 | `CVXFlyingUnit`, exclusively (all 148 flying units) |
| 23 | 445 | `idle`, `moveenter`, `disappear`, `talk` |
| 25 | 559 | mixed |
| 26 | 861 | `idle`, `guard`, `patrol`, `moveenter` |

Bits 18 and 22 have clean single meanings (sentry duty; is-flying). The rest are **unknown**. Do
not guess a layout.

### `CVXHero` — n=31, 4/9 files

Identical to `CVXUnit`, with no additional fields. `hero handle` is `65535` on every hero (a hero
does not belong to another hero's army). `holder handle` is non-null on 16 of 31. `level` reaches
24 and `experience` 74. The distinguishing state is entirely in `SyncFlags` bit 24.

### `CVXDruid` (n=47), `CVXWagon` (n=52), `CVXShip` (n=3), `CVXFlyingUnit` (n=192)

All print the `CVXUnit` block. Two add fields:

| Type | Extra fields | Domain |
|---|---|---|
| `CVXWagon` | `resource type`, `resource amount` | `food` (49) / `gold` (3); 0 … 124 |
| `CVXFlyingUnit` | — | but `hero`/`holder`/`damage magic` are always null, `food`/`experience`/`danger` always 0, `level` always 1 |

`CVXWagon` never prints `PathType` despite 49 of 52 having `SyncFlags` bit 17 set — the only
systematic exception to that correlation.

`CVXShip` carries a `CVXHolder` at `handle+1` for boarded units.

### `CVXBuilding` — n=1,982, 9/9 files

```
handle | ScriptClass | x,y | SyncFlags | Anim,x,y
method | health | stamina | action
command queue
  script=…                                       (1–2 entries)
```

| Field | Domain | Note |
|---|---|---|
| `method` | `idle` (1,963), `research` (10), `hirehero` (7), `teutonrider_school` (1), `trainex` (1) | |
| `health` | exactly 3 values: 5000, 1500, 1000 | buildings are at full health in every dump |
| `stamina` | 20, always | |
| `action` | 0, always | |
| `Anim` | `28, x = 0, y = 1`, always — 1,982 of 1,982 | |

Buildings carry **no** owner-side state beyond `SyncFlags`. Note that `health` never takes a
damaged value anywhere in the corpus, which is a gap in the evidence, not a property of the engine.

### `CVXTownHall` — n=191, `CVXOutpost` — n=112, `CVXBarrack` — n=73, `CVXTavern` — n=35, `CVXGate` — n=140, `CVXTeleport` — n=14, `CVXCatapult` — n=4

All print exactly the `CVXBuilding` block, with no extra fields. Their distinguishing state lives
elsewhere:

- A **town hall's economy is on the `CVXSettlement` object**, not on the town hall. This confirms
  `data-model.md`'s inference from the class properties, from the opposite direction.
- `CVXGate` prints no open/closed field. Gate state must be encoded in `method` (always `idle`
  here) or `Anim` (always 28). **Not observable in this corpus.**
- `CVXTownHall` is the only building type with `method` values `trainpeasant` and `broken`, and the
  only one whose `health` varies (5000, 15000, 1015, 1824) and whose `stamina` varies (20, 80, 46).
- `CVXCatapult` has `method` ∈ {`autofire`, `idle`} and one instance at `health=1`.

### `CVXDecorObj` — n=1,111, 9/9 files

```
handle | ScriptClass | x,y | SyncFlags | Anim,x,y
```

Five fields, nothing else. No method, no health, no script. `SyncFlags` is `0x80000000` for all
1,111 — the only type with a constant `SyncFlags` and the only one with no owner. `Anim` is
`28, x = 0, y = 1` for all 1,111.

**Decor carries no mutable state at all.** In Part 5 it does not belong in the simulation's update
loop; it belongs in the scene. The only reason it is in the sync set is that it occupies handles
and passability.

### `CVXScriptObj` — n=205, `CVXItemHolder` — n=229

Both print the `CVXBuilding` shape. Distinguishing observations:

| | `CVXScriptObj` | `CVXItemHolder` |
|---|---|---|
| `method` | `idle` always | `idle` always |
| `health` | 100 always | 100 always |
| `stamina` | 0 always | 0 (228), 58 (1) |
| `action` | 6 (187), 0 (18) | 0 (206), 6 (23) |
| `script` | `anim.vs` mostly | `anim.vs`, `object_idle.vs` |

`action=6` occurs only on these two types. Given both are script-driven props running `anim.vs`,
6 plausibly means "running a looping animation script", **unproven**.

### `CVXSettlement` — n=328, 9/9 files

The richest non-unit block, and the one Part 5 must get right.

```
handle
ID
class
capture/attack/gold/food
pop/maxpop
gold/food
first to repair
last tower fire time
last unit exit time
last capture query time
exit interval
loan
```

| Field | Domain (n=328) | Meaning |
|---|---|---|
| `ID` | 0 … 42, **dense and ascending within each file**, one per settlement | settlement array index — *not* a handle |
| `class` | 31 values | the `ScriptClass` of the settlement's anchor object; present as a live object in the same dump in 328/328 cases |
| `capture/attack/gold/food` | 4 ints on one line, 7 combinations | see below |
| `pop/maxpop` | 2 ints, 49 combos; `0 0` in 224 | current and maximum population |
| `gold/food` | 2 ints, 67 combos; up to `10014 1000` | the settlement's stored resources — **this is the warehouse's content** |
| `first to repair` | 0 … 40, 11 values | **unknown**; plausibly a handle or index of the next building to repair |
| `last tower fire time` | 0 or a `gametime` ms value, 15 values | tower cooldown |
| `last unit exit time` | 0 … 59,757, 94 values | spawn-rate throttle, paired with `exit interval` |
| `last capture query time` | 0 in all 328 | **unknown**, never used in retail |
| `exit interval` | 20 (324), 1000 (4) | minimum ms between unit exits |
| `loan` | 0 in all 328 | tavern loan balance; never non-zero in the corpus |

`capture/attack/gold/food` combinations: `1 1 0 0` (169), `1 1 0 24` (69), `1 1 24 0` (33),
`0 0 0 0` (30), `1 0 0 0` (21), `0 1 0 0` (4), `1 1 12 24` (2). The first two are booleans matching
the `can_be_captured` / `can_be_attacked` class properties; the last two are small integers that
look like production *rates* (`produces_gold` / `produces_food`), not stock. Stock is the separate
`gold/food` line.

`class` naming an anchor object rather than a town hall is important: settlement `class` values
include `ROutpost`, `Teleport_1`, `RCatapult`, `Ruins1` and `Stonehenge1` alongside the town halls
and shipyards. **A settlement is not the same thing as a town hall** — it is a separate object that
an anchor building owns, and outposts, teleports, catapults and ruins each own one too. The
settlement count exceeds the town-hall count in every dump (e.g. 39 vs 19).

### `CVXItem` — n=170, 3/9 files

```
handle | type | owner handle | usecount | customdata
```

| Field | Domain | Meaning |
|---|---|---|
| `type` | 12 names: `God's Gift`, `Spoils of War`, `Snake skin`, `Healing water`, `Eagle feather`, `Rye spikes`, `Poison Mushroom`, `Damage charm`, `Healing herbs`, `Finger of death`, `Fur gloves of health`, `Ring of Power` | the item kind, by localised-looking name |
| `owner handle` | `CVXUnit` 138, `CVXItemHolder` 25, `CVXBuilding` 3, `CVXHero` 1 | who holds it |
| `usecount` | 0 (153), 2000, 200, 500, 6, 1 | charges remaining |
| `customdata` | 0 in all 170 | **unknown**, never used in retail |

`type` being a **string, not an id**, is notable: item identity in the sync set is by name.

### `CVXItemScript` — n=1, 1/9 file

```
item handle=1114, script type=7
```

Two fields on one line. A single instance in the whole corpus. Binds a script to an item;
`script type` domain **unknown** (one sample, value 7).

### `CVXHolder`, `CVXWarehouse`, `CVXAIHelper`, `CVXPlayerBonus`, `CVXPlayerScripts`

Each prints `handle` and nothing else — 331, 328, 9, 9 and 9 instances respectively. Their state is
either folded into a related object (warehouse → settlement) or simply not dumped. **This is the
largest gap in the evidence:** the AI helper, the per-player bonus table and the per-player script
state are all part of the sync set (they hold handles, they are inside the hashed slot table) but
the dumper prints none of their fields.

## `SyncFlags`

**Resolved, with two bits left open.** 49 distinct values over 7,580 objects. The word is
structurally two halves.

### Low 16 bits — owner player, as a one-hot bitmask

Only 8 values ever occur, and **every one is either zero or a single bit**:

| Value | n | Bit | Owner |
|---|---:|---:|---|
| `0x0000` | 1,111 | — | unowned — `CVXDecorObj` and nothing else |
| `0x0001` | 1,028 | 0 | player 0 |
| `0x0002` | 1,008 | 1 | player 1 |
| `0x0004` | 926 | 2 | player 2 |
| `0x0008` | 604 | 3 | player 3 |
| `0x0010` | 321 | 4 | player 4 |
| `0x4000` | 2,381 | 14 | player 14 |
| `0x8000` | 201 | 15 | player 15 |

Three independent confirmations that this is the owner and not, say, a visibility mask:

1. It matches `squad=<n>(<p>)`'s player component in **2,972 of 2,972** cases where the unit is in
   a squad. Every disagreement is a unit with no squad, where the dump prints `0(0)`.
2. `CVXPlayerFlagsQuery.player` takes exactly the same value set: {0, 1, 2, 3, 4, 14, 15}.
3. A visibility mask in a 5-player game would routinely have several bits set. Not one object in
   7,580 has two bits set in the low half.

Players 14 and 15 are engine-reserved slots. Player 14 owns the wildlife (`Deer` 259, `Hen` 227,
`Crow` 147, `Fish` 78, `Wolf` 75, `Bear`, `Boar`, `Eagle` 45), the `FakeTower` pseudo-objects (288),
`CVXScriptObj` props and all 200 owned `CVXItemHolder`s — plus, in the early dumps, the map's
pre-placed garrison units (`CMaceman` 143, `RLiberatus` 120, `TTeutonArcher` 82). Player 15 owns a
smaller set: `IHouse1/2/3`, `IVillage`, `RShipyard1–4`, fountains, wells, boulders, obelisks and
`IVillagerAmbient` units. The precise distinction between the two neutral slots is **unknown**;
"14 = hostile/wild neutral, 15 = passive/scenery neutral" fits the data but is not proven.

### High 16 bits — cached category and status flags

Bit occurrence over 7,580 objects:

| Bit | Mask | n | Set on | Reading |
|---:|---|---:|---|---|
| 31 | `0x80000000` | 7,580 | everything | object is live / slot in use |
| 26 | `0x04000000` | 140 | `CVXUnit` only, and only `*VillagerAmbient` classes and `OldManE` | ambient civilian — **inferred** |
| 24 | `0x01000000` | 31 | `CVXHero`, all 31, and nothing else | is a hero |
| 23 | `0x00800000` | 2,551 | `CVXBuilding`, `TownHall`, `Outpost`, `Barrack`, `Tavern`, `Gate`, `Catapult`, `Teleport` | is a building |
| 22 | `0x00400000` | 3,484 | `CVXUnit`, `Hero`, `Druid`, `Wagon`, `Ship`, `FlyingUnit` | is a unit |
| 21 | `0x00200000` | 24 | only `THuntress` | **inferred**: invisible/hidden (`THuntress` is the class with `can_be_invisible`) |
| 17 | `0x00020000` | 1,161 | movers | has an active path |

Bits 22 and 23 form an **exact partition** of the class tree: 3,484 + 2,551 = 6,035, and the
remaining 1,545 objects are exactly the `CVXDecorObj` (1,111) + `CVXItemHolder` (229) +
`CVXScriptObj` (205) which are neither. They are cached type-category bits, almost certainly used
to dispatch queries without a virtual call.

Bit 17 against the presence of a `PathType` line:

| | has `PathType` | no `PathType` |
|---|---:|---:|
| bit 17 set | 1,112 | 49 |
| bit 17 clear | 0 | 6,419 |

Perfect in one direction. All 49 exceptions are `CVXWagon` in `method=unload`, which appears to
path without printing one.

Bits 0–13 and 16, 18, 19, 20, 25, 27, 28, 29, 30 are **never set** in any of the 7,580 objects.
That does not mean they are unused — it means this corpus does not exercise them. Bit 18 is
known now: it marks a destination lock (`CVXDestLock`), an object that clears bit 31 and is
therefore never printed; see `PathType` below.

## Command queue

Every object with a `method` prints a `command queue` header followed by 0–5 script entries:

| Length | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---:|---:|---:|---:|---:|---:|
| n | 15 | 6,263 | 117 | 3 | 6 | 65 |

An entry is:

```
script=data/subai/unit_move_enter.vs id=106820 cmd=Nonepoint = (1277,3233)
script=data/subai/wagon_unload.vs   id=105250 cmd=Nonehandle = 38
script=data/subai/hero_capture.vs   id=2853   cmd=capturehandle = 411
script=data/subai/research.vs       id=520    cmd=Barrack Level 1
```

Three fields plus an inline parameter list printed with **no separator** before it — `cmd=None`
immediately followed by `point = (…)` or `handle = …`. A parser must treat the parameter as
starting at the last ` = ` on the line, or match `cmd=(\S*?)(handle|point) = `.

| Component | Domain | Meaning |
|---|---|---|
| `script` | 61 distinct `.vs` paths | the script running this queue entry |
| `id` | 0 … 107,170, monotone within a file | a per-entry allocation id, from the same counter family as `cmdidseed` |
| `cmd` | `None` (6,735) + 36 named commands | the command that produced this entry |
| params | `handle = <h>`, `point = (x,y)`, or absent | the command's argument |

`cmd=None` means the entry is a script the engine started itself (idle, ambient, sentry behaviour);
a name means it came from a player or AI command (`capture`, `attach`, `trainMHastatus`, `enter`,
`hireheroM`, `investR`, `Barrack Level 1`; 36 distinct names in the corpus). Research commands carry human-readable multi-word names
with spaces, so `cmd` is not a token — it can contain spaces, which is what makes the
no-separator concatenation ambiguous.

The 15 zero-length queues are exactly the 15 dying units (`action=8`).

The **queue is state**: script path, entry id, command name, and one argument, per entry, in order.
It has to be serialised, and the `id` has to come from a deterministic counter.

## `PathType`

A discriminated union with three arms, printed after the queue:

| Arm | n | Followed by `Min Range`/`Range` |
|---|---:|---|
| `PathType point=(x,y)` | 716 | yes |
| `PathType formation sample` | 354 | no |
| `PathType handle=<h>` | 37 | yes |

`point` is a destination in map coordinates (values up to 16,383). `handle` is a moving target
object. `formation sample` prints no data at all — the formation's target is derived from the
hero's formation state, which is **not in the dump**. That is a hole: a formation move cannot be
reconstructed from a desync dump alone.

`Min Range` (0, 2, 245, 425 + 1 more) and `Range` (0, 15, 17, 200, 495, 510 + 2 more) are arrival
tolerances — the unit stops when it is between `Min Range` and `Range` of the target. Values
cluster at class-specific numbers, consistent with the `radius`/`selection_radius` properties.

### What the `PathType` line does not print: the path media

The line is `CVXPathRetry`'s — its `kind`, destination and ranges are exactly the fields
its save routine names (`kind`, `destx`, `desty`, `dest_handle`, `range`, `minrange`). The
route it is walking is a separate object, its *path media*: a `CVXSPath` (the route cut
into steps of the walk animation's `step`) wrapped in a `CVXPathCoop`, which is what keeps
units out of each other's way. Both are types in the executable's persistent factory
(codes 14 and 15), so a save carries them; neither prints here, neither is an object in
the slot table, and `pathfinder` is zero in every dump. The cooperative layer's only field
is `retrytime`, the 0/50/75/100 wait a blocked unit climbs before it gives way. This engine
keeps all of it on `MoveState`, **saved and not hashed** — see `sim/avoidance.hpp` for the
mechanism and its addresses.

### Destination locks: objects kept out of the sync set on purpose

`CVXDestLock` is a real world object — class `DestLock` in `DATA\CLASSES`, a factory type
(code 12) — and **none appears in any of the nine dumps**, although the executable makes one
at the end of every `Goto` route and several around every doorway and tree whose entity
declares `<point>` types 9, 10 or 11. The reason is in its constructor (`0x0040a580`): it
sets bit 18 of the `SyncFlags` word and **clears bit 31**, the bit set on all 7,580 printed
objects. Bit 31 is the sync set's membership, not merely "the slot is in use".

Two things follow. A lock holds a handle, so it is a plausible source of the 12–14 gaps
in the handle sequence the tick-2 dumps already show; that is not proven. And bit 18 has a
meaning after all — it is the lock's own category bit, the one the *is this spot free*
predicate (`0x004094d0`) tests to recognise a lock — though no printed object carries it. This engine keeps the ownerless locks as a list on the
movement system rather than as objects, so they take no id; the owned ones are read and not
built, for the reason `sim/avoidance.hpp` gives.

## What this means for Part 5

### The object model

1. **One flat, monotonic, non-reusing id space for every simulated thing** — units, buildings,
   decor, settlements, holders, warehouses, items, singletons *and query objects*. Not a per-type
   table. Not slot indices. This is the first architectural decision and the dumps settle it.
2. **The runtime class is a pure function of `ScriptClass`.** 366 of 366 classes in the corpus
   resolve, through XML inheritance, to exactly the runtime type the engine printed. Part 5 can
   build a `ScriptClass → runtime class` table at load time from `DATA\CLASSES` and never revisit
   it. `CVXDecor` and `CVXDecorObj` are the same class under two names.
3. **The declared 26 are not the whole model.** Eight more engine-internal state classes
   (`CVXSettlement`, `CVXHolder`, `CVXWarehouse`, `CVXItem`, `CVXItemScript`, `CVXAIHelper`,
   `CVXPlayerBonus`, `CVXPlayerScripts`) and seven query classes have to exist, and they occupy
   handles alongside the data-driven ones.
4. **Queries are persistent objects, not calls.** A script asks "what units of player 3 are in this
   circle" once, gets a handle, and re-reads it. `CVXSetOpQuery` composes two query handles.
   Building this as a stateless `find()` API will diverge from the original — the query objects are
   in the hashed slot table, so their allocation order affects the sync hash.
5. **Composite objects are allocated as consecutive handle runs.** Settlement+Holder+Warehouse
   (328/328), Ship+Holder (3/3), AIHelper+PlayerBonus+PlayerScripts (9/9). Allocation order is part
   of the observable state.

### What must be serialised

Per object: handle, class, position (or the `(-1,-1)` garrisoned marker), `SyncFlags`, animation
state, current method, health, stamina, action, and the whole command queue with its entry ids.
Units add the ownership/army links (`hero`, `holder`), inventory (`food`, items by owner handle),
progression (`experience`, `level`), combat memory (`last attack time`, `target handle`,
`number of attacks`, `danger`), `unit flags`, squad membership, and the active path.

Globally: `gametime`, the tick bounds, `cmdidseed`, `syncseed`, `cmdsprocessed`, the pending local
and net command queues, and every settlement's economy.

### What must be deterministic

The three things the original actually hashed: **the object slot table, the script thread states,
and the net command stream**. Everything else — pathfinder internals, exploration/fog, AI
scratch — was excluded from the hash by the shipped build, and the four zeroed hash channels say so
explicitly. That is a strong hint about where the determinism budget should go, and equally a
warning: those subsystems must not be allowed to feed values back into hashed state.

`cmdidseed` and the command-queue entry `id` come from the same counter family and must advance
identically on every client.

Unit-versus-unit avoidance is the clearest case of a subsystem outside the hash deciding
things inside it. Its state is path media and destination locks, both outside the sync set
(see `PathType` above), but *what it decides* is not: where a blocked unit stands, and — for
a unit blocked by another that is still moving — a draw of `rand(1, 7)` from the synchronised
generator that the `[SEEDS]` block's `syncseed` then carries. So the positions and the
generator are hashed, and the wait, the step and the sidestep that produced them are not.

### What is derived, not stored

- The `(p)` in `squad=<n>(<p>)` — it is the owner, already in `SyncFlags`.
- `SyncFlags` bits 22, 23, 24 — cached class-category bits recomputable from the class.
- `SyncFlags` bit 17 — "has a path", recomputable from whether a path exists.
- `Anim` — recomputable from the running method, though the engine clearly caches it.
- A garrisoned object's position — it is its holder's.
- `CVXSettlement.class` — the anchor object's `ScriptClass`.
- Very likely `CVXWarehouse`'s contents, which are printed on the settlement.

### What is *not* in the simulation loop

`CVXDecorObj` — 1,111 objects, 9.5% of every dump — has five fields and no mutable state
whatsoever. It needs a handle and a footprint, and nothing else.

## What is still unknown

- **`Anim.x` / `Anim.y`.** Direction correlates with travel (393 of 449 movers within 30°) and
  magnitude scales with class speed, but static objects are `(0,1)` rather than `(0,0)` and
  garrisoned objects hold map-coordinate-sized garbage. Not a plain velocity. Unresolved.
- **`unit flags`.** 20 distinct values, bits 16–26. Bit 18 = sentry (guard/patrol) and bit 22 =
  flying are clean. The other nine bits are not. The word is provably distinct from `SyncFlags`.
- **`SyncFlags` bits 21 and 26.** Bit 21 appears on 24 objects, all `THuntress`; bit 26 on 140, all
  ambient villagers. Single-class evidence is not enough to name a bit.
- **`action` enum names.** Six values observed (0, 2, 3, 4, 5, 6, 8); 1 and 7 never occur. The
  method cross-tabulation constrains them but does not name them.
- **`damage magic handle`** — null in all 3,432 unit blocks. Never exercised.
- **`customdata`** (`CVXItem`), **`loan`** and **`last capture query time`** (`CVXSettlement`) —
  zero in every instance. Never exercised.
- **`first to repair`** (`CVXSettlement`) — 11 values, 0–40. Could be a handle, a building index or
  a countdown. No correlation established.
- **`CVXItemScript.script type`** — one sample, value 7.
- **`CVXSetOpQuery.operation`** — three values with a clear frequency ordering (0: 611, 1: 299,
  2: 188), presumably union/intersect/difference, but the mapping is unproven.
- **`CVXPlayerFlagsQuery.type`** — {1, 2}, with exactly one instance of `type=1` in the corpus.
- **The fields of `CVXHolder`, `CVXWarehouse`, `CVXAIHelper`, `CVXPlayerBonus`,
  `CVXPlayerScripts`.** All five print only a handle. Their state is real (they are inside the
  hashed slot table) and entirely invisible here. **This is the largest single gap.**
- **Gate open/closed state.** `CVXGate` prints nothing that varies except `SyncFlags` owner and
  handle. All 140 are `method=idle`, `health=5000`, `Anim=28,0,1`.
- **Whether the 10 never-instantiated declared classes are excluded from the sync set** or simply
  absent from these nine skirmish sessions. `CVXFeedback`, `CVXMapObj` and `CVXCatapultShot` are
  transient and plausibly excluded; `CVXArea`/`CVXAdvArea` are adventure-mode only.
- **Handle exhaustion.** The counter is 16-bit and reached 21,754 in the longest dump. Whether the
  original wrapped, reused, or failed is not observable.
- **`CmdNotUI, idx:` semantics** in `[HASHHISTORY]` — indices 0–8, appearing 0–3 times per named
  command. Correlates loosely with player slot; not established.
- **Damaged buildings.** No building in any dump has `health` below its class maximum, so the
  damage/repair state fields (if any exist beyond `health`) are unobserved.

## Method

All figures were produced by parsing all nine files programmatically and aggregating; nothing in
this document is generalised from a single file or a read sample. The `ScriptClass → cpp_class`
verification was done by reading `DATA\CLASSES\*.SC.XML` directly out of `Packs/data.pak` and
resolving the `cpp_class` attribute through the `parent` chain, then joining against every
`ScriptClass` observed in the dumps.
