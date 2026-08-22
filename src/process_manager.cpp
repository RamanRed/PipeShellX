#include "process_manager.hpp"

#include "logger.hpp"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <utility>

namespace {

struct RemoteWorkerState {
    ClientEntry client;
    pid_t pid = -1;
    pid_t logPid = -1;
    std::array<int, 2> stdoutPipe{-1, -1};
    std::array<int, 2> stderrPipe{-1, -1};
    std::string stdoutData;
    std::string stderrData;
    bool stdoutClosed = false;
    bool stderrClosed = false;
    bool childExited = false;
    bool timedOut = false;
    int status = 0;
};

void closeFd(int& fd) {
    if (fd != -1) {
        while (close(fd) == -1 && errno == EINTR) {
        }
        fd = -1;
    }
}

void closePipePair(std::array<int, 2>& pipePair) {
    closeFd(pipePair[0]);
    closeFd(pipePair[1]);
}

// Writes the whole buffer, retrying on EINTR. Safe to call after fork().
bool writeFully(int fd, const char* data, std::size_t remaining) {
    while (remaining > 0) {
        const ssize_t written = ::write(fd, data, remaining);
        if (written > 0) {
            data += written;
            remaining -= static_cast<std::size_t>(written);
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// Largest secret that is guaranteed to fit in a fresh pipe without blocking
// (every supported kernel has a pipe capacity of at least 4 KiB).
constexpr std::size_t kMaxPasswordBytes = 4096;

[[noreturn]] void childExitWithError(const std::string& message, int code) {
    static_cast<void>(writeFully(STDERR_FILENO, message.data(), message.size()));
    _exit(code);
}

// After SIGKILLing a timed-out process group, how long to keep draining its
// pipes before giving up on a holder outside the group (a daemonised
// grandchild) that would otherwise keep the run alive indefinitely.
constexpr int kDrainGraceSec = 2;

// Absolute deadline derived from a timeout in seconds ("no timeout" is the
// far-future time point). Both poll loops use it so that the rule "an idle
// poll() is not a timeout; only the clock is" lives in exactly one place.
class Deadline {
public:
    // "No deadline": expired() is never true and poll() blocks (or idles).
    static Deadline none() { return after(0); }

    static Deadline after(int timeoutSec) {
        return Deadline{timeoutSec > 0 ? std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec)
                                       : std::chrono::steady_clock::time_point::max()};
    }

    bool expired() const {
        return at_ != std::chrono::steady_clock::time_point::max() && std::chrono::steady_clock::now() >= at_;
    }

    // poll() timeout in milliseconds: -1 (block) without a deadline, else the
    // remaining time. With nothing to poll (`idle`: pipes at EOF, child not yet
    // reaped) the wait is capped so the loop keeps checking waitpid().
    int pollTimeoutMs(bool idle) const {
        int timeoutMs = -1;
        if (at_ != std::chrono::steady_clock::time_point::max()) {
            const auto now = std::chrono::steady_clock::now();
            timeoutMs = now >= at_ ? 0
                                   : static_cast<int>(
                                         std::chrono::duration_cast<std::chrono::milliseconds>(at_ - now).count());
        }
        if (idle) {
            // Never 0 here: an expired deadline with nothing to poll would spin.
            timeoutMs = timeoutMs == -1 ? kIdlePollMs : std::clamp(timeoutMs, 1, kIdlePollMs);
        }
        return timeoutMs;
    }

private:
    static constexpr int kIdlePollMs = 50;
    explicit Deadline(std::chrono::steady_clock::time_point at) : at_(at) {}
    std::chrono::steady_clock::time_point at_;
};

} // namespace

ProcessManager::ProcessManager()
    : childPid(-1), stdinPipe{-1, -1}, stdoutPipe{-1, -1}, stderrPipe{-1, -1} {
    installSigChldHandler();
}

ProcessManager::~ProcessManager() {
    try {
        reapChild(true);
        closePipes();
    } catch (...) {
    }
}

ProcessManager::ProcessManager(ProcessManager&& other) noexcept
    : childPid(other.childPid) {
    stdinPipe[0] = other.stdinPipe[0];
    stdinPipe[1] = other.stdinPipe[1];
    stdoutPipe[0] = other.stdoutPipe[0];
    stdoutPipe[1] = other.stdoutPipe[1];
    stderrPipe[0] = other.stderrPipe[0];
    stderrPipe[1] = other.stderrPipe[1];

    other.childPid = -1;
    other.stdinPipe[0] = other.stdinPipe[1] = -1;
    other.stdoutPipe[0] = other.stdoutPipe[1] = -1;
    other.stderrPipe[0] = other.stderrPipe[1] = -1;
}

ProcessManager& ProcessManager::operator=(ProcessManager&& other) noexcept {
    if (this != &other) {
        closePipes();

        childPid = other.childPid;
        stdinPipe[0] = other.stdinPipe[0];
        stdinPipe[1] = other.stdinPipe[1];
        stdoutPipe[0] = other.stdoutPipe[0];
        stdoutPipe[1] = other.stdoutPipe[1];
        stderrPipe[0] = other.stderrPipe[0];
        stderrPipe[1] = other.stderrPipe[1];

        other.childPid = -1;
        other.stdinPipe[0] = other.stdinPipe[1] = -1;
        other.stdoutPipe[0] = other.stdoutPipe[1] = -1;
        other.stderrPipe[0] = other.stderrPipe[1] = -1;
    }

    return *this;
}

void ProcessManager::setupPipes() {
    if (pipe(stdinPipe.data()) == -1) {
        throw std::runtime_error("stdin pipe creation failed: " + std::string(std::strerror(errno)));
    }
    if (pipe(stdoutPipe.data()) == -1) {
        closePipes();
        throw std::runtime_error("stdout pipe creation failed: " + std::string(std::strerror(errno)));
    }
    if (pipe(stderrPipe.data()) == -1) {
        closePipes();
        throw std::runtime_error("stderr pipe creation failed: " + std::string(std::strerror(errno)));
    }
}

void ProcessManager::closePipes() {
    closeFd(stdinPipe[0]);
    closeFd(stdinPipe[1]);
    closeFd(stdoutPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[0]);
    closeFd(stderrPipe[1]);
}

void ProcessManager::setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        throw std::runtime_error("fcntl(F_GETFL) failed: " + std::string(std::strerror(errno)));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl(F_SETFL) failed: " + std::string(std::strerror(errno)));
    }
}

void ProcessManager::installSigChldHandler() {
    static std::once_flag once;
    static std::exception_ptr installError;

    std::call_once(once, [] {
        struct sigaction action {};
        action.sa_handler = &ProcessManager::sigChldHandler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_NOCLDSTOP;

        if (sigaction(SIGCHLD, &action, nullptr) == -1) {
            installError = std::make_exception_ptr(
                std::runtime_error("sigaction(SIGCHLD) failed: " + std::string(std::strerror(errno)))
            );
        }
    });

    if (installError) {
        std::rethrow_exception(installError);
    }
}

void ProcessManager::sigChldHandler(int signo) {
    (void)signo;
}

void ProcessManager::reapChild(bool terminateProcessGroup) noexcept {
    if (childPid <= 0) {
        return;
    }

    if (terminateProcessGroup) {
        (void)kill(-childPid, SIGKILL);
    }

    int status = 0;
    while (waitpid(childPid, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }

    childPid = -1;
}

bool ProcessManager::readAvailableData(int fd, std::string& output, bool& closed) {
    std::array<char, 4096> buffer{};
    bool madeProgress = false;

    while (true) {
        const ssize_t readCount = ::read(fd, buffer.data(), buffer.size());
        if (readCount > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(readCount));
            madeProgress = true;
            continue;
        }
        if (readCount == 0) {
            closed = true;
            return madeProgress;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return madeProgress;
        }
        throw std::runtime_error("read from pipe failed: " + std::string(std::strerror(errno)));
    }
}

bool ProcessManager::writeAvailableData(int fd,
                                        const std::string& input,
                                        std::size_t& written,
                                        bool& closed) {
    bool madeProgress = false;

    while (written < input.size()) {
        const ssize_t writeCount = ::write(fd, input.data() + written, input.size() - written);
        if (writeCount > 0) {
            written += static_cast<std::size_t>(writeCount);
            madeProgress = true;
            continue;
        }
        if (writeCount == -1 && errno == EINTR) {
            continue;
        }
        if (writeCount == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return madeProgress;
        }
        if (writeCount == -1 && errno == EPIPE) {
            closed = true;
            return madeProgress;
        }
        throw std::runtime_error("write to child stdin failed: " + std::string(std::strerror(errno)));
    }

    closed = true;
    return true;
}

std::string ProcessManager::formatClientResults(const std::vector<ClientResult>& clientResults, bool useStdout) const {
    std::string formatted;
    for (const auto& result : clientResults) {
        const std::string& selectedOutput = useStdout ? result.stdoutData : result.errorMessage;
        if (selectedOutput.empty()) {
            continue;
        }

        formatted += "CLIENT " + result.clientId + "\n";
        formatted += selectedOutput;
        if (formatted.back() != '\n') {
            formatted += '\n';
        }
    }
    return formatted;
}

std::string ProcessManager::classifyRemoteError(const ClientResult& clientResult) const {
    if (clientResult.timedOut) {
        return "ERROR: command timed out";
    }

    // Messages written by our own worker child (childExitWithError) have a
    // fixed spelling; everything else on stderr came from ssh/sshpass.
    const std::string& stderrText = clientResult.stderrData;
    if (stderrText.find("execvp failed") != std::string::npos) {
        return "ERROR: command execution failed";
    }
    if (stderrText.find("dup2 failed") != std::string::npos ||
        stderrText.find("open(/dev/null) failed") != std::string::npos ||
        stderrText.find("setpgid failed") != std::string::npos ||
        stderrText.find("password pipe") != std::string::npos ||
        stderrText.find("password too long") != std::string::npos) {
        return "ERROR: remote worker setup failed";
    }

    if (const auto sshFailure = classifySshFailure(stderrText); sshFailure.has_value()) {
        return "ERROR: " + *sshFailure;
    }

    if (clientResult.exitCode != 0) {
        return "ERROR: command failed with exit code " + std::to_string(clientResult.exitCode);
    }

    return {};
}

ProcessManager::Result ProcessManager::execute(const std::vector<std::string>& args,
                                               const LogContext& context,
                                               const std::string& input,
                                               int timeoutSec) {
    if (args.empty()) {
        throw std::runtime_error("cannot execute empty command");
    }

    Logger::getInstance().log(LogLevel::DEBUG, context, "Creating IPC pipes");
    setupPipes();

    childPid = fork();
    if (childPid == -1) {
        closePipes();
        Logger::getInstance().log(LogLevel::ERROR, context, "fork() failed: " + std::string(std::strerror(errno)));
        throw std::runtime_error("fork() failed: " + std::string(std::strerror(errno)));
    }

    if (childPid == 0) try {
        if (setpgid(0, 0) == -1) {
            childExitWithError("setpgid failed: " + std::string(std::strerror(errno)) + "\n", 126);
        }

        if (dup2(stdinPipe[0], STDIN_FILENO) == -1 ||
            dup2(stdoutPipe[1], STDOUT_FILENO) == -1 ||
            dup2(stderrPipe[1], STDERR_FILENO) == -1) {
            childExitWithError("dup2 failed: " + std::string(std::strerror(errno)) + "\n", 126);
        }

        closePipes();

        const rlimit cpuLimit{5, 5};
        const rlimit memLimit{64 * 1024 * 1024, 64 * 1024 * 1024};
        (void)setrlimit(RLIMIT_CPU, &cpuLimit);
        (void)setrlimit(RLIMIT_AS, &memLimit);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        childExitWithError(
            "execvp failed for '" + args[0] + "': " + std::string(std::strerror(errno)) + "\n",
            127
        );
    } catch (...) {
        // Never let an exception unwind the fork()ed copy of the parent's frames.
        childExitWithError("worker setup failed: unexpected exception\n", 126);
    }

    // Mirror the child's setpgid() so that kill(-processGroup) is valid even
    // before the child has run; processGroup outlives childPid (reset on reap).
    (void)setpgid(childPid, childPid);
    const pid_t processGroup = childPid;

    Logger::getInstance().log(
        LogLevel::INFO,
        LogContext{childPid, context.sessionId, context.clientId, context.command},
        "Child process created"
    );

    closeFd(stdinPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[1]);

    try {
        try {
            setNonBlocking(stdoutPipe[0]);
            setNonBlocking(stderrPipe[0]);
            if (stdinPipe[1] != -1) {
                setNonBlocking(stdinPipe[1]);
            }
        } catch (...) {
            reapChild(true);
            closePipes();
            throw;
        }

        std::string stdoutData;
        std::string stderrData;
        std::size_t inputWritten = 0;
        bool stdinClosed = input.empty();
        bool stdoutClosed = false;
        bool stderrClosed = false;
        bool timedOut = false;
        bool childExited = false;
        int status = 0;

        if (stdinClosed) {
            closeFd(stdinPipe[1]);
            Logger::getInstance().log(LogLevel::DEBUG, context, "Closed stdin pipe after sending input");
        }

        const auto deadline = Deadline::after(timeoutSec);
        Deadline drainDeadline = Deadline::none(); // armed once the deadline fires

        while (!childExited || !stdoutClosed || !stderrClosed) {
            std::vector<pollfd> pollFds;
            pollFds.reserve(3);

            if (!stdinClosed && stdinPipe[1] != -1) {
                pollFds.push_back(pollfd{stdinPipe[1], POLLOUT, 0});
            }
            if (!stdoutClosed && stdoutPipe[0] != -1) {
                pollFds.push_back(pollfd{stdoutPipe[0], POLLIN | POLLHUP, 0});
            }
            if (!stderrClosed && stderrPipe[0] != -1) {
                pollFds.push_back(pollfd{stderrPipe[0], POLLIN | POLLHUP, 0});
            }

            const int pollTimeoutMs = (timedOut ? drainDeadline : deadline).pollTimeoutMs(pollFds.empty());
            const int pollResult = poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), pollTimeoutMs);
            if (pollResult == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("poll failed: " + std::string(std::strerror(errno)));
            }

            if (!timedOut && deadline.expired()) {
                timedOut = true;
                (void)kill(-processGroup, SIGKILL);
                Logger::getInstance().log(LogLevel::ERROR, context, "Command timed out; sent SIGKILL to process group");
                drainDeadline = Deadline::after(kDrainGraceSec);
            } else if (timedOut && drainDeadline.expired() && (!stdinClosed || !stdoutClosed || !stderrClosed)) {
                // Something outside the process group still holds our pipes; stop waiting for it.
                closeFd(stdinPipe[1]);
                stdinClosed = true;
                closeFd(stdoutPipe[0]);
                stdoutClosed = true;
                closeFd(stderrPipe[0]);
                stderrClosed = true;
                Logger::getInstance().log(LogLevel::ERROR, context,
                                          "Pipes still open after the SIGKILL grace period; abandoning remaining output");
            }

            for (const auto& pollFd : pollFds) {
                if ((pollFd.revents & (POLLERR | POLLNVAL)) != 0) {
                    throw std::runtime_error("poll reported invalid pipe state");
                }

                if (pollFd.fd == stdinPipe[1] && (pollFd.revents & POLLOUT) != 0) {
                    if (writeAvailableData(stdinPipe[1], input, inputWritten, stdinClosed) && stdinClosed) {
                        closeFd(stdinPipe[1]);
                        Logger::getInstance().log(LogLevel::DEBUG, context, "Finished writing command input to child stdin");
                    }
                }

                if (pollFd.fd == stdoutPipe[0] && (pollFd.revents & (POLLIN | POLLHUP)) != 0) {
                    const std::size_t before = stdoutData.size();
                    if (readAvailableData(stdoutPipe[0], stdoutData, stdoutClosed) && stdoutClosed) {
                        closeFd(stdoutPipe[0]);
                    } else if (stdoutClosed) {
                        closeFd(stdoutPipe[0]);
                    }
                    if (stdoutData.size() > before && Logger::getInstance().enabled(LogLevel::DEBUG)) {
                        Logger::getInstance().log(
                            LogLevel::DEBUG,
                            context,
                            "Read " + std::to_string(stdoutData.size() - before) + " bytes from child stdout"
                        );
                    }
                }

                if (pollFd.fd == stderrPipe[0] && (pollFd.revents & (POLLIN | POLLHUP)) != 0) {
                    const std::size_t before = stderrData.size();
                    if (readAvailableData(stderrPipe[0], stderrData, stderrClosed) && stderrClosed) {
                        closeFd(stderrPipe[0]);
                    } else if (stderrClosed) {
                        closeFd(stderrPipe[0]);
                    }
                    if (stderrData.size() > before && Logger::getInstance().enabled(LogLevel::DEBUG)) {
                        Logger::getInstance().log(
                            LogLevel::DEBUG,
                            context,
                            "Read " + std::to_string(stderrData.size() - before) + " bytes from child stderr"
                        );
                    }
                }
            }

            if (!childExited && childPid > 0) {
                const pid_t waitResult = waitpid(childPid, &status, WNOHANG);
                if (waitResult == childPid) {
                    childExited = true;
                    if (Logger::getInstance().enabled(LogLevel::DEBUG)) {
                        Logger::getInstance().log(
                            LogLevel::DEBUG,
                            LogContext{waitResult, context.sessionId, context.clientId, context.command},
                            "Child process reaped with waitpid"
                        );
                    }
                    childPid = -1;
                } else if (waitResult == -1) {
                    Logger::getInstance().log(LogLevel::ERROR, context, "waitpid failed: " + std::string(std::strerror(errno)));
                    throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
                }
            }
        }

        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        Logger::getInstance().log(
            exitCode == 0 ? LogLevel::INFO : LogLevel::ERROR,
            context,
            "IPC collection completed: stdout=" + std::to_string(stdoutData.size()) +
                " bytes, stderr=" + std::to_string(stderrData.size()) + " bytes"
        );
        return Result{exitCode, std::move(stdoutData), std::move(stderrData), timedOut, {}};
    } catch (...) {
        Logger::getInstance().log(LogLevel::ERROR, context, "Process execution failed; cleaning up child and pipes");
        reapChild(true);
        closePipes();
        throw;
    }
}

ProcessManager::Result ProcessManager::executeRemote(const std::vector<ClientEntry>& clients,
                                                     const std::string& remoteCommand,
                                                     const LogContext& context,
                                                     int timeoutSec) {
    if (clients.empty()) {
        throw std::runtime_error("no clients configured for remote execution");
    }

    std::vector<RemoteWorkerState> workers;
    workers.reserve(clients.size());

    auto cleanupWorkers = [&]() noexcept {
        for (auto& worker : workers) {
            closePipePair(worker.stdoutPipe);
            closePipePair(worker.stderrPipe);
            if (worker.pid > 0) {
                (void)kill(-worker.pid, SIGKILL);
                while (waitpid(worker.pid, &worker.status, 0) == -1 && errno == EINTR) {
                }
                worker.pid = -1;
            }
        }
    };

    try {
        for (const auto& client : clients) {
            RemoteWorkerState worker;
            worker.client = client;

            if (pipe(worker.stdoutPipe.data()) == -1) {
                throw std::runtime_error("stdout pipe creation failed for " + client.clientId() + ": " + std::string(std::strerror(errno)));
            }
            if (pipe(worker.stderrPipe.data()) == -1) {
                closePipePair(worker.stdoutPipe);
                throw std::runtime_error("stderr pipe creation failed for " + client.clientId() + ": " + std::string(std::strerror(errno)));
            }

            const pid_t pid = fork();
            if (pid == -1) {
                closePipePair(worker.stdoutPipe);
                closePipePair(worker.stderrPipe);
                throw std::runtime_error("fork() failed for " + client.clientId() + ": " + std::string(std::strerror(errno)));
            }

            if (pid == 0) try {
                for (auto& existingWorker : workers) {
                    closePipePair(existingWorker.stdoutPipe);
                    closePipePair(existingWorker.stderrPipe);
                }

                if (setpgid(0, 0) == -1) {
                    childExitWithError("setpgid failed: " + std::string(std::strerror(errno)) + "\n", 126);
                }

                int nullFd = open("/dev/null", O_RDONLY);
                if (nullFd == -1) {
                    childExitWithError("open(/dev/null) failed: " + std::string(std::strerror(errno)) + "\n", 126);
                }

                if (dup2(nullFd, STDIN_FILENO) == -1 ||
                    dup2(worker.stdoutPipe[1], STDOUT_FILENO) == -1 ||
                    dup2(worker.stderrPipe[1], STDERR_FILENO) == -1) {
                    childExitWithError("dup2 failed: " + std::string(std::strerror(errno)) + "\n", 126);
                }

                closeFd(nullFd);
                closePipePair(worker.stdoutPipe);
                closePipePair(worker.stderrPipe);

                // Passwords never touch argv: sshpass reads the secret from an
                // inherited pipe (`sshpass -d <fd>`). The pipe is created in the
                // child so that no other worker can ever inherit it; the read end
                // is deliberately left inheritable across exec.
                int passwordFd = -1;
                if (!client.password.empty()) {
                    if (client.password.size() >= kMaxPasswordBytes) {
                        childExitWithError("password too long for secure hand-off\n", 126);
                    }
                    std::array<int, 2> passwordPipe{-1, -1};
                    if (pipe(passwordPipe.data()) == -1) {
                        childExitWithError("password pipe creation failed: " + std::string(std::strerror(errno)) + "\n", 126);
                    }
                    if (!writeFully(passwordPipe[1], client.password.data(), client.password.size()) ||
                        !writeFully(passwordPipe[1], "\n", 1)) {
                        childExitWithError("password pipe write failed: " + std::string(std::strerror(errno)) + "\n", 126);
                    }
                    closeFd(passwordPipe[1]);
                    passwordFd = passwordPipe[0];
                }

                std::vector<std::string> sshArgs = buildSshCommandArguments(client, remoteCommand, passwordFd);

                std::vector<char*> argv;
                argv.reserve(sshArgs.size() + 1);
                for (auto& arg : sshArgs) {
                    argv.push_back(arg.data());
                }
                argv.push_back(nullptr);

                execvp(argv[0], argv.data());
                childExitWithError(
                    "execvp failed for ssh client '" + client.clientId() + "': " + std::string(std::strerror(errno)) + "\n",
                    127
                );
            } catch (...) {
                // A throw here would run the parent's cleanupWorkers() inside the
                // child and SIGKILL every sibling worker's process group.
                childExitWithError("worker setup failed: unexpected exception\n", 126);
            }

            (void)setpgid(pid, pid); // see execute(): makes kill(-pid) valid immediately
            worker.pid = pid;
            worker.logPid = pid; // keeps the process-group id after worker.pid is reset on reap
            Logger::getInstance().log(
                LogLevel::INFO,
                LogContext{pid, context.sessionId, client.clientId(), "ssh " + client.clientId() + " " + remoteCommand},
                "Remote SSH worker created"
            );

            closeFd(worker.stdoutPipe[1]);
            closeFd(worker.stderrPipe[1]);
            setNonBlocking(worker.stdoutPipe[0]);
            setNonBlocking(worker.stderrPipe[0]);
            workers.push_back(std::move(worker));
        }

        const auto deadline = Deadline::after(timeoutSec);
        Deadline drainDeadline = Deadline::none(); // armed once the deadline fires

        bool anyTimedOut = false;

        while (true) {
            bool allComplete = true;
            std::vector<pollfd> pollFds;
            std::vector<std::pair<std::size_t, bool>> pollMap;

            for (std::size_t index = 0; index < workers.size(); ++index) {
                auto& worker = workers[index];
                if (!worker.childExited || !worker.stdoutClosed || !worker.stderrClosed) {
                    allComplete = false;
                }
                if (!worker.stdoutClosed && worker.stdoutPipe[0] != -1) {
                    pollFds.push_back(pollfd{worker.stdoutPipe[0], POLLIN | POLLHUP, 0});
                    pollMap.emplace_back(index, true);
                }
                if (!worker.stderrClosed && worker.stderrPipe[0] != -1) {
                    pollFds.push_back(pollfd{worker.stderrPipe[0], POLLIN | POLLHUP, 0});
                    pollMap.emplace_back(index, false);
                }
            }

            if (allComplete) {
                break;
            }

            const int pollTimeoutMs = (anyTimedOut ? drainDeadline : deadline).pollTimeoutMs(pollFds.empty());

            const int pollResult = poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), pollTimeoutMs);
            if (pollResult == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("poll failed during remote execution: " + std::string(std::strerror(errno)));
            }

            if (!anyTimedOut && deadline.expired()) {
                anyTimedOut = true;
                for (auto& worker : workers) {
                    if (worker.childExited && worker.stdoutClosed && worker.stderrClosed) {
                        continue; // finished in time
                    }
                    worker.timedOut = true;
                    if (worker.logPid > 0) {
                        (void)kill(-worker.logPid, SIGKILL);
                    }
                    Logger::getInstance().log(
                        LogLevel::ERROR,
                        LogContext{worker.logPid, context.sessionId, worker.client.clientId(), "ssh " + worker.client.clientId() + " " + remoteCommand},
                        "Remote SSH worker timed out; sent SIGKILL to process group"
                    );
                }
                drainDeadline = Deadline::after(kDrainGraceSec);
            } else if (anyTimedOut && drainDeadline.expired()) {
                for (auto& worker : workers) {
                    if (worker.stdoutClosed && worker.stderrClosed) {
                        continue;
                    }
                    closeFd(worker.stdoutPipe[0]);
                    worker.stdoutClosed = true;
                    closeFd(worker.stderrPipe[0]);
                    worker.stderrClosed = true;
                    Logger::getInstance().log(
                        LogLevel::ERROR,
                        LogContext{worker.logPid, context.sessionId, worker.client.clientId(), "ssh " + worker.client.clientId() + " " + remoteCommand},
                        "Pipes still open after the SIGKILL grace period; abandoning remaining output"
                    );
                }
            }

            for (std::size_t index = 0; index < pollFds.size(); ++index) {
                const auto [workerIndex, isStdout] = pollMap[index];
                auto& worker = workers[workerIndex];
                auto& targetOutput = isStdout ? worker.stdoutData : worker.stderrData;
                auto& targetClosed = isStdout ? worker.stdoutClosed : worker.stderrClosed;
                int& targetFd = isStdout ? worker.stdoutPipe[0] : worker.stderrPipe[0];

                if ((pollFds[index].revents & (POLLERR | POLLNVAL)) != 0) {
                    throw std::runtime_error("poll reported invalid pipe state for client " + worker.client.clientId());
                }

                if ((pollFds[index].revents & (POLLIN | POLLHUP)) != 0) {
                    const std::size_t before = targetOutput.size();
                    if (readAvailableData(targetFd, targetOutput, targetClosed) && targetClosed) {
                        closeFd(targetFd);
                    } else if (targetClosed) {
                        closeFd(targetFd);
                    }

                    if (targetOutput.size() > before && Logger::getInstance().enabled(LogLevel::DEBUG)) {
                        Logger::getInstance().log(
                            LogLevel::DEBUG,
                            LogContext{worker.logPid, context.sessionId, worker.client.clientId(), "ssh " + worker.client.clientId() + " " + remoteCommand},
                            "Read " + std::to_string(targetOutput.size() - before) +
                                (isStdout ? " bytes from remote stdout" : " bytes from remote stderr")
                        );
                    }
                }
            }

            for (auto& worker : workers) {
                if (!worker.childExited && worker.pid > 0) {
                    const pid_t waitResult = waitpid(worker.pid, &worker.status, WNOHANG);
                    if (waitResult == worker.pid) {
                        worker.childExited = true;
                        if (Logger::getInstance().enabled(LogLevel::DEBUG)) {
                            Logger::getInstance().log(
                                LogLevel::DEBUG,
                                LogContext{waitResult, context.sessionId, worker.client.clientId(), "ssh " + worker.client.clientId() + " " + remoteCommand},
                                "Remote SSH worker reaped with waitpid"
                            );
                        }
                        worker.pid = -1;
                    } else if (waitResult == -1) {
                        throw std::runtime_error("waitpid failed for client " + worker.client.clientId() + ": " + std::string(std::strerror(errno)));
                    }
                }
            }
        }

        std::vector<ClientResult> clientResults;
        clientResults.reserve(workers.size());
        int overallExitCode = 0;

        for (auto& worker : workers) {
            closePipePair(worker.stdoutPipe);
            closePipePair(worker.stderrPipe);

            const int exitCode = WIFEXITED(worker.status) ? WEXITSTATUS(worker.status) : -1;
            if (exitCode != 0 || worker.timedOut) {
                overallExitCode = exitCode == 0 ? 1 : exitCode;
            }

            clientResults.push_back(ClientResult{
                worker.client.clientId(),
                exitCode,
                std::move(worker.stdoutData),
                std::move(worker.stderrData),
                {},
                worker.timedOut
            });
        }

        for (auto& clientResult : clientResults) {
            clientResult.errorMessage = classifyRemoteError(clientResult);
            if (!clientResult.errorMessage.empty()) {
                Logger::getInstance().log(
                    LogLevel::ERROR,
                    LogContext{context.pid, context.sessionId, clientResult.clientId, "ssh " + clientResult.clientId + " " + remoteCommand},
                    clientResult.errorMessage
                );
            }
        }

        return Result{
            overallExitCode,
            formatClientResults(clientResults, true),
            formatClientResults(clientResults, false),
            anyTimedOut,
            std::move(clientResults)
        };
    } catch (...) {
        cleanupWorkers();
        throw;
    }
}
