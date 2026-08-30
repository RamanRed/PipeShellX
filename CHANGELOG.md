# Changelog

This file records user-visible changes. The repository currently identifies
the executable/library as 0.6.0, but `v0.6.0` has not been tagged and
release artifacts have not been published.

## Unreleased

### Documentation

- Reconciled the security, authentication, inventory, distributed execution,
  pipeline, architecture, system-flow, testing, and Windows documentation with
  the current v0.6 behavior.
- Distinguished the legacy demo shell allowlist, optional controller policy,
  optional native-node policy, and trusted node-operator model.

### Release work still open

- Create and validate the `v0.6.0` tag and release artifacts.
- Windows controller/native-node support, reconnect/resume, general non-linear
  remote DAGs, SSH cross-node edges, sandboxing, signed audit, and the full
  cross-platform reference-scenario gate remain deferred.

## 0.6.0 - Pending

### Added

- Lowercase `pipeshellx` multi-command CLI:
  `run`, `ping`, `diff`, `pipe`,
  `hosts`, `ca`, `node`, and legacy
  `shell`.
- INI inventories with groups, tags, selectors, per-host SSH/native transport,
  SAN pins, and native ports; deterministic precedence from explicit
  `-i` through environment/project/user sources.
- Safe `hosts list|add|remove|import` operations with secret and
  duplicate checks, legacy `clients.txt` compatibility, and atomic
  INI replacement.
- Native psx/1 mutual-TLS backplane with separate stdin/stdout/stderr channels,
  credit flow control, liveness leases, GOAWAY drain, CRLs, SAN authorization,
  connection-loss fencing, and CA/CSR commands.
- Optional `node --policy FILE` defense in depth. A denied native
  request is rejected before spawn, reports on stage stderr, and exits 126.
- Grouped, streaming, JSON, ordered, consensus, bounded/drop-aware, and
  disk-spooled run output; timeout, Ctrl-C cancellation, fail-fast, SSH retry
  gating, native canaries, and opt-in JSONL audit.
- Local general-DAG execution from restricted YAML with declared-edge
  fan-in/fan-out, bounded edge buffers, lifecycle/reap completion, and
  deterministic pipefail.
- Linear mixed local/native pipelines and first-stage native group fan-in.
- Strict native `diff` consensus using exact stdout only, with exit 0
  for unanimity, 1 for drift, and 2 for a host or usage/configuration failure.
- Installable lowercase executable/library and relocatable CMake package,
  packaging smoke tests, and an explicit SSH-only native-disabled build.

### Changed

- `run` is an unrestricted trusted-operator tool unless
  `--policy FILE` is supplied. The fixed allowlist remains scoped to
  the legacy `shell` demo.
- Per-host transport is honored. A selected mixed SSH/native inventory now
  requires an explicit `--transport` override instead of silently
  choosing a route.
- SSH host keys use `accept-new` with a per-inventory trust store;
  secrets are not placed on argv or persisted by inventory mutation.
- The CI definition covers Linux GCC/Clang and macOS AppleClang Debug/Release,
  warnings-as-errors, sanitizers, layering, packaging, and native-disabled
  builds. macOS+Homebrew GCC is excluded as an unsupported Apple SDK pairing.

### Fixed

- Native result capture preserves stdout and stderr separately and records
  timeout, cancellation, abort, and dropped-byte state.
- Mixed/native pipelines no longer fabricate exit 0 for unfinished upstream
  stages after an early consumer exit; they fence/account them as 137 and keep
  rightmost-nonzero pipefail semantics.
- `diff` rejects unknown/incomplete options and invalid ports, treats
  nonzero stage exits as host failures, and excludes stderr from consensus.
- Workflow action pins and compiler selection were updated for current hosted
  runners; nightly benchmark configuration disables tests.
- POSIX/GCC portability diagnostics and aggregate-initializer warning handling
  were corrected without weakening the Clang warnings-as-errors build.

### Known limitations

- General non-linear DAGs are local-only. Any graph containing a remote stage
  must be one declared chain and is otherwise rejected explicitly.
- Remote pipeline edges use native transport only; SSH cross-node edges are not
  implemented.
- Native transport has no reconnect/resume.
- Windows is an SSH-target tier only; the Windows controller, native node,
  IOCP backend, and SCM service are not implemented.
- Node policy is optional and is not a sandbox. There is no privilege
  separation, stage sandbox, secure-memory type, or signed/tamper-evident
  audit.
- Local tests and the CI configuration do not by themselves constitute a
  published or fully cross-platform release.
- Lossless `block` capture is unbounded. Spool bounds only its in-memory tail;
  temporary-disk growth and final full-result materialization remain unbounded.
