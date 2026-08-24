#include "psx/sink/consensus.hpp"
#include "psx/sink/consensus_sink.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

using psx::sink::consensus;
using psx::sink::ConsensusReport;
using psx::sink::renderConsensus;
using psx::sink::renderConsensusJson;

namespace {
using HostOut = std::pair<std::string, std::string>;
}

TEST(ConsensusTest, UnanimousIsASingleBucket) {
    auto report = consensus({{"a", "cfg\n"}, {"b", "cfg\n"}, {"c", "cfg\n"}});
    ASSERT_EQ(report.buckets.size(), 1U);
    EXPECT_TRUE(report.unanimous());
    EXPECT_EQ(report.hostCount(), 3U);
    EXPECT_EQ(report.buckets[0].hosts, (std::vector<std::string>{"a", "b", "c"}));

    std::ostringstream out;
    renderConsensus(report, out);
    EXPECT_NE(out.str().find("all 3 hosts agree"), std::string::npos);
}

TEST(ConsensusTest, MajorityFirstOutlierAfter) {
    auto report = consensus({{"web1", "v1\n"}, {"web2", "v1\n"}, {"web3", "v1\n"}, {"db1", "v2\n"}});
    ASSERT_EQ(report.buckets.size(), 2U);
    EXPECT_FALSE(report.unanimous());
    // Majority (v1, 3 hosts) first.
    EXPECT_EQ(report.buckets[0].hosts.size(), 3U);
    EXPECT_EQ(report.buckets[0].output, "v1\n");
    // Outlier (v2, 1 host: db1).
    EXPECT_EQ(report.buckets[1].hosts, (std::vector<std::string>{"db1"}));
    EXPECT_EQ(report.buckets[1].output, "v2\n");

    std::ostringstream out;
    renderConsensus(report, out);
    const std::string text = out.str();
    EXPECT_NE(text.find("3/4 hosts agree"), std::string::npos);
    EXPECT_NE(text.find("1 outlier"), std::string::npos);
    EXPECT_NE(text.find("db1"), std::string::npos);
    EXPECT_NE(text.find("v2"), std::string::npos);
}

TEST(ConsensusTest, HostsWithinABucketAreSorted) {
    auto report = consensus({{"z", "x"}, {"a", "x"}, {"m", "x"}});
    ASSERT_EQ(report.buckets.size(), 1U);
    EXPECT_EQ(report.buckets[0].hosts, (std::vector<std::string>{"a", "m", "z"}));
}

TEST(ConsensusTest, TiesAreOrderedDeterministicallyByOutput) {
    // Two buckets of size 2: order is deterministic (output ascending).
    auto report = consensus({{"h1", "bbb"}, {"h2", "bbb"}, {"h3", "aaa"}, {"h4", "aaa"}});
    ASSERT_EQ(report.buckets.size(), 2U);
    EXPECT_EQ(report.buckets[0].output, "aaa"); // ascending tie-break
    EXPECT_EQ(report.buckets[1].output, "bbb");
}

TEST(ConsensusTest, ThreeWaySplit) {
    auto report = consensus({{"a", "1"}, {"b", "1"}, {"c", "2"}, {"d", "3"}});
    ASSERT_EQ(report.buckets.size(), 3U);
    EXPECT_EQ(report.buckets[0].output, "1"); // majority (2)
    EXPECT_EQ(report.buckets[0].hosts.size(), 2U);

    std::ostringstream out;
    renderConsensus(report, out);
    EXPECT_NE(out.str().find("2 outliers"), std::string::npos);
}

TEST(ConsensusTest, EmptyInputRendersNoHosts) {
    auto report = consensus({});
    EXPECT_TRUE(report.buckets.empty());
    EXPECT_EQ(report.hostCount(), 0U);
    std::ostringstream out;
    renderConsensus(report, out);
    EXPECT_NE(out.str().find("no hosts"), std::string::npos);
}

TEST(ConsensusTest, JsonReportListsBucketsMajorityFirst) {
    auto report = consensus({{"web1", "v1\n"}, {"web2", "v1\n"}, {"db1", "v2\n"}});
    std::ostringstream out;
    renderConsensusJson(report, out);
    const std::string json = out.str();
    EXPECT_NE(json.find("\"unanimous\":false"), std::string::npos);
    EXPECT_NE(json.find("\"hosts\":3"), std::string::npos);
    EXPECT_NE(json.find("\"web1\""), std::string::npos);
    EXPECT_NE(json.find("\"db1\""), std::string::npos);
    EXPECT_NE(json.find("v1\\n"), std::string::npos); // newline escaped
    // The majority bucket precedes the outlier.
    EXPECT_LT(json.find("web1"), json.find("db1"));
}

TEST(ConsensusTest, JsonEscapesSpecialCharactersInOutput) {
    auto report = consensus({{"a", "q\"t\tn\n"}});
    std::ostringstream out;
    renderConsensusJson(report, out);
    EXPECT_NE(out.str().find("q\\\"t\\tn\\n"), std::string::npos); // quote/tab/newline escaped
    EXPECT_NE(out.str().find("\"unanimous\":true"), std::string::npos);
}

TEST(ConsensusSinkTest, RendersCapturedStdoutAtRunEnd) {
    std::ostringstream out;
    psx::sink::ConsensusSink sink(out, false);
    sink.stageStarted("b");
    sink.line("b", psx::sink::Channel::Stdout, "same");
    sink.stageFinished("b", psx::sink::StageResult{});
    sink.stageStarted("a");
    sink.line("a", psx::sink::Channel::Stdout, "same");
    sink.line("a", psx::sink::Channel::Stderr, "ignored");
    sink.stageFinished("a", psx::sink::StageResult{});

    EXPECT_TRUE(out.str().empty());
    sink.runFinished(psx::sink::RunSummary{2, 2, 0, 0, false});
    EXPECT_NE(out.str().find("all 2 hosts agree"), std::string::npos);
    EXPECT_EQ(out.str().find("ignored"), std::string::npos);
}
