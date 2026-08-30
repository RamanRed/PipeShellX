# Contributing to PipeShellX

Thank you for improving PipeShellX. Contributions should keep command
execution predictable, portable, testable, and honest about its trust and
platform boundaries.

## Set up a development build

You need CMake 3.20 or newer, a C++20 compiler, and Ninja or Make. The default
build also needs OpenSSL 3. CMake uses an installed GoogleTest when available
or fetches v1.17.0 during configuration; use `PIPESHELLX_SYSTEM_GTEST=ON` for
a strictly system-provided dependency.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/pipeshellx --help
```

Useful CMake options:

| Option | Default | Purpose |
| --- | ---: | --- |
| `PIPESHELLX_BUILD_TESTS` | `ON` | Build and register the GoogleTest suite. |
| `PIPESHELLX_BUILD_BENCH` | `ON` | Build the baseline benchmark. |
| `PIPESHELLX_WERROR` | `ON` | Treat first-party warnings as errors. |
| `PIPESHELLX_NATIVE_TRANSPORT` | `ON` | Build OpenSSL-backed native transport, CA, and node support. |
| `PIPESHELLX_STATIC_OPENSSL` | `OFF` | Prefer static OpenSSL libraries; this is not a fully-static guarantee. |
| `PIPESHELLX_SYSTEM_GTEST` | `OFF` | Require an installed GoogleTest rather than fetching it. |
| `PIPESHELLX_SANITIZE` | empty | Enable a sanitizer list such as `address,undefined`. |

For an SSH-only build without OpenSSL discovery:

```bash
cmake -S . -B build-ssh \
  -DPIPESHELLX_NATIVE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
cmake --build build-ssh --parallel
ctest --test-dir build-ssh --output-on-failure
```

See [testing](docs/testing.md) for sanitizer, focused-test, soak, packaging,
and CI commands.

## Make changes

- Add or update a focused regression test for behavior changes. Tests must not
  depend on the caller's working directory, environment, real fleet, or
  external network.
- Use helpers in `tests/test_support.hpp` for temporary directories and scoped
  environment changes.
- Keep production code warning-clean under `-Wall -Wextra -Werror` or
  `/W4 /WX`.
- Run `clang-format -i` on files you touch. Do not reformat unrelated code.
- Run `clang-tidy -p build <file>` for materially changed C++ files and fix or
  explain new findings.
- Update public documentation whenever a command, option, output schema,
  security boundary, platform claim, or packaging contract changes.

## Preserve layering and portability

Dependencies point down from CLI and orchestration through transports,
runtime, and the OS abstraction. In particular:

- headers under `include/psx/` expose standard C++ types rather than native
  file descriptors, sockets, or platform headers;
- POSIX platform headers and implementations stay under `src/os/posix/`;
- higher layers use the OS abstraction instead of calling platform APIs
  directly;
- new resources have explicit ownership, cleanup, cancellation, and error
  paths;
- Linux and macOS behavior must remain equivalent where the feature is
  documented as supported.

Run the repository checks after relevant changes:

```bash
./scripts/check_layering.sh
./scripts/check_docs.py
```

## Validate before opening a pull request

At minimum:

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./scripts/check_layering.sh
./scripts/check_docs.py
./build/bin/pipeshellx --version
./build/bin/pipeshellx --help
```

For process, pipe, socket, TLS, cancellation, or concurrency changes, also run
ASan and UBSan:

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPIPESHELLX_SANITIZE=address,undefined
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

Pull requests should be narrow enough to review and should explain the user
impact, supported platforms, tests run, and any known limitation. Before
submitting, verify that:

- tests cover the changed behavior and pass locally;
- touched files are formatted and warning-clean;
- layering and sanitizer checks relevant to the change pass;
- documentation and changelog entries match the implementation;
- the diff contains no credentials, private hostnames, audit output, generated
  inventories, build artifacts, or local absolute paths.

Conventional Commit subjects are preferred, for example:

```text
fix(transport): preserve stderr channel attribution
```

## Security reports

Do not disclose suspected vulnerabilities in a public pull request or issue.
Follow [SECURITY.md](SECURITY.md) for the current private-reporting procedure
and supported versions.

## Respectful conduct

Be constructive, specific, and respectful. Focus criticism on code and ideas;
do not harass, insult, discriminate against, or expose private information
about another participant. Maintainers may edit or remove abusive content and
restrict participation when necessary to keep collaboration safe.

## License

By submitting a contribution, you agree that it may be distributed under the
repository's Apache License 2.0.
