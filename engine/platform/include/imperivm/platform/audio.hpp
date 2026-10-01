#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/platform/bytes.hpp"
#include "imperivm/sound/mixer.hpp"
#include "imperivm/sound/placement.hpp"

struct SDL_AudioStream;

namespace imperivm::platform {

/// The sound device: one SDL playback stream fed by `sound::Mixer`.
///
/// The mixer decides what is heard -- pools, volume, pan, dropping -- and
/// this class only opens the device and hands it the mixer's output, 16-bit
/// stereo at 44,100 Hz, from SDL's audio callback. Calls from the game lock
/// the stream, which is the lock SDL holds around the callback, so the mixer
/// is only ever touched by one thread at a time.
///
/// A sound is a whole RIFF WAV from the player's installation, decoded once by
/// `sound::decode_wav` and kept under the name it was played by; or an Ogg
/// Vorbis file, the music, decoded as it plays (`sound::OggStream`) and never
/// kept.
///
/// Each voice remembers what it plays and where from, so that the game can
/// move it with the view (`live`, `set_voice`), as the original's sound
/// manager does (0x006b0810).
///
/// Opening the device is lazy and failing it is not an error: a machine with
/// no audio device -- a test runner, a server -- still plays the game,
/// silently, and `play` says why in `error`. Headless runs open SDL's dummy
/// driver (`prepare_headless`), which consumes the mix at the real rate and
/// plays nothing.
class Audio {
 public:
  static constexpr int kRate = 44100;

  Audio();
  ~Audio();
  Audio(const Audio&) = delete;
  Audio& operator=(const Audio&) = delete;

  /// The mixer's pools, one per sound type (docs/engine/sound.md). Stops
  /// whatever is playing.
  void configure(std::vector<sound::PoolSpec> pools);

  /// Whether a sound on `pool` would be dropped now.
  [[nodiscard]] bool full(std::size_t pool);
  /// How many of a pool's voices are playing.
  [[nodiscard]] std::size_t playing(std::size_t pool);

  /// Play `wav`, known as `name`, with `params`, from `placed`. False, with
  /// the reason, when the bytes are not a WAV the decoder reads, there is no
  /// device, or the pool is full and does not steal. `wav` may be empty when
  /// `knows(name)`.
  bool play(std::string_view name, ByteSpan wav, const sound::PlayParams& params, std::string* error = nullptr,
            const sound::Placed& placed = {});

  /// Play `ogg`, a whole Ogg Vorbis file known as `name`, decoding it as it
  /// plays. Never positioned. False, with the reason, as `play`.
  bool play_stream(std::string_view name, std::vector<std::uint8_t> ogg, const sound::PlayParams& params,
                   std::string* error = nullptr);

  /// A voice playing now: what it plays, where from, and as it stands.
  struct Live {
    int voice = -1;
    std::size_t pool = 0;
    std::string name;
    sound::Placed placed;
    std::int32_t volume = 0;
    std::int32_t pan = 0;
  };
  /// Every voice playing now, in voice order.
  [[nodiscard]] std::vector<Live> live();
  /// A live voice's volume and pan. False when it is no longer playing.
  bool set_voice(int voice, std::int32_t volume, std::int32_t pan);

  /// Stop every sound of a pool: a type switched off.
  void stop_pool(std::size_t pool);
  /// Stop everything (0x006aff00).
  void stop_all();

  /// Whether `name` has been decoded before: the bytes need not be read again.
  [[nodiscard]] bool knows(std::string_view name) const;

 private:
  bool open(std::string* error);
  static void feed(void* self, SDL_AudioStream* stream, int additional, int total);
  void lock();
  void unlock();
  void remember(int voice, std::size_t pool, std::string_view name, const sound::Placed& placed,
                const sound::PlayParams& params);

  bool tried_ = false;
  bool open_ = false;
  SDL_AudioStream* stream_ = nullptr;
  sound::Mixer mixer_;
  std::vector<std::int16_t> buffer_;
  /// Decoded clips by name; a name whose bytes did not decode keeps null.
  std::map<std::string, std::shared_ptr<const sound::Clip>, std::less<>> clips_;
  /// What each voice was last started with, by voice.
  std::vector<Live> voices_;
};

}  // namespace imperivm::platform
