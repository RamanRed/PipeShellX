#include "psx/transport/frame_codec.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

using psx::transport::encodeFrame;
using psx::transport::Frame;
using psx::transport::FrameDecoder;
using psx::transport::FrameType;
using psx::transport::kFlagEndStream;
using psx::transport::kFrameHeaderSize;

namespace {

std::span<const char> bytes(const std::string& s) {
    return std::span<const char>(s.data(), s.size());
}

std::vector<Frame> decodeAll(FrameDecoder& decoder, const std::string& wire) {
    std::vector<Frame> out;
    const auto r = decoder.push(bytes(wire), [&](Frame&& f) { out.push_back(std::move(f)); });
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().message());
    return out;
}

Frame sample(FrameType t, std::uint32_t id, std::string payload, std::uint8_t flags = 0) {
    Frame f;
    f.type = t;
    f.streamId = id;
    f.flags = flags;
    f.payload = std::move(payload);
    return f;
}

} // namespace

TEST(FrameCodecTest, EncodeProducesHeaderThenPayload) {
    const std::string wire = encodeFrame(sample(FrameType::Data, 0x01020304, "hi", kFlagEndStream));
    ASSERT_EQ(wire.size(), kFrameHeaderSize + 2);
    EXPECT_EQ(static_cast<unsigned char>(wire[0]), static_cast<unsigned char>(FrameType::Data));
    EXPECT_EQ(static_cast<unsigned char>(wire[1]), kFlagEndStream);
    // streamId big-endian
    EXPECT_EQ(static_cast<unsigned char>(wire[2]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(wire[3]), 0x02);
    EXPECT_EQ(static_cast<unsigned char>(wire[4]), 0x03);
    EXPECT_EQ(static_cast<unsigned char>(wire[5]), 0x04);
    // length big-endian == 2
    EXPECT_EQ(static_cast<unsigned char>(wire[8]), 0x00);
    EXPECT_EQ(static_cast<unsigned char>(wire[9]), 0x02);
    EXPECT_EQ(wire.substr(kFrameHeaderSize), "hi");
}

TEST(FrameCodecTest, RoundTripsEveryFieldIncludingEmptyAndBinaryPayloads) {
    const std::vector<Frame> frames{
        sample(FrameType::Open, 1, "job-spec"),
        sample(FrameType::Data, 2, std::string("\x00\x01\xFF\x00zed", 7)), // NUL + high bytes
        sample(FrameType::WindowUpdate, 3, ""),                            // empty payload
        sample(FrameType::Exit, 0xFFFFFFFF, "0", kFlagEndStream),
        sample(FrameType::Ping, 0, ""),
    };
    std::string wire;
    for (const auto& f : frames) {
        wire += encodeFrame(f);
    }
    FrameDecoder decoder;
    const auto got = decodeAll(decoder, wire);
    EXPECT_EQ(got, frames);
    EXPECT_FALSE(decoder.hasPartialFrame());
}

TEST(FrameCodecTest, DecodesRegardlessOfChunkBoundaries) {
    std::string wire;
    for (int i = 0; i < 6; ++i) {
        wire += encodeFrame(sample(FrameType::Data, static_cast<std::uint32_t>(i), "payload-" + std::to_string(i)));
    }
    // Feed one byte at a time — the worst-case fragmentation.
    FrameDecoder decoder;
    std::vector<Frame> got;
    for (char c : wire) {
        const std::string one(1, c);
        ASSERT_TRUE(decoder.push(bytes(one), [&](Frame&& f) { got.push_back(std::move(f)); }).ok());
    }
    ASSERT_EQ(got.size(), 6U);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(got[i].streamId, static_cast<std::uint32_t>(i));
        EXPECT_EQ(got[i].payload, "payload-" + std::to_string(i));
    }
    EXPECT_FALSE(decoder.hasPartialFrame());
}

TEST(FrameCodecTest, BuffersAPartialFrameUntilComplete) {
    const std::string wire = encodeFrame(sample(FrameType::Data, 7, "abcdef"));
    FrameDecoder decoder;
    std::vector<Frame> got;
    // Feed all but the last two payload bytes.
    ASSERT_TRUE(
        decoder.push(bytes(wire.substr(0, wire.size() - 2)), [&](Frame&& f) { got.push_back(std::move(f)); }).ok());
    EXPECT_TRUE(got.empty());
    EXPECT_TRUE(decoder.hasPartialFrame());
    // Feed the rest.
    ASSERT_TRUE(
        decoder.push(bytes(wire.substr(wire.size() - 2)), [&](Frame&& f) { got.push_back(std::move(f)); }).ok());
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].payload, "abcdef");
    EXPECT_FALSE(decoder.hasPartialFrame());
}

TEST(FrameCodecTest, RejectsAnOversizedPayloadAndPoisons) {
    FrameDecoder decoder(/*maxPayload=*/16); // tiny cap
    // Hand-craft a header claiming a 1000-byte payload.
    Frame big = sample(FrameType::Data, 1, std::string(1000, 'x'));
    const std::string wire = encodeFrame(big);
    auto r = decoder.push(bytes(wire), [](Frame&&) {});
    EXPECT_FALSE(r.ok());
    // Poisoned: any further push errors, even a valid small frame.
    const std::string ok = encodeFrame(sample(FrameType::Ping, 0, ""));
    EXPECT_FALSE(decoder.push(bytes(ok), [](Frame&&) {}).ok());
}

TEST(FrameCodecTest, PayloadAtExactlyTheCapIsAccepted) {
    FrameDecoder decoder(/*maxPayload=*/8);
    const std::string wire = encodeFrame(sample(FrameType::Data, 1, "12345678")); // exactly 8
    std::vector<Frame> got;
    ASSERT_TRUE(decoder.push(bytes(wire), [&](Frame&& f) { got.push_back(std::move(f)); }).ok());
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].payload, "12345678");
}
