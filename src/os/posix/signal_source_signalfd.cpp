// SignalSource for Linux: the subscribed signals are blocked in the calling
// thread (inherited by threads created afterwards) and read from a signalfd.

#include "psx/os/backend.hpp"
#include "psx/os/signal_source.hpp"

#include "posix_error.hpp"
#include "sources.hpp"

#include <cerrno>
#include <csignal>
#include <sys/signalfd.h>
#include <unistd.h>
#include <vector>

namespace psx::os::posix {

namespace {

class SignalfdSource final : public SignalSource {
public:
    SignalfdSource(Handle fd, sigset_t previousMask) : fd_(std::move(fd)), previousMask_(previousMask) {}

    ~SignalfdSource() override {
        fd_.close();
        (void)::pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr);
    }

    const Handle& handle() const noexcept override { return fd_; }

    Result<std::vector<Signal>> drain() override {
        std::vector<Signal> received;
        signalfd_siginfo info{};
        while (true) {
            const ssize_t got = ::read(static_cast<int>(Backend::native(fd_)), &info, sizeof(info));
            if (got == static_cast<ssize_t>(sizeof(info))) {
                Signal signal{};
                if (signalFromNumber(static_cast<int>(info.ssi_signo), signal)) {
                    received.push_back(signal);
                }
                continue;
            }
            if (got == -1 && errno == EINTR) {
                continue;
            }
            if (got == -1 && errno != EAGAIN) {
                return fromErrno("read(signalfd)", errno);
            }
            break;
        }
        return received;
    }

private:
    Handle fd_;
    sigset_t previousMask_;
};

} // namespace

Result<std::unique_ptr<SignalSource>> createPlatformSignalSource(const std::vector<int>& signalNumbers) {
    sigset_t wanted;
    sigemptyset(&wanted);
    for (int number : signalNumbers) {
        sigaddset(&wanted, number);
    }
    sigset_t previous;
    if (const int rc = ::pthread_sigmask(SIG_BLOCK, &wanted, &previous); rc != 0) {
        return fromErrno("pthread_sigmask", rc);
    }
    const int fd = ::signalfd(-1, &wanted, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd == -1) {
        const int err = errno;
        (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        return fromErrno("signalfd", err);
    }
    return std::unique_ptr<SignalSource>(new SignalfdSource(Backend::adopt(fd), previous));
}

} // namespace psx::os::posix
