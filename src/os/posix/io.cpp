#include "psx/os/io.hpp"
#include "psx/os/backend.hpp"

#include "posix_error.hpp"

#include <cerrno>
#include <unistd.h>

namespace psx::os {

Result<std::size_t> read(const Handle& handle, std::span<char> buffer) {
    if (!handle.valid()) {
        return Error{ErrorClass::Closed, EBADF, "read"};
    }
    const int fd = static_cast<int>(Backend::native(handle));
    while (true) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }
        if (errno != EINTR) {
            return posix::fromErrno("read", errno);
        }
    }
}

Result<std::size_t> write(const Handle& handle, std::span<const char> data) {
    if (!handle.valid()) {
        return Error{ErrorClass::Closed, EBADF, "write"};
    }
    const int fd = static_cast<int>(Backend::native(handle));
    while (true) {
        const ssize_t count = ::write(fd, data.data(), data.size());
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }
        if (errno != EINTR) {
            return posix::fromErrno("write", errno);
        }
    }
}

} // namespace psx::os
