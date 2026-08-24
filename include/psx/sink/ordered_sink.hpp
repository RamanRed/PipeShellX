#pragma once

// OrderedSink — defers rendering because stages finish nondeterministically.

#include "psx/sink/sink.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace psx::sink {

class OrderedSink : public Sink {
public:
    explicit OrderedSink(std::unique_ptr<Sink> inner) : inner_(std::move(inner)) {}

    void stageStarted(std::string_view stage) override;
    void line(std::string_view stage, Channel channel, std::string_view text) override;
    void stageFinished(std::string_view stage, const StageResult& result) override;
    void runFinished(const RunSummary& summary) override;

private:
    struct Event {
        Channel channel;
        std::string text;
    };
    struct Buffered {
        std::vector<Event> events;
        std::optional<StageResult> result;
    };

    std::unique_ptr<Sink> inner_;
    std::map<std::string, Buffered> buffers_;
};

} // namespace psx::sink
