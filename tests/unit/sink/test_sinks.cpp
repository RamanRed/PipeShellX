#include <gtest/gtest.h>

#include "psx/sink/group_sink.hpp"
#include "psx/sink/json_sink.hpp"
#include "psx/sink/ordered_sink.hpp"
#include "psx/sink/stream_sink.hpp"

#include <sstream>
#include <string>

using psx::sink::Channel;
using psx::sink::GroupSink;
using psx::sink::JsonSink;
using psx::sink::OrderedSink;
using psx::sink::RunSummary;
using psx::sink::StageResult;
using psx::sink::StreamSink;

// ---------------------------------------------------------------- GroupSink

TEST(GroupSinkTest, EmitsPerStageHeaderThenStdoutThenStderr) {
    std::ostringstream out;
    GroupSink sink(out);
    sink.stageStarted("alice@h1");
    sink.line("alice@h1", Channel::Stdout, "one");
    sink.line("alice@h1", Channel::Stdout, "two");
    sink.line("alice@h1", Channel::Stderr, "warn");
    sink.stageFinished("alice@h1", StageResult{0, false, "", 0});
    EXPECT_EQ(out.str(), "CLIENT alice@h1\none\ntwo\nwarn\n");
}

TEST(GroupSinkTest, PrefersTheNormalizedErrorOverRawStderr) {
    std::ostringstream out;
    GroupSink sink(out);
    sink.line("u@h", Channel::Stderr, "raw ssh noise");
    sink.stageFinished("u@h", StageResult{255, false, "ERROR: connection failed", 0});
    EXPECT_EQ(out.str(), "CLIENT u@h\nERROR: connection failed\n");
}

TEST(GroupSinkTest, StagesAppearInFinishOrderEachGroupedTogether) {
    std::ostringstream out;
    GroupSink sink(out);
    sink.line("a", Channel::Stdout, "a1");
    sink.line("b", Channel::Stdout, "b1");
    sink.line("a", Channel::Stdout, "a2");
    sink.stageFinished("b", StageResult{0, false, "", 0});
    sink.stageFinished("a", StageResult{0, false, "", 0});
    EXPECT_EQ(out.str(), "CLIENT b\nb1\nCLIENT a\na1\na2\n");
}

TEST(OrderedSinkTest, ReplaysStagesGroupedAndSortedAtRunEnd) {
    std::ostringstream out;
    OrderedSink sink(std::make_unique<GroupSink>(out));
    sink.line("z", Channel::Stdout, "z1");
    sink.line("a", Channel::Stdout, "a1");
    sink.line("z", Channel::Stdout, "z2");
    sink.stageFinished("z", StageResult{0, false, "", 0});
    sink.stageFinished("a", StageResult{0, false, "", 0});

    EXPECT_TRUE(out.str().empty());
    sink.runFinished(RunSummary{2, 2, 0, 0, false});
    EXPECT_EQ(out.str(), "CLIENT a\na1\nCLIENT z\nz1\nz2\n");
}

// ---------------------------------------------------------------- StreamSink

TEST(StreamSinkTest, PrefixesEachLineWithItsStageLive) {
    std::ostringstream out;
    std::ostringstream err;
    StreamSink sink(out, err, /*colour=*/false);
    sink.line("web001", Channel::Stdout, "GET / 200");
    sink.line("web002", Channel::Stdout, "GET / 500");
    sink.line("web001", Channel::Stderr, "disk full");
    EXPECT_EQ(out.str(), "[web001] GET / 200\n[web002] GET / 500\n");
    EXPECT_EQ(err.str(), "[web001] disk full\n");
}

TEST(StreamSinkTest, ColourIsStablePerStageAndResets) {
    std::ostringstream out;
    std::ostringstream err;
    StreamSink sink(out, err, /*colour=*/true);
    sink.line("hostA", Channel::Stdout, "x");
    sink.line("hostB", Channel::Stdout, "y");
    sink.line("hostA", Channel::Stdout, "z");
    const std::string text = out.str();
    // Every line carries an ANSI colour and a reset.
    EXPECT_NE(text.find("\033["), std::string::npos);
    EXPECT_NE(text.find("\033[0m"), std::string::npos);
    // hostA's two lines use the same colour code; hostB differs.
    auto colourOf = [&](const std::string& tag) {
        const std::size_t at = text.find("[" + tag + "]");
        const std::size_t start = text.rfind("\033[", at);
        return text.substr(start, text.find('m', start) - start + 1);
    };
    EXPECT_EQ(colourOf("hostA"), colourOf("hostA"));
    EXPECT_NE(colourOf("hostA"), colourOf("hostB"));
}

TEST(StreamSinkTest, SummaryGoesToStderrWithCountsAndDrops) {
    std::ostringstream out;
    std::ostringstream err;
    StreamSink sink(out, err, false);
    sink.runFinished(RunSummary{3, 2, 1, 4096, false});
    const std::string summary = err.str();
    EXPECT_NE(summary.find("3"), std::string::npos);    // stages
    EXPECT_NE(summary.find("2"), std::string::npos);    // succeeded
    EXPECT_NE(summary.find("1"), std::string::npos);    // failed
    EXPECT_NE(summary.find("4096"), std::string::npos); // dropped bytes
}

// ---------------------------------------------------------------- JsonSink

TEST(JsonSinkTest, OneObjectPerStageThenASummary) {
    std::ostringstream out;
    JsonSink sink(out);
    sink.line("h1", Channel::Stdout, "hello");
    sink.line("h1", Channel::Stdout, "world");
    sink.line("h1", Channel::Stderr, "oops");
    sink.stageFinished("h1", StageResult{0, false, "", 0});
    sink.runFinished(RunSummary{1, 1, 0, 0, false});

    const std::string text = out.str();
    // JSON Lines: two lines, each a complete object.
    std::istringstream lines(text);
    std::string stageLine;
    std::string summaryLine;
    ASSERT_TRUE(std::getline(lines, stageLine));
    ASSERT_TRUE(std::getline(lines, summaryLine));
    EXPECT_NE(stageLine.find("\"stage\":\"h1\""), std::string::npos) << stageLine;
    EXPECT_NE(stageLine.find("\"exit\":0"), std::string::npos) << stageLine;
    EXPECT_NE(stageLine.find("\"stdout\":\"hello\\nworld\""), std::string::npos) << stageLine;
    EXPECT_NE(stageLine.find("\"stderr\":\"oops\""), std::string::npos) << stageLine;
    EXPECT_NE(stageLine.find("\"timed_out\":false"), std::string::npos) << stageLine;
    EXPECT_NE(summaryLine.find("\"summary\":true"), std::string::npos) << summaryLine;
    EXPECT_NE(summaryLine.find("\"stages\":1"), std::string::npos) << summaryLine;
    EXPECT_NE(summaryLine.find("\"failed\":0"), std::string::npos) << summaryLine;
}

TEST(JsonSinkTest, EscapesControlCharactersAndQuotes) {
    std::ostringstream out;
    JsonSink sink(out);
    sink.line("h", Channel::Stdout, "quote:\" back:\\ tab:\there");
    sink.stageFinished("h", StageResult{2, false, "", 0});
    sink.runFinished(RunSummary{1, 0, 1, 0, false});
    const std::string text = out.str();
    // Input: quote:" back:\ tab:<TAB>here  ->  escaped in the JSON string.
    EXPECT_NE(text.find(R"(quote:\" back:\\ tab:\there)"), std::string::npos) << text;
}

TEST(JsonSinkTest, TimedOutStageCarriesTheFlagAndError) {
    std::ostringstream out;
    JsonSink sink(out);
    sink.stageFinished("h", StageResult{-1, true, "ERROR: command timed out", 128});
    const std::string text = out.str();
    EXPECT_NE(text.find("\"timed_out\":true"), std::string::npos) << text;
    EXPECT_NE(text.find("\"error\":\"ERROR: command timed out\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"dropped\":128"), std::string::npos) << text;
    EXPECT_NE(text.find("\"exit\":-1"), std::string::npos) << text;
}

TEST(JsonSinkTest, CancelledRunSummarySetsTheFlag) {
    std::ostringstream out;
    JsonSink sink(out);
    sink.runFinished(RunSummary{5, 3, 1, 0, true});
    EXPECT_NE(out.str().find("\"cancelled\":true"), std::string::npos) << out.str();
}
