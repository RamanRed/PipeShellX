#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

// Severity in increasing order; a message is emitted when its level is at
// least the configured threshold (see Logger::enabled).
enum class LogLevel { DEBUG, INFO, ERROR };

struct LogContext {
    std::int64_t pid = 0;
    std::string sessionId;
    std::string clientId;
    std::string command;
    std::string runId;   // unique token per run invocation (empty = not set)
    std::string stageId; // stage within the run (empty = not set)
};

class Logger {
public:
    static Logger& getInstance();

    void log(LogLevel level, const std::string& msg);
    void log(LogLevel level, const LogContext& context, const std::string& msg);

    // Directs output to `filename`, creating missing parent directories. An
    // empty name closes the current file so that lines fall back to stderr.
    // Returns false (never throws) when the file cannot be opened; logging then
    // continues on stderr so no message is lost (likewise for any line the file
    // later fails to accept). The caller decides whether and how to report it.
    bool setLogFile(const std::string& filename);

    // Minimum severity that is emitted. Default: INFO.
    void setLevel(LogLevel level) noexcept;
    LogLevel level() const noexcept;

    // When enabled every emitted line is also written to stderr (`--verbose`).
    void setConsoleMirror(bool enabled) noexcept;
    bool consoleMirror() const noexcept;

    // Size-based log rotation. When the current file passes `maxBytes` it is
    // renamed to `<path>.1` (shifting `.1`→`.2` … and dropping `.<keep>`), and
    // a fresh file is opened. maxBytes 0 (the default) disables rotation.
    void setRotation(std::uintmax_t maxBytes, int keep) noexcept;

    // True when a message of `level` would be emitted; lets callers skip
    // building expensive messages/contexts on hot paths.
    bool enabled(LogLevel level) const noexcept;

    // $XDG_STATE_HOME/pipeshellx/pipeshellx.log, else
    // $HOME/.local/state/pipeshellx/pipeshellx.log, else ./pipeshellx.log.
    static std::string defaultLogFilePath();

    // Non-copyable, non-movable singleton
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger();
    ~Logger();

    void rotateLocked(); // caller holds logMutex

    std::ofstream logFile;
    std::string logFilePath_;
    std::uintmax_t logFileBytes_ = 0;
    std::uintmax_t maxBytes_ = 0;
    int keepFiles_ = 5;
    std::mutex logMutex;
    std::atomic<LogLevel> currentLevel;
    std::atomic<bool> mirrorToConsole;

    static std::string getTimestamp();
    static std::string levelToString(LogLevel level);
};
