#pragma once

#include "client_config.hpp"

#include <optional>
#include <string>
#include <vector>

// Connection-level ssh options (Phase 2). ControlMaster reuse is opt-in
// (`run --reuse`): when controlPath is non-empty the worker adds
// ControlMaster=auto / ControlPersist / ControlPath so a persisted master
// socket lets repeated runs against the same host skip the TCP+KEX handshake.
struct SshOptions {
    std::string controlPath; // empty = no ControlMaster; ssh %r/%h/%p tokens allowed
    int controlPersistSeconds = 60;
};

// Hardened `ssh` argv prefix for one client: `ssh` is resolved from PATH,
// unknown host keys are recorded (`accept-new`) and changed ones rejected,
// the per-inventory known_hosts file is used when the entry carries one, and
// BatchMode is enabled unless a password prompt must be answered by sshpass.
// Throws std::invalid_argument when the known_hosts path cannot be expressed
// as an OpenSSH option value (it contains a double quote or a newline).
// The shell that interprets the remote command line on the target host. A
// POSIX target quotes very differently from a Windows one (cmd.exe / PowerShell),
// so the command is assembled per the target's shell.
enum class RemoteShell { Posix, Cmd, PowerShell };

// Joins `argv` into a single remote command line, quoting each argument for the
// given shell so the remote program receives the arguments verbatim.
//   Posix       -> each arg in single quotes, internal ' as '\''
//   PowerShell  -> each arg in single quotes, internal ' doubled
//   Cmd         -> CommandLineToArgvW rules (double-quote + backslash escaping)
std::string quoteRemoteCommand(const std::vector<std::string>& argv, RemoteShell shell = RemoteShell::Posix);

std::vector<std::string> buildSshBaseArguments(const ClientEntry& client, const SshOptions& options = {});

// Full argv for one remote command. A password-backed client requires
// `passwordFd`: an inherited descriptor from which `sshpass -d` reads the
// secret, so it never appears on the command line or in `ps`. Throws
// std::invalid_argument when a password is set but no descriptor is given.
std::vector<std::string> buildSshCommandArguments(const ClientEntry& client,
                                                  const std::string& remoteCommand,
                                                  int passwordFd = -1,
                                                  const SshOptions& options = {});

// Maps OpenSSH's stderr to one normalised failure class, in precedence order:
// "host key verification failed", "unreachable host", "connection failed",
// "authentication failed". Empty when stderr matches none of them.
std::optional<std::string> classifySshFailure(const std::string& stderrText);

bool isSshAuthenticationFailure(const std::string& stderrText);
bool isSshHostKeyFailure(const std::string& stderrText);

// True for *transient* transport failures worth retrying (host unreachable or a
// failed/refused/reset connection). Auth and host-key failures are permanent
// and return false — retrying them is futile.
bool isRetryableSshFailure(const std::string& stderrText);

// True when ssh reports the remote host key has CHANGED (possible MITM) — a
// graver, distinctly-reported subset of isSshHostKeyFailure().
bool isSshHostKeyChanged(const std::string& stderrText);
