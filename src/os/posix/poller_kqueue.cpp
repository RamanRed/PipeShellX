// kqueue backend (Darwin / BSD): EV_CLEAR filters per handle, EVFILT_USER for
// wake-ups. Attaching a filter reports the current state, so re-arming after
// Interest::None surfaces data that arrived in between.

#include "poller_backends.hpp"
#include "posix_error.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace psx::os::posix {

namespace {

constexpr std::uintptr_t kWakeIdent = 0;

void* tokenToUdata(std::uint64_t token) noexcept {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
}

std::uint64_t udataToToken(void* udata) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(udata));
}

class KqueuePoller final : public PollerBase {
public:
    explicit KqueuePoller(int kq) : kq_(kq) {}

    ~KqueuePoller() override { ::close(kq_); }

    Backend backend() const noexcept override { return Backend::Kqueue; }

    Result<void> add(const Handle& handle, Interest interest, std::uint64_t token) override {
        auto fd = checkAdd(handle, token);
        if (!fd.ok()) {
            return fd.error();
        }
        PSX_TRY(applyFilters(fd.value(), Interest::None, interest, token));
        registrations_.emplace(token, Registration{fd.value(), interest});
        return {};
    }

    Result<void> modify(std::uint64_t token, Interest interest) override {
        auto registration = find(token, "poller.modify");
        if (!registration.ok()) {
            return registration.error();
        }
        PSX_TRY(applyFilters(registration.value()->fd, registration.value()->interest, interest, token));
        registration.value()->interest = interest;
        return {};
    }

    Result<void> remove(std::uint64_t token) override {
        auto registration = find(token, "poller.remove");
        if (!registration.ok()) {
            return registration.error();
        }
        // A closed descriptor has already dropped its knotes: EBADF/ENOENT are fine.
        (void)applyFilters(registration.value()->fd, registration.value()->interest, Interest::None, token);
        registrations_.erase(token);
        return {};
    }

    Result<std::size_t> wait(std::span<Event> events, std::optional<std::chrono::milliseconds> timeout) override {
        if (events.empty()) {
            return std::size_t{0};
        }
        raw_.resize(events.size() + 1); // one slot for a possible wake-up
        timespec ts{};
        const timespec* tsPtr = nullptr;
        if (timeout) {
            const int ms = timeoutMs(timeout);
            ts.tv_sec = ms / 1000;
            ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
            tsPtr = &ts;
        }
        const int ready = ::kevent(kq_, nullptr, 0, raw_.data(), static_cast<int>(raw_.size()), tsPtr);
        if (ready == -1) {
            if (errno == EINTR) {
                return std::size_t{0};
            }
            return fromErrno("kevent", errno);
        }

        std::size_t count = 0;
        for (int i = 0; i < ready; ++i) {
            const struct kevent& ev = raw_[static_cast<std::size_t>(i)];
            if (ev.filter == EVFILT_USER) {
                continue; // wake-up, not a user event
            }
            const std::uint64_t token = udataToToken(ev.udata);
            Readiness readiness = Readiness::None;
            if (ev.filter == EVFILT_READ) {
                readiness = readiness | Readiness::Readable;
            } else if (ev.filter == EVFILT_WRITE) {
                readiness = readiness | Readiness::Writable;
            }
            if ((ev.flags & EV_EOF) != 0) {
                readiness = readiness | Readiness::Hangup;
            }
            if ((ev.flags & EV_ERROR) != 0) {
                readiness = readiness | Readiness::Error;
            }
            // Read and write filters of one handle arrive as two kevents: merge.
            bool merged = false;
            for (std::size_t j = 0; j < count; ++j) {
                if (events[j].token == token) {
                    events[j].readiness = events[j].readiness | readiness;
                    merged = true;
                    break;
                }
            }
            if (!merged && count < events.size()) {
                events[count++] = Event{token, readiness};
            }
        }
        return count;
    }

    Result<void> wake() override {
        struct kevent trigger;
        EV_SET(&trigger, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, tokenToUdata(kWakeToken));
        if (::kevent(kq_, &trigger, 1, nullptr, 0, nullptr) == -1) {
            return fromErrno("kevent(NOTE_TRIGGER)", errno);
        }
        return {};
    }

    Result<void> registerWake() {
        struct kevent change;
        EV_SET(&change, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, tokenToUdata(kWakeToken));
        if (::kevent(kq_, &change, 1, nullptr, 0, nullptr) == -1) {
            return fromErrno("kevent(EVFILT_USER)", errno);
        }
        return {};
    }

private:
    // Transitions the read/write filters of `fd` from `from` to `to`.
    Result<void> applyFilters(int fd, Interest from, Interest to, std::uint64_t token) {
        struct kevent changes[2];
        int n = 0;
        auto transition = [&](Interest flag, short filter) {
            const bool had = has(from, flag);
            const bool want = has(to, flag);
            if (want && !had) {
                EV_SET(&changes[n++], static_cast<std::uintptr_t>(fd), filter, EV_ADD | EV_CLEAR | EV_RECEIPT, 0, 0,
                       tokenToUdata(token));
            } else if (had && !want) {
                EV_SET(&changes[n++], static_cast<std::uintptr_t>(fd), filter, EV_DELETE | EV_RECEIPT, 0, 0,
                       tokenToUdata(token));
            }
        };
        transition(Interest::Readable, EVFILT_READ);
        transition(Interest::Writable, EVFILT_WRITE);
        if (n == 0) {
            return {};
        }
        struct kevent receipts[2];
        const int got = ::kevent(kq_, changes, n, receipts, n, nullptr);
        if (got == -1) {
            return fromErrno("kevent", errno);
        }
        for (int i = 0; i < got; ++i) {
            if ((receipts[i].flags & EV_ERROR) != 0 && receipts[i].data != 0) {
                const int err = static_cast<int>(receipts[i].data);
                if ((changes[i].flags & EV_DELETE) != 0 && (err == ENOENT || err == EBADF)) {
                    continue; // nothing to delete any more
                }
                return fromErrno((changes[i].flags & EV_DELETE) != 0 ? "kevent(EV_DELETE)" : "kevent(EV_ADD)", err);
            }
        }
        return {};
    }

    int kq_;
    std::vector<struct kevent> raw_;
};

} // namespace

Result<std::unique_ptr<Poller>> createKqueuePoller() {
    const int kq = ::kqueue();
    if (kq == -1) {
        return fromErrno("kqueue", errno);
    }
    (void)::fcntl(kq, F_SETFD, FD_CLOEXEC);
    auto poller = std::make_unique<KqueuePoller>(kq);
    PSX_TRY(poller->registerWake());
    return std::unique_ptr<Poller>(std::move(poller));
}

} // namespace psx::os::posix
