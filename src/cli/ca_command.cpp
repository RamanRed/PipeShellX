#include "psx/cli/ca_command.hpp"

#include "psx/ca/certificate_authority.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

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

// Reads a whole file into a string; nullopt if it cannot be opened.
std::optional<std::string> slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int caRevoke(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto caDir = flag(args, 1, "--ca");
    const auto certPath = flag(args, 1, "--cert");
    const auto serialArg = flag(args, 1, "--serial");
    if (!caDir || (!certPath && !serialArg)) {
        err << "pipeshellx ca revoke: --ca DIR and one of --cert FILE / --serial HEX are required\n";
        return 2;
    }
    const std::filesystem::path caBase(*caDir);
    const auto caKey = slurp(caBase / "ca.key");
    const auto caCert = slurp(caBase / "ca.crt");
    if (!caKey || !caCert) {
        err << "pipeshellx ca revoke: cannot read the CA in " << *caDir << "\n";
        return 2;
    }
    auto ca = psx::ca::CertificateAuthority::load(*caKey, *caCert);
    if (!ca.ok()) {
        err << "pipeshellx ca revoke: " << ca.error().message() << "\n";
        return 2;
    }

    // The serial to revoke: given directly, or read from the leaf certificate.
    std::string serial;
    if (serialArg) {
        serial = *serialArg;
    } else {
        const auto certPem = slurp(*certPath);
        if (!certPem) {
            err << "pipeshellx ca revoke: cannot read --cert " << *certPath << "\n";
            return 2;
        }
        auto s = psx::ca::CertificateAuthority::serialHex(*certPem);
        if (!s.ok()) {
            err << "pipeshellx ca revoke: " << s.error().message() << "\n";
            return 2;
        }
        serial = s.value();
    }
    for (char& c : serial) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); // match serialHex's casing
    }

    // Maintain the CA's revocation list, then regenerate the CRL from all of it.
    const std::filesystem::path revPath = caBase / "revoked.txt";
    std::vector<std::string> serials;
    if (const auto existing = slurp(revPath)) {
        std::istringstream lines(*existing);
        for (std::string line; std::getline(lines, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                serials.push_back(line);
            }
        }
    }
    if (std::find(serials.begin(), serials.end(), serial) == serials.end()) {
        serials.push_back(serial);
    }

    auto crl = ca.value().issueCrl(serials);
    if (!crl.ok()) {
        err << "pipeshellx ca revoke: " << crl.error().message() << "\n";
        return 2;
    }
    std::string revText;
    for (const std::string& s : serials) {
        revText += s + "\n";
    }
    if (!writeFile(revPath, revText, /*secret=*/false, err) ||
        !writeFile(caBase / "crl.pem", crl.value(), /*secret=*/false, err)) {
        return 2;
    }
    out << "revoked " << serial << "; CRL now lists " << serials.size() << ": " << (caBase / "crl.pem").string()
        << "\n";
    return 0;
}

// `ca sign`: turn a node's CSR into a certificate. The cert takes the CSR's
// public key but the operator-supplied SAN (the CSR's requested identity is not
// trusted). Enrollment step 2; pairs with `node keygen`.
int caSign(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto caDir = flag(args, 1, "--ca");
    const auto csrPath = flag(args, 1, "--csr");
    const auto san = flag(args, 1, "--san");
    const auto outPath = flag(args, 1, "--out");
    if (!caDir || !csrPath || !san || !outPath) {
        err << "pipeshellx ca sign: --ca DIR, --csr FILE, --san URI and --out FILE are required\n";
        return 2;
    }
    const std::filesystem::path caBase(*caDir);
    const auto caKey = slurp(caBase / "ca.key");
    const auto caCert = slurp(caBase / "ca.crt");
    if (!caKey || !caCert) {
        err << "pipeshellx ca sign: cannot read the CA in " << *caDir << "\n";
        return 2;
    }
    auto ca = psx::ca::CertificateAuthority::load(*caKey, *caCert);
    if (!ca.ok()) {
        err << "pipeshellx ca sign: " << ca.error().message() << "\n";
        return 2;
    }
    const auto csr = slurp(*csrPath);
    if (!csr) {
        err << "pipeshellx ca sign: cannot read --csr " << *csrPath << "\n";
        return 2;
    }
    auto cert = ca.value().signCsr(*csr, *san);
    if (!cert.ok()) {
        err << "pipeshellx ca sign: " << cert.error().message() << "\n";
        return 2;
    }
    if (!writeFile(*outPath, cert.value(), /*secret=*/false, err)) {
        return 2;
    }
    out << "signed " << *san << ": " << *outPath << "\n";
    return 0;
}

} // namespace

int caSubcommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        err << "Usage: pipeshellx ca <init|issue|revoke|sign> ...\n";
        return 2;
    }
    if (args[0] == "init") {
        return caInit(args, out, err);
    }
    if (args[0] == "issue") {
        return caIssue(args, out, err);
    }
    if (args[0] == "revoke") {
        return caRevoke(args, out, err);
    }
    if (args[0] == "sign") {
        return caSign(args, out, err);
    }
    err << "pipeshellx ca: unknown action '" << args[0] << "' (expected init|issue|revoke|sign)\n";
    return 2;
}

} // namespace psx::cli
