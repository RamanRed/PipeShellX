# Contributing to PipeShellX

Thanks for helping build PipeShellX. This document covers the mechanics; the
*what* and *why* live in [`PLAN.md`](PLAN.md) (the single source of truth for
scope) and the architecture decision records in [`docs/adr/`](docs/adr/).

## Build and test

Requirements: CMake ≥ 3.20, a C++20 compiler (GCC ≥ 11, Clang ≥ 14,
Apple Clang ≥ 14), Ninja or Make, network access on first configure
(GoogleTest is fetched via `FetchContent`; set `-DPIPESHELLX_SYSTEM_GTEST=ON`
to use an installed copy instead).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/pipeshellx --help
```

Useful options:

| Option | Default | Effect |
|---|---|---|
| `PIPESHELLX_BUILD_TESTS` | `ON` | build the GoogleTest suite |
| `PIPESHELLX_BUILD_BENCH` | `ON` | build `bench/` (`pipeshellx_bench_baseline`) |
| `PIPESHELLX_WERROR` | `ON` | `-Werror` / `/WX` for first-party targets |
| `PIPESHELLX_SANITIZE` | empty | e.g. `address,undefined` or `thread` |
| `PIPESHELLX_SYSTEM_GTEST` | `OFF` | `find_package(GTest)` instead of FetchContent |

Run the sanitizer build before opening a PR that touches process, pipe, or
threading code:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPIPESHELLX_SANITIZE=address,undefined
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

## Workflow

1. **Tests first.** Every behaviour change starts with a failing test in
   `tests/`. Unit tests are GoogleTest; use `tests/test_support.hpp`
   (`ScopedTempCwd`, `ScopedEnv`) so tests never depend on the caller's CWD or
   environment.
2. **Warnings are errors.** `-Wall -Wextra -Werror` on GCC/Clang, `/W4 /WX` on
   MSVC, applied through the `pipeshellx_warnings` target.
3. **Format what you touch.** `clang-format -i <files>` using the repository
   `.clang-format`. Do not reformat files you did not otherwise change.
4. **Lint.** `clang-tidy -p build <file>` with the repository `.clang-tidy`;
   new warnings in changed code should be fixed or justified in the PR.
   (Homebrew LLVM on macOS needs the Apple SDK:
   `clang-tidy -p build --extra-arg=-isysroot --extra-arg="$(xcrun --show-sdk-path)" <file>`.)
5. **Docs move with code.** Each roadmap item in `PLAN.md` names the `docs/`
   files it extends; update them in the same PR. Scope changes update
   `PLAN.md` itself.
6. **Decisions get an ADR.** Anything that changes a public contract, a
   security default, a dependency, or a layering rule gets a new
   `docs/adr/ADR-NNN-*.md` (template in `docs/adr/README.md`).

## Layering and portability rules

These are enforced progressively by CI (see `PLAN.md` §3.1, §4.1):

- Public headers under `include/psx/` (introduced in Phase 1) expose only
  `std::` types. Platform headers (`<unistd.h>`, `<sys/*.h>`, `<windows.h>`)
  live in `src/os/posix/` and `src/os/win32/` only.
- A layer may include only the layer directly below it.
- Every descriptor/handle is non-inheritable at creation; the only exception
  is the three stdio handles handed explicitly to a child.
- Nothing unbounded: buffers, descriptors, in-flight bytes, retries, and time
  are all capped.

## Commit messages

Conventional Commits, imperative mood, wrapped at 72 columns:

```
feat(ssh): pass passwords to sshpass over a pipe instead of argv

Closes the `ps`-visible secret described in docs/authentication.md.
```

Types: `feat`, `fix`, `refactor`, `test`, `docs`, `chore`, `perf`, `ci`.
Phase completions use `feat(phase-N): complete Phase N milestones`.

## Pull request checklist

- [ ] tests added/updated and `ctest` green locally
- [ ] sanitizer build green if process/pipe/thread code changed
- [ ] no new warnings; touched files clang-formatted
- [ ] `docs/` and, if scope changed, `PLAN.md` updated
- [ ] ADR added for contract/security/dependency/layering changes
- [ ] no secrets, hostnames, or local paths in the diff

## Reporting security issues

Please follow [`SECURITY.md`](SECURITY.md) rather than opening a public issue.

## License

By contributing you agree that your contributions are licensed under the
Apache License 2.0 (see `LICENSE` and `NOTICE`).
