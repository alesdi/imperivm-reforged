#pragma once

/// Editing a shipped XML document in place, byte for byte around the edit.
///
/// The container's small documents -- `game.xml`, `player<i>.xml`,
/// `map.xml`, `Notes.xml`, `sequences.xml` -- are one element deep or two,
/// everything in attributes, laid out one attribute per line with tab
/// indentation. The editor changes one attribute of one element and the
/// original writes the whole document back from its own tables, in the
/// same shape. Rather than a writer per document that would have to
/// reproduce every attribute this engine does not read, this patches the
/// text: an attribute's value is rewritten where it stands, a new attribute
/// goes on its own line after the last with the same indentation, an
/// element is cut out with the whitespace before it, and a new element is
/// laid in before its parent's closing tag in the shape of its siblings.
/// What is not touched is not rewritten, so a document the editor never
/// edited comes back unchanged.
///
/// The target is a path of element names from the root, `map/expl`, with an
/// optional `[key=value]` on any step to pick one of several siblings
/// (`notes/note[id=GOAL]`, `items/item[id=X]/bonus`) or `[n]` for the n-th
/// of that name (`conversation/phrase[2]`); neither picks the first. Values are escaped the
/// way `write_map_objects` escapes them (`&`, `<`, `"`).

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace imperivm::core {

using XmlEdit = std::pair<std::string_view, std::string_view>;  ///< attribute, value

/// Set `edits` on the element at `path`. Returns the patched document, or
/// the document unchanged with `*found` false when no element matches.
[[nodiscard]] std::string xml_set_attributes(std::string_view document, std::string_view path,
                                             std::span<const XmlEdit> edits, bool* found = nullptr);

/// Cut the element at `path` out -- its opening tag through its closing tag
/// or `/>`, and the whitespace before it.
[[nodiscard]] std::string xml_erase_element(std::string_view document, std::string_view path,
                                            bool* found = nullptr);

/// Lay a new self-closing `<name attr="v" .../>` in as the last child of the
/// element at `parent_path`, indented one level deeper than the parent's
/// closing tag, one attribute per line as the shipped documents are.
[[nodiscard]] std::string xml_append_element(std::string_view document, std::string_view parent_path,
                                             std::string_view name, std::span<const XmlEdit> attributes,
                                             bool* found = nullptr);

/// The same, with one self-closing child laid inside the new element --
/// an item's `<bonus/>` -- so the element opens, holds the child one level
/// deeper, and closes on its own line, as the shipped documents shape it.
[[nodiscard]] std::string xml_append_element(std::string_view document, std::string_view parent_path,
                                             std::string_view name, std::span<const XmlEdit> attributes,
                                             std::string_view child_name, std::span<const XmlEdit> child_attributes,
                                             bool* found = nullptr);

/// Exchange the two elements' places -- a phrase moved up or down.
[[nodiscard]] std::string xml_swap_elements(std::string_view document, std::string_view path_a,
                                            std::string_view path_b, bool* found = nullptr);

/// The value of one attribute at `path` as it stands in the text (character
/// references not decoded), or empty.
[[nodiscard]] std::string xml_get_attribute(std::string_view document, std::string_view path,
                                            std::string_view attribute);

/// How many elements `path` names when its last step is read as `name[n]`
/// for n = 0, 1, ...: the labels of a `root/label`, the phrases of a
/// `conversation/phrase`. Zero when the parent is missing.
[[nodiscard]] std::size_t xml_count_elements(std::string_view document, std::string_view path);

/// An attribute's value with its character references decoded -- `&lt;`,
/// `&gt;`, `&amp;`, `&quot;`, `&apos;` and `&#n;` -- for showing it; the
/// patcher writes the escaped form back.
[[nodiscard]] std::string xml_decode_entities(std::string_view value);

}  // namespace imperivm::core
