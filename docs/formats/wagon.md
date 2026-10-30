# The wagon (`CVXWagon`)

What `gbr.exe` keeps on a wagon -- a mule, a caravan, a camel, a supply boat
(`ShipS`) -- and the rules that read and write it. Every claim names the
address it was read at, so it can be checked; nothing here is decompiler
output (`docs/legal.md` rule 2). The engine's implementation is written from
this description, and the last two sections say how far it goes.

## The classes

Every shipped wagon class is a `cpp_class="CVXWagon"` heir of `Wagon`
(`DATA\CLASSES\WAGON.SC.XML`, itself a `Peaceful` unit): `Trader`,
`CartTrader`, `Tradecar_Donkey`, `Camel` and `ShipS`. `Wagon` declares
`max_load="1000"`, `feeds="0"`, `damage_type="none"`, and the methods
`unload`, `loadgoldbig`/`loadfoodbig`/`loadgoldsmall`/`loadfoodsmall` and
`follow`. None of the caravans declares a `sight`, so they inherit `Unit`'s
500; `ShipS` declares 400.

A settlement's mule is a race's class (0x005eed10, names loaded at
0x005efd71..0x005efdcf): `Trader` by default, `Tradecar_Donkey` for races 1,
4 and 5, `Camel` for 2 and 6, `CartTrader` for 3 and 7; a boat is always
`ShipS`.

## The fields

| offset | save key | meaning |
|---|---|---|
| `+0x1cc` | `amount` | the cargo |
| `+0x1d4` | (the type key) | the cargo's kind: 0 gold, 1 food |
| `+0x1d8` | `loyalty` | loyalty, in tenths of a point |
| `+0x1dc` | `loyalty-cap` | the loyalty change pending for the next window |
| `+0x1d0` | `capture` | the capture flag: 1 while a capture is running |

The wagon's serialiser (0x005eb3a0) writes and reads all five under those
keys; it reads `capture` only from a saved game written by a build newer than
100, and takes it as 0 otherwise. A map places a wagon with `amount` and its
type only (the map reader 0x005ec690): loyalty is never authored.

The kind is **not cleared** when the cargo is spent: an emptied gold mule
still reports `restype == 0`.

## The constants

`DATA\CONST.INI`, `[GamePlay]`, read together the first time a wagon is
built or loaded (0x005eaef0):

| key | shipped | use |
|---|---|---|
| `WagonLoyaltyInterval` | 1000 | timer 9's period |
| `WagonLoyaltyCapInterval` | 500 | timer 10's period |
| `WagonLoyaltyChangeCap` | 5 | timer 10 moves loyalty at most ten times this, in tenths |
| `WagonLoyaltyIncreasePerUnit` | 10 | tenths per guard, at every timer 9 |
| `LoyaltyWagonsInitial` | 100 | the loyalty a wagon starts at and is reset to -- **in tenths, used as it stands**, so `loyalty` reads 10 |

The settlement's own `LoyaltySettlementsInitial` is multiplied by ten before
it is stored; the wagon's is not.

## Construction

The constructor's post-step (vtable slot `+0x2c`, 0x005eae80) sets loyalty to
`LoyaltyWagonsInitial` and the pending change to 0. The capture flag and the
cargo start at 0 (0x005eb170). No timer of the loyalty pair is armed.

## The cargo setter, and the emptied wagon's death

Every write of the cargo goes through one setter (0x005eb0a0), in this order:

1. `+0x1cc` takes the new value, **unclamped** (neither at zero nor at the
   class's `max_load`);
2. the carrying-food flag `0x20000000` is raised when the value is positive
   and the kind is food, and lowered otherwise;
3. **a value of exactly zero kills the wagon** -- its death virtual (vtable
   `+0xb0`, the unit death 0x005db270, which zeroes the health and runs the
   class's `ondie`) -- unless the wagon is already dead (`+0x50`, health 0) or
   the session flag at `0x00a8734c` (the editor) is set.

Its callers: `Wagon::SetFood`/`SetGold` (0x005ebe70/0x005ebec0, which write
the kind first, 1 or 0), `Wagon::LoadFood`/`LoadGold`, the mule constructor
0x005eed90, `Settlement::UnloadWagon` (0x005ef490, no shipped site), and the
two feeding paths below. So `WAGON_UNLOAD.VS`'s closing
`.SetFood(0); .SetGold(0); .Erase();` kills the mule at the first call; the
second finds it dead and only writes the field.

## Loyalty and capture

Three entry points write the loyalty fields, and two timers:

* `Wagon::loyalty` (0x005ec090): `+0x1d8 / 10`; 0, with a print, for a handle
  that does not resolve.
* `Wagon::SetLoyalty(v)` (0x005ec100): `+0x1d8 = 10 * v` for `0 <= v <= 100`;
  otherwise "Loyalty must be between 0 and 100" and nothing written. The
  pending change and the flag are not touched.
* `Wagon::DecreaseLoyalty(n)` (0x005ec160): a negative `n` prints "Decrease
  amount must be positive" and writes nothing. Otherwise, if the capture flag
  is down, it arms timer 9 at `WagonLoyaltyInterval` and then timer 10 at
  `WagonLoyaltyCapInterval` (0x00688240, relative to now) and raises the flag;
  then the pending change drops by `10 * n`.
* `Wagon::StartCapture` (0x005ec210) is the arming half alone and
  `Wagon::StopCapture` (0x005ec280) kills both timers, resets loyalty to the
  initial value and lowers the flag. No shipped script calls either.

**Timer 9, the guard count** (vtable `+0x130`, 0x005ec410). Two sweeps of the
same circle -- the wagon's stored position (`vtbl+0x3c`, `(-1, -1)` for a
held wagon) and a radius of `[wagon+0xd0] * 160 / 100`, the instance sight
(500 for a caravan, so 800), truncated -- over the map grid, testing each
object by squared distance:

* the **guards** (0x005eb760): objects whose owner bit is the wagon's own
  player's (`[player+0xc]`, that player's one bit -- not its allies), that
  cast to a script object, are not dead, are heirs of `Military` and are not
  templates (`0x08000000`). Hidden units count; sentries and rams count.
* the **threat** (0x005eb9d0, the sweep the settlement's gain tick also uses):
  objects of every player the wagon's owner is at war with (`[player+0x10]`,
  the owner's own row read one way), alive, heirs of `Military`, neither
  hidden nor templates (`0x08200000`), and neither a `Sentry` nor a `RamUnit`.

No threat: both timers are killed, loyalty is reset to `LoyaltyWagonsInitial`
and the flag lowered -- **the pending change is left as it is**, and carries
into the next capture. Otherwise the pending change gains
`WagonLoyaltyIncreasePerUnit * guards` and timer 9 is re-armed
(0x00688220).

**Timer 10, the cap** (vtable `+0x134`, 0x005eb100): the pending change,
clamped to `±10 * WagonLoyaltyChangeCap`, is added to the loyalty; a negative
result becomes 0, and otherwise the result is held at or below
`LoyaltyWagonsInitial` (not 1000). The pending change is cleared -- whatever
the clamp cut off is lost -- and timer 10 re-armed.

**Order on a shared instant.** The two are on the game's timing wheel, which
fires a millisecond's timers in the order they were armed. Timer 9 is armed
before timer 10, and every re-arm of 10 lands in a bucket 9 was armed into
earlier, so whenever both fall due together 9 runs first; when it ends the
capture, it kills the 10 due on the same millisecond.

**The capture scripts.** `UNIT_CAPTURE.VS` and `HERO_CAPTURE.VS` walk to the
wagon (within 150), take it at once when neither side counts the other an
enemy, take it when `w.loyalty == 0`, and otherwise taunt for two seconds
(`Taunt(2000)`) and call `DecreaseLoyalty(1)`. So against an unguarded
caravan each taunt lands 10 tenths at the next window and the caravan falls
after ten taunts, about twenty seconds; each of its owner's soldiers within
800 gives back 10 tenths a second, so a single guard holds it against one
capturer. `w.SetPlayer` does not touch loyalty, and the capture flag stays up
after the wagon changes hands: the next guard count runs for the new owner.

## Loading, and the mule a settlement makes

`Wagon::LoadFood(n)` / `LoadGold(n)` (0x005ebf10 / 0x005ebfd0) find a town
hall (0x005ebc90):

* a wagon in a holder: the holder's settlement's central building, if it
  casts to `CVXTownHall`, and the wagon's owner grants that hall's owner
  share-support (bit 2 of its relations word, 0x005eb070);
* a wagon in the open: the first building the grid sweep of its sight finds
  (0x005eb4b0, cells in row order) that casts to `CVXTownHall`, stands within
  the wagon's sight (0x005a7870) and whose owner the wagon's grants
  share-support.

Then, if the wagon is empty or already carrying this kind, the request is
clamped to `max_load - cargo` (a negative one to 0), the kind written, the
hall's store drawn by up to that (0x005eec70), and the cargo set to the old
cargo plus what was drawn -- through the setter, so an empty mule that draws
nothing dies.

`Settlement::CreateMuleGold(n)` / `CreateMuleFood(n)` (0x005ef140 /
0x005ef040 -> 0x005eed90): `n` clamped to the race's mule's `max_load` and
then to the store; the store drawn by it; nothing drawn, no mule (the invalid
handle). Otherwise a mule is made (0x0059dcd0), its kind written, its cargo
set through the setter, its owner set to the settlement's player
(`vtbl+0xa0`), it is forced into the settlement's holder (0x005d3e10 with 1)
and handed to the AI's new-unit hook (0x0041f310). The script then orders it
out (`VILLAGE_BEHAVIOR_SUPPLY.VS`: `unload` at the supplied settlement, then
`unload` back home).

## Feeding from a wagon

A hungry unit's food search (0x005dbba0, with the predicate 0x005d58d0)
sweeps for a settlement building with food, a `CVXWagon` carrying food (the
fullest), or another unit with food; the draw (0x005d3700) takes from the
settlement first, else from the wagon -- `min(want, cargo)`, through the
setter, so the wagon that gives its last food dies -- else half a unit's
food. A unit in a holder also eats, one at a time, from a food wagon in the
same holder (0x00518da0). That search is the feeder's design, not the
wagon's; it is recorded here because it is the only reader of a following
wagon's cargo.

## The squad's food wagon

`Squad::FoodComing` (0x0042a5b0) sums the cargo of every `CVXWagon` member of
the player's peaceful squads (from index 1) carrying food whose running
command is an object command aimed at a unit of the asking squad.
`Squad::SendFoodWagon(amount, range)` (0x0042a750) first looks among the same
wagons for one carrying no more than `amount` whose command is not aimed at a
unit (and, when aimed at a building, only at a `BaseTownhall` settlement's),
the nearest within `range` of the leader; then the nearest own settlement
with at least 100 food within `range`; prefers the wagon unless the
settlement is 1,200 nearer; loads a mule through 0x005eed90 and orders it
`follow` on the leader.

## What this engine models

* **Loyalty and capture**, whole: `EconomySystem::wagon_loyalty_` holds a row
  per wagon a script has touched (`WagonLoyalty`), a wagon with no row being
  in the constructed state; the timers count in the economy's event-stepped
  clock and are armed from the script's own instant. `StartCapture` and
  `StopCapture` are not bound (no shipped site).
* **The cargo** is `ObjectState::cargo` / `cargo_resource` on the object, and
  every script write goes through `set_wagon_cargo`, the setter: an emptied
  wagon dies through `CombatSystem::kill`, the unit death with no blow. The
  carrying-food flag is not kept (only the state dump reads it).
* **Loading** has both arms; the open arm walks the world's grid in its
  order: rows, then cells, then each cell's list in the order its objects came
  into it (`spatial-index.md`).
* **A settlement's mule and the squad's food wagon are world objects**:
  `make_mule` is 0x005eed90 (the race's class, both clamps, the draw, the
  owner, into the holder), behind `CreateMule*`, `CreateBoat*` and
  `SendFoodWagon`; the calling script walks the mule where it goes, and
  `WAGON_UNLOAD.VS` delivers it. `FoodComing` and `SendFoodWagon` walk the
  player's peaceful squads and the wagons' running commands. The mule is
  handed to the AI register (0x0041f310) last, inside its holder, and filed
  in a squad there and then (`class-tally.md`). One difference remains: a
  negative request loads nothing.
* **Feeding from a wagon** is the original's search (0x005dbba0,
  `FeederSystem::search_open_field`): a hungry unit in no holder looks over
  its sight plus 512 -- the buildings of settlements whose owner grants it
  share-support, in the store's and then the deque's order, then the world's
  grid in its walk order for whatever carries food -- and keeps the store
  (one holding enough ends the grid walk), the fullest food wagon, and a fed
  comrade (the first holding enough, else the fullest), every one of them
  one its player grants share-support and it can see (0x005a7870). The draw
  takes from the store, else `min(want, cargo)` from the wagon through the
  setter, else half the comrade's food at most. Not modelled: the catapult
  crew's arm, and 0x00440100's exception for a tent whose holder names an
  object (every Teuton tent's settlement is passed over).

## What is still unknown

* The vtable dispatch from a timer number to `+0x130`/`+0x134` was inferred
  from the two slots holding exactly the two bodies; the dispatcher itself was
  not read.
* What the carrying-food flag `0x20000000` is read by, beyond the state dump.
