#pragma once

#include "client_config.hpp"
#include "psx/os/pipe.hpp"
#include "psx/os/process.hpp"
#include "psx/result.hpp"

#include <cstddef>
#include <string>

// Builds the process to spawn for running a command on one client over ssh:
// an `ssh` invocation (optionally wrapped in `sshpass -d <fd>` for password
// auth, and with ControlMaster reuse). Owns the transport-specific detail — the
// secret hand-off pipe — so the ProcessManager scheduler stays transport-
// agnostic. This is the seam a future NativeTransport slots into.
class SshTransport {
public:
    // The descriptor the child reads the password from (sshpass -d <fd>).
    static constexpr int kPasswordFd = 3;
    // sshpass hands off at most a page; longer secrets are rejected per-host.
    static constexpr std::size_t kMaxPasswordBytes = 4096;

    struct Options {
        std::string controlPath; // empty => no ControlMaster; ssh %r/%h/%p tokens allowed
        int controlPersistSeconds = 60;
    };

    // No  default: a nested-struct default argument cannot see Options'
    // member initializers while SshTransport is incomplete. Pass Options{} for
    // all-defaults.
    explicit SshTransport(Options options) : options_(std::move(options)) {}

    struct Prepared {
        psx::os::SpawnSpec spec;
        psx::os::Pipe passwordPipe; // reader inherited by the child at kPasswordFd
        std::string failure;        // non-empty => a per-host validation failure; do not spawn
    };

    // Fills `out` for `client` running `remoteCommand`. Returns an error only for
    // a genuine resource failure (pipe create/write) — the caller treats that as
    // fatal. A per-host validation failure (e.g. password too long) is reported
    // via out.failure with an ok() result, so one bad host does not abort the run.
    //
    // out.spec.extraHandles borrows out.passwordPipe.reader by pointer, so `out`
    // must outlive the spawn and must not be moved between prepare() and spawn().
    psx::Result<void> prepare(const ClientEntry& client, const std::string& remoteCommand, Prepared& out) const;

private:
    Options options_;
};
