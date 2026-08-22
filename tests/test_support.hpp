#pragma once

// Shared helpers for the PipeShellX test suite (POSIX only until Phase 3).

#include "client_config.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// Puts fake `ssh` and `sshpass` executables first on PATH for the lifetime
// of the object, so the remote-execution path can be exercised without a
// network. The fake ssh is driven by the remote command it receives:
//   ok           -> "host=<target>" on stdout, exit 0
//   fail N       -> "failing" on stderr, exit N
//   refused      -> OpenSSH's "Connection refused" text, exit 255
//   denied       -> "Permission denied (publickey)." exit 255
//   hostkey      -> "Host key verification failed." exit 255
//   hang         -> sleeps 30 s
//   pw           -> "pw=<password>" as delivered by the fake sshpass over its fd
//   big          -> 200 000 bytes on stdout and 100 000 on stderr
class FakeSshOnPath {
public:
    FakeSshOnPath() {
        dir_ = std::filesystem::temp_directory_path() / ("pipeshellx-fakessh-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
        writeScript("ssh", R"sh(#!/bin/sh
prev=""; last=""
for a in "$@"; do prev="$last"; last="$a"; done
target="$prev"
# Strip one shell-quoting level the way a real remote shell would, so both an
# unquoted command ("ok") and a per-argument-quoted one ("'\''ok'\''") dispatch alike.
eval "set -- $last" 2>/dev/null || set -- "$last"
cmd="$1"; rest="$*"
case "$cmd" in
  ok)       echo "host=$target"; exit 0 ;;
  fail)     r="${rest#fail }"; case "$r" in *:*) echo "${r#*:}" >&2; exit "${r%%:*}" ;; *) echo "failing" >&2; exit "$r" ;; esac ;;
  refused)  echo "ssh: connect to host $target port 22: Connection refused" >&2; exit 255 ;;
  denied)   echo "$target: Permission denied (publickey)." >&2; exit 255 ;;
  hostkey)  echo "Host key verification failed." >&2; exit 255 ;;
  hang)     sleep 30; exit 0 ;;
  pw)       echo "pw=$PSX_FAKE_PW"; exit 0 ;;
  big)      head -c 200000 /dev/zero | tr '\0' 'o'; head -c 100000 /dev/zero | tr '\0' 'e' >&2; exit 0 ;;
  echo)     shift; printf '%s\n' "$*"; exit 0 ;;
  *)        echo "fake ssh: unknown command '$last'" >&2; exit 9 ;;
esac
)sh");
        writeScript("sshpass", R"sh(#!/bin/sh
[ "$1" = "-d" ] || { echo "fake sshpass: expected -d, got $1" >&2; exit 9; }
fd=$2; shift 2
eval "pw=\$(cat <&$fd)"
PSX_FAKE_PW="$pw" exec "$@"
)sh");
        const char* previous = std::getenv("PATH");
        previousPath_ = previous != nullptr ? std::string(previous) : std::string();
        ::setenv("PATH", (dir_.string() + ":" + previousPath_).c_str(), 1);
    }

    ~FakeSshOnPath() {
        ::setenv("PATH", previousPath_.c_str(), 1);
        std::error_code ignored;
        std::filesystem::remove_all(dir_, ignored);
    }

    FakeSshOnPath(const FakeSshOnPath&) = delete;
    FakeSshOnPath& operator=(const FakeSshOnPath&) = delete;

private:
    void writeScript(const char* name, const char* body) {
        const auto path = dir_ / name;
        {
            std::ofstream out(path);
            out << body;
        }
        std::filesystem::permissions(path, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                               std::filesystem::perms::group_exec);
    }

    std::filesystem::path dir_;
    std::string previousPath_;
};

} // namespace test_support
