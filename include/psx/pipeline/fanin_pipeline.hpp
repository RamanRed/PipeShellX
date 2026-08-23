#pragma once

#include "psx/os/tls.hpp"
#include "psx/pipeline/segmented_pipeline.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/native_controller.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace psx::pipeline {

// Fan-in: runs one source command on N nodes (a group), merges their stdout,
// and feeds it into a downstream pipeline (which may be empty, local, remote, or
// mixed). The source instances run in parallel; the downstream's stdin closes
// once every source instance has finished. The run completes when the downstream
// finishes (or, with no downstream, when the sources do). pipefail spans the
// source hosts and the downstream stages.
class FanInPipeline {
public:
    using Outcome = SegmentedPipeline::Outcome;
    using OnOutput = std::function<void(std::string_view)>;

    FanInPipeline(psx::runtime::Reactor& reactor,
                  psx::os::TlsConfig controllerConfig,
                  OnOutput onOutput = {},
                  OnOutput onStderr = {});
    ~FanInPipeline();
    FanInPipeline(const FanInPipeline&) = delete;
    FanInPipeline& operator=(const FanInPipeline&) = delete;

    psx::Result<void> run(const std::vector<std::string>& sourceArgv,
                          const std::vector<psx::transport::NativeController::Target>& sourceTargets,
                          const std::vector<ResolvedStage>& downstream,
                          std::function<void(Outcome)> onComplete);

private:
    void onSourceOutput(std::string_view data);
    void onSourceDone(std::vector<psx::transport::NativeController::HostResult> results);
    void onDownstreamDone(Outcome downstreamOutcome);
    void finish(Outcome outcome);

    psx::runtime::Reactor& reactor_;
    psx::os::TlsConfig config_;
    OnOutput onOutput_;
    OnOutput onStderr_;
    std::function<void(Outcome)> onComplete_;
    std::unique_ptr<psx::transport::NativeController> source_;
    std::unique_ptr<SegmentedPipeline> downstream_;
    bool hasDownstream_ = false;
    int sourceExit_ = 0;      // pipefail across the source hosts
    std::string sourceError_; // non-empty: a source host failed to run
    bool done_ = false;
};

} // namespace psx::pipeline
