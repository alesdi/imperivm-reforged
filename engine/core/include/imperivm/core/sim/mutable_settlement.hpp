// A `Mutable` settlement made into a race's, from the shipped templates.
//
// A skirmish map does not know who will play it. Its strongholds and villages
// are placed as `MutableStronghold` and `MutableVillage` -- one placeholder
// building each, `race="Mutable"` -- and the players' races are decided in the
// lobby. Between the map loading and the match starting, `gbr.exe` walks those
// placeholders and replaces every one with a complete settlement of the
// owner's race, drawn from `Packs/RandomMapSettlements.bfhp`: a map of its own
// whose 64 `<settlement>` elements are the templates, two towns and five
// villages per race, plus shipyards. This is that step, 0x00552bf0 and the
// two routines under it, 0x00551bc0 (one settlement) and 0x0058e3c0 (one
// template stamped at a point).
//
// **What the original does, in order.** Two passes over the placeholders,
// first those owned by players 1..8, then the rest (0x00552d38, 0x00552ed1).
// For each: the race is the owner's, from the match setup (0x00551970);
// a placeholder whose owner has none takes, in the second pass, the race of
// the nearest settlement the first pass converted, and `Gaul` when there is
// none (0x00552f0f). A template is chosen among those whose name starts with
// the race's town or village prefix -- `"Gaul Town"`, `"Briton Village"` --
// by a draw over the matches (0x0058dfc0). The placeholder's buildings are
// destroyed and its record dropped; the template's objects are created around
// the placeholder's position, aligned so that the template keeps its own
// sub-cell offset on the 128-unit grid and clamped to the map; a fresh
// settlement record is created for the template's central building, and
// three things are carried over from the old record: its **name**, its
// **population and population ceiling**, and its **gold and food, each
// clamped to the new record's ceiling**. Loyalty starts over. The
// placeholder's object name -- the `<group type="0">` alias a mission script
// reads it by, `NO_MyTown` on six of the seven conquest maps -- is written
// onto the new central building (0x00551e6d), which is why `NO_MyTown.obj`
// keeps resolving after the building it named is gone.
//
// **What is read one step short, and labelled.** The template's anchor is
// the centre of the bounding box of its members' *footprints*
// (0x00592490: each class's entity rectangle at `entity+0x288`, offset by
// the member's position). This engine's core carries no footprint -- the
// passability masks never reach it -- so the box is over the members'
// *positions*. The difference is at most half a footprint's asymmetry, a
// few hundred units on the largest town, and it moves where the whole
// template lands, not how it is laid out. When a footprint reaches the core,
// `SettlementTemplate::centre` is where the correction goes. The draw uses
// the world's RNG, as `setup_match` does for `Random`; the original draws
// from the game object's generator at `+0x12a0`, which is assumed to be the
// synchronised one, since nothing else at match setup may differ between
// peers. And no footprint is stamped into the obstruction grid for the new
// buildings, for the same reason no building this engine spawns in play is
// -- the grid is the map's pre-baked one -- which is a standing gap of the
// movement domain and not of this file -- and it is closed one level up:
// once the templates have landed, `GameSession::start_match` rebuilds the
// **whole** passability layer the way the original does (0x00552e95; the
// rebuild is `core/world/editor.hpp`'s, proven against every shipped map),
// so the grid a match starts on carries the templates' footprints and the
// placeholders' are gone. See `SessionInputs::masks`. And the template's
// *ground* is laid too -- `stamp_template_ground` below, run by the session
// for each template after the members land: the pack's terrain copied, the
// height under it to sea level and sloped to meet the map, the decorations
// bulldozed, the shore taken out.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/world/editor.hpp"
#include "imperivm/core/world/map.hpp"

namespace imperivm::core::sim {

/// One template: a `<settlement>` of `RandomMapSettlements` with its members
/// re-based on the template's centre.
struct SettlementTemplate {
  std::string name;
  std::string class_of_first_building;
  /// The template's `<settlement>` numbers, -1 where the element has none.
  std::int32_t population = -1;
  std::int32_t max_population = -1;
  std::int32_t gold = -1;
  std::int32_t food = -1;
  std::int32_t max_gold = -1;
  std::int32_t max_food = -1;
  std::int32_t extra_sentries = -1;
  /// The bounding box's centre in the template map, and its extent. See the
  /// header on what the box is over.
  Point centre;
  std::int32_t width = 0;
  std::int32_t height = 0;
  /// The members, `x`/`y` **relative to `centre`**, everything else as
  /// authored. A template with no members is never chosen.
  std::vector<MapObject> members;
};

/// The 64 templates of `Packs/RandomMapSettlements.bfhp`'s `map.obj.xml`, or
/// any document of the same shape.
class SettlementTemplateLibrary {
 public:
  static Result<SettlementTemplateLibrary> parse(std::span<const std::byte> map_obj_xml);
  static SettlementTemplateLibrary from_map(const MapObjectList& map);

  [[nodiscard]] std::span<const SettlementTemplate> templates() const noexcept {
    return templates_;
  }
  [[nodiscard]] bool empty() const noexcept { return templates_.empty(); }

  /// Indices of the templates whose name starts with `prefix`, in document
  /// order, at most 64 of them -- 0x0058dfc0's own cap, which no shipped
  /// prefix approaches (two towns, five villages). Case-sensitive, as the
  /// original's byte compare is.
  [[nodiscard]] std::vector<std::size_t> matching(std::string_view prefix) const;

 private:
  std::vector<SettlementTemplate> templates_;
};

/// The prefix a race's templates are named by: `"Gaul Town"`,
/// `"Republican Roman Village"`, ... Empty for a race outside 0..7. The eight
/// strings are the two switch tables at 0x00590bd0 and 0x00590ec0, in `Race`
/// order.
[[nodiscard]] std::string_view settlement_template_prefix(std::int32_t race, bool village);

/// Where a template lands for a placeholder at `at`, on a map `map_size`
/// wide and high: the 128-unit alignment and the clamp of 0x0058e4b8. Exposed
/// for the tests; the materialiser calls it.
[[nodiscard]] Point settlement_template_anchor(const SettlementTemplate& tpl, Point at,
                                               std::int32_t map_size);

/// One template laid: which, and where its centre landed. What the ground
/// stamp needs, and what a save keeps so a load can lay the ground again.
struct TemplateStamp {
  std::size_t index = 0;
  Point origin;
};

struct MaterialiseReport {
  std::size_t strongholds = 0;  ///< placeholders made into towns
  std::size_t villages = 0;     ///< placeholders made into villages
  /// Placeholders left as they were: no template answered the prefix, or the
  /// template's central class is unknown to the class graph.
  std::size_t left = 0;
  /// Every template laid, in the order it landed.
  std::vector<TemplateStamp> stamps;
};

/// The layers a template's ground is laid into: the map's, writable, and
/// the marks the slope limiter keeps between templates -- one byte per
/// height cell, 1 where a template levelled the ground and it must stay,
/// 0xff where the limiter moved it, 0 elsewhere. Sized on first use.
struct GroundLayers {
  Grid* terrain = nullptr;
  Grid* height = nullptr;
  Grid* decor = nullptr;
  std::vector<std::uint8_t>* marks = nullptr;
};

/// What laying one template's ground did.
struct TemplateGround {
  /// The rectangle the ground was taken from, in template-map units; empty
  /// when the template paints nothing.
  edit::WorldRect box;
  std::size_t cells_copied = 0;
  std::size_t cells_levelled = 0;
  std::size_t decorations_bulldozed = 0;
  std::size_t shore_cells = 0;
  /// Height cells the slope limiter moved.
  std::size_t cells_sloped = 0;
  [[nodiscard]] bool any() const noexcept {
    return cells_copied + cells_levelled + decorations_bulldozed + shore_cells + cells_sloped > 0;
  }
};

/// A template's ground laid where its members landed -- the tail of
/// 0x0058e3c0, from 0x005904a3 -- into the map's own layers.
///
/// **What the original does.** Around the members' box grown by 256 it
/// bulldozes every decoration within `0.6 * diagonal` of the centre
/// (0x0058db30, "Trees bulldozed"). Over that box it copies the template
/// map's terrain type onto the map wherever the template paints one --
/// `RandomMapSettlements` leaves 15, `Invalid`, everywhere it does not --
/// and sets the height under each copied cell to sea level (0x00590540:
/// the four 32-unit cells, when the cell is 32 units inside the map). Over
/// the box grown by 512, wherever the template paints nothing, a cell that
/// is deep water no longer eligible to stay deep, or that is not deep and
/// within four cells of deep water (0x005468d0), becomes shallow water at
/// sea level (0x00590800) -- the shore is taken out from under the town.
/// Then the slope limiter (0x00548830 queues every height cell of the box
/// grown by 288; 0x00548070 walks the queue): a cell whose up, down, left
/// or right neighbour stands more than 20 higher or lower pulls that
/// neighbour to within 20 of itself, unless the neighbour is one a template
/// levelled and this cell is not; a moved cell is queued again, and so is
/// the cell that moved it. The ground a town on a plateau gets is
/// therefore sea level with a slope of 20 per 32 units up to the plateau
/// around it -- what the original makes of `Island War`'s islands, read
/// three times because it is hard to believe. The light and the
/// passability are re-baked over the box afterwards by the caller. The
/// template's own decorations are not copied.
///
/// **Reading, labelled.** The original's box is over the members' entity
/// image rectangles (`entity + 0x288`), which the core does not carry; the
/// box here is over the painted cells 4-connected to any painted cell
/// within 256 of a member, together with the members' own box grown by 256
/// -- on the shipped pack that is the template's own ground and never a
/// neighbour's, and it is what the copy would have reached anyway.
///
/// `origin` is where `tpl.centre` landed (`settlement_template_anchor`), so
/// map point `p` reads template point `p - origin + tpl.centre`. A layer
/// left null is left alone.
TemplateGround stamp_template_ground(const GroundLayers& map, const Grid& template_terrain,
                                     const SettlementTemplate& tpl, Point origin);

/// Replace every `MutableStronghold` and `MutableVillage` settlement in
/// `world` with a race's, as described in the header. Runs once, after
/// `setup_match` has resolved the players' races and before anything reads
/// the map's settlements. A world with no placeholders is left untouched; so
/// is one when `library` is empty.
///
/// The caller rebuilds the LSA partition and the GAIKA table afterwards --
/// both are functions of the settlement list, and the anchors moved.
MaterialiseReport materialise_mutable_settlements(World& world, EconomySystem& economy,
                                                  const MatchSystem& match,
                                                  const SettlementTemplateLibrary& library,
                                                  const EntityResolver* entities);

}  // namespace imperivm::core::sim
