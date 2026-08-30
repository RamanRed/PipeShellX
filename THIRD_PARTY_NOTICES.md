# Third-party notices

This document records PipeShellX's direct third-party dependencies and the
redistribution boundary for each one. Release artifacts also contain a
machine-readable SBOM describing the exact packages detected in that artifact.
The SBOM is the version-specific inventory; this file explains the stable
project-level obligations.

PipeShellX itself is licensed under Apache License 2.0. See [LICENSE](LICENSE).
This inventory was last reviewed on 2026-08-30 against every CMake
`find_package`, `FetchContent`, and link declaration; every external executable
spawned by the runtime; and every repository workflow and validation script.
The repository contains no vendored third-party source tree.

## Code linked into release artifacts

### OpenSSL

- Project: [OpenSSL](https://openssl-library.org/)
- Supported version: 3.0 or newer, selected by the package builder
- Use: TLS 1.3, certificate, hashing, and cryptographic operations when
  `PIPESHELLX_NATIVE_TRANSPORT=ON`
- License: [Apache License 2.0](https://github.com/openssl/openssl/blob/master/LICENSE.txt)

Native-enabled builds link `libssl` and `libcrypto`. A package may link them
dynamically or statically; `PIPESHELLX_STATIC_OPENSSL=ON` expresses a static
preference but does not override what the platform package provides. The full
Apache License 2.0 text distributed in [LICENSE](LICENSE) is also the license
text used by OpenSSL 3. OpenSSL does not ship an upstream `NOTICE` file that
adds attribution text for this supported version line.

Native-disabled builds neither discover nor link OpenSSL.

### Platform C, C++, and threading runtimes

PipeShellX uses the platform runtime and CMake's `Threads::Threads` abstraction.
These are supplied by the selected compiler and operating system, not vendored
by this repository. Normal release packages do not deliberately bundle them.
Their exact identity and license therefore vary by target and are reported by
the release SBOM and binary dependency inspection. If a future package copies
or statically links a runtime, the packager must add that runtime's license and
exception text to the artifact.

Common examples are LLVM `libc++` (Apache-2.0 WITH LLVM-exception), GNU
`libstdc++` (GPL-3.0-or-later WITH GCC-exception-3.1), glibc
(LGPL-2.1-or-later), and operating-system libraries. Listing an example here
does not imply that every artifact contains it.

## Test-only source dependency

### GoogleTest

- Project: [GoogleTest](https://github.com/google/googletest)
- Fallback version: v1.17.0
- Immutable fallback source commit: `52eb8108c5bdec04579160ae17225d66034bd723`
- Use: unit and integration test framework
- License: BSD 3-Clause
  ([reviewed upstream text](https://github.com/google/googletest/blob/52eb8108c5bdec04579160ae17225d66034bd723/LICENSE))

CMake first accepts an installed GoogleTest, whose version is selected by the
builder. When one is unavailable, the test build fetches the immutable commit
above. GoogleTest is not linked into the installed `pipeshellx` executable or
`pipeshellx::lib`, and its headers and binaries are not installed by this
project.

GoogleTest license text:

> Copyright 2008, Google Inc.
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> - Redistributions of source code must retain the above copyright notice,
>   this list of conditions and the following disclaimer.
> - Redistributions in binary form must reproduce the above copyright notice,
>   this list of conditions and the following disclaimer in the documentation
>   and/or other materials provided with the distribution.
> - Neither the name of Google Inc. nor the names of its contributors may be
>   used to endorse or promote products derived from this software without
>   specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
> CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
> ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

## External runtime programs

These programs are invoked as independent processes. They are not linked,
vendored, or copied into PipeShellX release artifacts.

| Program | When used | Upstream license | Packaging boundary |
| --- | --- | --- | --- |
| [OpenSSH client](https://www.openssh.com/) | Agentless SSH transport | Composite BSD/ISC-style and public-domain terms; see upstream `LICENCE` | Required on the host for SSH execution; never bundled by PipeShellX. |
| [`sshpass`](https://sourceforge.net/projects/sshpass/) | Optional password injection for SSH | GPL-2.0-or-later | Optional host-installed executable; never linked or bundled. |
| Target login shell and requested commands | Remote command execution | Host-specific | Supplied by the operator's target host. |

The GPL terms on `sshpass` do not apply to PipeShellX merely because PipeShellX
can execute a separately installed `sshpass` process. A distributor that
chooses to bundle `sshpass` alongside PipeShellX must satisfy the GPL obligations
for that separate program and must not describe it as part of the Apache-2.0
PipeShellX work.

## Build, test, and automation tools

The repository does not redistribute these tools. They are developer or CI
inputs, and their licenses do not change PipeShellX's Apache-2.0 license.

| Tool | Role | License |
| --- | --- | --- |
| CMake 3.20 or newer | Configure, build, test, install, and package metadata | BSD 3-Clause |
| GCC / GNU `libstdc++` | Supported Linux compiler and runtime | GPL-3.0-or-later with the applicable GCC Runtime Library Exception |
| Clang / LLVM `libc++` | Supported Linux/macOS compiler and runtime | Apache-2.0 WITH LLVM-exception |
| Python 3 | Documentation link checker | PSF-2.0 |
| Bash and standard POSIX utilities | Repository and CI checks | GPL-3.0-or-later for Bash; utility licenses vary by host |
| GitHub-maintained Actions (`checkout`, artifact transfer, CodeQL, and `actions/attest`) | Source checkout, CI artifacts, security analysis, provenance, and SBOM attestations | MIT |
| Syft / Anchore SBOM tooling | Release SBOM generation | Apache-2.0 |
| Cosign / Sigstore tooling | Keyless artifact signatures | Apache-2.0 |

All GitHub Actions used by the repository are pinned to immutable commit SHAs.
The human-readable version comment beside each pin identifies the reviewed
upstream release.

## Maintainer review checklist

For every dependency or release-tool upgrade:

1. verify the upstream tag resolves to the pinned commit and review the license
   at that commit;
2. update the version, commit, license, and redistribution boundary here;
3. regenerate the release SBOM and inspect it for newly bundled components;
4. inspect binary linkage on every target, especially when static-link options
   or compiler runtimes change; and
5. keep `LICENSE`, `NOTICE`, and this file inside every source and binary
   archive.
