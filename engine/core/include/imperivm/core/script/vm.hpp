#pragma once

// The interpreter, and the execution state it walks.
//
// ## A suspended script is world state
//
// This is the fact the whole design turns on. Scripts are coroutines: `Sleep`
// yields, `AIRun` spawns concurrently, and 101 shipped scripts are an infinite
// loop around a sleep. The original engine's desync dumps record a running
// script per object -- `script=data/subai/deer_idle.vs id=1937 cmd=None` -- so a
// half-finished script is saved, loaded and hashed alongside unit positions and
// RNG seeds.
//
// That rules out the obvious implementation. A tree-walking interpreter has to
// suspend on the host stack, which means real coroutines or threads, and
// neither serialises. `Execution` below is the alternative: an instruction
// pointer, a vector of local slots and an operand stack, all plain values. It
// round-trips through `serialize`/`deserialize` and resumes identically, which
// is the property that makes saves and lockstep determinism possible at all.
//
// Determinism agrees for a second reason. An explicit interpreter loop has no
// dependence on host stack layout, inlining or optimisation level, so the same
// bytecode produces the same trace on every target the core is built for.
//
// ## The frame stack
//
// `Execution::frames` is a stack even though VS never pushes a second frame:
// the language has no user-defined functions and no recursion, and `AIRun`
// starts a *separate* coroutine rather than nesting one. It is a vector anyway
// because the alternative -- a single inline frame -- would have to be widened
// later if inline `script="..."` snippets ever gain nesting, and because a
// stack is what serialises uniformly.
//
// ## Running
//
// `run` never blocks. It returns a status -- ready, suspended, finished, failed
// -- and the caller decides what to do about it. Suspension carries a wake time
// in game-time milliseconds; the scheduler owns the clock, and the core owns no
// clock at all.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/bytecode.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"

namespace imperivm::core::script {

enum class ExecStatus : std::uint8_t {
  ready,      ///< runnable now
  suspended,  ///< waiting for game time to reach `wake_time`
  finished,   ///< returned; `result` holds the value
  failed,     ///< trapped; `trap` says how
};

/// Why a script stopped dead. Every one of these names a specific mistake, on a
/// specific line of a specific shipped file, because a VM that fails vaguely
/// over 577 files of someone else's data is a VM nobody can debug.
enum class TrapCode : std::uint8_t {
  none,
  host_not_registered,   ///< no entry point of that name and arity exists at all
  host_not_implemented,  ///< declared in the inventory, nobody has written it yet
  host_error,            ///< the host function itself refused
  unknown_global,        ///< a bare name that is neither local nor host constant
  type_mismatch,         ///< `"x" * 2`, or an object where the host defines no operator
  divide_by_zero,
  bad_index,
  stack_underflow,       ///< a compiler bug, not a script bug
  bad_instruction,       ///< ditto
  budget_exceeded,       ///< ran a whole budget without yielding: a runaway loop
  no_such_script,        ///< `AIRun` of a name the library does not hold
};

struct Trap {
  TrapCode code = TrapCode::none;
  std::string detail;       ///< the name, operator or slot that failed
  std::string source_name;  ///< the `.vs` file
  std::uint32_t line = 0;
};

/// One activation: where it is, what it knows, what it is holding.
struct Frame {
  std::uint32_t ip = 0;
  std::vector<Value> locals;
  std::vector<Value> stack;
};

/// A script mid-flight. Plain data, start to finish.
struct Execution {
  /// Which chunk this is running, as an index into the scheduler's library. A
  /// bare `run` ignores it.
  ///
  /// **Not serialised, and this comment used to say the opposite.** It said the
  /// field existed so a serialised coroutine could be rebound to its code after
  /// a load, and it was written into the payload on that reading -- but an
  /// index into one session's library is meaningless in another, which is why
  /// `Scheduler::serialize` writes the chunk's *source name* instead and
  /// `Scheduler::deserialize` sets this from the by-name lookup. Nothing ever
  /// read the number that was written. See `serialize` in `script/vm.cpp`.
  std::uint32_t chunk_index = 0;
  std::vector<Frame> frames;
  ExecStatus status = ExecStatus::ready;
  /// Game time, in milliseconds, at which a suspended script becomes runnable.
  std::int64_t wake_time = 0;

  // -- the retry bookkeeping ---------------------------------------------
  //
  // `HostStatus::retry` suspends *at* a call and runs the whole call again on
  // resume, leaving the arguments on the stack and the instruction pointer
  // where it is. That is the blocking primitive the `Wait*` family needs, and
  // by itself it has no memory: a host function re-entered for the ninth time
  // cannot tell that from the first, so it cannot know whether its own timeout
  // has expired.
  //
  // These two fields are that memory, and they are the whole of it: whether a
  // sequence is in progress and when it began, so a blocking entry point can
  // compute its own deadline from `CallContext::waiting_since`. Nothing here
  // knows what any wait is waiting for -- the condition is the entry point's
  // own code, re-evaluated from arguments that are still on the frame stack,
  // which is why a waiting script serialises with no new state beyond these
  // two numbers.
  //
  // **A bare bool is enough because every non-retry outcome clears it**, and
  // only one call per script can be retrying at a time: the instruction pointer
  // does not advance across a retry, so nothing else in that frame can run, and
  // a call that completes -- including one that returns, traps or times out --
  // goes through the clear on its way out. An earlier draft also stored the
  // frame index and the instruction pointer as an identity check; both were
  // injected as faults and *neither could be made to fail a test*, because the
  // clear had already made them unreachable. A guard no case can distinguish is
  // not insurance, it is a fourth thing to keep correct.

  /// Whether a call is part-way through a retry sequence right now.
  bool retrying = false;
  /// Game time at which that sequence began. Meaningless unless `retrying`.
  std::int64_t retry_since = 0;
  Value result;
  Trap trap;
};

/// What the interpreter needs from outside itself. No clock, no allocator, no
/// randomness: whatever the script needs of those, it asks a host function for.
/// A diagnostic tap on every host call a script makes: the name, what it
/// was given, what came back, and the line. Null in every simulation; a
/// headless tool sets it to answer "where does this script stop" without
/// editing the script. It sees the outcome the script sees and cannot change
/// it. See `Scheduler::set_call_trace`.
using CallTrace = void (*)(void* user, ScriptId script, std::string_view source,
                           std::string_view name, std::span<const Value> args,
                           const HostOutcome& outcome, std::int32_t line);

struct VmEnv {
  const HostRegistry* registry = nullptr;
  Host* host = nullptr;
  Scheduler* scheduler = nullptr;
  void* user = nullptr;
  ScriptId script = kNoScript;
  CallTrace call_trace = nullptr;
  void* trace_user = nullptr;
  /// Game time now, in milliseconds. Used to compute a suspension's wake time.
  std::int64_t now = 0;
  /// Instructions one `run` may execute before the script is declared runaway.
  /// A script that means to run forever calls `Sleep`; 101 of them do exactly
  /// that. One that does not is a bug, and a bug that hangs the simulation is
  /// worse than one that traps.
  std::uint64_t instruction_budget = 1u << 20;
};

/// Bind `args` to the chunk's parameters and prepare frame zero.
///
/// Extra arguments are dropped, because `AIRun` is variadic in the corpus
/// (arities 1, 2 and 5 all occur against scripts with fixed signatures) and a
/// mismatch there must not be fatal. **A missing one takes its declared
/// type's default** when a host is given -- the same value a `point pt;` local
/// would be declared with -- and is left nil without one. That is what a
/// parameter the caller never pushed reads as in the original, where a frame's
/// slots start zeroed: `HERO_STAND_GROUND.VS` is declared `(Obj me, point pt)`,
/// re-issues itself as `SetCommand("stand_position")` with no point at all,
/// and reads `pt` on the next pass. Nil there was a trap on every hero told to
/// stand its ground; `(0, 0)` is what it was.
Execution start(const Chunk& chunk, std::span<const Value> args = {}, Host* host = nullptr);

/// Execute one instruction. Exposed for tests and for stepping a divergence.
ExecStatus step(Execution& execution, const Chunk& chunk, const VmEnv& env);

/// Execute until the script yields, finishes, fails, or exhausts its budget.
ExecStatus run(Execution& execution, const Chunk& chunk, const VmEnv& env,
               std::uint64_t* instructions_executed = nullptr);

/// Human-readable trap, for logs and test failures.
std::string describe(const Trap& trap);

// -- serialisation ---------------------------------------------------------
//
// Little-endian and self-describing, matching the byte conventions the rest of
// the core reads. The point is not compactness; it is that a suspended script
// survives a save and resumes bit-identically, which `deserialize(serialize(x))
// == x` is the honest test of.

void write_value(std::vector<std::byte>& out, const Value& value);
bool read_value(ByteReader& reader, Value& out);

void serialize(const Execution& execution, std::vector<std::byte>& out);
Result<Execution> deserialize(std::span<const std::byte> bytes);

}  // namespace imperivm::core::script
