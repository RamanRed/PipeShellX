#pragma once

#include <stdexcept>
#include <string>
#include <vector>

// Command-line options of the interactive client (`PipeShellX [options]`).
struct CliOptions {
    bool verbose = false; // DEBUG level + mirror log lines to stderr
    bool showVersion = false;
    bool showHelp = false;
    std::string logFile; // empty -> Logger::defaultLogFilePath()
};

class CliParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Exit-code contract (PLAN.md §3.7): 2 = usage / configuration error.
inline constexpr int kExitUsage = 2;
inline constexpr int kExitCancelled = 130; // POSIX: 128 + SIGINT

// Parses argv[1..]; throws CliParseError on unknown or incomplete arguments.
CliOptions parseCliOptions(const std::vector<std::string>& args);

std::string cliUsageText();
std::string cliVersionText();
