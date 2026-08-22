#pragma once

// `pipeshellx hosts [-i FILE]` — list the inventory's hosts, their groups and
// tags. Reads the INI at `inventoryPath` (or a clients.txt when empty).

#include <ostream>
#include <string>

namespace psx::cli {

int hostsSubcommand(const std::string& inventoryPath, std::ostream& out, std::ostream& err);

} // namespace psx::cli
