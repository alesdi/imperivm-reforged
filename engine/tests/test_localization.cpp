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
  CHECK(tail == "maxnumber, class");

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

TEST(text_host_parse_str_writes_the_tail_back_through_its_second_argument) {
  const script::HostRegistry registry = text_registry();
  HostCall call{script::Value::string("3, elephant, 5"), script::Value::string("")};
  const auto out = invoke(registry, "ParseStr", 2, call);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.as_string() == "3");
  // Argument 1 is an out-parameter; the VM copies assignable arguments back.
  CHECK(call.context.arg(1).as_string() == "elephant, 5");
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

  // Saturates rather than wrapping: a wrapped value is a number the simulation
  // would go on to use.
  HostCall huge{script::Value::string("99999999999")};
  CHECK(invoke(registry, "Str2Int", 1, huge).value.as_integer() == 2147483647);

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

  HostCall clear{};
  clear.state.debug = &sink;
  CHECK(invoke(registry, "ClearDebug", 0, clear).status == script::HostStatus::ok);
  CHECK(sink.lines.empty());
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
