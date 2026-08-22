#include "psx/stream/spool_buffer.hpp"

#include <gtest/gtest.h>

#include <string>

using psx::stream::SpoolBuffer;

TEST(SpoolBufferTest, ReadsBackWhatWasAppendedInOrder) {
    SpoolBuffer spool;
    EXPECT_TRUE(spool.empty());
    ASSERT_TRUE(spool.append("hello "));
    ASSERT_TRUE(spool.append("spooled "));
    ASSERT_TRUE(spool.append("world"));
    EXPECT_EQ(spool.size(), 19U);
    EXPECT_FALSE(spool.empty());
    EXPECT_EQ(spool.readAll(), "hello spooled world");
}

TEST(SpoolBufferTest, ReadAllIsRepeatableAndEmptyWhenUnused) {
    SpoolBuffer spool;
    EXPECT_EQ(spool.readAll(), "");
    ASSERT_TRUE(spool.append("abc"));
    EXPECT_EQ(spool.readAll(), "abc");
    EXPECT_EQ(spool.readAll(), "abc") << "readAll must not consume the spill";
}

TEST(SpoolBufferTest, HandlesLargeAndBinaryData) {
    SpoolBuffer spool;
    std::string big(1'000'000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>(i % 256); // includes NUL and high bytes
    }
    ASSERT_TRUE(spool.append(big));
    const std::string back = spool.readAll();
    ASSERT_EQ(back.size(), big.size());
    EXPECT_EQ(back, big);
}

TEST(SpoolBufferTest, ResetDiscardsTheSpill) {
    SpoolBuffer spool;
    ASSERT_TRUE(spool.append("gone"));
    spool.reset();
    EXPECT_TRUE(spool.empty());
    EXPECT_EQ(spool.size(), 0U);
    EXPECT_EQ(spool.readAll(), "");
    ASSERT_TRUE(spool.append("fresh")); // reusable after reset
    EXPECT_EQ(spool.readAll(), "fresh");
}

TEST(SpoolBufferTest, AppendAfterReadAllContinuesCorrectly) {
    // readAll() repositions the stream to the end so a later append() is defined
    // (C11 forbids a write straight after a read on an update stream).
    SpoolBuffer spool;
    ASSERT_TRUE(spool.append("first"));
    EXPECT_EQ(spool.readAll(), "first");
    ASSERT_TRUE(spool.append("second"));
    EXPECT_EQ(spool.readAll(), "firstsecond");
    EXPECT_EQ(spool.size(), 11U);
}

TEST(SpoolBufferTest, MoveTransfersOwnership) {
    SpoolBuffer a;
    ASSERT_TRUE(a.append("moved"));
    SpoolBuffer b(std::move(a));
    EXPECT_EQ(b.readAll(), "moved");
}
