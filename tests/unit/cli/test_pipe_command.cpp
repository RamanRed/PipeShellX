#include "psx/cli/pipe_command.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
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

TEST(PipeCommandTest, RejectsUnknownOptionLikeArgumentsExactly) {
    std::ostringstream out, err;

    EXPECT_EQ(pipeSubcommand({"--bogus", "'echo hello'"}, out, err), 2);
    EXPECT_TRUE(out.str().empty());
    EXPECT_EQ(err.str(), "pipeshellx pipe: unknown option '--bogus'\n");
}

TEST(PipeCommandTest, RejectsEveryMissingOptionValue) {
    for (const std::string option : {"-i", "-f", "--file", "--cert", "--key", "--ca", "--native-port"}) {
        SCOPED_TRACE(option);
        std::ostringstream out, err;
        EXPECT_EQ(pipeSubcommand({option}, out, err), 2);
        EXPECT_TRUE(out.str().empty());
        EXPECT_EQ(err.str(), "pipeshellx pipe: option '" + option + "' requires a value\n");
    }

    std::ostringstream out, err;
    EXPECT_EQ(pipeSubcommand({"-i", "--check", "'echo hello'"}, out, err), 2);
    EXPECT_EQ(err.str(), "pipeshellx pipe: option '-i' requires a value\n");
}

TEST(PipeCommandTest, RejectsDuplicateSingletonOptions) {
    const std::vector<std::vector<std::string>> duplicates{
        {"-i", "one", "-i", "two", "'echo hello'"},
        {"-f", "one.yaml", "--file", "two.yaml"},
        {"--cert", "one", "--cert", "two", "'echo hello'"},
        {"--key", "one", "--key", "two", "'echo hello'"},
        {"--ca", "one", "--ca", "two", "'echo hello'"},
        {"--native-port", "1", "--native-port", "2", "'echo hello'"},
        {"--check", "--check", "'echo hello'"},
    };
    for (const auto& args : duplicates) {
        SCOPED_TRACE(::testing::PrintToString(args));
        std::ostringstream out, err;
        EXPECT_EQ(pipeSubcommand(args, out, err), 2);
        EXPECT_TRUE(out.str().empty());
        EXPECT_NE(err.str().find("duplicate option"), std::string::npos) << err.str();
    }
}

TEST(PipeCommandTest, NativePortRequiresAnExactIntegerAcrossTheFullPortRange) {
    for (const std::string port : {"-1", "0", "65536", "7433junk", "999999999999999999999999"}) {
        SCOPED_TRACE(port);
        std::ostringstream out, err;
        EXPECT_EQ(pipeSubcommand({"--native-port", port, "'true'"}, out, err), 2);
        EXPECT_TRUE(out.str().empty());
        EXPECT_EQ(err.str(), "pipeshellx pipe: --native-port must be an integer in 1..65535\n");
    }

    for (const std::string port : {"1", "65535"}) {
        SCOPED_TRACE(port);
        std::ostringstream out, err;
        EXPECT_EQ(pipeSubcommand({"--native-port", port, "'true'"}, out, err), 0) << err.str();
    }
}

TEST(PipeCommandTest, OptionLikeCommandArgumentsRemainValidInsideTheQuotedSpec) {
    std::ostringstream out, err;

    EXPECT_EQ(pipeSubcommand({"'/bin/echo -n kept' | '/bin/cat'"}, out, err), 0) << err.str();
    EXPECT_EQ(out.str(), "kept");
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

TEST(PipeCommandTest, FileExecutesDeclaredEdgesInsteadOfDeclarationOrder) {
    test_support::ScopedTempCwd cwd("pipe-file-order");
    std::ofstream("pipeline.yaml") << R"yaml(stages:
  - id: upper
    run: [/usr/bin/tr, a-z, A-Z]
  - id: source
    run: [/bin/echo, hello]
edges:
  - from: source
    to: upper
)yaml";
    std::ostringstream out, err;

    EXPECT_EQ(pipeSubcommand({"--file", "pipeline.yaml"}, out, err), 0) << err.str();
    EXPECT_EQ(out.str(), "HELLO\n");
}

TEST(PipeCommandTest, FileExecutesLocalFanInAndFanOut) {
    test_support::ScopedTempCwd cwd("pipe-file-dag");
    std::ofstream("pipeline.yaml") << R"yaml(stages:
  - id: joined
    run: [/usr/bin/sort]
  - id: left
    run: [/bin/echo, beta]
  - id: right
    run: [/bin/echo, alpha]
  - id: prefixed
    run: [/usr/bin/sed, s/^/joined:/]
  - id: counted
    run: [/usr/bin/wc, -l]
edges:
  - from: left
    to: joined
  - from: right
    to: joined
  - from: joined
    to: prefixed
  - from: joined
    to: counted
)yaml";
    std::ostringstream out, err;

    EXPECT_EQ(pipeSubcommand({"--file", "pipeline.yaml"}, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("joined:alpha\njoined:beta\n"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("2"), std::string::npos) << out.str();
}

TEST(PipeCommandTest, FilePipefailUsesRightmostFailureInTopologicalOrder) {
    test_support::ScopedTempCwd cwd("pipe-file-pipefail");
    std::ofstream("pipeline.yaml") << R"yaml(stages:
  - id: last
    run: [/bin/sh, -c, "cat >/dev/null; exit 7"]
  - id: first
    run: [/bin/sh, -c, "exit 3"]
edges:
  - from: first
    to: last
)yaml";
    std::ostringstream out, err;

    EXPECT_EQ(pipeSubcommand({"--file", "pipeline.yaml"}, out, err), 7) << err.str();
    EXPECT_TRUE(out.str().empty());
}

TEST(PipeCommandTest, LocalFileInterruptReturns130Promptly) {
    test_support::ScopedTempCwd cwd("pipe-file-interrupt");
    std::ofstream("pipeline.yaml") << R"yaml(stages:
  - id: long-running
    run: [/bin/sh, -c, "kill -INT $PPID; sleep 30"]
edges:
)yaml";
    std::ostringstream out, err;

    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(pipeSubcommand({"--file", "pipeline.yaml"}, out, err), 130) << err.str();
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(5));
    EXPECT_TRUE(out.str().empty());
}

TEST(PipeCommandTest, RejectsUnsupportedNonLinearRemoteDagExactly) {
    test_support::ScopedTempCwd cwd("pipe-file-remote-dag");
    std::ofstream("pipeline.yaml") << R"yaml(stages:
  - id: source
    run: [/bin/echo, hello]
  - id: remote-a
    run: [/bin/cat]
    at: node-a
  - id: remote-b
    run: [/bin/cat]
    at: node-b
edges:
  - from: source
    to: remote-a
  - from: source
    to: remote-b
)yaml";
    std::ostringstream out, err;

    EXPECT_EQ(pipeSubcommand({"--file", "pipeline.yaml"}, out, err), 2);
    EXPECT_EQ(err.str(), "pipeshellx pipe: non-linear remote DAGs are not supported; use a single declared chain\n");
    EXPECT_TRUE(out.str().empty());
}
