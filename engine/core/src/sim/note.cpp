// The mission notes a campaign pins on the player's list.
// See include/imperivm/core/sim/note.hpp.

#include "imperivm/core/sim/note.hpp"

#include <algorithm>

#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core::sim {

namespace {

constexpr std::uint32_t kNotesMagic = 0x53544F4Eu;  // "NOTS"
constexpr std::uint32_t kNotesVersion = 1;

/// FNV-1a, the same constants every other store in this tree folds with.
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix(std::uint64_t& accumulator, std::string_view text) noexcept {
  for (const char c : text) {
    accumulator ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    accumulator *= kFnvPrime;
  }
  // A separator, so that {"ab", "c"} and {"a", "bc"} are different states.
  accumulator ^= 0xFFull;
  accumulator *= kFnvPrime;
}

}  // namespace

// --------------------------------------------------------------------------
// the catalogue
// --------------------------------------------------------------------------

Result<std::size_t> NoteCatalogue::add(std::span<const std::byte> xml) {
  // Empty is not an error: 36 of the installation's 50 note documents declare
  // nothing at all, and a container with no notes is ordinary.
  if (xml.empty()) return std::size_t{0};

  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode || doc->node(root).name != "notes") return FormatError::bad_magic;

  std::size_t added = 0;
  for (NodeIndex i = doc->child(root, "note"); i != kNoNode; i = doc->next(i, "note")) {
    NoteDefinition note;
    note.id = std::string(doc->attribute(i, "id"));
    // Nothing could ever name it, and an empty key would collide with every
    // other `<note>` that forgot one.
    if (note.id.empty()) continue;
    // First in wins, which is what a `std::map` insert does with a key it
    // already holds -- and what makes the map's document beat the container's
    // when both are added, map first.
    if (find(note.id) != nullptr) continue;

    note.title = std::string(doc->attribute(i, "title"));
    note.text = std::string(doc->attribute(i, "text"));
    note.icon = std::string(doc->attribute(i, "icon"));
    note.map = std::string(doc->attribute(i, "map"));
    note.show_on_minimap = doc->attribute_int(i, "show_on_minimap", 0) != 0;
    note.location.x = doc->attribute_int(i, "locationx", -1);
    note.location.y = doc->attribute_int(i, "locationy", -1);
    notes_.push_back(std::move(note));
    ++added;
  }
  return added;
}

const NoteDefinition* NoteCatalogue::find(std::string_view id) const noexcept {
  // A linear scan in declaration order. 122 declarations across the whole
  // installation and at most 24 in one container, so the sorted-vector shape
  // the boards and stores use would cost more in bookkeeping than it saves --
  // and declaration order is what `all()` has to yield.
  for (const NoteDefinition& note : notes_) {
    if (note.id == id) return &note;
  }
  return nullptr;
}

// --------------------------------------------------------------------------
// the board
// --------------------------------------------------------------------------

namespace {

[[nodiscard]] std::vector<std::string>::const_iterator lower(
    const std::vector<std::string>& active, std::string_view id) {
  return std::lower_bound(active.begin(), active.end(), id,
                          [](const std::string& row, std::string_view probe) {
                            return row < probe;
                          });
}

}  // namespace

bool NoteBoard::give(std::string_view id) {
  const auto at = lower(active_, id);
  if (at != active_.end() && *at == id) return false;
  active_.insert(active_.begin() + (at - active_.begin()), std::string(id));
  return true;
}

void NoteBoard::clear() noexcept { active_.clear(); }

bool NoteBoard::remove(std::string_view id) {
  const auto at = lower(active_, id);
  if (at == active_.end() || *at != id) return false;
  active_.erase(active_.begin() + (at - active_.begin()));
  return true;
}

bool NoteBoard::active(std::string_view id) const noexcept {
  const auto at = lower(active_, id);
  return at != active_.end() && *at == id;
}

void NoteBoard::hash(std::uint64_t& accumulator) const noexcept {
  for (const std::string& id : active_) mix(accumulator, id);
}

void NoteBoard::serialize(std::vector<std::byte>& out) const {
  bytes::put_u32(out, kNotesMagic);
  bytes::put_u32(out, kNotesVersion);
  bytes::put_u32(out, static_cast<std::uint32_t>(active_.size()));
  for (const std::string& id : active_) bytes::put_string(out, id);
}

Status NoteBoard::deserialize(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kNotesMagic) return FormatError::bad_magic;
  if (version != kNotesVersion) return FormatError::unsupported;
  if (!reader.u32(count)) return FormatError::truncated;

  std::vector<std::string> active;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string id;
    if (!bytes::get_string(reader, id)) return FormatError::truncated;
    // Strictly ascending, which `active` binary-searches on.
    if (i != 0 && !(active.back() < id)) return FormatError::malformed;
    active.push_back(std::move(id));
  }
  if (reader.remaining() != 0) return FormatError::malformed;
  active_ = std::move(active);
  return Status();
}

}  // namespace imperivm::core::sim
