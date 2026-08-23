#include "psx/transport/session.hpp"

#include "psx/transport/control_payloads.hpp"
#include "psx/transport/frame_codec.hpp"

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

using psx::os::ExitStatus;
using namespace psx::transport;

namespace {

struct Recorder : SessionHandler {
    std::vector<std::pair<StreamId, OpenRequest>> opens;
    std::vector<std::tuple<StreamId, std::string, bool>> datas;
    std::vector<Channel> channels; // the channel of each onData, in arrival order
    std::vector<std::pair<StreamId, ExitStatus>> exits;
    int goAways = 0;
    int pongs = 0;
    void onOpen(StreamId id, const OpenRequest& r) override { opens.emplace_back(id, r); }
    void onData(StreamId id, std::string_view b, bool e, Channel c) override {
        datas.emplace_back(id, std::string(b), e);
        channels.push_back(c);
    }
    void onExit(StreamId id, const ExitStatus& s) override { exits.emplace_back(id, s); }
    void onGoAway() override { ++goAways; }
    void onPong() override { ++pongs; }
};

// Two sessions wired write->receive through byte buffers, pumped to quiescence.
struct Link {
    std::string aToB;
    std::string bToA;
    Recorder ctl;
    Recorder node;
    Session a{Role::Controller, [this](std::string_view s) { aToB.append(s); }, ctl};
    Session b{Role::Node, [this](std::string_view s) { bToA.append(s); }, node};

    psx::Result<void> pump() {
        while (!aToB.empty() || !bToA.empty()) {
            std::string toB;
            toB.swap(aToB);
            std::string toA;
            toA.swap(bToA);
            if (!toB.empty()) {
                if (auto r = b.receive(toB); !r.ok()) {
                    return r;
                }
            }
            if (!toA.empty()) {
                if (auto r = a.receive(toA); !r.ok()) {
                    return r;
                }
            }
        }
        return {};
    }
};

} // namespace

TEST(SessionTest, OpenDataExitFlowsEndToEnd) {
    Link link;
    const OpenRequest req{.argv = {"echo", "hi"}, .cwd = "/tmp"};
    const StreamId id = link.a.open(req);
    ASSERT_TRUE(link.pump().ok());

    // Node saw the OPEN with the exact request.
    ASSERT_EQ(link.node.opens.size(), 1U);
    EXPECT_EQ(link.node.opens[0].first, id);
    EXPECT_EQ(link.node.opens[0].second, req);
    EXPECT_EQ(link.b.openStreamCount(), 1U);

    // Node streams stdout then exits; controller sees both.
    link.b.sendData(id, "hello\n", /*endStream=*/false);
    link.b.sendExit(id, {ExitStatus::Kind::Exited, 0});
    ASSERT_TRUE(link.pump().ok());

    ASSERT_EQ(link.ctl.datas.size(), 1U);
    EXPECT_EQ(std::get<0>(link.ctl.datas[0]), id);
    EXPECT_EQ(std::get<1>(link.ctl.datas[0]), "hello\n");
    ASSERT_EQ(link.ctl.exits.size(), 1U);
    EXPECT_EQ(link.ctl.exits[0].second.code, 0);
    // Terminal on both sides.
    EXPECT_EQ(link.a.openStreamCount(), 0U);
    EXPECT_EQ(link.b.openStreamCount(), 0U);
}

TEST(SessionTest, ControllerCanSendStdinToTheStream) {
    Link link;
    const StreamId id = link.a.open({.argv = {"cat"}, .cwd = ""});
    link.a.sendData(id, "stdin-bytes", /*endStream=*/true);
    ASSERT_TRUE(link.pump().ok());
    ASSERT_EQ(link.node.datas.size(), 1U);
    EXPECT_EQ(std::get<1>(link.node.datas[0]), "stdin-bytes");
    EXPECT_TRUE(std::get<2>(link.node.datas[0])); // endStream flag propagated
}

TEST(SessionTest, TwoStreamsAreMultiplexedIndependently) {
    Link link;
    const StreamId a = link.a.open({.argv = {"a"}, .cwd = ""});
    const StreamId b = link.a.open({.argv = {"b"}, .cwd = ""});
    EXPECT_NE(a, b);
    ASSERT_TRUE(link.pump().ok());
    EXPECT_EQ(link.node.opens.size(), 2U);
    EXPECT_EQ(link.b.openStreamCount(), 2U);
}

TEST(SessionTest, PingIsAutoAnsweredWithPong) {
    Link link;
    link.a.ping();
    ASSERT_TRUE(link.pump().ok());
    EXPECT_EQ(link.ctl.pongs, 1); // the controller got the node's auto-pong
    EXPECT_EQ(link.node.pongs, 0);
}

TEST(SessionTest, GoAwayIsDelivered) {
    Link link;
    link.a.goAway();
    ASSERT_TRUE(link.pump().ok());
    EXPECT_EQ(link.node.goAways, 1);
}

// --- Protocol validation: a malformed / illegal frame ends the connection ---

TEST(SessionTest, DataForAnUnknownStreamIsAProtocolError) {
    Recorder rec;
    Session node(Role::Node, [](std::string_view) {}, rec);
    const std::string wire = encodeFrame({.type = FrameType::Data, .flags = 0, .streamId = 42, .payload = "x"});
    EXPECT_FALSE(node.receive(wire).ok());
}

TEST(SessionTest, AControllerRejectsAnOpen) {
    Recorder rec;
    Session ctl(Role::Controller, [](std::string_view) {}, rec);
    const std::string wire =
        encodeFrame({.type = FrameType::Open, .flags = 0, .streamId = 1, .payload = encodeOpen({.argv = {"x"}})});
    EXPECT_FALSE(ctl.receive(wire).ok());
}

TEST(SessionTest, ANodeRejectsAnExit) {
    Recorder rec;
    Session node(Role::Node, [](std::string_view) {}, rec);
    const std::string wire = encodeFrame(
        {.type = FrameType::Exit, .flags = 0, .streamId = 1, .payload = encodeExit({ExitStatus::Kind::Exited, 0})});
    EXPECT_FALSE(node.receive(wire).ok());
}

TEST(SessionTest, AMalformedOpenPayloadIsAProtocolError) {
    Recorder rec;
    Session node(Role::Node, [](std::string_view) {}, rec);
    const std::string wire =
        encodeFrame({.type = FrameType::Open, .flags = 0, .streamId = 1, .payload = "not-a-valid-open"});
    EXPECT_FALSE(node.receive(wire).ok());
}

// --- Flow control (credit windows) ---

// Two sessions sharing a small window; used to exercise backpressure cheaply.
struct SmallLink {
    static constexpr std::uint32_t kWindow = 8;
    std::string aToB;
    std::string bToA;
    Recorder ctl;
    Recorder node;
    Session a{Role::Controller, [this](std::string_view s) { aToB.append(s); }, ctl, kWindow};
    Session b{Role::Node, [this](std::string_view s) { bToA.append(s); }, node, kWindow};
    psx::Result<void> pump() {
        while (!aToB.empty() || !bToA.empty()) {
            std::string toB;
            toB.swap(aToB);
            std::string toA;
            toA.swap(bToA);
            if (!toB.empty()) {
                if (auto r = b.receive(toB); !r.ok()) {
                    return r;
                }
            }
            if (!toA.empty()) {
                if (auto r = a.receive(toA); !r.ok()) {
                    return r;
                }
            }
        }
        return {};
    }
    std::string nodeBytes() const {
        std::string all;
        for (const auto& d : node.datas) {
            all += std::get<1>(d);
        }
        return all;
    }
};

TEST(SessionTest, SenderIsBackpressuredToTheWindowUntilCreditIsGranted) {
    SmallLink link;
    const StreamId id = link.a.open({.argv = {"cat"}, .cwd = ""});
    ASSERT_TRUE(link.pump().ok());
    // Queue 20 bytes into an 8-byte window.
    link.a.sendData(id, "0123456789ABCDEFGHIJ", /*endStream=*/true);
    ASSERT_TRUE(link.pump().ok());
    // Only the first window's worth got through; the rest is buffered at the sender.
    EXPECT_EQ(link.nodeBytes(), "01234567");

    // The node consumes 8 bytes -> WINDOW_UPDATE -> the sender releases the next 8.
    link.b.consume(id, 8);
    ASSERT_TRUE(link.pump().ok());
    EXPECT_EQ(link.nodeBytes(), "0123456789ABCDEF");

    // Drain the rest; the final chunk carries the buffered endStream.
    link.b.consume(id, 8);
    ASSERT_TRUE(link.pump().ok());
    EXPECT_EQ(link.nodeBytes(), "0123456789ABCDEFGHIJ");
    ASSERT_FALSE(link.node.datas.empty());
    EXPECT_TRUE(std::get<2>(link.node.datas.back())) << "endStream rides the last chunk";
}

TEST(SessionTest, InboundDataExceedingTheWindowIsAFlowControlError) {
    Recorder rec;
    // A node with an 8-byte window; feed it a raw DATA of 9 bytes on an opened stream.
    Session node(Role::Node, [](std::string_view) {}, rec, /*initialWindow=*/8);
    const std::string open =
        encodeFrame({.type = FrameType::Open, .flags = 0, .streamId = 1, .payload = encodeOpen({.argv = {"x"}})});
    ASSERT_TRUE(node.receive(open).ok());
    const std::string data =
        encodeFrame({.type = FrameType::Data, .flags = 0, .streamId = 1, .payload = "123456789"}); // 9 > 8
    EXPECT_FALSE(node.receive(data).ok());
}

TEST(SessionTest, AWindowUpdateThatOverflowsTheWindowIsAProtocolError) {
    Recorder rec;
    Session ctl(Role::Controller, [](std::string_view) {}, rec);
    const StreamId id = ctl.open({.argv = {"x"}, .cwd = ""}); // starts at kDefaultStreamWindow
    // A WINDOW_UPDATE near-u32max pushes sendCredit past kMaxStreamWindow.
    const std::string wu = encodeFrame(
        {.type = FrameType::WindowUpdate, .flags = 0, .streamId = id, .payload = encodeWindowUpdate(0xFFFFFFF0u)});
    EXPECT_FALSE(ctl.receive(wu).ok());
}

TEST(SessionTest, StreamWritableSignalsBackpressureAndResumeOnCredit) {
    SmallLink link; // window 8
    const StreamId id = link.a.open({.argv = {"x"}, .cwd = ""});
    ASSERT_TRUE(link.pump().ok());

    bool resumed = false;
    link.a.onStreamWritable([&](StreamId) { resumed = true; });

    // Queue 24 bytes into an 8-byte window: 8 fly, 16 buffer (>= window) -> not writable.
    link.a.sendData(id, "0123456789ABCDEFGHIJKLMN", /*endStream=*/true);
    ASSERT_TRUE(link.pump().ok());
    EXPECT_FALSE(link.a.streamWritable(id));
    EXPECT_FALSE(resumed);

    // The receiver consuming grants credit; the sender drains and, once back below
    // the mark, fires onStreamWritable and reports writable again.
    for (int round = 0; round < 4 && !link.a.streamWritable(id); ++round) {
        link.b.consume(id, 8);
        ASSERT_TRUE(link.pump().ok());
    }
    EXPECT_EQ(link.nodeBytes(), "0123456789ABCDEFGHIJKLMN");
    EXPECT_TRUE(link.a.streamWritable(id));
    EXPECT_TRUE(resumed);
}

TEST(SessionTest, InterleavedStdoutAndStderrKeepTheirChannelAndOrder) {
    Link link;
    const StreamId id = link.a.open({.argv = {"x"}, .cwd = ""});
    ASSERT_TRUE(link.pump().ok());

    // The node interleaves the two channels; each DATA frame carries its channel.
    link.b.sendData(id, "out-1", /*endStream=*/false, Channel::Stdout);
    link.b.sendData(id, "err-1", /*endStream=*/false, Channel::Stderr);
    link.b.sendData(id, "out-2", /*endStream=*/false, Channel::Stdout);
    ASSERT_TRUE(link.pump().ok());

    ASSERT_EQ(link.ctl.datas.size(), 3U);
    EXPECT_EQ(std::get<1>(link.ctl.datas[0]), "out-1");
    EXPECT_EQ(std::get<1>(link.ctl.datas[1]), "err-1");
    EXPECT_EQ(std::get<1>(link.ctl.datas[2]), "out-2");
    ASSERT_EQ(link.ctl.channels.size(), 3U);
    EXPECT_EQ(link.ctl.channels[0], Channel::Stdout);
    EXPECT_EQ(link.ctl.channels[1], Channel::Stderr);
    EXPECT_EQ(link.ctl.channels[2], Channel::Stdout);
}
