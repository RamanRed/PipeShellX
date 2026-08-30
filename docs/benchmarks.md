# Benchmarks

PipeShellX has one maintained benchmark executable:
`pipeshellx_bench_baseline`. It measures the current `ProcessManager` paths; it
is not a general workload generator or a release-performance guarantee.

## Current harness

The harness reports two sections:

- `--spawn N` runs `ProcessManager::execute({"true"})` after a short warm-up
  and reports p50, p90, p99, mean, maximum, throughput, failures, open file
  descriptors, and peak RSS.
- `--fanout N[,N...]` runs `uptime` over system OpenSSH against N copies of
  `localhost` and reports wall time, per-host time, successful hosts, open file
  descriptors, and peak RSS. It reports `SKIPPED` when localhost SSH is not
  usable.

Build and run it in Release mode:

```bash
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIPESHELLX_BUILD_TESTS=OFF
cmake --build build-bench --parallel --target pipeshellx_bench_baseline

ulimit -n 4096
./build-bench/bin/pipeshellx_bench_baseline \
  --spawn 2000 --fanout 50,100
```

The fan-out section requires a running SSH server that accepts the current
user's key. The harness uses a temporary known-hosts file rather than the
operator's default file.

## Current hosted result

GitHub Actions run
[33299069455](https://github.com/patil-rushikesh/PipeShellX/actions/runs/33299069455)
completed successfully on `ubuntu-latest` on 2026-08-30. It built the Release
harness, configured localhost OpenSSH, ran `--spawn 2000 --fanout 50,100`, and
uploaded `baseline.md`.

### Local process round-trip

| Metric | Result |
| --- | ---: |
| Iterations | 2,000 |
| Failures | 0 |
| Throughput | 1,652 spawns/s |
| p50 / p90 / p99 | 0.592 / 0.655 / 0.751 ms |
| Open descriptors before / after | 8 / 8 |

### Localhost SSH fan-out

| Hosts | Successful | Wall time |
| ---: | ---: | ---: |
| 50 | 50 / 50 | 3,161.871 ms |
| 100 | 100 / 100 | 6,210.868 ms |

These numbers are evidence for that runner and revision only. Hosted-runner
load, image changes, SSH setup, and shared infrastructure introduce variance;
compare runs made with the same command and environment.

## Workflow

`.github/workflows/bench.yml` runs nightly and by manual dispatch. It installs
and configures localhost OpenSSH, builds only the Release benchmark target,
runs the command above with shell pipeline failure propagation enabled, and
uploads the Markdown output. The workflow is intentionally separate from the
merge-gating correctness matrix: a failed benchmark run is visible, but the
recorded timings are not checked against a fixed threshold.

The current harness does not measure native transport, a real remote fleet,
network impairment, reconnect behavior, sustained output throughput, or
Windows. Those need purpose-built environments rather than interpretations of
the localhost numbers above.
