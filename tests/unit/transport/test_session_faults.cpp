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

TEST(SessionFaultTest, DataAfterTheStreamIsClosedIsIgnored) {
    NullHandler h;
    std::string toPeer;
    Session ctl(Role::Controller, [&](std::string_view s) { toPeer.append(s); }, h);
    const StreamId id = ctl.open({.argv = {"x"}, .cwd = ""});
    // The node reports EXIT: the controller erases the stream.
    ASSERT_TRUE(
        ctl.receive(frame(FrameType::Exit, kFlagEndStream, id, encodeExit({ExitStatus::Kind::Exited, 0}))).ok());
    // A straggler DATA for the now-closed stream is a benign close race (the peer
    // had it in flight when EXIT crossed): ignored, not fatal.
    EXPECT_TRUE(ctl.receive(frame(FrameType::Data, 0, id, "late")).ok());
}

TEST(SessionFaultTest, DuplicateExitIsRejected) {
    NullHandler h;
    Session ctl(Role::Controller, [](std::string_view) {}, h);
    const StreamId id = ctl.open({.argv = {"x"}, .cwd = ""});
    const std::string exit = frame(FrameType::Exit, kFlagEndStream, id, encodeExit({ExitStatus::Kind::Exited, 0}));
    ASSERT_TRUE(ctl.receive(exit).ok());
    EXPECT_FALSE(ctl.receive(exit).ok()) << "a second EXIT targets a stream that no longer exists";
}

TEST(SessionFaultTest, WindowUpdateForAClosedStreamIsIgnoredNotFatal) {
    NullHandler h;
    Session ctl(Role::Controller, [](std::string_view) {}, h);
    const StreamId id = ctl.open({.argv = {"x"}, .cwd = ""});
    // The stream closes via EXIT...
    ASSERT_TRUE(
        ctl.receive(frame(FrameType::Exit, kFlagEndStream, id, encodeExit({ExitStatus::Kind::Exited, 0}))).ok());
    // ...then a straggler WINDOW_UPDATE arrives for it: ignored, not fatal (the
    // closed-stream race, a real bug this locks in).
    EXPECT_TRUE(ctl.receive(frame(FrameType::WindowUpdate, 0, id, encodeWindowUpdate(128))).ok());
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
