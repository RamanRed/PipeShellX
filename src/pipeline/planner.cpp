#include "psx/pipeline/planner.hpp"

#include <cstddef>
#include <deque>
#include <set>
#include <unordered_map>
#include <utility>

namespace psx::pipeline {

namespace {
psx::Error invalid(const char* what) {
    return psx::Error{psx::ErrorClass::InvalidArgument, 0, what};
}
} // namespace

psx::Result<Planner::Plan> Planner::plan(const Pipeline& pipeline) {
    if (pipeline.stages.empty()) {
        return invalid("pipeline has no stages");
    }

    // Index stages by id (position), enforcing unique non-empty ids + commands.
    std::unordered_map<std::string, std::size_t> position;
    position.reserve(pipeline.stages.size());
    for (const Stage& stage : pipeline.stages) {
        if (stage.id.empty()) {
            return invalid("stage has an empty id");
        }
        if (stage.argv.empty()) {
            return invalid("stage has an empty command");
        }
        if (!position.emplace(stage.id, position.size()).second) {
            return invalid("duplicate stage id");
        }
    }

    // Build adjacency + in-degree from the edges (deduped), by position so the
    // topological order is deterministic in stage-definition order.
    const std::size_t n = pipeline.stages.size();
    std::vector<std::vector<std::size_t>> successors(n);
    std::vector<int> indegree(n, 0);
    std::set<std::pair<std::size_t, std::size_t>> seen;
    for (const Edge& edge : pipeline.edges) {
        const auto from = position.find(edge.from);
        const auto to = position.find(edge.to);
        if (from == position.end() || to == position.end()) {
            return invalid("edge references an unknown stage");
        }
        if (from->second == to->second) {
            return invalid("edge is a self-loop");
        }
        if (!seen.emplace(from->second, to->second).second) {
            continue; // duplicate edge: ignore
        }
        successors[from->second].push_back(to->second);
        ++indegree[to->second];
    }

    // Kahn's algorithm, seeding sources in definition order for a stable result.
    std::deque<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i);
        }
    }
    Plan plan;
    plan.order.reserve(n);
    while (!ready.empty()) {
        const std::size_t current = ready.front();
        ready.pop_front();
        plan.order.push_back(pipeline.stages[current].id);
        for (const std::size_t next : successors[current]) {
            if (--indegree[next] == 0) {
                ready.push_back(next);
            }
        }
    }
    if (plan.order.size() != n) {
        return invalid("pipeline has a cycle");
    }
    return plan;
}

} // namespace psx::pipeline
