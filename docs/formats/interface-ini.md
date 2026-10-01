# The interface files (`DATA/INTERFACE/*.ini`)

**Status:** the in-game slice is interpreted end to end; the mechanism is general, the widget
inventory is not
**Reference reader:** [`engine/core/include/imperivm/core/ui/`](../../engine/core/include/imperivm/core/ui/),
drawn by [`engine/platform/src/ui_renderer.cpp`](../../engine/platform/src/ui_renderer.cpp)
**Container:** [ini.md](ini.md) — this document is the layer above it
**Used by:** the info bar, the command bar, the menus, the editor

The entire user interface of Imperivm is declared in data. `gbr.exe` contains a widget
toolkit and no screens: 150 `.ini` files build every dialog, every button and every bar,
and 24 of them — 88 KB — are the in-game info bar and command bar in all eight factions'
skins. Interpreting those 24 reconstructs the real interface rather than an approximation
of it.

This specification covers the mechanism in full and the widget inventory only as far as
those 24 files exercise it. The menus and the editor need more widget *classes*; they do
not need a different interpreter.

| files | size | |
|---:|---|---|
| 52 | 236 KB | editor |
| 48 | 194 KB | menus |
| **24** | **88 KB** | **info bar and command bars — described here** |
| 13 | 33 KB | help, conversations, common dialogs, notifications, templates |

## A screen is three sections

```ini
[Cmdbar]                              ; the dialog
Template = %TmplIni%, StdDlg
RectWH = 0, 0, 1024, 54

[Cmdbar Objects]                      ; its widgets, in drawing order
Background
BackgroundFrame
CmdCancel

[Cmdbar Params]                       ; its %Name% scope
Template=%TmplIni%, Params
TmplIni=gameini/template.ini
Buttons = gameres/CmdBar

[Background]                          ; one section per widget
Type = Button
...
```

`[<Name> Objects]` is a **bare-line section** — no `=` on any line — and the reason the
container reader has to preserve lines that carry no key. It is also the draw order: the
background is first in all 24 files and the dividers and overlays come after the things
they sit on. A name listed there with no matching section is a mod's bug, not a legal
omission; the shipped files have none.

The string `" Objects"` is a literal in `gbr.exe`, immediately beside `Type`, `Style`,
`Template`, `UserData`, `TargetId`, `VAlign`, `HAlign`, `MinSize`, `MaxSize`, `ImageType`
and the rest of the key vocabulary.

## `%Name%` resolves against one scope per screen

**4,329 values in 132 of the 150 files carry `%Name%` placeholders, 179 distinct names.**
They resolve against the screen's `[<Name> Params]` section, and nothing else. Substitution
is textual, recursive and case-insensitive: `MENU/TEMPLATE.INI` writes
`ButtonSpacing2=%ButtonSpacing% + %ButtonSpacing%`, and `CMDBAR/EMPTY_ROME.INI` writes
`%dist%` for a key it spelled `Dist`.

A name with no binding is **left standing, markers and all**. An unbound `%RaceArt%` then
shows up as a path called `%RaceArt%/Back.bmp` that does not resolve, which names itself in
a log; expanding it to nothing would put a widget silently at the origin.

## `%TmplIni%` is not a string substitution — it names a file

That is why it alone accounts for **2,367 of the 4,329 uses**. It is always paired with
`Template`:

```
Template = <ini path>, <section>     ; merge that section of that file
Template = <section>                 ; merge that section of this file
```

`Template` merges the named section's entries into the current one **with the local entries
winning**, and it appears in two places:

* **on a screen or a widget**, pulling in an archetype. `[Infobar_RRome]` declares no
  rectangle of its own; `Template = %TmplIni%, UpperDlg` brings `RectWH = 0, 0, 1024, 80`
  in from `DATA/INTERFACE/TEMPLATE.INI`. `[Cmdbar]` includes `[StdDlg]` and overrides its
  `RectWH` with `0, 0, 1024, 54` while keeping its `MinSize`.
* **inside a `[<Name> Params]` section**, pulling in another file's parameters. This is how
  every screen inherits `Art = gameres` and `InterfaceFont = Fonts/Tahoma14b.apf` from the
  shared template while declaring only what is specific to it.

The recursion order follows from the data. In a Params section the `%TmplIni%` inside the
`Template` line has to resolve *before* the include happens, so the section's own entries
are collected first and the inherited ones are added afterwards. Across all 150 files,
**every `Template` line in a Params section names only placeholders that section defines
itself** — a corpus-wide check, not a spot one — so no other order can work.

The upshot is that a faction skin is three lines of difference:

```ini
[InfoBar_RRome Params]
Template=%TmplIni%, Params
TmplIni=gameini/template.ini
RaceArt=gameres/infobar/RROME       ; the only thing that varies between the eight
```

## Virtual roots

Paths begin with an alias, never a real directory:

| alias | resolves to | pinned by |
|---|---|---|
| `gameres/` | `UI/` | `gbr.exe` names `ui/menu/profileicons/unknown.bmp` and `gameres/cmdbar/` |
| `gameini/` | `DATA/INTERFACE/` | `CONST.INI` writes `data/interface/cmdBar/cmdbar.ini`; `gbr.exe` writes `gameini/cmdbar/cmdbar.ini` |
| `menures/` | `UI/MENU/` | `gbr.exe` names `ui/menu/profileicons/` and `menures/ShieldArrow.bmp` |
| `menuini/` | `DATA/INTERFACE/MENU/` | `gbr.exe` names `menuini/confirm.ini` and `gameini/Menu/RollOverTextDlg.ini` |
| `editorres/` | `UI/EDITOR/` | `gbr.exe` names `EditorRes/curmap.bmp`, and `ui/editor/curmap.bmp` is in `UI.pak` |
| `editorini/` | `DATA/INTERFACE/EDITOR/` | `gbr.exe` names `Data/interface/editor/combo/note_icons.ini` |
| `commonini/` | `DATA/INTERFACE/COMMON/` | `gbr.exe` names `data/interface/common/ScriptEdit.ini` |

Every alias is pinned by the same file being addressed both ways somewhere in the retail
data. Comparison is case-insensitive and `/` and `\` are interchangeable, as everywhere
else in this engine.

## `#expr#` is integer arithmetic over sibling rectangles

```ini
[Diplomacy]
RectWH = #right(Help)+%dist%#, 1, 51, 51
```

Placeholders expand first; anything between a pair of `#` markers is then evaluated as an
integer expression. The operator set is the literal string `+-*/%|&` in `gbr.exe`, beside
the diagnostics `Constant rect not found in section [%s]` and `Id not fount in section
[%s]` (the typo is theirs). Decimal and `0x` literals, parentheses, unary minus, and seven
functions naming an **already-declared** widget:

| function | uses in the corpus |
|---|---:|
| `top` | 1,029 |
| `left` | 886 |
| `right` | 559 |
| `width` | 474 |
| `bottom` | 413 |
| `height` | 196 |
| `id` | 117 |

Widget names may be dotted (`List.VScrollBack`, `0.text3`) because composite controls
create sub-widgets under their own name; those are ordinary section names and need no
special handling. A parameter's *value* may itself be an expression that only gets
evaluated where it is used — `EDITOR/ADVADVENTURE.INI` defines
`FullWidth = #%LeftWidth% + %RightWidth% + 9#` and substitutes it into a `RectWH`.

**There is no precedence.** The evaluator (`gbr.exe` 0x00667e10; the operator loop at
0x006681fc looks each operator up in the literal `+-*/%|&`) reads one operand, then an
operator and the next operand, and applies each operator to the running value **left to
right**; there are no parentheses, and a division by zero gives 0. This used to say
"precedence follows C" on the reasoning that no shipped expression could tell the two
conventions apart. One can: `EDITOR/ADVOBJPROPS.INI` places its tab buttons at
`#%TOR_GEN% - 1 * 92 + left(TabsFrame)#`, which is `(TOR - 1) * 92 + left` read flat -- a
row of tabs 92 apart -- and `TOR - 92 + left`, every tab off the left edge, under C.
An expression that fails to evaluate is left verbatim, markers included, rather than
becoming a plausible zero.

## `HAlign` and `VAlign` are three spring weights

```ini
HAlign = a, b, c
```

The difference between the screen's design extent and its actual extent is divided in the
ratio `a : b : c` between the space **before** the widget, the widget **itself**, and the
space **after** it. So:

| weights | effect | example |
|---|---|---|
| `0, 0, 1` | pinned to the left/top | the bars' `Background` |
| `1, 0, 0` | pinned to the right/bottom | the info bar's end cap at `x = 991` |
| `0, 1, 0` | stretched to fill | a list inside a resizable dialog |
| `1, 0, 1` | centred | the info bar's tab switches |
| `0, 2, 8` | grows by two tenths of the slack | `COMMON/SCRIPTEDIT.INI`'s source list |

**They are ratios, not flags.** `EDITOR/ADVADVENTURE.INI` gives the five column headers of
its territory table the weights `0,1,4`, `1,1,3`, `2,1,2`, `3,1,1`, `4,1,0` — five triples
summing to five. Under the ratio rule each column widens by exactly a fifth of the slack
and the ten-pixel gaps between them stay ten pixels at every width. Under any reading that
treats a non-zero weight as a flag, the middle three collapse to the same thing and the
gaps open and close. `COMMON/SCRIPTEDIT.INI` makes the weaker version of the same point at
both ends of a 320-wide dialog.

The observed weights are `0`, `1`, `2`, `3`, `4` and `8`; two files compute one from an
expression (`VAlign = #%gen_playervaligntop%#,0,#1-%gen_playervaligntop%#`).

Both edges of a widget are computed from the screen origin, `slack * weight / total`, and
never one from the other, so widgets that share an edge in the design keep sharing it after
rounding. Accumulating a rounded width per widget instead opens the gaps a pixel at a time.

A widget with no alignment key does not move: the default `0, 0, 1` is the identity.

## Rectangles

`RectWH = left, top, width, height` and `Rect = left, top, right, bottom` are **different
keys with different meanings**, and both appear in the same file:
`INFOBAR_RROME.INI`'s `[Queue]` writes `Rect = 550, 2, 984, 77` where `[UnitDivider]` writes
`RectWH = 600, 1, 22, 78`.

The rectangle is a **clip and a hit box, not a scale**. Images are drawn at the rectangle's
origin at their own size and clipped to it. That is what lets every skin's `Background` be a
2048-pixel-wide bitmap sitting in a rectangle the width of the display, and it is why
`CMDBARRROME.INI`'s `[BackgroundFrame]` can carry `RectWH = 889, 0, 1024, 54` — an author
who typed a right edge into a width field — without anything looking wrong.

## Bitmaps, strips and the colour key

```ini
Image = %RaceArt%/small frame.bmp, 10, 10
```

The two numbers after the path are **the coordinates of a pixel whose colour is the
transparent one**. Probed across the info bar art they return pure green every time —
`0x03E0` in the 16-bit files, `(0, 255, 0)` in the 24-bit ones — and in each of those
images green is the single most common colour. A negative coordinate (`-1, -1`) means no
key. The same shape is used by `Frame`, `BackImage`, `GradientImage`, `IconFrame` and eight
more keys.

All 4,045 bitmaps in `UI.pak` are uncompressed `BITMAPINFOHEADER` images: 3,626 at 16 bits
per pixel, 415 at 24, four at 8. **16 bits per pixel is X1R5G5B5**, which is what
`BI_RGB` means on Windows and what the colour key confirms — `0x03E0` is full green under
555 and half green under 565.

`ImageType` is a five-letter code, one letter per control state, selecting a frame of a
**horizontal strip**:

| state | index |
|---|---:|
| normal | 0 |
| highlighted | 1 |
| pressed | 2 |
| disabled | 3 |
| selected | 4 |

`ABCCC` therefore means frame 0 at rest, frame 1 under the pointer and frame 2 for
everything else; `AAAAA` means a single-frame bitmap. The frame count is the highest letter
plus one, and **the bitmap's width is an exact multiple of it in 139 of 139 multi-frame
sections across the whole corpus**; it equals `frame count * rectangle width` in 90 of
them, which is the authoring convention rather than the rule. A vertical-strip reading
scores 117 of 139 on the same test, which is the control this was checked against.

`Rows` divides the same bitmap the other way: a `Switch`'s two states are two rows of one
55 x 156 bitmap.

A `Switch` flips the info bar to its `SwitchToTab` and back after `SwitchBackTime`
milliseconds when it declares one (`Switch2`, 20000; `Switch1`, -1). The one with a
`BlinkTime` (`Switch2`, 500) **blinks while the selection's head is a hero the local player
controls with skill points to spend**: 0x006c0f30 reads the selection's head, tests the hero
bit of its flags, calls `AvailableSkillPoints` (0x0052da80, the level's allowance less the
points spent) and a per-player bit of the object, and turns the blink on; its timer message
(0x006c1019) then draws `1 - row` and re-arms itself every `BlinkTime` milliseconds while
the blink is on. That the event which re-evaluates it is the selection changing is the
reading; the engine re-evaluates on every bar refresh, which includes it.

## Widget classes

`gbr.exe` registers the toolkit's own classes — `Dialog`, `Control`, `Button`,
`ImageButton`, `Frame`, `Background`, `DarkFrame`, `List`, `Combobox`, `EditW`, `TextW`,
`Spin`, `SpinButton` — and the game's, in one adjacent block:

`ConquestMap`, `Switch`, `VXMenuBack`, `Combiner`, `BmpScroll`, `BrushSize`, `Thumbnail`,
`InfobarText`, `InfobarIcon`, `VXButton`, `UIHolder`, `BuildingQueue`, `UIInventory`,
`SelectionHealthText`, `SelectionHealth`, `SelectionName`, `TextEx`, `Edit`,
`ActiveButton`, `PlayerCombobox` — plus `Icon`, `HeroSkills`, `UnitSpecials` and
`QueueVisualization`, which appear only in that subsystem's `'X' key not found while
loading a 'Y' control!` diagnostics.

The 24 in-game files instantiate exactly fourteen of them:

| class | uses | what it draws |
|---|---:|---|
| `Button` | 103 | the bar backgrounds, the end caps, the nine no-selection buttons |
| `Icon` | 48 | dividers, thumbnail frames |
| `InfobarText` | 48 | one of six stat slots, as text |
| `InfobarIcon` | 48 | the same slot's icon, in a frame |
| `UIInventory` | 16 | carried items |
| `Switch` | 16 | the tab buttons in the middle of the info bar |
| `Thumbnail` | 8 | the selection's portrait |
| `SelectionName` | 8 | its name |
| `SelectionHealth` | 8 | its health bar |
| `BuildingQueue` | 8 | the training queue |
| `UIHolder` | 8 | garrisoned units |
| `Combiner` | 8 | draws `Id1` and `Id2` as one strip |
| `HeroSkills` | 8 | a hero's skill icons |
| `UnitSpecials` | 8 | a unit's special abilities |

## Style flags

`gbr.exe` knows 52 style names and rejects anything else with `Unknown style: %s`. They are
**per-class tables**, and the same name is a different bit in different classes; the eight
tables, read out of the executable's control registry (each entry is `{name, factory,
reader, style table}`), are:

| class | names and bits |
|---|---|
| every control (`0x00829e58`) | `TRANSPARENT 1`, `HIDDEN 2`, `SETCURSOR 4`, `FORCECURSOR 8`, `COLORUPDATE 0x10`, `COLORAWARE 0x20`, `INACTIVE 0x100`, `DISABLED 0x200`, `TABSTOP 0x400` |
| `Dialog` | the above, `MODAL 0x400` |
| `Button` | the above, `AUTOREPEAT 0x1000000`, `TOGGLE 0x2000000`, `MOVETOHEAD 0x4000000`, `DBLCLICK 0x8000000`, `TRISTATE 0x10000000`, `AUTOTEXTOFFSET 0x20000000` |
| `Scroll` | the above, `AUTOMOVE 0x10000`, `AUTOREPEAT 0x20000`, `AUTODISABLE 0x40000`, `VSCROLL 0x80000`, `AUTOHIDE 0x100000`, `AUTOBUTTONSDISABLE 0x200000`, `HSCROLL 0` |
| `List` | the above, `SINGLE 0x10000`, `ROWS 0x20000`, `MULTISEL 0x40000`, `FIXED 0x80000`, `TIGHTSCROLL 0x280000`, `PARTIALITEM 0x400000`, `NOFOCUS 0x800000`, `ICONONLY 0x1000000`, `TEXTONLY 0x2000000`, `AUTOCALC 0x4000000`, `SELIMAGE 0x8000000`, `ICONSONTOP 0x10000000` |
| `TextW`, `EditW` | the above, `SECURE 0x10000`, `TIGHTHSCROLL 0x20000`, `TIGHTVSCROLL 0x40000`, `SELECTION 0x80000`, `CURSOR 0x100000`, `CURSORVISIBLE 0x200000`, `EDIT 0x580000`, `MULTILINE 0x800000`, `ALIGN_LEFT 0x1000000`, `ALIGN_RIGHT 0x2000000`, `ALIGN_CENTER 0x3000000`, `NUMBER 0x4000000`, `AUTOWIDTH 0x8000000`, `UNDO 0x20000000`, `ALLOWTAB 0x40000000`, `NOWORDWRAP 0x80000000` |
| `Combobox` | the above, `EDIT 0x400000`, `NOLIST 0x800000`, `NOFOCUS 0x1000000`, `AUTOSIZE 0x2000000` |
| `Window` | the first six only |

So `TABSTOP` on a control and `MODAL` on a dialog are the same bit, `HSCROLL` is the
scrollbar's default rather than a flag, `TIGHTSCROLL` on a list is two bits at once, and the
alignment is a two-bit field (`ALIGN_CENTER` = `LEFT | RIGHT`). The reader keeps one enum of
its own with values that mean one thing everywhere, and knows every name above; the files'
bits never leave the executable.

**A local `Style` and the template's are unioned, the local alignment winning.** *Reading,
labelled.* `SELECTMAP.INI`'s `DescriptionText` writes `Style = ALIGN_LEFT` over
`StaticTextMultiline`'s `TRANSPARENT, MULTILINE, ALIGN_CENTER`; a style that replaced the
template's would be a single unwrapped line, one that only OR'd it would stay centred, and
the author asked for neither. Every other key is local-wins, including `Rect` against
`RectWH`, which are two spellings of one key (`ADVENTUREMENU.INI`'s `List` writes `RectWH`
over a template's `Rect`).

The 24 in-game files use four styles: `TRANSPARENT` (183), `INACTIVE` (104), `HIDDEN` (8)
and `ALIGN_LEFT` (8). The menus and the editor use most of the rest, and misspell
`TRANSPRENT` four times.

## The menu widgets

The 48 menu files and the 52 editor files instantiate twenty-one classes the bars do not.
What each one draws, and what was read to draw it:

| class | uses | what it is |
|---|---:|---|
| `TextW` | 713 | static text. `Font`, `TextColor`; `ALIGN_*`; `MULTILINE` wraps at `Width` (the rectangle's width without one) and sets from the top. A single line is vertically centred in its rectangle — *reading*: a 23-tall `StaticText` over a 14-pixel font. The two characters `\n` in a value are a line break (`GAMEOPTIONS.INI`: `Turn off object animations\n(saves CPU time)`). |
| `Button` (menus) | 568 | as the bars', plus a text label centred in `Font`/`FontColor` |
| `ImageButton` | 398 | a button whose bitmap is an `XFrames` × `YFrames` grid — `BUTTON.BMP` is 948 × 76 with `4, 2`, so 237 × 38 cells — the column by `ImageType` for the state, the row by `InitialRow` (or the code: a checked `RadioBtn` is row 1). The cell is **centred** in the rectangle (*reading*: `ImgButton200` is a 237 × 38 cell in a 250 × 40 rectangle, and the rectangles are themselves centred in their dialog). Label centred; `DisabledFontColor` when disabled. |
| `Background` | 298 | `BkColor` filled, then a two-pixel bevel of `FrameColor1..4`. *Reading*: 1 and 2 on the outer top-left and bottom-right, 3 and 4 on the inner; the data does not say. |
| `Frame` | 186 | a nine-slice of `Image` cut at **`Dividers = left, middle, top, middle`** — read from `0x00663570`: the first pair is the width of the left piece and of the middle one, the right piece being the rest of the bitmap, and the second pair the same for the rows; a middle that would pass the bitmap's edge is clamped there. `FRAME.BMP` is 241 wide with `73, 95` — 73 on the right too — and `LINE_FRAME.BMP` is 39 tall with `19, 1`. Corners as they are, edges and the middle **tiled** (*reading*: every shipped frame's bands are constant across their run and its middle is the colour key, so tiled and stretched are indistinguishable on the retail art; a 16-bit software rasteriser tiles). |
| `EditW` | 155 | editable text, `Bufsize` characters at most (`28` on a save's name), `NUMBER` digits only, `SECURE` as asterisks, a one-pixel caret in the text colour while focused |
| `Combobox`, `PlayerCombobox` | 125 | a closed box — `BkColor`, a `FrameColor1` line, the current text, `ButtonImage` at the right — the height of its arrow plus two (*reading*; the rectangle the file gives is the dropped-down one); open, its rows below in the same colours, drawn over everything declared after it and taking the click first; `AUTOSIZE` drops as tall as its items |
| `Scroll` | 66 | a scrollbar's `Thumb`, at the fraction of its run its `TargetId` list has scrolled; `BackID` and `ForwardID` name the arrow buttons that move the list. With no `TargetId` -- the options' `HScroll`s -- a slider: a press or a drag sets a value 0..100 along the run |
| `Control` | 65 | nothing: `Move` (the drag bar) and the size grips |
| `List` | 46 | rows of text in `Font` with `FontColor`, an item wrapped at the list's width unless `NOWORDWRAP` and as many rows tall as its lines; an item's icon at `IconOffs`, its text at `TextOffs`, or -- with no offsets declared, the help's list -- beside its own icon; a *large* item in `BoldFont`, a *centred* one centred (the help's headings); the selected row filled with `SelectedBkColor` in `SelectedFontColor`. `NOTES.INI`'s list is read from its own comments: `SelectedFontColor` is the *title colour* of every item's first line, `FocusFontColor` "defines the color of the selected items", nothing is filled, and an item is as many lines tall as its text has; icons at `IconOffs`, text at `TextOffs`. |
| `VXMenuBack` | 31 | a `Frame` grown by `topOffs`/`leftOffs`/`bottomOffs`/`rightOffs`, its `Text` centred at `TextYPos` between `LeftImage` and `RightImage`, `BulletTextDist` from the text and `BulletYPos` from its top |
| `DarkFrame` | 28 | **halves every pixel under it**: `0x00663c70` shifts each pixel right by one and masks the carry (`>> 1 & 0x7f7f7f`, or the 16-bit equivalent). Black at half cover, once the layer is blended over the world. |
| `Spin` | 16 | the up/down pair beside a number edit: `Buttons` drawn whole, `UpButtonPressed`/`DownButtonPressed` in its place while that half is held. A press on the top half counts the `TargetId` edit up by one, on the bottom half down by one; the change is reported as the edit's. *Reading*: the split at half the height and the step of one are this engine's; the exe's handler was not read. |
| `ActiveButton` | 3 | a `Button` |
| `TextEx` | 2 | drawn as `TextW` |
| `ConquestMap` | 2 | the campaign map: a picture the code composes (`global.bmp` tinted by territory state, scaled to the widget), a click reporting the point; see [adventure.md](adventure.md) |
| `BrushSize` | 6 | the editor's brush picker: the columns of `Image` that the digits of `Frames` name (`23456` for the terrain and height tools, `13456` for the decor tools), `ItemWidth` × `ItemHeight` each, side by side; the chosen column from the strip's second row. `BRUSHES.BMP` is 252 × 84 at 42: six columns -- a single stamp, then five dots -- by two rows. A click chooses the column under it and reports its frame number as the widget's value. *Reading*: the second row as the chosen look, read off the art. |
| `BmpScroll` | 1 | the editor's; not drawn |
| `Dialog` | — | a screen's own section, through `StdDlg` |

Three more things the menu files carry that the bars do not:

* **`Esc = <widget>`, `Enter = <widget>`, `Focus = <widget>`** on the dialog section: the
  buttons those keys press, and which widget has the keyboard when the dialog opens. 55
  files declare one; two name a widget the file does not have (`SETTINGS.INI`'s
  `Esc = Cancel`, `LOADGAME.INI`'s `Enter = SaveBtn`, copied from `SAVEGAME.INI`) and press
  nothing.
* **A `LeftBullet`/`RightBullet` with no rectangle** (`Id = %ID_LBULLET%`, `0x75ee`/`0x75ef`)
  stands beside the screen's caption (`Id = %ID_CAPTION%`, `0x1007f`), `BulletTextDist`
  (7, `MenuFrame`'s) from its text. *Reading*: nothing in the file places them, and the
  original's caption has its bullets either side.
* **Forward references.** `SAVEGAME.INI`'s `ChatFrame` is placed from `NameLabel`, listed
  two lines below it; the editor's `AdvObjProps` places its children from
  `width(AdvObjProps)`, the dialog itself; `MPGAMEMENU.INI` writes `#PlayerNameWidth#`, a
  bare parameter inside the expression marks. All three resolve — the referenced section on
  demand, memoised, to a bounded depth.

**Where a dialog stands.** *Reading, labelled:* centred in the display, as `0x006b3e00`
centres a child in its parent (`(parent + 1 - child + 1) / 2` on each axis) — which is how
the no-selection menu sits in the command bar, and how `NOTES.INI`'s 512 × 512 screen
authored at `0, 0` comes to the middle. The `RectWH` origin of a smaller dialog
(`SAVEGAME.INI`'s `100, 100`) is not where it is shown. Every shipped menu is authored for
1024 × 768 (`StdDlg` caps at `MaxSize = 1024, 768`); on a larger display the full-screen
ones are centred with the display's black around them.

**A screen with no widgets** -- `COMMON/ZOOMMAP.INI` declares an empty object list and a
`MinSize` -- is a frame the code fills: the zoom map is the dialog's backdrop, and a click on
it with no widget under the point is reported as one.

**The whole census** is `imcheck interface <Packs> [local pack]`: 136 screens over 137
files, 2,954 widgets, every one loads; six widgets in the Online Battle files inherit a
`MenuTitle` template no file defines and so have no `Type`; six bitmaps are named that no
pack holds, one of them `asdf`. `tests/test_corpus_interface.py` holds the numbers.

## Visibility: bare lines are selection tags

A widget's section may carry bare lines, and they are not stray keys:

```ini
[UnitDivider]
...
unit
!holder
```

`gbr.exe` contains the eight tag words with the negation already attached — `!thumb`,
`!empty`, `!building`, `!unit`, `!hero`, `!holder`, `!items`, `!queue` — which is what
identifies them.

The rule the 24 files require is that **positive tags are alternatives and negative tags
are vetoes**: a widget is drawn when the selection carries at least one of its positive tags
(or it has none) and none of its negated ones. `INFOBAR_RROME.INI`'s `[MultiDivider]`
carries `unit` and `holder`, and a conjunctive reading would make it unreachable, since
nothing is both. `[UnitDivider]` carries `unit` and `!holder` in the same file.

Two more gates sit alongside:

* `TabMask` is a bit per tab index; `0` or absent means every tab. The info bar's two
  `Switch` widgets are each visible on the tab the other switches to
  (`TabMask = 1, SwitchToTab = 1` and `TabMask = 2, SwitchToTab = 0`), which is what makes
  the pair a toggle.
* `ShowAll`, `ShowControl`, `ShowVision`, `ShowCover`, `ShowSupport` and `ShowCeaseFire`
  gate on the viewer's relationship to the selected object's owner. The files say which
  relationships a widget is for; the engine decides which one holds.

## Text is translated

`HelpText`, `Rollover`, `Text` and named `Value`s are **keys into
`CURRENTLANG\TRANSLATION.LOC.XML`**, not literals. All sixteen of the distinct display
strings the 24 in-game files carry — `Help (F1)`, `Diplomacy (F5)`, `Cancel Command`,
`Hero skills`, `Army and items`, `Population`, `Gold`, `Food` and the rest — are present in
the shipped table. See [`localization.hpp`](../../engine/core/include/imperivm/core/game/localization.hpp).

**The key is contextual.** A widget's text is keyed by where it stands:
`Cancel@/Menu/selectmap.ini:CancelBtn:Text` — the text, `@`, the file under
`DATA/INTERFACE/`, the section, the key — and the file part is spelled the way whichever
tool wrote the entry did (`/Menu/AdventureMenu.ini` beside `/Menu/gamemenu.ini`), so it is
matched case-blind. A miss falls back to the bare text, and that to itself. The table is
UTF-8 and the fonts index cp1252; a result is converted once, at parse.

The language pack is `local/<language>.pak`, named by `Settings.ini`'s `[Language]
Default=`; it holds `CurrentLang/` — the two menu backgrounds, the tips, and the table —
and is mounted at the root.

### The skirmish setup

`SETTINGS.INI` beside `MPGAMEMENU.INI` carries six rules, all read out of `gbr.exe` (the
settings record 0x006d9780, the collector 0x006f4d40, the restore 0x006f5630, the bridge
0x006dbc30 into the game object, the consumers named below):

| Widget | Items | Value → effect |
|---|---|---|
| `GameTypeCombo` 0x1002 | `data/GameScripts/*.vs` by file (0x006c4750), the text after the leading number, translated; nothing selected is `Map Default` | the victory script's basename → `MatchRules::condition`; `Map Default` keeps the map's `game.xml` rule. **That the choice replaces the map's rule is inferred**: the hop from the game object's field to the loaded rule was not located |
| `LimitCombo` 0x1003 | `CONST.INI [GamePlay]` `<basename with _><i> = number, label` until a key is missing (0x006c31a0); the value is translated whole then split at the comma; Elimination has none | the number → the script's `str param` (`Str2Int`) |
| `WorldPopCombo` 0x1004 | `Low/Normal/High` (`@population`), percents `LowPop/NormalPop/HighPop` | every non-wildlife settlement's `max_population = max_population * percent / 100` (0x005267dd; `population` untouched) |
| `StartingGoldCombo` 0x1005 | `2500/5000/10000/Default` (`@gold`) | not Default: `SetGold` on every settlement with a warehouse, the independents' too (0x005267b3) |
| `NoFogCB` 0x1007 | check | fog off: a cell is visible when explored (0x0041cbd0) — a renderer rule, kept on the match |
| `NoExplorationCB` 0x1008 | check | exploration off: the whole map revealed at load (0x00526813), every explored query yes (0x0041cb80) |
| `SharedSupportCB` 0x1009 / `SharedControlCB` 0x100a | check | **inferred**: teammates' relation word gains bits 2 / 5 (`kRelationBits`); the record's fields are persisted and no reader was found |
| `NoBonuses` 0x10a7 | check; shown only in a network lobby (0x6f859f) | the bonus scripts are not started; the consumer was not found |
| `MapTypeCombo` 0x1000, `RM.MapSize` | scenario vs random map | out of scope: no random-map generator |

The defaults are the automatch path's (0x00702906): `1 Elimination`, `NormalPop`, gold -1, fog
and exploration on. The choice persists as the profile's `[Player]` keys
([adventure.md](adventure.md)).

### The networked front

The main menu's *Multiplayer* (0x1003) opens `MPMENU.INI`, and three screens make up a
networked game. What they send is this engine's -- `sim/netlobby.hpp` -- because the
original's was DirectPlay's and the wire is not reproduced; what they *show* is the files'.

| Screen | Widgets used | What they do here |
|---|---|---|
| `MPMENU.INI` | `List` 0x1006, `InfoText` 0x1007, `InetHostCombo` 0x2000 (edit), `Lan` 0x1008 / `Refresh` 0x1004, `Join` 0x1002, `Host` 0x1003, `Cancel` 0x1005 | the LAN's open games, found by a broadcast query; the chosen one described; a typed address wins over the list. `GameSpy` 0x1001 is disabled, as the main menu's online buttons are -- the service is gone -- and `List2` with its frame is hidden |
| `MPGAMEMENU.INI` | the skirmish setup's rows, plus `PlayerReady_Pn`, `HostIPText`/`HostIPName` 0x10000, `Clip` 0x11000, `ImReadyBig` 0x9008, `Start` 0x9006, `Cancel` 0x9007 | the host's lobby and every joiner's view of it: `PlayerType`'s five `MMENBUT.BMP` rows are all used -- monitor this machine, face another peer, blank an open seat, hand the AI, cross closed. The host sees its address (`HostIPName` is sized for an address, so the port shows only when not the default) and copies it with the clip; a joiner edits only its own row and presses *I'm ready*; the host's *Start* waits for every ready mark |
| `MULTI.INI` | `Message` 0x29A, `CancelBtn` 0x1001 | "connecting", every refusal, a player leaving a match -- who is dropped from an agreed turn while the match goes on, the computer taking the seat (`sim/netdepart.hpp`) -- and the host leaving, which ends it. `QuitBtn` 0x1111 is hidden. The file's own comment calls its name inappropriate |
| `MPCHAT.INI` | `ChatLog` 0x9002, `ChatEdit` 0x9003, the `List.*` scrollbar | the lobby's chat, beside the players' screen at its own `RectWH = 0, 424`: the log every peer shares, and Enter in the edit says the line (neither chat file binds Enter; that is this engine's). A `StaticText` naming a `VScrollId` is drawn as a log -- from the top, wrapped at its `Width` -- which is a reading, labelled |
| `INGAMECHAT.INI` | `ChatEdit` 0x1004, `AllRadio` 0x20101, `FriendsRadio` 0x20102, `FoesRadio` 0x20103, `PlayerCombo` 0x2009, `SendBtn` 0x1001, `SendLocBtn` 0x1007, `CancelBtn` 0x1002 | Enter in a networked match -- the command bar's `Chat (Enter)` -- opens it: to all, to allies, or to the player the combo names; *Send location* adds the point at the middle of the view. A line heard is shown when it is for the viewer (`sim/netchat.hpp`'s `shown_to`), for twelve seconds in the top-left corner; where the original shows them is not modelled, and that place is this engine's |

What they send is `sim/netchat.hpp`'s, and none of it is an input: a line never reaches the
command stream, so it is in no hash. `ENDMULTIPLAYER.INI` and `MPGAMEMENU.INI`'s namesake
in-game menu are not wired: a networked match ends through the ordinary end screen. The `OB_*` family is the online service's and stays unreachable.

### The Great Battles

The main menu's *Great Battles* opens `PREADVENTUREMENU.INI` (0x006e944a), not
`ADVENTUREMENU.INI`: two lists under two headings -- `ListBattles`, *The Great Victories of
Rome*, over `adventures/GreatBattles/`, and `ListLoses`, *Rome's Enemies Fight for Freedom*,
over `adventures/GreatChallenges/` -- the chosen adventure's picture (the `.bmp` beside its
container) in `DescriptionBmp`, its `game.xml` name and description beside, the difficulty
combo (`AdvDifficultyType`), Start (0x1005) and Cancel (0x1006). The same screen serves the
exe's other fronts with other widgets shown -- `ListAll` and `AllAdv`, `Conquests`, `Custom`
(0x1007), `StartConquest` (0x1009), `GameAdv` (0x1008) -- which its constructor's long run of
show-and-hide calls (0x006e95f7 on) selects by mode; this engine builds the Great Battles
mode. Until it did, the six Great Challenges were reachable from no screen.

### The main menu's Tips, and the Credits

`MAINMENU.INI`'s `TipsBackFrame` shows one of `CURRENTLANG\TIPS.XML`'s tips -- `<tips>` of
`<tip link="/contents/shortcuts"><text>...</text></tip>`, the second shipped document whose
element text matters (`core/game/help.hpp`, `parse_tips`). The rule (0x006d8050): while not
every tip has been shown, the next in order, `LastTip + 1`, and past the end `AllShown` is
set; once all have been shown, one drawn at random, a draw repeating the tip standing moving
on by one. Both are kept in the game's `settings.ini` under `[Tips]` (`LastTip`, `AllShown`),
read when the menu opens and written when it closes. *Next* (0x1054) shows the next; *More
Info* (0x1055) opens the help at the tip's `link`.

`CREDITSMENU.INI` names its own source in its `[CreditsDescription]` section: `Text` is the
file (`CurrentLang/credits.txt`), `UniCode` whether it is UTF-8 (`0`: single-byte), `PixPerSec`
the roll's speed (`100`); the file's comments describe `[IMG_<name>]` sections that put an
image where the text says `<name>`, which the shipped file does not use. The `Credits` block
(`StaticTextMultiline`, `Bufsize = 100000`) rolls up from below the frame at that speed.
**Reading, labelled:** what happens when the last line has passed -- this engine returns to
the main menu.

### The Statistics screen

`STATISTICS.INI` is the end-of-match report `ENDGAMEMENU.INI`'s *Statistics* button (0x1005)
opens, built at 0x006fc800. Eight rows `pl0..pl7`, each a colour square, a name, three values
(`text1..3`, ids `0x400 + row * 0x10 + 2..4`) and three shares (`per1..3`, `+5..7`); a row per
player the game's table still lists, the rest hidden. Four buttons (ids 4..7) each run one
fill, which writes the title (`%ID_CAPTION%`), the column headings (ids 1..3) and, per row,
the value as `%d` and its share `(%d%%)` of the column's maximum (0x006faaa0; a column whose
maximum is nought shares against a hundred, 0x006fb2f1; the maximum's own row is set in
`0xffff00`, the rest `0xffffff`, from a two-entry table at 0x0082f798). The counters are the
player's statistics record, whose fields both of `gbr.exe`'s serialisers name (see
`sim/match.hpp`, `PlayerScoreCounters`):

| Tab | Fill | Columns | Record fields |
|---|---|---|---|
| *Resources* (4) | 0x006fc260 | `Gold spent`, `Food spent`; the third column hidden | `gold_used` +0x38, `food_used` +0x3c |
| *Gold production* (5) | 0x006fae20 | `From taxes`, `Other`, `Captured` | `gold_townhall` +0xb0, `gold_outpost` +0xb4, `gold_captured` +0xac |
| *Units* (6) | 0x006fbbb0 | `Killed`, `Lost`, `Maximum` | `units_killed` +0x44, `units_lost` +0x48, `units_max` +0x4c |
| *Scores* (7) | 0x006fb4e0 | `Military`, `Development`, `Overall` | the military rating (0x0056a260); `gold_used / 500` (0x0056a2a0); that plus `power_score * rating / 100` (0x0056a970) |

`0.time` (id 9) is `Game time: %s1:%s2:%s3` of the game clock in seconds, two digits each
(0x006fabd0); *Close* is id 8. **Reading, labelled:** which players are rows -- the table
0x006fc8b0 walks (stride `0xc0`, word `+0xb0` non-zero) is not the match's, and this engine
reads it as *participates*.

### Inline markup

Some strings carry inline markup, and there are exactly two tags: `<color r g b>` and
`<imagetransp path>`. `gbr.exe` composes with no others — its own tag-shaped strings are
these two and XML — and the command tooltip is built from them (0x004f3590 for the title,
0x004ea790 for the costs):

```
<color 255 255 0>%s1<color 255 255 255>  <imagetransp gameres/infobar/common/hotkey.bmp> <color 255 255 255>%s2
<color 255 255 255><imagetransp gameres/infobar/common/gold_cost.bmp> 250 <color 255 0 0><imagetransp gameres/infobar/common/food_cost.bmp> 100
```

The title template goes through the translation table (whose shipped Italian entry is an
older `(Hotkey : %s2)` form, so the untranslated one is what shows); `%s1` is the row's
rollover, `%s2` the key in upper case; a row with no key is the bare name wrapped in
yellow. The cost line is one icon and number per non-zero cost, white while the settlement
can pay and red when it cannot — the stamina icon becomes `Loyality.bmp` at ten and above.
The same tags are in `MPGAMEMENU.INI`'s bonus `HelpText` and in a verifier's
`reasonText = rollover(bld, true) + "\n<color 255 0 0>"` (`CREATE_FOOD_MULE_VERIFY.VS`).

`core/ui/markup.hpp` parses and draws them: a colour holds until the next tag, across
breaks; a break is a newline or the two characters `\n`; an image stands in the line like a
glyph, keyed at its top-left pixel for `imagetransp`, and the line grows to hold it (the
shipped icons are 18 and 20 pixels against a 14-pixel font). `<image path>` without the key
is accepted by analogy and unattested. What is not read: whether the original centres an
image in its line, whether a colour persists across a break (every shipped composer resets
to white before its own), and any tag the shipped text does not use.

## The editor

`DATA/INTERFACE/EDITOR/` is 52 files and **no root screen**: `gbr.exe`'s editor is a native
mode — a live map view with floating dialogs, each opened by path from the tool that owns it
(`EditorIni/MapToolsDlg.ini` beside its vtable at 0x7b5f68, `DefaultTool.ini` 0x7b5a9c,
`AdvExplorerDlg.ini` 0x7b39a4 …). No menu resource exists, so Save, Open and the map
properties are reached by keys the data does not declare. The files reference each other
only through `TmplIni=Editorini/template.ini` (`StdDlg`, `AdvDlg`, `ToolSettingsDlg`,
`TextList`, `BrushesCtrl` = `BrushSize`, `SrcEdit`, …) and `%Art%` = `EditorRes/`.

**The tools palette** (`MAPTOOLSDLG.INI`) holds a `Browser` — `Type = Control`, `Id 0x100FA`,
`TargetId = Browser.VScroll` — which the exe fills with a native tree built at
0x004a2a80..0x004a3a00: `Decorations`, `Height` (raise/lower, smooth, set), `Terrains`
(`TERRAINS.XML`'s types), `Structures` and `Units` from the classes' `edittree_pos` (400
classes: `Structures/Stronghold (Gaul)/Barracks`, the seasonal aliases one level deeper),
`Edit objects`, areas — each node paired with the settings pane docked into `SettingsPosCtl`
(`PlaceObj.ini`, `PlaceBuilding.ini`, `DecorSettings.ini` …). Labels translate as
`<leaf>@editortree/<parent path>` (`Bowman@editortree/Units/Britain`); 0x4a277b pushes the
context. This engine's editor (`--edit`) fills the `Browser` with one row per visible node
— a `Control` filled with items draws and answers as a `List` — and docks `PlaceObj.ini`
where the exe docks it; a leaf chosen places its class on a click, `Edit objects` selects,
drags and deletes; `SaveDlg.ini` names the container written. **Readings, labelled:** the
`+`/`-` markers and the keys (Escape, Delete, F2) are this engine's; the editor's screens
draw black where they name no colour, which the exe's Win32-styled controls presumably do
and the data does not say.

**The property sheet** (`ADVOBJPROPS.INI`) is six dialog sections in one file --
`AdvObjPropsParent`, the `StdDlg` frame, and `AdvUnitProps`, `AdvHeroProps`,
`AdvWagonProps`, `AdvBuildingProps`, `AdvMultipleUnitsProps`, one of which 0x004ad6f0
picks for the selection's kind and docks in the parent's `SettingsPosCtl` -- and it is where
**two rules the toolkit keys off the `Id` alone** show up. Every widget's id is tab-based
(`Id = #%IDT_STA% + 3#` with `IDT_STA=0x04000000`), the tab buttons are `Id = #%IDT_STA%
/ 0x01000000 + 0x030000#` = `0x30004`, and the toolkit does the rest (0x0066d515,
0x0066d2a0): a button whose id has `0x03` in bits 16..23 selects the tab in its low
half, which shows every widget whose top byte is that tab and hides the others (a zero top
byte is on every tab); every dialog opens on tab 1 (0x0066d5f0). Likewise an id with
`0x02` in bits 16..23 is a radio button, and pressing it clears the row of every other
radio button agreeing above the low byte (0x0066d410) -- `AdvArea.ini`'s
`0x021001`/`0x021002`, the sheet's `0x4020021`/`0x4020022`. The tab row itself is placed
by `#%TOR_GEN% - 1 * 92 + left(TabsFrame)#`, which only lays out under the flat
evaluator above. What each field does to the object (the handler at 0x004add20 and the
setters beside it) is written case by case in `engine/app/main.cpp`,
`object_properties_event`; [map.md](map.md) has the three `flags` bits and the two
`UnitFlags` bits the Stats tab settles.

**The Adventure Palette** (`ADVEXPLORERDLG.INI`) is the editor's other window, a `StdDlg`
with a `Browser` tree at the left and two docking controls, `SettingsPosCtl` (265, 27, 550
x 610) and the wider `SettingsPosCtl2`. The tree is not in the file: each manager registers
its nodes as it loads, through one routine (0x00463250) that takes the node path, the
dialog file and the label -- `players` / `AdvPlayer.ini` / *Players*, `resources` /
`AdvResources.ini` / *Settlements*, `CurMap` / `AdvCurMap.ini` / *Map*, `CurMap/groups/<name>`
/ `AdvGrpProps.ini`, `CurMap/Areas/<id>` / `AdvArea.ini` / *Area properties*, and so on --
and a node chosen docks its dialog in the control. The frames' `Collapse` (id `0x5000`, the
`CollapseButton` template) rolls a window up: the palette's handler (0x004a25b0) keeps the
height it had and sets it to `0xac`, hiding the widgets below the strip, and the next press
(0x004a23e0) puts both back. Each manager also registers one
*New ...* browseable with its own dialog -- *New note* (0x00472385, `AdvNotes.ini`), *New
sequence*, *New conversation*, *New item type* -- through the same routine, so it stands in
the tree as a leaf; choosing it makes the thing and the dialog opens on it. The frames' `Move` control
(`%ID_MOVE%`, 0x10015) is the strip a window is dragged by, and `%ID_CLOSE%` (0x10082)
its Close. The dialogs it docks are the container's documents, one screen each:
`AdvAdventure`/`AdvScenario` over `game.xml`, `AdvPlayer` over `player<i>.xml` (its
`AICombo` is filled at 0x004743b0: *None*, then `%s1(%s2)` rows of each `data/ai/`
subdirectory -- the list 0x004433b0 builds and sorts, less `default` and names beginning
with `_`, the first letter kept and the rest lowered -- with `easy`, `medium`, `hard`
translated in the `AIProfile` context; a row carries its profile and a difficulty of 0, 1
or 2, and the apply at 0x00475884 writes them to `AI` and `difficulty`, *None* being an
empty profile at 0), `AdvDiplomacy` over the players' `relations` words (its rows are authored at `RectWH = x,
y, 0, 0`, which the toolkit sizes from the bitmap's cell), `AdvCurMap` over `map.xml` (its
Maps tab, shared with `AdvAdventure`, is filled at 0x0046fac0: a row per map, its `map.xml`
name, the map being edited marked with `EditorRes/curmap.bmp`; *New* (0x0046f380) takes the
first number from 1 no map has and copies `Packs/newmap/Maps/1` in as `Maps/<n>`; *Import
adventure maps* and *Import scenario map* open `OPENDLG.INI` over `adventures/` or
`scenarios/` with `bfhp` (0x004aedd0); *Edit* opens the chosen map, *Delete* removes it),
`AdvNotes` over `Notes.xml`, `AdvSequence` over `sequences.xml` and the `.vs` it names,
`AdvConverse` over a `.conv.xml`, `AdvItems` over `itemsCustom.xml`, `AdvResources` and
`AdvOutpost` over the settlements, `AdvGrpProps` and `AdvItemHolder` over the map's
groups and chests, and the *Overlay text* tab of `AdvCurMap` and `AdvScenario` over
`labels.xml` (its handler 0x004a1640: *New* lays `<New label>` at the centre of the view,
*Delete* cuts the chosen one, *Set* writes the text, *Place* opens `MAPPLACE.INI`). This
engine edits those documents in place (`core/xml_patch.hpp`, see [map.md](map.md),
"Writing"); what each control does is written beside its case in `engine/app/main.cpp`.

**Place Label** (`MAPPLACE.INI`, opened at 0x004a0af0) is a full-window `StdDlg` whose
`BMP` is an `INACTIVE` button the code hangs the minimap on -- `ImageType = AAAAA` names
no cell -- and reads the click off; its `Text` is the label in `Fonts/Tahoma16b.apf`,
yellow, and `ErrText` says *Building Minimap. Please wait.* while the picture composes.
The labels are drawn onto the minimap by one routine (0x0049f7a0, the only drawer): each
scaled into the picture, centred on its point, kept inside the edges, in
`Fonts/tahoma16b.apf`. Its two callers are the editor's floating minimap window and this
screen; nothing in the game's own screens reads `labels.xml`.

**A saved scenario** is what `Packs/emptyscn.bfhp` holds — `game.xml`, `itemsCustom.xml`,
`Notes.xml`, `Sequences/sequences.xml`, `Maps/1/{map.xml, map.obj.xml, Notes.xml,
Sequences/sequences.xml, six Terrain.*.grid, warehouse.rle}` — plus the sixteen
`player<i>.xml` the templates lack, which is why the four templates build no session. See
[map.md](map.md), "Writing".

## Faction bindings

`gbr.exe` holds the faction table as eight adjacent pairs of paths:

| faction | info bar | command bar | no-selection bar | `%RaceArt%` |
|---|---|---|---|---|
| Gaul | `infobar_gaul.ini` | `cmdbar.ini` | `empty_gaul.ini` | `gameres/infobar/GAUL` |
| Republican Rome | `infobar_rrome.ini` | `cmdbarrrome.ini` | `empty_rome.ini` | `.../RROME` |
| Imperial Rome | `infobar_irome.ini` | `cmdbarirome.ini` | `empty_rome.ini` | `.../IROME` |
| Carthage | `infobar_carthage.ini` | `cmdbarCarthage.ini` | `empty_carthage.ini` | `.../CARTHAGE` |
| Egypt | `infobar_egypt.ini` | `cmdbarEgypt.ini` | `empty_egypt.ini` | `.../EGYPT` |
| Iberia | `infobar_iberia.ini` | `cmdbarIberia.ini` | `empty_iberia.ini` | `.../IBERIA` |
| Britain | `infobar_britain.ini` | `cmdbarbritain.ini` | `empty_britain.ini` | `.../BRITAIN` |
| Germany | `infobar_german.ini` | `cmdbarGerman.ini` | `empty_german.ini` | `.../GERMAN` |

Gaul's is the unsuffixed `cmdbar.ini`, which `CONST.INI` also names as
`[UIBars] LowerDefault`. Both Romes share one no-selection bar, which is why seven
`EMPTY_*.INI` files serve eight factions.

The twenty-fourth file, `CMDBAR/FRAMES.INI`, is not a screen: it is one `[Images]` section
mapping each faction to the frame its command buttons are drawn in, which the engine
composites at run time around an icon chosen per order. `CONST.INI` caps that row at
`[UIBars] CmdBarMaxButtons = 16`.

## The lower bar is two screens

The no-selection menu declares no background of its own, because it is an overlay: the
command bar screen supplies the background and `EMPTY_*.INI` draws its nine buttons over
it. `gbr.exe` calls the class `CVXUIEmptySelectionMenu`. The upper bar is one screen.

## What the reader does with all this

`engine/core/ui` parses, resolves, lays out and composites, with no window and no
filesystem: it is handed a callback that turns a virtual path into bytes.
`engine/platform/src/ui_renderer.cpp` provides that callback from the `Vfs`, uploads the
composited RGBA and blits it. The bars are composited in software because the original was
a 16-bit software rasteriser doing exactly that, because together they are under 110,000
pixels and change only when the selection does, and because the alternative is a second GPU
pipeline for true-colour quads beside the index-atlas one the sprites use.

**Measured against the retail packs**, all 24 files:

* 23 screens across 23 files load, plus `FRAMES.INI`'s 8-entry image table;
* 352 widgets, **0 unresolved sections, 0 warnings, 0 unknown styles, 0 unknown bare lines**;
* 424 image references, 157 distinct, **all resolve** in the shipped packs, as do all fonts;
* every screen lays out and composites at 1024 and at 1280 wide.

The `.bmp` reader agrees byte for byte with an independently written Python decoder over the
retail art, and the `.apf` reader agrees with
[`apf.py`](../../src/imperivm/formats/apf.py) on the glyph count, metrics, advances and every
coverage byte of all six shipped fonts.

## What the bar shows

The chrome is the files'; the contents are the class graph's, and the interpreter is handed
them by [`sim/infobar.hpp`](../../engine/core/include/imperivm/core/sim/infobar.hpp), which
reads them the way the original does:

```xml
<properties interface="thumb,unit,items"/>           the selection tags a widget's bare lines test
<properties icon="gameres/icons/RPraetorian.bmp"/>   the portrait
<properties display_name="Praetorian"/>              the name, a translation key
<value0 icon="gameres/infobar/common/level ico.bmp" script="return .AsUnit.level;" rollover="Level"/>
```

The six stat slots are **inline `.vs`** in [`sc-xml.md`](sc-xml.md)'s `<value0..5>`, one
expression each, run against the selected object with `this` bound to it — the same
synchronous side-run that evaluates a `verify=` script. A building's are settlement reads
(`set.population + '/' + set.max_population`); a multiple selection's are the `Sel*` family
(`SelAvgLevel()`, `SelHealth() * 100 / SelMaxHealth() + '%'`), read out of `gbr.exe`
(rounded means over the selected units and every selected hero's army; sums for the two
health calls; an empty selection answers 0, or 1 for the level).

**Which class is read** is `gbr.exe`'s rule at 0x005e5720: nothing selected shows `Empty`;
one object shows its own class, a `Wagon` becoming `WagonGold` or `WagonFood` by its cargo;
several show `<R>PeasantMulti` for peasants of one race, else by the first selected object
`Wagons`, `Multi` (`MultiOneShip` for a ship) for a hero or a mixed selection not led by a
sentry, and otherwise `MultiOne`, `MultiOneShip` or `MultiOneRanged`.

The strips: `BuildingQueue` is the building's `train` rows behind its running command, by
the row's `queueicon`, the running one in `TrainFrameImage` with the bar `Obj::Progress`
stamped; `UIHolder` is the settlement's garrison, each unit's `icon`, level and health, in
`SelFrameImage` when it is also selected; `UIInventory` is the hero's items by `ITEMS.XML`'s
`image`, glowing when the item has a `use_script`; `HeroSkills` the skills the hero's class
offers, in `SKILLS.INI` order, with points and `PlusSign` when one can be spent;
`UnitSpecials` the class's `unit_specials` through `UNIT_SPECIALS.INI`. `Combiner` draws
`Id1`'s cells and continues the row with `Id2`'s.

Two things the first selection put in the bar found: a class icon is keyed on the pure green
at its top-left corner when the corner *is* that green, because the `icon=` property carries
no probe; and the health ramps run green-to-red left to right, so full health samples column
zero — the bar had been sampling the other way for as long as no selection reached it.

## What the command bar shows

The lower bar's file is a background, a right cap and a hidden `CmdCancel`; the buttons are
the engine's, from the command rows (`docs/formats/sc-xml.md`'s companion, `DATA/COMMANDS`):

```xml
<cmd name="trainBSwordsman" button="actions/train BSwordsman.bmp" key="s" priority="3"
     groupverifier="data/subai/verify_cmdcost_building.vs" rollover="Equip Swordsman" ...>
  <src obj="BBarracks"/>
</cmd>
```

[`sim/cmdbar.hpp`](../../engine/core/include/imperivm/core/sim/cmdbar.hpp) offers a
selection the rows whose `<src>` is an ancestor of every selected object's class and whose
`<nsrc>` is not, keeps the ones with a `button=`, orders them by `priority`, caps them at
`CmdBarMaxButtons`, and runs each `groupverifier=` — `bool f(ObjList objs, str OUT
reasonText)` — the way a `verify=` is run, with the row *described* so that `cmdparam` and
`cmdcost_*` answer for it outside any command script. A press issues a row with no
`<cmdtext>` at once, one per actor (or through its `groupdispatch=` script), and arms a row
with one to take the next click on the map. The key letter presses the button; Escape and a
right click disarm it.

Each button is the faction's frame (`CMDBAR/FRAMES.INI`, a three-state strip: normal,
highlighted, pressed) over the row's `button=` icon (a three-state strip too, 153 × 51 across
the whole `UI/CmdBar/Actions` set); a refused row draws the pressed frame, which is the
`ABCCC` reading the file's own `Button` widgets declare. **Where the row stands is a
reading:** centred in the bar, the buttons abutting — the no-selection menu is a 612-wide
dialog the bar centres, and `gbr.exe` computes `(parent + 1 − child + 1) / 2` for a child at
0x006b3e00 — and the no-selection menu shows for an empty selection and for nothing else,
which is what its class is called (`CVXUIEmptySelectionMenu`). Its quick-save and quick-load
buttons (`ID = 0x1003`, `0x1004`) are wired; the other seven are the menus', Part 6's next.

## What is still unknown

- **Which entry wins when a Params section and the section it inherits from define the same
  name.** The reader keeps the local one, matching `Template`'s behaviour on a widget, which
  *is* observable. In a Params section it is not: no shipped file has a collision, so the
  choice is unfalsifiable against the corpus and a decompilation would settle it in one
  look.
- ~~**The numeric value of any style flag.**~~ Read: the eight per-class tables above.
  What is still a reading is how a local `Style` combines with its template's (unioned,
  local alignment winning).
- **Whether an unknown `%Name%` is really left standing.** `gbr.exe` has no diagnostic for
  one, and no shipped file has one. Leaving it visible is a choice made for debuggability.
- ~~**The precedence of `|` and `&` against arithmetic.**~~ Settled: there is none, for
  any operator -- see the expressions section.
- **What the eight `Show...` relationships mean precisely**, and which one a given object
  falls into. The files say a widget is `ShowCeaseFire = yes`; what the engine tests is not
  written down anywhere in the data.
- **The `ImageType` state order.** Five letters map to five control states and the codes are
  consistent with normal / highlighted / pressed / disabled / selected — `ABCCC` for a menu
  button, `ABBCA` for a cancel button whose selected state falls back to its resting frame —
  but nothing in the data names the states. The four-letter codes (`AAAA`, `AAAB`, `AABB`)
  presumably belong to a class with one state fewer; which one is missing is not known.
- **`UserData`, `Break`, `Debug`, `ScrollID`, `VScrollID`, `TextId`** and the rest of the
  toolkit keys nothing here reads. They are parsed and kept as attributes, uninterpreted.
  `Esc`, `Enter`, `Focus`, `TargetId`, `BackID`, `ForwardID`, `Dividers`, `XFrames`,
  `YFrames`, `Bufsize` and the `...Offs` keys are read now.
- **The menu widgets' readings**, each labelled where it stands in the table above: where a
  single line of text sits vertically, that a frame's middle is tiled rather than
  stretched, which `FrameColor` goes where on a `Background`, that an `ImageButton`'s cell is
  centred in its rectangle, the closed height of a combobox, the notes list's colours, the
  bullets beside a caption, and that a dialog is centred in the display.
- **What the original draws for a `List` item's icon when the note declares none**, and
  what it draws when the declared bitmap is missing: 75 of the 82 shipped notes declare no
  icon and the seven that do name `noteicons/triangle.bmp`, which no pack holds (the
  editor's own table lists seven others). Nothing is drawn either way.
- **How the adventure menus fit the adventure's picture** (440 × 360, beside its
  container) **into their 700 × 300 caption**. Scaled to fit, keeping its shape; a reading.
- **The editor's `BmpScroll`** is not drawn.
- **Inline markup**: an image's vertical placement in its line, and whether a colour
  persists across a break, are readings (see "Inline markup" above).
- **The command buttons' geometry.** Read off the art and the menu's dialog size, not off
  the executable; see above. A screenshot of the original would settle it in a minute.
- ~~**The runtime contents of `BuildingQueue`, `UIHolder`, `UIInventory`, `HeroSkills`,
  `UnitSpecials` and `Combiner`.**~~ Drawn now, from what the simulation hands the
  interpreter through `BarContent`'s five strips — see [What the bar shows](#what-the-bar-shows)
  below. What is still a reading rather than a measurement: the gap between cells is the
  widest of `MinIconSpace..MaxIconSpace` the row allows; `BuildingQueue`'s number is not
  drawn (what the original prints at `NumberYPosition` for a queue entry is unestablished);
  `HeroSkills`' `IconTextOffset` is taken from the cell's top-right corner; `UnitSpecials`
  shows the first three of a class's `unit_specials`.
- **`CONST.INI`'s `[UIBars] UpperDefault = data/interface/Infobar/empty/infobar.ini` names a
  file that is not in any pack.** `LowerDefault` beside it resolves. `gbr.exe` also looks
  for `UIShowDelay` and `UIHideDelay` keys that `[UIBars]` does not contain.
