// SignalSource for Darwin/BSD: EVFILT_SIGNAL records every delivery attempt,
// even for SIG_IGN — so the subscribed signals are ignored while the source
// lives (no default action) and their events are read from the kqueue.

#include "psx/os/backend.hpp"
#include "psx/os/signal_source.hpp"

#include "posix_error.hpp"
#include "sources.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace psx::os::posix {

namespace {

class KqueueSignalSource final : public SignalSource {
public:
    explicit KqueueSignalSource(Handle kq) : kq_(std::move(kq)) {}

    ~KqueueSignalSource() override {
        for (auto& [number, previous] : saved_) {
            (void)::sigaction(number, &previous, nullptr);
        }
    }

    Result<void> subscribe(const std::vector<int>& numbers) {
        for (int number : numbers) {
            struct kevent change;
            EV_SET(&change, static_cast<std::uintptr_t>(number), EVFILT_SIGNAL, EV_ADD | EV_CLEAR, 0, 0, nullptr);
            if (::kevent(fd(), &change, 1, nullptr, 0, nullptr) == -1) {
                return fromErrno("kevent(EVFILT_SIGNAL)", errno);
            }
            struct sigaction ignore{};
            ignore.sa_handler = SIG_IGN;
            sigemptyset(&ignore.sa_mask);
            struct sigaction previous{};
            if (::sigaction(number, &ignore, &previous) == -1) {
                return fromErrno("sigaction", errno);
            }
            saved_.emplace_back(number, previous);
        }
        return {};
    }

    const Handle& handle() const noexcept override { return kq_; }

    Result<std::vector<Signal>> drain() override {
        std::vector<Signal> received;
        struct kevent events[16];
        const timespec zero{0, 0};
        while (true) {
            const int ready = ::kevent(fd(), nullptr, 0, events, 16, &zero);
            if (ready == -1) {
                if (errno == EINTR) {
                    continue;
                }
                return fromErrno("kevent", errno);
            }
            for (int i = 0; i < ready; ++i) {
                Signal signal{};
                if (events[i].filter == EVFILT_SIGNAL && signalFromNumber(static_cast<int>(events[i].ident), signal)) {
                    received.push_back(signal);
                }
            }
            if (ready < 16) {
                break;
            }
        }
        return received;
    }

private:
    int fd() const noexcept { return static_cast<int>(Backend::native(kq_)); }

    Handle kq_;
    std::vector<std::pair<int, struct sigaction>> saved_;
};

} // namespace

Result<std::unique_ptr<SignalSource>> createPlatformSignalSource(const std::vector<int>& signalNumbers) {
    const int kq = ::kqueue();
    if (kq == -1) {
        return fromErrno("kqueue", errno);
    }
    (void)::fcntl(kq, F_SETFD, FD_CLOEXEC);
    auto source = std::make_unique<KqueueSignalSource>(Backend::adopt(kq));
    PSX_TRY(source->subscribe(signalNumbers));
    return std::unique_ptr<SignalSource>(std::move(source));
}

} // namespace psx::os::posix
