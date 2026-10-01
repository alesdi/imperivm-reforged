#pragma once

/// The determinism harness: the thing that checks the property this whole
/// architecture is bent around.
///
/// The freestanding core, the no-floating-point rule, the ordered containers,
/// the single seeded RNG and the CI boundary checker all exist to buy one
/// property -- **two peers running the same inputs produce the same state** --
/// and until this header nothing tested it end to end. Each domain tested its
/// own hash; nobody ran the pipeline twice and compared.
///
/// ## The three checks, and why each one is separate
///
///   1. **Self-consistency.** One scenario, one seed, one schedule, run twice.
///      The per-turn hash sequences must be byte-identical. This is the cheap
///      check and it catches the whole class of bug where something reads
///      memory the run does not own: an address in a hash, an iteration over an
///      unordered container, an uninitialised field. It reports the *first*
///      turn and the *channel*, because "they differ" is not a bug report.
///
///   2. **Partition invariance.** The same total game time, delivered as
///      different turn-length sequences, must end in the same state.
///      `docs/engine/tick.md` measures 200, 400, 799 and 800 all occurring in
///      the retail dumps, with one session renegotiating mid-game -- so a
///      system that accidentally depends on the *number* of turns rather than
///      the *elapsed time* is not a theoretical bug, it is a desync the first
///      time two peers agree on a different length. `test_combat.cpp` asserts a
///      smaller version of this over one system; this generalises it to a whole
///      pipeline.
///
///   3. **An oracle.** Compare a recording against a reference. Today the
///      reference is a golden trace file committed to the repository, so that a
///      change altering simulation results has to say so out loud in a diff.
///      Tomorrow it is a stream recorded from the original engine. See
///      "Attaching a recorded command stream" below: it is the same code path.
///
/// Plus a fourth, which costs nothing and guards a rule that is easy to break
/// by accident: **the four reserved channels must stay zero.**
/// `docs/engine/state-vector.md` measures `pathfinder`, `exploration`,
/// `scriptstate` and `aihash` as zero in all nine desync dumps -- the shipped
/// build deliberately kept pathfinding, fog of war, script state and the AI out
/// of the determinism contract. A harness that hashed them would report
/// divergence the original tolerated, so `check_reserved_channels` fails a run
/// that makes one of them non-zero rather than quietly comparing it.
///
/// ## What is not here: a fixed timestep
///
/// Every entry point below takes a `Schedule` -- an explicit sequence of turn
/// lengths -- and there is no overload that takes a turn count and a constant.
/// That is deliberate. The original renegotiates the turn length against
/// measured latency and scales it by `gamespeed`; a harness with a fixed `dt`
/// would be testing a game nobody shipped.
///
/// ## The seam
///
/// `Scenario` builds a `Run` from a seed; `Run` advances one declared turn and
/// exposes the world. Everything else in this header is written against those
/// two and knows nothing about sessions, maps or scripts. That is what lets the
/// harness exist before `GameSession` does, and what lets the same comparison
/// code serve three different oracles.
///
/// ## Attaching a `GameSession`
///
/// `SessionRun` below is the adapter, and it is a template for one reason: a
/// template is not instantiated until it is used, so `engine/core` does not
/// acquire a link-time dependency on `session.cpp` merely by containing this
/// header. Once a session exists, a scenario is:
///
/// ```cpp
/// class MapScenario final : public conformance::Scenario {
///  public:
///   std::string_view name() const noexcept override { return name_; }
///   std::unique_ptr<conformance::Run> start(std::uint32_t seed) const override {
///     auto session = GameSession::create(registry_, inputs_, seed);
///     if (!session) return nullptr;
///     auto run = std::make_unique<conformance::SessionRun<>>(std::move(session).value());
///     run->session().start_object_scripts();
///     return run;
///   }
/// };
/// ```
///
/// and nothing else changes. `start()` must be a **pure function of the seed**:
/// the harness calls it repeatedly and compares the results, so a scenario that
/// remembers anything between calls makes the harness accuse the simulation of
/// its own bug.
///
/// ## Attaching a recorded command stream
///
/// This is the case the design is aimed at, and it decomposes into exactly the
/// two halves this header already has.
///
///   * The **drive** is a `Scenario`. A recorded stream contains the map, the
///     seed, and the commands each peer issued on each turn. A
///     `RecordedScenario` builds the session from the recorded inputs and
///     returns a `Run` whose `advance()` injects that turn's commands into the
///     command system before calling `GameSession::advance(1, length)`. The
///     recorded turn lengths become the `Schedule`; nothing in this header has
///     to learn what a command is.
///
///   * The **oracle** is an `Oracle`. A desync dump's `[HASHES]` block, or a
///     per-turn hash log, becomes a `Trace` and then a `TraceOracle`, and
///     `check_against_oracle` is the function that already exists.
///     `Oracle::covers` is what makes this work against a *partial* recording:
///     our `threads` and `netcmds` channels stay zero until the scheduler and
///     the command pump fold themselves in, so an oracle that knows the
///     original's values for those declares them covered and an oracle that
///     does not declares them not, and neither one has to lie.
///
/// Note what is deliberately *not* offered: the nine `Logs/*/desync.txt` files
/// are **not** replayable command streams. Each is one divergence event plus a
/// world snapshot, with no command payloads at all. They specify the state
/// vector; they are not an oracle for a replay, and building an API that
/// implied otherwise would be building on a misreading.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim::conformance {

// --------------------------------------------------------------------------
// hash channels
// --------------------------------------------------------------------------

/// The `[HASHES]` block's fields, as an enumerable thing.
///
/// `WorldHashes` is a struct because that is how it diffs against a dump; this
/// is the same nine values as an index, so that a comparison can loop and name
/// the channel that differed instead of writing nine `if`s and getting the
/// eighth one wrong.
///
/// The order is the scan order, and it is chosen rather than alphabetical:
/// `slots` first because it is the channel that is non-zero and distinct in all
/// nine dumps and therefore the one that almost always carries the real answer,
/// and `hash_of_hashes` last because it is derived -- it differs whenever any
/// of the four above it does, so reporting it first would bury the cause under
/// its own symptom.
enum class Channel : std::uint8_t {
  slots = 0,
  threads,
  netcmds,
  extrahash,
  pathfinder,
  exploration,
  scriptstate,
  aihash,
  hash_of_hashes,
  count,
};

inline constexpr std::size_t kChannelCount = static_cast<std::size_t>(Channel::count);

/// The channel's name exactly as the dumps spell it, so a report can be grepped
/// against a real `desync.txt`.
[[nodiscard]] std::string_view channel_name(Channel channel) noexcept;
[[nodiscard]] std::uint64_t channel_value(const WorldHashes& hashes, Channel channel) noexcept;

/// One of the four the shipped build left out of the determinism contract.
///
/// Zero in all nine dumps (`docs/engine/state-vector.md`). Not a channel we
/// have failed to implement: a channel we must not implement.
[[nodiscard]] constexpr bool channel_is_reserved_zero(Channel channel) noexcept {
  return channel == Channel::pathfinder || channel == Channel::exploration ||
         channel == Channel::scriptstate || channel == Channel::aihash;
}

// --------------------------------------------------------------------------
// schedules
// --------------------------------------------------------------------------

/// A sequence of declared turn lengths, in game-time units.
///
/// The harness never takes a turn count and a constant. See the file header.
using Schedule = std::span<const std::int32_t>;

/// Total game time a schedule delivers. 64-bit: a long partition test runs
/// millions of units and an overflow would be a silent pass.
[[nodiscard]] GameTime schedule_time(Schedule schedule) noexcept;

/// `count` turns of `length`.
[[nodiscard]] std::vector<std::int32_t> uniform_schedule(std::int32_t length, std::size_t count);

/// Different cuts of the same interval, from the lengths the dumps actually
/// record.
///
/// Returns every uniform partition of `total` into 800s, 400s and 200s that
/// comes out exact, plus one deliberately ragged sequence that mixes them --
/// because a uniform partition can hide a bug that only shows when the length
/// *changes* mid-run, which is the case the dumps show a real session doing.
/// The 799-unit turn from the one off-speed dump is not used to partition,
/// since 799 divides nothing evenly; a scenario that wants it puts it in the
/// ragged sequence by hand.
///
/// Empty if `total` is not divisible by 800.
[[nodiscard]] std::vector<std::vector<std::int32_t>> canonical_partitions(GameTime total);

// --------------------------------------------------------------------------
// the seam
// --------------------------------------------------------------------------

/// One instance of a scenario, mid-flight.
///
/// Only two members are virtual, and that is the whole point: a `Run` says
/// which world it drives and how to push it forward one turn. Whether that is
/// `World::advance` alone, or a session that also pumps a scheduler, or a
/// replay that injects recorded commands first, is the implementation's
/// business and none of the harness's.
class Run {
 public:
  virtual ~Run() = default;

  /// The world being driven. Must return the same object every call.
  [[nodiscard]] virtual World& world() noexcept = 0;

  /// Advance exactly one lockstep turn of the declared length.
  ///
  /// One turn rather than a count, so that a replay can interleave per-turn
  /// input and so that the harness can sample the hash after every turn without
  /// the implementation having to offer a callback.
  virtual void advance(std::int32_t turn_length) = 0;

  // -- derived; not virtual, so every Run agrees on what these mean ------

  [[nodiscard]] const World& world() const noexcept {
    return const_cast<Run*>(this)->world();
  }
  [[nodiscard]] WorldHashes hashes() const noexcept { return world().hashes(); }
  [[nodiscard]] GameTime time() const noexcept { return world().time(); }
  [[nodiscard]] std::uint64_t turns() const noexcept { return world().turns(); }

  /// Erase the two parts of the clock that a partition legitimately changes.
  ///
  /// `World::state_hash()` folds in the turn count and the current turn length,
  /// and it is right to: two peers that disagree about either are running
  /// different simulations. But they are exactly what differs between `[800,
  /// 800]` and `[200 x 8]` *when nothing is wrong*, so comparing raw hashes
  /// across a partition would fail on every scenario including a correct one.
  ///
  /// So the partition check calls this on both runs once they have finished and
  /// then compares `World::state_hash()` as it stands. The harness deliberately
  /// does **not** grow its own copy of the world's hashing to do this: a
  /// second implementation of `state_hash` would drift from the first, and the
  /// day it did the harness would be checking a projection of the state vector
  /// that nothing else in the engine uses. Elapsed game time -- which *is*
  /// partition-invariant -- is captured before this call and compared
  /// separately.
  void freeze_clock() noexcept;
};

/// A world plus a fixed set of systems, buildable from a seed.
///
/// **`start()` must be a pure function of `seed`.** The harness calls it two or
/// more times and compares what comes out; a scenario that carries state
/// between calls is a scenario that makes the harness lie.
class Scenario {
 public:
  virtual ~Scenario() = default;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual std::unique_ptr<Run> start(std::uint32_t seed) const = 0;

  /// One peer's run: `start(seed)` as seen from `local`'s seat -- the one
  /// input two peers of a match are guaranteed to disagree about, and the one
  /// that must never reach hashed state. `check_netplay` starts each peer
  /// with its own seat so that a leak shows up as a divergence rather than
  /// hiding behind peers that all watch from nowhere; the unnetworked replay
  /// it compares them with still calls `start`. A scenario with no notion of
  /// a screen keeps this default.
  [[nodiscard]] virtual std::unique_ptr<Run> start_as(std::uint32_t seed, PlayerId local) const {
    (void)local;
    return start(seed);
  }
};

/// A `Run` over a bare `World` the caller owns and populates.
///
/// The simplest possible implementation of the seam, and what a synthetic
/// scenario uses. `advance` is `World::advance`, which runs the object pass and
/// then every registered system in registration order.
class WorldRun : public Run {
 public:
  explicit WorldRun(World& world) noexcept : world_(&world) {}
  [[nodiscard]] World& world() noexcept override { return *world_; }
  void advance(std::int32_t turn_length) override { world_->advance(turn_length); }

 private:
  World* world_;
};

/// A `Run` over a `GameSession`.
///
/// A template on the session type purely so that it is not instantiated -- and
/// therefore `engine/core` does not link against `session.cpp` -- until
/// somebody actually uses it. The default argument is the real type, so callers
/// write `SessionRun<>` and nobody has to know why.
///
/// `Session` must offer `World& world()` and
/// `void advance(std::uint64_t turns, std::int32_t turn_length)`, which is the
/// contract in `sim/session.hpp`.
template <class Session = GameSession>
class SessionRun final : public Run {
 public:
  explicit SessionRun(std::unique_ptr<Session> session) noexcept
      : session_(std::move(session)) {}

  [[nodiscard]] World& world() noexcept override { return session_->world(); }
  void advance(std::int32_t turn_length) override { session_->advance(1, turn_length); }

  [[nodiscard]] Session& session() noexcept { return *session_; }

 private:
  std::unique_ptr<Session> session_;
};

// --------------------------------------------------------------------------
// traces
// --------------------------------------------------------------------------

/// The state of a run at the end of one turn.
struct TraceEntry {
  std::uint64_t turn = 0;   ///< `Clock::turns()` after the turn ran
  std::int32_t length = 0;  ///< the length this turn was declared at
  GameTime time = 0;        ///< accumulated game time after the turn
  WorldHashes hashes;
};

/// A recording of a run, turn by turn.
///
/// Small enough to commit: a hundred-turn scenario writes about six kilobytes,
/// less when the state is quiet, because `write_trace` elides a channel that
/// repeats the previous line.
struct Trace {
  std::string scenario;
  std::uint32_t seed = 0;

  /// The system run order, in registration order.
  ///
  /// **Recorded because it is an input to every hash below it.** `World`
  /// folds each system's index and name into `slots` before calling its
  /// `hash`, so adding a system -- or moving one -- changes every hash in the
  /// trace from turn one. Without this line a golden file that met a new
  /// system would report "channel `slots` first differs at turn 1" and send a
  /// reader looking for a simulation bug that is not there. With it the
  /// harness says the pipeline changed and names the system, which is a
  /// different sentence and a much shorter afternoon.
  ///
  /// This is not hypothetical: it is what happened the first time this golden
  /// met the `feeder` system landing between `economy` and `hero`.
  std::vector<std::string> systems;

  std::vector<TraceEntry> entries;

  [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
  [[nodiscard]] GameTime elapsed() const noexcept {
    return entries.empty() ? 0 : entries.back().time;
  }
  [[nodiscard]] WorldHashes final_hashes() const noexcept {
    return entries.empty() ? WorldHashes{} : entries.back().hashes;
  }
};

/// Drives a peer's input: what a turn's player orders are, applied before it
/// advances.
///
/// Without one a run is a match nobody is playing, which is what every check
/// above this line has always measured. `sim/netcmds.hpp` has the
/// implementation that applies a `CommandStream`; this is the seam, so that
/// the harness stays ignorant of what a command is -- the same reason `Run`
/// has only two virtuals.
///
/// `drive` is called once per turn, **before** `Run::advance`, with the index
/// of the turn about to run. It must be a pure function of that index and the
/// run it is given: the harness calls it once per peer per turn and compares
/// what comes out, so a driver that remembers anything between peers makes the
/// harness accuse the simulation of the driver's bug.
///
/// **It returns the `netcmds` channel**, and that is not decoration. The
/// command queue deliberately hashes nothing (`sim/command.hpp` says why), so
/// two peers handed *different orders* have identical world hashes until those
/// orders take effect -- which may be many turns later, in a system that has
/// no idea why. The shipped build hashed the command stream as its own channel
/// for exactly this reason, and this is where that channel comes from: the
/// driver answers the stream's hash through `turn`, `record` files it under
/// `Channel::netcmds`, and `compare_traces` reports it like any other. A driver
/// with nothing to say returns zero, which is what the channel reads as today
/// in every undriven run -- so no existing trace or golden moves.
class Driver {
 public:
  virtual ~Driver() = default;
  [[nodiscard]] virtual std::uint64_t drive(Run& run, std::size_t turn) = 0;
};

/// Record a run of `scenario` over `schedule`, sampling every turn.
///
/// An empty trace means `Scenario::start` returned null, which is a scenario
/// that could not be built -- reported rather than crashed, because a
/// conformance run over a corpus of maps meets one.
///
/// `driver`, when given, supplies each turn's player orders before that turn
/// runs.
[[nodiscard]] Trace record(const Scenario& scenario, std::uint32_t seed, Schedule schedule,
                           Driver* driver = nullptr);

// -- the trace file format -------------------------------------------------
//
// Line oriented, hexadecimal, `#` comments. Deliberately not a binary blob:
// the whole value of a golden file is that the diff is readable, and a reviewer
// looking at "turn 412 line changed, slots only" learns something a changed
// checksum does not tell them.
//
//     # imperivm conformance trace v1
//     scenario reference
//     seed 2a
//     systems movement combat economy feeder hero env command
//     # turn length time slots threads netcmds extrahash
//     t 1 400 400 3c8a1f... 0 0 0
//     t 2 400 800 = = = =
//
// The `systems` line comes before the hashes because it explains them: run
// order is folded into `slots`, so a difference there accounts for every
// difference under it and is reported first. See `Trace::systems`.
//
// `=` means "the same as the previous line", which is what makes a quiet
// scenario cheap to commit. The four reserved channels are not written at all:
// they must be zero, `write_trace` refuses a trace in which they are not, and
// `read_trace` reads them back as zero. Encoding a field whose only legal value
// is zero would invite somebody to change it.
//
// `hash_of_hashes` is not written either -- it is a pure function of the four
// that are -- and is recomputed on read, so a hand-edited golden file cannot
// disagree with itself.

/// `hash_of_hashes` as `World::hashes()` computes it, from the four channels a
/// trace carries.
///
/// Exposed so that a test can assert the harness's arithmetic agrees with the
/// world's. It duplicates a fold that lives in `world.cpp` -- see the comment
/// on `kFnvOffset` in `conformance.cpp` for why that duplication exists and
/// what guards it.
[[nodiscard]] std::uint64_t roll_up(const WorldHashes& hashes) noexcept;

/// Serialise a trace. Returns `malformed` if any reserved channel is non-zero,
/// which is a bug in the simulation rather than in the caller.
[[nodiscard]] Result<std::string> write_trace(const Trace& trace);

/// Parse a trace. Tolerates blank lines, `#` comments and trailing whitespace;
/// refuses anything else, because a golden file that silently half-parses is a
/// golden file that passes for the wrong reason.
[[nodiscard]] Result<Trace> read_trace(std::string_view text);

// --------------------------------------------------------------------------
// divergence
// --------------------------------------------------------------------------

/// Where and how two runs stopped agreeing.
///
/// The point of this struct is that it is *actionable*. "They differ" sends
/// somebody bisecting by hand; "channel `slots` first differs at turn 412, game
/// time 164800" points at one turn of one system.
struct Divergence {
  enum class Kind : std::uint8_t {
    none = 0,
    /// A hash channel differs. `channel`, `expected` and `actual` are set.
    hash,
    /// The two runs declared different turn lengths at the same index. A
    /// harness bug or a scenario that renegotiates on its own, not a desync.
    length,
    /// Same schedule, different accumulated game time. Means the clock itself
    /// is not summing declared lengths, which would break everything below.
    time,
    /// One run produced fewer turns than the other, or than the oracle.
    truncated,
    /// A reserved channel is non-zero. See `channel_is_reserved_zero`.
    reserved,
    /// The two runs ran a different set of systems, or ran them in a different
    /// order. Reported ahead of every hash difference because it *causes*
    /// them: `World::state_hash` folds each system's index and name in before
    /// its contribution. `expected_text` and `actual_text` carry the two
    /// orders, and `index` is the first position they differ at.
    pipeline,
    /// The scenario could not be built at all.
    unbuildable,
  };

  Kind kind = Kind::none;
  std::size_t index = 0;   ///< index into the entry sequence
  std::uint64_t turn = 0;  ///< the turn number, which is `index + 1` for a full run
  GameTime time = 0;
  Channel channel = Channel::slots;
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
  /// Set for `pipeline`: the two system run orders, space separated.
  std::string expected_text;
  std::string actual_text;
  /// What the two sides were called, for a message that names them.
  std::string left = "expected";
  std::string right = "actual";

  [[nodiscard]] bool diverged() const noexcept { return kind != Kind::none; }
  explicit operator bool() const noexcept { return diverged(); }

  /// One line, for a report or a test failure.
  [[nodiscard]] std::string describe() const;
};

/// The first turn at which two traces disagree, and the first channel that did.
///
/// "First" in both axes is the contract: entries are scanned in order and, at
/// the first entry that differs at all, channels are scanned in `Channel` order
/// so that `slots` is reported ahead of the `hash_of_hashes` it caused.
[[nodiscard]] Divergence compare_traces(const Trace& expected, const Trace& actual);

/// Fail a trace in which any of the four reserved channels ever went non-zero.
[[nodiscard]] Divergence check_reserved_channels(const Trace& trace);

// --------------------------------------------------------------------------
// the checks
// --------------------------------------------------------------------------

/// Run the scenario twice from one seed and compare per-turn hashes.
///
/// `recorded`, if given, receives the first run's trace -- so a caller that
/// wants both a determinism check and a golden-file comparison pays for one run
/// of each rather than three.
[[nodiscard]] Divergence check_self_consistency(const Scenario& scenario, std::uint32_t seed,
                                                Schedule schedule, Trace* recorded = nullptr);

/// Two peers, one command stream: run the scenario twice from one seed with
/// the same orders on the same turns, and compare per-turn hashes.
///
/// This is the claim multiplayer rests on and the one thing the checks above
/// do not make. `check_self_consistency` proves the simulation is a function
/// of its seed and schedule; it says nothing about *input*, because it feeds
/// none. A desync in a real match is almost never the world drifting on its
/// own -- it is two peers resolving one right click differently, and the whole
/// order-resolution path (the per-actor candidate walk, the share-control
/// filter, the verifier) runs only when somebody clicks.
///
/// Identical in shape to `check_self_consistency`, deliberately: the same
/// `compare_traces`, the same `Divergence`, and the only difference is that
/// both runs are driven. A failure here that `check_self_consistency` does not
/// also report is a failure in applying orders rather than in simulating them,
/// which is the distinction worth having.
[[nodiscard]] Divergence check_lockstep(const Scenario& scenario, std::uint32_t seed,
                                        Schedule schedule, Driver& driver,
                                        Trace* recorded = nullptr);

/// Compare a recording of the scenario against an expected trace.
///
/// `covers` on the oracle decides which channels are compared, so a partial
/// recording -- one that knows the original's `slots` but not its `threads` --
/// is usable without pretending to know more than it does.
class Oracle {
 public:
  virtual ~Oracle() = default;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual std::size_t size() const noexcept = 0;
  /// The expected entry at `index`, or null past the end.
  [[nodiscard]] virtual const TraceEntry* at(std::size_t index) const noexcept = 0;
  /// Whether this oracle's value for `channel` is meaningful. The default
  /// covers everything the engine can currently produce.
  [[nodiscard]] virtual bool covers(Channel channel) const noexcept {
    return !channel_is_reserved_zero(channel) && channel != Channel::count;
  }
  /// The system run order this oracle's hashes were produced under, or empty
  /// for an oracle that does not know. Compared before any hash, because a
  /// pipeline difference explains every hash difference under it.
  ///
  /// A stream recorded from the original engine returns empty here: the
  /// original's system list is not ours and comparing them would be
  /// meaningless. A golden file recorded from this engine returns what it
  /// recorded.
  [[nodiscard]] virtual std::span<const std::string> systems() const noexcept { return {}; }
};

/// An oracle backed by a `Trace` -- a golden file, or a second run.
class TraceOracle final : public Oracle {
 public:
  explicit TraceOracle(Trace trace, std::string label = "golden")
      : trace_(std::move(trace)), label_(std::move(label)) {}

  [[nodiscard]] std::string_view name() const noexcept override { return label_; }
  [[nodiscard]] std::size_t size() const noexcept override { return trace_.entries.size(); }
  [[nodiscard]] const TraceEntry* at(std::size_t index) const noexcept override {
    return index < trace_.entries.size() ? &trace_.entries[index] : nullptr;
  }
  [[nodiscard]] bool covers(Channel channel) const noexcept override {
    const auto index = static_cast<std::size_t>(channel);
    return index < kChannelCount && covered_[index];
  }
  [[nodiscard]] std::span<const std::string> systems() const noexcept override {
    return trace_.systems;
  }
  [[nodiscard]] const Trace& trace() const noexcept { return trace_; }

  /// Restrict what this oracle claims to know. A golden file recorded before
  /// the scheduler folded `threads` in should not be trusted about `threads`.
  void set_covers(Channel channel, bool on) noexcept;

 private:
  Trace trace_;
  std::string label_;
  bool covered_[kChannelCount] = {true, true, true, true, false, false, false, false, true};
};

/// Compare an actual trace against an oracle, channel by covered channel.
[[nodiscard]] Divergence check_against_oracle(const Trace& actual, const Oracle& oracle);

/// The same total game time, cut up differently, must end in the same state.
struct PartitionDivergence {
  /// `none` when every partition agreed.
  Divergence::Kind kind = Divergence::Kind::none;
  /// Index into the partitions that disagreed with the reference.
  std::size_t partition = 0;
  /// The turn lengths that partition used, for a message that shows them.
  std::vector<std::int32_t> schedule;
  GameTime reference_time = 0;
  GameTime actual_time = 0;
  Channel channel = Channel::slots;
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;

  [[nodiscard]] bool diverged() const noexcept { return kind != Divergence::Kind::none; }
  explicit operator bool() const noexcept { return diverged(); }
  [[nodiscard]] std::string describe() const;
};

/// Run `reference` and every schedule in `alternates` and require they end
/// identically.
///
/// Every alternate must deliver the same total game time as the reference;
/// one that does not is reported as a `time` divergence, because comparing two
/// runs of different length would be comparing nothing. Comparison is of
/// `World::state_hash()` after `Run::freeze_clock()`, plus elapsed game time --
/// see `freeze_clock` for why the clock is excluded and only the clock.
[[nodiscard]] PartitionDivergence check_partition_invariance(
    const Scenario& scenario, std::uint32_t seed, Schedule reference,
    std::span<const Schedule> alternates);

/// The convenience form: partition `reference` with `canonical_partitions`.
[[nodiscard]] PartitionDivergence check_partition_invariance(const Scenario& scenario,
                                                             std::uint32_t seed,
                                                             Schedule reference);

// --------------------------------------------------------------------------
// running the lot
// --------------------------------------------------------------------------

/// What a full conformance pass does and what it found.
struct Report {
  std::string scenario;
  std::uint32_t seed = 0;
  std::size_t turns = 0;
  GameTime elapsed = 0;
  std::size_t objects = 0;

  /// Checks attempted and checks that failed. A pass with zero checks is a
  /// pass that tested nothing, and the summary says so rather than printing
  /// "ok".
  std::size_t checks = 0;
  /// One line per failed check, already formatted.
  std::vector<std::string> failures;

  /// The recording of the reference run, for writing a golden file.
  Trace trace;

  [[nodiscard]] bool ok() const noexcept { return checks > 0 && failures.empty(); }
  /// A multi-line human summary. The core cannot print; the tool does.
  [[nodiscard]] std::string summary() const;
};

/// Which checks to run.
struct Options {
  bool self_consistency = true;
  /// **Sound over the systems; not sound once scripts run.** A script's wake
  /// count over a fixed total depends on the partition -- `Scheduler::advance`
  /// does not interpolate -- and shipped scripts draw from the world RNG when
  /// they wake, so the shared generator desynchronises. Lockstep does not need
  /// the invariant: peers agree on a turn length, so what must hold is that the
  /// same partition gives the same result, which is `self_consistency`.
  /// `docs/engine/conformance.md` has the evidence; `imconform map` sets this
  /// false whenever scripts are on.
  bool partition_invariance = true;
  bool reserved_channels = true;
  /// Compared against if non-null. Not owned.
  const Oracle* oracle = nullptr;
};

/// Run every enabled check over one scenario and one schedule.
[[nodiscard]] Report run(const Scenario& scenario, std::uint32_t seed, Schedule schedule,
                         const Options& options = {});

// --------------------------------------------------------------------------
// the built-in scenario
// --------------------------------------------------------------------------

/// A synthetic scenario with no game data behind it.
///
/// It exists so that the harness has something to exercise on a machine that
/// has never seen the game, and so that `imconform` has a payload before
/// sessions can be built from maps. It populates a world with units, buildings,
/// a settlement composite and a query object -- covering every branch of
/// `World::state_hash` -- and drives them with one system that decays health,
/// moves units and draws from the world RNG.
///
/// **It is deliberately written to be partition-invariant, and the way it is
/// written is the point.** Nothing in it does work "per turn": every effect is
/// driven by an accumulator over elapsed game time, drained with a `while`
/// loop, so the number of effects over an interval is a function of the
/// interval and not of how it was cut up. That is the discipline a real system
/// has to follow, and having one worked example of it in the tree is worth more
/// than the paragraph in `docs/engine/conformance.md` that says the same thing.
///
/// This means a pass on the reference scenario proves the *harness* works, not
/// that the game is deterministic. Only a scenario built from real inputs does
/// that. `docs/engine/conformance.md` says so at more length.
[[nodiscard]] std::unique_ptr<Scenario> make_reference_scenario();

/// The reference scenario, deliberately broken in one specific way, so that a
/// test can prove a check *fails* when it should.
///
/// A check that has never been seen to fail is not a check. These are how the
/// test suite demonstrates that each check actually catches something: they are
/// constructed defects, each aimed at exactly one check.
///
/// There is no defect here for the reserved-channel check, and the reason is
/// worth writing down rather than leaving as an omission. `World::hashes()`
/// sets `pathfinder`, `exploration`, `scriptstate` and `aihash` to zero
/// literally, and no `System` can reach them -- `System::hash` folds into
/// `slots` and nothing else. **The engine currently cannot produce that
/// defect**, which is the guarantee the check is there to keep true; the check
/// is proven against a hand-built `Trace` instead, and it earns its place the
/// day somebody edits `World::hashes()`.
enum class Defect : std::uint8_t {
  /// A system that counts turns instead of integrating elapsed time. Passes
  /// self-consistency, fails partition invariance. This is the realistic bug:
  /// it is what you get by writing `++cooldown_` in `advance`.
  counts_turns,
  /// A scenario whose `start()` is not a pure function of the seed: the second
  /// instance perturbs one object's health from turn five. Fails
  /// self-consistency at turn five, and names `slots`.
  impure_start,
  /// A system that draws from the world RNG once per turn rather than once per
  /// unit of elapsed time. Subtler than `counts_turns`: the object state stays
  /// right, and it is only the RNG's own position in the stream -- the dump's
  /// `syncseed` -- that ends up wrong. Fails partition invariance.
  draws_per_turn,
};

[[nodiscard]] std::unique_ptr<Scenario> make_defective_scenario(Defect defect);

}  // namespace imperivm::core::sim::conformance
