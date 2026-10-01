# Conversations (`.conv.xml`)

**Status:** schema fully enumerated over the retail corpus; two semantics open, both recorded below
**Reference reader:** [`engine/core/src/sim/conversation.cpp`](../../engine/core/src/sim/conversation.cpp)
**Companion:** [`map.md`](map.md) (the container), [`vs-language.md`](vs-language.md) (what the code attributes hold)

A `.conv.xml` is one **conversation**: a small state machine of things characters say, with
conditions on what is eligible, actions that fire when a line plays, and a result string the
mission can branch on afterwards. It is the narration layer, and it is entirely a campaign
concern — every one of the 110 documents is inside a map container and none is in `data.pak`.

The scripts drive it through six entry points: `Conversation::Init/1` (54 call sites),
`Conversation::SetActor/2` (63), `Conversation::Run/0` (54), the one-call `RunConv/1` (74),
`ConvResult/1` (5) and `ConvSetResult/2`.

## File location and identity

    Maps/<n>/Conversations/cnv<k>.conv.xml     109 documents
    Conversations/cnv<k>.conv.xml                1 document, at a container root

**The file name is not the conversation's name.** `cnv1.conv.xml` declares
`<conversation name="C_Conv2">`, and `Init("C_Conv2")` finds it by that attribute. The
numbering of the files carries no meaning and is not contiguous; a loader must enumerate the
directory and index on the `name`. The executable's string table carries
`CurrentGame/Conversations` beside a `CurrentMap/…` spelling, which is the same
container-root-then-map-directory arrangement that [`sequences.xml`](adventure.md) and
`Notes.xml` use.

**A name may contain spaces.** The tutorial's are `1 Welcome`, `2 Navigation`, `A Feed`,
`B Stronghold`. `Init("A Feed")` is an ordinary call; nothing may split on whitespace.

Every one of the 110 documents holds exactly one `<conversation>`, at the root, with no
envelope element and no manifest.

The same extension appears under `Local/<language>/Maps/<n>/conversations/`, and **those are a
different format**: they are `<translationtable>` documents keyed by the conversation name and
the phrase number. A loader that globbed on the extension would read every conversation twice,
the second time under a translation. This is the trap `Notes.xml` has and it has the same answer:
read by path, not by suffix.

## Elements

```xml
<conversation name="…" startup="first" restore_view="0" startup_phrases="…">
  <actor name="…"/>
  <phrase label="…" actor="…" text="…" condition="…" action="…"
          followup="first" followup_phrases="…" choice_text="…" return="…" comments="…"/>
</conversation>
```

### `<conversation>`

| attribute | in 110 | meaning |
|---|---|---|
| `name` | 110 | the key `Init`, `RunConv` and `ConvResult` use |
| `startup` | 110 | how the opening phrase is chosen; `first` in every shipped document |
| `startup_phrases` | 9 | the candidate list for the opening, `;`-separated labels |
| `restore_view` | 110 | put the camera back afterwards; set in 34 |

### `<actor>`

A conversation declares the **roles** it needs, and the script binds each to an object:
`C_Conv.SetActor("Scipio", GetNamedObj("NO_Scipio").obj.AsUnit())`. 134 declarations over the
110 documents. A phrase's `actor` names one of them; 258 of the 260 phrases do, and every one
of those names a role its own conversation declares, so the binding can be checked at load
rather than at play. The two that name nobody are `choice` prompts — the menu itself, rather
than a character.

The role name is a display name as well as a key. It is what the interface shows beside the
line, which is why the same document can use `Scipio` and `NO_Scipio` for the same unit in two
different conversations.

### `<phrase>`

260 of them, in document order, which is the order every default candidate list walks.

| attribute | in 260 | meaning |
|---|---|---|
| `text` | 259 | what is said. A display string; see *Escapes* below |
| `actor` | 258 | which role says it |
| `followup` | 260 | how the next phrase is chosen |
| `label` | 66 | the name a `…_phrases` list uses. Unique within a conversation in all 110 |
| `condition` | 31 | VS source: is this phrase eligible at all? |
| `action` | 31 | VS source: run when the phrase plays |
| `followup_phrases` | 23 | the candidate list for the next phrase |
| `choice_text` | 22 | the menu label, when a `choice` can reach this phrase |
| `return` | 4 | VS source: the string `ConvResult` reads back |
| `comments` | 2 | authoring metadata; not read |

## Modes

`startup` and `followup` hold a mode name. The executable maps it at `0x005067d0`, and the
table of names it accepts is complete:

| name | code | meaning |
|---|---|---|
| `choice` | 0 | ask the player, from the candidates |
| `random` | 1 | one eligible candidate, drawn |
| `first` | 2 | the first eligible candidate |
| `end` | 3 | the conversation is over |
| `cycle` | 4 | the next candidate after the one used last time |
| `cycle then first` | 5 | cycle until exhausted, then stay on the first |
| `cycle then random` | 6 | cycle until exhausted, then draw |

**An empty string means `first`**, which the same function tests for explicitly. Anything else
is an error, and the executable's string table carries an `!error!` beside the seven names.

The shipped data uses **three** of the seven: `first` (221 phrases and all 110 startups), `end`
(28) and `choice` (11). The other four are named here because a reader that did not know them
would fall back on `first` and play the same line for ever, which is a mission that never
advances and no diagnostic anywhere.

## How the candidates are found

A mode chooses *from a list*, and where the list comes from is only half stated by the
attributes. `startup_phrases` and `followup_phrases` give it explicitly — `;`-separated
labels, in order — but they are present 9 times out of 110 and 23 times out of 260, so the
default is what governs almost everything:

* **startup**, with no `startup_phrases`: the candidates are **every phrase, in document
  order**. `startup` is `first` in all 110 documents, so the conversation opens on the first
  phrase whose `condition` holds. That is how a document leads with an early-out branch —
  *"you cannot afford this"* — and falls through to the real opening when the branch does not
  fire.

* **followup**, with no `followup_phrases`: the candidate is **the next phrase in document
  order**, and the conversation ends when there is none.

  This is not an inference from one file. **98 of the 110 documents end on a phrase whose
  `followup` is `first`**, with nothing after it. Under any reading where `first` rescanned the
  whole document, those 98 would jump back to their own opening line and loop for ever.

* **`choice`** with no `followup_phrases`: the candidates are the phrases that **follow this
  one and carry a `choice_text`**. Two documents do this, and both are a prompt followed
  immediately by its own options.

`;`-separated lists are trimmed and empty fields are dropped, so `"begining;"` is a
**one-element** list — the authoring tool writes a separator after every entry, and 4 of the 23
`followup_phrases` attributes are written that way.

## Escapes

`condition`, `action` and `return` carry **VS source inside an XML attribute**, and three of the
characters a program needs cannot appear there. The authoring tool escapes five things:

| written | means | why |
|---|---|---|
| `\l` | `<` | XML would read it as a tag |
| `\g` | `>` | for symmetry with `\l` |
| `\a` | `&` | XML would read it as an entity. `\a\a` is `&&` |
| `\'` | `'` | a VS string literal; `"` would end the attribute |
| `\n` | newline | an attribute is one line |

Those five are the **only** backslash escapes in all 110 documents — 207 `\n`, 204 `\'`, 5 `\g`,
4 `\a`, 2 `\l` — and there is not one XML character reference (`&lt;`, `&#60;`) anywhere in
them. So the escaping is the tool's own convention and not a second layer of XML.

`\'` decodes to a **single** quote, not a double one: the VS lexer accepts either as a string
delimiter, and 3 of the 31 `action` attributes write `'` unescaped and must mean the same thing.

A backslash before anything else keeps its backslash. `movies\Dintro.avi` is a likelier reading
than an escape nobody has heard of, and losing the separator is the worse mistake.

**`text` and `choice_text` are display strings and are not decoded.** They carry `\n` too — 131
of the 207 — and a line break in a display string is the interface layer's business. This is the
same call [`NoteDefinition::text`](../../engine/core/include/imperivm/core/sim/note.hpp) makes.

## `condition` and `return` are each two shapes

Both are written either as a bare expression or as a statement list, and both occur:

    condition="EnvReadInt('/En_NumidiansCharge') == 1"
    condition="if (EnvReadInt('/Ships')==1)\n return true;\nreturn false;"
    return="'YES'"
    return="return 'Trident';"

8 of the 31 conditions are bare and 23 are bodies; 1 of the 4 returns is bare and 3 are bodies.
Nothing but the grammar distinguishes them, so a reader keeps the text and lets whatever
compiles it decide: a snippet containing a `;`, or opening with a keyword that cannot start an
expression, is already a statement list, and anything else needs a `return` wrapped round it.

`action` is always a statement list — it is 29 distinct strings and every one ends a statement —
but nothing in the format says it has to be, and one of them declares a local and runs a `for`
loop with a `return` in it, so it is a script body and not an expression.

## What `ConvResult` reads

`ConvResult(name)` answers the string the conversation called `name` last left behind, and
`ConvSetResult(name, value)` writes one without running anything. The value comes from the
`return` of the phrase the conversation ended on — the four in the corpus are `'Trident'`,
`'Elephant'`, `'Priest'` and `'YES'`, and the callers use them as a group name to spawn and as
a yes/no. A phrase with no `return` leaves the previous value alone rather than clearing it, so
a conversation that ends on an option with nothing to say answers whatever it answered before.

The results are **world state**, unlike the catalogue: a script branches on one, and
`3_Great_Battles_Alesia` map 5 branches on one to decide which map to load next.

## What plays, and what this engine does about the parts nobody can answer

The reader above is the whole of the format. Two of the decisions the *runtime*
has to make are not in the file at all, and both are recorded here because they
are departures rather than readings.

**A conversation finishes inside the call.** `Conversation::Run` and `RunConv`
are registered through `gbr.exe`'s suspending registrar and in the original they
suspend for as long as the conversation is on screen -- the length of a voice
clip, or until the player clicks. The flag `Run` polls (`[conv+0x198]`) is set
by the dialog window closing, not by the simulation. Neither a clip length nor a
click is available to a headless deterministic simulation, and inventing a
duration would be inventing a number that decides when every other script
resumes. So the state machine runs to completion in one slice. The cost is a
turn of ordering -- `C_Conv.Run(); GiveNote("Kill Syphax");` gives the note in
the same slice rather than after the talking -- and the benefit is that a
conversation cannot stall a mission, which matters because 74 `RunConv` sites
are on the critical path of a campaign.

**`choice` takes the first eligible option.** The original asks the player;
there is nobody to ask. Drawing one would spend the world's generator on a
question the player answers, and refusing would stall 11 phrases across 9
documents. `first` is what the same document means by "pick one", it is
deterministic, and it is the option the author wrote at the top. An interface
layer that wants to ask is the seam this leaves open.

Two smaller rules follow from the data rather than from a choice:

* **A condition that cannot be evaluated makes its phrase ineligible**, where an
  **action that cannot be evaluated stops the conversation.** The asymmetry is
  deliberate. A condition only picks a different line; an action is what the
  conversation *does* -- gives the note, explores the area, writes the flag the
  next mission branches on -- and one that silently did not run leaves a player
  with no objective and no diagnostic.

* **A run is bounded by a count.** `followup_phrases` can name a phrase earlier
  in the document -- two shipped options name their own menu -- so the graph has
  cycles and a run is not bounded by the phrase count. The original is bounded
  by the player closing the window. Reaching the bound is reported rather than
  truncated silently.

## What is still unknown

**What `followup_phrases` means on a phrase whose `followup` is `end`.** Two phrases in
`5_Great_Battles_Britain` do this, both of them options in a buy-something menu, both naming
the menu's own prompt. Two readings: the list is a jump target and the player is returned to the
menu after paying, or the mode wins and the attribute is the authoring tool's leftover. The
reader treats `followup` as authoritative, so those two end the conversation. It is observable
in play — whether buying twice needs two conversations — and nothing in the data settles it.

**Where the cycle cursor lives.** `cycle`, `cycle then first` and `cycle then random` have to
remember which candidate was used last, and no shipped document uses any of the three, so there
is no sample of whether the original keeps that per conversation, per phrase, or per phrase per
actor.

**What `restore_view` restores, exactly.** The conversation code saves four words of the current
view before moving the camera to the average of the bound actors' positions. The attribute
plainly governs putting it back; what the four words are is a presentation question this engine
has not needed to answer.

**How long a phrase lasts.** In the original it is the length of the voice clip under
`Local/<language>/Maps/<n>/conversations/cnv<k>_phrase<i>.wav`, or a click, and the
conversation window sets a "finished" flag that `Run` polls. Neither is available to a headless
deterministic simulation, so nothing in `engine/core` models a duration; see
[`conversation.hpp`](../../engine/core/include/imperivm/core/sim/conversation.hpp).
