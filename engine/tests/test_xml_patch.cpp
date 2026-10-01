// The in-place XML patcher: an attribute rewritten where it stands, a new
// one on its own line, an element cut out, an element laid in -- and every
// other byte of the document as it was.

#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/xml_patch.hpp"
#include "test.hpp"

using namespace imperivm::core;

namespace {

constexpr std::string_view kMap =
    "\t<map\r\n"
    "\t\tname=\"Numantia\"\r\n"
    "\t\tdescr=\"\"\r\n"
    "\t\tpersist_state=\"0\">\r\n"
    "\t\t<size\r\n"
    "\t\t\tx=\"16384\"\r\n"
    "\t\t\ty=\"16384\"/>\r\n"
    "\t\t<expl\r\n"
    "\t\t\tNoFog=\"0\"\r\n"
    "\t\t\tNoExplore=\"0\"/>\r\n"
    "\t</map>\r\n";

constexpr std::string_view kNotes =
    "\t<notes>\r\n"
    "\t\t<note\r\n"
    "\t\t\tid=\"GOAL\"\r\n"
    "\t\t\ttitle=\"Numantia\"\r\n"
    "\t\t\tlocationx=\"15231\"/>\r\n"
    "\t\t<note\r\n"
    "\t\t\tid=\"LoseCond\"\r\n"
    "\t\t\ttitle=\"Scipio\"/>\r\n"
    "\t</notes>\r\n";

}  // namespace

TEST(xml_patch_rewrites_an_attribute_in_place_and_adds_one_on_its_own_line) {
  const XmlEdit edits[] = {{"name", "Numantia & \"co\""}, {"author", "me"}};
  bool found = false;
  const std::string out = xml_set_attributes(kMap, "map", edits, &found);
  CHECK(found);
  CHECK(out.find("\t<map\r\n\t\tname=\"Numantia &amp; &quot;co&quot;\"\r\n\t\tdescr=\"\"\r\n\t\tpersist_state=\"0\"\r\n\t\tauthor=\"me\">\r\n") == 0);
  // The rest is untouched.
  CHECK(out.substr(out.find("\t\t<size")) == std::string(kMap.substr(kMap.find("\t\t<size"))));
  // A nested element by path; a self-closing tag keeps its `/>`.
  const XmlEdit fog[] = {{"NoFog", "1"}};
  const std::string patched = xml_set_attributes(kMap, "map/expl", fog, &found);
  CHECK(found);
  CHECK(patched.find("\t\t<expl\r\n\t\t\tNoFog=\"1\"\r\n\t\t\tNoExplore=\"0\"/>\r\n") != std::string::npos);
  CHECK(xml_get_attribute(patched, "map/expl", "NoFog") == "1");
  CHECK(xml_get_attribute(patched, "map/size", "y") == "16384");
  // A path that matches nothing changes nothing.
  CHECK(xml_set_attributes(kMap, "map/start_pt", fog, &found) == std::string(kMap));
  CHECK(!found);
}

TEST(xml_patch_picks_a_sibling_by_key_erases_it_and_appends_another) {
  const XmlEdit edits[] = {{"title", "Scipio Amelianus"}};
  bool found = false;
  std::string out = xml_set_attributes(kNotes, "notes/note[id=LoseCond]", edits, &found);
  CHECK(found);
  CHECK(out.find("\t\t\tid=\"LoseCond\"\r\n\t\t\ttitle=\"Scipio Amelianus\"/>") != std::string::npos);
  CHECK(out.find("title=\"Numantia\"") != std::string::npos);
  out = xml_erase_element(out, "notes/note[id=GOAL]", &found);
  CHECK(found);
  CHECK(out.find("GOAL") == std::string::npos);
  CHECK(out.find("\t<notes>\r\n\t\t<note\r\n\t\t\tid=\"LoseCond\"") == 0);
  const XmlEdit fresh[] = {{"id", "New"}, {"title", "A <new> note"}};
  out = xml_append_element(out, "notes", "note", fresh, &found);
  CHECK(found);
  CHECK(out.find("\t\t\ttitle=\"Scipio Amelianus\"/>\r\n\t\t<note\r\n\t\t\tid=\"New\"\r\n\t\t\ttitle=\"A &lt;new> note\"/>\r\n\t</notes>\r\n") != std::string::npos);
  CHECK(xml_get_attribute(out, "notes/note[id=New]", "title") == "A &lt;new> note");
}

TEST(xml_patch_follows_a_keyed_step_into_a_child) {
  constexpr std::string_view kItems =
      "\t<items>\r\n"
      "\t\t<item\r\n\t\t\tid=\"A\">\r\n\t\t\t<bonus\r\n\t\t\t\tdamage=\"1\"/>\r\n\t\t</item>\r\n"
      "\t\t<item\r\n\t\t\tid=\"B\">\r\n\t\t\t<bonus\r\n\t\t\t\tdamage=\"2\"/>\r\n\t\t</item>\r\n"
      "\t</items>\r\n";
  const XmlEdit edits[] = {{"damage", "9"}};
  bool found = false;
  const std::string out = xml_set_attributes(kItems, "items/item[id=B]/bonus", edits, &found);
  CHECK(found);
  CHECK(xml_get_attribute(out, "items/item[id=A]/bonus", "damage") == "1");
  CHECK(xml_get_attribute(out, "items/item[id=B]/bonus", "damage") == "9");
  // Erasing the keyed element takes its children with it.
  const std::string cut = xml_erase_element(out, "items/item[id=A]", &found);
  CHECK(found);
  CHECK(cut.find("id=\"A\"") == std::string::npos);
  CHECK(xml_get_attribute(cut, "items/item[id=B]/bonus", "damage") == "9");
}

TEST(xml_patch_indexes_siblings_and_swaps_two) {
  constexpr std::string_view kConv =
      "\t<conversation\r\n\t\tname=\"C\">\r\n"
      "\t\t<actor\r\n\t\t\tname=\"A\"/>\r\n"
      "\t\t<phrase\r\n\t\t\ttext=\"one\"/>\r\n"
      "\t\t<phrase\r\n\t\t\ttext=\"two\"/>\r\n"
      "\t\t<phrase\r\n\t\t\ttext=\"three\"/>\r\n"
      "\t</conversation>\r\n";
  CHECK(xml_get_attribute(kConv, "conversation/phrase[0]", "text") == "one");
  CHECK(xml_get_attribute(kConv, "conversation/phrase[2]", "text") == "three");
  CHECK(xml_get_attribute(kConv, "conversation/phrase[3]", "text").empty());
  bool found = false;
  const std::string swapped = xml_swap_elements(kConv, "conversation/phrase[0]", "conversation/phrase[2]", &found);
  CHECK(found);
  CHECK(xml_get_attribute(swapped, "conversation/phrase[0]", "text") == "three");
  CHECK(xml_get_attribute(swapped, "conversation/phrase[1]", "text") == "two");
  CHECK(xml_get_attribute(swapped, "conversation/phrase[2]", "text") == "one");
  CHECK(swapped.size() == kConv.size());
  const XmlEdit edits[] = {{"text", "due"}};
  const std::string set = xml_set_attributes(swapped, "conversation/phrase[1]", edits, &found);
  CHECK(found && xml_get_attribute(set, "conversation/phrase[1]", "text") == "due");
  const std::string cut = xml_erase_element(set, "conversation/phrase[0]", &found);
  CHECK(found && xml_get_attribute(cut, "conversation/phrase[0]", "text") == "due");
}

TEST(xml_patch_counts_siblings) {
  // The shape `labels.xml` has -- tabs, CRLF, one attribute a line -- with
  // names and coordinates invented here. `docs/legal.md` rule 1: a fixture
  // is written from the specification, never copied out of the game.
  constexpr std::string_view kLabels =
      "\t<root>\r\n"
      "\t\t<label\r\n\t\t\ttext=\"Alpha\"\r\n\t\t\tx=\"1111\"\r\n\t\t\ty=\"222\"/>\r\n"
      "\t\t<label\r\n\t\t\ttext=\"Beta\"\r\n\t\t\tx=\"3333\"\r\n\t\t\ty=\"4444\"/>\r\n"
      "\t</root>\r\n";
  CHECK(xml_count_elements(kLabels, "root/label") == 2);
  CHECK(xml_count_elements(kLabels, "root/note") == 0);
  CHECK(xml_count_elements("\t<root>\r\n\t</root>\r\n", "root/label") == 0);
  CHECK(xml_count_elements("", "root/label") == 0);
  CHECK(xml_count_elements(kNotes, "notes/note") == 2);
  // A label laid into the empty document takes the shape of the shipped ones.
  bool found = false;
  const XmlEdit attributes[] = {{"text", "<New label>"}, {"x", "100"}, {"y", "200"}};
  const std::string one = xml_append_element("\t<root>\r\n\t</root>\r\n", "root", "label", attributes, &found);
  CHECK(found);
  // `<` and `&` are escaped as `write_map_objects` escapes them; `>` is not.
  CHECK(one == "\t<root>\r\n\t\t<label\r\n\t\t\ttext=\"&lt;New label>\"\r\n\t\t\tx=\"100\"\r\n\t\t\ty=\"200\"/>\r\n\t</root>\r\n");
  CHECK(xml_count_elements(one, "root/label") == 1);
  CHECK(xml_decode_entities(xml_get_attribute(one, "root/label[0]", "text")) == "<New label>");
  CHECK(xml_decode_entities("a &amp; b &quot;c&quot; &#65; &bogus; &unterminated") == "a & b \"c\" A &bogus; &unterminated");
}

TEST(xml_patch_lays_an_element_with_a_child_in) {
  constexpr std::string_view kItems = "\t<items>\r\n\t</items>\r\n";
  const XmlEdit item[] = {{"id", "Ash"}, {"name", "Ash"}};
  const XmlEdit bonus[] = {{"health", "0"}, {"damage", "10"}};
  bool found = false;
  const std::string out = xml_append_element(kItems, "items", "item", item, "bonus", bonus, &found);
  CHECK(found);
  CHECK(out ==
        "\t<items>\r\n"
        "\t\t<item\r\n\t\t\tid=\"Ash\"\r\n\t\t\tname=\"Ash\">\r\n"
        "\t\t\t<bonus\r\n\t\t\t\thealth=\"0\"\r\n\t\t\t\tdamage=\"10\"/>\r\n"
        "\t\t</item>\r\n"
        "\t</items>\r\n");
  CHECK(xml_get_attribute(out, "items/item[id=Ash]/bonus", "damage") == "10");
  // The next item goes after it, and the first is untouched.
  const XmlEdit second[] = {{"id", "Bone"}};
  const std::string two = xml_append_element(out, "items", "item", second, "bonus", {}, &found);
  CHECK(found && xml_count_elements(two, "items/item") == 2);
  CHECK(two.find(out.substr(0, out.size() - std::string_view("\t</items>\r\n").size())) == 0);
  CHECK(xml_get_attribute(two, "items/item[id=Bone]", "id") == "Bone");
  // An indexed step counts an element once, whether it closes on its own
  // line or in its tag -- the sequences' `<sequence ...></sequence>` shape.
  CHECK(xml_get_attribute(two, "items/item[1]", "id") == "Bone");
  CHECK(xml_get_attribute(two, "items/item[1]/bonus", "health").empty());
  CHECK(xml_get_attribute(two, "items/item[0]/bonus", "damage") == "10");
}
