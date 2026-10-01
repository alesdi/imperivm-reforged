#pragma once

/// Notes: the mission objectives a campaign pins on the player's list.
///
/// 220 call sites across four entry points -- `GiveNote/1` (115),
/// `RemoveNote/1` (65), `IsNoteActive/1` (24), `ClearNotes/0` (16) -- and
/// **every one of them is inside a map container**. They are what makes a
/// mission legible: *"Capture the rebel stronghold of Numantia"*, *"Scipio
/// Amelianus must survive"*.
///
/// ## Two halves, and only one of them is state
///
/// A note is **declared** in a container's `Notes.xml` and **given** by a
/// script. The declaration carries the text, the icon and the minimap pin; the
/// giving carries nothing but the id. So the catalogue is configuration -- it
/// is the same on both sides of a save and is rebuilt from the container -- and
/// the board of active ids is world state, because `IsNoteActive` is a
/// predicate a running script branches on.
///
/// ## A note that is not declared cannot be given
///
/// This is the rule that would be missed, and it is explicit in the
/// instructions. `gbr.exe`'s add helper (0x005584a0) opens by calling
/// 0x00557df0 with the id and **returns immediately when it answers null**
/// (`test ebp,ebp / je 0x0055869e`), before it touches the active map. That
/// lookup searches two catalogues in turn -- the `std::map`s at `[0x009bdaa0]`
/// and `[0x009bdaa4]` -- and returns the mapped value at `node + 0x28` or
/// null.
///
/// **It fires on shipped data exactly once, and the reason it is only once is
/// the finding.** Of the installation's 204 *live* literal note calls, 203 name
/// an id their own container declares. The one that does not is
/// `GiveNote("Historical Inconsistency")` in `1_Great_Battles_Zama` map 6's
/// `seq5.vs`. Its twin is the explanation:
/// `GiveNote("Historical Inconcistency")` in `3_Great_Battles_Alesia` map 5's
/// `seq4.vs` is the same note under a misspelling, and it is **commented out**
/// -- so nobody was ever going to notice that neither container declares it. A
/// grep finds 206 calls, not 204, and the two it adds are the commented ones;
/// this project has now been caught by that difference twice in one day, the
/// other time on `RunAIHelper`.
///
/// ## What is inferred
///
/// **Which of the two catalogues is searched first.** 0x00557df0 tries
/// `[0x009bdaa0]` and falls through to `[0x009bdaa4]`, and nothing says which
/// global holds the container's root `Notes.xml` and which holds the map's
/// own. It is unobservable on shipped data: **no id is declared in both**
/// across all 25 containers, and in fact all 122 declarations are in a map's
/// document -- every one of the 25 root `Notes.xml` files is an empty
/// `<notes></notes>`, as are 36 of the 50 note documents in total. The map's is consulted first here, on the same
/// specific-beats-general reading `Scheduler::find_chunk_exact` and the
/// sequence manifests already use.
///
/// **The order the board holds ids in.** The original's active set is a
/// `std::map` keyed by the id, so it is ascending by name, and that is what
/// this keeps -- iteration order is state (rule 3 of
/// docs/engine/architecture.md) and a `std::map`'s order is the one the
/// original can be seen to have.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// One `<note>` element, verbatim.
///
/// Eight attributes, and the shipped corpus uses all eight. `title` and `text`
/// are display strings -- `text` carries literal `\n` escapes, which are the
/// author's and are **not** unescaped here, because nothing in the simulation
/// reads them and the interface layer is where a line break means something.
struct NoteDefinition {
  /// The key `GiveNote`, `RemoveNote` and `IsNoteActive` all name. Free-form:
  /// the 112 distinct ids in the installation include `GOAL`, `War_Strategy1`,
  /// `Kill Syphax` and `Roman reinforcements`, so spaces and case are both
  /// significant and neither may be normalised.
  std::string id;
  std::string title;
  std::string text;
  /// `gameres/noteicons/triangle.bmp`, or empty for the default.
  std::string icon;
  /// The map name the note's pin belongs to, or empty for "no pin".
  std::string map;
  bool show_on_minimap = false;
  /// `locationx`/`locationy`. `(-1, -1)` is the shipped "nowhere", and 51 of
  /// the 122 declarations carry it.
  Point location{-1, -1};
};

/// Every note a container declares.
///
/// **Configuration, not state.** Rebuilt from the container on both sides of a
/// save, never hashed, never serialised -- the same standing as
/// `CampaignSystem`'s territory ids and `AreaSystem`'s regions.
class NoteCatalogue {
 public:
  /// Parse one `<notes>` document and append what it declares. Returns how
  /// many were added.
  ///
  /// An empty span adds nothing and is **not** an error: 36 of the
  /// installation's 50 note documents are an empty `<notes></notes>`, and a
  /// container with no notes at all is ordinary.
  ///
  /// A `<note>` with no `id` is skipped rather than stored, because nothing
  /// could ever name it; a duplicate id is skipped too, first-in wins, which
  /// is what a `std::map` insert does with a key it already holds.
  [[nodiscard]] Result<std::size_t> add(std::span<const std::byte> xml);

  /// The definition `id` names, or null. Byte-exact and case-sensitive, as the
  /// original's `std::map<std::string, ...>` lookup is.
  [[nodiscard]] const NoteDefinition* find(std::string_view id) const noexcept;

  /// Declaration order, which is document order, which is the order the two
  /// documents were added in.
  [[nodiscard]] std::span<const NoteDefinition> all() const noexcept { return notes_; }
  [[nodiscard]] std::size_t size() const noexcept { return notes_.size(); }
  [[nodiscard]] bool empty() const noexcept { return notes_.empty(); }
  void clear() noexcept { notes_.clear(); }

 private:
  std::vector<NoteDefinition> notes_;
};

/// The notes a session has given, ascending by id.
///
/// **World state**, because `IsNoteActive` is a predicate a running script
/// branches on -- `5_Great_Battles_Britain` map 3's `seq8.vs` opens
/// `if (selu.name == "Senator" && !IsNoteActive("Senator Pablius"))`. Hashed
/// and serialised for the same reason.
class NoteBoard {
 public:
  /// `GiveNote`. False when the note was already active, which is what
  /// 0x005584f6 does with a key `lower_bound` already found.
  bool give(std::string_view id);
  /// `RemoveNote`. False when it was not active.
  bool remove(std::string_view id);
  /// `ClearNotes`. Takes every note off the list; the catalogue is untouched.
  void clear() noexcept;

  [[nodiscard]] bool active(std::string_view id) const noexcept;
  /// Ascending by id -- a `std::map`'s order. See the header note.
  [[nodiscard]] std::span<const std::string> active_notes() const noexcept { return active_; }
  [[nodiscard]] std::size_t size() const noexcept { return active_.size(); }

  /// FNV-1a over the active ids in order. Folded into `CampaignSystem::hash`.
  void hash(std::uint64_t& accumulator) const noexcept;

  void serialize(std::vector<std::byte>& out) const;
  /// **Atomic**: decoded into a local and moved in only once every id has read
  /// cleanly, and refused when the ids do not arrive strictly ascending --
  /// `active` binary-searches on that, so a table out of order would make every
  /// lookup wrong in a way nothing later would notice.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  std::vector<std::string> active_;
};

}  // namespace imperivm::core::sim
