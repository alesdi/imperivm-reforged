#pragma once

// The seam between the VS virtual machine and the world it drives.
//
// Every call in a `.vs` script goes to the host, because the language has
// nothing else to call: no user functions, no recursion, no imports. The
// surface is 185 free functions and 520 members across 29 types, 17,131 call
// sites; it is inventoried in docs/formats/vs-host-api.md and it *is* the
// interoperability boundary of the whole engine.
//
// Two consequences shape this header.
//
// **Dispatch is a flat (kind, name, arity) table.** Parentheses are optional on
// a zero-argument call -- `u.IsValid` and `u.IsValid()` both occur, in the same
// files -- so a bare member access is indistinguishable from a property read
// and there is no property namespace to hang anything off. `.health`, `.pos`
// and `.IsValid` are all zero-argument member calls. Free functions and members
// are separate tables only because the receiver distinguishes them; within each
// one, name and arity are the whole key. Member lookup is case-insensitive,
// because the corpus spells the same host entry point `GetGAIKA` on `point` and
// `GetGaika` on `Settlement`; locals and free functions are case-sensitive,
// which `OUTPOST_IDLE.VS` proves by keeping `This` and `this` live at once.
//
// **Everything unimplemented must fail loudly.** The whole surface is declared
// here so that dispatch is wired from the start and Part 5 can fill entries in
// without reshaping anything, but a script that reaches an undefined entry
// point traps with its name and arity. Silently returning zero would turn a
// missing host function into a subtle behavioural divergence, which is exactly
// the class of bug the conformance harness exists to catch and the hardest one
// to trace back.
//
// ## Argument passing and out-parameters
//
// The host receives a *mutable* window over the call's arguments. Writing to it
// is how out-parameters work: `g.Eval(AI_COMING, idPlayer, own, ally, enemy,
// enemy_hidden)` writes through its last four arguments, `ParseStr(s, tail)`
// writes through its second, and nothing at the call site says so. The compiler
// records which arguments were assignable lvalues and the VM copies those back
// after the call returns. A host function that does not write leaves the window
// alone and the copy-back is a no-op.
//
// ## Suspension
//
// A host call can suspend the script. `Sleep(ms)` is the common case, but
// `WaitNonEmptyQuery(q, ms)` and `WaitQueryCountBetween(...)` prove the
// mechanism is general, so it lives in the call protocol rather than in a
// dedicated opcode: a host function returns `suspend` (resume after the call,
// with the result it produced) or `retry` (resume *at* the call and run it
// again, for a wait that must re-poll a condition).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/bytecode.hpp"
#include "imperivm/core/script/value.hpp"

namespace imperivm::core::script {

class Scheduler;

/// The handle `AIRun` returns and `AIBreakScript` takes. Zero is never issued.
using ScriptId = std::uint32_t;

inline constexpr ScriptId kNoScript = 0;

/// What a host call did.
enum class HostStatus : std::uint8_t {
  ok,       ///< produced `value`; carry on
  suspend,  ///< produced `value`, then suspend for `suspend_for` milliseconds
  retry,    ///< suspend and run this same call again on resume
  error,    ///< `error` names what went wrong; the script traps
};

struct HostOutcome {
  HostStatus status = HostStatus::ok;
  Value value;
  /// Game-time milliseconds to suspend for. `Sleep(1)` yields for one.
  std::int64_t suspend_for = 0;
  /// A static string. Traps quote it verbatim, so make it identifiable.
  const char* error = nullptr;

  static HostOutcome ok_with(Value v) { return {HostStatus::ok, std::move(v), 0, nullptr}; }
  static HostOutcome ok_void() { return {}; }
  static HostOutcome failed(const char* why) { return {HostStatus::error, Value::nil(), 0, why}; }
  static HostOutcome sleep_for(std::int64_t ms) {
    return {HostStatus::suspend, Value::nil(), ms, nullptr};
  }
};

/// Object-model services the interpreter cannot supply for itself.
///
/// The VM knows integers and strings. Everything else -- what a `point` is,
/// what adding two of them means, what `ol[0]` yields, what `AI_COMING` is
/// worth -- belongs to the host, and this is where it hands those answers back.
/// The defaults refuse, so a host that has not implemented `point` arithmetic
/// gets a trap that says so rather than a wrong number.
class Host {
 public:
  virtual ~Host();

  /// Where a local was declared: the running script, and its slot index.
  ///
  /// The pair identifies a **declaration site**, not an execution of one. That
  /// distinction is the whole point: `TS_CARTHAGETACTIC.VS` declares
  /// `ObjList ol;` inside a loop body, so a host that minted a fresh resource
  /// every time the declaration ran would leak one per iteration for as long as
  /// the tactic script lives, which is the whole match.
  struct DeclarationSite {
    ScriptId script = kNoScript;
    std::uint32_t slot = 0;
  };

  /// Build the initial value of `<type> name;`.
  ///
  /// Locals are default-constructed to an invalid or empty state: 331 files
  /// declare a handle and assign it several statements later, and many declare
  /// an `ObjList` and immediately `.Clear` or `.Add` on it. `int` and `bool`
  /// start at zero and `str` at empty, which the base implementation does; a
  /// host that owns `point`, `rect`, `ObjList` and the handle types overrides
  /// this to mint them.
  virtual Value default_value(std::string_view type_name);

  /// The same, told where the declaration is.
  ///
  /// Additive on purpose. The VM calls **this** overload, and the default
  /// forwards to the one above, so a host that does not care about the site
  /// overrides only the simple form and nothing changes for it. A host that
  /// owns a pooled type -- `ObjList` is the one that ships -- overrides this
  /// one and keys its pool by the site.
  ///
  /// It exists because the information was already there and unreachable:
  /// `Op::declare_local` carries the slot and `VmEnv` carries the script, and
  /// the one-argument signature threw both away. `sim/objlist.hpp` had to work
  /// around that with a lazily bound handle; see its note.
  virtual Value default_value(std::string_view type_name, DeclarationSite site);

  /// Copy semantics on assignment.
  ///
  /// `point` and `rect` are value types in VS -- `pt.SetLen(15)` mutates `pt`
  /// alone -- but they are opaque handles here, so a host that represents them
  /// by handle must clone on assignment or aliasing will leak between locals.
  /// Handle types (`Unit`, `ObjList`, `Query`) are references and must not be
  /// cloned. The base implementation copies the value as-is.
  virtual Value clone_for_assign(const Value& value);

  /// Truthiness of an object value, for `if (setGIn.IsValid)`-shaped tests.
  virtual Result<bool> truthy(const Value& value);

  /// A binary operator with at least one object operand: `ptCenter + ol[i].pos`,
  /// `pt / ol.count`, `(sqLeader.pos - ptBld) * nSight`.
  virtual Result<Value> binary(BinaryOp op, const Value& lhs, const Value& rhs);

  /// `ol[0]`, `aSkills[i]`.
  virtual Result<Value> index_get(const Value& container, const Value& key);

  /// `aSkills[i] = 3`. 197 of the corpus's 5,805 assignment targets.
  virtual Status index_set(Value& container, const Value& key, const Value& value);

  /// A bare name that is neither a local nor a parameter: the 245 global
  /// constants and ambient globals (`AI_COMING`, `Carthage`, `cmdparam`).
  virtual Result<Value> global(std::string_view name);

  /// How an object renders inside a string concatenation.
  virtual Result<std::string> to_string(const Value& value);
};

/// Everything a host function is given.
struct CallContext {
  /// The argument window, receiver first for a member call. Mutable: this is
  /// the out-parameter mechanism.
  std::span<Value> arguments;
  Host* host = nullptr;
  /// Present when the call runs under a scheduler, which is how `AIRun` and
  /// `AIBreakScript` reach it. Null for a bare `Vm` run.
  Scheduler* scheduler = nullptr;
  /// The running script, so a host call can name or address itself.
  ScriptId script = kNoScript;
  /// Whatever the embedder passed in. Part 5's world pointer lives here.
  void* user = nullptr;
  std::string_view name;
  CallKind kind = CallKind::free_function;

  /// Game time now, in milliseconds. The clock is the world's, not the wall's.
  std::int64_t now = 0;
  /// Game time at which *this call* first returned `HostStatus::retry`, or
  /// `now` when it has not retried yet.
  ///
  /// The one thing a blocking entry point needs that a plain one does not:
  /// `now - waiting_since` is how long it has been waiting, so it can decide
  /// whether its own timeout has expired. Zero elapsed on the first run is what
  /// makes `if (WaitQueryCountBetween(q, 1, 60, 100))` work at all -- the
  /// predicate is tested *before* anything suspends, so a 100 ms timeout still
  /// gets one honest test even when a turn is 800 ms long.
  std::int64_t waiting_since = 0;
  /// Whether this is the call's **first** run rather than a re-entry after a
  /// retry. `gbr.exe` passes the same bit: the interpreter writes 1 into the
  /// last byte of a fresh call frame at 0x0069d593 and 0 on every resume at
  /// 0x0069d577, and `WaitSettlementCapture` (0x005eda80) reads it to resolve
  /// its settlement name once instead of on every poll. Nothing here needs it
  /// yet; it is exposed because it is the same information the original gives
  /// and it costs a bool.
  bool first_call = true;

  [[nodiscard]] std::size_t count() const { return arguments.size(); }
  [[nodiscard]] const Value& arg(std::size_t i) const { return arguments[i]; }
  [[nodiscard]] Value& out(std::size_t i) { return arguments[i]; }
};

/// A plain function pointer, not a `std::function`: dispatch happens 17,131
/// times per corpus pass and the table is world-invariant, so there is nothing
/// to capture and no reason to allocate.
using HostFn = HostOutcome (*)(CallContext&);

/// One entry point.
struct HostEntry {
  CallKind kind = CallKind::free_function;
  std::string name;
  /// The lookup key: `name` verbatim for a free function, lowercased for a
  /// member. Precomputed because the table is built once and searched 17,131
  /// times per corpus pass.
  std::string key;
  std::uint16_t arity = 0;
  /// Null until someone implements it. Calling an unimplemented entry point
  /// traps with `name` and `arity` rather than returning a plausible zero.
  HostFn fn = nullptr;
};

/// The dispatch table.
///
/// Declared entries are the known surface; defined entries are the ones that
/// actually do something. Both are needed: the split is what lets a script
/// compile today, run as far as its first unimplemented call, and say exactly
/// which one it was.
class HostRegistry {
 public:
  /// Add a known but unimplemented entry point. Idempotent.
  std::uint32_t declare(CallKind kind, std::string_view name, std::uint16_t arity);

  /// Implement one, declaring it if it was not in the inventory.
  std::uint32_t define(CallKind kind, std::string_view name, std::uint16_t arity, HostFn fn);

  /// `kUnresolvedHost` when the name and arity are not in the table at all.
  [[nodiscard]] std::uint32_t find(CallKind kind, std::string_view name,
                                   std::uint16_t arity) const;

  [[nodiscard]] const HostEntry& entry(std::uint32_t index) const { return entries_[index]; }
  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] std::size_t implemented() const;
  [[nodiscard]] const std::vector<HostEntry>& entries() const { return entries_; }

 private:
  /// The lookup key: exact for free functions, lowercased for members.
  static std::string key_for(CallKind kind, std::string_view name);
  void reindex() const;

  std::vector<HostEntry> entries_;
  /// Indices into `entries_`, ordered by (kind, key, arity). Rebuilt lazily so
  /// that declaration order does not matter and lookup stays a binary search.
  mutable std::vector<std::uint32_t> order_;

  /// Exact `(kind, key, arity)` to entry index, maintained as entries are
  /// appended.
  ///
  /// **Only `declare` reads it, and only so that declaring does not re-sort.**
  /// `declare` asks `find` whether the entry already exists, and `find`
  /// reindexes when the table is dirty -- which `declare` itself made it, on
  /// the previous call. Building the shipped surface is a little over two
  /// thousand declarations, so that was two thousand full sorts of an
  /// ever-growing table: **O(n^2 log n)** to build a table that is then read
  /// with a binary search. Every test fixture in the suite builds one.
  ///
  /// `find` is deliberately left on the sorted order. It is the hot path -- one
  /// lookup per host call, 33,965 of them in a corpus pass -- and changing it
  /// would be a second thing to get wrong in the same commit for no measured
  /// gain; the lazy reindex now happens once, after the declarations, instead
  /// of once per declaration.
  std::unordered_map<std::string, std::uint32_t> by_signature_;
  mutable bool dirty_ = true;
};

/// Declare the whole shipped host surface: every free function and member name
/// and arity that occurs in the 577 scripts, all unimplemented.
///
/// Generated from docs/formats/vs-host-api.md. Wiring it all up front costs
/// nothing at run time and means Part 5 adds behaviour with `define()` alone --
/// no new names, no new arities, no reshaping.
void declare_shipped_surface(HostRegistry& registry);

/// Declare the 245 global constants and ambient globals as names the host is
/// expected to answer for. Purely informational: `Host::global` still decides.
std::span<const std::string_view> shipped_global_names();

}  // namespace imperivm::core::script
