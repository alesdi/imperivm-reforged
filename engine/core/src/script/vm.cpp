#include "imperivm/core/script/vm.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace imperivm::core::script {
namespace {

constexpr std::uint32_t kSaveMagic = 0x584D5649u;  // "IVMX", little-endian
constexpr std::uint32_t kSaveVersion = 4;  // 4: an object value carries `aux`

void put_u8(std::vector<std::byte>& out, std::uint32_t value) {
  out.push_back(static_cast<std::byte>(value & 0xFFu));
}
void put_u16(std::vector<std::byte>& out, std::uint32_t value) {
  put_u8(out, value);
  put_u8(out, value >> 8);
}
void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  put_u16(out, value);
  put_u16(out, value >> 16);
}
void put_i64(std::vector<std::byte>& out, std::int64_t value) {
  const std::uint64_t bits = static_cast<std::uint64_t>(value);
  put_u32(out, static_cast<std::uint32_t>(bits & 0xFFFFFFFFu));
  put_u32(out, static_cast<std::uint32_t>(bits >> 32));
}
void put_string(std::vector<std::byte>& out, std::string_view text) {
  put_u32(out, static_cast<std::uint32_t>(text.size()));
  for (const char c : text) put_u8(out, static_cast<std::uint8_t>(c));
}

bool get_i64(ByteReader& reader, std::int64_t& out) {
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  if (!reader.u32(low) || !reader.u32(high)) return false;
  out = static_cast<std::int64_t>((static_cast<std::uint64_t>(high) << 32) | low);
  return true;
}

bool get_string(ByteReader& reader, std::string& out) {
  std::uint32_t length = 0;
  if (!reader.u32(length)) return false;
  std::span<const std::byte> raw;
  if (!reader.bytes(length, raw)) return false;
  out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
  return true;
}

/// The interpreter's working state for one `run`, so the opcode handlers do not
/// each have to rediscover the frame and the chunk.
struct Machine {
  Execution& execution;
  const Chunk& chunk;
  const VmEnv& env;
  Frame* frame = nullptr;

  void trap(TrapCode code, std::string detail, std::uint32_t line) {
    execution.status = ExecStatus::failed;
    execution.trap.code = code;
    execution.trap.detail = std::move(detail);
    execution.trap.source_name = chunk.source_name;
    execution.trap.line = line;
  }

  bool pop(Value& out, std::uint32_t line) {
    if (frame->stack.empty()) {
      trap(TrapCode::stack_underflow, "operand stack empty", line);
      return false;
    }
    out = std::move(frame->stack.back());
    frame->stack.pop_back();
    return true;
  }

  void push(Value value) { frame->stack.push_back(std::move(value)); }

  /// Constant indices come from the compiler, so an out-of-range one is a bug
  /// in this library rather than in a script. It still must not index past the
  /// end of a vector.
  const std::string& constant_string(std::uint32_t index) const {
    static const std::string kEmpty;
    if (index >= chunk.constants.size()) return kEmpty;
    return chunk.constants[index].as_string();
  }

  bool truthy(const Value& value, bool& out, std::uint32_t line) {
    if (!value.is_object()) {
      out = value.truthy_scalar();
      return true;
    }
    if (env.host == nullptr) {
      out = value.as_object().valid();
      return true;
    }
    const Result<bool> answer = env.host->truthy(value);
    if (!answer.ok()) {
      trap(TrapCode::type_mismatch, "host cannot decide the truth of an object value", line);
      return false;
    }
    out = answer.value();
    return true;
  }

  bool stringify(const Value& value, std::string& out, std::uint32_t line) {
    switch (value.kind()) {
      case ValueKind::string: out = value.as_string(); return true;
      case ValueKind::integer: out = to_decimal(value.as_integer()); return true;
      case ValueKind::nil: out.clear(); return true;
      case ValueKind::object: break;
    }
    if (env.host == nullptr) {
      trap(TrapCode::type_mismatch, "no host to render an object as a string", line);
      return false;
    }
    const Result<std::string> answer = env.host->to_string(value);
    if (!answer.ok()) {
      trap(TrapCode::type_mismatch, "host cannot render this object as a string", line);
      return false;
    }
    out = answer.value();
    return true;
  }

  bool arithmetic(BinaryOp op, const Value& lhs, const Value& rhs, Value& out,
                  std::uint32_t line);
  bool compare(BinaryOp op, const Value& lhs, const Value& rhs, Value& out, std::uint32_t line);
  bool host_binary(BinaryOp op, const Value& lhs, const Value& rhs, Value& out,
                   std::uint32_t line, const char* what);
  void do_call(const Instruction& instruction);
};

bool Machine::host_binary(BinaryOp op, const Value& lhs, const Value& rhs, Value& out,
                          std::uint32_t line, const char* what) {
  if (env.host == nullptr) {
    trap(TrapCode::type_mismatch, std::string("no host to evaluate ") + what, line);
    return false;
  }
  Result<Value> answer = env.host->binary(op, lhs, rhs);
  if (!answer.ok()) {
    trap(TrapCode::type_mismatch, std::string("host defines no ") + what, line);
    return false;
  }
  out = std::move(answer.value());
  return true;
}

bool Machine::arithmetic(BinaryOp op, const Value& lhs, const Value& rhs, Value& out,
                         std::uint32_t line) {
  // `+` with a string on either side concatenates and coerces the other
  // operand: `pr("Player " + AIPlayer + " builds new fancy " + nCount)`, and
  // 462 sites like it.
  if (op == BinaryOp::add && (lhs.is_string() || rhs.is_string())) {
    std::string left;
    std::string right;
    if (!stringify(lhs, left, line) || !stringify(rhs, right, line)) return false;
    out = Value::string(left + right);
    return true;
  }

  if (lhs.is_integer() && rhs.is_integer()) {
    const std::int32_t a = lhs.as_integer();
    const std::int32_t b = rhs.as_integer();
    switch (op) {
      case BinaryOp::add: out = Value::integer(static_cast<std::int32_t>(
                              static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)));
        return true;
      case BinaryOp::sub: out = Value::integer(static_cast<std::int32_t>(
                              static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b)));
        return true;
      case BinaryOp::mul: out = Value::integer(static_cast<std::int32_t>(
                              static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b)));
        return true;
      case BinaryOp::div:
      case BinaryOp::mod:
        if (b == 0) {
          trap(TrapCode::divide_by_zero, op == BinaryOp::div ? "division by zero"
                                                             : "modulo by zero", line);
          return false;
        }
        // The one case C++ leaves undefined rather than merely
        // implementation-defined. Pinning it keeps two clients agreeing.
        if (a == INT32_MIN && b == -1) {
          out = Value::integer(op == BinaryOp::div ? INT32_MIN : 0);
          return true;
        }
        out = Value::integer(op == BinaryOp::div ? a / b : a % b);
        return true;
      default: break;
    }
  }

  // `point + point`, `point * int`, `point / int` -- the host owns the types
  // and therefore the arithmetic.
  return host_binary(op, lhs, rhs, out, line, "arithmetic on these operand types");
}

bool Machine::compare(BinaryOp op, const Value& lhs, const Value& rhs, Value& out,
                      std::uint32_t line) {
  const bool equality = op == BinaryOp::eq || op == BinaryOp::ne;

  if (lhs.is_integer() && rhs.is_integer()) {
    const std::int32_t a = lhs.as_integer();
    const std::int32_t b = rhs.as_integer();
    bool result = false;
    switch (op) {
      case BinaryOp::eq: result = a == b; break;
      case BinaryOp::ne: result = a != b; break;
      case BinaryOp::lt: result = a < b; break;
      case BinaryOp::le: result = a <= b; break;
      case BinaryOp::gt: result = a > b; break;
      case BinaryOp::ge: result = a >= b; break;
      default: break;
    }
    out = Value::boolean(result);
    return true;
  }

  const bool string_and_integer = (lhs.is_string() && rhs.is_integer()) ||
                                  (lhs.is_integer() && rhs.is_string());
  if ((lhs.is_string() && rhs.is_string()) || string_and_integer) {
    // `sqLeader.command == "idle"`. Case sensitivity is unattested -- every
    // comparand in the corpus is a lowercase command verb -- so this is exact
    // until something proves otherwise.
    //
    // A string against an integer compares as strings: `EnvReadString(set,
    // "NeedTechSucceed") != 0` (`DATA\AI\ESH_BUILDARMY.VS:364,381`, the only
    // two such sites in the corpus) reads a slot `EnvWriteInt` filled with
    // "0" or "1". INFERRED: the retail compiler resolves the operator through
    // a conversion -- it refuses with "Infix operator %s can not be applied."
    // otherwise, and the file ships and the AI researches -- and int->str is
    // the conversion the language already has for `+`; str->int exists only
    // as `Str2Int`. The two readings differ only on the first read of an
    // unset key, "" against 0.
    std::string left;
    std::string right;
    if (!stringify(lhs, left, line) || !stringify(rhs, right, line)) return false;
    const int order = left.compare(right);
    bool result = false;
    switch (op) {
      case BinaryOp::eq: result = order == 0; break;
      case BinaryOp::ne: result = order != 0; break;
      case BinaryOp::lt: result = order < 0; break;
      case BinaryOp::le: result = order <= 0; break;
      case BinaryOp::gt: result = order > 0; break;
      case BinaryOp::ge: result = order >= 0; break;
      default: break;
    }
    out = Value::boolean(result);
    return true;
  }

  if (equality && (lhs.is_object() || rhs.is_object() || lhs.is_nil() || rhs.is_nil())) {
    // Handle identity. Ordering handles is not a thing the language does, so
    // only equality is answered here; anything else falls through to the host.
    const bool same = lhs == rhs;
    out = Value::boolean(op == BinaryOp::eq ? same : !same);
    return true;
  }

  return host_binary(op, lhs, rhs, out, line, "comparison of these operand types");
}

void Machine::do_call(const Instruction& instruction) {
  if (instruction.a >= chunk.call_sites.size()) {
    trap(TrapCode::bad_instruction, "call site index out of range", instruction.line);
    return;
  }
  const CallSite& site = chunk.call_sites[instruction.a];
  const std::string& name = constant_string(site.name_const);
  const std::size_t window =
      static_cast<std::size_t>(site.arity) + (site.kind == CallKind::member ? 1u : 0u);

  if (frame->stack.size() < window) {
    trap(TrapCode::stack_underflow, "call has fewer arguments on the stack than it declared",
         instruction.line);
    return;
  }

  const std::string signature = name + "/" + to_decimal(static_cast<std::int32_t>(site.arity));

  std::uint32_t index = site.host_index;
  if (index == kUnresolvedHost && env.registry != nullptr) {
    // The chunk was compiled without a registry -- which is the useful way to
    // measure language coverage over the corpus -- so resolve it now.
    index = env.registry->find(site.kind, name, site.arity);
  }
  if (index == kUnresolvedHost || env.registry == nullptr) {
    trap(TrapCode::host_not_registered, "no host entry point " + signature, instruction.line);
    return;
  }
  const HostEntry& entry = env.registry->entry(index);
  if (entry.fn == nullptr) {
    trap(TrapCode::host_not_implemented, "host entry point " + signature + " is declared but "
                                         "not implemented",
         instruction.line);
    return;
  }

  CallContext context;
  context.arguments = std::span<Value>(frame->stack.data() + frame->stack.size() - window, window);
  context.host = env.host;
  context.scheduler = env.scheduler;
  context.script = env.script;
  context.user = env.user;
  context.name = name;
  context.kind = site.kind;
  context.now = env.now;

  // Is this call the one already retrying, or a fresh one? See `Execution`:
  // the flag is cleared by every outcome that is not a retry, so a set flag
  // means this very call, and a clear one means a wait whose timeout starts
  // over -- which is what a `while` loop polling a condition needs on every
  // iteration.
  context.waiting_since = execution.retrying ? execution.retry_since : env.now;
  context.first_call = !execution.retrying;

  const HostOutcome outcome = entry.fn(context);
  if (env.call_trace != nullptr) {
    env.call_trace(env.trace_user, env.script, chunk.source_name, name, context.arguments,
                   outcome, instruction.line);
  }

  if (outcome.status == HostStatus::error) {
    trap(TrapCode::host_error,
         signature + ": " + (outcome.error != nullptr ? outcome.error : "refused"),
         instruction.line);
    return;
  }

  if (outcome.status == HostStatus::retry) {
    // The call has not happened as far as the script is concerned: leave the
    // arguments where they are and the instruction pointer where it is, so the
    // whole call runs again when the wait is over. This is what a re-polling
    // wait such as `WaitNonEmptyQuery` needs.
    execution.status = ExecStatus::suspended;
    execution.wake_time = env.now + outcome.suspend_for;
    // Only on the first retry: overwriting `retry_since` on every poll would
    // reset the timeout each time and no `Wait*` in the game would ever expire.
    if (!execution.retrying) {
      execution.retrying = true;
      execution.retry_since = env.now;
    }
    return;
  }

  // Any other outcome means this call is done, so the retry sequence -- if this
  // was one -- ends here. **This clear is the whole identity mechanism**: it is
  // what lets the next wait, at this call site or any other, start its timeout
  // from zero. Without it a script's second `Wait*` inherits the first one's
  // stopwatch and gives up on its first poll.
  execution.retrying = false;
  execution.retry_since = 0;

  if (outcome.status == HostStatus::finish) {
    // The original's host code 2: the script stops at this call, with nothing
    // pushed and nothing after it run. See `script/host.hpp`.
    execution.result = Value::nil();
    execution.status = ExecStatus::finished;
    return;
  }

  // Out-parameters. Everything assignable that was passed in goes back where it
  // came from, whether or not the host touched it; a host that did not write
  // simply put the same value back.
  for (std::uint32_t i = 0; i < site.writeback_count; ++i) {
    const Writeback& writeback = chunk.writebacks[site.writeback_begin + i];
    if (writeback.argument >= window || writeback.slot >= frame->locals.size()) continue;
    frame->locals[writeback.slot] = context.arguments[writeback.argument];
  }

  frame->stack.resize(frame->stack.size() - window);
  push(outcome.value);
  ++frame->ip;

  if (outcome.status == HostStatus::suspend) {
    execution.status = ExecStatus::suspended;
    execution.wake_time = env.now + outcome.suspend_for;
  }
}

}  // namespace

Execution start(const Chunk& chunk, std::span<const Value> args, Host* host) {
  Execution execution;
  execution.frames.emplace_back();
  Frame& frame = execution.frames.back();
  frame.locals.assign(chunk.local_count(), Value::nil());
  // Extra arguments are dropped. `AIRun` is variadic in the corpus against
  // scripts with fixed signatures, and a mismatch there must not be fatal.
  const std::size_t bound = args.size() < chunk.parameter_count ? args.size()
                                                                : chunk.parameter_count;
  for (std::size_t i = 0; i < bound; ++i) frame.locals[i] = args[i];
  // A missing one is its type's default, as the header says. The plain
  // `default_value` rather than the declaration-site one: a parameter is not a
  // declaration, and a pooled type keyed by a site it never had would mint a
  // pool entry per spawn.
  if (host != nullptr) {
    for (std::size_t i = bound; i < chunk.parameter_count && i < chunk.locals.size(); ++i) {
      frame.locals[i] = host->default_value(chunk.locals[i].type_name);
    }
  }
  execution.status = ExecStatus::ready;
  return execution;
}

ExecStatus step(Execution& execution, const Chunk& chunk, const VmEnv& env) {
  if (execution.status != ExecStatus::ready) return execution.status;
  if (execution.frames.empty()) {
    execution.status = ExecStatus::finished;
    return execution.status;
  }

  Machine machine{execution, chunk, env, &execution.frames.back()};
  Frame& frame = *machine.frame;

  if (frame.ip >= chunk.code.size()) {
    // Falling off the end is a `return;`. The compiler appends one, so this is
    // belt and braces against a hand-built or truncated chunk.
    execution.status = ExecStatus::finished;
    execution.result = Value::nil();
    return execution.status;
  }

  const Instruction instruction = chunk.code[frame.ip];
  const std::uint32_t line = instruction.line;

  switch (instruction.op) {
    case Op::nop: break;

    case Op::push_const:
      if (instruction.a >= chunk.constants.size()) {
        machine.trap(TrapCode::bad_instruction, "constant index out of range", line);
        return execution.status;
      }
      machine.push(chunk.constants[instruction.a]);
      break;

    case Op::load_local:
      if (instruction.a >= frame.locals.size()) {
        machine.trap(TrapCode::bad_instruction, "local slot out of range", line);
        return execution.status;
      }
      machine.push(frame.locals[instruction.a]);
      break;

    case Op::store_local: {
      Value value;
      if (!machine.pop(value, line)) return execution.status;
      if (instruction.a >= frame.locals.size()) {
        machine.trap(TrapCode::bad_instruction, "local slot out of range", line);
        return execution.status;
      }
      // `point` and `rect` are value types wearing handles, so the host gets to
      // copy them here. Reference types (`Unit`, `ObjList`, `Query`) come back
      // unchanged.
      frame.locals[instruction.a] =
          env.host != nullptr ? env.host->clone_for_assign(value) : value;
      break;
    }

    case Op::load_global: {
      const std::string& name = machine.constant_string(instruction.a);
      if (env.host == nullptr) {
        machine.trap(TrapCode::unknown_global, "no host to resolve global '" + name + "'", line);
        return execution.status;
      }
      Result<Value> value = env.host->global(name);
      if (!value.ok()) {
        machine.trap(TrapCode::unknown_global, "unknown global '" + name + "'", line);
        return execution.status;
      }
      machine.push(std::move(value.value()));
      break;
    }

    case Op::declare_local: {
      if (instruction.a >= frame.locals.size()) {
        machine.trap(TrapCode::bad_instruction, "local slot out of range", line);
        return execution.status;
      }
      const std::string& type_name = machine.constant_string(instruction.b);
      // The declaration site travels with the request. A pooled type keys its
      // pool by it, so re-entering a scope reuses one entry instead of minting
      // one per iteration; see `Host::DeclarationSite`.
      const Host::DeclarationSite site{env.script, instruction.a};
      frame.locals[instruction.a] = env.host != nullptr
                                        ? env.host->default_value(type_name, site)
                                        : Value::integer(0);
      break;
    }

    case Op::pop: {
      Value discarded;
      if (!machine.pop(discarded, line)) return execution.status;
      break;
    }

    case Op::negate: {
      Value value;
      if (!machine.pop(value, line)) return execution.status;
      if (value.is_integer()) {
        machine.push(Value::integer(static_cast<std::int32_t>(
            0u - static_cast<std::uint32_t>(value.as_integer()))));
        break;
      }
      Value out;
      if (!machine.host_binary(BinaryOp::sub, Value::integer(0), value, out, line,
                               "negation of this operand type")) {
        return execution.status;
      }
      machine.push(std::move(out));
      break;
    }

    case Op::logical_not: {
      Value value;
      if (!machine.pop(value, line)) return execution.status;
      bool truth = false;
      if (!machine.truthy(value, truth, line)) return execution.status;
      machine.push(Value::boolean(!truth));
      break;
    }

    case Op::to_bool: {
      Value value;
      if (!machine.pop(value, line)) return execution.status;
      bool truth = false;
      if (!machine.truthy(value, truth, line)) return execution.status;
      machine.push(Value::boolean(truth));
      break;
    }

    case Op::add:
    case Op::sub:
    case Op::mul:
    case Op::div:
    case Op::mod: {
      Value rhs;
      Value lhs;
      if (!machine.pop(rhs, line) || !machine.pop(lhs, line)) return execution.status;
      BinaryOp op = BinaryOp::add;
      switch (instruction.op) {
        case Op::sub: op = BinaryOp::sub; break;
        case Op::mul: op = BinaryOp::mul; break;
        case Op::div: op = BinaryOp::div; break;
        case Op::mod: op = BinaryOp::mod; break;
        default: break;
      }
      Value out;
      if (!machine.arithmetic(op, lhs, rhs, out, line)) return execution.status;
      machine.push(std::move(out));
      break;
    }

    case Op::cmp_eq:
    case Op::cmp_ne:
    case Op::cmp_lt:
    case Op::cmp_le:
    case Op::cmp_gt:
    case Op::cmp_ge: {
      Value rhs;
      Value lhs;
      if (!machine.pop(rhs, line) || !machine.pop(lhs, line)) return execution.status;
      BinaryOp op = BinaryOp::eq;
      switch (instruction.op) {
        case Op::cmp_ne: op = BinaryOp::ne; break;
        case Op::cmp_lt: op = BinaryOp::lt; break;
        case Op::cmp_le: op = BinaryOp::le; break;
        case Op::cmp_gt: op = BinaryOp::gt; break;
        case Op::cmp_ge: op = BinaryOp::ge; break;
        default: break;
      }
      Value out;
      if (!machine.compare(op, lhs, rhs, out, line)) return execution.status;
      machine.push(std::move(out));
      break;
    }

    case Op::jump:
      frame.ip = instruction.a;
      return execution.status;

    case Op::jump_if_false: {
      Value value;
      if (!machine.pop(value, line)) return execution.status;
      bool truth = false;
      if (!machine.truthy(value, truth, line)) return execution.status;
      frame.ip = truth ? frame.ip + 1 : instruction.a;
      return execution.status;
    }

    case Op::jump_if_false_or_pop:
    case Op::jump_if_true_or_pop: {
      if (frame.stack.empty()) {
        machine.trap(TrapCode::stack_underflow, "short circuit with an empty stack", line);
        return execution.status;
      }
      bool truth = false;
      if (!machine.truthy(frame.stack.back(), truth, line)) return execution.status;
      const bool jump = instruction.op == Op::jump_if_false_or_pop ? !truth : truth;
      if (jump) {
        frame.ip = instruction.a;
      } else {
        frame.stack.pop_back();
        ++frame.ip;
      }
      return execution.status;
    }

    case Op::call:
      machine.do_call(instruction);
      return execution.status;

    case Op::index_get: {
      Value key;
      Value container;
      if (!machine.pop(key, line) || !machine.pop(container, line)) return execution.status;
      if (env.host == nullptr) {
        machine.trap(TrapCode::bad_index, "no host to subscript this value", line);
        return execution.status;
      }
      Result<Value> element = env.host->index_get(container, key);
      if (!element.ok()) {
        machine.trap(TrapCode::bad_index, "host refused this subscript", line);
        return execution.status;
      }
      machine.push(std::move(element.value()));
      break;
    }

    case Op::index_set: {
      Value value;
      Value key;
      Value container;
      if (!machine.pop(value, line) || !machine.pop(key, line) ||
          !machine.pop(container, line)) {
        return execution.status;
      }
      if (env.host == nullptr) {
        machine.trap(TrapCode::bad_index, "no host to subscript this value", line);
        return execution.status;
      }
      const Status stored = env.host->index_set(container, key, value);
      if (!stored.ok()) {
        machine.trap(TrapCode::bad_index, "host refused this subscript assignment", line);
        return execution.status;
      }
      if (instruction.a != 0 && instruction.a - 1 < frame.locals.size()) {
        frame.locals[instruction.a - 1] = container;
      }
      break;
    }

    case Op::return_value: {
      Value value;
      if (!machine.pop(value, line)) return execution.status;
      execution.result = std::move(value);
      execution.status = ExecStatus::finished;
      return execution.status;
    }

    case Op::return_void:
      execution.result = Value::nil();
      execution.status = ExecStatus::finished;
      return execution.status;
  }

  ++frame.ip;
  return execution.status;
}

ExecStatus run(Execution& execution, const Chunk& chunk, const VmEnv& env,
               std::uint64_t* instructions_executed) {
  std::uint64_t executed = 0;
  while (execution.status == ExecStatus::ready) {
    if (executed >= env.instruction_budget) {
      Machine machine{execution, chunk, env,
                      execution.frames.empty() ? nullptr : &execution.frames.back()};
      const std::uint32_t line =
          execution.frames.empty() || execution.frames.back().ip >= chunk.code.size()
              ? 0u
              : chunk.code[execution.frames.back().ip].line;
      machine.trap(TrapCode::budget_exceeded,
                   "ran an entire instruction budget without yielding", line);
      break;
    }
    step(execution, chunk, env);
    ++executed;
  }
  if (instructions_executed != nullptr) *instructions_executed = executed;
  return execution.status;
}

std::string describe(const Trap& trap) {
  const char* code = "ok";
  switch (trap.code) {
    case TrapCode::none: code = "ok"; break;
    case TrapCode::host_not_registered: code = "host-not-registered"; break;
    case TrapCode::host_not_implemented: code = "host-not-implemented"; break;
    case TrapCode::host_error: code = "host-error"; break;
    case TrapCode::unknown_global: code = "unknown-global"; break;
    case TrapCode::type_mismatch: code = "type-mismatch"; break;
    case TrapCode::divide_by_zero: code = "divide-by-zero"; break;
    case TrapCode::bad_index: code = "bad-index"; break;
    case TrapCode::stack_underflow: code = "stack-underflow"; break;
    case TrapCode::bad_instruction: code = "bad-instruction"; break;
    case TrapCode::budget_exceeded: code = "budget-exceeded"; break;
    case TrapCode::no_such_script: code = "no-such-script"; break;
  }
  std::string out = trap.source_name;
  out += ":";
  out += to_decimal(static_cast<std::int32_t>(trap.line));
  out += ": ";
  out += code;
  out += ": ";
  out += trap.detail;
  return out;
}

// -- serialisation ---------------------------------------------------------

void write_value(std::vector<std::byte>& out, const Value& value) {
  put_u8(out, static_cast<std::uint8_t>(value.kind()));
  switch (value.kind()) {
    case ValueKind::nil: break;
    case ValueKind::integer:
      put_u32(out, static_cast<std::uint32_t>(value.as_integer()));
      break;
    case ValueKind::string: put_string(out, value.as_string()); break;
    case ValueKind::object:
      put_u16(out, value.as_object().type);
      put_u32(out, value.as_object().id);
      put_u32(out, value.as_object().aux);
      break;
  }
}

bool read_value(ByteReader& reader, Value& out) {
  std::uint8_t kind = 0;
  if (!reader.u8(kind)) return false;
  switch (static_cast<ValueKind>(kind)) {
    case ValueKind::nil: out = Value::nil(); return true;
    case ValueKind::integer: {
      std::uint32_t bits = 0;
      if (!reader.u32(bits)) return false;
      out = Value::integer(static_cast<std::int32_t>(bits));
      return true;
    }
    case ValueKind::string: {
      std::string text;
      if (!get_string(reader, text)) return false;
      out = Value::string(std::move(text));
      return true;
    }
    case ValueKind::object: {
      std::uint16_t type = 0;
      std::uint32_t id = 0;
      std::uint32_t aux = 0;
      if (!reader.u16(type) || !reader.u32(id) || !reader.u32(aux)) return false;
      out = Value::object(ObjectRef{type, id, aux});
      return true;
    }
  }
  return false;
}

void serialize(const Execution& execution, std::vector<std::byte>& out) {
  put_u32(out, kSaveMagic);
  put_u32(out, kSaveVersion);
  // **`chunk_index` is deliberately not written.** It is an index into one
  // session's library, and a library is built by compiling files in whatever
  // order that session happened to need them -- so the same coroutine is a
  // different number in two sessions of one save, and writing it made the save
  // carry a value that means nothing outside the process that produced it.
  // `Scheduler::serialize` writes the chunk's *source name* for exactly that
  // reason and `Scheduler::deserialize` overwrites this field from the by-name
  // lookup the moment it has one, so nothing ever read what was written here.
  //
  // What it cost was the byte-identical re-save check: four of the shipped
  // conquest's seven maps failed it on this field alone, off by one, because
  // the load compiles in the order the save names its scripts and the original
  // session compiled in the order it happened to start them.
  put_u8(out, static_cast<std::uint8_t>(execution.status));
  put_i64(out, execution.wake_time);
  // The retry bookkeeping. A script suspended inside a `Wait*` is suspended
  // *at* the call with its arguments still on the frame stack, so the frames
  // below already carry the whole condition; these three are the only thing
  // that would otherwise be lost, and losing them would restart every pending
  // timeout at zero on load.
  put_u8(out, execution.retrying ? 1u : 0u);
  put_i64(out, execution.retry_since);
  write_value(out, execution.result);

  put_u8(out, static_cast<std::uint8_t>(execution.trap.code));
  put_string(out, execution.trap.detail);
  put_string(out, execution.trap.source_name);
  put_u32(out, execution.trap.line);

  put_u32(out, static_cast<std::uint32_t>(execution.frames.size()));
  for (const Frame& frame : execution.frames) {
    put_u32(out, frame.ip);
    put_u32(out, static_cast<std::uint32_t>(frame.locals.size()));
    for (const Value& value : frame.locals) write_value(out, value);
    put_u32(out, static_cast<std::uint32_t>(frame.stack.size()));
    for (const Value& value : frame.stack) write_value(out, value);
  }
}

Result<Execution> deserialize(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kSaveMagic) return FormatError::bad_magic;
  if (version != kSaveVersion) return FormatError::unsupported;

  // `chunk_index` is left at its default; see `serialize`. Whoever restores a
  // coroutine binds it -- `Scheduler::deserialize` from the record's source
  // name, and a bare `run` does not read it at all.
  Execution execution;
  std::uint8_t status = 0;
  if (!reader.u8(status)) return FormatError::truncated;
  execution.status = static_cast<ExecStatus>(status);
  if (!get_i64(reader, execution.wake_time)) return FormatError::truncated;
  std::uint8_t retrying = 0;
  if (!reader.u8(retrying) || !get_i64(reader, execution.retry_since)) {
    return FormatError::truncated;
  }
  execution.retrying = retrying != 0;
  if (!read_value(reader, execution.result)) return FormatError::truncated;

  std::uint8_t trap_code = 0;
  if (!reader.u8(trap_code)) return FormatError::truncated;
  execution.trap.code = static_cast<TrapCode>(trap_code);
  if (!get_string(reader, execution.trap.detail)) return FormatError::truncated;
  if (!get_string(reader, execution.trap.source_name)) return FormatError::truncated;
  if (!reader.u32(execution.trap.line)) return FormatError::truncated;

  std::uint32_t frame_count = 0;
  if (!reader.u32(frame_count)) return FormatError::truncated;
  for (std::uint32_t f = 0; f < frame_count; ++f) {
    Frame frame;
    std::uint32_t count = 0;
    if (!reader.u32(frame.ip) || !reader.u32(count)) return FormatError::truncated;
    frame.locals.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (!read_value(reader, frame.locals[i])) return FormatError::truncated;
    }
    if (!reader.u32(count)) return FormatError::truncated;
    frame.stack.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (!read_value(reader, frame.stack[i])) return FormatError::truncated;
    }
    execution.frames.push_back(std::move(frame));
  }
  return execution;
}

}  // namespace imperivm::core::script
