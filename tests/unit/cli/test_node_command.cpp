#include "psx/cli/node_command.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using psx::cli::nodeSubcommand;

namespace {
void touch(const std::string& path, const std::string& content = "x") {
    std::ofstream(path) << content;
}
} // namespace

TEST(NodeCommandTest, RejectsMissingRequiredFlags) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({}, out, err), 2);
    EXPECT_EQ(nodeSubcommand({"--cert", "c", "--key", "k", "--ca", "a"}, out, err), 2);          // no --listen
    EXPECT_EQ(nodeSubcommand({"run", "--key", "k", "--ca", "a", "--listen", "x"}, out, err), 2); // no --cert
}

TEST(NodeCommandTest, RejectsUnreadableFiles) {
    std::ostringstream out, err;
    EXPECT_EQ(nodeSubcommand({"--cert", "/no/such/cert", "--key", "/no/such/key", "--ca", "/no/such/ca", "--listen",
                              "127.0.0.1:17999"},
                             out, err),
              2);
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
