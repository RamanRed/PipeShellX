#include "psx/cli/node_command.hpp"

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/node_server.hpp"

#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace psx::cli {

namespace {

std::optional<std::string> flag(const std::vector<std::string>& args, std::size_t from, const std::string& name) {
    for (std::size_t i = from; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    return std::nullopt;
}

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Splits "host:port" on the last ':' so IPv4 hosts parse; returns false on error.
bool parseListen(const std::string& value, std::string& host, std::uint16_t& port) {
    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
        return false;
    }
    host = value.substr(0, colon);
    const std::string portStr = value.substr(colon + 1);
    unsigned long parsed = 0;
    const auto* end = portStr.data() + portStr.size();
    const auto [ptr, ec] = std::from_chars(portStr.data(), end, parsed);
    if (ec != std::errc{} || ptr != end || parsed == 0 || parsed > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

} // namespace

int nodeSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const std::size_t from = (!args.empty() && args[0] == "run") ? 1 : 0;

    const auto certPath = flag(args, from, "--cert");
    const auto keyPath = flag(args, from, "--key");
    const auto caPath = flag(args, from, "--ca");
    const auto listen = flag(args, from, "--listen");
    if (!certPath || !keyPath || !caPath || !listen) {
        err << "Usage: pipeshellx node --cert F --key F --ca F --listen HOST:PORT [--allow SAN[,SAN...]]\n";
        return 2;
    }

    const auto certPem = readFile(*certPath);
    const auto keyPem = readFile(*keyPath);
    const auto caPem = readFile(*caPath);
    if (!certPem || !keyPem || !caPem) {
        err << "pipeshellx node: cannot read cert/key/ca file\n";
        return 2;
    }

    std::string host;
    std::uint16_t port = 0;
    if (!parseListen(*listen, host, port)) {
        err << "pipeshellx node: --listen must be HOST:PORT, got '" << *listen << "'\n";
        return 2;
    }

    // --allow builds the SAN allow-list; empty means authenticate only (no
    // identity restriction) — warn, since that admits any CA-signed peer.
    std::unordered_set<std::string> allow;
    if (const auto allowCsv = flag(args, from, "--allow")) {
        std::stringstream ss(*allowCsv);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                allow.insert(item);
            }
        }
    }
    if (allow.empty()) {
        err << "pipeshellx node: warning: no --allow list; admitting any CA-signed peer\n";
    }

    auto reactor = psx::runtime::Reactor::create({.signals = {psx::os::Signal::Interrupt, psx::os::Signal::Terminate}});
    if (!reactor.ok()) {
        err << "pipeshellx node: " << reactor.error().message() << "\n";
        return 2;
    }
    auto listener = psx::os::Socket::listen(host, port);
    if (!listener.ok()) {
        err << "pipeshellx node: cannot listen on " << *listen << ": " << listener.error().message() << "\n";
        return 2;
    }

    std::function<bool(std::string_view)> authorize;
    if (!allow.empty()) {
        authorize = [allow = std::move(allow)](std::string_view san) { return allow.count(std::string(san)) != 0; };
    }
    psx::transport::NodeServer server(*reactor.value(), std::move(listener.value()),
                                      {.certificatePem = *certPem, .privateKeyPem = *keyPem, .caPem = *caPem},
                                      std::move(authorize));
    if (auto started = server.start(); !started.ok()) {
        err << "pipeshellx node: " << started.error().message() << "\n";
        return 2;
    }

    (void)reactor.value()->onSignal([&reactor](psx::os::Signal) { reactor.value()->stop(); });
    out << "pipeshellx node listening on " << host << ":" << port << "\n";
    out.flush();
    (void)reactor.value()->run();
    return 0;
}

} // namespace psx::cli
