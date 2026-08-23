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
}
