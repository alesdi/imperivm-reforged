# The class tally `Count` reads

**Status:** read from `gbr.exe`; reproduced in `engine/core/src/sim/hero.cpp`
(`HeroSystem::ai_register`, `ai_unregister`, `ai_mutate`, `class_tally`) with
the differences listed at the end.

`Count(player, "class")` (0x00422300) does not count units. It reads a
number the AI keeps, per player and per class name, and the number parts from
the units on the map in ways a script can see. This page describes where the
number goes up and down, as read from the executable. It is a runtime
behaviour, not a file; nothing here is code from the executable
(`docs/legal.md`).

## The map

Each of the sixteen player records (`globals+0x12cc + p*0x320`) has a squad
holder at `[record+0x8c]`, and the holder carries an ordered map from a class
name (the class's own `id`, as a string) to a signed count.

* **The read** (0x004441e0, by way of 0x00444030) looks the name up and
  answers the count, or 0 when the name has no entry. `Count` takes the
  record at `player - 1`, guarded only by `player - 1 < 16`.
  `CheckUEnabled` (0x004243d0) and `GetPlayerUnits(player, class)`'s gate
  (the call at 0x005de807) read the same map. So do `AllyCount` and
  `EnemyCount` (0x00422473, 0x00422583), which this engine does not yet
  implement.
* **The add** (0x00447200) walks the unit's class and then each parent in
  turn (the class record's parent link). For each name it adds one,
  creating an entry of 1 for a name that has none. It then files the unit
  in a squad (`AddToSquad`, towards the node under the unit).
* **The take** (0x004448a0) walks the same chain and takes one off each
  name. It stops at the first name that has no entry, so the ancestors past
  that name keep their counts. Nothing clamps at zero, so a count can go
  negative. It also empties the unit's squad handle `[unit+0x174]` and takes
  the unit out of its squad.

Both act on the record of the unit's owner at the time of the call
(`[unit+0x70]`).

## Who calls them

* **The register** (0x0041f310) runs when a unit is placed. That includes
  `Place`, `PlaceInHolder` and the factories, nineteen callers in all.
  Nothing happens while a global latch is up (`[0x00a8734c]`) or while the
  AI's world is absent (`[0x008c8e50]`). Nothing happens either for a spawn
  template (bit 27 of `+0x2c`) or for a unit carrying `SetNoselectFlag`'s
  bit (`[unit+0x194] & 0x800000`). A unit with no owner is skipped too.
  Before the AI is up (`[0x008c8e54]` zero) the unit's id is pushed onto a
  pending list, which is drained later. Once the manager exists
  (`[0x008c8e58]`), a unit whose squad handle is empty goes through the add.
* **The unregister** (0x0041eee0) requires a squad handle and runs the take.
  It runs when a unit dies or is erased, and when `SetNoselectFlag` raises
  its bit.
* **`Unit::Mutate`** (0x005deef0) runs the take on the old object at
  0x005df1e4 and the add on the new object at 0x005df287. Neither runs for a
  unit carrying the noselect bit.

Nothing else touches the map:

* **Entering a holder** (a building, a ship, a garrison) neither takes nor
  adds. A garrisoned unit stays counted.
* **A change of owner** (`SetPlayer`, a conversion, a capture) neither takes
  nor adds. The unit stays in its first owner's count. When it later dies,
  the take runs on its *new* owner's record.
* **A ship** is registered like any unit. The ship branch at the top of the
  register counts ships per GAIKA node and does not touch the tally.

## How it drifts from the units on the map

* A converted unit is still counted by the player it left, for as long as
  it lives and after.
* When a converted unit dies, it takes one off its new owner's count, which
  never counted it. If the new owner has no entry for the class, the walk
  stops at once and nothing changes. If the new owner does have an entry,
  that count goes down and can fall below zero.
* A unit that was never registered is not counted: one with the noselect
  bit when it was placed, or one placed while the latch was up.
* A unit raised to noselect leaves the count at once.
* The name is matched exactly against class ids. A script that names a
  class by a spelling no class carries as its `id` reads 0.

## What this engine models

* The tally is `HeroSystem::class_tally_`, keyed on (owner, class index).
  Class ids are unique, so the index stands for the name. The map is saved
  in the hero section and hashed. An entry, once made, is never erased.
* The unit's handle being set is `ObjectFlags::ai_registered`. Only the
  tally reads it.
* `Place` registers at once, before it returns. `BARRACK_TRAIN.VS` reads
  `Count` on the line after its `Place`.
* Every other unit is registered by the turn's enrol sweep
  (`enrol_new_units_in_squads`), held units included. Units that exist
  before the AI manager starts are registered by the first sweep after it
  starts. That plays the part of the original's pending list.
* `HeroSystem::ai_unregister` (on death, on erase, and in `SetNoselectFlag`
  before the bit is written) runs the take on the present owner.
* `Unit::Mutate` takes the old class's chain off the tally, adds the new
  one, and files the unit in a squad at the node under it.

## What differs, and why

* **Births other than `Place` are counted up to a turn late.** A unit made
  by a path other than `Place` is registered at the next sweep. The original
  registers it at the moment of placement. No shipped script reads `Count`
  between such a birth and the next turn.
* **The pending list is not modelled as a list.** The original drains its
  pending list when the AI comes up. Here, the sweep registers those units
  instead, in id order. The counts that result are the same. When the drain
  runs relative to the first turn was not read.
* **The latch at `[0x00a8734c]` is not modelled.** What raises it was not
  read. It is probably a load in progress, and this engine restores the
  tally from the save instead.

## What is still unknown

* Whether any path empties `[unit+0x174]` without the take: a script's own
  squad change, or a squad's dissolution. If one does, a later register of
  the same unit would count it twice. The register runs only at placement,
  so this would need a unit to be placed twice.
