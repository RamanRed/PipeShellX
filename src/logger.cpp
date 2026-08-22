#include "logger.hpp"

#include "psx/os/paths.hpp"
#include "psx/os/system.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger::Logger() : currentLevel(LogLevel::INFO), mirrorToConsole(false) {}

Logger::~Logger() {
    if (logFile.is_open())
        logFile.close();
}

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

bool Logger::setLogFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile.is_open())
        logFile.close();
    if (filename.empty()) {
        return true;
    }

    const auto parent = std::filesystem::path(filename).parent_path();
    if (!parent.empty()) {
        std::error_code ignored; // a failure here surfaces as the open() below failing
        std::filesystem::create_directories(parent, ignored);
    }

    logFile.open(filename, std::ios::app);
    return logFile.is_open();
}

void Logger::setLevel(LogLevel level) noexcept {
    currentLevel.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
    return currentLevel.load(std::memory_order_relaxed);
}

void Logger::setConsoleMirror(bool enabled) noexcept {
    mirrorToConsole.store(enabled, std::memory_order_relaxed);
}

bool Logger::consoleMirror() const noexcept {
    return mirrorToConsole.load(std::memory_order_relaxed);
}

bool Logger::enabled(LogLevel level) const noexcept {
    return level >= currentLevel.load(std::memory_order_relaxed);
}

std::string Logger::defaultLogFilePath() {
    return (std::filesystem::path(psx::os::stateDirectory("pipeshellx")) / "pipeshellx.log").string();
}

std::string Logger::getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    std::ostringstream oss;
    oss << buf << "." << std::setfill('0') << std::setw(3) << static_cast<int>(ms.count());
    return oss.str();
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::ERROR:
            return "ERROR";
    }
    return "UNKNOWN";
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (!enabled(level)) {
        return;
    }
    log(level, LogContext{psx::os::currentProcessId(), "-", "-", "-"}, msg);
}

void Logger::log(LogLevel level, const LogContext& context, const std::string& msg) {
    if (!enabled(level)) {
        return;
    }

    std::ostringstream formatted;
    formatted << "[" << getTimestamp() << "] "
              << "[" << levelToString(level) << "] "
              << "[pid=" << context.pid << "] "
              << "[session=" << (context.sessionId.empty() ? "-" : context.sessionId) << "] "
              << "[client=" << (context.clientId.empty() ? "-" : context.clientId) << "] "
              << "[command=" << (context.command.empty() ? "-" : context.command) << "] " << msg;
    const std::string logMsg = formatted.str();

    std::lock_guard<std::mutex> lock(logMutex);
    bool wroteToFile = false;
    if (logFile.is_open()) {
        logFile << logMsg << std::endl;
        wroteToFile = logFile.good(); // a failed write/flush (ENOSPC, EIO) sticks: fall back from now on
    }
    if (!wroteToFile || consoleMirror()) {
        std::cerr << logMsg << std::endl;
    }
}
