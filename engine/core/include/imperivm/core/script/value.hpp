#pragma once

// The value model the VS virtual machine operates on.
//
// VS has exactly three runtime shapes, and no more: a 32-bit signed integer, a
// string, and an opaque handle to something the host owns. `bool` is an integer
// (`tgt.SetExperience(tgt.experience + k + (chance > rand(99)))` adds a
// comparison result to an int, so the two are the same domain), and `point`,
// `rect`, `ObjList`, `Query`, `Settlement` and the twenty-odd handle types are
// all opaque as far as the language is concerned. What they can do lives in the
// host API, not here.
//
// **There is no floating point, and there will not be.** The engine is lockstep
// deterministic and the conformance harness compares per-tick state hashes, so
// one differently-rounded bit is a desync. `point` arithmetic in the corpus
// (`pt = ptBld + (sqLeader.pos - ptBld) * nSight / nDist`) is integer division
// on integer coordinates; the host implements it, and it stays integral.
//
// A `Value` is plain data on purpose. Suspended scripts are world state -- the
// original engine's desync dumps name a running script per object -- so every
// value that can sit in a local slot or on the operand stack has to serialise
// and hash. That rules out pointers into host memory; an object is a (type, id)
// pair the host resolves, which survives a save and reload.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace imperivm::core::script {

/// A host type's identity, assigned by the host when it registers its types.
///
/// `0` is reserved for "no type": an invalid handle carries it, which is what a
/// failed `AsHero()` yields and what `.IsValid` reports on.
using TypeId = std::uint16_t;

inline constexpr TypeId kNoType = 0;

/// A handle to an object the host owns.
///
/// Deliberately not a pointer. The id means whatever the host wants it to mean
/// (an entity index, a slot in a side table, a generation-tagged handle), and
/// the VM never dereferences it -- it only copies, compares and serialises it.
struct ObjectRef {
  TypeId type = kNoType;
  std::uint32_t id = 0;
  /// A second word, for a host type whose value does not fit one: a VS
  /// `point` is two 32-bit integers in `gbr.exe` (its operators pop eight
  /// bytes off the VM stack -- `0x00696e80`, `0x00696f00`, `0x00422630`), so
  /// the host carries `x` in `id` and `y` here. Zero for every handle.
  std::uint32_t aux = 0;

  [[nodiscard]] constexpr bool valid() const noexcept { return type != kNoType; }
  friend constexpr bool operator==(const ObjectRef&, const ObjectRef&) noexcept = default;
};

enum class ValueKind : std::uint8_t {
  nil,      ///< the result of a void call, and the value of an unset slot
  integer,
  string,
  object,
};

/// One VS runtime value.
///
/// Held by value everywhere, including in the operand stack and local slots.
/// The string payload is owned rather than interned so that a serialised frame
/// is self-contained; VS strings are short and the corpus builds few of them.
class Value {
 public:
  Value() = default;

  static Value nil() { return Value(); }
  static Value integer(std::int32_t v) {
    Value out;
    out.kind_ = ValueKind::integer;
    out.int_ = v;
    return out;
  }
  /// `true`/`false` are integers; this exists only so call sites read honestly.
  static Value boolean(bool v) { return integer(v ? 1 : 0); }
  static Value string(std::string v) {
    Value out;
    out.kind_ = ValueKind::string;
    out.str_ = std::move(v);
    return out;
  }
  static Value object(ObjectRef ref) {
    Value out;
    out.kind_ = ValueKind::object;
    out.obj_ = ref;
    return out;
  }
  static Value object(TypeId type, std::uint32_t id) { return object(ObjectRef{type, id}); }

  [[nodiscard]] ValueKind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_nil() const noexcept { return kind_ == ValueKind::nil; }
  [[nodiscard]] bool is_integer() const noexcept { return kind_ == ValueKind::integer; }
  [[nodiscard]] bool is_string() const noexcept { return kind_ == ValueKind::string; }
  [[nodiscard]] bool is_object() const noexcept { return kind_ == ValueKind::object; }

  [[nodiscard]] std::int32_t as_integer() const noexcept { return int_; }
  [[nodiscard]] const std::string& as_string() const noexcept { return str_; }
  [[nodiscard]] ObjectRef as_object() const noexcept { return obj_; }

  /// Truth for the two kinds the VM can decide by itself.
  ///
  /// An object's truthiness is a host question -- `if (setGIn.IsValid)` is
  /// explicit, but `while (!pt.InRect(rcMap) || !IsPassable3x3(pt))` bottoms out
  /// in host calls that return integers -- so `Host::truthy` gets the object
  /// case and this handles the rest.
  [[nodiscard]] bool truthy_scalar() const noexcept {
    switch (kind_) {
      case ValueKind::integer: return int_ != 0;
      case ValueKind::string: return !str_.empty();
      case ValueKind::nil: return false;
      case ValueKind::object: return obj_.valid();
    }
    return false;
  }

  friend bool operator==(const Value& a, const Value& b) noexcept {
    if (a.kind_ != b.kind_) return false;
    switch (a.kind_) {
      case ValueKind::nil: return true;
      case ValueKind::integer: return a.int_ == b.int_;
      case ValueKind::string: return a.str_ == b.str_;
      case ValueKind::object: return a.obj_ == b.obj_;
    }
    return false;
  }

 private:
  ValueKind kind_ = ValueKind::nil;
  std::int32_t int_ = 0;
  ObjectRef obj_;
  std::string str_;
};

/// The spelling used when an integer or a handle lands in a string concatenation.
///
/// `pr("Player " + AIPlayer + " builds new fancy " + nCount)` is 463 sites'
/// worth of idiom, so `+` with a string on either side stringifies the other
/// operand. Integers are rendered here; objects are the host's business and are
/// rendered by `Host::to_string`.
std::string to_decimal(std::int32_t value);

}  // namespace imperivm::core::script
