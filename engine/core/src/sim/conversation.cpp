// The conversation catalogue: what a `.conv.xml` declares.
// See include/imperivm/core/sim/conversation.hpp.

#include "imperivm/core/sim/conversation.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/script/vm.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core::sim {

namespace {

/// The seven names, in the executable's own order, so that the enumerator's
/// value is its index.
constexpr std::array<std::string_view, 7> kModeNames = {
    "choice", "random", "first", "end", "cycle", "cycle then first", "cycle then random",
};

[[nodiscard]] bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) ++begin;
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

/// Does this snippet already read as a statement list?
///
/// The two shapes a `condition` or a `return` comes in are told apart by
/// grammar and nothing else. A statement list either ends a statement or opens
/// with a keyword that cannot start an expression; a bare expression does
/// neither. `;` alone would be enough for all 35 shipped snippets -- every one
/// of the bodies has one and none of the bare ones does -- but a body could in
/// principle be a single `if` with no trailing semicolon, so the keywords are
/// checked too.
[[nodiscard]] bool is_statement_list(std::string_view source) noexcept {
  if (source.find(';') != std::string_view::npos) return true;
  const std::string_view head = trim(source);
  for (const std::string_view keyword : {"if", "for", "while", "return", "int", "str", "bool"}) {
    if (head.size() > keyword.size() && head.starts_with(keyword) &&
        (is_space(head[keyword.size()]) || head[keyword.size()] == '(')) {
      return true;
    }
  }
  return false;
}

}  // namespace

// --------------------------------------------------------------------------
// modes
// --------------------------------------------------------------------------

Result<ConversationMode> parse_conversation_mode(std::string_view text) {
  // The executable tests for the empty string explicitly, between `first` and
  // `cycle`, and answers `first`. An absent attribute reaches here as empty
  // too, which is the same answer and the same reason.
  if (text.empty()) return ConversationMode::first;
  for (std::size_t i = 0; i < kModeNames.size(); ++i) {
    if (text == kModeNames[i]) return static_cast<ConversationMode>(i);
  }
  // Refused rather than defaulted. A mode this reader does not know would be
  // read as `first` and play the same line for ever, which is a mission that
  // never advances and no diagnostic anywhere.
  return FormatError::malformed;
}

std::string_view conversation_mode_name(ConversationMode mode) noexcept {
  const auto index = static_cast<std::size_t>(mode);
  return index < kModeNames.size() ? kModeNames[index] : std::string_view{};
}

// --------------------------------------------------------------------------
// escapes
// --------------------------------------------------------------------------

std::string decode_conversation_escapes(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 >= text.size()) {
      out.push_back(text[i]);
      continue;
    }
    switch (text[i + 1]) {
      case 'n': out.push_back('\n'); ++i; break;
      case 'l': out.push_back('<'); ++i; break;
      case 'g': out.push_back('>'); ++i; break;
      case 'a': out.push_back('&'); ++i; break;
      case '\'': out.push_back('\''); ++i; break;
      // Anything else keeps the backslash. `\D` in a path is the likelier
      // reading than an escape this reader has not heard of, and swallowing
      // the separator would be the worse mistake.
      default: out.push_back('\\'); break;
    }
  }
  return out;
}

// --------------------------------------------------------------------------
// snippets
// --------------------------------------------------------------------------

std::string ConversationSnippet::wrap(std::string_view result_type) const {
  std::string out = "//";
  out += result_type;
  out.push_back('\n');
  if (source.empty()) return out;
  if (is_statement_list(source)) {
    out += source;
    out.push_back('\n');
    return out;
  }
  out += "return ";
  out += source;
  out += ";\n";
  return out;
}

// --------------------------------------------------------------------------
// lists
// --------------------------------------------------------------------------

std::vector<std::string> split_phrase_list(std::string_view text) {
  std::vector<std::string> out;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t at = text.find(';', begin);
    const std::size_t end = at == std::string_view::npos ? text.size() : at;
    const std::string_view field = trim(text.substr(begin, end - begin));
    // Dropped rather than kept. `"begining;"` is a one-element list -- the
    // authoring tool writes the separator after every entry -- and an empty
    // label would match no phrase and stall the conversation on it.
    if (!field.empty()) out.emplace_back(field);
    if (at == std::string_view::npos) break;
    begin = at + 1;
  }
  return out;
}

// --------------------------------------------------------------------------
// the catalogue
// --------------------------------------------------------------------------

std::int32_t ConversationDefinition::find(std::string_view label) const noexcept {
  // The empty label names nothing. 194 of the 260 shipped phrases carry no
  // `label` at all, so a lookup that matched the empty string would answer
  // with whichever of them came first -- and `split_phrase_list` drops empty
  // fields precisely so that no list can ask for it.
  if (label.empty()) return -1;
  for (std::size_t i = 0; i < phrases.size(); ++i) {
    if (phrases[i].label == label) return static_cast<std::int32_t>(i);
  }
  return -1;
}

namespace {

/// Read one `<conversation>` element into a definition, or say why not.
[[nodiscard]] Result<ConversationDefinition> read_conversation(const XmlDocument& doc,
                                                              NodeIndex node) {
  ConversationDefinition out;
  out.name = std::string(doc.attribute(node, "name"));

  auto startup = parse_conversation_mode(doc.attribute(node, "startup"));
  if (!startup) return startup.error();
  out.startup = startup.value();
  out.startup_phrases = split_phrase_list(doc.attribute(node, "startup_phrases"));
  out.restore_view = doc.attribute_int(node, "restore_view", 0) != 0;

  for (NodeIndex i = doc.child(node, "actor"); i != kNoNode; i = doc.next(i, "actor")) {
    const std::string_view name = doc.attribute(i, "name");
    // An actor with no name is a role nothing could bind; `SetActor` takes the
    // name as its key.
    if (!name.empty()) out.actors.emplace_back(name);
  }

  for (NodeIndex i = doc.child(node, "phrase"); i != kNoNode; i = doc.next(i, "phrase")) {
    ConversationPhrase phrase;
    phrase.label = std::string(doc.attribute(i, "label"));
    phrase.actor = std::string(doc.attribute(i, "actor"));
    // Display strings stay as written; see the header.
    phrase.text = std::string(doc.attribute(i, "text"));
    phrase.choice_text = std::string(doc.attribute(i, "choice_text"));
    // Source strings do not: `\l` is a `<` and the compiler needs it back.
    phrase.condition.source = decode_conversation_escapes(doc.attribute(i, "condition"));
    phrase.action.source = decode_conversation_escapes(doc.attribute(i, "action"));
    phrase.result.source = decode_conversation_escapes(doc.attribute(i, "return"));

    auto followup = parse_conversation_mode(doc.attribute(i, "followup"));
    if (!followup) return followup.error();
    phrase.followup = followup.value();
    phrase.followup_phrases = split_phrase_list(doc.attribute(i, "followup_phrases"));

    // A repeated label would make `followup_phrases` ambiguous. It does not
    // happen in any of the 110 shipped documents -- every one has unique
    // labels -- so a document where it does is one this reader does not
    // understand, not one to resolve by taking the first.
    if (!phrase.label.empty() && out.find(phrase.label) >= 0) return FormatError::malformed;

    out.phrases.push_back(std::move(phrase));
  }
  return out;
}

}  // namespace

Result<std::size_t> ConversationCatalogue::add(std::span<const std::byte> xml) {
  // Empty is not an error: six of the twenty-four containers declare no
  // conversations at all.
  if (xml.empty()) return std::size_t{0};

  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode) return FormatError::bad_magic;

  std::size_t added = 0;

  // The element is looked for rather than assumed. Every shipped document *is*
  // a bare `<conversation>`, but a wrapper costs one branch to support and a
  // reader that insisted on the bare form would refuse the whole container.
  const auto take = [&](NodeIndex node) -> Status {
    // Skipped, not refused: a conversation with no name could never be called,
    // and a document that also holds a good one must still yield it. Same rule
    // `NoteCatalogue` applies to a `<note>` with no `id`.
    if (doc->attribute(node, "name").empty()) return Status{};
    auto parsed = read_conversation(*doc, node);
    if (!parsed) return parsed.error();
    // First in wins, as a `std::map` insert does with a key it holds.
    if (find(parsed->name) != nullptr) return Status{};
    conversations_.push_back(std::move(parsed.value()));
    ++added;
    return Status{};
  };

  if (doc->node(root).name == "conversation") {
    const Status status = take(root);
    if (!status.ok()) return status.error();
    return added;
  }
  for (NodeIndex i = doc->child(root, "conversation"); i != kNoNode;
       i = doc->next(i, "conversation")) {
    const Status status = take(i);
    if (!status.ok()) return status.error();
  }
  return added;
}

// ==========================================================================
// the runtime
// ==========================================================================

namespace {

constexpr std::uint32_t kConversationPoolMagic = 0x564E4F43u;  // "CONV"
constexpr std::uint32_t kConversationPoolVersion = 1;
constexpr std::uint32_t kConversationResultsMagic = 0x53455243u;  // "CRES"
constexpr std::uint32_t kConversationResultsVersion = 1;

/// A bound big enough for anything the data can reach and small enough that a
/// corrupt length cannot make a load allocate the machine away. The largest
/// shipped conversation declares three actors.
constexpr std::uint32_t kMaxConversationActors = 64;

constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix(std::uint64_t& accumulator, std::string_view text) noexcept {
  for (const char c : text) {
    accumulator ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    accumulator *= kFnvPrime;
  }
  accumulator ^= 0xFFull;
  accumulator *= kFnvPrime;
}

}  // namespace

script::Value make_conversation_value(ConversationId id) noexcept {
  return script::Value::object(script::ObjectRef{kTypeConversation, id});
}

bool is_conversation(const script::Value& value) noexcept {
  return value.is_object() && value.as_object().type == kTypeConversation;
}

ConversationId conversation_of(const script::Value& value) noexcept {
  return is_conversation(value) ? value.as_object().id : kNoConversation;
}

ConversationPool& conversation_pool_of(World& world) { return world.conversations(); }

// --------------------------------------------------------------------------
// the pool
// --------------------------------------------------------------------------

ConversationId ConversationPool::acquire(script::ScriptId script, std::uint32_t slot) {
  // The same site always names the same entry -- `SquadListPool::acquire`'s
  // rule and `ArrayPool::acquire`'s, for the reason `WorldHost::default_value`
  // gives about a declaration inside a loop body.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = entries_[i];
    if (entry.script != script || entry.slot != slot) continue;
    entry.live = true;
    entry.name.clear();
    entry.actors.clear();
    return static_cast<ConversationId>(i + 1);
  }
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].live) continue;
    entries_[i] = Entry{script, slot, true, {}, {}};
    return static_cast<ConversationId>(i + 1);
  }
  entries_.push_back(Entry{script, slot, true, {}, {}});
  return static_cast<ConversationId>(entries_.size());
}

void ConversationPool::release_script(script::ScriptId script) {
  for (Entry& entry : entries_) {
    if (entry.script != script) continue;
    entry.live = false;
    entry.name.clear();
    entry.name.shrink_to_fit();
    entry.actors.clear();
    entry.actors.shrink_to_fit();
  }
}

bool ConversationPool::contains(ConversationId id) const noexcept {
  return id != kNoConversation && id <= entries_.size() && entries_[id - 1].live;
}

script::ScriptId ConversationPool::owner_of(ConversationId id) const noexcept {
  if (!contains(id)) return script::kNoScript;
  return entries_[id - 1].script;
}

bool ConversationPool::bind(ConversationId id, std::string_view name) {
  if (!contains(id)) return false;
  Entry& entry = entries_[id - 1];
  entry.name = std::string(name);
  // **Cleared, not kept.** A script that runs six conversations from one local
  // calls `Init` before each and binds only the roles that one needs; carrying
  // the previous conversation's actors over would leave a role bound to a unit
  // the new conversation never mentions, and `ActorPresent` would answer for a
  // conversation that is not running.
  entry.actors.clear();
  return true;
}

std::string_view ConversationPool::name_of(ConversationId id) const noexcept {
  if (!contains(id)) return {};
  return entries_[id - 1].name;
}

bool ConversationPool::bind_actor(ConversationId id, std::string_view role, ObjectId object) {
  if (!contains(id) || role.empty()) return false;
  Entry& entry = entries_[id - 1];
  for (ConversationActor& actor : entry.actors) {
    if (actor.role != role) continue;
    actor.object = object;
    actor.fallback_class.clear();
    return true;
  }
  if (entry.actors.size() >= kMaxConversationActors) return false;
  entry.actors.push_back(ConversationActor{std::string(role), object, {}});
  return true;
}

bool ConversationPool::bind_default_actor(ConversationId id, std::string_view role,
                                          std::string_view class_name) {
  if (!contains(id) || role.empty()) return false;
  Entry& entry = entries_[id - 1];
  for (ConversationActor& actor : entry.actors) {
    if (actor.role != role) continue;
    actor.object = kNoObject;
    actor.fallback_class = std::string(class_name);
    return true;
  }
  if (entry.actors.size() >= kMaxConversationActors) return false;
  entry.actors.push_back(ConversationActor{std::string(role), kNoObject, std::string(class_name)});
  return true;
}

std::span<const ConversationActor> ConversationPool::actors(ConversationId id) const noexcept {
  if (!contains(id)) return {};
  return entries_[id - 1].actors;
}

void ConversationPool::serialize(std::vector<std::byte>& out) const {
  bytes::put_u32(out, kConversationPoolMagic);
  bytes::put_u32(out, kConversationPoolVersion);
  // Every slot in index order, dead ones included: the index *is* the handle
  // minus one, and where the holes are decides what the next acquire returns.
  bytes::put_u32(out, static_cast<std::uint32_t>(entries_.size()));
  for (const Entry& entry : entries_) {
    bytes::put_u32(out, entry.script);
    bytes::put_u32(out, entry.slot);
    bytes::put_u8(out, entry.live ? 1u : 0u);
    bytes::put_string(out, entry.name);
    bytes::put_u32(out, static_cast<std::uint32_t>(entry.actors.size()));
    for (const ConversationActor& actor : entry.actors) {
      bytes::put_string(out, actor.role);
      bytes::put_u32(out, actor.object);
      bytes::put_string(out, actor.fallback_class);
    }
  }
}

Status ConversationPool::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kConversationPoolMagic) return FormatError::bad_magic;
  if (version != kConversationPoolVersion) return FormatError::unsupported;
  if (!reader.u32(count)) return FormatError::truncated;

  // Into a local first, moved in only once every entry has read cleanly.
  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Entry entry;
    std::uint8_t live = 0;
    std::uint32_t actors = 0;
    if (!reader.u32(entry.script) || !reader.u32(entry.slot) || !reader.u8(live) ||
        !bytes::get_string(reader, entry.name) || !reader.u32(actors)) {
      return FormatError::truncated;
    }
    if (actors > kMaxConversationActors) return FormatError::malformed;
    entry.live = live != 0;
    entry.actors.reserve(actors);
    for (std::uint32_t a = 0; a < actors; ++a) {
      ConversationActor actor;
      if (!bytes::get_string(reader, actor.role) || !reader.u32(actor.object) ||
          !bytes::get_string(reader, actor.fallback_class)) {
        return FormatError::truncated;
      }
      entry.actors.push_back(std::move(actor));
    }
    entries.push_back(std::move(entry));
  }
  entries_ = std::move(entries);
  return Status();
}

// --------------------------------------------------------------------------
// the results
// --------------------------------------------------------------------------

namespace {

[[nodiscard]] std::vector<ConversationResults::Entry>::const_iterator lower(
    const std::vector<ConversationResults::Entry>& entries, std::string_view name) {
  return std::lower_bound(entries.begin(), entries.end(), name,
                          [](const ConversationResults::Entry& row, std::string_view probe) {
                            return row.name < probe;
                          });
}

}  // namespace

void ConversationResults::set(std::string_view name, std::string_view value) {
  if (name.empty()) return;
  const auto at = lower(entries_, name);
  if (at != entries_.end() && at->name == name) {
    entries_[static_cast<std::size_t>(at - entries_.begin())].value = std::string(value);
    return;
  }
  entries_.insert(entries_.begin() + (at - entries_.begin()),
                  Entry{std::string(name), std::string(value)});
}

std::string_view ConversationResults::get(std::string_view name) const noexcept {
  const auto at = lower(entries_, name);
  if (at == entries_.end() || at->name != name) return {};
  return at->value;
}

void ConversationResults::hash(std::uint64_t& accumulator) const noexcept {
  for (const Entry& entry : entries_) {
    mix(accumulator, entry.name);
    mix(accumulator, entry.value);
  }
}

void ConversationResults::serialize(std::vector<std::byte>& out) const {
  bytes::put_u32(out, kConversationResultsMagic);
  bytes::put_u32(out, kConversationResultsVersion);
  bytes::put_u32(out, static_cast<std::uint32_t>(entries_.size()));
  for (const Entry& entry : entries_) {
    bytes::put_string(out, entry.name);
    bytes::put_string(out, entry.value);
  }
}

Status ConversationResults::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kConversationResultsMagic) return FormatError::bad_magic;
  if (version != kConversationResultsVersion) return FormatError::unsupported;
  if (!reader.u32(count)) return FormatError::truncated;

  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Entry entry;
    if (!bytes::get_string(reader, entry.name) || !bytes::get_string(reader, entry.value)) {
      return FormatError::truncated;
    }
    // Strictly ascending, which `get` binary-searches on. A table out of order
    // would make every lookup wrong in a way nothing later would notice.
    if (i != 0 && !(entries.back().name < entry.name)) return FormatError::malformed;
    entries.push_back(std::move(entry));
  }
  if (reader.remaining() != 0) return FormatError::malformed;
  entries_ = std::move(entries);
  return Status();
}

const ConversationDefinition* ConversationCatalogue::find(std::string_view name) const noexcept {
  // A linear scan in declaration order, as `NoteCatalogue::find` is and for the
  // same reason: 110 declarations in the whole installation and at most 15 in
  // one container.
  for (const ConversationDefinition& conversation : conversations_) {
    if (conversation.name == name) return &conversation;
  }
  return nullptr;
}


// ==========================================================================
// playing one
// ==========================================================================

namespace {

/// Compile a snippet and run it to completion, the way `OrderVerifier` runs a
/// `verify=` script.
///
/// **Synchronous, and a suspension is a failure.** The 66 shipped snippets have
/// no `Sleep` and no `Wait*` between them, so nothing in the data can suspend;
/// a snippet that did would have to be resumed on a later slice, and there is
/// nowhere to keep the half-finished conversation while it was. Refusing says
/// so, where a silent `false` would make a phrase quietly ineligible.
///
/// Compiled on use rather than cached. There are 66 snippets in the whole
/// installation, none longer than six lines, and a cache would have to be
/// invalidated against a catalogue that is rebuilt on every load -- a cost, and
/// a lifetime problem, for microseconds.
enum class SnippetResult {
  ok,
  /// The attribute is not a program this engine's grammar accepts.
  did_not_compile,
  /// It compiled and then failed to finish: a trap, a suspension, or a blown
  /// budget. In practice it is a trap on an entry point with no body, and the
  /// same entry point appears in the trap sweep under its own name from the
  /// ordinary scripts that call it -- so the cause is nowhere near as hidden as
  /// this one static message makes it look.
  trapped,
};

[[nodiscard]] SnippetResult evaluate(script::CallContext& ctx, const ConversationSnippet& snippet,
                                     std::string_view result_type, script::Value& out) {
  if (snippet.empty()) return SnippetResult::did_not_compile;
  const std::string source = snippet.wrap(result_type);
  const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(source.data()),
                                         source.size()};

  const script::HostRegistry* registry =
      ctx.scheduler != nullptr ? ctx.scheduler->registry() : nullptr;
  auto parsed = script::parse(bytes, "conversation", nullptr);
  if (!parsed) return SnippetResult::did_not_compile;
  auto chunk = script::compile(parsed.value(), registry);
  if (!chunk) return SnippetResult::did_not_compile;

  script::Execution execution = script::start(chunk.value());
  script::VmEnv env;
  env.host = ctx.host;
  env.scheduler = ctx.scheduler;
  env.registry = registry;
  env.user = ctx.user;
  env.script = script::kNoScript;
  env.now = ctx.now;
  env.call_trace = env.scheduler != nullptr ? env.scheduler->call_trace() : nullptr;
  env.trace_user = env.scheduler != nullptr ? env.scheduler->trace_user() : nullptr;

  if (script::run(execution, chunk.value(), env) != script::ExecStatus::finished) {
    return SnippetResult::trapped;
  }
  out = execution.result;
  return SnippetResult::ok;
}

/// Is this phrase eligible? A phrase with no `condition` always is.
[[nodiscard]] bool eligible(script::CallContext& ctx, const ConversationPhrase& phrase) {
  if (phrase.condition.empty()) return true;
  script::Value answer;
  // A condition that will not compile or traps is **false**, not true. A
  // conversation whose gate cannot be evaluated should fall through to whatever
  // comes next rather than assert a branch nobody could check.
  if (evaluate(ctx, phrase.condition, "bool", answer) != SnippetResult::ok) return false;
  return answer.truthy_scalar();
}

/// The candidate list a mode chooses from, as indices into `definition.phrases`.
///
/// `explicit_list` is the `startup_phrases` / `followup_phrases` attribute; when
/// it is empty the default applies, and the default is what
/// `docs/formats/conv-xml.md` derives from the 98 documents that end on a
/// `first`.
[[nodiscard]] std::vector<std::size_t> candidates(const ConversationDefinition& definition,
                                                  std::span<const std::string> explicit_list,
                                                  ConversationMode mode, std::size_t after,
                                                  bool from_start) {
  std::vector<std::size_t> out;
  if (!explicit_list.empty()) {
    for (const std::string& label : explicit_list) {
      const std::int32_t at = definition.find(label);
      // A label naming no phrase is dropped rather than stalling the list. It
      // does not happen in the shipped data -- every label a list names is
      // declared in the same document -- so this is the belt to the braces.
      if (at >= 0) out.push_back(static_cast<std::size_t>(at));
    }
    return out;
  }
  if (from_start) {
    // Startup with no list: every phrase, in document order.
    for (std::size_t i = 0; i < definition.phrases.size(); ++i) out.push_back(i);
    return out;
  }
  if (mode == ConversationMode::choice) {
    // A `choice` with no list: the phrases that follow this one and carry a
    // `choice_text`. Two documents do this and both are a prompt followed by
    // its own options.
    for (std::size_t i = after + 1; i < definition.phrases.size(); ++i) {
      if (!definition.phrases[i].choice_text.empty()) out.push_back(i);
    }
    return out;
  }
  // Everything else with no list: the next phrase, and nothing if there is
  // none. See the header for the 98 documents that make this the reading.
  if (after + 1 < definition.phrases.size()) out.push_back(after + 1);
  return out;
}

/// Pick one candidate under a mode, or `npos` for none.
///
/// **`choice` takes the first eligible option not yet taken in this run, and
/// ends the conversation when every option has been**, which is a decision
/// and not a reading: the original asks the player. A headless deterministic
/// simulation has nobody to ask, and the alternatives are worse -- drawing one
/// would spend the world RNG on a question the player answers, and refusing
/// would stall 11 phrases across 9 documents. Taking the first is what the
/// same document means by "pick one" and it is the option the author wrote at
/// the top; taking it *once* is what keeps a menu whose options all lead back
/// to the prompt from being asked for ever. The Tutorial's `1 Welcome` is
/// exactly that shape -- `Commands;Selection;Done`, the first two of which
/// loop to `repeat` -- and the first-always rule played it against the phrase
/// cap and reported a conversation that did not end. A run that has read every
/// option stops at the prompt, which is the player closing the window, and is
/// the bound the original has. An interface layer that wants to ask is the
/// seam this leaves open.
[[nodiscard]] std::size_t choose(script::CallContext& ctx, World& world,
                                 const ConversationDefinition& definition,
                                 std::span<const std::size_t> pool, ConversationMode mode,
                                 std::vector<std::size_t>& taken) {
  std::vector<std::size_t> ok;
  for (const std::size_t index : pool) {
    if (eligible(ctx, definition.phrases[index])) ok.push_back(index);
  }
  if (ok.empty()) return static_cast<std::size_t>(-1);
  switch (mode) {
    case ConversationMode::random:
    case ConversationMode::cycle_then_random:
      // The world's generator, not one of this domain's own: a draw that
      // decides what a mission says is part of the simulation's state.
      return ok[static_cast<std::size_t>(world.rng().below(static_cast<std::int32_t>(ok.size())))];
    case ConversationMode::choice:
      for (const std::size_t index : ok) {
        if (std::find(taken.begin(), taken.end(), index) != taken.end()) continue;
        taken.push_back(index);
        return index;
      }
      return static_cast<std::size_t>(-1);
    case ConversationMode::first:
    case ConversationMode::cycle:
    case ConversationMode::cycle_then_first:
    case ConversationMode::end:
      break;
  }
  return ok.front();
}

}  // namespace

ConversationRun play_conversation(script::CallContext& ctx,
                                  const ConversationDefinition& definition,
                                  std::span<const ConversationActor> actors,
                                  std::string_view previous_result) {
  ConversationRun run;
  run.result = std::string(previous_result);

  World* world = world_of(ctx);
  if (world == nullptr) {
    run.refusal = "no world";
    return run;
  }
  // The actors are not read by anything here yet: the simulation does not move
  // a camera and does not draw a name. They are carried because `ActorPresent`
  // answers on them and because the interface layer will need them, and they
  // are taken as a parameter rather than looked up so that `RunConv`, which
  // binds none, goes down the same path.
  (void)actors;

  // The options this run has already taken; see `choose`.
  std::vector<std::size_t> taken;
  std::size_t at =
      choose(ctx, *world, definition,
             candidates(definition, definition.startup_phrases, definition.startup, 0, true),
             definition.startup, taken);

  while (at != static_cast<std::size_t>(-1)) {
    if (run.phrases >= kMaxPhrasesPerRun) {
      // `followup_phrases` can name a phrase that comes before this one, so the
      // graph has cycles and a run is not bounded by the phrase count. The
      // original is bounded by the player closing the window; this is bounded
      // by a number, and reaching it is a document this engine cannot play
      // rather than a silent truncation.
      run.refusal = "conversation did not end";
      return run;
    }
    const ConversationPhrase& phrase = definition.phrases[at];
    ++run.phrases;

    // The action runs whether or not the phrase says anything, and before the
    // followup is resolved: the next phrase's condition may read what it wrote,
    // and `1_Great_Battles_Zama`'s menus are written that way.
    if (!phrase.action.empty()) {
      script::Value ignored;
      // **A failed action stops the conversation**, where a failed condition
      // only makes one phrase ineligible. The asymmetry is deliberate: an
      // action is what a conversation *does* -- it gives the note, explores the
      // area, writes the environment flag the next mission branches on -- and
      // one that silently did not run leaves a player with no objective and no
      // diagnostic, which is this project's signature failure. A condition that
      // cannot be evaluated only picks a different line.
      switch (evaluate(ctx, phrase.action, "void", ignored)) {
        case SnippetResult::ok:
          break;
        case SnippetResult::did_not_compile:
          run.refusal = "conversation: an action attribute does not compile";
          return run;
        case SnippetResult::trapped:
          run.refusal = "conversation: an action trapped, on an entry point with no body";
          return run;
      }
    }

    // A phrase with no `return` leaves the result alone rather than clearing
    // it, so a conversation that ends on an option with nothing to say answers
    // whatever it answered before.
    if (!phrase.result.empty()) {
      script::Value produced;
      if (evaluate(ctx, phrase.result, "str", produced) == SnippetResult::ok &&
          produced.is_string()) {
        run.result = std::string(produced.as_string());
      }
    }

    if (phrase.followup == ConversationMode::end) break;
    at = choose(ctx, *world, definition,
                candidates(definition, phrase.followup_phrases, phrase.followup, at, false),
                phrase.followup, taken);
  }

  run.ran = true;
  return run;
}


// ==========================================================================
// the script surface
// ==========================================================================

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::Value;

constexpr const char* kNoWorld = "conversation: no world";
constexpr const char* kNoCampaign = "conversation: no campaign system";
constexpr const char* kNoReceiver = "conversation: receiver is not a Conversation";
constexpr const char* kStaleHandle = "conversation: the handle is not bound";

struct Self {
  World* world = nullptr;
  CampaignSystem* campaign = nullptr;
  ConversationPool* pool = nullptr;
  ConversationId id = kNoConversation;
  const char* error = nullptr;

  [[nodiscard]] bool ok() const noexcept { return error == nullptr; }
};

/// The world and the campaign, without a receiver -- what a free function
/// needs.
[[nodiscard]] Self resolve_free(CallContext& ctx) {
  Self self;
  self.world = world_of(ctx);
  if (self.world == nullptr) {
    self.error = kNoWorld;
    return self;
  }
  self.campaign = campaign_system_of(*self.world);
  if (self.campaign == nullptr) {
    self.error = kNoCampaign;
    return self;
  }
  self.pool = &conversation_pool_of(*self.world);
  return self;
}

[[nodiscard]] Self resolve(CallContext& ctx) {
  Self self = resolve_free(ctx);
  if (!self.ok()) return self;
  if (ctx.count() == 0 || !is_conversation(ctx.arg(0))) {
    self.error = kNoReceiver;
    return self;
  }
  self.id = conversation_of(ctx.arg(0));
  if (!self.pool->contains(self.id)) self.error = kStaleHandle;
  return self;
}

/// The shared body of `Conversation::Run` and `RunConv`.
[[nodiscard]] HostOutcome run_named(CallContext& ctx, Self& self, std::string_view name,
                                    std::span<const ConversationActor> actors) {
  // A conversation nothing declares cannot be run, and the answer is the
  // previous result rather than a trap. `sim/note.hpp` makes the same call for
  // `GiveNote` on an undeclared id, and for the same reason: the shipped data
  // has a mission naming something its own container forgot to declare, and a
  // trap there would stop the mission rather than skip the line.
  const ConversationDefinition* definition = self.campaign->conversations().find(name);
  if (definition == nullptr) {
    return HostOutcome::ok_with(Value::string(std::string(self.campaign->results().get(name))));
  }

  const ConversationRun run =
      play_conversation(ctx, *definition, actors, self.campaign->results().get(name));
  if (run.refusal != nullptr) return HostOutcome::failed(run.refusal);

  // Keyed on the conversation's own name, which is what `ConvResult` is called
  // with -- not on the receiver, which is a local a script reuses.
  self.campaign->results().set(name, run.result);
  return HostOutcome::ok_with(Value::string(run.result));
}

/// `C_Conv.Init(name)` -- 54 sites, every one of them followed by `SetActor`
/// and `Run` in the same script.
HostOutcome init_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::ok_void();
  (void)self.pool->bind(self.id, ctx.arg(1).as_string());
  return HostOutcome::ok_void();
}

/// `C_Conv.SetActor(role, unit)` -- 63 sites, the most-called of the five,
/// because a conversation with two speakers binds twice.
///
/// The binding is carried and not read: nothing in the simulation moves a
/// camera or draws a name. It is world state all the same, because a save
/// taken between `Init` and `Run` has to come back with the same roles bound.
HostOutcome set_actor_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() < 3 || !ctx.arg(1).is_string()) return HostOutcome::ok_void();
  const Value& who = ctx.arg(2);
  // A role bound to nothing is still a role: the shipped idiom is
  // `SetActor("Scipio", GetNamedObj("NO_Scipio").obj.AsUnit())`, and
  // `GetNamedObj` on a unit that has died answers an invalid handle. Binding it
  // anyway keeps the conversation's cast complete and lets the interface layer
  // decide what an absent speaker looks like.
  const ObjectId object = who.is_object() ? who.as_object().id : kNoObject;
  (void)self.pool->bind_actor(self.id, ctx.arg(1).as_string(), object);
  return HostOutcome::ok_void();
}

/// `C_Conv.Run()` -- 54 sites. See `play_conversation` for why it finishes
/// inside the call rather than suspending the way the original's does.
HostOutcome run_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const std::string name(self.pool->name_of(self.id));
  // `Run` before `Init` is a script bug, not a trap: nothing is bound, so
  // nothing is said.
  if (name.empty()) return HostOutcome::ok_void();
  const std::vector<ConversationActor> actors(self.pool->actors(self.id).begin(),
                                              self.pool->actors(self.id).end());
  const HostOutcome outcome = run_named(ctx, self, name, actors);
  if (outcome.status != script::HostStatus::ok) return outcome;
  // `Conversation::Run` is registered `void`; the string goes to the results
  // table, where `ConvResult` reads it.
  return HostOutcome::ok_void();
}

/// `RunConv(name)` -- 74 sites, the largest single entry point left in the
/// coverage ranking, and the one-call form: no receiver, no actors, and it
/// **answers the result** where `Run` does not.
HostOutcome run_conv_impl(CallContext& ctx) {
  Self self = resolve_free(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() < 1 || !ctx.arg(0).is_string()) return HostOutcome::ok_with(Value::string(""));
  return run_named(ctx, self, ctx.arg(0).as_string(), {});
}

/// `ConvResult(name)` -- 5 sites, and one of them decides which map loads next.
HostOutcome conv_result_impl(CallContext& ctx) {
  Self self = resolve_free(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() < 1 || !ctx.arg(0).is_string()) return HostOutcome::ok_with(Value::string(""));
  return HostOutcome::ok_with(
      Value::string(std::string(self.campaign->results().get(ctx.arg(0).as_string()))));
}

constexpr std::size_t kEntryCount = 5;

/// `isqrt(dx² + dy²)` less both class radii -- `0x005a77b0`, the measure the
/// conversation reach test is written in. A local copy for the same reason
/// `sim/objlist.cpp` and `sim/hero.cpp` each carry one: it is four lines over
/// two exported helpers, and the alternative is a header for it.
[[nodiscard]] std::int64_t edge_distance(const World& world, const WorldObject& a,
                                         const WorldObject& b) {
  const Point pa = world.resolve_position(a.id);
  const Point pb = world.resolve_position(b.id);
  const std::int64_t dx = static_cast<std::int64_t>(pa.x) - pb.x;
  const std::int64_t dy = static_cast<std::int64_t>(pa.y) - pb.y;
  return isqrt(dx * dx + dy * dy) - class_int(world, a, "radius") - class_int(world, b, "radius");
}

}  // namespace

bool within_conversation_reach(const World& world, ObjectId walker, ObjectId host) noexcept {
  if (walker == kNoObject || host == kNoObject || walker == host) return false;
  const WorldObject* mover = world.find(walker);
  const WorldObject* target = world.find(host);
  if (mover == nullptr || target == nullptr) return false;
  // Sight first, and not as an optimisation: `0x005a7870` returns false on a
  // zero sight *before* it measures anything, so an object whose class declares
  // none is unreachable rather than reachable at distance zero.
  if (target->sight == 0) return false;
  return edge_distance(world, *mover, *target) <= target->sight;
}

bool conversation_meeting(const World& world, std::span<const ObjectId> walkers,
                          std::span<const ObjectId> hosts, ObjectId& walker, ObjectId& host) {
  for (const ObjectId candidate : walkers) {
    for (const ObjectId other : hosts) {
      if (!within_conversation_reach(world, candidate, other)) continue;
      walker = candidate;
      host = other;
      return true;
    }
  }
  return false;
}

std::size_t register_conversation_host(HostRegistry& registry) {
  std::size_t defined = 0;
  const auto member = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::member, name, arity, fn);
    ++defined;
  };
  const auto free_fn = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::free_function, name, arity, fn);
    ++defined;
  };

  // Descending corpus call frequency.
  free_fn("RunConv", 1, &run_conv_impl);   // 74
  member("SetActor", 2, &set_actor_impl);  // 63
  member("Run", 0, &run_impl);             // 54
  member("Init", 1, &init_impl);           // 54
  free_fn("ConvResult", 1, &conv_result_impl);  // 5

  // **Four registered entry points are deliberately not here**, and the reason
  // is the one `sim/campaign.hpp` gives for leaving out `ShowNotes`: each has
  // **zero call sites** in all 885 shipped scripts.
  //
  //   ActorPresent(str)          -> bool   0x00509960
  //   ConvSetResult(str, str)    -> void   0x0050a680
  //   Conversation::SetDefActor(str, str)  0x0050a7f0
  //   _ListConvs()               -> str    0x005099f0
  //
  // `ActorPresent` is the one worth naming twice, because implementing it would
  // mean inventing a concept this engine does not otherwise need. It takes no
  // receiver, so it asks about *the conversation now on screen* -- and here a
  // conversation exists only for the length of one `Run`, so "now" would have
  // to be a transient pointer parked somewhere that nothing saves and nothing
  // hashes. There is no caller to check that against, and the underscore on
  // `_ListConvs` says what the pair of them are: editor and console tools.
  return defined;
}

std::size_t conversation_host_entry_count() noexcept { return kEntryCount; }

}  // namespace imperivm::core::sim
