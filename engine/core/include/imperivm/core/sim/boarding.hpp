#pragma once

/// Boarding: the list of units a ship is waiting for.
///
/// Thirteen shipped scripts and twelve entry points, and they are one feature
/// rather than twelve errands. A ship that has been told to pick somebody up keeps an
/// **ordered list of the units it expects**; the units walk to it; each one
/// that gets close enough steps into the ship's holder and comes off the list.
/// `DATA\SUBAI\SHIP_BOARD.VS` is the ship's half of that loop and
/// `UNIT_BOARD.VS` / `UNIT_BOARD_COMMON.VS` are the unit's.
///
/// ## The list is the state, and the unit's own field is not it
///
/// This is the distinction the whole family turns on, and it would be missed.
/// A unit carries a **second**, separate field -- `[unit+0x18c]`, which
/// `Unit::GetShipToBoard` reads and `ObjectState::ship_to_board` is -- and it
/// is *not* a membership flag. `Ship::NotifyBoardUnit` writes it and **nothing
/// ever clears it**: not the cancel (`0x005c75c0` erases from the deque and
/// touches no unit), not a successful board, not the ship-wide cancel. So it
/// says *which ship last asked for this unit*, and it outlives the asking.
///
/// The two shipped `..._ONFINISH` scripts are written for exactly that:
///
///     if( .GetShipToBoard.IsValid ) .GetShipToBoard.NotifyBoardUnitCancel( this );
///
/// -- a cancelled boarding command finds its ship again through a field that is
/// still set precisely because nothing cleared it. Deriving the ship's list
/// from that field instead of storing it would therefore be wrong twice: a
/// cancelled unit would still look like a member, and a unit that two ships had
/// asked for could only name one of them. The original lets it be in both
/// deques at once, and `BoardingTable` below does too.
///
/// ## What each entry point comes to
///
/// | entry point | `gbr.exe` | |
/// |---|---|---|
/// | `Ship::NotifyBoardUnit` | 0x005c7fa0 | append unless already listed or already aboard; start the ship's `boardunit` command; stamp the unit's `ship_to_board` |
/// | `Ship::NotifyBoardUnitCancel` | 0x005c7d00 | erase from the list; the unit's field is left alone |
/// | `Ship::NotifyShipBoardingCancel` | 0x005c7d70 | kill each listed unit's boarding command, then empty the list |
/// | `Ship::AreUnitsToBoard` | 0x005c7920 | empty the list when the ship is full, prune the dead, answer "not empty" |
/// | `Ship::NumUnitsToBoard` | 0x005c6f60 | the size, with **no** prune |
/// | `Ship::BestCandidateToBoard` | 0x005c7a20 | prune the dead, then the nearest |
/// | `Ship::BoardUnit` | 0x005c7c00 | within 150, and not already aboard: into the holder, off the list |
/// | `Unit::GetShipToBoard` | 0x005d80d0 | the stamped field, whatever it now names |
/// | `Hero::CancelArmyBoard` | 0x0052fec0 | drop the followers that are on the wrong side of the gangplank |
/// | `Ship::UnboardAllUnits` | 0x005c85c0 | every passenger onto a random free land point nearby, until the points run out |
/// | `Ship::UnboardUnits` | 0x005c82f0 | the listed passengers, the same way |
/// | `Ship::FindPointToStay` | 0x005c7020 | a random free water point within 500, or where the ship is |
///
/// **Only two of the nine prune, and the third deliberately does not.**
/// `NumUnitsToBoard` is a bare read of the size member, so a ship whose whole
/// expected party has died still reports them until something asks
/// `AreUnitsToBoard` or `BestCandidateToBoard`. That is observable --
/// `UNIT_BOARD_COMMON.VS` ends its loop on `ship.NumUnitsToBoard == 0` -- so
/// the split is reproduced rather than tidied.
///
/// ## The full-ship clear compares for equality, and that is not the same as
/// "at capacity"
///
/// `AreUnitsToBoard` empties the list when the holder's occupancy **equals**
/// its capacity (`[holder+0x14] == [holder+0x10]`). Nothing in the original
/// stops a ship going past: `Ship::BoardUnit` does not check capacity at all,
/// and `UNIT_BOARD_COMMON.VS` tests `ship.UnitsCount == ship.UnitsMax` only
/// *after* it has already called `BoardUnit`. So a ship that is over capacity
/// never clears its list, and `==` is kept here rather than the `>=` that would
/// read as an improvement.
///
/// ## The re-entrancy latch is not reproduced, and can be shown not to matter
///
/// The original sets `[ship+0x1d8]` for the duration of
/// `NotifyShipBoardingCancel` and makes `NotifyBoardUnitCancel` a no-op while
/// it is set. That is a guard on a `std::deque` being walked while the
/// `..._ONFINISH` scripts its own `KillCommand`s launch call back into it. Its
/// two operations are the only reader and the only writer of the field, the
/// walk ends by emptying the list unconditionally, and this engine copies the
/// row before it kills anything -- so there is no arrangement in which the
/// latch changes the state anything can observe afterwards. It is left out, and
/// this paragraph is why.
///
/// ## One comparison in `BestCandidateToBoard` is not reproduced
///
/// After a candidate wins on distance, 0x005c7b93 compares `unit->holder == 0`
/// -- a zero-or-one boolean -- against the *ship's holder handle* as a 16-bit
/// number, and skips the candidate when they are equal. The two sides are
/// values of different kinds, and the test can only fire for a ship whose
/// holder happens to be object handle 0 or 1. This engine does not share the
/// original's handle numbering -- `ObjectId` here is a monotonic id, not an
/// index into a 65,536-entry table -- so there is no image of that test to
/// write. Nothing else in the routine filters, so every live listed unit is a
/// candidate.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;
struct WorldObject;

/// Which units each ship is waiting for, in the order it was told about them.
///
/// **World state.** `NumUnitsToBoard` is a number a running script loops on, so
/// two peers that disagree about it run two different loops. Hashed and
/// serialised with the world, beside `GroupTable` and `NamedObjectTable`, which
/// are the same kind of thing: a relation between objects that no object owns.
///
/// Rows are sorted by ship id and members are held in notify order, because
/// iteration order is state and `BestCandidateToBoard` breaks a distance tie by
/// taking the earlier member -- which is the order the original's `deque` walk
/// gives it.
class BoardingTable {
 public:
  /// Add `unit` to `ship`'s list unless it is already there. False when it was.
  bool notify(ObjectId ship, ObjectId unit);

  /// Take `unit` off `ship`'s list. False when it was not on it.
  bool cancel(ObjectId ship, ObjectId unit);

  /// Empty `ship`'s list, dropping the row.
  void clear(ObjectId ship);

  /// Drop the row `id` owns, for a ship that has left the world.
  ///
  /// **A dead *unit* is not removed**, and the asymmetry is the original's: a
  /// ship's deque dies with the ship, while a listed unit that dies stays in
  /// the deque until `AreUnitsToBoard` or `BestCandidateToBoard` walks it. Ids
  /// are never reused, so the stale member cannot alias a later object, and
  /// `NumUnitsToBoard` counting it is the behaviour a shipped loop reads.
  void forget(ObjectId id);

  /// `ship`'s list, in notify order. Empty for a ship with no row.
  [[nodiscard]] std::span<const ObjectId> expected(ObjectId ship) const noexcept;

  /// How many units `ship` is waiting for.
  [[nodiscard]] std::size_t count(ObjectId ship) const noexcept;

  /// Whether `unit` is on `ship`'s list.
  [[nodiscard]] bool listed(ObjectId ship, ObjectId unit) const noexcept;

  /// Replace `ship`'s list wholesale. Used by the prune and by a load; an empty
  /// `members` drops the row.
  void assign(ObjectId ship, std::span<const ObjectId> members);

  /// Rows, ascending by ship id. The count and the accessors below are what
  /// `World::serialize` walks.
  [[nodiscard]] std::size_t rows() const noexcept { return rows_.size(); }
  [[nodiscard]] ObjectId ship_at(std::size_t index) const noexcept;
  [[nodiscard]] std::span<const ObjectId> members_at(std::size_t index) const noexcept;

  void clear_all() noexcept { rows_.clear(); }

  /// Fold the whole table in row order. Folded into `World::state_hash` after
  /// the named objects and before the systems.
  void hash(std::uint64_t& accumulator) const noexcept;

 private:
  struct Row {
    ObjectId ship = kNoObject;
    std::vector<ObjectId> members;  ///< notify order, no duplicates
  };

  [[nodiscard]] std::size_t lower_bound(ObjectId ship) const noexcept;

  std::vector<Row> rows_;  ///< sorted by ship; iteration order is world state
};

/// Drop every listed unit that is gone or dead from `ship`'s row, and answer
/// how many are left.
///
/// The original's walk (0x005c79a0 and 0x005c7aa3, the same loop twice) drops a
/// member whose handle no longer resolves **or** whose `IsAlive` slot says no.
/// Both come to "there is no live object with that id" here.
std::size_t prune_boarding_list(World& world, ObjectId ship);

/// The units currently inside `ship`'s holder, which is the object at
/// `ship + 1`. See `sim/objlist.cpp`'s `GetUnitsOnBoard` for why that is where
/// a ship's holder lives.
[[nodiscard]] std::size_t units_on_board(const World& world, ObjectId ship);

// --------------------------------------------------------------------------
// the unboarding half
// --------------------------------------------------------------------------
//
// Three more entry points put the units back on land, and they are the other
// half of the same feature: `Ship::UnboardAllUnits` (0x005c85c0, the body at
// 0x005c84b0), `Ship::UnboardUnits(ol)` (0x005c82f0) and `Ship::FindPointToStay`
// (0x005c7020), which is what `SHIP_IDLE.VS` sails towards between jobs.
//
// ## Where a unit lands is drawn, not chosen
//
// Both unboard routines begin the same way: 0x005c8010 lays a **25 x 25
// lattice of 16-unit steps** over the square of half-side 192 around the
// ship's position -- the passability grid's own cells -- and keeps every point
// that is inside the map, on a passable cell, and not on deep water (terrain
// index 13, sampled with the half-cell offset `terrain_at` reproduces). The
// points are collected row by row, x fastest. Then, for each unit to drop,
// 0x005c81d0 draws one **at random from the world generator**, `between(0, n-1)`,
// and erases it from the vector *preserving the order of the rest*, so the
// same list of survivors is what the next draw indexes into. Every draw is
// therefore synchronised state, and both the lattice order and the ordered
// erase are reproduced rather than tidied: a swap-with-last would be one line
// shorter and would desync two peers on the second unit.
//
// A unit is dropped by taking it out of the ship's holder and placing it at
// the point (`World::remove_from_holder`). When the points run out the units
// left aboard stay aboard, and nothing says so: `UnboardAllUnits` simply ends.
// On a 25 x 25 lattice that takes a ship with more than 625 land points'
// worth of passengers or a very cramped beach, and both are possible.
//
// `UnboardAllUnits` walks the holder's contents in the holder's own order,
// which in the original is the order the units boarded (the holder keeps a
// deque). This engine keeps no per-holder list -- membership is the unit's
// back-link, and `World::contents_of` answers in ascending id -- so **the drop
// order is id order here and boarding order there**. That is the one
// approximation in this half, it is observable (which unit takes which
// point), and it is stated rather than hidden.
//
// `UnboardUnits(ol)` walks the script's list instead, in list order, dropping
// each member that is aboard *this* ship and skipping the rest; a list that
// names units aboard another ship or ashore leaves them where they are.
//
// ## Three things 0x005d3f20 does that are not reproduced
//
// The routine that actually moves the unit has three clauses around its
// kernel, and none of them can fire for a ship in the shipped data:
//
//   * a **rate limit** -- it refuses while `[owner+0x9c] + [owner+0xa0] > now`
//     and stamps `[owner+0x9c] = now` after a drop, where the owner is the
//     object whose holder the unit is leaving. The base object constructor
//     (0x00407264) zeroes both, nothing in the ship class writes the second,
//     and `exit_interval` -- the only interval a class declares, and only on
//     `Building` and `Catapult` -- lands in the class record at `+0x2f0`, not
//     here. So for a ship the refusal never fires. Had it fired, the point
//     drawn for the refused unit would still have been consumed;
//   * a **supply refill** from the settlement at `[holder+0x5c]`, which a ship's
//     holder does not have; and
//   * a **door re-route**: when the ship carries a link to an entrance object at
//     `[ship+0x8c]`, the drawn point is replaced by that object's nearest exit
//     point (0x004db6c0). This engine's ships carry no such link.
//
// Each is left out on the strength of that argument and no other; the
// condition that would lift any of them is a ship that sets the field.
//
// ## `FindPointToStay` is a hundred throws of two dice
//
// 0x005c7020 tries up to **100** candidates. Each is the ship's position plus
// an offset drawn as `dy = between(-500, 500)` **then** `dx = between(-500,
// 500)` -- y first, and the order is a synchronisation fact -- and is accepted
// when all three hold:
//
//   1. it is in the **same LSA** as the ship (`LsaPartition::at` on both, which
//      is the `[area+0x10]` word the original compares; a world with no
//      partition constrains nothing, as the original does with no area table);
//   2. its passability cell is free; and
//   3. it is clear of other **standing, living units** -- 0x0040a990's query
//      over a square of half-side `reach` around the point, where `reach` is
//      the ship's own class `radius` plus, for a `water_unit`, the `radius` of
//      the class named `ShipBattle`, and 40 otherwise. A neighbour blocks when
//      its distance to the point is under `reach` plus its own class `radius`.
//      The query's filter (0x004094d0) admits a unit that is alive and has no
//      path, and otherwise only an object with bit 18 of its `SyncFlags` word
//      set -- a runtime bit this engine does not carry -- so buildings and
//      walking units do not count here. The passability test is what keeps a
//      ship off a building.
//
// After a hundred misses it answers the ship's own position, having consumed
// two hundred draws. `SHIP_IDLE.VS` then `Goto`s there, which is a no-op.
//
// `unboarding_points`, `take_random_point` and `stand_point_free` are the three
// pieces, exported so the tests can pin each one down on its own.

/// 0x005c8010: the lattice, filtered. Fills `out` row by row, x fastest, and
/// returns how many points survived. A world with no movement system or an
/// empty passability grid yields none; a world with no terrain layer is all
/// land.
std::size_t unboarding_points(World& world, Point centre, std::vector<Point>& out);

/// The units inside `ship`'s holder, in the holder's own order; empty for
/// anything that is not a ship with a holder.
[[nodiscard]] std::vector<ObjectId> passengers_of(const World& world, ObjectId ship);

/// `Ship::UnboardAllUnits`'s body (0x005c84b0): every passenger onto a random
/// free land point nearby, in the holder's order, until the points run out.
/// Shared with `Ship::ApplyAiTransport`, which lands the ship's passengers
/// the same way before it orders their squads on. Returns how many landed.
std::size_t unboard_all(World& world, ObjectId ship);

/// 0x005c81d0: one point drawn uniformly from `points` with the world
/// generator and removed, the rest keeping their order. `kHeldPosition` for an
/// empty vector, without drawing -- the original would read past the end;
/// every caller checks first, and this guard is what makes the check the only
/// copy of the rule.
Point take_random_point(World& world, std::vector<Point>& points);

/// 0x0040a990: whether `self` could stand at `where` for the purposes of
/// `FindPointToStay`. See the header section for the reach and the filter.
[[nodiscard]] bool stand_point_free(World& world, const WorldObject& self, Point where);

/// The half-side of the drop-point square, and the lattice's edge in cells.
inline constexpr std::int32_t kUnboardingReach = 192;
inline constexpr std::int32_t kUnboardingSpan = 25;
/// `FindPointToStay`'s attempt budget and throw radius.
inline constexpr std::int32_t kStayAttempts = 100;
inline constexpr std::int32_t kStayThrow = 500;

/// Define the entry points this domain owns. Returns the number defined.
std::size_t register_boarding_host(script::HostRegistry& registry);

/// The number of `define` calls `register_boarding_host` makes.
[[nodiscard]] std::size_t boarding_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
