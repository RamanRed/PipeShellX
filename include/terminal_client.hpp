#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "client_manager.hpp"
#include "command_executor.hpp"
#include "process_manager.hpp"

class TerminalClient {
public:
    TerminalClient();
    ~TerminalClient();

    void run();

private:
    std::vector<std::string> history;
    bool running;
    std::mutex outputMutex;
    ClientManager clientManager;

    void printPrompt();
    void printColored(const std::string& msg, const std::string& color);
    void handleCommand(const std::string& command);
    bool handleClientCommand(const std::string& command);
    void printError(const std::string& msg);
    void printHistory();
    void printHelp();
    void printClients();
    void printStatusTable();
    void refreshClientStatuses(const std::vector<ProcessManager::ClientResult>& clientResults);
    // Runs `exec` synchronously on the REPL thread (no background thread) and
    // reports the outcome: refresh statuses, print a failure line on a non-zero
    // exit, and map any exception to a user message plus a logged error tagged
    // with `clientId`/`command`.
    void executeAndReport(const std::function<CommandResult()>& exec,
                          const std::string& sessionId,
                          const std::string& command,
                          const std::string& clientId,
                          const char* context);
    void handleExit();
    bool promptPasswordRequired();
    std::optional<std::string> promptPasswordSecurely();

    // Color codes
    const std::string COLOR_RESET = "\033[0m";
    const std::string COLOR_GREEN = "\033[32m";
    const std::string COLOR_RED = "\033[31m";
    const std::string COLOR_YELLOW = "\033[33m";
    const std::string COLOR_BLUE = "\033[34m";
};
