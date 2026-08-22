#include <gtest/gtest.h>

#include "psx/stream/line_framer.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

using psx::stream::LineFramer;

namespace {

std::span<const char> bytes(std::string_view text) {
    return std::span<const char>(text.data(), text.size());
}

// Feeds `data` and returns the complete lines emitted (each without its
// terminator), appending them to `out`.
void feed(LineFramer& framer, std::string_view data, std::vector<std::string>& out) {
    framer.push(bytes(data), [&](std::string_view line, bool truncated) {
        out.emplace_back(line);
        EXPECT_FALSE(truncated);
    });
}

} // namespace

TEST(LineFramerTest, EmitsOnlyCompleteLines) {
    LineFramer framer;
    std::vector<std::string> lines;
    feed(framer, "alpha\nbeta\n", lines);
    EXPECT_EQ(lines, (std::vector<std::string>{"alpha", "beta"}));
    EXPECT_FALSE(framer.hasPending());
}

TEST(LineFramerTest, HoldsAPartialLineUntilItsNewlineArrives) {
    LineFramer framer;
    std::vector<std::string> lines;
    feed(framer, "par", lines);
    EXPECT_TRUE(lines.empty());
    EXPECT_TRUE(framer.hasPending());
    feed(framer, "tial\ndone\n", lines);
    EXPECT_EQ(lines, (std::vector<std::string>{"partial", "done"}));
}

TEST(LineFramerTest, SplitsAcrossManyChunksArbitrarily) {
    LineFramer framer;
    std::vector<std::string> lines;
    const std::string input = "one\ntwo\nthree\nfour\n";
    for (char c : input) {
        feed(framer, std::string_view(&c, 1), lines);
    }
    EXPECT_EQ(lines, (std::vector<std::string>{"one", "two", "three", "four"}));
}

TEST(LineFramerTest, NormalisesCrlfToLf) {
    LineFramer framer;
    std::vector<std::string> lines;
    feed(framer, "windows\r\nunix\nmixed\r\n", lines);
    EXPECT_EQ(lines, (std::vector<std::string>{"windows", "unix", "mixed"}));
}

TEST(LineFramerTest, ACrSplitAcrossChunksIsStillStripped) {
    LineFramer framer;
    std::vector<std::string> lines;
    feed(framer, "half\r", lines);
    feed(framer, "\nrest\n", lines);
    EXPECT_EQ(lines, (std::vector<std::string>{"half", "rest"}));
}

TEST(LineFramerTest, EmptyLinesArePreserved) {
    LineFramer framer;
    std::vector<std::string> lines;
    feed(framer, "\n\na\n\n", lines);
    EXPECT_EQ(lines, (std::vector<std::string>{"", "", "a", ""}));
}

TEST(LineFramerTest, FlushEmitsATrailingPartialLineWithTheTruncatedMarkerFalse) {
    LineFramer framer;
    std::vector<std::string> lines;
    feed(framer, "complete\nleftover", lines);
    EXPECT_EQ(lines, (std::vector<std::string>{"complete"}));
    bool sawTruncated = true;
    bool emitted = framer.flush([&](std::string_view line, bool truncated) {
        lines.emplace_back(line);
        sawTruncated = truncated;
    });
    EXPECT_TRUE(emitted);
    EXPECT_FALSE(sawTruncated) << "a trailing partial line at EOF is complete-as-far-as-it-goes";
    EXPECT_EQ(lines.back(), "leftover");
    EXPECT_FALSE(framer.hasPending());
    // A second flush with nothing pending emits nothing.
    EXPECT_FALSE(framer.flush([](std::string_view, bool) { FAIL() << "nothing to flush"; }));
}

TEST(LineFramerTest, OverLongLinesAreEmittedTruncatedAtTheLimit) {
    LineFramer framer(8); // max line length 8
    std::vector<std::string> lines;
    std::vector<bool> truncated;
    auto sink = [&](std::string_view line, bool trunc) {
        lines.emplace_back(line);
        truncated.push_back(trunc);
    };
    // 20 non-newline bytes then a newline: emitted in 8/8/4 with the forced
    // cuts marked truncated and the natural line end not.
    framer.push(bytes("ABCDEFGHIJKLMNOPQRST\n"), sink);
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_EQ(lines[0], "ABCDEFGH");
    EXPECT_EQ(lines[1], "IJKLMNOP");
    EXPECT_EQ(lines[2], "QRST");
    EXPECT_EQ(truncated, (std::vector<bool>{true, true, false}));
    EXPECT_FALSE(framer.hasPending());
}

TEST(LineFramerTest, ExactlyMaxLengthThenNewlineIsOneUntruncatedLine) {
    LineFramer framer(4);
    std::vector<std::string> lines;
    std::vector<bool> truncated;
    framer.push(bytes("ABCD\n"), [&](std::string_view l, bool t) {
        lines.emplace_back(l);
        truncated.push_back(t);
    });
    EXPECT_EQ(lines, (std::vector<std::string>{"ABCD"}));
    EXPECT_EQ(truncated, (std::vector<bool>{false}));
}

TEST(LineFramerTest, NoInterleavedPartialLinesProperty) {
    // Every emitted line (except an explicit flush/truncation) is exactly a
    // maximal run of non-newline bytes from the input — never a fragment.
    LineFramer framer;
    std::vector<std::string> lines;
    const std::vector<std::string> chunks = {"he", "llo wor", "ld\nfoo", " bar\nba", "z\n"};
    for (const auto& c : chunks) {
        feed(framer, c, lines);
    }
    EXPECT_EQ(lines, (std::vector<std::string>{"hello world", "foo bar", "baz"}));
}
