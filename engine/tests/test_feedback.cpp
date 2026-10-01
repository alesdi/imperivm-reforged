// What a script asks the interface to show.
//
// Three things this file is built to catch:
//
//   1. **`rollover` is about the command, not the object.** Every one of its
//      five forms reads the described row first and falls back to the object
//      only when there is none, and the form that takes *no arguments at all*
//      is what proves it. A reader that built the tooltip from the receiver
//      would pass every test that only ever calls the two-argument form.
//
//   2. **The same signature means two different things.** `rollover/3` and
//      `rollover_desc/3` are both `(Obj, str, bool)`, and the middle argument
//      is a class name in one and a message in the other. Sixteen shipped
//      sites agree; getting it backwards puts a unit's name where a sentence
//      belongs and shows the wrong cost.
//
//   3. **`CreateFeedback` stores nothing and must still resolve.** It is a
//      display request with `SetPlayerStatus`'s standing, and the reason it is
//      worth having at all is that a trap is not a no-op: fifty scripts died on
//      it, twenty of them with nothing else in the way.

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/feedback.hpp"
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

/// Two `<cmd>` rows in the shape `DATA\COMMANDS\*.XML` uses: one with both
/// display strings and a cost, one with a name and nothing else.
///
/// 384 of the 404 shipped rows carry a `rollover` and 253 a `description`, so
/// "has a name and no sentence" is the ordinary minority case and not a
/// pathology.
constexpr std::string_view kCommandsXml = R"(<commands>
<cmd name="repair arena" priority="1" rollover="Repair"
     description="Puts the arena back in order" costgold="500" costfood="20"/>
<cmd name="BTestSwordsman" priority="1" sclass="BTestSwordsman" rollover="Swordsman"
     costgold="120" costpop="1"/>
<cmd name="plain" priority="1"/>
</commands>)";

/// Everything the family needs: a world with a command system holding the rows,
/// the full registry, and a scheduler to run a script through.
struct Stage {
  World world;
  CommandSystem commands;
  EnvSystem env;
  ViewState view;
  ShortcutTable shortcuts;
  script::HostRegistry registry;
  WorldHost host{world};
  script::Scheduler scheduler;
  HostContext context;

  Stage() {
    world.seed(3);
    world.add_system(&env);
    world.add_system(&commands);
    CommandTable table;
    (void)table.merge(bytes_of(kCommandsXml));
    commands.set_table(std::move(table));
    (void)register_all_hosts(registry);
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    context.world = &world;
    context.object_type = kTypeObj;
    context.view = &view;
    context.shortcuts = &shortcuts;
    scheduler.set_user(&context);
    install_objlist_lifetime(scheduler);
    // Something for a script to point the family at. The `rollover` family
    // ignores its object argument -- that is the finding -- and passing a real
    // one anyway is what makes the claim assertable rather than accidental.
    subject = world.spawn(NativeClass::decor, nullptr);
    (void)world.named_objects().bind("NO_Subject", subject);
  }

  ObjectId subject = kNoObject;

  bool run(std::string_view source, const char* name, ObjectId owner = kNoObject) {
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
    for (int pass = 0; pass < 4 && scheduler.live_count() > 0; ++pass) {
      const script::RunReport report = scheduler.advance(1000);
      for (const script::FailedScript& trap : report.traps) {
        std::printf("  trap %s:%u: %s\n", trap.source_name.c_str(), trap.trap.line,
                    trap.trap.detail.c_str());
        return false;
      }
    }
    return scheduler.live_count() == 0;
  }
};

}  // namespace

// --------------------------------------------------------------------------
// the command row
// --------------------------------------------------------------------------

TEST(feedback_the_command_table_reads_the_two_display_strings) {
  // They are new fields on `CommandDef`, and they are the whole reason the
  // `rollover` family can answer anything: the retail formatter reads two
  // `std::string` globals, and `DATA\COMMANDS\*.XML` carries exactly two
  // display strings per row.
  CommandTable table;
  REQUIRE(table.merge(bytes_of(kCommandsXml)).ok());

  const CommandDef* repair = table.find("repair arena");
  REQUIRE(repair != nullptr);
  CHECK(repair->rollover == "Repair");
  CHECK(repair->description == "Puts the arena back in order");
  CHECK(repair->cost_gold == 500);

  // A row with a name and no sentence, which 151 of the 404 shipped rows are.
  const CommandDef* swordsman = table.find("BTestSwordsman");
  REQUIRE(swordsman != nullptr);
  CHECK(swordsman->rollover == "Swordsman");
  CHECK(swordsman->description.empty());

  // And a row with neither.
  const CommandDef* plain = table.find("plain");
  REQUIRE(plain != nullptr);
  CHECK(plain->rollover.empty());
  CHECK(plain->description.empty());
}

// --------------------------------------------------------------------------
// the composition
// --------------------------------------------------------------------------

TEST(feedback_compose_rollover_stacks_the_pieces_the_row_carries) {
  CommandTable table;
  REQUIRE(table.merge(bytes_of(kCommandsXml)).ok());
  const CommandDef* repair = table.find("repair arena");
  REQUIRE(repair != nullptr);

  // Name, then sentence, one per line. `\n` is the separator every caller that
  // appends to the result also uses -- `rollover(this,false) + "\n<color 255 0
  // 0>" + Translate(...)`.
  CHECK(compose_rollover(repair, {}, false) == "Repair\nPuts the arena back in order");

  // A message goes after the sentence.
  const std::string with_message = compose_rollover(repair, "Already available", false);
  CHECK(with_message == "Repair\nPuts the arena back in order\nAlready available");

  // The cost block only when asked for, and only the pieces the row charges.
  const std::string with_cost = compose_rollover(repair, {}, true);
  CHECK(with_cost.find("500") != std::string::npos);
  CHECK(with_cost.find("20") != std::string::npos);
  CHECK(with_cost.find("gold ico") != std::string::npos);
  CHECK(with_cost.find("food ico") != std::string::npos);
  // Not population or stamina: this row charges neither, and a tooltip that
  // said "0 population" would be worse than one that said nothing.
  CHECK(with_cost.find("pop ico") == std::string::npos);
  CHECK(with_cost.find("stamina ico") == std::string::npos);

  // A row that charges nothing gets no cost block at all, even when asked.
  CHECK(compose_rollover(table.find("plain"), {}, true).empty());
}

TEST(feedback_compose_rollover_survives_a_row_that_is_not_there) {
  // Null is a real case and not a defensive check: it is what `rollover()`
  // means when nothing is being described, which is every headless run.
  CHECK(compose_rollover(nullptr, {}, false).empty());
  CHECK(compose_rollover(nullptr, {}, true).empty());
  // The message still comes through, because it is the caller's and not the
  // row's.
  CHECK(compose_rollover(nullptr, "Already available", true) == "Already available");
}

// --------------------------------------------------------------------------
// the entry points
// --------------------------------------------------------------------------

TEST(feedback_takes_no_entry_point_from_another_domain) {
  script::HostRegistry busy;
  imperivm::test::define_all_except("feedback", busy);

  const std::pair<std::string_view, std::uint16_t> claimed[] = {
      {"CreateFeedback", 2},  {"rollover", 0},         {"rollover", 2},
      {"rollover", 3},        {"rollover_desc", 3},    {"View", 2},
      {"ViewPos", 0},         {"LockView", 0},         {"UnlockView", 0},
      {"IsViewLocked", 0},    {"BlockUserInput", 0},   {"UnblockUserInput", 0},
      {"PlayMovie", 1},       {"ShowHint", 3},         {"ShowTutorial", 3},
      {"ShowAnnouncement", 2},{"HideAnnouncement", 1}, {"SetShortcutSel", 3},
      {"GetShortcutSel", 2},  {"PlaySound", 1},        {"PlaySound", 2},
      {"SetFog", 1},          {"HideZoomMap", 0},      {"ShowZoomMap", 0},
      {"_ZoomMapLastShownTime", 0}, {"UserNotification", 4}, {"cls", 0},
  };
  for (const auto& [name, arity] : claimed) {
    const std::uint32_t index = busy.find(script::CallKind::free_function, name, arity);
    if (index != script::kUnresolvedHost) CHECK(busy.entry(index).fn == nullptr);
  }

  script::HostRegistry mine;
  CHECK(register_feedback_host(mine) == feedback_host_entry_count());
  CHECK(feedback_host_entry_count() == 28);
}

/// The three presentation entry points that are correct as no-ops.
///
/// A trap is not a no-op, which is the whole reason these are bound: a script
/// that reaches an unimplemented name stops there, and `SQUADMONITOR.VS` calls
/// `UserNotification` six times. What has to be true is that binding them
/// changed nothing a peer could disagree about, so the test is the state hash
/// on both sides of the calls rather than an assertion about a return value
/// none of them has.
TEST(feedback_the_presentation_no_ops_run_and_move_no_state) {
  Stage stage;
  const std::uint64_t before = stage.world.state_hash();

  CHECK(stage.run(R"(
    UserNotification("location message", "Sent food wagon", GetNamedObj("NO_Subject").pos, 1);
    UserNotification("building attacked", "", GetNamedObj("NO_Subject").pos, 4);
    cls();
    InvalidateDamageFormulaParams();
  )", "noops.vs"));

  // Every one of them ran to the end -- `run` returns false on a trap -- and
  // none of them is a `HostStatus::error` dressed up as a success, because a
  // refusal would have stopped the script on its line.
  CHECK(stage.world.state_hash() == before);

  // The fourth argument is a **1-based** player id and the original compares it
  // against the local player's number, doing nothing when they differ. There is
  // no local player here at all, which is the permanent "they differ" case, so
  // the player argument cannot change the answer: a run that passes every slot
  // still moves nothing.
  CHECK(stage.run(R"(
    int i;
    for (i = 0; i < 17; i += 1)
      UserNotification("unit attacked", "", GetNamedObj("NO_Subject").pos, i);
  )", "notify_all.vs"));
  CHECK(stage.world.state_hash() == before);
}

TEST(feedback_rollover_answers_about_the_described_command) {
  Stage stage;
  // Nothing described yet: the family answers the empty string rather than
  // trapping, which is what a headless run and a script called outside the
  // interface both are.
  CHECK(stage.run(
      "//void\n"
      "EnvWriteString(\"/before\", rollover());\n",
      "before.vs"));
  CHECK(stage.env.env().read_string(EnvScope::root(), "/before").empty());

  // Now point at a row. The no-argument form is the one that proves the
  // reading: it has no object to build a tooltip from.
  stage.context.described_command = "repair arena";
  CHECK(stage.run(
      "//void\n"
      "EnvWriteString(\"/after\", rollover());\n",
      "after.vs"));
  CHECK(stage.env.env().read_string(EnvScope::root(), "/after") ==
        "Repair\nPuts the arena back in order");
}

TEST(feedback_rollover_two_reads_its_second_argument_by_type) {
  Stage stage;
  stage.context.described_command = "repair arena";

  // A bool asks for the cost block; a string is a message. One arity, two
  // shapes, told apart at run time -- `UnitsInSettlement`'s arrangement.
  CHECK(stage.run(
      "//void\n"
      "Obj it;\n"
      "it = GetNamedObj(\"NO_Subject\").obj;\n"
      "EnvWriteString(\"/cost\", rollover(it, true));\n"
      "EnvWriteString(\"/quiet\", rollover(it, false));\n"
      "EnvWriteString(\"/said\", rollover(it, \"Already available\"));\n",
      "two.vs"));

  const std::string cost(stage.env.env().read_string(EnvScope::root(), "/cost"));
  const std::string quiet(stage.env.env().read_string(EnvScope::root(), "/quiet"));
  const std::string said(stage.env.env().read_string(EnvScope::root(), "/said"));

  CHECK(cost.find("500") != std::string::npos);
  CHECK(quiet.find("500") == std::string::npos);
  CHECK(said.find("Already available") != std::string::npos);
  CHECK(said.find("500") == std::string::npos);
  // All three carry the row's own name, because all three are about the row.
  CHECK(cost.rfind("Repair", 0) == 0);
  CHECK(quiet.rfind("Repair", 0) == 0);
  CHECK(said.rfind("Repair", 0) == 0);
}

TEST(feedback_rollover_three_prices_the_named_class_and_desc_does_not) {
  Stage stage;
  // The command being described is the *build* command; the class named in the
  // middle argument is what would be trained. `VERIFY_CMDCOST_BUILDING` writes
  // exactly this: `rollover(this, class, true)`, with `class` parsed out of
  // `cmdparam`.
  stage.context.described_command = "repair arena";
  CHECK(stage.run(
      "//void\n"
      "Obj it;\n"
      "it = GetNamedObj(\"NO_Subject\").obj;\n"
      "EnvWriteString(\"/klass\", rollover(it, \"BTestSwordsman\", true));\n"
      "EnvWriteString(\"/desc\", rollover_desc(it, \"Already collected\", true));\n",
      "three.vs"));

  const std::string klass(stage.env.env().read_string(EnvScope::root(), "/klass"));
  const std::string desc(stage.env.env().read_string(EnvScope::root(), "/desc"));

  // `rollover/3` switched rows entirely: the swordsman's name and the
  // swordsman's cost, not the arena's.
  CHECK(klass.rfind("Swordsman", 0) == 0);
  CHECK(klass.find("120") != std::string::npos);
  CHECK(klass.find("500") == std::string::npos);

  // `rollover_desc/3` did not: same signature, and the middle argument is a
  // message. The arena's own name and cost, with the sentence appended.
  CHECK(desc.rfind("Repair", 0) == 0);
  CHECK(desc.find("Already collected") != std::string::npos);
  CHECK(desc.find("500") != std::string::npos);
  CHECK(desc.find("120") == std::string::npos);
}

TEST(feedback_rollover_three_falls_back_when_the_class_names_no_row) {
  Stage stage;
  stage.context.described_command = "repair arena";
  // A class the command table does not carry gets the described row's tooltip
  // rather than a blank: the player is pointing at a button either way, and an
  // empty tooltip reads as a bug in the interface.
  CHECK(stage.run(
      "//void\n"
      "Obj it;\n"
      "it = GetNamedObj(\"NO_Subject\").obj;\n"
      "EnvWriteString(\"/miss\", rollover(it, \"NoSuchClass\", false));\n",
      "miss.vs"));
  CHECK(stage.env.env().read_string(EnvScope::root(), "/miss") ==
        "Repair\nPuts the arena back in order");
}

TEST(feedback_create_feedback_runs_and_stores_nothing) {
  Stage stage;

  // The shipped shape, and the reason this entry point earns its place: the
  // statement after it is the one that matters, and until this existed the
  // script never reached it.
  CHECK(stage.run(
      "//void\n"
      "CreateFeedback(\"Heal\", GetNamedObj(\"NO_Subject\").obj.AsUnit());\n"
      "EnvWriteInt(\"/healed\", 1);\n",
      "heal.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/healed") == 1);

  // Nothing was recorded, and nothing may be: two peers that draw different
  // sparkles must still hash alike.
  const std::uint64_t before = stage.world.hashes().hash_of_hashes;
  CHECK(stage.run("//void\nCreateFeedback(\"Experience\", GetNamedObj(\"NO_Subject\").obj);\n",
                  "again.vs"));
  CHECK(stage.world.hashes().hash_of_hashes == before);

  // An effect on a handle that resolves to nothing is not an error: the unit
  // died between the spell landing and the sparkle being asked for, which is
  // ordinary.
  CHECK(stage.run("//void\nCreateFeedback(\"Damage1\", GetNamedObj(\"NO_Nobody\").obj);\n",
                  "gone.vs"));
}


// --------------------------------------------------------------------------
// what the fault sweep asked for
// --------------------------------------------------------------------------

TEST(feedback_the_cost_block_is_in_the_order_the_command_xml_writes) {
  // `costgold`, `costfood`, `costpop`, `coststamina` -- the order the attribute
  // list has and the order the interface shows. Unasserted, a reader could sort
  // them any way at all and every other test would pass, because every other
  // test looks for the pieces rather than at their order.
  CommandTable table;
  CommandDef row;
  row.name = "everything";
  row.rollover = "All Four";
  row.cost_gold = 1;
  row.cost_food = 2;
  row.cost_pop = 3;
  row.cost_stamina = 4;
  table.set(row);

  const std::string out = compose_rollover(table.find("everything"), {}, true);
  const std::size_t gold = out.find("gold ico");
  const std::size_t food = out.find("food ico");
  const std::size_t pop = out.find("pop ico");
  const std::size_t stamina = out.find("stamina ico");
  REQUIRE(gold != std::string::npos);
  REQUIRE(food != std::string::npos);
  REQUIRE(pop != std::string::npos);
  REQUIRE(stamina != std::string::npos);
  CHECK(gold < food);
  CHECK(food < pop);
  CHECK(pop < stamina);

  // And the whole block is one line under the name, rather than four.
  CHECK(out.find('\n') == out.find("\nAll Four") + 1 || out.rfind("All Four", 0) == 0);
  std::size_t newlines = 0;
  for (const char c : out) newlines += (c == '\n') ? 1 : 0;
  CHECK(newlines == 1);
}

TEST(feedback_create_feedback_answers_nothing_at_all) {
  // It is registered `void` in `gbr.exe` and every one of the 72 shipped sites
  // is a bare statement, so a value coming back is unobservable from script --
  // which is exactly why it needs asserting here. An entry point that quietly
  // answered something would be a value on the stack that the VM has to drop.
  script::HostRegistry registry;
  CHECK(register_feedback_host(registry) == feedback_host_entry_count());
  const std::uint32_t index =
      registry.find(script::CallKind::free_function, "CreateFeedback", 2);
  REQUIRE(index != script::kUnresolvedHost);
  const script::HostEntry& entry = registry.entry(index);
  REQUIRE(entry.fn != nullptr);

  World world;
  world.seed(1);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;

  script::Value arguments[] = {script::Value::string("Heal"),
                               script::Value::object(kTypeObj, kNoObject)};
  script::CallContext call;
  call.arguments = arguments;
  call.user = &context;
  call.name = "CreateFeedback";

  const script::HostOutcome outcome = entry.fn(call);
  CHECK(outcome.status == script::HostStatus::ok);
  CHECK(outcome.value.is_nil());
  CHECK(outcome.suspend_for == 0);

  // And with no world it refuses rather than dereferencing, which is the rule
  // every entry point in this tree follows.
  script::CallContext bare;
  bare.arguments = arguments;
  bare.user = nullptr;
  CHECK(entry.fn(bare).status == script::HostStatus::error);
}


// ==========================================================================
// the camera, the chrome and the control groups
// ==========================================================================

TEST(feedback_the_view_reads_back_what_the_camera_was_told) {
  Stage stage;
  // The shipped idiom, three statements, in three of Zama's sequences: save the
  // camera, take it somewhere for a cutscene, put it back. A `ViewPos` that did
  // not read back what `View` wrote would strand the player looking at the
  // wrong end of the map.
  CHECK(stage.run(
      "//void\n"
      "point saved;\n"
      "saved = ViewPos();\n"
      "View(Point(4000, 5000), true);\n"
      "EnvWriteInt(\"/moved_x\", ViewPos().x);\n"
      "EnvWriteInt(\"/moved_y\", ViewPos().y);\n"
      "if (IsViewLocked()) EnvWriteInt(\"/locked_midway\", 1);\n"
      "View(saved, false);\n"
      "EnvWriteInt(\"/back_x\", ViewPos().x);\n",
      "camera.vs"));

  CHECK(stage.env.env().read_int(EnvScope::root(), "/moved_x") == 4000);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/moved_y") == 5000);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/back_x") == 0);
  // The bool went with each move: locked on the way out, unlocked on the way
  // back. Both halves have to be asserted -- a `View` that ignored the argument
  // entirely would leave the flag false at the end too, which is what the
  // restoring call wants and says nothing about the outgoing one.
  CHECK(stage.env.env().read_int(EnvScope::root(), "/locked_midway") == 1);
  CHECK(!stage.view.locked);
  CHECK(stage.view.x == 0);
}

TEST(feedback_the_view_lock_and_the_input_block_are_two_different_switches) {
  Stage stage;
  CHECK(stage.run(
      "//void\n"
      "LockView();\n"
      "BlockUserInput();\n",
      "lock.vs"));
  CHECK(stage.view.locked);
  CHECK(stage.view.input_blocked);

  // Unlocking the view does not unblock the mouse, and vice versa. Seventeen
  // `BlockUserInput` sites and two `LockView` sites, and a cutscene uses both.
  CHECK(stage.run("//void\nUnlockView();\n", "unlock.vs"));
  CHECK(!stage.view.locked);
  CHECK(stage.view.input_blocked);

  CHECK(stage.run("//void\nUnblockUserInput();\n", "unblock.vs"));
  CHECK(!stage.view.input_blocked);

  // And `IsViewLocked` reads the one `LockView` writes -- the registered getter
  // that makes the flag worth keeping at all.
  CHECK(stage.run(
      "//void\n"
      "LockView();\n"
      "if (IsViewLocked()) EnvWriteInt(\"/saw\", 1);\n",
      "read.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/saw") == 1);
}

TEST(feedback_a_control_group_outlives_the_script_that_set_it) {
  Stage stage;
  const ObjectId one = stage.world.spawn(NativeClass::decor, nullptr);
  const ObjectId two = stage.world.spawn(NativeClass::decor, nullptr);
  CHECK(stage.world.named_objects().bind("NO_One", one));
  CHECK(stage.world.named_objects().bind("NO_Two", two));

  // The shipped shape: `SetShortcutSel(1, 1, NO_Scipio.GetObjList())`. The
  // list is a local of the script that made it and dies with it, so the group
  // has to be a **copy** -- keeping the handle would leave ctrl-1 pointing at a
  // released pool entry the moment the sequence ended.
  CHECK(stage.run(
      "//void\n"
      "ObjList ol;\n"
      "ol.Add(GetNamedObj(\"NO_One\").obj);\n"
      "ol.Add(GetNamedObj(\"NO_Two\").obj);\n"
      "SetShortcutSel(1, 1, ol);\n",
      "group.vs"));

  // The script has ended and its pool entry is gone; the group is still here.
  const std::span<const ObjectId> group = stage.shortcuts.group(0, 1);
  REQUIRE(group.size() == 2);
  CHECK(group[0] == one);
  CHECK(group[1] == two);

  // Player 1 in a script is player 0 here, and nobody else got a group. The
  // *second* assignment is what makes that assertable: one player alone cannot
  // tell "stored under the right player" from "stored under player zero".
  CHECK(stage.shortcuts.group(1, 1).empty());
  CHECK(stage.shortcuts.group(0, 2).empty());
  CHECK(stage.run(
      "//void\n"
      "ObjList ol;\n"
      "ol.Add(GetNamedObj(\"NO_Two\").obj);\n"
      "SetShortcutSel(4, 1, ol);\n",
      "group4.vs"));
  REQUIRE(stage.shortcuts.group(3, 1).size() == 1);
  CHECK(stage.shortcuts.group(3, 1)[0] == two);
  // And player 0's group is untouched by player 3's.
  REQUIRE(stage.shortcuts.group(0, 1).size() == 2);

  // And `GetShortcutSel` reads it back -- zero shipped sites, and the reason
  // the table is stored rather than dropped.
  CHECK(stage.run(
      "//void\n"
      "EnvWriteInt(\"/count\", GetShortcutSel(1, 1).count);\n"
      "EnvWriteInt(\"/empty\", GetShortcutSel(1, 5).count);\n",
      "read_group.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/count") == 2);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/empty") == 0);
}

TEST(feedback_a_control_group_drops_an_object_that_dies) {
  ShortcutTable table;
  const ObjectId objects[] = {7, 8, 9};
  table.assign(0, 3, objects);
  REQUIRE(table.group(0, 3).size() == 3);

  CHECK(table.forget(8) == 1);
  REQUIRE(table.group(0, 3).size() == 2);
  CHECK(table.group(0, 3)[0] == 7);
  CHECK(table.group(0, 3)[1] == 9);
  // Order is kept: a control group is a list the player built, not a set.
  CHECK(table.forget(999) == 0);

  // Out of range is dropped rather than refused, which is `SelectionTable`'s
  // rule for an out-of-range player and keeps a script with a bad slot inert
  // rather than fatal. **Dropped, not clamped**: a slot that folded onto zero
  // would let a script with a typo silently overwrite ctrl-0.
  const ObjectId other[] = {1};
  table.assign(0, 0, other);
  REQUIRE(table.group(0, 0).size() == 1);
  table.assign(0, 99, objects);
  CHECK(table.group(0, 99).empty());
  CHECK(table.group(0, 0).size() == 1);
  table.assign(200, 1, objects);
  CHECK(table.group(200, 1).empty());
  CHECK(table.group(0, 1).empty());
  table.assign(0, -1, objects);
  CHECK(table.group(0, -1).empty());
  CHECK(table.group(0, 0).size() == 1);

  table.clear();
  CHECK(table.group(0, 3).empty());
}

TEST(feedback_the_write_only_chrome_runs_and_answers_the_documented_nothing) {
  Stage stage;
  const ObjectId who = stage.world.spawn(NativeClass::decor, nullptr);
  CHECK(stage.world.named_objects().bind("NO_Subject2", who));

  // Every one of these is registered with a return type and discarded at every
  // shipped site, so the answers here are what a request that did not happen
  // would give. Pinned because a future reader would otherwise assume they were
  // recovered from the executable.
  CHECK(stage.run(
      "//void\n"
      "Obj it;\n"
      "it = GetNamedObj(\"NO_Subject2\").obj;\n"
      "if (PlayMovie(\"movies\\\\intro.avi\")) EnvWriteInt(\"/played\", 1);\n"
      "EnvWriteInt(\"/hint\", ShowHint(\"key\", \"text\", it));\n"
      "EnvWriteInt(\"/tut\", ShowTutorial(\"key\", \"text\", it));\n"
      "ShowAnnouncement(\"key\", \"value\");\n"
      "HideAnnouncement(\"key\");\n"
      "PlaySound(\"sound\");\n"
      "PlaySound(1, \"sound\");\n"
      "it.PlaySound(\"sound\");\n"
      "EnvWriteInt(\"/reached_the_end\", 1);\n",
      "chrome.vs"));

  CHECK(stage.env.env().read_int(EnvScope::root(), "/played") == 0);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/hint") == 0);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/tut") == 0);
  // The point of the whole family: the statement after it runs.
  CHECK(stage.env.env().read_int(EnvScope::root(), "/reached_the_end") == 1);
}

TEST(feedback_the_chrome_degrades_without_a_screen) {
  // A headless run has no `ViewState` and no `ShortcutTable`, which is the
  // ordinary case for `imrun` and for every conformance trace. Nothing here may
  // refuse for it -- 187 call sites would trap, and a mission that stops
  // because nobody is watching is worse than a camera that does not move.
  Stage stage;
  stage.context.view = nullptr;
  stage.context.shortcuts = nullptr;
  CHECK(stage.run(
      "//void\n"
      "ObjList ol;\n"
      "View(Point(1, 2), true);\n"
      "LockView();\n"
      "BlockUserInput();\n"
      "SetShortcutSel(1, 1, ol);\n"
      "EnvWriteInt(\"/x\", ViewPos().x);\n"
      "EnvWriteInt(\"/n\", GetShortcutSel(1, 1).count);\n"
      "if (IsViewLocked()) EnvWriteInt(\"/locked\", 1);\n",
      "headless.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/x") == 0);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/n") == 0);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/locked") == 0);
}

// --------------------------------------------------------------------------
// the fog switch and the zoom map
// --------------------------------------------------------------------------

/// `SetFog` is a switch of its own, and it is **not** any of the other three.
TEST(feedback_set_fog_is_a_switch_of_its_own) {
  Stage stage;
  // A match starts fogged, which is what the cutscene idiom's `SetFog(false)`
  // followed by `SetFog(true)` assumes.
  CHECK(stage.view.fog);

  CHECK(stage.run(
      "//void\n"
      "SetFog(false);\n",
      "lift.vs"));
  CHECK(!stage.view.fog);
  // The three switches beside it did not move. `1_Great_Battles_Zama` map 6
  // sequence 15 writes all four in four consecutive lines, so a body that wrote
  // the wrong field would pass every test that only read the one it wrote.
  CHECK(!stage.view.locked);
  CHECK(!stage.view.input_blocked);
  CHECK(!stage.view.zoom_map_open);

  CHECK(stage.run(
      "//void\n"
      "SetFog(true);\n",
      "restore.vs"));
  CHECK(stage.view.fog);
}

/// The zoom map: `HideZoomMap` closes it, `ShowZoomMap` opens it **and stamps
/// the clock**, and `_ZoomMapLastShownTime` is the reader that makes the pair
/// worth storing.
TEST(feedback_the_zoom_map_stamp_is_minus_one_until_it_is_opened) {
  Stage stage;
  CHECK(stage.view.zoom_map_shown_at == -1);

  // `GENERALADVICE3.VS`'s question, and the branch a headless session takes.
  CHECK(stage.run(
      "//void\n"
      "EnvWriteInt(\"/never\", _ZoomMapLastShownTime());\n"
      "HideZoomMap();\n"
      "EnvWriteInt(\"/after_hide\", _ZoomMapLastShownTime());\n",
      "ask.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/never") == -1);
  CHECK(!stage.view.zoom_map_open);
  // **Hiding does not stamp.** If it did, every one of the eight shipped
  // `HideZoomMap()` calls would tell the tutorial the player had opened the
  // minimap, and the hint would never appear.
  CHECK(stage.env.env().read_int(EnvScope::root(), "/after_hide") == -1);

  // Opening stamps. The clock is the scheduler's, which is what `ctx.now`
  // carries, so the value is asserted against that rather than against a
  // literal.
  CHECK(stage.run(
      "//void\n"
      "ShowZoomMap();\n"
      "EnvWriteInt(\"/shown\", _ZoomMapLastShownTime());\n",
      "open.vs"));
  CHECK(stage.view.zoom_map_open);
  const std::int64_t first = stage.view.zoom_map_shown_at;
  CHECK(first > 0);
  CHECK(stage.env.env().read_int(EnvScope::root(), "/shown") ==
        static_cast<std::int32_t>(first));

  // And closing it again leaves the stamp where it was: the question is *ever*,
  // not *now*.
  CHECK(stage.run("//void\nHideZoomMap();\n", "close.vs"));
  CHECK(!stage.view.zoom_map_open);
  CHECK(stage.view.zoom_map_shown_at == first);

  // A second opening moves it forward, so the stamp is the clock and not a
  // constant.
  CHECK(stage.run("//void\nShowZoomMap();\n", "reopen.vs"));
  CHECK(stage.view.zoom_map_shown_at > first);

  // **An embedder with no view bound still answers never, not "opened at time
  // zero".** That is the same answer a headless run gives with a view bound,
  // and the difference matters: zero is a time the tutorial would read as
  // *shown*.
  stage.context.view = nullptr;
  CHECK(stage.run(
      "//void\n"
      "EnvWriteInt(\"/unbound\", _ZoomMapLastShownTime());\n",
      "unbound.vs"));
  CHECK(stage.env.env().read_int(EnvScope::root(), "/unbound") == -1);
}
