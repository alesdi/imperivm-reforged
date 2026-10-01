# The `.vs` script language

**Status:** grammar decoded and validated; several run-time semantics inferred, not proven
**Reference implementation:** [`src/imperivm/formats/vs_parse.py`](../../src/imperivm/formats/vs_parse.py)
**Host surface:** [`vs-host-api.md`](vs-host-api.md)
**Companion:** [`sc-xml.md`](sc-xml.md) (the class graph that binds scripts to objects), [`pak.md`](pak.md) (the container)

`.vs` ("VX script") is the engine's embedded scripting language. Every unit behaviour, every
command implementation, the whole economy and tactical AI, the tutorial hints and the victory
conditions are written in it. The engine ships **577 `.vs` files, 768 KB, in `data.pak`**, all
of them plain ASCII source. Nothing is precompiled, so the corpus is the specification.

The language is a small statically typed C dialect with no user-defined functions and no
declarations of its own beyond local variables: a script *is* a function body. Everything else —
types, methods, functions, constants — comes from the host.

## Corpus

| | |
|---|---:|
| Files | 577 |
| Bytes | 786,576 |
| Lines | 30,837 |
| Files parsed by the reference parser | **577 / 577 (100.0%)** |
| Distinct identifiers | 2,081 |
| Call sites | 17,131 |

All files use CRLF line endings and are pure 7-bit ASCII. There is no byte-order mark and no
`#`-style preprocessor anywhere in the corpus; the only `#` character in all 577 files is inside
an English sentence in a tutorial string.

Distribution inside `data.pak`:

| Directory | Files | Role |
|---|---:|---|
| `DATA\SUBAI` | 438 | object command implementations, behaviours, command verifiers |
| `DATA\AI` (+ `\DEFENSIVE`, `\CHAOTIC`, `\DEFAULT`) | 78 | player-level AI: economy, tactics, squad and GAIKA management |
| `DATA\TUTORIALS` | 28 | tutorial hint scripts |
| `DATA\ITEMSCRIPTS` | 18 | item use / equip / kill hooks |
| `DATA\BONUSSCRIPTS` | 12 | per-map starting-bonus scripts |
| `DATA\AI HELPERS` | 4 | reusable AI subroutines |
| `DATA\GAMESCRIPTS` | 4 | victory conditions |
| `DATA\PATHFINDERINIT.VS` | 1 | pathfinder bootstrap |

## Lexical structure

### Comments

| Form | Note |
|---|---|
| `// ...` | to end of line. `///` (220 uses) is a stylistic variant, not a distinct form |
| `/* ... */` | 41 uses. Does not nest — the first `*/` closes it |

The **first** `//` comment of a file is special; see [Signature comment](#signature-comment).

### Tokens

| Class | Form |
|---|---|
| Identifier | `[A-Za-z_][A-Za-z0-9_]*` |
| Integer literal | `[0-9]+`. No hex, no floats, no exponents, no suffixes, no negative literals (unary `-` instead) |
| String literal | `"..."` (4,152 uses) or `'...'` (42 uses); the two are interchangeable |
| Operator/punctuation | see below |

The only escape observed in the corpus is `\n` (54 occurrences, all inside double-quoted
`Translate` arguments). No `\t`, `\\` or `\"` appears, so the escape set beyond `\n` is unattested.

Whitespace is insignificant. Newlines never terminate a statement — `;` does.

### Keywords

`if` `else` `while` `for` `break` `continue` `return` `true` `false`

That is the complete list. There is no `switch`, `case`, `do`, `goto`, `struct`, `function`,
`var`, `const`, `static`, `null` or `include`; every one of those words occurs in the corpus only
inside comments or string literals. Type names (`int`, `Unit`, `Settlement`, …) are **not**
reserved — they are ordinary identifiers recognised positionally (see
[Declarations](#declarations)).

### Operators

Every operator that occurs in the corpus, and only these:

| Category | Operators | Uses |
|---|---|---:|
| Assignment | `=` `+=` `-=` | 5,527 / 498 / 113 |
| Logical | `\|\|` `&&` `!` | 438 / 481 / 1,630 |
| Equality | `==` `!=` | 1,341 / 472 |
| Relational | `<` `>` `<=` `>=` | 938 / 726 / 124 / 264 |
| Additive | `+` `-` | 1,497 / 686 |
| Multiplicative | `*` `/` `%` | 396 / 324 / 74 |
| Postfix | `.` `(` `)` `[` `]` | 12,212 / 17,798 / – / 852 / – |

Notably **absent**: bitwise `& | ^ ~ << >>`, the ternary `?:`, increment/decrement `++ --`, and
the compound assignments `*= /= %=`. Flag sets are combined with `+` instead of `|`
(`g.Eval(AI_COMING + AI_STAYING, ...)` — `DATA\AI\CALCMAXTAKE.VS`), which means the host flag
constants must be disjoint powers of two for the idiom to be safe.

The reference parser accepts `*=`, `/=` and `%=` as a convenience; nothing in the corpus uses them.

### Precedence

Assignment is proven lowest by `bNeedTraining = EnvReadInt(AIPlayer, "NeedTraining")==1;`
(`DATA\AI\ESH_BUILDARMY.VS:46`), which is only meaningful if `==` binds tighter than `=`.
The rest of the table below is the C ordering. It is **consistent with** the corpus but not
proven by it: every place where the corpus nests a looser operator inside a tighter one, it does
so with explicit parentheses (all 16 `&&`-over-`||` nestings, all 32 `*`-over-`+` nestings, and
both `!`-over-binary nestings were verified to be parenthesised in the source). See
[What is still unknown](#what-is-still-unknown).

| Level | Operators | Associativity |
|---:|---|---|
| 1 (loosest) | `=` `+=` `-=` | right |
| 2 | `\|\|` | left |
| 3 | `&&` | left |
| 4 | `==` `!=` | left |
| 5 | `<` `>` `<=` `>=` | left |
| 6 | `+` `-` | left |
| 7 | `*` `/` `%` | left |
| 8 | unary `!` `-` `+` | right |
| 9 (tightest) | `.` `(...)` `[...]` | left |

## Types

There are no user-defined types. Every type name is supplied by the host. The corpus declares
locals of 29 distinct types and the signature comments name 13.

### Value types

| Type | Local decls | Notes |
|---|---:|---|
| `int` | 893 | 32-bit signed, assumed |
| `bool` | 323 | literals `true` / `false`; converts to `int` in arithmetic (see below) |
| `str` | 122 | immutable string; `+` concatenates |
| `point` | 146 | 2-D/3-D map position with `.x`, `.y`, `.z` and mutating methods `Set`, `Rot`, `SetLen`, `IntoRect`, `ClampToMap` |
| `rect` | 10 | `.left`, `.top`, `.right`, `.bottom` |
| `IntArray` | 27 | `.size`; indexed with `[]` |
| `StrArray` | 4 | indexed with `[]` |

`bool` is numerically usable: `tgt.SetExperience(tgt.experience + k + (chance > rand(99)));`
(`DATA\SUBAI\ENCHANTRESS_IDLE.VS:164`) adds a comparison result to an `int`.

`str` concatenation coerces the other operand: `pr("Player " + AIPlayer + " builds new fancy " +
nCount + " " + UType(nType, nRace));` (`DATA\AI\ESH_BUILDARMY.VS`). 463 `+` expressions in the
corpus have a string literal on one side.

`str` compared with `int` compares as strings: `if (EnvReadString(set, "NeedTechSucceed") != 0)`
(`DATA\AI\ESH_BUILDARMY.VS:364,381`, the only two such sites) reads a slot `EnvWriteInt` filled
with `"0"` or `"1"`. Which conversion the retail compiler picks is INFERRED, not read — see
[below](#what-is-still-unknown).

`point` supports arithmetic: `pt = ptBld + (sqLeader.pos - ptBld) * nSight / nDist;`
(`DATA\AI\SQUADMONITOR.VS`) — `point ± point`, `point * int`, `point / int`.

### Handle types

These are references to engine objects. All are nullable/invalid-able and are tested with
`.IsValid` before use.

| Type | Local decls | In signatures |
|---|---:|---:|
| `Obj` | 132 | 506 |
| `Unit` | 254 | – |
| `Building` | 151 | – |
| `ObjList` | 187 | 64 |
| `Settlement` | 72 | 56 |
| `Hero` | 60 | 3 |
| `Query` | 53 | – |
| `Druid` | 41 | – |
| `Wagon` | 28 | – |
| `Squad` | 20 | 3 |
| `SquadList` | 18 | 1 |
| `Catapult` | 16 | – |
| `Ship` | 13 | – |
| `GAIKA` | 11 | 20 |
| `Barrack` | 10 | – |
| `Gate` | 8 | – |
| `Flying` | 6 | – |
| `Sacrifice` | 6 | – |
| `ItemHolder` | 5 | – |
| `Item` | 4 | 7 |
| `Tower` | 4 | – |
| `Teleport` | 2 | – |

`Obj` is the root handle type; the narrower handles are obtained by explicit downcast methods
(`AsUnit`, `AsBuilding`, `AsHero`, `AsShip`, `AsDruid`, `AsCatapult`, `AsWagon`, `AsGate`,
`AsFlying`, `AsSacrifice`, `AsTeleport`, `AsItemHolder`, `AsTower`, `AsBarrack`). A failed cast
yields an invalid handle rather than an error — the universal idiom is
`if (!u.AsHero().IsValid()) ...` (`DATA\SUBAI\...`). These names map onto the 26 `cpp_class`
values in `DATA\CLASSES\*.SC.XML`.

`GAIKA` is the AI's area-of-interest node (a settlement or map region the AI reasons about);
`LAIKA` and `MAIKA` appear as related host functions. `Query` is a live, re-evaluating object set;
`ObjList` is a snapshot.

## Declarations

```
declaration := IDENT declarator ( ',' declarator )* ';'
declarator  := IDENT ( '[' expr ']' )? ( '=' expr )?
```

A statement is a declaration exactly when it begins with two adjacent identifiers, neither of
which is a keyword. This rule needs no table of type names and holds for all 577 files: no
expression form in the language ever places two identifiers side by side.

```
int own, ally, enemy, enemy_hidden;                 // DATA\AI\CALCMAXTAKE.VS
bool bResearchTime = !bNeedTraining && (...);       // DATA\AI\ESH_BUILDARMY.VS
Settlement setGIn, setGDest;                        // DATA\AI\SQUADMONITOR.VS
```

Declarations may appear anywhere a statement may, including inside `if`/`while`/`for` bodies.
Handles and arrays are default-constructed to an invalid/empty state — 331 files declare a handle
and only assign it several statements later, and many declare an `ObjList` and immediately call
`.Clear` or `.Add` on it without initialising.

## Scoping

Blocks introduce scopes. Names may be re-declared in a nested block, and are: `int i;` appears at
top level and again inside nested blocks in dozens of files. There is **no** case in the corpus of
the same name being declared twice in the same block, so whether that is legal is untested.

Identifiers are **case sensitive**. The decisive evidence is `DATA\SUBAI\OUTPOST_IDLE.VS`:

```
// void, Obj This

Settlement this;
...
this = This.AsBuilding().settlement;
...
bProduceGold = This.IsHeirOf("ROutpost") || This.IsHeirOf("EOutpost");
...
pt.SetLen(This.AsBuilding.radius);
```

`This` (the parameter, an outpost building) and `this` (a local `Settlement`) hold different
objects and are both live after line 12. 204 files use this two-variable idiom; other pairs in the
corpus are `Bld`/`bld` (`DATA\SUBAI\WAGON_UNLOAD.VS`), `SL`/`sl`, `Ships`/`ships`, `Dist`/`dist`
and `MaxSetIdx`/`maxSetIdx` (`DATA\SUBAI\HEN_IDLE.VS`, where `maxSetIdx = MaxSetIdx();` assigns a
host function's result to a local that differs only in case).

Host **member** names, in contrast, appear in inconsistent case across the corpus — `GetGAIKA` on
`point` versus `GetGaika` on `Settlement`, both returning `GAIKA` — which suggests the host side
of the lookup is case insensitive. No corpus site *requires* case-insensitive member lookup, so
either policy parses the shipped scripts; case-insensitive member lookup is the safer choice.

## Signature comment

The first non-blank line of a script is a `//` comment that declares the script's return type and
parameters:

```
// <return-type> [ , <param-type> [ '*' | 'OUT' ] <param-name> ]...
```

**576 of 577 files carry one, and every one of the 576 parses.** The single exception,
`DATA\ITEMSCRIPTS\BATTLE DRUMS OF RAGE.VS`, is an orphan: no data file references it, and its
item id does not appear in `DATA\ITEMS.XML`.

Examples:

```
//int, int idPlayer, GAIKA g, int *pOverneed          DATA\AI\CALCMAXTAKE.VS
//bool, ObjList objs, str OUT reasonText              DATA\SUBAI\AUTOTRAIN_START_VERIFY.VS
//void, Hero hero, IntArray* aSkills, IntArray* aSkillLevels   DATA\AI\TSH_HEROSKILLS.VS
//void, Obj This, Obj Bld                             DATA\SUBAI\WAGON_UNLOAD.VS
```

Return type is one of exactly three values:

| Return type | Files |
|---|---:|
| `void` | 487 |
| `bool` | 81 |
| `int` | 8 |

The signature is authoritative and consistent with the bodies: no `void` script returns a value,
and every `bool`/`int` script has at least one value-returning `return`.

Out-parameters are marked either with `*` (attached to the type as in `IntArray* aSkills`, or to
the name as in `int *pOverneed`) or with the word `OUT` between type and name. Writing to an
out-parameter name writes through to the caller — the group-verifier family does
`reasonText = Translate("Not enough gold");` and the engine displays the result as the greyed-out
button's tooltip. 52 scripts declare an out-parameter and 45 of them assign to it; the other seven
leave it at its default.

Parameter types observed across all signatures:

| Type | Params | | Type | Params |
|---|---:|---|---|---:|
| `Obj` | 506 | | `bool` | 36 |
| `int` | 86 | | `GAIKA` | 20 |
| `point` | 68 | | `Item` | 7 |
| `ObjList` | 64 | | `Squad` / `Hero` | 3 each |
| `str` | 61 | | `IntArray` | 2 |
| `Settlement` | 56 | | `SquadList` | 1 |

## The implicit receiver

A leading `.` denotes a member access on the variable named exactly `this`:

```
if (.health < .maxhealth * 8 / 10 + 10)     ==  if (this.health < this.maxhealth * 8 / 10 + 10)
```

3,829 leading-dot expressions appear across 350 files. **In every one of those 350 files a
variable spelled exactly `this` is in scope**, and in every one of the 331 files where `this` is a
local rather than a parameter, the first leading-dot use comes after the assignment to `this`.
There are zero counterexamples.

The dominant idiom is therefore:

```
// void, Obj This

Barrack this;
...
this = This.AsBarrack();
...
.Progress((.cmddelay * perc) / 100);          // DATA\SUBAI\BARRACK_TRAIN.VS
```

The parameter carries the wide `Obj` handle; the script narrows it once into a local `this` and
then uses `.` throughout. Scripts that receive no object at all (the 46 group verifiers, whose
signature is `bool, ObjList objs, str OUT reasonText`) build their own `this` from `objs[0]` and
use `.` the same way — proof that `.` resolves to the variable and is not an engine-supplied
receiver in `.vs` files.

Inline snippets embedded in XML (see [Where scripts come from](#where-scripts-come-from)) *do* use
`.` with no visible declaration, so in those contexts the host pre-binds `this`.

## Statements

```
statement := declaration
           | block
           | 'if' '(' expr ')' statement ( 'else' statement )?
           | 'while' '(' expr ')' statement
           | 'for' '(' expr? ';' expr? ';' expr? ')' statement
           | 'break' ';' | 'continue' ';'
           | 'return' expr? ';'
           | expr ';'
           | ';'
block     := '{' statement* '}' | '[' statement* ']'
```

Statement counts across the corpus: 9,833 expression statements, 6,014 `if`, 3,164 blocks,
2,626 declarations, 917 `return`, 648 `while`, 564 `break`, 409 `continue`, 351 `for`, 109 empty.

### `if` / `else`

`else` binds to the nearest unmatched `if`, as in C. The corpus leans heavily on chained
parenthesis-free `if` guards as a substitute for the missing short-circuit style:

```
if (nSqState == SS_Approach || nSqState == SS_IDLE)
if (setGIn.IsValid)
if (setGIn.IsIndependent)
{ ... }                                        // DATA\AI\SQUADMONITOR.VS
```

Each `if` is the whole body of the previous one; this is ordinary nesting, not a special form.

### `for`

The initialiser slot takes any expression, not only an assignment:

```
for (0; !SL.EOL; SL.Next)                      // DATA\AI\GS_CAPTURE.VS:153
for (i = 0; i < ol.count; i += 1)              // DATA\SUBAI\ARENA_BEHAVIOR.VS
```

All three slots may be empty. No declaration ever appears in the initialiser.

### Blocks, and the bracket anomaly

Blocks are normally `{ ... }` (3,163 occurrences). Exactly **one** block in the whole corpus is
delimited with `[ ... ]`: the body of the squad loop in `DATA\AI\SQUADMONITOR.VS`. Every other
`[` in the corpus (851) is a subscript. Two readings, and the corpus cannot separate them:

1. The engine's grammar genuinely accepts `[` `]` as block delimiters, and this is a stylistic
   one-off.
2. It is a typo that the engine's parser happens to tolerate (for instance by treating `[` as an
   unconditional block opener when it cannot start an expression).

`SQUADMONITOR.VS` is live — `DATA\AI\MAIN.VS` starts it with `AIRun('SquadMonitor.vs');` — so
whichever reading is right, the file must parse. Accepting `[ ]` as a block opener at statement
position, as the reference parser does, is sufficient and harmless.

### `return`

`return;` in a `void` script exits it immediately. `return expr;` in a `bool`/`int` script yields
the value to the host. A `return` at top level of a coroutine-style script terminates the
coroutine.

`DATA\AI\CALCMAXTAKE.VS` documents a value convention in its own comment:

```
return own - nMinNeed; // will be interpreted as zero if negative
```

so at least some `int`-returning AI hooks are clamped host-side.

## Expressions

```
expr    := assign
assign  := binary ( ('='|'+='|'-=') assign )?
unary   := ('!'|'-'|'+') unary | postfix
postfix := primary ( '.' IDENT | '(' args? ')' | '[' expr ']' )*
primary := INT | STRING | 'true' | 'false' | IDENT | '(' expr ')' | ε    /* before '.' */
```

### Calls and the optional parentheses

**Parentheses are optional on a zero-argument call.** This is the single most important syntactic
quirk of the language, and it is pervasive: 562 of the 797 uses of `IsValid` are written
`u.IsValid`, 235 as `u.IsValid()`. The same member is written both ways within single files.
`u.AsUnit` and `u.AsUnit()` both appear 180/225 times.

Consequently a bare member access is indistinguishable from a property read, and the reference
parser records both as `Member` nodes. For an implementer this means **there is no separate
property namespace** — `.health`, `.pos` and `.IsValid` are zero-argument host methods, and a
name/arity table is the whole dispatch mechanism.

The same applies to free functions: `GetTime` (29 bare, 34 with parens), `AIGetPlayer`,
`GetMapRect`, `GAIKACount`, `MapSize`, `MaxSetIdx` and `Breakpoint` all appear both ways.

A bare zero-argument call is also a legal *statement*:

```
SL.Rewind;
SL.Lock;                                       // DATA\AI\GS_CAPTURE.VS
This.Erase;                                    // DATA\SUBAI\UNIT_DISMISS.VS
```

### Assignment targets

Across the whole corpus assignment targets are only ever a plain name (5,608) or a subscript
(197). **A member is never an assignment target.** Object state is mutated exclusively through
`SetXxx` methods (`SetGold`, `SetFood`, `SetStamina`, `SetLevel`, `SetCommand`, …). This is a firm
constraint on the host object model: properties are read-only from script.

### Argument passing

Some host functions and methods write through their arguments — the caller passes a bare local and
reads it back afterwards:

```
g.Eval(AI_COMING + AI_STAYING, idPlayer, own, ally, enemy, enemy_hidden);
nMinNeed = g.MinNeed(idPlayer, own, ally, enemy);          // DATA\AI\CALCMAXTAKE.VS

SquadList SL;
gaika.GetSquads(SL, AI_COMING + AI_STAYING, AIPlayer, AI_OWN);   // DATA\AI\GS_CAPTURE.VS

str carCmdParam, cdrCmdParam;
carCmdParam = ParseStr(cmdparam, cdrCmdParam);             // DATA\SUBAI\BARRACK_TRAIN.VS
```

There is no syntax marking an argument as by-reference: whether an argument is in or out is a
property of the host entry point, and each such entry point must be documented individually.
`ParseStr` is a car/cdr string splitter — it returns the head and writes the tail through its
second argument.

Whether the underlying convention is by-reference for all arguments, or by-reference only for
lvalue arguments of specific host slots, cannot be determined from the corpus.

## Concurrency

Scripts are **coroutines**, not plain functions.

`Sleep(ms)` (652 calls in 236 files) suspends the script and resumes it later. 101 scripts are an
infinite `while (1) { ... Sleep(n); ... }` driver loop — this is the standard shape of a
`<behavior>` script and of the top-level AI monitors. `Sleep(1)` (17 uses) is used to yield inside
long loops:

```
for (i = 0; i < .Units.count; i += 1) {
    ...
    if (i % 50 == 49) Sleep(1);                // DATA\SUBAI\OUTPOST_IDLE.VS
}
```

None of the 81 `bool`-returning scripts calls `Sleep` — verifiers must answer synchronously.

Scripts spawn other scripts and can kill them:

```
int heroScId;
heroScId = AIRun("TS_AttackAtWill.vs", set, ol, oDummy, 0);
...
AIBreakScript(heroScId);                       // DATA\AI\TS_CARTHAGETACTIC.VS
```

`AIRun(name, args...)` is variadic (arities 1, 2 and 5 all occur), starts the named script
concurrently, and returns an `int` script handle. `AIBreakScript(id)` terminates it. There are
receiver-bound forms too — `set.AIRun("ESH_NeedTech.vs")` and `gaika.AIRun("GSH_SynchApproach.vs",
AIPlayer)` — which presumably bind the receiver as the new script's first parameter.
`StartPlayerScript(player, "data/subai/player_heroes_wisdom.vs")` is the player-scoped equivalent.

`DATA\AI\MAIN.VS` in its entirety is the AI bootstrap:

```
// void

AIRun('Prioritize.vs' );
AIRun('Recruiter.vs' );
AIRun('GAIKAMonitor.vs' );
AIRun('SquadMonitor.vs');
AIRun('EconomyMonitor.vs');
AIRun('TacticMonitor.vs');
```

Other blocking host calls exist — `WaitNonEmptyQuery(q, ms)` and `WaitQueryCountBetween(...)` —
so the suspension mechanism is general, not `Sleep`-specific.

## Where scripts come from

Scripts are not called by name from other scripts (except through `AIRun`). They are bound to
engine events by data files.

### Class definitions — `DATA\CLASSES\*.SC.XML`

Fully documented in [`sc-xml.md`](sc-xml.md). 845 files, plain XML, one `<class>` per file, forming a single-inheritance tree over 26 C++ base
classes (`CVXUnit`, `CVXBuilding`, `CVXHero`, `CVXShip`, `CVXWagon`, `CVXDruid`, `CVXGate`,
`CVXCatapult`, `CVXTownHall`, `CVXOutpost`, `CVXTavern`, `CVXBarrack`, `CVXTeleport`,
`CVXSacrifice`, `CVXItemHolder`, `CVXFlyingUnit`, `CVXGhost`, `CVXArea`, `CVXAdvArea`,
`CVXAreaEffect`, `CVXCatapultShot`, `CVXDecor`, `CVXDestLock`, `CVXFeedback`, `CVXMapObj`,
`CVXScriptObj`).

Four attributes bind a script:

| Attribute | Sites | Meaning |
|---|---:|---|
| `<method sig="…" vs="…"/>` | 415 | implementation of command verb *sig* on this class |
| `<method verify="…"/>` | 44 | predicate run before the method; `bool` script |
| `<method onfinish="…"/>` | 10 | run when the command completes; takes `bool bCanceled` |
| `<behavior script="…"/>` | 50 | free-running coroutine started with the object |

Example (`DATA\CLASSES\BASETOWNHALL.SC.XML`):

```xml
<method sig="unitsout" vs="data/subai/townhall_unitsout.vs"/>
<behavior script="data/subai/townhall_behavior_guard.vs"/>
<defaultcmd target=""><cmd name="unitsout"/></defaultcmd>
```

There are **187 distinct command verbs** across the class tree (`idle`, `move`, `attack`,
`enter`, `train`, `research`, `heal`, `teach`, `capture`, `sneak`, `bloodlust`, …). These are
exactly the strings passed to `SetCommand` / `AddCommand` / `SetCmd` from script, which is how
scripts drive one another indirectly.

The implicit receiver for a `<method>` or `<behavior>` script is the object the class describes,
delivered as the first parameter — conventionally spelled `This`, sometimes `me`, `THIS`, `this`,
or a role name (`gate`, `b`, `owner`, `attached`).

### Command definitions — `DATA\COMMANDS\*.XML`

35 files defining UI commands. Each `<cmd>` names a `method` (a verb from the class tree), a
`groupverifier` (350 sites) and sometimes a `groupdispatch` (20) or `onaddremovescript` (12).

`groupverifier` scripts always have the signature `bool, ObjList objs, str OUT reasonText` — this
is the largest single signature family in the corpus (46 files, 46 uses). They receive the current
selection, return whether the command is available, and write the greyed-out tooltip into
`reasonText`.

Commands also expose their cost and parameters to the running script as ambient globals:
`cmdparam` (`str`, the `param=` attribute), `cmdcost_gold`, `cmdcost_food`, `cmdcost_pop`,
`cmdcost_stamina` (`int`), `cmdwaiting` (`str`) and the receiver member `.cmddelay`
(the `execdelay=` attribute).

### Item, bonus and game-mode hooks

`DATA\ITEMS.XML` binds `use_script`, `kill_script`, `equip_script`, `attacheddie_script` and
`object_script` with `file://data/ItemScripts/*.vs` URLs. Bonus scripts and game scripts are
selected by index from the map/lobby settings (`DATA\BONUSSCRIPTS\NNN *.vs`,
`DATA\GAMESCRIPTS\N *.vs`).

### Engine-hardcoded entry points

186 of the 577 scripts are not referenced by any data file. They are reached by `AIRun`, or by
names the executable constructs at run time — `gbr.exe` retains the format strings
`HeroSkill %s.vs`, `HEROSKILL DEFAULT.VS`, `Main.vs`, `GetTacticScript.vs`,
`GetEconomyScript.vs`, `CalcGAIKAPriority.vs`, `unit_form_move.vs`, all of which correspond to
files present in `data.pak`.

Four scripts are referenced by class XML but **absent from `data.pak`**:
`data/subai/outpost_feeding_behavior.vs`, `data/subai/stonehenge_final_sacrifice.vs`,
`data/subai/stonehenge_golden_rain.vs`, `data/subai/stonehenge_wind_of_wisdom.vs`. A
reimplementation must therefore tolerate a missing script binding rather than treating it as a
fatal error.

### Inline scripts inside XML

113 `script="..."` attributes in `DATA\CLASSES\*.SC.XML`, `DATA\ITEMS.XML` and `DATA\SCDEBUG.XML`
contain `.vs` source directly, XML-escaped:

```xml
<value1 script="return .AsBuilding.settlement.population + '/' + .AsBuilding.settlement.max_population;"/>
<key id="Pause" script="Pause();"/>
<key id="Faster" script="int i; for (i=1; i&lt;=5; i+=1)
                         if (GetSpeed()&lt;GetConst('Speed'+i)) { SetSpeed(GetConst('Speed'+i)); break; }"/>
```

(The third is written out here rather than quoted; the shipped one is on one
line. Three things it shows that a one-statement snippet does not: a local
declared inside the attribute, `&lt;` standing in for `<` because the attribute
is XML, and single quotes for a string literal because the double ones are
already spoken for by the attribute.)

112 of the 113 parse with the same grammar. The 113th is `.AsUnit.level` — a bare expression with
no `return` and no `;`, in `DATA\SCDEBUG.XML`. That is a second, **expression-only** dialect used
for debug watches; it is not a counterexample to the statement grammar but a different entry mode.

These inline snippets are also the only place the following host functions appear: `Pause`,
`ProfileStart`, `ProfileStop`, `IsMultiplayer`, `GetSpeed`, `SetSpeed`, `SelAvgLevel`,
`SelAvgStamina`, `SelAvgFood`, `SelHealth`, `SelMaxHealth`.

## Worked example

`DATA\AI\CALCMAXTAKE.VS` in full, annotated:

```
//int, int idPlayer, GAIKA g, int *pOverneed      ← returns int; 3 params, 3rd is an out-param

int own, ally, enemy, enemy_hidden;               ← locals, default 0
int nMinNeed, nMaxNeed;

g.Eval(AI_COMING + AI_STAYING, idPlayer, own, ally, enemy, enemy_hidden);
                                                  ← flags combined with '+'; last four args are
                                                    written through by the host
nMinNeed = g.MinNeed(idPlayer, own, ally, enemy);
nMaxNeed = g.MaxNeed(idPlayer, own, ally, enemy);

pOverneed = own - nMaxNeed;                       ← writes through to the caller

return own - nMinNeed; // will be interpreted as zero if negative
```

## Validation

The reference parser establishes, over all 577 files:

- 577/577 parse with zero failures, and 576/577 signature comments parse.
- Every one of the 350 files using a leading `.` has a variable spelled `this` in scope, assigned
  before first use.
- No `void` script returns a value; every `bool`/`int` script returns one.
- No assignment target is a member access.
- No `bool`-returning script calls `Sleep`.
- No two declarations of the same name occur in the same block.
- Every place where the corpus nests a lower-precedence operator inside a higher-precedence one is
  explicitly parenthesised.

## What is still unknown

- **Operator precedence is consistent with C but not proven.** The corpus parenthesises every
  ambiguous nesting, so a different precedence table would still produce the same result on every
  shipped script. This is safe for running the retail data and unsafe for accepting user scripts.
- **Integer semantics.** Width, signedness and division/modulo rounding for negatives are
  unattested. `/` is used on positive quantities throughout.
- **`str` comparison.** `sqLeader.command == "idle"` compares strings; whether the comparison is
  case sensitive is untested (all comparands are lowercase verbs).
- **`str` against `int`.** The retail compiler is statically typed and refuses an operator it
  cannot resolve (`Infix operator %s can not be applied.`, `gbr.exe` 0x007dcd34), and
  `ESH_BUILDARMY.VS` ships and researches, so it resolves `str != int` through a conversion. The
  reading taken is int→str, the conversion `+` already has (str→int exists only as `Str2Int`); the
  two readings differ only on the first read of an unset key, `"" != 0`. The runtime opcode was not
  found; the compiler's emitter (0x006904a0) is where it would be read.
- **Short-circuit evaluation** of `&&` / `||` is assumed but not proven. Guard-shaped sites such
  as `while (This.IsValid() && This.stamina >= 0)`
  (`DATA\SUBAI\SUMMONING_BEHAVIOR.VS:22`) are weak evidence in favour, but nothing in the corpus
  would visibly misbehave under eager evaluation, because reading a member of an invalid handle
  appears to be tolerated rather than fatal.
- **Argument-passing convention.** Out-arguments exist (`Eval`, `GetSquads`, `ParseStr`) but the
  rule that decides which arguments are written through is a property of each host entry point,
  not of the syntax. Candidate readings: (a) all arguments are by-reference and most host slots
  simply do not write; (b) the host declares in/out per parameter and the VM copies back only for
  out slots.
- **Identifier case sensitivity for host member names.** Variables are demonstrably case
  sensitive; members are written inconsistently (`GetGAIKA` / `GetGaika`) but never in a way that
  forces a decision. Recommend case-insensitive member lookup, case-sensitive locals.
- **The bracket block.** One occurrence; see [above](#blocks-and-the-bracket-anomaly).
- **Method overloading in the class tree.** `DATA\CLASSES\UNIT.SC.XML` binds `sig="attack"` twice,
  to two different scripts, in the same file. Whether the engine takes the first, the last, or
  selects on the `verify` result is undetermined.
- **Uncaught-error behaviour.** What happens on a null-handle method call, a division by zero, or
  a missing script binding is unknown. The four dangling `<behavior script=…>` references in the
  class tree prove that at least a missing script is survivable.
- **Declaration initialiser evaluation order** relative to sibling declarators in the same
  statement is untested; no corpus site depends on it.
- **Recursion / re-entrancy.** A script cannot call itself by name, but `AIRun` can start a second
  copy of a running script. No corpus site does.
