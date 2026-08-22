#include "psx/transport/tls_stream.hpp"

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/runtime/reactor.hpp"
#include "tls_certs.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <thread>

using psx::os::Socket;
using psx::os::Tls;
using psx::runtime::Reactor;
using psx::transport::TlsStream;

namespace {

std::span<const char> bytes(const std::string& s) {
    return std::span<const char>(s.data(), s.size());
}

// Establishes a loopback TCP pair synchronously (both non-blocking).
struct TcpPair {
    Socket client;
    Socket server;
};

TcpPair connectLoopback() {
    auto listener = Socket::listen("127.0.0.1", 0);
    EXPECT_TRUE(listener.ok());
    const auto port = listener.value().localPort();
    EXPECT_TRUE(port.ok());
    auto client = Socket::connect("127.0.0.1", port.value());
    EXPECT_TRUE(client.ok());
    Socket server;
    for (int i = 0; i < 2000 && !server.valid(); ++i) {
        auto accepted = listener.value().accept();
        if (accepted.ok()) {
            server = std::move(accepted.value());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return {std::move(client.value()), std::move(server)};
}

} // namespace

TEST(TlsStreamTest, HandshakeAndBidirectionalEchoOverTheReactor) {
    TcpPair tcp = connectLoopback();
    ASSERT_TRUE(tcp.server.valid());

    const auto serverId = tls_test::generateSelfSigned("psx://node/server");
    const auto clientId = tls_test::generateSelfSigned("psx://node/client");
    auto serverTls = Tls::create({.certificatePem = serverId.certPem,
                                  .privateKeyPem = serverId.keyPem,
                                  .caPem = clientId.certPem,
                                  .isServer = true});
    auto clientTls = Tls::create({.certificatePem = clientId.certPem,
                                  .privateKeyPem = clientId.keyPem,
                                  .caPem = serverId.certPem,
                                  .isServer = false});
    ASSERT_TRUE(serverTls.ok());
    ASSERT_TRUE(clientTls.ok());

    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor.ok());
    Reactor& r = *reactor.value();

    std::string clientGot;
    std::string serverGot;
    bool clientReady = false;
    bool serverReady = false;
    bool errored = false;

    TlsStream* clientPtr = nullptr;
    TlsStream* serverPtr = nullptr;

    TlsStream server(r, std::move(tcp.server), std::move(serverTls.value()),
                     {.onReady = [&] { serverReady = true; },
                      .onData =
                          [&](std::string_view d) {
                              serverGot.append(d);
                              if (serverGot == "ping") {
                                  serverPtr->send(bytes("pong"));
                              }
                          },
                      .onError =
                          [&](psx::Error) {
                              errored = true;
                              r.stop();
                          }});
    TlsStream client(r, std::move(tcp.client), std::move(clientTls.value()),
                     {.onReady =
                          [&] {
                              clientReady = true;
                              clientPtr->send(bytes("ping")); // kick off once secured
                          },
                      .onData =
                          [&](std::string_view d) {
                              clientGot.append(d);
                              if (clientGot == "pong") {
                                  r.stop();
                              }
                          },
                      .onError =
                          [&](psx::Error) {
                              errored = true;
                              r.stop();
                          }});
    clientPtr = &client;
    serverPtr = &server;

    ASSERT_TRUE(server.start().ok());
    ASSERT_TRUE(client.start().ok());
    r.after(std::chrono::seconds(3), [&] { r.stop(); }); // safety net
    ASSERT_TRUE(r.run().ok());

    EXPECT_FALSE(errored);
    EXPECT_TRUE(clientReady);
    EXPECT_TRUE(serverReady);
    EXPECT_EQ(serverGot, "ping");
    EXPECT_EQ(clientGot, "pong");
    // Mutual identity surfaced through the real handshake.
    EXPECT_EQ(client.tls().peerSanUri(), "psx://node/server");
    EXPECT_EQ(server.tls().peerSanUri(), "psx://node/client");
}
