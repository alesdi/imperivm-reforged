// Translation table, substitution, and the string host entry points.
//
// Synthetic, like the rest of this suite. The shapes come from the retail
// table: `CURRENTLANG\TRANSLATION.LOC.XML` in the language pack, 3,887 entries,
// of which 2,744 carry a `context` and every one has a non-empty `result` whose
// placeholders match its key's. The reader was run over that file and parses
// all 3,887.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/text.hpp"
#include "test.hpp"

using namespace imperivm::core;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A translation table in the shipped shape, four rows, written here rather
/// than copied -- `docs/legal.md` rule 1, and `tools/check_fixtures.py`.
///
/// Each row is a lookup key and its replacement, and the four cover the four
/// cases the reader has to tell apart:
///
///   * a plain key, whose `text` and `justtext` are the same string;
///   * a key carrying a `%s1` placeholder, which passes through untouched
///     because the substitution happens after the lookup, not during it;
///   * a key **disambiguated by context**, written `text@context`, where
///     `justtext` is the part before the `@` and `context` the part after --
///     the same word can need two translations, and this is how the table says
///     so; and
///   * a row with an empty key, which is dropped rather than stored, because a
///     lookup of the empty string must not answer somebody's translation.
constexpr std::string_view kTable =
    "<translationtable>"
    "<translationtableentry text=\"Out of the game\" justtext=\"Out of the game\" "
    "context=\"\" result=\"Fuori dal gioco\" comment=\"\"/>"
    "<translationtableentry text=\"Needs %s1\" justtext=\"Needs %s1\" "
    "context=\"\" result=\"Serve %s1\" comment=\"\"/>"
    "<translationtableentry text=\"Mend@mend arena\" justtext=\"Mend\" "
    "context=\"mend arena\" result=\"Aggiusta\" comment=\"\"/>"
    "<translationtableentry text=\"\" justtext=\"\" context=\"\" result=\"dropped\" "
    "comment=\"\"/>"
    "</translationtable>";

/// Records what `pr` was given.
class RecordingSink : public sim::DebugSink {
 public:
  void write(std::string_view text) override { lines.emplace_back(text); }
  void clear() override { lines.clear(); }
  std::vector<std::string> lines;
};

/// One host call, with a context the domain will accept.
struct HostCall {
  std::vector<script::Value> arguments;
  script::CallContext context;
  sim::HostContext state;

  explicit HostCall(std::initializer_list<script::Value> args) : arguments(args) {
    context.arguments = arguments;
    context.user = &state;
  }
};

[[nodiscard]] script::HostOutcome invoke(const script::HostRegistry& registry,
                                         std::string_view name, std::uint16_t arity,
                                         HostCall& call) {
  const std::uint32_t index =
      registry.find(script::CallKind::free_function, name, arity);
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
  const script::HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
  call.context.arguments = call.arguments;
  call.context.name = name;
  return entry.fn(call.context);
}

[[nodiscard]] script::HostRegistry text_registry() {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)sim::register_text_host(registry);
  return registry;
}

}  // namespace

TEST(localization_parses_a_table_and_looks_up_by_the_text_attribute) {
  const auto parsed = game::TranslationTable::parse(bytes_of(kTable));
  REQUIRE(parsed.ok());
  const game::TranslationTable& table = parsed.value();

  // The empty-key row is dropped: an entry nothing can name is not a
  // translation.
  CHECK(table.size() == 3);
  CHECK(table.translate("Out of the game") == "Fuori dal gioco");
  CHECK(table.translate("Needs %s1") == "Serve %s1");
}

TEST(localization_keys_include_the_context_suffix) {
  // 2,744 of the 3,887 shipped entries carry a context, and the suffix is part
  // of `@text` rather than something a lookup reassembles from `@context`.
  const auto parsed = game::TranslationTable::parse(bytes_of(kTable));
  REQUIRE(parsed.ok());
  const game::TranslationTable& table = parsed.value();
  CHECK(table.translate("Mend@mend arena") == "Aggiusta");
  // The bare `justtext` is not a key.
  CHECK(!table.contains("Mend"));
  CHECK(table.translate("Mend") == "Mend");
}

TEST(localization_falls_back_to_the_source_string) {
  const auto parsed = game::TranslationTable::parse(bytes_of(kTable));
  REQUIRE(parsed.ok());
  // A miss is not an error: the original ships a log alongside the table that
  // accumulates the strings it was asked for, which only works if a miss is
  // survivable.
  CHECK(parsed.value().translate("no such key") == "no such key");
  CHECK(!parsed.value().contains("no such key"));
}

TEST(localization_rejects_a_document_that_is_not_a_translation_table) {
  CHECK(!game::TranslationTable::parse(bytes_of("<something><else/></something>")).ok());
}

TEST(localization_substitutes_positional_slots) {
  const std::vector<std::string> args{"3", "07"};
  // `%sN` and `%dN` are the same thing. The corpus passes integers to `%s`, and
  // the `0` in `"%s1:0%s2"` is a literal the author typed because the language
  // has no field width.
  CHECK(game::substitute("Tra %s1:0%s2 minuti", args) == "Tra 3:007 minuti");
  CHECK(game::substitute("%d1 and %d2", args) == "3 and 07");
  CHECK(game::substitute("no slots", args) == "no slots");
}

TEST(localization_leaves_a_slot_with_no_argument_as_written) {
  const std::vector<std::string> args{"one"};
  // Blanking it would hide the mismatch in exactly the string a tester is
  // staring at.
  CHECK(game::substitute("a %s1 b %s4 c", args) == "a one b %s4 c");
  CHECK(game::substitute("%s1", {}) == "%s1");
}

TEST(localization_treats_an_unrecognised_percent_as_literal) {
  const std::vector<std::string> args{"x"};
  CHECK(game::substitute("100% sure", args) == "100% sure");
  CHECK(game::substitute("%s0 %sa %s", args) == "%s0 %sa %s");
}

TEST(localization_splits_one_comma_separated_token) {
  std::string token;
  std::string tail;
  game::split_token("numbertotrain, maxnumber, class", token, tail);
  CHECK(token == "numbertotrain");
  CHECK(tail == " maxnumber, class");

  game::split_token("only", token, tail);
  CHECK(token == "only");
  CHECK(tail.empty());
}

TEST(localization_trims_each_token) {
  // `CONST.INI` writes `TributeTimes =  0,   10,   20,   30` and the corpus
  // feeds the tokens straight to `Str2Int`; `BARRACK_TRAIN_EX.VS` compares one
  // against `"elephant"` with no trimming of its own. Both need this.
  std::string source = " 0,   10,   20,   30";
  std::string token;
  std::vector<std::string> tokens;
  while (!source.empty()) {
    game::split_token(source, token, source);
    tokens.push_back(token);
  }
  REQUIRE(tokens.size() == 4);
  CHECK(tokens[0] == "0");
  CHECK(tokens[1] == "10");
  CHECK(tokens[3] == "30");
}

/// `ParseStr`'s own rule, 0x00697580: spaces, and only spaces, are cut from
/// the token's two ends; the tail after the comma is kept verbatim.
TEST(localization_split_trims_spaces_only_and_keeps_the_tail_verbatim) {
  std::string token;
  std::string tail;
  game::split_token("  a b  ,  rest , more  ", token, tail);
  CHECK(token == "a b");
  CHECK(tail == "  rest , more  ");

  // A tab is not a space to it.
  game::split_token("\tx\t, y", token, tail);
  CHECK(token == "\tx\t");
  CHECK(tail == " y");

  // A tail of nothing but spaces stays a non-empty string, so a script's
  // `while (dest != "")` runs once more and parses an empty token.
  game::split_token("a, ", token, tail);
  CHECK(token == "a");
  CHECK(tail == " ");
  game::split_token(tail, token, tail);
  CHECK(token.empty());
  CHECK(tail.empty());
}

TEST(localization_splits_safely_when_the_token_and_tail_alias_the_source) {
  // Every one of the 71 `ParseStr` call sites is `ParseStr(dest, dest)`.
  std::string source = "a,b,c";
  std::string token;
  game::split_token(source, token, source);
  CHECK(token == "a");
  CHECK(source == "b,c");
}

TEST(text_host_translates_and_falls_back_without_a_table) {
  const script::HostRegistry registry = text_registry();

  HostCall bare{script::Value::string("Out of the game")};
  const auto without = invoke(registry, "Translate", 1, bare);
  CHECK(without.status == script::HostStatus::ok);
  // A null table is a missing language pack, not an error.
  CHECK(without.value.as_string() == "Out of the game");

  const auto parsed = game::TranslationTable::parse(bytes_of(kTable));
  REQUIRE(parsed.ok());
  HostCall loaded{script::Value::string("Out of the game")};
  loaded.state.translations = &parsed.value();
  const auto with = invoke(registry, "Translate", 1, loaded);
  CHECK(with.value.as_string() == "Fuori dal gioco");
}

TEST(text_host_translates_then_substitutes) {
  const auto parsed = game::TranslationTable::parse(bytes_of(kTable));
  REQUIRE(parsed.ok());

  const script::HostRegistry registry = text_registry();
  HostCall call{script::Value::string("Needs %s1"), script::Value::integer(500)};
  call.state.translations = &parsed.value();
  const auto out = invoke(registry, "Translatef", 2, call);
  // The integer prints: the corpus passes integers into `%s` slots.
  CHECK(out.value.as_string() == "Serve 500");
}

/// A table shaped to tell the original's lookup (0x00689130 over the maps
/// 0x00689f40 fills) from a plain `text` search. Invented words throughout.
constexpr std::string_view kLookupTable =
    "<translationtable>"
    "<translationtableentry text=\"Quill\" justtext=\"Quill\" result=\"Penna\"/>"
    "<translationtableentry text=\"Quill@inkwell\" justtext=\"Quill\" result=\"Piuma\"/>"
    "<translationtableentry text=\"Lantern\" justtext=\"Lantern\" result=\"Lanterna\"/>"
    "<translationtableentry text=\"Hollow\" justtext=\"Hollow\" result=\"\"/>"
    "<translationtableentry text=\"Owes %s1 to %s2\" justtext=\"Owes %s1 to %s2\" "
    "result=\"Deve %s1 a %s2\"/>"
    "<translationtableentry text=\"Gives %s2 then %s1\" justtext=\"Gives %s2 then %s1\" "
    "result=\"%s2, poi %s1\"/>"
    "<translationtableentry text=\"Odd %s1\" justtext=\"Odd %s1\" result=\"Strano %d1\"/>"
    "<translationtableentry text=\"Half %s1\" justtext=\"Half %s1\" result=\"%s1 al 50%%\"/>"
    "<translationtableentry text=\"Broken %s1\" justtext=\"Broken %s1\" result=\"50% di %s1\"/>"
    "<translationtableentry text=\"Bare %s1\" justtext=\"Bare %s1\" result=\"Nudo %s\"/>"
    "<translationtableentry text=\"Echo\" justtext=\"Echo\" result=\"Eco\"/>"
    "<translationtableentry text=\"Say %s1\" justtext=\"Say %s1\" result=\"%s1\"/>"
    "</translationtable>";

TEST(localization_lookup_follows_the_originals_maps) {
  const auto parsed = game::TranslationTable::parse(bytes_of(kLookupTable));
  REQUIRE(parsed.ok());
  const game::TranslationTable& table = parsed.value();

  // `justtext` is a name too, and a later row overwrites an earlier one under
  // the same name: `Quill@inkwell`'s `justtext` comes after `Quill`.
  CHECK(table.lookup("Quill") == "Piuma");
  CHECK(table.translate("Quill") == "Penna");  // the interface's search is unchanged
  CHECK(table.lookup("Quill@inkwell") == "Piuma");

  // Case-blind when the exact name misses.
  CHECK(table.lookup("LANTERN") == "Lanterna");
  CHECK(table.lookup("lantern") == "Lanterna");

  // Empty answers empty; digits answer themselves; a miss and an empty result
  // answer the key.
  CHECK(table.lookup("").empty());
  CHECK(table.lookup("0420") == "0420");
  CHECK(table.lookup("nowhere") == "nowhere");
  CHECK(table.lookup("Hollow") == "Hollow");
  CHECK(table.lookup("HOLLOW") == "HOLLOW");

  // A context is tried first and dropped when it misses.
  CHECK(table.lookup("Quill", "inkwell") == "Piuma");
  CHECK(table.lookup("Lantern", "inkwell") == "Lanterna");
}

TEST(localization_format_translates_its_arguments) {
  const auto parsed = game::TranslationTable::parse(bytes_of(kLookupTable));
  REQUIRE(parsed.ok());
  const game::TranslationTable& table = parsed.value();
  const auto fmt = [&](std::string_view key, std::vector<std::string> args) {
    return table.format(key, args);
  };

  // `%s` arguments go through the lookup themselves (0x00689a82); a number
  // passes through on the digit rule.
  CHECK(fmt("Owes %s1 to %s2", {"12", "lantern"}) == "Deve 12 a Lanterna");
  CHECK(fmt("Gives %s2 then %s1", {"Quill", "Echo"}) == "Eco, poi Piuma");
  // `%d` writes the argument as it came.
  CHECK(fmt("Odd %s1", {"Echo"}) == "Strano Echo");
  // `%%` is one percent sign.
  CHECK(fmt("Half %s1", {"7"}) == "7 al 50%");
  // Any other `%`, or a placeholder without a number, abandons the text.
  CHECK(fmt("Broken %s1", {"7"}) == "Bad parameter at pos 4");
  CHECK(fmt("Bare %s1", {"7"}) == "Bad parameter at pos 7");
  // A finished text that is itself a name is translated once more.
  CHECK(fmt("Say %s1", {"Echo"}) == "Eco");
  CHECK(fmt("Say %s1", {"Lantern"}) == "Lanterna");
}

TEST(text_host_translatef_has_three_argument_forms) {
  const auto parsed = game::TranslationTable::parse(bytes_of(kLookupTable));
  REQUIRE(parsed.ok());
  const script::HostRegistry registry = text_registry();
  // 0x004ca470 takes the key and three strings.
  HostCall call{script::Value::string("Owes %s1 to %s2"), script::Value::string("Echo"),
                script::Value::string("Quill"), script::Value::string("unused")};
  call.state.translations = &parsed.value();
  const auto out = invoke(registry, "Translatef", 4, call);
  REQUIRE(out.status == script::HostStatus::ok);
  CHECK(out.value.as_string() == "Deve Eco a Piuma");

  HostCall single{script::Value::string("LANTERN")};
  single.state.translations = &parsed.value();
  CHECK(invoke(registry, "Translate", 1, single).value.as_string() == "Lanterna");
}

TEST(text_host_parse_str_writes_the_tail_back_through_its_second_argument) {
  const script::HostRegistry registry = text_registry();
  HostCall call{script::Value::string("3, elephant, 5"), script::Value::string("")};
  const auto out = invoke(registry, "ParseStr", 2, call);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.as_string() == "3");
  // Argument 1 is an out-parameter; the VM copies assignable arguments back.
  // The tail keeps the space after the comma (0x00697629).
  CHECK(call.context.arg(1).as_string() == " elephant, 5");
}

TEST(text_host_converts_and_measures_strings) {
  const script::HostRegistry registry = text_registry();

  HostCall plain{script::Value::string("2000")};
  CHECK(invoke(registry, "Str2Int", 1, plain).value.as_integer() == 2000);

  HostCall negative{script::Value::string("-42")};
  CHECK(invoke(registry, "Str2Int", 1, negative).value.as_integer() == -42);

  HostCall empty{script::Value::string("")};
  CHECK(invoke(registry, "Str2Int", 1, empty).value.as_integer() == 0);

  // Best-effort, unlike `formats/ini.hpp`'s `parse_int`, which refuses a
  // partial parse. The two answer different questions and the header says so;
  // no shipped call site distinguishes them.
  HostCall trailing{script::Value::string("12abc")};
  CHECK(invoke(registry, "Str2Int", 1, trailing).value.as_integer() == 12);

  // `atol` (0x00763919): leading white space is skipped, tab included, and
  // the 32-bit total wraps rather than saturating. 99,999,999,999 is
  // 0x174876E7FF; its low 32 bits are 0x4876E7FF.
  HostCall spaced{script::Value::string(" \t 30")};
  CHECK(invoke(registry, "Str2Int", 1, spaced).value.as_integer() == 30);
  HostCall huge{script::Value::string("99999999999")};
  CHECK(invoke(registry, "Str2Int", 1, huge).value.as_integer() == 0x4876E7FF);
  HostCall signed_space{script::Value::string("- 5")};
  CHECK(invoke(registry, "Str2Int", 1, signed_space).value.as_integer() == 0);

  HostCall length{script::Value::string("abcde")};
  CHECK(invoke(registry, "StrLen", 1, length).value.as_integer() == 5);
}

/// `StrMid`: a substring whose length is capped at what is left and floored at
/// zero, with a start past the end answering empty and one before it read as
/// the beginning.
TEST(text_host_strmid_caps_the_length_at_what_is_left) {
  const script::HostRegistry registry = text_registry();
  const auto mid = [&](const char* text, std::int32_t start, std::int32_t length) {
    HostCall call{script::Value::string(text), script::Value::integer(start),
                  script::Value::integer(length)};
    const auto out = invoke(registry, "StrMid", 3, call);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_string() ? out.value.as_string() : std::string("<not a string>");
  };
  CHECK(mid("abcdef", 2, 3) == "cde");
  CHECK(mid("abcdef", 2, 10) == "cdef");   // capped at what is left
  CHECK(mid("abcdef", 0, 6) == "abcdef");
  CHECK(mid("abcdef", 6, 1).empty());     // exactly at the end
  CHECK(mid("abcdef", 9, 1).empty());     // past it
  CHECK(mid("abcdef", 2, 0).empty());
  CHECK(mid("abcdef", 2, -4).empty());    // floored at zero
  CHECK(mid("abcdef", -2, 3) == "abc");   // before the beginning reads as 0
  CHECK(mid("", 0, 3).empty());

  HostCall untyped{script::Value::string("abc"), script::Value::string("1"),
                   script::Value::integer(1)};
  CHECK(invoke(registry, "StrMid", 3, untyped).status == script::HostStatus::error);
}

/// `Breakpoint` is a no-op in the shipped executable.
TEST(text_host_breakpoint_does_nothing) {
  const script::HostRegistry registry = text_registry();
  HostCall call{};
  const auto out = invoke(registry, "Breakpoint", 0, call);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.is_nil());
}

TEST(text_host_sends_pr_to_the_sink_and_survives_without_one) {
  const script::HostRegistry registry = text_registry();
  RecordingSink sink;

  HostCall quiet{script::Value::string("nowhere")};
  CHECK(invoke(registry, "pr", 1, quiet).status == script::HostStatus::ok);

  HostCall loud{script::Value::string("hello")};
  loud.state.debug = &sink;
  CHECK(invoke(registry, "pr", 1, loud).status == script::HostStatus::ok);
  REQUIRE(sink.lines.size() == 1);
  CHECK(sink.lines[0] == "hello");

  HostCall number{script::Value::integer(7)};
  number.state.debug = &sink;
  (void)invoke(registry, "pr", 1, number);
  REQUIRE(sink.lines.size() == 2);
  CHECK(sink.lines[1] == "7");

  // `ClearDebug` (0x005a7fd0) moves the per-object debug era and leaves the
  // console alone; clearing the console is `cls`'s.
  HostCall clear{};
  clear.state.debug = &sink;
  CHECK(invoke(registry, "ClearDebug", 0, clear).status == script::HostStatus::ok);
  CHECK(sink.lines.size() == 2);
}

TEST(text_host_refuses_every_entry_point_with_no_context) {
  const script::HostRegistry registry = text_registry();
  const struct {
    const char* name;
    std::uint16_t arity;
  } kEntries[] = {{"pr", 1},         {"ClearDebug", 0}, {"Translate", 1},  {"Translatef", 2},
                  {"Translatef", 3}, {"ParseStr", 2},   {"Str2Int", 1},    {"StrLen", 1},
                  {"StrMid", 3},     {"Breakpoint", 0}};

  for (const auto& entry : kEntries) {
    HostCall call{script::Value::string("x"), script::Value::string("y"),
                  script::Value::string("z")};
    call.context.user = nullptr;
    const auto out = invoke(registry, entry.name, entry.arity, call);
    CHECK(out.status == script::HostStatus::error);
  }
}

TEST(text_host_does_not_claim_ss_str) {
  // `SS_STR` maps a squad-state number back to its name, and the names are data
  // in the AI profile that no `HostContext` carries yet. An entry point that
  // returned a plausible string would be the silent divergence the
  // unimplemented-entry trap exists to prevent.
  const script::HostRegistry registry = text_registry();
  const std::uint32_t index =
      registry.find(script::CallKind::free_function, "SS_STR", 1);
  REQUIRE(index != script::kUnresolvedHost);
  CHECK(registry.entry(index).fn == nullptr);
}
