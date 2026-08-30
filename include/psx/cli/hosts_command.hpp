#pragma once

// `pipeshellx hosts [-i FILE]` — list the inventory's hosts, their groups and
// tags. Reads the INI at `inventoryPath` (or a clients.txt when empty).

#include <ostream>
#include <string>
#include <vector>

namespace psx::cli {

int hostsSubcommand(const std::string& inventoryPath, std::ostream& out, std::ostream& err);

// Parses the arguments following `pipeshellx hosts`. Bare `hosts` remains an
// alias for `hosts list`; mutations require an explicit `-i FILE` target.
int hostsSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
