# Security Policy

PipeShellX executes operating-system commands, locally and on remote hosts
over SSH, on behalf of the operator running it. Security defects are treated
as the highest-priority class of bug.

## Supported versions

| Version | Supported |
|---|---|
| `main` (unreleased) | yes — fixes land here first |
| latest tagged release (`v0.x`) | yes — receives backported fixes |
| older tags | no |

## Reporting a vulnerability

Please **do not** open a public issue for a suspected vulnerability.

1. Use GitHub's private reporting:
   **Security → Report a vulnerability** on
   <https://github.com/patil-rushikesh/PipeShellX/security/advisories/new>.
2. Include: affected version/commit, platform, a minimal reproduction, and the
   impact you believe it has (what an attacker gains).
3. You will receive an acknowledgement within **3 business days** and a
   triage decision (accepted / needs info / not a vulnerability) within
   **10 business days**.

Accepted reports are fixed on `main`, released as a patch tag, and credited in
the release notes unless you ask otherwise. We follow coordinated disclosure:
please allow up to 90 days before publishing details.

## Scope

In scope:

- command validation and the no-shell execution path (`CommandExecutor`,
  `ProcessManager`)
- SSH invocation defaults, host-key handling, and secret handling
  (`ssh_auth`, `ClientManager`, `ClientConfig`)
- anything that lets output of one host be attributed to another, or that
  leaks a password to disk, `ps`, or the log

Out of scope:

- weaknesses in OpenSSH, `sshpass`, or the remote hosts themselves
- running PipeShellX as root or with a deliberately widened allowlist/policy
  (documented as unsupported in `docs/security.md`)

## Secure defaults the project commits to

These are enforced by unit tests (`tests/test_ssh_auth.cpp`) and any change
to them requires an ADR in `docs/adr/`:

- `ssh` is resolved from `PATH`; host keys use `StrictHostKeyChecking=accept-new`
  with a per-inventory `known_hosts` file, so a changed key fails the host
  and is reported as `host key verification failed`.
- `BatchMode=yes` whenever no password is in play — nothing can hang on a prompt.
- Passwords are never placed on a command line: `sshpass -d <fd>` reads them
  from an inherited pipe created in the worker child. They are never written to
  `clients.txt` or to the log.
- Commands are executed via `execvp()` with an argument vector; there is no
  shell on the execution path, locally or remotely (remote arguments are
  single-quoted per argument).

See `docs/security.md` for the threat model and `docs/authentication.md` for
the SSH details.
