#include "psx/transport/native_transport.hpp"

#include "psx/transport/node_stage_runner.hpp"

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/runtime/reactor.hpp"
#include "tls_certs.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using psx::os::ExitStatus;
using psx::os::Socket;
using psx::os::Tls;
using psx::runtime::Reactor;
using namespace psx::transport;

namespace {

Socket acceptWithin(Socket& listener) {
    for (int i = 0; i < 2000; ++i) {
        auto a = listener.accept();
        if (a.ok()) {
            return std::move(a.value());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Socket{};
}

// The node side: on OPEN, "run" the stage — stream a line then exit 0.
struct NodeStage : SessionHandler {
    Session* session = nullptr;
    std::vector<OpenRequest> opens;
    void onOpen(StreamId id, const OpenRequest& request) override {
        opens.push_back(request);
        session->sendData(id, "output-line\n", /*endStream=*/false);
        session->sendExit(id, {ExitStatus::Kind::Exited, 0});
    }
};

// The controller side: collect the stage's output and exit.
struct ControllerSink : SessionHandler {
    Reactor* reactor = nullptr;
    std::string data;
    bool exited = false;
    ExitStatus status{};
    void onData(StreamId, std::string_view d, bool) override { data.append(d); }
    void onExit(StreamId, const ExitStatus& s) override {
        exited = true;
        status = s;
        reactor->stop();
    }
};

} // namespace

TEST(NativeTransportTest, RunsAStageEndToEndOverMtls) {
    // Loopback TCP.
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const auto port = listener.value().localPort();
    ASSERT_TRUE(port.ok());
    auto clientSock = Socket::connect("127.0.0.1", port.value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    // mTLS identities.
    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok());
    ASSERT_TRUE(nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStage nodeStage;
    ControllerSink ctlSink;
    ctlSink.reactor = &r;

    bool ctlReady = false;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, nodeStage,
                         {.onError = [&](psx::Error) {
                             errored = true;
                             r.stop();
                         }});
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, ctlSink,
                        {.onReady =
                             [&] {
                                 ctlReady = true;
                                 ctlPtr->session().open({.argv = {"echo", "hi"}, .cwd = "/tmp"});
                             },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    nodeStage.session = &node.session();
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(3), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    EXPECT_TRUE(ctlReady);
    ASSERT_EQ(nodeStage.opens.size(), 1U);
    EXPECT_EQ(nodeStage.opens[0].argv, (std::vector<std::string>{"echo", "hi"}));
    EXPECT_EQ(ctlSink.data, "output-line\n");
    EXPECT_TRUE(ctlSink.exited);
    EXPECT_EQ(ctlSink.status.code, 0);
    // Identity check both directions.
    EXPECT_EQ(ctl.peerSanUri(), "psx://node/1");
    EXPECT_EQ(node.peerSanUri(), "psx://controller");
}

TEST(NodeStageRunnerTest, RunsARealCommandOverTheEncryptedBackplane) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStageRunner runner(r); // the node runs each opened stream as a local process
    ControllerSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError = [&](psx::Error) {
                             errored = true;
                             r.stop();
                         }});
    runner.bind(node.session());
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady = [&] { ctlPtr->session().open({.argv = {"/bin/echo", "hello-from-stage"}}); },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    EXPECT_EQ(sink.data, "hello-from-stage\n"); // the real command's stdout, encrypted end to end
    EXPECT_TRUE(sink.exited);
    EXPECT_EQ(sink.status.code, 0);
    EXPECT_EQ(runner.runningStages(), 0U); // the stage was cleaned up
}

namespace {
// A controller sink that grants credit as it receives data (so backpressure can
// release) and collects the full output.
struct ConsumingSink : SessionHandler {
    Session* session = nullptr;
    Reactor* reactor = nullptr;
    std::string data;
    bool exited = false;
    void onData(StreamId id, std::string_view d, bool) override {
        data.append(d);
        session->consume(id, static_cast<std::uint32_t>(d.size()));
    }
    void onExit(StreamId, const ExitStatus&) override {
        exited = true;
        reactor->stop();
    }
};
} // namespace

TEST(NodeStageRunnerTest, BackpressureDeliversLargeOutputThroughASmallWindow) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned("psx://controller");
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create(
        {.certificatePem = nodeId.certPem, .privateKeyPem = nodeId.keyPem, .caPem = ctlId.certPem, .isServer = true});
    ASSERT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStageRunner runner(r);
    ConsumingSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;

    // Deliberately tiny window (512 B) so a ~9 KB stage forces many pause/resume
    // cycles in the node's reader.
    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError =
                              [&](psx::Error) {
                                  errored = true;
                                  r.stop();
                              }},
                         /*initialWindow=*/512);
    runner.bind(node.session());
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady = [&] { ctlPtr->session().open({.argv = {"sh", "-c", "seq 1 2000"}}); },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }},
                        /*initialWindow=*/512);
    sink.session = &ctl.session();
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(10), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    ASSERT_TRUE(sink.exited);
    // The full `seq 1 2000` output arrived intact despite the tiny window.
    std::string expected;
    for (int i = 1; i <= 2000; ++i) {
        expected += std::to_string(i) + "\n";
    }
    EXPECT_EQ(sink.data.size(), expected.size());
    EXPECT_EQ(sink.data, expected);
}
