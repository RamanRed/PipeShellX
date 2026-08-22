#include "psx/cli/selection.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace psx::cli {

namespace {

ClientEntry toClientEntry(const psx::inventory::Host& host, const std::string& knownHosts) {
    ClientEntry entry;
    entry.user = host.user;
    entry.host = host.host;
    entry.port = host.port;
    entry.identityFile = host.identity;
    entry.knownHostsFile = knownHosts;
    entry.raw = entry.serialize();
    return entry;
}

std::vector<psx::inventory::Host> select(const psx::inventory::Inventory& inventory, const Selector& selector) {
    switch (selector.kind) {
        case SelectorKind::Group:
            return inventory.selectGroup(selector.value);
        case SelectorKind::Tag:
            return inventory.selectTag(selector.value);
        case SelectorKind::Hosts:
            return inventory.selectHosts(selector.hosts);
        case SelectorKind::All:
        default:
            return inventory.all();
    }
}

} // namespace

ResolvedHosts resolveHosts(const std::string& inventoryPath, const Selector& selector, std::ostream& err) {
    ResolvedHosts resolved;
    resolved.inventoryPath = inventoryPath;

    psx::inventory::Inventory inventory;
    try {
        if (!inventoryPath.empty()) {
            inventory = psx::inventory::Inventory::loadFromFile(inventoryPath);
        } else if (std::filesystem::exists("clients.txt")) {
            std::ifstream file("clients.txt");
            std::stringstream buffer;
            buffer << file.rdbuf();
            inventory = psx::inventory::Inventory::importClientsTxt(buffer.str(), "clients.txt");
            resolved.inventoryPath = "clients.txt";
        } else {
            err << "no inventory (pass -i FILE or add a clients.txt)\n";
            resolved.exitCode = 2;
            return resolved;
        }
    } catch (const std::exception& ex) {
        err << ex.what() << "\n";
        resolved.exitCode = 2;
        return resolved;
    }

    std::vector<psx::inventory::Host> hosts;
    try {
        hosts = select(inventory, selector);
    } catch (const std::exception& ex) {
        err << ex.what() << "\n";
        resolved.exitCode = 2;
        return resolved;
    }
    if (hosts.empty()) {
        err << "no hosts selected\n";
        resolved.exitCode = 3;
        return resolved;
    }

    const std::string knownHosts = ClientConfig::knownHostsPathFor(resolved.inventoryPath);
    resolved.clients.reserve(hosts.size());
    for (const auto& host : hosts) {
        resolved.clients.push_back(toClientEntry(host, knownHosts));
    }
    return resolved;
}

} // namespace psx::cli
