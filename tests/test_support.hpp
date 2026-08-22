#pragma once

// Shared helpers for the PipeShellX test suite (POSIX only until Phase 3).

#include "client_config.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace test_support {

// Runs the body of a test inside a fresh, empty temporary directory so that a
// stray clients.txt / log file in the caller's CWD can never influence it.
class ScopedTempCwd {
public:
    explicit ScopedTempCwd(const std::string& tag) {
        original_ = std::filesystem::current_path();
        dir_ = std::filesystem::temp_directory_path() / ("pipeshellx-test-" + tag + "-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        // Resolve symlinks (macOS: /var -> /private/var) so that paths derived
        // from the real CWD compare equal to path().
        dir_ = std::filesystem::canonical(dir_);
        std::filesystem::current_path(dir_);
    }

    ~ScopedTempCwd() {
        std::error_code ignored;
        std::filesystem::current_path(original_, ignored);
        std::filesystem::remove_all(dir_, ignored);
    }

    ScopedTempCwd(const ScopedTempCwd&) = delete;
    ScopedTempCwd& operator=(const ScopedTempCwd&) = delete;

    const std::filesystem::path& path() const noexcept { return dir_; }

private:
    std::filesystem::path original_;
    std::filesystem::path dir_;
};

// Sets (or unsets, when value is nullopt) an environment variable for the
// lifetime of the object and restores the previous value afterwards.
class ScopedEnv {
public:
    ScopedEnv(std::string name, std::optional<std::string> value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str())) {
            previous_ = previous;
        }
        apply(value);
    }

    ~ScopedEnv() { apply(previous_); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void apply(const std::optional<std::string>& value) {
        if (value.has_value()) {
            ::setenv(name_.c_str(), value->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    std::string name_;
    std::optional<std::string> previous_;
};

// 127.0.0.1:1 is closed on every sane host, so ssh fails immediately with
// "connection refused" instead of waiting for ConnectTimeout. Used wherever a
// test needs a real, fast, deterministic SSH failure.
inline ClientEntry refusedLoopbackClient() {
    ClientEntry client;
    client.user = "nobody";
    client.host = "127.0.0.1";
    client.port = 1;
    return client;
}

// A loopback TCP listener that never accept()s. Clients complete the TCP
// handshake (kernel backlog) but never receive a byte, which is how a test
// gets a deterministic "hung host" without touching the network.
class SilentListener {
public:
    SilentListener() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == -1) {
            throw std::runtime_error("socket() failed");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // kernel-assigned
        socklen_t length = sizeof(address);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), length) == -1 || ::listen(fd_, 1) == -1 ||
            ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) == -1) {
            ::close(fd_);
            throw std::runtime_error("could not set up loopback listener");
        }
        port_ = ntohs(address.sin_port);
    }

    ~SilentListener() { ::close(fd_); }

    SilentListener(const SilentListener&) = delete;
    SilentListener& operator=(const SilentListener&) = delete;

    std::uint16_t port() const noexcept { return port_; }

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

} // namespace test_support
