#include "psx/ca/certificate_authority.hpp"

#include "psx/os/tls.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string>

using psx::ca::CertificateAuthority;
using psx::ca::Identity;
using psx::os::Tls;

namespace {

std::span<const char> bytes(const std::string& s) {
    return std::span<const char>(s.data(), s.size());
}

// Drives an in-memory mTLS handshake between two engines; true if both establish.
bool handshakes(Tls& client, Tls& server) {
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

TEST(CertificateAuthorityTest, IssuedLeavesChainToTheCaAndMutuallyAuthenticate) {
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok()) << (ca.ok() ? "" : ca.error().message());

    auto serverId = ca.value().issue("psx://node/1");
    auto clientId = ca.value().issue("psx://controller");
    ASSERT_TRUE(serverId.ok());
    ASSERT_TRUE(clientId.ok());

    // Each endpoint presents its CA-signed leaf and trusts the CA certificate.
    const std::string caCert = ca.value().certificatePem();
    auto server = Tls::create({.certificatePem = serverId.value().certificatePem,
                               .privateKeyPem = serverId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = true});
    auto client = Tls::create({.certificatePem = clientId.value().certificatePem,
                               .privateKeyPem = clientId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());

    ASSERT_TRUE(handshakes(client.value(), server.value())) << "leaves must verify against the CA";
    EXPECT_EQ(client.value().peerSanUri(), "psx://node/1");
    EXPECT_EQ(server.value().peerSanUri(), "psx://controller");
}

TEST(CertificateAuthorityTest, ReloadsFromPemAndKeepsIssuing) {
    auto original = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(original.ok());
    const std::string keyPem = original.value().privateKeyPem();
    const std::string certPem = original.value().certificatePem();

    auto reloaded = CertificateAuthority::load(keyPem, certPem);
    ASSERT_TRUE(reloaded.ok()) << (reloaded.ok() ? "" : reloaded.error().message());

    // A leaf issued by the reloaded CA still verifies against the same CA cert.
    auto leaf = reloaded.value().issue("psx://node/2");
    ASSERT_TRUE(leaf.ok());
    auto server = Tls::create({.certificatePem = leaf.value().certificatePem,
                               .privateKeyPem = leaf.value().privateKeyPem,
                               .caPem = certPem,
                               .isServer = true});
    auto client = Tls::create({.certificatePem = leaf.value().certificatePem,
                               .privateKeyPem = leaf.value().privateKeyPem,
                               .caPem = certPem,
                               .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());
    EXPECT_TRUE(handshakes(client.value(), server.value()));
}

TEST(CertificateAuthorityTest, ALeafFromAnotherCaIsRejected) {
    auto ca = CertificateAuthority::create("psx-fleet");
    auto other = CertificateAuthority::create("rogue-fleet");
    ASSERT_TRUE(ca.ok() && other.ok());

    auto serverId = ca.value().issue("psx://node/1");
    auto rogueClient = other.value().issue("psx://controller"); // signed by the WRONG CA
    ASSERT_TRUE(serverId.ok() && rogueClient.ok());

    // The server trusts only its own CA, so the rogue client's cert fails to verify.
    auto server = Tls::create({.certificatePem = serverId.value().certificatePem,
                               .privateKeyPem = serverId.value().privateKeyPem,
                               .caPem = ca.value().certificatePem(),
                               .isServer = true});
    auto client = Tls::create({.certificatePem = rogueClient.value().certificatePem,
                               .privateKeyPem = rogueClient.value().privateKeyPem,
                               .caPem = ca.value().certificatePem(),
                               .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());
    EXPECT_FALSE(handshakes(client.value(), server.value())) << "a cert from another CA must not verify";
}

TEST(CertificateAuthorityTest, RevokedLeafFailsWhenTheCrlIsEnforced) {
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto serverId = ca.value().issue("psx://node/1");
    auto clientId = ca.value().issue("psx://controller");
    ASSERT_TRUE(serverId.ok() && clientId.ok());

    // Revoke the client by its serial and publish a CRL.
    auto serial = CertificateAuthority::serialHex(clientId.value().certificatePem);
    ASSERT_TRUE(serial.ok()) << (serial.ok() ? "" : serial.error().message());
    auto crl = ca.value().issueCrl({serial.value()});
    ASSERT_TRUE(crl.ok()) << (crl.ok() ? "" : crl.error().message());

    const std::string caCert = ca.value().certificatePem();
    auto server = Tls::create({.certificatePem = serverId.value().certificatePem,
                               .privateKeyPem = serverId.value().privateKeyPem,
                               .caPem = caCert,
                               .crlPem = crl.value(), // the server enforces the CRL
                               .isServer = true});
    auto client = Tls::create({.certificatePem = clientId.value().certificatePem,
                               .privateKeyPem = clientId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());
    EXPECT_FALSE(handshakes(client.value(), server.value())) << "a revoked client cert must be rejected";
}

TEST(CertificateAuthorityTest, NonRevokedLeafStillPassesWithTheCrlEnforced) {
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto serverId = ca.value().issue("psx://node/1");
    auto clientId = ca.value().issue("psx://controller");
    ASSERT_TRUE(serverId.ok() && clientId.ok());

    // A CRL that revokes some other serial: the client is not on it.
    auto crl = ca.value().issueCrl({"0123456789ABCDEF"});
    ASSERT_TRUE(crl.ok());

    const std::string caCert = ca.value().certificatePem();
    auto server = Tls::create({.certificatePem = serverId.value().certificatePem,
                               .privateKeyPem = serverId.value().privateKeyPem,
                               .caPem = caCert,
                               .crlPem = crl.value(),
                               .isServer = true});
    auto client = Tls::create({.certificatePem = clientId.value().certificatePem,
                               .privateKeyPem = clientId.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());
    EXPECT_TRUE(handshakes(client.value(), server.value())) << "a cert absent from the CRL must still verify";
}

TEST(CertificateAuthorityTest, SignsACsrIntoAUsableIdentity) {
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto kc = CertificateAuthority::generateCsr("psx://node/enrolled");
    ASSERT_TRUE(kc.ok()) << (kc.ok() ? "" : kc.error().message());
    auto cert = ca.value().signCsr(kc.value().csrPem, "psx://node/enrolled");
    ASSERT_TRUE(cert.ok()) << (cert.ok() ? "" : cert.error().message());

    // The node's own (never-transmitted) key + the CA-signed cert authenticate.
    const std::string caCert = ca.value().certificatePem();
    auto server = Tls::create(
        {.certificatePem = cert.value(), .privateKeyPem = kc.value().privateKeyPem, .caPem = caCert, .isServer = true});
    auto client = Tls::create({.certificatePem = cert.value(),
                               .privateKeyPem = kc.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());
    EXPECT_TRUE(handshakes(client.value(), server.value()));
    EXPECT_EQ(client.value().peerSanUri(), "psx://node/enrolled");
}

TEST(CertificateAuthorityTest, SignCsrUsesTheOperatorSanNotTheRequestedOne) {
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    // The CSR requests one identity; the operator authorises a different one.
    auto kc = CertificateAuthority::generateCsr("psx://node/attacker-requested");
    ASSERT_TRUE(kc.ok());
    auto cert = ca.value().signCsr(kc.value().csrPem, "psx://node/authorized");
    ASSERT_TRUE(cert.ok());

    const std::string caCert = ca.value().certificatePem();
    auto server = Tls::create(
        {.certificatePem = cert.value(), .privateKeyPem = kc.value().privateKeyPem, .caPem = caCert, .isServer = true});
    auto client = Tls::create({.certificatePem = cert.value(),
                               .privateKeyPem = kc.value().privateKeyPem,
                               .caPem = caCert,
                               .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());
    EXPECT_TRUE(handshakes(client.value(), server.value()));
    EXPECT_EQ(client.value().peerSanUri(), "psx://node/authorized") << "the CA must stamp its own SAN, not the CSR's";
}

TEST(CertificateAuthorityTest, SignCsrRejectsATamperedCsr) {
    auto ca = CertificateAuthority::create("psx-fleet");
    ASSERT_TRUE(ca.ok());
    auto kc = CertificateAuthority::generateCsr("psx://node/x");
    ASSERT_TRUE(kc.ok());

    // Corrupt a byte inside the base64 body: parsing or signature verification fails.
    std::string csr = kc.value().csrPem;
    const std::size_t mid = csr.size() / 2;
    csr[mid] = (csr[mid] == 'A') ? 'B' : 'A';
    EXPECT_FALSE(ca.value().signCsr(csr, "psx://node/x").ok()) << "a tampered CSR must be refused";
}
