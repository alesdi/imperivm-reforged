#pragma once

/// A map container written back: every entry of the source copied, and any
/// subset of one map's documents regenerated -- `Maps/<n>/map.obj.xml` from
/// an edited object list, and the six `Maps/<n>/Terrain.*.grid` layers from
/// edited grids.
///
/// The object list was the editor's first slice; painting the terrain, height
/// and decoration layers is the next, and it needs the grids to come back the
/// same way. Whatever `MapEdits` leaves null is the source's, byte for byte --
/// `map.xml`, `game.xml`, the players, the sequences, the notes, the language
/// tables, and every layer that was not painted. A replaced document is
/// `core::write_map_objects`' (`core/world/map_writer.hpp`) or
/// `core::write_grid`'s (`core/formats/grid.hpp`); passing every layer
/// through unedited yields a container `imrun` cannot tell from the original
/// (`immap rewrite`, `tests/test_corpus_map_writer.py`).
///
/// A source that is LZIS-wrapped (`Packs/randommap.BFHP`) is read through
/// `MapContainer` and written *plain*, which every reader in this engine
/// accepts; whether the original's loader takes the plain form under that
/// name is not read, and the wrapper is not reproduced (this engine has no
/// LZIS encoder).

#include <filesystem>
#include <string>
#include <utility>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/world/map.hpp"

namespace imperivm::gamedata {

/// The names of the six layer files under `Maps/<n>/`, as every one of the 29
/// shipped map directories spells them.
inline constexpr const char* kPassLayerFile = "Terrain.pass.grid";
inline constexpr const char* kHeightLayerFile = "Terrain.height.grid";
inline constexpr const char* kLightLayerFile = "Terrain.light.grid";
inline constexpr const char* kTerrainLayerFile = "Terrain.terrain.grid";
inline constexpr const char* kDecorLayerFile = "Terrain.decor.grid";
inline constexpr const char* kTransLayerFile = "Terrain.trans.grid";

/// What to replace. Null means "the source's, untouched". A grid is written
/// with its own header, so its geometry is the caller's to keep consistent
/// with `map.xml` and the other layers; the writer does not police it.
struct MapEdits {
  const core::MapObjectList* objects = nullptr;
  const core::Grid* pass = nullptr;
  const core::Grid* height = nullptr;
  const core::Grid* light = nullptr;
  const core::Grid* terrain = nullptr;
  const core::Grid* decor = nullptr;
  const core::Grid* trans = nullptr;
  /// Any other document of the container by its full path (`game.xml`,
  /// `player3.xml`, `Maps/2/Notes.xml`, `Maps/2/Sequences/seq9.vs`):
  /// replaced when the source holds it, added at the end when it does not
  /// -- the editor's explorer writes these, patched in place from the
  /// source's own text (`core/xml_patch.hpp`).
  std::vector<std::pair<std::string, std::vector<std::byte>>> documents;
  /// Directories to leave out with everything under them (`Maps/3`): a
  /// map the editor deleted. Compared case-blind, either separator.
  std::vector<std::string> removed_directories;

  /// True when nothing is replaced, which makes the call a plain copy.
  [[nodiscard]] bool empty() const noexcept {
    return objects == nullptr && pass == nullptr && height == nullptr && light == nullptr &&
           terrain == nullptr && decor == nullptr && trans == nullptr && documents.empty() &&
           removed_directories.empty();
  }
};

/// Write `out` from `source_container` with the documents `edits` names
/// replaced under `Maps/<map_number>/`. A `map_number` of 0 names root-level
/// documents, the shape the format allows and no shipped container uses.
/// Refuses, with a sentence in `error`, when the source will not open, holds
/// no document an edit replaces, or the output cannot be written.
bool write_map_container(const std::filesystem::path& out,
                         const std::filesystem::path& source_container, int map_number,
                         const MapEdits& edits, std::string* error = nullptr);

/// The first slice's shape: only `Maps/<map_number>/map.obj.xml` replaced.
bool write_map_container(const std::filesystem::path& out,
                         const std::filesystem::path& source_container, int map_number,
                         const core::MapObjectList& objects, std::string* error = nullptr);

}  // namespace imperivm::gamedata
