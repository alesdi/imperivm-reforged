# Squads as `gbr.exe` keeps them

A squad is the AI's unit of an army: every unit a player owns is put in one
when it enters the world (while the AI manager exists), a hero leads one that
holds his army, and `Squadize` and the regroup make them on a script's say-so.
This page describes the original's squad record and the lists around it, as
read from `gbr.exe`, so the engine's model (`sim/squad.hpp`) can be checked
against it. Addresses are given so each claim can be checked; nothing here is
code from the executable (`docs/legal.md`).

It is a description of a runtime structure, not of a file. What a save
carries of it is in [`save.md`](save.md); the engine's save layout is its own.

## Where squads are held

Each of the sixteen player records (`globals + 0x12cc + player * 0x320`)
points at `+0x8c` to the player's **squad holder**. Its squad vector
(`[holder+0xc]`..`[holder+0x10]`, four bytes an entry) is indexed by the
squad's number: a unit's packed squad handle at `[unit+0x174]` carries the
player in its low four bits and the number in the next twelve, and
0x00444190 resolves it through this vector. The holder also keeps a per-class
tally (`ClassCount`, consulted by 0x004441e0) that 0x004448a0 maintains for
a unit's class and every ancestor as the unit is registered and unregistered.

Everything that walks "the player's squads" walks this vector in index
order: `MilUnits` (0x00421ee0 -> 0x00443eb0), `GetPlayerUnits`
(0x005de750), `HeroArmiesFullPerc` (0x00443f50).

## The squad record

Constructed by 0x00444310; persisted by 0x00445870 under the names in
brackets. Only the fields this page is about are listed; `sim/squad.hpp`
documents the AI fields (`+0x1c` to `+0x34`).

| offset | what |
|---|---|
| `+0x1c` | strength, `Squad::Eval` (`[squad+0x1c]`) |
| `+0x30` | flags word (`flags`); bit 3 is the script lock |
| `+0x34` | the packed handle: player in bits 0-3, number in bits 4-15 |
| `+0x38` | **`Units`**: an object list, its handle deque at `+0x40` (map `+0x44`, map size `+0x48`, offset `+0x4c`, size `+0x50`) -- the members, front to back |
| `+0x64` | **`Enemies`**: an object list of the same shape, deque at `+0x6c` (size `+0x7c`) |
| `+0x90` | a third object list of the same shape, deque at `+0x98`; not persisted |
| `+0xbc` | `LastFight` |

The constructor also reads `[GamePlay] SquadLookIvl` from `DATA\CONST.INI`
into a global (0x008c8e3c), with 0 for a missing key. The shipped value is
1000.

## The look: what fills `Enemies`

A squad's one timer is 0x28. The join (0x00445a50) arms it with no delay
when, after the new member is pushed, the squad has exactly one member and
the AI node table (`[0x008c8e54]`) exists (0x00445b4b). The timer's handler
(0x00444590) calls `vtbl+0x1c` of the squad's own table, 0x007b13c8, which
is the look, 0x00447700. (The table at 0x007b13c0, where that slot is a bare
return, belongs to the squad's virtual base.) The look ends by re-arming the
timer for `SquadLookIvl` from the clock now (0x00447a47 -> 0x00688220). When
a leave brings the squad's strength to 0, 0x00444750 kills the squad's
timers (0x00687c80), and the look stops until a join leaves the squad with
one member again.

The look, in order:

1. **The eyes.** A copy of `Units` is walked front to back. A member that
   does not resolve, or is dead (`vtbl+0x50`), is erased. The member with the
   strictly largest sight (`[unit+0xd0]`) so far is the leader; any other is
   erased when the floored integer root of its squared distance to the
   leader (both positions through `vtbl+0xc8`, the unit's `posRH`) plus its
   own sight is at most the leader's sight -- its circle lies inside the
   leader's. A second walk from the front to the leader erases the earlier
   members by the same test.
2. **The circles.** Each remaining member is a circle of its sight about its
   `posRH`; their bounding box (16-bit corners) bounds the sweep (0x00446380).
3. **The sweep** (0x00445d10) walks the 256-unit grid cells the box covers,
   clipped to the world. A cell whose owner mask meets the search mask is
   searched object by object along its chain. An object is offered to the
   accumulator when its owner bits meet the mask, it is not hidden
   (`[obj+0x2c] & 0x200000`) unless its owner is in the squad owner's
   `seeflags`, and its position lies in the box and in some circle
   (0x00443d60, squared distance at most the squared sight).
   The mask (0x004445f0 -> 0x00514080) is `~seeflags | enemyflags` of the
   squad's owner: `seeflags` (`[record+0x18]`) is the owner and every player
   whose relation to it carries the share-view bit (`SetRelation`,
   0x005652f0); `enemyflags` (`[record+0x10]`) every player its own row is
   not at peace with.
4. **The accumulator** (0x004459c0), with "at war" meaning the object's owner
   is in `enemyflags`:
   * a unit (`0x400000`) that is dead is passed over; a living one at war
     that is neither a messenger (`0x4000000`) nor an `Animal` heir
     (0x00540280) goes on the enemy list; any other goes on the third list;
   * a building (`0x800000`) at war goes on the enemy list, any other on the
     third list;
   * anything else is passed over.
5. `Enemies`' deque is cleared (0x00447982) and the enemy list copied in
   (0x00447a17, through `[squad+0x64]+8`); the third list likewise into
   `+0x98`.

That copy is why `Enemies` was once read as never written: a search for
writes at `+0x6c`..`+0x7c` finds only the constructors.

## Who reads `Enemies`

| reader | address | what it does with it |
|---|---|---|
| `BestTargetInSquadSight` and its five siblings | 0x005dc950 -> 0x005dbb20 | a unit in a squad, outside any holder or in an `SF_ADVCHOOSER` squad, offers each entry to the target predicate (0x005db7f0) and draws no circle; the five wrapper flags reach the predicate in their own slots on this path (0x005dc9b7) and one slot over on the circle path (0x005dca31) |
| `IsEnemyInSquadSight` | 0x005d7400 | `[squad+0x7c] != 0` |
| `GetBestCurseTarget` | 0x005d9cb0 -> 0x005d94f0 | the same gate; each entry to the curse predicate |
| `GetBestCrippleTarget` | 0x00513e10 -> 0x005138c0 | the same gate; non-units handed on as nothing |
| the free-spot search | 0x004180b0 | refuses (answers (-1, -1), no draw) a unit with a target (`[unit+0x1a8]`) whose squad's `Enemies` holds more than 10 |
| a route test | 0x00417ab0 | a unit with a target whose squad's `Enemies` holds 25 or more skips a distance test; reached through a table from 0x0041a024, not identified in the engine |

The third list (`+0x90`) has no reader found among the functions that
resolve a squad.

## What is still unknown

* Where the original fires a look within a pump turn relative to scripts and
  other timers; the engine fires due looks once a turn.
* The grid's chain order within a cell, which orders `Enemies`; the engine
  lists in ascending object id.
* The reader at 0x00417ab0 and the route step that calls it.
* Whether anything reads the third list.
