#include "psx/sink/ordered_sink.hpp"

namespace psx::sink {

void OrderedSink::stageStarted(std::string_view stage) {
    buffers_.erase(std::string(stage)); // a retry replaces the failed attempt
}

void OrderedSink::line(std::string_view stage, Channel channel, std::string_view text) {
    buffers_[std::string(stage)].events.push_back(Event{channel, std::string(text)});
}

void OrderedSink::stageFinished(std::string_view stage, const StageResult& result) {
    buffers_[std::string(stage)].result = result;
}

void OrderedSink::runFinished(const RunSummary& summary) {
    for (auto& [stage, buffered] : buffers_) {
        inner_->stageStarted(stage);
        for (const auto& event : buffered.events) {
            inner_->line(stage, event.channel, event.text);
        }
        if (buffered.result.has_value()) {
            inner_->stageFinished(stage, *buffered.result);
        }
    }
    inner_->runFinished(summary);
}

} // namespace psx::sink
