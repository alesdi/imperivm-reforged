// `IntArray` and `StrArray`. See include/imperivm/core/sim/array.hpp.

#include "imperivm/core/sim/array.hpp"

#include <utility>

#include "imperivm/core/sim/save.hpp"

namespace imperivm::core::sim {

namespace {

constexpr std::uint32_t kPoolMagic = 0x59525241u;  // "ARRY"
constexpr std::uint32_t kPoolVersion = 1;

}  // namespace

script::Value make_array_value(script::TypeId type, ScriptArrayId id) noexcept {
  if (id == kNoScriptArray) return script::Value::object(script::ObjectRef{script::kNoType, 0});
  return script::Value::object(script::ObjectRef{type, id});
}

bool is_script_array(const script::Value& value) noexcept {
  if (!value.is_object()) return false;
  const script::TypeId type = value.as_object().type;
  return type == kTypeIntArray || type == kTypeStrArray;
}

ScriptArrayId array_of(const script::Value& value) noexcept {
  return is_script_array(value) ? value.as_object().id : kNoScriptArray;
}

ScriptArrayId ArrayPool::acquire(script::ScriptId script, std::uint32_t slot, bool strings) {
  // The same site always names the same entry, which is what bounds the pool.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = entries_[i];
    if (entry.script != script || entry.slot != slot) continue;
    entry.live = true;
    entry.strings = strings;
    entry.ints.clear();
    entry.text.clear();
    return static_cast<ScriptArrayId>(i + 1);
  }
  // A hole first, so that a released script's slots are reused before the pool
  // grows -- and so that the handle a save writes is the index it reads back.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].live) continue;
    entries_[i] = Entry{script, slot, true, strings, {}, {}};
    return static_cast<ScriptArrayId>(i + 1);
  }
  entries_.push_back(Entry{script, slot, true, strings, {}, {}});
  return static_cast<ScriptArrayId>(entries_.size());
}

void ArrayPool::release_script(script::ScriptId script) {
  for (Entry& entry : entries_) {
    if (entry.script != script) continue;
    entry.live = false;
    entry.script = script::kNoScript;
    // Cleared rather than left: a dead entry's contents are unreachable, and a
    // save that carried them would carry a script's scratch for the rest of the
    // match.
    entry.ints.clear();
    entry.text.clear();
  }
}

bool ArrayPool::contains(ScriptArrayId id) const noexcept {
  return id != kNoScriptArray && id <= entries_.size() && entries_[id - 1].live;
}

script::ScriptId ArrayPool::owner_of(ScriptArrayId id) const noexcept {
  if (!contains(id)) return script::kNoScript;
  return entries_[id - 1].script;
}

std::size_t ArrayPool::size(ScriptArrayId id) const noexcept {
  if (!contains(id)) return 0;
  const Entry& entry = entries_[id - 1];
  return entry.strings ? entry.text.size() : entry.ints.size();
}

script::Value ArrayPool::get(ScriptArrayId id, std::int32_t index) const {
  if (!contains(id)) return script::Value::integer(0);
  const Entry& entry = entries_[id - 1];
  const bool inside =
      index >= 0 && static_cast<std::size_t>(index) < (entry.strings ? entry.text.size()
                                                                     : entry.ints.size());
  if (entry.strings) {
    return script::Value::string(inside ? entry.text[static_cast<std::size_t>(index)]
                                        : std::string());
  }
  return script::Value::integer(inside ? entry.ints[static_cast<std::size_t>(index)] : 0);
}

bool ArrayPool::set(ScriptArrayId id, std::int32_t index, const script::Value& value) {
  if (!contains(id)) return false;
  // Refused, not clamped: there is no element -1 to write and no length that
  // could hold one. The ceiling is the header's, and it is what stops a typo
  // from becoming an allocation.
  if (index < 0 || index >= kMaxElements) return false;
  Entry& entry = entries_[id - 1];
  const auto want = static_cast<std::size_t>(index) + 1;
  if (entry.strings) {
    if (!value.is_string()) return false;
    if (entry.text.size() < want) entry.text.resize(want);
    entry.text[static_cast<std::size_t>(index)] = value.as_string();
    return true;
  }
  if (!value.is_integer()) return false;
  if (entry.ints.size() < want) entry.ints.resize(want, 0);
  entry.ints[static_cast<std::size_t>(index)] = value.as_integer();
  return true;
}

void ArrayPool::serialize(std::vector<std::byte>& out) const {
  bytes::put_u32(out, kPoolMagic);
  bytes::put_u32(out, kPoolVersion);
  // Every slot, in index order, dead ones included: the index *is* the handle
  // minus one, and where the holes are decides what the next acquire returns.
  bytes::put_u32(out, static_cast<std::uint32_t>(entries_.size()));
  for (const Entry& entry : entries_) {
    bytes::put_u32(out, entry.script);
    bytes::put_u32(out, entry.slot);
    bytes::put_u8(out, entry.live ? 1u : 0u);
    bytes::put_u8(out, entry.strings ? 1u : 0u);
    if (entry.strings) {
      bytes::put_u32(out, static_cast<std::uint32_t>(entry.text.size()));
      for (const std::string& item : entry.text) bytes::put_string(out, item);
    } else {
      bytes::put_u32(out, static_cast<std::uint32_t>(entry.ints.size()));
      for (const std::int32_t item : entry.ints) bytes::put_i32(out, item);
    }
  }
}

Status ArrayPool::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kPoolMagic) return FormatError::bad_magic;
  if (version != kPoolVersion) return FormatError::unsupported;
  if (!reader.u32(count)) return FormatError::truncated;

  // Decoded into a local and moved in only once every entry has read cleanly,
  // so a truncated save leaves the pool it was loading into untouched.
  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Entry entry;
    std::uint8_t live = 0;
    std::uint8_t strings = 0;
    std::uint32_t elements = 0;
    if (!reader.u32(entry.script) || !reader.u32(entry.slot) || !reader.u8(live) ||
        !reader.u8(strings) || !reader.u32(elements)) {
      return FormatError::truncated;
    }
    entry.live = live != 0;
    entry.strings = strings != 0;
    if (elements > static_cast<std::uint32_t>(kMaxElements)) return FormatError::malformed;
    if (entry.strings) {
      entry.text.reserve(elements);
      for (std::uint32_t e = 0; e < elements; ++e) {
        std::string item;
        if (!bytes::get_string(reader, item)) return FormatError::truncated;
        entry.text.push_back(std::move(item));
      }
    } else {
      entry.ints.reserve(elements);
      for (std::uint32_t e = 0; e < elements; ++e) {
        std::int32_t item = 0;
        if (!bytes::get_i32(reader, item)) return FormatError::truncated;
        entry.ints.push_back(item);
      }
    }
    entries.push_back(std::move(entry));
  }
  entries_ = std::move(entries);
  return Status();
}


}  // namespace imperivm::core::sim
