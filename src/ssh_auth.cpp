#include "ssh_auth.hpp"

#include <array>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Needles are lower-case; the text is lowered once per classification.
constexpr std::array<std::string_view, 2> kHostKeyNeedles{"host key verification failed",
                                                          "remote host identification has changed"};
constexpr std::array<std::string_view, 3> kUnreachableNeedles{"could not resolve hostname", "name or service not known",
                                                              "temporary failure in name resolution"};
constexpr std::array<std::string_view, 6> kConnectionNeedles{"connection refused", "connection timed out",
                                                             "no route to host",   "operation not permitted",
                                                             "connection reset",   "connection closed"};
constexpr std::array<std::string_view, 3> kAuthNeedles{"permission denied", "authentication failed",
                                                       "no more authentication methods"};

std::string toLowerCopy(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

template <std::size_t N>
bool containsAny(const std::string& loweredText, const std::array<std::string_view, N>& needles) {
    for (std::string_view needle : needles) {
        if (loweredText.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// OpenSSH parses `-o Key=Value` exactly like a config line: the value is
// split on whitespace and `%` sequences are expanded. Quoting it handles
// whitespace; a double quote cannot be represented at all. `escapePercent`
// doubles `%` to a literal (paths like UserKnownHostsFile); leave it off where
// the `%` tokens are wanted (ControlPath's %r/%h/%p).
std::string quoteSshOptionValue(const std::string& value, bool escapePercent = true) {
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '"' || c == '\n' || c == '\r') {
            throw std::invalid_argument("ssh option value contains an unrepresentable character: " + value);
        }
        if (c == '%' && escapePercent) {
            quoted += "%%";
        } else {
            quoted += c;
        }
    }
    quoted += '"';
    return quoted;
}

void addOption(std::vector<std::string>& args, const std::string& option) {
    args.emplace_back("-o");
    args.push_back(option);
}

} // namespace

std::vector<std::string> buildSshBaseArguments(const ClientEntry& client, const SshOptions& options) {
    std::vector<std::string> args{"ssh"}; // resolved from PATH by execvp

    addOption(args, "StrictHostKeyChecking=accept-new");
    if (!options.controlPath.empty()) {
        addOption(args, "ControlMaster=auto");
        addOption(args, "ControlPersist=" + std::to_string(options.controlPersistSeconds) + "s");
        addOption(args, "ControlPath=" + quoteSshOptionValue(options.controlPath, /*escapePercent=*/false));
    }
    if (!client.knownHostsFile.empty()) {
        addOption(args, "UserKnownHostsFile=" + quoteSshOptionValue(client.knownHostsFile));
    }
    if (client.password.empty()) {
        // No interactive prompt can ever be answered; fail fast instead of hanging.
        addOption(args, "BatchMode=yes");
    }
    addOption(args, "ConnectTimeout=5");
    addOption(args, "ServerAliveInterval=15");

    if (client.port != 22) {
        args.emplace_back("-p");
        args.push_back(std::to_string(client.port));
    }
    if (!client.identityFile.empty()) {
        args.emplace_back("-i");
        args.push_back(client.identityFile);
    }

    args.push_back(client.sshTarget());
    return args;
}

std::vector<std::string> buildSshCommandArguments(const ClientEntry& client,
                                                  const std::string& remoteCommand,
                                                  int passwordFd,
                                                  const SshOptions& options) {
    std::vector<std::string> args;
    if (!client.password.empty()) {
        if (passwordFd < 0) {
            throw std::invalid_argument("password-backed client requires a password file descriptor");
        }
        args.emplace_back("sshpass");
        args.emplace_back("-d");
        args.push_back(std::to_string(passwordFd));
    }

    auto sshArgs = buildSshBaseArguments(client, options);
    args.insert(args.end(), sshArgs.begin(), sshArgs.end());
    args.push_back(remoteCommand);
    return args;
}

std::optional<std::string> classifySshFailure(const std::string& stderrText) {
    const std::string lowered = toLowerCopy(stderrText);
    if (containsAny(lowered, kHostKeyNeedles)) {
        return "host key verification failed";
    }
    if (containsAny(lowered, kUnreachableNeedles)) {
        return "unreachable host";
    }
    if (containsAny(lowered, kConnectionNeedles)) {
        return "connection failed";
    }
    if (containsAny(lowered, kAuthNeedles)) {
        return "authentication failed";
    }
    return std::nullopt;
}

bool isSshAuthenticationFailure(const std::string& stderrText) {
    return containsAny(toLowerCopy(stderrText), kAuthNeedles);
}

bool isSshHostKeyFailure(const std::string& stderrText) {
    return containsAny(toLowerCopy(stderrText), kHostKeyNeedles);
}
