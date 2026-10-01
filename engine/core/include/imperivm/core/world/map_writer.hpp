#pragma once

/// `map.obj.xml`, written back.
///
/// The one map document the editor changes. The shape is the shipped one to
/// the byte -- `docs/formats/map.md`, "Writing": a tab-indented element per
/// line with CRLF ends, `\t<mapobject>`, settlements at two tabs with their
/// attributes at three, every `<scriptobj>` at one tab with `class` and `num`
/// at two and the rest at one, groups at two tabs with `<obj>` at three and
/// `num` at four. The attribute order is the element's own, kept verbatim on
/// parse (`MapObject::attributes`) and brought up to date from the typed
/// fields before writing (`sync_attributes`), so that an unedited map writes
/// back byte for byte and an edited object keeps the attributes this engine
/// does not read -- a hero's skills, a unit's `Level`, a wagon's `amount`.
///
/// What a *new* object gets is the shipped tail alone: `class`, `num`,
/// `player`, `healthperc`, `x`, `y`, `flags`, `dir.x`, `dir.y`, plus
/// `UnitFlags`, `health` and `destination_set` when they carry a value, and
/// an area's `type` and shape when the list holds one for it. `stamina`,
/// `inventorysize`, `Level` and `data` are **not invented**: the original
/// writes them from the class hierarchy and what a class without them loads
/// as is not read.
///
/// `&`, `<` and `"` are escaped in attribute values; `'` and `>` are written
/// raw, as `slot1="King's Belt"` ships. No shipped document carries an entity.

#include <string>

#include "imperivm/core/world/map.hpp"

namespace imperivm::core {

/// The document, as `MapObjectList::parse` reads it back. Objects with a
/// `settlement` index are written inside that settlement's element, in list
/// order; the rest and the groups follow in list order. Areas are written as
/// the shape attributes of their object.
[[nodiscard]] std::string write_map_objects(const MapObjectList& list);

}  // namespace imperivm::core
