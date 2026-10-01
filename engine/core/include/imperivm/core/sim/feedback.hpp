#pragma once

/// What a script asks the interface to show.
///
/// Nineteen entry points here, 345 shipped call sites, and all but three of
/// them are write-only: the simulation asks for something to appear and never
/// asks whether it did.
///
/// The three that are not are the whole of what this file stores. `ViewPos()`
/// reads back what `View()` wrote, `IsViewLocked()` what `LockView()` wrote,
/// and `GetShortcutSel()` what `SetShortcutSel()` wrote -- a registered getter
/// against a registered setter, which is the test `Unit::user` passes and
/// `Unit::SetLastAttackTime` fails.
///
/// The first four, and the argument for the rest of them:
///
/// | entry point | sites | scripts blocked |
/// |---|---|---|
/// | `CreateFeedback/2` | 72 | 50, twenty of them by nothing else |
/// | `rollover/2` | 70 | 26, nineteen of them by nothing else |
/// | `rollover/3` | 15 | 5 |
/// | `rollover_desc/3` | 1 | 1 |
///
/// ## Why an unread output is worth implementing at all
///
/// Because a trap is not a no-op. `DATA\ITEMSCRIPTS\HEALING HERBS.VS` is three
/// lines long and its **first statement** is
/// `CreateFeedback("Heal", owner.AsUnit())`; until this file existed, the
/// script died there and healed nobody. That is the shape of most of the fifty:
/// item scripts and spell scripts that ask for a sparkle and then do the thing.
/// The sparkle is not simulation state; dying before the healing is.
///
/// `tools/host_coverage.py --scripts` is what made the size of that visible.
/// Ranked by call sites these two names are 4th and 5th; ranked by *scripts
/// they alone block*, they are the top two in the installation.
///
/// ## `CreateFeedback(effect, target)`
///
/// A visual effect at a place. The corpus uses eleven effect names -- `Heal`
/// (25 sites), `Experience` (20), `StaminaBoost` (7), `Lightning3` (6),
/// `Damage1`, `Damage2`, `StaminaLoss` (3 each), `Lightning`, `Lightning2`,
/// `Rage`, `OffensiveTactics` -- and the name is not validated here, because a
/// list closed against the shipped eleven would refuse a mod's twelfth.
///
/// `gbr.exe` registers **four** overloads: `(str, Unit)`, `(str, Unit, int)`,
/// `(str, point)` and `(str, point, int)`. All 72 shipped sites are the
/// two-argument form and every one passes a unit, so the point forms and the
/// three-argument forms are declared and **not bound** -- the rule
/// `sim/campaign.hpp` states for `ShowNotes` and `sim/conversation.hpp` for
/// `ActorPresent`. The one body accepts either shape of second argument, which
/// is `UnitsInSettlement`'s arrangement and for the same reason: the registry
/// keys on `(kind, name, arity)`, so one arity is one body.
///
/// **It stores nothing**, and the precedent is exact: `SetPlayerStatus` is a
/// display request, resolves its player, and keeps no state. Storing a queue of
/// effects would be a serialised, hashed vector that nothing could ever read --
/// the argument `Unit::SetLastAttackTime` makes at length. The receiver is
/// still resolved, because the original resolves it and because an entry point
/// that answers without looking is a test of nothing.
///
/// **The seam, named rather than left implicit:** the day a renderer wants
/// these, it wants a per-turn queue drained by the presentation layer and
/// cleared at the end of the turn -- an *output*, like `SessionReport::traps`,
/// not state. That is a different thing from a field on the world and must not
/// become one, because two peers that draw different sparkles must still hash
/// alike.
///
/// ## The `rollover` family
///
/// These build the tooltip a command button shows, and they answer about **the
/// command the player is pointing at**, not about the object they are handed.
/// That is the finding, and it is what makes the no-argument form make sense:
///
///     rollover()                       -- 0x005b4370
///     rollover(Obj, bool)              -- 0x005b3020
///     rollover(Obj, str)               -- 0x005b3da0
///     rollover(Obj, str, bool)         -- 0x005b39b0
///     rollover_desc(Obj, str, bool)    -- 0x005b4080
///
/// All five open by reading two `std::string` globals at `[0x008210e8]` and
/// `[0x00821104]`, and only fall back to the object when the first is empty.
/// Two globals, two strings -- and `DATA\COMMANDS\*.XML` carries exactly two
/// display strings per row: **`rollover=` on 384 of the 404 rows and
/// `description=` on 253**. The interface writes them before it runs a
/// `groupverifier`, which is the only kind of script that calls this family.
///
/// So `CommandDef` grew the two fields and `HostContext` grew
/// `described_command`, which is the name of the row being described.
///
/// ### The three argument shapes, and how the corpus tells them apart
///
///   * **`bool`** -- whether to show the cost block. `VERIFY_CMDCOST_*` passes
///     `true` (the cost *is* the problem, so show it) and `VERIFY_TRAINEX`
///     passes `false` and appends its own red line about a unit cap (the cost
///     is fine).
///   * **`str` in `rollover/2` and `rollover_desc/3`** -- a message to append.
///     Every site passes a literal: `rollover(this, "Already available")`,
///     `rollover_desc(this, "Already collected", false)`.
///   * **`str` in `rollover/3`** -- a **class name**, whose cost is shown
///     instead of the command's own. Every site passes a variable called
///     `class` or `strUnit`, built from `cmdparam`:
///     `rollover(this, class, true)` in `VERIFY_CMDCOST_BUILDING`.
///
/// Two entry points with the same signature and different meanings for the
/// same parameter is unusual enough to be worth the paragraph; the split is
/// `rollover/3` against `rollover_desc/3` and every one of the 16 sites agrees
/// with it.
///
/// ### What the text is, and what it is not
///
/// The retail formatter is a long `std::string` builder and its output carries
/// the interface's inline markup -- the literals in its body are
/// `<imagetransp gameres/infobar/common/atack ico.bmp>` and its companions for
/// piercing, defence and hearts, plus the section headings `Stats` and
/// `Specials` and the word `None`. `docs/formats/interface-ini.md` already
/// records that markup as a text-rendering feature this engine does not render.
///
/// **This does not reproduce the retail wording**, and `docs/legal.md` rule 4
/// is why: a format specification describes the bytes on disk, and this is not
/// a format. What it produces is a composition of the pieces the row actually
/// carries, in the same markup vocabulary, documented at `compose_rollover`.
/// Nothing in the simulation can tell the difference -- all 86 sites assign the
/// result to `reasonText`, the *out* parameter of a group verifier, and this
/// engine reads a verifier's return value and never its reason.
///
/// ## What is still unknown
///
/// **What the bool gates, exactly.** The reading above -- show the cost block
/// -- fits `VERIFY_CMDCOST_*` and `VERIFY_TRAINEX` cleanly and fits the five
/// Stonehenge verifiers awkwardly: they pass `true` *and* append their own red
/// line. A second reading, "include the requirements as well as the name",
/// fits those five better and the cost ones worse. Nothing in the corpus
/// separates them, because nothing in the corpus reads the string.
///
/// **Whether `groupverifier` gates anything but a button.** `sim/orders.hpp`
/// records the decision not to run them during default-order resolution, and
/// that stands: this file gives them a surface to run *on*, and does not give
/// them a caller.
///
/// ## The camera, the chrome and the control groups
///
/// Fifteen more entry points, 187 call sites, and 44 scripts that no other
/// name blocks:
///
/// | | sites | |
/// |---|---|---|
/// | `View/2` | 35 | move the camera, optionally locking it |
/// | `ShowHint/3` | 26 | the tutorial's advice bubble |
/// | `SetShortcutSel/3` | 24 | a player's numbered control group |
/// | `PlayMovie/1` | 24 | a cutscene |
/// | `BlockUserInput/0`, `UnblockUserInput/0` | 34 | the mouse, during a scene |
/// | `ShowAnnouncement/2`, `HideAnnouncement/1` | 17 | the on-screen ticker |
/// | `ViewPos/0` | 7 | where the camera is |
/// | `ShowTutorial/3` | 7 | the advice bubble's larger sibling |
/// | `PlaySound/1`, `PlaySound/2`, `Obj::PlaySound/1` | 9 | a sound |
/// | `LockView/0`, `UnlockView/0` | 4 | the lock on its own |
///
/// **`ViewPos` is why the view is state.** The shipped idiom is three
/// statements and it appears in three of Zama's sequences:
///
///     p_View = ViewPos();
///     View(AreaCenter("A_View1"), true);
///     ...
///     View(p_View, false);
///
/// Save the camera, take it somewhere for a cutscene, put it back. A `ViewPos`
/// that did not read back what `View` wrote would strand the player looking at
/// the wrong end of the map, and 35 `View` calls would have nothing to restore
/// to.
///
/// **Not world state, and `HostContext` is where it lives.** The camera is the
/// one thing a lockstep peer is *supposed* to differ about -- `local_player`
/// makes the same argument, and `SelectionTable` sits in the same place for the
/// same reason. A camera position folded into the hash would desynchronise two
/// players watching the same battle from different corners.
///
/// `ViewState` carries the block flag too, which has no registered getter. That
/// is a departure from the rule above and a small one: it costs a bool in a
/// struct that is neither hashed nor serialised, and *blocked input is a mode
/// rather than an event* -- the interface has to know it is in one, where it
/// does not have to know that a sparkle happened three turns ago.
///
/// ## What the write-only ones answer
///
/// `PlayMovie` is registered `bool` and `ShowHint` and `ShowTutorial` `int`,
/// and **all 57 shipped sites discard the result**. So nothing in the corpus
/// separates one answer from another, and the answers here are the ones a
/// request that did not happen would give: `false` for a movie nothing played,
/// `0` for a hint nothing showed. Said out loud because a future reader will
/// otherwise assume the values were recovered.
///
/// ## What is still unknown
///
/// **What `ShowHint`'s and `ShowTutorial`'s int is.** A handle to the bubble,
/// most likely, so that something can dismiss it; there is no `HideHint` in the
/// registration table to pair with it, and no call site keeps the value.
///
/// **What `View`'s bool does beyond locking.** It is named `bLock` in the
/// executable's own signature, and the corpus is consistent with that -- the
/// cutscene idiom passes `true` on the way out and `false` on the way back --
/// but `LockView` exists separately, so the two may not be the same lock.
///
/// ~~**Which slots `SetShortcutSel`'s `num` may take.**~~ **Ten**, and that is
/// now evidence rather than convention. `IsSelectionAssigned` (0x004c6aa0)
/// walks the player's record from `+0xac` in strides of 0x2c and stops at ten,
/// and the two-argument form (0x004c6a30) bounds its slot argument to `[0, 10)`
/// before it indexes. The 24 shipped `SetShortcutSel` sites still use 1 and 2
/// only; an out-of-range slot is dropped rather than refused, which is
/// `SelectionTable`'s rule for an out-of-range player. See `sim/orders.hpp`:
/// those two entry points read this table and not the selection, which is what
/// makes them evidence about it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"
#include "imperivm/core/sim/player.hpp"

namespace imperivm::core::sim {

class World;
struct CommandDef;

/// Where the camera is, whether it is pinned, and whether the mouse is live.
///
/// **Not world state.** See the header: this is the one thing two peers of a
/// lockstep match are supposed to disagree about.
struct ViewState {
  /// What `ViewPos()` answers and `View(pt, ...)` sets. World coordinates, the
  /// same ones `AreaCenter` and `obj.pos` produce.
  std::int32_t x = 0;
  std::int32_t y = 0;
  /// `LockView` / `UnlockView`, and `View`'s second argument.
  bool locked = false;
  /// `BlockUserInput` / `UnblockUserInput`. No registered getter; see the
  /// header for why it is here anyway.
  bool input_blocked = false;

  /// `SetFog(bool)` -- whether the fog of war is drawn.
  ///
  /// **Display, not the explored map.** 0x006a51e0 allocates a 16-byte command
  /// object, writes the argument into it at `+0xc` and posts it through
  /// 0x0051d3c0; nothing it touches is the per-player exploration state that
  /// `IsExplored` reads. A cutscene lifts the fog to show the player something
  /// and puts it back, which is exactly the `ViewPos`/`View`/`View` idiom
  /// beside it -- `1_Great_Battles_Zama` map 6 sequence 15 writes
  /// `HideZoomMap(); SetFog(false); p_View = ViewPos(); View(...)` as four
  /// consecutive statements.
  ///
  /// True by default: a match starts fogged.
  bool fog = true;

  /// Whether the zoom (mini) map panel is open.
  ///
  /// `ShowZoomMap` (0x0060e640) and `HideZoomMap` (0x0060e630) are one
  /// instruction each past the indirection -- `vtbl + 0x10` and `vtbl + 0x14`
  /// on the panel at `[0x009d2cc0]`. `HideZoomMap` has 8 shipped sites and
  /// `ShowZoomMap` none, which is what a cutscene needs: the panel is in the
  /// way of the camera and is put back by the interface, not by the script.
  bool zoom_map_open = false;

  /// `_ZoomMapLastShownTime()` -- game time the zoom map was last opened, or
  /// **-1** for never.
  ///
  /// One shipped site, and it is the whole reason the two above are stored:
  /// `DATA\TUTORIALS\GENERALADVICE3.VS` sleeps 140 seconds and then asks
  /// `if (_ZoomMapLastShownTime() == -1)` before offering the "Minimap"
  /// tutorial hint -- *has this player never opened the minimap?* A registered
  /// getter against a registered setter, which is the test `Unit::user` passes
  /// and `Unit::SetLastAttackTime` fails.
  ///
  /// **The player writes it, not the script.** The only shipped writer is
  /// `HideZoomMap`, which does not stamp; the stamp is what opening the panel
  /// does, and in this engine that means `ShowZoomMap` and whatever interface
  /// eventually calls it. So a headless session answers -1 for the whole match,
  /// which is the answer the tutorial is written for.
  std::int64_t zoom_map_shown_at = -1;
};

/// The numbered control groups, sixteen players deep.
///
/// `SetShortcutSel(player, num, list)` **copies the list** rather than keeping
/// the handle: an `ObjList` belongs to the script that declared it and is
/// released when that script ends, and a control group outlives the sequence
/// that set it.
class ShortcutTable {
 public:
  /// Slots per player. Ten, and `IsSelectionAssigned` is why; see the header.
  static constexpr std::size_t kSlots = 10;

  /// `SetShortcutSel`. An out-of-range player or slot is dropped rather than
  /// refused -- `SelectionTable`'s rule, and a script that names slot 99 has a
  /// bug this engine cannot fix for it.
  void assign(PlayerId player, std::int32_t slot, std::span<const ObjectId> objects);

  /// `GetShortcutSel`. Empty for a slot nothing has assigned, and for an
  /// out-of-range one.
  [[nodiscard]] std::span<const ObjectId> group(PlayerId player,
                                                std::int32_t slot) const noexcept;

  /// Drop `object` from every group, for an object that has just died.
  std::size_t forget(ObjectId object);

  void clear() noexcept;

 private:
  std::array<std::array<std::vector<ObjectId>, kSlots>, kPlayerCount> groups_{};
};

/// Build the tooltip for one command row.
///
/// The composition, which is this engine's and is asserted by a test rather
/// than inferred by a reader:
///
///     <rollover>
///     <description>                     when the row has one
///     <message>                         when the caller passed one
///     <cost>                            when `show_cost` and the row costs
///
/// `cost` is the pieces the row declares, in the order the command XML writes
/// them -- gold, food, population, stamina -- each with the icon markup the
/// retail formatter uses, and omitted entirely when all four are zero. Lines
/// are separated by `\n`, which is what every caller that appends to the result
/// also uses.
///
/// `row` may be null, which is what a `rollover()` with nothing being described
/// means; the answer is then the message alone, or the empty string.
[[nodiscard]] std::string compose_rollover(const CommandDef* row, std::string_view message,
                                           bool show_cost);

/// The four entry points this domain owns.
std::size_t register_feedback_host(script::HostRegistry& registry);

/// How many `register_feedback_host` defines. Pinned by a test so that a name
/// added here without a home in the corpus census shows up.
[[nodiscard]] std::size_t feedback_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
