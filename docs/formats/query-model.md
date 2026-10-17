# The query model

**Status:** read from `gbr.exe` for the host audit's eleventh round; the engine models the
cache, the refresh periods, the read protocol, the set-operation refresh and the waits'
cadence (`engine/core/src/sim/world.cpp`, `sim/wait.cpp`). Interning, the reference count and
the live aliases are described here and their state in the engine is given in each section.
**Not a file format.** Nothing here is on disk; it is the runtime model behind the `Query`
values the shipped scripts hold, kept beside the host API ([`vs-host-api.md`](vs-host-api.md))
because every query entry point answers through it.

Prose only, with addresses so each claim can be checked; [`../legal.md`](../legal.md) rule 2.

## A query is an object with a cache

Every query constructor the scripts reach (`ObjsInSight`, `ObjsInCircle`, `AreaObjs`,
`ClassPlayerObjs`, `EnemyObjs`, `UnitsInSettlement`, `Union`, ...) answers a handle to a
`CVX...Query` object that persists. All the cached kinds share a base laid out as follows
(offsets from the object; all read in the constructors 0x00571ff0 and 0x004fa5e0..0x004fb920):

| offset | what |
|---|---|
| `+0x1c` | an `ObjList` embedded in the query: the cached member list, a deque of 16-bit handles (its deque proper at `+0x24..+0x34`, size at `+0x34`) |
| `+0x48` | the reference count, 0 at construction |
| `+0x4c` | the game time (ms, the scripts' clock `[0x00996ff4] + 0x1258`) at which the list next falls due; the constructor writes the current time |
| `+0x50` | the refresh period in ms, a constant per kind |

The period each constructor writes:

| kind | constructor | period |
|---|---|---:|
| `CVXPlayerFlagsQuery` (`EnemyObjs`, `FriendlyObjs`, `ControllableObjs`) | 0x004fb920 | 4000 |
| `CVXUnitsInSettlementQuery` (and its two siblings) | 0x004fafc0 | 2000 |
| `CVXSetOpQuery` (`Union`, `Intersect`, `Subtract`) | 0x004fb1b0 | 2000 |
| `CVXObjsInSightQuery` (`ObjsInSight`, `VisibleObjsInSight`, `ObjsInRange`) | 0x004fb290 | 1000 |
| `CVXMapAreaQuery<TCircleArea>` | 0x004fac20 | 1000 |
| `CVXMapAreaQuery<TRectArea>` | 0x004fa9d0 | 1000 |
| `CVXClassPlayerAreaQuery` | 0x004fb6e0 | 1000 |
| buildings in a settlement | 0x004fb0b0 | 1000 |
| `CVXPartyQuery` (`PartyQuery`) | 0x004fa5e0 | 1000 |
| `CVXObjQuery` | 0x004ff2b0 | 1000 |
| attached units (`AttachedUnits`) | 0x004fb380 | 1000 |

A **group** (`Group(name)`, `CVXGroupQuery`) and a **named object** (`CVXNamedObjQuery`) have
no cache: their list at `+0x1c` *is* the membership, their period slot answers 0 (0x00746ea0)
and their due-time slot answers the clock (0x00571700).

## The protocol

Every reader goes through the query's own virtual slots, the same on every cached kind
(vtables 0x007bcad8..0x007bce48; group and named object 0x007c47e8, 0x007c4830):

* **`+0x1c`, purge** (0x00572c10, all kinds): a stable remove of every member whose handle no
  longer resolves or whose object answers dead (`vtbl + 0x50`). On a group this edits the
  group itself.
* **`+0x20`, the list** (0x004f9490 on the cached kinds): if the clock is at or past `+0x4c`,
  refresh (`vtbl + 0x38`); then answer the embedded list. A group answers its list
  (0x005716f0).
* **`+0x24`, membership** (0x004f9550): if the clock is before `+0x4c`, scan the cached list;
  otherwise call the kind's one-object slot `+0x3c`.
* **`+0x28`, emptiness** (0x004f8ab0): if fresh, the cached list's size is zero; otherwise call
  the kind's slot `+0x40`.
* **`+0x30`** answers the period, **`+0x34`** the due time.
* **`+0x38`, refresh**: writes `+0x4c = clock + period` first, clears the list and refills it
  with the kind's sweep.

The slots `+0x3c`/`+0x40` split the kinds in two:

* class-player, both settlement kinds and the set operation keep the base's, which call `+0x20`
  (and so refresh) and then scan or count (0x004f8230 -> 0x00571870, 0x004f8240 ->
  0x00571710);
* the circle, the rectangle, sight, player-flags and party kinds override both with tests that
  store nothing and leave the due time alone. Emptiness is the kind's sweep with the collector
  (0x004fc7b0) in its counting mode (0 where a refresh passes 1): the same filters as a refresh.
  Membership is a one-object test that is **not** the refresh's: the circle's (0x004f8c60) is
  `dx² + dy² <= r²` on any object that resolves, with no class, death or template test; the
  rectangle's (0x004f8c10) is the four inclusive corners and nothing else; sight's (0x004f91a0)
  is "the observer can see it" (0x005a7870: a non-zero observer sight `[obj+0xd0]` and the
  edge-to-edge distance, 0x005a77b0, within it -- the observer's whole sight, not the query's
  frozen radius of sight less 100), then the class, then visibility for `VisibleObjsInSight`;
  player-flags (0x004f8ad0) is the relation mask against the object's owner bit, then the
  class; party (0x004f97c0) is a scan of the session's party.

What each entry point calls:

| entry point | slots |
|---|---|
| `count`, `GetObjList`, `NearestObj`, `SetCommand`, `AddCommand`, `KillCommand`, `ClearCommands`, `AddToGroup`, `RemoveFromGroup`, `RemoveFromAllGroups`, `Heal`, `HealStamina`, `SetPlayer`, `AddDefCmd`, `DistributeExperience`, `AttackArea`, `MoveToArea` | `+0x1c`, `+0x20` |
| `Face`, `SetFood`, `InHolder`, `AddToHolder`, `GetAverageDirection`, `SetParty`, `SetMessengerStatus`, `SetFeeding`, `Damage` | `+0x20` only |
| `Erase` | `+0x20`, then `+0x1c` after the walk |
| `IsEmpty` | `+0x1c`, `+0x28` |
| `Contains` | `+0x1c`, `+0x24` |
| `WaitQueryCountBetween`, `WaitIdle`, `WaitHealthBetween`, `WaitUnitsInArea` | `+0x20` |
| `WaitEmptyQuery`, `WaitNonEmptyQuery` | `+0x28` |
| `WaitObjInQuery` | `+0x24` |
| `WaitCommonObjects` | `+0x20` on the first, `+0x24` on the second |

So a query read inside its period answers the list its last refresh made, less what purges have
dropped since; a `Wait*` never purges.

## The set operation's refresh

0x004ffbe0, after writing its due time, reads its operands through their slots, so it composes
their **cached** lists and may refresh an operand that is due:

* union: each operand purged and listed, left first, each copy sorted by handle (0x004fb510),
  merged without repeats (0x004f98c0);
* subtract: the left purged and listed, walked in its order, a member kept when the right's
  `+0x24` says no;
* intersect: the operand with the **smaller period** (`+0x30`, unsigned; a tie walks the right)
  purged, listed and walked, a member kept when the other's `+0x24` says yes. A group's period
  is 0, so an intersection with a group walks the group in its joining order.

## The waits' cadence

A query wait whose test fails sleeps until the query's next refresh: 0x005eca10 takes
`+0x34` less the clock, and sleeps 100 ms when that is under 101; 0x005eca70 does the same with
the smaller of two queries' hints. `WaitHealthBetween` inlines it. So after a list read a wait
on a sight query polls every second, on a set operation every two, on `EnemyObjs` every four,
and on a group or named object every 100 ms. A wait that only asks emptiness of a due area,
sight, player-flags or party query refreshes nothing, so its hint stays in the past and it
polls at 100 ms.

## Interning

Each constructor first scans a per-kind pool and answers the **existing** object when the key
matches; only a miss allocates (and a new object is appended to the pool and registered,
0x00571650):

| kind | pool | key |
|---|---|---|
| set operation (0x004fe490) | 0x00996a34 | operation, both operand pointers |
| class-player (0x004fe730) | 0x00996a44 | player, class string, area pointer (null for none) |
| player-flags (0x004fe990) | 0x00996a54 | player, type, class string |
| units in settlement (0x004fe080) | 0x00996a74 | settlement handle, class string, mode |
| buildings in settlement (0x004fe350) | 0x00996a14 | two strings |
| sight (0x004fee10) | 0x00996a84 | observer handle, class string, radius, visible flag |
| circle (0x004ff860) | 0x00996aa4 | x, y, r, class string |
| rectangle (0x004ff520) | 0x00996a94 | four corners, class string |
| attached units (0x004ff190) | 0x00996a64 | |
| group (0x005765a0) | the name map at 0x009bdac8 | the name; a miss creates the group |
| party (0x004fa6e0) | the singleton at 0x00996a0c | |

Class strings are compared as strings. **The engine does not intern yet**: every call mints a
fresh object, which is evaluated on its first read; `globals.cpp` reuses one object per group
for a group *global*.

## The reference count

`+0x48` counts references and the object deletes itself when a release brings it to zero
(0x00571680; addref 0x00571670). The references are:

* one per value on the script stack: every constructor entry point adds one to the object it
  pushes (interned or new), and every `Query` member and every query wait releases the one its
  argument held when it finishes (the waits only on completion, so a suspended wait keeps its
  query alive);
* one per variable: the `Query` type's handlers add one when a variable is pushed (0x00573710),
  release the old value on assignment while taking over the stack's reference (0x00573800), and
  release on destruction (0x005737a0);
* one per set-operation operand (0x004fb1b0 adds, the destructor 0x004fa0d0 releases, and also
  removes the object from its pool and clears it out of any set operation that names it);
* the party singleton's own extra reference, dropped at teardown (0x00500b10).

So a query used as a temporary -- `EnemyObjs(p, "Unit").count` -- is made, read once and
destroyed, and the next call makes a fresh one that refreshes on its first read; a query held in
a variable across a loop keeps its cache. **The engine has no reference count yet**: its query
objects are never destroyed, which, with no interning, gives a temporary the same fresh first
read the original gives it.

## Live aliases

Two getters answer a list that is another object's own rather than a copy: `Hero::army`
(0x00530580) pushes the hero's army list at `[hero+0x1cc]` with its count bumped, and
`Settlement::Buildings` (0x005c3d40) pushes the settlement's building list at `[s+0x60]`.
The engine's `ObjListPool` aliases one kind of owner (a settlement's holder, for
`Settlement::Units`); these two still answer snapshots.

## What is still unknown

* **The grid order.** The area sweeps walk the original's grid cells and each cell's list
  (0x004fd060, 0x004fde60, 0x004fd9c0, ...), so a refreshed list is in grid order; the engine's
  sweeps answer ascending handles. Reproducing it needs the original's cell size and per-cell
  list order, which is a spatial-index design of its own.
* **Bit 26 of `[obj+0x2c]`** refuses an object from `EnemyObjs`'s one-object test (0x004f8b16);
  its meaning is not settled in this engine.
* **Group lifetime.** A group is created by `Group(name)` on a miss and lives in the name map;
  whether a map-declared group holds a reference of its own, so that its membership survives
  every script dropping it, was not read.
