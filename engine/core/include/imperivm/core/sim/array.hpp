#pragma once

/// `IntArray` and `StrArray`: the script language's growable arrays.
///
/// Not a host entry point -- there is no `IntArray::` anything in `gbr.exe` --
/// but a **type the VM hands out and subscripts**, registered beside `ObjList`
/// in the executable's own type table at `0x006986cc` and `0x006986f2`:
///
///     RegisterType("IntArray", 0x0c, 8, ctor, copy, ..., index)
///     RegisterType("StrArray", 0x0d, 8, ctor, copy, ..., index)
///
/// Forty declarations across 38 shipped files, and until now every one of them
/// fell through `WorldHost::default_value` to a refusal: `IntArray`, `StrArray`,
/// `SquadList` and `Item` were listed there as *"other domains'"*, and no domain
/// owned the first two. A script that declared one and wrote to it trapped with
/// **"host refused this subscript assignment"**, which is what four sequences
/// did every corpus pass and what `TS_CARTHAGETACTIC.VS`, `ESH_MARKET.VS`,
/// `TOWNHALL_AUTOTRAIN.VS`, `WALL_PATROL.VS` and eleven other AI scripts would
/// have done the moment anything reached them.
///
/// ## The whole of what a script does with one
///
/// Two operations, and the corpus uses no others. A survey of every declared
/// array name across all 885 scripts finds **no `.count`, no member of any
/// kind, and no assignment of a whole array** -- an array appears only as
/// `a[i]` and `a[i] = v`. So this models exactly that and nothing more.
///
/// **Assignment grows.** The idiom that makes it necessary opens four shipped
/// sequences verbatim:
///
///     IntArray nA_Conditions;
///     for (n_Count = 0; n_Count < 4; n_Count += 1)
///         nA_Conditions[n_Count] = 0;
///
/// -- an array declared empty and written past its end four times. `ESH_MARKET`
/// does the same with eight strings and then reads them back in a loop. An
/// array that refused to grow would make every one of those a trap, and an
/// array that started at some fixed size would be a number nothing in the data
/// justifies.
///
/// ## What is inferred, and what bounds it
///
/// **A read past the end answers the zero value and does not grow.** No shipped
/// script reads an element it has not written -- all forty declarations are
/// filled from index 0 upward before anything reads them -- so the executable's
/// behaviour here is unobservable through shipped content, and the two readings
/// (grow, or answer the default) differ only in a case the corpus cannot
/// construct. Answering without growing is the one that cannot turn a stray
/// index into unbounded memory.
///
/// **Growth is capped at `kMaxElements`.** Nothing in the data justifies a
/// particular ceiling -- the largest shipped array holds eight -- and an
/// uncapped `a[2000000000] = 1` is a script's typo turned into an allocation
/// failure. A fixed bound is at least identical on every peer, which running out
/// of memory is not; it is the same argument `World::kMaxQueryDepth` is written
/// with, and the number is four orders of magnitude above anything shipped.
///
/// **A negative index is refused rather than clamped**, on both the read and the
/// write. There is no element -1 to answer with and no length that could hold
/// one.
///
/// ## Why a pool rather than a value
///
/// The same reason `ObjList` has one. An array reaches a host function --
/// `GetCounterUnits(Settlement, IntArray)` takes one and fills it -- so it has
/// to be a handle rather than a payload inside `script::Value`, and a handle
/// needs somewhere to point. Entries are keyed by `(script, slot)` exactly as
/// `ObjListPool`'s are, so re-entering a scope clears and reuses one entry and
/// the pool is bounded by the program text rather than by how long a script
/// runs. `release_script` drops a dead script's arrays; the scheduler's teardown
/// hook is what calls it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"

namespace imperivm::core::sim {

/// A handle into `ArrayPool`. One-based, so that zero stays distinguishable
/// from the first real array -- the rule `ObjListId` already follows.
using ScriptArrayId = std::uint32_t;

inline constexpr ScriptArrayId kNoScriptArray = 0;

/// The two handle types. **9 and 10, and the census they continue is spread
/// across five headers now** -- see `sim/world_host.hpp`, where it is kept, and
/// the corrections table entry about `kTypeRect` being appended onto
/// `kTypeObjList` for want of one.
inline constexpr script::TypeId kTypeIntArray = 9;
inline constexpr script::TypeId kTypeStrArray = 10;

/// The largest index an array will grow to hold. See the header.
inline constexpr std::int32_t kMaxElements = 65536;

[[nodiscard]] script::Value make_array_value(script::TypeId type, ScriptArrayId id) noexcept;
[[nodiscard]] bool is_script_array(const script::Value& value) noexcept;
/// `kNoScriptArray` when `value` is not an array handle.
[[nodiscard]] ScriptArrayId array_of(const script::Value& value) noexcept;

/// The arrays every live script holds.
class ArrayPool {
 public:
  /// Mint or reuse the entry for one declaration site, cleared.
  ///
  /// `strings` picks which of the two element types the entry holds. A site
  /// cannot change its mind: the declaration names the type once.
  ScriptArrayId acquire(script::ScriptId script, std::uint32_t slot, bool strings);

  /// Drop every entry a script owns.
  void release_script(script::ScriptId script);

  [[nodiscard]] bool contains(ScriptArrayId id) const noexcept;
  [[nodiscard]] script::ScriptId owner_of(ScriptArrayId id) const noexcept;
  /// How many elements the array holds, or 0 for an unknown handle.
  [[nodiscard]] std::size_t size(ScriptArrayId id) const noexcept;

  /// Read one element. Out of range -- above the end or negative -- answers the
  /// zero value for the element type and grows nothing.
  [[nodiscard]] script::Value get(ScriptArrayId id, std::int32_t index) const;

  /// Write one element, growing the array to hold it and filling the gap with
  /// the zero value. False for an unknown handle, a negative index, an index
  /// above `kMaxElements`, or a value of the wrong element type.
  bool set(ScriptArrayId id, std::int32_t index, const script::Value& value);

  [[nodiscard]] std::size_t capacity() const noexcept { return entries_.size(); }

  /// Little-endian, self-describing, versioned: the convention every other
  /// store in this tree follows.
  void serialize(std::vector<std::byte>& out) const;
  [[nodiscard]] Status deserialize(std::span<const std::byte> data);

  void clear() noexcept { entries_.clear(); }

  // **There is no `hash`, and that is the standing decision rather than an
  // omission.** `scriptstate` is zero in all nine desync dumps -- the original
  // does not hash a script's own storage -- and `ObjListPool` is saved and
  // unhashed for exactly that reason. An array is the same kind of thing: a
  // script's locals, restored on load because nothing downstream would notice
  // their absence, which is precisely why they cannot be trusted to a hash
  // check.

 private:
  struct Entry {
    script::ScriptId script = script::kNoScript;
    std::uint32_t slot = 0;
    bool live = false;
    bool strings = false;
    std::vector<std::int32_t> ints;
    std::vector<std::string> text;
  };

  std::vector<Entry> entries_;
};

}  // namespace imperivm::core::sim
