#pragma once

// psx::sink::Sink — a terminal renderer for a run's output (L4/L5). It sees
// per-stage line events and lifecycle callbacks; concrete sinks decide how to
// present them: `group` (today's per-client blocks), `stream` (live,
// host-tagged) and `json` (one object per stage + a summary). Sinks are pure
// (they write to std::ostream), so they are unit-tested without spawning.

#include <cstdint>
#include <string>
#include <string_view>

namespace psx::sink {

enum class Channel : std::uint8_t { Stdout, Stderr };

struct StageResult {
    int exitCode = 0;
    bool timedOut = false;
    std::string errorMessage; // normalized ("ERROR: connection failed"); empty when none
    std::uint64_t droppedBytes = 0;
};

struct RunSummary {
    std::size_t stages = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::uint64_t droppedBytes = 0;
    bool cancelled = false;
};

class Sink {
public:
    virtual ~Sink() = default;

    virtual void stageStarted(std::string_view /*stage*/) {}
    virtual void line(std::string_view stage, Channel channel, std::string_view text) = 0;
    virtual void stageFinished(std::string_view /*stage*/, const StageResult& /*result*/) {}
    virtual void runFinished(const RunSummary& /*summary*/) {}
};

} // namespace psx::sink
