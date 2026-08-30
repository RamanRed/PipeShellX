#include "psx/pipeline/local_runner.hpp"

#include "psx/pipeline/pipeline.hpp"
#include "psx/runtime/reactor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

using psx::pipeline::LocalRunner;
using psx::pipeline::Stage;
using psx::runtime::Reactor;

namespace {

Stage stage(std::vector<std::string> argv) {
    return Stage{.id = "", .argv = std::move(argv), .placement = ""};
}

struct RunResult {
    std::string output;
    LocalRunner::Outcome outcome;
    bool completed = false;
    bool started = false;
};

RunResult runPipeline(const std::vector<Stage>& stages) {
    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    RunResult result;
    LocalRunner runner(r, [&](std::string_view chunk) { result.output.append(chunk); });
    auto started = runner.run(stages, [&](LocalRunner::Outcome outcome) {
        result.outcome = std::move(outcome);
        result.completed = true;
        r.stop();
    });
    result.started = started.ok();
    if (!started.ok()) {
        return result;
    }
    (void)r.after(std::chrono::seconds(5), [&] { r.stop(); }); // safety net
    EXPECT_TRUE(r.run().ok());
    return result;
}

} // namespace

TEST(LocalRunnerTest, RunsASingleStageAndCapturesStdout) {
    auto result = runPipeline({stage({"/bin/echo", "hello"})});
    ASSERT_TRUE(result.started);
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output, "hello\n");
    EXPECT_EQ(result.outcome.exitCode, 0);
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{0}));
}

TEST(LocalRunnerTest, PipesStdoutIntoTheNextStage) {
    auto result = runPipeline({stage({"/bin/echo", "hello"}), stage({"/usr/bin/tr", "a-z", "A-Z"})});
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output, "HELLO\n");
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{0, 0}));
    EXPECT_EQ(result.outcome.exitCode, 0);
}

TEST(LocalRunnerTest, ThreeStagePipelineFlowsEndToEnd) {
    auto result = runPipeline(
        {stage({"/usr/bin/printf", "3\n1\n2\n"}), stage({"/usr/bin/sort"}), stage({"/usr/bin/head", "-n", "1"})});
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output, "1\n");
    EXPECT_EQ(result.outcome.exitCode, 0);
}

TEST(LocalRunnerTest, PipefailReportsARightmostNonZeroStage) {
    // s0 exits 0, s1 exits 2 (ignores stdin), s2 (cat) exits 0.
    auto result = runPipeline({stage({"/bin/echo", "hi"}), stage({"/bin/sh", "-c", "exit 2"}), stage({"/bin/cat"})});
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{0, 2, 0}));
    EXPECT_EQ(result.outcome.exitCode, 2) << "pipefail: the failing middle stage sets the pipeline code";
}

TEST(LocalRunnerTest, AllStagesSucceedGivesZero) {
    auto result = runPipeline({stage({"/bin/echo", "a"}), stage({"/bin/cat"}), stage({"/bin/cat"})});
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output, "a\n");
    EXPECT_EQ(result.outcome.exitCode, 0);
}

TEST(LocalRunnerTest, CancelKillsAndReapsEveryUnfinishedStage) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    LocalRunner runner(*reactor.value());
    LocalRunner::Outcome outcome;
    bool completed = false;
    ASSERT_TRUE(runner
                    .run({stage({"/usr/bin/yes"})},
                         [&](LocalRunner::Outcome result) {
                             outcome = std::move(result);
                             completed = true;
                             reactor.value()->stop();
                         })
                    .ok());
    reactor.value()->after(std::chrono::milliseconds(10), [&] { runner.cancel(); });
    reactor.value()->after(std::chrono::seconds(3), [&] { reactor.value()->stop(); });
    ASSERT_TRUE(reactor.value()->run().ok());

    ASSERT_TRUE(completed);
    EXPECT_EQ(outcome.stageExitCodes, (std::vector<int>{137}));
    EXPECT_EQ(outcome.exitCode, 137);
}

namespace {
RunResult runWithStdin(const std::vector<Stage>& stages, const std::string& input) {
    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    RunResult result;
    LocalRunner runner(r, [&](std::string_view chunk) { result.output.append(chunk); });
    auto started = runner.run(
        stages,
        [&](LocalRunner::Outcome outcome) {
            result.outcome = std::move(outcome);
            result.completed = true;
            r.stop();
        },
        /*externalStdin=*/true);
    result.started = started.ok();
    if (!started.ok()) {
        return result;
    }
    runner.writeStdin(input);
    runner.closeStdin();
    (void)r.after(std::chrono::seconds(5), [&] { r.stop(); });
    EXPECT_TRUE(r.run().ok());
    return result;
}
} // namespace

TEST(LocalRunnerTest, FeedsExternalStdinToTheFirstStage) {
    auto result = runWithStdin({stage({"/bin/cat"})}, "hello from upstream\n");
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output, "hello from upstream\n");
    EXPECT_EQ(result.outcome.exitCode, 0);
}

TEST(LocalRunnerTest, ExternalStdinFlowsThroughAChain) {
    auto result = runWithStdin({stage({"/bin/cat"}), stage({"/usr/bin/tr", "a-z", "A-Z"})}, "hi there\n");
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output, "HI THERE\n");
    EXPECT_EQ(result.outcome.exitCode, 0);
}

TEST(LocalRunnerTest, LargeExternalStdinRoundTrips) {
    std::string input;
    for (int i = 0; i < 5000; ++i) {
        input += "row " + std::to_string(i) + "\n";
    }
    auto result = runWithStdin({stage({"/bin/cat"})}, input);
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.output.size(), input.size());
    EXPECT_EQ(result.output, input);
}
