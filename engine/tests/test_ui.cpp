// The interface interpreter.
//
// The fixtures below are **written from `docs/formats/interface-ini.md`**, in
// the shapes the shipped screens use. They were verbatim copies of retail
// files until `tools/check_fixtures.py` was written and found them:
// `docs/legal.md` rule 1 forbids game assets in the repository, "not as test
// fixtures", and an `.ini` full of the game's own button labels and art paths
// is game assets.
//
// **What made the copies valuable is kept deliberately.** Hand-authored
// fixtures have accused four correct readers on this project already, always
// the same way -- by being tidier than the data. So these keep every untidiness
// that carried a meaning: the `STYLE = Transparent` that only one file spells
// in that case, the `HelpText =` with no space after it, the `%Buttons%` and
// `%buttons%` in one file, the commented-out lines, the section whose name
// differs in case from the `Objects` list that names it, the button row that
// overflows the screen it sits on by seven pixels, and the alignment weights
// `0,1,4 / 1,1,3 / 2,1,2 / 3,1,1 / 4,1,0` that are the corpus's one
// unambiguous discrimination between ratios and flags. What changed is the
// art paths and the button labels, which are the game's and prove nothing.
//
// The core tests link nothing and open nothing, so the fixtures live here
// rather than being read from the install. What they cannot cover -- that all
// 24 in-game files load, that every image path they name resolves, that the
// result looks like the game -- is checked against the retail packs by the
// harness described in docs/formats/interface-ini.md.

#include <cstring>
#include <string>
#include <string_view>

#include "imperivm/core/ui/font.hpp"
#include "imperivm/core/ui/image.hpp"
#include "imperivm/core/ui/interface.hpp"
#include "imperivm/core/ui/layout.hpp"
#include "imperivm/core/ui/paint.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::ui;

namespace {

// -- fixtures ---------------------------------------------------------------

/// The shared template a screen reaches through `%TmplIni%`: a `[Params]`
/// block and two dialog shapes. `DATA/INTERFACE/TEMPLATE.INI` has this form.
constexpr std::string_view kGameTemplate = R"(;the shared template

[Params]
Art = gameres
InterfaceFont=Fonts/Test14b.apf

[StdDlg]
Type = Dialog
MinSize = 320, 200
MaxSize = 1024, 768
RectWH = 0, 0, 320, 200

[UpperDlg]
Type = Dialog
MinSize = 320, 200
MaxSize = 1024, 768
RectWH = 0, 0, 1024, 80
)";

/// A command bar: nine equal buttons in a row, each placed by an expression
/// naming the one before it, on a screen narrower than the row it holds.
///
/// The button names are the interface's own vocabulary and are kept, because
/// the tests look widgets up by them and because a name is not content. The
/// art paths and the labels are written here.
constexpr std::string_view kEmptyRome = R"([test_bar]
STYLE = Transparent
Template = %TmplIni%, StdDlg
RectWH = 0, 0, 612, 53

[test_bar Objects]
Help
Diplomacy
QLoad
Party
Notes
QSave
MainMenu
Chat
Minimap

[test_bar Params]
Template=%TmplIni%, Params
TmplIni=gameini/template.ini
Buttons = gameres/CmdBar/testbar
Dist = 20


[Help]
Type = Button
RectWH = 0,1,51,51
ImageType = ABCCC
Image = %buttons%/one.bmp,0,0
HelpText = Button one (F1)
ID = 0x1008

[Diplomacy]
Type = Button
RectWH = #right(Help)+%dist%#,1,51,51
ImageType = ABCCC
Image = %buttons%/two.bmp,0,0
HelpText = Button two (F5)
ID = 0x1005

[QLoad]
Type = Button
RectWH = #right(Diplomacy)+%dist%#,1,51,51
ImageType = ABCCC
Image = %buttons%/three.bmp,0,0
HelpText = Button three (F6)
ID = 0x1003

[Party]
Type = Button
RectWH = #right(QLoad)+%dist%#,1,51,51
ImageType = ABCCC
Image = %Buttons%/four.bmp,0,0
HelpText =Button four (F7)
ID = 0x1009

[Notes]
Type = Button
RectWH = #right(Party)+%dist%#,1,51,51
ImageType = ABCCC
Image = %buttons%/five.bmp,0,0
HelpText = Button five (F8)
ID = 0x1002

[QSave]
Type = Button
RectWH = #right(Notes)+%dist%#,1,51,51
ImageType = ABCCC
Image = %buttons%/six.bmp,0,0
HelpText = Button six (F9)
ID = 0x1004

[MainMenu]
Type = Button
RectWH = #right(QSave)+%dist%#,1,51,51
ImageType = ABCCC
Image = %Buttons%/seven.bmp,0,0
HelpText = Button seven (F10)
ID = 0x1001

[Chat]
RectWH = #right(MainMenu)+%dist%#,1,51,51
Type = Button
ImageType = ABCCC
Image = %buttons%/eight.bmp,0,0
HelpText = Button eight (Enter)
ID = 0x1006

[Minimap]
Type = Button
RectWH = #right(Chat)+%dist%#,1,51,51
ImageType = ABCCC
Image = %buttons%/nine.bmp,0,0
HelpText = Button nine (Space)
ID = 0x1007
)";

/// A selection bar: a skinned background, a selection name, two dividers that
/// differ only in their tags, and an inventory.
///
/// The section is `[Infobar_RRome]` and the `Objects` list says
/// `[InfoBar_RRome Objects]` -- a different case in the middle of the name --
/// because that disagreement is in the shipped file and a reader that folded
/// only the first letter would still load it.
constexpr std::string_view kInfobarRRome = R"([Infobar_RRome]
Template = %TmplIni%, UpperDlg
Style = TRANSPARENT
;Debug
[InfoBar_RRome Objects]
Background
BackgroundEmpty
BackgroundFrame

Name

MultiDivider
UnitDivider
Inventory

[InfoBar_RRome Params]
Template=%TmplIni%, Params
TmplIni=gameini/template.ini
RaceArt=gameres/infobar/TESTSKIN

[Background]
Type = Button
ImageType = AAAAA
Image = %RaceArt%/panel.bmp
RectWH = 0, 0, 2048, 80
HAlign = 0, 0, 1
VAlign = 0, 0, 1
ShowAll = yes
Style = INACTIVE, TRANSPARENT

[BackgroundEmpty]
Type = Icon
Image = %RaceArt%/panel_empty.bmp
RectWH = 0, 0, 2048, 80
HAlign = 0, 0, 1
VAlign = 0, 0, 1
ShowAll = yes
Style = INACTIVE, TRANSPARENT
empty

[BackgroundFrame]
Type = Button
ImageType = AAAAA
Image = %RaceArt%/panel_frame.bmp, 0, 40
RectWH = 991, 1, 33, 78
HAlign = 1, 0, 0
VAlign = 0, 0, 1
ShowAll = yes
Style = INACTIVE, TRANSPARENT

[Name]
Type = SelectionName
RectWH = 129, 8, 225, 20
Font = %InterfaceFont%
BufSize = 64
HAlign = 0, 0, 1
VAlign = 0, 0, 1
Style = INACTIVE, TRANSPARENT, ALIGN_LEFT
ShowAll = yes

[MultiDivider]
Type = Icon
Image = %RaceArt%/rule.bmp, 0, 0
RectWH = 501, 1, 22, 78
HAlign = 1, 0, 1
VAlign = 0, 0, 1
Style = INACTIVE, TRANSPARENT
ShowAll = yes
unit
holder

[UnitDivider]
Type = Icon
Image = %RaceArt%/rule.bmp, 0, 0
RectWH = 600, 1, 22, 78
HAlign = 1, 0, 1
VAlign = 0, 0, 1
Style = INACTIVE, TRANSPARENT
ShowAll = yes
unit
!holder

[Inventory]
Type = UIInventory
Style = TRANSPARENT
Rect = 550, 2, 984, 77
Font = %InterfaceFont%
TextColor = 255, 255, 255
HAlign = 1, 1, 0
VAlign = 0, 1, 0
ShowAll = yes
ReverseDraw
items
!unit
Id = 0xa000
TabMask = 1
)";

/// `DATA/INTERFACE/COMMON/SCRIPTEDIT.INI`, the four sections whose alignment
/// weights prove the springs are ratios, plus the header it needs to load.
/// This file is **not** one of the 24 this part owns, which is what makes it an
/// independent control on the layout rule.
constexpr std::string_view kScriptEdit = R"([ScriptEdit]
Template = %TmplIni%, StdDlg

[ScriptEdit Objects]
SList.Back
SList.VScrollBack
ScriptBack
Script.VScrollBack

[ScriptEdit Params]
Template=%TmplIni%, Params
TmplIni=commonini/template.ini

[SList.Back]
RectWH = 7, 46, 125, 148
HAlign = 0, 2, 8
VAlign = 0, 1, 0

[SList.VScrollBack]
RectWH = 132, 58, 15, 123
HAlign = 2, 0, 8
VAlign = 0, 1, 0

[ScriptBack]
RectWH = 154, 46, 144, 127
HAlign = 2, 8, 0
VAlign = 0, 1, 0

[Script.VScrollBack]
RectWH = 298, 58, 15, 103
HAlign = 1, 0, 0
VAlign = 0, 1, 0
)";

/// `DATA/INTERFACE/COMMON/TEMPLATE.INI`, `[Params]` and `[StdDlg]`, verbatim.
constexpr std::string_view kCommonTemplate = R"([Params]
; paths
; Id constants
ID_SIZELEFT=0x10001
ID_MOVE=0x10015

[StdDlg]
Type = Dialog
MinSize = 320, 200
MaxSize = 1024, 768
RectWH = 0, 0, 320, 200
)";

/// A five-column table header, each column measured from the one to its left
/// and each with a different alignment weight.
///
/// The weights `0,1,4 / 1,1,3 / 2,1,2 / 3,1,1 / 4,1,0` are not invented: they
/// are the shipped editor's, and they are the corpus's one unambiguous
/// discrimination between reading the triple as **ratios** and reading it as
/// flags, because under flags the middle three all collapse to `1,1,1`. A
/// number is a fact about the format; the column labels are not, so those are
/// written here.
constexpr std::string_view kAdvAdventure = R"([AdvAdventure]
Template=%TmplIni%, AdvDlg
MinSize = 512, 600

[AdvAdventure Objects]
ChooseStartCheck
HeadersBack
HeadersText1
HeadersText2
HeadersText3
HeadersText4
HeadersText5

[AdvAdventure Params]
Template=%TmplIni%, Params
TmplIni=editorini/template.ini
LeftWidth = 248
RightWidth = 248
FullWidth = #%LeftWidth% + %RightWidth% + 9#

[ChooseStartCheck]
RectWH = 15, 85, 16, 16
Id = 0x3007021
HAlign = 0,0,1

[HeadersBack]
Template = %TmplIni%, DisabledShadowFrame
RectWH = #left(ChooseStartCheck)#, #bottom(ChooseStartCheck) + 10#, %FullWidth%, 20
Id = 0x3001000

[HeadersText1]
Template = %TmplIni%, StaticText
RectWH = #left(HeadersBack)+5#, #top(HeadersBack)+1#, 90, 16
Text = Col1
id=0x300050B
HAlign = 0,1,4

[HeadersText2]
Template = %TmplIni%, StaticText
RectWH = #right(HeadersText1) + 10#, #top(HeadersBack)+1#, 50, 16
Text = Col2
id=0x300050C
HAlign = 1,1,3

[HeadersText3]
Template = %TmplIni%, StaticText
RectWH = #right(HeadersText2) + 10#, #top(HeadersBack)+1#, 70, 16
Text = Col3
id=0x300050D
HAlign = 2,1,2

[HeadersText4]
Template = %TmplIni%, StaticText
RectWH = #right(HeadersText3) + 10#, #top(HeadersBack)+1#, 70, 16
Text = Col4
id=0x300050E
HAlign = 3,1,1

[HeadersText5]
Template = %TmplIni%, StaticText
RectWH = #right(HeadersText4) + 10#, #top(HeadersBack)+1#, 165, 16
Text = Col5
id=0x300050F
HAlign = 4,1,0
)";

/// The template the table above includes: a dialog, a text style and a frame.
/// The commented-out `RectWH` and `MinSize` are the shape the shipped editor
/// template has, and they are here because a reader that took the *first*
/// value of a repeated key rather than the last would still load it.
constexpr std::string_view kEditorTemplate = R"([Params]
; paths
Art=editorres
;
BoldFontPath =Fonts/Test13b.apf
FontPath = Fonts/Test13.apf

[AdvDlg]
Type = Dialog
;RectWH = 0, 0, 639, 603
RectWH = 0, 0, 550, 603
MinSize = 345, 300
;MinSize = 550, 603
MaxSize = 1600, 1200
Style = TRANSPARENT

[StaticText]
Type = TextW
Font = %FontPath%
Bufsize = 256

[DisabledShadowFrame]
Type = Background
HAlign = 0, 1, 0
;BkColor = 230, 220, 191
BkColor = 208, 192, 144
FrameColor1 = 164, 138, 86
)";

/// An image table rather than a screen: one entry per faction, each naming a
/// bitmap and a colour key. `DATA/INTERFACE/CMDBAR/FRAMES.INI` has this form
/// and is the one of the 24 in-game files that declares no widgets at all.
///
/// **The eight names are the engine's and are kept.** They are the values
/// `PlayerSetup::race` takes and the keys `<class race=>` is written against,
/// so a table that renamed them would stop testing the thing this file cares
/// about, which is that a skin exists for every faction the simulation can
/// hand it. The paths are written here.
constexpr std::string_view kFrames = R"([Images]
Gaul =           gameres/cmdbar/frames/f1.bmp, 10, 10
RepublicanRome = gameres/cmdbar/frames/f2.bmp, 10, 10
ImperialRome =   gameres/cmdbar/frames/f3.bmp, 10, 10
Iberia =         gameres/cmdbar/frames/f4.bmp, 10, 10
Carthage =       gameres/cmdbar/frames/f5.bmp, 10, 10
Britain =        gameres/cmdbar/frames/f6.bmp, 10, 10
Egypt =          gameres/cmdbar/frames/f7.bmp, 10, 10
Germany =        gameres/cmdbar/frames/f8.bmp, 10, 10
)";

std::span<const std::byte> as_bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// The fixture provider. Paths are matched after alias resolution, exactly as
/// the interpreter hands them over.
std::span<const std::byte> fixtures(std::string_view path) {
  if (path == "data/interface/template.ini") return as_bytes(kGameTemplate);
  if (path == "data/interface/common/template.ini") return as_bytes(kCommonTemplate);
  if (path == "data/interface/cmdbar/test_bar.ini") return as_bytes(kEmptyRome);
  if (path == "data/interface/infobar/infobar_rrome.ini") return as_bytes(kInfobarRRome);
  if (path == "data/interface/common/scriptedit.ini") return as_bytes(kScriptEdit);
  if (path == "data/interface/editor/advadventure.ini") return as_bytes(kAdvAdventure);
  if (path == "data/interface/editor/template.ini") return as_bytes(kEditorTemplate);
  if (path == "data/interface/cmdbar/frames.ini") return as_bytes(kFrames);
  return {};
}

}  // namespace

// -- the alias table --------------------------------------------------------

TEST(ui_aliases_rewrite_virtual_roots) {
  CHECK(resolve_alias("gameres/CmdBar/help.bmp") == "ui/CmdBar/help.bmp");
  CHECK(resolve_alias("gameini/template.ini") == "data/interface/template.ini");
  CHECK(resolve_alias("menuini/confirm.ini") == "data/interface/menu/confirm.ini");
  CHECK(resolve_alias("menures/ShieldArrow.bmp") == "ui/menu/ShieldArrow.bmp");
  CHECK(resolve_alias("EditorRes/curmap.bmp") == "ui/editor/curmap.bmp");
  CHECK(resolve_alias("commonini/template.ini") == "data/interface/common/template.ini");
  // Separators are unified and a path with no alias is left alone.
  CHECK(resolve_alias("gameres\\icons\\x.bmp") == "ui/icons/x.bmp");
  CHECK(resolve_alias("data/const.ini") == "data/const.ini");
}

// -- %Name% -----------------------------------------------------------------

TEST(ui_params_expand_recursively_and_case_insensitively) {
  ParamScope scope;
  scope.set("Buttons", "gameres/CmdBar/testbar");
  scope.set("Dist", "20");
  // MENU/TEMPLATE.INI really does define one parameter in terms of another.
  scope.set("ButtonSpacing", "44");
  scope.set("ButtonSpacing2", "%ButtonSpacing% + %ButtonSpacing%");

  CHECK(scope.expand("%buttons%/one.bmp") == "gameres/CmdBar/testbar/one.bmp");
  CHECK(scope.expand("%BUTTONS%/one.bmp") == "gameres/CmdBar/testbar/one.bmp");
  CHECK(scope.expand("#right(Help)+%dist%#") == "#right(Help)+20#");
  CHECK(scope.expand("%ButtonSpacing2%") == "44 + 44");
  // An unbound name survives as itself rather than becoming an empty string.
  CHECK(scope.expand("%NoSuchThing%/x") == "%NoSuchThing%/x");
  CHECK(scope.expand("100%% sure") == "100% sure");
}

TEST(ui_param_expansion_terminates_on_a_cycle) {
  ParamScope scope;
  scope.set("A", "%B%");
  scope.set("B", "%A%");
  // The value is uninteresting; not hanging is the point.
  const std::string out = scope.expand("%A%");
  CHECK(!out.empty());
}

// -- #expr# -----------------------------------------------------------------

TEST(ui_expressions_evaluate_integer_arithmetic) {
  const RectLookup none = [](std::string_view, std::string_view, std::int32_t&) { return false; };
  std::int32_t value = 0;
  CHECK(evaluate_expression("740+0", none, value) && value == 740);
  CHECK(evaluate_expression("40 + 44 + 44", none, value) && value == 128);
  CHECK(evaluate_expression("400-32", none, value) && value == 368);
  CHECK(evaluate_expression("2 * 0x100 + 6", none, value) && value == 518);
  CHECK(evaluate_expression("(1 + 2) * 3", none, value) && value == 9);
  CHECK(evaluate_expression("0x10001 | 4", none, value) && value == 0x10005);
  CHECK(evaluate_expression("7 % 4", none, value) && value == 3);
  CHECK(!evaluate_expression("1 +", none, value));
  // The evaluator is flat (0x006681fc): left to right, no precedence, and
  // a division by zero is 0. `AdvObjProps.ini`'s tab row depends on it.
  CHECK(evaluate_expression("1 / 0", none, value) && value == 0);
  CHECK(evaluate_expression("2 - 1 * 92 + 5", none, value) && value == 97);
  CHECK(evaluate_expression("1 + 2 * 3", none, value) && value == 9);
  CHECK(evaluate_expression("0x04000000 / 0x01000000 + 0x030000", none, value) && value == 0x30004);
}

TEST(ui_expression_spans_are_replaced_in_place) {
  const RectLookup lookup = [](std::string_view fn, std::string_view name,
                               std::int32_t& out) {
    if (name != "Help") return false;
    if (fn == "right") {
      out = 51;
      return true;
    }
    return false;
  };
  CHECK(evaluate_expressions("#right(Help)+20#,1,51,51", lookup) == "71,1,51,51");
  // A span that cannot be evaluated stays verbatim, markers and all, so the
  // failure is visible instead of becoming a plausible zero.
  CHECK(evaluate_expressions("#right(Nope)+20#,1", lookup) == "#right(Nope)+20#,1");
}

// -- Template ---------------------------------------------------------------

TEST(ui_template_include_pulls_a_section_from_another_file) {
  const Result<Screen> screen = load_screen("gameini/infobar/infobar_rrome.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  // `[Infobar_RRome]` declares no RectWH of its own; `Template = %TmplIni%,
  // UpperDlg` brings 0, 0, 1024, 80 in from DATA/INTERFACE/TEMPLATE.INI. That
  // is the whole mechanism in one assertion.
  CHECK(screen->design == (Rect{0, 0, 1024, 80}));
  CHECK(screen->max_size.width == 1024 && screen->max_size.height == 768);
  CHECK(has(screen->style, Style::kTransparent));
  CHECK(screen->warnings.empty());
  CHECK(screen->missing.empty());
}

TEST(ui_local_entries_beat_the_template_they_include) {
  // `[test_bar]` includes `[StdDlg]`, whose RectWH is 0, 0, 320, 200, and
  // overrides it with 0, 0, 612, 53.
  const Result<Screen> screen = load_screen("gameini/cmdbar/test_bar.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  CHECK(screen->design == (Rect{0, 0, 612, 53}));
  // MinSize came from the template, which the local section does not mention.
  CHECK(screen->min_size.width == 320 && screen->min_size.height == 200);
  // `STYLE = Transparent` -- the key and the flag name are both matched
  // case-insensitively, and only this file spells either that way.
  CHECK(has(screen->style, Style::kTransparent));
}

TEST(ui_params_inherit_through_their_own_template_line) {
  // `%InterfaceFont%` is defined only in DATA/INTERFACE/TEMPLATE.INI's
  // [Params], and reaches [Name] because [InfoBar_RRome Params] includes it --
  // while `%TmplIni%`, which names that very file, is local to the Params
  // section. Local-then-inherited is the only order that resolves both.
  const Result<Screen> screen = load_screen("gameini/infobar/infobar_rrome.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  const Widget* name = screen->find("Name");
  CHECK(name != nullptr);
  if (name == nullptr) return;
  CHECK(name->attribute("Font") == "Fonts/Test14b.apf");
  CHECK(name->kind == WidgetType::kSelectionName);
  CHECK(has(name->style, Style::kAlignLeft));
}

TEST(ui_race_art_parameter_skins_the_same_layout) {
  const Result<Screen> screen = load_screen("gameini/infobar/infobar_rrome.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  const Widget* background = screen->find("Background");
  CHECK(background != nullptr);
  if (background == nullptr) return;
  CHECK(background->image.path == "ui/infobar/TESTSKIN/panel.bmp");
  CHECK(!background->image.has_key);
  // `%Art%` comes from the shared template, `%RaceArt%` from the skin.
  const Widget* frame = screen->find("BackgroundFrame");
  CHECK(frame != nullptr);
  if (frame == nullptr) return;
  CHECK(frame->image.path == "ui/infobar/TESTSKIN/panel_frame.bmp");
  CHECK(frame->image.has_key && frame->image.key_x == 0 && frame->image.key_y == 40);
}

// -- expressions against sibling widgets ------------------------------------

TEST(ui_button_row_chains_through_right_of_the_previous) {
  const Result<Screen> screen = load_screen("gameini/cmdbar/test_bar.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  CHECK(screen->widgets.size() == 9);
  // 51 wide, 20 apart, starting at 0: 0, 71, 142, ... 568.
  const char* order[9] = {"Help",  "Diplomacy", "QLoad",    "Party",  "Notes",
                          "QSave", "MainMenu",  "Chat",     "Minimap"};
  for (int i = 0; i < 9; ++i) {
    const Widget* widget = screen->find(order[i]);
    CHECK(widget != nullptr);
    if (widget == nullptr) continue;
    CHECK(widget->design == (Rect{71 * i, 1, 51, 51}));
  }
  // The last button's right edge is 619, which is why the screen is 612 wide
  // and the bar overflows it slightly: that is what the file says.
  const Widget* last = screen->find("Minimap");
  CHECK(last != nullptr && last->design.right() == 619);
}

TEST(ui_command_ids_and_help_text_survive) {
  const Result<Screen> screen = load_screen("gameini/cmdbar/test_bar.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  const Widget* help = screen->find("Help");
  CHECK(help != nullptr);
  if (help == nullptr) return;
  CHECK(help->has_id && help->id == 0x1008);
  CHECK(help->help_text == "Button one (F1)");
  CHECK(help->image_type == "ABCCC");
  CHECK(help->image.path == "ui/CmdBar/testbar/one.bmp");
  CHECK(help->image.has_key && help->image.key_x == 0 && help->image.key_y == 0);
}

// -- tags -------------------------------------------------------------------

TEST(ui_bare_lines_are_selection_tags) {
  const Result<Screen> screen = load_screen("gameini/infobar/infobar_rrome.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;

  const Widget* multi = screen->find("MultiDivider");
  const Widget* unit = screen->find("UnitDivider");
  const Widget* empty = screen->find("BackgroundEmpty");
  const Widget* inventory = screen->find("Inventory");
  CHECK(multi != nullptr && unit != nullptr && empty != nullptr && inventory != nullptr);
  if (multi == nullptr || unit == nullptr || empty == nullptr || inventory == nullptr) return;

  // `unit` and `holder`: either will do. `unit` with `!holder`: the second is a
  // veto. Reading positives as a conjunction makes MultiDivider unreachable,
  // which is what settles it.
  CHECK(multi->visible_for(SelectionTag::kUnit));
  CHECK(multi->visible_for(SelectionTag::kHolder));
  CHECK(multi->visible_for(SelectionTag::kUnit | SelectionTag::kHolder));
  CHECK(!multi->visible_for(SelectionTag::kBuilding));

  CHECK(unit->visible_for(SelectionTag::kUnit));
  CHECK(!unit->visible_for(SelectionTag::kUnit | SelectionTag::kHolder));

  CHECK(empty->visible_for(SelectionTag::kEmpty));
  CHECK(!empty->visible_for(SelectionTag::kUnit));

  // No tag at all means every selection.
  const Widget* background = screen->find("Background");
  CHECK(background != nullptr && background->visible_for(SelectionTag::kNone));

  // TabMask is a bit per tab; `Inventory` lives on tab 0 only.
  CHECK(inventory->tab_mask == 1);
  CHECK(inventory->visible_for(SelectionTag::kItems, 0));
  CHECK(!inventory->visible_for(SelectionTag::kItems, 1));
  CHECK(!inventory->visible_for(SelectionTag::kItems | SelectionTag::kUnit, 0));
  // `ReverseDraw` is a flag, not a tag, and must not become one.
  CHECK(inventory->require_any == SelectionTag::kItems);
  CHECK(screen->warnings.empty());
}

TEST(ui_rect_and_rectwh_are_different_keys) {
  const Result<Screen> screen = load_screen("gameini/infobar/infobar_rrome.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  const Widget* inventory = screen->find("Inventory");
  CHECK(inventory != nullptr);
  if (inventory == nullptr) return;
  // `Rect = 550, 2, 984, 77` is left, top, right, bottom.
  CHECK(inventory->from_ltrb);
  CHECK(inventory->design == (Rect{550, 2, 434, 75}));
  const Widget* divider = screen->find("UnitDivider");
  CHECK(divider != nullptr && !divider->from_ltrb);
  CHECK(divider->design == (Rect{600, 1, 22, 78}));
}

// -- layout -----------------------------------------------------------------

TEST(ui_default_alignment_leaves_a_widget_where_it_was) {
  std::int32_t origin = 0;
  std::int32_t extent = 0;
  place_axis(129, 225, 1024, 1280, Align{}, origin, extent);
  CHECK(origin == 129 && extent == 225);
}

TEST(ui_alignment_pins_stretches_and_centres) {
  std::int32_t origin = 0;
  std::int32_t extent = 0;
  // 0,0,1 -- all slack after: pinned left.
  place_axis(0, 2048, 1024, 1280, Align{0, 0, 1}, origin, extent);
  CHECK(origin == 0 && extent == 2048);
  // 1,0,0 -- all slack before: pinned right. The info bar's end cap.
  place_axis(991, 33, 1024, 1280, Align{1, 0, 0}, origin, extent);
  CHECK(origin == 991 + 256 && extent == 33);
  // 0,1,0 -- all slack to the widget: stretched.
  place_axis(0, 1024, 1024, 1280, Align{0, 1, 0}, origin, extent);
  CHECK(origin == 0 && extent == 1280);
  // 1,0,1 -- half each side: centred. The info bar's tab switches.
  place_axis(494, 55, 1024, 1280, Align{1, 0, 1}, origin, extent);
  CHECK(origin == 494 + 128 && extent == 55);
}

TEST(ui_alignment_weights_are_ratios_not_flags) {
  // COMMON/SCRIPTEDIT.INI, which this part does not own. Its list and the
  // scroll bar beside it touch at x = 132 in the design, and stay touching at
  // every width only if 0,2,8 and 2,0,8 really mean two tenths of the slack.
  const Result<Screen> screen = load_screen("commonini/scriptedit.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  CHECK(screen->design == (Rect{0, 0, 320, 200}));

  for (const std::int32_t width : {320, 420, 640, 1000}) {
    const Layout layout = layout_screen(*screen, width, 200);
    const LaidOutWidget* list = layout.find("SList.Back");
    const LaidOutWidget* bar = layout.find("SList.VScrollBack");
    const LaidOutWidget* script = layout.find("ScriptBack");
    const LaidOutWidget* script_bar = layout.find("Script.VScrollBack");
    CHECK(list != nullptr && bar != nullptr && script != nullptr && script_bar != nullptr);
    if (list == nullptr || bar == nullptr || script == nullptr || script_bar == nullptr) continue;
    // The list's right edge and the bar's left edge stay in contact.
    CHECK(list->rect.right() == bar->rect.x);
    // The script pane's right edge and its scroll bar's left edge likewise.
    CHECK(script->rect.right() == script_bar->rect.x);
    // And the script bar keeps its 7-pixel gap to the dialog's right edge.
    CHECK(width - script_bar->rect.right() == 320 - 313);
  }
}

TEST(ui_alignment_weights_distribute_slack_by_magnitude) {
  // Five table headers whose weights sum to five, each widening by a fifth of
  // the slack while the ten-pixel gaps between them stay ten pixels. A reading
  // that treats a weight as a flag turns `1,1,3`, `2,1,2` and `3,1,1` into the
  // same thing and the gaps open and close; only the magnitudes hold them.
  const Result<Screen> screen = load_screen("editorini/advadventure.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  CHECK(screen->design == (Rect{0, 0, 550, 603}));

  // The design positions come from the `#right(previous) + 10#` chain.
  const char* headers[5] = {"HeadersText1", "HeadersText2", "HeadersText3", "HeadersText4",
                            "HeadersText5"};
  const std::int32_t design_x[5] = {20, 120, 180, 260, 340};
  const std::int32_t design_w[5] = {90, 50, 70, 70, 165};
  for (int i = 0; i < 5; ++i) {
    const Widget* widget = screen->find(headers[i]);
    CHECK(widget != nullptr);
    if (widget == nullptr) return;
    CHECK(widget->design.x == design_x[i] && widget->design.width == design_w[i]);
  }

  for (const std::int32_t width : {550, 555, 600, 750, 1050}) {
    const std::int32_t slack = width - 550;
    const Layout layout = layout_screen(*screen, width, 603);
    for (int i = 0; i < 5; ++i) {
      const LaidOutWidget* placed = layout.find(headers[i]);
      CHECK(placed != nullptr);
      if (placed == nullptr) return;
      CHECK(placed->rect.x == design_x[i] + slack * i / 5);
      CHECK(placed->rect.width == design_w[i] + slack / 5);
    }
    // Every gap is still ten pixels wide.
    for (int i = 0; i + 1 < 5; ++i) {
      CHECK(layout.find(headers[i + 1])->rect.x - layout.find(headers[i])->rect.right() == 10);
    }
  }

  // And at a slack the five weights do not divide, where rounding decides. Both
  // edges of a widget are computed from the screen origin rather than one from
  // the other, so the gaps stay exactly ten and the last column still ends where
  // the table does. Accumulating a rounded width per widget opens them by a
  // pixel at a time.
  for (std::int32_t width = 551; width <= 561; ++width) {
    const Layout layout = layout_screen(*screen, width, 603);
    for (int i = 0; i + 1 < 5; ++i) {
      CHECK(layout.find(headers[i + 1])->rect.x - layout.find(headers[i])->rect.right() == 10);
    }
    CHECK(layout.find("HeadersText5")->rect.right() == 505 + (width - 550));
  }
}

TEST(ui_expressions_reach_parameters_that_are_themselves_expressions) {
  // `FullWidth = #%LeftWidth% + %RightWidth% + 9#` is a parameter whose value is
  // an expression; it is substituted into `HeadersBack`'s RectWH and evaluated
  // there, so nothing evaluates it until it has a use.
  const Result<Screen> screen = load_screen("editorini/advadventure.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  const Widget* back = screen->find("HeadersBack");
  CHECK(back != nullptr);
  if (back == nullptr) return;
  CHECK(back->design == (Rect{15, 111, 505, 20}));
  // `Template = %TmplIni%, DisabledShadowFrame` brought `HAlign = 0, 1, 0` in.
  CHECK(back->halign == (Align{0, 1, 0}));
  CHECK(back->attribute("BkColor") == "208, 192, 144");
}

TEST(ui_layout_places_every_widget_of_a_real_screen) {
  const Result<Screen> screen = load_screen("gameini/infobar/infobar_rrome.ini", {}, fixtures);
  CHECK(screen.ok());
  if (!screen.ok()) return;
  const Layout layout = layout_screen(*screen, 1280, 80);
  CHECK(layout.widgets.size() == screen->widgets.size());
  const LaidOutWidget* background = layout.find("Background");
  const LaidOutWidget* cap = layout.find("BackgroundFrame");
  CHECK(background != nullptr && cap != nullptr);
  if (background == nullptr || cap == nullptr) return;
  CHECK(background->rect.x == 0);
  // The cap keeps its distance from the right edge: 1024 - (991 + 33) == 0.
  CHECK(cap->rect.right() == 1280);
}

// -- ImageType --------------------------------------------------------------

TEST(ui_image_type_counts_and_indexes_strip_frames) {
  CHECK(image_type_frames("AAAAA") == 1);
  CHECK(image_type_frames("ABCCC") == 3);
  CHECK(image_type_frames("ABBCD") == 4);
  CHECK(image_type_frames("AAAA") == 1);
  CHECK(image_type_frames("") == 0);

  // ABCCC: resting on frame 0, highlighted on 1, pressed and beyond on 2.
  CHECK(image_type_frame("ABCCC", 0) == 0);
  CHECK(image_type_frame("ABCCC", 1) == 1);
  CHECK(image_type_frame("ABCCC", 2) == 2);
  CHECK(image_type_frame("ABCCC", 4) == 2);
  // ABBCA, the cancel button: pressed shares the highlighted frame and the
  // selected state falls back to the resting one.
  CHECK(image_type_frame("ABBCA", 2) == 1);
  CHECK(image_type_frame("ABBCA", 4) == 0);
  // A state past the end of a short code falls back to the first letter.
  CHECK(image_type_frame("AAAB", 7) == 0);
}

// -- bitmaps ----------------------------------------------------------------

namespace {

/// A 2 x 2 16-bit BMP built field by field, used only to exercise the header
/// walk. The decoder's agreement with the retail art is checked elsewhere, byte
/// for byte against the Python reference reader.
std::string make_bmp_16(std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint16_t d) {
  std::string out(14 + 40 + 16, '\0');
  auto put16 = [&](std::size_t at, std::uint16_t v) {
    out[at] = static_cast<char>(v & 0xFF);
    out[at + 1] = static_cast<char>(v >> 8);
  };
  auto put32 = [&](std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  };
  out[0] = 'B';
  out[1] = 'M';
  put32(2, static_cast<std::uint32_t>(out.size()));
  put32(10, 54);
  put32(14, 40);
  put32(18, 2);
  put32(22, 2);
  put16(26, 1);
  put16(28, 16);
  put32(30, 0);
  // Bottom-up, and the stride of two 16-bit pixels is already four bytes, so
  // there is no padding: the first row in the file is the bottom row of the
  // image and the second, four bytes later, is the top.
  put16(54, c);
  put16(56, d);
  put16(58, a);
  put16(60, b);
  return out;
}

}  // namespace

TEST(ui_bmp_decodes_16_bit_as_x1r5g5b5_bottom_up) {
  // 0x03E0 is the colour the interface art nominates as transparent. Under 555
  // it is pure green; under 565 it would be half green, which is how the two
  // readings are told apart.
  const std::string bytes = make_bmp_16(0x7C00, 0x03E0, 0x001F, 0x7FFF);
  const Result<Image> image =
      decode_bmp({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
  CHECK(image.ok());
  if (!image.ok()) return;
  CHECK(image->width == 2 && image->height == 2);
  const std::uint8_t* top_left = image->pixel(0, 0);
  CHECK(top_left[0] == 255 && top_left[1] == 0 && top_left[2] == 0 && top_left[3] == 255);
  const std::uint8_t* top_right = image->pixel(1, 0);
  CHECK(top_right[0] == 0 && top_right[1] == 255 && top_right[2] == 0);
  const std::uint8_t* bottom_left = image->pixel(0, 1);
  CHECK(bottom_left[0] == 0 && bottom_left[1] == 0 && bottom_left[2] == 255);
  const std::uint8_t* bottom_right = image->pixel(1, 1);
  CHECK(bottom_right[0] == 255 && bottom_right[1] == 255 && bottom_right[2] == 255);
}

TEST(ui_colour_key_clears_alpha_of_every_matching_pixel) {
  const std::string bytes = make_bmp_16(0x03E0, 0x03E0, 0x001F, 0x7FFF);
  Result<Image> image =
      decode_bmp({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
  CHECK(image.ok());
  if (!image.ok()) return;
  apply_color_key(image.value(), 0, 0);
  CHECK(image->pixel(0, 0)[3] == 0);
  CHECK(image->pixel(1, 0)[3] == 0);
  CHECK(image->pixel(0, 1)[3] == 255);
  CHECK(image->pixel(1, 1)[3] == 255);
}

TEST(ui_sub_image_splits_a_strip_and_refuses_a_ragged_one) {
  const std::string bytes = make_bmp_16(0x7C00, 0x03E0, 0x001F, 0x7FFF);
  const Result<Image> image =
      decode_bmp({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
  CHECK(image.ok());
  if (!image.ok()) return;
  const Image right = sub_image(*image, 1, 2, 0, 1);
  CHECK(right.width == 1 && right.height == 2);
  CHECK(right.pixel(0, 0)[1] == 255);  // the green one
  // Three columns do not divide two pixels.
  CHECK(sub_image(*image, 0, 3, 0, 1).empty());
}

// -- painting ---------------------------------------------------------------

TEST(ui_canvas_blends_source_over_exactly) {
  Canvas canvas(2, 1);
  canvas.clear(Color{0, 0, 0, 255});
  Image image;
  image.width = 1;
  image.height = 1;
  image.rgba = {255, 255, 255, 128};
  canvas.blit(image, 0, 0, Rect{0, 0, 2, 1});
  // (255 * 128 + 0 * 127 + 127) / 255 == 128.
  CHECK(canvas.at(0, 0).red == 128);
  CHECK(canvas.at(1, 0).red == 0);
  // Fully transparent source leaves the destination alone.
  image.rgba = {255, 0, 0, 0};
  canvas.blit(image, 1, 0, Rect{0, 0, 2, 1});
  CHECK(canvas.at(1, 0).red == 0);
}

TEST(ui_canvas_clips_to_the_widget_rectangle) {
  Canvas canvas(4, 1);
  canvas.clear(Color{0, 0, 0, 255});
  Image image;
  image.width = 4;
  image.height = 1;
  image.rgba = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
  canvas.blit(image, 0, 0, Rect{1, 0, 2, 1});
  CHECK(canvas.at(0, 0).red == 0);
  CHECK(canvas.at(1, 0).red == 255);
  CHECK(canvas.at(2, 0).red == 255);
  CHECK(canvas.at(3, 0).red == 0);
}

TEST(ui_text_colour_parses_as_the_files_write_it) {
  CHECK(parse_color("255, 255, 255") == (Color{255, 255, 255, 255}));
  CHECK(parse_color("0,0,255") == (Color{0, 0, 255, 255}));
  CHECK(parse_color("", Color{1, 2, 3, 4}) == (Color{1, 2, 3, 4}));
  CHECK(parse_color("255, 300, 0", Color{1, 2, 3, 4}) == (Color{1, 2, 3, 4}));
}

TEST(ui_cp1252_maps_the_high_window_to_unicode) {
  // A translated apostrophe is 0x92 in the file and U+2019 in the font's
  // General Punctuation range. Without the mapping it draws as a hole.
  CHECK(cp1252_to_unicode(0x92) == 0x2019);
  CHECK(cp1252_to_unicode(0x80) == 0x20AC);
  CHECK(cp1252_to_unicode('A') == 'A');
  CHECK(cp1252_to_unicode(0xE9) == 0xE9);
}

// -- the faction table ------------------------------------------------------

TEST(ui_frames_ini_is_an_image_table_not_a_screen) {
  const Result<std::vector<NamedImage>> table =
      load_image_table(kCommandFrames, "Images", fixtures);
  CHECK(table.ok());
  if (!table.ok()) return;
  CHECK(table->size() == 8);
  // File order is preserved, because a table read as a map is a table read
  // wrong -- UNITICONS.INI gives three bitmaps the same label.
  CHECK((*table)[0].name == "Gaul");
  CHECK((*table)[1].name == "RepublicanRome");
  CHECK((*table)[1].image.path == "ui/cmdbar/frames/f2.bmp");
  CHECK((*table)[1].image.has_key);
  CHECK((*table)[1].image.key_x == 10 && (*table)[1].image.key_y == 10);
  // Every faction in the table has an entry, spelled as the faction table
  // spells it.
  for (const FactionBars& faction : faction_bars()) {
    bool found = false;
    for (const NamedImage& entry : *table) {
      if (entry.name == faction.frame_key) found = true;
    }
    CHECK(found);
  }
}

TEST(ui_faction_table_names_all_eight_skins) {
  CHECK(faction_bars().size() == 8);
  const FactionBars* rome = faction_bars_for("RepublicanRome");
  CHECK(rome != nullptr);
  if (rome == nullptr) return;
  CHECK(rome->infobar == "gameini/infobar/infobar_rrome.ini");
  CHECK(rome->cmdbar == "gameini/cmdbar/cmdbarrrome.ini");
  // Gaul is the one whose command bar is the unsuffixed default, which is what
  // gbr.exe's table says and what CONST.INI's LowerDefault repeats.
  const FactionBars* gaul = faction_bars_for("gaul");
  CHECK(gaul != nullptr && gaul->cmdbar == "gameini/cmdbar/cmdbar.ini");
  // Both Romes share one no-selection bar; there are seven empty_*.ini files
  // for eight factions.
  const FactionBars* imperial = faction_bars_for("ImperialRome");
  CHECK(imperial != nullptr && imperial->empty == rome->empty);
  CHECK(faction_bars_for("Atlantis") == nullptr);
}
