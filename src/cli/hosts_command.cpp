#include "psx/cli/hosts_command.hpp"

#include "psx/inventory/inventory.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace psx::cli {

namespace {

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += items[i];
    }
    return out.empty() ? "-" : out;
}

} // namespace

int hostsSubcommand(const std::string& inventoryPath, std::ostream& out, std::ostream& err) {
    psx::inventory::Inventory inventory;
    try {
        if (!inventoryPath.empty()) {
            inventory = psx::inventory::Inventory::loadFromFile(inventoryPath);
        } else if (std::filesystem::exists("clients.txt")) {
            std::ifstream file("clients.txt");
            std::stringstream buffer;
            buffer << file.rdbuf();
            inventory = psx::inventory::Inventory::importClientsTxt(buffer.str(), "clients.txt");
        } else {
            err << "pipeshellx hosts: no inventory (pass -i FILE or add a clients.txt)\n";
            return 2;
        }
    } catch (const std::exception& ex) {
        err << "pipeshellx hosts: " << ex.what() << "\n";
        return 2;
    }

    out << "HOST\tGROUPS\tTAGS\n";
    for (const auto& host : inventory.hosts()) {
        out << host.name << '\t' << join(host.groups) << '\t' << join(host.tags) << '\n';
    }
    return 0;
}

} // namespace psx::cli
