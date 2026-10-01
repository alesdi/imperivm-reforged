#include "imperivm/sound/ogg.hpp"

#include <algorithm>
#include <climits>

// The declarations only; the decoder itself is the `stb_vorbis` library,
// built with its own warnings off (engine/third_party/stb_vorbis).
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"  // NOLINT(bugprone-suspicious-include)

namespace imperivm::sound {

std::unique_ptr<OggStream> OggStream::open(std::vector<std::uint8_t> bytes, std::string* error) {
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX)) {
    if (error != nullptr) *error = "no Ogg Vorbis data";
    return nullptr;
  }
  std::unique_ptr<OggStream> stream(new OggStream());
  stream->bytes_ = std::move(bytes);
  int code = 0;
  stream->vorbis_ = stb_vorbis_open_memory(stream->bytes_.data(), static_cast<int>(stream->bytes_.size()),
                                           &code, nullptr);
  if (stream->vorbis_ == nullptr) {
    if (error != nullptr) *error = "not an Ogg Vorbis stream this decoder reads (stb_vorbis error " + std::to_string(code) + ")";
    return nullptr;
  }
  const stb_vorbis_info info = stb_vorbis_get_info(stream->vorbis_);
  if (info.sample_rate == 0 || info.channels < 1) {
    if (error != nullptr) *error = "an Ogg Vorbis stream with no rate or no channels";
    return nullptr;
  }
  stream->rate_ = info.sample_rate;
  stream->channels_ = static_cast<std::uint16_t>(std::min(info.channels, 2));
  stream->length_ = stb_vorbis_stream_length_in_samples(stream->vorbis_);
  return stream;
}

OggStream::~OggStream() {
  if (vorbis_ != nullptr) stb_vorbis_close(vorbis_);
}

std::size_t OggStream::read(std::int16_t* out, std::size_t frames) {
  if (vorbis_ == nullptr || frames == 0) return 0;
  std::size_t done = 0;
  // The decoder takes an int count of samples; ask in pieces it can count.
  constexpr std::size_t kMost = 1u << 20;
  while (done < frames) {
    const std::size_t ask = std::min(frames - done, kMost);
    const int got = stb_vorbis_get_samples_short_interleaved(
        vorbis_, channels_, reinterpret_cast<short*>(out + done * channels_), static_cast<int>(ask * channels_));
    if (got <= 0) break;
    done += static_cast<std::size_t>(got);
  }
  position_ += done;
  return done;
}

}  // namespace imperivm::sound
