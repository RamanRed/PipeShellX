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

A normal non-sanitized CTest run registers all three focused packaging and
dependency-metadata checks. They can be rerun directly with:

```bash
ctest --test-dir build-native \
  -R '^(dependency_notice_metadata|packaging_install_consumer_smoke|native_transport_requires_openssl3)$' \
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

## Release archive gate

`.github/workflows/release.yml` builds and tests three release targets:

- Ubuntu 22.04 x86-64 as `linux-x86_64`;
- macOS 15 Intel as `macos-x86_64`; and
- macOS 15 Apple silicon as `macos-arm64`.

For each target the workflow runs the full release CTest suite, installs and
archives one versioned root, rejects a dynamic `libssl` or `libcrypto` CLI
dependency, generates an SPDX JSON SBOM and SHA-256 checksum, then consumes the
archive on a fresh job. `scripts/smoke_release_archive.sh` validates path
safety, required files, the public/private header boundary, CLI behavior, the
relocatable downstream CMake package, and clean installation.

The same smoke reconstructs a fixed v0.5.0 fixture from commit
`7aae3db2ca9bfede8efb37af2f3594384c5ac5b9`. It installs v0.5 and the current
candidate in separate immutable versioned directories, atomically changes a
relative `current` symlink, and verifies all of these upgrade properties:

- the selected CLI reports the current version;
- the obsolete uppercase `PipeShellX` executable is not visible through the
  current install;
- operator-managed configuration outside the release directories survives;
  and
- a fresh downstream `find_package(pipeshellx)` consumer still builds and
  runs.

After every archive passes its target smoke, the workflow creates keyless
Cosign signatures plus GitHub SLSA provenance and SPDX SBOM attestations. A
manual dispatch assembles and verifies a signed dry-run Actions artifact but
does not publish. Only a pushed tag exactly matching the CMake version can
create a GitHub release. See [deployment](deployment.md#release-archives-and-verification)
for the asset and consumer-verification contract.

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
- a full SSH-only native-disabled test suite, build, install, CLI smoke, and
  downstream consumer;
- layering and tracked-Markdown link checks.

The stable `Required CI` job succeeds only when every matrix, sanitizer,
packaging, native-disabled, layering, and documentation dependency succeeds.
This single name is the branch-rule contract even when internal matrix labels
change.

`.github/workflows/codeql.yml` performs a C/C++ CodeQL manual-build analysis
with the `security-extended` query suite on pushes and pull requests to `main`,
weekly, and on manual dispatch. Its stable check name is `Analyze C/C++`.

The `main` rules require both `Required CI` and `Analyze C/C++` from an
up-to-date commit. Force-push and branch deletion are blocked without a
ruleset bypass; administrators can still edit or disable the ruleset through
an explicit repository-settings change when recovering an emergency.

Each default matrix job builds the full suite, runs CTest, and checks CLI
version/help/unknown-option behavior. The dedicated native-disabled hosted job
also builds and runs the full applicable SSH-only suite before installation
and downstream consumption.

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
- continuous libFuzzer, sandbox/privilege-separation, or signed-audit
  validation;
- byte-for-byte reproducible release builds and package-manager installation
  or upgrade coverage beyond the versioned archives;
- reproducible performance guarantees from the shared hosted benchmark runner.

Passing the POSIX matrix supports the documented Linux/macOS scope only; it is
not evidence for deferred platforms or features.
