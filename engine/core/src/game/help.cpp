#include "imperivm/core/game/help.hpp"

#include <algorithm>

#include "imperivm/core/game/localization.hpp"

namespace imperivm::core::game {
namespace {

struct Attribute {
  std::string_view name;
  std::string_view value;
};

/// The five predefined entities, and numeric references.
std::string decode_entities(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out.push_back(text[i]);
      continue;
    }
    const std::size_t end = text.find(';', i);
    if (end == std::string_view::npos) {
      out.push_back('&');
      continue;
    }
    const std::string_view name = text.substr(i + 1, end - i - 1);
    if (name == "amp") {
      out.push_back('&');
    } else if (name == "lt") {
      out.push_back('<');
    } else if (name == "gt") {
      out.push_back('>');
    } else if (name == "quot") {
      out.push_back('"');
    } else if (name == "apos") {
      out.push_back('\'');
    } else if (!name.empty() && name[0] == '#') {
      std::uint32_t code = 0;
      const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
      for (std::size_t k = hex ? 2 : 1; k < name.size(); ++k) {
        const char c = name[k];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
          digit = static_cast<std::uint32_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
          digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
          digit = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
          break;
        }
        code = code * (hex ? 16u : 10u) + digit;
      }
      // Re-encoded as UTF-8, which the whole text is converted from below.
      if (code < 0x80) {
        out.push_back(static_cast<char>(code));
      } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      }
    } else {
      out.append(text.substr(i, end - i + 1));
    }
    i = end;
  }
  return out;
}

bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// Whitespace as the entries carry it: a run of spaces and tabs collapses
/// to one, a line break stays a line break, blank lines stay one blank line,
/// and the ends are trimmed.
std::string tidy(std::string_view raw) {
  std::string out;
  bool pending_space = false;
  int pending_breaks = 0;
  for (const char c : raw) {
    if (c == '\r') continue;
    if (c == '\n') {
      ++pending_breaks;
      pending_space = false;
      continue;
    }
    if (c == ' ' || c == '\t') {
      pending_space = true;
      continue;
    }
    if (!out.empty()) {
      if (pending_breaks > 0) {
        out.append(std::min(pending_breaks, 2), '\n');
      } else if (pending_space) {
        out.push_back(' ');
      }
    }
    pending_breaks = 0;
    pending_space = false;
    out.push_back(c);
  }
  return out;
}

/// A tag at `at` (which is `<`): its name, attributes, whether it closes
/// itself, and where it ends.
struct Tag {
  std::string_view name;
  std::vector<Attribute> attributes;
  bool closing = false;
  bool self_closing = false;
  std::size_t end = 0;  ///< one past the `>`
};

bool read_tag(std::string_view text, std::size_t at, Tag& tag) {
  std::size_t i = at + 1;
  if (i < text.size() && text[i] == '/') {
    tag.closing = true;
    ++i;
  }
  const std::size_t name_start = i;
  while (i < text.size() && !is_space(text[i]) && text[i] != '>' && text[i] != '/') ++i;
  tag.name = text.substr(name_start, i - name_start);
  while (i < text.size()) {
    while (i < text.size() && is_space(text[i])) ++i;
    if (i >= text.size()) return false;
    if (text[i] == '/') {
      tag.self_closing = true;
      ++i;
      continue;
    }
    if (text[i] == '>') {
      tag.end = i + 1;
      return true;
    }
    const std::size_t key_start = i;
    while (i < text.size() && text[i] != '=' && !is_space(text[i]) && text[i] != '>') ++i;
    const std::string_view key = text.substr(key_start, i - key_start);
    while (i < text.size() && is_space(text[i])) ++i;
    if (i >= text.size() || text[i] != '=') continue;
    ++i;
    while (i < text.size() && is_space(text[i])) ++i;
    if (i >= text.size()) return false;
    const char quote = text[i];
    if (quote != '"' && quote != '\'') return false;
    const std::size_t value_start = ++i;
    while (i < text.size() && text[i] != quote) ++i;
    if (i >= text.size()) return false;
    tag.attributes.push_back(Attribute{key, text.substr(value_start, i - value_start)});
    ++i;
  }
  return false;
}

std::string_view attribute_of(const Tag& tag, std::string_view name) {
  for (const Attribute& attribute : tag.attributes) {
    if (attribute.name == name) return attribute.value;
  }
  return {};
}

}  // namespace

Result<HelpDocument> HelpDocument::parse(std::span<const std::byte> xml) {
  std::string_view text(reinterpret_cast<const char*>(xml.data()), xml.size());
  // A byte-order mark, and the declaration.
  if (text.size() >= 3 && static_cast<std::uint8_t>(text[0]) == 0xEF &&
      static_cast<std::uint8_t>(text[1]) == 0xBB && static_cast<std::uint8_t>(text[2]) == 0xBF) {
    text.remove_prefix(3);
  }
  HelpDocument out;
  std::vector<std::int32_t> stack;  // open topics
  bool in_help = false;
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t open = text.find('<', i);
    if (open == std::string_view::npos) break;
    if (text.compare(open, 4, "<!--") == 0) {
      const std::size_t close = text.find("-->", open);
      if (close == std::string_view::npos) return FormatError::malformed;
      i = close + 3;
      continue;
    }
    if (text.compare(open, 2, "<?") == 0) {
      const std::size_t close = text.find("?>", open);
      if (close == std::string_view::npos) return FormatError::malformed;
      i = close + 2;
      continue;
    }
    Tag tag;
    if (!read_tag(text, open, tag)) return FormatError::malformed;
    i = tag.end;
    if (tag.name == "help") {
      if (tag.closing) break;
      in_help = true;
      continue;
    }
    if (!in_help) return FormatError::bad_magic;
    if (tag.name == "topic") {
      if (tag.closing) {
        if (stack.empty()) return FormatError::malformed;
        stack.pop_back();
        continue;
      }
      HelpTopic topic;
      topic.id.assign(attribute_of(tag, "id"));
      topic.parent = stack.empty() ? -1 : stack.back();
      const std::int32_t index = static_cast<std::int32_t>(out.topics_.size());
      if (topic.parent >= 0) out.topics_[static_cast<std::size_t>(topic.parent)].children.push_back(index);
      out.topics_.push_back(std::move(topic));
      if (!tag.self_closing) stack.push_back(index);
      continue;
    }
    if (tag.name == "entry") {
      if (tag.closing || stack.empty()) continue;
      HelpEntry entry;
      entry.link.assign(attribute_of(tag, "link"));
      entry.image.assign(attribute_of(tag, "image"));
      entry.large = attribute_of(tag, "font") == "large";
      entry.centred = attribute_of(tag, "hcenter") == "1";
      if (!tag.self_closing) {
        const std::size_t close = text.find("</entry>", i);
        if (close == std::string_view::npos) return FormatError::malformed;
        entry.text = cp1252_from_utf8(tidy(decode_entities(text.substr(i, close - i))));
        i = close + 8;
      }
      out.topics_[static_cast<std::size_t>(stack.back())].entries.push_back(std::move(entry));
      continue;
    }
    // Anything else is skipped with its content, if it has any.
    if (!tag.self_closing && !tag.closing) {
      const std::string closer = "</" + std::string(tag.name) + ">";
      const std::size_t close = text.find(closer, i);
      if (close != std::string_view::npos) i = close + closer.size();
    }
  }
  if (!in_help) return FormatError::bad_magic;
  return out;
}

Result<std::vector<Tip>> parse_tips(std::span<const std::byte> xml) {
  std::string_view text(reinterpret_cast<const char*>(xml.data()), xml.size());
  if (text.size() >= 3 && static_cast<std::uint8_t>(text[0]) == 0xEF &&
      static_cast<std::uint8_t>(text[1]) == 0xBB && static_cast<std::uint8_t>(text[2]) == 0xBF) {
    text.remove_prefix(3);
  }
  std::vector<Tip> out;
  bool in_tips = false;
  bool in_tip = false;
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t open = text.find('<', i);
    if (open == std::string_view::npos) break;
    if (text.compare(open, 4, "<!--") == 0) {
      const std::size_t close = text.find("-->", open);
      if (close == std::string_view::npos) return FormatError::malformed;
      i = close + 3;
      continue;
    }
    if (text.compare(open, 2, "<?") == 0) {
      const std::size_t close = text.find("?>", open);
      if (close == std::string_view::npos) return FormatError::malformed;
      i = close + 2;
      continue;
    }
    Tag tag;
    if (!read_tag(text, open, tag)) return FormatError::malformed;
    i = tag.end;
    if (tag.name == "tips") {
      if (tag.closing) break;
      in_tips = true;
      continue;
    }
    if (!in_tips) return FormatError::bad_magic;
    if (tag.name == "tip") {
      if (tag.closing) {
        in_tip = false;
        continue;
      }
      Tip tip;
      tip.link.assign(attribute_of(tag, "link"));
      out.push_back(std::move(tip));
      in_tip = !tag.self_closing;
      continue;
    }
    if (tag.name == "text" && in_tip && !tag.closing && !tag.self_closing) {
      const std::size_t close = text.find("</text>", i);
      if (close == std::string_view::npos) return FormatError::malformed;
      out.back().text = cp1252_from_utf8(tidy(decode_entities(text.substr(i, close - i))));
      i = close + 7;
      continue;
    }
    if (!tag.self_closing && !tag.closing) {
      const std::string closer = "</" + std::string(tag.name) + ">";
      const std::size_t close = text.find(closer, i);
      if (close != std::string_view::npos) i = close + closer.size();
    }
  }
  if (!in_tips) return FormatError::bad_magic;
  return out;
}

std::int32_t HelpDocument::resolve(std::string_view link, std::int32_t from) const noexcept {
  if (link.empty()) return -1;
  std::string_view id = link;
  if (const std::size_t slash = id.rfind('/'); slash != std::string_view::npos) {
    id = id.substr(slash + 1);
  }
  if (from >= 0 && static_cast<std::size_t>(from) < topics_.size()) {
    for (const std::int32_t child : topics_[static_cast<std::size_t>(from)].children) {
      if (topics_[static_cast<std::size_t>(child)].id == id) return child;
    }
  }
  for (std::size_t i = 0; i < topics_.size(); ++i) {
    if (topics_[i].id == id) return static_cast<std::int32_t>(i);
  }
  return -1;
}

}  // namespace imperivm::core::game
