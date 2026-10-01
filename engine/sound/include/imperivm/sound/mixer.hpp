#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "imperivm/sound/wav.hpp"

namespace imperivm::sound {

/// A pool of voices one kind of sound plays on: the original's channels of a
/// sound type (0x006b0450).
struct PoolSpec {
  std::size_t voices = 1;
  /// When every voice is busy, whether a new sound takes the pool's last
  /// voice (the original does so for music, both ambiences and conversation
  /// speech) or is dropped (everything else).
  bool steal = false;
};

/// A sound decoded as it plays rather than all at once: the music, whose
/// seven tracks are 222 to 329 seconds each (`ogg.hpp`). The mixer pulls a
/// few thousand frames at a time from it and keeps only what it has not
/// played yet.
class Stream {
 public:
  virtual ~Stream() = default;
  [[nodiscard]] virtual std::uint32_t rate() const noexcept = 0;
  /// 1 or 2.
  [[nodiscard]] virtual std::uint16_t channels() const noexcept = 0;
  /// Up to `frames` frames into `out`, interleaved as `channels()` says.
  /// Fewer only at the end, and 0 once it is over.
  virtual std::size_t read(std::int16_t* out, std::size_t frames) = 0;
};

/// How one sound is to be played. Millibels, DirectSound's unit, which is
/// what the original computes (see `placement.hpp`).
struct PlayParams {
  /// 0 is the clip as recorded; less is quieter, -10,000 silent. Above 0
  /// plays at 0.
  std::int32_t volume = 0;
  /// -10,000 (left only) to 10,000 (right only). DirectSound's pan: the far
  /// channel is attenuated by the pan, the near one kept whole, so a centred
  /// sound plays at full volume in both.
  std::int32_t pan = 0;
  std::size_t pool = 0;
};

/// A software mixer: pools of voices, each voice a clip or a stream with its
/// own volume and pan, summed into interleaved signed 16-bit stereo at one
/// output rate.
///
/// **Integer mixing.** A voice's two gains are Q15, looked up from its
/// millibels when it starts or is moved; the sum is 32-bit and clipped to 16
/// bits, so a test can state the output exactly. Sound is presentation and
/// nothing here feeds the simulation.
///
/// **Rate conversion** is linear interpolation at a 16.16 step, per voice, so
/// the installation's 22,050 and 48,000 Hz files play at pitch on a 44,100 Hz
/// device. A clip at the output rate is copied sample for sample.
///
/// **Streams** are decoded inside `mix`, a block at a time, as far as the
/// frames being mixed need and no further.
///
/// Not thread-safe. `platform::Audio` calls it from the main thread and from
/// SDL's audio callback, under the audio stream's own lock.
class Mixer {
 public:
  explicit Mixer(std::uint32_t output_rate = 44100, std::vector<PoolSpec> pools = {PoolSpec{16, false}});

  /// Whether a new sound on `pool` would be dropped: every voice busy and
  /// no stealing. The original asks this before it draws a variant.
  [[nodiscard]] bool full(std::size_t pool) const noexcept;

  /// Start `clip` on `params.pool`. The voice it took, or -1 when dropped.
  ///
  /// The pool's first free voice; when there is none, its last voice if the
  /// pool steals, else nothing. Priority plays no part: the original stores
  /// one on every sound and its allocation never reads it.
  int play(std::shared_ptr<const Clip> clip, const PlayParams& params);
  /// The same for a stream, which the voice owns until it ends or stops.
  int play(std::unique_ptr<Stream> stream, const PlayParams& params);

  /// A live voice's volume and pan, changed while it plays: what the
  /// original's sound manager does to its live channels as the view moves
  /// (0x006b0810). False when the voice is not playing.
  bool set_voice(int voice, std::int32_t volume, std::int32_t pan);

  void stop(int voice);
  /// Stop every voice of a pool: what switching a type off does (0x006af960).
  void stop_pool(std::size_t pool);
  void stop_all();

  /// Mix the next `frames` frames into `out`, `2 * frames` samples, left
  /// then right. Voices that reach their sound's end are freed.
  void mix(std::int16_t* out, std::size_t frames);

  [[nodiscard]] std::uint32_t output_rate() const noexcept { return rate_; }
  [[nodiscard]] std::size_t voices() const noexcept { return voices_.size(); }
  [[nodiscard]] std::size_t pools() const noexcept { return pools_.size(); }
  /// Voices playing now, in all pools or in one.
  [[nodiscard]] std::size_t playing() const noexcept;
  [[nodiscard]] std::size_t playing(std::size_t pool) const noexcept;
  [[nodiscard]] bool busy(int voice) const noexcept;
  /// The clip a voice plays, or null (a stream's voice has none).
  [[nodiscard]] const Clip* clip(int voice) const noexcept;
  /// The voice's left and right gains as they stand.
  struct Gains {
    std::int32_t left = 0;
    std::int32_t right = 0;
  };
  [[nodiscard]] Gains voice_gains(int voice) const noexcept;

  /// Millibels as a Q15 amplitude: 32,768 at 0 and above, 0 at -10,000 and
  /// below, `32768 * 10^(mB / 2000)` rounded between.
  [[nodiscard]] static std::int32_t gain(std::int32_t millibels) noexcept;

  /// A voice's left and right Q15 gains for a volume and a pan.
  [[nodiscard]] static Gains gains(std::int32_t volume, std::int32_t pan) noexcept;

 private:
  struct Voice {
    std::shared_ptr<const Clip> clip;
    /// Or a stream, with what it has decoded and not yet played: `position`
    /// counts from `buffer`'s start, and played frames are dropped from it.
    std::unique_ptr<Stream> stream;
    std::vector<std::int16_t> buffer;
    std::size_t buffered = 0;  ///< frames in `buffer`
    bool drained = false;      ///< the stream has said it is over
    Gains gains;
    std::uint64_t position = 0;  ///< in source frames, 16.16
    std::uint32_t step = 0x10000;

    [[nodiscard]] bool active() const noexcept { return clip != nullptr || stream != nullptr; }
    void reset() noexcept {
      clip.reset();
      stream.reset();
      buffered = 0;
      drained = false;
    }
  };
  struct Pool {
    std::size_t first = 0;
    std::size_t count = 0;
    bool steal = false;
  };

  /// The voice a new sound on `pool` takes, or -1: the first free one, else
  /// the last when the pool steals.
  [[nodiscard]] int take(std::size_t pool) const noexcept;
  void start(Voice& voice, std::uint32_t source_rate, const PlayParams& params) const noexcept;
  /// Decode until a stream's voice holds every frame the next `frames`
  /// output frames read, after dropping the ones it has played.
  static void refill(Voice& voice, std::size_t frames);

  std::uint32_t rate_;
  std::vector<Pool> pools_;
  std::vector<Voice> voices_;
  std::vector<std::int32_t> scratch_;
};

}  // namespace imperivm::sound
