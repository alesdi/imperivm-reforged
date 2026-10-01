// Query specs and class filters. Evaluation lives in world.cpp, because it
// needs the object table. See include/imperivm/core/sim/query.hpp.

#include "imperivm/core/sim/query.hpp"

namespace imperivm::core::sim {

std::string_view query_type_name(QueryKind kind) noexcept {
  switch (kind) {
    case QueryKind::objs_in_sight: return "CVXObjsInSightQuery";
    // The dump prints the template argument. 200 blocks, all TCircleArea; the
    // rectangle instantiation below occurs in none of the nine, which says
    // nothing about whether it exists -- `gbr.exe` names it at 0x007bc834 --
    // and only that no dumped session had one live when it diverged.
    case QueryKind::map_area_circle: return "CVXMapAreaQuery<TCircleArea>";
    case QueryKind::map_area_rect: return "CVXMapAreaQuery<TRectArea>";
    case QueryKind::class_player_area: return "CVXClassPlayerAreaQuery";
    case QueryKind::player_flags: return "CVXPlayerFlagsQuery";
    case QueryKind::group: return "CVXGroupQuery";
    case QueryKind::party: return "CVXPartyQuery";
    case QueryKind::units_in_settlement: return "CVXUnitsInSettlementQuery";
    // Never instantiated in the corpus; the name is by analogy with the units
    // query and is unconfirmed.
    case QueryKind::buildings_in_settlement: return "CVXBuildingsInSettlementQuery";
    case QueryKind::set_op: return "CVXSetOpQuery";
    case QueryKind::count: break;
  }
  return "";
}

ClassFilter ClassFilter::of(ClassIndex index) {
  ClassFilter filter;
  if (index == kNoClass) return filter;
  filter.classes[0] = index;
  filter.count = 1;
  filter.match_all = false;
  return filter;
}

ClassFilter ClassFilter::parse(std::string_view names, const ClassGraph* graph) {
  ClassFilter filter;
  if (graph == nullptr || names.empty()) return filter;

  std::size_t start = 0;
  bool any_named = false;
  while (start <= names.size()) {
    std::size_t comma = names.find(',', start);
    if (comma == std::string_view::npos) comma = names.size();
    std::string_view token = names.substr(start, comma - start);
    start = comma + 1;

    // Trim: the corpus writes `"Military,Tower"` without spaces, but nothing
    // in the format forbids them and a stray space must not become a class
    // that does not exist.
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
    if (token.empty()) continue;
    any_named = true;

    // `lookup` rather than `find`: script names a class by `id` or by `altid`,
    // and the class graph resolves both with `id` winning.
    const ClassIndex index = graph->lookup(token);
    if (index == kNoClass) continue;
    if (filter.count >= kMaxClasses) continue;

    // Deduplicate, so that `"Unit,Unit"` and `"Unit"` are the same state and
    // therefore hash the same.
    bool already = false;
    for (std::uint8_t i = 0; i < filter.count; ++i) {
      if (filter.classes[i] == index) already = true;
    }
    if (already) continue;

    filter.classes[filter.count++] = index;
  }

  // A filter that named classes and resolved none of them selects nothing, and
  // must not silently widen to everything: that would turn a typo in the data
  // into a query over the whole world. `match_all` stays false with count 0,
  // which `World::matches_filter` reads as "matches nothing".
  filter.match_all = !any_named;
  return filter;
}

// --------------------------------------------------------------------------
// constructors
// --------------------------------------------------------------------------

QuerySpec objs_in_sight(ObjectId observer, ClassFilter filter, bool visible_only) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::objs_in_sight;
  spec.subject = observer;
  spec.filter = filter;
  spec.visible_only = visible_only;
  return spec;
}

QuerySpec objs_in_circle(Point center, std::int32_t radius, ClassFilter filter) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::map_area_circle;
  spec.center = center;
  spec.radius = radius;
  spec.filter = filter;
  return spec;
}

QuerySpec objs_in_rect(std::int32_t left, std::int32_t top, std::int32_t right,
                       std::int32_t bottom, ClassFilter filter) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::map_area_rect;
  // **Normalised here, and the original does not.** This is a labelled
  // divergence, not a transcription.
  //
  // `gbr.exe` never sorts the corners: the rectangle setter at 0x004d6d0e
  // copies the four authored integers verbatim into `area+0x164..0x170` and
  // marks the shape with `[area+0x148] = 0` at 0x004d6d28, with no min/max
  // anywhere. Its predicate (0x004f8c10) is `px >= left && px <= right &&
  // py >= top && py <= bottom`, which an inverted pair simply cannot satisfy,
  // and its grid sweep computes `(right >> 8) - (left >> 8) + 1` cells at
  // 0x004fdcbf, which is <= 0 for an inverted pair and exits the loop at once.
  // So a reversed rectangle selects **nothing** in the original, silently, from
  // every entry point.
  //
  // Sorting instead, because the divergence is unobservable on shipped data and
  // the failure mode is not: **0 of the 185 rectangles in the shipped maps are
  // reversed, and 0 are degenerate**, so no retail area reaches this at all.
  // What it buys is that a hand-authored or future rectangle answers correctly
  // rather than answering "nothing", which is a wrong answer wearing the shape
  // of a right one -- the failure this project keeps rediscovering.
  // `sim::AreaShape::of_rectangle` already made the same call for the same
  // reason, so the alternative is not "be faithful" but "be faithful in one of
  // the two places", which is worse than either.
  spec.left = left < right ? left : right;
  spec.right = left < right ? right : left;
  spec.top = top < bottom ? top : bottom;
  spec.bottom = top < bottom ? bottom : top;
  spec.filter = filter;
  return spec;
}

QuerySpec objs_in_range(ObjectId anchor, std::int32_t radius, ClassFilter filter) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::map_area_circle;
  spec.subject = anchor;
  spec.radius = radius;
  spec.filter = filter;
  return spec;
}

QuerySpec class_player_area(ClassFilter filter, PlayerId player, Point center,
                            std::int32_t radius) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::class_player_area;
  spec.filter = filter;
  spec.player = player;
  spec.center = center;
  spec.radius = radius;
  return spec;
}

QuerySpec class_player_area_rect(ClassFilter filter, PlayerId player, std::int32_t left,
                                 std::int32_t top, std::int32_t right,
                                 std::int32_t bottom) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::class_player_area;
  spec.filter = filter;
  spec.player = player;
  spec.area_is_rect = true;
  // Sorted for the same reason and with the same divergence as `objs_in_rect`;
  // the evidence is written out there.
  spec.left = left < right ? left : right;
  spec.right = left < right ? right : left;
  spec.top = top < bottom ? top : bottom;
  spec.bottom = top < bottom ? bottom : top;
  return spec;
}

QuerySpec player_flags(PlayerId player, ClassFilter filter, std::int32_t flags_type) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::player_flags;
  spec.player = player;
  spec.filter = filter;
  spec.flags_type = flags_type;
  return spec;
}

QuerySpec group_query(std::int32_t group) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::group;
  spec.group = group;
  return spec;
}

QuerySpec party_query() noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::party;
  return spec;
}

QuerySpec units_in_settlement(ObjectId settlement, ClassFilter filter,
                              SettlementScope scope) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::units_in_settlement;
  spec.subject = settlement;
  spec.filter = filter;
  spec.settlement_scope = scope;
  return spec;
}

QuerySpec buildings_in_settlement(ObjectId settlement, ClassFilter filter) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::buildings_in_settlement;
  spec.subject = settlement;
  spec.filter = filter;
  return spec;
}

QuerySpec set_op(SetOp op, ObjectId lhs, ObjectId rhs) noexcept {
  QuerySpec spec;
  spec.kind = QueryKind::set_op;
  spec.op = op;
  spec.lhs = lhs;
  spec.rhs = rhs;
  return spec;
}

}  // namespace imperivm::core::sim
