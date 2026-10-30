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

## The squad-list pool

A `SquadList` handle is an index into one pool of squad lists for the whole
game: a vector of 1,024 entries of 16 bytes, made by a static constructor
(0x007a8920) and never grown, shrunk or cleared. Each entry is a deque and an
in-use word at `+0xc`.

* **The claim** (0x0043ed10) walks the pool from the front and takes the
  first entry whose in-use word is zero. When all 1,024 are in use it
  answers -1.
* **The release** (0x0043ff60) empties the entry's deque and zeroes its
  in-use word. The index stays a valid index.
* **`SquadList::IsValid`** (0x00421a70, by way of 0x0043ed70) answers only
  whether the index lies inside the pool: from 0 to 1,023. So a list that
  was released, or never claimed, answers valid, and the one handle that
  answers invalid is the claim's -1.

The engine's handles are the index plus one (`kNoSquadList` is 0), so valid
handles run from 1 to 1,024 (`kSquadListPoolEntries`). Its pool grows on
demand rather than being 1,024 entries from the start, and never answers
the full pool's -1; a script could tell only by holding 1,024 lists at once.

## The regroup and the assignment

One worker (0x00447330) forms squads out of a list of bodies, and it has five
callers: `Squadize` (0x004255e2), the free `SetNoAIFlag` (0x00435584),
`Unit::SetNoAIFlag` (0x005de6e5), the siege planner's crews (0x00436565) and
`SquadList::Train`, once for the units it sends inside (0x00436ce8) and once
for the ones it trains (0x00436d5f). It empties the caller's squad list, and
then for each owned body in list order:

1. **A fresh squad of its own.** The body leaves the squad it was in through
   the join (0x00445a50: out of the old squad, which is stood down when that
   empties it, the squad handle `[unit+0x174]` set, the body appended and
   its strength added, and the squad filed under the body's node when it is
   the first member). The fresh squad copies the old one's flags, both
   GAIKAs and its last fight, or starts from the class's flags and the node
   under the body; bit 0 is rewritten from the body's no-AI flag, the lock
   bit cleared, the old squad's AI order re-posted, and its state is the
   placeholder `0xffff`. **Nothing here asks whether the body is a hero or
   attached to one**, and the join moves one object: a hero leaves his army
   in the squad he led.
2. **The assignment** (`AddToSquad`, 0x00446a70) on the body, towards the
   fresh squad's destination.
3. The squad the assignment answers is listed when it has exactly one member.

Then every listed squad takes the caller's state and the current time.

The assignment's arms, in order:

* a body with the no-select bit (`[obj+0x194] & 0x800000`) answers null;
* a **hero** with no squad gets one, joins it, and joins every member of his
  army after him; a hero with one only sets its destination and answers it;
* a body whose **hero** handle (`[obj+0x170]`, what `Unit::hero` reads,
  0x005d7380) resolves (0x00446bcc): when the hero is in no squad, null;
  when the body is already in the hero's squad, that squad; otherwise the
  body is moved to the end of the hero's squad (0x00446570 -> the join) and
  that squad is answered;
* the rest is the merge rule (a body close enough to its own leader stays; a
  ship and a member of a locked squad do not merge; otherwise the first of
  the player's squads that accepts it on nine clauses, within 256 units of
  its front member and under ten members; failing that a leader keeps its
  squad and a follower starts one), described where the engine keeps it
  (`add_to_squad` in `sim/squad.cpp`).

So the list's order decides what happens to a hero's army. Listed before his
men, the hero moves into a fresh squad alone and each man listed after him
is moved into it by the third arm; a man listed before him is put back at
the end of the squad the hero still leads, and the hero then leaves it, alone
in his fresh squad with his army behind him. Units attached to him and not
listed are not moved either way. Train lists a squad's members front to
back, and a hero leads his squad, so its enterers usually bring the hero's
men with him. The siege planner sorts its crews by engine and calls the
worker once per engine, as its walk reaches the next engine's crews, so one
engine's crews merge with each other and with nobody else's.

`Squadize` dereferences the assignment's null answer (`[eax+0x50]` at
0x0044756c): a body whose hero is in no squad would fault there. This engine
lists nothing for it and leaves the body in its fresh squad.

**This engine** (`squadize_bodies`, `regroup_into_fresh_squads`,
`add_to_squad` in `sim/squad.cpp`) does the same for all five callers. Its
hero keeps a record of the squad he is in (`HeroRecord::squad`), which
follows him into the fresh squad, and a member detached from him leaves the
squad it is actually in, which after such a split is not his. `GAIKAIn` and
the last attacker are left at their defaults on a fresh squad, where the
original leaves whatever its pooled slot held.

A unit attached to a hero is moved into his squad, out of the one it was in:
the hero's `AddUnit` (0x0052e3e0) runs the hook 0x0041e820, whose assignment
takes the hero arm, and the join takes the unit out of its old squad first.
The engine used to append it and leave it filed where it was, so a unit the
sweep below had filed could stand in two squads, and after a detach it could
be left in another hero's. The AI's orders to that other squad reached it --
on Crossroads a `ClrCmd` from `AIOSENDSQUAD.VS` on a squad parked at its node
ended, every few seconds, the `form_move` of men marching out of a hall with
a different hero, and they stood piled at the door. The engine still files
the unit in the hero's squad whether or not the AI manager exists, and does
not run the hook on a detach (0x0052dc03), where the original re-homes the
unit by the merge rule; a detached unit is in no squad here.

A unit's first squad comes from the AI register (0x0041f310 -> 0x00447200),
which files it with `AddToSquad` towards the node under it, at its birth:
`Place` (here at the next turn; class-tally.md, "What differs"), the
spawns, a settlement's mules and boats, a shipyard's ship at the end of its
slide, the manager's start for the units that were there before it, and a
change of owner. A held unit is filed like one on the map, by the
node under its holder's settlement; neither the register nor `AddToSquad`
asks about the holder. `docs/formats/class-tally.md` lists the callers. The
engine calls `HeroSystem::ai_register` at each of them, and once a turn
(`register_unregistered_units`) registers any unit on the map that is still
unregistered -- the move hook's half (0x0041f530), which `SetPos` runs.

Until the fourteenth round the engine filed a unit by a once-a-turn sweep
over ids it had not seen, which skipped a unit born inside a holder; that
was this engine's reading, made before the register's callers were read.

## What is still unknown

* Where the original fires a look within a pump turn relative to scripts and
  other timers; the engine fires due looks once a turn.
* The grid's chain order within a cell, which orders `Enemies`; the engine
  lists in ascending object id.
* The reader at 0x00417ab0 and the route step that calls it.
* Whether anything reads the third list.
