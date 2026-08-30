# Deployment Guide for v0.6.0

This guide describes the product that is implemented in PipeShellX 0.6.0.
The executable, build target, and installed command are lowercase:
`pipeshellx`.

## Supported deployment scope

| Role | Supported in v0.6.0 |
| --- | --- |
| Controller | Linux or macOS on a POSIX runtime. |
| Native node | Linux or macOS with OpenSSL 3. |
| SSH target | Any target supported by the controller's OpenSSH client, including Windows OpenSSH with an explicit matching `--shell`. |
| Windows controller or native node | Not implemented; the CMake build fails clearly on `WIN32`. |

Linux uses `epoll`, `pidfd`, and
`signalfd` when available; macOS uses kqueue filters. A portable
`poll` backend remains available on supported POSIX systems. Platform
headers and system calls are confined to the POSIX implementation layer.

PipeShellX is a trusted-operator execution tool, not a multi-tenant sandbox.
Run controllers and native nodes under dedicated, least-privileged OS accounts
and apply filesystem, network, service-manager, and workload limits outside the
process where required. See [Security](security.md) and
[Windows support](windows.md).

## Build requirements and options

Source builds require:

- CMake 3.20 or newer;
- a C++20-capable compiler and Ninja or Make;
- POSIX threads and process APIs;
- OpenSSL 3.0 or newer when native transport is enabled;
- an OpenSSH client 7.6 or newer at runtime for SSH execution;
- network access to fetch the pinned GoogleTest source when tests are enabled,
  unless an installed GoogleTest is selected.

Important CMake options are:

| Option | Default | Contract |
| --- | ---: | --- |
| `PIPESHELLX_NATIVE_TRANSPORT` | `ON` | Builds native mTLS, CA, node, native `run`, `diff`, and remote-pipeline support; requires OpenSSL 3. |
| `PIPESHELLX_STATIC_OPENSSL` | `OFF` | Prefers static OpenSSL libraries for native-enabled release builds. It does not guarantee a completely static executable. |
| `PIPESHELLX_BUILD_TESTS` | `ON` | Builds and registers the GoogleTest suite. |
| `PIPESHELLX_SYSTEM_GTEST` | `OFF` | Uses an installed GoogleTest instead of FetchContent. |
| `PIPESHELLX_BUILD_BENCH` | `ON` | Builds the baseline benchmark harness. |
| `PIPESHELLX_WERROR` | `ON` | Treats first-party compiler warnings as errors. |
| `PIPESHELLX_SANITIZE` | empty | Enables a supported sanitizer list such as `address,undefined`. |

### Default native-enabled build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/pipeshellx --version
```

The default configure fails with a clear diagnostic when OpenSSL 3 cannot be
found. For a deployment-only build, add
`-DPIPESHELLX_BUILD_TESTS=OFF -DPIPESHELLX_BUILD_BENCH=OFF`.

### SSH-only build without TLS

```bash
cmake -S . -B build-ssh \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIPESHELLX_NATIVE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
cmake --build build-ssh --parallel
./build-ssh/bin/pipeshellx --version
```

This configuration builds SSH execution, inventory management, the legacy
shell, and all-local pipelines without discovering or linking OpenSSL. Native
`node`, `ca`, native `run`, `diff`, and remote
`pipe` operations return a clear unsupported-configuration error.

### Static-OpenSSL release preference

```bash
cmake -S . -B build-static \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIPESHELLX_BUILD_TESTS=OFF \
  -DPIPESHELLX_BUILD_BENCH=OFF \
  -DPIPESHELLX_STATIC_OPENSSL=ON
cmake --build build-static --parallel
```

`PIPESHELLX_STATIC_OPENSSL` sets CMake's OpenSSL preference. Verify the
resulting binary's dynamic dependencies on the release platform; system
runtime libraries may remain dynamic.

## Build and install contract

The source tree defines these lowercase first-party targets:

- `pipeshellx`: `build/bin/pipeshellx`;
- `pipeshellx_lib`: the core library, output as
  `build/lib/libpipeshellx.*`;
- `pipeshellx_tests` when tests are enabled;
- `pipeshellx_bench_baseline` when benchmarks are enabled.

Install with GNUInstallDirs-aware paths:

```bash
cmake --install build --prefix /opt/pipeshellx
/opt/pipeshellx/bin/pipeshellx --version
```

The install includes the executable, library, public headers, and versioned
CMake package metadata. It also installs `LICENSE`, `NOTICE`,
`README.md`, and `CHANGELOG.md` under the data documentation
directory. Downstream CMake projects consume the exported target as follows:

```cmake
find_package(pipeshellx 0.6 CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE pipeshellx::lib)
```

A package produced with native transport enabled retains its OpenSSL 3 and
Threads dependencies. A package produced with native transport disabled does
not require OpenSSL. Build- and install-interface includes are separated, so
the installed export does not depend on the source-tree location.

## Inventory

The current inventory is an INI file. Groups are sections, and repeated host
entries can add group membership:

```ini
[defaults]
user = deploy
port = 22
identity = /home/operator/.ssh/fleet_ed25519

[web]
web-01 tag=prod,blue transport=ssh
web-02 user=release port=2222 transport=ssh

[nodes]
node-01 transport=native san=spiffe://psx/node/node-01 native_port=7433
```

Supported host keys are `user`, `port`,
`identity`, `tag`, `transport`,
`san`, and `native_port`. Transport must be
`ssh` or `native`, and ports must be in
`1..65535`.

### Resolution precedence

Commands using normal resolution take the first available candidate:

1. explicit `-i FILE` or `--inventory FILE`;
2. non-empty `PIPESHELLX_INVENTORY`;
3. `./inventory.ini`;
4. legacy `./clients.txt`;
5. `$XDG_CONFIG_HOME/pipeshellx/inventory.ini`, or
   `$HOME/.config/pipeshellx/inventory.ini` when XDG is unset.

`run`, `ping`, `diff`, and
`hosts list` use this resolver. Remote `pipe` is the
exception: it requires an explicit `-i FILE`.

### Listing and atomic mutation

```bash
pipeshellx hosts list -i fleet.ini

pipeshellx hosts add web-03 -i fleet.ini --group web \
  --user deploy --transport ssh --identity /home/operator/.ssh/fleet_ed25519

pipeshellx hosts add node-02 -i fleet.ini --group nodes \
  --transport native --san spiffe://psx/node/node-02 --native-port 7433

pipeshellx hosts remove web-03 -i fleet.ini
pipeshellx hosts import clients.txt -i fleet.ini
```

Listing may use normal precedence. Add, remove, and import require an explicit
INI target and refuse to mutate a file named `clients.txt`, which is
reserved for legacy import semantics. Mutations reject duplicate hosts and
secret-bearing values, serialize only supported metadata, and replace the
target atomically while preserving its permissions.

## SSH trust and authentication

SSH workers start `ssh` resolved from `PATH` and delegate
keys, agents, certificates, host aliases, proxy rules, and target-shell startup
to OpenSSH. The modern `run` command has no password flag; use keys,
an agent, SSH certificates, or OpenSSH configuration for unattended work.

PipeShellX supplies these operational defaults:

```text
StrictHostKeyChecking=accept-new
UserKnownHostsFile=<inventory>.known_hosts
BatchMode=yes
ConnectTimeout=5
ServerAliveInterval=15
```

`accept-new` is trust on first use. For a managed deployment, pre-seed
`<inventory>.known_hosts` with fingerprints verified out of band. A
changed key is refused. The legacy interactive `shell` can pass an
in-memory password to `sshpass -d` through a pipe; it does not persist
that password and is not the recommended deployment interface.

## Distributed `run`

The basic grammar is:

```bash
pipeshellx run [-i FILE] [-g GROUP|-t TAG|-H host1,host2] [options] -- command arg...
```

The `--` delimiter is required. Selectors are mutually exclusive; no
selector means every inventory host. No inventory is a configuration error,
not a request to run locally.

### Common execution and output controls

| Option | Behavior |
| --- | --- |
| `-c N`, `--concurrency N` | Sliding window of in-flight hosts; default `64`, while `0` requests all at once. |
| `--timeout S` | Bounds stage execution; `0` disables the command timeout. |
| `--fail-fast` | Stops pending work and aborts in-flight siblings after a final failure. |
| `--policy FILE` | Applies an optional controller-side command policy before any host is contacted. |
| `--ring SIZE` | Per-channel retained-capture limit for drop/spool policies; `0` is unbounded. |
| `--overflow block|drop-oldest|drop-newest|spool` | `block` is lossless/unbounded, drop policies enforce the ring, and spool uses the ring as its in-memory tail. |
| `--group` | Default completed block per host. |
| `--stream` | Live host-tagged complete lines, preserving stdout versus stderr. |
| `--json` | One object per completed stage plus a summary object. |
| `--consensus` | Buckets exact output; combine with `--json` for machine-readable consensus. |
| `--ordered` | Emits completed results in stable host order. |
| `--audit-log FILE` | Appends unsigned JSONL lifecycle/outcome records without captured output. |
| `--no-color` | Disables terminal color. |

Buffered sinks render only the post-policy capture. Live `--stream` output is
not truncated by a drop policy, although the returned capture and dropped-byte
summary are. Spool limits its in-memory tail while a command runs, but the
temporary file can grow without bound and lossless completion materializes the
full result.

Without `--policy`, `run` is an unrestricted trusted-operator
surface. The legacy shell's demonstration allowlist does not govern it.
The compatibility REPL remains available as `pipeshellx shell` and is
also entered when the executable is invoked without a subcommand.

### Transport selection and transport-specific options

Each host defaults to `transport=ssh`. Without
`--transport`, a selected set must be homogeneous and its inventory
transport is used. A mixed SSH/native selection exits `2`; use
`--transport ssh` or `--transport native` only
when every selected host is reachable by that transport.

SSH example:

```bash
pipeshellx run -i fleet.ini -g web --transport ssh \
  --stream -c 32 --shell posix -- uptime
```

SSH-only options are:

- `--shell posix|cmd|powershell` for target-shell argv
  serialization;
- `--reuse` for OpenSSH ControlMaster reuse;
- `--idempotent --retries N` for classified transient
  transport failures.

Only an explicitly idempotent command is retried. Authentication failures,
host-key changes, timeouts, and remote nonzero exits are not retried. The
controller starts OpenSSH directly, but the target SSH service invokes its
configured shell; SSH execution is not universally shell-free.

Native example:

```bash
pipeshellx run -i fleet.ini -g nodes --transport native \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  --crl ca/crl.pem --native-port 7433 --canary 10% \
  -- uname -a
```

Native-only options are `--cert`, `--key`,
`--ca`, optional `--crl`,
`--native-port`, and `--canary N|N%`.
Per-host `native_port` overrides the command default, and
inventory `san` pins the node certificate's SAN URI. Native nodes
receive argv over psx/1 and spawn it directly; a shell participates only when
the requested argv explicitly names one.

SSH-only and native-only options are rejected with the other transport.
`ping` probes SSH hosts only. `diff` uses native mTLS and
compares exact successful stdout bytes; it exits `0` for unanimous
output, `1` for drift, and `2` for usage, configuration,
transport, or nonzero-stage failures. Stderr is diagnostic and is not part of
the comparison.

### `run` exit status

| Code | Meaning |
| ---: | --- |
| `0` | Every selected stage succeeded. |
| `1` | At least one selected stage failed, timed out, or had a transport failure. |
| `2` | Usage, policy, inventory, credential, or transport configuration error. |
| `3` | No hosts were selected. |
| `130` | Operator cancellation. |

Ctrl-C cancels active work, terminates and reaps controller-owned processes,
and returns `130`. See [Distributed execution](distributed_execution.md),
[JSON output](json.md), and [Authentication](authentication.md).

## Native CA and node deployment

Native mode is a TLS 1.3 service using CA-signed certificates and SAN URI
identities. Both peers verify the CA. The controller can pin each node through
inventory `san=`, and the node can restrict controllers with
`--allow`.

### Create a CA and identities

```bash
pipeshellx ca init --cn pipeshellx-fleet --dir ca

pipeshellx ca issue --san spiffe://psx/controller/ops \
  --ca ca --out controller

pipeshellx ca issue --san spiffe://psx/node/node-01 \
  --ca ca --out node-01
```

The simpler `ca issue` flow creates each private key on the CA host.
For a node-held key, run `pipeshellx node keygen --san URI --out PREFIX`
on the node, transfer only its CSR, and sign it with
`pipeshellx ca sign --ca DIR --csr FILE --san URI --out FILE`.
Protect CA and leaf private keys with OS permissions and an operational backup
and rotation process.

Optional revocation is explicit on each endpoint:

```bash
pipeshellx ca revoke --ca ca --cert node-01.crt
```

This updates `ca/crl.pem`; distribute it and pass `--crl` to
both controllers and nodes that must enforce it.

### Configure and run a node

Add a SAN-pinned inventory entry:

```ini
[nodes]
node-01 transport=native san=spiffe://psx/node/node-01 native_port=7433
```

Start the daemon under a dedicated OS account:

```bash
pipeshellx node --listen 0.0.0.0:7433 \
  --cert /etc/pipeshellx/node.crt \
  --key /etc/pipeshellx/node.key \
  --ca /etc/pipeshellx/ca.crt \
  --allow spiffe://psx/controller/ops \
  --policy /etc/pipeshellx/node.policy \
  --control /run/pipeshellx/node.ctl
```

`--allow` accepts a comma-separated list of controller SAN URIs.
If it is omitted, the node admits any CA-signed controller and emits a warning.
`--policy` is optional defense in depth: it validates argv before
spawn and returns stage exit `126` when it rejects a request. Without
it, an admitted controller may request arbitrary argv as the node daemon's OS
account. It is not a sandbox or privilege-separation boundary.

`--control PATH` creates a local AF_UNIX status socket. Query it with:

```bash
pipeshellx node status --control /run/pipeshellx/node.ctl
```

The response is a one-line JSON metrics snapshot. Socket filesystem
permissions control local access, and the socket is removed on clean shutdown.

Restrict the network listener to intended controller networks and CA identities.
The psx/1 protocol has flow-control credit, connection leases, graceful drain,
and connection-loss fencing, but no reconnect/resume. A lost connection fails
unfinished work. The complete contract is in the
[psx/1 wire protocol](wire_protocol.md).

### Generate service definitions

The node command emits, but does not install, service definitions using the
same daemon flags:

```bash
# Linux systemd
pipeshellx node systemd-unit \
  --cert /etc/pipeshellx/node.crt \
  --key /etc/pipeshellx/node.key \
  --ca /etc/pipeshellx/ca.crt \
  --listen 0.0.0.0:7433 \
  --allow spiffe://psx/controller/ops \
  --policy /etc/pipeshellx/node.policy \
  --control /run/pipeshellx/node.ctl > pipeshellx-node.service

# macOS launchd
pipeshellx node launchd-plist \
  --cert /usr/local/etc/pipeshellx/node.crt \
  --key /usr/local/etc/pipeshellx/node.key \
  --ca /usr/local/etc/pipeshellx/ca.crt \
  --listen 127.0.0.1:7433 \
  --allow spiffe://psx/controller/ops \
  --policy /usr/local/etc/pipeshellx/node.policy \
  --control /usr/local/var/run/pipeshellx-node.ctl > pipeshellx-node.plist
```

Review paths, account names, permissions, network exposure, and the generated
definition before installation. The systemd template contains useful service
hardening, but neither generated definition makes arbitrary executed code safe
for hostile multi-tenant use.

For a hardened systemd unit, `--control` must be
`/run/pipeshellx/FILE`. The generator emits
`RuntimeDirectory=pipeshellx`, a restrictive umask, and an explicit writable
path so `ProtectSystem=strict` does not prevent creation of the socket. This
path constraint applies to `systemd-unit`; direct node and launchd invocations
may use another suitable local path.

## Pipelines

Inline pipelines declare a quoted chain with optional placements:

```bash
# Local
pipeshellx pipe "'/bin/echo hello' | '/usr/bin/tr a-z A-Z'"

# Validate a mixed linear plan without executing it
pipeshellx pipe --check -i fleet.ini \
  "'grep ERROR /var/log/app.log'@node-01 | 'sort -u'@local"
```

`pipe --file FILE` or `pipe -f FILE` loads the restricted
YAML stage/edge format documented in [Pipelines](pipelines.md). For an all-local
acyclic graph, declared edges determine execution regardless of stage
declaration order:

- fan-out copies bytes to every successor;
- fan-in fairly merges ready predecessors into one stdin;
- each edge has bounded buffering and propagates backpressure;
- terminal stdout is emitted by the controller;
- pipefail is the rightmost nonzero status in deterministic planner order;
- cancellation closes routes, terminates process groups, reaps children, and
  returns `130`.

Remote pipeline stages use native mTLS, not SSH, and require an explicit
inventory plus controller `--cert`, `--key`, and
`--ca`. If any stage is remote, the declared graph must be exactly one
chain. A non-linear remote or mixed graph exits `2` with:

```text
pipeshellx pipe: non-linear remote DAGs are not supported; use a single declared chain
```

This limitation is explicit; the graph is never silently linearized. The
special first-stage `@group` gather form is supported for a linear
remote pipeline, but general remote fan-in/fan-out and SSH pipeline edges are
not implemented.

An early downstream exit fences any unfinished upstream segment, accounts it
as `137`, and waits for cancellation/reap completion. The rightmost later
nonzero stage remains the pipeline result.

## Logs, output, and audit

Controller operational logs are rotating structured text. Their default path
is:

1. `$XDG_STATE_HOME/pipeshellx/pipeshellx.log` when XDG state is set;
2. `$HOME/.local/state/pipeshellx/pipeshellx.log` otherwise;
3. `./pipeshellx/pipeshellx.log` if no home directory can be resolved.

The legacy `shell` accepts `--log-file PATH` and
`--verbose`; `run`, `ping`, `hosts`,
`node`, and native `diff` initialize the default logger.
When configured logging cannot open its file, PipeShellX warns and falls back
to stderr. The default rotation threshold is 10 MiB with five archived
generations.

Do not confuse operational logs with command output:

- `run --json` and `diff --json` are implemented
  machine-readable output modes;
- `run --audit-log FILE` writes optional unsigned JSON Lines
  lifecycle records without captured stdout/stderr;
- the audit log is not signed, hash-chained, or tamper-evident and has no
  built-in retention policy.

## Capacity and process supervision

`run -c N` is the primary controller-side concurrency bound; its
default is 64. PipeShellX attempts to raise its soft descriptor limit within
the account's hard limit, but operators must still size descriptor, process,
memory, disk-spool, and network limits for their workloads. The project has
some POSIX child resource-limit plumbing, but it does not expose a complete,
uniform configurable per-stage resource contract in v0.6.

Use systemd, launchd, or another supervisor for the native daemon. External
cgroups, containers, quotas, and filesystem permissions may provide useful
deployment boundaries, but PipeShellX does not itself claim seccomp,
namespace, chroot, privilege-separation, or container sandboxing.

## Test and CI contract

Run the local suite with:

```bash
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test --parallel
ctest --test-dir build-test --output-on-failure
```

The configured GitHub Actions CI matrix contains:

- Linux GCC Debug and Release;
- Linux Clang Debug and Release;
- macOS AppleClang Debug and Release;
- warnings-as-errors builds and CLI help/version/error smoke tests;
- Linux Clang Debug with ASan+UBSan;
- architecture-layering lint;
- a static-OpenSSL Release install plus downstream
  `find_package(pipeshellx)` smoke test;
- an OpenSSL-disabled SSH-only build, install, CLI smoke, and downstream
  package smoke.

The best-effort nightly benchmark workflow builds Release without tests, runs
the local baseline harness, and uploads its output. It does not gate merges and
does not constitute heterogeneous-fleet qualification. Workflow configuration
does not prove that an unpublished commit has already passed hosted CI. See
[Testing](testing.md) for test categories and focused commands.

## Explicit v0.6 limitations

- No Windows controller, Windows native node, SCM service, or Windows CI
  matrix; Windows is an SSH target only.
- No general non-linear remote DAG and no SSH-carried pipeline edges.
- No native reconnect/resume; connection loss is terminal for unfinished work.
- No built-in multi-tenant sandbox or node privilege separation.
- Node policy is optional and an admitted controller is otherwise authorized
  to request arbitrary argv.
- Audit records are unsigned and not tamper-evident.
- Real heterogeneous-fleet qualification and environment-specific capacity
  sizing remain deployment responsibilities.

Related references:

- [Architecture](architecture.md)
- [Authentication and inventory](authentication.md)
- [Distributed execution](distributed_execution.md)
- [Pipelines](pipelines.md)
- [Security](security.md)
- [Windows support](windows.md)
- [psx/1 wire protocol](wire_protocol.md)
