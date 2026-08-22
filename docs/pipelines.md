# Pipelines and Sinks

This document describes how PipeShellX renders a run's output and, ahead of
Phase 5, where cross-node pipelines are going. Today a "run" is the degenerate
pipeline of §3.2: N parallel stages (one per host) feeding a single terminal
**sink**.

## Sinks

A sink is a terminal renderer for a run (`include/psx/sink/`). It receives
per-stage line events and lifecycle callbacks and decides how to present them.
All three are pure (they write to `std::ostream`), so they are unit-tested
without spawning anything.

| Sink | `pipeshellx run` flag | Output |
|---|---|---|
| `GroupSink` | `--group` (default) | one block per host: `CLIENT <id>` then its stdout lines, then the normalized error (or raw stderr), emitted when the stage finishes |
| `StreamSink` | `--stream` | live `[<host>] <line>` as each line arrives — stdout to stdout, stderr to stderr — with a stable per-host colour on a TTY; a one-line summary (counts, drops, cancellation) to stderr at the end |
| `JsonSink` | `--json` | JSON Lines: one object per stage `{stage, exit, timed_out, error, dropped, stdout, stderr}`, then a summary object (see `docs/json.md`) |

The line events come from a `psx::stream::LineFramer` per (stage, channel):
output is framed into whole lines as bytes arrive on the reactor, so two hosts
multiplexed into one `--stream` terminal never interleave a partial line. A
trailing partial line at EOF is flushed as a complete line.

## Flow control (L2)

Between the remote process and the sink sit the L2 primitives
(`include/psx/stream/`, `docs/os_abstraction.md` for the runtime beneath them):

- `BoundedBuffer` — a capacity-bounded ring with `block` / `drop-oldest` /
  `drop-newest` policies (drop counts reported). `block` is the backpressure
  mechanism: a full buffer stops the reactor reading that pipe, the kernel
  pipe fills, and the producer's `write(2)` blocks. `drop-oldest` is the
  log-tailing policy (liveness over completeness). `spool` bounds memory the
  same way but loses nothing — the overflow is spilled to an anonymous temp
  file and folded back into the captured output at the end (RAM stays flat
  during the run; the full result costs disk instead of drops). An in-memory
  ring can't spool, so `BoundedBuffer`/`Stream` treat `spool` as `block`; the
  spill lives in the output-capture path.
- `CreditWindow` — HTTP/2-style per-stream (256 KiB) and per-connection (4 MiB)
  flow control for the Phase 4 native backplane: the sender may not exceed the
  advertised window, replenished by `WINDOW_UPDATE` as the sink drains.
- `Stream` — the `Open → HalfClosed → Closed` state machine that ties a bounded
  buffer to EOF/half-close; `writable()` is the backpressure signal.

`pipeshellx run --overflow drop-oldest|drop-newest|spool --ring SIZE` (e.g.
`1MiB`, `256KiB`, `4096`) bounds the per-host output the run captures: with
`--stream` the sink already holds nothing, so a bounded ring keeps the
controller's RSS flat under an endless command like `tail -F`. `drop-*` counts
and reports what it discards; `spool` discards nothing (it spills the overflow
to disk and reconstructs the full capture at the end). `--group`/`--json` still
buffer each stage's full output by design (you asked for the complete
block/object). The default `block` policy captures everything in memory.

## Running

```bash
# grouped (default)
pipeshellx run -i fleet.ini -g web -- uptime

# live, host-tagged, colourised on a TTY
pipeshellx run -i fleet.ini -g web --stream -- tail -F /var/log/nginx/access.log

# machine-readable
pipeshellx run -i fleet.ini --json -- df -h
```

`--policy FILE` restricts what may run: a line-based file of `allow <cmd>`,
`max-args <n>`, and `allow-shell-metacharacters` directives — the command is
validated (allowed name, no explicit path, no shell metacharacters, arg count)
before any host is contacted, and rejected with exit `2`. Without it, `run` is
an unrestricted operator tool.

Host selection is `-g GROUP`, `-t TAG`, `-H h1,h2,…`, or all (see
`docs/authentication.md` for the inventory format). `-c N` bounds how many
hosts run at once (default 64, `0` = all): a large fan-out spawns ssh
processes in a sliding window rather than all at once, so the controller's
descriptor and process budget stays bounded. Exit codes: `0` all stages
succeeded, `1` some stage failed, `2` usage/config, `3` no hosts selected,
`130` cancelled by Ctrl-C.

## Reliability and operability

| Flag | Effect |
| --- | --- |
| `--timeout S` | Per-stage and global deadline; a hung host is SIGKILLed and reported as timed out. |
| `--retries N` | Retry a stage that fails with a *transient* transport error (connection refused/timed out/reset, host unreachable) up to `N` more times, with equal-jitter exponential backoff. Auth failures, host-key failures, a command’s own non-zero exit, and timeouts are **not** retried. |
| `--fail-fast` | Stop the whole run as soon as one stage *finally* fails (after any retries): pending hosts never start, in-flight hosts get SIGTERM then a SIGKILL grace, and the run exits non-zero. Aborted stages are reported as `ERROR: aborted (fail-fast)`. |
| `--reuse` | Enable ssh `ControlMaster` connection reuse: repeated runs against the same `user@host:port` share one authenticated master socket (under the state dir) and skip the TCP + key-exchange handshake. |
| `--audit-log FILE` | Append a JSONL audit trail — one `run_started`, one `stage_finished` per host, and one `run_finished` object per line, all sharing the run’s `run_id` and carrying an epoch-millisecond `ts_ms`. An unwritable path degrades to no audit with a warning; it never aborts the run. |

**Cancellation.** Ctrl-C (SIGINT) cancels an in-flight run gracefully — in-flight
hosts get SIGTERM then a SIGKILL grace, the end-of-run summary still prints, and
the process exits `130`.

**Correlation.** Every log line of one run carries `[run=<id>] [stage=s<index>]`;
the same `run_id` keys the audit records, so a run’s logs and audit trail can be
joined offline.

## Roadmap (Phase 5)

`pipeshellx pipe` will connect stages placed on different nodes with `'|'`
edges (`'cmd'@host '|' 'cmd'@local`), and `pipeline.yaml` will describe general
DAGs. `merge`, `group`, `json`, `consensus`, and `spool` become sink *stages*
in that model; `run` is `pipe` with a merge sink. See `PLAN.md` §5.
