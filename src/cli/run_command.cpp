#include "psx/cli/run_command.hpp"

#include "cli_options.hpp"
#include "client_config.hpp"
#include "logger.hpp"
#include "process_manager.hpp"
#include "psx/audit/audit_log.hpp"
#include "psx/cli/selection.hpp"
#include "psx/os/paths.hpp"
#include "psx/os/system.hpp"
#include "psx/policy/policy.hpp"
#include "psx/runtime/ids.hpp"
#include "psx/sink/group_sink.hpp"
#include "psx/sink/json_sink.hpp"
#include "psx/sink/stream_sink.hpp"

#if defined(PIPESHELLX_HAVE_TLS)
#include "psx/os/tls.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/stream/line_framer.hpp"
#include "psx/transport/native_controller.hpp"

#include <chrono>
#include <span>
#include <unordered_map>
#endif
#include "ssh_auth.hpp"

#include <charconv>
#include <filesystem>
#include <memory>
#include <system_error>

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

psx::stream::OverflowPolicy parsePolicy(const std::string& value) {
    if (value == "block") {
        return psx::stream::OverflowPolicy::Block;
    }
    if (value == "drop-oldest") {
        return psx::stream::OverflowPolicy::DropOldest;
    }
    if (value == "drop-newest") {
        return psx::stream::OverflowPolicy::DropNewest;
    }
    if (value == "spool") {
        return psx::stream::OverflowPolicy::Spool;
    }
    throw CliError("--overflow must be block, drop-oldest, drop-newest or spool, got '" + value + "'");
}

RemoteShell parseShell(const std::string& value) {
    if (value == "posix") {
        return RemoteShell::Posix;
    }
    if (value == "cmd") {
        return RemoteShell::Cmd;
    }
    if (value == "powershell" || value == "pwsh") {
        return RemoteShell::PowerShell;
    }
    throw CliError("--shell must be posix, cmd or powershell, got '" + value + "'");
}

// Parses a byte size: a decimal count with an optional K/M/G or KiB/MiB/GiB
// (binary) suffix. "1MiB" == "1M" == 1048576.
std::size_t parseSize(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size() && (value[i] >= '0' && value[i] <= '9')) {
        ++i;
    }
    if (i == 0) {
        throw CliError("--ring expects a size like 1MiB, got '" + value + "'");
    }
    std::size_t count = 0;
    const auto* digitsEnd = value.data() + i;
    (void)std::from_chars(value.data(), digitsEnd, count);
    std::string suffix = value.substr(i);
    std::size_t multiplier = 1;
    if (suffix.empty() || suffix == "B") {
        multiplier = 1;
    } else if (suffix == "K" || suffix == "KiB") {
        multiplier = 1024;
    } else if (suffix == "M" || suffix == "MiB") {
        multiplier = 1024ULL * 1024;
    } else if (suffix == "G" || suffix == "GiB") {
        multiplier = 1024ULL * 1024 * 1024;
    } else {
        throw CliError("--ring: unknown size suffix '" + suffix + "'");
    }
    return count * multiplier;
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
        } else if (arg == "-c" || arg == "--concurrency") {
            invocation.concurrency = parseIntArg(valueFor(i, arg), "--concurrency");
        } else if (arg == "--overflow") {
            invocation.policy = parsePolicy(valueFor(i, arg));
        } else if (arg == "--ring") {
            invocation.ringBytes = parseSize(valueFor(i, arg));
        } else if (arg == "--policy") {
            invocation.policyPath = valueFor(i, arg);
        } else if (arg == "--stream") {
            setSink(invocation, SinkMode::Stream, sinkSet);
        } else if (arg == "--group") {
            setSink(invocation, SinkMode::Group, sinkSet);
        } else if (arg == "--json") {
            setSink(invocation, SinkMode::Json, sinkSet);
        } else if (arg == "--reuse") {
            invocation.reuse = true;
        } else if (arg == "--retries") {
            invocation.retries = parseIntArg(valueFor(i, arg), "--retries");
        } else if (arg == "--fail-fast") {
            invocation.failFast = true;
        } else if (arg == "--shell") {
            invocation.shell = parseShell(valueFor(i, arg));
        } else if (arg == "--audit-log") {
            invocation.auditPath = valueFor(i, arg);
        } else if (arg == "--transport") {
            const std::string value = valueFor(i, arg);
            if (value == "native") {
                invocation.native = true;
            } else if (value != "ssh") {
                throw CliError("--transport must be ssh or native, got '" + value + "'");
            }
        } else if (arg == "--cert") {
            invocation.certPath = valueFor(i, arg);
        } else if (arg == "--key") {
            invocation.keyPath = valueFor(i, arg);
        } else if (arg == "--ca") {
            invocation.caPath = valueFor(i, arg);
        } else if (arg == "--native-port") {
            invocation.nativePort = parseIntArg(valueFor(i, arg), "--native-port");
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

#if defined(PIPESHELLX_HAVE_TLS)
namespace {

std::optional<std::string> slurpFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Runs the command on each host over the psx/1 native mTLS backplane.
int runNative(const RunInvocation& invocation,
              const std::vector<ClientEntry>& clients,
              psx::sink::Sink* sink,
              std::ostream& err) {
    if (invocation.certPath.empty() || invocation.keyPath.empty() || invocation.caPath.empty()) {
        err << "pipeshellx run: --transport native requires --cert, --key and --ca\n";
        return 2;
    }
    const auto cert = slurpFile(invocation.certPath);
    const auto key = slurpFile(invocation.keyPath);
    const auto ca = slurpFile(invocation.caPath);
    if (!cert || !key || !ca) {
        err << "pipeshellx run: cannot read --cert/--key/--ca\n";
        return 2;
    }

    auto reactor = psx::runtime::Reactor::create();
    if (!reactor.ok()) {
        err << "pipeshellx run: " << reactor.error().message() << "\n";
        return 2;
    }
    psx::runtime::Reactor& r = *reactor.value();

    // A line framer per host and channel feeds the sink whole lines from the raw
    // stream bytes, keeping stdout and stderr distinct as they are over SSH.
    struct HostFramers {
        psx::stream::LineFramer out;
        psx::stream::LineFramer err;
    };
    auto framers = std::make_shared<std::unordered_map<std::string, HostFramers>>();
    auto emit = [sink](const std::string& host, psx::sink::Channel channel, std::string_view line) {
        if (sink != nullptr) {
            sink->line(host, channel, line);
        }
    };
    psx::transport::NativeController controller(
        r, {.certificatePem = *cert, .privateKeyPem = *key, .caPem = *ca},
        [framers, emit](const std::string& host, std::string_view bytes, psx::transport::Channel channel) {
            const bool err = channel == psx::transport::Channel::Stderr;
            HostFramers& hf = (*framers)[host];
            const auto sinkChannel = err ? psx::sink::Channel::Stderr : psx::sink::Channel::Stdout;
            (err ? hf.err : hf.out)
                .push(std::span<const char>(bytes.data(), bytes.size()),
                      [&](std::string_view line, bool) { emit(host, sinkChannel, line); });
        });

    std::vector<psx::transport::NativeController::Target> targets;
    targets.reserve(clients.size());
    for (const auto& client : clients) {
        targets.push_back(
            {.host = client.host,
             .port = static_cast<std::uint16_t>(client.nativePort != 0 ? client.nativePort : invocation.nativePort),
             .expectedSan = client.expectedSan});
        if (sink != nullptr) {
            sink->stageStarted(client.host);
        }
    }

    int exitCode = 0;
    (void)controller.start(
        targets, invocation.command, [&](std::vector<psx::transport::NativeController::HostResult> results) {
            std::size_t succeeded = 0;
            for (auto& res : results) {
                HostFramers& hf = (*framers)[res.host];
                hf.out.flush([&](std::string_view line, bool) { emit(res.host, psx::sink::Channel::Stdout, line); });
                hf.err.flush([&](std::string_view line, bool) { emit(res.host, psx::sink::Channel::Stderr, line); });
                const bool ok = res.ok && res.exitCode == 0 && res.error.empty();
                succeeded += ok ? 1 : 0;
                if (!ok && exitCode == 0) {
                    exitCode = 1;
                }
                if (sink != nullptr) {
                    sink->stageFinished(res.host, psx::sink::StageResult{.exitCode = res.exitCode,
                                                                         .timedOut = false,
                                                                         .errorMessage = res.error,
                                                                         .droppedBytes = 0});
                }
            }
            if (sink != nullptr) {
                sink->runFinished(
                    psx::sink::RunSummary{results.size(), succeeded, results.size() - succeeded, 0, false});
            }
            r.stop();
        });
    if (invocation.timeoutSec > 0) {
        r.after(std::chrono::seconds(invocation.timeoutSec), [&] { controller.cancel("timed out"); });
    }
    (void)r.run();
    return exitCode;
}

} // namespace
#endif // PIPESHELLX_HAVE_TLS

int runSubcommand(const RunInvocation& invocation, std::ostream& out, std::ostream& err, bool colourTty) {
    if (!invocation.policyPath.empty()) {
        try {
            const auto policy = psx::policy::Policy::loadFromFile(invocation.policyPath);
            if (auto rejected = policy.validate(invocation.command); rejected.has_value()) {
                err << "pipeshellx run: " << *rejected << "\n";
                return 2;
            }
        } catch (const std::exception& ex) {
            err << "pipeshellx run: " << ex.what() << "\n";
            return 2;
        }
    }

    const ResolvedHosts resolved = resolveHosts(invocation.inventoryPath, invocation.selector, err);
    if (!resolved.ok()) {
        return resolved.exitCode;
    }
    const std::vector<ClientEntry>& clients = resolved.clients;

    if (invocation.native) {
#if defined(PIPESHELLX_HAVE_TLS)
        auto nativeSink = makeSink(invocation, out, err, colourTty);
        return runNative(invocation, clients, nativeSink.get(), err);
#else
        err << "pipeshellx run: this build has no native transport support (OpenSSL)\n";
        return 2;
#endif
    }

    // Quote the remote command line for the target shell (POSIX by default).
    const std::string remoteCommand = quoteRemoteCommand(invocation.command, invocation.shell);

    // ControlMaster reuse: a persisted master socket per user@host:port under
    // the state dir lets repeated runs skip the TCP+KEX handshake.
    std::string controlPath;
    if (invocation.reuse) {
        const std::filesystem::path dir = std::filesystem::path(psx::os::stateDirectory("pipeshellx")) / "control";
        std::error_code ignored;
        std::filesystem::create_directories(dir, ignored);
        controlPath = (dir / "cm-%r@%h:%p").string();
    }

    const std::string runId = psx::runtime::newRunId();

    // Optional JSONL audit trail (opt-in via --audit-log). An unwritable path
    // degrades to no audit with a warning; it never aborts the run.
    std::unique_ptr<psx::audit::AuditLog> audit;
    if (!invocation.auditPath.empty()) {
        audit = std::make_unique<psx::audit::AuditLog>(invocation.auditPath);
        if (!audit->ok()) {
            err << "pipeshellx run: warning: cannot open audit log " << invocation.auditPath << "\n";
            audit.reset();
        } else {
            audit->runStarted(runId, remoteCommand, clients.size());
        }
    }

    auto sink = makeSink(invocation, out, err, colourTty);
    ProcessManager manager;
    const LogContext context{
        .pid = psx::os::currentProcessId(), .sessionId = "run", .command = remoteCommand, .runId = runId};
    const auto result = manager.executeRemote(clients, remoteCommand, context,
                                              {.timeoutSec = invocation.timeoutSec,
                                               .sink = sink.get(),
                                               .concurrency = static_cast<std::size_t>(invocation.concurrency),
                                               .policy = invocation.policy,
                                               .ringBytes = invocation.ringBytes,
                                               .controlPath = controlPath,
                                               .cancellable = true,
                                               .failFast = invocation.failFast,
                                               .maxRetries = invocation.retries});
    const int exitCode = result.cancelled ? kExitCancelled : (result.exitCode == 0 ? 0 : 1);

    if (audit) {
        std::size_t succeeded = 0;
        for (std::size_t i = 0; i < result.clientResults.size(); ++i) {
            const auto& stage = result.clientResults[i];
            const bool ok = stage.exitCode == 0 && !stage.timedOut && !stage.cancelled && !stage.aborted &&
                            stage.errorMessage.empty();
            succeeded += ok ? 1 : 0;
            audit->stageFinished(runId, psx::audit::StageRecord{.host = stage.clientId,
                                                                .stageId = "s" + std::to_string(i),
                                                                .exitCode = stage.exitCode,
                                                                .attempts = stage.attempts,
                                                                .timedOut = stage.timedOut,
                                                                .cancelled = stage.cancelled,
                                                                .aborted = stage.aborted,
                                                                .droppedBytes = stage.droppedBytes,
                                                                .error = stage.errorMessage});
        }
        audit->runFinished(runId, psx::audit::RunRecord{.total = result.clientResults.size(),
                                                        .succeeded = succeeded,
                                                        .failed = result.clientResults.size() - succeeded,
                                                        .cancelled = result.cancelled,
                                                        .exitCode = exitCode});
    }
    return exitCode;
}

} // namespace psx::cli
