#pragma once

/// The VS tokeniser.
///
/// Internal to the front end: the public entry point is `parse` in
/// `imperivm/core/script/ast.hpp`. Split out from the parser because the token
/// rules are worth reading on their own, and because the unit tests exercise a
/// few of them directly.
///
/// Zero copy throughout. Every `Token::text` is a view into the caller's source
/// buffer, which is where `Expr::text` ends up pointing too, so the buffer must
/// outlive the tree. String literals are viewed *without* their quotes and
/// *without* escape processing, matching the reference parser in
/// `src/imperivm/formats/vs_parse.py`: the only escape in all 577 scripts is
/// `\n`, and turning it into a byte would require a side buffer for no gain the
/// front end can use. Unescaping is the runtime's job.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/ast.hpp"

namespace imperivm::core::script {

enum class TokenKind : std::uint8_t {
  end,         ///< the synthetic token past the last one
  identifier,  ///< [A-Za-z_][A-Za-z0-9_]*, keywords included
  number,      ///< [0-9]+
  string,      ///< "..." or '...', text excludes the quotes
  op,          ///< an operator or a punctuator
};

struct Token {
  TokenKind kind = TokenKind::end;
  /// The token's spelling, minus the quotes for a string literal.
  std::string_view text;
  std::uint32_t line = 1;    ///< 1 based
  std::uint32_t column = 1;  ///< 1 based, in bytes from the start of the line
  std::int32_t number = 0;   ///< `number` tokens only

  [[nodiscard]] bool is(std::string_view what) const {
    // Matches the reference parser: a keyword is an identifier, so `at("if")`
    // and `at(";")` are the same operation on different token classes.
    return (kind == TokenKind::op || kind == TokenKind::identifier) && text == what;
  }
};

/// Tokenise one script. Comments are discarded; `//` runs to end of line and
/// `/* */` does not nest, so the first `*/` closes it.
///
/// On failure the result carries `FormatError::malformed` and `diagnostic`, if
/// given, is filled in with the offending position.
Result<std::vector<Token>> tokenize(std::span<const std::byte> source,
                                    std::string_view source_name, Diagnostic* diagnostic);

/// The `TypeTag` for a declared or signature type name.
///
/// The corpus names 29 distinct types and the tag set has four values, so this
/// is lossy by design; `type_name` beside every tag keeps the spelling, which
/// is what the host API is keyed on. `bool` maps to `int_` because the language
/// treats it numerically — `tgt.experience + (chance > rand(99))` is a shipped
/// line — and everything that is not `void`, `int`, `bool` or `str` is an
/// opaque handle as far as the front end is concerned.
TypeTag type_tag_for(std::string_view name);

/// The signature comment `// <ret>[, <type> [*|OUT] <name>]...`, taken from the
/// first non-blank line. Returns false when that line is not a `//` comment or
/// when a parameter field does not match the form, which is how the one
/// unsignatured script in the corpus is detected. Fills `script` in place.
///
/// Deliberately a text pass rather than a token pass: the comment is a comment,
/// the tokeniser has already thrown it away, and the engine's own convention is
/// line based.
bool parse_signature(std::span<const std::byte> source, Script& script);

}  // namespace imperivm::core::script
