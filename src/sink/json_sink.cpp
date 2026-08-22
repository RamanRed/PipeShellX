#include "psx/sink/json_sink.hpp"

#include <cstdio>

namespace psx::sink {

namespace {

// Minimal RFC 8259 string escaping into a JSON double-quoted string.
std::string jsonString(std::string_view value) {
    std::string out = "\"";
    out.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

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

    out_ << "{\"stage\":" << jsonString(stage) << ",\"exit\":" << result.exitCode
         << ",\"timed_out\":" << (result.timedOut ? "true" : "false")
         << ",\"error\":" << jsonString(result.errorMessage) << ",\"dropped\":" << result.droppedBytes
         << ",\"stdout\":" << jsonString(buffered.stdoutText) << ",\"stderr\":" << jsonString(buffered.stderrText)
         << "}\n";
    buffers_.erase(key);
}

void JsonSink::runFinished(const RunSummary& summary) {
    out_ << "{\"summary\":true,\"stages\":" << summary.stages << ",\"succeeded\":" << summary.succeeded
         << ",\"failed\":" << summary.failed << ",\"dropped\":" << summary.droppedBytes
         << ",\"cancelled\":" << (summary.cancelled ? "true" : "false") << "}\n";
}

} // namespace psx::sink
