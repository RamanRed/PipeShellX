#include "psx/pipeline/parser.hpp"

#include "psx/pipeline/planner.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using psx::pipeline::parsePipeSpec;
using psx::pipeline::Pipeline;
using psx::pipeline::Planner;

namespace {
std::vector<std::string> argv(const Pipeline& p, std::size_t i) {
    return p.stages.at(i).argv;
}
} // namespace

TEST(PipeParserTest, ParsesASingleQuotedStage) {
    auto p = parsePipeSpec("'echo hello world'");
    ASSERT_TRUE(p.ok()) << (p.ok() ? "" : p.error().message());
    ASSERT_EQ(p.value().stages.size(), 1U);
    EXPECT_EQ(p.value().stages[0].id, "s0");
    EXPECT_EQ(argv(p.value(), 0), (std::vector<std::string>{"echo", "hello", "world"}));
    EXPECT_TRUE(p.value().stages[0].placement.empty());
    EXPECT_TRUE(p.value().edges.empty());
}

TEST(PipeParserTest, ParsesStagesWithPlacementsAndAChainEdge) {
    auto p = parsePipeSpec("'grep ERROR'@web | 'sort -u'@db");
    ASSERT_TRUE(p.ok()) << (p.ok() ? "" : p.error().message());
    ASSERT_EQ(p.value().stages.size(), 2U);
    EXPECT_EQ(argv(p.value(), 0), (std::vector<std::string>{"grep", "ERROR"}));
    EXPECT_EQ(p.value().stages[0].placement, "web");
    EXPECT_EQ(argv(p.value(), 1), (std::vector<std::string>{"sort", "-u"}));
    EXPECT_EQ(p.value().stages[1].placement, "db");
    ASSERT_EQ(p.value().edges.size(), 1U);
    EXPECT_EQ(p.value().edges[0].from, "s0");
    EXPECT_EQ(p.value().edges[0].to, "s1");
}

TEST(PipeParserTest, APipeInsideQuotesIsLiteralNotAnEdge) {
    auto p = parsePipeSpec("'grep a|b'@web");
    ASSERT_TRUE(p.ok()) << (p.ok() ? "" : p.error().message());
    ASSERT_EQ(p.value().stages.size(), 1U);
    EXPECT_EQ(argv(p.value(), 0), (std::vector<std::string>{"grep", "a|b"}));
}

TEST(PipeParserTest, ParsesBareTokensWithAndWithoutPlacement) {
    auto p = parsePipeSpec("ps@host1 | wc");
    ASSERT_TRUE(p.ok()) << (p.ok() ? "" : p.error().message());
    ASSERT_EQ(p.value().stages.size(), 2U);
    EXPECT_EQ(argv(p.value(), 0), (std::vector<std::string>{"ps"}));
    EXPECT_EQ(p.value().stages[0].placement, "host1");
    EXPECT_EQ(argv(p.value(), 1), (std::vector<std::string>{"wc"}));
    EXPECT_TRUE(p.value().stages[1].placement.empty());
}

TEST(PipeParserTest, AnAtInsideAQuotedCommandStaysInTheCommand) {
    auto p = parsePipeSpec("'mail admin@example.com'@relay");
    ASSERT_TRUE(p.ok()) << (p.ok() ? "" : p.error().message());
    ASSERT_EQ(p.value().stages.size(), 1U);
    EXPECT_EQ(argv(p.value(), 0), (std::vector<std::string>{"mail", "admin@example.com"}));
    EXPECT_EQ(p.value().stages[0].placement, "relay");
}

TEST(PipeParserTest, TheParsedPipelinePlansInOrder) {
    auto p = parsePipeSpec("'a'@h1 | 'b'@h2 | 'c'@h3");
    ASSERT_TRUE(p.ok());
    auto plan = Planner::plan(p.value());
    ASSERT_TRUE(plan.ok()) << (plan.ok() ? "" : plan.error().message());
    EXPECT_EQ(plan.value().order, (std::vector<std::string>{"s0", "s1", "s2"}));
}

TEST(PipeParserTest, RejectsMalformedSpecs) {
    EXPECT_FALSE(parsePipeSpec("").ok()) << "empty spec";
    EXPECT_FALSE(parsePipeSpec("   ").ok()) << "blank spec";
    EXPECT_FALSE(parsePipeSpec("'a' | | 'b'").ok()) << "empty stage";
    EXPECT_FALSE(parsePipeSpec("'oops").ok()) << "unterminated quote";
    EXPECT_FALSE(parsePipeSpec("'x'@").ok()) << "empty placement";
    EXPECT_FALSE(parsePipeSpec("grep foo").ok()) << "unquoted with spaces";
    EXPECT_FALSE(parsePipeSpec("'a' junk").ok()) << "junk after quoted command";
}
