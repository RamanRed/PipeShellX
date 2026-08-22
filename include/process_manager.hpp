#pragma once

#include "client_config.hpp"
#include "logger.hpp"
#include "psx/stream/bounded_buffer.hpp" // OverflowPolicy
#include "ssh_auth.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace psx::runtime {
class Reactor;
}
namespace psx::sink {
class Sink;
}

// Runs allowlisted commands locally, or one `ssh` worker per client, and
// collects their output. Built on psx::os::Process + psx::runtime::Reactor:
// no fork() on the hot path, every descriptor non-inheritable, child exits
// and deadlines delivered as reactor events.
class ProcessManager {
public:
    struct ClientResult {
        std::string clientId;
        int exitCode;
        std::string stdoutData;
        std::string stderrData;
        std::string errorMessage;
        bool timedOut;
        std::uint64_t droppedBytes = 0; // bytes discarded by a drop-policy ring
    };

    struct Result {
        int exitCode;
        std::string stdoutData;
        std::string stderrData;
        bool timedOut;
        std::vector<ClientResult> clientResults;
    };

    ProcessManager();
    ~ProcessManager();

    // Non-copyable, movable
    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;
    ProcessManager(ProcessManager&&) noexcept;
    ProcessManager& operator=(ProcessManager&&) noexcept;

    // Execute command with arguments, optional input, timeout in seconds.
    // exitCode is -1 for a signal-terminated child and 127 when the program
    // could not be started (stderr then carries the reason).
    Result execute(const std::vector<std::string>& args,
                   const LogContext& context,
                   const std::string& input = "",
                   int timeoutSec = 0);
    // When `sink` is non-null it receives live, line-framed, per-stage output
    // (host-tagged) during the run, plus a stageFinished per client and a
    // final runFinished; the returned Result still carries the full capture.
    // `concurrency` bounds how many workers run at once (0 = all at once,
    // the default is 64): a large fan-out spawns ssh processes in a sliding
    // window rather than all at once.
    // `policy`/`ringBytes` bound the captured output: a drop-policy ring keeps
    // only the newest/oldest `ringBytes` of each stream (dropped bytes counted),
    // so `--stream` over an endless command keeps a flat controller RSS; Block
    // (or ringBytes 0) captures everything. The sink still sees every byte live.
    Result executeRemote(const std::vector<ClientEntry>& clients,
                         const std::string& remoteCommand,
                         const LogContext& context,
                         int timeoutSec = 0,
                         psx::sink::Sink* sink = nullptr,
                         std::size_t concurrency = 64,
                         psx::stream::OverflowPolicy policy = psx::stream::OverflowPolicy::Block,
                         std::size_t ringBytes = 0);

private:
    psx::runtime::Reactor& reactor();
    std::string formatClientResults(const std::vector<ClientResult>& clientResults, bool useStdout) const;
    std::string classifyRemoteError(const ClientResult& clientResult) const;

    std::unique_ptr<psx::runtime::Reactor> reactor_;
};
