#pragma once

/// The VS language's syntax tree, and the value model it operates on.
///
/// VS is the engine's scripting language. All 577 shipped scripts parse, and
/// the language is small: nine keywords, no bitwise operators, no ternary, no
/// switch, no preprocessor, no user-defined functions, no recursion. See
/// docs/formats/vs-language.md for the grammar and docs/formats/vs-host-api.md
/// for the 705-entry host surface.
///
/// This header is the seam between the front end (source to AST) and the
/// runtime (AST to bytecode to execution). Both are being written against it.
///
/// ## Why a bytecode VM rather than a tree-walking interpreter
///
/// Scripts are coroutines. `Sleep` yields, `AIRun` spawns concurrently, and 101
/// scripts are infinite loops around a sleep. So execution must suspend at an
/// arbitrary point and resume later.
///
/// A tree-walking interpreter would need the host stack to suspend, which means
/// real coroutines or threads. Both are wrong here for the same reason: **a
/// suspended script is world state**. The original engine's desync dumps record
/// a running script per object (`script=data/subai/deer_idle.vs id=1937`), so
/// script state is saved, loaded, and hashed alongside everything else. A
/// bytecode VM with an explicit frame stack serialises as plain data; a
/// suspended native coroutine does not.
///
/// Determinism points the same way. An explicit interpreter loop has no
/// dependence on host stack layout, compiler inlining, or optimisation level.
///
/// So: the front end produces the AST declared here, the runtime compiles it to
/// bytecode, and execution state is an explicit, serialisable frame stack.
///
/// ## Storage
///
/// Nodes live in flat vectors and refer to each other by index, matching
/// `xml.hpp`. `kNoNode` is the null index. String views point into the caller's
/// source buffer, which must outlive the tree.
///
/// ## Lowering
///
/// This tree is smaller than the surface syntax, on purpose: fewer node kinds
/// mean fewer cases in the compiler, and the discarded distinctions carry no
/// meaning. The front end normalises
///
///   - `for (init; cond; step)` into a block holding `init` and a `while_`
///     whose step rides on `else_branch` (see the note there, which is a
///     correctness trap rather than a detail),
///   - `a += b` into `a = a + b`, copying the target subtree,
///   - `true` and `false` into integer 1 and 0,
///   - a declarator with an initialiser into `declare` followed by `assign`,
///   - a stray `;` into nothing, or an empty block where a statement is
///     required,
///   - unary `+` into its operand.
///
/// Array declarators (`int a[10];`) are grammatical but occur zero times in the
/// corpus, and there is no field for the size, so they parse and the size is
/// discarded. Assignment is statement-only; no corpus site nests one.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::script {

using NodeIndex = std::uint32_t;

inline constexpr NodeIndex kNoNode = 0xFFFFFFFFu;

/// The declared types in the corpus. Object types are opaque handles as far as
/// the language is concerned; what they can do lives in the host API.
enum class TypeTag : std::uint8_t {
  void_,
  int_,
  string_,
  object,  ///< any handle type: Obj, Unit, Settlement, GAIKA, Squad, ...
};

enum class ExprKind : std::uint8_t {
  int_literal,
  string_literal,
  name,          ///< a local, parameter, or global constant
  unary,         ///< - !
  binary,        ///< + - * / % == != < <= > >= && ||
  call,          ///< free function: Foo(a, b)
  method,        ///< receiver.Name(a, b), or receiver.Name with parens omitted
  index,         ///< a[i]
  this_,         ///< a leading '.' is exactly 'this.', where this is a local
};

enum class StmtKind : std::uint8_t {
  declare,   ///< int a, b;  Obj o;
  assign,    ///< a = expr;  a[i] = expr;
  expr,      ///< a call evaluated for effect
  if_,
  while_,
  return_,
  break_,
  continue_,
  block,
};

enum class BinaryOp : std::uint8_t {
  add, sub, mul, div, mod,
  eq, ne, lt, le, gt, ge,
  logical_and, logical_or,
};

enum class UnaryOp : std::uint8_t { negate, logical_not };

struct Expr {
  ExprKind kind = ExprKind::name;
  std::uint32_t line = 0;

  /// `int_literal` only.
  std::int32_t int_value = 0;
  /// `name`, `string_literal`, `call`, `method`: the identifier or text.
  std::string_view text;

  BinaryOp binary_op = BinaryOp::add;
  UnaryOp unary_op = UnaryOp::negate;

  /// `unary`/`binary` operands, `method`/`index` receiver.
  NodeIndex lhs = kNoNode;
  NodeIndex rhs = kNoNode;

  /// Arguments for `call` and `method`, as a range into `Script::arguments`.
  std::uint32_t argument_begin = 0;
  std::uint32_t argument_count = 0;

  /// True when a zero-argument method was written without parentheses.
  ///
  /// Both spellings occur, in the same files: `u.IsValid` and `u.IsValid()`.
  /// The consequence is that **there is no property namespace** — dispatch is a
  /// flat table keyed by name and arity — so this flag is for faithful
  /// round-tripping and diagnostics, never for dispatch.
  bool implicit_parens = false;
};

struct Stmt {
  StmtKind kind = StmtKind::expr;
  std::uint32_t line = 0;

  /// `declare`: the declared type. Otherwise unused.
  TypeTag declared_type = TypeTag::int_;
  /// `declare`: the type's spelling, since object types are distinguished only
  /// by name (`GAIKA`, `Squad`, `Obj`), and the host API needs the name.
  std::string_view type_name;

  /// `assign` target, `if_`/`while_` condition, `return_` value, `expr` value.
  NodeIndex expr = kNoNode;
  /// `assign` value.
  NodeIndex value = kNoNode;

  /// `if_` then-branch and else-branch; `while_` body.
  ///
  /// For `block`, these two fields carry a **range** rather than two branches:
  /// `then_branch` is the index of the first contained statement and
  /// `else_branch` is the count, over a contiguous run in
  /// `Script::statements`. Statements must therefore be appended only once
  /// their whole subtree is complete, so that a block's children stay adjacent.
  ///
  /// For `while_`, `else_branch` optionally holds a **step statement**, which
  /// is how a source-level `for` is represented after lowering. It is
  /// `kNoNode` on a plain `while`.
  ///
  /// The step is deliberately not appended to the loop body, and getting this
  /// wrong produces a hang rather than a wrong answer: **92 of the 351 `for`
  /// loops in the corpus `continue` out of the body**, and a `continue` that
  /// skipped the step would never advance the loop variable. The step runs
  /// after the body *and* on every `continue`.
  NodeIndex then_branch = kNoNode;
  NodeIndex else_branch = kNoNode;

  /// `declare`: names declared in this statement, a range into `Script::names`.
  std::uint32_t name_begin = 0;
  std::uint32_t name_count = 0;
};

/// One parameter from the signature comment.
///
/// 576 of the 577 scripts open with a comment of the form
/// `// <ret>[, <type> [*|OUT] <name>]...`, which gives the whole type system
/// without inference. The one exception is a dead item script that nothing
/// references.
struct Parameter {
  TypeTag type = TypeTag::int_;
  std::string_view type_name;
  std::string_view name;
  /// Marked `*` or `OUT`. Note the corpus also writes through arguments with no
  /// syntactic marker at all (`Eval`, `GetSquads`, `ParseStr`), so this flag
  /// records what was declared, not everything that mutates.
  bool is_out = false;
};

/// A parsed script.
struct Script {
  std::string_view source_name;

  TypeTag return_type = TypeTag::void_;
  std::string_view return_type_name;
  std::vector<Parameter> parameters;
  /// False when no signature comment was present; the single such script still
  /// parses and runs, it simply has no declared types.
  bool has_signature = false;

  /// A bare expression with no `return`, which is a second entry mode used by
  /// inline `script="..."` attributes in the class XML (for example
  /// `.AsUnit.level`). 112 of the 113 inline scripts are ordinary bodies.
  bool is_expression_only = false;

  std::vector<Expr> expressions;
  std::vector<Stmt> statements;
  /// Argument lists, referenced by `Expr::argument_begin/count`.
  std::vector<NodeIndex> arguments;
  /// Declared names, referenced by `Stmt::name_begin/count`.
  std::vector<std::string_view> names;

  /// Top-level statements, in order.
  std::vector<NodeIndex> body;

  [[nodiscard]] const Expr& expr(NodeIndex i) const { return expressions[i]; }
  [[nodiscard]] const Stmt& stmt(NodeIndex i) const { return statements[i]; }
};

/// Where a parse failed. Scripts are shipped data, so a failure is a finding
/// worth reporting precisely rather than an exception to unwind.
struct Diagnostic {
  std::string_view source_name;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::string_view message;
};

/// Parse VS source into a `Script`. Implemented by the front end.
Result<Script> parse(std::span<const std::byte> source, std::string_view source_name,
                     Diagnostic* diagnostic = nullptr);

}  // namespace imperivm::core::script
