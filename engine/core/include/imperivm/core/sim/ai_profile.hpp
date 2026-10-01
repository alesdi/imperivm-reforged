#pragma once

/// The AI profile: `DATA/AI/AI.INI` and the constants it declares.
///
/// This file is where a large part of the script VM's global namespace comes
/// from. `docs/formats/vs-host-api.md` counts 245 bare identifiers the scripts
/// read as globals, and four whole prefix families of them -- `SS_` (212 uses),
/// `GS_` (28), `ES_` (10), `TS_` (18) -- are **declared here as data**, not
/// compiled into the engine.
///
/// ## The closure that proves it
///
/// For squad states, GAIKA strategies and economy scripts, the set of names the
/// 577 shipped scripts use is **exactly** the set `AI.INI` declares plus one
/// sentinel, with nothing left over on either side:
///
/// | Family | Declared in `AI.INI` | Used by scripts | Sentinel |
/// |---|---:|---:|---|
/// | `SS_` | 13 | 14 | `SS_IDLE` |
/// | `GS_` |  5 |  6 | `GS_NONE` |
/// | `ES_` |  7 |  8 | `ES_NONE` |
///
/// (`SS_STR` is excluded from the count: it is a host function, `str, int
/// state`, not a constant.) The four sentinels `SS_IDLE`, `GS_NONE`, `ES_NONE`
/// and `TS_NONE` are the **only** names of these families that appear as
/// strings in `gbr.exe`; not one of the names `AI.INI` declares does. So the
/// engine hardcodes the sentinels and reads the rest.
///
/// ## What the numbering is, and what is inferred about it
///
/// A constant's value is its position in its section, the sentinel is `0`, and
/// the declared entries run from `1`. All three are **proven**: `gbr.exe`
/// registers `SS_IDLE` with the literal value 0, and `0x004435cb` registers the
/// enum set with a running index starting at 1.
///
/// **The order is not retail's.** The engine parses these four sections from
/// *every* `data/ai/*/AI.INI` into one `std::map` keyed by name and numbers the
/// merged set, so a retail id is a name's alphabetical rank across all profiles
/// rather than its line number in one file. This reader numbers by declaration
/// order within a profile, which is a deliberate divergence: both id spaces are
/// internal, so reader and writer need only agree with each other, and the
/// difference becomes visible only when comparing against a retail desync dump.
/// `docs/formats/ai-ini.md` has the addresses.
///
/// ## Profiles are overlays, and two of the three ship empty
///
/// `playerdata/@AI` names a subdirectory of `DATA/AI`. Three exist:
///
///   * `DEFENSIVE` -- a real overlay: its own `AI.INI` plus three `.vs` files
///     that replace the parent's `EvalRecruit`, `GAIKAMonitor` and `Main`.
///   * `DEFAULT` and `CHAOTIC` -- **`DUMMY.TXT` and nothing else.**
///
/// `3_Great_Losses_Egypt.bfhp` gives player 3 the `CHAOTIC` profile, so a
/// shipped map exercises the fallback to `data/ai/ai.ini`; `gbr.exe` carries
/// both `/ai.ini` and `data/ai/ai.ini` as separate strings, which is what a
/// profile path and a default path look like.
///
/// `DEFENSIVE`'s `[TacticScripts]` section is present and **empty**, while its
/// `[SquadStates]`, `[GAIKAStrat]` and `[EconomyScripts]` are byte-identical to
/// the parent's. Whether an empty section in an overlay means "inherit" or
/// "none" is **unknown**; the only script that reads a `TS_` constant,
/// `GETTACTICSCRIPT.VS`, lives in the shared directory rather than in either
/// profile, so the corpus does not settle it. See the note on `tactic_script`.
///
/// ## AiDifficulty
///
/// `[Vars.All]` holds the base `AIV_*` values and `[Vars.Easy]`,
/// `[Vars.Normal]` and `[Vars.Hard]` overlay them. `gbr.exe` builds the section
/// name by concatenating `Vars.` with one of `Easy`, `Normal`, `Hard`, so the
/// three are selected by index. `playerdata/@difficulty` takes the values 0
/// (76 rows), 2 (1 row) and 3 (195 rows) across the shipped containers, and is
/// absent from 16. Three overlays and a value that reaches 3 means the
/// attribute cannot be a direct 0-based index; `1..3` selecting Easy, Normal
/// and Hard with `0` selecting none is consistent with every row observed and
/// is what `ai_difficulty_overlay` implements. **Inferred.**

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::sim {

/// The value a name of one of the enum families does not have.
inline constexpr std::int32_t kUnknownAiConstant = -1;

/// The four families `AI.INI` declares.
enum class AiEnum : std::uint8_t {
  squad_state,     ///< `[SquadStates]`, sentinel `SS_IDLE`
  gaika_strategy,  ///< `[GAIKAStrat]`, sentinel `GS_NONE`
  economy_script,  ///< `[EconomyScripts]`, sentinel `ES_NONE`
  tactic_script,   ///< `[TacticScripts]`, sentinel `TS_NONE`
  count,
};

/// The overlay a difficulty selects.
///
/// **Not the same enum as `sim/combat.hpp`'s `Difficulty`**, and the difference
/// is a real open question rather than a naming accident. Combat models three
/// levels, because `CONST.INI` declares exactly three addends --
/// `EasyDifficultyLevelAddend`, `NormalDifficultyLevelAddend`,
/// `HardDifficultyLevelAddend`. This models four, because `[Vars.All]` is a
/// base that all three overlay and `playerdata/@difficulty` takes the value 0
/// on 76 of the 304 shipped rows.
///
/// One of the two is wrong, or the game genuinely has a base setting that is
/// not a difficulty. Until something settles it, the two stay separate: merging
/// them would pick an answer silently.
enum class AiDifficulty : std::uint8_t { none = 0, easy = 1, normal = 2, hard = 3 };

/// `playerdata/@difficulty` as an overlay. See the header note: inferred.
[[nodiscard]] constexpr AiDifficulty ai_difficulty_overlay(std::int32_t attribute) noexcept {
  switch (attribute) {
    case 1: return AiDifficulty::easy;
    case 2: return AiDifficulty::normal;
    case 3: return AiDifficulty::hard;
    default: return AiDifficulty::none;
  }
}

/// One `.vs` entry point the profile declares, with the signature `AI.INI`
/// gives it: `GetArmyNeed.vs = int, GAIKA g, int idPlayer, bool bMin, ...`.
///
/// The same signature appears as a leading `//` comment inside the `.vs` file
/// itself, so the two are cross-checkable and a disagreement between them is a
/// finding worth reporting rather than a tie to break silently.
struct AiScriptDeclaration {
  std::string file;       ///< as written, e.g. `GetArmyNeed.vs`
  std::string signature;  ///< as written, return type first
};

/// One `AIV_*` value.
struct AiVariable {
  std::string name;
  std::int32_t value = 0;
};

/// A parsed `AI.INI`.
///
/// Owns its strings rather than viewing the input: a profile outlives the byte
/// buffer it was read from, and an overlay merges two buffers' worth of names.
class AiProfile {
 public:
  /// Parse one `AI.INI`.
  ///
  /// `parent` is the profile this one overlays, or null for `data/ai/ai.ini`
  /// itself. Names the overlay does not declare are inherited; names it does
  /// declare replace the parent's **value**, not its position, so a constant
  /// keeps its meaning across profiles wherever both declare it.
  [[nodiscard]] static Result<AiProfile> parse(std::span<const std::byte> ini,
                                               const AiProfile* parent = nullptr);

  /// The value of a constant, or `kUnknownAiConstant`.
  ///
  /// Accepts the sentinel names (`SS_IDLE` and the rest), which are engine
  /// defined and appear in no section.
  [[nodiscard]] std::int32_t constant(AiEnum family, std::string_view name) const noexcept;

  /// The name a value has, or empty. The inverse of `constant`, for the
  /// engine's own `SS_STR` and `GS_STR`.
  [[nodiscard]] std::string_view constant_name(AiEnum family,
                                               std::int32_t value) const noexcept;

  /// Every name in a family, in declaration order, sentinel first.
  [[nodiscard]] std::span<const std::string> names(AiEnum family) const noexcept;

  [[nodiscard]] std::span<const AiScriptDeclaration> scripts() const noexcept {
    return scripts_;
  }
  /// The declared signature of one `.vs` file, or empty.
  [[nodiscard]] std::string_view script_signature(std::string_view file) const noexcept;

  /// An `AIV_*` value with the difficulty overlay applied, or `fallback`.
  [[nodiscard]] std::int32_t variable(std::string_view name, AiDifficulty difficulty,
                                      std::int32_t fallback = 0) const noexcept;
  /// Every `AIV_*` the profile declares at `difficulty`, in declaration order.
  [[nodiscard]] std::vector<AiVariable> variables(AiDifficulty difficulty) const;

 private:
  /// `[0]` is the sentinel; declared entries follow in file order.
  std::vector<std::string> families_[static_cast<std::size_t>(AiEnum::count)];
  std::vector<AiScriptDeclaration> scripts_;
  /// Indexed by `AiDifficulty`; `none` holds `[Vars.All]`.
  std::vector<AiVariable> variables_[4];
};

/// The engine-defined sentinel of a family: `SS_IDLE`, `GS_NONE`, `ES_NONE`,
/// `TS_NONE`. These are the only names of their families that appear as
/// strings in `gbr.exe`.
[[nodiscard]] std::string_view ai_enum_sentinel(AiEnum family) noexcept;

/// The `AI.INI` section a family is declared in.
[[nodiscard]] std::string_view ai_enum_section(AiEnum family) noexcept;

}  // namespace imperivm::core::sim
