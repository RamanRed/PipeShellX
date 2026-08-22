#include "psx/runtime/backoff.hpp"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using psx::runtime::backoffDelay;

TEST(BackoffTest, GrowsExponentiallyAtTheZeroJitterFloor) {
    // rand01 == 0 -> exactly full/2 for each attempt: 100, 200, 400, 800 ...
    EXPECT_EQ(backoffDelay(1, 200ms, 30s, 0.0), 100ms);
    EXPECT_EQ(backoffDelay(2, 200ms, 30s, 0.0), 200ms);
    EXPECT_EQ(backoffDelay(3, 200ms, 30s, 0.0), 400ms);
    EXPECT_EQ(backoffDelay(4, 200ms, 30s, 0.0), 800ms);
}

TEST(BackoffTest, FullJitterReachesTheWholeWindow) {
    // rand01 == 1 -> exactly full: 200, 400, 800 ...
    EXPECT_EQ(backoffDelay(1, 200ms, 30s, 1.0), 200ms);
    EXPECT_EQ(backoffDelay(2, 200ms, 30s, 1.0), 400ms);
    // Mid draw sits between the floor and the ceiling.
    const auto mid = backoffDelay(2, 200ms, 30s, 0.5);
    EXPECT_GE(mid, 200ms);
    EXPECT_LE(mid, 400ms);
}

TEST(BackoffTest, SaturatesAtTheCap) {
    // base*2^(attempt-1) would explode; the cap holds the window.
    EXPECT_EQ(backoffDelay(20, 200ms, 1000ms, 0.0), 500ms); // full == cap 1000 -> floor 500
    EXPECT_EQ(backoffDelay(20, 200ms, 1000ms, 1.0), 1000ms);
    // A huge attempt must not overflow into a tiny or negative delay.
    EXPECT_GE(backoffDelay(1000, 200ms, 1000ms, 0.0), 500ms);
    EXPECT_LE(backoffDelay(1000, 200ms, 1000ms, 1.0), 1000ms);
}

TEST(BackoffTest, ClampsDegenerateInputs) {
    EXPECT_EQ(backoffDelay(0, 200ms, 30s, 0.0), 100ms);  // attempt < 1 treated as 1
    EXPECT_EQ(backoffDelay(-5, 200ms, 30s, 0.0), 100ms); // negative attempt
    EXPECT_EQ(backoffDelay(1, 200ms, 30s, -1.0), 100ms); // rand below 0 clamps to floor
    EXPECT_EQ(backoffDelay(1, 200ms, 30s, 2.0), 200ms);  // rand above 1 clamps to ceiling
    EXPECT_EQ(backoffDelay(1, 0ms, 30s, 0.5), 0ms);      // zero base -> zero delay
}
