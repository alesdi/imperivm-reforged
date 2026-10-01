// `map.obj.xml` written back: byte for byte for what was read, the shipped
// tail for what was made, and the tables an edit changes.
//
// The corpus half -- every one of the 28 shipped documents coming back
// identical -- is `tests/test_corpus_map_writer.py`; this is the synthetic
// half, where the interesting cases are written rather than found.

#include "imperivm/core/world/map_writer.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "imperivm/core/world/map.hpp"
#include "test.hpp"

using namespace imperivm::core;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// The shipped shape: a free decor, a settlement with its stronghold, a hero
/// with skills and items (attributes this engine does not read), an area, an
/// army group and an alias. CRLF ends and the tab layout of the real files.
constexpr std::string_view kDocument =
    "\t<mapobject>\r\n"
    "\t<scriptobj\r\n"
    "\t\tclass=\"DeadTree\"\r\n"
    "\t\tnum=\"0\"\r\n"
    "\tx=\"100\"\r\n"
    "\ty=\"200\"\r\n"
    "\tflags=\"0x80000000\"\r\n"
    "\tdir.x=\"0\"\r\n"
    "\tdir.y=\"1\"/>\r\n"
    "\t<scriptobj\r\n"
    "\t\tclass=\"RHero1\"\r\n"
    "\t\tnum=\"1\"\r\n"
    "\thsTeamAttack=\"2\"\r\n"
    "\thsHealing=\"1\"\r\n"
    "\tdisplay_name=\"Marius\"\r\n"
    "\tLevel=\"12\"\r\n"
    "\tUnitFlags=\"262144\"\r\n"
    "\tplayer=\"1\"\r\n"
    "\thealthperc=\"100\"\r\n"
    "\tstamina=\"10\"\r\n"
    "\tinventorysize=\"2\"\r\n"
    "\tslot0=\"King's Belt\"\r\n"
    "\tx=\"300\"\r\n"
    "\ty=\"400\"\r\n"
    "\tflags=\"0x8080000A\"\r\n"
    "\tdir.x=\"0\"\r\n"
    "\tdir.y=\"-1\"/>\r\n"
    "\t<scriptobj\r\n"
    "\t\tclass=\"AdvArea\"\r\n"
    "\t\tnum=\"2\"\r\n"
    "\tnextmap=\"\"\r\n"
    "\ttargetarea=\"\"\r\n"
    "\ttype=\"1\"\r\n"
    "\tptx=\"500\"\r\n"
    "\tpty=\"600\"\r\n"
    "\tr=\"70\"\r\n"
    "\tplayer=\"1\"\r\n"
    "\thealthperc=\"100\"\r\n"
    "\tinventorysize=\"0\"\r\n"
    "\tx=\"500\"\r\n"
    "\ty=\"600\"\r\n"
    "\tflags=\"0x80000001\"\r\n"
    "\tdir.x=\"0\"\r\n"
    "\tdir.y=\"1\"/>\r\n"
    "\t\t<settlement\r\n"
    "\t\t\tid=\"0\"\r\n"
    "\t\t\tplayer=\"1\"\r\n"
    "\t\t\tclassoffirstbuilding=\"MutableStronghold\"\r\n"
    "\t\t\tmaxpopulation=\"100\"\r\n"
    "\t\t\textrasentries=\"0\"\r\n"
    "\t\t\tmaxgold=\"100000\"\r\n"
    "\t\t\tmaxfood=\"100000\"\r\n"
    "\t\t\tpopulation=\"60\"\r\n"
    "\t\t\tgold=\"5000\"\r\n"
    "\t\t\tfood=\"1000\"\r\n"
    "\t\t\ticon=\"\"\r\n"
    "\t\t\tname=\"\">\r\n"
    "\t<scriptobj\r\n"
    "\t\tclass=\"MutableStronghold\"\r\n"
    "\t\tnum=\"3\"\r\n"
    "\tdata=\"-1\"\r\n"
    "\tplayer=\"1\"\r\n"
    "\thealthperc=\"100\"\r\n"
    "\tstamina=\"20\"\r\n"
    "\tinventorysize=\"0\"\r\n"
    "\tx=\"2841\"\r\n"
    "\ty=\"2672\"\r\n"
    "\tflags=\"0x80800001\"\r\n"
    "\tdir.x=\"0\"\r\n"
    "\tdir.y=\"1\"/>\r\n"
    "\t\t</settlement>\r\n"
    "\t\t<group\r\n"
    "\t\t\tname=\"Q_Ambush\"\r\n"
    "\t\t\ttype=\"1\">\r\n"
    "\t\t\t<obj\r\n"
    "\t\t\t\tnum=\"1\"/>\r\n"
    "\t\t\t<obj\r\n"
    "\t\t\t\tnum=\"0\"/>\r\n"
    "\t\t</group>\r\n"
    "\t\t<group\r\n"
    "\t\t\tname=\"NO_Hero\"\r\n"
    "\t\t\ttype=\"0\">\r\n"
    "\t\t\t<obj\r\n"
    "\t\t\t\tnum=\"1\"/>\r\n"
    "\t\t</group>\r\n"
    "\t</mapobject>\r\n";

}  // namespace

TEST(map_writer_writes_what_it_read_byte_for_byte) {
  const Result<MapObjectList> parsed = MapObjectList::parse(bytes(kDocument));
  REQUIRE(parsed.ok());
  // The attributes this engine does not read are on the object, in order.
  const MapObject& hero = parsed->objects()[1];
  CHECK(hero.attributes.size() == 17);
  CHECK(hero.attribute("Level") == "12");
  CHECK(hero.attribute("slot0") == "King's Belt");
  CHECK(hero.attribute("nothing").empty());
  CHECK(write_map_objects(parsed.value()) == kDocument);

  // An empty document is the four blank templates' 29 bytes.
  const Result<MapObjectList> empty = MapObjectList::parse(bytes("\t<mapobject>\r\n\t</mapobject>\r\n"));
  REQUIRE(empty.ok());
  CHECK(write_map_objects(empty.value()) == "\t<mapobject>\r\n\t</mapobject>\r\n");
}

TEST(map_writer_carries_an_edit_through_the_typed_fields) {
  Result<MapObjectList> parsed = MapObjectList::parse(bytes(kDocument));
  REQUIRE(parsed.ok());
  MapObjectList& list = parsed.value();
  // The hero moved and turned; its skills, level and belt come along.
  MapObject& hero = list.objects_mut()[1];
  hero.x = 3100;
  hero.y = 4100;
  hero.dir_x = 1;
  hero.dir_y = 0;
  hero.player = 2;
  hero.flags = 0x80800002u;
  hero.health_percent = 40;
  const std::string written = write_map_objects(list);
  CHECK(written.find("\tx=\"3100\"\r\n\ty=\"4100\"\r\n\tflags=\"0x80800002\"\r\n\tdir.x=\"1\"\r\n\tdir.y=\"0\"/>") != std::string::npos);
  CHECK(written.find("\tplayer=\"2\"\r\n\thealthperc=\"40\"\r\n\tstamina=\"10\"") != std::string::npos);
  CHECK(written.find("\thsTeamAttack=\"2\"\r\n\thsHealing=\"1\"\r\n\tdisplay_name=\"Marius\"\r\n\tLevel=\"12\"") != std::string::npos);
  const Result<MapObjectList> again = MapObjectList::parse(bytes(written));
  REQUIRE(again.ok());
  CHECK(again->objects()[1].x == 3100);
  CHECK(again->objects()[1].player == 2);
  CHECK(again->objects()[1].health_percent == 40);
  CHECK(again->objects()[1].attribute("Level") == "12");
}

TEST(map_writer_carries_the_sheets_untyped_attributes) {
  // The editor's property sheet writes `Level`, `stamina`, `slotN` and
  // `display_name` through `set_attribute`: an existing one in place, a
  // new one before the positional tail, and `drop_attribute` takes one out.
  Result<MapObjectList> parsed = MapObjectList::parse(bytes(kDocument));
  REQUIRE(parsed.ok());
  MapObjectList& list = parsed.value();
  MapObject& hero = list.objects_mut()[1];
  hero.set_attribute("Level", "6");
  hero.set_attribute("slot2", "Healing water");
  hero.drop_attribute("slot0");
  hero.set_attribute("Level", "7");
  const std::string written = write_map_objects(list);
  CHECK(written.find("\tLevel=\"7\"") != std::string::npos);
  CHECK(written.find("\tLevel=\"6\"") == std::string::npos);
  CHECK(written.find("slot0=") == std::string::npos);
  CHECK(written.find("\tslot2=\"Healing water\"\r\n\tx=\"") != std::string::npos);
  const Result<MapObjectList> again = MapObjectList::parse(bytes(written));
  REQUIRE(again.ok());
  CHECK(again->objects()[1].attribute("Level") == "7");
  CHECK(again->objects()[1].attribute("slot2") == "Healing water");
  CHECK(again->objects()[1].attribute("slot0").empty());
}

TEST(map_writer_gives_a_fresh_object_the_shipped_tail_and_nothing_invented) {
  Result<MapObjectList> parsed = MapObjectList::parse(bytes(kDocument));
  REQUIRE(parsed.ok());
  MapObjectList& list = parsed.value();
  CHECK(list.next_num() == 4);
  MapObject placed;
  placed.class_name = "GBarracks";
  placed.num = list.next_num();
  placed.x = 700;
  placed.y = 800;
  placed.player = 3;
  placed.flags = 0x80800004u;
  placed.settlement = 0;  // inside the stronghold's settlement
  list.objects_mut().push_back(placed);
  MapObject scout;
  scout.class_name = "RScout";
  scout.num = 5;
  scout.x = 1;
  scout.y = 2;
  scout.unit_flags = 262144;
  scout.destination_set = 7;
  scout.health_absolute = 250;
  list.objects_mut().push_back(scout);
  const std::string written = write_map_objects(list);
  // `class, num, player, healthperc, x, y, flags, dir.x, dir.y` -- and no
  // `stamina`, `inventorysize` or `Level` made up for it.
  const std::string_view barracks =
      "\t<scriptobj\r\n\t\tclass=\"GBarracks\"\r\n\t\tnum=\"4\"\r\n\tplayer=\"3\"\r\n\thealthperc=\"100\"\r\n"
      "\tx=\"700\"\r\n\ty=\"800\"\r\n\tflags=\"0x80800004\"\r\n\tdir.x=\"0\"\r\n\tdir.y=\"1\"/>\r\n\t\t</settlement>";
  CHECK(written.find(barracks) != std::string::npos);
  const std::string_view scout_element =
      "\t<scriptobj\r\n\t\tclass=\"RScout\"\r\n\t\tnum=\"5\"\r\n\tdestination_set=\"7\"\r\n\tUnitFlags=\"262144\"\r\n"
      "\tplayer=\"0\"\r\n\thealth=\"250\"\r\n\tx=\"1\"\r\n\ty=\"2\"\r\n\tflags=\"0x00000000\"\r\n\tdir.x=\"0\"\r\n\tdir.y=\"1\"/>";
  CHECK(written.find(scout_element) != std::string::npos);
  CHECK(written.find("stamina=\"10\"\r\n\tinventorysize=\"2\"") != std::string::npos);  // the hero keeps its own
  const Result<MapObjectList> again = MapObjectList::parse(bytes(written));
  REQUIRE(again.ok());
  REQUIRE(again->objects().size() == 6);
  // Free objects come first, then the settlements' -- the barracks is last.
  const MapObject* barracks_back = again->find(4);
  const MapObject* scout_back = again->find(5);
  REQUIRE(barracks_back != nullptr && scout_back != nullptr);
  CHECK(barracks_back->settlement == 0);
  CHECK(barracks_back->class_name == "GBarracks");
  CHECK(&again->objects().back() == barracks_back);
  CHECK(scout_back->health_absolute == 250);
  CHECK(scout_back->destination_set == 7);
  CHECK(scout_back->unit_flags == 262144u);
}

TEST(map_writer_writes_an_area_from_its_table_and_a_settlement_from_its_fields) {
  Result<MapObjectList> parsed = MapObjectList::parse(bytes(kDocument));
  REQUIRE(parsed.ok());
  MapObjectList& list = parsed.value();
  // The circle grew; the rectangle is a new area on a new object.
  list.areas_mut()[0].radius = 99;
  MapObject box;
  box.class_name = "AdvArea";
  box.num = 4;
  box.x = 10;
  box.y = 10;
  box.player = 1;
  list.objects_mut().push_back(box);
  MapArea rect;
  rect.num = 4;
  rect.type = kAreaRectangle;
  rect.left = 1;
  rect.top = 2;
  rect.right = 3;
  rect.bottom = 4;
  list.areas_mut().push_back(rect);
  // A new village settlement with only the fields the editor knows.
  MapSettlement village;
  village.id = 1;
  village.player = 2;
  village.class_of_first_building = "MutableVillage";
  village.max_population = 30;
  village.gold = 0;
  village.name = "Hamlet";
  list.settlements_mut().push_back(village);
  const std::string written = write_map_objects(list);
  CHECK(written.find("\tr=\"99\"\r\n") != std::string::npos);
  CHECK(written.find("\tnextmap=\"\"\r\n\ttargetarea=\"\"\r\n\ttype=\"0\"\r\n\ttop=\"2\"\r\n\tbottom=\"4\"\r\n\tright=\"3\"\r\n\tleft=\"1\"\r\n\tplayer=\"1\"") != std::string::npos);
  CHECK(written.find("\t\t<settlement\r\n\t\t\tid=\"1\"\r\n\t\t\tplayer=\"2\"\r\n\t\t\tclassoffirstbuilding=\"MutableVillage\"\r\n\t\t\tmaxpopulation=\"30\"\r\n\t\t\tgold=\"0\"\r\n\t\t\tname=\"Hamlet\">\r\n\t\t</settlement>") != std::string::npos);
  const Result<MapObjectList> again = MapObjectList::parse(bytes(written));
  REQUIRE(again.ok());
  REQUIRE(again->areas().size() == 2);
  CHECK(again->areas()[0].radius == 99);
  CHECK(again->areas()[1].left == 1 && again->areas()[1].bottom == 4);
  REQUIRE(again->settlements().size() == 2);
  CHECK(again->settlements()[1].max_population == 30);
  CHECK(again->settlements()[1].food == -1);
}

TEST(map_writer_erase_takes_the_area_and_the_group_references_with_the_object) {
  Result<MapObjectList> parsed = MapObjectList::parse(bytes(kDocument));
  REQUIRE(parsed.ok());
  MapObjectList& list = parsed.value();
  CHECK(!list.erase(99));
  CHECK(list.erase(1));  // the hero: in the army and the alias
  CHECK(list.objects().size() == 3);
  REQUIRE(list.groups().size() == 1);  // the alias, left empty, went with it
  CHECK(list.groups()[0].name == "Q_Ambush");
  CHECK(list.groups()[0].members.size() == 1);
  CHECK(list.erase(2));
  CHECK(list.areas().empty());
  const std::string written = write_map_objects(list);
  CHECK(written.find("RHero1") == std::string::npos);
  CHECK(written.find("<group\r\n\t\t\tname=\"Q_Ambush\"\r\n\t\t\ttype=\"1\">\r\n\t\t\t<obj\r\n\t\t\t\tnum=\"0\"/>\r\n\t\t</group>") != std::string::npos);
  // `&`, `<` and `"` are the escapes; `'` is not.
  MapObject odd;
  odd.class_name = "X";
  odd.num = 9;
  odd.attributes.emplace_back("display_name", "A & B < \"C\" 'D'");
  list.objects_mut().push_back(odd);
  const std::string escaped = write_map_objects(list);
  CHECK(escaped.find("display_name=\"A &amp; B &lt; &quot;C&quot; 'D'\"") != std::string::npos);
  const Result<MapObjectList> again = MapObjectList::parse(bytes(escaped));
  REQUIRE(again.ok());
  REQUIRE(again->find(9) != nullptr);
  CHECK(again->find(9)->attribute("display_name") == "A & B < \"C\" 'D'");
}
