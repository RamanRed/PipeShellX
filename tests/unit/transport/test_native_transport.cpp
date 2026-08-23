#include "psx/transport/native_transport.hpp"

#include "psx/ca/certificate_authority.hpp"
#include "psx/transport/native_controller.hpp"
#include "psx/transport/node_server.hpp"
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
    std::string stderrData;
    bool exited = false;
    ExitStatus status{};
    void onData(StreamId, std::string_view d, bool, Channel channel) override {
        (channel == Channel::Stderr ? stderrData : data).append(d);
    }
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
    std::string stderrData;
    bool exited = false;
    void onData(StreamId id, std::string_view d, bool, Channel channel) override {
        (channel == Channel::Stderr ? stderrData : data).append(d);
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

namespace {
// Builds a controller+node NativeTransport pair over loopback with the given
// controller SAN and node authorize predicate, runs the reactor, and reports
// whether the node authorized (onReady) or rejected (onError) the controller.
struct AuthzOutcome {
    bool nodeReady = false;
    bool nodeErrored = false;
};

AuthzOutcome runAuthz(const std::string& controllerSan, std::function<bool(std::string_view)> nodeAuthorize) {
    auto listener = Socket::listen("127.0.0.1", 0);
    EXPECT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    EXPECT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    EXPECT_TRUE(serverSock.valid());

    const auto ctlId = tls_test::generateSelfSigned(controllerSan);
    const auto nodeId = tls_test::generateSelfSigned("psx://node/1");
    auto ctlTls = Tls::create(
        {.certificatePem = ctlId.certPem, .privateKeyPem = ctlId.keyPem, .caPem = nodeId.certPem, .isServer = false});
    auto nodeTls = Tls::create({.certificatePem = nodeId.certPem,
                                .privateKeyPem = nodeId.keyPem,
                                .caPem = ctlId.certPem, // the node trusts this cert (authN passes)
                                .isServer = true});
    EXPECT_TRUE(ctlTls.ok() && nodeTls.ok());

    auto reactor = Reactor::create();
    EXPECT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeStage nodeStage;
    ControllerSink sink;
    sink.reactor = &r;
    AuthzOutcome outcome;

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, nodeStage,
                         {.authorize = std::move(nodeAuthorize),
                          .onReady =
                              [&] {
                                  outcome.nodeReady = true;
                                  r.stop();
                              },
                          .onError =
                              [&](psx::Error) {
                                  outcome.nodeErrored = true;
                                  r.stop();
                              }});
    nodeStage.session = &node.session();
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onError = [&](psx::Error) { r.stop(); }});

    EXPECT_TRUE(node.start().ok());
    EXPECT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(3), [&] { r.stop(); });
    EXPECT_TRUE(r.run().ok());
    return outcome;
}
} // namespace

TEST(NativeTransportTest, RejectsAnAuthenticatedButUnauthorizedPeer) {
    // The controller's cert is trusted (CA-signed), but its identity is not on
    // the node's allow-list, so authorization fails after the handshake.
    const AuthzOutcome outcome =
        runAuthz("psx://attacker", [](std::string_view san) { return san == "psx://controller"; });
    EXPECT_TRUE(outcome.nodeErrored);
    EXPECT_FALSE(outcome.nodeReady) << "an unauthorized peer must not reach onReady";
}

TEST(NativeTransportTest, AcceptsAnAuthorizedPeer) {
    const AuthzOutcome outcome =
        runAuthz("psx://controller", [](std::string_view san) { return san == "psx://controller"; });
    EXPECT_TRUE(outcome.nodeReady);
    EXPECT_FALSE(outcome.nodeErrored);
}

TEST(NodeServerTest, AcceptsAControllerAndRunsAStageWithCaIssuedCerts) {
    using psx::ca::CertificateAuthority;

    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    // The node daemon: trusts the CA, only admits the controller identity.
    psx::transport::NodeServer server(r, std::move(listener.value()),
                                      {.certificatePem = nodeId.value().certificatePem,
                                       .privateKeyPem = nodeId.value().privateKeyPem,
                                       .caPem = caCert},
                                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    // A controller connects and runs a stage.
    auto clientSock = Socket::connect("127.0.0.1", port);
    ASSERT_TRUE(clientSock.ok());
    auto ctlTls = Tls::create({.certificatePem = ctlId.value().certificatePem,
                               .privateKeyPem = ctlId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(ctlTls.ok());

    ControllerSink sink;
    sink.reactor = &r;
    bool errored = false;
    NativeTransport* ctlPtr = nullptr;
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onReady = [&] { ctlPtr->session().open({.argv = {"/bin/echo", "via-node-server"}}); },
                         .onError =
                             [&](psx::Error) {
                                 errored = true;
                                 r.stop();
                             }});
    ctlPtr = &ctl;
    ASSERT_TRUE(ctl.start().ok());

    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    EXPECT_TRUE(sink.exited);
    EXPECT_EQ(sink.data, "via-node-server\n"); // the stage ran on the accepted connection
    EXPECT_EQ(sink.status.code, 0);
    EXPECT_EQ(server.connectionCount(), 1U); // the controller's connection was accepted and kept

    // Metrics snapshot (the `node status` source): one connection accepted and
    // still open; the echo stage has already exited, so none is running.
    const auto snapshot = server.metrics();
    EXPECT_EQ(snapshot.acceptedTotal, 1U);
    EXPECT_EQ(snapshot.activeConnections, 1U);
    EXPECT_EQ(snapshot.activeStages, 0U);
}

TEST(NodeServerTest, DropsAnUnauthorizedConnectionAndReapsIt) {
    using psx::ca::CertificateAuthority;
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto attackerId = ca.value().issue("psx://attacker"); // CA-signed, but not authorized
    ASSERT_TRUE(nodeId.ok() && attackerId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    psx::transport::NodeServer server(
        r, std::move(listener.value()),
        {.certificatePem = nodeId.value().certificatePem,
         .privateKeyPem = nodeId.value().privateKeyPem,
         .caPem = caCert},
        [](std::string_view san) { return san == "psx://controller"; }); // attacker not allowed
    ASSERT_TRUE(server.start().ok());

    auto clientSock = Socket::connect("127.0.0.1", port);
    ASSERT_TRUE(clientSock.ok());
    auto ctlTls = Tls::create({.certificatePem = attackerId.value().certificatePem,
                               .privateKeyPem = attackerId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(ctlTls.ok());

    ControllerSink sink;
    sink.reactor = &r;
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onError = [&](psx::Error) { /* connection dropped by the node */ }});
    ASSERT_TRUE(ctl.start().ok());

    // Let the accept + reject + deferred reap run, then stop.
    r.after(std::chrono::milliseconds(800), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_EQ(server.connectionCount(), 0U) << "the unauthorized connection must be reaped";
}

namespace {
// Stands up a NodeServer on a fresh listener; returns {server-owning-reactor use}.
struct Fleet {
    psx::ca::CertificateAuthority ca;
    psx::ca::Identity nodeId;
    psx::ca::Identity ctlId;
    std::string caCert;
};

Fleet makeFleet() {
    auto ca = psx::ca::CertificateAuthority::create("psx-fleet");
    EXPECT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    EXPECT_TRUE(nodeId.ok() && ctlId.ok());
    std::string caCert = ca.value().certificatePem();
    return {std::move(ca.value()), std::move(nodeId.value()), std::move(ctlId.value()), std::move(caCert)};
}
} // namespace

TEST(NativeControllerTest, RunsACommandOnANodeAndCollectsOutput) {
    Fleet fleet = makeFleet();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    psx::transport::NodeServer server(r, std::move(listener.value()),
                                      {.certificatePem = fleet.nodeId.certificatePem,
                                       .privateKeyPem = fleet.nodeId.privateKeyPem,
                                       .caPem = fleet.caCert},
                                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    psx::transport::NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                                    .caPem = fleet.caCert});

    std::vector<psx::transport::NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start({{.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"}},
                           {"/bin/echo", "native-run"},
                           [&](std::vector<psx::transport::NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           })
                    .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results[0].ok) << results[0].error;
    EXPECT_EQ(results[0].exitCode, 0);
    EXPECT_EQ(results[0].output, "native-run\n");
    EXPECT_EQ(results[0].host, "127.0.0.1");
}

TEST(NativeControllerTest, ReportsAnErrorForAnUnreachableNode) {
    Fleet fleet = makeFleet();
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    psx::transport::NativeController controller(r, {.certificatePem = fleet.ctlId.certificatePem,
                                                    .privateKeyPem = fleet.ctlId.privateKeyPem,
                                                    .caPem = fleet.caCert});

    // Port 1 on loopback: connection refused.
    std::vector<psx::transport::NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start({{.host = "127.0.0.1", .port = 1, .expectedSan = "psx://node/1"}}, {"/bin/echo", "x"},
                           [&](std::vector<psx::transport::NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           })
                    .ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results[0].ok);
    EXPECT_FALSE(results[0].error.empty());
}

// Lease / silent-partition fencing: a peer that completes the handshake and then
// goes quiet (no FIN, no PONG) must be detected. With a fast lease the controller
// fires onError(Timeout) after maxMissed silent intervals.
TEST(NativeTransportTest, LeaseExpiresWhenThePeerGoesSilent) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const auto port = listener.value().localPort();
    ASSERT_TRUE(port.ok());
    auto clientSock = Socket::connect("127.0.0.1", port.value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

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

    // A "frozen" node: it finishes the handshake, then never sends another frame
    // -- in particular it never PONGs. The socket stays open (no FIN), so only the
    // lease can tell that the peer is gone.
    TlsStream silentNode(r, std::move(serverSock), std::move(nodeTls.value()),
                         TlsStream::Callbacks{.onData = [](std::string_view) {}, .onError = [](psx::Error) {}});
    ASSERT_TRUE(silentNode.start().ok());

    struct Sink : SessionHandler {
    } sink;
    bool errored = false;
    psx::Error captured{psx::ErrorClass::Other, 0, "none"};
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
                        {.onError =
                             [&](psx::Error e) {
                                 errored = true;
                                 captured = e;
                                 r.stop();
                             }},
                        kDefaultStreamWindow, HeartbeatOptions{std::chrono::milliseconds(5), 3});
    ASSERT_TRUE(ctl.start().ok());

    r.after(std::chrono::seconds(2), [&] { r.stop(); }); // safety net against a hang
    ASSERT_TRUE(r.run().ok());

    EXPECT_TRUE(errored) << "the lease never expired on a silent peer";
    EXPECT_EQ(captured.cls, psx::ErrorClass::Timeout);
}

// The lease must not fence a healthy-but-idle connection: with both ends running
// a fast lease and no stage traffic, the automatic PING/PONG keeps each side's
// lease satisfied, so neither errors across many intervals.
TEST(NativeTransportTest, LeaseKeepsAnIdleConnectionAlive) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const auto port = listener.value().localPort();
    ASSERT_TRUE(port.ok());
    auto clientSock = Socket::connect("127.0.0.1", port.value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

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

    struct Sink : SessionHandler {
    } nodeSink, ctlSink;
    bool errored = false;
    const HeartbeatOptions lease{std::chrono::milliseconds(5), 3};
    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, nodeSink,
                         {.onError = [&](psx::Error) { errored = true; }}, kDefaultStreamWindow, lease);
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, ctlSink,
                        {.onError = [&](psx::Error) { errored = true; }}, kDefaultStreamWindow, lease);
    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());

    // Sit idle for many lease intervals; a broken lease would fire well before this.
    r.after(std::chrono::milliseconds(80), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored) << "the lease falsely fenced a healthy idle connection";
}

// A real stage that writes to both fds; the node tags each pipe's bytes with its
// channel, and the controller separates them (as the SSH path does).
TEST(NodeStageRunnerTest, SplitsStdoutAndStderrOnTheWire) {
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

    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError = [&](psx::Error) {
                             errored = true;
                             r.stop();
                         }});
    runner.bind(node.session());
    NativeTransport ctl(
        r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, sink,
        {.onReady = [&] { ctlPtr->session().open({.argv = {"sh", "-c", "printf OUT; printf ERR 1>&2"}}); },
         .onError =
             [&](psx::Error) {
                 errored = true;
                 r.stop();
             }});
    sink.session = &ctl.session();
    ctlPtr = &ctl;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    ASSERT_TRUE(sink.exited);
    EXPECT_EQ(sink.data, "OUT");       // stdout channel
    EXPECT_EQ(sink.stderrData, "ERR"); // stderr channel, kept distinct
}

// A revoked peer must be rejected over the live reactor path too (not only in the
// synchronous Tls handshake). Reproduces the `node --crl` scenario in-process.
TEST(NativeTransportTest, CrlRejectsARevokedPeerOverTheReactor) {
    auto ca = psx::ca::CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    auto serial = psx::ca::CertificateAuthority::serialHex(ctlId.value().certificatePem);
    ASSERT_TRUE(serial.ok());
    auto crl = ca.value().issueCrl({serial.value()});
    ASSERT_TRUE(crl.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSock = Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSock.ok());
    Socket serverSock = acceptWithin(listener.value());
    ASSERT_TRUE(serverSock.valid());

    auto nodeTls = Tls::create({.certificatePem = nodeId.value().certificatePem,
                                .privateKeyPem = nodeId.value().privateKeyPem,
                                .caPem = caCert,
                                .crlPem = crl.value(), // the node enforces the CRL
                                .isServer = true});
    auto ctlTls = Tls::create({.certificatePem = ctlId.value().certificatePem,
                               .privateKeyPem = ctlId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(nodeTls.ok() && ctlTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    struct Sink : SessionHandler {
    } nodeSink, ctlSink;
    bool nodeReady = false;
    NativeTransport node(r, std::move(serverSock), std::move(nodeTls.value()), Role::Node, nodeSink,
                         {.onReady = [&] { nodeReady = true; }, .onError = [&](psx::Error) { r.stop(); }});
    NativeTransport ctl(r, std::move(clientSock.value()), std::move(ctlTls.value()), Role::Controller, ctlSink,
                        {.onError = [&](psx::Error) { r.stop(); }});
    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(ctl.start().ok());
    r.after(std::chrono::seconds(2), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(nodeReady) << "the node accepted a peer whose certificate is revoked";
}

// Concurrent fan-out over the real transport: one reactor, one NodeServer, and a
// controller opening many mTLS connections at once, each running a stage. A
// scaled-down stand-in for the 1000-node target (the full count is a CI /
// dedicated-host concern — 2*N RSA handshakes); this proves the reactor + TLS +
// NodeServer path handles a large concurrent fan-out correctly.
TEST(NodeServerTest, HandlesManyConcurrentControllerConnections) {
    using psx::ca::CertificateAuthority;
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto nodeId = ca.value().issue("psx://node/1");
    auto ctlId = ca.value().issue("psx://controller");
    ASSERT_TRUE(nodeId.ok() && ctlId.ok());
    const std::string caCert = ca.value().certificatePem();

    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    const std::uint16_t port = listener.value().localPort().value();

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    NodeServer server(r, std::move(listener.value()),
                      {.certificatePem = nodeId.value().certificatePem,
                       .privateKeyPem = nodeId.value().privateKeyPem,
                       .caPem = caCert},
                      [](std::string_view san) { return san == "psx://controller"; });
    ASSERT_TRUE(server.start().ok());

    NativeController controller(r, {.certificatePem = ctlId.value().certificatePem,
                                    .privateKeyPem = ctlId.value().privateKeyPem,
                                    .caPem = caCert});
    constexpr int kNodes = 100;
    std::vector<NativeController::Target> targets(
        kNodes, NativeController::Target{.host = "127.0.0.1", .port = port, .expectedSan = "psx://node/1"});

    std::vector<NativeController::HostResult> results;
    ASSERT_TRUE(controller
                    .start(targets, {"/bin/echo", "hi"},
                           [&](std::vector<NativeController::HostResult> res) {
                               results = std::move(res);
                               r.stop();
                           })
                    .ok());
    r.after(std::chrono::seconds(30), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    ASSERT_EQ(results.size(), static_cast<std::size_t>(kNodes));
    int succeeded = 0;
    for (const auto& res : results) {
        if (res.ok && res.exitCode == 0 && res.output == "hi\n") {
            ++succeeded;
        }
    }
    EXPECT_EQ(succeeded, kNodes) << "every concurrent connection should run its stage to a clean exit";
    EXPECT_EQ(server.metrics().acceptedTotal, static_cast<std::uint64_t>(kNodes));
}
