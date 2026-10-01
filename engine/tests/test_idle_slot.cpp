// An object's `idle` is its command slot's resting occupant: an order takes the
// slot, and the idle does not go on running beside it.
//
// `gbr.exe` keeps one script per command slot. When the running command ends,
// 0x005b07d0 starts the next through 0x005b4f20, and a queue that has nothing
// left gets the class's `idle` method pushed as a command (0x005aef20 finds
// the method, 0x005b4960 queues it) whose name becomes the object's running
// command at `obj+0x10c`. The desync dumps agree: every live object's queue
// holds at least one entry, and the only empty ones are the dying units
// (`docs/engine/state-vector.md`). So an idle never runs beside an order.
//
// This engine starts an object's first idle straight on the scheduler, from
// `GameSession::start_object_scripts`, outside the queue -- and it went on
// running after the object was ordered. Playtest #12 is what that looked like:
// the shipped unit idle stops its unit every four seconds or so and stands
// idle for two, so a legionary walking a right-click move halted, and the move
// script only laid the route again when its own two-second slice ran out.
// The classes and scripts below are invented; what they exercise is the shape.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "builder.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace script = imperivm::core::script;

using imperivm::core::ClassGraph;
using imperivm::core::ClassIndex;
using imperivm::core::kNoClass;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Two soldiers in play, far apart, on a map with nothing else on it.
constexpr std::string_view kMap = R"(<mapobject>
	<scriptobj
		class="Soldier"
		num="0"
	player="1"
	x="1000"
	y="1000"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Soldier"
		num="1"
	player="1"
	x="3000"
	y="3000"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
</mapobject>
)";

ClassGraph soldier_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "test_idle_slot.cpp");
  graph.add(bytes_of(R"(<class id="Soldier" parent="Object" cpp_class="CVXUnit">
      <properties sight="100" maxhealth="50" maxstamina="10" speed="60" radius="15"/>
      <behavior script="watch.vs"/>
      <method sig="idle" vs="idle.vs"/>
      <method sig="move" vs="move.vs"/>
      <method sig="hold" vs="hold.vs"/>
    </class>)"),
            "test_idle_slot.cpp");
  graph.link();
  return graph;
}

class Scripts final : public ScriptResolver {
 public:
  void add(std::string path, std::string text) {
    files_.emplace_back(std::move(path), std::move(text));
  }
  std::span<const std::byte> source(std::string_view path) override {
    for (const auto& [name, body] : files_) {
      if (name == path) return bytes_of(body);
    }
    return {};
  }

 private:
  std::vector<std::pair<std::string, std::string>> files_;
};

/// A behaviour that only waits. An idle of the shipped kind: it halts its
/// unit, waits, and loops -- a halt about every four seconds, for as long as it
/// runs. A move of the shipped kind: one `Goto` with a two-second slice, asked
/// again until it arrives.
Scripts soldier_scripts() {
  Scripts scripts;
  scripts.add("idle.vs",
              "//void, Obj This\nUnit u;\nu = This.AsUnit();\n"
              "while (1) {\n  if (u.Stop(2000)) u.Idle(1950);\n}\n");
  scripts.add("move.vs",
              "//void, Obj This, point pt\nUnit u;\nu = This.AsUnit();\n"
              "while (!u.Goto(pt, 0, 2000, true, 0));\n");
  scripts.add("hold.vs", "//void, Obj This\nSleep(1000000);\n");
  scripts.add("watch.vs", "//void, Obj This\nSleep(1000000);\n");
  return scripts;
}

/// A passability layer in the shipped `GRID` layout with nothing blocked:
/// 16-unit cells, 4,096 units a side. An empty layer is not open ground -- the
/// search finds no route across it -- so the walk needs one.
std::vector<std::byte> open_ground() {
  constexpr std::uint32_t kCell = 16;
  constexpr std::uint32_t kCells = 256;
  imperivm::test::Builder out;
  out.text(imperivm::core::kGridMagic).u32(kCell).u32(1).u32(kCell * kCells).u32(kCell * kCells);
  for (std::uint32_t i = 0; i < kCells * kCells / 8; ++i) out.u8(0);
  return {out.span().begin(), out.span().end()};
}

struct Fixture {
  script::HostRegistry registry;
  ClassGraph graph = soldier_graph();
  Scripts scripts = soldier_scripts();
  std::vector<std::byte> grid = open_ground();
  SessionInputs inputs;
  Fixture() {
    register_all_hosts(registry);
    inputs.classes = &graph;
    inputs.scripts = &scripts;
    inputs.map_objects = bytes_of(kMap);
    inputs.passability = grid;
  }
};

/// The live scripts `id` owns, by file, in id order.
std::vector<std::string> owned(GameSession& run, ObjectId id) {
  std::vector<std::string> files;
  for (const script::ScriptRecord& record : run.scheduler().scripts()) {
    if (record.dead || record.owner.id != id || record.owner.type != kTypeObj) continue;
    if (record.execution.status == script::ExecStatus::finished ||
        record.execution.status == script::ExecStatus::failed) {
      continue;
    }
    files.push_back(run.scheduler().chunk(record.chunk_index).source_name);
  }
  return files;
}

Command move_to(Point where) {
  Command command;
  command.arg_kind = CommandArgKind::point;
  command.point = where;
  return command;
}

}  // namespace

TEST(an_order_takes_the_slot_of_the_idle_a_unit_was_started_with) {
  Fixture f;
  auto session = GameSession::create(f.registry, f.inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();
  REQUIRE(run.start_object_scripts() == 4);
  run.advance(1, 100);
  const std::vector<std::string> resting = {"watch.vs", "idle.vs"};
  CHECK(owned(run, 1) == resting);

  CommandSystem* commands = command_system(run.world());
  REQUIRE(commands != nullptr);
  REQUIRE(commands->set_command(run.world(), 1, "hold", Command{}) != 0);
  run.advance(1, 100);
  // The order holds the slot the idle held. The behaviour is not the slot's,
  // and the other soldier's idle is not this one's.
  const std::vector<std::string> ordered = {"watch.vs", "hold.vs"};
  CHECK(owned(run, 1) == ordered);
  CHECK(commands->command_name(1) == "hold");
  CHECK(owned(run, 2) == resting);
}

TEST(an_order_to_idle_leaves_one_idle_not_two) {
  Fixture f;
  auto session = GameSession::create(f.registry, f.inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();
  REQUIRE(run.start_object_scripts() == 4);
  run.advance(1, 100);

  // `SetCommand("idle")`, as `hero.army.SetCommand("idle")` sends it: the new
  // idle is the command, and it is not ended with the one it replaces.
  CommandSystem* commands = command_system(run.world());
  REQUIRE(commands != nullptr);
  REQUIRE(commands->set_command(run.world(), 1, "idle", Command{}) != 0);
  run.advance(2, 100);
  const std::vector<std::string> resting = {"watch.vs", "idle.vs"};
  CHECK(owned(run, 1) == resting);
  CHECK(commands->command_name(1) == "idle");
}

TEST(a_unit_ordered_before_its_first_turn_gets_no_idle_beside_the_order) {
  Fixture f;
  auto session = GameSession::create(f.registry, f.inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();
  REQUIRE(run.start_object_scripts() == 4);
  run.advance(1, 100);

  // A unit made mid-match and handed an order in the same pass -- a trained
  // soldier sent to its rally point -- starts on the order. Its idle comes when
  // the queue runs dry, as the command slot's default, and not before.
  const ClassIndex soldier = f.graph.find("Soldier");
  REQUIRE(soldier != kNoClass);
  const ObjectId made = run.world().spawn_of_class(soldier);
  REQUIRE(made != kNoObject);
  REQUIRE(run.world().set_position(made, Point{2000, 2000}));
  CommandSystem* commands = command_system(run.world());
  REQUIRE(commands != nullptr);
  REQUIRE(commands->add_command(run.world(), made, /*front=*/false, "move",
                                move_to(Point{2100, 2000})) != 0);
  run.advance(1, 100);
  run.advance(1, 100);
  // The order launched in the command pass, ahead of the sweep that started
  // the behaviour: script ids are in that order.
  const std::vector<std::string> moving = {"move.vs", "watch.vs"};
  CHECK(owned(run, made) == moving);

  // A hundred units at sixty a second: there in two, and then one idle.
  run.advance(40, 100);
  const std::vector<std::string> resting = {"watch.vs", "idle.vs"};
  CHECK(owned(run, made) == resting);
  CHECK(run.world().resolve_position(made) == (Point{2100, 2000}));
  CHECK(commands->command_name(made) == "idle");
}

TEST(a_walk_ordered_over_a_standing_idle_never_halts_on_open_ground) {
  // The app's 100 ms turn and `imrun`'s 800 ms one.
  for (const std::int32_t length : {100, 800}) {
    Fixture f;
    auto session = GameSession::create(f.registry, f.inputs, /*seed=*/1);
    REQUIRE(session.ok());
    GameSession& run = *session.value();
    REQUIRE(run.start_object_scripts() == 4);
    // Long enough for the idle to be deep in its loop, as a unit that has
    // stood a while is.
    run.advance(3000 / length, length);

    CommandSystem* commands = command_system(run.world());
    REQUIRE(commands != nullptr);
    REQUIRE(commands->set_command(run.world(), 1, "move", move_to(Point{1000, 1900})) != 0);

    // Fifteen seconds of a 900-unit walk at sixty a second: every turn until
    // arrival moves the unit, so there is no stretch of standing. The
    // playtest's was about ten 100 ms turns in every forty.
    Point last = run.world().resolve_position(1);
    std::int32_t still = 0;
    std::int32_t longest = 0;
    std::int32_t moved = 0;
    for (std::int32_t turn = 0; turn < 20000 / length; ++turn) {
      run.advance(1, length);
      const Point now = run.world().resolve_position(1);
      if (now == Point{1000, 1900}) break;
      if (now == last) {
        ++still;
        if (still > longest) longest = still;
      } else {
        still = 0;
        ++moved;
      }
      last = now;
    }
    CHECK(run.world().resolve_position(1) == (Point{1000, 1900}));
    // The first turn launches the order and lays the route; every one after
    // it steps.
    CHECK(longest <= 1);
    CHECK(moved >= 14000 / length);
  }
}
