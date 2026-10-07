#include "imperivm/core/script/scheduler.hpp"

#include <algorithm>
#include <utility>

namespace imperivm::core::script {
namespace {

constexpr std::uint32_t kSaveMagic = 0x44484353u;  // "SCHD"
constexpr std::uint32_t kSaveVersion = 1;

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

HostOutcome builtin_sleep(CallContext& context) {
  if (context.count() != 1 || !context.arg(0).is_integer()) {
    return HostOutcome::failed("Sleep expects one integer of milliseconds");
  }
  // 652 call sites in 236 files, and the reason the whole VM is built the way
  // it is: this returns, and the script continues thousands of ticks later.
  return HostOutcome::sleep_for(context.arg(0).as_integer());
}

/// Shared by every shape of `AIRun`: a script name, then the arguments to bind.
HostOutcome spawn_from(CallContext& context, const Value& name, std::span<const Value> args) {
  if (context.scheduler == nullptr) return HostOutcome::failed("AIRun needs a scheduler");
  if (!name.is_string()) return HostOutcome::failed("AIRun expects a script name");
  const ScriptId id = context.scheduler->spawn_by_name(name.as_string(), args, ObjectRef{},
                                                       context.script);
  // A missing script is survivable: four `<behavior script=...>` bindings in
  // the shipped class tree name files that are not in `data.pak` at all. The
  // handle comes back zero and `AIBreakScript(0)` is a no-op.
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(id)));
}

/// `Run(name)` -- 1 site, `DBGRUNSCRIPT.VS`'s `Run("data/subai/" + choice)`.
/// 0x004c6e30 spawns the named script with no arguments and prints a message
/// when there is no such script; the handle is not returned. The name goes
/// through the same resolution `AIRun` uses, which folds case and separators
/// and falls back to the leaf name.
HostOutcome builtin_run_free(CallContext& context) {
  if (context.count() < 1) return HostOutcome::failed("Run expects a script name");
  const HostOutcome spawned = spawn_from(context, context.arg(0), {});
  if (spawned.status != HostStatus::ok) return spawned;
  return HostOutcome::ok_void();
}

HostOutcome builtin_airun_free(CallContext& context) {
  if (context.count() < 1) return HostOutcome::failed("AIRun expects a script name");
  return spawn_from(context, context.arg(0), context.arguments.subspan(1));
}

HostOutcome builtin_airun_member(CallContext& context) {
  // `set.AIRun('ESH_NeedTech.vs')`, `gaika.AIRun('GSH_SynchApproach.vs', AIPlayer)`:
  // the receiver binds as the new script's first parameter, which is what the
  // signature comments of the scripts so started expect.
  if (context.count() < 2) return HostOutcome::failed("AIRun expects a script name");
  std::vector<Value> args;
  args.reserve(context.count() - 1);
  args.push_back(context.arg(0));
  for (std::size_t i = 2; i < context.count(); ++i) args.push_back(context.arg(i));
  return spawn_from(context, context.arg(1), args);
}

HostOutcome builtin_break_script(CallContext& context) {
  if (context.scheduler == nullptr) return HostOutcome::failed("AIBreakScript needs a scheduler");
  if (context.count() != 1 || !context.arg(0).is_integer()) {
    return HostOutcome::failed("AIBreakScript expects a script handle");
  }
  context.scheduler->kill(static_cast<ScriptId>(context.arg(0).as_integer()));
  return HostOutcome::ok_void();
}

HostOutcome builtin_start_player_script(CallContext& context) {
  if (context.count() != 2) return HostOutcome::failed("StartPlayerScript expects a player and a "
                                                      "script name");
  const Value player = context.arg(0);
  return spawn_from(context, context.arg(1), std::span<const Value>(&player, 1));
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

RunReport Scheduler::run_ready() {
  RunReport report;

  // Indexed, and re-reading `scripts_[i]` each time, because a script may spawn
  // others while it runs: the vector grows underneath this loop, and the new
  // entries -- which always have higher ids -- are picked up by the same pass.
  // `DATA\AI\MAIN.VS` is six `AIRun` calls and a return, and this is what makes
  // its six monitors start on the tick that started it.
  for (std::size_t i = 0; i < scripts_.size(); ++i) {
    if (scripts_[i].dead) continue;
    const ExecStatus status = scripts_[i].execution.status;
    const bool runnable = status == ExecStatus::ready ||
                          (status == ExecStatus::suspended &&
                           scripts_[i].execution.wake_time <= now_);
    if (!runnable) continue;
    if (scripts_[i].chunk_index >= chunks_.size()) continue;

    const ScriptId id = scripts_[i].id;
    const std::uint32_t chunk_index = scripts_[i].chunk_index;
    const VmEnv env = env_for(scripts_[i]);

    // Detached for the duration of the run. A host call can spawn, which
    // reallocates `scripts_`, and a reference held across that would dangle.
    Execution execution = std::move(scripts_[i].execution);
    execution.status = ExecStatus::ready;
    ++report.resumed;
    run(execution, chunks_[chunk_index], env);

    ScriptRecord* record = find(id);
    if (record == nullptr) continue;  // killed itself and was compacted away

    if (execution.status == ExecStatus::finished) {
      ++report.completed;
      record->dead = true;
    } else if (execution.status == ExecStatus::failed) {
      ++report.failed;
      report.traps.push_back(FailedScript{id, chunks_[chunk_index].source_name, execution.trap});
      record->dead = true;
    }
    record->execution = std::move(execution);
    // The slice is over and the record is whole again. See `StepHook`: this is
    // where the original performs a deferred `Erase`, and the reason it is here
    // rather than in the pass hook is that the next script must not see an
    // object the previous one destroyed.
    if (step_hook_ != nullptr) step_hook_(user_, *this, id);
  }

  report.visited = static_cast<std::uint32_t>(scripts_.size());
  compact();
  // After compaction, so the hook never sees a record that is on its way out,
  // and after every `Execution` has been moved back into its record, so a hook
  // that walks frames sees all of them. See `PassHook`.
  if (pass_hook_ != nullptr) pass_hook_(user_, *this);
  return report;
}

CallReport Scheduler::call(std::uint32_t chunk_index, std::span<const Value> args) {
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
  now_ += delta;
  return run_ready();
}

// -- serialisation ----------------------------------------------------------

void Scheduler::serialize(std::vector<std::byte>& out) const {
  put_u32(out, kSaveMagic);
  put_u32(out, kSaveVersion);
  put_i64(out, now_);
  put_u32(out, next_id_);

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
[[nodiscard]] Status read_section_header(ByteReader& reader, std::int64_t& now,
                                         std::uint32_t& next_id, std::uint32_t& count) {
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kSaveMagic) return FormatError::bad_magic;
  if (version != kSaveVersion) return FormatError::unsupported;
  if (!get_i64(reader, now) || !reader.u32(next_id) || !reader.u32(count)) {
    return FormatError::truncated;
  }
  return Status{};
}

/// One record's fixed prefix and its length-prefixed `Execution` payload. The
/// payload comes back as bytes rather than as an `Execution`: the caller that
/// wants one decodes it, and the caller that only wants the name skips it
/// without having to understand a format that changes underneath it.
[[nodiscard]] Status read_record_prefix(ByteReader& reader, ScriptId& id, std::string& source_name,
                                        ObjectRef& owner, ScriptId& parent,
                                        std::span<const std::byte>& payload) {
  std::uint16_t owner_type = 0;
  std::uint32_t owner_id = 0;
  if (!reader.u32(id) || !get_string(reader, source_name) || !reader.u16(owner_type) ||
      !reader.u32(owner_id) || !reader.u32(parent)) {
    return FormatError::truncated;
  }
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
  std::uint32_t count = 0;
  if (const Status header = read_section_header(reader, now, next_id, count); !header.ok()) {
    return header.error();
  }

  std::vector<std::string> names;
  names.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ScriptId id = kNoScript;
    std::string source_name;
    ObjectRef owner;
    ScriptId parent = kNoScript;
    std::span<const std::byte> payload;
    if (const Status record = read_record_prefix(reader, id, source_name, owner, parent, payload);
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
  std::uint32_t count = 0;
  if (const Status header = read_section_header(reader, now, next_id, count); !header.ok()) {
    return header;
  }

  std::vector<ScriptRecord> restored;
  restored.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ScriptRecord record;
    std::string source_name;
    std::span<const std::byte> payload;
    if (const Status prefix = read_record_prefix(reader, record.id, source_name, record.owner,
                                                 record.parent, payload);
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
  for (std::uint16_t arity = 1; arity <= 5; ++arity) {
    registry.define(CallKind::free_function, "AIRun", arity, &builtin_airun_free);
  }
  // Member `AIRun`: the receiver is argument zero, so arity 1 is
  // `set.AIRun("x.vs")` and arity 2 adds one bound parameter.
  for (std::uint16_t arity = 1; arity <= 5; ++arity) {
    registry.define(CallKind::member, "AIRun", arity, &builtin_airun_member);
  }
  registry.define(CallKind::free_function, "Run", 1, &builtin_run_free);
  registry.define(CallKind::free_function, "AIBreakScript", 1, &builtin_break_script);
  registry.define(CallKind::free_function, "KillScript", 0, &builtin_kill_script);
  registry.define(CallKind::free_function, "StartPlayerScript", 2,
                  &builtin_start_player_script);
}

}  // namespace imperivm::core::script
