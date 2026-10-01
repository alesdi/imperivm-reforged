// What a session has to have compiled before a save can be bound to it.
//
// `Scheduler::deserialize` rebinds every restored coroutine by source name
// against the library it is handed, and refuses the whole load with
// `not_found` rather than resume the wrong script. So a load has two jobs in
// order: build the library, then bind. `GameSession::prime_library` is the
// first one, and this file is about what it has to put in.
//
// **It used to enumerate the ways a script can start, and an enumeration is a
// claim somebody has to maintain.** The victory condition, the AI profile's
// `[Scripts]` manifest and each class's `idle` binding were the three; the
// class hooks made a fourth; and the one nobody added was
// `CommandSystem::launch`, which has compiled a class `<method>` through the
// library since the day it learned to. `Balcans` and the shipped conquest
// failed every save round trip on four wildlife verbs -- `deer_move`,
// `eagle_move`, `lion_lead` and `wolf_lead` -- named by no manifest anywhere,
// and `tests/test_corpus_imsave.py` had been reporting exactly that for as
// long as those two maps have had wolves on them.
//
// The list that cannot be short is the one the writer wrote, so the load reads
// it out of the save's own script section. `test_vs_vm.cpp` covers the reader;
// this covers the session using it, over the path that was actually broken.
//
// The other shape -- a running script whose *class* has no instances left, so
// that no walk over the world could name its file however many sources it
// enumerated -- cannot be built with a command verb, because a command script
// is spawned as its object's own coroutine and `Erase` sweeps it. It is built
// with a class hook instead, which is spawned detached; see
// `test_hooks.cpp`.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace script = imperivm::core::script;

using imperivm::core::ClassGraph;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// One unit, in play.
constexpr std::string_view kMapDocument = R"(<mapobject>
	<scriptobj
		class="RHastatus"
		num="0"
	player="1"
	x="1000"
	y="1000"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
</mapobject>
)";

/// `idle` is the binding every manifest already knew about. `move` is the one
/// only the command pump can reach -- a verb a script or a player asks for,
/// resolved out of the class's own method table at the moment it is asked.
ClassGraph command_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "test_session_library.cpp");
  graph.add(bytes_of(R"(<class id="RHastatus" parent="Object" cpp_class="CVXUnit">
      <properties sight="400" maxhealth="200" maxstamina="10"/>
      <method sig="idle" vs="unit_idle.vs"/>
      <method sig="move" vs="unit_move.vs"/>
    </class>)"),
            "test_session_library.cpp");
  graph.link();
  return graph;
}

class Scripts final : public ScriptResolver {
 public:
  void add(std::string path, std::string text) {
    files_.emplace_back(std::move(path), std::move(text));
  }
  std::span<const std::byte> source(std::string_view path) override {
    for (const auto& [name, text] : files_) {
      if (name == path) return bytes_of(text);
    }
    return {};
  }

 private:
  std::vector<std::pair<std::string, std::string>> files_;
};

void add_scripts(Scripts& scripts) {
  scripts.add("unit_idle.vs", "//void, Obj This\nSleep(100000);\n");
  // Long enough that it is still suspended when the save is taken, which is
  // what puts its file in the script section.
  scripts.add("unit_move.vs", "//void, Obj This\nSleep(100000);\n");
}

[[nodiscard]] imperivm::core::Result<std::unique_ptr<GameSession>> make(
    script::HostRegistry& registry, ClassGraph& graph, Scripts& scripts) {
  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kMapDocument);
  return GameSession::create(registry, inputs, /*seed=*/1);
}

constexpr ObjectId kUnit = 1;

}  // namespace

TEST(a_save_binds_when_the_only_thing_running_is_a_command_verb) {
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = command_graph();
  Scripts scripts;
  add_scripts(scripts);

  imperivm::core::Result<std::unique_ptr<GameSession>> made = make(registry, graph, scripts);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  CommandSystem* commands = command_system(run.world());
  REQUIRE(commands != nullptr);
  Command prototype;
  REQUIRE(commands->set_command(run.world(), kUnit, "move", prototype) != 0);
  run.advance(1, 400);

  // The verb's script is running, and nothing but the command pump could have
  // put it in the library: `move` is in no `[Scripts]` manifest, it is not the
  // victory condition, and it is not `idle`.
  const script::Scheduler& scheduler = run.scheduler();
  bool running_move = false;
  for (const script::ScriptRecord& record : scheduler.scripts()) {
    if (record.dead) continue;
    if (scheduler.chunk(record.chunk_index).source_name == "unit_move.vs") running_move = true;
  }
  REQUIRE(running_move);

  const imperivm::core::Result<std::vector<std::byte>> saved = run.save();
  REQUIRE(saved.ok());

  // A fresh session over the same game data, which is the contract `load`
  // states. Object scripts are deliberately *not* started on it: a load must
  // stand on the save, not on whatever the embedder happened to do first.
  imperivm::core::Result<std::unique_ptr<GameSession>> reloaded = make(registry, graph, scripts);
  REQUIRE(reloaded.ok());
  CHECK(reloaded.value()->load(saved.value()).ok());
  CHECK(reloaded.value()->scheduler().live_count() == scheduler.live_count());
}
