// AI profile tests.
//
// Synthetic fixtures that reproduce the structure of `DATA/AI/AI.INI` and of
// the one real overlay that ships, `DATA/AI/DEFENSIVE/AI.INI`. The parser was
// additionally run over both retail files and reproduces their measured shape:
// 14 squad states, 6 GAIKA strategies, 8 economy scripts, 22 tactic-script
// entries, 65 script declarations, and 125 distinct variables in `[Vars.All]`
// (128 lines less the three keys the file declares twice).
//
// The closure that justifies reading these constants out of data at all is
// measured, not assumed: for `SS_`, `GS_` and `ES_`, the set of names the 577
// shipped scripts use is exactly the set the file declares plus one sentinel,
// with nothing left over on either side.

#include <span>
#include <string_view>

#include "imperivm/core/sim/ai_profile.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// The shape of `DATA/AI/AI.INI`, cut down to what a test can read.
constexpr std::string_view kBase =
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
    "[Scripts]\n"
    "GetArmyNeed.vs = int, GAIKA g, int idPlayer, bool bMin\n"
    "Main.vs = void\n"
    "\n"
    "[Vars.All]\n"
    "AIV_Sleep_ES=3000              ; economy scripts think interval\n"
    "AIV_MaxMilUnits=-1             ; 0 - disable, -1: no limit\n"
    "AIV_SquanderGoldAmount=30000   ; spending without restrictions\n"
    "AIV_SquanderGoldAmount=15000   ; spending for research\n"
    "\n"
    "[Vars.Easy]\n"
    "AIV_MaxMilUnits=100\n"
    "[Vars.Hard]\n"
    "AIV_Sleep_ES=2000\n";

/// The shape of `DEFENSIVE`: no tactic scripts, three scripts replaced, a few
/// variables retuned.
constexpr std::string_view kOverlay =
    "[SquadStates]\n"
    "SS_Approach\n"
    "SS_Wait\n"
    "SS_ApproachWait\n"
    "\n"
    "[TacticScripts]\n"
    "\n"
    "[Scripts]\n"
    "Main.vs = void\n"
    "GAIKAMonitor.vs = void\n"
    "\n"
    "[Vars.Easy]\n"
    "AIV_Sleep_ES=5000\n";

}  // namespace

TEST(ai_profile_numbers_constants_by_position_with_the_sentinel_at_zero) {
  const auto parsed = AiProfile::parse(bytes_of(kBase));
  REQUIRE(parsed.ok());
  const AiProfile& profile = parsed.value();

  // The sentinels are engine defined -- they are the only names of these
  // families that appear as strings in `gbr.exe` -- so they occupy index 0 and
  // the declared entries follow. That the origin is 0 rather than something
  // else is the header's inference; this pins the behaviour either way.
  CHECK(profile.constant(AiEnum::squad_state, "SS_IDLE") == 0);
  CHECK(profile.constant(AiEnum::squad_state, "SS_Approach") == 1);
  CHECK(profile.constant(AiEnum::squad_state, "SS_ApproachWait") == 3);
  CHECK(profile.constant(AiEnum::gaika_strategy, "GS_NONE") == 0);
  CHECK(profile.constant(AiEnum::gaika_strategy, "GS_Capture") == 2);
  CHECK(profile.constant(AiEnum::economy_script, "ES_NONE") == 0);
  CHECK(profile.constant(AiEnum::tactic_script, "TS_NONE") == 0);

  CHECK(profile.constant(AiEnum::squad_state, "SS_NotAThing") == kUnknownAiConstant);
  // A name from the wrong family must not resolve, or a squad state would be
  // interchangeable with a strategy of the same index.
  CHECK(profile.constant(AiEnum::gaika_strategy, "SS_Approach") == kUnknownAiConstant);
}

TEST(ai_profile_maps_a_value_back_to_its_name) {
  const auto parsed = AiProfile::parse(bytes_of(kBase));
  REQUIRE(parsed.ok());
  const AiProfile& profile = parsed.value();
  // The engine has `SS_STR` and `GS_STR` host functions, `str, int state`.
  CHECK(profile.constant_name(AiEnum::squad_state, 0) == "SS_IDLE");
  CHECK(profile.constant_name(AiEnum::squad_state, 2) == "SS_Wait");
  CHECK(profile.constant_name(AiEnum::squad_state, 99).empty());
  CHECK(profile.constant_name(AiEnum::squad_state, -1).empty());
}

TEST(ai_profile_keeps_declaration_order) {
  const auto parsed = AiProfile::parse(bytes_of(kBase));
  REQUIRE(parsed.ok());
  const auto names = parsed.value().names(AiEnum::squad_state);
  REQUIRE(names.size() == 4);
  CHECK(names[0] == "SS_IDLE");
  CHECK(names[1] == "SS_Approach");
  CHECK(names[2] == "SS_Wait");
  CHECK(names[3] == "SS_ApproachWait");
}

TEST(ai_profile_reads_script_signatures) {
  const auto parsed = AiProfile::parse(bytes_of(kBase));
  REQUIRE(parsed.ok());
  const AiProfile& profile = parsed.value();
  CHECK(profile.scripts().size() == 2);
  CHECK(profile.script_signature("GetArmyNeed.vs") ==
        "int, GAIKA g, int idPlayer, bool bMin");
  // The same signature appears as a leading `//` comment inside the `.vs` file,
  // so the two are cross-checkable.
  CHECK(profile.script_signature("Main.vs") == "void");
  CHECK(profile.script_signature("Absent.vs").empty());
}

TEST(ai_profile_overlays_variables_by_difficulty) {
  const auto parsed = AiProfile::parse(bytes_of(kBase));
  REQUIRE(parsed.ok());
  const AiProfile& profile = parsed.value();

  CHECK(profile.variable("AIV_MaxMilUnits", AiDifficulty::none, 0) == -1);
  CHECK(profile.variable("AIV_MaxMilUnits", AiDifficulty::easy, 0) == 100);
  // Normal declares nothing, so it falls through to `[Vars.All]`.
  CHECK(profile.variable("AIV_MaxMilUnits", AiDifficulty::normal, 0) == -1);
  CHECK(profile.variable("AIV_Sleep_ES", AiDifficulty::hard, 0) == 2000);
  CHECK(profile.variable("AIV_Sleep_ES", AiDifficulty::easy, 0) == 3000);
  CHECK(profile.variable("AIV_NoRepair", AiDifficulty::none, -7) == -7);
}

TEST(ai_profile_takes_the_first_of_a_key_declared_twice_in_one_section) {
  // The retail `[Vars.All]` declares `AIV_SquanderGoldAmount`,
  // `AIV_SquanderFoolAmount` and `AIV_MaxRChariot` twice each, and the first
  // two carry different values. Win32's `GetPrivateProfileString` returns the
  // first match in a section; see `sim/ai_profile.hpp`, where this is recorded
  // as inferred from the platform rather than proven from the data.
  const auto parsed = AiProfile::parse(bytes_of(kBase));
  REQUIRE(parsed.ok());
  CHECK(parsed.value().variable("AIV_SquanderGoldAmount", AiDifficulty::none, 0) == 30000);
}

TEST(ai_profile_maps_the_difficulty_attribute_to_an_overlay) {
  // `playerdata/@difficulty` takes 0, 2 and 3 across the shipped containers and
  // there are three overlay sections, so the attribute cannot be a 0-based
  // index into them. Inferred; see the header.
  CHECK(ai_difficulty_overlay(0) == AiDifficulty::none);
  CHECK(ai_difficulty_overlay(1) == AiDifficulty::easy);
  CHECK(ai_difficulty_overlay(2) == AiDifficulty::normal);
  CHECK(ai_difficulty_overlay(3) == AiDifficulty::hard);
  CHECK(ai_difficulty_overlay(-1) == AiDifficulty::none);
  CHECK(ai_difficulty_overlay(9) == AiDifficulty::none);
}

TEST(ai_profile_overlay_inherits_what_it_does_not_declare) {
  const auto base = AiProfile::parse(bytes_of(kBase));
  REQUIRE(base.ok());
  const auto overlay = AiProfile::parse(bytes_of(kOverlay), &base.value());
  REQUIRE(overlay.ok());
  const AiProfile& profile = overlay.value();

  // Declared identically: same values.
  CHECK(profile.constant(AiEnum::squad_state, "SS_Wait") == 2);
  // Not declared at all: inherited wholesale.
  CHECK(profile.constant(AiEnum::economy_script, "ES_Village") == 2);
  // Declared but **empty**, which is the real `DEFENSIVE` case. Inheriting is
  // what keeps a shared script's `TS_` constants resolvable under every
  // profile, and `GETTACTICSCRIPT.VS` -- the only script that reads one --
  // lives in the shared directory rather than in a profile.
  CHECK(profile.constant(AiEnum::tactic_script, "TS_GaulTactic") == 2);

  // Scripts: the overlay replaces what it names and inherits the rest.
  CHECK(profile.scripts().size() == 3);
  CHECK(profile.script_signature("GetArmyNeed.vs") ==
        "int, GAIKA g, int idPlayer, bool bMin");
  CHECK(profile.script_signature("GAIKAMonitor.vs") == "void");

  // Variables: the overlay retunes Easy and leaves everything else alone.
  CHECK(profile.variable("AIV_Sleep_ES", AiDifficulty::easy, 0) == 5000);
  CHECK(profile.variable("AIV_Sleep_ES", AiDifficulty::hard, 0) == 2000);
  CHECK(profile.variable("AIV_MaxMilUnits", AiDifficulty::easy, 0) == 100);
}

TEST(ai_profile_lists_variables_in_declaration_order) {
  // Iteration order is state. Anything that walks the variables must see the
  // same sequence every run, and the base's order is that sequence.
  const auto parsed = AiProfile::parse(bytes_of(kBase));
  REQUIRE(parsed.ok());
  const auto all = parsed.value().variables(AiDifficulty::none);
  REQUIRE(all.size() == 3);
  CHECK(all[0].name == "AIV_Sleep_ES");
  CHECK(all[1].name == "AIV_MaxMilUnits");
  CHECK(all[2].name == "AIV_SquanderGoldAmount");

  const auto easy = parsed.value().variables(AiDifficulty::easy);
  REQUIRE(easy.size() == 3);
  CHECK(easy[1].name == "AIV_MaxMilUnits");
  CHECK(easy[1].value == 100);
}

TEST(ai_profile_rejects_a_malformed_file) {
  constexpr std::string_view kBroken = "[Unterminated\n";
  CHECK(!AiProfile::parse(bytes_of(kBroken)).ok());
}
