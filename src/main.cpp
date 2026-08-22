#include "cli_options.hpp"
#include "logger.hpp"
#include "psx/cli/hosts_command.hpp"
#include "psx/cli/ping_command.hpp"
#include "psx/cli/run_command.hpp"
#include "psx/os/console.hpp"
#include "psx/os/io.hpp"
#include "psx/os/system.hpp"
#include "terminal_client.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

void initLogging(const CliOptions& options) {
    // A child that exits before reading our stdin must not kill the controller
    // with SIGPIPE; write() then returns BrokenPipe instead.
    (void)psx::os::ignoreBrokenPipeSignal();
    (void)psx::os::raiseHandleLimit();

    auto& logger = Logger::getInstance();
    logger.setLevel(options.verbose ? LogLevel::DEBUG : LogLevel::INFO);
    logger.setConsoleMirror(options.verbose);
    logger.setRotation(10ULL * 1024 * 1024, 5); // 10 MiB per file, keep 5 generations
    const std::string logFile = options.logFile.empty() ? Logger::defaultLogFilePath() : options.logFile;
    if (!logger.setLogFile(logFile)) {
        std::cerr << "PipeShellX: warning: cannot open " << logFile << "; logging to stderr instead\n";
    }
}

// Pulls a leading `-i FILE` / `--inventory FILE` out of `args` for `hosts`.
std::string takeInventoryPath(std::vector<std::string>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "-i" || args[i] == "--inventory") && i + 1 < args.size()) {
            const std::string path = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
            return path;
        }
    }
    return {};
}

int runShell(const CliOptions& options) {
    initLogging(options);
    Logger::getInstance().log(LogLevel::INFO, "Starting " + cliVersionText() + " terminal client");
    TerminalClient client;
    client.run();
    return 0;
}

const char* kTopUsage = R"(Usage: pipeshellx <command> [options]

Commands:
  run    [-i FILE] [-g GROUP|-t TAG|-H h1,h2] [--stream|--group|--json]
         [-c N] [--timeout S] [--overflow P] [--ring SIZE] [--policy FILE] [--no-color]
         -- <command...>                              run a command on hosts
  ping   [-i FILE] [-g GROUP|-t TAG|-H h1,h2] [--timeout S]   probe reachability
  hosts  [-i FILE]                                     list inventory hosts
  shell  [--verbose] [--log-file PATH]                 interactive REPL (default)

  --version   print the version and exit
  --help      show this help and exit
)";

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    try {
        if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
            std::cout << kTopUsage;
            return 0;
        }
        if (!args.empty() && args[0] == "--version") {
            std::cout << cliVersionText() << '\n';
            return 0;
        }

        if (!args.empty() && args[0] == "run") {
            initLogging(CliOptions{});
            try {
                const auto invocation = psx::cli::parseRun(std::vector<std::string>(args.begin() + 1, args.end()));
                return psx::cli::runSubcommand(invocation, std::cout, std::cerr,
                                               psx::os::isInteractive(psx::os::StandardStream::Output));
            } catch (const psx::cli::CliError& ex) {
                std::cerr << "pipeshellx run: " << ex.what() << "\n";
                return kExitUsage;
            }
        }

        if (!args.empty() && args[0] == "ping") {
            initLogging(CliOptions{});
            try {
                const auto invocation = psx::cli::parsePing(std::vector<std::string>(args.begin() + 1, args.end()));
                return psx::cli::pingSubcommand(invocation, std::cout, std::cerr);
            } catch (const psx::cli::CliError& ex) {
                std::cerr << "pipeshellx ping: " << ex.what() << "\n";
                return kExitUsage;
            }
        }

        if (!args.empty() && args[0] == "hosts") {
            std::vector<std::string> rest(args.begin() + 1, args.end());
            const std::string inventoryPath = takeInventoryPath(rest);
            if (!rest.empty()) {
                std::cerr << "pipeshellx hosts: unexpected argument '" << rest.front() << "'\n";
                return kExitUsage;
            }
            initLogging(CliOptions{});
            return psx::cli::hostsSubcommand(inventoryPath, std::cout, std::cerr);
        }

        // `shell` or no subcommand: the interactive REPL, with its own flags.
        std::vector<std::string> shellArgs = args;
        if (!shellArgs.empty() && shellArgs[0] == "shell") {
            shellArgs.erase(shellArgs.begin());
        }
        CliOptions options;
        try {
            options = parseCliOptions(shellArgs);
        } catch (const CliParseError& ex) {
            std::cerr << "PipeShellX: " << ex.what() << "\n\n" << kTopUsage;
            return kExitUsage;
        }
        if (options.showHelp) {
            std::cout << cliUsageText();
            return 0;
        }
        if (options.showVersion) {
            std::cout << cliVersionText() << '\n';
            return 0;
        }
        return runShell(options);
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
    } catch (...) {
        std::cerr << "Fatal error: unknown exception\n";
    }
    return 1;
}
