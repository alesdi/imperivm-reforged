#include "imperivm/core/world/map_writer.hpp"

#include <algorithm>
#include <utility>

namespace imperivm::core {
namespace {

constexpr std::string_view kCrlf = "\r\n";

/// The three characters a double-quoted attribute cannot hold raw. `'` and
/// `>` stay as they are: `slot1="King's Belt"` ships that way.
void escape_into(std::string& out, std::string_view value) {
  for (const char c : value) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c); break;
    }
  }
}

/// `<tabs>name="value"` on its own line.
void attribute_line(std::string& out, int tabs, std::string_view name, std::string_view value) {
  out.append(static_cast<std::size_t>(tabs), '\t');
  out.append(name);
  out += "=\"";
  escape_into(out, value);
  out.push_back('"');
}

/// The area's attributes onto its object, in the shipped order and place:
/// `nextmap`, `targetarea`, `type`, then the shape, all before `player`.
void put_area(std::vector<std::pair<std::string, std::string>>& attributes, const MapArea& area) {
  const auto set = [&](std::string_view name, std::string value) {
    for (auto& entry : attributes) {
      if (entry.first == name) {
        entry.second = std::move(value);
        return;
      }
    }
    auto at = std::find_if(attributes.begin(), attributes.end(),
                           [](const auto& entry) { return entry.first == "player"; });
    if (at == attributes.end()) {
      at = std::find_if(attributes.begin(), attributes.end(),
                        [](const auto& entry) { return entry.first == "x"; });
    }
    attributes.emplace(at, std::string(name), std::move(value));
  };
  // Present on all 846 shipped areas, empty on all of them; kept as the
  // element's own when it already has them.
  set("nextmap", std::string(""));
  set("targetarea", std::string(""));
  set("type", std::to_string(area.type));
  if (area.is_circle()) {
    set("ptx", std::to_string(area.ptx));
    set("pty", std::to_string(area.pty));
    set("r", std::to_string(area.radius));
  } else {
    set("top", std::to_string(area.top));
    set("bottom", std::to_string(area.bottom));
    set("right", std::to_string(area.right));
    set("left", std::to_string(area.left));
  }
}

void write_object(std::string& out, const MapObjectList& list, const MapObject& source) {
  MapObject object = source;
  object.sync_attributes();
  if (const MapArea* area = list.find_area(object.num)) put_area(object.attributes, *area);
  out += "\t<scriptobj";
  out += kCrlf;
  bool first = true;
  for (const auto& [name, value] : object.attributes) {
    if (!first) out += kCrlf;
    first = false;
    // `class` and `num` stand one tab deeper than the rest, in every one of
    // the 25,483 shipped elements.
    attribute_line(out, name == "class" || name == "num" ? 2 : 1, name, value);
  }
  out += "/>";
  out += kCrlf;
}

}  // namespace

std::string write_map_objects(const MapObjectList& list) {
  std::string out;
  out += "\t<mapobject>";
  out += kCrlf;
  // Free objects, then the settlements with their members, then the groups:
  // `o+S+g+` in every shipped document that has all three.
  for (const MapObject& object : list.objects()) {
    if (object.settlement < 0) write_object(out, list, object);
  }
  for (std::size_t index = 0; index < list.settlements().size(); ++index) {
    MapSettlement settlement = list.settlements()[index];
    settlement.sync_attributes();
    out += "\t\t<settlement";
    out += kCrlf;
    bool first = true;
    for (const auto& [name, value] : settlement.attributes) {
      if (!first) out += kCrlf;
      first = false;
      attribute_line(out, 3, name, value);
    }
    out += ">";
    out += kCrlf;
    for (const MapObject& object : list.objects()) {
      if (object.settlement == static_cast<std::int32_t>(index)) write_object(out, list, object);
    }
    out += "\t\t</settlement>";
    out += kCrlf;
  }
  for (const MapGroup& group : list.groups()) {
    out += "\t\t<group";
    out += kCrlf;
    attribute_line(out, 3, "name", group.name);
    out += kCrlf;
    attribute_line(out, 3, "type", std::to_string(group.type));
    out += ">";
    out += kCrlf;
    for (const std::int32_t member : group.members) {
      out += "\t\t\t<obj";
      out += kCrlf;
      attribute_line(out, 4, "num", std::to_string(member));
      out += "/>";
      out += kCrlf;
    }
    out += "\t\t</group>";
    out += kCrlf;
  }
  out += "\t</mapobject>";
  out += kCrlf;
  return out;
}

}  // namespace imperivm::core
