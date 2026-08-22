#include "cli_options.hpp"

#include "logger.hpp"

#include <string_view>

#ifndef PIPESHELLX_VERSION
#define PIPESHELLX_VERSION "0.0.0-dev"
#endif

CliOptions parseCliOptions(const std::vector<std::string>& args) {
    CliOptions options;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];

        if (arg == "--verbose" || arg == "-v") {
            options.verbose = true;
        } else if (arg == "--version") {
            options.showVersion = true;
        } else if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--log-file") {
            if (index + 1 >= args.size()) {
                throw CliParseError("--log-file requires a path");
            }
            options.logFile = args[++index];
        } else if (arg.starts_with("--log-file=")) {
            options.logFile = arg.substr(std::string_view("--log-file=").size());
            if (options.logFile.empty()) {
                throw CliParseError("--log-file requires a path");
            }
        } else {
            throw CliParseError("unknown argument: " + arg);
        }
    }

    return options;
}

std::string cliUsageText() {
    return "Usage: PipeShellX [options]\n"
           "\n"
           "Interactive shell that runs allowlisted commands locally or on every\n"
           "host listed in ./clients.txt over the system OpenSSH client.\n"
           "\n"
           "Options:\n"
           "  -v, --verbose          log at DEBUG level and mirror log lines to stderr\n"
           "      --log-file <path>  write the log to <path>\n"
           "                         (default: " +
           Logger::defaultLogFilePath() +
           ")\n"
           "  -h, --help             show this help and exit\n"
           "      --version          print the version and exit\n";
}

std::string cliVersionText() {
    return std::string("PipeShellX ") + PIPESHELLX_VERSION;
}
