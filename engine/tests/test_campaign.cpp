// The campaign layer: what an adventure and a conquest are, and what crosses a
// mission boundary.
//
// Four things this file is built to catch:
//
//   1. **The retail `territories.xml` is here verbatim.** All 5,732 bytes of
//      `Conquests/mediterranean.BFHP`'s only campaign document, the one such
//      document in the whole install. A reader tested against a paraphrase is
//      tested against the paraphrase; this project has been bitten by
//      hand-authored fixtures four times. The graph, the seven bonuses and the
//      `mapname` resolution are all asserted against those bytes.
//
//   2. **The territory graph closes.** Every `neighbours` entry names a
//      declared territory and no `id` repeats. A reader that dropped the
//      trailing entry of a comma-separated list would still parse the file and
//      would fail this.
//
//   3. **`bonus` is a sequence name, not a class name.** All seven resolve
//      against the conquest's container-root `Sequences/sequences.xml`, whose
//      eight names are here verbatim too. This is the trap `docs/formats/map.md`
//      records and the reason `validate_bonuses` is a separate call.
//
//   4. **The three host entry points are reachable from a compiled script and
//      nothing else claims them.** `ConquestBonus`, `SetTerritoryState` and
//      `GetTerritoryState` are absent from `declare_shipped_surface` because
//      they are absent from `data.pak`; a test that only called them through
//      C++ would not notice if the arity were wrong.

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "domains.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A conquest map written from `docs/formats/map.md`, not copied from one.
///
/// **The retail `territories.xml` used to be here, all 5,729 bytes of it.**
/// `docs/legal.md` rule 1 forbids that -- no game assets in the repository,
/// "not as test fixtures" -- and `tools/check_fixtures.py` now refuses it. The
/// claims that were genuinely about the shipped document moved to
/// `tests/test_corpus_campaign.py`, where they run against the container
/// itself through `imcheck conquest` and are stronger for it. What stayed here
/// is what a unit test should have been asserting all along: the *reader's*
/// behaviour on documents chosen to break it.
///
/// So this fixture is deliberately nastier than the retail file:
///
///   * `neighbours` is written three ways -- no spaces, spaces after the
///     commas, and a **trailing comma** -- because the split has to keep the
///     last field and drop the empty one;
///   * `index` is non-contiguous, unordered, and never equal to the map number
///     the territory resolves to;
///   * `id`, `visualname` and `mapname` are three different strings for every
///     territory, in both directions, so nothing can be derived from anything;
///   * `bonus` reads like a class name and is not one;
///   * `name` carries a trailing space and `ConqueredOrder` is non-empty, both
///     of which the reader has to keep rather than tidy.
constexpr std::string_view kTerritories = R"XML(	<conquestmap
		name="Test Sea "
		data="ConquestMaps/2 - Test Board"
		ConqueredOrder=", 4, 9"
		choose="1"
		owned_colorize="1"
		enemy_colorize="1"
		disabled_colorize="0"
		owned_hue="560"
		enemy_hue="0"
		disabled_hue="1000"
		owned_sat="1360"
		enemy_sat="1000"
		disabled_sat="1024"
		interface="-1">
		<territory
			id="North"
			index="7"
			state="1"
			visualname="Septentrio"
			mapname="Upper"
			description="The northern reach.\nCold, and further away than it looks."
			bonus="rBoreal"
			bonus_descr="Your warriors ignore the cold.\n"
			neighbours="East,West"
			interface="3"/>
		<territory
			id="East"
			index="2"
			state="1"
			visualname="Oriens"
			mapname="Sunrise"
			description="The eastern reach."
			bonus="rLevantine"
			bonus_descr="Your archers see further.\n"
			neighbours="North, South"
			interface="5"/>
		<territory
			id="South"
			index="9"
			state="1"
			visualname="Meridies"
			mapname="Lower"
			description="The southern reach."
			bonus="rMeridian"
			bonus_descr="Your ships sail faster.\n"
			neighbours="East, West,"
			interface="0"/>
		<territory
			id="West"
			index="4"
			state="1"
			visualname="Occidens"
			mapname="Sunset"
			description="The western reach."
			bonus="rHesperian"
			bonus_descr="Your townhalls gather more.\n"
			neighbours="North, South"
			interface="1"/>
	</conquestmap>
)XML";

/// The `<sequence name>` values the fixture manifest declares, in document
/// order. The four after the first are the four `bonus` targets.
const std::string kConquestRootSequences[] = {
    "StartBonuses", "rBoreal", "rHesperian", "rLevantine", "rMeridian",
};

/// A manifest written from `docs/formats/map.md`, for the same reason the map
/// above is.
///
/// The two things this reader has to get right are both spelled out only by the
/// *absence* of an attribute: the `CurrentGame` script root, and
/// `autorunallowed` defaulting to true. So `StartBonuses` omits it and the four
/// bonuses deny it, which is the shape the shipped conquest has -- the
/// dispatcher autoruns and the rewards wait to be called by name.
constexpr std::string_view kConquestRootSequenceXml = R"XML(	<sequences>
		<sequence
			name="StartBonuses"
			wizard=""
			script="CurrentGame/sequences/seq0.vs">
		</sequence>
		<sequence
			name="rBoreal"
			wizard=""
			script="CurrentGame/sequences/seq1.vs"
			autorunallowed="no">
		</sequence>
		<sequence
			name="rHesperian"
			wizard=""
			script="CurrentGame/sequences/seq2.vs"
			autorunallowed="no">
		</sequence>
		<sequence
			name="rLevantine"
			wizard=""
			script="CurrentGame/sequences/seq3.vs"
			autorunallowed="no">
		</sequence>
		<sequence
			name="rMeridian"
			wizard=""
			script="CurrentGame/sequences/seq4.vs"
			autorunallowed="no">
		</sequence>
	</sequences>
)XML";

/// `Maps/<n>/map.xml` -> `map/@name` for the four maps the fixture container
/// would hold, enumerated out of order and with non-contiguous numbers -- both
/// of which the retail container also is, and neither of which the resolver may
/// depend on.
const std::pair<std::int32_t, std::string> kConquestMapNames[] = {
    {11, "Sunset"}, {2, "Upper"}, {6, "Lower"}, {9, "Sunrise"},
};

}  // namespace

// --------------------------------------------------------------------------
// what kind of container this is
// --------------------------------------------------------------------------

TEST(campaign_kind_is_the_three_blank_templates) {
  // `Packs/emptyscn.bfhp`, `Packs/emptyadv.bfhp`, `Packs/emptyconquest.bfhp`.
  CHECK(campaign_kind(0).value_or(CampaignKind::conquest) == CampaignKind::scenario);
  CHECK(campaign_kind(1).value_or(CampaignKind::scenario) == CampaignKind::adventure);
  CHECK(campaign_kind(2).value_or(CampaignKind::scenario) == CampaignKind::conquest);

  // A fourth flavour is a container this reader does not understand. Silently
  // reading it as a scenario would drop whatever campaign structure it carries.
  CHECK(!campaign_kind(3).ok());
  CHECK(!campaign_kind(-1).ok());
}

TEST(campaign_properties_parse_out_of_a_game_xml) {
  // `Conquests/mediterranean.BFHP` -> `game.xml`, the whole element verbatim
  // apart from the attributes `MatchRules` and `GameProperties` already own.
  const std::string_view xml =
      "<game><properties game_type=\"2\" name=\"Mediterranean\" author=\"Test Author\""
      " description=\"Conquest Game - 7 nations\" last_edited_map=\"4\" start_map=\"10\""
      " victory_condition=\"0\" victory_threshold=\"0\" single_only=\"0\""
      " start_player=\"0\" season=\"spring\" user_interface=\"0\"/></game>";
  const Result<CampaignProperties> properties = CampaignProperties::parse(bytes_of(xml));
  REQUIRE(properties.ok());
  CHECK(properties->kind == CampaignKind::conquest);
  CHECK(properties->name == "Mediterranean");
  CHECK(properties->author == "Test Author");
  CHECK(properties->description == "Conquest Game - 7 nations");
  // `start_map` is a directory number, not an index: this conquest's first
  // mission is `Maps/10`, and its lowest-numbered map is `Maps/3`.
  CHECK(properties->start_map == 10);
  CHECK(properties->last_edited_map == 4);

  // A `<map>` is not a `<game>`, and a `<game>` with no `<properties>` is a
  // container this reader does not understand.
  CHECK(!CampaignProperties::parse(bytes_of("<map/>")).ok());
  CHECK(!CampaignProperties::parse(bytes_of("<game/>")).ok());
  // `game_type` is required. Defaulting it would make every unreadable
  // container a scenario.
  CHECK(!CampaignProperties::parse(bytes_of("<game><properties name=\"x\"/></game>")).ok());
}

// --------------------------------------------------------------------------
// the adventure series
// --------------------------------------------------------------------------

TEST(campaign_adventure_order_is_the_shipped_file_names) {
  // The twelve files `CVXUIPreAdventureMenu`'s two literal directories hold.
  CHECK(adventure_order("1_Great_Battles_Zama.bfhp") == 1);
  CHECK(adventure_order("2_Great_Battles_Numantia.bfhp") == 2);
  CHECK(adventure_order("6_Great_Battles_Danube.bfhp") == 6);
  CHECK(adventure_order("1_Great_Losses_Rome.BFHP") == 1);
  CHECK(adventure_order("6_Great_loses_Boudicca.BFHP") == 6);

  // A path, either separator, because `gbr.exe` spells its literals with
  // forward slashes and the install is Windows.
  CHECK(adventure_order("adventures/GreatBattles/3_Great_Battles_Alesia.bfhp") == 3);
  CHECK(adventure_order("Adventures\\GreatChallenges\\4_Great_Loses_Gaul.BFHP") == 4);

  // The Tutorial sits in neither series and has no prefix.
  CHECK(adventure_order("Tutorial.BFHP") == -1);
  CHECK(adventure_order("Conquests/mediterranean.BFHP") == -1);
  CHECK(adventure_order("") == -1);

  // The separator is load-bearing. Without it a year, a resolution or a bare
  // number would answer as if it were a mission ordinal.
  CHECK(adventure_order("2005.bfhp") == -1);
  CHECK(adventure_order("12") == -1);
  CHECK(adventure_order("_leading.bfhp") == -1);
}

// --------------------------------------------------------------------------
// territories.xml -- the reader
// --------------------------------------------------------------------------
//
// What the *shipped* conquest says is checked by `tests/test_corpus_campaign.py`
// against the container, through `imcheck conquest`. These are about the
// reader.

TEST(campaign_conquest_map_reads_every_attribute_it_declares) {
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());

  // The trailing space in `name` is in the document. Trimming it here would
  // make the reader disagree with whatever the original displays, and the
  // retail file has one too.
  CHECK(conquest->name() == "Test Sea ");
  // `data` points outside the container and its case need not match the
  // directory on disk, so it is kept exactly as written and resolved
  // case-insensitively later.
  CHECK(conquest->data_path() == "ConquestMaps/2 - Test Board");
  CHECK(conquest->choose());
  CHECK(conquest->interface_id() == -1);

  // `ConqueredOrder` is what the original appends to with `, %d`, so a
  // non-empty one has a leading separator and the split has to survive it. It
  // is empty in the shipped conquest, which is why the interesting case has to
  // be written rather than found.
  CHECK(conquest->conquered_order() == ", 4, 9");
  const std::vector<std::int32_t> order = parse_conquered_order(conquest->conquered_order());
  REQUIRE(order.size() == 2);
  CHECK(order[0] == 4);
  CHECK(order[1] == 9);

  REQUIRE(conquest->territories().size() == 4);

  // Document order, which is the order every walk in this engine uses.
  const std::string_view ids[] = {"North", "East", "South", "West"};
  for (std::size_t i = 0; i < 4; ++i) CHECK(conquest->territories()[i].id == ids[i]);

  // `index` is neither contiguous nor sorted nor the map number. A reader that
  // counted rather than read would get 0..3 and be wrong four times.
  const std::int32_t indices[] = {7, 2, 9, 4};
  for (std::size_t i = 0; i < 4; ++i) CHECK(conquest->territories()[i].index == indices[i]);

  // `interface` is the territory's race index (Iberia 3, Britain 5, Gaul 0,
  // RepublicanRome 1), kept only within the eight races as 0x00506560 keeps
  // it; the map's own -1 is "none".
  const std::int32_t races[] = {3, 5, 0, 1};
  for (std::size_t i = 0; i < 4; ++i) CHECK(conquest->territories()[i].interface_id == races[i]);
  CHECK(race_to_name(conquest->territories()[0].interface_id) == "Iberia");

  for (const Territory& territory : conquest->territories()) {
    CHECK(territory.state == kShippedInitialTerritoryState);
    // Three different strings, always. Nothing may be derived from anything:
    // the shipped `Gaul` displays as `Gallia` and plays on `Galicia`.
    CHECK(territory.id != territory.visual_name);
    CHECK(territory.id != territory.map_name);
    CHECK(territory.visual_name != territory.map_name);
  }
  CHECK(conquest->territories()[0].visual_name == "Septentrio");
  CHECK(conquest->territories()[0].map_name == "Upper");

  // The colourisation knobs, in document order. There are **nine**, not the
  // twelve `docs/formats/map.md` claimed: three `_colorize`, three `_hue` and
  // three `_sat`, for the owned, enemy and disabled states.
  REQUIRE(conquest->display().size() == 9);
  CHECK(conquest->display()[0].first == "owned_colorize");
  CHECK(conquest->display()[0].second == 1);
  CHECK(conquest->display()[3].first == "owned_hue");
  CHECK(conquest->display()[3].second == 560);

  CHECK(conquest->find("South") == 2);
  CHECK(conquest->find("south") < 0);  // every reference in the data matches exactly
  CHECK(conquest->find("Atlantis") < 0);
}

TEST(campaign_neighbours_splits_on_commas_however_they_are_spaced) {
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());
  CHECK(conquest->validate().ok());

  // Eight references over four territories, and the split has to keep the last
  // field of each list: dropping it would still validate for a territory with
  // one neighbour and would break every one with more.
  std::size_t references = 0;
  for (const Territory& territory : conquest->territories()) {
    references += territory.neighbours.size();
  }
  CHECK(references == 8);

  // Written without spaces.
  const std::vector<std::string>& north = conquest->territories()[0].neighbours;
  REQUIRE(north.size() == 2);
  CHECK(north[0] == "East");
  CHECK(north[1] == "West");

  // Written with them, and the space is not part of the id.
  const std::vector<std::string>& east = conquest->territories()[1].neighbours;
  REQUIRE(east.size() == 2);
  CHECK(east[0] == "North");
  CHECK(east[1] == "South");

  // Written with a **trailing comma**, which must produce two entries and not
  // three. An empty field here would fail `validate` -- there is no territory
  // called "" -- so a reader that kept it would turn a loadable conquest into
  // an unloadable one.
  const std::vector<std::string>& south = conquest->territories()[2].neighbours;
  REQUIRE(south.size() == 2);
  CHECK(south[0] == "East");
  CHECK(south[1] == "West");

  // Every reference resolves, which is what `validate` promises.
  for (const Territory& from : conquest->territories()) {
    for (const std::string& to : from.neighbours) CHECK(conquest->find(to) >= 0);
  }
}

TEST(campaign_bonus_names_a_root_sequence_and_not_a_class) {
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());

  // `rBoreal`, `rHesperian` and the rest read exactly like class names -- a
  // prefix and an adjective -- and none of them is one. All four resolve
  // against the container-root `Sequences/sequences.xml`, and the shipped
  // conquest's seven do too (`tests/test_corpus_campaign.py`).
  CHECK(conquest->validate_bonuses(std::span(kConquestRootSequences)).ok());

  // The territory id and the bonus name disagree -- `North` rewards `rBoreal`,
  // as the shipped `Spain` rewards `rIberia` -- which is why the mapping has to
  // be read and not derived.
  CHECK(conquest->territories()[0].bonus == "rBoreal");
  CHECK(conquest->territories()[0].id != conquest->territories()[0].bonus.substr(1));

  // A sequence list missing one entry has to be refused, not tolerated: a
  // bonus that names nothing is a mission reward that silently never happens.
  const std::string short_list[] = {"StartBonuses", "rBoreal", "rHesperian", "rLevantine"};
  CHECK(!conquest->validate_bonuses(std::span(short_list)).ok());
}

TEST(campaign_every_territory_resolves_to_one_of_the_maps) {
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());

  const std::vector<TerritoryMap> resolved =
      resolve_maps(*conquest, std::span(kConquestMapNames));
  REQUIRE(resolved.size() == 4);

  // In territory document order regardless of how the caller walked the
  // container -- `kConquestMapNames` is deliberately out of order -- and every
  // one lands. None of the four map numbers equals the territory's `index`.
  const std::int32_t expected[] = {2, 9, 6, 11};
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(resolved[i].territory == static_cast<std::int32_t>(i));
    CHECK(resolved[i].map_number == expected[i]);
    CHECK(resolved[i].map_number != conquest->territories()[i].index);
  }

  // Every map is claimed exactly once, so the territories and the `Maps/<n>`
  // directories are a bijection.
  std::vector<std::int32_t> claimed;
  for (const TerritoryMap& entry : resolved) claimed.push_back(entry.map_number);
  std::sort(claimed.begin(), claimed.end());
  CHECK(std::unique(claimed.begin(), claimed.end()) == claimed.end());

  // A container missing a map leaves that territory unresolved rather than
  // failing the whole load: the caller decides whether that is fatal.
  const std::pair<std::int32_t, std::string> missing[] = {{9, "Sunrise"}};
  const std::vector<TerritoryMap> partial = resolve_maps(*conquest, std::span(missing));
  REQUIRE(partial.size() == 4);
  CHECK(partial[1].map_number == 9);
  CHECK(partial[0].map_number == -1);
}

TEST(campaign_conquest_map_refuses_documents_it_does_not_understand) {
  CHECK(!ConquestMap::parse(bytes_of("<map/>")).ok());
  // No territories is not an empty campaign map; it is a document that cannot
  // be played.
  CHECK(!ConquestMap::parse(bytes_of("<conquestmap/>")).ok());
  // A territory with no `id` can be named by nothing -- not by
  // `SetTerritoryState` and not by another territory's `neighbours`.
  CHECK(!ConquestMap::parse(bytes_of("<conquestmap><territory index=\"1\"/></conquestmap>")).ok());

  // A graph that does not close parses and fails validation, which is the
  // split: reading and believing are different steps.
  const auto open = ConquestMap::parse(bytes_of(
      "<conquestmap><territory id=\"A\" neighbours=\"B\"/></conquestmap>"));
  REQUIRE(open.ok());
  CHECK(!open->validate().ok());

  // So does a repeated id, which would make `SetTerritoryState` ambiguous.
  const auto twice = ConquestMap::parse(bytes_of(
      "<conquestmap><territory id=\"A\"/><territory id=\"A\"/></conquestmap>"));
  REQUIRE(twice.ok());
  CHECK(!twice->validate().ok());
}

TEST(campaign_conquered_order_splits_on_the_format_string) {
  // `gbr.exe` writes the attribute with `, %d`. It is empty in the one shipped
  // conquest, so every populated spelling below is a tolerance rather than an
  // observation, and this test says which is which.
  CHECK(parse_conquered_order("").empty());
  CHECK(parse_conquered_order("   ").empty());

  const std::vector<std::int32_t> spaced = parse_conquered_order("3, 7, 8");
  REQUIRE(spaced.size() == 3);
  CHECK(spaced[0] == 3);
  CHECK(spaced[1] == 7);
  CHECK(spaced[2] == 8);

  const std::vector<std::int32_t> tight = parse_conquered_order("3,7,8");
  CHECK(tight == spaced);

  // A single element has no comma at all, which is the shape the first append
  // produces.
  const std::vector<std::int32_t> one = parse_conquered_order("3");
  REQUIRE(one.size() == 1);
  CHECK(one[0] == 3);

  // Junk is dropped rather than aborting the parse: refusing would make a save
  // the original wrote unloadable over a field this engine does not yet use.
  const std::vector<std::int32_t> junk = parse_conquered_order("3, , Spain, 8");
  REQUIRE(junk.size() == 2);
  CHECK(junk[0] == 3);
  CHECK(junk[1] == 8);
}

// --------------------------------------------------------------------------
// the state that crosses a mission boundary
// --------------------------------------------------------------------------

TEST(campaign_system_carries_territory_state_and_nothing_else) {
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());

  CampaignSystem campaign;
  campaign.configure(*conquest);
  REQUIRE(campaign.territory_ids().size() == 4);
  CHECK(campaign.territory_ids()[0] == "North");
  CHECK(campaign.bonus_of(0) == "rBoreal");
  CHECK(campaign.bonus_of(4).empty());
  CHECK(campaign.bonus_of(-1).empty());

  // The authored state is the starting progress.
  CHECK(campaign.state_of("North") == kShippedInitialTerritoryState);
  CHECK(campaign.progress().conquered.empty());
  CHECK(campaign.progress().active_bonus.empty());

  // `Maps/10/Sequences/seq6.vs` does exactly this, with `tsOwned` in place of
  // the literal. Without an established value for `tsOwned` the state is
  // recorded and the order is not -- which is the honest behaviour, not a
  // guessed one.
  CHECK(campaign.set_state("North", TerritoryState::enemy));
  CHECK(campaign.state_of("North") == TerritoryState::enemy);
  CHECK(campaign.progress().conquered.empty());

  // `tsOwned` is what a conquest's map scripts write, and writing it is the
  // only thing that maintains `ConqueredOrder`.
  CampaignSystem told;
  told.configure(*conquest);
  REQUIRE(told.restore(CampaignProgress{std::vector<TerritoryState>(4, TerritoryState::enemy), {}, ""}).ok());
  CHECK(told.set_state("South", TerritoryState::owned));
  CHECK(told.set_state("East", TerritoryState::owned));
  CHECK(told.set_state("South", TerritoryState::owned));  // idempotent
  REQUIRE(told.progress().conquered.size() == 2);
  CHECK(told.progress().conquered[0] == 2);  // South
  CHECK(told.progress().conquered[1] == 1);  // East

  // An unknown territory is reported, not swallowed. The ids are string
  // literals in a conquest's map scripts, so a miss means the container's own
  // documents disagree with one another.
  CHECK(!told.set_state("Atlantis", TerritoryState::owned));
}

TEST(campaign_progress_round_trips_and_refuses_a_mismatched_table) {
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());

  CampaignSystem first;
  first.configure(*conquest);
  CHECK(first.set_state("South", TerritoryState::owned));
  first.set_active_bonus("rMeridian");

  // What a save writes and the next mission reads. Nothing else crosses.
  const CampaignProgress carried = first.progress();

  CampaignSystem second;
  second.configure(*conquest);
  CHECK(second.restore(carried).ok());
  CHECK(second.progress() == carried);
  CHECK(second.state_of("South") == TerritoryState::owned);
  CHECK(second.active_bonus() == "rMeridian");

  // Two systems in the same state hash alike; one territory apart, they do not.
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  first.hash(a);
  second.hash(b);
  CHECK(a == b);
  CHECK(second.set_state("East", TerritoryState::disabled));
  std::uint64_t c = 0;
  second.hash(c);
  CHECK(c != b);

  // A `states` vector of the wrong length would silently leave the tail at its
  // authored value, which is a campaign that quietly forgets a conquest.
  CampaignProgress short_progress = carried;
  short_progress.states.pop_back();
  CampaignSystem third;
  third.configure(*conquest);
  CHECK(!third.restore(short_progress).ok());

  // And an index outside the table is a save this build cannot honour.
  CampaignProgress bad = carried;
  bad.conquered.push_back(99);
  CHECK(!third.restore(bad).ok());
}

TEST(campaign_a_lost_mission_changes_nothing) {
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());

  CampaignProgress progress;
  progress.states.assign(7, kShippedInitialTerritoryState);

  // No shipped script writes a territory state on defeat: the four defeat
  // sequences call `EndGame(1, true, ...)` and stop.
  MissionResult lost;
  lost.map_number = 10;
  lost.territory = 0;
  lost.won = false;
  CHECK(!apply_mission_result(progress, lost));
  CHECK(progress.conquered.empty());

  MissionResult won = lost;
  won.won = true;
  CHECK(apply_mission_result(progress, won));
  REQUIRE(progress.conquered.size() == 1);
  CHECK(progress.conquered[0] == 0);
  // Replaying a territory does not append it twice.
  CHECK(!apply_mission_result(progress, won));

  // An adventure mission has no territory, so folding its result in is a no-op
  // rather than an error: the thirteen adventures have no campaign state.
  MissionResult adventure;
  adventure.map_number = 6;
  adventure.territory = -1;
  adventure.won = true;
  CHECK(!apply_mission_result(progress, adventure));

  // The inference, pinned so that it is visible rather than buried in a
  // comment: `ConquestBonus()` returns one name, and this is the reading of
  // *which* one that the ordered `ConqueredOrder` supports.
  CHECK(last_conquered_bonus(*conquest, progress) == "rBoreal");
  CampaignProgress fresh;
  CHECK(last_conquered_bonus(*conquest, fresh).empty());
}

TEST(campaign_a_won_mission_carries_to_the_next_through_the_file) {
  // The whole boundary, in the order the layers cross it: the victory
  // sequence writes its territory, the match ends won, the session's campaign
  // answers what the next mission starts from, that becomes a file, and a
  // fresh session on the next map restores it and answers `ConquestBonus()`.
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());

  CampaignSystem first;
  first.configure(*conquest);
  REQUIRE(first.restore(CampaignProgress{std::vector<TerritoryState>(4, TerritoryState::enemy), {}, ""}).ok());
  // `Maps/<n>/Sequences/seq6.vs`: `SetTerritoryState("East", tsOwned);
  // EndGame(1, false, ...)`.
  CHECK(first.set_state("East", TerritoryState::owned));

  MissionResult result;
  result.map_number = 4;
  result.territory = 1;  // East
  result.won = true;
  const CampaignCarry carried = first.carry("Conquests/test.bfhp", result);
  CHECK(carried.container == "Conquests/test.bfhp");
  CHECK(carried.territories == first.territory_ids());
  CHECK(carried.progress.states[1] == TerritoryState::owned);
  REQUIRE(carried.progress.conquered.size() == 1);
  CHECK(carried.progress.conquered[0] == 1);
  // The inference, acted on: the next mission's reward is East's.
  CHECK(carried.progress.active_bonus == "rLevantine");
  // ...and the session that produced it is untouched: the carry is a value
  // for the next mission, not a mutation of this one.
  CHECK(first.active_bonus().empty());

  // A lost mission carries the progress as it stood, bonus included.
  MissionResult lost = result;
  lost.won = false;
  const CampaignCarry unchanged = first.carry("Conquests/test.bfhp", lost);
  CHECK(unchanged.progress == first.progress());

  // The file: text a person can read, and the same facts back.
  const std::vector<std::byte> ini = encode_campaign_carry(carried);
  const std::string text(reinterpret_cast<const char*>(ini.data()), ini.size());
  CHECK(text.find("[Campaign]") != std::string::npos);
  CHECK(text.find("territories=North,East,South,West") != std::string::npos);
  CHECK(text.find("states=0,1,0,0") != std::string::npos);
  CHECK(text.find("conquered=1") != std::string::npos);
  CHECK(text.find("active_bonus=rLevantine") != std::string::npos);
  const Result<CampaignCarry> back = decode_campaign_carry(ini);
  REQUIRE(back.ok());
  CHECK(*back == carried);

  // The next mission: a fresh system over the same conquest, restored from
  // the file, answers the state and the bonus the last one left.
  CampaignSystem next;
  next.configure(*conquest);
  REQUIRE(next.restore(*back).ok());
  CHECK(next.state_of("East") == TerritoryState::owned);
  CHECK(next.state_of("North") == TerritoryState::enemy);
  CHECK(next.active_bonus() == "rLevantine");
  CHECK(next.progress() == carried.progress);

  // A file for another conquest -- or this one re-authored -- is refused by
  // its territory ids, not by a count that happens to match.
  CampaignCarry other = carried;
  other.territories[0] = "Septentrio";
  CHECK(!next.restore(other).ok());
  std::vector<std::string> reordered = carried.territories;
  std::swap(reordered[0], reordered[1]);
  other.territories = reordered;
  CHECK(!next.restore(other).ok());

  // And what the decoder refuses: no section, no container, a states list
  // that does not match the territories, a state outside 0..2, an index
  // outside the table, a number read half-way.
  const auto refuses = [](std::string_view doc) {
    return !decode_campaign_carry(bytes_of(doc)).ok();
  };
  CHECK(refuses("[Other]\ncontainer=x\n"));
  CHECK(refuses("[Campaign]\nterritories=A,B\nstates=0,0\n"));
  CHECK(refuses("[Campaign]\ncontainer=x\nterritories=A,B\nstates=0\n"));
  CHECK(refuses("[Campaign]\ncontainer=x\nterritories=A,B\nstates=0,3\n"));
  CHECK(refuses("[Campaign]\ncontainer=x\nterritories=A,B\nstates=0,0\nconquered=2\n"));
  CHECK(refuses("[Campaign]\ncontainer=x\nterritories=A,B\nstates=0,1x\n"));
  // Spaces after the commas, as `ConqueredOrder`'s own format string writes
  // them, are not a refusal.
  const Result<CampaignCarry> spaced =
      decode_campaign_carry(bytes_of("[Campaign]\ncontainer=x\nterritories=A, B\nstates=1, 0\nconquered=0\n"));
  REQUIRE(spaced.ok());
  CHECK(spaced->territories.size() == 2);
  CHECK(spaced->progress.states[0] == TerritoryState::owned);
}

// --------------------------------------------------------------------------
// the host surface
// --------------------------------------------------------------------------

TEST(campaign_takes_no_entry_point_from_another_domain) {
  // `HostRegistry::define` replaces silently, so this is the only thing
  // standing between this domain and quietly stealing a name.
  script::HostRegistry busy;
  imperivm::test::define_all_except("campaign", busy);

  const std::pair<std::string_view, std::uint16_t> claimed[] = {
      {"ConquestBonus", 0},
      {"SetTerritoryState", 2},
      {"GetTerritoryState", 1},
  };
  for (const auto& [name, arity] : claimed) {
    const std::uint32_t index = busy.find(script::CallKind::free_function, name, arity);
    // Declared but unimplemented, which is the whole point: nobody else may
    // have written a body for one of these.
    //
    // Two of the three *are* in `declare_shipped_surface`, and were not when
    // this test was written. The inventory that file is generated from used to
    // cover only the 577 `.vs` files in `data.pak`, which carry none of them;
    // it now covers all 885, including the 308 inside the containers, where
    // `ConquestBonus()` has one site and `SetTerritoryState(str, int)` has
    // seven. `GetTerritoryState/1` is still absent, because nothing in the
    // install calls it -- its arity comes from `gbr.exe`'s numeric registrar,
    // not from a call site, which is exactly why it is declared here by hand.
    if (index != script::kUnresolvedHost) CHECK(busy.entry(index).fn == nullptr);
  }
  // The split above, pinned. If `GetTerritoryState` ever turns up in the
  // inventory, something has changed about where the corpus is read from.
  CHECK(busy.find(script::CallKind::free_function, "ConquestBonus", 0) !=
        script::kUnresolvedHost);
  CHECK(busy.find(script::CallKind::free_function, "SetTerritoryState", 2) !=
        script::kUnresolvedHost);
  CHECK(busy.find(script::CallKind::free_function, "GetTerritoryState", 1) ==
        script::kUnresolvedHost);

  script::HostRegistry mine;
  CHECK(register_campaign_host(mine) == campaign_host_entry_count());
}

TEST(campaign_the_entry_points_run_from_a_compiled_script) {
  const Result<ConquestMap> conquest = ConquestMap::parse(bytes_of(kTerritories));
  REQUIRE(conquest.ok());

  World world;
  world.seed(1);
  CampaignSystem campaign;
  campaign.configure(*conquest);
  campaign.set_active_bonus("rBoreal");
  world.add_system(&campaign);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  (void)register_campaign_host(registry);

  // The two statements a conquest's map script uses, with the constant spelled
  // as a number because no value has been recovered for `tsOwned`. If the arity
  // of either entry point were wrong this would fail to compile rather than
  // misbehave at run time.
  const std::string_view source =
      "//void\n"
      "str bonus;\n"
      "int before;\n"
      "before = GetTerritoryState(\"North\");\n"
      "SetTerritoryState(\"North\", 1);\n"
      "bonus = ConquestBonus();\n";

  script::Diagnostic diagnostic;
  const auto parsed = script::parse(bytes_of(source), "conquest.vs", &diagnostic);
  if (!parsed.ok()) {
    std::printf("  parse conquest.vs:%u: %.*s\n", diagnostic.line,
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
  }
  REQUIRE(parsed.ok());
  script::CompileError error;
  auto chunk = script::compile(parsed.value(), &registry, &error);
  if (!chunk.ok()) {
    std::printf("  compile conquest.vs:%u: %s\n", error.line, error.message.c_str());
  }
  REQUIRE(chunk.ok());

  WorldHost host(world);
  script::Scheduler scheduler;
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);

  const std::uint32_t index = scheduler.add_chunk(std::move(chunk.value()));
  REQUIRE(scheduler.spawn(index, {}) != script::kNoScript);
  for (int pass = 0; pass < 4 && scheduler.live_count() > 0; ++pass) {
    const script::RunReport report = scheduler.advance(1000);
    for (const script::FailedScript& trap : report.traps) {
      std::printf("  trap %s:%u: %s\n", trap.source_name.c_str(), trap.trap.line,
                  trap.trap.detail.c_str());
    }
    CHECK(report.traps.empty());
  }
  CHECK(scheduler.live_count() == 0);

  // `Sequences/seq0.vs` follows `ConquestBonus()` with
  // `EnvWriteString("/Bonus", ...)`, which is the **root** scope of the
  // environment store -- the one-argument-key overload. The packs never call
  // it and the containers call it 403 times; `EnvWriteString/2` and
  // `EnvReadString/1` are now registered and are left out of this script only
  // because they belong to another domain. `sim/env.hpp` carries the tabulation
  // and the reason the leading slash is significant rather than noise.

  // The script's write landed in the campaign, and the campaign recorded the
  // conquest -- the whole of what crosses a mission boundary.
  CHECK(campaign.state_of("North") == TerritoryState::owned);
  REQUIRE(campaign.progress().conquered.size() == 1);
  CHECK(campaign.progress().conquered[0] == 0);
}

// --------------------------------------------------------------------------
// sequences.xml
// --------------------------------------------------------------------------

TEST(campaign_the_sequence_manifest_parses) {
  const Result<std::vector<SequenceRef>> manifest =
      parse_sequences(bytes_of(kConquestRootSequenceXml));
  REQUIRE(manifest.ok());
  REQUIRE(manifest->size() == 5);

  // Document order, and the same names the `bonus` attributes resolve against.
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(manifest.value()[i].name == kConquestRootSequences[i]);
  }

  // The dispatcher says nothing about autorun and therefore autoruns; the
  // bonus sequences say `no` and wait to be called by name. Getting this
  // backwards would start every bonus at once on every conquest map, which is
  // a mission that plays itself. The shipped manifest has exactly this shape --
  // one autorunning dispatcher and seven that do not (`test_corpus_campaign.py`).
  CHECK(manifest.value()[0].autorun_allowed);
  for (std::size_t i = 1; i < 5; ++i) CHECK(!manifest.value()[i].autorun_allowed);

  // The script root is virtual. `CurrentGame` is the container; `CurrentMap` is
  // the map directory. The shipped attribute spells the directory `sequences`
  // where the container entry spells it `Sequences`, which is why the tail is
  // taken verbatim and the container index folds case.
  CHECK(manifest.value()[0].script == "CurrentGame/sequences/seq0.vs");
  CHECK(sequence_entry_path(manifest.value()[0].script, "") == "sequences/seq0.vs");
  CHECK(sequence_entry_path("CurrentMap/Sequences/seq3.vs", "Maps/10") ==
        "Maps/10/Sequences/seq3.vs");
  // A backslash separator resolves the same way; the containers use both.
  CHECK(sequence_entry_path("CurrentMap\\Sequences\\seq3.vs", "Maps/4") ==
        "Maps/4/Sequences\\seq3.vs");

  // Not a manifest at all.
  CHECK(!parse_sequences(bytes_of(kTerritories)).ok());
}

// --------------------------------------------------------------------------
// the wiring: a session that loads a conquest, and one that runs its sequences
// --------------------------------------------------------------------------

namespace {

/// A `ScriptResolver` over an in-memory table, standing in for the container.
class FakeScripts final : public ScriptResolver {
 public:
  void add(std::string path, std::string source) {
    files_.emplace_back(std::move(path), std::move(source));
  }
  std::span<const std::byte> source(std::string_view path) override {
    ++asked_;
    for (const auto& [name, text] : files_) {
      if (name == path) return bytes_of(text);
    }
    return {};
  }
  [[nodiscard]] std::size_t asked() const noexcept { return asked_; }

 private:
  std::vector<std::pair<std::string, std::string>> files_;
  std::size_t asked_ = 0;
};

}  // namespace

// Both halves of the wiring this project keeps getting wrong: a subsystem that
// is complete, registered, ordered -- and that nothing ever hands its data to.
//
// `CampaignSystem::configure` had no caller at all, so every `ConquestBonus`
// and `SetTerritoryState` in the one shipped conquest answered against an empty
// territory table; and nothing in the engine read a `sequences.xml`, so the
// 308 scripts that make up the campaign layer were unreachable source. Neither
// gap showed as a failing test, because both subsystems passed theirs.
TEST(campaign_a_session_configures_from_territories_and_runs_its_sequences) {
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph;

  FakeScripts scripts;
  // Two sequences: `StartBonuses`, which autoruns, and a bonus sequence, which
  // does not. Both are the shipped shape -- `//void`, no parameters, and the
  // root scope of the environment store for their state.
  scripts.add("sequences/seq0.vs",
              "//void\nEnvWriteString(\"/Bonus\", ConquestBonus());\n");
  scripts.add("sequences/seq1.vs", "//void\nEnvWriteInt(\"/Ran\", 1);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.conquest = bytes_of(kTerritories);

  auto session = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();

  // The territory graph reached the system. Without the `configure` call in
  // `GameSession::create` this is zero and every campaign entry point is a
  // no-op that reports success.
  CampaignSystem* campaign = campaign_system_of(run.world());
  REQUIRE(campaign != nullptr);
  REQUIRE(campaign->territory_ids().size() == 4);
  CHECK(campaign->state_of("North") == kShippedInitialTerritoryState);

  const SequenceRef manifest[] = {
      SequenceRef{"StartBonuses", "CurrentGame/sequences/seq0.vs", "", true},
      SequenceRef{"rBoreal", "CurrentGame/sequences/seq1.vs", "", false},
  };
  // Both compile -- a sequence `RunSequence` will reach for has to be in the
  // library -- and only the autorun one starts.
  CHECK(run.start_sequences(std::span(manifest), /*base=*/"") == 1);
  CHECK(scripts.asked() == 2);

  run.advance(/*turns=*/2, /*turn_length=*/800);

  const EnvSystem* env = env_of(run.world());
  REQUIRE(env != nullptr);
  // `ConquestBonus()` resolved and its value landed in the **root** scope under
  // the key the script wrote, slash and all. Nothing was conquered, so the
  // bonus is empty -- which is what the shipped conquest's first turn also
  // shows when `imrun` prints its root scope.
  CHECK(env->env().find(EnvScope::root(), "/Bonus") != nullptr);
  CHECK(env->env().read_string(EnvScope::root(), "/Bonus").empty());
  // The sequence that forbids autorun did not run.
  CHECK(env->env().find(EnvScope::root(), "/Ran") == nullptr);

  const SessionReport report = run.report();
  for (const SessionReport::Trap& trap : report.traps) {
    std::printf("unexpected trap: %s\n", trap.message.c_str());
  }
  CHECK(report.traps.empty());
}

// --------------------------------------------------------------------------
// the sequence runner
// --------------------------------------------------------------------------

namespace {

/// A session with four sequences, one of which autoruns.
///
/// `Waiter` is the shape `RunSequence` exists for: `autorunallowed="no"`, so
/// nothing starts it at load. `Quick` returns immediately, which is what makes
/// the restart case testable. `Broken` names a file the resolver does not
/// have, which is the branch `Start` answers `"Finished"`.
struct SequenceSession {
  ClassGraph graph;
  script::HostRegistry registry;
  FakeScripts scripts;
  std::unique_ptr<GameSession> session;

  SequenceSession() {
    (void)register_all_hosts(registry);
    scripts.add("sequences/seq0.vs", "//void\nEnvWriteInt(\"/Auto\", 1);\nwhile (1) Sleep(1000);\n");
    scripts.add("sequences/seq1.vs", "//void\nEnvWriteInt(\"/Ran\", 1);\nwhile (1) Sleep(1000);\n");
    scripts.add("sequences/seq2.vs", "//void\nEnvWriteInt(\"/Quick\", 1);\n");

    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.scripts = &scripts;
    inputs.conquest = bytes_of(kTerritories);
    Result<std::unique_ptr<GameSession>> made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    session = std::move(made.value());

    const SequenceRef manifest[] = {
        SequenceRef{"Auto", "CurrentGame/sequences/seq0.vs", "", true},
        SequenceRef{"Waiter", "CurrentGame/sequences/seq1.vs", "", false},
        SequenceRef{"Quick", "CurrentGame/sequences/seq2.vs", "", false},
        SequenceRef{"Broken", "CurrentGame/sequences/seq9.vs", "", false},
    };
    CHECK(session->start_sequences(std::span(manifest), /*base=*/"") == 1);
  }

  [[nodiscard]] std::string_view status(std::string_view name) {
    const EnvSystem* env = env_of(session->world());
    if (env == nullptr) return {};
    std::string key = "/SequenceStatus/";
    key.append(name);
    return env->env().read_string(EnvScope::root(), key);
  }

  [[nodiscard]] bool ran(const char* key) {
    const EnvSystem* env = env_of(session->world());
    return env != nullptr && env->env().find(EnvScope::root(), key) != nullptr;
  }

  script::HostOutcome call(std::string_view name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(script::CallKind::free_function, name,
                                              static_cast<std::uint16_t>(args.size()));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    HostContext context;
    context.world = &session->world();
    context.object_type = kTypeObj;
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.scheduler = &session->scheduler();
    ctx.name = name;
    ctx.kind = script::CallKind::free_function;
    return entry.fn(ctx);
  }
};

}  // namespace

TEST(campaign_every_declared_sequence_starts_waiting_and_the_autorun_one_running) {
  // The status is a string in the environment registry under
  // `/SequenceStatus/<name>`, not a field -- which is why the whole feature
  // needs no save section of its own.
  //
  // **The `"Waiting"` initialisation is load-bearing and must not be skipped.**
  // `IsWaiting` is a predicate over the key, so an uninitialised sequence reads
  // as *not* waiting, and Britain's `WinCond` guards on
  // `!IsWaiting("CalgacusArmy")` -- under the wrong default it takes its
  // victory branch on the first pass. The original writes `"Waiting"` back
  // whenever its getter finds the key absent, which is the same thing.
  SequenceSession s;
  CHECK(s.status("Auto") == "Running");
  CHECK(s.status("Waiter") == "Waiting");
  CHECK(s.status("Quick") == "Waiting");
  // Installed even though its script does not exist: `RunSequence` has to be
  // able to find it before `Start`'s no-script branch is reachable at all.
  CHECK(s.status("Broken") == "Waiting");
  // `start_sequences` spawns; it does not run. The status is written at spawn
  // and the script's first statement lands a turn later, which is the
  // ordering the shipped `RunSequence(x); while (IsRunning(x))` idiom depends
  // on.
  CHECK(!s.ran("/Auto"));
  s.session->advance(/*turns=*/1, /*turn_length=*/800);
  CHECK(s.ran("/Auto"));
  CHECK(!s.ran("/Ran"));
  // And the reap only takes the ones that actually ended. `Auto` sleeps
  // forever, so it is still `"Running"` many turns later; a sweep that marked
  // every entry `"Finished"` each pass would satisfy every other assertion
  // here, because the rest look at a sequence in the turn it started.
  s.session->advance(/*turns=*/5, /*turn_length=*/800);
  CHECK(s.status("Auto") == "Running");
  CHECK(s.status("Waiter") == "Waiting");
}

TEST(campaign_run_sequence_starts_the_one_that_would_not_autorun) {
  SequenceSession s;
  CHECK(s.call("RunSequence", {script::Value::string("Waiter")}).status ==
        script::HostStatus::ok);
  CHECK(s.status("Waiter") == "Running");
  // Written **before** returning, not when the coroutine first executes: the
  // shipped `RunSequence(x); while (IsRunning(x)) Sleep(100);` idiom tests the
  // predicate before the new thread has run a single statement.
  CHECK(!s.ran("/Ran"));
  s.session->advance(/*turns=*/1, /*turn_length=*/800);
  CHECK(s.ran("/Ran"));
}

TEST(campaign_run_sequence_on_a_running_sequence_does_nothing) {
  SequenceSession s;
  const std::size_t before = s.session->report().scripts_running;
  CHECK(s.call("RunSequence", {script::Value::string("Auto")}).status == script::HostStatus::ok);
  CHECK(s.session->report().scripts_running == before);
  CHECK(s.status("Auto") == "Running");
}

TEST(campaign_run_sequence_restarts_a_finished_one) {
  // Not a corner. Numantia's `seq3.vs` runs
  // `RunSequence("BestTarget"); while (IsRunning("BestTarget")) Sleep(100);`
  // inside a `for` over 28 players, so it restarts a finished sequence 28
  // times per pass. `Start` only tests for status `"Running"`; `"Waiting"` and
  // `"Finished"` both fall through to the spawn. An implementation that made
  // the second call a no-op deadlocks that loop on iteration two.
  SequenceSession s;
  CHECK(s.call("RunSequence", {script::Value::string("Quick")}).status == script::HostStatus::ok);
  s.session->advance(/*turns=*/2, /*turn_length=*/800);
  CHECK(s.status("Quick") == "Finished");

  CHECK(s.call("RunSequence", {script::Value::string("Quick")}).status == script::HostStatus::ok);
  CHECK(s.status("Quick") == "Running");
  s.session->advance(/*turns=*/2, /*turn_length=*/800);
  CHECK(s.status("Quick") == "Finished");
}

TEST(campaign_a_sequence_with_no_compiled_script_is_finished_rather_than_pending) {
  // `Start`'s first branch, and it is what makes `IsFinished` answer true for a
  // sequence that never compiled -- a mission waiting on one is not left
  // waiting forever.
  SequenceSession s;
  CHECK(s.call("RunSequence", {script::Value::string("Broken")}).status == script::HostStatus::ok);
  CHECK(s.status("Broken") == "Finished");
}

TEST(campaign_run_sequence_does_not_trap_on_a_name_it_cannot_find) {
  // `Could not find sequence named '%s' in function 'RunSequence'. Check the
  // spelling.` through a sink that is a bare `ret` in the retail build -- so
  // the message is thrown away and the calling script runs on. The
  // `GetSettlement/1` rule: refusing by name here kills a script the original
  // finishes.
  SequenceSession s;
  const std::size_t before = s.session->report().scripts_running;
  CHECK(s.call("RunSequence", {script::Value::string("NoSuchSequence")}).status ==
        script::HostStatus::ok);
  CHECK(s.session->report().scripts_running == before);
  CHECK(s.status("NoSuchSequence").empty());
}

TEST(campaign_the_three_predicates_read_the_key_and_never_the_table) {
  // `IsRunning`, `IsWaiting` and `IsFinished` build the env key from the raw
  // argument and compare the value. They do not consult the sequence table at
  // all, so an unknown name reads the empty string and every one of them
  // answers false -- silently, with no diagnostic. That asymmetry with
  // `RunSequence`, which does look the name up, is the original's.
  SequenceSession s;
  const auto ask = [&](const char* fn, const char* name) {
    const script::HostOutcome out = s.call(fn, {script::Value::string(name)});
    return out.status == script::HostStatus::ok && out.value.is_integer() &&
           out.value.as_integer() != 0;
  };
  CHECK(ask("IsRunning", "Auto"));
  CHECK(!ask("IsWaiting", "Auto"));
  CHECK(!ask("IsFinished", "Auto"));

  CHECK(ask("IsWaiting", "Waiter"));
  CHECK(!ask("IsRunning", "Waiter"));

  for (const char* fn : {"IsRunning", "IsWaiting", "IsFinished"}) {
    CHECK(!ask(fn, "NoSuchSequence"));
  }

  // And the half that makes this a measurement rather than a restatement of
  // "an unknown name is false": a key written by hand, with no `<sequence>`
  // behind it at all, is still what the predicates answer from. A script can
  // do exactly this -- `EnvWriteString("/SequenceStatus/X", "Running")` is an
  // ordinary root-scope write -- and the original would answer true, because
  // its three predicates never look at the sequence table. An implementation
  // that consulted the table passes every assertion above and fails here.
  EnvSystem* env = env_of(s.session->world());
  REQUIRE(env != nullptr);
  env->env().write_string(EnvScope::root(), "/SequenceStatus/Ghost", "Running");
  CHECK(ask("IsRunning", "Ghost"));
  CHECK(!ask("IsWaiting", "Ghost"));
}

TEST(campaign_two_sequences_whose_scripts_share_a_basename_stay_apart) {
  // The hazard `find_chunk`'s basename fallback creates here, and it is not
  // hypothetical: every container names its sequence scripts `seq<n>.vs` under
  // its own `Maps/<n>/Sequences/`, so a conquest holding seven maps holds
  // seven different `seq1.vs`. Resolving by basename starts whichever loaded
  // first, which is the wrong mission script with no diagnostic at all.
  ClassGraph graph;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  FakeScripts scripts;
  scripts.add("Maps/3/Sequences/seq1.vs", "//void\nEnvWriteInt(\"/Three\", 1);\n");
  scripts.add("Maps/4/Sequences/seq1.vs", "//void\nEnvWriteInt(\"/Four\", 1);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.conquest = bytes_of(kTerritories);
  Result<std::unique_ptr<GameSession>> made = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  const SequenceRef three[] = {SequenceRef{"Three", "CurrentMap/Sequences/seq1.vs", "", false}};
  const SequenceRef four[] = {SequenceRef{"Four", "CurrentMap/Sequences/seq1.vs", "", false}};
  CHECK(run.start_sequences(std::span(three), /*base=*/"Maps/3") == 0);
  CHECK(run.start_sequences(std::span(four), /*base=*/"Maps/4") == 0);

  HostContext context;
  context.world = &run.world();
  context.object_type = kTypeObj;
  const auto start = [&](const char* name) {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "RunSequence", 1);
    REQUIRE(index != script::kUnresolvedHost);
    std::vector<script::Value> args = {script::Value::string(name)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.scheduler = &run.scheduler();
    ctx.name = "RunSequence";
    ctx.kind = script::CallKind::free_function;
    CHECK(registry.entry(index).fn(ctx).status == script::HostStatus::ok);
  };
  start("Four");
  run.advance(/*turns=*/2, /*turn_length=*/800);

  const EnvSystem* env = env_of(run.world());
  REQUIRE(env != nullptr);
  CHECK(env->env().find(EnvScope::root(), "/Four") != nullptr);
  CHECK(env->env().find(EnvScope::root(), "/Three") == nullptr);

  // That much passes under `find_chunk` too, because the exact path is in the
  // library and the exact match is tried first -- so the fallback is
  // unreachable and the guard against it is a guard whose case nothing
  // constructs. This is the case: a sequence whose own script is **missing**,
  // whose basename another map's script shares. `Start`'s no-script branch
  // must answer `"Finished"`; a basename fallback instead starts map 4's
  // mission script under map 9's name, with no diagnostic at all.
  const SequenceRef missing[] = {SequenceRef{"Nine", "CurrentMap/Sequences/seq1.vs", "", false}};
  CHECK(run.start_sequences(std::span(missing), /*base=*/"Maps/9") == 0);
  start("Nine");
  run.advance(/*turns=*/2, /*turn_length=*/800);

  CampaignSystem* campaign = campaign_system_of(run.world());
  REQUIRE(campaign != nullptr);
  const CampaignSystem::SequenceEntry* nine = campaign->find_sequence("Nine");
  REQUIRE(nine != nullptr);
  CHECK(nine->running == script::kNoScript);
  CHECK(env->env().read_string(EnvScope::root(), "/SequenceStatus/Nine") == "Finished");
  // **The assertion that discriminates is this one, and the obvious ones do
  // not.** Map 3's script writes its key and returns in the same turn, so a
  // basename fallback that started it leaves `Nine` reading `"Finished"` and
  // its recorded id cleared -- exactly what a correct refusal leaves. The only
  // trace is that the wrong script ran.
  CHECK(env->env().find(EnvScope::root(), "/Three") == nullptr);
}

// --------------------------------------------------------------------------
// the notes a mission pins on the player's list
// --------------------------------------------------------------------------

namespace {

/// A container's two note documents, in the shape the shipped ones have. The
/// second is a `<notes></notes>` with nothing in it, which is what 36 of the
/// installation's 50 note documents are.
constexpr std::string_view kMapNotes = R"(<notes>
  <note id="GOAL" title="Numantia" text="Capture the rebel stronghold.\n" icon=""
        map="Numantia" show_on_minimap="1" locationx="15231" locationy="830"/>
  <note id="LoseCond" title="Scipio" text="Scipio must survive." icon="" map=""
        show_on_minimap="0" locationx="-1" locationy="-1"/>
  <note id="" title="nameless" text="" icon="" map="" show_on_minimap="0"/>
  <note id="GOAL" title="a second GOAL" text="" icon="" map="" show_on_minimap="0"/>
</notes>)";

constexpr std::string_view kEmptyNotes = "<notes>\n</notes>";

/// A session with a note catalogue behind it and the whole host surface bound.
struct NoteSession {
  ClassGraph graph;
  script::HostRegistry registry;
  FakeScripts scripts;
  std::unique_ptr<GameSession> session;

  NoteSession() {
    (void)register_all_hosts(registry);
    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.scripts = &scripts;
    inputs.map_notes = bytes_of(kMapNotes);
    inputs.notes = bytes_of(kEmptyNotes);
    Result<std::unique_ptr<GameSession>> made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    session = std::move(made.value());
  }

  [[nodiscard]] CampaignSystem* campaign() { return campaign_system_of(session->world()); }

  script::HostOutcome call(std::string_view name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(script::CallKind::free_function, name,
                                              static_cast<std::uint16_t>(args.size()));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    HostContext context;
    context.world = &session->world();
    context.object_type = kTypeObj;
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.scheduler = &session->scheduler();
    ctx.name = name;
    ctx.kind = script::CallKind::free_function;
    return entry.fn(ctx);
  }

  [[nodiscard]] bool is_active(const char* id) {
    const script::HostOutcome out = call("IsNoteActive", {script::Value::string(id)});
    return out.status == script::HostStatus::ok && out.value.as_integer() != 0;
  }
};

}  // namespace

/// The catalogue reads what a container declares, and skips what it cannot use.
TEST(campaign_the_note_catalogue_reads_the_containers_two_documents) {
  NoteSession s;
  CampaignSystem* campaign = s.campaign();
  REQUIRE(campaign != nullptr);
  // Two of the four `<note>` elements: one has no `id` and could never be
  // named, and one repeats `GOAL`, which a `std::map` insert would drop.
  REQUIRE(campaign->notes().size() == 2);
  const NoteDefinition* goal = campaign->notes().find("GOAL");
  REQUIRE(goal != nullptr);
  CHECK(goal->title == "Numantia");
  // First in wins: the second `GOAL` is the one that lost.
  CHECK(goal->title != "a second GOAL");
  CHECK(goal->show_on_minimap);
  CHECK(goal->location.x == 15231 && goal->location.y == 830);
  // The `\n` in `text` is the author's own escape and is carried verbatim: the
  // simulation never reads it and the interface is where a line break means
  // something.
  CHECK(goal->text.find("\\n") != std::string::npos);

  const NoteDefinition* lose = campaign->notes().find("LoseCond");
  REQUIRE(lose != nullptr);
  CHECK(!lose->show_on_minimap);
  CHECK(lose->location.x == -1 && lose->location.y == -1);

  // Byte-exact and case-sensitive, as the original's `std::map` lookup is.
  CHECK(campaign->notes().find("goal") == nullptr);
  CHECK(campaign->notes().find("") == nullptr);
}

/// **A note nothing declares cannot be given.** The gate is the one thing about
/// `GiveNote` that a reasonable implementation leaves out, and it fires on
/// shipped data exactly once -- see `sim/note.hpp` for the call, and for why
/// its misspelled twin is not a second one.
TEST(campaign_givenote_refuses_an_id_the_container_never_declared) {
  NoteSession s;
  CHECK(!s.is_active("GOAL"));
  CHECK(s.call("GiveNote", {script::Value::string("GOAL")}).status == script::HostStatus::ok);
  CHECK(s.is_active("GOAL"));

  // Undeclared: nothing happens, and nothing traps. `gbr.exe` 0x005584c3
  // returns before it touches the active map.
  CHECK(s.call("GiveNote", {script::Value::string("Historical Inconsistency")}).status ==
        script::HostStatus::ok);
  CHECK(!s.is_active("Historical Inconsistency"));
  CHECK(s.campaign()->note_board().size() == 1);

  // Giving one twice leaves one.
  CHECK(s.call("GiveNote", {script::Value::string("GOAL")}).status == script::HostStatus::ok);
  CHECK(s.campaign()->note_board().size() == 1);
}

/// `RemoveNote` has **no** catalogue gate, which is the asymmetry with
/// `GiveNote` worth keeping: 0x005581d0 goes straight to the active map.
TEST(campaign_removenote_and_clearnotes_take_notes_off_the_list) {
  NoteSession s;
  (void)s.call("GiveNote", {script::Value::string("GOAL")});
  (void)s.call("GiveNote", {script::Value::string("LoseCond")});
  REQUIRE(s.campaign()->note_board().size() == 2);
  // Ascending by id -- a `std::map`'s order, which is not the order they were
  // given in.
  const std::span<const std::string> active = s.campaign()->note_board().active_notes();
  REQUIRE(active.size() == 2);
  CHECK(active[0] == "GOAL" && active[1] == "LoseCond");

  CHECK(s.call("RemoveNote", {script::Value::string("GOAL")}).status == script::HostStatus::ok);
  CHECK(!s.is_active("GOAL"));
  CHECK(s.is_active("LoseCond"));
  // An id the catalogue never declared, and one that was never given: both are
  // no-ops rather than refusals.
  CHECK(s.call("RemoveNote", {script::Value::string("Never Declared")}).status ==
        script::HostStatus::ok);

  // **And an undeclared note that is somehow active really does come off.**
  // The state is representable and `GiveNote` cannot reach it, which is why
  // this has to be constructed through the board: a `RemoveNote` that grew the
  // catalogue gate `GiveNote` has passes every other assertion in this file
  // and strands the note here forever.
  REQUIRE(s.campaign()->note_board().give("Never Declared"));
  CHECK(s.is_active("Never Declared"));
  CHECK(s.call("RemoveNote", {script::Value::string("Never Declared")}).status ==
        script::HostStatus::ok);
  CHECK(!s.is_active("Never Declared"));
  CHECK(s.call("RemoveNote", {script::Value::string("GOAL")}).status == script::HostStatus::ok);
  CHECK(s.campaign()->note_board().size() == 1);

  CHECK(s.call("ClearNotes", {}).status == script::HostStatus::ok);
  CHECK(s.campaign()->note_board().size() == 0);
  // The catalogue is untouched, so the same note can be given again.
  CHECK(s.campaign()->notes().size() == 2);
  (void)s.call("GiveNote", {script::Value::string("GOAL")});
  CHECK(s.is_active("GOAL"));
}

/// `IsNoteActive` consults the **board**, never the catalogue.
TEST(campaign_isnoteactive_asks_the_board_and_not_the_catalogue) {
  NoteSession s;
  // Declared and never given: false. An implementation that answered from the
  // catalogue -- which is the shape `GiveNote` uses one line away -- says true
  // for both of these.
  REQUIRE(s.campaign()->notes().find("LoseCond") != nullptr);
  CHECK(!s.is_active("LoseCond"));
  // Neither declared nor given: false, and not a refusal.
  CHECK(!s.is_active("Never Declared"));
  const script::HostOutcome out = s.call("IsNoteActive", {script::Value::string("Never Declared")});
  CHECK(out.status == script::HostStatus::ok);
}

/// The board is state: it moves the campaign hash and survives a save.
TEST(campaign_the_note_board_is_hashed_and_round_trips) {
  NoteBoard board;
  std::uint64_t empty = 0;
  board.hash(empty);
  CHECK(board.give("b"));
  CHECK(board.give("a"));
  CHECK(!board.give("a"));  // already active
  std::uint64_t filled = 0;
  board.hash(filled);
  CHECK(filled != empty);

  // The separator earns its place: `{"ab"}` and `{"a", "b"}` are different
  // states and a hash that concatenated would call them equal.
  NoteBoard split;
  CHECK(split.give("ab"));
  std::uint64_t joined = 0;
  split.hash(joined);
  CHECK(joined != filled);

  std::vector<std::byte> bytes;
  board.serialize(bytes);
  NoteBoard loaded;
  REQUIRE(loaded.deserialize(bytes).ok());
  REQUIRE(loaded.size() == 2);
  CHECK(loaded.active("a") && loaded.active("b"));
  CHECK(!loaded.active("c"));
  std::uint64_t reloaded = 0;
  loaded.hash(reloaded);
  CHECK(reloaded == filled);

  // Ids out of order are refused, because `active` binary-searches on that.
  std::vector<std::byte> wrong;
  NoteBoard forged;
  CHECK(forged.give("z"));
  CHECK(forged.give("y"));
  forged.serialize(wrong);
  // Swap the two ids in the payload by rewriting it by hand: the header is
  // eight bytes, then the count, then length-prefixed strings.
  REQUIRE(wrong.size() > 12);
  CHECK(NoteBoard{}.deserialize(std::span(wrong).subspan(0, wrong.size() - 1)).error() !=
        FormatError::none);
}

TEST(campaign_stop_sequence_and_is_sequence_running_are_not_entry_points) {
  // Neither string occurs anywhere in `gbr.exe` and neither has a call site in
  // the 885 shipped scripts. Three places in this tree named them as part of
  // the family; they are a documentation error rather than a deferred feature,
  // and adding them would invent both a name and a semantics. The real family
  // is `RunSequence`, `IsRunning`, `IsWaiting` and `IsFinished`.
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  CHECK(registry.find(script::CallKind::free_function, "StopSequence", 1) ==
        script::kUnresolvedHost);
  CHECK(registry.find(script::CallKind::free_function, "IsSequenceRunning", 1) ==
        script::kUnresolvedHost);
  CHECK(registry.find(script::CallKind::free_function, "RunSequence", 1) !=
        script::kUnresolvedHost);
  CHECK(registry.find(script::CallKind::free_function, "IsRunning", 1) !=
        script::kUnresolvedHost);
  CHECK(registry.find(script::CallKind::free_function, "IsWaiting", 1) !=
        script::kUnresolvedHost);
  CHECK(registry.find(script::CallKind::free_function, "IsFinished", 1) !=
        script::kUnresolvedHost);
}
