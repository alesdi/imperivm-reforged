#pragma once

/// The class graph: the game's object model, loaded from `.SC.XML`.
///
/// Specification: docs/formats/sc-xml.md. Resolved inventory: docs/data-model.md.
/// Reference reader (ground truth): src/imperivm/formats/gamedata.py.
///
/// Every placeable, spawnable or scriptable thing in Imperivm is one `<class>`
/// element, and the 845 of them form a single rooted inheritance tree, seven
/// deep at the most. A class is a native C++ class name, a property bag, a
/// table of `.vs` methods, some art, and some UI wiring; all of it inherited.
///
/// **Loading is tolerant.** The retail data has 38 dangling references out of
/// roughly 12,000 -- a few sounds, two icons, six entities, and four scripts
/// bound by classes whose scripts are not in `data.pak` -- and the retail
/// engine ships and runs with them. A loader that rejects a class because its
/// select sound is missing loses the outposts and the stonehenges. So `add()`
/// fails only on a document that is not a class definition at all, and
/// everything else is reported by `validate()` after the fact.
///
/// **The graph owns its strings.** The XML reader is zero-copy into the
/// caller's buffer, but a class graph outlives the 845 buffers it was built
/// from, so every name and value is interned here. That makes the graph
/// movable but not copyable: interned views point into its arena.
///
/// The core cannot open a file, so `validate()` takes a `ResourceProbe` for
/// the checks that need to know whether a path exists in the packs. Without
/// one it still reports everything the graph knows on its own: dangling
/// parents, cycles, duplicate ids, and class-name properties that name no
/// class.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core {

using ClassIndex = std::uint32_t;

inline constexpr ClassIndex kNoClass = 0xFFFFFFFFu;

/// Four seasons, not two. `entity_spring`, `entity_autumn` and `entity_winter`
/// override `entity`; summer has no attribute of its own and always falls back
/// to it. 14 classes declare the three seasonal paths and no `entity` at all,
/// so in summer those have no art -- see the open question in sc-xml.md.
enum class Season : std::uint8_t {
  summer = 0,
  /// The same slot under the name the entity runtime uses for it: `entity` is
  /// the seasonless default, and calling it `base` says that more plainly than
  /// calling it the season nothing declares. One enum, two spellings, so the
  /// class graph and the entity side cannot end up with two of them.
  base = 0,
  spring = 1,
  autumn = 2,
  winter = 3,
  count = 4,  ///< for sizing an array over the seasons; never a value
};

/// The season `game.xml` names -- `spring`, `autumn`, `winter` -- and the
/// seasonless slot for anything else, which is what the four blank templates
/// that name none get.
[[nodiscard]] constexpr Season season_from_name(std::string_view name) noexcept {
  if (name == "spring") return Season::spring;
  if (name == "autumn") return Season::autumn;
  if (name == "winter") return Season::winter;
  return Season::summer;
}

/// One key/value pair from `<properties>` or `<sounds>`. Kept in a vector
/// sorted by key rather than a hash map: iteration order is world state
/// (docs/engine/architecture.md), and 845 bags of at most a few dozen entries
/// do not need anything cleverer.
struct ClassProperty {
  std::string_view key;
  std::string_view value;
};

/// One `<method>`: a script bound to a name on a class.
struct ClassMethod {
  std::string_view sig;
  std::string_view vs;
  std::string_view verify;    ///< empty when absent
  std::string_view onfinish;  ///< empty when absent
};

/// One `<cmd>` inside a `<defaultcmd>`.
struct DefaultCommand {
  std::string_view name;
  bool ctrl = false;  ///< the modifier-held variant
};

/// One `<defaultcmd>`: the ordered commands offered against a target class.
/// `target` is empty for the no-target case (a right click on terrain).
struct DefaultCommandBlock {
  std::string_view target;
  std::vector<DefaultCommand> cmds;
};

/// One `<value0>`..`<value5>` info-bar slot. `script` is inline `.vs` source,
/// the one place in the format where script is embedded rather than referenced.
struct InfoValue {
  bool present = false;
  std::string_view icon;
  std::string_view script;
  std::string_view rollover;
  std::string_view help;
  std::string_view flags;  ///< `-1`, `0` or `5`; meaning unknown
};

/// One `<class>` element: its own contributions only, nothing inherited.
///
/// `properties`, `sounds` and `methods` are already merged *within* the class,
/// because `<properties>` and `<sounds>` repeat and merge attribute-wise and
/// `<method>` is keyed on `sig`. Later wins in all three, which is what the
/// reference reader does; see sc-xml.md for why last-wins is assumed rather
/// than proven for the 14 duplicate `sig` declarations.
struct ClassDefinition {
  std::string_view id;
  std::string_view cpp_class;
  std::string_view parent;  ///< empty on the root
  std::string_view altid;   ///< empty when absent; a second, non-unique registry key
  std::string_view source;  ///< where it was loaded from, for reports

  /// Indexed by `Season`; `entity` itself is `seasonal[summer]`.
  std::array<std::string_view, 4> seasonal{};

  std::vector<ClassProperty> properties;  ///< sorted by key
  std::vector<ClassProperty> sounds;      ///< sorted by channel
  std::vector<ClassMethod> methods;       ///< sorted by sig
  std::vector<std::string_view> behaviors;
  std::vector<DefaultCommandBlock> default_cmds;  ///< in document order
  std::array<InfoValue, 6> values{};
  bool no_defcmd_inherit = false;

  ClassIndex parent_index = kNoClass;
  std::vector<ClassIndex> children;  ///< sorted by id
  std::uint32_t depth = 0;           ///< 1 at the root, 0 before link()
};

/// What `validate()` found. Nothing here stops a load.
enum class ClassIssueKind : std::uint8_t {
  dangling_parent,  ///< `parent` names no declared class
  cycle,            ///< the ancestry of a class does not terminate
  duplicate_id,     ///< two files declare the same `id`; the first one wins
  altid_collision,  ///< an `altid` claimed twice, or claimed by a real `id`
  missing_entity,
  missing_script,
  missing_sound,
  missing_icon,
  dangling_class_ref,  ///< a property naming a class that does not exist
};

struct ClassIssue {
  ClassIssueKind kind = ClassIssueKind::dangling_parent;
  std::string_view source;  ///< the file the reference is in
  std::string detail;       ///< `attribute=value`, built for reporting
};

/// Existence checks the core cannot make for itself.
///
/// The engine has no filesystem and no pack index down here, so validation of
/// resource references is delegated. `exists` is asked about raw references as
/// written in the XML (`gameres/icons/x.bmp`, `data/subai/y.vs`); normalising
/// separators, case, and the `gameres/` -> `UI\` virtual root is the caller's
/// job, because only the caller knows what it is indexing.
class ResourceProbe {
 public:
  virtual ~ResourceProbe() = default;
  virtual bool exists(std::string_view path) const = 0;

  /// A `<sounds>` value is either a path or a bare sound-entity name that
  /// resolves under `DATA\SOUND ENTITIES`. The distinction is the caller's to
  /// make for the same reason.
  virtual bool sound_exists(std::string_view value) const = 0;
};

class ClassGraph {
 public:
  ClassGraph() = default;
  ClassGraph(const ClassGraph&) = delete;
  ClassGraph& operator=(const ClassGraph&) = delete;
  ClassGraph(ClassGraph&&) noexcept = default;
  ClassGraph& operator=(ClassGraph&&) noexcept = default;

  /// Parse one `.SC.XML` document and register it. `source` is kept for
  /// reports and is the only thing that identifies the file afterwards -- the
  /// file name is *not* the identity, `id` is.
  ///
  /// Fails only when the document is not a class definition: unparseable XML,
  /// a root element other than `<class>`, or a missing `id` / `cpp_class`. A
  /// duplicate `id` is kept out of the registry and reported by `validate()`.
  Status add(std::span<const std::byte> document, std::string_view source);

  /// Resolve `parent` links, order children, and compute depths. Call once,
  /// after the last `add()`. Idempotent. Classes whose parent does not resolve
  /// become additional roots rather than errors.
  void link();

  [[nodiscard]] std::size_t size() const noexcept { return classes_.size(); }
  [[nodiscard]] bool empty() const noexcept { return classes_.empty(); }
  [[nodiscard]] const ClassDefinition& at(ClassIndex index) const { return classes_[index]; }

  /// Lookup by `id`. This is the registry key; use it for `parent` and for
  /// anything the data guarantees is an `id`.
  [[nodiscard]] ClassIndex find(std::string_view id) const;

  /// Lookup by `id`, falling back to `altid`. Three `altid` values are claimed
  /// by more than one class and one collides with a real `id` (`Inn`), so `id`
  /// must win and the first registration of an `altid` must win after that.
  [[nodiscard]] ClassIndex lookup(std::string_view name) const;

  /// Root classes, sorted by id. The retail corpus has exactly one, `Object`.
  [[nodiscard]] std::span<const ClassIndex> roots() const noexcept { return roots_; }

  /// Classes in registration order (the order `add()` saw them).
  [[nodiscard]] const std::vector<ClassDefinition>& classes() const noexcept { return classes_; }

  /// 1 at a root, 0 if `link()` has not run.
  [[nodiscard]] std::uint32_t depth(ClassIndex index) const { return classes_[index].depth; }
  [[nodiscard]] std::uint32_t max_depth() const noexcept { return max_depth_; }

  [[nodiscard]] std::span<const ClassIndex> children(ClassIndex index) const {
    return classes_[index].children;
  }

  /// The class and its ancestors, nearest first. Truncated at the first repeat
  /// rather than looping, so a cycle in hostile data costs a short vector.
  [[nodiscard]] std::vector<ClassIndex> ancestry(ClassIndex index) const;

  /// Everything below `index`, including it. Sorted by id within each level.
  [[nodiscard]] std::size_t subtree_size(ClassIndex index) const;

  // -- resolution ------------------------------------------------------
  //
  // resolved(C) = merge(resolved(parent(C)), own(C)) for every bag below,
  // except the default-command table, which accumulates, and behaviors,
  // which append.

  [[nodiscard]] std::vector<ClassProperty> resolved_properties(ClassIndex index) const;
  [[nodiscard]] std::vector<ClassProperty> resolved_sounds(ClassIndex index) const;
  [[nodiscard]] std::vector<ClassMethod> resolved_methods(ClassIndex index) const;

  /// The behaviour list `gbr.exe` keeps on a class (`[class+0x1ec]` entries,
  /// read through 0x0059aec0): the class's own `<behavior>`s in document
  /// order, then its parent's resolved list, **duplicates kept**, at most
  /// `kMaxBehaviors`. The loader appends a class's own as it parses them and
  /// its parent's afterwards (0x005a5fd0), so a child's script runs in the
  /// lower slot. `MutableStronghold` is the one shipped class that declares
  /// what it also inherits, and it gets `settlement_behavior_ambient.vs` twice,
  /// as the original's does. Slot `i + 1` of an object is entry `i`.
  [[nodiscard]] std::vector<std::string_view> resolved_behaviors(ClassIndex index) const;
  /// The loader's limit on a class's behaviour list (0x005a046b, 0x005a6046).
  static constexpr std::size_t kMaxBehaviors = 8;

  /// `<defaultcmd>` blocks accumulate down the tree, keyed on `target`, and a
  /// child's block for a target it already inherits replaces that block in
  /// place. `<nodefcmdinherit/>` discards everything inherited so far -- only
  /// `Sentry` and `Wagon` use it.
  [[nodiscard]] std::vector<DefaultCommandBlock> resolved_default_cmds(ClassIndex index) const;

  [[nodiscard]] std::array<InfoValue, 6> resolved_values(ClassIndex index) const;

  /// One resolved property without materialising the whole bag: the nearest
  /// declaration walking up from `index`, or an empty view.
  [[nodiscard]] std::string_view property(ClassIndex index, std::string_view key) const;

  /// The entity a class uses in a season: `entity_<season>` when present and
  /// non-empty, otherwise `entity`. Empty means the class has no art of its
  /// own, which 61 abstract classes deliberately do not.
  [[nodiscard]] std::string_view entity_path(ClassIndex index, Season season) const;

  // -- validation ------------------------------------------------------

  /// Report every dangling reference. Never fails a load and never mutates.
  /// Pass a probe to include resource references; without one only the checks
  /// the graph can make on its own are run.
  [[nodiscard]] std::vector<ClassIssue> validate(const ResourceProbe* probe = nullptr) const;

  /// Property names whose value is a class reference. `importsettlement` is
  /// deliberately absent: its values name settlement templates, and validating
  /// it as a class reference invents five failures. See data-model.md.
  static std::span<const std::string_view> class_reference_properties() noexcept;

 private:
  /// Interned storage for every string the graph hands out. Blocks are never
  /// resized once written, so views into them survive the arena being moved.
  class StringArena {
   public:
    std::string_view intern(std::string_view text);

   private:
    static constexpr std::size_t kBlockSize = 64 * 1024;
    std::vector<std::vector<char>> blocks_;
  };

  StringArena arena_;
  std::vector<ClassDefinition> classes_;
  std::vector<ClassIndex> by_id_;     ///< indices into classes_, sorted by id
  std::vector<ClassIndex> by_altid_;  ///< indices into classes_, sorted by altid
  std::vector<ClassIndex> roots_;
  std::vector<std::pair<std::string_view, std::string_view>> duplicate_ids_;
  std::uint32_t max_depth_ = 0;
};

}  // namespace imperivm::core
