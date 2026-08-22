#pragma once

// StreamSink — live, host-tagged output: `[<stage>] <line>` as each line
// arrives, stdout lines to `out`, stderr lines to `err`. With colour on, each
// stage gets a stable colour from a fixed palette (same host → same colour).
// The run summary (counts, drops, cancellation) is written to `err`.

#include "psx/sink/sink.hpp"

#include <ostream>
#include <string>
#include <unordered_map>

namespace psx::sink {

class StreamSink : public Sink {
public:
    StreamSink(std::ostream& out, std::ostream& err, bool colour) : out_(out), err_(err), colour_(colour) {}

    void line(std::string_view stage, Channel channel, std::string_view text) override;
    void runFinished(const RunSummary& summary) override;

private:
    const char* colourFor(std::string_view stage);

    std::ostream& out_;
    std::ostream& err_;
    bool colour_;
    std::unordered_map<std::string, const char*> assigned_;
    std::size_t nextColour_ = 0;
};

} // namespace psx::sink
