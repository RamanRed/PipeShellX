#pragma once

// psx::os::Process — spawn a child with explicit stdio, its own process
// group, optional resource limits; wait for it; stop it gracefully or hard.
// Exit status is a tagged type: no WIFEXITED / GetExitCodeProcess leaks.

#include "psx/os/handle.hpp"
#include "psx/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace psx::os {

using ProcessId = std::int64_t;

struct ExitStatus {
    enum class Kind : std::uint8_t {
        Exited,    // code = exit code
        Signaled,  // code = signal number (POSIX)
        Terminated // code = platform status (Win32 STATUS_*)
    };
    Kind kind = Kind::Exited;
    int code = 0;

    bool success() const noexcept { return kind == Kind::Exited && code == 0; }
};

enum class StopSignal : std::uint8_t {
    Graceful, // SIGTERM to the process group (CTRL_BREAK on Windows)
    Kill      // SIGKILL to the process group (TerminateJobObject)
};

struct Limits {
    std::optional<std::uint32_t> cpuSeconds;        // RLIMIT_CPU
    std::optional<std::uint64_t> addressSpaceBytes; // RLIMIT_AS (advisory on Darwin)
    std::optional<std::uint32_t> openHandles;       // RLIMIT_NOFILE
};

struct SpawnSpec {
    struct Stdio {
        enum class Mode : std::uint8_t { Inherit, Null, Handle };
        Mode mode = Mode::Inherit;
        const Handle* handle = nullptr; // borrowed only for the duration of spawn()

        static Stdio inherit() noexcept { return Stdio{}; }
        static Stdio null() noexcept { return Stdio{Mode::Null, nullptr}; }
        static Stdio from(const Handle& source) noexcept { return Stdio{Mode::Handle, &source}; }
    };

    std::string program;                         // path, or a name looked up on PATH when it has no '/'
    std::vector<std::string> argv;               // including argv[0]
    std::optional<std::vector<std::string>> env; // "KEY=value" entries; nullopt inherits
    std::string cwd;                             // empty inherits
    Stdio in;
    Stdio out;
    Stdio err;
    Limits limits;

    // Handles made inheritable at fixed descriptor numbers (≥ 3) in the
    // child, e.g. a password pipe for `sshpass -d 3`. Borrowed during spawn().
    struct InheritedHandle {
        const Handle* handle = nullptr;
        int targetFd = -1;
    };
    std::vector<InheritedHandle> extraHandles;
};

class Process {
public:
    // Synchronous failure for a missing program, directory, or permission:
    // no reapable child exists when spawn() returns an error. (Darwin keeps
    // a transient, self-reaping child for a few milliseconds after a failed
    // exec; it never becomes a zombie.)
    static Result<Process> spawn(const SpawnSpec& spec);

    Process() noexcept = default;
    // A still-running child is killed (whole group) and reaped.
    ~Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    ProcessId id() const noexcept { return id_; }
    ProcessId groupId() const noexcept { return group_; }

    // True between spawn() and the reaping wait()/tryWait() (or release()).
    bool running() const noexcept { return owns_; }

    // Signals the whole process group; still valid after the leader was
    // reaped (descendants that stayed in the group). NoSuchProcess when empty.
    Result<void> signal(StopSignal how);

    // Reaps the child. Cached after the first success; retries EINTR.
    Result<ExitStatus> wait();
    // Non-blocking: nullopt while the child is still running.
    Result<std::optional<ExitStatus>> tryWait();

    // Gives up ownership; the caller must reap the child itself.
    ProcessId release() noexcept;

private:
    Process(ProcessId id, ProcessId group) noexcept : id_(id), group_(group), owns_(true) {}

    ProcessId id_ = 0;
    ProcessId group_ = 0;
    bool owns_ = false;
    std::optional<ExitStatus> status_;
};

} // namespace psx::os
