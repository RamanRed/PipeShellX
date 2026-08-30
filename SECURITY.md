# Security Policy and Trust Model

PipeShellX executes operating-system commands locally and on remote hosts. Its
security boundary is the trusted controller operator, that account's
inventory and credentials, and the operating-system account running each
native node. It is not a hardened multi-tenant execution service.

## Supported versions

| Version | Support status |
| --- | --- |
| `main` (currently identifies as 0.6.0; no public tag yet) | Supported; fixes land here. |
| Other commits | Unsupported; no stable GitHub release has been published yet. |

## Report a vulnerability

Do not publish vulnerability details in an issue, discussion, pull request,
log, or example inventory.

Use GitHub's **Security → Report a vulnerability** form when it is available:
<https://github.com/patil-rushikesh/PipeShellX/security/advisories/new>.
Include the affected version or commit, platform, minimal reproduction, and
the impact you believe an attacker gains.

If GitHub does not display that form, private vulnerability reporting is not
enabled. Do not work around that by posting technical details publicly. Open a
minimal public issue that asks the maintainer to establish a private contact
channel, without naming the vulnerable component or including a reproduction.

Expect acknowledgement within three business days and a triage decision
within ten business days. Accepted reports are fixed on `main` and included
in a future release when applicable. Reporters are credited unless they
request otherwise. Please coordinate disclosure, normally allowing up to 90
days for remediation.

## What is in scope

Security issues include, but are not limited to:

- bypassing `run --policy` or `node --policy`, or spawning after a
  node-policy rejection;
- TLS, CA, SAN allowlist or pin, CRL, identity, or downgrade failures;
- native protocol isolation, stdout/stderr attribution, credit, lease, drain,
  or connection-loss fencing failures;
- unsafe OpenSSH invocation, host-key handling, target-shell quoting, or
  credential handling;
- secrets written to inventory, argv, logs, audit records, or another host's
  output;
- descriptor inheritance, cross-stage output, unowned child processes, or
  cancellation and timeout failures;
- unsafe inventory mutation, permission handling, or atomic replacement.

OpenSSH, OpenSSL, `sshpass`, operating-system, and remote-host vulnerabilities
belong to their upstream projects. An unsafe way PipeShellX invokes or
configures one of those components remains in scope here.

A trusted operator intentionally running a command is not a policy bypass.
An unauthorized identity, policy-denied argv, credential disclosure, or data
crossing host or channel boundaries is in scope.

## Trust model

- The controller operator is trusted to select commands and targets and to
  protect inventories, policies, SSH state, certificates, and private keys.
- SSH targets are authenticated according to OpenSSH host-key verification and
  execute as the account selected by inventory or SSH configuration.
- Native nodes trust their configured fleet CA and may narrow that trust with
  controller SAN allowlists. Controllers may pin a node SAN in inventory.
- An admitted native controller is authorized to request arbitrary argv unless
  the node was started with `--policy FILE`. Accepted work runs as the node
  daemon's operating-system account.
- Remote hosts and command output are untrusted input. stdout and stderr must
  remain separate, but total retained output is not always bounded.

Do not run a controller or node as root. Run nodes under dedicated,
least-privileged accounts and do not expose a listener to a CA population or
controller identity that should not have command-execution authority.

## Command-execution surfaces

PipeShellX has no universal allowlist:

| Surface | Security behavior |
| --- | --- |
| `pipeshellx shell` | Legacy teaching/demo REPL with a fixed command allowlist, trusted-directory resolution, argument limits, and metacharacter checks. |
| `pipeshellx run` without `--policy` | Unrestricted trusted-operator surface. |
| `pipeshellx run --policy FILE` | Controller policy rejects before any target is contacted. |
| SSH `run` | Starts the local OpenSSH executable directly, then quotes argv for the target's configured login shell. The remote shell still interprets the command. |
| Native `run`, `diff`, and remote `pipe` | Sends framed argv to an authenticated node, which starts it directly unless argv explicitly requests a shell. |
| Local `pipe` | Starts declared argv directly; a stage may explicitly name a shell. |

`--shell posix|cmd|powershell` selects target-shell quoting. It does not remove
the remote shell or create a sandbox. In particular, `cmd.exe` metacharacters
are not escaped as a general injection defense.

### Optional policies

Controller and node policies use the same line-based format but are loaded and
enforced independently. A policy can allow exact command names, cap argv
length, and reject shell metacharacters. An empty allow set permits any command
name; the default metacharacter guard still applies. The
`allow-shell-metacharacters` directive deliberately removes that guard.

- `run --policy FILE` rejects at the controller before host selection starts.
- `node --policy FILE` rejects a native request before creating pipes or a
  process, writes the diagnostic to that stage's stderr, and returns exit 126.

The controller policy is not transmitted to the node. Configure both
boundaries for defense in depth. Policy does not scrub the environment, drop
privileges, confine filesystem access, filter syscalls, or isolate tenants.

## SSH transport

PipeShellX resolves `ssh` from `PATH` and delegates authentication, host-key
checking, SSH configuration, and remote-shell startup to OpenSSH.

- `StrictHostKeyChecking=accept-new` records an unknown key on first use and
  rejects a later key change.
- `UserKnownHostsFile=<inventory>.known_hosts` isolates trust per inventory.
  Pre-seed and verify it out of band when trust on first use is unacceptable.
- `BatchMode=yes` prevents an unattended prompt when no legacy in-memory
  password is used.
- Connection and keepalive timeouts bound common hangs.
- `--reuse` creates an OpenSSH control socket under PipeShellX state; protect
  that directory as credential-equivalent state.

The legacy interactive password path sends a password to `sshpass -d` over a
pipe, not on argv. Inventory mutation rejects embedded credentials and does not
serialize passwords. Ordinary secret strings are not locked in memory or
guaranteed to be zeroed.

## Native mutual-TLS transport

The psx/1 backplane uses OpenSSL 3 and mutual TLS 1.3:

- both peers must present a certificate chaining to the configured CA;
- inventory `san=` can pin the node's exact SAN URI;
- node `--allow` can restrict controller SAN URIs;
- `--crl` enables CA-issued certificate-revocation checks;
- there is no clear-text downgrade.

Omitting an inventory SAN accepts any node signed by the configured CA.
Omitting `node --allow` admits any controller signed by the configured CA and
emits a warning. Without `node --policy`, that admitted controller can request
arbitrary argv as the node account. Protect CA, controller, and node private
keys; scope CAs and SANs narrowly; distribute and refresh CRLs operationally.

The protocol separates stdin, stdout, and stderr, applies per-stream credit,
uses liveness leases, and fences node-owned stages when their controller
connection is lost. Credit bounds wire data in flight, not total controller
capture. There is no reconnect or resume; a broken connection is terminal.
See the [psx/1 wire protocol](docs/wire_protocol.md) for the framing contract.

## Output, availability, audit, and logs

Timeout, fail-fast, cancellation, process groups, and node fencing terminate
and account for owned work. They reduce accidental leaks and hangs but do not
contain malicious code running with the node account's privileges.

Capture limits depend on the selected overflow policy:

- `drop-oldest` and `drop-newest` bound retained capture and report lost bytes;
- `block` is lossless and unbounded;
- `spool` keeps a bounded in-memory tail but temporary-disk growth and final
  full-result materialization are unbounded.

`run --audit-log FILE` appends unsigned JSONL lifecycle and outcome metadata,
including command argv and topology, but excludes captured stdout and stderr.
An unwritable path warns and execution continues without audit. PipeShellX does
not sign, hash-chain, access-control, rotate, retain, or ship audit data.

Application logs may contain command context and paths. They do not
intentionally contain passwords or captured command output, but command
arguments and topology can still be sensitive. Protect logs and audit files
with filesystem permissions and external retention and integrity controls.

## Inventory mutation

`hosts add`, `remove`, and `import` require an explicit `-i FILE` INI target.
Mutations reject duplicates and recognized secrets, refuse a target whose
basename is `clients.txt`, preserve existing permissions, create new POSIX
files privately, and replace through a same-directory temporary file and
atomic rename.

Atomic replacement prevents a partial file; it is not a transaction across
concurrent writers. Coordinate administrative mutations externally.

## Explicit non-guarantees in v0.6

PipeShellX currently provides none of the following:

- seccomp, Landlock, namespaces, chroot, container, AppContainer, or another
  per-stage sandbox;
- privilege dropping or per-controller OS-account separation at the node;
- mandatory node policy;
- locked or reliably zeroed secret memory;
- signed or tamper-evident audit;
- native reconnect or resume;
- a Windows controller or native Windows node;
- general non-linear remote DAG execution.

For production use, combine dedicated service accounts, narrowly scoped CAs
and SAN allowlists, inventory SAN pins, CRLs, pre-seeded SSH host keys,
controller and node policies, protected filesystems, external audit shipping,
and OS- or container-level isolation. Deployment guidance is in
[docs/deployment.md](docs/deployment.md).
