#include <gtest/gtest.h>

#include "psx/policy/policy.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using psx::policy::Policy;

TEST(PolicyTest, AnEmptyPolicyAllowsEverything) {
    const Policy policy;
    EXPECT_FALSE(policy.validate({"anything", "--flag", "arg"}).has_value());
    EXPECT_FALSE(policy.validate({"rm", "-rf", "/"}).has_value());
    EXPECT_TRUE(policy.validate({}).has_value()) << "an empty command is always rejected";
}

TEST(PolicyTest, ParsesAllowedCommandsAndRejectsOthers) {
    const auto policy = Policy::parse(R"(
# demo policy
allow ls
allow cat
allow echo
)");
    EXPECT_FALSE(policy.validate({"ls", "-la"}).has_value());
    EXPECT_FALSE(policy.validate({"echo", "hi"}).has_value());
    auto denied = policy.validate({"rm", "-rf", "/"});
    ASSERT_TRUE(denied.has_value());
    EXPECT_NE(denied->find("rm"), std::string::npos) << *denied;
    EXPECT_NE(denied->find("not allowed"), std::string::npos) << *denied;
}

TEST(PolicyTest, MaxArgsIsEnforced) {
    const auto policy = Policy::parse("allow echo\nmax-args 3\n");
    EXPECT_FALSE(policy.validate({"echo", "a", "b"}).has_value()); // 3 tokens ok
    auto tooMany = policy.validate({"echo", "a", "b", "c"});
    ASSERT_TRUE(tooMany.has_value());
    EXPECT_NE(tooMany->find("too many"), std::string::npos) << *tooMany;
}

TEST(PolicyTest, ExplicitPathsAreRejectedWhenAnAllowlistIsSet) {
    const auto policy = Policy::parse("allow ls\n");
    auto denied = policy.validate({"/bin/ls"});
    ASSERT_TRUE(denied.has_value());
    EXPECT_NE(denied->find("path"), std::string::npos) << *denied;
    // With no allowlist, an explicit path is fine (operator's choice).
    EXPECT_FALSE(Policy{}.validate({"/bin/ls"}).has_value());
}

TEST(PolicyTest, ShellMetacharactersAreRejectedByDefault) {
    const auto policy = Policy::parse("allow echo\n");
    for (const std::string bad : {"a;b", "a|b", "a&b", "a$b", "a`b", "a>b", "a<b"}) {
        auto denied = policy.validate({"echo", bad});
        ASSERT_TRUE(denied.has_value()) << "should reject argument: " << bad;
        EXPECT_NE(denied->find("unsafe"), std::string::npos) << *denied;
    }
    EXPECT_FALSE(policy.validate({"echo", "safe-arg_1.txt"}).has_value());
}

TEST(PolicyTest, AllowShellDisablesTheMetacharacterCheck) {
    const auto policy = Policy::parse("allow sh\nallow-shell-metacharacters\n");
    EXPECT_FALSE(policy.validate({"sh", "-c", "a | b > c"}).has_value());
}

TEST(PolicyTest, MalformedDirectivesAreRejectedWithLineNumbers) {
    EXPECT_THROW(static_cast<void>(Policy::parse("bogus ls\n")), std::runtime_error);
    EXPECT_THROW(static_cast<void>(Policy::parse("max-args notanumber\n")), std::runtime_error);
    EXPECT_THROW(static_cast<void>(Policy::parse("allow\n")), std::runtime_error); // missing argument
    try {
        static_cast<void>(Policy::parse("allow ls\nbogus x\n"));
    } catch (const std::exception& ex) {
        EXPECT_NE(std::string(ex.what()).find("2"), std::string::npos) << ex.what();
    }
}

TEST(PolicyTest, CommentsAndBlankLinesIgnored) {
    const auto policy = Policy::parse("\n  # comment\nallow ls\n\n  ; also comment\n");
    EXPECT_FALSE(policy.validate({"ls"}).has_value());
    EXPECT_TRUE(policy.validate({"cat"}).has_value());
}
