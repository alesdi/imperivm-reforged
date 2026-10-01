#pragma once

/// The interface interpreter: HMMSYS `.ini` screens into a widget tree.
///
/// The whole user interface of Imperivm is declared in 150 `.ini` files and
/// none of it is in the executable. This header reads them. It covers the
/// in-game slice — the 24 files under `DATA/INTERFACE/CMDBAR/` and
/// `DATA/INTERFACE/INFOBAR/`, 88 KB, the command bar and the info bar in all
/// eight factions' skins — and the mechanism it implements is the general one,
/// so the menus and the editor need more widget classes rather than a second
/// interpreter. `docs/formats/interface-ini.md` is the specification.
///
/// Four mechanisms have to work before a single rectangle is right, and every
/// one of them was measured rather than guessed:
///
/// ## 1. `%Name%` is a per-screen parameter scope
///
/// 4,329 values across 132 files carry `%Name%` placeholders. They resolve
/// against **one scope per screen**, built from the screen's own
/// `[<Screen> Params]` section:
///
/// ```ini
/// [InfoBar_RRome Params]
/// Template=%TmplIni%, Params          ; inherit DATA/INTERFACE/TEMPLATE.INI's [Params]
/// TmplIni=gameini/template.ini
/// RaceArt=gameres/infobar/RROME
/// ```
///
/// Expansion is textual, recursive and case-insensitive: `MENU/TEMPLATE.INI`
/// defines `ButtonSpacing2=%ButtonSpacing% + %ButtonSpacing%` and
/// `EMPTY_ROME.INI` writes `%dist%` for a key spelled `Dist`. A name with no
/// binding is left standing, placeholder markers and all, so a missing
/// parameter shows up as itself instead of as an empty string.
///
/// ## 2. `%TmplIni%` names a file, and `Template` includes a section from it
///
/// It is not a string substitution at all — that is why it accounts for 2,367
/// of the 4,329 uses on its own. `Template = <ini>, <section>` (or
/// `Template = <section>` for the same file) **merges that section's entries
/// into this one, with the local entries winning**. It appears in two places:
///
/// * on a screen or a widget, where it pulls in an archetype — `[UpperDlg]`,
///   `[StaticText]`, `[SingleLineEdit]`;
/// * inside a `[<Screen> Params]` section, where it pulls in another file's
///   parameters, which is how every screen inherits `Art = gameres` and
///   `InterfaceFont = Fonts/Tahoma14b.apf` from the shared template.
///
/// The `%TmplIni%` inside that line resolves before the include happens, from
/// the *local* entries of the same Params section. That is not an assumption:
/// across all 150 files, **every `Template` line in a Params section names only
/// placeholders that section defines itself**, so nothing else can resolve.
///
/// What the corpus does *not* settle is which entry wins when a Params section
/// and the section it inherits from define the same name -- no shipped file has
/// such a collision. This reader keeps the local one, matching what `Template`
/// demonstrably does on a widget, and `docs/formats/interface-ini.md` records
/// the choice as unproven.
///
/// ## 3. `#expr#` is integer arithmetic over sibling rectangles
///
/// `RectWH = #right(Help)+%dist%#, 1, 51, 51`. Placeholders expand first, then
/// anything between `#` markers is evaluated: `+ - * / % | &` (the operator set
/// is a literal string in `gbr.exe`) over decimal and `0x` literals, with
/// `left top right bottom width height id` reading an already-declared widget.
///
/// ## 4. `HAlign`/`VAlign` are three spring weights, not flags
///
/// `HAlign = a, b, c` splits the difference between the screen's design width
/// and its actual width three ways: `a` to the space before the widget, `b` to
/// the widget itself, `c` to the space after. So `0,0,1` pins left, `1,0,0`
/// pins right, `1,0,1` centres, `0,1,0` stretches — and the weights are not
/// flags, because `COMMON/SCRIPTEDIT.INI` uses `0,2,8` for a list and `2,0,8`
/// for the scrollbar immediately to its right, which keeps them touching at
/// every width only if the split really is 20/80. `layout.hpp` applies it.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::ui {

/// An integer rectangle. The original is a 16-bit software rasteriser and never
/// needed a float; neither does this.
struct Rect {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;

  [[nodiscard]] constexpr std::int32_t right() const noexcept { return x + width; }
  [[nodiscard]] constexpr std::int32_t bottom() const noexcept { return y + height; }
  friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

/// The three spring weights of one axis. The default is the identity: all slack
/// goes to the space after the widget, so a widget with no `HAlign` does not
/// move when the screen grows.
struct Align {
  std::int32_t before = 0;
  std::int32_t self = 0;
  std::int32_t after = 1;

  [[nodiscard]] constexpr std::int32_t total() const noexcept { return before + self + after; }
  friend constexpr bool operator==(const Align&, const Align&) = default;
};

/// The `Style` flags, by name.
///
/// `gbr.exe` keeps one name-to-bit table per widget class, and the same name
/// is a different bit in different classes: `TABSTOP` is `0x400` on a
/// control and `MODAL` is `0x400` on a dialog; `NOFOCUS` is `0x800000` on a
/// list and `0x1000000` on a combobox; `HSCROLL` is `0`, the scrollbar's
/// default, and `TIGHTSCROLL` on a list is two bits at once (`0x280000`).
/// `docs/formats/interface-ini.md` carries the eight tables verbatim. This
/// enum is the union of their names with values of its own, because a bit
/// that means one thing on every class is what a reader wants; the file's
/// bits never leave the executable.
enum class Style : std::uint32_t {
  kNone = 0,
  kTransparent = 1u << 0,  ///< blit through the image's colour key
  kInactive = 1u << 1,     ///< takes no mouse input
  kHidden = 1u << 2,       ///< not drawn until something shows it
  kDisabled = 1u << 3,
  kAlignLeft = 1u << 4,
  kAlignRight = 1u << 5,
  kAlignCenter = 1u << 6,
  // The menus' classes.
  kModal = 1u << 7,        ///< a dialog that takes every click until closed
  kTabStop = 1u << 8,
  kMultiline = 1u << 9,    ///< `TextW`/`EditW`: wraps at `Width`
  kNumber = 1u << 10,      ///< `EditW`: digits only
  kTristate = 1u << 11,    ///< `Button`: three rows, off/on/mixed
  kToggle = 1u << 12,      ///< `Button`: a press flips its row
  kAutosize = 1u << 13,    ///< `Combobox`: the list is as tall as its items
  kNoFocus = 1u << 14,
  kVScroll = 1u << 15,     ///< `Scroll`: vertical (`HSCROLL` is the default, 0)
  kAutoDisable = 1u << 16, ///< `Scroll`: disabled when nothing scrolls
  kAutoMove = 1u << 17,    ///< `Scroll`: the thumb follows the target
  kTextOnly = 1u << 18,    ///< `List`: rows of text, no icons
  kAutoCalc = 1u << 19,    ///< `List`: the row height from the font
  kRows = 1u << 20,        ///< `List`: one item per row
  kSingle = 1u << 21,      ///< `List`: one selection at a time
  kTightScroll = 1u << 22,
  kSecure = 1u << 23,      ///< `EditW`: a password field
  kNoWordWrap = 1u << 24,
  kAutoRepeat = 1u << 25,
  kMultiSel = 1u << 26,
  kAutoHide = 1u << 27,
  kEdit = 1u << 28,        ///< `Combobox`: the text is editable
  kNoList = 1u << 29,
};

[[nodiscard]] constexpr Style operator|(Style a, Style b) noexcept {
  return static_cast<Style>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr Style& operator|=(Style& a, Style b) noexcept { return a = a | b; }
[[nodiscard]] constexpr bool has(Style set, Style flag) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0;
}

/// The widget classes, in-game and menu.
///
/// Every name here is a literal string in `gbr.exe`'s control registries --
/// three tables of `{name, factory, reader, style table}`: the toolkit's own
/// (`Window`, `Control`, `Dialog`, `Button`, `Combobox`, `Frame`,
/// `Background`, `DarkFrame`, `Scroll`, `List`, `TextW`, `EditW`,
/// `ImageButton`, `Spin`/`SpinButton`) and the game's, which re-registers
/// `ImageButton` and adds the bars' classes and `VXMenuBack`,
/// `ConquestMap`, `BrushSize`, `BmpScroll`. `kUnknown` keeps the raw name in
/// `Widget::type` rather than dropping the widget, because a screen that
/// mostly works is a better bug report than a screen that refuses to load.
enum class WidgetType {
  kUnknown,
  kButton,           ///< the toolkit's own button; the bars' backgrounds are these
  kIcon,             ///< a static image
  kThumbnail,        ///< the selected object's portrait
  kSelectionName,    ///< its name
  kSelectionHealth,  ///< its health bar
  kInfobarText,      ///< stat slot `Value`, as text
  kInfobarIcon,      ///< stat slot `Value`, as an icon in a frame
  kBuildingQueue,    ///< the training queue
  kUIHolder,         ///< the units garrisoned in the selection
  kUIInventory,      ///< the items it carries
  kCombiner,         ///< draws `Id1` and `Id2` in one strip
  kHeroSkills,
  kUnitSpecials,
  kSwitch,  ///< the tab buttons at the middle of the info bar
  // The menus' classes. `docs/formats/interface-ini.md`, "The menu widgets".
  kDialog,        ///< a screen's own section, through `StdDlg`
  kControl,       ///< an invisible region: `Move`, the size grips
  kBackground,    ///< `BkColor` fill with a `FrameColor1..4` bevel
  kDarkFrame,     ///< halves every pixel under it (0x00663c70: `>> 1 & 0x7f7f7f`)
  kFrame,         ///< nine-slice of `Image` cut at `Dividers`
  kImageButton,   ///< a `Button` with a text label over an `XFrames` x `YFrames` grid
  kTextW,         ///< static text
  kEditW,         ///< editable text
  kList,          ///< rows of text
  kScroll,        ///< a scrollbar's thumb
  kCombobox,      ///< a drop-down list
  kPlayerCombobox,  ///< the same, filled with the players' names
  kTextEx,        ///< rich text, the game's own
  kVXMenuBack,    ///< a `Frame` with a title between two bullets
  kSpin,          ///< up/down buttons beside a number edit
  kActiveButton,  ///< a `Button` that reports hover
  kBmpScroll,     ///< the editor's bitmap scroller
  kBrushSize,     ///< the editor's brush picker
  kConquestMap,   ///< the campaign map
  kSelectionHealthText,
};

[[nodiscard]] WidgetType widget_type_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view widget_type_name(WidgetType type) noexcept;

/// A reference to a bitmap, with the colour-key probe that follows it.
///
/// `Image = gameres/infobar/RROME/small frame.bmp, 10, 10` — the two numbers
/// are **the coordinates of a pixel whose colour is the transparent one**, not
/// an offset. Probed across the info bar art they return pure green (RGB565
/// `0x03E0`, RGB888 `0,255,0`) every time, and in each of those images green is
/// the single most common colour. A negative coordinate means no key.
struct ImageRef {
  std::string path;  ///< alias-resolved, `/`-separated
  bool has_key = false;
  std::int32_t key_x = 0;
  std::int32_t key_y = 0;

  [[nodiscard]] bool empty() const noexcept { return path.empty(); }
};

/// A visibility tag: one of the bare lines in a widget's section.
///
/// `unit`, `!holder`, `thumb`, `empty`, `building`, `hero`, `items`, `queue` —
/// the same eight words appear in `gbr.exe` with a `!` already attached
/// (`!thumb`, `!empty`, `!building`, `!unit`, `!hero`, `!holder`, `!items`,
/// `!queue`), which is what identifies the bare lines as this and not as
/// nameless keys.
enum class SelectionTag : std::uint32_t {
  kNone = 0,
  kThumb = 1u << 0,
  kEmpty = 1u << 1,
  kBuilding = 1u << 2,
  kUnit = 1u << 3,
  kHero = 1u << 4,
  kHolder = 1u << 5,
  kItems = 1u << 6,
  kQueue = 1u << 7,
};

[[nodiscard]] constexpr SelectionTag operator|(SelectionTag a, SelectionTag b) noexcept {
  return static_cast<SelectionTag>(static_cast<std::uint32_t>(a) |
                                   static_cast<std::uint32_t>(b));
}
constexpr SelectionTag& operator|=(SelectionTag& a, SelectionTag b) noexcept {
  return a = a | b;
}
[[nodiscard]] constexpr bool any(SelectionTag set, SelectionTag mask) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(mask)) != 0;
}
[[nodiscard]] SelectionTag selection_tag_from_name(std::string_view name) noexcept;

/// Which owners of the selected object a widget shows for, from the
/// `Show...= yes` keys. The engine decides the relationship; this records what
/// the file asked for.
enum class ShowFor : std::uint32_t {
  kNone = 0,
  kAll = 1u << 0,
  kControl = 1u << 1,
  kVision = 1u << 2,
  kCover = 1u << 3,
  kSupport = 1u << 4,
  kCeaseFire = 1u << 5,
};

[[nodiscard]] constexpr ShowFor operator|(ShowFor a, ShowFor b) noexcept {
  return static_cast<ShowFor>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr ShowFor& operator|=(ShowFor& a, ShowFor b) noexcept { return a = a | b; }

/// One resolved key/value of a widget section, placeholders expanded and
/// `#expr#` evaluated. Kept in file order and in full: the interpreter models
/// the keys it draws with and the app can read the rest without a change here.
struct Attribute {
  std::string key;
  std::string value;
};

/// One widget.
struct Widget {
  std::string name;  ///< the section name, which is also what `left(...)` names
  std::string type;  ///< `Type =`, verbatim
  WidgetType kind = WidgetType::kUnknown;

  /// The rectangle as authored, before any spring is applied.
  Rect design;
  /// Whether any section on the way authored one at all: a bullet with no
  /// `RectWH` stands beside its caption, a button with `RectWH = x, y, 0,
  /// 0` takes its bitmap's cell (`Dialog::fit_to_images`).
  bool has_rect = false;
  /// True when the source key was `Rect` (left, top, right, bottom) rather than
  /// `RectWH` (left, top, width, height). Both are stored here as x/y/w/h.
  bool from_ltrb = false;

  Align halign;
  Align valign;

  Style style = Style::kNone;
  std::vector<std::string> unknown_styles;

  ImageRef image;
  ImageRef frame;
  /// The five-letter `ImageType` code, e.g. `ABCCC`. Letter *i* selects the
  /// sub-image that control state *i* draws, and the bitmap is a horizontal
  /// strip of `max(letter) - 'A' + 1` equal frames. Measured: across the whole
  /// corpus the bitmap's width is an exact multiple of that frame count in
  /// **139 of 139** multi-frame sections, and equals `frame count * rect width`
  /// in 90 of them; a vertical-strip reading scores 117 of 139 on the same
  /// test.
  std::string image_type;
  /// `Rows`, which divides the same bitmap vertically. A `Switch`'s two states
  /// are two rows of one 55 x 156 bitmap.
  std::int32_t rows = 1;
  std::int32_t initial_row = 0;

  /// Positive tags are OR'd, negated tags are all required to be absent. A
  /// widget with no positive tag is visible for every selection.
  SelectionTag require_any = SelectionTag::kNone;
  SelectionTag forbid = SelectionTag::kNone;
  ShowFor show_for = ShowFor::kNone;
  /// `TabMask`, a bit per tab index; 0 means every tab.
  std::uint32_t tab_mask = 0;
  /// The bare `ReverseDraw` line: a strip fills from its right edge. The two
  /// `UIInventory` sections carry it, so a hero's items sit against the army
  /// they share the row with.
  bool reverse_draw = false;

  /// `Dividers = left, middle, top, middle`: the nine-slice of a `Frame`.
  /// Read from `gbr.exe` (0x00663570): the first pair is the width of the
  /// left piece and of the middle one, the right piece being what is left of
  /// the bitmap; the second pair is the same for the top and the middle
  /// rows. `FRAME.BMP` is 241 wide with `73, 95`, which leaves 73 on the
  /// right -- symmetric, as a frame is -- and `LINE_FRAME.BMP` is 39 tall
  /// with `19, 1`, a one-pixel band. A middle clamped so that `left +
  /// middle` never passes the bitmap's edge is the executable's own clamp.
  bool has_dividers = false;
  std::int32_t divider_left = 0;
  std::int32_t divider_middle = 0;
  std::int32_t divider_top = 0;
  std::int32_t divider_middle_height = 0;
  /// `XFrames`/`YFrames`: an `ImageButton`'s bitmap is a grid of this many
  /// columns and rows, each cell one control state (by `ImageType`) and one
  /// row (by `InitialRow`). `BUTTON.BMP` is 948 x 76 with `4, 2`: 237 x 38.
  std::int32_t xframes = 0;
  std::int32_t yframes = 0;

  /// `Id`, the command the widget posts. `0x1008` is Help, `0x7003` is Cancel.
  std::int32_t id = 0;
  bool has_id = false;

  /// Display strings, still untranslated: every one of them is a key in
  /// `CURRENTLANG\TRANSLATION.LOC.XML`. Checked for all sixteen that the 24
  /// in-game files carry — `Help (F1)`, `Cancel Command`, `Hero skills` and the
  /// rest are all present.
  std::string help_text;
  std::string rollover;
  std::string text;
  /// `Value`, which is a stat-slot index for `InfobarText`/`InfobarIcon` and a
  /// named quantity (`Population`, `Gold`) elsewhere.
  std::string value;

  std::vector<Attribute> attributes;

  [[nodiscard]] std::string_view attribute(std::string_view key,
                                           std::string_view fallback = {}) const noexcept;
  [[nodiscard]] bool attribute_int(std::string_view key, std::int32_t& out) const noexcept;
  /// A `path, key_x, key_y` attribute, alias-resolved.
  [[nodiscard]] ImageRef attribute_image(std::string_view key) const;
  /// Whether this widget is drawn for a selection carrying `tags` on `tab`.
  [[nodiscard]] bool visible_for(SelectionTag tags, std::uint32_t tab = 0) const noexcept;
};

/// One screen: the dialog section, its `[<name> Objects]` list and its widgets.
struct Screen {
  std::string name;
  /// The alias-resolved path the screen was loaded from
  /// (`data/interface/menu/gamemenu.ini`); the translation table keys a
  /// widget's text by it.
  std::string path;
  /// The dialog's own rectangle, which is the design size the springs measure
  /// slack against.
  Rect design;
  Rect min_size{0, 0, 0, 0};
  Rect max_size{0, 0, 0, 0};
  Style style = Style::kNone;
  /// `Esc = <widget>` and `Enter = <widget>`: the button those keys press.
  /// 55 of the files declare one. Two name a widget the file does not have
  /// (`SETTINGS.INI`'s `Esc = Cancel`, `LOADGAME.INI`'s `Enter = SaveBtn`,
  /// copied from `SAVEGAME.INI`), and those press nothing.
  std::string escape;
  std::string enter;
  /// `Focus = <widget>`: which widget has the keyboard when the dialog opens.
  std::string focus;
  std::vector<Widget> widgets;
  /// Names listed in `[<name> Objects]` for which no section exists. The
  /// shipped files have none; a mod would.
  std::vector<std::string> missing;
  /// Everything the interpreter did not understand, for a caller that wants to
  /// know rather than to guess.
  std::vector<std::string> warnings;

  [[nodiscard]] const Widget* find(std::string_view widget) const noexcept;
};

/// The translation context of a widget's text: `/Menu/gamemenu.ini:Quit:Text`
/// for `attribute` `Text` of widget `Quit` on the screen at
/// `data/interface/menu/gamemenu.ini` -- the path under `DATA/INTERFACE`,
/// the section, the key. The shipped table's keys are `<text>@<context>`.
[[nodiscard]] std::string translation_context(std::string_view screen_path,
                                              std::string_view widget, std::string_view attribute);

/// Supplies the bytes of an `.ini` by virtual path. The core does no I/O, and a
/// screen may include another file, so this is how `Template = <ini>, <section>`
/// reaches the disk without the core knowing there is one. Return an empty span
/// for a path that does not resolve.
using FileProvider = std::function<std::span<const std::byte>(std::string_view path)>;

/// Rewrite a leading virtual-root alias.
///
/// `gameres/` is `UI/`, `gameini/` is `DATA/INTERFACE/`, and the four others
/// follow. Every one is pinned by the same file being named both ways somewhere
/// in the retail data: `CONST.INI` writes `data/interface/cmdBar/cmdbar.ini`
/// where `gbr.exe` writes `gameini/cmdbar/cmdbar.ini`, and `gbr.exe` names
/// `ui/menu/profileicons/unknown.bmp` and `menures/ShieldArrow.bmp` for files
/// in one directory. Anything without a known alias is returned unchanged.
[[nodiscard]] std::string resolve_alias(std::string_view path);

/// A `%Name%` scope. Lookup is case-insensitive; entries keep insertion order.
class ParamScope {
 public:
  void set(std::string_view name, std::string_view value);
  /// Only if absent, which is what an inherited `[Params]` section gets.
  void set_default(std::string_view name, std::string_view value);
  [[nodiscard]] const std::string* find(std::string_view name) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  /// Expand every `%Name%` in `text`, recursively. An unbound name is left as
  /// written, `%%` is a literal `%`, and recursion stops at `kMaxDepth`.
  [[nodiscard]] std::string expand(std::string_view text) const;

  static constexpr int kMaxDepth = 16;

 private:
  struct Entry {
    std::string key;  ///< folded to lower case
    std::string value;
  };
  std::vector<Entry> entries_;
};

/// Evaluate every `#...#` span in `text`, leaving the rest alone.
///
/// `lookup` answers `left(Name)`, `top(Name)`, ..., `id(Name)`; it returns
/// false for a name it does not know, which leaves the whole `#...#` span as
/// written so the failure is visible in the output rather than silently zero.
using RectLookup = std::function<bool(std::string_view function, std::string_view widget,
                                      std::int32_t& out)>;
[[nodiscard]] std::string evaluate_expressions(std::string_view text, const RectLookup& lookup);

/// Evaluate one arithmetic expression. `false` on a syntax error or an unknown
/// function argument.
[[nodiscard]] bool evaluate_expression(std::string_view text, const RectLookup& lookup,
                                       std::int32_t& out);

/// Load one screen.
///
/// `path` is a virtual path, alias or not. `screen` names the dialog section;
/// empty takes the file's first section, which is what every in-game file's
/// screen is. `provider` supplies this file and any file a `Template` names.
[[nodiscard]] Result<Screen> load_screen(std::string_view path, std::string_view screen,
                                         const FileProvider& provider);

/// The eight factions, in the order `gbr.exe`'s table lists their two files.
struct FactionBars {
  std::string_view faction;   ///< as the class data spells it
  std::string_view infobar;   ///< `gameini/infobar/...`
  std::string_view cmdbar;    ///< `gameini/cmdbar/...`
  std::string_view empty;     ///< `gameini/cmdbar/empty_...`, the no-selection bar
  std::string_view race_art;  ///< the `%RaceArt%` directory the skin uses
  std::string_view frame_key;  ///< this faction's key in `CMDBAR/FRAMES.INI`
};

/// `gameini/cmdbar/frames.ini`, the command-button frame table.
inline constexpr std::string_view kCommandFrames = "gameini/cmdbar/frames.ini";

/// One entry of a name-to-bitmap table, such as `CMDBAR/FRAMES.INI`.
struct NamedImage {
  std::string name;
  ImageRef image;
};

/// Read a section that is a flat list of `Name = path, key_x, key_y`.
///
/// The twenty-fourth in-game file, `CMDBAR/FRAMES.INI`, is not a screen: it is
/// one `[Images]` section mapping a faction to the frame its command buttons
/// are drawn in, which the engine composites at run time around an icon it
/// picks per order (`gameres/cmdbar/` and `gameres/cmdbar/_dummy_icon_.bmp` are
/// literals in `gbr.exe`, and `CONST.INI` caps the row at
/// `[UIBars] CmdBarMaxButtons = 16`). Entries come back in file order.
[[nodiscard]] Result<std::vector<NamedImage>> load_image_table(std::string_view path,
                                                               std::string_view section,
                                                               const FileProvider& provider);

/// The faction table, verbatim from the adjacent string block in `gbr.exe`
/// (`gameini/infobar/infobar_german.ini`, `gameini/cmdbar/cmdbarGerman.ini`,
/// ... down to `gameini/infobar/infobar_gaul.ini`, `gameini/cmdbar/cmdbar.ini`
/// — Gaul is the one whose command bar is the unsuffixed default).
[[nodiscard]] std::span<const FactionBars> faction_bars() noexcept;
/// The entry for a faction name, case-insensitively, or null.
[[nodiscard]] const FactionBars* faction_bars_for(std::string_view faction) noexcept;

}  // namespace imperivm::core::ui
