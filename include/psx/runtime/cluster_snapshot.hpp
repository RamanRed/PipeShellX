#pragma once

#include "psx/json/json.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace psx::runtime {

// One node's state as of the moment it was record()-ed into a
// ClusterSnapshot. See docs/ds-project/02-cluster-snapshot.md for the
// simplifications versus a full Chandy-Lamport snapshot (this captures
// local state only, no in-flight channel bytes).
struct NodeSnapshot {
    std::string host;
    std::string stageId;         // empty if nothing running on this host right now
    std::string status;          // e.g. "running", "exited", "connecting", "lost"
    int exitCode = 0;            // meaningful only when status == "exited"
    std::uint64_t lamportTs = 0; // 0 if unknown / peer has no LamportClock wired in yet
};

// A point-in-time capture across every tracked node in a run. Passive: the
// caller decides when "now" is and calls record() once per host, then
// serializes. This class never polls the network itself.
class ClusterSnapshot {
public:
    explicit ClusterSnapshot(std::string runId) : runId_(std::move(runId)) {}

    // Records one node's state. Call once per host per capture round; call
    // record() again with a fresh ClusterSnapshot instance for the next
    // round rather than reusing one instance across rounds, so nodes()
    // reflects exactly one point in time.
    void record(NodeSnapshot node) { nodes_.push_back(std::move(node)); }

    const std::vector<NodeSnapshot>& nodes() const noexcept { return nodes_; }

    // One JSON object, no trailing newline:
    // {"type":"cluster_snapshot","run_id":"...","ts_epoch_ms":...,
    //  "nodes":[{"host":...,"stage_id":...,"status":...,"exit_code":...,
    //  "lamport_ts":...}, ...]}
    std::string toJsonLine() const {
        std::ostringstream body;
        body << "{\"type\":\"cluster_snapshot\",\"run_id\":" << json::quote(runId_)
             << ",\"ts_epoch_ms\":" << nowMillis() << ",\"nodes\":[";
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

    // Appends toJsonLine() + '\n' to `path`, creating missing parent
    // directories, matching psx::audit::AuditLog's degrade-to-false-never-
    // throw behaviour on an unwritable path.
    bool appendToFile(const std::string& path) const {
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

private:
    static std::int64_t nowMillis() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    std::string runId_;
    std::vector<NodeSnapshot> nodes_;
};

} // namespace psx::runtime
