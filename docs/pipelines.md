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
  log-tailing policy (liveness over completeness). `spool` (overflow to a temp
  file) is planned.
- `CreditWindow` — HTTP/2-style per-stream (256 KiB) and per-connection (4 MiB)
  flow control for the Phase 4 native backplane: the sender may not exceed the
  advertised window, replenished by `WINDOW_UPDATE` as the sink drains.
- `Stream` — the `Open → HalfClosed → Closed` state machine that ties a bounded
  buffer to EOF/half-close; `writable()` is the backpressure signal.

Wiring the bounded buffer into the run so that `--stream` keeps a flat
controller RSS under a slow downstream (the §5.1 exit criterion) is the
remaining piece of the streaming milestone; today output is streamed to the
sink live but also captured unbounded in the run Result.

## Running

```bash
# grouped (default)
pipeshellx run -i fleet.ini -g web -- uptime

# live, host-tagged, colourised on a TTY
pipeshellx run -i fleet.ini -g web --stream -- tail -F /var/log/nginx/access.log

# machine-readable
pipeshellx run -i fleet.ini --json -- df -h
```

Host selection is `-g GROUP`, `-t TAG`, `-H h1,h2,…`, or all (see
`docs/authentication.md` for the inventory format). Exit codes: `0` all stages
succeeded, `1` some stage failed, `2` usage/config, `3` no hosts selected.

## Roadmap (Phase 5)

`pipeshellx pipe` will connect stages placed on different nodes with `'|'`
edges (`'cmd'@host '|' 'cmd'@local`), and `pipeline.yaml` will describe general
DAGs. `merge`, `group`, `json`, `consensus`, and `spool` become sink *stages*
in that model; `run` is `pipe` with a merge sink. See `PLAN.md` §5.
