#include "psx/pipeline/planner.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using psx::pipeline::Edge;
using psx::pipeline::Pipeline;
using psx::pipeline::Planner;
using psx::pipeline::Stage;

namespace {
Stage stage(const std::string& id, const std::string& cmd = "run") {
    return Stage{.id = id, .argv = {cmd}, .placement = ""};
}

// Position of `id` in the plan order (or -1).
int indexOf(const std::vector<std::string>& order, const std::string& id) {
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
} // namespace

TEST(PlannerTest, OrdersALinearPipeline) {
    Pipeline p{.stages = {stage("a"), stage("b"), stage("c")}, .edges = {{"a", "b"}, {"b", "c"}}};
    auto plan = Planner::plan(p);
    ASSERT_TRUE(plan.ok()) << (plan.ok() ? "" : plan.error().message());
    EXPECT_EQ(plan.value().order, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(PlannerTest, ASingleStageNeedsNoEdges) {
    Pipeline p{.stages = {stage("only")}, .edges = {}};
    auto plan = Planner::plan(p);
    ASSERT_TRUE(plan.ok());
    EXPECT_EQ(plan.value().order, (std::vector<std::string>{"only"}));
}

TEST(PlannerTest, OrdersADiamondWithFanOutAndFanIn) {
    // a -> b, a -> c, b -> d, c -> d
    Pipeline p{.stages = {stage("a"), stage("b"), stage("c"), stage("d")},
               .edges = {{"a", "b"}, {"a", "c"}, {"b", "d"}, {"c", "d"}}};
    auto plan = Planner::plan(p);
    ASSERT_TRUE(plan.ok());
    const auto& order = plan.value().order;
    ASSERT_EQ(order.size(), 4U);
    // Every producer precedes its consumers.
    EXPECT_LT(indexOf(order, "a"), indexOf(order, "b"));
    EXPECT_LT(indexOf(order, "a"), indexOf(order, "c"));
    EXPECT_LT(indexOf(order, "b"), indexOf(order, "d"));
    EXPECT_LT(indexOf(order, "c"), indexOf(order, "d"));
}

TEST(PlannerTest, IsDeterministicByDefinitionOrder) {
    // Two independent sources: the plan follows stage definition order.
    Pipeline p{.stages = {stage("x"), stage("y")}, .edges = {}};
    auto plan = Planner::plan(p);
    ASSERT_TRUE(plan.ok());
    EXPECT_EQ(plan.value().order, (std::vector<std::string>{"x", "y"}));
}

TEST(PlannerTest, DuplicateEdgesAreIgnored) {
    Pipeline p{.stages = {stage("a"), stage("b")}, .edges = {{"a", "b"}, {"a", "b"}}};
    auto plan = Planner::plan(p);
    ASSERT_TRUE(plan.ok());
    EXPECT_EQ(plan.value().order, (std::vector<std::string>{"a", "b"}));
}

TEST(PlannerTest, RejectsACycle) {
    Pipeline p{.stages = {stage("a"), stage("b"), stage("c")}, .edges = {{"a", "b"}, {"b", "c"}, {"c", "a"}}};
    EXPECT_FALSE(Planner::plan(p).ok());
}

TEST(PlannerTest, RejectsASelfLoop) {
    Pipeline p{.stages = {stage("a")}, .edges = {{"a", "a"}}};
    EXPECT_FALSE(Planner::plan(p).ok());
}

TEST(PlannerTest, RejectsADanglingEdge) {
    Pipeline p{.stages = {stage("a")}, .edges = {{"a", "ghost"}}};
    EXPECT_FALSE(Planner::plan(p).ok());
}

TEST(PlannerTest, RejectsStructuralErrors) {
    EXPECT_FALSE(Planner::plan(Pipeline{}).ok()) << "empty pipeline";
    EXPECT_FALSE(Planner::plan(Pipeline{.stages = {stage("a"), stage("a")}, .edges = {}}).ok()) << "duplicate id";
    EXPECT_FALSE(Planner::plan(Pipeline{.stages = {Stage{.id = "", .argv = {"x"}}}, .edges = {}}).ok()) << "empty id";
    EXPECT_FALSE(Planner::plan(Pipeline{.stages = {Stage{.id = "a", .argv = {}}}, .edges = {}}).ok()) << "empty argv";
}
