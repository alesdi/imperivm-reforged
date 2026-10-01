// The 245 bare identifiers the shipped scripts read as globals.
//
// Two kinds of test live here and they are not interchangeable.
//
//   * **Transcription tests.** The 109 constants in `sim/globals.cpp` were read
//     out of `gbr.exe`'s registration table. A test that restates the same
//     numbers proves nothing about the executable, so the ones below check the
//     properties that a *mistranscription* would break: the table is sorted (a
//     name out of order becomes unreachable and nothing else notices), the two
//     loop-registered families are dense permutations of `0..n-1` with their
//     declared `*Count` totals, and the eight races agree with
//     `sim/player_host.hpp`, which read the same block of the executable
//     independently. Those are cross-checks, not restatements.
//
//   * **Refusal tests.** Half the value of this table is what it does *not*
//     answer. `cBuilding`, `revitalize`, an unloaded `SS_` family and an
//     unseeded `AIV_` all have to refuse, because a plausible zero for any of
//     them is a divergence the conformance harness would have to chase while a
//     refusal is a name and a line number. Those tests are the ones that will
//     fail first if somebody decides to be helpful.

#include <cstdint>
#include <span>
#include <string_view>

#include <string>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/ai_profile.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/world/map.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// `DATA\AI\AI.INI`'s four enum sections and a slice of `[Vars.All]`, in the
/// shipped order. Cut down; the ordering is what matters here.
constexpr std::string_view kProfileIni =
    "[SquadStates]\n"
    "SS_Approach\n"
    "SS_Wait\n"
    "SS_ApproachWait\n"
    "\n"
    "[GAIKAStrat]\n"
    "GS_EnterSettlement\n"
    "GS_Capture\n"
    "\n"
    "[EconomyScripts]\n"
    "ES_Stronghold\n"
    "ES_Village\n"
    "\n"
    "[TacticScripts]\n"
    "TS_AttackAtWill\n"
    "TS_GaulTactic\n"
    "\n"
    "[Vars.All]\n"
    "AIV_LogTrain=0\n"
    "AIV_Research=1\n"
    "AIMV_NoAttack=0\n";

/// The value a name resolves to, or a sentinel that no constant can be.
constexpr std::int32_t kRefused = -424242;

[[nodiscard]] std::int32_t resolved(std::string_view name, World* world,
                                    const AiProfile* profile) {
  const Result<script::Value> value = resolve_global(name, world, profile);
  if (!value.ok()) return kRefused;
  if (!value.value().is_integer()) return kRefused;
  return value.value().as_integer();
}

/// Whether `name` resolves to **nothing at all**.
///
/// `resolved` above cannot answer this and never could: it maps both "refused"
/// and "resolved to something that is not an integer" onto `kRefused`. That was
/// harmless while every resolution step returned an integer, and stopped being
/// harmless the moment `c<class id>` started returning a **string** and the
/// map's `<group>` names started returning **object handles** -- from then on a
/// refusal test written with `resolved` would have gone on passing if the name
/// had begun resolving, which is the exact failure the refusal tests exist to
/// catch. Every refusal below is asserted through this instead.
[[nodiscard]] bool refuses(std::string_view name, World* world, const AiProfile* profile) {
  return !resolve_global(name, world, profile).ok();
}

}  // namespace

// --------------------------------------------------------------------------
// the engine's own table
// --------------------------------------------------------------------------

TEST(engine_constants_are_sorted_and_searchable) {
  const std::span<const GlobalConstant> table = engine_constants();
  CHECK(table.size() == 109);
  for (std::size_t i = 1; i < table.size(); ++i) {
    CHECK(table[i - 1].name < table[i].name);
  }
  // Every entry has to be findable through the binary search, or a name could
  // sit in the table and still refuse.
  for (const GlobalConstant& entry : table) {
    std::int32_t value = 0;
    CHECK(engine_constant(entry.name, value));
    CHECK(value == entry.value);
  }
  std::int32_t ignored = 0;
  CHECK(!engine_constant("", ignored));
  CHECK(!engine_constant("AI_", ignored));
  CHECK(!engine_constant("AI_COMINGS", ignored));
  // The executable's table is a `std::map<std::string, int>`, so its lookup is
  // case-sensitive and so is this one.
  CHECK(!engine_constant("ai_coming", ignored));
}

TEST(the_two_ai_bitfields_are_disjoint_bits) {
  // `0x00443480` registers nine `AI_*` constants in one run. They are two
  // bitfields sharing a prefix: occupancy (`AI_COMING`/`LEAVING`/`STAYING`)
  // and ownership (`AI_OWN`/`ALLY`/`ENEMY`), each 1/2/4, with `AI_FRIENDLY`
  // the union of the first two ownership bits and `AI_ALL` every bit. That the
  // corpus writes `AI_COMING + AI_STAYING` is consistent with disjoint bits
  // and was the only thing script alone could establish.
  const auto value = [](const char* name) {
    std::int32_t out = 0;
    return engine_constant(name, out) ? out : kRefused;
  };
  CHECK(value("AI_NONE") == 0);
  CHECK(value("AI_COMING") == 1);
  CHECK(value("AI_LEAVING") == 2);
  CHECK(value("AI_STAYING") == 4);
  CHECK((value("AI_COMING") & value("AI_LEAVING") & value("AI_STAYING")) == 0);
  CHECK(value("AI_OWN") == 1);
  CHECK(value("AI_ALLY") == 2);
  CHECK(value("AI_ENEMY") == 4);
  CHECK(value("AI_FRIENDLY") == (value("AI_OWN") | value("AI_ALLY")));
  CHECK(value("AI_ALL") == 0xffff);
  // Squad flags are the same shape, and `SF_NOAI` must not collide with
  // `UNITFLAG_NOAI`: they are flags on different things.
  CHECK(value("SF_NOAI") == 1);
  CHECK(value("SF_ADVCHOOSER") == 2);
  CHECK(value("SF_PEACEFUL") == 4);
  CHECK(value("UNITFLAG_NOAI") == 262144);
  CHECK(value("MaxGAIKAPriority") == 100);
}

TEST(hero_skills_and_unit_specials_are_dense_index_spaces) {
  // Both families are registered by a loop over a pointer table --
  // `0x005317bf` over 25 names, `0x005dfdff` over 36 -- with the loop counter
  // as the value, and each loop registers its own total afterwards. So the
  // values must be exactly `0..n-1` with no gap and no repeat, and the totals
  // must agree. A transcription that dropped or duplicated a name breaks this
  // without breaking any single lookup.
  const auto is_dense = [](std::string_view prefix, std::string_view count_name) {
    std::int32_t total = 0;
    if (!engine_constant(count_name, total)) return false;
    if (total <= 0 || total > 64) return false;
    bool seen[64] = {};
    std::int32_t found = 0;
    for (const GlobalConstant& entry : engine_constants()) {
      if (!entry.name.starts_with(prefix)) continue;
      if (entry.value < 0 || entry.value >= total) return false;
      if (seen[entry.value]) return false;
      seen[entry.value] = true;
      ++found;
    }
    return found == total;
  };
  CHECK(is_dense("hs", "HeroSkillsCount"));
  CHECK(is_dense("gs", "GlobalSpellsCount"));

  // The unit specials have no shared prefix, so they are checked by name. The
  // order is `DATA\UNIT_SPECIALS.INI`'s 36 section headings, which is the
  // second source that makes the value an index into that file rather than an
  // arbitrary id: `[Parry]` first, `[Curse]` last.
  const auto value = [](const char* name) {
    std::int32_t out = 0;
    return engine_constant(name, out) ? out : kRefused;
  };
  CHECK(value("parry") == 0);
  CHECK(value("sneak") == 26);
  CHECK(value("invisibility") == 27);
  CHECK(value("teaching") == 29);
  CHECK(value("cripple") == 31);
  CHECK(value("healing") == 32);
  CHECK(value("curse") == 35);
  CHECK(value("UnitSpecialsCount") == 36);
  // `revitalize` is used by three live `DATA\SUBAI` scripts and is in neither
  // the executable's 36 nor `UNIT_SPECIALS.INI`'s 36 sections. It refuses; see
  // `AIV_NoRepair` in docs/formats/ai-ini.md for the same shape of finding.
  // Asserted on the lookup's own bool rather than on a sentinel value: this is
  // `engine_constant`, which cannot return anything but an integer, so the two
  // agree here -- but `refuses` is the only spelling that stays true when a
  // later resolution step starts answering with a string.
  std::int32_t unused = 0;
  CHECK(!engine_constant("revitalize", unused));
  CHECK(refuses("revitalize", nullptr, nullptr));
}

TEST(the_races_agree_with_the_player_domain) {
  // `sim/player_host.hpp` read the same block at `0x005b70b2` on its own. The
  // two transcriptions have to agree, and this is the only place they meet.
  const auto value = [](const char* name) {
    std::int32_t out = 0;
    return engine_constant(name, out) ? out : kRefused;
  };
  CHECK(value("Gaul") == static_cast<std::int32_t>(Race::gaul));
  CHECK(value("RepublicanRome") == static_cast<std::int32_t>(Race::republican_rome));
  CHECK(value("Carthage") == static_cast<std::int32_t>(Race::carthage));
  CHECK(value("Iberia") == static_cast<std::int32_t>(Race::iberia));
  CHECK(value("ImperialRome") == static_cast<std::int32_t>(Race::imperial_rome));
  CHECK(value("Britain") == static_cast<std::int32_t>(Race::britain));
  CHECK(value("Egypt") == static_cast<std::int32_t>(Race::egypt));
  CHECK(value("Germany") == static_cast<std::int32_t>(Race::germany));
  // `Rome` is an alias for 1 that the executable registers beside
  // `RepublicanRome`; `race_to_name` spells the long one on the way out.
  CHECK(value("Rome") == value("RepublicanRome"));
  for (const char* name : {"Gaul", "RepublicanRome", "Carthage", "Iberia", "ImperialRome",
                           "Britain", "Egypt", "Germany"}) {
    CHECK(race_from_name(name) == value(name));
  }
}

// --------------------------------------------------------------------------
// resolution
// --------------------------------------------------------------------------

TEST(engine_constants_resolve_without_a_world_or_a_profile) {
  // They are compiled into the executable, so nothing has to be loaded for a
  // script to read one. A null world is a real case: the tests run host
  // functions with `user == nullptr` on purpose.
  CHECK(resolved("Carthage", nullptr, nullptr) == 2);
  CHECK(resolved("AI_STAYING", nullptr, nullptr) == 4);
  CHECK(resolved("hsEuphoria", nullptr, nullptr) == 24);
  CHECK(resolved("gsBloodlust", nullptr, nullptr) == 4);
  // The four sentinels are in the executable's table, which is what settles
  // the numbering origin `docs/formats/ai-ini.md` recorded as inferred: the
  // sentinel is 0 and the declared entries start at 1.
  CHECK(resolved("SS_IDLE", nullptr, nullptr) == 0);
  CHECK(resolved("GS_NONE", nullptr, nullptr) == 0);
  CHECK(resolved("ES_NONE", nullptr, nullptr) == 0);
  CHECK(resolved("TS_NONE", nullptr, nullptr) == 0);
}

TEST(the_ai_profile_families_refuse_until_a_profile_is_loaded) {
  CHECK(refuses("SS_Approach", nullptr, nullptr));
  CHECK(refuses("GS_Capture", nullptr, nullptr));
  CHECK(refuses("ES_Village", nullptr, nullptr));
  CHECK(refuses("TS_GaulTactic", nullptr, nullptr));

  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());
  const AiProfile& profile = parsed.value();

  CHECK(resolved("SS_Approach", nullptr, &profile) == 1);
  CHECK(resolved("SS_Wait", nullptr, &profile) == 2);
  CHECK(resolved("SS_ApproachWait", nullptr, &profile) == 3);
  CHECK(resolved("GS_EnterSettlement", nullptr, &profile) == 1);
  CHECK(resolved("ES_Village", nullptr, &profile) == 2);
  CHECK(resolved("TS_GaulTactic", nullptr, &profile) == 2);
  // A name the profile does not declare still refuses with one loaded.
  CHECK(refuses("SS_Catapult", nullptr, &profile));

  // The sentinels answer the same with a profile as without: the executable's
  // table and the profile's own sentinel agree on 0, which is the cross-check
  // that the two numberings are the same numbering.
  CHECK(resolved("SS_IDLE", nullptr, &profile) == 0);
  CHECK(profile.constant(AiEnum::squad_state, "SS_IDLE") == 0);
}

TEST(ai_variables_resolve_to_the_id_the_store_is_keyed_by) {
  // The `AIV_*` id is not a constant anywhere -- it is an index into
  // `AiVarStore`, and `EnvSystem` owns both ends of it. Resolving the name
  // through `ai_var_id` is what makes the reader and the writer incapable of
  // disagreeing, so this test reads a variable back through the id the global
  // gave it rather than through a literal.
  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());

  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));
  env.seed_ai_vars(3, parsed.value(), AiDifficulty::none);

  const std::int32_t research = resolved("AIV_Research", &world, nullptr);
  CHECK(research != kRefused);
  CHECK(env.ai_vars().get(3, research) == 1);
  CHECK(resolved("AIV_LogTrain", &world, nullptr) == env.ai_var_id("AIV_LogTrain"));
  // `AIMV_*` share the id space: `AI.INI` declares them in the same sections.
  CHECK(resolved("AIMV_NoAttack", &world, nullptr) == env.ai_var_id("AIMV_NoAttack"));

  // Declared by no profile, read by one shipped script. It refuses rather than
  // resolving to an id nothing seeded -- `docs/formats/ai-ini.md` records the
  // engine's default for an undeclared variable as unknown, and zero as the
  // guess to distrust.
  CHECK(refuses("AIV_NoRepair", &world, nullptr));
}

TEST(ai_variables_refuse_without_an_env_system) {
  World bare;
  CHECK(refuses("AIV_Research", &bare, nullptr));
  CHECK(refuses("AIV_Research", nullptr, nullptr));
  // An env system whose name table was never seeded refuses too. Reading zero
  // here would put every AI player's tuning on variable 0.
  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));
  CHECK(refuses("AIV_Research", &world, nullptr));
}

TEST(the_names_that_are_not_constants_refuse) {
  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());
  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));
  env.seed_ai_vars(1, parsed.value(), AiDifficulty::none);

  // The `c*` class ids refuse **on a world with no class graph**, and that is
  // now the whole of what they have in common with the names below: they are
  // minted from the graph (`gbr.exe` `0x0059c100`), so a world that never
  // loaded one has nothing that could have minted them. Reading a class name
  // out of thin air here would make a tutorial select a set nothing declared.
  //
  // The precondition is asserted rather than assumed. Without this line the
  // loop below would keep passing for the wrong reason on the day somebody
  // gives `World` a default graph, and the test would then be proving that six
  // class names happen to be absent from it.
  REQUIRE(world.class_graph() == nullptr);
  for (const char* name : {"cBuilding", "cMilitary", "cHero", "cUnit", "cRanged", "cSentry"}) {
    CHECK(refuses(name, &world, &parsed.value()));
    CHECK(class_constant(name, &world).empty());
  }
  // And with no world at all, which is how `resolve_global` is called before a
  // session exists.
  CHECK(class_constant("cBuilding", nullptr).empty());

  // `owner` and `this` are one use each and neither is a global: `owner` is the
  // first parameter of every `DATA\ITEMSCRIPTS` entry point and `this` is an
  // ordinary assignable local in the same scripts (`this = owner.AsUnit();`).
  // The compiler resolves both as locals wherever they are declared, and where
  // they are not, refusing is right.
  CHECK(refuses("owner", &world, &parsed.value()));
  CHECK(refuses("this", &world, &parsed.value()));

  // The seven parenthesis-less calls are free functions the compiler resolves
  // before it asks for a global. Answering them here would shadow a real call
  // with a constant, so none of them is in the table.
  for (const char* name : {"GetTime", "GetMapRect", "AIGetPlayer", "GAIKACount", "MapSize",
                           "MaxSetIdx", "Breakpoint"}) {
    CHECK(refuses(name, &world, &parsed.value()));
  }

  CHECK(refuses("", &world, &parsed.value()));
  CHECK(refuses("NoSuchGlobal", &world, &parsed.value()));
}

// --------------------------------------------------------------------------
// the host
// --------------------------------------------------------------------------

TEST(world_host_answers_globals_and_carries_the_profile) {
  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());

  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));
  env.seed_ai_vars(1, parsed.value(), AiDifficulty::none);

  WorldHost host{world};
  CHECK(host.ai_profile() == nullptr);

  const auto value_of = [&](const char* name) {
    const Result<script::Value> r = host.global(name);
    return r.ok() && r.value().is_integer() ? r.value().as_integer() : kRefused;
  };

  CHECK(value_of("Germany") == 7);
  CHECK(value_of("AIV_Research") == env.ai_var_id("AIV_Research"));
  // Refused, and asserted as a refusal: `value_of` folds "not an integer" into
  // the same sentinel, so the host's own `ok()` is what is checked.
  CHECK(!host.global("SS_Approach").ok());
  CHECK(value_of("SS_Approach") == kRefused);

  host.set_ai_profile(&parsed.value());
  CHECK(host.ai_profile() == &parsed.value());
  CHECK(value_of("SS_Approach") == 1);
  CHECK(value_of("GS_Capture") == 2);
}

TEST(the_ambient_command_names_are_zero_argument_functions) {
  // `gbr.exe` registers them in the host function table, not the constant map:
  // `0x005b7cff` binds `cmdparam` with return type `0x0b` (`str`) and zero
  // arguments, `0x005b7da5` binds `cmdcost_gold` with return type `0x01`
  // (`int`) and zero arguments. The VS compiler resolves a bare name as a
  // zero-arity free function before it asks for a global, which is why they are
  // reachable at all -- `Host::global` is not told which script is asking, and
  // the answer depends entirely on that.
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  const std::size_t defined = register_global_hosts(registry);
  CHECK(defined == global_host_entry_count());

  const auto entry = [&](const char* name) -> const script::HostEntry* {
    const std::uint32_t index = registry.find(script::CallKind::free_function, name, 0);
    return index == script::kUnresolvedHost ? nullptr : &registry.entry(index);
  };
  for (const char* name : {"cmdparam", "cmdcost_gold", "cmdcost_food", "cmdcost_pop",
                           "cmdcost_stamina"}) {
    const script::HostEntry* found = entry(name);
    REQUIRE(found != nullptr);
    CHECK(found->fn != nullptr);
  }
  // Declared and deliberately undefined: reaching the queue entry behind the
  // running command needs the object whose queue it is, and
  // `command_of_script` hands back the command without its owner. Declared so
  // that its two call sites trap by name instead of reading as an unknown
  // global.
  const script::HostEntry* waiting = entry("cmdwaiting");
  REQUIRE(waiting != nullptr);
  CHECK(waiting->fn == nullptr);

  // A null `user` must refuse, not dereference. Every entry point in the core
  // is held to this and these are no exception.
  script::Value slot;
  for (const char* name : {"cmdparam", "cmdcost_gold"}) {
    const script::HostEntry* found = entry(name);
    REQUIRE(found != nullptr);
    script::CallContext ctx;
    ctx.arguments = std::span<script::Value>(&slot, 0);
    ctx.user = nullptr;
    ctx.name = name;
    const script::HostOutcome outcome = found->fn(ctx);
    CHECK(outcome.status == script::HostStatus::error);
  }
}

TEST(the_ambient_command_names_read_the_running_command) {
  World world;
  CommandSystem commands;
  REQUIRE(world.add_system(&commands));

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_global_hosts(registry);
  const std::uint32_t index = registry.find(script::CallKind::free_function, "cmdparam", 0);
  REQUIRE(index != script::kUnresolvedHost);
  const script::HostFn cmdparam = registry.entry(index).fn;
  REQUIRE(cmdparam != nullptr);

  HostContext context;
  context.world = &world;

  script::Value slot;
  script::CallContext ctx;
  ctx.arguments = std::span<script::Value>(&slot, 0);
  ctx.user = &context;
  ctx.name = "cmdparam";
  ctx.script = script::kNoScript;

  // No command is running under this script, and there is no value for
  // `cmdparam` to have. An empty string would be indistinguishable from a
  // command whose `<cmd param>` is absent, so it refuses instead.
  CHECK(cmdparam(ctx).status == script::HostStatus::error);
}

// --------------------------------------------------------------------------
// the class constants, and the sanitiser behind them
// --------------------------------------------------------------------------
//
// `gbr.exe` `0x0059c100` mints one **string** constant per class, named
// `c` + the class id put through the identifier sanitiser at `0x0059c060`.
// These tests are the positive half of the refusal test above: that one proves
// a world with no graph answers nothing, and these prove a world with one
// answers the right thing -- and, in the two cases the sanitiser touches,
// something that is deliberately *not* a class name.

namespace {

/// Five classes chosen for what they do to the sanitiser, not for what they
/// are. `Building` and `Military` survive it untouched (773 of the 845 shipped
/// ids do); `Rock Large 01` and `Witch hut` have a space in the middle, which
/// is the common non-identifier case; `1EDrop1` starts with a digit, which is
/// the only case where the head rule (`isalpha`) and the tail rule (`isalnum`)
/// disagree -- the seven shipped `<group name="1EDrop1">`-style names are why
/// the executable has two rules rather than one.
[[nodiscard]] ClassGraph constant_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Building" cpp_class="CVXBuilding" parent=""/>)"),
            "test_globals.cpp");
  graph.add(bytes_of(R"(<class id="Military" cpp_class="CVXUnit" parent="Building"/>)"),
            "test_globals.cpp");
  graph.add(bytes_of(R"(<class id="Rock Large 01" cpp_class="CVXDecor" parent="Building"/>)"),
            "test_globals.cpp");
  graph.add(bytes_of(R"(<class id="Witch hut" cpp_class="CVXDecor" parent="Building"/>)"),
            "test_globals.cpp");
  graph.add(bytes_of(R"(<class id="1EDrop1" cpp_class="CVXDecor" parent="Building"/>)"),
            "test_globals.cpp");
  graph.link();
  return graph;
}

/// The string `name` resolves to, or empty when it does not resolve to one.
[[nodiscard]] std::string resolved_string(std::string_view name, World* world) {
  const Result<script::Value> value = resolve_global(name, world, nullptr);
  if (!value.ok() || !value.value().is_string()) return {};
  return value.value().as_string();
}

}  // namespace

TEST(a_class_constant_is_the_class_id_and_arrives_as_a_string) {
  ClassGraph graph = constant_graph();
  World world;
  world.set_class_graph(&graph);

  // The common case: the id is already an identifier, so the constant is `c`
  // plus the id and its value is the id.
  CHECK(class_constant("cBuilding", &world) == "Building");
  CHECK(class_constant("cMilitary", &world) == "Military");
  CHECK(resolved_string("cBuilding", &world) == "Building");
  CHECK(resolved_string("cMilitary", &world) == "Military");

  // **A string, not an integer.** All 127 call sites in the corpus are in a
  // `str` argument position (`ClassPlayerObjs(str, int)`, `Count(int, str)`),
  // and an integer there would be a type error the VM cannot recover from.
  // Checked on the `Value` itself because the two are not interchangeable.
  const Result<script::Value> building = resolve_global("cBuilding", &world, nullptr);
  REQUIRE(building.ok());
  CHECK(building.value().is_string());
  CHECK(!building.value().is_integer());
  CHECK(!building.value().is_object());
}

TEST(the_sanitiser_spells_a_constant_the_way_the_executable_does) {
  ClassGraph graph = constant_graph();
  World world;
  world.set_class_graph(&graph);

  // A space is not `isalnum`, so it becomes `_` -- in the constant's name and,
  // because the value is the constant's own spelling, in the value too. The
  // value is therefore a string **no class is called**, and that is the
  // original's behaviour rather than a rounding of it.
  CHECK(class_constant("cRock_Large_01", &world) == "Rock_Large_01");
  CHECK(class_constant("cWitch_hut", &world) == "Witch_hut");
  CHECK(graph.find("Rock_Large_01") == kNoClass);
  CHECK(graph.find("Witch hut") != kNoClass);

  // The space itself is not a name the engine ever mints: the constant is
  // reached by the sanitised spelling and by nothing else.
  CHECK(class_constant("cRock Large 01", &world).empty());

  // The head rule is `isalpha` and the tail rule is `isalnum`, and `1EDrop1` is
  // the only shape that can tell them apart: a leading digit is alnum but not
  // alpha, so it is replaced and every later digit is kept.
  CHECK(class_constant("c_EDrop1", &world) == "_EDrop1");
  CHECK(class_constant("c1EDrop1", &world).empty());

  // Length is part of the match. `sanitises_to` compares character for
  // character over equal-length strings, so no prefix of a class id resolves.
  CHECK(class_constant("cBuild", &world).empty());
  CHECK(class_constant("cBuildingX", &world).empty());
}

TEST(a_class_constant_needs_the_leading_c_and_a_class_behind_it) {
  ClassGraph graph = constant_graph();
  World world;
  world.set_class_graph(&graph);

  // `c` alone is not a constant: the grammar is the letter plus at least one
  // more character, which is what `0x0059c100` registers.
  CHECK(class_constant("c", &world).empty());
  CHECK(class_constant("", &world).empty());
  // A class name without the prefix is not a constant either. `Building` is a
  // real class and still resolves nowhere, which is the property that keeps
  // 845 class ids out of the global namespace.
  CHECK(class_constant("Building", &world).empty());
  CHECK(refuses("Building", &world, nullptr));
  CHECK(refuses("Military", &world, nullptr));
  // And a `c` name with nothing behind it stays a trap rather than becoming an
  // empty string, which is what a script would silently select nothing with.
  CHECK(class_constant("cNoSuchClass", &world).empty());
  CHECK(refuses("cNoSuchClass", &world, nullptr));
  // Case is not folded: the engine's constant map is a `std::map<std::string>`.
  CHECK(class_constant("cbuilding", &world).empty());
  CHECK(class_constant("CBuilding", &world).empty());
}

TEST(a_class_reachable_only_through_an_altid_gets_no_constant) {
  // `0x004a5080` reads the class's `id` field (`class + 4`) and never its
  // `altid` (`class + 0x20`), so the constant space is the `id` space alone.
  // Three shipped `altid` values are claimed by more than one class and one
  // collides with a real `id`, so minting constants from `altid` too would put
  // a name in the table that cannot say which class it means.
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Building" cpp_class="CVXBuilding" parent=""/>)"),
            "test_globals.cpp");
  graph.add(bytes_of(
                R"(<class id="Barrack" altid="Kaserne" cpp_class="CVXBuilding" parent="Building"/>)"),
            "test_globals.cpp");
  graph.link();
  World world;
  world.set_class_graph(&graph);

  // The precondition: `lookup` finds the class through the alias and `find`
  // does not. Without this the test below would pass on a graph that had never
  // read the `altid` at all.
  REQUIRE(graph.lookup("Kaserne") != kNoClass);
  REQUIRE(graph.find("Kaserne") == kNoClass);

  CHECK(class_constant("cBarrack", &world) == "Barrack");
  CHECK(class_constant("cKaserne", &world).empty());
  CHECK(refuses("cKaserne", &world, nullptr));
}

// --------------------------------------------------------------------------
// the map's own `<group>` names
// --------------------------------------------------------------------------

namespace {

/// Three objects, one alias, one army group, and one name that is both.
///
/// The last is the case no shipped map presents -- 0 of 1,995 names carry both
/// types inside one map -- so it is here rather than in the corpus, which is
/// the only place a rule nothing exercises can be pinned.
constexpr std::string_view kGroupMap = R"(<mapobject>
  <scriptobj class="Building" num="0" x="100" y="100" player="1" flags="0x80800001"/>
  <scriptobj class="Building" num="1" x="200" y="100" player="1" flags="0x80800001"/>
  <scriptobj class="Building" num="2" x="300" y="100" player="2" flags="0x80800002"/>
  <group name="Village2" type="0"><obj num="0"/></group>
  <group name="T_RomanArmy" type="1"><obj num="1"/><obj num="2"/></group>
  <group name="Both" type="0"><obj num="1"/></group>
  <group name="Both" type="1"><obj num="2"/></group>
</mapobject>)";

[[nodiscard]] World& populate(World& world, const ClassGraph& graph,
                              const MapObjectList& map) {
  world.set_class_graph(&graph);
  (void)world.populate_from_map(map, graph, nullptr);
  return world;
}

}  // namespace

TEST(a_named_object_resolves_to_its_entry_and_a_group_to_a_query) {
  ClassGraph graph = constant_graph();
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kGroupMap));
  REQUIRE(map.ok());
  World world;
  populate(world, graph, map.value());

  REQUIRE(world.named_objects().find("Village2") != NamedObjectTable::kNoName);
  REQUIRE(world.groups().find("T_RomanArmy") != GroupTable::kNoGroup);

  // A `<group type="0">` is a `NamedObj`, and the id it carries is the index
  // into the table -- not the bound object's id. The binding outlives its
  // object so that `NamedObj::IsDead` has something to answer about, which is
  // only possible if the handle names the entry.
  const Result<script::Value> named = resolve_global("Village2", &world, nullptr);
  REQUIRE(named.ok());
  REQUIRE(named.value().is_object());
  CHECK(named.value().as_object().type == kTypeNamedObj);
  CHECK(named.value().as_object().id ==
        static_cast<std::uint32_t>(world.named_objects().find("Village2")));
  // The entry index and the object id are different numbers here, so a handle
  // carrying the wrong one is visible.
  CHECK(world.named_objects().object("Village2") !=
        static_cast<ObjectId>(named.value().as_object().id));

  // A `<group type="1">` is the same `Query` value `Group("...")` returns.
  const Result<script::Value> group = resolve_global("T_RomanArmy", &world, nullptr);
  REQUIRE(group.ok());
  REQUIRE(group.value().is_object());
  CHECK(group.value().as_object().type == kTypeQuery);
  const ObjectId query = static_cast<ObjectId>(group.value().as_object().id);
  CHECK(query != kNoObject);
  const QuerySpec* spec = world.query_spec(query);
  REQUIRE(spec != nullptr);
  CHECK(*spec == group_query(world.groups().find("T_RomanArmy")));
}

TEST(a_named_object_beats_a_group_of_the_same_name) {
  // `NAMEDOBJ_OVERRIDES_GROUP`: the editor warns that a name used for both
  // makes *the group* unreachable from sequence scripts. No shipped map does
  // it, so this is the only place the rule is observable at all.
  ClassGraph graph = constant_graph();
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kGroupMap));
  REQUIRE(map.ok());
  World world;
  populate(world, graph, map.value());

  // The precondition: both tables really do hold the name.
  REQUIRE(world.named_objects().find("Both") != NamedObjectTable::kNoName);
  REQUIRE(world.groups().find("Both") != GroupTable::kNoGroup);

  const Result<script::Value> both = resolve_global("Both", &world, nullptr);
  REQUIRE(both.ok());
  REQUIRE(both.value().is_object());
  CHECK(both.value().as_object().type == kTypeNamedObj);
}

TEST(a_group_global_reuses_one_query_object_per_group) {
  // `Group("X")` mints a fresh query object per call and that is right for a
  // call. A *global* is read once per mention -- `while (Q_Army.count != 0)`
  // reads it every turn -- so minting per read would spend a handle per read:
  // unbounded growth in the hashed slot table, every later allocation shifted,
  // and therefore a desync against a peer that read it a different number of
  // times.
  ClassGraph graph = constant_graph();
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kGroupMap));
  REQUIRE(map.ok());
  World world;
  populate(world, graph, map.value());

  const Result<script::Value> first = resolve_global("T_RomanArmy", &world, nullptr);
  REQUIRE(first.ok());
  const std::size_t after_first = world.objects().size();
  const std::uint32_t handle = first.value().as_object().id;

  for (int i = 0; i < 16; ++i) {
    const Result<script::Value> again = resolve_global("T_RomanArmy", &world, nullptr);
    REQUIRE(again.ok());
    CHECK(again.value().as_object().id == handle);
  }
  CHECK(world.objects().size() == after_first);
}

TEST(an_unknown_name_traps_rather_than_interning_a_group) {
  // `World::group_index` creates on lookup, because `Group("Oasis_Guards")` has
  // to be able to name a group that twelve later `AddToGroup` calls will build.
  // A global has no such warrant: interning here would turn every misspelt
  // identifier into an empty group that resolves forever, and a script reading
  // it would select nothing and say nothing. `resolve_global` uses `find`.
  ClassGraph graph = constant_graph();
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kGroupMap));
  REQUIRE(map.ok());
  World world;
  populate(world, graph, map.value());

  const std::size_t groups_before = world.groups().size();
  const std::size_t objects_before = world.objects().size();

  CHECK(refuses("T_RomanArmyy", &world, nullptr));
  CHECK(refuses("village2", &world, nullptr));  // case matters
  CHECK(refuses("NO_NothingLikeThis", &world, nullptr));

  CHECK(world.groups().size() == groups_before);
  CHECK(world.groups().find("T_RomanArmyy") == GroupTable::kNoGroup);
  // And no query object was minted for the misses either.
  CHECK(world.objects().size() == objects_before);
}

TEST(the_engines_own_table_is_searched_before_any_loaded_data) {
  // The compiled-in table is first because it is the only source that cannot
  // vary with loaded data. Nothing a map or a class file brings in may shadow
  // it. On retail data the order is unobservable -- 0 of the 1,995 shipped
  // `<group>` names collide with one of the 109 constants and 0 are claimed by
  // the `c*` rule -- so, like the rule above, it is only checkable here.
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Building" cpp_class="CVXBuilding" parent=""/>)"),
            "test_globals.cpp");
  // A class whose constant would be `cGermany`... which is not a collision.
  // The collision that matters is a *group* named after an engine constant.
  graph.link();

  constexpr std::string_view kShadowMap = R"(<mapobject>
  <scriptobj class="Building" num="0" x="100" y="100" player="1" flags="0x80800001"/>
  <group name="Germany" type="0"><obj num="0"/></group>
  <group name="AI_ENEMY" type="1"><obj num="0"/></group>
</mapobject>)";
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kShadowMap));
  REQUIRE(map.ok());
  World world;
  populate(world, graph, map.value());

  // The preconditions: the map really did claim both names.
  REQUIRE(world.named_objects().find("Germany") != NamedObjectTable::kNoName);
  REQUIRE(world.groups().find("AI_ENEMY") != GroupTable::kNoGroup);

  // And the engine's own values win, as integers, not as handles.
  std::int32_t germany = 0;
  REQUIRE(engine_constant("Germany", germany));
  CHECK(resolved("Germany", &world, nullptr) == germany);
  std::int32_t aggressive = 0;
  REQUIRE(engine_constant("AI_ENEMY", aggressive));
  CHECK(resolved("AI_ENEMY", &world, nullptr) == aggressive);
}
