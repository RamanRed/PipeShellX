# Security Policy

PipeShellX executes operating-system commands locally and on remote hosts on
behalf of an operator. Security defects in its authentication, authorization,
process, protocol, secret-handling, output-attribution, and audit boundaries
are treated as high priority.

## Supported versions

| Version | Supported |
| --- | --- |
| `main` (currently identifies as `0.6.0`; no v0.6 tag yet) | Yes; fixes land here first. |
| Latest tagged release (`v0.5.0` at this audit) | Yes; eligible for backported fixes. |
| Older tags | No. |

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability.

1. Use GitHub's private reporting page:
   <https://github.com/patil-rushikesh/PipeShellX/security/advisories/new>.
2. Include the affected version/commit and platform, a minimal reproduction,
   and the impact you believe an attacker gains.
3. You should receive an acknowledgement within three business days and a
   triage decision within ten business days.

Accepted reports are fixed on `main` and, when applicable, released
as a supported patch. Reporters are credited unless they request otherwise.
Please coordinate public disclosure with the maintainers, normally allowing up
to 90 days for remediation.

## Security scope

Examples of in-scope behavior:

- bypass of `run --policy` or `node --policy`;
- a node policy rejection that still spawns a process;
- native TLS, CA, SAN allowlist/pin, or CRL verification failures;
- native protocol isolation, channel attribution, credit, lease, or fencing
  flaws;
- unsafe OpenSSH invocation, host-key handling, or target-shell quoting;
- secrets written to inventory, argv, logs, audit, or another host's output;
- descriptor inheritance, cross-stage output attribution, orphaned owned
  processes, or cancellation/timeout failures;
- unsafe inventory mutation or file replacement.

OpenSSH, OpenSSL, `sshpass`, the OS, and remote-host vulnerabilities
belong upstream, but an unsafe way PipeShellX invokes or configures them remains
in scope here.

The expected operator model is important: without an optional policy,
`run` is intentionally an unrestricted operator tool, and without
`node --policy` a CA-authorized/allowlisted controller may
intentionally request arbitrary argv. Merely demonstrating that a trusted
operator can run a command is not a policy bypass. Demonstrating that an
unauthorized identity, a policy-denied argv, or one host's data crosses a
boundary is in scope.

Running a controller or node as root greatly increases impact and is strongly
discouraged; it does not excuse a boundary-bypass defect.

## Current security contract

### Execution is not universally shell-free

Local and native stages are started from an argument vector without an
implicitly inserted shell. A caller may explicitly request a shell such as
`sh -c`.

SSH is different. PipeShellX starts the local OpenSSH client directly, then
serializes command argv for the target's configured login shell. That remote
shell necessarily participates in interpretation. `--shell
posix|cmd|powershell` selects quoting rules; it is not a sandbox or a
universal injection defense.

The fixed allowlist and trusted-directory rules in `CommandExecutor`
apply only to the legacy `pipeshellx shell` teaching/demo interface.
They do not govern one-shot `run`, `diff`, `pipe`,
or an authenticated native node.

### SSH

- `ssh` is resolved from `PATH`.
- `StrictHostKeyChecking=accept-new` uses a per-inventory
  `known_hosts` file. Unknown hosts use TOFU; changed keys are
  rejected.
- `BatchMode=yes` is set when no legacy in-memory password is used.
- Legacy password compatibility sends the value to
  `sshpass -d <fd>` through a pipe, not argv.
- Inventory mutation refuses embedded credentials/secrets and never serializes
  passwords.

Pre-seed host keys out of band when TOFU is insufficient.

### Native mTLS

- psx/1 requires mutual TLS 1.3 with CA-signed peer certificates.
- Inventory `san=` pins a node SAN URI; node `--allow`
  restricts controller SAN URIs.
- `--crl` enables revocation checking.
- stdout and stderr are separate channels; bounded credit limits in-flight
  data; connection loss fences stages owned by that controller.

Omitting an inventory SAN trusts any CA-signed node. Omitting
`node --allow` admits any CA-signed controller and emits a warning.
There is no reconnect/resume.

### Optional command policies

The same line-based policy format can be enforced independently:

- `run --policy FILE` rejects before any target is contacted;
- `node --policy FILE` rejects a native request before pipes/process
  are created, writes a diagnostic to stage stderr, and returns exit
  `126`.

Policy supports exact allowed command names, an argv-count cap, and a default
shell-metacharacter guard. It does not scrub the environment, drop privileges,
confine filesystem access, or sandbox syscalls. The controller policy is not
transmitted to the node; configure both boundaries for defense in depth.

When `node --policy` is absent, a CA-authorized/allowlisted controller
may request arbitrary argv. This is the trusted node-operator model, not a
secure default for mutually untrusted tenants.

### Audit and output

`run --audit-log FILE` is opt-in. It records unsigned JSONL lifecycle
and outcome metadata, including command argv/context, but excludes captured
stdout/stderr. An unwritable path warns and continues without audit. The audit
is not signed, hash-chained, access-controlled, or rotated by PipeShellX.

Capture can be bounded and may report dropped bytes. Native channel separation,
cancellation, timeout, abort, and drop metadata are preserved in controller
results. Protect logs and audit files because command arguments and topology
may themselves be sensitive.

## Explicit v0.6 limitations

PipeShellX is not a hardened multi-tenant execution service:

- no stage sandbox, seccomp/Landlock, container boundary, chroot, or
  AppContainer;
- no privilege dropping or per-controller OS account;
- node policy is optional rather than mandatory;
- no locked/zeroing secret-memory type;
- no signed/tamper-evident audit;
- no native reconnect/resume;
- no Windows controller or native Windows node;
- general non-linear remote DAGs are rejected.

Run nodes under dedicated least-privileged accounts, scope CAs and SAN
allowlists narrowly, pin inventory SANs, distribute CRLs, pre-seed SSH host
keys, configure controller and node policies where appropriate, and add
external OS/container isolation and audit protection for production use.

The detailed threat model is in [docs/security.md](docs/security.md);
authentication and inventory behavior are in
[docs/authentication.md](docs/authentication.md); the native protocol contract
is in [docs/wire_protocol.md](docs/wire_protocol.md).
