#pragma once

// What the info bar shows for a player's selection, in the terms the
// interface files use -- and read from the data, because the original does.
//
// The bar's chrome is 24 INI files (`ui/interface.hpp`). What goes *in* it is
// declared per class in `DATA\CLASSES\*.SC.XML`:
//
//     <properties interface="thumb,unit,items"/>         the selection tags
//     <properties icon="gameres/icons/RPraetorian.bmp"/>  the portrait
//     <properties display_name="Praetorian"/>            the name, a translation key
//     <value0 icon="gameres/infobar/common/level ico.bmp"
//             script="return .AsUnit.level;" rollover="Level"/>
//     ... value5
//
// The six stat slots are **inline `.vs`**, one expression each, evaluated
// against the selected object -- the only place in the shipped data where
// script source is embedded rather than referenced. `sim/orders.hpp` already
// runs a `verify=` script synchronously off to the side; this runs a slot's
// script the same way, with `this` bound to the selection, and hands back the
// value's text and the slot's icon. A class that declares no `<valueN>` leaves
// the slot empty, and the bar draws nothing there, which is what the original
// shows for a building with three values.
//
// ## Which class
//
// The class whose declarations are read is not always the selected object's.
// 0x005e5720 decides, and its rule is:
//
//   * nothing selected -- the `Empty` class (`EMPTY.SC.XML`, `interface="empty"`);
//   * one object -- its own class, except a `Wagon`, which shows as
//     `WagonGold` or `WagonFood` by what it carries;
//   * several -- `<R>PeasantMulti` when every one is a `Peasant` of one race
//     and the class exists (`RPEASANTMULTI.SC.XML` and its seven siblings);
//     otherwise, by the *first* selected object: a `Wagon` -> `Wagons`; a
//     `Hero`, or a selection of more than one class that is not led by a
//     `Sentry` -> `Multi` (`MultiOneShip` for a `ShipBattle`); else -- one
//     class, or sentries -- `MultiOne`, `MultiOneShip` for a ship,
//     `MultiOneRanged` for a unit with a projectile.
//
// Those pseudo-classes carry `<valueN>` scripts of their own, over the
// `Sel*` family (`SelAvgLevel()`, `SelHealth() * 100 / SelMaxHealth() + '%'`),
// which `sim/orders.cpp` implements from the same executable. **Labelled:**
// "one class" is read off a count the selection object keeps at +0x118 and
// "has a projectile" off a string at unit+0x960; this engine tests the
// selection's class ids and the class's `projectile` property for them.
//
// ## The strips
//
// The six widget classes that draw a row of cells -- `BuildingQueue`,
// `UIHolder`, `UIInventory`, `HeroSkills`, `UnitSpecials`, and `Combiner`
// over two of those -- are filled from the systems that hold what they show:
// the building's command queue (its `train` rows, by `queueicon`), the
// `UIHolder` list (below), the hero's items (`ITEMS.XML`'s `image`), the hero's
// skills (`SKILLS.INI`, in file order, which the file itself says is the
// icon order), and the class's `unit_specials` (`UNIT_SPECIALS.INI`). Each
// cell is an icon, an optional number, an optional bar and a frame choice;
// what a cell's number and bar *mean* per strip is recorded on `InfoCell`.
//
// ## The `UIHolder` strip
//
// `CVXUIHolder` shows one list, built by 0x006d2ad0 from the selection:
//
//   * nothing selected -- nothing;
//   * one **hero** (`SyncFlags` bit 24, the word at `[obj+0x2c]`) -- its
//     army, the list at `[hero+0x1cc]` that `hero.army` answers, in attach
//     order and **without the hero**;
//   * one other object -- its holder's contents (the object's own virtual at
//     `vtbl+0xe4`, the list at `+0x28` of what it returns): here, the
//     garrison of the settlement the object is the holder (or settlement,
//     warehouse, anchor) of; nothing for an object with none;
//   * several -- the selected objects in selection order, then each
//     selected hero's army after them, but only when the selection is a
//     `Multi` one by the class rule above (more than one class, or a hero in
//     front) and not every member is a `Sentry` (0x005400c0); else nothing.
//
// The refresh (message `0x17050001`/`0x17050002`, 0x006d2d20) folds that
// list into cells, one per key, in the order a key is first met:
//
//   * a hero -- its own cell;
//   * a unit whose hero is also in the list -- its hero's cell, except when
//     the selection is one hero alone (then its army is keyed as below);
//   * a `Sentry` -- one cell for every sentry (key 0x1234abcd);
//   * any other -- its class and its owner (`[class]` + `[[obj+0x70]+8]`).
//
// A cell's number is how many it holds (`_itoa` of the cell's count at
// 0x006edecf, drawn at `NumberYPosition`) -- a hero's cell counts only its
// army, and shows no number when it has none (0x006d3119) -- and its bar is
// the summed health over the summed maximum. A click (0x006d3550) marks a
// cell and 0x006d3250 selects the marked cells' units in place: a class or
// sentry cell's units, a hero cell's hero alone (the army folded into it
// is skipped, 0x006d342b). **Not modelled, labelled:** the original keeps a
// cell where it was across refreshes and appends new keys at the end, where
// this rebuilds the order from the list each time; the two differ only when
// a key appears between two refreshes of one selection.
//
// Nothing here draws. `ui/paint.hpp` draws what this describes, and the
// application carries one to the other.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/session.hpp"

namespace imperivm::core::sim {

/// One of the six stat slots.
struct InfoSlot {
  std::string icon;  ///< the `<valueN icon>` path, or empty
  std::string text;  ///< what the slot's script returned, as text
  bool present = false;
};

/// One cell of an icon strip.
struct InfoCell {
  /// The bitmap: a class `icon` (queue, garrison), an item `image`, a skill
  /// or special `icon`.
  std::string icon;
  /// The text drawn at the strip's `NumberYPosition` (or, for skills, at
  /// `IconTextOffset`): a `UIHolder` cell's count, an item's remaining
  /// charges, a skill's points. Empty for none.
  std::string number;
  /// A bar 0..100 at `HealthBarYPosition`: a `UIHolder` cell's summed health
  /// over its summed maximum, a queue entry's training progress. -1 for none.
  std::int32_t health = -1;
  /// Which frame bitmap: `NoSelFrameImage`, `SelFrameImage` (a `UIHolder`
  /// cell the player has marked -- the application's to say, since the mark
  /// is interface state), `TrainFrameImage` (the queue entry in progress),
  /// `WaitFrameImage` (the ones behind it).
  enum class Frame : std::uint8_t { normal, selected, train, wait } frame = Frame::normal;
  /// `UnitSpecials`: the name beside the icon. `HeroSkills`: the rollover.
  std::string text;
  /// `UIInventory`: draw `BackGlowImage` rather than `BackImage` -- an item
  /// with a use script, which the original glows to say it can be used.
  bool glow = false;
  /// `HeroSkills`: the plus sign, when the hero has a point to spend here.
  bool plus = false;
  /// The object behind the cell, for a click: a `UIHolder` cell's first
  /// member (a hero cell's hero), the item.
  ObjectId object = kNoObject;
  /// `UIHolder`: what a click on the cell selects, in list order -- a class
  /// or sentry cell's units, a hero cell's hero alone -- and the cell's key,
  /// which names it across refreshes (a hero, a class and owner, or every
  /// sentry). Empty and 0 for the other strips.
  std::vector<ObjectId> objects;
  std::uint64_t key = 0;
  /// `BuildingQueue`: the queued command's id, which a click on the cell
  /// cancels -- the strip keeps it with each entry (`[item+0x28]`, read by
  /// 0x006bf7c0) -- and 0 for the other strips.
  std::uint32_t command = 0;
};

/// Everything about the selection that the files cannot know.
struct SelectionInfo {
  /// The class the declarations were read from: `Empty`, `Multi`, the
  /// object's, or one of the pseudo-classes above.
  std::string class_id;
  /// The class's `interface` words, split: `thumb`, `unit`, `items`, ...
  std::vector<std::string> tags;
  std::string name;       ///< translated `display_name`
  std::string icon;       ///< the class `icon`, the portrait
  std::int32_t health = -1;  ///< 0..100 for the selection's health bar, -1 none
  std::array<InfoSlot, 6> values{};

  std::vector<InfoCell> queue;
  std::vector<InfoCell> holder;
  std::vector<InfoCell> items;
  std::vector<InfoCell> skills;
  std::vector<InfoCell> specials;

  std::size_t selected = 0;  ///< how many objects the selection holds
  /// Slot scripts that trapped or would not compile, once each, so a caller
  /// can report them rather than see a blank slot.
  std::vector<std::string> complaints;
};

/// Builds what the info bar shows, from the running game.
///
/// Holds the compiled slot scripts -- one chunk per distinct script text,
/// because `return .AsUnit.level;` is declared by dozens of classes -- and
/// the two icon tables. `skills_ini` is `DATA\SKILLS.INI` and `specials_ini`
/// is `DATA\UNIT_SPECIALS.INI`; either may be empty, and the strip that
/// needs it is then empty too.
class InfoBar {
 public:
  InfoBar(GameSession& session, std::span<const std::byte> skills_ini,
          std::span<const std::byte> specials_ini);
  ~InfoBar();
  InfoBar(const InfoBar&) = delete;
  InfoBar& operator=(const InfoBar&) = delete;

  /// What `player`'s selection shows right now. Cheap enough to call every
  /// turn: the scripts are compiled once and are one expression each.
  [[nodiscard]] SelectionInfo describe(PlayerId player);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// The class whose declarations a selection shows, by the rule above. Exposed
/// for the test that pins the rule; `describe` calls it.
[[nodiscard]] std::string selection_class(const World& world, std::span<const ObjectId> selection);

/// The objects the `UIHolder` strip lists for `selection`, before they are
/// folded into cells: 0x006d2ad0's rule, above. Exposed for the test that
/// pins the rule; `describe` calls it.
[[nodiscard]] std::vector<ObjectId> holder_list(World& world, std::span<const ObjectId> selection);

}  // namespace imperivm::core::sim
