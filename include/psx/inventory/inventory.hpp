#pragma once

// psx::inventory::Inventory — the fleet a run targets. An INI file
// with a [defaults] section and [group] sections listing hosts; each host may
// carry per-host SSH/native options (user, port, identity, tag, transport,
// SAN, and native port). Hosts are selectable by group, tag, explicit name,
// or all. A legacy clients.txt imports as one implicit group named "all".
// Self-contained (std-only); selection converts a Host to a ClientEntry at the
// transport boundary.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace psx::inventory {

struct Host {
    std::string name; // the inventory name (usually the hostname)
    std::string user; // resolved from [defaults] then per-host
    std::string host; // the address to connect to (defaults to name)
    std::uint16_t port = 22;
    std::string identity;            // identity file, if any
    std::string transport = "ssh";   // per-host execution transport: ssh or native
    std::string san;                 // native transport: pinned SAN-URI identity (empty = trust the CA only)
    std::uint16_t nativePort = 0;    // native transport: node port (0 = use the run's --native-port)
    std::vector<std::string> groups; // groups this host belongs to, in file order
    std::vector<std::string> tags;   // tags, in first-seen order
};

class Inventory {
public:
    // Parses INI text. Throws std::runtime_error naming the offending line on
    // a malformed section, unknown key, or bad value.
    static Inventory parse(std::string_view iniText, const std::string& sourcePath = {});
    static Inventory loadFromFile(const std::string& path);

    // Resolves the inventory path with product precedence: an explicit CLI
    // path, $PIPESHELLX_INVENTORY, ./inventory.ini, the legacy
    // ./clients.txt, then the per-user config inventory. Returns an empty
    // string when no candidate exists. An explicit or environment path is
    // returned even when missing so loadFromFile() reports that exact error.
    static std::string resolvePath(const std::string& explicitPath = {});

    // Imports a legacy clients.txt (one `user@host` / `ssh://…` per line) as a
    // single implicit group "all".
    static Inventory importClientsTxt(std::string_view text, const std::string& sourcePath = {});

    const std::vector<Host>& hosts() const noexcept { return hosts_; }
    const std::string& sourcePath() const noexcept { return sourcePath_; }

    std::vector<Host> all() const { return hosts_; }
    std::vector<Host> selectGroup(std::string_view group) const;
    std::vector<Host> selectTag(std::string_view tag) const;
    // Throws if any requested name is unknown.
    std::vector<Host> selectHosts(const std::vector<std::string>& names) const;

    std::vector<std::string> groups() const;
    std::vector<std::string> tags() const;

    // Canonical, secret-free INI used by the `hosts` mutation commands.
    std::string serialize() const;

    // Adds/removes whole host records. add() rejects duplicate names or
    // addresses; remove() returns false when no matching host exists.
    void add(Host host);
    bool remove(std::string_view name);

private:
    Host& upsert(const std::string& name); // find-or-create by name

    std::vector<Host> hosts_;
    std::string sourcePath_;
};

} // namespace psx::inventory
