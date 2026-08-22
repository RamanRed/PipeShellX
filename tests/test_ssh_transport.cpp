#include "ssh_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {

ClientEntry keyClient() {
    ClientEntry c;
    c.user = "alice";
    c.host = "h1";
    c.port = 22;
    return c;
}

bool argvHas(const std::vector<std::string>& argv, const std::string& needle) {
    return std::find(argv.begin(), argv.end(), needle) != argv.end();
}

} // namespace

TEST(SshTransportTest, KeyAuthBuildsAPlainSshInvocation) {
    SshTransport transport(SshTransport::Options{});
    SshTransport::Prepared prepared;
    ASSERT_TRUE(transport.prepare(keyClient(), "uptime", prepared).ok());
    EXPECT_TRUE(prepared.failure.empty());
    EXPECT_EQ(prepared.spec.program, "ssh");
    EXPECT_EQ(prepared.spec.argv.front(), "ssh");
    EXPECT_EQ(prepared.spec.argv.back(), "uptime");  // the remote command is the last argv
    EXPECT_TRUE(prepared.spec.extraHandles.empty()); // no password pipe
    EXPECT_FALSE(prepared.passwordPipe.reader.valid());
}

TEST(SshTransportTest, PasswordAuthWrapsInSshpassAndHandsOffAPipe) {
    ClientEntry c = keyClient();
    c.password = "s3cret";
    SshTransport transport(SshTransport::Options{});
    SshTransport::Prepared prepared;
    ASSERT_TRUE(transport.prepare(c, "uptime", prepared).ok());
    EXPECT_TRUE(prepared.failure.empty());
    // sshpass -d <fd> ... wraps the ssh invocation.
    EXPECT_EQ(prepared.spec.program, "sshpass");
    ASSERT_GE(prepared.spec.argv.size(), 3U);
    EXPECT_EQ(prepared.spec.argv[0], "sshpass");
    EXPECT_EQ(prepared.spec.argv[1], "-d");
    EXPECT_EQ(prepared.spec.argv[2], std::to_string(SshTransport::kPasswordFd));
    EXPECT_TRUE(argvHas(prepared.spec.argv, "ssh"));
    // The reader is handed to the child at kPasswordFd; the writer is closed.
    ASSERT_EQ(prepared.spec.extraHandles.size(), 1U);
    EXPECT_EQ(prepared.spec.extraHandles[0].targetFd, SshTransport::kPasswordFd);
    EXPECT_EQ(prepared.spec.extraHandles[0].handle, &prepared.passwordPipe.reader);
    EXPECT_TRUE(prepared.passwordPipe.reader.valid());
    EXPECT_FALSE(prepared.passwordPipe.writer.valid()); // closed after the secret was written
}

TEST(SshTransportTest, AnOverlongPasswordIsAPerHostFailureNotAnError) {
    ClientEntry c = keyClient();
    c.password = std::string(SshTransport::kMaxPasswordBytes, 'x');
    SshTransport transport(SshTransport::Options{});
    SshTransport::Prepared prepared;
    ASSERT_TRUE(transport.prepare(c, "uptime", prepared).ok()) << "must not abort the whole run";
    EXPECT_FALSE(prepared.failure.empty());
    EXPECT_TRUE(prepared.spec.argv.empty()); // nothing to spawn
}

TEST(SshTransportTest, ControlMasterOptionsAppearWhenReuseIsEnabled) {
    SshTransport transport(SshTransport::Options{.controlPath = "/run/psx/cm-%r@%h:%p"});
    SshTransport::Prepared prepared;
    ASSERT_TRUE(transport.prepare(keyClient(), "uptime", prepared).ok());
    const auto& argv = prepared.spec.argv;
    EXPECT_TRUE(argvHas(argv, "ControlMaster=auto"));
    EXPECT_NE(
        std::find_if(argv.begin(), argv.end(), [](const std::string& a) { return a.rfind("ControlPath=", 0) == 0; }),
        argv.end());
}
