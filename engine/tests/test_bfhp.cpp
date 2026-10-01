// Block container tests.
//
// The one that matters most is `bfhp_ignores_the_stale_tail_of_a_level_one_node`:
// a promoted node keeps its old direct block list in the words past the index
// block pointers, so a reader that scans for a zero terminator walks straight
// into stale data. All 439 level-1 nodes in the retail set look like this, and
// the test below plants deliberately poisonous values there.

#include <vector>

#include "builder.hpp"
#include "imperivm/core/formats/bfhp.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

constexpr std::uint32_t kBlockSize = 64;
constexpr std::uint32_t kPointers = kBlockSize / 4;  // 16

/// Writes into a container laid out as a flat array of blocks.
class Container {
 public:
  explicit Container(std::uint32_t blocks) : blocks_(blocks) { data_.zeros(blocks * kBlockSize); }

  void header(std::uint32_t root = kBfhpRootNode, std::uint32_t block_size = kBlockSize) {
    std::size_t at = 0;
    const auto put = [&](std::uint32_t value) {
      data_.patch_u32(at, value);
      at += 4;
    };
    data_.raw()[0] = std::byte{'H'};
    data_.raw()[1] = std::byte{'P'};
    data_.raw()[2] = std::byte{'F'};
    data_.raw()[3] = std::byte{'S'};
    at = 4;
    put(block_size);
    put(blocks_);
    put(0xFFFFFFFF);  // free_head, -1 in every retail container
    put(0);
    put(0);
    put(8);
    put(root);
  }

  void word(std::uint32_t block, std::uint32_t index, std::uint32_t value) {
    data_.patch_u32(block * kBlockSize + index * 4, value);
  }

  void byte_at(std::uint32_t block, std::uint32_t offset, std::uint8_t value) {
    data_.patch_u8(block * kBlockSize + offset, value);
  }

  /// A directory record: node index, kind, name.
  void record(std::uint32_t block, std::uint32_t offset, std::uint32_t node, std::uint32_t kind,
              std::string_view name) {
    const std::size_t at = block * kBlockSize + offset;
    data_.patch_u32(at, node);
    data_.patch_u8(at + 4, kind);
    data_.patch_u8(at + 5, 0);
    data_.patch_u8(at + 6, static_cast<std::uint32_t>(name.size()));
    data_.patch_u8(at + 7, 0);
    for (std::size_t i = 0; i < name.size(); ++i) {
      data_.patch_u8(at + 8 + i, static_cast<std::uint8_t>(name[i]));
    }
  }

  /// The engine truncates the file at the last byte actually used rather than
  /// padding the final block, so containers almost never end on a boundary.
  void truncate(std::uint32_t used_in_last_block) {
    data_.raw().resize((blocks_ - 1) * kBlockSize + used_in_last_block);
  }

  std::span<const std::byte> span() const { return data_.span(); }

 private:
  Builder data_;
  std::uint32_t blocks_;
};

/// Blocks: 0 header, 1 root node, 2 root directory data, 3 file node, 4 file
/// data holding "world".
Container simple_container() {
  Container container(5);
  container.header();

  container.word(1, 0, 13);  // root directory payload: one 13-byte record
  container.word(1, 1, 0);   // level 0
  container.word(1, 2, 2);   // its only data block
  container.record(2, 0, 3, 0, "hello");

  container.word(3, 0, 5);  // the file's size
  container.word(3, 1, 0);
  container.word(3, 2, 4);
  const char payload[] = "world";
  for (std::uint32_t i = 0; i < 5; ++i) {
    container.byte_at(4, i, static_cast<std::uint8_t>(payload[i]));
  }
  container.truncate(5);
  return container;
}

bool equals(std::span<const std::byte> data, std::string_view text) {
  if (data.size() != text.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (static_cast<char>(data[i]) != text[i]) return false;
  }
  return true;
}

}  // namespace

TEST(bfhp_opens_a_container) {
  const Container container = simple_container();
  const auto opened = BlockFile::open(container.span());
  CHECK(opened.ok());
  CHECK(opened->header().block_size == kBlockSize);
  CHECK(opened->header().block_count == 5);
  CHECK(opened->header().root_node == 1);
  CHECK(opened->header().free_head == -1);
  // The physical length lands inside the final block, not on a boundary.
  CHECK(opened->validate().ok());
}

TEST(bfhp_walks_the_directory_tree) {
  const Container container = simple_container();
  const auto opened = BlockFile::open(container.span());
  const auto index = BlockFileIndex::build(*opened);
  CHECK(index.ok());
  CHECK(index->size() == 1);
  CHECK(index->entries()[0].path == "hello");
  CHECK(index->entries()[0].size == 5);
  CHECK(!index->entries()[0].is_dir);
}

TEST(bfhp_reads_a_stored_file) {
  const Container container = simple_container();
  const auto opened = BlockFile::open(container.span());
  const auto index = BlockFileIndex::build(*opened);
  std::vector<std::byte> out(5);
  const auto read = index->read(index->entries()[0], out);
  CHECK(read.ok());
  CHECK(*read == 5);
  CHECK(equals(out, "world"));
}

TEST(bfhp_lookup_ignores_case_and_separator) {
  const Container container = simple_container();
  const auto opened = BlockFile::open(container.span());
  const auto index = BlockFileIndex::build(*opened);
  CHECK(index->find("HELLO").ok());
  CHECK(index->find("nothing").error() == FormatError::not_found);
}

TEST(bfhp_builds_nested_paths) {
  // 0 header, 1 root node, 2 root data, 3 dir node, 4 dir data, 5 file node,
  // 6 file data.
  Container container(7);
  container.header();
  container.word(1, 0, 12);  // one directory record: 8 header + 4 name
  container.word(1, 2, 2);
  container.record(2, 0, 3, 1, "Maps");

  container.word(3, 0, 15);  // 8 header + 7 name
  container.word(3, 2, 4);
  container.record(4, 0, 5, 0, "map.xml");

  container.word(5, 0, 2);
  container.word(5, 2, 6);
  container.byte_at(6, 0, 'h');
  container.byte_at(6, 1, 'i');
  container.truncate(2);

  const auto opened = BlockFile::open(container.span());
  const auto index = BlockFileIndex::build(*opened);
  REQUIRE(index.ok());
  REQUIRE(index->size() == 2);
  // Depth first, descending as soon as a directory record is read.
  CHECK(index->entries()[0].path == "Maps");
  CHECK(index->entries()[0].is_dir);
  CHECK(index->entries()[1].path == "Maps/map.xml");
  CHECK(index->find("maps\\MAP.XML").ok());
}

TEST(bfhp_ignores_the_stale_tail_of_a_level_one_node) {
  // A file of 900 bytes needs 15 data blocks. The direct list holds 14, so the
  // node was promoted: word 2 became an index block pointer and words 3 onward
  // kept the old direct list. Those stale words are poisoned here — a reader
  // that scanned for a zero terminator would follow them out of the array.
  //
  // Blocks: 0 header, 1 root node, 2 root data, 3 file node, 4 index block,
  // 5..19 the fifteen data blocks.
  Container container(20);
  container.header();
  container.word(1, 0, 12);
  container.word(1, 2, 2);
  container.record(2, 0, 3, 0, "big");

  container.word(3, 0, 900);
  container.word(3, 1, 1);  // level 1
  container.word(3, 2, 4);  // the index block
  for (std::uint32_t i = 3; i < kPointers; ++i) container.word(3, i, 0xDEADBEEF);
  for (std::uint32_t i = 0; i < 15; ++i) container.word(4, i, 5 + i);
  // The unused tail of the index block is stale in the same way.
  container.word(4, 15, 0xDEADBEEF);

  for (std::uint32_t i = 0; i < 900; ++i) {
    container.byte_at(5 + i / kBlockSize, i % kBlockSize, static_cast<std::uint8_t>(i & 0xFF));
  }
  container.truncate(900 - 14 * kBlockSize);

  const auto opened = BlockFile::open(container.span());
  CHECK(opened->validate().ok());
  const auto node = opened->node(3);
  CHECK(node.ok());
  CHECK(node->level == 1);
  CHECK(*opened->node_block_count(3) == 15);
  CHECK(*opened->node_data_block(3, 14) == 19);
  CHECK(opened->node_data_block(3, 15).error() == FormatError::out_of_range);

  std::vector<std::byte> out(900);
  const auto read = opened->read_node(3, out);
  CHECK(read.ok());
  CHECK(*read == 900);
  bool intact = true;
  for (std::uint32_t i = 0; i < 900; ++i) {
    if (static_cast<std::uint8_t>(out[i]) != (i & 0xFF)) intact = false;
  }
  CHECK(intact);
}

TEST(bfhp_rejects_a_foreign_file) {
  Builder other;
  other.text("SFPH").zeros(64);
  CHECK(BlockFile::open(other.span()).error() == FormatError::bad_magic);
}

TEST(bfhp_rejects_a_block_size_that_is_not_a_power_of_two) {
  Container container = simple_container();
  Builder patched;
  for (const std::byte b : container.span()) patched.u8(static_cast<std::uint8_t>(b));
  patched.patch_u32(4, 100);
  CHECK(BlockFile::open(patched.span()).error() == FormatError::malformed);
}

TEST(bfhp_rejects_a_root_outside_the_block_array) {
  Container container(5);
  container.header(99);
  CHECK(BlockFile::open(container.span()).error() == FormatError::out_of_range);
}

TEST(bfhp_rejects_indirection_it_does_not_understand) {
  Container container = simple_container();
  container.word(3, 1, 2);  // level 2 never occurs in the retail data
  const auto opened = BlockFile::open(container.span());
  CHECK(opened->node(3).error() == FormatError::unsupported);
}

TEST(bfhp_rejects_a_data_block_outside_the_array) {
  Container container = simple_container();
  container.word(3, 2, 77);
  const auto opened = BlockFile::open(container.span());
  std::vector<std::byte> out(5);
  CHECK(opened->read_node(3, out).error() == FormatError::out_of_range);
}

TEST(bfhp_rejects_a_directory_that_points_at_itself) {
  Container container(3);
  container.header();
  container.word(1, 0, 12);
  container.word(1, 2, 2);
  container.record(2, 0, 1, 1, "loop");  // names the root as its own child
  container.truncate(12);

  const auto opened = BlockFile::open(container.span());
  CHECK(BlockFileIndex::build(*opened).error() == FormatError::malformed);
}

TEST(bfhp_rejects_an_unknown_entry_kind) {
  Container container = simple_container();
  container.byte_at(2, 4, 7);  // kind, which is 0 or 1
  const auto opened = BlockFile::open(container.span());
  CHECK(BlockFileIndex::build(*opened).error() == FormatError::malformed);
}

TEST(bfhp_rejects_a_name_running_past_the_payload) {
  Container container = simple_container();
  container.byte_at(2, 6, 200);  // name_len
  const auto opened = BlockFile::open(container.span());
  CHECK(BlockFileIndex::build(*opened).error() == FormatError::truncated);
}

TEST(bfhp_stops_a_directory_list_at_a_zero_node) {
  Container container = simple_container();
  container.word(2, 0, 0);  // the list ends early at a zero node
  const auto opened = BlockFile::open(container.span());
  const auto index = BlockFileIndex::build(*opened);
  CHECK(index.ok());
  CHECK(index->size() == 0);
}

TEST(bfhp_reads_an_empty_container) {
  // `currentadv.bfhp` is exactly this: a formatted but unwritten save slot,
  // two blocks and a zero root node.
  Container container(2);
  container.header();
  const auto opened = BlockFile::open(container.span());
  CHECK(opened.ok());
  const auto index = BlockFileIndex::build(*opened);
  CHECK(index.ok());
  CHECK(index->size() == 0);
}

// --------------------------------------------------------------------------
// the writer
// --------------------------------------------------------------------------
//
// `BlockFileBuilder` is a port of the Python builder, which reproduces every
// retail container byte for byte from its own extraction. These mirror
// `tests/test_bfhp_container.py` case for case, so that the two builders are
// held to the same facts; `tests/test_corpus_imsave.py` then holds the two to
// each other over a save this builder wrote.

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::byte>(text[i]);
  return out;
}

std::vector<std::byte> fill(std::size_t count, char c) {
  return std::vector<std::byte>(count, static_cast<std::byte>(c));
}

/// Build, open and index in one go; the index is what a reader sees.
struct Built {
  std::vector<std::byte> raw;
  BlockFile file;
  BlockFileIndex index;
};

Built open_built(const BlockFileBuilder& builder) {
  Built out;
  out.raw = builder.build();
  const auto opened = BlockFile::open(out.raw);
  CHECK(opened.ok());
  CHECK(opened->validate().ok());
  out.file = *opened;
  auto index = BlockFileIndex::build(out.file);
  CHECK(index.ok());
  // Moved, not copied: an entry's `path` views the index's own buffer, and a
  // copy of the index would leave every view pointing into the original.
  out.index = std::move(index.value());
  return out;
}

std::vector<std::byte> read_back(const Built& built, std::string_view path) {
  const auto entry = built.index.find(path);
  CHECK(entry.ok());
  if (!entry.ok()) return {};
  std::vector<std::byte> out(entry->size);
  const auto read = built.index.read(*entry, out);
  CHECK(read.ok());
  CHECK(*read == entry->size);
  return out;
}

/// 64,512 bytes: the largest payload a 512-byte-block node holds directly.
constexpr std::size_t kDirectLimit = (512 / 4 - 2) * 512;

}  // namespace

TEST(bfhp_writer_makes_an_unwritten_save_slot_from_nothing) {
  // `currentadv.bfhp` in a fresh install: two 4096-byte blocks, a header and a
  // zero root node, and the file is exactly the two blocks long because the
  // root node's flush writes its whole block.
  BlockFileBuilder builder(4096);
  const Built built = open_built(builder);
  CHECK(built.raw.size() == 8192);
  CHECK(built.file.header().block_size == 4096);
  CHECK(built.file.header().block_count == 2);
  CHECK(built.file.header().free_head == -1);
  CHECK(built.file.header().unknown == 8);
  CHECK(built.file.header().root_node == kBfhpRootNode);
  CHECK(built.index.size() == 0);
}

TEST(bfhp_writer_tree_survives_a_round_trip) {
  BlockFileBuilder builder;
  REQUIRE(builder.directory("Conversations").ok());
  REQUIRE(builder.file("game.xml", bytes_of("<game/>")).ok());
  REQUIRE(builder.directory("Maps").ok());
  REQUIRE(builder.directory("Maps/1").ok());
  REQUIRE(builder.file("Maps/1/map.xml", bytes_of("<map/>")).ok());
  REQUIRE(builder.file("Maps/1/empty.bin", {}).ok());
  const Built built = open_built(builder);
  REQUIRE(built.index.size() == 6);
  const auto& e = built.index.entries();
  CHECK(e[0].path == "Conversations" && e[0].is_dir && e[0].size == 0);
  CHECK(e[1].path == "game.xml" && !e[1].is_dir && e[1].size == 7);
  CHECK(e[2].path == "Maps" && e[2].is_dir);
  CHECK(e[3].path == "Maps/1" && e[3].is_dir);
  CHECK(e[4].path == "Maps/1/map.xml" && !e[4].is_dir && e[4].size == 6);
  CHECK(e[5].path == "Maps/1/empty.bin" && !e[5].is_dir && e[5].size == 0);
  CHECK(equals(read_back(built, "maps/1/MAP.XML"), "<map/>"));
}

TEST(bfhp_writer_switches_indirection_over_at_the_documented_size) {
  {
    BlockFileBuilder builder;
    REQUIRE(builder.file("f", fill(kDirectLimit, 'a')).ok());
    const Built built = open_built(builder);
    const auto node = built.file.node(built.index.entries()[0].node);
    REQUIRE(node.ok());
    CHECK(node->level == 0);
    CHECK(*built.file.node_block_count(built.index.entries()[0].node) == 126);
    // header, root node, root data, file node, 126 data blocks.
    CHECK(built.file.header().block_count == 130);
  }
  {
    BlockFileBuilder builder;
    REQUIRE(builder.file("f", fill(kDirectLimit + 1, 'a')).ok());
    const Built built = open_built(builder);
    const auto node = built.file.node(built.index.entries()[0].node);
    REQUIRE(node.ok());
    CHECK(node->level == 1);
    CHECK(*built.file.node_block_count(built.index.entries()[0].node) == 127);
    // ...plus the one index block the promotion allocated.
    CHECK(built.file.header().block_count == 132);
    CHECK(read_back(built, "f") == fill(kDirectLimit + 1, 'a'));
  }
}

TEST(bfhp_writer_multi_index_file_reads_back_intact) {
  // 307,200 bytes: five index blocks, and the index buffer is shared between
  // them without being cleared, which is what the reader's stale-tail rule
  // is about.
  std::vector<std::byte> payload;
  payload.reserve(256 * 1200);
  for (int i = 0; i < 1200; ++i) {
    for (int b = 0; b < 256; ++b) payload.push_back(static_cast<std::byte>(b));
  }
  BlockFileBuilder builder;
  REQUIRE(builder.file("big.bin", payload).ok());
  const Built built = open_built(builder);
  const auto node = built.file.node(built.index.entries()[0].node);
  REQUIRE(node.ok());
  CHECK(node->level == 1);
  CHECK(read_back(built, "big.bin") == payload);
}

TEST(bfhp_writer_stops_the_file_at_the_last_byte_used) {
  // 700 bytes spill 188 bytes into a second data block, and the file ends
  // there rather than at a block boundary.
  BlockFileBuilder builder;
  REQUIRE(builder.file("f.bin", fill(700, 'x')).ok());
  const Built built = open_built(builder);
  CHECK(built.raw.size() == (built.file.header().block_count - 1) * 512 + 188);
}

TEST(bfhp_writer_lays_blocks_out_in_creation_order) {
  // 0 header, 1 root node, 2 dir node, 3 root data, 4 file node, 5 dir data,
  // 6 file data -- measured on the reference builder over this tree. It is
  // **not** the tidier layout the reader's hand-built fixture uses (root data
  // at 2, the directory's node at 3), because `add` allocates the child's node
  // *before* appending its record to the parent, and the parent's first data
  // block is allocated by that append. The block numbers are the evidence
  // that this port keeps the writer's order rather than a sensible one.
  BlockFileBuilder builder(64);
  REQUIRE(builder.directory("dir").ok());
  REQUIRE(builder.file("dir/file", bytes_of("world")).ok());
  const Built built = open_built(builder);
  CHECK(built.file.header().block_count == 7);
  REQUIRE(built.index.size() == 2);
  CHECK(built.index.entries()[0].node == 2);
  CHECK(built.index.entries()[1].node == 4);
  CHECK(*built.file.node_data_block(1, 0) == 3);
  CHECK(*built.file.node_data_block(2, 0) == 5);
  CHECK(*built.file.node_data_block(4, 0) == 6);
  CHECK(equals(read_back(built, "dir/file"), "world"));
}

TEST(bfhp_writer_refuses_unusable_names_and_missing_parents) {
  BlockFileBuilder builder;
  CHECK(builder.file("", bytes_of("x")).error() == FormatError::malformed);
  CHECK(builder.file("a/", bytes_of("x")).error() == FormatError::malformed);
  CHECK(builder.file("bad\\name", bytes_of("x")).error() == FormatError::malformed);
  CHECK(builder.file(std::string_view("nul\0x", 5), bytes_of("x")).error() ==
        FormatError::malformed);
  CHECK(builder.file("nowhere/f", bytes_of("x")).error() == FormatError::malformed);
  REQUIRE(builder.file("Game.xml", bytes_of("x")).ok());
  // Two names differing only in case are one name to the engine's lookup.
  CHECK(builder.file("GAME.XML", bytes_of("y")).error() == FormatError::malformed);
  // A refusal leaves nothing behind: the accepted entry is still the only one.
  const Built built = open_built(builder);
  CHECK(built.index.size() == 1);
}
