// Portable, level-triggered backend: the pollfd set is rebuilt on every
// wait() — O(N) per wake-up, which is exactly today's loop and the oracle
// for the other backends.

#include "poller_backends.hpp"
#include "posix_error.hpp"
#include "psx/os/pipe.hpp"

#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <vector>

namespace psx::os::posix {

namespace {

class PollPoller final : public PollerBase {
public:
    explicit PollPoller(Pipe wakePipe) : wake_(std::move(wakePipe)) {}

    Backend backend() const noexcept override { return Backend::Poll; }

    Result<void> add(const Handle& handle, Interest interest, std::uint64_t token) override {
        auto fd = checkAdd(handle, token);
        if (!fd.ok()) {
            return fd.error();
        }
        registrations_.emplace(token, Registration{fd.value(), interest});
        return {};
    }

    Result<void> modify(std::uint64_t token, Interest interest) override {
        auto registration = find(token, "poller.modify");
        if (!registration.ok()) {
            return registration.error();
        }
        registration.value()->interest = interest;
        return {};
    }

    Result<void> remove(std::uint64_t token) override {
        if (registrations_.erase(token) == 0) {
            return Error{ErrorClass::NotFound, ENOENT, "poller.remove"};
        }
        return {};
    }

    Result<std::size_t> wait(std::span<Event> events, std::optional<std::chrono::milliseconds> timeout) override {
        pollFds_.clear();
        tokens_.clear();
        pollFds_.push_back(pollfd{static_cast<int>(psx::os::Backend::native(wake_.reader)), POLLIN, 0});
        tokens_.push_back(kWakeToken);
        for (const auto& [token, registration] : registrations_) {
            short flags = 0;
            if (has(registration.interest, Interest::Readable)) {
                flags |= POLLIN;
            }
            if (has(registration.interest, Interest::Writable)) {
                flags |= POLLOUT;
            }
            if (flags == 0) {
                continue;
            }
            pollFds_.push_back(pollfd{registration.fd, flags, 0});
            tokens_.push_back(token);
        }

        const int ready = ::poll(pollFds_.data(), static_cast<nfds_t>(pollFds_.size()), timeoutMs(timeout));
        if (ready == -1) {
            if (errno == EINTR) {
                return std::size_t{0};
            }
            return fromErrno("poll", errno);
        }

        std::size_t count = 0;
        for (std::size_t i = 0; i < pollFds_.size() && count < events.size(); ++i) {
            const short revents = pollFds_[i].revents;
            if (revents == 0) {
                continue;
            }
            if (tokens_[i] == kWakeToken) {
                drainWake();
                continue;
            }
            Readiness readiness = Readiness::None;
            if ((revents & POLLIN) != 0) {
                readiness = readiness | Readiness::Readable;
            }
            if ((revents & POLLOUT) != 0) {
                readiness = readiness | Readiness::Writable;
            }
            if ((revents & POLLHUP) != 0) {
                readiness = readiness | Readiness::Hangup;
            }
            if ((revents & (POLLERR | POLLNVAL)) != 0) {
                readiness = readiness | Readiness::Error;
            }
            events[count++] = Event{tokens_[i], readiness};
        }
        return count;
    }

    Result<void> wake() override {
        const char byte = 1;
        const ssize_t written = ::write(static_cast<int>(psx::os::Backend::native(wake_.writer)), &byte, 1);
        if (written == -1 && errno != EAGAIN && errno != EINTR) {
            return fromErrno("poller.wake", errno); // EAGAIN: a wake is already pending
        }
        return {};
    }

private:
    void drainWake() noexcept {
        char buffer[64];
        while (::read(static_cast<int>(psx::os::Backend::native(wake_.reader)), buffer, sizeof(buffer)) > 0) {}
    }

    Pipe wake_;
    std::vector<pollfd> pollFds_;
    std::vector<std::uint64_t> tokens_;
};

} // namespace

Result<std::unique_ptr<Poller>> createPollPoller() {
    auto pipe = Pipe::create();
    if (!pipe.ok()) {
        return pipe.error();
    }
    PSX_TRY(pipe.value().reader.setNonBlocking(true));
    PSX_TRY(pipe.value().writer.setNonBlocking(true));
    return std::unique_ptr<Poller>(new PollPoller(std::move(pipe.value())));
}

} // namespace psx::os::posix
