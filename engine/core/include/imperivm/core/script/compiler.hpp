#pragma once

// AST to bytecode.
//
// A single pass over the tree, emitting instructions as it goes and patching
// jump targets on the way out. There is nothing to optimise and nothing to
// analyse: VS has no user functions to inline, no recursion to unroll and no
// types the compiler can act on, since every type but `int` and `str` is opaque
// host state. The compiler's real jobs are three:
//
//   * **allocate local slots and get scoping right.** Blocks shadow, and the
//     corpus leans on it: `This` and `this`, `Bld` and `bld`, `SL` and `sl` are
//     live at once in the same files, and `int i;` is redeclared in nested
//     blocks in dozens of scripts. Slots are never reused across scopes, so a
//     suspended frame's locals stay meaningful for a debugger and a save file.
//
//   * **decide which arguments can be written back.** VS marks out-parameters
//     nowhere at the call site, so anything that is a plain local is recorded
//     as a writeback slot and the VM copies it back after the host returns.
//     See bytecode.hpp.
//
//   * **resolve host calls, or admit it could not.** With a registry, call
//     sites carry a resolved index and the interpreter never looks a name up.
//     Without one, they carry `kUnresolvedHost` and the VM traps by name at run
//     time. Compiling without a registry is the useful case for measuring
//     language coverage across the corpus: it answers "does the instruction set
//     cover this script", not "has anyone implemented `SetPlayerStatus` yet".

#include <cstdint>
#include <string>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/bytecode.hpp"
#include "imperivm/core/script/host.hpp"

namespace imperivm::core::script {

/// Where and why compilation stopped. Scripts are shipped data, so a failure is
/// a finding to report precisely, exactly as a parse failure is.
struct CompileError {
  std::string source_name;
  std::uint32_t line = 0;
  std::string message;
};

/// Compile one parsed script.
///
/// `host` may be null; see the header comment. `error` receives the detail when
/// the coarse `FormatError` is not enough, which for a compiler is always.
Result<Chunk> compile(const Script& script, const HostRegistry* host = nullptr,
                      CompileError* error = nullptr);

}  // namespace imperivm::core::script
