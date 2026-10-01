#pragma once

// Turning a click into a command: selection, the default order, and issuing.
//
// Queue mechanics: sim/command.hpp. Class data: game/class_graph.hpp.
// Command rows: `DATA\COMMANDS\*.XML`. Default orders: `<defaultcmd>` in
// `DATA\CLASSES\*.SC.XML`.
//
// ## The one script that specifies this whole file
//
// `DATA\SUBAI\GROUP_UNITSOUT.VS` is the `groupdispatch=` script of the
// `unitsout` command, and its header comment is the signature of a player
// order as the original engine passes it:
//
//     //void, ObjList objs, point pt, Obj obj, bool bReplace, bool bModifier, int player
//
// and its last line is
//
//     l.ExecDefaultCmd(pt, obj, bReplace, bModifier);
//
// which is `ObjList::ExecDefaultCmd/4` -- the entry point
// `docs/formats/vs-host-api.md` records with one call site and no signature.
// Six facts fall out of those two lines, and they are the whole design:
//
//   1. An order carries **both** a point and an object. Not one or the other:
//      a right click always has a position, and sometimes also has something
//      under the cursor. So `OrderTarget` has both fields.
//   2. `bReplace` is `SetCommand` versus `AddCommand(false, ...)`. That is
//      `OrderMode`, and `sim/command.hpp` already documents the two halves.
//      `exec_cmd_impl` in `sim/command.cpp` reads its own fourth argument the
//      same way, from the same evidence.
//   3. `bModifier` is one boolean, and `<cmd ctrl="1"/>` inside `<defaultcmd>`
//      is the only thing in the shipped data that reads it.
//   4. The order is issued to an `ObjList`, in list order.
//   5. `player` -- the issuing player -- is separate from the objects' owners,
//      and the script filters with `DiplGetShareControl(u.player, player)`
//      before it commands anything. That is the game's own answer to "whose
//      units may I command", and it is not "the ones I own": it is the ones
//      whose owner grants me `share_control`.
//   6. `ClearSelection(.player)` then `l[i].Select(.player)`: **selection is
//      mutated by name**, not a value a script passes around. The player id
//      those two carry looks like an index and is a guard; see below.
//
// ## Selection
//
// **There is one selection, and the player argument is a guard rather than an
// index.** That is a correction. This header used to read "sixteen selections,
// one per player", on the reasonable evidence that every shipped
// `ClearSelection` and every shipped `Select` passes a player id. The
// registrations say otherwise. `ClearSelection` (0x005a8120) turns its argument
// into a pointer to that player's record, compares it against the pointer the
// globals block keeps at `+0x12c8` -- *the local player's record* -- and
// **returns having done nothing when the two differ**. Only when they match
// does it empty the one selection the process owns. `Obj::Select` (0x005abfc0)
// and the two-argument `Obj::Deselect` (0x005ac060) make the identical
// comparison before they touch anything.
//
// So the argument asks *is this my screen*, and a lockstep peer running the
// same script for a player who is not its own does nothing at all. That is the
// reason a selection is not sync state, and it is a stronger reason than the
// one recorded below the fold: it is not that we choose not to hash it, it is
// that the original engine's selection legally differs between peers by
// construction.
//
// The sixteen slots survive that correction, because a hotseat or a replay
// embedder has sixteen screens to keep and nothing can observe the fifteen a
// given peer is not showing. What the *script* entry points do is guard on
// `HostContext::local_player`, which is what the original does.
//
// A selection is an **ordered, duplicate-free list of object ids**:
// `_GetSelection()` returns an `ObjList`, `UNITSADVICE8.VS` filters it
// (`_GetSelection().ObjClass(cWagon)`) and `SCDEBUG.XML` indexes it backwards,
// so order is observable. It is a `std::vector<ObjectId>`, never a set:
// iteration order is state. The deduplication is in the insert (0x005b9170),
// which walks the list for a matching id before it appends.
//
// `Obj::Select(player)` gates on four things in this order, and three of them
// are silent: an unresolvable handle prints *"called for an uninitialized or
// invalid object"* and a dead one prints *"called for a dead object"* -- both
// into the discard sink, so both are an ordinary return here -- then the
// local-player comparison, then `SetNoselectFlag`'s bit (0x08000000 at
// `[obj+0x2c]`), which drops the object without a word.
//
// `Obj::Deselect()` -- the no-argument member, and the only form the corpus
// uses -- has **no player comparison at all** (0x005ac0e0). It removes from the
// one selection unconditionally. The two-argument form, which nothing calls,
// is the guarded one.
//
// `SwapSelectedObj(from, to)` -- 7 sites, every one a summoning or a death that
// replaces one object with another -- is **remove and append, not a
// substitution in place**. That is the second correction, and this header had
// the reverse: "`Selection::swap` exists and is not `remove` plus `add`".
// 0x004c84e0 asks whether `from` is in the selection (0x005e3930), removes it
// through *Deselect's* helper, and then runs *Select's* helper on `to`, which
// appends. So the summoned wolf lands at the end of the selection and the
// druid's position is not kept. A null `to` leaves the removal standing; a null
// `from` does nothing at all; and there is no player guard, because there is no
// player argument and only one selection to act on.
//
// `IsSelectionAssigned(p)` and `WasSelectionAssigned(p)` are **not about the
// selection**. That is the third correction, and it is the one that changes
// what the tutorial means. `TUTORIALS\GENERALADVICE6.VS` nests them
// `if (Was...) break; else if (Is...) break;` around a hint, and both ask about
// the *numbered control groups*:
//
//   * `IsSelectionAssigned(p)` (0x004c6aa0) bounds `p` to 1..16 and then walks
//     **ten** groups at `record + 0xac`, stride 0x2c, answering true at the
//     first non-empty one. Ten is the count `ShortcutTable::kSlots` guessed
//     from convention and declined to defend; this is the evidence for it.
//   * `WasSelectionAssigned(p)` (0x004c6a00) does not look at the world at all.
//     It reads the environment store key **`/Player<p>/SelectionAssigned`** and
//     answers whether any bit is set.
//
// The two-argument forms of both, which no shipped script calls and which are
// therefore not bound, name a single group -- a bit for `Was`, an index for
// `Is`.
//
// That key is the one path in the binary with no `/<map>/` prefix; every other
// player-scoped key is `/%s/Player%d/...`. So it is a **root-scope** key and it
// outlives the map, which is what a question about a habit rather than about a
// mission needs. Nothing in this engine writes it: the two writers in `gbr.exe`
// are in the input layer, and `SetShortcutSel` is not one of them, so a script
// that assigns a control group does not make the tutorial believe the player
// has learned to. That is the whole point of asking two questions instead of
// one.
//
// `Obj::_LastSelectionTime()` (0x005abd60) is `[obj+0x144]`, and **-1** both for
// an object that has never been selected and for a handle that does not
// resolve. `TUTORIALS\BUILDINGSADVICE9.VS` counts the player's buildings that
// answer anything else -- *has this player ever clicked on one of these?* The
// stamp is written by the selection-changed handler (0x005e7af8) for **every
// member of the new selection**, not only for the one just added, which is why
// it is refreshed here by `note_selection_changed` rather than by `add`. It
// lives on `SelectionTable` and not on `ObjectState` because `ObjectState` is
// hashed and this is a UI artefact that legally differs between peers.
//
// **`GetSelection/0` is a name the original never registered.** It is in the
// declared surface because `DATA\SUBAI\DEBUG_DUMP.VS` calls it, and the string
// pool holds `_GetSelection` and nothing else. That call site therefore cannot
// ever have run past its first line, and the entry point is left declared and
// unimplemented on purpose: binding it would be inventing an entry point rather
// than recovering one, and it would hide the fact about the script.
//
// **What is still not decided: whether an enemy's units may be selected.** The
// executable does gate on something -- the insert helper (0x005e7d80) calls the
// object's own `vtbl + 0x74` with the local player's record and drops the object
// when it answers zero -- so, unlike what this header said before, there *is* a
// per-player predicate in the path. What it computes is not recovered, and it
// is not visibility alone, because the selection is populated from a mouse pick
// that has already answered that. Nothing in 577 scripts, in the 823 class
// files, or in the string table names it. So `is_selectable` still answers only
// what the data answers -- the class-level `non_selectable="1"` flag (`Tower`,
// `Wall`, `FakeTower`, and nothing else, which is also what makes every other
// building selectable), plus alive and not garrisoned -- and ownership is left
// to the embedder. `issue_default_order` separately refuses to command what the
// issuer does not control, which is the half `GROUP_UNITSOUT.VS` does settle.
//
// ## The default order
//
// **The game declares this; it is not hard-coded, and the obvious mapping is
// wrong.** 32 class files carry 108 `<defaultcmd>` blocks holding 173 `<cmd>`
// rows. A block is keyed on a *target class*, `target=""` meaning bare ground,
// and holds an **ordered list of candidate commands**. `Unit` against a
// `Building` is five deep:
//
//     <defaultcmd target="Building">
//       <cmd name="attack_independent"/>
//       <cmd name="capture"/>
//       <cmd name="attack"/>
//       <cmd name="enter"/>
//       <cmd name="approach"/>
//     </defaultcmd>
//
// so "own building -> enter, enemy building -> attack" is not a rule the engine
// applies; it is an *outcome* of walking that list. What decides is the
// `verify=` script on the `<method sig>` the command names:
// `attack_independent` verifies `settlement.IsOutpost() && IsIndependentGuarded()`,
// `capture` verifies `IsValidCaptureTarget && IsEnemy`, `attack` verifies
// `IsEnemy && IsValidTarget`, `enter` verifies `!IsEnemy && DiplGetShareView`,
// and `approach` -- last in almost every block in the game -- has **no
// verifier at all**. That is the shape: an ordered list ending in an
// unconditional fallback, filtered by predicates the class graph already
// records. Every conditional verb in a `<defaultcmd>` has a `verify=`; every
// unconditional one does not.
//
// The resolution rules, and where each comes from:
//
//   * **Point or object.** A point target matches `target=""`; an object
//     target matches a block whose `target` names a class the object is an
//     heir of. `<cmdtext target="">` rows in `DATA\COMMANDS\UNIT.XML` say
//     "Click to move at the point" against the empty target, which is the same
//     convention.
//   * **Most derived target class wins, per contributing class.** `Unit`
//     declares both `target="Tower"` (attack, enter, approach) and
//     `target="Building"` (attack_independent, capture, attack, enter,
//     approach), and `UNIT.SC.XML` puts a comment above them: `<!-- no capture
//     for towers, gates and walls -->`. That comment is only true if the
//     `Tower` block is authoritative for a tower and the `Building` block is
//     not consulted. Document order would give the opposite answer for `Hero`,
//     which `Unit` declares *after* `target="Unit"`: a hero is a unit, and
//     matching `target="Unit"` first would offer `attack` (fails on a friend),
//     `stay_hidden` (fails), `approach` (passes) and never reach the
//     `target="Hero"` block's `attach`, so right-clicking your own hero would
//     walk up to it instead of joining it.
//   * **A derived class's block for a target *replaces* the one it inherits,
//     and `<nodefcmdinherit/>` throws the whole inherited table away.**
//     `RamUnit` settles the first half: it declares `target="Unit"` as
//     `(approach)` alone, with no `<nodefcmdinherit/>`, and that block only
//     means anything if it erases `Unit`'s `(attack, stay_hidden, approach,
//     attack_unit_type[ctrl])`. A battering ram that inherited `attack` would
//     attack units, which is precisely what the block -- and `capture`'s
//     `<nsrc obj="RamUnit"/>` -- exist to prevent. `CCatapult`'s ground block
//     says it again: `(attack_ground, attack_ground[ctrl])` replaces the
//     inherited `(move, advance[ctrl])`, so right-clicking terrain with a
//     catapult fires at it rather than walking to it.
//
//     `<nodefcmdinherit/>` -- only `Sentry` and `Wagon` -- then covers the
//     targets the child does *not* redeclare: a sentry declares blocks for
//     `Unit` and `Building` and must not keep `Unit`'s ground `move`, its
//     `Hero` `attach`, or its `Teleport`.
//
//     This is `ClassGraph::resolved_default_cmds`' rule, with one difference.
//     **Two blocks for the same target in the same file concatenate.**
//     `HERO.SC.XML` declares `target="Unit"` twice, once as (attack,
//     attack_unit_type[ctrl]) and once, eight lines later, as (stay_hidden);
//     replacing within a class makes the first of the two dead code and takes
//     a hero's ability to attack a unit by right-clicking it with it.
//     Replacement is right *between* classes, where the child is overriding,
//     and wrong *within* one, where the author is extending. That single
//     divergence is asserted in `engine/tests/test_orders.cpp` rather than
//     merely claimed here.
//   * **`ctrl="1"` partitions the list; it does not extend it.** `Unit`'s
//     ground block is (move, advance[ctrl]). If the modifier merely *added*
//     `advance` as a further candidate, `move` -- which has no verifier --
//     would win every time and `advance` would be dead. So a candidate is
//     eligible exactly when its `ctrl` flag equals the modifier state.
//     `CCatapult`'s ground block, (attack_ground, attack_ground[ctrl]),
//     confirms it: both halves are spelled out because the class is overriding
//     an inherited (move, advance[ctrl]) pair on both sides.
//
// ## `<method sig>` is overloaded on the argument's kind, and the class graph
// drops one half of every overload
//
// Sixteen classes declare the same `<method sig>` twice. `sc-xml.md` records
// this as an unexplained duplicate and assumes last-wins; the shipped file
// names say plainly what it is:
//
//     BDruid:  <method sig="hide" vs="data/subai/druid_hide_unit.vs"/>
//              <method sig="hide" vs="data/subai/druid_hide_ground.vs"/>
//     GDruid:  <method sig="heal" vs="data/subai/druid_heal.vs"
//                                 verify="data/subai/druid_heal_verify.vs"/>
//              <method sig="heal" vs="data/subai/druid_heal_ground.vs"/>
//     Unit:    <method sig="attack" vs="data/subai/unit_attack.vs"
//                                   verify="data/subai/unit_attack_verify.vs"/>
//              <method sig="attack" vs="data/subai/unit_advance.vs"/>
//
// `_unit` against `_ground` in one class and `_heal` against `_heal_ground` in
// another is not a coincidence and not a leftover: **the first declaration is
// the object-argument form and the second is the point-argument form**, and
// only the object form carries a verifier, because only the object form has
// anything to verify. `Unit`'s second `attack` binding `unit_advance.vs` --
// the *ground*-attack script -- says the same thing a third time.
//
// `ClassGraph`'s `upsert` keeps the last of two same-sig declarations, so the
// object form is gone before this file can see it. Measured over the shipped
// data: of the 173 `<cmd>` rows in the 108 `<defaultcmd>` blocks, **21 resolve
// to a different `verify=` under first-wins than under last-wins**, and 20 of
// those 21 are `attack` (the remaining one is `heal`; `hide`'s two forms both
// lack a verifier). Every one is an object target, since the point form is the
// one that survives.
//
// The consequence, unmitigated, is that `attack` becomes unconditional and a
// unit right-clicking its own town hall attacks it -- `attack_independent` and
// `capture` both verify `IsEnemy` and fail, and `attack` would then pass
// without asking. **This file does not work around it**, because the fix is one
// line in a file it does not own: `ClassMethod` needs to keep both forms (or
// `upsert` must merge `verify` forward), after which `resolve_default_order`
// picks the object form for an object target with no further change here.
// `DefaultOrder::unverified` counts the candidates taken on an empty `verify=`
// so the hole is visible in a run rather than only in this comment, and
// `test_orders.cpp` pins the loss with a class that declares `attack` twice.
//
// A candidate names a row in `DATA\COMMANDS\*.XML`, not a `<method sig>`
// directly: the executable's message for a bad one is `Unknown Command: %s`,
// emitted from the same routine as `'cmd' tag should apear inside a
// 'defaultcmd' tag`. The row supplies the `method=` that is actually queued --
// `catapult_attack` has `method="attack"` -- along with its costs and
// `execdelay`. A candidate naming no row is skipped and counted;
// `stay_hidden` really is one (it appears in three `<defaultcmd>` blocks and
// in no `<commands>` file), so this is a shipped-data case and not a
// hypothetical.
//
// ## What this file refuses
//
//   * **`<src>` / `<nsrc>`, the per-command actor-class filter.** 516 `<src>`
//     and 59 `<nsrc>` rows in `DATA\COMMANDS` say which classes may issue a
//     command at all, and they matter: `move` carries `<nsrc obj="Sentry"/>`.
//     `CommandTable`/`CommandDef` in `sim/command.hpp` parse neither, and that
//     file is not this one's to change. The filter is therefore **not applied**,
//     and where it would have mattered `<nodefcmdinherit/>` mostly covers the
//     same ground (`Sentry` inherits no ground block in the first place).
//     Wiring it in means adding two vectors to `CommandDef`.
//   * **`groupverifier=` (351 rows).** It takes `ObjList objs` and an out
//     parameter, returns bool, and gates whether a command *button* is
//     enabled. Nothing shows it participating in default-order resolution, and
//     guessing that it does would change which verb a click produces.
//   * **`SetCommandOffset` / `offset="1"` (18 rows).** The formation spread
//     that keeps a group ordered to one point from stacking. `CommandSystem`
//     exposes no offset, so an order is issued to the bare point.
//   * **Charging costs.** `exec_cmd_impl` does not charge either, for the
//     reason recorded there: the shipped AI charges itself and double-charging
//     is a silent economy bug. The `CommandDef` is returned so a caller that
//     wants to charge can.
//
// ## What is blocked today, and by exactly what
//
// 15 `verify=` scripts are reachable from a `<defaultcmd>`. The list of entry
// points they call that `script/host_surface.cpp` declares and nobody has
// written was, when this file was written, eleven:
//
//     IsValidCaptureTarget/1  IsCentralBuliding/0  IsIndependentGuarded/0
//     IsTTent/0               IsWaterUnit/0        UnitsMax/0
//     CanSee/1                GetSacrifice/0       GetParty/0   GetInnState/1
//     class/0  (member)
//
// Seven are now written. Five of them -- `IsIndependentGuarded/0`, `IsTTent/0`
// (sim/economy.cpp) and `IsValidCaptureTarget/1`, `IsCentralBuliding/0`,
// `IsWaterUnit/0` (sim/world_host.cpp) -- are what turned every building click
// from `blocked` into a verb; `UnitsMax/0` and `GetParty/0` came after and
// unblock nothing on their own (see below). Four remain:
//
//     CanSee/1   GetSacrifice/0   GetInnState/1   class/0  (member)
//
// A trapped verifier is `unknown`, and `unknown` blocks. Two verifiers are
// still blocked and it is worth naming what by, because neither is the entry
// point it looks like: `SHIP_BOARD_VERIFY.VS` and `UNIT_BOARD_VERIFY.VS` reach
// `UnitsMax` *and* `UnitsCount`, and `UnitsCount/0` answers for a `Settlement`
// receiver only -- a `Ship` one traps. `UNIT_TRANSPORT_VERIFY.VS` reaches
// `GetParty` beside `GetInnState/1` and `class/0`.
//
// ## Determinism
//
// Everything here is ordered: selections are vectors in insertion order,
// candidates come out in resolution order, and orders are issued in the
// caller's list order. Nothing is hashed. A selection is not part of the
// original's sync state -- the desync dumps carry no selection channel -- and
// making one hashed state would put a UI artefact into the determinism
// contract. It is serialisable so that a save can restore what the player had
// selected; that is a different question from what the simulation agrees on.

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

// --------------------------------------------------------------------------
// selection
// --------------------------------------------------------------------------

/// One player's selection: an ordered, duplicate-free list of object ids.
///
/// A vector rather than any kind of set, because `_GetSelection()` hands the
/// thing to scripts that index it and because iteration order is state
/// (docs/engine/architecture.md). Insertion order is preserved on every
/// operation that is not `clear`, including `swap`.
class Selection {
 public:
  /// Append unless already present. Returns whether it was added.
  bool add(ObjectId id);
  /// Remove, preserving the order of everything else. Returns whether it was
  /// there.
  bool remove(ObjectId id);
  /// Remove if present, otherwise append. Returns whether it is present after.
  bool toggle(ObjectId id);
  /// Clear and add each of `ids` in order, dropping duplicates.
  void assign(std::span<const ObjectId> ids);
  void clear() noexcept { ids_.clear(); }

  [[nodiscard]] bool contains(ObjectId id) const noexcept;
  /// The index of `id`, or `size()` when absent.
  [[nodiscard]] std::size_t index_of(ObjectId id) const noexcept;

  /// `SwapSelectedObj(from, to)`: remove `from`, then append `to`.
  ///
  /// **Not a substitution in place**, which is what this said until the two
  /// helpers 0x004c84e0 calls were read: they are exactly Deselect's and
  /// Select's, in that order, so the replacement lands at the end. A no-op
  /// returning false when `from` is not selected. `to` may be `kNoObject`, and
  /// then this is only the removal. When `to` is already selected nothing is
  /// appended -- the list stays duplicate-free, and a druid that summons a wolf
  /// already in the selection must not produce a selection holding it twice.
  bool swap(ObjectId from, ObjectId to);

  /// Drop everything the world no longer holds, or holds with no health left.
  /// Returns how many went. `IsAlive` is `health > 0`, matching
  /// `sim/world_host.cpp`.
  std::size_t prune(const World& world);

  [[nodiscard]] std::span<const ObjectId> ids() const noexcept { return ids_; }
  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
  [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
  /// `kNoObject` past the end.
  [[nodiscard]] ObjectId at(std::size_t index) const noexcept {
    return index < ids_.size() ? ids_[index] : kNoObject;
  }

 private:
  std::vector<ObjectId> ids_;
};

/// Sixteen screens' worth of selection, one per player.
///
/// **The original has one**, the local player's; see the header. The slots are
/// here so that one process may stand in for several screens -- a hotseat, a
/// replay, a test that runs two players' scripts in one world -- and the script
/// entry points reach exactly one of them, the one `HostContext::local_player`
/// names.
///
/// Fixed-size for the reason `PlayerTable` is: `kPlayerCount` is a property of
/// the format, and indexing by `PlayerId` is what the corpus does
/// (`ClearSelection(wagon.player)`).
class SelectionTable {
 public:
  /// Out-of-range ids land on a shared scratch selection rather than
  /// out-of-bounds memory, so a bad `PlayerId` from a script is inert.
  [[nodiscard]] Selection& player(PlayerId id) noexcept;
  [[nodiscard]] const Selection& player(PlayerId id) const noexcept;

  /// Add to one player's selection. `Obj::Select`.
  bool select(PlayerId id, ObjectId object);
  /// `Obj::Deselect`. **The declared entry point takes no player** --
  /// `host_surface.cpp` has `{"Deselect", 0}` against `{"Select", 1}` -- so the
  /// script-facing form is `forget(object)` and this two-argument form is for
  /// the embedder, which does know whose click it was handling.
  bool deselect(PlayerId id, ObjectId object);
  /// `ClearSelection(player)`.
  void clear(PlayerId id) noexcept;
  void assign(PlayerId id, std::span<const ObjectId> objects);

  /// `SwapSelectedObj(from, to)` across every player, ascending. The corpus
  /// never names a player for it, and a summoned wolf must replace its druid in
  /// whoever's selection held it. Returns how many selections changed.
  std::size_t swap_object(ObjectId from, ObjectId to);
  /// Drop `object` from every selection. For an object that has just died.
  std::size_t forget(ObjectId object);
  /// `prune` every player's selection, ascending. Returns the total dropped.
  std::size_t prune(const World& world);

  /// Stamp `now` on every object in `id`'s selection. `_LastSelectionTime`.
  ///
  /// The original's selection-changed handler (0x005e7af8) does this for the
  /// whole new selection after **every** change, which is why it is a call of
  /// its own rather than something `select` does: the embedder that edits a
  /// `Selection` directly -- a rubber band, a shift-click -- makes the same one
  /// call afterwards that the host entry points make.
  void note_selection_changed(PlayerId id, GameTime now);

  /// When `object` was last in a selection that changed, or **-1** for never.
  [[nodiscard]] GameTime last_selected(ObjectId object) const noexcept;

  /// Little-endian, self-describing, versioned: the convention
  /// `Scheduler::serialize` follows.
  void serialize(std::vector<std::byte>& out) const;
  Status deserialize(std::span<const std::byte> bytes);

 private:
  std::array<Selection, kPlayerCount> selections_{};
  /// `(object, when)`, ascending by object. A sorted vector rather than a map
  /// for `EnvStore`'s reason: iteration order is what gets serialised, so it
  /// must not depend on a hash seed.
  std::vector<std::pair<ObjectId, GameTime>> stamps_;
  Selection scratch_;  ///< where an out-of-range PlayerId goes
};

/// Whether `id` is a thing a player may put in a selection.
///
/// Three things, and only three, because only three are evidenced:
///
///   * it exists and has health above zero (`Obj::IsAlive`);
///   * it is not inside a holder -- a garrisoned unit has no position and the
///     original stores `(-1, -1)` for it (`sim/system.hpp`), so there is
///     nothing on the map to click;
///   * its class does not resolve `non_selectable="1"`. Exactly three classes
///     declare it -- `Tower`, `Wall` and `FakeTower` -- which is also the
///     evidence that every other building *is* selectable.
///
/// Ownership is **not** tested. See the header note.
[[nodiscard]] bool is_selectable(const World& world, ObjectId id) noexcept;

/// Whether `issuer` may command `id`: `DiplGetShareControl(id.owner, issuer)`,
/// and not while `SetCmdEnable(false)` stands on the object.
///
/// `GROUP_UNITSOUT.VS` filters exactly this way before it selects and commands,
/// and `World`'s own visibility test uses the same call in the same direction
/// (`diplomacy.has(owner, viewer, Relation::share_control)`). A player always
/// controls their own units because `player<i>.xml` gives every player the
/// self-relation word `0x35`, which has bit 5 set.
[[nodiscard]] bool is_commandable(const World& world, ObjectId id, PlayerId issuer) noexcept;

// --------------------------------------------------------------------------
// the target of an order
// --------------------------------------------------------------------------

/// Where the player clicked: a point, and whatever was under it.
///
/// Both, never one -- `ExecDefaultCmd(pt, obj, ...)`. `object == kNoObject` is
/// a click on bare ground and selects the `target=""` blocks.
struct OrderTarget {
  Point point{};
  ObjectId object = kNoObject;

  [[nodiscard]] bool is_object() const noexcept { return object != kNoObject; }
};

/// `bReplace`: `SetCommand` versus `AddCommand(false, ...)`.
///
/// `replace` aborts the running command and everything queued behind it;
/// `append` adds to the back and disturbs nothing but a resting `idle`, which
/// gives way (`CommandSystem::append_order`, a labelled reading: an `idle`
/// never returns, so an order queued behind it would never run). That is
/// shift-click.
enum class OrderMode : std::uint8_t {
  replace,
  append,
};

// --------------------------------------------------------------------------
// candidates
// --------------------------------------------------------------------------

/// One `<cmd>` inside a `<defaultcmd>`, resolved against a target.
struct OrderCandidate {
  /// The `<cmd name>`. A row in `DATA\COMMANDS\*.XML`, not a `<method sig>`.
  std::string_view command;
  /// The class whose `<defaultcmd>` contributed it.
  ClassIndex from_class = kNoClass;
  /// The `<defaultcmd target>` class it was found under, `kNoClass` for
  /// `target=""`.
  ClassIndex target_class = kNoClass;
};

/// The ordered candidate list for (actor class, target class, modifier).
///
/// Pure over the class graph: no world, no verifiers, no commands table. That
/// is what makes it testable against the shipped class tree without a running
/// simulation.
///
/// `target_class` is `kNoClass` for a click on the ground. `modifier` is
/// `bModifier`; a candidate is eligible exactly when its `ctrl` flag matches.
/// Appends to `out` and returns how many were appended.
std::size_t default_command_candidates(const ClassGraph& graph, ClassIndex actor_class,
                                       ClassIndex target_class, bool modifier,
                                       std::vector<OrderCandidate>& out);

// --------------------------------------------------------------------------
// verification
// --------------------------------------------------------------------------

/// What a `verify=` script said.
enum class OrderVerdict : std::uint8_t {
  pass,
  fail,
  /// Could not be evaluated: no verifier wired up, no such script in the
  /// library, or it trapped or suspended. **Not** the same as `fail`: a
  /// candidate that might have passed must not be silently skipped in favour
  /// of a later one, because that would emit a verb the original would not.
  unknown,
};

/// Runs a `<method verify="...">` script.
///
/// An interface because running one needs a scheduler, a host registry and a
/// `HostContext`, and `default_command_candidates` deliberately needs none of
/// those. `ScriptOrderVerifier` below is the real implementation; a test that
/// only cares about candidate order passes null.
class OrderVerifier {
 public:
  virtual ~OrderVerifier() = default;

  /// `bool f(Obj this, Obj other)` for an object target, `bool f(Obj this,
  /// point pt)` for a point target -- the two shapes the shipped verify
  /// scripts declare in their header comments, and the shape follows the
  /// target, not the script.
  virtual OrderVerdict verify(std::string_view vs_path, ObjectId actor,
                              const OrderTarget& target) = 0;
};

/// Runs verifiers on the real VM, synchronously, off to the side.
///
/// `Scheduler` has no "run this and give me the answer": it owns coroutines and
/// reports counts. But `script::start` and `script::run` are free functions
/// over a `Chunk` and a `VmEnv`, and `Execution::result` holds the returned
/// value, so a predicate can be run without a `ScriptRecord` ever existing.
/// That matters: a verifier is asked on every mouse move, and a scheduler
/// record per hover would show up in the save.
///
/// The script id in the environment is `kNoScript`, which is what a host
/// function that keys anything by script sees. `sim/objlist.hpp` pools by
/// script, so a verifier that builds an `ObjList` writes into the `kNoScript`
/// slot; the shipped verifiers do not, and one that did would be visible as a
/// growing pool rather than as corruption.
///
/// A verifier that suspends (none of the shipped ones can: they have no `Sleep`
/// and no `Goto`) or traps is `unknown`, and the trap is counted.
class ScriptOrderVerifier final : public OrderVerifier {
 public:
  ScriptOrderVerifier(script::Scheduler& scheduler, HostContext& context) noexcept
      : scheduler_(&scheduler), context_(&context) {}

  OrderVerdict verify(std::string_view vs_path, ObjectId actor,
                      const OrderTarget& target) override;

  /// How many verify scripts trapped, and how many were not in the library.
  [[nodiscard]] std::size_t traps() const noexcept { return traps_; }
  [[nodiscard]] std::size_t missing() const noexcept { return missing_; }
  void reset_counts() noexcept { traps_ = 0; missing_ = 0; }
  /// The last trap, as `file:line: detail`, or empty -- what a blocked click
  /// can say about itself. Diagnostic only; nothing decides on it.
  [[nodiscard]] const std::string& last_trap() const noexcept { return last_trap_; }

 private:
  script::Scheduler* scheduler_;
  HostContext* context_;
  std::size_t traps_ = 0;
  std::size_t missing_ = 0;
  std::string last_trap_;
};

// --------------------------------------------------------------------------
// resolution
// --------------------------------------------------------------------------

enum class DefaultOrderStatus : std::uint8_t {
  /// `command`, `verb` and `def` name what to issue.
  resolved,
  /// Every candidate's verifier said no, or there were no candidates. The
  /// click does nothing -- which is a real outcome: a peasant right-clicking
  /// bare ground has a `move` block, but a `Sentry` has none at all.
  none,
  /// A candidate could not be verified, so no later candidate may be taken.
  /// `blocked_by` names it. Not an error: it is this file declining to guess.
  blocked,
};

struct DefaultOrder {
  DefaultOrderStatus status = DefaultOrderStatus::none;
  /// The `<cmd name>` chosen.
  std::string_view command;
  /// The `<method sig>` it queues: `CommandDef::method`, which differs from
  /// `command` for `catapult_attack` (`method="attack"`) and 316 other rows.
  std::string_view verb;
  /// The row, for a caller that wants its costs, `execdelay` or `param`.
  const CommandDef* def = nullptr;
  /// Set when `status == blocked`.
  std::string_view blocked_by;
  /// Candidates dropped because `DATA\COMMANDS` has no row of that name.
  /// `stay_hidden` is one in the shipped data.
  std::size_t unknown_commands = 0;
  /// Candidates that passed because their method binds no `verify=`, rather
  /// than because a verifier said so. Correct for `approach`, `move` and the
  /// other 150 unconditional rows; **see the overload note in the file header**
  /// for the 21 rows where it is currently wrong because the class graph has
  /// dropped the declaration that carried the verifier.
  std::size_t unverified = 0;
};

/// Which command one actor would run against one target.
///
/// The whole pipeline: candidates from the class graph, rows from the commands
/// table, verifiers from `verifier` (null means every `verify=` is `unknown`,
/// so the first conditional candidate blocks). Stops at the first candidate
/// that passes.
[[nodiscard]] DefaultOrder resolve_default_order(const World& world, const CommandTable& commands,
                                                 ObjectId actor, const OrderTarget& target,
                                                 bool modifier, OrderVerifier* verifier);

// --------------------------------------------------------------------------
// issuing
// --------------------------------------------------------------------------

/// Queue one already-chosen command on one object.
///
/// `SetCommand(verb, arg)` or `AddCommand(false, verb, arg)`, with the
/// argument taken from `target` the way `exec_cmd_impl` takes it: the object
/// wins over the point when both are present, because a targeted verb is aimed
/// at the object and the shipped sites pass a dummy point beside it. The
/// command is marked `user`, which is what `Unit.GetCommanded` reports.
///
/// Returns the new command's id, or 0.
///
/// `user` is what `Unit.GetCommanded` will report: true for a click, false for
/// the script form, which the original issues as player "nobody" -- see
/// `ExecDefaultCmd` below.
std::uint32_t issue_order(World& world, ObjectId actor, const CommandDef& def,
                          const OrderTarget& target, OrderMode mode, bool user = true);

/// What one player order did.
struct OrderReport {
  /// Commands queued.
  std::size_t issued = 0;
  /// Actors whose resolution found nothing to do.
  std::size_t unresolved = 0;
  /// Actors whose resolution hit an unverifiable candidate.
  std::size_t blocked = 0;
  /// Actors skipped: not in the world, or the issuer does not control them.
  std::size_t refused = 0;
  /// Candidates dropped for naming no `DATA\COMMANDS` row, over all actors.
  std::size_t unknown_commands = 0;
};

/// `ObjList::ExecDefaultCmd(pt, obj, bReplace, bModifier)`, plus the
/// share-control filter `GROUP_UNITSOUT.VS` applies before it.
///
/// Resolves per actor -- the candidate list depends on the actor's class, so a
/// mixed selection of a catapult and a swordsman right-clicking one building
/// really does produce two different verbs -- and issues in `actors` order.
///
/// **`issuer == kNoPlayer` is the script form.** `ObjList::ExecDefaultCmd`
/// (0x0055e140) and `Obj::ExecDefaultCmd` (0x005b52b0) reach the same
/// 0x004f4ff0 a click does, with the player pushed as the literal -1
/// (0x0055e1f1), and the per-unit issue (0x004ef3d0) forwards that plus one --
/// player 0, nobody -- to the command it queues. So a `kNoPlayer` issuer
/// vouches for control (the script filtered by `DiplGetShareControl` before
/// it called), only liveness is tested, and the commands are not marked as a
/// player action. A real player is the click, and both halves apply.
OrderReport issue_default_order(World& world, const CommandTable& commands,
                                std::span<const ObjectId> actors, const OrderTarget& target,
                                OrderMode mode, bool modifier, PlayerId issuer,
                                OrderVerifier* verifier);

/// The same, for the issuer's own selection. `actors` is
/// `selections.player(issuer).ids()`.
OrderReport issue_default_order(World& world, const CommandTable& commands,
                                const SelectionTable& selections, const OrderTarget& target,
                                OrderMode mode, bool modifier, PlayerId issuer,
                                OrderVerifier* verifier);

// --------------------------------------------------------------------------
// what the match loop calls
// --------------------------------------------------------------------------
//
// Two calls per click and one per turn. The `CommandTable` is a parameter above
// so that a test can hand in four rows instead of 399; an embedder always wants
// the one the world is running, and these three save it the lookup and the null
// check.

/// The table the world's `CommandSystem` holds, or null when it has none.
[[nodiscard]] const CommandTable* order_command_table(World& world) noexcept;

/// What the cursor should show: resolve without issuing.
///
/// `DefaultOrder::def` carries the row, so a caller can read its costs. It does
/// **not** carry `<cmd cursor="attack">` -- 46 rows have one and `CommandDef`
/// parses no such field; see the refusals in the file header.
[[nodiscard]] DefaultOrder resolve_default_order(World& world, ObjectId actor,
                                                 const OrderTarget& target, bool modifier,
                                                 OrderVerifier* verifier);

/// The whole of a right click: resolve and issue for the issuer's selection.
/// Every actor is refused when the world runs no `CommandSystem`.
OrderReport issue_default_order(World& world, const SelectionTable& selections,
                                const OrderTarget& target, OrderMode mode, bool modifier,
                                PlayerId issuer, OrderVerifier* verifier);

// --------------------------------------------------------------------------
// the host entry points
// --------------------------------------------------------------------------

/// Bind the selection entry points, and `ExecDefaultCmd/4`. Returns how many
/// bodies were attached.
///
/// Eight of the nine declared selection names -- `GetSelection/0` is the
/// ninth and stays unbound; the header says why -- plus `ExecDefaultCmd/4`,
/// the script form of the whole click, on both an `ObjList` and an `Obj`
/// receiver.
std::size_t register_orders_host(script::HostRegistry& registry);

/// What `register_orders_host` defines. A guard against a silent drop.
[[nodiscard]] std::size_t orders_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
