#include "psx/pipeline/dag_runner.hpp"

#include "psx/pipeline/pipeline.hpp"
#include "psx/pipeline/planner.hpp"
#include "psx/runtime/reactor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using psx::pipeline::DagRunner;
using psx::pipeline::Edge;
using psx::pipeline::Pipeline;
using psx::pipeline::Stage;
using psx::runtime::Reactor;

namespace {

Stage stage(std::string id, std::vector<std::string> argv) {
    return Stage{.id = std::move(id), .argv = std::move(argv), .placement = ""};
}

struct RunResult {
    std::string output;
    DagRunner::Outcome outcome;
    std::size_t activeChildren = 0;
    std::size_t bufferedBytes = 0;
    std::size_t peakBufferedBytes = 0;
    std::size_t watchedHandles = 0;
    std::size_t completionCalls = 0;
    bool started = false;
    bool completed = false;
    bool timedOut = false;
};

RunResult runPipeline(const Pipeline& pipeline, std::size_t edgeCapacity = DagRunner::kDefaultEdgeCapacity) {
    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    RunResult result;
    DagRunner runner(r, [&](std::string_view bytes) { result.output.append(bytes); }, edgeCapacity);
    psx::runtime::TimerId watchdog = 0;
    const auto started = runner.run(pipeline, [&](DagRunner::Outcome outcome) {
        result.outcome = std::move(outcome);
        result.completed = true;
        ++result.completionCalls;
        if (watchdog != 0) {
            (void)r.cancel(watchdog);
        }
        r.stop();
    });
    result.started = started.ok();
    if (!started.ok()) {
        return result;
    }
    watchdog = r.after(std::chrono::seconds(5), [&] {
        result.timedOut = true;
        r.stop();
    });
    EXPECT_TRUE(r.run().ok());
    (void)r.cancel(watchdog);
    EXPECT_TRUE(r.runOnce(std::chrono::milliseconds(0)).ok());
    result.activeChildren = runner.activeChildCount();
    result.bufferedBytes = runner.bufferedBytes();
    result.peakBufferedBytes = runner.peakBufferedBytes();
    result.watchedHandles = r.watchedHandles();
    return result;
}

void expectCleanCompletion(const RunResult& result) {
    ASSERT_TRUE(result.started);
    ASSERT_TRUE(result.completed);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.completionCalls, 1U);
    EXPECT_EQ(result.activeChildren, 0U);
    EXPECT_EQ(result.bufferedBytes, 0U);
    EXPECT_EQ(result.watchedHandles, 0U);
}

std::multiset<std::string> outputLines(const std::string& output) {
    std::multiset<std::string> lines;
    std::istringstream input(output);
    for (std::string line; std::getline(input, line);) {
        lines.insert(std::move(line));
    }
    return lines;
}

struct GeneratedDag {
    Pipeline pipeline;
    std::set<std::pair<std::size_t, std::size_t>> edges;
    std::unordered_map<std::string, int> exitCodeByStage;
    std::multiset<std::string> expectedOutput;
};

GeneratedDag generateDag(std::uint32_t seed, std::size_t nodeCount) {
    std::mt19937 random(seed);
    GeneratedDag generated;

    for (std::size_t from = 0; from < nodeCount; ++from) {
        for (std::size_t to = from + 1; to < nodeCount; ++to) {
            if ((random() % 100U) < 28U) {
                generated.edges.emplace(from, to);
            }
        }
    }
    // Every generated case contains both fan-out and fan-in, while the seeded
    // edges above vary the depth, number of paths, and independent branches.
    generated.edges.emplace(0, 2);
    generated.edges.emplace(0, 3);
    generated.edges.emplace(1, 3);
    generated.edges.emplace(2, nodeCount - 1);
    generated.edges.emplace(3, nodeCount - 1);

    std::vector<std::size_t> indegree(nodeCount, 0);
    std::vector<std::size_t> outdegree(nodeCount, 0);
    for (const auto& [from, to] : generated.edges) {
        ++outdegree[from];
        ++indegree[to];
        generated.pipeline.edges.push_back(Edge{"n" + std::to_string(from), "n" + std::to_string(to)});
    }
    // Exercise the runner's duplicate-edge normalization without changing the
    // expected graph or its memory bound.
    generated.pipeline.edges.push_back(generated.pipeline.edges.front());
    std::shuffle(generated.pipeline.edges.begin(), generated.pipeline.edges.end(), random);

    std::vector<std::vector<std::uint64_t>> paths(nodeCount, std::vector<std::uint64_t>(nodeCount, 0));
    for (std::size_t node = 0; node < nodeCount; ++node) {
        if (indegree[node] == 0) {
            paths[node][node] = 1;
        }
        if (outdegree[node] == 0) {
            for (std::size_t source = 0; source < nodeCount; ++source) {
                for (std::uint64_t copy = 0; copy < paths[node][source]; ++copy) {
                    generated.expectedOutput.insert("source-" + std::to_string(source));
                }
            }
        }
        for (const auto& [from, to] : generated.edges) {
            if (from != node) {
                continue;
            }
            for (std::size_t source = 0; source < nodeCount; ++source) {
                paths[to][source] += paths[from][source];
            }
        }
    }

    for (std::size_t node = 0; node < nodeCount; ++node) {
        const std::string id = "n" + std::to_string(node);
        const int exitCode =
            ((seed + static_cast<std::uint32_t>(node * 5U)) % 11U) < 3U ? static_cast<int>((node % 7U) + 1U) : 0;
        generated.exitCodeByStage.emplace(id, exitCode);
        const std::string script =
            indegree[node] == 0 ? "printf 'source-" + std::to_string(node) + "\\n'; exit " + std::to_string(exitCode)
                                : "cat; exit " + std::to_string(exitCode);
        generated.pipeline.stages.push_back(stage(id, {"/bin/sh", "-c", script}));
    }
    std::shuffle(generated.pipeline.stages.begin(), generated.pipeline.stages.end(), random);
    return generated;
}

} // namespace

TEST(DagRunnerTest, UsesDeclaredEdgesInsteadOfStageDeclarationOrder) {
    Pipeline pipeline{
        .stages = {stage("upper", {"/usr/bin/tr", "a-z", "A-Z"}), stage("source", {"/bin/echo", "hello"})},
        .edges = {Edge{"source", "upper"}},
    };

    const RunResult result = runPipeline(pipeline);

    expectCleanCompletion(result);
    EXPECT_EQ(result.output, "HELLO\n");
    EXPECT_EQ(result.outcome.topologicalOrder, (std::vector<std::string>{"source", "upper"}));
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{0, 0}));
}

TEST(DagRunnerTest, FansOneProducerOutToEverySuccessor) {
    Pipeline pipeline{
        .stages = {stage("right", {"/usr/bin/sed", "s/^/right:/"}), stage("source", {"/bin/echo", "value"}),
                   stage("left", {"/usr/bin/sed", "s/^/left:/"})},
        .edges = {Edge{"source", "left"}, Edge{"source", "right"}},
    };

    const RunResult result = runPipeline(pipeline);

    expectCleanCompletion(result);
    EXPECT_NE(result.output.find("left:value\n"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("right:value\n"), std::string::npos) << result.output;
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{0, 0, 0}));
}

TEST(DagRunnerTest, FansEveryPredecessorIntoOneConsumer) {
    Pipeline pipeline{
        .stages = {stage("sink", {"/usr/bin/sort"}), stage("beta", {"/bin/echo", "beta"}),
                   stage("alpha", {"/bin/echo", "alpha"})},
        .edges = {Edge{"beta", "sink"}, Edge{"alpha", "sink"}},
    };

    const RunResult result = runPipeline(pipeline);

    expectCleanCompletion(result);
    EXPECT_EQ(result.output, "alpha\nbeta\n");
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{0, 0, 0}));
}

TEST(DagRunnerTest, BoundsEachEdgeWhileASlowConsumerDrains) {
    constexpr std::size_t kEdgeCapacity = 4096;
    Pipeline pipeline{
        .stages = {stage("source", {"/bin/sh", "-c",
                                    "i=0; while [ $i -lt 20000 ]; do printf 0123456789abcdef; i=$((i+1)); done"}),
                   stage("sink", {"/bin/sh", "-c", "sleep 0.05; wc -c"})},
        .edges = {Edge{"source", "sink"}},
    };

    const RunResult result = runPipeline(pipeline, kEdgeCapacity);

    expectCleanCompletion(result);
    EXPECT_NE(result.output.find("320000"), std::string::npos) << result.output;
    EXPECT_EQ(result.bufferedBytes, 0U);
    EXPECT_LE(result.peakBufferedBytes, kEdgeCapacity);
}

TEST(DagRunnerTest, PipefailUsesTheRightmostRealFailureInTopologicalOrder) {
    Pipeline pipeline{
        .stages = {stage("last", {"/bin/sh", "-c", "cat >/dev/null; exit 7"}),
                   stage("first", {"/bin/sh", "-c", "exit 3"})},
        .edges = {Edge{"first", "last"}},
    };

    const RunResult result = runPipeline(pipeline);

    expectCleanCompletion(result);
    EXPECT_EQ(result.outcome.topologicalOrder, (std::vector<std::string>{"first", "last"}));
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{3, 7}));
    EXPECT_EQ(result.outcome.exitCode, 7);
}

TEST(DagRunnerTest, GeneratedAcyclicGraphsPreserveDataLifecycleAndDeterministicPipefail) {
    constexpr std::size_t kEdgeCapacity = 257;
    for (std::uint32_t caseIndex = 0; caseIndex < 12; ++caseIndex) {
        SCOPED_TRACE("generated DAG case " + std::to_string(caseIndex));
        const GeneratedDag generated = generateDag(0x5eedU + caseIndex * 97U, 5U + caseIndex % 4U);
        const RunResult result = runPipeline(generated.pipeline, kEdgeCapacity);

        expectCleanCompletion(result);
        EXPECT_EQ(outputLines(result.output), generated.expectedOutput);
        EXPECT_EQ(result.outcome.stageExitCodes.size(), generated.pipeline.stages.size());
        EXPECT_EQ(result.outcome.topologicalOrder.size(), generated.pipeline.stages.size());
        EXPECT_LE(result.peakBufferedBytes, generated.edges.size() * kEdgeCapacity);

        int expectedPipefail = 0;
        for (std::size_t index = 0; index < result.outcome.topologicalOrder.size(); ++index) {
            const int expectedStageCode = generated.exitCodeByStage.at(result.outcome.topologicalOrder[index]);
            EXPECT_EQ(result.outcome.stageExitCodes[index], expectedStageCode);
            if (expectedStageCode != 0) {
                expectedPipefail = expectedStageCode;
            }
        }
        EXPECT_EQ(result.outcome.exitCode, expectedPipefail);
    }
}

TEST(DagRunnerTest, EarlyConsumerExitDropsOnlyThatEdgeAndReapsTheProducer) {
    constexpr std::size_t kEdgeCapacity = 1024;
    Pipeline pipeline{
        .stages = {stage("source", {"/bin/sh", "-c", "head -c 1048576 /dev/zero | tr '\\0' x"}),
                   stage("early", {"/bin/sh", "-c", "head -c 1 >/dev/null"})},
        .edges = {Edge{"source", "early"}},
    };

    const RunResult result = runPipeline(pipeline, kEdgeCapacity);

    expectCleanCompletion(result);
    EXPECT_TRUE(result.output.empty());
    EXPECT_EQ(result.outcome.stageExitCodes, (std::vector<int>{0, 0}));
    EXPECT_LE(result.peakBufferedBytes, kEdgeCapacity);
}

TEST(DagRunnerTest, LargeFanOutBackpressuresOnTheSlowestBranchWithinPerEdgeCapacity) {
    constexpr std::size_t kEdgeCapacity = 2048;
    constexpr std::size_t kPayloadBytes = 1024U * 1024U;
    Pipeline pipeline{
        .stages = {stage("source", {"/bin/sh", "-c", "head -c 1048576 /dev/zero | tr '\\0' x"}),
                   stage("fast", {"/bin/sh", "-c", "wc -c"}), stage("slow", {"/bin/sh", "-c", "sleep 0.1; wc -c"})},
        .edges = {Edge{"source", "fast"}, Edge{"source", "slow"}},
    };

    const RunResult result = runPipeline(pipeline, kEdgeCapacity);

    expectCleanCompletion(result);
    std::istringstream output(result.output);
    std::vector<std::size_t> byteCounts;
    for (std::size_t count = 0; output >> count;) {
        byteCounts.push_back(count);
    }
    ASSERT_EQ(byteCounts.size(), 2U) << result.output;
    EXPECT_EQ(byteCounts, (std::vector<std::size_t>{kPayloadBytes, kPayloadBytes}));
    EXPECT_LE(result.peakBufferedBytes, 2U * kEdgeCapacity);
}

TEST(DagRunnerTest, SpawnFailureImmediatelyCleansUpPreviouslyStartedChildren) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();
    DagRunner runner(r);
    std::size_t completionCalls = 0;
    Pipeline pipeline{
        .stages = {stage("started", {"/bin/sh", "-c", "sleep 30"}),
                   stage("missing", {"/definitely/not/a/pipeshellx-program"})},
        .edges = {Edge{"started", "missing"}},
    };

    const auto started = runner.run(pipeline, [&](DagRunner::Outcome) { ++completionCalls; });

    ASSERT_FALSE(started.ok());
    EXPECT_EQ(started.error().cls, psx::ErrorClass::NotFound);
    EXPECT_EQ(completionCalls, 0U);
    EXPECT_EQ(runner.activeChildCount(), 0U);
    EXPECT_EQ(runner.bufferedBytes(), 0U);
    EXPECT_EQ(r.watchedHandles(), 0U);
}

TEST(DagRunnerTest, ExplicitCancellationReapsChildrenAndClearsAllBufferedState) {
    constexpr std::size_t kEdgeCapacity = 1024;
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();
    DagRunner runner(r, {}, kEdgeCapacity);
    std::size_t completionCalls = 0;
    Pipeline pipeline{
        .stages = {stage("source", {"/bin/sh", "-c", "while :; do printf 0123456789abcdef; done"}),
                   stage("blocked", {"/bin/sh", "-c", "sleep 30"})},
        .edges = {Edge{"source", "blocked"}},
    };
    ASSERT_TRUE(runner.run(pipeline, [&](DagRunner::Outcome) { ++completionCalls; }).ok());
    (void)r.after(std::chrono::milliseconds(30), [&] {
        runner.cancel();
        r.stop();
    });

    ASSERT_TRUE(r.run().ok());
    EXPECT_EQ(completionCalls, 0U);
    EXPECT_EQ(runner.activeChildCount(), 0U);
    EXPECT_EQ(runner.bufferedBytes(), 0U);
    EXPECT_LE(runner.peakBufferedBytes(), kEdgeCapacity);
    EXPECT_EQ(r.watchedHandles(), 0U);
}

TEST(DagRunnerTest, RepeatedRunsNeverAccumulateChildrenHandlesOrBufferedBytes) {
    Pipeline pipeline{
        .stages = {stage("source", {"/bin/echo", "repeat"}), stage("left", {"/bin/cat"}), stage("right", {"/bin/cat"})},
        .edges = {Edge{"source", "left"}, Edge{"source", "right"}},
    };

    for (int iteration = 0; iteration < 30; ++iteration) {
        SCOPED_TRACE("repeat iteration " + std::to_string(iteration));
        const RunResult result = runPipeline(pipeline, 128);
        expectCleanCompletion(result);
        EXPECT_EQ(outputLines(result.output), (std::multiset<std::string>{"repeat", "repeat"}));
        EXPECT_LE(result.peakBufferedBytes, 256U);
    }
}
