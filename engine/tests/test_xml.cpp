// XML reader tests.
//
// The acceptance test for this reader is the shipped corpus -- 2,863 documents
// through `imcheck xml` -- but CI has no installation, so what is pinned here
// is everything the corpus cannot show: the malformed inputs, which by
// definition do not exist in retail data, and the exact handling of the cases
// that appear only once or twice in it.

#include <string>
#include <string_view>

#include "imperivm/core/xml.hpp"
#include "test.hpp"

using namespace imperivm::core;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

Result<XmlDocument> parse(std::string_view text) { return XmlDocument::parse(bytes_of(text)); }

/// Every rejection below is checked through this, because "did not crash" is
/// not the property under test: a malformed document must produce an error,
/// never a plausible-looking half-tree.
bool rejects(std::string_view text) { return !parse(text).ok(); }

}  // namespace

TEST(xml_parses_an_element_with_attributes) {
  const auto doc = parse(R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)");
  REQUIRE(doc.ok());
  REQUIRE(doc->size() == 1);
  CHECK(doc->root() == 0);
  CHECK(doc->node(doc->root()).name == "class");
  CHECK(doc->attribute(doc->root(), "id") == "Object");
  CHECK(doc->attribute(doc->root(), "cpp_class") == "CVXDecor");
  CHECK(doc->attribute(doc->root(), "parent").empty());
  CHECK(doc->attribute(doc->root(), "altid").empty());
}

TEST(xml_keeps_children_in_document_order) {
  const auto doc = parse(
      "<class><properties a=\"1\"/><method sig=\"idle\"/><properties b=\"2\"/></class>");
  REQUIRE(doc.ok());
  REQUIRE(doc->size() == 4);

  const NodeIndex first = doc->child(doc->root(), "properties");
  REQUIRE(first != kNoNode);
  CHECK(doc->attribute(first, "a") == "1");

  const NodeIndex second = doc->next(first, "properties");
  REQUIRE(second != kNoNode);
  CHECK(doc->attribute(second, "b") == "2");
  CHECK(doc->next(second, "properties") == kNoNode);

  const NodeIndex method = doc->child(doc->root(), "method");
  REQUIRE(method != kNoNode);
  CHECK(doc->attribute(method, "sig") == "idle");
  CHECK(doc->child(doc->root(), "behavior") == kNoNode);
}

TEST(xml_nests_and_links_parents) {
  const auto doc = parse(R"(<class><defaultcmd target=""><cmd name="move"/></defaultcmd></class>)");
  REQUIRE(doc.ok());
  REQUIRE(doc->size() == 3);
  const NodeIndex block = doc->child(doc->root(), "defaultcmd");
  REQUIRE(block != kNoNode);
  const NodeIndex cmd = doc->child(block, "cmd");
  REQUIRE(cmd != kNoNode);
  CHECK(doc->node(cmd).parent == block);
  CHECK(doc->node(block).parent == doc->root());
  CHECK(doc->node(doc->root()).parent == kNoNode);
}

TEST(xml_skips_declarations_comments_and_processing_instructions) {
  const auto doc = parse(
      "<?xml version=\"1.0\" encoding=\"windows-1252\"?>\n"
      "<?xml-stylesheet href=\"class.xsl\" type=\"text/xsl\"?>\n"
      "<!-- authored by hand -->\n"
      "<class id=\"A\"><!-- inner --><properties x=\"1\"/></class>\n"
      "<!-- trailing -->\n");
  REQUIRE(doc.ok());
  REQUIRE(doc->size() == 2);
  CHECK(doc->node(doc->root()).name == "class");
  CHECK(doc->attribute(doc->child(doc->root(), "properties"), "x") == "1");
}

TEST(xml_skips_cdata_and_character_data) {
  // Five shipped files carry stray text; none of it is meaningful, and the
  // reader must step over it without inventing nodes.
  const auto doc = parse("<doc>text<![CDATA[ raw < & > ]]>more<child/>tail</doc>");
  REQUIRE(doc.ok());
  CHECK(doc->size() == 2);
  CHECK(doc->child(doc->root(), "child") != kNoNode);
}

TEST(xml_decodes_the_five_predefined_entities) {
  const auto doc = parse(R"(<v script="a &lt; b &amp;&amp; c &gt; d &quot;e&quot; &apos;f&apos;"/>)");
  REQUIRE(doc.ok());
  CHECK(doc->attribute(doc->root(), "script") == "a < b && c > d \"e\" 'f'");
}

TEST(xml_decodes_numeric_character_references) {
  const auto doc = parse(R"(<v a="line&#xA;break" b="&#65;&#x42;&#x63;"/>)");
  REQUIRE(doc.ok());
  CHECK(doc->attribute(doc->root(), "a") == "line\nbreak");
  CHECK(doc->attribute(doc->root(), "b") == "ABc");
}

TEST(xml_decoded_values_survive_the_document_being_moved) {
  // The decode buffer is a std::string inside the document. If it were small
  // enough for the small-string optimisation, moving the document would move
  // the bytes and every decoded view would dangle -- so the buffer is reserved
  // past the inline capacity of every standard library we build against.
  auto parsed = parse(R"(<v a="&amp;"/>)");
  REQUIRE(parsed.ok());
  XmlDocument moved = std::move(*parsed);
  CHECK(moved.attribute(moved.root(), "a") == "&");
}

TEST(xml_accepts_single_quoted_attribute_values) {
  const auto doc = parse("<v a='one' b=\"two\"/>");
  REQUIRE(doc.ok());
  CHECK(doc->attribute(doc->root(), "a") == "one");
  CHECK(doc->attribute(doc->root(), "b") == "two");
}

TEST(xml_tolerates_whitespace_everywhere_it_is_allowed) {
  const auto doc = parse("<v\n\ta\t=\n\"1\"\n/>");
  REQUIRE(doc.ok());
  CHECK(doc->attribute(doc->root(), "a") == "1");
}

TEST(xml_skips_a_byte_order_mark) {
  const auto doc = parse("\xEF\xBB\xBF<v a=\"1\"/>");
  REQUIRE(doc.ok());
  CHECK(doc->node(doc->root()).name == "v");
}

TEST(xml_rejects_malformed_documents) {
  CHECK(rejects(""));
  CHECK(rejects("   \n  "));
  CHECK(rejects("not xml at all"));
  CHECK(rejects("<class>"));                       // never closed
  CHECK(rejects("<class></klass>"));               // mismatched end tag
  CHECK(rejects("<class><a></class>"));            // child never closed
  CHECK(rejects("<class></class></class>"));       // one end tag too many
  CHECK(rejects("<a/><b/>"));                      // two top-level elements
  CHECK(rejects("<class id=\"a\" />junk"));        // junk after the root
  CHECK(rejects("<class id=>"));                   // no value
  CHECK(rejects("<class id=\"unterminated/>"));    // no closing quote
  CHECK(rejects("<class id=\"a\"altid=\"b\"/>"));  // no space between attributes
  CHECK(rejects("<class id=a/>"));                 // unquoted value
  CHECK(rejects("<class id=\"a<b\"/>"));           // raw < in a value
  CHECK(rejects("<class <nested/>/>"));
  CHECK(rejects("<1class/>"));                     // not a name
  CHECK(rejects("<class><!-- unterminated </class>"));
  CHECK(rejects("<class><![CDATA[ unterminated </class>"));
  CHECK(rejects("<?xml version=\"1.0\"<class/>"));  // unterminated PI
}

TEST(xml_rejects_what_it_deliberately_does_not_support) {
  // Refused rather than half-supported: a DTD subset could redefine entities,
  // and quietly ignoring it would mean silently reading a different document.
  const auto doctype = parse("<!DOCTYPE class SYSTEM \"class.dtd\"><class/>");
  CHECK(!doctype.ok());
  CHECK(doctype.error() == FormatError::unsupported);

  // The one named entity in the install outside the predefined five is in a
  // saved web page (update.xml), not in a game file.
  const auto named = parse(R"(<v a="Haemimont Games &raquo; Feed"/>)");
  CHECK(!named.ok());
  CHECK(named.error() == FormatError::unsupported);

  // Documents are cp1252 and every view handed out is bytes, so there is no
  // single-byte spelling for a reference above U+00FF.
  const auto wide = parse(R"(<v a="&#x2014;"/>)");
  CHECK(!wide.ok());
  CHECK(wide.error() == FormatError::unsupported);

  CHECK(rejects(R"(<v a="&notanentity;"/>)"));
  CHECK(rejects(R"(<v a="&#;"/>)"));
  CHECK(rejects(R"(<v a="&amp"/>)"));   // no semicolon
  CHECK(rejects(R"(<v a="&#xZZ;"/>)"));
}

TEST(xml_survives_deeply_nested_input) {
  // The parser runs from a heap stack, not the C++ one, because the depth of a
  // document is attacker-controlled and the retail corpus is only 7 deep.
  constexpr int kDepth = 20000;
  std::string text;
  text.reserve(kDepth * 8);
  for (int i = 0; i < kDepth; ++i) text += "<a>";
  for (int i = 0; i < kDepth; ++i) text += "</a>";

  const auto doc = parse(text);
  REQUIRE(doc.ok());
  CHECK(doc->size() == kDepth);
}

TEST(xml_attribute_int_reads_integers) {
  const auto doc = parse(R"(<p radius="245" offset="-842150451" plus="+7" zero="0"/>)");
  REQUIRE(doc.ok());
  CHECK(doc->attribute_int(doc->root(), "radius") == 245);
  CHECK(doc->attribute_int(doc->root(), "offset") == -842150451);
  CHECK(doc->attribute_int(doc->root(), "plus") == 7);
  CHECK(doc->attribute_int(doc->root(), "zero", 9) == 0);
}

TEST(xml_attribute_int_treats_malformed_as_absent) {
  // `pass_file=""` ships in 885 entity files. A loader that aborted on it
  // would refuse the retail data, so an unreadable integer reads as absent.
  const auto doc = parse(R"(<p empty="" words="none" mixed="12ab" huge="99999999999" nothing=" "/>)");
  REQUIRE(doc.ok());
  CHECK(doc->attribute_int(doc->root(), "empty", -1) == -1);
  CHECK(doc->attribute_int(doc->root(), "words", -1) == -1);
  CHECK(doc->attribute_int(doc->root(), "mixed", -1) == -1);
  CHECK(doc->attribute_int(doc->root(), "huge", -1) == -1);
  CHECK(doc->attribute_int(doc->root(), "nothing", -1) == -1);
  CHECK(doc->attribute_int(doc->root(), "missing", 42) == 42);
}

TEST(xml_attribute_bool_accepts_every_spelling_the_data_uses) {
  const auto doc = parse(
      R"(<p auto_repair="no" xray="0" on="1" yes="yes" t="true" f="false" caps="YES" junk="maybe"/>)");
  REQUIRE(doc.ok());
  CHECK(doc->attribute_bool(doc->root(), "auto_repair", true) == false);
  CHECK(doc->attribute_bool(doc->root(), "xray", true) == false);
  CHECK(doc->attribute_bool(doc->root(), "on") == true);
  CHECK(doc->attribute_bool(doc->root(), "yes") == true);
  CHECK(doc->attribute_bool(doc->root(), "t") == true);
  CHECK(doc->attribute_bool(doc->root(), "f", true) == false);
  CHECK(doc->attribute_bool(doc->root(), "caps") == true);
  CHECK(doc->attribute_bool(doc->root(), "junk", true) == true);
  CHECK(doc->attribute_bool(doc->root(), "absent", true) == true);
}

TEST(xml_lookups_are_case_sensitive_and_bounded) {
  const auto doc = parse(R"(<p Radius="1"/>)");
  REQUIRE(doc.ok());
  CHECK(doc->attribute(doc->root(), "radius").empty());
  CHECK(doc->attribute(doc->root(), "Radius") == "1");
  // Out-of-range handles answer rather than misbehave, matching result.hpp's
  // contract that a caller who forgets to check gets boring output.
  CHECK(doc->attribute(kNoNode, "Radius").empty());
  CHECK(doc->child(kNoNode, "x") == kNoNode);
  CHECK(doc->next(kNoNode, "x") == kNoNode);
}
