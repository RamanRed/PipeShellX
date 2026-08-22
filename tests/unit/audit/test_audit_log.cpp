#include "psx/audit/audit_log.hpp"

#include "test_support.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

std::vector<std::string> readLines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST(AuditLogTest, WritesOneJsonlRecordPerEvent) {
    test_support::ScopedTempCwd cwd("audit");
    const std::string path = (cwd.path() / "audit.jsonl").string();
    {
        psx::audit::AuditLog audit(path);
        ASSERT_TRUE(audit.ok());
        audit.runStarted("run-abc", "uptime", 2);
        audit.stageFinished("run-abc", {.host = "u@h1", .stageId = "s0", .exitCode = 0, .attempts = 1});
        audit.stageFinished("run-abc", {.host = "u@h2",
                                        .stageId = "s1",
                                        .exitCode = 255,
                                        .attempts = 3,
                                        .timedOut = false,
                                        .error = "ERROR: connection failed"});
        audit.runFinished("run-abc", {.total = 2, .succeeded = 1, .failed = 1, .cancelled = false, .exitCode = 1});
    }

    const auto lines = readLines(path);
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_NE(lines[0].find(R"("event":"run_started")"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find(R"("run_id":"run-abc")"), std::string::npos);
    EXPECT_NE(lines[0].find(R"("command":"uptime")"), std::string::npos);
    EXPECT_NE(lines[0].find(R"("hosts":2)"), std::string::npos);
    EXPECT_NE(lines[0].find(R"("ts_ms":)"), std::string::npos);

    EXPECT_NE(lines[1].find(R"("event":"stage_finished")"), std::string::npos);
    EXPECT_NE(lines[1].find(R"("stage":"s0")"), std::string::npos);
    EXPECT_NE(lines[1].find(R"("host":"u@h1")"), std::string::npos);
    EXPECT_NE(lines[1].find(R"("exit_code":0)"), std::string::npos);

    EXPECT_NE(lines[2].find(R"("exit_code":255)"), std::string::npos);
    EXPECT_NE(lines[2].find(R"("attempts":3)"), std::string::npos);
    EXPECT_NE(lines[2].find(R"("error":"ERROR: connection failed")"), std::string::npos);

    EXPECT_NE(lines[3].find(R"("event":"run_finished")"), std::string::npos);
    EXPECT_NE(lines[3].find(R"("succeeded":1)"), std::string::npos);
    EXPECT_NE(lines[3].find(R"("failed":1)"), std::string::npos);
    EXPECT_NE(lines[3].find(R"("exit_code":1)"), std::string::npos);
}

TEST(AuditLogTest, AppendsAcrossReopen) {
    test_support::ScopedTempCwd cwd("audit-append");
    const std::string path = (cwd.path() / "a.jsonl").string();
    { psx::audit::AuditLog(path).runStarted("r1", "a", 1); }
    { psx::audit::AuditLog(path).runStarted("r2", "b", 1); }
    EXPECT_EQ(readLines(path).size(), 2U); // append mode, not truncate
}

TEST(AuditLogTest, EscapesJsonSpecialCharacters) {
    test_support::ScopedTempCwd cwd("audit-escape");
    const std::string path = (cwd.path() / "e.jsonl").string();
    { psx::audit::AuditLog(path).runStarted("r", "echo \"hi\"\tthere", 1); }
    const auto lines = readLines(path);
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find(R"(echo \"hi\"\tthere)"), std::string::npos) << lines[0];
}

TEST(AuditLogTest, UnwritablePathIsNotOk) {
    psx::audit::AuditLog audit("/this/path/does/not/exist/and/cannot/be/made/a.jsonl");
    EXPECT_FALSE(audit.ok());
}
