// Native ChildExitSource for Darwin/BSD: one kqueue, EVFILT_PROC NOTE_EXIT per
// watched child, no descriptor per child. A child that is already a zombie
// cannot be attached (ESRCH) and is reported through the pending list.

#include "psx/os/backend.hpp"
#include "psx/os/child_exit.hpp"

#include "posix_error.hpp"
#include "sources.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace psx::os::posix {

namespace {

constexpr std::uintptr_t kWakeIdent = 1;

class KqueueChildExitSource final : public ChildExitSource {
public:
    explicit KqueueChildExitSource(Handle kq) : kq_(std::move(kq)) {}

    Result<void> init() {
        struct kevent change;
        EV_SET(&change, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(fd(), &change, 1, nullptr, 0, nullptr) == -1) {
            return fromErrno("kevent(EVFILT_USER)", errno);
        }
        return {};
    }

    ChildExitMode mode() const noexcept override { return ChildExitMode::Native; }
    const Handle& handle() const noexcept override { return kq_; }
    std::size_t size() const noexcept override { return watched_.size() + pending_.size(); }

    Result<void> watch(ProcessId pid) override {
        if (watched_.count(pid) != 0 || pending_.count(pid) != 0) {
            return Error{ErrorClass::InvalidArgument, EINVAL, "child_exit.watch"};
        }
        struct kevent change;
        EV_SET(&change, static_cast<std::uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_CLEAR, NOTE_EXIT, 0, nullptr);
        if (::kevent(fd(), &change, 1, nullptr, 0, nullptr) == 0) {
            watched_.insert(pid);
            return {};
        }
        if (errno != ESRCH) {
            return fromErrno("kevent(EVFILT_PROC)", errno);
        }
        // Not attachable: either already a zombie (report it) or not ours.
        bool exists = false;
        if (!childHasExited(pid, exists) || !exists) {
            return Error{ErrorClass::NoSuchProcess, ESRCH, "child_exit.watch"};
        }
        pending_.insert(pid);
        struct kevent wake;
        EV_SET(&wake, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        (void)::kevent(fd(), &wake, 1, nullptr, 0, nullptr);
        return {};
    }

    Result<void> unwatch(ProcessId pid) override {
        if (pending_.erase(pid) != 0) {
            return {};
        }
        if (watched_.erase(pid) == 0) {
            return Error{ErrorClass::NotFound, ENOENT, "child_exit.unwatch"};
        }
        struct kevent change;
        EV_SET(&change, static_cast<std::uintptr_t>(pid), EVFILT_PROC, EV_DELETE, 0, 0, nullptr);
        (void)::kevent(fd(), &change, 1, nullptr, 0, nullptr); // ENOENT/ESRCH once the proc is gone
        return {};
    }

    Result<std::vector<ProcessId>> drain() override {
        std::vector<ProcessId> exited(pending_.begin(), pending_.end());
        pending_.clear();
        struct kevent events[64];
        const timespec zero{0, 0};
        while (true) {
            const int ready = ::kevent(fd(), nullptr, 0, events, 64, &zero);
            if (ready == -1) {
                if (errno == EINTR) {
                    continue;
                }
                return fromErrno("kevent", errno);
            }
            for (int i = 0; i < ready; ++i) {
                if (events[i].filter != EVFILT_PROC) {
                    continue; // the wake-up
                }
                const auto pid = static_cast<ProcessId>(events[i].ident);
                if (watched_.erase(pid) != 0) {
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
    int fd() const noexcept { return static_cast<int>(Backend::native(kq_)); }

    Handle kq_;
    std::unordered_set<ProcessId> watched_;
    std::unordered_set<ProcessId> pending_;
};

} // namespace

bool nativeChildExitAvailable() noexcept {
    return true;
}

Result<std::unique_ptr<ChildExitSource>> createNativeChildExitSource() {
    const int kq = ::kqueue();
    if (kq == -1) {
        return fromErrno("kqueue", errno);
    }
    (void)::fcntl(kq, F_SETFD, FD_CLOEXEC);
    auto source = std::make_unique<KqueueChildExitSource>(Backend::adopt(kq));
    PSX_TRY(source->init());
    return std::unique_ptr<ChildExitSource>(std::move(source));
}

} // namespace psx::os::posix
