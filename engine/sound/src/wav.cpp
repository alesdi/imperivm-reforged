#include "imperivm/sound/wav.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace imperivm::sound {
namespace {

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::int16_t s16(std::span<const std::uint8_t> b, std::size_t at) {
  return static_cast<std::int16_t>(u16(b, at));
}

bool fail(std::string* error, const char* why) {
  if (error != nullptr) *error = why;
  return false;
}

std::int16_t clamp16(std::int32_t v) {
  return static_cast<std::int16_t>(std::clamp<std::int32_t>(v, -32768, 32767));
}

/// Microsoft ADPCM's step adaptation, indexed by the unsigned nibble. Part of
/// the published format, as are the seven coefficient pairs a file declares.
constexpr std::array<std::int32_t, 16> kAdaptation = {230, 230, 230, 230, 307, 409, 512, 614,
                                                       768, 614, 512, 409, 307, 230, 230, 230};

struct AdpcmChannel {
  std::int32_t coef1 = 0;
  std::int32_t coef2 = 0;
  std::int32_t delta = 0;
  std::int32_t sample1 = 0;
  std::int32_t sample2 = 0;

  std::int16_t expand(std::uint8_t nibble) {
    const std::int32_t signed_nibble = nibble >= 8 ? nibble - 16 : nibble;
    const std::int32_t predicted = (sample1 * coef1 + sample2 * coef2) / 256;
    const std::int16_t out = clamp16(predicted + signed_nibble * delta);
    sample2 = sample1;
    sample1 = out;
    delta = std::max<std::int32_t>(16, (kAdaptation[nibble] * delta) / 256);
    return out;
  }
};

bool decode_adpcm(std::span<const std::uint8_t> fmt, std::span<const std::uint8_t> data,
                  std::uint32_t fact, bool has_fact, Clip* out, std::string* error) {
  const std::uint16_t channels = out->channels;
  const std::uint16_t block_align = u16(fmt, 12);
  if (fmt.size() < 22) return fail(error, "ADPCM fmt chunk too short");
  const std::uint16_t coef_count = u16(fmt, 20);
  if (coef_count == 0 || fmt.size() < 22u + 4u * coef_count) return fail(error, "ADPCM coefficients missing");
  const std::size_t header = 7u * channels;
  if (block_align <= header) return fail(error, "ADPCM block too small");
  std::vector<std::int32_t> coefs(2u * coef_count);
  for (std::size_t i = 0; i < coefs.size(); ++i) coefs[i] = s16(fmt, 22 + 2 * i);

  out->samples.clear();
  for (std::size_t at = 0; at + header <= data.size(); at += block_align) {
    const std::span<const std::uint8_t> block = data.subspan(at, std::min<std::size_t>(block_align, data.size() - at));
    std::array<AdpcmChannel, 2> state{};
    for (std::uint16_t c = 0; c < channels; ++c) {
      const std::uint8_t predictor = block[c];
      if (predictor >= coef_count) return fail(error, "ADPCM predictor out of range");
      state[c].coef1 = coefs[2u * predictor];
      state[c].coef2 = coefs[2u * predictor + 1];
      state[c].delta = s16(block, channels + 2u * c);
      state[c].sample1 = s16(block, 3u * channels + 2u * c);
      state[c].sample2 = s16(block, 5u * channels + 2u * c);
    }
    // The header's two samples come out oldest first.
    for (std::uint16_t c = 0; c < channels; ++c) out->samples.push_back(static_cast<std::int16_t>(state[c].sample2));
    for (std::uint16_t c = 0; c < channels; ++c) out->samples.push_back(static_cast<std::int16_t>(state[c].sample1));
    // Then a nibble a sample, high nibble first; with two channels they
    // alternate left, right.
    std::uint16_t c = 0;
    for (std::size_t i = header; i < block.size(); ++i) {
      for (const std::uint8_t nibble : {static_cast<std::uint8_t>(block[i] >> 4), static_cast<std::uint8_t>(block[i] & 0x0F)}) {
        out->samples.push_back(state[c].expand(nibble));
        c = static_cast<std::uint16_t>((c + 1) % channels);
      }
    }
  }
  if (has_fact && static_cast<std::size_t>(fact) * channels < out->samples.size()) {
    out->samples.resize(static_cast<std::size_t>(fact) * channels);
  }
  return true;
}

}  // namespace

bool decode_wav(std::span<const std::uint8_t> bytes, Clip* out, std::string* error) {
  if (out == nullptr) return fail(error, "nowhere to decode into");
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    return fail(error, "not a RIFF WAVE file");
  }
  std::span<const std::uint8_t> fmt;
  std::span<const std::uint8_t> data;
  std::uint32_t fact = 0;
  bool has_fact = false;
  bool has_data = false;
  for (std::size_t at = 12; at + 8 <= bytes.size();) {
    const std::uint32_t size = u32(bytes, at + 4);
    const std::size_t body = at + 8;
    // A chunk that claims more than the file holds is cut at the end, which
    // is what players do with a `data` size the encoder never patched.
    const std::size_t length = std::min<std::size_t>(size, bytes.size() - body);
    const std::span<const std::uint8_t> chunk = bytes.subspan(body, length);
    if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0) fmt = chunk;
    if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
      data = chunk;
      has_data = true;
    }
    if (std::memcmp(bytes.data() + at, "fact", 4) == 0 && length >= 4) {
      fact = u32(chunk, 0);
      has_fact = true;
    }
    at = body + length + (length & 1u);
  }
  if (fmt.size() < 16) return fail(error, "no fmt chunk");
  if (!has_data) return fail(error, "no data chunk");
  const std::uint16_t tag = u16(fmt, 0);
  const std::uint16_t channels = u16(fmt, 2);
  const std::uint32_t rate = u32(fmt, 4);
  const std::uint16_t bits = u16(fmt, 14);
  if (channels != 1 && channels != 2) return fail(error, "only mono and stereo are played");
  if (rate == 0) return fail(error, "a rate of zero");
  out->channels = channels;
  out->rate = rate;
  out->samples.clear();

  if (tag == kWavPcm) {
    if (bits == 16) {
      out->samples.resize(data.size() / 2 / channels * channels);
      for (std::size_t i = 0; i < out->samples.size(); ++i) out->samples[i] = s16(data, 2 * i);
      return true;
    }
    if (bits == 8) {
      out->samples.resize(data.size() / channels * channels);
      for (std::size_t i = 0; i < out->samples.size(); ++i) {
        out->samples[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(data[i]) - 128) * 256);
      }
      return true;
    }
    return fail(error, "PCM of neither 8 nor 16 bits");
  }
  if (tag == kWavMsAdpcm) {
    if (bits != 4) return fail(error, "ADPCM of other than 4 bits");
    return decode_adpcm(fmt, data, fact, has_fact, out, error);
  }
  return fail(error, "a WAV format this engine does not decode");
}

}  // namespace imperivm::sound
