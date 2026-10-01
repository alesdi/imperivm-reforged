#include "imperivm/core/ui/dialog.hpp"

#include <algorithm>
#include <cctype>

#include "imperivm/core/formats/ini.hpp"

namespace imperivm::core::ui {
namespace {

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool is_button(const Widget& widget) noexcept {
  return widget.kind == WidgetType::kButton || widget.kind == WidgetType::kImageButton ||
         widget.kind == WidgetType::kActiveButton || widget.kind == WidgetType::kSwitch;
}

bool is_combobox(const Widget& widget) noexcept {
  return widget.kind == WidgetType::kCombobox || widget.kind == WidgetType::kPlayerCombobox;
}

/// `Bufsize`, the most characters an edit holds; the files put 28 on a save
/// name and 512 on a chat line.
std::size_t buffer_size(const Widget& widget) noexcept {
  std::int32_t size = 0;
  if (widget.attribute_int("Bufsize", size) && size > 0) return static_cast<std::size_t>(size);
  return 256;
}

/// Whether typed text goes into the widget: an `EditW`, or a `Combobox`
/// with the `EDIT` style -- the sheet's `GRP_Combo`, where a group that
/// does not exist yet is typed rather than picked.
bool editable(const Widget& widget) noexcept {
  if (widget.kind == WidgetType::kEditW) return true;
  return widget.kind == WidgetType::kCombobox && has(widget.style, Style::kEdit);
}

/// The text an edit starts from the first time it is typed into: its own
/// `Text`, or, for a combobox, the item it shows. From then on the state's
/// text is the truth and a typed combobox no longer names an item.
void start_editing(const Widget& widget, WidgetState& state) {
  if (state.has_text) {
    if (widget.kind == WidgetType::kCombobox) state.selected = -1;
    return;
  }
  state.has_text = true;
  state.text = widget.text;
  if (widget.kind == WidgetType::kCombobox) {
    if (state.selected >= 0 && static_cast<std::size_t>(state.selected) < state.items.size()) {
      state.text = state.items[static_cast<std::size_t>(state.selected)];
    }
    state.selected = -1;
  }
}

}  // namespace

Dialog::Dialog(Screen screen, ResourceCache& cache)
    : screen_(std::move(screen)), cache_(&cache) {
  const std::int32_t width = std::max(1, screen_.design.width);
  const std::int32_t height = std::max(1, screen_.design.height);
  layout_ = layout_screen(screen_, width, height);
  fit_to_images();
  rect_ = Rect{0, 0, width, height};
  if (!screen_.focus.empty() && screen_.find(screen_.focus) != nullptr) {
    content_.focused = screen_.focus;
  }
  for (const Widget& widget : screen_.widgets) {
    const std::string_view font = widget.attribute("Font");
    if (!font.empty()) {
      content_.default_font.assign(font);
      break;
    }
  }
  // The editor's screens draw black on parchment; see `default_ink`.
  std::string folded = screen_.path;
  for (char& c : folded) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (folded.find("/editor/") != std::string::npos) content_.default_ink = Color{0, 0, 0, 255};
  // The toolkit's constructor selects tab 1 (0x0066d5f0), which hides the
  // widgets of every other tab and touches nothing on a screen without any.
  set_tab(1);
}

void Dialog::place(std::int32_t x, std::int32_t y) {
  rect_.x = x;
  rect_.y = y;
}

void Dialog::resize(std::int32_t width, std::int32_t height) {
  rect_.width = std::max(1, width);
  rect_.height = std::max(1, height);
  layout_ = layout_screen(screen_, rect_.width, rect_.height);
  fit_to_images();
  dirty_ = true;
}

/// A widget authored with a zero width or height takes its bitmap's cell:
/// the editor's `AdvDiplomacy.ini` places its name buttons and toggles at
/// `RectWH = x, y, 0, 0` and the toolkit sizes them from `but 200pix.bmp`
/// and `cf.bmp`. **Reading, labelled:** the toolkit's rule was not read;
/// a rectangle of no size can mean nothing else.
void Dialog::fit_to_images() {
  for (LaidOutWidget& placed : layout_.widgets) {
    if (placed.widget == nullptr || (placed.rect.width > 0 && placed.rect.height > 0)) continue;
    const Widget& widget = *placed.widget;
    if (!widget.has_rect || widget.image.empty()) continue;
    const Image* image = cache_->image(widget.image);
    if (image == nullptr) continue;
    std::uint32_t columns = std::max(1u, image_type_frames(widget.image_type));
    if (widget.xframes > 0) columns = static_cast<std::uint32_t>(widget.xframes);
    std::uint32_t rows = static_cast<std::uint32_t>(std::max(1, widget.rows));
    if (widget.yframes > 0) rows = static_cast<std::uint32_t>(widget.yframes);
    if (placed.rect.width <= 0) placed.rect.width = static_cast<std::int32_t>(image->width / columns);
    if (placed.rect.height <= 0) placed.rect.height = static_cast<std::int32_t>(image->height / rows);
  }
}

// -- state --------------------------------------------------------------------

void Dialog::set_text(std::string_view widget, std::string_view text) {
  WidgetState& state = content_.state(widget);
  state.has_text = true;
  state.text.assign(text);
  state.caret = -1;
  dirty_ = true;
}

std::string Dialog::text(std::string_view widget) const {
  if (const WidgetState* state = content_.state_of(widget); state != nullptr && state->has_text) {
    return state->text;
  }
  if (const Widget* declared = screen_.find(widget)) return declared->text;
  return std::string();
}

void Dialog::set_enabled(std::string_view widget, bool enabled) {
  content_.state(widget).disabled = !enabled;
  dirty_ = true;
}

void Dialog::set_hidden(std::string_view widget, bool hidden) {
  WidgetState& state = content_.state(widget);
  state.has_hidden = true;
  state.hidden = hidden;
  dirty_ = true;
}

void Dialog::set_row(std::string_view widget, std::int32_t row) {
  content_.state(widget).row = row;
  dirty_ = true;
}

void Dialog::set_tab(std::int32_t tab) {
  // 0x0066d2a0: every widget whose id carries a tab byte is shown for its
  // own tab and hidden for the rest, through the same `HIDDEN`/`INACTIVE`
  // pair a script would set; the tab buttons (`0x0003xxxx`) show their
  // pressed row for the current tab.
  tab_ = tab;
  for (const Widget& widget : screen_.widgets) {
    if (!widget.has_id) continue;
    const std::uint32_t id = static_cast<std::uint32_t>(widget.id);
    if ((id & 0xff0000u) == 0x30000u) {
      content_.state(widget.name).row = static_cast<std::int32_t>(id & 0xffffu) == tab ? 1 : 0;
      continue;
    }
    const std::int32_t owner = static_cast<std::int32_t>(id >> 24);
    if (owner == 0) continue;
    WidgetState& state = content_.state(widget.name);
    state.has_hidden = true;
    state.hidden = owner != tab;
  }
  dirty_ = true;
}

void Dialog::set_items(std::string_view widget, std::vector<std::string> items) {
  WidgetState& state = content_.state(widget);
  state.items = std::move(items);
  if (state.selected >= static_cast<std::int32_t>(state.items.size())) state.selected = -1;
  state.scroll = 0;
  dirty_ = true;
}

void Dialog::select(std::string_view widget, std::int32_t index) {
  WidgetState& state = content_.state(widget);
  if (index < 0 || static_cast<std::size_t>(index) >= state.items.size()) {
    state.selected = -1;
  } else {
    state.selected = index;
    // A combobox shows its choice again, whatever was typed into it.
    if (const Widget* declared = screen_.find(widget); declared != nullptr && is_combobox(*declared)) {
      state.has_text = false;
    }
    // Scrolled into view.
    if (const Widget* declared = screen_.find(widget)) {
      if (index < state.scroll) state.scroll = index;
      const Rect rect = widget_rect(widget);
      while (state.scroll < index &&
             index >= state.scroll + list_visible_rows(*cache_, *declared, rect, &state)) {
        ++state.scroll;
      }
    }
  }
  dirty_ = true;
}

void Dialog::set_value(std::string_view widget, std::int32_t value) {
  content_.state(widget).value = std::clamp(value, 0, 100);
  dirty_ = true;
}

std::int32_t Dialog::value(std::string_view widget) const {
  const WidgetState* state = content_.state_of(widget);
  return state != nullptr ? state->value : 0;
}

bool Dialog::slide(const Widget& widget, std::int32_t local_x, std::int32_t local_y) {
  const Rect rect = widget_rect(widget.name);
  const bool vertical = has(widget.style, Style::kVScroll);
  // The thumb's size along the run, from its bitmap: a third of the strip.
  std::int32_t thumb = 18;
  if (const Image* image = cache_->image(widget.attribute_image("Thumb"))) {
    const std::uint32_t columns = std::max(1u, image_type_frames(widget.image_type));
    thumb = vertical ? static_cast<std::int32_t>(image->height)
                     : static_cast<std::int32_t>(image->width / columns);
  }
  const std::int32_t run = std::max(1, (vertical ? rect.height : rect.width) - thumb);
  const std::int32_t along = (vertical ? local_y - rect.y : local_x - rect.x) - thumb / 2;
  const std::int32_t value = std::clamp(along * 100 / run, 0, 100);
  WidgetState& state = content_.state(widget.name);
  if (state.value == value) return false;
  state.value = value;
  dirty_ = true;
  return true;
}

std::int32_t Dialog::selected(std::string_view widget) const {
  const WidgetState* state = content_.state_of(widget);
  return state != nullptr ? state->selected : -1;
}

void Dialog::focus(std::string_view widget) {
  content_.focused.assign(widget);
  dirty_ = true;
}

const Widget* Dialog::widget_with_id(std::int32_t id) const noexcept {
  for (const Widget& widget : screen_.widgets) {
    if (widget.has_id && widget.id == id) return &widget;
  }
  return nullptr;
}

// -- geometry -----------------------------------------------------------------

Rect Dialog::widget_rect(std::string_view widget) const {
  for (const LaidOutWidget& placed : layout_.widgets) {
    if (placed.widget != nullptr && iequal(placed.widget->name, widget)) return placed.rect;
  }
  return Rect{};
}

const Widget* Dialog::bitmap_widget_at(std::int32_t x, std::int32_t y) const {
  const std::int32_t local_x = x - rect_.x;
  const std::int32_t local_y = y - rect_.y;
  for (std::size_t i = layout_.widgets.size(); i-- > 0;) {
    const LaidOutWidget& placed = layout_.widgets[i];
    if (placed.widget == nullptr) continue;
    const WidgetState* state = content_.state_of(placed.widget->name);
    if (state == nullptr || state->bitmap == nullptr) continue;
    if (state->has_hidden ? state->hidden : has(placed.widget->style, Style::kHidden)) continue;
    const Rect& rect = placed.rect;
    if (local_x < rect.x || local_y < rect.y || local_x >= rect.right() || local_y >= rect.bottom()) continue;
    return placed.widget;
  }
  return nullptr;
}

Rect Dialog::item_rect(std::string_view widget, std::int32_t index) const {
  const Widget* found = screen_.find(widget);
  const WidgetState* state = content_.state_of(widget);
  if (found == nullptr || state == nullptr || index < 0) return Rect{};
  const Rect rect = widget_rect(widget);
  if (is_combobox(*found)) {
    // A dropped combobox's rows, below the closed box, as `mouse_down`
    // reads them; a closed one has no rows to aim at.
    if (!state->open || static_cast<std::size_t>(index) >= state->items.size()) return Rect{};
    const Font* font = nullptr;
    if (const std::string_view path = found->attribute("Font"); !path.empty()) font = cache_->font(path);
    const std::int32_t row_height = font != nullptr ? font->height() + 2 : 16;
    const std::int32_t closed = combobox_closed_height(*cache_, *found);
    const std::int32_t row = index - state->scroll;
    if (row < 0) return Rect{};
    return Rect{rect.x, rect.y + closed + 1 + row * row_height, rect.width, row_height};
  }
  if (!acts_as_list(*found, state)) return Rect{};
  std::int32_t top = rect.y;
  for (std::size_t i = static_cast<std::size_t>(std::max(0, state->scroll)); i < state->items.size(); ++i) {
    const std::int32_t height = list_item_height(*cache_, *found, *state, i, rect.width);
    if (top + height > rect.bottom()) break;
    if (static_cast<std::int32_t>(i) == index) return Rect{rect.x, top, rect.width, height};
    top += height;
  }
  return Rect{};
}

bool Dialog::takes_input(const Widget& widget) const noexcept {
  if (has(widget.style, Style::kInactive)) return false;
  bool hidden = has(widget.style, Style::kHidden);
  if (const WidgetState* state = content_.state_of(widget.name)) {
    if (state->has_hidden) hidden = state->hidden;
    if (state->disabled) return false;
  }
  if (hidden) return false;
  if (has(widget.style, Style::kDisabled)) return false;
  switch (widget.kind) {
    case WidgetType::kButton:
    case WidgetType::kImageButton:
    case WidgetType::kActiveButton:
    case WidgetType::kSwitch:
    case WidgetType::kList:
    case WidgetType::kEditW:
    case WidgetType::kCombobox:
    case WidgetType::kPlayerCombobox:
    case WidgetType::kScroll:
    case WidgetType::kConquestMap:
    case WidgetType::kSpin:
    case WidgetType::kBrushSize:
      return true;
    case WidgetType::kControl:
      return acts_as_list(widget, content_.state_of(widget.name));
    default:
      return false;
  }
}

const Widget* Dialog::widget_at(std::int32_t x, std::int32_t y) const {
  const std::int32_t local_x = x - rect_.x;
  const std::int32_t local_y = y - rect_.y;
  // An open combobox's list lies over whatever is declared after it, and
  // is drawn last; it takes the hit first.
  for (int pass = 0; pass < 2; ++pass)
  for (std::size_t i = layout_.widgets.size(); i-- > 0;) {
    const LaidOutWidget& placed = layout_.widgets[i];
    const Widget& widget = *placed.widget;
    if (!takes_input(widget)) continue;
    if (pass == 0) {
      const WidgetState* state = content_.state_of(widget.name);
      if (!is_combobox(widget) || state == nullptr || !state->open) continue;
    }
    Rect rect = placed.rect;
    if (is_combobox(widget)) {
      // Closed, only the box; open, the list below it too -- as tall as its
      // items under `AUTOSIZE`.
      const WidgetState* state = content_.state_of(widget.name);
      const std::int32_t closed = combobox_closed_height(*cache_, widget);
      if (state == nullptr || !state->open) {
        rect.height = closed;
      } else if (has(widget.style, Style::kAutosize)) {
        const Font* font = nullptr;
        if (const std::string_view path = widget.attribute("Font"); !path.empty()) font = cache_->font(path);
        const std::int32_t row_height = font != nullptr ? font->height() + 2 : 16;
        rect.height = closed + static_cast<std::int32_t>(state->items.size()) * row_height + 2;
      }
    }
    if (local_x < rect.x || local_x >= rect.right()) continue;
    if (local_y < rect.y || local_y >= rect.bottom()) continue;
    return &widget;
  }
  return nullptr;
}

// -- input ----------------------------------------------------------------------

DialogEvent Dialog::mouse_move(std::int32_t x, std::int32_t y) {
  DialogEvent event;
  const Widget* over = widget_at(x, y);
  const std::string name = over != nullptr ? over->name : std::string();
  if (name != content_.hovered) {
    content_.hovered = name;
    dirty_ = true;
  }
  // A slider being dragged follows the pointer wherever it goes.
  if (!content_.pressed.empty()) {
    if (const Widget* held = screen_.find(content_.pressed);
        held != nullptr && held->kind == WidgetType::kScroll && held->attribute("TargetId").empty()) {
      if (slide(*held, x - rect_.x, y - rect_.y)) {
        event.kind = DialogEvent::Kind::kChange;
        event.widget = held->name;
        event.index = content_.state(held->name).value;
      }
    }
  }
  return event;
}

DialogEvent Dialog::mouse_down(std::int32_t x, std::int32_t y) {
  DialogEvent event;
  const Widget* hit = widget_at(x, y);
  // A click outside an open combobox closes it.
  for (WidgetState& state : content_.states) {
    if (!state.open) continue;
    if (hit == nullptr || !iequal(hit->name, state.name)) {
      state.open = false;
      dirty_ = true;
    }
  }
  const std::int32_t local_x = x - rect_.x;
  const std::int32_t local_y = y - rect_.y;
  if (hit == nullptr) {
    // A widget carrying a composed picture takes the click whatever its
    // style, the point local to it: the *Place Label* screen's minimap is
    // an `INACTIVE` button the code hangs the picture on.
    if (const Widget* picture = bitmap_widget_at(x, y); picture != nullptr) {
      const Rect rect = widget_rect(picture->name);
      event.kind = DialogEvent::Kind::kClick;
      event.widget = picture->name;
      event.x = local_x - rect.x;
      event.y = local_y - rect.y;
      return event;
    }
    // The backdrop, when there is one and the point is on the canvas.
    if (content_.backdrop != nullptr && local_x >= 0 && local_y >= 0 && local_x < rect_.width &&
        local_y < rect_.height) {
      event.kind = DialogEvent::Kind::kClick;
      event.x = local_x;
      event.y = local_y;
    }
    return event;
  }
  const Rect rect = widget_rect(hit->name);

  if (is_button(*hit)) {
    content_.pressed = hit->name;
    dirty_ = true;
    return event;
  }
  if (hit->kind == WidgetType::kScroll && hit->attribute("TargetId").empty()) {
    content_.pressed = hit->name;
    (void)slide(*hit, local_x, local_y);
    event.kind = DialogEvent::Kind::kChange;
    event.widget = hit->name;
    event.index = content_.state(hit->name).value;
    return event;
  }
  if (hit->kind == WidgetType::kConquestMap) {
    event.kind = DialogEvent::Kind::kClick;
    event.widget = hit->name;
    event.x = local_x - rect.x;
    event.y = local_y - rect.y;
    return event;
  }
  if (hit->kind == WidgetType::kBrushSize) {
    // The column under the pointer names a frame; the widget's value is the
    // frame, and the tools read it as a brush size.
    const Image* image = cache_->image(hit->image);
    const std::int32_t item_width = brush_item_width(*hit);
    const std::uint32_t columns =
        image != nullptr ? std::max(1u, image->width / static_cast<std::uint32_t>(item_width)) : 6u;
    const std::vector<std::int32_t> frames = brush_frames(*hit, columns);
    const std::int32_t column = (local_x - rect.x) / item_width;
    if (column < 0 || static_cast<std::size_t>(column) >= frames.size()) return event;
    WidgetState& state = content_.state(hit->name);
    if (state.value == frames[static_cast<std::size_t>(column)]) return event;
    state.value = frames[static_cast<std::size_t>(column)];
    dirty_ = true;
    event.kind = DialogEvent::Kind::kChange;
    event.widget = hit->name;
    event.index = state.value;
    return event;
  }
  if (hit->kind == WidgetType::kSpin) {
    // The upper half counts its `TargetId` edit up by one, the lower half
    // down; the change is reported as the edit's, so a caller listening to
    // the edit hears the spin too. Held while the button is down, for the
    // pressed picture.
    const std::int32_t delta = local_y - rect.y < rect.height / 2 ? 1 : -1;
    content_.pressed = hit->name;
    content_.state(hit->name).value = delta;
    dirty_ = true;
    const std::string_view target = hit->attribute("TargetId");
    const Widget* edit = target.empty() ? nullptr : screen_.find(target);
    if (edit == nullptr) return event;
    const std::string current = text(edit->name);
    std::int32_t number = 0;
    (void)parse_int(current, number);
    set_text(edit->name, std::to_string(number + delta));
    event.kind = DialogEvent::Kind::kChange;
    event.widget = edit->name;
    event.index = number + delta;
    return event;
  }
  if (hit->kind == WidgetType::kEditW) {
    focus(hit->name);
    WidgetState& state = content_.state(hit->name);
    if (!state.has_text) {
      state.has_text = true;
      state.text = hit->text;
    }
    state.caret = -1;
    return event;
  }
  if (acts_as_list(*hit, content_.state_of(hit->name))) {
    focus(hit->name);
    WidgetState& state = content_.state(hit->name);
    const std::int32_t row = list_item_at(*cache_, *hit, rect, state, local_y);
    if (row < 0 || static_cast<std::size_t>(row) >= state.items.size()) return event;
    event.widget = hit->name;
    event.index = row;
    event.kind = row == state.selected ? DialogEvent::Kind::kActivate : DialogEvent::Kind::kSelect;
    state.selected = row;
    dirty_ = true;
    return event;
  }
  if (is_combobox(*hit)) {
    WidgetState& state = content_.state(hit->name);
    const std::int32_t closed = combobox_closed_height(*cache_, *hit);
    if (state.open && local_y >= rect.y + closed) {
      // A row of the dropped list.
      const Font* font = nullptr;
      if (const std::string_view path = hit->attribute("Font"); !path.empty()) font = cache_->font(path);
      const std::int32_t row_height = font != nullptr ? font->height() + 2 : 16;
      const std::int32_t row = state.scroll + (local_y - rect.y - closed - 1) / row_height;
      const std::int32_t rows = has(hit->style, Style::kAutosize)
                                    ? static_cast<std::int32_t>(state.items.size())
                                    : std::max(1, (rect.height - closed) / row_height);
      if (row >= 0 && row < rows && static_cast<std::size_t>(row) < state.items.size()) {
        state.selected = row;
        state.has_text = false;
        event.kind = DialogEvent::Kind::kChange;
        event.widget = hit->name;
        event.index = row;
      }
      state.open = false;
    } else {
      state.open = !state.open && !state.items.empty();
    }
    focus(hit->name);
    dirty_ = true;
    return event;
  }
  return event;
}

DialogEvent Dialog::mouse_up(std::int32_t x, std::int32_t y) {
  DialogEvent event;
  if (content_.pressed.empty()) return event;
  const std::string name = content_.pressed;
  content_.pressed.clear();
  dirty_ = true;
  if (const Widget* held = screen_.find(name);
      held != nullptr && (held->kind == WidgetType::kScroll || held->kind == WidgetType::kSpin)) {
    return event;  // a slider's drag ended, or a spin button came up
  }
  const Widget* over = widget_at(x, y);
  if (over == nullptr || !iequal(over->name, name)) return event;
  return press(*over);
}

DialogEvent Dialog::press(const Widget& widget) {
  DialogEvent event;
  if (has(widget.style, Style::kToggle) || has(widget.style, Style::kTristate)) {
    WidgetState& state = content_.state(widget.name);
    const std::int32_t current = state.row >= 0 ? state.row : widget.initial_row;
    state.row = current == 0 ? 1 : 0;
    dirty_ = true;
  }
  // A scrollbar's arrow: some `Scroll` names this button as its `BackID` or
  // `ForwardID`, and that scrollbar's `TargetId` is the list to move.
  for (const Widget& scroll : screen_.widgets) {
    if (scroll.kind != WidgetType::kScroll) continue;
    if (iequal(scroll.attribute("BackID"), widget.name)) {
      scroll_list(scroll.attribute("TargetId"), -1);
    } else if (iequal(scroll.attribute("ForwardID"), widget.name)) {
      scroll_list(scroll.attribute("TargetId"), 1);
    }
  }
  if (widget.has_id) {
    // The toolkit's two id-keyed rules (see the class comment): a tab
    // button switches the tab, a radio button clears its group.
    const std::uint32_t id = static_cast<std::uint32_t>(widget.id);
    if ((id & 0xff0000u) == 0x30000u) {
      set_tab(static_cast<std::int32_t>(id & 0xffffu));
    } else if ((id & 0xff0000u) == 0x20000u) {
      for (const Widget& other : screen_.widgets) {
        if (!other.has_id) continue;
        const std::uint32_t other_id = static_cast<std::uint32_t>(other.id);
        if ((other_id & 0xff0000u) != 0x20000u || (other_id & ~0xffu) != (id & ~0xffu)) continue;
        content_.state(other.name).row = &other == &widget ? 1 : 0;
      }
      dirty_ = true;
    }
    event.kind = DialogEvent::Kind::kCommand;
    event.id = widget.id;
    event.widget = widget.name;
  }
  return event;
}

DialogEvent Dialog::press_named(std::string_view name) {
  const Widget* widget = screen_.find(name);
  if (widget == nullptr || !takes_input(*widget)) return DialogEvent{};
  return press(*widget);
}

void Dialog::scroll_list(std::string_view list, std::int32_t by) {
  const Widget* widget = screen_.find(list);
  if (widget == nullptr) return;
  WidgetState& state = content_.state(list);
  // The last scroll position is the one from which the last item shows.
  const Rect rect = widget_rect(list);
  std::int32_t last = 0;
  const std::int32_t count = static_cast<std::int32_t>(state.items.size());
  for (std::int32_t from = count - 1; from >= 0; --from) {
    WidgetState probe = state;
    probe.scroll = from;
    if (from + list_visible_rows(*cache_, *widget, rect, &probe) < count) break;
    last = from;
  }
  state.scroll = std::clamp(state.scroll + by, 0, last);
  dirty_ = true;
}

DialogEvent Dialog::key(DialogKey key) {
  DialogEvent event;
  const Widget* focused = content_.focused.empty() ? nullptr : screen_.find(content_.focused);

  if (key == DialogKey::kEscape) {
    if (!screen_.escape.empty()) {
      event = press_named(screen_.escape);
      if (event.kind != DialogEvent::Kind::kNone) return event;
    }
    event.kind = DialogEvent::Kind::kEscape;
    return event;
  }
  if (key == DialogKey::kEnter && focused != nullptr && focused->kind == WidgetType::kEditW &&
      has(focused->style, Style::kMultiline)) {
    // A `MULTILINE` edit takes the line break itself: the editor's script
    // and description edits.
    WidgetState& state = content_.state(focused->name);
    start_editing(*focused, state);
    std::size_t caret = state.caret >= 0 ? std::min(state.text.size(), static_cast<std::size_t>(state.caret))
                                         : state.text.size();
    if (state.text.size() + 1 < buffer_size(*focused)) {
      state.text.insert(caret, 1, '\n');
      ++caret;
      event.kind = DialogEvent::Kind::kChange;
      event.widget = focused->name;
    }
    state.caret = caret == state.text.size() ? -1 : static_cast<std::int32_t>(caret);
    dirty_ = true;
    return event;
  }
  if (key == DialogKey::kEnter) {
    if (!screen_.enter.empty()) {
      event = press_named(screen_.enter);
      if (event.kind != DialogEvent::Kind::kNone) return event;
    }
    if (focused != nullptr && acts_as_list(*focused, content_.state_of(focused->name))) {
      const WidgetState* state = content_.state_of(focused->name);
      if (state != nullptr && state->selected >= 0) {
        event.kind = DialogEvent::Kind::kActivate;
        event.widget = focused->name;
        event.index = state->selected;
      }
    }
    return event;
  }
  if (key == DialogKey::kTab) {
    // The next `TABSTOP` widget after the focused one, wrapping.
    std::vector<const Widget*> stops;
    for (const Widget& widget : screen_.widgets) {
      if (has(widget.style, Style::kTabStop) && takes_input(widget)) stops.push_back(&widget);
    }
    if (stops.empty()) return event;
    std::size_t next = 0;
    for (std::size_t i = 0; i < stops.size(); ++i) {
      if (focused != nullptr && iequal(stops[i]->name, focused->name)) next = (i + 1) % stops.size();
    }
    focus(stops[next]->name);
    return event;
  }
  if (focused == nullptr) return event;

  if (editable(*focused)) {
    WidgetState& state = content_.state(focused->name);
    start_editing(*focused, state);
    std::size_t caret = state.caret >= 0 ? std::min(state.text.size(), static_cast<std::size_t>(state.caret))
                                         : state.text.size();
    switch (key) {
      case DialogKey::kBackspace:
        if (caret > 0) {
          state.text.erase(caret - 1, 1);
          --caret;
          event.kind = DialogEvent::Kind::kChange;
        }
        break;
      case DialogKey::kDelete:
        if (caret < state.text.size()) {
          state.text.erase(caret, 1);
          event.kind = DialogEvent::Kind::kChange;
        }
        break;
      case DialogKey::kLeft: if (caret > 0) --caret; break;
      case DialogKey::kRight: if (caret < state.text.size()) ++caret; break;
      case DialogKey::kHome: caret = 0; break;
      case DialogKey::kEnd: caret = state.text.size(); break;
      default: break;
    }
    state.caret = caret == state.text.size() ? -1 : static_cast<std::int32_t>(caret);
    event.widget = focused->name;
    dirty_ = true;
    return event;
  }
  if (acts_as_list(*focused, content_.state_of(focused->name))) {
    WidgetState& state = content_.state(focused->name);
    if (state.items.empty()) return event;
    const std::int32_t last = static_cast<std::int32_t>(state.items.size()) - 1;
    const std::int32_t visible = std::max(1, list_visible_rows(*cache_, *focused, widget_rect(focused->name), &state));
    std::int32_t index = state.selected;
    switch (key) {
      case DialogKey::kUp: index = std::max(0, index - 1); break;
      case DialogKey::kDown: index = std::min(last, index + 1); break;
      case DialogKey::kPageUp: index = std::max(0, index - visible); break;
      case DialogKey::kPageDown: index = std::min(last, index + visible); break;
      case DialogKey::kHome: index = 0; break;
      case DialogKey::kEnd: index = last; break;
      default: return event;
    }
    if (index != state.selected) {
      select(focused->name, index);
      event.kind = DialogEvent::Kind::kSelect;
      event.widget = focused->name;
      event.index = index;
    }
    return event;
  }
  return event;
}

DialogEvent Dialog::text_input(std::string_view text) {
  DialogEvent event;
  const Widget* focused = content_.focused.empty() ? nullptr : screen_.find(content_.focused);
  if (focused == nullptr || !editable(*focused) || text.empty()) return event;
  WidgetState& state = content_.state(focused->name);
  start_editing(*focused, state);
  const std::size_t limit = buffer_size(*focused);
  std::size_t caret = state.caret >= 0 ? std::min(state.text.size(), static_cast<std::size_t>(state.caret))
                                       : state.text.size();
  bool changed = false;
  for (const char c : text) {
    if (has(focused->style, Style::kNumber) && (c < '0' || c > '9') && c != '-') continue;
    if (static_cast<unsigned char>(c) < 0x20) continue;
    if (state.text.size() + 1 >= limit) break;
    state.text.insert(caret, 1, c);
    ++caret;
    changed = true;
  }
  state.caret = caret == state.text.size() ? -1 : static_cast<std::int32_t>(caret);
  if (changed) {
    event.kind = DialogEvent::Kind::kChange;
    event.widget = focused->name;
    dirty_ = true;
  }
  return event;
}

void Dialog::paint(Canvas& canvas) {
  const std::uint32_t width = static_cast<std::uint32_t>(std::max(1, rect_.width));
  const std::uint32_t height = static_cast<std::uint32_t>(std::max(1, rect_.height));
  if (canvas.width() != width || canvas.height() != height) canvas.resize(width, height);
  canvas.clear(Color{0, 0, 0, 0});
  paint_dialog(canvas, layout_, *cache_, content_);
  dirty_ = false;
}

}  // namespace imperivm::core::ui
