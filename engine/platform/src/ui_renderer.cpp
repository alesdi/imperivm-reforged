#include "imperivm/platform/ui_renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>

#include "imperivm/core/ui/markup.hpp"
#include "imperivm/platform/bytes.hpp"

namespace imperivm::platform {
namespace {

bool same_name(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

constexpr SDL_GPUTextureFormat kFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

/// The interface's virtual roots, and the evidence for each. Every one is
/// pinned by the same file being addressed both ways in the retail data:
///
/// * `gameini/` — `CONST.INI` writes `data/interface/cmdBar/cmdbar.ini` where
///   `gbr.exe` writes `gameini/cmdbar/cmdbar.ini`.
/// * `menuini/` — `gbr.exe` names both `menuini/confirm.ini` and
///   `gameini/Menu/RollOverTextDlg.ini`.
/// * `menures/` — `gbr.exe` names both `ui/menu/profileicons/unknown.bmp` and
///   `%s/ShieldArrow.bmp` under `menures`.
/// * `editorini/`, `editorres/`, `commonini/` — `gbr.exe` names
///   `Data/interface/editor/combo/note_icons.ini`, `EditorRes/curmap.bmp` and
///   `data/interface/common/ScriptEdit.ini` beside the aliased spellings.
constexpr struct {
  const char* prefix;
  const char* target;
} kAliases[] = {
    {"gameres\\", "UI\\"},
    {"gameini\\", "DATA\\INTERFACE\\"},
    {"menures\\", "UI\\MENU\\"},
    {"menuini\\", "DATA\\INTERFACE\\MENU\\"},
    {"editorres\\", "UI\\EDITOR\\"},
    {"editorini\\", "DATA\\INTERFACE\\EDITOR\\"},
    {"commonini\\", "DATA\\INTERFACE\\COMMON\\"},
    {"commonres\\", "UI\\COMMON\\"},
};

}  // namespace

UiRenderer::~UiRenderer() { destroy(); }

void UiRenderer::install_aliases(Vfs& vfs) {
  // `alias` appends, and resolution takes the first match, so registering
  // `gameres\` twice is harmless: the second copy is never reached.
  for (const auto& entry : kAliases) vfs.alias(entry.prefix, entry.target);
}

bool UiRenderer::create(Vfs& vfs, SDL_GPUDevice* device, std::uint32_t target_format,
                        std::string* error) {
  if (device == nullptr) {
    if (error != nullptr) *error = "no GPU device";
    return false;
  }
  destroy();
  vfs_ = &vfs;
  device_ = device;
  install_aliases(vfs);
  if (!layers_.create(device, target_format, error)) {
    vfs_ = nullptr;
    device_ = nullptr;
    return false;
  }
  // The provider is the whole of the core's contact with the filesystem: it
  // takes a virtual path and gets bytes, and never learns what a pack is.
  cache_ = std::make_unique<core::ui::ResourceCache>(
      [this](std::string_view path) { return as_core_bytes(vfs_->read(path)); });
  return true;
}

void UiRenderer::destroy() {
  for (BarState& bar : bars_) release(bar);
  release(tooltip_);
  release(messages_);
  release(overlay_);
  for (auto& open : dialogs_) release(open->layer);
  dialogs_.clear();
  layers_.destroy();
  cache_.reset();
  vfs_ = nullptr;
  device_ = nullptr;
  width_ = 0;
  height_ = 0;
  diagnostics_.clear();
}

void UiRenderer::release(BarState& bar) {
  if (device_ != nullptr && bar.texture != nullptr) SDL_ReleaseGPUTexture(device_, bar.texture);
  bar.texture = nullptr;
  bar.texture_width = 0;
  bar.texture_height = 0;
}

bool UiRenderer::load_faction(std::string_view faction, std::string* error) {
  if (vfs_ == nullptr) {
    if (error != nullptr) *error = "UiRenderer::create has not been called";
    return false;
  }
  const core::ui::FactionBars* bars = core::ui::faction_bars_for(faction);
  if (bars == nullptr) {
    if (error != nullptr) *error = "no interface skin for faction '" + std::string{faction} + "'";
    return false;
  }

  const auto provider = [this](std::string_view path) {
    return as_core_bytes(vfs_->read(path));
  };
  const auto load = [&](std::string_view path, std::vector<core::ui::Screen>& into) {
    core::Result<core::ui::Screen> screen = core::ui::load_screen(path, {}, provider);
    if (!screen.ok()) {
      diagnostics_.push_back(std::string{path} + ": screen would not load");
      return false;
    }
    for (const std::string& warning : screen->warnings) {
      diagnostics_.push_back(std::string{path} + ": " + warning);
    }
    for (const std::string& missing : screen->missing) {
      diagnostics_.push_back(std::string{path} + ": no section [" + missing + "]");
    }
    into.push_back(std::move(screen.value()));
    return true;
  };

  for (BarState& bar : bars_) {
    bar.screens.clear();
    bar.layouts.clear();
    bar.dirty = true;
  }

  bool ok = load(bars->infobar, bars_[static_cast<int>(Bar::kUpper)].screens);
  ok = load(bars->cmdbar, bars_[static_cast<int>(Bar::kLower)].screens) && ok;
  // The no-selection menu is an overlay on the command bar's background, which
  // is why it declares none of its own.
  ok = load(bars->empty, bars_[static_cast<int>(Bar::kLower)].screens) && ok;
  // The frame the command buttons are drawn in, by the faction's key in
  // `CMDBAR/FRAMES.INI`.
  button_frame_ = core::ui::ImageRef{};
  if (const auto table = core::ui::load_image_table(core::ui::kCommandFrames, "Images", provider);
      table.ok()) {
    for (const core::ui::NamedImage& entry : table.value()) {
      if (same_name(entry.name, bars->frame_key)) button_frame_ = entry.image;
    }
  }
  if (button_frame_.empty()) {
    diagnostics_.push_back(std::string{core::ui::kCommandFrames} + ": no frame for " +
                           std::string{bars->frame_key});
  }
  bars_[static_cast<int>(Bar::kLower)].content.button_frame = button_frame_;
  interface_font_.clear();
  for (const core::ui::Screen& screen : bars_[static_cast<int>(Bar::kUpper)].screens) {
    for (const core::ui::Widget& widget : screen.widgets) {
      const std::string_view font = widget.attribute("Font");
      if (!font.empty()) {
        interface_font_.assign(font);
        break;
      }
    }
    if (!interface_font_.empty()) break;
  }
  if (!ok && error != nullptr) *error = "one or more interface screens would not load";

  if (width_ != 0 && height_ != 0) {
    for (BarState& bar : bars_) relayout(bar);
  }
  return ok;
}

void UiRenderer::set_viewport(std::uint32_t width, std::uint32_t height) {
  if (width == width_ && height == height_) return;
  width_ = width;
  height_ = height;
  for (BarState& bar : bars_) relayout(bar);
  place_dialogs();
}

// -- dialogs -------------------------------------------------------------------

void UiRenderer::place_dialogs() {
  // A dialog is centred in the display: 0x006b3e00 places a child at
  // `(parent + 1 - child + 1) / 2` on each axis, which is how the
  // no-selection menu sits in the command bar and how `NOTES.INI`'s
  // 512 x 512 screen, authored at `0, 0`, comes to the middle. The full-
  // screen menus are 1024 x 768 and centre to the same place a centred
  // canvas would put them; the `RectWH` origin of a smaller one
  // (`SAVEGAME.INI`'s `100, 100`) is not where it is shown.
  for (auto& open : dialogs_) {
    const core::ui::Rect design = open->dialog->screen().design;
    std::int32_t x = (static_cast<std::int32_t>(width_) + 1 - design.width + 1) / 2;
    std::int32_t y = (static_cast<std::int32_t>(height_) + 1 - design.height + 1) / 2;
    if (open->placed) {
      x = (static_cast<std::int32_t>(width_) - 1024) / 2 + open->canvas_x;
      y = (static_cast<std::int32_t>(height_) - 768) / 2 + open->canvas_y;
    }
    open->dialog->place(x, y);
    open->layer.rect = open->dialog->rect();
  }
}

void UiRenderer::place_dialog(core::ui::Dialog* dialog, std::int32_t x, std::int32_t y) {
  for (auto& open : dialogs_) {
    if (open->dialog.get() != dialog) continue;
    open->placed = true;
    open->canvas_x = x;
    open->canvas_y = y;
  }
  place_dialogs();
}

core::ui::Dialog* UiRenderer::push_dialog(std::string_view path, std::string* error,
                                          std::string_view section) {
  if (vfs_ == nullptr || cache_ == nullptr) {
    if (error != nullptr) *error = "UiRenderer::create has not been called";
    return nullptr;
  }
  const auto provider = [this](std::string_view name) {
    return as_core_bytes(vfs_->read(name));
  };
  core::Result<core::ui::Screen> screen = core::ui::load_screen(path, section, provider);
  if (!screen.ok()) {
    if (error != nullptr) *error = std::string{path} + ": screen would not load";
    return nullptr;
  }
  for (const std::string& warning : screen->warnings) {
    diagnostics_.push_back(std::string{path} + ": " + warning);
  }
  auto open = std::make_unique<DialogLayer>();
  open->path.assign(path);
  open->dialog = std::make_unique<core::ui::Dialog>(std::move(screen.value()), *cache_);
  open->layer.dirty = true;
  dialogs_.push_back(std::move(open));
  place_dialogs();
  return dialogs_.back()->dialog.get();
}

void UiRenderer::pop_dialog() {
  if (dialogs_.empty()) return;
  release(dialogs_.back()->layer);
  dialogs_.pop_back();
}

void UiRenderer::close_dialog(std::size_t index) {
  if (index >= dialogs_.size()) return;
  release(dialogs_[index]->layer);
  dialogs_.erase(dialogs_.begin() + static_cast<std::ptrdiff_t>(index));
}

void UiRenderer::raise_dialog(core::ui::Dialog* dialog) {
  const std::size_t index = index_of(dialog);
  if (index >= dialogs_.size() || index + 1 == dialogs_.size()) return;
  // A rotate rather than an erase and a push: the entries between keep
  // their order, and the layer -- its texture and its dirty flag -- moves
  // with the dialog rather than being released and composited again.
  std::rotate(dialogs_.begin() + static_cast<std::ptrdiff_t>(index),
              dialogs_.begin() + static_cast<std::ptrdiff_t>(index) + 1, dialogs_.end());
}

std::size_t UiRenderer::index_of(const core::ui::Dialog* dialog) const noexcept {
  for (std::size_t i = 0; i < dialogs_.size(); ++i) {
    if (dialogs_[i]->dialog.get() == dialog) return i;
  }
  return dialogs_.size();
}

void UiRenderer::pop_all_dialogs() {
  while (!dialogs_.empty()) pop_dialog();
}

core::ui::Dialog* UiRenderer::top_dialog() noexcept {
  return dialogs_.empty() ? nullptr : dialogs_.back()->dialog.get();
}

core::ui::Dialog* UiRenderer::dialog(std::size_t index) noexcept {
  return index < dialogs_.size() ? dialogs_[index]->dialog.get() : nullptr;
}

std::size_t UiRenderer::dialog_at(std::int32_t x, std::int32_t y) const {
  if (dialogs_.empty()) return 0;
  // A modal top takes everything.
  if (dialogs_.back()->dialog->modal()) return dialogs_.size() - 1;
  for (std::size_t i = dialogs_.size(); i-- > 0;) {
    if (dialogs_[i]->dialog->widget_at(x, y) != nullptr) return i;
    if (dialogs_[i]->dialog->bitmap_widget_at(x, y) != nullptr) return i;
  }
  return dialogs_.size() - 1;
}

bool UiRenderer::modal() const noexcept {
  return !dialogs_.empty() && dialogs_.back()->dialog->modal();
}

void UiRenderer::relayout(BarState& bar) {
  bar.layouts.clear();
  if (bar.screens.empty() || width_ == 0 || height_ == 0) return;

  // The bar is as tall as its first screen -- the one that carries the
  // background -- and as wide as the display. `gbr.exe` reads the bars'
  // geometry from `CONST.INI`'s `[UIBars]` and gives them the full width.
  const std::int32_t bar_height = bar.screens.front().design.height > 0
                                      ? bar.screens.front().design.height
                                      : 0;
  const std::int32_t width = static_cast<std::int32_t>(width_);
  const bool upper = &bar == &bars_[static_cast<int>(Bar::kUpper)];
  bar.rect = core::ui::Rect{0, upper ? 0 : static_cast<std::int32_t>(height_) - bar_height, width,
                            bar_height};

  for (std::size_t i = 0; i < bar.screens.size(); ++i) {
    const core::ui::Screen& screen = bar.screens[i];
    core::ui::Layout layout = core::ui::layout_screen(screen, width, bar_height);
    // The second screen of the lower bar is the no-selection menu, a dialog
    // narrower than the bar that the original centres in it (`ui/paint.hpp`
    // records the reading). Laid out at its own design width, then moved.
    if (!upper && i > 0 && screen.design.width > 0 && screen.design.width < width) {
      layout = core::ui::layout_screen(screen, screen.design.width, bar_height);
      const std::int32_t shift = (width - screen.design.width) / 2;
      for (core::ui::LaidOutWidget& placed : layout.widgets) placed.rect.x += shift;
    }
    bar.layouts.push_back(std::move(layout));
  }
  if (bar.canvas.width() != static_cast<std::uint32_t>(width) ||
      bar.canvas.height() != static_cast<std::uint32_t>(std::max(0, bar_height))) {
    bar.canvas.resize(static_cast<std::uint32_t>(std::max(0, width)),
                      static_cast<std::uint32_t>(std::max(0, bar_height)));
  }
  bar.dirty = true;
}

core::ui::Rect UiRenderer::bar_rect(Bar bar) const noexcept {
  return bars_[static_cast<int>(bar)].rect;
}

void UiRenderer::set_content(Bar which, const core::ui::BarContent& content) {
  BarState& bar = bars_[static_cast<int>(which)];
  bar.content = content;
  if (which == Bar::kLower) bar.content.button_frame = button_frame_;
  bar.dirty = true;
}

void UiRenderer::set_overlay(const core::ui::Image& image, std::int32_t x, std::int32_t y) {
  overlay_.rect = core::ui::Rect{x, y, static_cast<std::int32_t>(image.width),
                                 static_cast<std::int32_t>(image.height)};
  if (image.empty()) {
    if (overlay_.canvas.width() != 0) {
      overlay_.canvas.resize(0, 0);
      release(overlay_);
    }
    return;
  }
  if (overlay_.canvas.width() == image.width && overlay_.canvas.height() == image.height &&
      std::equal(image.rgba.begin(), image.rgba.end(), overlay_.canvas.pixels().begin())) {
    return;
  }
  overlay_.canvas.resize(image.width, image.height);
  overlay_.canvas.clear();
  overlay_.canvas.blit(image, 0, 0,
                       core::ui::Rect{0, 0, static_cast<std::int32_t>(image.width),
                                      static_cast<std::int32_t>(image.height)});
  overlay_.dirty = true;
}

void UiRenderer::set_tooltip(std::vector<std::string> lines, std::int32_t x, std::int32_t y) {
  if (lines == tooltip_lines_ && !lines.empty()) {
    // Same text: only the place moves.
    const std::int32_t width = tooltip_.rect.width;
    const std::int32_t height = tooltip_.rect.height;
    tooltip_.rect.x = std::clamp(x, 0, std::max(0, static_cast<std::int32_t>(width_) - width));
    tooltip_.rect.y = std::clamp(y - height, 0, std::max(0, static_cast<std::int32_t>(height_) - height));
    return;
  }
  tooltip_lines_ = std::move(lines);
  if (!compose_lines(tooltip_, tooltip_lines_)) return;
  const std::int32_t width = tooltip_.rect.width;
  const std::int32_t height = tooltip_.rect.height;
  tooltip_.rect.x = std::clamp(x, 0, std::max(0, static_cast<std::int32_t>(width_) - width));
  tooltip_.rect.y = std::clamp(y - height, 0, std::max(0, static_cast<std::int32_t>(height_) - height));
}

void UiRenderer::set_messages(std::vector<std::string> lines) {
  if (lines == message_lines_) return;
  message_lines_ = std::move(lines);
  if (!compose_lines(messages_, message_lines_)) return;
  // The top-left corner, clear of the upper bar when it is at the top.
  const BarState& upper = bars_[static_cast<int>(Bar::kUpper)];
  const std::int32_t top = bars_shown_ && upper.rect.y == 0 ? upper.rect.bottom() : 0;
  messages_.rect.x = 8;
  messages_.rect.y = top + 8;
}

bool UiRenderer::compose_lines(BarState& box, const std::vector<std::string>& lines) {
  if (lines.empty() || cache_ == nullptr) {
    box.rect = core::ui::Rect{};
    box.canvas.resize(0, 0);
    release(box);
    return false;
  }
  // The bars' interface font, or -- on the front, where no faction is loaded
  // -- the first font a shown menu names, which is the template's
  // `%InterfaceFont%` in every shipped menu.
  std::string font_path = interface_font_;
  for (std::size_t i = dialogs_.size(); font_path.empty() && i-- > 0;) {
    for (const core::ui::Widget& widget : dialogs_[i]->dialog->screen().widgets) {
      const std::string_view font = widget.attribute("Font");
      if (!font.empty()) {
        font_path.assign(font);
        break;
      }
    }
  }
  const core::ui::Font* font = font_path.empty() ? nullptr : cache_->font(font_path);
  const std::int32_t pad = 4;
  // Each entry is a paragraph carrying the markup the original composes
  // with -- `<color 255 255 0>` around a title, `<imagetransp ...>` before a
  // cost -- and its own breaks; white is the colour a paragraph starts in,
  // which is the colour the exe's composers reset to.
  std::vector<core::ui::MarkedLine> marked;
  for (const std::string& paragraph : lines) {
    for (core::ui::MarkedLine& line : core::ui::parse_markup(paragraph, core::ui::Color{})) {
      marked.push_back(std::move(line));
    }
  }
  if (marked.empty()) {
    box.rect = core::ui::Rect{};
    box.canvas.resize(0, 0);
    release(box);
    return false;
  }
  std::int32_t width = 0;
  std::vector<std::int32_t> heights;
  heights.reserve(marked.size());
  std::int32_t height = 2 * pad;
  for (const core::ui::MarkedLine& line : marked) {
    if (font != nullptr) {
      width = std::max(width, core::ui::marked_line_width(*cache_, *font, line));
      heights.push_back(core::ui::marked_line_height(*cache_, *font, line));
    } else {
      std::size_t characters = 0;
      for (const core::ui::TextSpan& span : line.spans) characters += span.text.size();
      width = std::max(width, static_cast<std::int32_t>(characters) * 7);
      heights.push_back(14);
    }
    height += heights.back();
  }
  width += 2 * pad;
  box.canvas.resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  // An opaque box: the bars are blitted, not blended, and so is this.
  box.canvas.clear(core::ui::Color{16, 12, 8, 255});
  const core::ui::Color border{200, 180, 120, 255};
  box.canvas.fill_rect(core::ui::Rect{0, 0, width, 1}, border);
  box.canvas.fill_rect(core::ui::Rect{0, height - 1, width, 1}, border);
  box.canvas.fill_rect(core::ui::Rect{0, 0, 1, height}, border);
  box.canvas.fill_rect(core::ui::Rect{width - 1, 0, 1, height}, border);
  if (font != nullptr) {
    const core::ui::Rect clip{0, 0, width, height};
    std::int32_t top = pad;
    for (std::size_t i = 0; i < marked.size(); ++i) {
      core::ui::draw_marked_line(box.canvas, *cache_, *font, marked[i], pad, top, clip);
      top += heights[i];
    }
  }
  box.rect = core::ui::Rect{0, 0, width, height};
  box.dirty = true;
  return true;
}

std::int32_t UiRenderer::button_at(std::int32_t x, std::int32_t y) const {
  if (cache_ == nullptr) return -1;
  const BarState& bar = bars_[static_cast<int>(Bar::kLower)];
  if (bar.rect.width <= 0 || bar.rect.height <= 0) return -1;
  if (x < bar.rect.x || x >= bar.rect.right() || y < bar.rect.y || y >= bar.rect.bottom()) {
    return -1;
  }
  const core::ui::Rect local{0, 0, bar.rect.width, bar.rect.height};
  return core::ui::command_button_at(*cache_, bar.content, local, x - bar.rect.x,
                                     y - bar.rect.y);
}

const core::ui::Canvas& UiRenderer::canvas(Bar bar) const noexcept {
  return bars_[static_cast<int>(bar)].canvas;
}

void UiRenderer::composite(BarState& bar) {
  if (bar.canvas.width() == 0 || bar.canvas.height() == 0) return;
  // Cleared to opaque black rather than to nothing: the bar is blitted, not
  // blended, so a pixel the art does not cover has to be *some* colour, and
  // black is what the original's letterboxing is.
  bar.canvas.clear(core::ui::Color{0, 0, 0, 255});
  const std::size_t count = std::min(bar.layouts.size(), bar.screens.size());
  const bool lower = &bar == &bars_[static_cast<int>(Bar::kLower)];
  for (std::size_t i = 0; i < count; ++i) {
    // The no-selection menu shows for an empty selection and for nothing
    // else: `CVXUIEmptySelectionMenu` is what its class is called.
    if (lower && i > 0 && !core::ui::any(bar.content.tags, core::ui::SelectionTag::kEmpty)) {
      continue;
    }
    core::ui::paint_screen(bar.canvas, bar.layouts[i], *cache_, bar.content);
  }
  if (lower) {
    core::ui::paint_command_buttons(bar.canvas, *cache_, bar.content,
                                    core::ui::Rect{0, 0, bar.rect.width, bar.rect.height});
  }
  bar.dirty = false;
}

bool UiRenderer::upload(BarState& bar, std::string* error) {
  const std::uint32_t width = bar.canvas.width();
  const std::uint32_t height = bar.canvas.height();
  if (width == 0 || height == 0) return true;

  if (bar.texture == nullptr || bar.texture_width != width || bar.texture_height != height) {
    release(bar);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    bar.texture = SDL_CreateGPUTexture(device_, &info);
    if (bar.texture == nullptr) {
      if (error != nullptr) *error = std::string{"SDL_CreateGPUTexture: "} + SDL_GetError();
      return false;
    }
    bar.texture_width = width;
    bar.texture_height = height;
  }

  const std::size_t bytes = static_cast<std::size_t>(width) * height * 4u;
  SDL_GPUTransferBufferCreateInfo transfer_info{};
  transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  transfer_info.size = static_cast<Uint32>(bytes);
  SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
  if (transfer == nullptr) {
    if (error != nullptr) *error = std::string{"SDL_CreateGPUTransferBuffer: "} + SDL_GetError();
    return false;
  }
  void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
  if (mapped == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    if (error != nullptr) *error = std::string{"SDL_MapGPUTransferBuffer: "} + SDL_GetError();
    return false;
  }
  SDL_memcpy(mapped, bar.canvas.pixels().data(), bytes);
  SDL_UnmapGPUTransferBuffer(device_, transfer);

  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
  if (commands == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    if (error != nullptr) *error = std::string{"SDL_AcquireGPUCommandBuffer: "} + SDL_GetError();
    return false;
  }
  SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(commands);
  SDL_GPUTextureTransferInfo source{};
  source.transfer_buffer = transfer;
  SDL_GPUTextureRegion destination{};
  destination.texture = bar.texture;
  destination.w = width;
  destination.h = height;
  destination.d = 1;
  SDL_UploadToGPUTexture(pass, &source, &destination, false);
  SDL_EndGPUCopyPass(pass);
  SDL_SubmitGPUCommandBuffer(commands);
  SDL_ReleaseGPUTransferBuffer(device_, transfer);
  return true;
}

const core::ui::Widget* UiRenderer::widget_at(std::int32_t x, std::int32_t y) const {
  // The dialogs first, topmost first: a menu stands over the bars.
  for (std::size_t i = dialogs_.size(); i-- > 0;) {
    if (const core::ui::Widget* widget = dialogs_[i]->dialog->widget_at(x, y)) return widget;
  }
  for (const BarState& bar : bars_) {
    if (bar.rect.width <= 0 || bar.rect.height <= 0) continue;
    if (x < bar.rect.x || x >= bar.rect.right() || y < bar.rect.y || y >= bar.rect.bottom()) {
      continue;
    }
    const std::int32_t local_x = x - bar.rect.x;
    const std::int32_t local_y = y - bar.rect.y;
    // Later screens are drawn over earlier ones, and within a screen later
    // widgets over earlier ones, so the search runs backwards through both.
    for (std::size_t i = bar.layouts.size(); i-- > 0;) {
      const core::ui::Layout& layout = bar.layouts[i];
      for (std::size_t j = layout.widgets.size(); j-- > 0;) {
        const core::ui::LaidOutWidget& placed = layout.widgets[j];
        const core::ui::Widget& widget = *placed.widget;
        if (has(widget.style, core::ui::Style::kInactive)) continue;
        if (has(widget.style, core::ui::Style::kHidden)) continue;
        if (!widget.visible_for(bar.content.tags, bar.content.tab)) continue;
        if (local_x < placed.rect.x || local_x >= placed.rect.right()) continue;
        if (local_y < placed.rect.y || local_y >= placed.rect.bottom()) continue;
        return &widget;
      }
    }
  }
  return nullptr;
}

const core::ui::Screen* UiRenderer::screen_of(const core::ui::Widget* widget) const noexcept {
  if (widget == nullptr) return nullptr;
  for (const std::unique_ptr<DialogLayer>& open : dialogs_) {
    const core::ui::Screen& screen = open->dialog->screen();
    for (const core::ui::Widget& candidate : screen.widgets) {
      if (&candidate == widget) return &screen;
    }
  }
  for (const BarState& bar : bars_) {
    for (const core::ui::Screen& screen : bar.screens) {
      for (const core::ui::Widget& candidate : screen.widgets) {
        if (&candidate == widget) return &screen;
      }
    }
  }
  return nullptr;
}

core::ui::Rect UiRenderer::bar_widget_rect(std::string_view name) const {
  for (const BarState& bar : bars_) {
    if (bar.rect.width <= 0 || bar.rect.height <= 0) continue;
    for (std::size_t i = bar.layouts.size(); i-- > 0;) {
      for (const core::ui::LaidOutWidget& placed : bar.layouts[i].widgets) {
        const core::ui::Widget& widget = *placed.widget;
        if (widget.name.size() != name.size() ||
            !std::equal(widget.name.begin(), widget.name.end(), name.begin(), [](char a, char b) {
              return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
          continue;
        }
        if (has(widget.style, core::ui::Style::kInactive) || has(widget.style, core::ui::Style::kHidden)) continue;
        if (!widget.visible_for(bar.content.tags, bar.content.tab)) continue;
        return core::ui::Rect{bar.rect.x + placed.rect.x, bar.rect.y + placed.rect.y, placed.rect.width,
                              placed.rect.height};
      }
    }
  }
  return core::ui::Rect{};
}

core::ui::StripCellHit UiRenderer::strip_cell_at(std::int32_t x, std::int32_t y) const {
  for (const BarState& bar : bars_) {
    if (bar.rect.width <= 0 || bar.rect.height <= 0) continue;
    if (x < bar.rect.x || x >= bar.rect.right() || y < bar.rect.y || y >= bar.rect.bottom()) continue;
    for (std::size_t i = bar.layouts.size(); i-- > 0;) {
      const core::ui::StripCellHit hit =
          core::ui::strip_cell_at(bar.layouts[i], bar.content, x - bar.rect.x, y - bar.rect.y);
      if (hit.strip != nullptr) return hit;
    }
  }
  return core::ui::StripCellHit{};
}

core::ui::Rect UiRenderer::strip_cell_rect(std::string_view strip, std::size_t index) const {
  for (const BarState& bar : bars_) {
    if (bar.rect.width <= 0 || bar.rect.height <= 0) continue;
    for (const core::ui::Layout& layout : bar.layouts) {
      const core::ui::Rect cell = core::ui::strip_cell_rect(layout, bar.content, strip, index);
      if (cell.width > 0) {
        return core::ui::Rect{bar.rect.x + cell.x, bar.rect.y + cell.y, cell.width, cell.height};
      }
    }
  }
  return core::ui::Rect{};
}

const core::ui::Widget* UiRenderer::bar_widget(std::string_view name) const noexcept {
  for (const BarState& bar : bars_) {
    for (const core::ui::Screen& screen : bar.screens) {
      if (const core::ui::Widget* widget = screen.find(name)) return widget;
    }
  }
  return nullptr;
}

bool UiRenderer::render(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
                        std::string* error) {
  if (device_ == nullptr || commands == nullptr || target == nullptr) return false;
  const auto queue = [&](BarState& bar) {
    if (bar.texture != nullptr && bar.canvas.width() != 0) {
      layers_.draw(bar.texture, bar.texture_width, bar.texture_height, bar.rect.x, bar.rect.y);
    }
  };
  // The editor's marks over the world, pixel for pixel. (The fog of war is
  // the map renderer's: it darkens the ground pixel by pixel and the world
  // view's sprites each at its own point, not a picture drawn here.)
  if (overlay_.canvas.width() != 0 && overlay_.canvas.height() != 0) {
    if (overlay_.dirty) {
      if (!upload(overlay_, error)) return false;
      overlay_.dirty = false;
    }
    layers_.draw(overlay_.texture, overlay_.texture_width, overlay_.texture_height, overlay_.rect.x,
                 overlay_.rect.y);
  }
  for (BarState& bar : bars_) {
    if (!bars_shown_ || bar.canvas.width() == 0 || bar.canvas.height() == 0) continue;
    if (bar.dirty) {
      composite(bar);
      if (!upload(bar, error)) return false;
    }
    queue(bar);
  }
  for (auto& open : dialogs_) {
    BarState& layer = open->layer;
    if (open->dialog->dirty() || layer.dirty) {
      open->dialog->paint(layer.canvas);
      if (!upload(layer, error)) return false;
      layer.dirty = false;
    }
    queue(layer);
  }
  // The messages and the tooltip composite themselves as they are set.
  if (messages_.canvas.width() != 0 && messages_.canvas.height() != 0) {
    if (messages_.dirty) {
      if (!upload(messages_, error)) return false;
      messages_.dirty = false;
    }
    queue(messages_);
  }
  if (tooltip_.canvas.width() != 0 && tooltip_.canvas.height() != 0) {
    if (tooltip_.dirty) {
      if (!upload(tooltip_, error)) return false;
      tooltip_.dirty = false;
    }
    queue(tooltip_);
  }
  return layers_.render(commands, target, width_, height_, error);
}

}  // namespace imperivm::platform
