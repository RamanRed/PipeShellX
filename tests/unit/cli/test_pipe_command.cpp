#include "psx/cli/pipe_command.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using psx::cli::pipeSubcommand;

TEST(PipeCommandTest, RunsALocalTwoStagePipeline) {
    std::ostringstream out, err;
    const int rc = pipeSubcommand({"'echo hello' | 'tr a-z A-Z'"}, out, err);
    EXPECT_EQ(rc, 0) << err.str();
    EXPECT_EQ(out.str(), "HELLO\n");
}

TEST(PipeCommandTest, ReturnsThePipefailExitCode) {
    std::ostringstream out, err;
    // grep with no match exits 1; pipefail surfaces it as the pipeline code.
    const int rc = pipeSubcommand({"'echo hi' | 'grep nomatch'"}, out, err);
    EXPECT_EQ(rc, 1);
    EXPECT_TRUE(out.str().empty());
}

TEST(PipeCommandTest, MixedPipelineIsAllowedButNeedsAnInventory) {
    std::ostringstream out, err;
    // ps remote, wc local: a mixed pipeline is now spliced, but the remote part
    // still needs an inventory to resolve @web.
    EXPECT_EQ(pipeSubcommand({"'ps'@web | wc"}, out, err), 2);
    EXPECT_NE(err.str().find("inventory"), std::string::npos);
}

TEST(PipeCommandTest, RemoteStagesNeedAnInventory) {
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({"'ps'@web"}, out, err), 2);
    EXPECT_NE(err.str().find("inventory"), std::string::npos);
}

TEST(PipeCommandTest, RemoteStagesNeedControllerCerts) {
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({"-i", "inv.txt", "'ps'@web"}, out, err), 2);
    EXPECT_NE(err.str().find("--cert"), std::string::npos);
}

TEST(PipeCommandTest, RejectsUsageAndMalformedSpecs) {
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({}, out, err), 2) << "no spec";
    EXPECT_EQ(pipeSubcommand({"'oops"}, out, err), 2) << "unterminated quote";
    EXPECT_EQ(pipeSubcommand({"'a' | | 'b'"}, out, err), 2) << "empty stage";
}

TEST(PipeCommandTest, CheckValidatesALocalPipelineWithoutRunning) {
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({"--check", "'grep x' | 'sort -u'"}, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("valid"), std::string::npos);
    EXPECT_NE(out.str().find("@local"), std::string::npos);
    EXPECT_TRUE(err.str().empty());
}

TEST(PipeCommandTest, CheckResolvesRemotePlacementsFromTheInventory) {
    test_support::ScopedTempCwd cwd("pipe-check");
    std::ofstream("inv.txt") << "[fleet]\nweb native_port=7000 san=spiffe://psx/node/web\n";
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({"--check", "-i", "inv.txt", "'grep x'@web | 'sort'@local"}, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("web:7000"), std::string::npos);
    EXPECT_NE(out.str().find("valid"), std::string::npos);
}

TEST(PipeCommandTest, CheckRejectsAnUnknownPlacementWithoutCrashing) {
    test_support::ScopedTempCwd cwd("pipe-check-bad");
    std::ofstream("inv.txt") << "[fleet]\nweb native_port=7000\n";
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({"--check", "-i", "inv.txt", "'ps'@nonexistent"}, out, err), 2);
    EXPECT_NE(err.str().find("no such host"), std::string::npos);
}

TEST(PipeCommandTest, CheckRemoteNeedsInventoryAndRejectsBadSpecs) {
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({"--check", "'ps'@web"}, out, err), 2); // remote, no -i
    std::ostringstream out2, err2;
    EXPECT_EQ(pipeSubcommand({"--check", "'oops"}, out2, err2), 2); // malformed spec
}
