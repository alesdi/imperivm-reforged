#pragma once

// The saved game as it sits on disk: a `.bfhp` container holding the session's
// bytes beside a manifest that says which game they belong to.
//
// ## Why a container and not the bytes
//
// `GameSession::save` produces the `ISAV` envelope (`sim/save.hpp`), which is
// the whole of the *simulation's* state and nothing else -- by design: it is
// applied to a session built from the same game data, so it does not carry
// the game data. What it therefore cannot say is *which* game data: a loader
// holding only the envelope knows the map's spelling from `SaveMeta::map` and
// nothing about where the container is, which `Maps/<n>` inside it was
// played, or what seed the world was built from. Those are the manifest.
//
// The original's shape is the same. `gbr.exe` mounts `currentadv.bfhp` as the
// virtual folder `AdvSave/` while an adventure runs and persists the whole
// mounted filesystem (`docs/formats/adventure.md`), so a save there is a small
// filesystem rather than one record -- and the conquest keeps a second mount,
// `ConquestTempFolder/`, for the `territories.xml` it rewrites between
// missions. A container is what lets this engine's save carry that document
// beside the session, in the place the original keeps it.
//
// ## Layout
//
// A 512-byte-block container (`formats/bfhp.hpp`), entries created in this
// order, which fixes the block layout and is what `tests/test_corpus_imsave.py`
// rebuilds with the Python reference writer to hold the two writers to each
// other:
//
//     save.ini          the manifest, below
//     session.isav      `GameSession::save`'s bytes, verbatim
//
// Nothing else. The campaign's progress is in the envelope -- the campaign
// system's own section -- so a save of a conquest mission carries it without
// a document of its own; what crosses *between* missions is `sim/campaign.hpp`'s
// `CampaignCarry`, a separate file, because it outlives every session.
//
// The manifest is an INI document, the shipped configuration format:
//
//     [Save]
//     container=Scenarios/Balcans.BFHP     installation-relative, `/`-separated
//     map=                                 `Maps/<n>`'s number, or empty for the first
//     seed=1                               `World` seed the session was built from
//     turns=40                             `SaveMeta::turns`, for a browser
//     time=19995                           `SaveMeta::time`, likewise
//     engine=0.1.0                         who wrote it
//
// `turns` and `time` duplicate the envelope's meta on purpose and for the
// reason the envelope duplicates the world's: a save browser lists a slot
// without decoding the session. They are not authoritative; the envelope is.
//
// Nothing here touches a file. `engine/gamedata/save_file.hpp` puts the bytes
// on disk and resolves the manifest's container against an installation.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::sim {

inline constexpr std::string_view kSaveManifestEntry = "save.ini";
inline constexpr std::string_view kSaveSessionEntry = "session.isav";

/// What a save on disk says about the game it belongs to.
struct SaveFileManifest {
  /// The map container, relative to the installation root, `/`-separated:
  /// `Scenarios/Balcans.BFHP`, `Conquests/mediterranean.bfhp`.
  std::string container;
  /// The `Maps/<n>` number played, as the digits, or empty for the first map
  /// in walk order -- the spelling `read_payloads` takes.
  std::string map_index;
  std::uint32_t seed = 1;
  std::uint64_t turns = 0;
  std::int64_t time = 0;
  std::string engine;

  [[nodiscard]] bool operator==(const SaveFileManifest&) const = default;
};

/// The contents of one save file.
struct SaveFileContents {
  SaveFileManifest manifest;
  /// `GameSession::save`'s bytes.
  std::vector<std::byte> session;
};

/// The manifest as the INI text `save.ini` holds.
[[nodiscard]] std::vector<std::byte> encode_save_manifest(const SaveFileManifest& manifest);
/// Parse `save.ini`. Refuses a document without a `[Save]` section or a
/// `container` key, and a numeric field that is not entirely a number.
[[nodiscard]] Result<SaveFileManifest> decode_save_manifest(std::span<const std::byte> ini);

/// The whole file: manifest and session in a container.
[[nodiscard]] std::vector<std::byte> encode_save_file(const SaveFileContents& contents);
/// Open a file this engine wrote. Refuses a container without both the
/// manifest and the session, or one that is not a container at all.
[[nodiscard]] Result<SaveFileContents> decode_save_file(std::span<const std::byte> bytes);

}  // namespace imperivm::core::sim
