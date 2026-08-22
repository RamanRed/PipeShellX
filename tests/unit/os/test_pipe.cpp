#include <gtest/gtest.h>

#include "os_test_support.hpp"
#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"

#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using psx::ErrorClass;
using psx::os::Pipe;

namespace {

std::span<const char> bytes(std::string_view text) {
    return std::span<const char>(text.data(), text.size());
}

} // namespace

TEST(OsPipeTest, CreateYieldsTwoNonInheritableBlockingDescriptors) {
    const auto before = os_test::openDescriptors();
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok()) << pipe.error().message();
    EXPECT_TRUE(pipe.value().reader.valid());
    EXPECT_TRUE(pipe.value().writer.valid());

    const auto created = os_test::newDescriptors(before, os_test::openDescriptors());
    ASSERT_EQ(created.size(), 2U);
    for (int fd : created) {
        EXPECT_TRUE(os_test::isNonInheritable(fd)) << "fd " << fd << " would leak into children";
        EXPECT_FALSE(os_test::isNonBlocking(fd)) << "fd " << fd << " should start blocking";
    }
}

TEST(OsPipeTest, WriteThenReadRoundTrip) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());

    const auto written = psx::os::write(pipe.value().writer, bytes("hello pipe"));
    ASSERT_TRUE(written.ok()) << written.error().message();
    EXPECT_EQ(written.value(), 10U);

    char buffer[32] = {};
    const auto read = psx::os::read(pipe.value().reader, std::span<char>(buffer, sizeof(buffer)));
    ASSERT_TRUE(read.ok()) << read.error().message();
    EXPECT_EQ(std::string_view(buffer, read.value()), "hello pipe");
}

TEST(OsPipeTest, ReadReturnsZeroAtEndOfStream) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(psx::os::write(pipe.value().writer, bytes("x")).ok());
    pipe.value().writer.close();

    char buffer[8];
    auto first = psx::os::read(pipe.value().reader, std::span<char>(buffer, sizeof(buffer)));
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), 1U);
    auto eof = psx::os::read(pipe.value().reader, std::span<char>(buffer, sizeof(buffer)));
    ASSERT_TRUE(eof.ok());
    EXPECT_EQ(eof.value(), 0U) << "EOF is a successful zero-byte read, not an error";
}

TEST(OsPipeTest, WriteToClosedReaderIsBrokenPipeNotASignal) {
    ASSERT_TRUE(psx::os::ignoreBrokenPipeSignal().ok());
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    pipe.value().reader.close();

    const auto result = psx::os::write(pipe.value().writer, bytes("nobody listens"));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().cls, ErrorClass::BrokenPipe);
}

TEST(OsPipeTest, NonBlockingReadOnEmptyPipeWouldBlock) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());

    char buffer[8];
    const auto result = psx::os::read(pipe.value().reader, std::span<char>(buffer, sizeof(buffer)));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().cls, ErrorClass::WouldBlock);
}

TEST(OsPipeTest, NonBlockingWriteLargerThanCapacityIsPartial) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().writer.setNonBlocking(true).ok());

    const std::vector<char> payload(1 << 20, 'p'); // 1 MiB > any default pipe buffer
    const auto first = psx::os::write(pipe.value().writer, std::span<const char>(payload));
    ASSERT_TRUE(first.ok()) << first.error().message();
    EXPECT_GT(first.value(), 0U);
    EXPECT_LT(first.value(), payload.size());

    // The pipe is now full: the next write must not block.
    const auto full = psx::os::write(pipe.value().writer, bytes("one more"));
    ASSERT_FALSE(full.ok());
    EXPECT_EQ(full.error().cls, ErrorClass::WouldBlock);
}

TEST(OsPipeTest, BlockingWriteCompletesWhenAReaderDrains) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    const std::vector<char> payload(256 * 1024, 'q');

    std::size_t drained = 0;
    std::thread reader([&] {
        char buffer[4096];
        while (true) {
            auto chunk = psx::os::read(pipe.value().reader, std::span<char>(buffer, sizeof(buffer)));
            if (!chunk.ok() || chunk.value() == 0) {
                break;
            }
            drained += chunk.value();
        }
    });

    std::size_t sent = 0;
    while (sent < payload.size()) {
        auto part = psx::os::write(pipe.value().writer, std::span<const char>(payload).subspan(sent));
        ASSERT_TRUE(part.ok()) << part.error().message();
        sent += part.value();
    }
    pipe.value().writer.close();
    reader.join();
    EXPECT_EQ(drained, payload.size());
}

TEST(OsPipeTest, OperationsOnClosedHandlesReportClosed) {
    psx::os::Handle none;
    char buffer[1];
    EXPECT_EQ(psx::os::read(none, std::span<char>(buffer, 1)).error().cls, ErrorClass::Closed);
    EXPECT_EQ(psx::os::write(none, bytes("x")).error().cls, ErrorClass::Closed);
}

TEST(OsPipeTest, PipeIsMoveOnlyAndMovesBothEnds) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    const auto before = os_test::openDescriptors();

    Pipe moved = std::move(pipe.value());
    EXPECT_TRUE(moved.reader.valid());
    EXPECT_TRUE(moved.writer.valid());
    EXPECT_FALSE(pipe.value().reader.valid());
    EXPECT_FALSE(pipe.value().writer.valid());
    EXPECT_EQ(os_test::openDescriptors(), before);
    static_assert(!std::is_copy_constructible_v<Pipe>);
    static_assert(std::is_nothrow_move_constructible_v<Pipe>);
}
