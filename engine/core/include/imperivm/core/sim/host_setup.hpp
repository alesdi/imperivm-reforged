#pragma once

/// The one entry point that builds the whole `.vs` host surface.
///
/// ## Why there is exactly one
///
/// The surface is 185 free functions and 520 members over 17,131 call sites
/// (`docs/formats/vs-host-api.md`), and it is filled in by seven-odd domains
/// that were built in parallel: the scheduler's own primitives, the object
/// model, combat, economy, heroes, movement, and the collection types. Every
/// one of them exposes its own `register_*`/`define_*`, and an embedder that
/// calls six of the seven gets a registry that compiles, runs, and traps
/// somewhere in the middle of a scenario -- which is the same class of failure
/// as `CallContext::user` meaning a different type per domain
/// (`sim/host_context.hpp`): invisible to any test that exercises one domain
/// alone. So there is one function, and the list below **is** the manifest.
///
/// ## Registration order is fixed, and that is not tidiness
///
/// `HostRegistry::define` on a (kind, name, arity) that is already defined
/// *replaces* the previous function and says nothing. Two domains that both
/// claim `Unit::level/0` therefore produce a registry whose behaviour depends
/// entirely on which one ran last, and the loser's tests keep passing because
/// they build their own registry. The order here is the tie-break rule, so it
/// is data -- `host_domains()` -- rather than a sequence of calls buried in a
/// .cpp, and `engine/tests/test_host_setup.cpp` asserts that no domain's
/// entries are lost to a later one. Changing the order changes the simulation;
/// treat it the way `World::add_system` order is treated.
///
/// The order is: the runtime's own primitives first, then the object model
/// every other domain's handles come from, then the simulation domains, then
/// the collection and player/command/environment slices that sit on top.
///
/// ## What this does *not* do
///
/// It does not construct a `World`, a `HostContext` or a `script::Host`. The
/// registry is world-invariant -- `HostFn` is a plain function pointer and the
/// table is built once -- so it is built once per process and the world arrives
/// per call through `CallContext::user`.

#include <cstddef>
#include <span>
#include <string_view>

namespace imperivm::core::script {
class HostRegistry;
}

namespace imperivm::core::sim {

/// One domain's slice of the host surface, in run order.
///
/// `define` never introduces a name: `declare_shipped_surface` supplies the
/// whole inventory and each domain attaches behaviour to entries already in it.
struct HostDomain {
  /// The domain's name. Stable: the tests and any diagnostic name it.
  std::string_view name;
  /// Attach this domain's entry points to the registry.
  void (*define)(script::HostRegistry& registry);
};

/// The run-order manifest of the host surface.
///
/// Exposed rather than kept private so that a test can walk exactly the list
/// `register_all_hosts` walks. A test that restated the order would drift from
/// it, and the drift would be silent in precisely the way this file exists to
/// prevent.
[[nodiscard]] std::span<const HostDomain> host_domains() noexcept;

/// Declare the shipped surface and define every implemented entry point on it.
///
/// Calls `script::declare_shipped_surface` first -- so that an entry point no
/// domain has reached yet traps with its own name rather than reading as
/// unknown -- then each domain in `host_domains()` order.
///
/// Returns the number of entry points that are implemented afterwards and were
/// not before, so a caller can assert against the count rather than trust it.
/// On an empty registry that is the size of the implemented surface.
std::size_t register_all_hosts(script::HostRegistry& registry);

}  // namespace imperivm::core::sim
