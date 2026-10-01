// Cross-validation harness: run the core readers over real game files and
// print results in a form that diffs cleanly against the Python readers.
//
// The Python implementations in src/imperivm/formats/ are validated across the
// entire retail corpus, so they are ground truth. Compiling is not evidence
// that a port is correct; agreeing with them file by file is.
//
// Decoded pixels and cell grids are compared by FNV-1a hash rather than by
// dumping them, because the corpus is 400 MB of sprite payload and nobody reads
// a diff that size. The hash runs over the decoded *values* in a fixed order,
// so a single wrong bit anywhere changes it.
//
// This lives outside engine/core because it opens files, which core may not.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "imperivm/core/formats/bfhp.hpp"
#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/formats/lzis.hpp"
#include "imperivm/core/formats/pak.hpp"
#include "imperivm/core/formats/rle.hpp"
#include "imperivm/core/formats/vq.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/profile.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/conversation.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/ui/interface.hpp"
#include "imperivm/core/ui/paint.hpp"
#include "imperivm/core/world/map.hpp"
#include "imperivm/core/world/map_writer.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/xml.hpp"
// The `vs` subcommand lives in its own header so that the script front end and
// the format readers can be worked on without editing the same lines.
#include "vs_dump.hpp"

namespace {

using namespace imperivm::core;

std::vector<std::byte> read_file(const char* path) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot open %s\n", path);
    std::exit(2);
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::byte> data(static_cast<std::size_t>(size));
  if (!data.empty() && std::fread(data.data(), 1, data.size(), file) != data.size()) {
    std::fprintf(stderr, "short read on %s\n", path);
    std::exit(2);
  }
  std::fclose(file);
  return data;
}

/// FNV-1a, 64 bit. Chosen because it is four lines in both languages, so the
/// Python side of the comparison cannot itself become a source of disagreement.
struct Fnv {
  std::uint64_t state = 0xcbf29ce484222325ull;

  void byte(std::uint8_t value) {
    state ^= value;
    state *= 0x100000001b3ull;
  }
  void u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) byte(static_cast<std::uint8_t>(value >> shift));
  }
  void bytes(std::span<const std::byte> data) {
    for (const std::byte b : data) byte(static_cast<std::uint8_t>(b));
  }
};

int list_pak(const char* path) {
  const std::vector<std::byte> data = read_file(path);
  auto directory = PakDirectory::parse(data);
  if (!directory) {
    std::fprintf(stderr, "%s: parse failed (error %d)\n", path,
                 static_cast<int>(directory.error()));
    return 1;
  }

  const auto status = directory->validate();
  std::printf("%s: %zu files, validate=%s\n", path, directory->size(),
              status.ok() ? "ok" : "FAILED");

  for (const auto& entry : directory->entries()) {
    std::printf("%u %u %.*s\n", entry.offset, entry.size, static_cast<int>(entry.name.size()),
                entry.name.data());
  }
  return status.ok() ? 0 : 1;
}

int decompress_lzis(const char* path, const char* out_path) {
  const std::vector<std::byte> data = read_file(path);
  auto header = parse_lzis_header(data);
  if (!header) {
    std::fprintf(stderr, "%s: not an LZIS stream (error %d)\n", path,
                 static_cast<int>(header.error()));
    return 1;
  }

  std::vector<std::byte> out(header->uncompressed_size);
  auto written = lzis_decompress(data, out);
  if (!written) {
    std::fprintf(stderr, "%s: decompress failed (error %d)\n", path,
                 static_cast<int>(written.error()));
    return 1;
  }

  std::printf("%s: %zu -> %zu bytes\n", path, data.size(), *written);
  if (out_path != nullptr) {
    std::FILE* file = std::fopen(out_path, "wb");
    if (file == nullptr) return 2;
    std::fwrite(out.data(), 1, *written, file);
    std::fclose(file);
  }
  return 0;
}

/// Every GRID container stored in a pack, found by content rather than by name:
/// 51 decor masks are stored in a file called plainly `PASS`.
int list_grids(const char* pack_path) {
  const std::vector<std::byte> data = read_file(pack_path);
  auto directory = PakDirectory::parse(data);
  if (!directory) return 1;

  int failures = 0;
  std::size_t seen = 0;
  for (const auto& entry : directory->entries()) {
    const auto blob = directory->read(entry);
    if (!blob || blob->size() < 4 || !has_magic(*blob, kGridMagic)) continue;
    ++seen;

    const auto grid = Grid::parse(*blob);
    if (!grid) {
      std::printf("%.*s PARSE_FAILED %d\n", static_cast<int>(entry.name.size()),
                  entry.name.data(), static_cast<int>(grid.error()));
      ++failures;
      continue;
    }
    const auto status = grid->validate(blob->size());

    // Hash every cell in row-major order: the traversal the reference reader
    // materialises as a tuple of tuples. Bits are LSB-first, and reading them
    // the other way round produces plausible-looking output, so nothing short
    // of an exact comparison settles it.
    Fnv hash;
    for (std::uint32_t y = 0; y < grid->height(); ++y) {
      for (std::uint32_t x = 0; x < grid->width(); ++x) hash.u32(grid->cell(x, y));
    }
    std::printf("%.*s %u %u %u %u %u %016llx %s\n", static_cast<int>(entry.name.size()),
                entry.name.data(), grid->cell_size(), grid->bits_per_cell(), grid->width(),
                grid->height(), grid->count_set(), static_cast<unsigned long long>(hash.state),
                status.ok() ? "ok" : "FAILED");
    if (!status) ++failures;
  }
  std::fprintf(stderr, "%s: %zu grids, %d failures\n", pack_path, seen, failures);
  return failures == 0 ? 0 : 1;
}

int list_vq(const char* pack_path) {
  const std::vector<std::byte> data = read_file(pack_path);
  auto directory = PakDirectory::parse(data);
  if (!directory) return 1;

  int failures = 0;
  std::size_t seen = 0;
  std::vector<std::byte> pixels;
  for (const auto& entry : directory->entries()) {
    const auto blob = directory->read(entry);
    if (!blob || blob->size() < 4 || !has_magic(*blob, kVqMagic)) continue;
    ++seen;

    const auto image = VqImage::parse(*blob);
    if (!image) {
      std::printf("%.*s PARSE_FAILED %d\n", static_cast<int>(entry.name.size()),
                  entry.name.data(), static_cast<int>(image.error()));
      ++failures;
      continue;
    }
    const auto status = image->validate();

    pixels.assign(static_cast<std::size_t>(image->width()) * image->height() * 3, std::byte{0});
    const auto decoded = image->decode_rgb888(pixels);
    if (!decoded) ++failures;

    Fnv rgb;
    rgb.bytes(pixels);
    Fnv samples;
    for (std::uint32_t y = 0; y < image->height(); ++y) {
      for (std::uint32_t x = 0; x < image->width(); ++x) samples.u32(image->sample(x, y));
    }
    std::printf("%.*s %u %u %u %u %016llx %016llx %s\n", static_cast<int>(entry.name.size()),
                entry.name.data(), image->width(), image->height(), image->header().codebook_len,
                image->header().index_size, static_cast<unsigned long long>(samples.state),
                static_cast<unsigned long long>(rgb.state),
                (status.ok() && decoded.ok()) ? "ok" : "FAILED");
    if (!status) ++failures;
  }
  std::fprintf(stderr, "%s: %zu textures, %d failures\n", pack_path, seen, failures);
  return failures == 0 ? 0 : 1;
}

/// Sprite frame tables from one pack, decoded against the root-level pixel
/// store. `stride` samples every Nth frame table, because pushing all 196,519
/// frames through the Python reference would take hours.
int list_rle(const char* pack_path, const char* store_path, int stride) {
  const std::vector<std::byte> pack = read_file(pack_path);
  const std::vector<std::byte> store = read_file(store_path);
  auto directory = PakDirectory::parse(pack);
  if (!directory) return 1;

  int failures = 0;
  std::size_t seen = 0;
  std::size_t decoded_frames = 0;
  std::vector<std::byte> rgba;

  for (const auto& entry : directory->entries()) {
    const auto blob = directory->read(entry);
    if (!blob || blob->size() < 6 || !has_magic(*blob, kRleMagic)) continue;
    const std::size_t which = seen++;
    if (stride > 1 && which % static_cast<std::size_t>(stride) != 0) continue;

    const auto image = RleImage::parse(*blob);
    if (!image) {
      std::printf("%.*s PARSE_FAILED %d\n", static_cast<int>(entry.name.size()),
                  entry.name.data(), static_cast<int>(image.error()));
      ++failures;
      continue;
    }
    const auto status = image->validate();
    bool ok = status.ok();

    // Two hashes per file. The span hash is conversion-free — raw run geometry
    // and raw payload bytes — so it compares the decoders rather than their
    // colour maths. The RGBA hash then covers the expansion as well.
    Fnv spans;
    Fnv colours;
    for (const RleFrame& frame : image->frames()) {
      const auto payload = rle_frame_payload(frame, store);
      if (!payload) {
        ok = false;
        break;
      }
      const Status walked = rle_for_each_span(
          frame, *payload,
          [&](std::uint32_t y, std::uint32_t x, std::uint32_t length,
              std::span<const std::byte> run) {
            spans.u32(frame.index);
            spans.u32(y);
            spans.u32(x);
            spans.u32(length);
            spans.bytes(run);
          });
      if (!walked) {
        ok = false;
        break;
      }

      rgba.assign(static_cast<std::size_t>(frame.width) * frame.height * 4, std::byte{0});
      const Status expanded = rle_decode_rgba(*image, frame, *payload, rgba);
      if (!expanded) {
        ok = false;
        break;
      }
      colours.bytes(rgba);
      ++decoded_frames;
    }

    std::uint32_t canvas_width = 0;
    std::uint32_t canvas_height = 0;
    image->canvas_size(canvas_width, canvas_height);
    std::printf("%.*s %u %u %u %zu %u %u %016llx %016llx %s\n",
                static_cast<int>(entry.name.size()), entry.name.data(), image->raw_image_class(),
                image->columns(), image->rows(), image->palette_size(), canvas_width,
                canvas_height, static_cast<unsigned long long>(spans.state),
                static_cast<unsigned long long>(colours.state), ok ? "ok" : "FAILED");
    if (!ok) ++failures;
  }
  std::fprintf(stderr, "%s: %zu frame tables (stride %d), %zu frames decoded, %d failures\n",
               pack_path, seen, stride, decoded_frames, failures);
  return failures == 0 ? 0 : 1;
}

int list_bfhp(const char* path) {
  const std::vector<std::byte> data = read_file(path);
  auto container = BlockFile::open(data);
  if (!container) {
    std::fprintf(stderr, "%s: open failed (error %d)\n", path,
                 static_cast<int>(container.error()));
    return 1;
  }
  const auto status = container->validate();
  auto index = BlockFileIndex::build(*container);
  if (!index) {
    std::fprintf(stderr, "%s: walk failed (error %d)\n", path, static_cast<int>(index.error()));
    return 1;
  }

  std::printf("%s: %u blocks of %u, %zu entries, validate=%s\n", path,
              container->header().block_count, container->header().block_size, index->size(),
              status.ok() ? "ok" : "FAILED");

  std::vector<std::byte> payload;
  int failures = status.ok() ? 0 : 1;
  for (const auto& entry : index->entries()) {
    Fnv hash;
    if (!entry.is_dir) {
      payload.assign(entry.size, std::byte{0});
      const auto read = index->read(entry, payload);
      if (!read) {
        ++failures;
        std::printf("%.*s READ_FAILED\n", static_cast<int>(entry.path.size()),
                    entry.path.data());
        continue;
      }
      hash.bytes(payload);
    }
    std::printf("%u %d %u %016llx %.*s\n", entry.node, entry.is_dir ? 1 : 0, entry.size,
                static_cast<unsigned long long>(hash.state), static_cast<int>(entry.path.size()),
                entry.path.data());
  }
  return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// XML
// ---------------------------------------------------------------------------

std::string upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c == '/') c = '\\';
  }
  return out;
}

bool is_xml_name(std::string_view name) { return upper(name).ends_with(".XML"); }

/// Hash of the parsed tree: every element in document order, its name, its
/// attribute count, and each attribute name and value in document order. The
/// Python side walks `ElementTree.iter()` and `element.attrib`, which are the
/// same two orders, so an equal hash means the two parsers built the same tree
/// -- including the decoding of character references.
std::uint64_t hash_document(const XmlDocument& document) {
  Fnv hash;
  for (std::size_t i = 0; i < document.size(); ++i) {
    const XmlNode& node = document.node(static_cast<NodeIndex>(i));
    hash.bytes(std::as_bytes(std::span(node.name)));
    hash.byte(0);
    hash.u32(node.attribute_count);
    for (std::uint32_t a = 0; a < node.attribute_count; ++a) {
      const XmlAttribute& attribute = document.attributes()[node.attribute_begin + a];
      hash.bytes(std::as_bytes(std::span(attribute.name)));
      hash.byte(0);
      hash.bytes(std::as_bytes(std::span(attribute.value)));
      hash.byte(0);
    }
  }
  return hash.state;
}

bool check_xml(std::string_view name, std::span<const std::byte> data) {
  const auto document = XmlDocument::parse(data);
  if (!document) {
    std::printf("%.*s PARSE_FAILED %d\n", static_cast<int>(name.size()), name.data(),
                static_cast<int>(document.error()));
    return false;
  }
  std::printf("%.*s %zu %zu %.*s %016llx ok\n", static_cast<int>(name.size()), name.data(),
              document->size(), document->attributes().size(),
              static_cast<int>(document->node(document->root()).name.size()),
              document->node(document->root()).name.data(),
              static_cast<unsigned long long>(hash_document(*document)));
  return true;
}

/// Every XML document in a pack, in a `.bfhp` container, or on its own. The
/// acceptance test for the reader is that it parses all of them: 845 class
/// definitions, 889 entity definitions, and the map documents inside the
/// adventure and scenario containers.
int list_xml(const char* path) {
  const std::vector<std::byte> data = read_file(path);
  std::size_t seen = 0;
  std::size_t failures = 0;

  if (has_magic(data, kPakMagic)) {
    auto directory = PakDirectory::parse(data);
    if (!directory) {
      std::fprintf(stderr, "%s: not a readable pack\n", path);
      return 2;
    }
    for (const auto& entry : directory->entries()) {
      if (!is_xml_name(entry.name)) continue;
      ++seen;
      const auto blob = directory->read(entry);
      if (!blob || !check_xml(entry.name, *blob)) ++failures;
    }
  } else if (has_magic(data, kBfhpMagic)) {
    auto container = BlockFile::open(data);
    if (!container) {
      std::fprintf(stderr, "%s: not a readable container\n", path);
      return 2;
    }
    auto index = BlockFileIndex::build(*container);
    if (!index) {
      std::fprintf(stderr, "%s: container walk failed\n", path);
      return 2;
    }
    std::vector<std::byte> payload;
    for (const auto& entry : index->entries()) {
      if (entry.is_dir || !is_xml_name(entry.path)) continue;
      ++seen;
      payload.assign(entry.size, std::byte{0});
      const auto read = index->read(entry, payload);
      if (!read || !check_xml(entry.path, std::span(payload).first(*read))) ++failures;
    }
  } else {
    ++seen;
    if (!check_xml(path, data)) ++failures;
  }

  std::fprintf(stderr, "%s: %zu xml documents, %zu failures\n", path, seen, failures);
  return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// class graph
// ---------------------------------------------------------------------------

/// The pack index, reduced to the one question the core asks of it: does this
/// reference name something that ships? Normalisation matches the reference
/// reader: case- and separator-insensitive, with `gameres/` standing for `UI\`.
class PackProbe : public ResourceProbe {
 public:
  void add(std::string_view name) { names_.push_back(normalise(name)); }
  void seal() {
    std::sort(names_.begin(), names_.end());
    names_.erase(std::unique(names_.begin(), names_.end()), names_.end());
  }
  std::size_t size() const { return names_.size(); }

  bool exists(std::string_view path) const override { return holds(normalise(path)); }

  bool sound_exists(std::string_view value) const override {
    if (value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos) {
      return exists(value);
    }
    return holds(normalise("DATA\\SOUND ENTITIES\\" + std::string(value) + ".XML"));
  }

 private:
  static std::string normalise(std::string_view path) {
    std::size_t begin = 0;
    std::size_t end = path.size();
    while (begin < end && (path[begin] == ' ' || path[begin] == '\t')) ++begin;
    while (end > begin && (path[end - 1] == ' ' || path[end - 1] == '\t')) --end;
    std::string key = upper(path.substr(begin, end - begin));
    if (key.starts_with("GAMERES\\")) key = "UI\\" + key.substr(8);
    return key;
  }

  bool holds(const std::string& key) const {
    return std::binary_search(names_.begin(), names_.end(), key);
  }

  std::vector<std::string> names_;
};

const char* issue_kind_name(ClassIssueKind kind) {
  switch (kind) {
    case ClassIssueKind::dangling_parent: return "parent";
    case ClassIssueKind::cycle: return "cycle";
    case ClassIssueKind::duplicate_id: return "duplicate-id";
    case ClassIssueKind::altid_collision: return "altid-collision";
    case ClassIssueKind::missing_entity: return "entity";
    case ClassIssueKind::missing_script: return "script";
    case ClassIssueKind::missing_sound: return "sound";
    case ClassIssueKind::missing_icon: return "icon";
    case ClassIssueKind::dangling_class_ref: return "class-ref";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// paths, shared by the two subcommands below
// ---------------------------------------------------------------------------

/// Archive paths fold case **and separator**: `Sequences/sequences.xml` and
/// `sequences\\Sequences.XML` are the same entry. The shipped containers spell
/// the same directory both ways, and `data.pak` spells every one of its names
/// in upper case with backslashes while the engine asks for
/// `data/GameScripts/1 Elimination.vs`.
bool equal_nocase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  const auto fold = [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == '\\') return static_cast<unsigned char>('/');
    return (u >= 'A' && u <= 'Z') ? static_cast<unsigned char>(u + 32) : u;
  };
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (fold(left[i]) != fold(right[i])) return false;
  }
  return true;
}

/// `Maps/10/map.xml` -> 10. False for anything else, including `Maps/10/
/// map.obj.xml`, whose name starts the same way.
bool map_number_of(std::string_view path, std::int32_t& number) {
  constexpr std::string_view kPrefix = "Maps/";
  constexpr std::string_view kSuffix = "/map.xml";
  if (path.size() <= kPrefix.size() + kSuffix.size()) return false;
  if (!equal_nocase(path.substr(0, kPrefix.size()), kPrefix)) return false;
  if (!equal_nocase(path.substr(path.size() - kSuffix.size()), kSuffix)) return false;
  const std::string_view digits =
      path.substr(kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
  if (digits.empty()) return false;
  std::int32_t value = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  number = value;
  return true;
}


// ---------------------------------------------------------------------------
// conversations
// ---------------------------------------------------------------------------

/// Parse every `.conv.xml` a container holds and print what each declares.
///
/// One line per conversation and one per phrase, so that a corpus test can
/// assert on the shape of the whole installation's narration -- 110 documents,
/// 260 phrases -- without any of it being copied into this repository. See
/// `tests/test_corpus_conversations.py` and `docs/legal.md` rule 1.
///
/// The localised copies under `Local/<language>/` are skipped. They are
/// `<translationtable>` documents that happen to share the extension, they
/// carry the same conversation names, and reading them would put every
/// conversation in the catalogue twice under a second spelling -- the same trap
/// `Notes.xml` has and the same answer.
int dump_conversations(const char* path) {
  const std::vector<std::byte> data = read_file(path);
  auto container = BlockFile::open(data);
  if (!container) {
    std::fprintf(stderr, "%s: not a readable container\n", path);
    return 2;
  }
  auto index = BlockFileIndex::build(*container);
  if (!index) {
    std::fprintf(stderr, "%s: container walk failed\n", path);
    return 2;
  }

  std::vector<std::byte> payload;
  std::size_t documents = 0;
  std::size_t failures = 0;
  sim::ConversationCatalogue catalogue;

  for (const auto& entry : index->entries()) {
    if (entry.is_dir) continue;
    std::string name(entry.path);
    for (char& c : name) {
      if (c == '\\') c = '/';
    }
    std::string lower = name;
    for (char& c : lower) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    if (lower.size() < 9 || lower.compare(lower.size() - 9, 9, ".conv.xml") != 0) continue;
    if (lower.rfind("local/", 0) == 0) continue;

    ++documents;
    payload.assign(entry.size, std::byte{0});
    const auto read = index->read(entry, payload);
    if (!read) {
      ++failures;
      std::printf("document\t%s\tREAD FAILED\n", name.c_str());
      continue;
    }
    const std::size_t before = catalogue.size();
    const auto added = catalogue.add(std::span(payload).first(*read));
    if (!added) {
      ++failures;
      std::printf("document\t%s\tPARSE FAILED error=%d\n", name.c_str(),
                  static_cast<int>(added.error()));
      continue;
    }
    std::printf("document\t%s\t%zu\n", name.c_str(), *added);
    if (catalogue.size() == before) std::printf("duplicate\t%s\n", name.c_str());
  }

  // Tab-separated, because **a conversation name can contain spaces**: the
  // tutorial's are `1 Welcome`, `A Feed`, `B Stronghold`. A space-separated
  // report of this data is a report nobody can parse, and the first version of
  // it was exactly that.
  for (const sim::ConversationDefinition& conversation : catalogue.all()) {
    const std::string_view startup = conversation_mode_name(conversation.startup);
    std::printf("conv\t%s\tstartup=%.*s\trestore_view=%d\tactors=%zu\tphrases=%zu\tstartlist=%zu\n",
                conversation.name.c_str(), static_cast<int>(startup.size()), startup.data(),
                conversation.restore_view ? 1 : 0, conversation.actors.size(),
                conversation.phrases.size(), conversation.startup_phrases.size());
    for (const std::string& actor : conversation.actors) {
      std::printf("actor\t%s\t%s\n", conversation.name.c_str(), actor.c_str());
    }
    for (const sim::ConversationPhrase& phrase : conversation.phrases) {
      const std::string_view mode = conversation_mode_name(phrase.followup);
      std::printf(
          "phrase\t%s\tlabel=%s\tactor=%s\tfollowup=%.*s\tlist=%zu\tcond=%d\tact=%d\tret=%d"
          "\tchoice=%d\ttext=%zu\n",
          conversation.name.c_str(), phrase.label.empty() ? "-" : phrase.label.c_str(),
          phrase.actor.empty() ? "-" : phrase.actor.c_str(), static_cast<int>(mode.size()),
          mode.data(), phrase.followup_phrases.size(), phrase.condition.empty() ? 0 : 1,
          phrase.action.empty() ? 0 : 1, phrase.result.empty() ? 0 : 1,
          phrase.choice_text.empty() ? 0 : 1, phrase.text.size());
    }
  }

  std::printf("documents\t%zu\n", documents);
  std::printf("failures\t%zu\n", failures);
  std::printf("total\t%zu\n", catalogue.size());
  return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// victory
// ---------------------------------------------------------------------------

/// Compile and run the shipped victory script for one condition, and print
/// what the match did.
///
/// **Also a consequence of `docs/legal.md` rule 1.** `DATA/GAMESCRIPTS/1
/// ELIMINATION.VS` was 2,138 bytes of a string literal in
/// `engine/tests/test_match.cpp`, where it backed the one assertion in that
/// file a stub could not satisfy: that the script the game actually ships
/// compiles against this engine's registry and reaches `EndGame`. A copy of a
/// file can drift from it; this cannot, because it opens `data.pak`.
///
/// The world is the smallest one the script can finish in: two computer
/// players, no town hall, no units. `GetConst("EliminationTimeout")` answers
/// zero without a `CONST.INI`, so the countdown takes its `nTime == 0` branch
/// on the first pass -- the same branch, reached sooner.
int run_victory_script(const char* packs_dir) {
  const std::string directory_path(packs_dir);
  const std::vector<std::byte> data_pak = read_file((directory_path + "/data.pak").c_str());
  auto directory = PakDirectory::parse(data_pak);
  if (!directory) {
    std::fprintf(stderr, "%s/data.pak: parse failed\n", packs_dir);
    return 2;
  }

  const std::string path = sim::victory_script_path(sim::VictoryCondition::elimination);
  std::vector<std::byte> source;
  for (const auto& entry : directory->entries()) {
    if (!equal_nocase(entry.name, path)) continue;
    const auto blob = directory->read(entry);
    if (blob) source.assign(blob->begin(), blob->end());
    break;
  }
  std::printf("script    %s\n", path.c_str());
  if (source.empty()) {
    std::printf("result    NOT FOUND\n");
    return 1;
  }
  std::printf("bytes     %zu\n", source.size());

  sim::World world;
  sim::EnvSystem env;
  sim::MatchSystem match;
  world.seed(1);
  world.add_system(&env);
  world.add_system(&match);
  for (std::size_t i = 0; i < 2; ++i) {
    sim::PlayerSetup& setup = world.players().setup(static_cast<sim::PlayerId>(i));
    setup.control = sim::PlayerControl::computer;
    setup.race = "Gaul";
  }

  sim::MatchRules rules;
  rules.condition = sim::VictoryCondition::elimination;
  rules.param = "0";
  rules.start_player = 0;
  sim::MatchOptions options;
  options.human = 0;
  (void)sim::setup_match(world, match, rules, options);

  script::HostRegistry registry;
  (void)sim::register_all_hosts(registry);
  (void)sim::register_match_host(registry);

  script::Diagnostic diagnostic;
  const auto parsed = script::parse(source, path, &diagnostic);
  if (!parsed.ok()) {
    std::printf("result    PARSE FAILED at line %u: %.*s\n", diagnostic.line,
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
    return 1;
  }
  script::CompileError error;
  auto chunk = script::compile(parsed.value(), &registry, &error);
  if (!chunk.ok()) {
    std::printf("result    COMPILE FAILED at line %u: %s\n", error.line, error.message.c_str());
    return 1;
  }
  std::printf("compiled  ok\n");

  sim::WorldHost host(world);
  script::Scheduler scheduler;
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  sim::HostContext context;
  context.world = &world;
  context.object_type = sim::kTypeObj;
  scheduler.set_user(&context);
  sim::install_objlist_lifetime(scheduler);

  const std::uint32_t index = scheduler.add_chunk(std::move(chunk.value()));
  const script::Value args[] = {script::Value::integer(sim::player_to_script(0)),
                                script::Value::string("0")};
  if (scheduler.spawn(index, args) == script::kNoScript) {
    std::printf("result    SPAWN FAILED\n");
    return 1;
  }

  int traps = 0;
  for (int pass = 0; pass < 8 && scheduler.live_count() > 0; ++pass) {
    const script::RunReport report = scheduler.advance(1000);
    for (const script::FailedScript& trap : report.traps) {
      ++traps;
      std::printf("trap      %s:%u %s\n", trap.source_name.c_str(), trap.trap.line,
                  trap.trap.detail.c_str());
    }
  }
  std::printf("traps     %d\n", traps);
  std::printf("running   %zu\n", scheduler.live_count());
  std::printf("outcome   %d\n", static_cast<int>(match.outcome(0)));
  std::printf("human     %d\n", static_cast<int>(match.human_outcome()));
  std::printf("over      %d\n", match.over(world) ? 1 : 0);
  std::printf("env       %d\n",
              env.env().read_int(sim::EnvScope::for_player(sim::player_to_script(0)),
                                 "elimination"));
  return 0;
}

// ---------------------------------------------------------------------------
// conquest
// ---------------------------------------------------------------------------

/// Read a conquest container's campaign structure and print every fact the
/// reader recovered, one per line.
///
/// **This subcommand exists because `docs/legal.md` rule 1 forbids the
/// alternative.** The claims it prints -- that the territory graph closes, that
/// `bonus` names a root sequence and not a class, that every `mapname` resolves
/// to exactly one `Maps/<n>` -- used to be asserted in `engine/tests` against
/// 5,729 bytes of `territories.xml` pasted into a C++ string literal. Rule 1
/// says no game data in the repository, "not as test fixtures", and that was
/// game data.
///
/// So the claims moved to `tests/test_corpus_campaign.py`, where they are
/// checked against the container on the machine of whoever owns the game and
/// skipped where there is none -- the standing arrangement for every other
/// `test_corpus_*.py`. The unit tests kept the reader's mechanics and got
/// fixtures written from `docs/formats/`. The retail assertions did not weaken
/// in the move: they got stronger, because they now run against the real
/// bytes rather than against a copy of them that could drift.
int dump_conquest(const char* path) {
  const std::vector<std::byte> data = read_file(path);
  auto container = BlockFile::open(data);
  if (!container) {
    std::fprintf(stderr, "%s: not a readable container\n", path);
    return 2;
  }
  auto index = BlockFileIndex::build(*container);
  if (!index) {
    std::fprintf(stderr, "%s: container walk failed\n", path);
    return 2;
  }

  std::vector<std::byte> payload;
  const auto slurp = [&](std::string_view want) {
    std::vector<std::byte> out;
    for (const auto& entry : index->entries()) {
      if (entry.is_dir || !equal_nocase(entry.path, want)) continue;
      payload.assign(entry.size, std::byte{0});
      const auto read = index->read(entry, payload);
      if (read) out.assign(payload.begin(), payload.begin() + static_cast<long>(*read));
      break;
    }
    return out;
  };

  const std::vector<std::byte> territories = slurp("territories.xml");
  if (territories.empty()) {
    std::printf("kind      not a conquest: no territories.xml\n");
    return 0;
  }
  auto conquest = sim::ConquestMap::parse(territories);
  if (!conquest) {
    std::fprintf(stderr, "%s: territories.xml parse failed (error %d)\n", path,
                 static_cast<int>(conquest.error()));
    return 1;
  }

  std::printf("name      %s\n", conquest->name().c_str());
  std::printf("data      %s\n", conquest->data_path().c_str());
  std::printf("choose    %d\n", conquest->choose() ? 1 : 0);
  std::printf("interface %d\n", conquest->interface_id());
  std::printf("order     %s\n", conquest->conquered_order().c_str());
  std::printf("validate  %s\n", conquest->validate().ok() ? "ok" : "FAILED");
  for (const auto& knob : conquest->display()) {
    std::printf("display   %s %d\n", knob.first.c_str(), knob.second);
  }

  for (const sim::Territory& territory : conquest->territories()) {
    std::printf("territory %s index=%d state=%d visual=%s map=%s bonus=%s interface=%d\n",
                territory.id.c_str(), territory.index, static_cast<int>(territory.state),
                territory.visual_name.c_str(), territory.map_name.c_str(),
                territory.bonus.c_str(), territory.interface_id);
    for (const std::string& neighbour : territory.neighbours) {
      std::printf("neighbour %s %s\n", territory.id.c_str(), neighbour.c_str());
    }
  }

  // The container-root manifest, which is what `bonus` has to resolve against.
  std::vector<std::string> sequence_names;
  const std::vector<std::byte> manifest = slurp("Sequences/sequences.xml");
  if (!manifest.empty()) {
    auto sequences = sim::parse_sequences(manifest);
    if (sequences) {
      for (const sim::SequenceRef& sequence : *sequences) {
        sequence_names.push_back(sequence.name);
        std::printf("sequence  %s autorun=%d script=%s\n", sequence.name.c_str(),
                    sequence.autorun_allowed ? 1 : 0, sequence.script.c_str());
      }
    }
  }
  std::printf("bonuses   %s\n",
              conquest->validate_bonuses(std::span(sequence_names)).ok() ? "ok" : "FAILED");

  // Every `Maps/<n>/map.xml`, so that `mapname` can be resolved the way a
  // session resolves it.
  std::vector<std::pair<std::int32_t, std::string>> map_names;
  for (const auto& entry : index->entries()) {
    if (entry.is_dir) continue;
    const std::string name(entry.path);
    std::int32_t number = -1;
    if (!map_number_of(name, number)) continue;
    payload.assign(entry.size, std::byte{0});
    const auto read = index->read(entry, payload);
    if (!read) continue;
    auto doc = XmlDocument::parse(std::span(payload).first(*read));
    if (!doc) continue;
    const NodeIndex root = doc->root();
    if (root == kNoNode) continue;
    map_names.emplace_back(number, std::string(doc->attribute(root, "name")));
  }
  std::sort(map_names.begin(), map_names.end());
  for (const auto& entry : map_names) {
    std::printf("map       %d %s\n", entry.first, entry.second.c_str());
  }

  for (const sim::TerritoryMap& resolved : sim::resolve_maps(*conquest, std::span(map_names))) {
    std::printf("resolved  %d %d\n", resolved.territory, resolved.map_number);
  }
  return 0;
}

/// Load the class graph out of a real installation and print every resolved
/// fact, one per line, sorted by class id. The Python reference can print the
/// same lines, so `diff` is the whole comparison: 845 classes, the same tree,
/// the same merged properties, or a line that says otherwise.
int load_classes(const char* packs_dir) {
  const std::string directory_path(packs_dir);
  const std::vector<std::byte> data_pak = read_file((directory_path + "/data.pak").c_str());
  auto data_directory = PakDirectory::parse(data_pak);
  if (!data_directory) {
    std::fprintf(stderr, "%s/data.pak: parse failed\n", packs_dir);
    return 2;
  }

  // Every pack the object model can refer to. Names only; the payload of 230 MB
  // of art is not needed to answer "does this path exist".
  static constexpr const char* kPackNames[] = {
      "data",  "Units", "Buildings", "MapObjects",    "Visuals", "UI",
      "Sounds", "Terrain", "Outlines", "Minimap", "AdditionalArt", "Fonts"};
  PackProbe probe;
  for (const char* name : kPackNames) {
    const std::string path = directory_path + "/" + name + ".pak";
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) continue;
    std::fclose(file);
    const std::vector<std::byte> pack = read_file(path.c_str());
    auto pack_directory = PakDirectory::parse(pack);
    if (!pack_directory) continue;
    for (const auto& entry : pack_directory->entries()) probe.add(entry.name);
  }
  probe.seal();

  // Sorted, so registration order is the reference reader's order and a
  // duplicate id would be resolved the same way in both.
  std::vector<std::pair<std::string, PakEntry>> class_files;
  for (const auto& entry : data_directory->entries()) {
    const std::string name = upper(entry.name);
    if (!name.starts_with("DATA\\CLASSES\\") || !name.ends_with(".SC.XML")) continue;
    class_files.emplace_back(name, entry);
  }
  std::sort(class_files.begin(), class_files.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  ClassGraph graph;
  std::size_t rejected = 0;
  for (const auto& [name, entry] : class_files) {
    const auto blob = data_directory->read(entry);
    if (!blob) {
      ++rejected;
      std::printf("REJECTED %s read-failed\n", name.c_str());
      continue;
    }
    const auto status = graph.add(*blob, name);
    if (!status) {
      ++rejected;
      std::printf("REJECTED %s %d\n", name.c_str(), static_cast<int>(status.error()));
    }
  }
  graph.link();

  const auto issues = graph.validate(&probe);
  std::printf("classes %zu files %zu rejected %zu roots %zu maxdepth %u issues %zu packentries %zu\n",
              graph.size(), class_files.size(), rejected, graph.roots().size(), graph.max_depth(),
              issues.size(), probe.size());

  static constexpr const char* kSeasonName[] = {"summer", "spring", "autumn", "winter"};

  std::vector<ClassIndex> order;
  for (std::size_t i = 0; i < graph.size(); ++i) order.push_back(static_cast<ClassIndex>(i));
  std::sort(order.begin(), order.end(),
            [&](ClassIndex a, ClassIndex b) { return graph.at(a).id < graph.at(b).id; });

  for (const ClassIndex index : order) {
    const ClassDefinition& definition = graph.at(index);
    const auto text = [](std::string_view view) { return std::string(view); };
    std::printf("C %s cpp=%s parent=%s altid=%s depth=%u children=%zu subtree=%zu src=%s\n",
                text(definition.id).c_str(), text(definition.cpp_class).c_str(),
                definition.parent.empty() ? "-" : text(definition.parent).c_str(),
                definition.altid.empty() ? "-" : text(definition.altid).c_str(),
                definition.depth, graph.children(index).size(), graph.subtree_size(index),
                text(definition.source).c_str());

    for (std::size_t season = 0; season < 4; ++season) {
      const std::string path = text(graph.entity_path(index, static_cast<Season>(season)));
      if (path.empty()) continue;
      std::printf("E %s %s %s\n", text(definition.id).c_str(), kSeasonName[season], path.c_str());
    }
    for (const auto& property : graph.resolved_properties(index)) {
      std::printf("P %s %s = %s\n", text(definition.id).c_str(), text(property.key).c_str(),
                  text(property.value).c_str());
    }
    for (const auto& method : graph.resolved_methods(index)) {
      std::printf("M %s %s vs=%s", text(definition.id).c_str(), text(method.sig).c_str(),
                  text(method.vs).c_str());
      if (!method.verify.empty()) std::printf(" verify=%s", text(method.verify).c_str());
      if (!method.onfinish.empty()) std::printf(" onfinish=%s", text(method.onfinish).c_str());
      std::printf("\n");
    }
    for (const auto& sound : graph.resolved_sounds(index)) {
      std::printf("S %s %s = %s\n", text(definition.id).c_str(), text(sound.key).c_str(),
                  text(sound.value).c_str());
    }
    for (const auto script : graph.resolved_behaviors(index)) {
      std::printf("B %s %s\n", text(definition.id).c_str(), text(script).c_str());
    }
    for (const auto& block : graph.resolved_default_cmds(index)) {
      std::printf("D %s [%s]", text(definition.id).c_str(), text(block.target).c_str());
      for (const auto& cmd : block.cmds) {
        std::printf(" %s%s", text(cmd.name).c_str(), cmd.ctrl ? "+ctrl" : "");
      }
      std::printf("\n");
    }
    const auto values = graph.resolved_values(index);
    for (std::size_t slot = 0; slot < values.size(); ++slot) {
      if (!values[slot].present) continue;
      std::printf("V %s %zu icon=%s script=%s rollover=%s help=%s flags=%s\n",
                  text(definition.id).c_str(), slot, text(values[slot].icon).c_str(),
                  text(values[slot].script).c_str(), text(values[slot].rollover).c_str(),
                  text(values[slot].help).c_str(), text(values[slot].flags).c_str());
    }
  }

  std::vector<std::string> issue_lines;
  for (const auto& issue : issues) {
    issue_lines.push_back(std::string("I ") + issue_kind_name(issue.kind) + " " +
                          std::string(issue.source) + " " + issue.detail);
  }
  std::sort(issue_lines.begin(), issue_lines.end());
  for (const auto& line : issue_lines) std::printf("%s\n", line.c_str());

  std::fprintf(stderr, "%s: %zu classes, %zu rejected, %zu issues\n", packs_dir, graph.size(),
               rejected, issues.size());
  return rejected == 0 ? 0 : 1;
}

/// `imcheck surface` -- the whole host table, one entry per line.
///
/// Exists so that "N of M call sites are implemented" is a number somebody can
/// recompute rather than a number somebody remembered. The output is
///
///     free|member <name> <arity> impl|decl
///
/// sorted, which a script joins against a corpus census to get coverage over
/// whatever corpus it cares about -- and the corpus that matters grew from the
/// 577 scripts in `data.pak` to all 885 in the install once the containers were
/// counted, so a hard-coded denominator was going to be wrong sooner or later.
int dump_surface() {
  using namespace imperivm::core;
  script::HostRegistry registry;
  const std::size_t implemented = sim::register_all_hosts(registry);

  std::vector<std::string> lines;
  lines.reserve(registry.size());
  for (std::size_t i = 0; i < registry.size(); ++i) {
    const script::HostEntry& entry = registry.entry(static_cast<std::uint32_t>(i));
    lines.push_back(std::string(entry.kind == script::CallKind::member ? "member" : "free") + "|" +
                    entry.name + " " + std::to_string(entry.arity) + " " +
                    (entry.fn != nullptr ? "impl" : "decl"));
  }
  std::sort(lines.begin(), lines.end());
  for (const std::string& line : lines) std::printf("%s\n", line.c_str());
  std::printf("# %zu entry points, %zu implemented\n", registry.size(), implemented);
  return 0;
}

/// `imcheck globals` -- the engine's own constant table, one entry per line.
///
/// Same reason as `dump_surface`: a corpus census that wants to know which bare
/// identifiers the engine already answers should read the table the engine was
/// built with, not a list somebody transcribed into a Python file. The output
/// is `<name> <value>` in the table's own order, which is sorted by name --
/// and a table that stopped being sorted would be a table whose binary search
/// silently stopped finding half of it, so printing it in table order rather
/// than re-sorting keeps that visible.
int dump_globals() {
  using namespace imperivm::core;
  for (const sim::GlobalConstant& constant : sim::engine_constants()) {
    std::printf("%.*s %d\n", static_cast<int>(constant.name.size()), constant.name.data(),
                constant.value);
  }
  std::printf("# %zu constants\n", sim::engine_constants().size());
  return 0;
}

/// `imcheck profile <installation> <profile directory>`: read one
/// `Profiles/<name>/player.ini`, aggregate its journal the way the *Change
/// player* screen does, and print the result.
///
/// Its whole reason to exist is `tests/test_corpus_profile.py`: the reference
/// reader in `src/imperivm/formats/profile.py` and this engine's aggregation
/// are two implementations of one specification, and two implementations are
/// only worth having when something compares them. `hash` is the line that
/// matters -- it is the fold over the journal, and the file's own `hash=` is
/// the third opinion both are checked against.
int dump_profile(const char* game_dir, const char* profile_name) {
  const std::string root(game_dir);
  const std::string path = root + "/Profiles/" + profile_name + "/player.ini";
  const std::vector<std::byte> bytes = read_file(path.c_str());
  auto document = IniDocument::parse(bytes);
  if (!document) {
    std::fprintf(stderr, "%s: not a readable ini\n", path.c_str());
    return 2;
  }
  auto profile = game::parse_profile(document.value());
  if (!profile) {
    std::fprintf(stderr, "%s: a [game<n>] record is missing a key\n", path.c_str());
    return 1;
  }

  // The ranks live in `DATA\CONST.INI` inside `data.pak`, which is where the
  // engine reads them from too.
  std::vector<game::Rank> ranks;
  const std::string pak_path = root + "/Packs/data.pak";
  const std::vector<std::byte> pak = read_file(pak_path.c_str());
  auto packed = PakDirectory::parse(pak);
  if (!packed) {
    std::fprintf(stderr, "%s: not a readable pack\n", pak_path.c_str());
    return 2;
  }
  std::vector<std::byte> const_ini;
  if (auto entry = packed->find("DATA/CONST.INI"); entry) {
    if (auto read = packed->read(entry.value()); read) {
      const_ini.assign(read->begin(), read->end());
    }
  }
  auto constants = IniDocument::parse(const_ini);
  if (constants) ranks = game::parse_ranks(constants.value());

  game::aggregate(profile.value(), ranks);
  const game::ProfileStats& stats = profile->stats;
  std::printf("directory %s\n", profile_name);
  std::printf("name      %s\n", profile->name.c_str());
  std::printf("records   %zu\n", profile->journal.size());
  std::printf("games     %d\n", profile->games);
  std::printf("hash      %d\n", static_cast<std::int32_t>(stats.hash));
  std::printf("stored    %s\n",
              profile->has_stored_hash ? std::to_string(profile->stored_hash).c_str() : "none");
  std::printf("rank      %s\n", stats.rank.c_str());
  std::printf("rating    %lld\n", static_cast<long long>(stats.rating()));
  std::printf("single    %u\n", stats.single_games);
  std::printf("single_won %u\n", stats.single_won_percent);
  std::printf("multi     %u\n", stats.multi_games);
  std::printf("multi_won %u\n", stats.multi_won_percent);
  std::printf("hours     %lld\n", static_cast<long long>(stats.hours()));
  std::printf("nation    %d\n", stats.favourite_race);
  std::printf("nation_percent %u\n", stats.favourite_race_percent);
  std::printf("fav       %s\n", profile->favourite_unit.c_str());
  std::printf("gold      %lld\n", static_cast<long long>(stats.gold_spent));
  std::printf("food      %lld\n", static_cast<long long>(stats.food_spent));
  std::printf("killed    %lld\n", static_cast<long long>(stats.units_killed));
  std::printf("lost      %lld\n", static_cast<long long>(stats.units_lost));
  std::printf("health    %lld\n", static_cast<long long>(stats.health_sacrificed));
  std::printf("level     %d\n", stats.best_level);
  std::printf("unit      %s\n", stats.best_unit.c_str());
  std::printf("most_units %d\n", stats.most_units);
  return 0;
}

/// `imcheck interface <Packs directory> [local pack]`: load every screen the
/// installation ships -- every `.INI` under `DATA/INTERFACE/` with a
/// `[<name> Objects]` section -- and say what the interpreter did not
/// understand and which bitmaps and fonts it names that no pack holds. The
/// menu and editor files name art in `UI.pak`, `Fonts.pak` and, for the two
/// menu backgrounds, the language pack; pass `local/<language>.pak` as the
/// second argument to count those as present.
int sweep_interface(const char* packs_dir, const char* local_pack) {
  const std::string directory(packs_dir);
  struct Pack {
    std::string name;
    std::vector<std::byte> bytes;
    PakDirectory directory;
  };
  std::vector<Pack> packs;
  for (const char* name : {"data", "UI", "Fonts", "AdditionalArt"}) {
    const std::string path = directory + "/" + name + ".pak";
    std::vector<std::byte> bytes = read_file(path.c_str());
    if (bytes.empty()) continue;
    auto parsed = PakDirectory::parse(bytes);
    if (!parsed) continue;
    packs.push_back(Pack{name, std::move(bytes), std::move(parsed.value())});
  }
  if (local_pack != nullptr) {
    std::vector<std::byte> bytes = read_file(local_pack);
    if (auto parsed = PakDirectory::parse(bytes); parsed) {
      packs.push_back(Pack{local_pack, std::move(bytes), std::move(parsed.value())});
    }
  }
  if (packs.empty() || packs.front().name != "data") {
    std::fprintf(stderr, "%s/data.pak: not readable\n", packs_dir);
    return 2;
  }
  // The core sees `/`-separated, alias-resolved paths; the packs spell them
  // with backslashes in upper case, and `find` is case-blind either way.
  const ui::FileProvider provider = [&packs](std::string_view path) -> std::span<const std::byte> {
    for (const Pack& pack : packs) {
      if (auto entry = pack.directory.find(path); entry) {
        if (auto bytes = pack.directory.read(entry.value()); bytes) return bytes.value();
      }
    }
    return {};
  };

  std::vector<std::string> files;
  for (const auto& entry : packs.front().directory.entries()) {
    const std::string name = upper(entry.name);
    if (name.starts_with("DATA\\INTERFACE\\") && name.ends_with(".INI")) files.push_back(name);
  }
  std::sort(files.begin(), files.end());

  std::size_t screens = 0;
  std::size_t widgets = 0;
  std::size_t unknown_types = 0;
  std::size_t unknown_styles = 0;
  std::size_t missing_sections = 0;
  std::size_t warnings = 0;
  std::size_t images = 0;
  std::size_t images_missing = 0;
  std::size_t fonts = 0;
  std::size_t fonts_missing = 0;
  std::map<std::string, std::size_t> types;
  std::map<std::string, std::size_t> unknown_type_names;
  std::map<std::string, std::size_t> unknown_style_names;
  std::set<std::string> missing_paths;
  ui::ResourceCache cache(provider);

  for (const std::string& file : files) {
    const auto bytes = packs.front().directory.read(file);
    if (!bytes) continue;
    Result<IniDocument> document = IniDocument::parse(bytes.value());
    if (!document.ok()) {
      std::printf("%s: not an ini\n", file.c_str());
      continue;
    }
    std::string virtual_path;
    for (const char c : file) virtual_path.push_back(c == '\\' ? '/' : c);
    // Every section `[X Objects]` names a screen `[X]`.
    for (std::size_t i = 0; i < document->sections().size(); ++i) {
      const std::string_view section = document->sections()[i].name;
      const std::string_view suffix = " Objects";
      if (section.size() <= suffix.size() ||
          upper(section.substr(section.size() - suffix.size())) != upper(suffix)) {
        continue;
      }
      const std::string_view screen_name = section.substr(0, section.size() - suffix.size());
      Result<ui::Screen> screen = ui::load_screen(virtual_path, screen_name, provider);
      if (!screen.ok()) {
        std::printf("%s [%.*s]: would not load\n", file.c_str(),
                    static_cast<int>(screen_name.size()), screen_name.data());
        continue;
      }
      ++screens;
      widgets += screen->widgets.size();
      missing_sections += screen->missing.size();
      for (const std::string& missing : screen->missing) {
        std::printf("%s [%.*s]: no section [%s]\n", file.c_str(),
                    static_cast<int>(screen_name.size()), screen_name.data(), missing.c_str());
      }
      for (const std::string& warning : screen->warnings) {
        ++warnings;
        std::printf("%s [%.*s]: %s\n", file.c_str(), static_cast<int>(screen_name.size()),
                    screen_name.data(), warning.c_str());
      }
      for (const ui::Widget& widget : screen->widgets) {
        ++types[widget.type];
        if (widget.kind == ui::WidgetType::kUnknown) {
          ++unknown_types;
          ++unknown_type_names[widget.type];
          if (widget.type.empty()) {
            std::printf("%s [%.*s]: [%s] has no Type\n", file.c_str(),
                        static_cast<int>(screen_name.size()), screen_name.data(),
                        widget.name.c_str());
          }
        }
        for (const std::string& style : widget.unknown_styles) {
          ++unknown_styles;
          ++unknown_style_names[style];
        }
        // Every attribute that names a bitmap, by the keys the classes read.
        for (const ui::Attribute& attribute : widget.attributes) {
          const std::string key = upper(attribute.key);
          const bool is_image = key == "IMAGE" || key == "FRAME" || key.ends_with("IMAGE") ||
                                key == "THUMB" || key == "BUTTONS" || key == "PLUSSIGN" ||
                                key == "ICONFRAME" || key == "SCROLLBACK" ||
                                key == "UPBUTTONPRESSED" || key == "DOWNBUTTONPRESSED";
          if (is_image) {
            const ui::ImageRef ref = widget.attribute_image(attribute.key);
            if (ref.empty()) continue;
            ++images;
            if (cache.image(ref) == nullptr) {
              ++images_missing;
              missing_paths.insert(ref.path);
            }
          } else if (key == "FONT" || key == "BOLDFONT") {
            ++fonts;
            if (cache.font(attribute.value) == nullptr) {
              ++fonts_missing;
              missing_paths.insert(ui::resolve_alias(attribute.value));
            }
          }
        }
      }
    }
  }
  for (const auto& [name, count] : types) std::printf("type %s %zu\n", name.c_str(), count);
  for (const auto& [name, count] : unknown_type_names) {
    std::printf("unknown-type %s %zu\n", name.c_str(), count);
  }
  for (const auto& [name, count] : unknown_style_names) {
    std::printf("unknown-style %s %zu\n", name.c_str(), count);
  }
  for (const std::string& path : missing_paths) std::printf("missing %s\n", path.c_str());
  std::printf("# files %zu screens %zu widgets %zu unknown-types %zu unknown-styles %zu "
              "missing-sections %zu warnings %zu images %zu (missing %zu) fonts %zu (missing %zu)\n",
              files.size(), screens, widgets, unknown_types, unknown_styles, missing_sections,
              warnings, images, images_missing, fonts, fonts_missing);
  return 0;
}

}  // namespace

/// `imcheck objects <container> <path> [output]`: the writer against the
/// reader over one `map.obj.xml` -- parse, write, compare with the stored
/// bytes, parse the written text again and compare the tables. `output`
/// receives the regenerated document. Exit 0 when the bytes are identical,
/// 3 when only the tables agree, 1 on any failure.
int check_objects(const char* container_path, const char* entry, const char* output) {
  const std::vector<std::byte> data = read_file(container_path);
  auto container = BlockFile::open(data);
  if (!container) {
    std::fprintf(stderr, "%s: open failed (error %d)\n", container_path,
                 static_cast<int>(container.error()));
    return 1;
  }
  auto index = BlockFileIndex::build(*container);
  if (!index) {
    std::fprintf(stderr, "%s: walk failed\n", container_path);
    return 1;
  }
  const auto found = index->find(entry);
  if (!found.ok()) {
    std::fprintf(stderr, "%s: no %s\n", container_path, entry);
    return 1;
  }
  std::vector<std::byte> stored(found.value().size);
  if (!index->read(found.value(), stored).ok()) {
    std::fprintf(stderr, "%s: cannot read %s\n", container_path, entry);
    return 1;
  }
  const auto parsed = imperivm::core::MapObjectList::parse(stored);
  if (!parsed.ok()) {
    std::fprintf(stderr, "%s: %s does not parse (error %d)\n", container_path, entry,
                 static_cast<int>(parsed.error()));
    return 1;
  }
  const std::string written = imperivm::core::write_map_objects(parsed.value());
  if (output != nullptr) {
    if (FILE* file = std::fopen(output, "wb")) {
      std::fwrite(written.data(), 1, written.size(), file);
      std::fclose(file);
    }
  }
  std::vector<std::byte> again(written.size());
  std::memcpy(again.data(), written.data(), written.size());
  const auto reparsed = imperivm::core::MapObjectList::parse(again);
  if (!reparsed.ok()) {
    std::fprintf(stderr, "%s: the written document does not parse\n", entry);
    return 1;
  }
  const auto& a = parsed.value();
  const auto& b = reparsed.value();
  bool same = a.objects().size() == b.objects().size() &&
              a.settlements().size() == b.settlements().size() &&
              a.groups().size() == b.groups().size() && a.areas().size() == b.areas().size();
  for (std::size_t i = 0; same && i < a.objects().size(); ++i) {
    const auto& x = a.objects()[i];
    const auto& y = b.objects()[i];
    same = x.class_name == y.class_name && x.num == y.num && x.x == y.x && x.y == y.y &&
           x.dir_x == y.dir_x && x.dir_y == y.dir_y && x.flags == y.flags &&
           x.player == y.player && x.settlement == y.settlement &&
           x.health_percent == y.health_percent && x.health_absolute == y.health_absolute &&
           x.unit_flags == y.unit_flags && x.destination_set == y.destination_set &&
           x.attributes == y.attributes;
  }
  for (std::size_t i = 0; same && i < a.settlements().size(); ++i) {
    const auto& x = a.settlements()[i];
    const auto& y = b.settlements()[i];
    same = x.name == y.name && x.class_of_first_building == y.class_of_first_building &&
           x.id == y.id && x.player == y.player && x.population == y.population &&
           x.max_population == y.max_population && x.gold == y.gold && x.food == y.food &&
           x.max_gold == y.max_gold && x.max_food == y.max_food &&
           x.extra_sentries == y.extra_sentries && x.attributes == y.attributes;
  }
  for (std::size_t i = 0; same && i < a.groups().size(); ++i) {
    same = a.groups()[i].name == b.groups()[i].name && a.groups()[i].type == b.groups()[i].type &&
           a.groups()[i].members == b.groups()[i].members;
  }
  for (std::size_t i = 0; same && i < a.areas().size(); ++i) {
    const auto& x = a.areas()[i];
    const auto& y = b.areas()[i];
    same = x.num == y.num && x.type == y.type && x.ptx == y.ptx && x.pty == y.pty &&
           x.radius == y.radius && x.left == y.left && x.top == y.top && x.right == y.right &&
           x.bottom == y.bottom;
  }
  const bool identical = stored.size() == written.size() &&
                         std::memcmp(stored.data(), written.data(), stored.size()) == 0;
  std::printf("%s %s: %zu objects, %zu settlements, %zu groups, %zu areas; %zu -> %zu bytes; "
              "tables %s; bytes %s\n",
              container_path, entry, a.objects().size(), a.settlements().size(),
              a.groups().size(), a.areas().size(), stored.size(), written.size(),
              same ? "equal" : "DIFFER", identical ? "identical" : "differ");
  if (!same) return 1;
  return identical ? 0 : 3;
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "surface") return dump_surface();
  if (argc == 2 && std::string(argv[1]) == "globals") return dump_globals();
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: imcheck pak   <archive>\n"
                 "       imcheck lzis  <stream> [output]\n"
                 "       imcheck grid  <archive>\n"
                 "       imcheck vq    <archive>\n"
                 "       imcheck rle   <archive> <rle.mmp> [stride]\n"
                 "       imcheck bfhp  <container>\n"
                 "       imcheck xml     <archive|container|file>\n"
                 "       imcheck classes <Packs directory>\n"
                 "       imcheck conquest <container>\n"
                 "       imcheck profile  <installation> <profile directory>\n"
                 "       imcheck victory  <Packs directory>\n"
                 "       imcheck conv     <container>\n"
                 "       imcheck vs      <archive>\n"
                 "       imcheck interface <Packs directory> [local pack]\n"
                 "       imcheck objects <container> <Maps/n/map.obj.xml> [output]\n"
                 "       imcheck surface\n"
                 "       imcheck globals\n");
    return 2;
  }
  const std::string command = argv[1];
  if (command == "pak") return list_pak(argv[2]);
  if (command == "lzis") return decompress_lzis(argv[2], argc > 3 ? argv[3] : nullptr);
  if (command == "grid") return list_grids(argv[2]);
  if (command == "vq") return list_vq(argv[2]);
  if (command == "rle") {
    if (argc < 4) {
      std::fprintf(stderr, "rle needs the path of the pixel store\n");
      return 2;
    }
    return list_rle(argv[2], argv[3], argc > 4 ? std::atoi(argv[4]) : 1);
  }
  if (command == "bfhp") return list_bfhp(argv[2]);
  if (command == "xml") return list_xml(argv[2]);
  if (command == "classes") return load_classes(argv[2]);
  if (command == "conquest") return dump_conquest(argv[2]);
  if (command == "profile") {
    if (argc < 4) {
      std::fprintf(stderr, "usage: imcheck profile <installation> <profile directory>\n");
      return 2;
    }
    return dump_profile(argv[2], argv[3]);
  }
  if (command == "victory") return run_victory_script(argv[2]);
  if (command == "conv") return dump_conversations(argv[2]);
  if (command == "interface") return sweep_interface(argv[2], argc > 3 ? argv[3] : nullptr);
  if (command == "objects") {
    if (argc < 4) {
      std::fprintf(stderr, "objects needs the container and the document's path in it\n");
      return 2;
    }
    return check_objects(argv[2], argv[3], argc > 4 ? argv[4] : nullptr);
  }
  if (command == "vs") {
    const std::vector<std::byte> archive = read_file(argv[2]);
    return imperivm::vsdump::run(archive, argv[2]);
  }
  std::fprintf(stderr, "unknown command '%s'\n", command.c_str());
  return 2;
}
