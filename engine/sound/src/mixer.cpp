#include "imperivm/sound/mixer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace imperivm::sound {
namespace {

constexpr std::int32_t kFloorMillibels = -10000;

/// Every millibel from -10,000 to 0 as Q15, built once. Floating point is
/// fine here: this is presentation, outside core, and a table computed the
/// same way everywhere is the same table.
const std::array<std::int32_t, 10001>& gain_table() {
  static const std::array<std::int32_t, 10001> table = [] {
    std::array<std::int32_t, 10001> t{};
    for (std::size_t i = 0; i < t.size(); ++i) {
      const double millibels = -static_cast<double>(i);
      t[i] = static_cast<std::int32_t>(std::lround(32768.0 * std::pow(10.0, millibels / 2000.0)));
    }
    t[t.size() - 1] = 0;  // DirectSound's minimum is silence
    return t;
  }();
  return table;
}

/// The least a stream is asked for at once, in frames: about a twentieth of
/// a second at 44,100 Hz, so a callback of a few hundred frames does not
/// call the decoder every time.
constexpr std::size_t kStreamBlock = 2048;

}  // namespace

Mixer::Mixer(std::uint32_t output_rate, std::vector<PoolSpec> pools) : rate_(output_rate == 0 ? 44100 : output_rate) {
  for (const PoolSpec& spec : pools) {
    pools_.push_back(Pool{voices_.size(), spec.voices, spec.steal});
    voices_.resize(voices_.size() + spec.voices);
  }
}

std::int32_t Mixer::gain(std::int32_t millibels) noexcept {
  if (millibels >= 0) return 32768;
  if (millibels <= kFloorMillibels) return 0;
  return gain_table()[static_cast<std::size_t>(-millibels)];
}

Mixer::Gains Mixer::gains(std::int32_t volume, std::int32_t pan) noexcept {
  const std::int32_t p = std::clamp(pan, -10000, 10000);
  const std::int32_t v = std::min(volume, 0);
  Gains g;
  // A pan to the right attenuates the left, and the other way about.
  g.left = gain(v - std::max(p, 0));
  g.right = gain(v + std::min(p, 0));
  return g;
}

bool Mixer::full(std::size_t pool) const noexcept {
  if (pool >= pools_.size()) return true;
  const Pool& p = pools_[pool];
  if (p.count == 0) return true;
  if (p.steal) return false;
  return playing(pool) == p.count;
}

int Mixer::take(std::size_t pool_index) const noexcept {
  if (pool_index >= pools_.size()) return -1;
  const Pool& pool = pools_[pool_index];
  if (pool.count == 0) return -1;
  for (std::size_t i = pool.first; i < pool.first + pool.count; ++i) {
    if (!voices_[i].active()) return static_cast<int>(i);
  }
  return pool.steal ? static_cast<int>(pool.first + pool.count - 1) : -1;
}

void Mixer::start(Voice& voice, std::uint32_t source_rate, const PlayParams& params) const noexcept {
  voice.gains = gains(params.volume, params.pan);
  voice.position = 0;
  voice.step = static_cast<std::uint32_t>((static_cast<std::uint64_t>(source_rate) << 16) / rate_);
  if (voice.step == 0) voice.step = 1;
}

int Mixer::play(std::shared_ptr<const Clip> clip, const PlayParams& params) {
  if (clip == nullptr || clip->frames() == 0 || clip->rate == 0) return -1;
  const int chosen = take(params.pool);
  if (chosen < 0) return -1;
  Voice& voice = voices_[static_cast<std::size_t>(chosen)];
  voice.reset();
  voice.clip = std::move(clip);
  start(voice, voice.clip->rate, params);
  return chosen;
}

int Mixer::play(std::unique_ptr<Stream> stream, const PlayParams& params) {
  if (stream == nullptr || stream->rate() == 0 || stream->channels() < 1 || stream->channels() > 2) return -1;
  const int chosen = take(params.pool);
  if (chosen < 0) return -1;
  Voice& voice = voices_[static_cast<std::size_t>(chosen)];
  voice.reset();
  voice.stream = std::move(stream);
  start(voice, voice.stream->rate(), params);
  return chosen;
}

bool Mixer::set_voice(int voice, std::int32_t volume, std::int32_t pan) {
  if (!busy(voice)) return false;
  voices_[static_cast<std::size_t>(voice)].gains = gains(volume, pan);
  return true;
}

void Mixer::stop(int voice) {
  if (voice < 0 || static_cast<std::size_t>(voice) >= voices_.size()) return;
  voices_[static_cast<std::size_t>(voice)].reset();
}

void Mixer::stop_pool(std::size_t pool) {
  if (pool >= pools_.size()) return;
  for (std::size_t i = pools_[pool].first; i < pools_[pool].first + pools_[pool].count; ++i) voices_[i].reset();
}

void Mixer::stop_all() {
  for (Voice& voice : voices_) voice.reset();
}

std::size_t Mixer::playing() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.active(); }));
}

std::size_t Mixer::playing(std::size_t pool) const noexcept {
  if (pool >= pools_.size()) return 0;
  std::size_t n = 0;
  for (std::size_t i = pools_[pool].first; i < pools_[pool].first + pools_[pool].count; ++i) {
    if (voices_[i].active()) ++n;
  }
  return n;
}

bool Mixer::busy(int voice) const noexcept {
  return voice >= 0 && static_cast<std::size_t>(voice) < voices_.size() &&
         voices_[static_cast<std::size_t>(voice)].active();
}

const Clip* Mixer::clip(int voice) const noexcept {
  return busy(voice) ? voices_[static_cast<std::size_t>(voice)].clip.get() : nullptr;
}

Mixer::Gains Mixer::voice_gains(int voice) const noexcept {
  return busy(voice) ? voices_[static_cast<std::size_t>(voice)].gains : Gains{};
}

void Mixer::refill(Voice& voice, std::size_t frames) {
  const std::size_t channels = voice.stream->channels();
  // Drop what has been played; the position then counts from what is left.
  const std::size_t played = std::min(static_cast<std::size_t>(voice.position >> 16), voice.buffered);
  if (played > 0) {
    std::memmove(voice.buffer.data(), voice.buffer.data() + played * channels,
                 (voice.buffered - played) * channels * sizeof(std::int16_t));
    voice.buffered -= played;
    voice.position -= static_cast<std::uint64_t>(played) << 16;
  }
  // The last frame read, and the one after it that interpolation reads.
  const std::size_t needed =
      static_cast<std::size_t>((voice.position + static_cast<std::uint64_t>(voice.step) * frames) >> 16) + 2;
  while (!voice.drained && voice.buffered < needed) {
    const std::size_t want = std::max(needed - voice.buffered, kStreamBlock);
    if (voice.buffer.size() < (voice.buffered + want) * channels) voice.buffer.resize((voice.buffered + want) * channels);
    const std::size_t got = voice.stream->read(voice.buffer.data() + voice.buffered * channels, want);
    voice.buffered += std::min(got, want);
    if (got == 0) voice.drained = true;
  }
}

void Mixer::mix(std::int16_t* out, std::size_t frames) {
  scratch_.assign(frames * 2, 0);
  for (Voice& voice : voices_) {
    if (!voice.active()) continue;
    const std::int16_t* samples = nullptr;
    std::size_t length = 0;
    std::uint16_t channels = 1;
    if (voice.stream != nullptr) {
      refill(voice, frames);
      samples = voice.buffer.data();
      length = voice.buffered;
      channels = voice.stream->channels();
    } else {
      samples = voice.clip->samples.data();
      length = voice.clip->frames();
      channels = voice.clip->channels;
    }
    const std::int64_t left_gain = voice.gains.left;
    const std::int64_t right_gain = voice.gains.right;
    for (std::size_t f = 0; f < frames; ++f) {
      const std::size_t index = static_cast<std::size_t>(voice.position >> 16);
      if (index >= length) break;
      const std::int32_t frac = static_cast<std::int32_t>(voice.position & 0xFFFF);
      // At the very end the last frame is held; a stream not yet over always
      // has the next one decoded (`refill`).
      const std::size_t next = index + 1 < length ? index + 1 : index;
      std::int32_t l = 0;
      std::int32_t r = 0;
      if (channels == 2) {
        const std::int32_t l0 = samples[2 * index], l1 = samples[2 * next];
        const std::int32_t r0 = samples[2 * index + 1], r1 = samples[2 * next + 1];
        l = l0 + static_cast<std::int32_t>((static_cast<std::int64_t>(l1 - l0) * frac) >> 16);
        r = r0 + static_cast<std::int32_t>((static_cast<std::int64_t>(r1 - r0) * frac) >> 16);
      } else {
        const std::int32_t s0 = samples[index], s1 = samples[next];
        l = r = s0 + static_cast<std::int32_t>((static_cast<std::int64_t>(s1 - s0) * frac) >> 16);
      }
      scratch_[2 * f] += static_cast<std::int32_t>((l * left_gain) >> 15);
      scratch_[2 * f + 1] += static_cast<std::int32_t>((r * right_gain) >> 15);
      voice.position += voice.step;
    }
    const bool over = (voice.position >> 16) >= length;
    if (over && (voice.stream == nullptr || voice.drained)) voice.reset();
  }
  for (std::size_t i = 0; i < frames * 2; ++i) {
    out[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(scratch_[i], -32768, 32767));
  }
}

}  // namespace imperivm::sound
