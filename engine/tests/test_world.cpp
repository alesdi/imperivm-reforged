// Map loading and the projection.
//
// Two things here earn tests rather than a look at the screen.
//
// The **projection** is integer arithmetic with an exact constant, 46/64, and
// every object in a scene lands wherever it says. One wrong rounding rule moves
// a wall half a pixel per cell and the joins open up a hundred cells later, by
// which time nothing looks like a rounding bug any more.
//
// The **transition corner code** is derived, not read: `Terrain.trans.grid` is
// zero in every cell of every shipped map, so the four-bit pattern comes from
// the terrain layer's own neighbourhood. Get a corner's three touching cells
// wrong and the ground still renders — with the wrong mask, which reads as
// slightly odd edges rather than as an error.
//
// Everything is built from bytes rather than loaded from a container: CI has no
// game installation, and this project must never carry game assets.

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "builder.hpp"
#include "imperivm/core/world/map.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

Builder document(const std::string& text) {
  Builder out;
  out.text(text);
  return out;
}

/// An 8-bit terrain grid of `size` x `size` cells at 64 world units each.
Builder terrain_grid(std::uint32_t size, const std::vector<std::uint8_t>& cells) {
  Builder grid;
  grid.text("DIRG").u32(64).u32(8).u32(size * 64).u32(size * 64);
  for (const std::uint8_t cell : cells) grid.u8(cell);
  return grid;
}

}  // namespace

// --------------------------------------------------------------------------
// projection
// --------------------------------------------------------------------------

TEST(projection_leaves_x_alone_and_squashes_y_by_46_over_64) {
  CHECK(world_to_screen_x(0) == 0);
  CHECK(world_to_screen_x(12345) == 12345);

  // One cell is 64 world units and 46 pixels, exactly.
  CHECK(world_to_screen_y(0) == 0);
  CHECK(world_to_screen_y(64) == 46);
  CHECK(world_to_screen_y(640) == 460);
  CHECK(world_to_screen_y(16384) == 16384 * 46 / 64);
}

TEST(projection_is_integer_and_truncates) {
  // 100 * 46 / 64 = 71.875. The engine has no floating point, and the
  // implementable summary in docs/engine/projection.md truncates; a renderer
  // that rounded instead would sit up to a pixel above this one.
  CHECK(world_to_screen_y(100) == 71);
  CHECK(world_to_screen_y(1) == 0);
  CHECK(world_to_screen_y(63) == 45);
}

TEST(projection_inverse_recovers_cell_corners_exactly) {
  for (std::int32_t cell = 0; cell < 256; ++cell) {
    const std::int32_t world = cell * 64;
    CHECK(screen_to_world_y(world_to_screen_y(world)) == world);
  }
}

TEST(elevation_lifts_a_point_by_its_height) {
  // One pixel per unit of height, as 0x005c11a0 subtracts it.
  CHECK(kHeightScaleNumerator == 1 && kHeightScaleDenominator == 1);
  CHECK(world_to_screen_y(640, 200) == world_to_screen_y(640, 0) - 200);
}

// --------------------------------------------------------------------------
// map.xml, game.xml, player<i>.xml
// --------------------------------------------------------------------------

TEST(map_xml_yields_the_world_square_and_the_bookmark) {
  const Builder xml = document(
      "<map name=\"Zama\" displayname=\"Battle for Zama\">"
      "<size x=\"16384\" y=\"16384\"/>"
      "<start_pt x=\"12091\" y=\"10979\"/>"
      "</map>");
  const auto geometry = MapGeometry::parse(xml.span());
  REQUIRE(geometry.ok());
  CHECK(geometry->name == "Zama");
  CHECK(geometry->display_name == "Battle for Zama");
  CHECK(geometry->size_x == 16384);
  CHECK(geometry->size_y == 16384);
  CHECK(geometry->has_start);
  CHECK(geometry->start_x == 12091);
}

TEST(map_xml_rejects_the_uninitialised_bookmark) {
  // `randommap.BFHP` ships 0xCDCDCDCD in both axes: the MSVC debug-heap fill,
  // serialised straight into the file. It is "no bookmark", not a coordinate.
  const Builder xml = document(
      "<map name=\"New Map\"><size x=\"16384\" y=\"16384\"/>"
      "<start_pt x=\"-842150451\" y=\"-842150451\"/></map>");
  const auto geometry = MapGeometry::parse(xml.span());
  REQUIRE(geometry.ok());
  CHECK(!geometry->has_start);
}

TEST(map_xml_without_a_size_is_not_a_map) {
  const Builder xml = document("<map name=\"broken\"/>");
  CHECK(!MapGeometry::parse(xml.span()).ok());
}

TEST(game_xml_yields_the_season_and_defaults_it) {
  const Builder with = document(
      "<game><properties game_type=\"1\" name=\"Numantia\" start_map=\"2\""
      " season=\"winter\"/></game>");
  const auto properties = GameProperties::parse(with.span());
  REQUIRE(properties.ok());
  CHECK(properties->season == "winter");
  CHECK(properties->start_map == 2);

  // Absent in the four blank templates, and spring is what every shipped
  // container declares.
  const Builder without = document("<game><properties name=\"blank\"/></game>");
  const auto blank = GameProperties::parse(without.span());
  REQUIRE(blank.ok());
  CHECK(blank->season == "spring");
  CHECK(blank->start_map == 1);
}

TEST(player_colour_is_rgb555_packed_into_a_decimal_integer) {
  // 31810 = 0b111110000100010: red 31, green 2, blue 2.
  const Builder xml = document(
      "<playerdata id=\"0\" name=\"Player 1\" race=\"Mutable\" control=\"Both\""
      " color=\"31810\" startx=\"2635\" starty=\"2047\"/>");
  const auto slot = PlayerSlot::parse(xml.span());
  REQUIRE(slot.ok());
  CHECK(slot->id == 0);
  CHECK(slot->control == "Both");
  CHECK(slot->color.red == 255);
  CHECK(slot->color.green == 16);
  CHECK(slot->color.blue == 16);
  CHECK(slot->start_x == 2635);
}

// --------------------------------------------------------------------------
// map.obj.xml
// --------------------------------------------------------------------------

TEST(object_list_reads_objects_settlements_and_groups) {
  const Builder xml = document(
      "<mapobject>"
      "<scriptobj class=\"AdvArea\" num=\"0\" player=\"1\" x=\"2370\" y=\"503\""
      " flags=\"0x80000001\" dir.x=\"0\" dir.y=\"1\"/>"
      "<settlement id=\"0\" player=\"4\" classoffirstbuilding=\"CTownhall\" name=\"S_Utica\">"
      "<scriptobj class=\"CWallsNW\" num=\"1\" player=\"4\" x=\"13250\" y=\"9137\""
      " flags=\"0x80800008\" dir.x=\"0\" dir.y=\"1\"/>"
      "<scriptobj class=\"CTownhall\" num=\"2\" player=\"4\" x=\"13300\" y=\"9200\""
      " flags=\"0x80800008\" dir.x=\"1\" dir.y=\"0\"/>"
      "</settlement>"
      "<group name=\"Q_HannibalArmy\" type=\"1\"><obj num=\"1\"/><obj num=\"2\"/></group>"
      "</mapobject>");
  const auto objects = MapObjectList::parse(xml.span());
  REQUIRE(objects.ok());
  REQUIRE(objects->objects().size() == 3);
  REQUIRE(objects->settlements().size() == 1);
  REQUIRE(objects->groups().size() == 1);

  // Settlement members are ordinary objects that carry their settlement.
  CHECK(objects->objects()[0].settlement == -1);
  CHECK(objects->objects()[1].settlement == 0);
  CHECK(objects->objects()[1].class_name == "CWallsNW");
  CHECK(objects->objects()[1].x == 13250);
  CHECK(objects->objects()[2].dir_x == 1);
  CHECK(objects->settlements()[0].name == "S_Utica");
  CHECK(objects->groups()[0].members.size() == 2);
  CHECK(objects->find(2) != nullptr);
  CHECK(objects->find(99) == nullptr);
}

TEST(object_flags_are_hexadecimal_and_carry_the_owner_mask) {
  const Builder xml = document(
      "<mapobject><scriptobj class=\"RHero2\" num=\"0\" player=\"4\" x=\"1\" y=\"2\""
      " flags=\"0xA9400008\"/></mapobject>");
  const auto objects = MapObjectList::parse(xml.span());
  REQUIRE(objects.ok());
  REQUIRE(objects->objects().size() == 1);
  const MapObject& object = objects->objects().front();
  CHECK(object.flags == 0xA9400008u);
  // Bits 0..15 are a one-hot owner mask, `1 << (player - 1)`, and agree with
  // the `player` attribute on all 27,070 shipped objects.
  CHECK((object.flags & 0xFFFFu) == (1u << (object.player - 1)));
  CHECK((object.flags & 0x01000000u) != 0);  // bit 24: the class is a hero
}

TEST(object_list_rejects_a_document_that_is_not_an_object_list) {
  const Builder xml = document("<terrain><layer z=\"0\"/></terrain>");
  CHECK(!MapObjectList::parse(xml.span()).ok());
}

// --------------------------------------------------------------------------
// DATA\TERRAINS.XML
// --------------------------------------------------------------------------

TEST(terrain_table_reads_layers_and_the_water_flags) {
  const Builder xml = document(
      "<terrain>"
      "<layer z=\"3\" type=\"1\" display=\"Grass 1\" image=\"terrain/%season%/grass1024.vq\"/>"
      "<layer z=\"6\" type=\"5\" display=\"Rocks 1\" passable=\"0\""
      " image=\"terrain/%season%/rocks1024.vq\"/>"
      "<layer z=\"13\" type=\"4\" display=\"Water\" dark=\"1\" frames=\"15\""
      " transition=\"2\" image=\"terrain/dwater.vq\"/>"
      "</terrain>");
  const auto table = TerrainTable::parse(xml.span());
  REQUIRE(table.ok());
  REQUIRE(table->layers().size() == 3);
  REQUIRE(table->layer(6) != nullptr);
  CHECK(table->layer(6)->passable == false);
  CHECK(table->layer(3)->passable == true);
  REQUIRE(table->layer(kDeepWaterLayer) != nullptr);
  CHECK(table->layer(kDeepWaterLayer)->dark);
  CHECK(table->layer(kDeepWaterLayer)->frames == 15);
  CHECK(table->layer(40) == nullptr);
}

TEST(terrain_texture_path_substitutes_the_season_and_folds_the_spelling) {
  CHECK(TerrainTable::texture_path("terrain/%season%/grass1024.vq", "spring") ==
        "TERRAIN\\SPRING\\GRASS1024.VQ");
  // Not every path carries the token, and one carries nothing else.
  CHECK(TerrainTable::texture_path("terrain/dwater.vq", "winter") == "TERRAIN\\DWATER.VQ");
}

// --------------------------------------------------------------------------
// transitions
// --------------------------------------------------------------------------

TEST(transition_mask_name_is_style_then_tl_tr_br_bl) {
  CHECK(transition_mask_name('A', kCornerTopLeft) == "A1000");
  CHECK(transition_mask_name('A', kCornerBottomRight) == "A0010");
  CHECK(transition_mask_name('C', kCornerTopLeft | kCornerBottomLeft) == "C1001");
  CHECK(transition_mask_name('D', 0xF) == "D1111");
}

TEST(a_uniform_terrain_has_no_transitions) {
  const Builder grid = terrain_grid(3, std::vector<std::uint8_t>(9, 3));
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  std::array<TerrainOverlay, kMaxTerrainOverlays> overlays{};
  CHECK(terrain_overlays(terrain.value(), 1, 1, overlays) == 0);
}

TEST(a_higher_neighbour_claims_the_corners_that_touch_it) {
  // 3 is grass, 6 is rock; rock wins because priority is the layer z itself.
  //
  //   6 3 3        the centre cell's top-left corner touches (0,0), (0,1) and
  //   3 3 3        (1,0) -- one of which is rock -- and no other corner does.
  //   3 3 3
  const Builder grid = terrain_grid(3, {6, 3, 3, 3, 3, 3, 3, 3, 3});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());

  std::array<TerrainOverlay, kMaxTerrainOverlays> overlays{};
  REQUIRE(terrain_overlays(terrain.value(), 1, 1, overlays) == 1);
  CHECK(overlays[0].type == 6);
  CHECK(overlays[0].corners == kCornerTopLeft);
  CHECK(transition_mask_name('A', overlays[0].corners) == "A1000");
}

TEST(a_lower_neighbour_never_overlays) {
  // The same neighbourhood with the priority reversed: the centre is the rock
  // and the grass around it is lower, so nothing is composited over it. The
  // grass cells are the ones that carry the blend.
  const Builder grid = terrain_grid(3, {3, 3, 3, 3, 6, 3, 3, 3, 3});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  std::array<TerrainOverlay, kMaxTerrainOverlays> overlays{};
  CHECK(terrain_overlays(terrain.value(), 1, 1, overlays) == 0);

  // and its four edge neighbours each see the rock on two corners.
  REQUIRE(terrain_overlays(terrain.value(), 1, 0, overlays) == 1);
  CHECK(overlays[0].corners == (kCornerBottomLeft | kCornerBottomRight));
  REQUIRE(terrain_overlays(terrain.value(), 0, 1, overlays) == 1);
  CHECK(overlays[0].corners == (kCornerTopRight | kCornerBottomRight));
}

TEST(two_higher_neighbours_produce_two_overlays_ordered_by_type) {
  //   8 3 3
  //   3 3 3      6 on the bottom-right, 8 on the top-left.
  //   3 3 6
  const Builder grid = terrain_grid(3, {8, 3, 3, 3, 3, 3, 3, 3, 6});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  std::array<TerrainOverlay, kMaxTerrainOverlays> overlays{};
  REQUIRE(terrain_overlays(terrain.value(), 1, 1, overlays) == 2);
  CHECK(overlays[0].type == 6);
  CHECK(overlays[0].corners == kCornerBottomRight);
  CHECK(overlays[1].type == 8);
  CHECK(overlays[1].corners == kCornerTopLeft);
}

TEST(the_map_edge_clamps_rather_than_reading_zero) {
  // Cell (0, 0) of a uniform map has no neighbours to its left or above. Those
  // must clamp to the edge cell, not read type 0 -- "Ground 1" is a real
  // terrain and a fringe of it would appear around every map.
  const Builder grid = terrain_grid(3, std::vector<std::uint8_t>(9, 3));
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  std::array<TerrainOverlay, kMaxTerrainOverlays> overlays{};
  CHECK(terrain_overlays(terrain.value(), 0, 0, overlays) == 0);
}

// The ground tile as gbr.exe composes it (0x0061f8a0): a dual grid, the
// terrain byte a vertex and the tile between four of them.

TEST(a_tile_blends_its_own_vertex_and_the_three_to_its_right_and_below) {
  //   3 6 3      tile (0, 0): corners 3 (TL), 6 (TR), 3 (BR), 3 (BL).
  //   3 3 3      The rock is the vertex at the tile's top right, not a
  //   3 3 3      neighbour across an edge.
  const Builder grid = terrain_grid(3, {3, 6, 3, 3, 3, 3, 3, 3, 3});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  const TerrainTile tile = terrain_tile(terrain.value(), 0, 0);
  CHECK(tile.corners[0] == 3);
  CHECK(tile.corners[1] == 6);
  CHECK(tile.corners[2] == 3);
  CHECK(tile.corners[3] == 3);
  CHECK(tile.base == 3);
  REQUIRE(tile.overlay_count == 1);
  CHECK(tile.overlays[0].type == 6);
  CHECK(tile.overlays[0].corners == kCornerTopRight);

  // Tile (1, 0) has the rock at its top left, and tile (1, 1) sees none.
  const TerrainTile right = terrain_tile(terrain.value(), 1, 0);
  REQUIRE(right.overlay_count == 1);
  CHECK(right.overlays[0].corners == kCornerTopLeft);
  CHECK(terrain_tile(terrain.value(), 1, 1).overlay_count == 0);
}

TEST(the_lowest_layer_is_the_base_even_when_it_holds_one_corner) {
  // Priority is the layer number: 3 under 6 wherever they meet, whichever
  // holds more of the tile.
  const Builder grid = terrain_grid(2, {6, 6, 3, 6});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  const TerrainTile tile = terrain_tile(terrain.value(), 0, 0);
  CHECK(tile.base == 3);
  REQUIRE(tile.overlay_count == 1);
  CHECK(tile.overlays[0].type == 6);
  CHECK(tile.overlays[0].corners == (kCornerTopLeft | kCornerTopRight | kCornerBottomRight));
}

TEST(the_far_edge_repeats_the_last_vertex) {
  const Builder grid = terrain_grid(2, {3, 6, 3, 3});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  // Tile (1, 0) is the last column: its right-hand corners clamp to column 1.
  const TerrainTile tile = terrain_tile(terrain.value(), 1, 0);
  CHECK(tile.corners[0] == 6);
  CHECK(tile.corners[1] == 6);
  CHECK(tile.corners[2] == 3);
  CHECK(tile.corners[3] == 3);
}

TEST(shallow_water_is_never_the_base_of_a_shore) {
  // 12 is below 20 by number but is drawn over it: the scan skips shallow
  // water unless every corner is water.
  const Builder grid = terrain_grid(2, {kShallowWaterLayer, 20, 20, 20});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  const TerrainTile tile = terrain_tile(terrain.value(), 0, 0);
  CHECK(tile.base == 20);
  REQUIRE(tile.overlay_count == 1);
  CHECK(tile.overlays[0].type == kShallowWaterLayer);
  CHECK(tile.overlays[0].corners == kCornerTopLeft);
}

TEST(the_waters_are_drawn_after_every_land_layer) {
  //   20 13      base 3; then 20 (land, pass one); then 13 (deep, pass three),
  //    3  3      although 13 < 20.
  const Builder grid = terrain_grid(2, {20, kDeepWaterLayer, 3, 3});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  const TerrainTile tile = terrain_tile(terrain.value(), 0, 0);
  CHECK(tile.base == 3);
  REQUIRE(tile.overlay_count == 2);
  CHECK(tile.overlays[0].type == 20);
  CHECK(tile.overlays[1].type == kDeepWaterLayer);
}

TEST(an_all_water_tile_has_shallow_water_for_its_base) {
  const Builder grid = terrain_grid(2, {kDeepWaterLayer, kShallowWaterLayer, kDeepWaterLayer,
                                        kDeepWaterLayer});
  const auto terrain = Grid::parse(grid.span());
  REQUIRE(terrain.ok());
  const TerrainTile tile = terrain_tile(terrain.value(), 0, 0);
  CHECK(tile.base == kShallowWaterLayer);
  REQUIRE(tile.overlay_count == 1);
  CHECK(tile.overlays[0].type == kDeepWaterLayer);
  CHECK(tile.overlays[0].corners == (kCornerTopLeft | kCornerBottomRight | kCornerBottomLeft));
}

TEST(the_style_alternates_c_and_d_by_row_and_the_mask_names_the_other_corners) {
  CHECK(transition_style(kDefaultTransition, 0) == 'C');
  CHECK(transition_style(kDefaultTransition, 1) == 'D');
  CHECK(transition_style(kDefaultTransition, 6) == 'C');
  CHECK(transition_style(0, 3) == 'B');
  // An overlay at the top-left corner is drawn through the mask whose other
  // three corners are black: C0111, white at the top left.
  CHECK(transition_mask_name('C', transition_mask_code(kCornerTopLeft)) == "C0111");
  CHECK(transition_mask_code(kCornerTopRight | kCornerBottomLeft) ==
        (kCornerTopLeft | kCornerBottomRight));
}

TEST(a_layer_with_no_transition_attribute_takes_the_default) {
  const Builder xml = document(
      "<terrain><layer z=\"3\" type=\"1\" image=\"terrain/%season%/grass1024.vq\"/>"
      "<layer z=\"5\" type=\"3\" transition=\"0\" image=\"terrain/sand.vq\"/></terrain>");
  const auto table = TerrainTable::parse(xml.span());
  REQUIRE(table.ok());
  CHECK(table->layer(3)->transition == kDefaultTransition);
  CHECK(table->layer(5)->transition == 0);
}

TEST(the_light_gain_is_level_plus_four_over_twenty) {
  // 0x0061e210: c + trunc(c * (level - 16) / 20), clamped.
  CHECK(terrain_light_channel(200, 16) == 200);
  CHECK(terrain_light_channel(200, 0) == 40);    // 0.2
  CHECK(terrain_light_channel(200, 21) == 250);  // 1.25
  CHECK(terrain_light_channel(240, 21) == 255);  // clamped
  CHECK(terrain_light_channel(31, 21, 31) == 31);
  CHECK(terrain_light_channel(20, 21, 31) == 25);
  // The divide truncates toward zero: 7 * -1 / 20 is 0, not -1.
  CHECK(terrain_light_channel(7, 15) == 7);
  CHECK(terrain_light_channel(30, 15) == 29);
}

// --------------------------------------------------------------------------
// DECORS.INI and the decor layer
// --------------------------------------------------------------------------

TEST(decor_table_reads_an_ini_and_indexes_it_by_type) {
  const Builder ini = document(
      "[tr31]\n"
      "type = 1\n"
      "group = Trees\n"
      "subgroup = Broad-leaved\n"
      "entity = MapObjects/decors/3LTrees/tr4/s/tr4_s.ent.xml\n"
      "name = Tree 1\n"
      "season = spring\n"
      "\n"
      "[BlatoSmall01]\n"
      "type = 4\n"
      ";name = Swamp 1 (small)\n"
      ";group = Other\n"
      "entity = MapObjects/decors/blatosmall01/blatosmall01_s.ent.xml\n"
      "\n"
      "[Palm1]\n"
      "type = 7\n"
      "Group = Trees\n"
      "Subgroup = Palms\n"
      "entity = MapObjects/decors/palm1/palm1.ent.xml\n"
      "season = spring\n");
  const auto decors = DecorTable::parse(ini.span());
  REQUIRE(decors.ok());
  REQUIRE(decors->kinds().size() == 3);
  REQUIRE(decors->find(1) != nullptr);
  CHECK(decors->find(1)->entity == "MAPOBJECTS\\DECORS\\3LTREES\\TR4\\S\\TR4_S.ENT.XML");
  // The editor's tree: group, subgroup, name, season -- keys in either case.
  CHECK(decors->find(1)->group == "Trees");
  CHECK(decors->find(1)->subgroup == "Broad-leaved");
  CHECK(decors->find(1)->name == "Tree 1");
  CHECK(decors->find(1)->season == "spring");
  CHECK(decors->find(7)->group == "Trees");
  CHECK(decors->find(7)->subgroup == "Palms");
  // A commented-out group is no group.
  CHECK(decors->find(4)->group.empty());
  CHECK(decors->find(4)->name.empty());
  CHECK(decors->find(4) != nullptr);
  CHECK(decors->find(2) == nullptr);
  CHECK(decors->find(199) == nullptr);
}

TEST(a_decor_cell_unpacks_to_a_kind_and_a_sub_cell_offset) {
  DecorCell cell;
  CHECK(!decor_unpack(0, cell));

  // kind 31, x nibble 5, y nibble 12: four world units a step.
  CHECK(decor_unpack(0xC51Fu, cell));
  CHECK(cell.kind == 31);
  CHECK(cell.offset_x == 20);
  CHECK(cell.offset_y == 48);
}

// --------------------------------------------------------------------------
// the loaded map
// --------------------------------------------------------------------------

TEST(world_map_copies_its_layers_and_survives_the_source_bytes) {
  const Builder map_xml = document(
      "<map name=\"tiny\"><size x=\"192\" y=\"192\"/></map>");
  const Builder objects = document(
      "<mapobject><scriptobj class=\"Boulder\" num=\"0\" x=\"96\" y=\"96\""
      " flags=\"0x80000000\"/></mapobject>");
  Builder terrain = terrain_grid(3, {3, 3, 3, 3, 6, 3, 3, 3, 3});

  Builder light;
  light.text("DIRG").u32(32).u32(8).u32(192).u32(192);
  for (int i = 0; i < 36; ++i) light.u8(16);

  MapLayerBytes bytes;
  bytes.map_xml = map_xml.span();
  bytes.object_xml = objects.span();
  bytes.terrain = terrain.span();
  bytes.light = light.span();

  auto map = WorldMap::load(bytes);
  REQUIRE(map.ok());

  // Scribble over the source: the map must not be viewing it.
  for (std::byte& byte : terrain.raw()) byte = std::byte{0xFF};

  CHECK(map->geometry().size_x == 192);
  CHECK(map->terrain_cells() == 3);
  CHECK(map->terrain().cell(1, 1) == 6);
  CHECK(map->objects().objects().size() == 1);
  CHECK(map->light_at(0, 0) == 16);
  // An absent layer answers with the neutral rather than failing.
  CHECK(map->height_at(96, 96) == 0);
}

TEST(world_map_layers_are_editable_in_place) {
  const Builder map_xml = document("<map name=\"t\"><size x=\"128\" y=\"128\"/></map>");
  const Builder terrain = terrain_grid(2, {3, 3, 3, 6});
  // 8 x 8 one-bit cells at 16 world units: stride 1, so 8 body bytes. (A
  // 192-unit map would make 12 cells a row, which is not a whole number of
  // bytes and is refused, as the specification says it must be.)
  Builder pass;
  pass.text("DIRG").u32(16).u32(1).u32(128).u32(128).zeros(8);

  MapLayerBytes bytes;
  bytes.map_xml = map_xml.span();
  bytes.terrain = terrain.span();
  bytes.pass = pass.span();
  auto map = WorldMap::load(bytes);
  REQUIRE(map.ok());

  // The const view and the mutable one are the same grid over the map's own
  // copy: paint through one, read through the other.
  CHECK(map->terrain().cell(0, 0) == 3);
  CHECK(map->terrain_mut().writable());
  CHECK(map->terrain_mut().set_cell(0, 0, 13).ok());
  CHECK(map->terrain().cell(0, 0) == 13);
  CHECK(map->passability_mut().set_cell(7, 7, 1).ok());
  CHECK(map->passability().blocked(7, 7));
  CHECK(map->passability().count_set() == 1);
  // The source bytes the map copied from are untouched.
  CHECK(static_cast<std::uint8_t>(terrain.span()[20]) == 3);

  // A layer the container did not carry is not writable either.
  CHECK(!map->height_mut().writable());
  CHECK(map->height_mut().set_cell(0, 0, 1).error() == FormatError::unsupported);
  CHECK(map->height_at(0, 0) == 0);

  // What was painted is what `write_grid` hands back.
  const std::vector<std::byte> written = write_grid(map->terrain());
  const auto again = Grid::parse(written);
  REQUIRE(again.ok());
  CHECK(again->cell(0, 0) == 13);
  CHECK(again->cell(1, 1) == 6);
}

TEST(world_map_keeps_a_four_bit_terrain_layer_at_four_bits) {
  // The five blank templates in `Packs/` store the terrain layer at 4 bits
  // per cell. A setter that assumed 8 would write the neighbour's nibble.
  const Builder map_xml = document("<map name=\"t\"><size x=\"128\" y=\"64\"/></map>");
  Builder terrain;
  terrain.text("DIRG").u32(64).u32(4).u32(128).u32(64).u8(0x53);
  MapLayerBytes bytes;
  bytes.map_xml = map_xml.span();
  bytes.terrain = terrain.span();
  auto map = WorldMap::load(bytes);
  REQUIRE(map.ok());
  CHECK(map->terrain().bits_per_cell() == 4);
  CHECK(map->terrain().cell(0, 0) == 3);
  CHECK(map->terrain().cell(1, 0) == 5);
  CHECK(map->terrain_mut().max_cell_value() == 15);
  CHECK(map->terrain_mut().set_cell(0, 0, 16).error() == FormatError::buffer_too_small);
  CHECK(map->terrain().cell(0, 0) == 3);
  CHECK(map->terrain_mut().set_cell(0, 0, 15).ok());
  CHECK(map->terrain().cell(0, 0) == 15);
  CHECK(map->terrain().cell(1, 0) == 5);
  CHECK(static_cast<std::uint8_t>(map->terrain().cells()[0]) == 0x5F);
}

TEST(world_map_moves_with_its_layers) {
  const Builder map_xml = document("<map name=\"t\"><size x=\"192\" y=\"192\"/></map>");
  const Builder terrain = terrain_grid(3, {3, 3, 3, 3, 6, 3, 3, 3, 3});
  MapLayerBytes bytes;
  bytes.map_xml = map_xml.span();
  bytes.terrain = terrain.span();
  auto loaded = WorldMap::load(bytes);
  REQUIRE(loaded.ok());
  CHECK(loaded->terrain_mut().set_cell(2, 2, 9).ok());
  const std::byte* buffer = loaded->terrain().cells().data();

  // A `std::vector` move carries its buffer, so the grid copied alongside it
  // still points at bytes the new map owns -- the same address, not a copy.
  WorldMap moved(std::move(loaded.value()));
  CHECK(moved.terrain().cells().data() == buffer);
  CHECK(moved.terrain().cell(2, 2) == 9);
  CHECK(moved.terrain().cell(1, 1) == 6);
  CHECK(moved.terrain_mut().set_cell(0, 0, 1).ok());
  CHECK(moved.terrain().cell(0, 0) == 1);

  WorldMap assigned;
  assigned = std::move(moved);
  CHECK(assigned.terrain().cells().data() == buffer);
  CHECK(assigned.terrain().cell(2, 2) == 9);
  CHECK(assigned.terrain().cell(0, 0) == 1);
  CHECK(assigned.terrain_mut().writable());
  CHECK(assigned.terrain_cells() == 3);
}

TEST(world_map_without_a_terrain_layer_is_not_a_map) {
  const Builder map_xml = document("<map name=\"t\"><size x=\"192\" y=\"192\"/></map>");
  MapLayerBytes bytes;
  bytes.map_xml = map_xml.span();
  CHECK(!WorldMap::load(bytes).ok());
}
