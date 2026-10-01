#pragma once

// The `vs` subcommand of imcheck: parse every script in an archive and print a
// canonical serialisation of the tree.
//
// The point is a byte-for-byte diff against the same dump produced by the
// validated reference parser (`engine/tools/vs_dump.py`, driving
// `src/imperivm/formats/vs_parse.py`). That parser reads all 577 shipped
// scripts, so agreement with it is the only evidence that the C++ front end is
// right; compiling is not evidence of anything.
//
// The serialisation is deliberately positional — one node per line, kind first,
// children indented — so a disagreement shows up as a short diff hunk naming
// the construct rather than as a wall of moved text. It records node kinds,
// operators, names, literals and arities, plus the line number of every
// statement. It does *not* record expression line numbers: the reference
// parser attributes them to the operator token and this one to the leftmost
// token of the subtree, a difference with no meaning that would drown the
// diff.
//
// The AST is the normalised one described in engine/core/src/script/parser.cpp
// — no `for`, no compound assignment, no boolean literals — so the Python side
// applies the same five rewrites before dumping. That keeps the comparison a
// test of the parser rather than a test of the rewrite.
//
// It lives in engine/tools because it opens files, which the core may not.

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/pak.hpp"
#include "imperivm/core/script/ast.hpp"

namespace imperivm::vsdump {

// Only the script namespace is opened. `imperivm::core` also declares
// `NodeIndex` and `kNoNode`, for the XML tree, and pulling both in makes every
// mention of either ambiguous.
using namespace imperivm::core::script;
using imperivm::core::PakDirectory;

inline void print_escaped(std::string_view text) {
  for (const char c : text) {
    switch (c) {
      case '\\': std::fputs("\\\\", stdout); break;
      case '\n': std::fputs("\\n", stdout); break;
      case '\r': std::fputs("\\r", stdout); break;
      case '\t': std::fputs("\\t", stdout); break;
      default: std::fputc(c, stdout); break;
    }
  }
}

inline const char* binary_op_name(BinaryOp op) {
  switch (op) {
    case BinaryOp::add: return "+";
    case BinaryOp::sub: return "-";
    case BinaryOp::mul: return "*";
    case BinaryOp::div: return "/";
    case BinaryOp::mod: return "%";
    case BinaryOp::eq: return "==";
    case BinaryOp::ne: return "!=";
    case BinaryOp::lt: return "<";
    case BinaryOp::le: return "<=";
    case BinaryOp::gt: return ">";
    case BinaryOp::ge: return ">=";
    case BinaryOp::logical_and: return "&&";
    case BinaryOp::logical_or: return "||";
  }
  return "?";
}

inline void indent(int depth) {
  for (int i = 0; i < depth; ++i) std::fputs("  ", stdout);
}

inline void dump_expr(const Script& script, NodeIndex index, int depth) {
  indent(depth);
  if (index == kNoNode) {
    std::fputs("<none>\n", stdout);
    return;
  }
  const Expr& e = script.expr(index);
  switch (e.kind) {
    case ExprKind::int_literal:
      std::printf("int %d\n", e.int_value);
      return;
    case ExprKind::string_literal:
      std::fputs("str ", stdout);
      print_escaped(e.text);
      std::fputc('\n', stdout);
      return;
    case ExprKind::name:
      std::printf("name %.*s\n", static_cast<int>(e.text.size()), e.text.data());
      return;
    case ExprKind::this_:
      std::fputs("this\n", stdout);
      return;
    case ExprKind::unary:
      std::printf("unary %s\n", e.unary_op == UnaryOp::negate ? "-" : "!");
      dump_expr(script, e.lhs, depth + 1);
      return;
    case ExprKind::binary:
      std::printf("binary %s\n", binary_op_name(e.binary_op));
      dump_expr(script, e.lhs, depth + 1);
      dump_expr(script, e.rhs, depth + 1);
      return;
    case ExprKind::index:
      std::fputs("index\n", stdout);
      dump_expr(script, e.lhs, depth + 1);
      dump_expr(script, e.rhs, depth + 1);
      return;
    case ExprKind::call:
      std::printf("call %.*s %u\n", static_cast<int>(e.text.size()), e.text.data(),
                  e.argument_count);
      for (std::uint32_t i = 0; i < e.argument_count; ++i) {
        dump_expr(script, script.arguments[e.argument_begin + i], depth + 1);
      }
      return;
    case ExprKind::method:
      std::printf("method %.*s %u %d\n", static_cast<int>(e.text.size()), e.text.data(),
                  e.argument_count, e.implicit_parens ? 1 : 0);
      dump_expr(script, e.lhs, depth + 1);
      for (std::uint32_t i = 0; i < e.argument_count; ++i) {
        dump_expr(script, script.arguments[e.argument_begin + i], depth + 1);
      }
      return;
  }
}

inline void dump_stmt(const Script& script, NodeIndex index, int depth) {
  indent(depth);
  if (index == kNoNode) {
    std::fputs("<none>\n", stdout);
    return;
  }
  const Stmt& s = script.stmt(index);
  switch (s.kind) {
    case StmtKind::declare:
      std::printf("declare %u %.*s", s.line, static_cast<int>(s.type_name.size()),
                  s.type_name.data());
      for (std::uint32_t i = 0; i < s.name_count; ++i) {
        const std::string_view name = script.names[s.name_begin + i];
        std::printf(" %.*s", static_cast<int>(name.size()), name.data());
      }
      std::fputc('\n', stdout);
      return;
    case StmtKind::assign:
      std::printf("assign %u\n", s.line);
      dump_expr(script, s.expr, depth + 1);
      dump_expr(script, s.value, depth + 1);
      return;
    case StmtKind::expr:
      std::printf("expr %u\n", s.line);
      dump_expr(script, s.expr, depth + 1);
      return;
    case StmtKind::if_:
      std::printf("if %u %d\n", s.line, s.else_branch != kNoNode ? 1 : 0);
      dump_expr(script, s.expr, depth + 1);
      dump_stmt(script, s.then_branch, depth + 1);
      if (s.else_branch != kNoNode) dump_stmt(script, s.else_branch, depth + 1);
      return;
    case StmtKind::while_:
      // `else_branch` carries the step statement of a desugared `for`; see
      // engine/core/src/script/parser.cpp.
      std::printf("while %u %d\n", s.line, s.else_branch != kNoNode ? 1 : 0);
      dump_expr(script, s.expr, depth + 1);
      dump_stmt(script, s.then_branch, depth + 1);
      if (s.else_branch != kNoNode) dump_stmt(script, s.else_branch, depth + 1);
      return;
    case StmtKind::return_:
      std::printf("return %u %d\n", s.line, s.expr != kNoNode ? 1 : 0);
      if (s.expr != kNoNode) dump_expr(script, s.expr, depth + 1);
      return;
    case StmtKind::break_:
      std::printf("break %u\n", s.line);
      return;
    case StmtKind::continue_:
      std::printf("continue %u\n", s.line);
      return;
    case StmtKind::block:
      std::printf("block %u %u\n", s.line, s.else_branch);
      for (std::uint32_t i = 0; i < s.else_branch; ++i) {
        dump_stmt(script, s.then_branch + i, depth + 1);
      }
      return;
  }
}

inline void dump_script(std::string_view name, const Script& script) {
  std::printf("=== %.*s\n", static_cast<int>(name.size()), name.data());
  std::printf("sig %d %.*s\n", script.has_signature ? 1 : 0,
              static_cast<int>(script.return_type_name.size()),
              script.return_type_name.data());
  for (const Parameter& p : script.parameters) {
    std::printf("param %.*s %d %.*s\n", static_cast<int>(p.type_name.size()),
                p.type_name.data(), p.is_out ? 1 : 0, static_cast<int>(p.name.size()),
                p.name.data());
  }
  std::printf("exprmode %d\n", script.is_expression_only ? 1 : 0);
  for (const NodeIndex index : script.body) dump_stmt(script, index, 1);
}

/// The five predefined XML entities, plus numeric references. Hand-rolled here
/// and mirrored in the Python driver so that the two sides cannot disagree
/// about the decoding rather than about the parse.
inline std::string decode_entities(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] != '&') {
      out.push_back(text[i++]);
      continue;
    }
    const std::size_t end = text.find(';', i);
    if (end == std::string_view::npos || end - i > 10) {
      out.push_back(text[i++]);
      continue;
    }
    const std::string_view name = text.substr(i + 1, end - i - 1);
    if (name == "lt") {
      out.push_back('<');
    } else if (name == "gt") {
      out.push_back('>');
    } else if (name == "amp") {
      out.push_back('&');
    } else if (name == "quot") {
      out.push_back('"');
    } else if (name == "apos") {
      out.push_back('\'');
    } else if (!name.empty() && name[0] == '#') {
      unsigned value = 0;
      const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
      for (std::size_t k = hex ? 2 : 1; k < name.size(); ++k) {
        const char c = name[k];
        const unsigned digit = (c >= '0' && c <= '9')   ? static_cast<unsigned>(c - '0')
                               : (c >= 'a' && c <= 'f') ? static_cast<unsigned>(c - 'a' + 10)
                               : (c >= 'A' && c <= 'F') ? static_cast<unsigned>(c - 'A' + 10)
                                                        : 16u;
        if (digit >= (hex ? 16u : 10u)) {
          value = 0xFFFFFFFFu;
          break;
        }
        value = value * (hex ? 16u : 10u) + digit;
      }
      if (value < 128u) {
        out.push_back(static_cast<char>(value));
      } else {
        out.append(text.substr(i, end - i + 1));
      }
    } else {
      out.append(text.substr(i, end - i + 1));
      i = end + 1;
      continue;
    }
    i = end + 1;
  }
  return out;
}

/// True when a `script="..."` value is a file reference rather than inline
/// source. 46 of the 160 such attributes in `data.pak` name a `.vs` file; the
/// rest are source.
inline bool looks_like_a_path(std::string_view value) {
  if (value.size() < 4) return false;
  const std::string_view tail = value.substr(value.size() - 3);
  if (!((tail[0] == '.') && (tail[1] == 'v' || tail[1] == 'V') &&
        (tail[2] == 's' || tail[2] == 'S'))) {
    return false;
  }
  for (const char c : value) {
    const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == ' ' || c == '/' ||
                         c == '\\' || c == '.' || c == '-';
    if (!allowed) return false;
  }
  return true;
}

inline bool ends_with_ignoring_case(std::string_view text, std::string_view suffix) {
  if (text.size() < suffix.size()) return false;
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    char a = text[text.size() - suffix.size() + i];
    char b = suffix[i];
    if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
    if (a != b) return false;
  }
  return true;
}

/// Parse and dump every script in the archive: first the `.VS` files in stored
/// order, then the inline `script="..."` attributes of every XML file, in the
/// order they appear in the text.
inline int run(std::span<const std::byte> archive, const char* label) {
  auto directory = PakDirectory::parse(archive);
  if (!directory) {
    std::fprintf(stderr, "%s: not a pack (error %d)\n", label,
                 static_cast<int>(directory.error()));
    return 1;
  }

  std::size_t scripts = 0;
  std::size_t inline_scripts = 0;
  std::size_t failures = 0;

  const auto one = [&](std::string_view name, std::span<const std::byte> source) {
    Diagnostic diagnostic;
    const auto parsed = parse(source, name, &diagnostic);
    if (!parsed) {
      ++failures;
      std::printf("=== %.*s\nFAILED %u %u %.*s\n", static_cast<int>(name.size()), name.data(),
                  diagnostic.line, diagnostic.column,
                  static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
      return;
    }
    dump_script(name, *parsed);
  };

  for (const auto& entry : directory->entries()) {
    if (!ends_with_ignoring_case(entry.name, ".VS")) continue;
    const auto blob = directory->read(entry);
    if (!blob) {
      ++failures;
      continue;
    }
    ++scripts;
    one(entry.name, *blob);
  }

  for (const auto& entry : directory->entries()) {
    if (!ends_with_ignoring_case(entry.name, ".XML")) continue;
    const auto blob = directory->read(entry);
    if (!blob) continue;
    const std::string_view text(reinterpret_cast<const char*>(blob->data()), blob->size());

    std::size_t at = 0;
    unsigned ordinal = 0;
    while (true) {
      const std::size_t found = text.find("script=\"", at);
      if (found == std::string_view::npos) break;
      // Only an attribute, never a substring of a longer name: `use_script="`
      // and `kill_script="` are file URLs and are not inline source.
      const bool is_attribute =
          found == 0 || text[found - 1] == ' ' || text[found - 1] == '\t' ||
          text[found - 1] == '\n' || text[found - 1] == '\r';
      const std::size_t value_begin = found + 8;
      const std::size_t value_end = text.find('"', value_begin);
      if (value_end == std::string_view::npos) break;
      at = value_end + 1;
      if (!is_attribute) continue;

      const std::string source = decode_entities(text.substr(value_begin, value_end - value_begin));
      if (looks_like_a_path(source)) continue;

      char suffix[16];
      std::snprintf(suffix, sizeof(suffix), "#%u", ordinal++);
      const std::string name = std::string(entry.name) + suffix;
      ++inline_scripts;
      one(name, std::span<const std::byte>(reinterpret_cast<const std::byte*>(source.data()),
                                           source.size()));
    }
  }

  std::fprintf(stderr, "%s: %zu scripts, %zu inline, %zu failures\n", label, scripts,
               inline_scripts, failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace imperivm::vsdump
