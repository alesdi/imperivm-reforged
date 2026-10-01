#pragma once

/// A menu screen running: its widgets' state, what the pointer and the
/// keyboard do to them, and what comes out.
///
/// The 48 files under `DATA/INTERFACE/MENU/` declare the menus the way the
/// 24 in-game files declare the bars, and `interface.hpp` reads them with the
/// same interpreter. What they do not declare is behaviour: a button posts
/// its `Id` and the code that opened the dialog decides what `0x1006` means.
/// This class is the part between -- the toolkit's own `CUIDialog`, as far
/// as a screen needs it -- and it is in the core so that a test can open
/// `GAMEMENU.INI`, click `Quit` and see `0x1006` come out with no window.
///
/// A dialog is laid out at its design size and placed on the display by the
/// caller: `Rect` is where its canvas sits, and the caller centres it, as
/// the toolkit centres a child in its parent (0x006b3e00). Every shipped
/// menu is authored for 1024 x 768 -- `StdDlg` caps at `MaxSize = 1024, 768`
/// -- and the springs never stretch anything here. **Reading, labelled:**
/// the original ran at 1024 x 768 and the question of a larger display
/// never arose in it.
///
/// What a click does, by widget class:
///
///   * `Button`, `ImageButton`, `ActiveButton` with an `Id`: pressed on
///     release over the same widget, posting `Event::kCommand` with the id.
///     A `TOGGLE` button flips its row first.
///   * `List`: a click selects the row under it (`Event::kSelect`), a
///     second click on the selected row is `Event::kActivate`.
///   * `EditW`: takes the focus; typed text goes into it.
///   * `Combobox`: opens its list; a click on a row selects it and closes.
///   * `Scroll` with no `TargetId`: a slider; a press or a drag sets its
///     value, 0..100 along the run, and reports `Event::kChange` with it.
///   * A `TRISTATE` check button flips between its first two rows.
///   * `Control` (`Move`, the size grips), `TextW`, frames: nothing.
///
/// **Two rules the toolkit keys off the `Id` itself** (`CUIDialog`,
/// 0x0066d515 and 0x0066d4f0), which is how the editor's `AdvObjProps.ini`
/// gets its tabs and its radio groups with nothing but `Id` lines:
///
///   * An id whose bits 16..23 are `0x03` is a **tab button**: pressing it
///     is `set_tab(id & 0xffff)` (0x0066d2a0). A widget whose id has a
///     non-zero top byte belongs to that tab and is hidden while another is
///     current; an id with a zero top byte is on every tab. The tab
///     buttons themselves show their pressed row for the current tab.
///     Every dialog starts on tab 1 (the toolkit's constructor, 0x0066d5f0),
///     which is why a screen with no tabs at all -- and `SETTINGS.INI`'s
///     `0x01000008` -- shows everything.
///   * An id whose bits 16..23 are `0x02` is a **radio button**: pressing it
///     puts its row at 1 and the row of every other radio button whose id
///     agrees above the low byte at 0 (0x0066d410). `AdvArea.ini`'s
///     `0x021001`/`0x021002` and the sheet's `0x4020021`/`0x4020022`.
///
/// Keys: `Esc` and `Enter` press the widgets the screen's `Esc =` and
/// `Enter =` lines name -- as `Event::kCommand` with that widget's id -- or,
/// when the screen names none, `Event::kEscape` so the caller can close it.
/// `Tab` moves the focus between `TABSTOP` widgets.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/ui/interface.hpp"
#include "imperivm/core/ui/layout.hpp"
#include "imperivm/core/ui/paint.hpp"

namespace imperivm::core::ui {

/// The keys a dialog reads. Letters and digits arrive as text.
enum class DialogKey : std::uint8_t {
  kEscape,
  kEnter,
  kBackspace,
  kDelete,
  kLeft,
  kRight,
  kHome,
  kEnd,
  kUp,
  kDown,
  kPageUp,
  kPageDown,
  kTab,
};

struct DialogEvent {
  enum class Kind : std::uint8_t {
    kNone,
    kCommand,   ///< a button with an `Id` was pressed; `id` is it
    kSelect,    ///< a list row was selected; `widget`, `index`
    kActivate,  ///< a selected list row was clicked again
    kChange,    ///< an edit's text or a combobox's choice changed
    kEscape,    ///< Escape with no `Esc =` widget to press
    kClick,     ///< a `ConquestMap`, a widget carrying a composed `bitmap`,
                ///< or the backdrop with no widget under the point, was
                ///< clicked; `widget` names it (empty for the backdrop) and
                ///< `x`, `y` are local to it
  };
  Kind kind = Kind::kNone;
  std::int32_t id = 0;
  std::string widget;
  std::int32_t index = -1;
  std::int32_t x = 0;
  std::int32_t y = 0;
};

class Dialog {
 public:
  Dialog(Screen screen, ResourceCache& cache);

  /// Where the canvas sits on the display; the canvas is the design size.
  void place(std::int32_t x, std::int32_t y);
  /// Lay the screen out again at another size: the zoom map's dialog is
  /// as big as the map it shows.
  void resize(std::int32_t width, std::int32_t height);
  [[nodiscard]] Rect rect() const noexcept { return rect_; }
  [[nodiscard]] const Screen& screen() const noexcept { return screen_; }
  [[nodiscard]] const Layout& layout() const noexcept { return layout_; }
  [[nodiscard]] DialogContent& content() noexcept { return content_; }
  [[nodiscard]] const DialogContent& content() const noexcept { return content_; }
  [[nodiscard]] bool modal() const noexcept { return has(screen_.style, Style::kModal); }

  // -- state, by widget name -------------------------------------------------

  void set_text(std::string_view widget, std::string_view text);
  [[nodiscard]] std::string text(std::string_view widget) const;
  void set_enabled(std::string_view widget, bool enabled);
  void set_hidden(std::string_view widget, bool hidden);
  void set_row(std::string_view widget, std::int32_t row);
  /// The current tab (see the class comment): the widgets on another tab
  /// are hidden, the tab buttons show which is current.
  void set_tab(std::int32_t tab);
  [[nodiscard]] std::int32_t tab() const noexcept { return tab_; }
  void set_items(std::string_view widget, std::vector<std::string> items);
  void select(std::string_view widget, std::int32_t index);
  /// A slider's value, 0..100.
  void set_value(std::string_view widget, std::int32_t value);
  [[nodiscard]] std::int32_t value(std::string_view widget) const;
  [[nodiscard]] std::int32_t selected(std::string_view widget) const;
  void focus(std::string_view widget);
  [[nodiscard]] const std::string& focused() const noexcept { return content_.focused; }
  /// The widget by `Id`, or null.
  [[nodiscard]] const Widget* widget_with_id(std::int32_t id) const noexcept;

  // -- input, in display coordinates ------------------------------------------

  DialogEvent mouse_move(std::int32_t x, std::int32_t y);
  DialogEvent mouse_down(std::int32_t x, std::int32_t y);
  DialogEvent mouse_up(std::int32_t x, std::int32_t y);
  DialogEvent key(DialogKey key);
  DialogEvent text_input(std::string_view text);

  /// The widget under a display-space point, or null. Later widgets are
  /// drawn over earlier ones and win the hit; `INACTIVE`, hidden and
  /// disabled widgets are passed over.
  [[nodiscard]] const Widget* widget_at(std::int32_t x, std::int32_t y) const;
  /// The widget carrying a composed `bitmap` under the point, whatever its
  /// style -- the *Place Label* screen's minimap -- or null. Such a widget
  /// takes the click (`mouse_down`) and claims the point for its dialog.
  [[nodiscard]] const Widget* bitmap_widget_at(std::int32_t x, std::int32_t y) const;
  /// The placed rectangle of a widget, in canvas coordinates.
  [[nodiscard]] Rect widget_rect(std::string_view widget) const;
  /// The rectangle of a list's `index`-th row as it is drawn, in canvas
  /// coordinates, or an empty rectangle when the row is scrolled out of
  /// sight or the widget is not a list. What a scripted click aims at.
  [[nodiscard]] Rect item_rect(std::string_view widget, std::int32_t index) const;

  /// Whether anything has changed since `clear_dirty`.
  [[nodiscard]] bool dirty() const noexcept { return dirty_; }
  void clear_dirty() noexcept { dirty_ = false; }
  /// Something the code composed changed -- a `bitmap`, a `text_scroll`, a
  /// backdrop -- which the setters above did not see.
  void touch() noexcept { dirty_ = true; }

  /// Composite onto `canvas`, which is resized to the design size.
  void paint(Canvas& canvas);

 private:
  [[nodiscard]] DialogEvent press(const Widget& widget);
  [[nodiscard]] DialogEvent press_named(std::string_view name);
  [[nodiscard]] bool takes_input(const Widget& widget) const noexcept;
  void scroll_list(std::string_view list, std::int32_t by);
  /// Set a slider from a canvas-local x; true when the value changed.
  bool slide(const Widget& widget, std::int32_t local_x, std::int32_t local_y);
  void fit_to_images();

  Screen screen_;
  ResourceCache* cache_;
  Layout layout_;
  Rect rect_;
  DialogContent content_;
  std::int32_t tab_ = 1;
  bool dirty_ = true;
};

}  // namespace imperivm::core::ui
