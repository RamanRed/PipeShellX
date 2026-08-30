#include "psx/pipeline/fanin_pipeline.hpp"

#include <utility>

namespace psx::pipeline {

using psx::transport::NativeController;

FanInPipeline::FanInPipeline(psx::runtime::Reactor& reactor,
                             psx::os::TlsConfig controllerConfig,
                             OnOutput onOutput,
                             OnOutput onStderr)
    : reactor_(reactor), config_(std::move(controllerConfig)), onOutput_(std::move(onOutput)),
      onStderr_(std::move(onStderr)) {}

FanInPipeline::~FanInPipeline() = default;

psx::Result<void> FanInPipeline::run(const std::vector<std::string>& sourceArgv,
                                     const std::vector<NativeController::Target>& sourceTargets,
                                     const std::vector<ResolvedStage>& downstream,
                                     std::function<void(Outcome)> onComplete) {
    if (sourceArgv.empty() || sourceTargets.empty()) {
        return psx::Error{psx::ErrorClass::InvalidArgument, 0, "fan-in needs a source command and hosts"};
    }
    onComplete_ = std::move(onComplete);
    hasDownstream_ = !downstream.empty();

    // Start the downstream first so the merged source output always has a live
    // consumer (writeStdin buffers until its runners are up).
    if (hasDownstream_) {
        downstream_ = std::make_unique<SegmentedPipeline>(
            reactor_, config_, [this](std::string_view data) { onOutput_ ? onOutput_(data) : void(); }, onStderr_);
        auto started = downstream_->run(
            downstream, [this](Outcome outcome) { onDownstreamDone(std::move(outcome)); }, /*externalStdin=*/true);
        if (!started.ok()) {
            return started.error();
        }
    }

    // The source runs the command on every host; its merged stdout is routed
    // either into the downstream's stdin or straight to the output.
    source_ = std::make_unique<NativeController>(
        reactor_, config_, [this](const std::string&, std::string_view bytes, psx::transport::Channel channel) {
            if (channel == psx::transport::Channel::Stderr) {
                if (onStderr_) {
                    onStderr_(bytes);
                }
                return;
            }
            onSourceOutput(bytes);
        });
    return source_->start(sourceTargets, sourceArgv, [this](std::vector<NativeController::HostResult> results) {
        onSourceDone(std::move(results));
    });
}

void FanInPipeline::onSourceOutput(std::string_view data) {
    if (done_) {
        return;
    }
    if (hasDownstream_) {
        downstream_->writeStdin(data);
    } else if (onOutput_) {
        onOutput_(data);
    }
}

void FanInPipeline::onSourceDone(std::vector<NativeController::HostResult> results) {
    if (done_) {
        return;
    }
    sourceDone_ = true;
    for (const auto& result : results) {
        if (sourceCancellationRequested_ && !result.ok && result.error == "downstream completed") {
            sourceExit_ = 137; // NativeController closed it; node fencing is SIGKILL
        } else if (!result.ok || !result.error.empty()) {
            sourceError_ =
                "source host " + result.host + ": " + (result.error.empty() ? "non-zero exit" : result.error);
        } else if (result.exitCode != 0) {
            sourceExit_ = result.exitCode; // pipefail across the source hosts
        }
    }
    if (!hasDownstream_) {
        Outcome outcome;
        outcome.exitCode = sourceExit_;
        outcome.error = sourceError_;
        finish(std::move(outcome));
        return;
    }
    if (!downstreamDone_) {
        downstream_->closeStdin(); // every source finished: EOF the downstream
    }
    finishIfReady();
}

void FanInPipeline::onDownstreamDone(Outcome downstreamOutcome) {
    if (done_) {
        return;
    }
    downstreamOutcome_ = std::move(downstreamOutcome);
    downstreamDone_ = true;
    if (!sourceDone_) {
        sourceCancellationRequested_ = true;
        source_->cancel("downstream completed");
        return; // cancellation completion may synchronously destroy this
    }
    finishIfReady();
}

void FanInPipeline::finishIfReady() {
    if (!sourceDone_ || !downstreamDone_ || !downstreamOutcome_) {
        return;
    }
    // pipefail: a downstream failure wins (it is rightmost); otherwise a source
    // failure surfaces. A source connect failure is reported via error.
    Outcome outcome = std::move(*downstreamOutcome_);
    downstreamOutcome_.reset();
    if (outcome.exitCode == 0 && sourceExit_ != 0) {
        outcome.exitCode = sourceExit_;
    }
    if (outcome.error.empty() && !sourceError_.empty()) {
        outcome.error = sourceError_;
    }
    finish(std::move(outcome));
}

void FanInPipeline::finish(Outcome outcome) {
    if (done_) {
        return;
    }
    done_ = true;
    if (onComplete_) {
        auto callback = std::move(onComplete_);
        callback(std::move(outcome)); // may destroy `this`; touch nothing after
    }
}

} // namespace psx::pipeline
