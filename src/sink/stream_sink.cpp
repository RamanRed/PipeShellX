#include "psx/sink/stream_sink.hpp"

#include <array>

namespace psx::sink {

namespace {
constexpr const char* kReset = "\033[0m";
// Six stable, readable foreground colours cycled across hosts.
constexpr std::array<const char*, 6> kPalette{
    "\033[36m", "\033[32m", "\033[33m", "\033[35m", "\033[34m", "\033[31m",
};
} // namespace

const char* StreamSink::colourFor(std::string_view stage) {
    auto it = assigned_.find(std::string(stage));
    if (it != assigned_.end()) {
        return it->second;
    }
    const char* colour = kPalette[nextColour_ % kPalette.size()];
    ++nextColour_;
    assigned_.emplace(std::string(stage), colour);
    return colour;
}

void StreamSink::line(std::string_view stage, Channel channel, std::string_view text) {
    std::ostream& stream = channel == Channel::Stdout ? out_ : err_;
    if (colour_) {
        stream << colourFor(stage) << '[' << stage << ']' << kReset << ' ' << text << '\n';
    } else {
        stream << '[' << stage << "] " << text << '\n';
    }
}

void StreamSink::runFinished(const RunSummary& summary) {
    err_ << "-- " << summary.succeeded << '/' << summary.stages << " ok, " << summary.failed << " failed";
    if (summary.droppedBytes > 0) {
        err_ << ", " << summary.droppedBytes << " bytes dropped";
    }
    if (summary.cancelled) {
        err_ << ", cancelled";
    }
    err_ << " --\n";
}

} // namespace psx::sink
