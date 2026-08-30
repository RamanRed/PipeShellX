#include "psx/policy/policy.hpp"
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

namespace {

psx::os::Socket acceptWithin(psx::os::Socket& listener) {
    for (int i = 0; i < 2000; ++i) {
        auto accepted = listener.accept();
        if (accepted.ok()) {
            return std::move(accepted.value());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

struct ControllerSink : psx::transport::SessionHandler {
    psx::runtime::Reactor* reactor = nullptr;
    std::string stdoutData;
    std::string stderrData;
    bool exited = false;
    psx::os::ExitStatus status{};

    void onData(psx::transport::StreamId, std::string_view bytes, bool, psx::transport::Channel channel) override {
        (channel == psx::transport::Channel::Stderr ? stderrData : stdoutData).append(bytes);
    }

    void onExit(psx::transport::StreamId, const psx::os::ExitStatus& value) override {
        status = value;
        exited = true;
        reactor->stop();
    }
};

} // namespace

TEST(NodePolicyTest, RejectsACommandBeforeSpawningItAndReturnsExit126) {
    using namespace psx::transport;

    auto listener = psx::os::Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto clientSocket = psx::os::Socket::connect("127.0.0.1", listener.value().localPort().value());
    ASSERT_TRUE(clientSocket.ok());
    auto serverSocket = acceptWithin(listener.value());
    ASSERT_TRUE(serverSocket.valid());

    const auto controllerIdentity = tls_test::generateSelfSigned("psx://controller");
    const auto nodeIdentity = tls_test::generateSelfSigned("psx://node/policy-test");
    auto controllerTls = psx::os::Tls::create({.certificatePem = controllerIdentity.certPem,
                                               .privateKeyPem = controllerIdentity.keyPem,
                                               .caPem = nodeIdentity.certPem,
                                               .isServer = false});
    auto nodeTls = psx::os::Tls::create({.certificatePem = nodeIdentity.certPem,
                                         .privateKeyPem = nodeIdentity.keyPem,
                                         .caPem = controllerIdentity.certPem,
                                         .isServer = true});
    ASSERT_TRUE(controllerTls.ok() && nodeTls.ok());

    auto reactor = psx::runtime::Reactor::create();
    ASSERT_TRUE(reactor.ok());
    auto& r = *reactor.value();
    const auto policy = psx::policy::Policy::parse("allow echo\n");
    NodeStageRunner runner(r, [policy](const OpenRequest& request) { return policy.validate(request.argv); });
    ControllerSink sink;
    sink.reactor = &r;
    bool transportError = false;
    NativeTransport* controllerPtr = nullptr;

    NativeTransport node(r, std::move(serverSocket), std::move(nodeTls.value()), Role::Node, runner,
                         {.onError = [&](const psx::Error&) {
                             transportError = true;
                             r.stop();
                         }});
    runner.bind(node.session());
    NativeTransport controller(
        r, std::move(clientSocket.value()), std::move(controllerTls.value()), Role::Controller, sink,
        {.onReady = [&] { controllerPtr->session().open({.argv = {"/bin/echo", "must-not-run"}}); },
         .onError =
             [&](const psx::Error&) {
                 transportError = true;
                 r.stop();
             }});
    controllerPtr = &controller;

    ASSERT_TRUE(node.start().ok());
    ASSERT_TRUE(controller.start().ok());
    r.after(std::chrono::seconds(5), [&] { r.stop(); });
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(transportError);
    EXPECT_TRUE(sink.exited);
    EXPECT_EQ(sink.status.kind, psx::os::ExitStatus::Kind::Exited);
    EXPECT_EQ(sink.status.code, 126);
    EXPECT_TRUE(sink.stdoutData.empty());
    EXPECT_NE(sink.stderrData.find("rejected by policy"), std::string::npos);
    EXPECT_EQ(runner.runningStages(), 0U);
}
