#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace imperivm::sound {

/// A decoded sound: signed 16-bit samples, interleaved when there are two
/// channels, at the rate the file declares. The mixer converts the rate as it
/// plays, so a clip is kept exactly as the file had it.
struct Clip {
  std::vector<std::int16_t> samples;
  std::uint16_t channels = 1;  ///< 1 or 2
  std::uint32_t rate = 44100;  ///< frames a second

  [[nodiscard]] std::size_t frames() const noexcept {
    return channels == 0 ? 0 : samples.size() / channels;
  }
};

/// The RIFF `fmt ` format tags the installation uses. `Sounds.pak`'s 314
/// files are all PCM (16-bit, mono or stereo, 22,050 / 44,100 / 48,000 Hz);
/// the language pack's 388 voices are all Microsoft ADPCM (4-bit, mono,
/// 44,100 Hz, 1,024-byte blocks of 2,036 samples, the seven standard
/// coefficient pairs). See docs/engine/sound.md.
inline constexpr std::uint16_t kWavPcm = 1;
inline constexpr std::uint16_t kWavMsAdpcm = 2;

/// Decode a whole RIFF WAV: PCM of 8 or 16 bits, or Microsoft ADPCM, one or
/// two channels. False, with the reason, for anything else -- a format tag the
/// installation does not use is refused rather than half-read.
///
/// For ADPCM, what the `data` chunk holds is decoded, a short last block
/// included, and the `fact` chunk's sample count only ever trims it. It never
/// does in the shipped voices: 111 count exactly what the data holds, 116 one
/// sample more, and the 161 whose data ends on a block boundary count a last
/// block that is not there (531 to 2,031 samples more).
bool decode_wav(std::span<const std::uint8_t> bytes, Clip* out, std::string* error = nullptr);

}  // namespace imperivm::sound
