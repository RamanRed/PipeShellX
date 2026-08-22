#include <gtest/gtest.h>

#include "psx/os/console.hpp"
#include "psx/os/paths.hpp"
#include "psx/os/system.hpp"
#include "test_support.hpp"

#include <sys/resource.h>
#include <unistd.h>

TEST(OsSystemTest, CurrentProcessIdMatchesTheKernel) {
    EXPECT_EQ(psx::os::currentProcessId(), static_cast<psx::os::ProcessId>(::getpid()));
}

TEST(OsSystemTest, RaiseHandleLimitNeverLowersAndIsIdempotent) {
    rlimit before{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &before), 0);

    auto first = psx::os::raiseHandleLimit();
    ASSERT_TRUE(first.ok()) << first.error().message();
    EXPECT_GE(first.value().soft, static_cast<std::uint64_t>(before.rlim_cur));
    EXPECT_LE(first.value().soft, first.value().hard);

    rlimit after{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &after), 0);
    EXPECT_EQ(static_cast<std::uint64_t>(after.rlim_cur), first.value().soft);

    auto second = psx::os::raiseHandleLimit();
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().soft, first.value().soft);
}

TEST(OsSystemTest, IsExecutableFileDistinguishesProgramsFromOtherFiles) {
    EXPECT_TRUE(psx::os::isExecutableFile("/bin/sh"));
    EXPECT_FALSE(psx::os::isExecutableFile("/etc/hosts"));
    EXPECT_FALSE(psx::os::isExecutableFile("/definitely/not/here"));
    EXPECT_FALSE(psx::os::isExecutableFile("/bin")) << "a directory is not an executable file";
    EXPECT_FALSE(psx::os::isExecutableFile(""));
}

TEST(OsConsoleTest, InteractivityMatchesIsatty) {
    EXPECT_EQ(psx::os::isInteractive(psx::os::StandardStream::Input), ::isatty(STDIN_FILENO) == 1);
    EXPECT_EQ(psx::os::isInteractive(psx::os::StandardStream::Output), ::isatty(STDOUT_FILENO) == 1);
    EXPECT_EQ(psx::os::isInteractive(psx::os::StandardStream::Error), ::isatty(STDERR_FILENO) == 1);
}

TEST(OsConsoleTest, ReadSecretRefusesWithoutATerminal) {
    if (::isatty(STDIN_FILENO) == 1) {
        GTEST_SKIP() << "stdin is a terminal; the non-interactive path is what this test pins";
    }
    auto secret = psx::os::readSecret("Password: ");
    ASSERT_FALSE(secret.ok());
    EXPECT_EQ(secret.error().cls, psx::ErrorClass::Unsupported);
}

TEST(OsPathsTest, StateDirectoryFollowsXdgThenHome) {
    {
        test_support::ScopedEnv xdg("XDG_STATE_HOME", "/var/tmp/xdg-state");
        test_support::ScopedEnv home("HOME", "/home/tester");
        EXPECT_EQ(psx::os::stateDirectory("pipeshellx"), "/var/tmp/xdg-state/pipeshellx");
    }
    {
        test_support::ScopedEnv xdg("XDG_STATE_HOME", "");
        test_support::ScopedEnv home("HOME", "/home/tester");
        EXPECT_EQ(psx::os::stateDirectory("pipeshellx"), "/home/tester/.local/state/pipeshellx");
    }
    {
        test_support::ScopedEnv xdg("XDG_STATE_HOME", std::nullopt);
        test_support::ScopedEnv home("HOME", std::nullopt);
        // No $HOME: the account database still knows the home directory.
        EXPECT_EQ(psx::os::stateDirectory("pipeshellx"), psx::os::homeDirectory() + "/.local/state/pipeshellx");
    }
}

TEST(OsPathsTest, HomeDirectoryComesFromTheEnvironmentFirst) {
    test_support::ScopedEnv home("HOME", "/home/tester");
    EXPECT_EQ(psx::os::homeDirectory(), "/home/tester");
    test_support::ScopedEnv none("HOME", std::nullopt);
    // Falls back to the account database; never empty for a real user.
    EXPECT_FALSE(psx::os::homeDirectory().empty());
}
