#include <gtest/gtest.h>

#include "psx/cli/run_command.hpp"

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

TEST(ParseRunTest, ConcurrencyFlag) {
    EXPECT_EQ(psx::cli::parseRun({"--", "id"}).concurrency, 64);
    EXPECT_EQ(psx::cli::parseRun({"-c", "8", "--", "id"}).concurrency, 8);
    EXPECT_EQ(psx::cli::parseRun({"--concurrency", "256", "--", "id"}).concurrency, 256);
    EXPECT_EQ(psx::cli::parseRun({"-c", "0", "--", "id"}).concurrency, 0); // 0 = all at once
    EXPECT_THROW(static_cast<void>(psx::cli::parseRun({"-c", "x", "--", "id"})), std::runtime_error);
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
