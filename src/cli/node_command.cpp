#include "psx/cli/node_command.hpp"

#include "psx/ca/certificate_authority.hpp"
#include "psx/os/io.hpp"
#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/node_server.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

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

bool writeFile(const std::string& path, const std::string& content, bool secret, std::ostream& err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !(out << content) || (out.close(), !out)) {
        err << "pipeshellx node keygen: cannot write " << path << "\n";
        return false;
    }
    if (secret) {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
    }
    return true;
}

// `node keygen`: generate the node's private key + a CSR to send to the CA. The
// key never leaves the node; only the CSR travels (see `ca sign`).
int nodeKeygen(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto san = flag(args, 1, "--san");
    const auto outPfx = flag(args, 1, "--out");
    if (!san || !outPfx) {
        err << "Usage: pipeshellx node keygen --san URI --out PREFIX\n";
        return 2;
    }
    auto kc = psx::ca::CertificateAuthority::generateCsr(*san);
    if (!kc.ok()) {
        err << "pipeshellx node keygen: " << kc.error().message() << "\n";
        return 2;
    }
    if (!writeFile(*outPfx + ".key", kc.value().privateKeyPem, /*secret=*/true, err) ||
        !writeFile(*outPfx + ".csr", kc.value().csrPem, /*secret=*/false, err)) {
        return 2;
    }
    out << "wrote " << (*outPfx + ".key") << " (keep secret) and " << (*outPfx + ".csr")
        << " (send to the CA for `ca sign`)\n";
    return 0;
}

// The `pipeshellx node ...` argv a service manager should launch, built from the
// same flags the daemon takes. nullopt on a missing required flag.
std::optional<std::vector<std::string>> nodeExecArgv(const std::vector<std::string>& args, std::ostream& err) {
    const auto exec = flag(args, 1, "--exec");
    const auto cert = flag(args, 1, "--cert");
    const auto key = flag(args, 1, "--key");
    const auto ca = flag(args, 1, "--ca");
    const auto listen = flag(args, 1, "--listen");
    if (!cert || !key || !ca || !listen) {
        err << "pipeshellx node <systemd-unit|launchd-plist>: --cert F --key F --ca F --listen HOST:PORT are required "
               "(optional --allow SANs, --crl F, --exec PATH)\n";
        return std::nullopt;
    }
    std::vector<std::string> argv{
        exec.value_or("pipeshellx"), "node", "--cert", *cert, "--key", *key, "--ca", *ca, "--listen", *listen};
    if (const auto allow = flag(args, 1, "--allow")) {
        argv.emplace_back("--allow");
        argv.push_back(*allow);
    }
    if (const auto crl = flag(args, 1, "--crl")) {
        argv.emplace_back("--crl");
        argv.push_back(*crl);
    }
    return argv;
}

int nodeSystemdUnit(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto argv = nodeExecArgv(args, err);
    if (!argv) {
        return 2;
    }
    std::string execStart;
    for (std::size_t i = 0; i < argv->size(); ++i) {
        execStart += (i == 0 ? "" : " ") + (*argv)[i];
    }
    const std::string user = flag(args, 1, "--user").value_or("pipeshellx");
    out << "[Unit]\n"
        << "Description=PipeShellX node agent (psx/1 mTLS backplane)\n"
        << "After=network-online.target\n"
        << "Wants=network-online.target\n\n"
        << "[Service]\n"
        << "Type=simple\n"
        << "ExecStart=" << execStart << "\n"
        << "Restart=on-failure\n"
        << "RestartSec=5\n"
        << "User=" << user << "\n"
        << "Group=" << user
        << "\n"
        // Hardening: this daemon runs untrusted remote commands, so confine it.
        << "NoNewPrivileges=yes\n"
        << "ProtectSystem=strict\n"
        << "ProtectHome=yes\n"
        << "PrivateTmp=yes\n"
        << "PrivateDevices=yes\n"
        << "ProtectControlGroups=yes\n"
        << "ProtectKernelModules=yes\n"
        << "ProtectKernelTunables=yes\n"
        << "RestrictAddressFamilies=AF_INET AF_INET6\n"
        << "RestrictNamespaces=yes\n"
        << "LockPersonality=yes\n\n"
        << "[Install]\n"
        << "WantedBy=multi-user.target\n";
    return 0;
}

int nodeLaunchdPlist(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto argv = nodeExecArgv(args, err);
    if (!argv) {
        return 2;
    }
    const std::string label = flag(args, 1, "--label").value_or("com.pipeshellx.node");
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        << "<plist version=\"1.0\">\n"
        << "<dict>\n"
        << "  <key>Label</key>\n  <string>" << label << "</string>\n"
        << "  <key>ProgramArguments</key>\n  <array>\n";
    for (const std::string& arg : *argv) {
        out << "    <string>" << arg << "</string>\n";
    }
    out << "  </array>\n"
        << "  <key>RunAtLoad</key>\n  <true/>\n"
        << "  <key>KeepAlive</key>\n  <true/>\n"
        << "</dict>\n"
        << "</plist>\n";
    return 0;
}

std::string metricsJson(const psx::transport::NodeServer::Metrics& m) {
    std::ostringstream json;
    json << "{\"accepted_total\":" << m.acceptedTotal << ",\"active_connections\":" << m.activeConnections
         << ",\"active_stages\":" << m.activeStages << "}\n";
    return json.str();
}

// Answers each pending control connection with a one-shot metrics snapshot, then
// closes it (the client reads to EOF). The snapshot is tiny, so a single
// non-blocking write drains it; the short loop covers a partial write.
void serveControl(const psx::transport::NodeServer& server, const psx::os::Socket& listener) {
    while (true) {
        auto conn = listener.accept();
        if (!conn.ok()) {
            break; // WouldBlock: no more pending connections
        }
        const std::string body = metricsJson(server.metrics());
        std::size_t sent = 0;
        for (int i = 0; i < 1000 && sent < body.size(); ++i) {
            auto wrote =
                psx::os::write(conn.value().handle(), std::span<const char>(body.data() + sent, body.size() - sent));
            if (wrote.ok()) {
                sent += wrote.value();
            } else if (wrote.error().cls != psx::ErrorClass::WouldBlock) {
                break; // peer gone; drop this one
            }
        }
    }
}

// `node status --control PATH`: read the daemon's metrics snapshot and print it.
int nodeStatus(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto control = flag(args, 1, "--control");
    if (!control) {
        err << "Usage: pipeshellx node status --control PATH\n";
        return 2;
    }
    auto sock = psx::os::Socket::connectUnix(*control);
    if (!sock.ok()) {
        err << "pipeshellx node status: cannot connect to " << *control << ": " << sock.error().message() << "\n";
        return 2;
    }
    std::string body;
    char buffer[512];
    for (int i = 0; i < 5000; ++i) { // poll to EOF (bounded ~5 s)
        auto got = psx::os::read(sock.value().handle(), std::span<char>(buffer, sizeof(buffer)));
        if (got.ok()) {
            if (got.value() == 0) {
                break; // EOF: the daemon finished the snapshot
            }
            body.append(buffer, got.value());
        } else if (got.error().cls == psx::ErrorClass::WouldBlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            err << "pipeshellx node status: " << got.error().message() << "\n";
            return 2;
        }
    }
    out << body;
    return body.empty() ? 2 : 0;
}

} // namespace

int nodeSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (!args.empty() && args[0] == "keygen") {
        return nodeKeygen(args, out, err);
    }
    if (!args.empty() && args[0] == "systemd-unit") {
        return nodeSystemdUnit(args, out, err);
    }
    if (!args.empty() && args[0] == "launchd-plist") {
        return nodeLaunchdPlist(args, out, err);
    }
    if (!args.empty() && args[0] == "status") {
        return nodeStatus(args, out, err);
    }
    const std::size_t from = (!args.empty() && args[0] == "run") ? 1 : 0;

    const auto certPath = flag(args, from, "--cert");
    const auto keyPath = flag(args, from, "--key");
    const auto caPath = flag(args, from, "--ca");
    const auto listen = flag(args, from, "--listen");
    if (!certPath || !keyPath || !caPath || !listen) {
        err << "Usage: pipeshellx node --cert F --key F --ca F --listen HOST:PORT [--allow SAN[,SAN...]] [--crl F] "
               "[--control PATH]\n";
        return 2;
    }

    const auto certPem = readFile(*certPath);
    const auto keyPem = readFile(*keyPath);
    const auto caPem = readFile(*caPath);
    if (!certPem || !keyPem || !caPem) {
        err << "pipeshellx node: cannot read cert/key/ca file\n";
        return 2;
    }
    // Optional CRL: reject peers whose certificate it revokes.
    std::string crlPem;
    if (const auto crlPath = flag(args, from, "--crl")) {
        const auto pem = readFile(*crlPath);
        if (!pem) {
            err << "pipeshellx node: cannot read --crl " << *crlPath << "\n";
            return 2;
        }
        crlPem = *pem;
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
    psx::transport::NodeServer server(
        *reactor.value(), std::move(listener.value()),
        {.certificatePem = *certPem, .privateKeyPem = *keyPem, .caPem = *caPem, .crlPem = crlPem},
        std::move(authorize));
    if (auto started = server.start(); !started.ok()) {
        err << "pipeshellx node: " << started.error().message() << "\n";
        return 2;
    }

    // Optional local control endpoint: an AF_UNIX socket that answers each
    // connection with a metrics snapshot (see `node status`).
    const auto controlPath = flag(args, from, "--control");
    psx::os::Socket controlListener;
    if (controlPath) {
        auto listening = psx::os::Socket::listenUnix(*controlPath);
        if (!listening.ok()) {
            err << "pipeshellx node: cannot listen on control socket " << *controlPath << ": "
                << listening.error().message() << "\n";
            return 2;
        }
        controlListener = std::move(listening.value());
        auto watched = reactor.value()->watch(
            controlListener.handle(), psx::os::Interest::Readable,
            [&server, &controlListener](psx::os::Readiness) { serveControl(server, controlListener); });
        if (!watched.ok()) {
            err << "pipeshellx node: " << watched.error().message() << "\n";
            return 2;
        }
    }

    (void)reactor.value()->onSignal([&reactor](psx::os::Signal) { reactor.value()->stop(); });
    out << "pipeshellx node listening on " << host << ":" << port;
    if (controlPath) {
        out << " (control " << *controlPath << ")";
    }
    out << "\n";
    out.flush();
    (void)reactor.value()->run();
    if (controlPath) {
        std::error_code ec;
        std::filesystem::remove(*controlPath, ec); // clean up the socket file
    }
    return 0;
}

} // namespace psx::cli
