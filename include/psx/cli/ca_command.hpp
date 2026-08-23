#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace psx::cli {

// `pipeshellx ca <init|issue> ...` — the offline fleet CA CLI. Returns a process
// exit code (0 ok, 2 usage/error). Only available when built with native
// transport support (OpenSSL); main reports that when it is not.
//
//   ca init  --cn NAME --dir DIR            write DIR/ca.key (0600) + DIR/ca.crt
//   ca issue --san URI  --ca DIR --out PFX  write PFX.key (0600) + PFX.crt
int caSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
