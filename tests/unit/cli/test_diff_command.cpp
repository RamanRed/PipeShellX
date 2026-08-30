#include "psx/cli/diff_command.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#if defined(PIPESHELLX_HAVE_TLS)
#include "psx/ca/certificate_authority.hpp"
#include "psx/os/socket.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/node_server.hpp"

#include "test_support.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#endif

using psx::cli::diffSubcommand;

TEST(DiffCommandTest, RejectsMissingCommand) {
    std::ostringstream out, err;
    EXPECT_EQ(diffSubcommand({"-g", "fleet", "--cert", "c", "--key", "k", "--ca", "a"}, out, err), 2);
    EXPECT_NE(err.str().find("Usage"), std::string::npos);
}

TEST(DiffCommandTest, RejectsMissingCerts) {
    std::ostringstream out, err;
    EXPECT_EQ(diffSubcommand({"-g", "fleet", "--", "echo", "x"}, out, err), 2);
    EXPECT_NE(err.str().find("--cert"), std::string::npos);
}

TEST(DiffCommandTest, RejectsUnreadableCerts) {
    std::ostringstream out, err;
    EXPECT_EQ(
        diffSubcommand({"--cert", "/no/such", "--key", "/no/such", "--ca", "/no/such", "--", "echo", "x"}, out, err),
        2);
    EXPECT_NE(err.str().find("cannot read"), std::string::npos);
}

TEST(DiffCommandTest, RejectsUnknownOptionsAndPositionalsBeforeTheCommandSeparator) {
    for (const std::vector<std::string>& args :
         std::vector<std::vector<std::string>>{{"--bogus", "--", "true"}, {"unexpected", "--", "true"}}) {
        SCOPED_TRACE(::testing::PrintToString(args));
        std::ostringstream out, err;
        EXPECT_EQ(diffSubcommand(args, out, err), 2);
        EXPECT_NE(err.str().find("unknown argument"), std::string::npos) << err.str();
    }
}

TEST(DiffCommandTest, RejectsMissingOptionValues) {
    for (const std::string& option :
         std::vector<std::string>{"-i", "--cert", "--key", "--ca", "--native-port", "-g", "-t", "-H"}) {
        SCOPED_TRACE(option);
        std::ostringstream out, err;
        EXPECT_EQ(diffSubcommand({option}, out, err), 2);
        EXPECT_NE(err.str().find("requires a value"), std::string::npos) << err.str();
    }
}

TEST(DiffCommandTest, NativePortIsAStrictIntegerInTheTcpPortRange) {
    for (const std::string& value : std::vector<std::string>{"", "0", "-1", "65536", "12x", "+12", "1.5"}) {
        SCOPED_TRACE(value);
        std::ostringstream out, err;
        EXPECT_EQ(diffSubcommand({"--native-port", value, "--", "true"}, out, err), 2);
        EXPECT_NE(err.str().find("--native-port must be 1..65535"), std::string::npos) << err.str();
    }

    std::ostringstream out, err;
    EXPECT_EQ(diffSubcommand({"--native-port", "65535", "--", "true"}, out, err), 2);
    EXPECT_EQ(err.str().find("--native-port must be"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("--cert"), std::string::npos) << err.str();
}

TEST(DiffCommandTest, SelectorsAreMutuallyExclusive) {
    for (const std::vector<std::string>& selectors : std::vector<std::vector<std::string>>{
             {"-g", "web", "-t", "canary"}, {"-g", "web", "-H", "host-a"}, {"-t", "canary", "-H", "host-a"}}) {
        SCOPED_TRACE(::testing::PrintToString(selectors));
        std::vector<std::string> args = selectors;
        args.insert(args.end(), {"--", "true"});
        std::ostringstream out, err;
        EXPECT_EQ(diffSubcommand(args, out, err), 2);
        EXPECT_NE(err.str().find("mutually exclusive"), std::string::npos) << err.str();
    }
}

#if defined(PIPESHELLX_HAVE_TLS)
namespace {

template <typename T> T requireValue(psx::Result<T> result, const char* operation) {
    if (!result.ok()) {
        throw std::runtime_error(std::string(operation) + ": " + result.error().message());
    }
    return std::move(result).value();
}

class DiffNode {
public:
    DiffNode()
        : ca_(requireValue(psx::ca::CertificateAuthority::create("diff-test"), "create CA")),
          node_(requireValue(ca_.issue("psx://node/1"), "issue node identity")),
          controller_(requireValue(ca_.issue("psx://controller"), "issue controller identity")),
          caPem_(ca_.certificatePem()), reactor_(requireValue(psx::runtime::Reactor::create(), "create reactor")) {
        auto listener = requireValue(psx::os::Socket::listen("0.0.0.0", 0), "listen");
        port_ = requireValue(listener.localPort(), "local port");
        server_ = std::make_unique<psx::transport::NodeServer>(
            *reactor_, std::move(listener),
            psx::os::TlsConfig{.certificatePem = node_.certificatePem,
                               .privateKeyPem = node_.privateKeyPem,
                               .caPem = caPem_,
                               .crlPem = {},
                               .isServer = true},
            [](std::string_view san) { return san == "psx://controller"; });
        const auto started = server_->start();
        if (!started.ok()) {
            throw std::runtime_error("start node: " + started.error().message());
        }
        thread_ = std::thread([this] { runOk_.store(reactor_->run().ok(), std::memory_order_relaxed); });
    }

    ~DiffNode() { stop(); }
    DiffNode(const DiffNode&) = delete;
    DiffNode& operator=(const DiffNode&) = delete;

    std::uint16_t port() const noexcept { return port_; }
    const psx::ca::Identity& controller() const noexcept { return controller_; }
    const std::string& caPem() const noexcept { return caPem_; }

    void stop() {
        if (!stopped_) {
            stopped_ = true;
            reactor_->stop();
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }

    bool runOk() const noexcept { return runOk_.load(std::memory_order_relaxed); }

private:
    psx::ca::CertificateAuthority ca_;
    psx::ca::Identity node_;
    psx::ca::Identity controller_;
    std::string caPem_;
    std::unique_ptr<psx::runtime::Reactor> reactor_;
    std::unique_ptr<psx::transport::NodeServer> server_;
    std::thread thread_;
    std::atomic<bool> runOk_{true};
    std::uint16_t port_ = 0;
    bool stopped_ = false;
};

std::vector<std::string> diffArgs(const DiffNode& node, std::vector<std::string> command, bool json = false) {
    std::ofstream("fleet.ini") << "[fleet]\n127.0.0.1 san=psx://node/1 native_port=" << node.port() << "\n";
    std::ofstream("controller.crt", std::ios::binary) << node.controller().certificatePem;
    std::ofstream("controller.key", std::ios::binary) << node.controller().privateKeyPem;
    std::ofstream("ca.crt", std::ios::binary) << node.caPem();

    std::vector<std::string> args{"-i",    "fleet.ini",      "-g",   "fleet", "--cert", "controller.crt",
                                  "--key", "controller.key", "--ca", "ca.crt"};
    if (json) {
        args.push_back("--json");
    }
    args.push_back("--");
    args.insert(args.end(), std::make_move_iterator(command.begin()), std::make_move_iterator(command.end()));
    return args;
}

} // namespace

TEST(DiffCommandTest, NonZeroRemoteStageExitIsAHostFailure) {
    test_support::ScopedTempCwd cwd("diff-nonzero");
    DiffNode node;
    std::ostringstream out, err;

    const int code =
        diffSubcommand(diffArgs(node, {"/bin/sh", "-c", "printf ordinary; printf boom >&2; exit 7"}), out, err);

    node.stop();
    EXPECT_TRUE(node.runOk());
    EXPECT_EQ(code, 2) << out.str();
    EXPECT_NE(err.str().find("host failed"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("exit 7"), std::string::npos) << err.str();
}

TEST(DiffCommandTest, ConsensusUsesStdoutWithoutStderr) {
    test_support::ScopedTempCwd cwd("diff-channels");
    DiffNode node;
    std::ostringstream out, err;

    const int code = diffSubcommand(
        diffArgs(node, {"/bin/sh", "-c", "printf consensus-value; printf diagnostic-only >&2"}, true), out, err);

    node.stop();
    EXPECT_TRUE(node.runOk());
    EXPECT_EQ(code, 0) << err.str();
    EXPECT_NE(out.str().find("consensus-value"), std::string::npos) << out.str();
    EXPECT_EQ(out.str().find("diagnostic-only"), std::string::npos) << out.str();
}
#endif
