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
/// `tail` receives the remainder, with no leading separator. When there is no
/// comma the whole of `text` is the token and `tail` is empty, which is what
/// terminates the shipped `while (times != "")` loops.
void split_token(std::string_view text, std::string& token, std::string& tail);

}  // namespace imperivm::core::game
