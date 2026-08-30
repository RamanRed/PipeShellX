# Build and Deployment

This guide covers source builds, inventory, authentication, and operation of
PipeShellX 0.6. The installed executable is `pipeshellx`.

## Supported platforms

| Role | Supported |
| --- | --- |
| Controller | Linux and macOS |
| Native node | Linux and macOS with OpenSSL 3 |
| SSH target | Any host supported by the controller's OpenSSH client |
| Windows | SSH target only; no Windows controller or native node |

The CMake build intentionally fails on `WIN32`. A POSIX controller can reach a
Windows OpenSSH target with `--shell cmd` or
`--shell powershell`. These options quote argv for the target's login
shell; they do not bypass it. In particular, literal cmd metacharacters
`& | < > ^` are not escaped.

## Build

Requirements:

- CMake 3.20 or newer;
- a C++20 compiler and Ninja or Make;
- OpenSSL 3 when native transport is enabled;
- OpenSSH 7.6 or newer at runtime for SSH execution;
- GoogleTest, or network access during configuration to fetch the pinned test
  dependency.

Important CMake options:

| Option | Default | Purpose |
| --- | ---: | --- |
| `PIPESHELLX_NATIVE_TRANSPORT` | `ON` | Build native mTLS, CA, node, `diff`, and remote-pipeline support. |
| `PIPESHELLX_STATIC_OPENSSL` | `OFF` | Prefer static OpenSSL libraries; does not guarantee a fully static executable. |
| `PIPESHELLX_BUILD_TESTS` | `ON` | Build and register the GoogleTest suite. |
| `PIPESHELLX_SYSTEM_GTEST` | `OFF` | Use an installed GoogleTest instead of FetchContent. |
| `PIPESHELLX_BUILD_BENCH` | `ON` | Build the baseline benchmark. |
| `PIPESHELLX_WERROR` | `ON` | Treat first-party warnings as errors. |
| `PIPESHELLX_SANITIZE` | empty | Enable sanitizers, for example `address,undefined`. |

Default native-enabled build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/pipeshellx --version
```

SSH-only build without OpenSSL discovery:

```bash
cmake -S . -B build-ssh \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIPESHELLX_NATIVE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
cmake --build build-ssh --parallel
```

For a deployment-only native build, disable tests and benchmarks. Release
builders may also request static OpenSSL preference:

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIPESHELLX_BUILD_TESTS=OFF \
  -DPIPESHELLX_BUILD_BENCH=OFF \
  -DPIPESHELLX_STATIC_OPENSSL=ON
cmake --build build-release --parallel
```

Always inspect the resulting binary's dynamic dependencies on the target
platform; the static preference is not a universal static-link guarantee.

## Install and consume

```bash
cmake --install build-release --prefix /opt/pipeshellx
/opt/pipeshellx/bin/pipeshellx --version
```

The install contains the executable, `pipeshellx::lib`, its supported public
headers, CMake package metadata, and the project license/readme/changelog.
A downstream CMake project can use:

```cmake
find_package(pipeshellx 0.6 CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE pipeshellx::lib)
```

The installed header surface is `include/psx/`, excluding command-handler
headers under `psx/cli/` and the private OS backend selector. Flat application
headers used by the executable are intentionally not installed.

Native-enabled packages retain their OpenSSL 3 and Threads dependencies.
Native-disabled packages do not require OpenSSL. The CI packaging jobs verify
installation, relocation, and consumption from a fresh downstream project.

## Inventory

Inventories are INI files. Sections define groups; repeated host entries add
group membership.

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
`san`, and `native_port`. Transport is
`ssh` or `native`; ports must be in
`1..65535`.

Commands using normal inventory resolution choose the first available source:

1. `-i FILE` or `--inventory FILE`;
2. non-empty `PIPESHELLX_INVENTORY`;
3. `./inventory.ini`;
4. legacy `./clients.txt`;
5. `$XDG_CONFIG_HOME/pipeshellx/inventory.ini`, or
   `$HOME/.config/pipeshellx/inventory.ini` when XDG is unset.

`run`, `ping`, `diff`, and
`hosts list` use this resolver. A remote
`pipe` requires an explicit inventory. Selectors
`-g GROUP`, `-t TAG`, and
`-H host1,host2` are mutually exclusive.

### Transport selection

Each host defaults to SSH unless its inventory entry says
`transport=native`.

- Without `--transport`, every selected host must declare the same
  transport.
- A mixed selection is a configuration error.
- `--transport ssh|native` overrides the selected set.
- `ping` is SSH-only; `diff` and remote
  `pipe` are native-only.

This prevents a native host from being silently routed over SSH, or the
reverse.

### Host administration

```bash
pipeshellx hosts list -i fleet.ini
pipeshellx hosts add web-03 -i fleet.ini --group web \
  --user deploy --transport ssh --tag prod
pipeshellx hosts remove web-03 -i fleet.ini
pipeshellx hosts import clients.txt -i fleet.ini
```

Mutations require an explicit INI target. They reject duplicates,
secret-bearing values, and a target named `clients.txt`; legacy imports retain
supported identity metadata while discarding recognized secrets. Rewrites use
a same-directory temporary file and atomic rename, preserve existing
permissions, and create new POSIX files privately. They are not transactions
across concurrent writers, so administrative writes must be coordinated.

## SSH authentication

PipeShellX starts `ssh` from `PATH` instead of embedding an SSH
stack. OpenSSH therefore owns keys, certificates, agents, configuration,
aliases, proxy rules, and remote-shell startup. PipeShellX supplies:

```text
StrictHostKeyChecking=accept-new
UserKnownHostsFile=<inventory>.known_hosts
BatchMode=yes
ConnectTimeout=5
ServerAliveInterval=15
```

`accept-new` is trust on first use. Pre-seed and verify the per-inventory
known-hosts file when first-contact trust is not acceptable. Changed keys are
rejected.

Modern one-shot commands have no password flag. The legacy interactive shell
can pass an in-memory password to `sshpass -d` through a file descriptor; it
does not put the password on argv or persist it. Keys, agents, or SSH
certificates are recommended for unattended use.

SSH execution always involves the target's configured login shell. Match it
with `--shell posix|cmd|powershell`; quoting is not a sandbox or a
general injection defense.

## Distributed commands

`run` requires `--` before command argv:

```bash
pipeshellx run -i fleet.ini -g web --stream -c 32 -- uptime
```

Without `--policy`, `run` is an unrestricted trusted-operator
surface. The legacy shell's demonstration allowlist does not apply. See
[Pipelines and output](pipelines.md) for scheduling, output, retries, exit
codes, JSON, `diff`, and pipeline contracts.

## Native CA and nodes

Native mode is psx/1 over mutual TLS 1.3. Both peers verify a fleet CA.
Inventory `san=` pins the expected node URI; node
`--allow` restricts controller URIs.

Create a CA and identities:

```bash
pipeshellx ca init --cn pipeshellx-fleet --dir ca
pipeshellx ca issue --san spiffe://psx/controller/ops \
  --ca ca --out controller
pipeshellx ca issue --san spiffe://psx/node/node-01 \
  --ca ca --out node-01
```

To keep a node key on the node, generate a key and CSR there, transfer only
the CSR, and sign it on the CA host:

```bash
pipeshellx node keygen --san spiffe://psx/node/node-01 --out node-01
pipeshellx ca sign --ca ca --csr node-01.csr \
  --san spiffe://psx/node/node-01 --out node-01.crt
```

Revocation updates `ca/crl.pem`; administrators must distribute it and enable
`--crl` on both endpoints that should enforce it:

```bash
pipeshellx ca revoke --ca ca --cert node-01.crt
```

Run a node under a dedicated, least-privileged account:

```bash
pipeshellx node --listen 0.0.0.0:7433 \
  --cert /etc/pipeshellx/node.crt \
  --key /etc/pipeshellx/node.key \
  --ca /etc/pipeshellx/ca.crt \
  --allow spiffe://psx/controller/ops \
  --policy /etc/pipeshellx/node.policy \
  --control /run/pipeshellx/node.ctl
```

Omitting `--allow` admits any CA-signed controller and emits a
warning. `--policy` is optional; when present it rejects disallowed
argv before spawn with exit `126`. Without it, an admitted controller
may execute arbitrary argv as the node account. Neither certificate identity
nor policy creates a sandbox.

Query the optional local status socket with:

```bash
pipeshellx node status --control /run/pipeshellx/node.ctl
```

`node systemd-unit` and `node launchd-plist`
emit service definitions from the same node flags; they do not install or
start services. Review accounts, paths, permissions, and network exposure
before installation. A generated hardened systemd unit requires its control
socket below `/run/pipeshellx/` so its `RuntimeDirectory` and
write restrictions remain consistent.

The native protocol provides channel separation, stream credit, liveness,
graceful drain, and connection-loss fencing. It does not provide
reconnect/resume. See the [psx/1 specification](wire_protocol.md).

## Operations checklist

- Run controllers and nodes as dedicated, least-privileged accounts.
- Pre-seed SSH host keys where TOFU is insufficient.
- Scope CAs, inventory SAN pins, and node allowlists narrowly.
- Configure both controller and node policy where commands must be restricted.
- Protect and rotate private keys, inventories, CRLs, policies, logs, and
  audit files.
- Bound host concurrency and choose an output policy appropriate for expected
  volume; lossless capture and spool can consume unbounded memory or disk.
- Supervise nodes with systemd, launchd, or an equivalent service manager.
- Add external sandboxing, quotas, and tamper-resistant audit storage when the
  deployment requires them.

Operational logs default under `$XDG_STATE_HOME/pipeshellx/` or
`$HOME/.local/state/pipeshellx/`. An optional
`run --audit-log FILE` records unsigned JSON Lines lifecycle metadata,
including argv and topology but not captured stdout/stderr. An unwritable
audit path warns and execution continues. See [SECURITY.md](../SECURITY.md)
for the complete trust and limitation model.
