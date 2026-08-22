# ADR-004: OpenSSL 3, statically linked, for the native backplane's TLS

- **Status:** Accepted (the Phase 4 spike may add SChannel on Windows via a superseding ADR)
- **Date:** 2026-08-22
- **Deciders:** project maintainers
- **Related:** `PLAN.md` §3.3 (`Tls` primitive), §3.6 (`NativeTransport`), §3.8 (wire security), §7 T7/T13, Phase 4 roadmap

## Context

The `psx/1` backplane requires mutual TLS 1.3 with certificate identities
(SAN URI `psx://<fleet>/node/<name>`), an offline CA, CRL files, and AEAD-only
cipher suites — on Linux, macOS, and Windows, inside one static binary.

Candidates:

| Option | For | Against |
|---|---|---|
| OpenSSL 3 (static) | ubiquitous, FIPS provider available, full X.509/CRL/SAN control, identical behaviour on all OSes | ≈ 3–5 MB added binary size; we own the update cadence |
| LibreSSL / BoringSSL | smaller, cleaner APIs | fewer distro/FIPS options; BoringSSL has no stable API |
| mbedTLS / wolfSSL | small footprint | weaker X.509 tooling for CRL/SAN-URI needs; licensing friction (wolfSSL GPL/commercial) |
| Platform stacks (SChannel, Secure Transport) | no bundled crypto; OS-managed updates | three code paths, divergent X.509 semantics, SAN-URI/CRL gaps on older Windows; Secure Transport is deprecated on macOS |
| Rust/Go TLS via FFI | modern | foreign toolchain in a C++ static build |

Binary-size target T13 allows ≤ 9 MB for the native build, which accommodates
a static OpenSSL.

## Decision

- Link **OpenSSL 3** statically into the native-transport build on all three
  platforms; the SSH-only build does not link it at all.
- TLS 1.3 only; cipher suites fixed to AEAD; no plaintext or downgrade mode.
- Peer identity is the SAN URI; authorisation is fleet match plus an optional
  per-node allow-list. CRLs are plain files distributed over SSH; no OCSP.
- Pin the OpenSSL version in the build, track its security advisories, and
  rebuild releases on every OpenSSL security fix (recorded in `CHANGELOG`).
- The Phase 4 spike evaluates SChannel for Windows **controllers** only; if
  adopted it will be recorded in a superseding ADR and must pass the same
  protocol test-suite.

## Consequences

- Identical TLS behaviour and error messages on every platform; one X.509
  code path to audit.
- The project carries OpenSSL's update responsibility; reproducible static
  builds (Phase 6) and the SBOM make this visible.
- Air-gapped operation is preserved: no online CA, OCSP, or CT dependencies.
- The `Tls` primitive in `psx::os` must hide the OpenSSL types entirely so that
  a future backend swap touches `src/os/**` only.
