#include "psx/sink/consensus_sink.hpp"

#include "psx/sink/consensus.hpp"

#include <utility>
#include <vector>

namespace psx::sink {

void ConsensusSink::stageStarted(std::string_view stage) {
    stdoutByHost_.erase(std::string(stage)); // a retry replaces the failed attempt
    stdoutByHost_.emplace(std::string(stage), "");
}

void ConsensusSink::line(std::string_view stage, Channel channel, std::string_view text) {
    if (channel != Channel::Stdout) {
        return;
    }
    std::string& output = stdoutByHost_[std::string(stage)];
    output.append(text);
    output.push_back('\n');
}

void ConsensusSink::stageFinished(std::string_view stage, const StageResult& /*result*/) {
    stdoutByHost_.try_emplace(std::string(stage));
}

void ConsensusSink::runFinished(const RunSummary& /*summary*/) {
    std::vector<std::pair<std::string, std::string>> outputs;
    outputs.reserve(stdoutByHost_.size());
    for (const auto& [host, output] : stdoutByHost_) {
        outputs.emplace_back(host, output);
    }
    const ConsensusReport report = consensus(outputs);
    if (json_) {
        renderConsensusJson(report, out_);
    } else {
        renderConsensus(report, out_);
    }
}

} // namespace psx::sink
