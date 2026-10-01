#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/bfhp.hpp"

namespace imperivm::gamedata {

/// A `.bfhp` scenario container opened from disk.
///
/// The container is a small read/write virtual filesystem, not an archive, and
/// the whole file is occasionally LZIS-wrapped — `Packs/RandomMap.pak` is the
/// famous case, but nothing stops a scenario being shipped that way, so the
/// wrapper is unwrapped here rather than assumed absent.
///
/// This is the platform half of map loading exactly as the architecture
/// prescribes: open the file, gather the payloads, hand spans to
/// `imperivm::core::WorldMap`. Nothing here interprets a byte.
class MapContainer {
 public:
  MapContainer() = default;
  MapContainer(const MapContainer&) = delete;
  MapContainer& operator=(const MapContainer&) = delete;

  bool open(const std::filesystem::path& path, std::string* error = nullptr);

  [[nodiscard]] bool is_open() const noexcept { return !bytes_.empty(); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  /// Every stored path, `/`-separated, in directory-walk order.
  [[nodiscard]] std::vector<std::string> list() const;
  /// Every stored directory, likewise, parents before their children: what a
  /// copy has to recreate before it can place the files.
  [[nodiscard]] std::vector<std::string> directories() const;

  /// `Maps/<n>` directories that hold a `map.xml`, ascending by number. The
  /// numbers are **not** contiguous — the conquest uses 3, 4 and 6 to 10 — so
  /// they are enumerated, never counted.
  [[nodiscard]] std::vector<std::string> map_directories() const;

  [[nodiscard]] bool contains(std::string_view path) const;

  /// A stored file's bytes, or an empty vector. Copies: a node's payload is
  /// scattered across blocks, so there is nothing contiguous to view.
  [[nodiscard]] std::vector<std::byte> read(std::string_view path) const;

 private:
  std::filesystem::path path_;
  std::vector<std::byte> bytes_;
  core::BlockFile file_;
  core::BlockFileIndex index_;
};

}  // namespace imperivm::gamedata
