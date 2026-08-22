# PipeShellX

## Description

PipeShellX is a C++ systems programming project that demonstrates how operating system primitives can be used to build a controlled command execution workflow.

The project focuses on low-level process execution using:

- `fork()` for process creation
- unnamed pipes for IPC
- `dup2()` for standard stream redirection
- `execvp()` for command execution
- `waitpid()` for process lifecycle management

It provides an interactive terminal client, structured logging, command validation, and a process manager designed to avoid deadlocks and zombie processes.

The project also supports distributed execution over SSH when a `clients.txt` file is present. In that mode, a command entered once in the terminal is executed in parallel across all configured remote clients.

## Key Features

- Interactive terminal-based command execution
- Command allowlist with input validation
- Safe execution without shell invocation
- Parent/child IPC using stdin, stdout, and stderr pipes
- Nonblocking pipe handling with `poll()`
- Timeout-aware process execution
- Process group cleanup on timeout or failure
- Structured logging with timestamp, PID, session ID, and command
- Session management abstraction for concurrent execution tracking
- Distributed SSH execution across multiple configured clients
- Per-client grouped output and normalized remote error reporting
- Hardened SSH defaults: `ssh` from `PATH`, `accept-new` host keys with a per-inventory `known_hosts`, `BatchMode`, passwords handed to `sshpass` over a pipe (never on a command line)
- File-based structured logging by default; `--verbose` mirrors log lines to the console

## Operating System Concepts Demonstrated

- Process creation with `fork()`
- Program replacement with `execvp()`
- Inter-process communication with unnamed pipes
- File descriptor duplication with `dup2()`
- Nonblocking I/O with `fcntl()`
- Event-driven pipe monitoring with `poll()`
- Child reaping with `waitpid()`
- Signal handling with `SIGCHLD`
- Resource limiting with `setrlimit()`
- Process-group based termination with `kill()`

## Architecture Overview

The system is organized into a small set of focused modules:

- `TerminalClient`
  Handles user interaction, command history, colored output, and terminal-facing errors.
- `CommandExecutor`
  Parses commands, validates user input, resolves executables from trusted paths, and prepares execution context.
- `ProcessManager`
  Creates pipes, forks child processes, redirects stdio, executes commands, collects output, reaps child processes, and runs parallel SSH workers for distributed execution.
- `SessionManager`
  Tracks background command sessions using worker threads and per-session state.
- `Logger`
  Provides structured logs for command execution, process lifecycle, IPC activity, and failures.
- `ClientConfig`
  Loads and validates `clients.txt` for distributed SSH execution and derives the per-inventory `known_hosts` path.
- `ssh_auth`
  Builds the hardened OpenSSH argument vector and classifies authentication and host-key failures.
- `Pipe`
  A reusable RAII pipe abstraction included as a supporting IPC utility.

For more detail, see:

- [docs/architecture.md](docs/architecture.md)
- [docs/distributed_execution.md](docs/distributed_execution.md)
- [docs/ipc_design.md](docs/ipc_design.md)
- [docs/process_management.md](docs/process_management.md)

## Installation

### Prerequisites

- CMake 3.20 or newer
- C++20-capable compiler (GCC ≥ 11, Clang ≥ 14, Apple Clang ≥ 14)
- Ninja or Make
- Linux or macOS (both are built and tested in CI; Windows arrives in Phase 3 of `PLAN.md`)
- network access on the first configure: GoogleTest is fetched automatically
  (`-DPIPESHELLX_SYSTEM_GTEST=ON` uses an installed copy instead)
- OpenSSH client ≥ 7.6 (for `StrictHostKeyChecking=accept-new`; 2017 or newer on every supported OS)
- `sshpass`, only for password-backed hosts

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable will be generated at:

```bash
build/bin/PipeShellX
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the sanitizer build and the other CMake options.

## Usage

Run the interactive client:

```bash
./build/bin/PipeShellX
```

At the prompt, enter an allowed command:

```text
cmd> pwd
cmd> whoami
cmd> echo hello
cmd> date
cmd> exit
```

### Built-in Terminal Commands

- `history` shows command history
- `exit` or `quit` terminates the application

### Command-Line Options

| Flag | Effect |
|---|---|
| `-v`, `--verbose` | log at DEBUG level and mirror every log line to stderr |
| `--log-file <path>` | write the log to `<path>` instead of the default location |
| `-h`, `--help` | show usage and exit |
| `--version` | print the version and exit |

Logs go to `$XDG_STATE_HOME/pipeshellx/pipeshellx.log` (falling back to
`~/.local/state/pipeshellx/pipeshellx.log`) by default, so the terminal stays
clean unless `--verbose` is given. An unknown flag exits with status `2`.

## Distributed Usage

Create a `clients.txt` file in the directory where you run the application:

```text
user@192.168.1.10
user@192.168.1.11
user@192.168.1.12
```

Then run the shell normally:

```bash
./build/bin/PipeShellX
```

When `clients.txt` is present, an entered command is executed on all configured clients in parallel over SSH.

Host keys are recorded on first contact in `clients.txt.known_hosts` next to the inventory (`StrictHostKeyChecking=accept-new`). A host whose key later changes is refused and reported as `ERROR: host key verification failed`.

SSH authentication is delegated to the system OpenSSH client by default. When a password is supplied through the interactive `add-client` prompt, PipeShellX keeps it in memory for the current session and invokes `sshpass -d <fd> ssh ...` for that client, handing the password over a pipe so that it never appears on a command line. Passwords are not written to `clients.txt`.

### SSH Authentication

PipeShellX supports these SSH authentication modes:

- key-based authentication through the local OpenSSH client
- password-based authentication through `sshpass` when the password is supplied interactively
- `ssh-agent` authentication through the local OpenSSH client
- `~/.ssh/config`-driven authentication and host settings through the local OpenSSH client

No application-specific authentication configuration is required for key-based auth, `ssh-agent`, or `~/.ssh/config`. PipeShellX defers those behaviors to the system `ssh` client.

For password-only hosts:

1. run `add-client user@host`
2. answer `Password required? (y/n)` with `y`
3. enter the password at the hidden prompt

The password is kept in memory only for the current process. It is not printed to the terminal, not written to logs, and not persisted to `clients.txt`.

Example:

```text
cmd> whoami
CLIENT user@192.168.1.10
ubuntu

CLIENT user@192.168.1.11
devuser

CLIENT user@192.168.1.12
laptop
```

Example failure output:

```text
cmd> hostname
CLIENT user@192.168.1.10
ERROR: connection failed
```

## Example Commands

The current allowlist includes:

- `ls`
- `cat`
- `echo`
- `pwd`
- `whoami`
- `date`
- `hostname`
- `uptime`
- `df`
- `du`
- `ps`
- `id`

Example session:

```text
cmd> ls
cmd> pwd
cmd> whoami
cmd> echo hello
cmd> date
```

Distributed example:

```text
cmd> uptime
cmd> hostname
cmd> df -h
```

## Project Directory Structure

```text
.
├── CMakeLists.txt
├── CONTRIBUTING.md
├── PLAN.md                    # roadmap and architecture (single source of truth for scope)
├── README.md
├── SECURITY.md
├── .clang-format / .clang-tidy
├── .github/workflows/         # ci.yml (build + test matrix, sanitizers), bench.yml (nightly)
├── bench/
│   └── baseline.cpp           # pipeshellx_bench_baseline (docs/benchmarks.md)
├── docs/
│   ├── adr/                   # architecture decision records
│   ├── architecture.md
│   ├── authentication.md
│   ├── benchmarks.md
│   ├── deployment.md
│   ├── distributed_execution.md
│   ├── ipc_design.md
│   ├── process_management.md
│   ├── security.md
│   ├── system_flow.md
│   └── testing.md
├── include/
│   ├── cli_options.hpp
│   ├── client_config.hpp
│   ├── client_manager.hpp
│   ├── command_executor.hpp
│   ├── ipc_engine.h
│   ├── logger.hpp
│   ├── process_manager.hpp
│   ├── session_manager.hpp
│   ├── ssh_auth.hpp
│   └── terminal_client.hpp
├── src/
│   ├── CMakeLists.txt
│   ├── cli_options.cpp
│   ├── client_config.cpp
│   ├── client_manager.cpp
│   ├── command_executor.cpp
│   ├── ipc_engine.cpp
│   ├── logger.cpp
│   ├── main.cpp
│   ├── process_manager.cpp
│   ├── session_manager.cpp
│   ├── ssh_auth.cpp
│   └── terminal_client.cpp
└── tests/
    ├── CMakeLists.txt         # GoogleTest via FetchContent; gtest_discover_tests
    ├── test_support.hpp       # ScopedTempCwd, ScopedEnv
    ├── test_cli_options.cpp
    ├── test_client_config.cpp
    ├── test_command_executor.cpp
    ├── test_ipc.cpp
    ├── test_logger.cpp
    ├── test_process_manager.cpp
    └── test_ssh_auth.cpp
```

## Future Improvements

- True live streaming output to the terminal callback
- Deeper integration of `SessionManager` into the interactive runtime path
- Configurable command policy and resource limits
- Stronger sandboxing with seccomp, namespaces, or containers
- Expanded automated tests for concurrency, stress, and failure injection
- Log rotation and JSON log output
- Better support for production-style service deployment
- Configurable client configuration path instead of fixed `clients.txt`
- True live per-client streaming output for distributed execution

## Additional Documentation

- [docs/system_flow.md](docs/system_flow.md)
- [docs/distributed_execution.md](docs/distributed_execution.md)
- [docs/authentication.md](docs/authentication.md)
- [docs/security.md](docs/security.md)
- [docs/deployment.md](docs/deployment.md)
- [docs/testing.md](docs/testing.md)

## License

PipeShellX is licensed under the [Apache License 2.0](LICENSE).

See [PLAN.md](PLAN.md) for the product roadmap from the current prototype to the 1.0 launch.
