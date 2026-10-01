#pragma once

/// The three class script hooks: `ondie`, `onkill`, `onenter`.
///
/// ## What they are
///
/// A `<method sig="ondie">` on a class binds a `.vs` file that the engine --
/// not a script -- starts when the object dies. `onkill` and `onenter` are its
/// siblings, bound the same way and started from two other moments. They are
/// ordinary `<method>` elements in `sc.xml` and `ClassGraph::resolved_methods`
/// already resolves them; nothing about the *data* needed work. What needed
/// work is the seam below.
///
/// **Fifteen shipped classes declare one, in seventeen elements** -- `ondie` on
/// fourteen (`Military`, and thirteen casters and mercenaries), `onkill` on
/// `Unit`, and `onenter` on `Unit` twice, which is one of the fourteen
/// duplicate `sig` declarations `docs/formats/sc-xml.md` records and a
/// duplicate that changes nothing because both elements name the same file.
///
/// **Declaring is the small number and it is not the one that matters.**
/// Methods are inherited, so after `ClassGraph::resolved_methods` **203 of the
/// 845 classes answer `ondie`** -- every soldier, because `Military` is the
/// base of every soldier -- and **271 answer each of `onkill` and `onenter`**,
/// which is everything under `Unit`. `docs/plan.html` carried "eighteen shipped
/// classes declare one"; both halves of that were wrong, and the pair of
/// numbers above is what a reader wants.
///
/// ## Why this is a seam and not a call
///
/// `gbr.exe` fires `ondie` from a routine (`0x005141b0`) that sits on **both**
/// the death path and `Erase`. This engine's `Erase` is a host function, which
/// has a scheduler; its death path is `CombatSystem::advance`, which has a
/// world and no scheduler at all. Firing the hook from `Erase` alone would put
/// the mechanism on one of its two callers -- a unit erased by a script paying
/// out Warrior Tales while the same unit killed by a spear did not -- which is
/// worse than not having it, and is why this thread stood open while every
/// accessor around it was written.
///
/// So a system reaches the scheduler the way a host function reaches the
/// compiler: through an interface it does not own. `HostContext::ScriptLibrary`
/// is the precedent, and this is the same shape one layer down --
/// `GameSession` implements `ClassHookRunner` because it is the one place that
/// holds the class graph, the chunk cache and the scheduler at once, and
/// `World` carries the pointer because every firing site already has a world.
///
/// ## What the original does at the moment of firing
///
/// `0x0059b060` is the `ondie` launcher and `0x0059b1c0` / `0x0059b330` are its
/// siblings. Each is a thiscall on the **class descriptor**: it builds the
/// literal (`"ondie"`, `"onkill"`, `"onenter"`), looks it up in the descriptor's
/// own method map at `+0xe8`, and does nothing at all when the lookup misses.
/// On a hit it hands the bound path and the receiver's 16-bit handle to
/// `0x006a0360`, which finds or compiles the chunk by name and calls 0x0069dca0:
/// a fresh context, the interpreter run with a budget of `0x0fffffff`, and the
/// context deleted before the call returns. **A hook is therefore not a
/// coroutine.** It runs to its end inside the routine that fired it, before
/// that routine's next step -- the death virtual's detach from hero and squad
/// comes after it, which is why `CMERCENARY_ONDIE.VS` can still read `.hero` --
/// and it is never a peer of anything in the scheduler. The interpreter saves
/// and restores its current-context globals around the run, so a hook fired
/// from a host call of a script that is itself running (`Obj::Damage`,
/// `Erase`, `Settlement::AddUnit`) nests inside it. See
/// `script::Scheduler::call`, which is this engine's 0x0069dca0, and which
/// says what happens to a hook that tries to sleep.
///
/// This paragraph used to say the opposite -- that a hook was an ordinary
/// coroutine, spawned and run on the scheduler's next pass -- and the engine
/// behaved that way, so a dying unit's hook ran after its turn with the unit
/// already past the point the original ran it at.
///
/// ## Where each one fires
///
///   * **`ondie`** -- on the dying or erased object, itself as the one
///     argument. `0x005141b0` is reached from the unit's death virtual
///     (`0x005db270`, which also scores the settlement penalty) and from its
///     erase override (`0x005db230`), and the hook launch is the *first* thing
///     it does, ahead of the control-group sweep and the two back-reference
///     fixups.
///   * **`onkill`** -- on the **attacker**, with the victim as the second
///     argument, from the block that pays kill experience when a blow lands
///     fatally (`0x005119ee`).
///   * **`onenter`** -- on the object that entered, with the holder's
///     **settlement** as the second argument, from the holder-entry routine
///     (`0x005d3e10`): it detaches the object from its previous holder, refuses
///     when the new holder has no room, and only then writes the holder handle,
///     drops the position to `(-1, -1)` and fires.
///
/// ## The latch, which is the fact that makes this safe
///
/// `0x005141b0` opens by refusing when `[obj+0xbc]` is non-zero, and closes by
/// writing 1 to it (`0x0051446b`). **So the routine runs at most once per
/// object, ever**, and `ondie` fires exactly once whichever path reaches it
/// first. That matters here for the same reason it mattered there: this
/// engine's dying units sit in their death animation (`death_duration_of`)
/// before they leave the world, a script can `Erase` one inside that window, and
/// without the latch `MILITARY_ONDIE.VS` would pay Warrior Tales twice.
///
/// The latch is `ObjectFlags::ondie_fired`, hashed and saved with the rest of
/// the flag word, because fifteen objects sit in the dying state across the
/// nine dumps -- a save can be taken inside the window, and two peers that
/// disagreed about whether a payout had happened would pay it a different
/// number of times.

#include <cstdint>
#include <string_view>

#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

/// The three hooks, in the order their literals appear in `gbr.exe`.
enum class ClassHook : std::uint8_t { on_die, on_kill, on_enter };

/// What a hook is called in the class XML, and how many parameters its script
/// declares.
struct ClassHookShape {
  /// The `<method sig>` to look for. Matched the way every other `sig` is.
  std::string_view sig;
  /// 1 for `ondie` (`// void, Obj This`), 2 for the other two
  /// (`//void, Obj this, Obj Victim` and `//void, Obj this, Settlement sett`).
  /// Read off the shipped scripts' own header comments, which is where every
  /// other spawn site in this engine gets its arity.
  ///
  /// **Only an under-count is observable, and that is worth saying rather than
  /// testing.** `script::start` drops surplus arguments and leaves missing ones
  /// nil, deliberately, because `AIRun` is variadic in the corpus against fixed
  /// signatures -- so passing `ondie` a second argument it does not declare is
  /// a no-op no test can reach, while passing `onkill` one argument short costs
  /// it `Victim`. A fault sweep confirmed exactly that asymmetry.
  std::uint8_t arity;
};

[[nodiscard]] ClassHookShape class_hook_shape(ClassHook hook) noexcept;

/// How a system, or anything else holding only a world, starts a class hook.
///
/// Implemented at the session layer, and **synchronous**: the script has run
/// to completion when this returns. A caller that holds a reference into any
/// container a script can reach -- a system's own records, most of all -- must
/// not use it after the call; re-find by id. Null is a real case and must degrade
/// silently rather than fail: `engine/tests` builds worlds with no
/// installation behind them, and a world with no runner is exactly the
/// original with `[0x9c0824]` set -- the routine's third guard, which skips the
/// launch outright while the game is tearing down.
class ClassHookRunner {
 public:
  virtual ~ClassHookRunner() = default;

  /// Run `hook`'s script on `subject`, to completion. `argument` is the second
  /// parameter for the two-parameter hooks and is ignored by `ondie`. Returns
  /// whether a script ran -- false when the class binds nothing, which is the
  /// common case and not an error.
  virtual bool run_class_hook(ClassHook hook, ObjectId subject, ObjectId argument) = 0;
};

/// Fire `hook` on `subject`, honouring the `ondie` latch.
///
/// Returns whether a script ran. Every guard the original's routine applies
/// is here and in its order: a receiver that does not resolve, a latch already
/// set, and no runner wired.
///
/// **The latch is taken whether or not a script starts**, because the original
/// writes it at the tail of a routine that runs for every object, bound hook or
/// not. A class that binds no `ondie` still burns its one pass, and an
/// implementation that only latched on a launch would let the second pass
/// re-enter for the fourteen classes that bind one.
bool fire_class_hook(World& world, ClassHook hook, ObjectId subject,
                     ObjectId argument = kNoObject);

}  // namespace imperivm::core::sim
