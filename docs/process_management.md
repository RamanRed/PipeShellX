# Process Management

## Overview

Child processes are owned by `psx::os::Process` (`include/psx/os/process.hpp`);
`ProcessManager` (`src/process_manager.cpp`) composes them with the
`runtime::Reactor` to run one local command or N `ssh` workers to completion.

Primary responsibilities:

- create children with `posix_spawn()` — no `fork()` on the hot path
- wire stdio through `posix_spawn` file actions (`dup2` onto 0–2, `/dev/null`
  where nothing is connected, extra handles such as the `sshpass` password
  pipe at a fixed descriptor)
- put every child in its own process group; kill the whole group on timeout
- observe exits through a pollable source and reap each child exactly once
- report a tagged `ExitStatus` (`Exited(code)`, `Signaled(sig)`, `Terminated`)

## Lifecycle Steps

### 1. Validate Inputs

`ProcessManager::execute()` requires a non-empty argument vector. Command
parsing and validation occur earlier in `CommandExecutor`. `Process::spawn()`
additionally validates the `SpawnSpec` (valid stdio handles, descriptor
targets ≥ 3, an existing working directory) before anything is created.

### 2. Create Pipes

`psx::os::Pipe::create()` makes the stdout and stderr pipes (and a stdin pipe
when the command has input), all non-inheritable.

### 3. Spawn

`Process::spawn()` builds `posix_spawn_file_actions` (`dup2` for the stdio
handles, `addopen("/dev/null")` for unconnected streams, `addchdir` for a
working directory, glibc's `closefrom(3)` where available) and attributes:

- `POSIX_SPAWN_SETPGROUP` — a new process group whose id is the child's pid
- `POSIX_SPAWN_SETSIGMASK` (empty) and `POSIX_SPAWN_SETSIGDEF` (all) — the
  child starts with default signal handling whatever the parent ignores
- Darwin: `POSIX_SPAWN_CLOEXEC_DEFAULT` as belt and braces

`posix_spawnp` performs the `PATH` lookup when the program has no `/`. A
missing program, directory or permission fails **synchronously** — the
caller gets `NotFound`/`PermissionDenied`, and no reapable child exists.

Resource limits (`Limits{cpuSeconds, addressSpaceBytes, openHandles}`) are
applied with `prlimit()` right after the spawn on Linux. Darwin has no
`prlimit`; when limits are requested the spawn goes through a **fork
fallback** whose child runs only async-signal-safe calls (`sigprocmask`,
`setpgid`, `chdir`, `setrlimit`, `dup2`, `execv`) and reports an exec failure
through a close-on-exec error pipe, then `_exit(127)`. `RLIMIT_AS` is advisory
(Darwin refuses it); `RLIMIT_CPU` and `RLIMIT_NOFILE` are strict.

Local commands keep the v0.1.0 caps (`RLIMIT_CPU` 5 s, `RLIMIT_AS` 64 MiB);
per-stage limits become configurable in Phase 6.

### 4. Run

`ProcessManager` registers the parent-side pipe handles with the reactor,
asks the `ChildExitSource` to watch the child, and schedules the deadline
timer. Output is drained edge-triggered on readiness, input written on
writability, and the exit handled when the source reports it (see
`docs/ipc_design.md`).

### 5. Reap

The exit handler calls `Process::tryWait()` — exactly one `waitpid` per
child, for that child's pid only. `waitpid(-1)` is never used, so reaping
one child can never steal another's status (regression-tested).

## Timeout Handling

When the deadline timer fires, every incomplete worker receives
`Process::signal(StopSignal::Kill)`: `SIGKILL` to the **process group**,
which still works after the group leader exited and was reaped — a
`sh -c 'sleep 30 & exit 0'` dies together with its backgrounded `sleep`.
`StopSignal::Graceful` sends `SIGTERM` the same way. The one-shot `run` and
`pipe` paths register interrupt handling with the reactor: Ctrl-C cancels
owned work, reaps it, and returns `130`. Native-controller cancellation uses
the same outcome vocabulary (`timedOut`, `cancelled`, and `aborted`) while the
node owns and reaps the remote process group.

After the kill a 2-second drain grace collects remaining output; a holder
outside the process group (e.g. a `setsid` daemon) cannot stall the run
beyond that.

## Safety Properties

For processes spawned directly through this layer, process management
explicitly guarantees:

- argv is passed to `posix_spawn`/`exec` without an implicitly inserted local
  shell; a caller may still explicitly launch a shell, and SSH execution is
  serialized for the target's remote shell
- no descriptor other than 0–2 (plus explicitly granted extra handles) is
  visible to any child
- no zombie: every child is reaped by its owner, and a `Process` still
  running when its owner is destroyed is killed and reaped
- no C++ code runs in a forked child
- a parent exception never unwinds into a child (fork fallback has a
  `catch`-free, `_exit`-only child path)

## Remaining Gaps

- Resource limits are still hardcoded for local commands (Phase 6).
- Configurable per-stage limits, sandboxing, and privilege separation are not
  implemented.
- Native reconnect/resume is not implemented; connection loss is terminal and
  fences the affected node stages.
- Windows process creation (`CreateProcessW` + Job Objects) is Phase 3.
