#include "imperivm/gamedata/campaign_file.hpp"

#include <cstdio>
#include <system_error>
#include <utility>
#include <vector>

#include "imperivm/core/world/map.hpp"

namespace imperivm::gamedata {
namespace {

void fail(std::string* error, std::string text) {
  if (error != nullptr) *error = std::move(text);
}

}  // namespace

std::filesystem::path campaign_file_path(const std::filesystem::path& user_dir,
                                         std::string_view container_relative) {
  const std::filesystem::path container{std::string(container_relative)};
  return user_dir / (container.stem().string() + ".campaign.ini");
}

bool write_campaign_file(const std::filesystem::path& path, const core::sim::CampaignCarry& carry,
                         std::string* error) {
  const std::vector<std::byte> bytes = core::sim::encode_campaign_carry(carry);
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    fail(error, "cannot write " + path.string());
    return false;
  }
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  std::fclose(file);
  if (!ok) {
    fail(error, "short write to " + path.string());
    return false;
  }
  return true;
}

bool read_campaign_file(const std::filesystem::path& path, core::sim::CampaignCarry& out,
                        std::string* error) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    fail(error, std::string());
    return false;
  }
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    fail(error, "cannot open " + path.string());
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::byte> bytes(size > 0 ? static_cast<std::size_t>(size) : 0);
  const bool ok = bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
  std::fclose(file);
  if (!ok) {
    fail(error, "short read from " + path.string());
    return false;
  }
  const core::Result<core::sim::CampaignCarry> decoded = core::sim::decode_campaign_carry(bytes);
  if (!decoded.ok()) {
    fail(error, path.string() + " is not a campaign file this engine wrote");
    return false;
  }
  out = *decoded;
  return true;
}

namespace {

std::int32_t number_of(std::string_view directory) {
  const std::size_t slash = directory.find_last_of("/\\");
  const std::string_view digits =
      slash == std::string_view::npos ? directory : directory.substr(slash + 1);
  if (digits.empty()) return -1;
  std::int32_t number = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return -1;
    number = number * 10 + (c - '0');
  }
  return number;
}

/// `resolve_maps` over every `Maps/<n>/map.xml` the container holds.
std::vector<core::sim::TerritoryMap> territory_maps(const MapContainer& container,
                                                    const core::sim::ConquestMap& conquest) {
  std::vector<std::pair<std::int32_t, std::string>> names;
  for (const std::string& directory : container.map_directories()) {
    const std::int32_t number = number_of(directory);
    if (number < 0) continue;
    const std::vector<std::byte> xml = container.read(directory + "/map.xml");
    const core::Result<core::MapGeometry> geometry = core::MapGeometry::parse(xml);
    if (!geometry.ok()) continue;
    names.emplace_back(number, geometry->name);
  }
  return core::sim::resolve_maps(conquest, names);
}

}  // namespace

std::int32_t territory_of_map(const MapContainer& container,
                              const core::sim::ConquestMap& conquest,
                              std::string_view map_directory) {
  const std::int32_t played = number_of(map_directory);
  if (played < 0) return -1;
  for (const core::sim::TerritoryMap& entry : territory_maps(container, conquest)) {
    if (entry.map_number == played) return entry.territory;
  }
  return -1;
}

std::int32_t map_number_of_territory(const MapContainer& container,
                                     const core::sim::ConquestMap& conquest,
                                     std::int32_t territory) {
  for (const core::sim::TerritoryMap& entry : territory_maps(container, conquest)) {
    if (entry.territory == territory) return entry.map_number;
  }
  return -1;
}

std::vector<std::int32_t> open_territories(const core::sim::ConquestMap& conquest,
                                           const core::sim::CampaignProgress& progress) {
  const auto& territories = conquest.territories();
  std::vector<std::int32_t> out;
  bool any_owned = false;
  for (std::size_t i = 0; i < territories.size() && i < progress.states.size(); ++i) {
    if (progress.states[i] == core::sim::TerritoryState::owned) any_owned = true;
  }
  for (std::size_t i = 0; i < territories.size(); ++i) {
    if (i < progress.states.size() && progress.states[i] == core::sim::TerritoryState::owned) {
      continue;
    }
    if (!any_owned) {
      out.push_back(static_cast<std::int32_t>(i));
      continue;
    }
    for (const std::string& neighbour : territories[i].neighbours) {
      const std::int32_t index = conquest.find(neighbour);
      if (index >= 0 && static_cast<std::size_t>(index) < progress.states.size() &&
          progress.states[index] == core::sim::TerritoryState::owned) {
        out.push_back(static_cast<std::int32_t>(i));
        break;
      }
    }
  }
  return out;
}

}  // namespace imperivm::gamedata
