# ADR-003: Single coordinator per run; no consensus protocol

- **Status:** Accepted
- **Date:** 2026-08-22
- **Deciders:** project maintainers
- **Related:** `PLAN.md` §1.4 (principle 5 and non-goals), §3.6 (leases), §3.7 (orchestration), §5.3

## Context

Distributed execution tools tend to grow a control plane: a replicated store
of runs, leader election, and eventually a cluster to operate. PipeShellX's
promise is the opposite — "one static binary, no orchestrator": a run must be
startable from a laptop or a cron job with nothing else installed.

The failure we actually need to survive is *controller loss mid-run* without
leaving orphaned work on nodes, and *node loss* without hanging the run.
Neither requires agreement between multiple controllers; both are solved by
leases and idempotent re-execution.

Alternatives considered: Raft-replicated controllers (operational burden,
another binary role, and still needs fencing on nodes); an external store
(etcd/Consul) as the coordinator (violates the no-dependency principle);
"best effort" with no fencing (orphans are unacceptable for production use).

## Decision

- A run has **exactly one coordinator** — the process that started it. There is
  no leader election, replication, or shared state store.
- Liveness is guaranteed by **leases**: native nodes kill every job of a
  connection after three missed 2-second heartbeats; agentless nodes get the
  same guarantee from `sshd`'s `SIGHUP`-on-disconnect.
- Availability comes from **idempotent re-runs**, not from controller
  failover. Stages declare `idempotent` to opt into automatic retry; the spawn
  key `(run_id, stage_id, attempt)` lets nodes reject duplicates within the
  lease window.
- Delivery semantics are stated plainly: at-most-once spawn, at-least-once
  reporting; exactly-once is not promised.
- The consensus *feature* (`--consensus`, `diff`) is an output reduce sink
  that compares results across hosts; it is unrelated to distributed consensus
  and must not be described as such.

## Consequences

- No daemon, database, or cluster is required to run a pipeline; the same
  binary is controller, node, and CLI.
- Controller crash ⇒ every node fences its jobs within ≈ 6 s; the operator
  re-runs. Runs must therefore be designed as re-runnable, which the docs
  state as a requirement rather than a hope.
- Long-lived, must-not-restart workloads (a multi-hour migration that cannot be
  resumed) are explicitly a poor fit and are documented as such.
- Multi-controller scheduling (two operators driving the same fleet) is
  coordinated by the nodes' per-connection leases and by audit logs, not by
  PipeShellX itself.
