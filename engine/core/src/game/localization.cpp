// The translation table, and the substitution the scripts do with it.
// See include/imperivm/core/game/localization.hpp.

#include "imperivm/core/game/localization.hpp"

#include <algorithm>

#include "imperivm/core/xml.hpp"

namespace imperivm::core::game {
namespace {

[[nodiscard]] constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  std::size_t begin = 0;
  while (begin < text.size() && is_space(text[begin])) ++begin;
  std::size_t end = text.size();
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
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
    // An entry nothing can name is not a translation. The shipped table has
    // none; a hand-edited one might.
    if (key.empty()) continue;
    table.entries_.push_back(
        Translation{std::string(key), cp1252_from_utf8(doc.attribute(entry, "result"))});
  }

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
  const std::size_t comma = text.find(',');
  std::string head;
  std::string rest;
  if (comma == std::string_view::npos) {
    head = std::string(trim(text));
  } else {
    head = std::string(trim(text.substr(0, comma)));
    rest = std::string(trim(text.substr(comma + 1)));
  }
  token = std::move(head);
  tail = std::move(rest);
}

}  // namespace imperivm::core::game
