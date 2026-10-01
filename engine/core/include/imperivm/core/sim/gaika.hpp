#pragma once

/// `GAIKA`: the AI's area-of-interest node, and the one thing established about
/// it here -- **how a script names one**.
///
/// `docs/formats/vs-host-api.md` counts 241 uses over 33 distinct members, and
/// `Squad.AIDest/0` alone is 36 sites. Every one of them was unreachable
/// because no `script::Value` could carry a GAIKA. This header fixes that and
/// **nothing else**: it says what the runtime representation is, and it does
/// not say what a GAIKA *contains*. There is no table, no loader and no
/// `RunStrat` in this file, on purpose. See "What is deliberately absent".
///
/// ## A GAIKA is an integer index. Six independent sources say so.
///
/// 1. **`gbr.exe` declares the same operation twice, once with each type.**
///    The host signature table carries
///
///        SetMAIKA    void, GAIKA gSrc, GAIKA gDst, GAIKA gMAIKA
///        DumpMAIKA   void, int Src,    int gDst,   int gMAIKA
///
///    -- the same three arguments, declared `GAIKA` for the script API and
///    `int` for the debug console. It does it again for the centre accessor
///    (`GAIKA::Center` against `GetGaikaCenter: point, int nGaika`), for the
///    view command (`ViewGAIKA: void, int gaika`) and for the AI order queue
///    (`AIO_SendSquad: void, int nSquad, int nGAIKA, int nWeight`, whose
///    script-side spelling is `AIOSendSquad.vs = void, SquadList l, GAIKA g`).
/// 2. **The engine's own diagnostic calls it an index**: `GetAIControlledObjects
///    called with invalid GAIKA index`.
/// 3. **Every GAIKA-valued field prints with `%d`.** `Squad::Dump` is
///    `Squad %d(%d)[%d/%d], SrcGAIKA: %d, GAIKAIn %d, DestGAIKA %d,
///    AIOrderDest %d, State %s, Flags: %04x, Strat: %s` -- four GAIKA fields,
///    four `%d`, while the state and the strategy get `%s` through `SS_STR` and
///    `GS_STR`. `%s GAIKA %d at (%d,%d): squads: %d` says it a fifth time.
/// 4. **Three corpus sites compare a GAIKA against an integer literal.**
///    `DATA\AI\SQUADMONITOR.VS`: `if (sq.AIDest > 0)` (lines 78 and 170) and
///    `if (sq.GAIKAIn == sq.AIDest || sq.AIDest == 0)` (line 106). An ordering
///    comparison against `0` is not something an opaque handle supports.
/// 5. **`GetGAIKA(int)` and `GAIKACount()` close the space.** `GAIKAMONITOR.VS`
///    walks `for (i = 1; i < GAIKACount(); i += 1) gaika = LAIKA(AIPlayer, i);`
///    and `PRIORITIZE.VS` walks `GetGAIKA(i)` over the same range, so the ids
///    are a dense range whose **lower bound is 1**.
/// 6. **`GAIKA.ID` is the identity conversion, which is why it exists.** 33
///    sites, and every one of them is somewhere an `int` is syntactically
///    required and a `GAIKA` would not typecheck: string concatenation
///    (`pr("GAIKA " + g.ID + ...)`), an `int`-typed host argument
///    (`EnvWriteInt(AIPlayer, "HomeGaika", nHomeGaikaID)`), or a comparison
///    against an `int` local (`if (g.ID != nHomeGaikaID)`). Not one GAIKA-to-
///    GAIKA comparison in the corpus writes `.ID` on either side. Under the
///    integer reading both halves of that split fall out of one representation;
///    under an opaque-handle reading each would need its own host mechanism.
///
/// So a GAIKA value is `script::Value::integer(id)`, and it needs no `TypeId`.
/// `sim/squad.hpp`'s squad handle *does* take one, and the difference is not an
/// inconsistency: a squad is a `(index, player)` pair with no numeric meaning,
/// and `gbr.exe` never declares a squad argument as a bare `int` the way it
/// declares `int gaika` four times over.
///
/// **The cost of this choice, stated plainly.** An integer GAIKA is
/// indistinguishable from any other integer, so `g == 5` is true of GAIKA 5 and
/// `5.settlement` is a call the dispatcher will accept. That is a real loss of
/// type safety, and it is what the original engine does -- `DumpMAIKA` proves
/// the two spellings are interchangeable there too.
///
/// ## Zero is "no GAIKA"
///
/// `SQUADMONITOR.VS` line 106 tests `sq.AIDest == 0` as "this squad has no
/// destination", the commented-out `if (sq.SrcGAIKA == 0)` in `EVALRECRUIT.VS`
/// reads the same way, and both corpus walks over the id space start at 1. So
/// index 0 is reserved, exactly as squad index 0 is (`sim/squad.hpp`) and as
/// `SS_IDLE`, `GS_NONE` and `ES_NONE` are (`sim/ai_profile.hpp`).
///
/// ## The partition is an approximation, and this section is the record of that
///
/// **This header used to refuse to model a node table at all.** The refusal was
/// this, and it still stands as a description of the evidence:
///
/// > `gbr.exe`'s `CVXGlobalAI::Persist()` names its fields -- `GAIKAs`,
/// > `GAIKASlots`, `slotresx`, `slotresy`, `MAIKAs`, `gaikaset`, `setgaika`,
/// > and per node `settlement`, `squads`, `coverage`, `adjcount`, `shipcount`,
/// > `shipneed` -- which is enough to see the *shape*: a fixed-resolution slot
/// > grid over the map, a node per region, a two-way mapping between nodes and
/// > settlements. It is **not** enough to say how the grid is cut, what
/// > `coverage` counts, or what `GAIKA::RunStrat` does when it runs a strategy.
/// > A `GaikaTable` written from the field names alone would be a guess wearing
/// > the clothes of a measurement, and every entry point that consumed it would
/// > inherit the guess.
///
/// **That trade has now been taken deliberately, and by the project's owner
/// rather than by this file.** 314 of the installation's remaining 680
/// unimplemented call sites sit behind that one refusal -- 46% of everything
/// left -- and no amount of care anywhere else reaches them. The decision is:
/// build the partition as a **stated approximation**, ship it, and keep the
/// evidence for going back.
///
/// ### What is measured, and what is approximated
///
/// The two halves are not the same kind of claim and must not be read as one:
///
///   * **The LSA partition is measured.** An LSA is a connected component of
///     the map under a movement domain, and `sim/lsa.hpp` computes exactly that
///     by flood fill over the terrain layer this engine already parses. Its
///     *numbering* need not match the original's, because no script ever writes
///     an LSA id down: every shipped use passes one straight from a producer
///     (`gaika.LSA`, `sq.LSA`) into a consumer (`IsWaterLsa`, `CheckLsaPath`).
///     The ids are opaque handles and the relation between them is what the
///     scripts read.
///   * **The node partition is an approximation, and it is not the original's
///     cut.** `sim/gaika_table.hpp` builds one node per settlement plus one per
///     remaining connected region. The original derives its nodes from a slot
///     grid at `slotresx` x `slotresy` (0x0044e060), which this project cannot
///     reproduce and does not pretend to.
///
/// ### What that costs, named
///
///   * **Node ids do not match the original's**, so a save from the retail game
///     could not be read against this table even if the rest of it were
///     decodable. Nothing here claims otherwise.
///   * **`coverage`, `adjcount`, `shipcount` and `shipneed` are still not
///     modelled.** Nothing in the assigned entry-point list reads them, which
///     is why they can be left out rather than invented; a future entry point
///     that wants one is a reason to reopen this, not a reason to guess.
///   * **The node count differs from the original's**, so `GAIKACount` answers
///     a different number and any script arithmetic keyed to a specific id --
///     there is none in the corpus -- would diverge.
///   * The approximation is **stable and deterministic**: node order is
///     settlement order, then area order, both of which are already part of the
///     simulation's definition.
///
/// ### How to go back
///
/// Everything the refusal asked for is still what would settle it: **a save
/// file with a `CVXGlobalAI` block decoded against the `Persist` names**, or a
/// reading of the slot-grid builder at 0x0044e060 complete enough to reproduce
/// the cut. If either arrives, `GaikaTable::build` is the one function to
/// replace; the entry points read the table through its accessors and none of
/// them encodes the partition rule. `docs/plan.html` carries the same note
/// where the decision is visible to someone who is not reading this file.

#include <cstdint>

#include "imperivm/core/script/value.hpp"

namespace imperivm::core::sim {

/// A GAIKA's index, which is the whole of its identity. `GAIKA.ID` returns it.
using GaikaId = std::int32_t;

/// "No GAIKA". See the header note: both corpus walks over the id space start
/// at 1, and `sq.AIDest == 0` is the corpus's own "no destination" test.
inline constexpr GaikaId kNoGaika = 0;

/// What a fresh per-player node record starts its `Optimism` at (0x0041e294).
inline constexpr std::int32_t kDefaultGaikaOptimism = 100;

/// `MaxGAIKAPriority`, the ceiling `PRIORITIZE.VS` refuses to lower a node
/// from: `if (nOld >= nMaxPriority) continue; // adventure boosted`. It is a
/// script constant in `sim/globals.cpp` as well, and it is named here because
/// `AreaAIMaxPriority` writes exactly this value.
inline constexpr std::int32_t kMaxGaikaPriority = 100;

/// A GAIKA as a script value.
[[nodiscard]] script::Value gaika_value(GaikaId id) noexcept;

/// A script value as a GAIKA. Anything that is not a non-negative integer is
/// `kNoGaika`, so a nil local or a stray handle reads as "no GAIKA" rather than
/// as some arbitrary node -- which is the failure the `> 0` guards in
/// `SQUADMONITOR.VS` are written against.
[[nodiscard]] GaikaId gaika_of(const script::Value& value) noexcept;

}  // namespace imperivm::core::sim
