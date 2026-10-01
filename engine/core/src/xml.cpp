// The freestanding XML reader described in include/imperivm/core/xml.hpp.
//
// A hand-written recursive-descent grammar, run from an explicit stack rather
// than the C++ one: the depth of a shipped document is small, but the input is
// untrusted and `<a><a><a>...` must cost heap, not stack.
//
// Two decisions worth stating, because both are visible from the outside:
//
//   * **Text content is skipped, not stored.** It is still *validated* -- an
//     unknown entity in character data is rejected -- so "we do not model it"
//     never quietly becomes "we do not look at it". Five shipped files carry
//     stray character data (a duplicated `/>` in TTENT.SC.XML, the prose in
//     SCDOC.XML); none of it is meaningful.
//
//   * **Attribute values are not whitespace-normalised.** XML says a literal
//     tab or newline inside a value becomes a space. Doing that would mean
//     copying the value, which costs the zero-copy promise for every value in
//     the corpus to serve the two that contain a newline (SCDOC.XML and one
//     conversation file), neither of which the engine reads as data. Character
//     references *are* decoded, into the document's side buffer, because
//     `&amp;` is not a value any caller can use as it stands.

#include "imperivm/core/xml.hpp"

namespace imperivm::core {
namespace {

constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// XML's NameStartChar restricted to what the corpus uses. Names here are
/// ASCII identifiers; a non-ASCII name would be a finding, so it is rejected
/// rather than accepted on the strength of a high bit.
constexpr bool is_name_start(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':';
}

constexpr bool is_name_char(char c) noexcept {
  return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

/// Enough to force a heap allocation for the decode buffer on every standard
/// library we build against (libc++ inlines 22 bytes, libstdc++ 15). That
/// matters: the document is returned by value out of `parse`, and views into a
/// small-string-optimised buffer would dangle the moment it moved.
constexpr std::size_t kMinDecodeReserve = 64;

class Parser {
 public:
  Parser(std::string_view text, std::vector<XmlNode>& nodes,
         std::vector<XmlAttribute>& attributes, std::string& decoded)
      : in_(text), nodes_(nodes), attributes_(attributes), decoded_(decoded) {}

  /// document ::= misc* element misc*
  Status parse_document() {
    skip_byte_order_mark();
    if (Status status = skip_misc(); !status) return status;
    if (done() || peek() != '<' || !is_name_start(peek(1))) return FormatError::malformed;
    if (Status status = parse_tree(); !status) return status;
    if (Status status = skip_misc(); !status) return status;
    // A second top-level element is not a document this reader will pretend to
    // understand: which one is the root would be a guess.
    if (!done()) return FormatError::malformed;
    return {};
  }

 private:
  bool done() const noexcept { return position_ >= in_.size(); }

  char peek(std::size_t offset = 0) const noexcept {
    const std::size_t at = position_ + offset;
    return at < in_.size() ? in_[at] : '\0';
  }

  bool looking_at(std::string_view literal) const noexcept {
    return in_.substr(position_).starts_with(literal);
  }

  void skip_byte_order_mark() noexcept {
    if (in_.substr(position_).starts_with("\xEF\xBB\xBF")) position_ += 3;
  }

  bool skip_space() noexcept {
    const std::size_t start = position_;
    while (!done() && is_space(in_[position_])) ++position_;
    return position_ != start;
  }

  /// Skip to just past `terminator`, which must be present.
  Status skip_until(std::string_view terminator) {
    const std::size_t at = in_.find(terminator, position_);
    if (at == std::string_view::npos) return FormatError::truncated;
    position_ = at + terminator.size();
    return {};
  }

  /// Whitespace, comments and processing instructions, in any order. Anything
  /// else beginning `<!` -- a DOCTYPE, an entity declaration, a conditional
  /// section -- is refused: this reader has no DTD subset and would otherwise
  /// be silently ignoring declarations that change what the document means.
  Status skip_misc() {
    for (;;) {
      skip_space();
      if (looking_at("<!--")) {
        position_ += 4;
        if (Status status = skip_until("-->"); !status) return status;
      } else if (looking_at("<?")) {
        position_ += 2;
        if (Status status = skip_until("?>"); !status) return status;
      } else if (looking_at("<!")) {
        return FormatError::unsupported;
      } else {
        return {};
      }
    }
  }

  Status parse_name(std::string_view& out) {
    if (done() || !is_name_start(in_[position_])) return FormatError::malformed;
    const std::size_t start = position_;
    while (!done() && is_name_char(in_[position_])) ++position_;
    out = in_.substr(start, position_ - start);
    return {};
  }

  /// One character or entity reference at `index` within `text`. Appends the
  /// decoded byte to `out` when there is one; `out == nullptr` validates only.
  Status decode_reference(std::string_view text, std::size_t& index, std::string* out) const {
    const std::size_t semicolon = text.find(';', index);
    if (semicolon == std::string_view::npos) return FormatError::malformed;
    const std::string_view name = text.substr(index + 1, semicolon - index - 1);
    index = semicolon + 1;

    char decoded = '\0';
    if (name == "amp") {
      decoded = '&';
    } else if (name == "lt") {
      decoded = '<';
    } else if (name == "gt") {
      decoded = '>';
    } else if (name == "quot") {
      decoded = '"';
    } else if (name == "apos") {
      decoded = '\'';
    } else if (name.size() > 1 && name[0] == '#') {
      std::string_view digits = name.substr(1);
      std::uint32_t base = 10;
      if (digits[0] == 'x' || digits[0] == 'X') {
        base = 16;
        digits = digits.substr(1);
      }
      if (digits.empty()) return FormatError::malformed;
      std::uint32_t code = 0;
      for (const char c : digits) {
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
          digit = static_cast<std::uint32_t>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
          digit = static_cast<std::uint32_t>(c - 'a') + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
          digit = static_cast<std::uint32_t>(c - 'A') + 10;
        } else {
          return FormatError::malformed;
        }
        if (code > (0xFFFFFFFFu - digit) / base) return FormatError::malformed;
        code = code * base + digit;
      }
      // The corpus is cp1252 and every view we hand back is bytes, so a
      // reference above U+00FF has no single-byte spelling. Refusing it is
      // honest; re-encoding it as UTF-8 inside a cp1252 document would not be.
      if (code == 0 || code > 0xFF) return FormatError::unsupported;
      decoded = static_cast<char>(static_cast<unsigned char>(code));
    } else {
      // A named entity the DTD would have to define. `&raquo;` is the only one
      // in the install and it is in a saved web page, not a game file.
      return FormatError::unsupported;
    }

    if (out != nullptr) out->push_back(decoded);
    return {};
  }

  Result<std::string_view> decode_value(std::string_view raw) {
    const std::size_t begin = decoded_.size();
    for (std::size_t i = 0; i < raw.size();) {
      if (raw[i] != '&') {
        decoded_.push_back(raw[i]);
        ++i;
        continue;
      }
      if (Status status = decode_reference(raw, i, &decoded_); !status) return status.error();
    }
    return std::string_view(decoded_.data() + begin, decoded_.size() - begin);
  }

  Status parse_attribute() {
    XmlAttribute attribute;
    if (Status status = parse_name(attribute.name); !status) return status;
    skip_space();
    if (done() || in_[position_] != '=') return FormatError::malformed;
    ++position_;
    skip_space();
    const char quote = peek();
    if (quote != '"' && quote != '\'') return FormatError::malformed;
    ++position_;

    const std::size_t start = position_;
    bool has_reference = false;
    while (!done() && in_[position_] != quote) {
      // A raw `<` in an attribute value is invalid XML and, more practically,
      // is what a missing closing quote looks like: refusing it stops one
      // typo from swallowing the rest of the document into a value.
      if (in_[position_] == '<') return FormatError::malformed;
      if (in_[position_] == '&') has_reference = true;
      ++position_;
    }
    if (done()) return FormatError::truncated;
    const std::string_view raw = in_.substr(start, position_ - start);
    ++position_;

    if (has_reference) {
      auto decoded = decode_value(raw);
      if (!decoded) return decoded.error();
      attribute.value = *decoded;
    } else {
      attribute.value = raw;
    }
    attributes_.push_back(attribute);
    return {};
  }

  Status parse_start_tag(NodeIndex parent, NodeIndex& out, bool& self_closing) {
    ++position_;  // '<'
    XmlNode node;
    if (Status status = parse_name(node.name); !status) return status;
    if (nodes_.size() >= kNoNode) return FormatError::out_of_range;

    const NodeIndex index = static_cast<NodeIndex>(nodes_.size());
    node.parent = parent;
    node.attribute_begin = static_cast<std::uint32_t>(attributes_.size());
    nodes_.push_back(node);
    last_child_.push_back(kNoNode);

    if (parent != kNoNode) {
      if (last_child_[parent] == kNoNode) {
        nodes_[parent].first_child = index;
      } else {
        nodes_[last_child_[parent]].next_sibling = index;
      }
      last_child_[parent] = index;
    }

    for (;;) {
      const bool had_space = skip_space();
      if (looking_at("/>")) {
        position_ += 2;
        self_closing = true;
        break;
      }
      if (peek() == '>') {
        ++position_;
        self_closing = false;
        break;
      }
      if (done()) return FormatError::truncated;
      // `<class id="a"altid="b">` is not something to guess at.
      if (!had_space) return FormatError::malformed;
      if (Status status = parse_attribute(); !status) return status;
    }

    nodes_[index].attribute_count =
        static_cast<std::uint32_t>(attributes_.size()) - nodes_[index].attribute_begin;
    out = index;
    return {};
  }

  Status parse_end_tag(NodeIndex open) {
    position_ += 2;  // '</'
    std::string_view name;
    if (Status status = parse_name(name); !status) return status;
    if (name != nodes_[open].name) return FormatError::malformed;
    skip_space();
    if (done() || in_[position_] != '>') return FormatError::malformed;
    ++position_;
    return {};
  }

  /// Character data, comments, CDATA and processing instructions, up to the
  /// next start or end tag. Nothing is stored; references are checked.
  Status skip_content() {
    for (;;) {
      while (!done() && in_[position_] != '<') {
        if (in_[position_] == '&') {
          std::size_t index = position_;
          if (Status status = decode_reference(in_, index, nullptr); !status) return status;
          position_ = index;
        } else {
          ++position_;
        }
      }
      if (done()) return FormatError::truncated;
      if (looking_at("<!--")) {
        position_ += 4;
        if (Status status = skip_until("-->"); !status) return status;
      } else if (looking_at("<![CDATA[")) {
        position_ += 9;
        if (Status status = skip_until("]]>"); !status) return status;
      } else if (looking_at("<?")) {
        position_ += 2;
        if (Status status = skip_until("?>"); !status) return status;
      } else if (looking_at("<!")) {
        return FormatError::unsupported;
      } else {
        return {};
      }
    }
  }

  /// The element tree, iteratively. `open_` is the chain of elements whose end
  /// tag is still owed.
  Status parse_tree() {
    bool at_start_tag = true;
    for (;;) {
      if (at_start_tag) {
        NodeIndex index = kNoNode;
        bool self_closing = false;
        const NodeIndex parent = open_.empty() ? kNoNode : open_.back();
        if (Status status = parse_start_tag(parent, index, self_closing); !status) return status;
        if (!self_closing) open_.push_back(index);
        at_start_tag = false;
        if (open_.empty()) return {};  // a self-closed root: the whole document
        continue;
      }

      if (Status status = skip_content(); !status) return status;
      if (peek(1) == '/') {
        if (Status status = parse_end_tag(open_.back()); !status) return status;
        open_.pop_back();
        if (open_.empty()) return {};
      } else if (is_name_start(peek(1))) {
        at_start_tag = true;
      } else {
        return FormatError::malformed;
      }
    }
  }

  std::string_view in_;
  std::size_t position_ = 0;
  std::vector<XmlNode>& nodes_;
  std::vector<XmlAttribute>& attributes_;
  std::string& decoded_;
  std::vector<NodeIndex> last_child_;  ///< per node, so children keep document order
  std::vector<NodeIndex> open_;
};

std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

char lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ignoring_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

}  // namespace

Result<XmlDocument> XmlDocument::parse(std::span<const std::byte> text) {
  const std::string_view source(reinterpret_cast<const char*>(text.data()), text.size());

  XmlDocument document;
  // Reserved once, never grown: every view handed out of `decoded_` would
  // dangle on a reallocation. The decoded form of a value is never longer than
  // its source, so the input length is a sufficient bound for all of them
  // together. Documents with no `&` at all pay nothing.
  //
  // Moving a document is safe because the reserve puts the buffer on the heap.
  // *Copying* one is not: the copy allocates, and the copy's decoded attribute
  // values would point into the original's buffer. Nothing in the engine copies
  // a document, but the type does not forbid it, which is worth a decision
  // rather than a silent hazard.
  if (source.find('&') != std::string_view::npos) {
    document.decoded_.reserve(source.size() < kMinDecodeReserve ? kMinDecodeReserve
                                                                : source.size());
  }

  Parser parser(source, document.nodes_, document.attributes_, document.decoded_);
  if (Status status = parser.parse_document(); !status) return status.error();
  if (document.nodes_.empty()) return FormatError::malformed;

  document.root_ = 0;
  return document;
}

std::string_view XmlDocument::attribute(NodeIndex index, std::string_view name) const {
  if (index >= nodes_.size()) return {};
  const XmlNode& element = nodes_[index];
  for (std::uint32_t i = 0; i < element.attribute_count; ++i) {
    const XmlAttribute& attribute = attributes_[element.attribute_begin + i];
    if (attribute.name == name) return attribute.value;
  }
  return {};
}

std::int32_t XmlDocument::attribute_int(NodeIndex index, std::string_view name,
                                        std::int32_t fallback) const {
  std::string_view text = trim(attribute(index, name));
  if (text.empty()) return fallback;

  bool negative = false;
  if (text.front() == '-' || text.front() == '+') {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  if (text.empty()) return fallback;

  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return fallback;  // malformed reads as absent, on purpose
    value = value * 10 + (c - '0');
    if (value > 0x100000000ll) return fallback;  // far past the range; stop before overflow
  }
  if (negative) value = -value;
  if (value < INT32_MIN || value > INT32_MAX) return fallback;
  return static_cast<std::int32_t>(value);
}

bool XmlDocument::attribute_bool(NodeIndex index, std::string_view name, bool fallback) const {
  const std::string_view text = trim(attribute(index, name));
  if (equals_ignoring_case(text, "1") || equals_ignoring_case(text, "yes") ||
      equals_ignoring_case(text, "true")) {
    return true;
  }
  if (equals_ignoring_case(text, "0") || equals_ignoring_case(text, "no") ||
      equals_ignoring_case(text, "false")) {
    return false;
  }
  return fallback;
}

NodeIndex XmlDocument::child(NodeIndex index, std::string_view name) const {
  if (index >= nodes_.size()) return kNoNode;
  for (NodeIndex i = nodes_[index].first_child; i != kNoNode; i = nodes_[i].next_sibling) {
    if (nodes_[i].name == name) return i;
  }
  return kNoNode;
}

NodeIndex XmlDocument::next(NodeIndex index, std::string_view name) const {
  if (index >= nodes_.size()) return kNoNode;
  for (NodeIndex i = nodes_[index].next_sibling; i != kNoNode; i = nodes_[i].next_sibling) {
    if (nodes_[i].name == name) return i;
  }
  return kNoNode;
}

}  // namespace imperivm::core
