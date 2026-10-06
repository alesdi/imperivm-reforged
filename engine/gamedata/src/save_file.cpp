#include "imperivm/gamedata/save_file.hpp"

#include <cstdio>
#include <system_error>
#include <vector>

namespace imperivm::gamedata {
namespace {

void fail(std::string* error, std::string text) {
  if (error != nullptr) *error = std::move(text);
}

}  // namespace

bool write_save_file(const std::filesystem::path& path,
                     const core::sim::SaveFileContents& contents, std::string* error) {
  const core::Result<std::vector<std::byte>> encoded = core::sim::encode_save_file(contents);
  if (!encoded.ok()) {
    fail(error, "cannot write " + path.string() + ": the session does not fit in a container");
    return false;
  }
  const std::vector<std::byte>& bytes = encoded.value();
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

bool read_save_file(const std::filesystem::path& path, core::sim::SaveFileContents& out,
                    std::string* error) {
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
  const core::Result<core::sim::SaveFileContents> decoded = core::sim::decode_save_file(bytes);
  if (!decoded.ok()) {
    const char* why = "is not a save this engine wrote";
    switch (decoded.error()) {
      case core::FormatError::bad_magic: why = "is not a container"; break;
      case core::FormatError::truncated: why = "is truncated"; break;
      default: break;
    }
    fail(error, path.string() + " " + why);
    return false;
  }
  out = *decoded;
  return true;
}

std::string container_relative(const std::filesystem::path& game_dir,
                               const std::filesystem::path& container) {
  std::error_code ec;
  const std::filesystem::path game = std::filesystem::weakly_canonical(game_dir, ec);
  const std::filesystem::path full = std::filesystem::weakly_canonical(container, ec);
  std::filesystem::path relative = full.lexically_relative(game);
  // Outside the installation -- or not expressible under it -- is spelled
  // absolute rather than as a `../` chain that would move with the install.
  if (relative.empty() || *relative.begin() == "..") relative = full;
  return relative.generic_string();
}

std::filesystem::path container_absolute(const std::filesystem::path& game_dir,
                                         std::string_view relative) {
  const std::filesystem::path spelled{std::string(relative)};
  return spelled.is_absolute() ? spelled : game_dir / spelled;
}

std::string map_number(std::string_view map_directory) {
  std::string_view rest = map_directory;
  // The stored spelling, which the shipped containers write with either
  // separator.
  const std::size_t slash = rest.find_last_of("/\\");
  if (slash != std::string_view::npos) rest = rest.substr(slash + 1);
  for (const char c : rest) {
    if (c < '0' || c > '9') return std::string();
  }
  return std::string(rest);
}

std::string map_identity(const core::sim::SaveFileManifest& manifest) {
  if (manifest.map_index.empty()) return manifest.container;
  return manifest.container + "/Maps/" + manifest.map_index;
}

}  // namespace imperivm::gamedata
