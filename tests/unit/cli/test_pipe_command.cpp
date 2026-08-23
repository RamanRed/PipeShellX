#include "psx/cli/pipe_command.hpp"

#include <gtest/gtest.h>

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

TEST(PipeCommandTest, RejectsRemotePlacementForNow) {
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({"'ps'@web | wc"}, out, err), 2);
    EXPECT_NE(err.str().find("remote placement"), std::string::npos);
}

TEST(PipeCommandTest, RejectsUsageAndMalformedSpecs) {
    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({}, out, err), 2) << "no spec";
    EXPECT_EQ(pipeSubcommand({"'oops"}, out, err), 2) << "unterminated quote";
    EXPECT_EQ(pipeSubcommand({"'a' | | 'b'"}, out, err), 2) << "empty stage";
}
