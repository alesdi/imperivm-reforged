// The sound module on synthetic buffers: the WAV decoder, the sound-entity
// reader and bank, the variant choice, where a sound sits and how loud, and
// the mixer's arithmetic and its pools. Every fixture here is written from
// docs/engine/sound.md, never copied from the installation (docs/legal.md
// rule 1), and nothing opens a device: what is heard is `platform::Audio`'s,
// and tests assert on numbers.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "imperivm/sound/mixer.hpp"
#include "imperivm/sound/music.hpp"
#include "imperivm/sound/ogg.hpp"
#include "imperivm/sound/placement.hpp"
#include "imperivm/sound/sound_entity.hpp"
#include "imperivm/sound/wav.hpp"
#include "test.hpp"

namespace imperivm::test {

int failures = 0;
int checks = 0;

}  // namespace imperivm::test

namespace {

using imperivm::sound::Clip;
using imperivm::sound::Mixer;
using imperivm::sound::PlayParams;
using imperivm::sound::SoundBank;
using imperivm::sound::PoolSpec;
using imperivm::sound::SoundType;
using imperivm::sound::SoundEntity;
using imperivm::sound::SoundRefKind;

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& b, std::uint32_t v) {
  b.push_back(static_cast<std::uint8_t>(v));
  b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(Bytes& b, std::uint32_t v) {
  put16(b, v & 0xFFFF);
  put16(b, v >> 16);
}

void chunk(Bytes& b, const char* id, const Bytes& body) {
  b.insert(b.end(), id, id + 4);
  put32(b, static_cast<std::uint32_t>(body.size()));
  b.insert(b.end(), body.begin(), body.end());
  if (body.size() & 1u) b.push_back(0);
}

/// A RIFF WAVE of the given chunks, built as the format describes it.
Bytes riff(const std::vector<std::pair<const char*, Bytes>>& chunks) {
  Bytes inner;
  for (const auto& [id, body] : chunks) chunk(inner, id, body);
  Bytes b = {'R', 'I', 'F', 'F'};
  put32(b, static_cast<std::uint32_t>(inner.size() + 4));
  b.insert(b.end(), {'W', 'A', 'V', 'E'});
  b.insert(b.end(), inner.begin(), inner.end());
  return b;
}

Bytes pcm_fmt(std::uint16_t channels, std::uint32_t rate, std::uint16_t bits) {
  Bytes f;
  put16(f, 1);
  put16(f, channels);
  put32(f, rate);
  put32(f, rate * channels * bits / 8);
  put16(f, static_cast<std::uint16_t>(channels * bits / 8));
  put16(f, bits);
  return f;
}

std::shared_ptr<const Clip> clip_of(std::vector<std::int16_t> samples, std::uint16_t channels = 1,
                                    std::uint32_t rate = 44100) {
  auto clip = std::make_shared<Clip>();
  clip->samples = std::move(samples);
  clip->channels = channels;
  clip->rate = rate;
  return clip;
}

std::span<const std::uint8_t> span_of(const std::string& text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace

// -- WAV ----------------------------------------------------------------------

TEST(a_16_bit_pcm_wav_decodes_sample_for_sample) {
  Bytes data;
  for (const std::int16_t s : {0, 1000, -1000, 32767, -32768, 7}) put16(data, static_cast<std::uint16_t>(s));
  const Bytes wav = riff({{"fmt ", pcm_fmt(1, 22050, 16)}, {"data", data}});
  Clip clip;
  std::string why;
  REQUIRE(imperivm::sound::decode_wav(wav, &clip, &why));
  CHECK(clip.channels == 1);
  CHECK(clip.rate == 22050);
  CHECK((clip.samples == std::vector<std::int16_t>{0, 1000, -1000, 32767, -32768, 7}));
  CHECK(clip.frames() == 6);
}

TEST(a_stereo_wav_keeps_its_channels_interleaved) {
  Bytes data;
  for (const std::int16_t s : {1, -1, 2, -2}) put16(data, static_cast<std::uint16_t>(s));
  const Bytes wav = riff({{"LIST", Bytes{1, 2, 3}}, {"fmt ", pcm_fmt(2, 48000, 16)}, {"data", data}});
  Clip clip;
  REQUIRE(imperivm::sound::decode_wav(wav, &clip));
  CHECK(clip.channels == 2);
  CHECK(clip.frames() == 2);
  CHECK((clip.samples == std::vector<std::int16_t>{1, -1, 2, -2}));
}

TEST(an_8_bit_wav_is_unsigned_around_128) {
  const Bytes wav = riff({{"fmt ", pcm_fmt(1, 11025, 8)}, {"data", Bytes{128, 255, 0}}});
  Clip clip;
  REQUIRE(imperivm::sound::decode_wav(wav, &clip));
  CHECK((clip.samples == std::vector<std::int16_t>{0, 127 * 256, -128 * 256}));
}

TEST(a_data_chunk_longer_than_the_file_is_cut_at_its_end) {
  Bytes wav = riff({{"fmt ", pcm_fmt(1, 44100, 16)}, {"data", Bytes{1, 0, 2, 0}}});
  // Claim eight bytes of data where there are four.
  wav[wav.size() - 8] = 8;
  Clip clip;
  REQUIRE(imperivm::sound::decode_wav(wav, &clip));
  CHECK((clip.samples == std::vector<std::int16_t>{1, 2}));
}

TEST(what_is_not_a_wav_the_engine_decodes_is_refused_with_a_reason) {
  Clip clip;
  std::string why;
  CHECK(!imperivm::sound::decode_wav(Bytes{'n', 'o', 'p', 'e'}, &clip, &why));
  CHECK(!why.empty());
  Bytes fmt = pcm_fmt(1, 44100, 16);
  fmt[0] = 0x55;  // an MPEG layer 3 tag: not one the installation uses
  CHECK(!imperivm::sound::decode_wav(riff({{"fmt ", fmt}, {"data", Bytes{0, 0}}}), &clip, &why));
  CHECK(!imperivm::sound::decode_wav(riff({{"fmt ", pcm_fmt(1, 44100, 16)}}), &clip, &why));
  CHECK(!imperivm::sound::decode_wav(riff({{"fmt ", pcm_fmt(3, 44100, 16)}, {"data", Bytes{0, 0}}}), &clip, &why));
}

namespace {

/// A mono Microsoft ADPCM `fmt ` with a block of `block_align` bytes and two
/// coefficient pairs: (256, 0), which predicts the last sample, and
/// (512, -256), which extends the line through the last two.
Bytes adpcm_fmt(std::uint16_t block_align) {
  Bytes f;
  put16(f, 2);
  put16(f, 1);
  put32(f, 44100);
  put32(f, 22000);
  put16(f, block_align);
  put16(f, 4);
  put16(f, 8);  // cbSize
  put16(f, static_cast<std::uint16_t>((block_align - 7) * 2 + 2));
  put16(f, 2);
  for (const std::int16_t c : {256, 0, 512, -256}) put16(f, static_cast<std::uint16_t>(c));
  return f;
}

/// A mono block's header: predictor, delta, newest sample, older sample.
Bytes adpcm_header(std::uint8_t predictor, std::int16_t delta, std::int16_t sample1, std::int16_t sample2) {
  Bytes b = {predictor};
  put16(b, static_cast<std::uint16_t>(delta));
  put16(b, static_cast<std::uint16_t>(sample1));
  put16(b, static_cast<std::uint16_t>(sample2));
  return b;
}

}  // namespace

TEST(ms_adpcm_expands_nibbles_by_the_published_rule) {
  // Predictor 0 keeps the last sample; each nibble adds itself times the
  // step, and the step adapts: 16 -> 16 (nibble 1, 230/256, floored at 16)
  // -> 38 (nibble 7, 614/256) -> 34 (nibble -1, i.e. 15, 230/256) -> 30.
  Bytes block = adpcm_header(0, 16, 100, 50);
  block.push_back(0x17);
  block.push_back(0xF0);
  const Bytes wav = riff({{"fmt ", adpcm_fmt(static_cast<std::uint16_t>(block.size()))}, {"data", block}});
  Clip clip;
  std::string why;
  REQUIRE(imperivm::sound::decode_wav(wav, &clip, &why));
  CHECK(clip.channels == 1);
  CHECK(clip.rate == 44100);
  CHECK((clip.samples == std::vector<std::int16_t>{50, 100, 116, 228, 190, 190}));
}

TEST(ms_adpcm_reads_every_block_and_a_short_last_one) {
  // Two blocks of 9 bytes and a third of 8: the header and one nibble pair,
  // then a header and nothing but half a byte's worth.
  Bytes data;
  for (int b = 0; b < 2; ++b) {
    Bytes block = adpcm_header(1, 16, static_cast<std::int16_t>(10 * (b + 1)), 0);
    block.push_back(0x00);
    block.push_back(0x00);
    data.insert(data.end(), block.begin(), block.end());
  }
  Bytes last = adpcm_header(0, 16, -5, -6);
  last.push_back(0x10);
  data.insert(data.end(), last.begin(), last.end());
  const Bytes wav = riff({{"fmt ", adpcm_fmt(9)}, {"data", data}});
  Clip clip;
  REQUIRE(imperivm::sound::decode_wav(wav, &clip));
  // Predictor 1 extends the line 0, 10 by 10 a sample; predictor 0 holds.
  CHECK((clip.samples == std::vector<std::int16_t>{0, 10, 20, 30, 40, 50, 0, 20, 40, 60, 80, 100,
                                                   -6, -5, 11, 11}));
}

TEST(an_adpcm_fact_count_trims_but_never_extends) {
  Bytes block = adpcm_header(0, 16, 100, 50);
  block.push_back(0x17);
  block.push_back(0xF0);
  Bytes fact;
  put32(fact, 4);
  Clip clip;
  REQUIRE(imperivm::sound::decode_wav(riff({{"fmt ", adpcm_fmt(11)}, {"data", block}, {"fact", fact}}), &clip));
  CHECK((clip.samples == std::vector<std::int16_t>{50, 100, 116, 228}));
  // A count past what the data holds -- 161 of the shipped voices -- plays
  // what there is.
  fact.clear();
  put32(fact, 2000);
  REQUIRE(imperivm::sound::decode_wav(riff({{"fmt ", adpcm_fmt(11)}, {"data", block}, {"fact", fact}}), &clip));
  CHECK(clip.samples.size() == 6);
}

// -- sound entities -----------------------------------------------------------

namespace {

// Written from the format in docs/engine/sound.md; no shipped entity reads so.
const std::string kEntity = R"XML(<!-- a comment before the root -->
<entity description="Test Squad Voice" priority="UnitOrder" type="UnitFight">
  <files>
    <sound file="Sounds/test/first.wav" frequency="50"/>
    <sound file="Sounds/test/second.wav" frequency="30"/>
    <sound file="Sounds/test/third.wav" frequency="20"/>
  </files>
</entity>
)XML";

SoundEntity parsed(const std::string& text) {
  SoundEntity entity;
  std::string why;
  imperivm::test::check(imperivm::sound::parse_sound_entity(span_of(text), &entity, &why), "parses", __FILE__,
                        __LINE__);
  return entity;
}

/// An entity of the given weights, files named by their index.
SoundEntity weighted(std::initializer_list<int> weights) {
  std::string text = R"XML(<entity description="w"><files>)XML";
  int i = 0;
  for (const int w : weights) {
    text += "<sound file=\"f" + std::to_string(i++) + ".wav\" frequency=\"" + std::to_string(w) + "\"/>";
  }
  text += "</files></entity>";
  return parsed(text);
}

}  // namespace

TEST(a_sound_entity_reads_its_words_and_variants_in_order) {
  const SoundEntity entity = parsed(kEntity);
  CHECK(entity.description == "Test Squad Voice");
  CHECK(entity.priority == imperivm::sound::kPriorityUnitOrder);
  CHECK(entity.type == SoundType::unit_fight);
  REQUIRE(entity.variants.size() == 3);
  CHECK(entity.variants[0].file == "Sounds/test/first.wav");
  CHECK(entity.variants[2].frequency == 20);
  CHECK(entity.total_frequency() == 100);
}

TEST(type_words_are_the_originals_ids_and_event_is_not_one) {
  using imperivm::sound::type_from_name;
  CHECK(type_from_name("Music") == SoundType::music);
  CHECK(type_from_name("unitorder") == SoundType::unit_order);  // case-insensitive
  CHECK(type_from_name("Select") == SoundType::select);
  CHECK(type_from_name("ConvSpeech") == SoundType::conv_speech);
  CHECK(static_cast<int>(type_from_name("UnitWalk")) == 10);
  CHECK(type_from_name("7") == SoundType::select);
  CHECK(type_from_name("Event") == SoundType::none);
  CHECK(type_from_name("UnitWork") == SoundType::none);
  CHECK(type_from_name("") == SoundType::none);
  CHECK(imperivm::sound::type_name(SoundType::ambient2) == "Ambient2");
  CHECK(imperivm::sound::default_channels(SoundType::unit_fight) == 4);
  CHECK(imperivm::sound::default_channels(SoundType::unit_walk) == 2);
  CHECK(imperivm::sound::default_channels(SoundType::none) == 0);
}

TEST(priorities_are_words_numbers_or_a_word_and_an_offset) {
  using imperivm::sound::priority_from_name;
  CHECK(priority_from_name("UI") == 300);
  CHECK(priority_from_name("Event") == 2000);
  CHECK(priority_from_name("unitwalk") == 4750);
  CHECK(priority_from_name("1234") == 1234);
  CHECK(priority_from_name("UnitFight + 10") == 4010);
  CHECK(priority_from_name("UnitFight-10") == 3990);
  CHECK(priority_from_name("") == 0xFFFF);
  CHECK(priority_from_name("Loudest") == 0xFFFF);
}

TEST(the_loader_evens_weights_out_to_a_hundred) {
  // Zero weights share the shortfall.
  SoundEntity e = weighted({50, 0, 0});
  REQUIRE(e.variants.size() == 3);
  CHECK(e.variants[1].frequency == 25 && e.variants[2].frequency == 25);
  // Short: a silent filler takes the rest.
  e = weighted({40, 30});
  REQUIRE(e.variants.size() == 3);
  CHECK(e.variants[2].file.empty() && e.variants[2].frequency == 30);
  // Zeros sharing unevenly leave the remainder to the filler.
  e = weighted({33, 0, 0});
  REQUIRE(e.variants.size() == 4);
  CHECK(e.variants[1].frequency == 33 && e.variants[2].frequency == 33 && e.variants[3].frequency == 1);
  // Over a hundred is left alone.
  e = weighted({80, 70});
  CHECK(e.variants.size() == 2 && e.total_frequency() == 150);
  // Nothing at all is a filler of a hundred: always silent.
  e = weighted({});
  REQUIRE(e.variants.size() == 1);
  imperivm::sound::SoundRandom random;
  CHECK(!imperivm::sound::choose_variant(e, random).has_value());
  const std::string wrong = R"XML(<class id="NotAnEntity"/>)XML";
  CHECK(!imperivm::sound::parse_sound_entity(span_of(wrong), &e));
}

TEST(the_generator_is_the_c_runtimes) {
  imperivm::sound::SoundRandom random;  // seed 1, the runtime's own default
  CHECK(random.next() == 41);
  CHECK(random.next() == 18467);
  CHECK(random.next() == 6334);
  CHECK(random.next() == 26500);
  imperivm::sound::SoundRandom again(1);
  CHECK(again.between(0, 99) == 41);
}

TEST(a_draw_walks_the_weights_and_three_or_more_never_repeat) {
  // Seed 1 draws 41 67 34 0 69 24 78 58 62 64 modulo 100. Against 50/30/20:
  // 0 1 0 0 1 0 1 1 1 1, and every repeat moves on one.
  SoundEntity three = parsed(kEntity);
  imperivm::sound::SoundRandom random;
  std::vector<std::size_t> picks;
  for (int i = 0; i < 10; ++i) picks.push_back(*imperivm::sound::choose_variant(three, random));
  CHECK((picks == std::vector<std::size_t>{0, 1, 0, 1, 2, 0, 1, 2, 1, 2}));
  // Two may repeat.
  SoundEntity two = weighted({60, 40});
  imperivm::sound::SoundRandom again;
  picks.clear();
  for (int i = 0; i < 4; ++i) picks.push_back(*imperivm::sound::choose_variant(two, again));
  CHECK((picks == std::vector<std::size_t>{0, 1, 0, 0}));
  // One plays without a draw.
  SoundEntity one = weighted({100});
  imperivm::sound::SoundRandom third;
  CHECK(imperivm::sound::choose_variant(one, third) == 0u);
  CHECK(third.next() == 41);
}

TEST(a_short_entity_is_silent_as_often_as_it_is_short) {
  SoundEntity e = weighted({40, 30});
  imperivm::sound::SoundRandom random(12345);
  int silent = 0;
  for (int i = 0; i < 10000; ++i) {
    if (!imperivm::sound::choose_variant(e, random).has_value()) ++silent;
  }
  // The filler is drawn three times in ten, and the no-repeat rule moves
  // about as much onto it (a second "second" becomes the filler) as off it
  // (a second silence becomes the first): 30.9% in the long run.
  CHECK(silent > 2850 && silent < 3350);
}

TEST(a_sounds_value_names_an_entity_by_its_stem_or_is_a_file) {
  using imperivm::sound::resolve_sound_ref;
  auto ref = resolve_sound_ref("TestSelect");
  CHECK(ref.kind == SoundRefKind::entity);
  CHECK(ref.path == "DATA\\SOUND ENTITIES\\TESTSELECT.XML");
  ref = resolve_sound_ref(" data/sound entities/Voice Test.xml ");
  CHECK(ref.kind == SoundRefKind::entity);
  CHECK(ref.path == "DATA\\SOUND ENTITIES\\VOICE TEST.XML");
  CHECK(ref.stem == "Voice Test");
  ref = resolve_sound_ref("Anywhere/else/VoiceTest.XML");
  CHECK(ref.path == "DATA\\SOUND ENTITIES\\VOICETEST.XML");
  ref = resolve_sound_ref("Sounds/Test/one.WAV");
  CHECK(ref.kind == SoundRefKind::file);
  CHECK(ref.path == "Sounds/Test/one.WAV");
  CHECK(ref.stem == "one");
}

TEST(the_bank_resolves_stems_and_files_and_remembers) {
  std::map<std::string, std::string> files = {
      {"DATA\\SOUND ENTITIES\\TESTSELECT.XML", kEntity},
      {"DATA\\SOUND ENTITIES\\VOICETEST.XML", kEntity},
      {"Sounds/Test/one.wav", "RIFF"},
  };
  int reads = 0;
  SoundBank bank([&](std::string_view path) -> std::span<const std::uint8_t> {
    ++reads;
    const auto it = files.find(std::string(path));
    return it == files.end() ? std::span<const std::uint8_t>() : span_of(it->second);
  });
  SoundEntity* named = bank.entity("TestSelect");
  REQUIRE(named != nullptr);
  CHECK(named->name == "TestSelect");
  CHECK(named->variants.size() == 3);
  // A path that is not where it says is found by its stem.
  SoundEntity* moved = bank.entity("Sounds/entities/VoiceTest.xml");
  REQUIRE(moved != nullptr);
  CHECK(moved->name == "VoiceTest");
  CHECK(moved->priority == imperivm::sound::kPriorityUnitOrder);
  // A file is an entity of one, with no type of its own.
  SoundEntity* wav = bank.entity("Sounds/Test/one.wav");
  REQUIRE(wav != nullptr);
  REQUIRE(wav->variants.size() == 1);
  CHECK(wav->variants[0].file == "Sounds/Test/one.wav");
  CHECK(wav->type == SoundType::none);
  CHECK(bank.entity("nothing") == nullptr);
  CHECK(bank.entity("Sounds/Test/one.txt") == nullptr);
  CHECK(bank.entity("") == nullptr);
  const int before = reads;
  CHECK(bank.entity("TestSelect") == named);
  CHECK(bank.entity("nothing") == nullptr);
  CHECK(reads == before);
}

TEST(config_ini_names_the_channels_of_each_type) {
  const std::string text =
      "[SoundConfig]\nSound = 1\nMusic = 0\nReverseSpeakers = 1\n\n"
      "[SoundChannels]\nUnitOrder = 2\nUnitFight = 6 ; more\nUI = 1\n";
  const imperivm::sound::SoundConfig c = imperivm::sound::parse_sound_config(span_of(text));
  CHECK(c.sound && !c.music && c.reverse_speakers);
  CHECK(c.channels[static_cast<std::size_t>(SoundType::unit_order)] == 2);
  CHECK(c.channels[static_cast<std::size_t>(SoundType::unit_fight)] == 6);
  CHECK(c.channels[static_cast<std::size_t>(SoundType::ui)] == 1);
  // Listed nowhere: none, and never plays.
  CHECK(c.channels[static_cast<std::size_t>(SoundType::music)] == 0);
  // No section: the compiled-in counts.
  const imperivm::sound::SoundConfig d = imperivm::sound::parse_sound_config(span_of("[SoundConfig]\n"));
  CHECK(d.channels[static_cast<std::size_t>(SoundType::unit_fight)] == 4);
  CHECK(d.channels[static_cast<std::size_t>(SoundType::music)] == 1);
}

// -- where and how loud -------------------------------------------------------

namespace {

imperivm::sound::View view_of_1000_by_800() {
  imperivm::sound::View v;
  v.left = 0;
  v.top = 0;
  v.right = 1000;
  v.bottom = 800;
  v.screen_left = 0;
  v.screen_right = 1000;
  return v;
}

}  // namespace

TEST(distance_attenuates_five_millibels_a_unit_beyond_500) {
  const imperivm::sound::View v = view_of_1000_by_800();
  using imperivm::sound::attenuation;
  CHECK(attenuation(v, 500, 400) == 0);
  CHECK(attenuation(v, 1000, 800) == 0);  // the edge is inside
  // Centre (500, 400). x 1100 is 600 away: 100 beyond, -500.
  CHECK(attenuation(v, 1100, 400) == -500);
  // Chebyshev: the larger of the two distances.
  CHECK(attenuation(v, 1100, 1300) == -5 * (900 - 500));
  CHECK(attenuation(v, 500 + 1700, 400) == -6000);
  CHECK(attenuation(v, 500 + 1701, 400) == -6000);
  // Just outside a view narrower than 500 either side comes out positive.
  imperivm::sound::View narrow = v;
  narrow.right = 200;
  CHECK(attenuation(narrow, 300, 400) == 5 * (500 - 200));
}

TEST(on_screen_is_centred_and_off_it_pans_ten_a_unit_towards_its_side) {
  imperivm::sound::View v = view_of_1000_by_800();
  using imperivm::sound::pan;
  CHECK(pan(v, 1) == 0);
  CHECK(pan(v, 999) == 0);
  CHECK(pan(v, -100) == -1000);  // left of the screen, to the left
  CHECK(pan(v, 1050) == 500);
  CHECK(pan(v, -5000) == -10000);
  CHECK(pan(v, 0) == 0);  // on the edge: past it by nothing
  v.reverse_speakers = true;
  CHECK(pan(v, -100) == 1000);
}

TEST(the_slider_lifts_a_sound_onto_sixty_decibels) {
  using imperivm::sound::volume;
  CHECK(volume(0, 100, 100) == 0);
  CHECK(volume(0, 100, 68) == -1920);
  CHECK(volume(0, 100, 52) == -2880);
  CHECK(volume(0, 100, 0) == -6000);
  CHECK(volume(-500, 100, 100) == -500);
  CHECK(volume(-6000, 100, 100) == -6000);
  CHECK(volume(-1000, 50, 50) == -6000 + 5000 / 2 / 2);
}

// -- the mixer ----------------------------------------------------------------

TEST(millibels_come_to_q15_gains) {
  CHECK(Mixer::gain(0) == 32768);
  CHECK(Mixer::gain(250) == 32768);
  CHECK(Mixer::gain(-600) == 16423);   // 10^-0.3
  CHECK(Mixer::gain(-2000) == 3277);   // a tenth
  CHECK(Mixer::gain(-6000) == 33);     // a thousandth
  CHECK(Mixer::gain(-10000) == 0);
  CHECK(Mixer::gain(-20000) == 0);
  auto g = Mixer::gains(0, 0);
  CHECK(g.left == 32768 && g.right == 32768);
  g = Mixer::gains(0, 10000);
  CHECK(g.left == 0 && g.right == 32768);
  g = Mixer::gains(-600, -2000);  // to the left: the right drops by 20 dB more
  CHECK(g.left == 16423 && g.right == Mixer::gain(-2600));
}

namespace {

std::vector<PoolSpec> one_pool(std::size_t voices, bool steal = false) { return {PoolSpec{voices, steal}}; }

}  // namespace

TEST(one_voice_at_full_volume_mixes_to_itself) {
  Mixer mixer(44100, one_pool(4));
  REQUIRE(mixer.play(clip_of({100, -200, 300}), PlayParams{}) == 0);
  std::vector<std::int16_t> out(10, 7);
  mixer.mix(out.data(), 5);
  CHECK((out == std::vector<std::int16_t>{100, 100, -200, -200, 300, 300, 0, 0, 0, 0}));
  CHECK(mixer.playing() == 0);
}

TEST(voices_sum_scaled_by_volume_and_pan_and_clip) {
  Mixer mixer(44100, one_pool(4));
  PlayParams quieter;
  quieter.volume = -600;  // 16423 / 32768
  quieter.pan = -600;     // and the right as much again
  REQUIRE(mixer.play(clip_of({1000, 1000}), quieter) >= 0);
  REQUIRE(mixer.play(clip_of({400, 400}), PlayParams{}) >= 0);
  std::vector<std::int16_t> out(4);
  mixer.mix(out.data(), 2);
  CHECK(out[0] == 1000 * 16423 / 32768 + 400);
  CHECK(out[1] == 1000 * Mixer::gain(-1200) / 32768 + 400);
  REQUIRE(mixer.play(clip_of({30000}), PlayParams{}) >= 0);
  REQUIRE(mixer.play(clip_of({30000}), PlayParams{}) >= 0);
  REQUIRE(mixer.play(clip_of({-30000}), PlayParams{}) >= 0);
  REQUIRE(mixer.play(clip_of({-30000}), PlayParams{}) >= 0);
  mixer.mix(out.data(), 1);
  CHECK(out[0] == 0);
  REQUIRE(mixer.play(clip_of({30000}), PlayParams{}) >= 0);
  REQUIRE(mixer.play(clip_of({30000}), PlayParams{}) >= 0);
  mixer.mix(out.data(), 1);
  CHECK(out[0] == 32767 && out[1] == 32767);  // clipped, not wrapped
}

TEST(a_stereo_clip_keeps_left_and_right) {
  Mixer mixer(44100, one_pool(2));
  PlayParams right;
  right.pan = 10000;
  REQUIRE(mixer.play(clip_of({500, -500, 600, -600}, 2), right) == 0);
  std::vector<std::int16_t> out(4);
  mixer.mix(out.data(), 2);
  CHECK((out == std::vector<std::int16_t>{0, -500, 0, -600}));
}

TEST(a_half_rate_clip_is_interpolated_to_twice_its_frames) {
  Mixer mixer(44100, one_pool(1));
  REQUIRE(mixer.play(clip_of({0, 1000, 2000}, 1, 22050), PlayParams{}) == 0);
  std::vector<std::int16_t> out(16);
  mixer.mix(out.data(), 8);
  std::vector<std::int16_t> left;
  for (std::size_t i = 0; i < out.size(); i += 2) left.push_back(out[i]);
  // Midpoints between samples; the last sample is held, then silence.
  CHECK((left == std::vector<std::int16_t>{0, 500, 1000, 1500, 2000, 2000, 0, 0}));
  CHECK(mixer.playing() == 0);
}

TEST(a_clip_plays_on_across_calls_to_mix) {
  Mixer mixer(44100, one_pool(1));
  REQUIRE(mixer.play(clip_of({1, 2, 3, 4, 5}), PlayParams{}) == 0);
  std::vector<std::int16_t> out(4);
  mixer.mix(out.data(), 2);
  CHECK(out[0] == 1 && out[2] == 2);
  mixer.mix(out.data(), 2);
  CHECK(out[0] == 3 && out[2] == 4);
  CHECK(mixer.playing() == 1);
  mixer.mix(out.data(), 2);
  CHECK(out[0] == 5 && out[2] == 0);
  CHECK(mixer.playing() == 0);
}

TEST(a_full_pool_drops_the_new_sound_unless_it_steals_its_last_voice) {
  // Pool 0: two voices that drop. Pool 1: two that steal. Pool 2: none.
  Mixer mixer(44100, {PoolSpec{2, false}, PoolSpec{2, true}, PoolSpec{0, false}});
  CHECK(mixer.voices() == 4);
  auto a = clip_of(std::vector<std::int16_t>(1000, 1));
  auto b = clip_of(std::vector<std::int16_t>(1000, 2));
  auto c = clip_of(std::vector<std::int16_t>(1000, 3));
  PlayParams p0;
  PlayParams p1;
  p1.pool = 1;
  PlayParams p2;
  p2.pool = 2;
  CHECK(mixer.play(a, p0) == 0);
  CHECK(!mixer.full(0));
  CHECK(mixer.play(b, p0) == 1);
  CHECK(mixer.full(0));
  CHECK(mixer.play(c, p0) == -1);
  CHECK(mixer.clip(0) == a.get() && mixer.clip(1) == b.get());
  // Pools do not lend each other voices.
  CHECK(mixer.play(a, p1) == 2);
  CHECK(mixer.play(b, p1) == 3);
  CHECK(!mixer.full(1));
  CHECK(mixer.play(c, p1) == 3);  // the last voice, taken over
  CHECK(mixer.clip(2) == a.get() && mixer.clip(3) == c.get());
  CHECK(mixer.playing(1) == 2);
  CHECK(mixer.full(2));
  CHECK(mixer.play(a, p2) == -1);
  // A freed voice is taken again; stopping a pool stops only it.
  mixer.stop(0);
  CHECK(!mixer.full(0));
  CHECK(mixer.play(c, p0) == 0);
  mixer.stop_pool(1);
  CHECK(mixer.playing(1) == 0);
  CHECK(mixer.playing(0) == 2);
}

TEST(an_empty_clip_or_a_pool_that_is_not_there_takes_no_voice) {
  Mixer mixer(44100, one_pool(1));
  CHECK(mixer.play(std::shared_ptr<const Clip>(), PlayParams{}) == -1);
  CHECK(mixer.play(clip_of({}), PlayParams{}) == -1);
  PlayParams elsewhere;
  elsewhere.pool = 5;
  CHECK(mixer.play(clip_of({1}), elsewhere) == -1);
  CHECK(mixer.full(5));
  CHECK(mixer.playing() == 0);
}

// -- streams, and following the view ------------------------------------------

namespace {

/// A stream of a counting ramp, handed out in whatever pieces are asked for,
/// with a count of the calls so a test can see it was not read whole.
class Ramp final : public imperivm::sound::Stream {
 public:
  Ramp(std::size_t frames, std::uint16_t channels = 1, std::uint32_t rate = 44100)
      : frames_(frames), channels_(channels), rate_(rate) {}
  std::uint32_t rate() const noexcept override { return rate_; }
  std::uint16_t channels() const noexcept override { return channels_; }
  std::size_t read(std::int16_t* out, std::size_t frames) override {
    ++reads;
    const std::size_t n = std::min(frames, frames_ - next_);
    for (std::size_t f = 0; f < n; ++f) {
      for (std::uint16_t c = 0; c < channels_; ++c) {
        out[f * channels_ + c] = static_cast<std::int16_t>((next_ + f) * (c == 0 ? 1 : -1));
      }
    }
    next_ += n;
    largest = std::max(largest, n);
    return n;
  }
  int reads = 0;
  std::size_t largest = 0;

 private:
  std::size_t frames_;
  std::size_t next_ = 0;
  std::uint16_t channels_;
  std::uint32_t rate_;
};

}  // namespace

TEST(a_stream_plays_as_a_clip_would_and_is_read_a_block_at_a_time) {
  Mixer mixer(44100, one_pool(1, true));
  auto ramp = std::make_unique<Ramp>(10000);
  Ramp* seen = ramp.get();
  REQUIRE(mixer.play(std::move(ramp), PlayParams{}) == 0);
  CHECK(mixer.clip(0) == nullptr && mixer.busy(0));
  std::vector<std::int16_t> out(2 * 256);
  mixer.mix(out.data(), 256);
  CHECK(out[0] == 0 && out[1] == 0 && out[2] == 1 && out[510] == 255);
  // Not the whole of it: a block, then more as the mix reaches it.
  CHECK(seen->reads == 1 && seen->largest < 10000);
  std::size_t mixed = 256;
  while (mixer.busy(0) && mixed < 20000) {
    mixer.mix(out.data(), 256);
    if (mixed == 256 * 30) CHECK(out[0] == static_cast<std::int16_t>(256 * 30));
    mixed += 256;
  }
  CHECK(!mixer.busy(0));
  CHECK(mixed == 10240);  // the last mix ran past the end and freed the voice
  CHECK(out[2 * (10000 - 9984) - 2] == 9999 && out[2 * (10000 - 9984)] == 0);
}

TEST(a_stereo_stream_at_half_rate_is_interpolated_across_its_blocks) {
  Mixer mixer(44100, one_pool(1));
  REQUIRE(mixer.play(std::make_unique<Ramp>(5000, 2, 22050), PlayParams{}) == 0);
  std::vector<std::int16_t> out(2 * 3000);
  mixer.mix(out.data(), 3000);  // 1,500 source frames: past the first block
  // Frame 2k is source frame k, 2k+1 the midpoint, on both sides.
  CHECK(out[2 * 2400] == 1200 && out[2 * 2400 + 1] == -1200);
  CHECK(out[2 * 2401] == 1200 && out[2 * 2403] == 1201);
  mixer.mix(out.data(), 3000);
  CHECK(out[2 * 1000] == 2000);  // carried on from where it stopped
}

TEST(a_live_voice_takes_a_new_volume_and_pan) {
  Mixer mixer(44100, one_pool(2));
  REQUIRE(mixer.play(clip_of(std::vector<std::int16_t>(100, 1000)), PlayParams{}) == 0);
  CHECK(mixer.set_voice(0, -600, 10000));
  CHECK(mixer.voice_gains(0).left == 0 && mixer.voice_gains(0).right == 16423);
  std::vector<std::int16_t> out(4);
  mixer.mix(out.data(), 2);
  CHECK(out[0] == 0 && out[1] == 1000 * 16423 / 32768);
  CHECK(!mixer.set_voice(1, 0, 0));  // not playing
  CHECK(!mixer.set_voice(7, 0, 0));
}

TEST(a_live_sound_follows_the_view_at_the_slider_alone) {
  using imperivm::sound::follow;
  using imperivm::sound::Placed;
  imperivm::sound::View v = view_of_1000_by_800();
  // On screen: centred, at the slider.
  auto f = follow(v, Placed{true, 500, 400}, 68);
  CHECK(f.pans && f.pan == 0 && f.volume == -1920);
  // The view moved 1,300 to the right: the sound is 1,300 left of its
  // centre, 800 beyond 500 (-4000 mB), and 800 past the screen's left edge.
  v.left += 1300;
  v.right += 1300;
  v.screen_left += 1300;
  v.screen_right += 1300;
  f = follow(v, Placed{true, 500, 400}, 100);
  CHECK(f.volume == -4000 && f.pan == -8000);
  // The slider scales what is left above -60 dB.
  f = follow(v, Placed{true, 500, 400}, 50);
  CHECK(f.volume == (2000 * 50 / 100) - 6000);
  // Reversed speakers swap the side.
  v.reverse_speakers = true;
  CHECK(follow(v, Placed{true, 500, 400}, 100).pan == 8000);
  // No place: the slider's level and no pan at all.
  f = follow(v, Placed{}, 52);
  CHECK(!f.pans && f.volume == -2880);
  CHECK(imperivm::sound::kFollowedChannels == 64);
}

TEST(the_in_game_music_never_plays_a_track_twice_in_a_row) {
  using imperivm::sound::next_track;
  imperivm::sound::SoundRandom random(12345);
  CHECK(!next_track(0, std::nullopt, random).has_value());
  // One track: that one, again and again, with no draw.
  imperivm::sound::SoundRandom untouched(12345);
  CHECK(next_track(1, 0, untouched) == std::optional<std::size_t>(0));
  CHECK(untouched.next() == imperivm::sound::SoundRandom(12345).next());
  std::optional<std::size_t> last;
  std::vector<int> seen(7, 0);
  for (int i = 0; i < 5000; ++i) {
    const std::optional<std::size_t> pick = next_track(7, last, random);
    REQUIRE(pick.has_value() && *pick < 7);
    CHECK(pick != last);
    ++seen[*pick];
    last = pick;
  }
  for (const int n : seen) CHECK(n > 500);  // and every one of them comes round
  // Two tracks alternate.
  last = 0;
  for (int i = 0; i < 20; ++i) {
    last = next_track(2, last, random);
    CHECK(*last == static_cast<std::size_t>((i + 1) % 2));
  }
  CHECK(imperivm::sound::is_music_track("GBR_TRACK_1.ogg"));
  CHECK(!imperivm::sound::is_music_track("_menu.ogg"));
  CHECK(!imperivm::sound::is_music_track(".."));
}

// -- Ogg Vorbis ---------------------------------------------------------------
//
// A stream built bit by bit from the Vorbis I specification and the Ogg
// framing (RFC 3533), so the decoder is tested without a byte of the
// installation's music: one codebook of two one-bit codes for the residue's
// classes, one two-dimensional VQ book whose every vector is (k, k), floor 1
// with only its two end points, residue 1 over the first two coefficients,
// one mapping, one short-block mode. Each audio packet sets both floor ends
// to their top and the two coefficients to k: a little sound, every packet
// alike.

namespace {

/// Bits least significant first, as Vorbis packs them.
class BitWriter {
 public:
  void put(std::uint32_t value, int bits) {
    for (int i = 0; i < bits; ++i) {
      if (used_ % 8 == 0) bytes_.push_back(0);
      if ((value >> i) & 1u) bytes_.back() |= static_cast<std::uint8_t>(1u << (used_ % 8));
      ++used_;
    }
  }
  [[nodiscard]] Bytes take() { return std::move(bytes_); }

 private:
  Bytes bytes_;
  std::size_t used_ = 0;
};

std::uint32_t ogg_crc(const Bytes& page) {
  std::uint32_t crc = 0;
  for (const std::uint8_t byte : page) {
    crc ^= static_cast<std::uint32_t>(byte) << 24;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
  }
  return crc;
}

/// One Ogg page holding whole packets.
Bytes ogg_page(std::uint8_t flags, std::uint64_t granule, std::uint32_t sequence, const std::vector<Bytes>& packets) {
  Bytes lacing;
  Bytes body;
  for (const Bytes& p : packets) {
    std::size_t left = p.size();
    while (left >= 255) {
      lacing.push_back(255);
      left -= 255;
    }
    lacing.push_back(static_cast<std::uint8_t>(left));
    body.insert(body.end(), p.begin(), p.end());
  }
  Bytes page = {'O', 'g', 'g', 'S', 0, flags};
  put32(page, static_cast<std::uint32_t>(granule));
  put32(page, static_cast<std::uint32_t>(granule >> 32));
  put32(page, 0x51A11u);  // the stream's serial number
  put32(page, sequence);
  put32(page, 0);  // the CRC, filled in below
  page.push_back(static_cast<std::uint8_t>(lacing.size()));
  page.insert(page.end(), lacing.begin(), lacing.end());
  page.insert(page.end(), body.begin(), body.end());
  const std::uint32_t crc = ogg_crc(page);
  for (int i = 0; i < 4; ++i) page[22 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
  return page;
}

void vorbis_word(Bytes& b, std::uint8_t type) {
  b.push_back(type);
  b.insert(b.end(), {'v', 'o', 'r', 'b', 'i', 's'});
}

/// A Vorbis stream of `packets` audio packets of 256-sample blocks, each
/// playing coefficient value `k` (0: every channel's floor unused, silence),
/// ending at `granule` frames.
Bytes ogg_vorbis(std::uint8_t channels, std::uint32_t rate, int packets, std::uint64_t granule, std::uint32_t k) {
  Bytes id;
  vorbis_word(id, 1);
  put32(id, 0);
  id.push_back(channels);
  put32(id, rate);
  put32(id, 0);
  put32(id, 80000);
  put32(id, 0);
  id.push_back(0x88);  // both block sizes 2^8
  id.push_back(1);

  Bytes comment;
  vorbis_word(comment, 3);
  const std::string vendor = "imperivm-reforged sound_tests";
  put32(comment, static_cast<std::uint32_t>(vendor.size()));
  comment.insert(comment.end(), vendor.begin(), vendor.end());
  put32(comment, 0);
  comment.push_back(1);

  BitWriter s;
  s.put(1, 8);  // two codebooks
  const auto book = [&s](std::uint32_t dimensions) {
    s.put(0x564342, 24);
    s.put(dimensions, 16);
    s.put(2, 24);   // two entries
    s.put(0, 1);    // not ordered
    s.put(0, 1);    // not sparse
    s.put(0, 5);    // both one bit long
    s.put(0, 5);
  };
  book(1);
  s.put(0, 4);  // book 0: no values, the residue's class book
  book(2);
  s.put(1, 4);  // book 1: lookup type 1, one value shared by both dimensions
  s.put(0, 32);                              // minimum 0
  s.put((788u << 21) | std::max(k, 1u), 32);  // delta k
  s.put(0, 4);                               // one bit a value
  s.put(0, 1);                               // not a sequence
  s.put(1, 1);                               // the one multiplicand: 1
  s.put(0, 6);  // one time transform, 0
  s.put(0, 16);
  s.put(0, 6);   // one floor, type 1
  s.put(1, 16);
  s.put(0, 5);   // no partitions
  s.put(0, 2);   // multiplier 1
  s.put(7, 4);   // range bits: the far end at 128, half the block
  s.put(0, 6);   // one residue, type 1
  s.put(1, 16);
  s.put(0, 24);  // from coefficient 0
  s.put(2, 24);  // to 2
  s.put(1, 24);  // in partitions of 2
  s.put(0, 6);   // one classification
  s.put(0, 8);   // classified by book 0
  s.put(1, 3);   // its cascade: pass 0 only
  s.put(0, 1);
  s.put(1, 8);   // pass 0 by book 1
  s.put(0, 6);   // one mapping, type 0, one submap, no coupling
  s.put(0, 16);
  s.put(0, 1);
  s.put(0, 1);
  s.put(0, 2);
  s.put(0, 8);
  s.put(0, 8);   // floor 0
  s.put(0, 8);   // residue 0
  s.put(0, 6);   // one mode: short blocks, mapping 0
  s.put(0, 1);
  s.put(0, 16);
  s.put(0, 16);
  s.put(0, 8);
  s.put(1, 1);   // framing
  Bytes setup;
  vorbis_word(setup, 5);
  const Bytes bits = s.take();
  setup.insert(setup.end(), bits.begin(), bits.end());

  BitWriter a;
  a.put(0, 1);  // an audio packet; one mode, so no mode number
  for (std::uint8_t c = 0; c < channels; ++c) {
    a.put(k != 0 ? 1 : 0, 1);
    if (k != 0) {
      a.put(255, 8);  // the floor's two ends at the top
      a.put(255, 8);
    }
  }
  if (k != 0) {
    for (std::uint8_t c = 0; c < channels; ++c) a.put(0, 1);  // class 0
    for (std::uint8_t c = 0; c < channels; ++c) a.put(0, 1);  // vector 0: (k, k)
  }
  const Bytes audio = a.take();

  Bytes file = ogg_page(0x02, 0, 0, {id});
  const Bytes headers = ogg_page(0x00, 0, 1, {comment, setup});
  file.insert(file.end(), headers.begin(), headers.end());
  const Bytes body = ogg_page(0x04, granule, 2, std::vector<Bytes>(static_cast<std::size_t>(packets), audio));
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

std::vector<std::int16_t> drain(imperivm::sound::OggStream& stream, std::size_t piece) {
  std::vector<std::int16_t> all;
  std::vector<std::int16_t> buffer(piece * stream.channels());
  for (;;) {
    const std::size_t got = stream.read(buffer.data(), piece);
    if (got == 0) break;
    all.insert(all.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got * stream.channels()));
  }
  return all;
}

}  // namespace

TEST(an_ogg_vorbis_stream_decodes_piece_by_piece_to_its_last_granule) {
  using imperivm::sound::OggStream;
  // Twenty short blocks overlap into 19 * 128 frames; the last page's
  // granule cuts the end to 2,400.
  std::string why;
  auto stream = OggStream::open(ogg_vorbis(1, 22050, 20, 2400, 1000), &why);
  REQUIRE(stream != nullptr);
  CHECK(why.empty());
  CHECK(stream->rate() == 22050 && stream->channels() == 1);
  CHECK(stream->length() == 2400);
  const std::vector<std::int16_t> mono = drain(*stream, 100);
  CHECK(mono.size() == 2400);
  CHECK(stream->position() == 2400);
  CHECK(std::any_of(mono.begin(), mono.end(), [](std::int16_t s) { return s != 0; }));
  // Another copy read in one piece decodes to the same samples.
  auto again = OggStream::open(ogg_vorbis(1, 22050, 20, 2400, 1000));
  REQUIRE(again != nullptr);
  CHECK(drain(*again, 4096) == mono);
}

TEST(a_stereo_ogg_vorbis_stream_keeps_two_channels_and_silence_is_silent) {
  using imperivm::sound::OggStream;
  auto stream = OggStream::open(ogg_vorbis(2, 44100, 9, 8 * 128, 1000));
  REQUIRE(stream != nullptr);
  CHECK(stream->channels() == 2 && stream->rate() == 44100);
  const std::vector<std::int16_t> both = drain(*stream, 300);
  CHECK(both.size() == 2 * 8 * 128);
  // Both channels were coded alike.
  bool alike = true;
  for (std::size_t i = 0; i + 1 < both.size(); i += 2) alike = alike && both[i] == both[i + 1];
  CHECK(alike);
  auto quiet = OggStream::open(ogg_vorbis(2, 44100, 9, 8 * 128, 0));
  REQUIRE(quiet != nullptr);
  const std::vector<std::int16_t> silence = drain(*quiet, 300);
  CHECK(silence.size() == 2 * 8 * 128);
  CHECK(std::all_of(silence.begin(), silence.end(), [](std::int16_t s) { return s == 0; }));
}

TEST(what_is_not_ogg_vorbis_is_refused_with_a_reason) {
  using imperivm::sound::OggStream;
  std::string why;
  CHECK(OggStream::open({}, &why) == nullptr && !why.empty());
  why.clear();
  Bytes riff_bytes = riff({{"fmt ", pcm_fmt(1, 44100, 16)}, {"data", Bytes(4, 0)}});
  CHECK(OggStream::open(riff_bytes, &why) == nullptr && !why.empty());
  // A good stream cut short in its headers.
  Bytes cut = ogg_vorbis(1, 44100, 4, 3 * 128, 1);
  cut.resize(60);
  CHECK(OggStream::open(cut) == nullptr);
}

TEST(an_ogg_stream_plays_through_the_mixer_and_frees_its_voice) {
  Mixer mixer(44100, one_pool(1, true));
  auto stream = imperivm::sound::OggStream::open(ogg_vorbis(1, 44100, 20, 2400, 1000));
  REQUIRE(stream != nullptr);
  REQUIRE(mixer.play(std::move(stream), PlayParams{}) == 0);
  std::vector<std::int16_t> out(2 * 512);
  std::size_t mixed = 0;
  bool heard = false;
  while (mixer.busy(0) && mixed < 10000) {
    mixer.mix(out.data(), 512);
    heard = heard || std::any_of(out.begin(), out.end(), [](std::int16_t s) { return s != 0; });
    mixed += 512;
  }
  CHECK(heard);
  CHECK(!mixer.busy(0) && mixed == 2560);
}

/// `sound_tests --decode FILE`: an installation's Ogg Vorbis, decoded a
/// block at a time as the mixer would, with what a corpus test asserts on
/// (`tests/test_corpus_app_sound.py`). The file is read where it lies and
/// nothing is written.
int decode_file(const char* path) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    std::printf("cannot open %s\n", path);
    return 2;
  }
  Bytes bytes;
  std::uint8_t chunk_bytes[65536];
  std::size_t n = 0;
  while ((n = std::fread(chunk_bytes, 1, sizeof chunk_bytes, file)) > 0) bytes.insert(bytes.end(), chunk_bytes, chunk_bytes + n);
  std::fclose(file);
  std::string why;
  auto stream = imperivm::sound::OggStream::open(std::move(bytes), &why);
  if (stream == nullptr) {
    std::printf("not decoded: %s\n", why.c_str());
    return 1;
  }
  std::vector<std::int16_t> block(2048 * stream->channels());
  std::uint64_t frames = 0;
  std::uint64_t reads = 0;
  std::int32_t peak = 0;
  for (;;) {
    const std::size_t got = stream->read(block.data(), 2048);
    if (got == 0) break;
    ++reads;
    frames += got;
    for (std::size_t i = 0; i < got * stream->channels(); ++i) peak = std::max(peak, std::abs(static_cast<std::int32_t>(block[i])));
  }
  std::printf("decoded: rate %u channels %u length %llu frames %llu reads %llu peak %d\n", stream->rate(),
              stream->channels(), static_cast<unsigned long long>(stream->length()),
              static_cast<unsigned long long>(frames), static_cast<unsigned long long>(reads), peak);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--decode") == 0) return decode_file(argv[2]);
  imperivm::test::run_all(argc > 1);
  std::printf("%d checks, %d failures\n", imperivm::test::checks, imperivm::test::failures);
  return imperivm::test::failures == 0 ? 0 : 1;
}
