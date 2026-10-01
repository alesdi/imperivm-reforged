#include "imperivm/core/formats/bfhp.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "imperivm/core/formats/byte_reader.hpp"

namespace imperivm::core {
namespace {

constexpr char normalise(char c) noexcept {
  if (c == '\\') return '/';
  if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
  return c;
}

bool paths_match(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (normalise(a[i]) != normalise(b[i])) return false;
  }
  return true;
}

constexpr std::uint32_t divide_rounding_up(std::uint32_t value, std::uint32_t divisor) noexcept {
  return value == 0 ? 0u : (value - 1) / divisor + 1;
}

}  // namespace

Result<BlockFile> BlockFile::open(std::span<const std::byte> data) {
  if (!has_magic(data, kBfhpMagic)) return FormatError::bad_magic;
  if (data.size() < kBfhpHeaderSize) return FormatError::truncated;

  BlockFile container;
  container.data_ = data;
  BfhpHeader& header = container.header_;
  header.block_size = read_u32le(data, 4);
  header.block_count = read_u32le(data, 8);
  header.free_head = static_cast<std::int32_t>(read_u32le(data, 12));
  header.reserved_a = read_u32le(data, 16);
  header.reserved_b = read_u32le(data, 20);
  header.unknown = read_u32le(data, 24);
  header.root_node = read_u32le(data, 28);

  // A block has to hold at least the header and a whole number of pointers,
  // and the pointer maths below assumes a power of two.
  if (header.block_size < kBfhpHeaderSize || (header.block_size & (header.block_size - 1)) != 0) {
    return FormatError::malformed;
  }
  if (header.block_count == 0 || header.root_node >= header.block_count) {
    return FormatError::out_of_range;
  }
  return container;
}

Result<std::span<const std::byte>> BlockFile::block(std::uint32_t index) const {
  if (index >= header_.block_count) return FormatError::out_of_range;
  const std::uint64_t start = static_cast<std::uint64_t>(index) * header_.block_size;
  if (start >= data_.size()) return FormatError::truncated;
  // The last block on disk is short: the writer stops at the last byte used
  // rather than padding out to a block boundary.
  const std::uint64_t available = data_.size() - start;
  const std::uint64_t length = available < header_.block_size ? available : header_.block_size;
  return data_.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(length));
}

Result<BfhpNode> BlockFile::node(std::uint32_t block_index) const {
  const Result<std::span<const std::byte>> raw = block(block_index);
  if (!raw) return raw.error();
  // A node block always sits wholly inside the file: it is never the truncated
  // final block, because a node is written before the payload that follows it.
  if (raw->size() < 8) return FormatError::truncated;

  BfhpNode result;
  result.size = read_u32le(*raw, 0);
  result.level = read_u32le(*raw, 4);
  if (result.level > 1) return FormatError::unsupported;  // level 2 never occurs
  return result;
}

Result<std::uint32_t> BlockFile::node_block_count(std::uint32_t block_index) const {
  const Result<BfhpNode> info = node(block_index);
  if (!info) return info.error();
  return divide_rounding_up(info->size, header_.block_size);
}

Result<std::uint32_t> BlockFile::node_data_block(std::uint32_t block_index,
                                                 std::uint32_t n) const {
  const Result<BfhpNode> info = node(block_index);
  if (!info) return info.error();

  const std::uint32_t pointers = header_.pointers_per_block();
  const std::uint32_t needed = divide_rounding_up(info->size, header_.block_size);
  if (n >= needed) return FormatError::out_of_range;

  const Result<std::span<const std::byte>> words = block(block_index);
  if (!words) return words.error();
  if (words->size() < header_.block_size) return FormatError::truncated;

  if (info->level == 0) {
    if (2 + needed > pointers) return FormatError::malformed;
    return read_u32le(*words, 4 * (2 + n));
  }

  // Level 1: words 2.. are index blocks, each a full block of data block
  // indices. The count comes from `size` — the tail of the node is a stale
  // direct list left over from before the node was promoted, and scanning it
  // for a terminator would walk straight into it.
  const std::uint32_t slot = n / pointers;
  if (2 + divide_rounding_up(needed, pointers) > pointers) return FormatError::malformed;
  const std::uint32_t index_block = read_u32le(*words, 4 * (2 + slot));

  const Result<std::span<const std::byte>> table = block(index_block);
  if (!table) return table.error();
  if (table->size() < header_.block_size) return FormatError::truncated;
  return read_u32le(*table, 4 * (n % pointers));
}

Result<std::size_t> BlockFile::read_node(std::uint32_t block_index,
                                         std::span<std::byte> out) const {
  const Result<BfhpNode> info = node(block_index);
  if (!info) return info.error();
  if (out.size() < info->size) return FormatError::buffer_too_small;

  const std::uint32_t needed = divide_rounding_up(info->size, header_.block_size);
  std::size_t written = 0;
  for (std::uint32_t n = 0; n < needed; ++n) {
    const Result<std::uint32_t> index = node_data_block(block_index, n);
    if (!index) return index.error();
    const Result<std::span<const std::byte>> source = block(*index);
    if (!source) return source.error();

    const std::size_t take =
        std::min<std::size_t>(info->size - written, source->size());
    if (take == 0) return FormatError::truncated;
    std::memcpy(out.data() + written, source->data(), take);
    written += take;
  }
  if (written != info->size) return FormatError::truncated;
  return written;
}

Status BlockFile::validate() const {
  const std::uint64_t span = static_cast<std::uint64_t>(header_.block_size) * header_.block_count;
  // The physical length lands inside the final block, never on a boundary
  // below it: the writer truncates at the last byte the last file used.
  if (data_.size() > span || data_.size() <= span - header_.block_size) {
    return FormatError::malformed;
  }
  return {};
}

Result<BlockFileIndex> BlockFileIndex::build(const BlockFile& container) {
  BlockFileIndex index;
  index.container_ = container;

  // Directory payloads are gathered into this scratch buffer, one at a time.
  std::vector<std::size_t> path_starts;
  std::vector<std::size_t> path_lengths;
  // Guards against a container whose directory tree contains a cycle: a node
  // may be entered as a directory at most once. Without this, a `.bfhp` whose
  // directory lists itself would recurse until the stack ran out.
  std::vector<bool> entered(container.header().block_count, false);
  entered[container.header().root_node] = true;

  // Depth first, descending into a directory as soon as its record is read, so
  // entries come out in the order the tree stores them. Iteration order is
  // state in this engine, so it is worth being deliberate about.
  const auto walk = [&](std::uint32_t node, const std::string& prefix, std::uint32_t depth,
                        auto&& self) -> FormatError {
    const Result<BfhpNode> info = container.node(node);
    if (!info) return info.error();

    // The payload is gathered per level rather than into one shared scratch
    // buffer, because the walk descends while it is still reading this one.
    std::vector<std::byte> payload(info->size, std::byte{0});
    if (info->size != 0) {
      const Result<std::size_t> read = container.read_node(node, payload);
      if (!read) return read.error();
    }

    FormatError failure = FormatError::none;
    const Status status = bfhp_for_each_entry(payload, [&](const BfhpDirEntry& entry) {
      if (entry.node >= container.header().block_count) {
        failure = FormatError::out_of_range;
        return false;
      }
      const Result<BfhpNode> child = container.node(entry.node);
      if (!child) {
        failure = child.error();
        return false;
      }

      const std::size_t start = index.paths_.size();
      index.paths_.insert(index.paths_.end(), prefix.begin(), prefix.end());
      index.paths_.insert(index.paths_.end(), entry.name.begin(), entry.name.end());
      // The views are attached after the walk, once the buffer stops growing.
      path_starts.push_back(start);
      path_lengths.push_back(index.paths_.size() - start);

      Entry stored;
      stored.node = entry.node;
      stored.is_dir = entry.is_dir;
      stored.size = entry.is_dir ? 0u : child->size;
      index.entries_.push_back(stored);

      if (entry.is_dir) {
        if (depth + 1 >= kBfhpMaxDepth || entered[entry.node]) {
          failure = FormatError::malformed;
          return false;
        }
        entered[entry.node] = true;
        const FormatError nested =
            self(entry.node, prefix + std::string(entry.name) + "/", depth + 1, self);
        if (nested != FormatError::none) {
          failure = nested;
          return false;
        }
      }
      return true;
    });
    if (!status) return status.error();
    return failure;
  };

  const FormatError failure = walk(container.header().root_node, std::string(), 0, walk);
  if (failure != FormatError::none) return failure;

  for (std::size_t i = 0; i < index.entries_.size(); ++i) {
    index.entries_[i].path =
        std::string_view(index.paths_.data() + path_starts[i], path_lengths[i]);
  }
  return index;
}

Result<BlockFileIndex::Entry> BlockFileIndex::find(std::string_view path) const {
  for (const Entry& entry : entries_) {
    if (paths_match(entry.path, path)) return entry;
  }
  return FormatError::not_found;
}

Result<std::size_t> BlockFileIndex::read(const Entry& entry, std::span<std::byte> out) const {
  if (entry.is_dir) return FormatError::malformed;
  return container_.read_node(entry.node, out);
}

// --------------------------------------------------------------------------
// writing
// --------------------------------------------------------------------------
//
// Every step below is the Python builder's step in the Python builder's order,
// including the ones that look like accidents: the index buffer that is never
// cleared, the node flushed before its size is final, the header patched last.
// A port that tidied any of them would write a container the reader accepts
// and the reference does not reproduce, and byte-identity with the reference
// is the whole claim.

namespace {

void write_u32le(std::span<std::byte> out, std::size_t at, std::uint32_t value) noexcept {
  out[at] = static_cast<std::byte>(value & 0xFFu);
  out[at + 1] = static_cast<std::byte>((value >> 8) & 0xFFu);
  out[at + 2] = static_cast<std::byte>((value >> 16) & 0xFFu);
  out[at + 3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

/// `str.upper()` over the bytes, which for the ASCII the shipped names use is
/// the same fold the reader's `normalise` applies.
std::string upper_of(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

}  // namespace

BlockFileBuilder::BlockFileBuilder(std::uint32_t block_size)
    : block_size_(block_size), pointers_per_block_(block_size / 4) {
  // A block size that is not a power of two, or too small for the header, is a
  // programming error rather than input: the reference raises, and there is no
  // container to be had. The builder degrades to the default rather than
  // producing one the reader would refuse.
  if (block_size_ < kBfhpHeaderSize || (block_size_ & (block_size_ - 1)) != 0) {
    block_size_ = kDefaultBlockSize;
    pointers_per_block_ = block_size_ / 4;
  }
  index_buffer_.assign(pointers_per_block_, 0);
  // Block 0, the header: zeros now, patched by `build`.
  (void)allocate();
  const std::vector<std::byte> header(kBfhpHeaderSize, std::byte{0});
  put(0, header);
  // Block 1, the root directory's node. `make_node` allocates and flushes.
  nodes_.push_back(make_node());
  directories_.emplace_back(std::string(), 0);
}

std::uint32_t BlockFileBuilder::allocate() {
  blocks_.emplace_back(block_size_, std::byte{0});
  used_.push_back(0);
  return static_cast<std::uint32_t>(blocks_.size() - 1);
}

void BlockFileBuilder::put(std::uint32_t block, std::span<const std::byte> payload,
                           std::uint32_t offset) {
  std::copy(payload.begin(), payload.end(), blocks_[block].begin() + offset);
  used_[block] = std::max(used_[block], offset + static_cast<std::uint32_t>(payload.size()));
}

void BlockFileBuilder::flush_index(std::uint32_t block) {
  std::vector<std::byte> packed(block_size_);
  for (std::uint32_t i = 0; i < pointers_per_block_; ++i) {
    write_u32le(packed, 4 * i, index_buffer_[i]);
  }
  put(block, packed);
}

void BlockFileBuilder::flush_node(const Node& node) {
  std::vector<std::byte> packed(block_size_);
  for (std::uint32_t i = 0; i < pointers_per_block_; ++i) {
    write_u32le(packed, 4 * i, node.words[i]);
  }
  put(node.block, packed);
}

BlockFileBuilder::Node BlockFileBuilder::make_node() {
  Node node;
  node.block = allocate();
  node.words.assign(pointers_per_block_, 0);
  // `words[0]` is the size and `words[1]` the level, both zero here; the
  // reference flushes the fresh node before anything hangs off it.
  flush_node(node);
  return node;
}

Status BlockFileBuilder::grow(Node& node) {
  const std::uint32_t pointers = pointers_per_block_;
  const std::uint32_t direct_capacity = pointers - 2;

  const std::uint32_t block = allocate();
  const std::uint32_t position = static_cast<std::uint32_t>(node.data_blocks.size());
  node.data_blocks.push_back(block);

  if (node.level == 0 && position < direct_capacity) {
    node.words[2 + position] = block;
    flush_node(node);
    return Status();
  }

  if (node.level == 0) {
    // Promotion. The direct list moves into the shared index buffer, the new
    // block joins it, and word 2 -- and only word 2 -- becomes the index block
    // index. Words 3 onward keep the old direct list, which is the stale tail
    // the reader is warned never to scan.
    for (std::uint32_t i = 0; i < direct_capacity; ++i) index_buffer_[i] = node.words[2 + i];
    index_buffer_[direct_capacity] = block;
    node.level = 1;
    node.words[1] = 1;
    const std::uint32_t index_block = allocate();
    node.index_blocks.push_back(index_block);
    node.words[2] = index_block;
    flush_node(node);
    flush_index(index_block);
    return Status();
  }

  const std::uint32_t slot = position / pointers;
  if (slot >= node.index_blocks.size()) {
    const std::uint32_t index_block = allocate();
    node.index_blocks.push_back(index_block);
    // A payload needing a second level of indirection is one the format has
    // never been seen to hold and this writer does not produce.
    if (2 + slot >= pointers) return Status(FormatError::unsupported);
    node.words[2 + slot] = index_block;
    flush_node(node);
  }
  index_buffer_[position % pointers] = block;
  flush_index(node.index_blocks[slot]);
  return Status();
}

Status BlockFileBuilder::append(Node& node, std::span<const std::byte> payload) {
  std::size_t written = 0;
  while (written < payload.size()) {
    const std::uint32_t offset = node.size % block_size_;
    if (offset == 0) {
      if (const Status status = grow(node); !status.ok()) return status;
    }
    const std::size_t take =
        std::min<std::size_t>(block_size_ - offset, payload.size() - written);
    put(node.data_blocks[node.size / block_size_], payload.subspan(written, take), offset);
    written += take;
    node.size += static_cast<std::uint32_t>(take);
  }
  node.words[0] = node.size;
  node.words[1] = node.level;
  flush_node(node);
  return Status();
}

Status BlockFileBuilder::directory(std::string_view path) { return add(path, nullptr); }

Status BlockFileBuilder::file(std::string_view path, std::span<const std::byte> payload) {
  return add(path, &payload);
}

Status BlockFileBuilder::add(std::string_view path, const std::span<const std::byte>* payload) {
  // `path.rpartition("/")`.
  const std::size_t cut = path.rfind('/');
  const std::string_view parent_path = cut == std::string_view::npos ? std::string_view() : path.substr(0, cut);
  const std::string_view name = cut == std::string_view::npos ? path : path.substr(cut + 1);

  const std::string parent_key = upper_of(parent_path);
  std::size_t parent_index = nodes_.size();
  for (const auto& [key, index] : directories_) {
    if (key == parent_key) {
      parent_index = index;
      break;
    }
  }
  if (parent_index == nodes_.size()) return Status(FormatError::malformed);

  // `_check_name`.
  if (name.empty()) return Status(FormatError::malformed);
  if (name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
    return Status(FormatError::malformed);
  }
  if (name.size() > 0xFFFFu) return Status(FormatError::malformed);
  const std::string key = upper_of(path);
  if (std::find(names_.begin(), names_.end(), key) != names_.end()) {
    return Status(FormatError::malformed);
  }
  names_.push_back(key);

  // The three steps, in the writer's order: the child's node block, then the
  // record in the parent, then the child's own content.
  nodes_.push_back(make_node());
  const std::size_t child_index = nodes_.size() - 1;

  std::vector<std::byte> record(kBfhpDirEntryHeaderSize + name.size());
  write_u32le(record, 0, nodes_[child_index].block);
  const std::uint32_t kind = payload == nullptr ? 1u : 0u;
  record[4] = static_cast<std::byte>(kind & 0xFFu);
  record[5] = static_cast<std::byte>((kind >> 8) & 0xFFu);
  record[6] = static_cast<std::byte>(name.size() & 0xFFu);
  record[7] = static_cast<std::byte>((name.size() >> 8) & 0xFFu);
  std::memcpy(record.data() + kBfhpDirEntryHeaderSize, name.data(), name.size());
  if (const Status status = append(nodes_[parent_index], record); !status.ok()) return status;

  if (payload == nullptr) {
    directories_.emplace_back(key, child_index);
    return Status();
  }
  return append(nodes_[child_index], *payload);
}

std::vector<std::byte> BlockFileBuilder::build() const {
  const std::uint32_t count = static_cast<std::uint32_t>(blocks_.size());
  std::vector<std::byte> header(kBfhpHeaderSize);
  header[0] = std::byte{'H'};
  header[1] = std::byte{'P'};
  header[2] = std::byte{'F'};
  header[3] = std::byte{'S'};
  write_u32le(header, 4, block_size_);
  write_u32le(header, 8, count);
  write_u32le(header, 12, 0xFFFFFFFFu);  // free_head, -1 in every retail container
  write_u32le(header, 16, 0);
  write_u32le(header, 20, 0);
  write_u32le(header, 24, 8);  // the unknown field, 8 in every retail container
  write_u32le(header, 28, kBfhpRootNode);

  std::vector<std::byte> out;
  out.reserve(static_cast<std::size_t>(count) * block_size_);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::vector<std::byte>& block = blocks_[i];
    if (i == 0) {
      out.insert(out.end(), header.begin(), header.end());
      out.insert(out.end(), block.begin() + static_cast<std::ptrdiff_t>(kBfhpHeaderSize),
                 block.end());
    } else if (i + 1 == count) {
      // The file stops at the last byte actually written rather than at a
      // block boundary, so the final block on disk is usually short.
      out.insert(out.end(), block.begin(), block.begin() + used_[i]);
    } else {
      out.insert(out.end(), block.begin(), block.end());
    }
  }
  return out;
}

}  // namespace imperivm::core
