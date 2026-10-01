#pragma once

// Reader for the HMMSYS block container (`.bfhp`, magic `HPFS`).
//
// Specification: docs/formats/bfhp.md
//
// Every scenario, adventure, campaign map and saved game the player actually
// plays is one of these. Unlike a pack, which is a read-only archive, this is a
// small read/write virtual filesystem: a flat array of fixed-size blocks
// holding a directory tree, with node blocks and an indirect block map.
//
// The one rule that matters most for a reader: **never scan a node's block map
// for a zero terminator**. A level-1 node keeps the direct block list it had
// before it was promoted, so its tail is non-zero, stale and meaningless — 439
// of 439 level-1 nodes in the retail set look like this. The pointer count
// always comes from `size`.
//
// Two other traps the layout sets: `block_size` is 4096 in one shipped
// container and 512 in the rest, so it cannot be assumed; and the file on disk
// is truncated at the last byte actually used, so the final block is usually
// short.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

#include <string>

namespace imperivm::core {

inline constexpr std::string_view kBfhpMagic = "HPFS";
inline constexpr std::size_t kBfhpHeaderSize = 32;
inline constexpr std::size_t kBfhpDirEntryHeaderSize = 8;

/// The root directory's node is block 1 in every retail container.
inline constexpr std::uint32_t kBfhpRootNode = 1;

/// Deepest directory nesting the walker will follow. The retail containers
/// reach 4; the limit exists so a container whose directory points at itself
/// cannot recurse until the stack runs out.
inline constexpr std::uint32_t kBfhpMaxDepth = 32;

struct BfhpHeader {
  std::uint32_t block_size = 0;
  std::uint32_t block_count = 0;
  std::int32_t free_head = -1;      ///< -1 in every retail container
  std::uint32_t reserved_a = 0;
  std::uint32_t reserved_b = 0;
  std::uint32_t unknown = 0;        ///< 8 in every retail container
  std::uint32_t root_node = kBfhpRootNode;

  constexpr std::uint32_t pointers_per_block() const noexcept { return block_size / 4; }
};

/// The first two words of a node block.
struct BfhpNode {
  std::uint32_t size = 0;   ///< payload length in bytes
  std::uint32_t level = 0;  ///< indirection: 0 = direct block list, 1 = index blocks
};

/// A non-owning view of a container: block access, node decoding and payload
/// gathering, with no allocation of its own.
class BlockFile {
 public:
  static Result<BlockFile> open(std::span<const std::byte> data);

  const BfhpHeader& header() const noexcept { return header_; }
  std::span<const std::byte> data() const noexcept { return data_; }

  /// One block. The final block of the file is legitimately short, because the
  /// writer truncates at the last byte used rather than padding.
  Result<std::span<const std::byte>> block(std::uint32_t index) const;

  Result<BfhpNode> node(std::uint32_t block_index) const;

  /// Data blocks a node's payload occupies: `ceil(size / block_size)`.
  Result<std::uint32_t> node_block_count(std::uint32_t block_index) const;

  /// The `n`th data block of a node, resolving one level of indirection.
  Result<std::uint32_t> node_data_block(std::uint32_t block_index, std::uint32_t n) const;

  /// Gather a node's whole payload into `out`, which must hold `size` bytes.
  /// Returns the bytes written.
  Result<std::size_t> read_node(std::uint32_t block_index, std::span<std::byte> out) const;

  /// Header-level invariants: magic, a power-of-two block size, a root inside
  /// the array, and a physical length that lands inside the final block.
  Status validate() const;

 private:
  std::span<const std::byte> data_{};
  BfhpHeader header_{};
};

/// One record of a directory payload.
struct BfhpDirEntry {
  std::uint32_t node = 0;
  bool is_dir = false;
  std::string_view name;  ///< cp1252 bytes, no terminator; views the payload
};

/// Walk the packed records of a directory payload.
///
/// The list ends at the payload's end or earlier at a record whose node is 0.
/// `fn(entry)` is called for each; a false return from `fn` stops the walk.
template <class F>
Status bfhp_for_each_entry(std::span<const std::byte> payload, F&& fn) {
  std::size_t position = 0;
  while (position + kBfhpDirEntryHeaderSize <= payload.size()) {
    BfhpDirEntry entry;
    entry.node = static_cast<std::uint32_t>(payload[position]) |
                 (static_cast<std::uint32_t>(payload[position + 1]) << 8) |
                 (static_cast<std::uint32_t>(payload[position + 2]) << 16) |
                 (static_cast<std::uint32_t>(payload[position + 3]) << 24);
    if (entry.node == 0) break;

    const std::uint32_t kind = static_cast<std::uint32_t>(payload[position + 4]) |
                               (static_cast<std::uint32_t>(payload[position + 5]) << 8);
    const std::uint32_t name_len = static_cast<std::uint32_t>(payload[position + 6]) |
                                   (static_cast<std::uint32_t>(payload[position + 7]) << 8);
    if (kind > 1) return FormatError::malformed;
    position += kBfhpDirEntryHeaderSize;
    if (name_len > payload.size() - position) return FormatError::truncated;

    entry.is_dir = kind == 1;
    entry.name = std::string_view(reinterpret_cast<const char*>(payload.data() + position),
                                  name_len);
    position += name_len;
    if (!fn(entry)) break;
  }
  return {};
}

/// The whole directory tree, walked once and kept: paths in one flat buffer,
/// entries in one vector. Lookup is case-insensitive and accepts either
/// separator, because the shipped data is inconsistent about both.
class BlockFileIndex {
 public:
  static Result<BlockFileIndex> build(const BlockFile& container);

  struct Entry {
    std::string_view path;  ///< `/`-separated, no leading slash
    std::uint32_t node = 0;
    std::uint32_t size = 0;  ///< payload bytes; 0 for a directory
    bool is_dir = false;
  };

  const BlockFile& container() const noexcept { return container_; }
  const std::vector<Entry>& entries() const noexcept { return entries_; }
  std::size_t size() const noexcept { return entries_.size(); }

  Result<Entry> find(std::string_view path) const;

  /// Read a stored file into `out`, which must hold `entry.size` bytes.
  Result<std::size_t> read(const Entry& entry, std::span<std::byte> out) const;

 private:
  BlockFile container_{};
  std::vector<Entry> entries_;
  std::vector<char> paths_;
};

// --------------------------------------------------------------------------
// writing
// --------------------------------------------------------------------------

/// Assembles a container the way the engine's own writer does.
///
/// A port of `src/imperivm/formats/bfhp.py`'s `BlockFileBuilder`, which is
/// ground truth: fed the extraction of a retail container in creation order,
/// that builder reproduces it byte for byte, and this one reproduces that
/// builder. `tests/test_corpus_imsave.py` holds the two together over a save
/// this class wrote.
///
/// **Creation order is the layout.** `add` does three things in a fixed
/// sequence -- allocate the entry's node block, append its record to the
/// parent directory (which may grow the parent), then write the entry's
/// content -- and that sequence is what puts every block where it is. Two
/// trees with the same entries added in a different order are two different
/// files that read back the same. A caller that wants a particular layout
/// therefore adds in a particular order, and a caller that wants a stable one
/// adds in a fixed order.
///
/// One buffer serves every index block and is deliberately never cleared
/// between nodes: the retail writer's, and a level-1 node's stale tail is the
/// visible consequence (see the reader's note above).
///
/// Why the core carries a writer at all: a saved game is one of these. The
/// original mounts `AdvSave/` from `currentadv.bfhp` and persists the whole
/// mounted filesystem, and this engine's saves take the same shape -- the
/// session's bytes beside a manifest, and a conquest's rewritten
/// `territories.xml` beside those. `engine/gamedata` puts the file on disk;
/// this builds the bytes.
class BlockFileBuilder {
 public:
  static constexpr std::uint32_t kDefaultBlockSize = 512;

  /// `block_size` must be a power of two no smaller than the header.
  explicit BlockFileBuilder(std::uint32_t block_size = kDefaultBlockSize);

  /// Create one entry at `path` -- `/`-separated, no leading slash, every
  /// parent directory already added. A directory is added with `directory`;
  /// a file with `file`. A name is cp1252 bytes as given, at most 0xFFFF of
  /// them, with no `\` or NUL; two names differing only in case are one
  /// name, because the engine's lookup folds case. Refused as `malformed`
  /// when the parent is missing, the name is unusable or the path is taken.
  Status directory(std::string_view path);
  Status file(std::string_view path, std::span<const std::byte> payload);

  /// The finished container, truncated at the last byte written rather than
  /// at a block boundary, which is what the engine does and why the final
  /// block on disk is usually short. May be called more than once.
  [[nodiscard]] std::vector<std::byte> build() const;

  [[nodiscard]] std::uint32_t block_size() const noexcept { return block_size_; }
  [[nodiscard]] std::uint32_t block_count() const noexcept {
    return static_cast<std::uint32_t>(blocks_.size());
  }

 private:
  /// One node block plus the payload hanging off it, written incrementally.
  struct Node {
    std::uint32_t block = 0;
    std::vector<std::uint32_t> words;
    std::uint32_t size = 0;
    std::uint32_t level = 0;
    std::vector<std::uint32_t> data_blocks;
    std::vector<std::uint32_t> index_blocks;
  };

  std::uint32_t allocate();
  void put(std::uint32_t block, std::span<const std::byte> payload, std::uint32_t offset = 0);
  void flush_index(std::uint32_t block);
  void flush_node(const Node& node);
  Status grow(Node& node);
  Status append(Node& node, std::span<const std::byte> payload);
  Node make_node();
  Status add(std::string_view path, const std::span<const std::byte>* payload);

  std::uint32_t block_size_ = kDefaultBlockSize;
  std::uint32_t pointers_per_block_ = kDefaultBlockSize / 4;
  std::vector<std::vector<std::byte>> blocks_;
  std::vector<std::uint32_t> used_;
  std::vector<std::uint32_t> index_buffer_;
  /// Directories by upper-cased path, `""` for the root. Indices into
  /// `nodes_`, which only grows.
  std::vector<std::pair<std::string, std::size_t>> directories_;
  std::vector<Node> nodes_;
  std::vector<std::string> names_;  ///< every path added, upper-cased
};

}  // namespace imperivm::core
