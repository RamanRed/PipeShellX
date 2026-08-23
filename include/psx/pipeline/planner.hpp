#pragma once

#include "psx/pipeline/pipeline.hpp"
#include "psx/result.hpp"

#include <string>
#include <vector>

namespace psx::pipeline {

// Validates a pipeline and computes a runnable execution order. A valid pipeline
// is a DAG: at least one stage, each with a unique non-empty id and a non-empty
// command; every edge references existing stages; no self-loops; no cycles.
// Duplicate edges (same from/to) are ignored.
class Planner {
public:
    struct Plan {
        // Stage ids in a topological order: every producer precedes its
        // consumers. Deterministic — ties break by stage definition order.
        std::vector<std::string> order;
    };

    // psx::ErrorClass::InvalidArgument on any structural violation; the message
    // names the specific problem (empty pipeline, duplicate/empty id, empty
    // command, dangling edge, self-loop, or cycle).
    static psx::Result<Plan> plan(const Pipeline& pipeline);
};

} // namespace psx::pipeline
