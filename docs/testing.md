# Testing Strategy

## Current Test Surface

The GoogleTest suite lives in `tests/` and is built unconditionally: GoogleTest
is fetched by CMake `FetchContent` (pinned to `v1.17.0`), so the tests can never
be silently skipped. Each `TEST` is registered with CTest individually through
`gtest_discover_tests`.

| File | Covers |
|---|---|
| `test_cli_options.cpp` | `--verbose`, `--log-file` (both forms), `--version`, `--help`, rejection of unknown or incomplete arguments |
| `test_client_config.cpp` | `clients.txt` parsing (legacy and `ssh://` forms), password rejection, duplicate detection, per-inventory `known_hosts` derivation, `ClientManager::addClient` persisting without secrets |
| `test_command_executor.cpp` | allowlist (`top` rejected, `hostname` accepted), explicit paths, shell metacharacters, length bounds, quoting, exit-code propagation |
| `test_logger.cpp` | line format, level filtering, console mirror, stderr fallback, unwritable paths, parent-directory creation, default log path resolution, 4-thread interleaving check |
| `test_process_manager.cpp` | local execution, invalid command, timeout; regression: fast remote failure is not a timeout; hung worker (silent loopback listener) does time out; timeout SIGKILLs the whole process group (grandchild holding the pipes); a holder outside the group is abandoned after the 2 s drain grace |
| `test_process_manager_golden.cpp` | the v0.1.0 behavioural contract of `ProcessManager` (output capture, remote grouping, error classes, password over a descriptor, timeouts) verified against a fake `ssh`/`sshpass` on `PATH` |
| `unit/os/*.cpp` | the shared `psx::os` suite: `Result`, `Handle` (CLOEXEC audit, 10 k cycles), `Pipe`, `Process` (`posix_spawn`, limits, groups, EINTR injection, soak), `Poller` per backend, `ChildExitSource` per mode, `SignalSource` |
| `unit/runtime/test_reactor.cpp` | the `Reactor` on the poll and native backends |
| `test_ssh_auth.cpp` | hardened `ssh` argv (`PATH` lookup, `accept-new`, `UserKnownHostsFile`, `BatchMode`, `ServerAliveInterval`), `sshpass -d <fd>` with the password never on argv, auth and host-key failure classifiers |

`tests/test_support.hpp` provides `ScopedTempCwd` (runs a test in a fresh
temporary working directory so a stray `clients.txt` or log file cannot
influence it), `ScopedEnv` (scoped environment overrides),
`refusedLoopbackClient()` (127.0.0.1:1 — a deterministic, instant SSH
"connection refused") and `SilentListener` (a loopback socket that never
accepts, i.e. a deterministic hung host). No test depends on external network
reachability.

## Existing Validation Areas

### IPC Tests

Current coverage (`tests/unit/os/test_pipe.cpp`) includes:

- write/read correctness and EOF as a zero-byte read
- nonblocking reads/writes (`WouldBlock`, partial writes past the pipe capacity)
- broken pipe reported as an error, never as `SIGPIPE`
- non-inheritable descriptors at creation, verified by enumerating the process's descriptors

### Process Tests

Current coverage includes:

- valid command execution
- invalid command execution
- timeout behavior

## Gaps in Automated Testing

Automated coverage is still missing for:

- end-to-end SSH execution against a real `sshd` (the docker-compose fleet arrives in Phase 2)
- high-volume stress execution
- concurrent session execution
- zombie-process regression tests
- interactive terminal behavior

## Manual Validation Already Performed

The system has been validated with:

- clean rebuild from scratch
- functional execution of:
  - `ls`
  - `pwd`
  - `whoami`
  - `echo hello`
  - `date`
- 125-command single-session soak test
- 4 concurrent sessions with 120 commands each
- post-run zombie inspection

These checks confirmed:

- stable IPC behavior
- correct process reaping
- no observed defunct child processes after validation
- no reported runtime errors during the soak and concurrency runs

## Recommended Test Categories

### Unit Tests

Add tests for:

- command parser behavior
- allowlist enforcement
- unsafe argument rejection
- executable path resolution
- logger formatting

### Integration Tests

Add tests that execute the full stack:

- terminal client to command executor
- command executor to process manager
- stdout/stderr capture correctness
- non-zero exit handling

### Concurrency Tests

Add repeatable automated tests for:

- many parallel command sessions
- repeated session create/end cycles
- interleaved process completions
- thread-safe logging under load

### Lifecycle and Reliability Tests

Add targeted tests for:

- timeout kills process group
- child reaping under fast exit conditions
- large stdout/stderr output without deadlock
- broken pipe handling

### Regression Tests

There was a previously fixed bug where the parent could continue looping after reaping a child and accidentally call `waitpid(-1, ...)`. Add a regression test for this exact path.

## Running Tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a single test or group with `ctest --test-dir build -R LoggerTest`.
`-DPIPESHELLX_SYSTEM_GTEST=ON` uses an installed GoogleTest instead of the
fetched copy (for offline builds).

### Sanitizers

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPIPESHELLX_SANITIZE=address,undefined
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

`PIPESHELLX_SANITIZE=thread` is used for reactor tests from Phase 2 on.

### Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request:

- `{ubuntu-latest, macos-latest} × {gcc, clang} × {Debug, Release}` — configure,
  build with `-Werror`, `ctest`, and a CLI smoke test (`--version`, `--help`,
  unknown flag exits `2`);
- an AddressSanitizer + UndefinedBehaviorSanitizer job (ubuntu, clang, Debug).

`.github/workflows/bench.yml` runs the baseline harness nightly (see
`docs/benchmarks.md`).

## Production-Readiness Testing Recommendations

Before treating the system as operationally mature, add:

- integration tests against a real SSH fleet (Phase 2)
- stress tests with larger command counts
- failure-injection tests around `fork`, `pipe`, and `poll`
- platform validation on target Linux environments
- log verification tests

## Success Criteria

A stronger test suite should prove:

- commands are validated correctly
- IPC never deadlocks under expected workloads
- child processes are always reaped
- concurrent sessions remain isolated
- no FD leaks appear across repeated executions
- security controls reject unsupported input consistently
