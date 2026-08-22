#pragma once

#include "client_config.hpp"

#include <optional>
#include <string>
#include <vector>

// Hardened `ssh` argv prefix for one client: `ssh` is resolved from PATH,
// unknown host keys are recorded (`accept-new`) and changed ones rejected,
// the per-inventory known_hosts file is used when the entry carries one, and
// BatchMode is enabled unless a password prompt must be answered by sshpass.
// Throws std::invalid_argument when the known_hosts path cannot be expressed
// as an OpenSSH option value (it contains a double quote or a newline).
std::vector<std::string> buildSshBaseArguments(const ClientEntry& client);

// Full argv for one remote command. A password-backed client requires
// `passwordFd`: an inherited descriptor from which `sshpass -d` reads the
// secret, so it never appears on the command line or in `ps`. Throws
// std::invalid_argument when a password is set but no descriptor is given.
std::vector<std::string>
buildSshCommandArguments(const ClientEntry& client, const std::string& remoteCommand, int passwordFd = -1);

// Maps OpenSSH's stderr to one normalised failure class, in precedence order:
// "host key verification failed", "unreachable host", "connection failed",
// "authentication failed". Empty when stderr matches none of them.
std::optional<std::string> classifySshFailure(const std::string& stderrText);

bool isSshAuthenticationFailure(const std::string& stderrText);
bool isSshHostKeyFailure(const std::string& stderrText);
