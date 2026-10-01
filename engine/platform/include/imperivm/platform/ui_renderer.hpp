#pragma once

/// Putting the game's own interface on the screen.
///
/// The model — reading the `.ini` screens, resolving `%Name%`, laying the
/// widgets out and compositing them — is entirely in `imperivm::core::ui` and
/// runs with no window. This class is the seam: it hands the core the bytes it
/// asks for through the `Vfs`, and it uploads and blits the finished pixels.
///
/// **One CPU canvas per layer, drawn blended.** The bars are composited on
/// the CPU into one RGBA buffer each, because they change only when the
/// selection does. They were blitted for as long as they were the only
/// layers: both are opaque strips -- every skin's `Background` is a
/// 2048-pixel-wide bitmap with no colour key. The menus are not opaque: a
/// `DarkFrame` halves the world under it and a frame's colour-keyed middle
/// lets it through. So every layer -- bar, tooltip, dialog -- now goes
/// through `LayerRenderer`, one blended quad each, and the blit is gone.
///
/// The upper bar is one screen (`INFOBAR_<faction>.INI`). The lower bar is two,
/// composited in order: the command bar's own background (`CMDBAR*.INI`) and
/// then whichever overlay is current — the nine-button no-selection menu
/// (`EMPTY_*.INI`) by default. That is why the empty menu declares no
/// background of its own.
///
/// **The dialogs** are a stack over the bars: `push_dialog` opens a menu
/// screen by its virtual path (`menuini/gamemenu.ini`), laid out at its
/// design size and centred on the display as the toolkit centres a child
/// in its parent (0x006b3e00), and the top one takes the input
/// (`core::ui::Dialog`). Their behaviour -- what
/// `0x1006` means -- is the app's; this draws them and hands the events up.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/ui/dialog.hpp"
#include "imperivm/core/ui/interface.hpp"
#include "imperivm/core/ui/layout.hpp"
#include "imperivm/core/ui/paint.hpp"
#include "imperivm/platform/layer_renderer.hpp"
#include "imperivm/platform/vfs.hpp"

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;

namespace imperivm::platform {

/// Which of the two bars a call is about.
enum class Bar : std::uint8_t {
  kUpper = 0,  ///< the info bar, across the top
  kLower = 1,  ///< the command bar, across the bottom
};

class UiRenderer {
 public:
  UiRenderer() = default;
  ~UiRenderer();

  UiRenderer(const UiRenderer&) = delete;
  UiRenderer& operator=(const UiRenderer&) = delete;

  /// Registers the interface's virtual roots on `vfs`.
  ///
  /// `mount_installation` already aliases `gameres/`; the screens also name
  /// `gameini/`, `menuini/`, `menures/`, `editorini/`, `editorres/` and
  /// `commonini/`, and every one of them is pinned by the same file being
  /// addressed both ways somewhere in the retail data. Safe to call twice.
  static void install_aliases(Vfs& vfs);

  /// Binds the VFS and the GPU device, and builds the layer pipeline for
  /// `target_format` (`render_target_format()`). Does not load anything.
  bool create(Vfs& vfs, SDL_GPUDevice* device, std::uint32_t target_format,
              std::string* error = nullptr);
  void destroy();

  [[nodiscard]] bool ready() const noexcept { return device_ != nullptr; }

  /// Loads one faction's bars: `INFOBAR_<faction>.INI` for the upper bar,
  /// `CMDBAR<faction>.INI` plus `EMPTY_<faction>.INI` for the lower one.
  /// `faction` is matched case-insensitively against `core::ui::faction_bars`.
  ///
  /// Everything the screens name is read here and cached, so a later redraw
  /// touches no file.
  bool load_faction(std::string_view faction, std::string* error = nullptr);

  /// The display size. The bars span the full width; the upper one sits at the
  /// top and the lower one at the bottom, each keeping its authored height.
  void set_viewport(std::uint32_t width, std::uint32_t height);

  /// The fog of war over the world and under everything the interface
  /// draws: `image` is one pixel per fog cell (`FogView::overlay`), drawn
  /// stretched to `width` x `height` pixels from `(x, y)`, sampled linearly
  /// so the cells' edges blend. An empty image draws no fog.

  /// A picture over the world and the fog, under the bars and the menus,
  /// drawn pixel for pixel at `(x, y)`: the editor's marks -- the areas'
  /// outlines, the brush's footprint, a placed object's direction. An empty
  /// image draws none. Re-uploaded only when its pixels change.
  void set_overlay(const core::ui::Image& image, std::int32_t x, std::int32_t y);

  /// Whether the bars are drawn at all: not on the front, where there is
  /// no game under them.
  void show_bars(bool shown) noexcept { bars_shown_ = shown; }

  [[nodiscard]] core::ui::Rect bar_rect(Bar bar) const noexcept;

  /// Replaces what a bar shows. Recompositing happens here, not in `render`, so
  /// a frame that changes nothing costs one blit.
  void set_content(Bar bar, const core::ui::BarContent& content);

  /// The composited pixels of a bar, for a test or a screenshot.
  [[nodiscard]] const core::ui::Canvas& canvas(Bar bar) const noexcept;

  /// The widget under a display-space point, or null. Widgets are tested in
  /// reverse declaration order, so the thing drawn last wins the click, and
  /// `INACTIVE` and `HIDDEN` widgets are skipped.
  [[nodiscard]] const core::ui::Widget* widget_at(std::int32_t x, std::int32_t y) const;
  /// The bar screen a widget from `widget_at` belongs to, or null.
  [[nodiscard]] const core::ui::Screen* screen_of(const core::ui::Widget* widget) const noexcept;
  /// A bar widget by section name (`CmdCancel`), drawn or not, or null.
  [[nodiscard]] const core::ui::Widget* bar_widget(std::string_view name) const noexcept;
  /// Where a bar widget that `widget_at` could return is, in display space,
  /// by section name; empty when it is not shown. A scripted press aims here.
  [[nodiscard]] core::ui::Rect bar_widget_rect(std::string_view name) const;
  /// The bar strip cell under a display-space point: a garrison portrait, a
  /// queue entry, an item. See `core::ui::strip_cell_at`.
  [[nodiscard]] core::ui::StripCellHit strip_cell_at(std::int32_t x, std::int32_t y) const;
  /// Where cell `index` of the bar strip named `strip` is drawn, in display
  /// space, or an empty rectangle.
  [[nodiscard]] core::ui::Rect strip_cell_rect(std::string_view strip, std::size_t index) const;

  /// The command button under a display-space point -- an index into the
  /// lower bar's `BarContent::buttons` -- or -1.
  [[nodiscard]] std::int32_t button_at(std::int32_t x, std::int32_t y) const;

  /// A tooltip: `lines` in the interface font in an opaque box whose bottom
  /// -left corner sits at `(x, y)` in display space, clamped to the display.
  /// Empty lines clear it. Recomposited only when the text changes.
  void set_tooltip(std::vector<std::string> lines, std::int32_t x, std::int32_t y);

  /// Messages -- a networked match's chat -- in the tooltip's box, in the
  /// display's top-left corner below the upper bar, oldest first. Empty
  /// lines clear them. **The place is this engine's**: the original's
  /// message area is not modelled yet.
  void set_messages(std::vector<std::string> lines);

  // -- dialogs ------------------------------------------------------------------

  /// Open the screen at `path` (`menuini/gamemenu.ini`) on top of the stack.
  /// `section` names the dialog section in the file when it is not the
  /// first: the editor's `AdvObjProps.ini` holds six. Null, with `error`
  /// set, when it would not load.
  core::ui::Dialog* push_dialog(std::string_view path, std::string* error = nullptr,
                                std::string_view section = {});
  /// Put `dialog` at `(x, y)` on the 1024 x 768 canvas the menus are
  /// authored for (itself centred on the display) instead of centring it:
  /// the setup screen's `SETTINGS.INI` stands at `SettingsPos`, a right-top
  /// offset the players' screen declares.
  void place_dialog(core::ui::Dialog* dialog, std::int32_t x, std::int32_t y);
  /// Where the 1024 x 768 canvas the menus are authored for sits on the
  /// display: `place_dialog`'s coordinates are relative to its corner.
  [[nodiscard]] core::ui::Rect canvas_rect() const noexcept {
    return core::ui::Rect{(static_cast<std::int32_t>(width_) - 1024) / 2,
                          (static_cast<std::int32_t>(height_) - 768) / 2, 1024, 768};
  }
  /// Close the top dialog.
  void pop_dialog();
  /// Close the dialog at `index`, wherever it stands: the editor's windows
  /// are opened in one order and closed in another.
  void close_dialog(std::size_t index);
  /// The index of `dialog` in the stack, or `dialog_count()` when it is not there.
  [[nodiscard]] std::size_t index_of(const core::ui::Dialog* dialog) const noexcept;
  /// Move `dialog` to the top of the stack, keeping the order of the rest.
  ///
  /// The stack order **is** the z-order: later dialogs draw over earlier
  /// ones and `dialog_at` searches from the top down. Until the editor's
  /// `nextwnd` there was nothing that reordered it -- a dialog went on top
  /// when it opened and stayed where it was until it closed -- so a caller
  /// holding a stack *index* across this call must resolve it again, which
  /// is why this takes a pointer and not an index.
  void raise_dialog(core::ui::Dialog* dialog);
  void pop_all_dialogs();
  [[nodiscard]] core::ui::Dialog* top_dialog() noexcept;
  [[nodiscard]] core::ui::Dialog* dialog(std::size_t index) noexcept;
  [[nodiscard]] std::size_t dialog_count() const noexcept { return dialogs_.size(); }
  /// The index of the topmost dialog with a widget under a display-space
  /// point, or the top's when none has one; `dialog_count()` when none is
  /// open. Two screens side by side -- the players and the settings --
  /// share the pointer this way.
  [[nodiscard]] std::size_t dialog_at(std::int32_t x, std::int32_t y) const;
  /// Whether the top dialog is `MODAL`, which takes every click.
  [[nodiscard]] bool modal() const noexcept;

  /// Draws the bars, the dialogs and the tooltip onto `target`, blended in
  /// that order. Call inside a frame's command buffer, after the world has
  /// been drawn.
  bool render(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, std::string* error = nullptr);

  /// Anything a screen named that could not be read: a missing section, an
  /// unknown style, a bitmap that is not in the packs. Empty for a correct
  /// retail install.
  [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  struct BarState {
    /// The screens composited into this bar, in drawing order.
    std::vector<core::ui::Screen> screens;
    std::vector<core::ui::Layout> layouts;
    core::ui::Canvas canvas;
    core::ui::BarContent content;
    core::ui::Rect rect;
    SDL_GPUTexture* texture = nullptr;
    std::uint32_t texture_width = 0;
    std::uint32_t texture_height = 0;
    bool dirty = true;
  };

  /// One open menu: the dialog and the layer it is drawn into.
  struct DialogLayer {
    std::unique_ptr<core::ui::Dialog> dialog;
    BarState layer;
    std::string path;
    /// A placement on the 1024 x 768 canvas, or centred when unset.
    bool placed = false;
    std::int32_t canvas_x = 0;
    std::int32_t canvas_y = 0;
  };

  void relayout(BarState& bar);
  void composite(BarState& bar);
  bool upload(BarState& bar, std::string* error);
  void release(BarState& bar);
  void place_dialogs();

  Vfs* vfs_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  std::unique_ptr<core::ui::ResourceCache> cache_;
  /// The faction's command-button frame, from `CMDBAR/FRAMES.INI`.
  core::ui::ImageRef button_frame_;
  /// The tooltip, as a third bar-shaped thing: its own canvas and texture.
  BarState tooltip_;
  BarState messages_;
  BarState overlay_;
  std::vector<std::string> tooltip_lines_;
  std::vector<std::string> message_lines_;
  /// `lines` in the interface font into `box`, sized, at the origin. False,
  /// with `box` cleared, when there is nothing to show.
  bool compose_lines(BarState& box, const std::vector<std::string>& lines);
  /// `%InterfaceFont%`, read off the first widget of the info bar that names
  /// a font -- every skin's is `Fonts/Tahoma14b.apf`.
  std::string interface_font_;
  BarState bars_[2];
  std::vector<std::unique_ptr<DialogLayer>> dialogs_;
  bool bars_shown_ = true;
  LayerRenderer layers_;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::vector<std::string> diagnostics_;
};

}  // namespace imperivm::platform
