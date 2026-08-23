#include "psx/cli/pipe_command.hpp"

#include "psx/os/signal_source.hpp"
#include "psx/pipeline/local_runner.hpp"
#include "psx/pipeline/parser.hpp"
#include "psx/pipeline/pipeline.hpp"
#include "psx/pipeline/planner.hpp"
#include "psx/runtime/reactor.hpp"

#include "psx/inventory/inventory.hpp"

#include <exception>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(PIPESHELLX_HAVE_TLS)
#include "psx/pipeline/fanin_pipeline.hpp"
#include "psx/pipeline/segmented_pipeline.hpp"

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
    bool check = false;        // --check: validate + print the plan, do not run
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
        } else if (arg == "--check") {
            out.check = true;
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

// A placement of "" or "local" means the stage runs on the controller.
bool isLocalPlacement(const std::string& placement) {
    return placement.empty() || placement == "local";
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

// Fan-in: the first stage runs on every host of a group and the merged output
// feeds the rest of the pipeline (§5.2 `grep@shards | sort@local`).
int runFanIn(const std::vector<psx::pipeline::Stage>& stages,
             const PipeArgs& args,
             const psx::inventory::Inventory& inventory,
             const std::string& cert,
             const std::string& key,
             const std::string& ca,
             std::ostream& out,
             std::ostream& err) {
    std::vector<psx::transport::NativeController::Target> sourceTargets;
    for (const psx::inventory::Host& host : inventory.selectGroup(stages.front().placement)) {
        sourceTargets.push_back(
            {.host = host.host,
             .port = static_cast<std::uint16_t>(host.nativePort != 0 ? host.nativePort : args.nativePort),
             .expectedSan = host.san});
    }

    std::vector<psx::pipeline::ResolvedStage> downstream;
    for (std::size_t i = 1; i < stages.size(); ++i) {
        const auto& stage = stages[i];
        if (isLocalPlacement(stage.placement)) {
            downstream.push_back({.argv = stage.argv, .host = "", .port = 0, .expectedSan = ""});
            continue;
        }
        if (!inventory.selectGroup(stage.placement).empty()) {
            err << "pipeshellx pipe: a group placement '@" << stage.placement
                << "' is only supported on the first stage\n";
            return 2;
        }
        std::vector<psx::inventory::Host> hosts;
        try {
            hosts = inventory.selectHosts({stage.placement});
        } catch (const std::exception& e) {
            err << "pipeshellx pipe: " << e.what() << "\n";
            return 2;
        }
        const psx::inventory::Host& host = hosts.front();
        downstream.push_back(
            {.argv = stage.argv,
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
    psx::pipeline::FanInPipeline runner(
        r, {.certificatePem = cert, .privateKeyPem = key, .caPem = ca},
        [&out](std::string_view chunk) { out.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); },
        [&err](std::string_view chunk) { err.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); });
    auto started =
        runner.run(stages.front().argv, sourceTargets, downstream, [&](psx::pipeline::FanInPipeline::Outcome outcome) {
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

// Runs a pipeline with at least one remote stage via SegmentedPipeline,
// resolving each @placement to a node through the inventory; @local/unplaced
// stages run on the controller and are spliced in.
int runSegmented(const std::vector<psx::pipeline::Stage>& stages,
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

    // Fan-in: the first stage names a group of >1 hosts -> run it on all of them.
    if (!isLocalPlacement(stages.front().placement) && inventory.selectGroup(stages.front().placement).size() > 1) {
        return runFanIn(stages, args, inventory, *cert, *key, *ca, out, err);
    }

    std::vector<psx::pipeline::ResolvedStage> resolved;
    resolved.reserve(stages.size());
    for (const auto& stage : stages) {
        if (isLocalPlacement(stage.placement)) {
            resolved.push_back({.argv = stage.argv, .host = "", .port = 0, .expectedSan = ""});
            continue;
        }
        std::vector<psx::inventory::Host> hosts;
        try {
            hosts = inventory.selectHosts({stage.placement});
        } catch (const std::exception& e) {
            err << "pipeshellx pipe: " << e.what() << "\n";
            return 2;
        }
        const psx::inventory::Host& host = hosts.front();
        resolved.push_back(
            {.argv = stage.argv,
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
    psx::pipeline::SegmentedPipeline runner(
        r, {.certificatePem = *cert, .privateKeyPem = *key, .caPem = *ca},
        [&out](std::string_view chunk) { out.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); },
        [&err](std::string_view chunk) { err.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); });
    auto started = runner.run(resolved, [&](psx::pipeline::SegmentedPipeline::Outcome outcome) {
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

// `--check`: validate the pipeline and print the resolved plan without running.
// Remote @placements are resolved through the inventory (no connection is made).
int runCheck(const std::vector<psx::pipeline::Stage>& stages,
             const PipeArgs& args,
             std::ostream& out,
             std::ostream& err) {
    bool anyRemote = false;
    for (const auto& stage : stages) {
        if (!isLocalPlacement(stage.placement)) {
            anyRemote = true;
        }
    }
    psx::inventory::Inventory inventory;
    if (anyRemote) {
        if (args.inventoryPath.empty()) {
            err << "pipeshellx pipe --check: remote stages need an inventory (-i FILE)\n";
            return 2;
        }
        try {
            inventory = psx::inventory::Inventory::loadFromFile(args.inventoryPath);
        } catch (const std::exception& e) {
            err << "pipeshellx pipe --check: " << e.what() << "\n";
            return 2;
        }
    }

    out << "pipeline: " << stages.size() << (stages.size() == 1 ? " stage\n" : " stages\n");
    for (const auto& stage : stages) {
        std::string command;
        for (std::size_t i = 0; i < stage.argv.size(); ++i) {
            command += (i == 0 ? "" : " ") + stage.argv[i];
        }
        out << "  " << stage.id << "  " << command;
        if (isLocalPlacement(stage.placement)) {
            out << "  @local\n";
            continue;
        }
        const auto group = inventory.selectGroup(stage.placement);
        if (group.size() > 1) {
            out << "  @" << stage.placement << " (fan-in) ->";
            for (const psx::inventory::Host& host : group) {
                out << " " << host.host << ":" << (host.nativePort != 0 ? host.nativePort : args.nativePort);
            }
            out << "\n";
            continue;
        }
        std::vector<psx::inventory::Host> hosts;
        try {
            hosts = inventory.selectHosts({stage.placement});
        } catch (const std::exception& e) {
            out << "\n";
            err << "pipeshellx pipe --check: " << e.what() << "\n";
            return 2;
        }
        const psx::inventory::Host& host = hosts.front();
        const int port = host.nativePort != 0 ? host.nativePort : args.nativePort;
        out << "  @" << stage.placement << " -> " << host.host << ":" << port << "\n";
    }
    out << "valid\n";
    return 0;
}

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
    for (const auto& stage : pipeline.value().stages) {
        if (!isLocalPlacement(stage.placement)) {
            anyRemote = true;
        }
    }
    if (parsed.check) {
        return runCheck(pipeline.value().stages, parsed, out, err);
    }

    if (!anyRemote) {
        return runLocal(pipeline.value().stages, out, err);
    }
#if defined(PIPESHELLX_HAVE_TLS)
    return runSegmented(pipeline.value().stages, parsed, out, err);
#else
    err << "pipeshellx pipe: this build has no native transport support (OpenSSL); "
           "remote @placement stages are unavailable\n";
    return 2;
#endif
}

} // namespace psx::cli
