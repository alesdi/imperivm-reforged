#pragma once

// What the command bar offers a player's selection, and what a press does.
//
// The bar's chrome is `CMDBAR<faction>.INI`: a background, its right cap and
// a hidden `CmdCancel`. The buttons are not in the file. `gbr.exe` builds
// them at run time, up to `CONST.INI`'s `[UIBars] CmdBarMaxButtons = 16`,
// each an icon in the faction's frame (`CMDBAR/FRAMES.INI`), from the
// command rows the selection's classes offer -- and the rows are the data:
//
//     <cmd name="trainBSwordsman" button="actions/train BSwordsman.bmp" key="s"
//          groupverifier="data/subai/verify_cmdcost_building.vs"
//          rollover="Equip Swordsman" costgold="50" ... method="train" param="BSwordsman">
//       <src obj="BBarracks"/>
//     </cmd>
//
// `<src>` and `<nsrc>` say which classes offer a row: the commands loader
// files them in the class reverse index (`CommandClassIndex` in
// `sim/command.hpp`), every class an offered list `[class+0x1f8]` and a sticky
// one `[class+0x204]`. `groupverifier=` is a script, `bool f(ObjList objs, str
// OUT reasonText)`, run over the selection to decide whether the button is
// lit and, when it is not, why -- the same synchronous side-run that
// evaluates a `verify=` and an info-bar slot. `<cmdtext target="…">` says
// what the row is aimed at: a row with none is issued when its button is
// pressed, a row with any waits for a click on the map. `groupdispatch=`
// names a script that issues the order itself for the whole group, run to
// its end inside the press (0x004ef3d0).
//
// ## The bar's list, as `gbr.exe` fills it (0x005e7710)
//
// The interface keeps the selection as the selection insert (0x005b9170)
// builds it, rebuilt member by member at every change (0x005e7910): the
// objects, the **classes** -- each member's class the first time it is met,
// in the members' order -- a type and a control flag. The fill:
//
//   1. **Nothing unless the selection is controlled** (0x005b8ae0): its
//      control flag, which an object whose owner shares control with the
//      local player sets (the insert keeps a foreign object only alone), or a
//      lone foreign building (type 4) whose class is a `Stonehenge`
//      (0x0059c020) -- the circle's spells are any mage's who stands near.
//   2. **The sticky rows**, a union: for each of the selection's classes in
//      order, its sticky list in order, a row taken -- once -- when some
//      selected object of exactly that class is let have it (0x005e38d0: the
//      object's `vtbl+0xd8`, `CommandSystem::command_enabled`). So `move`,
//      sticky on `Unit`, stays on the bar for a hastatus and a sentry
//      together, the sentry's `<nsrc>` notwithstanding.
//   3. **The offered rows**, an intersection: the *first* class's offered
//      list in order, a row taken when every selected object's class has it on
//      its offered list too and every object is let have it
//      (0x005e7834..0x005e78c8). A refusal hides the row: a building that is
//      not a ruin shows no Repair, a ruin nothing else, and a row `CmdDisable`
//      took away is gone. This step does not look at the rows step 2 took, so
//      a row a selection has both ways stands twice.
//   4. **Sorted by `priority`** (`[def+0x1c0]`, `atol` as a short), with
//      `std::list::sort` (0x005e7250, merge 0x005e5b60): stable, so equal
//      priorities keep the list's order, which is the class lists' -- a
//      class's own rows by load id, then what it inherits.
//
// The bar widget (`CVXCmdInterface`, 0x006b3d60) then walks the list and
// fills its `CmdBarMaxButtons` slots in order with the rows it draws:
//
//   * a row with no button image (`[def+0x174]`, loaded from `button=`; a
//     missing file is the dummy icon) is passed over -- `transport`, `enter`,
//     `attack_unit_type`, reached by a right click and never from the bar;
//   * each other row is judged (0x006b3190): its group verifier when it has
//     one, and, when that passes or there is none, for a one-time row
//     (`CommandDef::one_time`, the research rows) the interface's own test
//     (0x006b3050 -> 0x005e20c0): refused while the row is in flight -- posted
//     and not yet executed -- or, for a `plrresearch` row, while the player of
//     the selection's first object, a building, holds it; that refusal's
//     reason is `" "`. **A row refused with no reason is not drawn** and takes
//     no slot; one refused with a reason is drawn unlit, the reason its
//     tooltip;
//   * hidden slots stay hidden.
//
// ## Not ported, and why
//
//   * `mod=` (`[def+0x7c]`: `no` 0, `yes` 1, else 2) shows a row only without
//     Ctrl (0), only with it (1) or always (2); `menu=` (`[def+0x5c]`) is a
//     sub-menu's number that the pending row's must match. No shipped row
//     carries either, so every row is "always" and "menu 0".
//   * `researchverifier=` (`[def+0x16c]`), a player-level verifier the judge
//     prefers to the group verifier: no shipped row carries it.
//   * While a row waits for its target the original's bar shows `CmdCancel`
//     alone (the pending order at the interface's `+0x11c` is a row whose
//     `menu` is 0); that is the app's (`engine/app/main.cpp`).
//   * The buttons' places are the interface layer's (`ui/paint.hpp`).

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/session.hpp"

namespace imperivm::core::sim {

/// One button.
struct CommandButton {
  std::string name;         ///< the `<cmd name>`
  std::string icon;         ///< `button=` under `gameres/CmdBar/`, a virtual path
  std::string rollover;     ///< translation key, the tooltip's title
  std::string description;  ///< translation key, the tooltip's sentence
  std::string key;          ///< `key=`, the shortcut letter
  std::string cursor;       ///< `cursor=`, while waiting for a target
  bool enabled = true;      ///< the group verifier's verdict; true with none
  std::string reason;       ///< what the verifier said when it refused, translated
  bool needs_target = false;
  std::int32_t priority = 0;
  /// `costgold=`, `costfood=`, `costpop=`, `coststamina=`: what the tooltip
  /// says the row costs. Zero for free.
  std::int32_t cost_gold = 0;
  std::int32_t cost_food = 0;
  std::int32_t cost_pop = 0;
  std::int32_t cost_stamina = 0;
};

/// The command bar, from the running game.
class CommandBar {
 public:
  explicit CommandBar(GameSession& session);
  ~CommandBar();
  CommandBar(const CommandBar&) = delete;
  CommandBar& operator=(const CommandBar&) = delete;

  /// `CmdBarMaxButtons`, 16 in the shipped `CONST.INI`.
  void set_max_buttons(std::size_t count) noexcept;
  [[nodiscard]] std::size_t max_buttons() const noexcept;

  /// The buttons `player`'s selection shows right now, at most `max_buttons`,
  /// each verifier run. Empty for an empty selection.
  [[nodiscard]] std::vector<CommandButton> describe(PlayerId player);

  enum class Press : std::uint8_t {
    /// The row was issued to the selection, or its dispatch script ran.
    issued,
    /// The row wants a target; `aim` finishes it.
    waiting,
    /// The verifier refused; nothing happened.
    disabled,
    /// No such row, or the selection does not offer it.
    unknown,
  };

  /// The keys held at a press or an aim.
  ///
  /// `gbr.exe` reads them live when the bar posts the order (0x005e39f0),
  /// and they mean two different things: **Shift** (virtual key 0x10) is
  /// whether the order is appended -- `bReplace` is `!Shift` for every row but
  /// a `traincommand` one, which never replaces -- and **Ctrl** (0x11) is the
  /// order's `bModifier`, which a `groupdispatch=` script is handed and every
  /// other issue carries to the class verbs. A right click reads the same two
  /// keys the same way (0x005e6050: `+0xc` from Shift, `+0x10` from Ctrl).
  struct Keys {
    bool shift = false;
    bool ctrl = false;
  };

  /// What a press posts, as a lockstep order carries it: the flags the bar
  /// wrote from the keys at the click, before any row rule is applied.
  struct Flags {
    /// Shift: append rather than replace. A train row appends either way.
    bool append = false;
    /// Ctrl: `bModifier`.
    bool modifier = false;
    /// How many times the row is issued: once, or -- Ctrl on a
    /// `traincommand` row -- `CONST.INI`'s `[GamePlay] TrainMultipleCount`
    /// times (5 shipped).
    ///
    /// **One order with a count, not five orders.** With Ctrl held on a train
    /// row, 0x005e39f0 reads `TrainMultipleCount` and swaps the order it is
    /// about to post for a copy (0x005e3680: every field, then the count as a
    /// *byte* at `+0x24`), clears its replace flag, and posts that one order
    /// through the local command path (0x0051d3c0) -- so the count crosses the
    /// network inside the order (the serialiser writes it as `count`,
    /// 0x004e5e90). Its execution (0x004e5e60) runs the row's own execution
    /// (0x004f1610) that many times over, each the full issue to the
    /// selection. The count is read on the issuing peer at the click; a key
    /// the table lacks reads zero there, and zero runs nothing. The const.ini
    /// comment beside the key says "with Shift"; the code reads Ctrl (0x11).
    std::uint8_t repeat = 1;
  };

  /// The flags a press of `name` with `keys` held posts, over `player`'s
  /// selection: the row decides `repeat`.
  [[nodiscard]] Flags flags(PlayerId player, std::string_view name, Keys keys);

  /// What pressing the button named `name` with `keys` held does.
  Press press(PlayerId player, std::string_view name, Keys keys);

  /// Finish a `waiting` press with the target the player clicked. Returns
  /// false when the row is not offered any more.
  bool aim(PlayerId player, std::string_view name, const OrderTarget& target, Keys keys);

  /// A one-time row's press posted as an order (0x005e6bd0): the row is
  /// marked in flight, and its button refused, until `executed`. False, and
  /// nothing marked, when it is in flight already: the original abandons that
  /// press. A row that is not one-time is never marked, and answers true.
  ///
  /// For a press that does not happen at the click -- a lockstep order. A
  /// local press runs the row at once, which in the original is a mark raised
  /// and lowered by the next turn, so it is not marked at all.
  bool post(std::string_view name);
  /// The row's order executes (0x004f1610 lowers the mark first, for every
  /// row, whoever posted it).
  void executed(std::string_view name);

  /// What `press` would answer, without doing it: `issued` means pressing
  /// would issue the row now, `waiting` that it wants a target.
  ///
  /// For a lockstep match, where the press does not happen at the click -- it
  /// is an order that happens on the turn every peer agrees on -- but the bar
  /// still has to know, at the click, whether to wait for a second one. It
  /// runs the verifier, which is observation: `imconform observe` measures that
  /// running one changes no hash.
  Press check(PlayerId player, std::string_view name);

  /// The same two, over `actors` rather than `player`'s selection.
  ///
  /// What a lockstep peer applies: an order from another peer names the
  /// actors it was given with, because this peer does not have -- and must not
  /// need -- the issuer's selection. The two forms above are these over the
  /// selection, so a local press and a remote one run the same code.
  Press press(PlayerId player, std::string_view name, std::span<const ObjectId> actors,
              Flags flags);
  bool aim(PlayerId player, std::string_view name, std::span<const ObjectId> actors,
           const OrderTarget& target, Flags flags);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// The session's `NetCommandSink`: a command order through the command bar,
/// a surrender through the match.
///
/// **Only the actors the issuer may command.** A packet's actor list is what a
/// peer *says* it selected; `is_commandable` is what the world says it may
/// order, and it is the same filter a right click goes through. A row that
/// needs a target and arrives pressed rather than aimed does nothing, as it
/// does locally until the second click.
class CommandBarSink final : public NetCommandSink {
 public:
  CommandBarSink(GameSession& session, CommandBar& bar) noexcept
      : session_(&session), bar_(&bar) {}
  /// A session with no command bar -- a headless peer: surrenders and
  /// takeovers apply, command rows do not.
  explicit CommandBarSink(GameSession& session) noexcept : session_(&session), bar_(nullptr) {}
  bool command(const NetOrder& order) override;
  bool surrender(PlayerId issuer) override;
  /// `AIStart` for the departed seat, the way the match setup starts a
  /// computer's: its `playerdata/@AI` profile (usually empty, the root) at
  /// its difficulty overlay. **The takeover is the original's** -- its drop
  /// routine (0x00406840) hands each dropped human seat to the computer
  /// through the body behind `AIStart` (0x00434ca0) with no profile -- and
  /// the turn it happens on is this engine's (`sim/netdepart.hpp`).
  bool take_over(PlayerId departed) override;
  /// `AIStop` for a seat a late joiner takes back (`sim/netjoin.hpp`): the
  /// body behind it (0x00422720) kills the seat's `Main.vs` and every script
  /// under it and forgets its settlement slots. The original never hands a
  /// seat back -- it has no late join -- so using `AIStop` as the inverse of
  /// the takeover is **this engine's**. Whatever the computer had already
  /// ordered its units to do they go on doing, as they would after a
  /// scripted `AIStop`, until the player says otherwise.
  bool hand_back(PlayerId joined) override;

 private:
  GameSession* session_;
  CommandBar* bar_;
};

}  // namespace imperivm::core::sim
