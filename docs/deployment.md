# Deployment Guide

## Supported Environment

The project targets a POSIX-style environment with:

- CMake 3.20 or newer
- a C++20 compiler
- POSIX process APIs such as `fork`, `pipe`, `dup2`, `poll`, and `waitpid`

Linux and macOS are the supported platforms. Every commit is built and tested
on both — GCC and Clang, Debug and Release — by `.github/workflows/ci.yml`,
plus an AddressSanitizer/UndefinedBehaviorSanitizer job. Windows is not
supported yet; it arrives in Phase 3 of `PLAN.md`.

## Build Requirements

- C++20-capable compiler (GCC ≥ 11, Clang ≥ 14, Apple Clang ≥ 14)
- CMake ≥ 3.20 and Ninja or Make
- pthread support through `Threads`
- network access on the first configure (GoogleTest is fetched by
  `FetchContent`; pass `-DPIPESHELLX_SYSTEM_GTEST=ON` to use an installed copy,
  or `-DPIPESHELLX_BUILD_TESTS=OFF` to skip the test suite in a deployment build)

Runtime:

- OpenSSH client ≥ 7.6 on `PATH` (`accept-new` host-key policy; older clients fail every
  connection with `Bad configuration option`)
- `sshpass`, only for password-backed hosts

## Build Steps

From the project root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The build produces:

- library target: `pipeshellx_lib`
- executable: `build/bin/PipeShellX` (target `pipeshellx_app`)
- test binary: `build/bin/pipeshellx_tests`
- benchmark harness: `build/bin/pipeshellx_bench_baseline` (`-DPIPESHELLX_BUILD_BENCH=OFF` to skip)

## Compiler Settings

The project is configured to build with:

- `-std=c++20`
- `-Wall`
- `-Wextra`
- `-Werror`

These flags are enforced via the top-level CMake configuration.

## Running the Application

From the project root:

```bash
./build/bin/PipeShellX            # logs to the default log file
./build/bin/PipeShellX --verbose  # DEBUG level, log lines mirrored to stderr
./build/bin/PipeShellX --log-file /var/log/pipeshellx.log
./build/bin/PipeShellX --version
```

Sample interactive session:

```text
cmd> pwd
cmd> whoami
cmd> echo hello
cmd> exit
```

## Distributed Deployment Example

Create a `clients.txt` file next to the executable launch directory:

```text
user@192.168.1.10
user@192.168.1.11
```

Run:

```bash
./build/bin/PipeShellX
```

Example usage:

```text
cmd> uptime
cmd> hostname
cmd> df -h
```

When `clients.txt` is present, these commands are executed on all configured clients in parallel over SSH.

Host keys are stored in `clients.txt.known_hosts` next to the inventory on
first contact. To pre-seed it (recommended for fleets), copy the relevant
lines from an existing `known_hosts` or run `ssh-keyscan` against the hosts
and review the fingerprints.

## Logging

Logs are written to a file by default:

1. `--log-file <path>` if given;
2. `$XDG_STATE_HOME/pipeshellx/pipeshellx.log` if `XDG_STATE_HOME` is set;
3. `~/.local/state/pipeshellx/pipeshellx.log` otherwise (directories are created as needed);
4. `./pipeshellx.log` when `HOME` is unset.

If the file cannot be opened a warning is printed and lines go to stderr so
that nothing is lost. `--verbose` lowers the level to DEBUG and mirrors every
line to stderr.

Each log line includes:

- timestamp (milliseconds, local time)
- log level (`DEBUG`, `INFO`, `ERROR`)
- PID
- session ID
- client ID
- command

Rotation is not implemented yet; use `logrotate` (`copytruncate`) or a
supervisor that rotates files.

## Operational Recommendations

### Run as a Dedicated User

Do not run the application as root. Use a dedicated low-privilege account.

### Restrict the Host Environment

Recommended:

- minimal filesystem permissions
- isolated runtime environment
- limited accessible binaries
- constrained environment variables

### Process Supervision

For service-style operation, use a supervisor such as:

- `systemd`
- container runtime
- process monitor

### Resource Controls

The application already applies hardcoded child CPU and memory limits. For real deployment:

- external cgroup/container limits are recommended
- make execution limits configurable per deployment

## Packaging Notes

Install rules are defined for:

- executable to `bin`
- library to `lib`
- headers to `include`

You can install with:

```bash
cmake --install build
```

## Known Deployment Constraints

- current terminal client is interactive-first, not a daemon or network service
- `SessionManager` exists but is not yet the primary deployment-facing orchestration layer
- log rotation and JSON output are not implemented yet
- sandboxing is not strong enough yet for hostile multi-tenant environments
