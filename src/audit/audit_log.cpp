#include "psx/audit/audit_log.hpp"

#include "psx/json/json.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>

namespace psx::audit {

namespace {

std::int64_t nowMillis() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

} // namespace

AuditLog::AuditLog(const std::string& path) {
    std::error_code ignored;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    out_.open(path, std::ios::app);
}

void AuditLog::writeLine(const std::string& body) {
    if (!out_) {
        return;
    }
    out_ << "{\"ts_ms\":" << nowMillis() << ',' << body << "}\n";
    out_.flush(); // durability over throughput: an audit line must survive a crash
}

void AuditLog::runStarted(const std::string& runId, const std::string& command, std::size_t hostCount) {
    std::ostringstream body;
    body << "\"event\":\"run_started\",\"run_id\":" << json::quote(runId) << ",\"command\":" << json::quote(command)
         << ",\"hosts\":" << hostCount;
    writeLine(body.str());
}

void AuditLog::stageFinished(const std::string& runId, const StageRecord& record) {
    std::ostringstream body;
    body << "\"event\":\"stage_finished\",\"run_id\":" << json::quote(runId)
         << ",\"stage\":" << json::quote(record.stageId) << ",\"host\":" << json::quote(record.host)
         << ",\"exit_code\":" << record.exitCode << ",\"attempts\":" << record.attempts
         << ",\"timed_out\":" << json::boolean(record.timedOut) << ",\"cancelled\":" << json::boolean(record.cancelled)
         << ",\"aborted\":" << json::boolean(record.aborted) << ",\"dropped_bytes\":" << record.droppedBytes
         << ",\"error\":" << json::quote(record.error);
    writeLine(body.str());
}

void AuditLog::runFinished(const std::string& runId, const RunRecord& record) {
    std::ostringstream body;
    body << "\"event\":\"run_finished\",\"run_id\":" << json::quote(runId) << ",\"total\":" << record.total
         << ",\"succeeded\":" << record.succeeded << ",\"failed\":" << record.failed
         << ",\"cancelled\":" << json::boolean(record.cancelled) << ",\"exit_code\":" << record.exitCode;
    writeLine(body.str());
}

} // namespace psx::audit
