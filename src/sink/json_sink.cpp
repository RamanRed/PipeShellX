#include "psx/sink/json_sink.hpp"

#include "psx/json/json.hpp"

namespace psx::sink {

namespace {

void appendLine(std::string& text, std::string_view line) {
    if (!text.empty()) {
        text += '\n';
    }
    text.append(line);
}

} // namespace

void JsonSink::line(std::string_view stage, Channel channel, std::string_view text) {
    Buffered& buffered = buffers_[std::string(stage)];
    appendLine(channel == Channel::Stdout ? buffered.stdoutText : buffered.stderrText, text);
}

void JsonSink::stageFinished(std::string_view stage, const StageResult& result) {
    const std::string key(stage);
    Buffered& buffered = buffers_[key];

    out_ << "{\"stage\":" << psx::json::quote(stage) << ",\"exit\":" << result.exitCode
         << ",\"timed_out\":" << (result.timedOut ? "true" : "false")
         << ",\"error\":" << psx::json::quote(result.errorMessage) << ",\"dropped\":" << result.droppedBytes
         << ",\"stdout\":" << psx::json::quote(buffered.stdoutText)
         << ",\"stderr\":" << psx::json::quote(buffered.stderrText) << "}\n";
    buffers_.erase(key);
}

void JsonSink::runFinished(const RunSummary& summary) {
    out_ << "{\"summary\":true,\"stages\":" << summary.stages << ",\"succeeded\":" << summary.succeeded
         << ",\"failed\":" << summary.failed << ",\"dropped\":" << summary.droppedBytes
         << ",\"cancelled\":" << (summary.cancelled ? "true" : "false") << "}\n";
}

} // namespace psx::sink
