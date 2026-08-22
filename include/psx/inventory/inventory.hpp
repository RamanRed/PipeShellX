#pragma once

// psx::inventory::Inventory — the fleet a run targets (Phase 2). An INI file
// with a [defaults] section and [group] sections listing hosts; each host may
// carry per-host options (user, port, identity, tag). Hosts are selectable by
// group, tag, explicit name, or all. A legacy clients.txt imports as one
// implicit group named "all". Self-contained (std-only); a Host is turned into
// an SSH descriptor at the transport boundary.

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
    std::vector<std::string> groups; // groups this host belongs to, in file order
    std::vector<std::string> tags;   // tags, in first-seen order
};

class Inventory {
public:
    // Parses INI text. Throws std::runtime_error naming the offending line on
    // a malformed section, unknown key, or bad value.
    static Inventory parse(std::string_view iniText, const std::string& sourcePath = {});
    static Inventory loadFromFile(const std::string& path);

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

private:
    Host& upsert(const std::string& name); // find-or-create by name

    std::vector<Host> hosts_;
    std::string sourcePath_;
};

} // namespace psx::inventory
