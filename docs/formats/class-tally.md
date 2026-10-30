# The class tally `Count` reads

**Status:** read from `gbr.exe`; reproduced in `engine/core/src/sim/hero.cpp`
(`HeroSystem::ai_register`, `ai_unregister`, `ai_owner_changed`,
`ai_mutate`, `class_tally`) with the differences listed at the end.

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

* **The register** (0x0041f310). Nothing happens while the map editor's
  latch is up (`[0x00a8734c]`; see below) or while the AI's world pointer
  is absent (`[0x008c8e50]`). Nothing happens either for a spawn template
  (bit 27 of `+0x2c`) or for a unit carrying `SetNoselectFlag`'s bit
  (`[unit+0x194] & 0x800000`). A unit with no owner is skipped too. Before
  the AI is up (`[0x008c8e54]` zero) the unit's id is pushed onto a pending
  list instead. Once the manager exists (`[0x008c8e58]`), a unit whose
  squad handle is empty goes through the add -- the tally, then
  `AddToSquad` towards the node under it. A ship also adjusts the ship
  counts of the GAIKA node it stands on, which is not the tally.
* **The unregister** (0x0041eee0) requires the manager, a unit that is not
  a template and not noselect, an owner and a squad handle, and runs the
  take. It runs when a unit dies or is erased, when `SetNoselectFlag`
  raises its bit, and in `Unit::SetPlayer` (below).
* **`Unit::Mutate`** (0x005deef0) runs the take on the old object at
  0x005df1e4 and the add on the new object at 0x005df287. Neither runs for a
  unit carrying the noselect bit.

### When the register runs

The register has nineteen callers. Every one is a birth, a move or a change
of owner, and each calls it at that moment:

* **`Place`** (0x005a8710) and **`_PlaceEx`** (0x005a8980) set the owner,
  then the position -- and a unit's `SetPos` (0x005d39e0) runs the move
  hook below, which registers it -- then call the register again for a
  unit whose position reads back with `x >= 0`, which finds it registered.
  `_PlaceBlockEx`, `PlaceInHolder`, the debug `_Place` and `Spawn`, and the
  animal spawner do the same.
* **`SpawnGroup`, `SpawnGroupInHolder` and `SpawnNamed`** (0x00572760,
  0x005724a0, 0x00571d40) save the AI world pointer and set it to zero while
  they clone, place, enter a holder and file the copy under names and
  groups, so nothing those steps run registers it. At the end of each copy
  they restore the pointer, call the register on the copy, and zero it
  again. Each copy is registered before the next is made.
* **A settlement's mules and boats** (`make_mule`, 0x005eed90): the owner,
  the forced entry into the settlement's holder, then the register. A held
  unit is registered like any other; the register has no holder test, and
  neither has `AddToSquad`.
* **A ship a shipyard builds**: `Settlement::CreateShip` (0x005ef240) sets
  the AI manager to zero while it sets the owner, the position and the
  holder, so the new ship is not registered there.
  `Ship::ShowBuildAnimation` (0x005c6a60) registers it at the end of its
  slide out of the shipyard.
* **The move hook** (0x0041f530), which a unit's `SetPos` runs, and the
  forced holder entry (0x005d3e10) too: for a unit that is alive, not a
  template, not noselect and owned, an empty squad handle means the
  register, and a set one means its squad is re-homed. So any unit that
  missed its registration is registered the next time it moves.
* **`Unit::SetPlayer`** (0x005dc3f0), when the new owner differs from the
  old: the unregister on the old owner (with the manager held at zero while
  the base `SetPlayer` writes the owner), then 0x0041f4d0 -- which checks
  the manager, the template bit and the noselect bit, and that `posRH` has
  no negative coordinate -- registers it on the new owner. The same owner
  again runs only the base.
* **The AI manager's start** (0x00450aa0, reached from `LSA()`): it sets
  the manager flag, builds the AI's own and per-player records, and then
  drains the pending list, registering each id in the order it was pushed.
  The map loader (0x0054e520) pushes the map's own units there, because it
  runs before the AI is up.
* Map load, `AIH_UnitBuilt` (0x004249e0) and 0x005a7fe0 complete the list.

**The latch at `[0x00a8734c]` is the map editor's.** The main menu's editor
button, the command-line editor mode and the editor's own start raise it;
starting a game in every form (single player, a multiplayer join, the
editor's test play) lowers it. It is never up in a played game.

Nothing else touches the map:

* **Entering a holder** (a building, a ship, a garrison) neither takes nor
  adds. A garrisoned unit stays counted.
* **A change of owner that is not `Unit::SetPlayer`** -- a building's, or a
  settlement's own record -- does not touch the tally, which counts units.

## How it drifts from the units on the map

* When a registered unit dies, it takes one off its present owner's count.
  A count that some path never added to -- a unit registered while
  noselect was clear, raised to noselect, and lowered again -- can fall
  below zero, since nothing clamps.
* The take stops at the first name in the chain with no entry, so an
  owner with no entry for the unit's own class keeps its ancestors' counts.
* A unit that was never registered is not counted: one carrying the
  noselect bit while every register ran.
* A unit raised to noselect leaves the count at once.
* The name is matched exactly against class ids. A script that names a
  class by a spelling no class carries as its `id` reads 0.

## What this engine models

* The tally is `HeroSystem::class_tally_`, keyed on (owner, class index).
  Class ids are unique, so the index stands for the name. The map is saved
  in the hero section and hashed. An entry, once made, is never erased.
* The unit's handle being set is `ObjectFlags::ai_registered`.
* `HeroSystem::ai_register` is the register: the gates, the tally and the
  squad filing (`add_to_squad` towards the GAIKA node under `posRH`), at
  once. `SpawnGroup`, `SpawnGroupInHolder`, `SpawnNamed`, `make_mule` and
  `ShowBuildAnimation` call it at the moment the original does.
* `Place` and `_PlaceEx` call `HeroSystem::ai_register_placed`: the gates
  and the tally at once, and the squad filing at the next turn's sweep (see
  "What differs").
* `HeroSystem::ai_owner_changed` is `Unit::SetPlayer`'s half, run by the
  `SetPlayer` host for a unit.
* `ai_start_players`, the first time it starts the manager, registers every
  unit not yet registered, in id order: the pending list's drain.
* `register_unregistered_units` runs once a turn while the manager exists
  and registers every unit that is alive, owned, not a template, not
  noselect, not held and not registered: the move hook's register. It then
  files `Place`'s units of the turn before in a squad, each that is still
  alive, registered, in no squad and not in a holder; each is looked at
  once. That list is saved in the hero section.
* `HeroSystem::ai_unregister` (on death, on erase, in `SetNoselectFlag`
  before the bit is written, and in `ai_owner_changed`) runs the take on
  the present owner.
* `Unit::Mutate` takes the old class's chain off the tally, adds the new
  one, and files the unit in a squad at the node under it.

## What differs, and why

* **`Place` files its unit in a squad at the next turn, not at once.** The
  tally is counted at once, as in the original, so `BARRACK_TRAIN.VS`'s
  `Count` on the line after its `Place` sees the recruit. The squad filing
  waits for the next turn's sweep, and is skipped for a unit that has
  entered a holder by then. The reason is measured, not read. On
  Crossroads, seed 5, 6,000 turns, filing at once made 241 units against
  379 deferred (main: 393) and 4 captures against 25 (main: 18), and it
  slowed Balcans from 33 s to 49 s. The likely mechanism, not confirmed: a
  recruit `BARRACK_TRAIN.VS` places at the barracks' exit and orders to
  `enter` its hall joins whatever squad stands there, and that squad's
  orders (`AIOSENDSQUAD.VS`'s `ClrCmd` among them; see `squad.md`) cancel
  the walk. What keeps the original's recruits walking was not read: it may
  be the move hook's re-homing at each step, which is not modelled, or the
  state of the squads they meet.
* **The move hook runs once a turn, not at every move.** A unit that
  reaches the map unregistered -- one whose owner changed while it stood
  in a holder, or a ship `CreateShip` made and a script then moved without
  `ShowBuildAnimation` -- is registered at the next turn rather than at the
  move that brought it out. The hook's other arm -- re-homing a registered
  unit's squad on every move -- is not modelled.
* **The pending list is not modelled as a list.** The drain registers every
  unregistered unit in id order. The original pushes the map's units in
  load order and any a script placed before `LSA()` after them, which is
  also id order here.
* **`ShowBuildAnimation` registers at once**, not at the end of the slide,
  because the slide is not modelled (the note on `f_presentation_void` in
  `engine/core/src/sim/world_host.cpp`).
* **The latch is not modelled**: there is no editor.

## What is still unknown

* Whether any path empties `[unit+0x174]` without the take: a script's own
  squad change, or a squad's dissolution. If one does, the move hook would
  register the unit again at its next move and count it twice. The engine
  keeps `ai_registered` apart from squad membership, so it does not.
