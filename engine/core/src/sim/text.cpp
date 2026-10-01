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

HostOutcome clear_debug_impl(CallContext& ctx) {
  HostContext* context = host_context_of(ctx);
  if (context == nullptr) return HostOutcome::failed(kNoContext);
  if (context->debug != nullptr) context->debug->clear();
  return HostOutcome::ok_void();
}

/// The table, or null. A null table translates everything to itself.
[[nodiscard]] const game::TranslationTable* table_of(CallContext& ctx) {
  HostContext* context = host_context_of(ctx);
  return context == nullptr ? nullptr : context->translations;
}

HostOutcome translate_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  const std::string key = text_of(ctx.arg(0));
  const game::TranslationTable* table = table_of(ctx);
  if (table == nullptr) return HostOutcome::ok_with(Value::string(key));
  return HostOutcome::ok_with(Value::string(std::string(table->translate(key))));
}

HostOutcome translatef_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  const std::string key = text_of(ctx.arg(0));
  const game::TranslationTable* table = table_of(ctx);
  const std::string pattern = table == nullptr ? key : std::string(table->translate(key));

  // Arguments after the pattern fill `%s1`, `%s2`, ... in order. The corpus
  // uses at most three.
  std::vector<std::string> arguments;
  arguments.reserve(ctx.count() > 1 ? ctx.count() - 1 : 0);
  for (std::size_t i = 1; i < ctx.count(); ++i) arguments.push_back(text_of(ctx.arg(i)));

  return HostOutcome::ok_with(Value::string(game::substitute(pattern, arguments)));
}

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

HostOutcome str2int_impl(CallContext& ctx) {
  if (host_context_of(ctx) == nullptr) return HostOutcome::failed(kNoContext);
  const std::string text = text_of(ctx.arg(0));

  // Leading sign, then digits, stopping at the first character that is not one.
  //
  // **Inferred, and deliberately different from `formats/ini.hpp`'s
  // `parse_int`, which refuses a partial parse.** The two are answering
  // different questions: a configuration value read half-way is a silent data
  // corruption (`ProductionInterval` was recorded as 20 when it is 2000),
  // whereas `Str2Int` is a script-level best-effort conversion whose failure
  // mode the corpus never exercises -- every shipped call feeds it either a
  // `ParseStr` token or an `EnvReadString` result. What the original returns
  // for `Str2Int("12abc")` is unknown.
  std::size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  std::int64_t value = 0;
  bool any = false;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    any = true;
    value = value * 10 + (text[i] - '0');
    // Saturate rather than wrap. A wrapped value is a number the simulation
    // would go on to use.
    if (value > 2147483647LL) {
      value = 2147483647LL;
      break;
    }
  }
  if (!any) value = 0;
  const std::int32_t out =
      negative ? static_cast<std::int32_t>(-value) : static_cast<std::int32_t>(value);
  return HostOutcome::ok_with(Value::integer(out));
}

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

/// `Breakpoint()` -- 62 sites, all in `UNIT_ATTACH.VS`. 0x00746ea0 is `xor
/// eax, eax; ret`: a hook a debugger build would have patched, and in the
/// shipped one a no-op.
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
