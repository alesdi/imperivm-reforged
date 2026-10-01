#include "imperivm/core/xml_patch.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace imperivm::core {
namespace {

struct Attribute {
  std::string_view name;
  std::size_t value_begin = 0;  ///< inside the quotes
  std::size_t value_end = 0;
  std::size_t begin = 0;        ///< the name's first byte
};

/// One opening tag, scanned.
struct Tag {
  std::string_view name;
  std::size_t begin = 0;      ///< the `<`
  std::size_t end = 0;        ///< one past the `>`
  bool self_closing = false;
  bool closing = false;       ///< `</name>`
  std::vector<Attribute> attributes;
};

bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_name(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
         c == '-' || c == ':';
}

/// The next tag at or after `from`, skipping comments and declarations.
bool next_tag(std::string_view text, std::size_t from, Tag& tag) {
  std::size_t at = from;
  while (at < text.size()) {
    at = text.find('<', at);
    if (at == std::string_view::npos) return false;
    if (text.compare(at, 4, "<!--") == 0) {
      const std::size_t close = text.find("-->", at);
      if (close == std::string_view::npos) return false;
      at = close + 3;
      continue;
    }
    if (at + 1 < text.size() && (text[at + 1] == '?' || text[at + 1] == '!')) {
      const std::size_t close = text.find('>', at);
      if (close == std::string_view::npos) return false;
      at = close + 1;
      continue;
    }
    break;
  }
  if (at >= text.size()) return false;
  tag = Tag{};
  tag.begin = at;
  std::size_t p = at + 1;
  if (p < text.size() && text[p] == '/') {
    tag.closing = true;
    ++p;
  }
  const std::size_t name_begin = p;
  while (p < text.size() && is_name(text[p])) ++p;
  tag.name = text.substr(name_begin, p - name_begin);
  for (;;) {
    while (p < text.size() && is_space(text[p])) ++p;
    if (p >= text.size()) return false;
    if (text[p] == '>') {
      tag.end = p + 1;
      return true;
    }
    if (text[p] == '/' && p + 1 < text.size() && text[p + 1] == '>') {
      tag.self_closing = true;
      tag.end = p + 2;
      return true;
    }
    Attribute attribute;
    attribute.begin = p;
    const std::size_t attr_name = p;
    while (p < text.size() && is_name(text[p])) ++p;
    attribute.name = text.substr(attr_name, p - attr_name);
    while (p < text.size() && is_space(text[p])) ++p;
    if (p >= text.size() || text[p] != '=') return false;
    ++p;
    while (p < text.size() && is_space(text[p])) ++p;
    if (p >= text.size() || (text[p] != '"' && text[p] != '\'')) return false;
    const char quote = text[p];
    attribute.value_begin = ++p;
    p = text.find(quote, p);
    if (p == std::string_view::npos) return false;
    attribute.value_end = p;
    ++p;
    tag.attributes.push_back(attribute);
  }
}

struct Step {
  std::string_view name;
  std::string_view key;
  std::string_view value;
  bool keyed = false;
  /// `name[n]`: the n-th sibling of that name under the same parent.
  bool indexed = false;
  std::size_t index = 0;
};

std::vector<Step> parse_path(std::string_view path) {
  std::vector<Step> steps;
  std::size_t at = 0;
  while (at <= path.size()) {
    std::size_t end = path.find('/', at);
    if (end == std::string_view::npos) end = path.size();
    std::string_view part = path.substr(at, end - at);
    Step step;
    if (const std::size_t bracket = part.find('['); bracket != std::string_view::npos && part.back() == ']') {
      step.name = part.substr(0, bracket);
      const std::string_view inside = part.substr(bracket + 1, part.size() - bracket - 2);
      const std::size_t eq = inside.find('=');
      if (eq != std::string_view::npos) {
        step.key = inside.substr(0, eq);
        step.value = inside.substr(eq + 1);
        step.keyed = true;
      } else if (!inside.empty() && inside.find_first_not_of("0123456789") == std::string_view::npos) {
        step.indexed = true;
        for (const char c : inside) step.index = step.index * 10 + static_cast<std::size_t>(c - '0');
      }
    } else {
      step.name = part;
    }
    if (!step.name.empty()) steps.push_back(step);
    if (end == path.size()) break;
    at = end + 1;
  }
  return steps;
}

std::string_view attribute_value(std::string_view text, const Tag& tag, std::string_view name) {
  for (const Attribute& attribute : tag.attributes) {
    if (attribute.name == name) return text.substr(attribute.value_begin, attribute.value_end - attribute.value_begin);
  }
  return {};
}

/// The element at `path`: its opening tag, and the position one past its
/// end (its `/>`, or its closing tag's `>`). Depth is tracked by opening
/// and closing tags; the documents this serves nest two deep at most.
bool find_element(std::string_view text, std::string_view path, Tag& out, std::size_t* element_end) {
  const std::vector<Step> steps = parse_path(path);
  if (steps.empty()) return false;
  // Each open element on the way, and whether it matched its step -- by
  // name and, for a keyed step, by the attribute -- so that
  // `items/item[id=X]/bonus` finds the bonus of that item alone.
  std::vector<bool> stack;
  // How many siblings of each step's name have been seen under the current
  // parent, for `name[n]`; reset when the parent closes.
  std::vector<std::size_t> seen(steps.size(), 0);
  std::size_t at = 0;
  Tag tag;
  const auto matches = [&](const Tag& candidate, const Step& step, std::size_t depth) {
    if (candidate.name != step.name) return false;
    if (step.keyed) return attribute_value(text, candidate, step.key) == step.value;
    if (step.indexed) return seen[depth]++ == step.index;
    return true;
  };
  while (next_tag(text, at, tag)) {
    at = tag.end;
    if (tag.closing) {
      if (!stack.empty()) stack.pop_back();
      if (stack.size() + 1 < seen.size()) seen[stack.size() + 1] = 0;
      continue;
    }
    // Whether every open element matched its step, and -- evaluated once,
    // because an indexed step counts as it matches -- whether this tag
    // matches the step at its depth.
    bool parents_match = stack.size() < steps.size();
    for (std::size_t i = 0; parents_match && i < stack.size(); ++i) parents_match = stack[i];
    const bool matched = parents_match && matches(tag, steps[stack.size()], stack.size());
    const bool on_path = stack.size() + 1 == steps.size();
    if (on_path && matched) {
      out = tag;
      if (element_end != nullptr) {
        if (tag.self_closing) {
          *element_end = tag.end;
        } else {
          // Its closing tag, past any nested children.
          std::size_t depth = 1;
          std::size_t scan = tag.end;
          Tag inner;
          *element_end = tag.end;
          while (depth > 0 && next_tag(text, scan, inner)) {
            scan = inner.end;
            if (inner.closing) {
              --depth;
            } else if (!inner.self_closing) {
              ++depth;
            }
            *element_end = inner.end;
          }
        }
      }
      return true;
    }
    if (!tag.self_closing) stack.push_back(matched && !on_path);
  }
  return false;
}

void escape_into(std::string& out, std::string_view value) {
  for (const char c : value) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c); break;
    }
  }
}

/// The whitespace run ending at `at`, back to the previous non-space.
std::string_view whitespace_before(std::string_view text, std::size_t at) {
  std::size_t begin = at;
  while (begin > 0 && is_space(text[begin - 1])) --begin;
  return text.substr(begin, at - begin);
}

}  // namespace

std::string xml_set_attributes(std::string_view document, std::string_view path,
                               std::span<const XmlEdit> edits, bool* found) {
  Tag tag;
  if (!find_element(document, path, tag, nullptr)) {
    if (found != nullptr) *found = false;
    return std::string(document);
  }
  if (found != nullptr) *found = true;
  // Rebuild the opening tag: existing attributes with their values swapped,
  // new ones appended on the last attribute's indentation (or a space
  // when the tag is on one line).
  std::string out(document.substr(0, tag.begin));
  std::string_view indent = " ";
  if (!tag.attributes.empty()) indent = whitespace_before(document, tag.attributes.back().begin);
  std::size_t cursor = tag.begin;
  std::vector<bool> used(edits.size(), false);
  for (const Attribute& attribute : tag.attributes) {
    out.append(document.substr(cursor, attribute.value_begin - cursor));
    bool replaced = false;
    for (std::size_t i = 0; i < edits.size(); ++i) {
      if (edits[i].first != attribute.name) continue;
      escape_into(out, edits[i].second);
      used[i] = true;
      replaced = true;
      break;
    }
    if (!replaced) out.append(document.substr(attribute.value_begin, attribute.value_end - attribute.value_begin));
    cursor = attribute.value_end;
  }
  // The tail of the tag: up to the `/>` or `>`, with the new attributes
  // laid in before it.
  const std::size_t close = tag.self_closing ? tag.end - 2 : tag.end - 1;
  std::size_t tail_begin = close;
  while (tail_begin > cursor && is_space(document[tail_begin - 1])) --tail_begin;
  out.append(document.substr(cursor, tail_begin - cursor));
  for (std::size_t i = 0; i < edits.size(); ++i) {
    if (used[i]) continue;
    out.append(indent);
    out.append(edits[i].first);
    out += "=\"";
    escape_into(out, edits[i].second);
    out.push_back('"');
  }
  out.append(document.substr(tail_begin));
  return out;
}

std::string xml_erase_element(std::string_view document, std::string_view path, bool* found) {
  Tag tag;
  std::size_t end = 0;
  if (!find_element(document, path, tag, &end)) {
    if (found != nullptr) *found = false;
    return std::string(document);
  }
  if (found != nullptr) *found = true;
  const std::size_t begin = tag.begin - whitespace_before(document, tag.begin).size();
  std::string out(document.substr(0, begin));
  out.append(document.substr(end));
  return out;
}

std::string xml_append_element(std::string_view document, std::string_view parent_path, std::string_view name,
                               std::span<const XmlEdit> attributes, bool* found) {
  return xml_append_element(document, parent_path, name, attributes, {}, {}, found);
}

std::string xml_append_element(std::string_view document, std::string_view parent_path, std::string_view name,
                               std::span<const XmlEdit> attributes, std::string_view child_name,
                               std::span<const XmlEdit> child_attributes, bool* found) {
  Tag parent;
  std::size_t end = 0;
  if (!find_element(document, parent_path, parent, &end) || parent.self_closing) {
    if (found != nullptr) *found = false;
    return std::string(document);
  }
  if (found != nullptr) *found = true;
  // The closing tag's own line: the new element goes before it, at the
  // closing tag's indentation plus one tab, its attributes two deeper.
  std::size_t closing = document.rfind('<', end - 1);
  const std::string_view before = whitespace_before(document, closing);
  std::string line_indent;
  if (const std::size_t nl = before.rfind('\n'); nl != std::string_view::npos) line_indent = std::string(before.substr(nl + 1));
  std::string out(document.substr(0, closing - before.size()));
  out += "\r\n";
  out += line_indent;
  out += '\t';
  out += '<';
  out.append(name);
  const auto write_attributes = [&](std::span<const XmlEdit> list, std::string_view indent) {
    for (const XmlEdit& attribute : list) {
      out += "\r\n";
      out += line_indent;
      out.append(indent);
      out.append(attribute.first);
      out += "=\"";
      escape_into(out, attribute.second);
      out.push_back('"');
    }
  };
  write_attributes(attributes, "\t\t");
  if (child_name.empty()) {
    out += "/>";
  } else {
    out += ">\r\n";
    out += line_indent;
    out += "\t\t<";
    out.append(child_name);
    write_attributes(child_attributes, "\t\t\t");
    out += "/>\r\n";
    out += line_indent;
    out += "\t</";
    out.append(name);
    out += '>';
  }
  out.append(document.substr(closing - before.size()));
  return out;
}

std::string xml_swap_elements(std::string_view document, std::string_view path_a, std::string_view path_b,
                              bool* found) {
  Tag a;
  Tag b;
  std::size_t end_a = 0;
  std::size_t end_b = 0;
  if (!find_element(document, path_a, a, &end_a) || !find_element(document, path_b, b, &end_b) || a.begin == b.begin) {
    if (found != nullptr) *found = false;
    return std::string(document);
  }
  if (found != nullptr) *found = true;
  if (a.begin > b.begin) {
    std::swap(a, b);
    std::swap(end_a, end_b);
  }
  // The two elements' spans, the text between them kept where it is.
  std::string out(document.substr(0, a.begin));
  out.append(document.substr(b.begin, end_b - b.begin));
  out.append(document.substr(end_a, b.begin - end_a));
  out.append(document.substr(a.begin, end_a - a.begin));
  out.append(document.substr(end_b));
  return out;
}

std::string xml_get_attribute(std::string_view document, std::string_view path, std::string_view attribute) {
  Tag tag;
  if (!find_element(document, path, tag, nullptr)) return std::string();
  return std::string(attribute_value(document, tag, attribute));
}

std::size_t xml_count_elements(std::string_view document, std::string_view path) {
  // The n-th sibling until there is none: the documents this serves hold
  // a few dozen elements at most, and the walk is the finder's own.
  std::size_t count = 0;
  Tag tag;
  while (find_element(document, std::string(path) + "[" + std::to_string(count) + "]", tag, nullptr)) ++count;
  return count;
}

std::string xml_decode_entities(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '&') {
      out.push_back(value[i]);
      continue;
    }
    const std::size_t end = value.find(';', i);
    if (end == std::string_view::npos) {
      out.push_back('&');
      continue;
    }
    const std::string_view name = value.substr(i + 1, end - i - 1);
    if (name == "lt") out.push_back('<');
    else if (name == "gt") out.push_back('>');
    else if (name == "amp") out.push_back('&');
    else if (name == "quot") out.push_back('"');
    else if (name == "apos") out.push_back('\'');
    else if (name.size() > 1 && name[0] == '#') {
      std::uint32_t code = 0;
      bool numeric = true;
      for (const char c : name.substr(1)) {
        if (c < '0' || c > '9') { numeric = false; break; }
        code = code * 10 + static_cast<std::uint32_t>(c - '0');
      }
      if (!numeric || code == 0 || code > 255) {
        out.push_back('&');
        continue;
      }
      out.push_back(static_cast<char>(code));
    } else {
      out.push_back('&');
      continue;
    }
    i = end;
  }
  return out;
}

}  // namespace imperivm::core
