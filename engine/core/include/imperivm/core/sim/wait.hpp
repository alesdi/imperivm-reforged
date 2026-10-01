#pragma once

/// The `Wait*` family: twelve implemented entry points over one piece of
/// runtime machinery.
///
/// A `Wait*` call **suspends the calling script until a predicate holds or a
/// timeout expires**, and returns whether it held. 338 call sites across the
/// 885 shipped scripts, led by `WaitQueryCountBetween/4` at 208.
///
/// Twelve entry points and eleven names: `WaitConvRequest` ships **two
/// overloads**, `(Obj, Obj, int)` and `(Query, Query, int, Obj&, Obj&)`, which
/// the registry tells apart by arity the way it does everywhere else.
///
/// ## The machinery is `HostStatus::retry`, and it is the original's own
///
/// `gbr.exe`'s interpreter (`0x0069d4c0`) calls a host entry point through
/// opcode `0x1E` and dispatches on the body's return value: **0 means done**
/// (advance the instruction pointer by 5 and carry on) and **1 means suspend
/// and run this same call again** -- `0x0069d5e3` leaves the program counter
/// where it is, so the argument frame is untouched and the call re-enters from
/// the top. `Sleep` (`0x006945e0`) uses exactly the same protocol and returns 1
/// exactly once; a `Wait*` returns 1 as many times as it needs to.
///
/// `script::HostStatus::retry` is that, and it predates this file: the VM has
/// always left the arguments and the instruction pointer in place for it, and
/// its comment named `WaitNonEmptyQuery` as the case it existed for. What was
/// missing is that a re-entered call had **no memory** -- it could not tell its
/// ninth run from its first, so it could not know whether its own timeout had
/// expired. `Execution::retrying` and `Execution::retry_since` are that memory,
/// and `CallContext::waiting_since` is how a body reads it.
///
/// This is why a waiting script needs no save format of its own. It is
/// suspended *at* the call with its arguments still on the frame stack, so the
/// predicate's inputs are already serialised by the frame serialiser that has
/// always been there.
///
/// ## Condition first, and no mandatory yield
///
/// Every body tests its predicate **before** it considers suspending, and a
/// predicate that already holds completes in the same slice --
/// `0x005ed082`..`0x005ed099` runs before the poll path at `0x005ed0b4`. That
/// is not a detail: 147 of the 208 `WaitQueryCountBetween` sites pass a timeout
/// of **100 ms**, and a turn in this engine is 200 to 800 ms long, so a family
/// that always yielded once would answer `false` to every one of them.
/// `if (WaitQueryCountBetween(q, 1, 60, 100))` is the shipped idiom for "is
/// this true right now", and it only works because of this.
///
/// ## The poll intervals are data, not tuning
///
/// A lockstep peer that polls on a different cadence wakes a script on a
/// different turn, which is a desync. The three cadences are immediate operands
/// in `gbr.exe`, and the slice actually taken is
/// `min(interval, remaining timeout)` -- so the last poll of a finite wait
/// lands **exactly** on the deadline rather than overshooting it.
///
///   * **100 ms** -- every query-based wait. Four independent sites carry the
///     literal: the shared helper at `0x005eca10`/`0x005eca27`, the two-query
///     helper at `0x005eca70`/`0x005ecaa8`, and inlined copies in
///     `WaitHealthBetween` (`0x005ed41b`) and `WaitPlayerChat` (`0x005ee973`).
///     The helper reads a *hint* from the query -- `max(hint - now, 100)`.
///
///     **The floor does not always win, and this header used to say it did.**
///     `0x00571700` -- four instructions returning the current game time, so
///     that the difference is zero -- is the *base* hint, and only the two
///     explicit-membership query kinds inherit it: `CVXGroupQuery`
///     (`0x7c47e8`) and `CVXNamedObjQuery` (`0x7c4830`). The other twelve query
///     vtables override the slot with `0x005e1350`, which returns
///     `[this+0x4c]`, the query's own cache-expiry stamp. A `CVXObjQuery` sets
///     its period to 1000 (`0x004ff356`), so a wait on one with a fresh cache
///     polls at 1000 ms, not 100. **The cadence of a query wait depends on the
///     runtime kind of the query.** This engine does not cache query evaluation
///     at all (`sim/query.hpp`), so it has no expiry stamp to read and uses the
///     floor throughout -- a labelled divergence, and the same one
///     `sim/query.hpp` records from the other side. The eleven remaining kinds'
///     periods were not read.
///   * **1000 ms** -- `WaitSettlementCapture`, `WaitSettlementAllied`,
///     `WaitConvRequest`, `WaitUnitsInArea`, `WaitIdleUnitsInArea`
///     (`0x005ecb30`, and inline at `0x005ee4e4`).
///   * **2000 ms** -- `WaitEnvIntBetween`, `WaitEnvStringEqual`, `WaitAddNote`,
///     `WaitRemoveNote` (`0x005ecaf0`, and inline at `0x005ecc27`).
///
/// ## The timeout
///
/// The last argument, in game-time milliseconds, and it has two sentinels that
/// the shipped scripts both use:
///
///   * **negative means never time out.** `0x005eca37` skips the countdown
///     entirely. 108 of the 331 corpus sites pass `-1`.
///   * **zero means test once and answer `false`.** The slice becomes
///     `min(interval, 0)` and the `> 0` test at `0x005ed0cb` fails.
///     `if (!WaitUnitsInArea(Group("Oasis_Guards" + i), "A_OasisB" + i, 0))` is
///     a shipped site.
///
/// The original counts the timeout **down in place** in its own argument slot,
/// which is sound there because the frame survives the suspension. This engine
/// measures elapsed time from `waiting_since` instead and computes the same
/// slice from it. The two agree turn for turn -- for a 250 ms timeout both poll
/// at 0, 100, 200 and 250 -- and not mutating the frame keeps the arguments a
/// script wrote equal to the arguments a save reads back.
///
/// ## What is not reproduced, and is recorded rather than smoothed
///
///   * **`NamedObj` needs no coercion, and this header used to say it did.**
///     The open question was how `gbr.exe` converts a `NamedObj` to a `Query`
///     for the ten shipped sites that pass one. It does not convert: `NamedObj`
///     (0x22) is registered as a *subtype* of `Query` (0x20) at `0x005b6d96`,
///     so the compiler upcasts at zero cost (`0x0068efee`) and the call
///     boundary never reads a type word at all. The query behind a named object
///     holds exactly the one bound object while it lives and goes **empty** --
///     not invalid -- when it dies, which is what makes the tutorial's
///     `(Caesar, 1, 1, -1)` then `(Caesar, 0, 0, -1)` pair read as "wait until
///     he exists" and "until he is gone". `sim/wait.cpp`'s `evaluate` does
///     that. One divergence stays: the original's `Wait*` bodies never call the
///     query's purge slot, so between a named object's death and its
///     destruction `NamedObj::IsDead` answers true while a `Wait*` on the same
///     name still counts it. This engine has no such window.
///   * **An invalid query handle answers `true`.** `0x005ed05f` re-checks the
///     handle every poll and, on failure, ends the wait *successfully* after
///     calling `0x00686eb0` -- which in the retail build is a bare `ret`, so
///     the diagnostic is a dead string. A broken wait silently succeeds. This
///     engine refuses by name instead: see `kDeadQuery`.
///   * **Per-call scratch.** The original declares extra frame bytes at
///     registration (`0x00699eb0`'s fourth argument) that survive suspension,
///     and `WaitSettlementCapture` uses four of them to resolve its settlement
///     name once rather than on every poll. This engine resolves each poll.
///     Observable only if a name's binding changes mid-wait, which nothing here
///     can do.
///   * **Atomic sections.** `[thread+0x3c]` is a nesting counter, and inside
///     one the suspend path does not end the slice: the call busy-spins until a
///     watchdog breaks the section. No such concept exists here.
///   * **`WaitConvRequest` keeps no request.** The original files the pair in a
///     manager (`0x996adc`) and lets the input layer complete it, because only
///     the input layer can see which object the player has selected. This
///     engine tests the meeting directly, which drops that selection gate and
///     leaves the entry point stateless like the other eleven. The full
///     argument, the manager's four operations and the reach test are in
///     `sim/conversation.hpp`; the two bodies are at the foot of
///     `sim/wait.cpp`.

#include <cstddef>

#include "imperivm/core/script/host.hpp"

namespace imperivm::core::sim {

/// The poll cadences, in game-time milliseconds. See the header.
inline constexpr std::int64_t kQueryPollInterval = 100;
inline constexpr std::int64_t kSettlementPollInterval = 1000;
inline constexpr std::int64_t kEnvPollInterval = 2000;

/// Decide what a blocking entry point does next.
///
/// `elapsed` is `now - waiting_since` and `timeout` is the call's own last
/// argument. Returns the slice to suspend for, or **zero when the wait is
/// over** -- which the caller turns into `false`.
///
/// This is `0x005eca10`'s arithmetic with the hint folded out: `slice =
/// min(interval, remaining)`, clamped at zero, and no countdown at all when the
/// timeout is negative.
[[nodiscard]] std::int64_t wait_slice(std::int64_t elapsed, std::int64_t timeout,
                                      std::int64_t interval) noexcept;

std::size_t register_wait_host(script::HostRegistry& registry);
[[nodiscard]] std::size_t wait_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
