// The 245 bare identifiers the shipped scripts read as globals.
// See include/imperivm/core/sim/globals.hpp for where each family comes from
// and for the disassembly the constant table below was transcribed out of.

#include "imperivm/core/sim/globals.hpp"

#include <algorithm>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/ai_profile.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::HostOutcome;
using script::Value;

/// `gbr.exe`'s constant table, sorted by name so that lookup is a binary
/// search and iteration order is the table's own rather than an allocator's.
///
/// Read out of the executable, not guessed. Every value here is a literal
/// operand of a call to the registrar at `0x0069c3a0` (or, for `SS_IDLE`, to
/// the one at `0x0069bd40`), or the loop index of one of the two registration
/// loops. The families, for orientation:
///
///   * `AI_*` -- two disjoint bitfields sharing one prefix, which is why the
///     values repeat. `AI_COMING`/`AI_LEAVING`/`AI_STAYING` are 1/2/4 and
///     `AI_OWN`/`AI_ALLY`/`AI_ENEMY` are 1/2/4 as well; they are argument
///     positions of different meaning on `Eval` and `GetSquads`, and
///     `AI_FRIENDLY = 3` is `AI_OWN | AI_ALLY`. `AI_ALL` is `0xffff`, which
///     covers both. That the corpus writes `AI_COMING + AI_STAYING` is
///     consistent with disjoint bits and was the only thing recoverable from
///     script alone.
///   * `SF_*` -- squad flags, 1/2/4/16/32.
///   * `hs*` -- hero skills, the index into the 25-entry name table at
///     `0x00824b68`. `DATA\SKILLS.INI`'s sections are the same names.
///   * the unit specials -- the index into the 36-entry table at
///     `0x00824c80`, which is section order in `DATA\UNIT_SPECIALS.INI`.
///   * the races -- `sim/player_host.hpp` records the same eight from the same
///     block, plus the `Rome` alias for 1.
///   * `gs*` -- the six global (stonehenge) spells, 0..5.
///   * `dt*`, `ts*`, `c*Easy/Normal/Hard` and the three `*Count` totals -- real
///     engine constants that no shipped script reads. Kept: see the header.
constexpr GlobalConstant kEngineConstants[] = {
    {"AI_ALL", 65535}, {"AI_ALLY", 2}, {"AI_COMING", 1}, {"AI_ENEMY", 4}, {"AI_FRIENDLY", 3},
    {"AI_LEAVING", 2}, {"AI_NONE", 0}, {"AI_OWN", 1}, {"AI_STAYING", 4}, {"Britain", 5},
    {"Carthage", 2}, {"ES_NONE", 0}, {"Egypt", 6}, {"GS_NONE", 0}, {"Gaul", 0},
    {"Germany", 7}, {"GlobalSpellsCount", 6}, {"HeroSkillsCount", 25}, {"Iberia", 3},
    {"ImperialRome", 4}, {"MaxGAIKAPriority", 100}, {"RepublicanRome", 1}, {"Rome", 1},
    {"SF_ADVCHOOSER", 2}, {"SF_NOAI", 1}, {"SF_PEACEFUL", 4}, {"SF_SENTRIES", 32},
    {"SF_WANTDRUIDS", 16}, {"SS_IDLE", 0}, {"TS_NONE", 0}, {"UNITFLAG_NOAI", 262144},
    {"UnitSpecialsCount", 36}, {"active", 5}, {"attack_skill", 15}, {"bleeding_attack", 14},
    {"cEasy", 0}, {"cHard", 2}, {"cNormal", 1}, {"charge", 20}, {"combat_skill", 8},
    {"cripple", 31}, {"curse", 35}, {"death_blow", 17}, {"defense_skill", 16},
    {"defensive_tactics", 11}, {"deflection", 4}, {"determination", 7},
    {"disease_attack", 25}, {"drain", 1}, {"dtNone", 3}, {"dtPierce", 1}, {"dtSiege", 2},
    {"dtSlash", 0}, {"expertise", 19}, {"ferocity", 2}, {"freedom", 30}, {"gsBloodlust", 4},
    {"gsDivineSacrifice", 3}, {"gsSoothingRain", 2}, {"gsStarvation", 1}, {"gsTribute", 5},
    {"gsWindOfWisdom", 0}, {"healing", 32}, {"hsAdministration", 0}, {"hsAssault", 20},
    {"hsBattleCry", 8}, {"hsCeasefire", 10}, {"hsCharge", 18}, {"hsConcealment", 22},
    {"hsDefensiveCry", 23}, {"hsDiscipline", 5}, {"hsEgoism", 14}, {"hsEpicArmor", 21},
    {"hsEpicAttack", 7}, {"hsEpicEndurance", 4}, {"hsEuphoria", 24}, {"hsFrenzy", 12},
    {"hsHealing", 9}, {"hsLeadership", 6}, {"hsQuickMarch", 3}, {"hsRecovery", 16},
    {"hsRush", 13}, {"hsScout", 19}, {"hsSurvival", 17}, {"hsTeamAttack", 1},
    {"hsTeamDefense", 2}, {"hsVigor", 11}, {"hsWisdom", 15}, {"invisibility", 27},
    {"keen_sight", 33}, {"learning", 28}, {"life_steal", 6}, {"offensive_tactics", 10},
    {"parry", 0}, {"penetration", 12}, {"power_strike", 18}, {"rage", 22},
    {"regeneration", 21}, {"revenge", 23}, {"sneak", 26}, {"spike_armor", 13},
    {"teaching", 29}, {"toughness", 9}, {"trample", 34}, {"triple_strike", 3},
    {"triumph", 24}, {"tsDisabled", 2}, {"tsEnemy", 0}, {"tsOwned", 1},
};

/// The table has to stay sorted for `engine_constant` to find anything, and a
/// name added out of order would silently become unreachable rather than
/// breaking anything visible. So the sort is a compile-time assertion.
consteval bool constants_are_sorted() {
  for (std::size_t i = 1; i < std::size(kEngineConstants); ++i) {
    if (!(kEngineConstants[i - 1].name < kEngineConstants[i].name)) return false;
  }
  return true;
}
static_assert(constants_are_sorted(), "kEngineConstants must be sorted by name");

/// The four `AI.INI` families, in the order `resolve_global` tries them.
///
/// A name is looked for in all four rather than dispatched on its prefix.
/// That is what the original does -- `0x00441160` parses `[SquadStates]`,
/// `[GAIKAStrat]`, `[EconomyScripts]` and `[TacticScripts]` into **one**
/// container and registers the lot from it -- and it means a profile that
/// declares `TS_...` under `[EconomyScripts]` still resolves rather than
/// resolving to nothing for a reason no diagnostic would name.
constexpr AiEnum kAiFamilies[] = {
    AiEnum::squad_state,
    AiEnum::gaika_strategy,
    AiEnum::economy_script,
    AiEnum::tactic_script,
};

/// `AIV_*` and `AIMV_*` share one id space; `AI.INI` declares them in the same
/// `[Vars.*]` sections and `AIVar` keys both by the same integer.
[[nodiscard]] bool is_ai_variable_name(std::string_view name) noexcept {
  return name.starts_with("AIV_") || name.starts_with("AIMV_");
}

// --------------------------------------------------------------------------
// the class constants
// --------------------------------------------------------------------------

/// `gbr.exe`'s identifier sanitiser, `0x0059c060`, character for character.
///
/// The executable calls `isalpha` on the first byte and `isalnum` on the rest
/// and writes `_` wherever the answer is false. Written out here rather than
/// called through `<cctype>` because those two are locale-sensitive and the
/// core is freestanding: a class id with a byte in the 0x80..0xFF range would
/// sanitise one way under one locale and another way under a different one,
/// and a name that depends on the host's locale is not a deterministic name.
/// ASCII is what the 845 shipped ids are, and ASCII is what this decides.
[[nodiscard]] constexpr bool is_alpha(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
[[nodiscard]] constexpr bool is_alnum(char c) noexcept {
  return is_alpha(c) || (c >= '0' && c <= '9');
}

/// Whether `text` survives `0x0059c060` unchanged -- i.e. is already an
/// identifier, and is therefore its own constant's spelling.
[[nodiscard]] bool is_identifier(std::string_view text) noexcept {
  if (text.empty() || !is_alpha(text.front())) return false;
  for (std::size_t i = 1; i < text.size(); ++i) {
    if (!is_alnum(text[i])) return false;
  }
  return true;
}

/// Whether `sanitise(id) == candidate`, without building the sanitised string.
[[nodiscard]] bool sanitises_to(std::string_view id, std::string_view candidate) noexcept {
  if (id.size() != candidate.size() || id.empty()) return false;
  const bool head_ok = is_alpha(id.front()) ? id.front() == candidate.front()
                                            : candidate.front() == '_';
  if (!head_ok) return false;
  for (std::size_t i = 1; i < id.size(); ++i) {
    const char want = is_alnum(id[i]) ? id[i] : '_';
    if (candidate[i] != want) return false;
  }
  return true;
}

}  // namespace

std::string_view class_constant(std::string_view name, const World* world) noexcept {
  // `0x0059c100` registers one string constant per class and nothing else, so
  // the leading `c` and at least one more character are the whole grammar.
  if (name.size() < 2 || name.front() != 'c') return {};
  if (world == nullptr) return {};
  const ClassGraph* graph = world->class_graph();
  if (graph == nullptr) return {};

  const std::string_view id = name.substr(1);
  // The common case: the class id *is* an identifier, so it survives
  // `0x0059c060` unchanged and a direct lookup finds it. 773 of the 845
  // shipped ids are of that shape, and all 27 the corpus reads are.
  //
  // **The `is_identifier` guard is not redundant with the lookup.** Without it
  // this fast path answers for a *candidate the executable could never have
  // minted*: `0x0059c100` keys its constant map by the sanitised spelling, so
  // the class `Rock Large 01` is registered as `cRock_Large_01` and the string
  // `cRock Large 01` names no constant at all -- yet a bare `graph->find` finds
  // the class behind it and would hand back a value. Nothing reaches it through
  // the VM, because the lexer cannot produce an identifier with a space in it;
  // it is reachable through this function, which `sim/globals.hpp` documents as
  // the way to exercise the sanitiser directly. A fast path that answers names
  // the slow path would refuse is not a fast path, it is a second rule.
  //
  // `find`, not `lookup`: `0x004a5080` reads the class's `id` field
  // (`class + 4`, written at `0x0059f604`) and never its `altid` (`class +
  // 0x20`, `0x0059f6b7`), so a class reachable only through an `altid` gets no
  // constant. Three `altid` values are claimed by more than one class anyway,
  // and one collides with a real `id`.
  if (is_identifier(id) && graph->find(id) != kNoClass) return id;

  // The other 72 -- `Rock Large 01`, `Big_Arrow`, `Witch hut`. Their constant
  // is spelled with `_` and so is its value, which is why the answer is the
  // *candidate* rather than the class's own id. Linear, and reached only by a
  // name that is already going to trap if it misses; the graph is 845 entries.
  for (const ClassDefinition& definition : graph->classes()) {
    if (sanitises_to(definition.id, id)) return id;
  }
  return {};
}

namespace {

// --------------------------------------------------------------------------
// the map's own `<group>` names
// --------------------------------------------------------------------------

/// The world's query object over `group`, minting one only if there is none.
///
/// `Group("X")` mints a fresh query object on every call and that is right for
/// a call: the original mints one there too. A *global* is read once per
/// mention -- `while (Q_Army.count != 0)` reads it every turn -- and minting
/// per read would spend an object handle per read, which is unbounded growth
/// in the hashed slot table and shifts every later allocation, i.e. a desync.
/// The original does not have the problem because its prologue mints one
/// object per group per script *start* (`0x005bc8ee`).
///
/// Reusing one is safe because a query object's spec is immutable in practice:
/// nothing in this engine but a test calls `World::mutable_query_spec`, and no
/// host entry point rewrites a spec. The scan is over `World::objects()`,
/// which is ascending id order and holds only live slots, so the object chosen
/// is a function of world state alone and identical on every peer.
[[nodiscard]] ObjectId group_query_object(World& world, std::int32_t group) {
  const QuerySpec want = group_query(group);
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::query) continue;
    const QuerySpec* spec = world.query_spec(slot.id);
    if (spec != nullptr && *spec == want) return slot.id;
  }
  return world.create_query(want);
}

// --------------------------------------------------------------------------
// the ambient command names
// --------------------------------------------------------------------------

/// The command whose script is running, or null.
///
/// `CallContext::script` is the coroutine; `CommandSystem::command_of_script`
/// is the reverse lookup the command domain exposes for exactly this. A null
/// `user`, a world with no command system, and a script that is not a command
/// script all give null, and every caller below refuses on it -- `cmdparam`
/// outside a command has no value, and inventing `""` for it would make a
/// misrouted script look like a command with no parameter.
[[nodiscard]] const Command* running_command(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  if (world == nullptr) return nullptr;
  CommandSystem* commands = command_system(*world);
  if (commands == nullptr) return nullptr;
  return commands->command_of_script(ctx.script);
}

/// The row the interface is asking about, when the caller is not a command
/// script at all.
///
/// A `groupverifier=` runs synchronously under no script record, before
/// there is any command -- it decides whether the button is enabled -- and
/// the shipped verifiers read `cmdparam` and `cmdcost_*` all the same:
/// `verify_cmdcost_building.vs` opens `dest = cmdparam;`. The original writes
/// the row's strings into globals before it runs one; here the row is named
/// by `HostContext::described_command`, the same field the `rollover` family
/// reads, and its `param` and costs answer. Only outside a script: inside
/// one, the running command is the truth and the described row is whatever
/// the mouse happens to be over.
[[nodiscard]] const CommandDef* described_row(CallContext& ctx) noexcept {
  if (ctx.script != script::kNoScript) return nullptr;
  HostContext* context = host_context_of(ctx);
  if (context == nullptr || context->described_command.empty()) return nullptr;
  World* world = world_of(ctx);
  if (world == nullptr) return nullptr;
  CommandSystem* commands = command_system(*world);
  if (commands == nullptr) return nullptr;
  return commands->table().find(context->described_command);
}

HostOutcome fn_cmdparam(CallContext& ctx) {
  if (const Command* command = running_command(ctx)) {
    return HostOutcome::ok_with(Value::string(command->param));
  }
  if (const CommandDef* row = described_row(ctx)) {
    return HostOutcome::ok_with(Value::string(row->param));
  }
  return HostOutcome::failed("cmdparam outside a command script");
}

/// `<cmd costgold>` and friends, as the running command captured them.
template <std::int32_t Command::*Field, std::int32_t CommandDef::*RowField>
HostOutcome fn_cmdcost(CallContext& ctx) {
  if (const Command* command = running_command(ctx)) {
    return HostOutcome::ok_with(Value::integer(command->*Field));
  }
  if (const CommandDef* row = described_row(ctx)) {
    return HostOutcome::ok_with(Value::integer(row->*RowField));
  }
  return HostOutcome::failed("cmdcost_* outside a command script");
}

}  // namespace

std::span<const GlobalConstant> engine_constants() noexcept { return kEngineConstants; }

bool engine_constant(std::string_view name, std::int32_t& out) noexcept {
  const auto* const first = std::begin(kEngineConstants);
  const auto* const last = std::end(kEngineConstants);
  const auto* found = std::lower_bound(
      first, last, name,
      [](const GlobalConstant& entry, std::string_view key) { return entry.name < key; });
  if (found == last || found->name != name) return false;
  out = found->value;
  return true;
}

Result<script::Value> resolve_global(std::string_view name, World* world,
                                     const AiProfile* profile) {
  if (name.empty()) return FormatError::not_found;

  // 1. The engine's own table. World-independent and proven, so it answers
  //    first and nothing loaded from data can shadow it.
  std::int32_t value = 0;
  if (engine_constant(name, value)) return Value::integer(value);

  // 2. `AIV_*` / `AIMV_*`. The id is `EnvSystem`'s to assign, because the same
  //    mapping is what `seed_ai_vars` writes the store through; asking it here
  //    is what makes the reader and the writer incapable of disagreeing.
  if (is_ai_variable_name(name)) {
    if (world == nullptr) return FormatError::not_found;
    const EnvSystem* env = env_of(*world);
    if (env == nullptr) return FormatError::not_found;
    const std::int32_t id = env->ai_var_id(name);
    if (id < 0) return FormatError::not_found;
    return Value::integer(id);
  }

  // 3. The four `AI.INI` families. Nothing resolves them without a profile,
  //    and a world with no profile refuses rather than guessing an ordinal.
  if (profile != nullptr) {
    for (const AiEnum family : kAiFamilies) {
      const std::int32_t constant = profile->constant(family, name);
      if (constant != kUnknownAiConstant) return Value::integer(constant);
    }
  }

  // 4. `c<class id>`, the string constants `0x0059c100` mints from the class
  //    graph. A **string**, not an integer: `0x0069c480` writes the string map
  //    at `+0x34` and `0x006922f0` emits what it finds there as a string
  //    constant, which is also the only shape the call sites accept -- all 127
  //    of them are in a `str` argument position. A world with no class graph
  //    refuses, because there is then nothing that could have minted one.
  if (const std::string_view id = class_constant(name, world); !id.empty()) {
    return Value::string(std::string(id));
  }

  if (world == nullptr) return FormatError::not_found;

  // 5. The map's own `<group>` names. Named objects first: `gbr.exe`'s
  //    `NAMEDOBJ_OVERRIDES_GROUP` says the alias wins and the group becomes
  //    unreachable. No shipped map declares a name as both inside one map
  //    (0 of 1,995), so this settles a case the retail data never presents.
  const std::int32_t named = world->named_objects().find(name);
  if (named != NamedObjectTable::kNoName) {
    return Value::object(script::ObjectRef{kTypeNamedObj, static_cast<std::uint32_t>(named)});
  }

  // `find`, never `intern`. `World::group_index` creates on lookup so that
  // `Group("Oasis_Guards")` can name a group twelve `AddToGroup` calls are
  // about to build; a *global* has no such warrant, and interning here would
  // turn every misspelt identifier into an empty group that resolves forever
  // instead of a trap that names the line. The prologue at `0x005bc640` only
  // ever declares names the map already holds.
  const std::int32_t group = world->groups().find(name);
  if (group != GroupTable::kNoGroup) {
    const ObjectId query = group_query_object(*world, group);
    // A world that could not mint the handle refuses. `kNoObject` is 0, and
    // `{kTypeQuery, 0}` would read as a *valid* handle to object zero rather
    // than as nothing -- the widening this project keeps rediscovering.
    if (query == kNoObject) return FormatError::not_found;
    return Value::object(script::ObjectRef{kTypeQuery, query});
  }

  return FormatError::not_found;
}

std::size_t register_global_hosts(script::HostRegistry& registry) {
  constexpr script::CallKind kFree = script::CallKind::free_function;
  std::size_t defined = 0;
  const auto def = [&](std::string_view name, script::HostFn fn) {
    registry.define(kFree, name, 0, fn);
    ++defined;
  };

  // Arities and return types are the ones `gbr.exe` registers at 0x005b7cff
  // and 0x005b7da5: zero arguments each, `str` for `cmdparam` and `int` for
  // the four costs.
  def("cmdparam", &fn_cmdparam);                          // 41 call sites
  def("cmdcost_pop", &fn_cmdcost<&Command::cost_pop, &CommandDef::cost_pop>);     // 21
  def("cmdcost_gold", &fn_cmdcost<&Command::cost_gold, &CommandDef::cost_gold>);   // 18
  def("cmdcost_food", &fn_cmdcost<&Command::cost_food, &CommandDef::cost_food>);   // 11
  def("cmdcost_stamina", &fn_cmdcost<&Command::cost_stamina, &CommandDef::cost_stamina>);  // 6

  // `cmdwaiting` is declared and deliberately **not** defined. It is "the name
  // of the command waiting to run", i.e. the queue entry behind the running
  // one, and reaching it needs the object whose queue this is --
  // `command_of_script` hands back the command and not its owner, and no
  // public API on `CommandSystem` walks from one to the other. Declaring it
  // makes its two call sites trap by name and arity instead of reading as an
  // unknown global; defining it would need a guess about which queue entry it
  // means. See the report note.
  registry.declare(kFree, "cmdwaiting", 0);

  return defined;
}

std::size_t global_host_entry_count() noexcept { return 5; }

}  // namespace imperivm::core::sim
