#include <gtest/gtest.h>

#include "os_test_support.hpp"
#include "psx/os/handle.hpp"
#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"

#include <span>
#include <utility>

using psx::os::Handle;
using psx::os::Pipe;

TEST(OsHandleTest, DefaultConstructedIsInvalidAndCloseIsIdempotent) {
    Handle handle;
    EXPECT_FALSE(handle.valid());
    EXPECT_FALSE(static_cast<bool>(handle));
    handle.close();
    handle.close();
    EXPECT_FALSE(handle.valid());
}

TEST(OsHandleTest, MoveTransfersOwnership) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok()) << pipe.error().message();
    const auto before = os_test::openDescriptors();

    Handle moved(std::move(pipe.value().reader));
    EXPECT_TRUE(moved.valid());
    EXPECT_FALSE(pipe.value().reader.valid());
    EXPECT_EQ(os_test::openDescriptors(), before) << "a move must not open or close anything";

    Handle assigned;
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.valid());
    EXPECT_FALSE(moved.valid());
    EXPECT_EQ(os_test::openDescriptors(), before);

    assigned.close();
    EXPECT_EQ(os_test::openDescriptors().size(), before.size() - 1);
}

TEST(OsHandleTest, MoveAssignmentClosesTheOverwrittenHandle) {
    auto first = Pipe::create();
    auto second = Pipe::create();
    ASSERT_TRUE(first.ok() && second.ok());
    const auto before = os_test::openDescriptors();

    first.value().reader = std::move(second.value().reader);
    EXPECT_EQ(os_test::openDescriptors().size(), before.size() - 1) << "the overwritten reader must be closed";
    EXPECT_TRUE(first.value().reader.valid());
    EXPECT_FALSE(second.value().reader.valid());
}

TEST(OsHandleTest, DestructorClosesExactlyOnce) {
    const auto before = os_test::openDescriptors();
    {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok());
        EXPECT_EQ(os_test::openDescriptors().size(), before.size() + 2);
    }
    EXPECT_EQ(os_test::openDescriptors(), before);
}

TEST(OsHandleTest, DuplicateYieldsIndependentNonInheritableHandle) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    const auto before = os_test::openDescriptors();

    auto duplicate = pipe.value().writer.duplicate();
    ASSERT_TRUE(duplicate.ok()) << duplicate.error().message();
    const auto created = os_test::newDescriptors(before, os_test::openDescriptors());
    ASSERT_EQ(created.size(), 1U);
    EXPECT_TRUE(os_test::isNonInheritable(*created.begin()));

    // Closing the original must not affect the duplicate.
    pipe.value().writer.close();
    const char byte = 'x';
    EXPECT_TRUE(psx::os::write(duplicate.value(), std::span<const char>(&byte, 1)).ok());

    Handle invalid;
    EXPECT_FALSE(invalid.duplicate().ok());
    EXPECT_EQ(invalid.duplicate().error().cls, psx::ErrorClass::Closed);
}

TEST(OsHandleTest, NonBlockingToggle) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());

    ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
    char buffer[1];
    const auto empty = psx::os::read(pipe.value().reader, std::span<char>(buffer, 1));
    ASSERT_FALSE(empty.ok());
    EXPECT_EQ(empty.error().cls, psx::ErrorClass::WouldBlock);

    ASSERT_TRUE(pipe.value().reader.setNonBlocking(false).ok());
    ASSERT_TRUE(pipe.value().reader.setNonBlocking(false).ok()) << "toggling to the current state is a no-op";
    Handle invalid;
    EXPECT_EQ(invalid.setNonBlocking(true).error().cls, psx::ErrorClass::Closed);
}

TEST(OsHandleTest, AccountingCountersTrackOpenHandles) {
    const auto start = psx::os::handleStats();
    {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok());
        const auto during = psx::os::handleStats();
        EXPECT_EQ(during.open, start.open + 2);
        EXPECT_EQ(during.created, start.created + 2);
        EXPECT_EQ(during.closed, start.closed);
    }
    const auto end = psx::os::handleStats();
    EXPECT_EQ(end.open, start.open);
    EXPECT_EQ(end.created, start.created + 2);
    EXPECT_EQ(end.closed, start.closed + 2);
}

// T11 seed: handle count identical before and after 10 000 create/close cycles.
TEST(OsHandleTest, TenThousandPipeCyclesLeakNothing) {
    const auto before = os_test::openDescriptors();
    const auto stats = psx::os::handleStats();
    for (int i = 0; i < 10000; ++i) {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok()) << "iteration " << i << ": " << pipe.error().message();
    }
    EXPECT_EQ(os_test::openDescriptors(), before);
    EXPECT_EQ(psx::os::handleStats().open, stats.open);
}
