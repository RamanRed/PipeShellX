#include "psx/cli/ca_command.hpp"

#include "psx/ca/certificate_authority.hpp"

#include <filesystem>
#include <fstream>
#include <optional>

namespace psx::cli {

namespace {

// A tiny flag parser: --name value pairs. Returns the value for `flag` or nullopt.
std::optional<std::string> flag(const std::vector<std::string>& args, std::size_t from, const std::string& name) {
    for (std::size_t i = from; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    return std::nullopt;
}

bool writeFile(const std::filesystem::path& path, const std::string& content, bool secret, std::ostream& err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err << "pipeshellx ca: cannot write " << path << "\n";
        return false;
    }
    out << content;
    out.close();
    if (!out) {
        err << "pipeshellx ca: failed writing " << path << "\n";
        return false;
    }
    if (secret) {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
    }
    return true;
}

int caInit(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto cn = flag(args, 1, "--cn");
    const auto dir = flag(args, 1, "--dir");
    if (!cn || !dir) {
        err << "pipeshellx ca init: --cn NAME and --dir DIR are required\n";
        return 2;
    }
    auto ca = psx::ca::CertificateAuthority::create(*cn);
    if (!ca.ok()) {
        err << "pipeshellx ca init: " << ca.error().message() << "\n";
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(*dir, ec);
    const std::filesystem::path base(*dir);
    if (!writeFile(base / "ca.key", ca.value().privateKeyPem(), /*secret=*/true, err) ||
        !writeFile(base / "ca.crt", ca.value().certificatePem(), /*secret=*/false, err)) {
        return 2;
    }
    out << "CA created: " << (base / "ca.crt").string() << "\n";
    return 0;
}

int caIssue(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto san = flag(args, 1, "--san");
    const auto caDir = flag(args, 1, "--ca");
    const auto outPfx = flag(args, 1, "--out");
    if (!san || !caDir || !outPfx) {
        err << "pipeshellx ca issue: --san URI, --ca DIR and --out PREFIX are required\n";
        return 2;
    }
    const std::filesystem::path caBase(*caDir);
    std::ifstream keyIn(caBase / "ca.key", std::ios::binary);
    std::ifstream certIn(caBase / "ca.crt", std::ios::binary);
    if (!keyIn || !certIn) {
        err << "pipeshellx ca issue: cannot read the CA in " << *caDir << "\n";
        return 2;
    }
    const std::string caKey((std::istreambuf_iterator<char>(keyIn)), std::istreambuf_iterator<char>());
    const std::string caCert((std::istreambuf_iterator<char>(certIn)), std::istreambuf_iterator<char>());
    auto ca = psx::ca::CertificateAuthority::load(caKey, caCert);
    if (!ca.ok()) {
        err << "pipeshellx ca issue: " << ca.error().message() << "\n";
        return 2;
    }
    auto identity = ca.value().issue(*san);
    if (!identity.ok()) {
        err << "pipeshellx ca issue: " << identity.error().message() << "\n";
        return 2;
    }
    if (!writeFile(*outPfx + ".key", identity.value().privateKeyPem, /*secret=*/true, err) ||
        !writeFile(*outPfx + ".crt", identity.value().certificatePem, /*secret=*/false, err)) {
        return 2;
    }
    out << "issued " << *san << ": " << (*outPfx + ".crt") << "\n";
    return 0;
}

} // namespace

int caSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        err << "Usage: pipeshellx ca <init|issue> ...\n";
        return 2;
    }
    if (args[0] == "init") {
        return caInit(args, out, err);
    }
    if (args[0] == "issue") {
        return caIssue(args, out, err);
    }
    err << "pipeshellx ca: unknown action '" << args[0] << "' (expected init|issue)\n";
    return 2;
}

} // namespace psx::cli
