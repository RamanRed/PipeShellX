#include "psx/os/tls.hpp"

#include <gtest/gtest.h>

#include "tls_certs.hpp"

#include <span>
#include <string>

using psx::os::Tls;
using psx::os::TlsConfig;
using tls_test::generateSelfSigned;
using tls_test::KeyCert;

namespace {

std::span<const char> bytes(const std::string& s) {
    return std::span<const char>(s.data(), s.size());
}

// Shuttle ciphertext between the two engines until both handshakes finish.
bool driveHandshake(Tls& client, Tls& server) {
    for (int i = 0; i < 20; ++i) {
        auto c = client.handshake();
        if (!c.ok()) {
            return false;
        }
        server.feedEncrypted(bytes(client.takeEncrypted()));
        auto s = server.handshake();
        if (!s.ok()) {
            return false;
        }
        client.feedEncrypted(bytes(server.takeEncrypted()));
        if (client.established() && server.established()) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(TlsTest, MutualHandshakeVerifiesIdentitiesAndTransfersData) {
    const KeyCert serverId = generateSelfSigned("psx://node/server");
    const KeyCert clientId = generateSelfSigned("psx://node/client");

    // Each side presents its own cert and trusts only the other's (as the CA).
    auto server = Tls::create({.certificatePem = serverId.certPem,
                               .privateKeyPem = serverId.keyPem,
                               .caPem = clientId.certPem,
                               .isServer = true});
    auto client = Tls::create({.certificatePem = clientId.certPem,
                               .privateKeyPem = clientId.keyPem,
                               .caPem = serverId.certPem,
                               .isServer = false});
    ASSERT_TRUE(server.ok()) << (server.ok() ? "" : server.error().message());
    ASSERT_TRUE(client.ok()) << (client.ok() ? "" : client.error().message());

    ASSERT_TRUE(driveHandshake(client.value(), server.value()));
    EXPECT_TRUE(client.value().established());
    EXPECT_TRUE(server.value().established());

    // Mutual auth exposed each peer's SAN-URI identity.
    EXPECT_EQ(client.value().peerSanUri(), "psx://node/server");
    EXPECT_EQ(server.value().peerSanUri(), "psx://node/client");

    // Application data both directions, end to end through the memory BIOs.
    ASSERT_TRUE(client.value().write(bytes("hello over tls")).ok());
    server.value().feedEncrypted(bytes(client.value().takeEncrypted()));
    auto atServer = server.value().read();
    ASSERT_TRUE(atServer.ok());
    EXPECT_EQ(atServer.value(), "hello over tls");

    ASSERT_TRUE(server.value().write(bytes("and back")).ok());
    client.value().feedEncrypted(bytes(server.value().takeEncrypted()));
    auto atClient = client.value().read();
    ASSERT_TRUE(atClient.ok());
    EXPECT_EQ(atClient.value(), "and back");
}

TEST(TlsTest, HandshakeFailsWhenThePeerIsNotTrusted) {
    const KeyCert serverId = generateSelfSigned("psx://node/server");
    const KeyCert clientId = generateSelfSigned("psx://node/client");
    const KeyCert strangerId = generateSelfSigned("psx://node/stranger");

    // The server trusts only the stranger, so the real client's cert is rejected.
    auto server = Tls::create({.certificatePem = serverId.certPem,
                               .privateKeyPem = serverId.keyPem,
                               .caPem = strangerId.certPem,
                               .isServer = true});
    auto client = Tls::create({.certificatePem = clientId.certPem,
                               .privateKeyPem = clientId.keyPem,
                               .caPem = serverId.certPem,
                               .isServer = false});
    ASSERT_TRUE(server.ok());
    ASSERT_TRUE(client.ok());
    EXPECT_FALSE(driveHandshake(client.value(), server.value())) << "an untrusted client cert must fail the handshake";
}
