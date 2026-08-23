#include "psx/cli/pipe_command.hpp"

#include "psx/os/signal_source.hpp"
#include "psx/pipeline/local_runner.hpp"
#include "psx/pipeline/parser.hpp"
#include "psx/pipeline/pipeline.hpp"
#include "psx/pipeline/planner.hpp"
#include "psx/runtime/reactor.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(PIPESHELLX_HAVE_TLS)
#include "psx/inventory/inventory.hpp"
#include "psx/pipeline/distributed_runner.hpp"

#include <exception>
#include <fstream>
#include <optional>
#include <sstream>
#endif

namespace psx::cli {

namespace {

struct PipeArgs {
    std::string spec;          // the joined pipeline spec (non-flag args)
    std::string inventoryPath; // -i
    std::string certPath;      // --cert
    std::string keyPath;       // --key
    std::string caPath;        // --ca
    int nativePort = 7433;     // --native-port
};

// Splits recognised flags from the pipeline spec (everything else, rejoined).
PipeArgs parsePipeArgs(const std::vector<std::string>& args) {
    PipeArgs out;
    std::vector<std::string> specParts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto takeValue = [&](std::string& dest) {
            if (i + 1 < args.size()) {
                dest = args[++i];
            }
        };
        if (arg == "-i") {
            takeValue(out.inventoryPath);
        } else if (arg == "--cert") {
            takeValue(out.certPath);
        } else if (arg == "--key") {
            takeValue(out.keyPath);
        } else if (arg == "--ca") {
            takeValue(out.caPath);
        } else if (arg == "--native-port") {
            std::string value;
            takeValue(value);
            if (!value.empty()) {
                try {
                    out.nativePort = std::stoi(value);
                } catch (const std::exception&) {
                    out.nativePort = -1; // flagged as invalid below
                }
            }
        } else {
            specParts.push_back(arg);
        }
    }
    for (std::size_t i = 0; i < specParts.size(); ++i) {
        if (i != 0) {
            out.spec += ' ';
        }
        out.spec += specParts[i];
    }
    return out;
}

// Runs an all-local pipeline via LocalRunner; returns the pipefail exit code.
int runLocal(const std::vector<psx::pipeline::Stage>& stages, std::ostream& out, std::ostream& err) {
    auto reactor = psx::runtime::Reactor::create({.signals = {psx::os::Signal::Interrupt, psx::os::Signal::Terminate}});
    if (!reactor.ok()) {
        err << "pipeshellx pipe: " << reactor.error().message() << "\n";
        return 2;
    }
    psx::runtime::Reactor& r = *reactor.value();

    int exitCode = 0;
    bool completed = false;
    psx::pipeline::LocalRunner runner(
        r, [&out](std::string_view chunk) { out.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); });
    auto started = runner.run(stages, [&](psx::pipeline::LocalRunner::Outcome outcome) {
        exitCode = outcome.exitCode;
        completed = true;
        r.stop();
    });
    if (!started.ok()) {
        err << "pipeshellx pipe: cannot start pipeline: " << started.error().message() << "\n";
        return 2;
    }
    (void)r.onSignal([&r](psx::os::Signal) { r.stop(); });
    (void)r.run();
    out.flush();
    return completed ? exitCode : 130;
}

#if defined(PIPESHELLX_HAVE_TLS)
std::optional<std::string> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Runs an all-remote pipeline via DistributedRunner, resolving each @placement
// to a node through the inventory.
int runRemote(const std::vector<psx::pipeline::Stage>& stages,
              const PipeArgs& args,
              std::ostream& out,
              std::ostream& err) {
    if (args.inventoryPath.empty()) {
        err << "pipeshellx pipe: remote stages need an inventory (-i FILE)\n";
        return 2;
    }
    if (args.certPath.empty() || args.keyPath.empty() || args.caPath.empty()) {
        err << "pipeshellx pipe: remote stages need --cert F --key F --ca F (the controller identity)\n";
        return 2;
    }
    if (args.nativePort <= 0 || args.nativePort > 65535) {
        err << "pipeshellx pipe: --native-port must be 1..65535\n";
        return 2;
    }
    const auto cert = slurp(args.certPath);
    const auto key = slurp(args.keyPath);
    const auto ca = slurp(args.caPath);
    if (!cert || !key || !ca) {
        err << "pipeshellx pipe: cannot read --cert/--key/--ca\n";
        return 2;
    }

    psx::inventory::Inventory inventory;
    try {
        inventory = psx::inventory::Inventory::loadFromFile(args.inventoryPath);
    } catch (const std::exception& e) {
        err << "pipeshellx pipe: " << e.what() << "\n";
        return 2;
    }

    std::vector<psx::pipeline::RemoteStage> remote;
    remote.reserve(stages.size());
    for (const auto& stage : stages) {
        const auto hosts = inventory.selectHosts({stage.placement});
        if (hosts.size() != 1) {
            err << "pipeshellx pipe: placement '@" << stage.placement << "' must name exactly one inventory host\n";
            return 2;
        }
        const psx::inventory::Host& host = hosts.front();
        remote.push_back({.argv = stage.argv,
                          .host = host.host,
                          .port = static_cast<std::uint16_t>(host.nativePort != 0 ? host.nativePort : args.nativePort),
                          .expectedSan = host.san});
    }

    auto reactor = psx::runtime::Reactor::create({.signals = {psx::os::Signal::Interrupt, psx::os::Signal::Terminate}});
    if (!reactor.ok()) {
        err << "pipeshellx pipe: " << reactor.error().message() << "\n";
        return 2;
    }
    psx::runtime::Reactor& r = *reactor.value();

    int exitCode = 0;
    bool completed = false;
    std::string failure;
    psx::pipeline::DistributedRunner runner(
        r, {.certificatePem = *cert, .privateKeyPem = *key, .caPem = *ca},
        [&out](std::string_view chunk) { out.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); },
        [&err](std::string_view chunk) { err.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); });
    auto started = runner.run(remote, [&](psx::pipeline::DistributedRunner::Outcome outcome) {
        exitCode = outcome.exitCode;
        failure = outcome.error;
        completed = true;
        r.stop();
    });
    if (!started.ok()) {
        err << "pipeshellx pipe: " << started.error().message() << "\n";
        return 2;
    }
    (void)r.onSignal([&r](psx::os::Signal) { r.stop(); });
    (void)r.run();
    out.flush();
    if (!completed) {
        return 130;
    }
    if (!failure.empty()) {
        err << "pipeshellx pipe: " << failure << "\n";
    }
    return exitCode;
}
#endif // PIPESHELLX_HAVE_TLS

} // namespace

int pipeSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const PipeArgs parsed = parsePipeArgs(args);
    if (parsed.spec.empty()) {
        err << "Usage: pipeshellx pipe [-i FILE --cert F --key F --ca F [--native-port P]] "
               "\"'cmd'@place | 'cmd2'@place2\"\n";
        return 2;
    }

    auto pipeline = psx::pipeline::parsePipeSpec(parsed.spec);
    if (!pipeline.ok()) {
        err << "pipeshellx pipe: " << pipeline.error().message() << "\n";
        return 2;
    }
    if (auto plan = psx::pipeline::Planner::plan(pipeline.value()); !plan.ok()) {
        err << "pipeshellx pipe: " << plan.error().message() << "\n";
        return 2;
    }

    bool anyRemote = false;
    bool anyLocal = false;
    for (const auto& stage : pipeline.value().stages) {
        (stage.placement.empty() ? anyLocal : anyRemote) = true;
    }
    if (anyRemote && anyLocal) {
        err << "pipeshellx pipe: mixing local and remote (@placement) stages is not supported yet\n";
        return 2;
    }

    if (!anyRemote) {
        return runLocal(pipeline.value().stages, out, err);
    }
#if defined(PIPESHELLX_HAVE_TLS)
    return runRemote(pipeline.value().stages, parsed, out, err);
#else
    err << "pipeshellx pipe: this build has no native transport support (OpenSSL); "
           "remote @placement stages are unavailable\n";
    return 2;
#endif
}

} // namespace psx::cli
