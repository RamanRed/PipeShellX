#include <gtest/gtest.h>

#include "psx/stream/line_framer.hpp"

#include <algorithm>
#include <random>
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

namespace {

// A deterministic corpus of newline-terminated text, varied in line length
// (including empty lines) so chunk splits land in many positions.
std::string makeStream(std::mt19937& rng, int lineCount) {
    std::uniform_int_distribution<int> lineLen(0, 40);
    std::uniform_int_distribution<int> ch('a', 'z');
    std::string out;
    for (int i = 0; i < lineCount; ++i) {
        const int n = lineLen(rng);
        for (int j = 0; j < n; ++j) {
            out.push_back(static_cast<char>(ch(rng)));
        }
        out.push_back('\n');
    }
    return out;
}

// Splits `data` into random-sized chunks (1..maxChunk bytes).
std::vector<std::string> randomChunks(std::mt19937& rng, const std::string& data, std::size_t maxChunk) {
    std::vector<std::string> chunks;
    std::uniform_int_distribution<std::size_t> size(1, maxChunk);
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t n = std::min(size(rng), data.size() - pos);
        chunks.push_back(data.substr(pos, n));
        pos += n;
    }
    return chunks;
}

std::vector<std::string> splitLines(const std::string& data) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : data) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    return lines; // data always ends in '\n', so no trailing partial
}

} // namespace

// Property: the framed output is invariant under chunk boundaries. For many
// random streams cut at many random boundaries, the emitted lines equal exactly
// the newline-delimited lines of the input — no fragment, no merge, no loss.
TEST(LineFramerTest, ChunkBoundariesDoNotChangeTheFramedLines) {
    std::mt19937 rng(0xC0FFEE);
    for (int trial = 0; trial < 500; ++trial) {
        std::uniform_int_distribution<int> lineCount(0, 30);
        const std::string stream = makeStream(rng, lineCount(rng));
        const std::vector<std::string> expected = splitLines(stream);

        LineFramer framer;
        std::vector<std::string> got;
        for (const auto& chunk : randomChunks(rng, stream, 7)) {
            feed(framer, chunk, got);
        }
        framer.flush([&](std::string_view line, bool truncated) {
            got.emplace_back(line);
            EXPECT_FALSE(truncated);
        });
        ASSERT_EQ(got, expected) << "trial " << trial;
    }
}

// Property: two producers multiplexed through separate framers into one shared
// sink never interleave a partial line. Each recorded emission is a complete
// line tagged with its producer; per producer the lines arrive in order and
// equal that producer's input exactly, regardless of how the two byte streams
// are interleaved chunk-by-chunk.
TEST(LineFramerTest, TwoProducersNeverInterleaveAPartialLine) {
    std::mt19937 rng(0x5EED);
    for (int trial = 0; trial < 300; ++trial) {
        std::uniform_int_distribution<int> lineCount(0, 20);
        const std::string a = makeStream(rng, lineCount(rng));
        const std::string b = makeStream(rng, lineCount(rng));
        const auto expectedA = splitLines(a);
        const auto expectedB = splitLines(b);

        auto chunksA = randomChunks(rng, a, 5);
        auto chunksB = randomChunks(rng, b, 5);
        LineFramer framerA;
        LineFramer framerB;
        std::vector<std::string> gotA;
        std::vector<std::string> gotB;

        // Interleave the two chunk queues in a random order.
        std::size_t ia = 0;
        std::size_t ib = 0;
        while (ia < chunksA.size() || ib < chunksB.size()) {
            const bool takeA = ib >= chunksB.size() || (ia < chunksA.size() && (rng() & 1));
            if (takeA) {
                feed(framerA, chunksA[ia++], gotA);
            } else {
                feed(framerB, chunksB[ib++], gotB);
            }
        }
        framerA.flush([&](std::string_view line, bool) { gotA.emplace_back(line); });
        framerB.flush([&](std::string_view line, bool) { gotB.emplace_back(line); });

        ASSERT_EQ(gotA, expectedA) << "producer A, trial " << trial;
        ASSERT_EQ(gotB, expectedB) << "producer B, trial " << trial;
    }
}
