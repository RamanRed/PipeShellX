# DS Course Add-On — Overview

**Purpose of this folder:** PipeShellX is being extended for a Distributed
Systems course project/FA. This folder is the working plan. It exists so
that work can be picked up by a different person or a different LLM session
without re-deriving context. Read this file first, then the numbered file
for whatever phase you're working on.

## Framing (read this before writing any code)

PipeShellX is **already a distributed system**: a controller process and N
worker (node) processes, separate address spaces, separate failure domains,
coordinating over a network (mTLS, custom `psx/1` wire protocol). This is
the Master-Worker / Coordinator-Worker architecture — the same category as
Hadoop MapReduce (JobTracker/TaskTracker), Kubernetes (control-plane/kubelet),
Slurm. It is not "not distributed" just because control is centralized.

What it currently has **no code for** (see `docs/architecture.md` L18, which
explicitly says so): logical clocks, election, distributed mutual exclusion,
global snapshots, distributed deadlock detection. That's the real gap versus
a typical DS syllabus, and closing part of it is the point of this folder.

The wire protocol (`psx/1`, see `docs/wire_protocol.md`) is **frozen and
conformance-tested**: the doc explicitly states "New frame types require
session-layer capability negotiation" and existing frame formats are fixed
byte layouts asserted by `tests/unit/transport/*`. Do not casually change
`FrameType`, `Frame`, `PING`/`PONG` payload shape, or the 5-byte `EXIT`
payload. The one sanctioned extension point is `OpenRequest`
(`include/psx/transport/open_request.hpp`), whose own doc comment says:
"Kept deliberately small and versioned; env/limits/timeout can extend the
wire format under a new version byte without breaking older readers."
Use that door, not a new one.

## Phases

| Phase | File | What it adds | Wire protocol touched? | Status |
| --- | --- | --- | --- | --- |
| 1 | `01-lamport-clocks.md` | Lamport logical clocks on stage dispatch (OPEN v2) + local event ticks | Yes -- `OpenRequest` v2 only | **Complete & Integrated** in `NativeController`, `DistributedRunner`, and `NodeStageRunner` with automated E2E tests |
| 2 | `02-cluster-snapshot.md` | Point-in-time cluster state snapshot (simplified Chandy-Lamport), JSONL output + CLI viewer | No | **Complete & Integrated** in `NativeController`, `--snapshot-file` CLI flag, and `pipeshellx snapshot dump` CLI viewer with automated tests |
| 3 | `03-election-stretch.md` | Bully algorithm for controller failover (multi-controller HA) | New, separate connection type -- design doc only, optional stretch goal | Design only, optional stretch |

Do Phase 1 and Phase 2 first -- both are small, additive, and don't put the
core transport at risk. Phase 3 is a bigger, separate subsystem; only start
it if 1 and 2 are done and merged and you still have time before the
deadline.

## Cross-cutting rules

See `04-dos-and-donts.md` for the full list. The short version: match
existing style exactly (`psx::` namespaces, `psx::Result<T>` for fallible
calls, no exceptions for control flow, single-reactor-thread => no mutexes
in new runtime code), add new source files to the relevant `CMakeLists.txt`,
write a GoogleTest file for anything new under `tests/unit/`, and never
weaken an existing protocol invariant to make a feature easier to add.

## Completed Implementation

- `include/psx/runtime/lamport_clock.hpp` & `tests/unit/runtime/test_lamport_clock.cpp`:
  Header-only Lamport clock with `tick()`, `observe()`, and `value()`.
- `include/psx/transport/wire.hpp`: Big-endian `writeU64BE` / `readU64BE` wire encoding.
- `include/psx/transport/open_request.hpp` & `src/transport/open_request.cpp`:
  `OPEN` v2 protocol with `lamportTs` field; backward-compatible v1 decoding.
- `src/transport/session.cpp`: Automatically routes `encodeOpenV2` when `lamportTs != 0`.
- `include/psx/transport/native_controller.hpp` & `src/transport/native_controller.cpp`:
  Ticking `clock_.tick()` on launch; passing `lamportTs` to `OpenRequest` and returning in `HostResult`.
- `include/psx/pipeline/distributed_runner.hpp` & `src/pipeline/distributed_runner.cpp`:
  Ticking `clock_.tick()` on remote pipeline stage dispatch and exposing in `Outcome.stageLamportTimestamps`.
- `include/psx/transport/node_stage_runner.hpp` & `src/transport/node_stage_runner.cpp`:
  Observing controller timestamps on `onOpen` and advancing node clock.
- `include/psx/runtime/cluster_snapshot.hpp` & `src/runtime/cluster_snapshot.cpp`:
  `ClusterSnapshot` and `NodeSnapshot` data structures, JSONL serialization, parsing (`fromJsonLine`), file loading (`readFromFile`), and tabular rendering (`formatTable`).
- `NativeController` Snapshot Wiring: `takeSnapshot()` automatically appends global cluster state to `--snapshot-file` on stage launch, completion, and exit.
- CLI Integration: `--snapshot-file <PATH>` in `pipeshellx run`, and new CLI subcommand `pipeshellx snapshot [dump] <PATH> [--latest] [--json]`.
- Comprehensive Unit & Integration Tests:
  - `tests/unit/runtime/test_lamport_clock.cpp`
  - `tests/unit/runtime/test_cluster_snapshot.cpp`
  - `tests/unit/cli/test_snapshot_command.cpp`
  - `tests/unit/cli/test_run_command.cpp`
  - `tests/unit/transport/test_open_request.cpp`
  - `tests/unit/transport/test_wire.cpp`
  - `tests/unit/transport/test_native_transport.cpp` (Lamport integration and Cluster Snapshot integration tests).
