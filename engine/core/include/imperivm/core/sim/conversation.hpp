#pragma once

/// Conversations: the scripted talking a mission does.
///
/// 110 documents in the shipped containers, 260 phrases between them, and the
/// entry points that drive them are the largest unimplemented cluster left --
/// `RunConv/1` (74 call sites), `Conversation::SetActor/2` (63),
/// `Conversation::Run/0` (54), `Conversation::Init/1` (54), `ConvResult/1` (5),
/// `EndConvSetup/2` (1). Every one of them is inside a map container: this is
/// mission scripting, not object behaviour.
///
/// ## Two halves, and only one of them is state
///
/// A conversation is **declared** in `Maps/<n>/Conversations/cnv<k>.conv.xml`
/// and **run** by a script. This file is the declaration half: the catalogue is
/// configuration, rebuilt from the container on both sides of a save, never
/// hashed -- the standing `NoteCatalogue` and `AreaSystem` have.
///
/// The file name is not the conversation's name. `cnv1.conv.xml` declares
/// `<conversation name="C_Conv2">`, and `Init("C_Conv2")` finds it by that
/// attribute, so the catalogue is a map from `name` to definition and the
/// numbering of the files means nothing. The executable loads the directory:
/// its string table carries `CurrentGame/Conversations` beside the
/// `CurrentMap/...` spelling, which is the same two-root arrangement
/// `sequences.xml` and `Notes.xml` use.
///
/// ## The mode vocabulary is the executable's, and the shipped data uses three
/// of the seven
///
/// `startup` and `followup` both hold a mode name, and 0x005067d0 maps it:
///
/// | name | code | |
/// |---|---|---|
/// | `choice` | 0 | ask the player, from the candidates |
/// | `random` | 1 | one eligible candidate, drawn |
/// | `first` | 2 | the first eligible candidate |
/// | `end` | 3 | the conversation is over |
/// | `cycle` | 4 | the next candidate after the one used last time |
/// | `cycle then first` | 5 | cycle until exhausted, then stay on the first |
/// | `cycle then random` | 6 | cycle until exhausted, then draw |
///
/// **An empty string is `first`**, which the same function spells out by
/// testing for it explicitly before it gives up. Anything else is refused;
/// there is an `!error!` in the table beside the seven names.
///
/// The shipped 110 documents use `first`, `end` and `choice` and nothing else,
/// so four of the seven are named here and not exercised by any container. They
/// are in the enumeration anyway, because a mode this reader did not know would
/// otherwise be read as `first` and play the wrong line for ever.
///
/// ## How the candidates are found, which the attributes only half say
///
/// A mode chooses *from a candidate list*, and the list comes from
/// `startup_phrases` / `followup_phrases` when the attribute is there --
/// semicolon-separated labels, in order. It is there **9 times out of 110** for
/// startup and **23 times out of 260** for followup, so the default is what
/// matters, and the data settles it:
///
///   * for **startup**, the candidates are every phrase in document order.
///     `startup` is `first` in all 110 documents, so the conversation opens on
///     the first phrase whose `condition` holds -- which is how a document can
///     lead with an early-out branch and fall through to the real opening.
///
///   * for **followup**, the candidate is **the next phrase in document
///     order**, and the conversation ends when there is none. This is not a
///     guess: 98 of the 110 documents end on a phrase whose `followup` is
///     `first`, and under any reading where `first` re-scanned the whole
///     document those 98 would loop for ever on their own opening line.
///
///   * for **choice** with no list, the candidates are the phrases that follow
///     this one and carry a `choice_text`. Two documents do this, and both are
///     a prompt followed by its own options.
///
/// ## `\l`, `\g` and `\a`, which are not C escapes
///
/// `condition`, `action` and `return` carry **VS source in an XML attribute**,
/// and three of the characters a program needs cannot appear there. The
/// authoring tool escapes them:
///
/// | written | means | why |
/// |---|---|---|
/// | `\l` | `<` | XML would read it as a tag |
/// | `\g` | `>` | for symmetry with `\l` |
/// | `\a` | `&` | XML would read it as an entity; `\a\a` is `&&` |
/// | `\'` | `'` | a VS string literal, since `"` ends the attribute |
/// | `\n` | newline | attributes are one line |
///
/// Those five are the only backslash escapes in all 110 documents -- 207 `\n`,
/// 204 `\'`, 5 `\g`, 4 `\a`, 2 `\l` -- and there is not one XML character
/// reference anywhere in them. `\'` decodes to a single quote rather than a
/// double one because the VS lexer accepts either as a string delimiter, and
/// because 3 of the 31 `action` attributes write `'` **unescaped** and must
/// mean the same thing.
///
/// **The display strings are not decoded**, and that is deliberate rather than
/// an oversight: `text` and `choice_text` carry `\n` too, and a line break in a
/// display string is the interface layer's business, not the simulation's. It
/// is the same call `NoteDefinition::text` makes and for the same reason. If a
/// renderer ever wants them cooked, it can call `decode_conversation_escapes`,
/// which is exported here for exactly that.
///
/// ## `condition` and `return` are each two shapes
///
/// Both are written either as a bare expression or as a statement list, and the
/// corpus has both:
///
///     condition="EnvReadInt('/En_NumidiansCharge') == 1"
///     condition="if (EnvReadInt('/Ships')==1)\n return true;\nreturn false;"
///     return="'YES'"
///     return="return 'Trident';"
///
/// 8 of the 31 conditions are bare and 23 are bodies; 1 of the 4 returns is
/// bare and 3 are bodies. Nothing distinguishes them but the grammar, so this
/// reader records the text and the compiler decides -- see
/// `ConversationSnippet::wrap`, which is the one place that knows the rule.
///
/// ## What is still unknown
///
/// **What `followup_phrases` means on a phrase whose `followup` is `end`.**
/// Two phrases in `5_Great_Battles_Britain` do this, both options in a
/// buy-something menu, both naming the menu's own prompt. The natural reading
/// is that the list is a jump target and the player is returned to the menu
/// after paying; the other reading is that the mode wins and the attribute is
/// the authoring tool's leftover. `followup` is treated as authoritative here,
/// so those two end the conversation. It is observable in play -- whether
/// buying twice needs two conversations -- and nothing in the data settles it.
///
/// **Where the cycle cursor lives.** `cycle`, `cycle then first` and
/// `cycle then random` need to remember which candidate was used last, and
/// nothing in the shipped data uses any of the three, so there is no sample of
/// where the original keeps it -- per conversation, per phrase, or per phrase
/// per actor. `ConversationBoard` keeps one cursor per phrase, which is the
/// reading that makes a `cycle` inside a `choice` branch behave.
///
/// **What `restore_view` restores.** 34 of the 110 set it. The conversation
/// code saves four words of the current view before it moves the camera to the
/// average of the actors' positions, so the attribute plainly governs putting
/// the camera back. It is a presentation flag, carried here and read by nobody
/// in `engine/core`.
///
/// **`comments`.** Two phrases carry one. It is authoring metadata and is not
/// read.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"

namespace imperivm::core::sim {

class World;

/// How a conversation picks the next thing to say.
///
/// The seven names `gbr.exe` accepts, with its own numbering. See the header
/// for the table and for which three the shipped data uses.
enum class ConversationMode : std::uint8_t {
  choice = 0,
  random = 1,
  first = 2,
  end = 3,
  cycle = 4,
  cycle_then_first = 5,
  cycle_then_random = 6,
};

/// Parse a mode name. Empty means `first`; anything unrecognised is refused,
/// because reading an unknown mode as `first` would play the wrong line for
/// ever rather than say so.
[[nodiscard]] Result<ConversationMode> parse_conversation_mode(std::string_view text);

/// The name, for diagnostics and for round-tripping a document.
[[nodiscard]] std::string_view conversation_mode_name(ConversationMode mode) noexcept;

/// Undo the five escapes a `.conv.xml` attribute uses: `\n`, `\'`, `\l`, `\g`,
/// `\a`. A backslash before anything else is kept, backslash and all, because
/// a Windows path in a `text` attribute is the likelier reading and losing the
/// separator would be worse than keeping a stray escape.
[[nodiscard]] std::string decode_conversation_escapes(std::string_view text);

/// A piece of VS source carried in an attribute.
///
/// Kept as text rather than compiled here, because `engine/core`'s format
/// readers do not depend on the script front end and because a conversation
/// document is read once per session while a condition is evaluated once per
/// phrase per run.
struct ConversationSnippet {
  /// The attribute with its escapes undone, or empty when there was none.
  std::string source;

  [[nodiscard]] bool empty() const noexcept { return source.empty(); }

  /// The source as a compilable body.
  ///
  /// Both shapes occur (see the header), and they are told apart by grammar:
  /// a snippet that contains a `;` or opens with a keyword is already a
  /// statement list, and anything else is a bare expression that has to be
  /// wrapped in a `return`. `result_type` names what the enclosing signature
  /// should say -- `bool` for a condition, `str` for a return, `void` for an
  /// action.
  [[nodiscard]] std::string wrap(std::string_view result_type) const;
};

/// One `<phrase>`.
struct ConversationPhrase {
  /// `label`: what a `startup_phrases` or `followup_phrases` list names. Only
  /// 66 of the 260 shipped phrases have one, and within a document they are
  /// unique -- all 110 of them.
  std::string label;
  /// `actor`: which `<actor>` says it. 258 of 260 name one.
  std::string actor;
  /// `text`, **verbatim, escapes and all**. See the header for why.
  std::string text;
  /// `choice_text`, verbatim: what the player sees on the menu button.
  std::string choice_text;

  /// `condition`: whether this phrase is eligible at all. Empty means always.
  ConversationSnippet condition;
  /// `action`: run when the phrase plays. Empty means nothing to do.
  ConversationSnippet action;
  /// `return`: the string this phrase leaves behind for `ConvResult`.
  ConversationSnippet result;

  /// `followup`, defaulting to `first` when the attribute is absent.
  ConversationMode followup = ConversationMode::first;
  /// `followup_phrases`, split on `;` and trimmed, empty fields dropped.
  /// Empty means the default candidate list -- see the header.
  std::vector<std::string> followup_phrases;
};

/// One `<conversation>`.
struct ConversationDefinition {
  /// `name`: what `Init` and `RunConv` are called with, and the key
  /// `ConvResult` reads back. Not the file name.
  std::string name;
  /// `startup`, defaulting to `first`. It is `first` in all 110 documents.
  ConversationMode startup = ConversationMode::first;
  /// `startup_phrases`, split like `followup_phrases`. Present 9 times.
  std::vector<std::string> startup_phrases;
  /// `restore_view`: put the camera back afterwards. Presentation; carried and
  /// not read here. Set in 34 of the 110.
  bool restore_view = false;

  /// `<actor name>`, in document order. A conversation declares the roles it
  /// needs and `SetActor` binds each to an object.
  std::vector<std::string> actors;
  /// `<phrase>`, in document order, which is the order every default candidate
  /// list walks.
  std::vector<ConversationPhrase> phrases;

  /// Index of the phrase with this label, or -1. Case-sensitive, as every
  /// other id in this data is.
  [[nodiscard]] std::int32_t find(std::string_view label) const noexcept;
};

/// Every conversation a container declares.
///
/// **Configuration, not state**, exactly as `NoteCatalogue` is: rebuilt from
/// the container on both sides of a save, never hashed, never serialised.
class ConversationCatalogue {
 public:
  /// Parse one `<conversation>` document and add what it declares.
  ///
  /// One document holds one conversation in all 110 shipped files, but the
  /// element is looked for rather than assumed, so a document that grew a
  /// wrapper still reads.
  ///
  /// An empty span adds nothing and is not an error -- a container with no
  /// conversations is ordinary, and six of the twenty-four have none. A
  /// `<conversation>` with no `name` is skipped, because nothing could ever
  /// call it; a duplicate name is skipped, first-in wins, which is what a
  /// `std::map` insert does with a key it already holds.
  [[nodiscard]] Result<std::size_t> add(std::span<const std::byte> xml);

  /// The definition `name` names, or null.
  [[nodiscard]] const ConversationDefinition* find(std::string_view name) const noexcept;

  /// Declaration order, which is the order the documents were added in.
  [[nodiscard]] std::span<const ConversationDefinition> all() const noexcept { return conversations_; }
  [[nodiscard]] std::size_t size() const noexcept { return conversations_.size(); }
  [[nodiscard]] bool empty() const noexcept { return conversations_.empty(); }
  void clear() noexcept { conversations_.clear(); }

 private:
  std::vector<ConversationDefinition> conversations_;
};

/// Split a `;`-separated label list, trimming each field and dropping the empty
/// ones. `"begining;"` is one label, not two: the trailing separator is how the
/// authoring tool writes a one-element list and appears in 4 of the 23 shipped
/// `followup_phrases` attributes.
[[nodiscard]] std::vector<std::string> split_phrase_list(std::string_view text);

// ==========================================================================
// the runtime
// ==========================================================================

/// The `TypeId` a `Conversation` handle carries. **12**, continuing the census
/// `sim/world_host.hpp` keeps: 1-4 and 8 are its, 5 `sim/objlist.hpp`'s,
/// 6 and 11 `sim/squad.hpp`'s, 7 `sim/globals.hpp`'s, 9 and 10
/// `sim/array.hpp`'s.
inline constexpr script::TypeId kTypeConversation = 12;

/// A pooled conversation's handle. Zero is never issued, so a
/// default-constructed value is distinguishable from a real one -- `ObjListId`'s
/// rule, and `SquadListId`'s.
using ConversationId = std::uint32_t;

inline constexpr ConversationId kNoConversation = 0;

[[nodiscard]] script::Value make_conversation_value(ConversationId id) noexcept;
[[nodiscard]] bool is_conversation(const script::Value& value) noexcept;
/// `kNoConversation` when `value` is not a `Conversation` handle.
[[nodiscard]] ConversationId conversation_of(const script::Value& value) noexcept;

/// One role bound to one object. `SetActor("Scipio", unit)`.
struct ConversationActor {
  std::string role;
  ObjectId object = kNoObject;
  /// `SetDefActor` binds a role to a **class name** rather than to an object,
  /// so that a conversation can be run with a stand-in nobody placed. Empty
  /// unless that entry point bound it.
  std::string fallback_class;
};

/// The conversations every live script holds.
///
/// `SquadListPool`'s shape: entries keyed by declaration site, released with
/// the script, serialised and **not hashed**. `Conversation C_Conv;` is a
/// local, every one of the 54 shipped `Init` receivers is one, and no host
/// function returns a conversation, so there are no aliases and nothing to
/// mark and sweep.
///
/// **What an entry holds is a binding, not a state machine.** The name the
/// script last called `Init` with, and the actors it has bound since. Running
/// the conversation is `play_conversation`, which finishes inside the call --
/// see its own note for why, and for what that costs.
class ConversationPool {
 public:
  /// Mint or reuse the entry for one declaration site, unbound.
  ConversationId acquire(script::ScriptId script, std::uint32_t slot);

  /// Drop every entry a script owns. Called from the scheduler's teardown hook.
  void release_script(script::ScriptId script);

  [[nodiscard]] bool contains(ConversationId id) const noexcept;
  [[nodiscard]] script::ScriptId owner_of(ConversationId id) const noexcept;

  /// `Init(name)`. Rebinds the entry and **clears the actors**, which is what
  /// makes the shipped idiom work: a script that runs six conversations from
  /// one local calls `Init` before each and binds only the roles that one
  /// needs.
  bool bind(ConversationId id, std::string_view name);

  /// The name `Init` last bound, or empty.
  [[nodiscard]] std::string_view name_of(ConversationId id) const noexcept;

  /// `SetActor(role, object)` / `SetDefActor(role, class)`. Rebinding a role
  /// replaces it rather than adding a second entry.
  bool bind_actor(ConversationId id, std::string_view role, ObjectId object);
  bool bind_default_actor(ConversationId id, std::string_view role, std::string_view class_name);

  /// The actors bound to one entry, in binding order.
  [[nodiscard]] std::span<const ConversationActor> actors(ConversationId id) const noexcept;

  [[nodiscard]] std::size_t capacity() const noexcept { return entries_.size(); }

  /// Little-endian, self-describing, versioned, like every other store here.
  void serialize(std::vector<std::byte>& out) const;
  [[nodiscard]] Status deserialize(std::span<const std::byte> data);

  void clear() noexcept { entries_.clear(); }

 private:
  struct Entry {
    script::ScriptId script = script::kNoScript;
    std::uint32_t slot = 0;
    bool live = false;
    std::string name;
    std::vector<ConversationActor> actors;
  };

  std::vector<Entry> entries_;
};

/// The pool a world holds. Declared here so `sim/world.hpp` needs only the
/// type, and defined in `sim/conversation.cpp` beside everything that uses it.
[[nodiscard]] ConversationPool& conversation_pool_of(World& world);

/// What each conversation last left behind for `ConvResult`.
///
/// **World state**, unlike the catalogue: a script branches on one.
/// `3_Great_Battles_Alesia` map 5 reads `ConvResult("C_Conv4")` and calls
/// `ChangeMap` on the answer, so this decides which mission plays next.
///
/// Ascending by name -- a `std::map`'s order, and iteration order is state.
class ConversationResults {
 public:
  /// `ConvSetResult(name, value)`, and what a phrase's `return` produces.
  /// Setting an empty value stores an empty value rather than removing the
  /// entry, because `ConvResult` on a conversation that has run and said
  /// nothing is not the same question as one that has never run.
  void set(std::string_view name, std::string_view value);

  /// `ConvResult(name)`. The empty string for a conversation that has not run.
  [[nodiscard]] std::string_view get(std::string_view name) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  void clear() noexcept { entries_.clear(); }

  /// FNV-1a over the pairs in order. Folded into `CampaignSystem::hash`.
  void hash(std::uint64_t& accumulator) const noexcept;

  void serialize(std::vector<std::byte>& out) const;
  /// **Atomic**, and refused when the names do not arrive strictly ascending:
  /// `get` binary-searches on that.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

  struct Entry {
    std::string name;
    std::string value;
  };
  [[nodiscard]] std::span<const Entry> all() const noexcept { return entries_; }

 private:
  std::vector<Entry> entries_;
};

/// What one run of a conversation did.
struct ConversationRun {
  /// False when the conversation could not be found or could not be started.
  bool ran = false;
  /// How many phrases played. Zero is legal: every phrase's condition can be
  /// false, and `NeedHelp`'s first one is written to be.
  std::size_t phrases = 0;
  /// The last `return` the run produced, or the previous result when it
  /// produced none. This is what goes into `ConversationResults`.
  std::string result;
  /// Null on success. A sentence naming what stopped it, for the trap.
  const char* refusal = nullptr;
};

/// The limit on phrases in one run.
///
/// `followup_phrases` can name a phrase that comes before this one -- two
/// shipped options name their own menu -- so the phrase graph has cycles in it
/// and a run is not bounded by the phrase count. The original is bounded by the
/// player, who closes the window. This is not, so it is bounded by a number,
/// and the number is far above anything the data can reach: the largest shipped
/// conversation has 12 phrases.
inline constexpr std::size_t kMaxPhrasesPerRun = 256;

/// Play a conversation to its end and answer what it left behind.
///
/// **It finishes inside the call, and that is a departure worth stating.**
/// `Conversation::Run` and `RunConv` are both registered through `gbr.exe`'s
/// *suspending* registrar, and in the original they suspend for as long as the
/// conversation is on screen -- which is the length of a voice clip, or until
/// the player clicks. Neither is available to a headless deterministic
/// simulation, and inventing a duration would be inventing a number that
/// decides when every other script resumes. So the state machine runs to
/// completion in one slice and the entry points return `ok`.
///
/// What that costs is one turn of ordering: a script that writes
/// `C_Conv.Run(); GiveNote("Kill Syphax");` gives the note in the same slice
/// rather than after the talking. What it buys is that a conversation cannot
/// stall a mission, which matters because 74 `RunConv` sites are on the
/// critical path of a campaign and a wait that never ended would hang all of
/// them.
///
/// `ctx` is needed because `condition`, `action` and `return` are VS source:
/// they are parsed, compiled against the live registry and run the way
/// `OrderVerifier` runs a `verify=` script, which is the established shape for
/// *evaluate this snippet now* in this engine.
[[nodiscard]] ConversationRun play_conversation(script::CallContext& ctx,
                                                const ConversationDefinition& definition,
                                                std::span<const ConversationActor> actors,
                                                std::string_view previous_result);

// ==========================================================================
// the conversation request
// ==========================================================================
//
// `WaitConvRequest` -- 7 sites, two overloads, and the last of the `Wait*`
// family to be written -- asks "has one of these come to talk to one of
// those". `sim/wait.hpp` owns the two entry points; this is the rule they
// test, and the manager they test it against in the original.
//
// ## The manager, and why this engine has none
//
// `gbr.exe` keeps a lazily-constructed singleton (`0x0050f4c0`, pointer at
// `0x996adc`) holding a list of 12-byte records: a state word, the two
// parties' handle ids, and the two object ids the meeting resolved to. Four
// operations reach it from the two entry points and from the input layer:
//
//   * **request** (`0x0050f7b0`): append `{pending, a, b, none, none}`. There
//     is no duplicate check, which is why the caller does it exactly once --
//     `gbr.exe`'s interpreter writes 1 into the last byte of a fresh call
//     frame and 0 on every resume, and the body gates the append on that byte.
//     `script::CallContext::first_call` is the same bit.
//   * **find** (`0x0050eab0`): the first record with this pair, answering its
//     state and, once the state is `met`, the two objects.
//   * **withdraw** (`0x0050ea40`): erase the first record with this pair. Both
//     entry points call it on success *and* on timeout.
//   * **offer** (`0x0050f3f0`): the completing side. It takes two objects and
//     walks the list looking for a pending record each of whose parties
//     matches one of them -- a party is either a `Query`, tested for
//     membership through its own vtable, or an `Obj`, tested for identity
//     (`0x0050e7d0`) -- and then applies `within_conversation_reach` below.
//
// **Nothing in the script surface can observe that list**, and the one thing
// that drives `offer` is the input layer: `0x005ea300`, the function that also
// reads `UserInputCenterSelectionWaitTime` and `UserInputViewDontChangeLimit`,
// offers the player's current selection (`0x005e6ae0`, from the object id at
// the global `0x824d38`). So the record's whole purpose is to carry a question
// from the script to a per-frame test that the script cannot make itself.
//
// This engine makes the test directly instead, and the reasoning is
// `sim/entrance.hpp`'s about building doors: a stored answer is a function of
// the world *at the moment something happened to look*, so it has to be saved,
// invalidated and reproduced; a derived one is a function of the world, and
// two peers that ask at the same turn get the same answer with nothing in the
// save file and no rule to get wrong. `WaitConvRequest` therefore holds no
// state at all -- it is a predicate over the world, like the other eleven
// members of its family, which is exactly what the plan said it was not.
//
// **The divergence that buys is the selection gate.** The original completes a
// request only for the object the player currently has selected; this engine
// completes it for any member of the first party that is in reach. In all
// seven shipped sites the first party is either a named hero the player drives
// (`NO_Scipio`, `NO_Hero3`) or the player's own units
// (`ClassPlayerObjs(cUnit, 1)`), so the walker has to be walked there either
// way and the two readings differ only when an *unselected* unit of the
// player's arrives first. Reproducing the gate would mean putting the mouse's
// selection into hashed world state, which is the one thing two peers drawing
// the same battle must be free to disagree about.

/// Whether `walker` has come close enough to `host` to talk to it.
///
/// `0x005a7870`, two clauses. `host`'s **sight** must be non-zero -- an object
/// whose class declares none can never be talked to, whatever the distance --
/// and the **edge-to-edge** distance must not exceed it: `isqrt(dx² + dy²)`
/// less both class radii (`0x005a77b0`), which is the same measure
/// `Druid::FindUnitToHeal` and `NearestObj` use and *not* the centre-to-centre
/// one `Obj::DistTo` answers with.
///
/// **The test is asymmetric, and it is the party being walked *to* whose sight
/// is read.** The executable reaches it through a matcher that works out which
/// side of the record the offered object matched and passes the other side's
/// object as the receiver, so the direction is decided by the data rather than
/// by the argument order. The corpus cannot tell the two apart: all seven
/// shipped sites put the walker first and the destination second, which is
/// also the order the five-argument overload writes its two out-parameters in.
[[nodiscard]] bool within_conversation_reach(const World& world, ObjectId walker,
                                             ObjectId host) noexcept;

/// The first pair -- one object from each party -- that
/// `within_conversation_reach` accepts, or false when there is none.
///
/// `walkers` and `hosts` are scanned in the order given, walkers outermost.
/// Every caller passes them in ascending object id, which is what
/// `World::evaluate_query` answers in, so which pair a world yields is a
/// function of the world rather than of when it was asked.
///
/// **An object in both parties is skipped rather than allowed to meet
/// itself.** The original cannot produce that pair: its matcher is handed two
/// distinct objects and tests one against each side.
[[nodiscard]] bool conversation_meeting(const World& world, std::span<const ObjectId> walkers,
                                        std::span<const ObjectId> hosts, ObjectId& walker,
                                        ObjectId& host);

/// The nine entry points this domain owns.
std::size_t register_conversation_host(script::HostRegistry& registry);

/// How many `register_conversation_host` defines. Pinned by a test, so that a
/// name added here without a home in the corpus census shows up.
[[nodiscard]] std::size_t conversation_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
