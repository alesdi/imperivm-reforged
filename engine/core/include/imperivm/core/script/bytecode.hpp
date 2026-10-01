#pragma once

// The instruction set a `.vs` script compiles to, and the chunk that holds it.
//
// VS is a small language and this is a small instruction set: no user
// functions, no recursion, no closures, no bitwise operators, no exceptions.
// Everything the language can do is arithmetic, comparison, branching, local
// slots, subscripting, and calling out to the host. Twenty-nine opcodes cover
// all 577 shipped scripts, in 91,847 instructions.
//
// ## Why decoded instructions rather than a byte stream
//
// An `Instruction` is a fixed 16-byte record, not a variable-length byte
// encoding. The engine never ships bytecode -- scripts are plain ASCII in
// `data.pak` and are compiled at load time -- so compactness buys nothing,
// while a fixed record buys a decoder that cannot desynchronise, a line number
// on every instruction for free diagnostics, and a chunk that dumps legibly
// when a conformance run diverges. The chunk is still plain data and still
// serialises as such.
//
// ## Calls
//
// Every call in VS goes to the host: there is nothing else to call. A call site
// is a record in the chunk rather than an operand pile, because dispatch needs
// four things the compiler knows and the interpreter should not have to
// recompute -- the name, the arity, whether a receiver is involved, and which
// arguments are lvalues that must be copied back afterwards.
//
// That last one is the out-parameter mechanism. VS marks nothing at the call
// site: `g.Eval(AI_COMING, idPlayer, own, ally, enemy, enemy_hidden)` writes
// through its last four arguments and looks exactly like a call that does not.
// So the compiler records, for each argument that *is* assignable (a plain
// local), where to put it back, and the VM copies back after the host returns.
// A host entry point that does not write simply leaves the value alone and the
// copy-back is a no-op.

#include <cstdint>
#include <string>
#include <vector>

#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/value.hpp"

namespace imperivm::core::script {

enum class Op : std::uint8_t {
  nop,

  push_const,    ///< a: constant index
  load_local,    ///< a: slot
  store_local,   ///< a: slot; pops
  load_global,   ///< a: constant index (the name); asks the host
  declare_local, ///< a: slot, b: constant index (the type's spelling)

  pop,

  negate,
  logical_not,
  to_bool,       ///< normalise to 0/1 after a short-circuit join

  add, sub, mul, div, mod,
  cmp_eq, cmp_ne, cmp_lt, cmp_le, cmp_gt, cmp_ge,

  jump,                 ///< a: target
  jump_if_false,        ///< a: target; pops
  jump_if_false_or_pop, ///< a: target; `&&`: jumps keeping the value, else pops
  jump_if_true_or_pop,  ///< a: target; `||`: ditto

  call,          ///< a: call-site index; pushes the result (nil for void)

  index_get,     ///< pops key, container; pushes element
  index_set,     ///< pops value, key, container

  return_value,  ///< pops the result and finishes the script
  return_void,   ///< finishes the script with nil
};

/// One decoded instruction. `line` is the source line, kept on every
/// instruction so a trap can name a place in a shipped file.
struct Instruction {
  Op op = Op::nop;
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  std::uint32_t line = 0;
};

enum class CallKind : std::uint8_t {
  free_function,  ///< `Sleep(500)`, `rand(99)`
  member,         ///< `u.IsValid`, `pt.SetLen(15)`; the receiver is argument 0
};

/// Where a written-through argument goes when the host call returns.
struct Writeback {
  /// Index into the argument window. For a member call the receiver is 0 and
  /// the declared arguments start at 1.
  std::uint16_t argument = 0;
  std::uint32_t slot = 0;  ///< local slot to store into
};

inline constexpr std::uint32_t kUnresolvedHost = 0xFFFFFFFFu;

/// Everything dispatch needs about one call site.
struct CallSite {
  CallKind kind = CallKind::free_function;
  /// Declared argument count, excluding the receiver -- so `u.IsValid` has
  /// arity 0, matching how the host API inventory counts it. The window the VM
  /// hands the host is `arity` values for a free call and `arity + 1` for a
  /// member call.
  std::uint16_t arity = 0;
  std::uint32_t name_const = 0;
  /// Resolved at compile time when a registry was supplied, so the interpreter
  /// never does a name lookup. `kUnresolvedHost` means the VM must trap with
  /// the name and arity rather than quietly returning zero.
  std::uint32_t host_index = kUnresolvedHost;
  std::uint32_t writeback_begin = 0;
  std::uint32_t writeback_count = 0;
};

/// Compiled locals, kept for diagnostics and for reading out-parameters back.
struct LocalInfo {
  std::string name;
  std::string type_name;  ///< the declared spelling: `int`, `point`, `GAIKA`
  bool is_parameter = false;
  bool is_out = false;
};

/// A compiled script: code, constants, and the tables the code indexes.
struct Chunk {
  std::string source_name;

  std::vector<Instruction> code;
  std::vector<Value> constants;
  std::vector<CallSite> call_sites;
  std::vector<Writeback> writebacks;
  std::vector<LocalInfo> locals;

  /// Parameters occupy slots `[0, parameter_count)` in declaration order, which
  /// is how a caller binds arguments and reads out-parameters back.
  std::uint32_t parameter_count = 0;
  TypeTag return_type = TypeTag::void_;
  /// True for the second entry mode: a bare expression with no `return`, used
  /// by inline `script="..."` attributes in the class XML. Its value is the
  /// script's result, so the compiler yields the trailing expression rather
  /// than discarding it.
  bool is_expression_only = false;

  [[nodiscard]] std::uint32_t local_count() const {
    return static_cast<std::uint32_t>(locals.size());
  }
};

/// Opcode spelling, for traps and disassembly.
const char* op_name(Op op);

/// A human-readable listing of one chunk. Diagnostics only; nothing in the
/// simulation depends on it.
std::string disassemble(const Chunk& chunk);

}  // namespace imperivm::core::script
