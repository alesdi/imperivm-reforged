#pragma once

// The formation object: `CVXFormObj`, one per hero at `[hero+0x2b0]`.
//
// Specification: docs/formats/formation.md. That page is the model; this
// header is the engine's copy of it, and says where the two still differ.
//
// A hero's army stands in formation round him. The object keeps who is in it
// (a **slot list** of members that have a place, and a **pending list** of
// units that joined since the shape was last laid), where each slot stands
// relative to the hero (its **offset**, laid by the layout from the shape in
// `DATA\FORMATIONS.XML`), and where each slot is on the map now (its
// **sample**, the offset turned into the formation's direction round its
// centre). The march (`place_army`, `sim/command.cpp`) sends each member to
// its slot's sample.
//
// **When things happen is the original's.** Joining and leaving only mark the
// layout; it reruns when a sample is placed or the hero's `FormKeepMoving`
// updates the march (`formation_rebuild`), so `FormRadius` answers the radius
// of the last layout, and a member that joined since has no slot yet. The
// members' places are chosen again only when the layout ran or the march
// turned back on itself (`formation_reassign`).
//
// **What is still approximated** (docs/formats/formation.md, "What this
// engine models"): the formation has no route of its own -- the hero's route
// stands for it, and a sample is placed whenever `place_army` runs -- and the
// members walk their own routes to their samples. The claim on the stations
// is not wired (`sim/movement.hpp`, inference 3).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;
class FormationTable;
struct FormationClassDef;

/// A slot record (0x18 bytes in the original).
struct FormationSlot {
  /// `[slot+0]`: the member, which points back at the record from
  /// `[unit+0x1c0]`.
  ObjectId unit = kNoObject;
  /// The member's class, which the layout groups by and the reassign matches
  /// on (the original keeps it in the same word while the slot is free).
  ClassIndex klass = kNoClass;
  /// `+0x08`: where the slot stands from the hero, in the formation's frame
  /// (forward is -y).
  Point offset{-1, -1};
  /// `+0x10`: where the slot is on the map, `(-1, -1)` until a sample places
  /// it.
  Point sample{-1, -1};
};

/// The formation's centre before any sample: `(-1, -1)`, as the constructor
/// (0x005f3a80) leaves it.
inline constexpr Point kNoSample{-1, -1};

/// `[hero+0x2b0]`.
struct Formation {
  ObjectId hero = kNoObject;
  /// `[form+0x18]`: the shape's name, as `SetFormation` gave it; empty for
  /// the file's `<Default>`, which the constructor takes.
  std::string shape;
  /// `[form+0xc]`: the members that have a place, in layout order.
  std::vector<FormationSlot> slots;
  /// `[form+0x74]`: units that joined since the last layout, in joining order.
  std::vector<ObjectId> pending;
  /// `rebuild_flag`, `[form+0x20]`: the layout is to be made again.
  bool rebuild = false;
  /// `reassign_flag`, `[form+0x1c]`: the members' slots are to be chosen
  /// again. Set by the constructor.
  bool reassign = true;
  /// `units_alive` and `units_died`, `[form+0x24]` and `[form+0x28]`.
  std::int32_t alive = 0;
  std::int32_t died = 0;
  /// `radius`, `[form+0x34]`: how far the farthest slot stood from the hero at
  /// the last layout; 0 before one, and after one with nobody to lay.
  std::int32_t radius = 0;
  /// `sample_dir`, `[form+0x38]`: the way the formation faces.
  Point dir{};
  /// `sample_center`, `[form+0x40]`: where it stands -- the hero's own
  /// station.
  Point centre = kNoSample;
  /// `[form+0x48]`: how many samples the current route has placed.
  std::int32_t samples = 0;
  /// Whether the formation holds a route object (`[form+0x84]` set): made by
  /// a `SetDest` that laid one, deleted by one that did not. `UpdateMove`
  /// does nothing without it. The route itself is the hero's here.
  bool path = false;
  /// `should_lock`, `[form+0x8c]`: `FormSetupAndMoveTo`'s fourth argument --
  /// whether the members own a destination lock once the march is over.
  bool should_lock = false;

  /// Whether `unit` has a slot or is pending.
  [[nodiscard]] bool holds(ObjectId unit) const noexcept;
  /// `unit`'s slot, or null.
  [[nodiscard]] const FormationSlot* slot_of(ObjectId unit) const noexcept;
};

// -- the roster ---------------------------------------------------------------

/// `AddUnit` (0x005f47e0): `unit` goes on the pending list and the layout is
/// marked. The caller has already taken it out of any formation it was in
/// (the unit's `vtbl+0x128`). A unit already held is left where it is.
void formation_add(Formation& form, ObjectId unit);

/// `RemoveUnit` (0x005f3290): `unit`'s slot is taken out (one fewer alive,
/// one more died) or, without one, it leaves the pending list; the layout is
/// marked either way. Returns whether `unit` was held. Deleting the unit's
/// route, the original's last act here, is the caller's.
bool formation_remove(Formation& form, ObjectId unit);

// -- the layout ---------------------------------------------------------------

/// The class data the layout reads for one class.
struct FormationClassTraits {
  /// `formation_radius` (`[class+0x2e4]`), or `selection_radius`
  /// (`[class+0x2e0]`) when that is negative or undeclared.
  std::int32_t spacing_radius = 0;
  /// `formation_priority` (`[class+0xa70]`); -15,000,000 when no class in the
  /// chain declares it.
  std::int32_t priority = 0;
};
/// What a class that declares no `formation_priority` has.
inline constexpr std::int32_t kUndeclaredPriority = -15000000;

[[nodiscard]] FormationClassTraits formation_class_traits(const ClassGraph& graph,
                                                          ClassIndex klass);

/// One record to lay: its class, and where the layout writes its offset.
struct FormationRecord {
  ClassIndex klass = kNoClass;
  Point offset{};
};

/// The layout's geometry (0x005f1d30, 0x005f0eb0): sort `records` into the
/// four groups by `shape`'s `<Class>` entries and lay each group's offsets
/// round a hero of class radius `hero_radius`. On return `records` is in slot
/// order -- central block, front line, left wing, right wing, each by
/// `formation_priority` -- with every offset written; a record no entry
/// matches is dropped. Answers the radius: the integer square root of the
/// largest squared offset, 0 for none. `shape_dist` and `shape_chaos` are 0
/// on every formation (docs/formats/formation.md), so no jitter is drawn.
std::int32_t formation_lay_out(const ClassGraph& graph, const FormationClassDef& shape,
                               std::int32_t hero_radius, std::vector<FormationRecord>& records);

/// `UpdateRebuild` (0x005f4390): nothing unless the layout is marked; then
/// the layout (0x005f1f60) -- the units of every slot and every pending unit
/// laid afresh in the hero's shape, each given the first slot of its own
/// class (0x005f4b30), the reassign marked -- and every unit that was pending
/// appended to `joined`, for the caller to give `form_move`. Answers whether
/// it ran.
bool formation_rebuild(World& world, Formation& form, const FormationTable& table,
                       std::vector<ObjectId>& joined);

// -- the sample ---------------------------------------------------------------

/// Where `offset` lands round `centre` facing `dir` (0x005f2900): forward is
/// local -y, each coordinate truncated toward zero. A zero `dir` leaves the
/// offset as it is.
[[nodiscard]] Point formation_turn(Point centre, Point dir, Point offset) noexcept;

/// `PlaceSample` (0x005f5a20): the centre and direction taken, the layout
/// rerun if marked (`formation_rebuild`), every slot's sample placed, then
/// `formation_reassign`.
void formation_place_sample(World& world, Formation& form, const FormationTable& table,
                            Point centre, Point dir, std::vector<ObjectId>& joined);

/// `UpdateReassign` (0x005f56d0): nothing unless the reassign is marked and
/// there is a slot; then, class by class, each member in slot order takes the
/// free slot whose sample sits nearest, relative to the samples' mean, to
/// where it stands relative to the members' mean. The flag is cleared.
void formation_reassign(World& world, Formation& form);

/// `FormRadius`'s answer (0x005f2350): the radius when the formation has a
/// slot and its radius is not 0, else `class_radius`.
[[nodiscard]] std::int32_t formation_reach(const Formation* form,
                                           std::int32_t class_radius) noexcept;

}  // namespace imperivm::core::sim
