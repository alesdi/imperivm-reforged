#pragma once

/// A freestanding INI reader for the engine's data files.
///
/// 150 `.ini` files ship in the retail packs. They carry the entire user
/// interface (Part 6), the AI profiles (`DATA/AI/AI.INI`), and the tuning
/// constants (`CONST.INI`). The core has to read them and the core may not take
/// a dependency, so we parse them ourselves — the same reasoning as
/// `imperivm/core/xml.hpp`, and the same two structural choices: **zero copy**
/// (views into the caller's buffer, which must outlive the document) and **flat
/// storage** (one vector, indices rather than pointers).
///
/// ## The dialect, measured rather than assumed
///
/// Across all 150 files: 3,662 section headers, 18,094 `key = value` lines,
/// **3,278 bare lines**, 672 whole-line comments and 408 inline ones. Nothing
/// else occurs. `docs/formats/ini.md` has the full table.
///
/// * `; comment` runs to end of line, and may follow a value **or a section
///   header** — 12 headers in the corpus carry a trailing comment, all in the
///   editor's `TEMPLATE.INI` and `ADVOBJPROPS.INI`.
/// * **No value in any shipped file contains a `;`.** That is the one
///   assumption the lexer rests on, and it is why `;` starts a comment
///   unconditionally with no quoting rule to respect. Only three values are
///   quoted at all — `ImageType = "AAAAA"`, in three editor dialogs — the
///   quotes are part of the value, and none of the three shares a line with a
///   `;`.
/// * `#` and `//` never begin a comment in any shipped file. They are not
///   treated as one; a file that needs them is a finding, not a reason to grow
///   the parser quietly.
/// * Whitespace around keys, values and headers is insignificant and stripped.
///
/// ## A section can be a list, and is not simply a map
///
/// 3,278 lines carry no `=` at all, and they are not malformed. `AI.INI`'s
/// `[SquadStates]` is an **ordered enum declaration** whose line numbers are
/// the constant values, and the interface files' `[<Screen> Objects]` sections
/// name their widgets the same way. So an entry is a key and a value of which
/// **either may be empty**, and order is preserved: for these sections the
/// index within the section is the meaning.
///
/// Even a keyed section is not reliably a map. 22 keys in the corpus are
/// declared twice within their own section and nine carry different values;
/// `UNITICONS.INI`'s `[FillCombo]` gives the label `Republican Roman Hero 1` to
/// three consecutive entries naming three different bitmaps, so reading that
/// section as a map loses two thirds of it. **Every entry is preserved**, and
/// `value()` is a convenience for the sections that really are maps.
///
/// ## Placeholders are somebody else's problem
///
/// 4,329 values in 132 of the 150 files contain `%Name%` placeholders, 179
/// distinct names, led by `%TmplIni%` at 2,367 uses. This reader does **not**
/// expand them: substitution needs a scope the file alone does not define, and
/// defining it is interface-layer work. Values come back with their
/// placeholders intact.
///
/// ## Encoding
///
/// The files are Windows-1252. This reader is byte-oriented and does not
/// transcode: it takes a byte span and hands back views over it. Deciding what
/// a byte above 0x7F means belongs to whoever displays it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core {

using SectionIndex = std::uint32_t;

inline constexpr SectionIndex kNoSection = 0xFFFFFFFFu;

/// One line of content.
///
/// `key` is empty for a bare line and `value` is empty for `key =` with nothing
/// after it. The two are distinguishable: `has_key` records whether an `=` was
/// present, because `[SquadStates]`'s bare `SS_Approach` and a hypothetical
/// `SS_Approach =` mean different things and only the file can say which it is.
struct IniEntry {
  std::string_view key;
  std::string_view value;
  bool has_key = false;
  /// One-based, for diagnostics that a human has to act on.
  std::uint32_t line = 0;
};

/// One `[Section]`.
struct IniSection {
  /// The text between the brackets, trimmed. Empty for the implicit section
  /// that holds entries appearing before any header — which no shipped file
  /// has, but a hand-edited one might.
  std::string_view name;
  std::uint32_t entry_begin = 0;  ///< index into `IniDocument::entries()`
  std::uint32_t entry_count = 0;
  std::uint32_t line = 0;
};

/// A parsed file.
class IniDocument {
 public:
  /// Parse `bytes`. The document holds views into it and must not outlive it.
  ///
  /// The only rejection is an unterminated section header: a `[` with no `]`
  /// on the same line. Everything else the shipped corpus contains is accepted,
  /// and a line that is neither a header nor a comment becomes an entry.
  [[nodiscard]] static Result<IniDocument> parse(std::span<const std::byte> bytes);

  [[nodiscard]] const std::vector<IniSection>& sections() const noexcept { return sections_; }
  [[nodiscard]] const std::vector<IniEntry>& entries() const noexcept { return entries_; }

  /// The first section with this name, or `kNoSection`. Comparison is
  /// case-insensitive: `CONST.INI` and the interface files disagree about the
  /// capitalisation of the same section between the file and the code that
  /// reads it.
  [[nodiscard]] SectionIndex section(std::string_view name) const noexcept;

  /// The entries of one section, in file order. Empty for `kNoSection`.
  [[nodiscard]] std::span<const IniEntry> entries_of(SectionIndex index) const noexcept;

  /// The value of `key` in `section`, or `fallback`.
  ///
  /// Keys compare case-insensitively, for the same reason section names do.
  /// Only entries with an `=` are considered, so a bare line never answers a
  /// lookup by key.
  ///
  /// The **first** match wins, which is what Win32's `GetPrivateProfileString`
  /// does; see the header note on duplicate keys. **Inferred from the platform,
  /// not proven from the data.** A caller reading a list-shaped section should
  /// walk `entries_of` instead, or it will silently see one of several.
  [[nodiscard]] std::string_view value(SectionIndex section, std::string_view key,
                                       std::string_view fallback = {}) const noexcept;

  /// The value of `key` parsed as a signed decimal, or `fallback`.
  ///
  /// **Returns `fallback` for a value that is not entirely a number**, rather
  /// than the prefix it could parse. A value read half-way is how
  /// `ProductionInterval` came to be recorded as 20 when it is 2000, and that
  /// error propagated through a whole session's work. See `docs/plan.html`.
  [[nodiscard]] std::int32_t value_int(SectionIndex section, std::string_view key,
                                       std::int32_t fallback = 0) const noexcept;

 private:
  std::vector<IniSection> sections_;
  std::vector<IniEntry> entries_;
};

/// Split a comma-separated value. 8,321 lines in the corpus carry one.
///
/// Elements are trimmed; an empty element is preserved, because a positional
/// list with a hole means something different from a shorter list.
[[nodiscard]] std::vector<std::string_view> split_list(std::string_view value);

/// Parse an entire value as a signed decimal.
///
/// `false` when the text is empty, or has anything but digits after an optional
/// sign, or overflows. Trailing text is a failure and not a truncation.
[[nodiscard]] bool parse_int(std::string_view text, std::int32_t& out) noexcept;

/// Trim ASCII whitespace from both ends.
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;

}  // namespace imperivm::core
