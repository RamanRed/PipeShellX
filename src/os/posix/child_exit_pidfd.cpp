// Native ChildExitSource for Linux ≥ 5.3: one pidfd per watched child inside
// a private epoll instance; the epoll descriptor is the pollable handle.

#include "psx/os/backend.hpp"
#include "psx/os/child_exit.hpp"

#include "posix_error.hpp"
#include "sources.hpp"

#include <cerrno>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#if !defined(SYS_pidfd_open)
#define SYS_pidfd_open 434
#endif

namespace psx::os::posix {

namespace {

int pidfdOpen(ProcessId pid) noexcept {
    return static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
}

class PidfdChildExitSource final : public ChildExitSource {
public:
    explicit PidfdChildExitSource(Handle epoll) : epoll_(std::move(epoll)) {}

    ~PidfdChildExitSource() override {
        for (auto& [pid, fd] : watched_) {
            ::close(fd);
        }
    }

    ChildExitMode mode() const noexcept override { return ChildExitMode::Native; }
    const Handle& handle() const noexcept override { return epoll_; }
    std::size_t size() const noexcept override { return watched_.size(); }

    Result<void> watch(ProcessId pid) override {
        if (watched_.count(pid) != 0) {
            return Error{ErrorClass::InvalidArgument, EINVAL, "child_exit.watch"};
        }
        bool exists = false;
        (void)childHasExited(pid, exists);
        if (!exists) {
            return Error{ErrorClass::NoSuchProcess, ECHILD, "child_exit.watch"};
        }
        const int pidfd = pidfdOpen(pid);
        if (pidfd == -1) {
            return fromErrno("pidfd_open", errno);
        }
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = static_cast<std::uint64_t>(pid);
        if (::epoll_ctl(static_cast<int>(Backend::native(epoll_)), EPOLL_CTL_ADD, pidfd, &ev) == -1) {
            const int err = errno;
            ::close(pidfd);
            return fromErrno("epoll_ctl(ADD pidfd)", err);
        }
        watched_.emplace(pid, pidfd);
        return {};
    }

    Result<void> unwatch(ProcessId pid) override {
        auto it = watched_.find(pid);
        if (it == watched_.end()) {
            return Error{ErrorClass::NotFound, ENOENT, "child_exit.unwatch"};
        }
        ::close(it->second); // closing removes it from the epoll set
        watched_.erase(it);
        return {};
    }

    Result<std::vector<ProcessId>> drain() override {
        std::vector<ProcessId> exited;
        epoll_event events[64];
        while (true) {
            const int ready = ::epoll_wait(static_cast<int>(Backend::native(epoll_)), events, 64, 0);
            if (ready == -1) {
                if (errno == EINTR) {
                    continue;
                }
                return fromErrno("epoll_wait", errno);
            }
            for (int i = 0; i < ready; ++i) {
                const auto pid = static_cast<ProcessId>(events[i].data.u64);
                auto it = watched_.find(pid);
                if (it != watched_.end()) {
                    ::close(it->second);
                    watched_.erase(it);
                    exited.push_back(pid);
                }
            }
            if (ready < 64) {
                break;
            }
        }
        return exited;
    }

private:
    Handle epoll_;
    std::unordered_map<ProcessId, int> watched_;
};

bool probePidfd() noexcept {
    const int fd = pidfdOpen(static_cast<ProcessId>(::getpid()));
    if (fd == -1) {
        return false;
    }
    ::close(fd);
    return true;
}

} // namespace

bool nativeChildExitAvailable() noexcept {
    static const bool available = probePidfd();
    return available;
}

Result<std::unique_ptr<ChildExitSource>> createNativeChildExitSource() {
    const int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        return fromErrno("epoll_create1", errno);
    }
    return std::unique_ptr<ChildExitSource>(new PidfdChildExitSource(Backend::adopt(epfd)));
}

} // namespace psx::os::posix
