#include "psx/pipeline/pipeline_yaml.hpp"

#include "psx/pipeline/planner.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psx::pipeline {

namespace {

struct Line {
    std::size_t indent = 0;
    std::string text;
};

struct Field {
    std::string key;
    std::string value;
};

struct Scalar {
    std::string value;
    bool quoted = false;
};

psx::Error invalid(const char* message) {
    return psx::Error{psx::ErrorClass::InvalidArgument, 0, message};
}

std::string trim(std::string_view value) {
    std::size_t first = 0;
    std::size_t last = value.size();
    while (first < last && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string stripComment(std::string_view line) {
    char quote = '\0';
    bool escaped = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != '\0') {
            if (quote == '"' && escaped) {
                escaped = false;
            } else if (quote == '"' && c == '\\') {
                escaped = true;
            } else if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])) != 0)) {
            return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

psx::Result<std::vector<Line>> tokenizeLines(std::string_view yaml) {
    std::vector<Line> lines;
    std::size_t start = 0;
    while (start <= yaml.size()) {
        const std::size_t end = yaml.find('\n', start);
        const std::size_t length = end == std::string_view::npos ? yaml.size() - start : end - start;
        std::string raw = stripComment(yaml.substr(start, length));
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        std::size_t indent = 0;
        while (indent < raw.size() && raw[indent] == ' ') {
            ++indent;
        }
        if (indent < raw.size() && raw[indent] == '\t') {
            return invalid("pipeline YAML: tabs are not valid indentation");
        }
        const std::string content = trim(std::string_view(raw).substr(indent));
        if (!content.empty()) {
            lines.push_back({.indent = indent, .text = content});
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

psx::Result<Field> parseField(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return invalid("pipeline YAML: expected key: value");
    }
    const std::string key = trim(text.substr(0, colon));
    if (key.empty() || key.find_first_of(" \t") != std::string::npos) {
        return invalid("pipeline YAML: invalid mapping key");
    }
    return Field{.key = key, .value = trim(text.substr(colon + 1))};
}

psx::Result<Scalar> parseScalar(std::string_view text) {
    const std::string value = trim(text);
    if (value.empty()) {
        return invalid("pipeline YAML: scalar cannot be empty");
    }
    if (value.front() != '\'' && value.front() != '"') {
        if (value.find_first_of("'\"") != std::string::npos) {
            return invalid("pipeline YAML: quotes must enclose the whole scalar");
        }
        return Scalar{.value = value, .quoted = false};
    }

    const char quote = value.front();
    std::string result;
    bool escaped = false;
    std::size_t close = std::string::npos;
    for (std::size_t i = 1; i < value.size(); ++i) {
        const char c = value[i];
        if (quote == '"' && escaped) {
            result.push_back(c);
            escaped = false;
        } else if (quote == '"' && c == '\\') {
            escaped = true;
        } else if (c == quote) {
            close = i;
            break;
        } else {
            result.push_back(c);
        }
    }
    if (close == std::string::npos || escaped) {
        return invalid("pipeline YAML: unterminated quoted scalar");
    }
    if (!trim(std::string_view(value).substr(close + 1)).empty()) {
        return invalid("pipeline YAML: trailing text after quoted scalar");
    }
    if (result.empty()) {
        return invalid("pipeline YAML: scalar cannot be empty");
    }
    return Scalar{.value = std::move(result), .quoted = true};
}

psx::Result<std::vector<std::string>> parseArgv(std::string_view text) {
    const std::string value = trim(text);
    if (value.empty()) {
        return invalid("pipeline YAML: run cannot be empty");
    }
    if (value.front() != '[') {
        auto scalar = parseScalar(value);
        if (!scalar.ok()) {
            return scalar.error();
        }
        if (!scalar.value().quoted && scalar.value().value.find_first_of(" \t") != std::string::npos) {
            return invalid("pipeline YAML: run with spaces must be quoted or a YAML list");
        }
        std::vector<std::string> argv;
        std::size_t start = 0;
        while (start < scalar.value().value.size()) {
            while (start < scalar.value().value.size() &&
                   std::isspace(static_cast<unsigned char>(scalar.value().value[start])) != 0) {
                ++start;
            }
            const std::size_t end = start;
            while (start < scalar.value().value.size() &&
                   std::isspace(static_cast<unsigned char>(scalar.value().value[start])) == 0) {
                ++start;
            }
            if (start > end) {
                argv.push_back(scalar.value().value.substr(end, start - end));
            }
        }
        return argv;
    }

    if (value.back() != ']') {
        return invalid("pipeline YAML: unterminated run list");
    }
    const std::string_view body(value.data() + 1, value.size() - 2);
    std::vector<std::string> argv;
    std::size_t start = 0;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        const char c = i < body.size() ? body[i] : ',';
        if (quote != '\0') {
            if (quote == '"' && escaped) {
                escaped = false;
            } else if (quote == '"' && c == '\\') {
                escaped = true;
            } else if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == ',') {
            auto item = parseScalar(body.substr(start, i - start));
            if (!item.ok()) {
                return item.error();
            }
            if (!item.value().quoted && item.value().value.find_first_of(" \t") != std::string::npos) {
                return invalid("pipeline YAML: run list items cannot contain spaces");
            }
            argv.push_back(std::move(item.value().value));
            start = i + 1;
        }
    }
    if (quote != '\0' || escaped) {
        return invalid("pipeline YAML: unterminated quoted run list item");
    }
    if (argv.size() == 1 && argv.front().empty()) {
        return invalid("pipeline YAML: run list cannot be empty");
    }
    return argv;
}

psx::Result<Pipeline> parseDocument(const std::vector<Line>& lines) {
    Pipeline pipeline;
    bool sawStages = false;
    bool sawEdges = false;

    std::size_t index = 0;
    while (index < lines.size()) {
        const Line& sectionLine = lines[index];
        if (sectionLine.indent != 0) {
            return invalid("pipeline YAML: bad indentation");
        }
        auto section = parseField(sectionLine.text);
        if (!section.ok()) {
            return section.error();
        }
        if (section.value().value != "") {
            return invalid("pipeline YAML: top-level sections must be block lists");
        }
        const bool isStages = section.value().key == "stages";
        const bool isEdges = section.value().key == "edges";
        if (!isStages && !isEdges) {
            return invalid("pipeline YAML: unknown top-level key");
        }
        bool& seen = isStages ? sawStages : sawEdges;
        if (seen) {
            return invalid("pipeline YAML: duplicate top-level key");
        }
        seen = true;
        ++index;

        while (index < lines.size() && lines[index].indent > 0) {
            if (lines[index].indent != 2 || lines[index].text.rfind("- ", 0) != 0) {
                return invalid("pipeline YAML: bad indentation or expected list item");
            }
            std::vector<Field> fields;
            auto first = parseField(std::string_view(lines[index].text).substr(2));
            if (!first.ok()) {
                return first.error();
            }
            fields.push_back(std::move(first.value()));
            ++index;
            while (index < lines.size() && lines[index].indent > 2) {
                if (lines[index].indent != 4 || lines[index].text.rfind("- ", 0) == 0) {
                    return invalid("pipeline YAML: bad indentation");
                }
                auto field = parseField(lines[index].text);
                if (!field.ok()) {
                    return field.error();
                }
                fields.push_back(std::move(field.value()));
                ++index;
            }

            if (isStages) {
                Stage stage;
                stage.placement = "local";
                bool hasId = false;
                bool hasRun = false;
                bool hasPlacement = false;
                for (const Field& field : fields) {
                    if (field.key == "id") {
                        if (hasId) {
                            return invalid("pipeline YAML: duplicate stage key");
                        }
                        auto id = parseScalar(field.value);
                        if (!id.ok()) {
                            return id.error();
                        }
                        stage.id = std::move(id.value().value);
                        hasId = true;
                    } else if (field.key == "run") {
                        if (hasRun) {
                            return invalid("pipeline YAML: duplicate stage key");
                        }
                        auto argv = parseArgv(field.value);
                        if (!argv.ok()) {
                            return argv.error();
                        }
                        stage.argv = std::move(argv.value());
                        hasRun = true;
                    } else if (field.key == "at") {
                        if (hasPlacement) {
                            return invalid("pipeline YAML: duplicate stage key");
                        }
                        auto placement = parseScalar(field.value);
                        if (!placement.ok()) {
                            return placement.error();
                        }
                        stage.placement = std::move(placement.value().value);
                        hasPlacement = true;
                    } else {
                        return invalid("pipeline YAML: unknown key in stage");
                    }
                }
                if (!hasId || !hasRun) {
                    return invalid("pipeline YAML: stage needs id and run");
                }
                pipeline.stages.push_back(std::move(stage));
            } else {
                Edge edge;
                bool hasFrom = false;
                bool hasTo = false;
                for (const Field& field : fields) {
                    if (field.key == "from") {
                        if (hasFrom) {
                            return invalid("pipeline YAML: duplicate edge key");
                        }
                        auto from = parseScalar(field.value);
                        if (!from.ok()) {
                            return from.error();
                        }
                        edge.from = std::move(from.value().value);
                        hasFrom = true;
                    } else if (field.key == "to") {
                        if (hasTo) {
                            return invalid("pipeline YAML: duplicate edge key");
                        }
                        auto to = parseScalar(field.value);
                        if (!to.ok()) {
                            return to.error();
                        }
                        edge.to = std::move(to.value().value);
                        hasTo = true;
                    } else {
                        return invalid("pipeline YAML: unknown key in edge");
                    }
                }
                if (!hasFrom || !hasTo) {
                    return invalid("pipeline YAML: edge needs from and to");
                }
                pipeline.edges.push_back(std::move(edge));
            }
        }
    }

    if (!sawStages) {
        return invalid("pipeline YAML: missing stages section");
    }
    auto plan = Planner::plan(pipeline);
    if (!plan.ok()) {
        return plan.error();
    }
    return pipeline;
}

} // namespace

psx::Result<Pipeline> loadPipelineYaml(std::string_view yamlText) {
    auto lines = tokenizeLines(yamlText);
    if (!lines.ok()) {
        return lines.error();
    }
    return parseDocument(lines.value());
}

} // namespace psx::pipeline
