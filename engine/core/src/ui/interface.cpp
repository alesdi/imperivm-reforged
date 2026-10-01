#include "imperivm/core/ui/interface.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <cstdlib>
#include <memory>
#include <utility>

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

std::string fold(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(lower(c));
  return out;
}

std::string_view trim_view(std::string_view text) noexcept { return trim(text); }

/// The virtual roots. Each pair is pinned by one file being named both ways in
/// the retail data; see the header.
struct Alias {
  std::string_view prefix;
  std::string_view target;
};
constexpr std::array<Alias, 8> kAliases{{
    {"gameres/", "ui/"},
    {"gameini/", "data/interface/"},
    {"menures/", "ui/menu/"},
    {"menuini/", "data/interface/menu/"},
    {"editorres/", "ui/editor/"},
    {"editorini/", "data/interface/editor/"},
    {"commonini/", "data/interface/common/"},
    // Not a string in `gbr.exe`; pinned by the data alone: `COMMON/TEMPLATE.INI`
    // names `commonres/exitbtn.bmp`, `commonres/ScrHndl.BMP`, `UpArrow` and
    // `DownArrow`, and `UI.pak` holds all four under `UI\COMMON\`.
    {"commonres/", "ui/common/"},
}};

}  // namespace

std::string translation_context(std::string_view screen_path, std::string_view widget,
                                std::string_view attribute) {
  std::string path = resolve_alias(screen_path);
  constexpr std::string_view kRoot = "data/interface";
  if (path.size() >= kRoot.size() && iequal(std::string_view{path}.substr(0, kRoot.size()), kRoot)) {
    path.erase(0, kRoot.size());
  }
  if (path.empty() || path.front() != '/') path.insert(path.begin(), '/');
  std::string out = path;
  out.push_back(':');
  out.append(widget);
  out.push_back(':');
  out.append(attribute);
  return out;
}

std::string resolve_alias(std::string_view path) {
  std::string out;
  out.reserve(path.size() + 8);
  for (const char c : path) out.push_back(c == '\\' ? '/' : c);
  for (const auto& alias : kAliases) {
    if (out.size() < alias.prefix.size()) continue;
    if (iequal(std::string_view{out}.substr(0, alias.prefix.size()), alias.prefix)) {
      return std::string{alias.target} + out.substr(alias.prefix.size());
    }
  }
  return out;
}

// -- ParamScope -------------------------------------------------------------

void ParamScope::set(std::string_view name, std::string_view value) {
  const std::string key = fold(trim_view(name));
  for (auto& entry : entries_) {
    if (entry.key == key) {
      entry.value.assign(value);
      return;
    }
  }
  entries_.push_back(Entry{key, std::string{value}});
}

void ParamScope::set_default(std::string_view name, std::string_view value) {
  if (find(name) != nullptr) return;
  entries_.push_back(Entry{fold(trim_view(name)), std::string{value}});
}

const std::string* ParamScope::find(std::string_view name) const noexcept {
  const std::string key = fold(trim_view(name));
  for (const auto& entry : entries_) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

namespace {

std::string expand_into(const ParamScope& scope, std::string_view text, int depth) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '%') {
      out.push_back(text[i++]);
      continue;
    }
    // `%%` is a literal per cent. No shipped value needs it; a modded one might,
    // and without the escape there would be no way to write one at all.
    if (i + 1 < text.size() && text[i + 1] == '%') {
      out.push_back('%');
      i += 2;
      continue;
    }
    const std::size_t close = text.find('%', i + 1);
    if (close == std::string_view::npos) {  // an unpaired marker is literal text
      out.append(text.substr(i));
      break;
    }
    const std::string_view name = text.substr(i + 1, close - i - 1);
    const std::string* bound = scope.find(name);
    if (bound == nullptr || depth >= ParamScope::kMaxDepth) {
      // Left as written. A parameter nobody defined shows up as `%Name%` in the
      // output, which names itself in a screenshot; expanding it to nothing
      // would leave a rectangle silently at the origin.
      out.append(text.substr(i, close - i + 1));
    } else {
      out.append(expand_into(scope, *bound, depth + 1));
    }
    i = close + 1;
  }
  return out;
}

}  // namespace

std::string ParamScope::expand(std::string_view text) const { return expand_into(*this, text, 0); }

// -- expressions ------------------------------------------------------------
//
// `+-*/%|&` is a literal string in gbr.exe, adjacent to the rect-parsing
// diagnostics, and the evaluator behind it (0x00667e10, the operator loop at
// 0x006681fc) is **flat**: it reads one operand, looks the next character up
// in that string, reads the next operand and applies the operator to the
// running value, left to right, with no precedence and no parentheses. A
// division by zero leaves 0. This used to follow C, on the reasoning that no
// shipped expression could tell the two apart -- one can: the editor's
// `AdvObjProps.ini` places its tab buttons at `#%TOR_GEN% - 1 * 92 +
// left(TabsFrame)#`, which under C precedence puts every tab 86 pixels off
// the left edge and under the flat rule lays them out 92 apart.

namespace {

class ExprParser {
 public:
  ExprParser(std::string_view text, const RectLookup& lookup) : text_(text), lookup_(lookup) {}

  bool parse(std::int32_t& out) {
    std::int32_t value = 0;
    if (!bitwise_or(value)) return false;
    skip_space();
    if (pos_ != text_.size()) return false;
    out = value;
    return true;
  }

 private:
  void skip_space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t')) ++pos_;
  }
  bool peek(char c) {
    skip_space();
    return pos_ < text_.size() && text_[pos_] == c;
  }
  bool eat(char c) {
    if (!peek(c)) return false;
    ++pos_;
    return true;
  }

  /// The flat chain: `operand (op operand)*`, applied as it is read.
  bool bitwise_or(std::int32_t& out) {
    if (!unary(out)) return false;
    for (;;) {
      skip_space();
      if (pos_ >= text_.size()) return true;
      const char op = text_[pos_];
      if (op != '+' && op != '-' && op != '*' && op != '/' && op != '%' && op != '|' && op != '&') return true;
      ++pos_;
      std::int32_t rhs = 0;
      if (!unary(rhs)) return false;
      switch (op) {
        case '+': out += rhs; break;
        case '-': out -= rhs; break;
        case '*': out *= rhs; break;
        case '/': out = rhs == 0 ? 0 : out / rhs; break;
        case '%': out = rhs == 0 ? 0 : out % rhs; break;
        case '|': out |= rhs; break;
        default: out &= rhs; break;
      }
    }
  }
  bool unary(std::int32_t& out) {
    skip_space();
    if (eat('-')) {
      if (!unary(out)) return false;
      out = -out;
      return true;
    }
    if (eat('+')) return unary(out);
    return primary(out);
  }
  bool primary(std::int32_t& out) {
    skip_space();
    if (pos_ >= text_.size()) return false;
    if (eat('(')) {
      if (!bitwise_or(out)) return false;
      return eat(')');
    }
    const char c = text_[pos_];
    if (c >= '0' && c <= '9') return number(out);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') return call(out);
    return false;
  }
  bool number(std::int32_t& out) {
    const std::size_t start = pos_;
    int base = 10;
    if (text_.compare(pos_, 2, "0x") == 0 || text_.compare(pos_, 2, "0X") == 0) {
      base = 16;
      pos_ += 2;
    }
    std::int64_t value = 0;
    std::size_t digits = 0;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (base == 16 && c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (base == 16 && c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      }
      if (digit < 0 || digit >= base) break;
      value = value * base + digit;
      if (value > 0x7FFFFFFFLL) return false;
      ++pos_;
      ++digits;
    }
    if (digits == 0) {
      pos_ = start;
      return false;
    }
    out = static_cast<std::int32_t>(value);
    return true;
  }
  bool call(std::int32_t& out) {
    const std::size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      const bool word = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
      if (!word) break;
      ++pos_;
    }
    const std::string_view name = text_.substr(start, pos_ - start);
    // A bare word is a parameter: `MPGAMEMENU.INI` writes `#PlayerNameWidth#`
    // for a name its Params section defines, without the `%` marks. Asked of
    // the lookup with an empty function, which the screen loader answers from
    // its scope.
    skip_space();
    if (pos_ >= text_.size() || text_[pos_] != '(') {
      return lookup_ && lookup_(std::string_view{}, name, out);
    }
    (void)eat('(');
    const std::size_t arg_start = pos_;
    int depth = 1;
    while (pos_ < text_.size() && depth > 0) {
      if (text_[pos_] == '(') ++depth;
      if (text_[pos_] == ')') --depth;
      if (depth > 0) ++pos_;
    }
    if (depth != 0) return false;
    const std::string_view arg = trim_view(text_.substr(arg_start, pos_ - arg_start));
    ++pos_;  // the ')'
    if (!lookup_) return false;
    return lookup_(name, arg, out);
  }

  std::string_view text_;
  const RectLookup& lookup_;
  std::size_t pos_ = 0;
};

}  // namespace

bool evaluate_expression(std::string_view text, const RectLookup& lookup, std::int32_t& out) {
  ExprParser parser{text, lookup};
  return parser.parse(out);
}

std::string evaluate_expressions(std::string_view text, const RectLookup& lookup) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '#') {
      out.push_back(text[i++]);
      continue;
    }
    const std::size_t close = text.find('#', i + 1);
    if (close == std::string_view::npos) {
      out.append(text.substr(i));
      break;
    }
    const std::string_view body = text.substr(i + 1, close - i - 1);
    std::int32_t value = 0;
    if (evaluate_expression(body, lookup, value)) {
      out.append(std::to_string(value));
    } else {
      // Left verbatim, markers included, so that a failure is visible in the
      // parsed value rather than becoming a plausible zero.
      out.append(text.substr(i, close - i + 1));
    }
    i = close + 1;
  }
  return out;
}

// -- names ------------------------------------------------------------------

namespace {

struct NamedType {
  std::string_view name;
  WidgetType type;
};
constexpr std::array<NamedType, 38> kTypes{{
    {"Button", WidgetType::kButton},
    {"VXButton", WidgetType::kButton},
    {"Icon", WidgetType::kIcon},
    {"Thumbnail", WidgetType::kThumbnail},
    {"SelectionName", WidgetType::kSelectionName},
    {"SelectionHealth", WidgetType::kSelectionHealth},
    {"InfobarText", WidgetType::kInfobarText},
    {"InfobarIcon", WidgetType::kInfobarIcon},
    {"BuildingQueue", WidgetType::kBuildingQueue},
    {"UIHolder", WidgetType::kUIHolder},
    {"UIInventory", WidgetType::kUIInventory},
    {"Combiner", WidgetType::kCombiner},
    {"HeroSkills", WidgetType::kHeroSkills},
    {"UnitSpecials", WidgetType::kUnitSpecials},
    {"Switch", WidgetType::kSwitch},
    {"Dialog", WidgetType::kDialog},
    {"Window", WidgetType::kDialog},
    {"Control", WidgetType::kControl},
    {"Background", WidgetType::kBackground},
    {"DarkFrame", WidgetType::kDarkFrame},
    {"Frame", WidgetType::kFrame},
    {"ImageButton", WidgetType::kImageButton},
    {"TextW", WidgetType::kTextW},
    {"EditW", WidgetType::kEditW},
    {"List", WidgetType::kList},
    {"Scroll", WidgetType::kScroll},
    {"Combobox", WidgetType::kCombobox},
    {"PlayerCombobox", WidgetType::kPlayerCombobox},
    {"TextEx", WidgetType::kTextEx},
    {"VXMenuBack", WidgetType::kVXMenuBack},
    {"Spin", WidgetType::kSpin},
    {"SpinButton", WidgetType::kSpin},
    {"ActiveButton", WidgetType::kActiveButton},
    {"BmpScroll", WidgetType::kBmpScroll},
    {"BrushSize", WidgetType::kBrushSize},
    {"ConquestMap", WidgetType::kConquestMap},
    {"SelectionHealthText", WidgetType::kSelectionHealthText},
    {"", WidgetType::kUnknown},
}};

struct NamedStyle {
  std::string_view name;
  Style flag;
};
// Every name in the executable's eight style tables. The ones a class
// declares and this reader has no use for -- `SETCURSOR`, `COLORAWARE`,
// `MOVETOHEAD`, `ICONSONTOP` -- fold to `kNone` so that they are known and
// not reported, and are still kept by name in `unknown_styles`' sibling,
// the attribute list.
constexpr std::array<NamedStyle, 52> kStyles{{
    {"TRANSPARENT", Style::kTransparent}, {"INACTIVE", Style::kInactive},
    {"HIDDEN", Style::kHidden},           {"DISABLED", Style::kDisabled},
    {"ALIGN_LEFT", Style::kAlignLeft},    {"ALIGN_RIGHT", Style::kAlignRight},
    {"ALIGN_CENTER", Style::kAlignCenter},
    {"MODAL", Style::kModal},             {"TABSTOP", Style::kTabStop},
    {"MULTILINE", Style::kMultiline},     {"NUMBER", Style::kNumber},
    {"TRISTATE", Style::kTristate},       {"TOGGLE", Style::kToggle},
    {"AUTOSIZE", Style::kAutosize},       {"NOFOCUS", Style::kNoFocus},
    {"VSCROLL", Style::kVScroll},         {"HSCROLL", Style::kNone},
    {"AUTODISABLE", Style::kAutoDisable}, {"AUTOMOVE", Style::kAutoMove},
    {"TEXTONLY", Style::kTextOnly},       {"AUTOCALC", Style::kAutoCalc},
    {"ROWS", Style::kRows},               {"SINGLE", Style::kSingle},
    {"TIGHTSCROLL", Style::kTightScroll}, {"SECURE", Style::kSecure},
    {"NOWORDWRAP", Style::kNoWordWrap},   {"AUTOREPEAT", Style::kAutoRepeat},
    {"MULTISEL", Style::kMultiSel},       {"AUTOHIDE", Style::kAutoHide},
    {"EDIT", Style::kEdit},               {"NOLIST", Style::kNoList},
    {"SETCURSOR", Style::kNone},          {"FORCECURSOR", Style::kNone},
    {"COLORUPDATE", Style::kNone},        {"COLORAWARE", Style::kNone},
    {"MOVETOHEAD", Style::kNone},         {"DBLCLICK", Style::kNone},
    {"AUTOTEXTOFFSET", Style::kNone},     {"AUTOBUTTONSDISABLE", Style::kNone},
    {"FIXED", Style::kNone},              {"PARTIALITEM", Style::kNone},
    {"ICONONLY", Style::kNone},           {"SELIMAGE", Style::kNone},
    {"ICONSONTOP", Style::kNone},         {"TIGHTHSCROLL", Style::kNone},
    {"TIGHTVSCROLL", Style::kNone},       {"SELECTION", Style::kNone},
    {"CURSOR", Style::kNone},             {"CURSORVISIBLE", Style::kNone},
    {"UNDO", Style::kNone},               {"ALLOWTAB", Style::kNone},
    {"AUTOWIDTH", Style::kNone},
}};

struct NamedTag {
  std::string_view name;
  SelectionTag tag;
};
constexpr std::array<NamedTag, 8> kTags{{
    {"thumb", SelectionTag::kThumb},   {"empty", SelectionTag::kEmpty},
    {"building", SelectionTag::kBuilding}, {"unit", SelectionTag::kUnit},
    {"hero", SelectionTag::kHero},     {"holder", SelectionTag::kHolder},
    {"items", SelectionTag::kItems},   {"queue", SelectionTag::kQueue},
}};

struct NamedShow {
  std::string_view key;
  ShowFor flag;
};
constexpr std::array<NamedShow, 6> kShows{{
    {"ShowAll", ShowFor::kAll},         {"ShowControl", ShowFor::kControl},
    {"ShowVision", ShowFor::kVision},   {"ShowCover", ShowFor::kCover},
    {"ShowSupport", ShowFor::kSupport}, {"ShowCeaseFire", ShowFor::kCeaseFire},
}};

}  // namespace

WidgetType widget_type_from_name(std::string_view name) noexcept {
  for (const auto& entry : kTypes) {
    if (!entry.name.empty() && iequal(entry.name, name)) return entry.type;
  }
  return WidgetType::kUnknown;
}

std::string_view widget_type_name(WidgetType type) noexcept {
  for (const auto& entry : kTypes) {
    if (entry.type == type) return entry.name;
  }
  return {};
}

SelectionTag selection_tag_from_name(std::string_view name) noexcept {
  for (const auto& entry : kTags) {
    if (iequal(entry.name, name)) return entry.tag;
  }
  return SelectionTag::kNone;
}

// -- Widget -----------------------------------------------------------------

std::string_view Widget::attribute(std::string_view key, std::string_view fallback) const noexcept {
  for (const auto& entry : attributes) {
    if (iequal(entry.key, key)) return entry.value;
  }
  return fallback;
}

bool Widget::attribute_int(std::string_view key, std::int32_t& out) const noexcept {
  const std::string_view text = attribute(key);
  return !text.empty() && parse_int(text, out);
}

ImageRef Widget::attribute_image(std::string_view key) const {
  ImageRef ref;
  const std::string_view text = attribute(key);
  if (text.empty()) return ref;
  const std::vector<std::string_view> parts = split_list(text);
  if (parts.empty()) return ref;
  ref.path = resolve_alias(parts[0]);
  if (parts.size() >= 3) {
    std::int32_t x = 0;
    std::int32_t y = 0;
    if (parse_int(parts[1], x) && parse_int(parts[2], y)) {
      ref.has_key = x >= 0 && y >= 0;
      ref.key_x = x;
      ref.key_y = y;
    }
  }
  return ref;
}

bool Widget::visible_for(SelectionTag tags, std::uint32_t tab) const noexcept {
  if (any(forbid, tags)) return false;
  if (require_any != SelectionTag::kNone && !any(require_any, tags)) return false;
  if (tab_mask != 0 && (tab_mask & (1u << tab)) == 0) return false;
  return true;
}

const Widget* Screen::find(std::string_view widget) const noexcept {
  for (const auto& entry : widgets) {
    if (iequal(entry.name, widget)) return &entry;
  }
  return nullptr;
}

// -- loading ----------------------------------------------------------------

namespace {

/// The resolved entries of one section: file order, `Template` already merged.
struct ResolvedSection {
  std::vector<Attribute> keyed;
  std::vector<std::string> bare;
};

/// A document plus the bytes it views, so a template file stays alive while its
/// entries are read.
struct LoadedIni {
  std::string path;
  IniDocument document;
};

class Loader {
 public:
  explicit Loader(const FileProvider& provider) : provider_(provider) {}

  const IniDocument* open(std::string_view path) {
    const std::string resolved = resolve_alias(path);
    for (const auto& entry : open_) {
      if (iequal(entry->path, resolved)) return &entry->document;
    }
    if (!provider_) return nullptr;
    const std::span<const std::byte> bytes = provider_(resolved);
    if (bytes.empty()) return nullptr;
    Result<IniDocument> parsed = IniDocument::parse(bytes);
    if (!parsed.ok()) return nullptr;
    open_.push_back(std::make_unique<LoadedIni>(LoadedIni{resolved, std::move(parsed.value())}));
    return &open_.back()->document;
  }

 private:
  const FileProvider& provider_;
  // Indirect so that opening a second file cannot move the first: a caller
  // holds the returned pointer across further `open` calls.
  std::vector<std::unique_ptr<LoadedIni>> open_;
};

/// Merge `section` of `document` into `out`, following one `Template` line.
///
/// Local entries win: they are appended first and an inherited key that is
/// already present is skipped. Depth is bounded because a template that names
/// itself would otherwise not terminate; no shipped template chains at all.
void collect(Loader& loader, const IniDocument& document, std::string_view section,
             const ParamScope& scope, std::string_view self_path, ResolvedSection& out,
             std::vector<std::string>& warnings, int depth) {
  const SectionIndex index = document.section(section);
  if (index == kNoSection) {
    warnings.push_back("no section [" + std::string{section} + "]");
    return;
  }
  std::string template_value;
  for (const IniEntry& entry : document.entries_of(index)) {
    if (!entry.has_key) {
      out.bare.emplace_back(entry.value.empty() ? entry.key : entry.value);
      continue;
    }
    if (iequal(entry.key, "Template")) {
      if (template_value.empty()) template_value = scope.expand(entry.value);
      continue;
    }
    // `Style` is the one key that accumulates rather than overrides; see
    // `apply_common`.
    bool seen = false;
    if (!iequal(entry.key, "Style")) {
      for (const auto& kept : out.keyed) {
        if (iequal(kept.key, entry.key)) {
          seen = true;
          break;
        }
      }
    }
    if (!seen) out.keyed.push_back(Attribute{std::string{entry.key}, std::string{entry.value}});
  }
  if (template_value.empty() || depth >= 8) return;

  // `Template = <ini>, <section>` or `Template = <section>` in this same file.
  const std::vector<std::string_view> parts = split_list(template_value);
  std::string_view target_path = self_path;
  std::string_view target_section;
  if (parts.size() >= 2) {
    target_path = parts[0];
    target_section = parts[1];
  } else if (parts.size() == 1) {
    target_section = parts[0];
  } else {
    return;
  }
  const IniDocument* target =
      iequal(target_path, self_path) ? &document : loader.open(target_path);
  if (target == nullptr) {
    warnings.push_back("template file not found: " + std::string{target_path});
    return;
  }
  collect(loader, *target, target_section, scope, target_path, out, warnings, depth + 1);
}

/// The bare `key` of an entry with no `=`. `IniEntry` puts a bare line in
/// `key` with `has_key` false, and `value` empty; both are checked so that the
/// reader's convention cannot silently change under this.
std::string_view bare_text(const IniEntry& entry) {
  return entry.key.empty() ? entry.value : entry.key;
}

/// An integer with whatever follows it ignored: `24Text = Delete`, where a
/// line break went missing between two keys in `ADVCURMAP.INI`, and `44f`
/// in `TERRAINSETTINGS.INI`. The original's reader takes the leading digits
/// and the dialogs open; a strict reader refuses two editor screens over a
/// typo.
bool leading_int(std::string_view text, std::int32_t& out) {
  const std::string_view trimmed = trim_view(text);
  std::size_t end = 0;
  if (end < trimmed.size() && (trimmed[end] == '-' || trimmed[end] == '+')) ++end;
  if (trimmed.size() > end + 1 && trimmed[end] == '0' &&
      (trimmed[end + 1] == 'x' || trimmed[end + 1] == 'X')) {
    end += 2;
    while (end < trimmed.size() && std::isxdigit(static_cast<unsigned char>(trimmed[end])) != 0) ++end;
  } else {
    while (end < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[end])) != 0) ++end;
  }
  return end > 0 && parse_int(trimmed.substr(0, end), out);
}

bool parse_rect(std::string_view text, Rect& out, bool ltrb) {
  const std::vector<std::string_view> parts = split_list(text);
  if (parts.size() < 4) return false;
  std::int32_t v[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    if (!leading_int(parts[static_cast<std::size_t>(i)], v[i])) return false;
  }
  out.x = v[0];
  out.y = v[1];
  out.width = ltrb ? v[2] - v[0] : v[2];
  out.height = ltrb ? v[3] - v[1] : v[3];
  return true;
}

bool parse_align(std::string_view text, Align& out) {
  const std::vector<std::string_view> parts = split_list(text);
  if (parts.size() < 3) return false;
  std::int32_t v[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (!parse_int(parts[static_cast<std::size_t>(i)], v[i])) return false;
  }
  if (v[0] < 0 || v[1] < 0 || v[2] < 0) return false;
  out = Align{v[0], v[1], v[2]};
  return true;
}

}  // namespace

Result<Screen> load_screen(std::string_view path, std::string_view screen,
                           const FileProvider& provider) {
  Loader loader{provider};
  const std::string self_path = resolve_alias(path);
  const IniDocument* document = loader.open(self_path);
  if (document == nullptr) return FormatError::not_found;
  if (document->sections().empty()) return FormatError::malformed;

  Screen out;
  out.name = screen.empty() ? std::string{document->sections().front().name} : std::string{screen};
  out.path = self_path;

  // 1. The parameter scope. Local entries first, so that the `%TmplIni%` inside
  //    the Params section's own `Template` line resolves before the include it
  //    asks for happens; then the inherited ones, which never override.
  const std::string params_name = out.name + " Params";
  ParamScope scope;
  const SectionIndex params = document->section(params_name);
  std::string params_template;
  if (params != kNoSection) {
    for (const IniEntry& entry : document->entries_of(params)) {
      if (!entry.has_key) continue;
      if (iequal(entry.key, "Template")) {
        if (params_template.empty()) params_template.assign(entry.value);
        continue;
      }
      scope.set(entry.key, entry.value);
    }
  }
  if (!params_template.empty()) {
    const std::string expanded = scope.expand(params_template);
    const std::vector<std::string_view> parts = split_list(expanded);
    std::string_view target_path = self_path;
    std::string_view target_section;
    if (parts.size() >= 2) {
      target_path = parts[0];
      target_section = parts[1];
    } else if (parts.size() == 1) {
      target_section = parts[0];
    }
    if (!target_section.empty()) {
      const IniDocument* target =
          iequal(target_path, self_path) ? document : loader.open(target_path);
      if (target == nullptr) {
        out.warnings.push_back("params template not found: " + std::string{target_path});
      } else {
        const SectionIndex inherited = target->section(target_section);
        if (inherited == kNoSection) {
          out.warnings.push_back("no params section [" + std::string{target_section} + "]");
        } else {
          for (const IniEntry& entry : target->entries_of(inherited)) {
            if (entry.has_key) scope.set_default(entry.key, entry.value);
          }
        }
      }
    }
  }

  // 2. The dialog section itself.
  ResolvedSection dialog;
  collect(loader, *document, out.name, scope, self_path, dialog, out.warnings, 0);
  if (dialog.keyed.empty() && dialog.bare.empty()) return FormatError::not_found;

  // Widget rectangles resolve against other widgets by name, and not only
  // earlier ones: `SAVEGAME.INI`'s `ChatFrame` is placed from `NameLabel`,
  // listed two lines below it, and the editor's `AdvObjProps` places its
  // children from `width(AdvObjProps)`, the dialog itself. So a name that is
  // not built yet is resolved on demand -- its section collected and its own
  // rectangle evaluated, recursively, to a bounded depth -- and remembered.
  struct Pending {
    std::string key;
    Rect rect;
    std::int32_t id = 0;
    bool has_id = false;
  };
  std::vector<Pending> resolved_ahead;
  RectLookup lookup;
  int lookahead_depth = 0;
  const auto rect_ahead = [&](std::string_view widget, Rect& rect, std::int32_t& id,
                              bool& has_id) -> bool {
    const std::string key = fold(widget);
    for (const Pending& pending : resolved_ahead) {
      if (pending.key == key) {
        rect = pending.rect;
        id = pending.id;
        has_id = pending.has_id;
        return true;
      }
    }
    if (lookahead_depth >= 8) return false;
    const SectionIndex index = document->section(widget);
    if (index == kNoSection) return false;
    ResolvedSection section;
    std::vector<std::string> ignored;
    collect(loader, *document, widget, scope, self_path, section, ignored, 0);
    Pending pending;
    pending.key = key;
    ++lookahead_depth;
    bool rect_set = false;
    for (const Attribute& entry : section.keyed) {
      const std::string value = evaluate_expressions(scope.expand(entry.value), lookup);
      if ((iequal(entry.key, "RectWH") || iequal(entry.key, "Rect")) && !rect_set) {
        rect_set = parse_rect(value, pending.rect, iequal(entry.key, "Rect"));
      } else if (iequal(entry.key, "Id") || iequal(entry.key, "ID")) {
        pending.has_id = evaluate_expression(value, lookup, pending.id);
      }
    }
    --lookahead_depth;
    resolved_ahead.push_back(pending);
    rect = pending.rect;
    id = pending.id;
    has_id = pending.has_id;
    return true;
  };
  lookup = [&](std::string_view function, std::string_view widget, std::int32_t& value) -> bool {
    Rect rect;
    std::int32_t id = 0;
    bool has_id = false;
    if (function.empty()) {
      // A bare parameter name inside `#...#`.
      const std::string* bound = scope.find(widget);
      return bound != nullptr && parse_int(trim_view(scope.expand(*bound)), value);
    }
    if (const Widget* target = out.find(widget); target != nullptr) {
      rect = target->design;
      id = target->id;
      has_id = target->has_id;
    } else if (iequal(widget, out.name)) {
      rect = out.design;
    } else if (!rect_ahead(widget, rect, id, has_id)) {
      return false;
    }
    if (iequal(function, "left")) {
      value = rect.x;
    } else if (iequal(function, "top")) {
      value = rect.y;
    } else if (iequal(function, "right")) {
      value = rect.right();
    } else if (iequal(function, "bottom")) {
      value = rect.bottom();
    } else if (iequal(function, "width")) {
      value = rect.width;
    } else if (iequal(function, "height")) {
      value = rect.height;
    } else if (iequal(function, "id")) {
      if (!has_id) return false;
      value = id;
    } else {
      return false;
    }
    return true;
  };

  const auto resolve_value = [&](std::string_view raw) {
    return evaluate_expressions(scope.expand(raw), lookup);
  };

  const auto apply_common = [&](const ResolvedSection& section, Rect& rect, bool& from_ltrb,
                                Align& halign, Align& valign, Style& style,
                                std::vector<std::string>* unknown_styles,
                                std::vector<Attribute>* keep, bool* has_rect = nullptr) {
    // The entries are local first, then inherited, and a key an entry
    // already set is skipped by `collect` -- but `Rect` and `RectWH` are two
    // spellings of one thing, and `ADVENTUREMENU.INI`'s `List` writes
    // `RectWH` over a template whose `Rect` would otherwise land second and
    // win. The first rectangle is the one kept.
    //
    // `Style` is different: the local one and the template's are **unioned**,
    // with the alignment -- a two-bit field in the executable's table,
    // `ALIGN_CENTER` being `LEFT | RIGHT` -- taken from the local one when it
    // names any. **Reading, labelled.** `SELECTMAP.INI`'s `DescriptionText`
    // writes `Style = ALIGN_LEFT` over `StaticTextMultiline`'s `TRANSPARENT,
    // MULTILINE, ALIGN_CENTER`: a paragraph that replaced the template's
    // style would be one unwrapped line, and one that only OR'd it would
    // stay centred, and the author asked for neither.
    bool rect_set = false;
    bool style_set = false;
    bool align_set = false;
    for (const Attribute& entry : section.keyed) {
      const std::string value = resolve_value(entry.value);
      if (iequal(entry.key, "RectWH") || iequal(entry.key, "Rect")) {
        if (rect_set) {
          if (keep != nullptr) keep->push_back(Attribute{entry.key, value});
          continue;
        }
        const bool ltrb = iequal(entry.key, "Rect");
        Rect parsed;
        if (parse_rect(value, parsed, ltrb)) {
          rect = parsed;
          from_ltrb = ltrb;
          rect_set = true;
          if (has_rect != nullptr) *has_rect = true;
        } else {
          out.warnings.push_back("bad " + entry.key + ": " + value);
        }
      } else if (iequal(entry.key, "HAlign")) {
        if (!parse_align(value, halign)) out.warnings.push_back("bad HAlign: " + value);
      } else if (iequal(entry.key, "VAlign")) {
        if (!parse_align(value, valign)) out.warnings.push_back("bad VAlign: " + value);
      } else if (iequal(entry.key, "Style")) {
        const bool inherited = style_set;
        style_set = true;
        for (const std::string_view name : split_list(value)) {
          if (name.empty()) continue;
          bool known = false;
          for (const auto& flag : kStyles) {
            if (iequal(flag.name, name)) {
              const bool alignment = flag.flag == Style::kAlignLeft ||
                                     flag.flag == Style::kAlignRight ||
                                     flag.flag == Style::kAlignCenter;
              if (alignment) {
                if (!inherited || !align_set) style |= flag.flag;
                if (!inherited) align_set = true;
              } else {
                style |= flag.flag;
              }
              known = true;
              break;
            }
          }
          if (!known && unknown_styles != nullptr) unknown_styles->emplace_back(name);
        }
      }
      if (keep != nullptr) keep->push_back(Attribute{entry.key, value});
    }
  };

  {
    bool ignored = false;
    Align h;
    Align v;
    apply_common(dialog, out.design, ignored, h, v, out.style, nullptr, nullptr);
    for (const Attribute& entry : dialog.keyed) {
      const std::string value = resolve_value(entry.value);
      if (iequal(entry.key, "MinSize")) {
        const std::vector<std::string_view> parts = split_list(value);
        if (parts.size() >= 2) {
          (void)parse_int(parts[0], out.min_size.width);
          (void)parse_int(parts[1], out.min_size.height);
        }
      } else if (iequal(entry.key, "MaxSize")) {
        const std::vector<std::string_view> parts = split_list(value);
        if (parts.size() >= 2) {
          (void)parse_int(parts[0], out.max_size.width);
          (void)parse_int(parts[1], out.max_size.height);
        }
      } else if (iequal(entry.key, "Esc")) {
        out.escape = value;
      } else if (iequal(entry.key, "Enter")) {
        out.enter = value;
      } else if (iequal(entry.key, "Focus")) {
        out.focus = value;
      }
    }
  }

  // 3. The widgets, in the order `[<name> Objects]` lists them, which is the
  //    order they draw in: the background is first in every one of the 24 files.
  const SectionIndex objects = document->section(out.name + " Objects");
  if (objects == kNoSection) return FormatError::not_found;

  for (const IniEntry& entry : document->entries_of(objects)) {
    const std::string_view widget_name = trim_view(bare_text(entry));
    if (widget_name.empty()) continue;
    // The editor's `ADVADVENTURE.INI` rules its list with `----` lines.
    if (widget_name.find_first_not_of('-') == std::string_view::npos) continue;

    ResolvedSection section;
    const SectionIndex index = document->section(widget_name);
    if (index == kNoSection) {
      out.missing.emplace_back(widget_name);
      continue;
    }
    collect(loader, *document, widget_name, scope, self_path, section, out.warnings, 0);

    Widget widget;
    widget.name.assign(widget_name);
    apply_common(section, widget.design, widget.from_ltrb, widget.halign, widget.valign,
                 widget.style, &widget.unknown_styles, &widget.attributes, &widget.has_rect);

    for (const Attribute& attribute : widget.attributes) {
      const std::string_view key = attribute.key;
      const std::string& value = attribute.value;
      if (iequal(key, "Type")) {
        widget.type = value;
        widget.kind = widget_type_from_name(value);
        if (widget.kind == WidgetType::kUnknown) {
          out.warnings.push_back("unknown Type in [" + widget.name + "]: " + value);
        }
      } else if (iequal(key, "ImageType")) {
        widget.image_type = value;
        // Three editor dialogs write `ImageType = "AAAAA"`; the quotes are part
        // of the value as the reader sees it and are not a quoting convention.
        if (widget.image_type.size() >= 2 && widget.image_type.front() == '"' &&
            widget.image_type.back() == '"') {
          widget.image_type = widget.image_type.substr(1, widget.image_type.size() - 2);
        }
      } else if (iequal(key, "Rows")) {
        (void)parse_int(value, widget.rows);
      } else if (iequal(key, "InitialRow")) {
        (void)parse_int(value, widget.initial_row);
      } else if (iequal(key, "XFrames")) {
        (void)parse_int(value, widget.xframes);
      } else if (iequal(key, "YFrames")) {
        (void)parse_int(value, widget.yframes);
      } else if (iequal(key, "Dividers")) {
        const std::vector<std::string_view> parts = split_list(value);
        if (parts.size() >= 4 && parse_int(parts[0], widget.divider_left) &&
            parse_int(parts[1], widget.divider_middle) &&
            parse_int(parts[2], widget.divider_top) &&
            parse_int(parts[3], widget.divider_middle_height)) {
          widget.has_dividers = true;
        } else {
          out.warnings.push_back("bad Dividers: " + value);
        }
      } else if (iequal(key, "TabMask")) {
        std::int32_t mask = 0;
        if (parse_int(value, mask) && mask >= 0) widget.tab_mask = static_cast<std::uint32_t>(mask);
      } else if (iequal(key, "Id") || iequal(key, "ID")) {
        std::int32_t id = 0;
        if (evaluate_expression(value, lookup, id)) {
          widget.id = id;
          widget.has_id = true;
        }
      } else if (iequal(key, "HelpText")) {
        widget.help_text = value;
      } else if (iequal(key, "Rollover")) {
        widget.rollover = value;
      } else if (iequal(key, "Text")) {
        widget.text = value;
      } else if (iequal(key, "Value")) {
        widget.value = value;
      } else {
        for (const auto& show : kShows) {
          if (iequal(key, show.key) && !iequal(value, "no")) widget.show_for |= show.flag;
        }
      }
    }
    widget.image = widget.attribute_image("Image");
    widget.frame = widget.attribute_image("Frame");
    if (widget.rows < 1) widget.rows = 1;

    for (const std::string& bare : section.bare) {
      std::string_view text = trim_view(bare);
      if (text.empty()) continue;
      const bool negated = text.front() == '!';
      if (negated) text.remove_prefix(1);
      const SelectionTag tag = selection_tag_from_name(text);
      if (tag != SelectionTag::kNone) {
        if (negated) {
          widget.forbid |= tag;
        } else {
          widget.require_any |= tag;
        }
        continue;
      }
      if (iequal(text, "ReverseDraw")) {  // a flag, not a tag
        widget.reverse_draw = true;
        continue;
      }
      out.warnings.push_back("unknown bare line in [" + widget.name + "]: " + std::string{text});
    }

    out.widgets.push_back(std::move(widget));
  }

  return out;
}

// -- the faction table ------------------------------------------------------

namespace {
constexpr std::array<FactionBars, 8> kFactions{{
    {"Gaul", "gameini/infobar/infobar_gaul.ini", "gameini/cmdbar/cmdbar.ini",
     "gameini/cmdbar/empty_gaul.ini", "gameres/infobar/GAUL", "Gaul"},
    {"RepublicanRome", "gameini/infobar/infobar_rrome.ini", "gameini/cmdbar/cmdbarrrome.ini",
     "gameini/cmdbar/empty_rome.ini", "gameres/infobar/RROME", "RepublicanRome"},
    {"ImperialRome", "gameini/infobar/infobar_irome.ini", "gameini/cmdbar/cmdbarirome.ini",
     "gameini/cmdbar/empty_rome.ini", "gameres/infobar/IROME", "ImperialRome"},
    {"Carthage", "gameini/infobar/infobar_carthage.ini", "gameini/cmdbar/cmdbarCarthage.ini",
     "gameini/cmdbar/empty_carthage.ini", "gameres/infobar/CARTHAGE", "Carthage"},
    {"Egypt", "gameini/infobar/infobar_egypt.ini", "gameini/cmdbar/cmdbarEgypt.ini",
     "gameini/cmdbar/empty_egypt.ini", "gameres/infobar/EGYPT", "Egypt"},
    {"Iberia", "gameini/infobar/infobar_iberia.ini", "gameini/cmdbar/cmdbarIberia.ini",
     "gameini/cmdbar/empty_iberia.ini", "gameres/infobar/IBERIA", "Iberia"},
    {"Britain", "gameini/infobar/infobar_britain.ini", "gameini/cmdbar/cmdbarbritain.ini",
     "gameini/cmdbar/empty_britain.ini", "gameres/infobar/BRITAIN", "Britain"},
    {"Germany", "gameini/infobar/infobar_german.ini", "gameini/cmdbar/cmdbarGerman.ini",
     "gameini/cmdbar/empty_german.ini", "gameres/infobar/GERMAN", "Germany"},
}};
}  // namespace

Result<std::vector<NamedImage>> load_image_table(std::string_view path,
                                                 std::string_view section,
                                                 const FileProvider& provider) {
  Loader loader{provider};
  const IniDocument* document = loader.open(path);
  if (document == nullptr) return FormatError::not_found;
  const SectionIndex index = document->section(section);
  if (index == kNoSection) return FormatError::not_found;

  std::vector<NamedImage> out;
  for (const IniEntry& entry : document->entries_of(index)) {
    if (!entry.has_key) continue;
    Widget holder;
    holder.attributes.push_back(Attribute{"Image", std::string{entry.value}});
    out.push_back(NamedImage{std::string{entry.key}, holder.attribute_image("Image")});
  }
  return out;
}

std::span<const FactionBars> faction_bars() noexcept { return kFactions; }

const FactionBars* faction_bars_for(std::string_view faction) noexcept {
  for (const auto& entry : kFactions) {
    if (iequal(entry.faction, faction)) return &entry;
  }
  return nullptr;
}

}  // namespace imperivm::core::ui
