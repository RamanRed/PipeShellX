#include "psx/transport/control_payloads.hpp"

#include <gtest/gtest.h>

#include <string>

using psx::os::ExitStatus;
using psx::transport::decodeExit;
using psx::transport::decodeWindowUpdate;
using psx::transport::encodeExit;
using psx::transport::encodeWindowUpdate;

TEST(ControlPayloadsTest, ExitRoundTripsEveryKindIncludingNegativeCodes) {
    for (const ExitStatus in : {
             ExitStatus{ExitStatus::Kind::Exited, 0},
             ExitStatus{ExitStatus::Kind::Exited, 255},
             ExitStatus{ExitStatus::Kind::Signaled, 9},
             ExitStatus{ExitStatus::Kind::Terminated, -1073741510}, // 0xC000013A as i32
         }) {
        const std::string wire = encodeExit(in);
        EXPECT_EQ(wire.size(), 5U);
        const auto out = decodeExit(wire);
        ASSERT_TRUE(out.ok()) << out.error().message();
        EXPECT_EQ(out.value().kind, in.kind);
        EXPECT_EQ(out.value().code, in.code);
    }
}

TEST(ControlPayloadsTest, ExitRejectsWrongLengthAndUnknownKind) {
    EXPECT_FALSE(decodeExit("").ok());
    EXPECT_FALSE(decodeExit(std::string(4, '\0')).ok()); // too short
    EXPECT_FALSE(decodeExit(std::string(6, '\0')).ok()); // too long
    std::string wire = encodeExit({ExitStatus::Kind::Exited, 0});
    wire[0] = 3; // kind out of range
    EXPECT_FALSE(decodeExit(wire).ok());
}

TEST(ControlPayloadsTest, WindowUpdateRoundTrips) {
    for (std::uint32_t delta : {1u, 1024u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        const std::string wire = encodeWindowUpdate(delta);
        EXPECT_EQ(wire.size(), 4U);
        const auto out = decodeWindowUpdate(wire);
        ASSERT_TRUE(out.ok()) << out.error().message();
        EXPECT_EQ(out.value(), delta);
    }
}

TEST(ControlPayloadsTest, WindowUpdateRejectsZeroAndWrongLength) {
    EXPECT_FALSE(decodeWindowUpdate(encodeWindowUpdate(0)).ok()); // zero delta is a protocol error
    EXPECT_FALSE(decodeWindowUpdate("").ok());
    EXPECT_FALSE(decodeWindowUpdate(std::string(3, '\0')).ok());
    EXPECT_FALSE(decodeWindowUpdate(std::string(5, '\0')).ok());
}
