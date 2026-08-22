#include "process_manager.hpp"
#include "test_support.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <unistd.h>

class ProcessManagerTest : public ::testing::Test {};

TEST_F(ProcessManagerTest, ExecuteValidCommand) {
    ProcessManager pm;
    std::vector<std::string> args = {"echo", "HelloWorld"};
    LogContext context{getpid(), "test", "-", "echo HelloWorld"};
    auto result = pm.execute(args, context);
    ASSERT_EQ(result.exitCode, 0);
    ASSERT_NE(result.stdoutData.find("HelloWorld"), std::string::npos);
    ASSERT_TRUE(result.stderrData.empty());
}

TEST_F(ProcessManagerTest, ExecuteInvalidCommand) {
    ProcessManager pm;
    std::vector<std::string> args = {"nonexistent_command"};
    LogContext context{getpid(), "test", "-", "nonexistent_command"};
    auto result = pm.execute(args, context);
    ASSERT_NE(result.exitCode, 0);
    ASSERT_TRUE(result.stdoutData.empty());
    ASSERT_FALSE(result.stderrData.empty());
}

TEST_F(ProcessManagerTest, CommandTimeout) {
    ProcessManager pm;
    std::vector<std::string> args = {"sleep", "10"};
    LogContext context{getpid(), "test", "-", "sleep 10"};
    auto result = pm.execute(args, context, "", 1); // 1 second timeout
    ASSERT_TRUE(result.timedOut);
}
// Regression: a worker whose pipes reach EOF before the child is reaped used
// to be reported as "timed out" after the 50 ms idle poll, regardless of the
// configured deadline.
TEST_F(ProcessManagerTest, FastRemoteFailureIsNotReportedAsTimeout) {
    ProcessManager pm;
    const ClientEntry client = test_support::refusedLoopbackClient();
    LogContext context{getpid(), "test", client.clientId(), "ssh true"};

    const auto start = std::chrono::steady_clock::now();
    const auto result = pm.executeRemote({client}, "true", context, 10);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_EQ(result.clientResults.size(), 1U);
    const auto& clientResult = result.clientResults.front();
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(clientResult.timedOut);
    EXPECT_NE(clientResult.exitCode, 0);
    EXPECT_EQ(clientResult.errorMessage, "ERROR: connection failed") << clientResult.stderrData;
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST_F(ProcessManagerTest, RemoteTimeoutStillFiresForHungWorker) {
    // The listener completes the TCP handshake but never sends an SSH banner,
    // so ssh waits (up to ConnectTimeout=5) — longer than the 1 s deadline.
    // No external network or black-hole address is involved.
    test_support::SilentListener listener;
    ProcessManager pm;
    ClientEntry client;
    client.user = "nobody";
    client.host = "127.0.0.1";
    client.port = listener.port();
    LogContext context{getpid(), "test", client.clientId(), "ssh true"};

    const auto start = std::chrono::steady_clock::now();
    const auto result = pm.executeRemote({client}, "true", context, 1);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_EQ(result.clientResults.size(), 1U);
    EXPECT_TRUE(result.timedOut);
    EXPECT_TRUE(result.clientResults.front().timedOut);
    EXPECT_EQ(result.clientResults.front().errorMessage, "ERROR: command timed out");
    EXPECT_GE(elapsed, std::chrono::milliseconds(900));
    EXPECT_LT(elapsed, std::chrono::seconds(4));
}

// Regression: after the child was reaped, the old timeout branch ran
// kill(-(-1)) == kill(1, SIGKILL) and then spun until the grandchild holding
// the pipes exited on its own. The whole process group must be killed and the
// call must return promptly.
TEST_F(ProcessManagerTest, TimeoutKillsGrandchildrenHoldingThePipes) {
    ProcessManager pm;
    std::vector<std::string> args = {"/bin/sh", "-c", "sleep 30 & exit 0"};
    LogContext context{getpid(), "test", "-", "sh -c 'sleep 30 & exit 0'"};

    const auto start = std::chrono::steady_clock::now();
    auto result = pm.execute(args, context, "", 1);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(result.timedOut);
    EXPECT_GE(elapsed, std::chrono::milliseconds(900));
    // SIGKILL to the group closes the pipes at once; the 2 s drain grace is not needed.
    EXPECT_LT(elapsed, std::chrono::milliseconds(2500));
}

TEST_F(ProcessManagerTest, TimeoutAbandonsPipesHeldOutsideTheProcessGroup) {
    if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "python3 is required to create a detached pipe holder";
    }
    ProcessManager pm;
    // The holder leaves the process group (setsid) so SIGKILL cannot reach it;
    // the drain grace period must bound the wait instead.
    std::vector<std::string> args = {"/bin/sh", "-c",
                                     "python3 -c 'import os,time; os.setsid(); time.sleep(8)' & exit 0"};
    LogContext context{getpid(), "test", "-", "sh -c 'detached holder'"};

    const auto start = std::chrono::steady_clock::now();
    auto result = pm.execute(args, context, "", 1);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(result.timedOut);
    EXPECT_GE(elapsed, std::chrono::milliseconds(2900)); // 1 s deadline + 2 s grace
    EXPECT_LT(elapsed, std::chrono::seconds(6));
}
