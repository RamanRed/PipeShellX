# Security Considerations

## Threat Model

This system executes operating system commands on behalf of a user. The main security risks are:

- command injection
- arbitrary executable launch
- path abuse
- unsafe shell expansion
- privilege misuse
- denial of service through hung or noisy child processes

## Current Security Controls

### No Shell Execution

The system does not use:

- `system()`
- `popen()`
- `/bin/sh -c`

Commands are executed directly through `execvp()` with an argument vector. This removes shell expansion behavior such as:

- pipes
- command chaining
- variable expansion
- redirection syntax
- subshell execution

### Command Allowlist

Only a fixed set of commands is allowed:

- `ls`
- `cat`
- `echo`
- `pwd`
- `whoami`
- `date`
- `uptime`
- `df`
- `du`
- `ps`
- `id`
- `hostname`

Any command outside this list is rejected. `top` was removed from the list
because it is interactive and hangs the REPL.

### Trusted Executable Resolution

Executables are resolved only from trusted system directories:

- `/bin`
- `/usr/bin`

Explicit paths are rejected. This prevents:

- execution from the current working directory
- user-supplied relative path tricks
- arbitrary path-based binary execution

### Argument Filtering

Arguments are length-bounded and rejected if they contain unsafe shell-like metacharacters such as:

- `;`
- `&`
- `|`
- `` ` ``
- `$`
- `<`
- `>`
- `\`

This is a defense-in-depth layer. The main protection remains the no-shell execution model.

### Process Group Control

Each child is placed into its own process group. On timeout, the system kills the entire group rather than only the direct child. This reduces the risk of runaway descendant processes.

### Resource Limits

Child processes are constrained with:

- CPU limit
- address space limit

These limits reduce impact from abusive or malformed commands, although the values are currently hardcoded.

### SSH Transport Defaults

Remote execution spawns the system OpenSSH client with hardened defaults
(`src/ssh_auth.cpp`, enforced by `tests/test_ssh_auth.cpp`):

- `ssh` is resolved from `PATH`; no hard-coded `/usr/bin/ssh`.
- `StrictHostKeyChecking=accept-new` (OpenSSH ≥ 7.6) with
  `UserKnownHostsFile="<inventory>.known_hosts"`: the key of a host is recorded
  on first contact in a trust store that lives next to the inventory file; a
  changed key fails the host and is reported as `ERROR: host key verification
  failed`. The old `StrictHostKeyChecking=no` (MITM-open) default is gone. The
  path is quoted for OpenSSH's option parser so directories with spaces or `%`
  cannot redirect the trust store.
- On timeout the whole process group of a worker is SIGKILLed; output is
  drained for at most 2 s afterwards so a daemonised grandchild holding the
  pipes cannot keep a run alive.
- `BatchMode=yes` whenever no password is in play, so nothing can hang on an
  interactive prompt; `ConnectTimeout=5` and `ServerAliveInterval=15` always.
- Passwords are never on a command line: the worker child creates a pipe,
  writes the secret into it, and runs `sshpass -d <fd>`. The password is not
  visible in `ps`, not written to `clients.txt`, and not written to the log.

### Logging

Logs default to a file (`$XDG_STATE_HOME/pipeshellx/pipeshellx.log` or
`~/.local/state/pipeshellx/pipeshellx.log`; `--log-file` overrides). Command
lines are logged; passwords and command output are not.

## Remaining Risks

### Allowlisted Command Scope

Some allowed commands expose system information:

- `ps`
- `id`
- `df`
- `du`
- `hostname`

These are not injection risks, but they are policy risks. In a more restricted deployment, the allowlist should be narrower.

### `cat` Risk

`cat` can read files available to the running user. If the execution environment contains sensitive readable files, this is a disclosure risk.

### Environment Inheritance

The child inherits the parent environment. Although executable resolution is constrained, environment variables can still affect subprocess behavior in other ways.

### No User Separation

The current application assumes the permissions of the user running `PipeShellX`. It does not implement:

- privilege dropping
- chroot jail
- namespaces
- seccomp filtering
- per-session OS account separation

## Recommended Hardening

For stronger deployment:

- run the service under a dedicated low-privilege user
- remove high-risk commands from the allowlist
- make resource limits configurable
- add seccomp or platform sandboxing
- restrict accessible filesystem locations
- consider namespaces or container isolation
- ship the log file to a centralized sink
- prefer key, agent, or certificate authentication; password-backed hosts are a compatibility feature

## Security Posture Summary

The current system is substantially safer than a shell-backed executor because it:

- avoids shell invocation
- restricts commands by allowlist
- resolves executables from trusted directories
- validates argument content
- constrains process execution

It is appropriate as a controlled teaching/demo system. It should not be treated as a hardened multi-tenant remote execution service without additional sandboxing and policy controls.
