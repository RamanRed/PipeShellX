#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

namespace psx::audit {

// One stage's outcome, as recorded in the audit trail.
struct StageRecord {
    std::string host;
    std::string stageId;
    int exitCode = 0;
    int attempts = 1;
    bool timedOut = false;
    bool cancelled = false;
    bool aborted = false;
    std::uint64_t droppedBytes = 0;
    std::string error; // classified error message (may be empty)
};

// A run's aggregate outcome.
struct RunRecord {
    std::size_t total = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    int exitCode = 0;
};

// Appends one JSON object per line (JSONL): a run_started, one stage_finished
// per host, and a run_finished, all sharing the run's run_id and each carrying
// an epoch-millisecond timestamp. Opens the file in append mode (creating parent
// directories); an open failure is silent — ok() reports it — so an unwritable
// audit path degrades to "no audit", never aborting the run.
class AuditLog {
public:
    explicit AuditLog(const std::string& path);

    bool ok() const noexcept { return static_cast<bool>(out_); }

    void runStarted(const std::string& runId, const std::string& command, std::size_t hostCount);
    void stageFinished(const std::string& runId, const StageRecord& record);
    void runFinished(const std::string& runId, const RunRecord& record);

private:
    void writeLine(const std::string& body);

    std::ofstream out_;
};

} // namespace psx::audit
