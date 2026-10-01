// The AI profile: DATA/AI/AI.INI and the constants it declares.
// See include/imperivm/core/sim/ai_profile.hpp.

#include "imperivm/core/sim/ai_profile.hpp"

namespace imperivm::core::sim {
namespace {

constexpr std::size_t kFamilyCount = static_cast<std::size_t>(AiEnum::count);

[[nodiscard]] constexpr char lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

/// The `[Vars.<name>]` section for each overlay. `[Vars.All]` is the base and
/// is stored at `AiDifficulty::none`.
[[nodiscard]] std::string_view vars_section(AiDifficulty difficulty) noexcept {
  switch (difficulty) {
    case AiDifficulty::easy: return "Vars.Easy";
    case AiDifficulty::normal: return "Vars.Normal";
    case AiDifficulty::hard: return "Vars.Hard";
    case AiDifficulty::none: break;
  }
  return "Vars.All";
}

}  // namespace

std::string_view ai_enum_sentinel(AiEnum family) noexcept {
  switch (family) {
    case AiEnum::squad_state: return "SS_IDLE";
    case AiEnum::gaika_strategy: return "GS_NONE";
    case AiEnum::economy_script: return "ES_NONE";
    case AiEnum::tactic_script: return "TS_NONE";
    case AiEnum::count: break;
  }
  return {};
}

std::string_view ai_enum_section(AiEnum family) noexcept {
  switch (family) {
    case AiEnum::squad_state: return "SquadStates";
    case AiEnum::gaika_strategy: return "GAIKAStrat";
    case AiEnum::economy_script: return "EconomyScripts";
    case AiEnum::tactic_script: return "TacticScripts";
    case AiEnum::count: break;
  }
  return {};
}

Result<AiProfile> AiProfile::parse(std::span<const std::byte> ini, const AiProfile* parent) {
  const Result<IniDocument> document = IniDocument::parse(ini);
  if (!document.ok()) return document.error();
  const IniDocument& file = document.value();

  AiProfile profile;

  for (std::size_t f = 0; f < kFamilyCount; ++f) {
    const AiEnum family = static_cast<AiEnum>(f);
    std::vector<std::string>& names = profile.families_[f];
    // Index 0 is the engine-defined sentinel, which appears in no section.
    names.emplace_back(ai_enum_sentinel(family));

    const SectionIndex section = file.section(ai_enum_section(family));
    for (const IniEntry& entry : file.entries_of(section)) {
      // These sections are ordered lists, not key/value maps: a line's position
      // is its constant's value. A line that somehow carries an `=` is not one
      // of them, so it is skipped rather than half-interpreted.
      if (entry.has_key || entry.value.empty()) continue;
      names.emplace_back(entry.value);
    }

    // An overlay that declares nothing for a family inherits the parent's
    // table wholesale. `DEFENSIVE` has an empty `[TacticScripts]`, and whether
    // the original reads that as "inherit" or as "none" is not something the
    // corpus settles -- see the header. Inheriting is the reading that keeps a
    // shared script's `TS_` constants resolvable under every profile, which is
    // the behaviour a shipped map needs, since `GETTACTICSCRIPT.VS` lives in
    // the shared directory and reads all fourteen of them.
    if (names.size() == 1 && parent != nullptr) names = parent->families_[f];
  }

  const SectionIndex scripts = file.section("Scripts");
  for (const IniEntry& entry : file.entries_of(scripts)) {
    if (!entry.has_key) continue;
    profile.scripts_.push_back(
        AiScriptDeclaration{std::string(entry.key), std::string(entry.value)});
  }
  // An overlay declares only what it replaces, so the parent's declarations
  // come along unless the overlay names the same file.
  if (parent != nullptr) {
    for (const AiScriptDeclaration& inherited : parent->scripts_) {
      bool overridden = false;
      for (const AiScriptDeclaration& own : profile.scripts_) {
        if (equal_ignoring_case(own.file, inherited.file)) {
          overridden = true;
          break;
        }
      }
      if (!overridden) profile.scripts_.push_back(inherited);
    }
  }

  for (std::uint8_t d = 0; d < 4; ++d) {
    const AiDifficulty difficulty = static_cast<AiDifficulty>(d);
    std::vector<AiVariable>& into = profile.variables_[d];
    if (parent != nullptr) into = parent->variables_[d];

    // Two merges, and they are not the same rule.
    //
    // **Within one section, the first declaration wins.** `[Vars.All]` declares
    // `AIV_SquanderGoldAmount` twice, at lines 150 and 159, with different
    // values -- 30000 then 15000 -- and the second block reads as a
    // copy-and-paste of the first with "for research" appended to every
    // comment. Something has to break the tie. Win32's
    // `GetPrivateProfileString`, which is what a game of this vintage reads
    // `.ini` files with, returns the first match in a section, and
    // `IniDocument::value` reproduces that. **Inferred**, but from the platform
    // the engine was built on rather than from taste.
    //
    // **Across sections, the later one wins**: `[Vars.Hard]` is an overlay on
    // `[Vars.All]`, and an overlay profile is an overlay on its parent.
    std::vector<AiVariable> own;
    const SectionIndex section = file.section(vars_section(difficulty));
    for (const IniEntry& entry : file.entries_of(section)) {
      if (!entry.has_key) continue;
      std::int32_t value = 0;
      // A value that is not entirely a number is dropped rather than truncated.
      // `parse_int` refuses a prefix on purpose: reading one half-way is how
      // `ProductionInterval` came to be recorded as 20 when it is 2000.
      if (!parse_int(entry.value, value)) continue;

      bool seen = false;
      for (const AiVariable& existing : own) {
        if (equal_ignoring_case(existing.name, entry.key)) {
          seen = true;
          break;
        }
      }
      if (!seen) own.push_back(AiVariable{std::string(entry.key), value});
    }

    for (const AiVariable& variable : own) {
      bool replaced = false;
      for (AiVariable& existing : into) {
        if (equal_ignoring_case(existing.name, variable.name)) {
          existing.value = variable.value;
          replaced = true;
          break;
        }
      }
      if (!replaced) into.push_back(variable);
    }
  }

  return profile;
}

std::int32_t AiProfile::constant(AiEnum family, std::string_view name) const noexcept {
  if (family == AiEnum::count) return kUnknownAiConstant;
  const std::vector<std::string>& names = families_[static_cast<std::size_t>(family)];
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (equal_ignoring_case(names[i], name)) return static_cast<std::int32_t>(i);
  }
  return kUnknownAiConstant;
}

std::string_view AiProfile::constant_name(AiEnum family, std::int32_t value) const noexcept {
  if (family == AiEnum::count || value < 0) return {};
  const std::vector<std::string>& names = families_[static_cast<std::size_t>(family)];
  const std::size_t index = static_cast<std::size_t>(value);
  return index < names.size() ? std::string_view(names[index]) : std::string_view();
}

std::span<const std::string> AiProfile::names(AiEnum family) const noexcept {
  if (family == AiEnum::count) return {};
  return families_[static_cast<std::size_t>(family)];
}

std::string_view AiProfile::script_signature(std::string_view file) const noexcept {
  for (const AiScriptDeclaration& declaration : scripts_) {
    if (equal_ignoring_case(declaration.file, file)) return declaration.signature;
  }
  return {};
}

std::int32_t AiProfile::variable(std::string_view name, AiDifficulty difficulty,
                                 std::int32_t fallback) const noexcept {
  // The overlay first, then the base. `[Vars.All]` is the base and every
  // difficulty section is a partial override of it.
  const std::uint8_t index = static_cast<std::uint8_t>(difficulty);
  if (index != 0 && index < 4) {
    for (const AiVariable& variable : variables_[index]) {
      if (equal_ignoring_case(variable.name, name)) return variable.value;
    }
  }
  for (const AiVariable& variable : variables_[0]) {
    if (equal_ignoring_case(variable.name, name)) return variable.value;
  }
  return fallback;
}

std::vector<AiVariable> AiProfile::variables(AiDifficulty difficulty) const {
  // Base order preserved, overlay values applied in place, and anything the
  // overlay adds appended. Declaration order is the iteration order everything
  // downstream sees, and iteration order is state.
  std::vector<AiVariable> out = variables_[0];
  const std::uint8_t index = static_cast<std::uint8_t>(difficulty);
  if (index == 0 || index >= 4) return out;
  for (const AiVariable& override_variable : variables_[index]) {
    bool replaced = false;
    for (AiVariable& existing : out) {
      if (equal_ignoring_case(existing.name, override_variable.name)) {
        existing.value = override_variable.value;
        replaced = true;
        break;
      }
    }
    if (!replaced) out.push_back(override_variable);
  }
  return out;
}

}  // namespace imperivm::core::sim
