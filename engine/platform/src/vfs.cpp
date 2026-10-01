#include "imperivm/platform/vfs.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <SDL3/SDL.h>

namespace imperivm::platform {
namespace {

/// The packs at the install root. `local/italian.pak` is deliberately absent:
/// language selection is a decision the game makes, not the mounter.
constexpr const char* kRootPacks[] = {
    "AdditionalArt.pak", "Buildings.pak", "Fonts.pak",  "MapObjects.pak",
    "Minimap.pak",       "Outlines.pak",  "Sounds.pak", "Terrain.pak",
    "UI.pak",            "Units.pak",     "Visuals.pak", "data.pak",
};

class PackProvider final : public Vfs::Provider {
 public:
  explicit PackProvider(std::unique_ptr<Pack> pack) : pack_(std::move(pack)) {}

  [[nodiscard]] ByteSpan read(std::string_view name) const override {
    return pack_->read(name);
  }
  [[nodiscard]] bool contains(std::string_view name) const override {
    return pack_->contains(name);
  }
  [[nodiscard]] std::vector<std::string> list() const override {
    const auto& entries = pack_->directory().entries();
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries) names.emplace_back(entry.name);
    return names;
  }

 private:
  std::unique_ptr<Pack> pack_;
};

/// A real directory. Used for loose files beside the packs, and for the
/// working directory during development.
class DirectoryProvider final : public Vfs::Provider {
 public:
  explicit DirectoryProvider(std::filesystem::path dir) : dir_(std::move(dir)) {}

  [[nodiscard]] ByteSpan read(std::string_view name) const override {
    auto it = cache_.find(std::string(name));
    if (it != cache_.end()) return {it->second.data(), it->second.size()};

    const auto path = to_path(name);
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    auto [entry, _] = cache_.emplace(std::string(name), std::move(bytes));
    return {entry->second.data(), entry->second.size()};
  }

  [[nodiscard]] bool contains(std::string_view name) const override {
    const auto path = to_path(name);
    return !path.empty() && std::filesystem::is_regular_file(path);
  }

  [[nodiscard]] std::vector<std::string> list() const override { return {}; }

 private:
  /// Case-insensitive resolution, because the shipped data is uppercase with
  /// backslashes and the host filesystem may be neither.
  [[nodiscard]] std::filesystem::path to_path(std::string_view name) const {
    std::filesystem::path path = dir_;
    std::string component;
    auto descend = [&]() -> bool {
      if (component.empty() || component == "." || component == "..") return false;
      const auto direct = path / component;
      if (std::filesystem::exists(direct)) {
        path = direct;
        return true;
      }
      std::error_code ec;
      for (const auto& child : std::filesystem::directory_iterator(path, ec)) {
        std::string have = child.path().filename().string();
        if (have.size() != component.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < have.size() && same; ++i) {
          same = std::toupper(static_cast<unsigned char>(have[i])) ==
                 std::toupper(static_cast<unsigned char>(component[i]));
        }
        if (same) {
          path = child.path();
          return true;
        }
      }
      return false;
    };

    for (char c : name) {
      if (c == '\\' || c == '/') {
        if (!descend()) return {};
        component.clear();
      } else {
        component.push_back(c);
      }
    }
    if (!descend()) return {};
    return path;
  }

  std::filesystem::path dir_;
  mutable std::unordered_map<std::string, std::vector<std::uint8_t>> cache_;
};

}  // namespace

// -- MappedFile -----------------------------------------------------------

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    close();
    data_ = other.data_;
    size_ = other.size_;
    mapped_ = other.mapped_;
    fallback_ = std::move(other.fallback_);
    if (!mapped_) data_ = fallback_.data();
    other.data_ = nullptr;
    other.size_ = 0;
    other.mapped_ = false;
  }
  return *this;
}

bool MappedFile::open(const std::filesystem::path& path, std::string* error) {
  close();
  const auto fail = [&](std::string message) {
    if (error != nullptr) *error = path.string() + ": " + std::move(message);
    close();
    return false;
  };

#if defined(_WIN32)
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return fail("cannot open");
  LARGE_INTEGER length{};
  if (GetFileSizeEx(file, &length) == 0 || length.QuadPart == 0) {
    CloseHandle(file);
    return fail("cannot size");
  }
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  CloseHandle(file);
  if (mapping == nullptr) return fail("cannot map");
  void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(mapping);
  if (view == nullptr) return fail("cannot view");
  data_ = static_cast<const std::uint8_t*>(view);
  size_ = static_cast<std::size_t>(length.QuadPart);
  mapped_ = true;
  return true;
#elif !defined(__EMSCRIPTEN__)
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return fail("cannot open");
  struct stat info {};
  if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
    ::close(fd);
    return fail("cannot size");
  }
  void* view = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ,
                      MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (view == MAP_FAILED) return fail("cannot map");
  data_ = static_cast<const std::uint8_t*>(view);
  size_ = static_cast<std::size_t>(info.st_size);
  mapped_ = true;
  return true;
#else
  // Emscripten's MEMFS has no mapping worth the name; read it instead. The
  // callers see the same span either way, which is the point of the class.
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail("cannot open");
  fallback_.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (fallback_.empty()) return fail("empty");
  data_ = fallback_.data();
  size_ = fallback_.size();
  mapped_ = false;
  return true;
#endif
}

void MappedFile::close() {
  if (mapped_ && data_ != nullptr) {
#if defined(_WIN32)
    UnmapViewOfFile(const_cast<std::uint8_t*>(data_));
#elif !defined(__EMSCRIPTEN__)
    ::munmap(const_cast<std::uint8_t*>(data_), size_);
#endif
  }
  data_ = nullptr;
  size_ = 0;
  mapped_ = false;
  fallback_.clear();
  fallback_.shrink_to_fit();
}

ByteSpan MappedFile::slice(std::size_t offset, std::size_t length) const noexcept {
  if (data_ == nullptr || offset > size_ || length > size_ - offset) return {};
  return {data_ + offset, length};
}

// -- Pack -----------------------------------------------------------------

bool Pack::open(const std::filesystem::path& path, std::string* error) {
  path_ = path;
  if (!file_.open(path, error)) return false;

  auto parsed = core::PakDirectory::parse(as_core_bytes(file_.bytes()));
  if (!parsed) {
    if (error != nullptr) {
      *error = path.string() + ": not a readable HMMSYS pack (" +
               std::to_string(static_cast<int>(parsed.error())) + ")";
    }
    file_.close();
    return false;
  }
  directory_ = std::move(parsed.value());
  return true;
}

ByteSpan Pack::read(std::string_view name) const {
  const auto bytes = directory_.read(name);
  if (!bytes) return {};
  return as_platform_bytes(bytes.value());
}

bool Pack::contains(std::string_view name) const {
  return static_cast<bool>(directory_.find(name));
}

// -- Vfs ------------------------------------------------------------------

std::string Vfs::normalise(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    if (c == '/') c = '\\';
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  // A leading separator is noise; `\UNITS\...` and `UNITS\...` are one path.
  if (!out.empty() && out.front() == '\\') out.erase(out.begin());
  return out;
}

std::optional<std::filesystem::path> Vfs::find_installation(
    const std::filesystem::path& hint) {
  const auto looks_right = [](const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "rle.mmp", ec) &&
           std::filesystem::is_directory(dir / "Packs", ec);
  };

  std::vector<std::filesystem::path> candidates;
  if (!hint.empty()) candidates.push_back(hint);
  if (const char* env = SDL_getenv("IMPERIVM_GAME_DIR"); env != nullptr && *env != '\0') {
    candidates.emplace_back(env);
  }
  std::error_code ec;
  candidates.push_back(std::filesystem::current_path(ec));
  if (const char* base = SDL_GetBasePath(); base != nullptr) candidates.emplace_back(base);

  for (const auto& candidate : candidates) {
    if (candidate.empty()) continue;
    if (looks_right(candidate)) return candidate;
    // Tolerate being pointed at a subdirectory one level down.
    if (candidate.has_parent_path() && looks_right(candidate.parent_path())) {
      return candidate.parent_path();
    }
  }
  return std::nullopt;
}

bool Vfs::mount_installation(const std::filesystem::path& root, std::string* error) {
  root_ = root;

  if (!pixel_store_.open(root / "rle.mmp", error)) return false;

  bool any = false;
  for (const char* name : kRootPacks) {
    auto pack = std::make_unique<Pack>();
    std::string pack_error;
    if (!pack->open(root / "Packs" / name, &pack_error)) {
      // A partial install is usable; a missing rle.mmp is not. Say so and
      // carry on rather than refusing to start.
      SDL_Log("vfs: skipping %s (%s)", name, pack_error.c_str());
      continue;
    }
    mount_pack("", std::move(pack));
    any = true;
  }
  if (!any) {
    if (error != nullptr) *error = root.string() + ": no readable packs under Packs/";
    return false;
  }

  // Loose files at the install root, so a modder's unpacked override wins over
  // nothing but is at least reachable.
  mount_directory("", root);

  // `gameres/` is a virtual root aliasing `UI\`. The rest are the container
  // prefixes; they resolve once a .bfhp provider is mounted under them, and
  // resolve to nothing until then, which is the correct behaviour for a game
  // with no map loaded.
  alias("gameres\\", "UI\\");

  return true;
}

void Vfs::mount_provider(std::string_view prefix, std::unique_ptr<Provider> provider) {
  if (provider == nullptr) return;
  mounts_.push_back(Mount{normalise(prefix), std::move(provider)});
  // Longest prefix wins, so that `CurrentMap\` beats `CurrentGame\`'s parent
  // and the root mount is always the last resort.
  std::stable_sort(mounts_.begin(), mounts_.end(), [](const Mount& a, const Mount& b) {
    return a.prefix.size() > b.prefix.size();
  });
}

void Vfs::mount_pack(std::string_view prefix, std::unique_ptr<Pack> pack) {
  mount_provider(prefix, std::make_unique<PackProvider>(std::move(pack)));
}

void Vfs::mount_directory(std::string_view prefix, const std::filesystem::path& dir) {
  mount_provider(prefix, std::make_unique<DirectoryProvider>(dir));
}

void Vfs::alias(std::string_view prefix, std::string_view target) {
  aliases_.emplace_back(normalise(prefix), normalise(target));
}

std::string Vfs::resolve_aliases(std::string_view path) const {
  std::string current = normalise(path);
  for (int depth = 0; depth < 8; ++depth) {
    bool rewrote = false;
    for (const auto& [prefix, target] : aliases_) {
      if (current.size() >= prefix.size() && current.compare(0, prefix.size(), prefix) == 0) {
        current = target + current.substr(prefix.size());
        rewrote = true;
        break;
      }
    }
    if (!rewrote) break;
  }
  return current;
}

const Vfs::Mount* Vfs::find_mount(const std::string& normalised,
                                  std::string_view* remainder) const {
  for (const auto& mount : mounts_) {
    if (normalised.size() < mount.prefix.size()) continue;
    if (normalised.compare(0, mount.prefix.size(), mount.prefix) != 0) continue;
    std::string_view rest{normalised};
    rest.remove_prefix(mount.prefix.size());
    if (!mount.provider->contains(rest)) continue;
    if (remainder != nullptr) *remainder = rest;
    return &mount;
  }
  return nullptr;
}

std::vector<std::string> Vfs::list() const {
  std::vector<std::string> out;
  for (const Mount& mount : mounts_) {
    for (std::string& name : mount.provider->list()) {
      out.push_back(mount.prefix + name);
    }
  }
  return out;
}

bool Vfs::contains(std::string_view path) const {
  const std::string resolved = resolve_aliases(path);
  return find_mount(resolved, nullptr) != nullptr;
}

ByteSpan Vfs::read(std::string_view path) const {
  const std::string resolved = resolve_aliases(path);
  std::string_view remainder;
  const Mount* mount = find_mount(resolved, &remainder);
  if (mount == nullptr) return {};
  return mount->provider->read(remainder);
}

}  // namespace imperivm::platform
