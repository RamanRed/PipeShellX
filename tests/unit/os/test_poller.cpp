#include <gtest/gtest.h>

#include "os_test_support.hpp"
#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"
#include "psx/os/poller.hpp"

#include <array>
#include <chrono>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using psx::ErrorClass;
using psx::os::Event;
using psx::os::Interest;
using psx::os::Pipe;
using psx::os::Poller;
using psx::os::Readiness;

namespace {

std::vector<Poller::Backend> availableBackends() {
    std::vector<Poller::Backend> backends;
    for (Poller::Backend backend : {Poller::Backend::Poll, Poller::Backend::Kqueue, Poller::Backend::Epoll}) {
        if (Poller::available(backend)) {
            backends.push_back(backend);
        }
    }
    return backends;
}

std::string backendName(const ::testing::TestParamInfo<Poller::Backend>& info) {
    switch (info.param) {
        case Poller::Backend::Poll:
            return "poll";
        case Poller::Backend::Kqueue:
            return "kqueue";
        case Poller::Backend::Epoll:
            return "epoll";
        case Poller::Backend::Auto:
            return "auto";
    }
    return "unknown";
}

class PollerTest : public ::testing::TestWithParam<Poller::Backend> {
protected:
    void SetUp() override {
        auto created = Poller::create(GetParam());
        ASSERT_TRUE(created.ok()) << created.error().message();
        poller_ = std::move(created.value());
        EXPECT_EQ(poller_->backend(), GetParam());
    }

    // Collects the tokens reported by one wait().
    std::set<std::uint64_t> waitTokens(std::optional<std::chrono::milliseconds> timeout) {
        std::array<Event, 64> events{};
        auto count = poller_->wait(events, timeout);
        EXPECT_TRUE(count.ok()) << count.error().message();
        std::set<std::uint64_t> tokens;
        for (std::size_t i = 0; count.ok() && i < count.value(); ++i) {
            tokens.insert(events[i].token);
        }
        return tokens;
    }

    std::unique_ptr<Poller> poller_;
};

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

TEST_P(PollerTest, ReportsReadableOnlyWhenDataIsPending) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
    ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, 7).ok());
    EXPECT_EQ(poller_->size(), 1U);

    EXPECT_TRUE(waitTokens(0ms).empty());
    writeText(pipe.value().writer, "x");

    std::array<Event, 4> events{};
    auto count = poller_->wait(events, 1000ms);
    ASSERT_TRUE(count.ok());
    ASSERT_EQ(count.value(), 1U);
    EXPECT_EQ(events[0].token, 7U);
    EXPECT_TRUE(psx::os::has(events[0].readiness, Readiness::Readable));
}

TEST_P(PollerTest, HangupIsReportedWhenTheWriterCloses) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
    ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, 1).ok());
    pipe.value().writer.close();

    std::array<Event, 4> events{};
    auto count = poller_->wait(events, 1000ms);
    ASSERT_TRUE(count.ok());
    ASSERT_EQ(count.value(), 1U);
    EXPECT_TRUE(psx::os::has(events[0].readiness, Readiness::Hangup) ||
                psx::os::has(events[0].readiness, Readiness::Readable));
    // Draining must observe EOF.
    EXPECT_EQ(drainAll(pipe.value().reader), "");
}

TEST_P(PollerTest, WritableIsReportedForAnEmptyPipe) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().writer.setNonBlocking(true).ok());
    ASSERT_TRUE(poller_->add(pipe.value().writer, Interest::Writable, 2).ok());

    std::array<Event, 4> events{};
    auto count = poller_->wait(events, 1000ms);
    ASSERT_TRUE(count.ok());
    ASSERT_EQ(count.value(), 1U);
    EXPECT_TRUE(psx::os::has(events[0].readiness, Readiness::Writable));
}

TEST_P(PollerTest, InterestNoneSilencesAndReArmReportsPendingData) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
    ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, 3).ok());
    writeText(pipe.value().writer, "pending");

    ASSERT_TRUE(poller_->modify(3, Interest::None).ok());
    EXPECT_TRUE(waitTokens(20ms).empty()) << "no interest means no events, even with data";

    // Backpressure release: re-arming must report the data that was already
    // there (edge-triggered backends included).
    ASSERT_TRUE(poller_->modify(3, Interest::Readable).ok());
    EXPECT_EQ(waitTokens(1000ms), std::set<std::uint64_t>{3});
}

TEST_P(PollerTest, ADrainingConsumerNeverMissesData) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
    ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, 4).ok());

    std::string received;
    for (int round = 0; round < 5; ++round) {
        writeText(pipe.value().writer, "round" + std::to_string(round) + ";");
        ASSERT_EQ(waitTokens(1000ms), std::set<std::uint64_t>{4}) << "round " << round;
        received += drainAll(pipe.value().reader);
    }
    EXPECT_EQ(received, "round0;round1;round2;round3;round4;");
    EXPECT_TRUE(waitTokens(0ms).empty());
}

TEST_P(PollerTest, RemoveStopsEventsAndUnknownTokensAreRejected) {
    auto pipe = Pipe::create();
    ASSERT_TRUE(pipe.ok());
    ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, 5).ok());
    EXPECT_EQ(poller_->add(pipe.value().writer, Interest::Writable, 5).error().cls, ErrorClass::InvalidArgument)
        << "tokens must be unique";
    ASSERT_TRUE(poller_->remove(5).ok());
    EXPECT_EQ(poller_->size(), 0U);
    writeText(pipe.value().writer, "x");
    EXPECT_TRUE(waitTokens(20ms).empty());

    EXPECT_EQ(poller_->remove(5).error().cls, ErrorClass::NotFound);
    EXPECT_EQ(poller_->modify(99, Interest::Readable).error().cls, ErrorClass::NotFound);
    psx::os::Handle invalid;
    EXPECT_EQ(poller_->add(invalid, Interest::Readable, 6).error().cls, ErrorClass::Closed);
}

TEST_P(PollerTest, WakeUnblocksAWaitFromAnotherThread) {
    const auto start = std::chrono::steady_clock::now();
    std::thread waker([this] {
        std::this_thread::sleep_for(50ms);
        ASSERT_TRUE(poller_->wake().ok());
    });
    std::array<Event, 4> events{};
    auto count = poller_->wait(events, 5000ms);
    waker.join();
    ASSERT_TRUE(count.ok()) << count.error().message();
    EXPECT_EQ(count.value(), 0U) << "a wake-up is not a user event";
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 40ms);
    EXPECT_LT(elapsed, 2000ms);

    // A wake that happened before the wait is not lost either.
    ASSERT_TRUE(poller_->wake().ok());
    EXPECT_TRUE(waitTokens(5000ms).empty());
}

TEST_P(PollerTest, TimeoutIsHonoured) {
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(waitTokens(60ms).empty());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 50ms);
    EXPECT_LT(elapsed, 1000ms);
}

TEST_P(PollerTest, OnlyReadyHandlesAreReportedAmongMany) {
    std::vector<Pipe> pipes;
    for (std::uint64_t token = 100; token < 300; ++token) {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok()) << pipe.error().message();
        ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
        ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, token).ok());
        pipes.push_back(std::move(pipe.value()));
    }
    writeText(pipes[3].writer, "a");
    writeText(pipes[77].writer, "b");
    writeText(pipes[199].writer, "c");
    EXPECT_EQ(waitTokens(1000ms), (std::set<std::uint64_t>{103, 177, 299}));
}

TEST_P(PollerTest, RegistrationChurnLeaksNothing) {
    const auto before = os_test::openDescriptors();
    for (int i = 0; i < 10000; ++i) {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok());
        ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, static_cast<std::uint64_t>(i)).ok());
        ASSERT_TRUE(poller_->remove(static_cast<std::uint64_t>(i)).ok());
    }
    EXPECT_EQ(poller_->size(), 0U);
    EXPECT_EQ(os_test::openDescriptors(), before);
}

TEST_P(PollerTest, ClosingARegisteredHandleAfterRemoveIsSafe) {
    // The poller must not hold on to a descriptor the owner already closed.
    const auto before = os_test::openDescriptors();
    {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok());
        ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, 8).ok());
        ASSERT_TRUE(poller_->remove(8).ok());
    }
    EXPECT_EQ(os_test::openDescriptors(), before);
    EXPECT_TRUE(waitTokens(0ms).empty());
}

TEST_P(PollerTest, NoReadyEdgeIsLostWhenMoreFdsAreReadyThanTheEventBatch) {
    // Register many readable pipes and drain them with a deliberately small
    // events span: an edge-triggered backend must not consume-and-drop the
    // overflow (which would strand those fds forever). Every token must appear.
    constexpr std::uint64_t kCount = 100;
    std::vector<Pipe> pipes;
    for (std::uint64_t token = 0; token < kCount; ++token) {
        auto pipe = Pipe::create();
        ASSERT_TRUE(pipe.ok());
        ASSERT_TRUE(pipe.value().reader.setNonBlocking(true).ok());
        ASSERT_TRUE(poller_->add(pipe.value().reader, Interest::Readable, token).ok());
        writeText(pipe.value().writer, "x"); // all ready at once
        pipes.push_back(std::move(pipe.value()));
    }

    std::set<std::uint64_t> seen;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (seen.size() < kCount && std::chrono::steady_clock::now() < deadline) {
        std::array<Event, 8> events{}; // span smaller than the ready set
        auto count = poller_->wait(events, 1000ms);
        ASSERT_TRUE(count.ok()) << count.error().message();
        for (std::size_t i = 0; i < count.value(); ++i) {
            seen.insert(events[i].token);
            // Drain so an edge-triggered fd goes quiet until written again.
            drainAll(pipes[events[i].token].reader);
        }
    }
    EXPECT_EQ(seen.size(), kCount) << "an edge-triggered backend dropped ready fds it could not deliver";
}

INSTANTIATE_TEST_SUITE_P(Backends, PollerTest, ::testing::ValuesIn(availableBackends()), backendName);

TEST(PollerFactoryTest, PollIsAlwaysAvailableAndAutoPrefersTheNativeBackend) {
    EXPECT_TRUE(Poller::available(Poller::Backend::Poll));
    auto automatic = Poller::create();
    ASSERT_TRUE(automatic.ok());
#if defined(__APPLE__)
    EXPECT_EQ(automatic.value()->backend(), Poller::Backend::Kqueue);
    EXPECT_FALSE(Poller::available(Poller::Backend::Epoll));
#elif defined(__linux__)
    EXPECT_EQ(automatic.value()->backend(), Poller::Backend::Epoll);
    EXPECT_FALSE(Poller::available(Poller::Backend::Kqueue));
#endif
    EXPECT_EQ(
        Poller::create(Poller::available(Poller::Backend::Kqueue) ? Poller::Backend::Epoll : Poller::Backend::Kqueue)
            .error()
            .cls,
        ErrorClass::Unsupported);
}
