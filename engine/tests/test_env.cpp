// The keyed stores: the environment, the AI variables, and research.
//
// No game data. Every fixture below reproduces a shape that occurs in the
// shipped files, and the comment beside it says which one -- `DATA\COMMANDS\
// ARENA.XML` for the research `param` grammar, `DATA\AI\AI.INI` for the AI
// variable semantics, `DATA\SUBAI\*.VS` for what the scripts do with the
// result, and `gbr.exe`'s own string pool for the shapes only the binary
// records.
//
// Two of these tests exist because getting them wrong would be invisible:
//
//   * `env_store_iteration_order_is_content_determined` -- iteration order is
//     world state (`sim/system.hpp` rule 3), and a hash map keyed by string
//     would pass every other test in this file while producing a different
//     world hash on a peer that inserted the same keys in a different order.
//
//   * `env_reads_an_int_back_out_of_a_string_write` -- the retail store holds
//     strings, and the arena training upgrades write `SetsPlr, maxtrainlevel,
//     4` with `EnvWriteString` and read it back with `EnvReadInt`. A design
//     with separate int and string namespaces disables every one of them and
//     nothing else notices.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/ai_profile.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "builder.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// `BaseTownhall`, as `test_economy.cpp` builds it, reduced to what the
/// research ledger needs: an owner and a store to pay from.
[[nodiscard]] SettlementInit town_hall(PlayerId owner) {
  SettlementInit init;
  init.kind = SettlementKind::stronghold;
  init.owner = owner;
  init.can_be_captured = true;
  init.produces_gold = true;
  init.efficiency = 1;
  init.population = 40;
  init.max_population = 100;
  init.gold = 2500;
  init.food = 200;
  init.max_gold = 100000000;
  init.max_food = 100000000;
  init.max_units = 10000;
  return init;
}

script::HostOutcome call_host(script::HostRegistry& registry, World* world,
                              script::CallKind kind, const char* name, std::uint16_t arity,
                              std::vector<script::Value> arguments,
                              std::vector<script::Value>* out = nullptr) {
  const std::uint32_t index = registry.find(kind, name, arity);
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
  const script::HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
  script::CallContext ctx;
  HostContext state;
  state.world = world;
  ctx.arguments = arguments;
  // A null world is passed as a null `user`, which is the case every host
  // function has to refuse rather than dereference.
  ctx.user = world == nullptr ? nullptr : &state;
  ctx.name = name;
  ctx.kind = kind;
  const script::HostOutcome outcome = entry.fn(ctx);
  // The argument window is the out-parameter mechanism; `CheckUEnabled` writes
  // through argument 0.
  if (out != nullptr) *out = arguments;
  return outcome;
}

}  // namespace

// --------------------------------------------------------------------------
// the environment store
// --------------------------------------------------------------------------

TEST(env_store_reads_zero_and_empty_for_a_key_nobody_wrote) {
  // `ES_STRONGHOLD.VS` tests `EnvReadInt(set, "AIV_NoRepair") == 0` and
  // `SETTLEMENT_BEHAVIOR_AMBIENT.VS` tests
  // `EnvReadInt(this, "no_villagers") == 1`, both against keys nothing in the
  // corpus ever writes. A miss has to read as zero or neither script runs.
  EnvStore store;
  const EnvScope set = EnvScope::for_settlement(0);
  CHECK(store.read_int(set, "AIV_NoRepair") == 0);
  CHECK(store.read_string(set, "strTech").empty());
  CHECK(store.read_object(set, "TributeBuilding") == sim::kNoObject);
  CHECK(store.find(set, "anything") == nullptr);
  CHECK(store.size() == 0);
}

TEST(env_reads_an_int_back_out_of_a_string_write) {
  // `DATA\COMMANDS\ARENA.XML`: `SetsPlr, maxtrainlevel, 4`.
  // `ONFINISH_RESEARCH.VS` applies it with
  //     EnvWriteString(.player, name, value);
  // and `UNIT_TRAIN.VS` reads it back with
  //     maxlevel = EnvReadInt(This.player, "maxtrainlevel");
  // One slot, coerced on read. This is the test that says so.
  EnvStore store;
  const EnvScope player = EnvScope::for_player(2);
  store.write_string(player, "maxtrainlevel", "4");
  CHECK(store.read_int(player, "maxtrainlevel") == 4);
  CHECK(store.read_string(player, "maxtrainlevel") == "4");

  // And the other way, which has no corpus witness and is the only reading
  // consistent with a string-valued store.
  store.write_int(player, "BestHeroLevel", 12);
  CHECK(store.read_string(player, "BestHeroLevel") == "12");
  CHECK(store.read_int(player, "BestHeroLevel") == 12);

  // A value that is not a number at all reads as zero, which is what
  // `EnvReadInt` on a research ledger entry has to do.
  store.write_string(player, "Tribute", "researched");
  CHECK(store.read_int(player, "Tribute") == 0);
}

TEST(env_parse_int_stops_at_the_first_non_digit) {
  CHECK(env_parse_int("4") == 4);
  CHECK(env_parse_int("-17") == -17);
  CHECK(env_parse_int("  25  ") == 25);
  CHECK(env_parse_int("") == 0);
  CHECK(env_parse_int("researched") == 0);
  // `ParseStr`-shaped: a list value yields its head. `ESH_MARKET.VS` reads
  // `TributeTimes` this way through `Str2Int`.
  CHECK(env_parse_int("0,   10,   20,   30") == 0);
  CHECK(env_parse_int("500, 1000") == 500);
  // Saturating rather than wrapping: a deterministic core may not have signed
  // overflow in it.
  CHECK(env_parse_int("999999999999") == 2147483647);

  CHECK(env_format_int(0) == "0");
  CHECK(env_format_int(-2147483647 - 1) == "-2147483648");
  CHECK(env_parse_int(env_format_int(123456)) == 123456);
}

TEST(env_object_values_round_trip_through_the_dollar_encoding) {
  // `RESEARCH.VS` stores a building: `EnvWriteObj(.player, "TributeBuilding",
  // th)`, and `ONFINISH_RESEARCH.VS` reads it back with `EnvReadObj`.
  // `$$%d$$` is `gbr.exe`'s own encoding; see env.hpp.
  EnvStore store;
  const EnvScope player = EnvScope::for_player(1);
  store.write_object(player, "TributeBuilding", 4242);
  CHECK(store.read_object(player, "TributeBuilding") == 4242);
  CHECK(store.read_string(player, "TributeBuilding") == "$$4242$$");
  CHECK(env_parse_object("4242") == sim::kNoObject);
  CHECK(env_parse_object("$$0$$") == sim::kNoObject);

  store.write_object(player, "TributeBuilding", sim::kNoObject);
  CHECK(store.read_object(player, "TributeBuilding") == sim::kNoObject);
}

TEST(env_scopes_with_the_same_number_are_different_scopes) {
  // The retail keys are `/<map>/Player2/x`, `/<map>/Settlement2/x` and
  // `/<map>/Settlement<id>/Bld2/x`. Three paths, one number.
  EnvStore store;
  store.write_int(EnvScope::for_player(2), "x", 10);
  store.write_int(EnvScope::for_settlement(2), "x", 20);
  store.write_int(EnvScope::for_building(2), "x", 30);
  store.write_int(EnvScope::root(), "x", 40);
  CHECK(store.read_int(EnvScope::for_player(2), "x") == 10);
  CHECK(store.read_int(EnvScope::for_settlement(2), "x") == 20);
  CHECK(store.read_int(EnvScope::for_building(2), "x") == 30);
  CHECK(store.read_int(EnvScope::root(), "x") == 40);
  CHECK(store.size() == 4);
}

TEST(env_store_iteration_order_is_content_determined) {
  // Iteration order is world state. Two peers that wrote the same keys in a
  // different order must walk them identically and hash identically -- which
  // is exactly what a `std::unordered_map<std::string, …>` would not
  // guarantee, and why this store is a sorted vector.
  const char* keys[] = {"TechSeq", "elimination", "FreezeArmyBuild", "ArenaRush",
                        "nLastResearched", "CounterUnit_1", "GTRush"};
  EnvStore forwards;
  EnvStore backwards;
  for (int i = 0; i < 7; ++i) {
    forwards.write_int(EnvScope::for_settlement(static_cast<SettlementId>(i % 3)), keys[i], i);
  }
  for (int i = 6; i >= 0; --i) {
    backwards.write_int(EnvScope::for_settlement(static_cast<SettlementId>(i % 3)), keys[i], i);
  }
  REQUIRE(forwards.size() == backwards.size());
  for (std::size_t i = 0; i < forwards.size(); ++i) {
    CHECK(forwards.entries()[i].key == backwards.entries()[i].key);
    CHECK(forwards.entries()[i].scope == backwards.entries()[i].scope);
    CHECK(forwards.entries()[i].value == backwards.entries()[i].value);
  }
  CHECK(forwards.hash() == backwards.hash());

  // And the order really is sorted, so it is a function of the contents rather
  // than of anything the store remembers.
  for (std::size_t i = 1; i < forwards.size(); ++i) {
    const EnvEntry& a = forwards.entries()[i - 1];
    const EnvEntry& b = forwards.entries()[i];
    const bool ordered = a.scope.kind < b.scope.kind ||
                         (a.scope.kind == b.scope.kind &&
                          (a.scope.id < b.scope.id ||
                           (a.scope.id == b.scope.id && a.key < b.key)));
    CHECK(ordered);
  }

  // A different value is a different hash; an erase gets back to where it was.
  EnvStore one;
  one.write_int(EnvScope::for_player(1), "k", 1);
  const std::uint64_t before = one.hash();
  one.write_int(EnvScope::for_player(1), "k", 2);
  CHECK(one.hash() != before);
  one.write_int(EnvScope::for_player(1), "k", 1);
  CHECK(one.hash() == before);
  CHECK(one.erase(EnvScope::for_player(1), "k"));
  CHECK(!one.erase(EnvScope::for_player(1), "k"));
  CHECK(one.size() == 0);
}

// --------------------------------------------------------------------------
// AI variables
// --------------------------------------------------------------------------

TEST(ai_vars_read_zero_until_a_profile_seeds_them) {
  AiVarStore vars;
  CHECK(vars.get(2, 0) == 0);
  CHECK(vars.get(2, 40) == 0);
  CHECK(!vars.bit(2, 0, 3));
  CHECK(vars.player_count() == 0);

  CHECK(vars.set(2, 7, 4000));
  CHECK(vars.get(2, 7) == 4000);
  CHECK(vars.get(3, 7) == 0);  // one player's tuning is not another's
  CHECK(vars.player_count() == 1);

  // Out of range is refused rather than silently allocating.
  CHECK(!vars.set(-1, 0, 1));
  CHECK(!vars.set(0, -1, 1));
  CHECK(!vars.set(0, AiVarStore::kMaxVar, 1));
  CHECK(!vars.set(AiVarStore::kMaxPlayer + 1, 0, 1));
}

TEST(ai_var_masks_are_bit_tests_indexed_by_player_number) {
  // `DATA\AI\AI.INI`, verbatim:
  //   ;   SetAIVar(2, AIMV_NoAttack, 1, true);  // player 2 AI won't attack
  //   ;                                         // player 1
  //   ;   AIVar(<AIPlayerNum>, <Variable>, <PlayerNum>); // returns bool
  //   AIMV_NoAttack=0  ; player mask variable, setting bit i disables
  //                    ; attacking player i
  //
  // `RECRUITER.VS` reads the same variable both ways in one function --
  // `AIVar(p, AIMV_NoAttack) != 0` for "any bit set", then
  // `AIVar(p, AIMV_NoAttack, j)` for `j` in 1..16 -- which pins the bit index
  // to the player number with no scaling.
  AiVarStore vars;
  constexpr std::int32_t kNoAttack = 11;  // some id; the profile decides which
  CHECK(vars.get(2, kNoAttack) == 0);

  CHECK(vars.set_bit(2, kNoAttack, 1, true));
  CHECK(vars.bit(2, kNoAttack, 1));
  CHECK(!vars.bit(2, kNoAttack, 2));
  CHECK(vars.get(2, kNoAttack) == 2);  // bit 1

  CHECK(vars.set_bit(2, kNoAttack, 16, true));
  CHECK(vars.bit(2, kNoAttack, 16));
  CHECK(vars.get(2, kNoAttack) != 0);

  CHECK(vars.set_bit(2, kNoAttack, 1, false));
  CHECK(!vars.bit(2, kNoAttack, 1));
  CHECK(vars.bit(2, kNoAttack, 16));

  // Out of a 32-bit word is false rather than undefined.
  CHECK(!vars.bit(2, kNoAttack, 32));
  CHECK(!vars.bit(2, kNoAttack, -1));
}

TEST(ai_vars_are_seeded_from_the_profile_in_declaration_order) {
  // The variable id is the position in `[Vars.All]`. Nothing else can supply a
  // numbering: `gbr.exe` declares `AIVar` as `int, int idPlayer, int var` and
  // carries not one `AIV_*` name.
  constexpr std::string_view kIni =
      "[Vars.All]\n"
      "AIV_BuildArmy=1\n"
      "AIV_Research=1\n"
      "AIV_ExcessGold=4000\n"
      "\n"
      "[Vars.Hard]\n"
      "AIV_ExcessGold=2000\n";
  const auto parsed = AiProfile::parse(bytes_of(kIni));
  REQUIRE(parsed.ok());

  EnvSystem env;
  env.seed_ai_vars(2, parsed.value(), AiDifficulty::none);
  CHECK(env.ai_var_id("AIV_BuildArmy") == 0);
  CHECK(env.ai_var_id("AIV_Research") == 1);
  CHECK(env.ai_var_id("AIV_ExcessGold") == 2);
  CHECK(env.ai_var_id("AIV_NoSuchThing") == -1);
  CHECK(env.ai_var_name(2) == "AIV_ExcessGold");
  CHECK(env.ai_var_name(9).empty());
  CHECK(env.ai_vars().get(2, 2) == 4000);

  // The difficulty overlay replaces the value and keeps the position, so an id
  // means the same variable at every difficulty.
  env.seed_ai_vars(3, parsed.value(), AiDifficulty::hard);
  CHECK(env.ai_var_id("AIV_ExcessGold") == 2);
  CHECK(env.ai_vars().get(3, 2) == 2000);
  CHECK(env.ai_vars().get(2, 2) == 4000);
}

// --------------------------------------------------------------------------
// research
// --------------------------------------------------------------------------

TEST(research_param_grammar_is_the_one_the_verifier_parses) {
  // The grammar the arena commands are written in, with a `param` in the shape
  // `DATA\COMMANDS\ARENA.XML` uses. `VERIFY_RESEARCH.VS` opens with the same
  // grammar as a comment and then parses it with `ParseStr` exactly this way.
  //
  // Four things this one string has to survive: a `ReqSet` and a `ReqPlr` in
  // one list, an **empty** third field on both of them, a `SetsPlr` whose third
  // field is present, and a trailing comma with nothing after it.
  const ResearchCommand command = parse_research_command(
      "Arena Drill 2",
      "ReqSet, Bouts, , ReqPlr, Drill, , SetsPlr, maxtrainlevel, 8, "
      "NamePlr, Advanced Drill, ");
  CHECK(command.name == "Arena Drill 2");
  CHECK(command.scope == ResearchScope::player);
  // The ledger name is not the command name: this one records `Advanced
  // Drill`, and the shipped arena rows do the same.
  CHECK(command.records == "Advanced Drill");
  REQUIRE(command.requires_.size() == 2);
  CHECK(command.requires_[0].scope == ResearchScope::settlement);
  CHECK(command.requires_[0].name == "Bouts");
  CHECK(!command.requires_[0].negated);
  CHECK(command.requires_[1].scope == ResearchScope::player);
  CHECK(command.requires_[1].name == "Drill");
  REQUIRE(command.assigns.size() == 1);
  CHECK(command.assigns[0].scope == ResearchScope::player);
  CHECK(command.assigns[0].key == "maxtrainlevel");
  CHECK(command.assigns[0].value == "8");

  // A specialisation, in the shape the blacksmith rows use: `NReqSet` on the
  // command's own ledger name is what makes a family of them mutually
  // exclusive, and the `,NReqSet` with no space after the comma is the kind of
  // spacing the shipped rows actually have.
  const ResearchCommand spec = parse_research_command(
      "CTestFootman",
      "NReqSet, CTestFootman, ,NReqSet, Specialization, default, "
      "NameSet, CTestFootman, , SetsSet, Specialization, researched, "
      "SetsSet, SpecializedUnit, 1");
  CHECK(spec.scope == ResearchScope::settlement);
  CHECK(spec.records == "CTestFootman");
  REQUIRE(spec.requires_.size() == 2);
  CHECK(spec.requires_[0].negated);
  CHECK(spec.requires_[1].negated);
  CHECK(spec.requires_[1].name == "Specialization");
  REQUIRE(spec.assigns.size() == 2);
  CHECK(spec.assigns[1].key == "SpecializedUnit");
  CHECK(spec.assigns[1].value == "1");

  // `<cmd name="Tournaments" … param="NameSet, Tournaments, default">`: a node
  // with no edges.
  const ResearchCommand leaf = parse_research_command("Tournaments",
                                                      "NameSet, Tournaments, default");
  CHECK(leaf.scope == ResearchScope::settlement);
  CHECK(leaf.records == "Tournaments");
  CHECK(leaf.requires_.empty());
  CHECK(leaf.assigns.empty());
}

TEST(research_catalog_is_built_from_the_command_table) {
  // The tree is data. A `CommandDef` whose `method` is `research` *is* a node,
  // and its costs are the same numbers `GetCmdCost` reports.
  CommandTable table;
  CommandDef fights;
  fights.name = "Fights";
  fights.method = "research";
  fights.param = "SetsSet, levels/GSwordsman, 4, NameSet, Fights, default,";
  fights.cost_gold = 500;
  table.set(fights);

  CommandDef tactics;
  tactics.name = "Battle tactics";
  tactics.method = "research";
  tactics.param = "ReqSet, Fights, default, NamePlr, Battle tactics, default";
  tactics.cost_gold = 1000;
  tactics.cost_food = 250;
  table.set(tactics);

  CommandDef move;  // not a research command
  move.name = "move";
  move.method = "move";
  table.set(move);

  ResearchCatalog catalog;
  catalog.build_from(table);
  CHECK(catalog.size() == 2);
  REQUIRE(catalog.find("Fights") != nullptr);
  CHECK(catalog.find("Fights")->scope == ResearchScope::settlement);
  CHECK(catalog.find("Fights")->cost_gold == 500);
  REQUIRE(catalog.find("Battle tactics") != nullptr);
  CHECK(catalog.find("Battle tactics")->scope == ResearchScope::player);
  CHECK(catalog.find("Battle tactics")->cost_food == 250);
  CHECK(catalog.find("move") == nullptr);

  // Rebuilding replaces rather than accumulates.
  catalog.build_from(table);
  CHECK(catalog.size() == 2);
}

namespace {

/// The two-node tree the tests below use: `Fights` on the settlement,
/// `Battle tactics` on the player and requiring `Fights`. Both shapes occur in
/// `ARENA.XML`, and `ESH_NEEDTECH.VS` asks a settlement about both through one
/// `IsResearched` call.
void install_tree(EnvSystem& env) {
  CommandTable table;
  CommandDef fights;
  fights.name = "Fights";
  fights.method = "research";
  fights.param = "NameSet, Fights, default,";
  fights.cost_gold = 500;
  table.set(fights);

  CommandDef tactics;
  tactics.name = "Battle tactics";
  tactics.method = "research";
  tactics.param = "ReqSet, Fights, default, NamePlr, Battle tactics, default";
  tactics.cost_gold = 1000;
  table.set(tactics);

  env.research().build_from(table);
}

}  // namespace

TEST(research_is_recorded_in_the_ledger_the_command_names) {
  EnvSystem env;
  install_tree(env);

  EconomySystem economy;
  World world;
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&env));
  const SettlementId id = economy.create(world, town_hall(2));
  economy.start(world);
  const Settlement& set = *economy.find(id);

  CHECK(!env.is_researched(set, "Fights"));
  CHECK(!env.is_researched(set, "Battle tactics"));

  // `RESEARCH.VS`: `EnvWriteString(.settlement, name, "researching")`.
  CHECK(env.begin_research(set, "Fights"));
  CHECK(env.is_researching(set, "Fights"));
  CHECK(!env.is_researched(set, "Fights"));
  CHECK(env.env().read_string(EnvScope::for_settlement(id), "Fights") == "researching");

  // `ONFINISH_RESEARCH.VS`, the not-cancelled branch.
  CHECK(env.finish_research(set, "Fights"));
  CHECK(env.is_researched(set, "Fights"));
  CHECK(!env.is_researching(set, "Fights"));

  // A `NamePlr` research lands on the *player's* ledger, and `IsResearched`
  // asked of the settlement still finds it -- which is what `ESH_NEEDTECH.VS`
  // relies on when it asks `IsResearched(set, "Battle tactics")`.
  CHECK(env.finish_research(set, "Battle tactics"));
  CHECK(env.env().read_string(EnvScope::for_player(2), "Battle tactics") == "researched");
  CHECK(env.env().read_string(EnvScope::for_settlement(id), "Battle tactics").empty());
  CHECK(env.is_researched(set, "Battle tactics"));
  CHECK(env.is_researched_for_player(2, "Battle tactics"));
  CHECK(!env.is_researched_for_player(3, "Battle tactics"));

  // Cancelling writes `""` back, which is what the cancelled branch does.
  CHECK(env.cancel_research(set, "Battle tactics"));
  CHECK(!env.is_researched(set, "Battle tactics"));
}

TEST(finishing_a_research_applies_its_env_writes) {
  // `SetsPlr, maxtrainlevel, 4` written as a string by `ONFINISH_RESEARCH.VS`,
  // read as an int by `UNIT_TRAIN.VS`. This is the whole arena-training
  // mechanism, end to end.
  CommandTable table;
  CommandDef training;
  training.name = "Arena Drill 1";
  training.method = "research";
  training.param = "ReqSet, Bouts, default, SetsPlr, maxtrainlevel, 4, NamePlr, Drill, ";
  training.cost_gold = 1000;
  table.set(training);

  EnvSystem env;
  env.research().build_from(table);

  EconomySystem economy;
  World world;
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&env));
  const SettlementId id = economy.create(world, town_hall(2));
  economy.start(world);
  const Settlement& set = *economy.find(id);

  CHECK(env.env().read_int(EnvScope::for_player(2), "maxtrainlevel") == 0);
  CHECK(env.finish_research(set, "Arena Drill 1"));
  CHECK(env.env().read_int(EnvScope::for_player(2), "maxtrainlevel") == 4);
  CHECK(env.env().read_string(EnvScope::for_player(2), "maxtrainlevel") == "4");
  // The ledger records `Drill`, not the command name.
  CHECK(env.env().read_string(EnvScope::for_player(2), "Drill") == "researched");
  CHECK(env.is_researched(set, "Arena Drill 1"));
}

TEST(can_research_checks_the_prerequisite_graph) {
  EnvSystem env;
  install_tree(env);

  EconomySystem economy;
  World world;
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&env));
  const SettlementId id = economy.create(world, town_hall(2));
  economy.start(world);
  const Settlement& set = *economy.find(id);

  CHECK(env.can_research(set, "Fights"));
  CHECK(!env.can_research(set, "Battle tactics"));  // `ReqSet, Fights` unmet

  env.finish_research(set, "Fights");
  CHECK(!env.can_research(set, "Fights"));  // already done
  CHECK(env.can_research(set, "Battle tactics"));

  env.begin_research(set, "Battle tactics");
  CHECK(!env.can_research(set, "Battle tactics"));  // already running

  // A name the catalog does not know is not researchable once a catalog is
  // loaded -- `gbr.exe`'s `No such upgrade %s`.
  CHECK(!env.can_research(set, "Cult of Anubis"));

  // A negated requirement: `NReqSet` is satisfied while the named research is
  // *not* done.
  CommandTable table;
  CommandDef spec;
  spec.name = "CMaceman";
  spec.method = "research";
  spec.param = "NReqSet, Specialization, default, NameSet, CMaceman, ";
  table.set(spec);
  EnvSystem other;
  other.research().build_from(table);
  const Settlement& set2 = *economy.find(id);
  CHECK(other.can_research(set2, "CMaceman"));
  other.env().write_string(EnvScope::for_settlement(id), "Specialization", "researched");
  CHECK(!other.can_research(set2, "CMaceman"));
}

TEST(ts_research_is_the_poll_loop_the_tactic_scripts_run) {
  // `TS_GAULTACTIC.VS`:
  //     while(!bFights) { bFights = set.TSResearch("Fights"); Sleep(...); }
  // so the call must be idempotent, must return false while the work is
  // outstanding, and must return true once it is done. `TSH_Research.vs` is
  // the six-line body; see env.hpp for what we substitute for `set.Research`.
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_env_host(registry);

  EnvSystem env;
  install_tree(env);
  EconomySystem economy;
  World world;
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&env));
  const SettlementId id = economy.create(world, town_hall(2));
  economy.start(world);
  const script::Value handle = script::Value::object(kTypeSettlement, economy.find(id)->object);

  const auto ts = [&](const char* name) {
    return call_host(registry, &world, script::CallKind::member, "TSResearch", 1,
                     {handle, script::Value::string(name)});
  };

  script::HostOutcome first = ts("Fights");
  REQUIRE(first.status == script::HostStatus::ok);
  CHECK(first.value.as_integer() == 0);  // started, not finished
  CHECK(env.is_researching(*economy.find(id), "Fights"));
  // Idempotent: a second poll while it is running does not start it twice.
  CHECK(ts("Fights").value.as_integer() == 0);

  env.finish_research(*economy.find(id), "Fights");
  CHECK(ts("Fights").value.as_integer() == 1);

  // Cannot afford: `Battle tactics` costs 1000 and the town hall holds 2500,
  // so drain it first.
  economy.set_resource(id, Resource::gold, 100);
  CHECK(ts("Battle tactics").value.as_integer() == 0);
  CHECK(!env.is_researching(*economy.find(id), "Battle tactics"));
}

// --------------------------------------------------------------------------
// unit availability
// --------------------------------------------------------------------------

TEST(the_shipped_unit_catalog_is_the_table_the_binary_carries) {
  const UnitCatalog catalog = shipped_unit_catalog();
  REQUIRE(catalog.races().size() == 8);

  // The race order, corroborated five ways in `gbr.exe`'s string pool. If the
  // race constants turn out not to be 0..7 in this order, this is the test
  // that has to change and nothing else does.
  CHECK(catalog.races()[0].name == "Gaul");
  CHECK(catalog.races()[1].name == "RepublicanRome");
  CHECK(catalog.races()[2].name == "Carthage");
  CHECK(catalog.races()[3].name == "Iberia");
  CHECK(catalog.races()[4].name == "ImperialRome");
  CHECK(catalog.races()[5].name == "Britain");
  CHECK(catalog.races()[6].name == "Egypt");
  CHECK(catalog.races()[7].name == "Germany");

  // `ESH_ENABLEDUNITS.VS` pairs Gaul's index 0 with `AIV_MaxGSwordsman` and
  // its index 5 with `AIV_MaxGWomanWarrior`; `GBARRACKS.XML` gives the same
  // six in the same order.
  REQUIRE(catalog.row(0, 0) != nullptr);
  CHECK(catalog.row(0, 0)->type == "GSwordsman");
  CHECK(catalog.row(0, 0)->tech.empty());  // the first two need no research
  CHECK(catalog.row(0, 2)->type == "GAxeman");
  CHECK(catalog.row(0, 2)->tech == "Gaul Iron Axes");
  CHECK(catalog.row(0, 2)->train_cmd == "trainGAxeman");
  CHECK(catalog.row(0, 5)->type == "GWomanWarrior");
  CHECK(catalog.row(0, 6) == nullptr);

  // Iberia is the case that decides the question: `IBARRACKS.XML` lists the
  // slinger before the defender, and the binary, `COUNTERUNITS.XML` and
  // `ESH_ENABLEDUNITS.VS` all put the defender at 2 and the slinger at 4. The
  // file order is a display order.
  CHECK(catalog.row(3, 2)->type == "IDefender");
  CHECK(catalog.row(3, 3)->type == "ICavalry");
  CHECK(catalog.row(3, 4)->type == "ISlinger");

  // The two Romes share unit classes and differ in train command and roster.
  CHECK(catalog.row(1, 5)->type == "RTribune");
  CHECK(catalog.row(4, 5)->type == "RPraetorian");
  CHECK(catalog.row(1, 0)->train_cmd == "trainRHastatus");
  CHECK(catalog.row(4, 0)->train_cmd == "trainMHastatus");

  // `RUType` is the inverse, and it is per race.
  CHECK(catalog.index_of(0, "GHorseman") == 4);
  CHECK(catalog.index_of(0, "IDefender") == -1);
  CHECK(catalog.index_of(3, "IDefender") == 2);
  CHECK(catalog.index_of(99, "GHorseman") == -1);

  // The arena unit and its gate, which `ESH_NEEDTECH.VS` names race by race in
  // its `bArenaHire` chain.
  CHECK(catalog.races()[0].arena.type == "GTridentWarrior");
  CHECK(catalog.races()[0].arena.train_cmd == "Hire Trident warrior");
  CHECK(catalog.races()[4].arena.train_cmd == "Hire Liberati");
  CHECK(catalog.races()[7].arena.train_cmd == "Call Valkyries");
  // Egypt's arena tech is the one string of the table `gbr.exe` does not
  // carry; reproduced as the hole it is.
  CHECK(catalog.races()[6].arena.tech.empty());

  CHECK(catalog.races()[0].temple.type == "GDruid");
  CHECK(catalog.races()[4].temple.type == "RPriest");  // shares Rome's temple
  CHECK(catalog.races()[5].temple.train_cmd.empty());  // Britain's is absent too
}

TEST(u_enabled_is_a_bit_test_and_check_u_enabled_writes_the_mask) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_env_host(registry);
  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));

  // `SQUADMONITOR.VS` uses `UEnabled` on a flee-reason bitfield, which is what
  // says the name is a misnomer: it is not unit-specific and not a research
  // test.
  const auto enabled = [&](std::int32_t mask, std::int32_t bit) {
    return call_host(registry, &world, script::CallKind::free_function, "UEnabled", 2,
                     {script::Value::integer(mask), script::Value::integer(bit)})
        .value.as_integer();
  };
  CHECK(enabled(0b1010, 1) == 1);
  CHECK(enabled(0b1010, 0) == 0);
  CHECK(enabled(0b1010, 3) == 1);
  CHECK(enabled(0b1010, 40) == 0);

  // `ESH_ENABLEDUNITS.VS`: zero the mask, one call per unit type carrying that
  // type's `AIV_Max*` value. `AI.INI` documents that value space as
  // "0 - disable, -1: no limit".
  std::vector<script::Value> out;
  script::HostOutcome set = call_host(
      registry, &world, script::CallKind::free_function, "CheckUEnabled", 5,
      {script::Value::integer(0), script::Value::integer(2), script::Value::integer(-1),
       script::Value::integer(3), script::Value::integer(0)},
      &out);
  REQUIRE(set.status == script::HostStatus::ok);
  CHECK(set.value.as_integer() == 1);
  REQUIRE(out.size() == 5);
  CHECK(out[0].as_integer() == 0b1000);

  // `ESH_BUILDARMY.VS`'s only call is the clearing half:
  //     CheckUEnabled(nEnabled, AIPlayer, 0, 5, Carthage);
  script::HostOutcome cleared = call_host(
      registry, &world, script::CallKind::free_function, "CheckUEnabled", 5,
      {script::Value::integer(0b101000), script::Value::integer(2),
       script::Value::integer(0), script::Value::integer(5), script::Value::integer(2)},
      &out);
  CHECK(cleared.value.as_integer() == 0);
  CHECK(out[0].as_integer() == 0b001000);
}

TEST(the_unit_table_entry_points_answer_from_the_catalog) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_env_host(registry);
  World world;
  EnvSystem env;
  env.units() = shipped_unit_catalog();
  REQUIRE(world.add_system(&env));

  const auto str = [&](const char* name, std::uint16_t arity,
                       std::vector<script::Value> args) {
    const script::HostOutcome outcome =
        call_host(registry, &world, script::CallKind::free_function, name, arity,
                  std::move(args));
    return outcome.value.is_string() ? outcome.value.as_string() : std::string{};
  };

  // `TSH_RecruitArmy.vs`: class -> index -> command and tech.
  const script::HostOutcome index =
      call_host(registry, &world, script::CallKind::free_function, "RUType", 2,
                {script::Value::string("GHorseman"), script::Value::integer(0)});
  REQUIRE(index.value.as_integer() == 4);
  CHECK(str("UTrainCmd", 2, {script::Value::integer(4), script::Value::integer(0)}) ==
        "trainGHorseman");
  CHECK(str("UTech", 2, {script::Value::integer(4), script::Value::integer(0)}) ==
        "Gaul Horseman");
  CHECK(str("UType", 2, {script::Value::integer(4), script::Value::integer(0)}) ==
        "GHorseman");

  // `ESH_BUILDARMY.VS` tests `UTech(i, nRace) != ""`, so an empty answer is
  // meaningful and an out-of-range one must be empty rather than an error.
  CHECK(str("UTech", 2, {script::Value::integer(0), script::Value::integer(0)}).empty());
  CHECK(str("UType", 2, {script::Value::integer(99), script::Value::integer(0)}).empty());
  CHECK(str("UType", 2, {script::Value::integer(0), script::Value::integer(99)}).empty());

  // `TSH_ArenaRecruit.vs` and `TSH_TempleRecruit.vs`.
  CHECK(str("ArenaUType", 1, {script::Value::integer(3)}) == "IMountaineer");
  CHECK(str("ArenaUTech", 1, {script::Value::integer(3)}) == "Tournaments");
  CHECK(str("ArenaTrainCmd", 1, {script::Value::integer(3)}) == "Call Mountaineer");
  CHECK(str("TempleUType", 1, {script::Value::integer(2)}) == "CShaman");
  CHECK(str("TempleUTrain", 1, {script::Value::integer(2)}) == "trainCShaman");

  // With no catalog at all every one of them is empty rather than wrong.
  World bare_world;
  EnvSystem bare_env;
  REQUIRE(bare_world.add_system(&bare_env));
  const script::HostOutcome none =
      call_host(registry, &bare_world, script::CallKind::free_function, "UType", 2,
                {script::Value::integer(0), script::Value::integer(0)});
  CHECK(none.value.as_string().empty());
}

// --------------------------------------------------------------------------
// balance constants
// --------------------------------------------------------------------------

TEST(constants_are_read_from_the_ini_rather_than_transcribed) {
  // A slice of `DATA\CONST.INI`'s `[GamePlay]` section, spacing and inline
  // comments as the file has them. `ProductionInterval` is here because it is
  // the value a mid-number read once turned into 20.
  constexpr std::string_view kIni =
      "[PlayerColors]\n"
      "id1 = 255, 23, 23\n"
      "\n"
      "[GamePlay]\n"
      "MinPopulation=10\n"
      "ProductionInterval = 2000\n"
      "PopGroup=5 ; peasants come in groups of this many\n"
      "GiveDistance = 25\n"
      "Speed3 = 1400\n"
      "TributeTimes =  0,   10,   20,   30\n"
      "TributeGold = 500, 1000, 1500, 2000\n"
      "DeathBlowFeedback = DeathBlow\n";
  const auto parsed = IniDocument::parse(bytes_of(kIni));
  REQUIRE(parsed.ok());

  EnvSystem env;
  CHECK(env.load_constants(parsed.value()) == 8);

  std::int32_t value = 0;
  REQUIRE(env.constant("ProductionInterval", value));
  CHECK(value == 2000);  // two thousand, not twenty
  REQUIRE(env.constant("PopGroup", value));
  CHECK(value == 5);  // the inline comment is not part of the value
  REQUIRE(env.constant("Speed3", value));
  CHECK(value == 1400);  // `GetConst('Speed' + i)` builds this key at run time
  CHECK(!env.constant("id1", value));           // a different section
  CHECK(!env.constant("RecallDistance", value));  // in no section at all

  // A value that is not entirely an integer is a string constant, which is
  // what `GetConstStr`'s only two corpus keys are.
  std::string_view text;
  REQUIRE(env.constant_string("TributeTimes", text));
  CHECK(text == "0,   10,   20,   30");
  REQUIRE(env.constant_string("TributeGold", text));
  CHECK(text == "500, 1000, 1500, 2000");
  REQUIRE(env.constant_string("DeathBlowFeedback", text));
  CHECK(text == "DeathBlow");
  CHECK(!env.constant_string("MinPopulation", text));
  CHECK(!env.constant("TributeTimes", value));
}

TEST(get_const_asks_the_economy_first_and_reads_zero_for_a_key_nobody_defines) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_env_host(registry);

  World world;
  EconomySystem economy;
  EnvSystem env;
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&env));
  economy.start(world);

  const auto get = [&](const char* key) {
    return call_host(registry, &world, script::CallKind::free_function, "GetConst", 1,
                     {script::Value::string(key)});
  };

  // The economy holds `MinPopulation` as an `EconomyRules` field, so it
  // answers even with no `CONST.INI` loaded -- and `set_rules` moves it.
  CHECK(get("MinPopulation").value.as_integer() == 10);
  EconomyRules retuned = economy.rules();
  retuned.min_population = 3;
  economy.set_rules(retuned);
  CHECK(get("MinPopulation").value.as_integer() == 3);

  // Everything else needs the file.
  CHECK(get("CatapultBaseFireRate").value.as_integer() == 0);
  env.set_constant("CatapultBaseFireRate", 40);
  CHECK(get("CatapultBaseFireRate").value.as_integer() == 40);

  // `HERO_DETACH_BEHAVIOR.VS` -- a shipped, live behaviour -- reads a key
  // `CONST.INI` does not define. Zero, not a trap.
  const script::HostOutcome missing = get("RecallDistance");
  CHECK(missing.status == script::HostStatus::ok);
  CHECK(missing.value.as_integer() == 0);

  // A non-string argument is still an error: that is a miswritten call, not a
  // missing key.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "GetConst", 1,
                  {script::Value::integer(3)})
            .status == script::HostStatus::error);

  // `GetConstStr` over the same table.
  env.set_constant_string("TributeGold", "500, 1000, 1500, 2000");
  const script::HostOutcome list =
      call_host(registry, &world, script::CallKind::free_function, "GetConstStr", 1,
                {script::Value::string("TributeGold")});
  CHECK(list.value.as_string() == "500, 1000, 1500, 2000");
  CHECK(call_host(registry, &world, script::CallKind::free_function, "GetConstStr", 1,
                  {script::Value::string("NoSuchKey")})
            .value.as_string()
            .empty());
}

// --------------------------------------------------------------------------
// the host seam
// --------------------------------------------------------------------------

TEST(env_host_functions_resolve_the_scope_from_the_value_shape) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_env_host(registry);

  World world;
  EconomySystem economy;
  EnvSystem env;
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&env));
  const SettlementId id = economy.create(world, town_hall(2));
  economy.start(world);
  const ObjectId anchor = world.spawn(NativeClass::town_hall, nullptr);
  REQUIRE(economy.add_building(world, id, anchor, 5000));
  const script::Value set = script::Value::object(kTypeSettlement, economy.find(id)->object);
  const script::Value building = script::Value::object(kTypeObj, anchor);

  // `ONFINISH_RESEARCH.VS` writes to the building and to its settlement in the
  // same run, on the same town hall:
  //     EnvWriteString(this, "researching", "no");
  //     EnvWriteString(.settlement, name, "researched");
  // The two must not land in the same scope, which is why an ordinary object
  // handle is never mapped back to its settlement.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvWriteString", 3,
                  {building, script::Value::string("researching"), script::Value::string("yes")})
            .status == script::HostStatus::ok);
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvWriteString", 3,
                  {set, script::Value::string("researching"), script::Value::string("no")})
            .status == script::HostStatus::ok);
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadString", 2,
                  {building, script::Value::string("researching")})
            .value.as_string() == "yes");
  CHECK(env.env().read_string(EnvScope::for_settlement(id), "researching") == "no");

  // An integer scope is a player number. `ESH_ENABLEDUNITS.VS`:
  //     EnvWriteInt(AIPlayer, "GaulUnitsEnabled", nEnabled);
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvWriteInt", 3,
                  {script::Value::integer(2), script::Value::string("GaulUnitsEnabled"),
                   script::Value::integer(0b111111)})
            .status == script::HostStatus::ok);
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadInt", 2,
                  {script::Value::integer(2), script::Value::string("GaulUnitsEnabled")})
            .value.as_integer() == 0b111111);
  CHECK(env.env().read_int(EnvScope::for_player(2), "GaulUnitsEnabled") == 0b111111);

  // Objects round-trip through `EnvWriteObj` / `EnvReadObj`.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvWriteObj", 3,
                  {script::Value::integer(2), script::Value::string("TributeBuilding"),
                   building})
            .status == script::HostStatus::ok);
  const script::HostOutcome read =
      call_host(registry, &world, script::CallKind::free_function, "EnvReadObj", 2,
                {script::Value::integer(2), script::Value::string("TributeBuilding")});
  REQUIRE(read.value.is_object());
  CHECK(read.value.as_object().id == anchor);
  CHECK(read.value.as_object().type == kTypeObj);

  // A key nobody wrote yields an invalid handle, not a fabricated id.
  CHECK(!call_host(registry, &world, script::CallKind::free_function, "EnvReadObj", 2,
                   {script::Value::integer(2), script::Value::string("Nothing")})
             .value.as_object()
             .valid());
}

// The root scope: the one-argument overloads the containers use 403 times.
//
// Two things are asserted here that the header argues at length. First, the
// root form is *reachable* — before the shorter arities were registered every
// one of those 403 sites trapped as "declared but not implemented", which is
// this project's recurring failure mode: a store that works, a path nothing
// takes. Second, the key is stored **verbatim**: `"/Bonus"` and `"Bonus"` are
// two slots, because `gbr.exe`'s `MakePath` only ever *prepends* a context and
// never strips a slash. Reproducing that reproduces the shipped conquest's bug,
// which is the point.
TEST(env_root_scope_is_reachable_and_stores_the_key_verbatim) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_env_host(registry);
  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));

  // `Conquests/mediterranean.BFHP:Sequences/seq0.vs`, verbatim.
  REQUIRE(call_host(registry, &world, script::CallKind::free_function, "EnvWriteString", 2,
                    {script::Value::string("/Bonus"), script::Value::string("rBritain")})
              .status == script::HostStatus::ok);
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadString", 1,
                  {script::Value::string("/Bonus")})
            .value.as_string() == "rBritain");
  // `seq1.vs`, also verbatim -- and this is the slot it actually reads.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadString", 1,
                  {script::Value::string("Bonus")})
            .value.as_string().empty());
  CHECK(env.env().size() == 1);

  // `3_Great_Losses_Egypt.bfhp:Maps/1/Sequences/seq1.vs`: the integer form,
  // relative on both sides, which is the spelling that works.
  REQUIRE(call_host(registry, &world, script::CallKind::free_function, "EnvWriteInt", 2,
                    {script::Value::string("Waves"), script::Value::integer(7)})
              .status == script::HostStatus::ok);
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadInt", 1,
                  {script::Value::string("Waves")})
            .value.as_integer() == 7);
  CHECK(env.env().read_int(EnvScope::root(), "Waves") == 7);

  // The root scope is a scope, not the player-0 scope wearing a disguise.
  CHECK(env.env().read_int(EnvScope::for_player(0), "Waves") == 0);

  // A missing key still reads as zero rather than trapping.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadInt", 1,
                  {script::Value::string("/En_NoSuchThing")})
            .value.as_integer() == 0);

  // The arity decides the layout; it is never sniffed. A scoped call handed a
  // string where the scope belongs must refuse, not quietly write to the root.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvWriteInt", 3,
                  {script::Value::string("Waves"), script::Value::string("k"),
                   script::Value::integer(9)})
            .status == script::HostStatus::error);
  CHECK(env.env().read_int(EnvScope::root(), "Waves") == 7);

  // ...and the root form must refuse a non-string key rather than collide
  // every miswritten call onto one slot.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadInt", 1,
                  {script::Value::integer(3)})
            .status == script::HostStatus::error);

  // All four root arities the corpus calls are registered and implemented.
  for (const auto& [name, arity] : std::initializer_list<std::pair<const char*, std::uint16_t>>{
           {"EnvReadInt", 1}, {"EnvWriteInt", 2}, {"EnvReadString", 1}, {"EnvWriteString", 2}}) {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, name, arity);
    REQUIRE(index != script::kUnresolvedHost);
    CHECK(registry.entry(index).fn != nullptr);
  }
}

TEST(env_host_functions_refuse_an_invalid_scope_and_a_null_world) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_env_host(registry);

  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));

  // `gbr.exe` carries "Parameter #Settlement in function 'EnvWriteInt' is
  // uninitialized or invalid object." once per overload. An invalid handle is
  // that case.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadInt", 2,
                  {script::Value::object(script::ObjectRef{}), script::Value::string("k")})
            .status == script::HostStatus::error);
  // So is a key that is not a string, and a scope that is neither an integer
  // nor a handle.
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadInt", 2,
                  {script::Value::integer(2), script::Value::integer(7)})
            .status == script::HostStatus::error);
  CHECK(call_host(registry, &world, script::CallKind::free_function, "EnvReadInt", 2,
                  {script::Value::string("nope"), script::Value::string("k")})
            .status == script::HostStatus::error);

  // A null `CallContext::user` must refuse rather than dereference. Every
  // entry point this domain defines, without exception.
  for (const script::HostEntry& entry : registry.entries()) {
    if (entry.fn == nullptr) continue;
    std::vector<script::Value> args(entry.arity + (entry.kind == script::CallKind::member ? 1u : 0u),
                                    script::Value::integer(0));
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = nullptr;
    ctx.name = entry.name;
    ctx.kind = entry.kind;
    const script::HostOutcome outcome = entry.fn(ctx);
    // `UEnabled` and `CheckUEnabled` are pure bit arithmetic and need no
    // world; everything else must say so rather than crash.
    const bool worldless = entry.name == "UEnabled" || entry.name == "CheckUEnabled";
    CHECK(worldless || outcome.status == script::HostStatus::error);
  }
}

TEST(env_system_hashes_both_stores_and_nothing_else) {
  EnvSystem a;
  EnvSystem b;
  std::uint64_t ha = 0;
  std::uint64_t hb = 0;
  a.hash(ha);
  b.hash(hb);
  CHECK(ha == hb);

  a.env().write_int(EnvScope::for_player(1), "GTRush", 1);
  ha = 0;
  a.hash(ha);
  CHECK(ha != hb);

  b.env().write_int(EnvScope::for_player(1), "GTRush", 1);
  hb = 0;
  b.hash(hb);
  CHECK(ha == hb);

  a.ai_vars().set(1, 3, 7);
  ha = 0;
  a.hash(ha);
  CHECK(ha != hb);

  // Configuration is not state: loading the unit catalog on one peer and not
  // the other cannot change the hash, because both load it from the same
  // shipped files and a difference there is a data mismatch rather than a
  // desync.
  b.ai_vars().set(1, 3, 7);
  b.units() = shipped_unit_catalog();
  hb = 0;
  b.hash(hb);
  CHECK(ha == hb);
}

TEST(register_env_host_defines_what_it_says_it_does) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  const std::size_t before = registry.implemented();
  const std::size_t defined = register_env_host(registry);
  CHECK(defined == env_host_entry_count());
  // Every one of them was already in the declared surface, so the count of
  // implemented entry points rises by exactly the number defined and the table
  // gains no names.
  CHECK(registry.implemented() - before == defined);
}

// --------------------------------------------------------------------------
// GetCatapultAttackPoint
// --------------------------------------------------------------------------

/// The shot-scatter model, and **the draw count is the thing under test**.
///
/// Two draws per rejection round, `x` then `y`, from the world RNG's inclusive
/// `[-r, r]`; the acceptance test is `isqrt(dx*dx + dy*dy) <= r`, which is a
/// slightly wider disc than `dx*dx + dy*dy <= r*r`; and a radius small enough
/// that the corner `(-r, -r)` would be accepted skips the loop entirely and
/// draws nothing. A body that got any of those wrong would desynchronise
/// everything downstream of the shot rather than only the shot.
TEST(catapult_attack_point_rejection_samples_a_disc_and_draws_in_pairs) {
  constexpr std::string_view kIni =
      "[GamePlay]\n"
      "CatapultVariationRadius = 200\n";
  const auto parsed = IniDocument::parse(bytes_of(kIni));
  REQUIRE(parsed.ok());

  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));
  CHECK(env.load_constants(parsed.value()) == 1);
  // A map to clamp into: no match system, so the height layer's extent is what
  // `GetMapRect` falls back to.
  imperivm::test::Builder height;
  height.text(kGridMagic).u32(32).u32(8).u32(1024).u32(1024);
  for (std::uint32_t i = 0; i < 32 * 32; ++i) height.u8(0);
  const std::vector<std::byte> height_bytes(height.span().begin(), height.span().end());
  const Result<Grid> layer = Grid::parse(height_bytes);
  REQUIRE(layer.ok());
  world.set_height(layer.value());

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &world;
  const auto shoot = [&](Point aim) {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "GetCatapultAttackPoint", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return Point{};
    script::CallContext ctx;
    std::vector<script::Value> args{pack_point(aim)};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "GetCatapultAttackPoint";
    ctx.kind = script::CallKind::free_function;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return unpack_point(out.value);
  };

  // Every shot lands inside the disc, and the disc is `isqrt`'s rather than the
  // squared one: the bound below is `(r + 1) * (r + 1)`, and a body that used
  // `r * r` would still pass it -- so the *spread* is checked separately.
  std::int64_t widest = 0;
  bool moved = false;
  for (int i = 0; i < 2000; ++i) {
    const Point landed = shoot(Point{512, 512});
    const std::int64_t dx = landed.x - 512;
    const std::int64_t dy = landed.y - 512;
    // `isqrt(d) <= r` is exactly `d < (r + 1) * (r + 1)`, and that is the
    // ceiling this must never cross.
    CHECK(dx * dx + dy * dy < 201 * 201);
    if (dx != 0 || dy != 0) moved = true;
    widest = (dx * dx + dy * dy) > widest ? dx * dx + dy * dy : widest;
  }
  CHECK(moved);
  // **And it crosses `r * r`**, which is the whole difference between the
  // `isqrt` test and the squared one: a body written with `dx*dx + dy*dy <=
  // r*r` can never produce a shot in the annulus this asserts.
  CHECK(widest > 200 * 200);

  // **Two draws per round, in pairs.** The stream is advanced an even number of
  // times, which a body that drew one number and derived the other would not do.
  Rng before = world.rng();
  const Point landed = shoot(Point{512, 512});
  std::int32_t rounds = 0;
  for (; rounds < 64; ++rounds) {
    Rng probe = before;
    for (std::int32_t k = 0; k <= rounds; ++k) {
      (void)probe.between(-200, 200);
      (void)probe.between(-200, 200);
    }
    if (probe == world.rng()) break;
  }
  CHECK(rounds < 64);
  CHECK(landed.x != 0 || landed.y != 0);

  // The clamp is the map rectangle's, inclusive on both edges.
  for (int i = 0; i < 50; ++i) {
    const Point corner = shoot(Point{0, 0});
    CHECK(corner.x >= 0);
    CHECK(corner.y >= 0);
    const Point far = shoot(Point{1023, 1023});
    CHECK(far.x <= 1023);
    CHECK(far.y <= 1023);
  }
}

/// A radius small enough that the corner `(-r, -r)` passes the acceptance test
/// **draws nothing at all** and answers `aim + (r, r)`. `isqrt(2)` and
/// `isqrt(8)` are 1 and 2, so 0, 1 and 2 are the radii that do it. The shipped
/// file says 200; a modded one could say 2, and a body that always looped would
/// consume draws the original does not.
TEST(catapult_attack_point_with_a_tiny_radius_is_deterministic) {
  const auto run = [](std::int32_t radius) {
    const std::string ini =
        "[GamePlay]\nCatapultVariationRadius = " + std::to_string(radius) + "\n";
    const auto parsed = IniDocument::parse(bytes_of(ini));
    CHECK(parsed.ok());
    World world;
    EnvSystem env;
    CHECK(world.add_system(&env));
    if (parsed.ok()) CHECK(env.load_constants(parsed.value()) == 1);

    script::HostRegistry registry;
    (void)register_all_hosts(registry);
    HostContext context;
    context.world = &world;
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "GetCatapultAttackPoint", 1);
    CHECK(index != script::kUnresolvedHost);
    script::CallContext ctx;
    std::vector<script::Value> args{pack_point(Point{100, 100})};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "GetCatapultAttackPoint";
    ctx.kind = script::CallKind::free_function;
    const Rng before = world.rng();
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    // With no map layer at all the high corner is 0, so the clamp pins both
    // axes there -- which is the one thing this case is *not* about.
    CHECK(world.rng() == before);
    return unpack_point(out.value);
  };
  // No layer, so the map rectangle is a single point and the clamp wins; what
  // is asserted here is that the stream did not move, which `run` checks.
  (void)run(0);
  (void)run(1);
  (void)run(2);
}
