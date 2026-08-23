#include "psx/cli/diff_command.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

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
