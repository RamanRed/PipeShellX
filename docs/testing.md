# Testing and validation

PipeShellX uses GoogleTest with individual tests discovered by CTest. Tests are
enabled by default. CMake uses an installed GoogleTest when
`PIPESHELLX_SYSTEM_GTEST=ON`; otherwise it finds one or fetches the pinned
source. Use a fresh build directory for release validation so deleted sources,
stale cache entries, and packaging references cannot be hidden by an
incremental build.

## Default native build

The default configuration enables the OpenSSL 3 native transport, tests,
benchmarks, and warnings as errors.

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --parallel
ctest --test-dir build-native --output-on-failure --timeout 120

./build-native/bin/pipeshellx --version
./build-native/bin/pipeshellx --help
```

The discovered suite includes the portable-poll golden test, the install and
downstream-consumer smoke test, and the check that native transport cannot be
configured without OpenSSL 3. Test counts are reported at validation time
rather than frozen in this document because the suite changes.

## SSH-only build

This gate proves that the project neither discovers nor links OpenSSL when the
native transport is disabled. Keep tests enabled: they cover the SSH product
and the CLI diagnostics for unavailable native-only commands.

```bash
cmake -S . -B build-native-off \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIPESHELLX_NATIVE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE \
  -DPIPESHELLX_BUILD_BENCH=OFF
cmake --build build-native-off --parallel
ctest --test-dir build-native-off --output-on-failure --timeout 120
```

## Sanitizers

Run the Linux Clang ASan+UBSan gate for changes involving processes, signals,
pipes, sockets, reactors, or object lifetime:

```bash
CC=clang CXX=clang++ cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPIPESHELLX_SANITIZE=address,undefined \
  -DPIPESHELLX_BUILD_BENCH=OFF
cmake --build build-sanitize --parallel
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build-sanitize --output-on-failure --timeout 180
```

The install smoke is intentionally omitted from sanitizer builds; packaging is
validated separately below.

## Install and package gate

A normal non-sanitized CTest run registers both packaging checks. They can be
rerun directly with:

```bash
ctest --test-dir build-native \
  -R '^(packaging_install_consumer_smoke|native_transport_requires_openssl3)$' \
  --output-on-failure
```

The release-package job additionally exercises the static-OpenSSL preference,
an isolated install prefix, the lowercase CLI, and a fresh downstream
`find_package(pipeshellx)` consumer:

```bash
cmake -S . -B build-package \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIPESHELLX_BUILD_TESTS=OFF \
  -DPIPESHELLX_BUILD_BENCH=OFF \
  -DPIPESHELLX_STATIC_OPENSSL=ON
cmake --build build-package --parallel
cmake --install build-package --prefix "$PWD/build-package-prefix"
"$PWD/build-package-prefix/bin/pipeshellx" --version

cmake -S tests/packaging/downstream -B build-package-consumer \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/build-package-prefix"
cmake --build build-package-consumer --parallel
./build-package-consumer/bin/pipeshellx_package_consumer
```

## Repository maintenance gates

Run these checks for every repository or documentation cleanup:

```bash
./scripts/check_layering.sh
./scripts/check_docs.py
python3 -m py_compile scripts/check_docs.py
actionlint .github/workflows/*.yml
git diff --check
```

`check_layering.sh` rejects platform headers outside the OS implementation,
platform types in public headers, and upward layer dependencies.
`check_docs.py` resolves relative links in tracked and unignored new Markdown,
so missing, repository-escaping, and case-mismatched targets fail on every
filesystem.

For focused debugging, pass a regular expression to CTest or a GoogleTest
filter to the test executable:

```bash
ctest --test-dir build-native -R 'DiffCommandTest|PipeCommandTest' --output-on-failure
build-native/bin/pipeshellx_tests \
  --gtest_filter='InventoryTest.*:HostsSubcommandTest.*:RunSubcommandTest.*'
```

`PIPESHELLX_POLLER=poll` forces the portable reactor backend. CTest already
registers `golden_on_poll_backend`. `PIPESHELLX_SOAK=1` lengthens selected
descriptor and zombie regression loops.

## GitHub Actions matrix

`.github/workflows/ci.yml` runs:

- Linux GCC Debug and Release;
- Linux Clang Debug and Release;
- macOS AppleClang Debug and Release;
- Linux Clang Debug with ASan+UBSan;
- static-OpenSSL Release install and downstream-package validation;
- an SSH-only native-disabled build, install, CLI smoke, and downstream
  consumer;
- layering and tracked-Markdown link checks.

Each default matrix job builds the full suite, runs CTest, and checks CLI
version/help/unknown-option behavior. The dedicated native-disabled hosted job
currently builds with tests disabled, so the full native-off CTest command
above remains an explicit release gate.

`.github/workflows/bench.yml` is scheduled and manually dispatchable. It builds
the current Release benchmark, configures localhost OpenSSH, runs the harness,
and uploads its Markdown output. It is measurement evidence, not a merge gate.

Workflow definitions are not proof that an unpublished revision passed them.
Record the commit, run URL, discovered counts, failures, and platform skips in
the release handoff.

## Known limitations

The maintained gates do not provide:

- a Windows controller/native-node build or MSVC/clang-cl coverage;
- qualification against a heterogeneous external SSH/native fleet;
- native reconnect/resume or general non-linear remote DAG behavior, which the
  product does not implement;
- continuous libFuzzer, sandbox/privilege-separation, signed-audit, or release
  artifact-signing validation;
- reproducible performance guarantees from the shared hosted benchmark runner.

Passing the POSIX matrix supports the documented Linux/macOS scope only; it is
not evidence for deferred platforms or features.
