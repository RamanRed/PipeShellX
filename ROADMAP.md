# PipeShellX Roadmap

This file lists work that is not implemented. Current behavior is documented
in [Architecture](docs/architecture.md), [Deployment](docs/deployment.md),
[Pipelines](docs/pipelines.md), and the
[psx/1 wire protocol](docs/wire_protocol.md). Items here are goals, not
release promises or supported behavior.

## Release and distribution

- Tag and publish v0.6.0 after the implemented signed release workflow and
  final release-candidate validation pass.
- Add reproducible release builds and package channels for the supported Linux
  and macOS scope. Windows packages depend on the Windows port.

## Platform portability

- Implement the Win32 L0/L1 backend: non-inheritable handles, overlapped pipes,
  `CreateProcessW`, Job Objects, IOCP, console control, paths, sockets,
  and local control transport.
- Support Windows as a controller and native node, including OpenSSH discovery,
  SCM service integration, UTF-8 console behavior, and MSVC/clang-cl CI.
- Keep the shared OS/runtime conformance suite behavior-identical across POSIX
  and future Win32 backends.

## Distributed pipelines and resilience

- Extend native execution from a declared remote chain to general non-linear
  remote and mixed DAGs with bounded cross-stream backpressure.
- Define and implement SSH-carried pipeline edges where native nodes are not
  available.
- Add native reconnect/resume, byte acknowledgements, and duplicate-spawn
  suppression without weakening connection-loss fencing.
- Guarantee descendant containment when the node process itself is hard-killed
  on each supported platform.
- Evaluate a Linux `splice()` fast path without changing observable
  pipe, EOF, cancellation, or accounting semantics.

## Resource isolation and security hardening

- Expose portable per-stage CPU, memory, descriptor, and wall-clock limits,
  with documented platform differences.
- Add opt-in OS isolation tiers, node privilege separation, and narrowly scoped
  per-controller execution identities suitable for stronger tenant boundaries.
- Introduce locked/zeroing secret storage and core-dump protections for
  password and private-key material.
- Add signed or hash-chained audit records, retention controls, and external
  verification guidance.

## Verification and performance

- Qualify supported behavior against heterogeneous real SSH/native fleets,
  including loss, delay, process crashes, and capacity limits.
- Add continuous libFuzzer coverage for inventory, CLI, framing, protocol, and
  line handling.
- Turn the nightly benchmark artifact into a documented, statistically sound
  regression gate with comparable baselines; cover cold/warm SSH, native
  fan-out, throughput, overload, handle hygiene, startup, and artifact size.
- Extend installation and upgrade coverage from versioned archives to future
  package-manager channels and heterogeneous deployment environments.
