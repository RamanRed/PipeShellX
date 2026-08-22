#include "process_manager.hpp"

#include "logger.hpp"
#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"
#include "psx/os/process.hpp"
#include "psx/runtime/reactor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using psx::os::Handle;
using psx::os::Pipe;
using psx::os::Process;
using psx::os::SpawnSpec;
using psx::runtime::Reactor;
using psx::runtime::Token;

// Resource caps for local children (unchanged from v0.1.0; configurable per
// stage in Phase 6).
constexpr std::uint32_t kLocalCpuSeconds = 5;
constexpr std::uint64_t kLocalAddressSpaceBytes = 64ULL * 1024 * 1024;

// After SIGKILLing a timed-out process group, how long to keep draining its
// pipes before giving up on a holder outside the group.
constexpr std::chrono::seconds kDrainGrace{2};

// Descriptor at which a password-backed worker finds its secret (sshpass -d).
constexpr int kPasswordFd = 3;
// Largest secret that fits a fresh pipe without blocking on every platform.
constexpr std::size_t kMaxPasswordBytes = 4096;

[[noreturn]] void fail(const std::string& what, const psx::Error& error) {
    throw std::runtime_error(what + ": " + error.message());
}

// One child on the reactor: a local command or one `ssh` worker.
struct Worker {
    std::string clientId;
    LogContext context;
    std::string failureMessage; // non-empty when the program could not be started
    Process process;
    Handle stdoutReader;
    Handle stderrReader;
    Handle stdinWriter;
    std::string_view input;
    std::size_t inputWritten = 0;
    std::string stdoutData;
    std::string stderrData;
    std::optional<psx::os::ExitStatus> status;
    Token stdoutToken = 0;
    Token stderrToken = 0;
    Token stdinToken = 0;
    bool stdoutClosed = false;
    bool stderrClosed = false;
    bool stdinClosed = false;
    bool exited = false;
    bool timedOut = false;

    bool complete() const noexcept { return exited && stdoutClosed && stderrClosed && stdinClosed; }

    // v0.1.0 contract: exit code, -1 when signal-terminated, 127 when not started.
    int exitCode() const noexcept {
        if (!failureMessage.empty()) {
            return 127;
        }
        if (!status || status->kind != psx::os::ExitStatus::Kind::Exited) {
            return -1;
        }
        return status->code;
    }
};

// Drives a set of workers to completion on one reactor with a shared
// deadline: SIGKILL the process groups at the deadline, drain for at most
// kDrainGrace, then abandon whatever still holds the pipes.
class WorkerRun {
public:
    WorkerRun(Reactor& reactor, std::vector<Worker>& workers, int timeoutSec)
        : reactor_(reactor), workers_(workers), timeoutSec_(timeoutSec) {}

    bool anyTimedOut() const noexcept { return anyTimedOut_; }

    void run() {
        for (auto& worker : workers_) {
            arm(worker);
        }
        if (timeoutSec_ > 0) {
            reactor_.after(std::chrono::seconds(timeoutSec_), [this] { onDeadline(); });
        }
        checkComplete();
        if (!allComplete()) {
            if (auto ran = reactor_.run(); !ran.ok()) {
                fail("event loop failed", ran.error());
            }
        }
    }

private:
    bool allComplete() const noexcept {
        return std::all_of(workers_.begin(), workers_.end(), [](const Worker& w) { return w.complete(); });
    }

    void checkComplete() {
        if (allComplete()) {
            reactor_.stop();
        }
    }

    void watch(Worker& worker, Handle& handle, psx::os::Interest interest, Token& token, Reactor::IoHandler handler) {
        if (auto setNonBlocking = handle.setNonBlocking(true); !setNonBlocking.ok()) {
            fail("fcntl(O_NONBLOCK) failed for " + worker.clientId, setNonBlocking.error());
        }
        auto watched = reactor_.watch(handle, interest, std::move(handler));
        if (!watched.ok()) {
            fail("registering pipe failed for " + worker.clientId, watched.error());
        }
        token = watched.value();
    }

    void arm(Worker& worker) {
        if (!worker.process.running()) {
            return; // could not be started: already complete
        }
        watch(worker, worker.stdoutReader, psx::os::Interest::Readable, worker.stdoutToken,
              [this, &worker](psx::os::Readiness readiness) { onReadable(worker, true, readiness); });
        watch(worker, worker.stderrReader, psx::os::Interest::Readable, worker.stderrToken,
              [this, &worker](psx::os::Readiness readiness) { onReadable(worker, false, readiness); });
        if (!worker.stdinClosed) {
            watch(worker, worker.stdinWriter, psx::os::Interest::Writable, worker.stdinToken,
                  [this, &worker](psx::os::Readiness) { onWritable(worker); });
        }
        if (auto watched =
                reactor_.watchChild(worker.process.id(), [this, &worker](psx::os::ProcessId) { onExit(worker); });
            !watched.ok()) {
            fail("watching child failed for " + worker.clientId, watched.error());
        }
    }

    void closeStream(Handle& handle, Token& token, bool& closed) {
        if (closed) {
            return;
        }
        if (token != 0) {
            (void)reactor_.unwatch(token);
            token = 0;
        }
        handle.close();
        closed = true;
    }

    void onReadable(Worker& worker, bool isStdout, psx::os::Readiness readiness) {
        Handle& handle = isStdout ? worker.stdoutReader : worker.stderrReader;
        Token& token = isStdout ? worker.stdoutToken : worker.stderrToken;
        bool& closed = isStdout ? worker.stdoutClosed : worker.stderrClosed;
        std::string& data = isStdout ? worker.stdoutData : worker.stderrData;
        const std::size_t before = data.size();

        bool endOfStream = false;
        char buffer[64 * 1024];
        while (true) { // edge-triggered discipline: drain until WouldBlock
            auto chunk = psx::os::read(handle, std::span<char>(buffer, sizeof(buffer)));
            if (!chunk.ok()) {
                if (chunk.error().cls == psx::ErrorClass::WouldBlock) {
                    // A hang-up with nothing left to read is the end of the stream.
                    endOfStream = psx::os::has(readiness, psx::os::Readiness::Hangup) && data.size() == before;
                } else {
                    endOfStream = true; // EBADF/EIO: nothing more will ever arrive
                }
                break;
            }
            if (chunk.value() == 0) {
                endOfStream = true;
                break;
            }
            data.append(buffer, chunk.value());
        }

        if (data.size() > before && Logger::getInstance().enabled(LogLevel::DEBUG)) {
            Logger::getInstance().log(LogLevel::DEBUG, worker.context,
                                      "Read " + std::to_string(data.size() - before) +
                                          (isStdout ? " bytes from child stdout" : " bytes from child stderr"));
        }
        if (endOfStream) {
            closeStream(handle, token, closed);
            checkComplete();
        }
    }

    void onWritable(Worker& worker) {
        while (worker.inputWritten < worker.input.size()) {
            auto written =
                psx::os::write(worker.stdinWriter, std::span<const char>(worker.input.data() + worker.inputWritten,
                                                                         worker.input.size() - worker.inputWritten));
            if (!written.ok()) {
                if (written.error().cls == psx::ErrorClass::WouldBlock) {
                    return; // the pipe is full; wait for the next Writable event
                }
                break; // BrokenPipe: the child stopped reading
            }
            worker.inputWritten += written.value();
        }
        closeStream(worker.stdinWriter, worker.stdinToken, worker.stdinClosed);
        Logger::getInstance().log(LogLevel::DEBUG, worker.context, "Finished writing command input to child stdin");
        checkComplete();
    }

    void onExit(Worker& worker) {
        auto status = worker.process.tryWait();
        if (status.ok() && status.value().has_value()) {
            worker.status = *status.value();
        } else if (auto blocking = worker.process.wait(); blocking.ok()) {
            worker.status = blocking.value(); // notified, so this cannot block for long
        }
        worker.exited = true;
        if (Logger::getInstance().enabled(LogLevel::DEBUG)) {
            Logger::getInstance().log(LogLevel::DEBUG, worker.context, "Child process reaped");
        }
        checkComplete();
    }

    void onDeadline() {
        for (auto& worker : workers_) {
            if (worker.complete()) {
                continue;
            }
            worker.timedOut = true;
            anyTimedOut_ = true;
            (void)worker.process.signal(psx::os::StopSignal::Kill); // the group, even if the leader is gone
            Logger::getInstance().log(LogLevel::ERROR, worker.context,
                                      "Command timed out; sent SIGKILL to process group");
        }
        reactor_.after(kDrainGrace, [this] { onDrainExpired(); });
    }

    void onDrainExpired() {
        for (auto& worker : workers_) {
            if (worker.complete()) {
                continue;
            }
            // Something outside the process group still holds the pipes; stop waiting for it.
            closeStream(worker.stdoutReader, worker.stdoutToken, worker.stdoutClosed);
            closeStream(worker.stderrReader, worker.stderrToken, worker.stderrClosed);
            closeStream(worker.stdinWriter, worker.stdinToken, worker.stdinClosed);
            if (!worker.exited) {
                (void)reactor_.unwatchChild(worker.process.id());
                onExit(worker); // SIGKILLed: the wait returns promptly
            }
            Logger::getInstance().log(LogLevel::ERROR, worker.context,
                                      "Pipes still open after the SIGKILL grace period; abandoning remaining output");
        }
        checkComplete();
    }

    Reactor& reactor_;
    std::vector<Worker>& workers_;
    int timeoutSec_;
    bool anyTimedOut_ = false;
};

// Creates the stdio pipes for a worker, spawns it, and leaves the parent ends
// in the worker. A start failure is recorded (not thrown) so that one bad
// worker never cancels its siblings.
void spawnWorker(Worker& worker, SpawnSpec spec, bool feedInput) {
    auto stdoutPipe = Pipe::create();
    auto stderrPipe = Pipe::create();
    if (!stdoutPipe.ok()) {
        fail("stdout pipe creation failed for " + worker.clientId, stdoutPipe.error());
    }
    if (!stderrPipe.ok()) {
        fail("stderr pipe creation failed for " + worker.clientId, stderrPipe.error());
    }
    Pipe stdinPipe;
    if (feedInput) {
        auto created = Pipe::create();
        if (!created.ok()) {
            fail("stdin pipe creation failed for " + worker.clientId, created.error());
        }
        stdinPipe = std::move(created.value());
        spec.in = SpawnSpec::Stdio::from(stdinPipe.reader);
    } else {
        spec.in = SpawnSpec::Stdio::null();
        worker.stdinClosed = true;
    }
    spec.out = SpawnSpec::Stdio::from(stdoutPipe.value().writer);
    spec.err = SpawnSpec::Stdio::from(stderrPipe.value().writer);

    auto process = Process::spawn(spec);
    if (!process.ok()) {
        // Same shape as the v0.1.0 child-side message, so classifiers and the
        // terminal's "command execution failed" mapping keep working.
        worker.failureMessage =
            "execvp failed for " +
            (worker.clientId == "-" ? "'" + spec.program + "'" : "ssh client '" + worker.clientId + "'") + ": " +
            process.error().message() + "\n";
        worker.stderrData = worker.failureMessage;
        worker.stdoutClosed = worker.stderrClosed = worker.stdinClosed = worker.exited = true;
        Logger::getInstance().log(LogLevel::ERROR, worker.context, worker.failureMessage);
        return;
    }
    worker.process = std::move(process.value());
    worker.stdoutReader = std::move(stdoutPipe.value().reader);
    worker.stderrReader = std::move(stderrPipe.value().reader);
    if (feedInput) {
        worker.stdinWriter = std::move(stdinPipe.writer);
    }
    // The child owns the other ends now; the Pipe temporaries close them here.
}

} // namespace

ProcessManager::ProcessManager() = default;
ProcessManager::~ProcessManager() = default;
ProcessManager::ProcessManager(ProcessManager&&) noexcept = default;
ProcessManager& ProcessManager::operator=(ProcessManager&&) noexcept = default;

// Test/diagnostic hook: PIPESHELLX_POLLER=poll|epoll|kqueue forces a
// backend; anything else (or unset) means the platform's native one.
psx::os::Poller::Backend pollerBackendFromEnvironment() {
    const char* value = std::getenv("PIPESHELLX_POLLER");
    const std::string_view choice = value != nullptr ? value : "";
    if (choice == "poll") {
        return psx::os::Poller::Backend::Poll;
    }
    if (choice == "epoll") {
        return psx::os::Poller::Backend::Epoll;
    }
    if (choice == "kqueue") {
        return psx::os::Poller::Backend::Kqueue;
    }
    return psx::os::Poller::Backend::Auto;
}

psx::runtime::Reactor& ProcessManager::reactor() {
    if (!reactor_) {
        Reactor::Options options;
        options.backend = pollerBackendFromEnvironment();
        auto created = Reactor::create(options);
        if (!created.ok()) {
            fail("event loop creation failed", created.error());
        }
        reactor_ = std::move(created.value());
    }
    return *reactor_;
}

ProcessManager::Result ProcessManager::execute(const std::vector<std::string>& args,
                                               const LogContext& context,
                                               const std::string& input,
                                               int timeoutSec) {
    if (args.empty()) {
        throw std::runtime_error("cannot execute empty command");
    }

    std::vector<Worker> workers(1);
    Worker& worker = workers.front();
    worker.clientId = "-";
    worker.context = context;
    worker.input = input;

    SpawnSpec spec;
    spec.program = args.front(); // PATH lookup when there is no '/', like execvp
    spec.argv = args;
    spec.limits.cpuSeconds = kLocalCpuSeconds;
    spec.limits.addressSpaceBytes = kLocalAddressSpaceBytes;
    Logger::getInstance().log(LogLevel::DEBUG, context, "Creating IPC pipes");
    spawnWorker(worker, std::move(spec), !input.empty());
    if (worker.process.running()) {
        Logger::getInstance().log(LogLevel::INFO,
                                  LogContext{worker.process.id(), context.sessionId, context.clientId, context.command},
                                  "Child process created");
    }

    WorkerRun run(reactor(), workers, timeoutSec);
    run.run();

    Logger::getInstance().log(worker.exitCode() == 0 ? LogLevel::INFO : LogLevel::ERROR, context,
                              "Child exited with status " + std::to_string(worker.exitCode()) +
                                  (worker.timedOut ? " (timed out)" : ""));
    return Result{worker.exitCode(), std::move(worker.stdoutData), std::move(worker.stderrData), worker.timedOut, {}};
}

ProcessManager::Result ProcessManager::executeRemote(const std::vector<ClientEntry>& clients,
                                                     const std::string& remoteCommand,
                                                     const LogContext& context,
                                                     int timeoutSec) {
    if (clients.empty()) {
        throw std::runtime_error("no clients configured for remote execution");
    }

    std::vector<Worker> workers(clients.size());
    for (std::size_t index = 0; index < clients.size(); ++index) {
        const ClientEntry& client = clients[index];
        Worker& worker = workers[index];
        worker.clientId = client.clientId();
        worker.context = LogContext{context.pid, context.sessionId, client.clientId(),
                                    "ssh " + client.clientId() + " " + remoteCommand};

        // Passwords never touch argv: sshpass reads the secret from an
        // inherited pipe created here (non-inheritable everywhere else).
        Pipe passwordPipe;
        SpawnSpec spec;
        if (!client.password.empty()) {
            if (client.password.size() >= kMaxPasswordBytes) {
                worker.failureMessage = "password too long for secure hand-off\n";
                worker.stderrData = worker.failureMessage;
                worker.stdoutClosed = worker.stderrClosed = worker.stdinClosed = worker.exited = true;
                continue;
            }
            auto created = Pipe::create();
            if (!created.ok()) {
                fail("password pipe creation failed for " + worker.clientId, created.error());
            }
            passwordPipe = std::move(created.value());
            const std::string secret = client.password + "\n";
            auto written = psx::os::write(passwordPipe.writer, std::span<const char>(secret.data(), secret.size()));
            if (!written.ok() || written.value() != secret.size()) {
                fail("password pipe write failed for " + worker.clientId,
                     written.ok() ? psx::Error{psx::ErrorClass::Other, 0, "write"} : written.error());
            }
            passwordPipe.writer.close();
            spec.extraHandles = {{&passwordPipe.reader, kPasswordFd}};
        }
        spec.argv = buildSshCommandArguments(client, remoteCommand, client.password.empty() ? -1 : kPasswordFd);
        spec.program = spec.argv.front();

        spawnWorker(worker, std::move(spec), false);
        if (worker.process.running()) {
            Logger::getInstance().log(LogLevel::INFO,
                                      LogContext{worker.process.id(), context.sessionId, client.clientId(),
                                                 "ssh " + client.clientId() + " " + remoteCommand},
                                      "Remote SSH worker created");
        }
        // passwordPipe closes here: only the child holds the secret now.
    }

    WorkerRun run(reactor(), workers, timeoutSec);
    run.run();

    std::vector<ClientResult> clientResults;
    clientResults.reserve(workers.size());
    int overallExitCode = 0;
    for (auto& worker : workers) {
        const int exitCode = worker.exitCode();
        if (exitCode != 0 || worker.timedOut) {
            overallExitCode = exitCode == 0 ? 1 : exitCode;
        }
        clientResults.push_back(ClientResult{worker.clientId,
                                             exitCode,
                                             std::move(worker.stdoutData),
                                             std::move(worker.stderrData),
                                             {},
                                             worker.timedOut});
    }

    for (auto& clientResult : clientResults) {
        clientResult.errorMessage = classifyRemoteError(clientResult);
        if (!clientResult.errorMessage.empty()) {
            Logger::getInstance().log(LogLevel::ERROR,
                                      LogContext{context.pid, context.sessionId, clientResult.clientId,
                                                 "ssh " + clientResult.clientId + " " + remoteCommand},
                                      clientResult.errorMessage);
        }
    }

    return Result{overallExitCode, formatClientResults(clientResults, true), formatClientResults(clientResults, false),
                  run.anyTimedOut(), std::move(clientResults)};
}

std::string ProcessManager::formatClientResults(const std::vector<ClientResult>& clientResults, bool useStdout) const {
    std::string formatted;
    for (const auto& result : clientResults) {
        const std::string& selectedOutput = useStdout ? result.stdoutData : result.errorMessage;
        if (selectedOutput.empty()) {
            continue;
        }

        formatted += "CLIENT " + result.clientId + "\n";
        formatted += selectedOutput;
        if (formatted.back() != '\n') {
            formatted += '\n';
        }
    }
    return formatted;
}

std::string ProcessManager::classifyRemoteError(const ClientResult& clientResult) const {
    if (clientResult.timedOut) {
        return "ERROR: command timed out";
    }

    // Messages written by our own worker child (childExitWithError) have a
    // fixed spelling; everything else on stderr came from ssh/sshpass.
    const std::string& stderrText = clientResult.stderrData;
    if (stderrText.find("execvp failed") != std::string::npos) {
        return "ERROR: command execution failed";
    }
    if (stderrText.find("dup2 failed") != std::string::npos ||
        stderrText.find("open(/dev/null) failed") != std::string::npos ||
        stderrText.find("setpgid failed") != std::string::npos ||
        stderrText.find("password pipe") != std::string::npos ||
        stderrText.find("password too long") != std::string::npos) {
        return "ERROR: remote worker setup failed";
    }

    if (const auto sshFailure = classifySshFailure(stderrText); sshFailure.has_value()) {
        return "ERROR: " + *sshFailure;
    }

    if (clientResult.exitCode != 0) {
        return "ERROR: command failed with exit code " + std::to_string(clientResult.exitCode);
    }

    return {};
}
