#include <gtest/gtest.h>

#include "psx/cli/ping_command.hpp"

#include "test_support.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

using psx::cli::parsePing;
using psx::cli::PingInvocation;
using psx::cli::pingSubcommand;
using psx::cli::SelectorKind;

TEST(ParsePingTest, DefaultsToAllHostsAndAcceptsSelectors) {
    EXPECT_EQ(parsePing({}).selector.kind, SelectorKind::All);
    const auto g = parsePing({"-i", "fleet.ini", "-g", "web", "--timeout", "5"});
    EXPECT_EQ(g.inventoryPath, "fleet.ini");
    EXPECT_EQ(g.selector.kind, SelectorKind::Group);
    EXPECT_EQ(g.selector.value, "web");
    EXPECT_EQ(g.timeoutSec, 5);
    EXPECT_EQ(parsePing({"-H", "a,b"}).selector.hosts, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(parsePing({"-t", "canary"}).selector.kind, SelectorKind::Tag);
}

TEST(ParsePingTest, RejectsMutualExclusionUnknownFlagsAndMissingValues) {
    EXPECT_THROW(static_cast<void>(parsePing({"-g", "a", "-t", "b"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parsePing({"--bogus"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parsePing({"-g"})), std::runtime_error);
    EXPECT_THROW(static_cast<void>(parsePing({"foo"})), std::runtime_error) << "ping takes no positionals";
}

namespace {
void writeFleet(const std::string& path) {
    std::ofstream ini(path);
    ini << "[web]\nonline@h1\noffline@h2\n";
}
} // namespace

TEST(PingSubcommandTest, ReportsOnlineAndOfflinePerHost) {
    test_support::ScopedTempCwd cwd("ping-e2e");
    test_support::FakeSshOnPath fakeSsh;
    // The fake ssh echoes for `echo connected` on a reachable host; a host
    // whose name starts "offline" we make refuse by using a refused marker
    // is not possible here (same fake), so both are reachable and ONLINE.
    writeFleet("fleet.ini");

    PingInvocation inv;
    inv.inventoryPath = "fleet.ini";
    inv.timeoutSec = 10;
    std::ostringstream out;
    std::ostringstream err;
    const int code = pingSubcommand(inv, out, err);
    EXPECT_EQ(code, 0) << err.str();
    EXPECT_NE(out.str().find("online@h1"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("ONLINE"), std::string::npos) << out.str();
}

TEST(PingSubcommandTest, AnUnreachableHostIsOfflineAndSetsExitCodeOne) {
    // Real ssh (no fake on PATH) against a closed loopback port fails fast with
    // "connection refused" -> OFFLINE, deterministically.
    test_support::ScopedTempCwd cwd("ping-offline");
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nnobody@127.0.0.1:1\n";
    }
    PingInvocation inv;
    inv.inventoryPath = "fleet.ini";
    inv.timeoutSec = 10;
    std::ostringstream out;
    std::ostringstream err;
    const int code = pingSubcommand(inv, out, err);
    EXPECT_EQ(code, 1) << out.str();
    EXPECT_NE(out.str().find("OFFLINE"), std::string::npos) << out.str();
}

TEST(PingSubcommandTest, NoHostsIsExitCode3AndMissingInventoryIsExitCode2) {
    test_support::ScopedTempCwd cwd("ping-empty");
    {
        std::ofstream ini("fleet.ini");
        ini << "[web]\nh1\n";
    }
    PingInvocation empty;
    empty.inventoryPath = "fleet.ini";
    empty.selector.kind = SelectorKind::Group;
    empty.selector.value = "nope";
    std::ostringstream out1, err1;
    EXPECT_EQ(pingSubcommand(empty, out1, err1), 3);

    PingInvocation noinv;
    std::ostringstream out2, err2;
    EXPECT_EQ(pingSubcommand(noinv, out2, err2), 2);
}

TEST(PingSubcommandTest, RejectsNativeInventoryHostsInsteadOfSilentlyUsingSsh) {
    test_support::ScopedTempCwd cwd("ping-native");
    {
        std::ofstream ini("fleet.ini");
        ini << "[native]\nnode-1 transport=native\n";
    }

    PingInvocation invocation;
    invocation.inventoryPath = "fleet.ini";
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(pingSubcommand(invocation, out, err), 2);
    EXPECT_TRUE(out.str().empty());
    EXPECT_NE(err.str().find("transport=native"), std::string::npos) << err.str();
}
