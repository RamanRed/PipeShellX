#include "psx/cli/ping_command.hpp"

#include "logger.hpp"
#include "process_manager.hpp"
#include "psx/cli/selection.hpp"
#include "psx/os/system.hpp"

#include <algorithm>
#include <charconv>

namespace psx::cli {

namespace {

std::string valueFor(const std::vector<std::string>& args, std::size_t& i, const std::string& flag) {
    if (i + 1 >= args.size()) {
        throw CliError(flag + " requires a value");
    }
    return args[++i];
}

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) {
            out.push_back(item);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

void setSelector(Selector& selector, SelectorKind kind, const std::string& raw) {
    if (selector.kind != SelectorKind::All) {
        throw CliError("selectors -g/-t/-H are mutually exclusive");
    }
    selector.kind = kind;
    if (kind == SelectorKind::Hosts) {
        selector.hosts = splitCsv(raw);
    } else {
        selector.value = raw;
    }
}

} // namespace

PingInvocation parsePing(const std::vector<std::string>& args) {
    PingInvocation invocation;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-g" || arg == "--group-name") {
            setSelector(invocation.selector, SelectorKind::Group, valueFor(args, i, arg));
        } else if (arg == "-t" || arg == "--tag") {
            setSelector(invocation.selector, SelectorKind::Tag, valueFor(args, i, arg));
        } else if (arg == "-H" || arg == "--hosts") {
            setSelector(invocation.selector, SelectorKind::Hosts, valueFor(args, i, arg));
        } else if (arg == "-i" || arg == "--inventory") {
            invocation.inventoryPath = valueFor(args, i, arg);
        } else if (arg == "--timeout") {
            const std::string value = valueFor(args, i, arg);
            int seconds = 0;
            const auto* end = value.data() + value.size();
            const auto [ptr, ec] = std::from_chars(value.data(), end, seconds);
            if (ec != std::errc{} || ptr != end || seconds < 0) {
                throw CliError("--timeout expects a non-negative integer");
            }
            invocation.timeoutSec = seconds;
        } else {
            throw CliError("unknown argument: " + arg);
        }
    }
    return invocation;
}

int pingSubcommand(const PingInvocation& invocation, std::ostream& out, std::ostream& err) {
    const ResolvedHosts resolved = resolveHosts(invocation.inventoryPath, invocation.selector, err);
    if (!resolved.ok()) {
        return resolved.exitCode;
    }
    if (std::any_of(resolved.clients.begin(), resolved.clients.end(),
                    [](const ClientEntry& client) { return client.transport == "native"; })) {
        err << "pipeshellx ping: transport=native hosts are not supported by ping; select SSH hosts instead\n";
        return 2;
    }

    ProcessManager manager;
    const LogContext context{.pid = psx::os::currentProcessId(),
                             .sessionId = "ping",
                             .clientId = "-",
                             .command = "echo connected",
                             .runId = {},
                             .stageId = {}};
    ProcessManager::RemoteRunOptions options;
    options.timeoutSec = invocation.timeoutSec;
    const auto result = manager.executeRemote(resolved.clients, "echo connected", context, options);

    bool anyOffline = false;
    for (const auto& client : result.clientResults) {
        const bool online =
            client.exitCode == 0 && !client.timedOut && client.stdoutData.find("connected") != std::string::npos;
        if (online) {
            out << client.clientId << " ONLINE\n";
        } else {
            anyOffline = true;
            std::string reason = client.errorMessage;
            if (reason.empty()) {
                reason = "exit " + std::to_string(client.exitCode);
            }
            out << client.clientId << " OFFLINE: " << reason << "\n";
        }
    }
    return anyOffline ? 1 : 0;
}

} // namespace psx::cli
