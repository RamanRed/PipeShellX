# Benchmarks

Targets are defined in `PLAN.md` §7.1 (T1–T14) and measured by the harness in
`bench/` (§7.2). This file records the **baseline** — what the Phase 0 code
costs — and, from each tagged release on, the numbers per platform. A nightly
result worse than the last tag by more than 10 % on any target fails the
`bench.yml` workflow.

## Harness

`pipeshellx_bench_baseline` (built by default; `-DPIPESHELLX_BUILD_BENCH=OFF`
to skip) measures the current `ProcessManager` end to end:

| Section | What is measured | Flag |
|---|---|---|
| Local spawn round-trip | `ProcessManager::execute({"true"})`: `fork` + `exec` + 3 pipes + `poll` loop + `waitpid`, p50/p90/p99/max/mean, throughput, open-descriptor count before/after, peak RSS | `--spawn N` (default 1000) |
| SSH fan-out | `executeRemote()` against N copies of `<user>@localhost` running `uptime`; wall time, per-host cost, successes, descriptors, peak RSS; skipped with the reason when `ssh localhost` is not usable | `--fanout 50,100` (default), `--user NAME` |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pipeshellx_bench_baseline
ulimit -n 4096
./build/bin/pipeshellx_bench_baseline --spawn 2000 --fanout 50,100
```

The fan-out section needs an `sshd` on the bench host that accepts the current
user's key (`ssh -o BatchMode=yes localhost true` must succeed). The nightly
workflow sets that up on `ubuntu-latest`; on a developer Mac enable
*Remote Login* or run it against the docker fleet (Phase 2).

## Baseline — Phase 0 (`v0.1.0`, 2026-08-22)

Machine: Apple M4 (10 cores), 16 GiB, macOS 26.5.2 (Darwin 25.5.0 arm64),
Apple Clang 21.0.0, `-O3`, quiet machine, `ulimit -n 4096`.

### Local spawn round-trip (`execute({"true"})`, N = 2000)

| Metric | Value | Note |
|---|---|---|
| p50 / p90 / p99 | 3.21 ms / 4.43 ms / 55.3 ms | T1 target: ≤ 0.5 ms / 2 ms (p50 / p99) |
| mean / max | 4.99 ms / 122 ms | |
| throughput | 200 spawns/s | |
| failures | 0 | |
| open descriptors before / after | 3 / 3 | no leak (T11 seed) |
| peak RSS | 1.6 MiB | |

Reading the numbers:

- **p50 ≈ 3 ms** is the cost of `fork()` + three `pipe()` calls + a `poll`
  loop that rebuilds its `pollfd` vector on every wake-up + `waitpid(WNOHANG)`
  polling. `posix_spawn` plus pollable child handles (Phase 1) is what the T1
  target assumes.
- **p99 ≈ 50 ms is structural, not noise.** When both pipes reach EOF before
  the child's exit status is collectable, the loop has no descriptors left to
  poll and sleeps for its 50 ms idle clamp before calling `waitpid` again
  (`src/process_manager.cpp`, `pollTimeoutMs = 50`). ≈ 1–2 % of spawns hit that
  window. `ChildExitSource` (`pidfd` / `kqueue EVFILT_PROC`) removes the sleep
  entirely. The same idle clamp used to be *misread as the deadline* and
  reported fast-failing SSH hosts as `command timed out`; fixed in Phase 0
  (`tests/test_process_manager.cpp`).
- RSS is tiny because output is a few bytes; the unbounded per-worker
  `std::string` growth (§2.1) only shows under load (T6) and is measured by the
  Phase 2 overload scenario.

### SSH fan-out (`ssh localhost uptime`, agentless)

**Not measured on the baseline machine** — `sshd` is disabled on the developer
Mac (`ssh localhost` → `connection failed`; the harness reports `SKIPPED`).
The first numbers come from the nightly `bench.yml` run on `ubuntu-latest`
(N = 50 and 100 `ssh localhost`) and will be recorded here with the run id.

What the baseline is expected to show, from the design (§2.1): one `ssh`
process per host at ≈ 5–8 MB RSS, two pipes per worker, an O(N) `pollfd`
rebuild and O(N) `waitpid(WNOHANG)` calls per wake-up — i.e. controller CPU
proportional to N × ready events. T3 (1 000 hosts ≤ 12 s, ≤ 1 core) is the
number to beat in Phase 2.

## Per-release results

| Release | Platform | T1 p50 / p99 | T3 (1 000 × `uptime`) | T11 descriptors | Notes |
|---|---|---|---|---|---|
| `v0.1.0` | macOS arm64 (M4) | 3.21 ms / 55.3 ms | not measured | 3 → 3 | baseline; fan-out requires `sshd` |
| `v0.1.0` | ubuntu-latest (CI) | *pending nightly* | *pending nightly* | | first `bench.yml` run |

Later phases add the scenarios from §7.2 (cold vs warm SSH, backplane
fan-out, throughput sink, overload, chaos, framing property test, size and
startup) as they become measurable.
