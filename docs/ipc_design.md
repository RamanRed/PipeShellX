# IPC Design

## IPC Model

The controller talks to every child — a local command or an `ssh` worker —
through unnamed pipes owned by `psx::os::Pipe` (`include/psx/os/pipe.hpp`):

- stdin pipe: parent writes, child reads (only when the command has input;
  otherwise the child's stdin is `/dev/null`)
- stdout pipe: child writes, parent reads
- stderr pipe: child writes, parent reads

Both ends of every pipe are **non-inheritable at creation** (`pipe2(O_CLOEXEC)`
on Linux, `pipe` + `FD_CLOEXEC` on Darwin). The child receives exactly the
ends it needs through `posix_spawn` file actions (`dup2` onto descriptors
0–2 clears the close-on-exec flag for the copy only), so a child can never
see a sibling's pipes, the log file, or the reactor's descriptors. This is
verified by listing `/dev/fd` inside a spawned child
(`tests/unit/os/test_process.cpp`).

## Pipe Topology

```text
Parent stdinWriter   ---->  fd 0 of the child      (optional)
Child  fd 1          ---->  stdoutReader in the parent
Child  fd 2          ---->  stderrReader in the parent
Parent passwordPipe  ---->  fd 3 of sshpass        (password-backed SSH workers only)
```

After `Process::spawn()` returns, the parent-side `Pipe` temporaries close the
child's ends; the parent keeps `stdinWriter`, `stdoutReader` and
`stderrReader` as `Handle`s for the duration of the run.

## Draining Without Deadlocks

A child that fills a pipe buffer blocks on `write(2)`; a parent that waits
for process exit before reading would then wait forever. PipeShellX never
does that: every parent-side handle is non-blocking and registered with the
`runtime::Reactor`, which demultiplexes all pipes of all workers on one
thread (`epoll` / `kqueue` / `poll`, see `docs/os_abstraction.md`).

On readiness the owner drains the pipe **until `WouldBlock`** (edge-triggered
discipline; required by `EPOLLET` / `EV_CLEAR` and harmless under `poll`).
End of stream is a zero-byte read or a hang-up with nothing left to read;
the handle is then unregistered and closed. Input is written on
writability and the writer closed after the last byte, so the child sees EOF.

Bounded buffering with backpressure (stop reading a stream whose buffer is
full so the producer blocks) arrives with the L2 `Stream` in Phase 2; today
the per-worker `std::string` still grows with the output.

## Child Exit and Timeouts

Child exits are reactor events too: the `ChildExitSource` (`pidfd` on Linux,
`kqueue EVFILT_PROC` on Darwin) makes `waitpid` a single call per child, made
by its owner after the notification — no `WNOHANG` polling, no `waitpid(-1)`.

A run's deadline is a reactor timer. When it fires, every incomplete worker's
**process group** is `SIGKILL`ed (descendants included, even if the leader
already exited), the worker is marked timed out, and a 2-second drain grace
timer starts; whatever still holds the pipes afterwards (a daemonised
grandchild outside the group) is abandoned and the run completes.

## FD Ownership Rules

| Descriptor | Owner after spawn | Closed by |
|---|---|---|
| child's 0/1/2 (and 3 for `sshpass`) | the child | exec/exit |
| `stdinWriter` | parent `Worker` | reactor handler after the last byte, or at the drain deadline |
| `stdoutReader`, `stderrReader` | parent `Worker` | reactor handler at end of stream, or at the drain deadline |
| password pipe (both ends) | parent, momentarily | immediately after spawn — only the child holds the secret |
| reactor internals (`kqueue`/`epoll`/self-pipe, child-exit and signal sources) | `ProcessManager`'s cached `Reactor` | when the manager is destroyed |

`psx::os::handleStats()` counts every `Handle` created and closed; the soak
tests assert the open count is unchanged after thousands of cycles.

## Stability Measures

- every read/write retries `EINTR`; `SIGPIPE` is an ordinary `BrokenPipe` error
- a child that cannot be started is reported synchronously (exit 127 with the
  reason on stderr) and never leaves a zombie
- exceptions cannot run in a forked child (the fork fallback uses an error
  pipe and `_exit`)
- a `Process` that is still running when its owner goes away is killed and
  reaped by the destructor

## Logging Coverage

IPC-related log lines: pipe creation, child creation, bytes read per stream
(DEBUG, skipped entirely when DEBUG is disabled), stdin completion, timeout,
drain-grace expiry, and exit status — each with timestamp, PID, session ID,
client ID and command context.

## Current Limitations

- Output is still delivered to the terminal after the run completes; live
  `--stream` rendering is a Phase 2 deliverable on top of the same reactor.
- Per-worker output buffers are unbounded until the L2 `Stream` lands.
- Interactive stdin (a TTY for the child) is not a supported use case.
