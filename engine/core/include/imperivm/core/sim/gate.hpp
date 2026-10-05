#pragma once

/// A gate's portcullis: how far it is raised, and how it gets there.
///
/// ## A gate has no animation
///
/// None of the 56 gate entities in `Buildings.pak` declares an `<anim>`, and
/// every one of them has four still states that are its damage tiers. What
/// opens is one **layer**: the third one each entity declares, index 2 on all
/// 56 -- `bars` on 40 of them, `base` on the eight Gallic, `gate` on the eight
/// Iberian, and a single-frame sheet of its own on every one. `gbr.exe` slides
/// it up the screen.
///
/// `[gate+0x20c]` is the raised position, 0 (down) to 70 (up). The gate's
/// vtable slot `+0x5c` (0x00529340) hands `(0, -position)` to the visual's
/// per-layer offset setter (0x0062a4f0) for layer **2**, which writes it
/// beside that layer's own offset; so an open gate draws its portcullis 70
/// pixels higher and nothing else about it changes.
///
/// ## The motion
///
/// The gate's tick (0x00529090) moves the position towards the target
/// `OpenNow`/`CloseNow` wrote (`[gate+0x208]`), and the arithmetic is read off
/// it rather than chosen:
///
///   * at the start of a move it records the time (`[gate+0x218]`), the
///     position it starts from (`[gate+0x220]`), the distance to go
///     (`[gate+0x224]`: `70 - from` opening, `from` closing) and a duration of
///     `distance * 2000 / 70` ms (`[gate+0x21c]`), so the full travel takes two
///     seconds and a part of it takes its share;
///   * each tick after that the position is `from +- elapsed * distance /
///     duration`, clamped to 0..70;
///   * a move starts whenever the position is not already at the target and
///     the latch `[gate+0x228]` is clear, and `OpenNow`/`CloseNow` clear the
///     latch -- so a call restarts the move from wherever the portcullis is.
///     `GATE_IDLE.VS` calls one of them twice a second, which with this
///     arithmetic changes nothing but rounding.
///
/// **Inference:** the move here starts at the `OpenNow`/`CloseNow` call's own
/// time. The original starts it at the gate's next tick after the call, which
/// is the same instant or a little later; how much later depends on a tick
/// schedule (0x00688240) that is not read.
///
/// ## State, since it decides who walks through
///
/// In the original the position is also passability: the gate lets units
/// through once it stands above 20 (0x00529300, which also answers yes for a
/// destroyed gate, building mode 3), and the tick raises a latch
/// (`[gate+0x22c]`) whenever the position crosses 20 either way. So the
/// motion is **state**: it is hashed and saved beside the gate's flags
/// (`WorldObject::gate`, world section, for gates only), and a save taken in
/// the two seconds of a swing reloads mid-swing, as the original's does -- it
/// persists the same two numbers (`CVXGate`'s `vtmovestart` and `cmovestart`,
/// `docs/formats/save.md`).
///
/// ## The barrier
///
/// The map's obstruction layer leaves every gate's passage open: a gate's
/// `.pass` stamp is two wall stubs with a gap between them
/// (`docs/formats/pass.md`). What closes it is a line of cells the original
/// lays across the gap from the gate's own geometry:
///
///   * the axis is `gate_axis` (0x00529380): the entity's two type-7 markers,
///     moved across onto the mean of its type-10 markers and onto the gate;
///   * its ends go to 16-unit cells, each coordinate divided truncating
///     towards zero (0x0052b13d..0x0052b1a1), and a Bresenham line joins them
///     (0x00529e40), major axis x only when `|dx| > |dy|`, the minor
///     coordinate stepping when the running error is no longer positive;
///   * each cell of the line is blocked **with the one below it**, `y + 1`
///     (0x005298f0), so a diagonal line cannot be slipped through.
///
/// **The line is never in the shared grid.** `gbr.exe` writes it into the
/// map's one grid for the length of a single search and restores the cells
/// after (0x0052b0c0 keeps what was there, 0x0052a4c0 puts it back), so every
/// other reader -- the step, the sidestep, `IsPassable3x3`, the regions --
/// sees the gap open. Here that is `CellOverlay`, handed to the one search
/// that lays it (`PathRequest::barrier`), and the grid is never written.
///
/// ### Who a search lays it for: the unit's route (0x00419110)
///
/// A unit's route is laid by `RecastPathfind` (0x00419730) through 0x00419110,
/// and gates enter it in three moves:
///
///   1. **The search runs on the grid as it stands**, no gate laid
///      (0x00419177).
///   2. **The route's crossings are listed** (0x00418fb0): every gate on the
///      map (the map's gate list, 0x00542180), each with the distance along
///      the route at which the route first meets its axis -- 0x00417ed0 walks
///      the route's legs and 0x0040aab0 intersects each with the axis, ends
///      included -- in route order (0x00418680).
///   3. **If a crossed gate counts the mover an enemy** -- bit 0 of the
///      gate's own diplomacy row for the mover's player clear, the row
///      `Gate::LookAround` reads (0x00419331) -- the search runs again with
///      the line of **every gate on the map that counts the mover an enemy**
///      laid (0x004193c0..0x0041946b), except a gate standing fully open
///      (0x00529070: target open and the portcullis at 70) and a destroyed one
///      (building mode 3) within 1,500 of the mover (0x0041943a..0x00419444).
///      The second route replaces the first (0x00419602..0x004196d3), save in
///      one case: a first route no longer than 1,300 that crossed exactly one
///      enemy gate is kept when the second does not arrive (0x00419490,
///      0x0041954e) -- the unit walks up to that gate and waits there.
///
/// So **a gate never bars its own side's search, open or shut**, and bars an
/// enemy's search unless it stands fully raised. A friend's route goes
/// through a closed gate; the step below lets it walk through.
///
/// ### The step: a gate on the route ahead (0x00418260)
///
/// Every tick of a route, the first crossing not yet passed (0x00418210) is
/// tested once the mover is within its class `radius` plus 210 of it
/// (0x004182f6; a radius below zero counts as zero). The mover walks on:
///
///   * when it is the gate's **friend**, the gate has **no enemies near**
///     (`[gate+0x210]`, `AreEnemiesAround`) and its current action's name
///     does not begin with `c` (0x00418346) -- **waved through, closed or
///     not**;
///   * otherwise, when the gate lets units through -- the portcullis above 20
///     or the gate destroyed (0x00529300) -- or its target is open, so that
///     it is opening (0x00418360);
///
/// and otherwise it stands, the route kept, and asks again on the next tick
/// (0x00419cd2..0x00419d12 clear its moving bit). So an enemy whose route was
/// laid through a gate that was open then -- or one too short to go round --
/// stops before it closed, and a friend stops at its own closed gate while an
/// enemy is near it, which is when `GATE_IDLE.VS` closes it.
///
/// **Inferences and what is not reproduced.**
///
///   * *Which search.* The original has a second, used for a formation's
///     shared route (0x005f5b62 -> 0x005f41b0), with another rule: it lays a
///     gate within 1,600 of either end of the search for an enemy always, and
///     for anyone else while it neither lets units through nor is opening
///     (0x005f4333..0x005f434e). This engine has no shared formation route --
///     a march's members each lay their own -- so every route takes the unit's
///     rule, and that one is not reproduced.
///   * *The action name.* `[gate+0x10c]` is the object's current action
///     (0x005b4f20 writes it, `idle` by default); which action of a gate's
///     begins with `c` was not traced. A gate here runs only `GATE_IDLE.VS`,
///     so the test always passes.
///   * *A group's floor.* In a formation the reach is the group's radius with
///     a floor of 275 (0x00418286..0x004182f6, 0x005f2350); without a group
///     route, here it is always the mover's own radius.
///   * *Two sides of one gate.* A formation stopped at a gate compares which
///     side of it its leader and the member stand (0x00417dc0, 0x005295d0)
///     and halts a member on the far side. Not reproduced, for the same reason.
///   * *Parallel legs.* 0x0040aab0 has a branch of its own for a leg parallel
///     to the axis; it was not followed, and a parallel leg crosses nothing
///     here. The arithmetic is 64-bit where the original's products are 32.
///   * *Movers that ignore passability* (the sentries) get no crossings and
///     no second search: their route is a straight line along the walkway,
///     and whether the original holds one at its own gate was not read.
///   * *When.* The original moves the portcullis and tests it on the gate's
///     own tick, whose schedule (0x00688240) is not read. Here every test --
///     a search's and a step's -- reads the gate at the turn's time
///     (`World::time`), so a portcullis that passes 20 during a turn lets
///     units through from the next one.
///   * *Destroyed* is health at zero, as in `OpenNow`: this engine has no
///     building mode.
///
/// The lines are **derived** -- each gate's, from its position and entity,
/// learnt once (`GateLines`) -- and never saved or hashed. A route's
/// crossings are path media, saved with the route and not hashed, as the
/// route is; what they decide, where the unit stands, is hashed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/sim/tick.hpp"

namespace imperivm::core::sim {

struct WorldObject;
class World;

/// The fully raised position: `0x46` in the original's tests.
inline constexpr std::int32_t kGateRaised = 70;
/// How long a full travel takes, in game time: the `0x7d0` the duration is
/// scaled by.
inline constexpr std::int32_t kGateTravelMs = 2000;
/// The declared layer the position lifts: the `push 2` at 0x00529368.
inline constexpr std::uint32_t kGateLayer = 2;

/// The motion a gate's last `OpenNow` or `CloseNow` started. The target is
/// `ObjectFlags::gate_open`; these two are the rest of what says where the
/// portcullis is, and they are hashed and saved with it.
///
/// The defaults describe a gate that has stood at its target since time 0,
/// which is every gate on a fresh map.
struct GateMotion {
  /// When the move began: `[gate+0x218]`.
  GameTime start = 0;
  /// Where it began, 0..70: `[gate+0x220]`.
  std::int32_t from = 0;
};

/// The raised position, 0..70, of a gate moving towards `open` since
/// `motion.start` from `motion.from`, at time `now`.
[[nodiscard]] std::int32_t gate_position(const GateMotion& motion, bool open,
                                         GameTime now) noexcept;

/// Where `slot`'s portcullis is at `now`: `gate_position` over its motion and
/// target. 0 for anything that is not a gate.
[[nodiscard]] std::int32_t gate_raise(const WorldObject& slot, GameTime now) noexcept;

/// What `OpenNow`/`CloseNow` do: write the target, and start a move towards it
/// from wherever the portcullis is at `now`.
void gate_retarget(WorldObject& slot, bool open, GameTime now) noexcept;

/// The position above which a gate lets units through: `cmp [gate+0x20c],
/// 0x14; jg` at 0x00529309.
inline constexpr std::int32_t kGatePassableAbove = 20;
/// How far ahead of a crossing, beyond the mover's radius, the step looks at
/// the gate: `add eax, 0xd2` at 0x004182f6.
inline constexpr std::int32_t kGateApproach = 210;
/// A route at most this long that crossed one enemy gate is kept when going
/// round does not arrive: `cmp eax, 0x514` at 0x00419490.
inline constexpr std::int32_t kGateShortRoute = 1300;
/// A destroyed enemy gate this near the mover is not laid: `cmp eax, 0x5dc`
/// at 0x0041943f.
inline constexpr std::int32_t kGateRubbleReach = 1500;

/// 0x00529300: whether `slot` lets units through at `now` -- its portcullis
/// above 20, or the gate destroyed. **Inference:** the original's destroyed
/// test is its building mode 3, and this engine has no building mode; zero
/// health is the nearest thing, as in `OpenNow`. False for anything that is
/// not a gate.
[[nodiscard]] bool gate_lets_through(const WorldObject& slot, GameTime now) noexcept;

/// 0x00529070, `Gate::IsOpened`'s body: target open and the portcullis fully
/// raised at `now`. False for anything that is not a gate.
[[nodiscard]] bool gate_fully_open(const WorldObject& slot, GameTime now) noexcept;

/// Whether `gate` counts `owner` an enemy: bit 0 of the gate's own row for
/// that player clear (0x00419331, 0x00418328), `PlayerTable::is_enemy` with
/// the gate as the viewer.
[[nodiscard]] bool gate_bars(const World& world, const WorldObject& gate, PlayerId owner) noexcept;

/// 0x00418260 once the mover is within reach: whether a mover of `owner`
/// walks on past `gate` at `now`. See the header, "The step".
[[nodiscard]] bool gate_waves_through(const World& world, const WorldObject& gate, PlayerId owner,
                                      GameTime now) noexcept;

/// 0x00529380: the gate's wall axis, in world coordinates.
///
/// The two type-7 markers of the gate's entity are the axis -- the first one
/// and the last one, as the original overwrites its second end with every
/// type-7 marker after the first; the mean of its type-10 markers is projected
/// onto that axis and the axis is shifted by the perpendicular offset from the
/// projection to the mean, then translated to the gate. The markers are used
/// as the entity stores them, where `Building::GetPoint` scales `y` by
/// 1448/1024: the original does not scale them here. **With no type-10 marker
/// the original returns the two markers untranslated** -- an offset from the
/// map origin -- and that quirk is kept; every shipped gate carries type-10
/// markers, so it is unreachable on retail data. Fewer than two type-7
/// markers is no axis.
[[nodiscard]] bool gate_axis(const WorldObject& gate, Point& a, Point& b);

/// A collision cell: `ObstructionGrid`'s 16-unit coordinates.
struct GateCell {
  std::int32_t x = 0;
  std::int32_t y = 0;
  friend bool operator==(const GateCell&, const GateCell&) noexcept = default;
};

/// The cells a gate's line blocks across the axis `a`..`b`, in the order the
/// original visits them: the line's cells from `a`'s end, each followed by the
/// cell below it. See the header. Appends to `out`.
void gate_line_cells(Point a, Point b, std::vector<GateCell>& out);

/// 0x00417ed0 over 0x0040aab0: the distance along `route` at which it first
/// meets the segment `a`..`b`, ends included -- the legs' lengths before it,
/// each rounded down, and the rounded-down distance from that leg's start to
/// the meeting point -- or -1 when it never does.
[[nodiscard]] std::int64_t route_crossing(std::span<const Point> route, Point a, Point b) noexcept;

/// One gate a route crosses, and where along it: a `0x00418fb0` entry, kept
/// as the distance from the route's start rather than the original's
/// distance left, which is the same order read the other way.
struct GateCrossing {
  ObjectId gate = kNoObject;
  std::int64_t at = 0;
  friend bool operator==(const GateCrossing&, const GateCrossing&) noexcept = default;
};

/// Every gate's line, learnt once from its position and entity. Derived: see
/// the header. Held by the `World`; ascending gate id, which is the order
/// everything that walks it walks.
class GateLines {
 public:
  struct Line {
    ObjectId gate = kNoObject;
    /// Whether the entity gives the gate an axis at all.
    bool has_axis = false;
    Point a;
    Point b;
    /// The line's cells, each once.
    std::vector<GateCell> cells;
  };

  /// Learn every gate spawned since the last call and drop every one that is
  /// gone. Cheap when neither happened.
  void refresh(const World& world);
  /// Forget everything: the world's objects are about to be replaced.
  void forget() noexcept;

  [[nodiscard]] const std::vector<Line>& lines() const noexcept { return lines_; }
  /// `gate`'s line, or null.
  [[nodiscard]] const Line* find(ObjectId gate) const noexcept;

  /// 0x00418fb0: every gate whose axis `route` meets, in route order, ties in
  /// gate order. Clears `out` first.
  void crossings(std::span<const Point> route, std::vector<GateCrossing>& out) const;

  /// 0x004193c0..0x0041946b: into `out`, the line of every gate that counts
  /// `owner` an enemy, unless it stands fully open at `now` or is destroyed
  /// and within 1,500 of `from`. Not sealed: the caller seals it.
  void lay_enemy_gates(const World& world, PlayerId owner, Point from, GameTime now,
                       CellOverlay& out) const;

 private:
  std::vector<Line> lines_;
  /// Every id below this has been looked at for a gate.
  ObjectId scanned_ = 0;
};

/// `Gate::Inside`'s predicate (0x005295d0): whether `unit` stands inside the
/// walls of the settlement whose central building is `centre`.
///
/// The original routes the unit to the central building with bit `0x100` set
/// in the query's options. 0x00419110 reads that bit once, at 0x004192d5,
/// after the first search has run on the open grid and listed the gates its
/// route crosses (0x00418fb0): set, it returns that route and lays no gate as
/// a barrier. The answer is whether that list, `[query+0x30]` -- the list
/// 0x00418210 walks as gate-and-distance pairs -- is empty. So: **inside when
/// the route to the town centre, every gate open, crosses no gate.**
///
/// A unit in a holder is not inside (`[unit+0x154] != 0xffff`, the first
/// test): it is in a building, not in the streets. A route that cannot be laid
/// lists no crossing and answers inside, as the original's empty list does.
/// A mover that ignores passability routes its straight line, as all its
/// routes do (0x0040b580). Without a movement system nothing is inside.
[[nodiscard]] bool inside_walls(World& world, ObjectId unit, ObjectId centre);

}  // namespace imperivm::core::sim
