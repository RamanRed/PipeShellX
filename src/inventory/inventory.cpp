#include "psx/inventory/inventory.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace psx::inventory {

namespace {

std::string_view trim(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Strips a trailing " # comment" / " ; comment" (only when preceded by space,
// so a '#' inside a value/host token is kept).
std::string_view stripComment(std::string_view line) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        if ((line[i] == '#' || line[i] == ';') && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::vector<std::string_view> splitWhitespace(std::string_view s) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            ++i;
        }
        if (i > start) {
            tokens.push_back(s.substr(start, i - start));
        }
    }
    return tokens;
}

[[noreturn]] void fail(std::size_t lineNumber, const std::string& message) {
    throw std::runtime_error("inventory: line " + std::to_string(lineNumber) + ": " + message);
}

std::uint16_t parsePort(std::string_view value, std::size_t lineNumber) {
    unsigned int port = 0;
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, port);
    if (ec != std::errc{} || ptr != end || port == 0 || port > 65535) {
        fail(lineNumber, "invalid port '" + std::string(value) + "'");
    }
    return static_cast<std::uint16_t>(port);
}

void addUnique(std::vector<std::string>& list, const std::string& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

} // namespace

Host& Inventory::upsert(const std::string& name) {
    for (auto& host : hosts_) {
        if (host.name == name) {
            return host;
        }
    }
    Host host;
    host.name = name;
    host.host = name;
    hosts_.push_back(std::move(host));
    return hosts_.back();
}

Inventory Inventory::parse(std::string_view iniText, const std::string& sourcePath) {
    Inventory inventory;
    inventory.sourcePath_ = sourcePath;

    std::string defaultUser;
    std::string defaultIdentity;
    std::uint16_t defaultPort = 22;
    bool defaultPortSet = false;

    std::string currentGroup;
    bool inDefaults = false;
    std::size_t lineNumber = 0;

    std::size_t pos = 0;
    while (pos <= iniText.size()) {
        const std::size_t nl = iniText.find('\n', pos);
        std::string_view rawLine =
            iniText.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? iniText.size() + 1 : nl + 1;
        ++lineNumber;

        const std::string_view line = trim(stripComment(rawLine));
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[') {
            if (line.back() != ']') {
                fail(lineNumber, "unterminated section header");
            }
            const std::string_view section = trim(line.substr(1, line.size() - 2));
            if (section == "defaults") {
                inDefaults = true;
                currentGroup.clear();
            } else {
                inDefaults = false;
                currentGroup = std::string(section);
            }
            continue;
        }

        if (inDefaults) {
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) {
                fail(lineNumber, "expected key = value in [defaults]");
            }
            const std::string_view key = trim(line.substr(0, eq));
            const std::string_view value = trim(line.substr(eq + 1));
            if (key == "user") {
                defaultUser = std::string(value);
            } else if (key == "identity") {
                defaultIdentity = std::string(value);
            } else if (key == "port") {
                defaultPort = parsePort(value, lineNumber);
                defaultPortSet = true;
            } else {
                fail(lineNumber, "unknown default '" + std::string(key) + "'");
            }
            continue;
        }

        if (currentGroup.empty()) {
            fail(lineNumber, "host '" + std::string(line) + "' appears before any [group]");
        }

        const auto tokens = splitWhitespace(line);
        std::string spec(tokens.front());

        // The host token may be user@host or a bare host.
        std::string user = defaultUser;
        std::string host = spec;
        if (const auto at = spec.find('@'); at != std::string::npos) {
            user = spec.substr(0, at);
            host = spec.substr(at + 1);
        }

        Host& entry = inventory.upsert(host);
        entry.host = host;
        if (!user.empty()) {
            entry.user = user;
        } else if (entry.user.empty()) {
            entry.user = defaultUser;
        }
        if (entry.identity.empty()) {
            entry.identity = defaultIdentity;
        }
        if (defaultPortSet && entry.port == 22) {
            entry.port = defaultPort;
        }
        addUnique(entry.groups, currentGroup);

        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::string_view opt = tokens[i];
            const auto eq = opt.find('=');
            if (eq == std::string_view::npos) {
                fail(lineNumber, "expected key=value option, got '" + std::string(opt) + "'");
            }
            const std::string_view key = opt.substr(0, eq);
            const std::string_view value = opt.substr(eq + 1);
            if (key == "user") {
                entry.user = std::string(value);
            } else if (key == "port") {
                entry.port = parsePort(value, lineNumber);
            } else if (key == "identity") {
                entry.identity = std::string(value);
            } else if (key == "tag") {
                std::size_t start = 0;
                while (start <= value.size()) {
                    const std::size_t comma = value.find(',', start);
                    const std::string_view tag =
                        value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
                    if (!tag.empty()) {
                        addUnique(entry.tags, std::string(tag));
                    }
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    start = comma + 1;
                }
            } else {
                fail(lineNumber, "unknown host option '" + std::string(key) + "'");
            }
        }
    }

    return inventory;
}

Inventory Inventory::importClientsTxt(std::string_view text, const std::string& sourcePath) {
    // Reuse the INI parser by wrapping the entries in an implicit [all] group.
    std::string ini = "[all]\n";
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view raw = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        // ssh://user@host:port -> user@host port=port
        if (line.rfind("ssh://", 0) == 0) {
            std::string_view rest = line.substr(6);
            const auto q = rest.find('?');
            if (q != std::string_view::npos) {
                rest = rest.substr(0, q); // identity query handled by ClientConfig import later
            }
            const auto colon = rest.rfind(':');
            const auto at = rest.find('@');
            if (colon != std::string_view::npos && (at == std::string_view::npos || colon > at)) {
                ini += std::string(rest.substr(0, colon)) + " port=" + std::string(rest.substr(colon + 1)) + "\n";
            } else {
                ini += std::string(rest) + "\n";
            }
        } else {
            ini += std::string(line) + "\n";
        }
    }
    return parse(ini, sourcePath);
}

std::vector<Host> Inventory::selectGroup(std::string_view group) const {
    std::vector<Host> out;
    for (const auto& host : hosts_) {
        if (std::find(host.groups.begin(), host.groups.end(), group) != host.groups.end()) {
            out.push_back(host);
        }
    }
    return out;
}

std::vector<Host> Inventory::selectTag(std::string_view tag) const {
    std::vector<Host> out;
    for (const auto& host : hosts_) {
        if (std::find(host.tags.begin(), host.tags.end(), tag) != host.tags.end()) {
            out.push_back(host);
        }
    }
    return out;
}

std::vector<Host> Inventory::selectHosts(const std::vector<std::string>& names) const {
    std::vector<Host> out;
    for (const auto& name : names) {
        auto it =
            std::find_if(hosts_.begin(), hosts_.end(), [&](const Host& h) { return h.name == name || h.host == name; });
        if (it == hosts_.end()) {
            throw std::runtime_error("inventory: no such host '" + name + "'");
        }
        out.push_back(*it);
    }
    return out;
}

std::vector<std::string> Inventory::groups() const {
    std::vector<std::string> out;
    for (const auto& host : hosts_) {
        for (const auto& group : host.groups) {
            addUnique(out, group);
        }
    }
    return out;
}

std::vector<std::string> Inventory::tags() const {
    std::vector<std::string> out;
    for (const auto& host : hosts_) {
        for (const auto& tag : host.tags) {
            addUnique(out, tag);
        }
    }
    return out;
}

} // namespace psx::inventory
