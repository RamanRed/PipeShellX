#pragma once

#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/native_transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psx::transport {

// The controller side of the backplane: connects to a set of nodes over mTLS,
// runs one command on each as a stream, and collects the merged output + exit.
// All connections share one reactor. onComplete fires once every target has
// finished or failed. Not thread-safe.
class NativeController {
public:
    struct Target {
        std::string host;
        std::uint16_t port = 0;
        std::string expectedSan; // the node identity to authorise (empty = any authenticated)
    };
    struct HostResult {
        std::string host;
        bool ok = false;    // the stage ran to an EXIT (not a connect/auth failure)
        int exitCode = -1;  // the stage's exit code (Exited) or -1
        std::string error;  // non-empty on a transport/auth failure
        std::string output; // merged stdout+stderr bytes
    };
    // Live output callback: (host, bytes) as each DATA chunk arrives.
    using OnOutput = std::function<void(const std::string&, std::string_view)>;

    NativeController(psx::runtime::Reactor& reactor, psx::os::TlsConfig controllerConfig, OnOutput onOutput = {});
    ~NativeController();
    NativeController(const NativeController&) = delete;
    NativeController& operator=(const NativeController&) = delete;

    // Connects to every target and runs `command`. onComplete gets the per-host
    // results (in target order) once all targets are done.
    psx::Result<void> start(const std::vector<Target>& targets,
                            const std::vector<std::string>& command,
                            std::function<void(std::vector<HostResult>)> onComplete);

    // Fail every still-running target with `reason` (e.g. a run deadline); this
    // completes the run via onComplete like any other finish.
    void cancel(const std::string& reason);

private:
    struct Conn;
    void onConnDone(std::size_t index);

    psx::runtime::Reactor& reactor_;
    psx::os::TlsConfig config_;
    OnOutput onOutput_;
    std::vector<std::string> command_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::function<void(std::vector<HostResult>)> onComplete_;
    std::size_t remaining_ = 0;
    bool completed_ = false;
};

} // namespace psx::transport
