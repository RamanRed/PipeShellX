// epoll backend (Linux): EPOLLET | EPOLLRDHUP per handle, eventfd wake-ups.
// EPOLL_CTL_MOD re-arms an edge-triggered registration and reports pending
// readiness, which is what Interest::None → Readable relies on.

#include "poller_backends.hpp"
#include "posix_error.hpp"

#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

namespace psx::os::posix {

namespace {

std::uint32_t interestFlags(Interest interest) noexcept {
    std::uint32_t flags = EPOLLET | EPOLLRDHUP;
    if (has(interest, Interest::Readable)) {
        flags |= EPOLLIN;
    }
    if (has(interest, Interest::Writable)) {
        flags |= EPOLLOUT;
    }
    return flags;
}

class EpollPoller final : public PollerBase {
public:
    EpollPoller(int epfd, int eventFd) : epfd_(epfd), eventFd_(eventFd) {}

    ~EpollPoller() override {
        ::close(eventFd_);
        ::close(epfd_);
    }

    Backend backend() const noexcept override { return Backend::Epoll; }

    Result<void> add(const Handle& handle, Interest interest, std::uint64_t token) override {
        auto fd = checkAdd(handle, token);
        if (!fd.ok()) {
            return fd.error();
        }
        epoll_event ev{};
        ev.events = interestFlags(interest);
        ev.data.u64 = token;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd.value(), &ev) == -1) {
            return fromErrno("epoll_ctl(ADD)", errno);
        }
        registrations_.emplace(token, Registration{fd.value(), interest});
        return {};
    }

    Result<void> modify(std::uint64_t token, Interest interest) override {
        auto registration = find(token, "poller.modify");
        if (!registration.ok()) {
            return registration.error();
        }
        epoll_event ev{};
        ev.events = interestFlags(interest);
        ev.data.u64 = token;
        if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, registration.value()->fd, &ev) == -1) {
            return fromErrno("epoll_ctl(MOD)", errno);
        }
        registration.value()->interest = interest;
        return {};
    }

    Result<void> remove(std::uint64_t token) override {
        auto registration = find(token, "poller.remove");
        if (!registration.ok()) {
            return registration.error();
        }
        // EBADF/ENOENT: the descriptor was closed and the kernel already dropped it.
        if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, registration.value()->fd, nullptr) == -1 && errno != EBADF &&
            errno != ENOENT) {
            return fromErrno("epoll_ctl(DEL)", errno);
        }
        registrations_.erase(token);
        return {};
    }

    Result<std::size_t> wait(std::span<Event> events, std::optional<std::chrono::milliseconds> timeout) override {
        if (events.empty()) {
            return std::size_t{0};
        }
        // Retrieve no more than we can deliver: an event pulled from the kernel
        // consumes its edge (EPOLLET), so an undelivered one would be lost. The
        // kernel keeps any ready fd we do not fetch and returns it next wait().
        raw_.resize(events.size());
        const int ready = ::epoll_wait(epfd_, raw_.data(), static_cast<int>(raw_.size()), timeoutMs(timeout));
        if (ready == -1) {
            if (errno == EINTR) {
                return std::size_t{0};
            }
            return fromErrno("epoll_wait", errno);
        }
        std::size_t count = 0;
        for (int i = 0; i < ready; ++i) {
            const epoll_event& ev = raw_[static_cast<std::size_t>(i)];
            if (ev.data.u64 == kWakeToken) {
                std::uint64_t counter = 0;
                [[maybe_unused]] const ssize_t bytesRead = ::read(eventFd_, &counter, sizeof(counter));
                continue;
            }
            Readiness readiness = Readiness::None;
            if ((ev.events & EPOLLIN) != 0) {
                readiness = readiness | Readiness::Readable;
            }
            if ((ev.events & EPOLLOUT) != 0) {
                readiness = readiness | Readiness::Writable;
            }
            if ((ev.events & (EPOLLHUP | EPOLLRDHUP)) != 0) {
                readiness = readiness | Readiness::Hangup;
            }
            if ((ev.events & EPOLLERR) != 0) {
                readiness = readiness | Readiness::Error;
            }
            if (count < events.size()) {
                events[count++] = Event{ev.data.u64, readiness};
            }
        }
        return count;
    }

    Result<void> wake() override {
        const std::uint64_t one = 1;
        if (::write(eventFd_, &one, sizeof(one)) == -1 && errno != EAGAIN) {
            return fromErrno("eventfd write", errno);
        }
        return {};
    }

private:
    int epfd_;
    int eventFd_;
    std::vector<epoll_event> raw_;
};

} // namespace

Result<std::unique_ptr<Poller>> createEpollPoller() {
    const int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        return fromErrno("epoll_create1", errno);
    }
    const int eventFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (eventFd == -1) {
        const int err = errno;
        ::close(epfd);
        return fromErrno("eventfd", err);
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = kWakeToken;
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, eventFd, &ev) == -1) {
        const int err = errno;
        ::close(eventFd);
        ::close(epfd);
        return fromErrno("epoll_ctl(ADD eventfd)", err);
    }
    return std::unique_ptr<Poller>(new EpollPoller(epfd, eventFd));
}

} // namespace psx::os::posix
