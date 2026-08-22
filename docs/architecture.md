# Architecture Overview

## Purpose

This project is a remote command execution system built on explicit operating-system primitives:

- process creation with `posix_spawn()` (no `fork()` on the hot path)
- parent/child communication with unnamed pipes that are non-inheritable at creation
- I/O redirection with `posix_spawn` file actions
- event demultiplexing with `epoll` / `kqueue` / `poll` behind one single-threaded reactor
- pollable child exits (`pidfd`, `kqueue EVFILT_PROC`) and operator signals as events

The current system is a layered C++20 application: a terminal client on top of a command execution and process-management core, which in turn sits on the `psx` OS-abstraction and runtime layers described in `docs/os_abstraction.md`. `PLAN.md` §3 describes where the layering is going (streams, transports, pipelines).

## High-Level Layers

### Entry Point

- `src/main.cpp`

Starts the application, initializes logging, and launches the terminal interface.

### User Interface Layer

- `include/terminal_client.hpp`
- `src/terminal_client.cpp`

Handles interactive command input, command history, colored output, and user-visible error reporting.

### Command Execution Layer

- `include/command_executor.hpp`
- `src/command_executor.cpp`

Responsible for:

- parsing user input into arguments
- validating commands against a strict allowlist
- resolving executables from trusted directories
- creating execution context for logging
- delegating process execution to `ProcessManager`

### Process Management Layer

- `include/process_manager.hpp`
- `src/process_manager.cpp`

Responsible for:

- creating the stdio pipes of each worker (`psx::os::Pipe`)
- spawning local commands and `ssh` workers (`psx::os::Process`)
- draining output and feeding input as reactor events, edge-triggered
- enforcing the run deadline: `SIGKILL` to each worker's process group, a bounded drain grace
- reaping every child exactly once when the exit source reports it
- grouping per-client output and normalizing remote failures

### Runtime Layer (L1)

- `include/psx/runtime/reactor.hpp`
- `src/runtime/reactor.cpp`

A single-threaded event loop composed from the L0 primitives: a `Poller`, a `ChildExitSource`, an optional `SignalSource` and a timer queue. Handlers run on the calling thread and may re-enter the reactor; `stop()`/`wake()` are thread-safe. Platform-agnostic: it includes `psx` headers only.

### OS Abstraction Layer (L0)

- `include/psx/os/*.hpp`, `include/psx/result.hpp`
- `src/os/posix/*.cpp`

`Handle`, `Pipe`, byte I/O, `Process`, `Poller` (`poll`/`kqueue`/`epoll`), `ChildExitSource`, `SignalSource`, `Console`, `paths`, `system`. Public headers use `std::` types only; this directory is the only place platform headers may appear (enforced in CI). See `docs/os_abstraction.md`.

### Logging Layer

- `include/logger.hpp`
- `src/logger.cpp`

Provides centralized logging with:

- timestamp
- log level
- PID
- session ID
- executed command

## Module Responsibilities

### TerminalClient

- interactive shell behavior
- history display
- command dispatch
- rendering stdout/stderr

### CommandExecutor

- input parsing
- command validation
- executable resolution
- execution audit logging

### ProcessManager

- worker lifecycle on the reactor (spawn, drain, exit, deadline)
- per-client result aggregation and error classification

### psx::runtime::Reactor / psx::os

- event demultiplexing, timers, child-exit and signal delivery
- the kernel-facing primitives with the non-inheritable-handle invariant

### Logger

- serialized output
- execution observability

## Dependency Graph

```text
main
  -> Logger                    (psx::os::paths, psx::os::system)
  -> TerminalClient            (psx::os::Console for the hidden password prompt)
       -> CommandExecutor      (psx::os::isExecutableFile for trusted-path lookup)
            -> ProcessManager
                 -> psx::runtime::Reactor
                      -> psx::os::Poller          epoll | kqueue | poll
                      -> psx::os::ChildExitSource pidfd | EVFILT_PROC | SIGCHLD
                      -> psx::os::SignalSource    signalfd | EVFILT_SIGNAL
                 -> psx::os::Process              posix_spawn (+ fork fallback for limits)
                 -> psx::os::Pipe / io            pipe2(O_CLOEXEC), read/write
```

## Current Design Notes

- The runtime command path does not use a shell. This is intentional and reduces injection risk.
- There is exactly one IPC abstraction (`psx::os::Pipe`) and one process abstraction (`psx::os::Process`); the former standalone `Pipe` helper and the thread-per-session `SessionManager` were removed in Phase 1 (git history keeps them).
- Errors in L0/L1 are `psx::Result` values (ADR-005); the application layers above still use exceptions.
- Logging is execution-aware and includes process/session context across the major layers.

## Known Architectural Limitations

- The terminal client still runs each command on a worker thread and spin-waits; Phase 2 rewrites it as a thin client over the reactor-driven `Run`.
- Output callbacks are still invoked after command completion rather than as truly live streamed events (Phase 2 `--stream`).
- Per-worker output buffers are unbounded until the L2 `Stream` with backpressure lands (Phase 2).
