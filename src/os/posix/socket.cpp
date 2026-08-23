#include "psx/os/socket.hpp"

#include "psx/os/backend.hpp"

#include "posix_error.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace psx::os {

namespace {

int fd(const Handle& handle) {
    return static_cast<int>(Backend::native(handle));
}

// Makes a freshly-created fd non-blocking and close-on-exec (portable; macOS has
// no SOCK_CLOEXEC/SOCK_NONBLOCK socket-type flags).
Result<void> makeNonBlockingCloexec(int rawFd) {
    const int flags = ::fcntl(rawFd, F_GETFL, 0);
    if (flags == -1 || ::fcntl(rawFd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return posix::fromErrno("fcntl(O_NONBLOCK)", errno);
    }
    const int fdflags = ::fcntl(rawFd, F_GETFD, 0);
    if (fdflags == -1 || ::fcntl(rawFd, F_SETFD, fdflags | FD_CLOEXEC) == -1) {
        return posix::fromErrno("fcntl(FD_CLOEXEC)", errno);
    }
    return {};
}

Error gaiError(const char* op, int gai) {
    // getaddrinfo failures are not errno; report EAI_SYSTEM's errno, else Other.
    if (gai == EAI_SYSTEM) {
        return posix::fromErrno(op, errno);
    }
    return Error{ErrorClass::Other, gai, op};
}

} // namespace

Result<Socket> Socket::connect(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (const int gai = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results); gai != 0) {
        return gaiError("getaddrinfo", gai);
    }

    Error lastError{ErrorClass::Other, 0, "connect"};
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        const int rawFd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (rawFd == -1) {
            lastError = posix::fromErrno("socket", errno);
            continue;
        }
        if (auto flags = makeNonBlockingCloexec(rawFd); !flags.ok()) {
            (void)::close(rawFd);
            lastError = flags.error();
            continue;
        }
        if (::connect(rawFd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS) {
            ::freeaddrinfo(results);
            return Socket(Backend::adopt(rawFd));
        }
        lastError = posix::fromErrno("connect", errno);
        (void)::close(rawFd);
    }
    ::freeaddrinfo(results);
    return lastError;
}

Result<void> Socket::connectResult() const {
    int soError = 0;
    socklen_t len = sizeof(soError);
    if (::getsockopt(fd(handle_), SOL_SOCKET, SO_ERROR, &soError, &len) == -1) {
        return posix::fromErrno("getsockopt(SO_ERROR)", errno);
    }
    if (soError != 0) {
        return posix::fromErrno("connect", soError);
    }
    return {};
}

Result<Socket> Socket::listen(const std::string& host, std::uint16_t port, int backlog) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (const int gai = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &results);
        gai != 0) {
        return gaiError("getaddrinfo", gai);
    }

    Error lastError{ErrorClass::Other, 0, "listen"};
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        const int rawFd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (rawFd == -1) {
            lastError = posix::fromErrno("socket", errno);
            continue;
        }
        const int one = 1;
        (void)::setsockopt(rawFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (auto flags = makeNonBlockingCloexec(rawFd); !flags.ok()) {
            (void)::close(rawFd);
            lastError = flags.error();
            continue;
        }
        if (::bind(rawFd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(rawFd, backlog) == 0) {
            ::freeaddrinfo(results);
            return Socket(Backend::adopt(rawFd));
        }
        lastError = posix::fromErrno("bind/listen", errno);
        (void)::close(rawFd);
    }
    ::freeaddrinfo(results);
    return lastError;
}

Result<Socket> Socket::accept() const {
    const int rawFd = ::accept(fd(handle_), nullptr, nullptr);
    if (rawFd == -1) {
        return posix::fromErrno("accept", errno);
    }
    if (auto flags = makeNonBlockingCloexec(rawFd); !flags.ok()) {
        (void)::close(rawFd);
        return flags.error();
    }
    return Socket(Backend::adopt(rawFd));
}

namespace {
// Creates an AF_UNIX stream socket bound (server) or connected (client) to path.
Result<Handle> makeUnixSocket(const std::string& path, bool server, int backlog) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        return Error{ErrorClass::InvalidArgument, 0, "unix socket path too long"};
    }
    std::memcpy(addr.sun_path, path.data(), path.size()); // struct is zero-filled: NUL-terminated
    const int rawFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (rawFd == -1) {
        return posix::fromErrno("socket", errno);
    }
    if (auto flags = makeNonBlockingCloexec(rawFd); !flags.ok()) {
        (void)::close(rawFd);
        return flags.error();
    }
    const auto* sa = reinterpret_cast<const sockaddr*>(&addr);
    if (server) {
        (void)::unlink(path.c_str()); // drop a stale socket file from a prior run
        if (::bind(rawFd, sa, sizeof(addr)) != 0 || ::listen(rawFd, backlog) != 0) {
            Error e = posix::fromErrno("bind/listen", errno);
            (void)::close(rawFd);
            return e;
        }
    } else if (::connect(rawFd, sa, sizeof(addr)) != 0 && errno != EINPROGRESS) {
        Error e = posix::fromErrno("connect", errno);
        (void)::close(rawFd);
        return e;
    }
    return Backend::adopt(rawFd);
}
} // namespace

Result<Socket> Socket::listenUnix(const std::string& path, int backlog) {
    auto handle = makeUnixSocket(path, /*server=*/true, backlog);
    if (!handle.ok()) {
        return handle.error();
    }
    return Socket(std::move(handle.value()));
}

Result<Socket> Socket::connectUnix(const std::string& path) {
    auto handle = makeUnixSocket(path, /*server=*/false, 0);
    if (!handle.ok()) {
        return handle.error();
    }
    return Socket(std::move(handle.value()));
}

Result<std::uint16_t> Socket::localPort() const {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd(handle_), reinterpret_cast<sockaddr*>(&addr), &len) == -1) {
        return posix::fromErrno("getsockname", errno);
    }
    if (addr.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    }
    return Error{ErrorClass::Other, 0, "getsockname: unexpected address family"};
}

} // namespace psx::os
