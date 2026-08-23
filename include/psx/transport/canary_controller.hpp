#pragma once

#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/native_controller.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psx::transport {

// Staged rollout over the native backplane: runs `command` on the first
// `canaryCount` targets, and only if every canary host succeeds does it run on
// the rest. If any canary host fails, the rest are skipped (reported, not run).
// onComplete receives all hosts' results in target order. All on one reactor.
class CanaryController {
public:
    using Target = NativeController::Target;
    using HostResult = NativeController::HostResult;
    using OnOutput = NativeController::OnOutput;

    CanaryController(psx::runtime::Reactor& reactor, psx::os::TlsConfig controllerConfig, OnOutput onOutput = {});
    ~CanaryController();
    CanaryController(const CanaryController&) = delete;
    CanaryController& operator=(const CanaryController&) = delete;

    // canaryCount is clamped to [1, targets.size()]. Returns a setup error only;
    // host failures complete normally via onComplete.
    psx::Result<void> start(const std::vector<Target>& targets,
                            std::size_t canaryCount,
                            const std::vector<std::string>& command,
                            std::function<void(std::vector<HostResult>)> onComplete);

    void cancel(const std::string& reason);

private:
    void onCanaryDone(std::vector<HostResult> results);
    void onRestDone(std::vector<HostResult> results);
    void finish(std::vector<HostResult> results);

    psx::runtime::Reactor& reactor_;
    psx::os::TlsConfig config_;
    OnOutput onOutput_;
    std::function<void(std::vector<HostResult>)> onComplete_;
    std::vector<Target> restTargets_;
    std::vector<std::string> command_;
    std::vector<HostResult> canaryResults_;
    std::unique_ptr<NativeController> canary_;
    std::unique_ptr<NativeController> rest_;
    bool done_ = false;
};

} // namespace psx::transport
