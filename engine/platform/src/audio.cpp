#include "imperivm/platform/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

#include "imperivm/sound/ogg.hpp"
#include "imperivm/sound/wav.hpp"

namespace imperivm::platform {

Audio::Audio() : mixer_(kRate) {}

Audio::~Audio() {
  // Destroying the stream stops the callback before the mixer goes.
  if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
  if (open_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool Audio::open(std::string* error) {
  if (!tried_) {
    tried_ = true;
    open_ = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (open_) {
      SDL_AudioSpec spec{};
      spec.format = SDL_AUDIO_S16;
      spec.channels = 2;
      spec.freq = kRate;
      stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Audio::feed, this);
      if (stream_ == nullptr || !SDL_ResumeAudioStreamDevice(stream_)) {
        if (error != nullptr) *error = SDL_GetError();
        if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        open_ = false;
      }
    } else if (error != nullptr) {
      *error = SDL_GetError();
    }
  } else if (!open_ && error != nullptr) {
    *error = "no audio";
  }
  return open_;
}

void Audio::feed(void* self, SDL_AudioStream* stream, int additional, int /*total*/) {
  // SDL holds the stream's lock around this call.
  auto* audio = static_cast<Audio*>(self);
  const std::size_t frames = static_cast<std::size_t>(std::max(additional, 0)) / 4;
  if (frames == 0) return;
  audio->buffer_.resize(frames * 2);
  audio->mixer_.mix(audio->buffer_.data(), frames);
  SDL_PutAudioStreamData(stream, audio->buffer_.data(), static_cast<int>(frames * 4));
}

void Audio::lock() {
  if (stream_ != nullptr) SDL_LockAudioStream(stream_);
}

void Audio::unlock() {
  if (stream_ != nullptr) SDL_UnlockAudioStream(stream_);
}

void Audio::configure(std::vector<sound::PoolSpec> pools) {
  lock();
  mixer_ = sound::Mixer(kRate, std::move(pools));
  voices_.assign(mixer_.voices(), Live{});
  unlock();
}

bool Audio::full(std::size_t pool) {
  lock();
  const bool full = mixer_.full(pool);
  unlock();
  return full;
}

std::size_t Audio::playing(std::size_t pool) {
  lock();
  const std::size_t n = mixer_.playing(pool);
  unlock();
  return n;
}

void Audio::stop_pool(std::size_t pool) {
  lock();
  mixer_.stop_pool(pool);
  unlock();
}

void Audio::stop_all() {
  lock();
  mixer_.stop_all();
  unlock();
}

bool Audio::knows(std::string_view name) const { return clips_.find(name) != clips_.end(); }

void Audio::remember(int voice, std::size_t pool, std::string_view name, const sound::Placed& placed,
                     const sound::PlayParams& params) {
  if (voice < 0) return;
  if (voices_.size() < mixer_.voices()) voices_.resize(mixer_.voices());
  Live& live = voices_[static_cast<std::size_t>(voice)];
  live.voice = voice;
  live.pool = pool;
  live.name = std::string(name);
  live.placed = placed;
  live.volume = params.volume;
  live.pan = params.pan;
}

bool Audio::play_stream(std::string_view name, std::vector<std::uint8_t> ogg, const sound::PlayParams& params,
                        std::string* error) {
  if (ogg.empty()) {
    if (error != nullptr) *error = "no such sound";
    return false;
  }
  // The headers are read here, on the caller's thread; the audio packets in
  // the callback, a block at a time.
  std::unique_ptr<sound::OggStream> stream = sound::OggStream::open(std::move(ogg), error);
  if (stream == nullptr) return false;
  if (!open(error)) return false;
  lock();
  const int voice = mixer_.play(std::move(stream), params);
  remember(voice, params.pool, name, sound::Placed{}, params);
  unlock();
  if (voice < 0) {
    if (error != nullptr) *error = "its type's channels are all busy";
    return false;
  }
  return true;
}

std::vector<Audio::Live> Audio::live() {
  std::vector<Live> out;
  lock();
  for (std::size_t v = 0; v < voices_.size(); ++v) {
    if (mixer_.busy(static_cast<int>(v))) out.push_back(voices_[v]);
  }
  unlock();
  return out;
}

bool Audio::set_voice(int voice, std::int32_t volume, std::int32_t pan) {
  lock();
  const bool done = mixer_.set_voice(voice, volume, pan);
  if (done) {
    voices_[static_cast<std::size_t>(voice)].volume = volume;
    voices_[static_cast<std::size_t>(voice)].pan = pan;
  }
  unlock();
  return done;
}

bool Audio::play(std::string_view name, ByteSpan wav, const sound::PlayParams& params, std::string* error,
                 const sound::Placed& placed) {
  auto it = clips_.find(name);
  if (it == clips_.end()) {
    auto clip = std::make_shared<sound::Clip>();
    std::string why = "no such sound";
    if (wav.empty() || !sound::decode_wav(wav, clip.get(), &why)) clip.reset();
    it = clips_.emplace(std::string(name), std::move(clip)).first;
    if (it->second == nullptr) {
      if (error != nullptr) *error = why;
      return false;
    }
  }
  if (it->second == nullptr) {
    if (error != nullptr) *error = "not a sound this engine decodes";
    return false;
  }
  if (!open(error)) return false;
  lock();
  const int voice = mixer_.play(it->second, params);
  remember(voice, params.pool, name, placed, params);
  unlock();
  if (voice < 0) {
    if (error != nullptr) *error = "its type's channels are all busy";
    return false;
  }
  return true;
}

}  // namespace imperivm::platform
