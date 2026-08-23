#include "psx/transport/session.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace psx::transport;
using psx::os::ExitStatus;

namespace {

// A node that, on each OPEN, streams one line and exits 0 — a minimal "stage".
struct EchoNode : SessionHandler {
    Session* session = nullptr;
    void onOpen(StreamId id, const OpenRequest&) override {
        session->sendData(id, "ok\n", /*endStream=*/false);
        session->sendExit(id, {ExitStatus::Kind::Exited, 0});
    }
};

// A controller that counts stage exits and the bytes it received.
struct Collector : SessionHandler {
    int exits = 0;
    std::size_t bytes = 0;
    void onData(StreamId, std::string_view d, bool, Channel) override { bytes += d.size(); }
    void onExit(StreamId, const ExitStatus& s) override {
        if (s.kind == ExitStatus::Kind::Exited && s.code == 0) {
            ++exits;
        }
    }
};

// Drives two wired Sessions to quiescence; false on any protocol error.
bool pump(Session& a, std::string& aToB, Session& b, std::string& bToA) {
    while (!aToB.empty() || !bToA.empty()) {
        std::string toB;
        toB.swap(aToB);
        std::string toA;
        toA.swap(bToA);
        if (!toB.empty() && !b.receive(toB).ok()) {
            return false;
        }
        if (!toA.empty() && !a.receive(toA).ok()) {
            return false;
        }
    }
    return true;
}

} // namespace

// One connection multiplexing 1000 concurrent streams: the mux + per-stream flow
// control scale to a large fan-out of in-flight stages on a single link.
TEST(ScaleTest, OneConnectionMultiplexesAThousandConcurrentStreams) {
    constexpr int kStreams = 1000;
    std::string aToB;
    std::string bToA;
    EchoNode node;
    Collector ctl;
    Session controller(Role::Controller, [&](std::string_view s) { aToB.append(s); }, ctl);
    Session nodeSession(Role::Node, [&](std::string_view s) { bToA.append(s); }, node);
    node.session = &nodeSession;

    for (int i = 0; i < kStreams; ++i) {
        controller.open({.argv = {"echo", "ok"}, .cwd = ""});
    }
    ASSERT_TRUE(pump(controller, aToB, nodeSession, bToA));

    EXPECT_EQ(ctl.exits, kStreams);
    EXPECT_EQ(ctl.bytes, static_cast<std::size_t>(kStreams) * 3); // "ok\n" per stream
    EXPECT_EQ(controller.openStreamCount(), 0U);                  // every stream reached EXIT and closed
    EXPECT_EQ(nodeSession.openStreamCount(), 0U);
}

// 1000 simulated nodes, each on its own connection, every one running a stage to
// completion — the fan-out count the Phase 4 exit criterion targets, exercised
// through the real protocol (no TLS/socket/process cost).
TEST(ScaleTest, ThousandSimulatedNodesEachRunAStage) {
    constexpr int kNodes = 1000;
    int completed = 0;
    for (int i = 0; i < kNodes; ++i) {
        std::string aToB;
        std::string bToA;
        EchoNode node;
        Collector ctl;
        Session controller(Role::Controller, [&](std::string_view s) { aToB.append(s); }, ctl);
        Session nodeSession(Role::Node, [&](std::string_view s) { bToA.append(s); }, node);
        node.session = &nodeSession;

        controller.open({.argv = {"echo", "ok"}, .cwd = ""});
        ASSERT_TRUE(pump(controller, aToB, nodeSession, bToA));
        if (ctl.exits == 1 && ctl.bytes == 3) {
            ++completed;
        }
    }
    EXPECT_EQ(completed, kNodes);
}
