#pragma once

// Shared host resolution for the run/ping subcommands: load the inventory,
// apply a selector, and produce the SSH descriptors (with the per-inventory
// known_hosts attached). Kept in one place so both subcommands agree on the
// lookup rules and exit codes.

#include "client_config.hpp"
#include "psx/cli/run_command.hpp"
#include "psx/inventory/inventory.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace psx::cli {

struct ResolvedHosts {
    std::vector<ClientEntry> clients;
    std::string inventoryPath; // the path actually used
    int exitCode = 0;          // 0 ok, 2 config error, 3 no hosts selected
    bool ok() const noexcept { return exitCode == 0; }
};

// Loads the INI at `inventoryPath`, or a clients.txt in the working directory
// when it is empty; selects hosts; converts them to ClientEntry. Writes a
// diagnostic to `err` and sets exitCode on failure.
ResolvedHosts resolveHosts(const std::string& inventoryPath, const Selector& selector, std::ostream& err);

} // namespace psx::cli
