// The `Wait*` family. Specification and evidence: sim/wait.hpp.

#include "imperivm/core/sim/wait.hpp"

#include <vector>

#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/conversation.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::Value;

constexpr const char* kNoWorld = "no world on the call context";
constexpr const char* kNoEnvSystem = "no env system on the world";
/// Not a query handle and not a named object either.
///
/// **There is no coercion, because there is nothing to coerce.** `NamedObj`
/// (type code 0x22) is a *subtype* of `Query` (0x20) in the script type graph:
/// 0x005b6d96 calls the parent-setting registrar 0x006997d0 with child 0x22 and
/// parent 0x20, both types are registered at 0x005b6d3c and 0x005b6d62 with the
/// same 12-byte size and the same copy/assign/destroy handlers, and
/// `Query::IsValid` and `NamedObj::IsValid` are literally one body (0x00577af0).
/// So `WaitQueryCountBetween(Caesar, 1, 1, -1)` is a free upcast the compiler
/// resolves at zero cost (0x0068efee) and the call boundary never looks at a
/// type word at all -- opcode 0x1E (0x0069d597) reads an entry index and a
/// frame delta, nothing else.
///
/// The ten shipped sites that pass one are therefore *correct*, not sloppy,
/// and this engine answers them the way the original does: see `evaluate`.
constexpr const char* kNotAQuery = "expects a Query or a NamedObj";
/// `gbr.exe` answers **true** here and swallows its own diagnostic: the failure
/// path at 0x005ed05f calls 0x00686eb0, which is a bare `ret` in the retail
/// build, and then completes the wait successfully. Reproducing that would turn
/// every dangling handle into a silently satisfied condition -- a wrong answer
/// wearing the shape of a right one, and one no test could see. Refused by name
/// instead, and the divergence is recorded in the header.
/// The two verbs the original accepts as "at rest", compared against the
/// object's command string at 0x005ece5a and 0x005ece6a.
constexpr std::string_view kRestingVerb = "idle";
constexpr std::string_view kStandstillVerb = "standstill";
constexpr const char* kNoCommandSystem =
    "WaitIdle needs the command system, and this world has none";
/// `gbr.exe` answers **true** and says nothing at all: the area lookup at
/// 0x005ee3f1 returns 0, the `testl`/`jne` at 0x005ee419 falls through to
/// 0x005ee41d, and the wait completes successfully without even reaching a
/// diagnostic -- unlike the dead-query path, which at least calls the (dead)
/// printer. A misspelt area name therefore satisfies every wait on it. Refused
/// by name here for the same reason `kDeadQuery` is.
constexpr const char* kNoSuchArea =
    "no area of that name; gbr.exe answers true here and prints nothing, see "
    "sim/wait.cpp";
constexpr const char* kNoAreaSystem = "no area system on the world";
constexpr const char* kNoEconomySystem = "no economy system on the world";
/// 0x005edc46: an unresolvable settlement name ends the wait **true**, after
/// calling the same dead diagnostic. Refused rather than reproduced.
constexpr const char* kNoSuchSettlement =
    "no settlement of that name; gbr.exe answers true here, see sim/wait.cpp";
/// 0x005ed3d7 divides by `sumMax` after a guard that only covers
/// `sumHP == sumMax`, so a query whose objects declare no `maxhealth` at all
/// divides by zero -- a `#DE`, not a wrong answer. There is nothing to
/// reproduce and nothing to infer, so this refuses instead.
constexpr const char* kNoMaxHealth =
    "WaitHealthBetween: the query's objects declare no maxhealth, which faults "
    "in gbr.exe rather than answering; see sim/wait.cpp";
constexpr const char* kDeadQuery =
    "the Query handle no longer names a query; gbr.exe answers true here and "
    "swallows the diagnostic, see sim/wait.hpp";

[[nodiscard]] HostOutcome answer(bool met) { return HostOutcome::ok_with(Value::boolean(met)); }

/// The shared tail of every entry point: the predicate has just been tested and
/// did not hold, so either poll again or give up.
[[nodiscard]] HostOutcome keep_waiting(CallContext& ctx, std::int64_t timeout,
                                       std::int64_t interval) {
  const std::int64_t slice = wait_slice(ctx.now - ctx.waiting_since, timeout, interval);
  if (slice <= 0) return answer(false);
  HostOutcome outcome;
  outcome.status = script::HostStatus::retry;
  outcome.suspend_for = slice;
  return outcome;
}

[[nodiscard]] std::int64_t timeout_arg(const CallContext& ctx, std::size_t index) {
  return ctx.arg(index).is_integer() ? ctx.arg(index).as_integer() : 0;
}

[[nodiscard]] std::int32_t int_arg(const CallContext& ctx, std::size_t index) {
  return ctx.arg(index).is_integer() ? ctx.arg(index).as_integer() : 0;
}

/// Resolve a `Query` argument and evaluate it. `ok` is false when the argument
/// is not a query handle at all; `alive` is false when it is one the world no
/// longer holds.
struct QueryResult {
  bool ok = false;
  bool alive = false;
  std::vector<ObjectId> objects;
};

[[nodiscard]] QueryResult evaluate(World& world, const Value& value) {
  QueryResult out;
  if (!value.is_object()) return out;

  // A `NamedObj` is a `Query` whose membership is explicit rather than
  // computed: `CVXNamedObjQuery` (vtable 0x7c4830) holds a deque that
  // `Obj::SetName` puts exactly one handle into (0x00572c60), and that
  // `Obj::OnDestroy` takes back out again (0x005ae024). So the query goes
  // **empty**, not invalid, when the object dies -- which is precisely what
  // makes `WaitQueryCountBetween(Caesar, 1, 1, -1)` and then
  // `(Caesar, 0, 0, -1)` read as "wait until he exists" and "until he is gone".
  //
  // The handle a named-object global carries here is a *table index*, not an
  // object id; `NamedObjectTable::object` is the indirection, and it answers
  // with an id that may since have died, which is why this re-checks `find`.
  if (value.as_object().type == kTypeNamedObj) {
    out.ok = true;
    out.alive = true;
    const auto index = static_cast<std::int32_t>(value.as_object().id);
    const ObjectId bound = world.named_objects().object(index);
    if (bound != kNoObject && world.find(bound) != nullptr) out.objects.push_back(bound);
    return out;
  }

  if (value.as_object().type != kTypeQuery) return out;
  out.ok = true;
  const auto id = static_cast<ObjectId>(value.as_object().id);
  if (id == kNoObject || world.query_spec(id) == nullptr) return out;
  out.alive = true;
  world.evaluate_query(id, out.objects);
  return out;
}

// -- the query family, all at the 100 ms cadence -----------------------------

/// `WaitQueryCountBetween(q, low, high, timeout)`. 208 sites, the family's
/// largest by a factor of four.
///
/// `count >= low && (count <= high || high < 0)`, **both bounds inclusive**,
/// and a negative `high` means no upper bound. Read off 0x005ed08b..0x005ed099
/// -- `jle` to the lower check, `test`/`jge` for the escape, `jl` to reject --
/// and corroborated two ways: `WaitEnvIntBetween` (0x005ecbf5) emits the
/// identical sequence, and seven shipped sites pass `high = -1`
/// (`WaitQueryCountBetween(RomanAtt, 1, -1, -1)`). There is no matching escape
/// for `low`, which is why this is not written as a symmetric range test.
HostOutcome fn_query_count_between(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  const QueryResult q = evaluate(*world, ctx.arg(0));
  if (!q.ok) return HostOutcome::failed(kNotAQuery);
  if (!q.alive) return HostOutcome::failed(kDeadQuery);

  const auto count = static_cast<std::int32_t>(q.objects.size());
  const std::int32_t low = int_arg(ctx, 1);
  const std::int32_t high = int_arg(ctx, 2);
  if (count >= low && (count <= high || high < 0)) return answer(true);
  return keep_waiting(ctx, timeout_arg(ctx, 3), kQueryPollInterval);
}

/// `WaitEmptyQuery(q, timeout)`. 50 sites. 0x005ed730.
HostOutcome fn_empty_query(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  const QueryResult q = evaluate(*world, ctx.arg(0));
  if (!q.ok) return HostOutcome::failed(kNotAQuery);
  if (!q.alive) return HostOutcome::failed(kDeadQuery);
  if (q.objects.empty()) return answer(true);
  return keep_waiting(ctx, timeout_arg(ctx, 1), kQueryPollInterval);
}

/// `WaitNonEmptyQuery(q, timeout)`. 0x005ed800, whose predicate is the negation
/// of the one above at 0x005ed86f.
HostOutcome fn_non_empty_query(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  const QueryResult q = evaluate(*world, ctx.arg(0));
  if (!q.ok) return HostOutcome::failed(kNotAQuery);
  if (!q.alive) return HostOutcome::failed(kDeadQuery);
  if (!q.objects.empty()) return answer(true);
  return keep_waiting(ctx, timeout_arg(ctx, 1), kQueryPollInterval);
}

/// `WaitObjInQuery(obj, q, timeout)`. 0x005ecf00, which asks the query itself
/// through vtable slot 0x24 rather than scanning a list.
HostOutcome fn_obj_in_query(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  const QueryResult q = evaluate(*world, ctx.arg(1));
  if (!q.ok) return HostOutcome::failed(kNotAQuery);
  if (!q.alive) return HostOutcome::failed(kDeadQuery);
  if (ctx.arg(0).is_object()) {
    const auto wanted = static_cast<ObjectId>(ctx.arg(0).as_object().id);
    for (const ObjectId id : q.objects) {
      if (id == wanted) return answer(true);
    }
  }
  return keep_waiting(ctx, timeout_arg(ctx, 2), kQueryPollInterval);
}

/// `WaitCommonObjects(q1, q2, timeout)`. 0x005ed100: any object of the first
/// query that is also in the second. Its cadence comes from the two-query
/// helper 0x005eca70, which takes the smaller of the two hints and floors it at
/// the same 100.
HostOutcome fn_common_objects(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  const QueryResult a = evaluate(*world, ctx.arg(0));
  const QueryResult b = evaluate(*world, ctx.arg(1));
  if (!a.ok || !b.ok) return HostOutcome::failed(kNotAQuery);
  if (!a.alive || !b.alive) return HostOutcome::failed(kDeadQuery);
  // Both sides come out in ascending id order, so this is a merge rather than a
  // nested scan -- the same reasoning `evaluate_query`'s set ops use.
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.objects.size() && j < b.objects.size()) {
    if (a.objects[i] == b.objects[j]) return answer(true);
    if (a.objects[i] < b.objects[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return keep_waiting(ctx, timeout_arg(ctx, 2), kQueryPollInterval);
}

// -- the environment store, at 2000 ms ---------------------------------------

/// `WaitEnvIntBetween(key, low, high, timeout)`. 18 sites, 15 of them with no
/// timeout. 0x005ecb70, and its bounds test at 0x005ecbf5 is byte-for-byte the
/// shape `WaitQueryCountBetween` uses -- which is the corroboration for both.
///
/// The key goes through the same root/scoped rule every other `Env*` entry
/// point uses: a leading slash is absolute. Every shipped site has one
/// (`WaitEnvIntBetween("/En_NumidiansCharge", 1, 2, -1)`).
HostOutcome fn_env_int_between(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  EnvSystem* env = env_of(*world);
  if (env == nullptr) return HostOutcome::failed(kNoEnvSystem);
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("WaitEnvIntBetween expects a key");

  // The root scope with the key verbatim, exactly as `EnvReadInt/1` does it:
  // `MakePath` (0x006ab0e0) leaves a path starting with `/` unchanged, and all
  // 18 shipped sites start with one.
  const std::int32_t value =
      env->env().read_int(EnvScope::root(), ctx.arg(0).as_string());
  const std::int32_t low = int_arg(ctx, 1);
  const std::int32_t high = int_arg(ctx, 2);
  if (value >= low && (value <= high || high < 0)) return answer(true);
  return keep_waiting(ctx, timeout_arg(ctx, 3), kEnvPollInterval);
}


// -- the four that read the world, not just the query ------------------------

/// `WaitIdle(q, timeout)`. 13 sites. Body 0x005ecd80, registered 0x005ee9da.
///
/// Every object of the query is at rest, and an **empty query never
/// satisfies** -- 0x005ece05 jumps past the loop entirely rather than letting
/// "all of nothing" succeed, which is the opposite of what a vacuous universal
/// would do and is why this is not written as a plain `all_of`.
///
/// "At rest" is two verbs in the original, compared against the object's
/// current-command string at `obj+0x10c` (0x005ece5a, 0x005ece6a): `"idle"`
/// and `"standstill"`. `"standstill"` occurs exactly once in the whole
/// executable -- here -- which is what identifies the second one at all.
///
/// **An object with no command queue counts as resting, and that is a reading
/// rather than a transcription.** In this engine `GameSession::start_object_
/// scripts` spawns each class's `idle` method straight onto the scheduler and
/// never routes it through `CommandSystem`, so a unit that has not been given
/// an order has no queue and `command_name` answers `{}` -- where the original
/// would be holding the string `"idle"`. Treating the empty answer as anything
/// but resting would make `WaitIdle(Q_Slingers, -1)` -- a shipped site with no
/// timeout -- wait forever on a map where nothing has been commanded yet.
/// `Obj::command` had the same gap and now answers the default verb for an
/// empty queue, by the same reading (`command_impl` in `sim/command.cpp`);
/// this reads `command_name` directly, so it keeps its own empty case.
HostOutcome fn_idle(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  CommandSystem* commands = command_system(*world);
  if (commands == nullptr) return HostOutcome::failed(kNoCommandSystem);
  const QueryResult q = evaluate(*world, ctx.arg(0));
  if (!q.ok) return HostOutcome::failed(kNotAQuery);
  if (!q.alive) return HostOutcome::failed(kDeadQuery);

  if (!q.objects.empty()) {
    bool all_resting = true;
    for (const ObjectId id : q.objects) {
      const std::string_view verb = commands->command_name(id);
      if (verb.empty() || verb == kRestingVerb || verb == kStandstillVerb) continue;
      all_resting = false;
      break;
    }
    if (all_resting) return answer(true);
  }
  return keep_waiting(ctx, timeout_arg(ctx, 1), kQueryPollInterval);
}

/// `WaitHealthBetween(q, low, high, timeout)`. 8 sites, and all eight are
/// literally `(0, 10, 2000)`. Body 0x005ed2c0, registered 0x005eea54.
///
/// `low <= pct && pct <= high`, both bounds inclusive and **neither with the
/// negative escape `WaitQueryCountBetween` has**: 0x005ed3dd is `cmp`/`jg`
/// followed by `cmp`/`jl` with no `test`/`jge` between them, where
/// 0x005ed08b..0x005ed099 does carry one. A negative `high` here makes the
/// predicate unsatisfiable rather than unbounded. No shipped site passes one,
/// so this is transcribed rather than observed.
///
/// `pct` is **100 when the sums are equal** and `sumHP * 100 / sumMax`
/// otherwise (0x005ed3c7 for the equality, 0x005ed3d3 for the signed
/// truncating `idiv`). The equality case is not an optimisation: it is also
/// what makes an **empty query read as full health**, since both sums are then
/// zero -- and unlike its two siblings this body has no size check anywhere.
/// With the shipped `(0, 10)` bounds an empty query therefore keeps waiting.
///
/// Health is `ObjectState::health`; the maximum is the class property
/// `maxhealth`, which is where `Obj::maxhealth` reads it. The two fields the
/// original sums are `[obj+0xc0]` and `[obj+0xc8]`, identified against
/// `Obj::health` (0x005adac3) and `Obj::maxhealth` (0x005adb83).
HostOutcome fn_health_between(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  const QueryResult q = evaluate(*world, ctx.arg(0));
  if (!q.ok) return HostOutcome::failed(kNotAQuery);
  if (!q.alive) return HostOutcome::failed(kDeadQuery);

  std::int64_t sum_health = 0;
  std::int64_t sum_max = 0;
  for (const ObjectId id : q.objects) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr) continue;  // 0x005ed394 skips a null handle likewise
    sum_health += slot->state.health;
    sum_max += class_int(*world, *slot, "maxhealth");
  }

  std::int64_t percent = 100;
  if (sum_health != sum_max) {
    if (sum_max == 0) return HostOutcome::failed(kNoMaxHealth);
    percent = sum_health * 100 / sum_max;
  }

  const std::int64_t low = int_arg(ctx, 1);
  const std::int64_t high = int_arg(ctx, 2);
  if (percent >= low && percent <= high) return answer(true);
  return keep_waiting(ctx, timeout_arg(ctx, 3), kQueryPollInterval);
}

/// `WaitSettlementCapture(name, player, timeout)`. 14 sites, every one of them
/// with a timeout of -1. Body 0x005eda80, registered 0x005eea70.
///
/// **A string, not a settlement handle** -- argument type 0xb at 0x005eea5d.
/// The name is resolved by scanning the settlement vector and comparing the
/// name each settlement carries, which is what `GetSettlement/1` does too
/// (0x005c3b17 against 0x005edb03: same table, same key). That is why
/// `Settlement` carries a `name` at all.
///
/// The predicate is `settlement->owner->index == player - 1` (0x005edbc9,
/// with the `decl` at 0x005edbd8), and the `- 1` is the one-based script
/// player number meeting a zero-based owner index -- exactly what
/// `player_from_script` does. Two controls: `Settlement::player` (0x005c2340)
/// reads the same two dereferences and *increments*, and `Obj::SetPlayer`
/// (0x005ab329) subtracts one and rejects anything outside 1..16.
///
/// An unresolvable player number must select **nothing**. `player_from_script`
/// answers `kNoPlayer` for one, and `kNoPlayer` is also what an *unowned*
/// settlement carries -- so comparing the two directly would make
/// `WaitSettlementCapture("S_Yard", 0, -1)` succeed the moment it ran, on the
/// wrong settlement, for the wrong reason.
HostOutcome fn_settlement_capture(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (!ctx.arg(0).is_string()) {
    return HostOutcome::failed("WaitSettlementCapture expects a settlement name");
  }
  EconomySystem* economy = economy_of(*world);
  if (economy == nullptr) return HostOutcome::failed(kNoEconomySystem);
  const Settlement* settlement = economy->settlements().find_by_name(ctx.arg(0).as_string());
  if (settlement == nullptr) return HostOutcome::failed(kNoSuchSettlement);

  const PlayerId wanted = player_from_script(int_arg(ctx, 1));
  if (wanted != kNoPlayer && settlement->owner == wanted) return answer(true);
  return keep_waiting(ctx, timeout_arg(ctx, 2), kSettlementPollInterval);
}

/// `WaitUnitsInArea(q, area, timeout)`. 12 sites. Body 0x005ee310, registered
/// 0x005eeb48.
///
/// **A conjunction of two separate tests**, and the second is easy to miss:
/// the loop must run to the end (0x005ee4b5) *and* the query must be non-empty
/// (0x005ee4c0). Unlike `WaitIdle`, which excludes the empty query before the
/// loop, this one excludes it after -- same outcome, and worth transcribing in
/// the same shape so the two read as the deliberate pair they are.
///
/// Containment is the **query rule**, `d2 <= r*r`: 0x004d7df0 reads the shape
/// discriminator at `[area+0x148]` and compares against the `r2` field of the
/// `{cx, cy, r, r2}` POD. It is *not* the sampler's `d2 < (r+1)^2`. See
/// `sim/area.hpp`, which carries both rules and the reason they differ.
///
/// **Two things in the original's filter are not reproduced, because they were
/// not read.** It tests only objects carrying bit 0x400000 at `[obj+0x2c]`,
/// treating the rest as satisfying; that bit is set from data and no `orl`
/// writes it anywhere in the image. And an object that fails the shape test
/// gets a second chance through a tile-walk keyed on a 16-bit handle at
/// `[obj+0x154]` whose meaning was not established, so "outside the shape" may
/// not always mean false there. This engine tests every object in the query,
/// once, against the shape.
HostOutcome fn_units_in_area(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  const QueryResult q = evaluate(*world, ctx.arg(0));
  if (!q.ok) return HostOutcome::failed(kNotAQuery);
  if (!q.alive) return HostOutcome::failed(kDeadQuery);
  if (!ctx.arg(1).is_string()) {
    return HostOutcome::failed("WaitUnitsInArea expects an area name");
  }
  if (area_system_of(*world) == nullptr) return HostOutcome::failed(kNoAreaSystem);
  const AreaShape* area = area_named(*world, ctx.arg(1).as_string());
  if (area == nullptr) return HostOutcome::failed(kNoSuchArea);

  bool all_inside = true;
  for (const ObjectId id : q.objects) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr) continue;
    if (area->contains_by_query_rule(slot->state.position)) continue;
    all_inside = false;
    break;
  }
  if (all_inside && !q.objects.empty()) return answer(true);
  return keep_waiting(ctx, timeout_arg(ctx, 2), kSettlementPollInterval);
}

[[nodiscard]] Value invalid_object() {
  return Value::object(script::ObjectRef{script::kNoType, 0});
}

/// One side of a `WaitConvRequest`.
///
/// A `Query` or a `NamedObj` contributes its members; any other object handle
/// contributes the id it carries. The original does not choose between those
/// readings at the call boundary either: it files whichever handle id it was
/// given and the matcher (`0x0050e7d0`) casts it when it tests -- a `Query`
/// through the membership slot, an `Obj` by identity. That is why the
/// three-argument overload is registered `(Obj, Obj, int)` and all three of its
/// shipped sites pass a `NamedObj`.
///
/// **Whether that id still names something is not asked here**, deliberately:
/// `within_conversation_reach` looks both parties up and answers false for
/// either that is gone, so a second test would be a second place to get the
/// rule wrong and no observable difference.
void conv_party(World& world, const Value& value, std::vector<ObjectId>& out) {
  const QueryResult side = evaluate(world, value);
  if (side.ok) {
    out = side.objects;
    return;
  }
  if (!value.is_object()) return;
  const auto id = static_cast<ObjectId>(value.as_object().id);
  if (id != kNoObject) out.push_back(id);
}

/// Both `WaitConvRequest` overloads: `(Obj, Obj, timeout)` at `0x005ed8d0`,
/// three sites, and `(Query, Query, timeout, Obj&, Obj&)` at `0x005ed480`,
/// four. The last of the family, and the one `docs/plan.html` records as
/// needing a conversation manager.
///
/// **It needs no manager, and `sim/conversation.hpp` says at length why.** The
/// original files a record and lets the input layer complete it because only
/// the input layer can see the player's selection; drop that gate -- which is
/// presentation state and must not be hashed -- and what is left is a
/// predicate over the world, which is what every other member of this family
/// already is. `within_conversation_reach` is the predicate.
///
/// The five-argument overload writes the pair that met into its last two
/// arguments, and writes the **invalid object** into both when it times out
/// (`0x005ed693`, `0xffff` twice) rather than leaving the caller's locals
/// alone. Three of the four shipped sites read one of them straight afterwards
/// -- `conv.SetActor("Pich", o1.AsUnit())`, `if (o2.name == "NO_mercenary")` --
/// so the clearing is what keeps a timed-out wait from handing the next
/// conversation the actor from the previous one.
///
/// A party that names nothing live simply never matches, which is the
/// original's behaviour rather than a simplification of it: `0x005ed931`
/// branches straight past the manager when a handle does not resolve, so the
/// call registers nothing, can never be completed, and burns its timeout down
/// to `false`. This reaches the same answer by having no pair to find.
[[nodiscard]] HostOutcome conv_request(CallContext& ctx, bool with_outputs) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);

  std::vector<ObjectId> walkers;
  std::vector<ObjectId> hosts;
  conv_party(*world, ctx.arg(0), walkers);
  conv_party(*world, ctx.arg(1), hosts);

  // **Never on the first call.** The original files the record on the fresh
  // frame and reads it back on a resume; the completing side is the input
  // layer's per-frame offer, so a request cannot be met inside the call that
  // made it, whatever is standing where. Without this, `while (…)
  // { WaitConvRequest(hero, talkers, -1, o1, o2); … }` -- `3_Great_Losses_Egypt`'s
  // `Maps/1/Sequences/seq8.vs` -- spins through its whole instruction budget
  // the moment the hero stands near any talker, because a predicate that is
  // true now is true again on the next line. A timeout of 0 therefore answers
  // false, as the original's does: the record is appended and then found still
  // pending against an expired countdown.
  ObjectId walker = kNoObject;
  ObjectId host = kNoObject;
  if (!ctx.first_call && conversation_meeting(*world, walkers, hosts, walker, host)) {
    if (with_outputs) {
      ctx.out(3) = Value::object(script::ObjectRef{kTypeObj, static_cast<std::uint32_t>(walker)});
      ctx.out(4) = Value::object(script::ObjectRef{kTypeObj, static_cast<std::uint32_t>(host)});
    }
    return answer(true);
  }

  const HostOutcome outcome = keep_waiting(ctx, timeout_arg(ctx, 2), kSettlementPollInterval);
  if (with_outputs && outcome.status != script::HostStatus::retry) {
    ctx.out(3) = invalid_object();
    ctx.out(4) = invalid_object();
  }
  return outcome;
}

/// `EndConvSetup(This, other)` -- 1 site, and it was the sole blocker of
/// `UNIT_GO_TALK.VS` (8 sites): the walker's half of a conversation, which
/// walks to the other party and, once both are alive and it has arrived,
/// calls this.
///
/// `0x005ecc80` is registered as suspending over two `Obj`s. It is the
/// **offer** side of the manager `sim/conversation.hpp` describes -- the
/// same operation the input layer performs with the player's selection.
/// On its first entry (the interpreter's fresh-frame byte, `first_call`
/// here) it writes 300 into the scheduler's wait global and hands the two
/// objects to the manager's slot `0x1c`, the offer: walk the pending
/// records for one whose parties match the pair, apply the reach test,
/// mark it met. A record completed answers 1 -- suspend for the 300 ms
/// just written -- and no record, or a re-entry, answers 0: finish.
///
/// This engine keeps no records: `WaitConvRequest` is a predicate over the
/// world, and the header records why. So what remains of the offer is the
/// test the manager would have applied, `within_conversation_reach`, in
/// either direction since the matcher works out which side matched; when
/// the pair is within reach the call suspends once for 300 ms, as the
/// completed offer does, and otherwise finishes at once. A handle naming
/// nothing is a pair nothing could match.
HostOutcome fn_end_conv_setup(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("EndConvSetup: no world");
  if (!ctx.first_call) return HostOutcome::ok_void();
  if (ctx.count() < 2 || !ctx.arg(0).is_object() || !ctx.arg(1).is_object()) {
    return HostOutcome::ok_void();
  }
  const ObjectId a = ctx.arg(0).as_object().id;
  const ObjectId b = ctx.arg(1).as_object().id;
  // A party naming nothing is out of every reach, so this test is redundant
  // with the one below and a fault that drops it survives as an equivalence;
  // kept because it says what a stale handle means here.
  if (world->find(a) == nullptr || world->find(b) == nullptr) return HostOutcome::ok_void();
  if (!within_conversation_reach(*world, a, b) && !within_conversation_reach(*world, b, a)) {
    return HostOutcome::ok_void();
  }
  HostOutcome out;
  out.status = script::HostStatus::suspend;
  out.suspend_for = 300;
  return out;
}

HostOutcome fn_conv_request(CallContext& ctx) { return conv_request(ctx, false); }
HostOutcome fn_conv_request_pair(CallContext& ctx) { return conv_request(ctx, true); }

struct WaitHostDef {
  CallKind kind;
  const char* name;
  std::uint16_t arity;
  script::HostFn fn;
};

constexpr WaitHostDef kWaitHosts[] = {
    // Descending corpus call frequency.
    {CallKind::free_function, "WaitQueryCountBetween", 4, &fn_query_count_between},  // 208
    {CallKind::free_function, "WaitEmptyQuery", 2, &fn_empty_query},                 // 50
    {CallKind::free_function, "WaitEnvIntBetween", 4, &fn_env_int_between},          // 18
    {CallKind::free_function, "WaitNonEmptyQuery", 2, &fn_non_empty_query},          // 2
    {CallKind::free_function, "WaitObjInQuery", 3, &fn_obj_in_query},                // 1
    {CallKind::free_function, "WaitCommonObjects", 3, &fn_common_objects},           // 1
    {CallKind::free_function, "WaitSettlementCapture", 3, &fn_settlement_capture},   // 14
    {CallKind::free_function, "WaitIdle", 2, &fn_idle},                              // 13
    {CallKind::free_function, "WaitUnitsInArea", 3, &fn_units_in_area},              // 12
    {CallKind::free_function, "WaitHealthBetween", 4, &fn_health_between},           // 8
    // Two overloads of one name, told apart by arity the way the registry does
    // -- `(Obj, Obj, int)` and `(Query, Query, int, Obj&, Obj&)`.
    {CallKind::free_function, "WaitConvRequest", 5, &fn_conv_request_pair},          // 4
    {CallKind::free_function, "WaitConvRequest", 3, &fn_conv_request},               // 3
    // The offer side of the same manager, suspending once. Not a wait by name,
    // and one by mechanism: see the body.
    {CallKind::free_function, "EndConvSetup", 2, &fn_end_conv_setup},                // 1
};

}  // namespace

std::int64_t wait_slice(std::int64_t elapsed, std::int64_t timeout,
                        std::int64_t interval) noexcept {
  // A negative timeout never expires, so the countdown is skipped outright --
  // 0x005eca37 branches past it. The slice is then the bare interval.
  if (timeout < 0) return interval;
  const std::int64_t remaining = timeout - elapsed;
  if (remaining <= 0) return 0;
  return remaining < interval ? remaining : interval;
}

std::size_t register_wait_host(HostRegistry& registry) {
  for (const WaitHostDef& def : kWaitHosts) {
    registry.define(def.kind, def.name, def.arity, def.fn);
  }
  return wait_host_entry_count();
}

std::size_t wait_host_entry_count() noexcept {
  return sizeof(kWaitHosts) / sizeof(kWaitHosts[0]);
}

}  // namespace imperivm::core::sim
