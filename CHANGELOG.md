# Changelog

This file records user-visible changes. The repository and executable identify
as 0.6.0, but no version tag or release artifact has been published on GitHub.

## [Unreleased] — v0.6.0 candidate

### Added

- Lowercase `pipeshellx` multi-command CLI with `run`, `ping`, `diff`, `pipe`,
  `hosts`, `ca`, `node`, and the legacy `shell` REPL.
- INI inventories with groups, tags, selectors, SSH/native transport metadata,
  SAN pins, native ports, deterministic resolution precedence, and legacy
  `clients.txt` import.
- Host inspection plus atomic `hosts add|remove|import` mutations with
  duplicate and secret rejection.
- Native psx/1 mutual-TLS transport with CA-issued identities, SAN
  authorization, optional CRLs, separate stdin/stdout/stderr channels,
  per-stream credit, liveness leases, graceful drain, and connection-loss
  fencing.
- Controller and node command policies as independent, optional defense in
  depth. Node policy denial happens before spawn and returns stage exit 126.
- Grouped, streaming, ordered, JSON, and consensus output; timeout,
  cancellation, fail-fast, idempotent SSH retries, native canaries,
  drop-aware capture, disk spooling, and opt-in JSONL audit.
- All-local DAG execution from restricted YAML with declared-edge ordering,
  fan-in, fan-out, per-edge buffering, child lifecycle accounting, and
  deterministic pipefail.
- Linear mixed local/native pipelines and first-stage native group fan-in.
- Native `diff` consensus over exact successful stdout, with stderr kept
  diagnostic and nonzero stage exits treated as host failures.
- Installable executable and library, relocatable CMake package, downstream
  packaging smoke tests, and an explicit native-disabled SSH-only build.

### Changed

- `run` is documented and enforced as an unrestricted trusted-operator surface
  unless `--policy FILE` is supplied. The fixed allowlist remains limited to
  the legacy `shell` demo.
- Per-host transport is honored. A mixed SSH/native selection requires an
  explicit `--transport` override instead of silently choosing a route.
- SSH host trust uses `accept-new` with a per-inventory known-hosts file;
  inventory mutation does not persist passwords or recognized secrets.
- The supported controller/native-node platforms are Linux and macOS. Windows
  remains an SSH-target tier reached from a POSIX controller.
- CI covers Linux GCC/Clang and macOS AppleClang in Debug and Release, Linux
  Clang ASan+UBSan, layering, install/downstream packaging, and a
  native-disabled build. The benchmark runs separately as a best-effort
  scheduled workflow.

### Fixed

- Native results preserve stdout and stderr separately and retain timeout,
  cancellation, abort, and dropped-byte metadata.
- Mixed/native pipelines fence and account unfinished upstream stages after an
  early consumer exit rather than reporting fabricated success; deterministic
  rightmost-nonzero pipefail is preserved.
- `diff` rejects incomplete or unknown options and invalid ports, excludes
  stderr from consensus, and treats nonzero remote stage exits as host
  failures.
- CI compiler selection and hosted-action versions were updated for current
  runners, and nightly benchmark configuration no longer builds the test
  suite.
- GCC/POSIX portability diagnostics and intentional aggregate-initializer
  handling were corrected without weakening the Clang warnings-as-errors
  build.
- GoogleTest discovery now has an explicit timeout suitable for sanitizer
  builds, preventing slow test enumeration from failing an otherwise valid
  build.

### Documentation and repository

- Rewrote the public README, contribution guide, security policy/trust model,
  and changelog around implemented v0.6 behavior and explicit limitations.
- Added a CI status badge, a clear pre-release statement, a platform support
  matrix, and concise build, install, inventory, and usage examples.
- Removed stale internal-planning and false universal-boundedness claims from
  contributor-facing documentation.
- Added ignore rules for local agent state and root-level generated inventory,
  credential, audit, and runtime artifacts.
- Added a dependency-free documentation link check to CI, immutable action
  pins, job timeouts, benchmark pipe-failure propagation, and weekly
  Dependabot updates for GitHub Actions.
- Limited installed headers to the supported `include/psx/` API surface;
  internal CLI/application and OS-backend headers are no longer published as
  compatibility commitments.

### Known limitations

- General non-linear DAGs are local-only. A graph containing a remote stage
  must be one declared chain.
- Remote pipeline edges use native transport; SSH cross-node edges are not
  implemented.
- Native transport has no reconnect or resume.
- There is no Windows controller, native Windows node, IOCP backend, SCM
  service, or Windows CI job.
- Node policy is optional and is not a sandbox. There is no privilege
  separation, per-stage sandbox, locked secret-memory type, or signed audit.
- The lossless `block` capture policy is unbounded. Spool bounds its in-memory
  tail but not temporary-disk growth or final full-result materialization.
- A passing source-tree CI run is not a published release or a substitute for
  signed artifacts, checksums, provenance, and multi-platform release
  qualification.

## Published releases

No tagged release or release artifact has been published yet.

[Unreleased]: https://github.com/patil-rushikesh/PipeShellX/commits/main
