# Security Model

PipeShellX is a remote-execution tool. Its security boundary is the operator
account, the inventory and credentials that account controls, and (for native
transport) the operating-system account running each node. It is not a
multi-tenant sandbox.

## Trust model

- The controller operator is trusted to choose commands and target hosts.
- SSH targets are trusted through OpenSSH host-key verification and the
  account selected by the inventory/OpenSSH configuration.
- Native nodes trust the fleet CA and may additionally restrict controller
  certificate SAN URIs with `node --allow`.
- An authenticated native controller is powerful: unless the node was started
  with `--policy FILE`, it may request arbitrary argv. An accepted
  request runs as the node daemon's OS account.
- Command output and remote hosts are untrusted input. Output is kept on its
  stdout/stderr channel and bounded according to the selected capture policy.

Run native nodes under a dedicated, least-privileged account. Do not expose a
node listener to a CA population or controller identity that should not have
command-execution authority.

## Execution surfaces

The commands do not share one universal allowlist:

| Surface | Current behavior |
| --- | --- |
| `pipeshellx shell` | Legacy teaching/demo REPL. `CommandExecutor` applies its fixed command allowlist, trusted-directory resolution, argument limits, and metacharacter checks. |
| `pipeshellx run` without `--policy` | Unrestricted operator tool. The supplied argv is allowed. |
| `pipeshellx run --policy FILE` | Controller-side policy may allow command names, cap argv length, and reject shell metacharacters before any host is contacted. It is not a node-side sandbox. |
| SSH `run` | PipeShellX starts the local OpenSSH client directly, but serializes argv for the target's configured remote shell. The remote shell participates in interpretation. |
| Native `run`, `diff`, and remote `pipe` | The authenticated node receives argv in a framed request and starts it directly. An operator may explicitly request a shell such as `sh -c`. Optional node `--policy` can reject the request before spawn. |
| Local `pipe` | Stages are started from argv. A pipeline file can likewise name a shell explicitly. |

The optional policy format is line based:

```text
allow uptime
allow systemctl
max-args 8
# Add this only when the allowed command intentionally accepts them:
allow-shell-metacharacters
```

An empty allow set does not create an allowlist; it permits any command name
(the default metacharacter guard still applies). The same policy format can be
loaded independently at two boundaries:

- `run --policy FILE` rejects on the controller before contacting a
  host;
- `node --policy FILE` rejects a native `OPEN` before
  creating pipes or a process, writes a policy diagnostic to that stage's
  stderr channel, and returns stage exit `126`.

The controller policy is not transmitted to or trusted by the node. For
defense in depth, configure both when the fleet should have a restricted
command surface. Service-unit generators preserve the node policy flag.

## SSH transport

PipeShellX delegates authentication and remote-shell execution to the system
OpenSSH client:

- `StrictHostKeyChecking=accept-new` records an unknown key on first use
  and rejects a later key change.
- `UserKnownHostsFile=<inventory>.known_hosts` isolates host trust per
  inventory. Pre-seed and verify this file out of band to avoid TOFU.
- `BatchMode=yes` avoids an unattended prompt when no legacy in-memory
  password is present.
- Connect and keepalive timeouts bound common hangs.
- `--reuse` uses an OpenSSH control socket under the PipeShellX state
  directory; protect that directory as operator credentials.

SSH remote commands are quoted for `posix`, `cmd`, or
`powershell`. Quoting preserves argv for supported cases; it does not
turn the remote shell into a sandbox. In particular, `cmd.exe`
metacharacters are not escaped. See [Authentication](authentication.md) and
[Windows support](windows.md).

The legacy interactive password path sends a password to `sshpass -d`
over a pipe, never on argv. New inventory mutation refuses embedded
`user:password@host` values and secret keys, and legacy imports discard
recognized secret query parameters. Passwords are never serialized to an
inventory. Ordinary in-memory strings are not locked or guaranteed to be
zeroed; secure-memory work remains deferred.

## Native mTLS transport

The psx/1 backplane uses OpenSSL 3 and mutual TLS 1.3:

- both peers must present a certificate chaining to the configured CA;
- the controller can pin a host's exact SAN URI through inventory `san=`;
- a node can restrict controller SAN URIs with `--allow`;
- `--crl` enables CA-issued certificate-revocation checks;
- there is no clear-text downgrade.

CA trust without an inventory SAN pin accepts any CA-signed node. A node with no
`--allow` accepts any CA-signed controller and emits a warning. Use both
identity controls for a scoped fleet. Add `node --policy FILE` when
CA-authorized controllers should be command-restricted; without it, identity
authorization permits arbitrary argv. Protect CA keys, controller private
keys, node private keys, inventories, policies, and CRLs with OS permissions
and an operational rotation process.

The wire protocol separates stdout and stderr, enforces per-stream credit, and
fences stages when a controller connection is lost. It does not yet support
reconnect/resume; a broken connection is terminal. See the
[psx/1 wire protocol](wire_protocol.md).

## Availability and containment

- SSH workers and local stages have dedicated process groups. Timeout,
  fail-fast, and operator cancellation terminate in-flight work and reap owned
  children.
- Native stages are also owned and fenced by the controller connection.
- Drop capture can be bounded with `--ring` and
  `--overflow drop-oldest|drop-newest`; buffering sinks receive only retained
  bytes and report loss. `block` is lossless and unbounded. Spool keeps a
  bounded in-memory tail but has unbounded temporary-disk and final
  materialization costs.
- Credit windows bound native in-flight data, not total lossless capture.
- Resource limits exist in parts of the POSIX process path, but configurable
  per-stage limits are not complete.

These controls reduce accidental resource exhaustion. They do not defend a
node from malicious code executing with the node account's privileges.

## Audit and logs

`run --audit-log FILE` appends unsigned JSON Lines:
`run_started`, one `stage_finished` per selected host, and
`run_finished`. Records contain the command, host/stage identifiers,
outcomes, cancellation/timeout/abort state, attempts, and dropped-byte counts;
they do not contain captured stdout/stderr. Audit is opt-in, and an unwritable
path produces a warning and continues without audit.

The audit file is neither tamper-evident nor signed, has no built-in retention,
and may contain sensitive command arguments. Store it on appropriately
protected storage. Signed audit chains remain Phase 6 work.

Application logs also contain command context. They do not intentionally log
passwords or command output, but commands and paths themselves may be
sensitive.

## Inventory mutation

`hosts add`, `remove`, and `import` require an
explicit `-i FILE` INI target. Mutations:

- reject duplicate hosts and secret-bearing add operands;
- refuse to rewrite a file whose basename is `clients.txt`, because
  that name is reserved for legacy import semantics;
- write a canonical, secret-free inventory with a same-directory temporary
  file and atomic rename;
- preserve the existing target's permissions, and create a new target as a
  private file on POSIX.

Atomic replacement prevents partial files; it is not a transaction across
multiple concurrent writers. Coordinate administrative writes externally.

## Current limitations

The following are explicitly not security guarantees in v0.6:

- no seccomp, Landlock, chroot, namespace, container, AppContainer, or other
  stage sandbox;
- no privilege dropping or per-controller OS-user separation in the node;
- node policy is optional defense in depth, not a mandatory sandbox; without
  `node --policy` an authorized controller may request arbitrary argv;
- no locked/zeroing secret-memory type;
- no signed or hash-chained audit;
- no Windows controller or native Windows node;
- no reconnect/resume after transport loss.

For production use, combine a dedicated node account, tightly scoped CA and
SAN allowlists, inventory SAN pins, CRLs, pre-seeded SSH host keys, optional
controller and node policies, filesystem permissions, external audit shipping,
and OS- or container-level isolation.
