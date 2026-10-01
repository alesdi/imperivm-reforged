#include "imperivm/sound/sound_entity.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::sound {
namespace {

bool fail(std::string* error, std::string why) {
  if (error != nullptr) *error = std::move(why);
  return false;
}

std::string upper(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  return out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}

/// A whole decimal, or nothing.
std::optional<std::int32_t> number(std::string_view text) {
  text = trimmed(text);
  if (text.empty()) return std::nullopt;
  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
    if (value > 0x7FFFFFFF) return std::nullopt;
  }
  return static_cast<std::int32_t>(value);
}

struct TypeRow {
  SoundType type;
  std::string_view name;
  std::int32_t channels;
};

/// 0x0082e6a0: id, name, and the channels a type has by default.
constexpr TypeRow kTypes[] = {
    {SoundType::music, "Music", 1},       {SoundType::ambient, "Ambient", 1},
    {SoundType::unit_order, "UnitOrder", 1}, {SoundType::unit_fight, "UnitFight", 4},
    {SoundType::unit_idle, "UnitIdle", 1},  {SoundType::ui, "UI", 1},
    {SoundType::select, "Select", 1},      {SoundType::ambient2, "Ambient2", 1},
    {SoundType::conv_speech, "ConvSpeech", 1}, {SoundType::unit_walk, "UnitWalk", 2},
};

struct PriorityRow {
  std::string_view name;
  std::uint16_t value;
};

/// 0x0082e600. `Select`'s 4500 is a caller's number, not a word.
constexpr PriorityRow kPriorities[] = {
    {"Highest", kPriorityHighest},     {"UI", kPriorityUI},
    {"Music", kPriorityMusic},         {"Ambient", kPriorityAmbient},
    {"Ambient2", kPriorityAmbient2},   {"Event", kPriorityEvent},
    {"UnitOrder", kPriorityUnitOrder}, {"UnitFight", kPriorityUnitFight},
    {"UnitWalk", kPriorityUnitWalk},   {"UnitWork", kPriorityUnitWork},
    {"UnitIdle", kPriorityUnitIdle},   {"Lowest", kPriorityLowest},
};

/// The loader's evening-out (0x006b1400): zero weights share the shortfall,
/// and a silent filler takes what is still missing.
void even_out(std::vector<SoundVariant>& variants) {
  std::int32_t sum = 0;
  std::int32_t zeros = 0;
  for (const SoundVariant& v : variants) {
    sum += v.frequency;
    if (v.frequency == 0) ++zeros;
  }
  if (zeros > 0 && sum < 100) {
    const std::int32_t share = (100 - sum) / zeros;
    for (SoundVariant& v : variants) {
      if (v.frequency == 0) {
        v.frequency = share;
        sum += share;
      }
    }
  }
  if (sum < 100) variants.push_back(SoundVariant{std::string(), 100 - sum});
}

}  // namespace

SoundType type_from_name(std::string_view word) noexcept {
  word = trimmed(word);
  if (const auto n = number(word)) {
    return *n >= 1 && *n < static_cast<std::int32_t>(kSoundTypes) ? static_cast<SoundType>(*n) : SoundType::none;
  }
  for (const TypeRow& row : kTypes) {
    if (iequals(row.name, word)) return row.type;
  }
  return SoundType::none;
}

std::string_view type_name(SoundType type) noexcept {
  for (const TypeRow& row : kTypes) {
    if (row.type == type) return row.name;
  }
  return "";
}

std::int32_t default_channels(SoundType type) noexcept {
  for (const TypeRow& row : kTypes) {
    if (row.type == type) return row.channels;
  }
  return 0;
}

std::uint16_t priority_from_name(std::string_view text) noexcept {
  text = trimmed(text);
  if (const auto n = number(text)) return static_cast<std::uint16_t>(std::min<std::int32_t>(*n, 0xFFFF));
  // `Word + N` or `Word - N`.
  std::int32_t offset = 0;
  const std::size_t sign = text.find_first_of("+-");
  std::string_view word = text;
  if (sign != std::string_view::npos) {
    const auto n = number(text.substr(sign + 1));
    if (!n) return kPriorityLowest;
    offset = text[sign] == '+' ? *n : -*n;
    word = trimmed(text.substr(0, sign));
  }
  for (const PriorityRow& row : kPriorities) {
    if (iequals(row.name, word)) {
      return static_cast<std::uint16_t>(std::clamp<std::int32_t>(row.value + offset, 0, 0xFFFF));
    }
  }
  return kPriorityLowest;
}

std::int32_t SoundEntity::total_frequency() const noexcept {
  std::int32_t sum = 0;
  for (const SoundVariant& v : variants) sum += v.frequency;
  return sum;
}

bool parse_sound_entity(std::span<const std::uint8_t> document, SoundEntity* out, std::string* error) {
  if (out == nullptr) return fail(error, "nowhere to parse into");
  auto parsed = core::XmlDocument::parse(std::as_bytes(document));
  if (!parsed) return fail(error, "not XML");
  const core::XmlDocument& xml = *parsed;
  const core::NodeIndex root = xml.root();
  if (root == core::kNoNode || xml.node(root).name != "entity") return fail(error, "the root is not <entity>");
  out->description = std::string(xml.attribute(root, "description"));
  out->priority = priority_from_name(xml.attribute(root, "priority"));
  out->type = type_from_name(xml.attribute(root, "type"));
  out->last = 0xFFFF;
  out->variants.clear();
  for (core::NodeIndex files = xml.child(root, "files"); files != core::kNoNode; files = xml.next(files, "files")) {
    for (core::NodeIndex s = xml.child(files, "sound"); s != core::kNoNode; s = xml.next(s, "sound")) {
      SoundVariant variant;
      variant.file = std::string(xml.attribute(s, "file"));
      variant.frequency = std::max<std::int32_t>(0, xml.attribute_int(s, "frequency", 0));
      out->variants.push_back(std::move(variant));
    }
  }
  even_out(out->variants);
  return true;
}

std::optional<std::size_t> choose_variant(SoundEntity& entity, SoundRandom& random) {
  const std::size_t count = entity.variants.size();
  if (count == 0) return std::nullopt;
  std::size_t pick = 0;
  if (count > 1) {
    const std::int32_t roll = random.next() % 100;
    std::int32_t running = 0;
    pick = count;
    for (std::size_t i = 0; i < count; ++i) {
      running += entity.variants[i].frequency;
      if (running > roll) {
        pick = i;
        break;
      }
    }
    if (pick == count) return std::nullopt;
    if (count >= 3 && pick == entity.last) pick = (pick + 1) % count;
  }
  entity.last = static_cast<std::uint16_t>(pick);
  if (entity.variants[pick].file.empty()) return std::nullopt;
  return pick;
}

SoundRef resolve_sound_ref(std::string_view value) {
  const std::string_view v = trimmed(value);
  const std::size_t slash = v.find_last_of("/\\");
  const std::string_view name = slash == std::string_view::npos ? v : v.substr(slash + 1);
  const std::size_t dot = name.find_last_of('.');
  const std::string_view ext = dot == std::string_view::npos ? std::string_view() : name.substr(dot);
  SoundRef ref;
  ref.stem = std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
  if (!ext.empty() && !iequals(ext, ".xml")) {
    ref.kind = SoundRefKind::file;
    ref.path = std::string(v);
  } else {
    ref.kind = SoundRefKind::entity;
    ref.path = "DATA\\SOUND ENTITIES\\" + upper(ref.stem) + ".XML";
  }
  return ref;
}

SoundEntity* SoundBank::entity(std::string_view value) {
  const std::string_view key = trimmed(value);
  if (key.empty()) return nullptr;
  if (auto it = entities_.find(key); it != entities_.end()) return it->second.get();

  const SoundRef ref = resolve_sound_ref(key);
  auto entity = std::make_unique<SoundEntity>();
  entity->name = ref.stem;
  bool found = false;
  if (ref.kind == SoundRefKind::entity) {
    const std::span<const std::uint8_t> bytes = reader_(ref.path);
    found = !bytes.empty() && parse_sound_entity(bytes, entity.get());
  }
  if (!found) {
    // A file, or a stem that names no entity: played as a file, which must
    // be a WAV or an Ogg the installation holds.
    const std::string file(key);
    const std::size_t dot = file.find_last_of('.');
    const std::string_view ext = dot == std::string::npos ? std::string_view() : std::string_view(file).substr(dot);
    if ((iequals(ext, ".wav") || iequals(ext, ".ogg")) && !reader_(file).empty()) {
      *entity = SoundEntity{};
      entity->name = ref.stem;
      entity->variants.push_back(SoundVariant{file, 100});
      found = true;
    }
  }
  auto [it, inserted] = entities_.emplace(std::string(key), found ? std::move(entity) : nullptr);
  (void)inserted;
  return it->second.get();
}

SoundConfig default_sound_config() {
  SoundConfig config;
  for (const TypeRow& row : kTypes) config.channels[static_cast<std::size_t>(row.type)] = row.channels;
  return config;
}

SoundConfig parse_sound_config(std::span<const std::uint8_t> ini_text) {
  SoundConfig config = default_sound_config();
  auto parsed = core::IniDocument::parse(std::as_bytes(ini_text));
  if (!parsed) return config;
  const core::IniDocument& ini = *parsed;
  if (const core::SectionIndex s = ini.section("SoundConfig"); s != core::kNoSection) {
    config.sound = ini.value_int(s, "Sound", 1) != 0;
    config.music = ini.value_int(s, "Music", 1) != 0;
    config.reverse_speakers = ini.value_int(s, "ReverseSpeakers", 0) != 0;
  }
  if (const core::SectionIndex s = ini.section("SoundChannels"); s != core::kNoSection) {
    config.channels.fill(0);
    for (const core::IniEntry& entry : ini.entries_of(s)) {
      if (!entry.has_key) continue;
      const SoundType type = type_from_name(entry.key);
      const auto n = number(entry.value);
      if (type != SoundType::none && n) config.channels[static_cast<std::size_t>(type)] = *n;
    }
  }
  return config;
}

}  // namespace imperivm::sound
