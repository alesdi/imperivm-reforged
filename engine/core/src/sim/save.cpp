// The saved game: the section container, its reader and its writer.
// See include/imperivm/core/sim/save.hpp and docs/formats/save.md.

#include "imperivm/core/sim/save.hpp"

#include <algorithm>

#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/orders.hpp"

namespace imperivm::core::sim {

namespace bytes {

bool get_u64(ByteReader& reader, std::uint64_t& out) noexcept {
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  if (!reader.u32(low) || !reader.u32(high)) return false;
  out = (static_cast<std::uint64_t>(high) << 32) | low;
  return true;
}

bool get_i64(ByteReader& reader, std::int64_t& out) noexcept {
  std::uint64_t raw = 0;
  if (!get_u64(reader, raw)) return false;
  out = static_cast<std::int64_t>(raw);
  return true;
}

bool get_i32(ByteReader& reader, std::int32_t& out) noexcept {
  std::uint32_t raw = 0;
  if (!reader.u32(raw)) return false;
  out = static_cast<std::int32_t>(raw);
  return true;
}

bool get_string(ByteReader& reader, std::string& out) {
  std::uint32_t length = 0;
  if (!reader.u32(length)) return false;
  std::span<const std::byte> raw;
  if (!reader.bytes(length, raw)) return false;
  out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
  return true;
}

}  // namespace bytes

namespace {

/// The longest a section name may be. One byte holds the length, and a name is
/// a fixed vocabulary plus a system name, none of which is close.
constexpr std::size_t kMaxSectionName = 255;

/// Printable ASCII only. A section name ends up in a report and in a
/// diagnostic; a control byte in one is a terminal that stops echoing.
[[nodiscard]] bool name_is_clean(std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaxSectionName) return false;
  for (const char c : name) {
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

void put_meta(std::vector<std::byte>& out, const SaveMeta& meta) {
  bytes::put_string(out, meta.map);
  bytes::put_u64(out, meta.turns);
  bytes::put_i64(out, meta.time);
  // The four channels that are ever non-zero across the nine dumps. The other
  // four are reserved zero (`conformance::channel_is_reserved_zero`) and are
  // not encoded at all -- a field whose only legal value is zero is an
  // invitation -- and `hash_of_hashes` is a pure function of these four.
  bytes::put_u64(out, meta.slots);
  bytes::put_u64(out, meta.threads);
  bytes::put_u64(out, meta.netcmds);
  bytes::put_u64(out, meta.extrahash);
  bytes::put_u32(out, static_cast<std::uint32_t>(meta.systems.size()));
  for (const std::string& name : meta.systems) bytes::put_string(out, name);
}

[[nodiscard]] Status get_meta(std::span<const std::byte> payload, SaveMeta& meta) {
  ByteReader reader(payload);
  std::uint32_t systems = 0;
  if (!bytes::get_string(reader, meta.map) || !bytes::get_u64(reader, meta.turns) ||
      !bytes::get_i64(reader, meta.time) || !bytes::get_u64(reader, meta.slots) ||
      !bytes::get_u64(reader, meta.threads) || !bytes::get_u64(reader, meta.netcmds) ||
      !bytes::get_u64(reader, meta.extrahash) || !reader.u32(systems)) {
    return FormatError::truncated;
  }
  meta.systems.clear();
  for (std::uint32_t i = 0; i < systems; ++i) {
    std::string name;
    if (!bytes::get_string(reader, name)) return FormatError::truncated;
    meta.systems.push_back(std::move(name));
  }
  if (reader.remaining() != 0) return FormatError::malformed;
  return Status();
}

/// The meta section's name. Written first, always: a reader must be able to
/// refuse a save on version or map before it decodes an object table.
constexpr std::string_view kMetaSection = "meta";

}  // namespace

std::string system_section_name(std::string_view name) {
  std::string out(kSystemSectionPrefix);
  out.append(name);
  return out;
}

// --------------------------------------------------------------------------
// writing
// --------------------------------------------------------------------------

Status SaveWriter::add(std::string_view name, std::span<const std::byte> payload) {
  if (!name_is_clean(name)) return FormatError::malformed;
  if (name == kMetaSection) return FormatError::malformed;  // the writer owns it
  if (payload.size() > kMaxSectionBytes) return FormatError::buffer_too_small;
  for (const SaveSection& section : sections_) {
    // Two sections of one name would make `SaveReader::section` answer with
    // whichever the lookup happened to reach first, which is a coin toss the
    // rest of the load would believe.
    if (section.name == name) return FormatError::malformed;
  }
  SaveSection section;
  section.name.assign(name);
  section.payload.assign(payload.begin(), payload.end());
  sections_.push_back(std::move(section));
  return Status();
}

std::vector<std::byte> SaveWriter::finish() const {
  std::vector<std::byte> meta;
  put_meta(meta, meta_);

  std::vector<std::byte> out;
  bytes::put_u32(out, kSaveMagic);
  bytes::put_u32(out, kSaveFormatVersion);
  bytes::put_u32(out, meta_.state_vector);
  bytes::put_u32(out, static_cast<std::uint32_t>(sections_.size() + 1));

  const auto emit = [&out](std::string_view name, std::span<const std::byte> payload) {
    bytes::put_u8(out, static_cast<std::uint32_t>(name.size()));
    for (const char c : name) bytes::put_u8(out, static_cast<std::uint8_t>(c));
    bytes::put_u32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
  };

  emit(kMetaSection, meta);
  for (const SaveSection& section : sections_) emit(section.name, section.payload);
  return out;
}

Result<SaveReport> write_save(const World& world, const SaveInputs& inputs,
                              std::vector<std::byte>& out) {
  const WorldHashes hashes = world.hashes();

  SaveMeta meta;
  meta.state_vector = kStateVectorVersion;
  meta.map.assign(inputs.map);
  meta.turns = world.turns();
  meta.time = world.time();
  meta.slots = hashes.slots;
  meta.threads = hashes.threads;
  meta.netcmds = hashes.netcmds;
  meta.extrahash = hashes.extrahash;
  for (const System* system : world.systems()) {
    meta.systems.emplace_back(system == nullptr ? std::string_view() : system->name());
  }

  SaveWriter writer(std::move(meta));
  SaveReport report;

  std::vector<std::byte> payload;
  world.serialize(payload);
  if (const Status status = writer.add(kWorldSection, payload); !status.ok()) {
    return status.error();
  }

  if (inputs.scheduler != nullptr) {
    payload.clear();
    inputs.scheduler->serialize(payload);
    if (const Status status = writer.add(kScriptSection, payload); !status.ok()) {
      return status.error();
    }
  }
  if (inputs.selections != nullptr) {
    payload.clear();
    inputs.selections->serialize(payload);
    if (const Status status = writer.add(kSelectionSection, payload); !status.ok()) {
      return status.error();
    }
  }
  if (!inputs.session.empty()) {
    if (const Status status = writer.add(kSessionSection, inputs.session); !status.ok()) {
      return status.error();
    }
  }

  // System sections in the order the caller listed them, which the caller is
  // free to make registration order and this file has no business reordering.
  for (const SaveSection& section : inputs.systems) {
    const std::string name = system_section_name(section.name);
    if (const Status status = writer.add(name, section.payload); !status.ok()) {
      return status.error();
    }
  }

  // Every registered system with no section is state this save does not carry.
  // Reported, not swallowed: it is the difference between a save that reloads
  // into the same game and one that reloads into a similar one.
  for (const System* system : world.systems()) {
    if (system == nullptr) continue;
    const std::string_view name = system->name();
    const bool saved = std::any_of(
        inputs.systems.begin(), inputs.systems.end(),
        [name](const SaveSection& section) { return section.name == name; });
    if (!saved) report.unsaved_systems.emplace_back(name);
  }

  out = writer.finish();
  report.bytes = out.size();
  return report;
}

// --------------------------------------------------------------------------
// reading
// --------------------------------------------------------------------------

std::string save_version_refusal(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  std::uint32_t magic = 0;
  std::uint32_t format = 0;
  std::uint32_t state_vector = 0;
  if (!reader.u32(magic) || !reader.u32(format) || !reader.u32(state_vector)) return {};
  if (magic != kSaveMagic) return {};
  if (format != kSaveFormatVersion) {
    return "written with save format " + std::to_string(format) + "; this build reads save format " +
           std::to_string(kSaveFormatVersion);
  }
  if (state_vector != kStateVectorVersion) {
    return "written with state vector " + std::to_string(state_vector) +
           "; this build reads state vector " + std::to_string(kStateVectorVersion);
  }
  return {};
}

Result<SaveReader> SaveReader::open(std::span<const std::byte> data) {
  ByteReader reader(data);
  std::uint32_t magic = 0;
  std::uint32_t format = 0;
  std::uint32_t state_vector = 0;
  std::uint32_t count = 0;
  if (!reader.u32(magic) || !reader.u32(format) || !reader.u32(state_vector) ||
      !reader.u32(count)) {
    return FormatError::truncated;
  }
  if (magic != kSaveMagic) return FormatError::bad_magic;
  // Two rejections, two meanings. A different framing version and a different
  // state vector are both `unsupported`, and both are refusals rather than
  // best-effort loads: the whole point of the number is that a build whose
  // state vector moved must not load an old save into something subtly wrong.
  if (format != kSaveFormatVersion) return FormatError::unsupported;
  if (state_vector != kStateVectorVersion) return FormatError::unsupported;

  SaveReader out;
  out.meta_.state_vector = state_vector;
  bool seen_meta = false;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t length = 0;
    std::span<const std::byte> name_bytes;
    std::uint32_t payload_length = 0;
    std::span<const std::byte> payload;
    if (!reader.u8(length) || !reader.bytes(length, name_bytes) ||
        !reader.u32(payload_length)) {
      return FormatError::truncated;
    }
    if (payload_length > kMaxSectionBytes) return FormatError::malformed;
    if (!reader.bytes(payload_length, payload)) return FormatError::truncated;

    std::string name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
    if (!name_is_clean(name)) return FormatError::malformed;
    if (std::find(out.names_.begin(), out.names_.end(), name) != out.names_.end()) {
      return FormatError::malformed;
    }

    if (name == kMetaSection) {
      // First, always: a reader that hit the meta block third would have
      // already decoded two sections it might have refused outright.
      if (i != 0) return FormatError::malformed;
      const std::uint32_t declared = out.meta_.state_vector;
      if (const Status status = get_meta(payload, out.meta_); !status.ok()) {
        return status.error();
      }
      out.meta_.state_vector = declared;
      seen_meta = true;
    }
    out.names_.push_back(std::move(name));
    out.payloads_.push_back(payload);
  }
  if (!seen_meta) return FormatError::malformed;
  // Trailing bytes mean the section count and the file disagree. A save that
  // half-parses is a save that loads for the wrong reason.
  if (reader.remaining() != 0) return FormatError::malformed;
  return out;
}

std::span<const std::byte> SaveReader::section(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < names_.size(); ++i) {
    if (names_[i] == name) return payloads_[i];
  }
  return {};
}

bool SaveReader::has(std::string_view name) const noexcept {
  return std::find(names_.begin(), names_.end(), name) != names_.end();
}

std::span<const std::byte> SaveReader::system_section(std::string_view name) const noexcept {
  return section(system_section_name(name));
}

Result<LoadReport> read_save(std::span<const std::byte> data, World& world,
                             const LoadOptions& options) {
  Result<SaveReader> reader = SaveReader::open(data);
  if (!reader.ok()) return reader.error();
  return read_save(reader.value(), world, options);
}

Result<LoadReport> read_save(const SaveReader& reader, World& world,
                             const LoadOptions& options) {
  const SaveMeta& meta = reader.meta();

  // The original refuses the same way, and says so: "The map you played this
  // game on when the game was saved (%s1) is missing or has been changed".
  if (!options.map.empty() && options.map != meta.map) return FormatError::not_found;

  if (options.require_same_pipeline) {
    const std::span<System* const> systems = world.systems();
    if (systems.size() != meta.systems.size()) return FormatError::unsupported;
    for (std::size_t i = 0; i < systems.size(); ++i) {
      const System* system = systems[i];
      if (system == nullptr || system->name() != meta.systems[i]) {
        return FormatError::unsupported;
      }
    }
  }

  const std::span<const std::byte> world_bytes = reader.section(kWorldSection);
  if (world_bytes.empty()) return FormatError::not_found;
  if (const Status status = world.deserialize(world_bytes, options.entities); !status.ok()) {
    return status.error();
  }

  LoadReport report;
  report.meta = meta;
  report.objects = world.size();

  std::vector<std::string> consumed{std::string(kMetaSection), std::string(kWorldSection)};

  // A save with a script section and no scheduler to put it in is not an
  // error -- a headless systems-only run legitimately has none -- but it is
  // not silent either: the section stays unconsumed and is reported, because a
  // game reloaded without its suspended scripts is a different game.
  if (reader.has(kScriptSection) && options.scheduler != nullptr) {
    const Status status = options.scheduler->deserialize(reader.section(kScriptSection));
    if (!status.ok()) return status.error();
    consumed.emplace_back(kScriptSection);
  }
  if (reader.has(kSelectionSection) && options.selections != nullptr) {
    const Status status = options.selections->deserialize(reader.section(kSelectionSection));
    if (!status.ok()) return status.error();
    consumed.emplace_back(kSelectionSection);
  }

  // System sections are not applied here: this cannot reach inside a `System`.
  // They are named so the caller can route each one, and every one the caller
  // has not been told about is reported rather than dropped.
  for (const System* system : world.systems()) {
    if (system == nullptr) continue;
    const std::string name = system_section_name(system->name());
    if (reader.has(name)) {
      consumed.push_back(name);
    } else {
      report.unrestored_systems.emplace_back(system->name());
    }
  }

  for (const std::string& name : reader.names()) {
    if (std::find(consumed.begin(), consumed.end(), name) == consumed.end()) {
      report.unconsumed.push_back(name);
    }
  }
  return report;
}

HashMismatch verify_hashes(const World& world, const SaveMeta& meta) noexcept {
  const WorldHashes actual = world.hashes();
  const struct {
    std::string_view name;
    std::uint64_t expected;
    std::uint64_t actual;
  } channels[] = {
      // In `conformance::Channel` order, so that `slots` -- the channel that
      // carries the real answer in all nine dumps -- is reported ahead of
      // anything it caused.
      {"slots", meta.slots, actual.slots},
      {"threads", meta.threads, actual.threads},
      {"netcmds", meta.netcmds, actual.netcmds},
      {"extrahash", meta.extrahash, actual.extrahash},
  };
  for (const auto& channel : channels) {
    if (channel.expected != channel.actual) {
      return HashMismatch{false, channel.name, channel.expected, channel.actual};
    }
  }
  return HashMismatch{};
}

}  // namespace imperivm::core::sim
