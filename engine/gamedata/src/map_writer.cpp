#include "imperivm/gamedata/map_writer.hpp"

#include <algorithm>

#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include "imperivm/core/formats/bfhp.hpp"
#include "imperivm/core/world/map_writer.hpp"
#include "imperivm/gamedata/map_source.hpp"

namespace imperivm::gamedata {

namespace {

/// One document to regenerate: where it lives in the container and what goes
/// there.
struct Replacement {
  std::string path;
  std::vector<std::byte> payload;
};

/// The container folds case and separators when it looks a name up, so a
/// replacement matches a stored entry the same way rather than by exact
/// spelling: every shipped directory says `Terrain.pass.grid`, but a container
/// this engine did not write may not.
bool same_path(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  const auto fold = [](char c) -> char {
    if (c == '\\') return '/';
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
  };
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (fold(a[i]) != fold(b[i])) return false;
  }
  return true;
}

std::vector<std::byte> bytes_of(const std::string& text) {
  std::vector<std::byte> out(text.size());
  std::memcpy(out.data(), text.data(), text.size());
  return out;
}

}  // namespace

bool write_map_container(const std::filesystem::path& out,
                         const std::filesystem::path& source_container, int map_number,
                         const MapEdits& edits, std::string* error) {
  MapContainer source;
  if (!source.open(source_container, error)) return false;

  const std::string prefix = map_number > 0 ? "Maps/" + std::to_string(map_number) + "/" : "";

  // Every replacement is rendered up front, and every one must name a
  // document the source holds: a layer the source lacks is not something to
  // invent a place for, since the map that lacks it loads without it.
  std::vector<Replacement> replacements;
  if (edits.objects != nullptr) {
    replacements.push_back({prefix + "map.obj.xml", bytes_of(core::write_map_objects(*edits.objects))});
  }
  const std::pair<const core::Grid*, const char*> layers[] = {
      {edits.pass, kPassLayerFile},       {edits.height, kHeightLayerFile},
      {edits.light, kLightLayerFile},     {edits.terrain, kTerrainLayerFile},
      {edits.decor, kDecorLayerFile},     {edits.trans, kTransLayerFile},
  };
  for (const auto& [grid, name] : layers) {
    if (grid != nullptr) replacements.push_back({prefix + name, core::write_grid(*grid)});
  }
  for (const Replacement& replacement : replacements) {
    if (!source.contains(replacement.path)) {
      if (error != nullptr) *error = source_container.string() + ": no " + replacement.path;
      return false;
    }
  }
  // The explorer's documents: in place when the source holds them, else
  // appended after everything the source had, their directories made.
  std::vector<Replacement> added;
  for (const auto& [path, payload] : edits.documents) {
    if (source.contains(path)) {
      replacements.push_back({path, payload});
    } else {
      added.push_back({path, payload});
    }
  }

  // A removed directory takes its files and its subdirectories with it.
  const auto removed = [&](const std::string& path) {
    for (const std::string& directory : edits.removed_directories) {
      if (same_path(path, directory)) return true;
      if (path.size() > directory.size() && same_path(path.substr(0, directory.size()), directory) &&
          (path[directory.size()] == '/' || path[directory.size()] == '\\')) {
        return true;
      }
    }
    return false;
  };
  added.erase(std::remove_if(added.begin(), added.end(), [&](const Replacement& r) { return removed(r.path); }),
              added.end());

  // The tree first, parents before children as the index walks them; then
  // every file, the replaced ones swapped in. Walk order is kept, so a
  // container the original wrote and this one rewrote list the same way.
  core::BlockFileBuilder builder;
  for (const std::string& directory : source.directories()) {
    if (removed(directory)) continue;
    if (const core::Status status = builder.directory(directory); !status.ok()) {
      if (error != nullptr) *error = out.string() + ": cannot add directory " + directory;
      return false;
    }
  }
  for (const std::string& path : source.list()) {
    if (removed(path)) continue;
    const Replacement* replaced = nullptr;
    for (const Replacement& candidate : replacements) {
      if (same_path(candidate.path, path)) {
        replaced = &candidate;
        break;
      }
    }
    const std::vector<std::byte> payload =
        replaced != nullptr ? replaced->payload : source.read(path);
    if (const core::Status status = builder.file(path, payload); !status.ok()) {
      if (error != nullptr) *error = out.string() + ": cannot add " + path;
      return false;
    }
  }
  for (const Replacement& fresh : added) {
    // Every directory on the way that the source did not have.
    for (std::size_t slash = fresh.path.find('/'); slash != std::string::npos; slash = fresh.path.find('/', slash + 1)) {
      const std::string directory = fresh.path.substr(0, slash);
      bool had = false;
      for (const std::string& existing : source.directories()) {
        if (same_path(existing, directory)) had = true;
      }
      if (!had) (void)builder.directory(directory);
    }
    if (const core::Status status = builder.file(fresh.path, fresh.payload); !status.ok()) {
      if (error != nullptr) *error = out.string() + ": cannot add " + fresh.path;
      return false;
    }
  }

  const std::vector<std::byte> bytes = builder.build();
  std::ofstream file(out, std::ios::binary | std::ios::trunc);
  if (!file) {
    if (error != nullptr) *error = out.string() + ": cannot open for writing";
    return false;
  }
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    if (error != nullptr) *error = out.string() + ": write failed";
    return false;
  }
  return true;
}

bool write_map_container(const std::filesystem::path& out,
                         const std::filesystem::path& source_container, int map_number,
                         const core::MapObjectList& objects, std::string* error) {
  MapEdits edits;
  edits.objects = &objects;
  return write_map_container(out, source_container, map_number, edits, error);
}

}  // namespace imperivm::gamedata
