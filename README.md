# PipeShellX

PipeShellX 0.6.0 is a C++20 command-execution controller for Linux and macOS.
It can fan one command out to inventory hosts over system OpenSSH or an
optional native mutual-TLS transport, run local process graphs, and operate a
native node daemon. The executable and CMake executable target are both
lowercase: `pipeshellx`.

Current capabilities include:

- bounded-concurrency SSH or native fan-out with grouped, streaming, JSON,
  ordered, and consensus output;
- INI inventories with group, tag, and explicit-host selection, plus atomic
  `hosts add`, `remove`, and legacy `clients.txt` import;
- native TLS 1.3 nodes with CA-issued identities, SAN authorization, optional
  CRLs and command policy, and a local status socket;
- inline pipelines and file-defined, all-local acyclic fan-in/fan-out graphs
  with bounded per-edge buffering and deterministic pipefail;
- an event-driven POSIX runtime built around nonblocking pipes,
  `epoll`/`kqueue`/`poll`, process groups, signals, and exact child reaping.

## Quick start

The default build enables native transport and therefore requires OpenSSL 3.
Tests are enabled and use an installed GoogleTest when requested or fetch the
pinned source during configuration.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

./build/bin/pipeshellx --version
./build/bin/pipeshellx --help
```

Required build tools are CMake 3.20 or newer, a C++20 compiler, and Ninja or
Make. Linux and macOS are the supported controller and native-node platforms.
OpenSSH 7.6 or newer is required for SSH execution because PipeShellX uses
`StrictHostKeyChecking=accept-new`.

For an SSH-only build that does not discover or link OpenSSL:

```bash
cmake -S . -B build-ssh \
  -DPIPESHELLX_NATIVE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
cmake --build build-ssh --parallel
```

Release builders may add `-DPIPESHELLX_STATIC_OPENSSL=ON` to prefer static
OpenSSL libraries. This controls dependency selection; it is not a promise
that every platform library will be folded into one fully static executable.

Install to a staging prefix with:

```bash
cmake --install build --prefix /tmp/pipeshellx-prefix
/tmp/pipeshellx-prefix/bin/pipeshellx --version
```

The always-present build targets are `pipeshellx` and
`pipeshellx_lib`; `pipeshellx_tests` and
`pipeshellx_bench_baseline` are present when their respective build
options are enabled. The install exports the library as `pipeshellx::lib`:

```cmake
find_package(pipeshellx 0.6 CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE pipeshellx::lib)
```

See [Deployment](docs/deployment.md) and
[Contributing](CONTRIBUTING.md) for the full option and validation matrix.

## Inventory and host management

An INI inventory can mix SSH and native metadata:

```ini
[defaults]
user = deploy
identity = /home/operator/.ssh/fleet_ed25519

[web]
web-01 tag=prod,blue transport=ssh
web-02 port=2222 transport=ssh

[nodes]
node-01 transport=native san=spiffe://psx/node/node-01 native_port=7433
```

Normal inventory resolution uses the first available source:

1. `-i FILE` or `--inventory FILE`;
2. `PIPESHELLX_INVENTORY`;
3. `./inventory.ini`;
4. legacy `./clients.txt`;
5. `$XDG_CONFIG_HOME/pipeshellx/inventory.ini`, or
   `$HOME/.config/pipeshellx/inventory.ini`.

`run`, `ping`, `diff`, and `hosts list` use that resolver. A
remote `pipe` deliberately requires an explicit `-i FILE`.
Inventory mutations also require an explicit INI target:

```bash
pipeshellx hosts list -i fleet.ini
pipeshellx hosts add web-03 -i fleet.ini --group web \
  --user deploy --transport ssh --tag prod
pipeshellx hosts remove web-03 -i fleet.ini
pipeshellx hosts import clients.txt -i fleet.ini
```

Rewrites are atomic and reject duplicate hosts and secret-bearing entries.
SSH host trust is kept per inventory in `<inventory>.known_hosts`. Pre-seed
and verify that file when trust on first use is not acceptable. See
[Authentication and inventory](docs/authentication.md).

## Run commands on hosts

`run` requires `--` before the command argv. With no selector it targets
every inventory host; `-g GROUP`, `-t TAG`, and
`-H host1,host2` are mutually exclusive.

```bash
# SSH, using the inventory's transport
pipeshellx run -i fleet.ini -g web --stream -c 32 -- uptime

# Native mTLS
pipeshellx run -i fleet.ini -g nodes --transport native \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  --canary 10% --fail-fast -- uname -a
```

Common controls include `--timeout S`, `-c N`,
`--fail-fast`, `--policy FILE`,
`--ring SIZE`, `--overflow POLICY`,
`--audit-log FILE`, and the output modes
`--group`, `--stream`, `--json`,
`--consensus`, and `--ordered`.

The ring is a retained-capture limit for drop/spool policies. The default
`block` policy is lossless and unbounded; spool can grow temporary disk and
materializes the full result at completion. See the detailed output contract
before using lossless modes with untrusted or indefinite producers.

SSH-only controls are `--shell posix|cmd|powershell`,
`--reuse`, and `--idempotent --retries N`.
Native-only controls are `--cert`, `--key`,
`--ca`, optional `--crl`,
`--native-port`, and `--canary`. Without an explicit
`--transport`, all selected inventory hosts must declare the same
transport; a mixed selection exits with a configuration error.

`run` exits `0` when all selected stages succeed, `1`
when any stage fails, `2` for usage or configuration errors,
`3` when no hosts are selected, and `130` after operator
cancellation. See [Distributed execution](docs/distributed_execution.md) for
the precise output, backpressure, retry, and failure contracts, and
[JSON output](docs/json.md) for the machine-readable schema.

## Native CA and node

A minimal local CA setup is:

```bash
pipeshellx ca init --cn pipeshellx-fleet --dir ca
pipeshellx ca issue --san spiffe://psx/controller/ops \
  --ca ca --out controller
pipeshellx ca issue --san spiffe://psx/node/node-01 \
  --ca ca --out node-01

pipeshellx node --listen 0.0.0.0:7433 \
  --cert node-01.crt --key node-01.key --ca ca/ca.crt \
  --allow spiffe://psx/controller/ops \
  --control /run/pipeshellx/node.ctl
```

`--allow` restricts admitted controller SAN URIs. Omitting it admits any
CA-signed controller and emits a warning. Add optional
`--policy FILE` to reject disallowed argv before spawn; without a
node policy, an admitted controller can request arbitrary argv as the node's OS
account. `node status --control PATH` reads the daemon's local JSON
status snapshot. `node systemd-unit` and
`node launchd-plist` emit service definitions; they do not install
or start them.

For enrollment that keeps a node key on the node, use
`node keygen` followed by `ca sign`. See
[Authentication](docs/authentication.md), [Security](docs/security.md), and
the [psx/1 wire protocol](docs/wire_protocol.md).

## Pipelines

Run an all-local inline chain:

```bash
pipeshellx pipe "'/bin/echo hello' | '/usr/bin/tr a-z A-Z'"
```

`pipe --file pipeline.yaml` loads declared stages and edges. All-local
acyclic files support fan-in and fan-out, follow edges rather than declaration
order, bound buffering per edge, reap every child, and use the rightmost
nonzero exit in deterministic planner order. `pipe --check` validates
without execution.

Remote stages use native mTLS, not SSH, and require `-i`,
`--cert`, `--key`, and `--ca`. If any stage
is remote, the declared graph must be one chain. Non-linear mixed or remote
graphs exit `2` with:

```text
pipeshellx pipe: non-linear remote DAGs are not supported; use a single declared chain
```

The supported first-stage `@group` gather form is described in
[Pipelines](docs/pipelines.md). General remote DAG execution is not silently
linearized and is not implemented.

## Platform and security boundaries

- Linux and macOS are supported as controllers and native nodes. Windows is
  supported only as an OpenSSH target reached from a POSIX controller; choose
  `--shell cmd` or `--shell powershell` as appropriate. There is
  no Windows controller, native node, or Windows CI job. See
  [Windows support](docs/windows.md).
- There is no universal command allowlist. The legacy `shell` REPL
  (also entered when no subcommand is given) has a small demonstration
  allowlist; `run` is unrestricted unless `--policy` is
  supplied, and `node --policy` is a separate optional check.
- SSH starts OpenSSH directly on the controller, but the target SSH service
  invokes its configured remote shell. Native stages and local pipeline stages
  receive argv directly unless the operator explicitly requests a shell.
- PipeShellX is not a multi-tenant sandbox. Per-stage sandboxing, privilege
  separation, signed/tamper-evident audit, native reconnect/resume, and general
  remote DAGs are not v0.6 capabilities.
- `run --json` and `diff --json` are implemented command
  output modes. Operational logs remain rotating structured text; optional
  `--audit-log` output is unsigned JSON Lines.

## Tests and CI

CTest discovers the GoogleTest suite. The configured CI matrix covers Linux
with GCC and Clang in Debug and Release, plus macOS AppleClang in Debug and
Release. Separate jobs run Linux Clang ASan+UBSan, layering checks, a
static-OpenSSL install/downstream-package smoke test, and an SSH-only
native-disabled build/install/downstream smoke test. The nightly benchmark
workflow is best-effort and does not gate merges.

These are workflow configurations, not a claim that an unpublished commit has
already passed hosted CI. See [Testing](docs/testing.md).

## Documentation

- [Architecture](docs/architecture.md)
- [Deployment](docs/deployment.md)
- [Distributed execution](docs/distributed_execution.md)
- [Authentication and inventory](docs/authentication.md)
- [Pipelines, output, and consensus](docs/pipelines.md)
- [Security model](docs/security.md)
- [psx/1 wire protocol](docs/wire_protocol.md)
- [Testing](docs/testing.md)

PipeShellX is distributed under the terms in [LICENSE](LICENSE); bundled and
third-party notices are recorded in [NOTICE](NOTICE), and release changes are
listed in [CHANGELOG.md](CHANGELOG.md).
