#include <gtest/gtest.h>

#include "psx/cli/run_command.hpp"

#include "psx/stream/bounded_buffer.hpp"

#include <stdexcept>
#include <vector>

using psx::cli::parseRun;
using psx::cli::SelectorKind;
using psx::cli::SinkMode;

TEST(ParseRunTest, DefaultsToAllHostsGroupSinkAndTheCommandAfterDashDash) {
    const auto inv = parseRun({"--", "uptime"});
    EXPECT_TRUE(inv.inventoryPath.empty());
    EXPECT_EQ(inv.selector.kind, SelectorKind::All);
    EXPECT_EQ(inv.sink, SinkMode::Group);
    EXPECT_EQ(inv.command, (std::vector<std::string>{"uptime"}));
    EXPECT_EQ(inv.timeoutSec, 0);
}

TEST(ParseRunTest, CommandKeepsItsOwnFlagsAfterDashDash) {
    const auto inv = parseRun({"-g", "web", "--stream", "--", "tail", "-F", "/var/log/x"});
    EXPECT_EQ(inv.selector.kind, SelectorKind::Group);
    EXPECT_EQ(inv.selector.value, "web");
    EXPECT_EQ(inv.sink, SinkMode::Stream);
    EXPECT_EQ(inv.command, (std::vector<std::string>{"tail", "-F", "/var/log/x"}));
}

TEST(ParseRunTest, SelectorsAreMutuallyExclusive) {
    EXPECT_EQ(parseRun({"-t", "canary", "--", "id"}).selector.kind, SelectorKind::Tag);
    EXPECT_EQ(parseRun({"-t", "canary", "--", "id"}).selector.value, "canary");
    const auto byHost = parseRun({"-H", "h1,h2,h3", "--", "id"});
    EXPECT_EQ(byHost.selector.kind, SelectorKind::Hosts);
    EXPECT_EQ(byHost.selector.hosts, (std::vector<std::string>{"h1", "h2", "h3"}));

    EXPECT_THROW(static_cast<void>(parseRun({"-g", "a", "-t", "b", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parseRun({"-g", "a", "-H", "h", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, SinkModesAndInventoryAndTimeout) {
    EXPECT_EQ(parseRun({"--json", "--", "id"}).sink, SinkMode::Json);
    EXPECT_EQ(parseRun({"--group", "--", "id"}).sink, SinkMode::Group);
    const auto inv = parseRun({"-i", "/etc/fleet.ini", "--timeout", "30", "--", "id"});
    EXPECT_EQ(inv.inventoryPath, "/etc/fleet.ini");
    EXPECT_EQ(inv.timeoutSec, 30);
    EXPECT_THROW(static_cast<void>(parseRun({"--stream", "--json", "--", "id"})), std::runtime_error); // one sink only
}

TEST(ParseRunTest, PolicyAndRingFlags) {
    using psx::stream::OverflowPolicy;
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).policy, OverflowPolicy::Block);
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).ringBytes, 0U);
    EXPECT_EQ(psx::cli::parseRun({"--overflow", "drop-oldest", "--", "id"}).policy, OverflowPolicy::DropOldest);
    EXPECT_EQ(psx::cli::parseRun({"--overflow", "drop-newest", "--", "id"}).policy, OverflowPolicy::DropNewest);
    EXPECT_EQ(psx::cli::parseRun({"--ring", "1MiB", "--", "id"}).ringBytes, 1024U * 1024);
    EXPECT_EQ(psx::cli::parseRun({"--ring", "256KiB", "--", "id"}).ringBytes, 256U * 1024);
    EXPECT_EQ(psx::cli::parseRun({"--ring", "2M", "--", "id"}).ringBytes, 2U * 1024 * 1024);
    EXPECT_EQ(psx::cli::parseRun({"--ring", "4096", "--", "id"}).ringBytes, 4096U);
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--overflow", "nope", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--ring", "1XB", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--ring", "big", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, ConcurrencyFlag) {
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).concurrency, 64);
    EXPECT_EQ(psx::cli::parseRun({"-c", "8", "--", "id"}).concurrency, 8);
    EXPECT_EQ(psx::cli::parseRun({"--concurrency", "256", "--", "id"}).concurrency, 256);
    EXPECT_EQ(psx::cli::parseRun({"-c", "0", "--", "id"}).concurrency, 0); // 0 = all at once
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"-c", "x", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, PolicyFileFlag) {
    EXPECT_TRUE(psx::cli::parseRun({"--", "id"}).policyPath.empty());
    EXPECT_EQ(psx::cli::parseRun({"--policy", "/etc/psx.policy", "--", "id"}).policyPath, "/etc/psx.policy");
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"--policy", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, TransportNativeAndItsFlags) {
    EXPECT_FALSE(psx::cli::parseRun({"--", "id"}).native);
    EXPECT_FALSE(psx::cli::parseRun({"--transport", "ssh", "--", "id"}).native);
    const auto n = psx::cli::parseRun(
        {"--transport", "native", "--cert", "c", "--key", "k", "--ca", "a", "--native-port", "9000", "--", "id"});
    EXPECT_TRUE(n.native);
    EXPECT_EQ(n.certPath, "c");
    EXPECT_EQ(n.keyPath, "k");
    EXPECT_EQ(n.caPath, "a");
    EXPECT_EQ(n.nativePort, 9000);
    EXPECT_THROW(psx::cli::parseRun({"--transport", "bogus", "--", "id"}), psx::cli::CliError);
}

TEST(ParseRunTest, ShellSelectsTheRemoteQuoting) {
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).shell, RemoteShell::Posix); // default
    EXPECT_EQ(psx::cli::parseRun({"--shell", "cmd", "--", "id"}).shell, RemoteShell::Cmd);
    EXPECT_EQ(psx::cli::parseRun({"--shell", "powershell", "--", "id"}).shell, RemoteShell::PowerShell);
    EXPECT_EQ(psx::cli::parseRun({"--shell", "pwsh", "--", "id"}).shell, RemoteShell::PowerShell);
    EXPECT_THROW(psx::cli::parseRun({"--shell", "bash", "--", "id"}), psx::cli::CliError);
}

TEST(ParseRunTest, OverflowSpoolIsAccepted) {
    EXPECT_EQ(psx::cli::parseRun({"--overflow", "spool", "--", "id"}).policy, psx::stream::OverflowPolicy::Spool);
    EXPECT_THROW(psx::cli::parseRun({"--overflow", "bogus", "--", "id"}), psx::cli::CliError);
}

TEST(ParseRunTest, ReuseFlag) {
    EXPECT_FALSE(psx::cli::parseRun({"--", "id"}).reuse);
    EXPECT_TRUE(psx::cli::parseRun({"--reuse", "--", "id"}).reuse);
}

TEST(ParseRunTest, NoColorIsHonoured) {
    EXPECT_FALSE(parseRun({"--no-color", "--stream", "--", "id"}).colour);
    EXPECT_TRUE(parseRun({"--stream", "--", "id"}).colour);
}

TEST(ParseRunTest, RejectsMissingCommandOrValues) {
    EXPECT_THROW(static_cast<void>(parseRun({"-g", "web"})), std::runtime_error);      // no --
    EXPECT_THROW(static_cast<void>(parseRun({"--"})), std::runtime_error);             // empty command
    EXPECT_THROW(static_cast<void>(parseRun({"-g", "--", "id"})), std::runtime_error); // -g needs a value
    EXPECT_THROW(static_cast<void>(parseRun({"--timeout", "x", "--", "id"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parseRun({"--bogus", "--", "id"})), std::runtime_error);
}

TEST(ParseRunTest, LongFormSelectorsAlsoWork) {
    EXPECT_EQ(parseRun({"--group-name", "web", "--", "id"}).selector.value, "web");
    EXPECT_EQ(parseRun({"--tag", "canary", "--", "id"}).selector.kind, SelectorKind::Tag);
    EXPECT_EQ(parseRun({"--hosts", "a,b", "--", "id"}).selector.hosts, (std::vector<std::string>{"a", "b"}));
}

// ---- end-to-end: runSubcommand over a fake ssh + an inventory file ----

#include "psx/cli/hosts_command.hpp"
#include "test_support.hpp"

#include <sstream>

TEST(RunSubcommandTest, RunsTheCommandOnSelectedHostsAndRendersGroup) {
    test_support::ScopedTempCwd cwd("run-e2e");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nh1 user=alice\nh2 user=bob\n[db]\nh3 user=carol\n";
    }

    psx::cli::RunInvocation inv;
    inv.inventoryPath = "fleet.ini";
    inv.selector.kind = psx::cli::SelectorKind::Group;
    inv.selector.value = "web";
    inv.sink = psx::cli::SinkMode::Group;
    inv.command = {"ok"}; // the fake ssh echoes host=<target> for the `ok` command

    std::ostringstream out;
    std::ostringstream err;
    const int code = psx::cli::runSubcommand(inv, out, err, false);
    EXPECT_EQ(code, 0) << err.str();
    // Only the two web hosts ran, grouped.
    EXPECT_NE(out.str().find("CLIENT alice@h1"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("CLIENT bob@h2"), std::string::npos) << out.str();
    EXPECT_EQ(out.str().find("carol"), std::string::npos) << "db host must not run";
}

TEST(RunSubcommandTest, NoHostsSelectedIsExitCode3) {
    test_support::ScopedTempCwd cwd("run-none");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nh1\n";
    }
    psx::cli::RunInvocation inv;
    inv.inventoryPath = "fleet.ini";
    inv.selector.kind = psx::cli::SelectorKind::Group;
    inv.selector.value = "nonexistent";
    inv.command = {"ok"};
    std::ostringstream out, err;
    EXPECT_EQ(psx::cli::runSubcommand(inv, out, err, false), 3);
}

TEST(RunSubcommandTest, MissingInventoryIsExitCode2) {
    test_support::ScopedTempCwd cwd("run-noinv");
    psx::cli::RunInvocation inv;
    inv.command = {"id"};
    std::ostringstream out, err;
    EXPECT_EQ(psx::cli::runSubcommand(inv, out, err, false), 2);
    EXPECT_NE(err.str().find("no inventory"), std::string::npos) << err.str();
}

TEST(HostsSubcommandTest, ListsHostsWithGroupsAndTags) {
    test_support::ScopedTempCwd cwd("hosts-e2e");
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nh1 tag=canary\nh2\n[db]\nh1\n";
    }
    std::ostringstream out, err;
    const int code = psx::cli::hostsSubcommand("fleet.ini", out, err);
    EXPECT_EQ(code, 0) << err.str();
    // h1 is in both groups and tagged canary; h2 only web.
    EXPECT_NE(out.str().find("h1"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("web"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("db"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("canary"), std::string::npos) << out.str();
}

TEST(RunSubcommandTest, PolicyFileRestrictsTheCommand) {
    test_support::ScopedTempCwd cwd("run-policy");
    test_support::FakeSshOnPath fakeSsh;
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nu@h1\n";
    }
    {
        std::ofstream pol("policy.txt");
        pol << "allow echo\n";
    }
    // An allowed command runs.
    psx::cli::RunInvocation ok;
    ok.inventoryPath = "fleet.ini";
    ok.policyPath = "policy.txt";
    ok.command = {"echo", "hi"};
    std::ostringstream out1, err1;
    EXPECT_EQ(psx::cli::runSubcommand(ok, out1, err1, false), 0) << err1.str();

    // A disallowed command is rejected before anything runs (exit 2).
    psx::cli::RunInvocation bad = ok;
    bad.command = {"rm", "-rf", "/"};
    std::ostringstream out2, err2;
    EXPECT_EQ(psx::cli::runSubcommand(bad, out2, err2, false), 2);
    EXPECT_NE(err2.str().find("not allowed"), std::string::npos) << err2.str();
    EXPECT_TRUE(out2.str().empty()) << "nothing runs when the policy rejects";
}

TEST(RunSubcommandTest, MissingPolicyFileIsExitCode2) {
    test_support::ScopedTempCwd cwd("run-nopolicy");
    psx::cli::RunInvocation inv;
    inv.policyPath = "does-not-exist.txt";
    inv.command = {"id"};
    std::ostringstream out, err;
    EXPECT_EQ(psx::cli::runSubcommand(inv, out, err, false), 2);
    EXPECT_NE(err.str().find("cannot open"), std::string::npos) << err.str();
}

TEST(ParseRunTest, CanaryFlagCarriesTheSpecVerbatim) {
    EXPECT_TRUE(parseRun({"--", "id"}).canary.empty());
    EXPECT_EQ(parseRun({"--canary", "2", "--", "id"}).canary, "2");
    EXPECT_EQ(parseRun({"--canary", "10%", "--", "id"}).canary, "10%");
}

TEST(CanaryCountTest, CountsAndPercentagesClampIntoRange) {
    using psx::cli::canaryCount;
    // An explicit count, clamped to [1, total].
    EXPECT_EQ(canaryCount("3", 10), 3U);
    EXPECT_EQ(canaryCount("0", 10), 1U);    // never zero canaries when hosts exist
    EXPECT_EQ(canaryCount("200", 10), 10U); // never more than the whole fleet
    // Percentages round up so a small percent still exercises at least one host.
    EXPECT_EQ(canaryCount("50%", 10), 5U);
    EXPECT_EQ(canaryCount("5%", 10), 1U); // ceil(0.5) -> 1
    EXPECT_EQ(canaryCount("100%", 7), 7U);
    EXPECT_EQ(canaryCount("25%", 8), 2U);
    // A malformed spec falls back to a single canary host.
    EXPECT_EQ(canaryCount("abc", 10), 1U);
    // No hosts -> no canaries, regardless of spec.
    EXPECT_EQ(canaryCount("50%", 0), 0U);
}
