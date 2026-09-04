#include "psx/runtime/cluster_snapshot.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace psx::runtime {

namespace {

psx::Error malformed(const char* what) {
    return psx::Error{psx::ErrorClass::Other, 0, what};
}

// Unescapes a JSON string literal (content between quotes).
std::string unescapeJson(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[++i];
            switch (next) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += next; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

std::optional<std::string> extractStringField(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const auto pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = pos + needle.size();
    std::size_t end = start;
    while (end < json.size()) {
        if (json[end] == '"' && (end == 0 || json[end - 1] != '\\')) {
            break;
        }
        ++end;
    }
    if (end >= json.size()) {
        return std::nullopt;
    }
    return unescapeJson(json.substr(start, end - start));
}

std::optional<std::int64_t> extractIntField(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const auto pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    auto start = pos + needle.size();
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {
        ++start;
    }
    auto end = start;
    if (end < json.size() && (json[end] == '-' || json[end] == '+')) {
        ++end;
    }
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
        ++end;
    }
    if (end == start) {
        return std::nullopt;
    }
    try {
        return std::stoll(std::string(json.substr(start, end - start)));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

ClusterSnapshot::ClusterSnapshot(std::string runId)
    : runId_(std::move(runId)), tsEpochMs_(nowMillis()) {}

ClusterSnapshot::ClusterSnapshot(std::string runId, std::int64_t tsEpochMs)
    : runId_(std::move(runId)), tsEpochMs_(tsEpochMs) {}

std::string ClusterSnapshot::toJsonLine() const {
    std::ostringstream body;
    body << "{\"type\":\"cluster_snapshot\",\"run_id\":" << json::quote(runId_)
         << ",\"ts_epoch_ms\":" << tsEpochMs_ << ",\"nodes\":[";
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const NodeSnapshot& n = nodes_[i];
        if (i != 0) {
            body << ',';
        }
        body << "{\"host\":" << json::quote(n.host) << ",\"stage_id\":" << json::quote(n.stageId)
             << ",\"status\":" << json::quote(n.status) << ",\"exit_code\":" << n.exitCode
             << ",\"lamport_ts\":" << n.lamportTs << "}";
    }
    body << "]}";
    return body.str();
}

bool ClusterSnapshot::appendToFile(const std::string& path) const {
    std::error_code ignored;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    std::ofstream out(path, std::ios::app);
    if (!out) {
        return false;
    }
    out << toJsonLine() << '\n';
    return static_cast<bool>(out);
}

psx::Result<ClusterSnapshot> ClusterSnapshot::fromJsonLine(std::string_view line) {
    if (line.find(R"("type":"cluster_snapshot")") == std::string_view::npos) {
        return malformed("not a cluster_snapshot line");
    }
    auto runId = extractStringField(line, "run_id");
    if (!runId) {
        return malformed("missing run_id");
    }
    auto tsEpoch = extractIntField(line, "ts_epoch_ms");
    const std::int64_t ts = tsEpoch.value_or(0);

    ClusterSnapshot snapshot(std::move(*runId), ts);

    const auto nodesStart = line.find("\"nodes\":[");
    if (nodesStart != std::string_view::npos) {
        auto pos = nodesStart + 8; // points at '['
        const auto nodesEnd = line.find(']', pos);
        if (nodesEnd == std::string_view::npos) {
            return malformed("malformed nodes array");
        }
        std::string_view nodesSub = line.substr(pos + 1, nodesEnd - (pos + 1));
        std::size_t objStart = 0;
        while ((objStart = nodesSub.find('{', objStart)) != std::string_view::npos) {
            const auto objEnd = nodesSub.find('}', objStart);
            if (objEnd == std::string_view::npos) {
                return malformed("malformed node object");
            }
            std::string_view nodeObj = nodesSub.substr(objStart, objEnd - objStart + 1);
            NodeSnapshot node;
            node.host = extractStringField(nodeObj, "host").value_or("");
            node.stageId = extractStringField(nodeObj, "stage_id").value_or("");
            node.status = extractStringField(nodeObj, "status").value_or("");
            node.exitCode = static_cast<int>(extractIntField(nodeObj, "exit_code").value_or(0));
            node.lamportTs = static_cast<std::uint64_t>(extractIntField(nodeObj, "lamport_ts").value_or(0));
            snapshot.record(std::move(node));
            objStart = objEnd + 1;
        }
    }
    return snapshot;
}

psx::Result<std::vector<ClusterSnapshot>> ClusterSnapshot::readFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return malformed("cannot open snapshot file");
    }
    std::vector<ClusterSnapshot> result;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto parsed = fromJsonLine(line);
        if (parsed.ok()) {
            result.push_back(std::move(parsed.value()));
        }
    }
    return result;
}

std::string ClusterSnapshot::formatTable() const {
    std::ostringstream out;
    out << "================================================================================\n";
    out << "Cluster Snapshot [Run: " << runId_ << " | Timestamp: " << tsEpochMs_ << " ms]\n";
    out << "================================================================================\n";
    out << std::left << std::setw(24) << "HOST"
        << std::left << std::setw(16) << "STAGE"
        << std::left << std::setw(14) << "STATUS"
        << std::left << std::setw(10) << "EXIT"
        << "LAMPORT_TS\n";
    out << "--------------------------------------------------------------------------------\n";
    for (const auto& node : nodes_) {
        std::string exitStr = (node.status == "exited") ? std::to_string(node.exitCode) : "-";
        out << std::left << std::setw(24) << node.host
            << std::left << std::setw(16) << (node.stageId.empty() ? "-" : node.stageId)
            << std::left << std::setw(14) << node.status
            << std::left << std::setw(10) << exitStr
            << node.lamportTs << "\n";
    }
    out << "================================================================================\n";
    return out.str();
}

} // namespace psx::runtime
