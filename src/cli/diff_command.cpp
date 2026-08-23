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

#include <cstdint>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>
#endif

namespace psx::cli {

#if defined(PIPESHELLX_HAVE_TLS)
namespace {
std::optional<std::string> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

int diffSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    std::string inventoryPath;
    std::string certPath;
    std::string keyPath;
    std::string caPath;
    int nativePort = 7433;
    Selector selector;
    std::vector<std::string> command;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto value = [&](std::string& dest) {
            if (i + 1 < args.size()) {
                dest = args[++i];
            }
        };
        if (arg == "--") {
            command.assign(args.begin() + static_cast<long>(i) + 1, args.end());
            break;
        } else if (arg == "-i") {
            value(inventoryPath);
        } else if (arg == "--cert") {
            value(certPath);
        } else if (arg == "--key") {
            value(keyPath);
        } else if (arg == "--ca") {
            value(caPath);
        } else if (arg == "--native-port") {
            std::string v;
            value(v);
            nativePort = v.empty() ? nativePort : std::atoi(v.c_str());
        } else if (arg == "-g") {
            std::string v;
            value(v);
            selector = {.kind = SelectorKind::Group, .value = v, .hosts = {}};
        } else if (arg == "-t") {
            std::string v;
            value(v);
            selector = {.kind = SelectorKind::Tag, .value = v, .hosts = {}};
        }
    }

    if (command.empty()) {
        err << "Usage: pipeshellx diff -i FILE -g GROUP --cert F --key F --ca F -- <command>\n";
        return 2;
    }
    if (certPath.empty() || keyPath.empty() || caPath.empty()) {
        err << "pipeshellx diff: --cert F --key F --ca F (the controller identity) are required\n";
        return 2;
    }
    const auto cert = slurp(certPath);
    const auto key = slurp(keyPath);
    const auto ca = slurp(caPath);
    if (!cert || !key || !ca) {
        err << "pipeshellx diff: cannot read --cert/--key/--ca\n";
        return 2;
    }

    const ResolvedHosts resolved = resolveHosts(inventoryPath, selector, err);
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
        targets.push_back({.host = client.host,
                           .port = static_cast<std::uint16_t>(client.nativePort != 0 ? client.nativePort : nativePort),
                           .expectedSan = client.expectedSan});
    }

    psx::transport::NativeController controller(r, {.certificatePem = *cert, .privateKeyPem = *key, .caPem = *ca});
    std::vector<psx::transport::NativeController::HostResult> results;
    auto started =
        controller.start(targets, command, [&](std::vector<psx::transport::NativeController::HostResult> hostResults) {
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
        if (result.ok && result.error.empty()) {
            hostOutputs.emplace_back(result.host, result.output);
        } else {
            failed.push_back(result.host + " (" + (result.error.empty() ? "non-zero exit" : result.error) + ")");
        }
    }

    const psx::sink::ConsensusReport report = psx::sink::consensus(hostOutputs);
    psx::sink::renderConsensus(report, out);
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
