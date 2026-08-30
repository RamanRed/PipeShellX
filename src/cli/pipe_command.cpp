#include "psx/cli/pipe_command.hpp"

#include "psx/os/signal_source.hpp"
#include "psx/pipeline/dag_runner.hpp"
#include "psx/pipeline/parser.hpp"
#include "psx/pipeline/pipeline.hpp"
#include "psx/pipeline/pipeline_yaml.hpp"
#include "psx/pipeline/planner.hpp"
#include "psx/runtime/reactor.hpp"

#include "psx/inventory/inventory.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <fstream>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#if defined(PIPESHELLX_HAVE_TLS)
#include "psx/pipeline/fanin_pipeline.hpp"
#include "psx/pipeline/segmented_pipeline.hpp"

#endif

namespace psx::cli {

namespace {

struct PipeArgs {
    std::string spec;          // the joined pipeline spec (non-flag args)
    std::string filePath;      // --file/-f
    std::string inventoryPath; // -i
    std::string certPath;      // --cert
    std::string keyPath;       // --key
    std::string caPath;        // --ca
    int nativePort = 7433;     // --native-port
    bool check = false;        // --check: validate + print the plan, do not run
    bool fileRequested = false;
    std::string argumentError;
};

bool looksLikeOption(std::string_view value) {
    return !value.empty() && value.front() == '-';
}

bool looksLikeSignedInteger(std::string_view value) {
    if (!value.empty() && (value.front() == '-' || value.front() == '+')) {
        value.remove_prefix(1);
    }
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Splits recognised flags from the pipeline spec (everything else, rejoined).
PipeArgs parsePipeArgs(const std::vector<std::string>& args) {
    PipeArgs out;
    std::vector<std::string> specParts;
    bool inventorySeen = false;
    bool fileSeen = false;
    bool certSeen = false;
    bool keySeen = false;
    bool caSeen = false;
    bool nativePortSeen = false;
    bool checkSeen = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto takeValue = [&](std::string_view canonical, bool& seen, std::string& destination,
                             bool signedInteger = false) {
            if (seen) {
                out.argumentError = "duplicate option '" + std::string(canonical) + "'";
                return false;
            }
            seen = true;
            if (i + 1 >= args.size() ||
                (looksLikeOption(args[i + 1]) && !(signedInteger && looksLikeSignedInteger(args[i + 1])))) {
                out.argumentError = "option '" + arg + "' requires a value";
                return false;
            }
            destination = args[++i];
            return true;
        };
        if (arg == "-i") {
            if (!takeValue("-i", inventorySeen, out.inventoryPath)) {
                break;
            }
        } else if (arg == "-f" || arg == "--file") {
            if (!takeValue("--file", fileSeen, out.filePath)) {
                break;
            }
            out.fileRequested = true;
        } else if (arg == "--cert") {
            if (!takeValue("--cert", certSeen, out.certPath)) {
                break;
            }
        } else if (arg == "--key") {
            if (!takeValue("--key", keySeen, out.keyPath)) {
                break;
            }
        } else if (arg == "--ca") {
            if (!takeValue("--ca", caSeen, out.caPath)) {
                break;
            }
        } else if (arg == "--check") {
            if (checkSeen) {
                out.argumentError = "duplicate option '--check'";
                break;
            }
            checkSeen = true;
            out.check = true;
        } else if (arg == "--native-port") {
            std::string value;
            if (!takeValue("--native-port", nativePortSeen, value, true)) {
                break;
            }
            int parsedPort = 0;
            const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsedPort);
            if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || parsedPort < 1 ||
                parsedPort > 65535) {
                out.argumentError = "--native-port must be an integer in 1..65535";
                break;
            }
            out.nativePort = parsedPort;
        } else if (looksLikeOption(arg)) {
            out.argumentError = "unknown option '" + arg + "'";
            break;
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

std::optional<std::string> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<psx::pipeline::Stage> stagesInPlanOrder(const psx::pipeline::Pipeline& pipeline,
                                                    const psx::pipeline::Planner::Plan& plan) {
    std::unordered_map<std::string, const psx::pipeline::Stage*> byId;
    byId.reserve(pipeline.stages.size());
    for (const auto& stage : pipeline.stages) {
        byId.emplace(stage.id, &stage);
    }
    std::vector<psx::pipeline::Stage> ordered;
    ordered.reserve(plan.order.size());
    for (const std::string& id : plan.order) {
        ordered.push_back(*byId.at(id));
    }
    return ordered;
}

// The existing remote runner accepts only a single sequence. Prove that the
// declared graph is exactly that sequence before handing it off, so no remote
// DAG can ever be silently linearised.
bool isDeclaredChain(const psx::pipeline::Pipeline& pipeline, const psx::pipeline::Planner::Plan& plan) {
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto& edge : pipeline.edges) {
        edges.emplace(edge.from, edge.to);
    }
    if (plan.order.size() == 1) {
        return edges.empty();
    }
    if (edges.size() + 1 != plan.order.size()) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < plan.order.size(); ++i) {
        if (edges.count({plan.order[i], plan.order[i + 1]}) == 0) {
            return false;
        }
    }
    return true;
}

// Runs an all-local graph via DagRunner; returns the topological pipefail code.
int runLocal(const psx::pipeline::Pipeline& pipeline, std::ostream& out, std::ostream& err) {
    auto reactor = psx::runtime::Reactor::create({.signals = {psx::os::Signal::Interrupt, psx::os::Signal::Terminate}});
    if (!reactor.ok()) {
        err << "pipeshellx pipe: " << reactor.error().message() << "\n";
        return 2;
    }
    psx::runtime::Reactor& r = *reactor.value();

    int exitCode = 0;
    bool completed = false;
    psx::pipeline::DagRunner runner(
        r, [&out](std::string_view chunk) { out.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); });
    auto started = runner.run(pipeline, [&](psx::pipeline::DagRunner::Outcome outcome) {
        exitCode = outcome.exitCode;
        completed = true;
        r.stop();
    });
    if (!started.ok()) {
        err << "pipeshellx pipe: cannot start pipeline: " << started.error().message() << "\n";
        return 2;
    }
    (void)r.onSignal([&r, &runner](psx::os::Signal) {
        runner.cancel();
        r.stop();
    });
    (void)r.run();
    out.flush();
    return completed ? exitCode : 130;
}

#if defined(PIPESHELLX_HAVE_TLS)
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
        r, {.certificatePem = cert, .privateKeyPem = key, .caPem = ca, .crlPem = {}, .isServer = false},
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
        r, {.certificatePem = *cert, .privateKeyPem = *key, .caPem = *ca, .crlPem = {}, .isServer = false},
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
    if (!parsed.argumentError.empty()) {
        err << "pipeshellx pipe: " << parsed.argumentError << "\n";
        return 2;
    }
    if (parsed.fileRequested && !parsed.spec.empty()) {
        err << "pipeshellx pipe: --file cannot be combined with an inline pipeline spec\n";
        return 2;
    }
    if (!parsed.fileRequested && parsed.spec.empty()) {
        err << "Usage: pipeshellx pipe [-f FILE | -i FILE --cert F --key F --ca F [--native-port P]] "
               "\"'cmd'@place | 'cmd2'@place2\"\n";
        return 2;
    }
    if (parsed.fileRequested && parsed.filePath.empty()) {
        err << "pipeshellx pipe: --file requires a path\n";
        return 2;
    }

    psx::Result<psx::pipeline::Pipeline> pipeline = [&]() -> psx::Result<psx::pipeline::Pipeline> {
        if (!parsed.fileRequested) {
            return psx::pipeline::parsePipeSpec(parsed.spec);
        }
        const auto contents = slurp(parsed.filePath);
        if (!contents) {
            return psx::Error{psx::ErrorClass::InvalidArgument, 0, "cannot read --file"};
        }
        return psx::pipeline::loadPipelineYaml(*contents);
    }();
    if (!pipeline.ok()) {
        err << "pipeshellx pipe: " << pipeline.error().message() << "\n";
        return 2;
    }
    auto planned = psx::pipeline::Planner::plan(pipeline.value());
    if (!planned.ok()) {
        err << "pipeshellx pipe: " << planned.error().message() << "\n";
        return 2;
    }
    const std::vector<psx::pipeline::Stage> orderedStages = stagesInPlanOrder(pipeline.value(), planned.value());

    bool anyRemote = false;
    for (const auto& stage : pipeline.value().stages) {
        if (!isLocalPlacement(stage.placement)) {
            anyRemote = true;
        }
    }
    if (anyRemote && !isDeclaredChain(pipeline.value(), planned.value())) {
        err << "pipeshellx pipe: non-linear remote DAGs are not supported; use a single declared chain\n";
        return 2;
    }
    if (parsed.check) {
        return runCheck(orderedStages, parsed, out, err);
    }

    if (!anyRemote) {
        return runLocal(pipeline.value(), out, err);
    }
#if defined(PIPESHELLX_HAVE_TLS)
    return runSegmented(orderedStages, parsed, out, err);
#else
    err << "pipeshellx pipe: this build has no native transport support (OpenSSL); "
           "remote @placement stages are unavailable\n";
    return 2;
#endif
}

} // namespace psx::cli
