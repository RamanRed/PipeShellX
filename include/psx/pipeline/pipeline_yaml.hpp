#pragma once

#include "psx/pipeline/pipeline.hpp"
#include "psx/result.hpp"

#include <string_view>

namespace psx::pipeline {

// Parses a restricted pipeline YAML document:
//
//   stages:
//     - id: a
//       run: "grep foo"
//       at: local
//     - id: b
//       run: [sort, -u]
//   edges:
//     - from: a
//       to: b
//
// `stages` and `edges` are block lists. A stage needs `id` and `run`; `at` is
// optional and defaults to local. A quoted `run` is split on whitespace, while
// a YAML list supplies argv elements directly. The resulting Pipeline is
// validated with Planner. InvalidArgument is returned for unsupported YAML,
// malformed documents, or invalid pipeline structure.
psx::Result<Pipeline> loadPipelineYaml(std::string_view yamlText);

} // namespace psx::pipeline
