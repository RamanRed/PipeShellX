#pragma once

// JsonSink — JSON Lines: one object per stage when it finishes, then a final
// summary object. Machine-readable; the schema is documented in docs/json.md.

#include "psx/sink/sink.hpp"

#include <ostream>
#include <string>
#include <unordered_map>

namespace psx::sink {

class JsonSink : public Sink {
public:
    explicit JsonSink(std::ostream& out) : out_(out) {}

    void line(std::string_view stage, Channel channel, std::string_view text) override;
    void stageFinished(std::string_view stage, const StageResult& result) override;
    void runFinished(const RunSummary& summary) override;

private:
    struct Buffered {
        std::string stdoutText;
        std::string stderrText;
    };
    std::ostream& out_;
    std::unordered_map<std::string, Buffered> buffers_;
};

} // namespace psx::sink
