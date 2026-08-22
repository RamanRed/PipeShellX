#pragma once

// Internal to src/os/posix: shared bookkeeping for the Poller backends.

#include "psx/os/backend.hpp"
#include "psx/os/poller.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>

namespace psx::os::posix {

inline constexpr std::uint64_t kWakeToken = std::numeric_limits<std::uint64_t>::max();

struct Registration {
    int fd;
    Interest interest;
};

// Token → descriptor map plus the argument checks every backend shares.
class PollerBase : public Poller {
public:
    std::size_t size() const noexcept override { return registrations_.size(); }

protected:
    Result<int> checkAdd(const Handle& handle, std::uint64_t token) const {
        if (!handle.valid()) {
            return Error{ErrorClass::Closed, EBADF, "poller.add"};
        }
        if (token == kWakeToken || registrations_.count(token) != 0) {
            return Error{ErrorClass::InvalidArgument, EINVAL, "poller.add"};
        }
        return static_cast<int>(psx::os::Backend::native(handle));
    }

    Result<Registration*> find(std::uint64_t token, const char* op) {
        auto it = registrations_.find(token);
        if (it == registrations_.end()) {
            return Error{ErrorClass::NotFound, ENOENT, op};
        }
        return &it->second;
    }

    std::unordered_map<std::uint64_t, Registration> registrations_;
};

// poll()/epoll_wait() take int milliseconds; -1 blocks.
inline int timeoutMs(std::optional<std::chrono::milliseconds> timeout) noexcept {
    if (!timeout) {
        return -1;
    }
    const auto count = timeout->count();
    if (count < 0) {
        return 0;
    }
    return count > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(count);
}

Result<std::unique_ptr<Poller>> createPollPoller();
Result<std::unique_ptr<Poller>> createKqueuePoller();
Result<std::unique_ptr<Poller>> createEpollPoller();

} // namespace psx::os::posix
