#include <gtest/gtest.h>

#include "psx/inventory/inventory.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using psx::inventory::Host;
using psx::inventory::Inventory;

namespace {

std::vector<std::string> names(const std::vector<Host>& hosts) {
    std::vector<std::string> out;
    for (const auto& h : hosts) {
        out.push_back(h.name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

const Host& find(const Inventory& inv, const std::string& name) {
    for (const auto& h : inv.hosts()) {
        if (h.name == name) {
            return h;
        }
    }
    throw std::runtime_error("no such host: " + name);
}

} // namespace

TEST(InventoryTest, ParsesGroupsAndListsHosts) {
    const auto inv = Inventory::parse(R"(
[web]
web001.example.com
web002.example.com

[db]
db001.example.com
)");
    EXPECT_EQ(inv.hosts().size(), 3U);
    EXPECT_EQ(names(inv.hosts()),
              (std::vector<std::string>{"db001.example.com", "web001.example.com", "web002.example.com"}));
    EXPECT_EQ(names(inv.selectGroup("web")), (std::vector<std::string>{"web001.example.com", "web002.example.com"}));
    EXPECT_EQ(names(inv.selectGroup("db")), (std::vector<std::string>{"db001.example.com"}));
    EXPECT_TRUE(inv.selectGroup("nope").empty());
}

TEST(InventoryTest, DefaultsApplyAndPerHostOptionsOverride) {
    const auto inv = Inventory::parse(R"(
[defaults]
user = deploy
port = 22
identity = /keys/id

[web]
plain.example.com
custom.example.com user=nginx port=2222 identity=/keys/web
)");
    const Host& plain = find(inv, "plain.example.com");
    EXPECT_EQ(plain.user, "deploy");
    EXPECT_EQ(plain.port, 22);
    EXPECT_EQ(plain.identity, "/keys/id");

    const Host& custom = find(inv, "custom.example.com");
    EXPECT_EQ(custom.user, "nginx");
    EXPECT_EQ(custom.port, 2222);
    EXPECT_EQ(custom.identity, "/keys/web");
}

TEST(InventoryTest, UserAtHostFormIsParsed) {
    const auto inv = Inventory::parse(R"(
[web]
alice@w1.example.com
bob@w2.example.com port=2200
)");
    EXPECT_EQ(find(inv, "w1.example.com").user, "alice");
    EXPECT_EQ(find(inv, "w2.example.com").user, "bob");
    EXPECT_EQ(find(inv, "w2.example.com").port, 2200);
}

TEST(InventoryTest, TagsAreCommaSeparatedAndSelectable) {
    const auto inv = Inventory::parse(R"(
[web]
w1.example.com tag=canary
w2.example.com tag=canary,prod
w3.example.com tag=prod
)");
    EXPECT_EQ(names(inv.selectTag("canary")), (std::vector<std::string>{"w1.example.com", "w2.example.com"}));
    EXPECT_EQ(names(inv.selectTag("prod")), (std::vector<std::string>{"w2.example.com", "w3.example.com"}));
    const auto& w2 = find(inv, "w2.example.com");
    EXPECT_EQ(w2.tags, (std::vector<std::string>{"canary", "prod"}));
}

TEST(InventoryTest, AHostInMultipleGroupsIsOneHostWithUnionMembership) {
    const auto inv = Inventory::parse(R"(
[web]
shared.example.com tag=a

[edge]
shared.example.com tag=b
)");
    EXPECT_EQ(inv.hosts().size(), 1U);
    const Host& h = find(inv, "shared.example.com");
    EXPECT_EQ(h.groups, (std::vector<std::string>{"web", "edge"}));
    EXPECT_EQ(h.tags, (std::vector<std::string>{"a", "b"}));
    EXPECT_FALSE(inv.selectGroup("web").empty());
    EXPECT_FALSE(inv.selectGroup("edge").empty());
}

TEST(InventoryTest, SelectHostsMatchesByNameAndAllReturnsEverything) {
    const auto inv = Inventory::parse(R"(
[web]
w1.example.com
w2.example.com
w3.example.com
)");
    EXPECT_EQ(names(inv.selectHosts({"w1.example.com", "w3.example.com"})),
              (std::vector<std::string>{"w1.example.com", "w3.example.com"}));
    EXPECT_EQ(inv.all().size(), 3U);
    EXPECT_THROW(static_cast<void>(inv.selectHosts({"missing"})), std::runtime_error);
}

TEST(InventoryTest, GroupsAndTagsAreEnumerable) {
    const auto inv = Inventory::parse(R"(
[web]
w1 tag=canary
[db]
d1 tag=primary
)");
    auto groups = inv.groups();
    std::sort(groups.begin(), groups.end());
    EXPECT_EQ(groups, (std::vector<std::string>{"db", "web"}));
    auto tags = inv.tags();
    std::sort(tags.begin(), tags.end());
    EXPECT_EQ(tags, (std::vector<std::string>{"canary", "primary"}));
}

TEST(InventoryTest, CommentsAndBlankLinesAreIgnored) {
    const auto inv = Inventory::parse(R"(
# a comment
; another comment

[web]
  w1.example.com    # trailing comment

  # indented comment
w2.example.com
)");
    EXPECT_EQ(names(inv.hosts()), (std::vector<std::string>{"w1.example.com", "w2.example.com"}));
}

TEST(InventoryTest, MalformedInputIsRejectedWithLineNumbers) {
    EXPECT_THROW(static_cast<void>(Inventory::parse("w1.example.com\n")), std::runtime_error); // host before any group
    EXPECT_THROW(static_cast<void>(Inventory::parse("[web]\nw1 port=notanumber\n")), std::runtime_error);
    EXPECT_THROW(static_cast<void>(Inventory::parse("[web]\nw1 bogus=x\n")), std::runtime_error); // unknown key
    EXPECT_THROW(static_cast<void>(Inventory::parse("[unterminated\n")), std::runtime_error);
    EXPECT_THROW(static_cast<void>(Inventory::parse("[web]\nw1 port=99999\n")), std::runtime_error);
    try {
        static_cast<void>(Inventory::parse("[web]\nw1 bogus=x\n"));
    } catch (const std::exception& ex) {
        EXPECT_NE(std::string(ex.what()).find("2"), std::string::npos) << "error names the line: " << ex.what();
    }
}

TEST(InventoryTest, ImportsLegacyClientsTxt) {
    const auto inv = Inventory::importClientsTxt(R"(
# clients.txt
alice@w1.example.com
ssh://bob@w2.example.com:2222
)");
    EXPECT_EQ(inv.hosts().size(), 2U);
    EXPECT_EQ(find(inv, "w1.example.com").user, "alice");
    const Host& w2 = find(inv, "w2.example.com");
    EXPECT_EQ(w2.user, "bob");
    EXPECT_EQ(w2.port, 2222);
    // Imported hosts all belong to a single implicit group.
    EXPECT_FALSE(inv.selectGroup("all").empty());
}

TEST(InventoryTest, DefaultsSectionRejectsUnknownKeys) {
    EXPECT_THROW(static_cast<void>(Inventory::parse("[defaults]\nbogus = x\n")), std::runtime_error);
}

TEST(InventoryTest, ParsesNativeSanAndPortPerHost) {
    const auto inv = Inventory::parse(R"(
[fleet]
node-1 san=spiffe://psx/node/n1 native_port=7433
node-2
)");
    const Host& pinned = find(inv, "node-1");
    EXPECT_EQ(pinned.san, "spiffe://psx/node/n1");
    EXPECT_EQ(pinned.nativePort, 7433);
    const Host& plain = find(inv, "node-2");
    EXPECT_TRUE(plain.san.empty());
    EXPECT_EQ(plain.nativePort, 0); // falls back to the run's --native-port
}
