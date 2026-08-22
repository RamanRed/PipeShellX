# ADR-007: OpenSSL 3.x for the native backplane's mTLS (over SChannel)

- **Status:** Accepted
- **Date:** 2026-08-23
- **Deciders:** project maintainers
- **Related:** `PLAN.md` §4.6 (toolchain), Phase 4 roadmap; ADR-004 (OpenSSL3 static for TLS); ADR-006 (TLV frame codec)

## Context

The Phase 4 native backplane (`psx/1`) runs over one **mutually-authenticated
TLS 1.3** connection per node, with SAN-URI certificate identities and a CRL
file, issued by an offline fleet CA (`pipeshellx ca`). The TLS engine must work
identically on Linux, macOS, and Windows, must support a caller-supplied CA and
CRL (no OS trust store), and must expose the peer certificate's SAN URI for
authorization — all on the single-threaded completion-style reactor.

Two engines were considered:

- **OpenSSL 3.x** — one portable API on all three platforms; full control over
  the trust anchor, CRL, cipher suite (TLS 1.3 only), and SAN-URI extraction;
  memory-BIO mode integrates cleanly with the reactor (feed ciphertext in, pull
  plaintext out — no socket ownership); static linking for air-gapped, single-file
  binaries (already the ADR-004 decision).
- **SChannel** — the native Windows TLS stack; no third-party dependency on
  Windows. But it is Windows-only (Linux/macOS would still need OpenSSL, so we'd
  maintain two engines), its trust model is oriented at the OS certificate store
  (custom-CA + CRL-file handling is awkward), and TLS 1.3 support varies by
  Windows version.

## Decision

Use **OpenSSL 3.x on every platform**, in **memory-BIO** mode so `os::Tls` owns no
socket and drives handshake/read/write from the reactor. mTLS 1.3 only;
identities are validated by matching the peer certificate's **SAN URI** against
the expected node identity; revocation is a CA-distributed **CRL file**. OpenSSL
is linked statically (ADR-004) so the binary carries its own crypto with no
network or OS-store dependency — essential for the air-gapped use case (§5.3).

SChannel is rejected: a second, Windows-only engine buys nothing here (OpenSSL
already runs natively on Windows via the §4.6 toolchain) while its OS-store trust
model fights the fleet-CA requirement.

## Consequences

- One TLS code path to write, test, and fuzz across all platforms.
- The memory-BIO design keeps `os::Socket` and `os::Tls` orthogonal: the socket
  is a plain byte pipe on the reactor; TLS is a byte transform above it.
- Windows builds gain an OpenSSL static-lib build dependency (already required by
  ADR-004), not the system SChannel.
