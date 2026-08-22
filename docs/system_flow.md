# System Execution Flow

## End-to-End Flow

This document describes what happens from user command entry to child process completion.

## Interactive Flow

### 1. Application Startup

`main()`:

- logs startup
- constructs `TerminalClient`
- enters the interactive loop

### 2. Prompt and Input

`TerminalClient::run()`:

- prints `cmd>`
- reads a line from stdin
- handles `exit`, `quit`, or `history`
- forwards other commands for execution

### 3. Session Context Creation

For interactive usage, the terminal client derives a synthetic session ID:

```text
interactive-<pid>
```

This is used for logging and tracing.

### 4. Command Execution Request

`TerminalClient::handleCommand()`:

- stores the command in history
- constructs `CommandExecutor`
- passes the raw command string, session ID, callback, and timeout

### 5. Parsing and Validation

`CommandExecutor::execute()`:

- parses the command string into arguments
- validates the command name and arguments
- checks the allowlist
- rejects explicit paths
- resolves the executable from trusted system directories
- logs the validated command

### 6. Process Launch

`CommandExecutor::runCommand()`:

- creates `ProcessManager`
- logs start of execution
- calls `ProcessManager::execute(...)`

### 7. IPC and Child Execution

`ProcessManager::execute()`:

- creates pipes
- forks
- configures the child
- calls `execvp()`
- monitors stdout/stderr using `poll()`
- reaps the child with `waitpid()`
- returns output, stderr, exit code, and timeout status

### 8. Output Handling

The returned output is split into lines and passed to the terminal callback:

- stdout lines are printed normally
- stderr lines are printed in red

### 9. Completion

`TerminalClient`:

- reports non-zero exit codes as user-visible errors
- returns to the prompt

## Logging Flow

Execution logs now exist at four levels:

- startup and terminal-level failures
- command validation and dispatch
- process creation and reaping
- IPC activity and error paths

Every log line includes:

- timestamp
- log level
- PID
- session ID
- command

## Control Flow Summary

```text
User input
  -> TerminalClient
  -> CommandExecutor
  -> ProcessManager
       -> psx::os::Pipe::create()            stdout/stderr (+ stdin when there is input)
       -> psx::os::Process::spawn()          posix_spawn, own process group, stdio via file actions
       -> psx::runtime::Reactor::run()
            readable  -> drain the pipe until WouldBlock
            writable  -> feed input, close stdin after the last byte
            child exit-> Process::tryWait() (one waitpid per child)
            deadline  -> SIGKILL the process group, 2 s drain grace
  -> CommandResult
  -> Terminal output
```

## Error Flow

Typical failure points:

- parse error
- validation failure
- missing executable (`posix_spawn` fails synchronously: exit 127 and the reason on stderr)
- pipe creation failure
- spawn failure
- timeout
- event-loop failure

Current behavior:

- L0/L1 failures are `psx::Result` values; `ProcessManager` turns the
  unrecoverable ones (no pipes, no event loop) into exceptions and the
  per-child ones (cannot start) into results
- terminal-level failures are shown to the user
- all major failure paths are logged
