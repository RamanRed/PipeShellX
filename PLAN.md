# PipeShellX — Systems Architecture & Engineering Master Plan

**One-liner:** A single executable that composes processes into pipelines *across machines* —
anonymous pipes on one host, backpressured mTLS streams between hosts — with the same semantics
(`|`, exit codes, signals, EOF) an engineer already expects from a Unix shell.

**License:** Apache-2.0 (`LICENSE`, `NOTICE`).
**Plan status:** v2 — supersedes the product/launch plan (its product, legal, and risk material is
carried forward in Appendix A). This document is the single source of truth for scope; update it
in the same PR as any scope change.
**Historical baseline:** 2026-08-22, commit `2e10869`.
**Release snapshot (2026-08-30):** CMake and the lowercase
`pipeshellx` executable report `0.6.0`. Phase 5 has shipped
local general-DAG execution, native linear cross-node pipelines, strict
stdout-only consensus, canary/fail-fast, and the documented inventory/product
contract. Phase 5 is **not fully complete**: non-linear remote DAGs, SSH
cross-node edges, `splice()`, all-platform reference scenarios, and
the Windows controller/native node remain open. The repository has no
`v0.6.0` tag or published release artifacts yet.

---

## 1. Executive Summary & Project Status

### 1.1 Where the project is, and where it is going

PipeShellX v0.6 is a multi-command C++20 POSIX controller and optional native
node. `run` fans arbitrary operator argv over SSH or psx/1 mTLS,
`pipe` runs a general local DAG or a declared linear native pipeline,
`diff` performs exact stdout consensus, and `hosts` manages
INI inventories while retaining legacy `clients.txt` import. The
current product boundary is documented in `docs/architecture.md`,
`docs/security.md`, and `docs/pipelines.md`.

The following paragraph and gap tables preserve the historical starting point
at commit `2e10869`; they are not claims about v0.6. At that baseline,
PipeShellX was a ~2.6 kLOC C++20 POSIX application that ran an
allowlisted command either locally — `fork()` / `pipe()` / `dup2()` / `execvp()` / `poll()` /
`waitpid()` (`docs/process_management.md`, `docs/ipc_design.md`) — or in parallel on every host in
`clients.txt` by forking one `ssh` worker per host and multiplexing all worker pipes through one
`poll()` loop (`docs/distributed_execution.md`). Authentication is delegated to the system OpenSSH
client (`docs/authentication.md`). The docs are candid about the limits: output is buffered and
grouped after completion rather than streamed, `SessionManager` and the RAII `Pipe` helper are
not on the active path, resource limits are hardcoded, logging goes to stdout, and host-key
checking is disabled.

The target is an **industry-ready, cross-platform distributed pipeline tool** built on the same
primitives the project already demonstrates, generalised in three directions:

| Direction | From (historical baseline) | To (target) |
|---|---|---|
| **I/O model** | one `poll()` loop, unbounded `std::string` buffers, post-hoc output | completion-oriented runtime over `epoll` / `kqueue` / `IOCP`, bounded buffers with credit-based backpressure, live streaming |
| **Topology** | 1 controller → N hosts, one command, stdout collected | DAG of stages placed on nodes; edges are pipes locally and multiplexed mTLS streams remotely; SSH remains the zero-install transport |
| **Portability** | POSIX only, `fork`/`termios`/`/usr/bin/ssh` hardcoded, POSIX types in public headers | `psx::os` abstraction with `posix/` and `win32/` backends; no OS header leaks outside `src/os/`; Linux, macOS, Windows as first-class controller *and* target |

Two transport modes now coexist:

1. **Agentless (SSH)** — targets need only `sshd`. Authentication and
   the target remote shell remain OpenSSH responsibilities.
2. **Native backplane** — an optional `pipeshellx node` agent (same binary) speaking a framed,
   multiplexed, credit-flow-controlled protocol over mTLS/TCP. This is what makes cross-node pipe
   streaming, end-to-end backpressure, leases/fencing, and thousand-host fan-out feasible without
   one `ssh` process per host.

### 1.2 Historical baseline status (fresh clone, 2026-08-22)

This table is retained as migration evidence for commit `2e10869`.
It does not describe the v0.6 tree.

| Area | State | Evidence |
|---|---|---|
| Build | **Fails** on a fresh configure with `-Werror` (`-Wunused-private-field`), Apple clang 21 / CMake 4.2 | `include/logger.hpp:33` (`LogLevel currentLevel` never read) |
| Tests | 11 GTest cases in 4 files exist; target is **silently skipped** when GTest is absent (it was absent here) | `tests/CMakeLists.txt:3-7` |
| Platforms | macOS (dev-verified), Linux (intended, `docs/deployment.md`), Windows: none (`fork`, `poll`, `termios`, `/usr/bin/ssh`) | `src/process_manager.cpp`, `src/terminal_client.cpp:6`, `src/ssh_auth.cpp:26` |
| Security defaults | `StrictHostKeyChecking=no`; password passed on `sshpass -p` argv | `src/ssh_auth.cpp:28`, `src/ssh_auth.cpp:49` |
| Docs | 9 files, accurate on architecture; drift: `docs/testing.md` lists 2 test files (4 exist), `docs/security.md` allowlist omits `hostname`, README links are absolute local paths | `docs/`, `README.md` |
| Git | `PLAN.md` untracked; 8 commits; CMake targets still named `remote_command_*` | `git status`, `src/CMakeLists.txt` |

Module inventory (LOC from `wc -l`):

| Module | Files | LOC | Status | Role today |
|---|---|---|---|---|
| `ProcessManager` | `process_manager.{hpp,cpp}` | 69 + 801 | **Active core** | `execute()` (`:348`, local: 3 pipes + `poll` loop) and `executeRemote()` (`:550`, SSH fan-out: 2 pipes/worker + one `poll` loop) |
| `CommandExecutor` | `command_executor.{hpp,cpp}` | 66 + 345 | Active | parse → allowlist (`:17`) → resolve from `/bin`,`/usr/bin` (`:24`) → route local/remote (`clients.txt`, `:188`); shell-quotes remote argv |
| `ClientConfig` | `client_config.{hpp,cpp}` | 36 + 267 | Active | `user@host` and `ssh://user@host:port?identity=` parser; rejects `password=`; dedup |
| `ClientManager` | `client_manager.{hpp,cpp}` | 67 + 290 | Active | add/remove/status, connectivity probe (`echo connected`), in-memory passwords never persisted |
| `ssh_auth` | `ssh_auth.{hpp,cpp}` | 10 + 64 | Active | builds `ssh` argv; `sshpass` prefix; stderr → auth-failure classifier |
| `TerminalClient` | `terminal_client.{hpp,cpp}` | 45 + 441 | Active | REPL, colours, hidden password prompt (`termios`), runs each command on a thread and spin-waits (`:311`, `:355`, `:418`) |
| `Logger` | `logger.{hpp,cpp}` | 41 + 75 | Active | singleton, mutex, `[ts][level][pid][session][client][command]`, stdout or file |
| `SessionManager` | `session_manager.{hpp,cpp}` | 42 + 127 | **Dormant** | thread-per-session wrapper; not on the REPL path (`docs/architecture.md`) |
| `Pipe` | `ipc_engine.{h,cpp}` | 40 + 101 | **Dormant** | RAII pipe + non-blocking toggle; not used by `ProcessManager` (`docs/ipc_design.md`) |
| `main` | `main.cpp` | 20 | Active | starts logger + REPL |

### 1.3 Baseline decisions preserved or generalized

These are the design decisions from `docs/` and the code that the target architecture keeps
verbatim or generalises rather than replaces:

- **Explicit argv at process boundaries** — local/native stages are spawned
  from argv; SSH serializes argv for the target's remote shell. The fixed
  allowlist remains only in the legacy demo shell, while `run --policy`
  is an optional controller restriction.
- **Child hygiene** — `setpgid(0,0)` + `kill(-pgid)` on timeout; `_exit()` after `fork()`;
  `EINTR` retry on every syscall; drain-until-`EAGAIN` reads (already edge-trigger-ready);
  monotonic `steady_clock` deadlines (`docs/process_management.md`).
- **Concurrent draining of all workers from one event loop** — the `executeRemote()` loop is
  the seed of the reactor; it is refactored, not discarded (`docs/distributed_execution.md`).
- **Inventory format** — `user@host` / `ssh://…?identity=` with the "passwords never on disk"
  rule and its tests (`docs/authentication.md`, `tests/test_client_config.cpp`).
- **Structured log context** — `{pid, session, client, command}` on every line becomes the
  trace context of the streaming engine.
- **Normalised remote error classes** — `connection failed / unreachable / authentication
  failed / timed out / exit N` become the failure taxonomy of the orchestrator.
- **OpenSSH as the agentless transport** — inherits `~/.ssh/config`, agents, certificates,
  `ProxyJump`; we still do not embed an SSH library.

### 1.4 Principles (every design decision is tested against these)

1. **OS-native first** — every abstraction is a thin, documented mapping onto a real kernel
   primitive; no emulation layers that hide cost.
2. **Bounded everything** — memory, fds/handles, in-flight bytes, retries, time. Unbounded
   growth is a bug, not a tuning problem.
3. **Pipe semantics end-to-end** — EOF, half-close, exit status, and cancellation propagate
   across nodes exactly as they would across a local `|`.
4. **Secure by default, agentless by default** — host-key verification on, mTLS on, keys/agent
   only; the native agent is opt-in.
5. **One executable, no central orchestrator service** — controller and node
   are the same executable; native placement uses the optional node daemon,
   but no database or control-plane cluster is required.
6. **Portability without leaks** — `include/psx/` exposes only `std::` types; platform headers
   live in `src/os/{posix,win32}/` only.

**Non-goals (revised):** configuration management and templating; a built-in scheduler (use
`cron`/`systemd` timers/Task Scheduler); a central server or state store; replicated/HA
controllers (runs are idempotent and re-runnable instead); an embedded SSH implementation.

---

## 2. Historical Baseline vs. Target Architecture Gap Analysis

Section 2 records the 2026-08-22 migration analysis. Many “Today” cells have
since been implemented; use the release snapshot above and the linked current
docs for v0.6 behavior.

### 2.1 Gap matrix by systems concern

| Concern | Baseline (2026-08-22 evidence) | Gap / risk | Target |
|---|---|---|---|
| **Event demultiplexing** | `poll()` with the `pollfd` vector rebuilt every iteration (`process_manager.cpp:667-671`) | O(N) per wake-up; at 1 000 hosts each ready byte costs a 2 000-entry scan | `psx::Reactor` with `epoll` (ET), `kqueue` (`EV_CLEAR`), `IOCP`; `poll` kept as portable fallback |
| **Child-exit notification** | no-op `SIGCHLD` handler (`:171-179`) + `waitpid(WNOHANG)` on *every* worker *every* loop (`:740`) | O(N) syscalls per iteration → O(N²) per run; exit observed only via pipe EOF | pollable process handles: `pidfd` (Linux ≥ 5.3), `kqueue EVFILT_PROC/NOTE_EXIT` (macOS), Job Object completion port (Windows); `signalfd`/self-pipe for the rest |
| **Buffering / backpressure** | per-worker `std::string` grows without bound; reads never pause | a chatty host can exhaust controller memory; no fairness | bounded per-stream ring buffers; stop polling a stream whose buffer is full (pipe fills → remote `write(2)` blocks); credit windows across the backplane |
| **Streaming** | callbacks fire after completion (`command_executor.cpp` remote path; `docs/ipc_design.md` "not true live streaming") | no `tail -F`, no real-time use case | line-framed live streams with per-host tagging; `--stream` / `--group` / `--json` sinks |
| **Process spawn** | `fork()` + `execvp()`; pipes created without `O_CLOEXEC` (`:139`, `:579-582`); inherited fds closed by hand in each child | `fork` cost is O(parent page tables) — grows with buffered output; fd-inheritance races if a thread spawns concurrently | `posix_spawn` (+ `prlimit` on Linux) and `CreateProcessW` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`; all handles CLOEXEC / non-inheritable at creation |
| **Process groups / kill** | `setpgid` + `kill(-pgid, SIGKILL)` | no graceful stage (TERM→KILL); no Windows equivalent | `Process::signal(Graceful\|Kill)` → `SIGTERM` then `SIGKILL`; Windows: `CTRL_BREAK` then `TerminateJobObject` |
| **Resource limits** | `RLIMIT_CPU=5s`, `RLIMIT_AS=64 MiB` hardcoded on local children (`:379-382`); none on SSH workers; `RLIMIT_AS` not reliably enforced on Darwin | breaks legitimate commands; not configurable (`docs/process_management.md` "Remaining Gaps") | per-stage `limits{cpu,mem,fds,wall}` → `prlimit`/cgroup v2 (Linux), Job Object limits (Windows), advisory on macOS |
| **fd / handle budget** | never raises `RLIMIT_NOFILE` (macOS soft default 256 → ~120 hosts max) | silent `EMFILE` at moderate fan-out | raise soft→hard at startup; sliding-window scheduler sized from the limit; handle accounting in metrics |
| **Timeouts / cancellation** | single global deadline per `executeRemote` call; no Ctrl-C handling (`docs/process_management.md`) | one slow host extends the run; Ctrl-C leaks children | per-stage deadline, global deadline, cancellation token; 1st Ctrl-C graceful, 2nd hard |
| **Threads** | REPL spawns a thread per command and spin-waits 100 ms (`terminal_client.cpp:311,355,418`); `SessionManager` thread-per-session | pointless latency, non-deterministic interleaving with logger | single-threaded reactor; optional bounded worker pool for CPU work (TLS, hashing) only |
| **Transport** | one `ssh` process per host; `/usr/bin/ssh` hardcoded (`ssh_auth.cpp:26`); `StrictHostKeyChecking=no` (`:28`); `sshpass -p` on argv (`:49`) | ~5–8 MB RSS per `ssh` ⇒ 1 000 hosts ≈ 5–8 GB; MITM-open; password visible in `ps` | `SshTransport` (PATH lookup, `accept-new` + per-inventory `known_hosts`, `ControlMaster` opt-in, `sshpass -d <fd>`/`SSH_ASKPASS`) **and** `NativeTransport` (1 TCP+TLS connection per node, multiplexed) |
| **Cross-node pipes** | none; only stdout/stderr collection | cannot express `A@host1 \| B@host2` | DAG of stages; edges = `Stream` (pipe locally, backplane stream remotely) with identical EOF/half-close semantics |
| **Fault tolerance** | errors classified post-hoc; no retry; no lease | controller crash leaves nothing orphaned via SSH (`sshd` HUPs), but the native path needs explicit fencing | retries with jittered backoff for idempotent stages; heartbeats + leases (node kills job when lease expires); partial-result contract |
| **Security model** | command allowlist + trusted dirs (`command_executor.cpp:17-24`); no sandbox (`docs/security.md`) | allowlist is right for a kiosk, wrong for an operator tool | allowlist → optional `--policy` file; stage sandbox opt-in (seccomp/Landlock, restricted tokens); mTLS identities; audit log |
| **Secrets in memory** | passwords in plain `std::string` | swapped/core-dumped/copied freely | `SecureString`: `mlock`/`VirtualLock`, `explicit_bzero`/`SecureZeroMemory`, `MADV_DONTDUMP`/`PR_SET_DUMPABLE=0` |
| **Public API hygiene** | `pid_t`, `ssize_t`, `<signal.h>`, `<sys/types.h>` in `include/` (`process_manager.hpp:6-7,52`, `logger.hpp:7,12`, `session_manager.hpp:13`, `ipc_engine.h:28-29`) | Windows port would ripple through every header | `include/psx/**` uses only `<cstdint>`/`std::`; CI lint rejects platform includes outside `src/os/` |
| **Config / inventory** | `clients.txt` in CWD only | no groups, tags, per-host options, no XDG/`%APPDATA%` | `Inventory` (INI, groups/tags, per-host user/port/identity/transport); `clients.txt` import kept |
| **Logging / audit** | stdout by default, no rotation (`docs/deployment.md`) | spams the terminal; no audit trail | file/JSON logs with levels; append-only JSONL audit log; OpenTelemetry-style trace ids from `LogContext` |
| **Build / CI / tests** | fresh build broken; GTest optional; no CI; no sanitizers; no integration rig | regressions invisible | CI matrix (Linux/macOS/Windows × gcc/clang/msvc), GTest via `FetchContent`, ASan/UBSan/TSan, dockerised `sshd` fleet, fuzzers |

### 2.2 Module fate map (current → target)

| Current | Fate | Target location |
|---|---|---|
| `ProcessManager::execute` (local, 3 pipes, `poll`) | **Refactor** into `os::Process` + `runtime::Reactor` + `stream::Stream`; behaviour preserved by golden tests | `src/os/posix/process.cpp`, `src/runtime/` |
| `ProcessManager::executeRemote` (SSH fan-out) | **Refactor** into `transport::SshTransport` (spawns `ssh` as a `Process`, exposes stdout/stderr as `Stream`s); scheduler moves to orchestrator | `src/transport/ssh/` |
| `Pipe` (`ipc_engine.h`) | **Absorbed** — becomes `os::Pipe` (the second IPC abstraction noted in `docs/architecture.md` disappears) | `include/psx/os/pipe.hpp` |
| `SessionManager` (threads) | **Replaced** by `pipeline::Run` (a run = one reactor-driven state machine; no thread per session) | `src/pipeline/run.cpp` |
| `CommandExecutor` (allowlist, parse, route) | **Split**: parser → `cli/`; allowlist → `policy/` (optional); routing → `pipeline::Planner` | `src/cli/`, `src/policy/`, `src/pipeline/` |
| `ClientConfig` / `ClientManager` | **Generalise** to `Inventory` (groups, tags, transport per host); `clients.txt` remains an importable format; connectivity probe becomes `pipeshellx ping` | `src/inventory/` |
| `ssh_auth` | **Harden** → `SshCommandBuilder`: PATH lookup, `accept-new`, per-inventory `known_hosts`, `BatchMode`, `ControlMaster` opt-in, password via fd | `src/transport/ssh/command_builder.cpp` |
| `TerminalClient` | **Thin** REPL over the same `Run` API as one-shot mode; spin-wait threads deleted; console abstraction replaces `termios` | `src/cli/repl.cpp`, `src/os/*/console.cpp` |
| `Logger` | **Keep** format; add JSON mode, file rotation, levels; separate audit sink | `src/observability/` |
| `main` | `pipeshellx` multi-command entry: `run`, `pipe`, `ping`, `hosts`, `shell`, `node`, `ca` | `src/main.cpp` |

### 2.3 `docs/` mapping — every roadmap item extends an existing document

| Existing doc | Extended by plan section | Planned evolution |
|---|---|---|
| `docs/architecture.md` | §3.1–3.2 | layered L0–L5 model; domain model (Pipeline/Stage/Stream/Node/Run) |
| `docs/system_flow.md` | §3.7, §5 | pipeline run flow (plan → place → spawn → stream → reap → summarise), cancellation flow |
| `docs/ipc_design.md` | §3.3, §3.5, §4.4 | `os::Pipe`, CLOEXEC rules, bounded buffers, credit windows, Windows overlapped named pipes |
| `docs/process_management.md` | §3.3, §3.8, §4.3 | `os::Process`, `posix_spawn`/`CreateProcessW`, pidfd/kqueue/Job Objects, graceful→kill, limits |
| `docs/distributed_execution.md` | §3.6, §3.7 | transports, wire protocol, multiplexing, leases, scheduler, failure taxonomy |
| `docs/authentication.md` | §3.6, §3.8 | SSH hardening, password-via-fd, mTLS identities, offline CA, enrollment over SSH |
| `docs/security.md` | §3.8, §4 | threat model v2 (controller, node, wire), sandbox tiers per OS, secrets handling |
| `docs/deployment.md` | §4.5–4.7, §6 Phase 7 | service units (systemd/launchd/SCM), packaging matrix, static linking, air-gapped install |
| `docs/testing.md` | §6 (every phase), §7 | test pyramid, fault injection, sanitizers, integration fleet, benchmark harness |
| *new* `docs/os_abstraction.md` | §4 | the POSIX↔Win32 primitive mapping table as a living document |
| *new* `docs/wire_protocol.md` | §3.6 | frame formats, flow control, versioning |
| *new* `docs/pipelines.md` | §3.2, §5 | stage placement syntax, `pipeline.yaml`, sinks |
| *new* `docs/benchmarks.md` | §7 | targets, harness, baseline numbers per release |

---

## 3. System Architecture & OS/Distributed Primitive Abstractions

### 3.1 Layered architecture

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│ L5  Interfaces        pipeshellx run | pipe | ping | hosts | shell | node | ca │
│                       --stream/--group/--json renderers, REPL, man page        │
├──────────────────────────────────────────────────────────────────────────────┤
│ L4  Orchestration     Planner (DAG + placement) · Scheduler (sliding window)   │
│                       Run/Stage state machines · deadlines · retries · leases  │
│                       Sinks: merge · group · consensus · json · spool          │
├──────────────────────────────────────────────────────────────────────────────┤
│ L3  Transports        SshTransport (agentless, system OpenSSH)                 │
│                       NativeTransport (psx/1 frames over mTLS/TCP, multiplexed)│
│                       LocalTransport (same host: pipes only)                   │
├──────────────────────────────────────────────────────────────────────────────┤
│ L2  Streams           Stream · BoundedBuffer · CreditWindow · LineFramer       │
│                       half-close/EOF propagation · fairness (round-robin)      │
├──────────────────────────────────────────────────────────────────────────────┤
│ L1  Runtime           Reactor{epoll|kqueue|poll|iocp} · Timer wheel            │
│                       ChildExitSource{pidfd|kqueue-proc|job-iocp|sigchld}      │
│                       SignalSource{signalfd|kqueue|self-pipe|ConsoleCtrl}      │
├──────────────────────────────────────────────────────────────────────────────┤
│ L0  psx::os           Handle · Pipe · Process · Socket · Tls · Console · Fs    │
│                       src/os/posix/  (linux.cpp, darwin.cpp)  src/os/win32/   │
└──────────────────────────────────────────────────────────────────────────────┘
Dependency rule: a layer may include only the layer directly below it (checked in CI).
```

Target tree (additive; existing files move in Phase 1–2, nothing is deleted before its
replacement is green):

```text
include/psx/{os,runtime,stream,transport,pipeline,inventory,observability}/*.hpp
src/os/posix/  src/os/win32/  src/runtime/  src/stream/  src/transport/{ssh,native,local}/
src/pipeline/  src/inventory/  src/policy/  src/observability/  src/cli/  src/node/
tests/unit/  tests/integration/  tests/fuzz/  bench/
```

### 3.2 Domain model

| Entity | Definition | Today's equivalent |
|---|---|---|
| **Node** | a machine reachable via a transport; identity = inventory name (+ mTLS SAN in native mode) | `ClientEntry` |
| **Stage** | one process: `{argv, env, cwd, limits, placement, idempotent: bool}` | the `ssh … 'cmd'` worker / local child |
| **Stream** | a unidirectional byte channel with EOF and half-close; local = pipe, remote = backplane stream id or SSH channel via pipe | `stdoutPipe[0]` etc. |
| **Pipeline** | a DAG of stages connected by streams; a plain fan-out is the degenerate DAG *N parallel stages → one merge sink* | `executeRemote()` |
| **Run** | one execution of a pipeline: `run_id`, deadline, cancellation token, audit record, summary | one REPL command |
| **Sink** | a terminal stage implemented in-process: `merge`, `group`, `consensus`, `json`, `spool`, `file` | `formatClientResults()` |

The existing fan-out CLI and the consensus feature are therefore *not* special cases: `run` is
`pipe` with a merge sink; `diff`/`--consensus` is `pipe` with a consensus reduce sink.

### 3.3 L0 — OS primitive abstractions (`psx::os`)

All five primitives are move-only RAII types whose public surface uses `std::` types only.
`native()` accessors exist but are callable only from `src/os/**` (enforced by a `friend`
backend tag and a CI grep).

| Primitive | Contract | Linux | macOS | Windows |
|---|---|---|---|---|
| `Handle` | owns one kernel object; closed exactly once; **non-inheritable at creation** | `int` + `O_CLOEXEC` | `int` + `FD_CLOEXEC` (no `pipe2`; set immediately) | `HANDLE`, `bInheritHandle=FALSE` |
| `Pipe` | `{reader, writer}`; non-blocking or overlapped; capacity query/hint | `pipe2(O_CLOEXEC\|O_NONBLOCK)`, `F_SETPIPE_SZ` | `pipe`+`fcntl`; 16→64 KiB buffer | `CreateNamedPipe(FILE_FLAG_OVERLAPPED)` with a unique `\\.\pipe\psx-<pid>-<n>` name — anonymous `CreatePipe` cannot do overlapped I/O |
| `NamedPipe` | rendezvous endpoint for local IPC with other processes | `AF_UNIX` socket (preferred) / `mkfifo` | same | `CreateNamedPipe` (message or byte mode) / `AF_UNIX` (1803+) |
| `Process` | spawn with explicit stdio handles, pgroup/job, limits; pollable exit; `signal(Graceful\|Kill)` | `posix_spawn` (glibc ≥ 2.24 uses `CLONE_VFORK`) + `posix_spawn_file_actions`, `POSIX_SPAWN_SETPGROUP`, `SETSIGMASK/SETSIGDEF`; `prlimit()` after spawn; exit via `pidfd_open` (5.3+) else `SIGCHLD` | `posix_spawn` (true syscall, fast); exit via `kqueue EVFILT_PROC NOTE_EXIT`; `fork` fallback only when a child-side hook is unavoidable | `CreateProcessW` + `STARTUPINFOEXW` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (inherit *only* the 3 stdio handles); `CREATE_NEW_PROCESS_GROUP`; `AssignProcessToJobObject`; exit via job→IOCP `JOB_OBJECT_MSG_EXIT_PROCESS` |
| `ProcessGroup` | kill-all semantics for a stage and its descendants | `setpgid` + `kill(-pgid)`; optional cgroup v2 leaf for hard guarantees | `setpgid` + `kill(-pgid)` | Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (descendants included; nested jobs on Win8+) |
| `Socket` | non-blocking TCP/Unix stream; `TCP_NODELAY`, keepalive, user-timeout | `SOCK_NONBLOCK\|SOCK_CLOEXEC`, `TCP_USER_TIMEOUT` | `fcntl`, `SO_KEEPALIVE` (`TCP_KEEPALIVE` idle) | `WSASocketW(WSA_FLAG_OVERLAPPED)`, `SIO_KEEPALIVE_VALS` |
| `Tls` | mTLS 1.3 session over a `Socket`; peer identity = SAN URI | OpenSSL 3 (static) | OpenSSL 3 (static) | OpenSSL 3 (static); SChannel evaluated as an alternative in Phase 4 spike |
| `Console` | raw/echo toggle, VT support, width, is-a-tty | `termios`, `ioctl(TIOCGWINSZ)` | `termios` | `SetConsoleMode` (`ENABLE_VIRTUAL_TERMINAL_PROCESSING`, `ENABLE_ECHO_INPUT`), `SetConsoleOutputCP(CP_UTF8)` |
| `SignalSource` | delivers `Interrupt`, `Terminate`, `Hangup`, `ChildExit`, `WindowResize` as reactor events | `signalfd` (+ blocked signal mask) | `kqueue EVFILT_SIGNAL` | `SetConsoleCtrlHandler` → posts a completion packet to the IOCP |

Invariants (tested in `tests/unit/os/`):
- No handle is ever inheritable except the three passed explicitly to a `Process` at spawn.
- `Handle` count before and after 10 000 spawn/exit cycles is identical (leak test).
- `SIGPIPE` is ignored process-wide; `EPIPE`/`ERROR_NO_DATA` are ordinary stream errors.
- The child never executes C++ code between spawn and `exec` (no `fork()` on the hot path).

### 3.4 L1 — Runtime

**Reactor API shape — completion-oriented.** `epoll`/`kqueue` are *readiness* demultiplexers;
IOCP is a *completion* port. The unifying API is completion-style (`stream.read(buf) → event`),
implemented on POSIX as readiness + immediate non-blocking `read` (the loop already does
drain-until-`EAGAIN`, which is exactly what edge-triggered `EPOLLET`/`EV_CLEAR` requires), and
on Windows as overlapped `ReadFile`/`WSARecv` + `GetQueuedCompletionStatusEx`. This is the same
decision libuv, ASIO, and .NET made, and it avoids the zero-byte-read hacks that readiness
emulation on Windows needs (which do not work for pipes at all).

```text
Reactor backends           registration         wake source                 notes
epoll   (Linux)            EPOLL_CTL_ADD, ET     epoll_wait                  O(ready) per wake
kqueue  (macOS/BSD)        EV_ADD|EV_CLEAR       kevent                      procs+signals+timers in same queue
poll    (portable)         rebuilt pollfd        poll                        today's loop; fallback only
iocp    (Windows)          CreateIoCompletionPort GetQueuedCompletionStatusEx job/console packets via PostQueuedCompletionStatus
```

- **Single-threaded by default.** One reactor thread owns all handles; CPU-heavy work (TLS
  records, SHA-256 for consensus, JSON encode) goes to a bounded worker pool that communicates
  back through a wake-up handle (`eventfd` / `kqueue EVFILT_USER` / `PostQueuedCompletionStatus`).
  This removes the thread-per-command and spin-wait in `TerminalClient` and the thread-per-session
  in `SessionManager`.
- **Timers:** hierarchical timing wheel keyed on `steady_clock` (deadlines, heartbeats, backoff).
- **Child exit:** `ChildExitSource` hides `pidfd` / `EVFILT_PROC` / job-IOCP / `SIGCHLD`+`waitpid`
  behind one event; `waitpid` is called exactly once per child, on notification, never polled.
- **`RLIMIT_NOFILE`** raised to the hard limit at startup; the scheduler's window is derived
  from `(limit − reserved) / handles_per_stage`.

### 3.5 L2 — Streams and flow control

A `Stream` is a byte channel with the pipe state machine: `Open → HalfClosed(local|remote) →
Closed`, plus an error terminal state. Every stream has a **bounded buffer** (default 256 KiB) and
a **credit window**.

Backpressure works at three boundaries using the kernel's own mechanisms:

```text
remote process ──write(2)──▶ pipe (64 KiB) ──read──▶ node/ssh ──DATA frames (credits)──▶ controller buffer (256 KiB) ──▶ sink
     blocks when full         kernel            stops reading when           sender may not exceed         stops reading
                                                 no credits                   advertised window            when sink is slow
```

- **Local (pipe) boundary:** when a stream's buffer is full, the reactor *deregisters interest*
  in that handle; the pipe fills; the producer's `write(2)` blocks. No bytes are dropped, no
  memory grows. This single change fixes the unbounded `std::string` growth in `executeRemote()`.
- **Backplane boundary target:** per-stream and per-connection credit windows
  (HTTP/2- and SSH-channel-style). Current psx/1 implements the 256 KiB
  per-stream window and sends `WINDOW_UPDATE` as bytes are consumed; the
  separate connection-wide window and a fixed 64 KiB `DATA` frame cap remain
  future work.
- **Fairness:** a merge sink distributes credits round-robin across producers so one chatty host
  cannot starve others (head-of-line protection).
- **Policies** (per sink, chosen by the use case): `block` (default, lossless), `drop-oldest` /
  `drop-newest` with a per-producer ring (log tailing where liveness > completeness; drops are
  counted and reported), `spool` (overflow to a per-stream temp file with `O_TMPFILE`/
  `FILE_ATTRIBUTE_TEMPORARY`).
- **Line framing:** `LineFramer` emits complete lines only (configurable max line length, CRLF
  normalisation); partial lines are flushed at EOF with a marker. Guarantees no interleaved
  partial lines in `--stream` output — a property test, not an aspiration.
- **Current v0.6 copy path:** local edges use bounded userspace buffers and
  native edges use credit-controlled TLS. A Linux `splice()` fast path
  remains an unchecked Phase 5 item.

### 3.6 L3 — Transports

#### `SshTransport` (agentless)

- `ssh` is found on `PATH`; v0.6 does not hard-code a
  platform path.
- Defaults are `StrictHostKeyChecking=accept-new`, a per-inventory
  `UserKnownHostsFile`, `BatchMode=yes` when no legacy
  password is used, `ConnectTimeout=5`, and
  `ServerAliveInterval=15`. There is no `--insecure` flag.
- `--reuse` enables OpenSSH control-socket reuse for repeated runs.
- The legacy interactive password path uses `sshpass -d <fd>`; no
  password is placed on argv or written to inventory.
- The controller starts one managed SSH process per in-flight host and bounds
  them with `-c`. OpenSSH and the target login shell own remote
  authentication/interpretation.

#### `NativeTransport` — the `psx/1` backplane

One TCP+mTLS connection per target carries multiplexed stage streams. The same
binary serves as the node agent:

```bash
pipeshellx node --listen 0.0.0.0:7433 \
  --cert node.crt --key node.key --ca ca/ca.crt \
  --allow spiffe://psx/controller/ops --policy node.policy
```

- **Normative protocol:** [`docs/wire_protocol.md`](docs/wire_protocol.md)
  defines the actual 10-byte psx/1 envelope, frame types, channel flags,
  versioned `OPEN` payload, credit, liveness, fencing, and
  compatibility rules. It supersedes earlier frame sketches.
- **Identity and authorization:** both peers present CA-signed TLS 1.3
  certificates; inventory can pin a node SAN URI, `node --allow` can
  restrict controller SAN URIs, and optional CRLs revoke identities.
- **Command authorization:** independent `run --policy` and
  `node --policy` gates exist. Node rejection occurs before spawn,
  writes a diagnostic to stage stderr, and exits 126. Without node policy, an
  admitted controller may request arbitrary argv.
- **Enrollment:** `node keygen` creates a local key/CSR and
  `ca sign` issues the certificate. Automated SSH copy/install is not
  implemented.
- **Liveness and fencing:** PING/PONG leases make a lost/silent connection
  terminal and node teardown kills its owned stages.
- **No resume:** reconnect-and-resume and native pipeline retry are not
  implemented in v0.6.
- **Local control:** a POSIX `AF_UNIX` endpoint exposes the node status
  snapshot used by `node status --control`. Win32 named-pipe support
  is deferred with the Windows port.

### 3.7 Target L4 — Orchestration, lifecycle, and fault tolerance

This subsection is the target model. The v0.6 product contract is narrower:
`run` has a concurrency window, timeout, cancellation, fail-fast,
explicitly idempotent SSH retries, and native canaries; native
reconnect/resume, duplicate-spawn suppression, per-node circuit breakers,
deadline metadata in `OPEN`, and Windows mappings are not
implemented. Current exit codes are documented in
`docs/distributed_execution.md` and `docs/pipelines.md`.

**Stage state machine** (replaces `RemoteWorkerState` booleans):

```text
Pending ─▶ Spawning ─▶ Running ─▶ Draining ─▶ Exited(code)
   │           │           │          │
   │           ▼           ▼          ▼
   └─▶ Skipped  SpawnFailed  Killed(timeout|cancel)  Lost(node|lease)
                      └──────────── retry? (idempotent ∧ attempts < max) ──▶ Pending
```

- **Scheduler:** sliding window of concurrently active stages (`-c`, default 64; bounded by the
  handle budget); per-stage and per-run deadlines on the timing wheel; jittered exponential
  backoff (`base 500 ms, ×2, cap 30 s, ±20 %`); per-node circuit breaker after `k` consecutive
  failures.
- **Cancellation:** one `CancellationToken` per run; first `Interrupt` → `Graceful` to every
  stage and the summary still prints; second → `Kill`; Windows maps `CTRL_C`/`CTRL_BREAK`/
  `CTRL_CLOSE` to the same token.
- **Deadline propagation:** the remaining run deadline is sent in `OPEN` so a node never runs a
  stage longer than the controller will wait (gRPC-style).
- **Idempotency:** `(run_id, stage_id, attempt)` is the spawn key; nodes reject duplicates
  within the lease window — at-most-once spawn, at-least-once reporting; exactly-once is not
  promised and the docs say so.
- **Failure taxonomy** (the existing classes, promoted to an enum with an exit-code mapping):
  `Unreachable`, `ConnectionFailed`, `AuthFailed`, `HostKeyChanged`, `SpawnFailed`, `TimedOut`,
  `Killed`, `Lost`, `ExitNonZero(n)`, `PolicyDenied`.
- **Exit-code contract:** `0` all stages succeeded · `1` some stage failed · `2` usage/config ·
  `3` connect/auth/host-key · `130` cancelled by the operator. Identical on Windows.
- **Partial results are first-class:** every sink receives per-stage status; `--json` emits one
  object per stage plus a run summary; `--fail-fast` cancels remaining stages on first failure;
  `--canary N%` runs a subset first and stops if any fails.
- **What we deliberately do not build:** a replicated controller or consensus protocol. A run
  has exactly one coordinator; availability comes from idempotent re-runs, not Raft. This is the
  "no Kubernetes" promise made concrete.
- **Consistency vocabulary for the docs:** streams are FIFO per `(stage, channel)`; cross-stage
  merge order is arrival order (non-deterministic); `--ordered` buffers within a window and emits
  by `(node, seq)`; timestamps are sender-monotonic plus wall-clock for log use.

### 3.8 Target isolation and security model

| Tier | Mechanism | Linux | macOS | Windows |
|---|---|---|---|---|
| Address-space isolation (always) | each stage is its own process; controller buffers are bounded | `posix_spawn`/`exec` | same | `CreateProcessW` |
| Resource caps (`limits{}` per stage) | CPU, memory, fds, wall time | `prlimit` (`RLIMIT_CPU/AS/NOFILE`) + cgroup v2 leaf (`memory.max`, `pids.max`) when available | `prlimit`-equivalent via `setrlimit` in a `fork` fallback; **memory cap advisory only** (Darwin does not reliably enforce `RLIMIT_AS`) — documented | Job Object `PROCESS_MEMORY`, `JOB_TIME`, `ACTIVE_PROCESS` limits |
| Sandbox (opt-in `--sandbox`) | syscall/FS restriction for untrusted stages | seccomp-bpf allow-list + Landlock (5.13+) path rules; `PR_SET_NO_NEW_PRIVS` | `sandbox_init` profile (deprecated API but functional) | restricted token (`CreateRestrictedToken`) + AppContainer profile |
| Policy (replaces the allowlist) | `--policy file`: allowed argv[0] patterns, trusted dirs, max args, env scrubbing | — | — | — |
| Secrets in memory | `SecureString` | `mlock`, `explicit_bzero`, `madvise(MADV_DONTDUMP)`, `prctl(PR_SET_DUMPABLE,0)` | `mlock`, `memset_s` | `VirtualLock`, `SecureZeroMemory`, `CryptProtectMemory` |
| Wire | mTLS 1.3 only; no plaintext mode; cipher suites fixed to AEAD | OpenSSL 3 | OpenSSL 3 | OpenSSL 3 / SChannel |
| Audit | append-only JSONL `{ts, run_id, user, node, stage, argv, exit, duration, bytes}`; `--no-audit` opt-out | `~/.local/state/pipeshellx/audit.jsonl` | `~/Library/Application Support/…` | `%LOCALAPPDATA%\PipeShellX\audit.jsonl` |

**v0.6 boundary:** address-space isolation, POSIX process groups, bounded
capture/credit, native mTLS, SAN authorization/CRL, and opt-in unsigned JSONL
audit exist. `run --policy` and independent
`node --policy` use exact command names, an argv-count cap, and a
metacharacter guard; the node rejects before spawn with stage exit 126. Policy
does not scrub the environment or provide a sandbox, and it is optional at
both boundaries. Sandboxes,
`SecureString`, configurable limits across all stages, signed/default
audit, privilege separation, and every Windows column in the target table are
not implemented.

Target threat model v2 (extends `docs/security.md`): (1) operator workstation compromise → bounded by
audit + per-fleet CA scoping; (2) hostile node → cannot open streams to the controller
(odd/even id rule + capability gating), cannot exceed windows; (3) network attacker → mTLS,
host-key pinning in SSH mode, no downgrade; (4) hostile stage → sandbox tiers + limits;
(5) controller loss → leases guarantee no orphaned work.

### 3.9 Target observability

In v0.6, contextual application logs, run/stage identifiers, opt-in unsigned
JSONL run audit, and the node's local control-socket counters exist. JSON log
mode, rotation/retention guarantees, Prometheus endpoints, default audit,
signed audit, and the full metric list below remain target work.

- Logs: existing `LogContext` format kept; add JSON lines, levels, rotation, and `run_id`/
  `stage_id` fields (the trace context). Default sink is a file; `--verbose` mirrors to stderr.
- Metrics (`--metrics-file` or `pipeshellx node --metrics :9433` in Prometheus text format):
  active stages, handles, bytes in/out per transport, window stalls, drops, retries, heartbeat
  RTT, spawn latency histogram.
- Architecture decision records kept under `docs/adr/` — seeded with: ADR-001 completion-style
  runtime API; ADR-002 system OpenSSH as agentless transport; ADR-003 single coordinator, no
  consensus protocol; ADR-004 OpenSSL 3 static for TLS; ADR-005 `psx::Result<T>` (expected-style)
  in L0–L2, exceptions permitted from L4 up.

---

## 4. Cross-Platform Compatibility Layer (Linux, macOS, Windows)

### 4.1 Rules and how they are enforced

1. Platform headers (`<unistd.h>`, `<sys/*.h>`, `<signal.h>`, `<poll.h>`, `<termios.h>`,
   `<windows.h>`, `<winsock2.h>`, …) may be included **only** under `src/os/posix/` and
   `src/os/win32/`. A CI step greps `include/` and the rest of `src/` and fails on any hit.
2. `#ifdef _WIN32` / `__APPLE__` / `__linux__` appear **only** in `src/os/` and
   `CMakeLists.txt`. Everything above L0 is platform-agnostic by construction.
3. Public types: `psx::os::NativeHandle = std::intptr_t` (fits `int` and `HANDLE`);
   `ProcessId = std::int64_t`; `ExitStatus` is a tagged type `{Exited(code) | Signaled(sig) |
   Terminated(win32 code)}` — no `WIFEXITED` leaks.
4. Build per platform selects exactly one backend source set; the shared test suite in
   `tests/unit/os/` runs unchanged on all three.
5. `-Wall -Wextra -Werror` (gcc/clang) and `/W4 /WX` (MSVC) everywhere; `clang-tidy` with
   `misc-include-cleaner` and `portability-*` checks.

### 4.2 Primitive mapping (living copy in `docs/os_abstraction.md`)

| Need | POSIX (Linux / macOS) | Windows | Abstraction |
|---|---|---|---|
| Anonymous pipe for a child's stdio | `pipe2(O_CLOEXEC)` / `pipe`+`fcntl` | overlapped `CreateNamedPipe` + `CreateFile` pair (anonymous pipes are synchronous-only) | `os::Pipe::create()` |
| Named pipe / local IPC | `AF_UNIX` stream socket, `mkfifo` | `CreateNamedPipe(PIPE_TYPE_BYTE)`, `AF_UNIX` (1803+) | `os::NamedPipe` |
| Make handle non-blocking | `O_NONBLOCK` | overlapped I/O (no concept of non-blocking handles) | hidden inside `Reactor` backend |
| Demultiplex | `epoll` (ET) / `kqueue` (`EV_CLEAR`) / `poll` | IOCP (`GetQueuedCompletionStatusEx`) | `runtime::Reactor` |
| Wake the loop from another thread | `eventfd` / `kqueue EVFILT_USER` / self-pipe | `PostQueuedCompletionStatus` | `Reactor::wake()` |
| Spawn with stdio redirection | `posix_spawn_file_actions_adddup2/addclose` | `STARTUPINFOEXW.hStdInput/Output/Error` + `HANDLE_LIST` | `os::Process::spawn(spec)` |
| Prevent fd inheritance | `O_CLOEXEC` on every fd | `bInheritHandle=FALSE` + explicit handle list | creation-time invariant |
| Process group | `setpgid(0,0)`; kill with `kill(-pgid)` | `CREATE_NEW_PROCESS_GROUP` + Job Object | `os::ProcessGroup` |
| Graceful stop | `SIGTERM` | `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pgid)` (console apps) / `WM_CLOSE` (GUI) | `Process::signal(Graceful)` |
| Hard kill incl. descendants | `kill(-pgid, SIGKILL)` (+ cgroup `kill` file on Linux ≥ 5.14) | `TerminateJobObject` | `Process::signal(Kill)` |
| Wait for exit without polling | `pidfd_open`+`poll` / `kqueue EVFILT_PROC` / `SIGCHLD`+`waitpid` | job object completion port / `RegisterWaitForSingleObject` | `runtime::ChildExitSource` |
| Exit status | `WIFEXITED/WEXITSTATUS/WTERMSIG` | `GetExitCodeProcess` (DWORD; `STATUS_CONTROL_C_EXIT` = 0xC000013A) | `os::ExitStatus` |
| Resource limits | `setrlimit`/`prlimit`, cgroup v2 | Job Object limits | `Process::Limits` |
| Operator interrupt | `SIGINT`/`SIGTERM`/`SIGHUP` via `signalfd`/`kqueue` | `SetConsoleCtrlHandler` | `runtime::SignalSource` |
| Ignore broken pipe | `signal(SIGPIPE, SIG_IGN)` | n/a (`ERROR_NO_DATA`/`ERROR_BROKEN_PIPE` returned) | `Stream` error mapping |
| Raise fd/handle limit | `setrlimit(RLIMIT_NOFILE)` (macOS: also `OPEN_MAX`/`kern.maxfilesperproc`) | none needed (handles ≫ fds); avoid `WaitForMultipleObjects` 64-object limit by using IOCP | `os::raise_handle_limit()` |
| Hidden password prompt | `tcsetattr(~ECHO)` | `SetConsoleMode(~ENABLE_ECHO_INPUT)` | `os::Console::read_secret()` |
| Colour / VT | assume VT on tty | `ENABLE_VIRTUAL_TERMINAL_PROCESSING` (Win10 1511+), UTF-8 code page | `os::Console` |
| Executable lookup | `access(X_OK)` over `PATH` | `SearchPathW` + `PATHEXT` | `os::find_executable()` |
| Zero-copy pipe↔socket | `splice` (Linux only) | none (`TransmitFile` is file→socket only) | optional fast path |
| Config dirs | XDG (`~/.config`, `~/.local/state`) / `~/Library/…` | `%APPDATA%`, `%LOCALAPPDATA%` | `os::paths()` |
| Service install | systemd unit / launchd plist | SCM service (`StartServiceCtrlDispatcher`) | `pipeshellx node install` |
| SSH client | `ssh` on PATH | `ssh.exe` (Win32-OpenSSH, inbox since Win10 1809) | `SshTransport` |

### 4.3 Process-model differences the orchestrator must know about

- Windows has no signals: `Graceful` is a console control event that only console processes
  honour, so the node falls back to `Kill` after the grace period regardless of platform.
- Windows exit codes are 32-bit; a killed job reports `Terminated(0xC000013A)`, which the
  summary renders as `killed` on every platform.
- Windows process creation is ~10× slower than `posix_spawn` (≈ 5–15 ms); the scheduler's
  default window on a Windows controller is therefore tuned by measured spawn latency, not a
  constant.
- Descendant tracking: POSIX process groups can be escaped by `setsid`; Job Objects cannot be
  escaped without `CREATE_BREAKAWAY_FROM_JOB`. Linux gets equivalent strength only with a cgroup
  leaf — offered when cgroup v2 is writable.

### 4.4 I/O-model differences

- Anonymous pipes on Windows are synchronous — the `Pipe` backend creates overlapped named
  pipes with unguessable names and a `PIPE_REJECT_REMOTE_CLIENTS | FIRST_PIPE_INSTANCE` ACL.
- Readiness vs completion (§3.4): the public `Stream` API is completion-style; POSIX backends
  issue the `read` immediately on readiness; Windows backends submit overlapped reads into the
  stream's buffer. Buffer ownership rules are identical (the buffer is pinned until completion).
- Pipe capacity differs (Linux 64 KiB adjustable, macOS 16→64 KiB, Windows configurable at
  creation); the credit window is chosen ≥ 4× the largest pipe buffer so the kernel pipe never
  becomes the throughput bottleneck.

### 4.5 Console, paths, text

- Line endings: stages on Windows may emit CRLF; `LineFramer` normalises on request (`--crlf
  keep|lf`). Output to a Windows console is UTF-8 with VT enabled; redirected output is bytes.
- Argument quoting: remote commands for Windows targets are quoted with the `CommandLineToArgvW`
  rules, not POSIX single quotes — `SshCommandBuilder` and `OPEN` carry argv arrays, and quoting
  is applied by the *executing* side according to its own rules.
- Paths in inventory/config use `std::filesystem` and accept both separators.

### 4.6 Build & toolchain matrix

| Target | Compiler | Link | Artifact |
|---|---|---|---|
| Linux x86_64 / aarch64 | gcc 13, clang 17 | static (musl via zig/`x86_64-linux-musl`), glibc dynamic for `.deb`/`.rpm` | `pipeshellx-linux-{amd64,arm64}` |
| macOS 12+ | Apple clang 15+ | static libs, universal (arm64 + x86_64) | `pipeshellx-darwin-universal` |
| Windows 10 1809+ / Server 2019+ | MSVC 19.3x (`/std:c++20`), clang-cl | static CRT (`/MT`), OpenSSL static | `pipeshellx-windows-{amd64,arm64}.exe` |

CMake: `PSX_BACKEND` chosen automatically; `GTest` and OpenSSL via `FetchContent`/vendored
tarballs so an air-gapped build needs no network; `-DPSX_NATIVE_TRANSPORT=OFF` produces a
smaller SSH-only binary.

### 4.7 Windows support tiers

| Tier | Capability | Phase |
|---|---|---|
| T1 | Windows as **SSH target** (Win32-OpenSSH `sshd`) from a POSIX controller — needs only Windows-aware quoting | Phase 2 |
| T2 | Windows as **controller** (native port of L0–L5, SSH transport via `ssh.exe`) | Phase 3 |
| T3 | Windows as **native node** (agent as SCM service, Job Objects, IOCP) | Phase 4 |

---

## 5. Reference Architectures for Enterprise Use Cases

**v0.6 reading rule:** these scenarios describe the architectural direction.
The command blocks below use the current CLI, but only the subsets called out
as implemented are release claims. In particular, remote execution is linear,
there is no automatic enrollment, reconnect, `splice()`, per-stage
sandbox/limits, signed audit, or native Windows node.

### 5.1 Real-time multi-node log streaming

**Scenario:** 500 web nodes; an SRE wants every `5xx` line from `access.log` on one terminal,
live, and piped into an alerting script — no log shipper installed on the nodes.

```text
 web001 ┐ tail -F access.log ──▶ node agent ──psx/1 (credits)──┐
 web002 ┤ tail -F access.log ──▶ node agent ──────────────────┤    controller
   …    ┤                                                    ├──▶ merge(line-framed, [host] tag) ──▶ grep ' 50[0-9] ' ──▶ alert.sh
 web500 ┘ tail -F access.log ──▶ node agent ──────────────────┘    policy: drop-oldest, ring 1 MiB/host
```

```bash
# Agentless: one managed SSH process per in-flight host.
pipeshellx run -i inventory.ini -g web --stream \
  --overflow drop-oldest --ring 1MiB -- tail -F /var/log/nginx/access.log \
  | grep ' 50[0-9] ' | ./alert.sh

# Native: supported first-stage group fan-in followed by one linear chain.
pipeshellx pipe -i inventory.ini \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  "'tail -F /var/log/nginx/access.log'@web | 'grep 500'@local | './alert.sh'@local"
```

- **OS primitives in play:** `tail` blocks on `write(2)` when its pipe fills (kernel
  backpressure); the agent reads only with credits; the controller merges from 500 streams on one
  `epoll`/`kqueue`/IOCP loop; line framing guarantees unbroken lines.
- **DS properties:** per-host FIFO and arrival-order merge. `run`
  drop policies report dropped bytes; native pipeline edges use credit
  backpressure. Connection loss is terminal—v0.6 does not auto-reattach or
  offer infinite retries.
- **Sizing:** credit and configured rings make memory planning explicit, but
  fleet-specific RSS/throughput figures must come from the benchmark harness;
  the plan does not present estimates as release measurements.
- **Exit semantics:** a `tail -F` pipeline is infinite; Ctrl-C → graceful cancel → summary with
  stage/drop/cancellation state; exit `130`. No recovery/resume is
  implied.

### 5.2 Low-overhead distributed IPC pipelines

**Scenario:** move a 200 GB database snapshot from `db1` to `analytics1` compressed in flight,
with no intermediate files and no staging disk, then fan a sharded `grep` across 12 storage
nodes with a single merged, sorted result.

```text
  db1:        pg_dump ──pipe──▶ zstd -T4 ──▶ agent ══ psx/1 stream (mTLS) ══▶ agent ──▶ zstd -d ──pipe──▶ psql     :analytics1
  shard01..12: grep -h "order=42" /data/*.log ──▶ agent ══▶ controller merge ──▶ sort -m -k1 ──▶ stdout
```

```bash
pipeshellx pipe -i inventory.ini \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  "'pg_dump prod'@db1 | 'zstd -T4'@db1 | 'zstd -d'@analytics1 | 'psql warehouse'@analytics1"

pipeshellx pipe -i inventory.ini \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  "'grep -h order=42 /data/shard.log'@shards | 'sort -m -k1'@local"
```

- **Current data path:** local-only graph edges are ordinary pipes with bounded
  controller edge buffers. Native remote segments use credit-controlled TLS
  and a controller relay. The Linux `splice()` fast path is not
  implemented, so v0.6 does not claim zero-copy remote transfer.
- **Pipe semantics across nodes:** `pg_dump` EOF → `zstd` EOF → `EOF` frame → `zstd -d` sees
  EOF → `psql` finishes → exit statuses propagate; a non-zero exit anywhere fails the run with
  per-stage status (`set -o pipefail` semantics by default).
- **Backpressure end-to-end:** if `psql` is slow, credits stop, `zstd -T4` blocks on its pipe,
  `pg_dump` blocks on its pipe. Memory on every hop is bounded by one window.
- **Fault tolerance:** a lost native pipeline connection fails unfinished
  stages and fences node-owned work. Pipeline-stage retry/resume is not
  implemented; `--idempotent --retries` applies to SSH
  `run`, not to `pipe`.
- **Isolation:** stages run as the controller/node service account. Per-stage
  configurable limits and sandboxing remain Phase 6 work.

### 5.3 Lightweight air-gapped automation (no Kubernetes)

**Scenario:** a 120-host facility network with no internet and no container platform. A
 single jump host must: verify configuration drift nightly, roll out a patch with a canary,
and keep an audit trail — with one binary carried in on removable media.

```text
 jump host (controller)                         fleet (POSIX native + Windows SSH)
 ┌───────────────────────────────┐              ┌──────────────────────────────────────┐
 │ pipeshellx controller         │──external───▶ copy binary/cert/service config       │
 │ offline CA + CRL              │──mTLS 7433──▶ pipeshellx node (systemd / launchd)    │
 │                               │──OpenSSH────▶ Windows SSH targets                    │
 │ inventory.ini (groups/tags)   │              │                                      │
 │ cron: nightly drift + audit   │◀─stage data──┤ process output/status                 │
 └───────────────────────────────┘              └──────────────────────────────────────┘
```

```bash
pipeshellx ca init --cn plant-7 --dir ca
pipeshellx node keygen --san spiffe://psx/node/node-01 --out node-01
pipeshellx ca sign --ca ca --csr node-01.csr \
  --san spiffe://psx/node/node-01 --out node-01.crt

pipeshellx diff -i inventory.ini -g linux-native \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  -- cat /etc/ssh/sshd_config

pipeshellx run -i inventory.ini -g linux-native --transport native \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  --canary 5% --fail-fast --timeout 300 --audit-log audit.jsonl \
  -- /usr/local/bin/apply-patch

pipeshellx run -i inventory.ini -g windows-ssh \
  --transport ssh --shell powershell -- Get-HotFix
```

- **No orchestrator:** no etcd, no API server, no scheduler daemon; `cron`/Task Scheduler on the
  jump host triggers runs; state is the inventory file and the audit log.
- **Security in an air gap:** mTLS with a fleet-scoped CA and optional CRL;
  host-key pinning for SSH; externally managed distribution. The opt-in audit
  JSONL is unsigned and not tamper-evident in v0.6.
- **Drift detection** groups successful hosts by exact stdout bytes. stderr is
  excluded, and nonzero exits are host failures. There is no normalization,
  hashing contract, or unified-diff renderer in v0.6.
- **Fault tolerance for automation:** idempotent stages retried; non-idempotent stages
  (`apply-patch`) run once with canary gating; strict exit codes make the cron job's success
  unambiguous.

### 5.4 Also fits (no additional architecture required)

Distributed test fan-out (`pipeshellx run -g ci-agents --json -- ctest …` → CI parses JSON);
fleet-wide forensic collection (`tar c … | zstd`@each → `spool` sink to local disk with
per-host files), musl release artifacts, and arbitrary remote scatter/gather
remain target applications rather than shipped v0.6 guarantees.

---

## 6. Refined Implementation Roadmap

Effort is in focused engineering days; each phase ends with a tagged release, green CI, and the
listed `docs/` updates. Phases 3 and 4 are independent of each other and can proceed in parallel
after Phase 2.

```text
P0 hygiene ─▶ P1 psx::os + reactor (POSIX) ─▶ P2 streaming engine + CLI ─┬─▶ P3 Windows port ──┐
                                                                         └─▶ P4 native backplane ┴─▶ P5 pipelines/DAG ─▶ P6 isolation & hardening ─▶ P7 packaging & 1.0
```

### Phase 0 — Truthful baseline (3–5 days) → `v0.1.0`
Fixes verified against the current tree; extends `docs/testing.md`, `docs/deployment.md`, `docs/security.md`, `docs/authentication.md`.
- [x] Fix the `-Werror` break at `include/logger.hpp:33` (use or remove `currentLevel`); fresh clone must build on Linux + macOS.
- [x] CI: GitHub Actions `{ubuntu, macos} × {gcc, clang} × {Debug, Release}`; GTest via `FetchContent` so tests can never be skipped; ASan/UBSan job; `ctest` must run the 11 existing tests.
- [x] Security defaults: `accept-new` + per-inventory `known_hosts` + `BatchMode=yes` (`src/ssh_auth.cpp:28`); password via `sshpass -d <fd>` instead of argv (`:49`); `ssh` via `PATH` (`:26`).
- [x] Remove `top` from the allowlist (hangs the REPL); default logging to a file; `--verbose` for console.
- [x] Rename CMake targets `remote_command_*` → `pipeshellx_*`; fix `test_prcoess_manager.cpp` filename and the two misnamed tests in `test_client_config.cpp`.
- [x] Docs drift: relative links in `README.md`; `docs/testing.md` lists all 4 test files; `docs/security.md` allowlist includes `hostname`; `docs/deployment.md` platform statement.
- [x] Add `SECURITY.md`, `CONTRIBUTING.md`, `.clang-format`, `.clang-tidy`, `docs/adr/` with ADR-001…005.
- [x] Baseline measurements recorded in `docs/benchmarks.md` (spawn latency, fan-out at 50/100 `ssh localhost`, RSS, fd count) — the numbers §7 will be compared against.
- [x] Bonus fix found by the baseline harness: `executeRemote()` reported any fast-failing host as
  `command timed out` (idle 50 ms poll misread as the deadline); now checks the real deadline
  (`tests/test_process_manager.cpp`).
- **Exit criteria:** fresh clone builds and tests pass in CI on both OSes; no insecure SSH default remains.
- **Status (2026-08-22):** done — 55 GTest cases, green on macOS (Apple Clang 21, Debug/Release,
  ASan+UBSan) and GCC 15 (library/app/bench); `.github/workflows/ci.yml` covers
  `{ubuntu, macos} × {gcc, clang} × {Debug, Release}` + sanitizers. CI on GitHub runs on the first
  push of the tag. Fan-out baseline numbers require `sshd` on the bench host and are filled in by
  the nightly `bench.yml` (see `docs/benchmarks.md`).

### Phase 1 — `psx::os` and the runtime on POSIX (8–12 days) → `v0.2.0`
Extends `docs/ipc_design.md`, `docs/process_management.md`, `docs/architecture.md`; creates `docs/os_abstraction.md`.
- [x] `include/psx/os/{handle,pipe,process,signal,console,paths}.hpp` with `src/os/posix/{common,linux,darwin}.cpp`; CLOEXEC-at-creation invariant; public headers free of POSIX types (removes `pid_t`/`ssize_t`/`<signal.h>` leaks listed in §2.1).
- [x] `os::Process::spawn` via `posix_spawn` (+ `prlimit` on Linux); `ProcessGroup`; `signal(Graceful|Kill)` with TERM→KILL grace.
- [x] `runtime::Reactor` with `poll` backend first (behaviour-identical to today's loop), then `epoll` (ET) and `kqueue` (`EV_CLEAR`); `ChildExitSource` via `pidfd`/`EVFILT_PROC`/`SIGCHLD`; `SignalSource` via `signalfd`/`kqueue`; timing wheel.
- [x] Refactor `ProcessManager::execute` and `executeRemote` onto the new primitives; `Pipe` from `ipc_engine.h` merged into `os::Pipe`; `SessionManager` deleted (git history keeps it).
- [x] Raise `RLIMIT_NOFILE` at startup; handle-accounting counters.
- [x] Tests: `tests/unit/os/` (handle leak over 10 k spawns, CLOEXEC audit via `/proc/self/fd` / `proc_pidinfo`, zombie regression from `docs/testing.md`, EINTR injection); golden tests proving output parity with `v0.1.0`.
- [x] CI lint: forbidden-include grep; layer-dependency check.
- **Exit criteria:** identical CLI behaviour; zero fd leaks and zero zombies in the soak test; `poll` backend removable without behaviour change.
- **Status (2026-08-22):** done — `include/psx/{result,os/*,runtime/*}.hpp` (std-only, lint-enforced) with
  `src/os/posix/` backends: `Handle`/`Pipe`/io, `Process` (`posix_spawn` + `prlimit`, fork fallback only for
  Darwin rlimits, `stop()` TERM→KILL grace), `Poller` (poll/kqueue/epoll), `ChildExitSource` (pidfd/`EVFILT_PROC`
  + SIGCHLD fallback), `SignalSource`, `Console`, `paths`, `system`; `runtime::Reactor` (timer queue is a min-heap —
  the hierarchical wheel is deferred until profiling asks for it). `ProcessManager` runs on the reactor; golden
  tests pin v0.1.0 behaviour and pass on the native and `poll` backends (`PIPESHELLX_POLLER`); soak tests show
  zero descriptor leaks and zero zombies; `scripts/check_layering.sh` runs in CI. Measured: spawn p50 3.2 → 1.7 ms,
  p99 55 → 3.7 ms (`docs/benchmarks.md`). Linux (epoll/pidfd/signalfd) compiles for CI but was not run locally.

### Phase 2 — Streaming engine and the real CLI (10–15 days) → `v0.3.0`
Extends `docs/ipc_design.md`, `docs/system_flow.md`, `docs/distributed_execution.md`, `docs/testing.md`; creates `docs/pipelines.md` (sinks), `docs/json.md`.
- [~] `stream::{Stream, BoundedBuffer, CreditWindow, LineFramer}`; local DAG edges use bounded backpressure and `run` implements bounded drop capture plus disk spool. **Open:** `run --overflow block` is currently lossless/unbounded rather than pausing producers, and spool reconstructs the full result at completion.
- [x] Live `--stream` (host-prefixed, colour-stable, TTY-detected), `--group` (today's format, kept), `--json`; end-of-run summary; strict exit codes; `130` on cancel.
- [~] Orchestrator v1: sliding-window scheduler (`-c`, default 64) ✓, per-stage + global deadlines ✓, `--retries` jittered backoff ✓, cancellation via `SignalSource` (exit `130`) ✓, `--fail-fast` ✓. **Deferred:** a formal `Run`/`Stage` state-machine *type* and the per-node **circuit breaker** (premature in the single-command model — the breaker needs multi-stage-per-node work).
- [x] `SshTransport` extracted from `executeRemote`; `ControlMaster` opt-in (`--reuse`); Windows-target quoting (support tier T1, `--shell posix|cmd|powershell`); host-key-change reporting UX.
- [x] `Inventory` (INI groups/tags/per-host options, XDG/`%APPDATA%` lookup, `clients.txt` import); `pipeshellx run|ping|hosts|shell`; `--policy` file replaces the hardcoded allowlist (allowlist kept as the default policy for `shell` demo mode).
- [x] REPL rewritten as a thin client over `Run`; spin-wait threads removed.
- [x] Audit log JSONL; file logging with rotation; `run_id`/`stage_id` in `LogContext`.
- [~] Integration rig: property test "no interleaved partial lines" ✓ (randomized, in `test_line_framer.cpp`). **Blocked in this env:** docker-compose `openssh-server` fleet + the TSan job (no Docker daemon available — see the dev-environment notes).
- **Exit criteria:** **open.** `tail -F` across 100 containers with flat controller RSS and §7 targets T1–T6 still need a Docker-capable Linux/macOS validation host; true lossless bounded `run` backpressure is also not implemented. Drop policies provide the current hard retained-memory bound.

### Phase 3 — Windows port (15–20 days) → `v0.4.0`  ⏸️ DEFERRED (future work)

> **Deferred to future work** (2026-08-23): building/testing the Win32 `os` backend, the IOCP
> reactor backend, `ssh.exe`-discovery `SshTransport`, and the `windows-latest` CI requires an
> MSVC/clang-cl Windows toolchain, which the current development environment lacks. Writing
> untested Win32 C++ here would violate the project's build/test discipline. The completable part
> — `docs/windows.md` (support tiers, §4.3–4.7 differences) — is done, and support tier **T1**
> (Windows as an SSH *target*, `--shell cmd|powershell`) already shipped in Phase 2. Resume this
> phase on a Windows-capable host / CI. Version `v0.4.0` is reserved for it; the native backplane
> (Phase 4) proceeds next as `v0.5.0`.
Extends `docs/os_abstraction.md`, `docs/deployment.md`, `docs/testing.md`.
- [ ] `src/os/win32/{handle,pipe(overlapped named),process(CreateProcessW + HANDLE_LIST + Job Objects),console,paths,signal(ConsoleCtrl)}.cpp`.
- [ ] `Reactor` IOCP backend; `ChildExitSource` via job completion port; `Reactor::wake()` via `PostQueuedCompletionStatus`.
- [ ] `SshTransport` with `ssh.exe` discovery; `%APPDATA%` config; UTF-8/VT console; CRLF option in `LineFramer`.
- [ ] CI: `windows-latest` × {MSVC, clang-cl}; the shared `tests/unit/os/` suite passes unmodified; static CRT build.
- [x] `docs/windows.md`: support tiers, known differences (§4.3), service install. _(Written; reflects T1 shipped in v0.3.0 and the T2/T3 plan. The Win32 os backend, IOCP reactor, and windows CI remain — they need a Windows toolchain, unavailable in this env.)_
- **Exit criteria:** support tier T2 — a Windows controller runs the same integration scenarios against the Linux container fleet.

### Phase 4 — Native backplane (15–20 days) → `v0.5.0`
Extends `docs/distributed_execution.md`, `docs/authentication.md`, `docs/security.md`; creates `docs/wire_protocol.md`.
- [x] Spike: frame codec (TLV envelope) + `OPEN` encoding (`OpenRequest`) + OpenSSL-vs-SChannel decision → ADR-006 (TLV) / ADR-007 (OpenSSL). In `psx::transport`, bounds-checked + fuzz-style tested.
- [x] `os::Socket`, `os::Tls` (mTLS 1.3, SAN-URI identity, CRL file); `transport::NativeTransport` (multiplexed streams, credit windows, `PING`/`PONG` leases, `GOAWAY` drain).
- [~] `pipeshellx node` agent: listener, job supervisor (lease expiry kills jobs), `AF_UNIX`/named-pipe local endpoint, metrics endpoint; service install for systemd/launchd/SCM. _(listener ✓, lease/fencing job supervisor ✓, optional pre-spawn `node --policy FILE` ✓, **service install ✓** — `node systemd-unit` (restart + hardening: NoNewPrivileges/ProtectSystem/PrivateTmp/…) and `node launchd-plist` emit ready-to-install units from the daemon flags, including optional `--policy` and `--control`; the plist passes `plutil -lint`. **AF_UNIX local endpoint + metrics ✓** — `os::Socket::listenUnix/connectUnix`; `node --control PATH` serves a JSON metrics snapshot (`accepted_total`/`active_connections`/`active_stages`) that `node status --control PATH` reads. **Remaining:** SCM (Windows service, with Phase 3).)_
- [~] `pipeshellx ca init|issue|revoke|sign` and manual CSR
  enrollment. _(`revoke` records the serial and regenerates a
  CA-signed CRL; TLS can enforce it. `node keygen` creates a local
  private key plus CSR, and `ca sign` verifies/signs the
  operator-approved SAN. **Remaining:** there is no `node enroll`
  command; automating CSR/certificate movement, binary copy, and service
  installation over SSH is deferred.)_
- [ ] Reconnect-and-resume within the lease window; `Lost` handling in the orchestrator; fuzzers for the frame decoder (`tests/fuzz/`).
- [~] Loopback transport for protocol tests; fault injection (drop/duplicate/delay frames, kill agent mid-stream, partition). _(Loopback `Session` pair in tests; **fault injection done** — `test_session_faults.cpp`: structured adversarial frames (DATA for unknown/closed stream, duplicate EXIT, unknown frame type, WINDOW_UPDATE for a closed stream, wrong-role OPEN) all yield clean protocol errors, plus two fuzz loops (arbitrary + mutated valid frame streams, 10k iters) that stay memory-safe under ASan/UBSan — a stand-in for the toolchain-blocked libFuzzer. kill-agent-mid-stream/partition are covered by the fencing + lease tests. **Remaining:** in-tree libFuzzer target (needs homebrew clang).)_
- **Exit criteria:** 1 000 simulated nodes on one TLS connection each; fencing proven (no orphan after controller `kill -9`); §7 targets T7–T10. _(Fencing on disconnect: ✓ proven — `NodeFencingTest` plus a real-binary `kill -9` smoke confirm the node kills the running stage's process group when the controller connection drops, leaving no orphan (~50 ms). `~NodeStageRunner` signals the group explicitly. Silent-partition lease: ✓ done — `NativeTransport` runs a PING/PONG heartbeat (`kDefaultLease` 2 s×3); a peer that goes quiet (no FIN, no PONG) fails via `onError(Timeout)` in ~3–4 intervals. Covered by `LeaseExpiresWhenThePeerGoesSilent` + `LeaseKeepsAnIdleConnectionAlive`, and a real-binary `SIGSTOP`-the-node smoke (controller gave up in ~8 s, not 60). Remaining: node-death fencing (a hard-killed node still orphans its stage — needs a parent-death signal / `PR_SET_PDEATHSIG` / kqueue parent watch), and full 1 000-connection-over-TLS scale (CI/dedicated-host). **Scale ✓ demonstrated** — `test_scale.cpp`: one connection multiplexes 1 000 concurrent streams, and 1 000 simulated nodes each run a stage to completion (loopback, no TLS/RSA cost); `NodeServerTest.HandlesManyConcurrentControllerConnections` runs 100 concurrent mTLS connections through one NodeServer on one reactor, every stage exiting cleanly (~0.9 s). The literal 1 000-over-real-TLS run (≈2 000 RSA handshakes) is left to CI to keep the dev machine unloaded.)_
- **Status (2026-08-23):** done → `v0.5.0`. The psx/1 native backplane end to end: TLV frame codec + `FrameDecoder`
  (fuzz-hardened); `Session` mux with HTTP/2-style per-stream credit flow control, real read-side backpressure, and
  distinct stdout/stderr channels; `os::{Socket(TCP+AF_UNIX),Tls}` (mTLS 1.3, SAN-URI identity, CRL); `NativeTransport`
  (PING/PONG liveness lease, GOAWAY); the `pipeshellx node` daemon (accept loop, deferred-reap teardown, stage fencing
  on controller loss, optional pre-spawn `--policy`, `--control` metrics endpoint, systemd/launchd unit emitters) and `NativeController` driving
  `run --transport native`; an offline CA (`ca init|issue|revoke|sign`, CRL, CSR enrollment via `node keygen`). Fencing
  proven (no orphan after controller `kill -9`; lease detects a silent partition); scale demonstrated (1 000-stream mux
  + 1 000 loopback nodes + 100-way concurrent mTLS fan-out). **Recorded as environment exceptions** (per the `v0.3.0`
  precedent): the literal 1 000-connection-over-real-TLS run and the §7 T7/T8 throughput targets (need a fleet/CI host);
  libFuzzer (Apple clang lacks the runtime); the SSH-push automation for enrollment (no `sshd` here). **Deferred to
  later phases/future work:** reconnect-and-resume within the lease window + orchestrator `Lost` handling (a larger
  resilience feature that revisits the immediate-fence model); node-death fencing on a hard-killed *node* (needs a
  kernel parent-death signal — Phase 6 isolation); Windows SCM service (with Phase 3).

### Phase 5 — Pipelines as DAGs (10–15 days) → `v0.6.0`
Extends `docs/system_flow.md`, `docs/pipelines.md`, `docs/architecture.md`.
- [x] `pipeshellx pipe` with `'cmd'@placement` stages and
  `|` edges; restricted `pipeline.yaml` via
  `pipe --file`; `Planner` validates identifiers, edges,
  acyclicity, and placement. All-local files execute as true declared DAGs,
  including fan-in/fan-out and bounded per-edge backpressure.
- [~] Cross-node edges over `NativeTransport` with stdin forwarding,
  EOF/half-close/exit propagation, pipefail, mixed `@local` segments,
  and first-stage group fan-in. **Current boundary:** every graph containing a
  remote stage must be exactly one declared chain; non-linear remote DAGs fail
  explicitly. SSH cross-node edges and general cross-stream remote-DAG
  backpressure remain open. `--ordered`, lossless `spool`
  capture, and `--idempotent` retry gating exist on the `run`
  path.
- [x] `ConsensusEngine`, `diff`,
  `run --consensus`, native `--canary N|N%`, and
  `--fail-fast`. `diff` compares exact stdout only; stderr
  is excluded, a nonzero remote exit is a host failure, and its exit contract
  is 0 unanimous / 1 drift / 2 host or usage/configuration failure.
- [ ] `splice()` fast path on Linux for pipe↔socket edges.
- [~] Tests: local declared-edge order, fan-in/fan-out, bounded slow-consumer
  behavior, terminal/reap lifecycle, deterministic pipefail, linear
  local/native placement, and exact remote non-linearity rejection are covered.
  The full §5 reference scenarios on all three platforms are not.
- **Exit criteria:** **open**. The v0.6 implementation is validated on the
  POSIX scope; Windows, non-linear remote DAGs, SSH cross-node edges, and the
  complete §5.1–5.3 integration fleet remain deferred. The version is present
  in the build, but a `v0.6.0` tag/release has not been published.

### Phase 6 — Isolation, limits, and hardening (8–12 days) → `v0.7.0`
Extends `docs/security.md`, `docs/process_management.md`, `docs/deployment.md`.
- [ ] Per-stage `limits{cpu,mem,fds,wall}` → `prlimit`/cgroup v2 (Linux), Job Objects (Windows), advisory on macOS with a warning.
- [ ] `--sandbox` tiers: seccomp-bpf + Landlock (Linux), restricted token + AppContainer (Windows), `sandbox_init` (macOS).
- [ ] `SecureString` for passwords/keys; `PR_SET_DUMPABLE`/`MADV_DONTDUMP`/`VirtualLock`.
- [ ] Signed audit chain; `--no-audit`; retention guidance.
- [ ] Threat model v2 review; dependency SBOM (CycloneDX); reproducible static builds; fuzzers for inventory, CLI, line framer, frame decoder in CI (libFuzzer, 10 min/PR).
- **Exit criteria:** external security review of defaults completed; no high findings open.

### Phase 7 — Packaging, documentation, benchmarks, launch (6–8 days) → `v0.9.0` → `v1.0.0`
Extends `docs/deployment.md`, `docs/benchmarks.md`; Appendix A launch material.
- [ ] Release CI: static Linux (musl, amd64/arm64), macOS universal, Windows amd64/arm64; checksums + sigstore signatures; `install.sh`/`install.ps1`; Homebrew tap, `.deb`/`.rpm` (nfpm), winget manifest, AUR.
- [ ] Shell completions; man page; `docs/` cookbook of 10 recipes including the three reference architectures.
- [ ] `docs/benchmarks.md` published with measured numbers per platform from the `bench/` harness (§7.2).
- [ ] 1.0 gate: CI green on 3 OSes · zero known crashes · §7 targets met or documented as exceptions · security review done · CHANGELOG · Appendix A launch checklist.

**Total: ≈ 75–107 focused days** (≈ 4–6 months part-time; Phases 3 and 4 parallelisable with a
second contributor).

---

## 7. Performance Benchmarks & Key Technical Metrics

### 7.1 Targets (measured by the harness in §7.2; baseline recorded in Phase 0)

| # | Metric | Target | Why this number |
|---|---|---|---|
| T1 | Local spawn latency (`posix_spawn` → `exec`), p50 / p99 | Linux/macOS ≤ 0.5 ms / 2 ms; Windows ≤ 10 ms / 25 ms | `posix_spawn` avoids page-table copy; `fork()` in a 500 MB parent costs ms |
| T2 | Time-to-first-byte, 1 host, SSH, warm `ControlMaster` | ≤ 15 ms over LAN | ssh multiplexing; no new TCP/KEX |
| T3 | Fan-out 1 000 × `uptime` (agentless, `-c 256`) | ≤ 12 s end-to-end on a 4-core controller; controller CPU ≤ 1 core | bounded by `ssh` KEX; controller must not be the bottleneck |
| T4 | Fan-out 1 000 × `uptime` (native backplane) | ≤ 2 s; controller RSS ≤ 200 MB | one TLS connection per node, multiplexed |
| T5 | Event-loop cost at 1 000 active streams | ≤ 3 syscalls per 64 KiB delivered; wake-ups scale O(ready), not O(N) | epoll/kqueue/IOCP vs today's O(N) `poll` + O(N) `waitpid` |
| T6 | Controller memory per active stream | ≤ 300 KiB (256 KiB window + state), **flat under producer overload** | bounded buffers; the backpressure test: 100 producers × 10 MB/s into a 1 MB/s sink → RSS stays flat, producers block |
| T7 | Cross-node stream throughput, single edge, 10 GbE | ≥ 600 MB/s with mTLS (AES-GCM, AES-NI); ≥ 1.5 GB/s plaintext loopback | TLS record cost dominates; `splice()` removes the user-space copy |
| T8 | Merged log throughput on controller (500 hosts, line-framed, `--stream`) | ≥ 1 M lines/s, p99 line latency ≤ 10 ms on LAN | §5.1 scenario |
| T9 | Node-loss detection / fencing | `LOST` reported ≤ 6 s (3 × 2 s heartbeats); no orphan process on the node ≤ 10 s after controller `kill -9` | lease semantics |
| T10 | Reconnect-resume | stream resumes with zero duplicate or lost bytes when reconnect < lease window | sequence/window accounting |
| T11 | Handle hygiene | fd/handle count identical before and after 10 000 spawn cycles; zero zombies; zero inheritable handles in children (`/proc/<pid>/fd`, Handle enumeration) | §3.3 invariants |
| T12 | Output correctness | 0 interleaved partial lines across 10⁶ lines from 200 concurrent producers | `LineFramer` property test |
| T13 | Binary size | SSH-only build ≤ 4 MB static; with native transport + OpenSSL ≤ 9 MB | "one binary on a USB stick" |
| T14 | Startup | `pipeshellx --version` ≤ 5 ms; `run` reaches first spawn ≤ 20 ms (inventory of 1 000 hosts parsed) | "instant" principle |

### 7.2 Benchmark harness (`bench/`, run in CI nightly, published in `docs/benchmarks.md`)

- **Local fleet:** `ssh localhost` × N with `ControlMaster` off/on, and the docker-compose
  `openssh-server` fleet (N = 50/200/500) for agentless numbers; `pipeshellx node` × N
  containers for backplane numbers; a `loopback` transport for protocol micro-benchmarks.
- **Tools:** `hyperfine` for wall-clock; `perf stat` / `strace -c` (Linux), `dtrace`
  (macOS), ETW (Windows) for syscalls and context switches; `/usr/bin/time -v` / `rusage` from
  `EXIT` frames for RSS; `ss -i`/`TCP_INFO` for backplane window behaviour.
- **Scenarios:** spawn-loop (T1), cold vs warm SSH (T2–T3), backplane fan-out (T4),
  throughput sink (`pv`-style null sink, T5/T7/T8), overload (T6), chaos (`kill -9` agent,
  `tc netem` partition/delay, T9–T10), hygiene (T11), framing property test (T12), size/startup
  (T13–T14).
- **Regression policy:** a nightly result worse than the last tagged release by > 10 % on any
  target fails the build; numbers per platform are committed with the release tag.

### 7.3 Target operational metrics (not a v0.6 CLI contract)

`stages_active`, `stages_total{state}`, `handles_open`, `bytes_{in,out}{transport}`,
`window_stalls_total`, `drops_total{policy}`, `retries_total{reason}`, `heartbeat_rtt_seconds`,
`spawn_latency_seconds` (histogram), `run_duration_seconds`,
`nodes{state}`. A future Prometheus endpoint and expanded JSON
summary will expose these. v0.6 exposes only
`accepted_total`/`active_connections`/`active_stages`
through the node's local `--control` socket.

---

## Appendix A — Product positioning, CLI, legal, and risks (carried forward)

**Positioning:** *"The ripgrep of parallel SSH — instant, secure by default, the only one that
tells you which hosts disagree — and the only one that lets you pipe between them."*

**Why existing tools leave this gap:** `pssh`/`dsh` unmaintained, buffered output, Python;
`pdsh` fast but no drift view; `clush` Python stack, limited diff; Ansible ad-hoc slow defaults,
Python on both ends; Fabric a library, not a tool; `csshX` window-per-host; Go one-offs dormant;
none of them stream a pipe between two remote hosts with backpressure.

**CLI surface (authoritative form lives in `docs/pipelines.md` from Phase 2):**

```text
pipeshellx run  [-i FILE] [-g GROUP|-t TAG|-H h1,h2]
                [--stream|--group|--json|--consensus] [--ordered]
                [-c N] [--timeout S] [--overflow P] [--ring SIZE]
                [--policy FILE] [--reuse] [--retries N --idempotent]
                [--fail-fast] [--shell S] [--audit-log FILE]
                [--transport ssh|native]
                [--cert F --key F --ca F [--crl F] [--native-port P]]
                [--canary N|N%] -- <command...>
pipeshellx ping  [-i FILE] [-g GROUP|-t TAG|-H h1,h2] [--timeout S]
pipeshellx diff  [--json] [-i F] [-g G|-t T|-H h1,h2]
                 --cert F --key F --ca F [--native-port P] -- <command...>
pipeshellx pipe  [--check] [--file FILE | "'cmd'@place | ..."]
                 [-i F --cert F --key F --ca F] [--native-port P]
pipeshellx hosts [list] [-i FILE]
pipeshellx hosts add HOST [-g GROUP] [host options] -i FILE
pipeshellx hosts remove HOST -i FILE
pipeshellx hosts import clients.txt -i FILE
pipeshellx ca    init|issue|revoke|sign ...
pipeshellx node  --cert F --key F --ca F --listen HOST:PORT
                 [--allow SANs] [--crl F] [--policy F] [--control PATH]
pipeshellx node  keygen|systemd-unit|launchd-plist|status ...
pipeshellx shell [--verbose] [--log-file PATH]
```

Inventory (INI): `[web]` /
`web1.example.com user=deploy port=2222 tag=prod transport=native`.
Inventory precedence: explicit `-i` >
`PIPESHELLX_INVENTORY` > project `inventory.ini` > project
legacy `clients.txt` > user config `inventory.ini`.

**IP & legal:** Apache-2.0 (express patent grant). Parallel SSH fan-out has decades of prior
art and the repo is public — do not budget around patents; if a genuinely novel mechanism emerges
(e.g., in credit-based cross-node pipe scheduling) consult counsel *before* pushing it. Real
protection: trademark the name, own the docs domain, ship faster than clones.

**Success metrics (post-1.0):** 30 days — 300+ stars, 3+ external contributors, 0 open crash
bugs; 90 days — packaged in Homebrew/winget, 5+ independent write-ups; qualitative —
"replaced my pssh scripts", consensus and cross-node pipe demos shared organically.

**Risks & mitigations:**

| Risk | Mitigation |
|---|---|
| Scope creep toward "mini-Ansible" / "mini-Kubernetes" | §1.4 principles and non-goals are the veto; no state store, no scheduler, no replication |
| Windows port stalls on IOCP/pipe semantics | completion-style API chosen up front (§3.4); overlapped named pipes from day one; shared OS test suite |
| Native backplane becomes a second product | same binary, opt-in, SSH bootstrap; SSH-only build flag keeps the small binary |
| OpenSSH version drift (`accept-new`, `SSH_ASKPASS_REQUIRE`) | detect version at startup; document minimums; fall back with loud warnings |
| Big-fleet fd/process limits | raise `RLIMIT_NOFILE`; scheduler sized from the handle budget; native transport for > 500 hosts |
| Single-maintainer burnout | CI carries quality; Phases 3/4 parallelisable; good-first-issues seeded from Phase 2 |

---

*Maintained by Rushikesh Patil · Apache-2.0 · Historical baseline verified
against commit `2e10869` on 2026-08-22; v0.6 truth audit updated
2026-08-30. Update this plan in the same PR as any scope change.*
