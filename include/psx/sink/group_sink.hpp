#pragma once

// GroupSink — the v0.1.0 format: each stage's output as one block
//   CLIENT <stage>
//   <stdout lines>
//   <normalized error, or the raw stderr lines>
// emitted when the stage finishes (grouped, not interleaved).

#include "psx/sink/sink.hpp"

#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace psx::sink {

class GroupSink : public Sink {
public:
    explicit GroupSink(std::ostream& out) : out_(out) {}

    void stageStarted(std::string_view stage) override;
    void line(std::string_view stage, Channel channel, std::string_view text) override;
    void stageFinished(std::string_view stage, const StageResult& result) override;

private:
    struct Buffered {
        std::vector<std::string> stdoutLines;
        std::vector<std::string> stderrLines;
    };
    std::ostream& out_;
    std::unordered_map<std::string, Buffered> buffers_;
};

} // namespace psx::sink
