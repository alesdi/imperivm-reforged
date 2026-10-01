#include "imperivm/gamedata/map_source.hpp"

#include <algorithm>
#include <cstdio>

#include "imperivm/core/formats/lzis.hpp"

namespace imperivm::gamedata {
namespace {

std::vector<std::byte> read_whole_file(const std::filesystem::path& path) {
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

bool has_magic(const std::vector<std::byte>& data, std::string_view magic) {
  if (data.size() < magic.size()) return false;
  for (std::size_t i = 0; i < magic.size(); ++i) {
    if (static_cast<char>(data[i]) != magic[i]) return false;
  }
  return true;
}

/// Case- and separator-insensitive comparison, matching `BlockFileIndex::find`.
bool path_less(std::string_view a, std::string_view b) {
  const auto fold = [](char c) -> char {
    if (c == '\\') return '/';
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
  };
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const char x = fold(a[i]);
    const char y = fold(b[i]);
    if (x != y) return x < y;
  }
  return a.size() < b.size();
}

}  // namespace

bool MapContainer::open(const std::filesystem::path& path, std::string* error) {
  path_ = path;
  bytes_ = read_whole_file(path);
  if (bytes_.empty()) {
    if (error != nullptr) *error = path.string() + ": cannot read";
    return false;
  }

  if (has_magic(bytes_, core::kLzisMagic)) {
    const auto header = core::parse_lzis_header(bytes_);
    if (!header) {
      if (error != nullptr) *error = path.string() + ": malformed LZIS header";
      return false;
    }
    std::vector<std::byte> plain(header->uncompressed_size);
    if (!core::lzis_decompress(bytes_, plain)) {
      if (error != nullptr) *error = path.string() + ": LZIS stream did not decode";
      return false;
    }
    bytes_ = std::move(plain);
  }

  auto container = core::BlockFile::open(bytes_);
  if (!container) {
    if (error != nullptr) *error = path.string() + ": not a .bfhp container";
    return false;
  }
  file_ = container.value();

  auto index = core::BlockFileIndex::build(file_);
  if (!index) {
    if (error != nullptr) *error = path.string() + ": directory tree is unreadable";
    return false;
  }
  index_ = std::move(index.value());
  return true;
}

std::vector<std::string> MapContainer::list() const {
  std::vector<std::string> out;
  out.reserve(index_.size());
  for (const auto& entry : index_.entries()) {
    if (!entry.is_dir) out.emplace_back(entry.path);
  }
  return out;
}

std::vector<std::string> MapContainer::directories() const {
  std::vector<std::string> out;
  for (const auto& entry : index_.entries()) {
    if (entry.is_dir) out.emplace_back(entry.path);
  }
  return out;
}

std::vector<std::string> MapContainer::map_directories() const {
  std::vector<std::string> out;
  for (const auto& entry : index_.entries()) {
    if (entry.is_dir) continue;
    const std::string_view path = entry.path;
    // `Maps/<n>/map.xml` is the marker: a directory with no map document in it
    // is not a playable map, whatever it is called.
    if (path.size() < 8 || path.compare(path.size() - 8, 8, "/map.xml") != 0) continue;
    const std::string_view dir = path.substr(0, path.size() - 8);
    if (dir.size() < 5 || path_less(dir.substr(0, 5), "Maps/") ||
        path_less("Maps/", dir.substr(0, 5))) {
      continue;
    }
    out.emplace_back(dir);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    // Ascending by the map number, not lexicographically: `Maps/10` follows
    // `Maps/9`.
    const auto number = [](const std::string& p) {
      long value = 0;
      for (std::size_t i = 5; i < p.size() && p[i] >= '0' && p[i] <= '9'; ++i) {
        value = value * 10 + (p[i] - '0');
      }
      return value;
    };
    return number(a) < number(b);
  });
  return out;
}

bool MapContainer::contains(std::string_view path) const {
  return index_.find(path).ok();
}

std::vector<std::byte> MapContainer::read(std::string_view path) const {
  const auto entry = index_.find(path);
  if (!entry) return {};
  std::vector<std::byte> out(entry->size);
  if (out.empty()) return out;
  const auto read = index_.read(entry.value(), out);
  if (!read) return {};
  out.resize(read.value());
  return out;
}

}  // namespace imperivm::gamedata
