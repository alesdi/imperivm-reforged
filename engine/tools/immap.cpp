// immap: the map-container writer's front end.
//
//     immap rewrite <container> <map-number> <out.bfhp> [--drop-map <n>]...
//     immap grid-roundtrip <container|archive>
//     immap passability <game-dir> <container> [map-number] [--dump]
//
// `rewrite` reads `Maps/<n>/map.obj.xml` and the six `Maps/<n>/Terrain.*.grid`
// layers out of a container, writes a copy of the container with every one
// of them regenerated -- the document by `core::write_map_objects`, the
// layers by `core::write_grid` -- and says how each stored document compares
// with its regenerated self. Nothing is edited on the way through: this is
// the round trip, and `tests/test_corpus_map_writer.py` runs `imrun` over
// both containers and requires the same hash. The editor's Save is the same
// call with an edited list and painted layers.
//
// `grid-roundtrip` finds every `GRID` container inside a `.bfhp` or a `.pak`
// by its magic -- 51 decor masks are called plainly `PASS` -- and requires
// two things of each: that `write_grid(parse(bytes))` is `bytes`, and that a
// fresh grid of the same geometry, painted cell by cell through `set_cell`
// from the parsed values, is `bytes` too. The first proves the writer, the
// second the setter's packing against what the original editor packed.
// `tests/test_corpus_grid.py` runs it over the whole install.
//
// `passability` rebuilds a map's `Terrain.pass.grid` from nothing -- the
// terrain's rules, every object's `.pass` mask through the projection and
// the height, every decoration's, and the frame (`core::edit::
// rebuild_passability`, the transcription of 0x00547700) -- and diffs the
// result against the stored layer, cell by cell. Exit 0 when identical, 3
// when not; `--dump` lists each differing cell. `tests/test_corpus_
// passability.py` requires identity on every map a player can open, which
// is what proves the reading in `docs/formats/pass.md`.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imperivm/core/formats/bfhp.hpp"
#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/formats/lzis.hpp"
#include "imperivm/core/formats/pak.hpp"
#include "imperivm/core/world/editor.hpp"
#include "imperivm/core/world/map.hpp"
#include "imperivm/gamedata/installation.hpp"
#include "imperivm/gamedata/map_source.hpp"
#include "imperivm/gamedata/map_writer.hpp"

namespace {

using imperivm::core::Grid;
using imperivm::core::OwnedGrid;

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) return {};
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::byte> data(size > 0 ? static_cast<std::size_t>(size) : 0);
  if (!data.empty() && std::fread(data.data(), 1, data.size(), file) != data.size()) {
    data.clear();
  }
  std::fclose(file);
  return data;
}

/// A stored grid against the writer and the setter. Prints one line and
/// returns whether both agreed with the stored bytes.
bool roundtrip_one(const char* label, std::string_view name, std::span<const std::byte> stored) {
  const auto parsed = Grid::parse(stored);
  if (!parsed.ok()) {
    std::printf("%s %.*s PARSE_FAILED %d\n", label, static_cast<int>(name.size()), name.data(),
                static_cast<int>(parsed.error()));
    return false;
  }
  if (!parsed->validate(stored.size()).ok()) {
    std::printf("%s %.*s SIZE_MISMATCH %zu\n", label, static_cast<int>(name.size()), name.data(),
                stored.size());
    return false;
  }
  const std::vector<std::byte> written = imperivm::core::write_grid(parsed.value());
  const bool writer_same = written.size() == stored.size() &&
                           std::memcmp(written.data(), stored.data(), stored.size()) == 0;

  bool setter_same = false;
  auto rebuilt = OwnedGrid::create(parsed->cell_size(), parsed->bits_per_cell(),
                                   parsed->extent_x(), parsed->extent_y());
  if (rebuilt.ok()) {
    setter_same = true;
    for (std::uint32_t y = 0; setter_same && y < parsed->height(); ++y) {
      for (std::uint32_t x = 0; x < parsed->width(); ++x) {
        if (!rebuilt->grid().set_cell(x, y, parsed->cell(x, y)).ok()) {
          setter_same = false;
          break;
        }
      }
    }
    setter_same = setter_same && rebuilt->bytes().size() == stored.size() &&
                  std::memcmp(rebuilt->bytes().data(), stored.data(), stored.size()) == 0;
  }
  std::printf("%s %.*s %u %u %u %u %s %s\n", label, static_cast<int>(name.size()), name.data(),
              parsed->cell_size(), parsed->bits_per_cell(), parsed->width(), parsed->height(),
              writer_same ? "identical" : "WRITER_DIFFERS",
              setter_same ? "identical" : "SETTER_DIFFERS");
  return writer_same && setter_same;
}

/// Every grid in one file, whichever kind of file it is: a `.bfhp` container
/// (bare or LZIS-wrapped) or a `.pak` archive (likewise). Returns the count of
/// grids seen, and adds the disagreements to `failures`.
std::size_t roundtrip_file(const char* path, int& failures) {
  std::vector<std::byte> data = read_file(path);
  if (data.empty()) {
    std::fprintf(stderr, "%s: cannot read\n", path);
    ++failures;
    return 0;
  }
  if (imperivm::core::has_magic(data, imperivm::core::kLzisMagic)) {
    const auto header = imperivm::core::parse_lzis_header(data);
    if (!header) {
      std::fprintf(stderr, "%s: malformed LZIS header\n", path);
      ++failures;
      return 0;
    }
    std::vector<std::byte> plain(header->uncompressed_size);
    if (!imperivm::core::lzis_decompress(data, plain)) {
      std::fprintf(stderr, "%s: LZIS stream did not decode\n", path);
      ++failures;
      return 0;
    }
    data = std::move(plain);
  }

  std::size_t seen = 0;
  if (auto archive = imperivm::core::PakDirectory::parse(data); archive.ok()) {
    for (const auto& entry : archive->entries()) {
      const auto blob = archive->read(entry);
      if (!blob || !imperivm::core::has_magic(*blob, imperivm::core::kGridMagic)) continue;
      ++seen;
      if (!roundtrip_one(path, entry.name, *blob)) ++failures;
    }
    return seen;
  }
  imperivm::gamedata::MapContainer container;
  std::string error;
  if (!container.open(path, &error)) {
    std::fprintf(stderr, "%s: neither a pack nor a container (%s)\n", path, error.c_str());
    ++failures;
    return 0;
  }
  for (const std::string& name : container.list()) {
    const std::vector<std::byte> blob = container.read(name);
    if (!imperivm::core::has_magic(blob, imperivm::core::kGridMagic)) continue;
    ++seen;
    if (!roundtrip_one(path, name, blob)) ++failures;
  }
  return seen;
}

int grid_roundtrip(int argc, char** argv) {
  int failures = 0;
  std::size_t seen = 0;
  for (int i = 2; i < argc; ++i) seen += roundtrip_file(argv[i], failures);
  std::fflush(stdout);  // the per-grid lines before the summary, even piped
  std::fprintf(stderr, "%zu grids, %d failures\n", seen, failures);
  return failures == 0 ? 0 : 1;
}

int rewrite(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: immap rewrite <container> <map-number> <out.bfhp> [--drop-map <n>]...\n");
    return 2;
  }
  const std::filesystem::path source = argv[2];
  const int number = std::atoi(argv[3]);
  const std::filesystem::path out = argv[4];
  // `--drop-map <n>`: leave `Maps/<n>/` and everything under it out of the
  // copy -- the editor's Delete map, exercised without the editor.
  std::vector<std::string> dropped;
  for (int i = 5; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--drop-map") == 0) dropped.push_back("Maps/" + std::string(argv[i + 1]));
  }

  imperivm::gamedata::MapContainer container;
  std::string error;
  if (!container.open(source, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const std::string prefix = number > 0 ? "Maps/" + std::to_string(number) + "/" : "";
  const std::string entry = prefix + "map.obj.xml";
  const std::vector<std::byte> stored = container.read(entry);
  if (stored.empty()) {
    std::fprintf(stderr, "%s: no %s\n", argv[2], entry.c_str());
    return 1;
  }
  const auto objects = imperivm::core::MapObjectList::parse(stored);
  if (!objects.ok()) {
    std::fprintf(stderr, "%s: %s does not parse\n", argv[2], entry.c_str());
    return 1;
  }

  // Every layer the map carries, parsed and handed back as a replacement for
  // itself. A layer the container lacks stays null and is simply not written.
  const char* const layer_files[6] = {
      imperivm::gamedata::kPassLayerFile,    imperivm::gamedata::kHeightLayerFile,
      imperivm::gamedata::kLightLayerFile,   imperivm::gamedata::kTerrainLayerFile,
      imperivm::gamedata::kDecorLayerFile,   imperivm::gamedata::kTransLayerFile,
  };
  std::vector<std::byte> layer_bytes[6];
  Grid layers[6];
  bool present[6] = {};
  for (int i = 0; i < 6; ++i) {
    layer_bytes[i] = container.read(prefix + layer_files[i]);
    if (layer_bytes[i].empty()) continue;
    const auto parsed = Grid::parse(layer_bytes[i]);
    if (!parsed.ok()) {
      std::fprintf(stderr, "%s: %s%s does not parse\n", argv[2], prefix.c_str(), layer_files[i]);
      return 1;
    }
    layers[i] = parsed.value();
    present[i] = true;
  }

  imperivm::gamedata::MapEdits edits;
  edits.objects = &objects.value();
  const imperivm::core::Grid** slots[6] = {&edits.pass,    &edits.height, &edits.light,
                                           &edits.terrain, &edits.decor,  &edits.trans};
  for (int i = 0; i < 6; ++i) {
    if (present[i]) *slots[i] = &layers[i];
  }
  edits.removed_directories = dropped;
  if (!imperivm::gamedata::write_map_container(out, source, number, edits, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  imperivm::gamedata::MapContainer written;
  if (!written.open(out, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  int differing = 0;
  const auto report = [&](const std::string& name, const std::vector<std::byte>& before) {
    const std::vector<std::byte> after = written.read(name);
    const bool identical = after == before;
    if (!identical) ++differing;
    std::printf("  %s %zu -> %zu bytes, %s\n", name.c_str(), before.size(), after.size(),
                identical ? "identical" : "differs");
  };
  std::printf("%s -> %s: %zu entries\n", argv[2], argv[4], written.list().size());
  report(entry, stored);
  for (int i = 0; i < 6; ++i) {
    if (present[i]) report(prefix + layer_files[i], layer_bytes[i]);
  }
  return differing == 0 ? 0 : 3;
}

int passability(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: immap passability <game-dir> <container> [map-number] [--dump]\n");
    return 2;
  }
  const std::filesystem::path game_dir = argv[2];
  const std::filesystem::path source = argv[3];
  int number = 0;
  bool dump = false;
  for (int i = 4; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dump") == 0) dump = true;
    else number = std::atoi(argv[i]);
  }

  imperivm::gamedata::Installation install;
  std::string error;
  if (!install.open(game_dir, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  imperivm::gamedata::MapContainer container;
  if (!container.open(source, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const std::string prefix = number > 0 ? "Maps/" + std::to_string(number) + "/" : "";

  // The map's layers and objects, as the editor holds them.
  const std::vector<std::byte> map_xml = container.read(prefix + "map.xml");
  const std::vector<std::byte> object_xml = container.read(prefix + "map.obj.xml");
  const std::vector<std::byte> pass_bytes = container.read(prefix + imperivm::gamedata::kPassLayerFile);
  const std::vector<std::byte> height_bytes = container.read(prefix + imperivm::gamedata::kHeightLayerFile);
  const std::vector<std::byte> terrain_bytes = container.read(prefix + imperivm::gamedata::kTerrainLayerFile);
  const std::vector<std::byte> decor_bytes = container.read(prefix + imperivm::gamedata::kDecorLayerFile);
  if (object_xml.empty() || pass_bytes.empty() || terrain_bytes.empty()) {
    std::fprintf(stderr, "%s: %smap.obj.xml, the passability and the terrain layer are all needed\n",
                 argv[3], prefix.c_str());
    return 1;
  }
  imperivm::core::MapLayerBytes bytes;
  bytes.map_xml = map_xml;
  bytes.object_xml = object_xml;
  bytes.pass = pass_bytes;
  bytes.height = height_bytes;
  bytes.terrain = terrain_bytes;
  bytes.decor = decor_bytes;
  auto loaded = imperivm::core::WorldMap::load(bytes);
  if (!loaded.ok()) {
    std::fprintf(stderr, "%s: the map does not load\n", argv[3]);
    return 1;
  }
  imperivm::core::WorldMap& map = loaded.value();
  // The season decides which entity a class stands as, and so which mask:
  // `Crops1` has a spring, an autumn and a winter entity and no seasonless one.
  imperivm::core::Season season = imperivm::core::Season::base;
  if (const std::vector<std::byte> game_xml = container.read("game.xml"); !game_xml.empty()) {
    if (const auto game = imperivm::core::GameProperties::parse(game_xml); game.ok()) {
      season = imperivm::core::season_from_name(game->season);
    }
  }
  const auto stored = Grid::parse(pass_bytes);
  if (!stored.ok()) {
    std::fprintf(stderr, "%s: the passability layer does not parse\n", argv[3]);
    return 1;
  }

  // The terrain's rules and the decoration palette.
  const auto terrains = imperivm::core::TerrainTable::parse(install.file("DATA\\TERRAINS.XML"));
  if (!terrains.ok()) {
    std::fprintf(stderr, "DATA\\TERRAINS.XML did not parse\n");
    return 1;
  }
  const auto decors = imperivm::core::DecorTable::parse(install.art_file("MAPOBJECTS\\DECORS\\DECORS.INI"));
  if (!decors.ok()) {
    std::fprintf(stderr, "MAPOBJECTS\\DECORS\\DECORS.INI did not parse\n");
    return 1;
  }

  // Every object's footprint, and every decoration kind's mask.
  std::vector<imperivm::core::edit::Footprint> footprints;
  std::size_t with_mask = 0;
  std::size_t no_class = 0;
  for (const imperivm::core::MapObject& object : map.objects().objects()) {
    const imperivm::core::ClassIndex index = install.classes().find(object.class_name);
    if (index == imperivm::core::kNoClass) {
      ++no_class;
      continue;
    }
    const std::string_view path = install.classes().entity_path(index, season);
    const imperivm::core::Entity* entity = path.empty() ? nullptr : install.entities().resolve(path);
    const imperivm::core::edit::PassMask* mask = install.masks().mask_of(entity);
    if (mask == nullptr) continue;
    ++with_mask;
    footprints.push_back({mask, imperivm::core::sim::Point{object.x, object.y}});
  }
  std::vector<const imperivm::core::edit::PassMask*> decor_masks(256, nullptr);
  std::size_t kinds_with_mask = 0;
  for (const imperivm::core::DecorKind& kind : decors->kinds()) {
    if (kind.type <= 0 || kind.type >= 256) continue;
    const imperivm::core::Entity* entity = install.entities().resolve(kind.entity);
    decor_masks[static_cast<std::size_t>(kind.type)] = install.masks().mask_of(entity);
    if (decor_masks[static_cast<std::size_t>(kind.type)] != nullptr) ++kinds_with_mask;
  }

  // The rebuild, from nothing, over the whole map.
  const imperivm::core::edit::WorldRect whole = imperivm::core::edit::WorldRect::of_map(map.terrain());
  imperivm::core::edit::rebuild_passability(map.passability_mut(), whole, map.terrain(), terrains.value(),
                                            map.height(), footprints, map.decor(), decor_masks);
  const Grid& rebuilt = map.passability();
  std::size_t stored_set = 0;
  std::size_t rebuilt_set = 0;
  std::size_t missing = 0;  // stored set, rebuilt clear
  std::size_t extra = 0;    // stored clear, rebuilt set
  for (std::uint32_t y = 0; y < stored->height(); ++y) {
    for (std::uint32_t x = 0; x < stored->width(); ++x) {
      const bool was = stored->cell(x, y) != 0;
      const bool now = rebuilt.cell(x, y) != 0;
      stored_set += was;
      rebuilt_set += now;
      if (was && !now) {
        ++missing;
        if (dump) std::printf("  missing %u %u\n", x, y);
      }
      if (!was && now) {
        ++extra;
        if (dump) std::printf("  extra %u %u\n", x, y);
      }
    }
  }
  std::printf("%s%s: %zu objects (%zu with a mask, %zu with no class), %zu decoration kinds with a mask\n",
              argv[3], prefix.empty() ? "" : (" " + prefix).c_str(), map.objects().objects().size(), with_mask,
              no_class, kinds_with_mask);
  std::printf("  %u x %u cells: stored %zu set, rebuilt %zu set, %zu missing, %zu extra, %s\n",
              stored->width(), stored->height(), stored_set, rebuilt_set, missing, extra,
              missing + extra == 0 ? "identical" : "DIFFERS");
  return missing + extra == 0 ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "grid-roundtrip") == 0) return grid_roundtrip(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "rewrite") == 0) return rewrite(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "passability") == 0) return passability(argc, argv);
  std::fprintf(stderr,
               "usage: immap rewrite <container> <map-number> <out.bfhp>\n"
               "       immap grid-roundtrip <container|archive>...\n"
               "       immap passability <game-dir> <container> [map-number] [--dump]\n");
  return 2;
}
