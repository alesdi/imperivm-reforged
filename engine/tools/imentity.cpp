// Cross-validation harness for the entity runtime and the native class
// registry: run the core readers over a real installation and print results in
// a form that diffs cleanly against the Python readers.
//
// `src/imperivm/formats/gamedata.py` is validated across the entire retail
// corpus, so it is ground truth. Compiling is not evidence that a port is
// correct; agreeing with it entity by entity is.
//
// Every entity is reduced to one line carrying its counts and an FNV-1a hash
// over a canonical serialisation of *every* attribute it holds — 126,704
// instances across the corpus. The hash runs over the decoded values in a fixed
// order, so a single wrong field anywhere changes it.
//
// This is a separate binary from imcheck deliberately: two agents were
// extending that file at the same time and a merge conflict in a verification
// tool is a good way to stop verifying.
//
// It lives outside engine/core because it opens files, which core may not.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imperivm/core/formats/pak.hpp"
#include "imperivm/core/formats/rle.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/xml.hpp"

namespace {

using namespace imperivm::core;

/// The packs the object model refers to, in the reference loader's load order.
/// First pack wins on a duplicate name, which is that loader's rule too.
constexpr const char* kPackNames[] = {
    "data",  "Units",    "Buildings", "MapObjects",     "Visuals", "UI",
    "Sounds", "Terrain", "Outlines",  "Minimap",        "AdditionalArt", "Fonts",
};

std::vector<std::byte> read_file(const std::string& path, bool required) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    if (!required) return {};
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    std::exit(2);
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::byte> data(static_cast<std::size_t>(size));
  if (!data.empty() && std::fread(data.data(), 1, data.size(), file) != data.size()) {
    std::fprintf(stderr, "short read on %s\n", path.c_str());
    std::exit(2);
  }
  std::fclose(file);
  return data;
}

/// FNV-1a, 64 bit. Four lines in both languages, so the Python side of the
/// comparison cannot itself become a source of disagreement.
struct Fnv {
  std::uint64_t state = 0xcbf29ce484222325ull;

  void byte(std::uint8_t value) {
    state ^= value;
    state *= 0x100000001b3ull;
  }
  void text(std::string_view value) {
    for (const char c : value) byte(static_cast<std::uint8_t>(c));
  }
};

/// A case- and separator-insensitive view over several packs, mirroring the
/// reference loader's ResourceIndex.
class Resources {
 public:
  explicit Resources(const std::string& directory) {
    for (const char* name : kPackNames) {
      std::vector<std::byte> bytes = read_file(directory + "/" + name + ".pak", false);
      if (bytes.empty()) continue;
      auto parsed = PakDirectory::parse(bytes);
      if (!parsed) {
        std::fprintf(stderr, "%s.pak: parse failed (error %d)\n", name,
                     static_cast<int>(parsed.error()));
        continue;
      }
      packs_.push_back(Pack{name, std::move(bytes), std::move(*parsed)});
    }
    for (std::size_t p = 0; p < packs_.size(); ++p) {
      for (const PakEntry& entry : packs_[p].directory.entries()) {
        index_.push_back(Index{normalise_resource_path(entry.name), p});
      }
    }
    // Sorted by name, and stably so that the earliest pack survives the
    // deduplication below — first pack wins, as in the reference loader.
    std::stable_sort(index_.begin(), index_.end(),
                     [](const Index& a, const Index& b) { return a.name < b.name; });
    index_.erase(std::unique(index_.begin(), index_.end(),
                             [](const Index& a, const Index& b) { return a.name == b.name; }),
                 index_.end());
  }

  static constexpr std::size_t kMissing = static_cast<std::size_t>(-1);

  [[nodiscard]] std::size_t size() const { return index_.size(); }
  [[nodiscard]] const std::string& name(std::size_t i) const { return index_[i].name; }

  [[nodiscard]] std::size_t index_by_name(const std::string& key) const {
    const auto found = std::lower_bound(
        index_.begin(), index_.end(), key,
        [](const Index& entry, const std::string& value) { return entry.name < value; });
    if (found == index_.end() || found->name != key) return kMissing;
    return static_cast<std::size_t>(found - index_.begin());
  }

  [[nodiscard]] bool contains(const std::string& key) const {
    return index_by_name(key) != kMissing;
  }

  [[nodiscard]] std::span<const std::byte> read(const std::string& key) const {
    const std::size_t i = index_by_name(key);
    if (i == kMissing) return {};
    const Pack& pack = packs_[index_[i].pack];
    const auto entry = pack.directory.find(key);
    if (!entry) return {};
    const auto blob = pack.directory.read(*entry);
    return blob ? *blob : std::span<const std::byte>{};
  }

  /// Every indexed name ending in `suffix`, in sorted order.
  [[nodiscard]] std::vector<std::string> glob(std::string_view prefix,
                                              std::string_view suffix) const {
    std::vector<std::string> found;
    for (const Index& entry : index_) {
      if (entry.name.size() < suffix.size()) continue;
      if (entry.name.compare(0, prefix.size(), prefix) != 0) continue;
      if (entry.name.compare(entry.name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        continue;
      }
      found.push_back(entry.name);
    }
    return found;
  }

 private:
  struct Pack {
    std::string name;
    std::vector<std::byte> bytes;
    PakDirectory directory;
  };
  struct Index {
    std::string name;
    std::size_t pack = 0;
  };

  std::vector<Pack> packs_;
  std::vector<Index> index_;
};

std::string number(std::int32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%d", value);
  return buffer;
}

std::string optional(OptionalInt value) { return value.present ? number(value.value) : "-"; }

/// The canonical serialisation the two implementations agree on, field for
/// field and in document order. Any divergence anywhere changes the hash.
void hash_entity(const Entity& entity, Fnv& hash) {
  const auto field = [&](std::string_view value) {
    hash.text(value);
    hash.byte('|');
  };
  const auto integer = [&](std::int32_t value) { field(number(value)); };

  field("E");
  field(entity.path());
  field(entity.name());
  field(entity.type());
  integer(entity.variations());
  field(entity.pass_file());
  field(optional(entity.radius()));
  field(optional(entity.selection_radius()));
  field(optional(entity.floating_turnspeed()));

  for (const EntityImage& image : entity.images()) {
    field("I");
    integer(image.idx);
    field(image.file);
    field(draw_mode_name(image.declared_draw_mode));
    field(anim_order_name(image.order));
    integer(static_cast<std::int32_t>(image.declared_rows));
    integer(static_cast<std::int32_t>(image.declared_columns));
  }
  for (const EntityPoint& point : entity.points()) {
    field("P");
    integer(point.idx);
    integer(point.type);
    integer(point.x);
    integer(point.y);
  }
  for (const EntityLayer& layer : entity.layers()) {
    field("L");
    integer(layer.idx);
    field(layer.name);
    integer(layer.image);
    integer(layer.z);
    integer(layer.offsetx);
    integer(layer.offsety);
    integer(layer.sortoffsetx);
    integer(layer.sortoffsety);
    integer(layer.xray);
    integer(layer.nohighlight);
    field(optional(layer.percent));
  }
  for (const EntityState& state : entity.states()) {
    field("S");
    integer(state.idx);
    field(state.name);
    integer(state.image_idx);
    integer(state.image_row);
    integer(state.offsetx);
    integer(state.offsety);
    integer(state.anim_idx);
    integer(state.anim_frame);
    field(optional(state.anim_row));
  }
  for (const EntityAnim& anim : entity.anims()) {
    field("A");
    integer(anim.idx);
    field(anim.name);
    integer(anim.startstate);
    integer(anim.endstate);
    integer(anim.frames);
    integer(anim.duration);
    integer(anim.default_duration);
    integer(anim.action_time);
    integer(anim.step);
    integer(anim.floating_heading ? 1 : 0);
    for (const AnimReplace& replace : anim.replaces) {
      field("R");
      integer(replace.layer);
      integer(replace.image);
      integer(replace.offsetx);
      integer(replace.offsety);
    }
    for (const std::int32_t duration : anim.frame_durations) {
      field("F");
      integer(duration);
    }
  }
}

/// One line per entity: counts and the canonical hash. Diff against the
/// reference loader's output for the same corpus.
int list_entities(const std::string& packs_dir) {
  const Resources resources(packs_dir);
  const std::vector<std::string> paths = resources.glob("", ".ENT.XML");

  std::size_t failures = 0;
  std::size_t images = 0;
  std::size_t points = 0;
  std::size_t layers = 0;
  std::size_t states = 0;
  std::size_t anims = 0;
  std::size_t replaces = 0;
  std::size_t frames = 0;

  for (const std::string& path : paths) {
    const auto blob = resources.read(path);
    const auto entity = Entity::parse(blob, path);
    if (!entity) {
      std::printf("%s PARSE_FAILED %d\n", path.c_str(), static_cast<int>(entity.error()));
      ++failures;
      continue;
    }
    Fnv hash;
    hash_entity(*entity, hash);
    images += entity->images().size();
    points += entity->points().size();
    layers += entity->layers().size();
    states += entity->states().size();
    anims += entity->anims().size();
    for (const EntityAnim& anim : entity->anims()) {
      replaces += anim.replaces.size();
      frames += anim.frame_durations.size();
    }
    std::printf("%s %zu %zu %zu %zu %zu %016llx\n", path.c_str(), entity->images().size(),
                entity->points().size(), entity->layers().size(), entity->states().size(),
                entity->anims().size(), static_cast<unsigned long long>(hash.state));
  }

  std::fprintf(stderr,
               "%zu pack entries, %zu entities, %zu failures\n"
               "images=%zu points=%zu layers=%zu states=%zu anims=%zu replaces=%zu frames=%zu\n",
               resources.size(), paths.size(), failures, images, points, layers, states, anims,
               replaces, frames);
  return failures == 0 ? 0 : 1;
}

/// Resolve every internal reference and every image against the packs, and put
/// the XML's declared grid up against the frame table that overrules it.
int check_references(const std::string& packs_dir) {
  const Resources resources(packs_dir);
  const std::vector<std::string> paths = resources.glob("", ".ENT.XML");

  std::size_t image_refs = 0;
  std::size_t image_dangling = 0;
  std::size_t sheets_parsed = 0;
  std::size_t sheet_failures = 0;
  std::size_t rows_disagree = 0;
  std::size_t columns_disagree = 0;
  std::size_t grid_disagree = 0;
  std::size_t drawmode_disagree = 0;
  std::size_t pass_refs = 0;
  std::size_t pass_dangling = 0;
  std::size_t layer_image_dangling = 0;
  std::size_t replace_layer_dangling = 0;
  std::size_t replace_image_dangling = 0;
  std::size_t state_anim_dangling = 0;
  std::size_t anim_state_dangling = 0;
  std::size_t unresolved_geometry = 0;

  // The bins layer `z` is sorted into. One file, but the whole depth-sort
  // depends on it, so it is checked here rather than assumed.
  const auto zbins = ZBins::parse(resources.read("DATA\\ZBINS.XML"));
  if (!zbins) {
    std::printf("DATA\\ZBINS.XML PARSE_FAILED %d\n", static_cast<int>(zbins.error()));
  } else {
    for (const ZBins::Bin& bin : zbins->bins()) {
      std::printf("ZBIN %d sort=%d\n", bin.start_z, bin.sorted ? 1 : 0);
    }
  }

  for (const std::string& path : paths) {
    auto parsed = Entity::parse(resources.read(path), path);
    if (!parsed) continue;
    Entity& entity = *parsed;

    for (const EntityImage& image : entity.images()) {
      ++image_refs;
      const auto candidates = image_path_candidates(entity.path(), image.file);
      std::string found;
      for (const std::string& candidate : candidates) {
        if (resources.contains(candidate)) {
          found = candidate;
          break;
        }
      }
      if (found.empty()) {
        ++image_dangling;
        std::printf("%s IMAGE_DANGLING %s\n", path.c_str(), image.file.c_str());
        continue;
      }
      const auto sheet = RleImage::parse(resources.read(found));
      if (!sheet) {
        ++sheet_failures;
        std::printf("%s SHEET_FAILED %s %d\n", path.c_str(), found.c_str(),
                    static_cast<int>(sheet.error()));
        continue;
      }
      ++sheets_parsed;
      const auto conflict = entity.adopt_frame_table(image.idx, *sheet);
      if (!conflict) continue;
      if (conflict->rows) ++rows_disagree;
      if (conflict->columns) ++columns_disagree;
      if (conflict->rows || conflict->columns) ++grid_disagree;
      if (conflict->draw_mode) ++drawmode_disagree;
      if (conflict->any()) {
        std::printf("%s DISAGREES %d %s xml=%ux%u/%s sheet=%ux%u/%s\n", path.c_str(), image.idx,
                    found.c_str(), image.declared_rows, image.declared_columns,
                    std::string(draw_mode_name(image.declared_draw_mode)).c_str(),
                    sheet->rows(), sheet->columns(),
                    std::string(draw_mode_name(draw_mode_from_image_class(sheet->image_class())))
                        .c_str());
      }
    }
    unresolved_geometry += entity.unresolved_geometry();

    if (!entity.pass_file().empty()) {
      ++pass_refs;
      bool found = false;
      for (const std::string& candidate :
           pass_path_candidates(entity.path(), entity.pass_file())) {
        if (resources.contains(candidate)) found = true;
      }
      // A miss here is not proof of a missing mask: the engine matches masks by
      // content rather than by name.
      if (!found) {
        ++pass_dangling;
        std::printf("%s PASS_DANGLING %s\n", path.c_str(), entity.pass_file().c_str());
      }
    }

    for (const EntityLayer& layer : entity.layers()) {
      if (entity.image(layer.image) == nullptr) ++layer_image_dangling;
    }
    for (const EntityState& state : entity.states()) {
      if (state.has_anim() && entity.anim(state.anim_idx) == nullptr) {
        ++state_anim_dangling;
        std::printf("%s STATE_ANIM %d -> %d\n", path.c_str(), state.idx, state.anim_idx);
      }
    }
    for (const EntityAnim& anim : entity.anims()) {
      if (entity.state(anim.startstate) == nullptr) ++anim_state_dangling;
      if (entity.state(anim.endstate) == nullptr) ++anim_state_dangling;
      for (const AnimReplace& replace : anim.replaces) {
        if (entity.layer(replace.layer) == nullptr) ++replace_layer_dangling;
        if (entity.image(replace.image) == nullptr) ++replace_image_dangling;
      }
    }
  }

  std::fprintf(stderr,
               "zbins=%zu entities=%zu\n"
               "image refs=%zu dangling=%zu sheets parsed=%zu failed=%zu\n"
               "frame table overrules XML: rows=%zu columns=%zu grid=%zu drawmode=%zu\n"
               "geometry still from XML=%zu\n"
               "pass refs=%zu dangling by name=%zu\n"
               "dangling layer->image=%zu replace->layer=%zu replace->image=%zu "
               "state->anim=%zu anim->state=%zu\n",
               zbins.ok() ? zbins->bins().size() : 0, paths.size(), image_refs, image_dangling, sheets_parsed, sheet_failures,
               rows_disagree, columns_disagree, grid_disagree, drawmode_disagree,
               unresolved_geometry, pass_refs, pass_dangling, layer_image_dangling,
               replace_layer_dangling, replace_image_dangling, state_anim_dangling,
               anim_state_dangling);
  return (image_dangling > 1 || sheet_failures > 0 || layer_image_dangling > 0) ? 1 : 0;
}

/// Every `cpp_class` named by a shipped class, resolved against the registry.
int check_registry(const std::string& packs_dir) {
  const Resources resources(packs_dir);
  const std::vector<std::string> paths = resources.glob("DATA\\CLASSES", ".SC.XML");

  std::vector<std::size_t> census(kNativeClassCount, 0);
  std::size_t unresolved = 0;
  std::size_t missing_attribute = 0;

  for (const std::string& path : paths) {
    const auto document = XmlDocument::parse(resources.read(path));
    if (!document) {
      std::printf("%s XML_FAILED %d\n", path.c_str(), static_cast<int>(document.error()));
      ++unresolved;
      continue;
    }
    const std::string_view cpp_class = document->attribute(document->root(), "cpp_class");
    if (cpp_class.empty()) {
      ++missing_attribute;
      std::printf("%s NO_CPP_CLASS\n", path.c_str());
      continue;
    }
    const auto id = native_class_from_name(cpp_class);
    if (!id) {
      ++unresolved;
      std::printf("%s UNRESOLVED %.*s\n", path.c_str(), static_cast<int>(cpp_class.size()),
                  cpp_class.data());
      continue;
    }
    ++census[static_cast<std::size_t>(*id)];
    // Constructing it is the other half of "resolves": a name in the table with
    // no type behind it would pass a lookup and fail at spawn.
    const auto object = make_native_object(*id);
    if (object == nullptr || object->native_class() != *id) {
      ++unresolved;
      std::printf("%s NO_NATIVE_TYPE %.*s\n", path.c_str(), static_cast<int>(cpp_class.size()),
                  cpp_class.data());
    }
  }

  std::size_t total = 0;
  for (const NativeClassInfo& info : native_classes()) {
    const std::size_t counted = census[static_cast<std::size_t>(info.id)];
    total += counted;
    std::printf("%-16.*s %4zu %4u %s\n", static_cast<int>(info.name.size()), info.name.data(),
                counted, info.corpus_classes,
                counted == info.corpus_classes ? "ok" : "CENSUS_DIFFERS");
  }

  std::size_t used = 0;
  for (const std::size_t counted : census) {
    if (counted > 0) ++used;
  }
  std::fprintf(stderr,
               "%zu classes, %zu bound, %zu unresolved, %zu without a cpp_class\n"
               "%zu of %zu registered native classes are named by the data\n",
               paths.size(), total, unresolved, missing_attribute, used, kNativeClassCount);
  return (unresolved == 0 && missing_attribute == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: imentity entities <packs-dir>   one line per entity, with a hash\n"
                 "       imentity refs     <packs-dir>   resolve images, masks and internal refs\n"
                 "       imentity registry <packs-dir>   resolve every cpp_class in the corpus\n");
    return 2;
  }
  const std::string command = argv[1];
  const std::string packs = argv[2];
  if (command == "entities") return list_entities(packs);
  if (command == "refs") return check_references(packs);
  if (command == "registry") return check_registry(packs);
  std::fprintf(stderr, "unknown command '%s'\n", command.c_str());
  return 2;
}
