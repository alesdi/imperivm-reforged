#pragma once

/// Queries: persistent, handle-bearing objects that name a set of objects.
///
/// **A query is not a function call.** This is the single most surprising thing
/// the desync dumps say about the object model, and building it the obvious way
/// -- a stateless `find()` returning a vector -- desynchronises, because a query
/// occupies a handle in the same monotonic space as a unit and the slot table is
/// hashed. Allocation order is observable state.
///
/// `docs/engine/state-vector.md` proves the handle occupancy three ways:
///
///   1. `CVXMapAreaQuery<TCircleArea>`, `CVXClassPlayerAreaQuery`,
///      `CVXGroupQuery` and `CVXUnitsInSettlementQuery` print a `handle` that
///      slots into the global ascending sequence like any unit.
///   2. `CVXObjsInSightQuery`, `CVXSetOpQuery` and `CVXPlayerFlagsQuery` print
///      no handle -- but in the tick-2 dumps the count of handle-less blocks
///      between two printed handles is *exactly* the gap between them, in every
///      gap-group, with zero over-runs.
///   3. `CVXSetOpQuery` names two operands by handle, and 2,194 of 2,196 such
///      references land on a printed query handle or in a gap below the file's
///      maximum. A set-op query composes two other query *objects*.
///
/// So a script asks "what enemy units are in my sight" once, is given a handle,
/// and re-reads it; `WaitNonEmptyQuery(q, ms)` in the host API is only sensible
/// against something that re-evaluates. Evaluation itself is not cached here --
/// caching would need an invalidation rule the corpus does not supply -- but the
/// *object* persists, which is the part the hash can see.
///
/// ## Iteration order
///
/// Every evaluation yields object ids in ascending order, which is spawn order,
/// which is the order `World` stores objects in. Rule 3 of
/// docs/engine/architecture.md: iteration order is state.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// The eight query flavours. Seven are the runtime types the dumps name; the
/// eighth, `buildings_in_settlement`, is the counterpart of
/// `units_in_settlement` that the host API implies but the corpus of nine dumps
/// never happened to instantiate.
enum class QueryKind : std::uint8_t {
  /// `CVXObjsInSightQuery` -- 1,338 blocks, 9/9 dumps. Binds an observer to a
  /// class filter: crows and eagles watching for units, a lair watching for
  /// prey, a town hall or outpost watching its surroundings.
  objs_in_sight = 0,
  /// `CVXMapAreaQuery<TCircleArea>` -- 200 blocks, 9/9 dumps. A circle on the
  /// map. `ObjsInCircle(pt, r, class)` and `ObjsInRange(obj, class, r)` are both
  /// this; the second anchors its centre on an object rather than a point.
  map_area_circle,
  /// `CVXClassPlayerAreaQuery` -- 56 blocks, 9/9 dumps.
  class_player_area,
  /// `CVXPlayerFlagsQuery` -- 158 blocks, 6/9 dumps. `type` takes 1 and 2, with
  /// exactly one instance of 1 in the whole corpus, so its meaning is unknown
  /// and it is carried verbatim rather than interpreted.
  player_flags,
  /// `CVXGroupQuery` -- 31 blocks, 6/9 dumps. `Group(n)` in script.
  ///
  /// `gbr.exe` lists a **`CVXNamedObjQuery`** immediately beside this one, and
  /// it is not here: `<group type="0">` is a *named object*, read by
  /// `GetNamedObj` through `sim::NamedObjectTable`, and every shipped call site
  /// uses its result as a `NamedObj` (`GetNamedObj("NO_37").obj.player`) rather
  /// than as a query. Nothing needs the query form yet, and adding a kind the
  /// corpus never instantiates would put an unused branch in the state hash.
  group,
  /// `CVXUnitsInSettlementQuery` -- 24 blocks, 6/9 dumps.
  units_in_settlement,
  /// The buildings counterpart. Never instantiated in the corpus.
  buildings_in_settlement,
  /// `CVXSetOpQuery` -- 1,098 blocks, 6/9 dumps. Composes two query handles.
  set_op,
  /// `CVXMapAreaQuery<TRectArea>` -- the other instantiation of the template
  /// `map_area_circle` is one of, at 0x007bc834 in `gbr.exe` immediately after
  /// the circle form. **Zero blocks in the nine dumps**, which says only that
  /// no dumped session had a rectangle area query live at the moment it
  /// diverged: the shipped maps declare 243 rectangles against 661 circles, and
  /// 84 of 187 `AreaObjs` and 23 of 249 `ClassPlayerAreaObjs` literal sites name
  /// one.
  ///
  /// **Appended rather than filed beside `map_area_circle`, and that is not
  /// cosmetic.** The ordinal is what the save writes and what the world hash
  /// folds, so inserting a kind in the middle would silently change the meaning
  /// of every stored kind above it and move every hash of a world holding a
  /// query. The dump-frequency ordering of the kinds above stops here.
  map_area_rect,
  /// `CVXPartyQuery`, listed beside `CVXObjQuery` in `gbr.exe`'s type table:
  /// what `PartyQuery()` (0x005731e0) hands back, the session's party as a
  /// live query rather than the copied list `Party()` mints. No subject, no
  /// filter: the membership is the `in_party` flag. Appended, for the reason
  /// `map_area_rect` gives.
  party,
  count,
};

/// The runtime type name the original prints for a query kind, so a populated
/// world can be diffed against a dump block for block.
[[nodiscard]] std::string_view query_type_name(QueryKind kind) noexcept;

/// `CVXSetOpQuery.operation`.
///
/// **The mapping is inferred, not proven.** The dumps show three values with a
/// clean frequency ordering -- 0: 611, 1: 299, 2: 188 -- and the host API shows
/// three set-algebra free functions with the same ordering: `Intersect` (64
/// call sites), `Union` (36), `Subtract`/`Substract` (8 + 2). Lining the two up
/// is the only evidence there is. Treat a mismatch here as a live suspect if
/// conformance ever diverges on a set-op query.
enum class SetOp : std::uint8_t {
  intersect = 0,
  set_union = 1,
  subtract = 2,
};

/// Which half of a settlement `CVXUnitsInSettlementQuery` sweeps.
///
/// **One runtime type, three entry points, one field** -- and the field is part
/// of the interning key, not just of the answer. `UnitsInSettlement`,
/// `UnitsAroundSettlement` and `UnitsGuardingSettlement` are three names in
/// `gbr.exe`'s registration block at 0x00577934-0x005779c2, each registered
/// twice (a `(str, str)` form and a `(Settlement, str)` form), and all six
/// bodies end in the same call: `0x004fe080(mode, class, settlement)`. That
/// helper walks the interning pool at 0x00996a70 comparing three things --
/// the settlement handle at `+0x74`, the class string at `+0x58`, and **the
/// mode at `+0x54`** (0x004fe0fb) -- so two queries over the same settlement
/// and class with different modes are two different objects.
///
/// `CVXUnitsInSettlementQuery::Refresh` (0x004fb640) turns the mode back into
/// two independent booleans before calling the collector at 0x005c58f0:
///
///     al = (mode != 0)   ; sweep the ring
///     dl = (mode != 1)   ; sweep the garrison
///
/// which is why this is a scope rather than a kind: the collector takes two
/// flags, and the three shipped modes are three of its four combinations.
enum class SettlementScope : std::uint8_t {
  /// `UnitsInSettlement` -- the holder's own unit vector and nothing else.
  garrison = 0,
  /// `UnitsAroundSettlement` -- every object standing inside one of the
  /// settlement's buildings' own sight, and nothing from the garrison.
  ring = 1,
  /// `UnitsGuardingSettlement` -- both. No shipped script calls it; it is here
  /// because the mode is one field and leaving a hole in it would be inventing
  /// a fourth encoding for a value the executable already spells 2.
  both = 2,
};

/// A filter on the class *tree*, not on leaf classes.
///
/// `ObjsInSight(this, "Military,Tower")` ships, and the dumps' sight queries
/// mix concrete classes (`Crow`, `Hen`) with abstract ones (`Unit`, `Building`,
/// `Object`), so a match is "is this object's class a descendant of, or equal
/// to, any of these". Comma-separated lists are the format.
///
/// Resolved to `ClassIndex` at construction rather than kept as a string: the
/// filter is part of a query object's state and therefore part of the hash, and
/// hashing a string that the class graph could have resolved would make the
/// hash depend on spelling.
struct ClassFilter {
  /// The longest list in the shipped corpus is two entries; six is slack.
  static constexpr std::size_t kMaxClasses = 6;

  std::array<ClassIndex, kMaxClasses> classes{};
  std::uint8_t count = 0;
  /// True when the filter names nothing, or names only classes the graph does
  /// not know. An empty filter matches every object -- which is what the
  /// absence of a filter means at the call sites, and what a world with no
  /// class graph loaded (every synthetic test) has to fall back to.
  bool match_all = true;

  /// Parse `"Military,Tower"` against a class graph. A null graph, an empty
  /// string, or names that resolve to nothing all give `match_all`.
  [[nodiscard]] static ClassFilter parse(std::string_view names, const ClassGraph* graph);

  /// A filter naming exactly one already-resolved class.
  [[nodiscard]] static ClassFilter of(ClassIndex index);

  [[nodiscard]] bool empty() const noexcept { return count == 0; }

  friend bool operator==(const ClassFilter&, const ClassFilter&) noexcept = default;
};

/// Everything a query object holds.
///
/// One struct for all eight kinds rather than a variant hierarchy: the union of
/// the fields is small, the dumps print them as a flat per-type field list, and
/// a flat record hashes and serialises without a visitor. Fields a kind does not
/// use stay at their defaults and are hashed anyway, so a stale value from a
/// reused record cannot hide.
struct QuerySpec {
  QueryKind kind = QueryKind::map_area_circle;

  /// `CVXObjsInSightQuery.object handle`: who is looking. Also the anchor of an
  /// `ObjsInRange` circle, and the settlement of a `units_in_settlement`.
  ObjectId subject = kNoObject;

  /// The circle, in world units. For a sight query the radius is the subject's
  /// own sight and this stays zero.
  Point center;
  std::int32_t radius = 0;

  /// The rectangle, in world units, **corners inclusive**, for
  /// `map_area_rect`. Flat fields beside the circle's rather than a variant, for
  /// the reason the rest of this struct is flat: the union is four integers, it
  /// copies and compares without a visitor, and a kind that does not use them
  /// leaves them at zero, so a stale value from a reused record cannot hide.
  ///
  /// The same four numbers `sim::AreaShape` carries, and deliberately not that
  /// type: `sim/area.hpp` sits above this header and including it here would
  /// invert the dependency. `test_area.cpp` asserts the two agree on every
  /// point rather than letting a comment carry the guarantee.
  std::int32_t left = 0;
  std::int32_t top = 0;
  std::int32_t right = 0;
  std::int32_t bottom = 0;

  /// `class_player_area` only: whether the four corners above bound it rather
  /// than `center`/`radius`.
  ///
  /// **A discriminator here and two kinds above, and the asymmetry is the
  /// executable's.** `AreaObjs` builds one of *two runtime types* -- 0x00577268
  /// tests the area's `[area+0x148]` and materialises a copy of the shape into
  /// either a `CVXMapAreaQuery<TCircleArea>` or a `CVXMapAreaQuery<TRectArea>`,
  /// which have separate vtables (0x007bce48, 0x007bcdf8), separate class-id
  /// globals (0x0082111c, 0x00821120) and separate interning pools (0x00996aa4,
  /// 0x00996a94). `ClassPlayerAreaObjs` builds *one* type: 0x004fb6e0 stores the
  /// area object itself at `+0x78`, and `CVXClassPlayerAreaQuery::Refresh`
  /// (0x004fe860) re-reads `[area+0x148]` on every refresh and calls one of two
  /// non-virtual sweeps -- 0x004fd060 for a circle, 0x004fd2d0 for a rectangle
  /// -- or sweeps the whole map when the pointer is null. So one kind with a
  /// branch is what that type is, and a second `QueryKind` would be inventing a
  /// distinction the original does not draw.
  ///
  /// **What this does not reproduce** is that the original holds the area by
  /// *pointer* and this holds a copy of its numbers. That is invisible while an
  /// area's geometry cannot change, and `sim/area.hpp` says it never does --
  /// which this disassembly falsifies: `_AdvPlaceAreaCirc` (0x004d883b) and
  /// `_AdvPlaceAreaRect` (0x004d6d0e) both rewrite an existing area object's
  /// shape in place, and holding a pointer is exactly how the original's query
  /// follows them. Neither entry point is implemented here (`_PlaceEx/4`, 89
  /// sites, is declared and unimplemented), so nothing can move an area and the
  /// copy cannot go stale. Implementing area placement means revisiting this.
  bool area_is_rect = false;

  /// `units_in_settlement` only: which half of the settlement to sweep.
  ///
  /// `CVXUnitsInSettlementQuery`'s `+0x54`, and part of the original's
  /// interning key -- see `SettlementScope`. A second field on one kind rather
  /// than two more `QueryKind` members, for the reason `area_is_rect` is one:
  /// the executable draws no type distinction here, and a `QueryKind`'s ordinal
  /// is what the save writes and the world hash folds.
  SettlementScope settlement_scope = SettlementScope::garrison;

  /// `CVXPlayerFlagsQuery.player` and `CVXClassPlayerAreaQuery`'s owner.
  /// `kNoPlayer` means "any owner".
  PlayerId player = kNoPlayer;

  /// `CVXPlayerFlagsQuery.type`, domain {1, 2}. **Unknown**; carried, not read.
  std::int32_t flags_type = 0;

  /// `CVXGroupQuery`'s group id: an index into the world's `GroupTable`.
  ///
  /// That table starts as `map.obj.xml`'s `<group type="1">` elements -- the
  /// type-0 ones are named objects and live in a different table, which
  /// `CVXNamedObjQuery` rather than this kind reads -- and it does **not** stay
  /// that way: the scripts add objects to groups, take them out, and name groups
  /// no map file declares. See `sim/world.hpp`'s `GroupTable` for the evidence,
  /// and for why `Group("...")`'s string becomes an integer here.
  std::int32_t group = 0;

  ClassFilter filter;

  /// `VisibleObjsInSight` rather than `ObjsInSight`. Visibility is fog of war,
  /// which the shipped build kept out of the sync hash (`exploration` is zero
  /// in all nine dumps), so this flag is recorded and **not yet applied**:
  /// applying it would pull fog into hashed state, which is exactly what the
  /// zeroed channel forbids.
  bool visible_only = false;

  /// `CVXSetOpQuery`'s two operands, themselves query handles.
  ObjectId lhs = kNoObject;
  ObjectId rhs = kNoObject;
  SetOp op = SetOp::intersect;

  friend bool operator==(const QuerySpec&, const QuerySpec&) noexcept = default;
};

// -- constructors, named after the host functions that mint them ------------
//
// These build the spec only. Creating the *object* -- which is what takes a
// handle -- is `World::create_query`.

/// `ObjsInSight(observer, "class")` and `VisibleObjsInSight(observer, "class")`.
[[nodiscard]] QuerySpec objs_in_sight(ObjectId observer, ClassFilter filter,
                                      bool visible_only = false) noexcept;
/// `ObjsInCircle(point, radius, "class")`.
[[nodiscard]] QuerySpec objs_in_circle(Point center, std::int32_t radius,
                                       ClassFilter filter) noexcept;
/// `ObjsInRange(object, "class", radius)` -- a circle centred on an object, so
/// that it follows the object rather than the point it stood on.
[[nodiscard]] QuerySpec objs_in_range(ObjectId anchor, std::int32_t radius,
                                      ClassFilter filter) noexcept;
/// `AreaObjs("area", "class")` where the area is a rectangle.
///
/// **Corners inclusive on all four edges**, which is `gbr.exe` 0x004f8c10
/// character for character -- `jl` against left and top, `jg` against right and
/// bottom, signed, with the point in the left operand of every `cmp`. Three
/// independent sites agree (the predicate, and the two grid sweeps at
/// 0x004fddc6 and 0x004fd4d5). Unlike the circle, the obvious rule is the real
/// one here; see `sim/area.hpp` for the circle's two.
///
/// Reversed corners are sorted, which the original does not do. See the
/// implementation for the evidence and for why the divergence is taken.
[[nodiscard]] QuerySpec objs_in_rect(std::int32_t left, std::int32_t top, std::int32_t right,
                                     std::int32_t bottom, ClassFilter filter) noexcept;
/// `ClassPlayerObjs("class", player)`, optionally bounded by a circle.
/// `ClassPlayerAreaObjs("class", player, "area")` where the area is a
/// rectangle. Corners inclusive and normalised, as `objs_in_rect`.
[[nodiscard]] QuerySpec class_player_area_rect(ClassFilter filter, PlayerId player,
                                               std::int32_t left, std::int32_t top,
                                               std::int32_t right, std::int32_t bottom) noexcept;
[[nodiscard]] QuerySpec class_player_area(ClassFilter filter, PlayerId player, Point center,
                                          std::int32_t radius) noexcept;
/// `EnemyObjs(player, "class")`, `FriendlyObjs(...)`, `ControllableObjs(...)`.
///
/// `flags_type` is `CVXPlayerFlagsQuery`'s `type`, and it is **no longer
/// unexplained**: 1 is `FriendlyObjs`, 2 is `EnemyObjs`, 3 is
/// `ControllableObjs`, read off the three constructors in `gbr.exe`
/// (0x00575ab0, 0x005756b0, 0x005758b0). `World::objects_by_relation` acts on
/// it; which per-player mask each value consults is inference from the names
/// and is labelled there.
[[nodiscard]] QuerySpec player_flags(PlayerId player, ClassFilter filter,
                                     std::int32_t flags_type) noexcept;
/// `Group(n)`. `n` is a `GroupTable` index; `World::group_index(name)` is what
/// turns the string every call site actually passes into one.
[[nodiscard]] QuerySpec group_query(std::int32_t group) noexcept;
/// `PartyQuery()`.
[[nodiscard]] QuerySpec party_query() noexcept;
/// `UnitsInSettlement(settlement, "class")` and its two siblings.
///
/// One constructor for all three names, because `gbr.exe` builds all three
/// through one function with `mode` as an argument. See `SettlementScope`.
[[nodiscard]] QuerySpec units_in_settlement(
    ObjectId settlement, ClassFilter filter,
    SettlementScope scope = SettlementScope::garrison) noexcept;
/// The buildings counterpart.
[[nodiscard]] QuerySpec buildings_in_settlement(ObjectId settlement, ClassFilter filter) noexcept;
/// `Intersect(a, b)`, `Union(a, b)`, `Subtract(a, b)`. Both spellings of
/// subtract ship; they are the same operation.
[[nodiscard]] QuerySpec set_op(SetOp op, ObjectId lhs, ObjectId rhs) noexcept;

}  // namespace imperivm::core::sim
