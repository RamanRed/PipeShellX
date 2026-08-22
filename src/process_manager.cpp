#include "process_manager.hpp"

#include "cli_options.hpp"
#include "logger.hpp"
#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"
#include "psx/os/process.hpp"
#include "psx/stream/spool_buffer.hpp"
#include "ssh_transport.hpp"

#include "psx/runtime/backoff.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/sink/sink.hpp"
#include "psx/stream/line_framer.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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
// Largest secret that fits a fresh pipe without blocking on every platform.

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
    psx::stream::LineFramer stdoutFramer;
    psx::stream::LineFramer stderrFramer;
    Token stdoutToken = 0;
    Token stderrToken = 0;
    Token stdinToken = 0;
    bool stdoutClosed = false;
    bool stderrClosed = false;
    bool stdinClosed = false;
    bool exited = false;
    bool timedOut = false;
    bool cancelled = false;
    bool aborted = false;                 // aborted by --fail-fast because a sibling failed
    int attempt = 0;                      // retries performed so far (0 on the first run)
    bool awaitingRetry = false;           // finished a failed attempt, waiting on a backoff timer
    std::uint64_t droppedBytes = 0;       // bytes discarded from stdout/stderr by the ring
    psx::stream::SpoolBuffer stdoutSpool; // Spool policy: overflow spilled to disk
    psx::stream::SpoolBuffer stderrSpool;
    bool counted = false; // WorkerRun: already subtracted from the remaining count
    bool spawned = false; // WorkerRun: spawnFn has been invoked for this worker

    bool complete() const noexcept { return exited && stdoutClosed && stderrClosed && stdinClosed; }

    // A worker that never started: it is immediately complete, carries the
    // reason on stderr (v0.1.0 shape, consumed by classifyRemoteError and the
    // REPL) and in failureMessage for the result.
    void failToStart(std::string message) {
        failureMessage = message;
        stderrData = std::move(message);
        stdoutClosed = stderrClosed = stdinClosed = exited = true;
    }

    // Wipe the per-attempt state so a retry starts clean, keeping identity
    // (clientId/context/input) and the retry counter. Fresh pipes and Process
    // are installed by spawnWorker(); the old handles were already closed.
    void resetForRetry() {
        failureMessage.clear();
        stdoutData.clear();
        stderrData.clear();
        status.reset();
        stdoutFramer = psx::stream::LineFramer{};
        stderrFramer = psx::stream::LineFramer{};
        stdoutToken = stderrToken = stdinToken = 0;
        stdoutClosed = stderrClosed = stdinClosed = false;
        exited = false;
        timedOut = false;
        cancelled = false;
        aborted = false;
        droppedBytes = 0;
        stdoutSpool.reset();
        stderrSpool.reset();
        inputWritten = 0;
        counted = false;
        awaitingRetry = false;
    }

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
    // `spawn(index)` populates workers_[index] (process + streams); the run
    // keeps at most `concurrency` workers in flight, spawning the next pending
    // one as each running process exits. concurrency 0 means "all at once".
    WorkerRun(Reactor& reactor,
              std::vector<Worker>& workers,
              int timeoutSec,
              psx::sink::Sink* sink,
              std::size_t concurrency,
              std::function<void(std::size_t)> spawn,
              psx::stream::OverflowPolicy policy,
              std::size_t ringBytes,
              bool cancellable,
              bool failFast,
              int maxRetries,
              std::chrono::milliseconds retryBase,
              std::chrono::milliseconds retryCap)
        : reactor_(reactor), workers_(workers), timeoutSec_(timeoutSec), sink_(sink),
          concurrency_(concurrency == 0 ? workers.size() : concurrency), spawn_(std::move(spawn)),
          // Block (or a 0 ring) captures everything; a drop policy bounds it.
          outputCap_(policy == psx::stream::OverflowPolicy::Block ? 0 : ringBytes), outputPolicy_(policy),
          cancellable_(cancellable), failFast_(failFast), maxRetries_(maxRetries), retryBase_(retryBase),
          retryCap_(retryCap), rng_(std::random_device{}()) {}

    // The reactor is cached and reused across runs; a timer left in it after
    // this WorkerRun is destroyed would fire into freed memory. Cancel any
    // that did not fire (cancel of an already-fired/absent timer is a no-op).
    ~WorkerRun() {
        if (deadlineTimer_ != 0) {
            reactor_.cancel(deadlineTimer_);
        }
        if (drainTimer_ != 0) {
            reactor_.cancel(drainTimer_);
        }
        if (signalRegistered_) {
            // The reactor outlives this run; drop the handler so a later SIGINT
            // cannot dispatch into this freed WorkerRun.
            (void)reactor_.onSignal({});
        }
        for (auto& [index, timer] : retryTimers_) {
            reactor_.cancel(timer);
        }
    }

    bool anyTimedOut() const noexcept { return anyTimedOut_; }
    bool wasCancelled() const noexcept { return cancelled_; }
    bool wasAborted() const noexcept { return aborted_; }

    void run() {
        remaining_ = workers_.size();
        if (cancellable_) {
            // A SIGINT (Ctrl-C) cancels the run gracefully. onSignal is a no-op
            // (Unsupported) when the reactor has no SignalSource; cancellation is
            // then simply unavailable, never a crash.
            signalRegistered_ = reactor_.onSignal([this](psx::os::Signal) { onCancel(); }).ok();
        }
        if (timeoutSec_ > 0) {
            deadlineTimer_ = reactor_.after(std::chrono::seconds(timeoutSec_), [this] { onDeadline(); });
        }
        fillSlots();
        // Reactor::run() returns immediately if account() already called stop().
        if (auto ran = reactor_.run(); !ran.ok()) {
            fail("event loop failed", ran.error());
        }
    }

private:
    // Spawns pending workers until `concurrency` are in flight. Reentrancy-safe:
    // arm() can complete a fast-exiting worker synchronously (calling back into
    // fillSlots); the guard makes the nested call a no-op and the outer loop
    // refills the freed slot.
    void fillSlots() {
        if (filling_) {
            return;
        }
        filling_ = true;
        while (inFlight_ < concurrency_ && (!retryQueue_.empty() || nextToSpawn_ < workers_.size())) {
            std::size_t index;
            if (!retryQueue_.empty()) {
                index = retryQueue_.front();
                retryQueue_.erase(retryQueue_.begin());
            } else {
                index = nextToSpawn_++;
            }
            Worker& worker = workers_[index];
            worker.spawned = true;
            spawn_(index);
            if (worker.process.running()) {
                ++inFlight_;
                arm(worker); // may call onExit() synchronously for a fast child
            } else {
                account(worker); // failed to start: complete now, no slot consumed
            }
        }
        filling_ = false;
    }

    // O(1) completion tracking: each worker is subtracted from `remaining_`
    // exactly once, when it first becomes complete; the loop stops at zero.
    void account(Worker& worker) {
        if (worker.counted || worker.awaitingRetry || !worker.complete()) {
            return;
        }
        // Fully finished one attempt: retry a transient transport failure (unless
        // the run is being cancelled) before counting it as done.
        // A fail-fast/cancel teardown must finalize this worker, never start a
        // fresh retry: `aborted_`/`cancelled_` both suppress the reschedule.
        if (!cancelled_ && !aborted_ && worker.attempt < maxRetries_ && retryable(worker)) {
            scheduleRetry(worker);
            return;
        }
        worker.counted = true;
        if (--remaining_ == 0) {
            reactor_.stop();
            return;
        }
        // --fail-fast: the first final failure aborts every remaining worker.
        if (failFast_ && !aborted_ && !cancelled_ && isFailure(worker)) {
            aborted_ = true;
            Logger::getInstance().log(LogLevel::ERROR, worker.context,
                                      "Stage failed with --fail-fast; aborting the rest of the run");
            teardown(StopReason::FailFast);
        }
    }

    static bool isFailure(const Worker& worker) noexcept {
        return worker.timedOut || !worker.failureMessage.empty() || worker.exitCode() != 0;
    }

    // Only ssh's own transient transport failures (exit 255 + connect/unreachable
    // on stderr) are retried. A command's own non-zero exit, an auth/host-key
    // failure, a timeout or a start failure are permanent.
    bool retryable(const Worker& worker) const {
        return !worker.timedOut && worker.failureMessage.empty() && worker.exitCode() == 255 &&
               isRetryableSshFailure(worker.stderrData);
    }

    void scheduleRetry(Worker& worker) {
        worker.awaitingRetry = true;
        ++worker.attempt;
        const double draw = std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
        const auto delay = psx::runtime::backoffDelay(worker.attempt, retryBase_, retryCap_, draw);
        const std::size_t index = static_cast<std::size_t>(&worker - workers_.data());
        Logger::getInstance().log(LogLevel::INFO, worker.context,
                                  "Transient failure; retry " + std::to_string(worker.attempt) + "/" +
                                      std::to_string(maxRetries_) + " in " + std::to_string(delay.count()) + " ms");
        retryTimers_[index] = reactor_.after(delay, [this, index] { onRetry(index); });
    }

    void onRetry(std::size_t index) {
        retryTimers_.erase(index);
        Worker& worker = workers_[index];
        // A teardown may have finalized this worker already. The reactor collects
        // all due timers into one batch before running any, and cancel() cannot
        // pull a handler out of that batch — so a deadline co-scheduled with this
        // retry runs first, teardown counts the worker, and cancelling our timer
        // is a no-op. Resurrecting a counted worker would double-count it (hang)
        // or leave live I/O handlers in the cached reactor (use-after-free). If it
        // is already counted, the retry is void.
        if (worker.counted) {
            return;
        }
        worker.awaitingRetry = false;
        if (cancelled_) {
            // Cancelled while backing off: this attempt is the last, as cancelled.
            worker.cancelled = true;
            account(worker); // still exited + closed from the failed attempt
            return;
        }
        worker.resetForRetry();
        retryQueue_.push_back(index);
        fillSlots();
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
        if (sink_ != nullptr) {
            sink_->stageStarted(worker.clientId);
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
            // The child can exit before we watch it (fast commands): the exit
            // source then reports "no such process". That is not a failure —
            // the child is already reapable, so handle its exit now. Any pipe
            // output it left is still drained by the readable handlers above.
            if (watched.error().cls == psx::ErrorClass::NoSuchProcess) {
                onExit(worker);
            } else {
                fail("watching child failed for " + worker.clientId, watched.error());
            }
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
            if (sink_ != nullptr) {
                framerFor(worker, isStdout)
                    .push(std::span<const char>(buffer, chunk.value()),
                          [&](std::string_view lineText, bool) { emitLine(worker, isStdout, lineText); });
            }
        }

        if (outputCap_ != 0 && data.size() > outputCap_) {
            const std::size_t over = data.size() - outputCap_;
            if (outputPolicy_ == psx::stream::OverflowPolicy::Spool) {
                // Spill the oldest bytes to disk (no loss) and keep the newest
                // ring in memory. A spill failure degrades to drop-oldest.
                psx::stream::SpoolBuffer& spool = isStdout ? worker.stdoutSpool : worker.stderrSpool;
                if (!spool.append(std::string_view(data.data(), over))) {
                    worker.droppedBytes += over;
                }
                data.erase(0, over);
            } else if (outputPolicy_ == psx::stream::OverflowPolicy::DropOldest) {
                data.erase(0, over); // keep the newest ring
                worker.droppedBytes += over;
            } else {
                data.resize(outputCap_); // DropNewest: keep the oldest ring
                worker.droppedBytes += over;
            }
        }

        if (data.size() > before && Logger::getInstance().enabled(LogLevel::DEBUG)) {
            Logger::getInstance().log(LogLevel::DEBUG, worker.context,
                                      "Read " + std::to_string(data.size() - before) +
                                          (isStdout ? " bytes from child stdout" : " bytes from child stderr"));
        }
        if (endOfStream) {
            if (sink_ != nullptr) {
                framerFor(worker, isStdout).flush([&](std::string_view lineText, bool) {
                    emitLine(worker, isStdout, lineText);
                });
            }
            closeStream(handle, token, closed);
            account(worker);
        }
    }

    psx::stream::LineFramer& framerFor(Worker& worker, bool isStdout) noexcept {
        return isStdout ? worker.stdoutFramer : worker.stderrFramer;
    }

    void emitLine(const Worker& worker, bool isStdout, std::string_view lineText) {
        sink_->line(worker.clientId, isStdout ? psx::sink::Channel::Stdout : psx::sink::Channel::Stderr, lineText);
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
        account(worker);
    }

    void onExit(Worker& worker) {
        auto status = worker.process.tryWait();
        if (status.ok() && status.value().has_value()) {
            worker.status = *status.value();
        } else if (auto blocking = worker.process.wait(); blocking.ok()) {
            worker.status = blocking.value(); // notified, so this cannot block for long
        }
        worker.exited = true;
        if (inFlight_ > 0) {
            --inFlight_; // this ssh process is gone; its slot is free
        }
        if (Logger::getInstance().enabled(LogLevel::DEBUG)) {
            Logger::getInstance().log(LogLevel::DEBUG, worker.context, "Child process reaped");
        }
        account(worker);
        fillSlots(); // spawn the next pending worker into the freed slot
    }

    enum class StopReason { Deadline, Cancel, FailFast };

    void onDeadline() {
        deadlineTimer_ = 0; // fired: nothing to cancel later
        teardown(StopReason::Deadline);
    }

    // SIGINT/Ctrl-C: stop the run promptly and report it as cancelled (exit 130),
    // distinct from a deadline. Idempotent: a second signal while draining is a
    // no-op. A cancel sends SIGTERM first (the drain grace escalates to SIGKILL),
    // so a well-behaved child gets to clean up.
    void onCancel() {
        if (cancelled_) {
            return;
        }
        Logger::getInstance().log(LogLevel::INFO, workers_.empty() ? LogContext{} : workers_.front().context,
                                  "Cancellation requested (SIGINT); draining workers");
        teardown(StopReason::Cancel);
    }

    void teardown(StopReason reason) {
        const bool deadline = reason == StopReason::Deadline;
        if (reason == StopReason::Cancel) {
            cancelled_ = true;
        } else if (deadline) {
            anyTimedOut_ = true;
        } // FailFast: aborted_ is already set by the caller
        // Abandon pending backoff timers; those workers end on this attempt.
        for (auto& [index, timer] : retryTimers_) {
            reactor_.cancel(timer);
        }
        retryTimers_.clear();
        // Workers already reset and queued for a respawn end now, too.
        for (std::size_t index : retryQueue_) {
            Worker& queued = workers_[index];
            mark(queued, reason);
            queued.stdoutClosed = queued.stderrClosed = queued.stdinClosed = queued.exited = true;
            account(queued);
        }
        retryQueue_.clear();
        // Pending workers never got a slot; they end without running.
        while (nextToSpawn_ < workers_.size()) {
            Worker& pending = workers_[nextToSpawn_++];
            mark(pending, reason);
            pending.stdoutClosed = pending.stderrClosed = pending.stdinClosed = pending.exited = true;
            account(pending);
        }
        for (auto& worker : workers_) {
            if (worker.counted) {
                continue;
            }
            if (worker.awaitingRetry) {
                // Was waiting on a (now-cancelled) backoff timer; already exited
                // and drained from its failed attempt, so just finalize it.
                worker.awaitingRetry = false;
                mark(worker, reason);
                account(worker);
                continue;
            }
            if (worker.complete()) {
                continue;
            }
            mark(worker, reason);
            // Signal the whole process group so a grandchild that stayed in it
            // dies too. This is valid while the group is non-empty (its id is
            // not recycled until the last member exits); a descendant that left
            // via setsid empties the group and is unreachable anyway, which the
            // drain grace below bounds. Signalling after the leader is reaped is
            // deliberate for the grandchild case (regression-tested). Deadline
            // uses SIGKILL; cancel uses SIGTERM and lets the grace escalate.
            (void)worker.process.signal(deadline ? psx::os::StopSignal::Kill : psx::os::StopSignal::Graceful);
            Logger::getInstance().log(LogLevel::ERROR, worker.context,
                                      deadline ? "Command timed out; sent SIGKILL to process group"
                                      : reason == StopReason::Cancel
                                          ? "Run cancelled; sent SIGTERM to process group"
                                          : "Run aborted (--fail-fast); sent SIGTERM to process group");
        }
        if (drainTimer_ == 0) {
            drainTimer_ = reactor_.after(kDrainGrace, [this] { onDrainExpired(); });
        }
    }

    void mark(Worker& worker, StopReason reason) {
        switch (reason) {
            case StopReason::Cancel:
                worker.cancelled = true;
                break;
            case StopReason::FailFast:
                worker.aborted = true;
                break;
            case StopReason::Deadline:
                worker.timedOut = true;
                break;
        }
    }

    void onDrainExpired() {
        drainTimer_ = 0; // fired
        for (auto& worker : workers_) {
            if (worker.complete()) {
                continue;
            }
            // Something outside the process group still holds the pipes; stop waiting for it.
            closeStream(worker.stdoutReader, worker.stdoutToken, worker.stdoutClosed);
            closeStream(worker.stderrReader, worker.stderrToken, worker.stderrClosed);
            closeStream(worker.stdinWriter, worker.stdinToken, worker.stdinClosed);
            if (!worker.exited) {
                // A cancel sent SIGTERM; anything still alive after the grace is
                // SIGKILLed now so the reap below cannot block.
                (void)worker.process.signal(psx::os::StopSignal::Kill);
                (void)reactor_.unwatchChild(worker.process.id());
                onExit(worker); // now dying: the wait returns promptly
            }
            Logger::getInstance().log(LogLevel::ERROR, worker.context,
                                      "Pipes still open after the SIGKILL grace period; abandoning remaining output");
            account(worker);
        }
    }

    Reactor& reactor_;
    std::vector<Worker>& workers_;
    int timeoutSec_;
    psx::sink::Sink* sink_ = nullptr;
    std::size_t concurrency_ = 1;
    std::function<void(std::size_t)> spawn_;
    std::size_t remaining_ = 0;
    std::size_t nextToSpawn_ = 0;
    std::size_t inFlight_ = 0;
    std::size_t outputCap_ = 0;
    psx::stream::OverflowPolicy outputPolicy_ = psx::stream::OverflowPolicy::Block;
    bool filling_ = false;
    psx::runtime::TimerId deadlineTimer_ = 0;
    psx::runtime::TimerId drainTimer_ = 0;
    bool anyTimedOut_ = false;
    bool cancellable_ = false;
    bool cancelled_ = false;
    bool signalRegistered_ = false;
    bool failFast_ = false;
    bool aborted_ = false;
    int maxRetries_ = 0;
    std::chrono::milliseconds retryBase_{200};
    std::chrono::milliseconds retryCap_{30000};
    std::mt19937 rng_;
    std::unordered_map<std::size_t, psx::runtime::TimerId> retryTimers_;
    std::vector<std::size_t> retryQueue_;
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
        // Same shape as the v0.1.0 child-side message, so the REPL's "command
        // execution failed" mapping keeps working.
        worker.failToStart(
            "execvp failed for " +
            (worker.clientId == "-" ? "'" + spec.program + "'" : "ssh client '" + worker.clientId + "'") + ": " +
            process.error().message() + "\n");
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

psx::runtime::Reactor& ProcessManager::reactor(bool withSignals) {
    if (!reactor_) {
        Reactor::Options options;
        options.backend = Reactor::backendFromEnvironment();
        if (withSignals) {
            // Ctrl-C (and SIGTERM) become reactor events instead of killing the
            // controller, so an in-flight run can drain and report exit 130.
            options.signals = {psx::os::Signal::Interrupt, psx::os::Signal::Terminate};
        }
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

    const bool feedInput = !input.empty();
    auto spawnLocal = [&](std::size_t) {
        SpawnSpec spec;
        spec.program = args.front(); // PATH lookup when there is no '/', like execvp
        spec.argv = args;
        spec.limits.cpuSeconds = kLocalCpuSeconds;
        spec.limits.addressSpaceBytes = kLocalAddressSpaceBytes;
        Logger::getInstance().log(LogLevel::DEBUG, context, "Creating IPC pipes");
        spawnWorker(worker, std::move(spec), feedInput);
        if (worker.process.running()) {
            Logger::getInstance().log(LogLevel::INFO,
                                      LogContext{.pid = worker.process.id(),
                                                 .sessionId = context.sessionId,
                                                 .clientId = context.clientId,
                                                 .command = context.command},
                                      "Child process created");
        }
    };

    WorkerRun run(reactor(), workers, timeoutSec, nullptr, 1, spawnLocal, psx::stream::OverflowPolicy::Block, 0,
                  /*cancellable=*/false, /*failFast=*/false, /*maxRetries=*/0, std::chrono::milliseconds{200},
                  std::chrono::milliseconds{30000});
    run.run();

    Logger::getInstance().log(worker.exitCode() == 0 ? LogLevel::INFO : LogLevel::ERROR, context,
                              "Child exited with status " + std::to_string(worker.exitCode()) +
                                  (worker.timedOut ? " (timed out)" : ""));
    return Result{worker.exitCode(), std::move(worker.stdoutData), std::move(worker.stderrData),
                  worker.timedOut,   /*cancelled=*/false,          {}};
}

ProcessManager::Result ProcessManager::executeRemote(const std::vector<ClientEntry>& clients,
                                                     const std::string& remoteCommand,
                                                     const LogContext& context,
                                                     const RemoteRunOptions& options) {
    if (clients.empty()) {
        throw std::runtime_error("no clients configured for remote execution");
    }

    std::vector<Worker> workers(clients.size());
    for (std::size_t index = 0; index < clients.size(); ++index) {
        Worker& worker = workers[index];
        worker.clientId = clients[index].clientId();
        worker.context = LogContext{.pid = context.pid,
                                    .sessionId = context.sessionId,
                                    .clientId = clients[index].clientId(),
                                    .command = "ssh " + clients[index].clientId() + " " + remoteCommand,
                                    .runId = context.runId,
                                    .stageId = "s" + std::to_string(index)};
    }

    SshTransport transport(SshTransport::Options{.controlPath = options.controlPath});
    auto spawnRemote = [&](std::size_t index) {
        const ClientEntry& client = clients[index];
        Worker& worker = workers[index];

        // `prepared` owns the secret hand-off pipe; its reader is borrowed by
        // spec.extraHandles, so it must outlive spawnWorker (it does — it lives
        // to the end of this lambda). A prepare() error is a fatal resource
        // failure; a per-host validation failure is reported on `prepared`.
        SshTransport::Prepared prepared;
        if (auto ready = transport.prepare(client, remoteCommand, prepared); !ready.ok()) {
            fail("ssh transport prepare failed for " + worker.clientId, ready.error());
        }
        if (!prepared.failure.empty()) {
            worker.failToStart(prepared.failure);
            return;
        }

        spawnWorker(worker, std::move(prepared.spec), false);
        if (worker.process.running()) {
            LogContext created = worker.context;
            created.pid = worker.process.id();
            Logger::getInstance().log(LogLevel::INFO, created, "Remote SSH worker created");
        }
        // prepared (and the password pipe) closes here: only the child holds the secret now.
    };

    WorkerRun run(reactor(/*withSignals=*/options.cancellable), workers, options.timeoutSec, options.sink,
                  options.concurrency, spawnRemote, options.policy, options.ringBytes, options.cancellable,
                  options.failFast, options.maxRetries, options.retryBaseDelay, options.retryMaxDelay);
    run.run();

    std::vector<ClientResult> clientResults;
    clientResults.reserve(workers.size());
    int overallExitCode = 0;
    for (auto& worker : workers) {
        const int exitCode = worker.exitCode();
        if (worker.aborted) {
            // Killed because a sibling failed: a generic failure that must not
            // overwrite the real failing exit code with the kill's -1.
            if (overallExitCode == 0) {
                overallExitCode = 1;
            }
        } else if (exitCode != 0 || worker.timedOut) {
            overallExitCode = exitCode == 0 ? 1 : exitCode;
        }
        // Spool policy: the older bytes live on disk; prepend them to the
        // in-memory tail to reconstruct the full output (no-op otherwise, since
        // the spool is empty unless the Spool policy spilled).
        std::string stdoutFull = worker.stdoutSpool.empty() ? std::move(worker.stdoutData)
                                                            : worker.stdoutSpool.readAll() + worker.stdoutData;
        std::string stderrFull = worker.stderrSpool.empty() ? std::move(worker.stderrData)
                                                            : worker.stderrSpool.readAll() + worker.stderrData;
        clientResults.push_back(ClientResult{worker.clientId,
                                             exitCode,
                                             std::move(stdoutFull),
                                             std::move(stderrFull),
                                             {},
                                             worker.timedOut,
                                             worker.droppedBytes,
                                             worker.cancelled,
                                             worker.attempt + 1,
                                             worker.aborted});
    }

    for (std::size_t index = 0; index < clientResults.size(); ++index) {
        clientResults[index].errorMessage = classifyRemoteError(clientResults[index]);
        if (!clientResults[index].errorMessage.empty()) {
            Logger::getInstance().log(LogLevel::ERROR, workers[index].context, clientResults[index].errorMessage);
        }
    }

    const bool cancelled = run.wasCancelled();
    if (cancelled) {
        overallExitCode = kExitCancelled; // 130: the CLI surfaces this verbatim
    }

    if (options.sink != nullptr) {
        std::size_t succeeded = 0;
        std::uint64_t totalDropped = 0;
        for (const auto& clientResult : clientResults) {
            const bool ok = clientResult.exitCode == 0 && !clientResult.timedOut && clientResult.errorMessage.empty();
            succeeded += ok ? 1 : 0;
            totalDropped += clientResult.droppedBytes;
            options.sink->stageFinished(clientResult.clientId,
                                        psx::sink::StageResult{clientResult.exitCode, clientResult.timedOut,
                                                               clientResult.errorMessage, clientResult.droppedBytes});
        }
        options.sink->runFinished(psx::sink::RunSummary{clientResults.size(), succeeded,
                                                        clientResults.size() - succeeded, totalDropped, cancelled});
    }

    return Result{overallExitCode,
                  formatClientResults(clientResults, true),
                  formatClientResults(clientResults, false),
                  run.anyTimedOut(),
                  cancelled,
                  std::move(clientResults)};
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
    if (clientResult.cancelled) {
        return "ERROR: cancelled";
    }
    if (clientResult.aborted) {
        return "ERROR: aborted (fail-fast)";
    }
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

    // ssh reports its own failures (connect/auth/host-key) with exit code 255;
    // any other exit code is the remote command's own result, so its stderr —
    // which may itself contain "permission denied" etc. — must not be sniffed.
    if (clientResult.exitCode == 255) {
        if (const auto sshFailure = classifySshFailure(stderrText); sshFailure.has_value()) {
            return "ERROR: " + *sshFailure;
        }
    }

    if (clientResult.exitCode != 0) {
        return "ERROR: command failed with exit code " + std::to_string(clientResult.exitCode);
    }

    return {};
}
