#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace imperivm::sound {

/// The original's sound types: the channel pools a sound plays on, and what
/// the options switch and scale. The numbers are the original's ids
/// (0x0082e6a0); 0 is "none", which a caller passes to mean "the entity's".
enum class SoundType : std::uint8_t {
  none = 0,
  music = 1,
  ambient = 2,
  unit_order = 3,  ///< the acknowledgements
  unit_fight = 4,
  unit_idle = 5,
  ui = 6,
  select = 7,
  ambient2 = 8,
  conv_speech = 9,
  unit_walk = 10,
};
inline constexpr std::size_t kSoundTypes = 11;

/// A type word, case-insensitive, or its number. `none` for anything else --
/// including `Event`, which is a priority and not a type.
[[nodiscard]] SoundType type_from_name(std::string_view word) noexcept;
[[nodiscard]] std::string_view type_name(SoundType type) noexcept;
/// The channels a type has when `config.ini` does not say (0x0082e6a0).
[[nodiscard]] std::int32_t default_channels(SoundType type) noexcept;

/// The original's priority words (0x0082e600). Lower is more important. The
/// priority is stored on every sound and read by nothing anyone has found:
/// channel allocation ignores it (docs/engine/sound.md).
inline constexpr std::uint16_t kPriorityHighest = 1;
inline constexpr std::uint16_t kPriorityUI = 300;
inline constexpr std::uint16_t kPriorityMusic = 500;
inline constexpr std::uint16_t kPriorityAmbient = 700;
inline constexpr std::uint16_t kPriorityAmbient2 = 800;
inline constexpr std::uint16_t kPriorityEvent = 2000;
inline constexpr std::uint16_t kPriorityUnitOrder = 3000;
inline constexpr std::uint16_t kPriorityUnitFight = 4000;
inline constexpr std::uint16_t kPrioritySelect = 4500;
inline constexpr std::uint16_t kPriorityUnitWalk = 4750;
inline constexpr std::uint16_t kPriorityUnitWork = 5000;
inline constexpr std::uint16_t kPriorityUnitIdle = 6000;
inline constexpr std::uint16_t kPriorityLowest = 0xFFFF;

/// A priority as an entity writes it: a number, a word, or a word plus or
/// minus a number. Absent or unknown is `kPriorityLowest`.
[[nodiscard]] std::uint16_t priority_from_name(std::string_view text) noexcept;

/// One `<sound file= frequency=/>`, or the silent filler the loader adds.
struct SoundVariant {
  std::string file;            ///< as written; empty for the filler
  std::int32_t frequency = 0;  ///< a percentage of `rand() % 100`
};

/// One `DATA\SOUND ENTITIES\*.XML`: `<entity description= priority= type=>`
/// holding `<files>` of `<sound>`s, as the original's loader leaves it.
///
/// **Its name is its file's stem** (0x006b13e7); `description` is kept and
/// read by nothing. The loader evens the weights out to a hundred: entries
/// of weight 0 share what is missing (integer division), and if the sum is
/// still short a silent filler takes the rest, so an entity summing to 70 is
/// silent three times in ten, and one with no sounds always (0x006b1400).
struct SoundEntity {
  std::string name;
  std::string description;
  std::uint16_t priority = kPriorityLowest;
  SoundType type = SoundType::none;
  std::vector<SoundVariant> variants;  ///< in document order, filler last
  /// The variant played last, for the no-repeat rule; 0xFFFF before any.
  std::uint16_t last = 0xFFFF;

  [[nodiscard]] std::int32_t total_frequency() const noexcept;
};

/// Parse one entity document and even its weights out. False, with the
/// reason, when it is not XML or its root is not `<entity>`.
bool parse_sound_entity(std::span<const std::uint8_t> document, SoundEntity* out,
                        std::string* error = nullptr);

/// The C runtime's `rand()`, the generator the original draws variants with
/// (0x007638f2): a 32-bit LCG, 214013 and 2531011, fifteen bits out. It is
/// the presentation's own and never the world's; the seed is this engine's.
class SoundRandom {
 public:
  explicit SoundRandom(std::uint32_t seed = 1) : state_(seed) {}
  /// 0 to 32767.
  std::int32_t next() noexcept {
    state_ = state_ * 214013u + 2531011u;
    return static_cast<std::int32_t>((state_ >> 16) & 0x7FFF);
  }
  /// `lo` to `hi`, both included: the game's `Random(lo, hi)` shape, drawn
  /// from this generator rather than the world's.
  std::int32_t between(std::int32_t lo, std::int32_t hi) noexcept {
    if (hi <= lo) return lo;
    return lo + next() % (hi - lo + 1);
  }

 private:
  std::uint32_t state_;
};

/// The variant to play (0x006b0da0), or none for silence. One variant plays
/// without a draw. Otherwise `rand() % 100` walks the cumulative weights;
/// with three or more, a pick equal to the last one moves on to the next,
/// wrapping. The filler, or a draw past every weight, is silence.
[[nodiscard]] std::optional<std::size_t> choose_variant(SoundEntity& entity, SoundRandom& random);

/// What a class's `<sounds>` value, or a script's sound name, names
/// (0x006b0910). The value is split as a path: with an extension other
/// than `.xml` it is a file; otherwise its **stem** names an entity,
/// whatever directory is written before it -- which is how the four
/// `Sounds/entities/Voice*.xml` values, a directory that does not exist,
/// still find their voices. A stem naming no entity is played as a file,
/// and a file must end in `.wav` or `.ogg`.
enum class SoundRefKind : std::uint8_t { entity, file };

struct SoundRef {
  SoundRefKind kind = SoundRefKind::entity;
  /// `DATA\SOUND ENTITIES\<STEM>.XML` for an entity; the value for a file.
  std::string path;
  std::string stem;  ///< the value's file name without its extension
};

[[nodiscard]] SoundRef resolve_sound_ref(std::string_view value);

/// Sound entities read from the installation on first use and kept, with
/// their no-repeat state.
///
/// The reader answers the bytes at an installation path, or an empty span.
/// A value that is a file, or a stem naming no entity, becomes an entity of
/// one variant with no priority or type of its own, so every caller plays
/// an entity and says which type.
class SoundBank {
 public:
  using Reader = std::function<std::span<const std::uint8_t>(std::string_view path)>;

  explicit SoundBank(Reader reader) : reader_(std::move(reader)) {}

  /// The entity a value names, or null when it names nothing the
  /// installation holds.
  SoundEntity* entity(std::string_view value);

  [[nodiscard]] std::size_t size() const noexcept { return entities_.size(); }

 private:
  Reader reader_;
  std::map<std::string, std::unique_ptr<SoundEntity>, std::less<>> entities_;  ///< null: absent
};

/// `config.ini`'s `[SoundConfig]` and `[SoundChannels]`, read from its plain
/// text (the file itself is LZIS; the caller unpacks it).
struct SoundConfig {
  bool sound = true;              ///< `Sound`: 0 never starts the sound manager
  bool music = true;              ///< `Music`
  bool reverse_speakers = false;  ///< `ReverseSpeakers`
  /// Channels per type, indexed by `SoundType`. With a `[SoundChannels]`
  /// section, a type it does not list has none and can never play; without
  /// one, `default_channels`.
  std::array<std::int32_t, kSoundTypes> channels{};
};

[[nodiscard]] SoundConfig parse_sound_config(std::span<const std::uint8_t> ini_text);
[[nodiscard]] SoundConfig default_sound_config();

}  // namespace imperivm::sound
