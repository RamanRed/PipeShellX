#include "psx/pipeline/pipeline_yaml.hpp"

#include "psx/pipeline/planner.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using psx::ErrorClass;
using psx::pipeline::loadPipelineYaml;

TEST(PipelineYamlTest, ParsesALinearPipeline) {
    auto pipeline = loadPipelineYaml(R"yaml(
stages:
  - id: a
    run: "grep foo"
  - id: b
    run: "sort"
edges:
  - from: a
    to: b
)yaml");

    ASSERT_TRUE(pipeline.ok()) << (pipeline.ok() ? "" : pipeline.error().message());
    ASSERT_EQ(pipeline.value().stages.size(), 2U);
    EXPECT_EQ(pipeline.value().stages[0].id, "a");
    EXPECT_EQ(pipeline.value().stages[0].argv, (std::vector<std::string>{"grep", "foo"}));
    EXPECT_EQ(pipeline.value().stages[1].id, "b");
    EXPECT_EQ(pipeline.value().stages[1].argv, (std::vector<std::string>{"sort"}));
    ASSERT_EQ(pipeline.value().edges.size(), 1U);
    EXPECT_EQ(pipeline.value().edges[0].from, "a");
    EXPECT_EQ(pipeline.value().edges[0].to, "b");
}

TEST(PipelineYamlTest, ParsesFanInAndFanOut) {
    auto pipeline = loadPipelineYaml(R"yaml(
stages:
  - id: source-a
    run: [cat, a.txt]
  - id: source-b
    run: [cat, b.txt]
  - id: sink-a
    run: [sort]
  - id: sink-b
    run: [uniq]
edges:
  - from: source-a
    to: sink-a
  - from: source-b
    to: sink-a
  - from: source-a
    to: sink-b
  - from: source-b
    to: sink-b
)yaml");

    ASSERT_TRUE(pipeline.ok()) << (pipeline.ok() ? "" : pipeline.error().message());
    EXPECT_EQ(pipeline.value().edges.size(), 4U);
    EXPECT_EQ(pipeline.value().edges[0].from, "source-a");
    EXPECT_EQ(pipeline.value().edges[0].to, "sink-a");
    EXPECT_EQ(pipeline.value().edges[1].from, "source-b");
    EXPECT_EQ(pipeline.value().edges[1].to, "sink-a");
    EXPECT_EQ(pipeline.value().edges[2].from, "source-a");
    EXPECT_EQ(pipeline.value().edges[2].to, "sink-b");
    EXPECT_EQ(pipeline.value().edges[3].from, "source-b");
    EXPECT_EQ(pipeline.value().edges[3].to, "sink-b");
}

TEST(PipelineYamlTest, ParsesPlacements) {
    auto pipeline = loadPipelineYaml(R"yaml(
stages:
  - id: local-stage
    run: "echo"
    at: local
  - id: host-stage
    run: "hostname"
    at: worker-1
  - id: group-stage
    run: "uptime"
    at: workers
)yaml");

    ASSERT_TRUE(pipeline.ok()) << (pipeline.ok() ? "" : pipeline.error().message());
    EXPECT_EQ(pipeline.value().stages[0].placement, "local");
    EXPECT_EQ(pipeline.value().stages[1].placement, "worker-1");
    EXPECT_EQ(pipeline.value().stages[2].placement, "workers");
}

TEST(PipelineYamlTest, AcceptsQuotedAndListRunForms) {
    auto pipeline = loadPipelineYaml(R"yaml(
stages:
  - id: quoted
    run: "grep foo"
  - id: listed
    run: [grep, foo]
)yaml");

    ASSERT_TRUE(pipeline.ok()) << (pipeline.ok() ? "" : pipeline.error().message());
    EXPECT_EQ(pipeline.value().stages[0].argv, (std::vector<std::string>{"grep", "foo"}));
    EXPECT_EQ(pipeline.value().stages[1].argv, (std::vector<std::string>{"grep", "foo"}));
}

TEST(PipelineYamlTest, RejectsCyclesThroughPlanner) {
    auto pipeline = loadPipelineYaml(R"yaml(
stages:
  - id: a
    run: a
  - id: b
    run: b
edges:
  - from: a
    to: b
  - from: b
    to: a
)yaml");

    ASSERT_FALSE(pipeline.ok());
    EXPECT_EQ(pipeline.error().cls, ErrorClass::InvalidArgument);
    EXPECT_NE(pipeline.error().message().find("cycle"), std::string::npos);
}

TEST(PipelineYamlTest, RejectsMalformedYamlWithClearErrors) {
    const auto badIndent = loadPipelineYaml("stages:\n - id: a\n   run: a\n");
    ASSERT_FALSE(badIndent.ok());
    EXPECT_EQ(badIndent.error().cls, ErrorClass::InvalidArgument);
    EXPECT_NE(badIndent.error().message().find("indent"), std::string::npos);

    const auto unknownKey = loadPipelineYaml("stages:\n  - id: a\n    command: a\n");
    ASSERT_FALSE(unknownKey.ok());
    EXPECT_EQ(unknownKey.error().cls, ErrorClass::InvalidArgument);
    EXPECT_NE(unknownKey.error().message().find("unknown key"), std::string::npos);

    const auto unterminated = loadPipelineYaml("stages:\n  - id: a\n    run: \"echo\n");
    ASSERT_FALSE(unterminated.ok());
    EXPECT_EQ(unterminated.error().cls, ErrorClass::InvalidArgument);
    EXPECT_NE(unterminated.error().message().find("unterminated"), std::string::npos);
}
