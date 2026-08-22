#include <gtest/gtest.h>

#include "client_config.hpp"
#include "client_manager.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

TEST(ClientConfigTest, RejectsPasswordInSshUrl) {
    EXPECT_THROW(static_cast<void>(ClientConfig::parseEntry(
                     "ssh://admin@server.example.com:2222?identity=/home/admin/.ssh/id_ed25519&password=secret123")),
                 std::runtime_error);
}

TEST(ClientConfigTest, SerializeOmitsInMemoryPassword) {
    ClientEntry entry;
    entry.user = "admin";
    entry.host = "server.example.com";
    entry.password = "secret123";

    EXPECT_EQ(entry.serialize(), "admin@server.example.com");
}

TEST(ClientConfigTest, ParsesLegacyAndUrlEntries) {
    const auto legacy = ClientConfig::parseEntry("admin@server.example.com");
    EXPECT_EQ(legacy.user, "admin");
    EXPECT_EQ(legacy.host, "server.example.com");
    EXPECT_EQ(legacy.port, 22);
    EXPECT_TRUE(legacy.identityFile.empty());

    const auto url =
        ClientConfig::parseEntry("ssh://admin@server.example.com:2222?identity=/home/admin/.ssh/id_ed25519");
    EXPECT_EQ(url.user, "admin");
    EXPECT_EQ(url.host, "server.example.com");
    EXPECT_EQ(url.port, 2222);
    EXPECT_EQ(url.identityFile, "/home/admin/.ssh/id_ed25519");
}

TEST(ClientConfigTest, KnownHostsPathIsDerivedFromInventoryPath) {
    test_support::ScopedTempCwd cwd("known-hosts-path");

    EXPECT_EQ(ClientConfig::knownHostsPathFor("clients.txt"), (cwd.path() / "clients.txt.known_hosts").string());

    const std::string absolute = ClientConfig::knownHostsPathFor("/etc/pipeshellx/fleet.txt");
    EXPECT_EQ(absolute, "/etc/pipeshellx/fleet.txt.known_hosts");
}

TEST(ClientConfigTest, LoadFromFileAttachesPerInventoryKnownHosts) {
    test_support::ScopedTempCwd cwd("load-known-hosts");
    {
        std::ofstream inventory("fleet.txt");
        inventory << "# comment\n"
                  << "alice@host-a.example.com\n"
                  << "ssh://bob@host-b.example.com:2200\n";
    }

    ClientConfig config;
    config.loadFromFile("fleet.txt");
    ASSERT_EQ(config.clients().size(), 2U);

    const std::string expected = (cwd.path() / "fleet.txt.known_hosts").string();
    for (const auto& entry : config.clients()) {
        EXPECT_EQ(entry.knownHostsFile, expected);
        // The known_hosts location is derived state and must never be persisted.
        EXPECT_EQ(entry.serialize().find("known_hosts"), std::string::npos);
    }
}

TEST(ClientConfigTest, MakeEntryAttachesKnownHostsOnlyWithAnInventory) {
    test_support::ScopedTempCwd cwd("make-entry");

    const auto pinned = ClientConfig("fleet.txt").makeEntry("alice@host-a.example.com");
    EXPECT_EQ(pinned.knownHostsFile, (cwd.path() / "fleet.txt.known_hosts").string());
    EXPECT_EQ(pinned.serialize(), "alice@host-a.example.com");

    // Without an inventory the entry is a pure parse (OpenSSH's own known_hosts applies).
    EXPECT_TRUE(ClientConfig().makeEntry("alice@host-a.example.com").knownHostsFile.empty());
    EXPECT_TRUE(ClientConfig::parseEntry("alice@host-a.example.com").knownHostsFile.empty());
}

TEST(ClientConfigTest, LoadFromFileRejectsDuplicatesAndPasswords) {
    test_support::ScopedTempCwd cwd("load-rejects");
    {
        std::ofstream inventory("dupes.txt");
        inventory << "alice@host-a.example.com\n"
                  << "alice@host-a.example.com\n";
    }
    {
        std::ofstream inventory("pw.txt");
        inventory << "ssh://alice@host-a.example.com?password=hunter2\n";
    }

    ClientConfig config;
    EXPECT_THROW(config.loadFromFile("dupes.txt"), std::runtime_error);
    EXPECT_THROW(config.loadFromFile("pw.txt"), std::runtime_error);
    EXPECT_THROW(config.loadFromFile("does-not-exist.txt"), std::runtime_error);
}

TEST(ClientManagerTest, AddClientAttachesKnownHostsAndReportsOffline) {
    test_support::ScopedTempCwd cwd("manager-add");

    ClientManager manager("inventory.txt");
    ASSERT_TRUE(manager.empty());

    const std::string specification = test_support::refusedLoopbackClient().serialize();
    manager.addClient(specification, std::nullopt);

    ASSERT_EQ(manager.clients().size(), 1U);
    const auto& managed = manager.clients().front();
    EXPECT_EQ(managed.entry.knownHostsFile, (cwd.path() / "inventory.txt.known_hosts").string());
    EXPECT_EQ(managed.status, ClientStatus::OFFLINE);
    EXPECT_FALSE(managed.client.online);
    // ssh must actually have run and been refused — not "not found", not "timed out".
    EXPECT_EQ(managed.lastError, "ERROR: connection failed");

    // The inventory was persisted without derived or secret fields.
    std::ifstream persisted("inventory.txt");
    std::string line;
    ASSERT_TRUE(std::getline(persisted, line));
    EXPECT_EQ(line, specification);
}
