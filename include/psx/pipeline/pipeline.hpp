#pragma once

#include <string>
#include <vector>

namespace psx::pipeline {

// One stage of a pipeline: a command run at a placement. `placement` names an
// inventory host or group; empty means the controller's local host.
struct Stage {
    std::string id;                // unique within the pipeline
    std::vector<std::string> argv; // the command and its arguments
    std::string placement;         // where it runs (empty = local)
};

// A directed data edge: the producer stage's stdout feeds the consumer's stdin.
struct Edge {
    std::string from; // producer stage id
    std::string to;   // consumer stage id
};

// A pipeline is a directed acyclic graph of stages joined by edges. Fan-out (a
// stage feeding several) and fan-in (several feeding one) are expressed as
// multiple edges; the Planner validates the shape.
struct Pipeline {
    std::vector<Stage> stages;
    std::vector<Edge> edges;
};

} // namespace psx::pipeline
