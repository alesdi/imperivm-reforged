#pragma once

/// A retail installation, opened and held.
///
/// Everything the core needs to build a `sim::GameSession` arrives as bytes,
/// because `engine/core` may not open a file. Somebody has to do the opening,
/// and until now two of them did: `engine/tools/imrun.cpp` grew a pack reader,
/// a class-graph builder, a container loader and a script resolver, and the
/// windowed application was about to grow the same four. This is that code,
/// once.
///
/// **It owns every byte it hands out.** The class graph is a tree of
/// `std::string_view`s into the XML it was parsed from, `SessionInputs` is a
/// bundle of spans, and a `ScriptResolver` is asked for source long after load.
/// All of it borrows, so this object has to outlive the session built from it.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/gamedata/map_source.hpp"


namespace imperivm::gamedata {

/// One opened installation: `Packs/data.pak` and the class graph from it.
class Installation {
 public:
  Installation();
  ~Installation();
  Installation(const Installation&) = delete;
  Installation& operator=(const Installation&) = delete;

  /// Open `game_dir`. `error` receives a human-readable reason on failure.
  bool open(const std::filesystem::path& game_dir, std::string* error = nullptr);

  [[nodiscard]] const core::ClassGraph& classes() const noexcept { return graph_; }
  /// How many `.SC.XML` files the graph was built from. 845 in a retail install.
  [[nodiscard]] std::size_t class_files() const noexcept { return class_files_; }

  /// Serves `.vs` source out of `data.pak`, case- and separator-insensitively,
  /// falling back to the basename -- the same three rules
  /// `Scheduler::find_chunk` applies, and for the same reason: class bindings
  /// spell the same file four ways.
  [[nodiscard]] core::sim::ScriptResolver& scripts() noexcept;

  /// Every `DATA/COMMANDS/*.XML`, in sorted name order. Spans into the pack.
  [[nodiscard]] std::span<const std::span<const std::byte>> commands() const noexcept;
  /// The file names under `DATA/BONUSSCRIPTS/`, sorted: `000 NONE.VS`,
  /// `001 WEALTH.VS`, ... -- what `GameSession::start_bonuses` keys by
  /// their leading number.
  [[nodiscard]] std::span<const std::string> bonus_scripts() const noexcept;
  /// `DATA/GameScripts/*.vs` by basename, sorted: `1 ELIMINATION`, `2 SCORE
  /// LIMIT`, ... -- the setup's game types, which 0x006c4750 lists the same
  /// way (the directory, one item per file, the text after the leading
  /// number, the data the whole basename).
  [[nodiscard]] std::span<const std::string> game_scripts() const noexcept;

  /// `DATA/CONST.INI` and `DATA/AI/AI.INI`, empty if absent.
  [[nodiscard]] std::span<const std::byte> constants() const noexcept;
  /// `DATA/FORMATIONS.XML` -- the six formation classes and the default.
  [[nodiscard]] std::span<const std::byte> formations() const noexcept;
  [[nodiscard]] std::span<const std::byte> ai_profile() const noexcept;
  /// `DATA/ITEMS.XML` -- the 42 items, for the hero system's item store.
  [[nodiscard]] std::span<const std::byte> items() const noexcept;
  /// `DATA/SKILLS.INI` and `DATA/UNIT_SPECIALS.INI` -- the hero skills' and the
  /// unit specials' icons and texts, for the info bar.
  [[nodiscard]] std::span<const std::byte> skills() const noexcept;
  [[nodiscard]] std::span<const std::byte> unit_specials() const noexcept;

  /// One file out of `data.pak` by its pack name, empty if absent.
  [[nodiscard]] std::span<const std::byte> file(std::string_view pack_name) const noexcept;
  /// One file out of the art packs the entities come from -- an entity's
  /// `.pass` mask, `MAPOBJECTS\DECORS\DECORS.INI` -- empty if no pack holds
  /// it. The packs are opened on the first ask, as `entities()` opens them.
  [[nodiscard]] std::span<const std::byte> art_file(std::string_view pack_name) const noexcept;

  /// Serves `.ent.xml` definitions out of the art packs beside `data.pak` --
  /// `Units.pak`, `Buildings.pak`, `MapObjects.pak`, `Visuals.pak` on a retail
  /// install -- loaded on first use and kept for the life of the installation. **The headless tools ran with no
  /// entities for as long as this did not exist**, which is not a rendering
  /// detail: `Unit::GetAnimDuration` reads an animation's length off the
  /// entity, `EAGLE_MOVE.VS` divides by it, and every eagle on every map
  /// divided by zero in `imrun` while flying perfectly well in the app. Each
  /// image's grid is corrected from its sheet's frame table, as the app's own
  /// resolver corrects it: an animation's length is its sheet's rows, so the
  /// grid is simulation, not drawing. Without that, `imrun` and the app ran
  /// Crossroads apart from its fourth turn.
  [[nodiscard]] core::sim::EntityResolver& entities() noexcept;
  /// Serves the entities' `.pass` masks out of the same packs, parsed once
  /// and kept -- what the session's passability rebuild at match start
  /// stamps, and what `immap passability` proves against every shipped layer.
  [[nodiscard]] core::sim::PassMaskResolver& masks() noexcept;

  /// `Packs/RandomMapSettlements.bfhp`'s `Maps/1/map.obj.xml`: the 64
  /// settlement templates a `Mutable` stronghold or village becomes at match
  /// start. Read on first use and kept; empty when the pack is not there or
  /// does not hold the document. See `SessionInputs::settlement_templates`.
  [[nodiscard]] std::span<const std::byte> settlement_templates() noexcept;
  /// The same pack's `Maps/1/Terrain.terrain.grid`: the ground each template
  /// brings with it. See `SessionInputs::settlement_template_terrain`.
  [[nodiscard]] std::span<const std::byte> settlement_template_terrain() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  core::ClassGraph graph_;
  std::size_t class_files_ = 0;
};

/// Scripts, looked for in the container first and in `data.pak` second.
///
/// A container's own `.vs` files are in no pack, and until this existed nothing
/// could compile one: `Installation` resolves against `data.pak` alone, so
/// every `Maps/<n>/Sequences/seq*.vs` in the installation was unreachable
/// source and the whole campaign layer was dead code with no way to notice.
/// The chain is ordered the way the retail engine's virtual filesystem is --
/// `CurrentGame/` and `CurrentMap/` name the container, and a bare
/// `data/ai/…` name falls through to the pack.
class ContainerScripts final : public core::sim::ScriptResolver {
 public:
  ContainerScripts(const MapContainer& container, core::sim::ScriptResolver& fallback)
      : container_(&container), fallback_(&fallback) {}

  std::span<const std::byte> source(std::string_view path) override;

  /// How many distinct paths each half of the chain answered. Diagnostics only.
  [[nodiscard]] std::size_t from_container() const noexcept { return from_container_; }
  [[nodiscard]] std::size_t from_pack() const noexcept { return from_pack_; }

 private:
  const MapContainer* container_ = nullptr;
  core::sim::ScriptResolver* fallback_ = nullptr;
  /// The container copies bytes out on every read, so they have to be held for
  /// as long as the compiler's span into them lives.
  std::vector<std::unique_ptr<std::vector<std::byte>>> held_;
  std::size_t from_container_ = 0;
  std::size_t from_pack_ = 0;
};

/// The payloads a session needs out of an already-open container.
///
/// **`MapContainer` is not redefined here.** `map_source.hpp` has one, it
/// already unwraps an LZIS stream around the container, and it already reads
/// any path out of it -- this file grew a second one for an afternoon and the
/// linker caught it, which is the cheapest way that mistake has ever been
/// caught in this project. This is the small part that was actually missing:
/// knowing *which* paths a session wants.
struct MapPayloads {
  std::vector<std::byte> objects;  ///< `Maps/<n>/map.obj.xml`
  /// `Maps/<n>/Terrain.pass.grid` -- the authored obstruction bitmap, empty if
  /// absent. One of the map's six `GRID` layers and the only one the simulation
  /// takes: it is the 16-unit, one-bit-per-cell one. See `SessionInputs`.
  std::vector<std::byte> passability;
  std::vector<std::byte> game;     ///< `game.xml`, empty if absent
  /// The container root's `itemsCustom.xml` -- the items a map defines for
  /// itself, on top of `DATA/ITEMS.XML`. Empty (21 bytes of `<items/>`) in
  /// most containers; the campaigns' druid ash and amulets live here.
  std::vector<std::byte> custom_items;
  /// `Maps/<n>/map.xml` -- the map's name and extent, empty if absent.
  ///
  /// Read by path for the same reason the obstruction layer is: a conquest
  /// keeps one per map. Nothing read it until `MatchRules::map_size` turned out
  /// to be zero in every session -- see `SessionInputs::map_properties`.
  std::vector<std::byte> map_properties;
  /// `Maps/<n>/Terrain.terrain.grid` -- the terrain-type layer, empty if
  /// absent. Read for `IsPointInWater` and for nothing else. See
  /// `SessionInputs::terrain`.
  std::vector<std::byte> terrain;
  /// `Maps/<n>/Terrain.height.grid` -- the elevation layer, empty if the
  /// container has none. Handed straight to `SessionInputs::height`.
  std::vector<std::byte> height;
  /// `Maps/<n>/Terrain.decor.grid` -- the decorations, whose masks the
  /// passability rebuild at match start stamps. `SessionInputs::decor`.
  std::vector<std::byte> decor;
  /// The sixteen `player<i>.xml` payloads in id order; absent slots are empty.
  std::vector<std::vector<std::byte>> players;
  /// Views over `players`, in the shape `SessionInputs` wants.
  std::vector<std::span<const std::byte>> player_spans;

  std::vector<std::byte> conquest;  ///< `territories.xml`, conquests only
  /// The container's root `Notes.xml` and the chosen map's `Maps/<n>/Notes.xml`.
  /// Both may be empty; 36 of the installation's 50 note documents declare
  /// nothing. The `Local/<language>` copies are deliberately not read.
  std::vector<std::byte> notes;
  std::vector<std::byte> map_notes;
  /// `Sequences/sequences.xml` at the container root, and the map's own
  /// `Maps/<n>/Sequences/sequences.xml`. These are the campaign layer: 49
  /// manifests over the 24 containers, 308 sequences, and nothing in the engine
  /// read one until `imrun` learned to.
  std::vector<core::sim::SequenceRef> game_sequences;
  std::vector<core::sim::SequenceRef> map_sequences;
  /// Every `.conv.xml` the container holds outside `Local/`, in path order.
  ///
  /// Not two documents the way the notes are: a map keeps one file per
  /// conversation and the tutorial keeps 23 of them. The `Local/<language>`
  /// copies share the extension and are a `<translationtable>`, so reading by
  /// suffix alone would put every conversation in twice under a translation --
  /// the trap `Notes.xml` has, with the same answer.
  std::vector<std::vector<std::byte>> conversations;
  /// Views over `conversations`, in the shape `SessionInputs` wants.
  std::vector<std::span<const std::byte>> conversation_spans;
  std::string map_directory;  ///< `Maps/<n>` the objects came from, or empty

  [[nodiscard]] bool ok() const noexcept { return !objects.empty(); }
};

/// Every `.xml` under `Local/<language>/` in an open container -- its own
/// translation tables: `adventure.loc.xml`, `itemsCustom.loc.xml`, the
/// notes and the conversations -- as documents for `SessionInputs::
/// localisations`. The language is matched case-blind; a container with no
/// such directory gives nothing.
[[nodiscard]] std::vector<std::vector<std::byte>> read_localisation(const MapContainer& container,
                                                                    std::string_view language);

/// Read a session's payloads out of an open container.
///
/// `map_index` selects among `Maps/<n>` when a container holds several -- a
/// conquest uses 3, 4 and 6 to 10, so the numbers are enumerated rather than
/// counted. Empty means "the first one".
[[nodiscard]] MapPayloads read_payloads(const MapContainer& container,
                                        std::string_view map_index = {});

/// Fill a `SessionInputs` from an installation and a map's payloads.
///
/// Both must outlive the session: every field is a span into one of them.
[[nodiscard]] core::sim::SessionInputs session_inputs(Installation& install,
                                                      const MapPayloads& map);

}  // namespace imperivm::gamedata
