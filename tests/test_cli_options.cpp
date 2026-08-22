#include <gtest/gtest.h>

#include "cli_options.hpp"

#include <string>
#include <vector>

TEST(CliOptionsTest, DefaultsAreQuietFileLogging) {
    const auto options = parseCliOptions({});
    EXPECT_FALSE(options.verbose);
    EXPECT_FALSE(options.showVersion);
    EXPECT_FALSE(options.showHelp);
    EXPECT_TRUE(options.logFile.empty());
}

TEST(CliOptionsTest, ParsesVerboseFlags) {
    EXPECT_TRUE(parseCliOptions({"--verbose"}).verbose);
    EXPECT_TRUE(parseCliOptions({"-v"}).verbose);
}

TEST(CliOptionsTest, ParsesLogFileInBothForms) {
    EXPECT_EQ(parseCliOptions({"--log-file", "/tmp/a.log"}).logFile, "/tmp/a.log");
    EXPECT_EQ(parseCliOptions({"--log-file=/tmp/b.log"}).logFile, "/tmp/b.log");
    EXPECT_EQ(parseCliOptions({"--log-file=/tmp/with spaces.log"}).logFile, "/tmp/with spaces.log");
}

TEST(CliOptionsTest, ParsesVersionAndHelp) {
    EXPECT_TRUE(parseCliOptions({"--version"}).showVersion);
    EXPECT_TRUE(parseCliOptions({"--help"}).showHelp);
    EXPECT_TRUE(parseCliOptions({"-h"}).showHelp);
}

TEST(CliOptionsTest, RejectsUnknownOrIncompleteArguments) {
    EXPECT_THROW(static_cast<void>(parseCliOptions({"--bogus"})), CliParseError);
    EXPECT_THROW(static_cast<void>(parseCliOptions({"--log-file"})), CliParseError);
    EXPECT_THROW(static_cast<void>(parseCliOptions({"--log-file="})), CliParseError);
    EXPECT_THROW(static_cast<void>(parseCliOptions({"positional"})), CliParseError);
}

TEST(CliOptionsTest, UsageAndVersionTextAreNonEmpty) {
    EXPECT_NE(cliUsageText().find("--verbose"), std::string::npos);
    EXPECT_NE(cliUsageText().find("--log-file"), std::string::npos);
    EXPECT_NE(cliVersionText().find("PipeShellX"), std::string::npos);
}
