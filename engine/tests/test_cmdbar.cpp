// The command bar: which rows a selection offers, what the verifier says,
// and what a press does -- over a synthetic session whose command rows are
// shaped like the shipped ones.

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/cmdbar.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/infobar.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/session.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace script = imperivm::core::script;
using imperivm::core::ClassGraph;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) noexcept {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

/// Several in-memory scripts by path.
class Scripts final : public ScriptResolver {
 public:
  void add(std::string path, std::string text) { files_[std::move(path)] = std::move(text); }
  std::span<const std::byte> source(std::string_view path) override {
    const auto it = files_.find(std::string(path));
    return it == files_.end() ? std::span<const std::byte>{} : bytes_of(it->second);
  }

 private:
  std::map<std::string, std::string> files_;
};

ClassGraph graph_of() {
  ClassGraph graph;
  const char* docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
      R"(<class id="Unit" parent="Object" cpp_class="CVXUnit"><properties maxhealth="100"/><method sig="idle" vs="idle.vs"/><method sig="move" vs="move.vs"/></class>)",
      R"(<class id="Sentry" parent="Unit" cpp_class="CVXUnit"/>)",
      R"(<class id="RHastatus" parent="Unit" cpp_class="CVXUnit"/>)",
      R"(<class id="RSentry" parent="Sentry" cpp_class="CVXUnit"/>)",
      R"(<class id="Building" parent="Object" cpp_class="CVXBuilding"><method sig="idle" vs="idle.vs"/><method sig="train" vs="train.vs"/></class>)",
      // Standing, with health: a turn does not prune it from a selection, and
      // the lockstep sink will command it.
      R"(<class id="RBarracks" parent="Building" cpp_class="CVXBuilding"><properties maxhealth="3000"/></class>)",
      // The shipped `Building` binds `repair`, declares `maxhealth="5000"` and
      // `auto_repair="yes"`; a town hall says `auto_repair="no"`, and a
      // village house keeps the yes and binds `repopulate`.
      R"(<class id="RepairableBuilding" parent="Building" cpp_class="CVXBuilding"><method sig="repair" vs="repair.vs"/><method sig="repopulate" vs="repair.vs"/><properties maxhealth="5000"/><properties auto_repair="yes"/></class>)",
      R"(<class id="RTownhall" parent="RepairableBuilding" cpp_class="CVXBuilding"><properties auto_repair="no"/></class>)",
      R"(<class id="RHouse" parent="RepairableBuilding" cpp_class="CVXBuilding"/>)",
      // A building whose rows issue themselves through a `groupdispatch=`.
      R"(<class id="RArena" parent="Building" cpp_class="CVXBuilding"><properties maxhealth="3000"/></class>)",
  };
  for (const char* doc : docs) graph.add(bytes_of(doc), "test_cmdbar.cpp");
  graph.link();
  return graph;
}

constexpr std::string_view kCommands = R"(<commands>
<cmd name="move" priority="0" offset="1" button="Actions/Move.bmp" rollover="Move" key="m">
  <cmdtext target="" text="Click to move at the point"/>
  <src obj="Unit"/>
  <nsrc obj="Sentry"/>
</cmd>
<cmd name="stand_position" priority="5" button="Actions/Stand Ground.bmp" rollover="Stand ground">
  <src obj="Unit"/>
</cmd>
<cmd name="trainRHastatus" priority="3" button="actions/train RHastatus.bmp" queueicon="gameres/icons/RHastatus.bmp"
     groupverifier="verify_cost.vs" traincommand="yes" costgold="50" execdelay="8000" rollover="Equip Hastatus"
     method="train" param="RHastatus">
  <src obj="RBarracks"/>
</cmd>
<cmd name="repair townhall" priority="1" method="repair" button="actions/repair.bmp" key="r"
     groupverifier="verify_always.vs" rollover="Repair" costgold="500" costfood="0">
  <src obj="RTownhall"/>
</cmd>
<cmd name="unitsout" priority="9" button="actions/units out.bmp" key="o">
  <src obj="RTownhall"/>
  <src obj="RHouse"/>
</cmd>
<cmd name="repair village" priority="1" method="repopulate" button="actions/repair.bmp" key="r"
     rollover="Repopulate" costgold="1000">
  <src obj="RHouse"/>
</cmd>
<cmd name="trainRSentry" priority="4" button="actions/train RSentry.bmp" groupverifier="verify_never.vs"
     traincommand="yes" method="train" param="RSentry">
  <src obj="RBarracks"/>
</cmd>
<cmd name="hireGladiator" priority="1" button="actions/hire.bmp" traincommand="yes" method="train"
     groupdispatch="dispatch_flags.vs">
  <src obj="RArena"/>
</cmd>
<cmd name="showGames" priority="2" button="actions/games.bmp" groupdispatch="dispatch_flags.vs">
  <src obj="RArena"/>
</cmd>
<cmd name="sendGladiator" priority="3" button="actions/send.bmp" traincommand="yes" method="train"
     groupdispatch="dispatch_flags.vs">
  <cmdtext target=""/>
  <src obj="RArena"/>
</cmd>
<cmd name="showGamesAt" priority="4" button="actions/games.bmp" groupdispatch="dispatch_flags.vs">
  <cmdtext target=""/>
  <src obj="RArena"/>
</cmd>
<cmd name="showGamesMod" priority="5" button="actions/games.bmp" groupdispatch="dispatch_modifier.vs">
  <src obj="RArena"/>
</cmd>
</commands>)";

constexpr std::string_view kMap = R"(<mapobject>
	<scriptobj class="RHastatus" num="0" player="1" x="100" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RSentry" num="1" player="1" x="200" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RBarracks" num="2" player="1" x="300" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RHastatus" num="3" player="1" x="400" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RBarracks" num="4" player="1" x="500" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RTownhall" num="5" player="1" x="600" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RHouse" num="6" player="1" x="700" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RArena" num="7" player="1" x="800" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
</mapobject>)";

/// An AI profile naming only its root script.
constexpr const char* kAiProfile = "[Scripts]\nMain.vs = void\n";

struct Fixture {
  script::HostRegistry registry;
  ClassGraph graph = graph_of();
  Scripts scripts;
  std::vector<std::span<const std::byte>> command_docs{bytes_of(kCommands)};
  std::unique_ptr<GameSession> session;
  std::unique_ptr<CommandBar> bar;

  Fixture() {
    register_all_hosts(registry);
    scripts.add("idle.vs", "//void, Obj This\nSleep(100000);\n");
    // An AI's root script, for the sink's takeover of a departed seat.
    scripts.add("DATA/AI/Main.vs", "// void\nSleep(100000);\n");
    scripts.add("move.vs", "//void, Obj This, point pt\nSleep(100000);\n");
    scripts.add("train.vs", "//void, Obj This\nSleep(100000);\n");
    // The shipped shape: `bool f(ObjList objs, str OUT reasonText)`, reading
    // the ambient `cmdparam` and `cmdcost_gold` the described row supplies.
    scripts.add("verify_cost.vs",
                "//bool, ObjList objs, str OUT reasonText\n"
                "if (cmdparam != 'RHastatus') { reasonText = 'wrong param'; return false; }\n"
                "if (cmdcost_gold != 50) { reasonText = 'wrong cost'; return false; }\n"
                "return objs.count == 1;\n");
    scripts.add("repair.vs", "//void, Obj b\nSleep(100000);\n");
    // `VERIFY_CMDCOST_BUILDING.VS`'s shape: it answers true on every path.
    scripts.add("verify_always.vs", "//bool, ObjList objs, str OUT reasonText\nreturn true;\n");
    scripts.add("verify_never.vs",
                "//bool, ObjList objs, str OUT reasonText\n"
                "reasonText = 'Not now';\nreturn false;\n");
    // A dispatch that shows the flags it was handed: `bReplace` queues a
    // `train`, its absence an `idle`, on every object of the list.
    scripts.add("dispatch_flags.vs",
                "//void, ObjList objs, point pt, Obj obj, bool bReplace, bool bModifier, int player\n"
                "if (bReplace) objs.AddCommand(false, 'train');\n"
                "else objs.AddCommand(false, 'idle');\n");
    // The same for `bModifier`.
    scripts.add("dispatch_modifier.vs",
                "//void, ObjList objs, point pt, Obj obj, bool bReplace, bool bModifier, int player\n"
                "if (bModifier) objs.AddCommand(false, 'train');\n"
                "else objs.AddCommand(false, 'idle');\n");
    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.scripts = &scripts;
    inputs.map_objects = bytes_of(kMap);
    inputs.commands = command_docs;
    inputs.ai_profile = bytes_of(kAiProfile);
    auto made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    session = std::move(made.value());
    session->set_local_player(0);
    (void)session->start_match(MatchOptions{});
    (void)session->start_object_scripts();
    session->advance(1, 400);
    bar = std::make_unique<CommandBar>(*session);
  }

  void select(std::initializer_list<ObjectId> ids) {
    Selection& selection = session->selections().player(0);
    selection.clear();
    for (const ObjectId id : ids) selection.add(id);
  }
  [[nodiscard]] const CommandQueue* queue(ObjectId id) {
    CommandSystem* commands = command_system(session->world());
    return commands == nullptr ? nullptr : commands->find(id);
  }
};

}  // namespace

TEST(cmdbar_offers_the_rows_the_selections_classes_declare) {
  Fixture f;
  f.select({1});  // a hastatus
  std::vector<CommandButton> buttons = f.bar->describe(0);
  REQUIRE(buttons.size() == 2);
  // By priority: `move` (0) before `stand_position` (5).
  CHECK(buttons[0].name == "move");
  CHECK(buttons[0].icon == "gameres/CmdBar/Actions/Move.bmp");
  CHECK(buttons[0].rollover == "Move");
  CHECK(buttons[0].key == "m");
  CHECK(buttons[0].needs_target);
  CHECK(buttons[0].enabled);
  CHECK(buttons[1].name == "stand_position");
  CHECK(!buttons[1].needs_target);

  // A sentry is a unit the `<nsrc>` takes `move` away from.
  f.select({2});
  buttons = f.bar->describe(0);
  REQUIRE(buttons.size() == 1);
  CHECK(buttons[0].name == "stand_position");

  // A mixed selection offers what every member offers: the sentry's one.
  f.select({1, 2});
  buttons = f.bar->describe(0);
  REQUIRE(buttons.size() == 1);
  CHECK(buttons[0].name == "stand_position");

  // Nothing selected, nothing offered.
  f.select({});
  CHECK(f.bar->describe(0).empty());
}

TEST(cmdbar_runs_the_group_verifier_with_the_row_described) {
  Fixture f;
  f.select({3});  // the barracks
  std::vector<CommandButton> buttons = f.bar->describe(0);
  REQUIRE(buttons.size() == 2);
  // `verify_cost.vs` reads `cmdparam` and `cmdcost_gold` outside any command
  // script; both answer for the described row, and it passes.
  CHECK(buttons[0].name == "trainRHastatus");
  CHECK(buttons[0].enabled);
  CHECK(buttons[0].reason.empty());
  // `verify_never.vs` refuses and says why through its OUT parameter.
  CHECK(buttons[1].name == "trainRSentry");
  CHECK(!buttons[1].enabled);
  CHECK(buttons[1].reason == "Not now");

  // Two barracks: the verifier sees both and refuses (`objs.count == 1`).
  f.select({3, 5});
  buttons = f.bar->describe(0);
  REQUIRE(buttons.size() == 2);
  CHECK(!buttons[0].enabled);
}

TEST(cmdbar_press_issues_a_targetless_row_and_waits_for_a_targeted_one) {
  Fixture f;
  f.select({3});
  CHECK(f.bar->press(0, "trainRHastatus", CommandBar::Keys{}) == CommandBar::Press::issued);
  const CommandQueue* queue = f.queue(3);
  REQUIRE(queue != nullptr);
  // Queued behind the idle the barracks is running, as its own row.
  bool queued = false;
  for (const Command& command : queue->entries) {
    if (command.name == "trainRHastatus") {
      queued = true;
      CHECK(command.param == "RHastatus");
      CHECK(command.cost_gold == 50);
      CHECK(command.user);
    }
  }
  CHECK(queued);
  // A refused row does nothing.
  const std::size_t before = f.queue(3)->entries.size();
  CHECK(f.bar->press(0, "trainRSentry", CommandBar::Keys{}) == CommandBar::Press::disabled);
  CHECK(f.queue(3)->entries.size() == before);
  // A row the selection does not offer is unknown.
  CHECK(f.bar->press(0, "move", CommandBar::Keys{}) == CommandBar::Press::unknown);

  // A targeted row waits, and `aim` issues it at the point.
  f.select({1, 4});
  CHECK(f.bar->press(0, "move", CommandBar::Keys{}) == CommandBar::Press::waiting);
  OrderTarget target;
  target.point = Point{500, 600};
  CHECK(f.bar->aim(0, "move", target, CommandBar::Keys{}));
  for (const ObjectId id : {ObjectId{1}, ObjectId{4}}) {
    const CommandQueue* q = f.queue(id);
    REQUIRE(q != nullptr);
    bool moved = false;
    for (const Command& command : q->entries) {
      if (command.name == "move") {
        moved = true;
        CHECK(command.arg_kind == CommandArgKind::point);
        CHECK(command.point.x == 500 && command.point.y == 600);
      }
    }
    CHECK(moved);
  }
}

namespace {

/// Shift held, and nothing else.
constexpr CommandBar::Keys kShift{true, false};

std::size_t named(const CommandQueue* queue, std::string_view name) {
  if (queue == nullptr) return 0;
  std::size_t n = 0;
  for (const Command& command : queue->entries) n += command.name == name ? 1 : 0;
  return n;
}

}  // namespace

TEST(cmdbar_a_train_press_queues_behind_the_training_in_progress) {
  // Playtest #14: a second unit's button on a barracks cancelled the training
  // in progress. A `traincommand="yes"` row never replaces: the bar clears the
  // replace flag for it whatever Shift says (0x005e39f0, `[def+0x1b4]`), and
  // the per-object issue masks it off again (0x004efbbb).
  Fixture f;
  const ObjectId barracks = 3;
  f.select({barracks});
  CHECK(f.bar->press(0, "trainRHastatus", CommandBar::Keys{}) == CommandBar::Press::issued);
  f.session->advance(1, 400);  // the first is running now
  REQUIRE(f.session->selections().player(0).size() == 1);
  CommandSystem* commands = command_system(f.session->world());
  REQUIRE(commands != nullptr);
  REQUIRE(commands->command_name(barracks, 0) == "train");
  const std::uint32_t first = f.queue(barracks)->entries[0].id;
  REQUIRE(f.queue(barracks)->entries[0].script != script::kNoScript);

  // A plain second press queues behind it, and the one in progress runs on.
  CHECK(f.bar->press(0, "trainRHastatus", CommandBar::Keys{}) == CommandBar::Press::issued);
  CHECK(commands->command_count(barracks) == 2);
  CHECK(named(f.queue(barracks), "trainRHastatus") == 2);
  CHECK(f.queue(barracks)->entries[0].id == first);
  CHECK(f.queue(barracks)->entries[0].script != script::kNoScript);

  // Shift changes nothing for a train row: it appends as well.
  CHECK(f.bar->press(0, "trainRHastatus", kShift) == CommandBar::Press::issued);
  CHECK(commands->command_count(barracks) == 3);
  CHECK(f.queue(barracks)->entries[0].id == first);

  // The order the network applies is the same press, and queues the same way.
  CommandBarSink sink(*f.session, *f.bar);
  NetOrder order;
  order.issuer = 0;
  order.kind = NetOrderKind::command;
  order.command = "trainRHastatus";
  order.actors = {barracks};
  CHECK(sink.command(order));
  CHECK(commands->command_count(barracks) == 4);
  CHECK(f.queue(barracks)->entries[0].id == first);
}

TEST(cmdbar_a_rows_press_still_replaces_unless_shift_is_held) {
  // The other side of #14's rule: only `traincommand` rows are exempt. A
  // unit's targetless row and its aimed move replace on a plain press and
  // append with Shift, as they did.
  Fixture f;
  const ObjectId hastatus = 1;
  f.select({hastatus});
  OrderTarget target;
  target.point = Point{500, 600};
  CHECK(f.bar->aim(0, "move", target, CommandBar::Keys{}));
  CHECK(f.bar->aim(0, "move", target, CommandBar::Keys{}));
  CHECK(named(f.queue(hastatus), "move") == 1);
  CHECK(f.bar->aim(0, "move", target, kShift));
  CHECK(named(f.queue(hastatus), "move") == 2);
  CHECK(f.bar->press(0, "stand_position", CommandBar::Keys{}) == CommandBar::Press::issued);
  CHECK(named(f.queue(hastatus), "move") == 0);
  CHECK(named(f.queue(hastatus), "stand_position") == 1);
  CHECK(f.bar->press(0, "stand_position", kShift) == CommandBar::Press::issued);
  CHECK(named(f.queue(hastatus), "stand_position") == 2);
}

TEST(cmdbar_a_dispatch_is_told_a_train_row_does_not_replace) {
  // The bar sets the flags before it knows how the row issues itself, so a
  // `groupdispatch=` script is handed the same `bReplace` the per-object path
  // would have used: never for a train row, and not with Shift for another.
  Fixture f;
  const ObjectId arena = 8;
  CommandSystem* commands = command_system(f.session->world());
  REQUIRE(commands != nullptr);
  const auto replaced = [&](std::string_view row, bool shift) -> bool {
    const std::size_t before = commands->command_count(arena, "train");
    f.select({arena});
    CHECK(f.bar->press(0, row, CommandBar::Keys{shift, false}) == CommandBar::Press::issued);
    f.session->advance(1, 400);  // the dispatch runs on the scheduler
    return commands->command_count(arena, "train") > before;
  };
  CHECK(!replaced("hireGladiator", false));
  CHECK(!replaced("hireGladiator", true));
  CHECK(replaced("showGames", false));
  CHECK(!replaced("showGames", true));

  // An aimed row's flags are set the same way when the target is clicked
  // (0x005e720e).
  const auto aimed = [&](std::string_view row, bool shift) -> bool {
    const std::size_t before = commands->command_count(arena, "train");
    f.select({arena});
    CHECK(f.bar->press(0, row, CommandBar::Keys{}) == CommandBar::Press::waiting);
    OrderTarget target;
    target.point = Point{900, 100};
    CHECK(f.bar->aim(0, row, target, CommandBar::Keys{shift, false}));
    f.session->advance(1, 400);
    return commands->command_count(arena, "train") > before;
  };
  CHECK(!aimed("sendGladiator", false));
  CHECK(aimed("showGamesAt", false));
  CHECK(!aimed("showGamesAt", true));
}

TEST(cmdbar_shift_appends_and_ctrl_is_the_modifier) {
  // `gbr.exe` 0x005e39f0 reads two keys into two flags: Shift (0x10) into the
  // replace flag, as `!Shift`, and Ctrl (0x11) into `bModifier`. The app had
  // passed Shift as both, so a Shift press told a dispatch `bModifier` and a
  // Ctrl press did nothing at all.
  Fixture f;
  const ObjectId hastatus = 1;
  f.select({hastatus});
  OrderTarget target;
  target.point = Point{500, 600};
  const CommandBar::Keys ctrl{false, true};
  CHECK(f.bar->aim(0, "move", target, CommandBar::Keys{}));
  // Ctrl alone does not append: the second move replaces the first.
  CHECK(f.bar->aim(0, "move", target, ctrl));
  CHECK(named(f.queue(hastatus), "move") == 1);
  CHECK(f.bar->aim(0, "move", target, CommandBar::Keys{true, true}));
  CHECK(named(f.queue(hastatus), "move") == 2);

  // A dispatch is handed Ctrl as `bModifier`, and Shift is not it.
  const ObjectId arena = 8;
  CommandSystem* commands = command_system(f.session->world());
  REQUIRE(commands != nullptr);
  const auto told = [&](std::string_view row, CommandBar::Keys keys) -> bool {
    const std::size_t before = commands->command_count(arena, "train");
    f.select({arena});
    CHECK(f.bar->press(0, row, keys) == CommandBar::Press::issued);
    f.session->advance(1, 400);
    return commands->command_count(arena, "train") > before;
  };
  CHECK(told("showGamesMod", ctrl));
  CHECK(!told("showGamesMod", kShift));
  CHECK(!told("showGamesMod", CommandBar::Keys{}));
  // And Ctrl leaves `bReplace` alone: a non-train row still replaces.
  CHECK(told("showGames", ctrl));

  // A lockstep order carries the two as its mode and its modifier.
  CommandBarSink sink(*f.session, *f.bar);
  NetOrder order;
  order.issuer = 0;
  order.kind = NetOrderKind::command;
  order.command = "move";
  order.aimed = true;
  order.actors = {hastatus};
  order.target.point = Point{700, 800};
  order.modifier = true;
  CHECK(sink.command(order));
  CHECK(named(f.queue(hastatus), "move") == 1);
  order.mode = OrderMode::append;
  CHECK(sink.command(order));
  CHECK(named(f.queue(hastatus), "move") == 2);
}

TEST(cmdbar_ctrl_on_a_train_row_queues_train_multiple_count_of_it) {
  // 0x005e39f0: with Ctrl held on a `traincommand` row the bar reads
  // `[GamePlay] TrainMultipleCount` (5 shipped) and posts one order that
  // runs the row that many times (0x004e5e60 over 0x004f1610).
  Fixture f;
  World& world = f.session->world();
  EnvSystem* env = env_of(world);
  REQUIRE(env != nullptr);
  const ObjectId barracks = 3;
  f.select({barracks});
  const CommandBar::Keys ctrl{false, true};
  // This fixture reads no `CONST.INI`: the key is missing, and a missing key
  // reads zero (the out value 0x005e3a91 reads into starts at zero).
  CHECK(f.bar->flags(0, "trainRHastatus", ctrl).repeat == 0);
  env->set_constant("TrainMultipleCount", 5);
  CHECK(f.bar->press(0, "trainRHastatus", CommandBar::Keys{}) == CommandBar::Press::issued);
  f.session->advance(1, 400);  // the first is running now
  const std::uint32_t first = f.queue(barracks)->entries[0].id;
  REQUIRE(named(f.queue(barracks), "trainRHastatus") == 1);

  // What the press posts: Ctrl, and the count read at the click.
  const CommandBar::Flags flags = f.bar->flags(0, "trainRHastatus", ctrl);
  CHECK(flags.modifier);
  CHECK(!flags.append);
  CHECK(flags.repeat == 5);
  CHECK(f.bar->press(0, "trainRHastatus", ctrl) == CommandBar::Press::issued);
  // Five behind the one in training, which runs on.
  CHECK(named(f.queue(barracks), "trainRHastatus") == 6);
  CHECK(f.queue(barracks)->entries[0].id == first);
  CHECK(f.queue(barracks)->entries[0].script != script::kNoScript);

  // Ctrl on a row that is not a train row repeats nothing.
  CHECK(f.bar->flags(0, "trainRSentry", ctrl).repeat == 5);  // a train row, refused below
  CHECK(f.bar->press(0, "trainRSentry", ctrl) == CommandBar::Press::disabled);
  f.select({1});
  CHECK(f.bar->flags(0, "stand_position", ctrl).repeat == 1);
  CHECK(f.bar->press(0, "stand_position", ctrl) == CommandBar::Press::issued);
  CHECK(named(f.queue(1), "stand_position") == 1);

  // The order a peer applies carries the count, not the key: a repeat of 3
  // is three, whatever this peer's table says.
  CommandBarSink sink(*f.session, *f.bar);
  NetOrder order;
  order.issuer = 0;
  order.kind = NetOrderKind::command;
  order.command = "trainRHastatus";
  order.actors = {barracks};
  order.modifier = true;
  order.repeat = 3;
  CHECK(sink.command(order));
  CHECK(named(f.queue(barracks), "trainRHastatus") == 9);

  // A table without the key reads zero, and zero runs nothing (the out value
  // 0x005e3a91 reads into starts at zero).
  env->set_constant("TrainMultipleCount", 0);
  f.select({barracks});
  CHECK(f.bar->flags(0, "trainRHastatus", ctrl).repeat == 0);
  CHECK(f.bar->press(0, "trainRHastatus", ctrl) == CommandBar::Press::issued);
  CHECK(named(f.queue(barracks), "trainRHastatus") == 9);

  // A dispatch row is dispatched that many times: each is the whole
  // execution again.
  env->set_constant("TrainMultipleCount", 4);
  const ObjectId arena = 8;
  CommandSystem* commands = command_system(world);
  REQUIRE(commands != nullptr);
  f.select({arena});
  const std::size_t idles = commands->command_count(arena, "idle");
  CHECK(f.bar->press(0, "hireGladiator", ctrl) == CommandBar::Press::issued);
  f.session->advance(1, 400);
  CHECK(commands->command_count(arena, "idle") >= idles + 4);
}

TEST(the_queue_strip_names_each_cell_by_its_command_and_a_cancel_takes_that_one) {
  // The `BuildingQueue` keeps each entry's command id (`[item+0x28]`), which
  // its click posts in a `CVXCmdCancelCmd` (0x006bf7c0); the execution finds
  // the command by that id (0x004e63c0), so the cell clicked is the command
  // cancelled however the queue moved in between.
  Fixture f;
  const ObjectId barracks = 3;
  f.select({barracks});
  for (int i = 0; i < 3; ++i) {
    CHECK(f.bar->press(0, "trainRHastatus", CommandBar::Keys{}) == CommandBar::Press::issued);
  }
  f.session->advance(1, 400);
  InfoBar infobar(*f.session, {}, {});
  SelectionInfo info = infobar.describe(0);
  const CommandQueue* q = f.queue(barracks);
  REQUIRE(q != nullptr);
  REQUIRE(info.queue.size() == 3);
  REQUIRE(q->entries.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) CHECK(info.queue[i].command == q->entries[i].id);
  CHECK(info.queue[0].frame == InfoCell::Frame::train);
  CHECK(info.queue[1].frame == InfoCell::Frame::wait);

  // The second cell, through the stream.
  const std::uint32_t second = info.queue[1].command;
  NetTurn turn;
  NetOrder cancel;
  cancel.kind = NetOrderKind::cancel_command;
  cancel.issuer = 0;
  cancel.target.object = barracks;
  cancel.command_id = second;
  turn.orders = {cancel};
  CHECK(apply_turn(f.session->world(), turn).cancels == 1);
  info = infobar.describe(0);
  REQUIRE(info.queue.size() == 2);
  CHECK(info.queue[0].command == q->entries[0].id);
  CHECK(info.queue[1].command == q->entries[1].id);
  CHECK(info.queue[0].command != second && info.queue[1].command != second);
}

TEST(issue_order_never_replaces_with_a_train_row) {
  // The mask is the per-object issue's too (0x004efbbb), so an order that
  // reaches it asking to replace -- a right click, a lockstep order -- still
  // queues a train row behind what is running.
  Fixture f;
  World& world = f.session->world();
  CommandSystem* commands = command_system(world);
  REQUIRE(commands != nullptr);
  const CommandDef* train = commands->table().find("trainRHastatus");
  const CommandDef* stand = commands->table().find("stand_position");
  REQUIRE(train != nullptr && stand != nullptr);
  const ObjectId barracks = 3;
  CHECK(issue_order(world, barracks, *train, OrderTarget{}, OrderMode::replace) != 0);
  CHECK(issue_order(world, barracks, *train, OrderTarget{}, OrderMode::replace) != 0);
  CHECK(commands->command_count(barracks, "train") == 2);
  // Any other row still replaces.
  CHECK(issue_order(world, barracks, *stand, OrderTarget{}, OrderMode::replace) != 0);
  CHECK(commands->command_count(barracks) == 1);
}

// --------------------------------------------------------------------------
// the object's own predicate: repair on a ruin, and only there
// --------------------------------------------------------------------------

namespace {

std::vector<std::string> names_of(const std::vector<CommandButton>& buttons) {
  std::vector<std::string> out;
  for (const CommandButton& button : buttons) out.push_back(button.name);
  return out;
}

bool offers(const std::vector<CommandButton>& buttons, std::string_view name) {
  for (const CommandButton& button : buttons) {
    if (button.name == name) return true;
  }
  return false;
}

}  // namespace

TEST(cmdbar_offers_repair_on_a_ruin_and_nowhere_else) {
  // Playtest #2: "Ripara" lit on a fort at 5000/5000. The row's verifier never
  // asks about health; the building's `vtbl+0xd8` (0x004de580) does, and the
  // bar's walk (0x005e787b) leaves off a row it refuses.
  Fixture f;
  World& world = f.session->world();
  const ObjectId fort = 6;
  REQUIRE(world.find(fort) != nullptr);
  REQUIRE(world.find(fort)->state.flags.is_building);
  f.select({fort});

  world.set_health(fort, 5000);  // full health: tier 0
  REQUIRE(world.find(fort)->state.damage_state == 0);
  std::vector<CommandButton> buttons = f.bar->describe(0);
  CHECK(!offers(buttons, "repair townhall"));
  CHECK(offers(buttons, "unitsout"));
  CHECK(f.bar->check(0, "repair townhall") == CommandBar::Press::unknown);
  CHECK(f.bar->press(0, "repair townhall", CommandBar::Keys{}) == CommandBar::Press::unknown);

  // Damaged but standing -- tiers 1 and 2 -- is still not a ruin, and
  // `RRepair` would do nothing to it: no Repair.
  world.set_health(fort, 3000);  // 60%
  REQUIRE(world.find(fort)->state.damage_state == 1);
  CHECK(!offers(f.bar->describe(0), "repair townhall"));
  world.set_health(fort, 1500);  // 30%
  REQUIRE(world.find(fort)->state.damage_state == 2);
  CHECK(!offers(f.bar->describe(0), "repair townhall"));

  // Broken: Repair, lit (its verifier passes), and nothing else.
  world.set_health(fort, 0);
  REQUIRE(world.find(fort)->state.damage_state == 3);
  buttons = f.bar->describe(0);
  REQUIRE(names_of(buttons) == std::vector<std::string>{"repair townhall"});
  CHECK(buttons[0].enabled);
  CHECK(buttons[0].cost_gold == 500);
  CHECK(buttons[0].key == "r");

  // Pressed, it runs; while it is the running command it is not offered again.
  CHECK(f.bar->press(0, "repair townhall", CommandBar::Keys{}) == CommandBar::Press::issued);
  CommandSystem* commands = command_system(world);
  REQUIRE(commands != nullptr);
  REQUIRE(commands->command_name(fort, 0) == "repair");
  REQUIRE(world.find(fort)->state.damage_state == 3);
  CHECK(f.bar->describe(0).empty());
  // Ended while still a ruin, it is offered again.
  CHECK(commands->kill_command(world, fort));
  CHECK(names_of(f.bar->describe(0)) == std::vector<std::string>{"repair townhall"});

  // Back on its feet, the row goes and the others come back.
  world.set_health(fort, 5000);
  buttons = f.bar->describe(0);
  CHECK(!offers(buttons, "repair townhall"));
  CHECK(offers(buttons, "unitsout"));

  // The clause is the building family's virtual, not every object's: a
  // hastatus's predicate does not refuse a `method="repair"` row -- whether
  // its class offers it is the `<src>` test's business, not this one's.
  CHECK(commands->command_enabled(world, 1, "repair townhall"));
}

TEST(cmdbar_offers_a_village_ruin_its_repopulate_and_not_repair) {
  // `auto_repair="yes"` -- `[class+0x2ec]` -- turns the ruin's one verb into
  // `repopulate`. Standing, the house is not refused `repopulate` by the
  // predicate: the shipped row's verifier asks for a broken village itself.
  Fixture f;
  World& world = f.session->world();
  const ObjectId house = 7;
  REQUIRE(world.find(house) != nullptr);
  f.select({house});
  world.set_health(house, 5000);
  std::vector<CommandButton> buttons = f.bar->describe(0);
  CHECK(offers(buttons, "repair village"));
  CHECK(offers(buttons, "unitsout"));
  world.set_health(house, 0);
  REQUIRE(world.find(house)->state.damage_state == 3);
  CHECK(names_of(f.bar->describe(0)) == std::vector<std::string>{"repair village"});

  // A fort and a house, both ruins: each refuses the other's verb, so the
  // intersection is empty.
  world.set_health(6, 0);
  f.select({6, house});
  CHECK(f.bar->describe(0).empty());
}

TEST(cmdbar_leaves_off_a_row_cmddisable_took_away) {
  // The same walk asks the other half of the predicate -- the by-name list
  // `Obj::CmdDisable` writes -- so a row a mission script took off an object
  // is gone from its bar, as it is from `GetCanExecCmd`.
  Fixture f;
  f.select({1});
  CommandSystem* commands = command_system(f.session->world());
  REQUIRE(commands != nullptr);
  REQUIRE(offers(f.bar->describe(0), "stand_position"));
  CHECK(commands->disable_command(f.session->world(), 1, "stand_position"));
  CHECK(names_of(f.bar->describe(0)) == std::vector<std::string>{"move"});
  CHECK(f.bar->press(0, "stand_position", CommandBar::Keys{}) == CommandBar::Press::unknown);
}

// --------------------------------------------------------------------------
// the lockstep sink
// --------------------------------------------------------------------------

namespace {

std::size_t moves_queued(Fixture& f, ObjectId id) {
  const CommandQueue* q = f.queue(id);
  if (q == nullptr) return 0;
  std::size_t moves = 0;
  for (const Command& command : q->entries) moves += command.name == "move" ? 1 : 0;
  return moves;
}

NetOrder aimed_move(PlayerId issuer, std::vector<ObjectId> actors) {
  NetOrder order;
  order.issuer = issuer;
  order.kind = NetOrderKind::command;
  order.command = "move";
  order.aimed = true;
  order.actors = std::move(actors);
  order.target.point = Point{700, 800};
  return order;
}

}  // namespace

TEST(check_answers_what_press_would_and_issues_nothing) {
  Fixture f;
  f.select({3});
  const auto entries = [&f](ObjectId id) {
    const CommandQueue* q = f.queue(id);
    return q == nullptr ? std::size_t{0} : q->entries.size();
  };
  const std::size_t before = entries(3);
  CHECK(f.bar->check(0, "trainRHastatus") == CommandBar::Press::issued);
  CHECK(f.bar->check(0, "trainRSentry") == CommandBar::Press::disabled);
  CHECK(f.bar->check(0, "move") == CommandBar::Press::unknown);
  CHECK(entries(3) == before);
  f.select({1, 4});
  CHECK(f.bar->check(0, "move") == CommandBar::Press::waiting);
  CHECK(moves_queued(f, 1) == 0);
}

TEST(the_sink_applies_a_row_over_the_actors_the_order_names_not_the_selection) {
  Fixture f;
  f.select({});  // the applying peer has nothing selected: it is not the issuer
  CommandBarSink sink(*f.session, *f.bar);
  CHECK(sink.command(aimed_move(0, {1, 4})));
  CHECK(moves_queued(f, 1) == 1);
  CHECK(moves_queued(f, 4) == 1);
  CHECK(moves_queued(f, 0) == 0);  // selected by nobody, named by nobody
}

TEST(the_sink_commands_nothing_its_issuer_may_not_command) {
  // Player 5 names player 0's units. A peer that says it selected another
  // player's army has not thereby been given it.
  Fixture f;
  CommandBarSink sink(*f.session, *f.bar);
  CHECK(!sink.command(aimed_move(5, {1, 4})));
  CHECK(moves_queued(f, 1) == 0);
  CHECK(moves_queued(f, 4) == 0);

  // A mixed list keeps only what the issuer controls -- here, nothing of 5's
  // exists, so a list with one of 0's units in it for issuer 0 goes through.
  CHECK(sink.command(aimed_move(0, {1, 999})));
  CHECK(moves_queued(f, 1) == 1);
}

TEST(the_sink_does_nothing_for_a_targeted_row_that_arrives_pressed) {
  Fixture f;
  CommandBarSink sink(*f.session, *f.bar);
  NetOrder pressed = aimed_move(0, {1});
  pressed.aimed = false;
  CHECK(!sink.command(pressed));
  CHECK(moves_queued(f, 1) == 0);
}

TEST(the_sink_surrenders_through_the_match) {
  Fixture f;
  CommandBarSink sink(*f.session, *f.bar);
  CHECK(!sink.surrender(imperivm::core::kNoPlayer));
  // Exactly what the menu's direct call does, on a twin: same outcome, same
  // world. (Whether it ends anything is the match's rule, not the sink's --
  // this fixture's player 0 does not participate, so it ends nothing.)
  Fixture twin;
  twin.session->declare_match(0, /*lost=*/true);
  CHECK(sink.surrender(0));
  const MatchSystem* mine = match_system_of(f.session->world());
  const MatchSystem* theirs = match_system_of(twin.session->world());
  REQUIRE(mine != nullptr && theirs != nullptr);
  CHECK(mine->outcome(0) == theirs->outcome(0));
  f.session->advance(1, 400);
  twin.session->advance(1, 400);
  CHECK(f.session->world().hashes().hash_of_hashes == twin.session->world().hashes().hash_of_hashes);
}

TEST(the_sink_hands_a_departed_seat_to_the_computer) {
  // The original's drop routine (0x00406840) starts an AI for the seat, as
  // `AIStart` does (0x00434ca0). A human seat has none until then.
  Fixture f;
  (void)f.session->start_ai();  // compiles the profile's scripts; no computer here
  const AiSystem* ai = ai_system_of(f.session->world());
  REQUIRE(ai != nullptr);
  REQUIRE(ai->player_ai(2) != nullptr);
  CHECK(!ai->player_ai(2)->active);
  CommandBarSink sink(*f.session);  // a headless peer: no bar needed
  CHECK(sink.take_over(2));
  CHECK(ai->player_ai(2)->active);
  CHECK(!sink.take_over(imperivm::core::kNoPlayer));
  // A late joiner takes the seat back: the computer stops (AIStop's body).
  CHECK(sink.hand_back(2));
  CHECK(!ai->player_ai(2)->active);
  CHECK(!sink.hand_back(2));  // nothing left to stop
  CHECK(!sink.hand_back(imperivm::core::kNoPlayer));
  // And no command row without a bar.
  CHECK(!sink.command(aimed_move(0, {1})));
}
