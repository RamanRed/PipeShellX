// Golden behaviour of ProcessManager as of v0.1.0: these tests pin the
// observable contract (exit codes, captured output, remote output format,
// error classes, password hand-off, timeouts) so that the Phase 1 refactor
// onto psx::os / psx::runtime can be proven behaviour-preserving.

#include <gtest/gtest.h>

#include "client_config.hpp"
#include "command_executor.hpp"
#include "process_manager.hpp"
#include "test_support.hpp"

#include <chrono>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

ClientEntry client(const std::string& user, const std::string& host) {
    ClientEntry entry;
    entry.user = user;
    entry.host = host;
    return entry;
}

LogContext context(const std::string& command) {
    return LogContext{getpid(), "golden", "-", command};
}

class GoldenRemoteTest : public ::testing::Test {
protected:
    test_support::FakeSshOnPath fakeSsh_;
    ProcessManager pm_;
};

} // namespace

TEST(GoldenLocalTest, CapturesBothStreamsAndExitCode) {
    ProcessManager pm;
    auto result = pm.execute({"/bin/sh", "-c", "printf out; printf err >&2; exit 2"}, context("sh"));
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_EQ(result.stdoutData, "out");
    EXPECT_EQ(result.stderrData, "err");
    EXPECT_FALSE(result.timedOut);
    EXPECT_TRUE(result.clientResults.empty());
}

TEST(GoldenLocalTest, FeedsInputToStdinAndClosesIt) {
    ProcessManager pm;
    auto result = pm.execute({"cat"}, context("cat"), "hello from stdin");
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.stdoutData, "hello from stdin");
}

TEST(GoldenLocalTest, LargeOutputOnBothStreamsDoesNotDeadlock) {
    ProcessManager pm;
    auto result = pm.execute(
        {"/bin/sh", "-c", "head -c 300000 /dev/zero | tr '\\0' a; head -c 300000 /dev/zero | tr '\\0' b >&2"},
        context("big"));
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.stdoutData.size(), 300000U);
    EXPECT_EQ(result.stderrData.size(), 300000U);
}

TEST(GoldenLocalTest, SignalTerminatedChildReportsMinusOne) {
    ProcessManager pm;
    auto result = pm.execute({"/bin/sh", "-c", "kill -9 $$"}, context("kill"));
    EXPECT_EQ(result.exitCode, -1);
    EXPECT_FALSE(result.timedOut);
}

TEST(GoldenLocalTest, MissingExecutableIsExitCode127WithStderr) {
    ProcessManager pm;
    auto result = pm.execute({"/no/such/program"}, context("missing"));
    EXPECT_EQ(result.exitCode, 127);
    EXPECT_NE(result.stderrData.find("/no/such/program"), std::string::npos) << result.stderrData;
}

TEST_F(GoldenRemoteTest, OutputIsGroupedPerClientInInputOrder) {
    const std::vector<ClientEntry> clients{client("alice", "h1"), client("bob", "h2")};
    auto result = pm_.executeRemote(clients, "ok", context("ok"), 10);

    EXPECT_EQ(result.exitCode, 0);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.stdoutData, "CLIENT alice@h1\nhost=alice@h1\nCLIENT bob@h2\nhost=bob@h2\n");
    EXPECT_EQ(result.stderrData, "");
    ASSERT_EQ(result.clientResults.size(), 2U);
    EXPECT_EQ(result.clientResults[0].clientId, "alice@h1");
    EXPECT_EQ(result.clientResults[0].stdoutData, "host=alice@h1\n");
    EXPECT_EQ(result.clientResults[0].exitCode, 0);
    EXPECT_TRUE(result.clientResults[0].errorMessage.empty());
    EXPECT_EQ(result.clientResults[1].clientId, "bob@h2");
}

TEST_F(GoldenRemoteTest, PartialFailureKeepsEveryClientAndAggregatesTheExitCode) {
    const std::vector<ClientEntry> clients{client("a", "ok-host"), client("b", "bad-host")};
    // The fake ssh keys off the remote command, so use two calls to mix outcomes.
    auto good = pm_.executeRemote({clients[0]}, "ok", context("ok"), 10);
    auto bad = pm_.executeRemote({clients[1]}, "fail 3", context("fail"), 10);
    EXPECT_EQ(good.exitCode, 0);
    EXPECT_EQ(bad.exitCode, 3);
    ASSERT_EQ(bad.clientResults.size(), 1U);
    EXPECT_EQ(bad.clientResults[0].stderrData, "failing\n");
    EXPECT_EQ(bad.clientResults[0].errorMessage, "ERROR: command failed with exit code 3");
    // The aggregated stderr view prefers the normalized error over raw stderr.
    EXPECT_EQ(bad.stderrData, "CLIENT b@bad-host\nERROR: command failed with exit code 3\n");
}

TEST_F(GoldenRemoteTest, SshFailuresAreClassified) {
    const ClientEntry c = client("u", "h");
    EXPECT_EQ(pm_.executeRemote({c}, "refused", context("refused"), 10).clientResults[0].errorMessage,
              "ERROR: connection failed");
    EXPECT_EQ(pm_.executeRemote({c}, "denied", context("denied"), 10).clientResults[0].errorMessage,
              "ERROR: authentication failed");
    EXPECT_EQ(pm_.executeRemote({c}, "hostkey", context("hostkey"), 10).clientResults[0].errorMessage,
              "ERROR: host key verification failed");
    EXPECT_EQ(pm_.executeRemote({c}, "refused", context("refused"), 10).exitCode, 255);
}

TEST_F(GoldenRemoteTest, PasswordReachesSshpassThroughTheDescriptor) {
    ClientEntry c = client("u", "h");
    c.password = "s3cret pa$$";
    auto result = pm_.executeRemote({c}, "pw", context("pw"), 10);
    EXPECT_EQ(result.exitCode, 0) << result.stderrData;
    EXPECT_EQ(result.clientResults[0].stdoutData, "pw=s3cret pa$$\n");
}

TEST_F(GoldenRemoteTest, TimeoutKillsHungWorkersAndReportsIt) {
    const std::vector<ClientEntry> clients{client("u", "h1"), client("u", "h2")};
    const auto start = std::chrono::steady_clock::now();
    auto result = pm_.executeRemote(clients, "hang", context("hang"), 1);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(result.timedOut);
    EXPECT_NE(result.exitCode, 0);
    ASSERT_EQ(result.clientResults.size(), 2U);
    for (const auto& clientResult : result.clientResults) {
        EXPECT_TRUE(clientResult.timedOut);
        EXPECT_EQ(clientResult.errorMessage, "ERROR: command timed out");
    }
    EXPECT_GE(elapsed, std::chrono::milliseconds(900));
    EXPECT_LT(elapsed, std::chrono::seconds(4));
}

TEST_F(GoldenRemoteTest, LargeRemoteOutputIsCapturedCompletely) {
    auto result = pm_.executeRemote({client("u", "h")}, "big", context("big"), 20);
    EXPECT_EQ(result.exitCode, 0);
    ASSERT_EQ(result.clientResults.size(), 1U);
    EXPECT_EQ(result.clientResults[0].stdoutData.size(), 200000U);
    EXPECT_EQ(result.clientResults[0].stderrData.size(), 100000U);
}

TEST_F(GoldenRemoteTest, CommandExecutorStreamsHeadersAndLinesPerClient) {
    test_support::ScopedTempCwd cwd("golden-executor");
    CommandExecutor executor;
    std::vector<std::string> lines;
    std::vector<bool> isStdout;
    // The allowlisted command is single-quoted per argument ('echo' 'hi'), which
    // the fake ssh rejects on stderr with exit 9 — enough to pin the contract:
    // one "CLIENT <id>" header line, then output/error lines, per client.
    auto result =
        executor.executeOnClients("echo hi", {client("alice", "h1")}, "golden", [&](const std::string& line, bool out) {
            lines.push_back(line);
            isStdout.push_back(out);
        });
    ASSERT_GE(lines.size(), 2U);
    EXPECT_EQ(lines[0], "CLIENT alice@h1");
    EXPECT_TRUE(isStdout[0]);
    EXPECT_EQ(lines[1], "ERROR: command failed with exit code 9");
    EXPECT_FALSE(isStdout[1]);
    ASSERT_EQ(result.clientResults.size(), 1U);
    EXPECT_EQ(result.exitCode, 9);
}
