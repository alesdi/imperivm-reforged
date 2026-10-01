#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "imperivm/sound/mixer.hpp"

struct stb_vorbis;

namespace imperivm::sound {

/// An Ogg Vorbis file decoded as it is played: the installation's
/// `music/*.ogg`, stereo 44,100 Hz at 128 kbit/s, 87 to 329 seconds each.
///
/// The compressed file is held whole -- 1.4 to 5.3 MB -- and the decoder,
/// the vendored `stb_vorbis` (`engine/third_party/stb_vorbis`), turns it into
/// samples a packet at a time as `read` asks, so at most a few thousand
/// decoded frames exist at once rather than a track's 58 MB of PCM.
///
/// The original links an Ogg Vorbis decoder statically too (an `OggS`
/// capture at 0x00753202, a `vorbis` header check at 0x00753a77); which one
/// is not known, and nothing here depends on it being the same.
class OggStream final : public Stream {
 public:
  /// The stream over `bytes`, a whole Ogg Vorbis file, or null with the
  /// reason. More than two channels are mixed down to two by the decoder.
  static std::unique_ptr<OggStream> open(std::vector<std::uint8_t> bytes, std::string* error = nullptr);

  ~OggStream() override;
  OggStream(const OggStream&) = delete;
  OggStream& operator=(const OggStream&) = delete;

  [[nodiscard]] std::uint32_t rate() const noexcept override { return rate_; }
  [[nodiscard]] std::uint16_t channels() const noexcept override { return channels_; }
  std::size_t read(std::int16_t* out, std::size_t frames) override;

  /// The stream's length in frames, from its last page's granule position;
  /// 0 when the file does not say.
  [[nodiscard]] std::uint64_t length() const noexcept { return length_; }
  /// Frames `read` has handed out so far.
  [[nodiscard]] std::uint64_t position() const noexcept { return position_; }

 private:
  OggStream() = default;

  std::vector<std::uint8_t> bytes_;
  ::stb_vorbis* vorbis_ = nullptr;
  std::uint32_t rate_ = 0;
  std::uint16_t channels_ = 0;
  std::uint64_t length_ = 0;
  std::uint64_t position_ = 0;
};

}  // namespace imperivm::sound
