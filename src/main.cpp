#include "cli_options.hpp"
#include "logger.hpp"
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

        auto& logger = Logger::getInstance();
        logger.setLevel(options.verbose ? LogLevel::DEBUG : LogLevel::INFO);
        logger.setConsoleMirror(options.verbose);

        const std::string logFile = options.logFile.empty() ? Logger::defaultLogFilePath() : options.logFile;
        if (!logger.setLogFile(logFile)) {
            std::cerr << "PipeShellX: warning: cannot open " << logFile << "; logging to stderr instead\n";
        }

        logger.log(LogLevel::INFO, "Starting " + cliVersionText() + " terminal client (log file: " + logFile + ")");
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
