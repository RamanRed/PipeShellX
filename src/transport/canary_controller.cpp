#include "psx/transport/canary_controller.hpp"

#include <algorithm>
#include <utility>

namespace psx::transport {

CanaryController::CanaryController(psx::runtime::Reactor& reactor,
                                   psx::os::TlsConfig controllerConfig,
                                   OnOutput onOutput)
    : reactor_(reactor), config_(std::move(controllerConfig)), onOutput_(std::move(onOutput)) {}

CanaryController::~CanaryController() = default;

psx::Result<void> CanaryController::start(const std::vector<Target>& targets,
                                          std::size_t canaryCount,
                                          const std::vector<std::string>& command,
                                          std::function<void(std::vector<HostResult>)> onComplete) {
    onComplete_ = std::move(onComplete);
    command_ = command;
    if (targets.empty()) {
        finish({});
        return {};
    }
    canaryCount = std::clamp<std::size_t>(canaryCount, 1, targets.size());

    const std::vector<Target> canaryTargets(targets.begin(), targets.begin() + static_cast<long>(canaryCount));
    restTargets_.assign(targets.begin() + static_cast<long>(canaryCount), targets.end());

    canary_ = std::make_unique<NativeController>(reactor_, config_, onOutput_);
    return canary_->start(canaryTargets, command_,
                          [this](std::vector<HostResult> results) { onCanaryDone(std::move(results)); });
}

void CanaryController::onCanaryDone(std::vector<HostResult> results) {
    if (done_) {
        return;
    }
    canaryResults_ = std::move(results);
    const bool allOk = std::all_of(canaryResults_.begin(), canaryResults_.end(),
                                   [](const HostResult& r) { return r.ok && r.exitCode == 0 && r.error.empty(); });

    if (allOk && !restTargets_.empty()) {
        rest_ = std::make_unique<NativeController>(reactor_, config_, onOutput_);
        (void)rest_->start(restTargets_, command_,
                           [this](std::vector<HostResult> more) { onRestDone(std::move(more)); });
        return;
    }

    // Canary failed (or nothing left to roll out): the rest are skipped.
    std::vector<HostResult> combined = std::move(canaryResults_);
    if (!allOk) {
        for (const Target& target : restTargets_) {
            HostResult skipped;
            skipped.host = target.host;
            skipped.error = "skipped: canary failed";
            combined.push_back(std::move(skipped));
        }
    }
    finish(std::move(combined));
}

void CanaryController::onRestDone(std::vector<HostResult> results) {
    if (done_) {
        return;
    }
    std::vector<HostResult> combined = std::move(canaryResults_);
    combined.insert(combined.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end()));
    finish(std::move(combined));
}

void CanaryController::cancel(const std::string& reason) {
    if (canary_) {
        canary_->cancel(reason);
    }
    if (rest_) {
        rest_->cancel(reason);
    }
}

void CanaryController::finish(std::vector<HostResult> results) {
    if (done_) {
        return;
    }
    done_ = true;
    if (onComplete_) {
        auto callback = std::move(onComplete_);
        callback(std::move(results)); // may destroy `this`; touch nothing after
    }
}

} // namespace psx::transport
