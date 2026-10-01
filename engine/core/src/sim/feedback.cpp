// What a script asks the interface to show.
// See include/imperivm/core/sim/feedback.hpp.

#include "imperivm/core/sim/feedback.hpp"

#include <algorithm>

#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::Value;

/// The icon markup the retail formatter uses for each cost. Its own literals
/// name four of these -- attack, piercing, defence and heart -- and the pattern
/// is one `<imagetransp path>` per quantity; these are the four a `<cmd>` row
/// can actually charge. See `docs/formats/interface-ini.md` for the markup.
constexpr std::string_view kGoldIcon = "<imagetransp gameres/infobar/common/gold ico.bmp> ";
constexpr std::string_view kFoodIcon = "<imagetransp gameres/infobar/common/food ico.bmp> ";
constexpr std::string_view kPopIcon = "<imagetransp gameres/infobar/common/pop ico.bmp> ";
constexpr std::string_view kStaminaIcon =
    "<imagetransp gameres/infobar/common/stamina ico.bmp> ";

void append_line(std::string& out, std::string_view text) {
  if (text.empty()) return;
  if (!out.empty()) out.push_back('\n');
  out.append(text);
}

void append_cost(std::string& out, std::string_view icon, std::int32_t amount, bool& any) {
  if (amount <= 0) return;
  if (!any) {
    if (!out.empty()) out.push_back('\n');
    any = true;
  } else {
    out.push_back(' ');
  }
  out.append(icon);
  out.append(std::to_string(amount));
}

/// The `CommandDef` the context says is being described, or null.
[[nodiscard]] const CommandDef* described(CallContext& ctx) {
  auto* context = static_cast<HostContext*>(ctx.user);
  if (context == nullptr || context->world == nullptr) return nullptr;
  if (context->described_command.empty()) return nullptr;
  CommandSystem* commands = command_system(*context->world);
  if (commands == nullptr) return nullptr;
  return commands->table().find(context->described_command);
}

/// The row for a named class's own command, or null.
///
/// `rollover(this, class, show_cost)` shows the cost of training *that class*
/// rather than the command's own, and the command table is keyed by command
/// name -- so the class name is looked up as a command name, which is what the
/// shipped `<cmd name="BSwordsman" sclass="BSwordsman">` rows make work.
[[nodiscard]] const CommandDef* row_for_class(CallContext& ctx, std::string_view class_name) {
  auto* context = static_cast<HostContext*>(ctx.user);
  if (context == nullptr || context->world == nullptr || class_name.empty()) return nullptr;
  CommandSystem* commands = command_system(*context->world);
  if (commands == nullptr) return nullptr;
  return commands->table().find(class_name);
}

/// `CreateFeedback(effect, target)` -- 72 sites, 50 scripts, and it stores
/// nothing.
///
/// See the header for the whole argument. The short version: the sparkle is not
/// simulation state, but *dying before the healing* is, and until this existed
/// `HEALING HERBS.VS` trapped on its own first statement.
///
/// The second argument is a unit at all 72 shipped sites; the point form is
/// registered by `gbr.exe` and never called, and both reach this body because
/// the registry keys on arity. Neither shape is refused: a handle that does not
/// resolve is an effect with nowhere to appear, which is not an error the
/// script can do anything about.
HostOutcome fn_create_feedback(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CreateFeedback: no world");
  if (ctx.count() < 2) return HostOutcome::ok_void();
  // Resolved and dropped. The original resolves it too, and an entry point that
  // answers without looking is a test of nothing.
  if (ctx.arg(1).is_object()) (void)world->find(ctx.arg(1).as_object().id);
  return HostOutcome::ok_void();
}

/// `rollover()` -- the described command's own two strings and nothing else.
HostOutcome fn_rollover_0(CallContext& ctx) {
  return HostOutcome::ok_with(Value::string(compose_rollover(described(ctx), {}, false)));
}

/// `rollover(obj, bool)` and `rollover(obj, str)`.
///
/// One arity, two shapes, told apart by the argument's runtime type -- the
/// arrangement `UnitsInSettlement` already has, and for the same reason: the
/// registry keys on `(kind, name, arity)`, so one arity is one body.
HostOutcome fn_rollover_2(CallContext& ctx) {
  if (ctx.count() < 2) {
    return HostOutcome::ok_with(Value::string(compose_rollover(described(ctx), {}, false)));
  }
  const Value& second = ctx.arg(1);
  if (second.is_string()) {
    return HostOutcome::ok_with(
        Value::string(compose_rollover(described(ctx), second.as_string(), false)));
  }
  return HostOutcome::ok_with(
      Value::string(compose_rollover(described(ctx), {}, second.truthy_scalar())));
}

/// `rollover(obj, class, bool)` -- the cost of *that class* rather than the
/// command's own. Every one of the 15 sites passes a variable built from
/// `cmdparam` and called `class` or `strUnit`.
HostOutcome fn_rollover_3(CallContext& ctx) {
  if (ctx.count() < 3) return HostOutcome::ok_with(Value::string(""));
  const CommandDef* row = ctx.arg(1).is_string() ? row_for_class(ctx, ctx.arg(1).as_string())
                                                 : nullptr;
  // A class the command table does not name falls back to the described row,
  // which is the tooltip the player would otherwise see rather than a blank.
  if (row == nullptr) row = described(ctx);
  return HostOutcome::ok_with(
      Value::string(compose_rollover(row, {}, ctx.arg(2).truthy_scalar())));
}

/// `rollover_desc(obj, message, bool)` -- the same signature as `rollover/3`
/// and a different meaning for the middle argument. Its one shipped site is
/// `rollover_desc(this, "Already collected", false)`.
HostOutcome fn_rollover_desc_3(CallContext& ctx) {
  if (ctx.count() < 3) return HostOutcome::ok_with(Value::string(""));
  const std::string_view message = ctx.arg(1).is_string() ? ctx.arg(1).as_string()
                                                          : std::string_view{};
  return HostOutcome::ok_with(
      Value::string(compose_rollover(described(ctx), message, ctx.arg(2).truthy_scalar())));
}

// --------------------------------------------------------------------------
// the camera, the chrome and the control groups
// --------------------------------------------------------------------------

/// The view behind a call, or null. Null is a real case: a headless run has no
/// screen, and every entry point here has to degrade rather than refuse.
[[nodiscard]] ViewState* view_of(CallContext& ctx) noexcept {
  auto* context = static_cast<HostContext*>(ctx.user);
  return context == nullptr ? nullptr : context->view;
}

[[nodiscard]] ShortcutTable* shortcuts_of(CallContext& ctx) noexcept {
  auto* context = static_cast<HostContext*>(ctx.user);
  return context == nullptr ? nullptr : context->shortcuts;
}

/// `View(pt, bLock)` -- 35 sites, every one inside a map container.
///
/// The shipped idiom saves the camera with `ViewPos()`, moves it here, and
/// moves it back; both halves are this entry point.
HostOutcome fn_view(CallContext& ctx) {
  ViewState* view = view_of(ctx);
  if (view == nullptr || ctx.count() < 1) return HostOutcome::ok_void();
  const Point where = unpack_point(ctx.arg(0));
  view->x = where.x;
  view->y = where.y;
  // The second argument is `bLock` in the executable's own signature. The
  // cutscene idiom passes `true` on the way out and `false` on the way back,
  // which is consistent with it and is all the corpus says; see the header.
  if (ctx.count() >= 2) view->locked = ctx.arg(1).truthy_scalar();
  return HostOutcome::ok_void();
}

/// `ViewPos()` -- 7 sites, and the reason the view is state at all.
HostOutcome fn_view_pos(CallContext& ctx) {
  const ViewState* view = view_of(ctx);
  if (view == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));
  return HostOutcome::ok_with(pack_point(Point{view->x, view->y}));
}

template <bool kLocked>
HostOutcome fn_set_view_lock(CallContext& ctx) {
  if (ViewState* view = view_of(ctx); view != nullptr) view->locked = kLocked;
  return HostOutcome::ok_void();
}

/// `SetFog(bool)` -- 20 sites, and it is **display, not the explored map**.
///
/// 0x006a51e0 allocates a 16-byte command object, writes `arg != 0` into it at
/// `+0xc` and posts it through 0x0051d3c0. Nothing on that path is the
/// per-player exploration state `IsExplored` reads; lifting the fog for a
/// cutscene does not explore anything, and putting it back does not un-explore
/// anything. See `ViewState::fog`.
///
/// `ToggleFog/0` is registered in `gbr.exe` and is **not** bound: no script in
/// the installation writes it, and there is no fog getter for it to be the
/// setter of. `ShowNotes`'s precedent.
HostOutcome fn_set_fog(CallContext& ctx) {
  if (ViewState* view = view_of(ctx); view != nullptr) {
    view->fog = ctx.count() >= 1 && ctx.arg(0).truthy_scalar();
  }
  return HostOutcome::ok_void();
}

/// `ShowZoomMap()` and `HideZoomMap()` -- one instruction each past the
/// indirection, `vtbl + 0x10` and `vtbl + 0x14` on the panel at `[0x009d2cc0]`.
///
/// `HideZoomMap` has 8 shipped sites and every one of them opens a cutscene,
/// beside `BlockUserInput()` and `ViewPos()`. `ShowZoomMap` has none and is
/// bound anyway: it is the registered setter whose state
/// `_ZoomMapLastShownTime` reads, which is `Unit::user`'s test.
///
/// `ToggleZoomMap/0` is a third form of the same write with no call site and no
/// getter that needs it, so it stays unbound.
template <bool kOpen>
HostOutcome fn_zoom_map(CallContext& ctx) {
  ViewState* view = view_of(ctx);
  if (view == nullptr) return HostOutcome::ok_void();
  view->zoom_map_open = kOpen;
  // Only *opening* stamps. `HideZoomMap` leaves the stamp where it was, which
  // is what makes "has this player ever opened it" answerable at all.
  if (kOpen) view->zoom_map_shown_at = ctx.now;
  return HostOutcome::ok_void();
}

/// `_ZoomMapLastShownTime()` -- 1 site, and **-1 means never**.
///
/// `DATA\TUTORIALS\GENERALADVICE3.VS` sleeps 140 seconds and then asks
/// `if (_ZoomMapLastShownTime() == -1)` before offering the "Minimap" hint.
/// A session with no interface never opens the panel and answers -1 for the
/// whole match, which is the branch that file is written for.
HostOutcome fn_zoom_map_last_shown(CallContext& ctx) {
  const ViewState* view = view_of(ctx);
  const std::int64_t when = view == nullptr ? -1 : view->zoom_map_shown_at;
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(when)));
}

/// `UserNotification(key, detail, where, player)` -- 14 sites, and a correct
/// no-op rather than a stub.
///
/// **The gate is what makes it correct.** After popping its four arguments,
/// `0x0055dc00` reads the local player's record out of the globals block and
/// compares its number against `player - 1`; **if they differ the body does
/// nothing at all**. Everything past that comparison builds a notification
/// panel out of the two strings and the point. So this is a per-screen effect
/// that a peer running the same script for somebody else's player skips
/// entirely -- it cannot reach the simulation, and a headless core is the
/// permanent "they differ" case.
///
/// The fourth argument is a **1-based** player id, as the comparison against
/// `player - 1` says and as the call sites confirm: `HERO_CAPTURE.VS` passes
/// `b.player`.
///
/// The first string is a **key**, not a message -- `"location message"`,
/// `"building attacked"`, `"unit attacked"`, `"cannot train"`, `"inn spotted"`,
/// `"item cannot use in holder"`, `"friendly target only"` -- and the second is
/// a detail line, often empty. Six of the fourteen sites are `SQUADMONITOR.VS`
/// using it as the AI author's debug channel, several of them commented out.
///
/// Nothing is stored. There is no registered reader, no shipped script asks
/// what was last notified, and a queue nobody drains is state with no consumer:
/// the test `ObjectState::user` passes and this fails.
HostOutcome fn_user_notification(CallContext& ctx) {
  (void)ctx;
  return HostOutcome::ok_void();
}

/// `cls()` -- 3 sites, and the same shape.
///
/// `0x006a3fe0` fetches the HUD's text-line widget group out of the globals
/// block and sets each of its entries to the empty string: it clears the
/// on-screen message lines. `Tutorial.BFHP:seq0.vs` runs `LockView();
/// BlockUserInput(); cls();` as its opening three statements, and
/// `5_Great_Battles_Britain:seq13.vs` calls it immediately before
/// `ShowAnnouncement`.
///
/// A core with no HUD has nothing to clear, and unlike `BlockUserInput` beside
/// it there is no flag a script can read back -- `gbr.exe` registers no getter
/// and no shipped script asks. So this stores nothing, for the same reason.
HostOutcome fn_cls(CallContext& ctx) {
  (void)ctx;
  return HostOutcome::ok_void();
}

template <bool kBlocked>
HostOutcome fn_set_input_block(CallContext& ctx) {
  if (ViewState* view = view_of(ctx); view != nullptr) view->input_blocked = kBlocked;
  return HostOutcome::ok_void();
}

/// `SetShortcutSel(player, num, list)` -- 24 sites.
///
/// The list is **copied**. An `ObjList` belongs to the script that declared it
/// and is released when that script ends; a control group outlives the sequence
/// that set it, and keeping the handle would leave the group pointing at a
/// released pool entry.
HostOutcome fn_set_shortcut_sel(CallContext& ctx) {
  World* world = world_of(ctx);
  ShortcutTable* shortcuts = shortcuts_of(ctx);
  if (world == nullptr || shortcuts == nullptr || ctx.count() < 3) {
    return HostOutcome::ok_void();
  }
  const PlayerId player = ctx.arg(0).is_integer()
                              ? player_from_script(ctx.arg(0).as_integer())
                              : kNoPlayer;
  const std::int32_t slot = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : -1;
  shortcuts->assign(player, slot, objlist_pool_of(*world).items(objlist_of(ctx.arg(2))));
  return HostOutcome::ok_void();
}

/// `GetShortcutSel(player, num)` -- **zero call sites**, and bound anyway.
///
/// It is the registered getter that makes `SetShortcutSel`'s table worth
/// storing rather than dropping -- `Unit::user`'s test exactly -- so binding it
/// is what makes the claim assertable instead of merely stated. The result is a
/// fresh pooled list, because the caller owns whatever it is handed.
HostOutcome fn_get_shortcut_sel(CallContext& ctx) {
  World* world = world_of(ctx);
  const ShortcutTable* shortcuts = shortcuts_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetShortcutSel: no world");
  const ObjListId list = objlist_pool_of(*world).acquire_temporary(ctx.script);
  if (shortcuts != nullptr && ctx.count() >= 2) {
    const PlayerId player = ctx.arg(0).is_integer()
                              ? player_from_script(ctx.arg(0).as_integer())
                              : kNoPlayer;
    const std::int32_t slot = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : -1;
    if (std::vector<ObjectId>* items = objlist_pool_of(*world).mutable_items(list)) {
      const std::span<const ObjectId> group = shortcuts->group(player, slot);
      items->assign(group.begin(), group.end());
    }
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `PlayMovie(path)` -- 24 sites, and **the sole blocker of 22 scripts**.
///
/// Registered `bool` and discarded at every site, so nothing in the corpus
/// separates one answer from another. `false` is what a movie nothing played
/// would say.
HostOutcome fn_play_movie(CallContext& ctx) {
  (void)ctx;
  return HostOutcome::ok_with(Value::boolean(false));
}

/// `ShowHint(key, text, obj)` and `ShowTutorial(key, text, obj)` -- 33 sites,
/// all of them in `DATA\TUTORIALS`, all of them discarding the result.
///
/// Registered `int`. Zero is what a hint nothing showed would answer; see the
/// header's "what is still unknown" for what the number probably is.
HostOutcome fn_show_hint(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ShowHint: no world");
  // The third argument is the object the bubble points at. Resolved and
  // dropped, because the original resolves it.
  if (ctx.count() >= 3 && ctx.arg(2).is_object()) {
    (void)world->find(ctx.arg(2).as_object().id);
  }
  return HostOutcome::ok_with(Value::integer(0));
}

/// `ShowAnnouncement(key, value)` and `HideAnnouncement(key)` -- 17 sites, the
/// on-screen ticker. Write-only in every sense: no getter is registered and no
/// script keeps anything.
HostOutcome fn_announcement(CallContext& ctx) {
  (void)ctx;
  return HostOutcome::ok_void();
}

/// `IsViewLocked()` -- zero sites, and bound for `GetShortcutSel`'s reason:
/// a registered getter is what makes a registered setter's state worth keeping.
HostOutcome fn_is_view_locked(CallContext& ctx) {
  const ViewState* view = view_of(ctx);
  return HostOutcome::ok_with(Value::boolean(view != nullptr && view->locked));
}

/// `PlaySound(sound)`, `PlaySound(player, sound)` and `Obj::PlaySound(sound)`
/// -- 9 sites. The receiver of the member form is resolved and dropped.
HostOutcome fn_play_sound(CallContext& ctx) {
  (void)ctx;
  return HostOutcome::ok_void();
}

HostOutcome m_play_sound(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("PlaySound: no world");
  if (ctx.count() >= 1 && ctx.arg(0).is_object()) {
    (void)world->find(ctx.arg(0).as_object().id);
  }
  return HostOutcome::ok_void();
}

constexpr std::size_t kEntryCount = 28;

}  // namespace

// --------------------------------------------------------------------------
// the control groups
// --------------------------------------------------------------------------

void ShortcutTable::assign(PlayerId player, std::int32_t slot,
                           std::span<const ObjectId> objects) {
  if (player >= kPlayerCount) return;
  if (slot < 0 || static_cast<std::size_t>(slot) >= kSlots) return;
  std::vector<ObjectId>& group = groups_[player][static_cast<std::size_t>(slot)];
  group.assign(objects.begin(), objects.end());
}

std::span<const ObjectId> ShortcutTable::group(PlayerId player,
                                               std::int32_t slot) const noexcept {
  if (player >= kPlayerCount) return {};
  if (slot < 0 || static_cast<std::size_t>(slot) >= kSlots) return {};
  return groups_[player][static_cast<std::size_t>(slot)];
}

std::size_t ShortcutTable::forget(ObjectId object) {
  std::size_t dropped = 0;
  for (auto& player : groups_) {
    for (auto& group : player) {
      const std::size_t before = group.size();
      group.erase(std::remove(group.begin(), group.end(), object), group.end());
      dropped += before - group.size();
    }
  }
  return dropped;
}

void ShortcutTable::clear() noexcept {
  for (auto& player : groups_) {
    for (auto& group : player) group.clear();
  }
}

std::string compose_rollover(const CommandDef* row, std::string_view message, bool show_cost) {
  std::string out;
  if (row != nullptr) {
    append_line(out, row->rollover);
    append_line(out, row->description);
  }
  append_line(out, message);
  if (show_cost && row != nullptr) {
    bool any = false;
    append_cost(out, kGoldIcon, row->cost_gold, any);
    append_cost(out, kFoodIcon, row->cost_food, any);
    append_cost(out, kPopIcon, row->cost_pop, any);
    append_cost(out, kStaminaIcon, row->cost_stamina, any);
  }
  return out;
}

std::size_t register_feedback_host(HostRegistry& registry) {
  std::size_t defined = 0;
  const auto free_fn = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::free_function, name, arity, fn);
    ++defined;
  };

  // Descending corpus call frequency.
  free_fn("CreateFeedback", 2, &fn_create_feedback);  // 72
  free_fn("rollover", 2, &fn_rollover_2);             // 70
  free_fn("rollover", 3, &fn_rollover_3);             // 15
  free_fn("rollover_desc", 3, &fn_rollover_desc_3);   //  1
  free_fn("rollover", 0, &fn_rollover_0);             //  0 -- see below

  // -- the camera, the chrome and the control groups, same ordering ---------
  free_fn("View", 2, &fn_view);                          // 35
  free_fn("ShowHint", 3, &fn_show_hint);                 // 26
  free_fn("SetShortcutSel", 3, &fn_set_shortcut_sel);    // 24
  free_fn("PlayMovie", 1, &fn_play_movie);               // 24
  free_fn("UnblockUserInput", 0, &fn_set_input_block<false>);  // 17
  free_fn("BlockUserInput", 0, &fn_set_input_block<true>);     // 17
  free_fn("UserNotification", 4, &fn_user_notification);      // 14
  free_fn("cls", 0, &fn_cls);                                 //  3
  free_fn("ShowAnnouncement", 2, &fn_announcement);      // 11
  free_fn("SetFog", 1, &fn_set_fog);                      // 20
  free_fn("HideZoomMap", 0, &fn_zoom_map<false>);         //  8
  free_fn("ViewPos", 0, &fn_view_pos);                   //  7
  free_fn("ShowTutorial", 3, &fn_show_hint);             //  7, the same body
  free_fn("PlaySound", 1, &fn_play_sound);               //  7
  free_fn("HideAnnouncement", 1, &fn_announcement);      //  6
  free_fn("UnlockView", 0, &fn_set_view_lock<false>);    //  2
  free_fn("LockView", 0, &fn_set_view_lock<true>);       //  2
  free_fn("PlaySound", 2, &fn_play_sound);               //  1
  registry.define(CallKind::member, "PlaySound", 1, &m_play_sound);  // 1
  ++defined;
  // Zero sites, and bound because it is the registered getter that makes
  // `SetShortcutSel`'s table worth storing -- `Unit::user`'s test.
  free_fn("GetShortcutSel", 2, &fn_get_shortcut_sel);    //  0
  // Likewise: the getter that makes the lock worth storing.
  free_fn("IsViewLocked", 0, &fn_is_view_locked);        //  0
  // And the pair the zoom map needs: one shipped reader, and the setter it
  // reads. `ToggleZoomMap/0`, `ToggleFog/0`, `BlockMiniMap/1`, `MiniMap/0`,
  // `SetMiniMapRect/1` and `BuildMiniMap/1` are all registered in `gbr.exe`
  // with **zero** sites and no getter that needs them, and stay unbound.
  free_fn("_ZoomMapLastShownTime", 0, &fn_zoom_map_last_shown);  //  1
  free_fn("ShowZoomMap", 0, &fn_zoom_map<true>);         //  0

  // `rollover/0` has **no shipped call site**, and is bound anyway. That is a
  // deliberate exception to the rule the rest of this tree follows for a
  // zero-site entry point, and the reason is that it is the form that *proves
  // the reading*: a rollover that takes no object at all cannot be about an
  // object, which is what says the family answers about the described command.
  // Binding it makes that assertable rather than merely stated.
  //
  // The other five registered forms are not bound, and have no sites either:
  //
  //     CreateFeedback(str, Unit, int)      0x005187c0
  //     CreateFeedback(str, point)          0x005188a0
  //     CreateFeedback(str, point, int)     0x005189d0
  //
  // The point forms would reach the same body as the unit ones if they were
  // called, because the registry keys on arity; the three-argument forms would
  // need a fourth body and there is nothing to write it against.
  return defined;
}

std::size_t feedback_host_entry_count() noexcept { return kEntryCount; }

}  // namespace imperivm::core::sim
