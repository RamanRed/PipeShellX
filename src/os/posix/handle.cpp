#include "psx/os/handle.hpp"
#include "psx/os/backend.hpp"

#include "posix_error.hpp"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace psx::os {

namespace {

std::atomic<std::int64_t> gCreated{0};
std::atomic<std::int64_t> gClosed{0};

void closeDescriptor(int fd) noexcept {
#if defined(__linux__)
    // On Linux the descriptor is released even when close() reports EINTR;
    // retrying could close a descriptor that another thread just opened.
    (void)::close(fd);
#else
    while (::close(fd) == -1 && errno == EINTR) {}
#endif
    gClosed.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

Handle Backend::adopt(NativeHandle handle) noexcept {
    if (handle != kInvalidHandle) {
        gCreated.fetch_add(1, std::memory_order_relaxed);
    }
    return Handle(handle);
}

NativeHandle Backend::release(Handle& handle) noexcept {
    if (handle.handle_ != kInvalidHandle) {
        gClosed.fetch_add(1, std::memory_order_relaxed); // no longer accounted as open
    }
    return std::exchange(handle.handle_, kInvalidHandle);
}

Handle::~Handle() {
    close();
}

Handle::Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, kInvalidHandle)) {}

Handle& Handle::operator=(Handle&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalidHandle);
    }
    return *this;
}

void Handle::close() noexcept {
    if (!valid()) {
        return;
    }
    const int fd = static_cast<int>(std::exchange(handle_, kInvalidHandle));
    closeDescriptor(fd);
}

Result<Handle> Handle::duplicate() const {
    if (!valid()) {
        return Error{ErrorClass::Closed, EBADF, "dup"};
    }
    int copy = -1;
    do {
        copy = ::fcntl(static_cast<int>(handle_), F_DUPFD_CLOEXEC, 0);
    } while (copy == -1 && errno == EINTR);
    if (copy == -1) {
        return posix::fromErrno("dup", errno);
    }
    return Backend::adopt(copy);
}

Result<void> Handle::setNonBlocking(bool enabled) {
    if (!valid()) {
        return Error{ErrorClass::Closed, EBADF, "fcntl(F_SETFL)"};
    }
    const int fd = static_cast<int>(handle_);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return posix::fromErrno("fcntl(F_GETFL)", errno);
    }
    const int wanted = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (wanted != flags && ::fcntl(fd, F_SETFL, wanted) == -1) {
        return posix::fromErrno("fcntl(F_SETFL)", errno);
    }
    return {};
}

void Handle::swap(Handle& other) noexcept {
    std::swap(handle_, other.handle_);
}

HandleStats handleStats() noexcept {
    const std::int64_t created = gCreated.load(std::memory_order_relaxed);
    const std::int64_t closed = gClosed.load(std::memory_order_relaxed);
    return HandleStats{created - closed, created, closed};
}

} // namespace psx::os
