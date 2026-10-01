#pragma once

/// The in-game help: `CURRENTLANG\HELP.XML` in the language pack.
///
/// A hypertext of topics, nested -- `contents` holds `fogofwar`, `map`,
/// `units` and the rest, `units` holds one topic per class -- each a list of
/// entries:
///
/// ```xml
/// <topic id="resources">
///   <entry hcenter="1" font="large">Risorse</entry>
///   <entry>In Imperivm ... devi gestire due risorse: viveri e oro.</entry>
///   <entry image="UI/help/food ico.bmp">I viveri sono prodotti ...</entry>
///   <entry vcenter="1" image="UI/icons/RHero3.bmp" link="RHero"> Eroe </entry>
///   <entry link="/contents/buildings/RBarracks">Si recluta nella Caserma</entry>
///   <entry/>
/// </topic>
/// ```
///
/// The shipped Italian document is 225 topics and 2,126 entries: 836 with an
/// image, 91 with a link, 151 in the `large` font, 207 centred. A link is a
/// topic id, either bare -- a child of the topic it stands in, or any topic
/// of that id -- or a path from `/contents/`; the ids are distinct, so the
/// last component resolves either. `HELP.INI` shows a topic as a list and
/// walks it with Backward, Forward, Up, Previous, Next and Home.
///
/// This is the one shipped document whose element *text* matters, and
/// `xml.hpp` deliberately keeps none; the reader here is its own, and reads
/// exactly this shape. The text arrives as UTF-8 and is kept as cp1252,
/// which the fonts index (`cp1252_from_utf8`).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::game {

struct HelpEntry {
  std::string text;   ///< cp1252, leading and trailing whitespace trimmed
  std::string link;   ///< a topic id or `/contents/...` path, or empty
  std::string image;  ///< a virtual bitmap path, or empty
  bool large = false;    ///< `font="large"`: the bold face
  bool centred = false;  ///< `hcenter="1"`
};

struct HelpTopic {
  std::string id;
  std::int32_t parent = -1;  ///< index into `topics()`, -1 for the root
  std::vector<HelpEntry> entries;
  std::vector<std::int32_t> children;  ///< in document order
};

class HelpDocument {
 public:
  [[nodiscard]] static Result<HelpDocument> parse(std::span<const std::byte> xml);

  [[nodiscard]] const std::vector<HelpTopic>& topics() const noexcept { return topics_; }
  /// The topic a link names, from the topic it stands in: a child of `from`
  /// by that id, else the last path component anywhere, else -1.
  [[nodiscard]] std::int32_t resolve(std::string_view link, std::int32_t from) const noexcept;
  /// The first topic: `contents`.
  [[nodiscard]] std::int32_t home() const noexcept { return topics_.empty() ? -1 : 0; }

 private:
  std::vector<HelpTopic> topics_;
};

/// One of `CURRENTLANG\TIPS.XML`'s tips: the text the main menu's Tips
/// frame shows, and the help topic *More Info* opens.
struct Tip {
  std::string text;  ///< cp1252, trimmed
  std::string link;  ///< a help link, `/contents/shortcuts`; may be empty
};

/// `<tips>` of `<tip link="..."><text>...</text></tip>`, the other shipped
/// document whose element text matters -- 60-odd tips in the Italian pack.
/// Read by the same hand as the help; `bad_magic` when the root is not
/// `tips`, `malformed` on a tag that does not close.
[[nodiscard]] Result<std::vector<Tip>> parse_tips(std::span<const std::byte> xml);

}  // namespace imperivm::core::game
