// The conversation catalogue: reading a `.conv.xml`.
//
// The claims about the *shipped* 110 documents -- that they all parse, that
// only three of the seven modes occur, that 98 of them end by running off a
// `first` -- are in `tests/test_corpus_conversations.py`, which drives
// `imcheck conv` over the installation. `docs/legal.md` rule 1 is why they are
// not here as fixtures.
//
// What is here is the reader, against documents written from
// `docs/formats/conv-xml.md` and chosen to break it. Four things this file is
// built to catch, each of which a plausible reader gets wrong:
//
//   1. **`\l`, `\g` and `\a` are not C escapes.** They are `<`, `>` and `&`,
//      and they exist because the attribute is XML and a program is not. A
//      reader that passed them through hands the compiler `i \l ol.count` and
//      gets a syntax error in a mission that used to work.
//
//   2. **The display strings are *not* decoded and the source strings are.**
//      Same document, same escape, two answers, and the asymmetry is
//      deliberate: `NoteDefinition::text` makes the same call.
//
//   3. **An unknown mode is refused, not defaulted.** Reading one as `first`
//      is a conversation that plays its opening line for ever with no
//      diagnostic anywhere -- this project's signature failure, again.
//
//   4. **A trailing `;` in a phrase list is a separator, not an empty label.**
//      `"begining;"` is one label. Four of the 23 shipped `followup_phrases`
//      attributes are written that way, and an empty label matches no phrase.

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/conversation.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "domains.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A linear conversation: two actors, four phrases, the last one running off
/// the end of the document rather than saying `end`, which is the shape 98 of
/// the 110 shipped documents have.
constexpr std::string_view kLinear = R"(<conversation
		name="T_Meeting"
		startup="first"
		restore_view="1">
		<actor name="Scout"/>
		<actor name="Captain"/>
		<phrase
			label="open"
			actor="Scout"
			text="The pass is held.\nThey have towers on both sides."
			followup="first"/>
		<phrase
			actor="Captain"
			text="Then we go around."
			condition="EnvReadInt(\'/found_pass\') == 1"
			action="EnvWriteInt(\'/going_around\', 1);"
			followup="first"/>
		<phrase
			actor="Captain"
			text="Then we go through."
			condition="if (GetSettlement(\'S_Camp\').gold \g= 2000)\n return true;\nreturn false;"
			followup="first"/>
		<phrase
			label="last"
			actor="Scout"
			text="At once."
			followup="first"/>
	</conversation>
)";

/// A menu: a prompt whose `followup` is `choice` with no list, three options
/// carrying `choice_text`, and a `return` on each -- the shape a `ConvResult`
/// caller needs.
constexpr std::string_view kMenu = R"(<conversation
		name="T_Bargain"
		startup="first"
		startup_phrases="ask;repeat"
		restore_view="0">
		<actor name="Trader"/>
		<phrase label="ask" actor="Trader" text="What will it be?" followup="choice"/>
		<phrase label="repeat" actor="Trader" text="Again?" followup="choice"
			followup_phrases="spear; shield ;"/>
		<phrase actor="Trader" text="Very well." choice_text="A spear"
			return="return \'Spear\';" followup="end" label="spear"/>
		<phrase actor="Trader" text="Very well." choice_text="A shield"
			return="\'Shield\'" followup="end" label="shield"/>
		<phrase actor="Trader" text="As you wish." choice_text="Nothing"
			followup="end"/>
	</conversation>
)";

}  // namespace

// --------------------------------------------------------------------------
// modes
// --------------------------------------------------------------------------

TEST(conversation_mode_names_are_the_executables_seven) {
  // The numbering is `gbr.exe`'s own, from the mapper at 0x005067d0. It is
  // pinned because `docs/formats/save.md` will need it the day a conversation's
  // state is serialised, and because the order is not alphabetical, not the
  // document's, and not guessable.
  const std::pair<std::string_view, ConversationMode> table[] = {
      {"choice", ConversationMode::choice},
      {"random", ConversationMode::random},
      {"first", ConversationMode::first},
      {"end", ConversationMode::end},
      {"cycle", ConversationMode::cycle},
      {"cycle then first", ConversationMode::cycle_then_first},
      {"cycle then random", ConversationMode::cycle_then_random},
  };
  for (const auto& [name, mode] : table) {
    const Result<ConversationMode> parsed = parse_conversation_mode(name);
    REQUIRE(parsed.ok());
    CHECK(parsed.value() == mode);
    CHECK(conversation_mode_name(mode) == name);
    CHECK(static_cast<int>(mode) == static_cast<int>(parsed.value()));
  }
  CHECK(static_cast<int>(ConversationMode::choice) == 0);
  CHECK(static_cast<int>(ConversationMode::first) == 2);
}

TEST(conversation_an_empty_mode_is_first_and_an_unknown_one_is_refused) {
  // The executable tests for the empty string explicitly, between `first` and
  // `cycle`, and answers `first`. An absent attribute arrives here as empty.
  const Result<ConversationMode> empty = parse_conversation_mode("");
  REQUIRE(empty.ok());
  CHECK(empty.value() == ConversationMode::first);

  // Everything else is refused. Defaulting an unknown mode to `first` would
  // play the same line for ever and say nothing -- and there is an `!error!`
  // beside the seven names in the executable's own table, so refusing is what
  // the original does too.
  CHECK(!parse_conversation_mode("First").ok());   // case-sensitive
  CHECK(!parse_conversation_mode("cycle then").ok());
  CHECK(!parse_conversation_mode("shuffle").ok());
  CHECK(!parse_conversation_mode(" first").ok());  // not trimmed
}

// --------------------------------------------------------------------------
// escapes
// --------------------------------------------------------------------------

TEST(conversation_escapes_are_the_five_the_authoring_tool_writes) {
  CHECK(decode_conversation_escapes("a\\nb") == "a\nb");
  CHECK(decode_conversation_escapes("\\'x\\'") == "'x'");
  // The three that exist because the attribute is XML.
  CHECK(decode_conversation_escapes("i \\l 5") == "i < 5");
  CHECK(decode_conversation_escapes("g \\g= 4000") == "g >= 4000");
  CHECK(decode_conversation_escapes("a\\a\\ab") == "a&&b");
  CHECK(decode_conversation_escapes("") == "");
  CHECK(decode_conversation_escapes("plain") == "plain");
}

TEST(conversation_an_unknown_escape_keeps_its_backslash) {
  // `\D` in a path is a likelier reading than an escape this reader has not
  // heard of, and swallowing the separator is the worse mistake of the two.
  CHECK(decode_conversation_escapes("movies\\Dintro.avi") == "movies\\Dintro.avi");
  // A trailing backslash is kept rather than reading past the end.
  CHECK(decode_conversation_escapes("end\\") == "end\\");
}

// --------------------------------------------------------------------------
// snippets
// --------------------------------------------------------------------------

TEST(conversation_a_snippet_is_wrapped_by_its_grammar_and_not_by_its_attribute) {
  // A bare expression gets a `return`; a statement list does not. Nothing but
  // the grammar tells them apart, and both shapes occur in the same attribute
  // name in the same installation.
  ConversationSnippet bare{"EnvReadInt('/x') == 1"};
  const std::string wrapped = bare.wrap("bool");
  CHECK(wrapped.find("return EnvReadInt('/x') == 1;") != std::string::npos);
  CHECK(wrapped.rfind("//bool", 0) == 0);

  ConversationSnippet body{"if (EnvReadInt('/x')==1)\n return true;\nreturn false;"};
  const std::string kept = body.wrap("bool");
  CHECK(kept.find("return return") == std::string::npos);
  CHECK(kept.find("if (EnvReadInt('/x')==1)") != std::string::npos);

  // A body with no semicolon at all, told apart by its keyword.
  ConversationSnippet loose{"if (a) return true"};
  CHECK(loose.wrap("bool").find("return if") == std::string::npos);

  // An empty snippet is a signature and nothing else, so a caller can compile
  // it without a special case.
  ConversationSnippet none;
  CHECK(none.empty());
  CHECK(none.wrap("void") == "//void\n");
}

// --------------------------------------------------------------------------
// phrase lists
// --------------------------------------------------------------------------

TEST(conversation_a_trailing_separator_does_not_make_an_empty_label) {
  const std::vector<std::string> one = split_phrase_list("begining;");
  REQUIRE(one.size() == 1);
  CHECK(one[0] == "begining");

  // Spaces around a field belong to the separator, not to the label.
  const std::vector<std::string> spaced = split_phrase_list("spear; shield ;");
  REQUIRE(spaced.size() == 2);
  CHECK(spaced[0] == "spear");
  CHECK(spaced[1] == "shield");

  CHECK(split_phrase_list("").empty());
  CHECK(split_phrase_list(";;;").empty());
  REQUIRE(split_phrase_list("1;2;3").size() == 3);
}

// --------------------------------------------------------------------------
// documents
// --------------------------------------------------------------------------

TEST(conversation_catalogue_reads_a_linear_document) {
  ConversationCatalogue catalogue;
  const Result<std::size_t> added = catalogue.add(bytes_of(kLinear));
  REQUIRE(added.ok());
  CHECK(added.value() == 1);
  REQUIRE(catalogue.size() == 1);

  const ConversationDefinition* conversation = catalogue.find("T_Meeting");
  REQUIRE(conversation != nullptr);
  CHECK(conversation->startup == ConversationMode::first);
  CHECK(conversation->startup_phrases.empty());
  CHECK(conversation->restore_view);

  REQUIRE(conversation->actors.size() == 2);
  CHECK(conversation->actors[0] == "Scout");
  CHECK(conversation->actors[1] == "Captain");

  REQUIRE(conversation->phrases.size() == 4);
  // Document order, which is what every default candidate list walks.
  CHECK(conversation->phrases[0].label == "open");
  CHECK(conversation->phrases[3].label == "last");
  CHECK(conversation->find("open") == 0);
  CHECK(conversation->find("last") == 3);
  CHECK(conversation->find("Open") < 0);  // case-sensitive, as every id here is
  CHECK(conversation->find("") < 0);

  // The last phrase says `first` and there is nothing after it. That is not a
  // malformed document; it is how 98 of the 110 shipped ones end.
  CHECK(conversation->phrases[3].followup == ConversationMode::first);
  CHECK(conversation->phrases[3].followup_phrases.empty());
}

TEST(conversation_display_text_keeps_its_escapes_and_source_does_not) {
  ConversationCatalogue catalogue;
  REQUIRE(catalogue.add(bytes_of(kLinear)).ok());
  const ConversationDefinition* conversation = catalogue.find("T_Meeting");
  REQUIRE(conversation != nullptr);

  // Same escape, same document, two answers -- and the asymmetry is the point.
  // A line break in a display string is the interface layer's business.
  CHECK(conversation->phrases[0].text.find("\\n") != std::string::npos);
  CHECK(conversation->phrases[0].text.find('\n') == std::string::npos);

  // The source strings are decoded, because a compiler cannot read `\'`.
  CHECK(conversation->phrases[1].condition.source == "EnvReadInt('/found_pass') == 1");
  CHECK(conversation->phrases[1].action.source == "EnvWriteInt('/going_around', 1);");

  // And `\g` really is `>`: this is the condition that would be a syntax error
  // under a reader that passed the escape through.
  const std::string& gold = conversation->phrases[2].condition.source;
  CHECK(gold.find(">=") != std::string::npos);
  CHECK(gold.find("\\g") == std::string::npos);
  CHECK(gold.find('\n') != std::string::npos);  // `\n` decoded here, unlike in `text`
}

TEST(conversation_catalogue_reads_a_menu_and_its_returns) {
  ConversationCatalogue catalogue;
  REQUIRE(catalogue.add(bytes_of(kMenu)).ok());
  const ConversationDefinition* conversation = catalogue.find("T_Bargain");
  REQUIRE(conversation != nullptr);

  CHECK(!conversation->restore_view);
  REQUIRE(conversation->startup_phrases.size() == 2);
  CHECK(conversation->startup_phrases[0] == "ask");
  CHECK(conversation->startup_phrases[1] == "repeat");

  REQUIRE(conversation->phrases.size() == 5);
  CHECK(conversation->phrases[0].followup == ConversationMode::choice);
  // A `choice` with no list of its own; the candidates are what follows it.
  CHECK(conversation->phrases[0].followup_phrases.empty());
  // And one with a list, written with the spacing the authoring tool uses.
  REQUIRE(conversation->phrases[1].followup_phrases.size() == 2);
  CHECK(conversation->phrases[1].followup_phrases[1] == "shield");

  // Both `return` shapes: a statement and a bare expression.
  CHECK(conversation->phrases[2].result.source == "return 'Spear';");
  CHECK(conversation->phrases[3].result.source == "'Shield'");
  CHECK(conversation->phrases[3].result.wrap("str").find("return 'Shield';") != std::string::npos);

  // The option with no `return` leaves the result alone, which is what a
  // `ConvResult` caller sees as "unchanged" rather than as an empty string.
  CHECK(conversation->phrases[4].result.empty());
  CHECK(conversation->phrases[4].choice_text == "Nothing");
}

TEST(conversation_catalogue_keeps_declaration_order_and_the_first_of_a_name) {
  ConversationCatalogue catalogue;
  REQUIRE(catalogue.add(bytes_of(kLinear)).ok());
  REQUIRE(catalogue.add(bytes_of(kMenu)).ok());
  REQUIRE(catalogue.size() == 2);
  CHECK(catalogue.all()[0].name == "T_Meeting");
  CHECK(catalogue.all()[1].name == "T_Bargain");

  // A repeated name is skipped rather than replacing what is there, which is
  // what a `std::map` insert does with a key it already holds -- and the count
  // returned says nothing was added, so a caller can notice.
  const Result<std::size_t> again = catalogue.add(bytes_of(kMenu));
  REQUIRE(again.ok());
  CHECK(again.value() == 0);
  CHECK(catalogue.size() == 2);

  CHECK(catalogue.find("T_Nothing") == nullptr);
  catalogue.clear();
  CHECK(catalogue.empty());
}

TEST(conversation_an_empty_document_adds_nothing_and_is_not_an_error) {
  // Six of the twenty-four containers declare no conversations at all, and a
  // session that refused to load one of them would refuse a quarter of the
  // installation.
  ConversationCatalogue catalogue;
  const Result<std::size_t> added = catalogue.add({});
  REQUIRE(added.ok());
  CHECK(added.value() == 0);
  CHECK(catalogue.empty());
}

TEST(conversation_catalogue_refuses_documents_it_does_not_understand) {
  ConversationCatalogue catalogue;
  // Not XML at all.
  CHECK(!catalogue.add(bytes_of("<conversation")).ok());

  // A conversation with no name can be called by nothing, so it is skipped
  // rather than stored -- and skipping is not refusing, because a document
  // that also holds a good one must still yield it.
  ConversationCatalogue nameless;
  const Result<std::size_t> skipped = nameless.add(bytes_of("<conversation startup=\"first\"/>"));
  REQUIRE(skipped.ok());
  CHECK(skipped.value() == 0);

  // An unknown mode is a refusal, at either level.
  CHECK(!catalogue.add(bytes_of("<conversation name=\"x\" startup=\"shuffle\"/>")).ok());
  CHECK(!catalogue
             .add(bytes_of("<conversation name=\"x\"><phrase followup=\"loop\"/></conversation>"))
             .ok());

  // A repeated label inside one conversation would make `followup_phrases`
  // ambiguous. All 110 shipped documents have unique labels, so a document
  // where they are not is one this reader does not understand.
  CHECK(!catalogue
             .add(bytes_of("<conversation name=\"y\"><phrase label=\"a\"/>"
                           "<phrase label=\"a\"/></conversation>"))
             .ok());

  // Two phrases with *no* label are fine: 194 of the 260 shipped ones have
  // none, and they are reached by position rather than by name.
  ConversationCatalogue unlabelled;
  REQUIRE(unlabelled
              .add(bytes_of("<conversation name=\"z\"><phrase/><phrase/></conversation>"))
              .ok());
  REQUIRE(unlabelled.find("z") != nullptr);
  CHECK(unlabelled.find("z")->phrases.size() == 2);
}

TEST(conversation_a_wrapper_element_reads_the_same_as_a_bare_one) {
  // Every shipped document is a bare `<conversation>`, but the element is
  // looked for rather than assumed: a reader that insisted on the bare form
  // would refuse a whole container over one document that grew a root.
  ConversationCatalogue catalogue;
  const std::string wrapped =
      std::string("<conversations>") + std::string(kLinear) + "</conversations>";
  const Result<std::size_t> added = catalogue.add(bytes_of(wrapped));
  REQUIRE(added.ok());
  CHECK(added.value() == 1);
  REQUIRE(catalogue.find("T_Meeting") != nullptr);
  CHECK(catalogue.find("T_Meeting")->phrases.size() == 4);
}


// ==========================================================================
// the runtime
// ==========================================================================

namespace {

/// A conversation whose three phrases are gated on an environment flag, so that
/// what plays depends on the world and can be steered from a test.
constexpr std::string_view kGated = R"(<conversation name="T_Gate" startup="first">
		<actor name="Herald"/>
		<phrase actor="Herald" text="The gate is shut."
			condition="EnvReadInt(\'/open\') == 0"
			action="EnvWriteInt(\'/knocked\', 1);"
			return="\'SHUT\'"
			followup="end"/>
		<phrase actor="Herald" text="The gate is open."
			condition="EnvReadInt(\'/open\') == 1"
			action="EnvWriteInt(\'/entered\', 1);"
			return="return \'OPEN\';"
			followup="end"/>
		<phrase actor="Herald" text="Nobody is here." followup="end"/>
	</conversation>
)";

/// Everything an entry point needs: a world with a campaign and an environment
/// store, the full registry, and a scheduler to run a script through.
struct Stage {
  World world;
  CampaignSystem campaign;
  EnvSystem env;
  script::HostRegistry registry;
  WorldHost host{world};
  script::Scheduler scheduler;
  HostContext context;

  Stage() {
    world.seed(7);
    world.add_system(&env);
    world.add_system(&campaign);
    (void)register_all_hosts(registry);
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    context.world = &world;
    context.object_type = kTypeObj;
    scheduler.set_user(&context);
    install_objlist_lifetime(scheduler);
  }

  /// Compile and run one script to completion, and say whether it did.
  ///
  /// `owner` is what `This` resolves to, which the `SetActor` idiom needs: the
  /// shipped form is `SetActor("Scipio", GetNamedObj("NO_Scipio").obj.AsUnit())`
  /// and the object is whatever the script can reach.
  bool run(std::string_view source, const char* name, ObjectId owner = kNoObject,
           bool expect_trap = false) {
    script::Diagnostic diagnostic;
    const auto parsed = script::parse(bytes_of(source), name, &diagnostic);
    if (!parsed.ok()) {
      std::printf("  parse %s:%u: %.*s\n", name, diagnostic.line,
                  static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
      return false;
    }
    script::CompileError error;
    auto chunk = script::compile(parsed.value(), &registry, &error);
    if (!chunk.ok()) {
      std::printf("  compile %s:%u: %s\n", name, error.line, error.message.c_str());
      return false;
    }
    const std::uint32_t index = scheduler.add_chunk(std::move(chunk.value()));
    if (scheduler.spawn(index, {}, script::ObjectRef{kTypeObj, owner}) ==
        script::kNoScript) {
      return false;
    }
    for (int pass = 0; pass < 8 && scheduler.live_count() > 0; ++pass) {
      const script::RunReport report = scheduler.advance(1000);
      for (const script::FailedScript& trap : report.traps) {
        // Printed only when it is a surprise. A test that asserts a trap does
        // not want its own expected diagnostic in a clean run's output.
        if (!expect_trap) {
          std::printf("  trap %s:%u: %s\n", trap.source_name.c_str(), trap.trap.line,
                      trap.trap.detail.c_str());
        }
        return false;
      }
    }
    return scheduler.live_count() == 0;
  }
};

}  // namespace

TEST(conversation_takes_no_entry_point_from_another_domain) {
  // `Init` and `Run` are as generic as a member name gets, and `define`
  // replaces silently. This domain registers last, so a name it claims that
  // somebody above already implemented would disappear without a diagnostic --
  // and both domains' own tests would keep passing.
  script::HostRegistry busy;
  imperivm::test::define_all_except("conversation", busy);

  const std::pair<std::string_view, std::uint16_t> claimed[] = {
      {"RunConv", 1}, {"ConvResult", 1},
  };
  for (const auto& [name, arity] : claimed) {
    const std::uint32_t index = busy.find(script::CallKind::free_function, name, arity);
    // Declared but unimplemented, which is the whole point: nobody else may
    // have written a body for one of these.
    if (index != script::kUnresolvedHost) CHECK(busy.entry(index).fn == nullptr);
  }
  const std::pair<std::string_view, std::uint16_t> members[] = {
      {"Init", 1}, {"SetActor", 2}, {"Run", 0},
  };
  for (const auto& [name, arity] : members) {
    const std::uint32_t index = busy.find(script::CallKind::member, name, arity);
    if (index != script::kUnresolvedHost) CHECK(busy.entry(index).fn == nullptr);
  }

  script::HostRegistry mine;
  CHECK(register_conversation_host(mine) == conversation_host_entry_count());
  CHECK(conversation_host_entry_count() == 5);
}

TEST(conversation_a_declared_local_gets_a_pooled_handle) {
  Stage stage;
  // `Conversation C_Conv;` is a declaration like `ObjList ol;`: the host mints
  // the handle at the declaration site, so a script that declares one inside a
  // loop body reuses one entry rather than growing the pool per iteration.
  CHECK(stage.run("//void\nConversation a;\nConversation b;\n", "decl.vs"));
  CHECK(stage.world.conversations().capacity() == 2);

  // And the script's entries go away with it, which is the teardown hook.
  CHECK(stage.world.conversations().owner_of(1) == script::kNoScript);
  CHECK(!stage.world.conversations().contains(1));
}

TEST(conversation_init_set_actor_and_run_play_the_eligible_phrase) {
  Stage stage;
  REQUIRE(stage.campaign.conversations().add(bytes_of(kGated)).ok());
  const ObjectId herald = stage.world.spawn(NativeClass::decor, nullptr);
  CHECK(stage.world.named_objects().bind("NO_Herald", herald));

  // The shipped idiom, three statements long, and the one the 54 `Init` sites
  // and 63 `SetActor` sites are all written in.
  CHECK(stage.run(
      "//void\n"
      "Conversation C_Conv;\n"
      "C_Conv.Init(\"T_Gate\");\n"
      "C_Conv.SetActor(\"Herald\", GetNamedObj(\"NO_Herald\").obj);\n"
      "C_Conv.Run();\n",
      "talk.vs"));

  // The cast is bound and it survives the script, because `Run` is the last
  // statement and the pool entry outlives the call rather than the script.
  const ConversationPool& pool = stage.world.conversations();
  REQUIRE(pool.capacity() == 1);
  CHECK(pool.name_of(1).empty());  // released with the script that declared it

  // `/open` is 0, so the first phrase is the eligible one: it knocked, and it
  // left `SHUT` behind for `ConvResult`.
  //
  // The keys keep their leading slash. `sim/env.hpp` says why: the root scope
  // stores the literal it was given, so `"/knocked"` and `"knocked"` are two
  // keys, and reproducing that is what makes three of the retail conquest
  // bonuses do nothing.
  CHECK(stage.env.env().read_int(EnvScope::root(), "/knocked") == 1);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/entered") == 0);
  CHECK(stage.campaign.results().get("T_Gate") == "SHUT");
}

TEST(conversation_the_condition_decides_which_line_plays) {
  Stage stage;
  REQUIRE(stage.campaign.conversations().add(bytes_of(kGated)).ok());
  stage.env.env().write_int(EnvScope::root(), "/open", 1);

  CHECK(stage.run("//void\nRunConv(\"T_Gate\");\n", "open.vs"));

  // The *second* phrase now, because the first one's condition is false. Its
  // `return` is written as a statement rather than as a bare expression, which
  // is the other of the two shapes.
  CHECK(stage.env.env().read_int(EnvScope::root(), "/knocked") == 0);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/entered") == 1);
  CHECK(stage.campaign.results().get("T_Gate") == "OPEN");
}

TEST(conversation_run_conv_answers_the_result_and_run_does_not) {
  Stage stage;
  REQUIRE(stage.campaign.conversations().add(bytes_of(kGated)).ok());

  // `RunConv` is registered `str` and `Conversation::Run` `void`. The result
  // goes to the table either way, which is what makes `ConvResult` readable
  // after both.
  CHECK(stage.run(
      "//void\n"
      "str got;\n"
      "got = RunConv(\"T_Gate\");\n"
      "EnvWriteString(\"/got\", got);\n"
      "EnvWriteString(\"/again\", ConvResult(\"T_Gate\"));\n",
      "result.vs"));
  CHECK(stage.env.env().read_string(EnvScope::root(), "/got") == "SHUT");
  CHECK(stage.env.env().read_string(EnvScope::root(), "/again") == "SHUT");
}

TEST(conversation_an_undeclared_name_plays_nothing_and_is_not_a_trap) {
  Stage stage;
  // A conversation nothing declares is `GiveNote`'s case: the shipped data has
  // a mission naming something its own container forgot, and a trap there
  // stops the mission rather than skipping a line.
  CHECK(stage.run(
      "//void\n"
      "EnvWriteString(\"/answer\", RunConv(\"T_Nothing\"));\n",
      "missing.vs"));
  CHECK(stage.env.env().read_string(EnvScope::root(), "answer").empty());
  CHECK(stage.campaign.results().get("T_Nothing").empty());
}

TEST(conversation_run_before_init_says_nothing) {
  Stage stage;
  REQUIRE(stage.campaign.conversations().add(bytes_of(kGated)).ok());
  // Nothing is bound, so nothing is said. A script bug, not a trap.
  CHECK(stage.run("//void\nConversation C;\nC.Run();\n", "early.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/knocked") == 0);
}

TEST(conversation_a_phrase_with_no_return_leaves_the_previous_result_alone) {
  Stage stage;
  constexpr std::string_view silent =
      R"(<conversation name="T_Quiet" startup="first">
		<phrase text="..." followup="end"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(silent)).ok());
  stage.campaign.results().set("T_Quiet", "EARLIER");

  CHECK(stage.run("//void\nRunConv(\"T_Quiet\");\n", "quiet.vs"));
  // Not cleared. A conversation that ends on an option with nothing to say
  // answers whatever it answered before, which is what the four shipped
  // `return` attributes -- all of them on *some* options of a menu and not on
  // others -- depend on.
  CHECK(stage.campaign.results().get("T_Quiet") == "EARLIER");
}

TEST(conversation_a_cycle_in_the_phrase_graph_is_refused_rather_than_hung) {
  Stage stage;
  // `followup_phrases` can name a phrase that comes *before* this one -- two
  // shipped options name their own menu -- so the graph has cycles in it and a
  // run is not bounded by the phrase count. The original is bounded by the
  // player closing the window; this is bounded by a number.
  constexpr std::string_view loop =
      R"(<conversation name="T_Loop" startup="first">
		<phrase label="a" text="a" followup="first" followup_phrases="b"/>
		<phrase label="b" text="b" followup="first" followup_phrases="a"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(loop)).ok());
  // The script traps rather than the session hanging, which is the whole point:
  // a hang has no diagnostic and this has one.
  CHECK(!stage.run("//void\nRunConv(\"T_Loop\");\n", "loop.vs", kNoObject,
                   /*expect_trap=*/true));
}

TEST(conversation_results_are_state_and_survive_a_save) {
  ConversationResults results;
  CHECK(results.get("nothing").empty());
  results.set("b", "second");
  results.set("a", "first");
  results.set("c", "third");
  // Ascending by name, which is a `std::map`'s order and what `get`
  // binary-searches on.
  REQUIRE(results.size() == 3);
  CHECK(results.all()[0].name == "a");
  CHECK(results.all()[2].name == "c");
  CHECK(results.get("b") == "second");
  // Setting an existing name replaces rather than appending.
  results.set("b", "again");
  CHECK(results.size() == 3);
  CHECK(results.get("b") == "again");
  // An empty value is stored, not removed: "ran and said nothing" is not the
  // same question as "never ran".
  results.set("d", "");
  CHECK(results.size() == 4);

  std::uint64_t before = 0;
  results.hash(before);
  CHECK(before != 0);

  std::vector<std::byte> bytes;
  results.serialize(bytes);
  ConversationResults back;
  REQUIRE(back.deserialize(bytes).ok());
  CHECK(back.size() == 4);
  CHECK(back.get("b") == "again");
  std::uint64_t after = 0;
  back.hash(after);
  CHECK(after == before);

  // A truncated payload leaves the target untouched, at every cut.
  ConversationResults target;
  for (std::size_t cut = 0; cut < bytes.size(); cut += 3) {
    CHECK(!target.deserialize(std::span(bytes).first(cut)).ok());
  }
  CHECK(target.empty());
}

// --------------------------------------------------------------------------
// what the fault sweep asked for
// --------------------------------------------------------------------------
//
// Every test below exists because a deliberate defect survived the suite
// without it. That is the standard this project holds a domain to, and seven
// of the thirty-nine faults injected into this one got through the first pass.

TEST(conversation_restore_view_is_read_and_defaults_to_off) {
  // 76 of the 110 shipped documents leave it out, so the default is the answer
  // three quarters of the time. Nothing in `engine/core` reads the flag -- it
  // is the camera's -- which is exactly why an unasserted default survives:
  // getting it backwards would move the camera on 110 conversations instead of
  // 34 and no simulation test could tell.
  ConversationCatalogue catalogue;
  REQUIRE(catalogue.add(bytes_of(kLinear)).ok());   // restore_view="1"
  REQUIRE(catalogue.add(bytes_of(kMenu)).ok());     // restore_view="0"
  REQUIRE(catalogue.add(bytes_of("<conversation name=\"T_Bare\"/>")).ok());  // absent
  CHECK(catalogue.find("T_Meeting")->restore_view);
  CHECK(!catalogue.find("T_Bargain")->restore_view);
  CHECK(!catalogue.find("T_Bare")->restore_view);
}

TEST(conversation_init_clears_the_cast_it_had) {
  ConversationPool pool;
  const ConversationId id = pool.acquire(1, 0);
  CHECK(pool.bind(id, "first"));
  CHECK(pool.bind_actor(id, "A", 11));
  CHECK(pool.bind_actor(id, "B", 12));
  REQUIRE(pool.actors(id).size() == 2);

  // A script that runs six conversations from one local calls `Init` before
  // each. Carrying the cast over would leave a role bound to a unit the new
  // conversation never mentions.
  CHECK(pool.bind(id, "second"));
  CHECK(pool.name_of(id) == "second");
  CHECK(pool.actors(id).empty());

  // And so does re-acquiring the same declaration site, which is what a loop
  // body does.
  CHECK(pool.bind_actor(id, "C", 13));
  CHECK(pool.acquire(1, 0) == id);
  CHECK(pool.name_of(id).empty());
  CHECK(pool.actors(id).empty());
}

TEST(conversation_results_out_of_order_are_refused_at_the_load) {
  // `get` binary-searches, so a table that arrives unsorted answers wrongly for
  // every name after the break -- and answers *something*, which is worse than
  // answering nothing. Hand-built rather than round-tripped, because the
  // serialiser cannot produce it.
  std::vector<std::byte> bytes;
  ConversationResults source;
  source.set("a", "1");
  source.set("b", "2");
  source.serialize(bytes);
  ConversationResults ok;
  REQUIRE(ok.deserialize(bytes).ok());

  // Swap the two names in the payload. Both are one byte long and the layout is
  // (length, bytes) per string, so the two name bytes are the only difference.
  std::vector<std::byte> swapped = bytes;
  std::size_t first = 0;
  for (std::size_t i = 0; i + 1 < swapped.size(); ++i) {
    if (swapped[i] == std::byte{'a'}) {
      first = i;
      break;
    }
  }
  REQUIRE(first != 0);
  std::size_t second = 0;
  for (std::size_t i = first + 1; i < swapped.size(); ++i) {
    if (swapped[i] == std::byte{'b'}) {
      second = i;
      break;
    }
  }
  REQUIRE(second != 0);
  swapped[first] = std::byte{'b'};
  swapped[second] = std::byte{'a'};

  ConversationResults target;
  CHECK(!target.deserialize(swapped).ok());
  CHECK(target.empty());
}

TEST(conversation_a_condition_that_traps_makes_its_phrase_ineligible) {
  Stage stage;
  // The first phrase's condition calls an entry point with no body, so it
  // cannot be evaluated. **False, not true**: a conversation whose gate cannot
  // be checked should fall through to whatever comes next rather than assert a
  // branch nobody could check. The other reading would have the second phrase
  // never play.
  constexpr std::string_view trapping =
      R"(<conversation name="T_Trap" startup="first">
		<phrase text="unreachable" condition="NoSuchEntryPointAnywhere() == 1"
			action="EnvWriteInt(\'/wrong\', 1);" followup="end"/>
		<phrase text="reachable" action="EnvWriteInt(\'/right\', 1);" followup="end"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(trapping)).ok());

  CHECK(stage.run("//void\nRunConv(\"T_Trap\");\n", "trap.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/wrong") == 0);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/right") == 1);
}

TEST(conversation_an_action_that_traps_stops_the_conversation) {
  Stage stage;
  // The asymmetry with the test above, and it is deliberate. An action is what
  // a conversation *does*; one that silently did not run leaves a player with
  // no objective and no diagnostic. So this traps rather than carrying on.
  constexpr std::string_view trapping =
      R"(<conversation name="T_Break" startup="first">
		<phrase text="one" action="NoSuchEntryPointAnywhere();" followup="first"/>
		<phrase text="two" action="EnvWriteInt(\'/second\', 1);" followup="end"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(trapping)).ok());

  CHECK(!stage.run("//void\nRunConv(\"T_Break\");\n", "break.vs", kNoObject,
                   /*expect_trap=*/true));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/second") == 0);
}

TEST(conversation_followup_first_walks_forward_and_does_not_rescan) {
  Stage stage;
  // The rule the whole format note turns on, and the one no single-phrase test
  // can see: **`first` with no list means the next phrase**. A reader that
  // rescanned the document would replay phrase one for ever, and 98 of the 110
  // shipped documents end on a `first` with nothing after it.
  //
  // Each phrase adds a different power of ten, so the sum says which played and
  // in what order rather than merely that something did.
  constexpr std::string_view walk =
      R"(<conversation name="T_Walk" startup="first">
		<phrase text="one" action="EnvWriteInt(\'/n\', EnvReadInt(\'/n\') + 1);" followup="first"/>
		<phrase text="two" action="EnvWriteInt(\'/n\', EnvReadInt(\'/n\') + 10);" followup="first"/>
		<phrase text="three" action="EnvWriteInt(\'/n\', EnvReadInt(\'/n\') + 100);" followup="first"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(walk)).ok());

  CHECK(stage.run("//void\nRunConv(\"T_Walk\");\n", "walk.vs"));
  // Each phrase exactly once, and then the conversation ended by running off
  // the last one rather than by any phrase saying `end`.
  CHECK(stage.env.env().read_int(EnvScope::root(), "/n") == 111);
}

TEST(conversation_followup_end_stops_in_the_middle_of_a_document) {
  Stage stage;
  // `end` on a phrase that is *not* the last one, which is the only shape that
  // can tell "the mode stopped it" from "there was nothing after it". Ten of
  // the 110 shipped documents end on an explicit `end`.
  constexpr std::string_view stops =
      R"(<conversation name="T_Stop" startup="first">
		<phrase text="one" action="EnvWriteInt(\'/n\', EnvReadInt(\'/n\') + 1);" followup="end"/>
		<phrase text="two" action="EnvWriteInt(\'/n\', EnvReadInt(\'/n\') + 10);" followup="end"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(stops)).ok());

  CHECK(stage.run("//void\nRunConv(\"T_Stop\");\n", "stop.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/n") == 1);
}

/// A menu whose options loop back to the prompt is read once through and then
/// closed: each option is taken the first time it is offered, and a prompt
/// whose every option has been taken ends the run. The Tutorial's `1 Welcome`
/// (`Commands;Selection;Done`, the first two looping to `repeat`) played
/// against the phrase cap under a first-always rule.
TEST(conversation_a_choice_takes_each_option_once_and_then_closes) {
  Stage stage;
  constexpr std::string_view menu =
      R"(<conversation name="T_Loop" startup="first" startup_phrases="ask">
		<phrase label="ask" text="Your task." followup="choice"
			followup_phrases="How;Why;Done"/>
		<phrase label="How" text="Like so." choice_text="How?"
			action="EnvWriteInt(\'/how\', EnvReadInt(\'/how\') + 1);"
			followup="first" followup_phrases="ask"/>
		<phrase label="Why" text="Because." choice_text="Why?"
			action="EnvWriteInt(\'/why\', EnvReadInt(\'/why\') + 1);"
			followup="first" followup_phrases="ask"/>
		<phrase label="Done" text="Right away." choice_text="Done"
			action="EnvWriteInt(\'/done\', 1);" return="\'GO\'" followup="end"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(menu)).ok());
  CHECK(stage.run("//void\nRunConv(\"T_Loop\");\n", "loop.vs"));
  // How once, Why once, then Done -- and the run ended on Done's `end`.
  CHECK(stage.env.env().read_int(EnvScope::root(), "/how") == 1);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/why") == 1);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/done") == 1);
  CHECK(stage.campaign.results().get("T_Loop") == "GO");

  // And a menu with no way out closes once every option has been read,
  // rather than being asked until the cap.
  constexpr std::string_view trap =
      R"(<conversation name="T_Trap" startup="first" startup_phrases="ask">
		<phrase label="ask" text="Again?" followup="choice" followup_phrases="A;B"/>
		<phrase label="A" text="a" choice_text="A"
			action="EnvWriteInt(\'/asked\', EnvReadInt(\'/asked\') + 1);"
			followup="first" followup_phrases="ask"/>
		<phrase label="B" text="b" choice_text="B"
			action="EnvWriteInt(\'/asked\', EnvReadInt(\'/asked\') + 1);"
			followup="first" followup_phrases="ask"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(trap)).ok());
  CHECK(stage.run("//void\nRunConv(\"T_Trap\");\n", "trap.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/asked") == 2);
}

TEST(conversation_a_choice_with_no_list_offers_only_the_phrases_that_have_one) {
  Stage stage;
  // The rule for the two shipped documents that write `followup="choice"` with
  // no `followup_phrases`: the candidates are the phrases that follow and carry
  // a `choice_text`. A reader that offered *every* following phrase would pick
  // the one right after the prompt whether or not it is an option, which here
  // is the "not an option" phrase deliberately placed first.
  //
  // And the pick itself is documented: with nobody to ask, `choice` takes the
  // first eligible option, so the answer is `SPEAR` rather than a draw.
  constexpr std::string_view menu =
      R"(<conversation name="T_Menu" startup="first">
		<phrase text="What will it be?" followup="choice"/>
		<phrase text="an aside, not an option"
			action="EnvWriteInt(\'/aside\', 1);" followup="end"/>
		<phrase text="here you are" choice_text="A spear"
			action="EnvWriteInt(\'/spear\', 1);" return="\'SPEAR\'" followup="end"/>
		<phrase text="here you are" choice_text="A shield"
			action="EnvWriteInt(\'/shield\', 1);" return="\'SHIELD\'" followup="end"/>
	</conversation>)";
  REQUIRE(stage.campaign.conversations().add(bytes_of(menu)).ok());

  CHECK(stage.run("//void\nRunConv(\"T_Menu\");\n", "menu.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/aside") == 0);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/spear") == 1);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/shield") == 0);
  CHECK(stage.campaign.results().get("T_Menu") == "SPEAR");
}
