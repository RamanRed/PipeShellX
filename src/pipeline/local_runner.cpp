#include "psx/pipeline/local_runner.hpp"

#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"

#include <array>
#include <span>
#include <utility>

namespace psx::pipeline {

using psx::os::ExitStatus;
using psx::os::Handle;
using psx::os::Interest;
using psx::os::Pipe;
using psx::os::Process;
using psx::os::Readiness;
using psx::os::SpawnSpec;

namespace {
// Bound one output dispatch so a continuously readable child cannot starve
// child-exit, transport, or timer handlers on the reactor thread. The watch is
// temporarily disabled around the bounded drain and re-armed afterwards;
// every poller backend reports readiness that became pending while disabled.
constexpr std::size_t kFinalReadQuantumBytes = 256U * 1024U;

// Conventional shell exit code: the exit code for a normal exit, else 128+signal.
int toExitCode(const ExitStatus& status) {
    return status.kind == ExitStatus::Kind::Exited ? status.code : 128 + status.code;
}
} // namespace

LocalRunner::LocalRunner(psx::runtime::Reactor& reactor, OnOutput onOutput)
    : reactor_(reactor), onOutput_(std::move(onOutput)) {}

LocalRunner::~LocalRunner() {
    if (finalToken_ != 0) {
        (void)reactor_.unwatch(finalToken_);
    }
    if (stdinToken_ != 0) {
        (void)reactor_.unwatch(stdinToken_);
    }
    for (Child& child : children_) {
        if (!child.exited) {
            (void)reactor_.unwatchChild(child.process.id());
        }
    }
}

psx::Result<void>
LocalRunner::run(const std::vector<Stage>& stages, std::function<void(Outcome)> onComplete, bool externalStdin) {
    if (stages.empty()) {
        return psx::Error{psx::ErrorClass::InvalidArgument, 0, "empty pipeline"};
    }
    onComplete_ = std::move(onComplete);
    const std::size_t n = stages.size();

    // A pipe between each consecutive pair of stages, plus the final stdout pipe.
    std::vector<Pipe> links;
    links.reserve(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        auto pipe = Pipe::create();
        if (!pipe.ok()) {
            return pipe.error();
        }
        links.push_back(std::move(pipe.value()));
    }
    auto finalPipe = Pipe::create();
    if (!finalPipe.ok()) {
        return finalPipe.error();
    }
    Pipe stdinPipe;
    if (externalStdin) {
        auto created = Pipe::create();
        if (!created.ok()) {
            return created.error();
        }
        stdinPipe = std::move(created.value());
        (void)stdinPipe.writer.setNonBlocking(true); // written without a watch until it backs up
    }

    // Spawn each stage with its stdio wired to the neighbouring pipes.
    children_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        SpawnSpec spec;
        spec.program = stages[i].argv.front();
        spec.argv = stages[i].argv;
        spec.in = (i != 0) ? SpawnSpec::Stdio::from(links[i - 1].reader)
                           : (externalStdin ? SpawnSpec::Stdio::from(stdinPipe.reader) : SpawnSpec::Stdio::null());
        spec.out =
            (i + 1 == n) ? SpawnSpec::Stdio::from(finalPipe.value().writer) : SpawnSpec::Stdio::from(links[i].writer);
        // spec.err stays Inherit: stage diagnostics go to the terminal.
        auto process = Process::spawn(spec);
        if (!process.ok()) {
            return process.error(); // ~LocalRunner kills any stages already spawned
        }
        children_.push_back(Child{.process = std::move(process.value())});
    }

    // Keep only the final read end; every other pipe end closes as `links` and
    // `finalPipe`'s writer go out of scope, so EOF propagates down the chain.
    finalReader_ = std::move(finalPipe.value().reader);
    if (externalStdin) {
        stdinWriter_ = std::move(stdinPipe.writer); // stdinPipe.reader closes here (child holds it)
        stdinOpen_ = true;
        if (auto watched = reactor_.watch(stdinWriter_, Interest::None, [this](Readiness) { onStdinWritable(); });
            watched.ok()) {
            stdinToken_ = watched.value();
        }
    }

    if (auto watched = reactor_.watch(finalReader_, Interest::Readable, [this](Readiness) { onFinalReadable(); });
        watched.ok()) {
        finalToken_ = watched.value();
    } else {
        return watched.error();
    }

    for (std::size_t i = 0; i < n; ++i) {
        const psx::os::ProcessId pid = children_[i].process.id();
        if (auto watched = reactor_.watchChild(pid, [this, i](psx::os::ProcessId) { onChildExit(i); }); !watched.ok()) {
            // Already exited before we watched it (a fast stage): reap it now.
            if (watched.error().cls == psx::ErrorClass::NoSuchProcess) {
                onChildExit(i);
            } else {
                return watched.error();
            }
        }
    }
    return {};
}

void LocalRunner::onFinalReadable() {
    std::array<char, 16 * 1024> buffer{};
    const psx::runtime::Token token = finalToken_;
    if (token == 0 || finalClosed_) {
        return;
    }

    // Edge-triggered backends normally require draining to WouldBlock. Disable
    // the watch first so a bounded drain can yield safely, then use the
    // None -> Readable transition to surface any readiness still pending.
    const bool disarmed = reactor_.modify(token, Interest::None).ok();
    std::size_t bytesRead = 0;
    while (true) {
        auto got = psx::os::read(finalReader_, std::span<char>(buffer.data(), buffer.size()));
        if (got.ok()) {
            if (got.value() == 0) { // EOF: the last stage closed its stdout
                finalClosed_ = true;
                if (finalToken_ != 0) {
                    (void)reactor_.unwatch(finalToken_);
                    finalToken_ = 0;
                }
                finishIfDone();
                return;
            }
            if (onOutput_) {
                onOutput_(std::string_view(buffer.data(), got.value()));
            }
            // The output callback may cancel the runner, which unwatches and
            // closes this stream. Never re-arm or read the cancelled handle.
            if (finalToken_ != token || finalClosed_ || done_) {
                return;
            }
            bytesRead += got.value();
            if (disarmed && bytesRead >= kFinalReadQuantumBytes) {
                break;
            }
            continue;
        }
        if (got.error().cls == psx::ErrorClass::WouldBlock) {
            break; // nothing more for now
        }
        // A read error also ends the stream.
        finalClosed_ = true;
        if (finalToken_ != 0) {
            (void)reactor_.unwatch(finalToken_);
            finalToken_ = 0;
        }
        finishIfDone();
        return;
    }

    if (disarmed && finalToken_ == token && !finalClosed_ && !done_) {
        // epoll MOD, kqueue filter re-add, and poll's rebuilt fd set all expose
        // data/EOF that arrived while the registration was Interest::None.
        if (!reactor_.modify(token, Interest::Readable).ok()) {
            finalClosed_ = true;
            (void)reactor_.unwatch(token);
            finalToken_ = 0;
            finishIfDone();
        }
    }
}

void LocalRunner::onChildExit(std::size_t index) {
    Child& child = children_[index];
    if (child.exited) {
        return;
    }
    ExitStatus status{ExitStatus::Kind::Exited, 0};
    if (auto reaped = child.process.tryWait(); reaped.ok() && reaped.value().has_value()) {
        status = *reaped.value();
    } else if (auto blocking = child.process.wait(); blocking.ok()) {
        status = blocking.value();
    }
    child.exitCode = toExitCode(status);
    child.exited = true;
    ++exitedCount_;
    finishIfDone();
}

void LocalRunner::writeStdin(std::string_view bytes) {
    if (!stdinOpen_) {
        return;
    }
    if (!bytes.empty()) {
        stdinBuffer_.append(bytes);
    }
    drainStdin();
}
void LocalRunner::closeStdin() {
    if (!stdinOpen_) {
        return;
    }
    stdinEndPending_ = true;
    drainStdin();
}
void LocalRunner::cancel() {
    if (done_) {
        return;
    }
    if (stdinToken_ != 0) {
        (void)reactor_.unwatch(stdinToken_);
        stdinToken_ = 0;
    }
    stdinWriter_.close();
    stdinOpen_ = false;
    stdinBuffer_.clear();

    // Signal before closing the output reader. Closing it first races SIGPIPE
    // against our explicit fence and makes the reported status nondeterministic
    // (141 or 137 depending on scheduling).
    for (Child& child : children_) {
        if (child.exited) {
            continue;
        }
        (void)reactor_.unwatchChild(child.process.id());
        (void)child.process.signal(psx::os::StopSignal::Kill);
        ExitStatus status{ExitStatus::Kind::Signaled, 9};
        if (auto reaped = child.process.wait(); reaped.ok()) {
            status = reaped.value();
        }
        child.exitCode = toExitCode(status);
        child.exited = true;
        ++exitedCount_;
    }
    if (finalToken_ != 0) {
        (void)reactor_.unwatch(finalToken_);
        finalToken_ = 0;
    }
    finalReader_.close();
    finalClosed_ = true;
    finishIfDone(); // may destroy this; touch nothing afterwards
}
void LocalRunner::drainStdin() {
    while (stdinOpen_ && !stdinBuffer_.empty()) {
        auto wrote = psx::os::write(stdinWriter_, std::span<const char>(stdinBuffer_.data(), stdinBuffer_.size()));
        if (wrote.ok() && wrote.value() > 0) {
            stdinBuffer_.erase(0, wrote.value());
        } else if (wrote.ok() || wrote.error().cls == psx::ErrorClass::WouldBlock) {
            break; // pipe full: resume on Writable
        } else {
            stdinBuffer_.clear(); // the stage closed its stdin (EPIPE)
            break;
        }
    }
    if (!stdinOpen_) {
        return;
    }
    if (stdinBuffer_.empty()) {
        if (stdinEndPending_) {
            if (stdinToken_ != 0) {
                (void)reactor_.unwatch(stdinToken_);
                stdinToken_ = 0;
            }
            stdinWriter_.close(); // EOF to the first stage
            stdinOpen_ = false;
        } else if (stdinToken_ != 0) {
            (void)reactor_.modify(stdinToken_, Interest::None);
        }
    } else if (stdinToken_ != 0) {
        (void)reactor_.modify(stdinToken_, Interest::Writable);
    }
}
void LocalRunner::onStdinWritable() {
    drainStdin();
}
void LocalRunner::finishIfDone() {
    if (done_ || !finalClosed_ || exitedCount_ != children_.size()) {
        return;
    }
    done_ = true;
    Outcome outcome;
    outcome.stageExitCodes.reserve(children_.size());
    for (const Child& child : children_) {
        outcome.stageExitCodes.push_back(child.exitCode);
        if (child.exitCode != 0) {
            outcome.exitCode = child.exitCode; // pipefail: rightmost non-zero wins
        }
    }
    if (onComplete_) {
        auto callback = std::move(onComplete_);
        callback(std::move(outcome)); // may destroy `this`; touch nothing after
    }
}

} // namespace psx::pipeline
