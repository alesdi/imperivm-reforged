#pragma once

/// Map areas: the named circles and rectangles scripts mean by a *place*.
///
/// ## What an area is
///
/// A `<scriptobj class="AdvArea">` with a shape hung off it -- `type="1"` a
/// circle with `ptx`/`pty`/`r`, `type="0"` a rectangle with
/// `left`/`top`/`right`/`bottom`. 904 ship across the 29 `map.obj.xml`
/// documents, 661 circles and 243 rectangles, and `docs/formats/map.md`
/// specifies the element. It is one of the largest unimplemented clusters in
/// the corpus: `AreaCenter` alone is 279 call sites.
///
/// `gbr.exe` confirms the object shape from the other side. `_AdvPlaceAreaCirc`
/// is registered as `void(str name, point centre, int radius, str nextmap, str
/// targetarea)` and `_AdvPlaceAreaRect` as `void(str name, rect, str nextmap,
/// str targetarea)` -- the editor's own placement calls, carrying exactly the
/// four authored attributes plus the name. The runtime type list holds
/// `CVXAdvArea` beside `CVXArea`, and `CVXMapAreaQuery<TCircleArea>` beside
/// `CVXMapAreaQuery<TRectArea>`.
///
/// The field layout is read straight off `AreaCenter` (0x004d8bb0) and the
/// point sampler (0x004d6d40), which agree:
///
///     [obj+0x148]  non-zero for a circle, zero for a rectangle
///     [obj+0x14c]  ptx        [obj+0x164]  left
///     [obj+0x150]  pty        [obj+0x168]  top
///     [obj+0x154]  r          [obj+0x16c]  right
///                             [obj+0x170]  bottom
///
/// ## An area is named through the group table, like everything else
///
/// There is no area registry. `GetAreaByName` (0x004d9050) looks the name up in
/// the same name-to-handle map every other named object uses and prints
/// `GetAreaByName: Cannot find area named %s` when it misses. Every one of the
/// 904 shipped areas is named by a `<group type="0">` with exactly one member,
/// which is `sim::NamedObjectTable`.
///
/// So this file adds **no naming mechanism**. `area_named(world, "Ruins")`
/// resolves the string through `World::named_object`, and the object id it
/// yields is what keys `AreaTable`. A second name table would be a second
/// thing to keep in step with the first, and would resolve names `gbr.exe`
/// would not.
///
/// ## Load-time data, not world state
///
/// `AreaTable` is filled once from `map.obj.xml` and never written again. No
/// entry point mutates a shape: `EnableArea`/`DisableArea` exist in the
/// executable but have **zero call sites** in all 885 shipped scripts and are
/// not in the declared surface, and `nextmap`/`targetarea` are the empty string
/// on all 904 areas. So it is derived load-time data in the same class as
/// `WorldObject::sight` -- not hashed, not serialised, rebuilt by whoever
/// populates the world. `AreaSystem::hash` is deliberately empty and says so;
/// a save restores it by re-running `load_areas` against the same map, exactly
/// as `World::deserialize` re-resolves art through the entity resolver.
///
/// ## The geometry is integer, and there are **two** circle tests
///
/// `gbr.exe` does not have one notion of "inside a circle". It has two, they
/// differ by a one-unit shell at the rim, and which one applies depends on the
/// entry point rather than on the area:
///
///   * **The sampler and `AreaDistTo`** compute a *truncating integer* square
///     root (0x0067c3d0, table-driven, no floating point) and compare it with
///     `r`, at two independent sites -- 0x004d6e2e and 0x004d8b0e. Since
///     `floor(sqrt(d)) <= r` is exactly `d < (r+1)^2` over the non-negative
///     integers, `AreaShape::contains` is written as that squared comparison:
///     same answer, still no square root. **This is the rule `contains`
///     implements, and `sim/area.cpp`'s point sampler is its only caller.**
///   * **Every query** -- `AreaObjs`, `ObjsInCircle`, `ObjsInRange`,
///     `ClassPlayerAreaObjs` -- uses the plain `d2 <= r*r`. A query does not
///     store a radius at all: it stores a four-field POD `{cx, cy, r, r2}` and
///     compares against the *fourth* field. `CVXMapAreaQuery<TCircleArea>`'s
///     predicate is 0x004f8c60, and the comparison is `cmp edx, [esi+0x60];
///     jg` -- `[esi+0x54]` and `[esi+0x58]` being the centre, so `+0x60` is
///     `r2` and not `r`. Two independent producers write that field as `r*r`
///     with an `imul`: the circle setter at 0x004d883b (which also writes the
///     `[area+0x148] = 1` shape discriminator) and `ObjsInCircle` at
///     0x005776ef. **This is the rule `World::objects_in_radius` implements.**
///
/// So the two differ *on purpose*, and the difference is the one-unit shell at
/// the rim -- 1,300 world units of circumference on a 200-unit area, not a
/// rounding detail. An earlier revision of this header claimed the truncating
/// rule for the whole engine; it was read off the sampler and generalised, and
/// the query path was never disassembled. Do not "fix" the disagreement.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core {
struct MapArea;
class MapObjectList;
}  // namespace imperivm::core

namespace imperivm::core::sim {

class World;

/// Which of the two shapes an area is. The values are the file's own `type`.
enum class AreaKind : std::uint8_t {
  rectangle = 0,
  circle = 1,
};

/// An axis-aligned box in world units, corners inclusive.
struct AreaBounds {
  std::int32_t left = 0;
  std::int32_t top = 0;
  std::int32_t right = 0;
  std::int32_t bottom = 0;

  friend constexpr bool operator==(const AreaBounds&, const AreaBounds&) noexcept = default;
};

/// One area's region.
///
/// A flat record holding both shapes' fields rather than a variant: the union
/// is six integers, it copies and compares without a visitor, and the fields a
/// kind does not use stay at their defaults, so a stale value from a reused
/// record cannot hide. Same reasoning as `QuerySpec`.
struct AreaShape {
  AreaKind kind = AreaKind::circle;

  /// Circle: centre and radius.
  Point center;
  std::int32_t radius = 0;

  /// Rectangle: the corners, inclusive, `left <= right` and `top <= bottom`.
  std::int32_t left = 0;
  std::int32_t top = 0;
  std::int32_t right = 0;
  std::int32_t bottom = 0;

  [[nodiscard]] static AreaShape of_circle(Point center, std::int32_t radius) noexcept;
  [[nodiscard]] static AreaShape of_rectangle(std::int32_t left, std::int32_t top,
                                              std::int32_t right,
                                              std::int32_t bottom) noexcept;

  /// The centre, as `AreaCenter` computes it (0x004d8ce9): the circle's own
  /// `ptx`/`pty`, or the rectangle's midpoint with C division -- `(left +
  /// right) / 2` truncating toward zero, which is the `cdq; sub; sar 1` the
  /// executable emits.
  [[nodiscard]] Point centre() const noexcept;

  /// The bounding box, as the sampler's helper computes it (0x004d6800):
  /// `(cx - r, cy - r, cx + r, cy + r)` for a circle, the corners themselves
  /// for a rectangle.
  [[nodiscard]] AreaBounds bounds() const noexcept;

  /// Whether `p` is inside, **by the sampler's rule**.
  ///
  /// The circle test here is `d2 < (r+1)^2` and not `d2 <= r*r`; see the header
  /// for why those are two different rules in the original and which entry
  /// points take which. A *query* must not be answered with this: use
  /// `World::objects_in_radius`, which is the query rule. The rectangle case is
  /// the same in both, so `World::objects_in_rect` and this do agree, and
  /// `test_area.cpp` asserts it point for point.
  [[nodiscard]] bool contains(Point p) const noexcept;

  /// Whether `p` is inside, **by the query rule**: `d2 <= r*r`.
  ///
  /// The same geometry `World::objects_in_radius` applies, as a test on one
  /// point rather than a sweep over many -- because `WaitUnitsInArea` asks it
  /// of objects it already has. `gbr.exe` agrees: 0x004d7df0 is the predicate
  /// that entry point calls, it reads the shape discriminator at `[area+0x148]`
  /// and compares `dx*dx + dy*dy` with the fourth field of the `{cx, cy, r,
  /// r2}` POD (`jle` at 0x004d7e76), not with `(r+1)^2`. Rectangles are
  /// inclusive on all four bounds (0x004d7eef..0x004d7f1a), which is the case
  /// where the two rules agree.
  ///
  /// **Not the same function as `contains`.** They differ by the one-unit shell
  /// at the rim, and which one an entry point gets is part of the entry point's
  /// definition. See the header.
  [[nodiscard]] bool contains_by_query_rule(Point p) const noexcept;

  /// `AreaDistTo`'s metric: the distance from `p` to the region, 0 inside it.
  ///
  /// Two different metrics, and both are read off 0x004d8b04:
  ///
  ///   * circle -- `isqrt((cx-px)^2 + (cy-py)^2) - r`, a true radial distance;
  ///   * rectangle -- `max(dx, dy)` where `dx` is `left - px` or `px - right`
  ///     according to which side `p` falls, and `dy` likewise. That is a
  ///     **Chebyshev** distance, not a Euclidean one, and it is what the
  ///     executable computes: `cmp ecx, eax / jle / mov eax, ecx` keeps the
  ///     larger of the two.
  ///
  /// Both clamp at zero, at the same instruction.
  [[nodiscard]] std::int32_t distance_to(Point p) const noexcept;

  friend constexpr bool operator==(const AreaShape&, const AreaShape&) noexcept = default;
};

/// One area's region, translated from the map reader's transcription.
[[nodiscard]] AreaShape area_shape_from_map(const MapArea& authored) noexcept;

/// How many times the point sampler retries an impassable draw.
///
/// **Proven, not chosen.** `GetRandomPointInArea` pushes `0x14` as the third
/// argument to the sampler at 0x004d920d, and the shared
/// `Query::AttackArea` / `Query::MoveToArea` helper pushes the same `0x14`
/// once per member of the query, at 0x0057a48d.
inline constexpr std::int32_t kAreaSampleAttempts = 20;

/// The regions of every area in the world, keyed by the object that carries
/// one.
///
/// Sorted by id, and a vector rather than a map: iteration order is state
/// everywhere else in the simulation and there is no reason for this to be the
/// exception, a shipped map has at most 118 areas, and a binary search over a
/// contiguous array beats a node-per-entry container at that size.
class AreaTable {
 public:
  /// Record `shape` for `id`. False when `id` is `kNoObject` or already has a
  /// region -- two aliases naming one area object is a real case (the shape is
  /// the same either way) and is not an error.
  bool bind(ObjectId id, const AreaShape& shape);

  [[nodiscard]] const AreaShape* find(ObjectId id) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  void clear() noexcept { entries_.clear(); }

  /// One row, for a report or a test.
  struct Entry {
    ObjectId id = kNoObject;
    AreaShape shape;
  };
  [[nodiscard]] std::span<const Entry> entries() const noexcept { return entries_; }

 private:
  [[nodiscard]] std::size_t lower_bound(ObjectId id) const noexcept;

  std::vector<Entry> entries_;  ///< ascending by id
};

/// The system that carries the area table.
///
/// It has no per-turn behaviour at all and never will: an area is geometry,
/// and nothing about it changes as the game runs. It is a `System` for one
/// reason -- `World::systems()` is the seam a host function has to reach its
/// domain's state through (`sim/host_context.hpp` argues why a pointer on the
/// context is the wrong place), and `movement`, `command` and `hero` are all
/// found the same way.
class AreaSystem : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "areas"; }

  /// Nothing. See the class note.
  void advance(World& world, const Turn& turn) override;

  /// **Deliberately empty.** The table is load-time data derived from
  /// `map.obj.xml`, in the same class as `WorldObject::sight`: nothing writes
  /// it after load, so folding it into the hash would put a constant in the
  /// determinism contract and make every save that rebuilt it from the same
  /// map compare unequal for no reason. See the header.
  void hash(std::uint64_t& accumulator) const override { (void)accumulator; }

  [[nodiscard]] AreaTable& areas() noexcept { return areas_; }
  [[nodiscard]] const AreaTable& areas() const noexcept { return areas_; }

  // -- the saved game ----------------------------------------------------
  //
  // Layout and rationale: docs/formats/save.md. Definitions in
  // `src/sim/save_systems.cpp`, next to the other ten.

  /// Append the area table to `out`, with its own magic and version.
  ///
  /// **Written although nothing writes it after load, and although it is not
  /// hashed** -- for the same reason `World::serialize` writes
  /// `WorldObject::sight`. Recomputing it on load would make a save's meaning
  /// depend on `map.obj.xml` staying put, which is the silent drift a versioned
  /// save exists to prevent, and it makes the byte-level re-save check in
  /// `engine/tools/imsave.cpp` cover this system too. It costs 29 bytes an
  /// area, and the largest shipped map has fewer than a hundred.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace the table with the one in `bytes`. **Atomic** on failure.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  AreaTable areas_;
};

/// The world's area system, or null when none is registered.
[[nodiscard]] AreaSystem* area_system_of(World& world) noexcept;

/// What `load_areas` did.
struct AreaLoadReport {
  std::size_t authored = 0;    ///< `<scriptobj>` regions the map declares
  std::size_t bound = 0;       ///< regions attached to a spawned object
  std::size_t unnamed = 0;     ///< regions no `<group type="0">` names
  std::size_t unresolved = 0;  ///< named, but the object was never spawned
};

/// Attach every area the map declares to the object the world spawned for it.
///
/// **Goes through the alias table rather than reproducing the loader's
/// numbering.** `World::populate_from_map` keeps its `num` -> `ObjectId`
/// mapping to itself, and rebuilding it here would mean re-deciding which
/// classes resolve and which do not -- a copy of the loader that would rot the
/// first time either changed. Instead this walks the map's `<group type="0">`
/// elements, which is the same list the loader bound names from: for each
/// alias, `map.find_area(member)` gives the shape and `world.named_object(name)`
/// gives the id the loader chose. The two agree by construction, including on
/// the four maps that repeat an alias name, because both take the first
/// binding in document order and this skips a name it has already seen.
///
/// An area no alias names cannot be reached from a script -- there is no other
/// way to say its name -- so it is counted and skipped rather than guessed at.
AreaLoadReport load_areas(const MapObjectList& map, const World& world, AreaTable& out);

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------

/// The region named `name`, or null. Resolves through `NamedObjectTable`.
[[nodiscard]] const AreaShape* area_named(World& world, std::string_view name) noexcept;

/// A point drawn uniformly from `shape`, avoiding blocked ground.
///
/// This is 0x004d6d40, reproduced instruction for instruction because both
/// `GetRandomPointInArea` and the `AttackArea`/`MoveToArea` pair are built out
/// of it, and because **it draws from the world RNG**, which makes the exact
/// number of draws world state:
///
///   1. `attempts <= 0` returns the centre without drawing at all.
///   2. Otherwise draw `x` in `[left, right]` and `y` in `[top, bottom]` of the
///      bounding box, in that order, two draws per attempt.
///   3. For a circle, a point outside the circle is redrawn **without
///      consuming an attempt** -- the executable jumps back over the counter.
///   4. A point on blocked ground consumes an attempt and is redrawn.
///   5. When the attempts run out, the last point drawn is returned anyway.
///
/// Step 4 needs the obstruction bitmap, which `MovementSystem` owns; with no
/// movement system registered, or an empty grid, every point counts as
/// passable and the sampler stops at the first draw inside the shape. That is
/// a degradation and not a guess: the point is still inside the area.
[[nodiscard]] Point random_point_in_area(World& world, const AreaShape& shape,
                                         std::int32_t attempts = kAreaSampleAttempts);

/// Implement the area slice of the `.vs` host API.
///
/// Seven entry points, and what each one is built on:
///
///   `AreaCenter/1`            279 sites. `AreaShape::centre`. A name that
///                             resolves to nothing answers `(-1, -1)` --
///                             0x004d8cb7 writes `0xffffffff` into both
///                             components before returning.
///   `GetRandomPointInArea/1`   31 sites. `random_point_in_area`. An unknown
///                             name answers `(1000, 1000)`: 0x004d91dc loads
///                             `0x3e8` into both components. That is not a
///                             number anybody would invent, which is why it is
///                             worth reproducing.
///   `AreaDistTo/2`              1 site.  `AreaShape::distance_to`.
///   `AttackArea/2`            263 sites. **Issues `advance`, not `attack`** --
///                             0x0057a514 pushes the literal string `advance`
///                             and 0x0057a534 pushes `move`, and both tail into
///                             the same helper at 0x0057a2e0. Every member of
///                             the query is ordered to *its own* random point
///                             in the area, not to the centre: the helper calls
///                             the sampler once per object.
///   `MoveToArea/2`              8 sites. The same helper with `move`.
///   `AreaObjs/2`              193 sites. **Circle areas only** -- see below.
///   `ClassPlayerAreaObjs/3`   274 sites. Circle areas only.
///
/// ### The two query entry points are half-implemented, on purpose
///
/// `AreaObjs("area", "class")` builds a `CVXMapAreaQuery` and
/// `ClassPlayerAreaObjs("class", player, "area")` a `CVXClassPlayerAreaQuery`.
/// `sim/query.hpp` already has both -- `QueryKind::map_area_circle` and
/// `QueryKind::class_player_area` -- and both are bounded by a *circle*,
/// because `QuerySpec` carries a centre and a radius and nothing else. There
/// is no `QueryKind::map_area_rect`, and `gbr.exe` has one:
/// `CVXMapAreaQuery<TRectArea>` sits at 0x007bc834 immediately after
/// `CVXMapAreaQuery<TCircleArea>`.
///
/// Adding it means a new `QueryKind`, a rectangle on `QuerySpec`, a
/// `World::objects_in_rect`, a branch in `World::evaluate_query` and a row in
/// the query serialiser -- all in `sim/world.hpp`, `sim/world.cpp` and
/// `sim/save.cpp`. Until that exists, a rectangle area passed to either entry
/// point **traps by name** rather than answering with a bounding circle, which
/// would be a wrong answer that looked right. Of the literal call sites whose
/// area name resolves in their own map, 103 of 187 `AreaObjs` and 226 of 249
/// `ClassPlayerAreaObjs` name a circle; the rest trap.
///
/// ### What is not here, and why
///
/// `IsProtected/3` is **not an area entry point at all**, whatever its name
/// suggests. `gbr.exe` declares it `bool, int player, point pos, str class`
/// (registered at 0x0043c828) and both corpus call sites pass a class name --
/// `IsProtected(.player, .pos, "CoverOfMercy")`. It used to be refused here
/// because it "needs a notion of a protective effect covering a point, which
/// nothing in this simulation has"; it does now, and the notion turned out to
/// be nothing new: a live object of the named class, owned by somebody the
/// player is not at war with, within twice the class's `radius` of the point
/// by the query circle rule. It lives in `sim/world_host.cpp` beside `Place`,
/// which is what puts a cover of mercy on the map in the first place.
///
/// `ExploreArea/2` -- `void, int PlayerID, str area`, 24 sites -- **is no
/// longer refused, and what it was refused for is worth keeping**: "there is no
/// fog state in this simulation to reveal. Implementing it would mean inventing
/// that state, so it stays declared". There is now (`sim/fog.hpp`), and it was
/// not invented: the cell size, the two-bit-per-player encoding, the eight-slot
/// limit and the 1-based player argument are all read off `gbr.exe`. The entry
/// point lives in the `fog` domain rather than here, because it is a member of
/// that family first and an area consumer second -- the same arrangement
/// `WaitUnitsInArea` has with `wait`. It does borrow this header's geometry: it
/// takes the named shape's centre and the smallest circle that covers its
/// bounds, and hands both to the stamp `ExploreCircle` uses.
///
/// `WaitUnitsInArea/3` is **no longer refused**, and what it was refused for is
/// worth keeping: "no `Wait*` entry point is implemented anywhere in this
/// engine, because the blocking primitive does not exist yet". It did exist --
/// `script::HostStatus::retry`, carried since Part 4 with a comment naming a
/// `Wait*` entry point as the case it was for. The entry point lives in the
/// `wait` domain now (`sim/wait.cpp`) rather than here, because it is a member
/// of that family first and an area consumer second. It does borrow this
/// header's geometry: it tests containment by the **query** rule, through
/// `AreaShape::contains_by_query_rule`.
///
/// `AreaAIMaxPriority/3`, `AreaAINoRecruit/3` and `AreaAISetAttackOptimism/3`
/// (12 sites between them) have left this list for the third time on the same
/// shape of reason. They were refused because "nothing in `sim/ai.hpp` has
/// anywhere to put them, and a table nothing reads is worse than a declared
/// entry point". `AiPlayer::gaika` is that table now, so they live in the `ai`
/// domain -- which borrows this header's geometry the way `wait` does, by the
/// **query** rule, and walks the node table applying its write to every node
/// whose centre falls inside.
///
/// `EnableArea/1` and `DisableArea/1` are registered in the executable
/// (0x004d927c, 0x004d9292) and have **zero call sites** in all 885 shipped
/// scripts. They are not in the declared surface and nothing here adds them.
///
/// Returns the number of entry points defined, the convention every other
/// `register_*_host` follows.
std::size_t register_area_host(script::HostRegistry& registry);

/// How many entry points `register_area_host` defines.
[[nodiscard]] std::size_t area_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
