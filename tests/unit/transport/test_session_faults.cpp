#include "psx/transport/control_payloads.hpp"
#include "psx/transport/frame_codec.hpp"
#include "psx/transport/open_request.hpp"
#include "psx/transport/session.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace psx::transport;
using psx::os::ExitStatus;

namespace {

// Records nothing meaningful; fault tests care that the Session never crashes and
// returns a clean Result, not about the delivered events.
struct NullHandler : SessionHandler {};

std::string frame(FrameType type, std::uint8_t flags, StreamId id, const std::string& payload) {
    return encodeFrame({.type = type, .flags = flags, .streamId = id, .payload = payload});
}

} // namespace

// --- Structured faults: each must be a clean protocol error, never a crash. ---

TEST(SessionFaultTest, DataForAnUnknownStreamIsRejected) {
    NullHandler h;
    Session node(Role::Node, [](std::string_view) {}, h);
    EXPECT_FALSE(node.receive(frame(FrameType::Data, 0, 999, "bytes")).ok());
}

TEST(SessionFaultTest, DataAfterPeerExitIsRejectedBecauseItCannotBeACrossDirectionRace) {
    NullHandler h;
    std::string toPeer;
    Session ctl(Role::Controller, [&](std::string_view s) { toPeer.append(s); }, h);
    const StreamId id = ctl.open({.argv = {"x"}, .cwd = ""});
    // The node reports EXIT: the controller erases the stream.
    ASSERT_TRUE(
        ctl.receive(frame(FrameType::Exit, kFlagEndStream, id, encodeExit({ExitStatus::Kind::Exited, 0}))).ok());
    // DATA and EXIT originated at the same peer and preserve wire order, so DATA
    // after EXIT is not a cross-direction race and must be rejected.
    EXPECT_FALSE(ctl.receive(frame(FrameType::Data, 0, id, "late")).ok());
}

TEST(SessionFaultTest, DuplicateExitIsRejected) {
    NullHandler h;
    Session ctl(Role::Controller, [](std::string_view) {}, h);
    const StreamId id = ctl.open({.argv = {"x"}, .cwd = ""});
    const std::string exit = frame(FrameType::Exit, kFlagEndStream, id, encodeExit({ExitStatus::Kind::Exited, 0}));
    ASSERT_TRUE(ctl.receive(exit).ok());
    EXPECT_FALSE(ctl.receive(exit).ok()) << "a second EXIT targets a stream that no longer exists";
}

TEST(SessionFaultTest, WindowUpdateAfterPeerExitIsRejectedBecauseItIsSameDirection) {
    NullHandler h;
    Session ctl(Role::Controller, [](std::string_view) {}, h);
    const StreamId id = ctl.open({.argv = {"x"}, .cwd = ""});
    // The stream closes via EXIT...
    ASSERT_TRUE(
        ctl.receive(frame(FrameType::Exit, kFlagEndStream, id, encodeExit({ExitStatus::Kind::Exited, 0}))).ok());
    // ...then a WINDOW_UPDATE arrives from that same peer. Frame order makes
    // this impossible as a valid cross-direction close race.
    EXPECT_FALSE(ctl.receive(frame(FrameType::WindowUpdate, 0, id, encodeWindowUpdate(128))).ok());
}

TEST(SessionFaultTest, FramesCrossingALocallySentExitAreTolerated) {
    NullHandler h;
    std::string toPeer;
    Session node(Role::Node, [&](std::string_view bytes) { toPeer.append(bytes); }, h);
    ASSERT_TRUE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
    node.sendExit(1, {ExitStatus::Kind::Exited, 0});
    ASSERT_FALSE(toPeer.empty());

    // These controller-originated frames could already have been in flight when
    // the node sent EXIT in the opposite direction.
    EXPECT_TRUE(node.receive(frame(FrameType::Data, 0, 1, "crossing stdin")).ok());
    EXPECT_TRUE(node.receive(frame(FrameType::WindowUpdate, 0, 1, encodeWindowUpdate(128))).ok());
}

TEST(SessionFaultTest, CrossDirectionCloseRaceStillHonorsAReceivedHalfClose) {
    NullHandler h;
    Session node(Role::Node, [](std::string_view) {}, h);
    ASSERT_TRUE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
    node.sendExit(1, {ExitStatus::Kind::Exited, 0});

    ASSERT_TRUE(node.receive(frame(FrameType::Data, kFlagEndStream, 1, "last stdin")).ok());
    EXPECT_FALSE(node.receive(frame(FrameType::Data, 0, 1, "after eof")).ok());
}

TEST(SessionFaultTest, UnknownFrameTypeIsRejected) {
    NullHandler h;
    Session node(Role::Node, [](std::string_view) {}, h);
    // 0x00 and 0x7F are not valid FrameType values.
    EXPECT_FALSE(node.receive(frame(static_cast<FrameType>(0x00), 0, 1, "")).ok());
    NullHandler h2;
    Session node2(Role::Node, [](std::string_view) {}, h2);
    EXPECT_FALSE(node2.receive(frame(static_cast<FrameType>(0x7F), 0, 1, "")).ok());
}

TEST(SessionFaultTest, OpenReceivedByAControllerIsRejected) {
    NullHandler h;
    Session ctl(Role::Controller, [](std::string_view) {}, h);
    EXPECT_FALSE(ctl.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
}

TEST(SessionFaultTest, OpenIdsAreNonzeroAndStrictlySequential) {
    const std::string payload = encodeOpen({.argv = {"x"}, .cwd = ""});

    for (const StreamId first : {0U, 2U, 99U}) {
        NullHandler h;
        Session node(Role::Node, [](std::string_view) {}, h);
        EXPECT_FALSE(node.receive(frame(FrameType::Open, 0, first, payload)).ok()) << "first id " << first;
    }

    for (const StreamId second : {0U, 1U, 3U, 99U}) {
        NullHandler h;
        Session node(Role::Node, [](std::string_view) {}, h);
        ASSERT_TRUE(node.receive(frame(FrameType::Open, 0, 1, payload)).ok());
        EXPECT_FALSE(node.receive(frame(FrameType::Open, 0, second, payload)).ok()) << "second id " << second;
    }

    NullHandler h;
    Session node(Role::Node, [](std::string_view) {}, h);
    EXPECT_TRUE(node.receive(frame(FrameType::Open, 0, 1, payload)).ok());
    EXPECT_TRUE(node.receive(frame(FrameType::Open, 0, 2, payload)).ok());
}

TEST(SessionFaultTest, OpenAfterLocallySentGoAwayIsRejected) {
    NullHandler h;
    Session node(Role::Node, [](std::string_view) {}, h);
    node.goAway();
    EXPECT_FALSE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
}

TEST(SessionFaultTest, OpenAfterPeerSentGoAwayIsRejected) {
    NullHandler h;
    Session node(Role::Node, [](std::string_view) {}, h);
    ASSERT_TRUE(node.receive(frame(FrameType::GoAway, 0, 0, "")).ok());
    EXPECT_FALSE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
}

TEST(SessionFaultTest, ConnectionFramesRequireZeroStreamFlagsAndPayload) {
    for (const FrameType type : {FrameType::Ping, FrameType::Pong, FrameType::GoAway}) {
        const std::vector<Frame> invalid = {
            {.type = type, .flags = 0, .streamId = 1, .payload = {}},
            {.type = type, .flags = kFlagEndStream, .streamId = 0, .payload = {}},
            {.type = type, .flags = 0, .streamId = 0, .payload = "x"},
        };
        for (const Frame& bad : invalid) {
            NullHandler h;
            Session peer(Role::Node, [](std::string_view) {}, h);
            EXPECT_FALSE(peer.receive(encodeFrame(bad)).ok())
                << "type=" << static_cast<int>(type) << " flags=" << static_cast<int>(bad.flags)
                << " stream=" << bad.streamId << " payload=" << bad.payload.size();
        }
    }
}

TEST(SessionFaultTest, StreamFramesRequireNonzeroIdsAndTheirDefinedFlags) {
    {
        NullHandler h;
        Session node(Role::Node, [](std::string_view) {}, h);
        EXPECT_FALSE(node.receive(frame(FrameType::Data, 0, 0, "x")).ok());
    }
    {
        NullHandler h;
        Session controller(Role::Controller, [](std::string_view) {}, h);
        EXPECT_FALSE(controller.receive(frame(FrameType::WindowUpdate, 0, 0, encodeWindowUpdate(1))).ok());
    }
    {
        NullHandler h;
        Session controller(Role::Controller, [](std::string_view) {}, h);
        EXPECT_FALSE(
            controller.receive(frame(FrameType::Exit, kFlagEndStream, 0, encodeExit({ExitStatus::Kind::Exited, 0})))
                .ok());
    }

    {
        NullHandler h;
        Session node(Role::Node, [](std::string_view) {}, h);
        EXPECT_FALSE(
            node.receive(frame(FrameType::Open, kFlagEndStream, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
    }
    {
        NullHandler h;
        Session node(Role::Node, [](std::string_view) {}, h);
        ASSERT_TRUE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
        EXPECT_FALSE(node.receive(frame(FrameType::Data, 0x04, 1, "x")).ok());
    }
    {
        NullHandler h;
        Session controller(Role::Controller, [](std::string_view) {}, h);
        const StreamId id = controller.open({.argv = {"x"}, .cwd = ""});
        EXPECT_FALSE(
            controller.receive(frame(FrameType::WindowUpdate, kFlagEndStream, id, encodeWindowUpdate(1))).ok());
    }
    {
        NullHandler h;
        Session controller(Role::Controller, [](std::string_view) {}, h);
        const StreamId id = controller.open({.argv = {"x"}, .cwd = ""});
        EXPECT_FALSE(
            controller.receive(frame(FrameType::Exit, kFlagStderr, id, encodeExit({ExitStatus::Kind::Exited, 0})))
                .ok());
    }
}

TEST(SessionFaultTest, ControllerToNodeDataCannotClaimAnOutputChannel) {
    NullHandler h;
    Session node(Role::Node, [](std::string_view) {}, h);
    ASSERT_TRUE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
    EXPECT_FALSE(node.receive(frame(FrameType::Data, kFlagStderr, 1, "not stdin")).ok());
}

TEST(SessionFaultTest, DataAfterReceivedEndStreamIsRejected) {
    NullHandler h;
    Session node(Role::Node, [](std::string_view) {}, h);
    ASSERT_TRUE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
    ASSERT_TRUE(node.receive(frame(FrameType::Data, kFlagEndStream, 1, "last")).ok());
    EXPECT_FALSE(node.receive(frame(FrameType::Data, 0, 1, "late")).ok());
}

TEST(SessionFaultTest, MalformedControlPayloadsAreNeverHiddenByCloseRaceTolerance) {
    for (const std::string& payload : {std::string(), std::string("\0\0\0", 3), encodeWindowUpdate(0)}) {
        NullHandler h;
        Session node(Role::Node, [](std::string_view) {}, h);
        ASSERT_TRUE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
        node.sendExit(1, {ExitStatus::Kind::Exited, 0});
        EXPECT_FALSE(node.receive(frame(FrameType::WindowUpdate, 0, 1, payload)).ok());
    }

    for (const std::string& payload : {std::string(), std::string("\0\0\0\0", 4), std::string("\0\0\0\0\0\0", 6)}) {
        NullHandler h;
        Session controller(Role::Controller, [](std::string_view) {}, h);
        const StreamId id = controller.open({.argv = {"x"}, .cwd = ""});
        EXPECT_FALSE(controller.receive(frame(FrameType::Exit, kFlagEndStream, id, payload)).ok());
    }
}

TEST(SessionFaultTest, ReservedFlagBitsAreRejectedDeterministicallyForEveryFrameType) {
    for (std::uint8_t bit = 0x04; bit != 0; bit = static_cast<std::uint8_t>(bit << 1)) {
        {
            NullHandler h;
            Session node(Role::Node, [](std::string_view) {}, h);
            EXPECT_FALSE(node.receive(frame(FrameType::Open, bit, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
        }
        {
            NullHandler h;
            Session node(Role::Node, [](std::string_view) {}, h);
            ASSERT_TRUE(node.receive(frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"x"}, .cwd = ""}))).ok());
            EXPECT_FALSE(node.receive(frame(FrameType::Data, bit, 1, "x")).ok());
        }
        {
            NullHandler h;
            Session controller(Role::Controller, [](std::string_view) {}, h);
            const StreamId id = controller.open({.argv = {"x"}, .cwd = ""});
            EXPECT_FALSE(controller.receive(frame(FrameType::WindowUpdate, bit, id, encodeWindowUpdate(1))).ok());
        }
        {
            NullHandler h;
            Session controller(Role::Controller, [](std::string_view) {}, h);
            const StreamId id = controller.open({.argv = {"x"}, .cwd = ""});
            EXPECT_FALSE(
                controller.receive(frame(FrameType::Exit, bit, id, encodeExit({ExitStatus::Kind::Exited, 0}))).ok());
        }
        for (const FrameType type : {FrameType::Ping, FrameType::Pong, FrameType::GoAway}) {
            NullHandler h;
            Session peer(Role::Node, [](std::string_view) {}, h);
            EXPECT_FALSE(peer.receive(frame(type, bit, 0, "")).ok());
        }
    }
}

TEST(SessionFaultTest, SemanticProtocolErrorPoisonsTheSession) {
    NullHandler h;
    std::string outbound;
    Session node(Role::Node, [&](std::string_view bytes) { outbound.append(bytes); }, h);
    ASSERT_FALSE(node.receive(frame(FrameType::Ping, kFlagEndStream, 0, "")).ok());
    EXPECT_FALSE(node.receive(frame(FrameType::Ping, 0, 0, "")).ok());
    EXPECT_TRUE(outbound.empty()) << "a poisoned session must not auto-answer later frames";
}

// --- Fuzz: arbitrary and corrupted byte streams must never crash (ASan/UBSan
//     validates memory safety); receive() may return ok or a clean error. ---

TEST(SessionFaultTest, ArbitraryByteStreamsNeverCrash) {
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> len(0, 80);
    std::uniform_int_distribution<int> byte(0, 255);
    for (int iter = 0; iter < 5000; ++iter) {
        std::string junk;
        const int n = len(rng);
        for (int i = 0; i < n; ++i) {
            junk.push_back(static_cast<char>(byte(rng)));
        }
        NullHandler h;
        Session s(iter % 2 == 0 ? Role::Node : Role::Controller, [](std::string_view) {}, h);
        (void)s.receive(junk); // must return, never crash
    }
    SUCCEED();
}

TEST(SessionFaultTest, CorruptedValidFrameStreamsNeverCrash) {
    // A plausible conversation to mutate: OPEN, two DATA, WINDOW_UPDATE, EXIT.
    std::string base;
    base += frame(FrameType::Open, 0, 1, encodeOpen({.argv = {"run"}, .cwd = "/tmp"}));
    base += frame(FrameType::Data, 0, 1, "hello");
    base += frame(FrameType::Data, kFlagEndStream, 1, "world");
    base += frame(FrameType::WindowUpdate, 0, 1, encodeWindowUpdate(64));
    base += frame(FrameType::Exit, kFlagEndStream, 1, encodeExit({ExitStatus::Kind::Exited, 0}));

    std::mt19937 rng(0xBADF00D);
    std::uniform_int_distribution<int> pick(0, 3);
    for (int iter = 0; iter < 5000; ++iter) {
        std::string mutated = base;
        if (!mutated.empty()) {
            std::uniform_int_distribution<std::size_t> at(0, mutated.size() - 1);
            switch (pick(rng)) {
                case 0: // flip a byte
                    mutated[at(rng)] ^= static_cast<char>(1 + (rng() & 0x7F));
                    break;
                case 1: // truncate
                    mutated.resize(at(rng));
                    break;
                case 2: // duplicate a chunk
                    mutated.insert(at(rng), mutated.substr(0, at(rng)));
                    break;
                case 3: // drop a byte
                    mutated.erase(at(rng), 1);
                    break;
            }
        }
        NullHandler h;
        Session s(Role::Node, [](std::string_view) {}, h);
        // Feed in two random splits to exercise the streaming decoder too.
        if (mutated.size() > 1) {
            const std::size_t cut = std::uniform_int_distribution<std::size_t>(0, mutated.size())(rng);
            (void)s.receive(std::string_view(mutated).substr(0, cut));
            (void)s.receive(std::string_view(mutated).substr(cut));
        } else {
            (void)s.receive(mutated);
        }
    }
    SUCCEED();
}
