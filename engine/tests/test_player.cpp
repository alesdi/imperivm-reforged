// Players, diplomacy, and the host entry points that read them.
//
// The `playerdata` fixtures below are written from `docs/formats/map.md`, in
// the exact byte shape the containers use. They were copies of two retail
// documents until `tools/check_fixtures.py` found them; see the note above the
// fixtures for what the copies were for and where each of those claims went.
//
// The literals are machine-generated rather than retyped, which is a lesson
// this file already paid for: a hand-split 128-digit `relations` attribute in
// its first draft lost two digits and failed four correct tests.

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/world/map.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// -- fixtures ---------------------------------------------------------------
//
// Four `<playerdata>` rows written for this file, in the byte-for-byte shape
// the containers use: CRLF, a tab before the element and two before every
// attribute, the attributes in the order the editor writes them, and a
// `relations` attribute of exactly sixteen eight-digit words whether or not the
// map has sixteen players.
//
// **They were copies of `3_Great_Losses_Egypt.bfhp` and `randommap.BFHP`.**
// `docs/legal.md` rule 1 forbids that -- no game assets in the repository,
// "not as test fixtures" -- and `tools/check_fixtures.py` refuses it now.
//
// The three properties the copies were chosen for are kept, because each is a
// property of the *format* rather than of those two files:
//
//   * an **asymmetric pair**: player 2 records `0x01` for player 3 while player
//     3 records `0x11` for player 2. A reader that mirrored the matrix -- and
//     the obvious reader does -- would pass every symmetric fixture ever
//     written and be wrong here;
//   * a row whose `relations` runs past the players that exist, so that the
//     tail must be read and not assumed zero; and
//   * a row with **no** `AllowedRaces`, `difficulty` or `bonus` at all, which is
//     what a random map writes and what the defaults have to hold for.
//
// The census the relation tests used to quote -- 304 `player<i>.xml` documents
// in 19 containers, 4,864 words, 4,544 of them zero, 304 `0x35` every one on a
// diagonal, 9 `0x11`, 6 `0x15`, exactly 1 `0x01`, 16 off-diagonal entries in 3
// maps and 3 pairs disagreeing with their transpose -- is a claim about the
// installation and is asserted in `tests/test_corpus_players.py`, which counts
// it rather than remembering it. The nineteenth container is the one a naive
// scan misses: `Packs/RandomMapSettlements.bfhp` is an LZIS stream wrapping the
// `HPFS` container, so sniffing the magic without decompressing first finds
// eighteen and undercounts the diagonal by sixteen.

// legal-ok: the runs these four literals share with the installation are the
// format's own scaffolding -- `AllowedRaces="All"`, `control="Computer"`, the
// tab-and-CRLF between attributes, and stretches of the zero word inside
// `relations`. A `<playerdata>` written from the specification has to look
// exactly like that, and deforming it to avoid the match would be deforming
// the fixture to fool the check.
constexpr std::string_view kEgyptPlayer0 =
    "\t<playerdata\r\n\t\tid=\"0\"\r\n\t\tname=\"Alpha\"\r\n\t\trace=\"Carthage\"\r\n\t\tAllowedR"
    "aces=\"All\"\r\n\t\tcontrol=\"Computer\"\r\n\t\tAI=\"\"\r\n\t\tdifficulty=\"3\"\r\n\t\tstart"
    "x=\"0\"\r\n\t\tstarty=\"0\"\r\n\t\tcolor=\"31810\"\r\n\t\tallied=\"0\"\r\n\t\tbonus=\"-1\"\r"
    "\n\t\trelations=\"00000035000000000000001100000000000000150000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000\"/>\r\n";

// legal-ok: format scaffolding only; see the note above the fixtures.
constexpr std::string_view kEgyptPlayer2 =
    "\t<playerdata\r\n\t\tid=\"2\"\r\n\t\tname=\"Gamma\"\r\n\t\trace=\"Carthage\"\r\n\t\tAllowedR"
    "aces=\"All\"\r\n\t\tcontrol=\"Computer\"\r\n\t\tAI=\"\"\r\n\t\tdifficulty=\"3\"\r\n\t\tstart"
    "x=\"0\"\r\n\t\tstarty=\"0\"\r\n\t\tcolor=\"992\"\r\n\t\tallied=\"0\"\r\n\t\tbonus=\"-1\"\r\n"
    "\t\trelations=\"0000001100000000000000350000000100000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000\"/>\r\n";

// legal-ok: format scaffolding only; see the note above the fixtures.
constexpr std::string_view kEgyptPlayer3 =
    "\t<playerdata\r\n\t\tid=\"3\"\r\n\t\tname=\"Delta\"\r\n\t\trace=\"Germany\"\r\n\t\tAllowedRa"
    "ces=\"All\"\r\n\t\tcontrol=\"Computer\"\r\n\t\tAI=\"CHAOTIC\"\r\n\t\tdifficulty=\"2\"\r\n\t"
    "\tstartx=\"0\"\r\n\t\tstarty=\"0\"\r\n\t\tcolor=\"957\"\r\n\t\tallied=\"0\"\r\n\t\tbonus=\"-"
    "1\"\r\n\t\trelations=\"000000000000000000000011000000350000000000000000000000000000001500000"
    "00000000000000000000000000000000000000000000000000000000000\"/>\r\n";

// legal-ok: format scaffolding only; see the note above the fixtures.
constexpr std::string_view kRandomMapPlayer0 =
    "\t<playerdata\r\n\t\tid=\"0\"\r\n\t\tname=\"*Slot 1\"\r\n\t\trace=\"Select\"\r\n\t\tcontrol="
    "\"Both\"\r\n\t\tAI=\"\"\r\n\t\tstartx=\"0\"\r\n\t\tstarty=\"0\"\r\n\t\tcolor=\"18470\"\r\n\t"
    "\tallied=\"0\"\r\n\t\trelations=\"0000003500000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000\"/>\r\n";

/// A `<playerdata>` with any one attribute replaced, for the rejection tests.
std::string with_relations(std::string_view relations) {
  std::string out = "<playerdata id=\"4\" name=\"x\" race=\"Gaul\" control=\"Computer\"";
  out += " relations=\"";
  out += relations;
  out += "\"/>";
  return out;
}

std::string with_id(std::string_view id) {
  std::string out = "<playerdata id=\"";
  out += id;
  out += "\" name=\"x\" race=\"Gaul\" control=\"Computer\" relations=\"";
  for (int i = 0; i < 16; ++i) out += "00000035";
  out += "\"/>";
  return out;
}

}  // namespace

// ==========================================================================
// the relation bits
// ==========================================================================

TEST(relation_words_decompose_under_the_proven_bits) {
  PlayerTable table;
  table.set_relation_word(0, 1, kRelationSelf);
  table.set_relation_word(0, 2, kRelationAllied);
  table.set_relation_word(0, 3, kRelationFriendly);
  table.set_relation_word(0, 4, kRelationCeasefireOnly);
  table.set_relation_word(0, 5, 0);

  // `0x35` -- what every player holds for itself. Everything but nothing.
  CHECK(table.has(0, 1, Relation::ceasefire));
  CHECK(table.has(0, 1, Relation::allied));
  CHECK(table.has(0, 1, Relation::share_view));
  CHECK(table.has(0, 1, Relation::share_control));
  CHECK(table.has(0, 1, Relation::share_support));

  // `0x15` -- the word the `allied="1"` pairs hold. Control is the one thing
  // it does not grant.
  CHECK(table.has(0, 2, Relation::ceasefire));
  CHECK(table.has(0, 2, Relation::allied));
  CHECK(table.has(0, 2, Relation::share_view));
  CHECK(!table.has(0, 2, Relation::share_control));
  CHECK(table.has(0, 2, Relation::share_support));

  // `0x11` -- ceasefire and shared vision, nothing else.
  CHECK(table.has(0, 3, Relation::ceasefire));
  CHECK(table.has(0, 3, Relation::share_view));
  CHECK(!table.has(0, 3, Relation::share_control));
  CHECK(!table.has(0, 3, Relation::share_support));

  // `0x01` -- the bare ceasefire, which occurs exactly once in the install.
  CHECK(table.has(0, 4, Relation::ceasefire));
  CHECK(!table.has(0, 4, Relation::share_view));
  CHECK(!table.has(0, 4, Relation::share_control));
  CHECK(!table.has(0, 4, Relation::share_support));

  // A zero word grants nothing, which is why hostility is the default.
  CHECK(!table.has(0, 5, Relation::ceasefire));
  CHECK(!table.has(0, 5, Relation::allied));
}

TEST(allied_and_ceasefire_are_the_same_bit) {
  // Not a coincidence to be tidied away: `_PlayersAlly` sets bit 0 and
  // `_PlayersMakeEnemies` clears it, and `DiplAreAllied` is the mutual reading
  // of what `DiplGetCeaseFire` reads one way round. If these two ever disagree,
  // the bit table has been "corrected" back to a guess.
  PlayerTable table;
  for (std::uint32_t word = 0; word < 64; ++word) {
    table.set_relation_word(0, 1, word);
    CHECK(table.has(0, 1, Relation::ceasefire) == table.has(0, 1, Relation::allied));
  }
}

TEST(setting_a_relation_touches_only_its_own_bit) {
  PlayerTable table;
  table.set_relation_word(1, 2, 0);
  table.set(1, 2, Relation::share_view, true);
  CHECK(table.relation_word(1, 2) == 0x10);
  table.set(1, 2, Relation::share_control, true);
  CHECK(table.relation_word(1, 2) == 0x30);
  table.set(1, 2, Relation::share_support, true);
  CHECK(table.relation_word(1, 2) == 0x34);
  table.set(1, 2, Relation::ceasefire, true);
  CHECK(table.relation_word(1, 2) == kRelationSelf);
  // Ceasefire off is what `_PlayersMakeEnemies` does, and it leaves the three
  // sharing bits alone.
  table.set(1, 2, Relation::ceasefire, false);
  CHECK(table.relation_word(1, 2) == 0x34);
  CHECK(!table.has(1, 2, Relation::allied));
}

TEST(bits_one_and_three_are_never_written) {
  // The competing reading of the four observed words was three two-bit fields,
  // which needs bits 1 and 3. Nothing in the executable reads or writes them,
  // so nothing here may either.
  PlayerTable table;
  table.set_relation_word(2, 3, 0);
  for (int r = 0; r < static_cast<int>(Relation::count); ++r) {
    table.set(2, 3, static_cast<Relation>(r), true);
  }
  CHECK((table.relation_word(2, 3) & 0x0Au) == 0);
  CHECK(table.relation_word(2, 3) == kRelationSelf);
}

// ==========================================================================
// is_enemy / are_allied
// ==========================================================================

TEST(a_fresh_table_is_a_free_for_all_with_a_seeded_diagonal) {
  PlayerTable table;
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    CHECK(table.relation_word(static_cast<PlayerId>(i), static_cast<PlayerId>(i)) ==
          kRelationSelf);
    CHECK(!table.is_enemy(static_cast<PlayerId>(i), static_cast<PlayerId>(i)));
    CHECK(table.are_allied(static_cast<PlayerId>(i), static_cast<PlayerId>(i)));
  }
  CHECK(table.is_enemy(0, 1));
  CHECK(table.is_enemy(1, 0));
  CHECK(!table.are_allied(0, 1));
}

TEST(hostility_is_one_directional) {
  // Egypt's second asymmetry: player 3 grants player 5 a ceasefire, player 5
  // grants nothing back. A mutual rule would call neither of them an enemy and
  // lose the one-sided truce the map author wrote.
  PlayerTable table;
  table.set_relation_word(3, 5, kRelationFriendly);
  table.set_relation_word(5, 3, 0);
  CHECK(!table.is_enemy(3, 5));
  CHECK(table.is_enemy(5, 3));
  CHECK(!table.are_allied(3, 5));
  CHECK(!table.are_allied(5, 3));
}

TEST(alliance_needs_both_directions) {
  // Egypt's first asymmetry: 2 -> 3 is `0x01` and 3 -> 2 is `0x11`. Both set
  // bit 0, so this pair *is* allied, asymmetric though it is -- the asymmetry
  // is in the sharing bits, not in the ceasefire.
  PlayerTable table;
  table.set_relation_word(2, 3, kRelationCeasefireOnly);
  table.set_relation_word(3, 2, kRelationFriendly);
  CHECK(table.are_allied(2, 3));
  CHECK(table.are_allied(3, 2));
  CHECK(!table.is_enemy(2, 3));
  CHECK(!table.is_enemy(3, 2));
  // ...and the sharing is one-directional even so.
  CHECK(!table.has(2, 3, Relation::share_view));
  CHECK(table.has(3, 2, Relation::share_view));
}

TEST(out_of_range_players_are_neither_enemies_nor_allies) {
  PlayerTable table;
  CHECK(!table.is_enemy(0, sim::kNoPlayer));
  CHECK(!table.is_enemy(sim::kNoPlayer, 0));
  CHECK(!table.are_allied(0, sim::kNoPlayer));
  CHECK(table.relation_word(sim::kNoPlayer, 0) == 0);
}

// ==========================================================================
// the relations attribute
// ==========================================================================

TEST(a_relations_row_is_sixteen_big_endian_words) {
  PlayerTable table;
  std::string hex;
  for (int i = 0; i < 16; ++i) hex += "0000003" + std::string(1, static_cast<char>('0' + i % 10));
  CHECK(table.set_row_from_hex(0, hex));
  CHECK(table.relation_word(0, 0) == 0x30);
  CHECK(table.relation_word(0, 5) == 0x35);
  CHECK(table.relation_word(0, 15) == 0x35);
}

TEST(a_malformed_relations_row_is_refused_and_changes_nothing) {
  PlayerTable table;
  table.set_relation_word(0, 7, kRelationAllied);

  std::string good;
  for (int i = 0; i < 16; ++i) good += "00000035";

  // One digit short.
  CHECK(!table.set_row_from_hex(0, good.substr(0, good.size() - 1)));
  // One digit long.
  CHECK(!table.set_row_from_hex(0, good + "0"));
  // Empty.
  CHECK(!table.set_row_from_hex(0, ""));
  // Right length, wrong alphabet -- and the bad digit is in the *last* word, so
  // a reader that wrote as it went would already have clobbered fifteen.
  std::string bad = good;
  bad[bad.size() - 1] = 'g';
  CHECK(!table.set_row_from_hex(0, bad));
  // Out-of-range row.
  CHECK(!table.set_row_from_hex(sim::kNoPlayer, good));

  // Nothing above touched the table.
  CHECK(table.relation_word(0, 7) == kRelationAllied);
  CHECK(table.relation_word(0, 0) == kRelationSelf);
}

// ==========================================================================
// player<i>.xml
// ==========================================================================

TEST(playerdata_parses_every_attribute) {
  PlayerTable table;
  CHECK(parse_player_setup(bytes_of(kEgyptPlayer3), table).ok());

  const PlayerSetup& p3 = table.setup(3);
  CHECK(p3.name == "Delta");
  CHECK(p3.race == "Germany");
  CHECK(p3.allowed_races == "All");
  CHECK(p3.control == PlayerControl::computer);
  CHECK(p3.difficulty == 2);
  CHECK(p3.colour == 957);
  CHECK(p3.start.x == 0);
  CHECK(p3.start.y == 0);
  CHECK(p3.bonus == -1);
  CHECK(!p3.allied_flag);
  CHECK(p3.ai_script == "CHAOTIC");

  // The row, straight out of the same attribute.
  CHECK(table.relation_word(3, 3) == kRelationSelf);
  CHECK(table.relation_word(3, 2) == kRelationFriendly);
  CHECK(table.relation_word(3, 7) == kRelationAllied);
  CHECK(table.relation_word(3, 0) == 0);
  CHECK(table.relation_word(3, 15) == 0);
}

TEST(playerdata_defaults_hold_for_randommap) {
  // A random map omits `AllowedRaces`, `difficulty` and `bonus` entirely.
  PlayerTable table;
  CHECK(parse_player_setup(bytes_of(kRandomMapPlayer0), table).ok());
  const PlayerSetup& p0 = table.setup(0);
  CHECK(p0.name == "*Slot 1");
  CHECK(p0.race == "Select");
  CHECK(p0.allowed_races == "All");
  CHECK(p0.control == PlayerControl::both);
  CHECK(p0.difficulty == 0);
  CHECK(p0.bonus == -1);
  CHECK(p0.colour == 18470);
  CHECK(table.relation_word(0, 0) == kRelationSelf);
  CHECK(table.relation_word(0, 1) == 0);
}

TEST(egypts_asymmetry_survives_the_round_trip) {
  PlayerTable table;
  CHECK(parse_player_setup(bytes_of(kEgyptPlayer0), table).ok());
  CHECK(parse_player_setup(bytes_of(kEgyptPlayer2), table).ok());
  CHECK(parse_player_setup(bytes_of(kEgyptPlayer3), table).ok());

  // The asymmetric pair, against its transpose. The shipped install has three
  // of these and exactly one `0x01`; `tests/test_corpus_players.py` counts them.
  CHECK(table.relation_word(2, 3) == kRelationCeasefireOnly);
  CHECK(table.relation_word(3, 2) == kRelationFriendly);
  CHECK(table.relation_word(2, 3) != table.relation_word(3, 2));

  // Player 0's row: `0x11` for player 2, `0x15` for player 4, nothing for 1.
  CHECK(table.relation_word(0, 2) == kRelationFriendly);
  CHECK(table.relation_word(0, 4) == kRelationAllied);
  CHECK(table.relation_word(0, 1) == 0);

  CHECK(table.are_allied(0, 2));
  CHECK(!table.is_enemy(0, 2));
  CHECK(table.is_enemy(0, 1));
  CHECK(table.is_enemy(1, 0));
}

TEST(playerdata_refuses_rather_than_zeroing_a_row) {
  PlayerTable table;
  table.set_relation_word(4, 9, kRelationAllied);

  const std::string short_row = with_relations(std::string(120, '0'));
  CHECK(!parse_player_setup(bytes_of(short_row), table).ok());

  std::string not_hex;
  for (int i = 0; i < 16; ++i) not_hex += "00000035";
  not_hex[3] = 'z';
  const std::string bad_row = with_relations(not_hex);
  CHECK(!parse_player_setup(bytes_of(bad_row), table).ok());

  const std::string no_row = "<playerdata id=\"4\" name=\"x\" control=\"Computer\"/>";
  CHECK(!parse_player_setup(bytes_of(no_row), table).ok());

  const std::string wrong_root = "<player id=\"4\" relations=\"\"/>";
  CHECK(!parse_player_setup(bytes_of(wrong_root), table).ok());

  // Every one of those left the table exactly as it was.
  CHECK(table.relation_word(4, 9) == kRelationAllied);
  CHECK(table.setup(4).name.empty());
}

TEST(playerdata_refuses_an_id_it_cannot_place) {
  PlayerTable table;
  CHECK(!parse_player_setup(bytes_of(with_id("16")), table).ok());
  CHECK(!parse_player_setup(bytes_of(with_id("-1")), table).ok());
  CHECK(parse_player_setup(bytes_of(with_id("15")), table).ok());
  CHECK(parse_player_setup(bytes_of(with_id("0")), table).ok());
  // No `id` at all: `attribute_int`'s fallback must not become player 0.
  const std::string anonymous =
      "<playerdata name=\"x\" control=\"Computer\" relations=\"" + std::string(128, '0') + "\"/>";
  CHECK(!parse_player_setup(bytes_of(anonymous), table).ok());
}

TEST(a_container_supplies_sixteen_players_or_none) {
  // Sixteen synthetic documents with ids 0..15, which is what every shipped map
  // directory holds.
  std::vector<std::string> docs;
  for (int i = 0; i < 16; ++i) {
    docs.push_back(with_id(std::to_string(i)));
  }
  std::vector<std::span<const std::byte>> spans;
  for (const std::string& d : docs) spans.push_back(bytes_of(d));

  PlayerTable table;
  CHECK(load_player_table(spans, table).ok());
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    CHECK(table.relation_word(static_cast<PlayerId>(i), 0) == kRelationSelf);
  }

  // Fifteen is not a map.
  PlayerTable fifteen;
  CHECK(!load_player_table(std::span(spans).first(15), fifteen).ok());

  // A duplicated id is a container this reader does not understand, not a
  // fifteen-player table.
  std::vector<std::string> dup = docs;
  dup[15] = with_id("14");
  std::vector<std::span<const std::byte>> dup_spans;
  for (const std::string& d : dup) dup_spans.push_back(bytes_of(d));
  PlayerTable duplicated;
  duplicated.set_relation_word(0, 3, kRelationAllied);
  CHECK(!load_player_table(dup_spans, duplicated).ok());
  // ...and it left the caller's table alone.
  CHECK(duplicated.relation_word(0, 3) == kRelationAllied);
}

// ==========================================================================
// script player numbers
// ==========================================================================

TEST(script_player_numbers_are_one_based) {
  CHECK(player_to_script(0) == 1);
  CHECK(player_to_script(15) == 16);
  CHECK(player_to_script(sim::kNeutralWildlife) == 15);
  CHECK(player_to_script(sim::kNeutralPassive) == 16);
  CHECK(player_to_script(sim::kNoPlayer) == -1);

  CHECK(player_from_script(1) == 0);
  CHECK(player_from_script(16) == 15);
  CHECK(player_from_script(0) == sim::kNoPlayer);
  CHECK(player_from_script(17) == sim::kNoPlayer);
  CHECK(player_from_script(-1) == sim::kNoPlayer);

  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    const PlayerId id = static_cast<PlayerId>(i);
    CHECK(player_from_script(player_to_script(id)) == id);
  }
}

TEST(race_constants_match_the_engines) {
  CHECK(race_from_name("Gaul") == 0);
  CHECK(race_from_name("Rome") == 1);
  CHECK(race_from_name("RepublicanRome") == 1);
  CHECK(race_from_name("Carthage") == 2);
  CHECK(race_from_name("Iberia") == 3);
  CHECK(race_from_name("ImperialRome") == 4);
  CHECK(race_from_name("Britain") == 5);
  CHECK(race_from_name("Egypt") == 6);
  CHECK(race_from_name("Germany") == 7);

  // The three match-setup markers and the raceless class are not races.
  CHECK(race_from_name("Random") == kNoRace);
  CHECK(race_from_name("Mutable") == kNoRace);
  CHECK(race_from_name("Select") == kNoRace);
  CHECK(race_from_name("None") == kNoRace);
  CHECK(race_from_name("") == kNoRace);

  CHECK(race_to_name(1) == "RepublicanRome");
  CHECK(race_to_name(4) == "ImperialRome");
  CHECK(race_to_name(kNoRace).empty());

  // **The demonyms, and the one shipped class that needed them.** The class
  // loader accepts a place name and a demonym for each race, each in a
  // capitalised and a lowercase spelling; this table used to carry only the
  // eight place names, and `TOutpost` -- the single class in the retail
  // install that writes `race="German"` -- read as raceless because of it.
  CHECK(race_from_name("German") == 7);
  CHECK(race_from_name("Roman") == 1);
  CHECK(race_from_name("Carthaginian") == 2);
  CHECK(race_from_name("Iberian") == 3);
  CHECK(race_from_name("British") == 5);
  CHECK(race_from_name("Briton") == 5);
  CHECK(race_from_name("Egyptian") == 6);
  CHECK(race_from_name("gaul") == 0);
  CHECK(race_from_name("german") == 7);
  CHECK(race_from_name("imperialrome") == 4);

  // And the round trip still hands back the spelling the shipped maps use,
  // which is what the table's order is for.
  for (std::int32_t race = 0; race < 8; ++race) {
    CHECK(race_from_name(race_to_name(race)) == race);
  }
  CHECK(race_to_name(7) == "Germany");
  CHECK(race_to_name(5) == "Britain");

  // The prefix letters, which are a class-name alphabet: every one of them
  // names a real sentry class in the retail install (`GSentry` .. `TSentry`).
  CHECK(race_prefix(0, false) == "G");
  CHECK(race_prefix(7, false) == "T");
  CHECK(race_prefix(4, false) == "M");
  CHECK(race_prefix(7, true) == "t");
}

// ==========================================================================
// the host entry points
// ==========================================================================

namespace {

struct Bench {
  World world;
  HostContext context;
  script::HostRegistry registry;

  Bench() {
    context.world = &world;
    context.object_type = kTypeObj;
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_player_host(registry);
  }

  /// Call a host entry point by name, with a world behind it.
  script::HostOutcome call(script::CallKind kind, std::string_view name,
                           std::vector<script::Value> args) {
    const std::uint32_t index =
        registry.find(kind, name, static_cast<std::uint16_t>(args.size() - (kind ==
            script::CallKind::member ? 1 : 0)));
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return entry.fn(ctx);
  }

  [[nodiscard]] bool truthy(script::CallKind kind, std::string_view name,
                            std::vector<script::Value> args) {
    const script::HostOutcome out = call(kind, name, std::move(args));
    return out.status == script::HostStatus::ok && out.value.is_integer() &&
           out.value.as_integer() != 0;
  }
};

constexpr script::CallKind kFree = script::CallKind::free_function;
constexpr script::CallKind kMember = script::CallKind::member;

}  // namespace

/// A world with three raced classes, for the six race-string entry points.
///
/// `race` is a *class* property, never the owner's, which is the whole point of
/// the member forms: a captured Egyptian town hall reads `Egypt` whoever holds
/// it.
struct RaceBench {
  ClassGraph graph;
  World world;
  HostContext context;
  script::HostRegistry registry;

  RaceBench() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="ETownhall" parent="Object" cpp_class="CVXDecor"><properties race="Egypt"/></class>)",
        R"(<class id="MVelit" parent="Object" cpp_class="CVXDecor"><properties race="ImperialRome"/></class>)",
        R"(<class id="Rock" parent="Object" cpp_class="CVXDecor"><properties race="None"/></class>)",
        // An `altid`, because a script names a class the way the data does and
        // `ClassGraph::lookup` resolves `id` first and `altid` after.
        R"(<class id="GSwordsman" altid="GaulSword" parent="Object" cpp_class="CVXDecor"><properties race="Gaul"/></class>)",
    };
    const char* names[] = {"o.sc.xml", "e.sc.xml", "m.sc.xml", "r.sc.xml", "g.sc.xml"};
    for (int i = 0; i < 5; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    world.set_class_graph(&graph);
    context.world = &world;
    context.object_type = kTypeObj;
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_player_host(registry);
  }

  ObjectId make(std::string_view id) {
    return world.spawn(imperivm::core::NativeClass::decor, nullptr, graph.lookup(id));
  }

  [[nodiscard]] std::string text(script::CallKind kind, std::string_view name,
                                 std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(
        kind, name,
        static_cast<std::uint16_t>(args.size() -
                                   (kind == script::CallKind::member ? 1 : 0)));
    if (index == script::kUnresolvedHost) return "<undeclared>";
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return "<no body>";
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    const script::HostOutcome out = entry.fn(ctx);
    if (out.status != script::HostStatus::ok || !out.value.is_string()) return "<not a string>";
    return std::string(out.value.as_string());
  }

  [[nodiscard]] script::Value obj(ObjectId id) {
    return script::Value::object(script::ObjectRef{kTypeObj, id});
  }
};

TEST(the_race_strings_are_one_table_read_three_ways) {
  RaceBench b;

  // The identifier, in the executable's own index order. These are the eight
  // tokens `player<i>.xml` authors, not display text.
  CHECK(b.text(kFree, "GetRaceStr", {script::Value::integer(0)}) == "Gaul");
  CHECK(b.text(kFree, "GetRaceStr", {script::Value::integer(1)}) == "RepublicanRome");
  CHECK(b.text(kFree, "GetRaceStr", {script::Value::integer(4)}) == "ImperialRome");
  CHECK(b.text(kFree, "GetRaceStr", {script::Value::integer(7)}) == "Germany");

  // The class-name prefix, same order, including the two that look like
  // mistakes and are not: `M` for Imperial Rome and `T` for Germany.
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(0)}) == "G");
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(1)}) == "R");
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(2)}) == "C");
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(3)}) == "I");
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(4)}) == "M");
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(5)}) == "B");
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(6)}) == "E");
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(7)}) == "T");

  // Outside 0..7 is the **empty string**, not a default race. Both ends, and
  // the sentinel `kNoRace` a class with no race resolves to.
  CHECK(b.text(kFree, "GetRaceStr", {script::Value::integer(8)}).empty());
  CHECK(b.text(kFree, "GetRaceStr", {script::Value::integer(-1)}).empty());
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(8)}).empty());
  CHECK(b.text(kFree, "GetRaceStrPref", {script::Value::integer(-1)}).empty());

  // The members read the *class*, and the lower-case one is a third reading of
  // the same table rather than a table of its own.
  const ObjectId egypt = b.make("ETownhall");
  const ObjectId rome = b.make("MVelit");
  const ObjectId rock = b.make("Rock");
  CHECK(b.text(kMember, "raceStr", {b.obj(egypt)}) == "Egypt");
  CHECK(b.text(kMember, "raceStrPref", {b.obj(egypt)}) == "E");
  CHECK(b.text(kMember, "raceStrPrefLow", {b.obj(egypt)}) == "e");
  CHECK(b.text(kMember, "raceStrPref", {b.obj(rome)}) == "M");
  CHECK(b.text(kMember, "raceStrPrefLow", {b.obj(rome)}) == "m");

  // A class whose `race` is `None`, and an unresolvable receiver, answer the
  // same empty string -- and the second is an ordinary return, not a refusal.
  CHECK(b.text(kMember, "raceStr", {b.obj(rock)}).empty());
  CHECK(b.text(kMember, "raceStrPref", {b.obj(rock)}).empty());
  CHECK(b.text(kMember, "raceStr", {b.obj(kNoObject)}).empty());
  CHECK(b.text(kMember, "raceStrPrefLow", {b.obj(kNoObject)}).empty());

  // `GetRaceStrPrefLow` is registered by `gbr.exe` (0x005a7e00) and is the
  // third arm of this same table, but **no shipped script calls it** -- the only
  // reader of the lower-case form is the member, in `ES_VILLAGE.VS`. So it is
  // not in the declared surface at all, let alone bound. That is the
  // `ShowNotes` rule, and this line is what would notice it being broken.
  CHECK(b.text(kFree, "GetRaceStrPrefLow", {script::Value::integer(0)}) == "<undeclared>");
}

TEST(register_player_host_defines_what_it_says_it_does) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  const std::size_t declared = registry.size();
  const std::size_t before = registry.implemented();
  const std::size_t defined = register_player_host(registry);
  CHECK(defined == player_host_entry_count());
  // Every one of them was already in the declared surface. `define` adds an
  // entry when the (kind, name, arity) is unknown, so an unchanged size is what
  // proves no name or arity was invented here -- the rule the whole
  // declared/defined split exists to enforce.
  CHECK(registry.size() == declared);
  // ...and none of them replaced another of this domain's own entries.
  CHECK(registry.implemented() == before + defined);
}

TEST(every_player_entry_point_refuses_without_a_world) {
  // `CallContext::user` null is a real case: the harness runs every host
  // function this way to prove none of them dereferences it.
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_player_host(registry);

  // The three free race-string forms are the exception, and they are named
  // rather than skipped by a pattern: `GetRaceStr(n)` and its two prefix
  // siblings are an eight-entry table lookup on their own argument. There is no
  // world in them to be missing, and making them refuse would be inventing a
  // failure the original does not have -- `0x005a7ce0` answers the empty string
  // for a race outside 0..7 and nothing else can go wrong. Every *member* form
  // reads the receiver's class and does refuse.
  const auto reads_the_world = [](std::string_view name) {
    return name != "GetRaceStr" && name != "GetRaceStrPref";
  };

  std::size_t checked = 0;
  std::size_t worldless = 0;
  for (const script::HostEntry& entry : registry.entries()) {
    if (entry.fn == nullptr) continue;
    if (entry.kind == kFree && !reads_the_world(entry.name)) {
      ++checked;
      ++worldless;
      continue;
    }
    std::vector<script::Value> args(entry.arity + (entry.kind == kMember ? 1u : 0u),
                                    script::Value::integer(1));
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = nullptr;
    ctx.name = entry.name;
    ctx.kind = entry.kind;
    const script::HostOutcome out = entry.fn(ctx);
    CHECK(out.status == script::HostStatus::error);
    ++checked;
  }
  CHECK(checked == player_host_entry_count());
  CHECK(worldless == 2);
}

TEST(dipl_getters_read_the_matrix_one_way_round) {
  Bench bench;
  PlayerTable& players = bench.world.players();
  // 1-based on the wire: these are players 0 and 1.
  players.set_relation_word(0, 1, kRelationAllied);
  players.set_relation_word(1, 0, kRelationFriendly);

  CHECK(bench.truthy(kFree, "DiplGetCeaseFire",
                     {script::Value::integer(1), script::Value::integer(2)}));
  CHECK(bench.truthy(kFree, "DiplGetShareView",
                     {script::Value::integer(1), script::Value::integer(2)}));
  CHECK(bench.truthy(kFree, "DiplGetShareSupport",
                     {script::Value::integer(1), script::Value::integer(2)}));
  CHECK(!bench.truthy(kFree, "DiplGetShareControl",
                      {script::Value::integer(1), script::Value::integer(2)}));
  // The other direction is `0x11`, which does not share support.
  CHECK(!bench.truthy(kFree, "DiplGetShareSupport",
                      {script::Value::integer(2), script::Value::integer(1)}));
  CHECK(bench.truthy(kFree, "DiplAreAllied",
                     {script::Value::integer(1), script::Value::integer(2)}));

  // 0 and 17 are outside 1..16: false, not a trap, because a script that walks
  // the table off the end relies on that.
  CHECK(!bench.truthy(kFree, "DiplGetCeaseFire",
                      {script::Value::integer(0), script::Value::integer(2)}));
  CHECK(!bench.truthy(kFree, "DiplAreAllied",
                      {script::Value::integer(1), script::Value::integer(17)}));
  CHECK(bench.call(kFree, "DiplGetCeaseFire",
                   {script::Value::integer(0), script::Value::integer(2)})
            .status == script::HostStatus::ok);
}

TEST(is_enemy_takes_a_player_number_or_an_object) {
  Bench bench;
  PlayerTable& players = bench.world.players();
  // Player 3 tolerates player 5; player 5 does not tolerate player 3.
  players.set_relation_word(3, 5, kRelationFriendly);
  players.set_relation_word(5, 3, 0);

  const ObjectId held_by_five = bench.world.spawn(NativeClass::unit, nullptr);
  const ObjectId held_by_three = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.set_owner(held_by_five, 5);
  bench.world.set_owner(held_by_three, 3);
  const script::Value five = script::Value::object(kTypeObj, held_by_five);
  const script::Value three = script::Value::object(kTypeObj, held_by_three);

  // Object form: the receiver's owner is the asker.
  CHECK(bench.truthy(kMember, "IsEnemy", {five, three}));
  CHECK(!bench.truthy(kMember, "IsEnemy", {three, five}));

  // Integer form: the *argument* is the asker, and it is 1-based.
  CHECK(bench.truthy(kMember, "IsEnemy", {three, script::Value::integer(6)}));
  CHECK(!bench.truthy(kMember, "IsEnemy", {five, script::Value::integer(4)}));

  // An unowned receiver is nobody's enemy rather than everybody's.
  const ObjectId unowned = bench.world.spawn(NativeClass::unit, nullptr);
  CHECK(!bench.truthy(kMember, "IsEnemy",
                      {script::Value::object(kTypeObj, unowned), three}));
}

TEST(is_own_and_is_ally_read_the_argument_as_the_asker) {
  Bench bench;
  PlayerTable& players = bench.world.players();
  players.set_relation_word(1, 2, kRelationFriendly);
  players.set_relation_word(2, 1, 0);

  const ObjectId owned_by_two = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.set_owner(owned_by_two, 2);
  const script::Value obj = script::Value::object(kTypeObj, owned_by_two);

  CHECK(bench.truthy(kMember, "IsOwn", {obj, script::Value::integer(3)}));
  CHECK(!bench.truthy(kMember, "IsOwn", {obj, script::Value::integer(2)}));
  CHECK(!bench.truthy(kMember, "IsOwn", {obj, script::Value::integer(0)}));

  // Player 1 (script 2) extends a ceasefire to player 2 (script 3).
  CHECK(bench.truthy(kMember, "IsAlly", {obj, script::Value::integer(2)}));
  // Player 2 does not extend one back, so asking as player 2 about its own
  // object still says yes -- the diagonal -- but asking as player 3 about a
  // player-1 object would not.
  const ObjectId owned_by_one = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.set_owner(owned_by_one, 1);
  const script::Value other = script::Value::object(kTypeObj, owned_by_one);
  CHECK(!bench.truthy(kMember, "IsAlly", {other, script::Value::integer(3)}));
  CHECK(bench.truthy(kMember, "IsAlly", {obj, script::Value::integer(3)}));
}

/// The three tests `RevealHiddenEnemyUnits` applies, one per assertion.
///
/// The circle, the hidden bit and the diplomacy row each have to be able to
/// refuse on their own, because in the original they are three separate
/// branches over one object and a test that only ever exercises the conjunction
/// cannot tell which one is missing.
TEST(reveal_hidden_enemy_units_clears_the_bit_on_enemy_units_in_the_circle) {
  Bench bench;
  PlayerTable& players = bench.world.players();
  // Player 0 is at war with player 1 and at peace with player 2. The rows are
  // written one way only: `is_enemy` reads the viewer's own row and the mask in
  // `gbr.exe` belongs to the revealing player, so the transpose must not matter.
  players.set_relation_word(0, 1, 0);
  players.set_relation_word(0, 2, kRelationFriendly);
  players.set_relation_word(1, 0, kRelationFriendly);

  const auto hidden_unit = [&](PlayerId owner, Point at) {
    const ObjectId id = bench.world.spawn(NativeClass::unit, nullptr);
    bench.world.set_owner(id, owner);
    bench.world.set_position(id, at);
    bench.world.mutable_state(id)->flags.hidden = true;
    return id;
  };
  const auto is_hidden = [&](ObjectId id) { return bench.world.find(id)->state.flags.hidden; };

  const ObjectId near_enemy = hidden_unit(1, Point{1000, 1000});
  // 700 from the centre against a radius of 500. The margin is deliberately
  // narrow: at 3,000 a doubled radius still would not reach it, and a fault
  // that doubles the radius would go unnoticed.
  const ObjectId far_enemy = hidden_unit(1, Point{1000, 1700});
  const ObjectId near_friend = hidden_unit(2, Point{1010, 1000});
  // An enemy unit in the circle that was never hiding. Clearing a bit that is
  // already clear changes nothing, so the `hidden` test in the sweep cannot be
  // caught by any assertion here -- see the fault sweep's annotated survivor.
  const ObjectId visible_enemy = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.set_owner(visible_enemy, 1);
  bench.world.set_position(visible_enemy, Point{1030, 1000});
  // Not a unit: `kSyncUnit` is the second test the scan applies, and a hidden
  // enemy *building* inside the circle stays hidden.
  const ObjectId near_building = bench.world.spawn(NativeClass::building, nullptr);
  bench.world.set_owner(near_building, 1);
  bench.world.set_position(near_building, Point{1020, 1000});
  bench.world.mutable_state(near_building)->flags.hidden = true;

  const script::HostOutcome out =
      bench.call(kFree, "RevealHiddenEnemyUnits",
                 {pack_point(Point{1000, 1000}), script::Value::integer(500),
                  script::Value::integer(1)});
  CHECK(out.status == script::HostStatus::ok);

  CHECK(!is_hidden(near_enemy));
  CHECK(is_hidden(far_enemy));
  CHECK(!is_hidden(visible_enemy));
  CHECK(is_hidden(near_friend));
  CHECK(is_hidden(near_building));
}

/// A unit that was never hiding is not touched, and the reveal is observable
/// through the two entry points that read the bit.
TEST(reveal_hidden_enemy_units_is_visible_to_isvisible_and_cansee) {
  Bench bench;
  bench.world.players().set_relation_word(0, 1, 0);

  const ObjectId observer = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.set_owner(observer, 0);
  const ObjectId sneak = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.set_owner(sneak, 1);
  bench.world.set_position(sneak, Point{2000, 2000});
  bench.world.mutable_state(sneak)->flags.hidden = true;

  const script::Value looker = script::Value::object(kTypeObj, observer);
  const script::Value target = script::Value::object(kTypeObj, sneak);
  CHECK(!bench.truthy(kMember, "IsVisible", {target}));
  CHECK(!bench.truthy(kMember, "CanSee", {looker, target}));

  CHECK(bench.call(kFree, "RevealHiddenEnemyUnits",
                   {pack_point(Point{2000, 2000}), script::Value::integer(64),
                    script::Value::integer(1)})
            .status == script::HostStatus::ok);

  CHECK(bench.truthy(kMember, "IsVisible", {target}));
  CHECK(bench.truthy(kMember, "CanSee", {looker, target}));
}

/// The argument guards. A player number outside 1..16 reveals nothing, which is
/// the one place this diverges from `gbr.exe` on purpose: 0x004c8460 has no
/// bounds check and indexes a record that is not there, and its sibling reader
/// answers all-ones for the same number -- reveal *everything*. Neither is
/// transcribable, so the refusal is the answer.
TEST(reveal_hidden_enemy_units_refuses_a_bad_point_radius_or_player) {
  Bench bench;
  bench.world.players().set_relation_word(0, 1, 0);
  const ObjectId sneak = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.set_owner(sneak, 1);
  bench.world.set_position(sneak, Point{0, 0});
  bench.world.mutable_state(sneak)->flags.hidden = true;

  const script::Value here = pack_point(Point{0, 0});
  CHECK(bench.call(kFree, "RevealHiddenEnemyUnits",
                   {script::Value::integer(0), script::Value::integer(64),
                    script::Value::integer(1)})
            .status == script::HostStatus::error);
  CHECK(bench.call(kFree, "RevealHiddenEnemyUnits",
                   {here, script::Value::string("far"), script::Value::integer(1)})
            .status == script::HostStatus::error);

  // Out of range is silent rather than a trap: `.player` is 1..16 and a script
  // that walks a table of fewer players relies on the quiet answer, exactly as
  // the five `Dipl*` getters above do.
  for (const std::int32_t player : {0, 17, -1}) {
    const script::HostOutcome out =
        bench.call(kFree, "RevealHiddenEnemyUnits",
                   {here, script::Value::integer(64), script::Value::integer(player)});
    CHECK(out.status == script::HostStatus::ok);
  }
  CHECK(bench.world.find(sneak)->state.flags.hidden);
}

TEST(enemy_objs_mints_a_player_flags_query_with_the_right_type) {
  Bench bench;
  const script::HostOutcome enemies =
      bench.call(kFree, "EnemyObjs", {script::Value::integer(3), script::Value::string("Unit")});
  CHECK(enemies.status == script::HostStatus::ok);
  CHECK(enemies.value.is_object());
  const QuerySpec* spec = bench.world.query_spec(enemies.value.as_object().id);
  CHECK(spec != nullptr);
  if (spec != nullptr) {
    CHECK(spec->kind == QueryKind::player_flags);
    // 1-based on the wire, 0-based in the spec.
    CHECK(spec->player == 2);
    CHECK(spec->flags_type == kPlayerFlagsEnemy);
  }

  const script::HostOutcome friends = bench.call(
      kFree, "FriendlyObjs", {script::Value::integer(3), script::Value::string("Unit")});
  const QuerySpec* fspec = bench.world.query_spec(friends.value.as_object().id);
  CHECK(fspec != nullptr && fspec->flags_type == kPlayerFlagsFriendly);

  const script::HostOutcome controllable = bench.call(
      kFree, "ControllableObjs", {script::Value::integer(3), script::Value::string("Unit")});
  const QuerySpec* cspec = bench.world.query_spec(controllable.value.as_object().id);
  CHECK(cspec != nullptr && cspec->flags_type == kPlayerFlagsControllable);

  // Out of range gives an invalid handle, which reads as an empty query rather
  // than as player zero's.
  const script::HostOutcome bad =
      bench.call(kFree, "EnemyObjs", {script::Value::integer(0), script::Value::string("Unit")});
  CHECK(bad.status == script::HostStatus::ok);
  CHECK(bad.value.is_object() && !bad.value.as_object().valid());
}

/// `Party()` is every object carrying the party flag, in id order, in a fresh
/// list each time.
TEST(party_lists_the_flagged_objects_in_id_order) {
  Bench bench;
  const ObjectId a = bench.world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = bench.world.spawn(NativeClass::unit, nullptr);
  const ObjectId c = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.find(c)->state.flags.in_party = true;
  bench.world.find(a)->state.flags.in_party = true;

  const script::HostOutcome out = bench.call(kFree, "Party", {});
  CHECK(out.status == script::HostStatus::ok);
  const ObjListId list = objlist_of(out.value);
  REQUIRE(list != 0);
  const std::span<const ObjectId> items = objlist_pool_of(bench.world).items(list);
  REQUIRE(items.size() == 2);
  CHECK(items[0] == a);
  CHECK(items[1] == c);
  (void)b;

  // Two calls, two lists: the original mints a new one each time.
  const script::HostOutcome again = bench.call(kFree, "Party", {});
  CHECK(objlist_of(again.value) != list);
}

TEST(get_player_units_returns_an_objlist) {
  Bench bench;
  const ObjectId a = bench.world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = bench.world.spawn(NativeClass::unit, nullptr);
  bench.world.set_owner(a, 4);
  bench.world.set_owner(b, 5);

  const script::HostOutcome out =
      bench.call(kFree, "GetPlayerUnits", {script::Value::integer(5)});
  CHECK(out.status == script::HostStatus::ok);
  CHECK(is_objlist(out.value));
  const std::span<const ObjectId> items = objlist_pool_of(bench.world).items(objlist_of(out.value));
  // No class graph is loaded, so the filter matches everything the player owns:
  // exactly the one object.
  CHECK(items.size() == 1);
  if (items.size() == 1) CHECK(items[0] == a);

  // Out of range still yields a real, empty list rather than an invalid handle:
  // every call site indexes it immediately.
  const script::HostOutcome empty =
      bench.call(kFree, "GetPlayerUnits", {script::Value::integer(0)});
  CHECK(is_objlist(empty.value));
  CHECK(objlist_pool_of(bench.world).items(objlist_of(empty.value)).empty());
}

TEST(per_player_facts_come_from_the_setup) {
  Bench bench;
  PlayerTable& players = bench.world.players();
  players.setup(0).control = PlayerControl::computer;
  players.setup(0).race = "Carthage";
  players.setup(1).control = PlayerControl::both;
  players.setup(1).race = "Random";
  players.setup(2).control = PlayerControl::disabled;

  CHECK(bench.truthy(kFree, "IsAIPlayer", {script::Value::integer(1)}));
  CHECK(!bench.truthy(kFree, "IsAIPlayer", {script::Value::integer(2)}));
  CHECK(!bench.truthy(kFree, "IsAIPlayer", {script::Value::integer(3)}));
  CHECK(!bench.truthy(kFree, "IsAIPlayer", {script::Value::integer(0)}));

  const script::HostOutcome carthage =
      bench.call(kFree, "GetPlayerRace", {script::Value::integer(1)});
  CHECK(carthage.value.as_integer() == 2);
  const script::HostOutcome unresolved =
      bench.call(kFree, "GetPlayerRace", {script::Value::integer(2)});
  CHECK(unresolved.value.as_integer() == kNoRace);

  // `SetPlayerStatus` is a validated no-op, but it must still succeed: it is on
  // the path of every win/lose script.
  CHECK(bench.call(kFree, "SetPlayerStatus",
                   {script::Value::integer(1), script::Value::integer(1),
                    script::Value::integer(1)})
            .status == script::HostStatus::ok);
  CHECK(bench.call(kFree, "SetPlayerStatus",
                   {script::Value::integer(1), script::Value::integer(1),
                    script::Value::string("You win"), script::Value::integer(1)})
            .status == script::HostStatus::ok);
}

TEST(counters_are_one_based_and_outposts_zero_means_all) {
  Bench bench;
  // No economy system is attached, so every count is zero -- but the argument
  // handling is what this asserts, and it must not trap or read out of range.
  CHECK(bench.call(kFree, "Strongholds", {script::Value::integer(1)}).value.as_integer() == 0);
  CHECK(bench.call(kFree, "Outposts", {script::Value::integer(0)}).value.as_integer() == 0);
  CHECK(bench.call(kFree, "Outposts", {script::Value::integer(16)}).value.as_integer() == 0);
  CHECK(bench.call(kFree, "Outposts", {script::Value::integer(17)}).value.as_integer() == 0);
  CHECK(bench.call(kFree, "MilUnits", {script::Value::integer(1)}).value.as_integer() == 0);
  CHECK(bench.call(kFree, "MilUnits", {script::Value::integer(99)}).value.as_integer() == 0);
}

// --------------------------------------------------------------------------
// the four Dipl* writers, and the one that wipes the table
// --------------------------------------------------------------------------

namespace {

/// The whole relation matrix as one number, for the assertions that are about
/// what did *not* change. Spot-checking cells cannot see a partial write into a
/// valid row before an invalid one is noticed; this can.
[[nodiscard]] std::uint64_t matrix_hash(const World& world) {
  std::uint64_t accumulator = 0;
  world.players().hash(accumulator);
  return accumulator;
}

}  // namespace

TEST(dipl_ceasefire_writes_one_row_and_not_the_transpose) {
  // The load-bearing assertion of the whole family. The matrix is not
  // symmetric, hostility is one-directional, and the shipped data contains
  // one-sided truces a symmetric model misplays -- so a writer that helpfully
  // wrote both rows would be wrong in a way that only shows up on real
  // missions. `gbr.exe` ships a *separate* symmetric API (`_PlayersAlly` and
  // its four siblings, none of which has a call site) for the other reading.
  Bench b;
  CHECK(b.call(kFree, "DiplCeaseFire",
               {script::Value::integer(1), script::Value::integer(2),
                script::Value::boolean(true)})
            .status == script::HostStatus::ok);

  CHECK(b.world.players().relation_word(0, 1) == kRelationCeasefireOnly);
  CHECK(b.world.players().relation_word(1, 0) == 0);
  CHECK(b.truthy(kFree, "DiplGetCeaseFire", {script::Value::integer(1), script::Value::integer(2)}));
  CHECK(!b.truthy(kFree, "DiplGetCeaseFire",
                  {script::Value::integer(2), script::Value::integer(1)}));

  // Half an alliance is not an alliance: `DiplAreAllied` reads bit 0 in both
  // rows, which is why every campaign alliance in the corpus is a mirrored
  // pair. A both-rows implementation answers true here.
  CHECK(!b.truthy(kFree, "DiplAreAllied", {script::Value::integer(1), script::Value::integer(2)}));

  // And a run-time one-sided truce is a real state: 1 does not attack 2, and 2
  // still attacks 1.
  CHECK(!b.world.players().is_enemy(0, 1));
  CHECK(b.world.players().is_enemy(1, 0));

  CHECK(b.call(kFree, "DiplCeaseFire",
               {script::Value::integer(2), script::Value::integer(1),
                script::Value::boolean(true)})
            .status == script::HostStatus::ok);
  CHECK(b.truthy(kFree, "DiplAreAllied", {script::Value::integer(1), script::Value::integer(2)}));
}

TEST(dipl_setters_touch_one_bit_and_leave_the_others_alone) {
  // Read-modify-write of one mask, not an assignment of the word. The four
  // masks are pinned in one place here, which is also the clearest statement
  // that five named relations sit on four bits.
  Bench b;
  b.world.players().set_relation_word(0, 1, kRelationSelf);
  const auto clear = [&](const char* name) {
    CHECK(b.call(kFree, name,
                 {script::Value::integer(1), script::Value::integer(2),
                  script::Value::boolean(false)})
              .status == script::HostStatus::ok);
  };
  clear("DiplCeaseFire");
  CHECK(b.world.players().relation_word(0, 1) == 0x34);
  clear("DiplShareControl");
  CHECK(b.world.players().relation_word(0, 1) == 0x14);
  clear("DiplShareSupport");
  CHECK(b.world.players().relation_word(0, 1) == 0x10);
  clear("DiplShareView");
  CHECK(b.world.players().relation_word(0, 1) == 0);

  // And back up again, in a different order, landing on the same word.
  const auto set = [&](const char* name) {
    CHECK(b.call(kFree, name,
                 {script::Value::integer(1), script::Value::integer(2),
                  script::Value::boolean(true)})
              .status == script::HostStatus::ok);
  };
  set("DiplShareSupport");
  set("DiplCeaseFire");
  set("DiplShareView");
  set("DiplShareControl");
  CHECK(b.world.players().relation_word(0, 1) == kRelationSelf);
}

TEST(dipl_setters_ignore_an_out_of_range_player_without_trapping) {
  // The original prints and returns; trapping here would kill a script that
  // walks 1..16 over a table of fewer players. And the check is
  // all-or-nothing -- if either number is out of range neither row is touched
  // -- so the assertion is on the whole matrix rather than on one cell.
  Bench b;
  b.world.players().set_relation_word(0, 1, kRelationFriendly);
  const std::uint64_t before = matrix_hash(b.world);

  const std::int32_t bad[][2] = {{0, 2}, {17, 2}, {1, 0}, {1, 17}, {-1, 1}, {1, 99}};
  for (const char* name :
       {"DiplCeaseFire", "DiplShareView", "DiplShareSupport", "DiplShareControl"}) {
    for (const auto& pair : bad) {
      const script::HostOutcome out =
          b.call(kFree, name,
                 {script::Value::integer(pair[0]), script::Value::integer(pair[1]),
                  script::Value::boolean(true)});
      CHECK(out.status == script::HostStatus::ok);
    }
  }
  CHECK(matrix_hash(b.world) == before);
}

TEST(the_relation_table_refuses_an_id_outside_the_matrix) {
  // The backstop under the host slice's own range check, and it was asserted
  // by nothing until a fault-injection run went looking. Injecting a partial
  // write into `fn_dipl_set` -- validate one player, write, then notice the
  // other is out of range -- **survived the whole suite**, because
  // `PlayerTable::set` refuses the write on its own and the host guard is
  // belt-and-braces. A property held up by two checks and asserted by neither
  // is one check away from being held up by none, so it is pinned here, at the
  // layer that actually enforces it.
  PlayerTable players;
  players.set_relation_word(0, 1, kRelationFriendly);
  players.set_relation_word(1, 0, kRelationFriendly);
  std::uint64_t before = 0;
  players.hash(before);

  // Every probe **clears** a bit that is set, and none of them targets the
  // diagonal. Both halves are deliberate. A first version of this test set
  // bits instead, and an out-of-range id clamped to 0 lands on `[0][0]`, where
  // `kRelationSelf` already has all four -- so the injected fault wrote and
  // changed nothing, and the test passed for the wrong reason. Same shape as
  // the combat-enrolment case the plan records: a fault that repairs itself
  // reads exactly like a fault that was caught.
  players.set(1, kNoPlayer, Relation::ceasefire, false);
  players.set(kNoPlayer, 1, Relation::ceasefire, false);
  players.set(0, static_cast<PlayerId>(kPlayerCount), Relation::share_view, false);
  players.set_relation_word(kNoPlayer, 0, 0xFFFF);
  players.set_relation_word(0, static_cast<PlayerId>(kPlayerCount), 0xFFFF);

  std::uint64_t after = 0;
  players.hash(after);
  CHECK(after == before);
  CHECK(players.relation_word(0, 1) == kRelationFriendly);
  CHECK(players.relation_word(1, 0) == kRelationFriendly);
  CHECK(players.relation_word(0, 0) == kRelationSelf);
  // And reading out of range is a miss rather than a fault.
  CHECK(players.relation_word(kNoPlayer, 0) == 0);
  CHECK(!players.has(0, kNoPlayer, Relation::ceasefire));
}

TEST(dipl_setters_leave_the_diagonal_alone) {
  // `SetRelation`'s own self-guard. Without it a `DiplCeaseFire(3, 3, false)`
  // clears bit 0 of `kRelationSelf`, which moves `PlayerTable::hash` and
  // therefore every conformance run, for a call no shipped script makes.
  Bench b;
  const std::uint64_t before = matrix_hash(b.world);
  CHECK(b.call(kFree, "DiplCeaseFire",
               {script::Value::integer(3), script::Value::integer(3),
                script::Value::boolean(false)})
            .status == script::HostStatus::ok);
  CHECK(b.world.players().relation_word(2, 2) == kRelationSelf);
  CHECK(matrix_hash(b.world) == before);
}

TEST(the_egypt_intel_beat_stays_one_directional) {
  // The shipped call that is not half of a mirrored pair. 211 of the corpus's
  // 215 setter calls are written `(a,b,x)` then `(b,a,x)`; this one is
  // `3_Great_Losses_Egypt` `Maps/1/Sequences/seq1.vs:227`, a lone
  // `DiplShareView(4, 1, true)` fired inside `if (i == 4)` right after
  // `RunConv("C_Conv6")` -- an ally showing the player what it can see.
  //
  // Under a both-rows implementation player 1 gains sight of player 4's map as
  // well, and the beat means the opposite of what it says.
  Bench b;
  CHECK(b.call(kFree, "DiplShareView",
               {script::Value::integer(4), script::Value::integer(1),
                script::Value::boolean(true)})
            .status == script::HostStatus::ok);

  CHECK(b.world.players().relation_word(3, 0) == 0x10);
  CHECK(b.world.players().relation_word(0, 3) == 0);
  CHECK(b.truthy(kFree, "DiplGetShareView", {script::Value::integer(4), script::Value::integer(1)}));
  CHECK(!b.truthy(kFree, "DiplGetShareView",
                  {script::Value::integer(1), script::Value::integer(4)}));
  // Bit 0 was not touched, so the two are still at war while one of them can
  // see. That combination is the whole point of a four-bit relation word.
  CHECK(b.world.players().is_enemy(3, 0));
}

TEST(clear_diplomacy_zeroes_the_off_diagonal_and_keeps_the_diagonal) {
  // The loop writes `kRelationSelf` to the diagonal too, and `SetRelation`
  // discards that store because the other record is this record -- so the
  // diagonal is *untouched* rather than rewritten. Indistinguishable on
  // shipped data, where every diagonal is already `kRelationSelf`; not
  // indistinguishable from a loop that skips nothing.
  Bench b;
  b.world.players().set_relation_word(0, 1, kRelationSelf);
  b.world.players().set_relation_word(1, 0, kRelationAllied);
  b.world.players().set_relation_word(4, 9, kRelationFriendly);
  b.world.players().set_relation_word(2, 2, 0x99);  // a diagonal that is not the default

  CHECK(b.call(kFree, "ClearDiplomacy", {}).status == script::HostStatus::ok);

  for (std::size_t from = 0; from < kPlayerCount; ++from) {
    for (std::size_t to = 0; to < kPlayerCount; ++to) {
      const std::uint32_t word = b.world.players().relation_word(
          static_cast<PlayerId>(from), static_cast<PlayerId>(to));
      if (from == to) continue;
      CHECK(word == 0);
      CHECK(b.world.players().is_enemy(static_cast<PlayerId>(from), static_cast<PlayerId>(to)));
    }
  }
  // The one diagonal that was deliberately not the default is still itself.
  CHECK(b.world.players().relation_word(2, 2) == 0x99);
  CHECK(b.world.players().relation_word(0, 0) == kRelationSelf);

  // Idempotent.
  const std::uint64_t once = matrix_hash(b.world);
  CHECK(b.call(kFree, "ClearDiplomacy", {}).status == script::HostStatus::ok);
  CHECK(matrix_hash(b.world) == once);
}

TEST(clear_diplomacy_then_declare_teams_is_the_zama_opening) {
  // The shipped idiom, end to end: `ClearDiplomacy()` on line 2 of a mission's
  // opening sequence, then the mirrored pairs that say who is on whose side.
  // This is the test that catches a `ClearDiplomacy` which wiped the diagonal
  // too, because `are_allied(p, p)` would start answering wrong.
  Bench b;
  CHECK(b.call(kFree, "ClearDiplomacy", {}).status == script::HostStatus::ok);

  const auto ally = [&](std::int32_t a, std::int32_t c) {
    for (const auto& pair : {std::pair{a, c}, std::pair{c, a}}) {
      CHECK(b.call(kFree, "DiplCeaseFire",
                   {script::Value::integer(pair.first), script::Value::integer(pair.second),
                    script::Value::boolean(true)})
                .status == script::HostStatus::ok);
    }
  };
  ally(1, 2);
  ally(1, 3);
  ally(2, 3);
  ally(4, 5);

  CHECK(b.truthy(kFree, "DiplAreAllied", {script::Value::integer(1), script::Value::integer(3)}));
  CHECK(b.truthy(kFree, "DiplAreAllied", {script::Value::integer(2), script::Value::integer(3)}));
  CHECK(b.truthy(kFree, "DiplAreAllied", {script::Value::integer(4), script::Value::integer(5)}));
  CHECK(!b.truthy(kFree, "DiplAreAllied", {script::Value::integer(1), script::Value::integer(4)}));
  CHECK(b.world.players().is_enemy(0, 3));
  CHECK(b.world.players().is_enemy(3, 0));
  CHECK(!b.world.players().is_enemy(0, 1));
  // The diagonal survived, which is what makes a player its own ally.
  CHECK(b.truthy(kFree, "DiplAreAllied", {script::Value::integer(6), script::Value::integer(6)}));
}

/// `GetClassRace(str)` is `Obj::race` with the class named instead of carried.
///
/// It exists because `BARRACK_TRAIN.VS` asks about a class it is *about to*
/// train, before anything of that class exists -- so the answer has to come
/// from the graph and not from an object, and there is no object to guard on.
TEST(get_class_race_answers_the_named_class_and_minus_one_for_anything_else) {
  RaceBench b;
  const auto race = [&](const char* name) {
    const std::uint32_t index = b.registry.find(kFree, "GetClassRace", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return -999;
    std::vector<script::Value> args{script::Value::string(std::string(name))};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &b.context;
    ctx.name = "GetClassRace";
    ctx.kind = kFree;
    const script::HostOutcome out = b.registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -999;
  };

  CHECK(race("ETownhall") == 6);      // Egypt
  CHECK(race("MVelit") == 4);         // ImperialRome
  // `race="None"` and a class that declares no race at all are both `kNoRace`,
  // which is the same answer a *missing* class gives -- so a script comparing
  // against a real race cannot match on any of the three.
  // By `altid` as well as by `id`, which is how every other script-supplied
  // class name here resolves.
  CHECK(race("GSwordsman") == 0);  // Gaul
  CHECK(race("GaulSword") == 0);
  CHECK(race("Rock") == kNoRace);
  CHECK(race("Object") == kNoRace);
  CHECK(race("NoSuchClass") == kNoRace);
  CHECK(race("") == kNoRace);

  // And it agrees with the member form on an object of the same class, which
  // is the equality the two entry points exist to keep.
  const ObjectId egyptian = b.make("ETownhall");
  const std::uint32_t member = b.registry.find(kMember, "race", 0);
  REQUIRE(member != script::kUnresolvedHost);
  std::vector<script::Value> args{b.obj(egyptian)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &b.context;
  ctx.name = "race";
  ctx.kind = kMember;
  CHECK(b.registry.entry(member).fn(ctx).value.as_integer() == race("ETownhall"));
}

/// Three globals that are accepted and dropped, on `GlobalSpellStart`'s
/// precedent -- and the assertion is that they *run*, because the alternative
/// to a body here is a trap that stops the script.
TEST(the_three_consumerless_globals_run_and_answer_the_retail_value) {
  Bench b;
  CHECK(b.call(kFree, "SetGlobalBloodlust", {script::Value::boolean(true)}).status ==
        script::HostStatus::ok);
  CHECK(b.call(kFree, "SetGlobalBloodlust", {script::Value::boolean(false)}).status ==
        script::HostStatus::ok);
  CHECK(b.call(kFree, "InvalidateRegenConsts", {}).status == script::HostStatus::ok);

  // `GetResearchHack` is false, and stays false: the only writer of the global
  // it reads is a debug path that sets and clears it inside one function, so
  // nothing a script or a map can do turns it on.
  CHECK(!b.truthy(kFree, "GetResearchHack", {}));
  CHECK(b.call(kFree, "SetGlobalBloodlust", {script::Value::boolean(true)}).status ==
        script::HostStatus::ok);
  CHECK(!b.truthy(kFree, "GetResearchHack", {}));
}
