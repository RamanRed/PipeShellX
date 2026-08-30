# Windows Support

PipeShellX 0.6 does not build as a Windows controller or native Windows node.
Its shipped Windows capability is a POSIX controller reaching a Windows
OpenSSH target.

## Support tiers

| Tier | Capability | v0.6 status |
| --- | --- | --- |
| T1 | Windows as an SSH target from a Linux/macOS controller | Implemented, subject to target-shell quoting limits. |
| T2 | Windows controller using `ssh.exe` | Deferred; there is no Win32 OS/reactor backend or Windows CI job. |
| T3 | Native Windows node/service | Deferred; there is no IOCP/Job Object/SCM implementation. |

The CMake build fails clearly on `WIN32` rather than producing a
partially working executable.

## Windows as an SSH target

A Linux or macOS controller can target a Windows host running Win32-OpenSSH.
Match `--shell` to the SSH server's configured default shell:

```bash
# cmd.exe target
pipeshellx run -i fleet.ini -H win-01 --shell cmd -- ipconfig /all

# PowerShell target
pipeshellx run -i fleet.ini -H win-01 --shell powershell -- Get-HotFix
```

`--shell` accepts:

- `posix` (default) — POSIX single-quote serialization;
- `cmd` — Windows argv quoting using double-quote/backslash rules;
- `powershell` or `pwsh` — PowerShell single-quoted literals.

This option serializes argv for the remote shell; it does not remove the shell
from the path. In particular, the `cmd.exe` metacharacters
`& | < > ^` are not escaped before cmd interpretation. Do not pass
them as literal data to a cmd target. PowerShell quoting has different parsing
rules, so test the exact target configuration.

`ping` can probe Windows SSH targets. Native `diff` and
remote `pipe` cannot target Windows because the Windows node does not
exist.

## Deferred controller/node design

The master plan records intended mappings, not implemented v0.6 behavior:

- `CreateProcessW` plus explicit inherited-handle lists for process
  creation;
- Job Objects for descendant containment and termination;
- IOCP and overlapped named pipes for completion-based I/O;
- console-control events followed by hard termination for cancellation;
- SCM installation for the native node service;
- Windows config directories, UTF-8 console handling, and CRLF policy;
- MSVC and clang-cl build/test coverage.

Do not infer those capabilities from the platform-neutral public headers.
Only the POSIX implementation currently backs them.

## Current operational guidance

Use a supported Linux/macOS controller and SSH transport for Windows hosts.
Keep each Windows host's inventory transport as `ssh` and select the
correct `--shell` for the run:

```ini
[windows]
win-01 user=Administrator transport=ssh tag=prod
```

If a selection mixes Windows SSH hosts and native POSIX nodes, split it or use
an explicit transport override only when every selected host is actually
reachable by that transport.

Windows controller, native-node, service, reconnect, sandbox, and release
artifact work remains open in [PLAN.md](../PLAN.md). No v0.6 claim depends on
those items being complete.
