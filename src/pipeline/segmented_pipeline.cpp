#include "psx/pipeline/segmented_pipeline.hpp"

#include <utility>

namespace psx::pipeline {

void SegmentedPipeline::Segment::writeStdin(std::string_view bytes) {
    if (localRunner) {
        localRunner->writeStdin(bytes);
    } else if (remoteRunner) {
        remoteRunner->writeStdin(bytes);
    }
}

void SegmentedPipeline::Segment::closeStdin() {
    if (localRunner) {
        localRunner->closeStdin();
    } else if (remoteRunner) {
        remoteRunner->closeStdin();
    }
}

void SegmentedPipeline::Segment::cancel() {
    if (localRunner) {
        localRunner->cancel();
    } else if (remoteRunner) {
        remoteRunner->cancel();
    }
}

SegmentedPipeline::SegmentedPipeline(psx::runtime::Reactor& reactor,
                                     psx::os::TlsConfig controllerConfig,
                                     OnOutput onOutput,
                                     OnOutput onStderr)
    : reactor_(reactor), config_(std::move(controllerConfig)), onOutput_(std::move(onOutput)),
      onStderr_(std::move(onStderr)) {}

SegmentedPipeline::~SegmentedPipeline() = default;

psx::Result<void> SegmentedPipeline::run(const std::vector<ResolvedStage>& stages,
                                         std::function<void(Outcome)> onComplete,
                                         bool externalStdin) {
    if (stages.empty()) {
        return psx::Error{psx::ErrorClass::InvalidArgument, 0, "empty pipeline"};
    }
    onComplete_ = std::move(onComplete);
    externalStdin_ = externalStdin;

    // Split into maximal same-locality segments, keeping the resolved stages so
    // each segment's runner can be built after the whole chain is grouped.
    std::vector<std::vector<ResolvedStage>> grouped;
    for (const ResolvedStage& stage : stages) {
        if (grouped.empty() || grouped.back().front().isLocal() != stage.isLocal()) {
            grouped.push_back({});
        }
        grouped.back().push_back(stage);
    }

    segments_.resize(grouped.size());
    for (std::size_t i = 0; i < grouped.size(); ++i) {
        Segment& segment = segments_[i];
        segment.local = grouped[i].front().isLocal();
        segment.stageCount = grouped[i].size();
        auto onOut = [this, i](std::string_view data) { routeOutput(i, data); };
        if (segment.local) {
            segment.localRunner = std::make_unique<LocalRunner>(reactor_, std::move(onOut));
        } else {
            segment.remoteRunner = std::make_unique<DistributedRunner>(reactor_, config_, std::move(onOut), onStderr_);
        }
    }

    // Start each segment. Segments after the first are fed by the upstream, so
    // they take external stdin. run() is non-blocking, so all are live before
    // any output flows.
    for (std::size_t i = 0; i < grouped.size(); ++i) {
        Segment& segment = segments_[i];
        const bool externalStdin = i > 0 || externalStdin_; // segment 0 is fed when the pipeline itself is
        auto onDone = [this, i](std::vector<int> codes, std::string error) {
            onSegmentDone(i, std::move(codes), error);
        };
        if (segment.local) {
            std::vector<Stage> stagesForRunner;
            stagesForRunner.reserve(grouped[i].size());
            for (const ResolvedStage& s : grouped[i]) {
                stagesForRunner.push_back({.id = "", .argv = s.argv, .placement = ""});
            }
            auto started = segment.localRunner->run(
                stagesForRunner,
                [onDone](LocalRunner::Outcome outcome) { onDone(std::move(outcome.stageExitCodes), std::string()); },
                externalStdin);
            if (!started.ok()) {
                return started.error();
            }
        } else {
            std::vector<RemoteStage> stagesForRunner;
            stagesForRunner.reserve(grouped[i].size());
            for (const ResolvedStage& s : grouped[i]) {
                stagesForRunner.push_back(
                    {.argv = s.argv, .host = s.host, .port = s.port, .expectedSan = s.expectedSan});
            }
            auto started = segment.remoteRunner->run(
                stagesForRunner,
                [onDone](DistributedRunner::Outcome outcome) {
                    onDone(std::move(outcome.stageExitCodes), outcome.error);
                },
                externalStdin);
            if (!started.ok()) {
                return started.error();
            }
        }
    }
    return {};
}

void SegmentedPipeline::routeOutput(std::size_t index, std::string_view data) {
    if (done_) {
        return;
    }
    if (index + 1 < segments_.size()) {
        segments_[index + 1].writeStdin(data);
    } else if (onOutput_) {
        onOutput_(data);
    }
}

void SegmentedPipeline::onSegmentDone(std::size_t index, std::vector<int> exitCodes, const std::string& error) {
    if (done_) {
        return;
    }
    if (!error.empty()) {
        Outcome outcome;
        outcome.error = error;
        outcome.exitCode = 1;
        finish(std::move(outcome));
        return;
    }
    segments_[index].exitCodes = std::move(exitCodes);
    segments_[index].done = true;
    if (index + 1 < segments_.size()) {
        segments_[index + 1].closeStdin(); // EOF to the downstream segment
    } else {
        finalSegmentDone_ = true;
        cancelUnfinishedUpstream();
    }
    if (!cancellingUpstream_) {
        finishIfAllDone();
    }
}

void SegmentedPipeline::cancelUnfinishedUpstream() {
    cancellingUpstream_ = true;
    for (std::size_t i = 0; i + 1 < segments_.size(); ++i) {
        if (!segments_[i].done) {
            segments_[i].cancel(); // completes that segment with real/known codes
        }
    }
    cancellingUpstream_ = false;
}

void SegmentedPipeline::finishIfAllDone() {
    if (!finalSegmentDone_) {
        return;
    }
    for (const Segment& segment : segments_) {
        if (!segment.done) {
            return;
        }
    }
    Outcome outcome;
    for (const Segment& segment : segments_) {
        for (const int code : segment.exitCodes) {
            outcome.stageExitCodes.push_back(code);
            if (code != 0) {
                outcome.exitCode = code; // pipefail: rightmost non-zero
            }
        }
    }
    finish(std::move(outcome));
}

void SegmentedPipeline::writeStdin(std::string_view bytes) {
    if (!done_ && !segments_.empty()) {
        segments_.front().writeStdin(bytes);
    }
}
void SegmentedPipeline::closeStdin() {
    if (!done_ && !segments_.empty()) {
        segments_.front().closeStdin();
    }
}
void SegmentedPipeline::finish(Outcome outcome) {
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
