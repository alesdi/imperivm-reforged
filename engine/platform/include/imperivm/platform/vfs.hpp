#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/pak.hpp"
#include "imperivm/platform/bytes.hpp"

namespace imperivm::platform {

/// A file mapped into the address space, unmapped when the object dies.
///
/// `rle.mmp` is 400 MB and the engine indexes it by absolute byte offset with
/// no directory of any kind, which is exactly the access pattern a mapping is
/// for: the pages a frame touches are the only pages that are ever read.
class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile();

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  /// Maps `path` read-only. Falls back to a plain read where the platform has
  /// no mapping (Emscripten), so callers see one behaviour everywhere.
  bool open(const std::filesystem::path& path, std::string* error = nullptr);
  void close();

  [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }
  [[nodiscard]] ByteSpan bytes() const noexcept { return {data_, size_}; }

  /// The `[offset, offset + length)` slice, or an empty span if it is out of
  /// range. Sprite payloads are addressed this way and nothing else validates
  /// them, so the bounds check lives here.
  [[nodiscard]] ByteSpan slice(std::size_t offset, std::size_t length) const noexcept;

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  bool mapped_ = false;                 // false => data_ points at fallback_
  std::vector<std::uint8_t> fallback_;
};

/// One HMMSYS `.pak`: the mapping, plus core's reader over it.
///
/// The split follows the architecture rule exactly. Opening the file and
/// keeping its pages alive is a platform job; decoding the front-coded entry
/// table is a pure bytes-to-state transformation and belongs to
/// `imperivm::core::PakDirectory`. This class is only the seam between them.
class Pack {
 public:
  bool open(const std::filesystem::path& path, std::string* error = nullptr);

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] const core::PakDirectory& directory() const noexcept { return directory_; }

  /// Case-insensitive, separator-insensitive lookup. Empty span if absent.
  [[nodiscard]] ByteSpan read(std::string_view name) const;
  [[nodiscard]] bool contains(std::string_view name) const;

 private:
  std::filesystem::path path_;
  MappedFile file_;
  core::PakDirectory directory_;
};

/// The engine's virtual filesystem.
///
/// The original addresses content through fixed virtual prefixes rather than
/// real paths -- `CurrentGame/`, `CurrentMap/`, `LocalGameData/`,
/// `AdvSaveGame/` -- and `gameres/` is a virtual root aliasing `UI\`. Those
/// names appear verbatim in `gbr.exe`, and no engine path ever names a
/// container file directly.
///
/// So this is a mount table with an alias table in front of it, not a set of
/// special cases bolted onto a path join. Resolution is:
///
///   1. rewrite the leading alias, repeatedly, up to a small depth;
///   2. find the mount whose prefix the path starts with, longest first;
///   3. ask that mount for the remainder.
///
/// A mount is currently a pack or a directory. `.bfhp` containers -- which is
/// what `CurrentGame/` and friends really resolve to -- are a third kind, and
/// the hook for them is `mount_provider`; the container reader itself belongs
/// in core and is not written yet.
class Vfs {
 public:
  /// Anything that can answer "give me the bytes stored at this name".
  class Provider {
   public:
    virtual ~Provider() = default;
    [[nodiscard]] virtual ByteSpan read(std::string_view name) const = 0;
    [[nodiscard]] virtual bool contains(std::string_view name) const = 0;
    [[nodiscard]] virtual std::vector<std::string> list() const = 0;
  };

  /// Locates a game installation.
  ///
  /// Tries, in order: `hint` if given, `$IMPERIVM_GAME_DIR`, the working
  /// directory, and the directory holding the executable. An installation is a
  /// directory containing `rle.mmp` and a `Packs` folder.
  static std::optional<std::filesystem::path> find_installation(
      const std::filesystem::path& hint = {});

  /// Mounts a real installation: every pack at the root prefix (their stored
  /// names are already fully qualified), plus the standard aliases.
  bool mount_installation(const std::filesystem::path& root, std::string* error = nullptr);

  void mount_provider(std::string_view prefix, std::unique_ptr<Provider> provider);
  void mount_pack(std::string_view prefix, std::unique_ptr<Pack> pack);
  void mount_directory(std::string_view prefix, const std::filesystem::path& dir);

  /// `alias("gameres/", "UI\\")` makes `gameres/CURSORS/X.BMP` resolve as
  /// `UI\CURSORS\X.BMP`.
  void alias(std::string_view prefix, std::string_view target);

  [[nodiscard]] bool contains(std::string_view path) const;

  /// Every name every mount holds, normalised. Packs enumerate; directories do
  /// not, because the install root is not something to walk. Used to find the
  /// 845 class definitions, which are addressed by directory and suffix rather
  /// than by an index the data ships.
  [[nodiscard]] std::vector<std::string> list() const;

  /// Bytes for `path`, borrowed from whichever mount holds it and valid for as
  /// long as the Vfs is. Empty span if the path does not resolve.
  [[nodiscard]] ByteSpan read(std::string_view path) const;

  /// The memory-mapped `rle.mmp` pixel store, or an empty span if no
  /// installation is mounted. Sprite frames name absolute offsets into it.
  [[nodiscard]] ByteSpan pixel_store() const noexcept { return pixel_store_.bytes(); }
  [[nodiscard]] ByteSpan pixels(std::size_t offset, std::size_t length) const noexcept {
    return pixel_store_.slice(offset, length);
  }

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  /// Normalises a virtual path: uppercase, `/` and `\` unified to `\`.
  static std::string normalise(std::string_view path);

 private:
  struct Mount {
    std::string prefix;  // normalised
    std::unique_ptr<Provider> provider;
  };

  [[nodiscard]] std::string resolve_aliases(std::string_view path) const;
  [[nodiscard]] const Mount* find_mount(const std::string& normalised,
                                        std::string_view* remainder) const;

  std::filesystem::path root_;
  MappedFile pixel_store_;
  std::vector<Mount> mounts_;
  std::vector<std::pair<std::string, std::string>> aliases_;
};

}  // namespace imperivm::platform
