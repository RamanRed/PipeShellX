#include <gtest/gtest.h>

#include "os_test_support.hpp"
#include "psx/os/signal_source.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using psx::os::Signal;
using psx::os::SignalSource;

namespace {

bool contains(const std::vector<Signal>& signals, Signal wanted) {
    return std::find(signals.begin(), signals.end(), wanted) != signals.end();
}

} // namespace

TEST(SignalSourceTest, DeliversInterruptAsAnEventInsteadOfTerminating) {
    auto source = SignalSource::create({Signal::Interrupt, Signal::WindowResize});
    ASSERT_TRUE(source.ok()) << source.error().message();
    EXPECT_FALSE(os_test::waitReadable(source.value()->handle(), 20ms));

    ASSERT_EQ(::kill(::getpid(), SIGINT), 0);
    ASSERT_TRUE(os_test::waitReadable(source.value()->handle(), 2000ms));
    auto received = source.value()->drain();
    ASSERT_TRUE(received.ok()) << received.error().message();
    EXPECT_TRUE(contains(received.value(), Signal::Interrupt));
    EXPECT_FALSE(contains(received.value(), Signal::WindowResize));

    auto empty = source.value()->drain();
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty.value().empty());
    EXPECT_FALSE(os_test::waitReadable(source.value()->handle(), 20ms));
}

TEST(SignalSourceTest, RepeatedSignalsAreReportedAtLeastOnce) {
    auto source = SignalSource::create({Signal::WindowResize});
    ASSERT_TRUE(source.ok());
    ASSERT_EQ(::kill(::getpid(), SIGWINCH), 0);
    ASSERT_EQ(::kill(::getpid(), SIGWINCH), 0);
    ASSERT_TRUE(os_test::waitReadable(source.value()->handle(), 2000ms));
    auto received = source.value()->drain();
    ASSERT_TRUE(received.ok());
    EXPECT_TRUE(contains(received.value(), Signal::WindowResize));
}

TEST(SignalSourceTest, UnsubscribedSignalsAreNotReported) {
    auto source = SignalSource::create({Signal::Hangup});
    ASSERT_TRUE(source.ok());
    // SIGUSR1 with a harmless handler so the process survives.
    struct sigaction action{};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    struct sigaction previous{};
    ASSERT_EQ(::sigaction(SIGUSR1, &action, &previous), 0);
    ASSERT_EQ(::kill(::getpid(), SIGUSR1), 0);
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(os_test::waitReadable(source.value()->handle(), 20ms));
    auto received = source.value()->drain();
    ASSERT_TRUE(received.ok());
    EXPECT_TRUE(received.value().empty());
    ::sigaction(SIGUSR1, &previous, nullptr);

    ASSERT_EQ(::kill(::getpid(), SIGHUP), 0);
    ASSERT_TRUE(os_test::waitReadable(source.value()->handle(), 2000ms));
    auto hangup = source.value()->drain();
    ASSERT_TRUE(hangup.ok());
    EXPECT_TRUE(contains(hangup.value(), Signal::Hangup));
}

TEST(SignalSourceTest, DestructionRestoresTheDefaultDisposition) {
    const auto before = os_test::openDescriptors();
    {
        auto source = SignalSource::create({Signal::Terminate});
        ASSERT_TRUE(source.ok());
        // Either ignored (kqueue) or blocked (signalfd) — never the default action.
        struct sigaction during{};
        ASSERT_EQ(::sigaction(SIGTERM, nullptr, &during), 0);
        sigset_t blocked;
        ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, nullptr, &blocked), 0);
        EXPECT_TRUE(during.sa_handler != SIG_DFL || sigismember(&blocked, SIGTERM))
            << "SIGTERM must not terminate the process while subscribed";
    }
    struct sigaction after{};
    ASSERT_EQ(::sigaction(SIGTERM, nullptr, &after), 0);
    EXPECT_EQ(after.sa_handler, SIG_DFL);
    sigset_t mask;
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, nullptr, &mask), 0);
    EXPECT_FALSE(sigismember(&mask, SIGTERM)) << "the signal mask must be restored too";
    EXPECT_EQ(os_test::openDescriptors(), before);
}

TEST(SignalSourceTest, EmptySubscriptionIsRejected) {
    auto source = SignalSource::create({});
    ASSERT_FALSE(source.ok());
    EXPECT_EQ(source.error().cls, psx::ErrorClass::InvalidArgument);
}
