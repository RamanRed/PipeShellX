#pragma once

#include "psx/pipeline/pipeline.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace psx::pipeline {

// Executes an all-local pipeline DAG without turning it into a declaration-
// order chain. Every declared edge owns a bounded buffer: when any successor
// is full, reading from that producer pauses and normal pipe backpressure
// reaches the child. Fan-out duplicates bytes to every successor; fan-in
// fairly merges ready predecessor streams into the consumer's stdin.
class DagRunner {
public:
    static constexpr std::size_t kDefaultEdgeCapacity = 256U * 1024U;

    struct Outcome {
        // pipefail in deterministic Planner topological order.
        int exitCode = 0;
        std::vector<int> stageExitCodes;
        std::vector<std::string> topologicalOrder;
    };
    using OnOutput = std::function<void(std::string_view)>;

    explicit DagRunner(psx::runtime::Reactor& reactor,
                       OnOutput onOutput = {},
                       std::size_t edgeCapacity = kDefaultEdgeCapacity);
    ~DagRunner();
    DagRunner(const DagRunner&) = delete;
    DagRunner& operator=(const DagRunner&) = delete;

    // One run per instance. The pipeline must contain only local placements.
    // onComplete runs after every child is terminal/reaped and every stdout
    // stream has reached EOF.
    psx::Result<void> run(const Pipeline& pipeline, std::function<void(Outcome)> onComplete);

    // Synchronously aborts an in-flight run: all watches and parent pipe ends
    // are removed, every child process group is killed/reaped, and buffered
    // edge data is discarded. The completion callback is not invoked.
    void cancel() noexcept;

    std::size_t activeChildCount() const noexcept;
    std::size_t bufferedBytes() const noexcept;
    std::size_t peakBufferedBytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace psx::pipeline
