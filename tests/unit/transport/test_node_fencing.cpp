#include "psx/transport/frame_codec.hpp"
#include "psx/transport/node_stage_runner.hpp"
#include "psx/transport/open_request.hpp"
#include "psx/transport/session.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <fstream>
#include <string>
#include <thread>

using namespace psx::transport;

namespace {
// Waits up to ~3 s for the stage to write its pid, returns 0 on timeout.
::pid_t waitForPid(const std::string& path) {
    for (int i = 0; i < 3000; ++i) {
        std::ifstream in(path);
        ::pid_t pid = 0;
        if (in >> pid && pid > 0) {
            return pid;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}
} // namespace

// Fencing: when the controller connection drops, the node must not orphan the
// stage. Destroying the NodeStageRunner (what NodeServer does on disconnect)
// must kill the running process group.
TEST(NodeFencingTest, DroppingTheRunnerKillsTheRunningStage) {
    test_support::ScopedTempCwd cwd("fencing");
    const std::string pidfile = (cwd.path() / "stage.pid").string();

    auto reactor = psx::runtime::Reactor::create();
    ASSERT_TRUE(reactor.ok());

    auto runner = std::make_unique<NodeStageRunner>(*reactor.value());
    Session session(Role::Node, [](std::string_view) {}, *runner);
    runner->bind(session);

    // A stage that records its pid and lingers well past the test.
    const OpenRequest req{.argv = {"sh", "-c", "echo $$ > '" + pidfile + "'; sleep 30"}};
    const std::string open =
        encodeFrame({.type = FrameType::Open, .flags = 0, .streamId = 1, .payload = encodeOpen(req)});
    ASSERT_TRUE(session.receive(open).ok());

    const ::pid_t pid = waitForPid(pidfile);
    ASSERT_NE(pid, 0) << "the stage never started";
    ASSERT_EQ(::kill(pid, 0), 0) << "the stage should be alive before the drop";

    runner.reset(); // the controller "disconnected": tear the node's runner down

    // The stage (and its group) must be gone within a short grace.
    bool dead = false;
    for (int i = 0; i < 3000 && !dead; ++i) {
        if (::kill(pid, 0) != 0 && errno == ESRCH) {
            dead = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT_TRUE(dead) << "the stage was orphaned after the connection dropped (pid " << pid << ")";
}
