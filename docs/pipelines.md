# Execution, Output, and Pipelines

PipeShellX has two execution interfaces:

- `run` fans one argv out to selected inventory hosts;
- `pipe` executes an inline chain or a declared process graph.

Remote `run` supports SSH and native mTLS. Remote pipeline edges use native
mTLS only.

## Distributed run

```bash
pipeshellx run -i fleet.ini -g web --stream -c 32 -- uptime

pipeshellx run -i fleet.ini -g nodes --transport native \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  --canary 10% --fail-fast -- /usr/local/bin/check
```

The `--` delimiter before command argv is required. One of
`-g GROUP`, `-t TAG`, or
`-H host1,host2` may be supplied; without a selector, every inventory host is
selected.

Execution controls:

- `-c N` maintains a sliding window of in-flight hosts; the default is
  64 and `0` requests all hosts at once;
- `--timeout S` bounds each stage; `0` disables the command timeout;
- `--fail-fast` stops pending work and aborts in-flight siblings after
  a final failure;
- SSH retries require both `--idempotent` and
  `--retries N`, and apply only to classified transient transport
  failures;
- native `--canary N|N%` runs a subset first and skips the remainder
  after a failed canary;
- Ctrl-C cancels owned work, terminates and reaps it, and returns `130`;
- `--policy FILE` optionally rejects argv on the controller before any
  target is contacted;
- `--audit-log FILE` appends unsigned lifecycle and outcome metadata.

Without `--policy`, `run` is an unrestricted
trusted-operator interface. The legacy `shell` allowlist does not
govern it. Native nodes enforce a separate optional
`node --policy` boundary.

`run` exit codes:

| Code | Meaning |
| ---: | --- |
| `0` | Every selected stage succeeded. |
| `1` | A stage failed, timed out, was cancelled, or had a transport failure. |
| `2` | Usage, policy, inventory, credential, or transport configuration error. |
| `3` | No hosts were selected. |
| `130` | Operator cancellation. |

## Output and capture

| Mode | Contract |
| --- | --- |
| `--group` | Default; one completed block per host. |
| `--stream` | Live host-tagged complete lines, preserving controller stdout versus stderr. |
| `--json` | One JSON object per completed stage followed by one summary object. |
| `--consensus` | Exact-output buckets, largest first; combine with `--json` for machine-readable output. |
| `--ordered` | Wraps the selected sink and emits completed results in stable host order. |

`LineFramer` prevents partial lines from different hosts from being
interleaved and flushes a final unterminated line at EOF.

Retained capture is controlled by `--ring SIZE` and
`--overflow`:

| Policy | Behavior |
| --- | --- |
| `block` | Lossless, unbounded capture; ignores the ring size. |
| `drop-oldest` | Retains the newest `SIZE` bytes per channel and reports discarded bytes. |
| `drop-newest` | Retains the first `SIZE` bytes per channel and reports discarded bytes. |
| `spool` | Keeps a bounded in-memory tail and spills older bytes to disk, but disk growth and final full-result materialization are unbounded. |

Buffered sinks receive the retained post-policy capture. Live
`--stream` output emits arriving lines even when the separately retained
result uses a drop policy. Native per-stream credit bounds wire data in flight;
it is not a bound on lossless controller capture.

## JSON Lines schema

`run --json` writes newline-delimited JSON (JSON Lines/NDJSON). It
does not use the RFC 7464 record-separator byte.

One stage object is emitted when each host completes:

```json
{"stage":"deploy@web-01","exit":0,"timed_out":false,"error":"","dropped":0,"stdout":"...","stderr":"..."}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `stage` | string | Host/client identifier. |
| `exit` | number | Process exit code; `-1` when terminated by a signal and `127` when spawn fails. |
| `timed_out` | boolean | The stage exceeded its timeout. |
| `error` | string | Normalized failure text, or an empty string. |
| `dropped` | number | Bytes discarded by a drop policy. |
| `stdout` | string | Retained standard-output capture. |
| `stderr` | string | Retained standard-error capture. |

One summary object is emitted last:

```json
{"summary":true,"stages":2,"succeeded":2,"failed":0,"dropped":0,"cancelled":false}
```

Its fields are `summary`, `stages`, `succeeded`,
`failed`, `dropped`, and `cancelled`.
Strings are escaped according to RFC 8259. Each object occupies one line, so a
consumer can process stage completions incrementally.

```bash
pipeshellx run -i fleet.ini -g web --json -- uptime \
  | jq -r 'select(.summary | not) | select(.exit != 0) | .stage'
```

## Inline pipelines

An inline stage is a quoted command with an optional `@placement`.
The separator outside quotes creates an edge:

```bash
# Local chain
pipeshellx pipe "'/bin/echo hello' | '/usr/bin/tr a-z A-Z'"

# Validate a mixed linear plan without execution
pipeshellx pipe --check -i fleet.ini \
  "'grep ERROR /var/log/app.log'@node-01 | 'sort -u'@local"
```

An omitted placement or `@local` runs on the controller. A host
placement uses the native transport and requires an explicit inventory plus
controller `--cert`, `--key`, and
`--ca`. A group placement is supported only as the first stage of a
linear remote gather.

Local/remote boundaries forward stdout to downstream stdin. EOF half-closes
downstream input. If a downstream stage exits while an upstream producer is
unfinished, the controller fences the producer, accounts it as exit
`137`, and waits for cancellation/reap completion. Pipeline status is
the rightmost nonzero stage in deterministic planner order.

## Declared local DAGs

`pipe --file FILE` accepts a dependency-free restricted YAML shape:

```yaml
stages:
  - id: left
    run: [/bin/echo, beta]
  - id: right
    run: [/bin/echo, alpha]
  - id: joined
    run: [/usr/bin/sort]
edges:
  - from: left
    to: joined
  - from: right
    to: joined
```

The planner rejects missing or duplicate identifiers, invalid edges, cycles,
and unsupported placement before execution. For all-local graphs:

- declared edges, not declaration order, determine execution;
- fan-out copies producer bytes to every successor;
- fan-in fairly merges ready predecessors into one stdin;
- each edge has a bounded buffer and pauses its producer at capacity;
- terminal-stage stdout is emitted by the controller;
- every child is terminated/reaped on cancellation;
- pipefail is the rightmost nonzero code in deterministic planner order.

If any stage is remote, the graph must be exactly one declared chain. A remote
fan-out, general remote fan-in, or other non-linear mixed graph exits
`2` with:

```text
pipeshellx pipe: non-linear remote DAGs are not supported; use a single declared chain
```

The graph is never silently linearized or moved locally. SSH-carried pipeline
edges, general remote DAGs, and reconnect/resume are not implemented.

## Strict drift comparison

`diff` runs one command on selected native hosts and compares exact
successful stdout bytes:

```bash
pipeshellx diff --json -i fleet.ini -g nodes \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  -- cat /etc/example.conf
```

- stdout alone forms consensus buckets;
- stderr remains diagnostic and cannot create drift;
- a transport error or nonzero stage exit is a host failure;
- exit `0` means unanimous stdout, `1` means drift, and
  `2` means usage/configuration or a host failure;
- comparison is byte-for-byte with no whitespace or structured-data
  normalization.

PipeShellX's consensus output is a result reduction, not a replicated control
plane or distributed-consensus protocol.
