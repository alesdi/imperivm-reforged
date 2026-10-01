#pragma once

/// The AI bootstrap: what `AIStart` actually does, and the entry points the
/// `DATA/AI` script tree needs before any of it can run.
///
/// Nothing under `DATA/AI` ran at all before this file, and the reason was not
/// that the strategic layer was missing -- it was that the *ignition* was.
/// `MAIN.VS` is six `AIRun` calls and a return, and no code path anywhere
/// started it.
///
/// ## What `gbr.exe` says the bootstrap is
///
/// `AIStart` is registered three times, at 0x0043c415, 0x0043c42b and
/// 0x0043c441:
///
/// ```
///   AIStart   void, int player                                 -> 0x00434e20
///   AIStart   void, int player, str profile                    -> 0x00434e60
///   AIStart   void, int player, str profile, int difficulty    -> 0x00434f90
///   AIStop    void, int player                                 -> 0x00422720
/// ```
///
/// All three funnel into one core at `0x00434ca0`, and that routine is short
/// enough to read end to end. In order it:
///
///   1. **validates the player as 1..16 and converts to 0-based.** The three
///      wrappers each do this themselves before calling the core; the string
///      they print on failure is `Function "AIStart": Player number should be
///      between 1 and 16`.
///   2. **uppercases the profile name** (the `cmp al,0x61 / cmp al,0x7a /
///      sub eax,0x20` loop at 0x00434ef0 and again at 0x00435030) and looks it
///      up in a `std::map` of loaded profiles rooted at 0x008c8dec. A miss
///      prints `AIStart: invalid AI profile` **and does not start the AI**.
///   3. **increments the difficulty and validates 1..3** (`inc ebx` at
///      0x00434fe2, then `cmp ebx,1 / jl` and `cmp ebx,3 / jg`). So the
///      argument a caller passes is **0, 1 or 2**, and it selects
///      `[Vars.Easy]`, `[Vars.Normal]`, `[Vars.Hard]`. The 1-argument and
///      2-argument forms pass 0 for "no difficulty", which the core stores
///      only when non-zero -- so "leave whatever is there" is a real state.
///      This is new evidence against `docs/formats/ai-ini.md`'s open question,
///      and it is **not** the same number as `playerdata/@difficulty`: that
///      attribute reaches 3, which this argument cannot. Two spaces, one
///      offset apart, and `sim/ai_profile.hpp`'s `ai_difficulty_overlay` maps
///      the *attribute*. `ai_start_difficulty` below maps the *argument*.
///   4. **destroys any AI the player already has** before allocating the new
///      one -- the virtual destructor call at 0x00434cd9 is the same one
///      `AIStop` makes. Starting an AI twice is a restart, not an error.
///   5. calls `CVXAI::Start` (0x0041e2d0), and **the whole of that is**:
///      size two per-settlement `uint16` arrays (0x0041e390's loop, one entry
///      per settlement in each), and
///
///          call 0x0041cd00(this, "Main.vs", "void", -1)
///
///      -- run `Main.vs` with signature `void`. That is the entire ignition.
///      Every other AI script in the game is started by `Main.vs` or by
///      something `Main.vs` started.
///
/// The two per-settlement arrays are the economy-script and tactic-script
/// slots: `Settlement::EconomyScript` (0x00426190) reads
/// `settlement -> owner -> AI -> [0x6c][setIdx]`, and
/// `CVXAI::RunEconomyScript` (0x0041d310) writes that slot and then spawns the
/// named script. They are modelled here, per settlement, for the same reason.
///
/// ## `AIStart` is called by the campaign, and this header used to say it was
/// not
///
/// The claim here was *"zero call sites across all 577 `.vs` files"*, and it
/// was true of `data.pak` and false of the installation: the three-argument
/// form has **20 sites, every one inside a map container**, and it is the sole
/// blocker of ten scripts. `4_Great_Battles_Egypt` map 4 hands player 4 to the
/// AI when its ambush fires; `5_Great_Battles_Britain` map 3 does it twice; and
/// eleven of the twenty pass `GetDifficulty()` rather than a literal. The
/// inventory covers the containers now, so the name is declared and `ai.cpp`
/// attaches behaviour to it like every other domain.
///
/// The AI is *also* started by the engine, from `playerdata` -- `@AI` names the
/// profile and `@difficulty` the overlay -- which is what `ai_start_players`
/// is for and what every skirmish uses. The two are the same bootstrap
/// (`AiSystem::start`) reached two ways, and a mission that starts a player
/// already running restarts it, as the original does.
///
/// The one- and two-argument forms are still unbound: no script writes either,
/// so `declare_shipped_surface` has no entry for them to fill.
///
/// ## `AIGetPlayer`
///
/// `AIGetPlayer` (0x00421cb0) is `0x0043caa0() + 1`, and `0x0043caa0` reads the
/// *currently running script object* out of the VM context and asks it for its
/// owning player, returning -1 when there is no script at all. In the original
/// each `CVXAI` owns its own script list, so "which AI owns this coroutine" is
/// a field. Here there is one `script::Scheduler` for the whole world, so the
/// same question is answered from the spawn tree: `Scheduler::spawn` records a
/// `parent`, `AIRun` passes the calling script as the parent, and this file
/// keeps a script-id-to-player table seeded with each player's `Main.vs`.
///
/// **The root entry is kept after `Main.vs` dies, and that is load bearing.**
/// `Main.vs` spawns its six monitors and returns on the same tick, and four of
/// the six `Sleep` before their first `AIGetPlayer`, by which time the parent
/// record has been compacted out of the scheduler. Resolution therefore
/// consults this table *before* the scheduler at every step of the walk, and
/// the roots are never pruned.
///
/// A script that is under no AI answers **-1**, which is what the original
/// answers when there is no script context. All 13 corpus files that call
/// `AIGetPlayer` are under `DATA/AI`, so no shipped script ever observes the
/// difference between that and whatever the original would return for an
/// object behaviour script.
///
/// ## What is deliberately absent, and why
///
/// **`GAIKACount`, `LAIKA` and every `GAIKA::*` member stay unimplemented.**
/// `sim/gaika.hpp` says what a GAIKA *is* and refuses to say what one
/// *contains*, and nothing found here changes that. A `GAIKACount` that
/// returned a plausible number would be the exact failure this project's
/// unimplemented-entry discipline exists to prevent, and it would not even buy
/// a working opponent: `PRIORITIZE.VS` is
///
///     while (1) { for (i = 1; i < GAIKACount; i += 1) { ...; Sleep(...); } }
///
/// -- the only `Sleep` is *inside* the loop body, so a `GAIKACount` of 0 or 1
/// turns the outer `while` into a runaway that trips the instruction budget
/// every tick forever. A lie that also hangs.
///
/// So four of the six monitors -- `Prioritize`, `GAIKAMonitor`, `Recruiter`,
/// `SquadMonitor` -- trap on their first GAIKA call, by name, and say so. The
/// other two, `EconomyMonitor` and `TacticMonitor`, touch no GAIKA at all and
/// run: they are what makes the opponent build, research, trade and recruit.
///
/// `GetGAIKA(int)` **is** implemented, and it is not a guess: the registration
/// table gives `GetGAIKA` (arity 1) and `GAIKA::ID` the *same* function
/// pointer, 0x00746ea0, and that function is `xor eax,eax; ret` -- it does not
/// touch the VM stack at all. `GetGAIKA(i)` is the identity on the integer,
/// with no bounds check, exactly as `GAIKA::ID` is. That is a seventh
/// independent confirmation of `sim/gaika.hpp`'s model.
///
/// `GAIKA::ID` itself is *not* defined here even though it is the same
/// function, because `(member, "ID", 0)` is a key the object model also owns.
/// See `sim/host_setup.hpp` on what a second `define` does.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/ai_profile.hpp"
#include "imperivm/core/sim/gaika.hpp"
#include "imperivm/core/sim/lsa.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

/// The difficulty an `AIStart` *argument* selects.
///
/// The core increments before validating (`inc ebx` at 0x00434fe2), so 0, 1
/// and 2 are Easy, Normal and Hard and anything else is rejected with
/// `AIStart: invalid diffivulty` (the typo is the original's). Distinct from
/// `ai_difficulty_overlay`, which maps `playerdata/@difficulty`; see the
/// header note.
[[nodiscard]] constexpr AiDifficulty ai_start_difficulty(std::int32_t argument) noexcept {
  switch (argument) {
    case 0: return AiDifficulty::easy;
    case 1: return AiDifficulty::normal;
    case 2: return AiDifficulty::hard;
    default: return AiDifficulty::none;
  }
}

/// Why `ai_start` refused, or `ok`. Every one of these is a refusal the
/// original also makes, except `no_system` and `no_main_script`.
enum class AiStartStatus : std::uint8_t {
  ok,
  no_system,       ///< the world has no `AiSystem` registered
  bad_player,      ///< outside 1..16, as the original's own message says
  unknown_profile, ///< `AIStart: invalid AI profile`
  no_main_script,  ///< the chunk library holds no `Main.vs` for that profile
};

/// One player's view of one GAIKA node -- the original's **LAIKA** record.
///
/// The names and the order are the record's own serialiser's (0x0041cbf0),
/// which writes seven fields under these names: `GAIKA`, `flags`, `strat`,
/// `enemies`, `LastSeen`, `priority`, `Optimism`. The original's record is 24
/// bytes with the first three as `uint16`; the widths are widened here because
/// nothing in the corpus depends on them wrapping and this engine's ids are
/// `std::int32_t` everywhere else.
///
/// **`Optimism` starts at 100 and everything else at zero**, which the
/// constructor's fill loop writes explicitly (0x0041e294).
struct Laika {
  GaikaId gaika = kNoGaika;
  std::uint16_t flags = 0;
  /// The `GS_*` constant currently running in this node for this player, or 0
  /// (`GS_NONE`). `RunStrat` writes it; `StratRunning` reads it and clears it
  /// when the script it names has ended.
  std::int32_t strat = 0;
  /// The coroutine `RunStrat` spawned.
  ///
  /// **The original has no such field**, and this is the one place this record
  /// departs from its layout: it keeps the script in a parallel slot table on
  /// the AI object, at index `0x34 + gaika`, and asks the scheduler whether
  /// that slot is still alive. Same information, one indirection fewer, and it
  /// is exactly what `AiSettlementScripts` already does for the economy and
  /// tactic scripts.
  script::ScriptId strat_script = script::kNoScript;
  /// The enemy strength remembered from the moment this player last lost sight
  /// of the node. Written by the original's visibility sweep; `AiSystem::
  /// advance` runs only that sweep's `Explored` half, so nothing writes it.
  std::int32_t enemies = 0;
  /// Game time at which this player last saw the node, in milliseconds, which
  /// `AiSystem::advance` reads as the last turn the node was explored --
  /// `GETARMYNEED.VS` compares it against `GetTime()` with thresholds of ten
  /// and five minutes.
  GameTime last_seen = 0;
  std::int32_t priority = 0;
  std::int32_t optimism = kDefaultGaikaOptimism;
};

/// The bits of `Laika::flags`, each measured at the accessor that reads it.
inline constexpr std::uint16_t kLaikaPrioritized = 0x01;
inline constexpr std::uint16_t kLaikaExplored = 0x02;
inline constexpr std::uint16_t kLaikaRevealed = 0x04;
inline constexpr std::uint16_t kLaikaNoRecruit = 0x08;
inline constexpr std::uint16_t kLaikaNoAttack = 0x10;
inline constexpr std::uint16_t kLaikaControlFlag = 0x20;

/// One player's whole view: a slot per node, and the records those index.
///
/// ## Two parallel arrays, and the order of the second one is behaviour
///
/// `GAIKAMap` maps a node id to a slot; `LAIKAs` holds the records **in
/// priority-descending order**. `LAIKA(player, i)` is "the *i*-th node in this
/// player's ranked list", and the ranking is load-bearing rather than
/// incidental: `PRIORITIZE.VS` walks nodes by *id* and `GAIKAMONITOR.VS` walks
/// them by *rank*, so the AI deliberately thinks about its high-priority nodes
/// first.
///
/// `set_priority` is what maintains the order -- an insertion by adjacent
/// swaps, stable, repairing the map as it goes -- and it is the only thing that
/// may reorder the vector.
///
/// **Slot 0 is the reserved node and never moves.** Every player gets a record
/// for every node the moment `AIStart` runs, the map starts as the identity,
/// and node 0 is not a place: the shipped walks all start at `i = 1` for that
/// reason. An earlier reading had slot 0 meaning "this player has no record for
/// that node"; the constructor's fill loop settles that it does not.
class GaikaView {
 public:
  /// Give this player a record for nodes `0 .. nodes`, the identity
  /// permutation, and `Optimism` 100. `nodes` is the number of *real* nodes;
  /// the reserved one is added here.
  void reset(std::int32_t nodes);

  void clear() noexcept;

  /// The number of records, reserved node included -- which is `GAIKACount()`.
  [[nodiscard]] std::int32_t size() const noexcept {
    return static_cast<std::int32_t>(laika_.size());
  }
  [[nodiscard]] bool empty() const noexcept { return laika_.empty(); }

  /// The record for node `gaika`, or null. Node 0 is not a place and answers
  /// null, which is every accessor's `g == 0` guard.
  [[nodiscard]] const Laika* find(GaikaId gaika) const noexcept;
  [[nodiscard]] Laika* find(GaikaId gaika) noexcept;

  /// `LAIKA(player, index)`: the node in rank slot `index`, or `kNoGaika`.
  [[nodiscard]] GaikaId ranked(std::int32_t index) const noexcept;

  /// `SetPriority`. Writes the value, sets `Prioritized` **only if the value
  /// changed**, and re-sorts.
  void set_priority(GaikaId gaika, std::int32_t priority);

  /// The records in rank order, for the serialiser and for tests.
  [[nodiscard]] std::span<const Laika> records() const noexcept { return laika_; }
  [[nodiscard]] std::span<const std::int32_t> slots() const noexcept { return map_; }

  /// Rebuild from a saved pair. False if the two disagree -- the invariant is
  /// `laika[map[g]].gaika == g` for every node.
  [[nodiscard]] bool adopt(std::vector<std::int32_t> map, std::vector<Laika> records);

 private:
  void swap_slots(std::int32_t a, std::int32_t b) noexcept;

  std::vector<std::int32_t> map_;  ///< node id -> slot
  std::vector<Laika> laika_;       ///< rank order; [0] is the reserved node
};

/// One player's AI, which is what `AIStart` creates and `AIStop` destroys.
struct AiPlayer {
  PlayerId player = kNoPlayer;
  bool active = false;
  /// Uppercased, as the original stores it. Empty is the root `DATA/AI`
  /// profile, which is what 285 of the 304 shipped `playerdata` rows name.
  std::string profile;
  AiDifficulty difficulty = AiDifficulty::none;
  /// The `Main.vs` coroutine. Dead within a tick of being spawned -- see the
  /// header note on why the entry outlives it.
  script::ScriptId root = script::kNoScript;
  /// This player's view of the node graph, sized by `AIStart`.
  GaikaView gaika;
};

/// A settlement's two AI script slots.
///
/// The original holds these as two `uint16` arrays on the per-player AI object,
/// indexed by settlement index, sized by `CVXAI::Start`. Held here per
/// settlement id instead, because a settlement index is a position in a table
/// that can shrink and the id is not.
struct AiSettlementScripts {
  SettlementId settlement = kNoSettlement;
  /// The `ES_*` constant currently running, or 0 (`ES_NONE`).
  std::int32_t economy = 0;
  script::ScriptId economy_script = script::kNoScript;
  /// The `TS_*` constant currently running, or 0 (`TS_NONE`).
  std::int32_t tactic = 0;
  script::ScriptId tactic_script = script::kNoScript;
};

/// The per-player AI state, and the script-ownership table `AIGetPlayer` reads.
///
/// A `System` so that it is found the way every other domain's state is found
/// -- by name through `World::systems()` -- rather than by growing `World` a
/// field.
///
/// **It contributes nothing to the world hash, on purpose.** `aihash` is zero
/// in all nine desync dumps: the shipped build kept the AI out of its
/// determinism contract, and `sim/system.hpp` names that as a constraint to
/// respect rather than improve on. `hash` is overridden to an empty body so
/// that the omission is a statement rather than an oversight.
///
/// Every container here is ordered and every walk is an index walk. The
/// script-ownership table is sorted by script id, the per-settlement table by
/// settlement id, and the per-player table is fixed-size.
class AiSystem final : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "ai"; }

  /// Nothing. The AI *is* its scripts, and the scheduler runs those.
  void advance(World& world, const Turn& turn) override;

  /// Deliberately empty: `aihash` is zero in all nine dumps. See the class note.
  void hash(std::uint64_t& accumulator) const override { (void)accumulator; }

  // -- profiles ----------------------------------------------------------

  /// Register a loaded profile under the name `playerdata/@AI` gives it.
  ///
  /// `name` is uppercased on the way in, matching what `AIStart` does to its
  /// argument before the lookup. The empty name is the root `DATA/AI` profile.
  ///
  /// `script_prefix` is the directory the profile's own `.vs` files were loaded
  /// under -- `DATA\AI\DEFENSIVE\` for the one overlay that ships any -- and is
  /// what makes `Main.vs` mean the *profile's* `Main.vs`. `Scheduler::find_chunk`
  /// documents that ambiguity and says it belongs to whoever wires the AI up;
  /// this is that.
  void add_profile(std::string_view name, const AiProfile* profile,
                   std::string_view script_prefix = {});

  [[nodiscard]] const AiProfile* profile_for(std::string_view name) const noexcept;
  [[nodiscard]] bool has_profile(std::string_view name) const noexcept;

  // -- lifetime ----------------------------------------------------------

  /// `AIStart`. `player` is 0-based here; the 1..16 the original validates is
  /// the *script*'s numbering, which `player_arg` strips everywhere else.
  ///
  /// Restarts an AI the player already has, as the original does.
  AiStartStatus start(PlayerId player, std::string_view profile, AiDifficulty difficulty,
                      script::Scheduler& scheduler);

  /// `AIStop`. Kills the player's `Main.vs` and every script under it that is
  /// still alive, and forgets the player's settlement slots. False if the
  /// player had no AI.
  bool stop(PlayerId player, script::Scheduler& scheduler);

  /// Give `player` a record for every node. `AIStart`'s last act, and the one
  /// this system cannot do inside `start` because the node count is the
  /// world's rather than the scheduler's.
  ///
  /// **Every player gets a record for every node**, the map starting as the
  /// identity -- which is the original's constructor and not an optimisation.
  void seed_gaika_view(PlayerId player, std::int32_t nodes);

  [[nodiscard]] GaikaView* gaika_view(PlayerId player) noexcept;
  [[nodiscard]] const GaikaView* gaika_view(PlayerId player) const noexcept;

  [[nodiscard]] const AiPlayer* player_ai(PlayerId player) const noexcept;
  [[nodiscard]] std::span<const AiPlayer> players() const noexcept { return players_; }
  [[nodiscard]] std::size_t active_count() const noexcept;

  // -- script ownership --------------------------------------------------

  /// `AIGetPlayer`'s answer: the **1-based** player owning `script`, or -1.
  ///
  /// Walks the spawn tree upward, consulting this table before the scheduler at
  /// every step, and caches what it learns. Not `const` for that reason.
  [[nodiscard]] std::int32_t script_player(script::ScriptId script,
                                           const script::Scheduler& scheduler);

  /// Record that `script` belongs to `player`. Used by the bootstrap and by
  /// the synchronous-call helper; a test may use it to plant a root.
  void adopt(script::ScriptId script, PlayerId player, bool root = false);
  void forget(script::ScriptId script);

  [[nodiscard]] std::size_t owner_entries() const noexcept { return owners_.size(); }

  // -- per-settlement script slots ---------------------------------------

  /// `Settlement::EconomyScript` / `Settlement::TacticScript`.
  ///
  /// Reports 0 (`ES_NONE` / `TS_NONE`) once the recorded coroutine is gone.
  /// The original clears the slot from a script-completion hook; deriving it
  /// from liveness instead is the same observable with one less thing to keep
  /// in step, and it means a script that trapped does not wedge its settlement
  /// forever.
  [[nodiscard]] std::int32_t economy_script(SettlementId settlement,
                                            const script::Scheduler& scheduler) const;
  [[nodiscard]] std::int32_t tactic_script(SettlementId settlement,
                                           const script::Scheduler& scheduler) const;
  void set_economy_script(SettlementId settlement, std::int32_t id, script::ScriptId script);
  void set_tactic_script(SettlementId settlement, std::int32_t id, script::ScriptId script);
  [[nodiscard]] std::span<const AiSettlementScripts> settlement_scripts() const noexcept {
    return settlements_;
  }

  // -- the AI helpers -----------------------------------------------------
  //
  // `RunAIHelper` (~285 sites), `IsAIHelperRunning` (236) and `StopAIHelper`
  // (179) -- the largest single cluster in the installation, and all 700 sites
  // are inside the map containers.
  //
  // **One table, keyed by a name the caller invents.** `gbr.exe` keeps a
  // `std::map<std::string, int>` on the `CVXAIHelper` singleton at
  // `[0x00996888] + 0x24`, which its own serializer names `"idmap"`
  // (0x004d42ad), and the mapped value is a script id -- not a pointer, not a
  // struct. That key is the *first* argument of `RunAIHelper`; the second is
  // the helper file. Nothing else ever sees the file again, so two helpers of
  // the same file under different names are two independent instances and one
  // name is one slot.
  //
  // **Not hashed, on purpose.** `AiSystem::hash` is a no-op because `aihash`
  // is zero in all nine desync dumps, and the standing decision is to respect
  // that rather than improve on it. This table is AI state by the strictest
  // reading -- it lives on the AI singleton in the original -- so it rides in
  // the same exclusion. It is serialised, because a save that dropped it would
  // resume with every helper unreachable by name.

  /// One live helper: the caller's key and the coroutine running it.
  struct AiHelperEntry {
    std::string name;
    script::ScriptId script = script::kNoScript;
  };

  /// The id `RunAIHelper` recorded under `name`, or `script::kNoScript`.
  ///
  /// Case-sensitive, as the original's `std::map<std::string, int>` is.
  [[nodiscard]] script::ScriptId helper(std::string_view name) const noexcept;

  /// `idmap[name] = id`, insert or overwrite.
  ///
  /// **Overwrite, with no branch, and that is the finding.** `gbr.exe`
  /// 0x004d3251 calls `std::map::operator[]` and stores into what it returns
  /// (0x004d3268) with no test of what was there. So starting a helper under a
  /// live name starts a *second* coroutine and forgets the first: it keeps
  /// running, and `StopAIHelper` and `IsAIHelperRunning` can only ever see the
  /// newer one. That is reproduced rather than tidied.
  void set_helper(std::string_view name, script::ScriptId script);

  /// Drop `name`'s entry. False when there was none.
  bool forget_helper(std::string_view name);

  /// Drop every entry whose coroutine has ended.
  ///
  /// The original does this from the script-completion hook at vftable slot
  /// +0x1c (0x004d2160), which walks the map for a node whose value equals the
  /// ended id and erases it. Polled once a turn here, for the reason
  /// `CampaignSystem::reap_sequences` is: the scheduler's one teardown hook is
  /// already taken by the `ObjList` pool, and a poll from `GameSession::advance`
  /// is as deterministic as the pass that triggers it.
  void reap_helpers(const script::Scheduler& scheduler);

  /// Every live helper, in ascending name order -- `std::map`'s order, so that
  /// iteration is reproducible and a save round-trips byte for byte.
  [[nodiscard]] std::span<const AiHelperEntry> helpers() const noexcept { return helpers_; }

  // -- script lookup -----------------------------------------------------

  /// The chunk index of `file` for `player`'s profile, or `script::kNoChunk`.
  ///
  /// Exact match against the profile's own directory first, then the
  /// scheduler's own name resolution. That ordering is the per-player search
  /// path; without it `Main.vs` is whichever of the four copies loaded first.
  [[nodiscard]] std::uint32_t find_script(const script::Scheduler& scheduler, PlayerId player,
                                          std::string_view file) const;

  /// The `.vs` file a constant of `family` names, e.g. `ES_Village` ->
  /// `ES_Village.vs`. Empty when the profile does not declare that value.
  ///
  /// Not a guess: every `ES_*`, `GS_*` and `TS_*` name in `[EconomyScripts]`,
  /// `[GAIKAStrat]` and `[TacticScripts]` has a `<name>.vs` line in
  /// `[Scripts]`, and `CVXAI::RunEconomyScript` (0x0041d310) resolves the id
  /// through the same enum table before spawning.
  [[nodiscard]] std::string script_file_for(PlayerId player, AiEnum family,
                                            std::int32_t value) const;

  // -- the saved game ----------------------------------------------------
  //
  // Layout and rationale: docs/formats/save.md. The definitions live together
  // in `src/sim/save_systems.cpp` rather than in this domain's own `.cpp`, so
  // that the ten systems' state vectors are one file to audit: adding a field
  // here and forgetting its section shows up as a diff that does not touch the
  // one place every section is written.

  /// Append this system's state to `out`, as one self-describing section with
  /// its own magic and version -- the shape `Scheduler::serialize` established.
  ///
  /// **Written:** the sixteen per-player rows, the script-owner table, the
  /// per-settlement economy/tactic bindings and the prune counter.
  ///
  /// **The other section whose absence is silent.** `AiSystem::hash` is a
  /// no-op, so a load that dropped this passes `sim::verify_hashes` and then
  /// diverges once `AIGetPlayer` starts answering -1 for scripts that used to
  /// have an owner.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** the profile table. Its entries hold borrowed
  /// `const AiProfile*`, which is game data and an address; both are rebuilt by
  /// whoever built this system, from `AI.INI` and the per-player
  /// `playerdata/@AI`.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

  // -- ship needs --------------------------------------------------------
  //
  // `ShipNeeds(player, lsa)`, `IncShipNeeds`, `ClrShipNeeds` and `Ships` --
  // the AI's transport bookkeeping, written by `RECRUITER.VS` and read by
  // `ES_SHIPYARD.VS`. The original keeps them on the area table: each LSA has
  // a 192-byte record at `[0x008c8e54]+0x24`, and at `+0x10` sits an `int`
  // per player that 0x0042d9f0 increments, 0x0042d9b0 zeroes and 0x0043ef00
  // reads. That table is rebuilt from the map here (`sim/lsa.hpp`), so the
  // counters live on this system instead, keyed by `(player, lsa)` in a
  // sorted vector, and ride in the save with the rest of it.
  //
  // `Ships(player, lsa)` reads a second per-player `int` at `+0x54` of the
  // same record, a count of the player's ships in the area that the engine
  // maintains as ships move. This engine answers it with a census over the
  // world instead -- every living ship the player owns whose position falls
  // in the area -- which is the same number whenever the maintained count is
  // right, and is stated as computed rather than kept.

  /// One counter. `player` is a table index, 0..15.
  struct ShipNeed {
    PlayerId player = kNoPlayer;
    LsaId lsa = kNoLsa;
    std::int32_t count = 0;
  };
  /// The counter, or 0 for a pair nothing has touched.
  [[nodiscard]] std::int32_t ship_needs(PlayerId player, LsaId lsa) const noexcept;
  /// `IncShipNeeds`: one more. Mints the row on first use.
  void add_ship_need(PlayerId player, LsaId lsa);
  /// `ClrShipNeeds`: back to zero. A row nothing minted stays unminted.
  void clear_ship_needs(PlayerId player, LsaId lsa) noexcept;
  /// Every row, ascending by `(player, lsa)`.
  [[nodiscard]] std::span<const ShipNeed> ship_need_rows() const noexcept { return ship_needs_; }
  /// Adopt rows from a save. Refuses rows out of order.
  bool adopt_ship_needs(std::vector<ShipNeed> rows);

  // -- the ship transport order ------------------------------------------
  //
  // **A `CVXShip` carries a pending transport order**: a `std::string` at
  // `[ship+0x20c]` and a destination point at `[ship+0x228]`/`[+0x22c]`,
  // constructed empty and `(-1, -1)`. Four entry points read or write it --
  // `Ship::HasAiTransport`, `ClearAiTransport`, `GetTransPt` and
  // `ApplyAiTransport` -- and its one writer is the free
  // `PrepareAiTransportShip/5`, which is why `sim/world_host.cpp` carried all
  // four as constants for as long as that name had no body and left a note
  // saying they expired together. This is that expiry.
  //
  // **It lives here rather than on the object, and is saved rather than
  // hashed.** The original keeps it on the ship, but the dumps print no such
  // field and `aihash` is zero in all nine of them; folding it into
  // `World::state_hash` would pull a subsystem into the determinism contract
  // that the shipped build left out of it, which is the argument
  // `sim/squad.hpp` already makes for a squad's AI fields. It is AI state by
  // every other measure too: only the AI path writes it, and only a ship's own
  // behaviour script reads it.
  struct ShipTransport {
    ObjectId ship = kNoObject;
    /// The order verb the caller handed `PrepareAiTransportShip`. Empty is the
    /// constructed state, and `HasAiTransport` is exactly "not empty".
    std::string order;
    /// `(-1, -1)` when there is none, which is also `kHeldPosition` -- two
    /// meanings of one sentinel, and nothing here confuses them because a real
    /// transport point is a place on the map.
    Point where{-1, -1};
  };
  /// `ship`'s order, or a row that reads as the constructed state.
  [[nodiscard]] const ShipTransport& ship_transport(ObjectId ship) const noexcept;
  /// `PrepareAiTransportShip`: the ship is now carrying this crossing.
  void set_ship_transport(ObjectId ship, std::string_view order, Point where);
  /// `Ship::ClearAiTransport`, and `ApplyAiTransport`'s last act. The row is
  /// dropped rather than blanked, which is the same thing to every reader.
  void clear_ship_transport(ObjectId ship) noexcept;
  /// Ascending by ship id, so a save round-trips byte for byte.
  [[nodiscard]] std::span<const ShipTransport> ship_transports() const noexcept {
    return ship_transports_;
  }
  /// **For the save reader only.** Refuses rows that are not strictly ascending.
  bool adopt_ship_transports(std::vector<ShipTransport> rows);

  /// Whether the AI manager exists -- `[0x8c8e58]` in `gbr.exe`, written 1 by
  /// the match-start bootstrap (0x00450ad2) before it builds the node table
  /// and starts the computer players, and never cleared for the match.
  ///
  /// **It gates squad formation, not only the AI.** 0x0041e820, the hook the
  /// object manager runs when a unit enters the world (0x0052dc03 on a spawn,
  /// 0x0052e491 on a placement), returns at once when the manager does not
  /// exist and otherwise puts the unit into a squad through the rule
  /// `sim/squad.cpp` calls `add_to_squad` -- which is how every deer in the
  /// reference dumps prints `squad=<n>(14)` and why `MilEval` is a sum over
  /// squads that were never formed here. `ai_start_players` raises it, and
  /// `HeroSystem::advance` reads it. Saved: a loaded match keeps forming.
  [[nodiscard]] bool manager_started() const noexcept { return manager_started_; }
  void start_manager() noexcept { manager_started_ = true; }

 private:
  /// Ascending by name. A sorted vector rather than a `std::map` because
  /// `sim/system.hpp` requires every iterated container to have an order the
  /// simulation defines; `std::map` would give the same order and a worse
  /// cache profile for a table this size.
  std::vector<AiHelperEntry> helpers_;
  /// Ascending by `(player, lsa)`; see the ship-needs note above.
  std::vector<ShipNeed> ship_needs_;
  /// Ascending by ship id; see the transport-order note above.
  std::vector<ShipTransport> ship_transports_;

  struct ProfileEntry {
    std::string name;  ///< uppercased
    const AiProfile* profile = nullptr;
    std::string script_prefix;
  };
  struct ScriptOwner {
    script::ScriptId script = script::kNoScript;
    PlayerId player = kNoPlayer;
    bool root = false;
  };

  [[nodiscard]] const ProfileEntry* profile_entry(std::string_view name) const noexcept;
  [[nodiscard]] const ProfileEntry* profile_of_player(PlayerId player) const noexcept;
  [[nodiscard]] const ScriptOwner* owner(script::ScriptId script) const noexcept;
  [[nodiscard]] AiSettlementScripts* slot(SettlementId settlement);
  [[nodiscard]] const AiSettlementScripts* slot(SettlementId settlement) const;
  void prune_owners(const script::Scheduler& scheduler);

  std::vector<ProfileEntry> profiles_;              ///< sorted by name
  std::vector<AiPlayer> players_ = std::vector<AiPlayer>(kPlayerCount);
  std::vector<ScriptOwner> owners_;                 ///< sorted by script id
  std::vector<AiSettlementScripts> settlements_;    ///< sorted by settlement id
  /// Resolutions since the last prune. Prunes on a count rather than on a
  /// clock so that two runs prune at the same points.
  std::uint32_t since_prune_ = 0;
  bool manager_started_ = false;
};

/// The world's `AiSystem`, or null. Found by name, like every other domain's.
[[nodiscard]] AiSystem* ai_system_of(World& world) noexcept;

/// Run a compiled script to completion, right now, from inside a host body, and
/// hand back its `return`. The synchronous trampoline behind
/// `Settlement::GetEconomyScript`, `GAIKA::MinNeed`, `Hero::TSAdvHeroSkills`
/// and `Squad::CalcGoAround`; `sim/ai.cpp` carries the full note on what it
/// refuses (a callee that suspends, a callee that traps) and why the pooled
/// state it releases afterwards matters. Fails with `what` when `chunk_index`
/// names no chunk.
[[nodiscard]] script::HostOutcome run_script_now(script::CallContext& ctx,
                                                 std::uint32_t chunk_index,
                                                 std::span<const script::Value> args,
                                                 const char* what, script::Value& result,
                                                 std::vector<script::Value>* out_locals = nullptr);

/// `CheckLsaPath`'s answer, and the route behind it.
struct LsaRoute {
  /// What `CheckLsaPath` returns: 0 for no route, otherwise hops + 1.
  std::int32_t answer = 0;
  /// `src` first, `dst` last, when `answer` is non-zero.
  std::vector<LsaId> path;
};

/// 0x00441b70 with an empty transport string and no rendezvous: the
/// breadth-first search over the area graph that `CheckLsaPath`,
/// `PrepareAiTransportShip` and `Squad::NearestHospital` all run. `player` is
/// 0-based. **It has a side effect**: an unreachable `dst` bumps the player's
/// ship need on the first shipless sea the walk saw, as the original does.
/// `sim/ai.cpp` carries the whole reading.
[[nodiscard]] LsaRoute lsa_route(World& world, AiSystem* ai, LsaId src, LsaId dst,
                                 PlayerId player);

/// The chunk `file` names on `player`'s AI search path -- the profile's own copy
/// when it has one, the root `DATA/AI` copy otherwise -- or `script::kNoChunk`.
/// `kNoPlayer`, or a world with no AI, resolves against the root alone.
[[nodiscard]] std::uint32_t ai_script_chunk(script::CallContext& ctx, PlayerId player,
                                            std::string_view file);

/// `AIStart` for one player, through the world's `AiSystem`.
AiStartStatus ai_start(World& world, PlayerId player, std::string_view profile,
                       AiDifficulty difficulty, script::Scheduler& scheduler);

/// `AIStop` for one player.
bool ai_stop(World& world, PlayerId player, script::Scheduler& scheduler);

/// Start an AI for every `Computer`-controlled player that has none.
///
/// Profile from `playerdata/@AI`, difficulty from `playerdata/@difficulty`
/// through `ai_difficulty_overlay`. `Both` slots are **not** started: the
/// header for `PlayerControl` says the choice is made at match setup, and
/// making it here would be making it silently.
///
/// Returns the number started. `refused` receives one status per player that
/// was a candidate and was refused, so a caller can report *which* rather than
/// only *how many*.
std::size_t ai_start_players(World& world, script::Scheduler& scheduler,
                             std::vector<std::pair<PlayerId, AiStartStatus>>* refused = nullptr);

/// Define this domain's entry points. Returns how many were newly implemented.
///
/// Defines, in `sim/host_setup.hpp`'s terms:
///
///   free    `AIGetPlayer/0`, `AIStart/3`, `AIStop/1`, `GetGAIKA/1`, and the
///           nine `SPF*` names of the pathfinder bootstrap
///   member  `EconomyScript/0`, `GetEconomyScript/1`, `RunEconomyScript/1`,
///           `TacticScript/0`,  `GetTacticScript/1`,  `RunTacticScript/1`
///
/// and nothing else. In particular it does **not** define `GAIKACount`,
/// `LAIKA`, any `GAIKA::*` member, or `AIStart`'s one- and two-argument
/// forms; see the header note for each. `AIStart/3` **is** defined -- see
/// the note above, and the claim it corrects -- and so is `AIStop/1`, which
/// is `stop` through the calling script's scheduler.
std::size_t register_ai_host(script::HostRegistry& registry);

}  // namespace imperivm::core::sim
