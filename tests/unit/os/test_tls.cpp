#include "psx/os/tls.hpp"

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <span>
#include <string>

using psx::os::Tls;
using psx::os::TlsConfig;

namespace {

struct KeyCert {
    std::string keyPem;
    std::string certPem;
};

std::string bioToString(BIO* bio) {
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(len));
}

// A self-signed certificate (its own CA) carrying `sanUri` as a SAN URI.
KeyCert generateSelfSigned(const std::string& sanUri) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 31536000L);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("psx-test"), -1, -1, 0);
    X509_set_issuer_name(x, name);
    const std::string san = "URI:" + sanUri;
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str());
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
    X509_sign(x, pkey, EVP_sha256());

    KeyCert kc;
    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    kc.keyPem = bioToString(keyBio);
    BIO_free(keyBio);
    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, x);
    kc.certPem = bioToString(certBio);
    BIO_free(certBio);

    X509_free(x);
    EVP_PKEY_free(pkey);
    return kc;
}

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
