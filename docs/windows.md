# Windows

PipeShellX targets Windows in three capability tiers. This page states what
works today, the OS-model differences the design accounts for, and what the
Windows controller port (Phase 3) still needs.

## Support tiers

| Tier | Capability | Status |
|---|---|---|
| **T1** | Windows as an **SSH target** (Win32-OpenSSH `sshd`) from a POSIX controller — needs only Windows-aware remote-command quoting | **Shipped** (`v0.3.0`) |
| **T2** | Windows as a **controller** — native port of the L0–L5 stack, SSH transport via `ssh.exe` | Phase 3 (in progress) |
| **T3** | Windows as a **native node** — the agent as a Windows service (SCM), Job Objects, IOCP | Phase 4 |

### T1 — running commands on Windows hosts (available now)

From a Linux or macOS controller you can run commands on a Windows host that
runs Win32-OpenSSH `sshd` (inbox since Windows 10 1809 / Server 2019). The only
Windows-specific concern is quoting: the remote shell is `cmd.exe` or
PowerShell, not a POSIX shell, so pick the target's shell with `--shell`:

```bash
# cmd.exe target (the default Win32-OpenSSH shell)
pipeshellx run -H win-01 --shell cmd -- ipconfig /all

# PowerShell target
pipeshellx run -H win-01 --shell powershell -- Get-HotFix
```

`--shell` accepts `posix` (default), `cmd`, or `powershell` (alias `pwsh`):

- `posix` — each argument in single quotes, `'` rendered as `'\''`.
- `cmd` — the `CommandLineToArgvW` rules (double-quote + backslash doubling) so a
  Windows program's argv parser recovers each argument exactly.
- `powershell` — single-quoted literals with `'` doubled.

> **Scope of `--shell cmd`/`powershell`:** this is *argument* quoting only. When
> the Win32-OpenSSH default shell is `cmd.exe`, cmd interprets its metacharacters
> (`& | < > ^`) *before* the program's argv parser, and these are **not** escaped.
> Do not pass them as literal arguments to a `cmd` target. PowerShell single-quote
> literals do not have this caveat for the characters they cover.

## Process-model differences (controller port, T2/T3)

The orchestrator is written to these Windows realities (§4.3 of `PLAN.md`):

- **No POSIX signals.** `Graceful` (SIGTERM equivalent) becomes a console control
  event that only console processes honour, so the node falls back to `Kill`
  after the grace period regardless of platform — the same TERM-then-KILL grace
  the POSIX path uses.
- **32-bit exit codes.** A killed Job Object reports `Terminated(0xC000013A)`;
  the run summary renders this as `killed` on every platform.
- **Slower process creation.** `CreateProcessW` is ~10× slower than `posix_spawn`
  (≈ 5–15 ms). The sliding-window scheduler's default concurrency on a Windows
  controller is tuned by *measured* spawn latency, not a fixed constant.
- **Descendant containment.** POSIX process groups can be escaped with `setsid`;
  Job Objects cannot be escaped without `CREATE_BREAKAWAY_FROM_JOB`, so Windows
  gets strictly stronger containment. (Linux reaches parity only with a cgroup v2
  leaf, used when cgroup v2 is writable.)

## I/O-model differences (§4.4)

- **Overlapped named pipes.** Anonymous pipes on Windows are synchronous, so the
  `Pipe` backend creates overlapped **named** pipes with unguessable names and a
  `PIPE_REJECT_REMOTE_CLIENTS | FIRST_PIPE_INSTANCE` ACL.
- **Completion vs readiness.** The public `Stream` API is completion-style. POSIX
  backends issue the `read` on readiness (`epoll`/`kqueue`); the Windows backend
  submits overlapped reads into the stream's buffer via **IOCP**. Buffer-ownership
  rules are identical — the buffer is pinned until completion.
- **Pipe capacity.** Windows pipe buffers are configurable at creation; the credit
  window is chosen ≥ 4× the largest pipe buffer so the kernel pipe is never the
  throughput bottleneck.

## Console, paths, and text (§4.5)

- **Line endings.** Stages on Windows may emit CRLF. Today `LineFramer` always
  normalises a trailing CR to LF (a CR split across chunks is still stripped); the
  planned `--crlf keep|lf` switch will make this selectable.
- **Console encoding.** Output to a Windows console is UTF-8 with VT processing
  enabled; redirected output is raw bytes.
- **Paths.** Inventory and config paths use `std::filesystem` and accept both
  separators; controller config lives under `%APPDATA%` on Windows (XDG dirs
  elsewhere).

## Build & toolchain (§4.6)

| Target | Compiler | Link | Artifact |
|---|---|---|---|
| Windows 10 1809+ / Server 2019+ | MSVC 19.3x (`/std:c++20`), clang-cl | static CRT (`/MT`), OpenSSL static | `pipeshellx-windows-{amd64,arm64}.exe` |

The reactor backend is selected automatically (IOCP on Windows). The shared
`tests/unit/os/` suite is intended to pass unmodified on Windows once the Win32
`os` backend lands; GoogleTest is fetched via `FetchContent` so the test build
needs no preinstalled framework.

## Service install (planned)

The `pipeshellx node` agent (Phase 4, T3) installs as a Windows service via the
Service Control Manager (SCM), mirroring the systemd/launchd units on the other
platforms. Details will be documented here when that lands.

## Current limitations

- **T2 (Windows controller) is not yet built.** The Win32 `os` backend
  (`src/os/win32/`), the IOCP reactor backend, and the `windows-latest` CI matrix
  (MSVC + clang-cl) are Phase 3 work. This repository is currently developed and
  verified on Linux/macOS; the Windows build has not been exercised here.
- **T3 (native Windows node)** depends on the Phase 4 native transport and agent.

Until T2 lands, use a Linux or macOS controller to reach Windows hosts over SSH
(tier T1, above).
