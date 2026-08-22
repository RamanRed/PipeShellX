#include "psx/sink/group_sink.hpp"

namespace psx::sink {

void GroupSink::line(std::string_view stage, Channel channel, std::string_view text) {
    Buffered& buffered = buffers_[std::string(stage)];
    (channel == Channel::Stdout ? buffered.stdoutLines : buffered.stderrLines).emplace_back(text);
}

void GroupSink::stageFinished(std::string_view stage, const StageResult& result) {
    const std::string key(stage);
    Buffered& buffered = buffers_[key];

    out_ << "CLIENT " << stage << '\n';
    for (const auto& l : buffered.stdoutLines) {
        out_ << l << '\n';
    }
    // The normalized error stands in for raw stderr when present (v0.1.0).
    if (!result.errorMessage.empty()) {
        out_ << result.errorMessage << '\n';
    } else {
        for (const auto& l : buffered.stderrLines) {
            out_ << l << '\n';
        }
    }
    buffers_.erase(key);
}

} // namespace psx::sink
