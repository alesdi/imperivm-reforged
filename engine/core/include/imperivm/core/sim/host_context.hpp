#pragma once

/// The one thing `script::CallContext::user` points at.
///
/// ## Why this header exists
///
/// `CallContext` carries a single `void* user`, and Part 5's five domains were
/// built in parallel against it. Each cast it to its own type: movement,
/// economy and the object model to `World*`, heroes to `HeroHostState*`,
/// combat to `CombatHostContext*`. Every domain's tests set `user` to the type
/// that domain expects, so all of them passed -- and no embedder can satisfy
/// more than one of them at once. The first script that called a hero entry
/// point and a movement entry point in the same run would have reinterpreted a
/// `World*` as a `HeroHostState*`.
///
/// That is the same failure mode as the compiler writeback bug: a defect that
/// only surfaces where two independently-correct pieces meet, invisible to
/// tests that exercise either one alone. `docs/plan.html` records both.
///
/// So there is exactly one type here, and every domain casts to it.
///
/// ## What goes in it
///
/// The world, and nothing a domain can derive from the world.
///
/// Systems are **not** members. A domain finds its own system by name through
/// `World::systems()` -- `movement_system`, `hero_system_of`,
/// `combat_system_of` -- which keeps this header from growing a field every
/// time a domain is added, and keeps the lookup deterministic (registration
/// order is part of the simulation's definition, and `System::name()` is
/// documented as stable). A pointer cached here would additionally have to be
/// kept correct across a save and reload, which a name lookup does not.
///
/// The exceptions are the things that are genuinely not derivable from a world
/// and genuinely not world state:
///
///   * `object_type`, the `TypeId` the embedder chose for object handles. The
///     VM never interprets it.
///   * `translations` and `debug`, which serve `Translate` and `pr`. Both are
///     **deliberately outside the world**: a translated string depends on which
///     language pack is installed, so folding one into hashed state would
///     desynchronise a multiplayer game between players running different
///     locales, and debug output is not state at all. Keeping them here rather
///     than on `World` is what makes that impossible rather than merely
///     discouraged.

#include <string_view>

#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/sim/feedback.hpp"
#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"

namespace imperivm::core::sim {

class World;
/// Forward-declared rather than included: `sim/orders.hpp` includes *this*
/// header, and a pointer needs only the name.
class SelectionTable;
class OrderOutbox;  // sim/netcmds.hpp

/// Where a script's `pr(...)` output goes.
///
/// An interface, not a buffer, because the core must not own an I/O sink and
/// because the 155 `pr` call sites in the corpus are a debugging aid whose
/// destination is the embedder's business. A null sink makes `pr` a no-op,
/// which is the right behaviour in a conformance run.
class DebugSink {
 public:
  virtual ~DebugSink() = default;
  /// One `pr(...)`. Line breaks are the sink's business.
  virtual void write(std::string_view text) = 0;
  /// `ClearDebug()`.
  virtual void clear() {}
};

/// A way for a host function to get a `.vs` file compiled.
///
/// **The seam exists because compilation is not in the core.** `Scheduler`
/// looks a chunk up; it does not build one. `GameSession::Impl::chunk_for`
/// builds one, through the embedder's `ScriptResolver`, and until now nothing
/// below the session could reach it -- every entry point that spawns a script
/// (`AIRun`, `RunSequence`, `RunEconomyScript`) works only because something at
/// the session layer primed its file first, from a manifest.
///
/// `RunAIHelper` has no manifest. `gbr.exe` formats `data/ai helpers/%s.vs`
/// from a string the calling script computed -- 275 of the 285 sites pass a
/// literal, but five concatenate and five pass a variable -- and compiles it on
/// demand (0x006a2300 opens the file and returns the open error). Priming a
/// list of four file names would reproduce the shipped corpus and nothing else,
/// which is precisely the kind of hardcoding this project exists to avoid. So
/// the demand-driven half of the session is exposed here instead.
///
/// Null is a real case: a test that builds a scheduler by hand has no
/// installation behind it, and every caller must fall back to
/// `Scheduler::find_chunk_exact` rather than dereference.
class ScriptLibrary {
 public:
  virtual ~ScriptLibrary() = default;
  /// The chunk index for `path`, compiling it if this is the first ask.
  /// `script::kNoChunk` when the file does not exist or does not compile.
  virtual std::uint32_t chunk_for(std::string_view path) = 0;
};

/// Everything a host function needs that it cannot reach through the world.
struct HostContext {
  World* world = nullptr;

  /// The `TypeId` object handles carry. `sim/world_host.hpp` fixes it at
  /// `kTypeObj`; it is a field only so a test can prove nothing depends on the
  /// particular number.
  script::TypeId object_type = 1;

  /// The loaded `.loc.xml`, or null. Null makes every translation fall back to
  /// its source string, which is exactly what a missing language pack should
  /// do; see `game/localization.hpp`.
  const game::TranslationTable* translations = nullptr;

  /// Where `pr(...)` goes, or null for nowhere.
  DebugSink* debug = nullptr;

  /// The sixteen per-player selections, or null.
  ///
  /// Reachable from here because `CallContext::user` is the only thing a host
  /// function gets, and 18 declared entry points -- `GetSelection`,
  /// `ClearSelection`, the `SelAvg*` family -- read or write one. See
  /// `sim/orders.hpp`.
  SelectionTable* selections = nullptr;

  /// Whose screen this is.
  ///
  /// **Not world state, and deliberately not on `World`.** Every peer in a
  /// lockstep match simulates all sixteen players identically and differs only
  /// in whose selection is on screen and whose orders the mouse produces; a
  /// local-player id folded into the simulation would be a value that legally
  /// differs between peers, which is the one thing hashed state may not
  /// contain. It lives here for the same reason `translations` does.
  ///
  /// `kNoPlayer` for a headless run, where nobody is watching.
  PlayerId local_player = kNoPlayer;

  /// Where a host function gets a `.vs` file compiled, or null. See
  /// `ScriptLibrary` above for why this is here rather than at the session
  /// layer, and why null must degrade rather than fail.
  ScriptLibrary* library = nullptr;

  /// The `<cmd name>` whose tooltip is being built, or empty.
  ///
  /// The `rollover` family answers about *the command the player is pointing
  /// at* rather than about the object it is handed -- which is why one of its
  /// five forms takes no arguments at all. The original keeps the row's two
  /// display strings in a pair of `std::string` globals that the interface
  /// writes before it runs a `groupverifier`; this keeps the row's *name* and
  /// looks the strings up, which is the same information without a second copy
  /// of it. See `sim/feedback.hpp`.
  ///
  /// **Not world state, for `local_player`'s reason.** Which button the mouse
  /// is over legally differs between peers in a lockstep match, and hashed
  /// state may not contain a value that legally differs.
  std::string_view described_command;

  /// Where the camera is, whether it is pinned, and whether the mouse is live,
  /// or null for a run with no screen.
  ///
  /// `ViewPos()` reads back what `View()` wrote and three of Zama's sequences
  /// depend on it -- save the camera, take it somewhere for a cutscene, put it
  /// back. **Not world state**, and this is the clearest case of the rule
  /// `local_player` states: the camera is the one thing two peers of a lockstep
  /// match are *supposed* to disagree about. See `sim/feedback.hpp`.
  ViewState* view = nullptr;

  /// The sixteen players' numbered control groups, or null.
  ///
  /// Here rather than on the world for `selections`' reason, and next to it
  /// because they are the same kind of thing: `SetShortcutSel(1, 1, list)` is
  /// the scripted form of the player pressing ctrl-1.
  ShortcutTable* shortcuts = nullptr;

  /// The local command path, or null for a run with no player at the
  /// controls. A host function that the original has *post a command* rather
  /// than change state -- `SetSpeed` posts a `CVXCmdSetSpeed` through
  /// 0x0051d3c0 -- hands its order here, and the embedder decides when it
  /// runs: on an agreed turn in a networked match, on the next turn alone.
  ///
  /// **Not world state, for `local_player`'s reason**: only the peer whose
  /// player pressed the key posts anything. What the order *does* is world
  /// state, and it does it on every peer, at the turn the stream says.
  OrderOutbox* outbox = nullptr;
};

/// The world behind a call, or null.
///
/// Null is a real case, not a defensive check: `engine/tests` runs host
/// functions with `user == nullptr` on purpose to assert that every one of them
/// refuses rather than dereferences. Return `HostOutcome::failed` when this
/// gives null.
[[nodiscard]] inline World* world_of(script::CallContext& ctx) noexcept {
  auto* context = static_cast<HostContext*>(ctx.user);
  return context == nullptr ? nullptr : context->world;
}

/// The whole context, or null. Use when `object_type` is needed too.
[[nodiscard]] inline HostContext* host_context_of(script::CallContext& ctx) noexcept {
  return static_cast<HostContext*>(ctx.user);
}

}  // namespace imperivm::core::sim
