# ADR-001: Completion-style runtime API over readiness and completion backends

- **Status:** Accepted
- **Date:** 2026-08-22
- **Deciders:** project maintainers
- **Related:** `PLAN.md` §3.4 (L1 Runtime), §4.4 (I/O-model differences), Phase 1 and Phase 3 roadmap

## Context

PipeShellX must multiplex thousands of pipe and socket handles from a single
thread on Linux, macOS, and Windows. The kernels disagree on the shape of the
primitive:

- `epoll` (Linux) and `kqueue` (macOS/BSD) are **readiness** demultiplexers:
  they report that a handle *can* be read, and the caller then performs the
  non-blocking read.
- Windows I/O Completion Ports are a **completion** mechanism: the caller
  submits an overlapped read and is told when the bytes *have* arrived.

Emulating readiness on top of IOCP requires zero-byte-read tricks that do not
work for pipes at all; emulating completion on top of readiness is trivial
(issue the non-blocking read immediately after the readiness event). Today's
`executeRemote()` loop already does drain-until-`EAGAIN`, which is exactly the
edge-triggered pattern a completion facade needs.

Alternatives considered: (a) readiness API with a Windows emulation layer —
rejected for the pipe problem above; (b) a thread-per-handle blocking model —
rejected by the "bounded everything" principle and by the cost at 1 000 hosts;
(c) depending on libuv/ASIO — rejected by the single-static-binary and
"OS-native first" principles (and the reactor is small).

## Decision

The runtime exposes a **completion-oriented** API: `stream.read(buffer)` yields
an event carrying the bytes (or EOF/error) when done. Backends:

| Backend | Platform | Mechanism |
|---|---|---|
| `epoll` | Linux | `EPOLL_CTL_ADD` + `EPOLLET`; readiness → immediate non-blocking read |
| `kqueue` | macOS / BSD | `EV_ADD \| EV_CLEAR`; processes, signals, and timers share the queue |
| `poll` | portable fallback | today's rebuilt `pollfd` loop; behaviour reference for golden tests |
| `iocp` | Windows | overlapped `ReadFile`/`WSARecv` + `GetQueuedCompletionStatusEx` |

One reactor thread owns all handles. CPU-heavy work (TLS records, hashing,
JSON encoding) goes to a bounded worker pool that wakes the reactor through a
wake-up handle (`eventfd` / `EVFILT_USER` / `PostQueuedCompletionStatus`).
Child exit and signals are reactor events (`ChildExitSource`, `SignalSource`)
so that `waitpid` is called exactly once per child, on notification.

## Consequences

- The same stream and orchestration code runs unchanged on all three platforms;
  only `src/runtime/` backends differ.
- Edge-triggered semantics are mandatory: every read drains until `EAGAIN`,
  and a stream whose bounded buffer is full is *deregistered* rather than
  read — this is how pipe-level backpressure works (§3.5).
- The `poll` backend must remain behaviour-identical so that it can serve as
  the oracle for golden tests and as the fallback on exotic platforms.
- The thread-per-command and spin-wait in `TerminalClient` and the
  thread-per-session `SessionManager` are removed in Phases 1–2.
