# The AI's ship count and ship list per area, as `gbr.exe` keeps them

The computer players ask how many ships a player has in a sea in two ways:
the scripts through `Ships(player, lsa)`, and the route search behind
`CheckLsaPath` and `PrepareAiTransportShip` through a walk of the ships it
can find in each sea. Neither looks at the map. Both read bookkeeping kept on
the node manager's passability-area records (`[0x008c8e54]+0x24`, `0xc0`
bytes each by area id; see [`gaika.md`](gaika.md)), and that bookkeeping is
written only when a ship is handed to the AI or taken away from it. This page
describes it as read from `gbr.exe`; addresses are given so each claim can be
checked, and nothing here is code from the executable (`docs/legal.md`).

## The two fields

In each area record:

| offset | what |
|---|---|
| `+0x14 + 4p` | the ship need of 0-based player `p` (`ShipNeeds`, `IncShipNeeds`, `ClrShipNeeds`) |
| `+0x54 + 4p` | the **kept ship count** of player `p` |
| `+0x94` | the **ship list**: an object list whose deque (at `+0x9c`) holds 16-bit object handles in the order they were pushed |

The persist of an area record (0x0044fab0, inside the node manager's
`CVXGlobalAI::Persist`, section `LSAs`) writes the coverage, the neighbours,
the distances, the sixteen `shipneed` words, the sixteen `shipcount` words and
the `ships` list. A save therefore carries both fields as they stood; a load
reads them back rather than recounting.

## The register raises it

The AI register (0x0041f310) is the routine every birth, placement, owner
change and first move hands a unit to (its unit half, the class tally and the
squad filing, is [`class-tally.md`](class-tally.md)). It returns at once while
the map editor's latch is up (`[0x00a8734c]`), while the AI world pointer is
absent (`[0x008c8e50]`), for a spawn template (bit 27 of `[obj+0x2c]`) and
for a unit carrying `SetNoselectFlag`'s bit (`[obj+0x194] & 0x800000`).
Then, **before it tests the owner and before it looks at the squad handle**,
and only while the node manager exists (`[0x008c8e54]`), it casts the object
to `CVXShip`. For a ship:

1. The area is that of the node under the ship's `posRH` (vtbl `+0xc8`;
   0x0044e940 reads the node slot grid and answers the node's `+0x10`), when
   the ship is in no holder (`[ship+0x154]` is `0xffff`). A held ship's area
   is read through its holder instead: the holder's settlement
   (`[holder+0xc]`), that settlement's central building (`[+0x8c]`), and
   the water area cached on it (`[central+0x1f6]`, filled when the node table
   is cut; it is what `Settlement::WaterLsa` answers for a shipyard). That
   read makes no class test.
2. The count of the ship's owner (`[[ship+0x70]+8]`, read with no test for a
   missing owner) in that area goes up by one (0x0044e900, 0x0044e920). No
   bounds test is made on the area.
3. The ship's handle is pushed on the back of the area's list (0x0044fd30 ->
   0x0041f290), with no test for a copy already there.

So the ship half runs on **every** call that passes the first gates, whether
the ship is registered already or not.

## The unregister lowers it

The unregister (0x0041eee0) needs the AI manager (`[0x008c8e58]`), a unit that
is not a template and not noselect, an owner, and a squad handle
(`[obj+0x174]` non-zero, which is what being registered means). After the
tally's take (0x004448a0) and while the node manager exists, for a ship:

1. The area is found as the register finds it -- from where the ship stands
   **now**.
2. The owner's count there is lowered by one, and never below zero: a count
   of zero or less is treated as one before the decrement.
3. If the area is within the table (0x0044e9a0 bounds-tests it), the first
   entry equal to the ship's handle is erased from its list (0x0041ee70).

## Nothing else writes them

Nothing else writes the count; the only other writer of the
list is the ship scan below, which erases. In particular **the move hook
(0x0041f530) does not touch either**: a ship that sails from one area to
another stays counted, and listed, in the area it was registered in. A water
area is a connected body of water, so a ship that moves under its own power
does not normally leave it; a placement, a held ship stepping out of a
shipyard on another sea, or a script's `SetPos` can.

## When the register and unregister run for a ship

Read caller by caller (the unit side of each is in `class-tally.md`):

* **`Place`** (0x005a8710), **`_PlaceEx`** (0x005a8980) and the anonymous
  placer 0x005a7fe0 create the object, set its owner, and set its position.
  A ship's `SetPos` is the unit's (0x005d39e0, the `CVXShip` vtable at
  0x007cf598 shares it), and it runs the move hook, which registers a unit
  whose squad handle is empty. Then the placer calls the register itself for
  a `CVXUnit` whose raw position has `x >= 0`. **A ship placed once in a
  running game is counted twice and listed twice.** Before the AI is up the
  move hook returns at once and the placer's call goes to the pending list,
  so a ship placed then counts once at the drain. The block placers, the
  holder placer and the debug `_Place` and `Spawn` follow the same pattern
  (class-tally.md); they were not reread for ships here.
* **`SpawnGroup`, `SpawnGroupInHolder`, `SpawnNamed`** (0x00572760,
  0x005724a0, 0x00571d40) hold the AI world pointer at zero while they clone
  and place, so the move hook's register returns at its first gate, and then
  call the register once: **one**.
* **A ship a shipyard builds.** `Settlement::CreateShip` (0x005ef240) holds
  the AI manager at zero while it sets the owner, the position and the holder,
  so neither its `SetPlayer` nor its move hooks register anything.
  `Ship::ShowBuildAnimation` (0x005c6a60) then, on its first pass, takes the
  ship out of the holder (0x005d3f20), whose `SetPos` runs the move hook on a
  ship never registered -- a register -- and at the end of the slide calls
  the register again: **two**.
* **The map's own ships** are pushed on the pending list by the map loader
  (0x0054e520, for every object carrying the unit bit) before the AI exists;
  the manager's start (0x00450aa0) creates the node manager and drains the
  list, one register each: **one**.
* **The move hook** registers a unit whose squad handle is empty, alive, not
  a template, not noselect and owned. A registered ship is not registered
  again by moving.
* **A change of owner** (`Unit::SetPlayer`, 0x005dc3f0, the ship's own vtable
  slot) runs the unregister on the old owner, in the area the ship stands in,
  and then 0x0041f4d0 registers it on the new owner when its `posRH` has no
  negative coordinate: one off the old owner (floored), one on the new.
* **Death** (0x005db270, the ship's vtable slot `+0xb0`), **erase**
  (0x005db230, slot `+0x94`) and **`SetNoselectFlag`** raising its bit
  (0x005d8750) run the unregister on a registered ship: one off.
* The unit's take-off-the-map virtual (0x005d4680, slot `+0x4c`) moves the
  unit to the held sentinel, ends its commands and drops its path, then runs
  the unregister. Its one direct caller found is the adventure's party store
  (0x004d0000), which, with the restore (0x004d04d0), also unregisters the
  party's units around a map change.
* **`Unit::Mutate`** (0x005deef0) calls the tally's take and add directly,
  not the register or the unregister, so it moves no ship count.
* A settlement's **boats** (0x005eed90 with `ShipS`) are `CVXWagon`s, not
  ships: the register sees no ship. The animal spawner (0x005887f0) and
  `AIH_UnitBuilt` (0x004249e0, no shipped site) are the remaining callers.

## Who reads them

* **`Ships(player, lsa)`** (0x00422660 -> 0x0043ee40): the count of
  `player - 1` in `lsa`, unchecked on either argument.
* **`AllyShips(player, lsa)`** (0x00422690 -> 0x0043ee60) and
  **`EnemyShips`** (0x004226c0 -> 0x0043eeb0): the counts of the sixteen
  players summed over those the relation read (0x0044e250, from `player - 1`'s
  row) answers 2 for (allies, which leaves out the viewer, who answers 1) or 4
  (enemies). No shipped site calls either.
* **The route search's shipless test** (0x00441b70): a water neighbour in
  which the asking player's count is 0 is the candidate for the ship need it
  bumps when the destination is unreached.
* **The ship scan** (0x00441730), which the route search asks of every water
  area it would enter and `PrepareAiTransportShip` asks of the sea it crosses:
  the area's list front to back. An entry whose handle no longer resolves, or
  whose object is dead (vtbl `+0x50`), is erased from the list and the walk
  goes on. Otherwise the entry is skipped unless its owner is the asking
  player and it has a running command; the first whose command makes it free
  (`idle`, a `boardunit` carrying the caller's order, an `advance` bound for
  a node with no enemy strength) is the answer. The list's order, its copies
  and its stale entries are what the scan sees.

## How the count drifts from the ships

* A ship placed by `Place` or built by a shipyard counts two; one from the
  map or a spawn counts one. Its death takes one, so a placed ship leaves one
  behind for good.
* A ship that leaves its area and dies elsewhere leaves its old area's count
  up, and the new area's count is floored at zero.
* A second register on a registered ship (`AIH_UnitBuilt`, a script's
  repeated hand-off) counts it again.
* The list holds a copy per register, less one per unregister; the scan
  prunes the dead ones as it meets them.

## What this engine models

* `AiSystem` keeps the counts (`ShipCount`, by area and player) and the lists
  (`ShipList`, by area, in push order), saves both, and hashes neither, like
  the rest of the AI's state.
* `ai_ship_register` and `ai_ship_unregister` (`sim/ai.hpp`) are the two
  ship halves; `HeroSystem::ai_register` runs the first after its noselect
  gate and before its owner and registered tests, `HeroSystem::ai_unregister`
  the second after the take. `ai_ship_area` is the area read, the held arm
  through `central_water_lsa` (`sim/economy.hpp`).
* `Ship::ShowBuildAnimation` calls the register twice for a held ship never
  registered, standing for the unboard's move hook and the slide's end.
* `Ships`, `AllyShips`, `EnemyShips`, the shipless test and the ship scan
  read the kept state.

## What differs, and why

* **`ShowBuildAnimation`'s ship stays in the shipyard** until its first
  order (the slide is not modelled; `sim/world_host.cpp`). Both its
  registers therefore count in the area the shipyard caches rather than in
  the area of the exit point; the two are the same sea whenever the yard's
  first water door lies in it.
* **An area the table does not have** -- area 0 (node 0's, a point off the
  map), the `0xffff` an uncut cache holds, or a holder that is no
  settlement's -- is written past the record vector or into the reserved
  record there; here nothing is kept. **An ownerless ship** is read through
  a null owner there; here it is not counted.
* **The editor latch** is not modelled; there is no editor.
* **The adventure's party store and restore** are not modelled, so their
  unregisters do not happen here.
