#include <gtest/gtest.h>

#include "command_executor.hpp"
#include "test_support.hpp"

#include <stdexcept>
#include <string>

namespace {

class CommandExecutorTest : public ::testing::Test {
protected:
    test_support::ScopedTempCwd cwd_{"executor"}; // a fresh fixture per test, so plain members suffice
    CommandExecutor executor_;
};

void expectRejected(CommandExecutor& executor, const std::string& command, const std::string& reasonFragment) {
    try {
        static_cast<void>(executor.execute(command));
        FAIL() << "expected '" << command << "' to be rejected";
    } catch (const std::runtime_error& ex) {
        EXPECT_NE(std::string(ex.what()).find(reasonFragment), std::string::npos)
            << "command '" << command << "' rejected for the wrong reason: " << ex.what();
    }
}

} // namespace

TEST_F(CommandExecutorTest, TopIsNoLongerAllowlisted) {
    expectRejected(executor_, "top", "not in the allowlist");
}

TEST_F(CommandExecutorTest, HostnameIsAllowlistedAndRunsLocally) {
    const auto result = executor_.execute("hostname");
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_FALSE(result.stdoutData.empty());
    EXPECT_FALSE(result.timedOut);
    EXPECT_TRUE(result.clientResults.empty());
}

TEST_F(CommandExecutorTest, RejectsUnknownCommandsAndExplicitPaths) {
    expectRejected(executor_, "rm -rf /", "not in the allowlist");
    expectRejected(executor_, "/bin/ls", "Explicit paths");
    expectRejected(executor_, "./ls", "Explicit paths");
}

TEST_F(CommandExecutorTest, RejectsMalformedInput) {
    expectRejected(executor_, "", "length");
    expectRejected(executor_, std::string(1025, 'a'), "length");
    expectRejected(executor_, "echo \"unterminated", "Unterminated");
    expectRejected(executor_, std::string("echo \x01"), "non-printable");
    expectRejected(executor_, "echo a; id", "unsupported characters");
    expectRejected(executor_, "echo $(id)", "unsupported characters");
    expectRejected(executor_, "ls | cat", "unsupported characters");
    expectRejected(executor_, "cat < /etc/passwd", "unsupported characters");
    expectRejected(executor_, "echo a > b", "unsupported characters");
    expectRejected(executor_, "echo `id`", "unsupported characters");
    expectRejected(executor_, "echo a & b", "unsupported characters");
    expectRejected(executor_, "echo back\\slash", "unsupported characters");
    expectRejected(executor_, "echo " + std::string(257, 'x'), "unsupported characters");
}

TEST_F(CommandExecutorTest, QuotedArgumentsArePreservedVerbatim) {
    std::string captured;
    const auto result = executor_.execute("echo \"hello   world\"", "-", [&](const std::string& line, bool isStdout) {
        if (isStdout) {
            captured += line;
        }
    });
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(captured, "hello   world");
    EXPECT_EQ(result.stdoutData, "hello   world\n");
}

TEST_F(CommandExecutorTest, NonZeroExitCodesPropagate) {
    const auto result = executor_.execute("ls /definitely/not/a/real/directory");
    EXPECT_NE(result.exitCode, 0);
    EXPECT_FALSE(result.stderrData.empty());
    EXPECT_FALSE(result.timedOut);
}

TEST_F(CommandExecutorTest, StrayClientsFileInCwdIsNotUsedWhenAbsent) {
    // The fixture guarantees an empty CWD; local execution must be the path taken.
    const auto result = executor_.execute("echo local");
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.stdoutData, "local\n");
    EXPECT_TRUE(result.clientResults.empty());
}
