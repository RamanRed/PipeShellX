# Architecture

PipeShellX 0.6 is a C++20 controller and optional node agent built from one
lowercase executable, `pipeshellx`. The same binary dispatches
one-shot commands and the legacy interactive shell:

- `run` — fan a command out over SSH or the native transport;
- `ping` — probe SSH hosts;
- `diff` — compare native-host stdout for configuration drift;
- `pipe` — run an inline chain or a restricted-YAML pipeline;
- `hosts` — inspect and safely mutate inventories;
- `ca` and `node` — operate the native mTLS backplane;
- `shell` — the legacy allowlisted teaching/demo REPL.

`pipeshellx --version` reports `PipeShellX 0.6.0`.

## Layered design

```text
main / CLI
  -> inventory selection, policy, CA and audit
  -> orchestration and pipeline planning
  -> sinks and bounded streams
  -> SSH or native transport
  -> reactor
  -> std-only OS interfaces
  -> POSIX implementation
```

Dependencies point downward. Public `include/psx/**` headers expose
standard C++ types; platform headers are confined to `src/os/posix/**`.
`scripts/check_layering.sh` enforces that boundary.

### CLI and product layer

`src/main.cpp` performs command dispatch. Parsers and command handlers
under `src/cli/` reject unknown or incomplete options, resolve hosts,
and translate product exit codes.

Inventory handling lives in `psx::inventory`. It parses INI groups,
tags, SSH connection fields, native SAN/port fields, and per-host
`transport=ssh|native`. The selection layer converts a host into the
transport-neutral `ClientEntry` used by the runners. Legacy
`clients.txt` remains an import/read format.

The optional `psx::policy` component validates a `run` argv
before selection is executed. It is not installed by default and is not a
node-side sandbox. `psx::audit` writes an opt-in JSONL outcome trail.

### Orchestration and pipelines

`psx::pipeline::Planner` validates stage identifiers, declared edges,
acyclicity, and placement. There are two execution shapes:

- all-local graphs run through `DagRunner` using the declared topology,
  including fan-in and fan-out;
- a graph containing any remote placement must currently be one declared
  linear chain. The segmented native runner bridges local and remote stages and
  supports a remote-group fan-in at the first stage.

Non-linear remote DAGs are rejected before execution. SSH cross-node edges are
not implemented.

`run` uses the fan-out orchestration path, including concurrency
windows, timeout/cancellation, fail-fast, retries for explicitly idempotent SSH
commands, and native canaries. `diff` reduces successful native-host
stdout into exact-output buckets. See [Pipelines](pipelines.md).

### Sinks and streams

The sink layer renders grouped, live, JSON, ordered, or consensus output while
preserving stdout/stderr identity. Line framing prevents partial lines from
different hosts being interleaved in streaming mode.

Stream components provide bounded buffers, drop accounting, disk spool, and
native per-stream credit. Lossy policies expose a dropped-byte count rather
than silently claiming complete output. psx/1 does not yet have a separate
connection-wide credit window.

### Transports

The agentless transport launches the system OpenSSH client as a managed local
process. OpenSSH owns authentication, host-key handling, SSH configuration, and
the target remote shell. A concurrency window bounds worker processes and
descriptors.

The native transport is the psx/1 protocol over mutually authenticated TLS 1.3.
One connection multiplexes stages with distinct stdin, stdout, and stderr data,
credit updates, exit frames, liveness probes, and graceful drain. A lost
controller connection fences its node-owned stages. Protocol details and
explicit reconnect limitations are in the
[wire protocol specification](wire_protocol.md).

The node receives versioned argv requests and starts accepted requests as the
node daemon's OS account. Certificate authentication authorizes the
controller. Optional `node --policy FILE` can reject argv before
spawn with exit `126`; without it, identity authorization does not
restrict the command. See [Security](security.md).

### Runtime and OS layer

`psx::runtime::Reactor` is a single-threaded event loop composed from
the L0 interfaces:

- readiness through epoll on Linux, kqueue on macOS/BSD, or poll fallback;
- child-exit delivery through pidfd/kqueue/fallback sources;
- signal delivery through signalfd/kqueue/fallback sources;
- timer callbacks used for deadlines and leases.

`psx::os` owns handles, pipes, byte I/O, processes, sockets, TLS-facing
configuration, console operations, paths, signals, and atomic file rewrite.
On the POSIX hot path, processes use `posix_spawn` with explicit stdio
file actions and their own process groups. A narrow fork fallback exists where
platform resource-limit handling requires it.

## Principal flows

```text
run
  parse -> optional policy -> inventory + selector -> transport decision
      -> SSH worker window OR native controller
      -> channel-aware sink -> summary + optional audit

pipe
  parse inline/file -> plan DAG -> local DagRunner
      OR validate remote linearity -> segmented native runner

diff
  parse -> inventory + selector -> native mTLS fan-out
      -> successful stdout buckets -> consensus/drift report
```

The detailed lifecycle is in [System flow](system_flow.md).

## Transport selection

For `run`, an explicit `--transport ssh|native` overrides
the inventory for the entire selected set. Without an override, every selected
host must declare the same transport; a mixed selection is a configuration
error. This avoids silently routing a native host over SSH or vice versa.

`ping` is SSH-only. `diff` and remote `pipe` are
native-only in v0.6.

## Current platform scope

Linux and macOS are the controller/native-node platforms represented by the
POSIX implementation and CI matrix. Windows is supported only as an OpenSSH
target reached from a POSIX controller with explicit target-shell quoting.

There is no Win32 controller backend, IOCP reactor, native Windows node, or SCM
service. General remote DAGs, SSH cross-node pipes, reconnect/resume, mandatory
policy or privilege separation, per-stage sandboxing, and signed audit are
also deferred. See
[Windows](windows.md) and [the master plan](../PLAN.md).
