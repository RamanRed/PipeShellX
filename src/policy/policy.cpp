#include "psx/policy/policy.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace psx::policy {

namespace {

std::string_view trim(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// argv[0] and every argument may not contain a shell metacharacter — a
// defence-in-depth layer on top of the no-shell execvp/ssh model.
bool hasShellMetacharacter(std::string_view value) {
    return value.find_first_of(";|&$`<>\\") != std::string_view::npos;
}

} // namespace

Policy Policy::parse(std::string_view text, const std::string& /*sourcePath*/) {
    Policy policy;
    std::size_t lineNumber = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view raw = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++lineNumber;

        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        const auto space = line.find_first_of(" \t");
        const std::string_view directive = line.substr(0, space);
        const std::string_view argument =
            space == std::string_view::npos ? std::string_view{} : trim(line.substr(space));

        auto fail = [&](const std::string& message) {
            throw std::runtime_error("policy: line " + std::to_string(lineNumber) + ": " + message);
        };

        if (directive == "allow") {
            if (argument.empty()) {
                fail("allow needs a command name");
            }
            policy.allowed_.emplace(argument);
        } else if (directive == "max-args") {
            std::size_t value = 0;
            const auto* end = argument.data() + argument.size();
            const auto [ptr, ec] = std::from_chars(argument.data(), end, value);
            if (ec != std::errc{} || ptr != end || argument.empty()) {
                fail("max-args needs a non-negative integer");
            }
            policy.maxArgs_ = value;
        } else if (directive == "allow-shell-metacharacters") {
            policy.allowShellMetacharacters_ = true;
        } else {
            fail("unknown directive '" + std::string(directive) + "'");
        }
    }
    return policy;
}

Policy Policy::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("policy: cannot open '" + path + "'");
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str(), path);
}

std::optional<std::string> Policy::validate(const std::vector<std::string>& argv) const {
    if (argv.empty()) {
        return "empty command";
    }
    const std::string& command = argv.front();

    if (!allowed_.empty()) {
        if (command.find('/') != std::string::npos) {
            return "explicit paths are not allowed by the policy: '" + command + "'";
        }
        if (allowed_.count(command) == 0) {
            return "command '" + command + "' is not allowed by the policy";
        }
    }
    if (maxArgs_.has_value() && argv.size() > *maxArgs_) {
        return "too many arguments (" + std::to_string(argv.size()) + " > " + std::to_string(*maxArgs_) + ")";
    }
    if (!allowShellMetacharacters_) {
        for (const auto& token : argv) {
            if (hasShellMetacharacter(token)) {
                return "argument contains an unsafe shell metacharacter: '" + token + "'";
            }
        }
    }
    return std::nullopt;
}

} // namespace psx::policy
