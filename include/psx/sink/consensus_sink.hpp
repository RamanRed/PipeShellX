#pragma once

// ConsensusSink — defers rendering because consensus needs every host's stdout.

#include "psx/sink/sink.hpp"

#include <map>
#include <ostream>
#include <string>

namespace psx::sink {

class ConsensusSink : public Sink {
public:
    ConsensusSink(std::ostream& out, bool json) : out_(out), json_(json) {}

    bool streamsLive() const noexcept override { return false; }
    void stageStarted(std::string_view stage) override;
    void line(std::string_view stage, Channel channel, std::string_view text) override;
    void stageFinished(std::string_view stage, const StageResult& result) override;
    void runFinished(const RunSummary& summary) override;

private:
    std::ostream& out_;
    bool json_;
    std::map<std::string, std::string> stdoutByHost_;
};

} // namespace psx::sink
