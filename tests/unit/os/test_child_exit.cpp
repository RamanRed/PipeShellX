#include <gtest/gtest.h>

#include "os_test_support.hpp"
#include "psx/os/child_exit.hpp"
#include "psx/os/process.hpp"

#include <algorithm>
#include <chrono>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using psx::ErrorClass;
using psx::os::ChildExitMode;
using psx::os::ChildExitSource;
using psx::os::Process;
using psx::os::ProcessId;
using psx::os::SpawnSpec;

namespace {

std::vector<ChildExitMode> availableModes() {
    std::vector<ChildExitMode> modes;
    for (ChildExitMode mode : {ChildExitMode::Native, ChildExitMode::SignalDriven}) {
        if (ChildExitSource::available(mode)) {
            modes.push_back(mode);
        }
    }
    return modes;
}

std::string modeName(const ::testing::TestParamInfo<ChildExitMode>& info) {
    return info.param == ChildExitMode::Native ? "native" : "signal_driven";
}

Process spawnShell(const std::string& script) {
    SpawnSpec spec;
    spec.program = "/bin/sh";
    spec.argv = {"sh", "-c", script};
    auto process = Process::spawn(spec);
    EXPECT_TRUE(process.ok()) << process.error().message();
    return std::move(process.value());
}

class ChildExitTest : public ::testing::TestWithParam<ChildExitMode> {
protected:
    void SetUp() override {
        auto created = ChildExitSource::create(GetParam());
        ASSERT_TRUE(created.ok()) << created.error().message();
        source_ = std::move(created.value());
        EXPECT_EQ(source_->mode(), GetParam());
    }

    // Waits (with poll(2), independently of our Poller) for the source's handle.
    bool readableWithin(std::chrono::milliseconds timeout) { return os_test::waitReadable(source_->handle(), timeout); }

    std::unique_ptr<ChildExitSource> source_;
};

} // namespace

TEST_P(ChildExitTest, ReportsAnExitedChildExactlyOnce) {
    auto child = spawnShell("exit 4");
    ASSERT_TRUE(source_->watch(child.id()).ok());
    EXPECT_EQ(source_->size(), 1U);

    ASSERT_TRUE(readableWithin(2000ms));
    auto exited = source_->drain();
    ASSERT_TRUE(exited.ok()) << exited.error().message();
    EXPECT_EQ(exited.value(), std::vector<ProcessId>{child.id()});
    EXPECT_EQ(source_->size(), 0U) << "a reported child is no longer watched";

    // The owner reaps; the source never does.
    auto status = child.tryWait();
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(status.value().has_value());
    EXPECT_EQ(status.value()->code, 4);

    auto again = source_->drain();
    ASSERT_TRUE(again.ok());
    EXPECT_TRUE(again.value().empty());
    EXPECT_FALSE(readableWithin(50ms));
}

TEST_P(ChildExitTest, OnlyTheChildThatExitedIsReported) {
    auto slow = spawnShell("sleep 0.4");
    auto fast = spawnShell("exit 0");
    ASSERT_TRUE(source_->watch(slow.id()).ok());
    ASSERT_TRUE(source_->watch(fast.id()).ok());

    ASSERT_TRUE(readableWithin(2000ms));
    auto first = source_->drain();
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), std::vector<ProcessId>{fast.id()});
    ASSERT_TRUE(fast.wait().ok());

    ASSERT_TRUE(readableWithin(3000ms));
    auto second = source_->drain();
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value(), std::vector<ProcessId>{slow.id()});
    ASSERT_TRUE(slow.wait().ok());
}

TEST_P(ChildExitTest, AChildThatExitedBeforeBeingWatchedIsStillReported) {
    auto child = spawnShell("exit 0");
    std::this_thread::sleep_for(150ms); // it is a zombie by now
    ASSERT_TRUE(source_->watch(child.id()).ok());
    ASSERT_TRUE(readableWithin(2000ms));
    auto exited = source_->drain();
    ASSERT_TRUE(exited.ok());
    EXPECT_EQ(exited.value(), std::vector<ProcessId>{child.id()});
    ASSERT_TRUE(child.wait().ok());
}

TEST_P(ChildExitTest, UnwatchedChildrenAreNotReported) {
    auto child = spawnShell("exit 0");
    ASSERT_TRUE(source_->watch(child.id()).ok());
    ASSERT_TRUE(source_->unwatch(child.id()).ok());
    EXPECT_EQ(source_->size(), 0U);
    ASSERT_TRUE(child.wait().ok());
    std::this_thread::sleep_for(50ms);
    auto exited = source_->drain();
    ASSERT_TRUE(exited.ok());
    EXPECT_TRUE(exited.value().empty());
    EXPECT_EQ(source_->unwatch(child.id()).error().cls, ErrorClass::NotFound);
}

TEST_P(ChildExitTest, RejectsUnknownAndDuplicatePids) {
    EXPECT_EQ(source_->watch(1 << 29).error().cls, ErrorClass::NoSuchProcess);
    auto child = spawnShell("sleep 0.2");
    ASSERT_TRUE(source_->watch(child.id()).ok());
    EXPECT_EQ(source_->watch(child.id()).error().cls, ErrorClass::InvalidArgument);
    ASSERT_TRUE(child.wait().ok());
}

TEST_P(ChildExitTest, ManyChildrenAreAllReportedAndNothingLeaks) {
    const auto before = os_test::openDescriptors();
    std::vector<Process> children;
    std::vector<ProcessId> expected;
    for (int i = 0; i < 40; ++i) {
        children.push_back(spawnShell("exit 0"));
        ASSERT_TRUE(source_->watch(children.back().id()).ok());
        expected.push_back(children.back().id());
    }
    std::vector<ProcessId> reported;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (reported.size() < expected.size() && std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE(readableWithin(2000ms));
        auto batch = source_->drain();
        ASSERT_TRUE(batch.ok());
        reported.insert(reported.end(), batch.value().begin(), batch.value().end());
    }
    std::sort(reported.begin(), reported.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(reported, expected);
    for (auto& child : children) {
        ASSERT_TRUE(child.wait().ok());
    }
    children.clear();
    EXPECT_EQ(source_->size(), 0U);
    EXPECT_EQ(os_test::openDescriptors(), before);
}

INSTANTIATE_TEST_SUITE_P(Modes, ChildExitTest, ::testing::ValuesIn(availableModes()), modeName);

TEST(ChildExitFactoryTest, AutoPicksTheNativeMechanismWhenPresent) {
    auto automatic = ChildExitSource::create();
    ASSERT_TRUE(automatic.ok()) << automatic.error().message();
    if (ChildExitSource::available(ChildExitMode::Native)) {
        EXPECT_EQ(automatic.value()->mode(), ChildExitMode::Native);
    } else {
        EXPECT_EQ(automatic.value()->mode(), ChildExitMode::SignalDriven);
    }
    EXPECT_TRUE(ChildExitSource::available(ChildExitMode::SignalDriven));
}
