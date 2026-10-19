#include "imperivm/core/script/scheduler.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <tuple>
#include <utility>

namespace imperivm::core::script {
namespace {

constexpr std::uint32_t kSaveMagic = 0x44484353u;  // "SCHD"
// 2: each record's arming order, and the player-script slots. 3: each record's
// context string (`ScriptRecord::context`).
constexpr std::uint32_t kSaveVersion = 3;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

/// `AIRun('SquadMonitor.vs')`, `data/subai/deer_idle.vs` and the pack's
/// `DATA\AI\SQUADMONITOR.VS` all have to name the same file.
std::string normalise(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (const char c : name) out.push_back(c == '\\' ? '/' : lower(c));
  return out;
}

std::string_view basename(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

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

// -- the concurrency primitives --------------------------------------------
//
// The only host entry points the runtime implements itself. Everything else in
// the 705-entry inventory is simulation, and belongs to Part 5.

/// `Sleep(ms)` -- 1,350 call sites, and the reason the whole VM is built the
/// way it is: this returns, and the script continues thousands of ticks later.
///
/// 0x006945e0 is a two-phase call. The compiler emits it with a flag byte
/// above the argument, and the interpreter's suspending-call opcode (`0x9e` in
/// 0x0069d4c0) sets that flag the first time it reaches the call and clears it
/// when it reaches the same call again. With the flag set the body stores the
/// argument into the wait the runner handed the interpreter (0x00a77eac points
/// at it for the slice) and answers 1, *suspend*, leaving both on the stack;
/// the runner (0x0069f7a0) then re-arms the thread at the wheel's clock plus
/// that wait (0x00688220). On the resumed pass the flag is clear, the body pops
/// both and answers 0, and the script carries on. So `Sleep(ms)` is one
/// suspension of exactly `ms`, counted from the instant the script ran at --
/// which is `HostOutcome::sleep_for` and `Execution::wake_time` here. There is
/// no clamp and no validation of the argument.
///
/// One gap, in the interpreter rather than here: a slice that ends without
/// suspending -- the original yields after 15,000 instructions outside an
/// atomic section (0x0069d4c0's first argument), re-arming the thread after
/// its own default interval (`[context+8]`, 100 for an `AIRun` thread) --
/// traps here after `instruction_budget` instead. No shipped script runs that
/// long without a wait; the trap sweep would say so.
HostOutcome builtin_sleep(CallContext& context) {
  if (context.count() != 1 || !context.arg(0).is_integer()) {
    return HostOutcome::failed("Sleep expects one integer of milliseconds");
  }
  return HostOutcome::sleep_for(context.arg(0).as_integer());
}

/// `Run(name[, i1 .. i5])` and `set.Run(name)` -- **a call, not a spawn**.
///
/// The six free bodies (0x004c6e30 for the name alone, then 0x004c6f00,
/// 0x004c6fe0, 0x004c70c0, 0x004c71b0 and 0x004c72c0 for one to five integers)
/// and `Settlement::Run` (0x004c81e0) are one shape: compile the named file
/// against a fixed signature (0x006a2110 -- `void`, `void, int i1`, ...,
/// `void, Settlement set`) and, when that succeeds, run it to its end inside
/// the caller through 0x006a0360, the synchronous launcher member `AIRun` and
/// the class hooks use, with the integers (or the settlement) bound as its
/// parameters. A file that does not compile prints "Error compiling %s: %s"
/// and the caller carries on. Nothing is returned. The one shipped site is
/// `DBGRUNSCRIPT.VS`'s `Run("data/subai/" + choice)`; this used to spawn a
/// peer, so the script it ran started late and ran beside its caller.
///
/// The name goes through the library's resolution, which folds case and
/// separators and falls back to the leaf name; the original compiles the path
/// it is given.
HostOutcome run_synchronously(CallContext& context, const Value& name,
                              std::span<const Value> args) {
  if (context.scheduler == nullptr) return HostOutcome::failed("Run needs a scheduler");
  if (!name.is_string()) return HostOutcome::failed("Run expects a script name");
  Scheduler& scheduler = *context.scheduler;
  const std::uint32_t chunk = scheduler.find_chunk(name.as_string());
  if (chunk == kNoChunk) return HostOutcome::ok_void();
  const CallReport ran = scheduler.call(chunk, args);
  if (ran.status == ExecStatus::failed) {
    scheduler.report_trap(FailedScript{ran.id, scheduler.chunk(chunk).source_name, ran.trap});
  }
  return HostOutcome::ok_void();
}

HostOutcome builtin_run_free(CallContext& context) {
  if (context.count() < 1) return HostOutcome::failed("Run expects a script name");
  return run_synchronously(context, context.arg(0), context.arguments.subspan(1));
}

/// `set.Run(name)`: the settlement is the script's one parameter.
HostOutcome builtin_run_settlement(CallContext& context) {
  if (context.count() < 2) return HostOutcome::failed("Run expects a script name");
  return run_synchronously(context, context.arg(1), context.arguments.subspan(0, 1));
}

/// `set.AIRun('ESH_NeedTech.vs')`, `gaika.AIRun('GSH_SynchApproach.vs',
/// AIPlayer)`, `sq.AIRun(...)` -- 21 sites, and **a call, not a spawn.**
///
/// `Settlement::AIRun` (0x0043df70), `GAIKA::AIRun` (0x0043e0b0) and
/// `Squad::AIRun` (0x0043e190) all resolve the script and hand it to
/// 0x006a0360, the synchronous launcher the class hooks use (0x0069dca0): the
/// helper runs to its end inside the caller's host call, and the caller's next
/// statement sees what it wrote. The scripts are written that way --
/// `ES_STRONGHOLD.VS` reads `nBuildCount` and `nBuildType` out of the
/// environment on the line after `set.AIRun("ESH_BuildArmy.vs")`, and
/// `ESH_BUILDARMY.VS` reads `UnitsEnabled` on the line after its own
/// `set.AIRun("ESH_EnabledUnits.vs")`. None of the fourteen helpers so called
/// sleeps or waits. The free `AIRun` is the other kind: it starts a thread
/// and answers its handle (0x0041cd00).
///
/// This used to spawn a peer, so every caller read what the helper had
/// written the *last* time it ran, and acted on it. While every script ran
/// once a turn at the turn's end the lag was one loop of the caller; once
/// scripts wake on their own millisecond it is a stale answer about another
/// moment, and the AI's strongholds stopped raising armies.
///
/// The receiver binds as the helper's first parameter, which is what the
/// signature comments of the scripts so called expect. A helper that traps is
/// reported with the pass that called it (`Scheduler::report_trap`); its
/// caller carries on, as the original's does once the interpreter returns.
HostOutcome builtin_airun_member(CallContext& context) {
  if (context.count() < 2) return HostOutcome::failed("AIRun expects a script name");
  if (context.scheduler == nullptr) return HostOutcome::failed("AIRun needs a scheduler");
  if (!context.arg(1).is_string()) return HostOutcome::failed("AIRun expects a script name");
  std::vector<Value> args;
  args.reserve(context.count() - 1);
  args.push_back(context.arg(0));
  for (std::size_t i = 2; i < context.count(); ++i) args.push_back(context.arg(i));
  Scheduler& scheduler = *context.scheduler;
  const std::uint32_t chunk = scheduler.find_chunk(context.arg(1).as_string());
  // A missing script is survivable, as the spawning forms' is.
  if (chunk == kNoChunk) return HostOutcome::ok_void();
  const CallReport ran = scheduler.call(chunk, args);
  if (ran.status == ExecStatus::failed) {
    scheduler.report_trap(FailedScript{ran.id, scheduler.chunk(chunk).source_name, ran.trap});
  }
  return HostOutcome::ok_void();
}

/// `StartPlayerScript(player, name)` -- 2 sites, `ONFINISH_RESEARCH.VS`'s two
/// wisdom researches. 0x0056f970 pops the name and the player and hands
/// `player - 1` and the name to 0x0056f720; `Scheduler::start_player_script`
/// describes that and is it.
HostOutcome builtin_start_player_script(CallContext& context) {
  if (context.count() != 2) return HostOutcome::failed("StartPlayerScript expects a player and a "
                                                      "script name");
  if (context.scheduler == nullptr) return HostOutcome::failed("StartPlayerScript needs a scheduler");
  if (!context.arg(0).is_integer() || !context.arg(1).is_string()) {
    return HostOutcome::failed("StartPlayerScript expects a player and a script name");
  }
  (void)context.scheduler->start_player_script(context.arg(0).as_integer(),
                                               context.arg(1).as_string(), context.arg(0));
  return HostOutcome::ok_void();
}

}  // namespace

// -- library ----------------------------------------------------------------

std::uint32_t Scheduler::add_chunk(Chunk chunk) {
  chunks_.push_back(std::move(chunk));
  return static_cast<std::uint32_t>(chunks_.size() - 1);
}

std::uint32_t Scheduler::find_chunk_exact(std::string_view path) const {
  const std::string wanted = normalise(path);
  for (std::size_t i = 0; i < chunks_.size(); ++i) {
    if (normalise(chunks_[i].source_name) == wanted) return static_cast<std::uint32_t>(i);
  }
  return kNoChunk;
}

std::uint32_t Scheduler::find_chunk(std::string_view name) const {
  const std::string wanted = normalise(name);
  for (std::size_t i = 0; i < chunks_.size(); ++i) {
    if (normalise(chunks_[i].source_name) == wanted) return static_cast<std::uint32_t>(i);
  }
  const std::string_view wanted_leaf = basename(wanted);
  for (std::size_t i = 0; i < chunks_.size(); ++i) {
    if (basename(normalise(chunks_[i].source_name)) == wanted_leaf) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return kNoChunk;
}

// -- lifetime ---------------------------------------------------------------

ScriptId Scheduler::spawn(std::uint32_t chunk_index, std::span<const Value> args,
                          ObjectRef owner, ScriptId parent) {
  if (chunk_index >= chunks_.size()) return kNoScript;
  ScriptRecord record;
  record.id = next_id_++;
  record.chunk_index = chunk_index;
  record.owner = owner;
  record.parent = parent;
  record.execution = start(chunks_[chunk_index], args, host_);
  record.execution.chunk_index = chunk_index;
  // Armed at its spawn: 0x00688240 puts a new thread on the wheel at once.
  record.armed = next_arm_++;
  // Ids are issued in order and this only ever appends, so the vector stays
  // sorted by id without anyone sorting it. That is the whole of the
  // deterministic iteration order.
  scripts_.push_back(std::move(record));
  return scripts_.back().id;
}

ScriptId Scheduler::spawn_by_name(std::string_view name, std::span<const Value> args,
                                  ObjectRef owner, ScriptId parent) {
  const std::uint32_t index = find_chunk(name);
  if (index == kNoChunk) return kNoScript;
  return spawn(index, args, owner, parent);
}

ScriptId Scheduler::start_player_script(std::int32_t player, std::string_view name,
                                        const Value& argument) {
  constexpr std::size_t kSlotsPerPlayer = 8;
  std::size_t taken = 0;
  for (const PlayerScriptSlot& slot : player_scripts_) {
    if (slot.player != player) continue;
    if (slot.name == name) return kNoScript;  // "for the second time"
    ++taken;
  }
  if (taken >= kSlotsPerPlayer) return kNoScript;  // "No free slot"
  const std::uint32_t index = find_chunk(name);
  if (index == kNoChunk) return kNoScript;  // "Error compiling player script"
  player_scripts_.push_back(PlayerScriptSlot{player, std::string(name)});
  return spawn(index, std::span<const Value>(&argument, 1));
}

const ScriptRecord* Scheduler::find(ScriptId id) const {
  const auto at = std::lower_bound(scripts_.begin(), scripts_.end(), id,
                                   [](const ScriptRecord& record, ScriptId target) {
                                     return record.id < target;
                                   });
  if (at == scripts_.end() || at->id != id) return nullptr;
  return &*at;
}

ScriptRecord* Scheduler::find(ScriptId id) {
  return const_cast<ScriptRecord*>(static_cast<const Scheduler*>(this)->find(id));
}

bool Scheduler::kill(ScriptId id) {
  ScriptRecord* record = find(id);
  if (record == nullptr || record->dead) return false;
  // Marked, not erased: `AIBreakScript` is called from inside a running script,
  // and a pass must not have the vector pulled out from under it.
  record->dead = true;
  return true;
}

bool Scheduler::alive(ScriptId id) const {
  const ScriptRecord* record = find(id);
  return record != nullptr && !record->dead;
}

bool Scheduler::set_context(ScriptId id, std::string_view context) {
  ScriptRecord* record = find(id);
  if (record == nullptr || record->dead) return false;
  record->context.assign(context);
  return true;
}

std::string_view Scheduler::context(ScriptId id) const {
  const ScriptRecord* record = find(id);
  if (record == nullptr || record->dead) return {};
  return record->context;
}

std::size_t Scheduler::live_count() const {
  std::size_t count = 0;
  for (const ScriptRecord& record : scripts_) {
    if (!record.dead) ++count;
  }
  return count;
}

void Scheduler::compact() {
  // The one place a script actually stops existing, whatever ended it --
  // `AIBreakScript`, a return, or a trap -- so it is the one place the teardown
  // hook can fire exactly once per script. Ascending id order, because
  // `scripts_` is kept sorted by id and this is an index walk: a simulation
  // that frees per-script state here must free it in the same order on every
  // peer or the freeing itself becomes a divergence.
  //
  // Before the erase, so the hook can still see the record it is being told
  // about. Skipped entirely when nothing is dead, which is the common pass.
  if (teardown_hook_ != nullptr) {
    for (const ScriptRecord& record : scripts_) {
      if (record.dead) teardown_hook_(user_, record.id);
    }
  }
  scripts_.erase(std::remove_if(scripts_.begin(), scripts_.end(),
                                [](const ScriptRecord& record) { return record.dead; }),
                 scripts_.end());
}

// -- running ----------------------------------------------------------------

VmEnv Scheduler::env_for(const ScriptRecord& record) const {
  VmEnv env;
  env.registry = registry_;
  env.host = host_;
  env.scheduler = const_cast<Scheduler*>(this);
  env.user = user_;
  env.script = record.id;
  env.now = now_;
  env.instruction_budget = instruction_budget_;
  env.call_trace = call_trace_;
  env.trace_user = trace_user_;
  return env;
}

RunReport Scheduler::run_ready() { return run_window(now_, now_); }

RunReport Scheduler::run_window(std::int64_t from, std::int64_t to) {
  RunReport report;

  // The pass's queue, by `(instant, armed)` -- the wheel's order: instants
  // ascending, and within one the order the scripts were armed (see "Ties run
  // in the order the scripts were armed"). Transient: it is empty before and
  // after every pass, so it is never saved -- between passes each script's
  // wake time and `armed` are the whole of its place in it.
  using Due = std::tuple<std::int64_t, std::uint64_t, ScriptId>;
  std::priority_queue<Due, std::vector<Due>, std::greater<Due>> due;

  // What is runnable as the pass opens. Anything that became so outside a
  // pass, or whose wait ended at or before the last pass's end (a zero
  // wait), runs at the end: see "A script wakes on its own millisecond".
  for (const ScriptRecord& record : scripts_) {
    if (record.dead) continue;
    const ExecStatus status = record.execution.status;
    if (status == ExecStatus::ready) {
      due.emplace(to, record.armed, record.id);
    } else if (status == ExecStatus::suspended && record.execution.wake_time <= to) {
      due.emplace(record.execution.wake_time > from ? record.execution.wake_time : to, record.armed,
                  record.id);
    }
  }

  while (!due.empty()) {
    const auto [instant, armed, id] = due.top();
    due.pop();
    ScriptRecord* queued = find(id);
    if (queued == nullptr || queued->dead) continue;
    if (queued->chunk_index >= chunks_.size()) continue;
    now_ = instant;

    // A script may spawn others while it runs, and the vector grows at its
    // tail -- new ids are always the highest. Whatever was appended by the
    // end of the slice is queued at this instant, so it runs after
    // everything else due now. `DATA\AI\MAIN.VS` is six `AIRun` calls and a
    // return, and this is what makes its six monitors start on the tick that
    // started it.
    const std::size_t known = scripts_.size();
    const std::uint32_t chunk_index = queued->chunk_index;
    // A copy: `env.context` borrows it for the run, and the record it came
    // from can move while the script spawns.
    const std::string context = queued->context;
    VmEnv env = env_for(*queued);
    env.context = context;

    // Detached for the duration of the run. A host call can spawn, which
    // reallocates `scripts_`, and a reference held across that would dangle.
    Execution execution = std::move(queued->execution);
    execution.status = ExecStatus::ready;
    ++report.resumed;
    run(execution, chunks_[chunk_index], env);

    ScriptRecord* record = find(id);
    if (record != nullptr) {
      if (execution.status == ExecStatus::finished) {
        ++report.completed;
        record->dead = true;
      } else if (execution.status == ExecStatus::failed) {
        ++report.failed;
        report.traps.push_back(
            FailedScript{id, chunks_[chunk_index].source_name, execution.trap});
        record->dead = true;
      }
      record->execution = std::move(execution);
      // Re-armed as the slice ends, if it suspended (0x0069f7a0 -> 0x00688220):
      // behind everything armed before it, including whatever it spawned.
      if (!record->dead && record->execution.status == ExecStatus::suspended) {
        record->armed = next_arm_++;
      }
      // The slice is over and the record is whole again. See `StepHook`: this
      // is where the original performs a deferred `Erase`, and the reason it is
      // here rather than in the pass hook is that the next script must not see
      // an object the previous one destroyed.
      if (step_hook_ != nullptr) step_hook_(user_, *this, id);
    }

    // Due again inside this pass: a wait that ends after the instant it began
    // and no later than the pass's end. Looked up again, because the step
    // hook may have killed it.
    if (const ScriptRecord* again = find(id);
        again != nullptr && !again->dead && again->execution.status == ExecStatus::suspended &&
        again->execution.wake_time > instant && again->execution.wake_time <= to) {
      due.emplace(again->execution.wake_time, again->armed, id);
    }
    for (std::size_t i = known; i < scripts_.size(); ++i) {
      if (!scripts_[i].dead && scripts_[i].execution.status == ExecStatus::ready) {
        due.emplace(instant, scripts_[i].armed, scripts_[i].id);
      }
    }
  }

  now_ = to;
  // The synchronous calls that trapped during the pass (`report_trap`).
  for (FailedScript& trap : call_traps_) {
    ++report.failed;
    report.traps.push_back(std::move(trap));
  }
  call_traps_.clear();
  report.visited = static_cast<std::uint32_t>(scripts_.size());
  compact();
  // After compaction, so the hook never sees a record that is on its way out,
  // and after every `Execution` has been moved back into its record, so a hook
  // that walks frames sees all of them. See `PassHook`.
  if (pass_hook_ != nullptr) pass_hook_(user_, *this);
  return report;
}

CallReport Scheduler::call(std::uint32_t chunk_index, std::span<const Value> args,
                           std::string_view context) {
  CallReport report;
  if (chunk_index >= chunks_.size()) return report;
  // The same counter `spawn` draws from, so a hook consumes the id it always
  // did and a host function that keys by script has one to key by.
  report.id = next_id_++;

  Execution execution = start(chunks_[chunk_index], args, host_);
  execution.chunk_index = chunk_index;
  VmEnv env;
  env.registry = registry_;
  env.host = host_;
  env.scheduler = this;
  env.user = user_;
  env.script = report.id;
  env.context = context;
  env.now = now_;
  env.instruction_budget = kCallBudget;
  env.call_trace = call_trace_;
  env.trace_user = trace_user_;
  // A deque element: a chunk appended during the run does not move this one.
  run(execution, chunks_[chunk_index], env);

  report.status = execution.status;
  if (execution.status == ExecStatus::failed) report.trap = execution.trap;
  if (execution.status == ExecStatus::finished) report.result = execution.result;
  // Discarded whatever the status, and a suspension with it: the original
  // deletes the context the moment the interpreter returns. The teardown fires
  // here, once, for the one id this call ever had.
  if (teardown_hook_ != nullptr) teardown_hook_(user_, report.id);
  return report;
}

RunReport Scheduler::advance(std::int64_t delta) {
  return run_window(now_, now_ + delta);
}

// -- serialisation ----------------------------------------------------------

void Scheduler::serialize(std::vector<std::byte>& out) const {
  put_u32(out, kSaveMagic);
  put_u32(out, kSaveVersion);
  put_i64(out, now_);
  put_u32(out, next_id_);
  put_i64(out, static_cast<std::int64_t>(next_arm_));
  put_u32(out, static_cast<std::uint32_t>(player_scripts_.size()));
  for (const PlayerScriptSlot& slot : player_scripts_) {
    put_u32(out, static_cast<std::uint32_t>(slot.player));
    put_string(out, slot.name);
  }

  std::uint32_t live = 0;
  for (const ScriptRecord& record : scripts_) {
    if (!record.dead) ++live;
  }
  put_u32(out, live);

  for (const ScriptRecord& record : scripts_) {
    if (record.dead) continue;
    put_u32(out, record.id);
    // By name, not by index: the library is rebuilt from `data.pak` on load and
    // a rebuild that disagrees must be caught, not silently resumed against the
    // wrong script.
    put_string(out, record.chunk_index < chunks_.size() ? chunks_[record.chunk_index].source_name
                                                        : std::string());
    put_u16(out, record.owner.type);
    put_u32(out, record.owner.id);
    put_u32(out, record.parent);
    put_i64(out, static_cast<std::int64_t>(record.armed));
    put_string(out, record.context);

    std::vector<std::byte> execution;
    script::serialize(record.execution, execution);
    put_u32(out, static_cast<std::uint32_t>(execution.size()));
    out.insert(out.end(), execution.begin(), execution.end());
  }
}

namespace {

/// The section header, read the one way. Two readers walk this section --
/// `deserialize` and `script_section_sources` -- and a header they parsed
/// separately would be two chances to disagree about a format one of them
/// writes.
[[nodiscard]] Status read_section_header(
    ByteReader& reader, std::int64_t& now, std::uint32_t& next_id, std::int64_t& next_arm,
    std::vector<std::pair<std::int32_t, std::string>>& player_scripts, std::uint32_t& count) {
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kSaveMagic) return FormatError::bad_magic;
  if (version != kSaveVersion) return FormatError::unsupported;
  std::uint32_t slots = 0;
  if (!get_i64(reader, now) || !reader.u32(next_id) || !get_i64(reader, next_arm) ||
      !reader.u32(slots)) {
    return FormatError::truncated;
  }
  // Not reserved from `slots`: a hostile length must not allocate before the
  // read that refuses it.
  for (std::uint32_t i = 0; i < slots; ++i) {
    std::uint32_t player = 0;
    std::string name;
    if (!reader.u32(player) || !get_string(reader, name)) return FormatError::truncated;
    player_scripts.emplace_back(static_cast<std::int32_t>(player), std::move(name));
  }
  if (!reader.u32(count)) return FormatError::truncated;
  return Status{};
}

/// One record's fixed prefix and its length-prefixed `Execution` payload. The
/// payload comes back as bytes rather than as an `Execution`: the caller that
/// wants one decodes it, and the caller that only wants the name skips it
/// without having to understand a format that changes underneath it.
[[nodiscard]] Status read_record_prefix(ByteReader& reader, ScriptId& id, std::string& source_name,
                                        ObjectRef& owner, ScriptId& parent, std::uint64_t& armed,
                                        std::string& context,
                                        std::span<const std::byte>& payload) {
  std::uint16_t owner_type = 0;
  std::uint32_t owner_id = 0;
  std::int64_t armed_bits = 0;
  if (!reader.u32(id) || !get_string(reader, source_name) || !reader.u16(owner_type) ||
      !reader.u32(owner_id) || !reader.u32(parent) || !get_i64(reader, armed_bits) ||
      !get_string(reader, context)) {
    return FormatError::truncated;
  }
  armed = static_cast<std::uint64_t>(armed_bits);
  owner = ObjectRef{owner_type, owner_id};
  std::uint32_t length = 0;
  if (!reader.u32(length) || !reader.bytes(length, payload)) return FormatError::truncated;
  return Status{};
}

}  // namespace

Result<std::vector<std::string>> script_section_sources(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  std::int64_t now = 0;
  std::uint32_t next_id = 0;
  std::int64_t next_arm = 0;
  std::vector<std::pair<std::int32_t, std::string>> player_scripts;
  std::uint32_t count = 0;
  if (const Status header =
          read_section_header(reader, now, next_id, next_arm, player_scripts, count);
      !header.ok()) {
    return header.error();
  }

  std::vector<std::string> names;
  names.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ScriptId id = kNoScript;
    std::string source_name;
    ObjectRef owner;
    ScriptId parent = kNoScript;
    std::uint64_t armed = 0;
    std::string context;
    std::span<const std::byte> payload;
    if (const Status record =
            read_record_prefix(reader, id, source_name, owner, parent, armed, context, payload);
        !record.ok()) {
      return record.error();
    }
    names.push_back(std::move(source_name));
  }
  return names;
}

Status Scheduler::deserialize(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  std::int64_t now = 0;
  std::uint32_t next_id = 0;
  std::int64_t next_arm = 0;
  std::vector<std::pair<std::int32_t, std::string>> player_scripts;
  std::uint32_t count = 0;
  if (const Status header =
          read_section_header(reader, now, next_id, next_arm, player_scripts, count);
      !header.ok()) {
    return header;
  }

  std::vector<ScriptRecord> restored;
  restored.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ScriptRecord record;
    std::string source_name;
    std::span<const std::byte> payload;
    if (const Status prefix = read_record_prefix(reader, record.id, source_name, record.owner,
                                                 record.parent, record.armed, record.context,
                                                 payload);
        !prefix.ok()) {
      return prefix;
    }

    // By name, against the library this scheduler was handed. A miss is the
    // whole load refused: `GameSession::prime_library` is what fills that
    // library, and it reads the same names out of this same section first, so
    // a miss here means the two disagree rather than that a file is optional.
    record.chunk_index = find_chunk(source_name);
    if (record.chunk_index == kNoChunk) return FormatError::not_found;

    Result<Execution> execution = script::deserialize(payload);
    if (!execution.ok()) return execution.error();
    record.execution = std::move(execution.value());
    record.execution.chunk_index = record.chunk_index;
    restored.push_back(std::move(record));
  }

  // Ids were written in order, but a corrupt or hand-edited save must not put
  // the simulation into an order-dependent state, so this is checked.
  for (std::size_t i = 1; i < restored.size(); ++i) {
    if (restored[i - 1].id >= restored[i].id) return FormatError::malformed;
  }

  scripts_ = std::move(restored);
  now_ = now;
  next_id_ = next_id;
  next_arm_ = static_cast<std::uint64_t>(next_arm);
  player_scripts_.clear();
  for (auto& [player, name] : player_scripts) {
    player_scripts_.push_back(PlayerScriptSlot{player, std::move(name)});
  }
  return Status();
}

/// `KillScript()` -- 13 sites in 8 map scripts, and **two instructions**.
///
/// `gbr.exe` 0x0061e730 is `mov eax, 2; ret`. The host-call return value is a
/// status the interpreter switches on -- 0 carry on, 1 suspend -- and 2 is
/// *stop this coroutine*. There is no argument and no receiver: the script it
/// kills is always the one calling it.
///
/// `Scheduler::kill` is documented safe on a script that is running right now,
/// which is the only case this has; the pass that is walking the vector marks
/// the record dead and drops it at the end of the pass rather than erasing
/// under itself.
///
/// **The call does not return to the script**, so what this answers matters
/// only to the VM's own bookkeeping. `ok_void` is what every other terminal
/// builtin answers.
///
/// Its eight scripts are all mission sequences using it as an early `return`
/// from a nested loop -- `4_Great_Battles_Egypt` map 4 alone has seven.
HostOutcome builtin_kill_script(CallContext& context) {
  if (context.scheduler == nullptr) return HostOutcome::failed("KillScript needs a scheduler");
  context.scheduler->kill(context.script);
  return HostOutcome::ok_void();
}

void register_scheduler_builtins(HostRegistry& registry) {
  registry.define(CallKind::free_function, "Sleep", 1, &builtin_sleep);
  // The free `AIRun` and `AIBreakScript` are the AI's launcher and live with
  // the AI (`sim/ai.cpp`): which thread manager they act in is the calling
  // thread's seat, and the handle is an id in that manager.
  // Member `AIRun`: the receiver is argument zero, so arity 1 is
  // `set.AIRun("x.vs")` and arity 2 adds one bound parameter.
  for (std::uint16_t arity = 1; arity <= 5; ++arity) {
    registry.define(CallKind::member, "AIRun", arity, &builtin_airun_member);
  }
  // `Run` with the name and up to five integers, and `set.Run(name)`. Member
  // `Run/0` is `Conversation::Run`, in sim/conversation.cpp.
  for (std::uint16_t arity = 1; arity <= 6; ++arity) {
    registry.define(CallKind::free_function, "Run", arity, &builtin_run_free);
  }
  registry.define(CallKind::member, "Run", 1, &builtin_run_settlement);
  registry.define(CallKind::free_function, "KillScript", 0, &builtin_kill_script);
  registry.define(CallKind::free_function, "StartPlayerScript", 2,
                  &builtin_start_player_script);
}

}  // namespace imperivm::core::script
