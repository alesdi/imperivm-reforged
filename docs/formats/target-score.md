# Target choice: the score, the danger counter and the sweeps

How `gbr.exe` chooses what a unit, a tower or a training partner goes for,
read from the executable, and what this engine reproduces. Addresses are
`gbr.exe`'s, cited so each claim can be checked; nothing here is the
executable's code (`docs/legal.md`, rule 2). The implementation is the
target-selection block of `engine/core/src/sim/combat.cpp`
(`CombatSystem::best_target`, `acquire`, `best_target_in_list`,
`best_target_among`, `danger`, `raise_danger`), `building_best_target` in
`sim/siege_fire.cpp` and `m_best_training_target` in `sim/world_host.cpp`.

## The danger counter

Every unit carries two words the persist stream (0x005dd710) names `danger`
(`[unit+0x1b0]`) and `lastdangertime` (`[unit+0x1b4]`). The constructor
(0x005d3170) zeroes both. They are saved with the unit.

`danger` counts how many searches have recently chosen this unit as their
answer. It is between 0 and 6.

* **Reading it decays it** (0x005d2800). The time since the stamp is taken
  against the object's game clock, as an unsigned 32-bit difference. When
  more than 1024 ms have passed, the counter loses one point per whole 1024
  ms of the difference (the difference shifted right by ten), and stops at
  0. The stamp then moves forward by the **whole** difference, so it becomes
  the current time and the remainder below 1024 ms is lost. When 1024 ms or
  less have passed, nothing changes. A read is therefore a write: two reads
  1.5 s apart lose one point and restart the clock, where one read after
  3 s would have lost two.
* **Raising it** (0x005d2850, with the amount as its argument) first applies
  the same decay, then adds the amount, and keeps the sum between 0 and 6.
  Every caller passes 1.

Who raises it, always on the unit that a search has just answered:

| raiser | when |
|---|---|
| the six `*InSquadSight` entry points' core 0x005dc950 | the answer is a unit (the squad path tests the unit bit 0x400000; the circle path casts to `CVXUnit`) |
| `BestTargetForPos` and `BestTargetInRange`, core 0x005dd060 | the search's "a unit was taken" word is set |
| `Building::BestTarget` 0x004dcc60 | there is an answer (the sweep finds units only) |
| `Unit::BestTrainingTarget` 0x005d5bf0 | the answer is a unit |

Who reads it: the unit score 0x005d3630 (inline, the same decay), the tower
predicate 0x004dbb20 and the training predicate 0x005d3cb0 (both through
0x005d2800). So the counter is how the original spreads its fire: a unit
several searches have just picked scores worse, by 40 a point, for a few
seconds.

## The unit score

0x005d3630 scores one candidate for one attacker. Its distance is the
**surface distance** of 0x005a77b0: the integer square root of the squared
centre distance, less both class radii (`[cls+0x2dc]`).

* **A unit that follows a hero measures from both.** When the attacker's
  hero handle (`[unit+0x170]`, `Unit::hero`) resolves, the distance is the
  mean of the hero's surface distance to the candidate and the attacker's
  own, the sum halved toward zero. Otherwise it is the attacker's own.
* The score is the candidate class's `target_factor` (`[cls+0xb2c]`), plus
  40 times the candidate's `danger` (decayed by this read), plus the
  distance.
* 40 more when the distance is under the attacker's `min_range`
  (`[unit+0xd8]`).
* 80 more when the candidate is moving: bit 0x20000 of its sync word, the
  active-path bit (`ObjectFlags::has_active_path` here).
* It also reports whether the distance is at most the attacker's stored
  `range` (`[unit+0xd4]`).

Lower is better. `target_priority` plays no part: the unit family never
ranks a building (below).

## The search object and its predicate

0x005d45a0 builds the search: the attacker, its position, the enemy mask of
the attacker's player (`[player+0x10]`, with bit 14 -- wildlife -- cleared
when the no-independents flag is set) and its `seeflags` side mask, and five
words: the in-range flag, a class name, the prefer-undiseased flag, the
exclusive flag and the armed flag. It starts with no answer and a best score
of 0x7fffffff.

Every candidate goes first through 0x005db7f0:

1. **The in-range flag** (`BestTargetForPos`, `BestTargetInRange`): the
   candidate's surface distance from the attacker (the attacker's own, never
   the hero mean) must be no more than the attacker's `range` and no less
   than its `min_range`, or it is skipped. Both ends are surface distances.
2. **A unit** (sync bit 0x400000) goes to the predicate 0x005db650.
3. **Anything else is the catapult fallback.** It is considered only while
   no unit has been taken. It must be a `CVXCatapult` the attacker's
   `CanAttack` virtual accepts; its plain surface distance is its score, and
   it is held when nothing is held yet or that distance is no more than the
   score held. A unit met later replaces it whatever its score, because the
   comparison below only applies against a unit.

The predicate 0x005db650, for a unit:

1. Skip it when the attacker's `CanAttack` virtual (`vtbl+0xb4`) refuses it,
   when it is inside a holder (`[obj+0x154]` not 0xffff), when it is dead
   (`vtbl+0x50`), when its class's `target_factor` is negative, when its
   owner bit is not in the enemy mask, or -- with the armed flag -- when its
   own `damage` (`[obj+0xdc]`) is not positive.
2. Score it (above).
3. Prefer-undiseased: a diseased candidate (bit 1 of `[obj+0x194]`) gains
   200.
4. A class name is given and the candidate's class name differs: with the
   exclusive flag, skip it; without, it gains 200, and step 5 is passed over.
5. Otherwise (the name matches, or none was given), when the candidate is in
   range by the score's report: a score under 50 **takes it at once and ends
   the search** (the predicate answers 0); any other score loses 100.
6. When a unit is already held and its score is lower than this one, keep
   it. Otherwise this one is held. **A tie goes to the later candidate.**

## The sweeps

The circle sweep 0x005db8c0 walks the object grid, 256 world units a cell,
over the box of the circle clamped to the map: rows top to bottom, cells
left to right, and within a cell the cell's own list. A cell is visited only
when its flag union meets the enemy mask. An object is offered when its
centre is within the circle (centre distance squared at most the radius
squared), its owner bit is in the enemy mask, and it is not hidden -- a
hidden object (sync bit 0x200000) only when its owner bit is in the
attacker's player's `seeflags`, which is to say its owner shares its view
with that player. The sweep stops when the predicate answers 0.

The users:

* **The `*InSquadSight` core 0x005dc950.** A unit in a squad, outside any
  holder or in a squad flagged `SF_ADVCHOOSER`, walks its squad's `Enemies`
  (0x005dbb20, `sim/squad.hpp`) with no circle, offering every entry and
  never stopping (it discards the predicate's answer). Any other unit
  outside a holder sweeps the circle **around its hero, with its hero's
  sight** (`[hero+0xd0]`), or around itself with its own when it has none.
  A held unit outside that squad case finds nothing. Which word lands in
  which predicate slot on each path is in `sim/combat.hpp`'s `TargetFilter`
  note.
* **`BestTargetForPos` 0x005dd280 and `BestTargetInRange` 0x005dd2a0**, core
  0x005dd060: a dead receiver prints and answers nothing; a held one answers
  nothing. The circle is around the given point (the receiver's own
  position for `ForPos`, or when the point's x is -1) with the given radius,
  or the receiver's sight when the radius is under 1 **or the receiver
  shoots** (its class declares a `projectile_class` other than
  `**Invalid**`, 0x0053f5b0). The in-range flag is set for `ForPos`. No class
  name, no other flags.
* **`Unit::BestTargetInGAIKA`**'s squad walk (0x004282c0, from 0x00431090)
  offers the squads standing in the node through the same predicate.

## The tower's search

`Building::BestTarget` (0x004dcc60, predicate 0x004dbb20) sweeps the box of
its sight for units only (`sim/siege_fire.cpp` has the rest). It reads
`danger` twice:

* the **first** unit it takes sets a "low danger" mark when that unit's
  `danger` is under 4;
* a later unit beyond `range` is skipped when its `danger` is over 3, and,
  while the mark is set, when it is farther than the one held.

A unit within `[min_range, range]` is still taken at once and ends the
sweep, without reading `danger`. The answer's `danger` is then raised by 1.

## The training partner

`Unit::BestTrainingTarget` (0x005d5bf0, predicate 0x005d3cb0) sweeps units
within its sight by the same grid walk (0x005d4cd0), hidden units by the
same side-mask rule. An enemy whose `damage` is not zero is taken at once.
Otherwise a candidate must be a unit, not the receiver, carrying the
training bit (sync 0x10000000), and is scored with **surface** distance:
`40 * danger + distance`, plus 100 for a hero (sync 0x1000000), plus 40
under the receiver's `min_range`, plus 80 when moving; within `range` a
score under 50 is taken at once and anything else loses 100. A tie goes to
the later candidate. A unit answer's `danger` is raised by 1.

## What this engine reproduces

All of the above, with these labelled differences:

* Within one grid cell the original follows the cell's list; this engine
  has no cell lists and orders a cell's objects by id
  (`docs/formats/siege-fire.md` has the same note for the tower sweeps).
* The game clock the counter reads is the object's (`[obj+0x18]`, a pointer
  this reading did not trace to its target); this engine reads the world's
  clock, which is the clock every other stamp on a unit reads.
* `Unit::BestTargetInGAIKA`'s walk offers every member of every standing
  squad and stops nothing (`sim/ai.cpp`); the original stops offering a
  squad's members at the first one the predicate answers 0 for. The
  predicate and score are the shared ones; whether that core raises
  `danger` was not read.
* This engine's own automatic acquisition (`CombatSystem::act`, a unit with
  no target taking one between scripted orders) has no counterpart in the
  original's family; it keeps the engine's older rule
  (`CombatSystem::auto_target`), labelled there.

## What is still unknown

* The target of the object's clock pointer `[obj+0x18]`.
* The order of a grid cell's object list (insertion at the head or the
  tail, and what re-files an object).
* Whether a unit's own `seeflags` bit is set, which decides whether a hidden
  unit of one's own player would pass the side test; the family never
  offers one's own units, so the answer is not observable here.
