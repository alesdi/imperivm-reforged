#pragma once

// Assembling the host surface with one domain left out.
//
// A domain's collision check has to run against **the others**, never against
// `register_all_hosts`: once the domain is in the manifest, re-registering it
// adds nothing, and the test then fails for a reason that has nothing to do
// with a collision. Three suites were written the other way -- squad, ai and
// match -- and all three broke the hour their domain was wired in, each time
// looking like a real overlap until somebody read it.
//
// `HostRegistry::define` replaces silently, so these checks are the only thing
// standing between a domain quietly taking an entry point from another and
// every existing test still passing.

#include <string_view>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_setup.hpp"

namespace imperivm::test {

/// Declare the shipped surface and run every domain except `skip`, in manifest
/// order -- which is the order that decides who wins a collision.
inline void define_all_except(std::string_view skip,
                              imperivm::core::script::HostRegistry& registry) {
  imperivm::core::script::declare_shipped_surface(registry);
  for (const imperivm::core::sim::HostDomain& domain :
       imperivm::core::sim::host_domains()) {
    if (domain.name == skip) continue;
    domain.define(registry);
  }
}

}  // namespace imperivm::test
