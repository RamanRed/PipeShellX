# Authentication Guide

## Overview

PipeShellX uses the system OpenSSH client for remote execution.

Depending on the client and the current session state, authentication is handled in one of two ways:

- plain `ssh` for OpenSSH-managed authentication (keys, agent, certificates, `~/.ssh/config`)
- `sshpass -d <fd> ssh ...` for password-backed clients added interactively

PipeShellX does not implement its own SSH protocol stack or credential exchange (see
[ADR-002](adr/ADR-002-system-openssh-as-agentless-transport.md)).

## The `ssh` invocation

Every remote worker runs `ssh` resolved from `PATH` (OpenSSH ≥ 7.6 is required for
`accept-new`) with these options (`src/ssh_auth.cpp`; each one is pinned by
`tests/test_ssh_auth.cpp`):

```text
ssh -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=<inventory>.known_hosts \
    -o BatchMode=yes \            # omitted for password-backed clients
    -o ConnectTimeout=5 \
    -o ServerAliveInterval=15 \
    [-p <port>] [-i <identity>] user@host 'command'
```

| Option | Why |
|---|---|
| `StrictHostKeyChecking=accept-new` | unknown hosts are recorded on first contact; a host whose key **changes** is refused — this is what protects against man-in-the-middle after the first connection |
| `UserKnownHostsFile="<inventory>.known_hosts"` | one trust store per inventory file (`clients.txt.known_hosts` next to `clients.txt`), so fleet keys stay out of `~/.ssh/known_hosts` and can be shipped with the inventory. The value is double-quoted with `%` doubled because OpenSSH parses `-o` values like config lines. Entries that were not loaded from an inventory (hand-built `ClientEntry` objects, e.g. the bench harness) carry no path and use OpenSSH's default `~/.ssh/known_hosts` |
| `BatchMode=yes` | no interactive prompt can ever be answered, so a misconfigured host fails fast instead of hanging a worker; it is dropped only when `sshpass` has to answer a password prompt |
| `ConnectTimeout=5` | unreachable hosts are reported within seconds |
| `ServerAliveInterval=15` | a hung network is detected instead of waiting forever |

### Host key changes

When a recorded key no longer matches, OpenSSH prints its
`REMOTE HOST IDENTIFICATION HAS CHANGED!` banner and exits. PipeShellX reports:

```text
CLIENT user@host
ERROR: host key verification failed
```

Inspect the fingerprints, and if the change is legitimate remove the stale line
from `<inventory>.known_hosts` (for example `ssh-keygen -R host -f clients.txt.known_hosts`).
To pre-seed a trust store, run `ssh-keyscan` against the hosts, verify the
fingerprints out of band, and save the output as `<inventory>.known_hosts`.

## Supported Authentication Methods

### Key-Based Authentication

Key-based authentication works through the local OpenSSH client. If the remote
host accepts the user's SSH keys, PipeShellX simply executes the `ssh` command
above. This covers:

- default private keys in `~/.ssh/`
- explicit identity files stored in client configuration (`ssh://user@host?identity=/path/to/key`)
- host-specific key selection configured by OpenSSH

### Password Authentication

Password authentication is supported through `sshpass`, but the password must be
supplied through the interactive shell:

```text
PipeShell > add-client user@192.168.1.10
Password required? (y/n) y
Enter password:
```

After the password is captured, each remote worker child:

1. creates a pipe and writes the password (plus a newline) into it;
2. leaves only the read end open across `exec`;
3. runs `sshpass -d <fd> ssh ... user@host 'command'`.

The password is therefore never part of a command line, never visible in `ps`,
never written to `clients.txt`, and never written to the log. It is kept in
memory only for the lifetime of the running PipeShellX process. Passwords of
4 KiB or more are rejected.

Password authentication is a compatibility feature; keys, agents, or
certificates are recommended and the password path is scheduled for removal in
1.0 (`PLAN.md` §3.6).

### ssh-agent Authentication

If the user has a running `ssh-agent` and the correct key is loaded, PipeShellX
works without any additional application-side configuration. OpenSSH discovers
and uses the agent automatically when the worker runs `ssh`.

### ~/.ssh/config Support

PipeShellX also inherits OpenSSH behavior from `~/.ssh/config`, so the following
can be managed outside the application:

- per-host usernames
- identity files
- proxy or jump-host rules
- agent preferences
- host aliases
- other OpenSSH connection settings

If `~/.ssh/config` already allows a host to be reached with `ssh my-host`,
PipeShellX benefits from the same OpenSSH resolution and authentication
behavior when it invokes `ssh`.

## Client Configuration Behavior

Persistent client configuration in `clients.txt` supports:

- `user@host`
- `ssh://user@host`
- `ssh://user@host:port?identity=/path/to/key`

Passwords are intentionally not allowed in `clients.txt`. If a configuration
entry contains `password=...`, the loader rejects it. This prevents password
persistence on disk. The `known_hosts` location is derived from the inventory
path at load time and is never written to the file.

## Verification and Status Checks

When a client is added, PipeShellX verifies connectivity using the same
authentication method currently attached to that client:

- key / agent / SSH config: plain `ssh`
- interactive password: `sshpass -d <fd>` + `ssh`

The verification command is `echo connected`. If stdout contains `connected`,
the client is marked `ONLINE`; otherwise it is marked `OFFLINE` with the
normalized error. The `status` command uses the same client-scoped path.

## Failure Reporting

SSH failures are normalized to one of:

```text
ERROR: unreachable host
ERROR: connection failed
ERROR: host key verification failed
ERROR: authentication failed
ERROR: command timed out
ERROR: command failed with exit code N
```

This applies to both connection verification and remote command execution.

## Security Notes

Current security properties:

- host keys are verified on every connection after the first (`accept-new`)
- `ssh` is resolved from `PATH`, never a hard-coded path
- passwords entered interactively are stored in memory only
- passwords are passed to `sshpass` over a pipe, never on argv
- passwords are not written to `clients.txt`, the terminal, or the log

Remaining limitations:

- the first connection to a host trusts whatever key it presents (TOFU) unless
  `<inventory>.known_hosts` is pre-seeded
- in-memory passwords live in ordinary `std::string` storage; `SecureString`
  (`mlock`, zeroisation) arrives in Phase 6

## Native backplane identity (`--transport native`)

The psx/1 backplane authenticates with mutual TLS 1.3, not SSH host keys. Each
peer presents an X.509 certificate issued by the fleet CA (`pipeshellx ca`); the
identity is the certificate's SAN URI (e.g. `spiffe://psx/node/n1`). Both ends
require and verify a peer certificate against the CA (`SSL_VERIFY_PEER |
SSL_VERIFY_FAIL_IF_NO_PEER_CERT`), so an unsigned or self-signed peer never
completes the handshake.

CA trust alone proves a peer is *some* node the CA vouched for, not that it is
*this* host. Pin the expected identity per host in the inventory so a
mis-issued or swapped certificate is rejected:

```ini
[fleet]
node-1 san=spiffe://psx/node/n1 native_port=7433
node-2 san=spiffe://psx/node/n2
```

- `san=<uri>` — the controller admits the connection only if the node's
  certificate SAN URI matches exactly; a mismatch fails with
  `peer SAN-URI not authorized`. Omit it to trust any CA-signed peer.
- `native_port=<port>` — the node's backplane port for this host; falls back to
  the run's `--native-port` (default 7433) when unset.

On the node side, `pipeshellx node --allow <SANs>` restricts which controller
identities may connect; with no `--allow` list the node admits any CA-signed
peer (and logs a warning).
