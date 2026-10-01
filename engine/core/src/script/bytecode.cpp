#include "imperivm/core/script/bytecode.hpp"

namespace imperivm::core::script {

const char* op_name(Op op) {
  switch (op) {
    case Op::nop: return "nop";
    case Op::push_const: return "push_const";
    case Op::load_local: return "load_local";
    case Op::store_local: return "store_local";
    case Op::load_global: return "load_global";
    case Op::declare_local: return "declare_local";
    case Op::pop: return "pop";
    case Op::negate: return "negate";
    case Op::logical_not: return "logical_not";
    case Op::to_bool: return "to_bool";
    case Op::add: return "add";
    case Op::sub: return "sub";
    case Op::mul: return "mul";
    case Op::div: return "div";
    case Op::mod: return "mod";
    case Op::cmp_eq: return "cmp_eq";
    case Op::cmp_ne: return "cmp_ne";
    case Op::cmp_lt: return "cmp_lt";
    case Op::cmp_le: return "cmp_le";
    case Op::cmp_gt: return "cmp_gt";
    case Op::cmp_ge: return "cmp_ge";
    case Op::jump: return "jump";
    case Op::jump_if_false: return "jump_if_false";
    case Op::jump_if_false_or_pop: return "jump_if_false_or_pop";
    case Op::jump_if_true_or_pop: return "jump_if_true_or_pop";
    case Op::call: return "call";
    case Op::index_get: return "index_get";
    case Op::index_set: return "index_set";
    case Op::return_value: return "return_value";
    case Op::return_void: return "return_void";
  }
  return "?";
}

std::string disassemble(const Chunk& chunk) {
  std::string out = chunk.source_name;
  out += "\n";
  for (std::size_t i = 0; i < chunk.code.size(); ++i) {
    const Instruction& instruction = chunk.code[i];
    out += to_decimal(static_cast<std::int32_t>(i));
    out += "\t";
    out += op_name(instruction.op);

    switch (instruction.op) {
      case Op::push_const:
      case Op::load_global:
        out += " ";
        if (instruction.a < chunk.constants.size()) {
          const Value& constant = chunk.constants[instruction.a];
          out += constant.is_string() ? "\"" + constant.as_string() + "\""
                                      : to_decimal(constant.as_integer());
        }
        break;
      case Op::load_local:
      case Op::store_local:
      case Op::declare_local:
        out += " ";
        out += instruction.a < chunk.locals.size() ? chunk.locals[instruction.a].name
                                                   : std::string("?");
        break;
      case Op::jump:
      case Op::jump_if_false:
      case Op::jump_if_false_or_pop:
      case Op::jump_if_true_or_pop:
        out += " -> ";
        out += to_decimal(static_cast<std::int32_t>(instruction.a));
        break;
      case Op::call:
        if (instruction.a < chunk.call_sites.size()) {
          const CallSite& site = chunk.call_sites[instruction.a];
          out += site.kind == CallKind::member ? " ." : " ";
          if (site.name_const < chunk.constants.size()) {
            out += chunk.constants[site.name_const].as_string();
          }
          out += "/";
          out += to_decimal(site.arity);
          if (site.writeback_count != 0) {
            out += " writeback:";
            out += to_decimal(static_cast<std::int32_t>(site.writeback_count));
          }
        }
        break;
      default: break;
    }

    out += "\n";
  }
  return out;
}

}  // namespace imperivm::core::script
