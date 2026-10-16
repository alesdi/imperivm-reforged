// The string and localisation slice of the host surface.
// See include/imperivm/core/sim/text.hpp.

#include "imperivm/core/sim/text.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_context.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::Value;

constexpr const char* kNoContext = "host called with no HostContext";

/// A script value as text.
///
/// `Translatef("...", nTimeout/60, ...)` passes integers into slots the table
/// spells `%s`, so every argument has to be printable regardless of its kind.
/// An object is not: `Host::to_string` owns that, and it is not reachable from
/// a plain host function without the host pointer, so an object argument comes
/// through this as empty rather than as a made-up rendering.
[[nodiscard]] std::string text_of(const Value& value) {
  if (value.is_string()) return value.as_string();
  if (value.is_integer()) return std::to_string(value.as_integer());
  return std::string();
}

/// `pr(x)` -- 157 sites with `Obj::pr`, nearly all commented out.
///
/// Three bodies, chosen by the argument's static type: 0x006a3f00 prints a
/// string with `%s`, 0x006a3f70 an integer with `%d`, 0x006a3fa0 a bool as
/// `true` or `false`; each writes one line to the in-game debug console
/// (0x00619060 on the console at `[0x00996ff4]+0x44cc`) and answers nothing.
/// No check, no state. The bool form is not told apart here: a VS bool is an
/// integer by the time it reaches a host function, so `pr(true)` prints `1`.
/// Debug output only; nothing reads it back.
HostOutcome pr_impl(CallContext& ctx) {
  HostContext* context = host_context_of(ctx);
  if (context == nullptr) return HostOutcome::failed(kNoContext);
  // No sink is not an error. A conformance run has none on purpose: debug
  // output is the one thing in the engine that may legitimately go nowhere.
  if (context->debug != nullptr && ctx.count() >= 1) {
    context->debug->write(text_of(ctx.arg(0)));
  }
  return HostOutcome::ok_void();
}

/// `ClearDebug()` -- one live site, `DEBUG_SELECTION.VS`, which calls it,
/// marks the selection with `SetDebug(true)` and then calls `cls()`.
///
/// 0x005a7fd0 adds one to the debug era at 0x00821354 and does nothing else.
/// An object is "debugged" when its own era (`[obj+0x74]`) equals that
/// counter (`Obj::GetDebug`, 0x005abd00), so moving the counter un-debugs
/// every object at once. **It does not clear the console**: that is `cls`
/// (0x006a3fe0, `sim/feedback.cpp`). The per-object flag is not modelled
/// (`SetDebug` is a presentation no-op in `world_host.cpp`), so there is
/// nothing here to move.
HostOutcome clear_debug_impl(CallContext& ctx) {
  HostContext* context = host_context_of(ctx);
  if (context == nullptr) return HostOutcome::failed(kNoContext);
  return HostOutcome::ok_void();
}

/// The table, or null. A null table translates everything to itself.
[[nodiscard]] const game::TranslationTable* table_of(CallContext& ctx) {
  HostContext* context = host_context_of(ctx);
  return context == nullptr ? nullptr : context->translations;
}

/// `Translate(key)` -- 227 sites.
///
/// 0x004c9b70 asks the language pack's table (`[0x00a77e94]`) for `key` with
/// the running script's file name as context, and, when that answer is `key`
/// itself, asks the map's own table (`[0x00a77e90]`) the same question
/// (0x004c9d1a). The rule of one question is `TranslationTable::lookup`'s.
/// The script-file context is not passed: no shipped key carries one. No
/// table translates everything to itself here, which a conformance run relies
/// on; the original always has one.
HostOutcome translate_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  const std::string key = text_of(ctx.arg(0));
  const game::TranslationTable* table = table_of(ctx);
  if (table == nullptr) return HostOutcome::ok_with(Value::string(key));
  return HostOutcome::ok_with(Value::string(table->lookup(key)));
}

/// `Translatef(key, a1[, a2[, a3]])` -- 85 sites; three bodies, one per
/// arity, all of whose arguments are strings (0x004c9ea0, 0x004ca150,
/// 0x004ca470).
///
/// Each asks the language pack for `key` with no context; when the answer is
/// `key` itself and the map has a table, that table formats instead,
/// otherwise the language pack does. The formatting is
/// `TranslationTable::format`'s: arguments under `%s` are translated too.
/// With no language table at all the original answers the empty string (its
/// 4 KB buffer starts empty and nothing writes it); here no table means a
/// conformance run, and the key is substituted untranslated.
HostOutcome translatef_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  const std::string key = text_of(ctx.arg(0));
  const game::TranslationTable* table = table_of(ctx);

  std::vector<std::string> arguments;
  arguments.reserve(ctx.count() > 1 ? ctx.count() - 1 : 0);
  for (std::size_t i = 1; i < ctx.count(); ++i) arguments.push_back(text_of(ctx.arg(i)));

  if (table == nullptr) {
    return HostOutcome::ok_with(Value::string(game::substitute(key, arguments)));
  }
  return HostOutcome::ok_with(Value::string(table->format(key, arguments)));
}

/// `ParseStr(src, tail)` -- 71 sites. 0x00697580; the rule is
/// `game::split_token`'s. The token is the answer and the tail is written to
/// the second argument, a string reference.
HostOutcome parse_str_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  if (ctx.count() < 2) return HostOutcome::failed("ParseStr needs a source and a tail");

  std::string token;
  std::string tail;
  // Copied, not viewed. Every one of the 71 call sites is `ParseStr(dest,
  // dest)`, so argument 0 and argument 1 are the same slot and writing the tail
  // would otherwise cut the source out from under the read.
  const std::string source = text_of(ctx.arg(0));
  game::split_token(source, token, tail);

  // Argument 1 is an out-parameter; the VM copies assignable arguments back
  // after the call. See `script/host.hpp`.
  ctx.out(1) = Value::string(std::move(tail));
  return HostOutcome::ok_with(Value::string(std::move(token)));
}

/// `Str2Int(text)` -- 24 sites.
///
/// 0x00696a40 is the C runtime's `atol` (0x00763919) on the string's bytes:
/// skip leading white space (the `isspace` class: space and 0x09..0x0D),
/// take one optional `+` or `-`, then decimal digits up to the first byte
/// that is not one, accumulating `10 * total + digit` in a 32-bit register
/// that **wraps** rather than saturating, and negate for `-`. No digits
/// answers 0, and so does an empty string; the rest of the text is ignored,
/// so `Str2Int("12abc")` is 12.
HostOutcome str2int_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  const std::string text = text_of(ctx.arg(0));

  std::size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r'))) ++i;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  std::uint32_t total = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    total = total * 10u + static_cast<std::uint32_t>(text[i] - '0');
  }
  if (negative) total = 0u - total;
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(total)));
}

/// `StrLen(text)` -- 3 sites. 0x00696b40 counts the bytes before the first
/// NUL (0x00649b40); a null string object answers 0.
HostOutcome strlen_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  // Bytes. The data is Windows-1252, one byte per character, so this is also
  // the character count for every string the game ships.
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(text_of(ctx.arg(0)).size())));
}

/// `StrMid(str, start, len)` -- 40 sites in one script.
///
/// 0x00697740: `len` is capped at `strlen - start` and floored at zero, then
/// `len` bytes from `start` are copied. Nothing checks `start` itself: past
/// the end the cap goes negative and the floor makes the answer empty, which
/// is reproduced; before the beginning the original reads memory in front of
/// the string, which is not a value and is answered as `start == 0` here.
HostOutcome strmid_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  if (ctx.count() < 3 || !ctx.arg(1).is_integer() || !ctx.arg(2).is_integer()) {
    return HostOutcome::failed("StrMid needs a string, a start and a length");
  }
  const std::string text = text_of(ctx.arg(0));
  const std::int64_t size = static_cast<std::int64_t>(text.size());
  std::int64_t start = ctx.arg(1).as_integer();
  std::int64_t length = ctx.arg(2).as_integer();
  if (start < 0) start = 0;
  if (length > size - start) length = size - start;
  // A start past the end leaves a negative cap, and the floor makes the answer
  // empty. The second clamp is redundant with that and kept on purpose:
  // `substr` throws on a position past the end rather than clamping it, and a
  // fault in the cap above took the whole test process down instead of
  // failing one check. That happened.
  if (start > size) start = size;
  if (length <= 0) return HostOutcome::ok_with(Value::string(std::string()));
  return HostOutcome::ok_with(Value::string(
      text.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(length))));
}

/// `Breakpoint()` -- 62 sites, all in `UNIT_ATTACH.VS`. 0x00746ea0 answers 0
/// and touches nothing: a hook a debugger build would have patched, and in the
/// shipped one a no-op. The same body is registered for `_itools`,
/// `PerlinTest`, `GenTransTables` and `IngameDebugSelected` (no live site),
/// and for `GetGAIKA(int)` and `GAIKA::ID`, where leaving the VM stack alone
/// makes the argument the answer -- those two are `sim/ai.cpp`'s and
/// `sim/squad.cpp`'s identity functions.
HostOutcome breakpoint_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  return HostOutcome::ok_void();
}

}  // namespace

std::size_t register_text_host(script::HostRegistry& registry) {
  const CallKind free_fn = CallKind::free_function;
  const std::size_t before = registry.implemented();

  registry.define(free_fn, "pr", 1, &pr_impl);
  registry.define(free_fn, "ClearDebug", 0, &clear_debug_impl);
  registry.define(free_fn, "Translate", 1, &translate_impl);
  registry.define(free_fn, "Translatef", 2, &translatef_impl);
  registry.define(free_fn, "Translatef", 3, &translatef_impl);
  registry.define(free_fn, "Translatef", 4, &translatef_impl);
  registry.define(free_fn, "ParseStr", 2, &parse_str_impl);
  registry.define(free_fn, "Str2Int", 1, &str2int_impl);
  registry.define(free_fn, "StrLen", 1, &strlen_impl);
  registry.define(free_fn, "StrMid", 3, &strmid_impl);
  registry.define(free_fn, "Breakpoint", 0, &breakpoint_impl);

  // `SS_STR` is deliberately absent. It maps a squad-state number back to its
  // name, and the names live in the AI profile (`sim/ai_profile.hpp`), which no
  // `HostContext` carries yet. Returning a plausible string would be exactly
  // the silent divergence the unimplemented-entry trap exists to prevent.

  return registry.implemented() - before;
}

}  // namespace imperivm::core::sim
