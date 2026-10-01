// The class graph described in include/imperivm/core/game/class_graph.hpp.
//
// Ground truth for every rule here is src/imperivm/formats/gamedata.py, which
// is validated across the whole retail corpus. Where the two could disagree --
// merge order, what wins a collision, which references are checked -- this file
// follows it deliberately, and `imcheck classes` exists to prove they agree.

#include "imperivm/core/game/class_graph.hpp"

#include <algorithm>

#include "imperivm/core/xml.hpp"

namespace imperivm::core {
namespace {

/// Sorted-vector upsert, last writer wins. Used for `<properties>` and
/// `<sounds>`, which repeat within a class and merge attribute-wise, and for
/// the same merge applied down the inheritance chain.
void upsert(std::vector<ClassProperty>& bag, std::string_view key, std::string_view value) {
  const auto at = std::lower_bound(
      bag.begin(), bag.end(), key,
      [](const ClassProperty& entry, std::string_view needle) { return entry.key < needle; });
  if (at != bag.end() && at->key == key) {
    at->value = value;
    return;
  }
  bag.insert(at, ClassProperty{key, value});
}

void upsert(std::vector<ClassMethod>& table, const ClassMethod& method) {
  const auto at = std::lower_bound(
      table.begin(), table.end(), method.sig,
      [](const ClassMethod& entry, std::string_view needle) { return entry.sig < needle; });
  if (at != table.end() && at->sig == method.sig) {
    *at = method;
    return;
  }
  table.insert(at, method);
}

std::string join(std::string_view a, std::string_view b) {
  std::string out;
  out.reserve(a.size() + b.size());
  out.append(a);
  out.append(b);
  return out;
}

std::string join(std::string_view a, std::string_view b, std::string_view c) {
  std::string out = join(a, b);
  out.append(c);
  return out;
}

constexpr std::string_view kSeasonSuffix[4] = {"entity", "entity_spring", "entity_autumn",
                                               "entity_winter"};

constexpr std::string_view kClassReferenceProperties[] = {
    "projectile_class",   "projectile_explosion", "projectile_shadow",
    "projectile_fire",    "building_projectile_class",
    "sentry_class_name",  "select_class",         "defender_cls_1",
    "defender_cls_2",
};

}  // namespace

std::string_view ClassGraph::StringArena::intern(std::string_view text) {
  if (text.empty()) return {};
  for (auto& block : blocks_) {
    if (block.capacity() - block.size() >= text.size()) {
      const std::size_t offset = block.size();
      block.insert(block.end(), text.begin(), text.end());
      return std::string_view(block.data() + offset, text.size());
    }
  }
  blocks_.emplace_back();
  std::vector<char>& block = blocks_.back();
  // Reserved once and never exceeded, so the data pointer never moves. The
  // outer vector may reallocate; that copies the inner vector objects, not the
  // buffers they own, which is the whole reason for the nesting.
  block.reserve(text.size() > kBlockSize ? text.size() : kBlockSize);
  block.insert(block.end(), text.begin(), text.end());
  return std::string_view(block.data(), text.size());
}

std::span<const std::string_view> ClassGraph::class_reference_properties() noexcept {
  return kClassReferenceProperties;
}

Status ClassGraph::add(std::span<const std::byte> document, std::string_view source) {
  auto parsed = XmlDocument::parse(document);
  if (!parsed) return parsed.error();
  const XmlDocument& doc = *parsed;

  const NodeIndex root = doc.root();
  if (doc.node(root).name != "class") return FormatError::malformed;

  const std::string_view id = doc.attribute(root, "id");
  const std::string_view cpp_class = doc.attribute(root, "cpp_class");
  if (id.empty() || cpp_class.empty()) return FormatError::malformed;

  ClassDefinition definition;
  definition.id = arena_.intern(id);
  definition.cpp_class = arena_.intern(cpp_class);
  definition.parent = arena_.intern(doc.attribute(root, "parent"));
  definition.altid = arena_.intern(doc.attribute(root, "altid"));
  definition.source = arena_.intern(source);
  for (std::size_t season = 0; season < 4; ++season) {
    definition.seasonal[season] = arena_.intern(doc.attribute(root, kSeasonSuffix[season]));
  }

  for (NodeIndex child = doc.node(root).first_child; child != kNoNode;
       child = doc.node(child).next_sibling) {
    const XmlNode& element = doc.node(child);
    const std::string_view tag = element.name;

    if (tag == "properties" || tag == "sounds") {
      std::vector<ClassProperty>& bag = tag == "properties" ? definition.properties
                                                            : definition.sounds;
      for (std::uint32_t i = 0; i < element.attribute_count; ++i) {
        const XmlAttribute& attribute = doc.attributes()[element.attribute_begin + i];
        upsert(bag, arena_.intern(attribute.name), arena_.intern(attribute.value));
      }
    } else if (tag == "method") {
      const std::string_view sig = doc.attribute(child, "sig");
      // A `<method>` with no `sig` cannot be bound to anything and cannot
      // collide with anything: dropping it loses nothing, and refusing the
      // whole class over it would lose a great deal.
      if (sig.empty()) continue;
      upsert(definition.methods, ClassMethod{arena_.intern(sig),
                                             arena_.intern(doc.attribute(child, "vs")),
                                             arena_.intern(doc.attribute(child, "verify")),
                                             arena_.intern(doc.attribute(child, "onfinish"))});
    } else if (tag == "behavior") {
      const std::string_view script = doc.attribute(child, "script");
      if (!script.empty()) definition.behaviors.push_back(arena_.intern(script));
    } else if (tag == "nodefcmdinherit") {
      definition.no_defcmd_inherit = true;
    } else if (tag == "defaultcmd") {
      DefaultCommandBlock block;
      block.target = arena_.intern(doc.attribute(child, "target"));
      for (NodeIndex cmd = doc.child(child, "cmd"); cmd != kNoNode;
           cmd = doc.next(cmd, "cmd")) {
        block.cmds.push_back(DefaultCommand{arena_.intern(doc.attribute(cmd, "name")),
                                            doc.attribute(cmd, "ctrl") == "1"});
      }
      definition.default_cmds.push_back(std::move(block));
    } else if (tag.size() == 6 && tag.starts_with("value") && tag[5] >= '0' && tag[5] <= '5') {
      InfoValue& value = definition.values[static_cast<std::size_t>(tag[5] - '0')];
      value.present = true;
      value.icon = arena_.intern(doc.attribute(child, "icon"));
      value.script = arena_.intern(doc.attribute(child, "script"));
      value.rollover = arena_.intern(doc.attribute(child, "rollover"));
      value.help = arena_.intern(doc.attribute(child, "help"));
      value.flags = arena_.intern(doc.attribute(child, "flags"));
    }
  }

  const ClassIndex existing = find(definition.id);
  if (existing != kNoClass) {
    // The first declaration keeps the name. Reported, not fatal: a second file
    // claiming an id is a packaging mistake, not a reason to lose 844 classes.
    duplicate_ids_.emplace_back(definition.id, definition.source);
    return {};
  }

  const ClassIndex index = static_cast<ClassIndex>(classes_.size());
  classes_.push_back(std::move(definition));

  const std::string_view key = classes_[index].id;
  by_id_.insert(std::lower_bound(by_id_.begin(), by_id_.end(), key,
                                 [this](ClassIndex candidate, std::string_view needle) {
                                   return classes_[candidate].id < needle;
                                 }),
                index);

  if (!classes_[index].altid.empty()) {
    const std::string_view alias = classes_[index].altid;
    const auto at = std::lower_bound(by_altid_.begin(), by_altid_.end(), alias,
                                     [this](ClassIndex candidate, std::string_view needle) {
                                       return classes_[candidate].altid < needle;
                                     });
    // First registration wins, matching the reference reader's `setdefault`.
    if (at == by_altid_.end() || classes_[*at].altid != alias) by_altid_.insert(at, index);
  }
  return {};
}

void ClassGraph::link() {
  roots_.clear();
  max_depth_ = 0;
  for (ClassDefinition& definition : classes_) {
    definition.children.clear();
    definition.depth = 0;
    definition.parent_index = kNoClass;
  }

  // Walking in id order makes every child list, and the root list, sorted by
  // id without a second pass. Iteration order is world state; it is not left
  // to the order the files happened to arrive in.
  for (const ClassIndex index : by_id_) {
    ClassDefinition& definition = classes_[index];
    const ClassIndex parent =
        definition.parent.empty() ? kNoClass : find(definition.parent);
    definition.parent_index = parent;
    if (parent == kNoClass) {
      // Includes a `parent` that names nothing. validate() says so; the class
      // still loads, as its own root.
      roots_.push_back(index);
    } else {
      classes_[parent].children.push_back(index);
    }
  }

  std::vector<ClassIndex> stack(roots_.rbegin(), roots_.rend());
  for (const ClassIndex root : roots_) classes_[root].depth = 1;
  while (!stack.empty()) {
    const ClassIndex index = stack.back();
    stack.pop_back();
    const std::uint32_t depth = classes_[index].depth;
    if (depth > max_depth_) max_depth_ = depth;
    for (const ClassIndex child : classes_[index].children) {
      classes_[child].depth = depth + 1;
      stack.push_back(child);
    }
  }
  // Anything still at depth 0 is unreachable from a root, which for a graph
  // where every node has at most one parent means a cycle.
}

ClassIndex ClassGraph::find(std::string_view id) const {
  const auto at = std::lower_bound(by_id_.begin(), by_id_.end(), id,
                                   [this](ClassIndex candidate, std::string_view needle) {
                                     return classes_[candidate].id < needle;
                                   });
  if (at == by_id_.end() || classes_[*at].id != id) return kNoClass;
  return *at;
}

ClassIndex ClassGraph::lookup(std::string_view name) const {
  const ClassIndex direct = find(name);
  if (direct != kNoClass) return direct;
  const auto at = std::lower_bound(by_altid_.begin(), by_altid_.end(), name,
                                   [this](ClassIndex candidate, std::string_view needle) {
                                     return classes_[candidate].altid < needle;
                                   });
  if (at == by_altid_.end() || classes_[*at].altid != name) return kNoClass;
  return *at;
}

std::vector<ClassIndex> ClassGraph::ancestry(ClassIndex index) const {
  std::vector<ClassIndex> chain;
  for (ClassIndex current = index; current != kNoClass;
       current = classes_[current].parent_index) {
    if (std::find(chain.begin(), chain.end(), current) != chain.end()) break;
    chain.push_back(current);
  }
  return chain;
}

std::size_t ClassGraph::subtree_size(ClassIndex index) const {
  std::size_t total = 0;
  std::vector<ClassIndex> stack{index};
  while (!stack.empty()) {
    const ClassIndex current = stack.back();
    stack.pop_back();
    ++total;
    for (const ClassIndex child : classes_[current].children) stack.push_back(child);
  }
  return total;
}

std::vector<ClassProperty> ClassGraph::resolved_properties(ClassIndex index) const {
  std::vector<ClassProperty> resolved;
  const std::vector<ClassIndex> chain = ancestry(index);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const ClassProperty& property : classes_[*it].properties) {
      upsert(resolved, property.key, property.value);
    }
  }
  return resolved;
}

std::vector<ClassProperty> ClassGraph::resolved_sounds(ClassIndex index) const {
  std::vector<ClassProperty> resolved;
  const std::vector<ClassIndex> chain = ancestry(index);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const ClassProperty& sound : classes_[*it].sounds) {
      upsert(resolved, sound.key, sound.value);
    }
  }
  return resolved;
}

std::vector<ClassMethod> ClassGraph::resolved_methods(ClassIndex index) const {
  std::vector<ClassMethod> resolved;
  const std::vector<ClassIndex> chain = ancestry(index);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const ClassMethod& method : classes_[*it].methods) upsert(resolved, method);
  }
  return resolved;
}

std::vector<std::string_view> ClassGraph::resolved_behaviors(ClassIndex index) const {
  std::vector<std::string_view> scripts;
  // The class's own first, then everything its parent resolved to, and no
  // duplicate dropped: the loader appends a class's own `<behavior>`s as it
  // parses them (0x005a046b) and only afterwards, once every file is read,
  // appends its parent's whole list behind them (0x005a5fd0, recursive, so the
  // parent's list is itself own-then-inherited). Both stop at eight.
  for (const ClassIndex link : ancestry(index)) {
    for (const std::string_view script : classes_[link].behaviors) {
      if (scripts.size() >= kMaxBehaviors) return scripts;
      scripts.push_back(script);
    }
  }
  return scripts;
}

std::vector<DefaultCommandBlock> ClassGraph::resolved_default_cmds(ClassIndex index) const {
  std::vector<DefaultCommandBlock> blocks;
  const std::vector<ClassIndex> chain = ancestry(index);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const ClassDefinition& definition = classes_[*it];
    if (definition.no_defcmd_inherit) blocks.clear();
    for (const DefaultCommandBlock& block : definition.default_cmds) {
      const auto at = std::find_if(
          blocks.begin(), blocks.end(),
          [&](const DefaultCommandBlock& seen) { return seen.target == block.target; });
      // Replacement in place, keeping the position the target was first seen
      // at, because that is what a dict update does in the reference reader
      // and the order of the table is the order the engine offers commands in.
      if (at != blocks.end()) {
        *at = block;
      } else {
        blocks.push_back(block);
      }
    }
  }
  return blocks;
}

std::array<InfoValue, 6> ClassGraph::resolved_values(ClassIndex index) const {
  std::array<InfoValue, 6> resolved{};
  const std::vector<ClassIndex> chain = ancestry(index);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (std::size_t slot = 0; slot < resolved.size(); ++slot) {
      if (classes_[*it].values[slot].present) resolved[slot] = classes_[*it].values[slot];
    }
  }
  return resolved;
}

std::string_view ClassGraph::property(ClassIndex index, std::string_view key) const {
  for (ClassIndex current = index; current != kNoClass;
       current = classes_[current].parent_index) {
    const std::vector<ClassProperty>& bag = classes_[current].properties;
    const auto at = std::lower_bound(
        bag.begin(), bag.end(), key,
        [](const ClassProperty& entry, std::string_view needle) { return entry.key < needle; });
    if (at != bag.end() && at->key == key) return at->value;
    if (classes_[current].parent_index == current) break;  // defensive: self-parent
  }
  return {};
}

std::string_view ClassGraph::entity_path(ClassIndex index, Season season) const {
  const ClassDefinition& definition = classes_[index];
  const std::string_view seasonal = definition.seasonal[static_cast<std::size_t>(season)];
  if (!seasonal.empty()) return seasonal;
  return definition.seasonal[static_cast<std::size_t>(Season::summer)];
}

std::vector<ClassIssue> ClassGraph::validate(const ResourceProbe* probe) const {
  std::vector<ClassIssue> issues;

  for (const auto& [id, source] : duplicate_ids_) {
    issues.push_back({ClassIssueKind::duplicate_id, source, join("id=", id)});
  }

  for (const ClassIndex index : by_id_) {
    const ClassDefinition& definition = classes_[index];

    if (!definition.parent.empty() && definition.parent_index == kNoClass) {
      issues.push_back(
          {ClassIssueKind::dangling_parent, definition.source, join("parent=", definition.parent)});
    } else if (definition.depth == 0) {
      issues.push_back({ClassIssueKind::cycle, definition.source, join("id=", definition.id)});
    }

    if (!definition.altid.empty() && lookup(definition.altid) != index) {
      issues.push_back(
          {ClassIssueKind::altid_collision, definition.source, join("altid=", definition.altid)});
    }

    for (const std::string_view key : kClassReferenceProperties) {
      const std::string_view value = [&] {
        const auto at = std::lower_bound(definition.properties.begin(),
                                         definition.properties.end(), key,
                                         [](const ClassProperty& entry, std::string_view needle) {
                                           return entry.key < needle;
                                         });
        return (at != definition.properties.end() && at->key == key) ? at->value
                                                                     : std::string_view{};
      }();
      if (!value.empty() && lookup(value) == kNoClass) {
        issues.push_back(
            {ClassIssueKind::dangling_class_ref, definition.source, join(key, "=", value)});
      }
    }

    if (probe == nullptr) continue;

    for (std::size_t season = 0; season < definition.seasonal.size(); ++season) {
      const std::string_view path = definition.seasonal[season];
      if (path.empty() || probe->exists(path)) continue;
      issues.push_back(
          {ClassIssueKind::missing_entity, definition.source, join(kSeasonSuffix[season], "=", path)});
    }

    for (const ClassMethod& method : definition.methods) {
      const std::pair<std::string_view, std::string_view> bound[] = {
          {"vs", method.vs}, {"verify", method.verify}, {"onfinish", method.onfinish}};
      for (const auto& [label, path] : bound) {
        if (path.empty() || probe->exists(path)) continue;
        std::string detail = join(method.sig, "/", label);
        detail.append("=");
        detail.append(path);
        issues.push_back({ClassIssueKind::missing_script, definition.source, std::move(detail)});
      }
    }

    for (const std::string_view script : definition.behaviors) {
      if (probe->exists(script)) continue;
      issues.push_back({ClassIssueKind::missing_script, definition.source, join("behavior=", script)});
    }

    for (const ClassProperty& sound : definition.sounds) {
      if (sound.value.empty() || probe->sound_exists(sound.value)) continue;
      issues.push_back(
          {ClassIssueKind::missing_sound, definition.source, join(sound.key, "=", sound.value)});
    }

    // Icons are checked on the class's *own* properties, not the resolved bag:
    // an inherited icon is the ancestor's problem and would otherwise be
    // reported once per descendant.
    std::vector<std::string_view> icons;
    const auto icon = std::lower_bound(
        definition.properties.begin(), definition.properties.end(), std::string_view("icon"),
        [](const ClassProperty& entry, std::string_view needle) { return entry.key < needle; });
    if (icon != definition.properties.end() && icon->key == "icon") icons.push_back(icon->value);
    for (const InfoValue& value : definition.values) {
      if (value.present && !value.icon.empty()) icons.push_back(value.icon);
    }
    for (const std::string_view path : icons) {
      if (path.empty() || probe->exists(path)) continue;
      issues.push_back({ClassIssueKind::missing_icon, definition.source, join("icon=", path)});
    }
  }

  return issues;
}

}  // namespace imperivm::core
