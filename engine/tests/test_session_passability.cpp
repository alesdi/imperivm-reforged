// The passability layer at match start: rebuilt over the world as it stands
// -- the original's 0x00552e95 once the templates have landed -- from the
// terrain's rules and every object's mask, made into the grid units walk on,
// and carried by the save. See `SessionInputs::masks` and
// `core/world/editor.hpp`, "passability".

#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/world/editor.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

constexpr std::string_view kClasses[] = {
    R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="1000"/></class>)",
    R"(<class id="House" parent="Building" cpp_class="CVXBuilding" entity="Buildings/House/House.ent.xml"/>)",
    R"(<class id="Hut" parent="Building" cpp_class="CVXBuilding" entity_spring="Buildings/Hut/Hut.ent.xml"/>)",
    R"(<class id="Unit" cpp_class="CVXUnit"><properties maxhealth="100"/></class>)",
};

/// A house at (2000, 2000), a hut -- whose entity is the spring one -- at
/// (3000, 3000), and a unit, which has no mask.
constexpr std::string_view kMap = R"(<mapobject>
<scriptobj class="House" num="0" player="1" healthperc="100" x="2000" y="2000" flags="0x80800001"/>
<scriptobj class="Hut" num="1" player="1" healthperc="100" x="3000" y="3000" flags="0x80800001"/>
<scriptobj class="Unit" num="2" player="1" healthperc="100" x="2500" y="2500" flags="0x80800001"/>
</mapobject>)";
constexpr std::string_view kMapXml = R"(<map name="Test"><size x="4096" y="4096"/></map>)";
constexpr std::string_view kGame = R"(<game><properties season="spring" start_map="1"/></game>)";
constexpr std::string_view kTerrains =
    "<terrain><layer z=\"3\" type=\"1\" display=\"Grass 1\" image=\"g.vq\"/>"
    "<layer z=\"6\" type=\"5\" display=\"Rocks 1\" passable=\"0\" image=\"r.vq\"/>"
    "<layer z=\"13\" type=\"4\" display=\"Deep\" passable_water=\"1\" image=\"d.vq\"/></terrain>";

/// Two entities, each naming a mask; the masks are one and two cells at
/// the grid's centre.
class Art final : public EntityResolver, public PassMaskResolver {
 public:
  Art() {
    add("Buildings/House/House.ent.xml", "House.pass", {{64, 64}, {65, 64}});
    add("Buildings/Hut/Hut.ent.xml", "Hut.pass", {{64, 64}});
  }
  const Entity* resolve(std::string_view path) const override {
    const auto found = entities_.find(std::string(path));
    return found == entities_.end() ? nullptr : found->second.get();
  }
  const edit::PassMask* mask_of(const Entity* entity) override {
    if (entity == nullptr) return nullptr;
    const auto found = masks_.find(entity->pass_file());
    return found == masks_.end() ? nullptr : &found->second.mask;
  }
  std::size_t asked = 0;

 private:
  struct Mask {
    OwnedGrid grid;
    edit::PassMask mask;
  };
  void add(std::string_view path, std::string_view pass, std::vector<std::pair<int, int>> cells) {
    const std::string xml = "<entity name=\"\" type=\"\" variations=\"1\" pass_file=\"" + std::string(pass) +
                            "\"><images/><layers/><states><state idx=\"1\" name=\"idle\"/></states></entity>";
    auto entity = Entity::parse(bytes_of(xml), path);
    REQUIRE(entity.ok());
    entities_[std::string(path)] = std::make_unique<Entity>(std::move(entity.value()));
    auto grid = OwnedGrid::create(16, 1, 2048, 2048);
    REQUIRE(grid.ok());
    for (const auto& [x, y] : cells) (void)grid->grid().set_cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), 1);
    Mask& slot = masks_[std::string(pass)];
    slot.grid = std::move(grid.value());
    slot.mask.grid = &slot.grid.grid();
    slot.mask.bounds = edit::mask_bounds(slot.grid.grid());
  }
  std::map<std::string, std::unique_ptr<Entity>> entities_;
  std::map<std::string, Mask> masks_;
};

struct Bench {
  ClassGraph graph;
  script::HostRegistry registry;
  Art art;
  OwnedGrid terrain;
  OwnedGrid pass;
  std::unique_ptr<GameSession> session;

  explicit Bench(bool with_masks = true) {
    const char* names[] = {"a.sc.xml", "b.sc.xml", "c.sc.xml", "d.sc.xml"};
    for (std::size_t i = 0; i < std::size(kClasses); ++i) REQUIRE(graph.add(bytes_of(kClasses[i]), names[i]).ok());
    graph.link();
    (void)register_all_hosts(registry);
    auto grass = OwnedGrid::create(64, 8, 4096, 4096);
    REQUIRE(grass.ok());
    (void)grass->grid().fill(3);
    (void)grass->grid().set_cell(10, 10, 6);  // one rock
    terrain = std::move(grass.value());
    auto clear = OwnedGrid::create(16, 1, 4096, 4096);
    REQUIRE(clear.ok());
    pass = std::move(clear.value());
    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.map_objects = bytes_of(kMap);
    inputs.map_properties = bytes_of(kMapXml);
    inputs.game = bytes_of(kGame);
    inputs.terrain = terrain.bytes();
    inputs.passability = pass.bytes();
    inputs.entities = &art;
    if (with_masks) {
      inputs.terrain_table = bytes_of(kTerrains);
      inputs.masks = &art;
    }
    auto made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    session = std::move(made.value());
  }
  void start() {
    MatchOptions options;
    options.human = kNoPlayer;
    options.control_set[0] = true;
    options.controls[0] = PlayerControl::computer;
    options.races[0] = static_cast<std::int32_t>(Race::gaul);
    (void)session->start_match(options);
  }
  const ObstructionGrid& grid() { return movement_system(session->world())->grid(); }
};

}  // namespace

TEST(match_start_rebuilds_the_passability_layer_over_the_world) {
  Bench b;
  CHECK(b.grid().count_blocked() == 0);  // adopted as shipped: nothing
  b.start();
  const PassabilityReport& report = b.session->passability();
  CHECK(report.rebuilt);
  CHECK(report.cells_changed > 0);
  const ObstructionGrid& grid = b.grid();
  // The house's two cells: column 64 is 8 right of the anchor, 65 is 24;
  // row 64 lands on the anchor's row. Cells (125, 125) and (126, 125).
  CHECK(grid.blocked_cell(125, 125));
  CHECK(grid.blocked_cell(126, 125));
  CHECK(!grid.blocked_cell(127, 125));
  // The hut's, through its spring entity: x 3008; the scan lands its row on
  // 3008 too (the bracket's low end projects to exactly the target row).
  CHECK(grid.blocked_cell(188, 188));
  CHECK(!grid.blocked_cell(188, 187));
  // The rock at terrain cell 10: the four-by-four window sampling it.
  CHECK(grid.blocked_cell(38, 38));
  CHECK(!grid.blocked_cell(42, 38));
  // The frame: the first two columns on every row.
  CHECK(grid.blocked_cell(0, 100) && grid.blocked_cell(1, 100) && !grid.blocked_cell(2, 100));
  // A unit stamps nothing: the ground beside it is free.
  CHECK(!grid.blocked_cell(156, 156));
}

TEST(the_rebuilt_layer_travels_with_the_save) {
  Bench b;
  b.start();
  const std::uint32_t generation = movement_system(b.session->world())->grid_generation();
  const auto saved = b.session->save();
  REQUIRE(saved.ok());
  Bench again;
  REQUIRE(again.session->load(saved.value()).ok());
  CHECK(again.session->passability().rebuilt);
  const ObstructionGrid& before = b.grid();
  const ObstructionGrid& after = again.grid();
  REQUIRE(after.width() == before.width() && after.height() == before.height());
  std::size_t differing = 0;
  for (std::int32_t y = 0; y < before.height(); ++y) {
    for (std::int32_t x = 0; x < before.width(); ++x) differing += before.blocked_cell(x, y) != after.blocked_cell(x, y);
  }
  CHECK(differing == 0);
  CHECK(after.count_blocked() == before.count_blocked());
  // The grid went in ahead of the movement section, so the saved generation
  // is the restored one and no route is sent back to the pathfinder.
  CHECK(movement_system(again.session->world())->grid_generation() == generation);
}

TEST(without_its_inputs_the_layer_is_adopted_as_shipped) {
  Bench b(/*with_masks=*/false);
  b.start();
  CHECK(!b.session->passability().rebuilt);
  CHECK(b.session->passability().cells_changed == 0);
  CHECK(b.grid().count_blocked() == 0);
  // And the save carries no layer; a load of it keeps the map's.
  const auto saved = b.session->save();
  REQUIRE(saved.ok());
  Bench again(/*with_masks=*/false);
  REQUIRE(again.session->load(saved.value()).ok());
  CHECK(!again.session->passability().rebuilt);
}
