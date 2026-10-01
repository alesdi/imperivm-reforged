// Front-end tests for the VS language: source in, AST out.
//
// These need no game data, because CI has no installation. What they cannot do
// is prove the parser right — only the corpus can, and `imcheck vs` does that
// by diffing 691 canonical dumps against the validated Python parser. What they
// do instead is pin the decisions that dump cannot see: the precedence table
// (which the corpus parenthesises around, so it never exercises it), the
// normalisations the contract forces, and the diagnostics, which the corpus has
// no examples of at all because every shipped script parses.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "imperivm/core/script/ast.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::script;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

Result<Script> parse_text(std::string_view text, Diagnostic* diagnostic = nullptr) {
  return parse(bytes_of(text), "<test>", diagnostic);
}

/// A parenthesised rendering of an expression, so that a precedence test is one
/// readable line instead of a walk over five node indices.
std::string sexp(const Script& script, NodeIndex index) {
  if (index == kNoNode) return "<none>";
  const Expr& e = script.expr(index);
  switch (e.kind) {
    case ExprKind::int_literal:
      return std::to_string(e.int_value);
    case ExprKind::string_literal:
      return "\"" + std::string(e.text) + "\"";
    case ExprKind::name:
      return std::string(e.text);
    case ExprKind::this_:
      return "this";
    case ExprKind::unary:
      return "(" + std::string(e.unary_op == UnaryOp::negate ? "-" : "!") + " " +
             sexp(script, e.lhs) + ")";
    case ExprKind::binary: {
      static const char* const names[] = {"+",  "-",  "*",  "/",  "%",  "==", "!=",
                                          "<",  "<=", ">",  ">=", "&&", "||"};
      return "(" + std::string(names[static_cast<int>(e.binary_op)]) + " " +
             sexp(script, e.lhs) + " " + sexp(script, e.rhs) + ")";
    }
    case ExprKind::index:
      return "([] " + sexp(script, e.lhs) + " " + sexp(script, e.rhs) + ")";
    case ExprKind::call: {
      std::string out = "(call " + std::string(e.text);
      for (std::uint32_t i = 0; i < e.argument_count; ++i) {
        out += " " + sexp(script, script.arguments[e.argument_begin + i]);
      }
      return out + ")";
    }
    case ExprKind::method: {
      std::string out = std::string(e.implicit_parens ? "(. " : "(.() ") +
                        sexp(script, e.lhs) + " " + std::string(e.text);
      for (std::uint32_t i = 0; i < e.argument_count; ++i) {
        out += " " + sexp(script, script.arguments[e.argument_begin + i]);
      }
      return out + ")";
    }
  }
  return "?";
}

/// The s-expression of the sole top-level expression statement in `text`.
std::string expression_of(std::string_view text) {
  const std::string source = "x = " + std::string(text) + ";";
  const auto script = parse_text(source);
  if (!script || script->body.size() != 1) return "<parse failed>";
  const Stmt& s = script->stmt(script->body[0]);
  if (s.kind != StmtKind::assign) return "<not an assignment>";
  return sexp(*script, s.value);
}

}  // namespace

TEST(vs_binary_operators) {
  CHECK(expression_of("a + b") == "(+ a b)");
  CHECK(expression_of("a - b") == "(- a b)");
  CHECK(expression_of("a * b") == "(* a b)");
  CHECK(expression_of("a / b") == "(/ a b)");
  CHECK(expression_of("a % b") == "(% a b)");
  CHECK(expression_of("a == b") == "(== a b)");
  CHECK(expression_of("a != b") == "(!= a b)");
  CHECK(expression_of("a < b") == "(< a b)");
  CHECK(expression_of("a <= b") == "(<= a b)");
  CHECK(expression_of("a > b") == "(> a b)");
  CHECK(expression_of("a >= b") == "(>= a b)");
  CHECK(expression_of("a && b") == "(&& a b)");
  CHECK(expression_of("a || b") == "(|| a b)");
}

TEST(vs_unary_operators) {
  CHECK(expression_of("!a") == "(! a)");
  CHECK(expression_of("-a") == "(- a)");
  // Unary '+' has no node in the contract, and nothing is lost by dropping it.
  CHECK(expression_of("+a") == "a");
  CHECK(expression_of("!!a") == "(! (! a))");
  CHECK(expression_of("- -a") == "(- (- a))");
}

TEST(vs_precedence_is_c) {
  // The corpus parenthesises every ambiguous nesting, so none of this is
  // proven by the shipped data -- see docs/formats/vs-language.md. It is
  // pinned here so that a change to the table is a deliberate act.
  CHECK(expression_of("a + b * c") == "(+ a (* b c))");
  CHECK(expression_of("a * b + c") == "(+ (* a b) c)");
  CHECK(expression_of("a - b - c") == "(- (- a b) c)");
  CHECK(expression_of("a / b * c") == "(* (/ a b) c)");
  CHECK(expression_of("a + b < c") == "(< (+ a b) c)");
  CHECK(expression_of("a < b == c") == "(== (< a b) c)");
  CHECK(expression_of("a == b && c") == "(&& (== a b) c)");
  CHECK(expression_of("a && b || c") == "(|| (&& a b) c)");
  CHECK(expression_of("a || b && c") == "(|| a (&& b c))");
  CHECK(expression_of("!a && b") == "(&& (! a) b)");
  CHECK(expression_of("!a.IsValid") == "(! (. a IsValid))");
  CHECK(expression_of("-a * b") == "(* (- a) b)");
  CHECK(expression_of("(a + b) * c") == "(* (+ a b) c)");
  CHECK(expression_of("a[i] + 1") == "(+ ([] a i) 1)");
  CHECK(expression_of("a.b.c") == "(. (. a b) c)");
  CHECK(expression_of("f(1)[2].g") == "(. ([] (call f 1) 2) g)");
}

TEST(vs_assignment_binds_looser_than_everything) {
  // Proven by the corpus: DATA\AI\ESH_BUILDARMY.VS:46 writes
  // `bNeedTraining = EnvReadInt(AIPlayer, "NeedTraining")==1;`, which is only
  // meaningful if '==' binds tighter than '='.
  const auto script = parse_text("a = b == 1;");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& s = script->stmt(script->body[0]);
  REQUIRE(s.kind == StmtKind::assign);
  CHECK(sexp(*script, s.expr) == "a");
  CHECK(sexp(*script, s.value) == "(== b 1)");
}

TEST(vs_optional_parentheses) {
  // `u.IsValid` and `u.IsValid()` occur in the same files. Both are
  // zero-argument calls; only the spelling differs, and only for round
  // tripping -- dispatch is by name and arity, so there is no property
  // namespace to tell them apart.
  const auto script = parse_text("a = u.IsValid; b = u.IsValid();");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 2);

  const Expr& bare = script->expr(script->stmt(script->body[0]).value);
  const Expr& called = script->expr(script->stmt(script->body[1]).value);
  CHECK(bare.kind == ExprKind::method);
  CHECK(called.kind == ExprKind::method);
  CHECK(bare.text == "IsValid");
  CHECK(called.text == "IsValid");
  CHECK(bare.argument_count == 0);
  CHECK(called.argument_count == 0);
  CHECK(bare.implicit_parens);
  CHECK(!called.implicit_parens);
}

TEST(vs_bare_free_function_is_a_name) {
  // `GetTime` and `GetTime()` both occur. A bare identifier stays a `name`:
  // the runtime resolves it against locals first and the host table second,
  // which is the same rule the language needs for global constants.
  CHECK(expression_of("GetTime") == "GetTime");
  CHECK(expression_of("GetTime()") == "(call GetTime)");
  CHECK(expression_of("AIRun(\"x.vs\", 1)") == "(call AIRun \"x.vs\" 1)");
}

TEST(vs_leading_dot_is_this) {
  // A leading '.' is exactly 'this.', where `this` is an ordinary local. 350
  // files use it and all 350 have a lowercase `this` in scope.
  CHECK(expression_of(".health") == "(. this health)");
  CHECK(expression_of(".AsUnit.level") == "(. (. this AsUnit) level)");
  CHECK(expression_of(".Progress(50)") == "(.() this Progress 50)");
  CHECK(expression_of("this.health") == "(. this health)");
  CHECK(expression_of(".health < .maxhealth * 8 / 10 + 10") ==
        "(< (. this health) (+ (/ (* (. this maxhealth) 8) 10) 10))");
}

TEST(vs_identifiers_are_case_sensitive) {
  // DATA\SUBAI\OUTPOST_IDLE.VS holds `Obj This` and `Settlement this` at once,
  // both live, so folding case would merge two different objects.
  const auto script = parse_text("this = This.AsBuilding().settlement;");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& s = script->stmt(script->body[0]);
  CHECK(sexp(*script, s.expr) == "this");
  CHECK(sexp(*script, s.value) == "(. (.() This AsBuilding) settlement)");
}

TEST(vs_literals) {
  CHECK(expression_of("0") == "0");
  CHECK(expression_of("2147483647") == "2147483647");
  CHECK(expression_of("\"hello\"") == "\"hello\"");
  // Both quote styles are interchangeable, and the text excludes the quotes.
  CHECK(expression_of("'hello'") == "\"hello\"");
  CHECK(expression_of("\"a\\nb\"") == "\"a\\nb\"");
  // true/false are integers: `bool` is numerically usable and the corpus
  // relies on it (`tgt.experience + (chance > rand(99))`).
  CHECK(expression_of("true") == "1");
  CHECK(expression_of("false") == "0");
}

TEST(vs_comments_are_discarded) {
  const auto script = parse_text(
      "// void\n"
      "a = 1; // trailing\n"
      "/* a block\n   comment */\n"
      "b = 2;\n");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 2);
  CHECK(script->stmt(script->body[0]).line == 2);
  CHECK(script->stmt(script->body[1]).line == 5);
}

TEST(vs_signature_comment) {
  const auto script = parse_text("//int, int idPlayer, GAIKA g, int *pOverneed\nreturn 0;");
  REQUIRE(script.ok());
  CHECK(script->has_signature);
  CHECK(script->return_type == TypeTag::int_);
  CHECK(script->return_type_name == "int");
  REQUIRE(script->parameters.size() == 3);
  CHECK(script->parameters[0].type_name == "int");
  CHECK(script->parameters[0].name == "idPlayer");
  CHECK(!script->parameters[0].is_out);
  CHECK(script->parameters[1].type_name == "GAIKA");
  CHECK(script->parameters[1].type == TypeTag::object);
  CHECK(script->parameters[1].name == "g");
  CHECK(script->parameters[2].type_name == "int");
  CHECK(script->parameters[2].name == "pOverneed");
  CHECK(script->parameters[2].is_out);
}

TEST(vs_signature_out_markers) {
  // Three spellings occur: '*' on the name, '*' on the type, and the word OUT.
  const auto star_on_name = parse_text("//void, int *p\n");
  REQUIRE(star_on_name.ok());
  REQUIRE(star_on_name->parameters.size() == 1);
  CHECK(star_on_name->parameters[0].is_out);
  CHECK(star_on_name->parameters[0].type_name == "int");
  CHECK(star_on_name->parameters[0].name == "p");

  const auto star_on_type = parse_text("//void, Hero hero, IntArray* aSkills\n");
  REQUIRE(star_on_type.ok());
  REQUIRE(star_on_type->parameters.size() == 2);
  CHECK(!star_on_type->parameters[0].is_out);
  CHECK(star_on_type->parameters[1].is_out);
  CHECK(star_on_type->parameters[1].type_name == "IntArray");
  CHECK(star_on_type->parameters[1].name == "aSkills");

  const auto out_word = parse_text("//bool, ObjList objs, str OUT reasonText\n");
  REQUIRE(out_word.ok());
  CHECK(out_word->return_type == TypeTag::int_);  // bool is numerically an int
  CHECK(out_word->return_type_name == "bool");
  REQUIRE(out_word->parameters.size() == 2);
  CHECK(!out_word->parameters[0].is_out);
  CHECK(out_word->parameters[0].type == TypeTag::object);
  CHECK(out_word->parameters[1].is_out);
  CHECK(out_word->parameters[1].type == TypeTag::string_);
  CHECK(out_word->parameters[1].name == "reasonText");
}

TEST(vs_signature_absent_or_malformed) {
  // The one script in the corpus without a signature is a dead item script.
  // A body with no leading comment simply has no declared types.
  const auto none = parse_text("int a;\n");
  REQUIRE(none.ok());
  CHECK(!none->has_signature);
  CHECK(none->parameters.empty());

  // A comment whose fields do not parse is treated as no signature at all,
  // never as a half-applied one.
  const auto broken = parse_text("//void, this is not a parameter\nint a;\n");
  REQUIRE(broken.ok());
  CHECK(!broken->has_signature);
  CHECK(broken->parameters.empty());
  CHECK(broken->return_type_name.empty());

  // Leading blank lines are skipped; the first non-blank line is the one.
  const auto after_blanks = parse_text("\n\n//void, Obj This\n");
  REQUIRE(after_blanks.ok());
  CHECK(after_blanks->has_signature);
  CHECK(after_blanks->return_type == TypeTag::void_);
  REQUIRE(after_blanks->parameters.size() == 1);
  CHECK(after_blanks->parameters[0].name == "This");
}

TEST(vs_expression_only_script) {
  // The second entry mode, used by the debug watches in DATA\SCDEBUG.XML.
  const auto script = parse_text(".AsUnit.level");
  REQUIRE(script.ok());
  CHECK(script->is_expression_only);
  REQUIRE(script->body.size() == 1);
  const Stmt& s = script->stmt(script->body[0]);
  REQUIRE(s.kind == StmtKind::expr);
  CHECK(sexp(*script, s.expr) == "(. (. this AsUnit) level)");

  // An ordinary body is never mistaken for one.
  const auto body = parse_text("return .AsUnit.level;");
  REQUIRE(body.ok());
  CHECK(!body->is_expression_only);
}

TEST(vs_declarations) {
  const auto script = parse_text("int own, ally;\nSettlement setGIn;\n");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 2);

  const Stmt& ints = script->stmt(script->body[0]);
  REQUIRE(ints.kind == StmtKind::declare);
  CHECK(ints.type_name == "int");
  CHECK(ints.declared_type == TypeTag::int_);
  REQUIRE(ints.name_count == 2);
  CHECK(script->names[ints.name_begin] == "own");
  CHECK(script->names[ints.name_begin + 1] == "ally");

  const Stmt& handle = script->stmt(script->body[1]);
  REQUIRE(handle.kind == StmtKind::declare);
  CHECK(handle.type_name == "Settlement");
  CHECK(handle.declared_type == TypeTag::object);
  REQUIRE(handle.name_count == 1);
  CHECK(script->names[handle.name_begin] == "setGIn");
}

TEST(vs_declaration_initialisers_become_assignments) {
  // The contract has no slot for an initialiser, so a declaration with one
  // becomes a `declare` naming every name and an `assign` per initialiser.
  const auto script = parse_text("int a = 1, b, c = 3;");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 3);

  const Stmt& declare = script->stmt(script->body[0]);
  REQUIRE(declare.kind == StmtKind::declare);
  REQUIRE(declare.name_count == 3);
  CHECK(script->names[declare.name_begin + 1] == "b");

  const Stmt& first = script->stmt(script->body[1]);
  REQUIRE(first.kind == StmtKind::assign);
  CHECK(sexp(*script, first.expr) == "a");
  CHECK(sexp(*script, first.value) == "1");

  const Stmt& second = script->stmt(script->body[2]);
  REQUIRE(second.kind == StmtKind::assign);
  CHECK(sexp(*script, second.expr) == "c");
  CHECK(sexp(*script, second.value) == "3");
}

TEST(vs_compound_assignment_is_expanded) {
  const auto script = parse_text("i += 1;\nn -= f(2);\n");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 2);

  const Stmt& add = script->stmt(script->body[0]);
  REQUIRE(add.kind == StmtKind::assign);
  CHECK(sexp(*script, add.expr) == "i");
  CHECK(sexp(*script, add.value) == "(+ i 1)");

  const Stmt& sub = script->stmt(script->body[1]);
  REQUIRE(sub.kind == StmtKind::assign);
  CHECK(sexp(*script, sub.value) == "(- n (call f 2))");
}

TEST(vs_compound_assignment_copies_a_subscript_target) {
  // The duplicated target must be a copy, not a shared index: consumers are
  // entitled to assume the tree is a tree.
  const auto script = parse_text("a[i] += 1;");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& s = script->stmt(script->body[0]);
  REQUIRE(s.kind == StmtKind::assign);
  CHECK(sexp(*script, s.expr) == "([] a i)");
  CHECK(sexp(*script, s.value) == "(+ ([] a i) 1)");
  const Expr& value = script->expr(s.value);
  CHECK(value.lhs != s.expr);
}

TEST(vs_if_else) {
  const auto script = parse_text("if (a) b(); else c();");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& s = script->stmt(script->body[0]);
  REQUIRE(s.kind == StmtKind::if_);
  CHECK(sexp(*script, s.expr) == "a");
  REQUIRE(s.then_branch != kNoNode);
  REQUIRE(s.else_branch != kNoNode);
  CHECK(sexp(*script, script->stmt(s.then_branch).expr) == "(call b)");
  CHECK(sexp(*script, script->stmt(s.else_branch).expr) == "(call c)");
}

TEST(vs_dangling_else_binds_to_the_nearest_if) {
  const auto script = parse_text("if (a) if (b) x(); else y();");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& outer = script->stmt(script->body[0]);
  REQUIRE(outer.kind == StmtKind::if_);
  CHECK(outer.else_branch == kNoNode);
  REQUIRE(outer.then_branch != kNoNode);
  const Stmt& inner = script->stmt(outer.then_branch);
  REQUIRE(inner.kind == StmtKind::if_);
  CHECK(inner.else_branch != kNoNode);
}

TEST(vs_blocks_are_contiguous_runs) {
  // A block's contents are `then_branch` .. `then_branch + else_branch` in
  // Script::statements, which is why statements are appended only once their
  // whole subtree is known.
  const auto script = parse_text("{ a(); b(); { c(); } }");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& outer = script->stmt(script->body[0]);
  REQUIRE(outer.kind == StmtKind::block);
  REQUIRE(outer.else_branch == 3);
  CHECK(sexp(*script, script->stmt(outer.then_branch).expr) == "(call a)");
  CHECK(sexp(*script, script->stmt(outer.then_branch + 1).expr) == "(call b)");
  const Stmt& inner = script->stmt(outer.then_branch + 2);
  REQUIRE(inner.kind == StmtKind::block);
  REQUIRE(inner.else_branch == 1);
  CHECK(sexp(*script, script->stmt(inner.then_branch).expr) == "(call c)");
}

TEST(vs_bracket_block) {
  // Exactly one block in the corpus is delimited with '[' ']', in the live
  // script DATA\AI\SQUADMONITOR.VS. Accepting it costs nothing: every other
  // '[' is a subscript, and a subscript never begins a statement.
  const auto script = parse_text("while (1) [ a(); b(); ]");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& loop = script->stmt(script->body[0]);
  REQUIRE(loop.kind == StmtKind::while_);
  REQUIRE(loop.then_branch != kNoNode);
  const Stmt& body = script->stmt(loop.then_branch);
  REQUIRE(body.kind == StmtKind::block);
  CHECK(body.else_branch == 2);
}

TEST(vs_empty_statement) {
  // A stray ';' produces nothing at all...
  const auto dropped = parse_text("a(); ; b();");
  REQUIRE(dropped.ok());
  CHECK(dropped->body.size() == 2);

  // ...except where exactly one statement is required, which needs an empty
  // block to point at.
  const auto wrapped = parse_text("if (a) ;");
  REQUIRE(wrapped.ok());
  REQUIRE(wrapped->body.size() == 1);
  const Stmt& s = wrapped->stmt(wrapped->body[0]);
  REQUIRE(s.kind == StmtKind::if_);
  REQUIRE(s.then_branch != kNoNode);
  const Stmt& branch = wrapped->stmt(s.then_branch);
  CHECK(branch.kind == StmtKind::block);
  CHECK(branch.else_branch == 0);
}

TEST(vs_while_and_loop_control) {
  const auto script = parse_text("while (a.IsValid) { break; continue; }");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& loop = script->stmt(script->body[0]);
  REQUIRE(loop.kind == StmtKind::while_);
  CHECK(sexp(*script, loop.expr) == "(. a IsValid)");
  // A plain `while` has no step, which is what tells it apart from a `for`.
  CHECK(loop.else_branch == kNoNode);
  REQUIRE(loop.then_branch != kNoNode);
  const Stmt& body = script->stmt(loop.then_branch);
  REQUIRE(body.kind == StmtKind::block);
  REQUIRE(body.else_branch == 2);
  CHECK(script->stmt(body.then_branch).kind == StmtKind::break_);
  CHECK(script->stmt(body.then_branch + 1).kind == StmtKind::continue_);
}

TEST(vs_for_becomes_a_while_with_a_step) {
  // `for` has no StmtKind. It becomes `block { init; while (cond) body }` and
  // the step hangs off the loop's else_branch, *not* off the end of the body:
  // 92 of the 351 for-loops in the corpus `continue` out of the body, and a
  // continue that skipped the step would never terminate.
  const auto script = parse_text("for (i = 0; i < n; i += 1) { continue; }");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);

  const Stmt& wrapper = script->stmt(script->body[0]);
  REQUIRE(wrapper.kind == StmtKind::block);
  REQUIRE(wrapper.else_branch == 2);

  const Stmt& init = script->stmt(wrapper.then_branch);
  REQUIRE(init.kind == StmtKind::assign);
  CHECK(sexp(*script, init.expr) == "i");
  CHECK(sexp(*script, init.value) == "0");

  const Stmt& loop = script->stmt(wrapper.then_branch + 1);
  REQUIRE(loop.kind == StmtKind::while_);
  CHECK(sexp(*script, loop.expr) == "(< i n)");
  REQUIRE(loop.else_branch != kNoNode);
  const Stmt& step = script->stmt(loop.else_branch);
  REQUIRE(step.kind == StmtKind::assign);
  CHECK(sexp(*script, step.expr) == "i");
  CHECK(sexp(*script, step.value) == "(+ i 1)");

  REQUIRE(loop.then_branch != kNoNode);
  const Stmt& body = script->stmt(loop.then_branch);
  REQUIRE(body.kind == StmtKind::block);
  REQUIRE(body.else_branch == 1);
  CHECK(script->stmt(body.then_branch).kind == StmtKind::continue_);
}

TEST(vs_for_with_empty_slots) {
  // `for (0; !SL.EOL; SL.Next)` -- DATA\AI\GS_CAPTURE.VS:153. The initialiser
  // slot takes any expression, and every slot may be empty.
  const auto plain = parse_text("for (0; !SL.EOL; SL.Next) x();");
  REQUIRE(plain.ok());
  REQUIRE(plain->body.size() == 1);
  const Stmt& wrapper = plain->stmt(plain->body[0]);
  REQUIRE(wrapper.kind == StmtKind::block);
  REQUIRE(wrapper.else_branch == 2);
  CHECK(plain->stmt(wrapper.then_branch).kind == StmtKind::expr);
  const Stmt& loop = plain->stmt(wrapper.then_branch + 1);
  CHECK(sexp(*plain, loop.expr) == "(! (. SL EOL))");
  REQUIRE(loop.else_branch != kNoNode);
  CHECK(sexp(*plain, plain->stmt(loop.else_branch).expr) == "(. SL Next)");

  const auto bare = parse_text("for (;;) x();");
  REQUIRE(bare.ok());
  REQUIRE(bare->body.size() == 1);
  const Stmt& bare_wrapper = bare->stmt(bare->body[0]);
  REQUIRE(bare_wrapper.kind == StmtKind::block);
  REQUIRE(bare_wrapper.else_branch == 1);
  const Stmt& bare_loop = bare->stmt(bare_wrapper.then_branch);
  REQUIRE(bare_loop.kind == StmtKind::while_);
  // A missing condition means "always", not "never".
  CHECK(sexp(*bare, bare_loop.expr) == "1");
  CHECK(bare_loop.else_branch == kNoNode);
}

TEST(vs_return) {
  const auto with_value = parse_text("return own - nMinNeed;");
  REQUIRE(with_value.ok());
  REQUIRE(with_value->body.size() == 1);
  const Stmt& value = with_value->stmt(with_value->body[0]);
  REQUIRE(value.kind == StmtKind::return_);
  CHECK(sexp(*with_value, value.expr) == "(- own nMinNeed)");

  const auto bare = parse_text("return;");
  REQUIRE(bare.ok());
  REQUIRE(bare->body.size() == 1);
  const Stmt& none = bare->stmt(bare->body[0]);
  REQUIRE(none.kind == StmtKind::return_);
  CHECK(none.expr == kNoNode);
}

TEST(vs_bare_call_statement) {
  // `SL.Rewind;` and `This.Erase;` are statements in the corpus.
  const auto script = parse_text("SL.Rewind;\nThis.Erase;\n");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 2);
  CHECK(script->stmt(script->body[0]).kind == StmtKind::expr);
  CHECK(sexp(*script, script->stmt(script->body[0]).expr) == "(. SL Rewind)");
  CHECK(sexp(*script, script->stmt(script->body[1]).expr) == "(. This Erase)");
}

TEST(vs_subscript_assignment) {
  // Assignment targets are only ever a name or a subscript; no member is an
  // assignment target anywhere in the corpus.
  const auto script = parse_text("a[i + 1] = 2;");
  REQUIRE(script.ok());
  REQUIRE(script->body.size() == 1);
  const Stmt& s = script->stmt(script->body[0]);
  REQUIRE(s.kind == StmtKind::assign);
  CHECK(sexp(*script, s.expr) == "([] a (+ i 1))");
}

// -- diagnostics -----------------------------------------------------------

namespace {

/// Parse `text` expecting failure, and report where the parser said it was.
struct Failure {
  bool failed = false;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::string message;
};

Failure failure_of(std::string_view text) {
  Diagnostic diagnostic;
  const auto script = parse_text(text, &diagnostic);
  Failure out;
  out.failed = !script.ok();
  out.line = diagnostic.line;
  out.column = diagnostic.column;
  out.message = std::string(diagnostic.message);
  return out;
}

}  // namespace

TEST(vs_missing_semicolon_is_reported_where_it_is_missing) {
  const Failure f = failure_of("int a;\na = 1\nb = 2;\n");
  REQUIRE(f.failed);
  // The parser notices at the token that should have been the ';'.
  CHECK(f.line == 3);
  CHECK(f.column == 1);
  CHECK(f.message == "expected ';'");
}

TEST(vs_unclosed_paren_is_reported) {
  const Failure f = failure_of("if (a {\n}\n");
  REQUIRE(f.failed);
  CHECK(f.line == 1);
  CHECK(f.column == 7);
  CHECK(f.message == "expected ')' after an if condition");
}

TEST(vs_unterminated_block_names_the_opening_brace) {
  const Failure f = failure_of("while (1) {\n  a();\n");
  REQUIRE(f.failed);
  CHECK(f.line == 1);
  CHECK(f.column == 11);
  CHECK(f.message == "unterminated block");
}

TEST(vs_unterminated_block_comment_is_reported) {
  const Failure f = failure_of("a();\n  /* never closed\n");
  REQUIRE(f.failed);
  CHECK(f.line == 2);
  CHECK(f.column == 3);
  CHECK(f.message == "unterminated block comment");
}

TEST(vs_unterminated_string_is_reported) {
  const Failure f = failure_of("a = \"unclosed;\n");
  REQUIRE(f.failed);
  CHECK(f.line == 1);
  CHECK(f.column == 5);
  CHECK(f.message == "unterminated string literal");
}

TEST(vs_unexpected_character_is_reported) {
  // Bitwise operators do not exist in the language; '&' alone is not a token.
  const Failure f = failure_of("a = b & c;\n");
  REQUIRE(f.failed);
  CHECK(f.line == 1);
  CHECK(f.column == 7);
  CHECK(f.message == "unexpected character");
}

TEST(vs_keyword_in_an_expression_is_reported) {
  const Failure f = failure_of("a = while;\n");
  REQUIRE(f.failed);
  CHECK(f.line == 1);
  CHECK(f.column == 5);
  CHECK(f.message == "keyword in an expression");
}

TEST(vs_member_name_must_follow_a_dot) {
  const Failure f = failure_of("a = b.5;\n");
  REQUIRE(f.failed);
  CHECK(f.line == 1);
  CHECK(f.column == 7);
  CHECK(f.message == "expected a member name after '.'");
}

TEST(vs_truncated_input_does_not_run_off_the_end) {
  // Each of these ends mid-construct. None may crash, and each must produce a
  // diagnostic pointing at the synthetic end-of-input token.
  //
  // `int` alone is deliberately absent: it is a valid expression-only script,
  // because a bare identifier is a legal expression. That is a real
  // consequence of the second entry mode, not an oversight.
  const char* const truncated[] = {"if (",        "if (a", "while (a) {", "a = ",
                                   "a.",          "f(1,",  "int a",       "return",
                                   "for (i = 0;", "(",     "a["};
  for (const char* text : truncated) {
    const Failure f = failure_of(text);
    CHECK(f.failed);
    CHECK(f.line >= 1);
    CHECK(!f.message.empty());
  }
}

TEST(vs_empty_source_parses_to_an_empty_script) {
  const auto script = parse_text("");
  REQUIRE(script.ok());
  CHECK(script->body.empty());
  CHECK(!script->has_signature);
  CHECK(!script->is_expression_only);
}

TEST(vs_source_name_is_carried) {
  const auto script = parse_text("a();");
  REQUIRE(script.ok());
  CHECK(script->source_name == "<test>");

  Diagnostic diagnostic;
  const auto broken = parse_text("a(", &diagnostic);
  CHECK(!broken.ok());
  CHECK(diagnostic.source_name == "<test>");
}

TEST(vs_deep_nesting_is_a_diagnostic_not_a_stack_overflow) {
  // Scripts are untrusted input like every other game file, and recursive
  // descent recurses. Without a cap this input takes the process down, which
  // the core is not allowed to do (see formats/result.hpp).
  std::string source = "a = ";
  for (int i = 0; i < 5000; ++i) source += "(";
  source += "1";
  for (int i = 0; i < 5000; ++i) source += ")";
  source += ";";

  Diagnostic diagnostic;
  const auto script = parse_text(source, &diagnostic);
  CHECK(!script.ok());
  CHECK(diagnostic.line == 1);
  CHECK(diagnostic.message == "expression nested too deeply");

  std::string blocks;
  for (int i = 0; i < 5000; ++i) blocks += "{";
  for (int i = 0; i < 5000; ++i) blocks += "}";
  Diagnostic block_diagnostic;
  const auto nested = parse_text(blocks, &block_diagnostic);
  CHECK(!nested.ok());
  CHECK(block_diagnostic.message == "statements nested too deeply");

  // The limit is far above anything real: the deepest tree in the corpus is
  // 29 levels.
  std::string shallow = "a = ";
  for (int i = 0; i < 64; ++i) shallow += "(";
  shallow += "1";
  for (int i = 0; i < 64; ++i) shallow += ")";
  shallow += ";";
  CHECK(parse_text(shallow).ok());
}
