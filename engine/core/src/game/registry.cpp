// The native class registry. See include/imperivm/core/game/registry.hpp.
//
// One table, in enum order, holding the name, the parent link, the corpus
// census and an honest confidence. The lookup is a linear scan of 26 entries:
// a hash map would be faster and would also be an unordered container in a
// simulation that treats iteration order as state, which is a trade nobody
// should take for 26 strings compared once per class at load time.

#include "imperivm/core/game/registry.hpp"

namespace imperivm::core {
namespace {

using C = NativeClass;
using Confidence = NativeClassConfidence;

constexpr NativeClassInfo kNativeClasses[] = {
    {"CVXDecor", C::decor, C::decor, 148, Confidence::characterised,
     "The universal base: position, health, radius, art, an idle/wait script pair."},
    {"CVXBuilding", C::building, C::decor, 134, Confidence::characterised,
     "Static, damageable, repairable structure with a capture threshold and an exit interval."},
    {"CVXTownHall", C::town_hall, C::building, 41, Confidence::characterised,
     "Settlement owner: holds a town's gold, food, population and loyalty."},
    {"CVXOutpost", C::outpost, C::town_hall, 9, Confidence::characterised,
     "A settlement that tributes resources to another settlement and spawns pack mules."},
    {"CVXBarrack", C::barrack, C::building, 23, Confidence::characterised,
     "A building with a production queue and a research tree."},
    {"CVXTavern", C::tavern, C::building, 12, Confidence::characterised,
     "Hire heroes, buy slaves, take and repay loans, invest, gossip, scout."},
    {"CVXGate", C::gate, C::building, 65, Confidence::characterised,
     "An openable wall segment garrisoned by sentries."},
    {"CVXCatapult", C::catapult, C::building, 8, Confidence::characterised,
     "A siege engine, structurally a building, that can be told to fire at ground."},
    {"CVXTeleport", C::teleport, C::building, 3, Confidence::characterised,
     "Linked structures moving armies instantly; the mechanic is entirely native."},
    {"CVXUnit", C::unit, C::decor, 134, Confidence::characterised,
     "Mobile, ordered, fighting object: pathing, combat, inventory, boarding, formations."},
    {"CVXHero", C::hero, C::unit, 114, Confidence::characterised,
     "A unit that commands an army, carries skills and levels, and owns the UI command set."},
    {"CVXDruid", C::druid, C::unit, 9, Confidence::characterised,
     "A spellcaster: targeted spells with a learning progression."},
    {"CVXWagon", C::wagon, C::unit, 8, Confidence::uninvestigated,
     "Capturable carrier. Adds no property or method; cargo-and-capture is unproven."},
    {"CVXShip", C::ship, C::unit, 4, Confidence::characterised,
     "A vessel that carries units."},
    {"CVXFlyingUnit", C::flying_unit, C::unit, 3, Confidence::characterised,
     "A unit that ignores terrain and hovers."},
    {"CVXGhost", C::ghost, C::unit, 2, Confidence::uninvestigated,
     "Incorporeal unit. Only evidence: a unit_specials property and damaged_by_ghost elsewhere."},
    {"CVXFeedback", C::feedback, C::decor, 47, Confidence::uninvestigated,
     "Transient visual/audio cue. 47 classes that add nothing at all; lifetime rules unknown."},
    {"CVXScriptObj", C::script_obj, C::decor, 37, Confidence::characterised,
     "An object whose whole existence is a script: projectiles, spawn markers, props."},
    {"CVXAreaEffect", C::area_effect, C::script_obj, 1, Confidence::characterised,
     "CoverOfMercy: a persistent area-of-effect field."},
    {"CVXItemHolder", C::item_holder, C::decor, 23, Confidence::characterised,
     "A lootable container: graves, wells, boulders, chests."},
    {"CVXMapObj", C::map_obj, C::decor, 12, Confidence::uninvestigated,
     "Placement ghosts and projectile shadows: drawn but not simulated. Purpose unknown."},
    {"CVXCatapultShot", C::catapult_shot, C::decor, 3, Confidence::uninvestigated,
     "The catapult projectile in flight. Ballistic handling is a guess; classes declare nothing."},
    {"CVXArea", C::area, C::decor, 1, Confidence::uninvestigated,
     "Named only. A scripting trigger volume is inferred from the name alone."},
    {"CVXAdvArea", C::adv_area, C::area, 1, Confidence::uninvestigated,
     "Named only; derives from CVXArea. Presumably an adventure-mode trigger."},
    {"CVXSacrifice", C::sacrifice, C::decor, 2, Confidence::uninvestigated,
     "One method, `sacrifice`. What it consumes and what it grants is not in the data."},
    {"CVXDestLock", C::dest_lock, C::decor, 1, Confidence::uninvestigated,
     "Editor/debug marker; the one entity in UI.pak, referencing art that never shipped."},
};

static_assert(sizeof(kNativeClasses) / sizeof(kNativeClasses[0]) == kNativeClassCount,
              "every NativeClass enumerator needs a table row, in enum order");

}  // namespace

std::span<const NativeClassInfo> native_classes() noexcept {
  return std::span<const NativeClassInfo>(kNativeClasses, kNativeClassCount);
}

const NativeClassInfo& native_class_info(NativeClass id) noexcept {
  const std::size_t index = static_cast<std::size_t>(id);
  return kNativeClasses[index < kNativeClassCount ? index : 0];
}

Result<NativeClass> native_class_from_name(std::string_view cpp_class) noexcept {
  for (const NativeClassInfo& info : kNativeClasses) {
    if (info.name == cpp_class) return info.id;
  }
  return FormatError::not_found;
}

bool native_class_is_a(NativeClass derived, NativeClass base) noexcept {
  if (static_cast<std::size_t>(derived) >= kNativeClassCount) return false;
  if (static_cast<std::size_t>(base) >= kNativeClassCount) return false;
  NativeClass current = derived;
  // CVXDecor is its own parent, so the walk ends there rather than on a null.
  for (std::size_t guard = 0; guard <= kNativeClassCount; ++guard) {
    if (current == base) return true;
    const NativeClass parent = native_class_info(current).parent;
    if (parent == current) return false;
    current = parent;
  }
  return false;
}

std::unique_ptr<NativeObject> make_native_object(NativeClass id) {
  switch (id) {
    case NativeClass::decor:
      return std::make_unique<native::Decor>();
    case NativeClass::building:
      return std::make_unique<native::Building>();
    case NativeClass::town_hall:
      return std::make_unique<native::TownHall>();
    case NativeClass::outpost:
      return std::make_unique<native::Outpost>();
    case NativeClass::barrack:
      return std::make_unique<native::Barrack>();
    case NativeClass::tavern:
      return std::make_unique<native::Tavern>();
    case NativeClass::gate:
      return std::make_unique<native::Gate>();
    case NativeClass::catapult:
      return std::make_unique<native::Catapult>();
    case NativeClass::teleport:
      return std::make_unique<native::Teleport>();
    case NativeClass::unit:
      return std::make_unique<native::Unit>();
    case NativeClass::hero:
      return std::make_unique<native::Hero>();
    case NativeClass::druid:
      return std::make_unique<native::Druid>();
    case NativeClass::wagon:
      return std::make_unique<native::Wagon>();
    case NativeClass::ship:
      return std::make_unique<native::Ship>();
    case NativeClass::flying_unit:
      return std::make_unique<native::FlyingUnit>();
    case NativeClass::ghost:
      return std::make_unique<native::Ghost>();
    case NativeClass::feedback:
      return std::make_unique<native::Feedback>();
    case NativeClass::script_obj:
      return std::make_unique<native::ScriptObj>();
    case NativeClass::area_effect:
      return std::make_unique<native::AreaEffect>();
    case NativeClass::item_holder:
      return std::make_unique<native::ItemHolder>();
    case NativeClass::map_obj:
      return std::make_unique<native::MapObj>();
    case NativeClass::catapult_shot:
      return std::make_unique<native::CatapultShot>();
    case NativeClass::area:
      return std::make_unique<native::Area>();
    case NativeClass::adv_area:
      return std::make_unique<native::AdvArea>();
    case NativeClass::sacrifice:
      return std::make_unique<native::Sacrifice>();
    case NativeClass::dest_lock:
      return std::make_unique<native::DestLock>();
    case NativeClass::count:
      break;
  }
  // Unreachable for any enumerator; a decor stub is the harmless answer for a
  // value cast in from outside, in the spirit of Result's "boring zeroes".
  return std::make_unique<native::Decor>();
}

std::unique_ptr<NativeObject> make_native_object(std::string_view cpp_class) {
  const auto id = native_class_from_name(cpp_class);
  if (!id) return nullptr;
  return make_native_object(*id);
}

}  // namespace imperivm::core
