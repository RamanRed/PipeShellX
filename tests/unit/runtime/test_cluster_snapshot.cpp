#include "psx/runtime/cluster_snapshot.hpp"

#include "test_support.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using psx::runtime::ClusterSnapshot;
using psx::runtime::NodeSnapshot;

namespace {

std::vector<std::string> readLines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST(ClusterSnapshotTest, EmptySnapshotSerializesToEmptyNodesArray) {
    ClusterSnapshot snap("run-1");
    const std::string line = snap.toJsonLine();
    EXPECT_NE(line.find(R"("type":"cluster_snapshot")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("run_id":"run-1")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("nodes":[])"), std::string::npos) << line;
}

TEST(ClusterSnapshotTest, RecordAccumulatesInCallOrder) {
    ClusterSnapshot snap("run-2");
    snap.record(NodeSnapshot{.host = "h1", .stageId = "s0", .status = "running", .exitCode = 0, .lamportTs = 3});
    snap.record(NodeSnapshot{.host = "h2", .stageId = "s1", .status = "exited", .exitCode = 1, .lamportTs = 7});

    ASSERT_EQ(snap.nodes().size(), 2U);
    EXPECT_EQ(snap.nodes()[0].host, "h1");
    EXPECT_EQ(snap.nodes()[1].host, "h2");

    const std::string line = snap.toJsonLine();
    EXPECT_NE(line.find(R"("host":"h1")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("host":"h2")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("status":"running")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("status":"exited")"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("lamport_ts":3)"), std::string::npos) << line;
    EXPECT_NE(line.find(R"("lamport_ts":7)"), std::string::npos) << line;
    // h1 must appear before h2 in the serialized array (call order preserved).
    EXPECT_LT(line.find("\"h1\""), line.find("\"h2\""));
}

TEST(ClusterSnapshotTest, AppendToFileWritesOneLinePerCallAndCreatesParentDirs) {
    test_support::ScopedTempCwd cwd("cluster-snapshot");
    const std::string path = (cwd.path() / "nested" / "snapshots.jsonl").string();

    ClusterSnapshot first("run-a");
    first.record(NodeSnapshot{.host = "h1", .status = "running"});
    ASSERT_TRUE(first.appendToFile(path));

    ClusterSnapshot second("run-a");
    second.record(NodeSnapshot{.host = "h1", .status = "exited", .exitCode = 0});
    ASSERT_TRUE(second.appendToFile(path));

    const auto lines = readLines(path);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_NE(lines[0].find(R"("status":"running")"), std::string::npos) << lines[0];
    EXPECT_NE(lines[1].find(R"("status":"exited")"), std::string::npos) << lines[1];
}

TEST(ClusterSnapshotTest, UnwritablePathReturnsFalseWithoutThrowing) {
    ClusterSnapshot snap("run-x");
    snap.record(NodeSnapshot{.host = "h1", .status = "running"});
    EXPECT_FALSE(snap.appendToFile("/this/path/does/not/exist/and/cannot/be/made/s.jsonl"));
}
