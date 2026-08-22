#include <gtest/gtest.h>

#include "psx/result.hpp"

#include <memory>
#include <string>

namespace {

psx::Result<int> parsePositive(int value) {
    if (value <= 0) {
        return psx::Error{psx::ErrorClass::InvalidArgument, 22, "parsePositive"};
    }
    return value;
}

psx::Result<void> requirePositive(int value) {
    PSX_TRY(parsePositive(value));
    return {};
}

} // namespace

TEST(ResultTest, CarriesValueOrError) {
    auto good = parsePositive(7);
    ASSERT_TRUE(good.ok());
    EXPECT_TRUE(static_cast<bool>(good));
    EXPECT_EQ(good.value(), 7);

    auto bad = parsePositive(-1);
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().cls, psx::ErrorClass::InvalidArgument);
    EXPECT_EQ(bad.error().code, 22);
    EXPECT_STREQ(bad.error().op, "parsePositive");
}

TEST(ResultTest, VoidResultDefaultsToSuccess) {
    psx::Result<void> success;
    EXPECT_TRUE(success.ok());
    EXPECT_TRUE(requirePositive(3).ok());

    const auto failure = requirePositive(0);
    ASSERT_FALSE(failure.ok());
    EXPECT_EQ(failure.error().cls, psx::ErrorClass::InvalidArgument);
}

TEST(ResultTest, HoldsMoveOnlyValues) {
    psx::Result<std::unique_ptr<int>> result(std::make_unique<int>(42));
    ASSERT_TRUE(result.ok());
    std::unique_ptr<int> taken = std::move(result).value();
    EXPECT_EQ(*taken, 42);
}

TEST(ResultTest, ErrorMessageNamesOperationClassAndCode) {
    const psx::Error error{psx::ErrorClass::NotFound, 2, "open"};
    const std::string message = error.message();
    EXPECT_NE(message.find("open"), std::string::npos) << message;
    EXPECT_NE(message.find("not found"), std::string::npos) << message;
    EXPECT_NE(message.find("2"), std::string::npos) << message;
    EXPECT_STREQ(psx::toString(psx::ErrorClass::WouldBlock), "would block");
    EXPECT_STREQ(psx::toString(psx::ErrorClass::BrokenPipe), "broken pipe");
}
