// Save a real session, load it into a fresh one, and prove the two are the
// same game.
//
// `engine/tests/core_tests` links `imperivm_core` and nothing else -- "if a
// test needs a file or a window, it is testing the wrong layer" -- so the
// round trip it can run is the synthetic one in `test_save.cpp`. That test
// proves the encoder and the decoder agree with each other over a world built
// by hand. It cannot prove the save carries *Numantia*: 1,600 objects, 34
// settlements, several hundred suspended coroutines, ten systems with state in
// all of them. This is the half that opens the installation, and it is the
// check that matters, for the reason `imrun` exists at all -- every one of this
// project's worst bugs was invisible until two domains met.
//
// ## Three checks, and why one of them is not the hash
//
// 1. **`verify_hashes` at the load.** `SaveMeta` records `World::hashes()` as
//    it stood at the save; `GameSession::load` recomputes them once every
//    section is applied and refuses a mismatch. This catches any system that
//    folds into `slots`, which is seven of the ten.
//
// 2. **The re-save is byte-identical.** Save, load into a fresh session, save
//    again: the two blobs must match byte for byte. **This is the only check
//    that sees `command` and `ai`**, which contribute nothing to any hash --
//    `CommandSystem` has no `hash` override and `AiSystem::hash` is a no-op --
//    so a save that dropped a command queue or an AI script binding passes
//    check 1 unchanged and then diverges a few turns later with nothing to
//    point at.
//
// 3. **The per-turn hash sequences agree.** Both sessions advance the same
//    ragged schedule and every channel is compared at *every* turn, not only
//    at the end. A save that restored a field to a value that happens to hash
//    the same at turn zero and drifts afterwards shows up here.
//
// ## And a check that the checks work
//
// `--verify-faults` re-runs the whole thing ten times, each time deleting one
// system's section from the save before loading it, and requires every one of
// them to be caught. A check that has never been seen to fail is not a check.
//
// It lives outside engine/core because it opens files, which core may not.

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>

#include <span>
#include <string>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/gamedata/installation.hpp"
#include "imperivm/gamedata/save_file.hpp"
#include "imperivm/core/version.hpp"

using namespace imperivm::core;
namespace gamedata = imperivm::gamedata;

namespace {

/// Which section of `save` byte `at` belongs to, or a note that it is in the
/// envelope itself. The reader accounts for every byte -- `SaveReader::open`
/// refuses a save with any left over -- so a walk of the named sections either
/// lands or says the offset is header or framing.
[[nodiscard]] std::string section_at(std::span<const std::byte> save, std::size_t at) {
  const imperivm::core::Result<sim::SaveReader> reader = sim::SaveReader::open(save);
  if (!reader.ok()) return "unreadable";
  for (const std::string& name : reader->names()) {
    const std::span<const std::byte> payload = reader->section(name);
    if (payload.empty()) continue;
    const std::size_t begin = static_cast<std::size_t>(payload.data() - save.data());
    if (at >= begin && at < begin + payload.size()) return name;
  }
  return "envelope or section framing";
}

/// Not a uniform schedule. The turn length is renegotiated in a real session --
/// 200, 400, 799 and 800 all occur across the nine retail dumps -- and a bug
/// that only shows when it changes is the one worth catching.
constexpr std::int32_t kSchedule[] = {400, 400, 800, 200, 799, 400, 800, 200};

[[nodiscard]] std::int32_t schedule_at(std::size_t turn) {
  return kSchedule[turn % (sizeof(kSchedule) / sizeof(kSchedule[0]))];
}

/// Everything loaded once and shared by both sessions.
///
/// The class graph, the host registry and the script pack are world-invariant,
/// so loading them per session would only give the two a chance to differ.
struct Fixture {
  gamedata::Installation install;
  gamedata::MapContainer container;
  gamedata::MapPayloads payloads;
  script::HostRegistry registry;
  /// The container's own scripts ahead of the pack's -- `imrun`'s and the
  /// app's chain. **This tool ran without it for as long as it existed**, so
  /// no mission sequence ever ran inside a round trip and the campaign
  /// section was byte-identical at turn 100 to turn 0 on every map: the save
  /// sweep was measuring a skirmish on every campaign container.
  std::unique_ptr<gamedata::ContainerScripts> scripts;
  /// The map identity every save and load in this run uses: the container's
  /// installation-relative spelling plus the map number -- what a save file's
  /// manifest carries, so a file this tool writes loads where the app loads it.
  std::string identity;
};

[[nodiscard]] bool open_fixture(const std::string& game, const std::string& map,
                                std::string_view index, Fixture& out) {
  std::string error;
  if (!out.install.open(game, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  }
  if (!out.container.open(map, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  }
  out.payloads = gamedata::read_payloads(out.container, index);
  if (!out.payloads.ok()) {
    std::fprintf(stderr, "no map.obj.xml in %s\n", map.c_str());
    return false;
  }
  out.scripts = std::make_unique<gamedata::ContainerScripts>(out.container, out.install.scripts());
  sim::SaveFileManifest manifest;
  manifest.container = gamedata::container_relative(game, map);
  manifest.map_index = gamedata::map_number(out.payloads.map_directory);
  out.identity = gamedata::map_identity(manifest);
  (void)sim::register_all_hosts(out.registry);
  return true;
}

/// Build a session and bring it to the state a game is in on turn zero.
///
/// The three starts are in the order `imrun` and `imconform` use, and the order
/// matters: each of them draws script ids from the same counter, and a script
/// id is world state.
[[nodiscard]] std::unique_ptr<sim::GameSession> start_session(Fixture& fixture,
                                                              std::uint32_t seed,
                                                              bool scripts) {
  sim::SessionInputs inputs = gamedata::session_inputs(fixture.install, fixture.payloads);
  inputs.scripts = scripts ? fixture.scripts.get() : nullptr;

  auto session = sim::GameSession::create(fixture.registry, inputs, seed);
  if (!session.ok()) {
    std::fprintf(stderr, "session failed to build: %d\n", static_cast<int>(session.error()));
    return nullptr;
  }
  std::unique_ptr<sim::GameSession> run = std::move(session.value());
  // The app's order: match, objects, AI, then the mission -- the container's
  // manifest and then the map's. Each draws script ids from one counter.
  (void)run->start_match(sim::MatchOptions{});
  (void)run->start_object_scripts();
  (void)run->start_ai();
  if (scripts) {
    (void)run->start_sequences(fixture.payloads.game_sequences, /*base=*/"");
    (void)run->start_sequences(fixture.payloads.map_sequences, fixture.payloads.map_directory);
  }
  return run;
}

/// One row of the comparison: every channel a turn can move.
struct Hashes {
  std::uint64_t slots = 0;
  std::uint64_t threads = 0;
  std::uint64_t netcmds = 0;
  std::uint64_t extrahash = 0;
  std::int64_t time = 0;
  std::uint64_t turns = 0;

  [[nodiscard]] static Hashes of(sim::GameSession& session) {
    const sim::WorldHashes h = session.world().hashes();
    return Hashes{h.slots, h.threads,      h.netcmds,
                  h.extrahash, session.world().time(), session.world().turns()};
  }
  [[nodiscard]] friend bool operator==(const Hashes&, const Hashes&) = default;
};

[[nodiscard]] const char* first_difference(const Hashes& a, const Hashes& b) {
  if (a.turns != b.turns) return "turns";
  if (a.time != b.time) return "time";
  // In `conformance::Channel` order, so that `slots` -- the channel that
  // carries the real answer in all nine dumps -- is reported ahead of anything
  // it caused.
  if (a.slots != b.slots) return "slots";
  if (a.threads != b.threads) return "threads";
  if (a.netcmds != b.netcmds) return "netcmds";
  if (a.extrahash != b.extrahash) return "extrahash";
  return nullptr;
}

/// Rebuild a save without one system's section.
///
/// The fault injector. Deleting the section is a truer fault than skipping the
/// restore would be, because it is exactly what a save written by a build that
/// had never learned to serialise that system would look like: `load` finds no
/// section, reports the system in `unrestored_systems`, and leaves it holding
/// whatever the fresh session started with.
/// Whether a system's saved state differs from what a freshly started session
/// would have produced.
///
/// **A fault the scenario cannot express is not a failed check.** If a system's
/// section at turn N is byte-identical to its section at turn 0, then deleting
/// it loses nothing and no check on earth could notice -- which is a fact about
/// the map and the turn count, not about the save. So the fault report
/// distinguishes the two, and only a system that *had* something to lose is
/// required to be caught.
[[nodiscard]] bool has_state_to_lose(std::span<const std::byte> fresh,
                                     std::span<const std::byte> saved,
                                     std::string_view system) {
  const Result<sim::SaveReader> a = sim::SaveReader::open(fresh);
  const Result<sim::SaveReader> b = sim::SaveReader::open(saved);
  if (!a.ok() || !b.ok()) return true;
  const std::span<const std::byte> x = a->system_section(system);
  const std::span<const std::byte> y = b->system_section(system);
  return x.size() != y.size() || !std::equal(x.begin(), x.end(), y.begin());
}

[[nodiscard]] std::vector<std::byte> without_system(std::span<const std::byte> save,
                                                    std::string_view system) {
  const Result<sim::SaveReader> reader = sim::SaveReader::open(save);
  if (!reader.ok()) return {};
  const std::string drop = sim::system_section_name(system);

  sim::SaveWriter writer(reader->meta());
  for (const std::string& name : reader->names()) {
    if (name == "meta" || name == drop) continue;
    if (!writer.add(name, reader->section(name)).ok()) return {};
  }
  return writer.finish();
}

struct Args {
  std::string game;
  std::string map;
  /// The `Maps/<n>` inside the container, empty for the first in walk order.
  /// A conquest holds seven and their numbers are not contiguous.
  std::string map_index;
  std::size_t turns = 40;
  std::size_t more = 20;
  std::uint32_t seed = 1;
  bool scripts = true;
  bool faults = false;
  bool quiet = false;
  /// Write the save as a file -- the container the app reads -- here.
  std::string out;
};

/// The whole round trip, over `save` as handed in.
///
/// Returns true when the two sessions are indistinguishable. `expect_caught`
/// inverts that: with a section deleted, an *undetected* difference is the
/// failure.
struct Outcome {
  bool loaded = false;
  bool hashes_ok = false;
  bool resave_identical = false;
  bool trace_agrees = false;
  std::string detail;

  [[nodiscard]] bool clean() const noexcept {
    return loaded && hashes_ok && resave_identical && trace_agrees;
  }
  /// Which check noticed, for the fault report.
  [[nodiscard]] const char* caught_by() const noexcept {
    if (!loaded) return "load refused";
    if (!hashes_ok) return "verify_hashes";
    if (!resave_identical) return "re-save differs";
    if (!trace_agrees) return "hash trace diverges";
    return nullptr;
  }
};

/// `save` is what the fresh session is asked to load. `expected` is what a
/// correct re-save has to equal -- the *honest* blob, which is the same object
/// in the clean case and the un-faulted one when a section has been deleted.
///
/// The distinction is the whole of the fault check. Comparing a faulted load's
/// re-save against the faulted blob would only prove the envelope round-trips
/// its own section count; comparing it against the honest blob asks the
/// question that matters -- **did that system's state survive?**
[[nodiscard]] Outcome round_trip(Fixture& fixture, const Args& args,
                                 sim::GameSession& original,
                                 std::span<const std::byte> save,
                                 std::span<const std::byte> expected, bool verbose) {
  Outcome out;

  std::unique_ptr<sim::GameSession> restored =
      start_session(fixture, args.seed, args.scripts);
  if (restored == nullptr) {
    out.detail = "the fresh session did not build";
    return out;
  }

  sim::SessionLoadReport report;
  const Status status = restored->load(save, fixture.identity, &report);
  out.loaded = status.ok();
  out.hashes_ok = report.hashes.ok;
  if (!status.ok()) {
    char buffer[256];
    if (!report.hashes.ok) {
      std::snprintf(buffer, sizeof(buffer),
                    "channel %.*s expected %016llx actual %016llx",
                    static_cast<int>(report.hashes.channel.size()),
                    report.hashes.channel.data(),
                    static_cast<unsigned long long>(report.hashes.expected),
                    static_cast<unsigned long long>(report.hashes.actual));
    } else {
      std::snprintf(buffer, sizeof(buffer), "load refused: error %d",
                    static_cast<int>(status.error()));
    }
    out.detail = buffer;
    return out;
  }
  if (verbose) {
    std::printf("  loaded    %zu objects, %zu scripts, turn %llu, time %lld\n", report.objects,
                report.scripts, static_cast<unsigned long long>(report.meta.turns),
                static_cast<long long>(report.meta.time));
    if (!report.unrestored_systems.empty()) {
      std::printf("  UNRESTORED");
      for (const std::string& name : report.unrestored_systems) {
        std::printf(" %s", name.c_str());
      }
      std::printf("\n");
    }
    if (!report.unconsumed.empty()) {
      std::printf("  UNCONSUMED");
      for (const std::string& name : report.unconsumed) std::printf(" %s", name.c_str());
      std::printf("\n");
    }
  }

  // Check 2. The only one that sees `command` and `ai`.
  const Result<std::vector<std::byte>> again = restored->save(fixture.identity);
  out.resave_identical =
      again.ok() && again.value().size() == expected.size() &&
      std::equal(again.value().begin(), again.value().end(), expected.begin());
  if (!out.resave_identical) {
    // **Name the section, not just the length.** Two saves of equal size that
    // differ somewhere is the shape this check exists to catch, and "re-save
    // differs" on its own sends the next reader to a hex editor. The envelope
    // knows where every byte belongs, so ask it.
    char buffer[256];
    if (again.ok() && again.value().size() == expected.size()) {
      const auto [a, b] = std::mismatch(again.value().begin(), again.value().end(),
                                        expected.begin());
      const std::size_t at = static_cast<std::size_t>(a - again.value().begin());
      std::snprintf(buffer, sizeof(buffer), "re-save differs at byte %zu of %zu, in section '%s'",
                    at, expected.size(), section_at(expected, at).c_str());
    } else {
      std::snprintf(buffer, sizeof(buffer), "re-save is %zu bytes against %zu",
                    again.ok() ? again.value().size() : 0u, expected.size());
    }
    out.detail = buffer;
    return out;
  }

  // `IMSAVE_TRACE=<text>`: every host call made, on either side, by a script
  // whose source name contains `<text>`, tagged `A` (the original) or `B`
  // (the restored), with its arguments, outcome and line -- `imrun`'s
  // `IMRUN_TRACE`, doubled, so a divergence points at the first call the two
  // sides answered differently rather than at a hash. `IMSAVE_TRACE_LIMIT`
  // caps the lines per side (default 2000).
  struct Trace {
    script::Scheduler* scheduler = nullptr;
    const char* tag = "";
    std::string needle;
    std::size_t limit = 2000;
    std::size_t printed = 0;
  };
  static Trace traces[2];
  if (const char* needle = std::getenv("IMSAVE_TRACE")) {
    const script::CallTrace tap =
        [](void* user, script::ScriptId, std::string_view source_name, std::string_view name,
           std::span<const script::Value> args, const script::HostOutcome& outcome,
           std::int32_t line) {
          Trace& t = *static_cast<Trace*>(user);
          if (t.printed >= t.limit) return;
          const std::string source(source_name);
          if (source.find(t.needle) == std::string::npos) return;
          ++t.printed;
          std::string text = std::string("  tr") + t.tag + " " + source + ":" +
                             std::to_string(line) + " " + std::string(name) + "(";
          const auto show = [&](const script::Value& v) {
            if (v.is_integer()) return std::to_string(v.as_integer());
            if (v.is_string()) return "\"" + v.as_string() + "\"";
            if (v.is_nil()) return std::string("nil");
            script::Host* host = t.scheduler->host();
            if (host == nullptr) return std::string("?");
            const auto rendered = host->to_string(v);
            return rendered.ok() ? rendered.value() : std::string("?");
          };
          for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) text += ", ";
            text += show(args[i]);
          }
          text += ")";
          switch (outcome.status) {
            case script::HostStatus::ok:
              text += " -> " + (outcome.value.is_nil() ? std::string("void") : show(outcome.value));
              break;
            case script::HostStatus::retry: text += " -> retry"; break;
            case script::HostStatus::suspend:
              text += " -> suspend " + std::to_string(outcome.suspend_for);
              break;
            case script::HostStatus::finish: text += " -> finish"; break;
            case script::HostStatus::error:
              text += std::string(" -> ERROR ") + (outcome.error != nullptr ? outcome.error : "");
              break;
          }
          std::fprintf(stderr, "%s\n", text.c_str());
        };
    sim::GameSession* sides[2] = {&original, restored.get()};
    const char* tags[2] = {"A", "B"};
    for (int side = 0; side < 2; ++side) {
      traces[side].scheduler = &sides[side]->scheduler();
      traces[side].tag = tags[side];
      traces[side].needle = needle;
      traces[side].printed = 0;
      if (const char* limit = std::getenv("IMSAVE_TRACE_LIMIT")) {
        traces[side].limit = std::strtoul(limit, nullptr, 10);
      }
      sides[side]->scheduler().set_call_trace(tap, &traces[side]);
    }
  }

  // Check 3. Every turn, not just the last.
  out.trace_agrees = true;
  for (std::size_t i = 0; i < args.more; ++i) {
    const std::int32_t length = schedule_at(args.turns + i);
    original.advance(1, length);
    restored->advance(1, length);
    const Hashes a = Hashes::of(original);
    const Hashes b = Hashes::of(*restored);
    if (const char* channel = first_difference(a, b); channel != nullptr) {
      if (std::getenv("IMSAVE_DIFF") != nullptr) {
        // `IMSAVE_DIFF=1`: name the first objects whose state differs, field
        // by field, so a diverging trace points at a system rather than at a
        // hash. This is what found `MoveState::walking` missing from the
        // movement section: every moving unit's cursor was at step zero on
        // the restored side and nowhere else was anything different.
        std::vector<std::byte> wa, wb;
        original.world().serialize(wa);
        restored->world().serialize(wb);
        const std::size_t n = std::min(wa.size(), wb.size());
        std::size_t at = 0;
        while (at < n && wa[at] == wb[at]) ++at;
        std::fprintf(stderr, "world bytes differ at %zu of %zu/%zu\n", at, wa.size(), wb.size());
        std::size_t k = 0;
        for (const auto& slot : original.world().objects()) {
          const auto* other = restored->world().find(slot.id);
          if (other == nullptr) { std::fprintf(stderr, "object %u missing\n", slot.id); break; }
          bool diff = slot.state.position.x != other->state.position.x || slot.state.position.y != other->state.position.y ||
                      slot.state.health != other->state.health || slot.animating != other->animating ||
                      slot.state.owner != other->state.owner || slot.settlement != other->settlement ||
                      slot.state.holder != other->state.holder || slot.state.stamina != other->state.stamina ||
                      slot.state.user != other->state.user || slot.query != other->query ||
                      sim::pack_sync_flags(slot.state) != sim::pack_sync_flags(other->state) ||
                      (slot.object && other->object && (slot.object->anim.elapsed_ms != other->object->anim.elapsed_ms || slot.object->anim.step != other->object->anim.step || slot.object->anim.anim_slot != other->object->anim.anim_slot || slot.object->anim.state_idx != other->object->anim.state_idx));
          if (diff) {
            std::fprintf(stderr, "object %u: owner %d/%d settlement %u/%u holder %u/%u flags %08x/%08x user %d/%d\n",
              slot.id, (int)slot.state.owner, (int)other->state.owner, slot.settlement, other->settlement,
              slot.state.holder, other->state.holder, sim::pack_sync_flags(slot.state), sim::pack_sync_flags(other->state),
              slot.state.user, other->state.user);
            std::fprintf(stderr, "object %u class %d: pos (%d,%d)/(%d,%d) hp %d/%d animating %d/%d slot %d/%d state %d/%d elapsed %d/%d step %u/%u timeline valid %d/%d\n",
              slot.id, (int)slot.class_index, slot.state.position.x, slot.state.position.y, other->state.position.x, other->state.position.y,
              slot.state.health, other->state.health, (int)slot.animating, (int)other->animating,
              slot.object ? slot.object->anim.anim_slot : -9, other->object ? other->object->anim.anim_slot : -9,
              slot.object ? slot.object->anim.state_idx : -9, other->object ? other->object->anim.state_idx : -9,
              slot.object ? slot.object->anim.elapsed_ms : -9, other->object ? other->object->anim.elapsed_ms : -9,
              slot.object ? slot.object->anim.step : 0u, other->object ? other->object->anim.step : 0u,
              (int)slot.timeline.valid(), (int)other->timeline.valid());
            {
              const sim::MovementSystem* ma = sim::movement_system(const_cast<sim::World&>(original.world()));
              const sim::MovementSystem* mb = sim::movement_system(const_cast<sim::World&>(restored->world()));
              const sim::MoveState* xa = ma ? ma->find(slot.id) : nullptr;
              const sim::MoveState* xb = mb ? mb->find(slot.id) : nullptr;
              std::fprintf(stderr, "  move: has_path %d/%d goto %d/%d target (%d,%d)/(%d,%d) waypoints %zu/%zu\n",
                xa ? (int)xa->has_path : -1, xb ? (int)xb->has_path : -1, xa ? (int)xa->goto_active : -1, xb ? (int)xb->goto_active : -1,
                xa ? xa->target.x : -9, xa ? xa->target.y : -9, xb ? xb->target.x : -9, xb ? xb->target.y : -9,
                xa ? xa->waypoints.size() : 0u, xb ? xb->waypoints.size() : 0u);
              const sim::CommandSystem* ca = sim::command_system(const_cast<sim::World&>(original.world()));
              const sim::CommandSystem* cb = sim::command_system(const_cast<sim::World&>(restored->world()));
              std::fprintf(stderr, "  cmd: %s/%s (%zu/%zu queued)\n",
                ca ? std::string(ca->command_name(slot.id)).c_str() : "?", cb ? std::string(cb->command_name(slot.id)).c_str() : "?",
                ca ? ca->command_count(slot.id) : 0u, cb ? cb->command_count(slot.id) : 0u);
            }
            if (++k > 8) break;
          }
        }
      }
      char buffer[256];
      std::snprintf(buffer, sizeof(buffer),
                    "turn %llu (+%zu, length %d): %s %016llx against %016llx",
                    static_cast<unsigned long long>(a.turns), i + 1, length, channel,
                    static_cast<unsigned long long>(a.slots),
                    static_cast<unsigned long long>(b.slots));
      out.detail = buffer;
      out.trace_agrees = false;
      return out;
    }
  }
  return out;
}

int run(const Args& args) {
  Fixture fixture;
  if (!open_fixture(args.game, args.map, args.map_index, fixture)) return 1;

  std::unique_ptr<sim::GameSession> original =
      start_session(fixture, args.seed, args.scripts);
  if (original == nullptr) return 1;

  std::printf("map       %s%s%s\n", args.map.c_str(),
              args.map_index.empty() ? "" : " Maps/", args.map_index.c_str());
  std::printf("classes   %zu from %zu files\n", fixture.install.classes().size(),
              fixture.install.class_files());
  std::printf("objects   %zu\n", original->world().objects().size());
  std::printf("systems  ");
  for (const sim::System* system : original->world().systems()) {
    std::printf(" %.*s", static_cast<int>(system->name().size()), system->name().data());
  }
  std::printf("\nscripts   %s, %zu running\n", args.scripts ? "on" : "off",
              original->report().scripts_running);

  // Each settlement's owner before any turn runs, so that the mid-war report
  // below can say which towns changed hands.
  std::map<sim::ObjectId, sim::PlayerId> owner_at_start;
  if (const sim::EconomySystem* economy = sim::economy_of(original->world())) {
    for (const sim::Settlement& town : economy->settlements().all()) {
      owner_at_start[town.object] = town.owner;
    }
  }
  for (std::size_t i = 0; i < args.turns; ++i) original->advance(1, schedule_at(i));
  std::printf("advanced  %llu turns, game time %lld\n",
              static_cast<unsigned long long>(original->world().turns()),
              static_cast<long long>(original->world().time()));
  // How many of the AI's settlement nodes outlive their central building. A
  // save taken after one has fallen is the case a rebuild of the node table
  // got wrong -- the node went to the map's corner -- so a check that means
  // to cover it says whether it did.
  //
  // Damage no longer takes a central building away -- one at no health stands
  // broken, as `gbr.exe`'s does -- so on a shipped map that count is a script
  // `Erase` or nothing. What war does leave is the next two lines: a centre
  // standing broken (tier 3, the `IsBroken` state the save has to carry) and
  // a settlement in other hands than the map gave it. A mid-war check asks
  // for one of the three.
  {
    std::size_t fallen = 0;
    std::size_t broken = 0;
    std::size_t taken = 0;
    sim::World& world = original->world();
    const sim::EconomySystem* economy = sim::economy_of(world);
    for (const sim::GaikaNode& node : world.gaika().nodes()) {
      if (node.settlement == sim::kNoObject || economy == nullptr) continue;
      for (const sim::Settlement& town : economy->settlements().all()) {
        if (town.object == node.settlement && world.find(town.anchor) == nullptr) ++fallen;
      }
    }
    if (economy != nullptr) {
      for (const sim::Settlement& town : economy->settlements().all()) {
        const sim::WorldObject* centre = world.find(town.anchor);
        if (centre != nullptr && centre->state.damage_state == 3) ++broken;
        const auto was = owner_at_start.find(town.object);
        if (was != owner_at_start.end() && was->second != town.owner) ++taken;
      }
    }
    std::printf("fallen    %zu settlement node(s) outlive their central building\n", fallen);
    std::printf("broken    %zu central building(s) stand broken\n", broken);
    std::printf("taken     %zu settlement(s) in other hands than the map's\n\n", taken);
  }

  const Result<std::vector<std::byte>> saved = original->save(fixture.identity);
  if (!saved.ok()) {
    std::fprintf(stderr, "save refused: error %d\n", static_cast<int>(saved.error()));
    return 1;
  }
  // The writer is deterministic by construction -- no map, no set, no
  // length-dependent ordering -- and this is the assertion of it.
  const Result<std::vector<std::byte>> twice = original->save(fixture.identity);
  if (!twice.ok() || twice.value() != saved.value()) {
    std::fprintf(stderr, "two saves of one session differ\n");
    return 1;
  }
  std::printf("saved     %zu bytes\n", saved.value().size());
  if (!args.out.empty()) {
    // The file the application and `imrun --load` read: the envelope beside
    // a manifest naming the container, the map and the seed, in a `.bfhp`.
    // Written before the round trip so that a failing round trip still
    // leaves the file that failed it for `imrun --load` to look at.
    sim::SaveFileContents contents;
    contents.manifest.container = gamedata::container_relative(args.game, args.map);
    contents.manifest.map_index = gamedata::map_number(fixture.payloads.map_directory);
    contents.manifest.seed = args.seed;
    contents.manifest.turns = original->world().turns();
    contents.manifest.time = original->world().time();
    contents.manifest.engine = version_string();
    contents.session = saved.value();
    std::string error;
    if (!gamedata::write_save_file(args.out, contents, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("wrote     %s (%s)\n", args.out.c_str(), fixture.identity.c_str());
  }

  // The original has to be re-run from the same point for each round trip, so
  // it is rebuilt rather than reused once faults are involved.
  const auto fresh_original = [&]() -> std::unique_ptr<sim::GameSession> {
    std::unique_ptr<sim::GameSession> run = start_session(fixture, args.seed, args.scripts);
    if (run == nullptr) return nullptr;
    for (std::size_t i = 0; i < args.turns; ++i) run->advance(1, schedule_at(i));
    return run;
  };

  int failures = 0;
  {
    std::unique_ptr<sim::GameSession> reference = fresh_original();
    if (reference == nullptr) return 1;
    // The reference is a second process-local run of the same seed, and check
    // 3 compares the restored session against *it*, not against the session
    // that wrote the save. If the two runs disagree, every divergence check 3
    // reports is the engine's own nondeterminism wearing the save's clothes,
    // and the next reader would chase it through the loader. So the reference
    // must save byte-for-byte what the first run saved, and a mismatch is
    // named as what it is. (Asked of Balcans while its divergence was open, it
    // held -- which is what sent the search to the load side, where the script
    // watermark was.)
    const Result<std::vector<std::byte>> rerun = reference->save(fixture.identity);
    if (!rerun.ok() || rerun.value() != saved.value()) {
      char buffer[256];
      if (rerun.ok() && rerun.value().size() == saved.value().size()) {
        const auto [a, b] = std::mismatch(rerun.value().begin(), rerun.value().end(),
                                          saved.value().begin());
        const std::size_t at = static_cast<std::size_t>(a - rerun.value().begin());
        std::snprintf(buffer, sizeof(buffer),
                      "differs at byte %zu of %zu, in section '%s'", at,
                      saved.value().size(), section_at(saved.value(), at).c_str());
      } else {
        std::snprintf(buffer, sizeof(buffer), "is %zu bytes against %zu",
                      rerun.ok() ? rerun.value().size() : 0u, saved.value().size());
      }
      std::printf("\nROUND TRIP FAILED (two runs of one seed are not one game)\n"
                  "  a second session from seed %llu saved a different game: %s\n",
                  static_cast<unsigned long long>(args.seed), buffer);
      return 1;
    }
    const Outcome outcome =
        round_trip(fixture, args, *reference, saved.value(), saved.value(),
                   /*verbose=*/true);
    if (outcome.clean()) {
      std::printf("  hashes    match at every one of the %zu turns after the load\n",
                  args.more);
      std::printf("  re-save   byte-identical\n");
      std::printf("\nROUND TRIP OK\n");
    } else {
      std::printf("\nROUND TRIP FAILED (%s)\n  %s\n", outcome.caught_by(),
                  outcome.detail.c_str());
      ++failures;
    }
  }

  if (args.faults) {
    // A check that has never been seen to fail is not a check. Each system's
    // section is deleted in turn and every one of them must be noticed.
    std::printf("\nfault injection: delete one system's section and require a catch\n");
    // What the same session looks like before any turn has run. A system whose
    // section is unchanged from this has nothing to lose here; see
    // `has_state_to_lose`.
    std::unique_ptr<sim::GameSession> zero = start_session(fixture, args.seed, args.scripts);
    if (zero == nullptr) return 1;
    const Result<std::vector<std::byte>> at_zero = zero->save(fixture.identity);
    if (!at_zero.ok()) return 1;

    for (const std::string_view name : sim::kSystemOrder) {
      if (!has_state_to_lose(at_zero.value(), saved.value(), name)) {
        std::printf("  %-9.*s  nothing to lose: unchanged from turn zero on this map\n",
                    static_cast<int>(name.size()), name.data());
        continue;
      }
      const std::vector<std::byte> broken = without_system(saved.value(), name);
      if (broken.empty()) {
        std::printf("  %-9.*s  COULD NOT BUILD THE FAULT\n", static_cast<int>(name.size()),
                    name.data());
        ++failures;
        continue;
      }
      std::unique_ptr<sim::GameSession> reference = fresh_original();
      if (reference == nullptr) return 1;
      const Outcome outcome = round_trip(fixture, args, *reference, broken, saved.value(),
                                         /*verbose=*/false);
      if (outcome.clean()) {
        std::printf("  %-9.*s  NOT CAUGHT -- the save does not carry this system\n",
                    static_cast<int>(name.size()), name.data());
        ++failures;
      } else {
        std::printf("  %-9.*s  caught by %s\n", static_cast<int>(name.size()), name.data(),
                    outcome.caught_by());
      }
    }
  }

  if (failures != 0) std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}

void usage() {
  std::fprintf(stderr,
               "usage: imsave <game-dir> <map.bfhp> [options]\n"
               "\n"
               "Runs a real session, saves it, loads it into a fresh session and\n"
               "requires the two to be the same game -- at every turn, not just the\n"
               "last, and byte for byte on a re-save.\n"
               "\n"
               "  --map-index N    which Maps/<n> in the container (default the first)\n"
               "  --turns N        turns to run before saving (default 40)\n"
               "  --more N         turns to run on both sides afterwards (default 20)\n"
               "  --seed N         world seed (default 1)\n"
               "  --no-scripts     run the systems with the script VM switched off\n"
               "  --verify-faults  also delete each system's section in turn and\n"
               "                   require every one of them to be caught\n"
               "  --out FILE       also write the save as a file -- the container\n"
               "                   the app and `imrun --load` read\n"
               "\nIMSAVE_DIFF=1 in the environment names the first objects whose state\n"
               "differs when the hash trace diverges, field by field.\n"
               "IMSAVE_TRACE=<text> prints every host call made after the load by a\n"
               "script whose source name contains <text>, on both sides, tagged A\n"
               "(the original) and B (the restored); IMSAVE_TRACE_LIMIT caps the\n"
               "lines per side (default 2000). Diff the two tags.\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  Args args;
  args.game = argv[1];
  args.map = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto text = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    const auto value = [&](std::size_t fallback) -> std::size_t {
      return i + 1 < argc ? static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10))
                          : fallback;
    };
    if (flag == "--map-index") args.map_index = text();
    else if (flag == "--turns") args.turns = value(args.turns);
    else if (flag == "--more") args.more = value(args.more);
    else if (flag == "--seed") args.seed = static_cast<std::uint32_t>(value(args.seed));
    else if (flag == "--no-scripts") args.scripts = false;
    else if (flag == "--verify-faults") args.faults = true;
    else if (flag == "--out") args.out = text();
    else {
      usage();
      return 2;
    }
  }
  return run(args);
}
