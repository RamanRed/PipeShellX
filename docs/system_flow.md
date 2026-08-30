# System Flow

This document describes the current v0.6 control paths. The lowercase
`pipeshellx` executable is both the controller CLI and the optional
native node daemon.

## Startup and dispatch

`main()` initializes logging, handles `--help` and
`--version`, then dispatches the first positional command:

```text
pipeshellx
  +-- run
  +-- ping
  +-- diff
  +-- pipe
  +-- hosts
  +-- ca
  +-- node
  +-- shell
```

No command keeps backward compatibility by entering the interactive
`shell`. The one-shot subcommands do not pass through that shell's
fixed demo allowlist.

## Run

```text
argv
  -> strict run parser
  -> optional controller policy
  -> inventory resolution and selector
  -> per-host/explicit transport decision
  -> SSH fan-out or native fan-out
  -> channel-aware sink
  -> summary, exit code, optional audit
```

### Parse and authorize

The parser requires `-- <command...>` and rejects unknown options,
missing values, conflicting selectors/sinks, invalid numeric ranges, and flags
belonging to the other transport.

If `--policy FILE` is present, the controller loads and evaluates it
before any connection. Without it, `run` accepts arbitrary
operator-provided argv.

### Resolve targets

Inventory resolution follows explicit `-i`, environment, project INI,
legacy project `clients.txt`, then user config. Selection is one
group, one tag, an explicit host list, or all.

An explicit `--transport` overrides selected hosts. Otherwise every
selected host must declare the same transport; a mixed selection is rejected.

### SSH path

For each host admitted by the concurrency window:

1. the controller serializes argv for the target's selected remote shell;
2. it builds a hardened OpenSSH argv;
3. `psx::os::Process` starts the SSH worker in its own process group;
4. stdout/stderr pipes and child exit are registered with the reactor;
5. output is framed and sent to the selected sink;
6. timeout, fail-fast, or Ctrl-C terminates owned work;
7. the child is reaped exactly once and its failure is classified.

Retries are possible only for an explicitly idempotent SSH invocation and a
transient transport failure.

### Native path

For every selected target:

1. the controller loads its certificate, key, CA, and optional CRL;
2. TLS authenticates both peers and applies node SAN pins;
3. the controller sends a versioned `OPEN` request containing argv;
4. the node starts the stage as its daemon OS account;
5. framed `DATA` preserves stdin/stdout/stderr identity and credit;
6. `EXIT` carries the terminal stage outcome;
7. line framers and the sink receive independent stdout/stderr;
8. timeout, fail-fast, canary failure, or Ctrl-C cancels/aborts affected work.

PING/PONG leases detect silent peers. A lost controller connection fences the
node stages owned by it. There is no reconnect/resume.

### Completion and audit

Success requires a zero stage exit with no timeout, cancellation, abort, or
transport error. The sink receives a per-stage result and a run summary.
Ctrl-C returns `130`.

When `--audit-log FILE` is enabled, both SSH and native paths append
`run_started`, one `stage_finished` per host, and
`run_finished` with a shared run identifier. Audit excludes captured
output and is unsigned.

## Ping

`ping` uses the same inventory resolver and selectors, then runs
`echo connected` through the SSH worker path. It prints each target as
`ONLINE` or `OFFLINE` and exits nonzero when any is offline.
A selected native host is a usage/configuration error; native ping is not
implemented.

## Diff

```text
strict parser -> inventory selection -> native mTLS fan-out
  -> keep only successful exit-0 host stdout
  -> exact-output consensus buckets
  -> stderr diagnostics and exit contract
```

stderr never participates in consensus. A nonzero stage exit is a host failure,
not a drift bucket. The command returns `0` for unanimity,
`1` for drift, and `2` for usage/configuration or any host
failure.

## Pipe

`pipe` parses an inline chain or restricted YAML, then asks the
planner to validate identifiers, edges, acyclicity, and placement.

```text
all stages local
  -> DagRunner executes declared edges
     -> per-edge bounded buffers
     -> fan-out copy / fair fan-in
     -> terminal stdout + deterministic pipefail

any stage remote
  -> require exactly one declared chain
  -> explicit inventory + native controller credentials
  -> segmented local/native runner
  -> EOF/exit propagation + pipefail
```

`--check` stops after validation and placement resolution. A
non-linear graph containing a remote stage is explicitly rejected; it is never
flattened or silently run locally.

## Hosts

`hosts list` resolves an inventory and prints host, group, tag, and
transport columns. Mutations require explicit `-i FILE`:

```text
parse action
  -> validate secret-free host/options or legacy import
  -> load explicit INI target
  -> duplicate/removal checks
  -> canonical serialization
  -> same-directory temp file
  -> fsync/close and atomic rename
```

A `clients.txt` target is rejected because that basename is reserved
for legacy import semantics.

## Node and CA

`ca` manages the offline CA, issued certificates, CSR signing, and CRL
generation. `node keygen` creates a private key and CSR locally.

The long-running `node` command:

1. loads TLS identity, CA, optional CRL, controller SAN allowlist, and optional
   node command policy;
2. listens for mTLS controller connections;
3. creates one session per accepted connection;
4. rejects disallowed argv before spawn with stage exit `126`, or
   launches and tracks the accepted stage;
5. exposes optional local control-socket status metrics;
6. terminates and reaps session-owned stages on connection loss.

The policy is optional and is not a sandbox. Without `node --policy`,
any CA-authorized/allowed controller may request arbitrary argv. Service-unit
generators preserve `--policy` for systemd and launchd; Windows SCM
support is deferred.

## Legacy shell

`shell` retains the original interactive flow:

```text
prompt -> parse -> fixed demo allowlist/trusted-path checks
  -> ProcessManager -> reactor -> output callback -> prompt
```

This compatibility UI is the only surface governed by the universal-looking
fixed allowlist in `CommandExecutor`. It must not be used to describe
the authorization posture of `run` or authenticated native nodes.
