// The translation table, and the substitution the scripts do with it.
// See include/imperivm/core/game/localization.hpp.

#include "imperivm/core/game/localization.hpp"

#include <algorithm>

#include "imperivm/core/xml.hpp"

namespace imperivm::core::game {
namespace {

/// `tolower` in the C locale, byte by byte: what 0x00689030 does to a name.
[[nodiscard]] std::string folded(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

/// Rows in file order into a sorted map where a later row overwrites an
/// earlier one under the same name -- the loader's plain assignment.
void settle(std::vector<Translation>& rows) {
  std::stable_sort(rows.begin(), rows.end(),
                   [](const Translation& a, const Translation& b) { return a.key < b.key; });
  std::vector<Translation> out;
  out.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i + 1 < rows.size() && rows[i + 1].key == rows[i].key) continue;  // a later one wins
    out.push_back(std::move(rows[i]));
  }
  rows = std::move(out);
}

[[nodiscard]] bool all_digits(std::string_view text) noexcept {
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

}  // namespace

std::string cp1252_from_utf8(std::string_view text) {
  // cp1252's 0x80..0x9F window, as `ui/font.cpp` maps it the other way.
  static constexpr std::uint32_t kHighWindow[32] = {
      0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
      0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const std::uint8_t lead = static_cast<std::uint8_t>(text[i]);
    if (lead < 0x80) {
      out.push_back(static_cast<char>(lead));
      ++i;
      continue;
    }
    std::size_t length = 0;
    std::uint32_t code = 0;
    if ((lead & 0xE0) == 0xC0) {
      length = 2;
      code = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      code = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      code = lead & 0x07;
    }
    bool valid = length > 0 && i + length <= text.size();
    for (std::size_t k = 1; valid && k < length; ++k) {
      const std::uint8_t follow = static_cast<std::uint8_t>(text[i + k]);
      if ((follow & 0xC0) != 0x80) {
        valid = false;
      } else {
        code = (code << 6) | (follow & 0x3F);
      }
    }
    if (!valid) {
      // Not UTF-8: cp1252 already.
      out.push_back(static_cast<char>(lead));
      ++i;
      continue;
    }
    i += length;
    if (code < 0x80 || (code >= 0xA0 && code <= 0xFF)) {
      out.push_back(static_cast<char>(code));
      continue;
    }
    char mapped = '?';
    for (std::size_t k = 0; k < 32; ++k) {
      if (kHighWindow[k] == code) {
        mapped = static_cast<char>(0x80 + k);
        break;
      }
    }
    out.push_back(mapped);
  }
  return out;
}

Result<TranslationTable> TranslationTable::parse(std::span<const std::byte> xml) {
  const Result<XmlDocument> document = XmlDocument::parse(xml);
  if (!document.ok()) return document.error();
  const XmlDocument& doc = document.value();

  const NodeIndex root = doc.root();
  if (root == kNoNode || doc.node(root).name != "translationtable") {
    return FormatError::bad_magic;
  }

  TranslationTable table;
  for (NodeIndex entry = doc.child(root, "translationtableentry"); entry != kNoNode;
       entry = doc.next(entry, "translationtableentry")) {
    const std::string_view key = doc.attribute(entry, "text");
    const std::string_view just = doc.attribute(entry, "justtext");
    std::string result = cp1252_from_utf8(doc.attribute(entry, "result"));
    // The loader's maps (0x00689f40): `text` always, `justtext` when present,
    // each exactly and folded. An empty `text` is filed too, but `lookup`
    // answers an empty key before it reads a map, so it is never found.
    if (!key.empty()) {
      table.named_.push_back(Translation{std::string(key), result});
      table.named_folded_.push_back(Translation{folded(key), result});
    }
    if (!just.empty()) {
      table.named_.push_back(Translation{std::string(just), result});
      table.named_folded_.push_back(Translation{folded(just), result});
    }
    // An entry nothing can name is not a translation. The shipped table has
    // none; a hand-edited one might.
    if (key.empty()) continue;
    table.entries_.push_back(Translation{std::string(key), std::move(result)});
  }
  settle(table.named_);
  settle(table.named_folded_);

  // Sorted once, so lookup is a binary search and iteration order is stable.
  std::sort(table.entries_.begin(), table.entries_.end(),
            [](const Translation& a, const Translation& b) { return a.key < b.key; });
  // No key occurs twice in the shipped table. A duplicate in a modded one
  // resolves to the first, matching how the INI reader breaks the same tie.
  table.entries_.erase(std::unique(table.entries_.begin(), table.entries_.end(),
                                   [](const Translation& a, const Translation& b) {
                                     return a.key == b.key;
                                   }),
                       table.entries_.end());
  table.folded_.reserve(table.entries_.size());
  for (std::uint32_t i = 0; i < table.entries_.size(); ++i) {
    std::string folded = table.entries_[i].key;
    for (char& c : folded) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    table.folded_.emplace_back(std::move(folded), i);
  }
  std::sort(table.folded_.begin(), table.folded_.end());
  return table;
}

std::size_t TranslationTable::merge(const TranslationTable& other) {
  // The names `lookup` reads: the other table's, where this one has none.
  // See `lookup` for how that stands in for the original's second table.
  const auto merge_names = [](std::vector<Translation>& mine,
                              const std::vector<Translation>& theirs) {
    std::vector<Translation> extra;
    for (const Translation& entry : theirs) {
      if (find_in(mine, entry.key) == nullptr) extra.push_back(entry);
    }
    if (!extra.empty()) {
      mine.insert(mine.end(), extra.begin(), extra.end());
      std::sort(mine.begin(), mine.end(),
                [](const Translation& a, const Translation& b) { return a.key < b.key; });
    }
  };
  merge_names(named_, other.named_);
  merge_names(named_folded_, other.named_folded_);

  std::size_t added = 0;
  for (const Translation& entry : other.entries_) {
    if (contains(entry.key)) continue;
    entries_.push_back(entry);
    ++added;
  }
  if (added == 0) return 0;
  std::sort(entries_.begin(), entries_.end(),
            [](const Translation& a, const Translation& b) { return a.key < b.key; });
  folded_.clear();
  folded_.reserve(entries_.size());
  for (std::uint32_t i = 0; i < entries_.size(); ++i) {
    std::string folded = entries_[i].key;
    for (char& c : folded) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    folded_.emplace_back(std::move(folded), i);
  }
  std::sort(folded_.begin(), folded_.end());
  return added;
}

std::string_view TranslationTable::translate_in_context(std::string_view text,
                                                        std::string_view context) const {
  // An exact key first; then the folded index, with or without a context --
  // the game scripts' basenames arrive in the pack's upper case (`2 SCORE
  // LIMIT`) and the table keys them as `Score limit`.
  if (context.empty() && contains(text)) return translate(text);
  {
    std::string probe;
    probe.reserve(text.size() + 1 + context.size());
    probe.append(text);
    if (!context.empty()) {
      probe.push_back('@');
      probe.append(context);
    }
    for (char& c : probe) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const auto it = std::lower_bound(
        folded_.begin(), folded_.end(), probe,
        [](const std::pair<std::string, std::uint32_t>& entry, const std::string& wanted) {
          return entry.first < wanted;
        });
    if (it != folded_.end() && it->first == probe) return entries_[it->second].result;
  }
  return translate(text);
}

bool TranslationTable::contains(std::string_view key) const noexcept {
  const auto it = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const Translation& entry, std::string_view probe) { return entry.key < probe; });
  return it != entries_.end() && it->key == key;
}

std::string_view TranslationTable::translate(std::string_view key) const noexcept {
  const auto it = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const Translation& entry, std::string_view probe) { return entry.key < probe; });
  if (it == entries_.end() || it->key != key) return key;
  return it->result;
}

const std::string* TranslationTable::find_in(const std::vector<Translation>& map,
                                             std::string_view name) noexcept {
  const auto it = std::lower_bound(
      map.begin(), map.end(), name,
      [](const Translation& entry, std::string_view probe) { return entry.key < probe; });
  if (it == map.end() || it->key != name) return nullptr;
  return &it->result;
}

std::string TranslationTable::lookup(std::string_view key, std::string_view context) const {
  if (key.empty()) return std::string();
  if (all_digits(key)) return std::string(key);

  // The first probe that finds its name decides, exact before folded; a name
  // filed with an empty result answers the key (0x006892f4, 0x00689429).
  const auto probe = [&](const std::string& name) -> const std::string* {
    const std::string* exact = find_in(named_, name);
    return exact != nullptr ? exact : find_in(named_folded_, folded(name));
  };
  std::string name(key);
  if (!context.empty()) {
    name.push_back('@');
    name.append(context);
  }
  const std::string* hit = probe(name);
  if (hit == nullptr && !context.empty()) hit = probe(std::string(key));
  if (hit == nullptr || hit->empty()) return std::string(key);
  return *hit;
}

std::string TranslationTable::format(std::string_view key,
                                     std::span<const std::string> arguments) const {
  const std::string pattern = lookup(key);
  const auto bad = [](std::size_t at) { return "Bad parameter at pos " + std::to_string(at); };

  std::string out;
  out.reserve(pattern.size());
  std::size_t i = 0;
  while (i < pattern.size()) {
    if (pattern[i] != '%') {
      out.push_back(pattern[i]);
      ++i;
      continue;
    }
    const char letter = i + 1 < pattern.size() ? pattern[i + 1] : '\0';
    if (letter == '%') {
      out.push_back('%');
      i += 2;
      continue;
    }
    i += 2;
    const bool text = letter == 's' || letter == 'S';
    if (!text && letter != 'd' && letter != 'D') return bad(i);
    std::size_t number = 0;
    std::size_t digits = 0;
    while (i < pattern.size() && pattern[i] >= '0' && pattern[i] <= '9') {
      number = number * 10 + static_cast<std::size_t>(pattern[i] - '0');
      ++digits;
      ++i;
    }
    if (digits == 0 || number == 0) return bad(i);
    if (number > arguments.size()) {
      // Past the arguments the original reads whatever follows them; the
      // placeholder is left as written instead.
      out.append(pattern, i - digits - 2, digits + 2);
      continue;
    }
    const std::string& argument = arguments[number - 1];
    out += text ? lookup(argument) : argument;
  }
  if (find_in(named_, out) != nullptr) return lookup(out);
  return out;
}

std::string substitute(std::string_view text, std::span<const std::string> arguments) {
  std::string out;
  out.reserve(text.size());

  for (std::size_t i = 0; i < text.size(); ++i) {
    // `%s1` and `%d1` mean the same thing. The letter is not a type directive:
    // the corpus passes integers to `%s` and the language has no field width,
    // so `"%s1:0%s2"` pads by writing the `0` out.
    const bool placeholder = text[i] == '%' && i + 2 < text.size() &&
                             (text[i + 1] == 's' || text[i + 1] == 'd') &&
                             text[i + 2] >= '1' && text[i + 2] <= '9';
    if (!placeholder) {
      out.push_back(text[i]);
      continue;
    }

    const std::size_t index = static_cast<std::size_t>(text[i + 2] - '1');
    if (index < arguments.size()) {
      out += arguments[index];
    } else {
      // Left as written. A blank here would hide the mismatch in exactly the
      // string a tester is staring at.
      out.append(text.substr(i, 3));
    }
    i += 2;
  }
  return out;
}

void split_token(std::string_view text, std::string& token, std::string& tail) {
  // Both halves are built before either is assigned. Every one of the 71 call
  // sites in the corpus is `ParseStr(dest, dest)`, and callers reach this with
  // `text` viewing the very string they are about to overwrite; assigning in
  // place would cut the view out from under the second read.
  std::size_t begin = 0;
  while (begin < text.size() && text[begin] == ' ') ++begin;
  const std::size_t comma = text.find(',', begin);
  std::size_t end = comma == std::string_view::npos ? text.size() : comma;
  while (end > begin && text[end - 1] == ' ') --end;
  std::string head(text.substr(begin, end - begin));
  std::string rest =
      comma == std::string_view::npos ? std::string() : std::string(text.substr(comma + 1));
  token = std::move(head);
  tail = std::move(rest);
}

}  // namespace imperivm::core::game
