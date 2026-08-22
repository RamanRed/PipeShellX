#include <gtest/gtest.h>

#include "logger.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const std::regex kLineFormat(
    R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] \[(DEBUG|INFO|ERROR)\] \[pid=\d+\] \[session=[^\]]+\] \[client=[^\]]+\] \[command=[^\]]+\] .*$)");

std::vector<std::string> readLines(const std::filesystem::path& file) {
    std::ifstream input(file);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& logger = Logger::getInstance();
        logger.setLevel(LogLevel::INFO);
        logger.setConsoleMirror(false);
        logger.setRotation(0, 5); // no rotation unless a test opts in
        ASSERT_TRUE(logger.setLogFile(logFile_.string()));
    }

    void TearDown() override {
        auto& logger = Logger::getInstance();
        logger.setConsoleMirror(false);
        logger.setLevel(LogLevel::INFO);
        logger.setRotation(0, 5);
        logger.setLogFile(""); // release the file before the temp dir goes away
    }

    test_support::ScopedTempCwd cwd_{"logger"};
    std::filesystem::path logFile_ = cwd_.path() / "test.log";
};

} // namespace

TEST_F(LoggerTest, RotatesWhenTheFileExceedsMaxBytes) {
    namespace fs = std::filesystem;
    auto& logger = Logger::getInstance();
    logger.setRotation(300, 2); // rotate past 300 bytes, keep 2 old generations
    for (int i = 0; i < 40; ++i) {
        logger.log(LogLevel::INFO, "rotation-line-" + std::to_string(i));
    }
    EXPECT_TRUE(fs::exists(logFile_));
    EXPECT_TRUE(fs::exists(logFile_.string() + ".1"));
    EXPECT_TRUE(fs::exists(logFile_.string() + ".2"));
    EXPECT_FALSE(fs::exists(logFile_.string() + ".3")) << "keep=2: no third generation";
    // The newest line is in the current file, or in .1 if the last write just
    // triggered a rotation (rotate-after-write leaves the current file fresh).
    auto current = readLines(logFile_);
    auto gen1 = readLines(logFile_.string() + ".1");
    const bool newestFound = (!current.empty() && current.back().find("rotation-line-39") != std::string::npos) ||
                             (!gen1.empty() && gen1.back().find("rotation-line-39") != std::string::npos);
    EXPECT_TRUE(newestFound) << "newest line should be in test.log or test.log.1";
    // Every generation is bounded near the rotation threshold.
    EXPECT_LE(fs::file_size(logFile_.string() + ".1"), 700U);
}

TEST_F(LoggerTest, NoRotationWhenMaxBytesIsZero) {
    namespace fs = std::filesystem;
    auto& logger = Logger::getInstance();
    logger.setRotation(0, 5); // off
    for (int i = 0; i < 50; ++i) {
        logger.log(LogLevel::INFO, "line-" + std::to_string(i));
    }
    EXPECT_FALSE(fs::exists(logFile_.string() + ".1"));
    EXPECT_EQ(readLines(logFile_).size(), 50U);
}

TEST_F(LoggerTest, RotationAccountsForAPreexistingFileSize) {
    namespace fs = std::filesystem;
    auto& logger = Logger::getInstance();
    // The file already has content from setLogFile(append); reopening must not
    // reset the size accounting to zero (else a large file never rotates).
    logger.log(LogLevel::INFO, std::string(500, 'x'));
    logger.setLogFile(logFile_.string()); // reopen (append)
    logger.setRotation(300, 2);
    logger.log(LogLevel::INFO, "trigger");
    EXPECT_TRUE(fs::exists(logFile_.string() + ".1")) << "reopen kept the existing size";
}

TEST_F(LoggerTest, WritesStructuredLinesToFile) {
    auto& logger = Logger::getInstance();
    logger.log(LogLevel::INFO, "plain message");
    logger.log(LogLevel::ERROR, LogContext{42, "sess", "host-a", "uptime"}, "with context");

    const auto lines = readLines(logFile_);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_TRUE(std::regex_match(lines[0], kLineFormat)) << lines[0];
    EXPECT_NE(lines[0].find("[INFO] "), std::string::npos);
    EXPECT_NE(lines[0].find("[session=-] [client=-] [command=-] plain message"), std::string::npos);
    EXPECT_TRUE(std::regex_match(lines[1], kLineFormat)) << lines[1];
    EXPECT_NE(lines[1].find("[ERROR] [pid=42] [session=sess] [client=host-a] [command=uptime] with context"),
              std::string::npos);
}

TEST_F(LoggerTest, EmptyContextFieldsRenderAsDash) {
    Logger::getInstance().log(LogLevel::INFO, LogContext{1, "", "", ""}, "dashes");
    const auto lines = readLines(logFile_);
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("[session=-] [client=-] [command=-] dashes"), std::string::npos);
}

TEST_F(LoggerTest, FiltersMessagesBelowConfiguredLevel) {
    auto& logger = Logger::getInstance();
    logger.setLevel(LogLevel::INFO);
    EXPECT_TRUE(logger.enabled(LogLevel::INFO));
    EXPECT_TRUE(logger.enabled(LogLevel::ERROR));
    EXPECT_FALSE(logger.enabled(LogLevel::DEBUG));
    logger.log(LogLevel::DEBUG, "suppressed");
    logger.log(LogLevel::INFO, "kept-info");
    logger.log(LogLevel::ERROR, "kept-error");

    logger.setLevel(LogLevel::ERROR);
    logger.log(LogLevel::INFO, "suppressed-info");
    logger.log(LogLevel::ERROR, "kept-error-2");

    logger.setLevel(LogLevel::DEBUG);
    logger.log(LogLevel::DEBUG, "kept-debug");

    const auto lines = readLines(logFile_);
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_NE(lines[0].find("kept-info"), std::string::npos);
    EXPECT_NE(lines[1].find("kept-error"), std::string::npos);
    EXPECT_NE(lines[2].find("kept-error-2"), std::string::npos);
    EXPECT_NE(lines[3].find("kept-debug"), std::string::npos);
}

TEST_F(LoggerTest, ConsoleMirrorDuplicatesToStderr) {
    std::ostringstream captured;
    auto* original = std::cerr.rdbuf(captured.rdbuf());

    auto& logger = Logger::getInstance();
    logger.log(LogLevel::INFO, "file-only");
    logger.setConsoleMirror(true);
    logger.log(LogLevel::INFO, "mirrored");
    logger.setConsoleMirror(false);

    std::cerr.rdbuf(original);

    EXPECT_EQ(captured.str().find("file-only"), std::string::npos);
    EXPECT_NE(captured.str().find("mirrored"), std::string::npos);
    EXPECT_EQ(readLines(logFile_).size(), 2U);
}

TEST_F(LoggerTest, FallsBackToStderrWhenNoFileIsOpen) {
    auto& logger = Logger::getInstance();
    EXPECT_TRUE(logger.setLogFile(""));

    std::ostringstream captured;
    auto* original = std::cerr.rdbuf(captured.rdbuf());
    logger.log(LogLevel::ERROR, "console-fallback");
    std::cerr.rdbuf(original);

    EXPECT_NE(captured.str().find("console-fallback"), std::string::npos);
    EXPECT_TRUE(readLines(logFile_).empty());
}

TEST_F(LoggerTest, UnwritableLogFileIsReportedNotThrown) {
    auto& logger = Logger::getInstance();
    // A path whose parent is a regular file can never be created.
    std::ofstream blocker(cwd_.path() / "blocker");
    blocker << "x";
    blocker.close();
    const auto blocked = cwd_.path() / "blocker" / "x.log";

    EXPECT_FALSE(logger.setLogFile(blocked.string()));
    EXPECT_NO_THROW(logger.log(LogLevel::INFO, "still logs somewhere"));
}

TEST_F(LoggerTest, CreatesMissingParentDirectories) {
    auto& logger = Logger::getInstance();
    const auto nested = cwd_.path() / "state" / "pipeshellx" / "nested.log";
    ASSERT_TRUE(logger.setLogFile(nested.string()));
    logger.log(LogLevel::INFO, "nested");
    EXPECT_EQ(readLines(nested).size(), 1U);
}

TEST_F(LoggerTest, ConcurrentWritersNeverInterleaveLines) {
    auto& logger = Logger::getInstance();
    constexpr int kThreads = 4;
    constexpr int kLinesPerThread = 250;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&logger, t]() {
            for (int i = 0; i < kLinesPerThread; ++i) {
                logger.log(LogLevel::INFO, LogContext{0, "t" + std::to_string(t), "-", "-"},
                           "line-" + std::to_string(t) + "-" + std::to_string(i));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto lines = readLines(logFile_);
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(kThreads * kLinesPerThread));
    const std::regex payload(R"(\[session=t(\d)\] \[client=-\] \[command=-\] line-(\d)-(\d+)$)");
    for (const auto& line : lines) {
        std::smatch match;
        ASSERT_TRUE(std::regex_match(line, kLineFormat)) << line;
        ASSERT_TRUE(std::regex_search(line, match, payload)) << line;
        EXPECT_EQ(match[1], match[2]) << line;
    }
}

TEST(LoggerPathTest, DefaultLogFileHonoursXdgStateHome) {
    test_support::ScopedEnv xdg("XDG_STATE_HOME", "/var/tmp/xdg-state");
    test_support::ScopedEnv home("HOME", "/home/tester");
    EXPECT_EQ(Logger::defaultLogFilePath(), "/var/tmp/xdg-state/pipeshellx/pipeshellx.log");
}

TEST(LoggerPathTest, DefaultLogFileFallsBackToHomeLocalState) {
    test_support::ScopedEnv xdg("XDG_STATE_HOME", std::nullopt);
    test_support::ScopedEnv home("HOME", "/home/tester");
    EXPECT_EQ(Logger::defaultLogFilePath(), "/home/tester/.local/state/pipeshellx/pipeshellx.log");
}

TEST(LoggerPathTest, EmptyXdgStateHomeIsTreatedAsUnset) {
    test_support::ScopedEnv xdg("XDG_STATE_HOME", "");
    test_support::ScopedEnv home("HOME", "/home/tester");
    EXPECT_EQ(Logger::defaultLogFilePath(), "/home/tester/.local/state/pipeshellx/pipeshellx.log");
}

TEST(LoggerPathTest, DefaultLogFileUsesTheAccountHomeWithoutHomeVariable) {
    test_support::ScopedEnv xdg("XDG_STATE_HOME", std::nullopt);
    test_support::ScopedEnv home("HOME", std::nullopt);
    const std::string path = Logger::defaultLogFilePath();
    EXPECT_TRUE(path.starts_with("/")) << path;
    EXPECT_TRUE(path.ends_with("/.local/state/pipeshellx/pipeshellx.log")) << path;
}
