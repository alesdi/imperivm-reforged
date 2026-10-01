#pragma once

// The campaign between missions, on disk, for the tools and the application.
//
// `core/sim/campaign.hpp` defines `CampaignCarry` -- what a finished mission
// leaves for the next one -- and its INI spelling. This puts that document at
// `<installation>/Saves/<conquest>.campaign.ini`, reads it back, and answers
// the one question the core cannot without the container: which territory
// the map being played belongs to.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/gamedata/map_source.hpp"

namespace imperivm::gamedata {

/// Where a conquest's carry lives: `<user_dir>/<container stem>.campaign.ini`,
/// the stem being the container's file name without its extension. The user
/// directory is the app's choice -- the installation's `Saves/` for a
/// person's run, a directory of its own for a test's -- and not this
/// function's, which is why it is given rather than derived from the
/// installation.
[[nodiscard]] std::filesystem::path campaign_file_path(const std::filesystem::path& user_dir,
                                                       std::string_view container_relative);

/// Write the carry. `error` gets a sentence on failure.
bool write_campaign_file(const std::filesystem::path& path, const core::sim::CampaignCarry& carry,
                         std::string* error = nullptr);

/// Read one. A file that does not exist is not an error -- there is no
/// campaign in progress -- and answers false with an empty `error`; a file
/// that exists and does not parse answers false with a sentence.
bool read_campaign_file(const std::filesystem::path& path, core::sim::CampaignCarry& out,
                        std::string* error = nullptr);

/// The territory whose `mapname` is the map in `map_directory` (`Maps/<n>`),
/// as an index into `conquest.territories()`, or -1. Reads each
/// `Maps/<n>/map.xml` the container holds, which is what `resolve_maps`
/// needs and the core cannot do for itself.
[[nodiscard]] std::int32_t territory_of_map(const MapContainer& container,
                                            const core::sim::ConquestMap& conquest,
                                            std::string_view map_directory);

/// The other direction: the `Maps/<n>` number of a territory's map, or -1.
[[nodiscard]] std::int32_t map_number_of_territory(const MapContainer& container,
                                                   const core::sim::ConquestMap& conquest,
                                                   std::int32_t territory);

/// Which territories a carry opens: every one not yet owned that neighbours
/// an owned one -- the choice the original's campaign map offers. Indices
/// into `conquest.territories()`, in table order. Every territory when none
/// is owned, because a conquest has to start somewhere and the shipped file
/// does not say where.
[[nodiscard]] std::vector<std::int32_t> open_territories(
    const core::sim::ConquestMap& conquest, const core::sim::CampaignProgress& progress);

}  // namespace imperivm::gamedata
