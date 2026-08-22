#include "psx/transport/open_request.hpp"

#include "psx/transport/wire.hpp"

#include <gtest/gtest.h>

#include <string>

using psx::transport::decodeOpen;
using psx::transport::encodeOpen;
using psx::transport::OpenRequest;

TEST(OpenRequestTest, RoundTripsArgvAndCwd) {
    const OpenRequest in{.argv = {"/bin/sh", "-c", "echo hi"}, .cwd = "/tmp/work"};
    const auto out = decodeOpen(encodeOpen(in));
    ASSERT_TRUE(out.ok()) << out.error().message();
    EXPECT_EQ(out.value(), in);
}

TEST(OpenRequestTest, HandlesEmptyArgvEmptyCwdAndBinaryArgs) {
    for (const OpenRequest& in : {
             OpenRequest{.argv = {}, .cwd = ""},
             OpenRequest{.argv = {""}, .cwd = ""},                                // one empty arg
             OpenRequest{.argv = {std::string("a\x00b", 3), "c"}, .cwd = "d\ne"}, // NUL + newline
         }) {
        const auto out = decodeOpen(encodeOpen(in));
        ASSERT_TRUE(out.ok()) << out.error().message();
        EXPECT_EQ(out.value(), in);
    }
}

TEST(OpenRequestTest, VersionByteIsFirst) {
    const std::string wire = encodeOpen(OpenRequest{.argv = {"x"}, .cwd = ""});
    EXPECT_EQ(static_cast<unsigned char>(wire[0]), 1u);
}

TEST(OpenRequestTest, RejectsAnUnknownVersion) {
    std::string wire = encodeOpen(OpenRequest{.argv = {"x"}, .cwd = ""});
    wire[0] = 2; // bump the version
    EXPECT_FALSE(decodeOpen(wire).ok());
}

TEST(OpenRequestTest, RejectsTruncatedInput) {
    const std::string wire = encodeOpen(OpenRequest{.argv = {"hello"}, .cwd = "/w"});
    // Every proper prefix that stops mid-field must be rejected, never over-read.
    for (std::size_t n = 0; n < wire.size(); ++n) {
        EXPECT_FALSE(decodeOpen(wire.substr(0, n)).ok()) << "prefix length " << n;
    }
    EXPECT_TRUE(decodeOpen(wire).ok()); // the full thing decodes
}

TEST(OpenRequestTest, RejectsTrailingBytes) {
    std::string wire = encodeOpen(OpenRequest{.argv = {"x"}, .cwd = ""});
    wire.push_back('!');
    EXPECT_FALSE(decodeOpen(wire).ok());
}

TEST(OpenRequestTest, RejectsAGarbageArgcWithoutHugeAllocationOrOverread) {
    // version=1, argc=0xFFFFFFFF, then no argument bytes → must fail on the first
    // truncated argument, not loop 4 billion times or over-read.
    std::string wire;
    wire.push_back(1);
    psx::transport::writeU32BE(wire, 0xFFFFFFFFu);
    EXPECT_FALSE(decodeOpen(wire).ok());
}
