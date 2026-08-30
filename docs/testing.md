# Testing

PipeShellX uses GoogleTest with each test discovered individually by CTest.
Tests are enabled by default and cannot be silently omitted: CMake uses an
installed GoogleTest when requested or fetches the pinned v1.17.0 source.

Release validation records the final discovered count and pass/skip result in
the commit or release handoff rather than hard-coding a number here as the suite
continues to grow.

## Coverage map

| Area | Evidence in the suite |
| --- | --- |
| OS primitives | Handle inheritance/leaks, pipe EOF/nonblocking/broken-pipe behavior, process spawn/groups/limits/reaping, sockets, TLS, poller/child/signal backends, atomic file rewrite. |
| Runtime | Reactor readiness, timers, child exits, signals, backend parametrization, and retry backoff. |
| Streams and sinks | Bounded-buffer policies, credit windows, line framing, spool replay, grouped/stream/JSON/ordered/consensus rendering. |
| Inventory and policy | INI parsing, precedence, selectors, legacy `clients.txt` import, identity preservation, secret rejection, transport validation, serialization, duplicate detection, and optional command policy. |
| Product CLI | Strict parsers and exit codes for `run`, `ping`, `diff`, `pipe`, `hosts`, `ca`, and `node`. |
| SSH | Hardened argv, target-shell quoting, fake `ssh`/`sshpass` workers, error classification, timeout, fail-fast, cancellation, concurrency, retry gating, and golden behavior. |
| Native transport | Frame/payload codecs, TLS identity/CRL, channel separation, credit/drop/spool behavior, leases, GOAWAY, session faults, pre-spawn node-policy rejection/exit 126, node fencing, concurrent connections, cancellation, timeout, fail-fast, canary, and audit integration. |
| Pipelines | Parser/planner/YAML validation, local chains, declared-edge order, local fan-in/fan-out, bounded slow-consumer edges, deterministic pipefail, mixed linear placement, and exact rejection of non-linear remote DAGs. |
| Diff | Strict option parsing, exact stdout consensus, stderr exclusion, drift/unanimous exit codes, and nonzero stage exit as host failure. |
| Packaging | Install-tree smoke, lowercase installed executable, relocatable exported CMake package consumed by a fresh downstream project, and required-OpenSSL failure behavior. |

Most transport tests use loopback sockets or deterministic fake executables and
do not require external network reachability. Scale tests exercise a
1,000-stream multiplexed session, 1,000 simulated nodes, and a smaller
concurrent real-mTLS server fan-out without requiring an external fleet.

## Running the suite

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use an installed GoogleTest for an offline build:

```bash
cmake -S . -B build-system-gtest \
  -DPIPESHELLX_SYSTEM_GTEST=ON \
  -DCMAKE_BUILD_TYPE=Debug
```

Run a focused CTest selection:

```bash
ctest --test-dir build -R 'DiffCommandTest|PipeCommandTest' --output-on-failure
```

Or invoke the test binary with a GoogleTest filter:

```bash
build/bin/pipeshellx_tests \
  --gtest_filter='InventoryTest.*:HostsSubcommandTest.*:RunSubcommandTest.*:DiffCommandTest.*'
```

## Alternate configurations

### Sanitizers

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPIPESHELLX_SANITIZE=address,undefined
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

The build system also accepts the thread sanitizer on supported toolchains,
although the main CI sanitizer job is ASan+UBSan.

### SSH-only build

```bash
cmake -S . -B build-native-off \
  -DPIPESHELLX_NATIVE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
cmake --build build-native-off --parallel
ctest --test-dir build-native-off --output-on-failure
```

Native-only source/tests are excluded, while the CLI must still explain that
`node`, `ca`, `diff`, native `run`,
and remote `pipe` are unavailable.

### Portable poll backend and soak

`PIPESHELLX_POLLER=poll` forces the portable reactor backend. CTest
also registers `golden_on_poll_backend` for the golden,
`ProcessManager`, and `CommandExecutor` suites.

`PIPESHELLX_SOAK=1` lengthens spawn/execute loops for descriptor and
zombie checks:

```bash
PIPESHELLX_SOAK=1 build/bin/pipeshellx_tests \
  --gtest_filter='ProcessTest.*:ProcessManagerTest.*'
```

### Layering

```bash
./scripts/check_layering.sh
```

The check rejects platform headers outside the OS implementation, public
platform types, and upward layer dependencies.

## Continuous integration

`.github/workflows/ci.yml` is configured for:

- Linux GCC and Clang, Debug and Release;
- macOS AppleClang, Debug and Release (macOS+Homebrew GCC is explicitly
  excluded because it is not a supported Apple SDK compiler pairing);
- warnings as errors;
- CLI version/help/error smoke tests;
- ASan+UBSan on Linux Clang Debug;
- layering lint;
- static-OpenSSL install/downstream-package smoke;
- an SSH-only native-disabled build/install/downstream smoke.

`.github/workflows/bench.yml` configures a Release build without the
test suite, starts localhost OpenSSH, runs the baseline spawn/fan-out harness,
and uploads the result as a best-effort nightly artifact.

These statements describe workflow configuration. They do not claim that an
unpublished commit or release tag has already passed GitHub-hosted CI.

## Known gaps

The current suite does not complete these release/roadmap items:

- a Windows controller/native-node build and MSVC/clang-cl test matrix;
- end-to-end qualification against a heterogeneous real SSH/native fleet;
- general non-linear remote DAG execution (the product rejects it);
- reconnect/resume testing (the protocol does not implement it);
- node-death containment for every hard-kill/platform case;
- in-tree continuous libFuzzer jobs (structured and mutation fuzz-style tests
  exist, but not the release fuzzing program);
- sandbox/privilege-separation tests, because those features are deferred;
- signed-audit and release-artifact verification, also deferred.

Passing the POSIX suite is necessary for v0.6, but it is not evidence that
deferred Windows, isolation, resilience, or release-distribution milestones
are complete.
