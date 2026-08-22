#include "cli_options.hpp"
#include "logger.hpp"
#include "psx/os/io.hpp"
#include "psx/os/system.hpp"
#include "terminal_client.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        CliOptions options;
        try {
            options = parseCliOptions(std::vector<std::string>(argv + 1, argv + argc));
        } catch (const CliParseError& ex) {
            std::cerr << "PipeShellX: " << ex.what() << "\n\n" << cliUsageText();
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

        // A child that exits before reading our stdin must not kill the
        // controller with SIGPIPE; write() then returns BrokenPipe instead.
        (void)psx::os::ignoreBrokenPipeSignal();

        auto& logger = Logger::getInstance();
        logger.setLevel(options.verbose ? LogLevel::DEBUG : LogLevel::INFO);
        logger.setConsoleMirror(options.verbose);

        const std::string logFile = options.logFile.empty() ? Logger::defaultLogFilePath() : options.logFile;
        if (!logger.setLogFile(logFile)) {
            std::cerr << "PipeShellX: warning: cannot open " << logFile << "; logging to stderr instead\n";
        }

        logger.log(LogLevel::INFO, "Starting " + cliVersionText() + " terminal client (log file: " + logFile + ")");
        if (auto limit = psx::os::raiseHandleLimit(); limit.ok()) {
            logger.log(LogLevel::DEBUG, "Open-handle limit: soft " + std::to_string(limit.value().soft) + ", hard " +
                                            std::to_string(limit.value().hard));
        } else {
            logger.log(LogLevel::ERROR, "Could not raise the open-handle limit: " + limit.error().message());
        }
        TerminalClient client;
        client.run();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
    } catch (...) {
        std::cerr << "Fatal error: unknown exception\n";
    }

    return 1;
}
