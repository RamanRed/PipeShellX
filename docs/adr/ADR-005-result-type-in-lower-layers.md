# ADR-005: `psx::Result<T>` in L0–L2, exceptions permitted from L4 up

- **Status:** Accepted
- **Date:** 2026-08-22
- **Deciders:** project maintainers
- **Related:** `PLAN.md` §3.1 (layers), §3.3–3.5, §3.7 (failure taxonomy); Phase 1 roadmap

## Context

The lower layers — OS primitives (L0), the reactor (L1), and streams (L2) —
run on the hot path of a single-threaded event loop, are called from code that
executes between `fork`/`spawn` and `exec`, and must behave identically on
Windows, where C++ exceptions interact poorly with overlapped-I/O callbacks.
Failures there are *expected values* (`EAGAIN`, `EPIPE`, `ECONNRESET`,
`ERROR_BROKEN_PIPE`), not exceptional events, and must be cheap to propagate.

The upper layers — orchestration (L4) and interfaces (L5) — deal with
configuration, inventory parsing, and user input, where an exception that
unwinds to a single handler that prints a message and exits with code 2 is
the clearest possible code.

Today the code base throws `std::runtime_error` from everywhere, including the
`poll` loop, which makes cleanup paths hard to reason about (see the `catch(...)
{ cleanupWorkers(); throw; }` in `executeRemote`).

## Decision

- L0, L1, and L2 (`psx::os`, `psx::runtime`, `psx::stream`) return
  `psx::Result<T>` (an `expected`-style type carrying `T` or `psx::Error`)
  and are compiled so that they never throw. `psx::Error` carries the portable
  error class (mapped from `errno` / `GetLastError()`), the raw platform code,
  and the operation name.
- L3 (transports) returns `Result` on data paths and may translate to the
  orchestrator's failure taxonomy (`Unreachable`, `AuthFailed`,
  `HostKeyChanged`, …) at its boundary.
- L4 and L5 may throw for configuration and usage errors; `main` maps them to
  the exit-code contract (`2` usage/config, `3` connect/auth/host-key, …).
- No exception crosses a layer boundary downward, and no `Result` is silently
  discarded (`[[nodiscard]]`).
- Phase 0 code keeps its current exception style; the migration happens with
  the Phase 1 `psx::os` refactor, file by file, behind golden tests.

## Consequences

- Hot-path error handling has no unwinding cost and is explicit at every call
  site; fuzzers and fault injection can drive every error branch.
- Windows backends and child-side code are exception-free by construction.
- Upper-layer code stays concise; one handler produces the user-facing message.
- A small amount of boilerplate (`if (!r) return r.error();`) in L0–L2, which
  the `PSX_TRY` helper macro keeps to one line.
