#include "lexer.hpp"

#include <cstddef>

namespace imperivm::core::script {
namespace {

constexpr bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
constexpr bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

/// Every multi-character operator in the language, longest first so that the
/// match is greedy: `<=` must not lex as `<` then `=`.
///
/// `*=`, `/=` and `%=` are accepted although nothing in the corpus uses them,
/// matching the reference parser. The corpus uses only `=`, `+=` and `-=`.
constexpr std::string_view kTwoCharOps[] = {"==", "!=", "<=", ">=", "&&", "||",
                                            "+=", "-=", "*=", "/=", "%="};

constexpr std::string_view kOneCharOps = "+-*/%<>=!.,;()[]{}";

void fail(Diagnostic* diagnostic, std::string_view source_name, std::uint32_t line,
          std::uint32_t column, std::string_view message) {
  if (diagnostic == nullptr) return;
  diagnostic->source_name = source_name;
  diagnostic->line = line;
  diagnostic->column = column;
  diagnostic->message = message;
}

}  // namespace

Result<std::vector<Token>> tokenize(std::span<const std::byte> source,
                                    std::string_view source_name, Diagnostic* diagnostic) {
  const char* const text = reinterpret_cast<const char*>(source.data());
  const std::size_t size = source.size();

  std::vector<Token> tokens;
  // One token per three source bytes is a close over-estimate for this corpus
  // and costs one allocation instead of a dozen.
  tokens.reserve(size / 3 + 8);

  std::size_t pos = 0;
  std::uint32_t line = 1;
  std::size_t line_start = 0;

  const auto column_of = [&](std::size_t at) {
    return static_cast<std::uint32_t>(at - line_start + 1);
  };

  while (pos < size) {
    const char c = text[pos];

    if (c == '\n') {
      ++line;
      ++pos;
      line_start = pos;
      continue;
    }
    if (is_space(c)) {
      ++pos;
      continue;
    }

    if (c == '/' && pos + 1 < size && text[pos + 1] == '/') {
      while (pos < size && text[pos] != '\n') ++pos;
      continue;
    }
    if (c == '/' && pos + 1 < size && text[pos + 1] == '*') {
      const std::uint32_t open_line = line;
      const std::uint32_t open_column = column_of(pos);
      std::size_t scan = pos + 2;
      bool closed = false;
      while (scan + 1 < size) {
        if (text[scan] == '\n') {
          ++line;
          line_start = scan + 1;
        } else if (text[scan] == '*' && text[scan + 1] == '/') {
          closed = true;
          break;
        }
        ++scan;
      }
      if (!closed) {
        fail(diagnostic, source_name, open_line, open_column, "unterminated block comment");
        return FormatError::malformed;
      }
      pos = scan + 2;
      continue;
    }

    Token token;
    token.line = line;
    token.column = column_of(pos);

    if (c == '"' || c == '\'') {
      const std::size_t begin = pos + 1;
      std::size_t scan = begin;
      // A literal never spans a line: an unterminated one is a typo, and
      // swallowing the rest of the file would turn it into a baffling error
      // hundreds of lines later.
      while (scan < size && text[scan] != c && text[scan] != '\n') {
        if (text[scan] == '\\' && scan + 1 < size && text[scan + 1] != '\n') ++scan;
        ++scan;
      }
      if (scan >= size || text[scan] != c) {
        fail(diagnostic, source_name, token.line, token.column, "unterminated string literal");
        return FormatError::malformed;
      }
      token.kind = TokenKind::string;
      token.text = std::string_view(text + begin, scan - begin);
      tokens.push_back(token);
      pos = scan + 1;
      continue;
    }

    if (is_digit(c)) {
      const std::size_t begin = pos;
      std::uint32_t value = 0;
      while (pos < size && is_digit(text[pos])) {
        // Wraps rather than saturating. No literal in the corpus comes within
        // three orders of magnitude of the limit; wrapping is at least a
        // defined behaviour to test against.
        value = value * 10u + static_cast<std::uint32_t>(text[pos] - '0');
        ++pos;
      }
      token.kind = TokenKind::number;
      token.text = std::string_view(text + begin, pos - begin);
      token.number = static_cast<std::int32_t>(value);
      tokens.push_back(token);
      continue;
    }

    if (is_alpha(c)) {
      const std::size_t begin = pos;
      while (pos < size && is_alnum(text[pos])) ++pos;
      token.kind = TokenKind::identifier;
      token.text = std::string_view(text + begin, pos - begin);
      tokens.push_back(token);
      continue;
    }

    bool matched = false;
    if (pos + 1 < size) {
      const std::string_view pair(text + pos, 2);
      for (const std::string_view op : kTwoCharOps) {
        if (pair == op) {
          token.kind = TokenKind::op;
          token.text = std::string_view(text + pos, 2);
          tokens.push_back(token);
          pos += 2;
          matched = true;
          break;
        }
      }
    }
    if (matched) continue;

    if (kOneCharOps.find(c) != std::string_view::npos) {
      token.kind = TokenKind::op;
      token.text = std::string_view(text + pos, 1);
      tokens.push_back(token);
      ++pos;
      continue;
    }

    fail(diagnostic, source_name, token.line, token.column, "unexpected character");
    return FormatError::malformed;
  }

  Token end;
  end.kind = TokenKind::end;
  end.line = line;
  end.column = column_of(pos);
  tokens.push_back(end);
  return tokens;
}

namespace {

/// `[A-Za-z_][A-Za-z0-9_]*` starting at `at`; returns the length, or 0.
std::size_t identifier_length(std::string_view field, std::size_t at) {
  if (at >= field.size() || !is_alpha(field[at])) return 0;
  std::size_t end = at;
  while (end < field.size() && is_alnum(field[end])) ++end;
  return end - at;
}

std::string_view trim(std::string_view s) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end && is_space(s[begin])) ++begin;
  while (end > begin && is_space(s[end - 1])) --end;
  return s.substr(begin, end - begin);
}

/// One `<type> [*|OUT] <name>` field. Mirrors the reference parser's regex
/// exactly, including its tolerance of `int*name`, `int *name`, `int* name`
/// and `int OUT name`.
bool parse_parameter(std::string_view field, Parameter& out) {
  field = trim(field);
  std::size_t at = 0;
  const std::size_t type_length = identifier_length(field, at);
  if (type_length == 0) return false;
  const std::string_view type_name = field.substr(at, type_length);
  at += type_length;

  bool is_out = false;
  const std::size_t after_type = at;
  while (at < field.size() && is_space(field[at])) ++at;

  if (at < field.size() && field[at] == '*') {
    is_out = true;
    ++at;
    while (at < field.size() && is_space(field[at])) ++at;
  } else if (at > after_type && field.compare(at, 3, "OUT") == 0 && at + 3 < field.size() &&
             is_space(field[at + 3])) {
    // `OUT` must be surrounded by whitespace, so a parameter genuinely named
    // `OUTpost` is not mistaken for an out marker.
    is_out = true;
    at += 3;
    while (at < field.size() && is_space(field[at])) ++at;
  } else if (at == after_type) {
    // `intfoo` is one identifier, not a type and a name.
    return false;
  }

  const std::size_t name_length = identifier_length(field, at);
  if (name_length == 0) return false;
  const std::string_view name = field.substr(at, name_length);
  if (at + name_length != field.size()) return false;

  out.type_name = type_name;
  out.name = name;
  out.is_out = is_out;
  out.type = type_tag_for(type_name);
  return true;
}

}  // namespace

TypeTag type_tag_for(std::string_view name) {
  if (name == "void") return TypeTag::void_;
  if (name == "int" || name == "bool") return TypeTag::int_;
  if (name == "str") return TypeTag::string_;
  return TypeTag::object;
}

bool parse_signature(std::span<const std::byte> source, Script& script) {
  const char* const text = reinterpret_cast<const char*>(source.data());
  const std::size_t size = source.size();

  std::size_t pos = 0;
  while (pos < size) {
    std::size_t end = pos;
    while (end < size && text[end] != '\n' && text[end] != '\r') ++end;
    const std::string_view raw(text + pos, end - pos);
    const std::string_view line = trim(raw);

    // Advance past the line terminator, treating CRLF as one break.
    std::size_t next = end;
    if (next < size && text[next] == '\r') ++next;
    if (next < size && text[next] == '\n') ++next;
    pos = next;

    if (line.empty()) continue;
    if (line.size() < 2 || line[0] != '/' || line[1] != '/') return false;

    // Nothing is written back until every field has parsed: a half-applied
    // signature is worse than none, and the one script without a valid one
    // must come out indistinguishable from a script with no comment at all.
    const std::string_view rest = line.substr(2);
    std::size_t field_begin = 0;
    bool first = true;
    std::string_view return_type;
    std::vector<Parameter> parameters;
    while (true) {
      const std::size_t comma = rest.find(',', field_begin);
      const std::string_view field =
          rest.substr(field_begin, comma == std::string_view::npos
                                       ? std::string_view::npos
                                       : comma - field_begin);
      if (first) {
        return_type = trim(field);
        first = false;
      } else {
        Parameter parameter;
        if (!parse_parameter(field, parameter)) return false;
        parameters.push_back(parameter);
      }
      if (comma == std::string_view::npos) break;
      field_begin = comma + 1;
    }
    script.return_type_name = return_type;
    script.return_type = type_tag_for(return_type);
    script.parameters = std::move(parameters);
    script.has_signature = true;
    return true;
  }
  return false;
}

}  // namespace imperivm::core::script
