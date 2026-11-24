# The blow: what gbr.exe does when one object strikes another

Read end to end from `gbr.exe`: the dealer 0x00511550 (body to 0x00511b34) and
the calculator it calls, 0x0050ff30 (body to 0x00511545), with their callees.
Every decision below was checked instruction by instruction: which object
`IsHero`, `IsRanged`, `IsSentry` and the skill lookups are asked about is the
`this` of a thiscall, and the level-factor routine takes the difference in a
register, and both change the meaning.

## Conventions

* **A** is the dealer's first argument (the striker: the melee unit, or the
  shooter when a projectile lands), **D** its second (the struck object).
  **A-unit** / **D-unit** are the same pointers when the object carries the
  unit bit (`[obj+0x2c] & 0x400000`, `kSyncUnit`, tested by 0x004091f0), else
  null. Both routines make this cast first, independently.
* "Toward zero" is C integer division. Every division in both routines is
  signed and truncates toward zero; the magic-number divisions (by 5, 10 and
  100) carry the sign fix, and halvings use the add-the-sign-bit-then-shift
  idiom. The level factor's `/256` is the same: a signed division toward zero.
* **RNG** is the game object's generator at `[game+0x12a0]`, slot `+0x14`,
  0x0063ff00: `Range(lo, hi)`, inclusive at both ends, one LCG step per call
  (none when `lo == hi`). It is the engine's `Rng::between(lo, hi)`. Every draw
  in the blow is on this one generator. **0x006ac5c0 and 0x006ac740 are not a
  generator**: they are the environment ledger's string read and its integer
  read (`EnvReadInt`, an `atol` of the stored string).
* **Hero of X** (0x005d37b0 / 0x005d37f0): X itself when it carries the hero
  bit (`[obj+0x2c] & 0x1000000`), else the hero whose handle is at
  `[X+0x170]`, else none. `points(X, s)` is the signed byte `[hero+0x1fc+s]`,
  or **-1** with no hero. `active(X, s)` is "the timed-skill slot
  `[hero+0x218+4*s]` is not -1", false with no hero. Skills used: 10
  Ceasefire, 20 Assault, 22 Concealment, 24 Euphoria (the order of
  `SKILLS.INI`, engine `kSkillNames`).
* **IsHero(X)** is 0x004179e0 or 0x004091f0(0x1000000) on X: the hero bit.
  **IsRanged(X)** is 0x0053f5b0: the class's `projectile_class` string
  (`[class+0x960]`) is non-empty and not `**Invalid**`. **IsSentry(X)** is
  0x005400c0 (heir of class `Sentry`), **IsRam(X)** 0x00540460 (heir of
  `RamUnit`).
* **Specials** are bits of the low word `[unit+0x198]` (the 36-name table,
  engine `kUnitSpecialNames`): 0x1 Parry, 0x2 Drain, 0x4 Ferocity, 0x8 Triple
  strike, 0x10 Deflection, 0x40 Life steal, 0x80 Determination, 0x100 Combat
  skill, 0x400 Offensive tactics, 0x800 Defensive tactics, 0x1000
  Penetration, 0x2000 Spike armor, 0x4000 Bleeding attack, 0x8000 Attack
  skill, 0x10000 Defense skill, 0x20000 Death blow, 0x40000 Power strike,
  0x80000 Expertise, 0x100000 Charge, 0x400000 Rage, 0x800000 Revenge,
  0x1000000 Triumph, 0x2000000 Disease attack, 0x10000000 Learning, 0x80000000
  Cripple. The high word `[unit+0x19c]` is loaded for both sides and every
  test masks it with 0: none of Healing, Keen sight, Trample, Curse is read.
  A's specials are 0 when A is not a unit; D's are read from D-unit.
* "**Pays X**": the running stamina of that side is at least the constant `X`
  (signed compare) and the constant is subtracted. Every special below that
  says "pays" is tested **and** charged in one step, in the order given, on
  the running stamina, so an earlier special can starve a later one.

### Fields

| field | meaning | engine name |
|---|---|---|
| `[obj+0x2c]` | sync flags: 0x400000 unit, 0x800000 building, 0x1000000 hero, 0x10000000 training, 0x4000000 civilian immunity | `kSync*`, `ObjectFlags` |
| `[obj+0x3c]` | class descriptor | `class_index` |
| `[obj+0x70]` | owner's player record; its `+8` is the 0-based player index | `owner` |
| `[obj+0xc0]` | health | `Combatant::health` |
| `[obj+0xc4]` | stamina | `Combatant::stamina` |
| `[obj+0xc8]` | maximum health | `StatBlock::max_health` |
| `[obj+0xcc]` | maximum stamina | `StatBlock::max_stamina` |
| `[obj+0xdc]` | attack | `StatBlock::attack` (`attack_of`) |
| `[obj+0xe4]` | armour against slash | `StatBlock::armour_slash` |
| `[unit+0x180]` | experience, running total | `HeroSystem::experience` |
| `[unit+0x184]` | stored level, `vtbl+0x114` (0x00511c60 reads it bare) | `Combatant::level` |
| `[unit+0x194]` | second flag word: bit 0 blessed, bit 1 diseased, bit 21 beast (`strikes_anyone`), bit 27 cursed, bit 28 sheltered (`half_damage`) | `ObjectFlags` |
| `[unit+0x198]` | specials, low word | `class_has_special` |
| `[unit+0x1a0]` | parry mode, 0 or 1 | `ObjectState::parry_mode` |
| `[unit+0x1a4]` | last attack time (the strike stamps it after the blow, 0x005dc8a0) | `Combatant::last_attack_time` |
| `[unit+0x1c4]` | handle of the disease visual, 0xffff none | -- |
| `[unit+0x1c6]` | handle of the bless/curse visual, 0xffff none | -- |
| `[class+0x294]` | class `maxhealth` | `profile().max_health` |
| `[class+0x2a0]` | class `damage` | -- |

**The blow reads one armour field, `[obj+0xe4]`, and nothing else.** No
instruction in either routine reads `[obj+0xe8]` (pierce), and the projectile
landing (0x004d9bb0 -> 0x00511550) goes through the same dealer, so an arrow
meets the slash armour too. The blow also reads no `damage_type`, no counter
table and no `DamageAmplify` (that key's one reader is 0x005c0bb0, settlement
code, not examined here).

### Constants

All `CONST.INI` `[GamePlay]`, read through 0x0063f200 into globals.

* Read **once per process** by the calculator (guard: the global at 0x00821170
  still -1): `AvoidChancePercentPerConcealmentLevel`, `PercentPerEuphoriaLevel`,
  `EuphoriaStamina`, `ChargeDamageFactor`, `ChargeTime`, and the costs
  `OffensiveTacticsCost`, `DefensiveTacticsCost`, `TrippleStrikeCost` (sic),
  `DeflectionCost`, `PenetrationCost`, `AttackSkillCost`, `DefenseSkillCost`,
  `DeathBlowCost`, `PowerStrikeCost`, `ExpertiseCost`, `RageCost`,
  `BleedingAttackCost`, `LifeStealCost`, `DrainCost`, `CripplingAttackCost`,
  `SpikeArmorCost`, `ChargeCost`, `TriumphCost`, `LearningCost`, `DiseaseCost`,
  and `ArcherAgainstHeroChance`. There is no `RevengeCost`, `FerocityCost`,
  `DeterminationCost` or `CombatSkillCost` read.
* Read once per process by the dealer (guard: `MinDamage` global below 0):
  `MinDamage`, `MinPercentOfAttackersDamage`; and, on the first training blow,
  `TrainingExpChance`.
* `LevelDifferenceDamageTable`, once per process, into the factor table below.

**The match cache** (guard: the dword at 0x00996ae8 is 0; the calculator sets
it to 1 and fills the rest). It is cleared by `InvalidateDamageFormulaParams`
(0x004c6b10, called by `ONFINISH_RESEARCH.VS` after five researches), by the
game load 0x00524460, by the new-game path 0x00525dd0, and by both difficulty
writers: 0x006e71c0, which stores the difficulty and clears the flag
unconditionally and which the script entry point `SetDifficulty` (0x004c65d0)
calls for a value of 0, 1 or 2 (any other is refused with its diagnostic
before either), and the options screen's 0x006e7ff0, which clears it only
when the difficulty differs. The flag's other cross-references are the
calculator's own test and set (0x00510353, 0x00510369). Nothing saves the
cache. Until the next clear, the values are frozen:

1. **The favoured player and its addend** (0x00510351..0x00510432). The
   favoured index starts at -10 (matches no player). When the game object's
   word `[game+0xa0]` is 1 (believed to mark a single-player game; not
   confirmed here), it becomes the player index of the local player's record
   (`[[game+0x12c8]+8]`), and the addend is read by the difficulty
   `[game+0x254]` (0, 1, 2): `EasyDifficultyLevelAddend`,
   `NormalDifficultyLevelAddend`, `HardDifficultyLevelAddend`. Any other
   difficulty value leaves the addend global as it was. If the addend is then
   0, the favoured index becomes -11 (again matches no one).
2. **Per player, 1..16** (0x0051043c..0x005106b0), from the ledger path
   `/<map>/Player<n>/...`: the integers `GamblersLuckChance` and
   `GamblersTestChance` (`EnvReadInt`), and three flags that are true when
   the string entry is exactly `researched` (an 11-byte compare, the
   terminator included): `Attack focus`, `Learning`, `Ferocity`. They are
   stored by 0-based player index and looked up with `[[obj+0x70]+8]`.

So a research that sets one of these takes effect only once a script calls
`InvalidateDamageFormulaParams` (or the game reloads or the difficulty
changes).

### The level factor (0x0050fd40, 0x0050fe60)

A table `T[0..100]` in 256ths, built once. The string is scanned as repeated
`int,int` pairs (C `%i`, so a leading 0 would read as octal and `0x` as hex;
the shipped table has neither): breakpoint `x` (a level difference) and
percent `p`. Starting from the implicit point `(x0, p0) = (0, 0)`, for each
pair `(x1, p1)` with `span = x1 - x0 > 0` and each `k` in `0 .. span-1`:

    T[x0 + k] = 256 + ((p1 - p0) * k + span * p0) * 256 / (span * 100)

one signed division, toward zero. A pair that fails to scan is taken as
`(100, p0)` and ends the table. The loop stops once `x1 >= 100`; then
`T[100] = 256 + p_last * 256 / 100` (toward zero). Breakpoints are assumed
increasing and the last at or past 100; nothing checks it.

Worked from the shipped table (`1,5 5,20 10,40 20,60 40,80 100,90`):
`T[0] = 256` (no change); `T[1] = 268`; `T[3] = 288` (12.5%); `T[100] = 486`.

**Applying it**, `factor(value)` with the level difference `d` in a register
(ESI): `|d|` clamped to 100; `F = T[|d|]`, or `512 - T[|d|]` when `d < 0`;
result `F * value / 256`, signed, toward zero. So `d = 0` returns `value`
exactly, a positive difference raises it by up to 90%, a negative one lowers
it symmetrically. The interpolation is in 256ths in one step, not in whole
percents.

## Part 1. The dealer, up to the call (0x00511550)

**D1. Casts** (0x0051156c..0x005115bb). A-unit and D-unit as above.

**D2. Constants** (0x005115bf): `MinDamage` and `MinPercentOfAttackersDamage`
on first use.

**D3. The training blow** (0x0051161f..0x0051177c), taken instead of the
calculator when **all** hold: A-unit and D-unit exist; `Obj::IsEnemy`
(0x005a78e0, on A, about D) answers no; A's `[unit+0x194]` bit 21 (the
beast's `strikes_anyone`) is clear. Nothing of the calculator runs: no
ceasefire, no concealment draw, no specials, no stamina.

1. `TrainingExpChance` on first use (0x0051164f).
2. `cap = D.health - D.maxhealth / 10` (0x0051169b..0x005116c5).
3. `floor = A.attack * MinPercentOfAttackersDamage / 100` (0x005116ae..0x005116d6).
4. The difference `d = level(A) - level(D)`, both stored levels
   (`vtbl+0x114`, D's read first). **No difficulty addend, no Attack focus.**
5. `value = factor(A.attack - D.armour_slash)` with that `d` (0x0051170b):
   the factor is applied to the raw difference, which may be negative.
6. `floor` raised to `MinDamage`; `cap` raised to 0; `value` raised to
   `floor`, then lowered to `cap` (0x00511712..0x00511730). The floor wins
   over a negative difference; the cap wins over the floor.
7. `value == 0`: return, no draw (0x00511734).
8. **Draw** `Range(0, 99)` (0x00511750). When `TrainingExpChance > draw`:
   A's `SetExperience(experience + 1, refresh)` (`vtbl+0x118`, 0x005dbf70).
   No `max_battle_level` cap, no hero pool.
9. D's damage virtual `vtbl+0xa8` (0x005a7720) with `value`
   (0x00511776): civilian-immune objects take nothing; a dead one is left
   alone; the amount is clamped to the health left, stored, and a unit now
   dead dies. No score, no feedback, no `onkill`, and D's stamina untouched.

**D4.** Otherwise the calculator is called (0x005117ab) with A, D and eight
out-parameters; see the table in Part 3 for where each lives.

## Part 2. The calculator (0x0050ff30)

Steps C1..C47, in the binary's order. `L`, `dmg`, `As`, `Ah`, `Ds`, `Dh` are
the running values: A's level, the damage, A's stamina and health, D's
stamina and health.

### Gates

**C1. Outputs reset** (0x005106b5..0x00510717). `p3 p4 p5 p6 p7 p8` = 0, the
first slot of each effect list `p9`, `p10` = -1. Every early return below
leaves these as they are unless it says otherwise.

**C2. Casts** (0x0051071d..0x0051076c), A-unit and D-unit again.

**C3. Attacker ceasefire** (0x00510776): A-unit and `active(A, Ceasefire)`:
return, nothing.

**C4. Defender ceasefire** (0x0051078b): D-unit and `active(D, Ceasefire)`:
return, nothing.

**C5. Concealment** (0x0051079c..0x005107c7), whenever D-unit exists and C4
did not return: `chance = points(D, Concealment) * AvoidChancePercentPerConcealmentLevel`
(a missing hero gives -1 points, so a negative chance); **draw**
`Range(0, 99)`, **unconditionally**; `chance > draw`: return, nothing (the
blow is dodged). Every blow on a unit not under ceasefire takes this draw.

**C6. A cursed attacker** (0x005107d1): A-unit with `[unit+0x194]` bit 27:
0x005d4800 on A, return. 0x005d4800 clears the blessing bit 0 if it is set,
**otherwise** the curse bit 27, and destroys the visual whose handle is at
`[unit+0x1c6]` (writing 0xffff). So a curse costs exactly one blow and is
then gone. (Bless and curse are exclusive: their writer 0x005d4880 lifts one
before setting the other.)

**C7. A parrying attacker** (0x005107ef..0x00510817): A-unit with parry mode
`[unit+0x1a0] != 0`: `p5 = 1` when A's stamina is below its maximum, else 0;
return. A unit in parry mode deals nothing and regains one stamina per blow.

**C8. A's level** (0x00510820..0x00510851). `L = 10`. With A-unit, `L =`
A's stored level, plus the cached addend when A's owner index equals the
favoured index.

### Non-unit defender and the ram

**C9. D not a unit** (a building, 0x00510855..0x005108b5).
`v = A.attack`. With A-unit: `v = factor(v)` at `d = L - 10`, and when
IsRanged(A) `v = (v + 1) / 2` (toward zero). `v` lowered to D's health;
`p4 = -v`; return. No armour, no floor, no `MinDamage`, no specials, no
stamina, no draw. Without A-unit the raw attack lands.

**C10. A ram defender** (0x005108b6..0x00510955), D-unit and IsRam(D).
`v = A.attack`. When IsRanged(A): **draw** `Range(0, 220)` (0x005108f4); if
`L + 20 < draw`, return with nothing; else `v = L / 5 + MinDamage`. When not
ranged and A-unit: `v = factor(v)` at `d = L - 10`. `v` lowered to D's
health; `p4 = -v`; return.

### The unit-on-unit blow

**C11. Snapshot** (0x00510956..0x005109b1): `Ds = D.stamina`,
D's max health, D's specials, `As = A.stamina` (read off A whether or not it
is a unit), A's specials (0 without A-unit).

**C12. A parrying defender** (0x005109ab..0x005109cb): `Ds != 0` and D's parry
mode set: `p6 = -1`, return. The blow is parried for one stamina; a
parrying defender with no stamina is struck normally.

**C13. Deflection** (0x005109d9..0x00510aa3), when D has Deflection,
`Ds >= DeflectionCost`, and **not** (A has Penetration and
`As >= PenetrationCost`) -- Penetration beats Deflection without being paid
for. Then:

1. `Ds -= DeflectionCost`; effect 5 (Deflection) on D's list.
2. If D is not a hero and `Ds != 0` (0x00510a21..0x00510a30): A with Cripple
   that pays `CripplingAttackCost` sets `Ds = 0`; **else** A with Drain that
   pays `DrainCost` sets `Ds = max(Ds - 2, 0)`. Either way effect 7
   (StaminaLoss) on D's list. Then `p5 = As - A.stamina` (0x00510a6e).
   When D is a hero or `Ds` reached 0, `p5` stays 0.
3. `p6 = Ds - D.stamina`; D's list terminated; return. No damage.

**C14. The ranged miss test** (0x00510aa4..0x00510b20), when A-unit and
IsRanged(A):

1. **Draw** `Range(-(D.maxhealth / 2), D.maxhealth)` (0x00510ae6). When
   `D.health < draw` the shot misses (go to C15). So a shot never misses a
   unit at full health and misses a wounded one with probability
   `(M - h) / (M + M/2 + 1)` for `M` the maximum and `h` the health.
2. Otherwise, when D is a hero: **draw** `Range(0, 99)` (0x00510b17); when
   `ArcherAgainstHeroChance > draw` the shot misses (C15).
3. Otherwise it hits (C16).

**C15. A miss still costs the attacker** (0x00510b26..0x00510c75). On `As`
alone, in order, each "pays" independently: Attack skill
(`AttackSkillCost`), Offensive tactics (`OffensiveTacticsCost`), Penetration
(`PenetrationCost`, only when D is not a hero, A is not under
`active(A, Assault)`, and the `SetGlobalBloodlust` dword at 0x00996ae4 is 0),
Triple strike (`TrippleStrikeCost`), Bleeding attack (`BleedingAttackCost`,
only when D is not a hero), Disease attack (`DiseaseCost`, only when D is
not already diseased), Cripple (`CripplingAttackCost`, D not a hero and
`Ds != 0`), Drain (`DrainCost`, same conditions; **both** may be paid here,
unlike the hit). Then `p5 = As - A.stamina`; return. No effects are listed,
no damage, D untouched.

**C16. Hit setup** (0x00510c76): `Ah = A.health`, `dmg = A.attack`,
`Dh = D.health`.

**C17. Blessing** (0x00510ca3): A-unit with `[unit+0x194]` bit 0: `dmg *= 2`,
then 0x005d4800 on A clears the blessing and destroys its visual. Consumed
only by a blow that reaches here (not by a dodge, parry, deflection or miss).

**C18. Attack skill** (0x00510cb3): A has it and `As >= AttackSkillCost`:
`dmg += 2 * As` with the stamina **before** paying, then pay; effect 21
(OffensiveTactics) on D's list.

**C19. Offensive tactics** (0x00510ce8): A has it and pays
`OffensiveTacticsCost`: `dmg += level(A)` (stored level, no addend); effect
12 (OffensiveTactics) on D's list.

**C20. Does armour apply?** (0x00510d26..0x00510d7d). When D is a hero,
always (C21). When D is not a hero: A-unit under `active(A, Assault)`, or the
bloodlust dword set: **no armour, and no floor** (skip to C22). Else A with
Penetration that pays `PenetrationCost`: no armour and no floor, effect 14
(Penetration) on D's list. Else C21.

**C21. Armour** (0x00510d82..0x00510e22):

1. `dmg -= D.armour_slash` (`[D+0xe4]`, whatever A's damage type).
2. Defense skill (D has it, `Ds >= DefenseSkillCost`): `dmg -= 2 * Ds`
   before paying, then pay from `Ds`; effect 22 (AdditionalDefense) on D's
   list.
3. Defensive tactics (D has it, pays `DefensiveTacticsCost` from `Ds`):
   `dmg -= level(D)` (stored); effect 23 (AdditionalDefense) on D's list.
4. Floor: `dmg` raised to `A.attack * MinPercentOfAttackersDamage / 100`,
   where `A.attack` is re-read raw (`[A+0xdc]`, not doubled by a blessing,
   not raised by C18/C19).

**C22. Level difference** (0x00510e24..0x00510e65). `d = L - level(D)`
(stored). When `d < 0`, A-unit, and A's owner has *Attack focus*: `d = 0`.
**Then** when D's owner is the favoured player, `d -= addend` (so Attack
focus does not protect against the favoured defender's addend).

**C23. Factor** (0x00510e6c): `dmg = factor(dmg)` at `d`.

**C24. Charge** (0x00510e73..0x00510ec1): A has Charge, `As >= ChargeCost`,
and `now - [A+0x1a4]`, compared **unsigned**, is at least `ChargeTime` (`now`
the game clock `[game+0x1258]`): `dmg *= ChargeDamageFactor`, pay; effect 13
(Charge) on D's list. (For a projectile the strike stamped `[A+0x1a4]` at
launch, so at landing the gap is the flight time.)

**C25. Triple strike** (0x00510ec4..0x00510eef): A has it and pays
`TrippleStrikeCost`: `dmg *= 3`; effect 15 (TripleStrike) on **A's** list.

**C26. MinDamage** (0x00510ef6): `dmg` raised to `MinDamage`.

**C27. Hero gate** (0x00510f12): when D is a hero, skip C28..C33 (go to C34).

**C28. Rage** (0x00510f28): A has it, `As >= RageCost`, and
`Ah > A.maxhealth / 2`: `dmg += Ah - A.maxhealth / 2`, `Ah = A.maxhealth / 2`,
pay; effect 10 (Rage) on D's list.

**C29. Bleeding attack** (0x00510f63): A has it and pays
`BleedingAttackCost`: `dmg += D.maxhealth / 10`; effect 9 (BleedingAttack) on
D's list.

**C30..C33. The killing blows.** The first that fires sets `dmg = Dh`,
appends effect 3 (Death) to D's list, and jumps to C35 (skipping the
shelter halving and the cap). They are tried in this order:

* **C30. Death blow** (0x00510fa4): A has it, `As >= DeathBlowCost`, and
  `Dh - dmg <= D.maxhealth / 2`; pays.
* **C31. Power strike** (0x00510fee): A has it, `As >= PowerStrikeCost`,
  `A.maxhealth > 2 * D.maxhealth`, and `Dh < D.maxhealth`; pays.
* **C32. Expertise** (0x0051102c): A has it, `As >= ExpertiseCost`, and
  `dmg > [A's class + 0x2a0]` (the class `damage`, not the stored attack);
  pays.
* **C33. Gambler's Test** (0x00511067), free: A-unit, `L > level(D)`
  (stored, no addend on D), A's owner's cached `GamblersTestChance` is not 0,
  and a **draw** `Range(0, 99)` (0x005110ac, taken only when the first three
  hold) is below the chance.

**C34. Shelter and cap** (0x005110c2..0x005110e3): D's `[unit+0x194]` bit 28:
`dmg /= 2` (toward zero) -- **after** `MinDamage`, so a sheltered unit can
take less than the minimum. Then `dmg` lowered to `Dh`.

**C35. Gambler's Luck** (0x005110e5..0x0051112f): `Dh - dmg <= 0`, D's
owner's cached `GamblersLuckChance` not 0, and a **draw** `Range(0, 99)`
(0x0051111a, only when the first two hold) below it: `dmg = Dh - D.maxhealth`
(zero or negative), effect 0 (Heal) on D's list. The blow that would have
killed restores D to full health instead. Applies to heroes too.

**C36. D's health** (0x00511132..0x00511147): `Dh -= dmg`.

**C37. Triumph, else Life steal** (0x0051114b..0x005111a0): Triumph when A
has it, `Dh == 0`, `Ah < A.maxhealth`, `As >= TriumphCost`: `Ah =
A.maxhealth`, pay. Otherwise Life steal when A has it, `Ah < A.maxhealth`,
`As >= LifeStealCost`: `Ah += dmg`, pay. Either: effect 0 (Heal) on A's list.

**C38. Spike armor** (0x005111a7..0x005111ff): D has it,
`Ds >= SpikeArmorCost`, A-unit, not IsRanged(A): `Ah -= dmg`, pay from `Ds`;
effect 8 (SpikedArmor) on A's list. Fires whether or not D survives.

**C39. Revenge** (0x00511206..0x0051124f): D has it and `Dh == 0`. Without
A-unit, skip straight to C41 (also skipping C40). With IsRanged(A) nothing
happens. Otherwise `Ah -= 100` (a literal; no cost read, no stamina paid),
effect 11 (Revenge) on A's list.

**C40. Ferocity, Euphoria** (0x00511256..0x005112fa), when A-unit, `Dh == 0`
and `As < A.maxstamina`:

* A has Ferocity: `As = A.maxstamina`.
* else A's owner has *Ferocity* researched: `As += EuphoriaStamina`.
* else `points(A, Euphoria) > 0`: **draw** `Range(0, 99)` (0x005112d4,
  only then); when `PercentPerEuphoriaLevel * points > draw`,
  `As += EuphoriaStamina`.

Whichever fires appends effect 6 (StaminaBoost) to A's list.

**C41. Disease attack** (0x00511301..0x00511364): A has it, `As >= DiseaseCost`,
`Dh != 0`, D not already diseased: set D's `[unit+0x194]` bit 1 **directly on
the object**, pay; and when `[D+0x1c4]` is 0xffff, spawn effect 26 (Disease,
or Disease2 for a big class) on D with no expiry (0x00518650 with -1) and
store its handle at `[D+0x1c4]`. This is the calculator's one write to the
world.

**C42. Combat skill** (0x0051136a..0x0051139b): A has it and
`As < A.maxstamina`: `As += 1` (free). It then appends effect 6 to A's list
unless the slot **at A's list cursor** already holds 6. That slot has not
been written in this call: it is the initial -1 when A's list is empty, and
otherwise an uninitialised slot of the dealer's stack array. In practice the
6 is appended (possibly a second 6 after C40's); the test is a bug, and the
only consequence is a feedback object. **Uncertain** only in that the stale
stack word could in principle be 6.

**C43. Cripple, else Drain** (0x005113a2..0x005113f3), when D is not a hero
and `Ds != 0`: Cripple paying `CripplingAttackCost`: `Ds = 0`; else Drain
paying `DrainCost`: `Ds -= 2` (no clamp here; C46 clamps). Either: effect 7
(StaminaLoss) on D's list. The cost is tested on the running `As`, after
everything above.

**C44. Learning** (0x005113f6..0x0051146d): A-unit, not IsSentry(D),
`[A+0x180] < [D+0x180]` (raw experience totals, not levels),
`As >= LearningCost`, and (A has Learning or A's owner has *Learning*
researched): `p7 = 1`, pay; effect 2 (Experience) on A's list. No kill
needed.

**C45. Determination** (0x00511474..0x00511496): D has it and
`Ds < D.maxstamina`: `Ds += 1` (free); effect 6 on D's list.

**C46. Clamps** (0x00511499..0x005114e0): `Ah` into `[0, A.maxhealth]`,
`Ds` into `[0, D.maxstamina]`, `As` into `[0, A.maxstamina]`. `Dh` is not
clamped (C34 and the killing blows keep it in `[0, max]`).

**C47. Outputs** (0x005114e2..0x00511535): `p3 = Ah - A.health`,
`p5 = As - A.stamina`, A's list terminated, `p4 = Dh - D.health`,
`p6 = Ds - D.stamina`, D's list terminated.

## Part 3. The outputs and what the dealer does with them

| param | dealer slot | written by | meaning | dealer's use |
|---|---|---|---|---|
| p3 | `[E+0x18]` | C47 | A's health delta (Rage, Life steal, Triumph, Spike armor, Revenge) | D11: A `vtbl+0xac` |
| p4 | `[E+0x10]` | C9, C10, C47 | D's health delta: minus the damage; positive after Gambler's Luck | D6 score, D7 |
| p5 | `[E+0x1c]` | C7, C13, C15, C47 | A's stamina delta | D11: A `SetStamina` |
| p6 | `[E+0x20]` | C12, C13, C47 | D's stamina delta | D8, only if D survived |
| p7 | `[E+0x24]` | C44 | Learning's 1 point | D10 |
| p8 | `[E+0x14]` | C1 only (0) | nothing; the dealer reuses the slot (first its D-unit local, then the kill gain) | D9, D10 |
| p9 | `[E+0x28]`, 8 ints | C25, C37, C38, C39, C40, C42, C44 | effect ids on **A**, -1 terminated | D5 |
| p10 | `[E+0x48]`, 8 ints | C13, C18..C21, C24, C28..C35, C43, C45 | effect ids on **D**, -1 terminated | D6 |

(`E` is the dealer's stack after its prologue.) D's list can in principle
hold more than its 8 slots (11 ids plus the terminator with every special at
once) and would run into the dealer's path buffer; harmless in the original,
a port should simply not cap it.

Effect ids, from the factory 0x00518650 (table at 0x00821180; the second
name is the variant for a class whose `[class+0x2dc]` radius exceeds 20):
0 Heal, 2 Experience, 3 Death, 5 Deflection, 6 StaminaBoost, 7 StaminaLoss,
8 SpikedArmor, 9 BleedingAttack, 10 Rage, 11 Revenge, 12 and 21
OffensiveTactics, 13 Charge, 14 Penetration, 15 TripleStrike, 22 and 23
AdditionalDefense, 26 Disease / Disease2. Each spawn (0x005183e0) creates a
`CVXFeedback` object and returns its handle; the dealer's (lifetime argument
0) expire by their own timer, the disease visual (-1) never does.

**The feedback object takes an asynchronous handle** (round 17, batch 9).
The original's handle table (pointers at 0x00a2a610, next links at
0x00a0a608, 65,536 slots) holds two free lists, saved under the names
`FirstFreeSync`, `LastFreeSync`, `FirstFreeAsync` and `LastFreeAsync`
(0x0067b5f0). The synchronous list is slots 0..39,999 (built by 0x0067b510,
taken by 0x0067b410), the asynchronous one 40,000..65,534 (0x0067b580,
0x0067b480); a freed handle goes to the tail of its own list (0x0067b660),
and a reset clears every slot and builds both lists again (0x0067b6e0). An
object is given its handle in the innermost of its base constructors
(0x00531840), from the synchronous list when its constructor asks for a
synchronised object and the asynchronous one otherwise, and gives it none
(0xffff) when it asks for no handle. A unit asks for the synchronous list
(0x005d3170, through 0x005b4ca0); `CVXFeedback`'s constructor (0x005180d0,
which 0x005183e0 runs) asks for the asynchronous one, and so does its load
constructor (0x00518520). **A shot asks for none**: the missile's
constructor (0x004d9740, which both shooters run, 0x005dc650 and
0x004df650) passes no-handle to the same chain, and keeps the shooter's and
the target's handles in its own words. It is also put on the second of the game's three
thread managers, `[game+0x1298]`, where every other object is on
`[game+0x1294]`; the game builds the three in 0x00525dd0, and only the
first is given a checksum word (0x009a7058), which every message posted
to one of its objects folds in (0x00688150). So **a feedback object moves
neither the synchronous handles nor the checksum**: a unit or a shot spawned
after a blow with a special takes the handle it would have taken without
it.

Its life: 0x005183e0 stores the expiry, the game clock `[game+0x1258]` plus
the lifetime or -1 for none, at `[obj+0x80]`, the receiving unit's handle at
`[obj+0x70]`, and posts message 0x13 to itself. That message's handler
(0x005181b0, through `vtbl+0xc`, 0x005184b0) destroys the object once the
clock reaches the expiry plus ten plus a duration it reads through
0x0053cee0 (not followed), or when the unit's slot is empty; otherwise it
moves to the unit, and posts itself 0x13 again after the delay at
0x0082117c. Its `vtbl+0x54` (0x005182a0) destroys it as well once the expiry
has passed or the unit answers that it is going. It is saved with the game
(the load constructor) and lives on the unit only through the two handle
words the unit keeps to destroy it: the disease visual's at `[unit+0x1c4]`,
destroyed by the health store at full health (0x005d3a80), and the
bless or curse visual's at `[unit+0x1c6]`, destroyed by the cure
(0x005d4800, after it tests the object is a `CVXFeedback`). Nothing reads
either word to decide anything else.

### The dealer after the call

**D5. A's effects** (0x005117b3..0x005117e0): with A-unit, each id of p9 in
order, spawned on A (lifetime 0).

**D6. D's effects and the score** (0x005117e7..0x005118b8).
With D-unit: each id of p10 spawned on D; then, with A-unit, A's owner's
`damage_inflicted` (`[stats+0xa4]`, the per-player record from 0x0051cce0 on
`[game+0x258]`) gains `-p4`; and always D's owner's `damage_taken`
(`[stats+0xa0]`) gains `-p4`. Both **before** the damage is applied, and
`-p4` is negative after Gambler's Luck. Without D-unit (a building): no
score here at all; when A is not IsRanged and D is a building
(`0x800000`), 0x004e0fd0 runs D's fire points with `-p4` (presentation).

**D7. Damage** (0x005118c0..0x005118d0): D's `vtbl+0xac` (0x004d6690 for a
unit) with p4. A negative delta goes to the damage virtual `vtbl+0xa8`
(immune, already dead, clamp, store, death virtual, answer "dead"); zero or
positive heals, capped at the maximum, and answers "not dead".

**D8. D survived** (0x00511a43..0x00511a5c): with D-unit and `p6 != 0`, D's
`SetStamina(stamina + p6)` (`vtbl+0x9c`, 0x005d2960: a raw store when the
value differs). A dying D keeps its stamina.

**D9. D died** (0x005118de..0x005119ee). `H` = D's class `maxhealth`. With
A-unit: A's owner's `[stats+0x44]` += 1 (probably `units_killed`; not
confirmed) and `kill_healths` (`+0xbc`) += `H`. Always: D's owner's
`die_healths` (`+0xc0`) += `H`. With A-unit, D-unit and D not a Sentry, the
**kill gain** as `docs/formats/unit-stats.md` "What a blow pays" gives it
(levels of the running totals by 0x005d28c0, `(D - A) / 6` kept to `[0, 8]`
plus 1, the owner's modifier 0x0050fec0 with its conditional draw, and the
gain added to A's hero's pool `[hero+0x2c4]`). With A-unit, A's `onkill`
hook (0x005b5ca0, receiver A, argument D).

**D10. Experience** (0x005119f3..0x00511ae4): `total = gain + p7`. When not
0: a hero or training A takes `SetExperience(exp + total)`; anyone else
honours its owner's `max_battle_level` as unit-stats.md describes. So the
Learning point is paid by a blow that does not kill.

**D11. A's own deltas** (0x00511aea..0x00511b16), with A-unit: `p3 != 0`
goes through A's `vtbl+0xac` (so Spike armor and Revenge can kill the
attacker, through its death virtual), then `p5 != 0` through
`SetStamina(stamina + p5)`, even on an attacker that just died.

### The draws, in order

For one blow, at most these, in this order:

1. Training only: `Range(0, 99)` when the value is not 0 (D3.8). Nothing else.
2. C5 Concealment, `Range(0, 99)`: always, when D is a unit not under ceasefire
   and A is not under ceasefire.
3. C10 the ram, `Range(0, 220)`: ranged A on a ram (then the blow ends).
4. C14.1 `Range(-(M/2), M)`: ranged A on a unit, after the parry and
   deflection tests.
5. C14.2 `Range(0, 99)`: same, the hero defender, when 4 hit.
6. C33 Gambler's Test, `Range(0, 99)`: as stated.
7. C35 Gambler's Luck, `Range(0, 99)`: as stated.
8. C40 Euphoria, `Range(0, 99)`: as stated.
9. D9 the kill gain's modifier draw (unit-stats.md), after the damage.

**The bloodlust** is a dword at 0x00996ae4. `SetGlobalBloodlust` (0x004c6610)
stores its argument's low byte into it, zero-extended. Its only other
references are the two reads in the calculator (0x00510b8a in C15,
0x00510d4e in C20): no load, new game or save touches it, and no checksum
folds it, so it lives as long as the process.

## What is still unknown

* `[game+0xa0] == 1`: what game mode it marks (single-player is the guess).
* `[stats+0x44]`: named here as `units_killed` by elimination only.
* The duration 0x005181b0 adds to a feedback object's expiry, read through
  0x0053cee0, was not followed.
* The C42 duplicate test reads stale stack memory; its result is assumed
  "append".
* Whether anything outside these two routines reads `[obj+0xe8]` in a fight
  was not searched.
* A `%i` breakpoint that fails to scan leaves the `%n` count stale; with a
  malformed table the scan position is unpredictable. The shipped table is
  well formed.

## This engine

`engine/core/src/sim/combat.cpp`. Ported whole (round 17, batches 4 and 5):

* **The level factor** (0x0050fd40, 0x0050fe60): `LevelDifferenceTable`.
* **The calculator** (C1..C47): `calculate_blow`, a pure function of the two
  sides (`BlowSide`), the cache's addend, the bloodlust and the clock
  (`BlowRules`), `CombatConstants` (every `CONST.INI` key above) and the
  world's generator. Every gate, both other branches, Deflection, the ranged
  miss and its costs, every special and its stamina cost, the clamps and the
  deltas, in the binary's order and with its draws. Its answer
  (`BlowOutcome`) carries both effect lists, uncapped.
* **The sides** (`CombatSystem::blow_side`): the unit and hero bits, IsRanged
  (`CombatProfile::shoots`), IsRam and IsSentry by lineage, the stored stats,
  the stored level, the running experience, the class `damage`, the specials
  word (`CombatProfile::specials`, parsed from `unit_specials` as the class
  reader does), parry mode, the curse (`ObjectState::cursed_at`), the disease
  and shelter bits, the hero of X's Ceasefire, Assault, Concealment and
  Euphoria, and the cache's entries for the owner.
* **The match cache** (`CombatSystem::damage_formula_params`): filled at the
  first blow after a clear, from `MatchSystem::human` in a game that is not
  networked (the reading of `[game+0xa0] == 1` above, **inferred**) and
  `difficulty`, and from the ledger. Cleared by
  `InvalidateDamageFormulaParams`, by a load (`CombatSystem::deserialize`), by
  `SetDifficulty` and by a new game (a fresh system). Not saved, not hashed.
  The addend outlives a clear as the global does. `vtbl+0x114` here is the
  stored level alone, as 0x00511c60 is (the power census and
  `effective_level_of` no longer add an addend).
* **The bloodlust** (`CombatSystem::bloodlust`, set by
  `SetGlobalBloodlust`): a process global, as the original's is
  (`ProcessGlobals`, `sim/process.hpp`; round 17, batch 9). Neither saved nor
  hashed, and a load leaves it as it was. The process is the `ProcessGlobals`
  a session is given (`SessionInputs::process`): the app keeps one for its
  life and gives it to every session it builds, so a game loaded in the app
  keeps what the last one set, as the original does; a session given none
  (`imrun`, `imconform`, `imsave`, the tests) is a fresh process and starts
  at zero. Lockstep peers are each their own process, here as there: they
  agree because the only writer, `STONEHENGE_BLOODLUST.VS`, runs on every
  peer in lockstep. A peer whose process carried the byte in from an earlier
  game, or a late joiner that starts at zero while the others are inside
  the spell's sleep, plays different blows from the others without the hash
  showing it -- which is the original's own behaviour.
* **The feedback objects** (D5, D6, C41's disease visual, the curse's
  visual): `BlowEffect` lists them and nothing is spawned. In the original
  each takes an **asynchronous** handle on the asynchronous thread manager
  (see the effect ids above), so neither the synchronous handles every
  unit, building and shot takes nor the checksum moves; allocating them in
  this engine's one handle space would put every later handle out of step.
  What is not reproduced is the asynchronous space itself and the objects'
  own life in it, which nothing in the simulation reads: presentation.
* **A shot takes a handle here and none in the original** (0x004d9740, see
  the effect ids above): `CombatSystem::strike` spawns each projectile as a
  world object, so every later unit's handle runs one higher per shot than
  the original's. Found in round 17, batch 9, and not changed there: the
  shot's id keys its record and its save and hash, and taking it out of the
  handle space is a port of its own.
* **The dealer** (`CombatSystem::deal`, D1..D11): the training branch
  (`training_blow`), then the calculator; the curse lifted and the disease
  bit set as the calculator does; the score booked raw before the damage
  (`MatchSystem::record_blow_damage`, `record_blow_kill`: no sign test, no
  alliance test); the damage through `take_health` (0x004d6690: a heal
  capped at the maximum that lifts the disease at full health, or the damage
  routine); a survivor's stamina; on a kill the tallies, the gain and
  `onkill`; experience with Learning's point; then the attacker unit's health
  and stamina.
* **The landing** (0x004d9bb0, `CombatSystem::land`): the dealer with the
  live shooter, only while shooter and target stand and the target is not
  held. No splash on a targeted shot.
* **The one stamina**: `ObjectState::stamina` is the truth. Combat takes it
  at each turn and before each blow, writes both copies when a blow moves it,
  and seeds an enrolled combatant from the world's (the map's number) or, at
  zero, the class maximum.

Not ported, and why:

* **The blessing** (C17): read, and never true, because nothing here writes
  bit 0. Its writer is `Unit::Bless` (0x005d9100, which runs 0x005d4880
  with 1), and it has no shipped call site, so it is not bound; `Unit::Curse`
  (0x005d90c0, the same worker with 0) is.
* **The civilian immunity** (`[obj+0x2c] & 0x4000000`) in the damage
  routine: no such bit is carried.
* **The building fire points** (D6 without a defender unit): presentation.
