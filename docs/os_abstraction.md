# OS Abstraction Layer (`psx::os`)

`include/psx/os/` is the only place PipeShellX touches the operating system.
Every public header uses `std::` types only; the platform code lives under
`src/os/posix/` (and `src/os/win32/` from Phase 3). The CI lint
`scripts/check_layering.sh` fails the build when a platform header appears
anywhere else, when a POSIX/Win32 type leaks into `include/psx/`, or when a
layer includes a layer above it.

This is the living copy of the primitive mapping from `PLAN.md` §4.2.

## Layers

```text
L1 runtime   include/psx/runtime/reactor.hpp      src/runtime/reactor.cpp (platform-agnostic)
L0 os        include/psx/os/*.hpp                 src/os/posix/*.cpp       (the only syscalls)
             psx::Result<T> / psx::Error          include/psx/result.hpp
```

Dependency rule: a layer includes only layers at or below its own. The
application code (`ProcessManager`, the CLI, pipelines, transports, and
the legacy REPL) sits above these layers. L2 streams and both SSH/native L3
transports are present in v0.6; the Win32 L0/L1 implementation remains
deferred.

## Error model (ADR-005)

L0 and L1 never throw. Every fallible call returns `psx::Result<T>` carrying
either a `T` or a `psx::Error{cls, code, op}` where `cls` is a portable
`ErrorClass` (`WouldBlock`, `BrokenPipe`, `Closed`, `NotFound`, …), `code`
the raw `errno`/`GetLastError()` and `op` the operation name.
`Error::message()` renders all three. `PSX_TRY(expr)` propagates failures.

## Primitives

| Primitive | Contract | Linux | macOS | Windows (Phase 3) |
|---|---|---|---|---|
| `Handle` | owns one kernel object; closed exactly once; **non-inheritable at creation**; `duplicate()`, `setNonBlocking()`; process-wide `handleStats()` | `int` + `O_CLOEXEC`, `F_DUPFD_CLOEXEC` | `int` + `FD_CLOEXEC` set immediately after creation | `HANDLE`, `bInheritHandle=FALSE` |
| `Pipe` | `{reader, writer}`; blocking by default | `pipe2(O_CLOEXEC)` | `pipe` + `fcntl(FD_CLOEXEC)` | overlapped named pipe pair |
| `read()` / `write()` (`io.hpp`) | byte I/O with `EINTR` retried; `0` bytes = end of stream; `WouldBlock`, `BrokenPipe`, `Closed` classes; `ignoreBrokenPipeSignal()` | `read`/`write`, `SIGPIPE` ignored | same | `ReadFile`/`WriteFile` |
| `Process` | `spawn(SpawnSpec)` with explicit stdio (`Inherit`/`Null`/`Handle`), extra handles at fixed descriptors, env, cwd, limits; own process group; `signal(Graceful\|Kill)` hits the **group** (also after the leader was reaped); `wait()`/`tryWait()` cache the status; `release()`; destructor kills and reaps | `posix_spawn` + `SETPGROUP/SETSIGMASK/SETSIGDEF` + `closefrom`, `prlimit()` after spawn, `addchdir` | `posix_spawn` + `CLOEXEC_DEFAULT`, `addchdir`; **fork fallback** (async-signal-safe, error pipe) only for resource limits | `CreateProcessW` + handle list + Job Object |
| `ExitStatus` | `{Exited(code) \| Signaled(sig) \| Terminated(code)}` | `WIFEXITED`/`WIFSIGNALED` | same | `GetExitCodeProcess` |
| `Poller` | readiness demultiplexer keyed by caller tokens; `Interest::None` silences, re-arm reports pending readiness; `wake()` from any thread | `epoll` (`EPOLLET\|EPOLLRDHUP`, `eventfd` wake) | `kqueue` (`EV_CLEAR`, `EVFILT_USER` wake) | IOCP |
| `Poller` (portable) | same interface, level-triggered, `pollfd` set rebuilt per wait — the oracle the others are tested against | `poll` + self-pipe | same | — |
| `ChildExitSource` | one pollable handle readable when any watched child exited; `drain()` reports each pid once; **never reaps** | `pidfd_open` + private `epoll` | `kqueue EVFILT_PROC NOTE_EXIT` | job completion port |
| `ChildExitSource` (portable) | `ChildExitMode::SignalDriven`: `SIGCHLD` → self-pipe, exits confirmed with `waitid(WNOWAIT)` | any POSIX | any POSIX | — |
| `SignalSource` | `Interrupt`/`Terminate`/`Hangup`/`WindowResize` as events; default actions suppressed while subscribed; dispositions and mask restored on destruction | `signalfd` + blocked mask | `kqueue EVFILT_SIGNAL` + `SIG_IGN` | `SetConsoleCtrlHandler` |
| `Console` | `isInteractive(stream)`, `readSecret(prompt)` (echo off, terminal restored) | `isatty`, `termios` | same | `SetConsoleMode` |
| `paths` | `homeDirectory()`, `stateDirectory(app)` | `$HOME`/`getpwuid_r`, `$XDG_STATE_HOME`/`~/.local/state` | same | `%LOCALAPPDATA%` |
| `system` | `currentProcessId()`, `raiseHandleLimit()` (soft → hard, never lowers), `isExecutableFile()` | `setrlimit(RLIMIT_NOFILE)` | capped by `OPEN_MAX` | not needed |

## Invariants (tested in `tests/unit/os/`)

- No handle owned by `psx::os::Handle` is ever inheritable; a spawned child
  sees only descriptors 0–2 plus the `extraHandles` it was given
  (`OsProcessTest.ChildInheritsOnlyItsThreeStdioDescriptors`).
- The descriptor count is identical before and after 10 000 `Pipe` cycles and
  after a `Process` spawn soak (`PIPESHELLX_SOAK=1` runs 10 000 spawns).
- `SIGPIPE` is ignored process-wide once `ignoreBrokenPipeSignal()` ran;
  `BrokenPipe` is an ordinary error.
- The child never executes C++ code between spawn and exec on the fast
  path; the fork fallback runs only async-signal-safe calls.
- `waitpid` is called exactly once per child, by its owner, after the
  `ChildExitSource` reported it.
- Every `Poller` backend passes the same test-suite; `Interest::None`
  followed by a re-arm reports data that arrived in between on all of them.

## Platform notes

- **Darwin** refuses `RLIMIT_AS`; it is applied best-effort (advisory,
  `PLAN.md` §3.8). `posix_spawn` of a missing program leaves a transient,
  self-reaping child for a few milliseconds — never a zombie. macOS 26 ships
  the POSIX.1-2024 `posix_spawn_file_actions_addchdir`; older releases use
  the `_np` spelling.
- **Linux** uses `pidfd_open` (kernel ≥ 5.3, probed at runtime; the
  `SignalDriven` mode is the fallback) and glibc's `closefrom` file action
  (≥ 2.34) as belt and braces.
- `PIPESHELLX_POLLER=poll|epoll|kqueue` forces the reactor backend
  (diagnostics and the CI "poll removable" run).
