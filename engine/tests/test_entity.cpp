// Entity definition tests.
//
// No game data: every document below is written out here, which is also the
// only way to cover the cases that matter most — the malformed ones, and the
// shipped defects (uninitialised offsets, a typo'd draw mode, a non-boolean
// boolean) that a strict loader would reject and the retail game runs with.
//
// The one test to read first is the frame-table one. 430 of the 4,033
// resolvable `<image>` declarations in the retail data state a grid their
// sprite sheet contradicts, so a loader that believes the XML mis-slices about
// one image in ten, and it mis-slices it into plausible-looking garbage rather
// than into an error.

#include <string_view>

#include "builder.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A unit-shaped entity: two sheets, a body/shadow layer pair, two states and
/// two animations addressed by slot. Modelled on UNITS\BBOWMAN.
constexpr std::string_view kUnitEntity = R"(<?xml version="1.0"?>
<?xml-stylesheet href="../entity.xsl"?>
<entity name="BBowman" type="vx/unit" variations="8" pass_file="">
  <images>
    <image idx="1" file="Attack.rle" drawmode="player_color" remaping="none" rows="15" columns="8"/>
    <image idx="2" file="Attack_shadow" drawmode="shadow" remaping="pingpong" rows="15" columns="8"/>
  </images>
  <points>
    <point idx="1" type="2" x="-70" y="74"/>
    <point idx="2" type="16" x="12" y="-3"/>
  </points>
  <layers>
    <layer idx="1" name="unit" image="1" z="1000" offsetx="-93" offsety="-114"
           sortoffsetx="0" sortoffsety="80" xray="0"/>
    <layer idx="2" name="unit_shadow" image="2" z="800" offsetx="-93" offsety="-114"/>
    <layer idx="3" name="crown" image="1" z="1500" offsetx="-93" offsety="-114" xray="1"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="0" image_row="1" offsetx="-93" offsety="-114"
           anim_idx="13" anim_frame="1"/>
    <state idx="2" name="attack" image_idx="0" image_row="0" offsetx="-93" offsety="-114"
           anim_idx="65536" anim_frame="65536"/>
  </states>
  <anims>
    <anim idx="13" name="idle" startstate="1" endstate="1" frames="4" duration="198"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1" offsetx="-93" offsety="-114"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="66"/>
      <frame idx="3" duration="66"/>
      <frame idx="4" duration="0"/>
    </anim>
    <anim idx="19" name="toattack" startstate="1" endstate="2" frames="3" duration="132"
          default_duration="0" action_time="66" step="57" floating="1"/>
  </anims>
</entity>)";

/// A 2x1 indexed frame table, deliberately disagreeing with what the entity
/// above declares for image 1 (15 rows, 8 columns, player colour).
Builder disagreeing_frame_table() {
  Builder table;
  table.text("IMGRLE").u32(1).u32(0).u32(0);  // class 1 = indexed
  table.u32(2).u32(1);                        // columns, rows
  table.u32(3).u32(5).u32(6).u32(6);          // frame 0 box
  table.text("RLE2").u32(4).u32(2).u16(1);
  table.u32(16).u32(0).u32(0).u32(0);
  table.text("pamm").u32(8);
  table.u32(200).u32(200).u32(0).u32(0);  // frame 1, empty
  table.u32(1);
  table.u8(0).u8(0).u8(0).u8(0);
  return table;
}

}  // namespace

TEST(entity_parses_a_unit_document) {
  const auto entity = Entity::parse(bytes(kUnitEntity), "Units\\BBowman\\BBowman.ent.xml");
  REQUIRE(entity.ok());
  CHECK(entity->name() == "BBowman");
  CHECK(entity->type() == "vx/unit");
  CHECK(entity->variations() == 8);
  CHECK(entity->pass_file().empty());
  // The path is folded to the pack index's spelling, because that is what
  // every reference to it will be compared against.
  CHECK(entity->path() == "UNITS\\BBOWMAN\\BBOWMAN.ENT.XML");

  REQUIRE(entity->images().size() == 2);
  REQUIRE(entity->points().size() == 2);
  REQUIRE(entity->layers().size() == 3);
  REQUIRE(entity->states().size() == 2);
  REQUIRE(entity->anims().size() == 2);

  const EntityImage& image = entity->images()[0];
  CHECK(image.idx == 1);
  CHECK(image.file == "Attack.rle");
  CHECK(image.declared_draw_mode == DrawMode::player_color);
  CHECK(image.order == AnimOrder::forward);
  CHECK(entity->images()[1].order == AnimOrder::pingpong);
  CHECK(entity->images()[1].declared_draw_mode == DrawMode::shadow);

  const EntityLayer* layer = entity->layer(1);
  REQUIRE(layer != nullptr);
  CHECK(layer->name == "unit");
  CHECK(layer->image == 1);
  CHECK(layer->z == 1000);
  CHECK(layer->offsetx == -93);
  CHECK(layer->sortoffsety == 80);
  // Absent optional attributes read as absent, not as zero.
  CHECK(!entity->layer(2)->percent.present);

  const EntityAnim* idle = entity->anim(kAnimIdle);
  REQUIRE(idle != nullptr);
  CHECK(idle->name == "idle");
  CHECK(idle->startstate == 1 && idle->endstate == 1);
  CHECK(idle->replaces.size() == 1);
  REQUIRE(idle->frame_durations.size() == 4);
  // The strip is bookended by two zero-length entry/exit markers.
  CHECK(idle->frame_durations[0] == 0);
  CHECK(idle->frame_durations[3] == 0);
  CHECK(idle->measured_duration() == 132);
  CHECK(idle->sprite_rows() == 2);
  CHECK(!idle->floating_heading);
  CHECK(entity->anim(kAnimToAttack)->floating_heading);
}

TEST(entity_rejects_a_document_that_is_not_an_entity) {
  CHECK(!Entity::parse(bytes("<class id=\"BBowman\" cpp_class=\"CVXUnit\"/>")).ok());
  CHECK(!Entity::parse(bytes("")).ok());
  CHECK(!Entity::parse(bytes("<entity name=\"x\"")).ok());
  CHECK(!Entity::parse(bytes("<entity><images><image idx=\"1\"/></entity>")).ok());
  CHECK(!Entity::parse(bytes("not xml at all")).ok());
}

TEST(entity_tolerates_a_document_missing_whole_sections) {
  // Three shipped files carry no <anims> and one carries no <points>. An entity
  // with nothing but a state is still a valid entity.
  const auto entity = Entity::parse(bytes(
      "<entity name=\"\" type=\"\" variations=\"1\" pass_file=\"\">"
      "<images/><layers/><states><state idx=\"1\" name=\"idle\"/></states></entity>"));
  REQUIRE(entity.ok());
  CHECK(entity->images().empty());
  CHECK(entity->points().empty());
  CHECK(entity->anims().empty());
  REQUIRE(entity->states().size() == 1);
  // No anim_idx at all means the sentinel, not slot zero.
  CHECK(!entity->states()[0].has_anim());
  CHECK(entity->states()[0].anim_idx == kNoAnim);
}

TEST(entity_prefers_the_frame_table_over_the_declared_grid) {
  auto entity = Entity::parse(bytes(kUnitEntity), "Units\\BBowman\\BBowman.ent.xml");
  REQUIRE(entity.ok());
  const Builder table = disagreeing_frame_table();
  const auto sheet = RleImage::parse(table.span());
  REQUIRE(sheet.ok());
  REQUIRE(sheet->rows() == 1);
  REQUIRE(sheet->columns() == 2);

  CHECK(entity->unresolved_geometry() == 2);
  const auto conflict = entity->adopt_frame_table(1, *sheet);
  REQUIRE(conflict.ok());
  CHECK(conflict->rows);
  CHECK(conflict->columns);
  CHECK(conflict->draw_mode);
  CHECK(conflict->any());

  const EntityImage* image = entity->image(1);
  REQUIRE(image != nullptr);
  // The sheet wins on every count...
  CHECK(image->geometry.rows == 1);
  CHECK(image->geometry.columns == 2);
  CHECK(image->geometry.draw_mode == DrawMode::indexed);
  CHECK(image->geometry.from_frame_table);
  // ...and what the XML claimed is still there to be reported on.
  CHECK(image->declared_rows == 15);
  CHECK(image->declared_columns == 8);
  CHECK(image->declared_draw_mode == DrawMode::player_color);

  CHECK(entity->unresolved_geometry() == 1);
  CHECK(!entity->adopt_frame_table(99, *sheet).ok());
}

TEST(entity_reports_an_agreeing_frame_table_as_no_conflict) {
  const auto entity_result = Entity::parse(bytes(
      "<entity><images>"
      "<image idx=\"1\" file=\"a\" drawmode=\"index\" remaping=\"none\" rows=\"1\" columns=\"2\"/>"
      "</images><layers/><states/></entity>"));
  REQUIRE(entity_result.ok());
  Entity entity = *entity_result;
  const Builder table = disagreeing_frame_table();
  const auto sheet = RleImage::parse(table.span());
  REQUIRE(sheet.ok());
  const auto conflict = entity.adopt_frame_table(1, *sheet);
  REQUIRE(conflict.ok());
  CHECK(!conflict->any());
  CHECK(entity.unresolved_geometry() == 0);
}

TEST(entity_addresses_animations_by_slot) {
  const auto entity = Entity::parse(bytes(kUnitEntity));
  REQUIRE(entity.ok());
  REQUIRE(entity->anim(kAnimIdle) != nullptr);
  CHECK(entity->anim(kAnimIdle)->idx == 13);
  CHECK(entity->anim(kAnimToAttack)->name == "toattack");

  // Scripts call PlayAnim with literals, including slots no entity declares.
  // A missing slot is ordinary and must never be an error.
  CHECK(entity->anim(0) == nullptr);
  CHECK(entity->anim(16) == nullptr);
  CHECK(entity->anim(kAnimDie) == nullptr);
  CHECK(!entity->has_anim(kAnimAttack));
  CHECK(entity->has_anim(kAnimIdle));

  // A state loops the animation in the slot it names; the 65536 sentinel means
  // it loops nothing.
  const EntityState* idle = entity->state(1);
  const EntityState* attack = entity->state(2);
  REQUIRE(idle != nullptr && attack != nullptr);
  CHECK(idle->has_anim());
  CHECK(entity->anim_for(*idle) == entity->anim(kAnimIdle));
  CHECK(!attack->has_anim());
  CHECK(entity->anim_for(*attack) == nullptr);
}

TEST(entity_tolerates_a_state_pointing_at_an_undeclared_anim) {
  // UNITS\ICATAPULT and UNITS\IMOUNTAINEER ship exactly this: a state naming a
  // slot the file never declares. The engine runs them, so the loader must.
  const auto entity = Entity::parse(bytes(
      "<entity><images/><layers/><states>"
      "<state idx=\"1\" name=\"idle\" anim_idx=\"13\"/></states><anims/></entity>"));
  REQUIRE(entity.ok());
  REQUIRE(entity->state(1) != nullptr);
  CHECK(entity->state(1)->has_anim());
  CHECK(entity->anim_for(*entity->state(1)) == nullptr);
}

TEST(entity_orders_layers_by_z) {
  const auto entity = Entity::parse(bytes(kUnitEntity));
  REQUIRE(entity.ok());
  const std::vector<std::uint32_t> order = entity->draw_order();
  REQUIRE(order.size() == 3);
  // Shadow (800) under body (1000) under crown (1500), whatever order they
  // were declared in.
  CHECK(entity->layers()[order[0]].z == 800);
  CHECK(entity->layers()[order[1]].z == 1000);
  CHECK(entity->layers()[order[2]].z == 1500);
  CHECK(order[0] == 1 && order[1] == 0 && order[2] == 2);
}

TEST(entity_breaks_z_ties_by_declaration_order) {
  // Iteration order is world state in this engine, so an arbitrary tiebreak in
  // a comparison sort would be a desync hazard rather than a cosmetic detail.
  const auto entity = Entity::parse(bytes(
      "<entity><images/><layers>"
      "<layer idx=\"1\" name=\"a\" image=\"1\" z=\"1000\"/>"
      "<layer idx=\"2\" name=\"b\" image=\"1\" z=\"500\"/>"
      "<layer idx=\"3\" name=\"c\" image=\"1\" z=\"1000\"/>"
      "<layer idx=\"4\" name=\"d\" image=\"1\" z=\"500\"/>"
      "</layers><states/></entity>"));
  REQUIRE(entity.ok());
  const std::vector<std::uint32_t> order = entity->draw_order();
  REQUIRE(order.size() == 4);
  CHECK(entity->layers()[order[0]].name == "b");
  CHECK(entity->layers()[order[1]].name == "d");
  CHECK(entity->layers()[order[2]].name == "a");
  CHECK(entity->layers()[order[3]].name == "c");
}

TEST(zbins_partition_the_layer_depth_range) {
  constexpr std::string_view kZBins = R"(<zbins>
    <zbin startz="0" sort="1"/>
    <zbin startz="750" sort="0"/>
    <zbin startz="900" sort="1"/>
    <zbin startz="1080" sort="0"/>
    <zbin startz="10000" sort="1"/>
  </zbins>)";
  const auto bins = ZBins::parse(bytes(kZBins));
  REQUIRE(bins.ok());
  REQUIRE(bins->bins().size() == 5);
  CHECK(bins->bin_for(20) == 0);
  CHECK(bins->bin_for(500) == 0);
  CHECK(bins->bin_for(800) == 1);
  CHECK(bins->bin_for(1000) == 2);
  CHECK(bins->bin_for(1500) == 3);
  // Shadows and full-screen effects are unsorted; the sprite body is sorted.
  CHECK(bins->sorted_at(500));
  CHECK(!bins->sorted_at(800));
  CHECK(bins->sorted_at(1000));
  CHECK(!bins->sorted_at(1500));
  // Below the first bin there is no bin at all.
  CHECK(bins->bin_for(-1) == bins->bins().size());
  CHECK(!bins->sorted_at(-1));

  CHECK(!ZBins::parse(bytes("<entity/>")).ok());
}

TEST(seasonal_variants_select_a_different_loaded_entity) {
  // The seam this exercises: the class graph owns which path a class uses in a
  // season, the library owns turning a path into a loaded entity, and neither
  // knows about the other. Getting it wrong shows up as a summer tree in a
  // snowfield rather than as an error, which is why it is worth a test.
  ClassGraph graph;
  REQUIRE(graph
              .add(bytes("<class id=\"Tree01\" cpp_class=\"CVXDecor\""
                         " entity=\"MapObjects/3LTrees/Tree01.ent.xml\""
                         " entity_winter=\"MapObjects/3LTrees/Tree01_w.ent.xml\"/>"),
                   "TREE01.SC.XML")
              .ok());
  graph.link();
  const ClassIndex tree = graph.find("Tree01");
  REQUIRE(tree != kNoClass);

  EntityLibrary library;
  const auto summer_entity = library.load(graph.entity_path(tree, Season::summer),
                                          bytes("<entity name=\"Tree01\"><images/><layers/>"
                                                "<states><state idx=\"1\" name=\"idle\"/>"
                                                "</states></entity>"));
  const auto winter_entity = library.load(graph.entity_path(tree, Season::winter),
                                          bytes("<entity name=\"Tree01_w\"><images/><layers/>"
                                                "<states><state idx=\"1\" name=\"idle\"/>"
                                                "</states></entity>"));
  REQUIRE(summer_entity.ok());
  REQUIRE(winter_entity.ok());
  REQUIRE(library.size() == 2);

  CHECK(library.find(graph.entity_path(tree, Season::winter))->name() == "Tree01_w");
  // Spring and autumn are not declared, so the base entity is what is drawn.
  CHECK(library.find(graph.entity_path(tree, Season::spring))->name() == "Tree01");
  CHECK(library.find(graph.entity_path(tree, Season::autumn))->name() == "Tree01");
  CHECK(library.find(graph.entity_path(tree, Season::summer))->name() == "Tree01");
}

TEST(entity_library_shares_one_definition) {
  // 889 entities back 845 classes: eight faction gates point at one definition,
  // and every object of every one of them draws from the same loaded entity.
  EntityLibrary library;
  constexpr std::string_view kDocument =
      "<entity name=\"Gate\"><images/><layers/><states>"
      "<state idx=\"1\" name=\"idle\"/></states></entity>";
  const auto first = library.load("Buildings/Gate/Gate.ent.xml", bytes(kDocument));
  REQUIRE(first.ok());
  // A second load of the same path returns what is already there rather than
  // reparsing: definitions are immutable, and replacing one behind live objects
  // would be a dangling pointer with extra steps.
  const auto again = library.load("BUILDINGS\\GATE\\GATE.ENT.XML", bytes("<entity/>"));
  REQUIRE(again.ok());
  CHECK(*again == *first);
  CHECK(library.size() == 1);

  // Lookup folds case and separators, because references do not agree on them.
  CHECK(library.find("buildings/gate/gate.ent.xml") == *first);
  CHECK(library.find("BUILDINGS\\GATE\\GATE.ENT.XML") == *first);
  CHECK(library.find("Buildings/Gate/Other.ent.xml") == nullptr);
  CHECK(&library.at(0) == *first);

  // A malformed document is refused and leaves nothing behind.
  CHECK(!library.load("Buildings/Gate/Broken.ent.xml", bytes("<class/>")).ok());
  CHECK(library.size() == 1);
}

TEST(sequenced_row_follows_the_remaping_order) {
  // `remaping` is the row playback order, not a palette operation.
  CHECK(sequenced_row(AnimOrder::forward, 4, 0) == 0);
  CHECK(sequenced_row(AnimOrder::forward, 4, 3) == 3);
  CHECK(sequenced_row(AnimOrder::forward, 4, 4) == 0);

  CHECK(sequenced_row(AnimOrder::reverse, 4, 0) == 3);
  CHECK(sequenced_row(AnimOrder::reverse, 4, 1) == 2);
  CHECK(sequenced_row(AnimOrder::reverse, 4, 4) == 3);

  // 0 1 2 3 2 1 | 0 1 ... — neither endpoint is held twice, which is what
  // "must not snap back" buys the swaying trees and fires that use it.
  CHECK(sequenced_row(AnimOrder::pingpong, 4, 0) == 0);
  CHECK(sequenced_row(AnimOrder::pingpong, 4, 3) == 3);
  CHECK(sequenced_row(AnimOrder::pingpong, 4, 4) == 2);
  CHECK(sequenced_row(AnimOrder::pingpong, 4, 5) == 1);
  CHECK(sequenced_row(AnimOrder::pingpong, 4, 6) == 0);

  // Degenerate sheets must not divide by zero or wrap negatively.
  CHECK(sequenced_row(AnimOrder::pingpong, 1, 7) == 0);
  CHECK(sequenced_row(AnimOrder::reverse, 1, 7) == 0);
  CHECK(sequenced_row(AnimOrder::forward, 0, 7) == 0);
}

TEST(entity_carries_point_types_verbatim) {
  // The 16 point types have no legend anywhere in the data. Preserving them
  // exactly is the whole contract; interpreting them here would be inventing.
  const auto entity = Entity::parse(bytes(kUnitEntity));
  REQUIRE(entity.ok());
  REQUIRE(entity->points().size() == 2);
  CHECK(entity->points()[0].idx == 1);
  CHECK(entity->points()[0].type == 2);
  CHECK(entity->points()[0].x == -70);
  CHECK(entity->points()[0].y == 74);
  CHECK(entity->points()[1].type == 16);
  CHECK(entity->points()[1].y == -3);
}

TEST(entity_tolerates_the_shipped_data_defects) {
  const auto entity = Entity::parse(bytes(
      "<entity name=\"\" type=\"\" variations=\"1\" pass_file=\"\" radius=\"\">"
      "<images>"
      "<image idx=\"1\" file=\"x\" drawmode=\"playercol\" remaping=\"none\" rows=\"4\""
      " columns=\"1\"/>"
      "</images>"
      "<layers><layer idx=\"1\" name=\"l\" image=\"1\" z=\"20\" xray=\"226\"/></layers>"
      "<states><state idx=\"1\" name=\"idle\" offsetx=\"-842150451\" offsety=\"-842150451\"/>"
      "<state idx=\"2\" name=\"2\" offsetx=\"-93\" offsety=\"-114\"/></states>"
      "</entity>"));
  REQUIRE(entity.ok());
  REQUIRE(entity->images().size() == 1);
  // `playercol` occurs once in the corpus and is the same mode.
  CHECK(entity->images()[0].declared_draw_mode == DrawMode::player_color);
  // `xray="226"` is out of a 0/1 domain on four shipped layers. Carried, not
  // clamped: clamping would hide a defect the renderer should decide about.
  REQUIRE(entity->layer(1) != nullptr);
  CHECK(entity->layer(1)->xray == 226);
  // 0xCDCDCDCD is MSVC's uninitialised-heap fill, in seven shipped states.
  REQUIRE(entity->state(1) != nullptr);
  CHECK(entity->state(1)->offsets_are_garbage());
  CHECK(!entity->state(2)->offsets_are_garbage());
  // An empty numeric attribute is absent, not zero.
  CHECK(!entity->radius().present);
  CHECK(entity->radius().value_or(-1) == -1);
}

TEST(entity_distinguishes_a_zero_attribute_from_a_missing_one) {
  const auto entity = Entity::parse(bytes(
      "<entity radius=\"0\" selection_radius=\"152\"><images/><layers/><states/></entity>"));
  REQUIRE(entity.ok());
  CHECK(entity->radius().present);
  CHECK(entity->radius().value == 0);
  CHECK(entity->selection_radius().value_or(-1) == 152);
  CHECK(!entity->floating_turnspeed().present);
}

TEST(entity_lookup_misses_return_null) {
  const auto entity = Entity::parse(bytes(kUnitEntity));
  REQUIRE(entity.ok());
  CHECK(entity->image(3) == nullptr);
  CHECK(entity->layer(9) == nullptr);
  CHECK(entity->state(4) == nullptr);
}

TEST(image_references_are_spelled_three_ways) {
  // `file="Building"` has to find BUILDING.RLE.MMP; the extension is dropped
  // more often than not, and the pixel data ships under a doubled one.
  const auto candidates =
      image_path_candidates("UNITS\\BBOWMAN\\BBOWMAN.ENT.XML", "Attack.rle");
  CHECK(candidates[0] == "UNITS\\BBOWMAN\\ATTACK.RLE");
  CHECK(candidates[1] == "UNITS\\BBOWMAN\\ATTACK.RLE.MMP");
  CHECK(candidates[2] == "UNITS\\BBOWMAN\\ATTACK.RLE.RLE.MMP");

  const auto bare = image_path_candidates("MAPOBJECTS/TOBJ/TOBJ.ENT.XML", "tobj23");
  CHECK(bare[0] == "MAPOBJECTS\\TOBJ\\TOBJ23");
  CHECK(bare[2] == "MAPOBJECTS\\TOBJ\\TOBJ23.RLE.MMP");
}

TEST(pass_masks_are_spelled_two_ways) {
  // 51 of the 597 shipped masks are stored under the bare name `PASS`.
  const auto candidates = pass_path_candidates("BUILDINGS\\EBARRACKS\\E.ENT.XML", "pass");
  CHECK(candidates[0] == "BUILDINGS\\EBARRACKS\\PASS");
  CHECK(candidates[1] == "BUILDINGS\\EBARRACKS\\PASS.PASS");
  CHECK(entity_directory("A\\B\\C.ENT.XML") == "A\\B");
  CHECK(entity_directory("C.ENT.XML").empty());
}

TEST(resource_paths_fold_to_the_pack_index_spelling) {
  CHECK(normalise_resource_path("Units/BBowman/bbowman.ent.xml") ==
        "UNITS\\BBOWMAN\\BBOWMAN.ENT.XML");
  CHECK(normalise_resource_path("  gameres/icons/x.bmp  ") == "UI\\ICONS\\X.BMP");
  CHECK(normalise_resource_path("").empty());
}

TEST(draw_modes_map_both_ways) {
  CHECK(draw_mode_from_name("player_color") == DrawMode::player_color);
  CHECK(draw_mode_from_name("index") == DrawMode::indexed);
  CHECK(draw_mode_from_name("") == DrawMode::unknown);
  CHECK(draw_mode_from_name("nonsense") == DrawMode::unknown);
  CHECK(draw_mode_name(DrawMode::indexed) == "index");
  CHECK(draw_mode_from_image_class(RleImageClass::player_color) == DrawMode::player_color);
  CHECK(draw_mode_from_image_class(RleImageClass::shadow) == DrawMode::shadow);
  CHECK(draw_mode_from_image_class(RleImageClass::truecolor) == DrawMode::normal);
  CHECK(anim_order_from_name("pingpong") == AnimOrder::pingpong);
  CHECK(anim_order_from_name("") == AnimOrder::forward);
  CHECK(anim_order_name(AnimOrder::forward) == "none");
}
