#pragma once

/// The translation table, and the substitution the scripts do with it.
///
/// `Translate` is the eighth most called free function in the corpus (142
/// sites) and `Translatef` adds 76 more. Both bottom out in one file:
/// `CURRENTLANG\TRANSLATION.LOC.XML` inside the language pack, 3,887 entries.
///
/// ## The table
///
/// ```xml
/// <translationtableentry text="Requires %s1" justtext="Requires %s1"
///                        context="" result="Richiede %s1" comment=""/>
/// ```
///
/// **`text` is the key**, and it already carries any disambiguating suffix:
/// where two source strings collide, the key is `justtext@context`
/// (`Repair@repair arena`). 2,744 of the 3,887 entries carry a context, and 473
/// more carry an `@` suffix in `text` while leaving the `context` attribute
/// empty -- so the suffix belongs to the key, not to the attribute, and a
/// lookup must use `text` verbatim rather than reassembling it.
///
/// Measured over the shipped table: **no key occurs twice, no `result` is
/// empty, and every `result` carries exactly the placeholders its key does.**
/// The last of those is what makes `format` safe to run on a translation.
///
/// ## Substitution
///
/// Placeholders are positional: `%s1` through `%s3` and `%d1` through `%d4`
/// occur, 472 uses in all. They are **not** type directives --
/// `Translatef("You lose after %s1:0%s2 minutes", nTimeout/60, nTimeout%60)`
/// passes integers to `%s`, and the `0` before `%s2` is a literal the author
/// typed because the language has no field width. So `%sN` and `%dN` are the
/// same thing: substitute argument `N`.
///
/// ## This must never reach hashed state
///
/// A translated string depends on which language pack is installed. Comparing
/// one, or folding one into a hash, would desynchronise a multiplayer game
/// between players running different locales -- the exact class of bug the
/// determinism rules in `sim/system.hpp` exist to prevent. Translation output
/// is for display and for the debug log, and for nothing else.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::game {

/// One row of the table.
struct Translation {
  /// The lookup key, verbatim from `@text`, `@` suffix included.
  std::string key;
  /// The translated text, from `@result`.
  std::string result;
};

/// UTF-8 into cp1252, the encoding the fonts index and the scripts write.
/// The language packs' `.loc.xml` files are UTF-8 -- `Difficoltà` is three
/// bytes there -- and a result is converted once, at parse, so that every
/// consumer sees the game's own encoding. A code point cp1252 has no slot
/// for becomes `?`; bytes that are not valid UTF-8 are taken as cp1252
/// already and kept.
[[nodiscard]] std::string cp1252_from_utf8(std::string_view text);

/// The parsed `.loc.xml`. Results are cp1252; see `cp1252_from_utf8`.
class TranslationTable {
 public:
  /// Parse a `<translationtable>` document.
  ///
  /// Entries with an empty `text` are dropped: the key is what a lookup uses,
  /// and an entry nothing can name is not a translation.
  [[nodiscard]] static Result<TranslationTable> parse(std::span<const std::byte> xml);

  /// The translation of `key`, or `key` itself.
  ///
  /// Falling back to the source string is what makes an incomplete table
  /// survivable, and it is what the original does: it ships a
  /// `game.loc.xml` alongside the real table that accumulates the strings it
  /// was asked for, which is only useful if a miss is non-fatal.
  [[nodiscard]] std::string_view translate(std::string_view key) const noexcept;

  /// Whether `key` is in the table, for a caller that wants to know rather than
  /// to fall back.
  [[nodiscard]] bool contains(std::string_view key) const noexcept;

  /// The translation of an interface string. The shipped table keys a
  /// widget's text by where it stands: `Cancel@/Menu/selectmap.ini:CancelBtn:Text`
  /// -- the text, then `@`, then the file under `DATA/INTERFACE/`, the
  /// section and the key -- and spells the file the way whichever tool wrote
  /// the entry did (`/Menu/AdventureMenu.ini` beside `/Menu/gamemenu.ini`).
  /// So the contextual key is matched **case-blind**, through a folded index
  /// built at parse; a miss falls back to `translate(text)`, and that to the
  /// text itself. `context` is `/Menu/selectmap.ini:CancelBtn:Text`.
  [[nodiscard]] std::string_view translate_in_context(std::string_view text,
                                                      std::string_view context) const;

  /// Add `other`'s entries whose keys this table does not have. A container
  /// ships its own tables under `Local/<language>/` -- `adventure.loc.xml`
  /// with its name, its description and its territories', `Maps/<n>/notes.xml`
  /// with the notes' titles and texts, the conversations' phrases -- all
  /// keyed by the English source text, and a session's table is the
  /// language pack's plus the container's. Returns how many were added.
  std::size_t merge(const TranslationTable& other);

  /// What a script's `Translate(key)` answers: the original's table lookup,
  /// 0x00689130, reached from `Translate` at 0x004c9b70.
  ///
  /// * An empty key answers the empty string (0x0068915b).
  /// * A key made only of decimal digits answers itself, untranslated
  ///   (0x00689183..0x00689252, `isdigit` over every byte). That is what lets
  ///   `Translatef` pass a number through a translated `%s`.
  /// * Otherwise up to four probes, and the first that finds its name decides:
  ///   `key@context` exactly, then folded to lower case (0x00689030, `tolower`
  ///   byte by byte); then `key` exactly, then folded (0x006893a2). `context`
  ///   is empty for `Translatef`; for `Translate` it is the running script's
  ///   file name (0x006a3a60), which no shipped key carries and which
  ///   `sim/text.cpp` does not pass.
  /// * A name filed with an empty result answers `key` without probing
  ///   further (0x006892f4, 0x00689429), and so does a miss (0x006894bc),
  ///   after noting it (0x0068adc0, the `game.loc.xml` log, not reproduced).
  ///
  /// The two maps the probes read are not `entries()`. The loader, 0x00689f40,
  /// files every row under its `text` **and**, when it has one, its
  /// `justtext`, each in the exact map and folded in the second, by plain
  /// assignment: a later row in the file overwrites an earlier one under the
  /// same name. So `Translate("Mend")` finds the row whose `justtext` is
  /// `Mend` even though its key is `Mend@mend arena`, and where two rows
  /// share a name the one further down the file answers. `entries()` keeps
  /// only `text`, and keeps the first.
  ///
  /// The original keeps the map's own table apart and asks it only when the
  /// language pack's answer was the key itself (0x004c9cfc). `merge` folds the
  /// two together instead, adding the other table's names this one lacks;
  /// that differs only where the language pack answers a name with the name
  /// itself, or holds it under a different case.
  [[nodiscard]] std::string lookup(std::string_view key, std::string_view context = {}) const;

  /// What a script's `Translatef(key, a1, ...)` answers: 0x006897a0, reached
  /// through 0x00689c90 from the three `Translatef` bodies (0x004c9ea0,
  /// 0x004ca150, 0x004ca470).
  ///
  /// The pattern is `lookup(key)` with no context. It is copied byte by byte,
  /// except:
  ///
  /// * `%%` writes one `%`;
  /// * `%s`, `%S`, `%d` or `%D` followed by a decimal number `N` (any number
  ///   of digits, 0x00688ff0) writes argument `N`, counted from 1. Under `%s`
  ///   **the argument is itself translated**: it goes through `lookup` with
  ///   no context before it is written (0x00689a82, then `%s` at 0x00689aa5),
  ///   so `Translatef("Needs %s1", "Mend")` translates both. A number argument
  ///   arrives as its digits and the digit rule passes it through.
  ///   Under `%d` the original prints its argument with `%d` (0x006899d3), and every
  ///   `Translatef` form passes strings, so it prints a pointer; the
  ///   argument's own text is written untranslated here instead (inferred:
  ///   the original's answer is not a value). An `N` beyond the arguments
  ///   reads past them in the original; it is left as written here.
  /// * `%` followed by anything else, or a placeholder letter with no number
  ///   or with the number 0, abandons the formatting: the whole answer is
  ///   `Bad parameter at pos P`, `P` being the offset in the pattern just past
  ///   what was read (no number: 0x00689c07; another letter: 0x00689c2a).
  ///
  /// If the finished text is itself a name in the exact map, it is translated
  /// once more (0x00689b8d, then 0x00689650 at 0x00689bd2).
  [[nodiscard]] std::string format(std::string_view key,
                                   std::span<const std::string> arguments) const;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::span<const Translation> entries() const noexcept { return entries_; }

 private:
  /// Sorted by key, so lookup is a binary search and iteration order is stable.
  /// Never a hash map: `sim/system.hpp` forbids unordered containers anywhere
  /// the simulation can see, and a table that is loaded once has nothing to
  /// gain from one.
  std::vector<Translation> entries_;
  /// The keys folded to lower case, each with its index into `entries_`,
  /// sorted; for `translate_in_context`.
  std::vector<std::pair<std::string, std::uint32_t>> folded_;
  /// The original's two maps, for `lookup`: every `text` and non-empty
  /// `justtext`, later rows overwriting earlier ones; sorted by name.
  std::vector<Translation> named_;
  /// The same names folded to lower case, filled the same way.
  std::vector<Translation> named_folded_;

  /// The result filed under `name` in `map` (sorted), or null.
  [[nodiscard]] static const std::string* find_in(const std::vector<Translation>& map,
                                                  std::string_view name) noexcept;
};

/// Substitute `%s1`/`%d1`-style placeholders in `text`.
///
/// Named `substitute` rather than `format` because `<format>` is in the
/// standard library now and the two are ambiguous wherever both are visible.
///
/// `arguments[0]` fills `%s1` and `%d1`, `arguments[1]` fills the `2` forms,
/// and so on. A placeholder with no argument is **left as written** rather than
/// blanked, so a mismatch shows up in the string a tester is looking at instead
/// of vanishing.
///
/// Only a single digit is read, which is all the corpus uses (`%s1` to `%s3`,
/// `%d1` to `%d4`). `%` followed by anything else is literal.
[[nodiscard]] std::string substitute(std::string_view text,
                                 std::span<const std::string> arguments);

/// Split the first comma-separated token off `text`.
///
/// This is `ParseStr`, 71 call sites, and every one of them is
/// `ParseStr(dest, dest)`: the head is returned and the tail replaces the
/// source. `BARRACK_TRAIN_EX.VS` documents its own parameter string as
/// `parm="numbertotrain, maxnumber, class, grouptoadd"` and then compares a
/// parsed token against `"elephant"` with no trimming of its own, and
/// `CONST.INI` writes `TributeTimes =  0,   10,   20,   30` and feeds the
/// tokens straight to `Str2Int`. **Both only work if the separator is a comma
/// and the token is trimmed**, so that is what this does.
///
/// The rule is `ParseStr`'s body, 0x00697580: leading **spaces** are skipped
/// (0x006975f2, the byte 0x20 and nothing else -- a tab is kept), the token
/// runs to the first comma (`strchr`, 0x00697601) or the end, and trailing
/// spaces are cut from it (0x00697634). `tail` is everything after that comma
/// **verbatim**, its own leading and trailing spaces included (0x00697629,
/// 0x00697703); with no comma it is empty, which is what terminates the
/// shipped `while (times != "")` loops. The next `ParseStr` skips the spaces
/// a tail starts with, so only a tail that is nothing but spaces ever shows:
/// `"a, "` leaves `" "`, not `""`.
void split_token(std::string_view text, std::string& token, std::string& tail);

}  // namespace imperivm::core::game
