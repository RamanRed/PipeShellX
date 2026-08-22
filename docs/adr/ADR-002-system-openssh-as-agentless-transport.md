# ADR-002: System OpenSSH as the agentless transport

- **Status:** Accepted
- **Date:** 2026-08-22
- **Deciders:** project maintainers
- **Related:** `PLAN.md` §1.3, §3.6 (`SshTransport`), Phase 0 and Phase 2 roadmap; `docs/authentication.md`

## Context

Remote execution needs a transport that works on day one against any host an
operator can already reach, with no agent install. The options were:

1. Embed an SSH implementation (libssh2, libssh, a Rust/Go library via FFI).
2. Spawn the system `ssh` client as a child process per host.
3. Native-only: require the `pipeshellx node` agent everywhere.

Embedding an SSH library duplicates a security-critical protocol stack, loses
`~/.ssh/config`, agents, certificates, `ProxyJump`, FIDO keys, and every
enterprise SSH policy the operator already has, and puts the project on the
hook for SSH CVEs. Native-only defeats the "works against what you have"
promise. The system client has a real cost — one process (≈ 5–8 MB RSS) per
host and a KEX per connection — which bounds agentless scale; that ceiling is
precisely why the native backplane exists (Phase 4).

## Decision

Use the **system OpenSSH client** as the agentless transport, hardened:

- `ssh` is located on `PATH` (never a hard-coded `/usr/bin/ssh`).
- `StrictHostKeyChecking=accept-new` with a **per-inventory** `known_hosts`
  file (`<inventory>.known_hosts`); a changed key fails the host and is
  surfaced as `host key verification failed`. An explicit `--insecure`
  (Phase 2) may relax this and always warns.
- `BatchMode=yes` unless a password prompt must be answered; `ConnectTimeout`
  and `ServerAliveInterval` always set.
- Passwords are a compatibility feature only: delivered via
  `sshpass -d <fd>` from a pipe created in the worker child, never on argv.
  They are deprecated in favour of keys/agent/certificates and removed in 1.0.
- `ControlMaster`/`ControlPersist` multiplexing is opt-in (`--reuse`).

We do **not** embed an SSH library, now or later.

## Consequences

- Every OpenSSH feature (config, agents, certificates, jump hosts, hardware
  keys) works without PipeShellX code, and OpenSSH security updates apply
  automatically.
- The remote process lifetime is tied to the SSH session by `sshd` (`SIGHUP`
  on disconnect), which gives agentless mode its lease semantics for free.
- Agentless fan-out scales to roughly the number of `ssh` processes the
  controller can afford; this is measured (`docs/benchmarks.md`) and is the
  stated reason for `NativeTransport`.
- `sshpass` is an optional runtime dependency only for password-backed hosts.
- Windows controllers need `ssh.exe` (Win32-OpenSSH) discoverable on `PATH`
  (Phase 3).
