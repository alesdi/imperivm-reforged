#pragma once

// The saved game on disk, for the tools and the application.
//
// `core/sim/save_file.hpp` defines the bytes -- a container holding the
// session beside a manifest -- and cannot touch a file. This is the small
// part that can: write the bytes to a path, read them back, and turn the
// manifest's installation-relative container spelling into a path and back.
//
// Where a save lives is the caller's choice. The original keeps its slots
// inside the installation (`AdvSaveGame/`, `docs/formats/adventure.md`), and
// the application follows it with `<game>/Saves/`; the tools take a path.

#include <filesystem>
#include <string>
#include <string_view>

#include "imperivm/core/sim/save_file.hpp"

namespace imperivm::gamedata {

/// Write `contents` as a container at `path`, creating the directory above
/// it. `error` gets a sentence on failure.
bool write_save_file(const std::filesystem::path& path,
                     const core::sim::SaveFileContents& contents, std::string* error = nullptr);

/// Read a container this engine wrote. `error` gets a sentence on failure,
/// naming the file and what was wrong with it.
bool read_save_file(const std::filesystem::path& path, core::sim::SaveFileContents& out,
                    std::string* error = nullptr);

/// The manifest's spelling of a container: relative to the installation root,
/// `/`-separated. A container outside the installation is spelled absolute,
/// which the manifest carries as well -- a save of a map on the desktop is a
/// save all the same.
[[nodiscard]] std::string container_relative(const std::filesystem::path& game_dir,
                                             const std::filesystem::path& container);

/// The path a manifest's `container` names, against `game_dir`.
[[nodiscard]] std::filesystem::path container_absolute(const std::filesystem::path& game_dir,
                                                       std::string_view relative);

/// The map identity a save records and `GameSession::load` checks: the
/// manifest's container spelling, with `/Maps/<n>` appended when a map number
/// is known. One spelling for every writer, so that a save of the conquest's
/// seventh map refuses to load into its third.
[[nodiscard]] std::string map_identity(const core::sim::SaveFileManifest& manifest);

/// The number in a `Maps/<n>` directory name -- `MapPayloads::map_directory`
/// -- as the digits, or empty for a container-root map. The manifest carries
/// the number actually played, never the one requested, because "the first
/// in walk order" and `game.xml`'s `start_map` are two ways of asking for a
/// map and one spelling of having played it.
[[nodiscard]] std::string map_number(std::string_view map_directory);

}  // namespace imperivm::gamedata
