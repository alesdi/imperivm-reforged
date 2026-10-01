#include "imperivm/core/script/host.hpp"

#include <algorithm>
#include <utility>

namespace imperivm::core::script {
namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

Host::~Host() = default;

Value Host::default_value(std::string_view type_name) {
  // The two the language owns. Everything else -- `point`, `rect`, `ObjList`,
  // `Query`, and the twenty handle types -- is the host's to mint, and a host
  // that has not got round to a type gets a nil rather than a wrong zero.
  if (type_name == "int" || type_name == "bool") return Value::integer(0);
  if (type_name == "str") return Value::string(std::string());
  return Value::nil();
}

Value Host::default_value(std::string_view type_name, DeclarationSite site) {
  // Forwarding by default keeps the overload additive: a host that does not
  // pool anything never has to know the site exists.
  (void)site;
  return default_value(type_name);
}

Value Host::clone_for_assign(const Value& value) { return value; }

Result<bool> Host::truthy(const Value& value) { return value.as_object().valid(); }

Result<Value> Host::binary(BinaryOp, const Value&, const Value&) {
  return FormatError::unsupported;
}

Result<Value> Host::index_get(const Value&, const Value&) { return FormatError::unsupported; }

Status Host::index_set(Value&, const Value&, const Value&) { return FormatError::unsupported; }

Result<Value> Host::global(std::string_view) { return FormatError::not_found; }

Result<std::string> Host::to_string(const Value&) { return FormatError::unsupported; }

// -- registry ---------------------------------------------------------------

std::string HostRegistry::key_for(CallKind kind, std::string_view name) {
  std::string key(name);
  if (kind == CallKind::member) {
    // Member lookup is case-insensitive because the corpus is: the same host
    // entry point is spelled `GetGAIKA` on `point` and `GetGaika` on
    // `Settlement`. Free functions and locals stay case-sensitive, which
    // `OUTPOST_IDLE.VS` requires by keeping `This` and `this` live at once.
    for (char& c : key) c = lower(c);
  }
  return key;
}

void HostRegistry::reindex() const {
  order_.resize(entries_.size());
  for (std::uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
  std::sort(order_.begin(), order_.end(), [this](std::uint32_t a, std::uint32_t b) {
    const HostEntry& left = entries_[a];
    const HostEntry& right = entries_[b];
    if (left.kind != right.kind) return left.kind < right.kind;
    if (left.key != right.key) return left.key < right.key;
    return left.arity < right.arity;
  });
  dirty_ = false;
}

std::uint32_t HostRegistry::find(CallKind kind, std::string_view name,
                                 std::uint16_t arity) const {
  if (dirty_) reindex();
  const std::string key = key_for(kind, name);

  std::size_t low = 0;
  std::size_t high = order_.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    const HostEntry& entry = entries_[order_[mid]];
    bool less = false;
    if (entry.kind != kind) {
      less = entry.kind < kind;
    } else if (entry.key != key) {
      less = entry.key < key;
    } else {
      less = entry.arity < arity;
    }
    if (less) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low >= order_.size()) return kUnresolvedHost;
  const HostEntry& candidate = entries_[order_[low]];
  if (candidate.kind != kind || candidate.arity != arity) return kUnresolvedHost;
  if (candidate.key != key) return kUnresolvedHost;
  return order_[low];
}

std::uint32_t HostRegistry::declare(CallKind kind, std::string_view name, std::uint16_t arity) {
  // The signature this entry would answer to, in the same three parts `find`
  // compares: the kind, the case-folded key, and the arity.
  std::string signature = key_for(kind, name);
  signature.push_back('\0');
  signature.push_back(static_cast<char>(static_cast<int>(kind)));
  signature.push_back(static_cast<char>(arity & 0xFF));
  signature.push_back(static_cast<char>((arity >> 8) & 0xFF));

  // **Not `find`**, which would re-sort the whole table -- see
  // `by_signature_` in the header for what that cost.
  const auto seen = by_signature_.find(signature);
  if (seen != by_signature_.end()) return seen->second;

  entries_.push_back(HostEntry{kind, std::string(name), key_for(kind, name), arity, nullptr});
  const auto index = static_cast<std::uint32_t>(entries_.size() - 1);
  by_signature_.emplace(std::move(signature), index);
  dirty_ = true;
  return index;
}

std::uint32_t HostRegistry::define(CallKind kind, std::string_view name, std::uint16_t arity,
                                   HostFn fn) {
  const std::uint32_t index = declare(kind, name, arity);
  entries_[index].fn = fn;
  return index;
}

std::size_t HostRegistry::implemented() const {
  std::size_t count = 0;
  for (const HostEntry& entry : entries_) {
    if (entry.fn != nullptr) ++count;
  }
  return count;
}

}  // namespace imperivm::core::script
