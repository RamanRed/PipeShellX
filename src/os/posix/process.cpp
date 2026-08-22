#include "psx/os/process.hpp"
#include "psx/os/backend.hpp"

#include "posix_error.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <Availability.h>
#endif

extern char** environ;

#if defined(__GLIBC__)
#define PSX_GLIBC_AT_LEAST(major, minor) __GLIBC_PREREQ(major, minor)
#else
#define PSX_GLIBC_AT_LEAST(major, minor) 0
#endif

namespace psx::os {

namespace {

// POSIX.1-2024 standardised addchdir; macOS 26 and glibc 2.41 ship it and
// deprecate the _np spelling, which older Apple/glibc releases still need.
#if (defined(__APPLE__) && defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000) ||   \
    PSX_GLIBC_AT_LEAST(2, 41)
#define PSX_SPAWN_ADDCHDIR posix_spawn_file_actions_addchdir
#elif defined(__APPLE__) || PSX_GLIBC_AT_LEAST(2, 29)
#define PSX_SPAWN_ADDCHDIR posix_spawn_file_actions_addchdir_np
#endif

#if defined(PSX_SPAWN_ADDCHDIR)
constexpr bool kSpawnSupportsChdir = true;
#else
constexpr bool kSpawnSupportsChdir = false;
#endif

#if defined(__linux__)
constexpr bool kSpawnSupportsLimits = true; // prlimit() after spawn
#else
constexpr bool kSpawnSupportsLimits = false;
#endif

ExitStatus decode(int status) noexcept {
    if (WIFEXITED(status)) {
        return ExitStatus{ExitStatus::Kind::Exited, WEXITSTATUS(status)};
    }
    if (WIFSIGNALED(status)) {
        return ExitStatus{ExitStatus::Kind::Signaled, WTERMSIG(status)};
    }
    return ExitStatus{ExitStatus::Kind::Terminated, status};
}

bool hasLimits(const Limits& limits) noexcept {
    return limits.cpuSeconds.has_value() || limits.addressSpaceBytes.has_value() || limits.openHandles.has_value();
}

struct Argv {
    std::vector<char*> argv;
    std::vector<char*> envp;
    std::vector<std::string> envStorage;

    explicit Argv(const SpawnSpec& spec) {
        argv.reserve(spec.argv.size() + 1);
        for (const auto& arg : spec.argv) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        if (spec.env.has_value()) {
            envStorage = *spec.env;
            envp.reserve(envStorage.size() + 1);
            for (auto& entry : envStorage) {
                envp.push_back(entry.data());
            }
            envp.push_back(nullptr);
        }
    }

    char** environment() noexcept { return envp.empty() ? environ : envp.data(); }
};

int stdioSourceFd(const SpawnSpec::Stdio& stdio) noexcept {
    return stdio.handle != nullptr ? static_cast<int>(Backend::native(*stdio.handle)) : -1;
}

Result<void> validate(const SpawnSpec& spec) {
    if (spec.program.empty() || spec.argv.empty()) {
        return Error{ErrorClass::InvalidArgument, EINVAL, "spawn"};
    }
    if (!spec.cwd.empty()) {
        // Checked up front: some posix_spawn implementations report a failed
        // chdir action only after creating (and abandoning) the child.
        struct stat info{};
        if (::stat(spec.cwd.c_str(), &info) == -1) {
            return posix::fromErrno("spawn(cwd)", errno);
        }
        if (!S_ISDIR(info.st_mode)) {
            return Error{ErrorClass::NotFound, ENOTDIR, "spawn(cwd)"};
        }
        if (::access(spec.cwd.c_str(), X_OK) == -1) {
            return posix::fromErrno("spawn(cwd)", errno);
        }
    }
    for (const SpawnSpec::Stdio* stdio : {&spec.in, &spec.out, &spec.err}) {
        if (stdio->mode == SpawnSpec::Stdio::Mode::Handle &&
            (stdio->handle == nullptr || !stdio->handle->valid() || stdioSourceFd(*stdio) <= 2)) {
            return Error{ErrorClass::InvalidArgument, EBADF, "spawn(stdio)"};
        }
    }
    return {};
}

// ---------------------------------------------------------------- posix_spawn

struct SpawnAttributes {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attr{};
    bool actionsInit = false;
    bool attrInit = false;

    ~SpawnAttributes() {
        if (actionsInit) {
            posix_spawn_file_actions_destroy(&actions);
        }
        if (attrInit) {
            posix_spawnattr_destroy(&attr);
        }
    }
};

Result<void> addStdio(posix_spawn_file_actions_t& actions, const SpawnSpec::Stdio& stdio, int target) {
    switch (stdio.mode) {
        case SpawnSpec::Stdio::Mode::Inherit:
#if defined(__APPLE__)
            // POSIX_SPAWN_CLOEXEC_DEFAULT closes everything not listed here.
            if (const int rc = posix_spawn_file_actions_addinherit_np(&actions, target); rc != 0) {
                return posix::fromErrno("posix_spawn_file_actions_addinherit_np", rc);
            }
#endif
            return {};
        case SpawnSpec::Stdio::Mode::Null:
            if (const int rc = posix_spawn_file_actions_addopen(&actions, target, "/dev/null",
                                                                target == 0 ? O_RDONLY : O_WRONLY, 0);
                rc != 0) {
                return posix::fromErrno("posix_spawn_file_actions_addopen", rc);
            }
            return {};
        case SpawnSpec::Stdio::Mode::Handle:
            // dup2 clears FD_CLOEXEC on the target; the CLOEXEC source closes at exec.
            if (const int rc = posix_spawn_file_actions_adddup2(&actions, stdioSourceFd(stdio), target); rc != 0) {
                return posix::fromErrno("posix_spawn_file_actions_adddup2", rc);
            }
            return {};
    }
    return Error{ErrorClass::InvalidArgument, EINVAL, "spawn(stdio)"};
}

Result<pid_t> spawnViaPosixSpawn(const SpawnSpec& spec, Argv& argv) {
    SpawnAttributes attributes;
    if (const int rc = posix_spawn_file_actions_init(&attributes.actions); rc != 0) {
        return posix::fromErrno("posix_spawn_file_actions_init", rc);
    }
    attributes.actionsInit = true;
    if (const int rc = posix_spawnattr_init(&attributes.attr); rc != 0) {
        return posix::fromErrno("posix_spawnattr_init", rc);
    }
    attributes.attrInit = true;

    PSX_TRY(addStdio(attributes.actions, spec.in, STDIN_FILENO));
    PSX_TRY(addStdio(attributes.actions, spec.out, STDOUT_FILENO));
    PSX_TRY(addStdio(attributes.actions, spec.err, STDERR_FILENO));

    if (!spec.cwd.empty()) {
#if defined(PSX_SPAWN_ADDCHDIR)
        if (const int rc = PSX_SPAWN_ADDCHDIR(&attributes.actions, spec.cwd.c_str()); rc != 0) {
            return posix::fromErrno("posix_spawn_file_actions_addchdir", rc);
        }
#endif
    }
#if PSX_GLIBC_AT_LEAST(2, 34)
    // Belt and braces: every handle is CLOEXEC already.
    if (const int rc = posix_spawn_file_actions_addclosefrom_np(&attributes.actions, 3); rc != 0) {
        return posix::fromErrno("posix_spawn_file_actions_addclosefrom_np", rc);
    }
#endif

    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    sigset_t none;
    sigemptyset(&none);
    sigset_t all;
    sigfillset(&all);
    if (posix_spawnattr_setflags(&attributes.attr, flags) != 0 || posix_spawnattr_setpgroup(&attributes.attr, 0) != 0 ||
        posix_spawnattr_setsigmask(&attributes.attr, &none) != 0 ||
        posix_spawnattr_setsigdefault(&attributes.attr, &all) != 0) {
        return Error{ErrorClass::Other, errno, "posix_spawnattr"};
    }

    pid_t pid = -1;
    const bool lookup = spec.program.find('/') == std::string::npos;
    const int rc = lookup ? posix_spawnp(&pid, spec.program.c_str(), &attributes.actions, &attributes.attr,
                                         argv.argv.data(), argv.environment())
                          : posix_spawn(&pid, spec.program.c_str(), &attributes.actions, &attributes.attr,
                                        argv.argv.data(), argv.environment());
    if (rc != 0) {
        if (pid > 0) {
            // Darwin may have created a child that failed its file actions; never leave a zombie.
            int status = 0;
            while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
        }
        return posix::fromErrno(lookup ? "posix_spawnp" : "posix_spawn", rc);
    }
    return pid;
}

#if defined(__linux__)
Result<void> applyLimits(pid_t pid, const Limits& limits) {
    auto apply = [&](int resource, std::uint64_t value, const char* op) -> Result<void> {
        const rlimit limit{static_cast<rlim_t>(value), static_cast<rlim_t>(value)};
        if (::prlimit(pid, resource, &limit, nullptr) == -1) {
            return posix::fromErrno(op, errno);
        }
        return {};
    };
    if (limits.cpuSeconds) {
        PSX_TRY(apply(RLIMIT_CPU, *limits.cpuSeconds, "prlimit(RLIMIT_CPU)"));
    }
    if (limits.addressSpaceBytes) {
        PSX_TRY(apply(RLIMIT_AS, *limits.addressSpaceBytes, "prlimit(RLIMIT_AS)"));
    }
    if (limits.openHandles) {
        PSX_TRY(apply(RLIMIT_NOFILE, *limits.openHandles, "prlimit(RLIMIT_NOFILE)"));
    }
    return {};
}
#endif

// ---------------------------------------------------------------------- fork

// Used only for what posix_spawn cannot express on this platform (resource
// limits on Darwin, chdir on old glibc). Everything between fork() and exec
// is async-signal-safe: no allocation, no C++ exceptions.
[[noreturn]] void childFail(int errorFd, int err) noexcept {
    (void)::write(errorFd, &err, sizeof(err));
    ::_exit(127);
}

bool applyLimitInChild(int resource, const std::optional<std::uint64_t>& value) noexcept {
    if (!value) {
        return true;
    }
    const rlimit limit{static_cast<rlim_t>(*value), static_cast<rlim_t>(*value)};
    return ::setrlimit(resource, &limit) == 0;
}

void wireStdioInChild(const SpawnSpec::Stdio& stdio, int target, int errorFd) noexcept {
    switch (stdio.mode) {
        case SpawnSpec::Stdio::Mode::Inherit:
            return;
        case SpawnSpec::Stdio::Mode::Null: {
            const int fd = ::open("/dev/null", target == 0 ? O_RDONLY : O_WRONLY);
            if (fd == -1 || ::dup2(fd, target) == -1) {
                childFail(errorFd, errno);
            }
            ::close(fd);
            return;
        }
        case SpawnSpec::Stdio::Mode::Handle:
            if (::dup2(stdioSourceFd(stdio), target) == -1) {
                childFail(errorFd, errno);
            }
            return;
    }
}

Result<pid_t> spawnViaFork(const SpawnSpec& spec, Argv& argv) {
    int errorPipe[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(errorPipe, O_CLOEXEC) == -1) {
        return posix::fromErrno("pipe2", errno);
    }
#else
    if (::pipe(errorPipe) == -1) {
        return posix::fromErrno("pipe", errno);
    }
    ::fcntl(errorPipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC);
#endif

    const std::optional<std::uint64_t> cpu =
        spec.limits.cpuSeconds ? std::optional<std::uint64_t>(*spec.limits.cpuSeconds) : std::nullopt;
    const std::optional<std::uint64_t> handles =
        spec.limits.openHandles ? std::optional<std::uint64_t>(*spec.limits.openHandles) : std::nullopt;
    const bool lookup = spec.program.find('/') == std::string::npos;

    const pid_t pid = ::fork();
    if (pid == -1) {
        const int err = errno;
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        return posix::fromErrno("fork", err);
    }

    if (pid == 0) {
        const int errorFd = errorPipe[1];
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int sig = 1; sig < NSIG; ++sig) {
            if (sig != SIGKILL && sig != SIGSTOP) {
                ::signal(sig, SIG_DFL);
            }
        }
        if (::setpgid(0, 0) == -1) {
            childFail(errorFd, errno);
        }
        if (!spec.cwd.empty() && ::chdir(spec.cwd.c_str()) == -1) {
            childFail(errorFd, errno);
        }
        if (!applyLimitInChild(RLIMIT_CPU, cpu) || !applyLimitInChild(RLIMIT_AS, spec.limits.addressSpaceBytes) ||
            !applyLimitInChild(RLIMIT_NOFILE, handles)) {
            childFail(errorFd, errno);
        }
        wireStdioInChild(spec.in, STDIN_FILENO, errorFd);
        wireStdioInChild(spec.out, STDOUT_FILENO, errorFd);
        wireStdioInChild(spec.err, STDERR_FILENO, errorFd);
        environ = argv.environment();
        if (lookup) {
            ::execvp(spec.program.c_str(), argv.argv.data());
        } else {
            ::execv(spec.program.c_str(), argv.argv.data());
        }
        childFail(errorFd, errno);
    }

    ::close(errorPipe[1]);
    int childErrno = 0;
    ssize_t got = -1;
    do {
        got = ::read(errorPipe[0], &childErrno, sizeof(childErrno));
    } while (got == -1 && errno == EINTR);
    ::close(errorPipe[0]);

    if (got == static_cast<ssize_t>(sizeof(childErrno))) {
        // The child reported a failure and has exited; reap it so no zombie remains.
        int status = 0;
        while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
        return posix::fromErrno(lookup ? "execvp" : "execv", childErrno);
    }
    return pid; // pipe closed by exec: success
}

} // namespace

// ------------------------------------------------------------------- Process

Result<Process> Process::spawn(const SpawnSpec& spec) {
    PSX_TRY(validate(spec));
    Argv argv(spec);

    const bool needsFork =
        (hasLimits(spec.limits) && !kSpawnSupportsLimits) || (!spec.cwd.empty() && !kSpawnSupportsChdir);
    Result<pid_t> spawned = needsFork ? spawnViaFork(spec, argv) : spawnViaPosixSpawn(spec, argv);
    if (!spawned.ok()) {
        return spawned.error();
    }
    const pid_t pid = spawned.value();
    // Mirror the child's setpgid so kill(-pid) is valid immediately.
    (void)::setpgid(pid, pid);

#if defined(__linux__)
    if (hasLimits(spec.limits)) {
        if (auto applied = applyLimits(pid, spec.limits); !applied.ok()) {
            (void)::kill(-pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
            return applied.error();
        }
    }
#endif
    return Process(static_cast<ProcessId>(pid), static_cast<ProcessId>(pid));
}

Process::~Process() {
    if (owns_) {
        (void)::kill(-static_cast<pid_t>(group_), SIGKILL);
        (void)::kill(static_cast<pid_t>(id_), SIGKILL);
        int status = 0;
        while (::waitpid(static_cast<pid_t>(id_), &status, 0) == -1 && errno == EINTR) {}
    }
}

Process::Process(Process&& other) noexcept
    : id_(other.id_), group_(other.group_), owns_(std::exchange(other.owns_, false)), status_(other.status_) {}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        this->~Process();
        id_ = other.id_;
        group_ = other.group_;
        owns_ = std::exchange(other.owns_, false);
        status_ = other.status_;
    }
    return *this;
}

Result<void> Process::signal(StopSignal how) {
    if (!owns_) {
        return Error{ErrorClass::NoSuchProcess, ESRCH, "kill"};
    }
    const int sig = how == StopSignal::Kill ? SIGKILL : SIGTERM;
    if (::kill(-static_cast<pid_t>(group_), sig) == 0) {
        return {};
    }
    if (errno == ESRCH && ::kill(static_cast<pid_t>(id_), sig) == 0) {
        return {}; // the group is gone but the leader may still be reapable
    }
    return posix::fromErrno("kill", errno);
}

Result<ExitStatus> Process::wait() {
    if (status_) {
        return *status_;
    }
    if (!owns_) {
        return Error{ErrorClass::NoSuchProcess, ECHILD, "waitpid"};
    }
    int status = 0;
    while (::waitpid(static_cast<pid_t>(id_), &status, 0) == -1) {
        if (errno != EINTR) {
            return posix::fromErrno("waitpid", errno);
        }
    }
    owns_ = false;
    status_ = decode(status);
    return *status_;
}

Result<std::optional<ExitStatus>> Process::tryWait() {
    if (status_) {
        return std::optional<ExitStatus>(*status_);
    }
    if (!owns_) {
        return Error{ErrorClass::NoSuchProcess, ECHILD, "waitpid"};
    }
    int status = 0;
    pid_t result = -1;
    do {
        result = ::waitpid(static_cast<pid_t>(id_), &status, WNOHANG);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        return posix::fromErrno("waitpid", errno);
    }
    if (result == 0) {
        return std::optional<ExitStatus>{};
    }
    owns_ = false;
    status_ = decode(status);
    return std::optional<ExitStatus>(*status_);
}

ProcessId Process::release() noexcept {
    owns_ = false;
    return id_;
}

} // namespace psx::os
