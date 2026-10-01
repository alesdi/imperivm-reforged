// The help document: topics nested, entries with their links, images and
// faces, links resolved from where they stand.

#include <cstddef>
#include <span>
#include <string_view>

#include "imperivm/core/game/help.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::core::game::HelpDocument;
using imperivm::core::game::parse_tips;

namespace {

std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

constexpr std::string_view kHelp = "\xEF\xBB\xBF"
    R"(<?xml version="1.0"?>
<help language="english" vspacing="10">
  <topic id="contents" vcenter="1">
    <entry hcenter="1" font="large">Basics</entry>
    <entry link="fog">Fog &amp; war</entry>
    <entry/>
    <entry link="units"> Units </entry>
    <topic id="fog">
      <entry hcenter="1" font="large">Fog</entry>
      <entry>
Some parts   are dark.

Others are   lit.
      </entry>
      <entry image="" link="/contents/units/RHastatus">See the hastatus</entry>
    </topic>
    <topic id="units">
      <entry vcenter="1" image="UI/icons/RHastatus.bmp" link="RHastatus"> Hastatus </entry>
      <topic id="RHastatus">
        <entry>Citt&#224; e legion&#xE0;ri</entry>
        <entry link="/contents/fog">Back to the fog</entry>
      </topic>
    </topic>
  </topic>
</help>)";

}  // namespace

TEST(help_parses_topics_entries_and_links) {
  Result<HelpDocument> parsed = HelpDocument::parse(as_bytes(kHelp));
  REQUIRE(parsed.ok());
  const HelpDocument& help = parsed.value();
  REQUIRE(help.topics().size() == 4);
  CHECK(help.home() == 0);
  const auto& contents = help.topics()[0];
  CHECK(contents.id == "contents");
  CHECK(contents.parent == -1);
  REQUIRE(contents.entries.size() == 4);
  CHECK(contents.entries[0].text == "Basics");
  CHECK(contents.entries[0].large);
  CHECK(contents.entries[0].centred);
  CHECK(contents.entries[1].text == "Fog & war");
  CHECK(contents.entries[1].link == "fog");
  CHECK(contents.entries[2].text.empty());
  CHECK(contents.entries[3].text == "Units");
  REQUIRE(contents.children.size() == 2);
  CHECK(help.topics()[1].id == "fog");
  CHECK(help.topics()[1].parent == 0);
  // Whitespace: runs collapse, line breaks stay, a blank line stays one.
  CHECK(help.topics()[1].entries[1].text == "Some parts are dark.\n\nOthers are lit.");
  const auto& units = help.topics()[2];
  CHECK(units.entries[0].image == "UI/icons/RHastatus.bmp");
  CHECK(units.children.size() == 1);
  // Entities, numeric ones included, land as cp1252.
  CHECK(help.topics()[3].entries[0].text == "Citt\xE0 e legion\xE0ri");
  // Links: a child by id, a path by its last component, from anywhere.
  CHECK(help.resolve("fog", 0) == 1);
  CHECK(help.resolve("RHastatus", 2) == 3);
  CHECK(help.resolve("/contents/units/RHastatus", 1) == 3);
  CHECK(help.resolve("/contents/fog", 3) == 1);
  CHECK(help.resolve("nowhere", 0) == -1);
  CHECK(help.resolve("", 0) == -1);
}

TEST(help_refuses_what_is_not_help) {
  CHECK(!HelpDocument::parse(as_bytes("<notes></notes>")).ok());
  CHECK(!HelpDocument::parse(as_bytes("<help><topic id=\"a\"><entry>x")).ok());
}

TEST(help_tips_parse_text_and_link) {
  constexpr std::string_view kTips =
      "\xEF\xBB\xBF<tips>\r\n"
      "    <tip link=\"/contents/shortcuts\"> <text>Puoi usare CONTROL+1 &amp; CONTROL+2.</text></tip>\r\n"
      "    <tip> <text>  Senza link.  </text></tip>\r\n"
      "    <!-- a comment --><tip link=\"x\"/>\r\n"
      "</tips>\r\n";
  const auto tips = parse_tips(std::span<const std::byte>{reinterpret_cast<const std::byte*>(kTips.data()), kTips.size()});
  REQUIRE(tips.ok());
  REQUIRE(tips->size() == 3);
  CHECK((*tips)[0].link == "/contents/shortcuts");
  CHECK((*tips)[0].text == "Puoi usare CONTROL+1 & CONTROL+2.");
  CHECK((*tips)[1].link.empty());
  CHECK((*tips)[1].text == "Senza link.");
  CHECK((*tips)[2].text.empty() && (*tips)[2].link == "x");
  constexpr std::string_view kNot = "<help></help>";
  CHECK(!parse_tips(std::span<const std::byte>{reinterpret_cast<const std::byte*>(kNot.data()), kNot.size()}).ok());
}
