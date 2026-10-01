#include "imperivm/core/script/compiler.hpp"

#include <string>
#include <utility>
#include <vector>

namespace imperivm::core::script {
namespace {

/// The statements a `block` contains.
///
/// `ast.hpp` says block contents are "a range into `Script::statements`", and
/// the only pair of fields a block does not otherwise use is
/// `then_branch`/`else_branch` -- which for `if_` and `while_` hold single
/// statement indices. So a block reads them as begin and count. This is the one
/// place the reading is encoded; if the front end settles on another encoding,
/// this function is the whole of the change.
struct StatementRange {
  std::uint32_t begin = 0;
  std::uint32_t count = 0;
};

StatementRange block_children(const Stmt& statement) {
  if (statement.then_branch == kNoNode) return {};
  StatementRange range;
  range.begin = statement.then_branch;
  range.count = statement.else_branch == kNoNode ? 0 : statement.else_branch;
  return range;
}

const char* default_type_spelling(TypeTag tag) {
  switch (tag) {
    case TypeTag::int_: return "int";
    case TypeTag::string_: return "str";
    case TypeTag::object: return "Obj";
    case TypeTag::void_: return "void";
  }
  return "int";
}

class Compiler {
 public:
  Compiler(const Script& script, const HostRegistry* host, CompileError* error)
      : script_(script), host_(host), error_(error) {}

  Result<Chunk> compile() {
    chunk_.source_name = std::string(script_.source_name);
    chunk_.return_type = script_.return_type;
    chunk_.is_expression_only = script_.is_expression_only;

    push_scope();
    for (const Parameter& parameter : script_.parameters) {
      const std::uint32_t slot = declare_local(
          parameter.name,
          parameter.type_name.empty() ? default_type_spelling(parameter.type)
                                      : std::string(parameter.type_name),
          true, parameter.is_out);
      (void)slot;
    }
    chunk_.parameter_count = static_cast<std::uint32_t>(script_.parameters.size());

    // The 113 inline `script="..."` snippets in the class XML include one --
    // `.AsUnit.level` in DATA\SCDEBUG.XML -- that is a bare expression with no
    // `return` and no `;`. It is a second entry mode, not a malformed body, and
    // its value is what the host reads, so the trailing expression yields.
    const std::size_t body_size = script_.body.size();
    for (std::size_t i = 0; i < body_size && !failed_; ++i) {
      const NodeIndex index = script_.body[i];
      const bool last = i + 1 == body_size;
      if (chunk_.is_expression_only && last && statement_is_expression(index)) {
        const Stmt& tail = statement_at(index);
        if (failed_) break;
        expression(tail.expr);
        emit(Op::return_value, 0, 0, tail.line);
        continue;
      }
      statement(index);
    }
    pop_scope();

    if (failed_) return FormatError::malformed;

    emit(Op::return_void, 0, 0, last_line_);
    return std::move(chunk_);
  }

 private:
  // -- diagnostics -------------------------------------------------------

  void fail(std::uint32_t line, std::string message) {
    if (failed_) return;
    failed_ = true;
    if (error_ != nullptr) {
      error_->source_name = std::string(script_.source_name);
      error_->line = line;
      error_->message = std::move(message);
    }
  }

  [[nodiscard]] bool valid_expr(NodeIndex index) const {
    return index != kNoNode && index < script_.expressions.size();
  }
  [[nodiscard]] bool valid_stmt(NodeIndex index) const {
    return index != kNoNode && index < script_.statements.size();
  }

  const Stmt& statement_at(NodeIndex index) {
    if (!valid_stmt(index)) {
      fail(last_line_, "statement index out of range");
      static const Stmt kEmpty{};
      return kEmpty;
    }
    return script_.statements[index];
  }

  bool statement_is_expression(NodeIndex index) {
    return valid_stmt(index) && script_.statements[index].kind == StmtKind::expr;
  }

  // -- emission ----------------------------------------------------------

  std::uint32_t emit(Op op, std::uint32_t a, std::uint32_t b, std::uint32_t line) {
    if (line != 0) last_line_ = line;
    chunk_.code.push_back(Instruction{op, a, b, line != 0 ? line : last_line_});
    return static_cast<std::uint32_t>(chunk_.code.size() - 1);
  }

  std::uint32_t here() const { return static_cast<std::uint32_t>(chunk_.code.size()); }

  void patch(std::uint32_t at, std::uint32_t target) { chunk_.code[at].a = target; }

  std::uint32_t constant(Value value) {
    for (std::size_t i = 0; i < chunk_.constants.size(); ++i) {
      if (chunk_.constants[i] == value) return static_cast<std::uint32_t>(i);
    }
    chunk_.constants.push_back(std::move(value));
    return static_cast<std::uint32_t>(chunk_.constants.size() - 1);
  }

  std::uint32_t string_constant(std::string_view text) {
    return constant(Value::string(std::string(text)));
  }

  // -- scopes ------------------------------------------------------------
  //
  // Slots are never reused when a scope closes. VS scripts hold a few dozen
  // locals at most, and a stable slot for every declaration keeps a serialised
  // frame legible: slot 7 is always `ptCenter`, whatever the instruction
  // pointer happens to be.

  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() { scopes_.pop_back(); }

  std::uint32_t declare_local(std::string_view name, std::string type_name, bool is_parameter,
                              bool is_out) {
    const std::uint32_t slot = static_cast<std::uint32_t>(chunk_.locals.size());
    chunk_.locals.push_back(LocalInfo{std::string(name), std::move(type_name), is_parameter,
                                      is_out});
    scopes_.back().push_back(Binding{std::string(name), slot});
    return slot;
  }

  /// Case-sensitive, deliberately. `OUTPOST_IDLE.VS` keeps `This` (an outpost
  /// building) and `this` (a settlement) live at the same time, and 204 files
  /// use that idiom.
  [[nodiscard]] bool lookup(std::string_view name, std::uint32_t& slot) const {
    for (std::size_t depth = scopes_.size(); depth-- > 0;) {
      const std::vector<Binding>& scope = scopes_[depth];
      for (std::size_t i = scope.size(); i-- > 0;) {
        if (scope[i].name == name) {
          slot = scope[i].slot;
          return true;
        }
      }
    }
    return false;
  }

  // -- expressions -------------------------------------------------------

  void expression(NodeIndex index) {
    if (failed_) return;
    if (!valid_expr(index)) {
      fail(last_line_, "expression index out of range");
      return;
    }
    const Expr& node = script_.expressions[index];
    switch (node.kind) {
      case ExprKind::int_literal:
        emit(Op::push_const, constant(Value::integer(node.int_value)), 0, node.line);
        return;
      case ExprKind::string_literal:
        emit(Op::push_const, string_constant(node.text), 0, node.line);
        return;
      case ExprKind::name: {
        std::uint32_t slot = 0;
        if (lookup(node.text, slot)) {
          emit(Op::load_local, slot, 0, node.line);
          return;
        }
        // Parentheses are optional on a zero-argument call, for free functions
        // as well as members: `rcMap = GetMapRect;` in DEER_IDLE.VS is a call,
        // and `GetTime`, `AIGetPlayer`, `GAIKACount`, `MapSize`, `MaxSetIdx`
        // and `Breakpoint` are all written both ways across the corpus. So a
        // bare name resolves local first, then zero-argument host function,
        // then global constant.
        if (host_ != nullptr) {
          const std::uint32_t index = host_->find(CallKind::free_function, node.text, 0);
          if (index != kUnresolvedHost) {
            CallSite site;
            site.kind = CallKind::free_function;
            site.arity = 0;
            site.name_const = string_constant(node.text);
            site.host_index = index;
            site.writeback_begin = static_cast<std::uint32_t>(chunk_.writebacks.size());
            chunk_.call_sites.push_back(site);
            emit(Op::call, static_cast<std::uint32_t>(chunk_.call_sites.size() - 1), 0,
                 node.line);
            return;
          }
        }
        // Otherwise it is one of the 245 global constants and ambient globals --
        // `AI_COMING`, `Carthage`, `cmdparam`. The host answers for it, and
        // traps identifiably if it cannot.
        emit(Op::load_global, string_constant(node.text), 0, node.line);
        return;
      }
      case ExprKind::this_: {
        std::uint32_t slot = 0;
        if (lookup("this", slot)) {
          emit(Op::load_local, slot, 0, node.line);
        } else {
          // Inline XML snippets use a leading `.` with no declaration in sight,
          // so in that entry mode the host pre-binds `this`.
          emit(Op::load_global, string_constant("this"), 0, node.line);
        }
        return;
      }
      case ExprKind::unary:
        expression(node.lhs);
        emit(node.unary_op == UnaryOp::negate ? Op::negate : Op::logical_not, 0, 0, node.line);
        return;
      case ExprKind::binary:
        binary(node);
        return;
      case ExprKind::index:
        expression(node.lhs);
        expression(node.rhs);
        emit(Op::index_get, 0, 0, node.line);
        return;
      case ExprKind::call:
        call(node, CallKind::free_function);
        return;
      case ExprKind::method:
        call(node, CallKind::member);
        return;
    }
    fail(node.line, "unknown expression kind");
  }

  void binary(const Expr& node) {
    if (node.binary_op == BinaryOp::logical_and || node.binary_op == BinaryOp::logical_or) {
      // Short-circuit. The corpus never *needs* it -- reading a member of an
      // invalid handle appears to be tolerated -- but `while (This.IsValid() &&
      // This.stamina >= 0)` is written as though it holds, and eager evaluation
      // of a host call is exactly the kind of extra side effect that shows up
      // as a desync three thousand ticks later.
      expression(node.lhs);
      const Op jump = node.binary_op == BinaryOp::logical_and ? Op::jump_if_false_or_pop
                                                             : Op::jump_if_true_or_pop;
      const std::uint32_t patch_site = emit(jump, 0, 0, node.line);
      expression(node.rhs);
      patch(patch_site, here());
      emit(Op::to_bool, 0, 0, node.line);
      return;
    }

    expression(node.lhs);
    expression(node.rhs);
    switch (node.binary_op) {
      case BinaryOp::add: emit(Op::add, 0, 0, node.line); return;
      case BinaryOp::sub: emit(Op::sub, 0, 0, node.line); return;
      case BinaryOp::mul: emit(Op::mul, 0, 0, node.line); return;
      case BinaryOp::div: emit(Op::div, 0, 0, node.line); return;
      case BinaryOp::mod: emit(Op::mod, 0, 0, node.line); return;
      case BinaryOp::eq: emit(Op::cmp_eq, 0, 0, node.line); return;
      case BinaryOp::ne: emit(Op::cmp_ne, 0, 0, node.line); return;
      case BinaryOp::lt: emit(Op::cmp_lt, 0, 0, node.line); return;
      case BinaryOp::le: emit(Op::cmp_le, 0, 0, node.line); return;
      case BinaryOp::gt: emit(Op::cmp_gt, 0, 0, node.line); return;
      case BinaryOp::ge: emit(Op::cmp_ge, 0, 0, node.line); return;
      case BinaryOp::logical_and:
      case BinaryOp::logical_or: return;  // handled above
    }
    fail(node.line, "unknown binary operator");
  }

  /// True when `index` names a plain local, and if so which slot.
  ///
  /// This is the whole of out-parameter detection. VS marks nothing:
  /// `g.Eval(flags, idPlayer, own, ally, enemy, enemy_hidden)` writes through
  /// its last four arguments and looks like any other call. So every argument
  /// that *could* be written back is recorded as one, and a host entry point
  /// that does not write simply leaves the value alone.
  bool argument_slot(NodeIndex index, std::uint32_t& slot) const {
    if (!valid_expr(index)) return false;
    const Expr& node = script_.expressions[index];
    if (node.kind == ExprKind::name) return lookup(node.text, slot);
    if (node.kind == ExprKind::this_) return lookup("this", slot);
    return false;
  }

  void call(const Expr& node, CallKind kind) {
    CallSite site;
    site.kind = kind;
    site.arity = static_cast<std::uint16_t>(node.argument_count);
    site.name_const = string_constant(node.text);

    // Collect this call's writebacks locally, and append them only once every
    // sub-expression has been compiled.
    //
    // Compiling an argument can itself emit a call, which appends writebacks of
    // its own. Reserving `writeback_begin` up front therefore swallowed the
    // nested call's entries into this call's range, and the VM copied the wrong
    // values back into locals. In `deer_idle.vs` that turned
    // `ol = ObjsInSight(this, "...").GetObjList` into an assignment of the
    // query handle to `this`, so every later `.Goto(...)` ran with a query as
    // its receiver. It stayed invisible for as long as host functions ignored
    // their receiver, and surfaced the moment one looked.
    std::vector<Writeback> pending;

    std::uint16_t argument = 0;
    if (kind == CallKind::member) {
      expression(node.lhs);
      std::uint32_t slot = 0;
      if (argument_slot(node.lhs, slot)) {
        pending.push_back(Writeback{argument, slot});
      }
      ++argument;
    }

    for (std::uint32_t i = 0; i < node.argument_count; ++i) {
      const std::uint32_t at = node.argument_begin + i;
      if (at >= script_.arguments.size()) {
        fail(node.line, "argument index out of range");
        return;
      }
      const NodeIndex argument_expr = script_.arguments[at];
      expression(argument_expr);
      std::uint32_t slot = 0;
      if (argument_slot(argument_expr, slot)) {
        pending.push_back(Writeback{argument, slot});
      }
      ++argument;
    }

    site.writeback_begin = static_cast<std::uint32_t>(chunk_.writebacks.size());
    chunk_.writebacks.insert(chunk_.writebacks.end(), pending.begin(), pending.end());
    site.writeback_count = static_cast<std::uint32_t>(pending.size());
    if (host_ != nullptr) site.host_index = host_->find(kind, node.text, site.arity);

    chunk_.call_sites.push_back(site);
    emit(Op::call, static_cast<std::uint32_t>(chunk_.call_sites.size() - 1), 0, node.line);
  }

  // -- statements --------------------------------------------------------

  void statement(NodeIndex index) {
    if (failed_) return;
    const Stmt& node = statement_at(index);
    if (failed_) return;
    if (node.line != 0) last_line_ = node.line;

    switch (node.kind) {
      case StmtKind::declare: return declare(node);
      case StmtKind::assign: return assign(node);
      case StmtKind::expr:
        expression(node.expr);
        emit(Op::pop, 0, 0, node.line);
        return;
      case StmtKind::if_: return if_statement(node);
      case StmtKind::while_: return while_statement(node);
      case StmtKind::return_:
        if (node.expr == kNoNode) {
          emit(Op::return_void, 0, 0, node.line);
        } else {
          expression(node.expr);
          emit(Op::return_value, 0, 0, node.line);
        }
        return;
      case StmtKind::break_:
        if (loops_.empty()) return fail(node.line, "break outside a loop");
        loops_.back().breaks.push_back(emit(Op::jump, 0, 0, node.line));
        return;
      case StmtKind::continue_:
        if (loops_.empty()) return fail(node.line, "continue outside a loop");
        loops_.back().continues.push_back(emit(Op::jump, 0, 0, node.line));
        return;
      case StmtKind::block: {
        const StatementRange range = block_children(node);
        push_scope();
        for (std::uint32_t i = 0; i < range.count && !failed_; ++i) {
          statement(range.begin + i);
        }
        pop_scope();
        return;
      }
    }
    fail(node.line, "unknown statement kind");
  }

  void declare(const Stmt& node) {
    const std::string type_name = node.type_name.empty()
                                      ? std::string(default_type_spelling(node.declared_type))
                                      : std::string(node.type_name);
    for (std::uint32_t i = 0; i < node.name_count; ++i) {
      const std::uint32_t at = node.name_begin + i;
      if (at >= script_.names.size()) return fail(node.line, "declared name index out of range");
      const std::uint32_t slot = declare_local(script_.names[at], type_name, false, false);
      // Handles and arrays default-construct to an invalid or empty state: 331
      // files declare a handle and assign it several statements later, and many
      // declare an ObjList and immediately `.Clear` or `.Add` on it.
      emit(Op::declare_local, slot, string_constant(type_name), node.line);
    }
  }

  void assign(const Stmt& node) {
    if (!valid_expr(node.expr)) return fail(node.line, "assignment with no target");
    const Expr& target = script_.expressions[node.expr];

    if (target.kind == ExprKind::name || target.kind == ExprKind::this_) {
      const std::string_view name = target.kind == ExprKind::this_ ? "this" : target.text;
      std::uint32_t slot = 0;
      if (!lookup(name, slot)) {
        // Nothing in the corpus assigns to a name it never declared, and a host
        // constant is not an lvalue -- `AI_COMING = 1` would be a script bug.
        // Refusing here turns that into a finding instead of a silent local.
        return fail(node.line, "assignment to undeclared name '" + std::string(name) + "'");
      }
      expression(node.value);
      emit(Op::store_local, slot, 0, node.line);
      return;
    }

    if (target.kind == ExprKind::index) {
      expression(target.lhs);
      expression(target.rhs);
      expression(node.value);
      // `a` carries slot + 1 when the container is a plain local, so that a
      // host whose `index_set` has to re-seat the handle (a growing IntArray,
      // say) can, and the local sees it. Zero means "not an lvalue".
      std::uint32_t slot = 0;
      const std::uint32_t operand = argument_slot(target.lhs, slot) ? slot + 1 : 0;
      emit(Op::index_set, operand, 0, node.line);
      return;
    }

    // Across all 577 files an assignment target is only ever a plain name
    // (5,608 sites) or a subscript (197). A member is never assigned to; all
    // mutation goes through `SetXxx` methods, which is a firm constraint on the
    // host object model rather than an accident of the sample.
    fail(node.line, "assignment target is neither a name nor a subscript");
  }

  void if_statement(const Stmt& node) {
    expression(node.expr);
    const std::uint32_t to_else = emit(Op::jump_if_false, 0, 0, node.line);
    if (node.then_branch != kNoNode) statement(node.then_branch);

    if (node.else_branch == kNoNode) {
      patch(to_else, here());
      return;
    }
    const std::uint32_t to_end = emit(Op::jump, 0, 0, node.line);
    patch(to_else, here());
    statement(node.else_branch);
    patch(to_end, here());
  }

  /// `while`, and `for` -- which is the same shape with one extra statement.
  ///
  /// The front end normalises `for (init; cond; step) body` into
  /// `block { init; while (cond) body }` and carries `step` on the `while_` in
  /// `else_branch`, which loops do not otherwise use. It deliberately does *not*
  /// append the step to the body, because 92 of the corpus's 351 `for` loops
  /// `continue` out of their body and a `continue` that skipped the step would
  /// spin forever. So the step gets its own label, and `continue` jumps there.
  void while_statement(const Stmt& node) {
    const std::uint32_t top = here();
    expression(node.expr);
    const std::uint32_t to_end = emit(Op::jump_if_false, 0, 0, node.line);

    loops_.emplace_back();
    if (node.then_branch != kNoNode) statement(node.then_branch);
    LoopContext loop = std::move(loops_.back());
    loops_.pop_back();

    // For a plain `while` this label sits on the jump back to the condition, so
    // the two cases need no separate handling.
    const std::uint32_t step = here();
    if (node.else_branch != kNoNode) statement(node.else_branch);
    emit(Op::jump, top, 0, node.line);

    for (const std::uint32_t site : loop.continues) patch(site, step);
    const std::uint32_t end = here();
    patch(to_end, end);
    for (const std::uint32_t site : loop.breaks) patch(site, end);
  }

  struct Binding {
    std::string name;
    std::uint32_t slot;
  };
  struct LoopContext {
    std::vector<std::uint32_t> breaks;
    std::vector<std::uint32_t> continues;
  };

  const Script& script_;
  const HostRegistry* host_;
  CompileError* error_;
  Chunk chunk_;
  std::vector<std::vector<Binding>> scopes_;
  std::vector<LoopContext> loops_;
  std::uint32_t last_line_ = 0;
  bool failed_ = false;
};

}  // namespace

Result<Chunk> compile(const Script& script, const HostRegistry* host, CompileError* error) {
  if (error != nullptr) *error = CompileError{};
  Compiler compiler(script, host, error);
  return compiler.compile();
}

}  // namespace imperivm::core::script
