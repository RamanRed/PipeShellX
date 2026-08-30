#include "psx/cli/hosts_command.hpp"

#include "psx/inventory/inventory.hpp"
#include "psx/os/paths.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace psx::cli {

namespace {

struct HostsInvocation {
    std::string inventoryPath;
    std::string action = "list";
    std::vector<std::string> operands;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string join(const std::vector<std::string>& items) {
    std::string result;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            result += ',';
        }
        result += items[i];
    }
    return result.empty() ? "-" : result;
}

HostsInvocation parseInvocation(const std::vector<std::string>& args) {
    HostsInvocation invocation;
    std::vector<std::string> remaining;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-i" || args[i] == "--inventory") {
            if (!invocation.inventoryPath.empty()) {
                fail("-i/--inventory may only be specified once");
            }
            if (i + 1 >= args.size() || args[i + 1].empty() || args[i + 1].front() == '-') {
                fail(args[i] + " requires a value");
            }
            invocation.inventoryPath = args[++i];
        } else {
            remaining.push_back(args[i]);
        }
    }

    if (remaining.empty()) {
        return invocation;
    }
    invocation.action = remaining.front();
    if (invocation.action != "list" && invocation.action != "add" && invocation.action != "remove" &&
        invocation.action != "import") {
        fail("unknown action '" + invocation.action + "' (expected list, add, remove, or import)");
    }
    invocation.operands.assign(remaining.begin() + 1, remaining.end());
    return invocation;
}

std::string requiredValue(const std::vector<std::string>& args, std::size_t& index, const std::string& option) {
    if (index + 1 >= args.size() || args[index + 1].empty() || args[index + 1].front() == '-') {
        fail(option + " requires a value");
    }
    return args[++index];
}

void rejectUnsafeValue(std::string_view value, const std::string& description) {
    if (value.empty() || value.find_first_of("\r\n") != std::string_view::npos) {
        fail(description + " contains an unsafe value");
    }
}

bool isSecretKey(std::string_view key) {
    return key == "password" || key == "pass" || key == "secret" || key == "token" || key == "private_key";
}

psx::inventory::Host parseHostToAdd(const std::vector<std::string>& args) {
    if (args.empty()) {
        fail("add requires HOST");
    }
    if (args.front().empty() || args.front().front() == '-' || args.front().find('?') != std::string::npos) {
        fail("add requires a valid HOST without URL query parameters");
    }
    const auto at = args.front().find('@');
    if (args.front().rfind("ssh://", 0) == 0 ||
        (at != std::string::npos && args.front().substr(0, at).find(':') != std::string::npos)) {
        fail("add HOST must not contain a URL or user:password credentials; use user@host");
    }
    rejectUnsafeValue(args.front(), "HOST");

    std::string group = "all";
    bool groupSeen = false;
    std::vector<std::string> options;
    std::unordered_set<std::string> optionKeys;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& argument = args[i];
        if (argument == "-g" || argument == "--group" || argument == "--group-name") {
            if (groupSeen) {
                fail("add group may only be specified once");
            }
            group = requiredValue(args, i, argument);
            groupSeen = true;
            rejectUnsafeValue(group, "group");
            if (group.find_first_of("[]#; \t") != std::string::npos) {
                fail("group must be a single safe inventory name");
            }
            continue;
        }

        std::string key;
        std::string value;
        if (argument == "--user" || argument == "--port" || argument == "--identity" || argument == "--tag" ||
            argument == "--tags" || argument == "--transport" || argument == "--san" || argument == "--native-port") {
            key = argument.substr(2);
            if (key == "tags") {
                key = "tag";
            } else if (key == "native-port") {
                key = "native_port";
            }
            value = requiredValue(args, i, argument);
        } else if (const auto equals = argument.find('='); equals != std::string::npos) {
            key = argument.substr(0, equals);
            value = argument.substr(equals + 1);
        } else {
            fail("unexpected add argument '" + argument + "'");
        }

        rejectUnsafeValue(key, "option name");
        rejectUnsafeValue(value, "option value");
        if (isSecretKey(key)) {
            fail("secret option '" + key + "' is not allowed in an inventory");
        }
        if (!optionKeys.insert(key).second) {
            fail("add option '" + key + "' may only be specified once");
        }
        options.push_back(key + '=' + value);
    }

    std::ostringstream ini;
    ini << '[' << group << "]\n" << args.front();
    for (const auto& option : options) {
        ini << ' ' << option;
    }
    ini << '\n';
    auto parsed = psx::inventory::Inventory::parse(ini.str());
    if (parsed.hosts().size() != 1) {
        fail("add must describe exactly one host");
    }
    return parsed.hosts().front();
}

psx::inventory::Inventory loadMutationTarget(const std::string& path, bool allowMissing) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        fail("cannot inspect inventory '" + path + "': " + error.message());
    }
    if (!exists && allowMissing) {
        return {};
    }
    return psx::inventory::Inventory::loadFromFile(path);
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        fail("cannot open '" + path + "'");
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        fail("cannot read '" + path + "'");
    }
    return contents.str();
}

void atomicRewrite(const std::string& path, const std::string& contents) {
    const auto rewritten = psx::os::atomicRewriteFile(path, contents);
    if (!rewritten.ok()) {
        fail("cannot atomically rewrite '" + path + "': " + rewritten.error().message());
    }
}

int listHosts(const std::string& requestedPath, std::ostream& out) {
    const std::string path = psx::inventory::Inventory::resolvePath(requestedPath);
    if (path.empty()) {
        fail("no inventory (pass -i FILE, set PIPESHELLX_INVENTORY, or add inventory.ini)");
    }
    // Arbitrary inventory paths are an intentional CLI capability. Ambient
    // environment paths reach this sink only with matching real/effective IDs.
    // lgtm[cpp/path-injection]
    const auto inventory = psx::inventory::Inventory::loadFromFile(path);
    out << "HOST\tGROUPS\tTAGS\tTRANSPORT\n";
    for (const auto& host : inventory.hosts()) {
        out << host.name << '\t' << join(host.groups) << '\t' << join(host.tags) << '\t' << host.transport << '\n';
    }
    return 0;
}

void requireExplicitInventory(const HostsInvocation& invocation) {
    if (invocation.inventoryPath.empty()) {
        fail(invocation.action + " requires an explicit -i FILE inventory");
    }
    if (std::filesystem::path(invocation.inventoryPath).filename() == "clients.txt") {
        fail("clients.txt is a legacy import source; mutation requires an INI inventory target");
    }
}

int addHost(const HostsInvocation& invocation, std::ostream& out) {
    requireExplicitInventory(invocation);
    auto inventory = loadMutationTarget(invocation.inventoryPath, true);
    const auto host = parseHostToAdd(invocation.operands);
    const std::string name = host.name;
    inventory.add(host);
    atomicRewrite(invocation.inventoryPath, inventory.serialize());
    out << "added " << name << " to " << invocation.inventoryPath << '\n';
    return 0;
}

int removeHost(const HostsInvocation& invocation, std::ostream& out) {
    requireExplicitInventory(invocation);
    if (invocation.operands.size() != 1 || invocation.operands.front().empty()) {
        fail("remove requires exactly one HOST");
    }
    auto inventory = loadMutationTarget(invocation.inventoryPath, false);
    if (!inventory.remove(invocation.operands.front())) {
        fail("host '" + invocation.operands.front() + "' not found");
    }
    atomicRewrite(invocation.inventoryPath, inventory.serialize());
    out << "removed " << invocation.operands.front() << " from " << invocation.inventoryPath << '\n';
    return 0;
}

int importClients(const HostsInvocation& invocation, std::ostream& out) {
    requireExplicitInventory(invocation);
    if (invocation.operands.size() != 1 || invocation.operands.front().empty()) {
        fail("import requires exactly one clients.txt path");
    }
    const std::string& source = invocation.operands.front();
    std::error_code sourceError;
    std::error_code targetError;
    const auto absoluteSource = std::filesystem::absolute(source, sourceError).lexically_normal();
    const auto absoluteTarget = std::filesystem::absolute(invocation.inventoryPath, targetError).lexically_normal();
    if (sourceError || targetError) {
        fail("cannot resolve import paths");
    }
    if (absoluteSource == absoluteTarget) {
        fail("import source and inventory target must be different files");
    }

    const auto imported = psx::inventory::Inventory::importClientsTxt(readFile(source), source);
    if (imported.hosts().empty()) {
        fail("import source contains no hosts");
    }
    auto inventory = loadMutationTarget(invocation.inventoryPath, true);
    for (const auto& host : imported.hosts()) {
        inventory.add(host);
    }
    atomicRewrite(invocation.inventoryPath, inventory.serialize());
    out << "imported " << imported.hosts().size() << " host(s) into " << invocation.inventoryPath << '\n';
    return 0;
}

} // namespace

int hostsSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    try {
        const HostsInvocation invocation = parseInvocation(args);
        if (invocation.action == "list") {
            if (!invocation.operands.empty()) {
                fail("list does not accept positional arguments");
            }
            return listHosts(invocation.inventoryPath, out);
        }
        if (invocation.action == "add") {
            return addHost(invocation, out);
        }
        if (invocation.action == "remove") {
            return removeHost(invocation, out);
        }
        return importClients(invocation, out);
    } catch (const std::exception& ex) {
        err << "pipeshellx hosts: " << ex.what() << '\n';
        return 2;
    }
}

int hostsSubcommand(const std::string& inventoryPath, std::ostream& out, std::ostream& err) {
    std::vector<std::string> args{"list"};
    if (!inventoryPath.empty()) {
        args.insert(args.end(), {"-i", inventoryPath});
    }
    return hostsSubcommand(args, out, err);
}

} // namespace psx::cli
