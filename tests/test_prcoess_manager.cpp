#include <gtest/gtest.h>
#include "process_manager.hpp"

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