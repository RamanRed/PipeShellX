#include "psx/os/system.hpp"

#include "posix_error.hpp"

#include <cerrno>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/syslimits.h>
#endif

namespace psx::os {

ProcessId currentProcessId() noexcept {
    return static_cast<ProcessId>(::getpid());
}

Result<HandleLimit> raiseHandleLimit() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == -1) {
        return posix::fromErrno("getrlimit(RLIMIT_NOFILE)", errno);
    }
    rlim_t target = limit.rlim_max;
#if defined(__APPLE__)
    // Darwin refuses soft limits above OPEN_MAX even when the hard limit is unlimited.
    if (target == RLIM_INFINITY || target > OPEN_MAX) {
        target = OPEN_MAX;
    }
#endif
    if (target != RLIM_INFINITY && target > limit.rlim_cur) {
        rlimit raised{target, limit.rlim_max};
        if (::setrlimit(RLIMIT_NOFILE, &raised) == 0) {
            limit = raised;
        } else if (::setrlimit(RLIMIT_NOFILE, &limit) == -1) {
            return posix::fromErrno("setrlimit(RLIMIT_NOFILE)", errno);
        }
    }
    const auto value = [](rlim_t v) {
        return v == RLIM_INFINITY ? static_cast<std::uint64_t>(-1) : static_cast<std::uint64_t>(v);
    };
    return HandleLimit{value(limit.rlim_cur), value(limit.rlim_max)};
}

bool isExecutableFile(const std::string& path) noexcept {
    if (path.empty()) {
        return false;
    }
    struct stat info{};
    if (::stat(path.c_str(), &info) == -1 || !S_ISREG(info.st_mode)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

} // namespace psx::os
