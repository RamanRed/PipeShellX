#include "psx/cli/ca_command.hpp"

#include "psx/os/tls.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>

using psx::cli::caSubcommand;
using psx::os::Tls;

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::span<const char> bytes(const std::string& s) {
    return std::span<const char>(s.data(), s.size());
}

bool handshakes(Tls& client, Tls& server) {
    for (int i = 0; i < 20; ++i) {
        if (!client.handshake().ok()) {
            return false;
        }
        server.feedEncrypted(bytes(client.takeEncrypted()));
        if (!server.handshake().ok()) {
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

TEST(CaCommandTest, InitThenIssueProducesAWorkingCaSignedIdentity) {
    test_support::ScopedTempCwd cwd("ca-cli");
    const std::filesystem::path caDir = cwd.path() / "ca";
    const std::filesystem::path leaf = cwd.path() / "node1";
    std::ostringstream out, err;

    ASSERT_EQ(caSubcommand({"init", "--cn", "psx-fleet", "--dir", caDir.string()}, out, err), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(caDir / "ca.key"));
    EXPECT_TRUE(std::filesystem::exists(caDir / "ca.crt"));

    ASSERT_EQ(
        caSubcommand({"issue", "--san", "psx://node/1", "--ca", caDir.string(), "--out", leaf.string()}, out, err), 0)
        << err.str();
    EXPECT_TRUE(std::filesystem::exists(leaf.string() + ".key"));
    EXPECT_TRUE(std::filesystem::exists(leaf.string() + ".crt"));

    // The issued leaf verifies against the CA cert via a real mTLS handshake.
    const std::string caCert = readFile(caDir / "ca.crt");
    const std::string leafCert = readFile(leaf.string() + ".crt");
    const std::string leafKey = readFile(leaf.string() + ".key");
    auto server =
        Tls::create({.certificatePem = leafCert, .privateKeyPem = leafKey, .caPem = caCert, .isServer = true});
    auto client =
        Tls::create({.certificatePem = leafCert, .privateKeyPem = leafKey, .caPem = caCert, .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());
    EXPECT_TRUE(handshakes(client.value(), server.value()));
    EXPECT_EQ(client.value().peerSanUri(), "psx://node/1");
}

TEST(CaCommandTest, ReportsUsageErrors) {
    std::ostringstream out, err;
    EXPECT_EQ(caSubcommand({}, out, err), 2);                      // no action
    EXPECT_EQ(caSubcommand({"bogus"}, out, err), 2);               // unknown action
    EXPECT_EQ(caSubcommand({"init", "--cn", "x"}, out, err), 2);   // missing --dir
    EXPECT_EQ(caSubcommand({"issue", "--san", "u"}, out, err), 2); // missing --ca/--out
    EXPECT_EQ(caSubcommand({"revoke", "--ca", "x"}, out, err), 2); // missing --cert/--serial
}

TEST(CaCommandTest, RevokeProducesACrlThatRejectsTheRevokedLeaf) {
    test_support::ScopedTempCwd cwd("ca-revoke");
    const std::filesystem::path caDir = cwd.path() / "ca";
    const std::filesystem::path node = cwd.path() / "node1";
    const std::filesystem::path ctl = cwd.path() / "ctl";
    std::ostringstream out, err;

    ASSERT_EQ(caSubcommand({"init", "--cn", "psx-fleet", "--dir", caDir.string()}, out, err), 0) << err.str();
    ASSERT_EQ(
        caSubcommand({"issue", "--san", "psx://node/1", "--ca", caDir.string(), "--out", node.string()}, out, err), 0)
        << err.str();
    ASSERT_EQ(
        caSubcommand({"issue", "--san", "psx://controller", "--ca", caDir.string(), "--out", ctl.string()}, out, err),
        0)
        << err.str();

    // Revoke the controller by its certificate; a CRL and index appear.
    ASSERT_EQ(caSubcommand({"revoke", "--ca", caDir.string(), "--cert", ctl.string() + ".crt"}, out, err), 0)
        << err.str();
    ASSERT_TRUE(std::filesystem::exists(caDir / "crl.pem"));
    ASSERT_TRUE(std::filesystem::exists(caDir / "revoked.txt"));

    const std::string caCert = readFile(caDir / "ca.crt");
    const std::string crl = readFile(caDir / "crl.pem");
    const std::string nodeCert = readFile(node.string() + ".crt");
    const std::string nodeKey = readFile(node.string() + ".key");

    // The node enforces the CRL: the revoked controller is rejected.
    {
        auto server = Tls::create(
            {.certificatePem = nodeCert, .privateKeyPem = nodeKey, .caPem = caCert, .crlPem = crl, .isServer = true});
        auto client = Tls::create({.certificatePem = readFile(ctl.string() + ".crt"),
                                   .privateKeyPem = readFile(ctl.string() + ".key"),
                                   .caPem = caCert,
                                   .isServer = false});
        ASSERT_TRUE(server.ok() && client.ok());
        EXPECT_FALSE(handshakes(client.value(), server.value())) << "the revoked controller must be rejected";
    }

    // A freshly issued, non-revoked controller still connects under the same CRL.
    const std::filesystem::path ctl2 = cwd.path() / "ctl2";
    ASSERT_EQ(caSubcommand({"issue", "--san", "psx://controller/2", "--ca", caDir.string(), "--out", ctl2.string()},
                           out, err),
              0);
    {
        auto server = Tls::create(
            {.certificatePem = nodeCert, .privateKeyPem = nodeKey, .caPem = caCert, .crlPem = crl, .isServer = true});
        auto client = Tls::create({.certificatePem = readFile(ctl2.string() + ".crt"),
                                   .privateKeyPem = readFile(ctl2.string() + ".key"),
                                   .caPem = caCert,
                                   .isServer = false});
        ASSERT_TRUE(server.ok() && client.ok());
        EXPECT_TRUE(handshakes(client.value(), server.value())) << "a non-revoked controller must still connect";
    }
}
