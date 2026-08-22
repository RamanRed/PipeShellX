#include <gtest/gtest.h>

#include "psx/stream/credit_window.hpp"

using psx::stream::CreditWindow;

TEST(CreditWindowTest, StartsFullyOpenWithNothingOutstanding) {
    CreditWindow window(256 * 1024);
    EXPECT_EQ(window.window(), 256U * 1024);
    EXPECT_EQ(window.sendable(), 256U * 1024);
    EXPECT_EQ(window.outstanding(), 0U);
    EXPECT_EQ(window.updateThreshold(), 128U * 1024); // default: half the window
}

TEST(CreditWindowTest, IncomingDataReducesTheSendableAllowance) {
    CreditWindow window(1000);
    EXPECT_TRUE(window.onData(400));
    EXPECT_EQ(window.outstanding(), 400U);
    EXPECT_EQ(window.sendable(), 600U);
    EXPECT_TRUE(window.onData(600));
    EXPECT_EQ(window.sendable(), 0U);
    EXPECT_EQ(window.outstanding(), 1000U);
}

TEST(CreditWindowTest, DataBeyondTheWindowIsRejectedWithoutStateChange) {
    CreditWindow window(1000);
    ASSERT_TRUE(window.onData(700));
    EXPECT_FALSE(window.onData(400)) << "700 + 400 > 1000: a flow-control violation";
    EXPECT_EQ(window.outstanding(), 700U);
    EXPECT_EQ(window.sendable(), 300U);
    EXPECT_TRUE(window.onData(300)); // exactly fills the window
    EXPECT_EQ(window.sendable(), 0U);
}

TEST(CreditWindowTest, ConsumingBelowThresholdAdvertisesNothingButAccumulates) {
    CreditWindow window(1000); // threshold 500
    ASSERT_TRUE(window.onData(1000));
    EXPECT_EQ(window.onConsumed(200), 0U);
    EXPECT_EQ(window.onConsumed(200), 0U);
    EXPECT_EQ(window.outstanding(), 600U); // 1000 delivered, 400 consumed
    // Crossing the threshold advertises the whole accumulated amount and resets.
    EXPECT_EQ(window.onConsumed(150), 550U);
    EXPECT_EQ(window.outstanding(), 450U);
    EXPECT_EQ(window.sendable(), 550U) << "the peer may now send the freed credit";
}

TEST(CreditWindowTest, AccumulatorResetsAfterEachUpdate) {
    CreditWindow window(1000);
    ASSERT_TRUE(window.onData(1000));
    EXPECT_EQ(window.onConsumed(500), 500U); // first update
    EXPECT_EQ(window.onConsumed(300), 0U);   // below threshold again
    EXPECT_EQ(window.onConsumed(200), 500U); // second update
    EXPECT_EQ(window.outstanding(), 0U);
    EXPECT_EQ(window.sendable(), 1000U);
}

TEST(CreditWindowTest, ConsumingMoreThanOutstandingIsClamped) {
    CreditWindow window(1000);
    ASSERT_TRUE(window.onData(300));
    EXPECT_EQ(window.onConsumed(1000), 300U); // only 300 was ever delivered
    EXPECT_EQ(window.outstanding(), 0U);
    EXPECT_EQ(window.sendable(), 1000U);
}

TEST(CreditWindowTest, CustomThresholdIsHonoured) {
    CreditWindow window(1000, 100); // advertise every 100 consumed
    ASSERT_TRUE(window.onData(1000));
    EXPECT_EQ(window.onConsumed(60), 0U);
    EXPECT_EQ(window.onConsumed(60), 120U); // crossed 100
    EXPECT_EQ(window.onConsumed(100), 100U);
}

TEST(CreditWindowTest, ZeroByteDataAndConsumeAreNoOps) {
    CreditWindow window(1000);
    EXPECT_TRUE(window.onData(0));
    EXPECT_EQ(window.onConsumed(0), 0U);
    EXPECT_EQ(window.sendable(), 1000U);
}

TEST(CreditWindowTest, FullDrainCycleReturnsToTheInitialState) {
    CreditWindow window(4 * 1024 * 1024); // connection window
    std::uint32_t advertised = 0;
    std::uint32_t delivered = 0;
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(window.onData(64 * 1024));
        delivered += 64 * 1024;
    }
    EXPECT_EQ(window.sendable(), 0U);
    for (int i = 0; i < 64; ++i) {
        advertised += window.onConsumed(64 * 1024);
    }
    EXPECT_EQ(advertised, delivered) << "every delivered byte is eventually re-credited";
    EXPECT_EQ(window.outstanding(), 0U);
    EXPECT_EQ(window.sendable(), 4U * 1024 * 1024);
}
