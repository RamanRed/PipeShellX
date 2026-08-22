#include <gtest/gtest.h>

#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"
#include "psx/os/process.hpp"
#include "psx/runtime/reactor.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using psx::ErrorClass;
using psx::os::Interest;
using psx::os::Pipe;
using psx::os::Poller;
using psx::os::Readiness;
using psx::runtime::Reactor;

namespace {

class ReactorTest : public ::testing::TestWithParam<Poller::Backend> {
protected:
    void SetUp() override {
        Reactor::Options options;
        options.backend = GetParam();
        options.signals = {psx::os::Signal::Interrupt};
        auto created = Reactor::create(options);
        ASSERT_TRUE(created.ok()) << created.error().message();
        reactor_ = std::move(created.value());
    }

    std::unique_ptr<Reactor> reactor_;
};

std::string backendName(const ::testing::TestParamInfo<Poller::Backend>& info) {
    return info.param == Poller::Backend::Poll ? "poll" : "native";
}

void writeText(const psx::os::Handle& handle, std::string_view text) {
    ASSERT_TRUE(psx::os::write(handle, std::span<const char>(text.data(), text.size())).ok());
}

std::string drainAll(const psx::os::Handle& reader) {
    std::string out;
    char buffer[256];
    while (true) {
        auto chunk = psx::os::read(reader, std::span<char>(buffer, sizeof(buffer)));
        if (!chunk.ok() || chunk.value() == 0) {
            break;
        }
        out.append(buffer, chunk.value());
    }
    return out;
}

} // namespace

TEST_P(ReactorTest, TimersFireInDeadlineOrderAndCancelWorks) {
    std::vector<int> order;
    const auto start = std::chrono::steady_clock::now();
    reactor_->after(60ms, [&] { order.push_back(60); });
    reactor_->after(20ms, [&] { order.push_back(20); });
    const auto cancelled = reactor_->after(40ms, [&] { order.push_back(40); });
    reactor_->after(0ms, [&] { order.push_back(0); });
    EXPECT_TRUE(reactor_->cancel(cancelled));
    EXPECT_FALSE(reactor_->cancel(cancelled)) << "cancelling twice reports false";
    EXPECT_EQ(reactor_->pendingTimers(), 3U);

    reactor_->after(80ms, [&] { reactor_->stop(); });
    ASSERT_TRUE(reactor_->run().ok());
    EXPECT_EQ(order, (std::vector<int>{0, 20, 60}));
    EXPECT_GE(std::chrono::steady_clock::now() - start, 75ms);
    EXPECT_EQ(reactor_->pendingTimers(), 0U);
}

TEST_P(ReactorTest, ATimerScheduledFromACallbackFires) {
    int fired = 0;
    reactor_->after(5ms, [&] {
        ++fired;
        reactor_->after(5ms, [&] {
            ++fired;
            reactor_->stop();
        });
    });
    ASSERT_TRUE(reactor_->run().ok());
    EXPECT_EQ(fired, 2);
}

TEST_P(ReactorTest, IoHandlerReceivesReadinessAndCanStopTheLoop) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
    std::string received;
    auto token = reactor_->watch(pipe.value().reader, Interest::Readable, [&](Readiness readiness) {
        EXPECT_TRUE(psx::os::has(readiness, Readiness::Readable) || psx::os::has(readiness, Readiness::Hangup));
        received += drainAll(pipe.value().reader);
        if (received.size() >= 5) {
            reactor_->stop();
        }
    });
    ASSERT_TRUE(token.ok()) << token.error().message();
    EXPECT_EQ(reactor_->watchedHandles(), 1U);

    writeText(pipe.value().writer, "hello");
    reactor_->after(2000ms, [&] { reactor_->stop(); }); // safety net
    ASSERT_TRUE(reactor_->run().ok());
    EXPECT_EQ(received, "hello");
    ASSERT_TRUE(reactor_->unwatch(token.value()).ok());
    EXPECT_EQ(reactor_->unwatch(token.value()).error().cls, ErrorClass::NotFound);
    EXPECT_EQ(reactor_->watchedHandles(), 0U);
}

TEST_P(ReactorTest, UnwatchingInsideTheHandlerIsSafe) {
    auto a = Pipe::create();
    auto b = Pipe::create();
    ASSERT_TRUE(a.ok() && b.ok());
    ASSERT_TRUE(a.value().reader.setNonBlocking(true).ok());
    ASSERT_TRUE(b.value().reader.setNonBlocking(true).ok());
    int aCalls = 0;
    int bCalls = 0;
    psx::runtime::Token tokenB = 0;
    auto tokenA = reactor_->watch(a.value().reader, Interest::Readable, [&](Readiness) {
        ++aCalls;
        drainAll(a.value().reader);
        // Removing a sibling with a pending event in the same round must not call it.
        EXPECT_TRUE(reactor_->unwatch(tokenB).ok());
    });
    ASSERT_TRUE(tokenA.ok());
    auto tokenBResult = reactor_->watch(b.value().reader, Interest::Readable, [&](Readiness) { ++bCalls; });
    ASSERT_TRUE(tokenBResult.ok());
    tokenB = tokenBResult.value();

    writeText(a.value().writer, "x");
    writeText(b.value().writer, "y");
    ASSERT_TRUE(reactor_->runOnce(1000ms).ok());
    EXPECT_EQ(aCalls, 1);
    // Whether b ran depends on event order only if it was not removed first;
    // after a second round it must be silent for good.
    ASSERT_TRUE(reactor_->runOnce(50ms).ok());
    EXPECT_LE(bCalls, 1);
    EXPECT_EQ(reactor_->watchedHandles(), 1U);
}

TEST_P(ReactorTest, ModifyToNoneSilencesAHandle) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
    int calls = 0;
    auto token = reactor_->watch(pipe.value().reader, Interest::Readable, [&](Readiness) { ++calls; });
    ASSERT_TRUE(token.ok());
    ASSERT_TRUE(reactor_->modify(token.value(), Interest::None).ok());
    writeText(pipe.value().writer, "quiet");
    ASSERT_TRUE(reactor_->runOnce(30ms).ok());
    EXPECT_EQ(calls, 0);
    ASSERT_TRUE(reactor_->modify(token.value(), Interest::Readable).ok());
    ASSERT_TRUE(reactor_->runOnce(1000ms).ok());
    EXPECT_EQ(calls, 1);
}

TEST_P(ReactorTest, ChildExitHandlerRunsOnceAndTheOwnerReaps) {
    psx::os::SpawnSpec spec;
    spec.program = "/bin/sh";
    spec.argv = {"sh", "-c", "exit 3"};
    auto child = psx::os::Process::spawn(spec);
    ASSERT_TRUE(child.ok());
    int calls = 0;
    ASSERT_TRUE(reactor_
                    ->watchChild(child.value().id(),
                                 [&](psx::os::ProcessId pid) {
                                     ++calls;
                                     EXPECT_EQ(pid, child.value().id());
                                     auto status = child.value().tryWait();
                                     ASSERT_TRUE(status.ok());
                                     ASSERT_TRUE(status.value().has_value());
                                     EXPECT_EQ(status.value()->code, 3);
                                     reactor_->stop();
                                 })
                    .ok());
    EXPECT_EQ(reactor_->watchChild(child.value().id(), [](psx::os::ProcessId) {}).error().cls,
              ErrorClass::InvalidArgument);
    reactor_->after(3000ms, [&] { reactor_->stop(); });
    ASSERT_TRUE(reactor_->run().ok());
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(child.value().running());
    ASSERT_TRUE(reactor_->runOnce(20ms).ok());
    EXPECT_EQ(calls, 1);
}

TEST_P(ReactorTest, SignalHandlerReceivesSubscribedSignals) {
    std::vector<psx::os::Signal> received;
    ASSERT_TRUE(reactor_
                    ->onSignal([&](psx::os::Signal signal) {
                        received.push_back(signal);
                        reactor_->stop();
                    })
                    .ok());
    reactor_->after(10ms, [] { ASSERT_EQ(::kill(::getpid(), SIGINT), 0); });
    reactor_->after(3000ms, [&] { reactor_->stop(); });
    ASSERT_TRUE(reactor_->run().ok());
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received[0], psx::os::Signal::Interrupt);
}

TEST_P(ReactorTest, StopFromAnotherThreadWakesTheLoop) {
    const auto start = std::chrono::steady_clock::now();
    std::thread stopper([&] {
        std::this_thread::sleep_for(50ms);
        reactor_->stop(); // thread-safe: stop() wakes the loop
    });
    ASSERT_TRUE(reactor_->run().ok());
    stopper.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 40ms);
    EXPECT_LT(elapsed, 2000ms);
}

TEST_P(ReactorTest, RunOnceHonoursItsTimeout) {
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(reactor_->runOnce(40ms).ok());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 35ms);
}

TEST_P(ReactorTest, ManyReadyHandlesAreEachDispatchedOnce) {
    std::vector<Pipe> pipes;
    std::vector<int> calls(100, 0);
    for (int i = 0; i < 100; ++i) {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok());
        ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
        pipes.push_back(std::move(pipe.value()));
    }
    for (int i = 0; i < 100; ++i) {
        auto token = reactor_->watch(pipes[i].reader, Interest::Readable, [&, i](Readiness) {
            ++calls[i];
            drainAll(pipes[i].reader);
        });
        ASSERT_TRUE(token.ok());
    }
    for (auto& pipe : pipes) {
        writeText(pipe.writer, "z");
    }
    for (int round = 0; round < 5; ++round) {
        ASSERT_TRUE(reactor_->runOnce(200ms).ok());
    }
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(calls[i], 1) << "handle " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         ReactorTest,
                         ::testing::Values(Poller::Backend::Poll, Poller::Backend::Auto),
                         backendName);

TEST(ReactorOptionsTest, SignalsAreOptional) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok()) << reactor.error().message();
    EXPECT_EQ(reactor.value()->onSignal([](psx::os::Signal) {}).error().cls, ErrorClass::Unsupported);
    EXPECT_NE(reactor.value()->backend(), Poller::Backend::Auto) << "the resolved backend is reported";
}
