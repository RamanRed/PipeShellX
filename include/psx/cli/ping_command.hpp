#pragma once

// `pipeshellx ping [-i FILE] [-g GROUP|-t TAG|-H h1,h2] [--timeout S]` — probe
// each selected host's SSH reachability and report ONLINE/OFFLINE.

#include "psx/cli/run_command.hpp" // Selector, CliError

#include <ostream>
#include <string>
#include <vector>

namespace psx::cli {

struct PingInvocation {
    std::string inventoryPath;
    Selector selector;
    int timeoutSec = 10;
};

PingInvocation parsePing(const std::vector<std::string>& args);

// Runs `echo connected` on each selected host. Exit codes: 0 all ONLINE,
// 1 some OFFLINE, 2 config, 3 no hosts.
int pingSubcommand(const PingInvocation& invocation, std::ostream& out, std::ostream& err);

} // namespace psx::cli
