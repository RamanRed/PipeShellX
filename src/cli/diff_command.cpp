#include "psx/cli/diff_command.hpp"

#include <ostream>
#include <string>
#include <vector>

#if defined(PIPESHELLX_HAVE_TLS)
#include "psx/cli/selection.hpp"
#include "psx/os/tls.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/sink/consensus.hpp"
#include "psx/transport/native_controller.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#endif

namespace psx::cli {

#if defined(PIPESHELLX_HAVE_TLS)
namespace {
struct DiffInvocation {
    std::string inventoryPath;
    std::string certPath;
    std::string keyPath;
    std::string caPath;
    int nativePort = 7433;
    bool json = false;
    Selector selector;
    std::vector<std::string> command;
};

bool isOption(std::string_view value) {
    return value == "-i" || value == "--inventory" || value == "--cert" || value == "--key" || value == "--ca" ||
           value == "--native-port" || value == "-g" || value == "--group-name" || value == "-t" || value == "--tag" ||
           value == "-H" || value == "--hosts" || value == "--json";
}

std::string valueFor(const std::vector<std::string>& args, std::size_t& index, const std::string& option) {
    if (index + 1 >= args.size() || args[index + 1] == "--" || isOption(args[index + 1])) {
        throw std::invalid_argument(option + " requires a value");
    }
    return args[++index];
}

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) {
            items.push_back(item);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return items;
}

void setSelector(Selector& selector, SelectorKind kind, const std::string& value) {
    if (selector.kind != SelectorKind::All) {
        throw std::invalid_argument("selectors -g/-t/-H are mutually exclusive");
    }
    selector.kind = kind;
    if (kind == SelectorKind::Hosts) {
        selector.hosts = splitCsv(value);
    } else {
        selector.value = value;
    }
}

int parseNativePort(const std::string& value) {
    int port = 0;
    const char* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, port);
    if (value.empty() || ec != std::errc{} || ptr != end || port < 1 || port > 65535) {
        throw std::invalid_argument("--native-port must be 1..65535");
    }
    return port;
}

DiffInvocation parseDiff(const std::vector<std::string>& args) {
    DiffInvocation invocation;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            invocation.command.assign(args.begin() + static_cast<std::ptrdiff_t>(i + 1), args.end());
            break;
        }
        if (arg == "-i" || arg == "--inventory") {
            invocation.inventoryPath = valueFor(args, i, arg);
        } else if (arg == "--cert") {
            invocation.certPath = valueFor(args, i, arg);
        } else if (arg == "--key") {
            invocation.keyPath = valueFor(args, i, arg);
        } else if (arg == "--ca") {
            invocation.caPath = valueFor(args, i, arg);
        } else if (arg == "--native-port") {
            invocation.nativePort = parseNativePort(valueFor(args, i, arg));
        } else if (arg == "-g" || arg == "--group-name") {
            setSelector(invocation.selector, SelectorKind::Group, valueFor(args, i, arg));
        } else if (arg == "-t" || arg == "--tag") {
            setSelector(invocation.selector, SelectorKind::Tag, valueFor(args, i, arg));
        } else if (arg == "-H" || arg == "--hosts") {
            setSelector(invocation.selector, SelectorKind::Hosts, valueFor(args, i, arg));
        } else if (arg == "--json") {
            invocation.json = true;
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    return invocation;
}

std::optional<std::string> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

int diffSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    DiffInvocation invocation;
    try {
        invocation = parseDiff(args);
    } catch (const std::invalid_argument& ex) {
        err << "pipeshellx diff: " << ex.what() << "\n";
        return 2;
    }

    if (invocation.command.empty()) {
        err << "Usage: pipeshellx diff -i FILE -g GROUP --cert F --key F --ca F -- <command>\n";
        return 2;
    }
    if (invocation.certPath.empty() || invocation.keyPath.empty() || invocation.caPath.empty()) {
        err << "pipeshellx diff: --cert F --key F --ca F (the controller identity) are required\n";
        return 2;
    }
    const auto cert = slurp(invocation.certPath);
    const auto key = slurp(invocation.keyPath);
    const auto ca = slurp(invocation.caPath);
    if (!cert || !key || !ca) {
        err << "pipeshellx diff: cannot read --cert/--key/--ca\n";
        return 2;
    }

    const ResolvedHosts resolved = resolveHosts(invocation.inventoryPath, invocation.selector, err);
    if (!resolved.ok()) {
        return resolved.exitCode;
    }

    auto reactor = psx::runtime::Reactor::create();
    if (!reactor.ok()) {
        err << "pipeshellx diff: " << reactor.error().message() << "\n";
        return 2;
    }
    psx::runtime::Reactor& r = *reactor.value();

    std::vector<psx::transport::NativeController::Target> targets;
    targets.reserve(resolved.clients.size());
    for (const auto& client : resolved.clients) {
        targets.push_back(
            {.host = client.host,
             .port = static_cast<std::uint16_t>(client.nativePort != 0 ? client.nativePort : invocation.nativePort),
             .expectedSan = client.expectedSan});
    }

    psx::transport::NativeController controller(
        r, {.certificatePem = *cert, .privateKeyPem = *key, .caPem = *ca, .crlPem = {}, .isServer = false});
    std::vector<psx::transport::NativeController::HostResult> results;
    auto started = controller.start(targets, invocation.command,
                                    [&](std::vector<psx::transport::NativeController::HostResult> hostResults) {
                                        results = std::move(hostResults);
                                        r.stop();
                                    });
    if (!started.ok()) {
        err << "pipeshellx diff: " << started.error().message() << "\n";
        return 2;
    }
    if (auto ran = r.run(); !ran.ok()) {
        err << "pipeshellx diff: " << ran.error().message() << "\n";
        return 2;
    }

    std::vector<std::pair<std::string, std::string>> hostOutputs;
    std::vector<std::string> failed;
    for (const auto& result : results) {
        if (result.ok && result.error.empty() && result.exitCode == 0) {
            hostOutputs.emplace_back(result.host, result.stdoutData);
        } else {
            const std::string reason = !result.error.empty() ? result.error : "exit " + std::to_string(result.exitCode);
            failed.push_back(result.host + " (" + reason + ")");
        }
    }

    const psx::sink::ConsensusReport report = psx::sink::consensus(hostOutputs);
    if (invocation.json) {
        psx::sink::renderConsensusJson(report, out);
    } else {
        psx::sink::renderConsensus(report, out);
    }
    for (const std::string& host : failed) {
        err << "pipeshellx diff: host failed: " << host << "\n";
    }
    if (!failed.empty()) {
        return 2;
    }
    return report.unanimous() ? 0 : 1;
}

#else  // no TLS
int diffSubcommand(const std::vector<std::string>& /*args*/, std::ostream& /*out*/, std::ostream& err) {
    err << "pipeshellx diff: this build has no native transport support (OpenSSL)\n";
    return 2;
}
#endif // PIPESHELLX_HAVE_TLS

} // namespace psx::cli
