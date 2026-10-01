#pragma once

// Where a class's authored points land in the world, and the doors a building
// has.
//
// ## Three mechanisms, not one
//
// `getpoint`, `getenterpoint`/`getexitpoint`, `getenterexit` and `getexitvector`
// all read "a point on a building" and **no two of them read it the same way**.
// Conflating them is the trap in this family, so they are set out here once:
//
// | name | source | conversion | selection | absent |
// |---|---|---|---|---|
// | `GetPoint(type, n)` | class table, any type | flat `y * 1448/1024` | the *n*th of that type | `(-32768, -32768)`, **relative** |
// | `GetEnterPoint` / `GetExitPoint` | class types 1 and 2, filed by ground | the full height-aware inverse | nearest / weighted random | the building's position, **absolute** |
// | `GetEnterExit` | class type 12 | the full height-aware inverse | **uniformly random** among them | `(-1, -1)`, **absolute** |
// | `GetExitVector` | class scalars, *not* the point table | none | -- | `(-1, -1)`, **relative direction** |
//
// `GetPoint` is `sim/world_host.cpp`'s and needs none of this: an offset is a
// *difference*, so the height term cancels and the inverse is a multiplication.
// This file is the other three's substrate -- the round trip that snaps a class
// point onto the ground it will actually be stood on.
//
// ## The round trip
//
// `0x005c11a0` is the whole of it, and it has only two call sites in the image:
// take the object's world position, project it to screen (`sim/projection.hpp`),
// add the slot's authored screen-pixel offset, reject the result if it falls
// outside a screen-space rectangle, and run the stepping inverse back to a world
// point -- rejecting that in turn if it falls outside the map. What comes out is
// a point on the terrain rather than a point in the air, which is why the
// enter/exit machinery uses it and `GetPoint` does not.
//
// **The screen rectangle is the map's, not the camera's**, and this matters
// enough to say twice: it is written by exactly one function, called from the
// singleton's constructor and from the map-load path, and it reads nothing but
// the map bounds. There is no dependence on where anybody is looking, and so no
// determinism hazard. `screen_bounds` below is its arithmetic.
//
// It is **not** redundant with the map check that follows it. It is tighter by
// one height cell in `x`, and at the southern edge by as much as the full height
// range -- a band of a few hundred world units on a shipped map. A class point
// that hangs off the south edge is exactly where the two disagree, which is why
// both are applied.
//
// **`(-1, -1)` is the sentinel and it is a real answer**, not an error: the
// original returns it when either rejection fires, and the one shipped script
// that reads a type-12 point tests the building handle rather than the point.
//
// ## The doors are recomputed, not cached, and that is a deliberate difference
//
// The original bakes four vectors onto each **building instance** -- enter and
// exit, each split land and deep water -- on first use, behind a "built" flag at
// `+0x1ec`, and keeps them for the building's life. This engine computes them on
// demand.
//
// The reason is determinism rather than tidiness. A baked list is a function of
// the terrain and passability layers *at the moment it was first asked for*, so
// two peers that first asked at different times hold different lists for the
// same building, and a save has to carry them or reproduce that timing. Deriving
// them from world state on every call has no such freedom: the same world always
// answers the same way, nothing new goes into the save, and there is no
// invalidation rule to get wrong when a wall goes up beside a barracks.
//
// It is still a difference, and it is observable in one direction: where the
// original would keep a stale door after the ground around it changed, this
// finds the door that is there now.
//
// ## The shipyard fixup
//
// A class that is or derives from `BaseShipyard` gets a second pass on any of
// the four lists that came out empty (0x004de400), and it searches from a pool
// that is **not** the enter/exit points: the baker keeps a fifth, temporary list
// of every class slot whose round trip landed on the map, *of every type and
// before the passability test*, and the fixup steps outward from those. So a
// shipyard's water door can come from a patrol station or a gate that was
// discarded as impassable.
//
// One ring of eight 16-unit neighbours, in the order west, east, north, south,
// south-west, north-west, north-east, south-east; all eight of a candidate are
// tried before the next candidate; the first that is of the right ground wins;
// exactly one point is appended and the search stops. The bounds test there is
// **strict** where the baker's is inclusive, and there is **no passability
// test** -- a shipyard really can be given a door standing on blocked ground.
// Deterministic; no RNG.
//
// ## What is still unknown
//
//   * **`GetExitVector`'s class scalars.** They are `exit_vector_x` and
//     `exit_vector_y` on the `.sc.xml` class record -- `docs/formats/sc-xml.md`
//     already names both -- rather than entries in the point table, and they are
//     inherited as a pair when either is absent. This engine's class loader does
//     not carry them yet, so nothing here reads them.

#include <cstdint>
#include <vector>

#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

/// The `<point>` type tags this file's readers ask for.
///
/// `docs/formats/ent-xml.md` left "the meaning of the other twelve point types"
/// open; these six are settled, each by the entry point that names its constant.
inline constexpr std::int32_t kEnterPointType = 1;
inline constexpr std::int32_t kExitPointType = 2;
inline constexpr std::int32_t kEnterExitPointType = 12;
inline constexpr std::int32_t kWaterSourcePointType = 13;
inline constexpr std::int32_t kGoodsSourcePointType = 14;
inline constexpr std::int32_t kFixSitePointType = 15;

/// The terrain index that is deep water, and the one thing that splits the
/// enter and exit lists in two. Shared with `sim/lsa.hpp`, which names it for
/// the same layer and the same reason.
inline constexpr std::uint32_t kDeepWaterIndex = 13;

/// The terrain index under a world point, sampled the way the original samples
/// it: **half a cell added first**, then floored to the 64-unit cell, so a
/// point is read from the cell whose centre is nearest rather than the cell it
/// falls in. Every reader in the executable that goes from a world point to
/// the terrain layer does this (`0x005c8010`'s `+0x20 >> 6` is one), and it is
/// exported so that the ship drop-point lattice and the shipyard door test read
/// the same cell for the same point. Zero when the layer is unparsed or the
/// point is off it.
[[nodiscard]] std::uint32_t terrain_at(const Grid& terrain, Point where) noexcept;

/// What the round trip answers when it lands nowhere.
inline constexpr Point kNoClassPoint{-1, -1};

/// The screen rectangle a class point must fall inside, in screen units.
///
/// `(map.min_x, project(map.min_y), map.max_x - 32, project(map.max_y) - 255 -
/// 32)`. The two margins are the terms that make the inverse safe rather than
/// arbitrary: 255 is the largest a terrain height can be and the projection
/// *subtracts* it, so a screen row at or below that bound is guaranteed to
/// unproject inside the map; 32 is one height cell, which is the inverse scan's
/// own step and the rounding both samplers apply.
struct ScreenBounds {
  std::int32_t min_x = 0;
  std::int32_t min_y = 0;
  std::int32_t max_x = 0;
  std::int32_t max_y = 0;
};
[[nodiscard]] ScreenBounds screen_bounds(const World& world) noexcept;

/// Where the class point at screen offset `(dx, dy)` on `object` lands.
///
/// `kNoClassPoint` when `object` names nothing, when the screen point falls
/// outside `screen_bounds`, or when the world point falls outside the map.
[[nodiscard]] Point class_point_in_world(const World& world, ObjectId object, std::int32_t dx,
                                         std::int32_t dy) noexcept;

/// **Every** point of `type` on `object`'s class that lands somewhere, appended
/// to `out` in class table order.
///
/// Plural on purpose. The four readers of a single typed point -- `GetEnterExit`
/// and its three siblings -- pick from this list **at random**, so a helper that
/// answered "the first" would be the wrong shape as well as the wrong point: 19
/// of the 91 gate-bearing classes in the installation declare more than one.
void class_points_of_type(const World& world, ObjectId object, std::int32_t type,
                          std::vector<Point>& out);

/// Whether `object`'s class declares any point of `type`. The cheap test the
/// original does with a bitmask summary on the class record, and the *only*
/// test the settlement-level readers apply -- it does not ask whether the point
/// would survive the round trip.
[[nodiscard]] bool class_has_point_type(const World& world, ObjectId object,
                                        std::int32_t type) noexcept;

/// The doors of `building` of one `type` (enter or exit) on one kind of ground,
/// appended to `out` in class table order.
///
/// A point is a door only if the round trip lands it on the map **and** the
/// passability layer says it can be stood on; `water` picks between the deep
/// water list and the land list. A shipyard whose list comes out empty gets the
/// fixup described above.
///
/// **A null `passability` skips the standable test rather than failing it.**
/// `ObstructionGrid` reads out of bounds as blocked, which is right for a
/// pathfinder and wrong here: a world with no movement system has no layer at
/// all, and treating "no information" as "a wall everywhere" would leave every
/// building in it doorless. Every shipped world has the layer.
void building_doors(const World& world, const ObstructionGrid* passability, ObjectId building,
                    std::int32_t type, bool water, std::vector<Point>& out);

/// One of `doors`, drawn at random with each door weighted by the **inverse
/// fourth power** of its distance from `from`.
///
/// `0x0056fa40`, and the sibling of the nearest-wins picker `GetEnterPoint`
/// uses. It is a list utility rather than a door utility -- it takes the vector
/// and a reference point and nothing else -- so it is named for what it does.
///
/// The original is two passes over the list in `double`. The first sums
/// `1 / (d2 * d2 + 1e-7)` over every element, where `d2` is the *squared*
/// distance as a 32-bit integer -- so the weight falls off as the fourth power
/// of the distance, not the second. It then draws `r` in `[0, 10000]`
/// inclusive, forms `threshold = r / 10000 * total`, and walks the list a second
/// time accumulating the same weights, taking **the first element whose running
/// sum is strictly greater than `threshold`**. If none is -- which the last
/// element's rounding can leave true -- it answers **the last element**.
///
/// **`1e-7` is what makes a door on top of `from` a special case rather than a
/// division by zero.** A zero distance weighs `1e7`, and every coordinate here
/// is an integer, so the nearest a *different* door can be is `d2 == 1` and a
/// weight of about `1`. A door standing exactly on the reference point is
/// therefore ten million times likelier than its closest possible rival, which
/// this reproduces as "it wins outright, and ties uniformly among its equals".
///
/// **The two passes do not compute the same expression**, and that is the
/// original's, not a transcription slip: the first squares `d2` and then adds
/// the epsilon, the second adds the epsilon and *then* squares. The two differ
/// by `2e-7 * d2` relatively, which is below `double`'s own resolution for the
/// sums it then forms and far below this engine's. It is recorded because a
/// reader comparing the two loops will see it and should know it was looked at.
///
/// ## Why this is integer arithmetic, and what that costs
///
/// Rule 1 of `docs/engine/architecture.md` forbids the `double`. The weights are
/// carried instead as a 20-bit fixed-point **ratio to the nearest door's**,
/// which is the one normalisation that needs no saturation: the nearest door is
/// `1 << 20` by construction and every other is `(d2_min << 20 / d2)` squared
/// back down, so the whole computation stays in `std::int64_t` for any distance
/// two points on a shipped map can be apart. The cost is a relative resolution
/// of about `1e-6` per weight where the original has `1e-16`, which can pick the
/// other of two doors whose distances differ in the seventh significant figure.
/// Nothing in the corpus distinguishes those. Weights are floored at one rather
/// than allowed to truncate to zero, which keeps the smallest draw answering the
/// *first* door the way the original's never-zero `double` weights do.
///
/// **One draw, always, and exactly one** -- including for a single-element list,
/// where the answer is not in doubt but whether the stream advanced is. Empty
/// in, `kNoClassPoint` out, and no draw: the original never reaches its picker
/// with an empty list and would read off the end of one if it did.
[[nodiscard]] Point pick_door_by_distance(const std::vector<Point>& doors, Point from, Rng& rng);

}  // namespace imperivm::core::sim
