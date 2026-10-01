// The run-order manifest of the `.vs` host surface: sim/host_setup.hpp.
//
// Nothing here decides behaviour. It decides *order*, which for a table where
// `define` silently replaces is the same thing.

#include "imperivm/core/sim/host_setup.hpp"

#include "imperivm/core/sim/anim.hpp"
#include "imperivm/core/sim/feedback.hpp"
#include "imperivm/core/sim/boarding.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/flying.hpp"
#include "imperivm/core/sim/heading.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/wait.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/conversation.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/siege.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/text.hpp"

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

// The domain entry points differ in return type -- `register_world_host`,
// `register_economy_hosts`, `register_objlist_host`, `register_command_host`
// and `register_env_host` return a count, the rest return void -- so each is wrapped to the one shape
// the manifest holds. The
// counts they return are not used here: `HostRegistry::implemented()` is the
// measurement that also notices an entry being *overwritten*, which a
// self-reported count by construction cannot.

void define_scheduler(script::HostRegistry& registry) {
  script::register_scheduler_builtins(registry);
}

void define_world(script::HostRegistry& registry) { (void)register_world_host(registry); }

void define_combat(script::HostRegistry& registry) { define_combat_host(registry); }

void define_economy(script::HostRegistry& registry) { (void)register_economy_hosts(registry); }

void define_heroes(script::HostRegistry& registry) { define_hero_host(registry); }

void define_movement(script::HostRegistry& registry) { register_movement_host(registry); }

void define_objlist(script::HostRegistry& registry) { (void)register_objlist_host(registry); }

void define_command(script::HostRegistry& registry) { (void)register_command_host(registry); }

void define_env(script::HostRegistry& registry) { (void)register_env_host(registry); }

void define_feeder(script::HostRegistry& registry) { (void)register_feeder_hosts(registry); }

/// Ships and the units walking towards them. **After `command`**, and that is
/// documentation rather than a dependency: `NotifyBoardUnit` puts the ship on
/// its `boardunit` command through `CommandSystem`, which it finds over
/// `World::systems()` rather than over the registry, so nothing here would
/// break if it moved. It owns nine names no other domain declares.
void define_boarding(script::HostRegistry& registry) { (void)register_boarding_host(registry); }

void define_text(script::HostRegistry& registry) { (void)register_text_host(registry); }

void define_player(script::HostRegistry& registry) { (void)register_player_host(registry); }

void define_squad(script::HostRegistry& registry) { (void)register_squad_host(registry); }

void define_siege(script::HostRegistry& registry) { (void)register_siege_host(registry); }

void define_match(script::HostRegistry& registry) { (void)register_match_host(registry); }

void define_ai(script::HostRegistry& registry) { (void)register_ai_host(registry); }

void define_campaign(script::HostRegistry& registry) { (void)register_campaign_host(registry); }

void define_area(script::HostRegistry& registry) { (void)register_area_host(registry); }

/// The explored map: `IsExplored`, `ExploreArea`, `ExploreCircle`, `ExploreAll`
/// and `GetUnexploredPoint`. **After `area`**, and that is a dependency rather
/// than a preference: `ExploreArea` resolves its argument through `area_named`
/// and is written against that domain, not against the registry. It owns
/// nothing anybody else defines -- all five are free of every other domain's
/// inventory -- so the position is documentation and
/// `test_host_setup.cpp`'s overlap check is what keeps it true.
void define_fog(script::HostRegistry& registry) { (void)register_fog_host(registry); }
void define_wait(script::HostRegistry& registry) { (void)register_wait_host(registry); }

/// The animation family. **Last, and it has to be**: it owns `SetState/1`,
/// which is a squad entry point at twelve of its sixteen shipped sites and an
/// object one at the other four, and it is the only domain that answers both.
/// Nothing above defines it, so the position is documentation rather than
/// arbitration -- and `test_host_setup.cpp`'s overlap check is what keeps that
/// true.
void define_anim(script::HostRegistry& registry) { (void)register_anim_host(registry); }

/// The heading family: `GetVecByDir`, `GetDirByAngle`, `OnLeft`,
/// `GetAngleByDir`. Four free functions about points, owned by nothing else --
/// `world_host` owns the point *members* (`Rot`, `SetLen`, `Len`, `Dist`) and
/// defines none of these -- so the position is documentation rather than
/// arbitration, and `test_host_setup.cpp`'s overlap check keeps it that way.
void define_heading(script::HostRegistry& registry) { (void)register_heading_host(registry); }

/// Flight. **After `heading`, and that is a dependency rather than a
/// preference**: `PickFlyingPoint`'s scan scores each candidate with
/// `GetAngleByDir`'s own arithmetic, so this domain is written against that one
/// -- not against the registry, which is why the order is documentation here
/// too. It owns nothing anybody else defines: `world_host` has `IsInAir` and
/// `movement` has `GetDir`, and neither has `z`, `dir` or either picker.
void define_flying(script::HostRegistry& registry) { (void)register_flying_host(registry); }

/// Conversations: `RunConv`, `ConvResult`, and the three members `Init`,
/// `SetActor` and `Run`.
///
/// **The two member names are the reason this is last.** `Init` and `Run` are
/// as generic as a name gets, and member lookup here is case-insensitive, so
/// defining them is exactly the move that has silently replaced somebody else's
/// body twice on this project. Nothing above defines either -- the coverage
/// ranking lists both as *unimplemented* with 54 sites each, which is the
/// conversation count -- so this position is documentation rather than
/// arbitration, and `test_host_setup.cpp`'s overlap check is what keeps it
/// true. If a domain above ever wants `Init`, this one loses it, not the order.
void define_conversation(script::HostRegistry& registry) {
  (void)register_conversation_host(registry);
}

/// The selection: `ClearSelection`, `Select`, `Deselect`, `SwapSelectedObj`,
/// `_GetSelection`, the two `*SelectionAssigned` questions and
/// `_LastSelectionTime`.
///
/// **After `feedback`, and that is a dependency**: `IsSelectionAssigned` is a
/// question about `ShortcutTable`, which is that domain's, and this one is
/// written against it rather than against the registry. It owns nothing anybody
/// else defines -- `Select/1` and `Deselect/0` are member names generic enough
/// to be worth checking, and the coverage ranking listed both as unimplemented
/// with 11 and 2 sites -- so the position is documentation, and
/// `test_host_setup.cpp`'s overlap check is what keeps it true.
void define_selection(script::HostRegistry& registry) {
  (void)register_orders_host(registry);
}

/// What a script asks the interface to show: `CreateFeedback` and the
/// `rollover` family. Nothing else defines any of the four names -- they are
/// 4th, 5th and 6th in the coverage ranking and unclaimed -- so the position is
/// documentation rather than arbitration.
void define_feedback(script::HostRegistry& registry) {
  (void)register_feedback_host(registry);
}


/// The order. See the header before changing it.
///
///   scheduler   `Sleep`, `AIRun`, `AIBreakScript`, `StartPlayerScript`. First
///               because they are the runtime's own and belong to no world.
///   world       identity, position, owner, the class tree, queries, `rand`.
///               Second because every other domain's handles are the ones this
///               slice mints, and `object_type` is its choice.
///   combat      health, damage, targeting.
///   economy     gold, food, population, settlements.
///   heroes      armies, skills, items.
///   movement    `Goto`, paths, formations, facing.
///   objlist     `ObjList` and the collection entry points, whose values are
///               containers *of* the handles every domain above mints.
///   command     unit and building orders.
///   siege       the planner behind `ObjList::Siege` and `Squad::Siege`. After
///               `squad`, whose fresh-squad regrouping it ends with.
///   env         the map, the weather, the temples, the difficulty and the rest
///               of the ambient world. Last because it is the widest and the
///               least owned: an entry point it shares with a domain above is
///               that domain's, and running it last would be the wrong way
///               round -- so if a collision with `env` ever appears, the fix is
///               for `env` to drop the entry, not for the order to change.
///
///   campaign    `ConquestBonus`, `SetTerritoryState`, `GetTerritoryState`.
///               After `match`, because a conquest mission's sequence writes
///               its territory state and then calls `EndGame`, and because
///               none of the three is in `declare_shipped_surface` -- they are
///               reached only from scripts stored inside a conquest container,
///               which no pack carries. Nothing above defines them, so the
///               position is documentation rather than arbitration.
///
///   area        `AreaCenter`, `AttackArea`, the two area queries and the
///               point sampler. All seven are free functions, all seven name a
///               place through `NamedObjectTable`, and **no other domain
///               defines any of them** -- so this position is documentation
///               rather than arbitration, and `test_area.cpp`'s overlap check
///               is what keeps it that way. Last because it reads the object
///               model, the query table, the movement grid and the command
///               queue, and owns none of them.
///
///   conversation  `RunConv`, `ConvResult`, `Init`, `SetActor`, `Run`. **Last**,
///               because two of the five are one-word member names that any
///               domain might want; see the note above `define_conversation`.
///
///   feedback    `CreateFeedback` and the `rollover` family -- what a script
///               asks the interface to show and never reads back. Late because
///               it reads the command table and owns nothing anybody else has.
///
///   selection   what the player has clicked on, and the two questions about
///               numbered control groups that only look like they are about it.
///               After `feedback`, whose `ShortcutTable` holds the groups.
///
///   text        translation and the string entry points.
///   player      players and diplomacy. **Last on purpose**: it owns
///               `IsEnemy/1`, which combat also defined for a while with a
///               symmetric ally table of its own, and which the corpus calls
///               with an object argument *and* with an integer player. Combat
///               has given the entry point up rather than relying on the order,
///               so this position is belt and braces.
///
/// Nothing is listed here on optimism. Naming a registration function that does
/// not link, or including a header that does not compile, breaks the build for
/// every other domain, and `objlist`, `command`, `env` and `player` each sat in
/// this paragraph until they built.
///
/// `test_host_setup.cpp` then measures it for overlap along with the rest, and
/// will say by name which domain lost an entry point to which.
constexpr HostDomain kDomains[] = {
    {"scheduler", &define_scheduler}, {"world", &define_world},
    {"combat", &define_combat},       {"economy", &define_economy},
    {"feeder", &define_feeder},
    {"heroes", &define_heroes},       {"movement", &define_movement},
    {"objlist", &define_objlist},     {"command", &define_command},
    {"env", &define_env},             {"text", &define_text},
    {"player", &define_player},        {"squad", &define_squad},
    {"siege", &define_siege},
    {"match", &define_match},          {"ai", &define_ai},
    {"campaign", &define_campaign},  {"area", &define_area},
    {"wait", &define_wait},        {"anim", &define_anim},
    {"fog", &define_fog},
    {"heading", &define_heading},  {"flying", &define_flying},
    {"conversation", &define_conversation}, {"feedback", &define_feedback},
    {"selection", &define_selection},
    {"boarding", &define_boarding},
};

}  // namespace

std::span<const HostDomain> host_domains() noexcept { return kDomains; }

std::size_t register_all_hosts(script::HostRegistry& registry) {
  const std::size_t before = registry.implemented();
  // Declared first, always: an entry point no domain has reached yet must trap
  // with its own name and arity rather than read as an unknown call.
  script::declare_shipped_surface(registry);
  for (const HostDomain& domain : kDomains) domain.define(registry);
  return registry.implemented() - before;
}

}  // namespace imperivm::core::sim
