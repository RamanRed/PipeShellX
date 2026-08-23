#pragma once

#include "psx/os/handle.hpp"
#include "psx/result.hpp"

#include <cstdint>
#include <string>

namespace psx::os {

// A non-blocking, close-on-exec TCP socket for the reactor. Byte I/O goes through
// psx::os::read/write on handle(). Connecting is asynchronous: connect() may
// return with the handshake still in progress — watch handle() for Writable,
// then call connectResult() to learn whether it succeeded.
class Socket {
public:
    Socket() = default;

    const Handle& handle() const noexcept { return handle_; }
    Handle& handle() noexcept { return handle_; }
    bool valid() const noexcept { return handle_.valid(); }

    // Starts a TCP connection to host:port. Returns the socket even while the
    // connect is still in flight (EINPROGRESS); use connectResult() once the
    // reactor reports the handle Writable.
    static Result<Socket> connect(const std::string& host, std::uint16_t port);
    // Ok when a non-blocking connect has completed successfully; an error carries
    // the connect failure (SO_ERROR).
    Result<void> connectResult() const;
    // True while a connect() is still in flight (returned EINPROGRESS): the caller
    // should wait for the handle to become Writable, then call connectResult().
    bool connecting() const noexcept { return connecting_; }
    // After connectResult() reports failure, try the next resolved address of the
    // original host (dual-stack fallback, e.g. ::1 refused -> 127.0.0.1). Replaces
    // handle() with a fresh socket; an error means no address is left.
    Result<void> connectNextAddress();

    // A listening socket bound to host:port (SO_REUSEADDR). Port 0 asks the OS
    // for an ephemeral port — read it back with localPort().
    static Result<Socket> listen(const std::string& host, std::uint16_t port, int backlog = 128);

    // AF_UNIX stream sockets for a node's local control endpoint. connectUnix is
    // effectively immediate but still non-blocking (connectResult() applies).
    // listenUnix unlinks a stale socket file at `path` before binding; the caller
    // removes the file when done. Fails if `path` exceeds the platform limit.
    static Result<Socket> connectUnix(const std::string& path);
    static Result<Socket> listenUnix(const std::string& path, int backlog = 128);
    // Accepts one pending connection; a WouldBlock error means none is ready.
    Result<Socket> accept() const;

    // The local port the socket is bound to (host byte order).
    Result<std::uint16_t> localPort() const;

private:
    explicit Socket(Handle handle) noexcept : handle_(std::move(handle)) {}
    Result<void> connectFrom(std::size_t startIndex); // resolve + connect from a cursor

    Handle handle_;
    std::string connectHost_; // retained so connectNextAddress() can re-resolve
    std::uint16_t connectPort_ = 0;
    std::size_t addressCursor_ = 0; // resolved addresses already attempted
    bool connecting_ = false;
};

} // namespace psx::os
