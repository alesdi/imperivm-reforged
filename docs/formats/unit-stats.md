# A unit's stored stats and their rebuild

**Ours, read off `gbr.exe`.** This is not a file format: it is the record of
how the original keeps the numbers a unit fights with, because every host
entry point that reads one (`Obj::attack`, `Obj::range`, `Obj::maxhealth`,
`Unit::level`, ...) and the damage formula itself read the stored field, and
the field is only as fresh as the last rebuild. The engine models it in
`engine/core/src/sim/stats.cpp` (`recalc_bonuses`), from this description.

## The stored fields

Every object carries these, filled from its class when it is built:

| field | what | the class property it starts from |
|---|---|---|
| `[obj+0xc8]` | maximum health | `maxhealth` (`[class+0x294]`) |
| `[obj+0xcc]` | maximum stamina | `maxstamina` (`[class+0x298]`) |
| `[obj+0xd4]` | range | `range` (`[class+0x2d4]`) |
| `[obj+0xdc]` | attack (`Obj::attack` and `Obj::damage` are one getter) | `damage` (`[class+0x2a0]`) |
| `[obj+0xe0]` | zeroed by every rebuild; no reader found | -- |
| `[obj+0xe4]` | armour against slash | `armor_slash` (`[class+0x2ac]`) |
| `[obj+0xe8]` | armour against pierce | `armor_pierce` (`[class+0x2b0]`) |
| `[unit+0x180]` | experience, the running total | 0 |
| `[unit+0x184]` | the level the game uses (`Unit::level`, `vtbl+0x114`) | -- |

`[obj+0xd8]`, the minimum range, is the class's and no rebuild touches it.

## The rebuild

`Unit::RecalcBonuses` (0x005e0380) is one virtual call, `vtbl+0xc4`, on a
unit: 0x005d9df0. It does nothing while the game is shutting down (the global
at 0xa8734c) and nothing for a dead unit (`vtbl+0x50`). Otherwise, in order:

1. The squad's running strength loses this unit's valuation (0x0041e890); it
   gets it back at the very end (0x0041e900).
2. It remembers the current maximum health, maximum stamina and stored level,
   and starts an experience total of `[unit+0x180]` plus, when the unit has a
   hero (`[unit+0x170]`), the hero's **army addend** `[hero+0x2c0]`, and a
   level addend of 0.
3. **Class.** The six fields of the table above are copied from the class;
   `[obj+0xe0]` is zeroed.
4. **`AddBonus` record.** The side table at 0x9c0914 is searched by the
   unit's handle (0x005e0530); a record adds its attack, maximum health,
   maximum stamina, slash and pierce.
5. **Items**, in inventory order (`vtbl+0x104` count, `vtbl+0xf4` by index).
   **An item whose kind (the item class's name) equals that of an item earlier
   in the inventory is skipped** -- two of the same item give the bonus once.
   Each other item adds: `damage_percent`, `armor_slash_percent`,
   `armor_pierce_percent` and `health_percent` **of the class value** (not of
   the running total), each truncated toward zero; then its flat `damage`,
   `armor_slash`, `armor_pierce`, `health`, a stamina term that `ITEMS.XML`
   cannot set (the loader 0x005361f0 reads no attribute into it), its `level`
   into the level addend and its `experience` into the experience total. The
   bonus block sits at `[item class+0x94]`, in the order health, stamina, an
   unread word, damage, slash, pierce, level, experience, then the four
   percents health, damage, slash, pierce.
6. **Toughness.** When the unit has the `Toughness` special (bit 9 of the
   specials word `[unit+0x198]`), maximum health gains
   `(L - 1) * ToughnessHealthIncrease`, where `L` is the level the raw
   `[unit+0x180]` stands at (0x005d28c0) -- not the stored level, and without
   the hero's addend or the items' experience.
7. **Statue of Amon-Ra.** When the owner's environment entry
   `/<map>/Player<n>/Statue of Amon-Ra` reads exactly `researched`, maximum
   health gains `(L - 1) * StatueOfAmonRaHealthIncrease`, the same `L`.
8. **A hero's own skills.** For a hero (`kSyncHero` in `[obj+0x2c]`): Epic
   Endurance adds `HealthPerEpicEnduranceLevel` maximum health per point,
   Epic Attack `AttackPerEpicAttackLevel` attack per point, Epic Armor
   `DefensePerEpicArmorLevel` to both armours per point. From here on the
   hero is **its own leader**. Anything else takes its hero as leader, and a
   unit with no hero skips straight to step 13.
9. **Formation.** When the unit's current command name (the string at
   `[obj+0x110]`) is exactly `stand_position`, the leader's party formation
   (`[[leader+0x2b0]+0x18]`, the shape `Hero::SetFormation` chose) adds its
   `BonusDamage` to attack, `BonusDefenceSlash` and `BonusDefencePierce` to
   the armours, `BonusRange` **percent of the range** to the range (truncated
   toward zero), and -- not for a hero -- its `BonusLevel` to the level
   addend. Then, **inside this branch only**, attack, both armours and range
   are floored at 0.
10. **Team skills**, the leader's (so a hero's own): Team Attack adds
    `AttackPerTeamAttackLevel` attack per point, Team Defense
    `DefensePerTeamDefenseLevel` to both armours per point.
11. **Defensive Cry** in effect on the leader (its slot at
    `[leader+0x218+4*23]` is not -1): both armours gain `DefensiveCryDefense`.
12. Not for a hero: **Battle Cry** in effect on the leader adds
    `LevelsPerBattleCryLevel * points` to the level addend; **Frenzy** in
    effect doubles the attack.
13. **Level.** The stored level becomes the level the experience total stands
    at (0x005d28c0) plus the level addend, at least 1; then, when the unit's
    hero (`[unit+0x170]`, so never a hero itself) has Discipline, at least
    `DisciplineBonusBase + DisciplineBonusStep * points`.
14. **Rescale.** Health becomes `health * new max / old max` and is stored
    with the plain setter (no clamp, 0x005a7a00); stamina the same when the
    old maximum stamina is above 0 (0x005a7a30). Integer division, truncating
    toward zero.
15. If the stored level changed, the sprite is refreshed (`vtbl+0x5c`).

The constants are `CONST.INI` `[GamePlay]` entries, each read once on first
use. A skill's points are the signed byte `[hero+0x1fc+skill]`; a timed
skill's slot is `[hero+0x218+4*skill]`, the game time it ends, or -1.

## When it runs

Nothing rebuilds on a timer of its own; the stats are as of the last of these.
Each is a call of `vtbl+0xc4` on the unit, or of 0x0052d8e0 (the same call)
for every member of a hero's party through 0x005f5520, the hero first.

| trigger | where |
|---|---|
| the unit is placed in the world | 0x005d3090, after the object start 0x005aec40 |
| `Unit::RecalcBonuses` | 0x005e0380 |
| `Unit::AddBonus`, `Unit::RemoveBonus` | 0x005e0ce0, 0x005e07f0 |
| an item enters the inventory | the add virtual 0x005b60a0, the slot write 0x005ae3c0 (`Obj::GetItem` 0x005ac910 and `ExchangeItem` reach these) |
| an item leaves it | 0x005afc50, unless the holder is dead |
| attached to a hero, detached from one | 0x005ddf50, 0x005db190 |
| `Unit::SetExperience` and every experience award | the virtual 0x005dbf70, when its second argument is set (the entry point 0x005d7610, the blow 0x00511550 and the hero tick 0x0052fc50 all set it) |
| a mutation's fresh object | 0x005deef0 |
| a command named `stand_position` starts or ends | the command advance 0x005b4f20 |
| Team Attack, Team Defense or Discipline written | the skill writer 0x0052def0, the whole party |
| Epic Endurance, Epic Attack or Epic Armor written | the same, the hero alone |
| a timed skill starts | 0x0052d8f0, the whole party, after the skill's own walk |
| a timed skill ends | the expiry message 0x0052de70, the whole party |
| the hero's army addend changes | the hero's tick 0x0052fc50, every army member |
| a hero's attributes are loaded | 0x005307a0, after its skills |

Leadership is not in the skill writer's list: it reaches the army through the
addend, at the next tick.

The hero's tick is its message 0x1c, which reschedules itself every 1,013 ms.
It first converts the hero's experience accumulator `[hero+0x2c4]` into
experience (`SetExperience`), one point per `ExpFromArmyDivider`, then
computes the addend 0x0052d7a0 as `(ArmyBonusExperiencePercent +
ExpPercentPerLeadershipLevel * Leadership) * Exp[stored level of the hero] /
100 + 1`, and only when it differs from `[hero+0x2c0]` stores it and rebuilds
the army.

## The readers

| entry point | reads |
|---|---|
| `Obj::maxhealth` 0x005adb40 | `[obj+0xc8]` |
| `Obj::maxstamina` 0x005adba0 | `[obj+0xcc]` |
| `Obj::range` 0x005adde0 | `[obj+0xd4]` |
| `Obj::attack`, `Obj::damage` 0x005add80 | `[obj+0xdc]` |
| `Unit::level` 0x005d7740 | `vtbl+0x114`, the stored `[unit+0x184]` |
| `Unit::InRange` 0x005d72e0 | `[obj+0xd4]` and `[obj+0xd8]` through 0x005a7800 |
| the blow, 0x0050ff30 | the attacker's `[obj+0xdc]`, the defender's `[obj+0xe4]`/`[obj+0xe8]`, both levels through `vtbl+0x114` |

## Where the engine still differs

* A timed skill's expiry rebuild runs at the end of the turn its slot ends
  in, not on the millisecond (`HeroSystem::expire_effects`).
* The hero's tick runs on multiples of 1,013 ms of the world clock, not
  1,013 ms after the hero was made; its accumulator half has nothing to
  convert, because the engine's one writer of it (Wisdom) pays at once.
* The formation loader's clamp to `[-1000, 1000]` is taken where the rebuild
  reads a bonus, not where `FORMATIONS.XML` is read.
* The amount of experience a blow pays is still the engine's
  (`target level / LevelExpDivider + 1` per damaging blow); only where it
  goes is the original's.

## What is still unknown

* `[obj+0xe0]`, which the rebuild zeroes and nothing found reads.
* The stamina term of an item's bonus block: present, applied, never set by
  any loader found.
* What sets the shutdown flag at 0xa8734c beyond the three writers seen; it
  only matters while the game is closing.
