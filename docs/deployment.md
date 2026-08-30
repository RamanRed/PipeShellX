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
headers, CMake package metadata, and the project license, third-party notices,
readme, and changelog.

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

## Release archives and verification

No v0.6 release has been published yet. The release workflow is ready to
produce these native-enabled archives when v0.6.0 is published:

| Target | Build runner | Archive root |
| --- | --- | --- |
| `linux-x86_64` | Ubuntu 22.04 x86-64 | `pipeshellx-0.6.0-linux-x86_64/` |
| `macos-x86_64` | macOS 15 Intel | `pipeshellx-0.6.0-macos-x86_64/` |
| `macos-arm64` | macOS 15 Apple silicon | `pipeshellx-0.6.0-macos-arm64/` |

Each build runs the release test suite, installs into that single versioned
root, verifies clean archive consumption, rejects a dynamic OpenSSL dependency
from the CLI, and emits the following assets:

| Asset | Purpose |
| --- | --- |
| `pipeshellx-VERSION-TARGET.tar.gz` | Versioned install tree. |
| `*.tar.gz.sha256` | Per-archive SHA-256 checksum. |
| `*.tar.gz.spdx.json` | SPDX JSON software bill of materials generated from the install tree. |
| `*.tar.gz.sigstore.json` | Keyless Cosign signature bundle for the archive. |
| `*.tar.gz.provenance.sigstore.json` | GitHub SLSA build-provenance attestation bundle. |
| `*.tar.gz.sbom-attestation.sigstore.json` | GitHub attestation binding the SPDX SBOM to the archive. |
| `CHECKSUMS.sha256` and its two `*.sigstore.json` files | Aggregate archive checksums, Cosign signature, and GitHub provenance bundle. |

A manual dispatch against `main` exercises the complete build, smoke,
v0.5-to-current upgrade, signing, and attestation path, then uploads a
short-lived `pipeshellx-0.6.0-release-dry-run` Actions artifact. It does not
create a tag or GitHub release. Only a pushed `v0.6.0` tag matching the CMake
project version enters the publish job.

Maintainers run the non-publishing gate explicitly:

```bash
gh workflow run release.yml --ref main
```

After that run and all required checks pass, publication is deliberately a
separate tag operation against the validated commit:

```bash
git tag -a v0.6.0 VALIDATED_COMMIT -m "PipeShellX v0.6.0"
git push origin refs/tags/v0.6.0
```

These commands describe the release procedure; they have not been run for
v0.6.0. A mismatched tag and project version fails metadata validation before
publication.

### Verify a published archive

Install `cosign`, GitHub CLI `gh`, `jq`, and a SHA-256 implementation first.
Download all assets into one directory, select the archive for the machine,
and verify it before extraction. For a future tagged v0.6.0 release:

```bash
repo=patil-rushikesh/PipeShellX
version=0.6.0
tag="v$version"
target=linux-x86_64 # or macos-x86_64 / macos-arm64
base="pipeshellx-$version-$target"
identity="https://github.com/$repo/.github/workflows/release.yml@refs/tags/$tag"
issuer=https://token.actions.githubusercontent.com

shasum -a 256 -c "$base.tar.gz.sha256"
shasum -a 256 -c CHECKSUMS.sha256
jq -e '.spdxVersion and (.packages | type == "array")' \
  "$base.tar.gz.spdx.json" >/dev/null

cosign verify-blob "$base.tar.gz" \
  --bundle "$base.tar.gz.sigstore.json" \
  --certificate-identity "$identity" \
  --certificate-oidc-issuer "$issuer"
cosign verify-blob CHECKSUMS.sha256 \
  --bundle CHECKSUMS.sha256.sigstore.json \
  --certificate-identity "$identity" \
  --certificate-oidc-issuer "$issuer"

gh attestation verify "$base.tar.gz" --repo "$repo" \
  --bundle "$base.tar.gz.provenance.sigstore.json" \
  --predicate-type https://slsa.dev/provenance/v1 \
  --cert-identity "$identity" \
  --cert-oidc-issuer "$issuer"
gh attestation verify "$base.tar.gz" --repo "$repo" \
  --bundle "$base.tar.gz.sbom-attestation.sigstore.json" \
  --predicate-type https://spdx.dev/Document/v2.3 \
  --cert-identity "$identity" \
  --cert-oidc-issuer "$issuer"
```

For a manually dispatched dry run from `main`, use the downloaded dry-run
artifact and change only the expected identity suffix from
`@refs/tags/v0.6.0` to `@refs/heads/main`. Verification against an unexpected
tag, branch, repository, OIDC issuer, digest, or predicate type must fail.

### Install and upgrade atomically

Keep each release immutable and keep inventories, certificates, policies,
logs, and other operator-managed state outside its directory. After verifying
an archive, install it beside the old version and atomically replace a relative
`current` symlink:

```bash
install_root="$HOME/.local/pipeshellx"
archive_root="$base"
mkdir -p "$install_root/releases" "$install_root/etc/pipeshellx"
tar -xzf "$base.tar.gz" -C "$install_root/releases"

next="$install_root/current.next.$$"
ln -s "releases/$archive_root" "$next"
python3 -c 'import os,sys; os.replace(sys.argv[1], sys.argv[2])' \
  "$next" "$install_root/current"
"$install_root/current/bin/pipeshellx" --version
```

The release smoke performs this process with a fixed, source-derived v0.5.0
install fixture on all three targets. It proves that the old uppercase
`PipeShellX` executable is not retained through the `current` path, the new
lowercase `pipeshellx` CLI and downstream CMake package work, and state kept
under `etc/pipeshellx/` is not overwritten. Rollback uses the same atomic
symlink replacement to select the previous versioned directory; do not merge
two release trees in place.

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
