#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace psx::cli {

// `pipeshellx node [run] --cert F --key F --ca F --listen HOST:PORT [--allow SAN[,SAN...]]`
// Runs the psx/1 node daemon (a NodeServer) until SIGINT/SIGTERM. Returns a
// process exit code (0 clean shutdown, 2 usage/error). Native-transport builds
// only; main reports the missing support otherwise.
int nodeSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
