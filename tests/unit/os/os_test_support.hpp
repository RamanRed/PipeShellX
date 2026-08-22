#pragma once

// Helpers for the shared psx::os test suite. Tests may use platform APIs to
// *audit* the abstractions (descriptor enumeration, inheritance flags); the
// code under test must never need them.

#include <fcntl.h>
#include <set>
#include <unistd.h>

namespace os_test {

inline std::set<int> openDescriptors() {
    std::set<int> fds;
    const long limit = sysconf(_SC_OPEN_MAX) > 65536 ? 65536 : sysconf(_SC_OPEN_MAX);
    for (int fd = 0; fd < limit; ++fd) {
        if (fcntl(fd, F_GETFD) != -1) {
            fds.insert(fd);
        }
    }
    return fds;
}

inline bool isNonInheritable(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags != -1 && (flags & FD_CLOEXEC) != 0;
}

inline bool isNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags != -1 && (flags & O_NONBLOCK) != 0;
}

// Descriptors present in `after` but not in `before`.
inline std::set<int> newDescriptors(const std::set<int>& before, const std::set<int>& after) {
    std::set<int> created;
    for (int fd : after) {
        if (before.count(fd) == 0) {
            created.insert(fd);
        }
    }
    return created;
}

} // namespace os_test
