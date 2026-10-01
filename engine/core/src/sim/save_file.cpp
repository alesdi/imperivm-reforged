#include "imperivm/core/sim/save_file.hpp"

#include <charconv>
#include <string>

#include "imperivm/core/formats/bfhp.hpp"
#include "imperivm/core/formats/ini.hpp"

namespace imperivm::core::sim {
namespace {

void put_text(std::vector<std::byte>& out, std::string_view text) {
  for (const char c : text) out.push_back(static_cast<std::byte>(c));
}

/// `std::from_chars` over the whole value: a trailing character is a refusal,
/// for the reason `IniDocument::value_int` gives -- a number read half-way is
/// how `ProductionInterval` came to be 20 when it is 2000.
template <class T>
[[nodiscard]] bool whole_number(std::string_view text, T& out) noexcept {
  if (text.empty()) return false;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const auto [end, ec] = std::from_chars(first, last, out);
  return ec == std::errc() && end == last;
}

}  // namespace

std::vector<std::byte> encode_save_manifest(const SaveFileManifest& manifest) {
  std::vector<std::byte> out;
  put_text(out, "[Save]\r\n");
  put_text(out, "container=" + manifest.container + "\r\n");
  put_text(out, "map=" + manifest.map_index + "\r\n");
  put_text(out, "seed=" + std::to_string(manifest.seed) + "\r\n");
  put_text(out, "turns=" + std::to_string(manifest.turns) + "\r\n");
  put_text(out, "time=" + std::to_string(manifest.time) + "\r\n");
  put_text(out, "engine=" + manifest.engine + "\r\n");
  return out;
}

Result<SaveFileManifest> decode_save_manifest(std::span<const std::byte> ini) {
  const Result<IniDocument> document = IniDocument::parse(ini);
  if (!document.ok()) return document.error();
  const SectionIndex section = document->section("Save");
  if (section == kNoSection) return FormatError::malformed;

  SaveFileManifest manifest;
  manifest.container.assign(document->value(section, "container"));
  if (manifest.container.empty()) return FormatError::malformed;
  manifest.map_index.assign(document->value(section, "map"));
  manifest.engine.assign(document->value(section, "engine"));
  // Absent is the default; present and not a number is a refusal.
  const std::string_view seed = document->value(section, "seed");
  if (!seed.empty() && !whole_number(seed, manifest.seed)) return FormatError::malformed;
  const std::string_view turns = document->value(section, "turns");
  if (!turns.empty() && !whole_number(turns, manifest.turns)) return FormatError::malformed;
  const std::string_view time = document->value(section, "time");
  if (!time.empty() && !whole_number(time, manifest.time)) return FormatError::malformed;
  return manifest;
}

std::vector<std::byte> encode_save_file(const SaveFileContents& contents) {
  // Creation order is the block layout; see the header. The builder cannot
  // refuse either of these: the names are fixed, distinct and at the root.
  BlockFileBuilder builder;
  (void)builder.file(kSaveManifestEntry, encode_save_manifest(contents.manifest));
  (void)builder.file(kSaveSessionEntry, contents.session);
  return builder.build();
}

Result<SaveFileContents> decode_save_file(std::span<const std::byte> bytes) {
  const Result<BlockFile> file = BlockFile::open(bytes);
  if (!file.ok()) return file.error();
  if (const Status status = file->validate(); !status.ok()) return status.error();
  const Result<BlockFileIndex> index = BlockFileIndex::build(*file);
  if (!index.ok()) return index.error();

  const auto read = [&](std::string_view name, std::vector<std::byte>& out) -> Status {
    const Result<BlockFileIndex::Entry> entry = index->find(name);
    if (!entry.ok()) return entry.error();
    out.assign(entry->size, std::byte{0});
    const Result<std::size_t> got = index->read(*entry, out);
    if (!got.ok()) return got.error();
    if (*got != entry->size) return Status(FormatError::truncated);
    return Status();
  };

  SaveFileContents contents;
  std::vector<std::byte> manifest;
  if (const Status status = read(kSaveManifestEntry, manifest); !status.ok()) {
    return status.error() == FormatError::not_found ? FormatError::malformed : status.error();
  }
  const Result<SaveFileManifest> parsed = decode_save_manifest(manifest);
  if (!parsed.ok()) return parsed.error();
  contents.manifest = *parsed;
  if (const Status status = read(kSaveSessionEntry, contents.session); !status.ok()) {
    return status.error() == FormatError::not_found ? FormatError::malformed : status.error();
  }
  return contents;
}

}  // namespace imperivm::core::sim
