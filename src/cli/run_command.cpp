#include "psx/cli/run_command.hpp"

#include "client_config.hpp"
#include "logger.hpp"
#include "process_manager.hpp"
#include "psx/cli/selection.hpp"
#include "psx/os/paths.hpp"
#include "psx/os/system.hpp"
#include "psx/sink/group_sink.hpp"
#include "psx/sink/json_sink.hpp"
#include "psx/sink/stream_sink.hpp"

#include <charconv>
#include <memory>

namespace psx::cli {

namespace {

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

int parseIntArg(const std::string& value, const char* flag) {
    int result = 0;
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, result);
    if (ec != std::errc{} || ptr != end || result < 0) {
        throw CliError(std::string(flag) + " expects a non-negative integer, got '" + value + "'");
    }
    return result;
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

void setSink(RunInvocation& invocation, SinkMode mode, bool& sinkSet) {
    if (sinkSet) {
        throw CliError("only one of --stream/--group/--json may be given");
    }
    invocation.sink = mode;
    sinkSet = true;
}

} // namespace

RunInvocation parseRun(const std::vector<std::string>& args) {
    RunInvocation invocation;
    bool sinkSet = false;
    bool sawDashDash = false;

    auto valueFor = [&](std::size_t& i, const std::string& flag) -> std::string {
        if (i + 1 >= args.size()) {
            throw CliError(flag + " requires a value");
        }
        return args[++i];
    };

    std::size_t i = 0;
    for (; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            sawDashDash = true;
            ++i;
            break;
        }
        if (arg == "-g" || arg == "--group-name") {
            setSelector(invocation.selector, SelectorKind::Group, valueFor(i, arg));
        } else if (arg == "-t" || arg == "--tag") {
            setSelector(invocation.selector, SelectorKind::Tag, valueFor(i, arg));
        } else if (arg == "-H" || arg == "--hosts") {
            setSelector(invocation.selector, SelectorKind::Hosts, valueFor(i, arg));
        } else if (arg == "-i" || arg == "--inventory") {
            invocation.inventoryPath = valueFor(i, arg);
        } else if (arg == "--timeout") {
            invocation.timeoutSec = parseIntArg(valueFor(i, arg), "--timeout");
        } else if (arg == "--stream") {
            setSink(invocation, SinkMode::Stream, sinkSet);
        } else if (arg == "--group") {
            setSink(invocation, SinkMode::Group, sinkSet);
        } else if (arg == "--json") {
            setSink(invocation, SinkMode::Json, sinkSet);
        } else if (arg == "--no-color" || arg == "--no-colour") {
            invocation.colour = false;
        } else {
            throw CliError("unknown option: " + arg);
        }
    }

    if (!sawDashDash) {
        throw CliError("expected `-- <command>` (a `--` before the command to run)");
    }
    for (; i < args.size(); ++i) {
        invocation.command.push_back(args[i]);
    }
    if (invocation.command.empty()) {
        throw CliError("no command given after `--`");
    }
    return invocation;
}

namespace {

std::unique_ptr<psx::sink::Sink>
makeSink(const RunInvocation& invocation, std::ostream& out, std::ostream& err, bool colourTty) {
    switch (invocation.sink) {
        case SinkMode::Stream:
            return std::make_unique<psx::sink::StreamSink>(out, err, invocation.colour && colourTty);
        case SinkMode::Json:
            return std::make_unique<psx::sink::JsonSink>(out);
        case SinkMode::Group:
        default:
            return std::make_unique<psx::sink::GroupSink>(out);
    }
}

} // namespace

int runSubcommand(const RunInvocation& invocation, std::ostream& out, std::ostream& err, bool colourTty) {
    const ResolvedHosts resolved = resolveHosts(invocation.inventoryPath, invocation.selector, err);
    if (!resolved.ok()) {
        return resolved.exitCode;
    }
    const std::vector<ClientEntry>& clients = resolved.clients;

    // Reuse the existing per-argument quoting for the remote command line.
    std::string remoteCommand;
    for (std::size_t i = 0; i < invocation.command.size(); ++i) {
        if (i != 0) {
            remoteCommand += ' ';
        }
        remoteCommand += '\'';
        for (char c : invocation.command[i]) {
            if (c == '\'') {
                remoteCommand += "'\\''";
            } else {
                remoteCommand += c;
            }
        }
        remoteCommand += '\'';
    }

    auto sink = makeSink(invocation, out, err, colourTty);
    ProcessManager manager;
    const LogContext context{psx::os::currentProcessId(), "run", "-", remoteCommand};
    const auto result = manager.executeRemote(clients, remoteCommand, context, invocation.timeoutSec, sink.get());
    return result.exitCode == 0 ? 0 : 1;
}

} // namespace psx::cli
