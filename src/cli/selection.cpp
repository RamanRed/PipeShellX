#include "psx/cli/selection.hpp"

namespace psx::cli {

namespace {

ClientEntry toClientEntry(const psx::inventory::Host& host, const std::string& knownHosts) {
    ClientEntry entry;
    entry.user = host.user;
    entry.host = host.host;
    entry.port = host.port;
    entry.identityFile = host.identity;
    entry.expectedSan = host.san;
    entry.nativePort = host.nativePort;
    entry.transport = host.transport;
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
    resolved.inventoryPath = psx::inventory::Inventory::resolvePath(inventoryPath);

    psx::inventory::Inventory inventory;
    try {
        if (resolved.inventoryPath.empty()) {
            err << "no inventory (pass -i FILE, set PIPESHELLX_INVENTORY, or add inventory.ini)\n";
            resolved.exitCode = 2;
            return resolved;
        }
        // Arbitrary inventory paths are an intentional CLI capability. Ambient
        // environment paths reach this sink only with matching real/effective IDs.
        // codeql[cpp/path-injection]
        inventory = psx::inventory::Inventory::loadFromFile(resolved.inventoryPath);
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
