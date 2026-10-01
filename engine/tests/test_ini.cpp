// INI reader tests.
//
// Synthetic, like the rest of this suite: every fixture below reproduces a
// shape that occurs in the shipped files rather than a shape a general INI
// parser might have to handle. The dialect was measured before the reader was
// written -- across all 150 `.ini` files in the retail packs there are 3,662
// section headers, 18,094 `key = value` lines, 3,278 bare lines, 672 whole-line
// comments and 408 inline ones, and nothing else.
//
// `tests/test_corpus_ini.py` asserts those counts against a real installation,
// and it earned its place: the first draft of this reader shipped with three
// wrong numbers in its documentation because the corpus scan behind them had
// enumerated packs by hand and missed one.

#include <cstring>
#include <span>
#include <string_view>

#include "imperivm/core/formats/ini.hpp"
#include "test.hpp"

using namespace imperivm::core;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

TEST(ini_parses_sections_and_pairs) {
  constexpr std::string_view kFile =
      "[First]\n"
      "Alpha = 1\n"
      "Beta=two\n"
      "\n"
      "[Second]\n"
      "Gamma = 3\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();

  REQUIRE(ini.sections().size() == 2);
  CHECK(ini.sections()[0].name == "First");
  CHECK(ini.sections()[1].name == "Second");

  const SectionIndex first = ini.section("First");
  REQUIRE(first != kNoSection);
  CHECK(ini.entries_of(first).size() == 2);
  CHECK(ini.value(first, "Alpha") == "1");
  CHECK(ini.value(first, "Beta") == "two");
  CHECK(ini.value_int(first, "Alpha", -1) == 1);
}

TEST(ini_looks_up_sections_and_keys_without_regard_to_case) {
  constexpr std::string_view kFile = "[SquadStates]\nKey = 7\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();

  CHECK(ini.section("squadstates") != kNoSection);
  CHECK(ini.section("SQUADSTATES") != kNoSection);
  CHECK(ini.value(ini.section("SquadStates"), "KEY") == "7");
}

TEST(ini_keeps_bare_lines_as_ordered_entries) {
  // `[SquadStates]` in `DATA/AI/AI.INI` is an ordered enum declaration, not a
  // map: the line's position is the constant's value. 3,278 lines in the
  // shipped corpus are of this shape.
  constexpr std::string_view kFile =
      "[SquadStates]\n"
      "SS_Approach\n"
      "SS_Wait\n"
      "SS_ApproachWait\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();

  const auto entries = ini.entries_of(ini.section("SquadStates"));
  REQUIRE(entries.size() == 3);
  CHECK(!entries[0].has_key);
  CHECK(entries[0].value == "SS_Approach");
  CHECK(entries[1].value == "SS_Wait");
  CHECK(entries[2].value == "SS_ApproachWait");

  // A bare line must never answer a lookup by key, or an enum member would
  // shadow a variable of the same name.
  CHECK(ini.value(ini.section("SquadStates"), "SS_Wait", "absent") == "absent");
}

TEST(ini_distinguishes_a_bare_line_from_an_empty_value) {
  constexpr std::string_view kFile = "[S]\nBare\nEmpty =\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const auto entries = document.value().entries_of(document.value().section("S"));
  REQUIRE(entries.size() == 2);
  CHECK(!entries[0].has_key);
  CHECK(entries[1].has_key);
  CHECK(entries[1].key == "Empty");
  CHECK(entries[1].value.empty());
}

TEST(ini_strips_comments_from_values_and_from_section_headers) {
  // Twelve headers in the shipped corpus carry a trailing comment, all in the
  // editor's TEMPLATE.INI and ADVOBJPROPS.INI.
  constexpr std::string_view kFile =
      "; a whole-line comment\n"
      "[SingleLineEdit] ; A single line edit :)\n"
      "PopGroup=5 ; peasants come in groups of this many\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();

  REQUIRE(ini.sections().size() == 1);
  CHECK(ini.sections()[0].name == "SingleLineEdit");
  CHECK(ini.value(ini.section("SingleLineEdit"), "PopGroup") == "5");
}

TEST(ini_does_not_treat_hash_or_slashes_as_comments) {
  // No shipped file uses either, so treating them as comments would be a
  // behaviour nothing asked for and something could silently depend on.
  constexpr std::string_view kFile = "[S]\nA = # not a comment\nB = // neither\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();
  CHECK(ini.value(ini.section("S"), "A") == "# not a comment");
  CHECK(ini.value(ini.section("S"), "B") == "// neither");
}

TEST(ini_rejects_an_unterminated_section_header) {
  constexpr std::string_view kFile = "[Unterminated\nA = 1\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  CHECK(!document.ok());
  CHECK(document.error() == FormatError::malformed);
}

TEST(ini_takes_the_first_of_a_duplicated_key) {
  // 22 keys in the corpus are declared twice within their own section and nine
  // carry different values. Something has to break the tie. Win32's
  // `GetPrivateProfileString`, which is how a game of this vintage reads `.ini`
  // files, returns the first match in a section, so that is what this does.
  // **Inferred**: nothing in the shipped data proves the original used the
  // platform reader rather than its own.
  //
  // Both entries survive in `entries_of`, which is the part that matters:
  // `UNITICONS.INI`'s `[FillCombo]` gives one label to three different bitmaps,
  // so a section that resolved duplicates on the way in would lose data.
  constexpr std::string_view kFile =
      "[Vars.All]\n"
      "AIV_SquanderGoldAmount=30000 ; spending without restrictions\n"
      "AIV_SquanderGoldAmount=15000 ; spending for research\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();
  // Both entries are preserved; it is the lookup that resolves the conflict.
  CHECK(ini.entries_of(ini.section("Vars.All")).size() == 2);
  CHECK(ini.value(ini.section("Vars.All"), "AIV_SquanderGoldAmount") == "30000");
}

TEST(ini_parses_a_whole_value_or_none_of_it) {
  std::int32_t out = -1;
  CHECK(parse_int("2000", out) && out == 2000);
  CHECK(parse_int("  -42  ", out) && out == -42);
  CHECK(parse_int("+7", out) && out == 7);

  // The trap this project already fell into: `ProductionInterval` was recorded
  // as 20 when it is 2000, because a value was read part-way. A parser that
  // stops at the first non-digit and returns what it has makes that silent.
  out = -1;
  CHECK(!parse_int("2000x", out));
  CHECK(!parse_int("20 00", out));
  CHECK(!parse_int("", out));
  CHECK(!parse_int("-", out));
  CHECK(!parse_int("0x10", out));
  CHECK(out == -1);

  CHECK(parse_int("2147483647", out) && out == 2147483647);
  CHECK(parse_int("-2147483648", out) && out == -2147483647 - 1);
  CHECK(!parse_int("2147483648", out));
  CHECK(!parse_int("-2147483649", out));
}

TEST(ini_value_int_falls_back_rather_than_truncating) {
  constexpr std::string_view kFile = "[S]\nGood = 2000\nBad = 20abc\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();
  CHECK(ini.value_int(ini.section("S"), "Good", -1) == 2000);
  CHECK(ini.value_int(ini.section("S"), "Bad", -1) == -1);
  CHECK(ini.value_int(ini.section("S"), "Missing", -1) == -1);
}

TEST(ini_splits_comma_lists_and_keeps_holes) {
  const auto three = split_list("a, b ,c");
  REQUIRE(three.size() == 3);
  CHECK(three[0] == "a");
  CHECK(three[1] == "b");
  CHECK(three[2] == "c");

  // A positional list with a hole means something different from a short one.
  const auto hole = split_list("a,,c");
  REQUIRE(hole.size() == 3);
  CHECK(hole[1].empty());

  CHECK(split_list("").empty());
  CHECK(split_list("solo").size() == 1);
}

TEST(ini_handles_a_file_with_no_trailing_newline_and_crlf_lines) {
  constexpr std::string_view kFile = "[S]\r\nA = 1\r\nB = 2";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();
  CHECK(ini.value(ini.section("S"), "A") == "1");
  CHECK(ini.value(ini.section("S"), "B") == "2");
}

TEST(ini_puts_entries_before_any_header_in_an_implicit_section) {
  // No shipped file needs this. Dropping the lines would be the wrong way to
  // discover that a hand-edited one does.
  constexpr std::string_view kFile = "Loose = 1\n[S]\nA = 2\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();
  REQUIRE(ini.sections().size() == 2);
  CHECK(ini.sections()[0].name.empty());
  CHECK(ini.value(0, "Loose") == "1");
}

TEST(ini_reports_one_based_line_numbers) {
  constexpr std::string_view kFile = "\n[S]\n\nA = 1\n";
  const auto document = IniDocument::parse(bytes_of(kFile));
  REQUIRE(document.ok());
  const IniDocument& ini = document.value();
  REQUIRE(ini.sections().size() == 1);
  CHECK(ini.sections()[0].line == 2);
  REQUIRE(ini.entries().size() == 1);
  CHECK(ini.entries()[0].line == 4);
}
