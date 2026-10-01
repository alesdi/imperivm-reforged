#pragma once

// The native class registry: the 26 C++ binding points every game class names.
//
// Inventory and evidence: docs/data-model.md ("The 26 native classes").
//
// All 845 shipped classes carry a `cpp_class` attribute — every one declares it
// rather than inheriting it — and every one names one of these 26 values. The
// assignment is not arbitrary: it follows the inheritance tree, so the 26 names
// are the original engine's own C++ hierarchy showing through the data. That
// hierarchy is reproduced here, so `CVXOutpost` really does derive from
// `CVXTownHall` from `CVXBuilding` from `CVXDecor`, and behaviour added at a
// base in Part 5 is inherited exactly where the data says it should be.
//
// Every parent link below is witnessed by at least one class whose `cpp_class`
// differs from its parent's, with one exception: `CVXWagon`. Five of its eight
// classes hang off a `CVXUnit` class and three off `Object` directly, so the
// link to `CVXUnit` is docs/data-model.md's inference and not a measurement.
//
// `corpus_classes` below is measured against the retail install, and differs
// from the census table in docs/data-model.md for six of the 26 — that table
// undercounts by 24 classes in total (`CVXBuilding` 125 vs 134, `CVXTownHall`
// 35 vs 41, `CVXBarrack` 19 vs 23, `CVXOutpost` 7 vs 9, `CVXDecor` 146 vs 148,
// `CVXTavern` 11 vs 12) and does not sum to 845. The numbers here are the ones
// `src/imperivm/formats/gamedata.py` produces over the shipped packs.
//
// **These are stubs.** Every native type below carries the common object state
// and no behaviour at all; the hooks are no-ops. What matters at this stage is
// that all 26 names resolve, that the shape is right, and that filling the
// behaviour in later does not require reshaping anything.
//
// Nine of the 26 could not be characterised from the shipped data — they add no
// property, method or behaviour that distinguishes them from their parent — and
// are marked `NativeClassConfidence::uninvestigated` rather than given an
// invented responsibility. A registry that quietly guesses is worse than one
// that admits the gap, because the guess gets built on.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"

namespace imperivm::core {

/// The 26 `cpp_class` values, ordered as the hierarchy in docs/data-model.md:
/// each class appears after its parent.
enum class NativeClass : std::uint8_t {
  decor = 0,      ///< CVXDecor, the universal base — 148
  building,       ///< CVXBuilding — 134
  town_hall,      ///< CVXTownHall — 41
  outpost,        ///< CVXOutpost — 9
  barrack,        ///< CVXBarrack — 23
  tavern,         ///< CVXTavern — 12
  gate,           ///< CVXGate — 65
  catapult,       ///< CVXCatapult — 8
  teleport,       ///< CVXTeleport — 3
  unit,           ///< CVXUnit — 134
  hero,           ///< CVXHero — 114
  druid,          ///< CVXDruid — 9
  wagon,          ///< CVXWagon — 8
  ship,           ///< CVXShip — 4
  flying_unit,    ///< CVXFlyingUnit — 3
  ghost,          ///< CVXGhost — 2
  feedback,       ///< CVXFeedback — 47
  script_obj,     ///< CVXScriptObj — 37
  area_effect,    ///< CVXAreaEffect — 1
  item_holder,    ///< CVXItemHolder — 23
  map_obj,        ///< CVXMapObj — 12
  catapult_shot,  ///< CVXCatapultShot — 3
  area,           ///< CVXArea — 1
  adv_area,       ///< CVXAdvArea — 1
  sacrifice,      ///< CVXSacrifice — 2
  dest_lock,      ///< CVXDestLock — 1
  count,
};

inline constexpr std::size_t kNativeClassCount = static_cast<std::size_t>(NativeClass::count);

/// How well the responsibility is understood from the shipped data alone.
enum class NativeClassConfidence : std::uint8_t {
  /// Distinguishing properties, methods, behaviours or default commands in the
  /// class graph say what the native class does. Not the same as *verified*
  /// against the original binary; it is what the data supports.
  characterised,
  /// The data says nothing: no distinguishing member anywhere in its subtree.
  /// Requires investigation before behaviour is written for it.
  uninvestigated,
};

struct NativeClassInfo {
  std::string_view name;   ///< the exact `cpp_class` spelling, e.g. "CVXDecor"
  NativeClass id = NativeClass::decor;
  /// The native class this one derives from. `CVXDecor` is the root and is its
  /// own parent, which keeps `is_a` walks terminating without a null case.
  NativeClass parent = NativeClass::decor;
  /// How many of the 845 shipped classes bind to it, measured over the retail
  /// packs. A cross-check, not an input to anything; see the file header for
  /// where it disagrees with docs/data-model.md.
  std::uint16_t corpus_classes = 0;
  NativeClassConfidence confidence = NativeClassConfidence::characterised;
  /// One line on what it appears to do, or what has to be found out.
  std::string_view summary;
};

/// Every native class, indexed by `NativeClass`. Stable order: it is iterated
/// during load, and iteration order is world state in this engine.
[[nodiscard]] std::span<const NativeClassInfo> native_classes() noexcept;
[[nodiscard]] const NativeClassInfo& native_class_info(NativeClass id) noexcept;

/// Resolve a `cpp_class` attribute. Matching is exact: the shipped data spells
/// these consistently, and a near miss is a finding rather than something to
/// paper over. Fails with `not_found`.
[[nodiscard]] Result<NativeClass> native_class_from_name(std::string_view cpp_class) noexcept;

/// Whether `derived` is `base` or descends from it.
[[nodiscard]] bool native_class_is_a(NativeClass derived, NativeClass base) noexcept;

// --------------------------------------------------------------------------
// objects
// --------------------------------------------------------------------------

using ObjectId = std::uint32_t;
inline constexpr ObjectId kNoObject = 0;

using PlayerId = std::uint8_t;
inline constexpr PlayerId kNoPlayer = 0xFF;

/// World position in integer world units. No floating point anywhere in the
/// simulation: see docs/engine/architecture.md.
struct WorldPos {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t z = 0;
};

/// Where an object is in its entity's state machine.
///
/// Data only. Advancing it is animation playback, which belongs with the tick
/// loop in Part 5; putting the logic here now would fix a shape before the
/// thing that drives it exists.
struct AnimCursor {
  std::int32_t state_idx = 1;    ///< an `<state idx>`; every entity declares 1
  std::int32_t anim_slot = kNoAnim;  ///< the slot being played, or kNoAnim
  std::int32_t elapsed_ms = 0;   ///< time into the current animation
  std::uint32_t step = 0;        ///< animation step, before AnimOrder sequencing
  std::uint32_t variation = 0;   ///< facing direction or random look: the sheet column
};

/// The common base: what every object in the world has, whatever it does.
///
/// The fields are the instance state that `CVXDecor` — the class all 845
/// inherit from — implies: identity, ownership, placement, health, and the art
/// binding. Everything the *class graph* declares (maxhealth, radius, speed,
/// sight, the script bindings) stays in the class and is read through
/// `class_index`; copying it per object would make 26 stubs into 26 property
/// bags that later have to be unpicked.
class NativeObject {
 public:
  NativeObject() = default;
  NativeObject(const NativeObject&) = default;
  NativeObject& operator=(const NativeObject&) = default;
  virtual ~NativeObject() = default;

  [[nodiscard]] virtual NativeClass native_class() const noexcept = 0;
  [[nodiscard]] std::string_view native_class_name() const noexcept {
    return native_class_info(native_class()).name;
  }
  [[nodiscard]] bool is_a(NativeClass base) const noexcept {
    return native_class_is_a(native_class(), base);
  }

  // -- behaviour hooks ---------------------------------------------------
  //
  // Deliberately empty. Part 5 fills them in; the signatures exist now so that
  // the call sites the loader and the tick loop need can be written against
  // something real.

  virtual void on_spawn() {}
  /// One simulation tick. Takes the tick number rather than a duration because
  /// the tick count is world state and wall time is not.
  virtual void on_tick(std::uint32_t /*tick*/) {}
  virtual void on_destroy() {}

  // -- common state ------------------------------------------------------

  ObjectId id = kNoObject;
  /// The class this object was spawned from. `ClassIndex` is the class graph's
  /// own handle: the graph owns the definition, this does not copy it.
  ClassIndex class_index = kNoClass;
  PlayerId owner = kNoPlayer;
  bool alive = true;

  WorldPos position;
  std::int32_t health = 0;
  std::int32_t max_health = 0;

  /// The loaded entity this object is drawn from, or null for an object whose
  /// class names no entity — six retail classes name one that was never
  /// shipped, and abstract nodes name none. Not owned: entities are shared by
  /// every instance of a class.
  const Entity* entity = nullptr;
  AnimCursor anim;
};

/// Construct the native type bound to `id`. Never null.
[[nodiscard]] std::unique_ptr<NativeObject> make_native_object(NativeClass id);
/// Construct from a `cpp_class` attribute. Null when the name does not resolve,
/// which for shipped data never happens.
[[nodiscard]] std::unique_ptr<NativeObject> make_native_object(std::string_view cpp_class);

namespace native {

/// CVXDecor — the universal base. Position, health, radius, art, an idle/wait
/// script pair and nothing else; 146 classes use it directly for inert scenery.
class Decor : public NativeObject {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::decor; }
};

/// CVXBuilding — a static, damageable, repairable structure with a capture
/// threshold and an exit interval.
class Building : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::building;
  }
};

/// CVXTownHall — the settlement owner: gold, food, population and loyalty for a
/// town live on this object, not on the settlement as a separate thing.
class TownHall : public Building {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::town_hall;
  }
};

/// CVXOutpost — a settlement that tributes resources to another settlement and
/// spawns pack mules.
class Outpost : public TownHall {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::outpost;
  }
};

/// CVXBarrack — a building with a production queue and a research tree.
class Barrack : public Building {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::barrack;
  }
};

/// CVXTavern — hire heroes, buy slaves, take and repay loans, invest, gossip.
class Tavern : public Building {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::tavern; }
};

/// CVXGate — an openable wall segment garrisoned by sentries.
class Gate : public Building {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::gate; }
};

/// CVXCatapult — a siege engine that is structurally a building and can be told
/// to fire at ground.
class Catapult : public Building {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::catapult;
  }
};

/// CVXTeleport — a pair of linked structures moving armies instantly. The
/// mechanic is entirely in C++: the classes declare no method for it.
class Teleport : public Building {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::teleport;
  }
};

/// CVXUnit — a mobile, ordered, fighting object: pathing, combat, inventory,
/// boarding, formations.
class Unit : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::unit; }
};

/// CVXHero — a unit that commands an army, carries skills and levels, and has a
/// UI command set of its own.
class Hero : public Unit {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::hero; }
};

/// CVXDruid — a spellcaster: targeted spells with a learning progression.
class Druid : public Unit {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::druid; }
};

/// CVXWagon — **requires investigation.** Eight capturable carriers, but they
/// add no property or method; the only evidence is that they replace `Unit`'s
/// command table with capture/unload/boardship.
class Wagon : public Unit {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::wagon; }
};

/// CVXShip — a vessel that carries units.
class Ship : public Unit {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::ship; }
};

/// CVXFlyingUnit — ignores terrain and hovers.
class FlyingUnit : public Unit {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::flying_unit;
  }
};

/// CVXGhost — **requires investigation.** Two incorporeal units; the only
/// distinguishing datum is a `unit_specials` property and a `damaged_by_ghost`
/// flag on the classes that fight them.
class Ghost : public Unit {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::ghost; }
};

/// CVXFeedback — **requires investigation.** 47 transient cues (`Heal`,
/// `Curse`, `Lightning`, `Damage1`) that add no property, method or behaviour
/// whatsoever. Attach-and-expire is the guess; nothing in the data confirms
/// what attaches them, to what, or when they die.
class Feedback : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::feedback;
  }
};

/// CVXScriptObj — an object whose whole existence is a script: projectiles,
/// spawn markers, decorative tents, `WatchEye`.
class ScriptObj : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::script_obj;
  }
};

/// CVXAreaEffect — `CoverOfMercy`, a persistent area-of-effect field.
class AreaEffect : public ScriptObj {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::area_effect;
  }
};

/// CVXItemHolder — a container that can be looted: graves, wells, boulders.
class ItemHolder : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::item_holder;
  }
};

/// CVXMapObj — **requires investigation.** Twelve classes: `catapult_placing`
/// and its faction variants plus three projectile shadows. The common thread is
/// "drawn but not simulated", which is an observation, not a responsibility.
class MapObj : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::map_obj; }
};

/// CVXCatapultShot — **requires investigation.** Three projectiles in flight.
/// A ballistic arc is the obvious reason they are not `CVXScriptObj`, but the
/// classes themselves declare nothing.
class CatapultShot : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::catapult_shot;
  }
};

/// CVXArea — **requires investigation.** One class, named only: a scripting
/// trigger volume is a plausible reading of the name and of nothing else.
class Area : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override { return NativeClass::area; }
};

/// CVXAdvArea — **requires investigation.** One class deriving from `CVXArea`,
/// named only. Presumably adventure mode, which is the campaign layer.
class AdvArea : public Area {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::adv_area;
  }
};

/// CVXSacrifice — **requires investigation.** Two classes with one method,
/// `sacrifice`. What it does to what is not in the data.
class Sacrifice : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::sacrifice;
  }
};

/// CVXDestLock — **requires investigation.** One editor/debug marker, the only
/// class whose entity lives in `UI.pak`, referencing a sheet that never
/// shipped. Almost certainly not needed to play the game.
class DestLock : public Decor {
 public:
  [[nodiscard]] NativeClass native_class() const noexcept override {
    return NativeClass::dest_lock;
  }
};

}  // namespace native

}  // namespace imperivm::core
