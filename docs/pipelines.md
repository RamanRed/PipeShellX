# Pipelines, Output, and Consensus

PipeShellX 0.6 has two related execution interfaces:

- `run` fans one argv out to selected inventory hosts and feeds a
  terminal sink;
- `pipe` executes a declared process graph.

The executable is `pipeshellx`. Inline and file pipelines share the
same planner, but their current remote topology is deliberately narrower than
their local topology.

## Run sinks

| Sink | Flag | Contract |
| --- | --- | --- |
| Group | `--group` (default) | One completed `CLIENT <host>` block per host. |
| Stream | `--stream` | Live `[host]` whole lines; stdout goes to controller stdout and stderr to controller stderr. |
| JSON | `--json` | One JSON object per completed stage followed by a summary object; see [JSON output](json.md). |
| Consensus | `--consensus` | Exact-output buckets, largest bucket first; `--json --consensus` selects machine-readable rendering. |

`--ordered` wraps the selected sink and emits hosts in stable host
order after completion. Without it, completion/arrival order is intentionally
not deterministic across hosts.

`LineFramer` keeps partial lines from different hosts separate and
flushes a final unterminated line at EOF.

## Capture and flow control

`--ring SIZE` sets the per-channel retained-capture limit for the drop and
spool policies. `--overflow` chooses what happens at that limit:

| Policy | Behavior |
| --- | --- |
| `block` | Lossless, unbounded capture. It ignores `--ring`; use a drop policy for a hard retained-output bound. |
| `drop-oldest` | Keeps the newest `SIZE` bytes and reports how many old bytes were discarded. |
| `drop-newest` | Keeps the first `SIZE` bytes and reports how many new bytes were discarded. |
| `spool` | Keeps a `SIZE`-byte in-memory tail and spills older bytes to a temporary file. Completion reconstructs the lossless result, so disk growth and final materialization are not bounded. |

Buffered group, JSON, consensus, and ordered sinks receive only the retained
post-policy capture. A live `--stream` sink emits every arriving line even when
the separately retained result uses a drop policy. The native transport also
starts with bounded per-stream credit and returns credit once bytes have been
handled by capture/live output. This bounds wire data in flight, not total
lossless capture. There is no separate
connection-wide credit window in psx/1. stdout and stderr remain separate
protocol channels. See
[the psx/1 wire protocol](wire_protocol.md).

## Run reliability

```bash
# SSH fan-out, live output, bounded concurrency
pipeshellx run -i fleet.ini -g web --stream -c 32 -- uptime

# Native fan-out with mTLS and a canary
pipeshellx run -i fleet.ini -g nodes --transport native \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  --canary 10% --fail-fast -- /usr/local/bin/check
```

- `--timeout S` bounds stages.
- `--fail-fast` prevents pending work from starting and aborts
  in-flight siblings after a final failure.
- SSH `--retries N` is active only with
  `--idempotent` and only for classified transient transport errors.
- Native `--canary N|N%` runs a subset first; a failed canary marks
  the remainder skipped.
- Ctrl-C cancels an in-flight run, reports cancellation, reaps owned children,
  and returns `130`.
- `--audit-log FILE` appends unsigned JSONL lifecycle/outcome records
  for SSH and native runs. It excludes captured output and degrades with a
  warning when unwritable.

SSH and native flags are not interchangeable. See
[Distributed execution](distributed_execution.md) and
[Authentication](authentication.md).

## Inline pipelines

An inline stage is a command plus optional `@placement`. The pipe
separator outside quotes creates an edge:

```bash
# All-local declared chain
pipeshellx pipe "'grep ERROR app.log' | 'sort -u' | 'head -n 20'"

# Validate placement and topology without starting a process or connection
pipeshellx pipe --check -i fleet.ini \
  "'grep ERROR /var/log/app.log'@web-01 | 'sort -u'@local"
```

`@local` or an omitted placement executes on the controller.
Otherwise the placement names an inventory host. A placement naming a group is
supported as a fan-in only at the first stage of a remote linear pipeline.

Remote stages use native mTLS and require explicit inventory and controller
credentials:

```bash
pipeshellx pipe -i fleet.ini \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  "'grep -h order=42 /data/log'@shard-01 | 'sort -k1'@local"
```

Local/remote boundaries forward stdout to the next stage's stdin. EOF
half-closes downstream input, and stage exits contribute to pipefail. SSH is
not used for remote pipeline edges in v0.6.

If a downstream stage exits while an upstream local or remote stage is still
producing, the controller fences and accounts the unfinished producer as exit
`137` instead of inventing success or waiting forever. Completion waits for
that cancellation/reap accounting. Normal pipefail still selects the rightmost
nonzero status, so a later explicit downstream failure takes precedence.

## Pipeline files and local general DAGs

`pipe --file FILE` loads a dependency-free restricted YAML shape:

```yaml
stages:
  - id: left
    run: [/bin/echo, beta]
  - id: right
    run: [/bin/echo, alpha]
  - id: joined
    run: [/usr/bin/sort]
  - id: prefixed
    run: [/usr/bin/sed, s/^/joined:/]
  - id: counted
    run: [/usr/bin/wc, -l]
edges:
  - from: left
    to: joined
  - from: right
    to: joined
  - from: joined
    to: prefixed
  - from: joined
    to: counted
```

```bash
pipeshellx pipe --file pipeline.yaml
```

For an all-local graph, `DagRunner` follows declared edges rather than
stage declaration order:

- fan-out duplicates a producer's bytes to each successor;
- fan-in fairly merges ready predecessor streams into one stdin;
- every edge has a bounded buffer and pauses its producer at capacity;
- stdout from terminal stages is emitted by the controller;
- the result uses the rightmost nonzero stage in deterministic planner
  topological order, or `0`.

The planner rejects missing/duplicate stage identifiers, invalid edges, and
cycles before execution.

## Remote topology boundary

If any stage is remote, the entire declared graph must be exactly one chain.
A remote fan-out, remote fan-in graph expressed as multiple edges, or another
non-linear mixed topology exits `2` with:

```text
pipeshellx pipe: non-linear remote DAGs are not supported; use a single declared chain
```

This is a product constraint, not a silent linearization. General fan-in/fan-out
is implemented locally; general remote DAG scheduling, cross-stream
backpressure, and SSH cross-node edges remain deferred. The special first-stage
`@group` form is the supported linear remote gather path.

## Strict diff consensus

`diff` runs one command on selected native hosts and compares exact
successful stdout:

```bash
pipeshellx diff -i fleet.ini -g nodes \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  -- cat /etc/example.conf
```

- stdout alone forms consensus buckets;
- stderr is diagnostic and never changes the consensus;
- a transport error or nonzero remote stage exit is a host failure;
- exit `0` means unanimous stdout, `1` means drift, and
  `2` means usage/configuration or any host failure;
- `--json` returns `{unanimous, hosts, buckets}` with the
  largest bucket first.

The comparison is byte-for-byte; v0.6 does not normalize whitespace, line
endings, or structured data.

## Current gaps

- `splice()` is not implemented.
- A remote graph cannot be non-linear.
- SSH does not carry pipeline edges.
- Remote reconnect/resume is not implemented.
- Windows controller and native-node execution are not implemented.
- Per-stage sandboxing and signed audit remain future hardening work.
