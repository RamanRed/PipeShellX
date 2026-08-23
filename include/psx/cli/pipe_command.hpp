#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace psx::cli {

// `pipeshellx pipe "<spec>"`: parse a pipe spec, validate it as a DAG, and run
// it. Local pipelines (no @placement) execute now; a remote placement is
// reported as not-yet-supported. Returns the pipeline's exit code (pipefail),
// 2 on a usage/validation error, or 130 if interrupted.
int pipeSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
