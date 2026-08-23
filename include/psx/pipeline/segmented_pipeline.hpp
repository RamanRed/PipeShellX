#pragma once

#include "psx/os/tls.hpp"
#include "psx/pipeline/distributed_runner.hpp"
#include "psx/pipeline/local_runner.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace psx::pipeline {

// A pipeline stage with its placement resolved: local when `host` is empty,
// otherwise remote at host:port with `expectedSan` pinned.
struct ResolvedStage {
    std::vector<std::string> argv;
    std::string host; // empty = local
    std::uint16_t port = 0;
    std::string expectedSan;

    bool isLocal() const { return host.empty(); }
};

// Runs a linear pipeline whose stages mix local and remote placements. It splits
// the chain into maximal same-locality segments, runs each on its own runner
// (LocalRunner / DistributedRunner), and splices them: each segment's stdout
// feeds the next segment's stdin, and a segment finishing closes the next
// segment's stdin. The final segment's stdout is the pipeline output; the run
// completes when the final segment completes. One run per instance.
class SegmentedPipeline {
public:
    struct Outcome {
        int exitCode = 0;
        std::vector<int> stageExitCodes; // per stage, in pipeline order
        std::string error;               // non-empty: a remote connect/handshake failure
    };
    using OnOutput = std::function<void(std::string_view)>;

    SegmentedPipeline(psx::runtime::Reactor& reactor,
                      psx::os::TlsConfig controllerConfig,
                      OnOutput onOutput = {},
                      OnOutput onStderr = {});
    ~SegmentedPipeline();
    SegmentedPipeline(const SegmentedPipeline&) = delete;
    SegmentedPipeline& operator=(const SegmentedPipeline&) = delete;

    psx::Result<void>
    run(const std::vector<ResolvedStage>& stages, std::function<void(Outcome)> onComplete, bool externalStdin = false);

    // Feed the pipeline's first stage stdin (only when run(..., externalStdin=true))
    // -- used to splice a fan-in source ahead of the pipeline.
    void writeStdin(std::string_view bytes);
    void closeStdin();

private:
    struct Segment {
        bool local = true;
        std::size_t stageCount = 0;
        std::unique_ptr<LocalRunner> localRunner;
        std::unique_ptr<DistributedRunner> remoteRunner;
        std::vector<int> exitCodes;
        bool done = false;
        void writeStdin(std::string_view bytes);
        void closeStdin();
    };

    void routeOutput(std::size_t index, std::string_view data);
    void onSegmentDone(std::size_t index, std::vector<int> exitCodes, const std::string& error);
    void finish(Outcome outcome);

    psx::runtime::Reactor& reactor_;
    psx::os::TlsConfig config_;
    OnOutput onOutput_;
    OnOutput onStderr_;
    std::function<void(Outcome)> onComplete_;
    std::vector<Segment> segments_;
    bool externalStdin_ = false;
    bool done_ = false;
};

} // namespace psx::pipeline
