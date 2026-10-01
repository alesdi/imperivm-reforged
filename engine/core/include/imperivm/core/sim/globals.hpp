#pragma once

/// The 245 bare identifiers the shipped scripts read as globals.
///
/// `docs/formats/vs-host-api.md` counts 245 names that are neither locals nor
/// parameters -- `AI_COMING`, `AIV_Research`, `cWarrior`, `SS_Approach`,
/// `Gaul`. `script::Host::global` is what the VM asks for them, and until this
/// file existed it refused all 245, which put essentially every AI script one
/// instruction from a trap. This is the table it asks.
///
/// The names come from four different places and only one of them is a table
/// in the executable. Keeping them apart is the whole point of this header:
/// each family is answered by whoever actually owns its numbering, and a name
/// nobody owns **refuses** rather than returning a plausible zero.
///
/// ## 1. `engine_constants()` -- the table in `gbr.exe`
///
/// The retail engine registers its constants one call at a time into a
/// `std::map<std::string, int>` through a single function at `0x0069c3a0`.
/// There are exactly 49 static call sites; 47 pass a literal name and value,
/// and the remaining two are loops over a pointer table:
///
///   * `0x005317bf` walks 25 names at `0x00824b68` -- the `hs*` hero skills --
///     registering each with its index, then registers `HeroSkillsCount = 25`;
///   * `0x005dfdff` walks 36 names at `0x00824c80` -- the unit specials --
///     the same way, then `UnitSpecialsCount = 36`.
///
/// One more name, `SS_IDLE = 0`, is registered at `0x004435c6` through a second
/// registrar (`0x0069bd40`) that writes into a *different* map on the same
/// object -- `+0x40` rather than `+0x28`. That is the map the `AI.INI` enum
/// names go into, which is why the sentinel is there and not in the first one.
///
/// So the engine's constant name space is exactly 109 names, and this is all
/// of them, transcribed with their literal values. Nothing here is inferred.
/// Two independent corroborations fell out of the transcription:
///
///   * the 36 unit-special names are in the same order as the 36 section
///     headings of `DATA\UNIT_SPECIALS.INI` (`[Parry]`, `[Drain]`,
///     `[Ferocity]`, ... `[Curse]`), which is what makes the value an index
///     into that file rather than an arbitrary id;
///   * the eight race values match `sim/player_host.hpp`'s `Race`, which was
///     read out of the same block at `0x005b70b2` independently.
///
/// Names the shipped corpus never reads are kept anyway -- `AI_NONE`,
/// `AI_FRIENDLY`, `SF_SENTRIES`, `SF_WANTDRUIDS`, `hsEgoism`, the `dt*` damage
/// types, the `ts*` territory states, the three `c*` difficulty levels and the
/// three `*Count` totals. They are engine surface, a map may use them, and
/// dropping evidence because the shipped scripts happen not to exercise it is
/// how a table starts disagreeing with the thing it was read from.
///
/// ## 2. `SS_` / `GS_` / `ES_` / `TS_` -- the AI profile
///
/// Data, not code: `DATA\AI\AI.INI` declares them and `sim/ai_profile.hpp`
/// reads them. Only the four sentinels `SS_IDLE`, `GS_NONE`, `ES_NONE` and
/// `TS_NONE` are in the executable's table -- all four as `0`, which is
/// visible in the block above and which settles the numbering origin
/// `docs/formats/ai-ini.md` recorded as inferred.
///
/// A profile is not reachable from a `World`, so `WorldHost` carries a pointer
/// to one and these 42 names refuse until an embedder sets it.
///
/// ## 3. `AIV_` / `AIMV_` -- the AI variable id space
///
/// `AIVar`'s second argument is an integer id, and `EnvSystem::ai_var_id` is
/// the only place the name-to-id mapping exists. It is asked directly rather
/// than recomputed here: the id is internal -- it indexes `AiVarStore`, which
/// `EnvSystem::seed_ai_vars` fills through the same mapping -- so the one thing
/// that must hold is that the reader and the writer agree, and they cannot
/// disagree if there is one table. A world with no `EnvSystem`, or one whose
/// name table was never seeded, refuses all 118.
///
/// ## 4. What is *not* a global at all
///
/// Three groups in the 245 are miscounted by the inventory, and this file
/// answers none of them:
///
///   * **The six ambient command names.** `cmdparam`, `cmdwaiting` and the
///     four `cmdcost_*` are registered in `gbr.exe` as **zero-argument host
///     functions**, in the same table and through the same registrar as
///     `rollover` and `Place`: `0x005b7cff` registers `cmdparam` returning
///     type `0x0b` (`str`) with zero arguments, and `0x005b7da5` registers
///     `cmdcost_gold` returning type `0x01` (`int`) with zero arguments. The
///     VS compiler resolves a bare name as a zero-arity free function *before*
///     it asks for a global, so binding them as functions is both what the
///     original does and what makes them reachable at all -- a `Host::global`
///     implementation cannot see which script is asking, and the answer
///     depends entirely on that. `register_global_hosts` binds them.
///   * **The seven parenthesis-less calls.** `AIGetPlayer`, `Breakpoint`,
///     `GAIKACount`, `GetMapRect`, `GetTime`, `MapSize` and `MaxSetIdx` are
///     already free functions in `declare_shipped_surface`, and the compiler
///     already resolves them. Duplicating them here would shadow a real call
///     with a constant.
///   * **`owner` and `this`.** Both are one use each and neither is a global.
///     `owner` is the first *parameter* of every `DATA\ITEMSCRIPTS` entry point
///     (`//void, Obj owner`); `this` is used as an ordinary assignable local in
///     the same scripts (`this = owner.AsUnit();`). One file each is missing or
///     misspelling the signature comment the analyser reads, which is what put
///     them in the inventory.
///
/// ## 5. `c*` -- one string constant per class, minted from the class graph
///
/// The `c*` names are **not** in the executable's constant table and never
/// were; that is why they refused for as long as this file has existed. They
/// are minted from `DATA\CLASSES\*.SC.XML` at load time, and they are
/// **strings**, not integers.
///
/// `gbr.exe` has *two* constant registrars writing into two different maps on
/// the object at `[0x0082a500 + 0x4010]`:
///
///   * `0x0069c3a0` -- `(name, int)` into the map at `+0x28`. 49 static call
///     sites, all of them `mov ecx, 0x82a500` (e.g. `0x00443480`), which is
///     the 109-name table above.
///   * `0x0069c480` -- `(name, str)` into the map at `+0x34`. **One** call
///     site, `0x0059c1f9`, inside the loop at `0x0059c100`.
///
/// That loop walks the class registry and, for each class, does
/// `0x004a5080` -> the class's `id` (the `std::string` at `class + 4`; the
/// `<class id=…>` attribute is parsed into that field at `0x0059f604`, `altid`
/// goes to `+0x20` and `entity` to `+0x74`), then `0x0059c060` to sanitise it
/// (first character not `isalpha` -> `_`, later characters not `isalnum` ->
/// `_`), then registers
///
///     name  = "c" + sanitised id        // the literal "c" is at 0x007b6c0c-4
///     value =       sanitised id
///
/// The compiler reads that map. A bare identifier that is not a local is
/// looked up at `0x00692524` in this order: `0x00692100` searches the int map
/// at `+0x28` and emits an integer constant; `0x006922f0` searches the string
/// map at `+0x34` and emits a **string** constant; only then does
/// `0x00690b90` try to resolve the name as a function call. So the engine's
/// own integers answer first and a class can never shadow one -- which is the
/// order `resolve_global` uses, for the same reason.
///
/// Every one of the 27 `c*` names the corpus reads -- the 16 in `data.pak` and
/// the 11 more inside the containers (`cUnit` 32 sites, `cRanged` 28,
/// `cSentry` 13, `cPeaceful` 4, `cCJavelinThrower` 3, `cCBerberAssassin` 2,
/// `cCLibyanFootman` 2, `cCatapult` 2, `cCNumidianRider` 1, `cShipBattle` 1,
/// `cGGhost` 1) -- is `c` followed by a shipped `<class id>`, exactly, with no
/// `altid` needed. The controls are what make that mean something:
///
///   * of the other 229 bare names in `data.pak`, **0** are claimed by the
///     rule;
///   * of the 1,995 distinct `<group>` names in the 28 shipped maps, **0** are
///     claimed by it;
///   * of 611 adversarial controls (`"c" + <a real group name>`), 3 are.
///
/// And the shape agrees: all 38 `data.pak` uses and all 89 container uses sit
/// in a `str` argument position -- `ClassPlayerObjs(str, int)`,
/// `ClassPlayerAreaObjs(str, int, str)`, `Count(int, str)` -- where an integer
/// could not go. So this is a *resolution step against the loaded class
/// graph*, not more rows in `kEngineConstants`: a world with no class graph
/// refuses all of them, and a map that ships its own class would get its own
/// constant for free, exactly as `0x0059c100` gives it one.
///
/// ## 6. The map's own `<group>` names
///
/// 611 bare identifiers in the 308 container scripts appear in no `data.pak`
/// script at all, over 3,226 sites; 596 of them, over 3,118 sites, are
/// `<group>` names declared by the map the script belongs to.
///
/// **The original does not resolve these as globals.** `RunSequence`
/// (`0x005bc170`) hands the script text to `0x005bc280`, which builds a
/// prologue out of the current map's two registries and prepends it:
///
///     NamedObj <n>;           // per <group type="0">, loop at 0x005bc500
///     Query    <n>;           // per <group type="1">, loop at 0x005bc640
///     <n>=GetNamedObj("<n>"); //                       loop at 0x005bc760
///     <n>=Group("<n>");       //                       loop at 0x005bc8ee
///     {                       // 0x007b6bfc, appended at 0x005bca6a
///     …the script as authored…
///     \n}                     // 0x007b37f0
///
/// -- skipping any name `0x004b4280` rejects as not an identifier, which is
/// what the editor's `" is not a valid identifier."` warning is about and what
/// keeps the seven shipped `<group name="1EDrop1">`-style names out. So in the
/// original these names are **locals**, of type `NamedObj` or `Query`.
///
/// This engine resolves them in `resolve_global` instead, because the compile
/// path a prologue would have to be spliced into is not this file's. The two
/// readings differ in exactly one way -- a local is assignable and a global is
/// not -- and no shipped script assigns to one. They agree on everything else,
/// including the types: a `<group type="0">` resolves to a `NamedObj`
/// (`kTypeNamedObj`, and `.obj` is how every one of the 1,174 `.obj` sites in
/// the corpus reaches the object behind it), a `<group type="1">` to the same
/// `Query` value `Group("<n>")` returns.
///
/// Named objects are tried before groups because `gbr.exe` says which wins:
/// `NAMEDOBJ_OVERRIDES_GROUP`, and the editor warns that a name "used for both
/// a group and a named object" makes *the group* unreachable from sequence
/// scripts. No shipped map does it -- 0 of 1,995 names carry both types inside
/// one map -- so the order is unobservable on retail data and recorded rather
/// than relied on.
///
/// ## The tally
///
/// Of the 245 names `script::shipped_global_names()` lists:
///
///   * **229 resolve** through `resolve_global` -- 57 from the engine's table
///     (the seven `AI_`, three `SF_`, 20 `hs*`, six `gs*`, eight races, seven of
///     the eight unit specials, `UNITFLAG_NOAI`, `MaxGAIKAPriority` and the four
///     family sentinels), 38 from the AI profile's four sections, all 118
///     `AIV_`/`AIMV_` from the env system, and the 16 `c*` from the class graph;
///   * **13 are host functions, not globals** -- the six ambient command names
///     and the seven parenthesis-less calls. Twelve of the thirteen are
///     implemented; `cmdwaiting` is declared and traps by name;
///   * **3 refuse on purpose** -- `revitalize`, `owner` and `this`. Each is
///     refused for a reason recorded above or in
///     `engine/tests/test_globals.cpp`, and none of them is a name a value
///     could be invented for without changing what a shipped script does.
///
/// ## Iteration order
///
/// The constant table is a sorted array searched by binary search. No hash, no
/// unordered container: `docs/engine/architecture.md` makes iteration order
/// part of the simulation's definition, and a table that answers 245 names on
/// every script that starts is not a place to make an exception.

#include <cstdint>
#include <span>
#include <string_view>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"

namespace imperivm::core::sim {

class World;
class AiProfile;

/// A named object's handle: `map.obj.xml`'s `<group type="0">`.
///
/// The id it carries is the index into `World::named_objects()`, not the bound
/// object's id. The binding outlives its object on purpose (`World::despawn`
/// leaves the table alone, because `gbr.exe` gives `NamedObj` an `IsDead`
/// member), so the handle has to name the *entry* for `IsDead` to have anything
/// to answer about.
///
/// `gbr.exe` makes `NamedObj` its own script type -- code `0x22`, registered at
/// `0x0055342c` alongside `GetNamedObj` (`0x22, 1 arg, 0x0b`) and its three
/// members: `IsValid` and `IsDead` (`0x0055345b`, `0x00553471`; return `7`,
/// receiver `0x22`) and `obj` (`0x0055348d`; returns `0x14`, `Obj`). The number
/// here is this engine's own, continuing `sim/world_host.hpp`'s `kTypeObj`=1
/// .. `kTypeSettlement`=4, `sim/objlist.hpp`'s `kTypeObjList`=5 and
/// `sim/squad.hpp`'s `kTypeSquad`=6; it lives in this header rather than in
/// `world_host.hpp` only because this is where the named-object globals are
/// resolved.
inline constexpr script::TypeId kTypeNamedObj = 7;

/// One name the engine itself defines, with the value `gbr.exe` gives it.
struct GlobalConstant {
  std::string_view name;
  std::int32_t value = 0;
};

/// Every constant `gbr.exe` registers, sorted by name. 109 of them.
[[nodiscard]] std::span<const GlobalConstant> engine_constants() noexcept;

/// One of them, by exact (case-sensitive) name. The engine's table is a
/// `std::map<std::string, int>`, so its lookup is case-sensitive too.
/// The size of the unit-specials table: 36 names, and the executable's own
/// `UnitSpecialsCount` constant says so. `Unit::HasSpecial` refuses an index
/// this large or larger before it shifts.
inline constexpr std::int32_t kUnitSpecialsCount = 36;


[[nodiscard]] bool engine_constant(std::string_view name, std::int32_t& out) noexcept;

/// Resolve one bare identifier.
///
/// `world` supplies the `AIV_*` id space through its `EnvSystem`, the class
/// graph the `c*` constants are minted from, and the two `<group>` tables;
/// `profile` supplies the four `AI.INI` families. Either may be null, and so
/// may any of the world's three sources -- a name that needed a missing one
/// refuses (`FormatError::not_found`) rather than resolving to zero.
///
/// The order is: the engine's own table, `AIV_`/`AIMV_`, the four profile
/// families, the class graph's `c*`, then the map's named objects and groups.
///
/// **The engine's own table is first, and that is not a preference.** It is the
/// only source that cannot vary with loaded data, so nothing a map or a class
/// file brings in can shadow a compiled-in constant. It is also the order
/// `gbr.exe` compiles in: `0x00692524` searches the int-constant map at
/// `+0x28` (`0x00692100`) before the string-constant map at `+0x34`
/// (`0x006922f0`), and only then tries the name as a call (`0x00690b90`). On
/// retail data the order is unobservable anyway -- of the 1,995 distinct
/// `<group>` names in the 28 shipped maps, 0 collide with one of the 109
/// engine constants and 0 are claimed by the `c*` rule -- which is the
/// measurement, not the argument.
[[nodiscard]] Result<script::Value> resolve_global(std::string_view name, World* world,
                                                   const AiProfile* profile);

/// The string `gbr.exe` would have registered for the constant `name`, or
/// empty when `name` is not a `c<class id>` the graph knows.
///
/// Split out so a test can exercise the sanitiser (`0x0059c060`) directly:
/// 72 of the 845 shipped `<class id>` values contain a character an identifier
/// cannot, so their constant is spelled with `_` in place of it and *carries
/// the same substitution as its value* -- `cRock_Large_01` is the string
/// `"Rock_Large_01"`, which no class is called. That is the original's own
/// behaviour and not a rounding of it; no shipped script reads one.
[[nodiscard]] std::string_view class_constant(std::string_view name, const World* world) noexcept;

/// Bind the ambient command names as the zero-argument host functions they are.
///
/// Returns how many entry points were defined, so a caller can assert against
/// the count rather than trust it. `register_world_host` calls this; see the
/// header note on why they are functions and not globals.
std::size_t register_global_hosts(script::HostRegistry& registry);

/// How many `register_global_hosts` defines.
[[nodiscard]] std::size_t global_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
