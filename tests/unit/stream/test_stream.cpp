#include <gtest/gtest.h>

#include "psx/stream/stream.hpp"

#include <span>
#include <string>
#include <string_view>

using psx::stream::OverflowPolicy;
using psx::stream::Stream;
using psx::stream::StreamState;

namespace {

std::span<const char> bytes(std::string_view text) {
    return std::span<const char>(text.data(), text.size());
}

std::string readAll(Stream& stream) {
    std::string out;
    char chunk[32];
    std::size_t n = 0;
    while ((n = stream.read(std::span<char>(chunk, sizeof(chunk)))) > 0) {
        out.append(chunk, n);
    }
    return out;
}

} // namespace

TEST(StreamTest, StartsOpenAndEmpty) {
    Stream stream(64);
    EXPECT_EQ(stream.state(), StreamState::Open);
    EXPECT_FALSE(stream.readable());
    EXPECT_TRUE(stream.writable());
    EXPECT_FALSE(stream.finished());
    EXPECT_FALSE(stream.atEnd());
}

TEST(StreamTest, WriteThenReadFlowsThroughTheBuffer) {
    Stream stream(64);
    EXPECT_EQ(stream.write(bytes("hello")), 5U);
    EXPECT_TRUE(stream.readable());
    EXPECT_EQ(readAll(stream), "hello");
    EXPECT_FALSE(stream.readable());
    EXPECT_EQ(stream.state(), StreamState::Open) << "still open: more may arrive";
}

TEST(StreamTest, RemoteEofThenDrainReachesClosed) {
    Stream stream(64);
    stream.write(bytes("tail"));
    stream.closeRemote();
    EXPECT_EQ(stream.state(), StreamState::HalfClosedRemote);
    EXPECT_FALSE(stream.atEnd()) << "buffered bytes remain";
    EXPECT_FALSE(stream.writable()) << "no more writes accepted after EOF";
    EXPECT_EQ(stream.write(bytes("x")), 0U);

    EXPECT_EQ(readAll(stream), "tail");
    EXPECT_TRUE(stream.atEnd());
    EXPECT_EQ(stream.state(), StreamState::Closed);
    EXPECT_TRUE(stream.finished());
}

TEST(StreamTest, RemoteEofOnAnEmptyStreamClosesImmediatelyOnNextRead) {
    Stream stream(64);
    stream.closeRemote();
    EXPECT_EQ(stream.state(), StreamState::HalfClosedRemote);
    char c[4];
    EXPECT_EQ(stream.read(std::span<char>(c, 4)), 0U);
    EXPECT_EQ(stream.state(), StreamState::Closed);
    EXPECT_TRUE(stream.atEnd());
}

TEST(StreamTest, CloseLocalDiscardsTheBufferAndCloses) {
    Stream stream(64);
    stream.write(bytes("unwanted"));
    stream.closeLocal(); // the sink gave up (cancel)
    EXPECT_EQ(stream.state(), StreamState::HalfClosedLocal);
    EXPECT_FALSE(stream.writable());
    // A later remote EOF completes the close.
    stream.closeRemote();
    EXPECT_EQ(stream.state(), StreamState::Closed);
}

TEST(StreamTest, RemoteThenLocalClosePathAlsoReachesClosed) {
    Stream stream(64);
    stream.closeRemote(); // HalfClosedRemote
    stream.closeLocal();  // sink done before draining
    EXPECT_EQ(stream.state(), StreamState::Closed);
}

TEST(StreamTest, BackpressureViaWritableWhenTheBufferIsFull) {
    Stream stream(4, OverflowPolicy::Block);
    EXPECT_EQ(stream.write(bytes("ABCD")), 4U);
    EXPECT_TRUE(stream.full());
    EXPECT_FALSE(stream.writable()) << "full buffer: the reactor should deregister read interest";
    EXPECT_EQ(stream.write(bytes("E")), 0U);
    char c[2];
    stream.read(std::span<char>(c, 2)); // sink drains → room again
    EXPECT_TRUE(stream.writable());
}

TEST(StreamTest, DropPolicyKeepsFlowingAndCountsDrops) {
    Stream stream(4, OverflowPolicy::DropOldest);
    stream.write(bytes("ABCDEF")); // keeps CDEF, drops AB
    EXPECT_TRUE(stream.writable()) << "a drop-policy stream never blocks its producer";
    EXPECT_EQ(stream.droppedBytes(), 2U);
    stream.closeRemote();
    EXPECT_EQ(readAll(stream), "CDEF");
    EXPECT_EQ(stream.state(), StreamState::Closed);
}

TEST(StreamTest, FailIsTerminalAndRejectsFurtherIo) {
    Stream stream(64);
    stream.write(bytes("partial"));
    stream.fail(psx::Error{psx::ErrorClass::BrokenPipe, 32, "read"});
    EXPECT_EQ(stream.state(), StreamState::Error);
    EXPECT_TRUE(stream.finished());
    ASSERT_TRUE(stream.error().has_value());
    EXPECT_EQ(stream.error()->cls, psx::ErrorClass::BrokenPipe);
    EXPECT_EQ(stream.write(bytes("more")), 0U);
    char c[8];
    EXPECT_EQ(stream.read(std::span<char>(c, 8)), 0U) << "a failed stream yields no data";
    EXPECT_FALSE(stream.writable());
}

TEST(StreamTest, FailAfterCloseDoesNotOverrideTheClosedState) {
    Stream stream(64);
    stream.closeRemote();
    char c[4];
    stream.read(std::span<char>(c, 4)); // -> Closed
    ASSERT_EQ(stream.state(), StreamState::Closed);
    stream.fail(psx::Error{psx::ErrorClass::Other, 0, "late"});
    EXPECT_EQ(stream.state(), StreamState::Closed) << "a clean close is not downgraded to error";
    EXPECT_FALSE(stream.error().has_value());
}
