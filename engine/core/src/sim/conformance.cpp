// The determinism harness. See sim/conformance.hpp for what it checks and why,
// and docs/engine/conformance.md for how to add a scenario.
//
// Two constraints shape everything below.
//
//   * `engine/core` is freestanding, so there is no printf here. Every message
//     is built into a `std::string` and handed back; `engine/tools/imconform`
//     is the thing that prints. The number formatting at the top of this file
//     exists for that reason and no other.
//   * Nothing here may itself be non-deterministic, or the harness becomes a
//     source of the divergence it is meant to find. No unordered container, no
//     pointer compared or hashed, no floating point -- the same rules the code
//     under test obeys.

#include "imperivm/core/sim/conformance.hpp"

#include <utility>

namespace imperivm::core::sim::conformance {
namespace {

// --------------------------------------------------------------------------
// formatting
// --------------------------------------------------------------------------
//
// Hand-rolled because <cstdio> is off limits in the core and <format> is not
// available on every toolchain this has to build with. Small enough to read.

void append_uint(std::string& out, std::uint64_t value) {
  char digits[20];
  int count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (count > 0) out.push_back(digits[--count]);
}

void append_int(std::string& out, std::int64_t value) {
  if (value < 0) {
    out.push_back('-');
    // Negating the minimum would overflow; go through the unsigned domain.
    append_uint(out, ~static_cast<std::uint64_t>(value) + 1u);
    return;
  }
  append_uint(out, static_cast<std::uint64_t>(value));
}

/// Lower-case hex, no padding, `0` for zero. Traces are compared as text, so
/// the spelling has to be canonical: `0x00ff` and `ff` must not both be
/// writable or a golden file would diff against itself.
void append_hex(std::string& out, std::uint64_t value) {
  char digits[16];
  int count = 0;
  do {
    const auto nibble = static_cast<unsigned>(value & 0xFu);
    digits[count++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    value >>= 4;
  } while (value != 0);
  while (count > 0) out.push_back(digits[--count]);
}

// --------------------------------------------------------------------------
// parsing
// --------------------------------------------------------------------------

constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

void skip_space(std::string_view text, std::size_t& at) noexcept {
  while (at < text.size() && is_space(text[at])) ++at;
}

/// One whitespace-delimited token, or empty at end of line.
std::string_view next_token(std::string_view line, std::size_t& at) noexcept {
  skip_space(line, at);
  const std::size_t start = at;
  while (at < line.size() && !is_space(line[at])) ++at;
  return line.substr(start, at - start);
}

bool parse_hex(std::string_view token, std::uint64_t& out) noexcept {
  if (token.empty() || token.size() > 16) return false;
  std::uint64_t value = 0;
  for (const char c : token) {
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= static_cast<std::uint64_t>(c - 'a' + 10);
    } else {
      return false;  // upper case is refused: one spelling, or no canonical form
    }
  }
  out = value;
  return true;
}

bool parse_uint(std::string_view token, std::uint64_t& out) noexcept {
  if (token.empty() || token.size() > 20) return false;
  std::uint64_t value = 0;
  for (const char c : token) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  out = value;
  return true;
}

// --------------------------------------------------------------------------
// hashing, for the synthetic systems below
// --------------------------------------------------------------------------

/// **Copied from `world.cpp`, deliberately, including its oddity.**
///
/// `engine/core/src/sim/world.cpp` seeds its fold with 1469598103934665603,
/// which is *not* FNV-1a's offset basis -- that is 14695981039346656037
/// (0xcbf29ce484222325), one decimal digit longer, and the value `imcheck` and
/// `test_combat.cpp` both use. Any constant gives a deterministic hash, so this
/// is harmless to the property the engine cares about; but it means the trace
/// format's `hash_of_hashes`, which this file recomputes on read, has to match
/// the world's arithmetic rather than the textbook's.
///
/// So it is mirrored here rather than corrected, and
/// `the_harness_rollup_matches_the_worlds` in `test_conformance.cpp` asserts
/// the two agree over a real `World`. If somebody fixes the constant in
/// `world.cpp`, that test fails and names this line -- which is the whole point
/// of having it, since the alternative is a golden file that silently stops
/// meaning anything.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void fold(std::uint64_t& state, std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    state ^= static_cast<std::uint8_t>(value >> shift);
    state *= kFnvPrime;
  }
}

}  // namespace

// --------------------------------------------------------------------------
// channels
// --------------------------------------------------------------------------

std::string_view channel_name(Channel channel) noexcept {
  switch (channel) {
    case Channel::slots: return "slots";
    case Channel::threads: return "threads";
    case Channel::netcmds: return "netcmds";
    case Channel::extrahash: return "extrahash";
    case Channel::pathfinder: return "pathfinder";
    case Channel::exploration: return "exploration";
    case Channel::scriptstate: return "scriptstate";
    case Channel::aihash: return "aihash";
    case Channel::hash_of_hashes: return "hash_of_hashes";
    case Channel::count: break;
  }
  return "?";
}

std::uint64_t channel_value(const WorldHashes& hashes, Channel channel) noexcept {
  switch (channel) {
    case Channel::slots: return hashes.slots;
    case Channel::threads: return hashes.threads;
    case Channel::netcmds: return hashes.netcmds;
    case Channel::extrahash: return hashes.extrahash;
    case Channel::pathfinder: return hashes.pathfinder;
    case Channel::exploration: return hashes.exploration;
    case Channel::scriptstate: return hashes.scriptstate;
    case Channel::aihash: return hashes.aihash;
    case Channel::hash_of_hashes: return hashes.hash_of_hashes;
    case Channel::count: break;
  }
  return 0;
}

// --------------------------------------------------------------------------
// schedules
// --------------------------------------------------------------------------

GameTime schedule_time(Schedule schedule) noexcept {
  GameTime total = 0;
  for (const std::int32_t length : schedule) total += length;
  return total;
}

std::vector<std::int32_t> uniform_schedule(std::int32_t length, std::size_t count) {
  return std::vector<std::int32_t>(count, length);
}

std::vector<std::vector<std::int32_t>> canonical_partitions(GameTime total) {
  std::vector<std::vector<std::int32_t>> out;
  if (total <= 0 || total % 800 != 0) return out;

  for (const std::int32_t length : {800, 400, 200}) {
    out.push_back(uniform_schedule(length, static_cast<std::size_t>(total / length)));
  }

  // A ragged sequence over the same interval. 800 + 400 + 200 + 200 = 1600, so
  // it tiles any multiple of 1600; a total that is an odd multiple of 800 gets
  // one 800 on the end. A uniform partition cannot catch a bug that only
  // appears when the length *changes* mid-run, and the dumps show a real
  // session doing exactly that.
  std::vector<std::int32_t> ragged;
  GameTime remaining = total;
  while (remaining >= 1600) {
    ragged.insert(ragged.end(), {800, 400, 200, 200});
    remaining -= 1600;
  }
  while (remaining >= 800) {
    ragged.push_back(800);
    remaining -= 800;
  }
  out.push_back(std::move(ragged));
  return out;
}

// --------------------------------------------------------------------------
// Run
// --------------------------------------------------------------------------

void Run::freeze_clock() noexcept {
  Clock& clock = world().clock();
  // `reset()` zeroes the turn counter and accumulated game time but leaves the
  // configuration alone, so the turn length is set separately. Both runs get
  // the same constant; which constant it is does not matter, only that it is
  // the same one.
  clock.reset();
  clock.set_turn_length(kDefaultTurnLength);
}

// --------------------------------------------------------------------------
// recording
// --------------------------------------------------------------------------

Trace record(const Scenario& scenario, std::uint32_t seed, Schedule schedule, Driver* driver) {
  Trace trace;
  trace.scenario = std::string(scenario.name());
  trace.seed = seed;

  std::unique_ptr<Run> run = scenario.start(seed);
  if (run == nullptr) return trace;  // empty: reported as `unbuildable`

  // Registration order, captured before the first turn. It is an input to every
  // hash that follows -- see `Trace::systems` for why that matters enough to
  // record.
  for (const System* system : run->world().systems()) {
    trace.systems.emplace_back(system->name());
  }

  trace.entries.reserve(schedule.size());
  std::size_t index = 0;
  for (const std::int32_t length : schedule) {
    // Input first, then the turn: an order issued on turn n is a command the
    // simulation sees during turn n, which is what "the same orders on the
    // same turn" has to mean for two peers to agree.
    const std::uint64_t netcmds = driver != nullptr ? driver->drive(*run, index) : 0;
    ++index;
    run->advance(length);
    TraceEntry entry;
    entry.turn = run->turns();
    entry.length = length;
    entry.time = run->time();
    entry.hashes = run->hashes();
    // The world does not compute this one -- see `Driver` -- so the harness
    // files the driver's answer and rolls it up the way the world would have.
    entry.hashes.netcmds = netcmds;
    entry.hashes.hash_of_hashes = roll_up(entry.hashes);
    trace.entries.push_back(entry);
  }
  return trace;
}

// --------------------------------------------------------------------------
// the trace file format
// --------------------------------------------------------------------------

namespace {

constexpr std::string_view kTraceMagic = "# imperivm conformance trace v1";

/// The four channels a trace actually carries, in file order.
constexpr Channel kWrittenChannels[] = {Channel::slots, Channel::threads, Channel::netcmds,
                                        Channel::extrahash};

}  // namespace

std::uint64_t roll_up(const WorldHashes& hashes) noexcept {
  std::uint64_t roll = kFnvOffset;
  fold(roll, hashes.slots);
  fold(roll, hashes.threads);
  fold(roll, hashes.netcmds);
  fold(roll, hashes.extrahash);
  return roll;
}

Result<std::string> write_trace(const Trace& trace) {
  for (const TraceEntry& entry : trace.entries) {
    for (std::size_t i = 0; i < kChannelCount; ++i) {
      const auto channel = static_cast<Channel>(i);
      if (channel_is_reserved_zero(channel) && channel_value(entry.hashes, channel) != 0) {
        // Refused rather than written. A trace file that recorded a non-zero
        // reserved channel would make the defect look like the format's
        // problem the next time somebody read it back.
        return FormatError::malformed;
      }
    }
  }

  std::string out;
  out.reserve(64 + trace.entries.size() * 56);
  out.append(kTraceMagic).push_back('\n');
  out.append("scenario ").append(trace.scenario).push_back('\n');
  out.append("seed ");
  append_hex(out, trace.seed);
  out.push_back('\n');
  if (!trace.systems.empty()) {
    // Before the hashes, because it explains them.
    out.append("systems");
    for (const std::string& system : trace.systems) out.push_back(' '), out.append(system);
    out.push_back('\n');
  }
  out.append("# turn length time slots threads netcmds extrahash ('=' repeats the line above)\n");

  WorldHashes previous{};
  bool have_previous = false;
  for (const TraceEntry& entry : trace.entries) {
    out.push_back('t');
    out.push_back(' ');
    append_uint(out, entry.turn);
    out.push_back(' ');
    append_int(out, entry.length);
    out.push_back(' ');
    append_int(out, entry.time);
    for (const Channel channel : kWrittenChannels) {
      const std::uint64_t value = channel_value(entry.hashes, channel);
      out.push_back(' ');
      if (have_previous && value == channel_value(previous, channel)) {
        out.push_back('=');
      } else {
        append_hex(out, value);
      }
    }
    out.push_back('\n');
    previous = entry.hashes;
    have_previous = true;
  }
  return out;
}

Result<Trace> read_trace(std::string_view text) {
  Trace trace;
  bool saw_magic = false;
  bool saw_seed = false;
  WorldHashes previous{};
  bool have_previous = false;

  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t end = text.find('\n', at);
    const std::string_view raw =
        text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
    at = end == std::string_view::npos ? text.size() + 1 : end + 1;

    // Trim, then classify. A blank line and a comment are the only things
    // allowed to carry no meaning.
    std::size_t begin = 0;
    skip_space(raw, begin);
    std::string_view line = raw.substr(begin);
    while (!line.empty() && is_space(line.back())) line.remove_suffix(1);
    if (line.empty()) continue;
    if (line.front() == '#') {
      if (!saw_magic && line == kTraceMagic) saw_magic = true;
      continue;
    }

    std::size_t cursor = 0;
    const std::string_view keyword = next_token(line, cursor);

    if (keyword == "scenario") {
      skip_space(line, cursor);
      trace.scenario = std::string(line.substr(cursor));
      continue;
    }
    if (keyword == "seed") {
      std::uint64_t value = 0;
      if (!parse_hex(next_token(line, cursor), value) || value > 0xFFFFFFFFull) {
        return FormatError::malformed;
      }
      trace.seed = static_cast<std::uint32_t>(value);
      saw_seed = true;
      continue;
    }
    if (keyword == "systems") {
      trace.systems.clear();
      for (;;) {
        const std::string_view system = next_token(line, cursor);
        if (system.empty()) break;
        trace.systems.emplace_back(system);
      }
      continue;
    }
    if (keyword != "t") return FormatError::malformed;

    TraceEntry entry;
    std::uint64_t turn = 0;
    std::uint64_t length = 0;
    std::uint64_t time = 0;
    if (!parse_uint(next_token(line, cursor), turn)) return FormatError::malformed;
    if (!parse_uint(next_token(line, cursor), length)) return FormatError::malformed;
    if (!parse_uint(next_token(line, cursor), time)) return FormatError::malformed;
    if (length == 0 || length > 0x7FFFFFFFull) return FormatError::malformed;
    entry.turn = turn;
    entry.length = static_cast<std::int32_t>(length);
    entry.time = static_cast<GameTime>(time);

    std::uint64_t values[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; i < 4; ++i) {
      const std::string_view token = next_token(line, cursor);
      if (token == "=") {
        // A repeat on the first line has nothing to repeat.
        if (!have_previous) return FormatError::malformed;
        values[i] = channel_value(previous, kWrittenChannels[i]);
      } else if (!parse_hex(token, values[i])) {
        return FormatError::malformed;
      }
    }
    // Trailing junk is an error rather than ignored: a golden file with an
    // extra column is one somebody meant to mean something.
    if (!next_token(line, cursor).empty()) return FormatError::malformed;

    entry.hashes.slots = values[0];
    entry.hashes.threads = values[1];
    entry.hashes.netcmds = values[2];
    entry.hashes.extrahash = values[3];
    // Derived on read, so a hand-edited file cannot contradict itself.
    entry.hashes.hash_of_hashes = roll_up(entry.hashes);

    previous = entry.hashes;
    have_previous = true;
    trace.entries.push_back(entry);
  }

  if (!saw_magic) return FormatError::bad_magic;
  if (!saw_seed) return FormatError::malformed;
  return trace;
}

// --------------------------------------------------------------------------
// divergence
// --------------------------------------------------------------------------

std::string Divergence::describe() const {
  std::string out;
  switch (kind) {
    case Kind::none:
      return "no divergence";
    case Kind::unbuildable:
      out.append("scenario could not be built");
      return out;
    case Kind::truncated:
      out.append(left).append(" has ");
      append_uint(out, expected);
      out.append(" turns, ").append(right).append(" has ");
      append_uint(out, actual);
      return out;
    case Kind::length:
      out.append("turn length disagrees at turn ");
      append_uint(out, turn);
      out.append(": ").append(left).append(" declared ");
      append_uint(out, expected);
      out.append(", ").append(right).append(" declared ");
      append_uint(out, actual);
      return out;
    case Kind::time:
      out.append("game time disagrees at turn ");
      append_uint(out, turn);
      out.append(": ").append(left).append(" is at ");
      append_uint(out, expected);
      out.append(", ").append(right).append(" is at ");
      append_uint(out, actual);
      return out;
    case Kind::reserved:
      out.append("channel `").append(channel_name(channel));
      out.append("` is non-zero (0x");
      append_hex(out, actual);
      out.append(") at turn ");
      append_uint(out, turn);
      out.append(" -- it is zero in all nine retail dumps and must stay zero");
      return out;
    case Kind::pipeline:
      out.append("the system run order changed at position ");
      append_uint(out, index);
      out.append(": ").append(left).append(" ran [").append(expected_text);
      out.append("], ").append(right).append(" ran [").append(actual_text);
      out.append("]. Run order is folded into `slots`, so every hash below this "
                 "differs because of it and not because of a simulation change");
      return out;
    case Kind::hash:
      out.append("channel `").append(channel_name(channel));
      out.append("` first differs at turn ");
      append_uint(out, turn);
      out.append(" (game time ");
      append_int(out, time);
      out.append("): ").append(left).append(" 0x");
      append_hex(out, expected);
      out.append(", ").append(right).append(" 0x");
      append_hex(out, actual);
      return out;
  }
  return "?";
}

namespace {

std::string join(const std::vector<std::string>& names) {
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i != 0) out.push_back(' ');
    out.append(names[i]);
  }
  return out;
}

/// Compare two system run orders.
///
/// Checked before any hash, because it *causes* hash differences rather than
/// being one: `World::state_hash` folds each system's index and name in before
/// calling its `hash`, so a pipeline that gained a system differs from turn one
/// in a way that has nothing to do with the simulation changing. Reporting the
/// hash first would send a reader hunting a bug that is not there -- which is
/// exactly what happened to this file's own golden the first time a system
/// landed underneath it.
///
/// An empty list on either side means "not recorded" and is not compared. A
/// golden written before this field existed still parses and still checks its
/// hashes; it just cannot explain them.
bool pipelines_differ(const std::vector<std::string>& expected,
                      const std::vector<std::string>& actual, Divergence& out) {
  if (expected.empty() || actual.empty()) return false;

  std::size_t at = 0;
  const std::size_t common = expected.size() < actual.size() ? expected.size() : actual.size();
  while (at < common && expected[at] == actual[at]) ++at;
  if (at == common && expected.size() == actual.size()) return false;

  out.kind = Divergence::Kind::pipeline;
  out.index = at;
  out.expected_text = join(expected);
  out.actual_text = join(actual);
  return true;
}

}  // namespace

Divergence compare_traces(const Trace& expected, const Trace& actual) {
  Divergence out;
  if (pipelines_differ(expected.systems, actual.systems, out)) return out;

  const std::size_t common =
      expected.entries.size() < actual.entries.size() ? expected.entries.size()
                                                      : actual.entries.size();

  for (std::size_t i = 0; i < common; ++i) {
    const TraceEntry& a = expected.entries[i];
    const TraceEntry& b = actual.entries[i];
    out.index = i;
    out.turn = a.turn;
    out.time = a.time;

    if (a.length != b.length) {
      out.kind = Divergence::Kind::length;
      out.expected = static_cast<std::uint64_t>(a.length);
      out.actual = static_cast<std::uint64_t>(b.length);
      return out;
    }
    if (a.time != b.time) {
      out.kind = Divergence::Kind::time;
      out.expected = static_cast<std::uint64_t>(a.time);
      out.actual = static_cast<std::uint64_t>(b.time);
      return out;
    }
    // Channels in declaration order, so that `slots` is reported ahead of the
    // `hash_of_hashes` it caused.
    for (std::size_t c = 0; c < kChannelCount; ++c) {
      const auto channel = static_cast<Channel>(c);
      const std::uint64_t left = channel_value(a.hashes, channel);
      const std::uint64_t right = channel_value(b.hashes, channel);
      if (left == right) continue;
      out.kind = Divergence::Kind::hash;
      out.channel = channel;
      out.expected = left;
      out.actual = right;
      return out;
    }
  }

  // Only after the common prefix agrees, because a length difference that also
  // diverges is better reported at the turn it diverged.
  if (expected.entries.size() != actual.entries.size()) {
    out.kind = Divergence::Kind::truncated;
    out.index = common;
    out.expected = expected.entries.size();
    out.actual = actual.entries.size();
    return out;
  }
  out.kind = Divergence::Kind::none;
  return out;
}

Divergence check_reserved_channels(const Trace& trace) {
  Divergence out;
  for (std::size_t i = 0; i < trace.entries.size(); ++i) {
    const TraceEntry& entry = trace.entries[i];
    for (std::size_t c = 0; c < kChannelCount; ++c) {
      const auto channel = static_cast<Channel>(c);
      if (!channel_is_reserved_zero(channel)) continue;
      const std::uint64_t value = channel_value(entry.hashes, channel);
      if (value == 0) continue;
      out.kind = Divergence::Kind::reserved;
      out.index = i;
      out.turn = entry.turn;
      out.time = entry.time;
      out.channel = channel;
      out.actual = value;
      return out;
    }
  }
  return out;
}

// --------------------------------------------------------------------------
// the checks
// --------------------------------------------------------------------------

Divergence check_self_consistency(const Scenario& scenario, std::uint32_t seed, Schedule schedule,
                                  Trace* recorded) {
  const Trace first = record(scenario, seed, schedule);
  if (recorded != nullptr) *recorded = first;

  Divergence out;
  if (first.entries.empty() && !schedule.empty()) {
    out.kind = Divergence::Kind::unbuildable;
    return out;
  }

  const Trace second = record(scenario, seed, schedule);
  out = compare_traces(first, second);
  out.left = "run 1";
  out.right = "run 2";
  return out;
}

Divergence check_lockstep(const Scenario& scenario, std::uint32_t seed, Schedule schedule,
                          Driver& driver, Trace* recorded) {
  const Trace first = record(scenario, seed, schedule, &driver);
  if (recorded != nullptr) *recorded = first;

  Divergence out;
  if (first.entries.empty() && !schedule.empty()) {
    out.kind = Divergence::Kind::unbuildable;
    return out;
  }

  const Trace second = record(scenario, seed, schedule, &driver);
  out = compare_traces(first, second);
  out.left = "peer 1";
  out.right = "peer 2";
  return out;
}

void TraceOracle::set_covers(Channel channel, bool on) noexcept {
  const auto index = static_cast<std::size_t>(channel);
  if (index < kChannelCount) covered_[index] = on;
}

Divergence check_against_oracle(const Trace& actual, const Oracle& oracle) {
  Divergence out;
  out.left = std::string(oracle.name());
  out.right = "run";

  const std::span<const std::string> want_systems = oracle.systems();
  if (pipelines_differ(std::vector<std::string>(want_systems.begin(), want_systems.end()),
                       actual.systems, out)) {
    return out;
  }

  const std::size_t common =
      oracle.size() < actual.entries.size() ? oracle.size() : actual.entries.size();
  for (std::size_t i = 0; i < common; ++i) {
    const TraceEntry* want = oracle.at(i);
    if (want == nullptr) break;
    const TraceEntry& got = actual.entries[i];
    out.index = i;
    out.turn = want->turn;
    out.time = want->time;

    if (want->length != got.length) {
      out.kind = Divergence::Kind::length;
      out.expected = static_cast<std::uint64_t>(want->length);
      out.actual = static_cast<std::uint64_t>(got.length);
      return out;
    }
    if (want->time != got.time) {
      out.kind = Divergence::Kind::time;
      out.expected = static_cast<std::uint64_t>(want->time);
      out.actual = static_cast<std::uint64_t>(got.time);
      return out;
    }
    for (std::size_t c = 0; c < kChannelCount; ++c) {
      const auto channel = static_cast<Channel>(c);
      if (!oracle.covers(channel)) continue;
      const std::uint64_t want_value = channel_value(want->hashes, channel);
      const std::uint64_t got_value = channel_value(got.hashes, channel);
      if (want_value == got_value) continue;
      out.kind = Divergence::Kind::hash;
      out.channel = channel;
      out.expected = want_value;
      out.actual = got_value;
      return out;
    }
  }

  if (oracle.size() != actual.entries.size()) {
    out.kind = Divergence::Kind::truncated;
    out.index = common;
    out.expected = oracle.size();
    out.actual = actual.entries.size();
    return out;
  }
  return out;
}

std::string PartitionDivergence::describe() const {
  std::string out;
  if (kind == Divergence::Kind::none) return "no divergence";

  out.append("partition ");
  append_uint(out, partition);
  out.append(" [");
  for (std::size_t i = 0; i < schedule.size(); ++i) {
    if (i != 0) out.append(", ");
    if (i == 6 && schedule.size() > 8) {
      // A thousand-turn schedule in a one-line message helps nobody.
      out.append("... ");
      append_uint(out, schedule.size() - 6);
      out.append(" more");
      break;
    }
    append_int(out, schedule[i]);
  }
  out.append("] ");

  switch (kind) {
    case Divergence::Kind::unbuildable:
      out.append("could not be built");
      return out;
    case Divergence::Kind::time:
      out.append("delivers ");
      append_int(out, actual_time);
      out.append(" game-time units, but the reference delivers ");
      append_int(out, reference_time);
      return out;
    default:
      out.append("ends in a different state: channel `");
      out.append(channel_name(channel));
      out.append("` is 0x");
      append_hex(out, actual);
      out.append(" against the reference's 0x");
      append_hex(out, expected);
      out.append(" after ");
      append_int(out, reference_time);
      out.append(" game-time units");
      return out;
  }
}

namespace {

/// Run a schedule to the end and return the state as it stands once the clock's
/// turn count and current length have been erased. See `Run::freeze_clock`.
bool run_frozen(const Scenario& scenario, std::uint32_t seed, Schedule schedule,
                WorldHashes& hashes, GameTime& elapsed) {
  std::unique_ptr<Run> run = scenario.start(seed);
  if (run == nullptr) return false;
  for (const std::int32_t length : schedule) run->advance(length);
  elapsed = run->time();
  run->freeze_clock();
  hashes = run->hashes();
  return true;
}

}  // namespace

PartitionDivergence check_partition_invariance(const Scenario& scenario, std::uint32_t seed,
                                               Schedule reference,
                                               std::span<const Schedule> alternates) {
  PartitionDivergence out;

  WorldHashes want{};
  GameTime want_time = 0;
  if (!run_frozen(scenario, seed, reference, want, want_time)) {
    out.kind = Divergence::Kind::unbuildable;
    return out;
  }
  out.reference_time = want_time;

  for (std::size_t i = 0; i < alternates.size(); ++i) {
    const Schedule alternate = alternates[i];
    out.partition = i;
    out.schedule.assign(alternate.begin(), alternate.end());

    // Checked before running: two schedules that deliver different amounts of
    // game time are not two cuts of one interval, and comparing them would be
    // comparing nothing. This is the check that stops a partition test from
    // passing for the wrong reason.
    const GameTime total = schedule_time(alternate);
    if (total != want_time) {
      out.kind = Divergence::Kind::time;
      out.actual_time = total;
      return out;
    }

    WorldHashes got{};
    GameTime got_time = 0;
    if (!run_frozen(scenario, seed, alternate, got, got_time)) {
      out.kind = Divergence::Kind::unbuildable;
      return out;
    }
    if (got_time != want_time) {
      out.kind = Divergence::Kind::time;
      out.actual_time = got_time;
      return out;
    }

    for (std::size_t c = 0; c < kChannelCount; ++c) {
      const auto channel = static_cast<Channel>(c);
      const std::uint64_t want_value = channel_value(want, channel);
      const std::uint64_t got_value = channel_value(got, channel);
      if (want_value == got_value) continue;
      out.kind = Divergence::Kind::hash;
      out.channel = channel;
      out.expected = want_value;
      out.actual = got_value;
      return out;
    }
  }

  out.schedule.clear();
  out.partition = 0;
  out.kind = Divergence::Kind::none;
  return out;
}

PartitionDivergence check_partition_invariance(const Scenario& scenario, std::uint32_t seed,
                                               Schedule reference) {
  const std::vector<std::vector<std::int32_t>> partitions =
      canonical_partitions(schedule_time(reference));
  std::vector<Schedule> spans;
  spans.reserve(partitions.size());
  for (const std::vector<std::int32_t>& partition : partitions) spans.emplace_back(partition);
  return check_partition_invariance(scenario, seed, reference, spans);
}

// --------------------------------------------------------------------------
// running the lot
// --------------------------------------------------------------------------

std::string Report::summary() const {
  std::string out;
  out.append("scenario ").append(scenario).append(", seed 0x");
  append_hex(out, seed);
  out.append(", ");
  append_uint(out, turns);
  out.append(" turns, ");
  append_int(out, elapsed);
  out.append(" game-time units, ");
  append_uint(out, objects);
  out.append(" objects\n");

  // The run order, because it is folded into every hash below it and a reader
  // comparing two reports needs to see it without going to the source.
  if (!trace.systems.empty()) {
    out.append("systems ");
    for (std::size_t i = 0; i < trace.systems.size(); ++i) {
      if (i != 0) out.append(" -> ");
      out.append(trace.systems[i]);
    }
    out.push_back('\n');
  }

  for (const std::string& failure : failures) out.append("  FAIL ").append(failure).push_back('\n');

  if (checks == 0) {
    // Never "ok". A pass with nothing in it is the failure mode this whole
    // harness exists to make impossible.
    out.append("  no checks ran -- nothing was tested\n");
    return out;
  }
  append_uint(out, checks);
  out.append(failures.empty() ? " checks, all passed\n" : " checks, ");
  if (!failures.empty()) {
    append_uint(out, failures.size());
    out.append(" failed\n");
  }
  return out;
}

Report run(const Scenario& scenario, std::uint32_t seed, Schedule schedule,
           const Options& options) {
  Report report;
  report.scenario = std::string(scenario.name());
  report.seed = seed;

  if (options.self_consistency) {
    ++report.checks;
    const Divergence divergence = check_self_consistency(scenario, seed, schedule, &report.trace);
    if (divergence) report.failures.push_back("self-consistency: " + divergence.describe());
  } else {
    report.trace = record(scenario, seed, schedule);
  }

  report.turns = report.trace.entries.size();
  report.elapsed = report.trace.elapsed();
  {
    // Object count is reporting only, so a fresh run is cheap enough and keeps
    // the checks from having to hand a world back.
    std::unique_ptr<Run> probe = scenario.start(seed);
    if (probe != nullptr) report.objects = probe->world().size();
  }

  if (options.reserved_channels) {
    ++report.checks;
    const Divergence divergence = check_reserved_channels(report.trace);
    if (divergence) report.failures.push_back("reserved channels: " + divergence.describe());
  }

  if (options.partition_invariance) {
    const std::vector<std::vector<std::int32_t>> partitions =
        canonical_partitions(schedule_time(schedule));
    if (partitions.empty()) {
      // Said out loud rather than skipped silently. A schedule whose total is
      // not a multiple of 800 cannot be repartitioned from the observed
      // lengths, and a check that quietly does not run is the failure mode.
      report.failures.push_back(
          "partition invariance: schedule total is not a multiple of 800, so it cannot be "
          "repartitioned into the turn lengths the dumps record");
      ++report.checks;
    } else {
      std::vector<Schedule> spans;
      spans.reserve(partitions.size());
      for (const std::vector<std::int32_t>& partition : partitions) spans.emplace_back(partition);
      report.checks += spans.size();
      const PartitionDivergence divergence =
          check_partition_invariance(scenario, seed, schedule, spans);
      if (divergence) report.failures.push_back("partition invariance: " + divergence.describe());
    }
  }

  if (options.oracle != nullptr) {
    ++report.checks;
    const Divergence divergence = check_against_oracle(report.trace, *options.oracle);
    if (divergence) report.failures.push_back("oracle: " + divergence.describe());
  }

  return report;
}

// --------------------------------------------------------------------------
// the built-in scenario
// --------------------------------------------------------------------------
//
// Everything below is synthetic. It needs no game data, which is the point: it
// runs in CI on a machine that has never seen the game, and it is the payload
// `imconform` drives before sessions can be built from real maps.

namespace {

/// The state the reference system keeps per object.
struct Actor {
  ObjectId id = kNoObject;
  std::int32_t vx = 0;
  std::int32_t vy = 0;
  std::int32_t wear = 0;  ///< accumulated damage debt, in game-time units
};

/// A system driven entirely by elapsed game time.
///
/// **How it is written is the specification of a partition-invariant system.**
/// Nothing here does work "per turn". Every effect is driven by an accumulator
/// over `turn.length` drained with a `while` loop, so the number of effects over
/// an interval is a function of the interval and not of how it was cut up:
/// `(a + b) mod c` does not depend on where the sum was split, which is the same
/// argument `advance_elapsed` rests on.
///
/// The two things that would break it, and that `Defect` reproduces on purpose:
/// an `if` instead of a `while` (which drops effects when a turn is longer than
/// the period), and anything that counts turns.
class DriftSystem : public System {
 public:
  DriftSystem() = default;

  void set_counts_turns(bool on) noexcept { counts_turns_ = on; }
  void set_draws_per_turn(bool on) noexcept { draws_per_turn_ = on; }

  [[nodiscard]] std::string_view name() const noexcept override { return "drift"; }

  void start(World& world) override {
    actors_.clear();
    for (const WorldObject& slot : world.objects()) {
      if (slot.internal != InternalKind::none) continue;
      if (!slot.state.flags.is_unit) continue;
      Actor actor;
      actor.id = slot.id;
      // Derived from the id, so the layout is a pure function of the world and
      // not of anything the harness could accidentally vary.
      actor.vx = 1 + static_cast<std::int32_t>(slot.id % 3);
      actor.vy = 1 + static_cast<std::int32_t>((slot.id * 7) % 5);
      actors_.push_back(actor);
    }
  }

  void advance(World& world, const Turn& turn) override {
    if (counts_turns_) {
      // THE DEFECT: state that depends on how many times `advance` was called
      // rather than on how much time passed. Exactly what `++cooldown_` gives.
      ++turn_count_;
    }
    if (draws_per_turn_) {
      // THE SUBTLER DEFECT: the RNG's position in the stream ends up depending
      // on the turn count. The object state stays right; `syncseed` does not.
      (void)world.rng().below(1000);
    }

    phase_ += turn.length;
    // A `while`, not an `if`. With an `if` a single 800-unit turn would fire
    // once where four 200-unit turns fire four times, and partition invariance
    // would fail -- which is what makes this loop the load-bearing line.
    while (phase_ >= kStridePeriod) {
      phase_ -= kStridePeriod;
      for (Actor& actor : actors_) {
        const ObjectState* state = world.state(actor.id);
        if (state == nullptr) continue;
        Point to = state->position;
        to.x += actor.vx;
        to.y += actor.vy;
        // Bounce inside a box, so a long run stays in coordinate range and the
        // state keeps changing instead of settling.
        if (to.x < 0 || to.x > kBox) actor.vx = -actor.vx;
        if (to.y < 0 || to.y > kBox) actor.vy = -actor.vy;
        world.set_position(actor.id, to);
      }
    }

    wear_ += turn.length;
    while (wear_ >= kWearPeriod) {
      wear_ -= kWearPeriod;
      // One draw per wear tick, from the world's generator -- never a private
      // one. Which actor takes the hit is random, so the RNG's own state is
      // part of what the hash has to reproduce.
      if (actors_.empty()) continue;
      const auto index = static_cast<std::size_t>(
          world.rng().below(static_cast<std::int32_t>(actors_.size())));
      Actor& actor = actors_[index];
      const ObjectState* state = world.state(actor.id);
      if (state == nullptr) continue;
      std::int32_t health = state->health - (1 + world.rng().below(3));
      if (health <= 0) health = kMaxHealth;  // revive rather than despawn: keeps
                                             // the object table stable so the
                                             // check is about state, not size
      world.set_health(actor.id, health);
      ++actor.wear;
    }
  }

  void hash(std::uint64_t& accumulator) const override {
    fold(accumulator, static_cast<std::uint64_t>(phase_));
    fold(accumulator, static_cast<std::uint64_t>(wear_));
    fold(accumulator, actors_.size());
    for (const Actor& actor : actors_) {
      fold(accumulator, actor.id);
      fold(accumulator, static_cast<std::uint64_t>(static_cast<std::uint32_t>(actor.vx)));
      fold(accumulator, static_cast<std::uint64_t>(static_cast<std::uint32_t>(actor.vy)));
      fold(accumulator, static_cast<std::uint64_t>(actor.wear));
    }
    if (counts_turns_) fold(accumulator, turn_count_);
  }

 private:
  static constexpr std::int64_t kStridePeriod = 250;
  static constexpr std::int64_t kWearPeriod = 700;
  static constexpr std::int32_t kBox = 4000;
  static constexpr std::int32_t kMaxHealth = 200;

  std::vector<Actor> actors_;
  std::int64_t phase_ = 0;
  std::int64_t wear_ = 0;
  std::uint64_t turn_count_ = 0;
  bool counts_turns_ = false;
  bool draws_per_turn_ = false;
};

/// A world, its system and the run over it, owned together so that the lifetime
/// is one object the caller can hold in a `unique_ptr<Run>`.
class ReferenceRun final : public Run {
 public:
  ReferenceRun(std::uint32_t seed, bool counts_turns, bool draws_per_turn, bool perturb)
      : world_(TickConfig{kDefaultTurnLength, kDefaultGameSpeed}) {
    world_.seed(seed);
    drift_.set_counts_turns(counts_turns);
    drift_.set_draws_per_turn(draws_per_turn);
    perturb_ = perturb;
    populate();
    world_.add_system(&drift_);
    world_.start();
  }

  [[nodiscard]] World& world() noexcept override { return world_; }

  void advance(std::int32_t turn_length) override {
    world_.advance(turn_length);
    if (perturb_ && world_.turns() == 5) {
      // THE DEFECT for `impure_start`: this instance is not the same simulation
      // as the one the same seed produced a moment ago. One field, one turn.
      if (const ObjectState* state = world_.state(first_unit_)) {
        world_.set_health(first_unit_, state->health + 1);
      }
    }
  }

 private:
  void populate() {
    // A settlement composite first, because allocation order is observable
    // state and the dumps show the triple minted before the objects that belong
    // to it. Reproducing the shape here means the hash covers the branch of
    // `World::state_hash` that handles internal objects.
    const World::SettlementIds town = world_.spawn_settlement(/*owner=*/0);

    for (int i = 0; i < 6; ++i) {
      const bool building = i % 3 == 2;
      const ObjectId id =
          world_.spawn(building ? NativeClass::building : NativeClass::unit, nullptr);
      ObjectState* state = world_.mutable_state(id);
      state->owner = static_cast<PlayerId>(i % 2);
      state->flags.is_unit = !building;
      state->flags.is_building = building;
      state->health = 200;
      state->stamina = 100;
      world_.set_position(id, Point{100 * i, 50 * i});
      if (first_unit_ == kNoObject && !building) first_unit_ = id;
      WorldObject* slot = world_.find(id);
      if (slot != nullptr) slot->settlement = town.settlement;
    }

    // One garrisoned unit, so the held-position rule (-1,-1 plus a holder) is
    // inside the hash rather than only inside its own unit test.
    const ObjectId garrison = world_.spawn(NativeClass::unit, nullptr);
    if (ObjectState* state = world_.mutable_state(garrison)) {
      state->owner = 0;
      state->flags.is_unit = true;
      state->health = 200;
    }
    world_.put_in_holder(garrison, town.holder);

    // A query object: it takes a handle from the same counter as everything
    // else, and its *definition* is hashed state.
    QuerySpec spec;
    spec.kind = QueryKind::map_area_circle;
    spec.center = Point{0, 0};
    spec.radius = 1000;
    world_.create_query(spec);

    world_.spawn_singletons();
  }

  World world_;
  DriftSystem drift_;
  ObjectId first_unit_ = kNoObject;
  bool perturb_ = false;
};

class ReferenceScenario final : public Scenario {
 public:
  ReferenceScenario(std::string_view name, bool counts_turns, bool draws_per_turn, bool impure)
      : name_(name), counts_turns_(counts_turns), draws_per_turn_(draws_per_turn),
        impure_(impure) {}

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }

  [[nodiscard]] std::unique_ptr<Run> start(std::uint32_t seed) const override {
    // `starts_` is the only mutable state in any scenario here, and it exists
    // solely to make `impure_start` impure -- the second instance differs from
    // the first. A real scenario must have nothing like it.
    const bool perturb = impure_ && starts_++ > 0;
    return std::make_unique<ReferenceRun>(seed, counts_turns_, draws_per_turn_, perturb);
  }

 private:
  std::string name_;
  bool counts_turns_ = false;
  bool draws_per_turn_ = false;
  bool impure_ = false;
  mutable std::size_t starts_ = 0;
};

}  // namespace

std::unique_ptr<Scenario> make_reference_scenario() {
  return std::make_unique<ReferenceScenario>("reference", false, false, false);
}

std::unique_ptr<Scenario> make_defective_scenario(Defect defect) {
  switch (defect) {
    case Defect::counts_turns:
      return std::make_unique<ReferenceScenario>("defect:counts_turns", true, false, false);
    case Defect::impure_start:
      return std::make_unique<ReferenceScenario>("defect:impure_start", false, false, true);
    case Defect::draws_per_turn:
      return std::make_unique<ReferenceScenario>("defect:draws_per_turn", false, true, false);
  }
  return nullptr;
}

}  // namespace imperivm::core::sim::conformance
