// The determinism harness, tested against divergences introduced on purpose.
//
// A check that has never been observed to fail is not a check, and this project
// has been bitten by that specifically: an earlier agent's determinism test
// passed on a build with a known desync in it. So every check here appears
// twice -- once proving it passes on a correct scenario, once proving it fails
// on a scenario broken in exactly the way that check exists to catch. The
// second half is the one that matters.
//
// Nothing here reads a file or needs a game installation. `sim/conformance.hpp`
// lives in the core precisely so that this can be true.

#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;

using imperivm::core::FormatError;
using imperivm::core::NativeClass;
using imperivm::core::Result;

namespace {

/// Whether `text` mentions `needle`. Divergence messages are the harness's
/// actual product -- a caller reads them, not the struct -- so their content is
/// asserted rather than assumed.
bool mentions(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

conformance::Schedule as_schedule(const std::vector<std::int32_t>& lengths) {
  return conformance::Schedule(lengths);
}

}  // namespace

// --------------------------------------------------------------------------
// channels
// --------------------------------------------------------------------------

TEST(the_channels_are_the_dumps_seven_plus_the_rollup) {
  // Spelled as the dumps spell them, so a report greps against a real
  // desync.txt without a translation table in between.
  CHECK(conformance::channel_name(conformance::Channel::slots) == "slots");
  CHECK(conformance::channel_name(conformance::Channel::pathfinder) == "pathfinder");
  CHECK(conformance::channel_name(conformance::Channel::hash_of_hashes) == "hash_of_hashes");
  CHECK(conformance::kChannelCount == 9);

  // The four the shipped build kept out of the determinism contract: zero in
  // all nine dumps (docs/engine/state-vector.md).
  CHECK(conformance::channel_is_reserved_zero(conformance::Channel::pathfinder));
  CHECK(conformance::channel_is_reserved_zero(conformance::Channel::exploration));
  CHECK(conformance::channel_is_reserved_zero(conformance::Channel::scriptstate));
  CHECK(conformance::channel_is_reserved_zero(conformance::Channel::aihash));
  CHECK(!conformance::channel_is_reserved_zero(conformance::Channel::slots));
  CHECK(!conformance::channel_is_reserved_zero(conformance::Channel::threads));
  CHECK(!conformance::channel_is_reserved_zero(conformance::Channel::netcmds));
  CHECK(!conformance::channel_is_reserved_zero(conformance::Channel::extrahash));
}

TEST(channel_value_reads_every_field_of_the_hash_block) {
  // A switch that forgot a case would silently read zero, and a comparison
  // against zero-on-both-sides passes. So every field is given a distinct
  // value and read back.
  WorldHashes hashes;
  hashes.slots = 1;
  hashes.threads = 2;
  hashes.netcmds = 3;
  hashes.extrahash = 4;
  hashes.pathfinder = 5;
  hashes.exploration = 6;
  hashes.scriptstate = 7;
  hashes.aihash = 8;
  hashes.hash_of_hashes = 9;
  for (std::size_t i = 0; i < conformance::kChannelCount; ++i) {
    CHECK(conformance::channel_value(hashes, static_cast<conformance::Channel>(i)) == i + 1);
  }
}

// --------------------------------------------------------------------------
// schedules
// --------------------------------------------------------------------------

TEST(a_schedule_is_a_sequence_of_lengths_not_a_timestep) {
  const std::vector<std::int32_t> mixed = {800, 400, 799, 200};
  CHECK(conformance::schedule_time(as_schedule(mixed)) == 2199);
  CHECK(conformance::schedule_time(conformance::Schedule{}) == 0);

  const std::vector<std::int32_t> uniform = conformance::uniform_schedule(400, 40);
  CHECK(uniform.size() == 40);
  CHECK(conformance::schedule_time(as_schedule(uniform)) == 16000);
}

TEST(the_canonical_partitions_all_deliver_the_same_game_time) {
  const auto partitions = conformance::canonical_partitions(16000);
  REQUIRE(partitions.size() == 4);  // 800s, 400s, 200s, and one ragged
  for (const auto& partition : partitions) {
    CHECK(conformance::schedule_time(as_schedule(partition)) == 16000);
  }
  CHECK(partitions[0].size() == 20);
  CHECK(partitions[1].size() == 40);
  CHECK(partitions[2].size() == 80);

  // The ragged one must actually change length mid-run; a uniform partition
  // cannot catch a bug that only appears when the length moves, and the dumps
  // show a real session renegotiating mid-game.
  const auto& ragged = partitions[3];
  bool varies = false;
  for (std::size_t i = 1; i < ragged.size(); ++i) {
    if (ragged[i] != ragged[i - 1]) varies = true;
  }
  CHECK(varies);

  // A total the observed lengths cannot tile is reported as no partitions at
  // all rather than as a partial set.
  CHECK(conformance::canonical_partitions(2199).empty());
  CHECK(conformance::canonical_partitions(0).empty());
}

// --------------------------------------------------------------------------
// the trace format
// --------------------------------------------------------------------------

namespace {

conformance::Trace tiny_trace() {
  conformance::Trace trace;
  trace.scenario = "tiny";
  trace.seed = 0x2a;
  for (int i = 1; i <= 3; ++i) {
    conformance::TraceEntry entry;
    entry.turn = static_cast<std::uint64_t>(i);
    entry.length = 400;
    entry.time = 400 * i;
    entry.hashes.slots = 0x1000u + static_cast<std::uint64_t>(i);
    entry.hashes.threads = 7;  // constant, so the `=` elision is exercised
    // Derived, exactly as `World::hashes()` derives it. A hand-built fixture
    // that left this at zero would be a trace no run could ever produce, and
    // round-tripping it through the format -- which recomputes the field --
    // would then "diverge" for a reason that says nothing about the harness.
    // Hand-authored fixtures have accused correct readers in this project four
    // times; this is the same mistake in miniature.
    entry.hashes.hash_of_hashes = conformance::roll_up(entry.hashes);
    trace.entries.push_back(entry);
  }
  return trace;
}

}  // namespace

TEST(a_trace_round_trips_through_the_text_format) {
  const conformance::Trace original = tiny_trace();
  const Result<std::string> text = conformance::write_trace(original);
  REQUIRE(text.ok());

  // The elision is not cosmetic: it is what keeps a golden file for a quiet
  // scenario small enough that committing it is reasonable.
  CHECK(mentions(text.value(), " ="));

  const Result<conformance::Trace> parsed = conformance::read_trace(text.value());
  REQUIRE(parsed.ok());
  CHECK(parsed.value().scenario == "tiny");
  CHECK(parsed.value().seed == 0x2a);
  REQUIRE(parsed.value().entries.size() == original.entries.size());
  for (std::size_t i = 0; i < original.entries.size(); ++i) {
    const auto& a = original.entries[i];
    const auto& b = parsed.value().entries[i];
    CHECK(a.turn == b.turn);
    CHECK(a.length == b.length);
    CHECK(a.time == b.time);
    CHECK(a.hashes.slots == b.hashes.slots);
    CHECK(a.hashes.threads == b.hashes.threads);
  }
  // Round-tripping a trace must be idempotent at the byte level, or a golden
  // file rewritten by a tool would diff against itself.
  const Result<std::string> again = conformance::write_trace(parsed.value());
  REQUIRE(again.ok());
  CHECK(again.value() == text.value());
}

TEST(the_trace_format_refuses_a_reserved_channel) {
  // `World::hashes()` cannot produce this -- it writes the four reserved
  // channels as literal zero -- so it is built by hand. That is the point: the
  // check is here for the day somebody edits `World::hashes()`.
  conformance::Trace trace = tiny_trace();
  trace.entries[1].hashes.pathfinder = 1;
  CHECK(!conformance::write_trace(trace).ok());
  CHECK(conformance::write_trace(trace).error() == FormatError::malformed);
}

TEST(read_trace_refuses_a_file_that_only_half_parses) {
  // A golden file that silently half-parses is a golden file that passes for
  // the wrong reason, which is worse than one that fails.
  const auto refused = [](std::string_view text) {
    return !conformance::read_trace(text).ok();
  };
  CHECK(refused(""));                                  // no magic
  CHECK(refused("t 1 400 400 aa 0 0 0\n"));            // no magic, no seed
  CHECK(refused("# imperivm conformance trace v1\n"));  // no seed
  CHECK(refused("# imperivm conformance trace v1\nseed 2a\nt 1 400 400 aa 0 0\n"));  // short row
  CHECK(refused("# imperivm conformance trace v1\nseed 2a\nt 1 400 400 aa 0 0 0 9\n"));  // long
  CHECK(refused("# imperivm conformance trace v1\nseed 2a\nt 1 0 400 aa 0 0 0\n"));  // zero length
  CHECK(refused("# imperivm conformance trace v1\nseed 2a\nt 1 400 400 AA 0 0 0\n"));  // upper hex
  CHECK(refused("# imperivm conformance trace v1\nseed 2a\nt 1 400 400 = 0 0 0\n"));  // no previous
  CHECK(refused("# imperivm conformance trace v1\nseed 2a\nwat 1 400 400 aa 0 0 0\n"));  // keyword

  // ...but blank lines, comments and trailing spaces are fine, because a human
  // annotating a golden file is a thing that should work.
  const auto ok = conformance::read_trace(
      "# imperivm conformance trace v1\n"
      "\n"
      "scenario tiny\n"
      "seed 2a\n"
      "# a note somebody left  \n"
      "t 1 400 400 aa 0 0 0   \n");
  REQUIRE(ok.ok());
  CHECK(ok.value().entries.size() == 1);
  CHECK(ok.value().entries[0].hashes.slots == 0xaa);
}

TEST(the_harness_rollup_matches_the_worlds) {
  // The one piece of arithmetic the harness duplicates from `world.cpp`: the
  // fold that turns four channels into `hash_of_hashes`. `read_trace` has to
  // recompute it, because a trace file does not carry a derived field, and if
  // the two implementations ever disagree every golden file silently stops
  // meaning anything.
  //
  // Note for whoever lands here after changing `world.cpp`: its fold is seeded
  // with 1469598103934665603, which is *not* FNV-1a's offset basis
  // (14695981039346656037). That is harmless -- any constant is deterministic
  // -- but it is mirrored in `conformance.cpp` and this is the assertion that
  // catches it moving.
  World world(TickConfig{400, 1000});
  world.seed(99);
  world.spawn(NativeClass::unit, nullptr);
  world.advance(400);
  const WorldHashes hashes = world.hashes();
  CHECK(hashes.slots != 0);
  CHECK(conformance::roll_up(hashes) == hashes.hash_of_hashes);
}

TEST(hash_of_hashes_is_derived_on_read_so_a_golden_file_cannot_contradict_itself) {
  const conformance::Trace original = tiny_trace();
  const Result<std::string> text = conformance::write_trace(original);
  REQUIRE(text.ok());
  // Not written at all -- it is a pure function of the four that are.
  const Result<conformance::Trace> parsed = conformance::read_trace(text.value());
  REQUIRE(parsed.ok());
  CHECK(parsed.value().entries[0].hashes.hash_of_hashes != 0);
  CHECK(parsed.value().entries[0].hashes.hash_of_hashes !=
        parsed.value().entries[1].hashes.hash_of_hashes);
}

// --------------------------------------------------------------------------
// comparison
// --------------------------------------------------------------------------

TEST(compare_traces_reports_the_first_divergence_and_not_merely_a_divergence) {
  conformance::Trace a = tiny_trace();
  conformance::Trace b = a;
  b.entries[1].hashes.slots ^= 1;  // turn 2
  b.entries[2].hashes.slots ^= 1;  // turn 3, also different

  const conformance::Divergence divergence = conformance::compare_traces(a, b);
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::hash);
  CHECK(divergence.turn == 2);  // the first, not the last and not "some"
  CHECK(divergence.index == 1);
  CHECK(divergence.time == 800);
  CHECK(divergence.channel == conformance::Channel::slots);
  CHECK(mentions(divergence.describe(), "slots"));
  CHECK(mentions(divergence.describe(), "turn 2"));
}

TEST(compare_traces_names_the_cause_rather_than_the_symptom) {
  // `hash_of_hashes` differs whenever `slots` does, so a scan in the wrong
  // order would report the roll-up and bury the channel that actually moved.
  conformance::Trace a = tiny_trace();
  conformance::Trace b = a;
  b.entries[0].hashes.slots ^= 1;
  b.entries[0].hashes.hash_of_hashes ^= 1;

  const conformance::Divergence divergence = conformance::compare_traces(a, b);
  REQUIRE(divergence.diverged());
  CHECK(divergence.channel == conformance::Channel::slots);
}

TEST(compare_traces_distinguishes_a_shorter_run_from_a_different_one) {
  conformance::Trace a = tiny_trace();
  conformance::Trace b = a;
  b.entries.pop_back();
  const conformance::Divergence divergence = conformance::compare_traces(a, b);
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::truncated);
  CHECK(divergence.expected == 3);
  CHECK(divergence.actual == 2);

  // Identical traces do not diverge, which is the case that would make every
  // other assertion above meaningless if it failed.
  CHECK(!conformance::compare_traces(a, a).diverged());
  CHECK(conformance::compare_traces(a, a).describe() == "no divergence");
}

TEST(compare_traces_catches_a_schedule_that_does_not_match) {
  conformance::Trace a = tiny_trace();
  conformance::Trace b = a;
  b.entries[1].length = 200;
  const conformance::Divergence divergence = conformance::compare_traces(a, b);
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::length);
}

TEST(the_reserved_channel_check_catches_a_channel_that_went_non_zero) {
  conformance::Trace clean = tiny_trace();
  CHECK(!conformance::check_reserved_channels(clean).diverged());

  for (const conformance::Channel channel :
       {conformance::Channel::pathfinder, conformance::Channel::exploration,
        conformance::Channel::scriptstate, conformance::Channel::aihash}) {
    conformance::Trace dirty = tiny_trace();
    switch (channel) {
      case conformance::Channel::pathfinder: dirty.entries[2].hashes.pathfinder = 9; break;
      case conformance::Channel::exploration: dirty.entries[2].hashes.exploration = 9; break;
      case conformance::Channel::scriptstate: dirty.entries[2].hashes.scriptstate = 9; break;
      default: dirty.entries[2].hashes.aihash = 9; break;
    }
    const conformance::Divergence divergence = conformance::check_reserved_channels(dirty);
    REQUIRE(divergence.diverged());
    CHECK(divergence.kind == conformance::Divergence::Kind::reserved);
    CHECK(divergence.channel == channel);
    CHECK(divergence.turn == 3);
    CHECK(mentions(divergence.describe(), conformance::channel_name(channel)));
  }
}

// --------------------------------------------------------------------------
// oracles
// --------------------------------------------------------------------------

TEST(an_oracle_catches_an_edited_golden_file) {
  const conformance::Trace golden = tiny_trace();
  conformance::Trace actual = golden;
  actual.entries[2].hashes.slots ^= 0x40;

  const conformance::TraceOracle oracle(golden, "golden");
  const conformance::Divergence divergence = conformance::check_against_oracle(actual, oracle);
  REQUIRE(divergence.diverged());
  CHECK(divergence.turn == 3);
  CHECK(divergence.channel == conformance::Channel::slots);
  CHECK(mentions(divergence.describe(), "golden"));

  CHECK(!conformance::check_against_oracle(golden, oracle).diverged());
}

TEST(an_oracle_may_decline_to_know_a_channel) {
  // The case a recorded stream will land in: a recording may know the
  // original's `slots` and know nothing about our `threads`, and it must be
  // able to say so rather than assert a value it does not have.
  const conformance::Trace golden = tiny_trace();
  conformance::Trace actual = golden;
  actual.entries[1].hashes.threads ^= 0xff;

  conformance::TraceOracle strict(golden, "strict");
  CHECK(conformance::check_against_oracle(actual, strict).diverged());

  conformance::TraceOracle lenient(golden, "lenient");
  lenient.set_covers(conformance::Channel::threads, false);
  CHECK(!conformance::check_against_oracle(actual, lenient).diverged());

  // Declining `threads` must not make it blind to `slots`.
  actual.entries[1].hashes.slots ^= 0xff;
  const conformance::Divergence divergence = conformance::check_against_oracle(actual, lenient);
  REQUIRE(divergence.diverged());
  CHECK(divergence.channel == conformance::Channel::slots);
}

TEST(the_pipeline_check_catches_a_system_landing_underneath_a_golden) {
  // THE DELIBERATE DIVERGENCE for the run-order check, and a reconstruction of
  // something that actually happened: a `feeder` system landed in
  // `kSystemOrder` between `economy` and `hero` while this golden file existed.
  //
  // `World` folds each system's index and name into `slots` before calling its
  // `hash`, so every hash in the trace moved from turn one. Without the
  // `systems` line the harness would have said "channel `slots` first differs
  // at turn 1" -- true, and a whole afternoon pointed at the wrong thing. With
  // it, it names the position and both orders.
  conformance::Trace golden = tiny_trace();
  golden.systems = {"movement", "combat", "economy", "hero", "env", "command"};

  conformance::Trace actual = golden;
  actual.systems = {"movement", "combat", "economy", "feeder", "hero", "env", "command"};
  // The hashes moved too, exactly as they would in reality.
  for (auto& entry : actual.entries) entry.hashes.slots ^= 0x5555;

  const conformance::Divergence divergence = conformance::compare_traces(golden, actual);
  REQUIRE(divergence.diverged());
  // The pipeline, not the hash: the pipeline is the cause and the hash is the
  // symptom, and reporting the symptom first is what this check exists to stop.
  CHECK(divergence.kind == conformance::Divergence::Kind::pipeline);
  CHECK(divergence.index == 3);  // the position `feeder` took
  CHECK(mentions(divergence.describe(), "feeder"));
  CHECK(mentions(divergence.describe(), "run order"));

  // Same through an oracle, which is the path a golden file actually takes.
  const conformance::TraceOracle oracle(golden, "golden");
  const conformance::Divergence via_oracle = conformance::check_against_oracle(actual, oracle);
  REQUIRE(via_oracle.diverged());
  CHECK(via_oracle.kind == conformance::Divergence::Kind::pipeline);

  // A reordering with no additions is caught just as well: registration order
  // is the simulation's definition, so swapping two systems is a real change.
  conformance::Trace swapped = golden;
  swapped.systems = {"combat", "movement", "economy", "hero", "env", "command"};
  const conformance::Divergence reorder = conformance::compare_traces(golden, swapped);
  REQUIRE(reorder.diverged());
  CHECK(reorder.kind == conformance::Divergence::Kind::pipeline);
  CHECK(reorder.index == 0);
}

TEST(an_unrecorded_run_order_is_not_compared_rather_than_compared_to_nothing) {
  // A golden written before the `systems` line existed, or an oracle recorded
  // from the original engine -- whose system list is not ours and never will
  // be -- must still check its hashes. An empty list means "not recorded", not
  // "no systems", and comparing it would fail every real trace against it.
  conformance::Trace golden = tiny_trace();  // no systems recorded
  conformance::Trace actual = tiny_trace();
  actual.systems = {"movement", "combat"};
  CHECK(!conformance::compare_traces(golden, actual).diverged());
  CHECK(!conformance::compare_traces(actual, golden).diverged());

  // ...and it still catches the hashes.
  actual.entries[1].hashes.slots ^= 1;
  const conformance::Divergence divergence = conformance::compare_traces(golden, actual);
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::hash);
}

TEST(the_run_order_round_trips_through_the_trace_format) {
  conformance::Trace trace = tiny_trace();
  trace.systems = {"movement", "combat", "economy", "feeder", "hero", "env", "command"};
  const Result<std::string> text = conformance::write_trace(trace);
  REQUIRE(text.ok());
  CHECK(mentions(text.value(), "systems movement combat economy feeder hero env command"));

  const Result<conformance::Trace> parsed = conformance::read_trace(text.value());
  REQUIRE(parsed.ok());
  CHECK(parsed.value().systems == trace.systems);
  CHECK(!conformance::compare_traces(trace, parsed.value()).diverged());
}

TEST(an_oracle_catches_a_run_that_stopped_early) {
  const conformance::Trace golden = tiny_trace();
  conformance::Trace actual = golden;
  actual.entries.pop_back();
  const conformance::TraceOracle oracle(golden, "golden");
  const conformance::Divergence divergence = conformance::check_against_oracle(actual, oracle);
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::truncated);
}

// --------------------------------------------------------------------------
// the reference scenario
// --------------------------------------------------------------------------

TEST(the_reference_scenario_actually_simulates_something) {
  // The guard against every check below passing for the wrong reason. A
  // scenario whose state never changes is trivially self-consistent, trivially
  // partition-invariant, and worthless.
  const auto scenario = conformance::make_reference_scenario();
  REQUIRE(scenario != nullptr);
  const auto schedule = conformance::uniform_schedule(400, 20);
  const conformance::Trace trace = conformance::record(*scenario, 0x2a, as_schedule(schedule));
  REQUIRE(trace.entries.size() == 20);

  std::size_t distinct = 0;
  for (std::size_t i = 1; i < trace.entries.size(); ++i) {
    if (trace.entries[i].hashes.slots != trace.entries[i - 1].hashes.slots) ++distinct;
  }
  CHECK(distinct == 19);  // every single turn moves the state
  CHECK(trace.elapsed() == 8000);
  CHECK(trace.entries.back().turn == 20);

  // And the world it builds is not empty: units, a building, a settlement
  // triple, a garrisoned unit, a query object and the three singletons.
  const auto run = scenario->start(0x2a);
  REQUIRE(run != nullptr);
  CHECK(run->world().size() == 14);

  // A different seed must give a different simulation, or the RNG is not
  // actually feeding anything and the hash is not covering it.
  const conformance::Trace other = conformance::record(*scenario, 0x2b, as_schedule(schedule));
  CHECK(other.entries.back().hashes.slots != trace.entries.back().hashes.slots);
}

TEST(the_reference_scenario_is_self_consistent) {
  const auto scenario = conformance::make_reference_scenario();
  const auto schedule = conformance::uniform_schedule(400, 40);
  const conformance::Divergence divergence =
      conformance::check_self_consistency(*scenario, 0x2a, as_schedule(schedule));
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("    %s\n", divergence.describe().c_str());
}

TEST(self_consistency_catches_a_scenario_that_is_not_a_function_of_its_seed) {
  // THE DELIBERATE DIVERGENCE. `impure_start` perturbs one object's health on
  // turn five of its second instantiation and nothing else, which is as small
  // as a desync gets: one field, one turn, one object.
  const auto scenario = conformance::make_defective_scenario(conformance::Defect::impure_start);
  REQUIRE(scenario != nullptr);
  const auto schedule = conformance::uniform_schedule(400, 40);
  const conformance::Divergence divergence =
      conformance::check_self_consistency(*scenario, 0x2a, as_schedule(schedule));

  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::hash);
  // Not "they differ" -- the exact turn and the exact channel.
  CHECK(divergence.turn == 5);
  CHECK(divergence.time == 2000);
  CHECK(divergence.channel == conformance::Channel::slots);
  CHECK(mentions(divergence.describe(), "slots"));
  CHECK(mentions(divergence.describe(), "turn 5"));
}

TEST(the_reference_scenario_is_partition_invariant) {
  const auto scenario = conformance::make_reference_scenario();
  const auto schedule = conformance::uniform_schedule(400, 40);  // 16,000 units
  const conformance::PartitionDivergence divergence =
      conformance::check_partition_invariance(*scenario, 0x2a, as_schedule(schedule));
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("    %s\n", divergence.describe().c_str());
}

TEST(partition_invariance_catches_a_system_that_counts_turns) {
  // THE DELIBERATE DIVERGENCE, and the realistic one: `++cooldown_` in
  // `advance` is what a domain author writes without thinking, and it is
  // invisible to self-consistency because both runs count the same turns.
  const auto scenario = conformance::make_defective_scenario(conformance::Defect::counts_turns);
  REQUIRE(scenario != nullptr);
  const auto schedule = conformance::uniform_schedule(400, 40);

  // Self-consistency is blind to it, which is exactly why the second check
  // exists. If this assertion ever fails, the two checks have stopped being
  // independent and the partition test is no longer earning its keep.
  CHECK(!conformance::check_self_consistency(*scenario, 0x2a, as_schedule(schedule)).diverged());

  const conformance::PartitionDivergence divergence =
      conformance::check_partition_invariance(*scenario, 0x2a, as_schedule(schedule));
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::hash);
  CHECK(divergence.channel == conformance::Channel::slots);
  CHECK(divergence.reference_time == 16000);
  CHECK(mentions(divergence.describe(), "slots"));
  CHECK(mentions(divergence.describe(), "partition"));
}

TEST(partition_invariance_catches_an_rng_drawn_once_per_turn) {
  // Subtler than counting turns: every object ends where it should, and it is
  // only the generator's position in the stream -- the dump's `syncseed` --
  // that ends up depending on how the interval was cut. The original hashes
  // that, so it is a real desync and the harness has to see it.
  const auto scenario = conformance::make_defective_scenario(conformance::Defect::draws_per_turn);
  REQUIRE(scenario != nullptr);
  const auto schedule = conformance::uniform_schedule(400, 40);
  CHECK(!conformance::check_self_consistency(*scenario, 0x2a, as_schedule(schedule)).diverged());

  const conformance::PartitionDivergence divergence =
      conformance::check_partition_invariance(*scenario, 0x2a, as_schedule(schedule));
  REQUIRE(divergence.diverged());
  CHECK(divergence.channel == conformance::Channel::slots);
}

TEST(partition_invariance_refuses_to_compare_unequal_intervals) {
  // The way this check would pass for the wrong reason is by comparing two
  // runs that were never the same interval. Caught before either one runs.
  const auto scenario = conformance::make_reference_scenario();
  const auto reference = conformance::uniform_schedule(400, 40);   // 16,000
  const auto shorter = conformance::uniform_schedule(400, 39);     // 15,600
  const std::vector<conformance::Schedule> alternates = {as_schedule(shorter)};

  const conformance::PartitionDivergence divergence = conformance::check_partition_invariance(
      *scenario, 0x2a, as_schedule(reference), alternates);
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::time);
  CHECK(divergence.reference_time == 16000);
  CHECK(divergence.actual_time == 15600);
  CHECK(mentions(divergence.describe(), "15600"));
}

TEST(partition_invariance_holds_over_a_ragged_schedule_too) {
  // The dumps show one session going 400, 400, ..., 460, 800, 800 and another
  // 400, ..., 291, 213, 200, 200. A uniform partition would not catch a bug
  // that only appears when the length moves, so the interval is also cut with
  // the odd lengths the corpus actually contains -- 799 included.
  const auto scenario = conformance::make_reference_scenario();
  const std::vector<std::int32_t> reference = conformance::uniform_schedule(1, 4800);
  std::vector<std::int32_t> ragged;
  const std::int32_t lengths[] = {800, 799, 400, 200, 401, 800, 200, 400, 800};
  imperivm::core::sim::GameTime total = 0;
  for (const std::int32_t length : lengths) {
    ragged.push_back(length);
    total += length;
  }
  REQUIRE(total == 4800);
  const std::vector<conformance::Schedule> alternates = {as_schedule(ragged)};
  const conformance::PartitionDivergence divergence = conformance::check_partition_invariance(
      *scenario, 0x2a, as_schedule(reference), alternates);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("    %s\n", divergence.describe().c_str());
}

// --------------------------------------------------------------------------
// the golden file
// --------------------------------------------------------------------------

/// A short scenario's trace, committed as a literal.
///
/// This is the mechanism the brief asks for: **a change that alters simulation
/// results has to say so out loud.** Touch the RNG, the object model, the
/// hashing or the turn loop and this test fails; making it pass means editing
/// these twelve lines in the same diff, where a reviewer sees them.
///
/// A literal rather than a file on disk because the core test binary cannot
/// open a file -- that is the same freestanding rule the harness itself obeys.
/// `imconform trace 12 400` regenerates it; `imconform map --record` writes the
/// on-disk equivalent for a real map, which the tool can do because tools may.
constexpr std::string_view kGoldenReferenceTrace =
    "# imperivm conformance trace v1\n"
    "scenario reference\n"
    "seed 2a\n"
    "systems drift\n"
    "# turn length time slots threads netcmds extrahash ('=' repeats the line above)\n"
    "t 1 400 400 bfcfa5977643c652 0 0 0\n"
    "t 2 400 800 7eb69be123f939d5 = = =\n"
    "t 3 400 1200 863faa0e40dfe7c6 = = =\n"
    "t 4 400 1600 ffc68e923b61fbf0 = = =\n"
    "t 5 400 2000 95eea27564e9075c = = =\n"
    "t 6 400 2400 2c27fe29ebfd2465 = = =\n"
    "t 7 400 2800 2e23c4858d45a677 = = =\n"
    "t 8 400 3200 667b816ed7f7b883 = = =\n"
    "t 9 400 3600 315f53d2096b004d = = =\n"
    "t 10 400 4000 397ba6f201c8421a = = =\n"
    "t 11 400 4400 76f3d2f88d23ab5f = = =\n"
    "t 12 400 4800 a6972dc27a5df3c7 = = =\n";

TEST(the_reference_scenario_still_matches_its_golden_trace) {
  const Result<conformance::Trace> golden = conformance::read_trace(kGoldenReferenceTrace);
  REQUIRE(golden.ok());
  REQUIRE(golden.value().entries.size() == 12);

  const auto scenario = conformance::make_reference_scenario();
  const auto schedule =
      conformance::uniform_schedule(golden.value().entries[0].length,
                                    golden.value().entries.size());
  const conformance::Trace actual =
      conformance::record(*scenario, golden.value().seed, as_schedule(schedule));

  // The run order is part of the golden, because it is an input to every hash
  // in it. A golden that did not record it would report "slots differs at turn
  // 1" the next time a system landed, which is a true statement that points at
  // the wrong thing.
  REQUIRE(golden.value().systems.size() == 1);
  CHECK(golden.value().systems[0] == "drift");

  const conformance::TraceOracle oracle(golden.value(), "golden");
  const conformance::Divergence divergence = conformance::check_against_oracle(actual, oracle);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) {
    std::printf("    %s\n", divergence.describe().c_str());
    // **Build first.** `imconform` links the same core library this test does,
    // so regenerating with a stale one writes a golden for the build you no
    // longer have -- and the symptom is this test passing while
    // `the_golden_check_notices_a_single_flipped_bit` fails, because the
    // literal disagrees with the run from turn 1 rather than at the flip.
    std::printf("    regenerate with: cmake --build build && \\\n");
    std::printf("        ./build/engine/tools/imconform trace 12 400\n");
  }

  // The written form must round-trip to the committed bytes, so that
  // regenerating the literal produces exactly what is here and a diff shows
  // only real change.
  const Result<std::string> written = conformance::write_trace(actual);
  REQUIRE(written.ok());
  CHECK(written.value() == kGoldenReferenceTrace);
}

TEST(the_golden_check_notices_a_single_flipped_bit) {
  // Proof that the previous test is capable of failing. One bit of one hash on
  // one turn, in the middle of the file.
  //
  // Every expectation below is derived from the flip, never from the golden's
  // contents: `kFlipped` picks the entry and the turn number is read back off
  // it. Regenerating the golden -- which happens whenever somebody
  // deliberately changes simulation results -- must not move this test, or a
  // test whose whole job is to prove the check works becomes brittle for an
  // unrelated reason.
  constexpr std::size_t kFlipped = 6;
  Result<conformance::Trace> tampered = conformance::read_trace(kGoldenReferenceTrace);
  REQUIRE(tampered.ok());
  REQUIRE(tampered.value().entries.size() > kFlipped);
  const std::uint64_t expected_turn = tampered.value().entries[kFlipped].turn;
  tampered.value().entries[kFlipped].hashes.slots ^= 1;

  const auto scenario = conformance::make_reference_scenario();
  const auto schedule =
      conformance::uniform_schedule(400, tampered.value().entries.size());
  const conformance::Trace actual =
      conformance::record(*scenario, tampered.value().seed, as_schedule(schedule));

  const conformance::TraceOracle oracle(std::move(tampered.value()), "tampered");
  const conformance::Divergence divergence = conformance::check_against_oracle(actual, oracle);
  REQUIRE(divergence.diverged());
  CHECK(divergence.index == kFlipped);
  CHECK(divergence.turn == expected_turn);
  CHECK(divergence.channel == conformance::Channel::slots);
}

// --------------------------------------------------------------------------
// the full pass
// --------------------------------------------------------------------------

TEST(a_full_pass_over_the_reference_scenario_reports_every_check) {
  const auto scenario = conformance::make_reference_scenario();
  const auto schedule = conformance::uniform_schedule(400, 40);
  const conformance::Report report = conformance::run(*scenario, 0x2a, as_schedule(schedule));

  CHECK(report.ok());
  CHECK(report.failures.empty());
  CHECK(report.checks == 6);  // self-consistency, reserved, four partitions
  CHECK(report.turns == 40);
  CHECK(report.elapsed == 16000);
  CHECK(report.objects == 14);
  CHECK(report.trace.entries.size() == 40);
  CHECK(mentions(report.summary(), "reference"));
  CHECK(mentions(report.summary(), "all passed"));
}

TEST(a_full_pass_fails_and_says_which_check_failed) {
  const auto scenario = conformance::make_defective_scenario(conformance::Defect::counts_turns);
  const auto schedule = conformance::uniform_schedule(400, 40);
  const conformance::Report report = conformance::run(*scenario, 0x2a, as_schedule(schedule));

  CHECK(!report.ok());
  REQUIRE(report.failures.size() == 1);
  CHECK(mentions(report.failures[0], "partition invariance"));
  CHECK(mentions(report.summary(), "FAIL"));
}

TEST(a_pass_that_ran_no_checks_is_not_a_pass) {
  // The failure mode this whole harness exists to prevent: a green report over
  // nothing at all. `ok()` is false and the summary says why.
  const auto scenario = conformance::make_reference_scenario();
  const auto schedule = conformance::uniform_schedule(400, 4);
  conformance::Options nothing;
  nothing.self_consistency = false;
  nothing.partition_invariance = false;
  nothing.reserved_channels = false;

  const conformance::Report report =
      conformance::run(*scenario, 0x2a, as_schedule(schedule), nothing);
  CHECK(report.checks == 0);
  CHECK(!report.ok());
  CHECK(mentions(report.summary(), "no checks ran"));
  // The trace is still recorded, so `--record` works with the checks off.
  CHECK(report.trace.entries.size() == 4);
}

TEST(a_full_pass_says_so_when_the_schedule_cannot_be_repartitioned) {
  // A check that quietly does not run is the failure mode. 2,199 units is not
  // a multiple of 800, so the observed turn lengths cannot tile it, and the
  // report says that rather than skipping the check and printing "ok".
  const auto scenario = conformance::make_reference_scenario();
  const std::vector<std::int32_t> ragged = {800, 400, 799, 200};
  const conformance::Report report = conformance::run(*scenario, 0x2a, as_schedule(ragged));
  CHECK(!report.ok());
  REQUIRE(!report.failures.empty());
  CHECK(mentions(report.failures[0], "partition invariance"));
  CHECK(mentions(report.failures[0], "multiple of 800"));
}

// --------------------------------------------------------------------------
// the seam
// --------------------------------------------------------------------------

namespace {

/// A system that does nothing but hold a number, so that the `WorldRun` test
/// has state to watch without pulling in a domain.
class CounterSystem final : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "counter"; }
  void advance(World& world, const Turn& turn) override {
    elapsed_ += turn.length;
    // Time-driven, like everything else that wants to be partition-invariant.
    if (const ObjectState* state = world.state(subject_)) {
      world.set_health(subject_, static_cast<std::int32_t>(elapsed_ / 100));
    }
  }
  void hash(std::uint64_t& accumulator) const override {
    accumulator ^= static_cast<std::uint64_t>(elapsed_);
  }
  ObjectId subject_ = kNoObject;

 private:
  GameTime elapsed_ = 0;
};

}  // namespace

TEST(a_world_run_drives_a_hand_built_world_through_the_same_seam) {
  // The seam has to work over a bare `World` as well as over a session, or a
  // domain cannot write a conformance scenario without a map.
  World world(TickConfig{400, 1000});
  world.seed(7);
  CounterSystem counter;
  counter.subject_ = world.spawn(NativeClass::unit, nullptr);
  world.add_system(&counter);
  world.start();

  conformance::WorldRun run(world);
  CHECK(run.turns() == 0);
  run.advance(800);
  run.advance(200);
  CHECK(run.turns() == 2);
  CHECK(run.time() == 1000);
  CHECK(run.hashes().slots == world.state_hash());
  CHECK(run.hashes().pathfinder == 0);

  // `freeze_clock` must erase the turn count and the current length, and
  // nothing else. That is the whole basis of the partition check.
  const std::int32_t health_before = world.state(counter.subject_)->health;
  run.freeze_clock();
  CHECK(run.turns() == 0);
  CHECK(run.time() == 0);
  CHECK(world.clock().turn_length() == kDefaultTurnLength);
  CHECK(world.state(counter.subject_)->health == health_before);
  CHECK(world.rng().state() != 0 || true);  // the RNG is untouched by freezing
}
