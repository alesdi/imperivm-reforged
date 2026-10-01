#pragma once

/// Compositing a laid-out screen into pixels.
///
/// This is a **software** compositor writing RGBA8 into a buffer, and that is a
/// deliberate choice rather than a shortcut. The original is a 16-bit software
/// rasteriser and drew its bars exactly this way; the bars are 1024 x 80 and
/// 1024 x 54, together under 110,000 pixels, and they change only when the
/// selection does. Putting them on the GPU would mean a second pipeline for
/// true-colour quads next to the index-atlas one the sprites use, and a shader
/// pair to maintain, to save a redraw that costs less than a millisecond and
/// does not happen every frame.
///
/// It also keeps the whole interface inside the core, where it is testable with
/// no window: `engine/tests/test_ui.cpp` composites the real Republican Roman
/// info bar and checks pixels. The platform's only job is to upload the
/// finished buffer and blit it — see `imperivm/platform/ui_renderer.hpp`.
///
/// Nothing here uses a float. Alpha blending is `(src * a + dst * (255 - a) +
/// 127) / 255`, which is exact, monotonic and identical on every target.

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/ui/font.hpp"
#include "imperivm/core/ui/image.hpp"
#include "imperivm/core/ui/interface.hpp"
#include "imperivm/core/ui/layout.hpp"

namespace imperivm::core::ui {

struct Color {
  std::uint8_t red = 255;
  std::uint8_t green = 255;
  std::uint8_t blue = 255;
  std::uint8_t alpha = 255;

  friend constexpr bool operator==(const Color&, const Color&) = default;
};

/// Parse `r, g, b` as a `TextColor` value writes it. Missing or malformed
/// leaves `fallback` alone.
[[nodiscard]] Color parse_color(std::string_view text, Color fallback = Color{});

/// An RGBA8 surface, top row first.
class Canvas {
 public:
  Canvas() = default;
  Canvas(std::uint32_t width, std::uint32_t height) { resize(width, height); }

  void resize(std::uint32_t width, std::uint32_t height);
  void clear(Color color = Color{0, 0, 0, 0});

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept { return rgba_; }
  [[nodiscard]] Color at(std::uint32_t x, std::uint32_t y) const noexcept;

  /// Source-over blit of `image`'s top-left corner at `(x, y)`, clipped to
  /// `clip` intersected with the canvas.
  void blit(const Image& image, std::int32_t x, std::int32_t y, const Rect& clip);
  /// The same, tiled horizontally to fill `clip` — which is what a 2048-wide
  /// background does not need but a mod at 2560 would.
  void fill_rect(const Rect& rect, Color color);

  /// Draw `text` (cp1252) with its line box's top-left at `(x, y)`, in `ink`.
  /// Returns the pen advance.
  std::int32_t draw_text(const Font& font, std::string_view text, std::int32_t x,
                         std::int32_t y, Color ink, const Rect& clip);
  /// Source-over blit of the `source` rectangle of `image` with its top-left
  /// at `(x, y)`, clipped to `clip`. A nine-slice is nine of these.
  void blit_region(const Image& image, const Rect& source, std::int32_t x, std::int32_t y,
                   const Rect& clip);
  /// `blit_region`, repeated across `target` from its top-left corner.
  void tile_region(const Image& image, const Rect& source, const Rect& target,
                   const Rect& clip);
  /// Source-over blit of the whole of `image` resampled (nearest) to fill
  /// `target`, clipped to `clip`.
  void blit_scaled(const Image& image, const Rect& target, const Rect& clip);

 private:
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::vector<std::uint8_t> rgba_;
};

/// Decoded art and fonts, keyed by the path and colour key that produced them.
///
/// The cache exists because one screen names the same bitmap from several
/// widgets — every `InfobarIcon` in a skin shares one `small frame.bmp` — and
/// because a redraw must not re-decode anything.
class ResourceCache {
 public:
  explicit ResourceCache(FileProvider provider) : provider_(std::move(provider)) {}

  /// The bitmap at `path`, with the colour key at `(key_x, key_y)` made
  /// transparent when `keyed`. Null if it does not resolve or does not decode.
  [[nodiscard]] const Image* image(const ImageRef& ref);
  /// One cell of a strip: `frames` columns and `rows` rows.
  [[nodiscard]] const Image* image_frame(const ImageRef& ref, std::uint32_t column,
                                         std::uint32_t columns, std::uint32_t row,
                                         std::uint32_t rows);
  [[nodiscard]] const Font* font(std::string_view path);

  /// Paths that failed to load, once each, so a caller can report them rather
  /// than discover a blank bar.
  [[nodiscard]] const std::vector<std::string>& missing() const noexcept { return missing_; }

 private:
  struct ImageEntry {
    std::string key;
    Image image;
    bool ok = false;
  };
  struct FontEntry {
    std::string path;
    Font font;
    bool ok = false;
  };

  FileProvider provider_;
  /// Deques, not vectors: `image` and `font` hand out pointers into these,
  /// and a caller that holds one while loading another -- every strip holds
  /// four frames and a ramp -- must not have it invalidated by the growth.
  /// A vector did exactly that, and the first strip drawn found its frames
  /// pointing at freed memory.
  std::deque<ImageEntry> images_;
  std::deque<FontEntry> fonts_;
  std::vector<std::string> missing_;
};

/// One entry of the info bar's six stat slots, or of a queue.
struct Slot {
  std::string text;   ///< already translated
  std::string icon;   ///< a virtual path, or empty
  bool present = false;
};

/// One cell of an icon strip: what `BuildingQueue`, `UIHolder`,
/// `UIInventory`, `HeroSkills` and `UnitSpecials` draw a row of.
///
/// The shape mirrors `sim::InfoCell` field for field and is repeated here
/// rather than included, because this layer draws paths and numbers and must
/// not learn what a garrison or an item is.
struct StripCell {
  std::string icon;    ///< a virtual path, or empty
  std::string number;  ///< text at `NumberYPosition` / `IconTextOffset`
  std::int32_t health = -1;  ///< 0..100 for a bar at `HealthBarYPosition`, -1 none
  enum class Frame : std::uint8_t { kNormal, kSelected, kTrain, kWait } frame = Frame::kNormal;
  std::string text;  ///< `UnitSpecials`: the label beside the icon
  bool glow = false;  ///< `UIInventory`: `BackGlowImage` rather than `BackImage`
  bool plus = false;  ///< `HeroSkills`: the `PlusSign`
};

/// One command button, as the bar draws it. Mirrors `sim::CommandButton` in
/// the two fields this layer needs.
struct CommandButtonView {
  std::string name;
  std::string icon;  ///< a three-frame strip: normal, highlighted, pressed
  bool enabled = true;
};

/// Everything about the current selection that the files cannot know.
///
/// The interpreter draws the chrome from the data; this is the hole the running
/// game fills. A field left at its default means "nothing to draw", and the
/// widget that would have shown it is skipped rather than drawn empty.
struct BarContent {
  SelectionTag tags = SelectionTag::kEmpty;
  std::uint32_t tab = 0;
  /// Whether a `Switch` that declares a `BlinkTime` is in the lit half of
  /// its blink: it draws its second row rather than its `InitialRow`. The
  /// original's switch handler (0x006c0f30) turns the blink on when the
  /// selection's head is a hero the local player controls with skill points
  /// to spend (0x0052da80 is `AvailableSkillPoints`), and its timer message
  /// flips `1 - row` every `BlinkTime` milliseconds while it is on
  /// (0x006c1019). Which event 0x1705000x is -- the selection changing, by
  /// its callers -- is the reading; the phase is the embedder's clock.
  bool blink = false;

  std::string name;              ///< the selection's name, translated
  std::vector<Slot> values;      ///< indexed by a widget's `Value`
  std::int32_t health = -1;      ///< 0..100, negative for "no health bar"
  const Image* thumbnail = nullptr;
  /// The portrait by virtual path -- the class `icon` -- used when
  /// `thumbnail` is null. The cache decodes it once.
  std::string thumbnail_path;

  /// The strips, each in its drawing order. A widget whose strip is empty
  /// draws nothing, which is what the original shows.
  std::vector<StripCell> queue;
  std::vector<StripCell> holder;
  std::vector<StripCell> items;
  std::vector<StripCell> skills;
  std::vector<StripCell> specials;

  /// Which widget the pointer is over and which is held, by name. These pick
  /// the `ImageType` letter a button draws.
  std::string hovered;
  std::string pressed;

  /// The command buttons, in order, and the faction's frame for them
  /// (`CMDBAR/FRAMES.INI`). Drawn by `paint_command_buttons`, not by any
  /// widget: the row is the engine's, not the file's.
  std::vector<CommandButtonView> buttons;
  ImageRef button_frame;

  /// Translation, injected so that the core's UI does not reach for a global.
  /// The identity is a perfectly good default for a headless test.
  std::function<std::string_view(std::string_view)> translate;

  [[nodiscard]] std::string_view text_of(std::string_view key) const {
    return translate ? translate(key) : key;
  }
};

/// What a menu screen's widgets show that the file cannot know: a caption
/// set at run time, a list's rows, an edit's text, which button is held and
/// which widget has the keyboard. Keyed by widget name. A widget with no
/// entry shows what its section declares.
struct WidgetState {
  std::string name;
  bool has_text = false;
  std::string text;  ///< the caption, the edit's contents, the combobox's line
  /// A colour to fill a `Background` with in place of its `BkColor`: the
  /// setup screen's `Color_P<n>` squares are `ShadowFrame`s the game paints
  /// with each player's colour.
  bool has_color = false;
  Color color;
  /// A text colour set from the code in place of the widget's `TextColor`:
  /// the statistics screen sets a column's leader in yellow (0x006fab1c
  /// picks from a two-entry table on *value equals the maximum*).
  bool has_ink = false;
  Color ink;
  /// A multiline text's vertical scroll in pixels: the block is drawn that
  /// much higher and clipped to its rectangle. The credits roll
  /// (`CREDITSMENU.INI`'s `Credits`, `PixPerSec` of it a second).
  std::int32_t text_scroll = 0;
  /// A picture composed by the code, drawn at the widget's top-left and
  /// clipped to it: the campaign map's viewport, which the `ConquestMap`
  /// widget shows and nothing in the file names.
  const Image* bitmap = nullptr;
  /// A bitmap to draw in place of the widget's own `Image`, by virtual
  /// path: the adventure menus' `Caption` shows the chosen adventure's
  /// picture, which no file names. **Reading, labelled:** the pictures are
  /// 440 x 360 and the caption is 700 x 300, and how the original fits the
  /// one in the other is not read; this scales the picture to fit the
  /// rectangle, keeping its shape, and centres it.
  std::string image;
  bool disabled = false;
  /// Visibility set from outside, overriding the section's `HIDDEN`: the
  /// front's load dialog declares its Load and Cancel hidden and the code
  /// that opens it shows them.
  bool has_hidden = false;
  bool hidden = false;
  /// `Rows`/`YFrames` row to draw, or -1 for the widget's `InitialRow`: a
  /// checked `RadioBtn` is row 1 of its two.
  std::int32_t row = -1;
  /// A `List` or `Combobox`. An item with line breaks in it (`\n`, or the
  /// two characters `\n` as the note texts carry them) is as many rows
  /// tall as it has lines; the notes list shows a title over its text.
  std::vector<std::string> items;
  /// One icon per item, a virtual path or empty, drawn at the list's
  /// `IconOffs` with the text at `TextOffs`. Empty for no icons.
  std::vector<std::string> icons;
  /// One per item, or empty: `kItemLarge` sets it in the list's `BoldFont`,
  /// `kItemCentred` centres its lines. The help's headings.
  std::vector<std::uint8_t> item_flags;
  /// One per item, or empty: the datum the toolkit keeps beside a row
  /// (`CUIList`'s item data, which 0x00463080 reads back as a string) --
  /// the editor's icon combo lists names and carries the bitmap paths.
  std::vector<std::string> items_data;
  static constexpr std::uint8_t kItemLarge = 1;
  static constexpr std::uint8_t kItemCentred = 2;
  std::int32_t selected = -1;      ///< the item drawn selected, or -1
  std::int32_t scroll = 0;         ///< the first row a `List` shows
  bool open = false;               ///< a `Combobox` with its list dropped
  /// A `Scroll` with no `TargetId` is a slider: its thumb stands at this
  /// fraction of the run, 0..100. The options dialog's volumes and speeds.
  std::int32_t value = 0;
  std::int32_t caret = -1;         ///< an `EditW`'s caret, or -1 for the end
};

struct DialogContent {
  std::vector<WidgetState> states;
  /// A picture under every widget, at the canvas's origin: the zoom map,
  /// whose screen (`ZOOMMAP.INI`) declares no widget at all and whose
  /// contents the code composes.
  const Image* backdrop = nullptr;
  std::string hovered;
  std::string pressed;
  std::string focused;
  /// Whether the caret is in its visible half of the blink.
  bool caret_on = true;
  std::function<std::string_view(std::string_view)> translate;
  /// Translation of a text the screen declares, given the widget and the
  /// key it came from (`Text`, `HelpText`): the shipped table keys those by
  /// where they stand. Falls back to `translate`.
  std::function<std::string_view(std::string_view text, std::string_view widget,
                                 std::string_view attribute)>
      translate_widget;

  /// The font a widget that names none draws its rows in: the editor's
  /// `Browser` is a bare `Control`, and its rows are this engine's. The
  /// first `Font` any widget of the screen names, which is the template's
  /// `%FontPath%` in every shipped file.
  std::string default_font;
  /// The ink a widget that names no `FontColor`/`TextColor` draws in. White
  /// for the game's menus, whose dark frames every shipped file relies on;
  /// **black for the editor's** (`Dialog` sets it by the screen's path),
  /// whose parchment `MsgBack`/`ShadowFrame` panes name no colour and whose
  /// captions say `TextColor = 0, 0, 0` where they say anything. Reading,
  /// labelled: the exe's default for a Win32-styled control was not read.
  Color default_ink{255, 255, 255, 255};

  [[nodiscard]] const WidgetState* state_of(std::string_view name) const noexcept;
  [[nodiscard]] WidgetState& state(std::string_view name);
  [[nodiscard]] std::string_view text_of(std::string_view key) const {
    return translate ? translate(key) : key;
  }
  [[nodiscard]] std::string_view text_of(std::string_view key, std::string_view widget,
                                         std::string_view attribute) const {
    if (translate_widget) return translate_widget(key, widget, attribute);
    return text_of(key);
  }
};

/// Whether `widget` draws and answers as a `List`: a `List`, or a `Control`
/// the code has filled with items. The editor's `Browser` (`MAPTOOLSDLG.INI`)
/// is a `Control` the original attaches a native tree to (0x004a2a80 builds
/// it); here the tree is flattened into rows the code keeps, one per visible
/// node, and the control is a list of them. **Reading, labelled.**
[[nodiscard]] bool acts_as_list(const Widget& widget, const WidgetState* state) noexcept;

/// The brush pictures a `BrushSize` offers, as 1-based columns of its
/// `Image`: the digits of `Frames` in order (`23456` for the terrain and
/// height tools, `13456` for the decor tools -- column 1 is the single
/// stamp, 2..6 the dots), or every column of the strip when the widget names
/// none. `BRUSHES.BMP` is 252 x 84 at `ItemWidth`/`ItemHeight` 42: six
/// columns, two rows, the second row the chosen look. **Reading, labelled:**
/// the second row as the selected state is this engine's, read off the art.
[[nodiscard]] std::vector<std::int32_t> brush_frames(const Widget& widget, std::uint32_t columns);
/// A `BrushSize` widget's item size, `ItemWidth` x `ItemHeight`, 42 when unset.
[[nodiscard]] std::int32_t brush_item_width(const Widget& widget) noexcept;
[[nodiscard]] std::int32_t brush_item_height(const Widget& widget) noexcept;

/// Composite a menu screen. Every widget kind the 48 menu files and the
/// editor's 52 instantiate is drawn or deliberately skipped -- `Control`,
/// `Dialog`, `Spin` draw nothing -- and `docs/formats/interface-ini.md`,
/// "The menu widgets", says which readings each one rests on.
void paint_dialog(Canvas& canvas, const Layout& layout, ResourceCache& cache,
                  const DialogContent& content);

/// The height of one line of a `List`, from its font: `AUTOCALC`.
[[nodiscard]] std::int32_t list_row_height(ResourceCache& cache, const Widget& widget);
/// The height a multiline block of `text` takes when wrapped at `width`, as
/// `draw_text_block` lays it: its lines times the font's height. What a roll
/// of the block needs to know to see its end.
[[nodiscard]] std::int32_t text_block_height(const Font& font, std::string_view text, std::int32_t width);
/// The height of item `index` of the list: its lines -- wrapped at `width`
/// unless the list is `NOWORDWRAP` -- or its icon, whichever is taller.
[[nodiscard]] std::int32_t list_item_height(ResourceCache& cache, const Widget& widget,
                                            const WidgetState& state, std::size_t index,
                                            std::int32_t width);
/// How many items of `widget` fit in `rect` from `state.scroll` on; with no
/// state, one-line items.
[[nodiscard]] std::int32_t list_visible_rows(ResourceCache& cache, const Widget& widget,
                                             const Rect& rect, const WidgetState* state = nullptr);
/// The item under `y` (canvas coordinates) in a list at `rect`, or -1.
[[nodiscard]] std::int32_t list_item_at(ResourceCache& cache, const Widget& widget,
                                        const Rect& rect, const WidgetState& state, std::int32_t y);
/// The height of a `Combobox`'s closed box: its arrow's, plus a pixel each
/// side. The rectangle the file gives it is the dropped-down one.
[[nodiscard]] std::int32_t combobox_closed_height(ResourceCache& cache, const Widget& widget);
/// Where a `LeftBullet`/`RightBullet` with no rectangle of its own stands:
/// beside the screen's caption (`Id = %ID_CAPTION%`), `BulletTextDist` from
/// its text. Empty when there is no caption to flank.
[[nodiscard]] Rect bullet_rect(ResourceCache& cache, const Layout& layout,
                               const DialogContent& content, const Widget& bullet);

/// The control states an `ImageType` code indexes, in the order its letters
/// appear. Five letters, five states; the four-letter codes in the corpus
/// belong to classes with one state fewer and simply have no `kSelected`.
enum class ControlState : std::uint32_t {
  kNormal = 0,
  kHighlighted = 1,
  kPressed = 2,
  kDisabled = 3,
  kSelected = 4,
};

/// Composite `layout` onto `canvas`.
///
/// Widgets are drawn in the order `[<Screen> Objects]` lists them, which in all
/// 24 in-game files puts `Background` first and the dividers and overlays after
/// the things they sit on. Any widget whose tags, tab mask or `HIDDEN` style
/// exclude it is skipped.
void paint_screen(Canvas& canvas, const Layout& layout, ResourceCache& cache,
                  const BarContent& content);

// -- the strips' cells ------------------------------------------------------

/// A cell of an icon strip: the strip whose cell it is (`UIHolder`,
/// `BuildingQueue`, `UIInventory` -- through a `Combiner` too, which draws
/// two of them in one row) and its index into that strip's `BarContent`
/// list. `strip` null for none.
struct StripCellHit {
  const Widget* strip = nullptr;
  std::int32_t index = -1;
};
/// The strip cell under a point of `layout`'s space, among the strips
/// `paint_screen` would draw for `content` -- the same rectangles.
[[nodiscard]] StripCellHit strip_cell_at(const Layout& layout, const BarContent& content,
                                         std::int32_t x, std::int32_t y);
/// Where cell `index` of the strip named `strip` is drawn, or an empty
/// rectangle when it is not.
[[nodiscard]] Rect strip_cell_rect(const Layout& layout, const BarContent& content,
                                   std::string_view strip, std::size_t index);

// -- the command buttons -----------------------------------------------------
//
// `gbr.exe` builds up to `CmdBarMaxButtons` buttons at run time, each the
// faction's frame around an icon chosen per row, and neither the file nor
// the executable's data says where the row stands. **Reading, labelled:**
// the row is centred in the bar, the buttons abutting -- the no-selection
// menu is a 612-wide dialog the bar centres (0x006b3e00 computes
// `(parent + 1 - child + 1) / 2` for a child), and this puts the two rows
// in the same place. The frame strip is three states wide -- normal,
// highlighted, pressed -- and so is every `button=` icon (153 x 51 over the
// whole `UI/CmdBar/Actions` set); a disabled button draws the pressed
// frame, which is the `ABCCC` reading the `Button` widgets declare.

/// The size of one button, from the frame strip: a third of its width, its
/// height. `{51, 52}` for every shipped faction.
[[nodiscard]] Rect command_button_cell(ResourceCache& cache, const BarContent& content);
/// Where button `index` of `count` stands inside a bar of `bar`'s size.
[[nodiscard]] Rect command_button_rect(const Rect& bar, const Rect& cell, std::size_t index,
                                       std::size_t count);
/// Draw the row over an already-composited bar.
void paint_command_buttons(Canvas& canvas, ResourceCache& cache, const BarContent& content,
                           const Rect& bar);
/// The button under a bar-local point, or -1.
[[nodiscard]] std::int32_t command_button_at(ResourceCache& cache, const BarContent& content,
                                             const Rect& bar, std::int32_t x, std::int32_t y);

}  // namespace imperivm::core::ui
