#include "psx/os/pipe.hpp"
#include "psx/os/backend.hpp"

#include "posix_error.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace psx::os {

Result<Pipe> Pipe::create() {
    int fds[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) == -1) {
        return posix::fromErrno("pipe2", errno);
    }
#else
    // No pipe2(): mark both ends non-inheritable immediately after creation.
    // The window between the two calls is the documented Darwin caveat (§3.3).
    if (::pipe(fds) == -1) {
        return posix::fromErrno("pipe", errno);
    }
    for (int fd : fds) {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
            const int err = errno;
            (void)::close(fds[0]);
            (void)::close(fds[1]);
            return posix::fromErrno("fcntl(FD_CLOEXEC)", err);
        }
    }
#endif
    Pipe pipe;
    pipe.reader = Backend::adopt(fds[0]);
    pipe.writer = Backend::adopt(fds[1]);
    return pipe;
}

} // namespace psx::os
