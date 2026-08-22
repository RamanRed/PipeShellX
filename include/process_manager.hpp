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
        bool cancelled = false;         // run was cancelled (SIGINT) before this stage finished
    };

    struct Result {
        int exitCode;
        std::string stdoutData;
        std::string stderrData;
        bool timedOut;
        bool cancelled = false; // the run was cancelled by SIGINT (exit 130)
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
    // Options for executeRemote(). Grouped so the call site names what it sets
    // and callers that only need a timeout stay a one-liner.
    struct RemoteRunOptions {
        // Per-stage + global deadline in seconds; 0 disables it.
        int timeoutSec = 0;
        // When non-null, receives live line-framed per-stage output (host-tagged),
        // a stageFinished per client and a final runFinished. The returned Result
        // still carries the full capture regardless.
        psx::sink::Sink* sink = nullptr;
        // How many workers run at once (0 = all): a large fan-out spawns ssh in a
        // sliding window instead of all at once.
        std::size_t concurrency = 64;
        // policy/ringBytes bound the captured output: a drop-policy ring keeps only
        // the newest/oldest ringBytes of each stream (drops counted), so --stream
        // over an endless command keeps flat RSS; Block (or ringBytes 0) keeps all.
        psx::stream::OverflowPolicy policy = psx::stream::OverflowPolicy::Block;
        std::size_t ringBytes = 0;
        // Non-empty enables ssh ControlMaster reuse with this socket path template.
        std::string controlPath;
        // When true a SIGINT drains in-flight workers (TERM then a KILL grace) and
        // the run reports Result::cancelled (the CLI maps that to exit 130).
        bool cancellable = false;
    };

    // Callers pass a RemoteRunOptions (use {} for all-defaults). A default
    // argument here is not possible: a `= {}` default cannot see the nested
    // struct's member initializers while ProcessManager is still incomplete.
    Result executeRemote(const std::vector<ClientEntry>& clients,
                         const std::string& remoteCommand,
                         const LogContext& context,
                         const RemoteRunOptions& options);

private:
    psx::runtime::Reactor& reactor(bool withSignals = false);
    std::string formatClientResults(const std::vector<ClientResult>& clientResults, bool useStdout) const;
    std::string classifyRemoteError(const ClientResult& clientResult) const;

    std::unique_ptr<psx::runtime::Reactor> reactor_;
};
