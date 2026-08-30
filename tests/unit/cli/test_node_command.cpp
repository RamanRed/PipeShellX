#include "psx/cli/node_command.hpp"

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
using psx::cli::nodeSubcommand;
using psx::os::Tls;

namespace {
void touch(const std::string& path, const std::string& content = "x") {
    std::ofstream(path) << content;
}

std::string slurp(const std::filesystem::path& path) {
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

TEST(NodeCommandTest, RejectsMissingRequiredFlags) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({}, out, err), 2);
    EXPECT_EQ(nodeSubcommand({"--cert", "c", "--key", "k", "--ca", "a"}, out, err), 2);          // no --listen
    EXPECT_EQ(nodeSubcommand({"run", "--key", "k", "--ca", "a", "--listen", "x"}, out, err), 2); // no --cert
}

TEST(NodeCommandTest, RejectsUnknownMissingAndDuplicateOptions) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({"--bogus", "value"}, out, err), 2);
    EXPECT_NE(err.str().find("unknown option '--bogus'"), std::string::npos);

    out.str({});
    err.str({});
    EXPECT_EQ(nodeSubcommand({"--cert"}, out, err), 2);
    EXPECT_NE(err.str().find("--cert requires a value"), std::string::npos);

    out.str({});
    err.str({});
    EXPECT_EQ(nodeSubcommand({"keygen", "--san", "psx://node/one", "--san", "psx://node/two", "--out", "n"}, out, err),
              2);
    EXPECT_NE(err.str().find("--san may only be specified once"), std::string::npos);

    out.str({});
    err.str({});
    EXPECT_EQ(nodeSubcommand({"status", "--control", "/tmp/control", "--bogus", "x"}, out, err), 2);
    EXPECT_NE(err.str().find("unknown option '--bogus'"), std::string::npos);
}

TEST(NodeCommandTest, RejectsUnreadableFiles) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({"--cert", "/no/such/cert", "--key", "/no/such/key", "--ca", "/no/such/ca", "--listen",
                              "127.0.0.1:17999"},
                             out, err),
              2);
}

TEST(NodeCommandTest, RejectsAnUnreadablePolicyBeforeStarting) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({"--cert", "c", "--key", "k", "--ca", "a", "--listen", "127.0.0.1:17999", "--policy",
                              "/no/such/pipeshellx.policy"},
                             out, err),
              2);
    EXPECT_NE(err.str().find("cannot open '/no/such/pipeshellx.policy'"), std::string::npos);
}

TEST(NodeCommandTest, RejectsABadListenAddress) {
    test_support::ScopedTempCwd cwd("node-cli");
    touch("c");
    touch("k");
    touch("a");
    std::ostringstream out, err;
    // Files exist (readable); the listen address is malformed -> exit 2.
    EXPECT_EQ(nodeSubcommand({"--cert", "c", "--key", "k", "--ca", "a", "--listen", "no-port"}, out, err), 2);
    EXPECT_EQ(nodeSubcommand({"--cert", "c", "--key", "k", "--ca", "a", "--listen", "h:0"}, out, err), 2);
    EXPECT_EQ(nodeSubcommand({"--cert", "c", "--key", "k", "--ca", "a", "--listen", "h:99999"}, out, err), 2);
}

TEST(NodeCommandTest, KeygenRejectsMissingFlags) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({"keygen"}, out, err), 2);                     // no --san/--out
    EXPECT_EQ(nodeSubcommand({"keygen", "--san", "psx://n"}, out, err), 2); // no --out
}

// Full enrollment: node keygen (key stays local) -> ca sign the CSR -> the node's
// key and the CA-signed cert authenticate. The private key is never an argument.
TEST(NodeCommandTest, EnrollmentViaKeygenThenCaSignProducesAWorkingIdentity) {
    test_support::ScopedTempCwd cwd("enroll");
    const std::filesystem::path caDir = cwd.path() / "ca";
    const std::filesystem::path node = cwd.path() / "node";
    const std::string san = "psx://node/enrolled-1";
    std::ostringstream out, err;

    ASSERT_EQ(caSubcommand({"init", "--cn", "psx-fleet", "--dir", caDir.string()}, out, err), 0) << err.str();
    // Node side: generate key + CSR.
    ASSERT_EQ(nodeSubcommand({"keygen", "--san", san, "--out", node.string()}, out, err), 0) << err.str();
    ASSERT_TRUE(std::filesystem::exists(node.string() + ".key"));
    ASSERT_TRUE(std::filesystem::exists(node.string() + ".csr"));
    // CA side: sign the CSR into a cert (operator authorises the SAN).
    ASSERT_EQ(caSubcommand({"sign", "--ca", caDir.string(), "--csr", node.string() + ".csr", "--san", san, "--out",
                            node.string() + ".crt"},
                           out, err),
              0)
        << err.str();

    const std::string caCert = slurp(caDir / "ca.crt");
    const std::string cert = slurp(node.string() + ".crt");
    const std::string key = slurp(node.string() + ".key");
    auto server = Tls::create({.certificatePem = cert, .privateKeyPem = key, .caPem = caCert, .isServer = true});
    auto client = Tls::create({.certificatePem = cert, .privateKeyPem = key, .caPem = caCert, .isServer = false});
    ASSERT_TRUE(server.ok() && client.ok());
    EXPECT_TRUE(handshakes(client.value(), server.value()));
    EXPECT_EQ(client.value().peerSanUri(), san);
}

TEST(NodeCommandTest, SystemdUnitHasExecStartAndHardening) {
    std::ostringstream out, err;
    ASSERT_EQ(nodeSubcommand({"systemd-unit", "--cert", "/c", "--key", "/k", "--ca", "/a", "--listen", "0.0.0.0:7433",
                              "--allow", "spiffe://x", "--policy", "/etc/pipeshellx/node.policy", "--exec",
                              "/usr/bin/pipeshellx"},
                             out, err),
              0)
        << err.str();
    const std::string u = out.str();
    EXPECT_NE(u.find("ExecStart=\"/usr/bin/pipeshellx\" \"node\" \"--cert\" \"/c\" \"--key\" \"/k\" \"--ca\" "
                     "\"/a\" \"--listen\" \"0.0.0.0:7433\" \"--allow\" \"spiffe://x\" \"--policy\" "
                     "\"/etc/pipeshellx/node.policy\""),
              std::string::npos);
    EXPECT_NE(u.find("Restart=on-failure"), std::string::npos);
    EXPECT_NE(u.find("NoNewPrivileges=yes"), std::string::npos);
    EXPECT_NE(u.find("RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX"), std::string::npos);
    EXPECT_NE(u.find("WantedBy=multi-user.target"), std::string::npos);
}

TEST(NodeCommandTest, ServiceGeneratorsEscapeSystemdAndXmlValuesAndPreserveControl) {
    std::ostringstream out, err;
    ASSERT_EQ(nodeSubcommand({"systemd-unit", "--cert", "/path/a b", "--key", "/k", "--ca", "/a", "--listen",
                              "0.0.0.0:7433", "--allow", "spiffe://controller/%n$HOME", "--control",
                              "/run/pipeshellx/node.ctl", "--exec", "/opt/Pipe Shell/pipeshellx"},
                             out, err),
              0)
        << err.str();
    EXPECT_NE(out.str().find("\"/opt/Pipe Shell/pipeshellx\""), std::string::npos);
    EXPECT_NE(out.str().find("\"/path/a b\""), std::string::npos);
    EXPECT_NE(out.str().find("spiffe://controller/%%n$$HOME"), std::string::npos);
    EXPECT_NE(out.str().find("\"--control\" \"/run/pipeshellx/node.ctl\""), std::string::npos);
    EXPECT_NE(out.str().find("RuntimeDirectory=pipeshellx\n"), std::string::npos);
    EXPECT_NE(out.str().find("RuntimeDirectoryMode=0750\n"), std::string::npos);
    EXPECT_NE(out.str().find("ReadWritePaths=/run/pipeshellx\n"), std::string::npos);
    EXPECT_NE(out.str().find("UMask=0077\n"), std::string::npos);

    out.str({});
    err.str({});
    ASSERT_EQ(nodeSubcommand({"launchd-plist", "--cert", "/c&<", "--key", "/k", "--ca", "/a", "--listen",
                              "127.0.0.1:7433", "--label", "com.example.<node>&", "--exec", "/opt/a&b"},
                             out, err),
              0)
        << err.str();
    EXPECT_NE(out.str().find("com.example.&lt;node&gt;&amp;"), std::string::npos);
    EXPECT_NE(out.str().find("/opt/a&amp;b"), std::string::npos);
    EXPECT_NE(out.str().find("/c&amp;&lt;"), std::string::npos);
}

TEST(NodeCommandTest, SystemdControlSocketMustUseTheManagedRuntimeDirectory) {
    for (const std::string& path :
         {"/tmp/node.ctl", "/run/node.ctl", "/run/pipeshellx/../node.ctl", "relative/node.ctl"}) {
        std::ostringstream out, err;
        EXPECT_EQ(nodeSubcommand({"systemd-unit", "--cert", "/c", "--key", "/k", "--ca", "/a", "--listen",
                                  "127.0.0.1:7433", "--control", path},
                                 out, err),
                  2)
            << path;
        EXPECT_NE(err.str().find("/run/pipeshellx/FILE"), std::string::npos) << path << ": " << err.str();
    }
}

TEST(NodeCommandTest, LaunchdPlistListsProgramArguments) {
    std::ostringstream out, err;
    ASSERT_EQ(nodeSubcommand({"launchd-plist", "--cert", "/c", "--key", "/k", "--ca", "/a", "--listen",
                              "127.0.0.1:7433", "--exec", "/opt/psx"},
                             out, err),
              0)
        << err.str();
    const std::string p = out.str();
    EXPECT_NE(p.find("<key>Label</key>"), std::string::npos);
    EXPECT_NE(p.find("<string>/opt/psx</string>"), std::string::npos);
    EXPECT_NE(p.find("<string>--listen</string>"), std::string::npos);
    EXPECT_NE(p.find("<string>127.0.0.1:7433</string>"), std::string::npos);
    EXPECT_NE(p.find("<key>RunAtLoad</key>"), std::string::npos);
}

TEST(NodeCommandTest, UnitGeneratorsRejectMissingFlags) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({"systemd-unit", "--cert", "x"}, out, err), 2);
    EXPECT_EQ(nodeSubcommand({"launchd-plist"}, out, err), 2);
}

TEST(NodeCommandTest, StatusRejectsMissingControlAndUnreachableSocket) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({"status"}, out, err), 2); // no --control
    // A control path that isn't a live socket fails cleanly (not a crash).
    EXPECT_EQ(nodeSubcommand({"status", "--control", "/no/such/pipeshellx.sock"}, out, err), 2);
}
