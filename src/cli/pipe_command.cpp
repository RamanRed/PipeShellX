#include "psx/cli/pipe_command.hpp"

#include "psx/os/signal_source.hpp"
#include "psx/pipeline/local_runner.hpp"
#include "psx/pipeline/parser.hpp"
#include "psx/pipeline/planner.hpp"
#include "psx/runtime/reactor.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace psx::cli {

int pipeSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        err << "Usage: pipeshellx pipe \"'cmd'@place | 'cmd2'@place2\"\n";
        return 2;
    }
    // The shell may have split the spec on spaces; rejoin into one string.
    std::string spec;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            spec += ' ';
        }
        spec += args[i];
    }

    auto pipeline = psx::pipeline::parsePipeSpec(spec);
    if (!pipeline.ok()) {
        err << "pipeshellx pipe: " << pipeline.error().message() << "\n";
        return 2;
    }
    if (auto plan = psx::pipeline::Planner::plan(pipeline.value()); !plan.ok()) {
        err << "pipeshellx pipe: " << plan.error().message() << "\n";
        return 2;
    }
    for (const auto& stage : pipeline.value().stages) {
        if (!stage.placement.empty()) {
            err << "pipeshellx pipe: remote placement '@" << stage.placement
                << "' is not supported yet (local pipelines only)\n";
            return 2;
        }
    }

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
    auto started = runner.run(pipeline.value().stages, [&](psx::pipeline::LocalRunner::Outcome outcome) {
        exitCode = outcome.exitCode;
        completed = true;
        r.stop();
    });
    if (!started.ok()) {
        err << "pipeshellx pipe: cannot start pipeline: " << started.error().message() << "\n";
        return 2;
    }
    (void)r.onSignal([&r](psx::os::Signal) { r.stop(); }); // Ctrl-C cancels the pipeline
    (void)r.run();
    out.flush();
    return completed ? exitCode : 130; // 130: interrupted before completion
}

} // namespace psx::cli
