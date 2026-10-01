#pragma once

/// Items: `DATA\ITEMS.XML`, `CVXItem`, `CVXItemHolder`, and the holder
/// relationship they are carried by.
///
/// ## The data is the specification
///
/// `DATA\ITEMS.XML` declares 42 items and every number below comes from it. An
/// item is a name, a charge count, a bonus block, and up to five script hooks:
///
/// ```xml
/// <item id="King's Belt" level="0" important="yes" ...>
///   <bonus health="600" damage="0" armor_slash="10" armor_pierce="10"
///          level="0" experience="0"/>
/// </item>
/// ```
///
/// The bonus block's full attribute set over the 42 items is `health`,
/// `health_percent`, `damage`, `damage_percent`, `armor_slash`,
/// `armor_slash_percent`, `armor_pierce`, `armor_pierce_percent`, `level` and
/// `experience` -- flat and proportional for each of the four combat stats, plus
/// a level and an experience grant. The percent forms exist: `Veteran Offence`
/// is `damage_percent="20"`, `Veteran Defence` is 20% on both armours, and
/// `Veteran Health` is `health_percent="20"`.
///
/// ## An item is an object with a handle
///
/// `docs/engine/state-vector.md`: `CVXItem` prints
/// `handle | type | owner handle | usecount | customdata`, 170 blocks over 3 of
/// 9 dumps. Two things follow and both are reproduced here.
///
///   * **Identity is by name.** `type` is a string, not an id -- twelve distinct
///     names across the corpus. So `ItemCatalog` resolves names, and an
///     `ItemInstance` remembers the definition it was minted from.
///   * **The owner is the holder relationship.** `owner handle` lands on a
///     `CVXUnit` 138 times, a `CVXItemHolder` 25, a `CVXBuilding` 3 and a
///     `CVXHero` once. The same field, the same mechanism, as a garrisoned
///     unit's `holder handle`: an object inside something has no position of its
///     own. So an item is spawned as `InternalKind::item` and placed with
///     `World::put_in_holder`, and its position is its owner's.
///
/// `usecount` is charges remaining; the dumps show 0 (153 items), 1, 6, 200,
/// 500 and 2000, which line up with `Poison Mushroom`/`Healing herbs` (1),
/// `Finger of death` (6), `Rye spikes` (200), `Ring of Power` (grown to a 500
/// cap by its own script) and `Healing water` (2000). `customdata` is 0 in all
/// 170 and is carried rather than interpreted.
///
/// ## Capacity
///
/// `inventory_size` is a class property: 4 on `Unit`, 16 on `ItemHolder`, 1 on
/// `Wagon`, 0 on `BaseRuins`. Nothing else declares it, so everything else
/// inherits `Unit`'s 4 or has none at all.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

/// The `<bonus>` block, verbatim.
///
/// Flat terms add; percent terms are of the base stat and are integer
/// arithmetic -- `base + base * percent / 100`, truncating -- because a
/// floating-point multiply here would be a desync (rule 1 of
/// docs/engine/architecture.md).
struct ItemBonus {
  std::int32_t health = 0;
  std::int32_t health_percent = 0;
  std::int32_t damage = 0;
  std::int32_t damage_percent = 0;
  std::int32_t armor_slash = 0;
  std::int32_t armor_slash_percent = 0;
  std::int32_t armor_pierce = 0;
  std::int32_t armor_pierce_percent = 0;
  std::int32_t level = 0;
  std::int32_t experience = 0;

  ItemBonus& operator+=(const ItemBonus& other) noexcept;

  [[nodiscard]] bool any() const noexcept;

  friend bool operator==(const ItemBonus&, const ItemBonus&) noexcept = default;
};

/// When an item's script runs. The five `*_script` attributes in `ITEMS.XML`.
enum class ItemScriptHook : std::uint8_t {
  /// `use_script` -- the player (or the AI) activates the item. 9 of 42 items.
  /// The script ends by calling `ItemUsed(n)` to spend charges.
  use = 0,
  /// `equip_script` -- a long-running thread that starts when the item is
  /// picked up. `Ring of Power` and `Elephant Tusk`, both `while (1) { Sleep }`
  /// loops that heal the bearer.
  equip,
  /// `object_script` -- `Concentration stone` only.
  object,
  /// `kill_script(owner, victim)` -- the bearer killed something. `God's Gift`,
  /// `Veteran Guild`, `Amulet of Triumph`, `Pillaging Permit`.
  kill,
  /// `attacheddie_script(owner, attached)` -- a warrior attached to the bearer
  /// died. `Scroll of Death` and `Gem of Wisdom`, and the reason this hook is
  /// in the hero domain rather than the combat one.
  attached_die,
  count,
};

/// One `<item>` declaration.
struct ItemDefinition {
  std::string id;    ///< the `id`/`type` string; item identity in the sync set
  std::string name;  ///< `name`, the display string; usually equal to `id`
  /// `image`: the icon the info bar's `UIInventory` strip draws, a bitmap
  /// under `gameres/icons/items/`. All 42 declare one.
  std::string image;
  /// `description`: the tooltip text, a translation key.
  std::string description;
  ItemBonus bonus;
  /// `usecount`: the charges an instance is minted with. 0 means "not a
  /// consumable" -- 27 of the 42 items -- and a `use_script` on a zero-charge
  /// item is one that manages its own counter (`Ring of Power` grows one).
  std::int32_t usecount = 0;
  /// `level`. Zero on all 42 shipped items; carried because the attribute is
  /// declared on every one of them.
  std::int32_t level = 0;
  bool important = false;  ///< `important="yes"`; a UI distinction
  /// `cursed="1"`. 11 of 42 items. The retail meaning is unrecovered -- no
  /// shipped script reads it -- so it is carried and never acted on.
  bool cursed = false;
  /// Script paths per hook, empty when the item declares none.
  std::string scripts[static_cast<std::size_t>(ItemScriptHook::count)];

  [[nodiscard]] std::string_view script(ItemScriptHook hook) const noexcept {
    return scripts[static_cast<std::size_t>(hook)];
  }
};

using ItemTypeIndex = std::uint32_t;
inline constexpr ItemTypeIndex kNoItemType = 0xFFFFFFFFu;

/// The parsed `ITEMS.XML`.
///
/// Definitions are load-time state: they are the same for every peer and never
/// change, so they are not hashed. Only instances are.
class ItemCatalog {
 public:
  /// Parse a `<items>` document. Unknown attributes are ignored rather than
  /// rejected, the same tolerance `ClassGraph::add` applies, because the retail
  /// data is what it is.
  Status load(std::span<const std::byte> document);

  /// Register one definition directly, for tests and for synthetic worlds.
  ItemTypeIndex add(ItemDefinition definition);

  /// Case-insensitive lookup by `id`. Shipped ids mix case (`God's Gift`,
  /// `Boar teeth`) and scripts spell them back exactly, but `AddItem("Boar
  /// tooth")` in `BONUSSCRIPTS\103` names an item that does not exist at all,
  /// so a tolerant lookup costs nothing and a strict one loses nothing either.
  [[nodiscard]] ItemTypeIndex find(std::string_view id) const;

  [[nodiscard]] const ItemDefinition* at(ItemTypeIndex index) const;
  [[nodiscard]] std::span<const ItemDefinition> definitions() const noexcept { return defs_; }
  [[nodiscard]] std::size_t size() const noexcept { return defs_.size(); }

 private:
  std::vector<ItemDefinition> defs_;  ///< document order
};

/// One `CVXItem`: a live item in the world.
///
/// `id` is a real object handle out of `World`'s counter, because that is what
/// the dumps show. `owner` mirrors `ObjectState::holder` for the item's object
/// and is kept in step by `ItemStore`, never written on its own.
struct ItemInstance {
  ObjectId id = kNoObject;
  ItemTypeIndex type = kNoItemType;
  ObjectId owner = kNoObject;
  std::int32_t usecount = 0;
  /// `customdata`: 0 in all 170 dumped items. **Unknown**; carried.
  std::int32_t customdata = 0;
};

/// Every live item, and who holds it.
///
/// Instances are kept sorted by object id, which is spawn order, which is the
/// order everything else in the simulation iterates in.
class ItemStore {
 public:
  explicit ItemStore(const ItemCatalog* catalog = nullptr) noexcept : catalog_(catalog) {}

  void set_catalog(const ItemCatalog* catalog) noexcept { catalog_ = catalog; }
  [[nodiscard]] const ItemCatalog* catalog() const noexcept { return catalog_; }

  /// `inventory_size` for an owner, resolved from its class. Falls back to
  /// `fallback` when there is no class graph -- every synthetic test.
  [[nodiscard]] std::int32_t capacity_of(const World& world, ObjectId owner,
                                         std::int32_t fallback = 4) const;

  /// `Obj.AddItem(name)` -- 32 call sites. Mints a `CVXItem` object in the
  /// world, charges it from the definition, and puts it in `owner`'s holder.
  /// Returns `kNoObject` when the name is unknown or the inventory is full;
  /// the shipped bonus scripts test the result and retry, so failing is normal.
  ObjectId add(World& world, ObjectId owner, std::string_view id);
  ObjectId add(World& world, ObjectId owner, ItemTypeIndex type);

  /// `Obj.GiveItem(item, to)` -- move an item between owners. Fails when the
  /// destination is full, which leaves the item where it was.
  bool give(World& world, ObjectId item, ObjectId to);

  /// `Unit.PutItem(item, holder)`. The same operation as `give`; both spellings
  /// ship, and `PutItem`'s destination is always a `CVXItemHolder`.
  bool put(World& world, ObjectId item, ObjectId holder) { return give(world, item, holder); }

  /// `Unit.DropItem(item, pt)` -- leave the item on the ground at `at`. The
  /// original creates a `DefItemHolder` prop for a dropped item
  /// (`delete_empty="1"`), which is an object spawn and therefore the map
  /// layer's business; here the item simply leaves its owner and takes a
  /// position of its own.
  bool drop(World& world, ObjectId item, Point at);

  /// Destroy the item and its object.
  bool remove(World& world, ObjectId item);

  /// `Obj.RemoveItem(index)` -- the shipped `Spoils of War` script indexes into
  /// an owner's inventory. Indices are the order `contents_for` returns.
  bool remove_at(World& world, ObjectId owner, std::size_t index);

  /// `Obj.RemoveItemsOfType(name)` -- returns how many went.
  std::int32_t remove_all_of_type(World& world, ObjectId owner, std::string_view id);

  /// `Obj.ExchangeItem(item, newname)` -- `Veteran Guild` turns itself into a
  /// `Veteran Medal` on a kill. Keeps the object and its handle, and re-charges
  /// it from the new definition.
  bool exchange(ObjectId item, std::string_view new_id);

  /// One charge off, for `Obj::UseItem`. An item whose charges reach zero
  /// *having started above zero* is consumed and removed; an item that was
  /// never a consumable (`usecount == 0` in `ITEMS.XML`) is not.
  ///
  /// **This is not `ItemUsed(n)`**, which its doc comment used to claim it was.
  /// `ItemUsed` is `spend` below and the two rules genuinely differ. What this
  /// one implements is `fn_use_item`'s assumption that running an item's
  /// `use_script` costs a charge -- and that assumption is *not* read off
  /// `Obj::UseItem` (0x00426480), which takes a **string** and does not touch
  /// `[item+0x28]` anywhere in its body. It is left standing, and named here as
  /// an assumption, rather than quietly changed on the strength of a different
  /// function's disassembly.
  bool use(World& world, ObjectId item, std::int32_t charges = 1);

  /// `ItemUsed(n)` -- `0x00534a30`, transcribed, and it is blunter than `use`.
  ///
  ///     count -= n;  if (count <= 0) destroy the item object;
  ///
  /// Three differences, all of them observable:
  ///
  ///   * **the count is written before the test and is not clamped.** That one
  ///     is transcribed rather than tested, and it cannot be otherwise: a count
  ///     that goes negative is by the next line a count at or below zero, so
  ///     the item is destroyed and nothing can read it back. It is written this
  ///     way because it is what `0x00534a90` does, not because a caller can
  ///     tell;
  ///   * **there is no consumable rule.** `use` destroys only an item whose
  ///     definition declares `usecount > 0`; this destroys anything whose count
  ///     lands at or below zero, and 153 of the dumps' 170 items carry
  ///     `usecount == 0`, so `ItemUsed(0)` on one of those destroys it. That is
  ///     not a hypothetical: `HEALING WATER.VS`'s zero-healing early exit is
  ///     `ItemUsed(GetUseCount())`, which is how the script says *consume me*;
  ///   * **`n` may be negative**, which adds charges. `RING OF POWER.VS` tops
  ///     itself back up with `ItemUsed(GetUseCount() - charges)`.
  ///
  /// Returns false only when the handle names no item.
  bool spend(World& world, ObjectId item, std::int32_t charges);

  /// `GetUseCount()`.
  [[nodiscard]] std::int32_t use_count(ObjectId item) const;
  void set_use_count(ObjectId item, std::int32_t value);

  [[nodiscard]] const ItemInstance* find(ObjectId item) const;
  [[nodiscard]] ItemInstance* find(ObjectId item);

  /// `Obj.FindItem(name)` -- the first item of that type the owner holds.
  [[nodiscard]] ObjectId find_of_type(ObjectId owner, std::string_view id) const;
  /// `Obj.HasItem(name)`.
  [[nodiscard]] bool has_type(ObjectId owner, std::string_view id) const;
  /// `Obj.item_count`.
  [[nodiscard]] std::int32_t count_for(ObjectId owner) const;
  /// `Obj.GetItemIndex(item)` -- 1-based, because the shipped `Spoils of War`
  /// script tests the result for truth (`if (idx) owner.RemoveItem(idx)`) and a
  /// 0-based index would make the first slot untestable. 0 means "not held".
  [[nodiscard]] std::size_t index_of(ObjectId owner, ObjectId item) const;

  /// Everything `owner` holds, in ascending item-object order.
  std::size_t contents_for(ObjectId owner, std::vector<ObjectId>& out) const;

  /// The bonus block of everything `owner` holds, summed.
  [[nodiscard]] ItemBonus total_bonus(ObjectId owner) const;

  [[nodiscard]] std::span<const ItemInstance> items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

  /// Forget every item whose owner is `owner` -- called when an owner dies.
  /// Items are destroyed rather than dropped: dropping needs a prop object, and
  /// inventing one here would put a spawn in a bookkeeping call.
  void drop_owner(World& world, ObjectId owner);

  void hash(std::uint64_t& accumulator) const noexcept;

  // -- the saved game ----------------------------------------------------

  /// Append the whole store to `out`, with its own magic and version.
  ///
  /// A pair rather than a rebuild through `add`, because `add` needs a `World`
  /// to spawn the item's object, and a saved item's object is already in the
  /// world's own section -- spawning a second one would hand out a new handle
  /// and leave the first orphaned.
  ///
  /// Definitions in `src/sim/save_systems.cpp`, next to the systems that own
  /// this store. Layout: docs/formats/save.md.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace it with the one in `bytes`. **Atomic**: decoded into a local and
  /// moved in only once every row has read cleanly.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  [[nodiscard]] std::size_t lower_bound(ObjectId item) const noexcept;
  ObjectId insert(World& world, ItemTypeIndex type, ObjectId owner);

  const ItemCatalog* catalog_ = nullptr;
  std::vector<ItemInstance> items_;  ///< sorted by id; iteration order is state
};

}  // namespace imperivm::core::sim
