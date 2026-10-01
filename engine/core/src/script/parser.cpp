/// The VS parser: source to `Script`.
///
/// Hand-written recursive descent, mirroring the validated reference parser in
/// `src/imperivm/formats/vs_parse.py` production for production. That parser
/// reads all 577 shipped scripts and all 113 inline ones, so where the grammar
/// is ambiguous the reference is the tie-breaker, not taste.
///
/// ## Precedence
///
/// The table below is C's, and it is **consistent with the corpus but not
/// proven by it**: every place the shipped scripts nest a looser operator
/// inside a tighter one, they parenthesise it, so a different table would parse
/// the retail data identically. This matters for user-authored scripts, not for
/// running the game. See docs/formats/vs-language.md, "What is still unknown".
///
/// ## What the AST does not have, and what is done instead
///
/// `ast.hpp` is the shared contract with the runtime, and it is smaller than
/// the surface syntax. Five constructs are therefore normalised here. All five
/// are lossless with respect to execution; only the spelling is lost.
///
///  1. **`for`** has no `StmtKind`. `for (init; cond; step) body` becomes
///     `block { init; while (cond) body }`, and the step is carried on the
///     `while_` in `Stmt::else_branch`, which is otherwise unused for loops.
///
///     The step is *not* appended to the loop body, and that is the whole
///     point: 92 of the 351 `for` loops in the corpus `continue` out of the
///     body, and a `continue` must still run the step or the loop never
///     advances. A `while_` with `else_branch != kNoNode` is a `for`, and its
///     step statement runs after the body **and at every `continue`**.
///
///  2. **Compound assignment.** `a += b` becomes `a = a + b`, with the target
///     subtree copied rather than shared, so the tree stays a tree. Only 14 of
///     the 613 compound assignments in the corpus target a subscript, and none
///     has a side-effecting subscript, so double evaluation is not observable.
///
///  3. **`true` / `false`** become integer literals 1 and 0. The language has
///     no distinct boolean value: `bool` is numerically usable, and the corpus
///     relies on it.
///
///  4. **Declarator initialisers.** `int a = 1, b;` becomes a `declare` naming
///     both, followed by one `assign` per initialised declarator, in source
///     order. An array size (`int a[10];`) is accepted by the grammar and
///     occurs zero times in the corpus; it is parsed and discarded, because
///     there is nowhere in the contract to put it.
///
///  5. **The empty statement.** A stray `;` produces nothing. In a position
///     that requires exactly one statement (`if (x) ;`) it produces an empty
///     block.
///
/// Two further points where the contract needed an interpretation:
///
///  * A `block` statement's contents are a contiguous run in
///    `Script::statements`: `then_branch` is the first index and `else_branch`
///    is the count. That is the only reading of the field comment that the
///    struct's fields permit, and it is why statements are built by value and
///    only appended once their whole subtree is known.
///  * `[ ... ]` is accepted as a block delimiter at statement position, which
///    is what the reference parser does and what `DATA\AI\SQUADMONITOR.VS` (a
///    live script, started from `MAIN.VS`) needs. Whether the original grammar
///    genuinely has two block forms or merely tolerates the typo cannot be
///    decided from one occurrence; accepting it costs nothing, because every
///    other `[` in the corpus is a subscript and a subscript never begins a
///    statement.
///
/// Assignment is a statement, never an expression, because `Stmt` models it and
/// `Expr` does not. No corpus site nests one — all 5,805 assignments are either
/// a whole statement or a `for` initialiser or step — so the restriction is
/// invisible on shipped data and produces a diagnostic on anything else.

#include "imperivm/core/script/ast.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "lexer.hpp"

namespace imperivm::core::script {
namespace {

/// Binary operators by binding strength, loosest first. C's ordering.
constexpr std::string_view kLevel0[] = {"||"};
constexpr std::string_view kLevel1[] = {"&&"};
constexpr std::string_view kLevel2[] = {"==", "!="};
constexpr std::string_view kLevel3[] = {"<", ">", "<=", ">="};
constexpr std::string_view kLevel4[] = {"+", "-"};
constexpr std::string_view kLevel5[] = {"*", "/", "%"};

constexpr std::span<const std::string_view> kLevels[] = {kLevel0, kLevel1, kLevel2,
                                                         kLevel3, kLevel4, kLevel5};
constexpr int kLevelCount = 6;

/// How deeply constructs may nest before the parser gives up.
///
/// Recursive descent recurses, and a script is untrusted input like any other
/// game file: without a cap, `((((((...` is a stack overflow rather than a
/// diagnostic, and the core is not allowed to take the process down over a bad
/// byte (see formats/result.hpp). The deepest tree in the whole corpus is 29
/// levels, in `DATA\AI\SQUADMONITOR.VS`, so this leaves an order of magnitude
/// of headroom.
constexpr int kMaxDepth = 256;

/// Not reserved words in any useful sense — type names are ordinary
/// identifiers — but these nine may not begin a declaration or stand as a
/// value, so the parser recognises them by spelling.
bool is_keyword(std::string_view text) {
  return text == "if" || text == "else" || text == "while" || text == "for" ||
         text == "break" || text == "continue" || text == "return" || text == "true" ||
         text == "false";
}

BinaryOp binary_op_for(std::string_view op) {
  if (op == "+") return BinaryOp::add;
  if (op == "-") return BinaryOp::sub;
  if (op == "*") return BinaryOp::mul;
  if (op == "/") return BinaryOp::div;
  if (op == "%") return BinaryOp::mod;
  if (op == "==") return BinaryOp::eq;
  if (op == "!=") return BinaryOp::ne;
  if (op == "<") return BinaryOp::lt;
  if (op == "<=") return BinaryOp::le;
  if (op == ">") return BinaryOp::gt;
  if (op == ">=") return BinaryOp::ge;
  if (op == "&&") return BinaryOp::logical_and;
  return BinaryOp::logical_or;
}

class Parser {
 public:
  Parser(std::vector<Token> tokens, Script& script, std::string_view source_name,
         Diagnostic* diagnostic)
      : tokens_(std::move(tokens)),
        script_(script),
        source_name_(source_name),
        diagnostic_(diagnostic) {}

  /// The ordinary entry mode: a statement list running to end of input.
  bool parse_statements() {
    while (!failed_ && tok().kind != TokenKind::end) {
      std::vector<Stmt> produced;
      statement(produced);
      for (const Stmt& s : produced) script_.body.push_back(emit(s));
    }
    return !failed_;
  }

  /// The second entry mode: one bare expression and nothing else. Used by the
  /// debug-watch snippets in `DATA\SCDEBUG.XML`, of which `.AsUnit.level` is
  /// the only one that is not an ordinary body.
  bool parse_bare_expression() {
    const NodeIndex value = expression();
    if (failed_) return false;
    if (tok().kind != TokenKind::end) {
      error(tok(), "trailing tokens after a bare expression");
      return false;
    }
    Stmt s;
    s.kind = StmtKind::expr;
    s.line = script_.expressions[value].line;
    s.expr = value;
    script_.body.push_back(emit(s));
    script_.is_expression_only = true;
    return true;
  }

 private:
  /// Counts nesting for `kMaxDepth`. Both recursion points -- one statement
  /// inside another, one expression inside another -- share the counter.
  struct DepthGuard {
    explicit DepthGuard(Parser& parser) : parser_(parser) { ++parser_.depth_; }
    ~DepthGuard() { --parser_.depth_; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
    Parser& parser_;
  };

  bool too_deep(std::string_view message) {
    if (depth_ <= kMaxDepth) return false;
    error(tok(), message);
    return true;
  }

  // -- token helpers ---------------------------------------------------

  const Token& tok() const { return tokens_[pos_]; }
  const Token& peek(std::size_t offset) const {
    const std::size_t at = pos_ + offset;
    return tokens_[at < tokens_.size() ? at : tokens_.size() - 1];
  }

  bool at(std::string_view what) const { return tok().is(what); }

  bool accept(std::string_view what) {
    if (!at(what)) return false;
    ++pos_;
    return true;
  }

  bool expect(std::string_view what, std::string_view message) {
    if (accept(what)) return true;
    error(tok(), message);
    return false;
  }

  void error(const Token& where, std::string_view message) {
    if (failed_) return;  // the first failure is the informative one
    failed_ = true;
    if (diagnostic_ == nullptr) return;
    diagnostic_->source_name = source_name_;
    diagnostic_->line = where.line;
    diagnostic_->column = where.column;
    diagnostic_->message = message;
  }

  // -- node construction -----------------------------------------------

  NodeIndex emit(const Expr& e) {
    script_.expressions.push_back(e);
    return static_cast<NodeIndex>(script_.expressions.size() - 1);
  }

  NodeIndex emit(const Stmt& s) {
    script_.statements.push_back(s);
    return static_cast<NodeIndex>(script_.statements.size() - 1);
  }

  /// Lay a statement list out as one contiguous run and wrap it in a `block`.
  Stmt make_block(const std::vector<Stmt>& children, std::uint32_t line) {
    Stmt block;
    block.kind = StmtKind::block;
    block.line = line;
    block.then_branch = static_cast<NodeIndex>(script_.statements.size());
    for (const Stmt& child : children) script_.statements.push_back(child);
    block.else_branch = static_cast<NodeIndex>(children.size());
    return block;
  }

  /// A deep copy of an expression subtree, for the compound-assignment
  /// rewrite. Sharing the index instead would turn the tree into a DAG, and
  /// every consumer would have to know.
  NodeIndex clone(NodeIndex index) {
    if (index == kNoNode) return kNoNode;
    Expr copy = script_.expressions[index];
    copy.lhs = clone(copy.lhs);
    copy.rhs = clone(copy.rhs);
    if (copy.argument_count != 0) {
      // Cloned first, appended second: a nested call clones its own arguments
      // into the same vector, so writing them as they come would interleave
      // two lists and leave neither contiguous.
      std::vector<NodeIndex> cloned;
      cloned.reserve(copy.argument_count);
      for (std::uint32_t i = 0; i < copy.argument_count; ++i) {
        cloned.push_back(clone(script_.arguments[copy.argument_begin + i]));
      }
      copy.argument_begin = static_cast<std::uint32_t>(script_.arguments.size());
      for (const NodeIndex arg : cloned) script_.arguments.push_back(arg);
    }
    return emit(copy);
  }

  // -- statements ------------------------------------------------------

  /// Parse one statement, appending the zero, one or more `Stmt`s it produces.
  /// Statements are returned by value and appended by the caller so that a
  /// block's children land in one contiguous run.
  void statement(std::vector<Stmt>& out) {
    if (failed_) return;
    DepthGuard guard(*this);
    if (too_deep("statements nested too deeply")) return;
    const Token start = tok();

    if (start.kind == TokenKind::op) {
      if (start.text == "{" || start.text == "[") {
        out.push_back(block());
        return;
      }
      if (start.text == ";") {
        ++pos_;  // the empty statement produces nothing
        return;
      }
    }

    if (start.kind == TokenKind::identifier) {
      if (start.text == "if") {
        out.push_back(if_statement());
        return;
      }
      if (start.text == "while") {
        out.push_back(while_statement());
        return;
      }
      if (start.text == "for") {
        out.push_back(for_statement());
        return;
      }
      if (start.text == "break" || start.text == "continue") {
        ++pos_;
        expect(";", "expected ';'");
        Stmt s;
        s.kind = start.text == "break" ? StmtKind::break_ : StmtKind::continue_;
        s.line = start.line;
        out.push_back(s);
        return;
      }
      if (start.text == "return") {
        ++pos_;
        Stmt s;
        s.kind = StmtKind::return_;
        s.line = start.line;
        if (!at(";")) s.expr = expression();
        expect(";", "expected ';' after return");
        out.push_back(s);
        return;
      }
      if (looks_like_declaration()) {
        declaration(out);
        return;
      }
    }

    Stmt s = simple_statement();
    expect(";", "expected ';'");
    if (failed_) return;
    out.push_back(s);
  }

  /// Exactly one statement, for the body of an `if`, `while` or `for`. A
  /// construct that expands to several statements is wrapped in a block, which
  /// is also what gives `if (x) ;` a body to point at.
  NodeIndex single_statement() {
    const std::uint32_t line = tok().line;
    std::vector<Stmt> produced;
    statement(produced);
    if (failed_) return kNoNode;
    if (produced.size() == 1) return emit(produced[0]);
    return emit(make_block(produced, line));
  }

  /// `IDENT IDENT` at statement position is always a declaration: no
  /// expression form in the language places two identifiers side by side. This
  /// is why the parser needs no table of type names, and it holds for all 577
  /// files.
  bool looks_like_declaration() const {
    if (tok().kind != TokenKind::identifier || is_keyword(tok().text)) return false;
    const Token& next = peek(1);
    return next.kind == TokenKind::identifier && !is_keyword(next.text);
  }

  void declaration(std::vector<Stmt>& out) {
    const Token type = tok();
    ++pos_;

    Stmt declare;
    declare.kind = StmtKind::declare;
    declare.line = type.line;
    declare.type_name = type.text;
    declare.declared_type = type_tag_for(type.text);
    declare.name_begin = static_cast<std::uint32_t>(script_.names.size());

    std::vector<Stmt> initialisers;
    do {
      if (failed_) return;
      const Token name = tok();
      if (name.kind != TokenKind::identifier) {
        error(name, "expected a declarator name");
        return;
      }
      ++pos_;
      script_.names.push_back(name.text);

      if (accept("[")) {
        // Grammatically legal, used nowhere in the corpus, and the contract has
        // no field for it. Parsed so the file still parses; discarded because
        // inventing storage for it would be inventing semantics.
        expression();
        expect("]", "expected ']' after an array size");
      }
      if (accept("=")) {
        Expr target;
        target.kind = ExprKind::name;
        target.line = name.line;
        target.text = name.text;
        Stmt assign;
        assign.kind = StmtKind::assign;
        assign.line = name.line;
        assign.expr = emit(target);
        assign.value = expression();
        initialisers.push_back(assign);
      }
    } while (accept(","));

    declare.name_count =
        static_cast<std::uint32_t>(script_.names.size()) - declare.name_begin;
    expect(";", "expected ';' after a declaration");
    if (failed_) return;

    out.push_back(declare);
    for (const Stmt& s : initialisers) out.push_back(s);
  }

  Stmt block() {
    const Token opener = tok();
    const std::string_view closer = opener.text == "{" ? "}" : "]";
    ++pos_;
    std::vector<Stmt> children;
    while (!at(closer)) {
      if (tok().kind == TokenKind::end) {
        error(opener, "unterminated block");
        return Stmt{};
      }
      statement(children);
      if (failed_) return Stmt{};
    }
    ++pos_;
    return make_block(children, opener.line);
  }

  Stmt if_statement() {
    const Token start = tok();
    ++pos_;
    Stmt s;
    s.kind = StmtKind::if_;
    s.line = start.line;
    expect("(", "expected '(' after if");
    s.expr = expression();
    expect(")", "expected ')' after an if condition");
    if (failed_) return s;
    s.then_branch = single_statement();
    if (!failed_ && accept("else")) s.else_branch = single_statement();
    return s;
  }

  Stmt while_statement() {
    const Token start = tok();
    ++pos_;
    Stmt s;
    s.kind = StmtKind::while_;
    s.line = start.line;
    expect("(", "expected '(' after while");
    s.expr = expression();
    expect(")", "expected ')' after a while condition");
    if (failed_) return s;
    s.then_branch = single_statement();
    return s;
  }

  /// `for (init; cond; step) body` becomes `block { init; while (cond) body }`
  /// with the step hung off the loop's `else_branch`. See the file header for
  /// why the step is not simply appended to the body.
  Stmt for_statement() {
    const Token start = tok();
    ++pos_;
    expect("(", "expected '(' after for");
    if (failed_) return Stmt{};

    std::vector<Stmt> outer;
    if (!at(";")) outer.push_back(simple_statement());
    expect(";", "expected ';' after a for initialiser");
    if (failed_) return Stmt{};

    NodeIndex condition = kNoNode;
    if (at(";")) {
      // `for (;;)` is `while (1)`. No corpus site omits the condition, but the
      // grammar allows it and a missing condition means "true", not "never".
      Expr always;
      always.kind = ExprKind::int_literal;
      always.line = start.line;
      always.int_value = 1;
      condition = emit(always);
    } else {
      condition = expression();
    }
    expect(";", "expected ';' after a for condition");
    if (failed_) return Stmt{};

    bool has_step = false;
    Stmt step;
    if (!at(")")) {
      step = simple_statement();
      has_step = true;
    }
    expect(")", "expected ')' after a for header");
    if (failed_) return Stmt{};

    const NodeIndex body = single_statement();
    if (failed_) return Stmt{};

    Stmt loop;
    loop.kind = StmtKind::while_;
    loop.line = start.line;
    loop.expr = condition;
    loop.then_branch = body;
    loop.else_branch = has_step ? emit(step) : kNoNode;
    outer.push_back(loop);
    return make_block(outer, start.line);
  }

  /// An expression or an assignment, with no terminator consumed. This is what
  /// a `for` header slot holds and what an expression statement is made of.
  Stmt simple_statement() {
    const Token start = tok();
    const NodeIndex left = expression();
    if (failed_) return Stmt{};

    const Token& op = tok();
    const bool is_assignment =
        op.kind == TokenKind::op &&
        (op.text == "=" || op.text == "+=" || op.text == "-=" || op.text == "*=" ||
         op.text == "/=" || op.text == "%=");
    if (!is_assignment) {
      Stmt s;
      s.kind = StmtKind::expr;
      s.line = start.line;
      s.expr = left;
      return s;
    }

    const Token assign_token = op;
    ++pos_;
    const NodeIndex right = expression();
    if (failed_) return Stmt{};

    Stmt s;
    s.kind = StmtKind::assign;
    s.line = assign_token.line;
    s.expr = left;
    if (assign_token.text == "=") {
      s.value = right;
    } else {
      Expr combined;
      combined.kind = ExprKind::binary;
      combined.line = assign_token.line;
      combined.binary_op = binary_op_for(assign_token.text.substr(0, 1));
      combined.lhs = clone(left);
      combined.rhs = right;
      s.value = emit(combined);
    }
    return s;
  }

  // -- expressions -----------------------------------------------------

  NodeIndex expression() { return binary(0); }

  NodeIndex binary(int level) {
    if (level >= kLevelCount) return unary();
    NodeIndex node = binary(level + 1);
    while (!failed_) {
      const Token& op = tok();
      if (op.kind != TokenKind::op) break;
      bool matches = false;
      for (const std::string_view candidate : kLevels[level]) {
        if (op.text == candidate) {
          matches = true;
          break;
        }
      }
      if (!matches) break;
      const Token op_token = op;
      ++pos_;
      const NodeIndex right = binary(level + 1);
      Expr e;
      e.kind = ExprKind::binary;
      e.line = op_token.line;
      e.binary_op = binary_op_for(op_token.text);
      e.lhs = node;
      e.rhs = right;
      node = emit(e);
    }
    return node;
  }

  NodeIndex unary() {
    DepthGuard guard(*this);
    if (too_deep("expression nested too deeply")) return kNoNode;
    const Token& op = tok();
    if (op.kind == TokenKind::op && (op.text == "!" || op.text == "-" || op.text == "+")) {
      const Token op_token = op;
      ++pos_;
      const NodeIndex operand = unary();
      if (failed_) return kNoNode;
      // Unary `+` is a no-op with no node of its own: `UnaryOp` has two values
      // and inventing a third would change the contract for a spelling.
      if (op_token.text == "+") return operand;
      Expr e;
      e.kind = ExprKind::unary;
      e.line = op_token.line;
      e.unary_op = op_token.text == "!" ? UnaryOp::logical_not : UnaryOp::negate;
      e.lhs = operand;
      return emit(e);
    }
    return postfix(primary());
  }

  /// `(` `)` `[` `]` `.` chains, left to right.
  ///
  /// The AST folds `Member` and `Call` into one `method` node, so a member
  /// access is recognised together with the call that may follow it: `u.IsValid`
  /// and `u.IsValid()` differ only in `implicit_parens`.
  NodeIndex postfix(NodeIndex node) {
    while (!failed_ && node != kNoNode) {
      if (at(".")) {
        ++pos_;
        const Token name = tok();
        if (name.kind != TokenKind::identifier) {
          error(name, "expected a member name after '.'");
          return kNoNode;
        }
        ++pos_;
        Expr e;
        e.kind = ExprKind::method;
        e.line = name.line;
        e.text = name.text;
        e.lhs = node;
        if (at("(")) {
          arguments(e);
        } else {
          e.implicit_parens = true;
        }
        node = emit(e);
      } else if (at("(")) {
        // A call whose callee is not a plain name. The corpus has none: all
        // 10,425 call sites have a `Name` or a member as callee, and the
        // contract stores the callee as `Expr::text`, so there is nowhere to
        // put a computed one.
        if (script_.expressions[node].kind != ExprKind::name) {
          error(tok(), "call of an expression that is not a name");
          return kNoNode;
        }
        Expr e = script_.expressions[node];
        e.kind = ExprKind::call;
        arguments(e);
        node = emit(e);
      } else if (at("[")) {
        const Token open = tok();
        ++pos_;
        const NodeIndex subscript = expression();
        expect("]", "expected ']' after a subscript");
        if (failed_) return kNoNode;
        Expr e;
        e.kind = ExprKind::index;
        e.line = open.line;
        e.lhs = node;
        e.rhs = subscript;
        node = emit(e);
      } else {
        return node;
      }
    }
    return node;
  }

  /// A parenthesised argument list, written into `e`. The current token is `(`.
  void arguments(Expr& e) {
    ++pos_;
    std::vector<NodeIndex> args;
    if (!at(")")) {
      do {
        if (failed_) return;
        args.push_back(expression());
      } while (accept(","));
    }
    if (!expect(")", "expected ')' after an argument list")) return;
    e.argument_begin = static_cast<std::uint32_t>(script_.arguments.size());
    e.argument_count = static_cast<std::uint32_t>(args.size());
    for (const NodeIndex arg : args) script_.arguments.push_back(arg);
  }

  NodeIndex primary() {
    const Token start = tok();

    if (start.kind == TokenKind::number) {
      ++pos_;
      Expr e;
      e.kind = ExprKind::int_literal;
      e.line = start.line;
      e.int_value = start.number;
      e.text = start.text;
      return emit(e);
    }

    if (start.kind == TokenKind::string) {
      ++pos_;
      Expr e;
      e.kind = ExprKind::string_literal;
      e.line = start.line;
      e.text = start.text;
      return emit(e);
    }

    if (start.kind == TokenKind::identifier) {
      if (start.text == "true" || start.text == "false") {
        ++pos_;
        Expr e;
        e.kind = ExprKind::int_literal;
        e.line = start.line;
        e.int_value = start.text == "true" ? 1 : 0;
        e.text = start.text;
        return emit(e);
      }
      if (is_keyword(start.text)) {
        error(start, "keyword in an expression");
        return kNoNode;
      }
      ++pos_;
      Expr e;
      e.kind = ExprKind::name;
      e.line = start.line;
      e.text = start.text;
      return emit(e);
    }

    if (at("(")) {
      ++pos_;
      const NodeIndex inner = expression();
      expect(")", "expected ')'");
      return failed_ ? kNoNode : inner;
    }

    if (at(".")) {
      // A leading '.' is exactly 'this.', where `this` is an ordinary local.
      // The dot is left for `postfix` to consume as the member operator, so
      // `.health` and `this.health` take the same path from here on.
      Expr e;
      e.kind = ExprKind::this_;
      e.line = start.line;
      return emit(e);
    }

    error(start, "unexpected token in an expression");
    return kNoNode;
  }

  std::vector<Token> tokens_;
  Script& script_;
  std::string_view source_name_;
  Diagnostic* diagnostic_ = nullptr;
  std::size_t pos_ = 0;
  int depth_ = 0;
  bool failed_ = false;
};

}  // namespace

Result<Script> parse(std::span<const std::byte> source, std::string_view source_name,
                     Diagnostic* diagnostic) {
  Script script;
  script.source_name = source_name;
  parse_signature(source, script);

  auto tokens = tokenize(source, source_name, diagnostic);
  if (!tokens) return tokens.error();

  {
    Parser parser(*tokens, script, source_name, diagnostic);
    if (parser.parse_statements()) return script;
  }

  // The statement grammar failed. Before reporting that, try the second entry
  // mode: a bare expression with no `return` and no `;`, which is how the
  // debug watches in DATA\SCDEBUG.XML are written (`.AsUnit.level`). Only one
  // of the 113 inline scripts needs this, and no `.vs` file does, so it is a
  // fallback rather than a mode the caller has to choose.
  Script bare;
  bare.source_name = source_name;
  parse_signature(source, bare);
  Diagnostic ignored;
  Parser expression_parser(std::move(*tokens), bare, source_name, &ignored);
  if (expression_parser.parse_bare_expression()) {
    if (diagnostic != nullptr) *diagnostic = Diagnostic{};
    return bare;
  }
  return FormatError::malformed;
}

}  // namespace imperivm::core::script
