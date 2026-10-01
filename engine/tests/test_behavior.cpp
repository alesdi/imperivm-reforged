// Class `<behavior>` scripts: started on the object, owned by it, restored by
// a load, gone with the object.
//
// `gbr.exe` starts them from the object's start virtual (vtbl+0x2c,
// 0x005aec40) the moment the factory has built the object, one coroutine per
// entry of the class's behaviour list, each in its own script slot and handed
// the object. `sim/world_host.hpp`'s `start_behaviors` says the rest. The
// classes and scripts below are invented; what they exercise is the shape --
// own behaviours before inherited ones, a missing file skipped, a template
// left out, a spawned object picked up, a corpse's and an erased object's
// scripts reaped, a load that restores rather than restarts, a mutation that
// swaps one class's behaviours for the other's.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace script = imperivm::core::script;

using imperivm::core::ClassGraph;
using imperivm::core::ClassIndex;
using imperivm::core::kNoClass;
using imperivm::core::Result;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Two soldiers in play and one spawn template, all of `Leaf`.
constexpr std::string_view kMap = R"(<mapobject>
	<scriptobj
		class="Leaf"
		num="0"
	player="1"
	x="100"
	y="100"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Leaf"
		num="1"
	player="2"
	x="3000"
	y="3000"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Leaf"
		num="2"
	player="2"
	x="3100"
	y="3100"
	flags="0xA8400002"
	dir.x="0"
	dir.y="1"/>
</mapobject>
)";

/// `Base` declares one behaviour, `Leaf` two of its own -- one of them a file
/// nobody has -- and the idle. `Other` is what `Mutant` becomes.
ClassGraph behaviour_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "test_behavior.cpp");
  graph.add(bytes_of(R"(<class id="Base" parent="Object" cpp_class="CVXUnit">
      <properties sight="100" maxhealth="50" maxstamina="10"/>
      <behavior script="b_base.vs"/>
    </class>)"),
            "test_behavior.cpp");
  graph.add(bytes_of(R"(<class id="Leaf" parent="Base" cpp_class="CVXUnit">
      <behavior script="b_leaf.vs"/>
      <behavior script="b_missing.vs"/>
      <method sig="idle" vs="idle.vs"/>
    </class>)"),
            "test_behavior.cpp");
  graph.add(bytes_of(R"(<class id="Mutant" parent="Base" cpp_class="CVXUnit">
      <behavior script="b_leaf.vs"/>
      <behavior script="b_mutate.vs"/>
      <method sig="idle" vs="idle.vs"/>
    </class>)"),
            "test_behavior.cpp");
  graph.add(bytes_of(R"(<class id="Other" parent="Object" cpp_class="CVXUnit">
      <properties sight="100" maxhealth="50" maxstamina="10"/>
      <behavior script="b_other.vs"/>
    </class>)"),
            "test_behavior.cpp");
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

/// Each behaviour writes a digit into the object's `user`, so one turn says
/// which ran, in which order, and on which object.
Scripts behaviour_scripts() {
  Scripts scripts;
  scripts.add("b_leaf.vs",
              "//void, Obj This\nUnit u;\nu = This.AsUnit();\nu.SetUser(u.user * 10 + 1);\n"
              "Sleep(1000000);\n");
  scripts.add("b_base.vs",
              "//void, Obj This\nUnit u;\nu = This.AsUnit();\nu.SetUser(u.user * 10 + 2);\n"
              "Sleep(1000000);\n");
  scripts.add("b_other.vs",
              "//void, Obj This\nUnit u;\nu = This.AsUnit();\nu.SetUser(u.user * 10 + 3);\n"
              "Sleep(1000000);\n");
  scripts.add("idle.vs", "//void, Obj This\nSleep(1000000);\n");
  scripts.add("b_mutate.vs",
              "//void, Obj This\nUnit u;\nu = This.AsUnit();\nSleep(1000);\nu.Mutate(\"Other\");\n");
  return scripts;
}

struct Fixture {
  script::HostRegistry registry;
  ClassGraph graph = behaviour_graph();
  Scripts scripts = behaviour_scripts();
  SessionInputs inputs;
  Fixture() {
    register_all_hosts(registry);
    inputs.classes = &graph;
    inputs.scripts = &scripts;
    inputs.map_objects = bytes_of(kMap);
  }
};

std::string source_of(GameSession& run, const script::ScriptRecord& record) {
  return run.scheduler().chunk(record.chunk_index).source_name;
}

/// The live scripts `id` owns, by file, in id order.
std::vector<std::string> owned(GameSession& run, ObjectId id) {
  std::vector<std::string> files;
  for (const script::ScriptRecord& record : run.scheduler().scripts()) {
    if (record.dead || record.owner.id != id || record.owner.type != kTypeObj) continue;
    if (record.execution.status == script::ExecStatus::finished ||
        record.execution.status == script::ExecStatus::failed) {
      continue;
    }
    files.push_back(source_of(run, record));
  }
  return files;
}

std::int32_t user_of(GameSession& run, ObjectId id) {
  const WorldObject* slot = run.world().find(id);
  return slot == nullptr ? -1 : slot->state.user;
}

}  // namespace

TEST(behaviours_start_on_every_object_in_play_before_any_idle) {
  Fixture f;
  auto session = GameSession::create(f.registry, f.inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();

  // Two objects in play, two behaviours each that exist and an idle: six. The
  // template gets none, and neither object gets the file that is not there.
  REQUIRE(run.start_object_scripts() == 6);

  // Every object's behaviours, own before inherited, then every object's
  // idle -- in id order, which is what makes the script ids world state.
  std::vector<std::pair<ObjectId, std::string>> order;
  for (const script::ScriptRecord& record : run.scheduler().scripts()) {
    CHECK(record.owner.type == kTypeObj);
    CHECK(record.parent == script::kNoScript);
    order.emplace_back(record.owner.id, source_of(run, record));
  }
  const std::vector<std::pair<ObjectId, std::string>> expected = {
      {1, "b_leaf.vs"}, {1, "b_base.vs"}, {2, "b_leaf.vs"},
      {2, "b_base.vs"}, {1, "idle.vs"},   {2, "idle.vs"},
  };
  CHECK(order == expected);

  // One turn: each behaviour ran once, on its own object, leaf first.
  run.advance(1, 400);
  CHECK(user_of(run, 1) == 12);
  CHECK(user_of(run, 2) == 12);
  CHECK(user_of(run, 3) == 0);  // the template ran nothing

  // And nothing is started twice.
  run.advance(2, 400);
  CHECK(run.report().scripts_started == 6);
  CHECK(user_of(run, 1) == 12);
}

TEST(a_spawned_object_gets_its_behaviours_and_loses_them_when_it_goes) {
  Fixture f;
  auto session = GameSession::create(f.registry, f.inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();
  REQUIRE(run.start_object_scripts() == 6);
  run.advance(1, 400);

  const ObjectId copy = run.world().spawn_from_template(static_cast<ObjectId>(3));
  REQUIRE(copy != kNoObject);
  run.advance(1, 400);
  CHECK(run.report().scripts_started == 9);
  const std::vector<std::string> mine = {"b_leaf.vs", "b_base.vs", "idle.vs"};
  CHECK(owned(run, copy) == mine);
  CHECK(user_of(run, copy) == 12);

  // Gone from the world: every coroutine it owned goes with it, behaviours
  // included, on the next turn's reap.
  REQUIRE(run.world().despawn(copy));
  run.advance(1, 400);
  CHECK(owned(run, copy).empty());
  // The others are untouched.
  CHECK(owned(run, 1).size() == 3);
}

TEST(a_load_restores_behaviours_rather_than_starting_them_again) {
  Fixture f;
  auto session = GameSession::create(f.registry, f.inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();
  REQUIRE(run.start_object_scripts() == 6);
  run.advance(2, 400);
  const Result<std::vector<std::byte>> saved = run.save("test");
  REQUIRE(saved.ok());

  auto fresh = GameSession::create(f.registry, f.inputs, /*seed=*/1);
  REQUIRE(fresh.ok());
  GameSession& restored = *fresh.value();
  REQUIRE(restored.start_object_scripts() == 6);
  REQUIRE(restored.load(saved.value(), "test").ok());

  // The saved coroutines, not a second set: the same files on the same
  // objects, and a turn after the load starts nothing and runs no behaviour
  // from its top -- which would have written another digit.
  CHECK(owned(restored, 1) == owned(run, 1));
  CHECK(owned(restored, 2) == owned(run, 2));
  CHECK(restored.scheduler().live_count() == run.scheduler().live_count());
  const std::size_t started = restored.report().scripts_started;
  run.advance(1, 400);
  restored.advance(1, 400);
  CHECK(restored.report().scripts_started == started);
  CHECK(user_of(restored, 1) == 12);
  CHECK(restored.scheduler().live_count() == run.scheduler().live_count());
}

TEST(a_mutation_swaps_one_classs_behaviours_for_the_others) {
  Fixture f;
  auto session = GameSession::create(f.registry, f.inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();
  const ClassIndex mutant = f.graph.find("Mutant");
  REQUIRE(mutant != kNoClass);
  const ObjectId id = run.world().spawn_of_class(mutant);
  REQUIRE(id != kNoObject);
  REQUIRE(run.start_object_scripts() == 6 + 4);
  run.advance(1, 400);
  const std::vector<std::string> before = {"b_leaf.vs", "b_mutate.vs", "b_base.vs", "idle.vs"};
  CHECK(owned(run, id) == before);
  CHECK(user_of(run, id) == 12);

  // `b_mutate.vs` wakes after a second and makes it an `Other`: `Mutant`'s
  // behaviours stop -- the one that asked with them -- and `Other`'s starts.
  // The idle is not a behaviour and is not theirs to stop. `user` does not
  // survive a mutation in the original, and the new behaviour's digit lands
  // on whatever is there.
  run.advance(4, 400);
  const WorldObject* slot = run.world().find(id);
  REQUIRE(slot != nullptr);
  CHECK(slot->class_index == f.graph.find("Other"));
  const std::vector<std::string> after = {"idle.vs", "b_other.vs"};
  CHECK(owned(run, id) == after);
  CHECK(user_of(run, id) % 10 == 3);
  // Nobody else's behaviours were touched.
  CHECK(owned(run, 1).size() == 3);
}
