// Golden behaviour of ProcessManager as of v0.1.0: these tests pin the
// observable contract (exit codes, captured output, remote output format,
// error classes, password hand-off, timeouts) so that the Phase 1 refactor
// onto psx::os / psx::runtime can be proven behaviour-preserving.

#include <gtest/gtest.h>

using namespace std::chrono_literals;

#include "client_config.hpp"
#include "command_executor.hpp"
#include "process_manager.hpp"
#include "psx/sink/group_sink.hpp"
#include "psx/sink/json_sink.hpp"
#include "psx/sink/stream_sink.hpp"
#include "test_support.hpp"

#include "psx/stream/bounded_buffer.hpp"

#include <chrono>
#include <sstream>

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
    return LogContext{.pid = getpid(), .sessionId = "golden", .clientId = "-", .command = command};
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
    auto result = pm_.executeRemote(clients, "ok", context("ok"), {.timeoutSec = 10});

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
    auto good = pm_.executeRemote({clients[0]}, "ok", context("ok"), {.timeoutSec = 10});
    auto bad = pm_.executeRemote({clients[1]}, "fail 3", context("fail"), {.timeoutSec = 10});
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
    EXPECT_EQ(pm_.executeRemote({c}, "refused", context("refused"), {.timeoutSec = 10}).clientResults[0].errorMessage,
              "ERROR: connection failed");
    EXPECT_EQ(pm_.executeRemote({c}, "denied", context("denied"), {.timeoutSec = 10}).clientResults[0].errorMessage,
              "ERROR: authentication failed");
    EXPECT_EQ(pm_.executeRemote({c}, "hostkey", context("hostkey"), {.timeoutSec = 10}).clientResults[0].errorMessage,
              "ERROR: host key verification failed");
    EXPECT_EQ(pm_.executeRemote({c}, "refused", context("refused"), {.timeoutSec = 10}).exitCode, 255);
}

TEST_F(GoldenRemoteTest, RemoteCommandStderrIsNotMisreadAsAnSshFailure) {
    // The remote command runs (ssh exits with the command's code, not 255) and
    // prints text that looks like an ssh diagnostic; it must be reported as a
    // plain command failure, never "authentication failed".
    auto denied =
        pm_.executeRemote({client("u", "h")}, "fail 13:Permission denied", context("cmd"), {.timeoutSec = 10});
    ASSERT_EQ(denied.clientResults.size(), 1U);
    EXPECT_EQ(denied.clientResults[0].exitCode, 13);
    EXPECT_EQ(denied.clientResults[0].errorMessage, "ERROR: command failed with exit code 13");

    // A genuine ssh failure (exit 255) is still classified.
    auto refused = pm_.executeRemote({client("u", "h")}, "refused", context("ssh"), {.timeoutSec = 10});
    EXPECT_EQ(refused.clientResults[0].errorMessage, "ERROR: connection failed");
}

TEST_F(GoldenRemoteTest, PasswordReachesSshpassThroughTheDescriptor) {
    ClientEntry c = client("u", "h");
    c.password = "s3cret pa$$";
    auto result = pm_.executeRemote({c}, "pw", context("pw"), {.timeoutSec = 10});
    EXPECT_EQ(result.exitCode, 0) << result.stderrData;
    EXPECT_EQ(result.clientResults[0].stdoutData, "pw=s3cret pa$$\n");
}

TEST_F(GoldenRemoteTest, TimeoutKillsHungWorkersAndReportsIt) {
    const std::vector<ClientEntry> clients{client("u", "h1"), client("u", "h2")};
    const auto start = std::chrono::steady_clock::now();
    auto result = pm_.executeRemote(clients, "hang", context("hang"), {.timeoutSec = 1});
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
    auto result = pm_.executeRemote({client("u", "h")}, "big", context("big"), {.timeoutSec = 20});
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
    // The allowlisted command is single-quoted per argument ('echo' 'hi'); the
    // fake ssh echoes it back. Pins the streaming contract: one "CLIENT <id>"
    // header line, then the output line, per client.
    auto result =
        executor.executeOnClients("echo hi", {client("alice", "h1")}, "golden", [&](const std::string& line, bool out) {
            lines.push_back(line);
            isStdout.push_back(out);
        });
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], "CLIENT alice@h1");
    EXPECT_TRUE(isStdout[0]);
    EXPECT_EQ(lines[1], "hi");
    EXPECT_TRUE(isStdout[1]);
    ASSERT_EQ(result.clientResults.size(), 1U);
    EXPECT_EQ(result.exitCode, 0);
}

// --- Sink integration: executeRemote streams live, framed, per-stage lines ---

TEST_F(GoldenRemoteTest, GroupSinkRendersEachClientBlock) {
    std::ostringstream out;
    psx::sink::GroupSink sink(out);
    const std::vector<ClientEntry> clients{client("alice", "h1"), client("bob", "h2")};
    auto result = pm_.executeRemote(clients, "ok", context("ok"), {.timeoutSec = 10, .sink = &sink});
    EXPECT_EQ(result.exitCode, 0);
    // Grouped per client, in client order, headers + stdout lines.
    EXPECT_EQ(out.str(), "CLIENT alice@h1\nhost=alice@h1\nCLIENT bob@h2\nhost=bob@h2\n");
}

TEST_F(GoldenRemoteTest, GroupSinkRendersTheNormalizedErrorForAFailure) {
    std::ostringstream out;
    psx::sink::GroupSink sink(out);
    pm_.executeRemote({client("u", "h")}, "refused", context("refused"), {.timeoutSec = 10, .sink = &sink});
    EXPECT_EQ(out.str(), "CLIENT u@h\nERROR: connection failed\n");
}

TEST_F(GoldenRemoteTest, StreamSinkEmitsHostTaggedLinesLive) {
    std::ostringstream out;
    std::ostringstream err;
    psx::sink::StreamSink sink(out, err, /*colour=*/false);
    pm_.executeRemote({client("u", "h")}, "big", context("big"), {.timeoutSec = 20, .sink = &sink});
    // 200 000 'o' bytes with no newline -> one very long line, host-tagged.
    const std::string text = out.str();
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.rfind("[u@h] ", 0), 0U) << text.substr(0, 40);
    EXPECT_NE(text.find("oooo"), std::string::npos);
    EXPECT_NE(err.str().find("1/1 ok"), std::string::npos) << err.str();
}

TEST_F(GoldenRemoteTest, JsonSinkEmitsOneObjectPerStageAndASummary) {
    std::ostringstream out;
    psx::sink::JsonSink sink(out);
    const std::vector<ClientEntry> clients{client("a", "h1"), client("b", "h2")};
    // a: ok (exit 0); b: fail 3
    pm_.executeRemote({clients[0]}, "ok", context("ok"), {.timeoutSec = 10, .sink = &sink});
    pm_.executeRemote({clients[1]}, "fail 3", context("fail"), {.timeoutSec = 10, .sink = &sink});
    const std::string text = out.str();
    EXPECT_NE(text.find(R"("stage":"a@h1")"), std::string::npos) << text;
    EXPECT_NE(text.find(R"("stdout":"host=a@h1")"), std::string::npos) << text;
    EXPECT_NE(text.find(R"("stage":"b@h2","exit":3)"), std::string::npos) << text;
    EXPECT_NE(text.find(R"("summary":true)"), std::string::npos) << text;
}

TEST_F(GoldenRemoteTest, SinkAndResultCaptureAgree) {
    std::ostringstream out;
    psx::sink::GroupSink sink(out);
    auto result = pm_.executeRemote({client("alice", "h1")}, "ok", context("ok"), {.timeoutSec = 10, .sink = &sink});
    // The Result still carries the full capture (API unchanged) and matches.
    ASSERT_EQ(result.clientResults.size(), 1U);
    EXPECT_EQ(result.clientResults[0].stdoutData, "host=alice@h1\n");
    EXPECT_EQ(out.str(), "CLIENT alice@h1\nhost=alice@h1\n");
}

// --- Sliding-window scheduler (-c concurrency) ---

TEST_F(GoldenRemoteTest, LowConcurrencyStillRunsEveryHostCorrectly) {
    std::vector<ClientEntry> clients;
    for (int i = 0; i < 6; ++i) {
        clients.push_back(client("u" + std::to_string(i), "h" + std::to_string(i)));
    }
    // Window of 2: workers spawn in waves but every one must run and report.
    auto result = pm_.executeRemote(clients, "ok", context("ok"), {.timeoutSec = 20, .concurrency = 2});
    EXPECT_EQ(result.exitCode, 0);
    ASSERT_EQ(result.clientResults.size(), 6U);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(result.clientResults[i].clientId, "u" + std::to_string(i) + "@h" + std::to_string(i));
        EXPECT_EQ(result.clientResults[i].stdoutData, "host=u" + std::to_string(i) + "@h" + std::to_string(i) + "\n");
    }
}

TEST_F(GoldenRemoteTest, ConcurrencyBoundsHowManyWorkersRunAtOnce) {
    std::vector<ClientEntry> clients;
    for (int i = 0; i < 4; ++i) {
        clients.push_back(client("u" + std::to_string(i), "h" + std::to_string(i)));
    }
    // Each `slow` worker sleeps ~0.3 s. Serialised (-c 1) that is ~1.2 s;
    // fully parallel (-c 4) it is ~0.3 s. The gap proves the window throttles.
    const auto t0 = std::chrono::steady_clock::now();
    auto serial = pm_.executeRemote(clients, "slow", context("slow"), {.timeoutSec = 30, .concurrency = 1});
    const auto serialTime = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(serial.exitCode, 0) << serial.stderrData;

    const auto t1 = std::chrono::steady_clock::now();
    auto parallel = pm_.executeRemote(clients, "slow", context("slow"), {.timeoutSec = 30, .concurrency = 4});
    const auto parallelTime = std::chrono::steady_clock::now() - t1;
    EXPECT_EQ(parallel.exitCode, 0);

    EXPECT_GE(serialTime, std::chrono::milliseconds(1000)) << "4x0.3s serialised should exceed 1s";
    EXPECT_LT(parallelTime, std::chrono::milliseconds(900)) << "4x0.3s in parallel should be well under 1s";
    EXPECT_GT(serialTime, parallelTime * 2) << "the window clearly throttles concurrency";
}

TEST_F(GoldenRemoteTest, TimeoutWithPendingWorkersDoesNotHang) {
    std::vector<ClientEntry> clients;
    for (int i = 0; i < 6; ++i) {
        clients.push_back(client("u" + std::to_string(i), "h" + std::to_string(i)));
    }
    // Window 1, each host sleeps 0.3 s, 1 s deadline: only ~3 run before the
    // deadline; the pending ones must be reported timed-out, not hang the run.
    const auto t0 = std::chrono::steady_clock::now();
    auto result = pm_.executeRemote(clients, "slow", context("slow"), {.timeoutSec = 1, .concurrency = 1});
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_TRUE(result.timedOut);
    EXPECT_EQ(result.clientResults.size(), 6U);
    EXPECT_LT(elapsed, std::chrono::seconds(4)) << "must not wait for the un-spawned workers";
}

// --- Bounded output capture (--policy / --ring): flat RSS for streaming ---

TEST_F(GoldenRemoteTest, DropOldestRingBoundsTheCapturedOutput) {
    // `big` writes 200000 'o' to stdout and 100000 'e' to stderr. A 64 KiB ring
    // with drop-oldest keeps only the newest 65536 bytes of each; the rest is
    // dropped and counted.
    auto result = pm_.executeRemote(
        {client("u", "h")}, "big", context("big"),
        {.timeoutSec = 20, .concurrency = 1, .policy = psx::stream::OverflowPolicy::DropOldest, .ringBytes = 65536});
    ASSERT_EQ(result.clientResults.size(), 1U);
    EXPECT_EQ(result.clientResults[0].stdoutData.size(), 65536U);
    EXPECT_EQ(result.clientResults[0].stderrData.size(), 65536U);
    EXPECT_EQ(result.clientResults[0].droppedBytes, (200000U - 65536U) + (100000U - 65536U));
}

TEST_F(GoldenRemoteTest, DropNewestRingKeepsTheOldestBytes) {
    auto result = pm_.executeRemote(
        {client("u", "h")}, "big", context("big"),
        {.timeoutSec = 20, .concurrency = 1, .policy = psx::stream::OverflowPolicy::DropNewest, .ringBytes = 65536});
    EXPECT_EQ(result.clientResults[0].stdoutData.size(), 65536U);
    EXPECT_EQ(result.clientResults[0].droppedBytes, (200000U - 65536U) + (100000U - 65536U));
}

TEST_F(GoldenRemoteTest, BlockPolicyIgnoresTheRingAndCapturesEverything) {
    auto result = pm_.executeRemote(
        {client("u", "h")}, "big", context("big"),
        {.timeoutSec = 20, .concurrency = 1, .policy = psx::stream::OverflowPolicy::Block, .ringBytes = 65536});
    EXPECT_EQ(result.clientResults[0].stdoutData.size(), 200000U);
    EXPECT_EQ(result.clientResults[0].droppedBytes, 0U);
}

TEST_F(GoldenRemoteTest, ADropRingStillStreamsEveryByteToTheSink) {
    // The sink sees the full output live even though the Result capture is
    // capped — the ring bounds memory, not the stream.
    std::ostringstream out;
    std::ostringstream err;
    psx::sink::StreamSink sink(out, err, false);
    pm_.executeRemote({client("u", "h")}, "big", context("big"),
                      {.timeoutSec = 20,
                       .sink = &sink,
                       .concurrency = 1,
                       .policy = psx::stream::OverflowPolicy::DropOldest,
                       .ringBytes = 4096});
    // 200000 'o' with no newline -> one host-tagged line of exactly that length.
    const std::string prefix = "[u@h] ";
    ASSERT_EQ(out.str().rfind(prefix, 0), 0U);
    EXPECT_EQ(out.str().size(), prefix.size() + 200000U + 1U) << "sink received all 200000 bytes";
    // The summary reports drops.
    EXPECT_NE(err.str().find("dropped"), std::string::npos) << err.str();
}

// --- Retries: transient transport failures back off and retry (Phase 2) ---

TEST_F(GoldenRemoteTest, PersistentTransientFailureExhaustsRetries) {
    // `refused` fails with "Connection refused" (exit 255) every time: with two
    // retries it is attempted three times, then reported as a connection failure.
    auto result =
        pm_.executeRemote({client("u", "h")}, "refused", context("refused"),
                          {.timeoutSec = 10, .maxRetries = 2, .retryBaseDelay = 20ms, .retryMaxDelay = 200ms});
    ASSERT_EQ(result.clientResults.size(), 1U);
    EXPECT_EQ(result.clientResults[0].attempts, 3); // 1 initial + 2 retries
    EXPECT_EQ(result.clientResults[0].exitCode, 255);
    EXPECT_EQ(result.clientResults[0].errorMessage, "ERROR: connection failed");
}

TEST_F(GoldenRemoteTest, ATransientFailureRecoversOnRetry) {
    test_support::ScopedTempCwd cwd("flaky");
    test_support::ScopedEnv okOn("PSX_FLAKY_OK_ON", std::string("2"));           // succeed on attempt 2
    test_support::ScopedEnv file("PSX_FLAKY_FILE", (cwd.path() / "n").string()); // counter file
    auto result =
        pm_.executeRemote({client("u", "h")}, "flaky", context("flaky"),
                          {.timeoutSec = 10, .maxRetries = 3, .retryBaseDelay = 20ms, .retryMaxDelay = 200ms});
    ASSERT_EQ(result.clientResults.size(), 1U);
    EXPECT_EQ(result.clientResults[0].exitCode, 0) << result.clientResults[0].stderrData;
    EXPECT_EQ(result.clientResults[0].attempts, 2);
    EXPECT_TRUE(result.clientResults[0].errorMessage.empty());
    EXPECT_EQ(result.clientResults[0].stdoutData, "host=u@h\n");
}

TEST_F(GoldenRemoteTest, AnAuthFailureIsNotRetried) {
    auto result = pm_.executeRemote({client("u", "h")}, "denied", context("denied"),
                                    {.timeoutSec = 10, .maxRetries = 3, .retryBaseDelay = 20ms});
    EXPECT_EQ(result.clientResults[0].attempts, 1); // permanent: no retry
    EXPECT_EQ(result.clientResults[0].errorMessage, "ERROR: authentication failed");
}

TEST_F(GoldenRemoteTest, ACommandsOwnNonZeroExitIsNotRetried) {
    auto result = pm_.executeRemote({client("u", "h")}, "fail 7", context("fail"),
                                    {.timeoutSec = 10, .maxRetries = 3, .retryBaseDelay = 20ms});
    EXPECT_EQ(result.clientResults[0].attempts, 1); // a real result, not a transport failure
    EXPECT_EQ(result.clientResults[0].exitCode, 7);
}

// --- Fail-fast: the first final failure aborts the rest (Phase 2) ---

TEST_F(GoldenRemoteTest, FailFastAbortsTheRemainingWorkers) {
    // Window of 1, three hosts that all fail: the first failure aborts the two
    // still-pending workers before they ever spawn.
    std::vector<ClientEntry> clients{client("a", "h1"), client("b", "h2"), client("c", "h3")};
    auto result = pm_.executeRemote(clients, "refused", context("refused"),
                                    {.timeoutSec = 10, .concurrency = 1, .failFast = true});
    ASSERT_EQ(result.clientResults.size(), 3U);
    EXPECT_NE(result.exitCode, 0);
    EXPECT_EQ(result.clientResults[0].exitCode, 255); // the real failure
    EXPECT_FALSE(result.clientResults[0].aborted);
    EXPECT_TRUE(result.clientResults[1].aborted); // never ran: aborted
    EXPECT_TRUE(result.clientResults[2].aborted);
    EXPECT_EQ(result.clientResults[1].errorMessage, "ERROR: aborted (fail-fast)");
}

TEST_F(GoldenRemoteTest, WithoutFailFastEveryWorkerStillRuns) {
    std::vector<ClientEntry> clients{client("a", "h1"), client("b", "h2"), client("c", "h3")};
    auto result = pm_.executeRemote(clients, "refused", context("refused"), {.timeoutSec = 10, .concurrency = 1});
    ASSERT_EQ(result.clientResults.size(), 3U);
    for (const auto& stage : result.clientResults) {
        EXPECT_FALSE(stage.aborted); // all three were attempted
        EXPECT_EQ(stage.exitCode, 255);
        EXPECT_EQ(stage.attempts, 1);
    }
}
