// Factory and the portable SIGCHLD-driven ChildExitSource.

#include "psx/os/child_exit.hpp"
#include "psx/os/backend.hpp"
#include "psx/os/pipe.hpp"

#include "posix_error.hpp"
#include "sources.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <mutex>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

namespace psx::os {

namespace posix {

bool childHasExited(ProcessId pid, bool& exists) noexcept {
    siginfo_t info{};
    info.si_pid = 0;
    int rc = 0;
    do {
        rc = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
    } while (rc == -1 && errno == EINTR);
    if (rc == -1) {
        exists = false;
        return false;
    }
    exists = true;
    return info.si_pid == static_cast<pid_t>(pid);
}

} // namespace posix

namespace {

// One process-wide SIGCHLD handler fans out to every live SignalDriven source
// through its own self-pipe (async-signal-safe: atomics and write() only).
constexpr int kMaxSources = 16;
std::atomic<int> gWriters[kMaxSources];
std::mutex gRegistryMutex;
int gSourceCount = 0;
struct sigaction gPreviousAction{};

void onSigChld(int) {
    const int savedErrno = errno;
    const char byte = 1;
    for (auto& writer : gWriters) {
        const int fd = writer.load(std::memory_order_relaxed);
        if (fd >= 0) {
            (void)::write(fd, &byte, 1);
        }
    }
    errno = savedErrno;
}

class SignalDrivenChildExitSource final : public ChildExitSource {
public:
    explicit SignalDrivenChildExitSource(Pipe pipe) : pipe_(std::move(pipe)) {}

    ~SignalDrivenChildExitSource() override { unregister(); }

    Result<void> registerHandler() {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        for (int i = 0; i < kMaxSources; ++i) {
            if (gWriters[i].load(std::memory_order_relaxed) < 0) {
                slot_ = i;
                break;
            }
        }
        if (slot_ < 0) {
            return Error{ErrorClass::TooManyHandles, EMFILE, "child_exit.create"};
        }
        if (gSourceCount == 0) {
            struct sigaction action{};
            action.sa_handler = onSigChld;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
            if (::sigaction(SIGCHLD, &action, &gPreviousAction) == -1) {
                return posix::fromErrno("sigaction(SIGCHLD)", errno);
            }
        }
        ++gSourceCount;
        gWriters[slot_].store(static_cast<int>(Backend::native(pipe_.writer)), std::memory_order_release);
        return {};
    }

    ChildExitMode mode() const noexcept override { return ChildExitMode::SignalDriven; }
    const Handle& handle() const noexcept override { return pipe_.reader; }
    std::size_t size() const noexcept override { return watched_.size(); }

    Result<void> watch(ProcessId pid) override {
        if (watched_.count(pid) != 0) {
            return Error{ErrorClass::InvalidArgument, EINVAL, "child_exit.watch"};
        }
        bool exists = false;
        const bool exited = posix::childHasExited(pid, exists);
        if (!exists) {
            return Error{ErrorClass::NoSuchProcess, ECHILD, "child_exit.watch"};
        }
        watched_.insert(pid);
        if (exited) {
            const char byte = 1; // already a zombie: make the handle readable now
            (void)::write(static_cast<int>(Backend::native(pipe_.writer)), &byte, 1);
        }
        return {};
    }

    Result<void> unwatch(ProcessId pid) override {
        if (watched_.erase(pid) == 0) {
            return Error{ErrorClass::NotFound, ENOENT, "child_exit.unwatch"};
        }
        return {};
    }

    Result<std::vector<ProcessId>> drain() override {
        char buffer[64];
        while (::read(static_cast<int>(Backend::native(pipe_.reader)), buffer, sizeof(buffer)) > 0) {}
        std::vector<ProcessId> exited;
        for (auto it = watched_.begin(); it != watched_.end();) {
            bool exists = false;
            if (posix::childHasExited(*it, exists) || !exists) {
                exited.push_back(*it); // gone entirely counts as exited too
                it = watched_.erase(it);
            } else {
                ++it;
            }
        }
        return exited;
    }

private:
    void unregister() noexcept {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        if (slot_ >= 0) {
            gWriters[slot_].store(-1, std::memory_order_release);
            slot_ = -1;
            if (--gSourceCount == 0) {
                (void)::sigaction(SIGCHLD, &gPreviousAction, nullptr);
            }
        }
    }

    Pipe pipe_;
    int slot_ = -1;
    std::unordered_set<ProcessId> watched_;
};

struct RegistryInit {
    RegistryInit() {
        for (auto& writer : gWriters) {
            writer.store(-1, std::memory_order_relaxed);
        }
    }
} gRegistryInit;

} // namespace

namespace posix {

Result<std::unique_ptr<ChildExitSource>> createSignalDrivenChildExitSource() {
    auto pipe = Pipe::create();
    if (!pipe.ok()) {
        return pipe.error();
    }
    PSX_TRY(pipe.value().reader.setNonBlocking(true));
    PSX_TRY(pipe.value().writer.setNonBlocking(true));
    auto source = std::make_unique<SignalDrivenChildExitSource>(std::move(pipe.value()));
    PSX_TRY(source->registerHandler());
    return std::unique_ptr<ChildExitSource>(std::move(source));
}

} // namespace posix

bool ChildExitSource::available(ChildExitMode mode) noexcept {
    switch (mode) {
        case ChildExitMode::Auto:
        case ChildExitMode::SignalDriven:
            return true;
        case ChildExitMode::Native:
            return posix::nativeChildExitAvailable();
    }
    return false;
}

Result<std::unique_ptr<ChildExitSource>> ChildExitSource::create(ChildExitMode mode) {
    if (mode == ChildExitMode::Auto) {
        mode = posix::nativeChildExitAvailable() ? ChildExitMode::Native : ChildExitMode::SignalDriven;
    }
    if (mode == ChildExitMode::Native) {
        if (!posix::nativeChildExitAvailable()) {
            return Error{ErrorClass::Unsupported, ENOTSUP, "child_exit.create"};
        }
        return posix::createNativeChildExitSource();
    }
    return posix::createSignalDrivenChildExitSource();
}

} // namespace psx::os
