// A freestanding INI reader.
// See include/imperivm/core/formats/ini.hpp.

#include "imperivm/core/formats/ini.hpp"

namespace imperivm::core {
namespace {

[[nodiscard]] constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

[[nodiscard]] constexpr char lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

/// The text of a line with any `;` comment removed.
///
/// Unconditional, because the corpus has no counter-example: all 51 lines
/// containing a double quote have it after the `;`, so no value is ever quoted
/// and no `;` is ever data.
[[nodiscard]] std::string_view strip_comment(std::string_view line) noexcept {
  const std::size_t at = line.find(';');
  return at == std::string_view::npos ? line : line.substr(0, at);
}

}  // namespace

std::string_view trim(std::string_view text) noexcept {
  std::size_t begin = 0;
  while (begin < text.size() && is_space(text[begin])) ++begin;
  std::size_t end = text.size();
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

bool parse_int(std::string_view text, std::int32_t& out) noexcept {
  text = trim(text);
  if (text.empty()) return false;

  bool negative = false;
  std::size_t i = 0;
  if (text[0] == '+' || text[0] == '-') {
    negative = text[0] == '-';
    i = 1;
  }
  if (i >= text.size()) return false;

  // Accumulated as unsigned so that overflow is detectable rather than
  // undefined, and checked against the range the sign allows.
  constexpr std::uint32_t kPositiveLimit = 2147483647u;
  constexpr std::uint32_t kNegativeLimit = 2147483648u;
  const std::uint32_t limit = negative ? kNegativeLimit : kPositiveLimit;
  std::uint32_t magnitude = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    // Anything that is not a digit fails the whole parse. Stopping here and
    // returning what was read so far is exactly how `ProductionInterval` came
    // to be recorded as 20 when it is 2000.
    if (c < '0' || c > '9') return false;
    const std::uint32_t digit = static_cast<std::uint32_t>(c - '0');
    if (magnitude > (limit - digit) / 10) return false;
    magnitude = magnitude * 10 + digit;
  }

  out = negative ? -static_cast<std::int32_t>(magnitude) : static_cast<std::int32_t>(magnitude);
  return true;
}

std::vector<std::string_view> split_list(std::string_view value) {
  std::vector<std::string_view> out;
  if (value.empty()) return out;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = value.find(',', begin);
    if (comma == std::string_view::npos) {
      out.push_back(trim(value.substr(begin)));
      break;
    }
    out.push_back(trim(value.substr(begin, comma - begin)));
    begin = comma + 1;
  }
  return out;
}

Result<IniDocument> IniDocument::parse(std::span<const std::byte> bytes) {
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  IniDocument document;

  // Entries land in one flat vector and each section records a range into it,
  // so a section's entries stay in file order -- which is the whole meaning of
  // a section like `[SquadStates]`, where the index is the constant's value.
  std::uint32_t line_number = 0;
  std::size_t cursor = 0;
  while (cursor <= text.size()) {
    const std::size_t newline = text.find('\n', cursor);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    const std::string_view raw = text.substr(cursor, end - cursor);
    ++line_number;

    const std::string_view line = trim(raw);
    if (!line.empty() && line[0] != ';') {
      if (line[0] == '[') {
        const std::size_t close = line.find(']');
        // The one rejection. A header with no `]` means the file is not what
        // the caller thinks it is, and guessing where the name ends would make
        // that failure silent.
        if (close == std::string_view::npos) return FormatError::malformed;
        if (!document.sections_.empty()) {
          IniSection& previous = document.sections_.back();
          previous.entry_count =
              static_cast<std::uint32_t>(document.entries_.size()) - previous.entry_begin;
        }
        IniSection section;
        section.name = trim(line.substr(1, close - 1));
        section.entry_begin = static_cast<std::uint32_t>(document.entries_.size());
        section.line = line_number;
        document.sections_.push_back(section);
      } else {
        const std::string_view content = trim(strip_comment(line));
        if (!content.empty()) {
          // An entry before any header goes into an implicit unnamed section.
          // No shipped file needs this; a hand-edited one might, and dropping
          // the line would be the wrong way to find out.
          if (document.sections_.empty()) {
            IniSection implicit;
            implicit.entry_begin = 0;
            implicit.line = line_number;
            document.sections_.push_back(implicit);
          }
          IniEntry entry;
          entry.line = line_number;
          const std::size_t equals = content.find('=');
          if (equals == std::string_view::npos) {
            entry.value = content;
          } else {
            entry.has_key = true;
            entry.key = trim(content.substr(0, equals));
            entry.value = trim(content.substr(equals + 1));
          }
          document.entries_.push_back(entry);
        }
      }
    }

    if (newline == std::string_view::npos) break;
    cursor = newline + 1;
  }

  if (!document.sections_.empty()) {
    IniSection& last = document.sections_.back();
    last.entry_count = static_cast<std::uint32_t>(document.entries_.size()) - last.entry_begin;
  }
  return document;
}

SectionIndex IniDocument::section(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < sections_.size(); ++i) {
    if (equal_ignoring_case(sections_[i].name, name)) return static_cast<SectionIndex>(i);
  }
  return kNoSection;
}

std::span<const IniEntry> IniDocument::entries_of(SectionIndex index) const noexcept {
  if (index >= sections_.size()) return {};
  const IniSection& section = sections_[index];
  return std::span<const IniEntry>(entries_).subspan(section.entry_begin, section.entry_count);
}

std::string_view IniDocument::value(SectionIndex section, std::string_view key,
                                    std::string_view fallback) const noexcept {
  for (const IniEntry& entry : entries_of(section)) {
    if (entry.has_key && equal_ignoring_case(entry.key, key)) return entry.value;
  }
  return fallback;
}

std::int32_t IniDocument::value_int(SectionIndex section, std::string_view key,
                                    std::int32_t fallback) const noexcept {
  std::int32_t parsed = 0;
  const std::string_view text = value(section, key);
  return parse_int(text, parsed) ? parsed : fallback;
}

}  // namespace imperivm::core
