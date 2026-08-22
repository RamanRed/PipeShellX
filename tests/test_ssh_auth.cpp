#include <gtest/gtest.h>

#include "ssh_auth.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool contains(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

bool hasOption(const std::vector<std::string>& args, const std::string& option) {
    for (std::size_t index = 0; index + 1 < args.size(); ++index) {
        if (args[index] == "-o" && args[index + 1] == option) {
            return true;
        }
    }
    return false;
}

ClientEntry keyClient() {
    ClientEntry client;
    client.user = "admin";
    client.host = "server.example.com";
    client.port = 2222;
    client.identityFile = "/home/admin/.ssh/id_ed25519";
    client.knownHostsFile = "/srv/fleet/clients.txt.known_hosts";
    return client;
}

ClientEntry passwordClient() {
    ClientEntry client;
    client.user = "admin";
    client.host = "server.example.com";
    client.password = "s3cret $pa ce";
    return client;
}

} // namespace

TEST(SshAuthTest, ResolvesSshFromPathNotHardcodedLocation) {
    const auto args = buildSshBaseArguments(keyClient());
    ASSERT_FALSE(args.empty());
    EXPECT_EQ(args.front(), "ssh");
    EXPECT_FALSE(contains(args, "/usr/bin/ssh"));
}

TEST(SshAuthTest, HardenedHostKeyDefaults) {
    const auto client = keyClient();
    const auto args = buildSshBaseArguments(client);

    EXPECT_TRUE(hasOption(args, "StrictHostKeyChecking=accept-new"));
    EXPECT_FALSE(hasOption(args, "StrictHostKeyChecking=no"));
    EXPECT_TRUE(hasOption(args, "UserKnownHostsFile=\"" + client.knownHostsFile + "\""));
    EXPECT_TRUE(hasOption(args, "ConnectTimeout=5"));
    EXPECT_TRUE(hasOption(args, "ServerAliveInterval=15"));
}

TEST(SshAuthTest, KnownHostsPathIsQuotedForOpenSshOptionParser) {
    // ssh splits an unquoted -o value on whitespace and expands % tokens.
    auto client = keyClient();
    client.knownHostsFile = "/srv/my fleet/100%done/clients.txt.known_hosts";
    EXPECT_TRUE(hasOption(buildSshBaseArguments(client),
                          "UserKnownHostsFile=\"/srv/my fleet/100%%done/clients.txt.known_hosts\""));

    client.knownHostsFile = "/srv/quote\"here/clients.txt.known_hosts";
    EXPECT_THROW(static_cast<void>(buildSshBaseArguments(client)), std::invalid_argument);
    client.knownHostsFile = "/srv/new\nline";
    EXPECT_THROW(static_cast<void>(buildSshBaseArguments(client)), std::invalid_argument);
}

TEST(SshAuthTest, OmitsKnownHostsOptionWhenInventoryHasNone) {
    auto client = keyClient();
    client.knownHostsFile.clear();

    const auto args = buildSshBaseArguments(client);
    for (const auto& arg : args) {
        EXPECT_EQ(arg.rfind("UserKnownHostsFile=", 0), std::string::npos) << arg;
    }
    EXPECT_TRUE(hasOption(args, "StrictHostKeyChecking=accept-new"));
}

TEST(SshAuthTest, BatchModeOnlyWhenNoPasswordIsUsed) {
    EXPECT_TRUE(hasOption(buildSshBaseArguments(keyClient()), "BatchMode=yes"));
    // sshpass must be allowed to answer the password prompt.
    EXPECT_FALSE(hasOption(buildSshBaseArguments(passwordClient()), "BatchMode=yes"));
}

TEST(SshAuthTest, PortAndIdentityAreForwarded) {
    const auto client = keyClient();
    const auto args = buildSshBaseArguments(client);

    ASSERT_GE(args.size(), 8U);
    EXPECT_TRUE(contains(args, "-p"));
    EXPECT_TRUE(contains(args, "2222"));
    EXPECT_TRUE(contains(args, "-i"));
    EXPECT_TRUE(contains(args, client.identityFile));
    EXPECT_EQ(args.back(), client.sshTarget());

    ClientEntry defaults;
    defaults.user = "admin";
    defaults.host = "server.example.com";
    const auto defaultArgs = buildSshBaseArguments(defaults);
    EXPECT_FALSE(contains(defaultArgs, "-p"));
    EXPECT_FALSE(contains(defaultArgs, "-i"));
}

TEST(SshAuthTest, PasswordIsPassedThroughFileDescriptorNeverArgv) {
    const auto client = passwordClient();
    const auto args = buildSshCommandArguments(client, "hostname", 7);

    ASSERT_GE(args.size(), 5U);
    EXPECT_EQ(args[0], "sshpass");
    EXPECT_EQ(args[1], "-d");
    EXPECT_EQ(args[2], "7");
    EXPECT_EQ(args[3], "ssh");
    EXPECT_EQ(args.back(), "hostname");
    EXPECT_FALSE(contains(args, "-p"));
    for (const auto& arg : args) {
        EXPECT_EQ(arg.find(client.password), std::string::npos) << "password leaked into argv: " << arg;
    }
}

TEST(SshAuthTest, PasswordWithoutDescriptorIsRejected) {
    EXPECT_THROW(static_cast<void>(buildSshCommandArguments(passwordClient(), "hostname", -1)), std::invalid_argument);
}

TEST(SshAuthTest, KeyClientNeverInvokesSshpass) {
    const auto args = buildSshCommandArguments(keyClient(), "uptime");
    ASSERT_FALSE(args.empty());
    EXPECT_EQ(args.front(), "ssh");
    EXPECT_FALSE(contains(args, "sshpass"));
    EXPECT_EQ(args.back(), "uptime");

    // A descriptor supplied for a key client is ignored rather than misused.
    const auto ignored = buildSshCommandArguments(keyClient(), "uptime", 9);
    EXPECT_EQ(ignored, args);
}

TEST(SshAuthTest, NoControlMasterByDefault) {
    const auto args = buildSshBaseArguments(keyClient());
    EXPECT_FALSE(hasOption(args, "ControlMaster=auto"));
    for (const auto& arg : args) {
        EXPECT_EQ(arg.rfind("ControlPath=", 0), std::string::npos) << arg;
        EXPECT_EQ(arg.rfind("ControlPersist=", 0), std::string::npos) << arg;
    }
}

TEST(SshAuthTest, ControlMasterOptionsWhenReuseIsEnabled) {
    SshOptions options;
    options.controlPath = "/run/psx/cm-%r@%h:%p";
    options.controlPersistSeconds = 30;
    const auto args = buildSshBaseArguments(keyClient(), options);
    EXPECT_TRUE(hasOption(args, "ControlMaster=auto"));
    EXPECT_TRUE(hasOption(args, "ControlPersist=30s"));
    // Quoted for the option parser, but the %r/%h/%p tokens are preserved
    // (unlike UserKnownHostsFile, whose % is doubled to a literal).
    EXPECT_TRUE(hasOption(args, "ControlPath=\"/run/psx/cm-%r@%h:%p\""));
}

TEST(SshAuthTest, ControlMasterFlowsThroughTheCommandBuilder) {
    SshOptions options;
    options.controlPath = "/run/psx/cm-%r@%h:%p";
    const auto args = buildSshCommandArguments(keyClient(), "uptime", -1, options);
    EXPECT_TRUE(hasOption(args, "ControlMaster=auto"));
    EXPECT_EQ(args.back(), "uptime");
}

TEST(SshAuthTest, DetectsCommonAuthenticationFailures) {
    EXPECT_TRUE(isSshAuthenticationFailure("Permission denied (publickey,password)."));
    EXPECT_TRUE(isSshAuthenticationFailure("No more authentication methods available"));
    EXPECT_TRUE(isSshAuthenticationFailure("AUTHENTICATION FAILED"));
    EXPECT_FALSE(isSshAuthenticationFailure("Connection refused"));
    EXPECT_FALSE(isSshAuthenticationFailure(""));
}

TEST(SshAuthTest, ClassifiesSshFailuresInPrecedenceOrder) {
    EXPECT_EQ(classifySshFailure("ssh: connect to host 127.0.0.1 port 1: Connection refused"), "connection failed");
    EXPECT_EQ(classifySshFailure("ssh: Could not resolve hostname nope.invalid: nodename nor servname provided"),
              "unreachable host");
    EXPECT_EQ(classifySshFailure("nobody@host: Permission denied (publickey)."), "authentication failed");
    EXPECT_EQ(classifySshFailure("Host key verification failed."), "host key verification failed");
    // A changed key also mentions "permission"/"connection" words in its banner; host key wins.
    EXPECT_EQ(classifySshFailure("WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\n"
                                 "Host key verification failed.\nConnection closed by remote host"),
              "host key verification failed");
    EXPECT_FALSE(classifySshFailure("").has_value());
    EXPECT_FALSE(classifySshFailure("uptime: command not found").has_value());
}

TEST(SshAuthTest, DetectsHostKeyVerificationFailures) {
    EXPECT_TRUE(isSshHostKeyFailure("Host key verification failed."));
    EXPECT_TRUE(isSshHostKeyFailure("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                                    "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
                                    "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"));
    EXPECT_FALSE(isSshHostKeyFailure("Permission denied (publickey)."));
    EXPECT_FALSE(isSshHostKeyFailure(""));
}

TEST(SshAuthTest, RetryableFailuresAreTransientTransportOnly) {
    EXPECT_TRUE(isRetryableSshFailure("ssh: connect to host h port 22: Connection refused"));
    EXPECT_TRUE(isRetryableSshFailure("connection timed out"));
    EXPECT_TRUE(isRetryableSshFailure("could not resolve hostname h"));
    EXPECT_TRUE(isRetryableSshFailure("No route to host"));
    // Permanent failures: retrying cannot help.
    EXPECT_FALSE(isRetryableSshFailure("Permission denied (publickey)."));
    EXPECT_FALSE(isRetryableSshFailure("Host key verification failed."));
    EXPECT_FALSE(isRetryableSshFailure("hello from the remote command"));
}
