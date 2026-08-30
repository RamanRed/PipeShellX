#include <gtest/gtest.h>

#include "os_test_support.hpp"
#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"
#include "psx/os/process.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <set>
#include <span>
#include <string>
#include <sys/time.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using psx::ErrorClass;
using psx::os::ExitStatus;
using psx::os::Pipe;
using psx::os::Process;
using psx::os::SpawnSpec;
using psx::os::StopSignal;

namespace {

SpawnSpec shell(const std::string& script) {
    SpawnSpec spec;
    spec.program = "/bin/sh";
    spec.argv = {"sh", "-c", script};
    return spec;
}

std::string drain(const psx::os::Handle& reader) {
    std::string out;
    char buffer[4096];
    while (true) {
        auto chunk = psx::os::read(reader, std::span<char>(buffer, sizeof(buffer)));
        if (!chunk.ok() || chunk.value() == 0) {
            break;
        }
        out.append(buffer, chunk.value());
    }
    return out;
}

// True once no process in `group` exists any more (polls for up to 3 s).
bool groupGone(psx::os::ProcessId group) {
    for (int i = 0; i < 300; ++i) {
        if (::kill(-static_cast<pid_t>(group), 0) == -1 && errno == ESRCH) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// No child of ours may remain reapable. Darwin's posix_spawn leaves a
// transient child behind for a few milliseconds after a failed exec (it is
// torn down by the kernel and never becomes a zombie), so poll for ECHILD
// and fail only on a child that is actually reapable.
::testing::AssertionResult noUnreapedChildren() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const pid_t result = ::waitpid(-1, nullptr, WNOHANG);
        const int err = errno;
        if (result == -1 && err == ECHILD) {
            return ::testing::AssertionSuccess();
        }
        if (result > 0) {
            return ::testing::AssertionFailure()
                   << "waitpid(-1, WNOHANG) reaped pid " << result << ": a zombie was left";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ::testing::AssertionFailure() << "a child still exists 500 ms after the operation";
}

} // namespace

TEST(OsProcessTest, ExitCodesAreReported) {
    SpawnSpec spec;
    spec.program = "true";
    spec.argv = {"true"};
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();
    auto status = process.value().wait();
    ASSERT_TRUE(status.ok()) << status.error().message();
    EXPECT_TRUE(status.value().success());
    EXPECT_EQ(status.value().kind, ExitStatus::Kind::Exited);
    EXPECT_EQ(status.value().code, 0);

    auto three = Process::spawn(shell("exit 3"));
    ASSERT_TRUE(three.ok());
    auto threeStatus = three.value().wait();
    ASSERT_TRUE(threeStatus.ok());
    EXPECT_EQ(threeStatus.value().kind, ExitStatus::Kind::Exited);
    EXPECT_EQ(threeStatus.value().code, 3);
    EXPECT_FALSE(threeStatus.value().success());
}

TEST(OsProcessTest, MissingProgramFailsSynchronouslyWithoutLeaks) {
    const auto before = os_test::openDescriptors();
    SpawnSpec spec;
    spec.program = "/definitely/not/here";
    spec.argv = {"nope"};
    auto process = Process::spawn(spec);
    ASSERT_FALSE(process.ok());
    EXPECT_EQ(process.error().cls, ErrorClass::NotFound) << process.error().message();
    EXPECT_EQ(os_test::openDescriptors(), before);
    EXPECT_TRUE(noUnreapedChildren()) << "a failed spawn must not leave a zombie";

    SpawnSpec byName;
    byName.program = "pipeshellx-no-such-binary";
    byName.argv = {"pipeshellx-no-such-binary"};
    auto lookup = Process::spawn(byName);
    ASSERT_FALSE(lookup.ok());
    EXPECT_EQ(lookup.error().cls, ErrorClass::NotFound);
}

TEST(OsProcessTest, StdioIsWiredThroughPipesAndNothingLeaks) {
    const auto before = os_test::openDescriptors();
    auto in = Pipe::create();
    auto out = Pipe::create();
    auto err = Pipe::create();
    ASSERT_TRUE(in.ok() && out.ok() && err.ok());

    SpawnSpec spec = shell("read line; printf 'got:%s' \"$line\"; echo oops >&2");
    spec.in = SpawnSpec::Stdio::from(in.value().reader);
    spec.out = SpawnSpec::Stdio::from(out.value().writer);
    spec.err = SpawnSpec::Stdio::from(err.value().writer);
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();

    // The parent keeps only its own ends.
    in.value().reader.close();
    out.value().writer.close();
    err.value().writer.close();

    ASSERT_TRUE(psx::os::write(in.value().writer, std::span<const char>("ping\n", 5)).ok());
    in.value().writer.close();

    EXPECT_EQ(drain(out.value().reader), "got:ping");
    EXPECT_EQ(drain(err.value().reader), "oops\n");
    auto status = process.value().wait();
    ASSERT_TRUE(status.ok());
    EXPECT_TRUE(status.value().success());

    out.value().reader.close();
    err.value().reader.close();
    EXPECT_EQ(os_test::openDescriptors(), before);
}

TEST(OsProcessTest, NullStdioAndInheritedStdio) {
    auto out = Pipe::create();
    ASSERT_TRUE(out.ok());
    SpawnSpec spec = shell("cat; echo done");
    spec.in = SpawnSpec::Stdio::null();
    spec.out = SpawnSpec::Stdio::from(out.value().writer);
    spec.err = SpawnSpec::Stdio::null();
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();
    out.value().writer.close();
    EXPECT_EQ(drain(out.value().reader), "done\n") << "cat must see EOF on /dev/null immediately";
    ASSERT_TRUE(process.value().wait().ok());

    // Default: inherit the parent's stdio (nothing captured, nothing broken).
    auto inherit = Process::spawn(shell("exit 0"));
    ASSERT_TRUE(inherit.ok());
    EXPECT_TRUE(inherit.value().wait().value().success());
}

TEST(OsProcessTest, ChildInheritsOnlyItsThreeStdioDescriptors) {
    // Hold a pile of handles in the parent; none may be visible in the child.
    std::vector<Pipe> extra;
    for (int i = 0; i < 8; ++i) {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok());
        extra.push_back(std::move(pipe.value()));
    }
    auto out = Pipe::create();
    ASSERT_TRUE(out.ok());

    SpawnSpec spec = shell("echo /dev/fd/*");
    spec.out = SpawnSpec::Stdio::from(out.value().writer);
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();
    out.value().writer.close();
    const std::string listing = drain(out.value().reader);
    ASSERT_TRUE(process.value().wait().ok());

    std::set<int> seen;
    std::size_t pos = 0;
    while ((pos = listing.find("/dev/fd/", pos)) != std::string::npos) {
        pos += 8;
        seen.insert(std::atoi(listing.c_str() + pos));
    }
    ASSERT_TRUE(seen.count(0) && seen.count(1) && seen.count(2)) << listing;
    for (int fd : seen) {
        // 3 may be the shell's own directory handle while it expands the glob.
        EXPECT_LE(fd, 3) << "descriptor " << fd << " leaked into the child: " << listing;
    }
}

TEST(OsProcessTest, ExtraHandlesAreInheritedAtTheRequestedDescriptor) {
    auto secret = Pipe::create();
    auto out = Pipe::create();
    ASSERT_TRUE(secret.ok() && out.ok());
    ASSERT_TRUE(psx::os::write(secret.value().writer, std::span<const char>("hunter2\n", 8)).ok());
    secret.value().writer.close();

    SpawnSpec spec = shell("read -r line <&3; echo \"got $line\"; ls /dev/fd | tr '\\n' ' '");
    spec.out = SpawnSpec::Stdio::from(out.value().writer);
    spec.extraHandles = {{&secret.value().reader, 3}};
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();
    out.value().writer.close();
    secret.value().reader.close();
    const std::string output = drain(out.value().reader);
    ASSERT_TRUE(process.value().wait().ok());
    EXPECT_EQ(output.rfind("got hunter2\n", 0), 0U) << output;

    // A closefrom-style spawn must not close the extra handle it just placed
    // (regression: the glibc closefrom floor sitting below fd 3).
    auto secret2 = Pipe::create();
    auto out2 = Pipe::create();
    ASSERT_TRUE(secret2.ok() && out2.ok());
    ASSERT_TRUE(psx::os::write(secret2.value().writer, std::span<const char>("kept\n", 5)).ok());
    secret2.value().writer.close();
    SpawnSpec keep = shell("cat <&3");
    keep.out = SpawnSpec::Stdio::from(out2.value().writer);
    keep.extraHandles = {{&secret2.value().reader, 3}};
    auto keeper = Process::spawn(keep);
    ASSERT_TRUE(keeper.ok()) << keeper.error().message();
    out2.value().writer.close();
    secret2.value().reader.close();
    EXPECT_EQ(drain(out2.value().reader), "kept\n") << "fd 3 was closed before exec";
    ASSERT_TRUE(keeper.value().wait().ok());

    SpawnSpec bad = shell("true");
    bad.extraHandles = {{&secret.value().reader, 1}}; // stdio slots are reserved
    EXPECT_EQ(Process::spawn(bad).error().cls, ErrorClass::InvalidArgument);
    SpawnSpec closed = shell("true");
    closed.extraHandles = {{&secret.value().reader, 4}}; // already closed above
    EXPECT_EQ(Process::spawn(closed).error().cls, ErrorClass::InvalidArgument);
}

TEST(OsProcessTest, KillStopsTheWholeProcessGroup) {
    auto process = Process::spawn(shell("sleep 30 & sleep 30"));
    ASSERT_TRUE(process.ok());
    const auto group = process.value().groupId();
    EXPECT_EQ(group, process.value().id());

    ASSERT_TRUE(process.value().signal(StopSignal::Kill).ok());
    auto status = process.value().wait();
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(status.value().kind, ExitStatus::Kind::Signaled);
    EXPECT_EQ(status.value().code, SIGKILL);
    EXPECT_TRUE(groupGone(group)) << "the backgrounded sleep must die with the group";
}

TEST(OsProcessTest, GracefulSignalLetsTheChildExitCleanly) {
    auto process = Process::spawn(shell("trap 'exit 7' TERM; while :; do sleep 0.1; done"));
    ASSERT_TRUE(process.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let the trap install
    ASSERT_TRUE(process.value().signal(StopSignal::Graceful).ok());
    auto status = process.value().wait();
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(status.value().kind, ExitStatus::Kind::Exited);
    EXPECT_EQ(status.value().code, 7);
}

TEST(OsProcessTest, StopIsGracefulFirstAndHardAfterTheGracePeriod) {
    auto polite = Process::spawn(shell("trap 'exit 7' TERM; while :; do sleep 0.1; done"));
    ASSERT_TRUE(polite.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let the trap install
    auto politeStatus = polite.value().stop(std::chrono::seconds(3));
    ASSERT_TRUE(politeStatus.ok()) << politeStatus.error().message();
    EXPECT_EQ(politeStatus.value().kind, ExitStatus::Kind::Exited);
    EXPECT_EQ(politeStatus.value().code, 7);

    auto stubborn = Process::spawn(shell("trap '' TERM; while :; do sleep 0.1; done"));
    ASSERT_TRUE(stubborn.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto start = std::chrono::steady_clock::now();
    auto stubbornStatus = stubborn.value().stop(std::chrono::milliseconds(300));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(stubbornStatus.ok());
    EXPECT_EQ(stubbornStatus.value().kind, ExitStatus::Kind::Signaled);
    EXPECT_EQ(stubbornStatus.value().code, SIGKILL);
    EXPECT_GE(elapsed, std::chrono::milliseconds(290));
    EXPECT_LT(elapsed, std::chrono::seconds(3));
    EXPECT_FALSE(stubborn.value().running());
    EXPECT_EQ(stubborn.value().stop(std::chrono::milliseconds(1)).value().code, SIGKILL) << "cached after reaping";
}

TEST(OsProcessTest, TryWaitDoesNotBlockAndWaitIsIdempotentAfterReaping) {
    auto process = Process::spawn(shell("sleep 0.3; exit 5"));
    ASSERT_TRUE(process.ok());
    auto early = process.value().tryWait();
    ASSERT_TRUE(early.ok());
    EXPECT_FALSE(early.value().has_value());
    EXPECT_TRUE(process.value().running());

    auto status = process.value().wait();
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(status.value().code, 5);
    EXPECT_FALSE(process.value().running());

    auto again = process.value().wait();
    ASSERT_TRUE(again.ok()) << "a reaped status is cached, not re-waited";
    EXPECT_EQ(again.value().code, 5);
    auto tryAgain = process.value().tryWait();
    ASSERT_TRUE(tryAgain.ok());
    ASSERT_TRUE(tryAgain.value().has_value());
    EXPECT_EQ(tryAgain.value()->code, 5);
}

// docs/testing.md regression: reaping one child must never touch another.
TEST(OsProcessTest, ReapingOneChildNeverStealsAnother) {
    auto slow = Process::spawn(shell("sleep 0.3; exit 11"));
    auto fast = Process::spawn(shell("exit 0"));
    ASSERT_TRUE(slow.ok() && fast.ok());
    ASSERT_TRUE(fast.value().wait().ok());
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(fast.value().tryWait().ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto status = slow.value().wait();
    ASSERT_TRUE(status.ok()) << status.error().message();
    EXPECT_EQ(status.value().code, 11);
}

TEST(OsProcessTest, DestructorKillsAndReapsARunningChild) {
    psx::os::ProcessId id = 0;
    const auto start = std::chrono::steady_clock::now();
    {
        auto process = Process::spawn(shell("sleep 30"));
        ASSERT_TRUE(process.ok());
        id = process.value().id();
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    EXPECT_EQ(::waitpid(static_cast<pid_t>(id), nullptr, WNOHANG), -1) << "already reaped";
    EXPECT_EQ(errno, ECHILD);
}

TEST(OsProcessTest, MoveAssignKillsTheOverwrittenChildAndTakesOverTheOther) {
    auto victim = Process::spawn(shell("sleep 30"));
    auto survivor = Process::spawn(shell("exit 7"));
    ASSERT_TRUE(victim.ok() && survivor.ok());
    const auto victimGroup = victim.value().groupId();

    // Overwrite `victim` with `survivor`: the sleeping child must be killed and
    // reaped, and `victim` now owns the exit-7 process.
    victim.value() = std::move(survivor.value());
    EXPECT_TRUE(groupGone(victimGroup)) << "the overwritten child must be killed";
    EXPECT_FALSE(survivor.value().running());

    auto status = victim.value().wait();
    ASSERT_TRUE(status.ok()) << status.error().message();
    EXPECT_EQ(status.value().kind, ExitStatus::Kind::Exited);
    EXPECT_EQ(status.value().code, 7);
    EXPECT_TRUE(noUnreapedChildren());
}

TEST(OsProcessTest, ReleaseDetachesOwnership) {
    auto process = Process::spawn(shell("exit 0"));
    ASSERT_TRUE(process.ok());
    const auto id = process.value().release();
    EXPECT_FALSE(process.value().running());
    int status = 0;
    ASSERT_EQ(::waitpid(static_cast<pid_t>(id), &status, 0), static_cast<pid_t>(id));
    EXPECT_TRUE(WIFEXITED(status));
}

TEST(OsProcessTest, CpuLimitTerminatesARunawayChild) {
    SpawnSpec spec = shell("while :; do :; done");
    spec.limits.cpuSeconds = 1;
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();
    const auto start = std::chrono::steady_clock::now();
    auto status = process.value().wait();
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(status.value().kind, ExitStatus::Kind::Signaled);
    EXPECT_TRUE(status.value().code == SIGXCPU || status.value().code == SIGKILL) << status.value().code;
    // One CPU-second may take several wall-clock seconds on a loaded host.
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(15));
}

TEST(OsProcessTest, AddressSpaceLimitIsEnforcedWhereTheKernelSupportsIt) {
#if defined(__APPLE__)
    GTEST_SKIP() << "Darwin does not reliably enforce RLIMIT_AS";
#else
    SpawnSpec spec = shell("dd if=/dev/zero of=/dev/null bs=200M count=1 2>/dev/null");
    spec.limits.addressSpaceBytes = 64ULL * 1024 * 1024;
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();
    auto status = process.value().wait();
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(status.value().success());
#endif
}

TEST(OsProcessTest, WorkingDirectoryIsApplied) {
    auto out = Pipe::create();
    ASSERT_TRUE(out.ok());
    SpawnSpec spec = shell("pwd");
    spec.cwd = "/";
    spec.out = SpawnSpec::Stdio::from(out.value().writer);
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();
    out.value().writer.close();
    EXPECT_EQ(drain(out.value().reader), "/\n");
    ASSERT_TRUE(process.value().wait().ok());

    SpawnSpec missing = shell("pwd");
    missing.cwd = "/no/such/dir";
    auto failed = Process::spawn(missing);
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.error().cls, ErrorClass::NotFound);
    EXPECT_TRUE(noUnreapedChildren());
}

TEST(OsProcessTest, EnvironmentCanBeReplaced) {
    auto out = Pipe::create();
    ASSERT_TRUE(out.ok());
    SpawnSpec spec = shell("echo \"$PSX_MARK|$HOME\"");
    spec.env = std::vector<std::string>{"PSX_MARK=42", "PATH=/usr/bin:/bin"};
    spec.out = SpawnSpec::Stdio::from(out.value().writer);
    auto process = Process::spawn(spec);
    ASSERT_TRUE(process.ok()) << process.error().message();
    out.value().writer.close();
    EXPECT_EQ(drain(out.value().reader), "42|\n");
    ASSERT_TRUE(process.value().wait().ok());
}

// EINTR injection: a 1 ms interval timer without SA_RESTART interrupts the
// blocking wait() repeatedly; it must still deliver the exit status.
TEST(OsProcessTest, BlockingWaitSurvivesSignalInterruptions) {
    struct sigaction action{};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // deliberately no SA_RESTART
    struct sigaction previous{};
    ASSERT_EQ(::sigaction(SIGALRM, &action, &previous), 0);
    itimerval interval{};
    interval.it_interval.tv_usec = 1000;
    interval.it_value.tv_usec = 1000;
    ASSERT_EQ(::setitimer(ITIMER_REAL, &interval, nullptr), 0);

    auto process = Process::spawn(shell("sleep 0.3; exit 9"));
    ASSERT_TRUE(process.ok());
    auto status = process.value().wait();

    itimerval off{};
    ::setitimer(ITIMER_REAL, &off, nullptr);
    ::sigaction(SIGALRM, &previous, nullptr);

    ASSERT_TRUE(status.ok()) << status.error().message();
    EXPECT_EQ(status.value().code, 9);
}

// T11: descriptors and children are balanced after a spawn soak.
TEST(OsProcessTest, SpawnSoakLeaksNeitherDescriptorsNorZombies) {
    const int cycles = std::getenv("PIPESHELLX_SOAK") != nullptr ? 10000 : 500;
    const auto before = os_test::openDescriptors();
    const auto stats = psx::os::handleStats();
    SpawnSpec spec;
    spec.program = "true";
    spec.argv = {"true"};
    for (int i = 0; i < cycles; ++i) {
        auto out = Pipe::create();
        ASSERT_TRUE(out.ok());
        spec.out = SpawnSpec::Stdio::from(out.value().writer);
        auto process = Process::spawn(spec);
        ASSERT_TRUE(process.ok()) << "cycle " << i << ": " << process.error().message();
        out.value().writer.close();
        drain(out.value().reader);
        ASSERT_TRUE(process.value().wait().ok());
    }
    EXPECT_EQ(os_test::openDescriptors(), before);
    EXPECT_EQ(psx::os::handleStats().open, stats.open);
    EXPECT_TRUE(noUnreapedChildren());
}
