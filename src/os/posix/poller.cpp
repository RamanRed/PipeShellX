#include "psx/os/poller.hpp"

#include "poller_backends.hpp"

#include <cerrno>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define PSX_HAS_KQUEUE 1
#else
#define PSX_HAS_KQUEUE 0
#endif
#if defined(__linux__)
#define PSX_HAS_EPOLL 1
#else
#define PSX_HAS_EPOLL 0
#endif

namespace psx::os {

bool Poller::available(Backend backend) noexcept {
    switch (backend) {
        case Backend::Auto:
        case Backend::Poll:
            return true;
        case Backend::Kqueue:
            return PSX_HAS_KQUEUE != 0;
        case Backend::Epoll:
            return PSX_HAS_EPOLL != 0;
    }
    return false;
}

Result<std::unique_ptr<Poller>> Poller::create(Backend preferred) {
    Backend chosen = preferred;
    if (chosen == Backend::Auto) {
        chosen = PSX_HAS_EPOLL ? Backend::Epoll : (PSX_HAS_KQUEUE ? Backend::Kqueue : Backend::Poll);
    }
    if (!available(chosen)) {
        return Error{ErrorClass::Unsupported, ENOTSUP, "poller.create"};
    }
    switch (chosen) {
        case Backend::Poll:
            return posix::createPollPoller();
#if PSX_HAS_KQUEUE
        case Backend::Kqueue:
            return posix::createKqueuePoller();
#endif
#if PSX_HAS_EPOLL
        case Backend::Epoll:
            return posix::createEpollPoller();
#endif
        default:
            return Error{ErrorClass::Unsupported, ENOTSUP, "poller.create"};
    }
}

} // namespace psx::os
