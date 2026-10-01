// A retail installation, opened and held.
// See include/imperivm/platform/installation.hpp.

#include "imperivm/gamedata/installation.hpp"

#include "imperivm/core/world/editor.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "imperivm/core/formats/bfhp.hpp"
#include "imperivm/core/formats/lzis.hpp"
#include "imperivm/core/formats/pak.hpp"
#include "imperivm/core/formats/rle.hpp"
#include "imperivm/gamedata/map_source.hpp"

namespace imperivm::gamedata {
namespace {

using namespace imperivm::core;

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) return {};
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::byte> data(static_cast<std::size_t>(size < 0 ? 0 : size));
  if (!data.empty() && std::fread(data.data(), 1, data.size(), file) != data.size()) data.clear();
  std::fclose(file);
  return data;
}

/// Uppercase, and `/` folded to `\`. Pack names are cp1252 with backslashes and
/// conventionally uppercase, but class bindings spell them every other way.
std::string normalise(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c == '/') c = '\\';
  }
  return out;
}

[[nodiscard]] std::string_view basename_of(std::string_view path) {
  const std::size_t slash = path.find_last_of('\\');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

}  // namespace

/// Everything the pack side owns, kept out of the header so that including it
/// does not pull in the pack reader.
struct Installation::Impl final : public core::sim::ScriptResolver,
                                  public core::sim::EntityResolver,
                                  public core::sim::PassMaskResolver {
  std::vector<std::byte> pack_bytes;
  std::unique_ptr<PakDirectory> pack;
  /// Entities, loaded on demand; see `Installation::entities`.
  core::EntityLibrary entities;
  /// Their masks, likewise; see `Installation::masks`.
  core::edit::PassMaskLibrary masks;
  /// The art packs -- `Units.pak`, `Buildings.pak`, `MapObjects.pak`,
  /// `Visuals.pak` on a retail install -- opened on the first resolve. Every
  /// `*.pak` beside `data.pak` is tried in sorted name order and the ones that
  /// hold no `.ent.xml` at all are let go again, so `Sounds.pak` costs one
  /// read and no memory. `data.pak` itself holds none and is not searched.
  std::filesystem::path packs_dir;
  struct ArtPack {
    std::vector<std::byte> bytes;
    std::unique_ptr<PakDirectory> directory;
  };
  mutable std::vector<ArtPack> art;
  mutable bool art_opened = false;
  /// See `Installation::settlement_templates`.
  std::vector<std::byte> settlement_templates;
  std::vector<std::byte> settlement_template_terrain;
  bool settlement_templates_read = false;

  void open_art() const {
    art_opened = true;
    std::vector<std::filesystem::path> candidates;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(packs_dir, ignored)) {
      const std::string name = normalise(entry.path().filename().string());
      if (name.size() < 4 || name.substr(name.size() - 4) != ".PAK" || name == "DATA.PAK") continue;
      candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());
    for (const std::filesystem::path& path : candidates) {
      ArtPack one;
      one.bytes = read_file(path);
      if (one.bytes.empty()) continue;
      auto directory = PakDirectory::parse(one.bytes);
      if (!directory.ok()) continue;
      bool any = false;
      for (const PakEntry& e : directory.value().entries()) {
        const std::string name = normalise(e.name);
        if (name.size() > 8 && name.substr(name.size() - 8) == ".ENT.XML") {
          any = true;
          break;
        }
      }
      if (!any) continue;
      one.directory = std::make_unique<PakDirectory>(std::move(directory.value()));
      art.push_back(std::move(one));
    }
  }

  const core::Entity* resolve(std::string_view path) const override {
    if (path.empty()) return nullptr;
    if (const core::Entity* found = entities.find(path)) return found;
    if (!art_opened) open_art();
    for (const ArtPack& one : art) {
      const auto blob = one.directory->read(path);
      if (!blob.ok()) continue;
      // `load` is a write on a registry the interface hands out as const; the
      // registry is this installation's own and the definition is immutable
      // once loaded, which is the contract `EntityLibrary::load` keeps.
      auto loaded = const_cast<core::EntityLibrary&>(entities).load(path, blob.value());
      if (!loaded.ok()) return nullptr;
      if (core::Entity* entity = const_cast<core::EntityLibrary&>(entities).find_mutable(path)) {
        adopt_frame_tables(*entity);
      }
      return loaded.value();
    }
    return nullptr;
  }
  /// Each image's grid from its sheet's frame table, as the app's resolver
  /// takes it (`WorldView::adopt_frame_tables`): the first candidate path
  /// that holds a frame table that parses. **Simulation, not drawing**: an
  /// animation's length is its sheet's rows (`Entity::anim_rows`), and 430
  /// of the shipped `<image>` declarations state rows the sheet contradicts.
  /// For as long as this took the declared grid, every headless tool ran a
  /// different game from the app's: Crossroads parted at turn 4 and ended
  /// on another hash.
  void adopt_frame_tables(core::Entity& entity) const {
    std::vector<std::int32_t> indices;
    indices.reserve(entity.images().size());
    for (const core::EntityImage& image : entity.images()) indices.push_back(image.idx);
    for (const std::int32_t idx : indices) {
      const core::EntityImage* image = entity.image(idx);
      if (image == nullptr) continue;
      for (const std::string& candidate : core::image_path_candidates(entity.path(), image->file)) {
        const std::span<const std::byte> table = art_file(candidate);
        if (table.empty()) continue;
        auto sheet = core::RleImage::parse(table);
        if (!sheet) continue;
        (void)entity.adopt_frame_table(idx, sheet.value());
        break;
      }
    }
  }
  /// One file out of the art packs, or nothing.
  std::span<const std::byte> art_file(std::string_view path) const {
    if (!art_opened) open_art();
    for (const ArtPack& one : art) {
      const auto blob = one.directory->read(path);
      if (blob.ok()) return blob.value();
    }
    return {};
  }

  const core::edit::PassMask* mask_of(const core::Entity* entity) override {
    return masks.resolve(entity, [this](std::string_view path) { return art_file(path); });
  }

  /// Class XML, held because the graph views into it.
  std::vector<std::vector<std::byte>> class_blobs;
  std::vector<std::byte> constants;
  std::vector<std::byte> formations;
  std::vector<std::byte> ai_profile;
  std::vector<std::byte> items;
  std::vector<std::byte> skills;
  std::vector<std::byte> unit_specials;
  /// `DATA/COMMANDS/*.XML`, in sorted name order, as spans into the pack.
  std::vector<std::span<const std::byte>> commands;
  std::vector<std::string> bonus_scripts;
  std::vector<std::string> game_scripts;

  /// `.vs` entries by full name and by basename, both normalised. Sorted, so
  /// lookup is a binary search and the order is reproducible.
  std::vector<std::pair<std::string, const PakEntry*>> by_path;
  std::vector<std::pair<std::string, const PakEntry*>> by_base;

  std::span<const std::byte> source(std::string_view path) override {
    const std::string wanted = normalise(path);
    const PakEntry* entry = lookup(by_path, wanted);
    if (entry == nullptr) entry = lookup(by_base, std::string(basename_of(wanted)));
    if (entry == nullptr || pack == nullptr) return {};
    const auto blob = pack->read(*entry);
    return blob.ok() ? blob.value() : std::span<const std::byte>{};
  }

  static const PakEntry* lookup(
      const std::vector<std::pair<std::string, const PakEntry*>>& table, const std::string& key) {
    const auto it = std::lower_bound(
        table.begin(), table.end(), key,
        [](const auto& row, const std::string& probe) { return row.first < probe; });
    return it != table.end() && it->first == key ? it->second : nullptr;
  }
};

Installation::Installation() : impl_(std::make_unique<Impl>()) {}
Installation::~Installation() = default;

core::sim::ScriptResolver& Installation::scripts() noexcept { return *impl_; }
core::sim::EntityResolver& Installation::entities() noexcept { return *impl_; }

std::span<const std::span<const std::byte>> Installation::commands() const noexcept {
  return impl_->commands;
}

std::span<const std::byte> Installation::constants() const noexcept { return impl_->constants; }
std::span<const std::byte> Installation::formations() const noexcept {
  return impl_->formations;
}
std::span<const std::byte> Installation::ai_profile() const noexcept { return impl_->ai_profile; }
std::span<const std::byte> Installation::items() const noexcept { return impl_->items; }
std::span<const std::string> Installation::bonus_scripts() const noexcept {
  return impl_->bonus_scripts;
}
std::span<const std::string> Installation::game_scripts() const noexcept {
  return impl_->game_scripts;
}
std::span<const std::byte> Installation::skills() const noexcept { return impl_->skills; }
std::span<const std::byte> Installation::unit_specials() const noexcept {
  return impl_->unit_specials;
}

std::span<const std::byte> Installation::settlement_templates() noexcept {
  Impl& impl = *impl_;
  if (!impl.settlement_templates_read) {
    impl.settlement_templates_read = true;
    // The pack is a map container like any other -- LZIS-wrapped `.bfhp`
    // with one `Maps/1` -- so the map reader opens it. Read by exact name:
    // `gbr.exe` names it as a literal (0x005923db) and nothing else has the
    // templates.
    MapContainer container;
    if (container.open(impl.packs_dir / "RandomMapSettlements.bfhp")) {
      constexpr std::string_view kDocument = "Maps/1/map.obj.xml";
      if (container.contains(kDocument)) impl.settlement_templates = container.read(kDocument);
      constexpr std::string_view kTerrain = "Maps/1/Terrain.terrain.grid";
      if (container.contains(kTerrain)) impl.settlement_template_terrain = container.read(kTerrain);
    }
  }
  return impl.settlement_templates;
}

std::span<const std::byte> Installation::settlement_template_terrain() noexcept {
  (void)settlement_templates();
  return impl_->settlement_template_terrain;
}

std::span<const std::byte> Installation::file(std::string_view pack_name) const noexcept {
  if (impl_->pack == nullptr) return {};
  const auto blob = impl_->pack->read(pack_name);
  return blob.ok() ? blob.value() : std::span<const std::byte>{};
}

std::span<const std::byte> Installation::art_file(std::string_view pack_name) const noexcept {
  return impl_->art_file(pack_name);
}

core::sim::PassMaskResolver& Installation::masks() noexcept { return *impl_; }

bool Installation::open(const std::filesystem::path& game_dir, std::string* error) {
  const auto fail = [error](std::string why) {
    if (error != nullptr) *error = std::move(why);
    return false;
  };

  const std::filesystem::path pack_path = game_dir / "Packs" / "data.pak";
  impl_->packs_dir = game_dir / "Packs";
  impl_->pack_bytes = read_file(pack_path);
  if (impl_->pack_bytes.empty()) return fail("cannot read " + pack_path.string());

  auto directory = PakDirectory::parse(impl_->pack_bytes);
  if (!directory.ok()) return fail(pack_path.string() + " did not parse as a pack");
  impl_->pack = std::make_unique<PakDirectory>(std::move(directory.value()));

  // The class graph, in sorted file order so that class indices reproduce.
  std::vector<std::pair<std::string, const PakEntry*>> class_files;
  std::vector<std::pair<std::string, const PakEntry*>> command_files;
  for (const PakEntry& entry : impl_->pack->entries()) {
    const std::string name = normalise(entry.name);
    if (name.starts_with("DATA\\CLASSES\\") && name.ends_with(".SC.XML")) {
      class_files.emplace_back(name, &entry);
    } else if (name.starts_with("DATA\\COMMANDS\\") && name.ends_with(".XML")) {
      command_files.emplace_back(name, &entry);
    } else if (name.ends_with(".VS")) {
      impl_->by_path.emplace_back(name, &entry);
      impl_->by_base.emplace_back(std::string(basename_of(name)), &entry);
      if (name.starts_with("DATA\\BONUSSCRIPTS\\")) {
        impl_->bonus_scripts.emplace_back(basename_of(name));
      } else if (name.starts_with("DATA\\GAMESCRIPTS\\")) {
        // Without the extension: the profile's `victorycond=1 Elimination`
        // and `CONST.INI`'s `2_Score_limit0` keys are the bare name.
        std::string_view base = basename_of(name);
        base.remove_suffix(3);
        impl_->game_scripts.emplace_back(base);
      }
    }
  }
  std::sort(impl_->bonus_scripts.begin(), impl_->bonus_scripts.end());
  std::sort(impl_->game_scripts.begin(), impl_->game_scripts.end());
  std::sort(class_files.begin(), class_files.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  // A basename can be ambiguous; the first in sorted order wins, which is at
  // least reproducible. The full-path table is tried first, so this only
  // decides for a binding that gave no directory.
  const auto by_name = [](const auto& a, const auto& b) { return a.first < b.first; };
  std::sort(impl_->by_path.begin(), impl_->by_path.end(), by_name);
  std::stable_sort(impl_->by_base.begin(), impl_->by_base.end(), by_name);

  for (const auto& [name, entry] : class_files) {
    const auto blob = impl_->pack->read(*entry);
    if (!blob.ok()) continue;
    impl_->class_blobs.emplace_back(blob.value().begin(), blob.value().end());
    if (graph_.add(impl_->class_blobs.back(), name).ok()) ++class_files_;
  }
  graph_.link();

  // Sorted, because merge order decides which of two rows sharing a name wins.
  std::sort(command_files.begin(), command_files.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [name, entry] : command_files) {
    const auto blob = impl_->pack->read(*entry);
    if (blob.ok()) impl_->commands.push_back(blob.value());
  }
  if (class_files_ == 0) return fail("no class definitions in " + pack_path.string());

  const auto slurp = [&](const char* pack_name) {
    const auto blob = impl_->pack->read(pack_name);
    return blob.ok() ? std::vector<std::byte>(blob.value().begin(), blob.value().end())
                     : std::vector<std::byte>();
  };
  impl_->constants = slurp("DATA\\CONST.INI");
  impl_->formations = slurp("DATA\\FORMATIONS.XML");
  impl_->ai_profile = slurp("DATA\\AI\\AI.INI");
  impl_->items = slurp("DATA\\ITEMS.XML");
  impl_->skills = slurp("DATA\\SKILLS.INI");
  impl_->unit_specials = slurp("DATA\\UNIT_SPECIALS.INI");
  return true;
}

std::span<const std::byte> ContainerScripts::source(std::string_view path) {
  if (container_ != nullptr && container_->contains(path)) {
    std::vector<std::byte> bytes = container_->read(path);
    if (!bytes.empty()) {
      held_.push_back(std::make_unique<std::vector<std::byte>>(std::move(bytes)));
      ++from_container_;
      return *held_.back();
    }
  }
  ++from_pack_;
  return fallback_ == nullptr ? std::span<const std::byte>{} : fallback_->source(path);
}

MapPayloads read_payloads(const MapContainer& container, std::string_view map_index) {
  MapPayloads out;
  out.players.assign(core::sim::kPlayerCount, {});

  // The container reader already handles the LZIS-wrapped case and hands back
  // `/`-separated paths, so all this has to know is which paths matter.
  std::string objects_path;
  // Collected during the walk and filtered after it, because which ones belong
  // to this session depends on `map_directory`, which is not known until the
  // objects document has been chosen.
  std::vector<std::string> conversation_paths;
  for (const std::string& stored : container.list()) {
    const std::string name = normalise(stored);
    if (name.ends_with(".CONV.XML")) {
      // `Local/<language>/…/cnv<k>.conv.xml` shares the extension and is a
      // `<translationtable>`, not a conversation. Reading it would put every
      // conversation in the catalogue twice under a second spelling.
      if (!name.starts_with("LOCAL\\")) conversation_paths.push_back(stored);
      continue;
    }
    if (name == "GAME.XML") {
      out.game = container.read(stored);
      continue;
    }
    if (name == "ITEMSCUSTOM.XML") {
      out.custom_items = container.read(stored);
      continue;
    }
    if (name.starts_with("PLAYER") && name.ends_with(".XML") &&
        name.find('\\') == std::string::npos) {
      const std::string digits = name.substr(6, name.size() - 10);
      if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
      const int id = std::atoi(digits.c_str());
      if (id >= 0 && id < static_cast<int>(core::sim::kPlayerCount)) {
        out.players[static_cast<std::size_t>(id)] = container.read(stored);
      }
      continue;
    }
    if (!name.ends_with("MAP.OBJ.XML")) continue;
    // A conquest holds several `Maps/<n>` and their numbers are not
    // contiguous, so an explicit index is matched rather than counted, and the
    // first in walk order is the fallback.
    if (!map_index.empty()) {
      const std::string wanted = normalise(std::string("MAPS\\") + std::string(map_index) + "\\");
      // **Not `break`.** This walk also collects `game.xml` and the sixteen
      // `player<i>.xml`, and in a conquest `Maps/` sorts before `player0.xml`,
      // so stopping at the chosen map left the player table entirely empty and
      // `load_player_table` refused all seven conquest maps with a bare
      // `malformed`. The index selects a document; it does not end the walk.
      if (name.find(wanted) != std::string::npos) objects_path = stored;
      continue;
    }
    if (objects_path.empty()) objects_path = stored;
  }

  if (!objects_path.empty()) out.objects = container.read(objects_path);
  for (const auto& document : out.players) out.player_spans.push_back(document);

  // The map directory the objects came from. `Maps/<n>/map.obj.xml` minus the
  // file name; empty for the flat containers that keep their map at the root.
  {
    const std::string name = normalise(objects_path);
    const std::size_t slash = name.find_last_of('\\');
    if (slash != std::string::npos && name.starts_with("MAPS\\")) {
      out.map_directory = objects_path.substr(0, slash);
    }
  }

  // The obstruction bitmap, from the same directory the objects came from --
  // read by path rather than collected in the walk above, because a conquest
  // holds one of these per map and only the chosen map's is wanted. A container
  // that keeps its map at the root has no `Maps/<n>` and no layer either; that
  // leaves the payload empty, which the session degrades over.
  if (!out.map_directory.empty()) {
    const std::string layer = out.map_directory + "/Terrain.pass.grid";
    if (container.contains(layer)) out.passability = container.read(layer);
  }

  // `map.xml`, beside the objects. The extent it declares is what makes the map
  // rectangle a rectangle: without it `MatchRules::map_size` is zero and
  // `Place` refuses every position in the world.
  if (!out.map_directory.empty()) {
    const std::string path = out.map_directory + "/map.xml";
    if (container.contains(path)) out.map_properties = container.read(path);
  }

  // And the terrain-type layer beside them, which `IsPointInWater` reads.
  if (!out.map_directory.empty()) {
    const std::string path = out.map_directory + "/Terrain.terrain.grid";
    if (container.contains(path)) out.terrain = container.read(path);
  }

  // And the height layer, which `GetTerrainHeight` reads. Same directory, same
  // degradation: a container without one leaves the map flat.
  if (!out.map_directory.empty()) {
    const std::string path = out.map_directory + "/Terrain.height.grid";
    if (container.contains(path)) out.height = container.read(path);
  }

  // And the decorations, for the passability rebuild at match start.
  if (!out.map_directory.empty()) {
    const std::string path = out.map_directory + "/Terrain.decor.grid";
    if (container.contains(path)) out.decor = container.read(path);
  }

  // `territories.xml` is a conquest's and only a conquest's; one of the 24
  // containers has it.
  if (container.contains("territories.xml")) out.conquest = container.read("territories.xml");

  // The conversations, by the same two roots the sequence manifests use:
  // `CurrentGame/Conversations` at the container root and the chosen map's
  // own. Sorted by path so that two runs over the same container build the
  // catalogue in the same order -- declaration order decides which of two
  // documents with one name wins, and iteration order is state.
  {
    const std::string prefix = normalise(out.map_directory) + "\\CONVERSATIONS\\";
    std::vector<std::string> wanted;
    for (const std::string& stored : conversation_paths) {
      const std::string name = normalise(stored);
      const bool at_root = name.starts_with("CONVERSATIONS\\");
      const bool in_map = !out.map_directory.empty() && name.starts_with(prefix);
      if (at_root || in_map) wanted.push_back(stored);
    }
    std::sort(wanted.begin(), wanted.end());
    for (const std::string& path : wanted) out.conversations.push_back(container.read(path));
    for (const auto& document : out.conversations) out.conversation_spans.push_back(document);
  }

  // The two note catalogues, by the names `gbr.exe` searches in that order:
  // the container's root `Notes.xml` and the chosen map's own. Read by path
  // for the same reason the obstruction layer is -- a conquest keeps one per
  // map. `Local/<language>/**/notes.xml` is the translated copy and is *not*
  // read: the ids are the same and only the display strings differ, so taking
  // it would put the same 122 declarations in twice under a second spelling.
  if (container.contains("Notes.xml")) out.notes = container.read("Notes.xml");
  if (!out.map_directory.empty()) {
    const std::string path = out.map_directory + "/Notes.xml";
    if (container.contains(path)) out.map_notes = container.read(path);
  }

  // The two sequence manifests `gbr.exe` names: `CurrentGame/Sequences/
  // sequences.xml` and `CurrentMap/Sequences/sequences.xml`.
  const auto manifest = [&container](const std::string& path) {
    std::vector<core::sim::SequenceRef> out;
    if (!container.contains(path)) return out;
    const std::vector<std::byte> bytes = container.read(path);
    if (bytes.empty()) return out;
    auto parsed = core::sim::parse_sequences(bytes);
    if (parsed.ok()) out = std::move(parsed.value());
    return out;
  };
  out.game_sequences = manifest("Sequences/sequences.xml");
  if (!out.map_directory.empty()) {
    out.map_sequences = manifest(out.map_directory + "/Sequences/sequences.xml");
  }
  return out;
}

std::vector<std::vector<std::byte>> read_localisation(const MapContainer& container,
                                                      std::string_view language) {
  std::vector<std::vector<std::byte>> out;
  if (language.empty()) return out;
  std::string prefix = "local/";
  for (const char c : language) prefix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  prefix.push_back('/');
  for (const std::string& name : container.list()) {
    std::string folded;
    for (const char c : name) folded.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (folded.size() <= prefix.size() || folded.compare(0, prefix.size(), prefix) != 0) continue;
    if (folded.size() < 4 || folded.compare(folded.size() - 4, 4, ".xml") != 0) continue;
    const auto bytes = container.read(name);
    if (bytes.empty()) continue;
    out.emplace_back(bytes.begin(), bytes.end());
  }
  return out;
}

core::sim::SessionInputs session_inputs(Installation& install, const MapPayloads& map) {
  core::sim::SessionInputs inputs;
  inputs.map_objects = map.objects;
  inputs.passability = map.passability;
  inputs.player_setup = map.player_spans;
  inputs.game = map.game;
  inputs.map_properties = map.map_properties;
  inputs.terrain = map.terrain;
  inputs.height = map.height;
  // The passability rebuild's inputs (`SessionInputs::masks`): the
  // decorations, the terrain's rules, the decoration palette, the masks.
  inputs.decor = map.decor;
  inputs.terrain_table = install.file("DATA\\TERRAINS.XML");
  inputs.decor_table = install.art_file("MAPOBJECTS\\DECORS\\DECORS.INI");
  inputs.masks = &install.masks();
  inputs.conquest = map.conquest;
  inputs.notes = map.notes;
  inputs.map_notes = map.map_notes;
  inputs.conversations = map.conversation_spans;
  inputs.commands = install.commands();
  inputs.constants = install.constants();
  inputs.formations = install.formations();
  inputs.items = install.items();
  inputs.custom_items = map.custom_items;
  inputs.ai_profile = install.ai_profile();
  inputs.classes = &install.classes();
  inputs.scripts = &install.scripts();
  // The art definitions are simulation inputs too -- an animation's length is
  // read off the entity -- and every headless tool ran without them until
  // this line; see `Installation::entities`. The app installs its own
  // resolver over this one, because it also corrects image grids.
  inputs.entities = &install.entities();
  // And the settlement templates, so that a skirmish map's `Mutable`
  // placeholders become a race's at `start_match`. Every headless tool and the
  // app ran every skirmish with one-building strongholds until this line.
  inputs.settlement_templates = install.settlement_templates();
  inputs.settlement_template_terrain = install.settlement_template_terrain();
  return inputs;
}

}  // namespace imperivm::gamedata
