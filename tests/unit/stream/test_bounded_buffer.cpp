#include <gtest/gtest.h>

#include "psx/stream/bounded_buffer.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using psx::stream::BoundedBuffer;
using psx::stream::OverflowPolicy;

namespace {

std::span<const char> bytes(std::string_view text) {
    return std::span<const char>(text.data(), text.size());
}

std::string consumeAll(BoundedBuffer& buffer) {
    std::string out;
    char chunk[64];
    std::size_t n = 0;
    while ((n = buffer.consume(std::span<char>(chunk, sizeof(chunk)))) > 0) {
        out.append(chunk, n);
    }
    return out;
}

} // namespace

TEST(BoundedBufferTest, StartsEmptyWithTheGivenCapacityAndPolicy) {
    BoundedBuffer buffer(256, OverflowPolicy::Block);
    EXPECT_EQ(buffer.capacity(), 256U);
    EXPECT_EQ(buffer.size(), 0U);
    EXPECT_TRUE(buffer.empty());
    EXPECT_FALSE(buffer.full());
    EXPECT_EQ(buffer.available(), 256U);
    EXPECT_EQ(buffer.droppedBytes(), 0U);
    EXPECT_EQ(buffer.policy(), OverflowPolicy::Block);
}

TEST(BoundedBufferTest, AppendThenConsumeIsFifo) {
    BoundedBuffer buffer(16);
    EXPECT_EQ(buffer.append(bytes("hello ")), 6U);
    EXPECT_EQ(buffer.append(bytes("world")), 5U);
    EXPECT_EQ(buffer.size(), 11U);
    EXPECT_EQ(consumeAll(buffer), "hello world");
    EXPECT_TRUE(buffer.empty());
}

TEST(BoundedBufferTest, WrapsAroundTheRingCorrectly) {
    BoundedBuffer buffer(8);
    ASSERT_EQ(buffer.append(bytes("ABCDEF")), 6U);
    char tmp[4];
    ASSERT_EQ(buffer.consume(std::span<char>(tmp, 4)), 4U); // drains ABCD, head moves
    EXPECT_EQ(std::string_view(tmp, 4), "ABCD");
    ASSERT_EQ(buffer.append(bytes("GHIJ")), 4U); // wraps past the end
    EXPECT_EQ(buffer.size(), 6U);
    EXPECT_EQ(consumeAll(buffer), "EFGHIJ");
}

TEST(BoundedBufferTest, PeekAndDropExposeTheContiguousFront) {
    BoundedBuffer buffer(8);
    buffer.append(bytes("ABCDEF"));
    auto front = buffer.peek();
    EXPECT_EQ(std::string_view(front.data(), front.size()), "ABCDEF");
    buffer.drop(2); // head now at 2 (CDEF buffered, tail at 6)
    EXPECT_EQ(buffer.size(), 4U);
    // "GHI" fills slots 6,7 then wraps "I" to slot 0; peek returns only the
    // contiguous run up to the ring end, not the wrapped tail.
    buffer.append(bytes("GHI"));
    auto wrapped = buffer.peek();
    EXPECT_EQ(std::string_view(wrapped.data(), wrapped.size()), "CDEFGH");
    EXPECT_EQ(consumeAll(buffer), "CDEFGHI");
}

TEST(BlockPolicyTest, AcceptsOnlyWhatFitsAndNeverDrops) {
    BoundedBuffer buffer(8, OverflowPolicy::Block);
    EXPECT_EQ(buffer.append(bytes("ABCDEFGHIJ")), 8U); // only 8 fit
    EXPECT_TRUE(buffer.full());
    EXPECT_EQ(buffer.available(), 0U);
    EXPECT_EQ(buffer.append(bytes("X")), 0U); // full: nothing accepted
    EXPECT_EQ(buffer.droppedBytes(), 0U);
    EXPECT_EQ(consumeAll(buffer), "ABCDEFGH");
}

TEST(DropNewestPolicyTest, KeepsOldestAndCountsTheRejectedTail) {
    BoundedBuffer buffer(8, OverflowPolicy::DropNewest);
    EXPECT_EQ(buffer.append(bytes("ABCDEF")), 6U);
    // 2 fit, 3 dropped; the whole input is "consumed" from the caller's view.
    EXPECT_EQ(buffer.append(bytes("GHIJK")), 5U);
    EXPECT_TRUE(buffer.full());
    EXPECT_EQ(buffer.droppedBytes(), 3U);
    EXPECT_EQ(consumeAll(buffer), "ABCDEFGH");
}

TEST(DropOldestPolicyTest, EvictsOldestToMakeRoomForTheNewest) {
    BoundedBuffer buffer(8, OverflowPolicy::DropOldest);
    EXPECT_EQ(buffer.append(bytes("ABCDEF")), 6U);
    EXPECT_EQ(buffer.append(bytes("GHIJ")), 4U); // evicts AB, keeps last 8
    EXPECT_TRUE(buffer.full());
    EXPECT_EQ(buffer.droppedBytes(), 2U);
    EXPECT_EQ(consumeAll(buffer), "CDEFGHIJ");
}

TEST(DropOldestPolicyTest, InputLargerThanCapacityKeepsOnlyItsTail) {
    BoundedBuffer buffer(4, OverflowPolicy::DropOldest);
    buffer.append(bytes("AB"));
    EXPECT_EQ(buffer.append(bytes("CDEFGHIJ")), 8U); // keep last 4: GHIJ
    EXPECT_EQ(buffer.size(), 4U);
    EXPECT_EQ(buffer.droppedBytes(), 6U); // 2 old + 4 of the input
    EXPECT_EQ(consumeAll(buffer), "GHIJ");
}

TEST(BoundedBufferTest, ClearResetsContentsButKeepsTheDropTally) {
    BoundedBuffer buffer(4, OverflowPolicy::DropOldest);
    buffer.append(bytes("ABCDEF")); // drops 2
    buffer.clear();
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.droppedBytes(), 2U);
    EXPECT_EQ(buffer.append(bytes("XY")), 2U);
    EXPECT_EQ(consumeAll(buffer), "XY");
}

TEST(BoundedBufferTest, LargeRoundTripAcrossManyWraps) {
    BoundedBuffer buffer(64);
    std::string expected;
    std::string drained;
    for (int i = 0; i < 1000; ++i) {
        const std::string piece = "chunk" + std::to_string(i) + ";";
        std::size_t offset = 0;
        while (offset < piece.size()) {
            offset += buffer.append(bytes(std::string_view(piece).substr(offset)));
            char tmp[32];
            std::size_t n = 0;
            while ((n = buffer.consume(std::span<char>(tmp, sizeof(tmp)))) > 0) {
                drained.append(tmp, n);
            }
        }
        expected += piece;
    }
    drained += consumeAll(buffer);
    EXPECT_EQ(drained, expected);
    EXPECT_EQ(buffer.droppedBytes(), 0U);
}
