#pragma once

// Many scripts, one clock.
//
// A running game has hundreds of `.vs` coroutines alive at once: one behaviour
// per object with a `<behavior script=...>` binding, one per command in flight,
// six AI monitors that `DATA\AI\MAIN.VS` starts at the top of every match, and
// whatever those spawn in turn. They all suspend on `Sleep` and wake when game
// time reaches them.
//
// ## Iteration order is world state
//
// Not a style preference -- a correctness requirement. Two scripts that wake on
// the same millisecond and both call `rand` consume the RNG in whichever order
// the scheduler visits them, and a lockstep match desynchronises if two clients
// disagree about that order. So scripts are held in a vector kept sorted by
// script id, ids are issued monotonically, and a pass visits them by
// `(wake time, id)` -- a total order, since ids are unique.
// There is no unordered container anywhere in this file, and there must never
// be one.
//
// ## Spawning
//
// `AIRun("SquadMonitor.vs")` returns an integer handle and `AIBreakScript(id)`
// kills it; 74 call sites do the latter. A spawned script is a peer, not a
// child frame -- it has its own `Execution` and its own place in the wake
// order. A script spawned during a pass is appended and queued at its
// spawner's instant, and because ids only ever increase it runs after
// everything else due then -- so `MAIN.VS`, which is six `AIRun` calls and a
// return, does start its six monitors on the tick that started it,
// deterministically and without recursion.
//
// ## The clock
//
// Game time in milliseconds, held here, advanced by the caller. The core owns
// no clock: `Sleep(1000)` means "when this counter has advanced by 1000", and
// what a millisecond costs in wall time is the platform's business.
//
// ## A script wakes on its own millisecond
//
// `advance` is called once a turn, but a turn is not the scheduler's grain.
// In `gbr.exe` the script VMs are thread managers (base `0x006876f0`, which
// keeps its wheel at `+0x18`) built against the game's timing wheel at
// `+0x1294` (the VM base `0x0069f6b0`, from `0x0053b0dc`, `0x00541e30`,
// `0x005670e5`, `0x0056f69e`, `0x0053d50f` and `0x006b88e9`), and
// `0x00528b40` steps that wheel one millisecond at a time inside the turn
// (`0x006879a0` fires what is due on exactly that millisecond, through slot
// `+0xc` of the owner, which for a VM is the runner `0x0069f7a0`). The runner
// re-arms a script that suspended at the wheel's *current* millisecond plus
// the wait (`0x00688220`), so `Sleep(500)` runs every 500 ms wherever the
// turn boundaries fall, and a wake (`0x0069f8f0`) is armed at the current
// millisecond too.
//
// So a pass over a turn `(from, to]` runs scripts in **wake-time order**,
// each at its own wake time: `now()` reads that instant while it runs, its
// next wait counts from it, and whatever falls due again before `to` runs
// again in the same pass. Ties keep the order they always had, ascending id.
// `now()` is `to` again when the pass ends.
//
// What this clock is *not* is the world's. The world has already been
// advanced to `to` when a pass runs (`GameSession::advance`): every position,
// fight and queue a script reads is the turn's end, and an order it gives
// takes effect from there. Only the scripts' own time -- waits, timeouts,
// `GetTime`, the stamps a script writes and reads back -- is exact.
//
// Two cases run at `to`, not at a wake time inside the pass:
//
//  * a script made runnable outside a pass -- spawned by the session or a
//    system, which act at the world's time, the turn's end; and
//  * a script whose wait ended no later than the instant it ran at (a zero
//    wait). The original re-arms that on the millisecond being swept and
//    runs it again there (`0x00688060` points the sweep at the new node);
//    here it waits for the next pass instead, because the world it waits on
//    moves only between passes and a re-run would spin. **Engine choice.**

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/bytecode.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/vm.hpp"

namespace imperivm::core::script {

inline constexpr std::uint32_t kNoChunk = 0xFFFFFFFFu;

/// One live coroutine.
struct ScriptRecord {
  ScriptId id = kNoScript;
  std::uint32_t chunk_index = kNoChunk;
  /// The object this script is attached to, when it has one. The desync dumps
  /// pair a script with an object id, so this is state, not bookkeeping.
  ObjectRef owner;
  ScriptId parent = kNoScript;
  Execution execution;
  /// Killed, and awaiting compaction at the end of the pass. Killing during a
  /// run must not disturb the vector a pass is walking.
  bool dead = false;
};

struct FailedScript {
  ScriptId id = kNoScript;
  std::string source_name;
  Trap trap;
};

/// What one `Scheduler::call` did.
///
/// `status` is the `Execution`'s at the moment the call gave up on it:
/// `finished`, `failed` (with `trap`), or `suspended` for a script that tried
/// to yield and was abandoned there -- see `call`. `id` is the real script id
/// the call ran under, and `kNoScript` when there was no such chunk.
/// `result` is what a `finished` script returned, nil when it returned
/// nothing; a caller that asks a hook a question (a command row's
/// `onaddremovescript`, `bool f(Obj This, bool bAdd)`) reads its answer here.
struct CallReport {
  ScriptId id = kNoScript;
  ExecStatus status = ExecStatus::failed;
  Trap trap;
  Value result;
};

/// What one `run` did. Counts rather than logs, plus the traps, because a trap
/// is the thing worth reading.
struct RunReport {
  std::uint32_t visited = 0;
  std::uint32_t resumed = 0;
  std::uint32_t completed = 0;
  std::uint32_t failed = 0;
  std::vector<FailedScript> traps;
};

class Scheduler {
 public:
  // -- code library ------------------------------------------------------
  //
  // Chunks are owned here and referred to by index, so a serialised coroutine
  // names its code by a number that a rebuilt library reproduces, and by its
  // source name so that a rebuild that disagrees is caught rather than
  // silently mis-bound.

  /// Takes ownership. The chunk's `source_name` is its lookup key.
  std::uint32_t add_chunk(Chunk chunk);

  /// Case-insensitive, separator-insensitive, and falls back to the basename:
  /// the corpus writes `AIRun('SquadMonitor.vs')` for a file the pack stores as
  /// `DATA\AI\SQUADMONITOR.VS`, while the class XML uses
  /// `data/subai/deer_idle.vs`. All three must find the same chunk.
  ///
  /// **The basename fallback is ambiguous and the ambiguity is real.** `MAIN.VS`
  /// exists in `DATA\AI`, `DATA\AI\DEFENSIVE`, `DATA\AI\CHAOTIC` and
  /// `DATA\AI\DEFAULT`, and which one a bare `AIRun('Main.vs')` means depends on
  /// the player's AI personality. This falls back to the first match in load
  /// order, which is a placeholder: the right answer is a per-player search path
  /// rooted at the active personality directory, and it belongs to whoever wires
  /// the AI up.
  [[nodiscard]] std::uint32_t find_chunk(std::string_view name) const;

  /// `find_chunk` without the basename fallback -- the same normalisation, the
  /// whole path, or `kNoChunk`.
  ///
  /// For the callers that hold a path they resolved themselves and for whom a
  /// near miss is a bug rather than a convenience. `RunSequence` is the first:
  /// every container names its sequence scripts `seq<n>.vs` under its own
  /// `Maps/<n>/Sequences/`, so the fallback would happily hand a conquest map
  /// another map's `seq1.vs` and start the wrong mission script.
  [[nodiscard]] std::uint32_t find_chunk_exact(std::string_view path) const;
  [[nodiscard]] const Chunk& chunk(std::uint32_t index) const { return chunks_[index]; }
  [[nodiscard]] std::size_t chunk_count() const { return chunks_.size(); }

  // -- environment -------------------------------------------------------

  void set_registry(const HostRegistry* registry) { registry_ = registry; }
  void set_host(Host* host) { host_ = host; }
  void set_user(void* user) { user_ = user; }
  void set_instruction_budget(std::uint64_t budget) { instruction_budget_ = budget; }
  [[nodiscard]] const HostRegistry* registry() const { return registry_; }
  [[nodiscard]] Host* host() const { return host_; }

  // -- lifetime ----------------------------------------------------------

  ScriptId spawn(std::uint32_t chunk_index, std::span<const Value> args = {},
                 ObjectRef owner = {}, ScriptId parent = kNoScript);
  /// `kNoScript` when the library holds no such file, which must be survivable:
  /// four `<behavior script=...>` bindings in the shipped class tree name files
  /// that are not in `data.pak` at all.
  ScriptId spawn_by_name(std::string_view name, std::span<const Value> args = {},
                         ObjectRef owner = {}, ScriptId parent = kNoScript);

  /// Called once per script as the scheduler tears it down, whatever ended it:
  /// `AIBreakScript`, a clean return, or a trap.
  ///
  /// The runtime owns coroutines; it does not own the resources a *simulation*
  /// hangs off one. `sim::ObjListPool` keys its entries by script and has no
  /// other way to learn a script is gone, and a pool that never shrinks grows
  /// for the life of the match. A plain function pointer handed the scheduler's
  /// `user` -- the same `HostContext*` every host function receives -- keeps
  /// this file from learning what a world is.
  ///
  /// Fired from `compact`, in ascending id order, exactly once per script, and
  /// never while a pass is walking the vector: a script killed mid-pass is
  /// reported at the end of that pass. Not fired by the destructor, which is
  /// not a teardown a save could ever observe.
  using TeardownHook = void (*)(void* user, ScriptId id);
  void set_teardown_hook(TeardownHook hook) noexcept { teardown_hook_ = hook; }
  [[nodiscard]] TeardownHook teardown_hook() const noexcept { return teardown_hook_; }

  /// Called once at the end of every pass, after teardown and compaction.
  ///
  /// The safe point: every `Execution` is back in its record, no host call is
  /// in flight, and the dead are gone. That is what a simulation needs to walk
  /// the live coroutines' values -- `sim::sweep_objlists` collects pooled
  /// `ObjList`s no live script can still reach, which is the only thing that
  /// bounds a list a host function returned rather than a script declared.
  ///
  /// Handed the scheduler as well as `user`, because a sweep's roots *are* the
  /// coroutines. Const: a pass hook reads the scripts, it does not run them.
  using PassHook = void (*)(void* user, const Scheduler& scheduler);
  void set_pass_hook(PassHook hook) noexcept { pass_hook_ = hook; }
  [[nodiscard]] PassHook pass_hook() const noexcept { return pass_hook_; }

  /// Called after each script's slice, before the next script is resumed.
  ///
  /// The original's runner does exactly this: `0x0069f7a0` runs the bytecode,
  /// clears the "currently running object" global, and then calls the object's
  /// own `vtbl[0x24]` (`0x005a7ac0`) to perform whatever the slice asked for
  /// and could not do to itself while it was running. Today that is one thing,
  /// `Erase` on the running script's own object; the original also defers
  /// `Unit::Mutate` through the same hook.
  ///
  /// A per-*pass* hook is not a substitute. The deferral is over by the time
  /// the next script runs, so a pass hook would let every other coroutine in
  /// the same pass observe an object the original had already destroyed --
  /// and the difference is a divergence that only appears once two scripts
  /// touch the same object in one turn.
  ///
  /// Fired for every script that ran, whether it finished, trapped, suspended
  /// or was killed, and always after its `Execution` has been moved back into
  /// its record. The hook may kill scripts, including the one it is handed.
  /// The diagnostic tap of `VmEnv::call_trace`, with its own user pointer so
  /// a tool can hold state the simulation's `user` does not. Not saved, not
  /// hashed, and not part of any result: a trace that changed the run would
  /// be a second simulation.
  void set_call_trace(CallTrace trace, void* user) noexcept {
    call_trace_ = trace;
    trace_user_ = user;
  }
  [[nodiscard]] CallTrace call_trace() const noexcept { return call_trace_; }
  [[nodiscard]] void* trace_user() const noexcept { return trace_user_; }

  using StepHook = void (*)(void* user, Scheduler& scheduler, ScriptId id);
  void set_step_hook(StepHook hook) noexcept { step_hook_ = hook; }
  [[nodiscard]] StepHook step_hook() const noexcept { return step_hook_; }

  /// `AIBreakScript`. Safe to call on a script that is running right now.
  bool kill(ScriptId id);
  [[nodiscard]] bool alive(ScriptId id) const;
  [[nodiscard]] const ScriptRecord* find(ScriptId id) const;
  [[nodiscard]] ScriptRecord* find(ScriptId id);
  [[nodiscard]] const std::vector<ScriptRecord>& scripts() const { return scripts_; }
  [[nodiscard]] std::size_t live_count() const;

  // -- synchronous calls -------------------------------------------------

  /// The instruction budget a synchronous call runs under. `0x0fffffff`,
  /// which is the literal `gbr.exe` hands its interpreter (0x0069d4c0) from the
  /// synchronous launcher 0x0069dca0; the scheduler's own slice passes 15000.
  /// Not a limit anything shipped comes near: it exists so a runaway hook
  /// traps instead of hanging the simulation.
  static constexpr std::uint64_t kCallBudget = 0x0FFFFFFFu;

  /// Run `chunk_index` to completion, **now**, inside the caller, and discard
  /// it. What the original does with a class hook.
  ///
  /// `0x006a0360` -- which the `ondie`, `onkill` and `onenter` launchers all
  /// reach -- finds the chunk and calls 0x0069dca0, which allocates a context,
  /// binds the arguments, runs the interpreter with `kCallBudget`, and deletes
  /// the context before it returns. The interpreter saves its current-context
  /// globals on entry and restores them on exit (0x0069d4e1 / 0x0069da67),
  /// which is what lets it nest inside a host call of a script that is itself
  /// running. So a hook is **not** a coroutine: it has run to its end before
  /// the routine that fired it takes its next step.
  ///
  /// **A real id.** Taken from the same monotonic counter `spawn` uses, so the
  /// ids issued around a hook are the ones the spawned hook used to consume,
  /// and so that everything a host function keys by script -- the `ObjList`
  /// pool first -- has a key. The record is never in `scripts()`: the call is
  /// not a peer of anything, is never resumed by a pass and is never saved.
  /// The teardown hook fires for it once, before this returns, exactly as
  /// `compact` would have fired it; without that, `MILITARY_ONDIE.VS`'s pooled
  /// `ObjList` would outlive the only script that could reach it.
  ///
  /// **A yield abandons the script where it stands.** INFERRED from the
  /// disassembly rather than observed running: a host call answering "suspend"
  /// makes the interpreter leave its loop and return, and 0x0069dca0 deletes
  /// the context whatever the interpreter returned -- so the statements after
  /// a `Sleep` in a hook never run. None of the ten shipped hook scripts
  /// sleeps, waits or loops on one, so nothing shipped can tell this from any
  /// other choice; it is the original's structure, reproduced. The report
  /// says `suspended` so a caller can see it happened.
  ///
  /// Safe to call from inside a running script's host call. The outer
  /// script's `Execution` is detached from its record for the length of its
  /// run and this touches no record but by appending, which is what a `spawn`
  /// from a host call already does. `chunks_` is a deque so that a chunk
  /// compiled on demand from inside the nested run moves nothing a running
  /// interpreter holds a reference to.
  CallReport call(std::uint32_t chunk_index, std::span<const Value> args = {});

  // -- the clock ---------------------------------------------------------

  /// The scripts' clock. Between passes, the end of the last one; while a
  /// script runs, the instant it woke at. See "A script wakes on its own
  /// millisecond" above.
  [[nodiscard]] std::int64_t now() const { return now_; }
  void set_now(std::int64_t milliseconds) { now_ = milliseconds; }

  /// Advance game time by `delta` milliseconds and run everything that falls
  /// due on the way, each at its own wake time, in `(wake time, id)` order.
  ///
  /// A script spawned during the pass runs at its spawner's instant, after
  /// whatever else is due then. A script that sleeps runs again in the same
  /// pass if it is due by the end. A zero wait, and anything runnable before
  /// the pass began, runs at the end.
  RunReport advance(std::int64_t delta);

  /// Run everything runnable at the current time, in id order, without moving
  /// the clock: `advance(0)`.
  ///
  /// Each script is resumed at most once per call, so a script that sleeps for
  /// zero milliseconds yields the rest of the tick rather than spinning the
  /// scheduler. Scripts spawned during the pass do run in it.
  RunReport run_ready();

  // -- serialisation -----------------------------------------------------
  //
  // Coroutines, the clock and the id counter -- everything that is world
  // state. The chunk library is not: it is derived from `data.pak` and is
  // rebuilt identically on load, so records carry their source name and are
  // re-bound against the rebuilt library, and a mismatch is an error rather
  // than a wrong script resuming on the wrong object.

  void serialize(std::vector<std::byte>& out) const;
  Status deserialize(std::span<const std::byte> bytes);

 private:
  /// One pass over `(from, to]`. See `advance`.
  RunReport run_window(std::int64_t from, std::int64_t to);
  void compact();
  [[nodiscard]] VmEnv env_for(const ScriptRecord& record) const;

  /// A deque, not a vector: a chunk compiled on demand -- `RunAIHelper`, or a
  /// class hook fired from inside a host call -- is appended while a running
  /// interpreter holds a reference to another chunk, and a vector's
  /// reallocation would pull it out from under it. See `call`.
  std::deque<Chunk> chunks_;
  /// Sorted by id, ascending, because ids are issued in order and spawns
  /// append. Iteration is therefore stable without sorting anything.
  std::vector<ScriptRecord> scripts_;
  ScriptId next_id_ = 1;
  std::int64_t now_ = 0;

  const HostRegistry* registry_ = nullptr;
  Host* host_ = nullptr;
  void* user_ = nullptr;
  TeardownHook teardown_hook_ = nullptr;
  PassHook pass_hook_ = nullptr;
  StepHook step_hook_ = nullptr;
  CallTrace call_trace_ = nullptr;
  void* trace_user_ = nullptr;
  std::uint64_t instruction_budget_ = 1u << 20;
};

/// Every `.vs` path a serialised scheduler section names, in record order.
///
/// **The load has to compile before it can bind, and only the save knows what
/// to compile.** `deserialize` rebinds each coroutine by source name against
/// the library it is handed and refuses the whole load with `not_found` rather
/// than resume the wrong script -- so whatever built that library has to have
/// put every one of those files in it first. Enumerating the ways a script can
/// start is how that was done and it is not a closed set: the class `<method>`
/// a command launches was missed for as long as `CommandSystem` has had a
/// library to compile through, and `Balcans` and the conquest failed every save
/// round trip on four wildlife verbs -- `deer_move`, `eagle_move`, `lion_lead`,
/// `wolf_lead` -- that no manifest anywhere names.
///
/// This is the list that cannot be wrong, because it is the one the writer
/// wrote. It parses the header and the per-record prefixes and skips every
/// `Execution` payload, so it is cheap and it does not care what a record
/// contains.
///
/// Duplicates are kept: two coroutines of one file name it twice, and a caller
/// compiling by path memoises anyway.
[[nodiscard]] Result<std::vector<std::string>> script_section_sources(
    std::span<const std::byte> bytes);

/// Register the concurrency primitives: `Sleep`, `AIRun` in all its shapes,
/// `AIBreakScript`, `StartPlayerScript`.
///
/// These live with the scheduler rather than with the rest of the host surface
/// because they are the only entry points implemented *by the runtime itself* --
/// everything else in the 705-entry inventory is Part 5's simulation.
void register_scheduler_builtins(HostRegistry& registry);

}  // namespace imperivm::core::script
