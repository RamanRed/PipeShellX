# Distributed Execution

PipeShellX 0.6 fans operator-supplied argv out to inventory hosts over either
system OpenSSH or the native psx/1 mTLS backplane. There is no automatic local
fallback when an inventory is absent: `run` reports a configuration
error. Use `pipe` for an explicit local pipeline or
`shell` for the legacy REPL.

## Run model

```bash
pipeshellx run -i fleet.ini -g web -- uptime
pipeshellx run -i fleet.ini -t prod --stream -c 32 -- journalctl -n 20
pipeshellx run -i fleet.ini -H web-01,web-02 --json -- id
```

The command grammar requires `--` before argv. Exactly one of
`-g GROUP`, `-t TAG`, or `-H h1,h2` may be
given; no selector means all hosts. Inventory lookup and mutation are documented
in [Authentication and inventory](authentication.md).

The controller:

1. parses options and argv strictly;
2. loads and selects inventory hosts;
3. applies an optional `--policy FILE` before contacting any host;
4. chooses one transport for the selected set;
5. schedules work up to `-c N` concurrent hosts;
6. preserves stdout and stderr as separate channels;
7. renders stage results and a run summary;
8. appends audit outcome records when `--audit-log FILE` is set.

Without `--policy`, `run` is an unrestricted trusted-operator
tool. It is not governed by the legacy shell's fixed demo allowlist.

## Transport selection

Each inventory host has `transport=ssh` by default and may instead
declare `transport=native`.

- `--transport ssh|native` overrides every selected host.
- Without an override, a homogeneous selected set uses its declared transport.
- A mixed selected set exits `2` rather than silently choosing one.

`ping` is SSH-only. Native hosts must be excluded from a ping
selection. `diff` and remote `pipe` use native transport
only in v0.6.

## SSH execution

One managed OpenSSH process represents each in-flight host. The controller
quotes argv for the chosen target shell and passes the resulting remote command
to `ssh`. OpenSSH handles keys, agents, certificates, config, proxy
rules, and host keys.

The local controller does not launch `/bin/sh -c` to start SSH, but the
target SSH service does invoke its configured remote shell. Choose
`--shell posix|cmd|powershell` accordingly. This is not shell-free
execution and does not itself restrict the requested command.

`-c` is a sliding window, not a promise to start every SSH process at
once. It bounds controller process/descriptor use. `--reuse` enables
OpenSSH ControlMaster sockets for repeated connections.

Retries are conservative:

```bash
pipeshellx run -i fleet.ini -g web --idempotent --retries 2 -- uptime
```

Only explicitly idempotent SSH commands are retried, and only for classified
transient transport failures. Authentication failures, host-key changes,
timeouts, and a command's own nonzero exit are not retried.

## Native execution

```bash
pipeshellx run -i fleet.ini -g nodes --transport native \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  --crl ca/crl.pem -- uname -a
```

The controller opens mutually authenticated TLS connections and sends a
versioned argv request. A node starts the process directly as the node daemon's
OS account and maps stdin/stdout/stderr to psx/1 channels. No remote shell is
inserted, although the argv may explicitly request one.

Native execution provides:

- certificate SAN pinning and optional CRL checking;
- multiplexed streams with per-stream credit;
- independent stdout/stderr capture and rendering;
- timeout, Ctrl-C cancellation, fail-fast abort, and dropped-byte metadata;
- `--canary N` or `--canary N%` staged rollout;
- connection leases, graceful drain, and node-side fencing when the controller
  connection is lost.

An authenticated controller is authorized to request arbitrary argv unless
the daemon has its own `node --policy FILE`. That independent policy
uses the same format, rejects before spawn, emits a diagnostic on the stage's
stderr channel, and returns exit `126`. It is defense in depth, not a
sandbox. Use a dedicated OS account, a narrowly scoped CA/SAN allowlist, and a
node policy where appropriate. See [Security](security.md).

The protocol has no reconnect/resume. A lost connection fails unfinished work;
it does not fabricate successful exits. The complete contract is in
[the psx/1 wire protocol](wire_protocol.md).

## Output and backpressure

`run` offers these primary sinks:

| Mode | Behavior |
| --- | --- |
| `--group` | Default; prints one completed block per host. |
| `--stream` | Emits live host-tagged whole lines, keeping stdout and stderr on their original controller streams. |
| `--json` | Emits one JSON object per completed stage and one summary object. |
| `--consensus` | Buckets stage output for a consensus/drift view. |

`--ordered` wraps the chosen sink and emits by host order after
completion. The JSON schema is documented in [JSON output](json.md).

Capture is configurable with `--ring SIZE` and
`--overflow block|drop-oldest|drop-newest|spool`:

- `block` is lossless and unbounded; it ignores the ring size;
- drop policies bound each retained stdout/stderr channel, feed buffering
  sinks only retained bytes, and report discarded bytes;
- `spool` keeps the configured in-memory tail and spills older bytes to a
  temporary file, but lossless completion reconstructs the full result and
  does not bound disk growth or final materialization;
- live `--stream` output emits every line as it arrives; a drop policy bounds
  the separately returned capture, not what was already displayed.

Native transport also applies credit windows on the wire. Credit bounds data in
flight and is returned as the controller handles bytes; it is not a cap on
lossless controller capture. Output from separate hosts is not globally ordered
unless an ordered sink is selected.

## Cancellation, timeout, and failures

`--timeout S` bounds stages. `--fail-fast` stops pending work
and aborts in-flight siblings after a final failure. Ctrl-C cancels an active
run and returns `130`; owned processes are terminated and reaped, and
the summary/audit records cancellation state.

Product exit codes for `run` are:

| Code | Meaning |
| ---: | --- |
| `0` | Every selected stage succeeded. |
| `1` | At least one stage failed. |
| `2` | Usage, policy, inventory, credential, or transport configuration error. |
| `3` | No hosts were selected. |
| `130` | Operator cancellation. |

Stage output is never treated as proof of success by itself; exit status,
timeout, cancellation, abort, and transport errors all contribute to the
result.

## Strict drift consensus

`diff` is a native-mTLS drift command:

```bash
pipeshellx diff --json -i fleet.ini -g nodes \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  -- cat /etc/example.conf
```

It compares exact successful stdout bytes. Stderr is excluded from consensus,
so a diagnostic on stderr cannot create false drift. Any transport failure or
nonzero remote exit is a host failure and makes `diff` exit
`2`; otherwise it exits `0` for unanimous stdout or
`1` for drift.

Selectors are mutually exclusive, unknown options and missing values are
rejected, and `--native-port` is parsed strictly in
`1..65535`.

## Current limitations

- No Windows controller or native Windows node; a POSIX controller can reach a
  Windows OpenSSH target.
- No reconnect/resume for native work.
- Node policy is optional; there is no mandatory policy, privilege separation,
  or sandbox.
- No SSH implementation of cross-node pipeline edges.
- General non-linear DAGs are local-only; any graph containing a remote stage
  must be a single declared chain.
- The test suite uses deterministic fake/loopback transports for most failure
  coverage; real-fleet qualification remains operational work.
