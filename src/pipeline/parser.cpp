#include "psx/pipeline/parser.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace psx::pipeline {

namespace {
psx::Error invalid(const char* what) {
    return psx::Error{psx::ErrorClass::InvalidArgument, 0, what};
}

std::string trim(std::string_view s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (std::isspace(static_cast<unsigned char>(s[begin])) != 0)) {
        ++begin;
    }
    while (end > begin && (std::isspace(static_cast<unsigned char>(s[end - 1])) != 0)) {
        --end;
    }
    return std::string(s.substr(begin, end - begin));
}

std::vector<std::string> tokenize(const std::string& command) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < command.size()) {
        while (i < command.size() && (std::isspace(static_cast<unsigned char>(command[i])) != 0)) {
            ++i;
        }
        const std::size_t start = i;
        while (i < command.size() && (std::isspace(static_cast<unsigned char>(command[i])) == 0)) {
            ++i;
        }
        if (i > start) {
            out.push_back(command.substr(start, i - start));
        }
    }
    return out;
}
} // namespace

psx::Result<Pipeline> parsePipeSpec(std::string_view spec) {
    // Split on top-level `|` — a `|` inside a single-quoted command is literal.
    std::vector<std::string> segments;
    std::string current;
    bool inQuote = false;
    for (char c : spec) {
        if (c == '\'') {
            inQuote = !inQuote;
            current.push_back(c);
        } else if (c == '|' && !inQuote) {
            segments.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (inQuote) {
        return invalid("unterminated quote in pipe spec");
    }
    segments.push_back(current);

    Pipeline pipeline;
    for (std::size_t idx = 0; idx < segments.size(); ++idx) {
        const std::string seg = trim(segments[idx]);
        if (seg.empty()) {
            return invalid("empty stage in pipe spec");
        }

        std::string command;
        std::string placement;
        if (seg.front() == '\'') {
            const std::size_t close = seg.find('\'', 1);
            if (close == std::string::npos) {
                return invalid("unterminated quote in pipe spec");
            }
            command = seg.substr(1, close - 1);
            const std::string rest = trim(seg.substr(close + 1));
            if (!rest.empty()) {
                if (rest.front() != '@') {
                    return invalid("expected @placement after a quoted command");
                }
                placement = trim(rest.substr(1));
                if (placement.empty()) {
                    return invalid("empty placement after @");
                }
            }
        } else {
            if (seg.find_first_of(" \t") != std::string::npos) {
                return invalid("unquoted command contains spaces; wrap it in single quotes");
            }
            const std::size_t at = seg.find('@');
            if (at == std::string::npos) {
                command = seg;
            } else {
                command = seg.substr(0, at);
                placement = seg.substr(at + 1);
                if (placement.empty()) {
                    return invalid("empty placement after @");
                }
            }
        }

        std::vector<std::string> argv = tokenize(command);
        if (argv.empty()) {
            return invalid("empty command in pipe spec");
        }
        Stage stage;
        stage.id = "s" + std::to_string(idx);
        stage.argv = std::move(argv);
        stage.placement = std::move(placement);
        pipeline.stages.push_back(std::move(stage));
        if (idx > 0) {
            pipeline.edges.push_back({.from = "s" + std::to_string(idx - 1), .to = "s" + std::to_string(idx)});
        }
    }
    return pipeline;
}

} // namespace psx::pipeline
