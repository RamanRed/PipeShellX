#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace psx::cli {

// `pipeshellx pipe "<spec>"` or `pipeshellx pipe --file FILE`: parse, validate,
// and run a pipeline. Local pipelines (no @placement) execute on the
// controller; remote placements require the configured inventory and transport
// credentials. Returns the pipeline's exit code (pipefail), 2 on a
// usage/validation error, or 130 if interrupted.
int pipeSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
