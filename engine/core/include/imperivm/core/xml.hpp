#pragma once

/// A freestanding XML reader for the engine's data files.
///
/// The game declares almost everything in XML: 845 class definitions, 889
/// entity definitions, map object lists, player setup. The core has to read all
/// of it, and the core may not take a dependency (see
/// docs/engine/architecture.md), so we parse it ourselves.
///
/// This is deliberately not a general XML implementation. It handles what the
/// shipped data actually uses — elements, attributes, nesting, comments,
/// processing instructions, CDATA, and the five predefined entities — and
/// rejects the rest rather than half-supporting it. No namespaces, no DTDs, no
/// validation, no schema. If a shipped file needs a feature listed as
/// unsupported, that is a finding worth documenting, not a reason to grow the
/// parser quietly.
///
/// **Zero copy.** Names and values are `std::string_view` into the caller's
/// buffer, so the document must outlive the tree. Attribute values containing
/// character references are the one exception: they need decoding, so the
/// document owns a small side buffer for those and the views point into it.
///
/// **Flat storage.** Nodes live in one vector and refer to each other by index,
/// not by pointer, so the tree is cheap to build, cheap to copy, and has no
/// ownership questions. `kNoNode` is the null index.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core {

using NodeIndex = std::uint32_t;

inline constexpr NodeIndex kNoNode = 0xFFFFFFFFu;

struct XmlAttribute {
  std::string_view name;
  std::string_view value;
};

/// One element. Text content is not modelled: no shipped file carries
/// meaningful character data, everything is in attributes. If that turns out to
/// be wrong for some file, add it deliberately rather than by accident.
struct XmlNode {
  std::string_view name;
  NodeIndex parent = kNoNode;
  NodeIndex first_child = kNoNode;
  NodeIndex next_sibling = kNoNode;
  std::uint32_t attribute_begin = 0;  ///< index into XmlDocument::attributes()
  std::uint32_t attribute_count = 0;
};

/// A parsed document.
///
/// Construct with `XmlDocument::parse`. A failed parse yields an error rather
/// than a partial tree, because a half-read class definition is worse than none.
class XmlDocument {
 public:
  static Result<XmlDocument> parse(std::span<const std::byte> text);

  [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

  /// The single top-level element. `kNoNode` only if the document is empty.
  [[nodiscard]] NodeIndex root() const noexcept { return root_; }

  [[nodiscard]] const XmlNode& node(NodeIndex index) const { return nodes_[index]; }
  [[nodiscard]] const std::vector<XmlAttribute>& attributes() const noexcept {
    return attributes_;
  }

  /// The value of `name` on `index`, or an empty view if absent. Attribute
  /// lookup is case-sensitive, matching the shipped data, which is consistent.
  [[nodiscard]] std::string_view attribute(NodeIndex index, std::string_view name) const;

  /// Attribute parsed as a signed integer, or `fallback` when absent or
  /// malformed. Malformed is treated as absent on purpose: the shipped data
  /// contains empty attributes such as `pass_file=""` that must not abort a load.
  [[nodiscard]] std::int32_t attribute_int(NodeIndex index, std::string_view name,
                                           std::int32_t fallback = 0) const;

  /// Attribute parsed as a boolean. Accepts `yes`/`no`, `true`/`false`, `1`/`0`,
  /// which the data uses interchangeably (`auto_repair="no"`, `xray="0"`).
  [[nodiscard]] bool attribute_bool(NodeIndex index, std::string_view name,
                                    bool fallback = false) const;

  /// First child of `index` named `name`, or `kNoNode`.
  [[nodiscard]] NodeIndex child(NodeIndex index, std::string_view name) const;

  /// Next sibling of `index` sharing its name, or `kNoNode`. With `child`, this
  /// is enough to walk repeated elements without allocating:
  ///
  ///     for (auto i = doc.child(root, "image"); i != kNoNode;
  ///          i = doc.next(i, "image")) { ... }
  [[nodiscard]] NodeIndex next(NodeIndex index, std::string_view name) const;

 private:
  std::vector<XmlNode> nodes_;
  std::vector<XmlAttribute> attributes_;
  /// Decoded attribute values, for the few that contain character references.
  /// Kept stable by reserving up front; views into it must not dangle.
  std::string decoded_;
  NodeIndex root_ = kNoNode;
};

}  // namespace imperivm::core
