#pragma once

// The world: a playable map's terrain layers, its object list, and the tables
// that give both meaning.
//
// Specifications: docs/formats/map.md (what a map holds),
//                 docs/engine/projection.md (how it reaches the screen).
//
// Everything here is a byte-span-in, state-out transformation, which is why it
// is in core: the platform opens the `.bfhp`, gathers the six `GRID` payloads
// and the two XML documents, and hands them down. Nothing below knows where a
// byte came from and nothing below draws anything.
//
// **The projection lives here too**, as integer arithmetic, because picking is
// its inverse and picking is simulation. A terrain cell is 64 world units wide
// and occupies 64 x 46 screen pixels: horizontal scale exactly 1, vertical
// scale exactly 46/64. There is no projection constant anywhere in the shipped
// data — it was recovered from the transition masks, which are 64 x 46 and
// compiled into the executable — so the two numbers below are the whole camera.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/formats/color.hpp"
#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/formats/result.hpp"
// The simulation's player table, for `load_player_table` below. This is the one
// place the map reader reaches into `sim/`: `player<i>.xml` carries the
// diplomacy matrix, which is simulation state rather than anything the renderer
// can use, and there is no second parse of the same bytes. `sim/player.hpp`
// pulls in nothing but `sim/system.hpp`, so this does not drag the object model
// in behind it.
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core {

// --------------------------------------------------------------------------
// projection
// --------------------------------------------------------------------------

/// World units per terrain cell, from `TERRAIN_TILE_SIZE` in `DATA\CONST.INI`.
inline constexpr std::int32_t kTerrainTileSize = 64;
/// The same cell's screen width in pixels: the horizontal scale is exactly 1.
inline constexpr std::int32_t kTileWidth = 64;
/// And its screen height: `k = 46/64 = 0.71875`, exactly, from the 56 terrain
/// transition masks, every one of which is 64 x 46 and covers one cell.
inline constexpr std::int32_t kTileHeight = 46;

/// Pixels of screen Y per unit of terrain elevation: **1**, recovered from
/// `gbr.exe` (0x005c11a0 subtracts the sampled height verbatim; see
/// `docs/engine/projection.md`). The ground is drawn as a mesh whose
/// vertices carry the displacement (`platform/map_renderer.hpp`,
/// `compose_ground`), and everything standing on it is lifted by the height
/// sampled under its feet (`Camera::project`), so the two meet.
inline constexpr std::int32_t kHeightScaleNumerator = 1;
inline constexpr std::int32_t kHeightScaleDenominator = 1;

/// X passes through unchanged; nothing rotates and nothing shears.
[[nodiscard]] constexpr std::int32_t world_to_screen_x(std::int32_t world_x) noexcept {
  return world_x;
}

/// Y is squashed by 46/64 and lifted by the elevation term.
[[nodiscard]] constexpr std::int32_t world_to_screen_y(std::int32_t world_y,
                                                       std::int32_t elevation = 0) noexcept {
  return world_y * kTileHeight / kTileWidth -
         elevation * kHeightScaleNumerator / kHeightScaleDenominator;
}

/// The inverse, for picking. `world_y` recovers exactly only at multiples of
/// 64, so this rounds rather than truncating.
[[nodiscard]] constexpr std::int32_t screen_to_world_y(std::int32_t screen_y) noexcept {
  return (screen_y * kTileWidth + kTileHeight / 2) / kTileHeight;
}

// --------------------------------------------------------------------------
// map.xml and game.xml
// --------------------------------------------------------------------------

/// `map.xml`: the world square and the camera bookmark.
struct MapGeometry {
  std::string name;
  std::string display_name;
  std::int32_t size_x = 0;  ///< 8192, 16384 or 32768; always equal to size_y
  std::int32_t size_y = 0;
  std::int32_t start_x = 0;
  std::int32_t start_y = 0;
  /// False when `start_pt` is absent or holds 0xCDCDCDCD, which `randommap`
  /// ships: uninitialised memory serialised straight into the file.
  bool has_start = false;

  static Result<MapGeometry> parse(std::span<const std::byte> xml);
};

/// `game.xml`: the container-level properties the renderer needs, which is the
/// season (it selects the `%season%` half of every terrain texture path) and
/// the map to start on.
struct GameProperties {
  std::string name;
  /// `description=`, the paragraph the adventure and conquest menus show.
  std::string description;
  std::string season = "spring";  ///< absent in the four blank templates
  std::int32_t start_map = 1;
  std::int32_t game_type = 0;
  /// `start_player=`, the slot the human takes; 0 when absent.
  std::int32_t start_player = 0;

  static Result<GameProperties> parse(std::span<const std::byte> xml);
};

/// One `player<i>.xml` slot -- only the fields placement and drawing need.
///
/// The owner attribute on an object is **1-based**: `player="1"` is the slot
/// `player0.xml` describes, so subtract one before indexing a table of these.
struct PlayerSlot {
  std::int32_t id = 0;
  std::string name;
  std::string race;
  std::string control;  ///< `Both`, `Computer`, `Disabled`
  /// The player's colour, stored in the file as RGB555 packed into a decimal
  /// integer and expanded here. This is the only per-player colour anywhere in
  /// the shipped data; the palette ramps the engine actually swaps into slots
  /// 0..63 of a unit sheet are not in the data at all.
  Rgb888 color{};
  std::int32_t start_x = 0;
  std::int32_t start_y = 0;

  static Result<PlayerSlot> parse(std::span<const std::byte> xml);
};

/// Sixteen slots, `player0.xml` through `player15.xml`.
inline constexpr std::size_t kPlayerSlots = 16;

/// Parse one `<playerdata>` document into a simulation player table.
///
/// `PlayerSlot` above keeps only what the renderer needs; this fills in the
/// whole element -- setup *and* the player's row of the relations matrix --
/// because the simulation needs both and neither is derivable from the other.
/// `docs/formats/map.md` specifies the element and the relations encoding.
///
/// The document names its own slot: the row written is `playerdata/@id`, not a
/// position the caller supplies. Refuses rather than half-writing:
///
///   * a root element that is not `playerdata`,
///   * a missing or out-of-range `id`,
///   * a missing `relations`, or one that is not exactly 128 hex digits.
///
/// The last of those is the point. A short or misspelt `relations` that quietly
/// zeroed a row would produce a map that loads and then plays as a free-for-all
/// between players the author allied, which is worse than one that refuses to
/// load.
[[nodiscard]] Status parse_player_setup(std::span<const std::byte> xml, sim::PlayerTable& table);

/// Parse a whole map's sixteen `player<i>.xml` documents.
///
/// Every map directory in the retail install carries exactly sixteen -- 288
/// documents across 18 map directories in the 23 readable containers, with `id`
/// a permutation of 0..15 in every one -- so a different count is a container
/// this reader does not understand rather than a table to fill in partially.
///
/// `documents` is indexed by file number, so `documents[3]` is `player3.xml`;
/// the `id` attribute is still what decides which row each one writes, and a
/// duplicated or missing `id` is refused.
///
/// Bytes in, state out: the platform opens the container and gathers the
/// sixteen payloads, exactly as `WorldMap::load` is handed its layers.
[[nodiscard]] Status load_player_table(std::span<const std::span<const std::byte>> documents,
                                       sim::PlayerTable& table);

// --------------------------------------------------------------------------
// map.obj.xml
// --------------------------------------------------------------------------

/// One `<scriptobj>`. Only the fields placement and drawing need are kept;
/// the balance and script fields belong to the simulation and are not read
/// here, so that a renderer does not carry a copy of the object model.
struct MapObject {
  std::string class_name;
  std::int32_t num = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t dir_x = 0;
  std::int32_t dir_y = 1;
  std::uint32_t flags = 0;
  /// 1-based as authored: `player="1"` is the slot `player0.xml` describes.
  /// Zero when the object has no owner.
  std::int32_t player = 0;
  /// Index into `MapObjectList::settlements`, or -1.
  std::int32_t settlement = -1;

  /// `healthperc`: the object's health as a percentage of its class `maxhealth`.
  ///
  /// 100 on 21,547 of the 21,549 objects that carry it, across the 28
  /// `map.obj.xml` documents in the retail install; the two exceptions are one
  /// at 30 and one at 40. Defaults to 100, which is also what an object with no
  /// `healthperc` at all (decor, areas) gets, and for those the product is
  /// simply the class maximum.
  std::int32_t health_percent = 100;
  /// `health`: an **absolute** figure that overrides `healthperc` when present.
  ///
  /// Exactly one object in the whole install carries it -- `<scriptobj
  /// class="Townhall" num="0" health="2000">` in `randommap.BFHP`. Negative
  /// means the attribute was absent, which is the case for every other object;
  /// zero is a legal authored value and must not be confused with absence.
  std::int32_t health_absolute = -1;

  /// `UnitFlags`: the **second** flag word, `[obj+0x194]` in `gbr.exe`, and
  /// not the `flags` attribute above.
  ///
  /// Present on all 16,171 units and on nothing else. Eight distinct values
  /// across the retail install, using five bits, and this project can now name
  /// two of them -- see `docs/formats/map.md`, where the other three are still
  /// open:
  ///
  ///   * **bit 18** (`0x00040000`), on 14,586: `UNITFLAG_NOAI`, the constant
  ///     `Unit::GetFlags` is asked about at every one of its shipped call
  ///     sites. `ObjectFlags::no_ai`.
  ///   * **bit 22** (`0x00400000`), on 46: `Flying::IsInAir`.
  ///     `ObjectFlags::in_air`.
  ///
  /// Carried whole rather than as the two bits, because the other three are
  /// real and unexplained and a reader that dropped them would make them
  /// unrecoverable.
  std::uint32_t unit_flags = 0;

  /// Every attribute of the element, in document order and verbatim -- the
  /// ones the fields above read (`class`, `num`, `x`, ...) and the ones
  /// nothing in this engine reads yet (`Level`, `stamina`, `inventorysize`,
  /// `slot0..3`, the `hs*` skills, `display_name`, `Icon`, `amount`, `data`,
  /// `nextmap`, `targetarea`). Kept so that `write_map_objects` can put the
  /// element back **byte for byte**: the original serialises each class's own
  /// attributes between `num` and `player` in an order the class hierarchy
  /// decides, and a writer that emitted only the fields it understood would
  /// strip a hero of its skills on the first save. The fields above win over
  /// the list when the two disagree (`MapObject::sync_attributes`).
  std::vector<std::pair<std::string, std::string>> attributes;

  /// Write the typed fields back into `attributes`: an existing entry is
  /// overwritten in place, a missing one is inserted where the shipped files
  /// put it (`num` after `class`; `player`, `healthperc`, `x`, `y`, `flags`,
  /// `dir.x`, `dir.y` at the tail; `health`, `UnitFlags`, `destination_set`
  /// only when they carry a value). `flags` is written `0x%08x`, as authored.
  void sync_attributes();
  /// The value of an attribute by name, or empty.
  [[nodiscard]] std::string_view attribute(std::string_view name) const noexcept;
  /// Overwrite an attribute in place, or add it before the positional tail
  /// (`x`, `y`, `flags`, ...) -- where the shipped files keep a class's own
  /// attributes (`stamina`, `Level`, `slot0`). The editor's property sheet
  /// writes the attributes nothing above types this way.
  void set_attribute(std::string_view name, std::string value);
  /// Remove an attribute; nothing when it is absent.
  void drop_attribute(std::string_view name);

  /// `destination_set`: which settlement holds the teleport this one is paired
  /// with, by the target settlement's **`id` attribute** rather than by its
  /// index. -1 when the attribute is absent, which is every object that is not
  /// a teleport.
  ///
  /// 54 teleports across the 28 shipped `map.obj.xml` documents, and three
  /// things hold in all 54, which is what makes the pairing resolvable at all:
  ///
  ///   * **every teleport is inside a `<settlement>`**, and
  ///   * **no settlement holds two of them**, so "the teleport of settlement
  ///     *n*" names exactly one object; and
  ///   * **the relation is symmetric** -- if A names B's settlement then B
  ///     names A's, 54 of 54. The executable stores only one direction (a
  ///     16-bit object id per instance), so a one-way pair would have been
  ///     legal and does not occur.
  ///
  /// The `id` attribute is **not** the index: it differs from document position
  /// on 97 settlements across the install, so resolving this needs the id
  /// table rather than a subscript.
  std::int32_t destination_set = -1;
};

/// The two values an area object's `type` takes.
///
/// **Not the `type` a `CVXWagon` carries**, which is a cargo kind. The two
/// share the attribute name and are told apart by the company they keep: an
/// area carries `ptx`/`pty`/`r` or `left`/`top`/`right`/`bottom` beside it, a
/// wagon carries `amount`. The reader keys off the shape attributes rather
/// than off the class name, so a cargo object cannot be read as a degenerate
/// area however its class happens to be spelt.
inline constexpr std::int32_t kAreaRectangle = 0;
inline constexpr std::int32_t kAreaCircle = 1;

/// One `<scriptobj class="AdvArea">`'s trigger region, as authored.
///
/// 904 ship across the 29 `map.obj.xml` documents -- 661 circles and 243
/// rectangles, with no object carrying attributes of the wrong shape. This is
/// a transcription of the file, not an interpretation of it: `sim/area.hpp`
/// owns the geometry, and it is the simulation rather than the renderer that
/// needs it.
///
/// `nextmap` and `targetarea` are the empty string on all 904 and are not read
/// here; when a fan-made map sets one, this is where they go.
///
/// The object's own `x`/`y` is exactly the centre of the region in all 904
/// cases -- `tests/test_corpus_area.py` pins that -- but `AreaCenter` in
/// `gbr.exe` (0x004d8ce9) computes the centre from the *shape* and never looks
/// at `x`/`y`, so `sim::AreaShape::centre` does the same.
struct MapArea {
  /// The `<scriptobj num>` this region belongs to.
  std::int32_t num = 0;
  std::int32_t type = kAreaRectangle;

  /// `ptx`, `pty`, `r` -- present exactly when `type == kAreaCircle`.
  std::int32_t ptx = 0;
  std::int32_t pty = 0;
  std::int32_t radius = 0;

  /// `left`, `top`, `right`, `bottom` -- present exactly when
  /// `type == kAreaRectangle`.
  std::int32_t left = 0;
  std::int32_t top = 0;
  std::int32_t right = 0;
  std::int32_t bottom = 0;

  [[nodiscard]] bool is_circle() const noexcept { return type == kAreaCircle; }
};

/// One `<settlement>`, without its buildings: those are ordinary objects
/// carrying this settlement's index.
struct MapSettlement {
  std::string name;
  std::string class_of_first_building;
  std::int32_t id = 0;
  std::int32_t player = 0;

  /// The six economy attributes and `extrasentries`, **-1 when the element
  /// does not carry one**. Every one of the 748 shipped `<settlement>`
  /// elements declares all six (747 the seventh), so the default is for data
  /// this installation does not have -- and -1 is the value the settlement
  /// constructor in `gbr.exe` (0x005c3e60) takes to mean "the class's own
  /// number": `[cls+0xaa0]` for the population ceiling, `[cls+0xa9c]` for the
  /// population, which are the `max_population` and `population` properties.
  /// `GameSession::seed_economy` reads them the same way.
  std::int32_t population = -1;
  std::int32_t max_population = -1;
  std::int32_t gold = -1;
  std::int32_t food = -1;
  std::int32_t max_gold = -1;
  std::int32_t max_food = -1;
  std::int32_t extra_sentries = -1;

  /// Every attribute of the element, verbatim and in order; see
  /// `MapObject::attributes`. The one settlement in the install that carries
  /// `type`, `efficiency` and `maxunits` keeps them through a save this way.
  std::vector<std::pair<std::string, std::string>> attributes;
  /// The typed fields back into `attributes`; a -1 economy field is left out.
  void sync_attributes();
};

/// The two `<group type>` values. Nothing else occurs: 1,378 aliases and 803
/// armies across the 2,181 groups in the 29 shipped `map.obj.xml` documents.
inline constexpr std::int32_t kGroupAlias = 0;
inline constexpr std::int32_t kGroupArmy = 1;

/// A `<group>`: the format's only naming mechanism.
///
/// Flat, and only two attributes wide. Across all 29 shipped maps the element
/// carries `name` and `type` and nothing else (2,181 of 2,181 for each), its
/// only child element is `<obj num>` (23,408 of 23,408), it never nests, it is
/// never self-closing, and it is always a direct child of `<mapobject>` --
/// never inside a `<settlement>`, in 0 of 2,181. So a group is not a scope, not
/// a hierarchy and not per-player: it is a name over a set of object `num`s.
/// Per-player groups exist only as a *naming convention* the scripts apply --
/// `Group("Player" + .player + groupname)` in `DATA\SUBAI\ARENA_BEHAVIOR.VS`,
/// `Group("GoldMules" + idPlayer)` in `DATA\AI\ES_OUTPOSTSELLGOLD.VS` -- and
/// the engine sees flat tables.
///
/// **`type` is not a flavour: it selects between two different engine tables.**
/// `gbr.exe` keeps `CVXGroupQuery` and `CVXNamedObjQuery` as separate runtime
/// types, gives `NamedObj` its own script type with `obj`, `IsValid` and
/// `IsDead` members, and warns through `NAMEDOBJ_OVERRIDES_GROUP` that a name
/// "used for both a group and a named object" makes the *group* unreachable
/// from sequence scripts. The editor writes `<name>=Group("` for one kind and
/// `<name>=GetNamedObj("` for the other.
///
///   * `kGroupAlias` (1,378 elements) has exactly one member, in 1,378 of
///     1,378: a name for one object. `GetNamedObj(name)` reads it, and of the
///     67 literal `GetNamedObj("...")` sites in the shipped map scripts 66 name
///     one of their own map's elements -- **all 66 type 0, none type 1**.
///   * `kGroupArmy` (803 elements) is a set -- 170 of size one, mean 27, the
///     largest 922 (`T_BritainArmy`). `Group`, `SpawnGroup`, `AddToGroup` and
///     `RemoveFromGroup` read it, and across 417 literal sites in the map
///     scripts **all 417 are type 1**. 483 typed resolutions, zero crossover.
///
/// `sim::GroupTable` holds the second kind and `sim::NamedObjectTable` the
/// first; neither answers for the other.
///
/// Membership as authored is well formed: every one of the 23,408 `<obj num>`
/// references names a `<scriptobj>` the same document declares, no element
/// repeats a member, and none is empty. Member order is authoring order, not
/// ascending `num`.
///
/// **Names are not unique.** Four maps repeat one, and every repeat is a type-0
/// alias: `NO_Invisible` occurs 11 times in `5_Great_Loses_German`, each naming
/// a different object, and `NO_OUT8`, `Village2` and `sandruins2` occur twice in
/// three other maps. No shipped map repeats a type-1 name. Which object a
/// repeated alias resolves to is not established; `sim::NamedObjectTable::bind`
/// takes the first in document order and says so.
///
/// **And the army table is only a starting state.** Scripts add objects to
/// groups and take them out again while the game runs, and name groups no map
/// file declares; `sim/world.hpp` carries the live tables. See
/// `docs/formats/map.md`.
struct MapGroup {
  std::string name;
  std::int32_t type = kGroupAlias;
  std::vector<std::int32_t> members;  ///< object `num`s, in authoring order
};

/// `map.obj.xml`: every object placed in the world, in document order.
///
/// `num` is contiguous from zero in document order in all 29 shipped maps,
/// counting objects inside settlements, and is what groups and scripts refer
/// to — so `find(num)` is a bounds check in practice, but it is written as a
/// search because nothing in the format guarantees it.
class MapObjectList {
 public:
  static Result<MapObjectList> parse(std::span<const std::byte> xml);
  static Result<MapObjectList> from_document(const XmlDocument& doc);

  [[nodiscard]] const std::vector<MapObject>& objects() const noexcept { return objects_; }
  [[nodiscard]] const std::vector<MapSettlement>& settlements() const noexcept {
    return settlements_;
  }
  [[nodiscard]] const std::vector<MapGroup>& groups() const noexcept { return groups_; }
  /// The trigger regions, in document order. A parallel table rather than a
  /// field on `MapObject`: 904 of the 27,070 shipped objects carry one, and
  /// eight more integers on every object to serve 3% of them is the wrong
  /// trade for a structure a renderer walks per frame.
  [[nodiscard]] const std::vector<MapArea>& areas() const noexcept { return areas_; }

  [[nodiscard]] const MapObject* find(std::int32_t num) const noexcept;
  /// The region belonging to object `num`, or null when it has none.
  [[nodiscard]] const MapArea* find_area(std::int32_t num) const noexcept;

  // -- editing -----------------------------------------------------------
  //
  // What the editor needs and nothing more: the tables by reference, and
  // the next free `num`. `write_map_objects` (`map_writer.hpp`) turns the
  // result back into the document.

  [[nodiscard]] std::vector<MapObject>& objects_mut() noexcept { return objects_; }
  [[nodiscard]] std::vector<MapSettlement>& settlements_mut() noexcept { return settlements_; }
  [[nodiscard]] std::vector<MapGroup>& groups_mut() noexcept { return groups_; }
  [[nodiscard]] std::vector<MapArea>& areas_mut() noexcept { return areas_; }
  /// One past the largest `num` in the list; 0 for an empty one. The shipped
  /// maps number contiguously in document order, so this is also the count.
  [[nodiscard]] std::int32_t next_num() const noexcept;
  /// Remove the object numbered `num`, its area, and every group reference
  /// to it (an alias left with no member is removed with it). Returns
  /// whether there was one.
  bool erase(std::int32_t num);

 private:
  std::vector<MapObject> objects_;
  std::vector<MapSettlement> settlements_;
  std::vector<MapGroup> groups_;
  std::vector<MapArea> areas_;
};

// --------------------------------------------------------------------------
// DATA\TERRAINS.XML
// --------------------------------------------------------------------------

/// The two layers the engine hardcodes, which `DATA\TERRAINS.XML` says in a
/// comment of its own: `SHALLOW_WATER_IDX` and `DEEP_WATER_IDX`.
inline constexpr std::int32_t kShallowWaterLayer = 12;
inline constexpr std::int32_t kDeepWaterLayer = 13;

/// A layer's transition style when `DATA\TERRAINS.XML` does not give one:
/// C, the soft ramps. See `TerrainLayerDef::transition`.
inline constexpr std::int32_t kDefaultTransition = 2;

/// One `<layer>` of the terrain table. `z` is the value stored in
/// `Terrain.terrain.grid`, 0..40.
struct TerrainLayerDef {
  std::int32_t z = 0;
  std::int32_t type = 0;     ///< 0 invalid, 1 grass, 2 ground, 3 sand, 4 water, 5 rock, 6 road, 7 waves
  std::int32_t frames = 0;   ///< >1 on the two waters: 15 stacked frames in one image
  /// The transition style a layer is blended in with, `0..3` for the mask
  /// rows A..D. Absent, it is `kDefaultTransition`: `gbr.exe` builds every
  /// layer from one template whose value is 2 (0x0061f6d0) and the XML only
  /// overrides it -- the two waters say `transition="2"`, which is the default.
  std::int32_t transition = 2;
  bool passable = true;      ///< `passable="0"` on 6 and 26
  /// `passable_water="1"` on 13, deep water, and nowhere else: the layer
  /// ships pass by rather than walk over, and its rim is what the
  /// passability bake blocks (`edit::bake_terrain_passability`).
  bool passable_water = false;
  bool dark = false;
  std::string display;
  std::string image;    ///< with `%season%` unsubstituted
  std::string minimap;
};

class TerrainTable {
 public:
  static Result<TerrainTable> parse(std::span<const std::byte> xml);

  [[nodiscard]] const std::vector<TerrainLayerDef>& layers() const noexcept { return layers_; }
  /// The layer whose `z` is `z`, or null. Layers are stored in document order,
  /// which is ascending `z` in the shipped file, but the lookup does not assume it.
  [[nodiscard]] const TerrainLayerDef* layer(std::int32_t z) const noexcept;

  /// `terrain/%season%/grass1024.vq` with the season substituted, folded to
  /// the pack index's spelling (upper case, backslashes).
  [[nodiscard]] static std::string texture_path(std::string_view image,
                                                std::string_view season);

 private:
  std::vector<TerrainLayerDef> layers_;
};

// --------------------------------------------------------------------------
// terrain transitions
// --------------------------------------------------------------------------
//
// `Terrain.trans.grid` is zero in every cell of every shipped map, so the
// blend pattern has to be derived at load time from the terrain layer itself.

/// Corner bits of a transition mask's four-character name, `TL TR BR BL`.
/// A set bit means that corner is black in the mask, and black is where the
/// overlaid terrain is opaque — so the overlay's alpha is `255 - mask`.
inline constexpr std::uint8_t kCornerTopLeft = 0x8;
inline constexpr std::uint8_t kCornerTopRight = 0x4;
inline constexpr std::uint8_t kCornerBottomRight = 0x2;
inline constexpr std::uint8_t kCornerBottomLeft = 0x1;

/// One neighbouring terrain type to composite over a cell, and where.
struct TerrainOverlay {
  std::int32_t type = 0;
  std::uint8_t corners = 0;  ///< the four bits above
};

/// At most this many distinct neighbouring types can touch one cell: eight
/// neighbours, so eight types.
inline constexpr std::size_t kMaxTerrainOverlays = 8;

/// Neighbouring types of higher priority that touch cell `(cx, cy)`, with the
/// corner pattern each of them touches it at, ascending by type.
///
/// A corner is set when any of the three cells touching it carries that type.
/// Priority is the `<layer z>` itself.
///
/// This is the per-cell reconstruction the ground was first drawn with, and
/// it is **not** how the original composes it: `terrain_tile` below is, and
/// the zoom map's `zoom_terrain_tile` (`zoom_ground.hpp`). Nothing draws
/// from it any more.
///
/// Returns the number of overlays written, at most `out.size()`.
[[nodiscard]] std::size_t terrain_overlays(const Grid& terrain, std::int32_t cx,
                                           std::int32_t cy, std::span<TerrainOverlay> out);

/// `A1000`-style name of a mask: the style letter and the four corner bits,
/// TL first. Neither `0000` nor `1111` ships, so callers must handle those two
/// themselves — nothing to blend, and blend everything, respectively.
[[nodiscard]] std::string transition_mask_name(char style, std::uint8_t corners);

// --------------------------------------------------------------------------
// the ground tile, as gbr.exe composes it
// --------------------------------------------------------------------------
//
// Read off the original's software ground compositor (0x0061f8a0, one 64-unit
// cell of the 16-bit back buffer per call; the masks are loaded by 0x0061ed80).
// It is a **dual grid**: the terrain byte of cell (cx, cy) belongs to the
// *vertex* at world (64·cx, 64·cy), and the tile drawn over the square
// [64·cx, 64·cx + 64) x [64·cy, 64·cy + 64) blends the four vertices at its
// corners -- (cx, cy), (cx+1, cy), (cx+1, cy+1), (cx, cy+1), the far ones
// clamped at the map edge. That is the half-cell bias `IsPointInWater` and the
// passability bake already apply (`x + 32` before the divide): a point's
// terrain is the vertex it is nearest.

/// One layer drawn over a tile: its type and the corners it occupies, in the
/// `kCorner*` bits.
struct TerrainTileLayer {
  std::int32_t type = 0;
  std::uint8_t corners = 0;
};

/// What one ground tile is made of.
struct TerrainTile {
  /// The four corner vertices' types, TL TR BR BL.
  std::array<std::int32_t, 4> corners{};
  /// Drawn whole, with no mask.
  std::int32_t base = 0;
  /// Drawn over the base in this order, each through its mask. Four corners
  /// hold at most four types, one of which is the base.
  std::array<TerrainTileLayer, 3> overlays{};
  std::size_t overlay_count = 0;
};

/// The tile over cell `(cx, cy)`, by the original's rules (0x0061f8a0):
///
/// - **The base** is the lowest layer present at any corner -- except shallow
///   water (12), which is passed over unless every corner is water (12 or 13),
///   so a shore's land is drawn whole and the water over it.
/// - **The overlays** follow in three passes: every other layer present,
///   ascending, but neither water; then shallow water; then deep water. Each
///   occupies the corners whose vertex is that layer.
///
/// So priority is the layer number, as the reference render had assumed, with
/// the two waters lifted above every land layer.
[[nodiscard]] TerrainTile terrain_tile(const Grid& terrain, std::int32_t cx, std::int32_t cy);

/// The mask row an overlay of a layer with transition `transition` uses on a
/// tile of row `cy`: `'A' + transition + (cy & 1)`. The original indexes its
/// mask table by `transition + ((y / 64) & 1)` (0x0061ffb6), so every shipped
/// layer -- all at the default 2 -- alternates C and D, the soft ramps, row by
/// row; D is C mirrored, which is what breaks up the repetition. Rows past D
/// (the original generates two more, animated, from A/C and B/D at load,
/// 0x0061e760) have no file; this returns them as letters past 'D' and the
/// caller finds no mask.
[[nodiscard]] char transition_style(std::int32_t transition, std::int32_t cy);

/// The mask an overlay occupying `corners` is drawn through: the file whose
/// four bits are the corners it does **not** occupy (0x0061ffc1). A set bit is
/// black in a mask, so the mask is white where the overlay is -- it is the
/// overlay's opacity as it stands, with no inversion.
[[nodiscard]] inline std::uint8_t transition_mask_code(std::uint8_t corners) {
  return static_cast<std::uint8_t>(corners ^ 0xF);
}

/// The baked light's effect on one colour channel (0x0061e210, the lookup the
/// ground warp reads per pixel): `c + trunc(c * (level - 16) / 20)`, clamped to
/// the channel's range -- a gain of `(level + 4) / 20`, unity at the neutral
/// 16, 0.2 at level 0 and 1.25 at the layer's top of 21. The original builds
/// it for 32 levels over 5-bit channels; `max` is the channel's top, 31 there
/// and 255 for the 8-bit channels this engine composes in.
[[nodiscard]] inline std::int32_t terrain_light_channel(std::int32_t channel, std::int32_t level,
                                                        std::int32_t max = 255) {
  const std::int32_t lit = channel + channel * (level - 16) / 20;
  return lit < 0 ? 0 : (lit > max ? max : lit);
}

// --------------------------------------------------------------------------
// MAPOBJECTS\DECORS\DECORS.INI
// --------------------------------------------------------------------------

/// One decoration kind: the `type` number a decor cell stores, and the entity
/// to stamp for it.
struct DecorKind {
  std::int32_t type = 0;
  std::string section;  ///< the INI section name, for reports
  std::string entity;   ///< pack path, as written
  /// The editor's brush palette: `group` / `subgroup` are the two levels of
  /// its Decorations tree (`Trees` / `Pine trees`), `name` the leaf's label,
  /// `season` which set the kind belongs to (`spring`, `winter`, `autumn`).
  /// Keys are read case-insensitively -- the palms and desert bushes spell
  /// them `Group`/`Subgroup`. A kind whose group line is commented out
  /// (`;group = Other`, the swamps) has none and stands in no tree.
  std::string group;
  std::string subgroup;
  std::string name;
  std::string season;
};

/// The decoration palette. An INI, not XML — the one such file the renderer
/// reads — so it gets a small dedicated parser rather than a general one.
class DecorTable {
 public:
  static Result<DecorTable> parse(std::span<const std::byte> ini);

  [[nodiscard]] const std::vector<DecorKind>& kinds() const noexcept { return kinds_; }
  [[nodiscard]] const DecorKind* find(std::int32_t type) const noexcept;

 private:
  std::vector<DecorKind> kinds_;  ///< sorted by type
};

/// World units per step of a decor cell's sub-cell offset nibbles.
inline constexpr std::int32_t kDecorOffsetStep = 4;

/// One unpacked cell of `Terrain.decor.grid`.
struct DecorCell {
  std::int32_t kind = 0;
  std::int32_t offset_x = 0;
  std::int32_t offset_y = 0;
};

/// Unpack a 16-bit decor cell. False for an empty (zero) cell.
[[nodiscard]] constexpr bool decor_unpack(std::uint32_t value, DecorCell& out) noexcept {
  if (value == 0) return false;
  out.kind = static_cast<std::int32_t>(value & 0xFFu);
  out.offset_x = static_cast<std::int32_t>((value >> 8) & 0xFu) * kDecorOffsetStep;
  out.offset_y = static_cast<std::int32_t>((value >> 12) & 0xFu) * kDecorOffsetStep;
  return out.kind != 0;
}

// --------------------------------------------------------------------------
// the loaded map
// --------------------------------------------------------------------------

/// The six `Terrain.*.grid` payloads of one map, as the platform gathered them.
struct MapLayerBytes {
  std::span<const std::byte> map_xml;
  std::span<const std::byte> object_xml;
  std::span<const std::byte> pass;
  std::span<const std::byte> height;
  std::span<const std::byte> light;
  std::span<const std::byte> terrain;
  std::span<const std::byte> decor;
  std::span<const std::byte> trans;
};

/// One playable map: geometry, objects, and the six terrain layers.
///
/// The grid bytes are **copied in**, not viewed: a map outlives the container
/// buffer it was gathered from, and six layers of a 16384-unit map come to
/// about 900 KB, which is not worth a lifetime rule.
///
/// The layers are made over those copies with `Grid::parse_mutable`, so the
/// `*_mut()` accessors below paint straight into the owned bytes and the
/// const accessors -- the same `Grid` objects -- see every stroke. Saving is
/// then `write_grid(map.terrain())`, or the bytes vector itself.
///
/// Move-only, and the defaulted moves are sound: a `std::vector` move hands
/// its buffer to the destination (the standard guarantees references into the
/// source stay valid, and `std::allocator` propagates on move assignment), so
/// the `Grid` members copied alongside keep pointing at bytes this map now
/// owns. `world_map_moves_with_its_layers` in `test_world.cpp` pins that
/// rather than trusting the argument.
class WorldMap {
 public:
  static Result<WorldMap> load(const MapLayerBytes& bytes);

  WorldMap() = default;
  WorldMap(const WorldMap&) = delete;
  WorldMap& operator=(const WorldMap&) = delete;
  WorldMap(WorldMap&&) noexcept = default;
  WorldMap& operator=(WorldMap&&) noexcept = default;

  [[nodiscard]] const MapGeometry& geometry() const noexcept { return geometry_; }
  [[nodiscard]] const MapObjectList& objects() const noexcept { return objects_; }

  [[nodiscard]] const Grid& passability() const noexcept { return pass_; }
  [[nodiscard]] const Grid& height() const noexcept { return height_; }
  [[nodiscard]] const Grid& light() const noexcept { return light_; }
  [[nodiscard]] const Grid& terrain() const noexcept { return terrain_; }
  [[nodiscard]] const Grid& decor() const noexcept { return decor_; }
  [[nodiscard]] const Grid& transitions() const noexcept { return trans_; }

  // -- editing -----------------------------------------------------------
  //
  // The same six grids, writable. Each writes through to this map's own
  // bytes, so `terrain()` reads what `terrain_mut().set_cell(...)` wrote. A
  // layer the container did not carry is an empty `Grid` here as well:
  // `writable()` is false on it and `set_cell` refuses, rather than a layer
  // appearing from nowhere with a geometry nothing chose.
  //
  // The setter keeps the layer's own depth: the terrain layer of the five
  // blank templates in `Packs/` is 4 bits per cell where the authored maps
  // use 8, so `terrain_mut().max_cell_value()` is 15 on one and 255 on the
  // other, and a type that does not fit is refused, not clamped.

  /// The terrain layer at eight bits a cell, for editing. The five blank
  /// templates ship it at four, which holds `z` 0..15 and not the 41 layers
  /// a brush can paint; the original unpacks a nibble layer to bytes at load
  /// (0x0054b0c0) and always writes bytes (0x0054a260), so an edited
  /// template comes back at eight like every authored map. A layer already
  /// at eight, or absent, is left alone. False when the layer's own header
  /// refuses the copy.
  bool widen_terrain();

  [[nodiscard]] Grid& passability_mut() noexcept { return pass_; }
  [[nodiscard]] Grid& height_mut() noexcept { return height_; }
  [[nodiscard]] Grid& light_mut() noexcept { return light_; }
  [[nodiscard]] Grid& terrain_mut() noexcept { return terrain_; }
  [[nodiscard]] Grid& decor_mut() noexcept { return decor_; }
  [[nodiscard]] Grid& transitions_mut() noexcept { return trans_; }

  /// Terrain cells across the map. Zero if the terrain layer is missing.
  [[nodiscard]] std::uint32_t terrain_cells() const noexcept {
    return terrain_.cell_size() != 0 ? terrain_.width() : 0;
  }

  /// Baked brightness at a world position, 0..21 around a neutral of 16.
  [[nodiscard]] std::uint32_t light_at(std::int32_t world_x,
                                       std::int32_t world_y) const noexcept;
  /// Terrain elevation at a world position, in world units. 0 is sea level.
  [[nodiscard]] std::uint32_t height_at(std::int32_t world_x,
                                        std::int32_t world_y) const noexcept;

 private:
  Status adopt(std::vector<std::byte>& owned, std::span<const std::byte> source, Grid& grid);

  MapGeometry geometry_;
  MapObjectList objects_;

  std::vector<std::byte> pass_bytes_;
  std::vector<std::byte> height_bytes_;
  std::vector<std::byte> light_bytes_;
  std::vector<std::byte> terrain_bytes_;
  std::vector<std::byte> decor_bytes_;
  std::vector<std::byte> trans_bytes_;

  Grid pass_;
  Grid height_;
  Grid light_;
  Grid terrain_;
  Grid decor_;
  Grid trans_;
};

}  // namespace imperivm::core
