#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace psx::cli {

// `pipeshellx diff -i FILE -g GROUP --cert F --key F --ca F -- <command>`: run
// the command on every selected host over the native backplane, group the hosts
// by identical output, and print the consensus (majority) and any outliers
// (configuration drift). Exit 0 when all agree, 1 on drift, 2 on a usage error
// or a host that failed to run.
int diffSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
